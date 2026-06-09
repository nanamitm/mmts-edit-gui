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
// Phase 1: RAP-aligned segments, stream copy (no re-encode). Frame-accurate
// smart rendering (re-encoding boundary GOPs) is future work on top of this.
class MmtsTsExporter {
public:
    struct Segment {
        qint64 startByte = 0;   // source byte offset of the segment's first RAP
        qint64 endByte = 0;     // source byte offset to stop at
        qint64 startMs = 0;     // absolute presentation ms at startByte (a RAP time)
        qint64 endMs = 0;       // absolute presentation ms at endByte
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
