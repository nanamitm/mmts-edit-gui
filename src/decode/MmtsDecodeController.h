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
#include <QVector>
#include <memory>

class MmtsVideoDecoder;

// Thread-safe LRU cache of decoded frames (index -> image), shared between the
// main decode worker and the background prefetch worker.
class FrameCache {
public:
    void setCapacity(int c) { QMutexLocker l(&m_mutex); m_capacity = c; }
    void clear()
    {
        QMutexLocker l(&m_mutex);
        m_cache.clear();
        m_lru.clear();
    }
    bool contains(qint64 index)
    {
        QMutexLocker l(&m_mutex);
        return m_cache.contains(index);
    }
    bool get(qint64 index, QImage& image, qint64& ms)
    {
        QMutexLocker l(&m_mutex);
        auto it = m_cache.find(index);
        if (it == m_cache.end())
            return false;
        image = it.value().first;
        ms = it.value().second;
        m_lru.removeOne(index);
        m_lru.append(index);
        return true;
    }
    void put(qint64 index, const QImage& image, qint64 ms)
    {
        QMutexLocker l(&m_mutex);
        if (m_cache.contains(index))
            m_lru.removeOne(index);
        m_cache.insert(index, qMakePair(image, ms));
        m_lru.append(index);
        while (m_lru.size() > m_capacity) {
            const qint64 evict = m_lru.takeFirst();
            m_cache.remove(evict);
        }
    }

private:
    QMutex m_mutex;
    QHash<qint64, QPair<QImage, qint64>> m_cache;
    QList<qint64> m_lru;
    int m_capacity = 96; // ~3 GOPs at 8K (current + prefetched neighbours)
};

// Main decode worker: serves frame requests with coalescing, using the shared
// cache. Runs on its own thread.
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

    void setCache(std::shared_ptr<FrameCache> c) { m_cache = std::move(c); }
    void setPending(const Request& req); // thread-safe

public slots:
    void open(const QString& path, const MmtsMap& map, quint64 generation);
    void processPending();
    void shutdown();

signals:
    void opened(bool ok, qint64 durationMs, double fps, QString error, quint64 generation);
    void frameReady(QImage image, qint64 relMs, quint64 requestId, quint64 generation);

private:
    qint64 frameIndexOf(qint64 ms) const;
    qint64 frameTimeOf(qint64 index) const;

    std::unique_ptr<MmtsVideoDecoder> m_decoder;
    std::shared_ptr<FrameCache> m_cache;

    QMutex m_mutex;
    Request m_pending;
    bool m_havePending = false;

    quint64 m_generation = 0;
    qint64 m_curMs = 0;
    qint64 m_curIndex = 0;
};

// Background prefetch worker: a second decoder that fills the shared cache with
// the GOP just before the current position, so stepping backward across a GOP
// boundary is a cache hit instead of a re-seek. Runs on its own thread.
class MmtsPrefetchWorker : public QObject {
    Q_OBJECT
public:
    MmtsPrefetchWorker();
    ~MmtsPrefetchWorker() override;

    void setCache(std::shared_ptr<FrameCache> c) { m_cache = std::move(c); }
    void setPending(qint64 currentRelMs); // thread-safe

public slots:
    void open(const QString& path, const MmtsMap& map);
    void prefetch();
    void shutdown();

private:
    qint64 frameIndexOf(qint64 ms) const;

    std::unique_ptr<MmtsVideoDecoder> m_decoder;
    std::shared_ptr<FrameCache> m_cache;
    QVector<qint64> m_rapRel; // RAP times, relative ms, sorted

    QMutex m_mutex;
    qint64 m_pendingRel = -1;
    bool m_havePending = false;
};

// UI-thread facade owning the decode + prefetch threads and the shared cache.
class MmtsDecodeController : public QObject {
    Q_OBJECT
public:
    explicit MmtsDecodeController(QObject* parent = nullptr);
    ~MmtsDecodeController() override;

    void openMedia(const QString& path, const MmtsMap& map);
    void closeMedia();
    quint64 requestFrameAt(qint64 relMs);
    quint64 requestStep(int delta);

signals:
    void opened(bool ok, qint64 durationMs, double fps, QString error);
    void frameReady(QImage image, qint64 relMs, quint64 requestId);

private:
    std::shared_ptr<FrameCache> m_cache = std::make_shared<FrameCache>();
    quint64 m_generation = 0;
    QThread m_thread;
    QThread m_prefetchThread;
    MmtsDecodeWorker* m_worker = nullptr;
    MmtsPrefetchWorker* m_prefetch = nullptr;
    quint64 m_seq = 0;
};
