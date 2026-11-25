/* Qt clipboard и настоящие Linux file promises; bridge заглушён на границе команд ядру.
 * Проверяет типы значков, flash, UTF-8/alpha PNG, snapshot дерева и скачивание только при чтении ОС. */
#include "../src/clipboard_exchange.h"
#include "../src/clipboard_files.h"
#include "../transport/gui_bridge.h"
#include <QtTest>
#include <QClipboard>
#include <QToolButton>
#include <QPushButton>
#include <QBuffer>
#include <QFile>
#include <QMimeData>
#include <QUrl>
#include <QDir>
#include <QDialog>
#include <QLabel>
#include <atomic>
#include <cstring>
extern "C" {
#include "debug_config.h"
}
namespace {
quint64 request;
QByteArray sentGroup;
QString groupText(int n) { auto key = QByteArray(32, 0); key[0] = char(n); return QString::fromLatin1(key.toHex()); }
QByteArray sentPeer;
int sentType, applied;
bool success, corrupt;
QByteArray sent;
std::vector<chat_clipboard_source> sources;
std::atomic<int> fetches{0};
QByteArray clipboardEvent(int type, const QByteArray& data) {
    QByteArray bytes(CHAT_CLIPBOARD_EVENT_HEADER, 0); bytes[0] = 1; bytes[16] = 1; bytes[CHAT_CLIPBOARD_EVENT_PEER_ED_OFF] = 2; bytes[CHAT_CLIPBOARD_EVENT_TYPE_OFF] = char(type); return bytes + data;
}
QByteArray memberEvent(int type, const QByteArray& data, quint64 group = 1) {
    auto bytes = clipboardEvent(type, data); bytes[CHAT_CLIPBOARD_EVENT_SCOPE_OFF] = CHAT_CLIPBOARD_MEMBER;
    bytes[CHAT_CLIPBOARD_EVENT_GROUP_OFF] = char(group);
    return bytes;
}
}
extern "C" {
void gui_bridge_set_clipboard_received_cb(gui_dm_messages_fn) {}
void gui_bridge_set_clipboard_result_cb(gui_dm_messages_fn) {}
int gui_bridge_send_clipboard(uint64_t id, int type, const uint8_t* data, size_t size, const uint8_t group[32], const uint8_t peer[32]) {
    request = id; sentGroup = group ? QByteArray(reinterpret_cast<const char*>(group), 32) : QByteArray(); sentPeer = peer ? QByteArray(reinterpret_cast<const char*>(peer), 32) : QByteArray(); sentType = type; sent = QByteArray(reinterpret_cast<const char*>(data), int(size)); return 0;
}
int gui_bridge_clipboard_applied(const uint8_t*, int value) { applied++; success = value; return 0; }
int gui_bridge_offer_clipboard_files(uint64_t id, const chat_clipboard_source* files, size_t count, const uint8_t* data, size_t size, const uint8_t group[32], const uint8_t peer[32]) {
    request = id; sentGroup = group ? QByteArray(reinterpret_cast<const char*>(group), 32) : QByteArray(); sentPeer = peer ? QByteArray(reinterpret_cast<const char*>(peer), 32) : QByteArray(); sources.assign(files, files + count);
    sent = QByteArray(reinterpret_cast<const char*>(data), int(size)); sentType = 3; return 0;
}
int gui_bridge_fetch_clipboard_file(const uint8_t[32], const uint8_t[32], const uint8_t object[16], uint64_t size,
    const char* destination, void (*done)(void*, int), void* arg) {
    fetches++;
    for (const auto& source : sources) if (!std::memcmp(source.object, object, 16) && source.size == size) {
        QFile input(QString::fromUtf8(source.path)), output(QString::fromUtf8(destination));
        if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) return -1;
        QByteArray data = input.readAll(); if (corrupt && !data.isEmpty()) data[0] ^= 1;
        if (output.write(data) != data.size() || !output.flush()) return -1;
        output.close(); done(arg, 0); return 0;
    }
    return -1;
}
}
class ClipboardTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); }
    void init() { applied = 0; sent.clear(); sentType = 0; sentGroup.clear(); sentPeer.clear(); success = false; corrupt = false; sources.clear(); fetches = 0; }
    void memberDestinationAndOwnReset() {
        ClipboardExchange widget; const QByteArray peer(32, char(7));
        QApplication::clipboard()->setText("target"); widget.sendTo(groupText(123), peer, "Recipient");
        QCOMPARE(sentGroup.toHex(), groupText(123).toLatin1()); QCOMPARE(sentPeer, peer); QCOMPARE(sent, QByteArray("target"));
        QByteArray reply(25, 0); for (int i = 0; i < 8; i++) reply[i] = char(request >> (8 * i)); reply[8] = CHAT_CLIPBOARD_FINISHED;
        widget.result(reinterpret_cast<const uint8_t*>(reply.constData()), reply.size());
        widget.send(); QVERIFY(sentGroup.isEmpty()); QVERIFY(sentPeer.isEmpty());
    }
    void memberOffersRequireExplicitCopyAndKeepOpenedSnapshot() {
        ClipboardExchange widget; widget.setNameResolver([](const QString&, const QByteArray&) { return QString("<Sender>"); });
        QSignalSpy changed(&widget, &ClipboardExchange::memberClipboardChanged);
        QApplication::clipboard()->setText("local"); auto bytes = memberEvent(1, "first");
        widget.receive(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        QTRY_COMPARE(applied, 1); QVERIFY(success); QCOMPARE(changed.count(), 1);
        QCOMPARE(widget.memberSender(groupText(1)), QString("<Sender>")); QCOMPARE(widget.memberType(groupText(1)), 1);
        QCOMPARE(QApplication::clipboard()->text(), QString("local")); widget.openMember(groupText(1));
        auto* dialog = widget.findChild<QDialog*>("memberClipboardDialog"); QVERIFY(dialog);
        auto* description = dialog->findChild<QLabel*>("memberClipboardDescription"); QVERIFY(description);
        QCOMPARE(description->textFormat(), Qt::PlainText); QVERIFY(description->text().contains("<Sender>"));
        bytes = memberEvent(1, "second"); widget.receive(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        QTRY_COMPARE(applied, 2); QCOMPARE(QApplication::clipboard()->text(), QString("local"));
        dialog->findChild<QToolButton*>("memberClipboardCopy")->click(); QCOMPARE(QApplication::clipboard()->text(), QString("first"));
        dialog->findChild<QPushButton*>("memberClipboardDismiss")->click(); QCOMPARE(widget.memberType(groupText(1)), 1);
        delete dialog; widget.openMember(groupText(1)); dialog = widget.findChild<QDialog*>("memberClipboardDialog"); QVERIFY(dialog);
        dialog->findChild<QToolButton*>("memberClipboardCopy")->click(); QCOMPARE(QApplication::clipboard()->text(), QString("second"));
        bytes = clipboardEvent(1, "own"); widget.receive(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        QTRY_COMPARE(applied, 3); QCOMPARE(QApplication::clipboard()->text(), QString("own")); QCOMPARE(widget.memberType(groupText(1)), 1);
        dialog->findChild<QPushButton*>("memberClipboardDismiss")->click(); QCOMPARE(widget.memberType(groupText(1)), 0);
        QCOMPARE(changed.last()[2].toInt(), 0);
        bytes = memberEvent(1, "after dismiss"); widget.receive(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        QTRY_COMPARE(applied, 4); QCOMPARE(widget.memberType(groupText(1)), 1); widget.result(nullptr, 0); QCOMPARE(widget.memberType(groupText(1)), 0);
    }
    void memberImageAndOtherGroupsRemainIndependent() {
        ClipboardExchange widget; QApplication::clipboard()->setText("local");
        QImage image(8, 8, QImage::Format_ARGB32); image.fill(qRgba(1, 2, 3, 80));
        QByteArray png; QBuffer output(&png); QVERIFY(output.open(QIODevice::WriteOnly)); QVERIFY(image.save(&output, "PNG"));
        auto bytes = memberEvent(2, png); widget.receive(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        bytes = memberEvent(1, "other group", 2); widget.receive(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        QTRY_COMPARE(applied, 2); QCOMPARE(widget.memberType(groupText(1)), 2); QCOMPARE(widget.memberType(groupText(2)), 1);
        QCOMPARE(QApplication::clipboard()->text(), QString("local")); widget.openMember(groupText(1));
        widget.findChild<QToolButton*>("memberClipboardCopy")->click(); QCOMPARE(QApplication::clipboard()->image(), image);
    }
    void memberFilesStayLazyAndKeepOwnProvider() {
#ifndef __linux__
        QSKIP("Linux file promises; native IStream has separate tests");
#else
        QTemporaryDir input; QVERIFY(QDir(input.path()).mkdir("folder"));
        QFile source(input.filePath("folder/file.txt")); QVERIFY(source.open(QIODevice::WriteOnly)); QCOMPARE(source.write("data"), qint64(4)); source.close();
        QString error; auto prepared = ClipboardFiles::prepare({input.filePath("folder")}, error); QVERIFY2(prepared, qPrintable(error));
        sources = prepared->sources; ClipboardExchange widget;
        auto bytes = clipboardEvent(3, prepared->manifest); widget.receive(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        QCOMPARE(applied, 1); const QString ownPath = QApplication::clipboard()->mimeData()->urls()[0].toLocalFile();
        QApplication::clipboard()->setText("local"); bytes = memberEvent(3, prepared->manifest);
        widget.receive(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        QCOMPARE(applied, 2); QVERIFY(success); QCOMPARE(widget.memberType(groupText(1)), 4); QCOMPARE(fetches.load(), 0);
        QCOMPARE(QApplication::clipboard()->text(), QString("local")); QVERIFY(QFileInfo(ownPath).isDir());
        widget.openMember(groupText(1)); widget.findChild<QToolButton*>("memberClipboardCopy")->click(); QCOMPARE(fetches.load(), 0);
        QFile promised(QApplication::clipboard()->mimeData()->urls()[0].toLocalFile() + "/file.txt");
        QVERIFY(promised.open(QIODevice::ReadOnly)); QCOMPARE(promised.readAll(), QByteArray("data")); promised.close(); QCOMPARE(fetches.load(), 1);
#endif
    }
    void sendSnapshotsTextAndImage() {
        ClipboardExchange widget; widget.show(); QApplication::clipboard()->setText(QString::fromUtf8("Текст 🙂\nline"));
        widget.send(); QCOMPARE(sentType, 1); QCOMPARE(sent, QString::fromUtf8("Текст 🙂\nline").toUtf8());
        QByteArray reply(25, 0); for (int i = 0; i < 8; i++) reply[i] = char(request >> (8 * i)); reply[8] = CHAT_CLIPBOARD_FINISHED;
        widget.result(reinterpret_cast<const uint8_t*>(reply.constData()), reply.size());
        QImage image(8, 8, QImage::Format_ARGB32); image.fill(qRgba(1, 2, 3, 80)); QApplication::clipboard()->setImage(image);
        widget.send(); QTRY_COMPARE(sentType, 2); QCOMPARE(QImage::fromData(sent, "PNG"), image);
    }
    void receiveTypesFlashAndInvalidData() {
        ClipboardExchange widget; widget.show();
        auto* icon = widget.findChild<QToolButton*>("receivedClipboardButton"); QVERIFY(icon); QVERIFY(icon->isHidden());
        QByteArray bytes = clipboardEvent(1, QString::fromUtf8("текст 🙂").toUtf8());
        widget.receive(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        QTRY_COMPARE(applied, 1); QVERIFY(success); QCOMPARE(QApplication::clipboard()->text(), QString::fromUtf8("текст 🙂"));
        QVERIFY(!icon->isHidden()); const auto textIcon = icon->icon().cacheKey();
        QTRY_VERIFY(!icon->styleSheet().isEmpty()); QTRY_VERIFY_WITH_TIMEOUT(icon->styleSheet().isEmpty(), 1800);
        QImage image(4, 4, QImage::Format_ARGB32); image.fill(qRgba(12, 45, 67, 23));
        QByteArray png; QBuffer output(&png); output.open(QIODevice::WriteOnly); QVERIFY(image.save(&output, "PNG"));
        bytes = clipboardEvent(2, png); widget.receive(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        QTRY_COMPARE(applied, 2); QVERIFY(success); QCOMPARE(QApplication::clipboard()->image(), image); QVERIFY(icon->icon().cacheKey() != textIcon);
        bytes = clipboardEvent(1, QByteArray::fromHex("c328")); widget.receive(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        QTRY_COMPARE(applied, 3); QVERIFY(!success); QCOMPARE(QApplication::clipboard()->image(), image);
    }
    void stopDiscardsPendingPreparationAndReenablesSend() {
        ClipboardExchange widget; widget.show();
        QImage image(256, 256, QImage::Format_RGB32); image.fill(Qt::blue); QApplication::clipboard()->setImage(image);
        widget.send(); auto* button = widget.findChild<QPushButton*>("sendClipboardButton"); QVERIFY(button); QVERIFY(!button->isEnabled());
        widget.result(nullptr, 0); QVERIFY(button->isEnabled());
        QTest::qWait(150); QCOMPARE(sentType, 0);
        QApplication::clipboard()->setText("after restart"); widget.send(); QCOMPARE(sentType, 1); QCOMPARE(sent, QByteArray("after restart"));
    }
    void corruptFileIsNeverReturnedToFileManager() {
#ifndef __linux__
        QSKIP("Linux FUSE path; IStream mechanics covered by test_clipboard_native");
#else
        QTemporaryDir input; QFile source(input.filePath("file.txt")); QVERIFY(source.open(QIODevice::WriteOnly)); QVERIFY(source.write("correct") == 7); source.close();
        QString error; auto prepared = ClipboardFiles::prepare({source.fileName()}, error); QVERIFY2(prepared, qPrintable(error));
        sources = prepared->sources; corrupt = true;
        auto files = ClipboardFiles::receive(prepared->manifest, QByteArray::fromHex(groupText(1).toLatin1()), QByteArray(32, char(2)), error); QVERIFY2(files, qPrintable(error)); QVERIFY(files->publish());
        QFile promised(QApplication::clipboard()->mimeData()->urls()[0].toLocalFile()); QVERIFY(promised.open(QIODevice::ReadOnly));
        QVERIFY(promised.readAll().isEmpty()); QVERIFY(promised.error() != QFileDevice::NoError); promised.close(); QCOMPARE(fetches.load(), 1);
#endif
    }
    void rejectedOfferReleasesUnpublishedSnapshot() {
        QTemporaryDir input; QFile original(input.filePath("file.txt")); QVERIFY(original.open(QIODevice::WriteOnly));
        QVERIFY(original.write("data") == 4); original.close();
        ClipboardExchange widget; auto* mime = new QMimeData; mime->setUrls({QUrl::fromLocalFile(original.fileName())});
        QApplication::clipboard()->setMimeData(mime); widget.send(); QTRY_COMPARE(sentType, 3); QCOMPARE(sources.size(), size_t(1));
        const QString snapshot = QString::fromUtf8(sources[0].path); QVERIFY(QFileInfo::exists(snapshot));
        QByteArray reply(25, 0); for (int i = 0; i < 8; i++) reply[i] = char(request >> (8 * i)); reply[8] = CHAT_CLIPBOARD_REJECTED;
        widget.result(reinterpret_cast<const uint8_t*>(reply.constData()), reply.size());
        QVERIFY(!QFileInfo::exists(snapshot)); QVERIFY(QFileInfo::exists(original.fileName()));
    }
    void promisesKeepFoldersAndFetchOnRead() {
#ifndef __linux__
        QSKIP("Native promises covered by test_clipboard_native on Windows");
#else
        QTemporaryDir input; QVERIFY(input.isValid()); QDir root(input.path()); QVERIFY(root.mkpath("folder/nested/empty"));
        const QByteArray contents = QString::fromUtf8("данные 🙂\n").toUtf8();
        QFile source(root.filePath("folder/nested/one.txt")); QVERIFY(source.open(QIODevice::WriteOnly)); QCOMPARE(source.write(contents), contents.size()); source.close();
        ClipboardExchange widget; widget.show(); auto* mime = new QMimeData; mime->setUrls({QUrl::fromLocalFile(root.filePath("folder"))});
        QApplication::clipboard()->setMimeData(mime); widget.send(); QTRY_COMPARE(sentType, 3); QVERIFY(!sources.empty());
        // Файл отправителя меняется после Send; передаётся уже закреплённый snapshot.
        QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate)); QVERIFY(source.write("changed") == 7); source.close();
        QByteArray bytes = clipboardEvent(3, sent); widget.receive(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        QCOMPARE(applied, 1); QVERIFY(success); QCOMPARE(fetches.load(), 0);
        auto urls = QApplication::clipboard()->mimeData()->urls(); QCOMPARE(urls.size(), 1);
        const QString folder = urls[0].toLocalFile(); QVERIFY(QFileInfo(folder + "/nested/empty").isDir());
        QCOMPARE(fetches.load(), 0); QFile promised(folder + "/nested/one.txt"); QVERIFY(promised.open(QIODevice::ReadOnly));
        QCOMPARE(promised.readAll(), contents); promised.close(); QCOMPARE(fetches.load(), 1);
        QVERIFY(promised.open(QIODevice::ReadOnly)); QCOMPARE(promised.readAll(), contents); promised.close(); QCOMPARE(fetches.load(), 1);
        QVERIFY(!promised.open(QIODevice::WriteOnly));
        auto* icon = widget.findChild<QToolButton*>("receivedClipboardButton"); QVERIFY(icon->toolTip().contains("folder"));
#endif
    }
    void rejectTraversalAndSymlinks() {
        QString error; QVERIFY(!ClipboardFiles::receive("{\"version\":1,\"roots\":[\"..\"],\"entries\":[{\"path\":\"..\",\"directory\":true}]}", QByteArray::fromHex(groupText(1).toLatin1()), QByteArray(32, char(2)), error));
        QTemporaryDir input; QVERIFY(input.isValid()); QFile file(input.filePath("file")); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        auto cancelled = std::make_shared<std::atomic<bool>>(true);
        QVERIFY(!ClipboardFiles::prepare({file.fileName()}, error, cancelled)); QVERIFY(QFileInfo::exists(file.fileName()));
        QVERIFY(QFile::link(file.fileName(), input.filePath("link")));
        QVERIFY(!ClipboardFiles::prepare({input.filePath("link")}, error)); QVERIFY(!error.isEmpty());
    }
    void rejectDirectoryAndNestedSymlinks() {
#ifndef __linux__
        QSKIP("QFile::link creates symbolic links on Linux; Windows creates Shell shortcuts");
#else
        QTemporaryDir input, other; QVERIFY(input.isValid()); QVERIFY(other.isValid());
        QFile secret(other.filePath("secret.txt")); QVERIFY(secret.open(QIODevice::WriteOnly));
        QCOMPARE(secret.write("secret"), qint64(6)); secret.close();
        QString error;
        QVERIFY(QFile::link(other.path(), input.filePath("link")));
        QVERIFY(!ClipboardFiles::prepare({input.filePath("link")}, error)); QVERIFY(!error.isEmpty());
        QVERIFY(QDir(input.path()).mkdir("folder"));
        QVERIFY(QFile::link(secret.fileName(), input.filePath("folder/link")));
        QVERIFY(!ClipboardFiles::prepare({input.filePath("folder")}, error)); QVERIFY(!error.isEmpty());
        QVERIFY(QFile::remove(input.filePath("folder/link")));
        QVERIFY(QFile::link(other.path(), input.filePath("folder/link")));
        QVERIFY(!ClipboardFiles::prepare({input.filePath("folder")}, error)); QVERIFY(!error.isEmpty());
        QFile check(secret.fileName()); QVERIFY(check.open(QIODevice::ReadOnly)); QCOMPARE(check.readAll(), QByteArray("secret"));
#endif
    }
};
QTEST_MAIN(ClipboardTest)
#include "test_clipboard_exchange.moc"
