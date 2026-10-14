#include "MmtsDecodeController.h"

#include "MmtsVideoDecoder.h"

#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// Main decode worker
// ---------------------------------------------------------------------------
MmtsDecodeWorker::MmtsDecodeWorker() = default;
MmtsDecodeWorker::~MmtsDecodeWorker() = default;

void MmtsDecodeWorker::setPending(const Request& req)
{
    QMutexLocker lock(&m_mutex);
    m_pending = req;
    m_havePending = true;
}

void MmtsDecodeWorker::open(const QString& path, const MmtsMap& map, quint64 generation)
{
    m_decoder = std::make_unique<MmtsVideoDecoder>();
    m_generation = generation;
    m_decoder->setPreviewMaxWidth(960);
    // Cache frames decoded on the way to a step target so stepping back through
    // an already-decoded GOP is instant (enabled per-request in processPending).
    m_decoder->setFrameSink([this](qint64 ms, const QImage& img) {
        if (m_cache)
            m_cache->put(frameIndexOf(ms), img, ms);
    });

    m_curMs = 0;
    m_curIndex = 0;
    {
        QMutexLocker lock(&m_mutex);
        m_havePending = false;
    }

    QString error;
    if (!m_decoder->open(path, map, &error)) {
        m_decoder.reset();
        emit opened(false, 0, 0.0, error, m_generation);
        return;
    }

    // Prime: decode the first frame so the frame rate (from the HEVC SPS/VUI) is
    // known before we report 'opened'.
    qint64 actual = 0;
    const QImage first = m_decoder->frameAtMs(0, &actual);
    if (!first.isNull()) {
        m_curMs = actual;
        m_curIndex = frameIndexOf(actual);
        if (m_cache)
            m_cache->put(m_curIndex, first, actual);
    }
    emit opened(true, m_decoder->durationMs(), m_decoder->frameRate(), QString(), m_generation);
}

void MmtsDecodeWorker::shutdown()
{
    m_decoder.reset();
}

qint64 MmtsDecodeWorker::frameIndexOf(qint64 ms) const
{
    const double fps = m_decoder ? m_decoder->frameRate() : 0.0;
    return fps > 0.0 ? llround(ms * fps / 1000.0) : ms;
}

qint64 MmtsDecodeWorker::frameTimeOf(qint64 index) const
{
    const double fps = m_decoder ? m_decoder->frameRate() : 0.0;
    return fps > 0.0 ? llround(index * 1000.0 / fps) : index;
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

    qint64 targetIndex = (req.kind == Kind::Step) ? m_curIndex + req.delta : frameIndexOf(req.relMs);
    if (targetIndex < 0)
        targetIndex = 0;

    QImage image;
    qint64 ms = 0;
    if (m_cache && m_cache->get(targetIndex, image, ms)) {
        m_curIndex = targetIndex;
        m_curMs = ms;
        m_decoder->resync(ms); // keep the decoder consistent for the next decode
        emit frameReady(image, ms, req.id, m_generation);
        return;
    }

    qint64 reqMs = (req.kind == Kind::Step) ? frameTimeOf(targetIndex) - 1 : req.relMs;
    if (reqMs < 0)
        reqMs = 0;

    // For steps, cache the frames decoded on the way to the target so stepping
    // back through this GOP is instant. Skip for slider seeks (keep them light).
    m_decoder->setEmitIntermediate(req.kind == Kind::Step);

    qint64 actual = reqMs;
    image = m_decoder->frameAtMs(reqMs, &actual);
    if (image.isNull())
        return;

    m_curMs = actual;
    m_curIndex = frameIndexOf(actual);
    if (m_cache)
        m_cache->put(m_curIndex, image, actual);
    emit frameReady(image, actual, req.id, m_generation);
}

// ---------------------------------------------------------------------------
// Background prefetch worker
// ---------------------------------------------------------------------------
MmtsPrefetchWorker::MmtsPrefetchWorker() = default;
MmtsPrefetchWorker::~MmtsPrefetchWorker() = default;

void MmtsPrefetchWorker::setPending(qint64 currentRelMs)
{
    QMutexLocker lock(&m_mutex);
    m_pendingRel = currentRelMs;
    m_havePending = true;
}

