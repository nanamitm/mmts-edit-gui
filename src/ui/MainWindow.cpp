#include "MainWindow.h"

#include "MmtsDecodeController.h"
#include "HevcReencoder.h"
#include "MmtsMapBuilder.h"
#include "MmtsCutPlan.h"
#include "MmtsSmartExporter.h"
#if MMTS_ENABLE_TSDUCK
#include "MmtsTsExporter.h"
#endif
#include "PreviewWidget.h"

// The version this build comes from, written at build time by
// cmake/GenerateVersionHeader.cmake (like TVTest's TVTestVersionHashGen.bat).
#if __has_include("MmtsEditVersion.h")
#include "MmtsEditVersion.h"
#endif
#ifndef MMTS_EDIT_VERSION_STRING
#define MMTS_EDIT_VERSION_STRING "unknown"
#endif

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
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
#include <algorithm>

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
    setWindowTitle("MMTS Edit GUI " MMTS_EDIT_VERSION_STRING);
    resize(1100, 720);

    auto* openMediaAction = new QAction("Open Media...", this);
    connect(openMediaAction, &QAction::triggered, this, &MainWindow::openMediaDialog);
    auto* openAction = new QAction("Open Map...", this);
    connect(openAction, &QAction::triggered, this, &MainWindow::openMapDialog);
    auto* openEditAction = new QAction("Open Edit...", this);
    connect(openEditAction, &QAction::triggered, this, &MainWindow::openEditDialog);
    auto* saveEditAction = new QAction("Save Edit...", this);
    connect(saveEditAction, &QAction::triggered, this, &MainWindow::saveEditDialog);
    auto* exportAction = new QAction("Export Edited TS...", this);
    connect(exportAction, &QAction::triggered, this, &MainWindow::exportMediaDialog);
#if !MMTS_ENABLE_TSDUCK
    exportAction->setEnabled(false);
    exportAction->setToolTip("TS export is currently available only in Windows release builds.");
