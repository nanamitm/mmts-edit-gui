#pragma once

#include "MmtsEdit.h"
#include "MmtsMap.h"

#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>
#include <functional>

// Non-destructive .mmts export (RAP-aligned stream copy, no re-encode).
//
// Each EDL segment is copied as the source byte range
//   [first RAP offset at/after segment IN, first RAP offset at/after segment OUT)
// taken from the .mmtsmap RAP table. The output is the concatenation of those
// ranges. Cuts therefore snap to RAP (GOP) boundaries -- matching how the
// DirectShow filter already starts each EDL segment on playback.
//
// Note: a single segment exports a clean, contiguous clip. Multiple segments
// keep their original MMT timestamps, so seams carry a PTS discontinuity
// (players resync at each segment's RAP). For frame-accurate cuts on a
// contiguous timeline use MmtsTsExporter (.ts) instead; re-muxing .mmts into
// a continuous timeline is impractical (no MMTP/signaling pack path in
// dantto4k).
class MmtsExporter {
public:
    struct Range {
        qint64 srcStart = 0; // inclusive byte offset in the source
        qint64 srcEnd = 0;   // exclusive byte offset in the source
        qint64 length() const { return srcEnd > srcStart ? srcEnd - srcStart : 0; }
    };

    // Computes the RAP-aligned source byte ranges for the EDL segments.
    static QVector<Range> planRanges(const MmtsMap& map,
                                     const QVector<EditSegment>& segments,
                                     qint64 sourceSizeBytes);

    // Copies the planned ranges from src to dst. progress(bytesDone,bytesTotal)
    // is called periodically; returning false from it cancels the export.
    static bool copyRanges(const QString& srcPath, const QString& dstPath,
                           const QVector<Range>& ranges, QString* errorMessage,
                           const std::function<bool(qint64, qint64)>& progress = {});
};

// Worker that runs the copy on a background thread (move to a QThread).
class MmtsExportWorker : public QObject {
    Q_OBJECT
public:
    void configure(const QString& srcPath, const QString& dstPath,
                   const QVector<MmtsExporter::Range>& ranges);
    void cancel() { m_cancel.store(true); }

public slots:
    void run();

signals:
    void progress(qint64 bytesDone, qint64 bytesTotal);
    void finished(bool ok, const QString& error);

private:
    QString m_srcPath;
    QString m_dstPath;
    QVector<MmtsExporter::Range> m_ranges;
    std::atomic<bool> m_cancel{false};
};
