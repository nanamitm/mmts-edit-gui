// Headless test for HevcReencoder: re-encode a frame range to a raw .hevc.
// Usage: mmts-reencode-test <src.mmts> <startMs> <endMs> <out.hevc> [encoder]
#include "HevcReencoder.h"
#include "MmtsMap.h"

#include <QCoreApplication>
#include <QFile>
#include <cstdio>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 5) {
        std::fprintf(stderr, "Usage: mmts-reencode-test <src.mmts> <startMs> <endMs> <out.hevc> [encoder]\n");
        return 2;
    }
    const QString src = QString::fromLocal8Bit(argv[1]);
    const qint64 startMs = QString::fromLocal8Bit(argv[2]).toLongLong();
    const qint64 endMs = QString::fromLocal8Bit(argv[3]).toLongLong();
    const QString out = QString::fromLocal8Bit(argv[4]);

    MmtsMap map;
    QString err;
    if (!MmtsMapReader::load(src + "map", map, &err)) {
        std::fprintf(stderr, "map load failed: %s\n", err.toLocal8Bit().constData());
        return 1;
    }

    HevcReencoder re;
    if (argc >= 6)
        re.setEncoderName(QString::fromLocal8Bit(argv[5]));

    std::vector<HevcReencoder::Au> aus;
    if (!re.reencode(src, map, startMs, endMs, aus, &err)) {
        std::fprintf(stderr, "reencode failed: %s\n", err.toLocal8Bit().constData());
        return 1;
    }

    QFile f(out);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::fprintf(stderr, "cannot write %s\n", out.toLocal8Bit().constData());
        return 1;
    }
    qint64 total = 0;
    qint64 minPts = -1, maxPts = -1;
    for (const auto& au : aus) {
        f.write(reinterpret_cast<const char*>(au.data.data()), static_cast<qint64>(au.data.size()));
        total += static_cast<qint64>(au.data.size());
        if (minPts < 0 || au.ptsMs < minPts) minPts = au.ptsMs;
        if (au.ptsMs > maxPts) maxPts = au.ptsMs;
    }
    f.close();

    std::fprintf(stderr, "aus=%zu bytes=%lld firstKeyframe=%d ptsMs=[%lld..%lld]\n",
                 aus.size(), (long long)total, aus.empty() ? 0 : (aus.front().keyframe ? 1 : 0),
                 (long long)minPts, (long long)maxPts);
    return aus.empty() ? 1 : 0;
}
