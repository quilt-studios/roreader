#include "sketch_canvas.h"
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QTabletEvent>
#include <QTouchEvent>
#include <cmath>

namespace kobalt::reader {
SketchCanvas::SketchCanvas(QWidget *parent) : QWidget(parent) {
    setObjectName("rrSketchCanvas");
    setAttribute(Qt::WA_AcceptTouchEvents);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(120, 160);
    image.fill(Qt::white);
    frame.setSingleShot(true);
    frame.setInterval(16);
    connect(&frame, &QTimer::timeout, this, [this] {
        const auto area = pendingRegion;
        pendingRegion = {};
        update(area);
    });
}
QRectF SketchCanvas::paperRect() const {
    const QSizeF size = QSizeF(image.size()).scaled(this->size(), Qt::KeepAspectRatio);
    return {(width() - size.width()) / 2, (height() - size.height()) / 2, size.width(), size.height()};
}
QPointF SketchCanvas::paper(QPointF position) const {
    const auto bounds = paperRect();
    return {qBound(0.0, (position.x() - bounds.left()) * image.width() / qMax(1.0, bounds.width()),
                   qreal(image.width() - 1)),
            qBound(0.0, (position.y() - bounds.top()) * image.height() / qMax(1.0, bounds.height()),
                   qreal(image.height() - 1))};
}
void SketchCanvas::remember() {
    if (undo.size() == 4)
        undo.removeFirst();
    undo.push_back(image);
    redo.clear();
}
void SketchCanvas::begin(QPointF position, qreal pressure) {
    if (!paperRect().contains(position))
        return;
    remember();
    drawing = true;
    previous = midpoint = paper(position);
    stroke(position, pressure);
}
void SketchCanvas::stroke(QPointF position, qreal pressure, bool final) {
    if (!drawing)
        return;
    const auto next = paper(position);
    const auto middle = final ? next : (previous + next) / 2;
    const qreal size = eraser ? qMax(24, penWidth * 5) : qMax(1.0, penWidth * qBound(0.25, pressure, 1.0));
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(eraser ? Qt::white : ink, size, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QPainterPath path(midpoint);
    path.quadTo(previous, middle);
    painter.drawPath(path);
    if (next == previous)
        painter.drawPoint(next);
    const auto dirtyBounds =
        path.controlPointRect().united(QRectF(previous, next).normalized()).adjusted(-size, -size, size, size);
    const auto bounds = paperRect();
    const qreal sx = bounds.width() / image.width(), sy = bounds.height() / image.height();
    const QRect area = QRectF(bounds.left() + dirtyBounds.left() * sx, bounds.top() + dirtyBounds.top() * sy,
                              dirtyBounds.width() * sx, dirtyBounds.height() * sy)
                           .toAlignedRect()
                           .adjusted(-2, -2, 2, 2);
    pendingRegion |= area;
    if (!frame.isActive())
        frame.start();
    previous = next;
    midpoint = middle;
    dirty = true;
}
void SketchCanvas::finish(QPointF position, qreal pressure) {
    stroke(position, pressure, true);
    drawing = false;
    touchId = -1;
    frame.stop();
    update(pendingRegion);
    pendingRegion = {};
}
void SketchCanvas::undoStroke() {
    drawing = false;
    if (undo.isEmpty())
        return;
    redo.push_back(image);
    image = undo.takeLast();
    dirty = true;
    update();
}
void SketchCanvas::redoStroke() {
    drawing = false;
    if (redo.isEmpty())
        return;
    undo.push_back(image);
    image = redo.takeLast();
    dirty = true;
    update();
}
void SketchCanvas::clearPaper() {
    remember();
    image.fill(Qt::white);
    dirty = true;
    update();
}
void SketchCanvas::replaceImage(const QImage &source) {
    remember();
    image.fill(Qt::white);
    QPainter painter(&image);
    const auto size = source.size().scaled(image.size(), Qt::KeepAspectRatio);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(QRect(QPoint((image.width() - size.width()) / 2, (image.height() - size.height()) / 2), size),
                      source);
    dirty = false;
    update();
}
void SketchCanvas::paintEvent(QPaintEvent *event) {
    QPainter painter(this);
    painter.setClipRegion(event->region());
    painter.fillRect(rect(), palette().color(QPalette::Midlight));
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(paperRect(), image);
}
void SketchCanvas::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && touchId < 0)
        begin(event->pos());
}
void SketchCanvas::mouseMoveEvent(QMouseEvent *event) {
    if (touchId < 0)
        stroke(event->pos());
}
void SketchCanvas::mouseReleaseEvent(QMouseEvent *event) {
    if (touchId < 0 && event->button() == Qt::LeftButton)
        finish(event->pos());
}
void SketchCanvas::tabletEvent(QTabletEvent *event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const auto position = event->position();
#else
    const auto position = event->posF();
#endif
    if (event->type() == QEvent::TabletPress)
        begin(position, event->pressure());
    else if (event->type() == QEvent::TabletMove)
        stroke(position, event->pressure());
    else if (event->type() == QEvent::TabletRelease)
        finish(position, event->pressure());
    event->accept();
}
bool SketchCanvas::event(QEvent *event) {
    if (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchUpdate ||
        event->type() == QEvent::TouchEnd) {
        auto *touch = static_cast<QTouchEvent *>(event);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const auto points = touch->points();
#else
        const auto points = touch->touchPoints();
#endif
        for (const auto &point : points) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            const auto position = point.position();
#else
            const auto position = point.pos();
#endif
            if (event->type() == QEvent::TouchBegin && touchId < 0) {
                begin(position, point.pressure());
                touchId = point.id();
            }
            if (point.id() != touchId)
                continue;
            if (event->type() == QEvent::TouchEnd)
                finish(position, point.pressure());
            else
                stroke(position, point.pressure());
            break;
        }
        if (event->type() == QEvent::TouchEnd) {
            drawing = false;
            touchId = -1;
        }
        event->accept();
        return true;
    }
    if (event->type() == QEvent::TouchCancel) {
        drawing = false;
        touchId = -1;
    }
    return QWidget::event(event);
}
} // namespace kobalt::reader
