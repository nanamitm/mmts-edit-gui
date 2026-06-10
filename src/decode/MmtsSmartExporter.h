#pragma once

#include "MmtsEdit.h"
#include "MmtsMap.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <functional>

// Frame-accurate .mmts export (smart rendering in the MMT/TLV domain).
//
// Per EDL segment the source is split at MPU boundaries:
//   [boundary IN]   original video MPUs dropped, replaced by one synthesized
//                   MPU carrying the re-encoded [IN, copyStart) GOP (IDR-led);
//   [copy]          byte-identical TLV packet pass-through of the complete
//                   MPUs between the RAP boundaries (lossless);
//   [boundary OUT]  original video MPUs dropped, replaced by the re-encoded
//                   [copyEnd, OUT) tail MPU.
// MPT packets inside the boundary windows are rewritten so their
// mpu_timestamp / mpu_extended_timestamp entries describe the synthesized
// MPUs (dantto4k mmtSplice primitives); audio/subtitle MPUs are gated to
// [IN, OUT) at MPU granularity.
//
// The output timeline is CONTINUOUS: in MMT all presentation timestamps live
// in the MPT's mpu_timestamp entries (media packets carry none), so segments
// after the first are rebased by rewriting those entries in every MPT (and
// the NTP packets, which seed the PCR on TS conversion). Media packets stay
// byte-identical; only signaling carries the shift. MPU/packet sequence
// numbers still jump at the seams, which the dantto4k MPU-resync fix handles.
class MmtsSmartExporter {
public:
    static bool exportMmts(const QString& srcPath, const QString& dstPath,
                           const MmtsMap& map, const QVector<EditSegment>& segments,
                           qint64 sourceSizeBytes, QString* errorMessage,
                           const std::function<bool(qint64, qint64)>& progress = {},
                           QStringList* warnings = nullptr,
                           bool requireFrameAccurate = false);
};

class MmtsSmartExportWorker : public QObject {
    Q_OBJECT
public:
    void configure(const QString& srcPath, const QString& dstPath, const MmtsMap& map,
                   const QVector<EditSegment>& segments, qint64 sourceSizeBytes,
                   bool requireFrameAccurate = false);
    void cancel() { m_cancel.store(true); }

public slots:
    void run();

signals:
    void progress(qint64 bytesDone, qint64 bytesTotal);
    void finished(bool ok, const QString& error, const QStringList& warnings);

private:
    QString m_srcPath;
    QString m_dstPath;
    MmtsMap m_map;
    QVector<EditSegment> m_segments;
    qint64 m_sourceSize = 0;
    bool m_requireFrameAccurate = false;
    std::atomic<bool> m_cancel{false};
};
