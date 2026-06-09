#include "MmtsVideoDecoder.h"

#include "demuxerHandler.h"
#include "mmtStream.h"
#include "mmtTlvDemuxer.h"
#include "mpuProcessorBase.h" // MfuData, NOPTS_VALUE
#include "stream.h"           // Common::ReadStream

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstdio>

namespace {
constexpr size_t kChunkBytes = 1u * 1024 * 1024;
constexpr size_t kBufHighWater = 4u * 1024 * 1024;

qint64 ptsToMs(uint64_t pts, const MmtTlv::MmtStream& stream)
{
    if (pts == MmtTlv::NOPTS_VALUE)
        return -1;
    const auto& tb = stream.getTimeBase();
    if (tb.den <= 0)
        return static_cast<qint64>(pts / 90); // fallback: 90kHz
    return static_cast<qint64>((static_cast<double>(pts) * tb.num * 1000.0) / tb.den);
}
} // namespace

// ---------------------------------------------------------------------------
// VideoHandler: assembles complete HEVC access units from the demuxer.
// ---------------------------------------------------------------------------
class MmtsVideoDecoder::VideoHandler : public MmtTlv::DemuxerHandler {
public:
    void onVideoData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu) override
    {
        if (mfu.data.empty())
            return;

        const int streamIndex = static_cast<int>(stream.getStreamIndex());
        if (m_videoStreamIndex < 0)
            m_videoStreamIndex = streamIndex; // lock onto the first video asset
        if (streamIndex != m_videoStreamIndex)
            return;

        if (mfu.isFirstFragment) {
            m_cur.data.clear();
            m_cur.ptsMs = ptsToMs(mfu.pts, stream);
            m_cur.keyframe = false;
            m_assembling = true;
        }
        if (!m_assembling)
            return; // started mid-AU; wait for the next AU boundary

        m_cur.keyframe = m_cur.keyframe || mfu.keyframe;
        m_cur.data.insert(m_cur.data.end(), mfu.data.begin(), mfu.data.end());

        if (mfu.isLastFragment) {
            if (!m_cur.data.empty() && m_cur.ptsMs >= 0)
                m_queue.push_back(std::move(m_cur));
            m_cur = AccessUnit{};
            m_assembling = false;
        }
    }

    bool pop(AccessUnit& out)
    {
        if (m_queue.empty())
            return false;
        out = std::move(m_queue.front());
        m_queue.erase(m_queue.begin());
        return true;
    }

    bool empty() const { return m_queue.empty(); }

    // Discards buffered AUs but keeps the locked video stream index.
    void clearQueue()
    {
        m_queue.clear();
        m_cur = AccessUnit{};
        m_assembling = false;
    }

    int videoStreamIndex() const { return m_videoStreamIndex; }

private:
    std::vector<AccessUnit> m_queue;
    AccessUnit m_cur;
    bool m_assembling = false;
    int m_videoStreamIndex = -1;
};

// ---------------------------------------------------------------------------
MmtsVideoDecoder::MmtsVideoDecoder() = default;

MmtsVideoDecoder::~MmtsVideoDecoder()
{
    close();
}

bool MmtsVideoDecoder::open(const QString& mmtsPath, const MmtsMap& map, QString* errorMessage)
{
    close();

    m_ifs.open(mmtsPath.toStdWString(), std::ios::binary);
    if (!m_ifs.is_open()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Cannot open media file: %1").arg(mmtsPath);
        return false;
    }

    m_map = map;
    m_firstVideoPtsMs = map.firstVideoPtsMs >= 0 ? map.firstVideoPtsMs : 0;
    m_durationMs = map.durationMs > 0 ? map.durationMs : 0;

    // RAP points sorted by time for nearest-preceding lookup.
    std::sort(m_map.rapPoints.begin(), m_map.rapPoints.end(),
              [](const MmtsPoint& a, const MmtsPoint& b) { return a.timeMs < b.timeMs; });

    m_demuxer = std::make_unique<MmtTlv::MmtTlvDemuxer>();
    m_handler = std::make_unique<VideoHandler>();
    m_demuxer->setDemuxerHandler(*m_handler);

    if (!initCodec(errorMessage)) {
        close();
        return false;
    }
    return true;
}

