/* Настоящий модальный редактор: ввод, минимальный квадрат и уничтожение владельца. */
#include "../src/avatars/avatarcrop.h"
#include <QtTest>
#include <QDialog>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QScopeGuard>
#include <QBuffer>
#include <QFile>
#include <functional>
#include <cmath>
extern "C" {
#include "debug_config.h"
}

static void wheel(QWidget* view, QPoint angle, QPoint pixels = {}) {
    const QPoint center = view->rect().center();
    QWheelEvent event(center, view->mapToGlobal(center), pixels, angle, Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(view, &event);
}

static bool edit(const QString& path, QRectF& crop, QWidget* parent,
                 const std::function<void(QDialog*, QWidget*, QPushButton*)>& action, bool waitForImage = true) {
    QTimer poll, deadline; poll.setInterval(5); deadline.setSingleShot(true); bool timedOut = false;
    QObject::connect(&poll, &QTimer::timeout, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()); if (!dialog) return;
        auto* view = dialog->findChild<QWidget*>("avatarCropView");
        auto* upload = dialog->findChild<QPushButton*>("avatarCropUpload");
        if (!view || !upload || (waitForImage && !upload->isEnabled())) return;
        poll.stop(); action(dialog, view, upload);
    });
    QObject::connect(&deadline, &QTimer::timeout, [&] {
        timedOut = true;
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();
    });
    poll.start(); deadline.start(3000);
    const bool result = selectAvatarCrop(path, crop, parent);
    if (timedOut) QTest::qFail("crop editor did not reach the requested state", __FILE__, __LINE__);
    return result;
}

class AvatarCropTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); }

    void invalidAcceptancePreservesOutput() {
        const QRectF before(.1, .2, .3, .4); QRectF crop = before;
        QVERIFY(!edit(QStringLiteral("/nonexistent/avatar.png"), crop, nullptr,
            [](QDialog* dialog, QWidget*, QPushButton*) { dialog->accept(); }, false));
        QCOMPARE(crop, before);
    }

    void zeroAndHorizontalWheel_data() {
        QTest::addColumn<QPoint>("angle"); QTest::addColumn<QPoint>("pixels");
        QTest::newRow("zero") << QPoint() << QPoint();
        QTest::newRow("horizontal-angle") << QPoint(120, 0) << QPoint();
        QTest::newRow("horizontal-pixels") << QPoint() << QPoint(30, 0);
    }
    void zeroAndHorizontalWheel() {
        QFETCH(QPoint, angle); QFETCH(QPoint, pixels);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("source.png"); QImage image(800, 400, QImage::Format_RGB32); image.fill(Qt::blue);
        QVERIFY(image.save(path)); QRectF crop;
        QVERIFY(edit(path, crop, nullptr, [&](QDialog*, QWidget* view, QPushButton* upload) {
            wheel(view, QPoint(0, 120)); wheel(view, angle, pixels); upload->click();
        }));
        QVERIFY(qAbs(crop.width() - .5 / 1.15) < 1e-8);
        QVERIFY(qAbs(crop.height() - 1.0 / 1.15) < 1e-8);
    }

    void sourceGeometry_data() {
        QTest::addColumn<QSize>("size"); QTest::addColumn<int>("zoomSteps");
        QTest::newRow("wide") << QSize(800, 400) << 0;
        QTest::newRow("portrait") << QSize(400, 800) << 0;
        QTest::newRow("large-zoom") << QSize(800, 400) << 32;
        QTest::newRow("tiny-zoom") << QSize(20, 2) << 32;
        QTest::newRow("thin") << QSize(1, 10000) << 0;
    }
    void sourceGeometry() {
        QFETCH(QSize, size); QFETCH(int, zoomSteps);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("source.png"); QImage image(size, QImage::Format_RGB32); image.fill(Qt::green);
        QVERIFY(image.save(path)); QRectF crop;
        QVERIFY(edit(path, crop, nullptr, [&](QDialog*, QWidget* view, QPushButton* upload) {
            for (int i = 0; i < zoomSteps; i++) wheel(view, QPoint(0, 120));
            const QImage rendered = view->grab().toImage();
            QCOMPARE(rendered.pixelColor(rendered.width()/2, rendered.height()/2), QColor(Qt::green));
            upload->click();
        }));
        QVERIFY(std::isfinite(crop.x()) && std::isfinite(crop.y()) && std::isfinite(crop.width()) && std::isfinite(crop.height()));
        QVERIFY(crop.isValid()); QVERIFY(crop.x() >= -1e-8 && crop.y() >= -1e-8);
        QVERIFY(crop.right() <= 1 + 1e-8 && crop.bottom() <= 1 + 1e-8);
        const double pixelsWide = crop.width() * size.width(), pixelsHigh = crop.height() * size.height();
        QVERIFY(qAbs(pixelsWide - pixelsHigh) < 1e-6);
        QVERIFY(pixelsWide >= 1 - 1e-8);
        if (zoomSteps == 32) QVERIFY(qAbs(pixelsWide - qMin(size.width(), size.height()) / qMin(16.0, double(qMin(size.width(), size.height())))) < 1e-6);
    }

    void cancelPreservesOutput() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("source.png"); QImage image(800, 400, QImage::Format_RGB32); image.fill(Qt::blue);
        QVERIFY(image.save(path)); const QRectF before(.1, .2, .3, .4); QRectF crop = before;
        QVERIFY(!edit(path, crop, nullptr, [](QDialog* dialog, QWidget*, QPushButton*) { dialog->reject(); }));
        QCOMPARE(crop, before);
    }

    void lostLeftButtonStopsDrag() {
        debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_TRACE);
        auto restore = qScopeGuard([] { debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_WARN); });
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("source.png"); QImage image(800, 400, QImage::Format_RGB32); image.fill(Qt::blue);
        QVERIFY(image.save(path)); QRectF crop;
        QVERIFY(edit(path, crop, nullptr, [](QDialog*, QWidget* view, QPushButton* upload) {
            const QPoint center = view->rect().center(); QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, center);
            for (Qt::MouseButtons buttons : {Qt::MouseButtons(), Qt::MouseButtons(Qt::LeftButton)}) {
                const QPoint point = center + QPoint(240, 0);
                QMouseEvent move(QEvent::MouseMove, point, view->mapToGlobal(point), Qt::NoButton, buttons, Qt::NoModifier);
                QApplication::sendEvent(view, &move);
            }
            QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, center + QPoint(240, 0)); upload->click();
        }));
        QVERIFY(qAbs(crop.x() - .25) < 1e-8);
    }

    void parentCanDisappearDuringLoad() {
        QPointer<QWidget> parent = new QWidget;
        const QRectF before(.1, .2, .3, .4); QRectF crop = before;
        QVERIFY(!edit(QStringLiteral("/nonexistent/avatar.png"), crop, parent,
            [&](QDialog*, QWidget*, QPushButton*) { delete parent.data(); }, false));
        QVERIFY(!parent); QCOMPARE(crop, before);
    }

    void selectionUsesExifOrientedCoordinates() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QImage image(800, 400, QImage::Format_RGB32); image.fill(Qt::red);
        QByteArray jpeg; QBuffer buffer(&jpeg); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "JPEG"));
        const QByteArray exif = QByteArray::fromHex("ffe1002245786966000049492a0008000000010012010300010000000600000000000000");
        const QByteArray original = jpeg.left(2) + exif + jpeg.mid(2);
        const QString path = directory.filePath("rotated.jpg"); QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(original), qint64(original.size())); file.close(); QRectF crop;
        QVERIFY(edit(path, crop, nullptr, [](QDialog*, QWidget*, QPushButton* upload) { upload->click(); }));
        QVERIFY(qAbs(crop.x()) < 1e-8 && qAbs(crop.y() - .25) < 1e-8);
        QVERIFY(qAbs(crop.width() - 1) < 1e-8 && qAbs(crop.height() - .5) < 1e-8);
    }
};

QTEST_MAIN(AvatarCropTest)
#include "test_avatar_crop.moc"
