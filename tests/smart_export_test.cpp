// Headless test for MmtsSmartExporter (frame-accurate .mmts export).
// Usage: mmts-smart-export-test <src.mmts> <out.mmts> [inMs outMs ...]
//   Default EDL: two segments with non-RAP IN/OUT points so both the lead-in
//   and tail re-encodes are exercised. Prints warnings and basic output stats;
//   deeper validation (map rebuild + decode) is done by the caller.
#include "MmtsEdit.h"
#include "MmtsMap.h"
#include "MmtsSmartExporter.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <cstdio>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "Usage: mmts-smart-export-test <src.mmts> <out.mmts> [inMs outMs ...]\n");
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

    QVector<EditSegment> segs;
    if (argc >= 5 && ((argc - 3) % 2) == 0) {
        for (int i = 3; i + 1 < argc; i += 2)
            segs.push_back({ atoll(argv[i]), atoll(argv[i + 1]) });
    } else {
        // Non-RAP cut points: exercise lead-in and tail re-encode on both segments.
        segs = { { 60250, 90250 }, { 300250, 330250 } };
    }
    for (const auto& s : segs)
        std::fprintf(stderr, "segment %lld..%lld ms\n", (long long)s.startMs, (long long)s.endMs);

    QStringList warnings;
    qint64 lastPct = -1;
    const bool ok = MmtsSmartExporter::exportMmts(
        src, out, map, segs, srcSize, &err,
        [&](qint64 done, qint64 total) {
            if (total > 0) {
                const qint64 pct = done * 100 / total;
                if (pct != lastPct && pct % 10 == 0) {
                    std::fprintf(stderr, "progress %lld%%\n", (long long)pct);
                    lastPct = pct;
                }
            }
            return true;
        },
        &warnings,
        /*requireFrameAccurate=*/true);

    for (const auto& w : warnings)
        std::fprintf(stderr, "warning: %s\n", w.toLocal8Bit().constData());
    if (!ok) {
        std::fprintf(stderr, "export failed: %s\n", err.toLocal8Bit().constData());
        return 1;
    }

    const qint64 outSize = QFileInfo(out).size();
    QFile f(out);
    unsigned char first = 0;
    if (f.open(QIODevice::ReadOnly)) { char c = 0; f.read(&c, 1); first = static_cast<unsigned char>(c); }
    const bool tlvOk = (first == 0x7F);
    std::fprintf(stderr, "out=%lld bytes firstByte=0x%02X tlvSyncOk=%d warnings=%d\n",
                 (long long)outSize, first, tlvOk ? 1 : 0, (int)warnings.size());
    return tlvOk ? 0 : 1;
}
