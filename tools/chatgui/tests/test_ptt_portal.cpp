#include "../src/ptt_portal.h"
#include "../../../lib/debug_config.h"
#include <QtTest>
#include <QDBusAbstractAdaptor>
#include <QDBusContext>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QCoreApplication>

static const QString service = "org.freedesktop.portal.Desktop";
static const QString root = "/org/freedesktop/portal/desktop";

class FakeRequest : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Request")
public:
    using QObject::QObject;
public slots:
    void Close() {}
signals:
    void Response(uint code, const QVariantMap& results);
};
class FakeSession : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Session")
public:
    using QObject::QObject;
public slots:
    void Close() { emit Closed({}); }
signals:
    void Closed(const QVariantMap& details);
};
class FakePortal : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.GlobalShortcuts")
    Q_PROPERTY(uint version READ version)
public:
    uint version() const { return 2; }
    uint bindResponse = 0;
    int bindCount = 0, configureCount = 0;
    QString session;
    PttPortalShortcuts shortcuts;
    FakeSession* sessionObject = nullptr;
    QDBusObjectPath answer(const QVariantMap& options, uint code, const QVariantMap& results) {
        QString sender = message().service().mid(1).replace('.', '_');
        QString path = root + "/request/" + sender + "/" + options.value("handle_token").toString();
        auto* request = new FakeRequest(this);
        QDBusConnection::sessionBus().registerObject(path, request, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals);
        QTimer::singleShot(0, request, [request, code, results]() { emit request->Response(code, results); });
        return QDBusObjectPath(path);
    }
public slots:
    QDBusObjectPath CreateSession(const QVariantMap& options) {
        QString sender = message().service().mid(1).replace('.', '_');
        session = root + "/session/" + sender + "/" + options.value("session_handle_token").toString();
        QDBusConnection::sessionBus().unregisterObject(session);
        sessionObject = new FakeSession(this);
        QDBusConnection::sessionBus().registerObject(session, sessionObject, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals);
        return answer(options, 0, {{"session_handle", session}});
    }
    QDBusObjectPath ListShortcuts(const QDBusObjectPath&, const QVariantMap& options) {
        return answer(options, 0, {{"shortcuts", QVariant::fromValue(shortcuts)}});
    }
    QDBusObjectPath BindShortcuts(const QDBusObjectPath&, const PttPortalShortcuts& requested,
                                  const QString&, const QVariantMap& options) {
        bindCount++;
        if (bindResponse == 0) {
            shortcuts = requested;
            for (int i = 0; i < shortcuts.size(); i++) shortcuts[i].properties["trigger_description"] = QString("F%1").arg(8 + i);
        }
        return answer(options, bindResponse, {{"shortcuts", QVariant::fromValue(shortcuts)}});
    }
    void ConfigureShortcuts(const QDBusObjectPath&, const QString&, const QVariantMap&) { configureCount++; }
signals:
    void Activated(const QDBusObjectPath& session, const QString& id, qulonglong timestamp, const QVariantMap& options);
    void Deactivated(const QDBusObjectPath& session, const QString& id, qulonglong timestamp, const QVariantMap& options);
    void ShortcutsChanged(const QDBusObjectPath& session, const PttPortalShortcuts& shortcuts);
};

