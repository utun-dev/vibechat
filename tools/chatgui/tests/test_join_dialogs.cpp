/* Настоящие JOIN/OFFER-диалоги: тексты ошибок, восстановление UI и корреляция ответов.
 * Подменён только bridge: тест управляет отказом submit и асинхронным результатом без сетевого подключения. */
#include "../src/joindialog.h"
#include "../src/inviteby.h"
#include "../transport/gui_bridge.h"
#include "../transport/utun_node.h"
#include "../../../lib/debug_config.h"
#include "../../../src/chat/chat_sync.h"
#include "../../../src/chat/invite_link.h"
#include "../../../src/transport_layer/secure_channel.h"

#include <QtTest>
#include <QTranslator>
#include <memory>

static gui_connect_result_fn result_callback;
static const uint8_t group_key[32] = {0x42};
static const QString offer_channel(64, QLatin1Char('a'));
static QString last_offer_channel;
static int submit_result;
static uint64_t next_request, last_request;
static bool last_offer;

void gui_bridge_set_connect_result_cb(gui_connect_result_fn callback) { result_callback = callback; }
int gui_bridge_join_link(const char*, size_t, uint64_t* request) {
    last_offer = false;
    if (!submit_result) *request = last_request = ++next_request;
    return submit_result;
}
int gui_bridge_offer_link(const char* channel, const char*, size_t, uint64_t* request) {
    last_offer = true;
    last_offer_channel = QString::fromLatin1(channel);
    if (!submit_result) *request = last_request = ++next_request;
    return submit_result;
}
QString UtunNode::pubKeyHex() const { return QString(64, QLatin1Char('0')); }

class JoinDialogsTest : public QObject {
    Q_OBJECT
    QTranslator m_translator;
    QString m_link;
    uint8_t m_peer[32] = {0};

    std::unique_ptr<QDialog> makeDialog(bool offer) {
        if (offer) return std::make_unique<InviteByLinkDialog>(offer_channel, nullptr);
        return std::make_unique<JoinDialog>(nullptr, nullptr);
    }

private slots:
    void initTestCase() {
        debug_config_init(); debug_set_level(DEBUG_LEVEL_DEBUG);
        InviteData invite = {};
        uint8_t privateKey[32], seed[32];
        QCOMPARE(sc_identity_generate(invite.pubkey, privateKey, invite.inviter_pubkey, seed), SC_OK);
        invite.join_key = 123; invite.addrCount = 1;
        invite.addrs[0].family = 4; invite.addrs[0].address[0] = 127; invite.addrs[0].address[3] = 1;
        invite.addrs[0].proto = INVITE_PROTO_TCP; invite.addrs[0].port = 1333;
        char link[INVITE_LINK_MAX];
        QVERIFY(invite_link_encode(&invite, link, sizeof(link)) > 0);
        m_link = QString::fromUtf8(link); memcpy(m_peer, invite.pubkey, sizeof(m_peer));
    }

    void cleanup() {
        qApp->removeTranslator(&m_translator);
        submit_result = 0;
        QVERIFY(!result_callback);
    }

    void resultErrors_data() {
        QTest::addColumn<bool>("offer"); QTest::addColumn<bool>("immediate");
        QTest::addColumn<QString>("language"); QTest::addColumn<int>("result"); QTest::addColumn<QString>("message");
        struct Case { int result; const char* english; const char* russian; };
        const Case cases[] = {
            {CHAT_CONNECT_ERROR, "Could not complete the request.", "Не удалось выполнить запрос."},
            {CHAT_CONNECT_INVALID, "Invalid request or chat service unavailable.", "Некорректный запрос или сервис чата недоступен."},
            {CHAT_CONNECT_TIMEOUT, "The request timed out.", "Истекло время ожидания запроса."},
            {CHAT_CONNECT_BUSY, "Another join or invitation is in progress. Try again later.",
                "Уже выполняется вход или приглашение. Повторите попытку позже."},
            {CHAT_CONNECT_CANCELLED, "The request was cancelled.", "Запрос отменён."},
            {CHAT_CONNECT_AUTH, "Invitation authorization failed.", "Отказано в доступе по приглашению."},
            {CHAT_CONNECT_INVITE_DENIED, "You do not have permission to invite members to this group. Ask the owner or an administrator.",
                "У вас нет права приглашать участников в эту группу. Обратитесь к владельцу или администратору."},
            {CHAT_CONNECT_SERVER_KEY, "The server access key is invalid or has changed.", "Ключ доступа к серверу неверен или был изменён."},
            {12345, "Could not complete the request.", "Не удалось выполнить запрос."}
        };
        for (bool offer : {false, true}) for (bool immediate : {false, true})
            for (const QString& language : {QStringLiteral("en"), QStringLiteral("ru")}) for (const auto& c : cases) {
                /* TIMEOUT и неизвестный код приходят только в завершении, не в отказе запуска. */
                if (immediate && c.result > 0) continue;
                const QString name = QStringLiteral("%1-%2-%3-%4").arg(offer ? "offer" : "join")
                    .arg(immediate ? "submit" : "callback").arg(language).arg(c.result);
                QTest::newRow(qPrintable(name)) << offer << immediate << language << c.result
                    << QString::fromUtf8(language == QStringLiteral("ru") ? c.russian : c.english);
            }
    }

