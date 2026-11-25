#include "avatarcrop.h"
#include "avatarcache.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QImageReader>
#include <QImageIOHandler>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QRunnable>
#include <QThreadPool>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
extern "C" {
#include "debug_config.h"
}

namespace {
class CropView : public QWidget {
public:
    QImage image;
    QSize originalSize;
    explicit CropView(QWidget* parent) : QWidget(parent) {
        setMinimumSize(320, 320); setMouseTracking(true); setCursor(Qt::OpenHandCursor);
    }
    QRectF frame() const { const double side = qMin(width(), height()) * 0.8; return {(width()-side)/2, (height()-side)/2, side, side}; }
    double scale() const { return qMax(frame().width()/originalSize.width(), frame().height()/originalSize.height()) * zoom; }
    void clamp() {
        if (image.isNull()) return;
        const double s = scale();
        const double mx = qMax(0.0, (originalSize.width()*s-frame().width())/2);
        const double my = qMax(0.0, (originalSize.height()*s-frame().height())/2);
        offset = {qBound(-mx, offset.x(), mx), qBound(-my, offset.y(), my)};
    }
    QRectF selection() {
        clamp();
        const double s = scale();
        const QPointF top = QPointF(width()/2.0,height()/2.0) + offset - QPointF(originalSize.width()*s/2, originalSize.height()*s/2);
        const QRectF f = frame();
        return {(f.x()-top.x())/s/originalSize.width(), (f.y()-top.y())/s/originalSize.height(),
            f.width()/s/originalSize.width(), f.height()/s/originalSize.height()};
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.fillRect(rect(), QColor(20,20,22)); if (image.isNull()) return;
        clamp(); const double s = scale(); const QSizeF size(originalSize.width()*s, originalSize.height()*s);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF photo(QPointF(width()/2.0,height()/2.0)+offset-QPointF(size.width()/2,size.height()/2), size);
        const QRectF visible = photo.intersected(rect());
        const QRectF sample((visible.x()-photo.x())/photo.width()*image.width(), (visible.y()-photo.y())/photo.height()*image.height(),
                            visible.width()/photo.width()*image.width(), visible.height()/photo.height()*image.height());
        p.drawImage(visible, image, sample);
        QPainterPath mask; mask.addRect(rect()); mask.addEllipse(frame()); p.fillPath(mask, QColor(0,0,0,150));
        p.setPen(QPen(Qt::white, 2)); p.drawEllipse(frame());
    }
    void wheelEvent(QWheelEvent* e) override {
        const int delta = e->angleDelta().y() ? e->angleDelta().y() : e->pixelDelta().y();
        if (image.isNull() || !delta) { e->ignore(); return; }
        const double previous = zoom;
        const double maximum = qMin(16.0, double(qMin(originalSize.width(), originalSize.height())));
        zoom = qBound(1.0, zoom * (delta > 0 ? 1.15 : 1/1.15), maximum);
        offset *= zoom/previous; clamp(); update(); e->accept();
    }
    void mousePressEvent(QMouseEvent* e) override {
        DEBUG_TRACE(DEBUG_CATEGORY_MEDIA, "avatar crop press: button=%x image=%d", unsigned(e->button()), !image.isNull());
        if (e->button() == Qt::LeftButton && !image.isNull()) { last = e->pos(); dragging = true; setCursor(Qt::ClosedHandCursor); }
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        DEBUG_TRACE(DEBUG_CATEGORY_MEDIA, "avatar crop move: dragging=%d buttons=%x x=%d y=%d",
                    dragging, unsigned(e->buttons()), e->pos().x(), e->pos().y());
        if (!(e->buttons() & Qt::LeftButton)) { dragging = false; setCursor(Qt::OpenHandCursor); }
        if (dragging) { offset += e->pos()-last; last = e->pos(); clamp(); update(); }
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        DEBUG_TRACE(DEBUG_CATEGORY_MEDIA, "avatar crop release: button=%x dragging=%d", unsigned(e->button()), dragging);
        if (e->button() == Qt::LeftButton) { dragging = false; setCursor(Qt::OpenHandCursor); }
    }
private:
    double zoom = 1; QPointF offset; QPoint last; bool dragging = false;
};
}

