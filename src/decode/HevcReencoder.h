#pragma once

#include "MmtsMap.h"

#include <QString>
#include <cstdint>
#include <functional>
#include <vector>

// Re-encodes a frame range of a (descrambled) .mmts to fresh HEVC access units
// for smart rendering: decode [startRelMs, endRelMs) and encode it via a
// hardware HEVC encoder (hevc_nvenc) as a self-contained GOP whose first frame
// is an IDR. The resulting AUs (Annex-B) splice cleanly in front of a stream
// copy that resumes at the next source RAP, giving a frame-accurate IN cut.
//
// Streaming: frames are encoded as they are decoded (8K 10-bit frames are ~100MB
// each, so buffering a whole GOP is not viable); only the small encoded AUs are
// collected.
class HevcReencoder {
public:
    struct Au {
        std::vector<uint8_t> data; // Annex-B HEVC access unit
        qint64 ptsMs = -1;         // source-relative presentation time
        qint64 dtsMs = -1;
        bool keyframe = false;
    };

    // Encoder backend by ffmpeg name (e.g. "hevc_nvenc", "libkvazaar").
    void setEncoderName(const QString& name) { m_encoderName = name; }

    static bool encoderAvailable(const QString& name);

    bool reencode(const QString& mmtsPath, const MmtsMap& map,
                  qint64 startRelMs, qint64 endRelMs,
                  std::vector<Au>& out, QString* errorMessage = nullptr);

private:
    QString m_encoderName = QStringLiteral("hevc_nvenc");
};
