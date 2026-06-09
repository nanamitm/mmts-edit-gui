#pragma once

#include <QObject>
#include <QString>
#include <atomic>
#include <functional>

// Generates a binary .mmtsmap sidecar for an (already-descrambled) .mmts by
// scanning the whole file with the dantto4k demuxer + MmtsMapWriter -- the same
// output as `dantto4k --write-mmtsmap-only`, but in-process (no external exe).
class MmtsMapBuilder {
public:
    // Scans srcPath and writes mapPath. progress(bytesDone,bytesTotal) is called
    // periodically; returning false cancels (and removes the partial map).
    static bool build(const QString& srcPath, const QString& mapPath, QString* errorMessage,
                      const std::function<bool(qint64, qint64)>& progress = {});
};

// Worker that runs the scan on a background thread (move to a QThread).
class MmtsMapBuildWorker : public QObject {
    Q_OBJECT
public:
    void configure(const QString& srcPath, const QString& mapPath);
    void cancel() { m_cancel.store(true); }

public slots:
    void run();

signals:
    void progress(qint64 bytesDone, qint64 bytesTotal);
    void finished(bool ok, const QString& error);

private:
    QString m_srcPath;
    QString m_mapPath;
    std::atomic<bool> m_cancel{false};
};
