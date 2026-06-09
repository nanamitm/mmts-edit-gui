// Headless test for MmtsMapBuilder: generate a .mmtsmap and validate it.
// Usage: mmts-mapbuild-test <src.mmts> <out.mmtsmap>
#include "MmtsMap.h"
#include "MmtsMapBuilder.h"

#include <QCoreApplication>
#include <cstdio>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "Usage: mmts-mapbuild-test <src.mmts> <out.mmtsmap>\n");
        return 2;
    }
    const QString src = QString::fromLocal8Bit(argv[1]);
    const QString out = QString::fromLocal8Bit(argv[2]);

    QString err;
    if (!MmtsMapBuilder::build(src, out, &err)) {
        std::fprintf(stderr, "build failed: %s\n", err.toLocal8Bit().constData());
        return 1;
    }

    MmtsMap map;
    if (!MmtsMapReader::load(out, map, &err)) {
        std::fprintf(stderr, "generated map does not parse: %s\n", err.toLocal8Bit().constData());
        return 1;
    }

    std::fprintf(stderr, "map: format=%s duration=%lld ms firstPts=%lld tracks=%d mpt=%d rap=%d seek=%d\n",
                 map.format.toLocal8Bit().constData(), (long long)map.durationMs,
                 (long long)map.firstVideoPtsMs, (int)map.tracks.size(),
                 (int)map.mptChanges.size(), (int)map.rapPoints.size(), (int)map.seekPoints.size());

    const bool ok = map.format == "MMTSMAP2" && map.durationMs > 0 && !map.rapPoints.isEmpty();
    std::fprintf(stderr, "valid=%d\n", ok ? 1 : 0);
    return ok ? 0 : 1;
}
