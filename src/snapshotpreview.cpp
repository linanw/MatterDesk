#include "snapshotpreview.h"

#include <QKeyEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>

SnapshotPreview::SnapshotPreview(QWidget *parent) : QAbstractButton(parent) {
    setObjectName("cameraSnapshot"); setFocusPolicy(Qt::StrongFocus); setCursor(Qt::PointingHandCursor);
    auto policy = QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    policy.setHeightForWidth(true); setSizePolicy(policy);
}
QSize SnapshotPreview::sizeHint() const { return {320, 180}; }
QSize SnapshotPreview::minimumSizeHint() const { return m_fillMode ? QSize(0, 0) : QSize(160, 90); }
int SnapshotPreview::heightForWidth(int width) const { return m_fillMode ? -1 : qRound(width * 9.0 / 16.0); }
void SnapshotPreview::setImage(const QImage &image) { m_image = image; update(); }
bool SnapshotPreview::hasImage() const { return !m_image.isNull(); }
void SnapshotPreview::setFillMode(bool fill) {
    if (m_fillMode == fill) return;
    m_fillMode = fill;
    auto policy = QSizePolicy(QSizePolicy::Expanding, fill ? QSizePolicy::Expanding : QSizePolicy::Fixed);
    policy.setHeightForWidth(!fill); setSizePolicy(policy);
    if (fill) {
        setMinimumHeight(0); setMaximumHeight(QWIDGETSIZE_MAX);
    } else {
        setFixedHeight(heightForWidth(width()));
    }
    updateGeometry(); update();
}
void SnapshotPreview::setPreviewState(bool loading, bool stale, const QString &emptyText) {
    m_loading = loading; m_stale = stale; m_emptyText = emptyText; update();
}
void SnapshotPreview::resizeEvent(QResizeEvent *event) {
    QAbstractButton::resizeEvent(event);
    if (m_fillMode) return;
    const int target = heightForWidth(event->size().width());
    if (minimumHeight() != target || maximumHeight() != target) setFixedHeight(target);
}
void SnapshotPreview::keyPressEvent(QKeyEvent *event) {
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && isEnabled()) {
        click(); event->accept(); return;
    }
    QAbstractButton::keyPressEvent(event);
}
void SnapshotPreview::paintEvent(QPaintEvent *) {
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF area = m_fillMode ? QRectF(rect()) : QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (m_fillMode) {
        painter.setClipRect(area);
    } else {
        QPainterPath clip; clip.addRoundedRect(area, 12, 12);
        painter.setClipPath(clip);
    }
    QLinearGradient background(area.topLeft(), area.bottomRight());
    background.setColorAt(0, QColor("#254a3b")); background.setColorAt(1, QColor("#10291f"));
    painter.fillRect(area, background);
    if (!m_image.isNull()) {
        QRectF source(0, 0, m_image.width(), m_image.height());
        const qreal targetRatio = area.width() / area.height();
        if (source.width() / source.height() > targetRatio) {
            const qreal width = source.height() * targetRatio;
            source.setLeft((m_image.width() - width) / 2); source.setWidth(width);
        } else {
            const qreal height = source.width() / targetRatio;
            source.setTop((m_image.height() - height) / 2); source.setHeight(height);
        }
        painter.drawImage(area, m_image, source);
        QLinearGradient shade(area.topLeft(), area.bottomLeft());
        shade.setColorAt(0, QColor(0, 0, 0, 30)); shade.setColorAt(0.5, QColor(0, 0, 0, 0));
        shade.setColorAt(1, QColor(0, 0, 0, 50)); painter.fillRect(area, shade);
    }
    if (!isEnabled()) painter.fillRect(area, QColor(22, 35, 27, 65));
    else if (underMouse()) painter.fillRect(area, QColor(255, 255, 255, 12));
    // The camera viewer presents its actions and status in native overlays.
    if (m_fillMode) return;
    const QPointF center = area.center() - QPointF(0, m_image.isNull() ? 9 : 0);
    painter.setPen(QPen(QColor(255, 255, 255, isEnabled() ? 110 : 55), 1));
    painter.setBrush(QColor(20, 36, 28, isDown() ? 180 : 125));
    painter.drawEllipse(center, 23, 23);
    QPainterPath play; play.moveTo(center + QPointF(-5, -8));
    play.lineTo(center + QPointF(9, 0)); play.lineTo(center + QPointF(-5, 8)); play.closeSubpath();
    painter.setPen(Qt::NoPen); painter.setBrush(QColor(255, 255, 255, isEnabled() ? 240 : 150)); painter.drawPath(play);
    if (m_image.isNull()) {
        painter.setPen(QColor("#c3d6c8")); QFont font = painter.font(); font.setPixelSize(11); painter.setFont(font);
        painter.drawText(QRectF(area.left() + 12, center.y() + 32, area.width() - 24, 24), Qt::AlignHCenter | Qt::AlignTop, m_emptyText);
    }
    if (!m_fillMode) {
        const QString badge = m_loading ? QStringLiteral("更新中") : m_stale && !m_image.isNull() ? QStringLiteral("旧快照") : QStringLiteral("快照");
        QFont badgeFont = painter.font(); badgeFont.setPixelSize(10); painter.setFont(badgeFont);
        const qreal badgeWidth = painter.fontMetrics().horizontalAdvance(badge) + 16;
        const QRectF badgeRect(area.left() + 10, area.top() + 10, badgeWidth, 21);
        painter.setPen(Qt::NoPen); painter.setBrush(QColor(12, 26, 19, 150)); painter.drawRoundedRect(badgeRect, 6, 6);
        painter.setPen(QColor("#edf5ee")); painter.drawText(badgeRect, Qt::AlignCenter, badge);
    }
    painter.setClipping(false);
    if (hasFocus()) {
        painter.setBrush(Qt::NoBrush); painter.setPen(QPen(QColor("#80ac87"), 2, Qt::DashLine));
        if (m_fillMode) painter.drawRect(area.adjusted(2, 2, -2, -2));
        else painter.drawRoundedRect(area.adjusted(2, 2, -2, -2), 10, 10);
    }
}
