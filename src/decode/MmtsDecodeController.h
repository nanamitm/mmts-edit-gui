#pragma once

#include "MmtsMap.h"

#include <QHash>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPair>
#include <QString>
#include <QThread>
#include <memory>

class MmtsVideoDecoder;

// Runs the (heavy) HEVC decode on a worker thread so the UI never blocks.
//
// Responsiveness comes from two mechanisms:
//   * Coalescing: only the most recent pending request is decoded; intermediate
//     scrub positions are dropped while a decode is in flight ("latest wins").
//   * LRU frame cache: recently decoded frames are kept, so stepping back and
//     forth (the common editing action) is instant.
class MmtsDecodeWorker : public QObject {
    Q_OBJECT
public:
    MmtsDecodeWorker();
    ~MmtsDecodeWorker() override;

    enum class Kind { Abs, Step };
    struct Request {
        Kind kind = Kind::Abs;
        qint64 relMs = 0;
        int delta = 0;
        quint64 id = 0;
    };

    // Thread-safe: overwrites the single pending request (callable from UI thread).
    void setPending(const Request& req);

public slots:
    void open(const QString& path, const MmtsMap& map);
    void processPending();
    void shutdown();

signals:
    void opened(bool ok, qint64 durationMs, double fps, QString error);
    void frameReady(QImage image, qint64 relMs, quint64 requestId);

private:
    qint64 frameIndexOf(qint64 ms) const;
    qint64 frameTimeOf(qint64 index) const;
    bool cacheGet(qint64 index, QImage& image, qint64& ms);
    void cachePut(qint64 index, const QImage& image, qint64 ms);

    std::unique_ptr<MmtsVideoDecoder> m_decoder;

    QMutex m_mutex;
    Request m_pending;
    bool m_havePending = false;

    qint64 m_curMs = 0;
    qint64 m_curIndex = 0;

    QHash<qint64, QPair<QImage, qint64>> m_cache;
    QList<qint64> m_lru;
    int m_cacheCapacity = 24;
};

// UI-thread facade owning the worker thread.
class MmtsDecodeController : public QObject {
    Q_OBJECT
public:
    explicit MmtsDecodeController(QObject* parent = nullptr);
    ~MmtsDecodeController() override;

    void openMedia(const QString& path, const MmtsMap& map);
    // Return the id assigned to the request so the caller can recognise its
    // result (and ignore stale, in-flight frames from earlier requests).
    quint64 requestFrameAt(qint64 relMs);
    quint64 requestStep(int delta);

signals:
    void opened(bool ok, qint64 durationMs, double fps, QString error);
    void frameReady(QImage image, qint64 relMs, quint64 requestId);

private:
    QThread m_thread;
    MmtsDecodeWorker* m_worker = nullptr;
    quint64 m_seq = 0;
};