#endif
    auto* exportMmtsAction = new QAction("Export Edited MMTS...", this);
    connect(exportMmtsAction, &QAction::triggered, this, &MainWindow::exportMmtsDialog);
    auto* fileMenu = menuBar()->addMenu("File");
    fileMenu->addAction(openMediaAction);
    fileMenu->addAction(openAction);
    fileMenu->addAction(openEditAction);
    fileMenu->addSeparator();
    fileMenu->addAction(saveEditAction);
    fileMenu->addAction(exportAction);
    fileMenu->addAction(exportMmtsAction);
    fileMenu->setMinimumWidth(fontMetrics().horizontalAdvance("Export Edited MMTS...") + 48);

    m_draftInLabel = new QLabel(formatMs(-1), this);
    m_draftInLabel->setMinimumWidth(110);
    m_draftInLabel->setToolTip("New range in point");
    m_draftOutLabel = new QLabel(formatMs(-1), this);
    m_draftOutLabel->setMinimumWidth(110);
    m_draftOutLabel->setToolTip("New range out point");

    m_editInSpin = new QSpinBox(this);
    m_editInSpin->setRange(0, 24 * 60 * 60 * 1000);
    m_editInSpin->setSuffix(" ms");
    m_editInSpin->setToolTip("Selected range in point");
    m_editOutSpin = new QSpinBox(this);
    m_editOutSpin->setRange(0, 24 * 60 * 60 * 1000);
    m_editOutSpin->setSuffix(" ms");
    m_editOutSpin->setToolTip("Selected range out point");
    m_editSegmentCombo = new QComboBox(this);
    connect(m_editSegmentCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &MainWindow::selectSegment);

    auto* toolbar = addToolBar("Edit");
    toolbar->addAction(openMediaAction);
    toolbar->addAction(openAction);
    toolbar->addAction(openEditAction);
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

    // Add Range panel: draft range + list management. Selecting a row here only
    // chooses the row for reorder/delete; range contents are edited elsewhere.
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

    auto* addBtn = new QPushButton("Add Range", this);
    auto* clearDraftBtn = new QPushButton("Clear", this);
    auto* setDraftInBtn = new QPushButton("Set In", this);
    auto* setDraftOutBtn = new QPushButton("Set Out", this);
    auto* upBtn = new QPushButton("Up", this);
    auto* downBtn = new QPushButton("Down", this);
    auto* delBtn = new QPushButton("Delete", this);
    connect(addBtn, &QPushButton::clicked, this, &MainWindow::addSegment);
    connect(clearDraftBtn, &QPushButton::clicked, this, &MainWindow::clearDraftSegment);
    connect(setDraftInBtn, &QPushButton::clicked, this, &MainWindow::setInToCurrent);
    connect(setDraftOutBtn, &QPushButton::clicked, this, &MainWindow::setOutToCurrent);
    connect(upBtn, &QPushButton::clicked, this, &MainWindow::moveSelectedSegmentUp);
    connect(downBtn, &QPushButton::clicked, this, &MainWindow::moveSelectedSegmentDown);
    connect(delBtn, &QPushButton::clicked, this, &MainWindow::deleteSelectedSegment);

    auto* draftControls = new QWidget(this);
    auto* draftLayout = new QHBoxLayout(draftControls);
    draftLayout->setContentsMargins(4, 4, 4, 0);
    draftLayout->addWidget(new QLabel("In", this));
    draftLayout->addWidget(m_draftInLabel);
    draftLayout->addWidget(setDraftInBtn);
    draftLayout->addWidget(new QLabel("Out", this));
    draftLayout->addWidget(m_draftOutLabel);
    draftLayout->addWidget(setDraftOutBtn);
    draftLayout->addWidget(addBtn);
    draftLayout->addWidget(clearDraftBtn);
    draftLayout->addStretch(1);

    auto* listButtons = new QWidget(this);
    auto* listButtonsLayout = new QHBoxLayout(listButtons);
    listButtonsLayout->setContentsMargins(4, 4, 4, 0);
    listButtonsLayout->addWidget(upBtn);
    listButtonsLayout->addWidget(downBtn);
    listButtonsLayout->addWidget(delBtn);
    listButtonsLayout->addStretch(1);

    auto* addRangePanel = new QWidget(this);
    auto* addRangeLayout = new QVBoxLayout(addRangePanel);
    addRangeLayout->setContentsMargins(0, 0, 0, 0);
    addRangeLayout->addWidget(draftControls);
    addRangeLayout->addWidget(listButtons);
    addRangeLayout->addWidget(m_segmentTable, 1);

    auto* editSetInBtn = new QPushButton("Set In", this);
    auto* editSetOutBtn = new QPushButton("Set Out", this);
    auto* applyEditBtn = new QPushButton("Apply", this);
    auto* splitBtn = new QPushButton("Split @ playhead", this);
    auto* editDelBtn = new QPushButton("Delete", this);
    connect(editSetInBtn, &QPushButton::clicked, this, &MainWindow::setInToCurrent);
    connect(editSetOutBtn, &QPushButton::clicked, this, &MainWindow::setOutToCurrent);
    connect(applyEditBtn, &QPushButton::clicked, this, &MainWindow::applyEditedSegment);
    connect(splitBtn, &QPushButton::clicked, this, &MainWindow::splitSegmentAtPlayhead);
    connect(editDelBtn, &QPushButton::clicked, this, &MainWindow::deleteSelectedSegment);

    auto* editControls = new QWidget(this);
    auto* editLayout = new QHBoxLayout(editControls);
    editLayout->setContentsMargins(4, 4, 4, 0);
    editLayout->addWidget(new QLabel("Target", this));
    editLayout->addWidget(m_editSegmentCombo);
    editLayout->addWidget(new QLabel("In", this));
    editLayout->addWidget(m_editInSpin);
    editLayout->addWidget(editSetInBtn);
    editLayout->addWidget(new QLabel("Out", this));
    editLayout->addWidget(m_editOutSpin);
    editLayout->addWidget(editSetOutBtn);
    editLayout->addWidget(applyEditBtn);
    editLayout->addWidget(splitBtn);
    editLayout->addWidget(editDelBtn);
    editLayout->addStretch(1);

    auto* editRangePanel = new QWidget(this);
    auto* editRangeLayout = new QVBoxLayout(editRangePanel);
    editRangeLayout->setContentsMargins(0, 0, 0, 0);
    editRangeLayout->addWidget(editControls);
    auto* editHint = new QLabel("Use the target list above to choose which range to modify.", this);
    editHint->setContentsMargins(6, 6, 6, 0);
    editRangeLayout->addWidget(editHint);
    editRangeLayout->addStretch(1);

    auto* tabs = new QTabWidget(this);
    m_segmentTabs = new QTabWidget(this);
    m_segmentTabs->addTab(addRangePanel, "Add Range");
    m_segmentTabs->addTab(editRangePanel, "Edit Ranges");
    connect(m_segmentTabs, &QTabWidget::currentChanged, this, [this] { syncTimeline(); });
    tabs->addTab(m_segmentTabs, "Segments");
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
            [this](qint64 relMs) {
        if (editRangesTabActive())
            m_editInSpin->setValue(static_cast<int>(relMs));
        else {
            m_draftInSet = true;
            m_draftInMs = relMs;
            updateDraftLabels();
            syncTimeline();
        }
    });
    connect(m_timeline, &TimelineWidget::outChanged, this,
            [this](qint64 relMs) {
        if (editRangesTabActive())
            m_editOutSpin->setValue(static_cast<int>(relMs));
        else {
            m_draftOutSet = true;
            m_draftOutMs = relMs;
            updateDraftLabels();
            syncTimeline();
        }
    });

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

    m_decode->closeMedia();
    m_mediaPath.clear();
    m_durationMs = 0;
    m_fps = 0.0;
    m_currentRelMs = 0;
    m_pendingRequestId = 0;
    m_preview->clearFrame();
    m_seekSlider->setEnabled(false);
    m_seekSlider->setRange(0, 0);
    m_map = map;
    m_timeline->setMap(m_map);
    const qint64 fullRangeMs = 24ll * 60 * 60 * 1000;
    const qint64 durationMs = m_map.durationMs > 0 ? m_map.durationMs : fullRangeMs;
    const int maximumMs = static_cast<int>(qMin(durationMs, fullRangeMs));
    m_editInSpin->setMaximum(maximumMs);
    m_editOutSpin->setMaximum(maximumMs);
    m_draftInMs = 0;
    m_draftOutMs = 0;
    m_draftInSet = false;
    m_draftOutSet = false;
    updateDraftLabels();
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
#if !MMTS_ENABLE_TSDUCK
    QMessageBox::information(this, "Export",
                             "TS export is not available in this build.\n\n"
                             "Use the Windows release to export edited MPEG-2 TS files.");
    return;
