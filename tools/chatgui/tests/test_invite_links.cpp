/* Проверка реальных координат ссылок и трёх кнопок действий в строке участника. */
#include "../src/messagedelegate.h"
#include "../src/chatview.h"
#include "../src/accountdelegate.h"
#include "../src/memberlistmodel.h"
#include "../src/message_links.h"
#include "../src/message_text.h"
#include "../../../lib/debug_config.h"
#include <QtTest>
#include <QStandardItemModel>
#include <QClipboard>
#include <QPainter>
#include <QDir>
#include <QScopedValueRollback>
#include <QTemporaryDir>
extern "C" {
#include "chat/invite_link.h"
#include "transport_layer/secure_channel.h"
#include "chat/chat_member.h"
}

class InviteLinksTest : public QObject {
    Q_OBJECT
private slots:
    void formattedMessageSelection_data() {
        QTest::addColumn<bool>("outgoing"); QTest::addColumn<int>("width"); QTest::addColumn<bool>("dark");
        for (bool outgoing : {false, true}) for (int width : {280, 700}) for (bool dark : {false, true}) {
            const auto name = QString("out=%1 width=%2 dark=%3").arg(outgoing).arg(width).arg(dark);
            QTest::newRow(qPrintable(name)) << outgoing << width << dark;
        }
    }
    void formattedMessageSelection() {
        QFETCH(bool, outgoing); QFETCH(int, width); QFETCH(bool, dark);
        QScopedValueRollback<debug_level_t> logging(g_debug_config.category_levels[DEBUG_CATEGORY_GENERAL], DEBUG_LEVEL_DEBUG);
        const QString source = "**Bold** __italic__ ~~gone~~\n```cpp\nint x = 42;\n// https://example.org\n```\nhttps://example.com";
        const QString visible = "Bold italic gone\nint x = 42;\n// https://example.org\nhttps://example.com";
        QStandardItemModel model;
        for (int i = 0; i < 15; ++i) {
            auto* item = new QStandardItem;
            item->setData(i == 14 ? source : QString("Previous message"), MsgTextRole);
            item->setData("text/plain", MsgContentTypeRole); item->setData(outgoing, MsgIsOutgoingRole);
            item->setData("Alice", MsgAuthorRole); model.appendRow(item);
        }
        ChatView view; MessageDelegate delegate;
        view.setModel(&model); view.setItemDelegate(&delegate); view.setEditTriggers(QAbstractItemView::NoEditTriggers);
        view.setSelectionMode(QAbstractItemView::NoSelection); view.setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        QPalette palette = view.palette(); palette.setColor(QPalette::Text, QColor(200, 0, 200));
        palette.setColor(QPalette::Window, dark ? QColor(40, 40, 40) : QColor(Qt::white)); view.setPalette(palette);
        view.setLinkHitTest([&](const QModelIndex& index, const QPoint& point) {
            QStyleOptionViewItem opt; opt.initFrom(&view); opt.widget = &view; opt.rect = view.visualRect(index);
            return delegate.linkAt(opt, index, point);
        });
        view.resize(width, 450); view.show(); QTest::qWait(20); view.scrollToBottom(); QTest::qWait(20);
        const auto index = model.index(14, 0); const auto prepared = formattedMessageText(index); QVERIFY(prepared);
        QCOMPARE(prepared->text, visible); QVERIFY(!prepared->lightSyntax.isEmpty());
        QStyleOptionViewItem option; option.initFrom(&view); option.widget = &view; option.rect = view.visualRect(index);
        QVERIFY(view.viewport()->rect().contains(option.rect));
        QImage image(view.viewport()->size(), QImage::Format_ARGB32); image.fill(Qt::white);
        { QPainter painter(&image); delegate.paint(&painter, option, index); }
        // Начало берём с реально нарисованной первой строки, конец — с последней кликабельной ссылки.
        QPoint end; QRect firstLine;
        for (int y = option.rect.top(); y <= option.rect.bottom(); ++y) {
            QRect ink;
            for (int x = 0; x < image.width(); ++x) {
                const auto color = image.pixelColor(x, y);
                if (color.red() > 150 && color.blue() > 150 && color.green() < 60) ink |= QRect(x, y, 1, 1);
            }
            if (ink.isEmpty() && !firstLine.isEmpty()) break;
            firstLine |= ink;
        }
        QVERIFY(!firstLine.isEmpty()); const QPoint start(firstLine.left() - 3, firstLine.center().y());
        QSet<QString> links;
        for (int y = option.rect.top(); y <= option.rect.bottom(); y += 5) {
            for (int x = 5; x < image.width(); x += 12) {
                const auto hit = delegate.linkAt(option, index, QPoint(x, y));
                if (hit.isEmpty()) continue;
                links.insert(hit); end = QPoint(image.width() - 5, y);
            }
        }
        QCOMPARE(links, QSet<QString>{"https://example.com"}); QVERIFY(!end.isNull());
        QSignalSpy activated(&view, &ChatView::linkActivated);
        for (bool reverse : {false, true}) {
            QPoint from = reverse ? end : start, to = reverse ? start : end;
            QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, from);
            QMouseEvent drag(QEvent::MouseMove, to, view.viewport()->mapToGlobal(to), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view.viewport(), &drag);
            QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, to);
            { QPainter painter(&image); delegate.paint(&painter, option, index); }
            QTest::keyClick(&view, Qt::Key_C, Qt::ControlModifier);
            QCOMPARE(QApplication::clipboard()->text(), visible); QCOMPARE(activated.count(), 0);
            QCOMPARE(index.data(MsgTextRole).toString(), source);
        }
        if (outgoing && width == 700 && dark) image.save(QDir::tempPath() + "/chat-markup-selection.png");
    }

    void multilineSelectionFollowsPaintedText_data() {
        QTest::addColumn<bool>("outgoing"); QTest::addColumn<bool>("blankLine"); QTest::addColumn<bool>("reverse");
        for (bool outgoing : {false, true}) for (bool blankLine : {false, true}) for (bool reverse : {false, true}) {
            const auto name = QString("outgoing=%1 blank=%2 reverse=%3").arg(outgoing).arg(blankLine).arg(reverse);
            QTest::newRow(qPrintable(name)) << outgoing << blankLine << reverse;
        }
    }

    void multilineSelectionFollowsPaintedText() {
        QFETCH(bool, outgoing); QFETCH(bool, blankLine); QFETCH(bool, reverse);
        QScopedValueRollback<debug_level_t> logging(g_debug_config.category_levels[DEBUG_CATEGORY_GENERAL], DEBUG_LEVEL_DEBUG);
        const QStringList paragraphs = {"aaaaaaaaaa", "bbbbbbbbbb", "cccccccccc"};
        const QString text = paragraphs.join(blankLine ? "\n\n" : "\n");
        QStandardItemModel model;
        for (int row = 0; row < 12; ++row) {
            auto* item = new QStandardItem;
            item->setData(row == 11 ? text : QString("Previous message"), MsgTextRole);
            item->setData("text/plain", MsgContentTypeRole); item->setData(outgoing, MsgIsOutgoingRole);
            item->setData("Alice", MsgAuthorRole); model.appendRow(item);
        }
        ChatView view; MessageDelegate delegate;
        view.setModel(&model); view.setItemDelegate(&delegate);
        view.setSelectionMode(QAbstractItemView::NoSelection); view.setEditTriggers(QAbstractItemView::NoEditTriggers);
        view.setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        QPalette palette = view.palette(); palette.setColor(QPalette::Text, QColor(200, 0, 200));
        palette.setColor(QPalette::Window, Qt::white); view.setPalette(palette);
        view.resize(420, 300); view.show(); QTest::qWait(20); view.scrollToBottom(); QTest::qWait(20);
        const auto index = model.index(11, 0);
        QStyleOptionViewItem option; option.initFrom(&view); option.widget = &view; option.rect = view.visualRect(index);
        QVERIFY(option.rect.top() > 0); QVERIFY(view.viewport()->rect().contains(option.rect));
        auto render = [&] {
            QImage image(view.viewport()->size(), QImage::Format_ARGB32); image.fill(Qt::white);
            QPainter painter(&image); delegate.paint(&painter, option, index); return image;
        };
        const QImage original = render();
        // Находим реальные строки по пикселям текста, независимо от расчёта выделения.
        QVector<QRect> lines;
        for (int y = option.rect.top(); y <= option.rect.bottom(); ++y) {
            QRect ink;
            for (int x = 0; x < original.width(); ++x) {
                const QColor color = original.pixelColor(x, y);
                if (color.red() > 150 && color.blue() > 150 && color.green() < 60) ink |= QRect(x, y, 1, 1);
            }
            if (ink.isEmpty()) continue;
            if (lines.isEmpty() || lines.last().bottom() + 1 < y) lines.append(ink);
            else lines.last() |= ink;
        }
        QCOMPARE(lines.size(), paragraphs.size());
        for (int line = 0; line < lines.size(); ++line) {
            const QRect glyphs = lines[line];
            QPoint start(glyphs.left() - 2, glyphs.center().y()), end(glyphs.right() + 3, glyphs.center().y());
            if (reverse) qSwap(start, end);
            QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, start);
            QMouseEvent drag(QEvent::MouseMove, end, view.viewport()->mapToGlobal(end), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view.viewport(), &drag);
            QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, end);
            const QImage selected = render();
            QRect highlight;
            for (int y = 0; y < selected.height(); ++y) for (int x = 0; x < selected.width(); ++x) {
                if (original.pixel(x, y) != selected.pixel(x, y)) highlight |= QRect(x, y, 1, 1);
            }
            const auto selection = view.textSelection();
            DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL,
                "selection regression: line=%d glyphs=%d,%d %dx%d highlight=%d,%d %dx%d chars=%d..%d",
                line, glyphs.x(), glyphs.y(), glyphs.width(), glyphs.height(),
                highlight.x(), highlight.y(), highlight.width(), highlight.height(), selection.startChar, selection.endChar);
            QTest::keyClick(&view, Qt::Key_C, Qt::ControlModifier);
            QCOMPARE(QApplication::clipboard()->text(), paragraphs[line]);
            QVERIFY2(highlight.contains(glyphs), "Highlight must cover the painted text on the selected line");
            QVERIFY(highlight.height() <= QFontMetrics(view.font()).height() + 2);
        }
    }

    void preparedMediaPaint_data() {
        QTest::addColumn<QString>("type"); QTest::newRow("image") << QString("image/png"); QTest::newRow("video") << QString("video/mp4");
    }
    void preparedMediaPaint() {
        QFETCH(QString, type); QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QStandardItemModel model; auto* item = new QStandardItem;
        item->setData(type, MsgContentTypeRole); item->setData("Alice", MsgAuthorRole); item->setData("12:34", MsgTimeRole);
        item->setData(temporary.filePath("removed-original"), MsgVoiceFileRole); item->setData(2, MsgFileDownloadStateRole);
        item->setData(1234, MsgFileSizeRole); item->setData(120, MsgImageWidthRole); item->setData(80, MsgImageHeightRole);
        const QColor green(35, 170, 95); QImage preview(120, 80, QImage::Format_RGB32); preview.fill(green);
        item->setData(preview, MsgImagePreviewRole); model.appendRow(item);
        MessageDelegate delegate; QStyleOptionViewItem option; option.font = QApplication::font(); option.rect = QRect(0, 0, 600, 400);
        QImage output(option.rect.size(), QImage::Format_ARGB32); output.fill(Qt::white);
        QPainter painter(&output); delegate.paint(&painter, option, model.index(0, 0)); painter.end();
        int preparedPixels = 0;
        for (int y = 0; y < output.height(); ++y) for (int x = 0; x < output.width(); ++x)
            if (output.pixelColor(x, y) == green) ++preparedPixels;
        QVERIFY(preparedPixels > 100); // Готовые пиксели рисуются даже без оригинала и соседнего .jpg.
    }
    void replyClick_data() {
        QTest::addColumn<QString>("type"); QTest::addColumn<bool>("outgoing"); QTest::addColumn<int>("width");
        for (const auto* type : {"text/plain", "audio/opus", "image/png", "video/mp4", "application/octet-stream", "poll", "emoji"})
            for (bool outgoing : {false, true}) for (int width : {280, 600})
                QTest::newRow(qPrintable(QString("%1-%2-%3").arg(type).arg(outgoing).arg(width))) << QString(type) << outgoing << width;
    }

    void replyClick() {
        QFETCH(QString, type); QFETCH(bool, outgoing); QFETCH(int, width);
        QStandardItemModel model; auto* message = new QStandardItem;
        message->setData(type == "emoji" ? QString::fromUtf8("😀") : QString("Answer"), MsgTextRole);
        message->setData(type == "emoji" ? "text/plain" : type, MsgContentTypeRole);
        message->setData(outgoing, MsgIsOutgoingRole); message->setData("1001", MsgChannelIdRole);
        message->setData(QString(64, 'a'), MsgMessageKeyRole); message->setData("Alice", MsgAuthorRole);
        message->setData("12:34", MsgTimeRole); message->setData("example.txt", MsgFileDisplayNameRole);
        message->setData("0.5s", MsgVoiceDurationRole); message->setData(QString(64, 'b'), MsgReplyToRole);
        message->setData("Bob", MsgQuoteAuthorRole); message->setData(QString(2000, 'x'), MsgQuoteTextRole);
        model.appendRow(message);
        ChatView view; MessageDelegate delegate; view.setModel(&model); view.setItemDelegate(&delegate);
        view.setSelectionMode(QAbstractItemView::NoSelection); view.resize(width, 500); view.show(); QTest::qWait(10);
        const auto index = model.index(0, 0); QStyleOptionViewItem option; option.initFrom(&view); option.widget = &view;
        const int before = delegate.sizeHint(option, index).height();
        message->setData(true, MsgHasQuoteRole); view.doItemsLayout();
        option.rect = view.visualRect(index);
        QVERIFY(delegate.sizeHint(option, index).height() > before);
        QVERIFY(delegate.sizeHint(option, index).height() < before + 120);
        QSignalSpy replies(&delegate, &MessageDelegate::replyRequested), polls(&delegate, &MessageDelegate::pollRequested);
        int downloads = 0; delegate.setDownloadCallback([&](const QModelIndex&) { downloads++; return true; });
        // Верхняя цитата у медиа/emoji; у текста она ниже имени автора.
        const bool text = type == "text/plain" || type == "poll";
        const int y = text ? 8 + (outgoing ? 0 : QFontMetrics(view.font()).height() + 2) + 6 : 6;
        const int x = width < 480 ? view.viewport()->width() / 2 : outgoing ? view.viewport()->width() - 170 : 170;
        const QPoint quote(x, y);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, quote);
        QCOMPARE(replies.size(), 1); QCOMPARE(replies[0][0].toString(), QString(64, 'b'));
        QCOMPARE(downloads, 0); QCOMPARE(polls.size(), 0);
        QTest::mouseClick(view.viewport(), Qt::RightButton, Qt::NoModifier, quote); QCOMPARE(replies.size(), 1);
        QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, quote);
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, quote + QPoint(30, 0));
        QCOMPARE(replies.size(), 1);
        QImage image(option.rect.size(), QImage::Format_ARGB32); image.fill(Qt::transparent);
        QPainter painter(&image); delegate.paint(&painter, option, index); painter.end();
    }

    void initTestCase() { debug_config_init(); }
    void pollLeftClick_data() {
        QTest::addColumn<bool>("outgoing"); QTest::addColumn<int>("width");
        for (bool outgoing : {false, true}) for (int width : {280, 600})
            QTest::newRow(qPrintable(QString("%1-%2").arg(outgoing).arg(width))) << outgoing << width;
    }

    void pollLeftClick() {
        QFETCH(bool, outgoing); QFETCH(int, width);
        QScopedValueRollback<debug_level_t> logging(g_debug_config.category_levels[DEBUG_CATEGORY_GENERAL], DEBUG_LEVEL_INFO);
        QStandardItemModel model;
        auto* message = new QStandardItem;
        const QString key(64, 'a');
        message->setData("Which of these desktop options should everyone choose today?\nOption A\nOption B", MsgTextRole);
        message->setData("poll", MsgContentTypeRole); message->setData(outgoing, MsgIsOutgoingRole);
        message->setData("1001", MsgChannelIdRole); message->setData(key, MsgMessageKeyRole);
        message->setData("Alice", MsgAuthorRole); message->setData("12:34", MsgTimeRole);
        model.appendRow(message);
        ChatView view; MessageDelegate delegate;
        view.setModel(&model); view.setItemDelegate(&delegate); view.setFrameShape(QFrame::NoFrame);
        view.setSelectionMode(QAbstractItemView::NoSelection); view.setEditTriggers(QAbstractItemView::NoEditTriggers);
        QFont font = view.font(); font.setPointSize(10); view.setFont(font);
        view.resize(width, 400); view.show(); QTest::qWait(20);
        QSignalSpy polls(&delegate, &MessageDelegate::pollRequested);
        const QModelIndex index = model.index(0, 0);
        const QPoint body(view.viewport()->width() / 2, QFontMetrics(view.font()).height() + 12);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, body);
        QCOMPARE(polls.count(), 1); QCOMPARE(polls.first().first().toString(), key);

        QTest::mouseClick(view.viewport(), Qt::RightButton, Qt::NoModifier, body);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(0, body.y()));
        const QPoint footer(body.x(), view.visualRect(index).bottom() - 10);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, footer);
        if (width == 600) {
            const QPoint avatar(outgoing ? view.viewport()->width() - 35 : 35, view.visualRect(index).bottom() - 30);
            QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, avatar);
        }
        QCOMPARE(polls.count(), 1);

        QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, body);
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, body + QPoint(20, 0));
        QCOMPARE(polls.count(), 1);
        QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, body);
        for (const QPoint point : {body + QPoint(20, 0), body}) {
            QMouseEvent drag(QEvent::MouseMove, point, view.viewport()->mapToGlobal(point), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view.viewport(), &drag);
        }
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, body);
        QVERIFY(view.hasSelection()); QCOMPARE(polls.count(), 1); // Возврат мыши не превращает выделение в клик.
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, body);
        QCOMPARE(polls.count(), 2);
        message->setData(QString(), MsgMessageKeyRole);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, body);
        QCOMPARE(polls.count(), 2);
        message->setData(key, MsgMessageKeyRole); message->setData("text/plain", MsgContentTypeRole);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, body);
        QCOMPARE(polls.count(), 2);
    }

    void pollEmojiQuestion() {
        QStandardItemModel model;
        auto* message = new QStandardItem;
        const QString key(64, 'a');
        message->setData(QString::fromUtf8("😀"), MsgTextRole); message->setData("poll", MsgContentTypeRole);
        message->setData("1001", MsgChannelIdRole); message->setData(key, MsgMessageKeyRole);
        model.appendRow(message);
        ChatView view; MessageDelegate delegate;
        view.setModel(&model); view.setItemDelegate(&delegate); view.setFrameShape(QFrame::NoFrame);
        view.setSelectionMode(QAbstractItemView::NoSelection); view.resize(300, 250); view.show(); QTest::qWait(20);
        QStyleOptionViewItem option; option.initFrom(&view); option.widget = &view; option.rect = view.visualRect(model.index(0, 0));
        const QColor background(1, 2, 3);
        QImage image(option.rect.size(), QImage::Format_ARGB32); image.fill(background);
        QPainter painter(&image); delegate.paint(&painter, option, model.index(0, 0)); painter.end();
        qint64 xTotal = 0, yTotal = 0, pixels = 0;
        for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y) == background) continue;
            xTotal += x; yTotal += y; ++pixels;
        }
        QVERIFY(pixels > 0);
        QSignalSpy polls(&delegate, &MessageDelegate::pollRequested);
        const QPoint question(int(xTotal / pixels), int(yTotal / pixels)); // Нажимаем на реально нарисованный вопрос.
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, question);
        QCOMPARE(polls.count(), 1); QCOMPARE(polls.first().first().toString(), key);
    }

    void disabledReactionsPreserveMessageAndTimeWithoutClickableHeart() {
        QStandardItemModel model; auto* message = new QStandardItem;
        message->setData("Message in a card", MsgTextRole); message->setData("text/plain", MsgContentTypeRole);
        message->setData("1001", MsgChannelIdRole); message->setData(QString(64, 'a'), MsgMessageKeyRole);
        message->setData("Alice", MsgAuthorRole); message->setData("12:34", MsgTimeRole); model.appendRow(message);
        ChatView view; MessageDelegate delegate; delegate.setReactionsEnabled(false);
        view.setModel(&model); view.setItemDelegate(&delegate); view.resize(340, 240); view.show(); QTest::qWait(10);
        const auto index = model.index(0, 0); QStyleOptionViewItem option; option.initFrom(&view); option.widget = &view;
        option.rect = view.visualRect(index); const int height = delegate.sizeHint(option, index).height();
        QImage disabled(view.viewport()->width(), height, QImage::Format_ARGB32); disabled.fill(Qt::transparent);
        { QPainter painter(&disabled); delegate.paint(&painter, option, index); }
        QSignalSpy reactions(&delegate, &MessageDelegate::reactionRequested);
        for (int y = 1; y < height; y += 4) for (int x = 1; x < disabled.width(); x += 4) {
            const QPoint point(x, y);
            QMouseEvent press(QEvent::MouseButtonPress, point, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, point, point, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            delegate.editorEvent(&press, &model, option, index); delegate.editorEvent(&release, &model, option, index);
        }
        QCOMPARE(reactions.size(), 0);
        // Совпадает с обычным сообщением вне групповых реакций, включая строку времени.
        message->setData(QString(), MsgChannelIdRole); MessageDelegate plain;
        QCOMPARE(plain.sizeHint(option, index).height(), height);
        QImage expected(disabled.size(), disabled.format()); expected.fill(Qt::transparent);
        { QPainter painter(&expected); plain.paint(&painter, option, index); }
        QCOMPARE(disabled, expected);
    }

    void reactionFooter_data() {
        QTest::addColumn<QString>("type"); QTest::addColumn<bool>("outgoing"); QTest::addColumn<int>("width");
        for (const auto& type : {"text/plain", "audio/opus", "image/png", "video/mp4", "application/octet-stream", "poll"})
            for (bool outgoing : {false, true}) for (int width : {280, 600}) {
                const QByteArray name = QString("%1-%2-%3").arg(type).arg(outgoing).arg(width).toUtf8();
                QTest::newRow(name.constData()) << QString(type) << outgoing << width;
            }
    }

    void reactionFooter() {
        QFETCH(QString, type); QFETCH(bool, outgoing); QFETCH(int, width);
        QStandardItemModel model; auto* message = new QStandardItem;
        message->setData(type == "text/plain" || type == "poll" ? QString("Example message") : QString::fromUtf8("😀"), MsgTextRole);
        message->setData(type, MsgContentTypeRole); message->setData(outgoing, MsgIsOutgoingRole);
        message->setData("1001", MsgChannelIdRole); message->setData(QString(64, 'a'), MsgMessageKeyRole);
        message->setData("Alice", MsgAuthorRole); message->setData("12:34", MsgTimeRole);
        message->setData("00:05", MsgVoiceDurationRole); message->setData("example.dat", MsgFileDisplayNameRole);
        message->setData(1, MsgIdRole); message->setData(0, MsgFileDownloadStateRole);
        message->setData(QVariantList{QVariantMap{{"id", 7}, {"emoji", QString::fromUtf8("👍")}, {"count", 2}, {"mine", true}}}, MsgReactionsRole);
        model.appendRow(message);
        ChatView view; MessageDelegate delegate; view.setModel(&model); view.setItemDelegate(&delegate);
        view.setSelectionMode(QAbstractItemView::NoSelection); view.resize(width, 480); view.show(); QTest::qWait(10);
        int downloads = 0; delegate.setDownloadCallback([&](const QModelIndex&) { downloads++; return true; });
        const auto index = model.index(0, 0); QStyleOptionViewItem option; option.initFrom(&view); option.widget = &view;
        option.rect = view.visualRect(index); option.rect.setHeight(delegate.sizeHint(option, index).height());
        option.palette.setColor(QPalette::Window, QColor(42, 52, 62)); option.palette.setColor(QPalette::Text, Qt::white);
        QSignalSpy requested(&delegate, &MessageDelegate::reactionRequested);
        QMap<int, QPoint> points;
        QSignalSpy menus(&delegate, &MessageDelegate::messageMenuRequested);
        QPoint menuPoint; bool hasMenuPoint = false;
        /* Ищем области по фактическим событиям, не повторяя вычисления layout. */
        for (int y = 1; y < option.rect.height(); y += 4) for (int x = 1; x < view.viewport()->width(); x += 4) {
            const QPoint point(x, y);
            QMouseEvent press(QEvent::MouseButtonPress, point, view.viewport()->mapToGlobal(point), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, point, view.viewport()->mapToGlobal(point), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            requested.clear(); menus.clear(); delegate.editorEvent(&press, &model, option, index); delegate.editorEvent(&release, &model, option, index);
            if (!requested.isEmpty()) {
                QCOMPARE(requested.count(), 1); const int emoji = requested.first().at(1).toInt();
                if (!points.contains(emoji)) points[emoji] = point;
                QVERIFY(emoji >= 0);
            }
            if (!menus.isEmpty()) { menuPoint = point; hasMenuPoint = true; }
        }
        QVERIFY(points.contains(7)); QVERIFY(!points.contains(-1)); QVERIFY(hasMenuPoint);
        menus.clear(); downloads = 0; requested.clear();
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, menuPoint);
        QCOMPARE(menus.count(), 1); QCOMPARE(requested.count(), 0);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, points[7]);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, points[7]);
        QCOMPARE(requested.count(), 2); QCOMPARE(requested.last().at(1).toInt(), 7); QCOMPARE(downloads, 0);
        QTest::mouseClick(view.viewport(), Qt::RightButton, Qt::NoModifier, menuPoint); QCOMPARE(requested.count(), 2);
        QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, menuPoint);
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, menuPoint + QPoint(20, 0));
        QCOMPARE(requested.count(), 2); QCOMPARE(downloads, 0);

        QCOMPARE(menus.count(), 1);
        const QColor background(70, 80, 90); QImage image(option.rect.size(), QImage::Format_ARGB32); image.fill(background);
        QPainter painter(&image); delegate.paint(&painter, option, index); painter.end();
        QVERIFY(image.pixelColor(points[7]) != background);

    }

    void largeEmojiHasNoReactionFooter() {
        QStandardItemModel model; auto* message = new QStandardItem;
        message->setData(QString::fromUtf8("😀"), MsgTextRole); message->setData("1001", MsgChannelIdRole); model.appendRow(message);
        ChatView view; MessageDelegate delegate; view.setModel(&model); view.setItemDelegate(&delegate); view.resize(600, 240);
        QStyleOptionViewItem option; option.initFrom(&view); option.widget = &view;
        const auto index = model.index(0, 0); const auto size = delegate.sizeHint(option, index);
        message->setData(QString(64, 'a'), MsgMessageKeyRole); QCOMPARE(delegate.sizeHint(option, index), size);
    }

    void multipleReactionsExpandBottomRow() {
        QStandardItemModel model; auto* message = new QStandardItem;
        message->setData("Message", MsgTextRole); message->setData("1001", MsgChannelIdRole);
        message->setData(QString(64, 'a'), MsgMessageKeyRole); message->setData("12:34", MsgTimeRole); model.appendRow(message);
        ChatView view; MessageDelegate delegate; view.setModel(&model); view.setItemDelegate(&delegate); view.resize(280, 480);
        view.show(); QTest::qWait(10); const auto index = model.index(0, 0); const int before = view.visualRect(index).height();
        QVariantList reactions;
        for (int i = 0; i < 16; i++) reactions.append(QVariantMap{{"id", i}, {"emoji", QString::fromUtf8("👍")}, {"count", 123}});
        message->setData(reactions, MsgReactionsRole);
        QTRY_VERIFY(view.visualRect(index).height() > before);
        QStyleOptionViewItem option; option.initFrom(&view); option.widget = &view;
        QCOMPARE(view.visualRect(index).height(), delegate.sizeHint(option, index).height());
        message->setData(QVariantList{}, MsgReactionsRole); QTRY_COMPARE(view.visualRect(index).height(), before);
    }

    void messageLinkBoundaries() {
        QFile fixture(QFINDTESTDATA("message_links_cases.tsv"));
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        int count = 0;
        for (const auto& line : QString::fromUtf8(fixture.readAll()).split('\n')) {
            if (line.isEmpty() || line.startsWith('#')) continue;
            auto columns = line.split('\t');
            QString text = columns.takeFirst(); text.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
            const auto links = messageLinks(text);
            QStringList urls;
            int end = 0;
            for (const auto& link : links) {
                urls.append(link.url);
                QVERIFY(link.start >= end);
                QCOMPARE(text.mid(link.start, link.length), link.url);
                end = link.start + link.length;
            }
            QCOMPARE(urls, columns);
            count++;
        }
        QVERIFY(count >= 15);
    }

    void longMessageLinkTail() {
        const QString first = QStringLiteral("https://example.com/a([0])");
        const QString second = QStringLiteral("http://example.org/b");
        const QString text = QString::fromUtf8("👋 ") + first + QStringLiteral(")]},.!?").repeated(20000) + ' ' + second;
        QElapsedTimer timer; timer.start();
        const auto links = messageLinks(text);
        const auto elapsed = timer.elapsed();
        qInfo() << "Message links:" << text.size() << "UTF-16 units parsed in" << elapsed << "ms";
        QCOMPARE(links.size(), 2);
        QCOMPARE(links.first().url, first);
        QCOMPARE(links.first().start, 3);
        QCOMPARE(links.last().url, second);
        QCOMPARE(text.mid(links.last().start, links.last().length), second);
        QVERIFY2(elapsed < 5000, "Message link parsing exceeded the UI regression guard");
    }

    void codeCopyConfirmation() {
        QStandardItemModel model(1, 1); const auto index = model.index(0, 0);
        model.setData(index, "```cpp\nint value = 42;\n```", MsgTextRole);
        ChatView view; MessageDelegate delegate;
        view.setModel(&model); view.setItemDelegate(&delegate); view.resize(400, 220); view.show(); QTest::qWait(30);
        QStyleOptionViewItem option; option.initFrom(&view); option.widget = &view; option.rect = view.visualRect(index);
        QPoint button; bool found = false;
        for (int y = 0; y < option.rect.height() && !found; y += 4) for (int x = 0; x < view.viewport()->width(); x += 8) {
            if (delegate.codeCopyAt(option, index, QPoint(x, y)) != 0) continue;
            button = QPoint(x, y); found = true; break;
        }
        QVERIFY(found);
        const auto original = view.viewport()->grab().toImage();
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, button);
        QCOMPARE(QApplication::clipboard()->text(), QString("int value = 42;"));
        const auto copied = view.viewport()->grab().toImage();
        QVERIFY(copied != original);
        QVERIFY(view.grab().save(QDir::tempPath() + "/code-copy-confirmation.png"));
        QTest::qWait(800);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, button);
        QTest::qWait(800); // Первый timeout уже прошёл, повторное нажатие должно продлить надпись.
        QCOMPARE(view.viewport()->grab().toImage(), copied);
        QTRY_COMPARE_WITH_TIMEOUT(view.viewport()->grab().toImage(), original, 2000);
        // Уничтожение строки и вида во время подтверждения безопасно.
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, button);
        model.clear(); QCoreApplication::processEvents();
        QCOMPARE(model.rowCount(), 0);
    }

    void codeBlockCopy_data() {
        QTest::addColumn<bool>("outgoing"); QTest::addColumn<bool>("dark"); QTest::addColumn<int>("width");
        for (bool outgoing : {false, true}) for (bool dark : {false, true}) for (int width : {280, 700})
            QTest::newRow(qPrintable(QString("%1-%2-%3").arg(outgoing).arg(dark).arg(width))) << outgoing << dark << width;
    }
    void codeBlockCopy() {
        QFETCH(bool, outgoing); QFETCH(bool, dark); QFETCH(int, width);
        const QString first = "\tint value = 42;\n\n// __literal__ https://example.org\n";
        const QString second = "  final value = 123456789;";
        const QString source = "before\n```cpp\n" + first + "\n```\nbetween\n```\n" + second + "\n```\n```\n```\nafter";
        QStandardItemModel model(1, 1); const auto index = model.index(0, 0);
        model.setData(index, source, MsgTextRole); model.setData(index, outgoing, MsgIsOutgoingRole);
        model.setData(index, "author", MsgAuthorRole);
        ChatView view; MessageDelegate delegate;
        view.setModel(&model); view.setItemDelegate(&delegate); view.resize(width, 650);
        view.setCodeCopyHitTest([&](const QModelIndex& item, const QPoint& point) {
            QStyleOptionViewItem opt; opt.initFrom(&view); opt.widget = &view; opt.rect = view.visualRect(item);
            return delegate.codeCopyAt(opt, item, point) >= 0;
        });
        auto palette = view.palette(); palette.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
        if (dark) {
            palette.setColor(QPalette::Base, QColor(28, 28, 28)); palette.setColor(QPalette::AlternateBase, QColor(38, 38, 38));
            palette.setColor(QPalette::Text, Qt::white); palette.setColor(QPalette::ButtonText, Qt::white);
            palette.setColor(QPalette::Button, QColor(55, 55, 55)); palette.setColor(QPalette::Mid, QColor(100, 100, 100));
        }
        view.setPalette(palette);
        view.show(); QTest::qWait(30);
        QStyleOptionViewItem option; option.initFrom(&view); option.widget = &view; option.rect = view.visualRect(index);
        QMap<int, QPoint> buttons;
        for (int y = 0; y < option.rect.height() && buttons.size() < 3; y += 4) {
            // Все Copy выровнены по правому краю: после первой найденной кнопки проверяем только её колонку.
            const int startX = buttons.isEmpty() ? 0 : buttons.first().x();
            const int endX = buttons.isEmpty() ? view.viewport()->width() : startX + 1;
            for (int x = startX; x < endX; x += 8) {
                const int block = delegate.codeCopyAt(option, index, QPoint(x, y));
                if (block >= 0 && !buttons.contains(block)) { buttons[block] = QPoint(x, y); break; }
            }
        }
        QCOMPARE(buttons.size(), 3);
        QTest::mouseMove(view.viewport(), buttons[0]);
        QCOMPARE(view.viewport()->cursor().shape(), Qt::PointingHandCursor);
        QSignalSpy links(&view, &ChatView::linkActivated);
        const QStringList expected{first, second, QString()};
        for (int block = 0; block < expected.size(); ++block) {
            QApplication::clipboard()->setText("sentinel");
            QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, buttons[block]);
            QCOMPARE(QApplication::clipboard()->text(), expected[block]);
        }
        QCOMPARE(links.size(), 0); QCOMPARE(index.data(MsgTextRole).toString(), source);
        QApplication::clipboard()->setText("unchanged");
        QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, buttons[0]);
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, buttons[1]);
        QCOMPARE(QApplication::clipboard()->text(), QString("unchanged"));
        QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, buttons[0]);
        const QPoint end = buttons[0] + QPoint(8, 0);
        QMouseEvent drag(QEvent::MouseMove, end, view.viewport()->mapToGlobal(end), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &drag);
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, end);
        QCOMPARE(QApplication::clipboard()->text(), QString("unchanged"));
        QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, buttons[0]);
        QApplication::sendEvent(view.viewport(), &drag);
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, buttons[0]);
        QCOMPARE(QApplication::clipboard()->text(), QString("unchanged"));
        QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, buttons[0]);
        model.setData(index, source + " edited", MsgTextRole);
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, buttons[0]);
        QCOMPARE(QApplication::clipboard()->text(), QString("unchanged"));
        if (outgoing && dark && width == 700) QVERIFY(view.grab().save(QDir::tempPath() + "/code-copy-buttons.png"));
    }

    void webLinksUsePaintedCoordinatesAndDragDoesNotOpen() {
        const QString first = QStringLiteral("https://пример.рф/путь?") + QString(160, 'a') + QStringLiteral("=1#fragment");
        const QString second = QStringLiteral("http://example.org/a(b)?x=2");
        const QString invite = QStringLiteral("utun://abc+/==");
        const QString text = QStringLiteral("✨ (%1),\nВторая: %2.\nПриглашение: %3").arg(first, second, invite);
        QStandardItemModel model;
        auto* message = new QStandardItem;
        message->setData(text, MsgTextRole); message->setData(QStringLiteral("text/plain"), MsgContentTypeRole);
        message->setData(true, MsgIsOutgoingRole); model.appendRow(message);
        ChatView view; MessageDelegate delegate;
        view.setModel(&model); view.setItemDelegate(&delegate); view.resize(280, 900);
        view.setLinkHitTest([&](const QModelIndex& index, const QPoint& point) {
            QStyleOptionViewItem option; option.initFrom(&view); option.widget = &view; option.rect = view.visualRect(index);
            return delegate.linkAt(option, index, point);
        });
        view.show(); QTest::qWait(30);
        const QModelIndex index = model.index(0, 0);
        QStyleOptionViewItem option; option.initFrom(&view); option.widget = &view; option.rect = view.visualRect(index);
        QMap<QString, QPoint> points;
        QSet<int> rows;
        for (int y = 8; y < option.rect.height(); y += 8) {
            for (int x = 8; x < view.viewport()->width(); x += 12) {
                const QString hit = delegate.linkAt(option, index, QPoint(x, y));
                if (hit.isEmpty()) continue;
                QVERIFY(hit == first || hit == second || hit == invite);
                if (!points.contains(hit)) points[hit] = QPoint(x, y);
                if (hit == first) rows.insert(y);
            }
        }
        QCOMPARE(points.size(), 3); QVERIFY(rows.size() > 4);
        QVERIFY(delegate.linkAt(option, index, QPoint(0, 0)).isEmpty());
        QSignalSpy clicked(&view, &ChatView::linkActivated);
        for (const auto& link : {first, second, invite}) {
            QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, points.value(link));
            QCOMPARE(clicked.last().first().toString(), link);
        }
        QCOMPARE(clicked.count(), 3);
        const QPoint start = points.value(first), end = start + QPoint(20, 0);
        QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, start);
        QMouseEvent drag(QEvent::MouseMove, end, view.viewport()->mapToGlobal(end), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &drag);
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, end);
        QTest::qWait(20);
        QCOMPARE(clicked.count(), 3);
        const auto selection = view.textSelection();
        QVERIFY(selection.startChar >= text.indexOf(first) && selection.endChar <= text.indexOf(first) + first.length());
        QVERIFY(selection.endChar > selection.startChar);
        QTest::keyClick(&view, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(), text.mid(selection.startChar, selection.endChar - selection.startChar));
    }

    void compactInvitationUsesFullLinkForClickAndCopy() {
        struct InviteData invite = {};
        invite.join_key = 123; invite.pubkey[0] = 1; invite.addrCount = 8;
        uint8_t relayX[32], relayPrivate[32], relaySeed[32];
        QCOMPARE(sc_identity_generate(relayX, relayPrivate, invite.inviter_pubkey, relaySeed), SC_OK);
        for (auto& address : invite.addrs) { address.family = 6; address.proto = INVITE_PROTO_TCP; address.port = 443; }
        char buffer[1024];
        QVERIFY(invite_link_encode(&invite, buffer, sizeof(buffer)) > 0);
        const QString link = QString::fromUtf8(buffer);
        const QString text = QStringLiteral("Приглашение в группу «Группа»\n") + link;
        for (int width : {240, 480}) {
            QStandardItemModel model;
            auto* message = new QStandardItem;
            message->setData(text, MsgTextRole); message->setData(link, MsgInviteLinkRole);
            message->setData(QStringLiteral("text/plain"), MsgContentTypeRole); message->setData(true, MsgIsOutgoingRole);
            model.appendRow(message);
            ChatView view; MessageDelegate delegate;
            view.setModel(&model); view.setItemDelegate(&delegate); view.resize(width, 300);
            view.setLinkHitTest([&](const QModelIndex& index, const QPoint& point) {
                QStyleOptionViewItem option; option.initFrom(&view); option.widget = &view; option.rect = view.visualRect(index);
                return delegate.linkAt(option, index, point);
            });
            view.show(); QTest::qWait(30);
            const QModelIndex index = model.index(0, 0);
            QStyleOptionViewItem option; option.initFrom(&view); option.widget = &view; option.rect = view.visualRect(index);
            QPoint firstPoint, lastPoint;
            QSet<int> rows;
            for (int y = 0; y < option.rect.height(); ++y) {
                for (int x = 0; x < view.viewport()->width(); x += 2) {
                    const QString hit = delegate.linkAt(option, index, QPoint(x, y));
                    if (hit.isEmpty()) continue;
                    QCOMPARE(hit, link);
                    rows.insert(y);
                    if (firstPoint.isNull()) firstPoint = QPoint(x, y);
                    lastPoint = QPoint(x, y);
                }
            }
            QVERIFY(!firstPoint.isNull());
            QVERIFY(rows.size() <= QFontMetrics(option.font).height() + 1);
            QVERIFY(option.rect.height() < 160);
            QSignalSpy clicked(&view, &ChatView::linkActivated);
            QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, lastPoint);
            QCOMPARE(clicked.count(), 1); QCOMPARE(clicked.last().first().toString(), link);
            QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, firstPoint);
            QMouseEvent drag(QEvent::MouseMove, lastPoint, view.viewport()->mapToGlobal(lastPoint),
                             Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view.viewport(), &drag);
            QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, lastPoint);
            QTest::qWait(20);
            QCOMPARE(clicked.count(), 1);
            QTest::keyClick(&view, Qt::Key_C, Qt::ControlModifier);
            QCOMPARE(QApplication::clipboard()->text(), link);
            QCOMPARE(index.data(MsgTextRole).toString(), text);
        }
    }

    void wrappedLinksPreserveTheirCompleteText() {
        struct InviteData invite = {};
        invite.join_key = 123; invite.pubkey[0] = 1; invite.addrCount = 8;
        uint8_t relayX[32], relayPrivate[32], relaySeed[32];
        QCOMPARE(sc_identity_generate(relayX, relayPrivate, invite.inviter_pubkey, relaySeed), SC_OK);
        for (auto& address : invite.addrs) { address.family = 6; address.proto = INVITE_PROTO_TCP; address.port = 443; }
        char buffer[1024];
        QVERIFY(invite_link_encode(&invite, buffer, sizeof(buffer)) > 0);
        const QString first = QString::fromUtf8(buffer);
        invite.join_key++;
        QVERIFY(invite_link_encode(&invite, buffer, sizeof(buffer)) > 0);
        const QString second = QString::fromUtf8(buffer);
        const QString text = QStringLiteral("Приглашение\n(%1),\nВторая группа: %2.").arg(first, second);
        QStandardItemModel model;
        auto* message = new QStandardItem;
        message->setData(text, MsgTextRole); message->setData(QStringLiteral("text/plain"), MsgContentTypeRole);
        message->setData(true, MsgIsOutgoingRole);
        model.appendRow(message);
        ChatView view; MessageDelegate delegate;
        view.setModel(&model); view.setItemDelegate(&delegate); view.resize(280, 900);
        view.setLinkHitTest([&](const QModelIndex& index, const QPoint& point) {
            QStyleOptionViewItem option; option.initFrom(&view); option.widget = &view; option.rect = view.visualRect(index);
            return delegate.linkAt(option, index, point);
        });
        view.show(); QTest::qWait(30);
        const QModelIndex index = model.index(0, 0);
        QStyleOptionViewItem option; option.initFrom(&view); option.widget = &view; option.rect = view.visualRect(index);
        QPoint firstPoint, secondPoint;
        QSet<int> firstRows;
        for (int y = 10; y < option.rect.height(); y += 8) {
            for (int x = 10; x < view.viewport()->width(); x += 16) {
                const QString link = delegate.linkAt(option, index, QPoint(x, y));
                if (link == first) { if (firstPoint.isNull()) firstPoint = QPoint(x, y); firstRows.insert(y); }
                else if (link == second) secondPoint = QPoint(x, y);
                else QVERIFY(link.isEmpty());
            }
        }
        QVERIFY(firstRows.size() > 4); QVERIFY(!firstPoint.isNull()); QVERIFY(!secondPoint.isNull());
        QVERIFY(delegate.linkAt(option, index, QPoint(0, 0)).isEmpty());
        QSignalSpy clicked(&view, &ChatView::linkActivated);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, firstPoint);
        QCOMPARE(clicked.count(), 1); QCOMPARE(clicked.last().first().toString(), first);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, secondPoint);
        QCOMPARE(clicked.count(), 2); QCOMPARE(clicked.last().first().toString(), second);
        /* Выделение использует те же перенесённые строки, что и отрисовка ссылки. */
        QPoint dragEnd = firstPoint + QPoint(20, 0);
        QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, firstPoint);
        QMouseEvent drag(QEvent::MouseMove, dragEnd, view.viewport()->mapToGlobal(dragEnd),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &drag);
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, dragEnd);
        QTest::qWait(20);
        QCOMPARE(clicked.count(), 2);
        const auto selection = view.textSelection();
        QVERIFY(selection.startChar >= text.indexOf(first));
        QVERIFY(selection.endChar <= text.indexOf(first) + first.length());
        QVERIFY(selection.endChar > selection.startChar);
        QVERIFY(selection.endChar - selection.startChar < 20);
    }

    void memberCallButtonsTargetOnlinePeersOnly() {
        QStandardItemModel model;
        auto* other = new QStandardItem("other"); other->setData(false, Qt::UserRole + 2); model.appendRow(other);
        auto* self = new QStandardItem("self"); self->setData(true, Qt::UserRole + 2); model.appendRow(self);
        QListView view; AccountDelegate delegate;
        view.setModel(&model); view.setItemDelegate(&delegate); view.resize(240, 140); view.show();
        QTest::qWait(30);
        QSignalSpy radios(&delegate, &AccountDelegate::radioCallRequested);
        QSignalSpy calls(&delegate, &AccountDelegate::callRequested);
        const QRect row = view.visualRect(model.index(0, 0));
        const QPoint radio(row.right() - 82, row.center().y()), phone(row.right() - 50, row.center().y());
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, radio);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, phone);
        QCOMPARE(radios.count(), 0); QCOMPARE(calls.count(), 0);
        other->setData(true, MemberListModel::OnlineRole);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, radio);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, phone);
        QCOMPARE(radios.count(), 1); QCOMPARE(qvariant_cast<QModelIndex>(radios.first().first()).row(), 0);
        QCOMPARE(calls.count(), 1); QCOMPARE(qvariant_cast<QModelIndex>(calls.first().first()).row(), 0);
        const QRect own = view.visualRect(model.index(1, 0));
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(own.right() - 17, own.center().y()));
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(own.right() - 50, own.center().y()));
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(60, row.center().y()));
        QCOMPARE(radios.count(), 1); QCOMPARE(calls.count(), 1);
        QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, radio);
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, phone);
        QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, radio);
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, radio + QPoint(6, 0));
        QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, phone);
        other->setData(false, MemberListModel::OnlineRole);
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, phone);
        QCOMPARE(radios.count(), 1); QCOMPARE(calls.count(), 1);
        other->setData(true, MemberListModel::OnlineRole);
        const QString preview = qEnvironmentVariable("UTUN_MEMBER_ACTION_PREVIEW");
        if (!preview.isEmpty()) QVERIFY(view.grab().save(preview));
    }

    void phoneButtonReflectsBusyFreeUnknownAndOffline() {
        QStandardItemModel model;
        auto* peer = new QStandardItem("peer"); model.appendRow(peer);
        peer->setData(false, Qt::UserRole + 2); peer->setData(true, MemberListModel::OnlineRole);
        AccountDelegate delegate;
        QStyleOptionViewItem option; option.rect = QRect(0, 0, 280, 52);
        auto pixels = [&](int flags, bool online, const QColor& color) {
            peer->setData(flags, Qt::UserRole + 3); peer->setData(online, MemberListModel::OnlineRole);
            QImage image(option.rect.size(), QImage::Format_RGB32); image.fill(Qt::white);
            QPainter painter(&image); delegate.paint(&painter, option, model.index(0, 0)); painter.end();
            int count = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = image.width() - 64; x < image.width() - 36; ++x) count += image.pixelColor(x, y) == color;
            return count;
        };
        const int known = CHAT_MEMBER_FLAG_PHONE_KNOWN, busy = CHAT_MEMBER_FLAG_PHONE_BUSY;
        QVERIFY(pixels(known | busy, true, QColor("#E53935")) > 0);
        QVERIFY(pixels(known, true, QColor("#4CAF50")) > 0);
        QCOMPARE(pixels(0, true, QColor("#4CAF50")), 0);
        QCOMPARE(pixels(busy, true, QColor("#E53935")), 0);
        QCOMPARE(pixels(known | busy, false, QColor("#E53935")), 0);
        QCOMPARE(pixels(known | busy, true, QColor("#4CAF50")), 0);
    }

    void draggingAwayAndBackDoesNotCallMember_data() {
        QTest::addColumn<int>("action");
        QTest::newRow("avatar") << 0;
        QTest::newRow("radio-call") << 1;
        QTest::newRow("call") << 2;
        QTest::newRow("radio-mute") << 3;
    }

    void draggingAwayAndBackDoesNotCallMember() {
        QFETCH(int, action);
        QScopedValueRollback<debug_level_t> diagnostic(g_debug_config.category_levels[DEBUG_CATEGORY_GENERAL], DEBUG_LEVEL_DEBUG);
        QStandardItemModel model;
        auto* peer = new QStandardItem("peer"); model.appendRow(peer);
        peer->setData(true, MemberListModel::OnlineRole);
        QListView view; AccountDelegate delegate;
        view.setModel(&model); view.setItemDelegate(&delegate); view.resize(280, 140); view.show();
        QTest::qWait(30);
        const auto signal = action == 0 ? &AccountDelegate::avatarRequested : action == 1 ? &AccountDelegate::radioCallRequested :
            action == 2 ? &AccountDelegate::callRequested : &AccountDelegate::radioMuteRequested;
        QSignalSpy activated(&delegate, signal);
        const QRect row = view.visualRect(model.index(0, 0));
        const QPoint radio(action == 0 ? row.left() + GroupedMemberModel::DisclosureWidth + 21 :
            row.right() - (action == 1 ? 82 : action == 2 ? 50 : 18), row.center().y());
        QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, radio);
        for (QPoint point : {radio + QPoint(-15, 0), radio}) {
            QMouseEvent move(QEvent::MouseMove, point, view.viewport()->mapToGlobal(point), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view.viewport(), &move);
        }
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, radio);
        QCOMPARE(activated.count(), 0);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, radio);
        QCOMPARE(activated.count(), 1);
    }

    void radioMuteButtonTargetsMemberAndChangesColor() {
        QStandardItemModel model;
        auto* other = new QStandardItem("other"); other->setData(false, Qt::UserRole + 2); model.appendRow(other);
        auto* self = new QStandardItem("self"); self->setData(true, Qt::UserRole + 2); model.appendRow(self);
        QListView view; AccountDelegate delegate;
        view.setModel(&model); view.setItemDelegate(&delegate); view.resize(280, 140); view.show(); QTest::qWait(30);
        QSignalSpy toggles(&delegate, &AccountDelegate::radioMuteRequested);
        QSignalSpy radios(&delegate, &AccountDelegate::radioCallRequested);
        QSignalSpy calls(&delegate, &AccountDelegate::callRequested);
        connect(&delegate, &AccountDelegate::radioMuteRequested, &model, [&](const QModelIndex& index) {
            model.setData(index, index.data(Qt::UserRole + 3).toInt() ^ CHAT_MEMBER_FLAG_RADIO_MUTED, Qt::UserRole + 3);
        });
        const QModelIndex index = model.index(0, 0);
        const QRect row = view.visualRect(index);
        const QPoint button(row.right() - 18, row.center().y());
        auto colorCount = [&](QRgb color) {
            QImage image(view.viewport()->size(), QImage::Format_RGB32); image.fill(Qt::white);
            QStyleOptionViewItem option; option.initFrom(&view); option.rect = row;
            QPainter painter(&image); delegate.paint(&painter, option, index); painter.end();
            int count = 0;
            for (int y = row.top(); y <= row.bottom(); ++y)
                for (int x = row.right() - 31; x < row.right() - 3; ++x) count += image.pixel(x, y) == color;
            return count;
        };
        QVERIFY(colorCount(QColor("#9E9E9E").rgb()) > 0); // Молчащий участник серый; зелёный означает передачу.
        model.setData(index, true, MemberListModel::RadioSpeakingRole);
        QVERIFY(colorCount(QColor("#4CAF50").rgb()) > 0);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, button);
        QCOMPARE(toggles.count(), 1); QCOMPARE(qvariant_cast<QModelIndex>(toggles.first().first()).row(), 0);
        QVERIFY(colorCount(QColor("#EF5350").rgb()) > 0); QCOMPARE(colorCount(QColor("#4CAF50").rgb()), 0);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, button);
        QCOMPARE(toggles.count(), 2); QVERIFY(colorCount(QColor("#4CAF50").rgb()) > 0);
        const QRect own = view.visualRect(model.index(1, 0));
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(own.right() - 17, own.center().y()));
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(65, row.center().y()));
        QCOMPARE(toggles.count(), 2); QCOMPARE(radios.count(), 0); QCOMPARE(calls.count(), 0);
        QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, button);
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(65, row.center().y()));
        QCOMPARE(toggles.count(), 2);
    }
};
QTEST_MAIN(InviteLinksTest)
#include "test_invite_links.moc"
