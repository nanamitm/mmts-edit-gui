#include "MmtsTsExporter.h"

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

// Subclass of the dantto4k TS remuxer that rebases each segment's timestamps so
// the output stays contiguous, and suppresses NTP-driven PCR / subtitles (which
// would otherwise carry the original, non-contiguous clock).
class RebasingRemuxer : public RemuxerHandler {
public:
    explicit RebasingRemuxer(MmtTlv::MmtTlvDemuxer& demuxer) : RemuxerHandler(demuxer) {}

    void setOffsetMs(qint64 offsetMs) { m_offsetMs = offsetMs; }

    void onVideoData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu) override
    {
        MmtTlv::MfuData m = mfu;
        rebase(stream, m);
        RemuxerHandler::onVideoData(stream, m);
    }
    void onAudioData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu) override
    {
        MmtTlv::MfuData m = mfu;
        rebase(stream, m);
        RemuxerHandler::onAudioData(stream, m);
    }
    // Drop subtitles (their timing is derived from the original absolute clock,
    // not from mfu.pts, so rebasing here is not enough). v1 limitation.
    void onSubtitleData(const MmtTlv::MmtStream&, const MmtTlv::MfuData&) override {}
    // Suppress NTP-driven PCR so PCR comes only from the rebased video DTS.
    void onNtp(const MmtTlv::NTPv4&) override {}

private:
    void rebase(const MmtTlv::MmtStream& stream, MmtTlv::MfuData& m) const
    {
        if (m_offsetMs == 0)
            return;
        const auto& tb = stream.getTimeBase();
        if (tb.num <= 0 || tb.den <= 0)
            return;
        const int64_t ticks = m_offsetMs * static_cast<int64_t>(tb.den) / (1000LL * tb.num);
        if (m.pts != MmtTlv::NOPTS_VALUE)
            m.pts -= ticks;
        if (m.dts != MmtTlv::NOPTS_VALUE)
            m.dts -= ticks;
    }

    qint64 m_offsetMs = 0;
};

qint64 firstRapAtOrAfter(const QVector<MmtsPoint>& raps, qint64 t, qint64& timeOut, qint64 fallbackOffset, qint64 fallbackTime)
{
    for (const auto& p : raps) {
        if (p.timeMs >= t) {
            timeOut = p.timeMs;
            return static_cast<qint64>(p.offset);
        }
    }
    timeOut = fallbackTime;
    return fallbackOffset;
}

} // namespace

QVector<MmtsTsExporter::Segment> MmtsTsExporter::planSegments(const MmtsMap& map,
                                                             const QVector<EditSegment>& segments,
                                                             qint64 sourceSizeBytes)
{
    QVector<MmtsPoint> raps = map.rapPoints;
    std::sort(raps.begin(), raps.end(),
              [](const MmtsPoint& a, const MmtsPoint& b) { return a.timeMs < b.timeMs; });

    const qint64 firstPts = map.firstVideoPtsMs > 0 ? map.firstVideoPtsMs : 0;
    const qint64 endAbsFallback = (map.durationMs > 0 ? map.durationMs : 0) + firstPts;

    QVector<Segment> out;
    for (const auto& seg : segments) {
        Segment s;
        qint64 startT = 0, endT = 0;
        s.startByte = firstRapAtOrAfter(raps, seg.startMs + firstPts, startT, 0, firstPts);
        s.endByte = firstRapAtOrAfter(raps, seg.endMs + firstPts, endT, sourceSizeBytes, endAbsFallback);
        s.startMs = startT;
        s.endMs = endT;
        s.startByte = std::clamp<qint64>(s.startByte, 0, sourceSizeBytes);
        s.endByte = std::clamp<qint64>(s.endByte, 0, sourceSizeBytes);
        if (s.endByte > s.startByte && s.endMs > s.startMs)
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
        if (errorMessage)
            *errorMessage = QStringLiteral("Nothing to export (no segments map to RAP ranges).");
        return false;
    }

    std::ifstream ifs(std::filesystem::path(srcPath.toStdWString()), std::ios::binary);
    if (!ifs.is_open()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Cannot open source: %1").arg(srcPath);
        return false;
    }
    std::ofstream ofs(std::filesystem::path(dstPath.toStdWString()), std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Cannot create output: %1").arg(dstPath);
        return false;
    }

    MmtTlv::MmtTlvDemuxer demuxer;
    RebasingRemuxer remuxer(demuxer);
    remuxer.setOutputCallback([&ofs](const uint8_t* data, size_t size) {
        ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    });
    demuxer.setDemuxerHandler(remuxer);

    qint64 totalBytes = 0;
    for (const auto& s : plan)
        totalBytes += (s.endByte - s.startByte);
    qint64 doneBytes = 0;

    qint64 desiredStartMs = 0;
    constexpr size_t kChunk = 4 * 1024 * 1024;
    std::vector<uint8_t> buf;
    buf.reserve(kChunk * 2);

    for (const Segment& seg : plan) {
        remuxer.setOffsetMs(seg.startMs - desiredStartMs);
        demuxer.resetStreams();
        ifs.clear();
        ifs.seekg(seg.startByte, std::ios::beg);
        buf.clear();
        qint64 segPos = seg.startByte; // file offset of next byte to read

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
                    try {
                        status = demuxer.demux(stream);
                    } catch (...) {
                        break;
                    }
                    if (status == MmtTlv::DemuxStatus::NotEnoughBuffer)
                        break;
                    if (stream.getPos() == before)
                        break;
                }
                consumed = buf.size() - stream.leftBytes();
            }
            if (consumed > 0)
                buf.erase(buf.begin(), buf.begin() + consumed);
            else if (segPos >= seg.endByte)
                break; // no progress and nothing more to read for this segment

            if (progress && !progress(doneBytes, totalBytes)) {
                if (errorMessage)
                    *errorMessage = QStringLiteral("Cancelled");
                ofs.close();
                QFile::remove(dstPath);
                return false;
            }
        }

        desiredStartMs += (seg.endMs - seg.startMs);
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