#else
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
    int smartRenderSegments = 0;
    constexpr qint64 kSmartRenderThresholdMs = 20;
    for (const auto& seg : plan) {
        if ((seg.copyStartMs - seg.inAbsMs) > kSmartRenderThresholdMs ||
            (seg.outAbsMs - seg.copyEndMs) > kSmartRenderThresholdMs) {
            ++smartRenderSegments;
        }
    }
    constexpr auto kSmartRenderEncoder = "hevc_nvenc";
    bool requireFrameAccurate = smartRenderSegments > 0;
    if (requireFrameAccurate && !HevcReencoder::encoderAvailable(QString::fromLatin1(kSmartRenderEncoder))) {
        QMessageBox box(QMessageBox::Warning, "Hardware Encoder Not Found",
                        QString("%1 segment(s) require HEVC re-encoding for frame-accurate cuts.\n\n"
                                "The current encoder is %2, but this FFmpeg build does not expose that encoder. "
                                "If fallback is allowed, export can continue by using RAP boundaries, but the "
                                "affected cut(s) may lose frame accuracy.")
                            .arg(smartRenderSegments)
                            .arg(kSmartRenderEncoder),
                        QMessageBox::Yes | QMessageBox::No, this);
        box.setDefaultButton(QMessageBox::No);
        box.setButtonText(QMessageBox::Yes, "Continue");
        box.setButtonText(QMessageBox::No, "Cancel");
        QCheckBox allowFallback("Allow RAP-boundary fallback for this export", &box);
        allowFallback.setChecked(false);
        box.setCheckBox(&allowFallback);
        if (box.exec() != QMessageBox::Yes)
            return;
        requireFrameAccurate = !allowFallback.isChecked();
    }

    const QFileInfo srcInfo(m_mediaPath);
    const QString def = srcInfo.absolutePath() + "/" + srcInfo.completeBaseName() + "_edited.ts";
    const QString path = QFileDialog::getSaveFileName(this, "Export Edited TS", def, "MPEG-2 TS (*.ts)");
    if (path.isEmpty())
        return;

    auto* thread = new QThread(this);
    auto* worker = new MmtsTsExportWorker;
    worker->moveToThread(thread);
    worker->configure(m_mediaPath, path, m_map, m_segments, srcSize, requireFrameAccurate);

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
    connect(worker, &MmtsTsExportWorker::finished, this,
            [this, dlg, thread, path](bool ok, const QString& err, const QStringList& warnings) {
        dlg->close();
        dlg->deleteLater();
        if (ok) {
            statusBar()->showMessage(QString("Exported %1").arg(path));
            if (!warnings.isEmpty()) {
                QMessageBox::warning(
                    this, "Export Completed With Warnings",
                    QString("Export finished, but some smart-rendered cuts fell back to RAP boundaries. "
                            "Frame accuracy may be reduced for those cuts.\n\n%1")
                        .arg(warnings.join("\n")));
            }
        } else if (err != QLatin1String("Cancelled")) {
            QMessageBox::warning(this, "Export Failed", err);
        } else {
            statusBar()->showMessage("Export cancelled");
        }
        thread->quit();
    });
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);

    statusBar()->showMessage(QString("Exporting %1 segments to TS...").arg(plan.size()));
    dlg->show();
    thread->start();
