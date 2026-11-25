/* Проверяем время жизни уведомления и первую активность без ввода в настоящую desktop-сессию. */
#include "../src/message_notification.h"
#include "../src/user_activity_monitor.h"
#include "../../../lib/debug_config.h"

#include <QtTest>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QMouseEvent>

#ifdef UTUN_TEST_ACTIVITY_DBUS
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#endif

class FakeSystemActivity : public UserActivityMonitor {
public:
    FakeSystemActivity() { clock.start(); }
    QElapsedTimer clock;
    qint64 lastInput = -5000;
    bool available = true;
protected:
    qint64 systemIdleTime() override { return available ? clock.elapsed() - lastInput : -1; }
};

#ifdef UTUN_TEST_ACTIVITY_DBUS
class FakeGnomeIdle : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.gnome.Mutter.IdleMonitor")
public:
    FakeGnomeIdle() { clock.start(); }
    QElapsedTimer clock;
    qint64 lastInput = -5000;
    int delayMs = 0;
    int requests = 0;
public slots:
    qulonglong GetIdletime() {
        ++requests;
        const qulonglong idle = clock.elapsed() - lastInput;
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "notification test: GNOME request=%d idle=%llu delay_ms=%d dbus=%d",
                   requests, (unsigned long long)idle, delayMs, int(calledFromDBus()));
        if (delayMs) {
            const QDBusMessage reply = message().createReply(QVariant::fromValue(idle));
            const QDBusConnection bus = connection();
            setDelayedReply(true);
            QTimer::singleShot(delayMs, this, [reply, bus] { bus.send(reply); });
        }
        return idle;
    }
};
#endif

class MessageNotificationTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        debug_config_init();
        debug_set_level(DEBUG_LEVEL_DEBUG);
        qunsetenv("XDG_SESSION_TYPE");
    }

    void cleanup() {
        qunsetenv("XDG_SESSION_TYPE");
#ifdef UTUN_TEST_ACTIVITY_DBUS
        QDBusConnection::disconnectFromBus("notification-test-service");
#endif
    }

    void firstSystemActivityAndCancellation() {
        FakeSystemActivity activity;
        QSignalSpy spy(&activity, &UserActivityMonitor::activity);
        activity.start();
        QTest::qWait(600);
        QCOMPARE(spy.size(), 0); // Старый ввод до начала наблюдения не считается активностью.
        activity.lastInput = activity.clock.elapsed();
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 700);
        QTest::qWait(300);
        QCOMPARE(spy.size(), 1);
        activity.start();
        activity.stop();
        activity.lastInput = activity.clock.elapsed();
        QTest::qWait(400);
        QCOMPARE(spy.size(), 1);
        activity.start();
        QTest::qWait(20);
        activity.lastInput = activity.clock.elapsed();
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 2, 700);
    }

    void localFallbackDoesNotConsumeInput() {
        FakeSystemActivity activity;
        activity.available = false;
        QSignalSpy spy(&activity, &UserActivityMonitor::activity);
        QLineEdit input;
        input.show();
        activity.start();
        QTest::keyClick(&input, Qt::Key_A);
        QTest::keyClick(&input, Qt::Key_B);
        QCOMPARE(input.text(), QString("ab"));
        QCOMPARE(spy.size(), 1);
    }

    void waitsForActivityThenExpiresWithoutExtension() {
        QWidget hiddenParent;
        QLineEdit otherWindow;
        otherWindow.show();
        otherWindow.activateWindow();
        QTRY_COMPARE(QApplication::activeWindow(), &otherWindow);
        MessageNotification notification;
        notification.showMessage("42", "Alice", "Test channel", {});
        QVERIFY(notification.isVisible());
        QVERIFY(!notification.parentWidget()); // ОС не должна скрывать уведомление вместе с главным окном.
        QCOMPARE(QApplication::activeWindow(), &otherWindow);
        hiddenParent.showMinimized();
        QVERIFY(notification.isVisible());
        QTest::qWait(10200);
        QVERIFY(notification.isVisible()); // Без активности автоматического таймера нет.
        QElapsedTimer elapsed;
        elapsed.start();
        QTest::keyClick(&otherWindow, Qt::Key_A);
        QTest::qWait(5500);
        QVERIFY(notification.isVisible());
        QTest::keyClick(&otherWindow, Qt::Key_B);
        QTRY_VERIFY_WITH_TIMEOUT(!notification.isVisible(), 6000);
        QVERIFY(elapsed.elapsed() >= 10000);
        QVERIFY(elapsed.elapsed() < 12000);
        QCOMPARE(otherWindow.text(), QString("ab"));
    }

    void replacementCancelsOldCountdownAndOpensLatestChat() {
        QLineEdit otherWindow;
        otherWindow.show();
        MessageNotification notification;
        QSignalSpy activated(&notification, &MessageNotification::activated);
        notification.showMessage("42", "Alice", "First channel", {});
        QTest::keyClick(&otherWindow, Qt::Key_A);
        QTest::qWait(100);
        notification.showMessage("dm:7", "Bob", "Direct messages", QRect(-10, -10, 20, 20));
        QTest::qWait(10200);
        QVERIFY(notification.isVisible()); // Прежний таймер не закрывает новое уведомление.
        const QRect area = notification.screen()->availableGeometry();
        QVERIFY(area.contains(notification.geometry()));
        QTest::mouseClick(&notification, Qt::LeftButton, Qt::NoModifier, QPoint(30, notification.height() - 15));
        QVERIFY(!notification.isVisible());
        QCOMPARE(activated.size(), 1);
        QCOMPARE(activated[0][0].toString(), QString("dm:7"));
        notification.showMessage("43", "Charlie", "Third channel", {});
        auto* closeButton = notification.findChild<QPushButton*>("notificationCloseButton");
        QVERIFY(closeButton);
        QTest::mouseClick(closeButton, Qt::LeftButton);
        QVERIFY(!notification.isVisible());
        QCOMPARE(activated.size(), 1);
    }

    void remappingKeepsControlsUsable() {
        MessageNotification notification;
        QSignalSpy activated(&notification, &MessageNotification::activated);
        notification.showMessage("42", "Alice", "First channel", {});
        notification.showMessage("dm:7", "Bob", "Second channel", {});
        QVERIFY(notification.isVisible());
        auto* closeButton = notification.findChild<QPushButton*>("notificationCloseButton");
        QVERIFY(closeButton);
        QTest::mouseClick(closeButton, Qt::LeftButton);
        QVERIFY(!notification.isVisible());
        notification.showMessage("43", "Charlie", "Third channel", {});
        QVERIFY(notification.isVisible());
        QTest::mouseClick(&notification, Qt::LeftButton, Qt::NoModifier, QPoint(30, notification.height() - 15));
        QVERIFY(!notification.isVisible());
        QCOMPARE(activated.size(), 1);
        QCOMPARE(activated[0][0].toString(), QString("43"));
    }

