#include "FilePathSafety.h"
#include "MmtsExporter.h"

#include <QFile>
#include <algorithm>
#include <vector>

namespace {
// Offset of the first RAP whose (absolute) time is >= t; `fallback` if none.
qint64 firstRapOffsetAtOrAfter(const QVector<MmtsPoint>& raps, qint64 t, qint64 fallback)
{
    for (const auto& p : raps) {
        if (p.timeMs >= t)
            return static_cast<qint64>(p.offset);
    }
    return fallback;
}
} // namespace

QVector<MmtsExporter::Range> MmtsExporter::planRanges(const MmtsMap& map,
                                                      const QVector<EditSegment>& segments,
                                                      qint64 sourceSizeBytes)
{
    QVector<MmtsPoint> raps = map.rapPoints;
    std::sort(raps.begin(), raps.end(),
              [](const MmtsPoint& a, const MmtsPoint& b) { return a.timeMs < b.timeMs; });

    const qint64 firstPts = map.firstVideoPtsMs > 0 ? map.firstVideoPtsMs : 0;

    QVector<Range> ranges;
    for (const auto& seg : segments) {
        const qint64 absStart = seg.startMs + firstPts;
        const qint64 absEnd = seg.endMs + firstPts;
        Range r;
        r.srcStart = firstRapOffsetAtOrAfter(raps, absStart, 0);
        r.srcEnd = firstRapOffsetAtOrAfter(raps, absEnd, sourceSizeBytes);
        // Clamp to the file and drop empties.
        r.srcStart = std::clamp<qint64>(r.srcStart, 0, sourceSizeBytes);
        r.srcEnd = std::clamp<qint64>(r.srcEnd, 0, sourceSizeBytes);
        if (r.srcEnd > r.srcStart)
            ranges.push_back(r);
    }
    return ranges;
}

bool MmtsExporter::copyRanges(const QString& srcPath, const QString& dstPath,
                              const QVector<Range>& ranges, QString* errorMessage,
                              const std::function<bool(qint64, qint64)>& progress)
{
    if (ranges.isEmpty()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Nothing to export (no segments).");
        return false;
    }

    if (!distinctOutput(srcPath, dstPath, errorMessage)) return false;
    QFile in(srcPath);
    if (!in.open(QIODevice::ReadOnly)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Cannot open source: %1").arg(in.errorString());
        return false;
    }
    QFile out(dstPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Cannot create output: %1").arg(out.errorString());
        return false;
    }

    qint64 total = 0;
    for (const auto& r : ranges)
        total += r.length();

    constexpr qint64 kChunk = 4 * 1024 * 1024;
    QByteArray buf;
    buf.resize(static_cast<int>(kChunk));
    qint64 done = 0;

    for (const auto& r : ranges) {
        if (!in.seek(r.srcStart)) {
            if (errorMessage)
                *errorMessage = QStringLiteral("Seek failed at %1").arg(r.srcStart);
            return false;
        }
        qint64 remaining = r.length();
        while (remaining > 0) {
            const qint64 want = std::min(kChunk, remaining);
            const qint64 got = in.read(buf.data(), want);
            if (got <= 0) {
                if (errorMessage)
                    *errorMessage = QStringLiteral("Read failed (%1 bytes left)").arg(remaining);
                return false;
            }
            if (out.write(buf.constData(), got) != got) {
                if (errorMessage)
                    *errorMessage = QStringLiteral("Write failed: %1").arg(out.errorString());
                return false;
            }
            remaining -= got;
            done += got;
            if (progress && !progress(done, total)) {
                if (errorMessage)
                    *errorMessage = QStringLiteral("Cancelled");
                out.close();
                out.remove();
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
void MmtsExportWorker::configure(const QString& srcPath, const QString& dstPath,
                                 const QVector<MmtsExporter::Range>& ranges)
{
    m_srcPath = srcPath;
    m_dstPath = dstPath;
    m_ranges = ranges;
    m_cancel.store(false);
}

void MmtsExportWorker::run()
{
    QString error;
    const bool ok = MmtsExporter::copyRanges(
        m_srcPath, m_dstPath, m_ranges, &error,
        [this](qint64 done, qint64 total) {
            emit progress(done, total);
            return !m_cancel.load();
        });
    emit finished(ok, error);
}