#endif
}

void MainWindow::exportMmtsDialog()
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
    const auto plan = planMmtsCutSegments(m_map, m_segments, srcSize);
    if (plan.isEmpty()) {
        QMessageBox::warning(this, "Export", "Could not map any segment to RAP byte ranges.");
        return;
    }
    int smartRenderSegments = 0;
    for (const auto& seg : plan) {
        if ((seg.copyStartMs - seg.inAbsMs) > kMmtsCutRapThresholdMs ||
            (seg.outAbsMs - seg.copyEndMs) > kMmtsCutRapThresholdMs) {
            ++smartRenderSegments;
        }
    }
    constexpr auto kSmartRenderEncoder = "hevc_nvenc";
    bool requireFrameAccurate = smartRenderSegments > 0;
    if (requireFrameAccurate && !HevcReencoder::encoderAvailable(QString::fromLatin1(kSmartRenderEncoder))) {
        QMessageBox box(QMessageBox::Warning, "Hardware Encoder Not Found",
                        QString("%1 segment(s) require HEVC re-encoding for frame-accurate cuts.\n\n"
                                "The current encoder is %2, but this FFmpeg build does not expose that encoder. "
                                "If fallback is allowed, export can continue by using RAP boundaries, but the "
                                "affected cut(s) may lose frame accuracy.")
                            .arg(smartRenderSegments)
                            .arg(kSmartRenderEncoder),
                        QMessageBox::Yes | QMessageBox::No, this);
        box.setDefaultButton(QMessageBox::No);
        box.setButtonText(QMessageBox::Yes, "Continue");
        box.setButtonText(QMessageBox::No, "Cancel");
        QCheckBox allowFallback("Allow RAP-boundary fallback for this export", &box);
        allowFallback.setChecked(false);
        box.setCheckBox(&allowFallback);
        if (box.exec() != QMessageBox::Yes)
            return;
        requireFrameAccurate = !allowFallback.isChecked();
    }

    const QFileInfo srcInfo(m_mediaPath);
    const QString def = srcInfo.absolutePath() + "/" + srcInfo.completeBaseName() + "_edited.mmts";
    const QString path = QFileDialog::getSaveFileName(this, "Export Edited MMTS", def, "MMTS (*.mmts)");
    if (path.isEmpty())
        return;
    if (QFileInfo(path).canonicalFilePath() == srcInfo.canonicalFilePath()) {
        QMessageBox::warning(this, "Export", "The output file must differ from the source.");
        return;
    }

    auto* thread = new QThread(this);
    auto* worker = new MmtsSmartExportWorker;
    worker->moveToThread(thread);
    worker->configure(m_mediaPath, path, m_map, m_segments, srcSize, requireFrameAccurate);

    auto* dlg = new QProgressDialog("Exporting edited MMTS (smart copy)...", "Cancel", 0, 100, this);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setMinimumDuration(0);
    dlg->setAutoClose(false);
    dlg->setAutoReset(false);

    connect(thread, &QThread::started, worker, &MmtsSmartExportWorker::run);
    connect(worker, &MmtsSmartExportWorker::progress, this, [dlg](qint64 done, qint64 total) {
        // Ignore progress still queued after a cancel: setValue() on a cancelled
        // QProgressDialog would pop it back up momentarily.
        if (dlg->wasCanceled() || total <= 0)
            return;
        dlg->setValue(static_cast<int>(done * 100 / total));
    });
    // Direct connection: cancel() only flips an atomic, and the worker thread is
    // busy (not running its event loop), so a queued call would not be delivered
    // until it finished. Direct lets the GUI thread set it now.
    connect(dlg, &QProgressDialog::canceled, worker, &MmtsSmartExportWorker::cancel, Qt::DirectConnection);
    connect(worker, &MmtsSmartExportWorker::finished, this,
            [this, dlg, thread, path](bool ok, const QString& err, const QStringList& warnings) {
        dlg->close();
        dlg->deleteLater();
        if (ok) {
            statusBar()->showMessage(QString("Exported %1").arg(path));
            if (!warnings.isEmpty()) {
                QMessageBox::warning(
                    this, "Export Completed With Warnings",
                    QString("Export finished, but some smart-rendered cuts fell back to RAP boundaries. "
                            "Frame accuracy may be reduced for those cuts.\n\n%1")
                        .arg(warnings.join("\n")));
            }
        } else if (err != QLatin1String("Cancelled")) {
            QMessageBox::warning(this, "Export Failed", err);
        } else {
            statusBar()->showMessage("Export cancelled");
        }
        thread->quit();
    });
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);

    statusBar()->showMessage(QString("Exporting %1 segments to MMTS...").arg(plan.size()));
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
    m_selectedSegment = selected >= 0 && selected < m_segments.size() ? selected : -1;
    rebuildSegmentTable();
    rebuildEditCombo();
    selectSegment(m_selectedSegment);
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

