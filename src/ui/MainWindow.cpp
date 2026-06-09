#include "MainWindow.h"

#include "MmtsDecodeController.h"
#include "MmtsExporter.h"
#include "MmtsMapBuilder.h"
#include "MmtsTsExporter.h"
#include "PreviewWidget.h"

#include <QAction>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStatusBar>
#include <QThread>
#include <QToolBar>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {
QString formatMs(qint64 ms)
{
    if (ms < 0)
        return QStringLiteral("--:--:--.---");
    const qint64 h = ms / 3600000;
    const qint64 m = (ms / 60000) % 60;
    const qint64 s = (ms / 1000) % 60;
    const qint64 f = ms % 1000;
    return QString::asprintf("%02lld:%02lld:%02lld.%03lld", h, m, s, f);
}
} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("MMTS Edit GUI");
    resize(1100, 720);

    auto* openMediaAction = new QAction("Open Media...", this);
    connect(openMediaAction, &QAction::triggered, this, &MainWindow::openMediaDialog);
    auto* openAction = new QAction("Open Map...", this);
    connect(openAction, &QAction::triggered, this, &MainWindow::openMapDialog);
    auto* openEditAction = new QAction("Open Edit...", this);
    connect(openEditAction, &QAction::triggered, this, &MainWindow::openEditDialog);
    auto* saveEditAction = new QAction("Save Edit...", this);
    connect(saveEditAction, &QAction::triggered, this, &MainWindow::saveEditDialog);
    auto* exportAction = new QAction("Export Edited Media...", this);
    connect(exportAction, &QAction::triggered, this, &MainWindow::exportMediaDialog);
    auto* fileMenu = menuBar()->addMenu("File");
    fileMenu->addAction(openMediaAction);
    fileMenu->addAction(openAction);
    fileMenu->addAction(openEditAction);
    fileMenu->addSeparator();
    fileMenu->addAction(saveEditAction);
    fileMenu->addAction(exportAction);

    m_inSpin = new QSpinBox(this);
    m_inSpin->setRange(0, 24 * 60 * 60 * 1000);
    m_inSpin->setSuffix(" ms");
    m_inSpin->setToolTip("Source in point");
    m_outSpin = new QSpinBox(this);
    m_outSpin->setRange(0, 24 * 60 * 60 * 1000);
    m_outSpin->setSuffix(" ms");
    m_outSpin->setToolTip("Source out point");
    connect(m_inSpin, qOverload<int>(&QSpinBox::valueChanged), this, &MainWindow::updateSelectedFromSpins);
    connect(m_outSpin, qOverload<int>(&QSpinBox::valueChanged), this, &MainWindow::updateSelectedFromSpins);

    auto* toolbar = addToolBar("Edit");
    toolbar->addAction(openMediaAction);
    toolbar->addAction(openAction);
    toolbar->addAction(openEditAction);
    toolbar->addSeparator();
    toolbar->addWidget(new QLabel("In", this));
    toolbar->addWidget(m_inSpin);
    toolbar->addWidget(new QLabel("Out", this));
    toolbar->addWidget(m_outSpin);
    toolbar->addSeparator();
    auto* snapAction = new QAction("Snap to RAP", this);
    snapAction->setCheckable(true);
    snapAction->setChecked(true);
    snapAction->setToolTip("Snap In/Out drags to the nearest RAP");
    connect(snapAction, &QAction::toggled, this, [this](bool on) { m_timeline->setSnapToRap(on); });
    toolbar->addAction(snapAction);
    toolbar->addAction(saveEditAction);

    m_timeline = new TimelineWidget(this);
    m_trackTable = new QTableWidget(this);
    m_mptTable = new QTableWidget(this);
    m_rapTable = new QTableWidget(this);
    m_seekTable = new QTableWidget(this);

    // Segments (EDL) panel: cut list + add/split/delete.
    m_segmentTable = new QTableWidget(this);
    m_segmentTable->setColumnCount(4);
    m_segmentTable->setHorizontalHeaderLabels({"#", "Start", "End", "Duration"});
    m_segmentTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_segmentTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_segmentTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(m_segmentTable, &QTableWidget::itemSelectionChanged, this, [this] {
        const int row = m_segmentTable->currentRow();
        if (row != m_selectedSegment)
            selectSegment(row);
    });

    auto* addBtn = new QPushButton("Add", this);
    auto* splitBtn = new QPushButton("Split @ playhead", this);
    auto* delBtn = new QPushButton("Delete", this);
    connect(addBtn, &QPushButton::clicked, this, &MainWindow::addSegment);
    connect(splitBtn, &QPushButton::clicked, this, &MainWindow::splitSegmentAtPlayhead);
    connect(delBtn, &QPushButton::clicked, this, &MainWindow::deleteSelectedSegment);

    auto* segButtons = new QWidget(this);
    auto* segButtonsLayout = new QHBoxLayout(segButtons);
    segButtonsLayout->setContentsMargins(4, 4, 4, 0);
    segButtonsLayout->addWidget(addBtn);
    segButtonsLayout->addWidget(splitBtn);
    segButtonsLayout->addWidget(delBtn);
    segButtonsLayout->addStretch(1);

    auto* segmentsPanel = new QWidget(this);
    auto* segmentsLayout = new QVBoxLayout(segmentsPanel);
    segmentsLayout->setContentsMargins(0, 0, 0, 0);
    segmentsLayout->addWidget(segButtons);
    segmentsLayout->addWidget(m_segmentTable, 1);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(segmentsPanel, "Segments");
    tabs->addTab(m_trackTable, "Tracks");
    tabs->addTab(m_mptTable, "MPT");
    tabs->addTab(m_rapTable, "RAP");
    tabs->addTab(m_seekTable, "Seek");

    // Preview + transport (frame-accurate scrubbing).
    m_preview = new PreviewWidget(this);

    m_seekSlider = new QSlider(Qt::Horizontal, this);
    m_seekSlider->setRange(0, 0);
    m_seekSlider->setEnabled(false);
    connect(m_seekSlider, &QSlider::valueChanged, this, &MainWindow::onSeekSliderMoved);
    connect(m_seekSlider, &QSlider::sliderPressed, this, [this] { m_sliderDragging = true; });
    connect(m_seekSlider, &QSlider::sliderReleased, this, [this] {
        m_sliderDragging = false;
        showFrameAt(m_seekSlider->value()); // final precise request at the drop position
    });

    auto* prevFrameBtn = new QPushButton("-1f", this);
    auto* nextFrameBtn = new QPushButton("+1f", this);
    prevFrameBtn->setToolTip("Step one frame back (hold to repeat)");
    nextFrameBtn->setToolTip("Step one frame forward (hold to repeat)");
    // Hold to repeat. Decode requests coalesce, so this advances about one frame
    // per decode while held rather than flooding the worker.
    for (auto* b : { prevFrameBtn, nextFrameBtn }) {
        b->setAutoRepeat(true);
        b->setAutoRepeatDelay(350);
        b->setAutoRepeatInterval(80);
    }
    connect(prevFrameBtn, &QPushButton::clicked, this, [this] { stepFrame(-1); });
    connect(nextFrameBtn, &QPushButton::clicked, this, [this] { stepFrame(1); });

    auto* setInBtn = new QPushButton("Set In", this);
    auto* setOutBtn = new QPushButton("Set Out", this);
    connect(setInBtn, &QPushButton::clicked, this, &MainWindow::setInToCurrent);
    connect(setOutBtn, &QPushButton::clicked, this, &MainWindow::setOutToCurrent);

    m_posLabel = new QLabel(formatMs(-1), this);
    m_posLabel->setMinimumWidth(110);

    auto* transport = new QWidget(this);
    auto* transportLayout = new QHBoxLayout(transport);
    transportLayout->setContentsMargins(6, 4, 6, 4);
    transportLayout->addWidget(prevFrameBtn);
    transportLayout->addWidget(m_seekSlider, 1);
    transportLayout->addWidget(nextFrameBtn);
    transportLayout->addWidget(m_posLabel);
    transportLayout->addSpacing(12);
    transportLayout->addWidget(setInBtn);
    transportLayout->addWidget(setOutBtn);

    auto* previewPane = new QWidget(this);
    auto* previewLayout = new QVBoxLayout(previewPane);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewLayout->setSpacing(0);
    previewLayout->addWidget(m_preview, 1);
    previewLayout->addWidget(transport);

    auto* splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(previewPane);
    splitter->addWidget(m_timeline);
    splitter->addWidget(tabs);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 3);
    setCentralWidget(splitter);

    m_decode = std::make_unique<MmtsDecodeController>(this);
    connect(m_decode.get(), &MmtsDecodeController::opened, this, &MainWindow::onDecoderOpened);
    connect(m_decode.get(), &MmtsDecodeController::frameReady, this, &MainWindow::onFrameReady);

    connect(m_timeline, &TimelineWidget::seekRequested, this, &MainWindow::showFrameAt);
    connect(m_timeline, &TimelineWidget::segmentSelected, this, &MainWindow::selectSegment);
    connect(m_timeline, &TimelineWidget::inChanged, this,
            [this](qint64 relMs) { m_inSpin->setValue(static_cast<int>(relMs)); });
    connect(m_timeline, &TimelineWidget::outChanged, this,
            [this](qint64 relMs) { m_outSpin->setValue(static_cast<int>(relMs)); });

    statusBar()->showMessage("Ready");
}

