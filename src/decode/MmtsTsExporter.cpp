#include "FilePathSafety.h"
#include "MmtsTsExporter.h"

#include "HevcReencoder.h"
#include "MmtsCutPlan.h"
#include "mmtStream.h"
#include "mmtTlvDemuxer.h"
#include "mpuProcessorBase.h" // MfuData, NOPTS_VALUE
#include "remuxerHandler.h"
#include "stream.h"

#include <QFile>
#include <algorithm>
#include <fstream>
#include <vector>

namespace {

qint64 ptsToMs(uint64_t pts, const MmtTlv::MmtStream& s)
{
    if (pts == MmtTlv::NOPTS_VALUE)
        return -1;
    const auto& tb = s.getTimeBase();
    if (tb.den <= 0)
        return static_cast<qint64>(pts / 90);
    return static_cast<qint64>((static_cast<double>(pts) * tb.num * 1000.0) / tb.den);
}

// Absolute ms -> raw stream PTS (inverse of ptsToMs).
uint64_t absMsToStreamPts(qint64 absMs, const MmtTlv::MmtStream::TimeBase& tb)
{
    if (tb.num <= 0 || tb.den <= 0)
        return static_cast<uint64_t>(absMs * 90); // assume 90kHz
    return static_cast<uint64_t>(absMs * static_cast<int64_t>(tb.den) / (1000LL * tb.num));
}

// TS remuxer that, per segment: rebases timestamps (base setPtsOffset), copies
// only the video in [copyStart, copyEnd), injects the re-encoded lead-in before
// it and the re-encoded tail after it, and gates audio/subtitle to [IN, OUT).
// NTP-driven PCR is suppressed.
class SmartRebasingRemuxer : public RemuxerHandler {
public:
    explicit SmartRebasingRemuxer(MmtTlv::MmtTlvDemuxer& d) : RemuxerHandler(d) {}

    void setOffsetMs(qint64 offsetMs) { setPtsOffset(offsetMs * 90); }
    void setSegment(const std::vector<HevcReencoder::Au>* leadIn,
                    const std::vector<HevcReencoder::Au>* tail,
                    qint64 copyStartMs, qint64 copyEndMs,
                    qint64 inAbsMs, qint64 outAbsMs, qint64 firstPts,
                    bool tailReencoded)
    {
        m_leadIn = leadIn;
        m_tail = tail;
        m_copyStartMs = copyStartMs;
        m_copyEndMs = copyEndMs;
        m_inAbsMs = inAbsMs;
        m_outAbsMs = outAbsMs;
        m_firstPts = firstPts;
        m_endUseDts = tailReencoded;
        m_videoStream = nullptr;
        m_leadInDone = false;
    }

    // Called after the segment's demux: ensures the lead-in is injected (e.g.
    // when there was no copied frame) and appends the re-encoded tail.
    void finalizeSegment()
    {
        ensureLeadIn();
        inject(m_tail);
    }

    void onVideoData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu) override
    {
        if (!m_videoStream)
            m_videoStream = &stream;
        const qint64 pts = ptsToMs(mfu.pts, stream);
        if (pts < m_copyStartMs)
            return; // before copy start (lead-in is re-encoded)
        if (m_endUseDts) {
            // Cut the copy end in DECODE order so the boundary GOP (incl. its
            // open-GOP RASL leading pictures, which present < rLast but decode
            // after it) is dropped cleanly; [rLast, OUT) comes from the tail.
            const qint64 dts = ptsToMs(mfu.dts, stream);
            const qint64 effDts = (dts >= 0) ? dts : pts;
            if (effDts >= m_copyEndMs)
                return;
        } else if (pts >= m_copyEndMs) {
            return; // OUT at a RAP: presentation-accurate copy to the boundary
        }
        ensureLeadIn();
        RemuxerHandler::onVideoData(stream, mfu);
    }
    void onAudioData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu) override
    {
        const qint64 abs = ptsToMs(mfu.pts, stream);
        if (abs >= 0 && (abs < m_inAbsMs || abs >= m_outAbsMs))
            return;
        RemuxerHandler::onAudioData(stream, mfu);
    }
    void onSubtitleData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu) override
    {
        const qint64 abs = ptsToMs(mfu.pts, stream);
        if (abs >= 0 && (abs < m_inAbsMs || abs >= m_outAbsMs))
            return;
        RemuxerHandler::onSubtitleData(stream, mfu);
    }
    void onNtp(const MmtTlv::NTPv4&) override {}