void MainWindow::rebuildEditCombo()
{
    if (!m_editSegmentCombo)
        return;

    const QSignalBlocker blocker(m_editSegmentCombo);
    m_editSegmentCombo->clear();
    for (int i = 0; i < m_segments.size(); ++i) {
        const auto& s = m_segments[i];
        m_editSegmentCombo->addItem(QString("#%1  %2 - %3")
            .arg(i + 1)
            .arg(formatMs(s.startMs))
            .arg(formatMs(s.endMs)));
    }
    if (m_selectedSegment >= 0 && m_selectedSegment < m_segments.size())
        m_editSegmentCombo->setCurrentIndex(m_selectedSegment);
    else
        m_editSegmentCombo->setCurrentIndex(-1);
}

void MainWindow::selectSegment(int index)
{
    m_selectedSegment = (index >= 0 && index < m_segments.size()) ? index : -1;
    const bool valid = m_selectedSegment >= 0;
    const bool hasSource = sourceDurationMs() > 0;

    m_draftInLabel->setEnabled(hasSource);
    m_draftOutLabel->setEnabled(hasSource);
    m_editInSpin->setEnabled(valid);
    m_editOutSpin->setEnabled(valid);
    if (valid) {
        const auto& s = m_segments[m_selectedSegment];
        const QSignalBlocker inBlocker(m_editInSpin);
        const QSignalBlocker outBlocker(m_editOutSpin);
        m_editInSpin->setValue(static_cast<int>(s.startMs));
        m_editOutSpin->setValue(static_cast<int>(s.endMs));
    } else {
        const QSignalBlocker inBlocker(m_editInSpin);
        const QSignalBlocker outBlocker(m_editOutSpin);
        m_editInSpin->setValue(0);
        m_editOutSpin->setValue(0);
    }
    {
        const QSignalBlocker blocker(m_segmentTable);
        if (valid)
            m_segmentTable->selectRow(m_selectedSegment);
        else
            m_segmentTable->clearSelection();
    }
    rebuildEditCombo();
    syncTimeline();
}

