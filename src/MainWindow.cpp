#include "MainWindow.h"

#include <QAction>
#include <QFileDialog>
#include <QHeaderView>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("MMTS Edit GUI");
    resize(1100, 720);

    auto* openAction = new QAction("Open Map...", this);
    connect(openAction, &QAction::triggered, this, &MainWindow::openMapDialog);
    auto* fileMenu = menuBar()->addMenu("File");
    fileMenu->addAction(openAction);

    m_timeline = new TimelineWidget(this);
    m_trackTable = new QTableWidget(this);
    m_mptTable = new QTableWidget(this);
    m_rapTable = new QTableWidget(this);
    m_seekTable = new QTableWidget(this);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(m_trackTable, "Tracks");
    tabs->addTab(m_mptTable, "MPT");
    tabs->addTab(m_rapTable, "RAP");
    tabs->addTab(m_seekTable, "Seek");

    auto* splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(m_timeline);
    splitter->addWidget(tabs);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    setCentralWidget(splitter);

    statusBar()->showMessage("Ready");
}

bool MainWindow::openMap(const QString& path)
{
    MmtsMap map;
    QString error;
    if (!MmtsMapReader::load(path, map, &error)) {
        QMessageBox::warning(this, "Open Map Failed", error);
        return false;
    }

    m_map = map;
    m_timeline->setMap(m_map);
    populateTables();
    statusBar()->showMessage(QString("Loaded %1").arg(path));
    return true;
}

void MainWindow::openMapDialog()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        "Open MMTS Map",
        QString(),
        "MMTS Map (*.mmtsmap);;All Files (*)");
    if (!path.isEmpty())
        openMap(path);
}

void MainWindow::populateTables()
{
    populateTrackTable();
    populateMptTable();
    populatePointTable(m_rapTable, m_map.rapPoints);
    populatePointTable(m_seekTable, m_map.seekPoints);
}

void MainWindow::populateTrackTable()
{
    m_trackTable->clear();
    m_trackTable->setColumnCount(6);
    m_trackTable->setHorizontalHeaderLabels({"Type", "Stream", "Packet ID", "Component", "Rate", "Format"});
    m_trackTable->setRowCount(m_map.tracks.size());
    for (int row = 0; row < m_map.tracks.size(); ++row) {
        const auto& t = m_map.tracks[row];
        m_trackTable->setItem(row, 0, new QTableWidgetItem(MmtsMapReader::trackTypeName(t.type)));
        m_trackTable->setItem(row, 1, new QTableWidgetItem(QString::number(t.streamIndex)));
        m_trackTable->setItem(row, 2, new QTableWidgetItem(QString("0x%1").arg(t.packetId, 4, 16, QLatin1Char('0')).toUpper()));
        m_trackTable->setItem(row, 3, new QTableWidgetItem(QString::number(t.componentTag)));
        m_trackTable->setItem(row, 4, new QTableWidgetItem(t.samplingRate ? QString::number(t.samplingRate) : QString()));
        m_trackTable->setItem(row, 5, new QTableWidgetItem(t.type == MmtsTrack::Type::Audio ? (t.latm ? "LATM" : "ADTS") : QString()));
    }
    m_trackTable->horizontalHeader()->setStretchLastSection(true);
}

void MainWindow::populateMptTable()
{
    m_mptTable->clear();
    m_mptTable->setColumnCount(4);
    m_mptTable->setHorizontalHeaderLabels({"Time ms", "Offset", "Audio", "Subtitle"});
    m_mptTable->setRowCount(m_map.mptChanges.size());
    for (int row = 0; row < m_map.mptChanges.size(); ++row) {
        const auto& change = m_map.mptChanges[row];
        QStringList audio;
        QStringList subtitle;
        for (int index : change.trackIndices) {
            if (index < 0 || index >= m_map.tracks.size())
                continue;
            const auto& track = m_map.tracks[index];
            const QString label = QString("%1:0x%2:%3")
                .arg(track.streamIndex)
                .arg(track.packetId, 4, 16, QLatin1Char('0')).toUpper()
                .arg(track.componentTag);
            if (track.type == MmtsTrack::Type::Audio)
                audio << label;
            else if (track.type == MmtsTrack::Type::Subtitle)
                subtitle << label;
        }
        m_mptTable->setItem(row, 0, new QTableWidgetItem(QString::number(change.timeMs)));
        m_mptTable->setItem(row, 1, new QTableWidgetItem(QString::number(change.offset)));
        m_mptTable->setItem(row, 2, new QTableWidgetItem(audio.join(", ")));
        m_mptTable->setItem(row, 3, new QTableWidgetItem(subtitle.join(", ")));
    }
    m_mptTable->horizontalHeader()->setStretchLastSection(true);
}

void MainWindow::populatePointTable(QTableWidget* table, const QVector<MmtsPoint>& points)
{
    table->clear();
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({"Time ms", "Offset"});
    table->setRowCount(points.size());
    for (int row = 0; row < points.size(); ++row) {
        table->setItem(row, 0, new QTableWidgetItem(QString::number(points[row].timeMs)));
        table->setItem(row, 1, new QTableWidgetItem(QString::number(points[row].offset)));
    }
    table->horizontalHeader()->setStretchLastSection(true);
}