private:
    void ensureLeadIn()
    {
        if (m_leadInDone)
            return;
        inject(m_leadIn);
        m_leadInDone = true;
    }
    void inject(const std::vector<HevcReencoder::Au>* aus)
    {
        if (!m_videoStream || !aus || aus->empty())
            return;
        const auto& tb = m_videoStream->getTimeBase();
        for (const auto& au : *aus) {
            MmtTlv::MfuData m;
            m.data = au.data;
            m.streamIndex = m_videoStream->getStreamIndex();
            m.keyframe = au.keyframe;
            m.isFirstFragment = true;
            m.isLastFragment = true;
            const qint64 dtsMs = au.dtsMs >= 0 ? au.dtsMs : au.ptsMs;
            m.pts = absMsToStreamPts(au.ptsMs + m_firstPts, tb);
            m.dts = absMsToStreamPts(dtsMs + m_firstPts, tb);
            RemuxerHandler::onVideoData(*m_videoStream, m);
        }
    }

    const std::vector<HevcReencoder::Au>* m_leadIn = nullptr;
    const std::vector<HevcReencoder::Au>* m_tail = nullptr;
    qint64 m_copyStartMs = 0, m_copyEndMs = 0;
    qint64 m_inAbsMs = 0, m_outAbsMs = 0, m_firstPts = 0;
    const MmtTlv::MmtStream* m_videoStream = nullptr;
    bool m_leadInDone = false;
    bool m_endUseDts = false;
};

} // namespace

QVector<MmtsTsExporter::Segment> MmtsTsExporter::planSegments(const MmtsMap& map,
                                                             const QVector<EditSegment>& segments,
                                                             qint64 sourceSizeBytes)
{
    // Planning lives in MmtsCutPlan (shared with the .mmts smart exporter).
    QVector<Segment> out;
    for (const auto& c : planMmtsCutSegments(map, segments, sourceSizeBytes)) {
        Segment s;
        s.inAbsMs = c.inAbsMs;
        s.outAbsMs = c.outAbsMs;
        s.prerollByte = c.prerollByte;
        s.endByte = c.endByte;
        s.copyStartMs = c.copyStartMs;
        s.copyEndMs = c.copyEndMs;
        s.leadInImpossible = c.leadInImpossible;
        out.push_back(s);
    }
    return out;
}