MainWindow::~MainWindow() = default;

bool MainWindow::openFile(const QString& path)
{
    if (path.endsWith(".mmtsedit", Qt::CaseInsensitive))
        return openEdit(path);
    if (path.endsWith(".mmts", Qt::CaseInsensitive))
        return openMedia(path);
    return openMap(path);
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
    const qint64 fullRangeMs = 24ll * 60 * 60 * 1000;
    const qint64 durationMs = m_map.durationMs > 0 ? m_map.durationMs : fullRangeMs;
    const int maximumMs = static_cast<int>(qMin(durationMs, fullRangeMs));
    m_inSpin->setMaximum(maximumMs);
    m_outSpin->setMaximum(maximumMs);
    setSegments({}, -1); // start empty: the user adds the ranges to keep
    populateTables();
    statusBar()->showMessage(QString("Loaded %1").arg(path));
    return true;
}

bool MainWindow::openEdit(const QString& path)
{
    MmtsEdit edit;
    QString error;
    if (!MmtsEdit::load(path, edit, &error)) {
        QMessageBox::warning(this, "Open Edit Failed", error);
        return false;
    }

    QString mapPath = edit.map;
    if (mapPath.isEmpty()) {
        mapPath = path;
        if (mapPath.endsWith("edit", Qt::CaseInsensitive))
            mapPath.chop(4);
        mapPath += "map";
    }
    if (QFileInfo(mapPath).isRelative())
        mapPath = QFileInfo(path).dir().absoluteFilePath(mapPath);

    if (!openMap(mapPath))
        return false;

    // Start the decoder for the source media so the preview works. Prefer the
    // edit's "source"; fall back to the .mmts next to the map.
    QString sourcePath = edit.source;
    if (!sourcePath.isEmpty() && QFileInfo(sourcePath).isRelative())
        sourcePath = QFileInfo(path).dir().absoluteFilePath(sourcePath);
    if (sourcePath.isEmpty() || !QFileInfo::exists(sourcePath)) {
        QString derived = mapPath;
        if (derived.endsWith("map", Qt::CaseInsensitive))
            derived.chop(3); // sample.mmtsmap -> sample.mmts
        if (QFileInfo::exists(derived))
            sourcePath = derived;
    }

    if (QFileInfo::exists(sourcePath)) {
        m_mediaPath = sourcePath;
        m_decode->openMedia(sourcePath, m_map); // async -> onDecoderOpened shows frame 0
    } else {
        statusBar()->showMessage(QString("Edit loaded, but source media not found: %1").arg(sourcePath));
    }

    if (edit.segments.isEmpty())
        edit.segments.push_back(EditSegment{0, m_map.durationMs});
    setSegments(edit.segments, 0);
    return true;
}

