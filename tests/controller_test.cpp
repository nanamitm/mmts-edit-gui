// Headless test for the threaded decode controller + frame cache.
// Usage: mmts-controller-test <file.mmts>
//
// Drives: seek to 60s, +1, +1, -1, -1. The backward steps revisit just-decoded
// frames and should be served from cache (visibly faster).
#include "MmtsDecodeController.h"
#include "MmtsMap.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <cstdio>
#include <vector>

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
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
    // Each entry: kind 0=abs(value ms), 1=step(delta)
    struct Action { int kind; long long arg; };
    const std::vector<Action> plan = {
        {0, 60000}, {1, +1}, {1, +1}, {1, -1}, {1, -1}, {1, -1}
    };

    auto fire = [&](int i) {
        timer.restart();
        if (plan[i].kind == 0)
            ctrl.requestFrameAt(plan[i].arg);
        else
            ctrl.requestStep(static_cast<int>(plan[i].arg));
    };

    QObject::connect(&ctrl, &MmtsDecodeController::frameReady,
                     [&](const QImage& img, qint64 relMs, quint64) {
        const qint64 elapsed = timer.elapsed();
        const char* kind = plan[step].kind == 0 ? "seek" : "step";
        std::fprintf(stderr, "[%d] %s %lld -> %dx%d at %lld ms in %lld ms%s\n",
                     step, kind, plan[step].arg, img.width(), img.height(),
                     (long long)relMs, (long long)elapsed,
                     elapsed < 20 ? "  (cache)" : "");
        if (step == 0)
            img.save("controller_seek.png");
        ++step;
        if (step >= (int)plan.size()) {
            img.save("controller_last.png");
            app.quit();
        } else {
            fire(step);
        }
    });

    QObject::connect(&ctrl, &MmtsDecodeController::opened,
                     [&](bool ok, qint64 dur, double fps, const QString& e) {
        if (!ok) {
            std::fprintf(stderr, "open failed: %s\n", e.toLocal8Bit().constData());
            app.exit(1);
            return;
        }
        std::fprintf(stderr, "opened: duration=%lld ms fps=%.3f\n", (long long)dur, fps);
        fire(0);
    });

    ctrl.openMedia(mmts, map);
    return app.exec();
}