bool MmtsVideoDecoder::initCodec(QString* errorMessage)
{
    av_log_set_level(AV_LOG_FATAL); // suppress per-NALU warmup spam after seeks

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!codec) {
        if (errorMessage)
            *errorMessage = QStringLiteral("HEVC decoder not available in ffmpeg build");
        return false;
    }
    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Failed to allocate HEVC decoder context");
        return false;
    }
    m_codecCtx->thread_count = 0; // auto
    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Failed to open HEVC decoder");
        return false;
    }
    m_frame = av_frame_alloc();
    m_packet = av_packet_alloc();
    if (!m_frame || !m_packet) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Failed to allocate ffmpeg frame/packet");
        return false;
    }
    return true;
}

void MmtsVideoDecoder::close()
{
    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    if (m_packet)
        av_packet_free(&m_packet);
    if (m_frame)
        av_frame_free(&m_frame);
    if (m_codecCtx)
        avcodec_free_context(&m_codecCtx);

    m_handler.reset();
    m_demuxer.reset();
    if (m_ifs.is_open())
        m_ifs.close();
    m_ifs.clear();

    m_buf.clear();
    m_swsSrcW = m_swsSrcH = m_swsDstW = m_swsDstH = 0;
    m_swsSrcFmt = -1;
    m_fps = 0.0;
    m_lastFpsPtsMs = -1;
    m_curAbsMs = -1;
    m_curRapOffset = -1;
    m_decoderPrimed = false;
    m_eof = false;
}

qint64 MmtsVideoDecoder::frameDurationMs() const
{
    if (m_fps > 0.0)
        return std::max<qint64>(1, static_cast<qint64>(1000.0 / m_fps + 0.5));
    return 33; // safe default until measured
}

void MmtsVideoDecoder::findRap(qint64 absMs, qint64& rapOffset, qint64& rapMs, qint64& prevRapOffset) const
{
    rapOffset = 0;
    rapMs = m_firstVideoPtsMs;
    prevRapOffset = 0;
    const auto& raps = m_map.rapPoints;
    qint64 prevOffset = 0;
    for (const auto& p : raps) {
        if (p.timeMs <= absMs) {
            prevRapOffset = prevOffset;          // the RAP before this one
            prevOffset = static_cast<qint64>(p.offset);
            rapOffset = static_cast<qint64>(p.offset);
            rapMs = p.timeMs;
        } else {
            break; // sorted
        }
    }
}

void MmtsVideoDecoder::seekTo(qint64 byteOffset)
{
    m_ifs.clear();
    m_ifs.seekg(static_cast<std::streamoff>(byteOffset), std::ios::beg);
    m_buf.clear();
    m_eof = false;
    if (m_demuxer)
        m_demuxer->resetStreams();
    if (m_handler)
        m_handler->clearQueue();
    if (m_codecCtx)
        avcodec_flush_buffers(m_codecCtx);
    m_decoderPrimed = false;
    m_lastFpsPtsMs = -1; // avoid bogus delta across the seek
}

bool MmtsVideoDecoder::fillBuffer()
{
    if (m_eof)
        return false;
    const size_t oldSize = m_buf.size();
    m_buf.resize(oldSize + kChunkBytes);
    m_ifs.read(reinterpret_cast<char*>(m_buf.data() + oldSize), kChunkBytes);
    const std::streamsize got = m_ifs.gcount();
    m_buf.resize(oldSize + static_cast<size_t>(got));
    if (got <= 0) {
        m_eof = true;
        return false;
    }
    return true;
}

void MmtsVideoDecoder::demuxAvailable()
{
    if (!m_demuxer || m_buf.empty())
        return;

    size_t consumed = 0;
    {
        MmtTlv::Common::ReadStream stream(m_buf);
        while (!stream.isEof()) {
            const size_t before = stream.getPos();
            MmtTlv::DemuxStatus status;
            try {
                status = m_demuxer->demux(stream);
            } catch (...) {
                break;
            }
            if (status == MmtTlv::DemuxStatus::NotEnoughBuffer)
                break;
            if (stream.getPos() == before)
                break; // no progress guard
        }
        consumed = m_buf.size() - stream.leftBytes();
    }
    if (consumed > 0)
        m_buf.erase(m_buf.begin(), m_buf.begin() + consumed);
}

