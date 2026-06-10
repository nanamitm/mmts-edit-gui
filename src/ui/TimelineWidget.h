#pragma once

#include "MmtsEdit.h"
#include "MmtsMap.h"

#include <QRect>
#include <QVector>
#include <QWidget>

// Interactive timeline: shows RAP/seek/MPT markers, the EDL cut segments and a
// playhead. Click the lane to seek (and select the segment under the cursor);
// drag the selected segment's In/Out grips to trim (optionally snapping to RAP).
class TimelineWidget : public QWidget {
    Q_OBJECT

public:
    explicit TimelineWidget(QWidget* parent = nullptr);

    void setMap(const MmtsMap& map);
    void setSegments(const QVector<EditSegment>& segments, int selected);
    void setHighlightedSegment(int index);
    void setDraftSegment(const EditSegment& segment, bool visible, bool hasIn = true, bool hasOut = true);
    void setPlayhead(qint64 relMs);
    void setSnapToRap(bool enabled);

signals:
    void seekRequested(qint64 relMs);
    void segmentSelected(int index);
    void inChanged(qint64 relMs);   // selected segment start
    void outChanged(qint64 relMs);  // selected segment end

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    QSize minimumSizeHint() const override;

private:
    enum class Drag { None, Playhead, In, Out };

    QRect laneRect() const;
    int xForTime(qint64 timeMs) const;          // map/relative ms -> x pixel
    qint64 timeForX(int x) const;               // x pixel -> relative ms
    qint64 snapRel(qint64 relMs, int xPixel) const; // snap to nearest RAP if close
    int segmentAtX(int x) const;                // segment index whose block spans x, or -1
    Drag hitTest(const QPoint& pos) const;

    MmtsMap m_map;
    QVector<EditSegment> m_segments;
    EditSegment m_draftSegment;
    bool m_showDraftSegment = false;
    bool m_draftHasIn = true;
    bool m_draftHasOut = true;
    int m_selected = -1;
    int m_highlighted = -1;
    qint64 m_playheadMs = 0;
    bool m_snapToRap = true;
    Drag m_drag = Drag::None;
};