bool selectAvatarCrop(const QString& source, QRectF& crop, QWidget* parent) {
    QPointer<QDialog> dialog = new QDialog(parent);
    auto release = qScopeGuard([&] { delete dialog.data(); });
    dialog->setWindowTitle(QCoreApplication::translate("ChatGui", "Crop avatar")); dialog->resize(620,650);
    auto* layout = new QVBoxLayout(dialog); auto* view = new CropView(dialog); layout->addWidget(view, 1);
    view->setObjectName("avatarCropView");
    auto* status = new QLabel(QCoreApplication::translate("ChatGui", "Drag photo and use the wheel to fit the circle"), dialog);
    layout->addWidget(status); auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QCoreApplication::translate("ChatGui", "Upload avatar"));
    buttons->button(QDialogButtonBox::Ok)->setObjectName("avatarCropUpload");
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false); layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    // Получатель живёт до завершения worker, независимо от уничтожения окна/parent.
    QObject completion;
    QThreadPool pool; pool.setMaxThreadCount(1);
    pool.start(QRunnable::create([source, &completion, dialog, view, status, buttons] {
        QImageReader reader(source); reader.setAutoTransform(true); const QSize size = reader.size(); QImage image;
        QString error;
        const qint64 sourceBytes = QFileInfo(source).size(); const QByteArray format = reader.format();
        QSize orientedSize=size;
        if (reader.transformation() & QImageIOHandler::TransformationRotate90) orientedSize.transpose();
        if (sourceBytes < 1 || sourceBytes > AvatarCache::OriginalMaxBytes) error = QStringLiteral("source must contain 1..33554432 bytes");
        else if (size.width() < 1 || size.height() < 1) error = reader.errorString();
        else if (qint64(size.width())*size.height() > 16*1024*1024) error = QStringLiteral("source exceeds 16 megapixels");
        else if (format != "jpeg" && format != "png" && format != "webp") error = QStringLiteral("unsupported image format");
        else {
            if (qMax(size.width(), size.height()) > 1600) {
                const QSize preview = size.scaled(1600, 1600, Qt::KeepAspectRatio);
                reader.setScaledSize({qMax(1, preview.width()), qMax(1, preview.height())});
            }
            image = reader.read(); if (image.isNull()) error = reader.errorString();
        }
        if (image.isNull()) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "avatar: editor decode failed source=%s bytes=%lld original=%dx%d format=%s reason=%s",
            qPrintable(source), (long long)sourceBytes, size.width(), size.height(), format.constData(), qPrintable(error));
        if (!QMetaObject::invokeMethod(&completion, [dialog, view, image, orientedSize, status, buttons] {
            if (!dialog) return;
            if (image.isNull()) {
                status->setText(QCoreApplication::translate("ChatGui", "Cannot load avatar image")); return;
            }
            view->image=image; view->originalSize=orientedSize; view->update(); buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
            DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "avatar: editor loaded original=%dx%d preview=%dx%d mask=circle",
                orientedSize.width(), orientedSize.height(), image.width(), image.height());
        }, Qt::QueuedConnection)) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "avatar: cannot queue editor preview source=%s", qPrintable(source));
    }));
    const int result = dialog->exec(); pool.waitForDone();
    if (!dialog || result != QDialog::Accepted) { DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "avatar: crop cancelled owner_gone=%d", !dialog); return false; }
    if (view->image.isNull() || view->originalSize.width() <= 0 || view->originalSize.height() <= 0) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "avatar: crop acceptance rejected before preview loaded source=%s", qPrintable(source)); return false;
    }
    const QRectF selected = view->selection().intersected(QRectF(0, 0, 1, 1));
    if (!selected.isValid() || !std::isfinite(selected.x()) || !std::isfinite(selected.y())
        || !std::isfinite(selected.width()) || !std::isfinite(selected.height())
        || selected.width()*view->originalSize.width() < 1-1e-6
        || qAbs(selected.width()*view->originalSize.width()-selected.height()*view->originalSize.height()) > 1e-6) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "avatar: invalid editor selection original=%dx%d x=%g y=%g w=%g h=%g",
                    view->originalSize.width(), view->originalSize.height(), selected.x(), selected.y(), selected.width(), selected.height()); return false;
    }
    crop = selected; DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "avatar: crop selected x=%.5f y=%.5f w=%.5f h=%.5f",
        crop.x(), crop.y(), crop.width(), crop.height()); return true;
}