#ifdef UTUN_TEST_ACTIVITY_DBUS
    void gnomeIdleAndStaleReplies() {
        // Разные соединения исключают прямой вызов внутри Qt, чтобы задержка действительно прошла через D-Bus.
        auto bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "notification-test-service");
        QVERIFY(bus.registerService("org.gnome.Mutter.IdleMonitor"));
        FakeGnomeIdle idle;
        QVERIFY(bus.registerObject("/org/gnome/Mutter/IdleMonitor/Core", &idle, QDBusConnection::ExportAllSlots));
        qputenv("XDG_SESSION_TYPE", "wayland");
        UserActivityMonitor activity;
        QSignalSpy spy(&activity, &UserActivityMonitor::activity);
        activity.start();
        QTRY_VERIFY_WITH_TIMEOUT(idle.requests >= 2, 1000);
        QCOMPARE(spy.size(), 0);
        idle.lastInput = idle.clock.elapsed();
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 1000);

        idle.delayMs = 300;
        activity.start();
        QTest::qWait(20);
        idle.lastInput = idle.clock.elapsed();
        const int before = idle.requests;
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "notification test: waiting for delayed active query after request=%d", before);
        QTRY_VERIFY_WITH_TIMEOUT(idle.requests > before, 1000);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "notification test: cancelling old watch activity_count=%d", int(spy.size()));
        activity.stop();
        idle.lastInput = idle.clock.elapsed() - 5000;
        activity.start();
        QTest::qWait(800);
        QCOMPARE(spy.size(), 1); // Ответ об активности прежнего наблюдения должен быть отброшен.
        activity.stop();
        qunsetenv("XDG_SESSION_TYPE");
        bus.unregisterObject("/org/gnome/Mutter/IdleMonitor/Core");
        bus.unregisterService("org.gnome.Mutter.IdleMonitor");
    }
#endif
};

QTEST_MAIN(MessageNotificationTest)
#include "test_message_notification.moc"