bool MainWindow::openMedia(const QString& path)
{
    const QString mapPath = path + "map"; // sample.mmts -> sample.mmtsmap
    if (!QFileInfo::exists(mapPath)) {
        const auto answer = QMessageBox::question(
            this, "Create Map",
            QString("No .mmtsmap sidecar found for:\n%1\n\nCreate it now? This scans the whole "
                    "file and may take a while.").arg(QFileInfo(path).fileName()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer != QMessageBox::Yes)
            return false;
        buildMapThenOpen(path, mapPath);
        return true;
    }
    return openMediaWithMap(path, mapPath);
}

bool MainWindow::openMediaWithMap(const QString& path, const QString& mapPath)
{
    if (!openMap(mapPath))
        return false;

    m_mediaPath = path;
    statusBar()->showMessage(QString("Opening %1 ...").arg(path));
    m_decode->openMedia(path, m_map); // async; UI continues in onDecoderOpened
    return true;
}

void MainWindow::buildMapThenOpen(const QString& path, const QString& mapPath)
{
    auto* thread = new QThread(this);
    auto* worker = new MmtsMapBuildWorker;
    worker->moveToThread(thread);
    worker->configure(path, mapPath);

    auto* dlg = new QProgressDialog("Creating .mmtsmap (scanning file)...", "Cancel", 0, 100, this);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setMinimumDuration(0);
    dlg->setAutoClose(false);
    dlg->setAutoReset(false);

    connect(thread, &QThread::started, worker, &MmtsMapBuildWorker::run);
    connect(worker, &MmtsMapBuildWorker::progress, this, [dlg](qint64 done, qint64 total) {
        if (dlg->wasCanceled() || total <= 0)
            return;
        dlg->setValue(static_cast<int>(done * 100 / total));
    });
    connect(dlg, &QProgressDialog::canceled, worker, &MmtsMapBuildWorker::cancel, Qt::DirectConnection);
    connect(worker, &MmtsMapBuildWorker::finished, this,
            [this, dlg, thread, path, mapPath](bool ok, const QString& err) {
        dlg->close();
        dlg->deleteLater();
        if (ok) {
            statusBar()->showMessage(QString("Created %1").arg(mapPath));
            openMediaWithMap(path, mapPath);
        } else if (err != QLatin1String("Cancelled")) {
            QMessageBox::warning(this, "Create Map Failed", err);
        } else {
            statusBar()->showMessage("Map creation cancelled");
        }
        thread->quit();
    });
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);

    dlg->show();
    thread->start();
}