void MainWindow::syncTimeline()
{
    if (m_timeline) {
        const bool editMode = editRangesTabActive() && m_selectedSegment >= 0 && m_selectedSegment < m_segments.size();
        m_timeline->setSegments(m_segments, editMode ? m_selectedSegment : -1);
        m_timeline->setHighlightedSegment(!editMode ? m_selectedSegment : -1);
        const qint64 dur = sourceDurationMs();
        const bool showDraft = (!editMode && dur > 0 && (m_draftInSet || m_draftOutSet));
        const qint64 in = qBound<qint64>(0, m_draftInMs, dur);
        const qint64 out = qBound<qint64>(0, m_draftOutMs, dur);
        const qint64 draftIn = m_draftInSet ? in : out;
        const qint64 draftOut = m_draftOutSet ? out : in;
        m_timeline->setDraftSegment(EditSegment{qMin(draftIn, draftOut), qMax(draftIn, draftOut)},
                                    showDraft, m_draftInSet, m_draftOutSet);
    }
}

void MainWindow::updateSelectedFromSpins()
{
    syncTimeline();
}

void MainWindow::updateDraftLabels()
{
    if (m_draftInLabel)
        m_draftInLabel->setText(m_draftInSet ? formatMs(m_draftInMs) : QStringLiteral("--:--:--.---"));
    if (m_draftOutLabel)
        m_draftOutLabel->setText(m_draftOutSet ? formatMs(m_draftOutMs) : QStringLiteral("--:--:--.---"));
}

void MainWindow::applyEditedSegment()
{
    if (!ensureSegmentSelected())
        return;

    const qint64 dur = sourceDurationMs();
    const qint64 in = qBound<qint64>(0, static_cast<qint64>(m_editInSpin->value()), dur);
    const qint64 out = qBound<qint64>(0, static_cast<qint64>(m_editOutSpin->value()), dur);
    if (in == out) {
        statusBar()->showMessage("Edited range needs different In and Out points");
        return;
    }

    m_segments[m_selectedSegment] = EditSegment{qMin(in, out), qMax(in, out)};
    rebuildSegmentTable();
    rebuildEditCombo();
    selectSegment(m_selectedSegment);
    statusBar()->showMessage(QString("Updated range %1").arg(m_selectedSegment + 1));
}

