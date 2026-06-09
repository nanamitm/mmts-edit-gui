// Headless test for MmtsExporter (RAP-aligned .mmts copy export).
// Usage: mmts-export-test <src.mmts> <out.mmts>
//   Builds a 2-segment EDL from the source's own .mmtsmap, exports, and checks
//   the output size and that it begins on a TLV packet boundary (sync 0x7F).
#include "MmtsEdit.h"
#include "MmtsExporter.h"
#include "MmtsMap.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <cstdio>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "Usage: mmts-export-test <src.mmts> <out.mmts>\n");
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
    std::fprintf(stderr, "src=%lld bytes duration=%lld ms raps=%d firstPts=%lld\n",
                 (long long)srcSize, (long long)map.durationMs, (int)map.rapPoints.size(),
                 (long long)map.firstVideoPtsMs);

    // Two cut segments well inside the program.
    QVector<EditSegment> segs = { {60000, 120000}, {300000, 360000} };
    const auto ranges = MmtsExporter::planRanges(map, segs, srcSize);
    qint64 planned = 0;
    for (int i = 0; i < ranges.size(); ++i) {
        std::fprintf(stderr, "range[%d] %lld..%lld (%lld bytes)\n", i,
                     (long long)ranges[i].srcStart, (long long)ranges[i].srcEnd,
                     (long long)ranges[i].length());
        planned += ranges[i].length();
    }
    if (ranges.isEmpty()) {
        std::fprintf(stderr, "no ranges planned\n");
        return 1;
    }

    if (!MmtsExporter::copyRanges(src, out, ranges, &err)) {
        std::fprintf(stderr, "export failed: %s\n", err.toLocal8Bit().constData());
        return 1;
    }

    const qint64 outSize = QFileInfo(out).size();
    QFile f(out);
    unsigned char first = 0;
    if (f.open(QIODevice::ReadOnly)) { char c = 0; f.read(&c, 1); first = static_cast<unsigned char>(c); }

    const bool sizeOk = (outSize == planned);
    const bool tlvOk = (first == 0x7F); // ARIB TLV packet sync byte
    std::fprintf(stderr, "out=%lld bytes (planned=%lld) sizeOk=%d firstByte=0x%02X tlvSyncOk=%d\n",
                 (long long)outSize, (long long)planned, sizeOk ? 1 : 0, first, tlvOk ? 1 : 0);

    return (sizeOk && tlvOk) ? 0 : 1;
}
