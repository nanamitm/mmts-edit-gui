// Headless test for MmtsTsExporter (EDL -> contiguous MPEG-2 TS).
// Usage: mmts-ts-export-test <src.mmts> <out.ts>
#include "MmtsEdit.h"
#include "MmtsMap.h"
#include "MmtsTsExporter.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <cstdio>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "Usage: mmts-ts-export-test <src.mmts> <out.ts>\n");
        return 2;
    }
    const QString src = QString::fromLocal8Bit(argv[1]);
    const QString out = QString::fromLocal8Bit(argv[2]);

    MmtsMap map;
    QString err;
    if (!MmtsMapReader::load(src + "map", map, &err)) {
        std::fprintf(stderr, "map load failed: %s\n", err.toLocal8Bit().constData());
        return 1;
    }
    const qint64 srcSize = QFileInfo(src).size();

    QVector<EditSegment> segs = { {60000, 120000}, {300000, 360000} };
    const auto plan = MmtsTsExporter::planSegments(map, segs, srcSize);
    for (int i = 0; i < plan.size(); ++i) {
        std::fprintf(stderr, "seg[%d] byte %lld..%lld  time %lld..%lld ms\n", i,
                     (long long)plan[i].startByte, (long long)plan[i].endByte,
                     (long long)plan[i].startMs, (long long)plan[i].endMs);
    }

    if (!MmtsTsExporter::exportTs(src, out, map, segs, srcSize, &err)) {
        std::fprintf(stderr, "export failed: %s\n", err.toLocal8Bit().constData());
        return 1;
    }

    const qint64 outSize = QFileInfo(out).size();
    QFile f(out);
    unsigned char first = 0;
    if (f.open(QIODevice::ReadOnly)) { char c = 0; f.read(&c, 1); first = static_cast<unsigned char>(c); }
    const bool tsSync = (first == 0x47);
    std::fprintf(stderr, "out=%lld bytes firstByte=0x%02X tsSyncOk=%d (188-aligned=%d)\n",
                 (long long)outSize, first, tsSync ? 1 : 0, (outSize % 188 == 0) ? 1 : 0);
    return (outSize > 0 && tsSync) ? 0 : 1;
}