void MainWindow::onDecoderOpened(bool ok, qint64 durationMs, double fps, const QString& error)
{
    if (!ok) {
        QMessageBox::warning(this, "Open Media Failed", error);
        m_seekSlider->setEnabled(false);
        return;
    }

    m_fps = fps;
    m_durationMs = durationMs > 0 ? durationMs : m_map.durationMs;
    const int frameDur = fps > 0.0 ? qMax(1, static_cast<int>(1000.0 / fps + 0.5)) : 33;
    {
        const QSignalBlocker blocker(m_seekSlider);
        m_seekSlider->setRange(0, static_cast<int>(qMax<qint64>(0, m_durationMs)));
        m_seekSlider->setSingleStep(frameDur);
        m_seekSlider->setPageStep(1000);
        m_seekSlider->setValue(0);
    }
    m_seekSlider->setEnabled(true);

    statusBar()->showMessage(QString("Media loaded %1 (%2 fps)").arg(m_mediaPath).arg(fps, 0, 'f', 3));
    showFrameAt(0);
}

void MainWindow::onFrameReady(const QImage& image, qint64 relMs, quint64 requestId)
{
    const bool isLatest = (requestId == m_pendingRequestId);

    // When settled (not dragging), ignore frames from superseded requests still
    // in flight -- otherwise the just-released drag flashes back to an older
    // position before the final frame arrives. During a drag we keep showing
    // every frame for live feedback.
    if (!m_sliderDragging && !isLatest)
        return;

    m_currentRelMs = relMs;
    m_preview->setFrame(image);
    updatePositionLabel(relMs);
    m_timeline->setPlayhead(relMs);

    // Only move the handle for the freshest result, and never mid-drag (that
    // would yank it away from the cursor).
    if (!m_sliderDragging && isLatest) {
        const QSignalBlocker blocker(m_seekSlider);
        m_seekSlider->setValue(static_cast<int>(relMs));
    }
}

