#include "MmtsDecodeController.h"

#include "MmtsVideoDecoder.h"

#include <cmath>

// ---------------------------------------------------------------------------
// Worker (runs on the decode thread)
// ---------------------------------------------------------------------------
MmtsDecodeWorker::MmtsDecodeWorker() = default;
MmtsDecodeWorker::~MmtsDecodeWorker() = default;

void MmtsDecodeWorker::setPending(const Request& req)
{
    QMutexLocker lock(&m_mutex);
    m_pending = req;
    m_havePending = true;
}

void MmtsDecodeWorker::open(const QString& path, const MmtsMap& map)
{
    m_decoder = std::make_unique<MmtsVideoDecoder>();
    m_decoder->setPreviewMaxWidth(960);

    m_cache.clear();
    m_lru.clear();
    m_curMs = 0;
    m_curIndex = 0;
    {
        QMutexLocker lock(&m_mutex);
        m_havePending = false;
    }

    QString error;
    if (!m_decoder->open(path, map, &error)) {
        m_decoder.reset();
        emit opened(false, 0, 0.0, error);
        return;
    }

    // Prime: decode the first frame so the frame rate (from the HEVC SPS/VUI) is
    // known before we report 'opened'. Frame stepping and the cache both depend
    // on a valid fps to map frame indices <-> time.
    qint64 actual = 0;
    const QImage first = m_decoder->frameAtMs(0, &actual);
    if (!first.isNull()) {
        m_curMs = actual;
        m_curIndex = frameIndexOf(actual);
        cachePut(m_curIndex, first, actual);
    }
    emit opened(true, m_decoder->durationMs(), m_decoder->frameRate(), QString());
}

qint64 MmtsDecodeWorker::frameTimeOf(qint64 index) const
{
    const double fps = m_decoder ? m_decoder->frameRate() : 0.0;
    if (fps > 0.0)
        return llround(index * 1000.0 / fps);
    return index;
}

void MmtsDecodeWorker::shutdown()
{
    m_decoder.reset();
    m_cache.clear();
    m_lru.clear();
}

qint64 MmtsDecodeWorker::frameIndexOf(qint64 ms) const
{
    const double fps = m_decoder ? m_decoder->frameRate() : 0.0;
    if (fps > 0.0)
        return llround(ms * fps / 1000.0);
    return ms; // ms-granularity fallback before fps is known
}

bool MmtsDecodeWorker::cacheGet(qint64 index, QImage& image, qint64& ms)
{
    auto it = m_cache.find(index);
    if (it == m_cache.end())
        return false;
    image = it.value().first;
    ms = it.value().second;
    m_lru.removeOne(index);
    m_lru.append(index); // mark most-recently-used
    return true;
}

void MmtsDecodeWorker::cachePut(qint64 index, const QImage& image, qint64 ms)
{
    if (m_cache.contains(index))
        m_lru.removeOne(index);
    m_cache.insert(index, qMakePair(image, ms));
    m_lru.append(index);
    while (m_lru.size() > m_cacheCapacity) {
        const qint64 evict = m_lru.takeFirst();
        m_cache.remove(evict);
    }
}

void MmtsDecodeWorker::processPending()
{
    Request req;
    {
        QMutexLocker lock(&m_mutex);
        if (!m_havePending)
            return; // already consumed by an earlier processPending (coalesced)
        req = m_pending;
        m_havePending = false;
    }
    if (!m_decoder || !m_decoder->isOpen())
        return;

    qint64 targetIndex = (req.kind == Kind::Step)
        ? m_curIndex + req.delta
        : frameIndexOf(req.relMs);
    if (targetIndex < 0)
        targetIndex = 0;

    QImage image;
    qint64 ms = 0;
    if (cacheGet(targetIndex, image, ms)) {
        m_curIndex = targetIndex;
        m_curMs = ms;
        m_decoder->resync(ms); // keep the decoder consistent for the next decode
        emit frameReady(image, ms, req.id);
        return;
    }

    qint64 reqMs;
    if (req.kind == Kind::Step) {
        // Map the target frame index back to time, biased just below the frame's
        // PTS so frameAtMs() lands on exactly that frame (it returns >= target).
        reqMs = frameTimeOf(targetIndex) - 1;
    } else {
        reqMs = req.relMs;
    }
    if (reqMs < 0)
        reqMs = 0;

    qint64 actual = reqMs;
    image = m_decoder->frameAtMs(reqMs, &actual);
    if (image.isNull())
        return;

    m_curMs = actual;
    m_curIndex = frameIndexOf(actual);
    cachePut(m_curIndex, image, actual);
    emit frameReady(image, actual, req.id);
}

// ---------------------------------------------------------------------------
// Controller (UI thread)
// ---------------------------------------------------------------------------
MmtsDecodeController::MmtsDecodeController(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<MmtsMap>("MmtsMap");

    m_worker = new MmtsDecodeWorker();
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &MmtsDecodeWorker::opened, this, &MmtsDecodeController::opened);
    connect(m_worker, &MmtsDecodeWorker::frameReady, this, &MmtsDecodeController::frameReady);
    m_thread.start();
}

MmtsDecodeController::~MmtsDecodeController()
{
    QMetaObject::invokeMethod(m_worker, "shutdown", Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
}

void MmtsDecodeController::openMedia(const QString& path, const MmtsMap& map)
{
    QMetaObject::invokeMethod(m_worker, "open", Qt::QueuedConnection,
                              Q_ARG(QString, path), Q_ARG(MmtsMap, map));
}

quint64 MmtsDecodeController::requestFrameAt(qint64 relMs)
{
    const quint64 id = ++m_seq;
    m_worker->setPending({MmtsDecodeWorker::Kind::Abs, relMs, 0, id});
    QMetaObject::invokeMethod(m_worker, "processPending", Qt::QueuedConnection);
    return id;
}

quint64 MmtsDecodeController::requestStep(int delta)
{
    const quint64 id = ++m_seq;
    m_worker->setPending({MmtsDecodeWorker::Kind::Step, 0, delta, id});
    QMetaObject::invokeMethod(m_worker, "processPending", Qt::QueuedConnection);
    return id;
}