bool MmtsTsExporter::exportTs(const QString& srcPath, const QString& dstPath,
                              const MmtsMap& map, const QVector<EditSegment>& segments,
                              qint64 sourceSizeBytes, QString* errorMessage,
                              const std::function<bool(qint64, qint64)>& progress,
                              QStringList* warnings,
                              bool requireFrameAccurate)
{
    const QVector<Segment> plan = planSegments(map, segments, sourceSizeBytes);
    if (plan.isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("Nothing to export (no segments map to RAP ranges).");
        return false;
    }
    const qint64 firstPts = map.firstVideoPtsMs > 0 ? map.firstVideoPtsMs : 0;

    if (!distinctOutput(srcPath, dstPath, errorMessage)) return false;
    std::ifstream ifs(std::filesystem::path(srcPath.toStdWString()), std::ios::binary);
    if (!ifs.is_open()) {
        if (errorMessage) *errorMessage = QStringLiteral("Cannot open source: %1").arg(srcPath);
        return false;
    }
    std::ofstream ofs(std::filesystem::path(dstPath.toStdWString()), std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        if (errorMessage) *errorMessage = QStringLiteral("Cannot create output: %1").arg(dstPath);
        return false;
    }

    MmtTlv::MmtTlvDemuxer demuxer;
    SmartRebasingRemuxer remuxer(demuxer);
    remuxer.setOutputCallback([&ofs](const uint8_t* data, size_t size) {
        ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    });
    demuxer.setDemuxerHandler(remuxer);

    HevcReencoder reenc;

    qint64 totalBytes = 0;
    for (const auto& s : plan)
        totalBytes += (s.endByte - s.prerollByte);
    qint64 doneBytes = 0;
    qint64 desiredStartMs = 0;

    constexpr size_t kChunk = 4 * 1024 * 1024;
    std::vector<uint8_t> buf;
    buf.reserve(kChunk * 2);

    constexpr qint64 kThr = 20;
    int segmentNo = 0;
    for (const Segment& seg : plan) {
        ++segmentNo;
        std::vector<HevcReencoder::Au> leadIn, tail;
        qint64 effInMs = seg.inAbsMs;
        qint64 effOutMs = seg.outAbsMs;

        // Lead-in re-encode [IN, copyStart) (also the whole segment when copy is empty).
        if (seg.copyStartMs - seg.inAbsMs > kThr) {
            QString rerr;
            const bool reencOk = seg.leadInImpossible
                ? false
                : reenc.reencode(srcPath, map, seg.inAbsMs - firstPts, seg.copyStartMs - firstPts, leadIn, &rerr);
            if (seg.leadInImpossible) {
                rerr = QStringLiteral("IN is before the recording's first keyframe; those leading frames were never captured.");
            }
            if (!reencOk) {
                const QString message = QString("Segment #%1 lead-in re-encode failed; IN cannot be exported frame-accurately. %2")
                    .arg(segmentNo)
                    .arg(rerr);
                if (requireFrameAccurate) {
                    if (errorMessage) *errorMessage = message;
                    ofs.close();
                    QFile::remove(dstPath);
                    return false;
                }
                if (warnings) {
                    warnings->push_back(message + QStringLiteral(" IN was moved to the next RAP boundary."));
                }
                effInMs = seg.copyStartMs; // fall back: start at the copy RAP
            }
        }
        // Tail re-encode [copyEnd, OUT).
        if (seg.outAbsMs - seg.copyEndMs > kThr) {
            QString rerr;
            if (!reenc.reencode(srcPath, map, seg.copyEndMs - firstPts, seg.outAbsMs - firstPts, tail, &rerr)) {
                const QString message = QString("Segment #%1 tail re-encode failed; OUT cannot be exported frame-accurately. %2")
                    .arg(segmentNo)
                    .arg(rerr);
                if (requireFrameAccurate) {
                    if (errorMessage) *errorMessage = message;
                    ofs.close();
                    QFile::remove(dstPath);
                    return false;
                }
                if (warnings) {
                    warnings->push_back(message + QStringLiteral(" OUT was moved to the previous RAP boundary."));
                }
                effOutMs = seg.copyEndMs; // fall back: end at the copy RAP
            }
        }
        const bool tailReencoded = !tail.empty();

        remuxer.setSegment(&leadIn, &tail, seg.copyStartMs, seg.copyEndMs, effInMs, effOutMs, firstPts, tailReencoded);
        remuxer.setOffsetMs(effInMs - desiredStartMs);
        demuxer.resetStreams();
        ifs.clear();
        ifs.seekg(seg.prerollByte, std::ios::beg);
        buf.clear();
        qint64 segPos = seg.prerollByte;

        while (segPos < seg.endByte || !buf.empty()) {
            if (segPos < seg.endByte && buf.size() < 2 * 1024 * 1024) {
                const qint64 want = std::min<qint64>(kChunk, seg.endByte - segPos);
                const size_t old = buf.size();
                buf.resize(old + static_cast<size_t>(want));
                ifs.read(reinterpret_cast<char*>(buf.data() + old), want);
                const std::streamsize got = ifs.gcount();
                buf.resize(old + static_cast<size_t>(got));
                segPos += got;
                doneBytes += got;
                if (got == 0 && buf.empty())
                    break;
            }

            size_t consumed = 0;
            {
                MmtTlv::Common::ReadStream stream(buf);
                while (!stream.isEof()) {
                    const size_t before = stream.getPos();
                    MmtTlv::DemuxStatus status;
                    try { status = demuxer.demux(stream); } catch (...) { break; }
                    if (status == MmtTlv::DemuxStatus::NotEnoughBuffer) break;
                    if (stream.getPos() == before) break;
                }
                consumed = buf.size() - stream.leftBytes();
            }
            if (consumed > 0)
                buf.erase(buf.begin(), buf.begin() + consumed);
            else if (segPos >= seg.endByte)
                break;

            if (progress && !progress(doneBytes, totalBytes)) {
                if (errorMessage) *errorMessage = QStringLiteral("Cancelled");
                ofs.close();
                QFile::remove(dstPath);
                return false;
            }
        }

        remuxer.finalizeSegment(); // inject lead-in (if not yet) and the tail
        desiredStartMs += (effOutMs - effInMs);
    }

    ofs.flush();
    return true;
}

// ---------------------------------------------------------------------------
void MmtsTsExportWorker::configure(const QString& srcPath, const QString& dstPath, const MmtsMap& map,
                                   const QVector<EditSegment>& segments, qint64 sourceSizeBytes,
                                   bool requireFrameAccurate)
{
    m_srcPath = srcPath;
    m_dstPath = dstPath;
    m_map = map;
    m_segments = segments;
    m_sourceSize = sourceSizeBytes;
    m_requireFrameAccurate = requireFrameAccurate;
    m_cancel.store(false);
}

void MmtsTsExportWorker::run()
{
    QString error;
    QStringList warnings;
    const bool ok = MmtsTsExporter::exportTs(
        m_srcPath, m_dstPath, m_map, m_segments, m_sourceSize, &error,
        [this](qint64 done, qint64 total) {
            emit progress(done, total);
            return !m_cancel.load();
        },
        &warnings,
        m_requireFrameAccurate);
    emit finished(ok, error, warnings);
}