void MmtsPrefetchWorker::open(const QString& path, const MmtsMap& map)
{
    m_decoder = std::make_unique<MmtsVideoDecoder>();
    m_decoder->setPreviewMaxWidth(960); // must match the main decoder's frame size
    m_decoder->setEmitIntermediate(true);
    m_decoder->setFrameSink([this](qint64 ms, const QImage& img) {
        if (m_cache)
            m_cache->put(frameIndexOf(ms), img, ms);
    });

    // RAP times in relative ms, sorted -- used to find GOP boundaries.
    m_rapRel.clear();
    const qint64 firstPts = map.firstVideoPtsMs > 0 ? map.firstVideoPtsMs : 0;
    for (const auto& p : map.rapPoints) {
        const qint64 rel = p.timeMs - firstPts;
        if (rel >= 0)
            m_rapRel.push_back(rel);
    }
    std::sort(m_rapRel.begin(), m_rapRel.end());

    QString error;
    if (!m_decoder->open(path, map, &error)) {
        m_decoder.reset();
        return;
    }
    // Prime so the frame rate is known (cache indices must match the main worker).
    m_decoder->frameAtMs(0);
    {
        QMutexLocker lock(&m_mutex);
        m_havePending = false;
    }
}

void MmtsPrefetchWorker::shutdown()
{
    m_decoder.reset();
}

qint64 MmtsPrefetchWorker::frameIndexOf(qint64 ms) const
{
    const double fps = m_decoder ? m_decoder->frameRate() : 0.0;
    return fps > 0.0 ? llround(ms * fps / 1000.0) : ms;
}

void MmtsPrefetchWorker::prefetch()
{
    qint64 cur = -1;
    {
        QMutexLocker lock(&m_mutex);
        if (!m_havePending)
            return;
        cur = m_pendingRel;
        m_havePending = false;
    }
    if (!m_decoder || !m_decoder->isOpen() || m_rapRel.isEmpty() || !m_cache)
        return;

    // Start of the GOP containing the current position.
    qint64 curGopStart = -1;
    for (qint64 r : m_rapRel) {
        if (r <= cur)
            curGopStart = r;
        else
            break;
    }
    if (curGopStart < 0)
        return;

    // Fill the current GOP first (so backward stepping right after a slider seek,
    // which does not cache intermediates, becomes a cache hit), then the previous
    // GOP (for crossing the boundary). The cache holds the GOP-start frame as a
    // sentinel, so already-prefetched GOPs are skipped cheaply.
    if (!m_cache->contains(frameIndexOf(curGopStart)))
        m_decoder->frameAtMs(cur); // caches [curGopStart .. cur)

    if (curGopStart > 0) {
        const qint64 prevTarget = curGopStart - 1;
        if (!m_cache->contains(frameIndexOf(prevTarget)))
            m_decoder->frameAtMs(prevTarget); // caches the previous GOP
    }
}

// ---------------------------------------------------------------------------
// Controller (UI thread)
// ---------------------------------------------------------------------------
MmtsDecodeController::MmtsDecodeController(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<MmtsMap>("MmtsMap");

    m_worker = new MmtsDecodeWorker();
    m_worker->setCache(m_cache);
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &MmtsDecodeWorker::opened, this,
            [this](bool ok, qint64 duration, double fps, const QString& error, quint64 generation) {
        if (generation == m_generation) emit opened(ok, duration, fps, error);
    });
    connect(m_worker, &MmtsDecodeWorker::frameReady, this,
            [this](const QImage& image, qint64 ms, quint64 id, quint64 generation) {
        if (generation != m_generation) return;
        emit frameReady(image, ms, id);
        m_prefetch->setPending(ms);
        QMetaObject::invokeMethod(m_prefetch, "prefetch", Qt::QueuedConnection);
    });
    m_thread.start();

    m_prefetch = new MmtsPrefetchWorker();
    m_prefetch->setCache(m_cache);
    m_prefetch->moveToThread(&m_prefetchThread);
    connect(&m_prefetchThread, &QThread::finished, m_prefetch, &QObject::deleteLater);
    m_prefetchThread.start();
}

MmtsDecodeController::~MmtsDecodeController()
{
    QMetaObject::invokeMethod(m_worker, "shutdown", Qt::BlockingQueuedConnection);
    QMetaObject::invokeMethod(m_prefetch, "shutdown", Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_prefetchThread.quit();
    m_thread.wait();
    m_prefetchThread.wait();
}

void MmtsDecodeController::openMedia(const QString& path, const MmtsMap& map)
{
    const auto generation = ++m_generation;
    m_cache = std::make_shared<FrameCache>();
    const auto cache = m_cache;
    QMetaObject::invokeMethod(m_worker, [this, path, map, cache, generation] {
        m_worker->setCache(cache);
        m_worker->open(path, map, generation);
    }, Qt::QueuedConnection);
    QMetaObject::invokeMethod(m_prefetch, [this, path, map, cache] {
        m_prefetch->setCache(cache);
        m_prefetch->open(path, map);
    }, Qt::QueuedConnection);
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

void MmtsDecodeController::closeMedia()
{
    ++m_generation;
    QMetaObject::invokeMethod(m_worker, "shutdown", Qt::QueuedConnection);
    QMetaObject::invokeMethod(m_prefetch, "shutdown", Qt::QueuedConnection);
    m_cache = std::make_shared<FrameCache>();
}
