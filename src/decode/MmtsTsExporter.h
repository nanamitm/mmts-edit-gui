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
// Smart rendering: the IN point is frame-accurate -- if it does not sit on a
// RAP, the lead-in [IN, nextRAP) is re-encoded as a fresh IDR-led GOP and spliced
// in front of the stream copy that resumes at nextRAP. The OUT point stays
// RAP-aligned for now.
class MmtsTsExporter {
public:
    struct Segment {
        qint64 inAbsMs = 0;      // frame-accurate IN, absolute presentation ms
        qint64 prerollByte = 0;  // demux start: RAP at/before IN
        qint64 rNextByte = 0;    // stream copy start: RAP at/after IN
        qint64 rNextMs = 0;      // absolute presentation ms at rNextByte
        qint64 endByte = 0;      // demux/copy end: RAP at/after the EDL out
        qint64 endMs = 0;        // absolute presentation ms at endByte
        bool reencode = false;   // IN is not on a RAP -> re-encode [IN, rNext)
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