bool MmtsVideoDecoder::ensureAu()
{
    while (m_handler->empty()) {
        const bool got = fillBuffer();
        demuxAvailable();
        if (m_handler->empty() && !got)
            return false; // EOF and nothing decodable left
    }
    return true;
}

void MmtsVideoDecoder::updateFps(qint64 ptsMs)
{
    // Prefer the codec-reported frame rate (from the HEVC SPS/VUI) when available.
    if (m_codecCtx && m_codecCtx->framerate.num > 0 && m_codecCtx->framerate.den > 0) {
        m_fps = static_cast<double>(m_codecCtx->framerate.num) / m_codecCtx->framerate.den;
        return;
    }
    // Fallback: measure from two consecutive decoded frames.
    if (m_fps <= 0.0 && m_lastFpsPtsMs >= 0 && ptsMs > m_lastFpsPtsMs) {
        const qint64 d = ptsMs - m_lastFpsPtsMs;
        if (d > 0 && d < 1000)
            m_fps = 1000.0 / static_cast<double>(d);
    }
    m_lastFpsPtsMs = ptsMs;
}

QImage MmtsVideoDecoder::decodeUntil(qint64 targetAbsMs, qint64* actualRelMs)
{
    for (;;) {
        AccessUnit au;
        const bool haveAu = ensureAu() && m_handler->pop(au);

        if (haveAu) {
            av_packet_unref(m_packet);
            m_packet->data = au.data.data();
            m_packet->size = static_cast<int>(au.data.size());
            m_packet->pts = au.ptsMs;
            m_packet->dts = AV_NOPTS_VALUE;
            if (avcodec_send_packet(m_codecCtx, m_packet) == 0)
                m_decoderPrimed = true;
        } else {
            // EOF: flush the decoder once and drain the remaining frames.
            avcodec_send_packet(m_codecCtx, nullptr);
        }

        for (;;) {
            const int ret = avcodec_receive_frame(m_codecCtx, m_frame);
            if (ret == AVERROR(EAGAIN))
                break; // need more input
            if (ret < 0) {
                // AVERROR_EOF or hard error: return whatever we last had.
                return QImage();
            }
            qint64 ptsMs = m_frame->pts;
            if (ptsMs == AV_NOPTS_VALUE)
                ptsMs = m_frame->best_effort_timestamp;
            updateFps(ptsMs);
            m_curAbsMs = ptsMs;

            if (ptsMs >= targetAbsMs || !haveAu) {
                if (actualRelMs)
                    *actualRelMs = ptsMs - m_firstVideoPtsMs;
                return convert(m_frame);
            }
            // Frame is before the target: normally discarded. When enabled, hand
            // it to the sink so a cache can keep it -- makes stepping back through
            // an already-decoded GOP instant instead of re-seeking each frame.
            if (m_emitIntermediate && m_frameSink)
                m_frameSink(ptsMs - m_firstVideoPtsMs, convert(m_frame));
        }

        if (!haveAu)
            return QImage(); // drained at EOF without reaching target
    }
}