// Selects a segment to edit without ever creating one: keeps the current
// selection, otherwise falls back to the first segment.
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
    if (sourceDurationMs() <= 0) {
        statusBar()->showMessage("Open media or a map first");
        return;
    }
    if (editRangesTabActive()) {
        if (!ensureSegmentSelected())
            return;
        m_editInSpin->setValue(static_cast<int>(m_currentRelMs));
        syncTimeline();
        return;
    }
    m_draftInSet = true;
    m_draftInMs = m_currentRelMs;
    updateDraftLabels();
    syncTimeline();
}

void MainWindow::setOutToCurrent()
{
    if (sourceDurationMs() <= 0) {
        statusBar()->showMessage("Open media or a map first");
        return;
    }
    if (editRangesTabActive()) {
        if (!ensureSegmentSelected())
            return;
        m_editOutSpin->setValue(static_cast<int>(m_currentRelMs));
        syncTimeline();
        return;
    }
    m_draftOutSet = true;
    m_draftOutMs = m_currentRelMs;
    updateDraftLabels();
    syncTimeline();
}

void MainWindow::addSegment()
{
    const qint64 dur = sourceDurationMs();
    if (dur <= 0)
        return;
    if (!m_draftInSet || !m_draftOutSet) {
        statusBar()->showMessage("Set both In and Out before adding a range");
        return;
    }
    EditSegment s;
    const qint64 in = qBound<qint64>(0, m_draftInMs, dur);
    const qint64 out = qBound<qint64>(0, m_draftOutMs, dur);
    if (in == out) {
        statusBar()->showMessage("New range needs different In and Out points");
        return;
    }
    s.startMs = qMin(in, out);
    s.endMs = qMax(in, out);
    m_segments.push_back(s);
    m_selectedSegment = m_segments.size() - 1;
    rebuildSegmentTable();
    rebuildEditCombo();
    clearDraftSegment();
    {
        const QSignalBlocker blocker(m_segmentTable);
        m_segmentTable->selectRow(m_selectedSegment);
    }
    statusBar()->showMessage(QString("Added range %1").arg(m_segments.size()));
}

void MainWindow::clearDraftSegment()
{
    const qint64 cur = qBound<qint64>(0, m_currentRelMs, sourceDurationMs());
    m_draftInMs = cur;
    m_draftOutMs = cur;
    m_draftInSet = false;
    m_draftOutSet = false;
    updateDraftLabels();
    syncTimeline();
}

void MainWindow::moveSelectedSegmentUp()
{
    if (m_selectedSegment <= 0 || m_selectedSegment >= m_segments.size())
        return;
    std::swap(m_segments[m_selectedSegment], m_segments[m_selectedSegment - 1]);
    --m_selectedSegment;
    rebuildSegmentTable();
    rebuildEditCombo();
    syncTimeline();
}

void MainWindow::moveSelectedSegmentDown()
{
    if (m_selectedSegment < 0 || m_selectedSegment >= m_segments.size() - 1)
        return;
    std::swap(m_segments[m_selectedSegment], m_segments[m_selectedSegment + 1]);
    ++m_selectedSegment;
    rebuildSegmentTable();
    rebuildEditCombo();
    syncTimeline();
}

void MainWindow::splitSegmentAtPlayhead()
{
    if (!ensureSegmentSelected())
        return;
    const qint64 ph = m_currentRelMs;
    for (int i = 0; i < m_segments.size(); ++i) {
        auto& s = m_segments[i];
        if (ph > s.startMs && ph < s.endMs) {
            EditSegment right{ph, s.endMs};
            s.endMs = ph;
            m_segments.insert(i + 1, right);
            m_selectedSegment = i + 1;
            rebuildSegmentTable();
            rebuildEditCombo();
            selectSegment(m_selectedSegment);
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
    m_selectedSegment = m_segments.isEmpty() ? -1 : qMin(m_selectedSegment, m_segments.size() - 1);
    rebuildSegmentTable();
    rebuildEditCombo();
    selectSegment(m_selectedSegment);
}

bool MainWindow::editRangesTabActive() const
{
    return m_segmentTabs && m_segmentTabs->currentIndex() == 1;
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