    void resultErrors() {
        QFETCH(bool, offer); QFETCH(bool, immediate); QFETCH(QString, language); QFETCH(int, result); QFETCH(QString, message);
        QVERIFY(m_translator.load(QStringLiteral(":/translations/chatgui_") + language + QStringLiteral(".qm")));
        QVERIFY(qApp->installTranslator(&m_translator));
        auto dialog = makeDialog(offer);
        auto* edit = dialog->findChild<QLineEdit*>();
        auto* action = dialog->findChild<QPushButton*>(offer ? "inviteSend" : "joinConnect");
        auto* status = dialog->findChild<QLabel*>(offer ? "inviteStatus" : "joinStatus");
        QVERIFY(edit && action && status && result_callback);
        dialog->show(); QTRY_COMPARE(QApplication::activeModalWidget(), dialog.get());
        edit->setText(m_link); submit_result = immediate ? result : 0;
        QTest::mouseClick(action, Qt::LeftButton);
        QCOMPARE(last_offer, offer);
        if (offer) QCOMPARE(last_offer_channel, offer_channel);
        if (!immediate) {
            QVERIFY(!action->isEnabled());
            result_callback(m_peer, group_key, result, last_request);
        }
        const QString prefix = QCoreApplication::translate("ChatGui", offer ? "Error: %1" : "Connection failed: %1");
        QCOMPARE(status->text(), prefix.arg(message));
        QVERIFY(dialog->isVisible() && action->isEnabled() && !edit->isReadOnly());
        QCOMPARE(edit->text(), m_link);
    }

    void serverLinkStartsOfferAndCannotStartLocalJoin() {
        InviteData server = {}; server.kind = INVITE_KIND_SERVER; server.addrCount = 1;
        memcpy(server.pubkey, m_peer, sizeof(server.pubkey));
        memset(server.add_key, 0x57, sizeof(server.add_key));
        server.addrs[0].family = 4; server.addrs[0].proto = INVITE_PROTO_TCP; server.addrs[0].port = 1333;
        server.addrs[0].address[0] = 127; server.addrs[0].address[3] = 1;
        char buffer[INVITE_LINK_MAX]; QVERIFY(invite_link_encode(&server, buffer, sizeof(buffer)) > 0);
        const QString link = QString::fromUtf8(buffer);
        {
            InviteByLinkDialog dialog(offer_channel, nullptr, nullptr, link);
            auto* action = dialog.findChild<QPushButton*>("inviteSend");
            dialog.show(); QTRY_COMPARE(QApplication::activeModalWidget(), &dialog);
            QTest::mouseClick(action, Qt::LeftButton); QVERIFY(last_offer); QVERIFY(!action->isEnabled());
            result_callback(m_peer, group_key, CHAT_CONNECT_SERVER_KEY, last_request); QVERIFY(action->isEnabled());
        }
        {
            const uint64_t before = last_request;
            JoinDialog dialog(nullptr, nullptr);
            dialog.findChild<QLineEdit*>()->setText(link);
            QTest::mouseClick(dialog.findChild<QPushButton*>("joinConnect"), Qt::LeftButton);
            QCOMPARE(last_request, before);
            QVERIFY(dialog.findChild<QLabel*>("joinStatus")->text().contains("Select a group"));
        }
    }

    void errorSurvivesPendingPreview_data() {
        QTest::addColumn<bool>("accepted");
        QTest::newRow("core-result") << true;
        QTest::newRow("validation") << false;
    }

    void errorSurvivesPendingPreview() {
        QFETCH(bool, accepted);
        JoinDialog dialog(nullptr, nullptr);
        auto* edit = dialog.findChild<QLineEdit*>();
        auto* action = dialog.findChild<QPushButton*>("joinConnect");
        auto* status = dialog.findChild<QLabel*>("joinStatus");
        dialog.show(); QTRY_COMPARE(QApplication::activeModalWidget(), &dialog);
        edit->setText(accepted ? m_link : QStringLiteral("utun://invalid")); QTest::mouseClick(action, Qt::LeftButton);
        if (accepted) result_callback(m_peer, group_key, CHAT_CONNECT_ERROR, last_request);
        const QString error = status->text();
        QVERIFY(!error.isEmpty());
        QTest::qWait(500);
        QCOMPARE(status->text(), error);
    }

    void retryAndSuccess_data() {
        QTest::addColumn<bool>("offer");
        QTest::newRow("join") << false; QTest::newRow("offer") << true;
    }

    void retryAndSuccess() {
        QFETCH(bool, offer);
        auto dialog = makeDialog(offer);
        auto* edit = dialog->findChild<QLineEdit*>();
        auto* action = dialog->findChild<QPushButton*>(offer ? "inviteSend" : "joinConnect");
        QVERIFY(edit && action && result_callback);
        QSignalSpy finished(dialog.get(), &QDialog::finished);
        dialog->show(); QTRY_COMPARE(QApplication::activeModalWidget(), dialog.get());
        edit->setText(m_link); QTest::mouseClick(action, Qt::LeftButton);
        const uint64_t previous = last_request;
        result_callback(m_peer, group_key, CHAT_CONNECT_BUSY, previous);
        QVERIFY(action->isEnabled());
        QTest::mouseClick(action, Qt::LeftButton); QVERIFY(last_request != previous);
        result_callback(m_peer, group_key, 0, previous);
        const uint8_t other[32] = {2};
        result_callback(other, group_key, 0, last_request);
        QVERIFY(dialog->isVisible() && !action->isEnabled()); QCOMPARE(finished.size(), 0);
        result_callback(m_peer, group_key, 0, last_request);
        QTRY_VERIFY(!dialog->isVisible());
        QCOMPARE(finished.size(), 1); QCOMPARE(finished.first().first().toInt(), int(QDialog::Accepted));
    }
};

QTEST_MAIN(JoinDialogsTest)
#include "test_join_dialogs.moc"