class PortalTest : public QObject {
    Q_OBJECT
private slots:
    void lifecycle() {
        qDBusRegisterMetaType<PttPortalShortcut>(); qDBusRegisterMetaType<PttPortalShortcuts>();
        FakePortal fake;
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.registerService(service));
        QVERIFY(bus.registerObject(root, &fake, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals | QDBusConnection::ExportAllProperties));
        {
            PttPortal portal(nullptr, "utun_ptt_test");
            QSignalSpy talking(&portal, &PttPortal::talkingChanged);
            portal.initialize();
            QTRY_VERIFY(!portal.busy());
            QVERIFY(!fake.session.isEmpty());
            QCOMPARE(fake.bindCount, 0); // при первом запуске не открываем системный диалог
            portal.configure("x11:123");
            QTRY_COMPARE(fake.bindCount, 1);
            QTRY_COMPARE(portal.description(), QString("F8"));
            QCOMPARE(fake.shortcuts.size(), 1);
            QCOMPARE(fake.shortcuts[0].id, QString("ptt"));
            QTRY_VERIFY(!portal.busy());
            QDBusObjectPath session(fake.session);
            emit fake.Activated(QDBusObjectPath("/unrelated"), "ptt", 0, {});
            emit fake.Activated(session, "other_action", 0, {});
            emit fake.Activated(session, "ptt", 1, {});
            QTRY_COMPARE(talking.size(), 1);
            QCOMPARE(talking.last()[0].toBool(), true);
            emit fake.Activated(session, "ptt", 2, {}); // повторное событие не создаёт второй start
            emit fake.Activated(session, "ptt2", 3, {});
            emit fake.Deactivated(session, "ptt", 4, {});
            emit fake.Deactivated(session, "ptt2", 5, {});
            QTRY_COMPARE(talking.size(), 2);
            QCOMPARE(talking.last()[0].toBool(), false); // старые/чужие ID не активируют передачу
            QSignalSpy resets(&portal, &PttPortal::resetRequested);
            portal.setInhibited(true);
            QCOMPARE(resets.size(), 1); // сброс нужен также при фиксации после отпускания клавиши
            emit fake.Activated(session, "ptt", 6, {});
            emit fake.Deactivated(session, "ptt", 7, {});
            QTest::qWait(20);
            QCOMPARE(talking.size(), 2);
            portal.setInhibited(false);
            portal.configure("x11:123");
            QTRY_COMPARE(fake.configureCount, 1); // изменение через Configure, без повторного Bind
            fake.shortcuts[0].properties["trigger_description"] = "Ctrl+F8";
            emit fake.ShortcutsChanged(session, fake.shortcuts);
            QTRY_COMPARE(portal.description(), QString("Ctrl+F8"));
            QTRY_COMPARE(resets.size(), 2);
            emit fake.Activated(session, "ptt", 8, {});
            QTRY_COMPARE(talking.size(), 3);
            emit fake.sessionObject->Closed({});
            QTRY_COMPARE(talking.size(), 4);
            QCOMPARE(talking.last()[0].toBool(), false);
            QTRY_COMPARE(resets.size(), 3);
            QVERIFY(portal.description().isEmpty());
            fake.bindResponse = 1;
            portal.configure("");
            QTRY_COMPARE(fake.bindCount, 2);
            QTRY_VERIFY(!portal.busy());
            QVERIFY(portal.description().isEmpty()); // отмена не оставляет активную сессию
            fake.bindResponse = 0;
            portal.configure("");
            QTRY_COMPARE(fake.bindCount, 3);
            QTRY_COMPARE(fake.configureCount, 2); // сохранённые сочетания восстанавливаются, затем открывается настройка
            QTRY_COMPARE(portal.description(), QString("F8"));
            emit fake.Activated(QDBusObjectPath(fake.session), "ptt", 9, {});
            QTRY_COMPARE(talking.size(), 5);
            QVERIFY(bus.unregisterService(service));
            QTRY_COMPARE(talking.size(), 6);
            QCOMPARE(talking.last()[0].toBool(), false); // потеря сервиса не оставляет передачу включённой
        }
        bus.unregisterObject(root, QDBusConnection::UnregisterTree);
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    debug_set_level(DEBUG_LEVEL_INFO);
    if (app.arguments().contains("--probe")) {
        PttPortal portal(nullptr, "utun_vibechat_ptt_probe");
        bool done = false;
        QObject::connect(&portal, &PttPortal::statusChanged, &app, [&]() {
            std::printf("portal: %s busy=%d\n", qPrintable(portal.status()), portal.busy());
            if (!portal.busy()) { done = true; QTimer::singleShot(0, &app, &QCoreApplication::quit); }
        });
        QTimer::singleShot(10000, &app, &QCoreApplication::quit);
        portal.initialize();
        app.exec();
        return done && !portal.status().contains("unavailable") ? 0 : 1;
    }
    PortalTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_ptt_portal.moc"
