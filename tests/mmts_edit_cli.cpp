// mmts-edit-cli: headless command-line counterpart to mmts-edit-gui.
//
// Reuses the same MmtsMap / MmtsSmartExporter / MmtsTsExporter the GUI's
// Export menu calls, so a project can be cut interactively in the GUI
// (frame-accurate preview/scrubbing) and exported headlessly afterwards --
// useful for batch jobs, or just because an 8K recording is too large to
// comfortably keep re-opening in the GUI.
//
// Usage:
//   mmts-edit-cli --edit <project.mmtsedit> --output <out.ts|out.mmts> [options]
//   mmts-edit-cli --input <src.mmts> --output <out.ts|out.mmts> --cut <start>-<end> [--cut ...] [options]
//
// The output format is chosen from --output's extension: ".ts" exports a
// contiguous MPEG-2 TS via MmtsTsExporter (needs a tsduck-enabled build);
// ".mmts" exports a frame-accurate MMT/TLV file via MmtsSmartExporter.
//
// Times for --cut accept plain milliseconds or [HH:]MM:SS[.mmm] timecodes.
//
// Options:
//   --input <path>       Source .mmts (overrides the .mmtsedit's stored source)
//   --map <path>         .mmtsmap sidecar (default: <source>map); built automatically if missing
//   --no-frame-accurate  Allow falling back to RAP boundaries instead of failing when a
//                        cut needs a boundary re-encode the build/GPU can't do
//   --quiet              Suppress progress output
#include "MmtsCutPlan.h"
#include "MmtsEdit.h"
#include "MmtsMap.h"
#include "MmtsMapBuilder.h"
#include "MmtsSmartExporter.h"
#include "HevcReencoder.h"
#if MMTS_ENABLE_TSDUCK
#include "MmtsTsExporter.h"
#endif

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

void PrintUsage()
{
    std::fprintf(stderr,
        "mmts-edit-cli: headless MMTS exporter (CLI counterpart to mmts-edit-gui)\n\n"
        "Usage:\n"
        "  mmts-edit-cli --edit <project.mmtsedit> --output <out.ts|out.mmts> [options]\n"
        "  mmts-edit-cli --input <src.mmts> --output <out.ts|out.mmts> --cut <start>-<end> [--cut ...] [options]\n\n"
        "Output format is chosen by --output's extension:\n"
#if MMTS_ENABLE_TSDUCK
        "  .ts    contiguous MPEG-2 TS (re-multiplexed, frame-accurate cuts)\n"
#else
        "  .ts    NOT available in this build (needs tsduck)\n"
#endif
        "  .mmts  frame-accurate MMT/TLV (transparent copy + boundary splice)\n\n"
        "Times accept plain milliseconds or [HH:]MM:SS[.mmm] timecodes.\n\n"
        "Options:\n"
        "  --input <path>       Source .mmts (overrides the .mmtsedit's stored source)\n"
        "  --map <path>         .mmtsmap sidecar (default: <source>map); built automatically if missing\n"
        "  --no-frame-accurate  Allow falling back to RAP boundaries instead of failing when a\n"
        "                       cut needs a boundary re-encode the build/GPU can't do\n"
        "  --quiet              Suppress progress output\n");
}

bool ParseTimeMs(const QString& raw, qint64& outMs)
{
    const QString s = raw.trimmed();
    if (s.isEmpty())
        return false;

    if (!s.contains(QLatin1Char(':'))) {
        bool ok = false;
        const qint64 ms = s.toLongLong(&ok);
        if (!ok || ms < 0)
            return false;
        outMs = ms;
        return true;
    }

    // [HH:]MM:SS[.mmm]
    const QStringList parts = s.split(QLatin1Char(':'));
    if (parts.size() != 2 && parts.size() != 3)
        return false;

    qint64 hours = 0, minutes = 0;
    bool ok = true;
    if (parts.size() == 3) {
        hours = parts[0].toLongLong(&ok);
        if (ok)
            minutes = parts[1].toLongLong(&ok);
    } else {
        minutes = parts[0].toLongLong(&ok);
    }
    bool secOk = false;
    const double seconds = parts.last().toDouble(&secOk);
    if (!ok || !secOk || hours < 0 || minutes < 0 || seconds < 0)
        return false;

    outMs = (hours * 3600 + minutes * 60) * 1000 + static_cast<qint64>(std::llround(seconds * 1000.0));
    return true;
}

bool ParseCutRange(const QString& s, EditSegment& seg)
{
    const int dash = s.indexOf(QLatin1Char('-'));
    if (dash < 0)
        return false;
    qint64 startMs = 0, endMs = 0;
    if (!ParseTimeMs(s.left(dash), startMs) || !ParseTimeMs(s.mid(dash + 1), endMs))
        return false;
    if (endMs <= startMs)
        return false;
    seg.startMs = startMs;
    seg.endMs = endMs;
    return true;
}

// Prints "<label>: NN%" in place; never cancels.
std::function<bool(qint64, qint64)> MakeProgressPrinter(bool quiet, const char* label)
{
    return [quiet, label](qint64 done, qint64 total) {
        if (!quiet && total > 0) {
            std::fprintf(stderr, "\r%s: %3lld%%", label, (long long)(done * 100 / total));
            std::fflush(stderr);
        }
        return true;
    };
}