void MainWindow::showFrameAt(qint64 relMs)
{
    m_pendingRequestId = m_decode->requestFrameAt(relMs); // async -> onFrameReady
}

void MainWindow::stepFrame(int delta)
{
    m_pendingRequestId = m_decode->requestStep(delta); // async -> onFrameReady
}

void MainWindow::onSeekSliderMoved(int valueMs)
{
    showFrameAt(valueMs);
}

void MainWindow::updatePositionLabel(qint64 relMs)
{
    QString text = formatMs(relMs);
    if (m_fps > 0.0) {
        const qint64 frameNo = static_cast<qint64>(relMs * m_fps / 1000.0 + 0.5);
        text += QString("  (#%1)").arg(frameNo);
    }
    m_posLabel->setText(text);
}

void MainWindow::openMediaDialog()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        "Open MMTS Media",
        QString(),
        "MMTS Media (*.mmts);;All Files (*)");
    if (!path.isEmpty())
        openMedia(path);
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

void MainWindow::openEditDialog()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        "Open MMTS Edit",
        QString(),
        "MMTS Edit (*.mmtsedit);;All Files (*)");
    if (!path.isEmpty())
        openEdit(path);
}

void MainWindow::saveEditDialog()
{
    if (m_map.path.isEmpty()) {
        QMessageBox::information(this, "Save Edit", "Open an .mmtsmap first.");
        return;
    }

    QString defaultPath = m_map.path;
    if (defaultPath.endsWith("map"))
        defaultPath.chop(3);
    defaultPath += "edit";

    const QString path = QFileDialog::getSaveFileName(
        this,
        "Save MMTS Edit",
        defaultPath,
        "MMTS Edit (*.mmtsedit);;All Files (*)");
    if (path.isEmpty())
        return;

    QString sourcePath = m_map.path;
    if (sourcePath.endsWith("map"))
        sourcePath.chop(3);

    MmtsEdit edit;
    edit.version = 1;
    edit.source = sourcePath;
    edit.sourceSize = m_map.sourceSize;
    edit.map = m_map.path;
    edit.segments = m_segments;

    QString error;
    if (!edit.save(path, &error)) {
        QMessageBox::warning(this, "Save Edit Failed", error);
        return;
    }
    statusBar()->showMessage(QString("Saved %1 (%2 segments)").arg(path).arg(m_segments.size()));
}

