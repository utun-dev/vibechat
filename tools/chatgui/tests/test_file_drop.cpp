/* Реальные Qt-события на viewport: drop только готовит вложение, отправку инициирует пользователь. */
#include "../src/inputbar.h"
#include "../src/chatview.h"
#include "../src/emojipanel.h"
#include "../src/formattingpanel.h"
#include "clipboard_fixture.h"
#include "../../../lib/debug_config.h"
extern "C" {
#include "media_async/attachment_send.h"
#include "mem.h"
}
#include <QtTest>
#include <QTextEdit>
#include <QListView>
#include <QMimeData>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QLabel>
#include <QTimer>
#include <QUrl>
#include <QMessageBox>
#include <QAbstractButton>
#include <QPushButton>
#include <QCheckBox>
#include <QMainWindow>
#include <QVBoxLayout>
#include <QStringListModel>
#include <QScrollBar>
#include <QScrollArea>
#include <QPointer>
#include <QScopeGuard>
#include <QImage>
#include <QClipboard>
#include <QBuffer>
#include <QMenu>
#include <QAction>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QRadioButton>
#include <QImageWriter>
#include <QImageReader>
#include <QPainter>
#include <QRegularExpression>

static QSet<QString> clipboardPaths() {
    QSet<QString> paths;
    const QDir directory(QDir::tempPath());
    for (const auto& name : directory.entryList({"vibechat-clipboard-*.png", "vibechat-clipboard-*.jpg"}, QDir::Files))
        paths.insert(directory.filePath(name));
    return paths;
}

static QString clipboardPngPath(const QSet<QString>& paths) {
    for (const auto& path : paths) if (path.endsWith(".png") && !path.contains("clipboard-send-")) return path;
    return {};
}

static void copyImage(const QImage& image, bool encoded = false) {
    auto* mime = new QMimeData;
    if (encoded) {
        QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG")) qFatal("Cannot create clipboard PNG fixture");
        mime->setData("image/png", bytes);
    } else mime->setImageData(image);
    mime->setText("image alternate text"); mime->setHtml("<b>image alternate text</b>");
    QApplication::clipboard()->setMimeData(mime);
}

static QString makeFile(const QTemporaryDir& dir, const QString& name, const QByteArray& contents = "test") {
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size()) return {};
    return path;
}

static bool dropUrls(QWidget* target, const QList<QUrl>& urls, bool process = true) {
    QMimeData mime;
    mime.setUrls(urls);
    QDragEnterEvent enter(QPoint(5, 5), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &enter);
    if (!enter.isAccepted()) return false;
    QDragMoveEvent move(QPoint(5, 5), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &move);
    QDropEvent drop(QPointF(5, 5), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &drop);
    if (process) QCoreApplication::processEvents();
    return drop.isAccepted() && drop.dropAction() == Qt::CopyAction;
}

class FileDropTest : public QObject {
    Q_OBJECT
private slots:
    void cardSwitchCancelsClipboardPreparationAndLateSubmission() {
        QTemporaryDir directory; InputBar bar(nullptr, nullptr, nullptr); bar.show();
        auto* input = bar.findChild<QTextEdit*>(); QVERIFY(input);
        bar.setRecordingContext(directory.path(), "123", 0, QString(64, 'a'));
        QImage screenshot(320, 200, QImage::Format_RGB32); screenshot.fill(Qt::blue);
        QApplication::clipboard()->setImage(screenshot); QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
        auto* preview = bar.findChild<QLabel*>("clipboardImageThumbnail"); QVERIFY(preview);
        QTRY_VERIFY(preview->isVisible());
        bar.setRecordingContext(directory.path(), "123", 0, QString(64, 'b'));
        QVERIFY(!preview->isVisible());
        input->setPlainText("old card"); QSignalSpy sent(&bar, &InputBar::sendMessage); QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(sent.size(), 1); const auto request = sent.first().at(2).toULongLong(); QVERIFY(request);
        bar.setRecordingContext(directory.path(), "123", 0, QString(64, 'c')); input->setPlainText("new card");
        bar.submissionResult(request, true); QCOMPARE(input->toPlainText(), QString("new card"));
    }
    void cleanup() { QApplication::clipboard()->clear(); }

    void formattingPanelSharesComposerOverlayWithEmoji() {
        QWidget history; history.resize(700, 500); history.show();
        auto* emoji = new EmojiPanel(&history);
        auto* formatting = new FormattingPanel(&history);
        InputBar bar(emoji, formatting, nullptr); bar.show();
        auto* emojiButton = bar.findChild<QPushButton*>("emojiButton");
        auto* helpButton = bar.findChild<QPushButton*>("messageFormattingHelpButton");
        QVERIFY(emojiButton && helpButton); QVERIFY(helpButton->isVisible());
        QVERIFY(helpButton->mapTo(&bar, QPoint()).x() < emojiButton->mapTo(&bar, QPoint()).x());
        QVERIFY(emoji->isHidden() && formatting->isHidden());

        QTest::mouseClick(helpButton, Qt::LeftButton); QCoreApplication::processEvents();
        QVERIFY(formatting->isVisible()); QVERIFY(emoji->isHidden()); QVERIFY(!formatting->findChild<QScrollArea*>());
        QVERIFY(formatting->testAttribute(Qt::WA_StyledBackground));
        auto* title = formatting->findChild<QLabel*>("messageFormattingHelpTitle");
        auto* firstExample = formatting->findChild<QLabel*>("formattingBoldSource"); QVERIFY(title && firstExample);
        const int lineHeight = title->fontMetrics().lineSpacing();
        QCOMPARE(title->y(), lineHeight);
        QCOMPARE(firstExample->y() - title->geometry().bottom() - 1, lineHeight);
        for (int i = 0; i < 3; ++i) {
            const char* names[] = {"Bold", "Italic", "Strike"};
            const QString samples[] = {"bold", "italic", "strikethrough"};
            const QString markers[] = {"**bold**", "__italic__", "~~strikethrough~~"};
            auto* source = formatting->findChild<QLabel*>(QStringLiteral("formatting%1Source").arg(QLatin1String(names[i])));
            auto* example = formatting->findChild<QLabel*>(QStringLiteral("formatting%1Example").arg(QLatin1String(names[i])));
            QVERIFY(source && example); QCOMPARE(source->text(), markers[i]); QCOMPARE(example->text(), samples[i]);
            QCOMPARE(example->font().bold(), i == 0); QCOMPARE(example->font().italic(), i == 1); QCOMPARE(example->font().strikeOut(), i == 2);
        }
        auto* inlineHint = formatting->findChild<QLabel*>("formattingInlineHint");
        auto* codeTitle = formatting->findChild<QLabel*>("formattingCodeTitle"); QVERIFY(inlineHint && codeTitle);
        QCOMPARE(codeTitle->y() - inlineHint->geometry().bottom() - 1, lineHeight / 2);
        auto* code = formatting->findChild<QLabel*>("formattingCodeExample"); QVERIFY(code);
        QCOMPARE(code->text(), QStringLiteral("```cpp\nint value = 42;\n```"));
        auto* details = formatting->findChild<QLabel*>("formattingHelpDetails"); QVERIFY(details);
        QVERIFY(details->text().contains(QStringLiteral("Spaces around ```")));
        QVERIFY(history.rect().contains(formatting->geometry()));
        QCOMPARE(formatting->geometry().bottomRight(), history.rect().bottomRight());
        QCOMPARE(formatting->height(), details->geometry().bottom() + 1 + formatting->layout()->contentsMargins().bottom());

        QTest::mouseClick(emojiButton, Qt::LeftButton);
        QVERIFY(emoji->isVisible()); QVERIFY(formatting->isHidden());
        QTest::mouseClick(helpButton, Qt::LeftButton);
        QVERIFY(formatting->isVisible()); QVERIFY(emoji->isHidden());

        history.resize(300, 500); QCoreApplication::processEvents();
        QCOMPARE(formatting->width(), history.width());
        QCOMPARE(formatting->height(), details->geometry().bottom() + 1 + formatting->layout()->contentsMargins().bottom());
        QCOMPARE(formatting->geometry().bottomRight(), history.rect().bottomRight());
        QTest::mouseClick(helpButton, Qt::LeftButton); QVERIFY(formatting->isHidden());
    }

