#pragma once

#include "MmtsMap.h"

#include <QWidget>

class TimelineWidget : public QWidget {
    Q_OBJECT

public:
    explicit TimelineWidget(QWidget* parent = nullptr);

    void setMap(const MmtsMap& map);

protected:
    void paintEvent(QPaintEvent* event) override;
    QSize minimumSizeHint() const override;

private:
    MmtsMap m_map;
};
