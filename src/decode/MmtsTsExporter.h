#pragma once

#include "MmtsEdit.h"
#include "MmtsMap.h"

#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>
#include <functional>

// Exports the EDL as a single MPEG-2 TS (.ts) by demuxing each source segment
// and re-muxing through dantto4k's RemuxerHandler (HEVC + AAC in TS). Per-segment
// PTS/DTS are rebased so the output timeline is contiguous across cuts (no PTS
// discontinuity) -- unlike the raw .mmts byte-copy export.
//
// Smart rendering: both IN and OUT points are frame-accurate. The video is
//   re-encode [IN, copyStart)  ++  copy [copyStart, copyEnd)  ++  re-encode [copyEnd, OUT)
// where copyStart/copyEnd are RAP boundaries. A partial lead-in / tail GOP is
// re-encoded (IDR-led) and spliced around the stream copy; a segment shorter
// than one GOP is re-encoded whole. Audio/subtitle are gated to [IN, OUT).
class MmtsTsExporter {
public:
    struct Segment {
        qint64 inAbsMs = 0;      // frame-accurate IN, absolute presentation ms
        qint64 outAbsMs = 0;     // frame-accurate OUT (exclusive), absolute ms
        qint64 prerollByte = 0;  // demux start: RAP at/before IN
        qint64 endByte = 0;      // demux end: RAP at/after OUT
        qint64 copyStartMs = 0;  // stream-copy video range (abs ms); empty if >= copyEndMs
        qint64 copyEndMs = 0;
    };

    static QVector<Segment> planSegments(const MmtsMap& map,
                                         const QVector<EditSegment>& segments,
                                         qint64 sourceSizeBytes);

    static bool exportTs(const QString& srcPath, const QString& dstPath,
                         const MmtsMap& map, const QVector<EditSegment>& segments,
                         qint64 sourceSizeBytes, QString* errorMessage,
                         const std::function<bool(qint64, qint64)>& progress = {});
};

class MmtsTsExportWorker : public QObject {
    Q_OBJECT
public:
    void configure(const QString& srcPath, const QString& dstPath, const MmtsMap& map,
                   const QVector<EditSegment>& segments, qint64 sourceSizeBytes);
    void cancel() { m_cancel.store(true); }

public slots:
    void run();

signals:
    void progress(qint64 bytesDone, qint64 bytesTotal);
    void finished(bool ok, const QString& error);

private:
    QString m_srcPath;
    QString m_dstPath;
    MmtsMap m_map;
    QVector<EditSegment> m_segments;
    qint64 m_sourceSize = 0;
    std::atomic<bool> m_cancel{false};
};
