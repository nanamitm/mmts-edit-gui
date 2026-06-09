// Headless test for the threaded decode controller + prefetch + frame cache.
// Usage: mmts-controller-test <file.mmts>
//
// Seeks to 60s, waits so the background prefetch can fill the current+previous
// GOP, then steps backward across the GOP boundary -- those should be cache
// hits (fast) thanks to prefetch.
#include "MmtsDecodeController.h"
#include "MmtsMap.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QTimer>
#include <cstdio>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "Usage: mmts-controller-test <file.mmts>\n");
        return 2;
    }
    const QString mmts = QString::fromLocal8Bit(argv[1]);

    MmtsMap map;
    QString err;
    if (!MmtsMapReader::load(mmts + "map", map, &err)) {
        std::fprintf(stderr, "map load failed: %s\n", err.toLocal8Bit().constData());
        return 1;
    }

    MmtsDecodeController ctrl;
    QElapsedTimer timer;
    int step = 0;
    constexpr int kBackSteps = 24;

    auto fireStep = [&]() {
        timer.restart();
        ctrl.requestStep(-1);
    };

    QObject::connect(&ctrl, &MmtsDecodeController::frameReady,
                     [&](const QImage& img, qint64 relMs, quint64) {
        const qint64 elapsed = timer.elapsed();
        if (step == 0) {
            std::fprintf(stderr, "seek -> %dx%d at %lld ms in %lld ms; waiting for prefetch...\n",
                         img.width(), img.height(), (long long)relMs, (long long)elapsed);
            ++step;
            QTimer::singleShot(3000, [&]() { fireStep(); }); // let prefetch fill GOPs
            return;
        }
        std::fprintf(stderr, "back[%d] -> %lld ms in %lld ms%s\n",
                     step, (long long)relMs, (long long)elapsed, elapsed < 30 ? "  (cache)" : "");
        ++step;
        if (step > kBackSteps) {
            app.quit();
            return;
        }
        QTimer::singleShot(40, [&]() { fireStep(); });
    });

    QObject::connect(&ctrl, &MmtsDecodeController::opened,
                     [&](bool ok, qint64, double fps, const QString& e) {
        if (!ok) { std::fprintf(stderr, "open failed: %s\n", e.toLocal8Bit().constData()); app.exit(1); return; }
        std::fprintf(stderr, "opened fps=%.3f\n", fps);
        timer.restart();
        ctrl.requestFrameAt(60000);
    });

    ctrl.openMedia(mmts, map);
    return app.exec();
}
