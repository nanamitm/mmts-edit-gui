#pragma once

#include "MmtsMap.h"

#include <QWidget>

class TimelineWidget : public QWidget {
    Q_OBJECT

public:
    explicit TimelineWidget(QWidget* parent = nullptr);

    void setMap(const MmtsMap& map);
    void setEditRange(qint64 inMs, qint64 outMs);

protected:
    void paintEvent(QPaintEvent* event) override;
    QSize minimumSizeHint() const override;

private:
    MmtsMap m_map;
    qint64 m_inMs = 0;
    qint64 m_outMs = -1;
};