QImage MmtsVideoDecoder::convert(const AVFrame* frame)
{
    if (frame->width <= 0 || frame->height <= 0)
        return QImage();

    int dstW = frame->width;
    int dstH = frame->height;
    if (m_previewMaxWidth > 0 && dstW > m_previewMaxWidth) {
        const double scale = static_cast<double>(m_previewMaxWidth) / dstW;
        dstW = m_previewMaxWidth;
        dstH = std::max(2, (static_cast<int>(frame->height * scale) / 2) * 2);
    }

    if (!m_sws || m_swsSrcW != frame->width || m_swsSrcH != frame->height ||
        m_swsSrcFmt != frame->format || m_swsDstW != dstW || m_swsDstH != dstH) {
        if (m_sws)
            sws_freeContext(m_sws);
        m_sws = sws_getContext(frame->width, frame->height,
                               static_cast<AVPixelFormat>(frame->format),
                               dstW, dstH, AV_PIX_FMT_BGRA,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
        m_swsSrcW = frame->width;
        m_swsSrcH = frame->height;
        m_swsSrcFmt = frame->format;
        m_swsDstW = dstW;
        m_swsDstH = dstH;
    }
    if (!m_sws)
        return QImage();

    QImage image(dstW, dstH, QImage::Format_RGB32);
    uint8_t* dstData[4] = { image.bits(), nullptr, nullptr, nullptr };
    int dstStride[4] = { static_cast<int>(image.bytesPerLine()), 0, 0, 0 };
    sws_scale(m_sws, frame->data, frame->linesize, 0, frame->height, dstData, dstStride);
    return image;
}

QImage MmtsVideoDecoder::frameAtMs(qint64 relativeMs, qint64* actualRelMs)
{
    if (!isOpen())
        return QImage();
    if (relativeMs < 0)
        relativeMs = 0;

    const qint64 absMs = relativeMs + m_firstVideoPtsMs;

    qint64 rapOffset = 0, rapMs = 0, prevRapOffset = 0;
    findRap(absMs, rapOffset, rapMs, prevRapOffset);

    // Decide between continuing forward and re-seeking. Continuing keeps the
    // decoder warm and is always frame-accurate, so we only re-seek when:
    //   * the decoder is not anchored yet, or
    //   * the target is behind the current frame (a decoder can't rewind), or
    //   * the target is far enough ahead that decoding everything in between
    //     would cost more than a seek + one GOP of pre-roll.
    // Small forward moves (incl. crossing a GOP boundary while stepping) just
    // continue -- no warm-up, exact frame.
    constexpr qint64 kForwardSeekThresholdMs = 1500;
    const bool needSeek = !m_decoderPrimed ||
                          absMs < m_curAbsMs ||
                          (absMs - m_curAbsMs) > kForwardSeekThresholdMs;

    // Pre-roll: the .mmtsmap RAP offset points at the RAP picture's slice, but
    // the HEVC parameter sets (VPS/SPS/PPS) for that GOP sit in packets just
    // before it -- seeking exactly there misses them, so the decoder cannot
    // produce a clean frame until the *next* RAP (a full GOP of warm-up, which
    // makes the target clamp ~half a GOP late). Backing the seek up a little so
    // we read this GOP's AUD+SPS+IDR in sequence fixes it. A few MB is plenty
    // (the parameter sets sit right before the slice); the previous RAP is the
    // safe floor so we never back up more than one GOP.
    constexpr qint64 kPrerollMarginBytes = 3 * 1024 * 1024;
    const qint64 seekOffset = std::max(prevRapOffset, rapOffset - kPrerollMarginBytes);

    if (qEnvironmentVariableIsSet("MMTS_DEBUG")) {
        fprintf(stderr, "frameAtMs rel=%lld abs=%lld rap(rel=%lld off=%lld) preroll=%lld curAbs=%lld seek=%d\n",
                (long long)relativeMs, (long long)absMs, (long long)(rapMs - m_firstVideoPtsMs),
                (long long)rapOffset, (long long)seekOffset,
                (long long)(m_curAbsMs - m_firstVideoPtsMs), needSeek ? 1 : 0);
    }
    if (needSeek) {
        seekTo(seekOffset);
        m_curRapOffset = rapOffset;
        m_curAbsMs = -1;
    }
    return decodeUntil(absMs, actualRelMs);
}

QImage MmtsVideoDecoder::stepFrames(int delta, qint64* actualRelMs)
{
    if (!isOpen())
        return QImage();
    const qint64 base = currentMs() >= 0 ? currentMs() : 0;
    const qint64 step = frameDurationMs();
    // Nudge slightly past the boundary so the comparison lands on the next frame.
    const qint64 target = base + static_cast<qint64>(delta) * step + (delta > 0 ? step / 2 : -step / 2);
    return frameAtMs(std::max<qint64>(0, target), actualRelMs);
}

void MmtsVideoDecoder::resync(qint64 curRelMs)
{
    m_curAbsMs = curRelMs + m_firstVideoPtsMs;
    m_decoderPrimed = false; // force the next frameAtMs() to re-seek
}