void MainWindow::exportMediaDialog()
{
    if (m_mediaPath.isEmpty() || m_map.path.isEmpty()) {
        QMessageBox::information(this, "Export", "Open the media (.mmts) first.");
        return;
    }
    if (m_segments.isEmpty()) {
        QMessageBox::information(this, "Export", "Add at least one segment first.");
        return;
    }

    const qint64 srcSize = QFileInfo(m_mediaPath).size();
    const auto plan = MmtsTsExporter::planSegments(m_map, m_segments, srcSize);
    if (plan.isEmpty()) {
        QMessageBox::warning(this, "Export", "Could not map any segment to RAP byte ranges.");
        return;
    }

    const QFileInfo srcInfo(m_mediaPath);
    const QString def = srcInfo.absolutePath() + "/" + srcInfo.completeBaseName() + "_edited.ts";
    const QString path = QFileDialog::getSaveFileName(this, "Export Edited TS", def, "MPEG-2 TS (*.ts)");
    if (path.isEmpty())
        return;

    auto* thread = new QThread(this);
    auto* worker = new MmtsTsExportWorker;
    worker->moveToThread(thread);
    worker->configure(m_mediaPath, path, m_map, m_segments, srcSize);

    auto* dlg = new QProgressDialog("Exporting edited TS (remuxing)...", "Cancel", 0, 100, this);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setMinimumDuration(0);
    dlg->setAutoClose(false);
    dlg->setAutoReset(false);

    connect(thread, &QThread::started, worker, &MmtsTsExportWorker::run);
    connect(worker, &MmtsTsExportWorker::progress, this, [dlg](qint64 done, qint64 total) {
        // Ignore progress still queued after a cancel: setValue() on a cancelled
        // QProgressDialog would pop it back up momentarily.
        if (dlg->wasCanceled() || total <= 0)
            return;
        dlg->setValue(static_cast<int>(done * 100 / total));
    });
    // Direct connection: cancel() only flips an atomic, and the worker thread is
    // busy (not running its event loop), so a queued call would not be delivered
    // until it finished. Direct lets the GUI thread set it now.
    connect(dlg, &QProgressDialog::canceled, worker, &MmtsTsExportWorker::cancel, Qt::DirectConnection);
    connect(worker, &MmtsTsExportWorker::finished, this, [this, dlg, thread, path](bool ok, const QString& err) {
        dlg->close();
        dlg->deleteLater();
        if (ok)
            statusBar()->showMessage(QString("Exported %1").arg(path));
        else if (err != QLatin1String("Cancelled"))
            QMessageBox::warning(this, "Export Failed", err);
        else
            statusBar()->showMessage("Export cancelled");
        thread->quit();
    });
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);

    statusBar()->showMessage(QString("Exporting %1 segments to TS...").arg(plan.size()));
    dlg->show();
    thread->start();
}

qint64 MainWindow::sourceDurationMs() const
{
    if (m_durationMs > 0)
        return m_durationMs;
    return m_map.durationMs > 0 ? m_map.durationMs : 0;
}

void MainWindow::setSegments(const QVector<EditSegment>& segments, int selected)
{
    m_segments = segments;
    rebuildSegmentTable();
    selectSegment(selected >= 0 && selected < m_segments.size() ? selected : (m_segments.isEmpty() ? -1 : 0));
}

void MainWindow::rebuildSegmentTable()
{
    const QSignalBlocker blocker(m_segmentTable);
    m_segmentTable->setRowCount(m_segments.size());
    for (int i = 0; i < m_segments.size(); ++i) {
        const auto& s = m_segments[i];
        m_segmentTable->setItem(i, 0, new QTableWidgetItem(QString::number(i + 1)));
        m_segmentTable->setItem(i, 1, new QTableWidgetItem(formatMs(s.startMs)));
        m_segmentTable->setItem(i, 2, new QTableWidgetItem(formatMs(s.endMs)));
        m_segmentTable->setItem(i, 3, new QTableWidgetItem(formatMs(s.durationMs())));
    }
    m_segmentTable->horizontalHeader()->setStretchLastSection(true);
    if (m_selectedSegment >= 0 && m_selectedSegment < m_segments.size())
        m_segmentTable->selectRow(m_selectedSegment);
}

void MainWindow::selectSegment(int index)
{
    m_selectedSegment = (index >= 0 && index < m_segments.size()) ? index : -1;
    const bool valid = m_selectedSegment >= 0;

    m_inSpin->setEnabled(valid);
    m_outSpin->setEnabled(valid);
    if (valid) {
        const auto& s = m_segments[m_selectedSegment];
        const QSignalBlocker inBlocker(m_inSpin);
        const QSignalBlocker outBlocker(m_outSpin);
        m_inSpin->setValue(static_cast<int>(s.startMs));
        m_outSpin->setValue(static_cast<int>(s.endMs));
    }
    {
        const QSignalBlocker blocker(m_segmentTable);
        if (valid)
            m_segmentTable->selectRow(m_selectedSegment);
        else
            m_segmentTable->clearSelection();
    }
    syncTimeline();
}