constexpr auto kSmartRenderEncoder = "hevc_nvenc";

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();

    QString editPath, inputPath, mapPath, outputPath;
    QVector<EditSegment> cutSegs;
    bool requireFrameAccurate = true;
    bool quiet = false;

    for (int i = 1; i < args.size(); i++) {
        const QString& a = args[i];
        auto nextArg = [&]() -> QString { return (i + 1 < args.size()) ? args[++i] : QString(); };

        if (a == QLatin1String("--edit")) {
            editPath = nextArg();
        } else if (a == QLatin1String("--input")) {
            inputPath = nextArg();
        } else if (a == QLatin1String("--map")) {
            mapPath = nextArg();
        } else if (a == QLatin1String("--output")) {
            outputPath = nextArg();
        } else if (a == QLatin1String("--cut")) {
            const QString v = nextArg();
            EditSegment seg;
            if (!ParseCutRange(v, seg)) {
                std::fprintf(stderr, "error: bad --cut range '%s'\n", v.toLocal8Bit().constData());
                return 2;
            }
            cutSegs.push_back(seg);
        } else if (a == QLatin1String("--no-frame-accurate")) {
            requireFrameAccurate = false;
        } else if (a == QLatin1String("--quiet")) {
            quiet = true;
        } else if (a == QLatin1String("--help") || a == QLatin1String("-h")) {
            PrintUsage();
            return 0;
        } else {
            std::fprintf(stderr, "error: unknown argument '%s'\n", a.toLocal8Bit().constData());
            PrintUsage();
            return 2;
        }
    }

    if (outputPath.isEmpty()) {
        std::fprintf(stderr, "error: --output is required\n");
        PrintUsage();
        return 2;
    }
    const bool wantTs = outputPath.endsWith(QLatin1String(".ts"), Qt::CaseInsensitive);
    const bool wantMmts = outputPath.endsWith(QLatin1String(".mmts"), Qt::CaseInsensitive);
    if (!wantTs && !wantMmts) {
        std::fprintf(stderr, "error: --output must end in .ts or .mmts\n");
        return 2;
    }
#if !MMTS_ENABLE_TSDUCK
    if (wantTs) {
        std::fprintf(stderr,
            "error: TS export is not available in this build.\n"
            "Use a .mmts --output, or the Windows release to export MPEG-2 TS files.\n");
        return 2;
    }
#endif

    if (editPath.isEmpty() && cutSegs.isEmpty()) {
        std::fprintf(stderr, "error: no segments to export (use --edit or --cut)\n");
        PrintUsage();
        return 2;
    }

    QVector<EditSegment> segments;
    QString srcPath;

    if (!editPath.isEmpty()) {
        MmtsEdit edit;
        QString err;
        if (!MmtsEdit::load(editPath, edit, &err)) {
            std::fprintf(stderr, "error: failed to load '%s': %s\n",
                         editPath.toLocal8Bit().constData(), err.toLocal8Bit().constData());
            return 1;
        }
        segments = edit.segments;
        if (!cutSegs.isEmpty())
            std::fprintf(stderr, "note: --cut ignored, using segments from --edit\n");

        srcPath = edit.source;
        if (!srcPath.isEmpty() && QFileInfo(srcPath).isRelative())
            srcPath = QFileInfo(editPath).dir().absoluteFilePath(srcPath);

        if (mapPath.isEmpty()) {
            mapPath = edit.map;
            if (mapPath.isEmpty()) {
                mapPath = editPath;
                if (mapPath.endsWith(QLatin1String("edit"), Qt::CaseInsensitive))
                    mapPath.chop(4); // sample.mmtsedit -> sample.mmts
                mapPath += QLatin1String("map"); // -> sample.mmtsmap
            }
            if (QFileInfo(mapPath).isRelative())
                mapPath = QFileInfo(editPath).dir().absoluteFilePath(mapPath);
        }

        if (srcPath.isEmpty() || !QFileInfo::exists(srcPath)) {
            QString derived = mapPath;
            if (derived.endsWith(QLatin1String("map"), Qt::CaseInsensitive))
                derived.chop(3); // sample.mmtsmap -> sample.mmts
            if (QFileInfo::exists(derived))
                srcPath = derived;
        }
    } else {
        segments = cutSegs;
    }

    if (!inputPath.isEmpty())
        srcPath = inputPath;

    if (srcPath.isEmpty()) {
        std::fprintf(stderr, "error: no source .mmts (use --input, or --edit pointing at a project with a source)\n");
        return 2;
    }
    if (!QFileInfo::exists(srcPath)) {
        std::fprintf(stderr, "error: source not found: %s\n", srcPath.toLocal8Bit().constData());
        return 1;
    }
    if (mapPath.isEmpty())
        mapPath = srcPath + QLatin1String("map"); // sample.mmts -> sample.mmtsmap

    if (wantMmts && QFileInfo(outputPath).exists() &&
        QFileInfo(outputPath).canonicalFilePath() == QFileInfo(srcPath).canonicalFilePath()) {
        std::fprintf(stderr, "error: --output must differ from the source\n");
        return 2;
    }

    MmtsMap map;
    QString err;
    if (!QFileInfo::exists(mapPath)) {
        if (!quiet)
            std::fprintf(stderr, "No .mmtsmap sidecar found; scanning %s to build %s ...\n",
                         srcPath.toLocal8Bit().constData(), mapPath.toLocal8Bit().constData());
        if (!MmtsMapBuilder::build(srcPath, mapPath, &err, MakeProgressPrinter(quiet, "map"))) {
            if (!quiet)
                std::fprintf(stderr, "\n");
            std::fprintf(stderr, "error: map build failed: %s\n", err.toLocal8Bit().constData());
            return 1;
        }
        if (!quiet)
            std::fprintf(stderr, "\n");
    }
    if (!MmtsMapReader::load(mapPath, map, &err)) {
        std::fprintf(stderr, "error: failed to load '%s': %s\n",
                     mapPath.toLocal8Bit().constData(), err.toLocal8Bit().constData());
        return 1;
    }

    if (segments.isEmpty()) {
        std::fprintf(stderr, "error: edit timeline is empty; nothing to export\n");
        return 1;
    }

    const qint64 srcSize = QFileInfo(srcPath).size();
    int smartRenderSegments = 0;

    if (wantMmts) {
        const auto plan = planMmtsCutSegments(map, segments, srcSize);
        if (plan.isEmpty()) {
            std::fprintf(stderr, "error: could not map any segment to RAP byte ranges\n");
            return 1;
        }
        for (const auto& seg : plan) {
            if ((seg.copyStartMs - seg.inAbsMs) > kMmtsCutRapThresholdMs ||
                (seg.outAbsMs - seg.copyEndMs) > kMmtsCutRapThresholdMs) {
                ++smartRenderSegments;
            }
        }
    }
