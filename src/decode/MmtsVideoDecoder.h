#pragma once

#include "MmtsMap.h"

#include <QImage>
#include <QString>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <vector>

// Forward declarations for ffmpeg (avoid pulling C headers into Qt translation units).
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace MmtTlv {
class MmtTlvDemuxer;
}

// Frame-accurate HEVC decoder for (already-descrambled) .mmts files.
//
// Pipeline: dantto4k MmtTlvDemuxer -> HEVC access units (Annex-B) -> libavcodec
//           -> AVFrame -> swscale -> QImage.
//
// Seeking is frame-accurate: it jumps to the nearest preceding RAP byte offset
// (from the .mmtsmap), flushes the decoder, then decodes forward discarding
// frames until the requested presentation time is reached.
//
// All public times are RELATIVE milliseconds (0 = first video frame), matching
// the editor timeline / .mmtsmap duration_ms convention.
class MmtsVideoDecoder {
public:
    MmtsVideoDecoder();
    ~MmtsVideoDecoder();

    MmtsVideoDecoder(const MmtsVideoDecoder&) = delete;
    MmtsVideoDecoder& operator=(const MmtsVideoDecoder&) = delete;

    // Opens the media file. The map supplies RAP byte offsets and the
    // firstVideoPts baseline used to translate relative <-> absolute time.
    bool open(const QString& mmtsPath, const MmtsMap& map, QString* errorMessage = nullptr);
    void close();
    bool isOpen() const { return m_codecCtx != nullptr; }

    // Limit the decoded preview to this width (keeps aspect ratio). 0 = native.
    void setPreviewMaxWidth(int width) { m_previewMaxWidth = width; }

    // Optional sink that receives EVERY frame decoded while seeking forward to a
    // target (the ones normally discarded), so a cache can keep them. Enable it
    // only when it pays off (e.g. backward stepping) via setEmitIntermediate().
    void setFrameSink(std::function<void(qint64 relMs, const QImage&)> sink) { m_frameSink = std::move(sink); }
    void setEmitIntermediate(bool on) { m_emitIntermediate = on; }

    qint64 durationMs() const { return m_durationMs; }
    double frameRate() const { return m_fps; }
    // Estimated frame duration in ms (>= 1).
    qint64 frameDurationMs() const;

    // Last successfully decoded frame's relative presentation time, or -1.
    qint64 currentMs() const { return m_curAbsMs < 0 ? -1 : (m_curAbsMs - m_firstVideoPtsMs); }

    // Decodes the frame at or just after relativeMs (frame-accurate).
    // Returns a null QImage on failure. Optionally reports the actual relative
    // time of the returned frame.
    QImage frameAtMs(qint64 relativeMs, qint64* actualRelMs = nullptr);

    // Steps delta frames relative to the last decoded frame (+1 / -1 ...).
    QImage stepFrames(int delta, qint64* actualRelMs = nullptr);

    // Tells the decoder its logical current position changed externally (e.g. a
    // frame was served from a cache without touching the decoder). Forces the
    // next frameAtMs() to re-seek so decode stays correct.
    void resync(qint64 curRelMs);

private:
    struct AccessUnit {
        std::vector<uint8_t> data; // HEVC Annex-B (one access unit)
        qint64 ptsMs = -1;         // absolute presentation time (ms)
        bool keyframe = false;
    };

    class VideoHandler; // collects video access units from the demuxer

    bool initCodec(QString* errorMessage);
    void findRap(qint64 absMs, qint64& rapOffset, qint64& rapMs, qint64& prevRapOffset) const;
    void seekTo(qint64 byteOffset);
    bool ensureAu();          // make at least one AU available, false at EOF
    void demuxAvailable();    // consume m_buf -> AUs
    bool fillBuffer();        // read a chunk from file, false at EOF
    QImage decodeUntil(qint64 targetAbsMs, qint64* actualRelMs);
    QImage convert(const AVFrame* frame);
    void updateFps(qint64 ptsMs);

    std::ifstream m_ifs;
    MmtsMap m_map;
    qint64 m_firstVideoPtsMs = 0;
    qint64 m_durationMs = 0;

    std::unique_ptr<MmtTlv::MmtTlvDemuxer> m_demuxer;
    std::unique_ptr<VideoHandler> m_handler;

    std::vector<uint8_t> m_buf;       // demux input buffer
    std::ifstream::pos_type m_eofGuard = 0;

    AVCodecContext* m_codecCtx = nullptr;
    AVFrame* m_frame = nullptr;
    AVPacket* m_packet = nullptr;
    SwsContext* m_sws = nullptr;
    int m_swsSrcW = 0, m_swsSrcH = 0, m_swsSrcFmt = -1, m_swsDstW = 0, m_swsDstH = 0;

    std::function<void(qint64 relMs, const QImage&)> m_frameSink;
    bool m_emitIntermediate = false;

    int m_previewMaxWidth = 0;
    double m_fps = 0.0;
    qint64 m_lastFpsPtsMs = -1;

    qint64 m_curAbsMs = -1;      // last decoded frame (absolute ms)
    qint64 m_curRapOffset = -1;  // RAP offset the decoder is currently anchored to
    bool m_decoderPrimed = false;
    bool m_eof = false;
};
