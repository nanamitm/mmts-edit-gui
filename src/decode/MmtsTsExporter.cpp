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

// TS remuxer that: rebases timestamps per segment (base setPtsOffset), drops the
// lead-in that the re-encoded GOP replaces, injects the re-encoded AUs, and gates
// audio/subtitle to the frame-accurate IN. NTP-driven PCR is suppressed.
class SmartRebasingRemuxer : public RemuxerHandler {
public:
    explicit SmartRebasingRemuxer(MmtTlv::MmtTlvDemuxer& d) : RemuxerHandler(d) {}

    void setOffsetMs(qint64 offsetMs) { setPtsOffset(offsetMs * 90); }
    void setSegment(const std::vector<HevcReencoder::Au>* reAus, qint64 rNextMs, qint64 inAbsMs, qint64 firstPts)
    {
        m_reAus = reAus;
        m_rNextMs = rNextMs;
        m_inAbsMs = inAbsMs;
        m_firstPts = firstPts;
        m_videoStream = nullptr;
        m_injected = false;
    }

    void onVideoData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu) override
    {
        if (!m_videoStream)
            m_videoStream = &stream;
        const qint64 abs = ptsToMs(mfu.pts, stream);
        if (abs >= 0 && abs < m_rNextMs)
            return; // lead-in: replaced by the re-encoded GOP
        if (!m_injected) {
            injectReencoded();
            m_injected = true;
        }
        RemuxerHandler::onVideoData(stream, mfu);
    }
    void onAudioData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu) override
    {
        const qint64 abs = ptsToMs(mfu.pts, stream);
        if (abs >= 0 && abs < m_inAbsMs)
            return; // before the frame-accurate IN
        RemuxerHandler::onAudioData(stream, mfu);
    }
    void onSubtitleData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu) override
    {
        const qint64 abs = ptsToMs(mfu.pts, stream);
        if (abs >= 0 && abs < m_inAbsMs)
            return;
        RemuxerHandler::onSubtitleData(stream, mfu);
    }
    void onNtp(const MmtTlv::NTPv4&) override {}

private:
    void injectReencoded()
    {
        if (!m_videoStream || !m_reAus || m_reAus->empty())
            return;
        const auto& tb = m_videoStream->getTimeBase();
        for (const auto& au : *m_reAus) {
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

    const std::vector<HevcReencoder::Au>* m_reAus = nullptr;
    qint64 m_rNextMs = 0;
    qint64 m_inAbsMs = 0;
    qint64 m_firstPts = 0;
    const MmtTlv::MmtStream* m_videoStream = nullptr;
    bool m_injected = false;
};

} // namespace

QVector<MmtsTsExporter::Segment> MmtsTsExporter::planSegments(const MmtsMap& map,
                                                             const QVector<EditSegment>& segments,
                                                             qint64 sourceSizeBytes)
{
    QVector<MmtsPoint> raps = map.rapPoints;
    std::sort(raps.begin(), raps.end(), [](const MmtsPoint& a, const MmtsPoint& b) { return a.timeMs < b.timeMs; });

    const qint64 firstPts = map.firstVideoPtsMs > 0 ? map.firstVideoPtsMs : 0;
    const qint64 endAbsFallback = (map.durationMs > 0 ? map.durationMs : 0) + firstPts;
    constexpr qint64 kReencodeThresholdMs = 20; // IN within ~1 frame of a RAP -> no re-encode

    QVector<Segment> out;
    for (const auto& seg : segments) {
        Segment s;
        s.inAbsMs = seg.startMs + firstPts;

        // RAP at/before IN (demux start) and at/after IN (copy start).
        qint64 prevByte = 0, prevMs = firstPts;
        qint64 nextByte = -1, nextMs = -1;
        for (const auto& p : raps) {
            if (p.timeMs <= s.inAbsMs) {
                prevByte = static_cast<qint64>(p.offset);
                prevMs = p.timeMs;
            } else {
                nextByte = static_cast<qint64>(p.offset);
                nextMs = p.timeMs;
                break;
            }
        }
        if (nextByte < 0) { // IN at/after last RAP: copy from the RAP at/before
            nextByte = prevByte;
            nextMs = prevMs;
        }
        s.prerollByte = prevByte;
        s.rNextByte = nextByte;
        s.rNextMs = nextMs;

        // End: RAP at/after the EDL out point.
        const qint64 outAbs = seg.endMs + firstPts;
        qint64 endByte = sourceSizeBytes, endMs = endAbsFallback;
        for (const auto& p : raps) {
            if (p.timeMs >= outAbs) { endByte = static_cast<qint64>(p.offset); endMs = p.timeMs; break; }
        }
        s.endByte = std::clamp<qint64>(endByte, 0, sourceSizeBytes);
        s.endMs = endMs;
        s.prerollByte = std::clamp<qint64>(s.prerollByte, 0, sourceSizeBytes);
        s.rNextByte = std::clamp<qint64>(s.rNextByte, 0, sourceSizeBytes);

        s.reencode = (s.rNextMs - s.inAbsMs) > kReencodeThresholdMs;

        if (s.endByte > s.prerollByte && s.endMs > s.inAbsMs)
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

    for (const Segment& seg : plan) {
        std::vector<HevcReencoder::Au> reAus;
        qint64 effInMs = seg.inAbsMs;
        if (seg.reencode) {
            QString rerr;
            if (!reenc.reencode(srcPath, map, seg.inAbsMs - firstPts, seg.rNextMs - firstPts, reAus, &rerr)) {
                // Fall back to RAP-aligned IN for this segment.
                reAus.clear();
                effInMs = seg.rNextMs;
            }
        }

        remuxer.setSegment(&reAus, seg.rNextMs, effInMs, firstPts);
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

        desiredStartMs += (seg.endMs - effInMs);
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
