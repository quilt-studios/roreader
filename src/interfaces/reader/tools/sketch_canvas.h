#pragma once
#include <QImage>
#include <QWidget>
#include <QTimer>
#include <QVector>

namespace kobalt::reader {
class SketchCanvas final : public QWidget {
  public:
    explicit SketchCanvas(QWidget *parent = nullptr);
    QImage image{800, 1136, QImage::Format_RGB32};
    bool dirty = false, eraser = false;
    int penWidth = 5;
    QColor ink = Qt::black;
    void undoStroke();
    void redoStroke();
    void clearPaper();
    void replaceImage(const QImage &source);
    bool canUndo() const {
        return !undo.isEmpty();
    }
    bool canRedo() const {
        return !redo.isEmpty();
    }

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void tabletEvent(QTabletEvent *) override;
    bool event(QEvent *) override;

  private:
    QVector<QImage> undo, redo;
    QPointF previous, midpoint;
    bool drawing = false;
    int touchId = -1;
    QTimer frame;
    QRect pendingRegion;
    QRectF paperRect() const;
    QPointF paper(QPointF position) const;
    void remember();
    void begin(QPointF position, qreal pressure = 1);
    void stroke(QPointF position, qreal pressure = 1, bool final = false);
    void finish(QPointF position, qreal pressure = 1);
};
} // namespace kobalt::reader
