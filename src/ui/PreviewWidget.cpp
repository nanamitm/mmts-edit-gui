#include "PreviewWidget.h"

#include <QPainter>

PreviewWidget::PreviewWidget(QWidget* parent)
    : QWidget(parent)
{
    setAutoFillBackground(false);
}

void PreviewWidget::setFrame(const QImage& image)
{
    m_image = image;
    update();
}

void PreviewWidget::clearFrame()
{
    m_image = QImage();
    update();
}

void PreviewWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(12, 13, 16));

    if (m_image.isNull()) {
        p.setPen(QColor(150, 156, 165));
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("No preview\nOpen a .mmts media file"));
        return;
    }

    const QSize scaled = m_image.size().scaled(size(), Qt::KeepAspectRatio);
    const QRect target(QPoint((width() - scaled.width()) / 2,
                              (height() - scaled.height()) / 2),
                       scaled);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage(target, m_image);
}
