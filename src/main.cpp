#include "MainWindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    MainWindow window;
    window.show();

    if (argc >= 2)
        window.openFile(QString::fromLocal8Bit(argv[1]));

    return app.exec();
}
