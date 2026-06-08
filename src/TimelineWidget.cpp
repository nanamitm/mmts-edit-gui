#include "TimelineWidget.h"

#include <QPainter>
#include <QPaintEvent>

TimelineWidget::TimelineWidget(QWidget* parent)
    : QWidget(parent)
{
    setAutoFillBackground(false);
}

void TimelineWidget::setMap(const MmtsMap& map)
{
    m_map = map;
    update();
}

QSize TimelineWidget::minimumSizeHint() const
{
    return QSize(640, 160);
}

void TimelineWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(rect(), QColor(22, 24, 28));

    const QRect lane = rect().adjusted(20, 42, -20, -46);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(52, 57, 65));
    p.drawRoundedRect(lane, 4, 4);

    p.setPen(QColor(225, 229, 235));
    p.drawText(20, 24, m_map.path.isEmpty()
        ? QStringLiteral("Open an .mmtsmap file")
        : QString("%1  %2 ms").arg(m_map.format).arg(m_map.durationMs));

    if (m_map.durationMs <= 0 || lane.width() <= 0)
        return;

    auto xForTime = [&](qint64 timeMs) {
        qint64 relative = timeMs;
        if (m_map.firstVideoPtsMs >= 0 && timeMs >= m_map.firstVideoPtsMs)
            relative = timeMs - m_map.firstVideoPtsMs;
        if (relative < 0)
            relative = 0;
        if (relative > m_map.durationMs)
            relative = m_map.durationMs;
        return lane.left() + static_cast<int>((static_cast<double>(relative) / m_map.durationMs) * lane.width());
    };

    p.setPen(QColor(84, 155, 255, 90));
    for (const auto& point : m_map.rapPoints) {
        const int x = xForTime(point.timeMs);
        p.drawLine(x, lane.top(), x, lane.bottom());
    }

    p.setPen(QPen(QColor(116, 214, 143), 2));
    for (const auto& point : m_map.seekPoints) {
        const int x = xForTime(point.timeMs);
        p.drawLine(x, lane.top(), x, lane.bottom());
    }

    p.setPen(QPen(QColor(255, 196, 87), 3));
    for (const auto& change : m_map.mptChanges) {
        const int x = xForTime(change.timeMs);
        p.drawLine(x, lane.top() - 8, x, lane.bottom() + 8);
    }

    p.setPen(QColor(196, 202, 211));
    const QString summary = QString("tracks %1   mpt %2   rap %3   seek %4")
        .arg(m_map.tracks.size())
        .arg(m_map.mptChanges.size())
        .arg(m_map.rapPoints.size())
        .arg(m_map.seekPoints.size());
    p.drawText(20, height() - 18, summary);
}
