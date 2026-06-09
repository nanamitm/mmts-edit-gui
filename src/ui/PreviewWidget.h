#pragma once

#include <QImage>
#include <QWidget>

// Displays a decoded video frame, scaled to fit while preserving aspect ratio.
class PreviewWidget : public QWidget {
    Q_OBJECT
public:
    explicit PreviewWidget(QWidget* parent = nullptr);

    void setFrame(const QImage& image);
    void clearFrame();

    QSize minimumSizeHint() const override { return QSize(480, 270); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage m_image;
};
