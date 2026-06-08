#pragma once

#include "MmtsMap.h"
#include "TimelineWidget.h"

#include <QMainWindow>
#include <QSpinBox>
#include <QTableWidget>

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    bool openMap(const QString& path);

private:
    void openMapDialog();
    void saveEditDialog();
    void updateEditRange();
    void populateTables();
    void populateTrackTable();
    void populateMptTable();
    void populatePointTable(QTableWidget* table, const QVector<MmtsPoint>& points);

    MmtsMap m_map;
    TimelineWidget* m_timeline = nullptr;
    QSpinBox* m_inSpin = nullptr;
    QSpinBox* m_outSpin = nullptr;
    QTableWidget* m_trackTable = nullptr;
    QTableWidget* m_mptTable = nullptr;
    QTableWidget* m_rapTable = nullptr;
    QTableWidget* m_seekTable = nullptr;
};