#if MMTS_ENABLE_TSDUCK
    else {
        const auto plan = MmtsTsExporter::planSegments(map, segments, srcSize);
        if (plan.isEmpty()) {
            std::fprintf(stderr, "error: could not map any segment to RAP byte ranges\n");
            return 1;
        }
        constexpr qint64 kThresholdMs = 20;
        for (const auto& seg : plan) {
            if ((seg.copyStartMs - seg.inAbsMs) > kThresholdMs ||
                (seg.outAbsMs - seg.copyEndMs) > kThresholdMs) {
                ++smartRenderSegments;
            }
        }
    }
#endif

    if (!quiet) {
        std::fprintf(stderr, "map: tracks=%d rap=%d duration=%lld ms\n",
                     map.tracks.size(), map.rapPoints.size(), (long long)map.durationMs);
        for (int i = 0; i < segments.size(); i++) {
            std::fprintf(stderr, "  segment %d: %lld..%lld ms (%.1f s)\n", i,
                         (long long)segments[i].startMs, (long long)segments[i].endMs,
                         segments[i].durationMs() / 1000.0);
        }
        if (smartRenderSegments > 0)
            std::fprintf(stderr, "%d segment(s) need a boundary re-encode for frame-accurate cuts\n",
                         smartRenderSegments);
    }

    if (smartRenderSegments > 0 && requireFrameAccurate &&
        !HevcReencoder::encoderAvailable(QString::fromLatin1(kSmartRenderEncoder))) {
        std::fprintf(stderr,
            "error: %d segment(s) need frame-accurate re-encoding (%s) for non-RAP-aligned cuts, "
            "but this ffmpeg build does not expose that encoder.\n"
            "Re-run with --no-frame-accurate to fall back to RAP boundaries for those cuts.\n",
            smartRenderSegments, kSmartRenderEncoder);
        return 1;
    }

    if (!quiet)
        std::fprintf(stderr, "Exporting...\n");

    QStringList warnings;
    bool ok;
    if (wantMmts) {
        ok = MmtsSmartExporter::exportMmts(srcPath, outputPath, map, segments, srcSize, &err,
                                           MakeProgressPrinter(quiet, "export"), &warnings, requireFrameAccurate);
    } else {
#if MMTS_ENABLE_TSDUCK
        ok = MmtsTsExporter::exportTs(srcPath, outputPath, map, segments, srcSize, &err,
                                      MakeProgressPrinter(quiet, "export"), &warnings, requireFrameAccurate);
#else
        ok = false; // unreachable: rejected above when tsduck is unavailable
#endif
    }
    if (!ok) {
        if (!quiet)
            std::fprintf(stderr, "\n");
        std::fprintf(stderr, "error: export failed: %s\n", err.toLocal8Bit().constData());
        return 1;
    }
    if (!quiet)
        std::fprintf(stderr, "\n");
    for (const QString& w : warnings)
        std::fprintf(stderr, "warning: %s\n", w.toLocal8Bit().constData());
    std::fprintf(stderr, "done: %s (%lld bytes)\n", outputPath.toLocal8Bit().constData(),
                 (long long)QFileInfo(outputPath).size());
    return 0;
}