void MainWindow::syncTimeline()
{
    if (m_timeline)
        m_timeline->setSegments(m_segments, m_selectedSegment);
}

void MainWindow::updateSelectedFromSpins()
{
    if (m_selectedSegment < 0 || m_selectedSegment >= m_segments.size())
        return;
    if (m_outSpin->value() < m_inSpin->value()) {
        const QSignalBlocker blocker(m_outSpin);
        m_outSpin->setValue(m_inSpin->value());
    }
    auto& s = m_segments[m_selectedSegment];
    s.startMs = m_inSpin->value();
    s.endMs = m_outSpin->value();

    const QSignalBlocker blocker(m_segmentTable);
    m_segmentTable->setItem(m_selectedSegment, 1, new QTableWidgetItem(formatMs(s.startMs)));
    m_segmentTable->setItem(m_selectedSegment, 2, new QTableWidgetItem(formatMs(s.endMs)));
    m_segmentTable->setItem(m_selectedSegment, 3, new QTableWidgetItem(formatMs(s.durationMs())));
    syncTimeline();
}

// Selects a segment to edit without ever creating one: keeps the current
// selection, otherwise falls back to the first segment. Returns false only when
// there are no segments at all (so Set In/Out never silently add a new track).
bool MainWindow::ensureSegmentSelected()
{
    if (m_selectedSegment >= 0 && m_selectedSegment < m_segments.size())
        return true;
    if (m_segments.isEmpty())
        return false;
    selectSegment(0);
    return m_selectedSegment >= 0;
}

void MainWindow::setInToCurrent()
{
    if (!ensureSegmentSelected()) {
        statusBar()->showMessage("No segment to edit — press Add first");
        return;
    }
    m_inSpin->setValue(static_cast<int>(m_currentRelMs));
}

void MainWindow::setOutToCurrent()
{
    if (!ensureSegmentSelected()) {
        statusBar()->showMessage("No segment to edit — press Add first");
        return;
    }
    m_outSpin->setValue(static_cast<int>(m_currentRelMs));
}

void MainWindow::addSegment()
{
    const qint64 dur = sourceDurationMs();
    if (dur <= 0)
        return;
    EditSegment s;
    s.startMs = qBound<qint64>(0, m_currentRelMs, dur);
    s.endMs = dur;
    if (s.endMs <= s.startMs)
        s.startMs = qMax<qint64>(0, dur - 1000);
    m_segments.push_back(s);
    rebuildSegmentTable();
    selectSegment(m_segments.size() - 1);
}

void MainWindow::splitSegmentAtPlayhead()
{
    const qint64 ph = m_currentRelMs;
    for (int i = 0; i < m_segments.size(); ++i) {
        auto& s = m_segments[i];
        if (ph > s.startMs && ph < s.endMs) {
            EditSegment right{ph, s.endMs};
            s.endMs = ph;
            m_segments.insert(i + 1, right);
            rebuildSegmentTable();
            selectSegment(i + 1);
            statusBar()->showMessage(QString("Split segment %1 at %2").arg(i + 1).arg(formatMs(ph)));
            return;
        }
    }
    statusBar()->showMessage("Playhead is not inside a segment");
}

void MainWindow::deleteSelectedSegment()
{
    if (m_selectedSegment < 0 || m_selectedSegment >= m_segments.size())
        return;
    m_segments.removeAt(m_selectedSegment);
    rebuildSegmentTable();
    selectSegment(m_segments.isEmpty() ? -1 : qMin(m_selectedSegment, m_segments.size() - 1));
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
