#include "TimelineWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <algorithm>

namespace {
constexpr int kHandleGrabPx = 7; // px tolerance for grabbing an In/Out grip
constexpr int kSnapPx = 8;       // px within which a drag snaps to a RAP
}

TimelineWidget::TimelineWidget(QWidget* parent)
    : QWidget(parent)
{
    setAutoFillBackground(false);
    setMouseTracking(true);
}

void TimelineWidget::setMap(const MmtsMap& map)
{
    m_map = map;
    m_playheadMs = 0;
    update();
}

void TimelineWidget::setSegments(const QVector<EditSegment>& segments, int selected)
{
    m_segments = segments;
    m_selected = selected;
    update();
}

void TimelineWidget::setHighlightedSegment(int index)
{
    m_highlighted = index;
    update();
}

void TimelineWidget::setDraftSegment(const EditSegment& segment, bool visible, bool hasIn, bool hasOut)
{
    m_draftSegment = segment;
    m_showDraftSegment = visible;
    m_draftHasIn = hasIn;
    m_draftHasOut = hasOut;
    update();
}

void TimelineWidget::setPlayhead(qint64 relMs)
{
    m_playheadMs = relMs;
    update();
}

void TimelineWidget::setSnapToRap(bool enabled)
{
    m_snapToRap = enabled;
}

QRect TimelineWidget::laneRect() const
{
    return rect().adjusted(20, 42, -20, -46);
}

int TimelineWidget::xForTime(qint64 timeMs) const
{
    const QRect lane = laneRect();
    if (m_map.durationMs <= 0 || lane.width() <= 0)
        return lane.left();

    qint64 relative = timeMs;
    // Map markers (rap/seek/mpt) carry absolute PTS ms; segment/playhead times
    // are already relative. Normalise the absolute ones to 0-based.
    if (m_map.firstVideoPtsMs >= 0 && timeMs >= m_map.firstVideoPtsMs)
        relative = timeMs - m_map.firstVideoPtsMs;
    relative = std::clamp<qint64>(relative, 0, m_map.durationMs);
    return lane.left() + static_cast<int>((static_cast<double>(relative) / m_map.durationMs) * lane.width());
}

qint64 TimelineWidget::timeForX(int x) const
{
    const QRect lane = laneRect();
    if (lane.width() <= 0 || m_map.durationMs <= 0)
        return 0;
    const double frac = static_cast<double>(x - lane.left()) / lane.width();
    return std::clamp<qint64>(static_cast<qint64>(frac * m_map.durationMs), 0, m_map.durationMs);
}

qint64 TimelineWidget::snapRel(qint64 relMs, int xPixel) const
{
    if (!m_snapToRap || m_map.firstVideoPtsMs < 0)
        return relMs;

    qint64 bestRel = relMs;
    int bestDist = kSnapPx + 1;
    for (const auto& p : m_map.rapPoints) {
        const qint64 rel = p.timeMs - m_map.firstVideoPtsMs;
        if (rel < 0 || rel > m_map.durationMs)
            continue;
        const int dist = std::abs(xForTime(p.timeMs) - xPixel);
        if (dist < bestDist) {
            bestDist = dist;
            bestRel = rel;
        }
    }
    return bestRel;
}

int TimelineWidget::segmentAtX(int x) const
{
    for (int i = 0; i < m_segments.size(); ++i) {
        const int a = xForTime(m_segments[i].startMs);
        const int b = xForTime(m_segments[i].endMs);
        if (x >= std::min(a, b) && x <= std::max(a, b))
            return i;
    }
    return -1;
}

TimelineWidget::Drag TimelineWidget::hitTest(const QPoint& pos) const
{
    if (m_map.durationMs <= 0)
        return Drag::None;
    if (m_selected >= 0 && m_selected < m_segments.size()) {
        const int inX = xForTime(m_segments[m_selected].startMs);
        const int outX = xForTime(m_segments[m_selected].endMs);
        const int dIn = std::abs(pos.x() - inX);
        const int dOut = std::abs(pos.x() - outX);
        if (dIn <= kHandleGrabPx && dIn <= dOut)
            return Drag::In;
        if (dOut <= kHandleGrabPx)
            return Drag::Out;
    } else if (m_showDraftSegment) {
        const int inX = xForTime(m_draftSegment.startMs);
        const int outX = xForTime(m_draftSegment.endMs);
        const int dIn = std::abs(pos.x() - inX);
        const int dOut = std::abs(pos.x() - outX);
        if (m_draftHasIn && dIn <= kHandleGrabPx && dIn <= dOut)
            return Drag::In;
        if (m_draftHasOut && dOut <= kHandleGrabPx)
            return Drag::Out;
    }
    if (laneRect().adjusted(0, -20, 0, 20).contains(pos))
        return Drag::Playhead;
    return Drag::None;
}

void TimelineWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || m_map.durationMs <= 0)
        return;
    m_drag = hitTest(event->pos());
    if (m_drag == Drag::Playhead) {
        // A plain click also selects the segment under the cursor.
        const int seg = segmentAtX(event->pos().x());
        if (seg >= 0 && seg != m_selected)
            emit segmentSelected(seg);
    }
    mouseMoveEvent(event); // apply immediately on press
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_drag == Drag::None) {
        const Drag over = hitTest(event->pos());
        setCursor((over == Drag::In || over == Drag::Out) ? Qt::SplitHCursor : Qt::ArrowCursor);
        return;
    }

    const int x = event->pos().x();
    const qint64 rel = timeForX(x);
    switch (m_drag) {
    case Drag::Playhead:
        m_playheadMs = rel;
        update();
        emit seekRequested(rel);
        break;
    case Drag::In:
        emit inChanged(snapRel(rel, x));
        break;
    case Drag::Out:
        emit outChanged(snapRel(rel, x));
        break;
    case Drag::None:
        break;
    }
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* event)
{
    Q_UNUSED(event);
    m_drag = Drag::None;
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

    const QRect lane = laneRect();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(52, 57, 65));
    p.drawRoundedRect(lane, 4, 4);

    p.setPen(QColor(225, 229, 235));
    p.drawText(20, 24, m_map.path.isEmpty()
        ? QStringLiteral("Open an .mmtsmap file")
        : QString("%1  %2 ms").arg(m_map.format).arg(m_map.durationMs));

    if (m_map.durationMs <= 0 || lane.width() <= 0)
        return;

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

    // EDL segment blocks.
    auto drawHandles = [&](int a, int b, bool hasIn = true, bool hasOut = true) {
        auto drawHandle = [&](int x, const QColor& color) {
            p.setPen(QPen(color, 3));
            p.drawLine(x, lane.top() - 18, x, lane.bottom() + 18);
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            const QPoint top(x, lane.top() - 18);
            p.drawPolygon(QPolygon({ top + QPoint(-5, -6), top + QPoint(5, -6), top + QPoint(0, 2) }));
        };
        if (hasIn)
            drawHandle(a, QColor(255, 92, 92));
        if (hasOut)
            drawHandle(b, QColor(255, 126, 58));
    };

    if (m_showDraftSegment && (m_selected < 0 || m_selected >= m_segments.size())) {
        const int a = xForTime(m_draftSegment.startMs);
        const int b = xForTime(m_draftSegment.endMs);
        if (m_draftHasIn && m_draftHasOut && a != b) {
            const QRect block = QRect(QPoint(a, lane.top()), QPoint(b, lane.bottom())).normalized();
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 126, 58, 45));
            p.drawRect(block);
        }
        drawHandles(a, b, m_draftHasIn, m_draftHasOut);
    }

    for (int i = 0; i < m_segments.size(); ++i) {
        const int a = xForTime(m_segments[i].startMs);
        const int b = xForTime(m_segments[i].endMs);
        const QRect block = QRect(QPoint(a, lane.top()), QPoint(b, lane.bottom())).normalized();
        const bool sel = (i == m_selected);
        const bool hi = (i == m_highlighted);
        p.setPen(Qt::NoPen);
        p.setBrush(sel ? QColor(255, 126, 58, 70)
                       : (hi ? QColor(120, 160, 220, 95) : QColor(120, 160, 220, 45)));
        p.drawRect(block);
        if (hi && !sel) {
            p.setPen(QPen(QColor(170, 205, 255), 2));
            p.setBrush(Qt::NoBrush);
            p.drawRect(block.adjusted(0, 0, -1, -1));
        }
        // index label
        p.setPen(QColor(230, 234, 240));
        p.drawText(block.adjusted(3, 0, -3, 0), Qt::AlignLeft | Qt::AlignVCenter, QString::number(i + 1));

        if (sel) {
            drawHandles(a, b);
        }
    }

    // Playhead.
    const int phX = xForTime(m_playheadMs);
    p.setPen(QPen(QColor(240, 240, 245), 1));
    p.drawLine(phX, lane.top() - 22, phX, lane.bottom() + 22);
    p.setBrush(QColor(240, 240, 245));
    p.setPen(Qt::NoPen);
    const QPoint phTop(phX, lane.top() - 22);
    p.drawPolygon(QPolygon({ phTop + QPoint(-4, -5), phTop + QPoint(4, -5), phTop + QPoint(0, 3) }));

    p.setPen(QColor(196, 202, 211));
    const QString summary = QString("segments %1   tracks %2   rap %3   seek %4")
        .arg(m_segments.size())
        .arg(m_map.tracks.size())
        .arg(m_map.rapPoints.size())
        .arg(m_map.seekPoints.size());
    p.drawText(20, height() - 18, summary);
}
