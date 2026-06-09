// Headless round-trip test for the MmtsEdit (EDL) model.
// Usage: mmts-edit-test <out.mmtsedit>
#include "MmtsEdit.h"

#include <QCoreApplication>
#include <QFile>
#include <cstdio>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString path = argc >= 2 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("roundtrip.mmtsedit");

    MmtsEdit edit;
    edit.version = 1;
    edit.source = "sample.mmts";
    edit.sourceSize = 123456789ULL;
    edit.map = "sample.mmtsmap";
    edit.segments = { {25000, 60000}, {90000, 120000}, {200000, 215500} };

    QString err;
    if (!edit.save(path, &err)) {
        std::fprintf(stderr, "save failed: %s\n", err.toLocal8Bit().constData());
        return 1;
    }

    MmtsEdit back;
    if (!MmtsEdit::load(path, back, &err)) {
        std::fprintf(stderr, "load failed: %s\n", err.toLocal8Bit().constData());
        return 1;
    }

    bool ok = back.segments.size() == edit.segments.size()
        && back.source == edit.source && back.sourceSize == edit.sourceSize
        && back.totalDurationMs() == edit.totalDurationMs();
    for (int i = 0; i < back.segments.size() && i < edit.segments.size(); ++i)
        ok = ok && back.segments[i].startMs == edit.segments[i].startMs
                && back.segments[i].endMs == edit.segments[i].endMs;

    std::fprintf(stderr, "segments=%d totalDuration=%lld ms roundtrip=%s\n",
                 (int)back.segments.size(), (long long)back.totalDurationMs(),
                 ok ? "OK" : "MISMATCH");

    QFile f(path);
    if (f.open(QIODevice::ReadOnly))
        std::fprintf(stderr, "--- %s ---\n%s\n", path.toLocal8Bit().constData(), f.readAll().constData());

    return ok ? 0 : 1;
}
