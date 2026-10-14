#include "HevcReencoder.h"

#include "demuxerHandler.h"
#include "mmtStream.h"
#include "mmtTlvDemuxer.h"
#include "mpuProcessorBase.h" // MfuData, NOPTS_VALUE
#include "stream.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace {
constexpr size_t kChunk = 1u * 1024 * 1024;

qint64 ptsToMs(uint64_t pts, const MmtTlv::MmtStream& s)
{
    if (pts == MmtTlv::NOPTS_VALUE)
        return -1;
    const auto& tb = s.getTimeBase();
    if (tb.den <= 0)
        return static_cast<qint64>(pts / 90);
    return static_cast<qint64>((static_cast<double>(pts) * tb.num * 1000.0) / tb.den);
}

// Collects complete HEVC access units (one video stream) from the demuxer.
class VideoHandler : public MmtTlv::DemuxerHandler {
public:
    struct Au { std::vector<uint8_t> data; qint64 ptsMs = -1; bool keyframe = false; };

    void onVideoData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu) override
    {
        if (mfu.data.empty())
            return;
        const int idx = static_cast<int>(stream.getStreamIndex());
        if (m_video < 0)
            m_video = idx;
        if (idx != m_video)
            return;
        if (mfu.isFirstFragment) {
            m_cur = Au{};
            m_cur.ptsMs = ptsToMs(mfu.pts, stream);
            m_assembling = true;
        }
        if (!m_assembling)
            return;
        m_cur.keyframe = m_cur.keyframe || mfu.keyframe;
        m_cur.data.insert(m_cur.data.end(), mfu.data.begin(), mfu.data.end());
        if (mfu.isLastFragment) {
            if (!m_cur.data.empty() && m_cur.ptsMs >= 0)
                m_queue.push_back(std::move(m_cur));
            m_cur = Au{};
            m_assembling = false;
        }
    }
    bool pop(Au& out)
    {
        if (m_queue.empty())
            return false;
        out = std::move(m_queue.front());
        m_queue.erase(m_queue.begin());
        return true;
    }

private:
    std::vector<Au> m_queue;
    Au m_cur;
    bool m_assembling = false;
    int m_video = -1;
};
} // namespace

bool HevcReencoder::encoderAvailable(const QString& name)
{
    return avcodec_find_encoder_by_name(name.toUtf8().constData()) != nullptr;
}

