// Headless smoke test for MmtsVideoDecoder.
// Usage: mmts-decode-test <file.mmts> <relativeMs> <out.png>
#include "MmtsMap.h"
#include "MmtsVideoDecoder.h"

#include <QGuiApplication>
#include <QImage>
#include <cstdio>

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (argc < 4) {
        std::fprintf(stderr, "Usage: mmts-decode-test <file.mmts> <relativeMs> <out.png>\n");
        return 2;
    }
    const QString mmts = QString::fromLocal8Bit(argv[1]);
    const qint64 ms = QString::fromLocal8Bit(argv[2]).toLongLong();
    const QString out = QString::fromLocal8Bit(argv[3]);

    const QString mapPath = mmts + "map";
    MmtsMap map;
    QString err;
    if (!MmtsMapReader::load(mapPath, map, &err)) {
        std::fprintf(stderr, "map load failed: %s\n", err.toLocal8Bit().constData());
        return 1;
    }
    std::fprintf(stderr, "map: format=%s duration=%lld ms firstPts=%lld raps=%d\n",
                 map.format.toLocal8Bit().constData(), (long long)map.durationMs,
                 (long long)map.firstVideoPtsMs, (int)map.rapPoints.size());

    MmtsVideoDecoder dec;
    dec.setPreviewMaxWidth(960);
    if (!dec.open(mmts, map, &err)) {
        std::fprintf(stderr, "decoder open failed: %s\n", err.toLocal8Bit().constData());
        return 1;
    }

    // argv[2] may be a comma-separated list of relative ms to decode in sequence
    // (tests the decoder's continue-forward / re-seek behaviour with one instance).
    const QStringList parts = QString::fromLocal8Bit(argv[2]).split(',', Qt::SkipEmptyParts);
    QImage img;
    int i = 0;
    for (const QString& part : parts) {
        const qint64 reqMs = part.toLongLong();
        qint64 actual = -1;
        img = dec.frameAtMs(reqMs, &actual);
        if (img.isNull()) {
            std::fprintf(stderr, "[%d] decode null at req %lld\n", i, (long long)reqMs);
            return 1;
        }
        std::fprintf(stderr, "[%d] req %lld -> %dx%d at %lld ms, fps=%.3f\n",
                     i, (long long)reqMs, img.width(), img.height(), (long long)actual, dec.frameRate());
        ++i;
    }
    if (!img.save(out)) {
        std::fprintf(stderr, "save failed: %s\n", out.toLocal8Bit().constData());
        return 1;
    }
    std::fprintf(stderr, "saved %s\n", out.toLocal8Bit().constData());
    return 0;
}
