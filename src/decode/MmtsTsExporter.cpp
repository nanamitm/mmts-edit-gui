#include "MmtsTsExporter.h"

#include "HevcReencoder.h"
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
                    qint64 inAbsMs, qint64 outAbsMs, qint64 firstPts)
    {
        m_leadIn = leadIn;
        m_tail = tail;
        m_copyStartMs = copyStartMs;
        m_copyEndMs = copyEndMs;
        m_inAbsMs = inAbsMs;
        m_outAbsMs = outAbsMs;
        m_firstPts = firstPts;
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
        if (pts < m_copyStartMs || pts >= m_copyEndMs)
            return; // outside the stream-copy range (lead-in is re-encoded)
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
};

} // namespace

QVector<MmtsTsExporter::Segment> MmtsTsExporter::planSegments(const MmtsMap& map,
                                                             const QVector<EditSegment>& segments,
                                                             qint64 sourceSizeBytes)
{
    QVector<MmtsPoint> raps = map.rapPoints;
    std::sort(raps.begin(), raps.end(), [](const MmtsPoint& a, const MmtsPoint& b) { return a.timeMs < b.timeMs; });

    const qint64 firstPts = map.firstVideoPtsMs > 0 ? map.firstVideoPtsMs : 0;
    constexpr qint64 kThr = 20; // within ~1 frame of a RAP -> treat as RAP-aligned

    QVector<Segment> out;
    for (const auto& seg : segments) {
        Segment s;
        s.inAbsMs = seg.startMs + firstPts;
        s.outAbsMs = seg.endMs + firstPts;
        if (s.outAbsMs <= s.inAbsMs)
            continue;

        // RAPs around IN: prev (demux start) and next (copy start).
        qint64 prevByte = 0;
        qint64 rNextMs = -1;
        for (const auto& p : raps) {
            if (p.timeMs <= s.inAbsMs) prevByte = static_cast<qint64>(p.offset);
            else { rNextMs = p.timeMs; break; }
        }
        if (rNextMs < 0) rNextMs = s.inAbsMs;

        // OUT: RAP at/after OUT (copy end + demux end). OUT-point smart rendering
        // (re-encoding the tail GOP) is deferred -- splicing a re-encoded tail
        // after a stream copy hits HEVC parameter-set / open-GOP reference issues.
        qint64 rEndMs = -1;
        qint64 endByte = sourceSizeBytes;
        for (const auto& p : raps) {
            if (p.timeMs >= s.outAbsMs) { rEndMs = p.timeMs; endByte = static_cast<qint64>(p.offset); break; }
        }
        if (rEndMs < 0)
            rEndMs = (map.durationMs > 0 ? map.durationMs : 0) + firstPts;
        s.outAbsMs = rEndMs; // OUT snaps to the next RAP for now

        const bool reencIn = (rNextMs - s.inAbsMs) > kThr;
        s.copyStartMs = reencIn ? rNextMs : s.inAbsMs;
        s.copyEndMs = s.outAbsMs;

        s.prerollByte = std::clamp<qint64>(prevByte, 0, sourceSizeBytes);
        s.endByte = std::clamp<qint64>(endByte, 0, sourceSizeBytes);

        // copyStart<=copyEnd always; when equal (segment within one GOP) the copy
        // is empty and the lead-in re-encode [IN, copyStart) covers the whole span.
        if (s.endByte > s.prerollByte && s.outAbsMs > s.inAbsMs)
            out.push_back(s);
    }
    return out;
}

bool MmtsTsExporter::exportTs(const QString& srcPath, const QString& dstPath,
                              const MmtsMap& map, const QVector<EditSegment>& segments,
                              qint64 sourceSizeBytes, QString* errorMessage,
                              const std::function<bool(qint64, qint64)>& progress)
{
    const QVector<Segment> plan = planSegments(map, segments, sourceSizeBytes);
    if (plan.isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("Nothing to export (no segments map to RAP ranges).");
        return false;
    }
    const qint64 firstPts = map.firstVideoPtsMs > 0 ? map.firstVideoPtsMs : 0;

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
    for (const Segment& seg : plan) {
        std::vector<HevcReencoder::Au> leadIn, tail;
        qint64 effInMs = seg.inAbsMs;
        qint64 effOutMs = seg.outAbsMs;

        // Lead-in re-encode [IN, copyStart) (also the whole segment when copy is empty).
        if (seg.copyStartMs - seg.inAbsMs > kThr) {
            QString rerr;
            if (!reenc.reencode(srcPath, map, seg.inAbsMs - firstPts, seg.copyStartMs - firstPts, leadIn, &rerr))
                effInMs = seg.copyStartMs; // fall back: start at the copy RAP
        }
        // Tail re-encode [copyEnd, OUT).
        if (seg.outAbsMs - seg.copyEndMs > kThr) {
            QString rerr;
            if (!reenc.reencode(srcPath, map, seg.copyEndMs - firstPts, seg.outAbsMs - firstPts, tail, &rerr))
                effOutMs = seg.copyEndMs; // fall back: end at the copy RAP
        }

        remuxer.setSegment(&leadIn, &tail, seg.copyStartMs, seg.copyEndMs, effInMs, effOutMs, firstPts);
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
                                   const QVector<EditSegment>& segments, qint64 sourceSizeBytes)
{
    m_srcPath = srcPath;
    m_dstPath = dstPath;
    m_map = map;
    m_segments = segments;
    m_sourceSize = sourceSizeBytes;
    m_cancel.store(false);
}

void MmtsTsExportWorker::run()
{
    QString error;
    const bool ok = MmtsTsExporter::exportTs(
        m_srcPath, m_dstPath, m_map, m_segments, m_sourceSize, &error,
        [this](qint64 done, qint64 total) {
            emit progress(done, total);
            return !m_cancel.load();
        });
    emit finished(ok, error);
}