bool HevcReencoder::reencode(const QString& mmtsPath, const MmtsMap& map,
                             qint64 startRelMs, qint64 endRelMs,
                             std::vector<Au>& out, QString* errorMessage)
{
    out.clear();
    av_log_set_level(AV_LOG_FATAL);

    std::ifstream ifs(std::filesystem::path(mmtsPath.toStdWString()), std::ios::binary);
    if (!ifs.is_open()) {
        if (errorMessage) *errorMessage = QStringLiteral("cannot open %1").arg(mmtsPath);
        return false;
    }

    const qint64 firstPts = map.firstVideoPtsMs > 0 ? map.firstVideoPtsMs : 0;
    const qint64 absStart = startRelMs + firstPts;
    const qint64 absEnd = endRelMs + firstPts;

    // Seek to the RAP before the start (preroll) so the decoder is warm by absStart.
    QVector<MmtsPoint> raps = map.rapPoints;
    std::sort(raps.begin(), raps.end(), [](const MmtsPoint& a, const MmtsPoint& b) { return a.timeMs < b.timeMs; });
    qint64 seekOffset = 0, prev = 0;
    for (const auto& p : raps) {
        if (p.timeMs <= absStart) { seekOffset = prev; prev = static_cast<qint64>(p.offset); }
        else break;
    }
    ifs.seekg(seekOffset, std::ios::beg);

    MmtTlv::MmtTlvDemuxer demuxer;
    VideoHandler handler;
    demuxer.setDemuxerHandler(handler);

    const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    AVCodecContext* decCtx = dec ? avcodec_alloc_context3(dec) : nullptr;
    if (!decCtx || avcodec_open2(decCtx, dec, nullptr) < 0) {
        if (errorMessage) *errorMessage = QStringLiteral("HEVC decoder unavailable");
        if (decCtx) avcodec_free_context(&decCtx);
        return false;
    }

    const AVCodec* enc = avcodec_find_encoder_by_name(m_encoderName.toUtf8().constData());
    if (!enc) {
        if (errorMessage) *errorMessage = QStringLiteral("encoder %1 not found").arg(m_encoderName);
        avcodec_free_context(&decCtx);
        return false;
    }

    AVCodecContext* encCtx = nullptr;
    SwsContext* sws = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* encFrame = nullptr;
    bool ok = true;
    bool done = false;

    auto drainEncoder = [&](bool flush) {
        if (!encCtx) return;
        if (flush) avcodec_send_frame(encCtx, nullptr);
        for (;;) {
            AVPacket* op = av_packet_alloc();
            const int r = avcodec_receive_packet(encCtx, op);
            if (r == 0) {
                Au au;
                au.data.assign(op->data, op->data + op->size);
                au.ptsMs = op->pts;
                au.dtsMs = op->dts;
                au.keyframe = (op->flags & AV_PKT_FLAG_KEY) != 0;
                out.push_back(std::move(au));
            }
            av_packet_free(&op);
            if (r != 0) break;
        }
    };

    auto encodeFrame = [&](AVFrame* f, qint64 relMs) -> bool {
        if (!encCtx) {
            encCtx = avcodec_alloc_context3(enc);
            encCtx->width = f->width;
            encCtx->height = f->height;
            const bool tenBit = (f->format == AV_PIX_FMT_YUV420P10LE || f->format == AV_PIX_FMT_P010LE);
            encCtx->pix_fmt = tenBit ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
            encCtx->time_base = AVRational{1, 1000}; // ms
            encCtx->framerate = (decCtx->framerate.num > 0) ? decCtx->framerate : AVRational{60000, 1001};
            encCtx->gop_size = 600;  // long: the short range is effectively one GOP (IDR at start)
            encCtx->max_b_frames = 0; // no reorder -> dts==pts, simpler splicing
            if (QString(enc->name).startsWith("hevc_nvenc")) {
                av_opt_set(encCtx->priv_data, "preset", "p4", 0);
                av_opt_set(encCtx->priv_data, "tune", "hq", 0);
            }
            if (avcodec_open2(encCtx, enc, nullptr) < 0) {
                if (errorMessage) *errorMessage = QStringLiteral("failed to open encoder %1").arg(m_encoderName);
                return false;
            }
            sws = sws_getContext(f->width, f->height, static_cast<AVPixelFormat>(f->format),
                                 f->width, f->height, encCtx->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
            encFrame = av_frame_alloc();
            encFrame->format = encCtx->pix_fmt;
            encFrame->width = f->width;
            encFrame->height = f->height;
            av_frame_get_buffer(encFrame, 32);
        }
        if (!sws) return false;
        av_frame_make_writable(encFrame);
        sws_scale(sws, f->data, f->linesize, 0, f->height, encFrame->data, encFrame->linesize);
        encFrame->pts = relMs;
        if (avcodec_send_frame(encCtx, encFrame) < 0)
            return false;
        drainEncoder(false);
        return true;
    };

    std::vector<uint8_t> buf;
    buf.reserve(kChunk * 2);
    bool eof = false;

    while (ok && !done) {
        if (buf.size() < 2 * 1024 * 1024 && !eof) {
            const size_t oldSz = buf.size();
            buf.resize(oldSz + kChunk);
            ifs.read(reinterpret_cast<char*>(buf.data() + oldSz), kChunk);
            const std::streamsize got = ifs.gcount();
            buf.resize(oldSz + static_cast<size_t>(got));
            if (got <= 0) eof = true;
        }

        VideoHandler::Au au;
        bool gotAu = false;
        {
            size_t consumed = 0;
            {
                MmtTlv::Common::ReadStream stream(buf);
                while (!stream.isEof()) {
                    const size_t before = stream.getPos();
                    MmtTlv::DemuxStatus st;
                    try { st = demuxer.demux(stream); } catch (...) { break; }
                    if (st == MmtTlv::DemuxStatus::NotEnoughBuffer) break;
                    if (stream.getPos() == before) break;
                }
                consumed = buf.size() - stream.leftBytes();
            }
            if (consumed > 0) buf.erase(buf.begin(), buf.begin() + consumed);
        }
        gotAu = handler.pop(au);
        if (!gotAu && eof) {
            // flush decoder to drain trailing frames
            avcodec_send_packet(decCtx, nullptr);
        } else if (gotAu) {
            av_packet_unref(pkt);
            pkt->data = au.data.data();
            pkt->size = static_cast<int>(au.data.size());
            pkt->pts = au.ptsMs; // already absolute ms (ptsToMs); decoder passes through
            pkt->dts = AV_NOPTS_VALUE;
            avcodec_send_packet(decCtx, pkt);
        } else {
            continue; // need more data
        }

        for (;;) {
            const int r = avcodec_receive_frame(decCtx, frame);
            if (r == AVERROR(EAGAIN)) break;
            if (r < 0) { done = true; break; }
            qint64 framePtsAbs = frame->pts;
            if (framePtsAbs == AV_NOPTS_VALUE) framePtsAbs = frame->best_effort_timestamp;
            // pkt.pts was set to au.ptsMs+firstPts (abs ms); decoder passes through.
            if (framePtsAbs < absStart) continue;          // warmup / lead-in
            if (framePtsAbs >= absEnd) { done = true; break; } // past range
            if (!encodeFrame(frame, framePtsAbs - firstPts)) { ok = false; done = true; break; }
        }

        if (!gotAu && eof)
            break; // decoder flushed and drained
    }

    drainEncoder(true); // flush encoder

    av_frame_free(&frame);
    if (encFrame) av_frame_free(&encFrame);
    av_packet_free(&pkt);
    if (sws) sws_freeContext(sws);
    if (encCtx) avcodec_free_context(&encCtx);
    avcodec_free_context(&decCtx);

    if (ok && out.empty()) {
        if (errorMessage) *errorMessage = QStringLiteral("no frames re-encoded in range");
        return false;
    }
    return ok;
}
