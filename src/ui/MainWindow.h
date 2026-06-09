#pragma once

#include "MmtsEdit.h"
#include "MmtsMap.h"
#include "TimelineWidget.h"

#include <QMainWindow>
#include <QSpinBox>
#include <QTableWidget>
#include <QVector>
#include <memory>

class PreviewWidget;
class MmtsDecodeController;
class QSlider;
class QLabel;
class QPushButton;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    bool openFile(const QString& path);
    bool openMap(const QString& path);
    bool openEdit(const QString& path);
    bool openMedia(const QString& path);

private:
    bool openMediaWithMap(const QString& path, const QString& mapPath);
    void buildMapThenOpen(const QString& path, const QString& mapPath);
    void openMapDialog();
    void openEditDialog();
    void openMediaDialog();
    void saveEditDialog();
    void exportMediaDialog();
    void populateTables();
    void populateTrackTable();
    void populateMptTable();
    void populatePointTable(QTableWidget* table, const QVector<MmtsPoint>& points);

    void showFrameAt(qint64 relMs);
    void stepFrame(int delta);
    void onSeekSliderMoved(int valueMs);
    void updatePositionLabel(qint64 relMs);
    void onFrameReady(const QImage& image, qint64 relMs, quint64 requestId);
    void onDecoderOpened(bool ok, qint64 durationMs, double fps, const QString& error);

    // EDL segment editing.
    void setSegments(const QVector<EditSegment>& segments, int selected);
    void selectSegment(int index);
    void rebuildSegmentTable();
    void syncTimeline();
    void updateSelectedFromSpins();
    bool ensureSegmentSelected();
    void setInToCurrent();
    void setOutToCurrent();
    void addSegment();
    void splitSegmentAtPlayhead();
    void deleteSelectedSegment();
    qint64 sourceDurationMs() const;

    MmtsMap m_map;
    TimelineWidget* m_timeline = nullptr;
    QSpinBox* m_inSpin = nullptr;
    QSpinBox* m_outSpin = nullptr;
    QTableWidget* m_trackTable = nullptr;
    QTableWidget* m_mptTable = nullptr;
    QTableWidget* m_rapTable = nullptr;
    QTableWidget* m_seekTable = nullptr;
    QTableWidget* m_segmentTable = nullptr;

    PreviewWidget* m_preview = nullptr;
    QSlider* m_seekSlider = nullptr;
    QLabel* m_posLabel = nullptr;
    std::unique_ptr<MmtsDecodeController> m_decode;
    QString m_mediaPath;
    qint64 m_currentRelMs = 0;
    double m_fps = 0.0;
    qint64 m_durationMs = 0;
    bool m_sliderDragging = false;
    quint64 m_pendingRequestId = 0;

    QVector<EditSegment> m_segments;
    int m_selectedSegment = -1;
};