    void clipboardFormatChoice_data() {
        QTest::addColumn<int>("density"); QTest::addColumn<bool>("alpha"); QTest::addColumn<bool>("dm");
        QTest::addColumn<bool>("encrypted"); QTest::addColumn<QString>("manual"); QTest::addColumn<bool>("automaticJpeg");
        QTest::newRow("png-smaller") << 0 << false << false << false << QString() << false;
        QTest::newRow("png-bigger-but-below-twofold") << 16 << false << false << false << QString() << false;
        QTest::newRow("jpeg-group") << 64 << false << false << false << QString() << true;
        QTest::newRow("jpeg-pm") << 64 << false << true << false << QString() << true;
        QTest::newRow("jpeg-encrypted") << 64 << false << false << true << QString() << true;
        QTest::newRow("manual-jpeg") << 0 << false << false << false << QString("jpeg") << false;
        QTest::newRow("manual-png-encrypted") << 64 << false << false << true << QString("png") << true;
        QTest::newRow("transparent-manual-png") << 1024 << true << true << false << QString("png") << true;
        QTest::newRow("transparent-manual-jpeg") << 0 << true << false << false << QString("jpeg") << false;
    }
    void clipboardFormatChoice() {
        QFETCH(int, density); QFETCH(bool, alpha); QFETCH(bool, dm); QFETCH(bool, encrypted);
        QFETCH(QString, manual); QFETCH(bool, automaticJpeg);
        const QImage image = clipboardCodecFixture(density, alpha);
        QImage jpegImage = image;
        if (alpha) {
            jpegImage = QImage(image.size(), QImage::Format_RGB32); jpegImage.fill(Qt::white);
            QPainter painter(&jpegImage); painter.drawImage(0, 0, image);
        }
        QByteArray pngBytes, jpegBytes; QBuffer pngBuffer(&pngBytes), jpegBuffer(&jpegBytes);
        QVERIFY(pngBuffer.open(QIODevice::WriteOnly)); QVERIFY(jpegBuffer.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&pngBuffer, "PNG")); QVERIFY(jpegImage.save(&jpegBuffer, "JPEG", 85));
        if (automaticJpeg) QVERIFY(pngBytes.size() >= 2 * jpegBytes.size());
        else {
            QVERIFY(pngBytes.size() < 2 * jpegBytes.size());
            if (density == 16) QVERIFY(pngBytes.size() > jpegBytes.size());
        }
        const auto before = clipboardPaths(); QTemporaryDir directory;
        InputBar bar(nullptr, nullptr, nullptr); bar.setRecordingContext(directory.path(), dm ? "dm:456" : "123", dm ? 7 : 0);
        bar.setDmMode(dm); bar.show();
        auto* editor = bar.findChild<QTextEdit*>(); auto* thumbnail = bar.findChild<QLabel*>("clipboardImageThumbnail");
        auto* jpeg = bar.findChild<QRadioButton*>("clipboardJpegChoice"); auto* png = bar.findChild<QRadioButton*>("clipboardPngChoice");
        QVERIFY(editor); QVERIFY(thumbnail); QVERIFY(jpeg); QVERIFY(png);
        copyImage(image); editor->paste(); QTRY_VERIFY(!thumbnail->isHidden());
        QVERIFY(jpeg->isVisible() && png->isVisible()); QVERIFY(jpeg->isEnabled() && png->isEnabled());
        QCOMPARE(jpeg->isChecked(), automaticJpeg); QCOMPARE(png->isChecked(), !automaticJpeg);
        QCOMPARE(bar.focusWidget(), static_cast<QWidget*>(automaticJpeg ? jpeg : png));
        QVERIFY(jpeg->text().startsWith("JPEG (")); QVERIFY(png->text().startsWith("PNG ("));
        QVERIFY(jpeg->text().contains(QRegularExpression("[0-9]+(\\.[0-9]+)?(B|KB|MB)")));
        QVERIFY(png->text().contains(QRegularExpression("[0-9]+(\\.[0-9]+)?(B|KB|MB)")));
        QCOMPARE(clipboardPaths().size(), before.size() + 2);
        auto* encrypt = bar.findChild<QCheckBox*>(); QVERIFY(encrypt); if (encrypted) encrypt->setChecked(true);
        const bool useJpeg = manual.isEmpty() ? automaticJpeg : manual == "jpeg";
        if (!manual.isEmpty()) {
            auto* current = automaticJpeg ? jpeg : png;
            QTest::keyClick(current, automaticJpeg ? Qt::Key_Right : Qt::Key_Left);
            QCOMPARE(jpeg->isChecked(), useJpeg); QCOMPARE(png->isChecked(), !useJpeg);
            QCOMPARE(bar.focusWidget(), static_cast<QWidget*>(useJpeg ? jpeg : png));
        }
        attachment_send_req* pending = nullptr;
        const auto discard = qScopeGuard([&] { attachment_send_discard(pending); });
        connect(&bar, &InputBar::submitAttachment, &bar, [&](const QString&, attachment_send_req* request) { pending = request; });
        QTest::keyClick(useJpeg ? jpeg : png, Qt::Key_Return); QVERIFY(pending);
        QCOMPARE(pending->info.kind, useJpeg ? ATTACHMENT_JPEG : ATTACHMENT_PNG); QCOMPARE(pending->encrypted, int(encrypted));
        QCOMPARE(pending->info.width, image.width()); QCOMPARE(pending->info.height, image.height());
        QVERIFY(QString::fromUtf8(pending->info.name).endsWith(useJpeg ? ".jpg" : ".png"));
        const QString source = QString::fromUtf8(pending->source); QVERIFY(source.endsWith(useJpeg ? ".jpg" : ".png"));
        QFile payload(source); QVERIFY(payload.open(QIODevice::ReadOnly)); QCOMPARE(payload.readAll(), useJpeg ? jpegBytes : pngBytes); payload.close();
        const QImage decoded(source); QCOMPARE(decoded.size(), image.size());
        if (useJpeg) QVERIFY(!decoded.hasAlphaChannel()); else QCOMPARE(decoded.pixelColor(0, 0), image.pixelColor(0, 0));
        bar.submissionResult(pending->request_id, true); QVERIFY(jpeg->isHidden() || !jpeg->isVisible());
        attachment_send_discard(pending); pending = nullptr; QCOMPARE(clipboardPaths(), before);
    }

    void clipboardManualChoiceOutlivesPendingResult() {
        const auto before = clipboardPaths(); QTemporaryDir directory;
        InputBar bar(nullptr, nullptr, nullptr); bar.setRecordingContext(directory.path(), "123"); bar.show();
        auto* editor = bar.findChild<QTextEdit*>(); auto* thumbnail = bar.findChild<QLabel*>("clipboardImageThumbnail");
        auto* jpeg = bar.findChild<QRadioButton*>("clipboardJpegChoice"); auto* png = bar.findChild<QRadioButton*>("clipboardPngChoice");
        QVERIFY(editor); QVERIFY(thumbnail); QVERIFY(jpeg); QVERIFY(png);
        copyImage(clipboardCodecFixture(0)); editor->paste(); QTRY_VERIFY(!thumbnail->isHidden()); QVERIFY(png->isChecked());
        attachment_send_req* pending = nullptr;
        const auto discard = qScopeGuard([&] { attachment_send_discard(pending); });
        connect(&bar, &InputBar::submitAttachment, &bar, [&](const QString&, attachment_send_req* request) { pending = request; });
        QTest::keyClick(png, Qt::Key_Return, Qt::ControlModifier); QVERIFY(!pending);
        QCOMPARE(editor->toPlainText(), QString("\n")); QCOMPARE(bar.focusWidget(), static_cast<QWidget*>(editor)); editor->clear();
        QTest::keyClick(png, Qt::Key_Return); QVERIFY(pending); QCOMPARE(pending->info.kind, ATTACHMENT_PNG);
        QTest::mouseClick(jpeg, Qt::LeftButton); QVERIFY(jpeg->isChecked());
        bar.submissionResult(pending->request_id, true); QVERIFY(jpeg->isVisible());
        attachment_send_discard(pending); pending = nullptr;
        QTest::keyClick(jpeg, Qt::Key_Return); QVERIFY(pending); QCOMPARE(pending->info.kind, ATTACHMENT_JPEG);
        const auto request = pending->request_id;
        attachment_send_discard(pending); pending = nullptr; bar.submissionResult(request, false);
        QVERIFY(jpeg->isChecked()); QVERIFY(jpeg->isVisible()); // Ошибка сохраняет ручной выбор и оба варианта.
        bar.setRecordingContext(directory.path(), "456"); QCOMPARE(clipboardPaths(), before);
    }

    void clipboardImageUsesPngPipeline_data() {
        QTest::addColumn<bool>("dm"); QTest::addColumn<bool>("encrypted"); QTest::addColumn<QString>("method");
        QTest::newRow("group-ctrl-v") << false << false << QString("ctrl");
        QTest::newRow("pm-ctrl-v") << true << false << QString("ctrl");
        QTest::newRow("encrypted-shift-insert") << false << true << QString("shift");
        QTest::newRow("group-context-menu-png") << false << false << QString("menu");
    }
    void clipboardImageUsesPngPipeline() {
        QFETCH(bool, dm); QFETCH(bool, encrypted); QFETCH(QString, method);
        const auto before = clipboardPaths(); QTemporaryDir directory; QVERIFY(directory.isValid());
        InputBar bar(nullptr, nullptr, nullptr); bar.setRecordingContext(directory.path(), dm ? "dm:456" : "123", dm ? 7 : 0);
        bar.setDmMode(dm); bar.show();
        auto* editor = bar.findChild<QTextEdit*>(); QVERIFY(editor);
        auto* thumbnail = bar.findChild<QLabel*>("clipboardImageThumbnail"); QVERIFY(thumbnail);
        QSignalSpy errors(&bar, &InputBar::attachmentRejected), texts(&bar, &InputBar::sendMessage);
        attachment_send_req* pending = nullptr;
        const auto discard = qScopeGuard([&] { attachment_send_discard(pending); });
        connect(&bar, &InputBar::submitAttachment, &bar, [&](const QString& target, attachment_send_req* request) {
            QCOMPARE(target, dm ? QString("dm:456") : QString("123")); QVERIFY(!pending); pending = request;
        });
        QImage original(120, 80, QImage::Format_ARGB32); original.fill(qRgba(17, 61, 95, 127));
        editor->setPlainText("existing draft"); copyImage(original, method == "menu");
        if (method == "ctrl") QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
        else if (method == "shift") QTest::keyClick(editor, Qt::Key_Insert, Qt::ShiftModifier);
        else {
            std::unique_ptr<QMenu> menu(editor->createStandardContextMenu()); QAction* paste = nullptr;
            for (auto* action : menu->actions()) if (action->text().remove('&').startsWith("Paste")) paste = action;
            QVERIFY(paste); QVERIFY(paste->isEnabled()); paste->trigger();
        }
        QCOMPARE(editor->toPlainText(), QString("existing draft")); QVERIFY(!pending); QCOMPARE(texts.size(), 0);
        QTest::keyClick(editor, Qt::Key_Return); QVERIFY(!pending); QCOMPARE(texts.size(), 0); // PNG ещё кодируется.
        QApplication::clipboard()->setText("clipboard has changed");
        QTRY_VERIFY(!thumbnail->isHidden());
        QCOMPARE(errors.size(), 0); QCOMPARE(editor->toPlainText(), QString("existing draft"));
        const auto drafts = clipboardPaths() - before; QCOMPARE(drafts.size(), 2); const QString draft = clipboardPngPath(drafts);
        QImage decoded(draft); QCOMPARE(decoded.size(), original.size()); QCOMPARE(decoded.pixelColor(0, 0), original.pixelColor(0, 0));
        editor->clear(); const QByteArray reply(32, 'r'); if (!dm) bar.setReply(reply, "Member", "Original");
        if (encrypted) bar.findChild<QCheckBox*>()->setChecked(true);
        QTest::keyClick(editor, Qt::Key_Return); QVERIFY(pending); QVERIFY(pending->request_id); QVERIFY(pending->temporary_source);
        QCOMPARE(pending->info.kind, ATTACHMENT_PNG); QCOMPARE(pending->info.width, 120); QCOMPARE(pending->info.height, 80);
        QCOMPARE(pending->encrypted, int(encrypted)); QCOMPARE(pending->dm_generation, dm ? UINT64_C(7) : UINT64_C(0));
        QVERIFY(QString::fromUtf8(pending->info.name).startsWith("clipboard-"));
        QVERIFY(QString::fromUtf8(pending->info.name).endsWith(".png"));
        if (!dm) QCOMPARE(QByteArray(reinterpret_cast<const char*>(pending->reply_to), 32), reply);
        const QString source = QString::fromUtf8(pending->source); QVERIFY(source != draft);
        QCOMPARE(QImage(source).pixelColor(0, 0), original.pixelColor(0, 0));
        bar.submissionResult(pending->request_id, true); QVERIFY(thumbnail->isHidden()); QVERIFY(!QFile::exists(draft));
        QVERIFY(QFile::exists(source)); // Worker владеет отдельным source после очистки preview.
        attachment_send_discard(pending); pending = nullptr; QVERIFY(!QFile::exists(source));
        QCOMPARE(clipboardPaths(), before);
    }

    void clipboardFailureKeepsDraftAndRetries_data() {
        QTest::addColumn<bool>("dm"); QTest::newRow("group") << false; QTest::newRow("pm") << true;
    }
    void clipboardFailureKeepsDraftAndRetries() {
        QFETCH(bool, dm); const auto before = clipboardPaths(); QTemporaryDir directory;
        InputBar bar(nullptr, nullptr, nullptr); bar.setRecordingContext(directory.path(), dm ? "dm:456" : "123", dm ? 7 : 0);
        bar.setDmMode(dm); bar.show(); auto* editor = bar.findChild<QTextEdit*>(); QVERIFY(editor);
        auto* thumbnail = bar.findChild<QLabel*>("clipboardImageThumbnail"); QVERIFY(thumbnail);
        attachment_send_req* pending = nullptr;
        const auto discard = qScopeGuard([&] { attachment_send_discard(pending); });
        connect(&bar, &InputBar::submitAttachment, &bar, [&](const QString&, attachment_send_req* request) { pending = request; });
        QImage image(32, 24, QImage::Format_RGB32); image.fill(Qt::green); copyImage(image); editor->paste();
        QTRY_VERIFY(!thumbnail->isHidden());
        const auto drafts = clipboardPaths() - before; QCOMPARE(drafts.size(), 2); const QString draft = clipboardPngPath(drafts);
        QTest::keyClick(editor, Qt::Key_Return); QVERIFY(pending); const auto first = pending->request_id; QVERIFY(first);
        QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(pending->request_id, first);
        attachment_send_discard(pending); pending = nullptr;
        QSignalSpy errors(&bar, &InputBar::attachmentRejected); bar.submissionResult(first, false);
        QCOMPARE(errors.size(), 1); QVERIFY(!thumbnail->isHidden()); QVERIFY(QFile::exists(draft));
        QTest::keyClick(editor, Qt::Key_Return); QVERIFY(pending); QVERIFY(pending->request_id != first);
        bar.submissionResult(pending->request_id, true); QVERIFY(thumbnail->isHidden()); QVERIFY(!QFile::exists(draft));
        attachment_send_discard(pending); pending = nullptr; QCOMPARE(clipboardPaths(), before);
    }

    void clipboardOldSuccessPreservesNewPaste() {
        const auto before = clipboardPaths(); QTemporaryDir directory;
        InputBar bar(nullptr, nullptr, nullptr); bar.setRecordingContext(directory.path(), "123"); bar.show();
        auto* editor = bar.findChild<QTextEdit*>(); auto* thumbnail = bar.findChild<QLabel*>("clipboardImageThumbnail");
        QVERIFY(editor); QVERIFY(thumbnail);
        attachment_send_req* pending = nullptr;
        const auto discard = qScopeGuard([&] { attachment_send_discard(pending); });
        connect(&bar, &InputBar::submitAttachment, &bar, [&](const QString&, attachment_send_req* request) { pending = request; });
        QImage image(32, 24, QImage::Format_RGB32); image.fill(Qt::red); copyImage(image); editor->paste();
        QTRY_VERIFY(!thumbnail->isHidden()); QTest::keyClick(editor, Qt::Key_Return); QVERIFY(pending);
        const auto oldDrafts = clipboardPaths() - before; QCOMPARE(oldDrafts.size(), 3);
        image.fill(Qt::blue); copyImage(image); editor->paste();
        bar.submissionResult(pending->request_id, true); // Успех A не должен отменить кодирование B.
        const QString sendSource = QString::fromUtf8(pending->source);
        auto* status = bar.findChild<QLabel*>("mediaPreparationStatus"); QVERIFY(status); QTRY_VERIFY(status->isHidden());
        const auto newDrafts = clipboardPaths() - before - QSet<QString>{sendSource}; QCOMPARE(newDrafts.size(), 2);
        QCOMPARE(QImage(clipboardPngPath(newDrafts)).pixelColor(0, 0), QColor(Qt::blue));
        QVERIFY(!thumbnail->isHidden()); QVERIFY(QFile::exists(sendSource));
        attachment_send_discard(pending); pending = nullptr;
        bar.setRecordingContext(directory.path(), "456"); QCOMPARE(clipboardPaths(), before);
    }

    void clipboardWorkIsSupersededAndCleaned_data() {
        QTest::addColumn<QString>("action");
        for (const char* action : {"replace", "change-chat", "clear", "destroy", "edit", "disable", "file"})
            QTest::newRow(action) << QString(action);
    }
    void clipboardWorkIsSupersededAndCleaned() {
        QFETCH(QString, action); const auto before = clipboardPaths(); QTemporaryDir directory;
        {
            InputBar bar(nullptr, nullptr, nullptr); bar.setRecordingContext(directory.path(), "123"); bar.show();
            auto* editor = bar.findChild<QTextEdit*>(); QVERIFY(editor);
            QImage image(120, 80, QImage::Format_RGB32); image.fill(Qt::red); copyImage(image); editor->paste();
            if (action != "destroy") {
                if (action == "change-chat") bar.setRecordingContext(directory.path(), "456");
                if (action == "clear") QVERIFY(QMetaObject::invokeMethod(&bar, "clearPendingAttachment", Qt::DirectConnection));
                if (action == "edit") { bar.editMessage(QByteArray(32, 'e'), 1, "edit"); bar.cancelEdit(); }
                if (action == "disable") { bar.setMediaEnabled(false); bar.setMediaEnabled(true); }
                if (action == "file") QVERIFY(dropUrls(editor->viewport(), {QUrl::fromLocalFile(makeFile(directory, "file.txt"))}));
                image = QImage(60, 40, QImage::Format_RGB32); image.fill(Qt::blue); copyImage(image); editor->paste();
                auto* thumbnail = bar.findChild<QLabel*>("clipboardImageThumbnail"); QVERIFY(thumbnail); QTRY_VERIFY(!thumbnail->isHidden());
                const auto drafts = clipboardPaths() - before; QCOMPARE(drafts.size(), 2);
                const QImage decoded(clipboardPngPath(drafts));
                QCOMPARE(decoded.size(), image.size()); QCOMPARE(decoded.pixelColor(0, 0), QColor(Qt::blue));
            }
        }
        QCOMPARE(clipboardPaths(), before);
    }

    void clipboardInvalidImagePreservesDraft_data() {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"corrupt-png", "too-wide", "too-many-pixels"}) QTest::newRow(kind) << QString(kind);
    }
    void clipboardInvalidImagePreservesDraft() {
        QFETCH(QString, kind); const auto before = clipboardPaths(); QTemporaryDir directory;
        InputBar bar(nullptr, nullptr, nullptr); bar.setRecordingContext(directory.path(), "123"); bar.show();
        auto* editor = bar.findChild<QTextEdit*>(); auto* thumbnail = bar.findChild<QLabel*>("clipboardImageThumbnail");
        QVERIFY(editor); QVERIFY(thumbnail);
        QImage original(32, 24, QImage::Format_RGB32); original.fill(Qt::green); copyImage(original); editor->paste();
        QTRY_VERIFY(!thumbnail->isHidden()); const auto drafts = clipboardPaths() - before; QCOMPARE(drafts.size(), 2);
        QSignalSpy errors(&bar, &InputBar::attachmentRejected);
        if (kind == "corrupt-png") {
            auto* mime = new QMimeData; mime->setData("image/png", "invalid PNG"); QApplication::clipboard()->setMimeData(mime);
        } else copyImage(QImage(kind == "too-wide" ? 16385 : 8192, kind == "too-wide" ? 1 : 4097, QImage::Format_Mono));
        editor->paste(); QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(clipboardPaths() - before, drafts); QVERIFY(!thumbnail->isHidden());
        QCOMPARE(QImage(clipboardPngPath(drafts)).pixelColor(0, 0), QColor(Qt::green));
        bar.setRecordingContext(directory.path(), "456"); QCOMPARE(clipboardPaths(), before);
    }

    void clipboardTextAndUnavailableComposer_data() {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"text", "disabled-media", "no-target", "edit"}) QTest::newRow(mode) << QString(mode);
    }
    void clipboardTextAndUnavailableComposer() {
        QFETCH(QString, mode); const auto before = clipboardPaths(); QTemporaryDir directory;
        InputBar bar(nullptr, nullptr, nullptr); bar.setRecordingContext(directory.path(), "123");
        auto* editor = bar.findChild<QTextEdit*>(); QVERIFY(editor); QSignalSpy errors(&bar, &InputBar::attachmentRejected);
        if (mode == "text") {
            QApplication::clipboard()->setText("обычный текст\nnext line"); editor->paste();
            QCOMPARE(editor->toPlainText(), QString("обычный текст\nnext line")); QCOMPARE(errors.size(), 0);
        } else {
            if (mode == "disabled-media") bar.setMediaEnabled(false);
            if (mode == "no-target") bar.setRecordingContext(directory.path(), "");
            if (mode == "edit") bar.editMessage(QByteArray(32, 'e'), 1, "edit");
            const QString text = editor->toPlainText();
            QImage image(32, 24, QImage::Format_RGB32); image.fill(Qt::red); copyImage(image); editor->paste();
            QCOMPARE(errors.size(), 1); QCOMPARE(editor->toPlainText(), text);
        }
        QCOMPARE(clipboardPaths(), before);
    }

    void externalClipboardImage() {
        const QString platform = QGuiApplication::platformName();
        const bool wayland = platform.startsWith("wayland");
        if (!wayland && platform != "xcb") QSKIP("Run with QT_QPA_PLATFORM=xcb or wayland to test an external clipboard owner.");
        const QString program = QStandardPaths::findExecutable(wayland ? "wl-copy" : "xclip");
        if (program.isEmpty()) QSKIP("Install wl-copy/xclip to test an external PNG clipboard owner.");
        const auto before = clipboardPaths(); QTemporaryDir directory;
        InputBar bar(nullptr, nullptr, nullptr); bar.setRecordingContext(directory.path(), "123"); bar.show(); bar.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&bar));
        QImage original(120, 80, QImage::Format_ARGB32); original.fill(qRgba(41, 81, 121, 127));
        QByteArray bytes; QBuffer buffer(&bytes); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(original.save(&buffer, "PNG"));
        QProcess owner;
        const auto stop = qScopeGuard([&] { owner.kill(); owner.waitForFinished(1000); });
        owner.start(program, wayland ? QStringList{"--foreground", "--type", "image/png"}
                                    : QStringList{"-selection", "clipboard", "-target", "image/png", "-in", "-quiet"});
        QVERIFY(owner.waitForStarted(1000)); QCOMPARE(owner.write(bytes), qint64(bytes.size())); owner.closeWriteChannel();
        QTRY_VERIFY(QApplication::clipboard()->mimeData()->hasFormat("image/png"));
        auto* editor = bar.findChild<QTextEdit*>(); QVERIFY(editor); editor->paste();
        auto* thumbnail = bar.findChild<QLabel*>("clipboardImageThumbnail"); QVERIFY(thumbnail); QTRY_VERIFY(!thumbnail->isHidden());
        const auto drafts = clipboardPaths() - before; QCOMPARE(drafts.size(), 2);
        QCOMPARE(QImage(clipboardPngPath(drafts)).pixelColor(0, 0), original.pixelColor(0, 0));
        bar.setRecordingContext(directory.path(), "456"); QCOMPARE(clipboardPaths(), before);
    }

    void ordinaryTextWaitsForResultAndCanRetry() {
        InputBar bar(nullptr, nullptr, nullptr); bar.setRecordingContext("/tmp", "123");
        auto* editor = bar.findChild<QTextEdit*>(); QVERIFY(editor);
        QSignalSpy sends(&bar, &InputBar::sendMessage), errors(&bar, &InputBar::attachmentRejected);
        editor->setPlainText("ordinary");
        QVERIFY(QMetaObject::invokeMethod(&bar, "onSendClicked", Qt::DirectConnection));
        QCOMPARE(sends.size(), 1); const auto request = sends[0][2].toULongLong(); QVERIFY(request);
        QCOMPARE(editor->toPlainText(), QString("ordinary"));
        QVERIFY(QMetaObject::invokeMethod(&bar, "onSendClicked", Qt::DirectConnection)); QCOMPARE(sends.size(), 1);
        bar.submissionResult(request, false); QCOMPARE(errors.size(), 1); QCOMPARE(editor->toPlainText(), QString("ordinary"));
        QVERIFY(QMetaObject::invokeMethod(&bar, "onSendClicked", Qt::DirectConnection)); QCOMPARE(sends.size(), 2);
        QVERIFY(sends[1][2].toULongLong() != request); bar.submissionResult(sends[1][2].toULongLong(), true);
        QVERIFY(editor->toPlainText().isEmpty());
    }

    void ordinaryResultPreservesNewDraftAndContext() {
        InputBar bar(nullptr, nullptr, nullptr); bar.setRecordingContext("/tmp", "123");
        auto* editor = bar.findChild<QTextEdit*>(); QVERIFY(editor); QSignalSpy sends(&bar, &InputBar::sendMessage);
        editor->setPlainText("first"); QVERIFY(QMetaObject::invokeMethod(&bar, "onSendClicked", Qt::DirectConnection));
        QVERIFY(sends[0][2].toULongLong());
        editor->setPlainText("second"); bar.submissionResult(sends[0][2].toULongLong(), true);
        QCOMPARE(editor->toPlainText(), QString("second"));
        QVERIFY(QMetaObject::invokeMethod(&bar, "onSendClicked", Qt::DirectConnection));
        bar.setRecordingContext("/tmp", "456"); bar.setRecordingContext("/tmp", "123");
        editor->setPlainText("returned to channel"); bar.submissionResult(sends[1][2].toULongLong(), true);
        QCOMPARE(editor->toPlainText(), QString("returned to channel"));
    }

    void ordinaryAttachmentWaitsForResultAndCanRetry() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const QString file = makeFile(dir, "music.mp3");
        InputBar bar(nullptr, nullptr, nullptr); bar.show(); bar.setRecordingContext(dir.filePath("media"), "123");
        auto* editor = bar.findChild<QTextEdit*>(); QVERIFY(editor);
        auto* clear = bar.findChild<QPushButton*>("clearAttachmentButton"); QVERIFY(clear);
        QSignalSpy files(&bar, &InputBar::sendFileAttachment), errors(&bar, &InputBar::attachmentRejected);
        QVERIFY(dropUrls(editor->viewport(), {QUrl::fromLocalFile(file)}));
        QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(files.size(), 1);
        const auto request = files[0][5].toULongLong(); QVERIFY(request); QVERIFY(clear->isVisible());
        QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(files.size(), 1);
        bar.submissionResult(request, false); QCOMPARE(errors.size(), 1);
        QVERIFY(errors[0][0].toString().contains("music.mp3")); QVERIFY(clear->isVisible()); QVERIFY(QFile::exists(file));
        QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(files.size(), 2);
        const auto retry = files[1][5].toULongLong(); QVERIFY(retry && retry != request);
        bar.submissionResult(retry, true); QVERIFY(!clear->isVisible());
        QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(files.size(), 2);
        QVERIFY(dropUrls(editor->viewport(), {QUrl::fromLocalFile(file)}));
        QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(files.size(), 3);
        const auto old = files[2][5].toULongLong();
        QVERIFY(dropUrls(editor->viewport(), {QUrl::fromLocalFile(makeFile(dir, "new.txt"))}));
        bar.submissionResult(old, true); QVERIFY(clear->isVisible());
        QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(files.size(), 4);
        QCOMPARE(files[3][3].toString(), QString("new.txt"));
    }

    void submissionIdsOutliveComposer() {
        quint64 oldRequest = 0;
        {
            InputBar bar(nullptr, nullptr, nullptr); bar.setRecordingContext("/tmp", "123");
            QSignalSpy sends(&bar, &InputBar::sendMessage); bar.findChild<QTextEdit*>()->setPlainText("old");
            QVERIFY(QMetaObject::invokeMethod(&bar, "onSendClicked", Qt::DirectConnection)); oldRequest = sends[0][2].toULongLong();
        }
        InputBar bar(nullptr, nullptr, nullptr); bar.setRecordingContext("/tmp", "123");
        auto* editor = bar.findChild<QTextEdit*>(); editor->setPlainText("new"); QSignalSpy sends(&bar, &InputBar::sendMessage);
        QVERIFY(QMetaObject::invokeMethod(&bar, "onSendClicked", Qt::DirectConnection));
        QVERIFY(oldRequest); QVERIFY(sends[0][2].toULongLong() != oldRequest);
        bar.submissionResult(oldRequest, true); QCOMPARE(editor->toPlainText(), QString("new"));
    }

    void attachmentHandlerCanChangeOrDestroyComposer_data() {
        QTest::addColumn<bool>("destroy"); QTest::newRow("change-context") << false; QTest::newRow("destroy") << true;
    }
    void attachmentHandlerCanChangeOrDestroyComposer() {
        QFETCH(bool, destroy); QTemporaryDir dir; QVERIFY(dir.isValid()); const QString file = makeFile(dir, "file.txt");
        QPointer<InputBar> bar = new InputBar(nullptr, nullptr, nullptr); auto cleanup = qScopeGuard([&] { delete bar.data(); });
        bar->setRecordingContext(dir.path(), "123"); auto* editor = bar->findChild<QTextEdit*>(); QVERIFY(editor);
        QVERIFY(dropUrls(editor->viewport(), {QUrl::fromLocalFile(file)})); editor->setPlainText("old text");
        QSignalSpy texts(bar.data(), &InputBar::sendMessage); QString target, source;
        connect(bar.data(), &InputBar::sendFileAttachment, this, [&](const QString&, const QString&, const QString&,
            const QString&, const QByteArray&, quint64) {
            if (destroy) delete bar.data();
            else { bar->setRecordingContext(dir.path(), "456"); editor->setPlainText("new draft"); }
        });
        connect(bar.data(), &InputBar::sendFileAttachment, this, [&](const QString& capturedTarget, const QString& capturedSource,
            const QString&, const QString&, const QByteArray&, quint64) { target = capturedTarget; source = capturedSource; });
        QVERIFY(QMetaObject::invokeMethod(bar.data(), "onSendClicked", Qt::DirectConnection)); QCOMPARE(texts.size(), 0);
        if (destroy) QVERIFY(!bar);
        else { QCOMPARE(target, QString("123")); QCOMPARE(source, file); QCOMPARE(editor->toPlainText(), QString("new draft")); }
    }

    void emojiSelectionInsertsCorrectTextAtCursor() {
        EmojiPanel panel;
        InputBar input(&panel, nullptr, nullptr);
        auto* editor = input.findChild<QTextEdit*>(); QVERIFY(editor);
        editor->setPlainText("beforeafter");
        auto cursor = editor->textCursor(); cursor.setPosition(6); editor->setTextCursor(cursor);
        QPushButton* button = nullptr;
        for (auto* candidate : panel.findChildren<QPushButton*>()) {
            if (candidate->toolTip() == ":pinched_fingers:") { button = candidate; break; }
        }
        QVERIFY(button); QSignalSpy selected(&panel, &EmojiPanel::emojiSelected); button->click();
        const QString emoji = QString::fromUtf8("🤌");
        QCOMPARE(selected.count(), 1); QCOMPARE(selected.first().first().toString(), emoji);
        QCOMPARE(editor->toPlainText(), "before" + emoji + "after");
        QCOMPARE(editor->textCursor().position(), 6 + emoji.size());
    }

    void deletedPmDropsDraftAndLatePreparation() {
        QTemporaryDir source; QVERIFY(source.isValid());
        const QString path = makeFile(source, "private.txt"); QVERIFY(!path.isEmpty());
        InputBar input(nullptr, nullptr, nullptr); input.setDmMode(true);
        input.setRecordingContext(source.path(), "dm:42", 1); input.show();
        auto* editor = input.findChild<QTextEdit*>(); QVERIFY(editor);
        editor->setPlainText("old draft");
        QVERIFY(dropUrls(editor->viewport(), {QUrl::fromLocalFile(path)}));
        const QByteArray oldJob(16, 'a'), newJob(16, 'b');
        input.updatePreparation("dm:42", oldJob, 0);
        auto statusVisible = [&input](const QString& text) {
            for (auto* label : input.findChildren<QLabel*>()) if (label->isVisible() && label->text() == text) return true;
            return false;
        };
        QVERIFY(statusVisible("Preparing media…"));
        input.forgetDmHistory("dm:42", 1);
        input.setRecordingContext(source.path(), "dm:42", 2);
        input.updatePreparation("dm:42", oldJob, 2);
        QVERIFY(editor->toPlainText().isEmpty()); QVERIFY(QFileInfo::exists(path));
        QVERIFY(!statusVisible("Preparing media…")); QVERIFY(!statusVisible("Failed to prepare attachment"));
        QSignalSpy files(&input, &InputBar::sendFileAttachment);
        QVERIFY(QMetaObject::invokeMethod(&input, "onSendClicked", Qt::DirectConnection)); QCOMPARE(files.size(), 0);
        editor->setPlainText("new draft");
        input.setEnabled(false); QSignalSpy messages(&input, &InputBar::sendMessage);
        QVERIFY(QMetaObject::invokeMethod(&input, "onSendClicked", Qt::DirectConnection)); QCOMPARE(messages.size(), 0);
        QCOMPARE(editor->toPlainText(), QString("new draft"));
        input.setEnabled(true); input.updatePreparation("dm:42", newJob, 0);
        QVERIFY(statusVisible("Preparing media…")); input.updatePreparation("dm:42", newJob, 1);
        QVERIFY(!statusVisible("Preparing media…"));
        input.setRecordingContext(source.path(), "dm:42", 3);
        QVERIFY(editor->toPlainText().isEmpty()); // Смена поколения также аннулирует прежний composer.
    }

    void historyKeyboardNavigation_data() {
        QTest::addColumn<int>("key");
        QTest::addColumn<int>("focus");
        for (int key : {Qt::Key_Up, Qt::Key_Down, Qt::Key_PageUp, Qt::Key_PageDown, Qt::Key_End})
            for (int focus = 0; focus < 3; focus++)
                QTest::newRow(qPrintable(QStringLiteral("key-%1-focus-%2").arg(key).arg(focus))) << key << focus;
    }
    void historyKeyboardNavigation() {
        QFETCH(int, key); QFETCH(int, focus);
        QMainWindow window;
        auto* body = new QWidget(&window); auto* layout = new QVBoxLayout(body);
        auto* view = new ChatView(body); auto* input = new InputBar(nullptr, nullptr, nullptr, body);
        layout->addWidget(view, 1); layout->addWidget(input); window.setCentralWidget(body);
        QStringList rows; for (int i = 0; i < 100; i++) rows.append(QString::number(i));
        QStringListModel model(rows); view->setModel(&model);
        view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        auto* editor = input->findChild<QTextEdit*>(); QVERIFY(editor); QCOMPARE(input->focusProxy(), editor);
        editor->installEventFilter(view); window.installEventFilter(view);
        const QString draft = QStringLiteral("first line\nsecond line\nthird line"); editor->setPlainText(draft);
        auto cursor = editor->textCursor(); cursor.setPosition(5); editor->setTextCursor(cursor);
        window.resize(600, 400); window.show(); window.activateWindow();
        QWidget* target = focus == 0 ? static_cast<QWidget*>(editor) : focus == 1 ? static_cast<QWidget*>(&window) : view;
        if (focus == 0) input->setFocus(); else target->setFocus();
        QTRY_COMPARE(QApplication::focusWidget(), target);
        auto* bar = view->verticalScrollBar(); QTRY_VERIFY(bar->maximum() > bar->pageStep() * 2);
        bar->setValue(bar->maximum() / 2);
        const int before = bar->value();
        const bool up = key == Qt::Key_Up || key == Qt::Key_PageUp;
        const int step = key == Qt::Key_Up || key == Qt::Key_Down ? bar->singleStep() : bar->pageStep();
        QSignalSpy latest(view, &ChatView::latestRequested);
        QTest::keyClick(target, static_cast<Qt::Key>(key));
        QCOMPARE(bar->value(), key == Qt::Key_End ? bar->maximum() : before + (up ? -step : step));
        QCOMPARE(latest.count(), key == Qt::Key_End ? 1 : 0);
        QCOMPARE(QApplication::focusWidget(), target); QCOMPARE(editor->textCursor().position(), 5); QCOMPARE(editor->toPlainText(), draft);
        bar->setValue(up ? bar->minimum() : bar->maximum());
        const int boundary = bar->value(); QTest::keyClick(target, static_cast<Qt::Key>(key));
        QCOMPARE(bar->value(), boundary); QCOMPARE(editor->textCursor().position(), 5);
        if (focus == 0) {
            bar->setValue(before);
            QTest::keyClick(editor, Qt::Key_Down, Qt::ShiftModifier);
            QVERIFY(editor->textCursor().hasSelection()); QCOMPARE(bar->value(), before);
            QCOMPARE(editor->toPlainText(), draft); QCOMPARE(QApplication::focusWidget(), editor);
        }
    }
    void encryptedAttachmentCapturesTargetAndResetsChoice_data() {
        QTest::addColumn<bool>("picker");
        QTest::newRow("drop") << false; QTest::newRow("file-picker") << true;
    }
    void encryptedAttachmentCapturesTargetAndResetsChoice() {
        QFETCH(bool, picker);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString file = makeFile(dir, "private.txt", "private bytes");
        InputBar bar(nullptr, nullptr, nullptr); bar.show(); bar.setRecordingContext(dir.filePath("media"), "123");
        auto* editor = bar.findChild<QTextEdit*>(); QVERIFY(editor);
        auto* encrypt = bar.findChild<QCheckBox*>("encryptAttachmentCheckBox"); QVERIFY(encrypt);
        auto* clear = bar.findChild<QPushButton*>("clearAttachmentButton"); QVERIFY(clear);
        QVERIFY(!encrypt->isVisible()); QVERIFY(!encrypt->isChecked());
        QSignalSpy ordinary(&bar, &InputBar::sendFileAttachment);
        int encrypted = 0;
        connect(&bar, &InputBar::submitAttachment, &bar, [&](const QString& target, attachment_send_req* request) {
            QCOMPARE(target, QString("123")); QVERIFY(request->encrypted); QCOMPARE(request->info.kind, ATTACHMENT_FILE);
            QCOMPARE(QString::fromUtf8(request->source), file); QCOMPARE(QString::fromUtf8(request->info.name), QString("private.txt"));
            ++encrypted; const auto id = request->request_id; u_free(request); bar.submissionResult(id, true);
        });
        QTimer timer; timer.setInterval(1);
        if (picker) {
            connect(&timer, &QTimer::timeout, &bar, [&] {
                auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
                if (!dialog) return;
                timer.stop(); dialog->selectFile(file);
                QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
            });
            timer.start(); QVERIFY(QMetaObject::invokeMethod(&bar, "onAttachClicked", Qt::DirectConnection));
        } else QVERIFY(dropUrls(editor->viewport(), {QUrl::fromLocalFile(file)}));
        QVERIFY(encrypt->isVisible()); QVERIFY(!encrypt->isChecked());
        QTest::mouseClick(encrypt, Qt::LeftButton); QVERIFY(encrypt->isChecked());
        QTest::keyClick(editor, Qt::Key_Return);
        QCOMPARE(encrypted, 1); QCOMPARE(ordinary.size(), 0); QVERIFY(!encrypt->isChecked()); QVERIFY(QFile::exists(file));
        QVERIFY(!encrypt->isVisible());
        QVERIFY(dropUrls(editor->viewport(), {QUrl::fromLocalFile(file)}));
        QTest::mouseClick(encrypt, Qt::LeftButton); QVERIFY(encrypt->isChecked());
        QTest::mouseClick(clear, Qt::LeftButton); QVERIFY(!encrypt->isVisible()); QVERIFY(!encrypt->isChecked());
        QVERIFY(dropUrls(editor->viewport(), {QUrl::fromLocalFile(file)}));
        QTest::mouseClick(encrypt, Qt::LeftButton); QVERIFY(encrypt->isChecked());
        QVERIFY(dropUrls(editor->viewport(), {QUrl::fromLocalFile(dir.filePath("missing.txt"))}));
        QVERIFY(encrypt->isChecked());
        QVERIFY(dropUrls(editor->viewport(), {QUrl::fromLocalFile(makeFile(dir, "second.txt"))}));
        QVERIFY(encrypt->isVisible()); QVERIFY(!encrypt->isChecked());
        QTest::mouseClick(encrypt, Qt::LeftButton); QVERIFY(encrypt->isChecked());
        bar.setRecordingContext(dir.filePath("media"), "456"); QVERIFY(!encrypt->isVisible()); QVERIFY(!encrypt->isChecked());
        bar.setDmMode(true);
        QVERIFY(dropUrls(editor->viewport(), {QUrl::fromLocalFile(file)}));
        QVERIFY(!encrypt->isVisible()); QVERIFY(!encrypt->isChecked());
    }

    void editDraftSaveCancelAndFailure() {
        InputBar bar(nullptr, nullptr, nullptr); bar.resize(600, 160); bar.show(); bar.setRecordingContext("/tmp", "123");
        auto* editor = bar.findChild<QTextEdit*>(); QVERIFY(editor);
        const QByteArray id(32, 'm'), reply(32, 'r');
        QSignalSpy edits(&bar, &InputBar::saveEdit), sends(&bar, &InputBar::sendMessage), errors(&bar, &InputBar::attachmentRejected);
        bar.setReply(reply, "Alice", "original"); editor->setPlainText("draft");
        bar.editMessage(id, 2, "old"); QCOMPARE(editor->toPlainText(), QString("old"));
        QTest::keyClick(editor, Qt::Key_Escape); QCOMPARE(editor->toPlainText(), QString("draft"));
        bar.editMessage(id, 2, "old"); editor->setPlainText("new"); QTest::keyClick(editor, Qt::Key_Return);
        QCOMPARE(edits.size(), 1); QVERIFY(sends.isEmpty());
        QCOMPARE(edits[0][0].toString(), QString("123")); QCOMPARE(edits[0][1].toByteArray(), id);
        QCOMPARE(edits[0][2].toULongLong(), quint64(2)); QCOMPARE(edits[0][3].toString(), QString("new"));
        QVERIFY(editor->isReadOnly()); const auto request = edits[0][4].toULongLong(); QVERIFY(request);
        QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(edits.size(), 1);
        bar.submissionResult(request, false); QCOMPARE(errors.size(), 1); QVERIFY(!editor->isReadOnly());
        QCOMPARE(editor->toPlainText(), QString("new")); QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(edits.size(), 2);
        bar.submissionResult(edits[1][4].toULongLong(), true); QCOMPARE(editor->toPlainText(), QString("draft"));
        QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(sends.size(), 1); QCOMPARE(sends[0][1].toByteArray(), reply);
        bar.submissionResult(sends[0][2].toULongLong(), true);
        editor->setPlainText("another draft"); bar.editMessage(id, 3, "edit"); QTest::keyClick(editor, Qt::Key_Return);
        bar.setRecordingContext("/tmp", "456"); editor->setPlainText("new channel draft");
        bar.submissionResult(edits.last()[4].toULongLong(), true); QCOMPARE(editor->toPlainText(), QString("new channel draft"));
    }

    void replyDraftAndLateResults() {
        InputBar bar(nullptr, nullptr, nullptr); bar.resize(600, 160); bar.show(); bar.setRecordingContext("/tmp", "123");
        auto* editor = bar.findChild<QTextEdit*>(); QVERIFY(editor);
        QSignalSpy sends(&bar, &InputBar::sendMessage);
        const QByteArray first(32, 'a'), second(32, 'b');
        bar.setReply(first, "Alice", "Original"); editor->setPlainText("answer");
        QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(sends.size(), 1);
        QCOMPARE(sends[0][1].toByteArray(), first); const quint64 id = sends[0][2].toULongLong(); QVERIFY(id);
        QCOMPARE(editor->toPlainText(), QString("answer"));
        QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(sends.size(), 1); // Не отправлять повторно до результата.
        bar.submissionResult(id, false); QCOMPARE(editor->toPlainText(), QString("answer"));
        QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(sends.size(), 2);
        bar.setReply(second, "Bob", "Another original"); editor->setPlainText("new draft");
        bar.submissionResult(sends[1][2].toULongLong(), true); QCOMPARE(editor->toPlainText(), QString("new draft"));
        QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(sends.size(), 3); QCOMPARE(sends[2][1].toByteArray(), second);
        bar.submissionResult(sends[2][2].toULongLong(), true); QVERIFY(editor->toPlainText().isEmpty());
        editor->setPlainText("ordinary"); QTest::keyClick(editor, Qt::Key_Return); QVERIFY(sends[3][1].toByteArray().isEmpty());
        bar.setReply(first, "Alice", "Original"); QTest::keyClick(editor, Qt::Key_Escape);
        editor->setPlainText("cancelled"); QTest::keyClick(editor, Qt::Key_Return); QVERIFY(sends[4][1].toByteArray().isEmpty());
        bar.setReply(first, "Alice", "Original"); bar.setRecordingContext("/tmp", "456");
        editor->setPlainText("other channel"); QTest::keyClick(editor, Qt::Key_Return); QVERIFY(sends[5][1].toByteArray().isEmpty());
    }

    void replyAttachmentAndTextHaveIndependentResults() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const QString file = makeFile(dir, "answer.txt");
        InputBar bar(nullptr, nullptr, nullptr); bar.resize(600, 180); bar.show(); bar.setRecordingContext(dir.path(), "123");
        auto* editor = bar.findChild<QTextEdit*>(); QVERIFY(editor);
        const QByteArray original(32, 'a'); bar.setReply(original, "Alice", "original");
        QVERIFY(dropUrls(editor->viewport(), {QUrl::fromLocalFile(file)})); editor->setPlainText("answer");
        QSignalSpy files(&bar, &InputBar::sendFileAttachment), texts(&bar, &InputBar::sendMessage);
        QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(files.size(), 1); QCOMPARE(texts.size(), 1);
        QCOMPARE(files[0][4].toByteArray(), original); QCOMPARE(texts[0][1].toByteArray(), original);
        QVERIFY(files[0][5].toULongLong() != texts[0][2].toULongLong());
        bar.submissionResult(texts[0][2].toULongLong(), true); QVERIFY(editor->toPlainText().isEmpty());
        bar.submissionResult(files[0][5].toULongLong(), false);
        QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(files.size(), 2); QCOMPARE(texts.size(), 1);
        QCOMPARE(files[1][4].toByteArray(), original);
        bar.submissionResult(files[1][5].toULongLong(), true);
        editor->setPlainText("next"); QTest::keyClick(editor, Qt::Key_Return); QVERIFY(texts[1][1].toByteArray().isEmpty());
    }

    void replyResultKeepsTextWrittenWhileSending() {
        InputBar bar(nullptr, nullptr, nullptr); bar.show(); bar.setRecordingContext("/tmp", "123");
        auto* editor = bar.findChild<QTextEdit*>(); QVERIFY(editor);
        const QByteArray original(32, 'a'); bar.setReply(original, "Alice", "Original");
        QSignalSpy sends(&bar, &InputBar::sendMessage);
        editor->setPlainText("first"); QTest::keyClick(editor, Qt::Key_Return);
        editor->setPlainText("second"); bar.submissionResult(sends[0][2].toULongLong(), true);
        QCOMPARE(editor->toPlainText(), QString("second")); QTest::keyClick(editor, Qt::Key_Return);
        QCOMPARE(sends.size(), 2); QCOMPARE(sends[1][1].toByteArray(), original);
    }

    void initTestCase() {
        debug_config_init(); debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_DEBUG);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    }

    void previewBeforeExplicitSend_data() {
        QTest::addColumn<bool>("history");
        QTest::addColumn<bool>("dm");
        QTest::newRow("group-input") << false << false;
        QTest::newRow("group-history") << true << false;
        QTest::newRow("dm-input") << false << true;
        QTest::newRow("dm-history") << true << true;
    }
    void previewBeforeExplicitSend() {
        QFETCH(bool, history); QFETCH(bool, dm);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString path = makeFile(dir, QString::fromUtf8("файл # 100%.txt")); QVERIFY(!path.isEmpty());
        InputBar bar(nullptr, nullptr, nullptr); bar.resize(600, 150); bar.show();
        const QString target = dm ? "dm:0123456789abcdef" : "7206723622466219923";
        bar.setRecordingContext(dir.filePath("media"), target); bar.setDmMode(dm);
        auto* editor = bar.findChild<QTextEdit*>(); QVERIFY(editor);
        editor->setPlainText("draft");
        QListView view; view.resize(600, 400); view.show(); bar.installFileDropTarget(view.viewport());
        QSignalSpy files(&bar, &InputBar::sendFileAttachment);
        QSignalSpy texts(&bar, &InputBar::sendMessage);
        QVERIFY(dropUrls(history ? view.viewport() : editor->viewport(), {QUrl::fromLocalFile(path)}));
        QCOMPARE(files.count(), 0); QCOMPARE(texts.count(), 0); QCOMPARE(editor->toPlainText(), QString("draft"));
        QTest::keyClick(editor, Qt::Key_Return);
        QCOMPARE(files.count(), 1); QCOMPARE(texts.count(), 1);
        QCOMPARE(files.at(0).at(0).toString(), target); QCOMPARE(files.at(0).at(1).toString(), path);
        QCOMPARE(files.at(0).at(3).toString(), QFileInfo(path).fileName());
        QCOMPARE(texts.at(0).at(0).toString(), QString("draft"));
        QVERIFY(QFile::exists(path)); // CopyAction не удаляет исходный файл.
    }

    void rejectInvalidDropsWithoutReplacingTheDraft_data() {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"multiple", "remote", "directory", "missing", "empty"}) QTest::newRow(kind) << QString(kind);
    }
    void rejectInvalidDropsWithoutReplacingTheDraft() {
        QFETCH(QString, kind);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString first = makeFile(dir, "first.txt"); QVERIFY(!first.isEmpty());
        InputBar bar(nullptr, nullptr, nullptr); bar.show(); bar.setRecordingContext(dir.filePath("media"), "123");
        auto* editor = bar.findChild<QTextEdit*>(); QVERIFY(editor);
        QVERIFY(dropUrls(editor->viewport(), {QUrl::fromLocalFile(first)}));
        editor->setPlainText("draft");
        QSignalSpy rejected(&bar, &InputBar::attachmentRejected);
        QSignalSpy files(&bar, &InputBar::sendFileAttachment);
        QList<QUrl> urls;
        if (kind == "multiple") urls = {QUrl::fromLocalFile(first), QUrl::fromLocalFile(first)};
        else if (kind == "remote") urls = {QUrl("https://example.invalid/file.txt")};
        else if (kind == "directory") urls = {QUrl::fromLocalFile(dir.path())};
        else if (kind == "missing") urls = {QUrl::fromLocalFile(dir.filePath("missing.txt"))};
        else urls = {QUrl::fromLocalFile(makeFile(dir, "empty.txt", {}))};
        dropUrls(editor->viewport(), urls);
        QCOMPARE(rejected.count(), 1); QCOMPARE(editor->toPlainText(), QString("draft"));
        QTest::keyClick(editor, Qt::Key_Return);
        QCOMPARE(files.count(), 1); QCOMPARE(files.at(0).at(1).toString(), first);
    }

    void queuedDropCannotFollowAChangedAccountOrChannel_data() {
        QTest::addColumn<bool>("account");
        QTest::newRow("channel-switch-and-back") << false;
        QTest::newRow("account-switch-and-back") << true;
    }
    void queuedDropCannotFollowAChangedAccountOrChannel() {
        QFETCH(bool, account);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString path = makeFile(dir, "test.txt");
        InputBar bar(nullptr, nullptr, nullptr); bar.show(); bar.setRecordingContext(dir.filePath("media"), "123");
        auto* editor = bar.findChild<QTextEdit*>(); QVERIFY(editor);
        QSignalSpy rejected(&bar, &InputBar::attachmentRejected);
        QSignalSpy files(&bar, &InputBar::sendFileAttachment);
        QVERIFY(dropUrls(editor->viewport(), {QUrl::fromLocalFile(path)}, false));
        bar.setRecordingContext(dir.filePath(account ? "other-account" : "media"), account ? "123" : "456");
        bar.setRecordingContext(dir.filePath("media"), "123");
        QCoreApplication::processEvents();
        QCOMPARE(rejected.count(), 1);
        QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(files.count(), 0);
    }

    void pendingFileClearedWhenChatChanges() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        InputBar bar(nullptr, nullptr, nullptr); bar.show(); bar.setRecordingContext(dir.filePath("media"), "123");
        auto* editor = bar.findChild<QTextEdit*>(); QVERIFY(editor);
        QVERIFY(dropUrls(editor->viewport(), {QUrl::fromLocalFile(makeFile(dir, "test.txt"))}));
        bar.setRecordingContext(dir.filePath("media"), "456");
        QSignalSpy files(&bar, &InputBar::sendFileAttachment);
        QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(files.count(), 0);
    }

    void disabledOrUnselectedChatDoesNotAcceptFiles() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        InputBar bar(nullptr, nullptr, nullptr); bar.show();
        auto* editor = bar.findChild<QTextEdit*>(); QVERIFY(editor);
        const QList<QUrl> urls{QUrl::fromLocalFile(makeFile(dir, "test.txt"))};
        QVERIFY(!dropUrls(editor->viewport(), urls));
        bar.setRecordingContext(dir.filePath("media"), "123"); bar.setMediaEnabled(false);
        QVERIFY(!dropUrls(editor->viewport(), urls));
        QCOMPARE(editor->toPlainText(), QString());
    }

    void dragHighlightLeavesNoWidgetWhenComposerIsDestroyed() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QListView view; view.resize(600, 400); view.show();
        auto* bar = new InputBar(nullptr, nullptr, nullptr); bar->show();
        bar->setRecordingContext(dir.filePath("media"), "123"); bar->installFileDropTarget(view.viewport());
        QMimeData mime; mime.setUrls({QUrl::fromLocalFile(makeFile(dir, "test.txt"))});
        QDragEnterEvent enter(QPoint(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &enter); QVERIFY(enter.isAccepted());
        QPointer<QLabel> hint = view.viewport()->findChild<QLabel*>();
        QVERIFY(hint); QVERIFY(!hint->isHidden()); QVERIFY(hint->testAttribute(Qt::WA_TransparentForMouseEvents));
        QDragLeaveEvent leave; QApplication::sendEvent(view.viewport(), &leave); QVERIFY(hint->isHidden());
        QApplication::sendEvent(view.viewport(), &enter); QVERIFY(!hint->isHidden());
        delete bar; QVERIFY(hint.isNull());
    }

    void queuedDropIsCancelledWhenComposerIsDestroyed() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        auto* bar = new InputBar(nullptr, nullptr, nullptr); bar->show(); bar->setRecordingContext(dir.filePath("media"), "123");
        auto* editor = bar->findChild<QTextEdit*>(); QVERIFY(editor);
        QVERIFY(dropUrls(editor->viewport(), {QUrl::fromLocalFile(makeFile(dir, "test.txt"))}, false));
        delete bar; QCoreApplication::processEvents();
        QVERIFY(!QDir(dir.filePath("media")).exists());
    }

    void filePickerUsesTheSamePreparationAndContextGuard_data() {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"file", "cancel", "changed-chat"}) QTest::newRow(mode) << QString(mode);
    }
    void filePickerUsesTheSamePreparationAndContextGuard() {
        QFETCH(QString, mode);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString first = makeFile(dir, "first.txt"), second = makeFile(dir, "second.txt");
        InputBar bar(nullptr, nullptr, nullptr); bar.show(); bar.setRecordingContext(dir.filePath("media"), "123");
        auto* editor = bar.findChild<QTextEdit*>(); QVERIFY(editor);
        QVERIFY(dropUrls(editor->viewport(), {QUrl::fromLocalFile(first)}));
        QSignalSpy files(&bar, &InputBar::sendFileAttachment), rejected(&bar, &InputBar::attachmentRejected);
        QTimer timer; timer.setInterval(1);
        connect(&timer, &QTimer::timeout, &bar, [&] {
            auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            timer.stop();
            if (mode == "cancel") { dialog->reject(); return; }
            if (mode == "changed-chat") bar.setRecordingContext(dir.filePath("media"), "456");
            dialog->selectFile(second);
            QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
        });
        timer.start(); QVERIFY(QMetaObject::invokeMethod(&bar, "onAttachClicked", Qt::DirectConnection));
        QTest::keyClick(editor, Qt::Key_Return);
        QCOMPARE(files.count(), mode == "changed-chat" ? 0 : 1);
        QCOMPARE(rejected.count(), mode == "changed-chat" ? 1 : 0);
        if (!files.isEmpty()) QCOMPARE(files.at(0).at(1).toString(), mode == "cancel" ? first : second);
    }

    void imageHasTypedMetadataInGroupAndPm_data() {
        QTest::addColumn<bool>("dm");
        QTest::addColumn<QString>("format");
        QTest::addColumn<bool>("encrypted");
        for (bool dm : {false, true}) for (bool encrypted : {false, true}) for (const QString& format : {QString("PNG"), QString("JPEG")}) {
            if (dm && encrypted) continue;
            QTest::newRow(qPrintable(QString("%1-%2-%3").arg(dm ? "pm" : "group", format).arg(encrypted))) << dm << format << encrypted;
        }
    }
    void imageHasTypedMetadataInGroupAndPm() {
        QFETCH(bool, dm); QFETCH(QString, format); QFETCH(bool, encrypted);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        InputBar bar(nullptr, nullptr, nullptr); bar.show();
        bar.setRecordingContext(dir.filePath("media"), dm ? "dm:456" : "123", dm ? 7 : 0); bar.setDmMode(dm);
        auto* editor = bar.findChild<QTextEdit*>(); QVERIFY(editor);
        const QString path = dir.filePath("фото.PNG"); // Фактический JPEG должен победить расширение PNG.
        QImage image(120, 80, QImage::Format_ARGB32); image.fill(Qt::red); QVERIFY(image.save(path, qPrintable(format)));
        int sends = 0;
        connect(&bar, &InputBar::submitAttachment, &bar, [&](const QString& target, attachment_send_req* request) {
            ++sends; QCOMPARE(target, dm ? QString("dm:456") : QString("123"));
            QCOMPARE(request->info.kind, format == "PNG" ? ATTACHMENT_PNG : ATTACHMENT_JPEG);
            QCOMPARE(request->info.width, 120); QCOMPARE(request->info.height, 80);
            QCOMPARE(request->dm_generation, dm ? UINT64_C(7) : UINT64_C(0)); QCOMPARE(request->encrypted, (int)encrypted);
            QCOMPARE(QString::fromUtf8(request->source), path); QVERIFY(!request->temporary_source);
            const auto id = request->request_id; attachment_send_discard(request); bar.submissionResult(id, true);
        });
        QVERIFY(dropUrls(editor->viewport(), {QUrl::fromLocalFile(path)}));
        if (encrypted) { auto* check = bar.findChild<QCheckBox*>(); QVERIFY(check); check->setChecked(true); }
        QTest::keyClick(editor, Qt::Key_Return);
        QCOMPARE(sends, 1); QVERIFY(QFile::exists(path));
    }

    void corruptImageIsRejectedAndPreservesSelection() {
        QTemporaryDir dir; InputBar bar(nullptr, nullptr, nullptr); bar.show(); bar.setRecordingContext(dir.path(), "123");
        auto* editor = bar.findChild<QTextEdit*>(); QVERIFY(editor);
        QSignalSpy errors(&bar, &InputBar::attachmentRejected);
        int sends = 0; connect(&bar, &InputBar::submitAttachment, &bar, [&](const QString&, attachment_send_req* req) {
            ++sends; attachment_send_discard(req);
        });
        const QString path = makeFile(dir, "broken.jpg");
        QVERIFY(dropUrls(editor->viewport(), {QUrl::fromLocalFile(path)})); QTest::keyClick(editor, Qt::Key_Return);
        QCOMPARE(sends, 0); QVERIFY(!errors.isEmpty()); QVERIFY(QFile::exists(path));
    }

    void videoModeSelectionAndCancellation_data() {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"file", "video", "cancel", "changed-chat"}) QTest::newRow(mode) << QString(mode);
    }
    void videoModeSelectionAndCancellation() {
        QFETCH(QString, mode);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        InputBar bar(nullptr, nullptr, nullptr); bar.show(); bar.setRecordingContext(dir.filePath("media"), "123");
        auto* editor = bar.findChild<QTextEdit*>(); QVERIFY(editor);
        QSignalSpy files(&bar, &InputBar::sendFileAttachment), rejected(&bar, &InputBar::attachmentRejected);
        int videos = 0;
        connect(&bar, &InputBar::submitAttachment, &bar, [&](const QString& target, attachment_send_req* request) {
            QCOMPARE(target, QString("123")); QCOMPARE(request->info.kind, ATTACHMENT_VIDEO); QVERIFY(request->transcode);
            ++videos; u_free(request);
        });
        QVERIFY(dropUrls(editor->viewport(), {QUrl::fromLocalFile(makeFile(dir, "video.mp4"))}, false));
        QTimer timer; timer.setInterval(1);
        connect(&timer, &QTimer::timeout, &bar, [&] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!box) return;
            timer.stop();
            if (mode == "cancel") { box->reject(); return; }
            if (mode == "changed-chat") bar.setRecordingContext(dir.filePath("media"), "456");
            for (auto* button : box->buttons()) if (button->text() == (mode == "video" ? "As a video" : "As a file")) {
                button->click(); return;
            }
            box->reject();
        });
        timer.start(); QCoreApplication::processEvents();
        QTest::keyClick(editor, Qt::Key_Return);
        QCOMPARE(files.count(), mode == "file" ? 1 : 0); QCOMPARE(videos, mode == "video" ? 1 : 0);
        QCOMPARE(rejected.count(), mode == "changed-chat" ? 1 : 0);
    }
};

QTEST_MAIN(FileDropTest)
#include "test_file_drop.moc"
