#include "../src/animtimer.h"
#include "../src/chatview.h"
#include "../src/emoji.h"
#include "../src/messagedelegate.h"
#include "../src/emojipanel.h"
#include "../src/lottieicon.h"
#include "../../../lib/debug_config.h"
#include <QtTest>
#include <QEnterEvent>
#include <QPushButton>
#include <QTabWidget>
#include <QFile>
#include <QStandardItemModel>
#include <zlib.h>
#include <memory>

static QStringList playbackLog;
static QStringList errorLog;

static void captureLog(int level, const char *, const char *message) {
    playbackLog.append(QString::fromUtf8(message));
    if (level == DEBUG_LEVEL_ERROR) errorLog.append(QString::fromUtf8(message));
}

static QByteArray gzipData(QByteArray json) {
    QByteArray gzip(int(compressBound(json.size()) + 32), '\0');
    z_stream stream = {};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) return {};
    stream.next_in = reinterpret_cast<Bytef *>(json.data());
    stream.avail_in = json.size();
    stream.next_out = reinterpret_cast<Bytef *>(gzip.data());
    stream.avail_out = gzip.size();
    int result = deflate(&stream, Z_FINISH);
    gzip.resize(stream.total_out);
    deflateEnd(&stream);
    return result == Z_STREAM_END ? gzip : QByteArray();
}

// Два коротких цикла разной длины выявляют остановку посреди кадра и ожидание общего кратного.
static QByteArray animationData(int frames) {
    return gzipData(QString(R"({"v":"5.7.4","fr":30,"ip":0,"op":%1,"w":32,"h":32,"layers":[]})").arg(frames).toUtf8());
}

class EmojiPlaybackTest : public QObject {
    Q_OBJECT
    std::unique_ptr<LottieIcon> shortIcon, longIcon;

private slots:
    void init() {
        debug_config_init();
        debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_DEBUG);
        debug_set_log_hook(captureLog);
        playbackLog.clear();
        errorLog.clear();
        shortIcon = std::make_unique<LottieIcon>(animationData(7), "short");
        longIcon = std::make_unique<LottieIcon>(animationData(11), "long");
        QVERIFY(shortIcon->isValid());
        QVERIFY(longIcon->isValid());
        AnimTimer::instance()->registerIcon("short", shortIcon.get());
        AnimTimer::instance()->registerIcon("long", longIcon.get());
    }

    void cleanup() {
        AnimTimer::shutdown();
        debug_set_log_hook(nullptr);
        shortIcon.reset();
        longIcon.reset();
    }

    void catalogSelection_data() {
        QTest::addColumn<QString>("shortcode");
        QTest::addColumn<QString>("unicode");
        QTest::newRow("ok-hand") << QString(":ok_hand:") << QString::fromUtf8("👌");
        QTest::newRow("pinched-fingers") << QString(":pinched_fingers:") << QString::fromUtf8("🤌");
        QTest::newRow("hugging") << QString(":hugging:") << QString::fromUtf8("🤗");
        QTest::newRow("shushing") << QString(":shushing:") << QString::fromUtf8("🤫");
        QTest::newRow("star-struck") << QString(":star_struck:") << QString::fromUtf8("🤩");
        QTest::newRow("woozy-face") << QString(":woozy_face:") << QString::fromUtf8("🥴");
        QTest::newRow("robot-control") << QString(":robot:") << QString::fromUtf8("🤖");
        QTest::newRow("thumbs-up-control") << QString(":+1:") << QString::fromUtf8("👍");
    }

    void catalogSelection() {
        QFETCH(QString, shortcode); QFETCH(QString, unicode);
        EmojiPanel panel;
        QSignalSpy selected(&panel, &EmojiPanel::emojiSelected);
        int buttons = 0;
        for (auto* button : panel.findChildren<QPushButton*>()) {
            if (button->toolTip() != shortcode) continue;
            ++buttons; const int before = selected.count(); button->click();
            QCOMPARE(selected.count(), before + 1);
            QCOMPARE(selected.last().first().toString(), unicode);
        }
        QVERIFY2(buttons > 0, qPrintable(shortcode));
        int entries = 0;
        for (const auto& category : builtinEmojiSet()) for (const auto& entry : category.items) {
            if (entry.shortcode != shortcode) continue;
            ++entries; QCOMPARE(entry.unicode, unicode);
        }
        QCOMPARE(buttons, entries);
        QCOMPARE(detectEmojiOnly(unicode), 1);
        QCOMPARE(detectEmojiOnly(" \t" + unicode + "\n"), 1);
        QCOMPARE(detectEmojiOnly(unicode + unicode), 2);
        QCOMPARE(detectEmojiOnly(unicode + " " + QString::fromUtf8("🤖") + "\n" + unicode), 3);
        QCOMPARE(detectEmojiOnly(unicode.repeated(4)), 0);
        QCOMPARE(detectEmojiOnly("text " + unicode), 0);
    }

    void rejectsInvalidAnimations_data() {
        QTest::addColumn<QByteArray>("data");
        const auto valid = animationData(7);
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("not-gzip") << QByteArray("not an animation");
        QTest::newRow("short-header") << valid.left(4);
        QTest::newRow("missing-trailer") << valid.left(valid.size() - 4);
        auto corrupt = valid; corrupt[corrupt.size() - 1] = char(corrupt.back() ^ 1);
        QTest::newRow("invalid-size-checksum") << corrupt;
        QTest::newRow("trailing-data") << valid + QByteArray("trailing");
        QTest::newRow("invalid-json") << gzipData("{broken");
        QByteArray json(R"({"v":"5.7.4","fr":30,"ip":0,"op":7,"w":32,"h":32,"layers":[]})");
        json.append('\0'); json.append("ignored suffix");
        QTest::newRow("embedded-nul") << gzipData(json);
        QTest::newRow("zero-frames") << animationData(0);
        QTest::newRow("json-budget") << gzipData(QByteArray(LottieIcon::MaxJsonBytes + 1, ' '));
    }

    void rejectsInvalidAnimations() {
        QFETCH(QByteArray, data);
        LottieIcon invalid(data, QTest::currentDataTag());
        QVERIFY(!invalid.isValid());
        QVERIFY(!errorLog.filter("LottieIcon:").isEmpty());
        invalid.advanceFrame(); QCOMPARE(invalid.currentFrame(), 0);
        QVERIFY(invalid.renderFrame(0, QSize(32, 32)).isNull());
        AnimTimer::instance()->registerIcon("invalid", &invalid);
        QVERIFY(AnimTimer::instance()->icon("invalid") == nullptr);
        AnimTimer::instance()->registerIcon("short", nullptr);
        QCOMPARE(AnimTimer::instance()->icon("short"), shortIcon.get());
        QVERIFY(!playbackLog.filter("LottieIcon:").isEmpty());
    }

    void bundledAnimationsRemainValid() {
        const QDir directory(QFINDTESTDATA("../resources/animations"));
        const auto files = directory.entryList({"*.tgs"}, QDir::Files);
        QVERIFY(!files.isEmpty());
        for (const QString& name : files) {
            QFile file(directory.filePath(name)); QVERIFY(file.open(QIODevice::ReadOnly));
            LottieIcon icon(file.readAll(), "bundled-" + name);
            QVERIFY2(icon.isValid(), qPrintable(name));
            QVERIFY(!icon.renderFrame(0, QSize(32, 32)).isNull());
        }
    }

    void rendersOwnedImagesAndWrapsFrameNumbers() {
        QFile file(QFINDTESTDATA("../resources/animations/robot.tgs")); QVERIFY(file.open(QIODevice::ReadOnly));
        LottieIcon icon(file.readAll(), "render-robot"); QVERIFY(icon.isValid());
        QImage first = icon.renderFrame(0, QSize(48, 48));
        QVERIFY(!first.isNull()); QCOMPARE(first.format(), QImage::Format_ARGB32_Premultiplied);
        QCOMPARE(icon.renderFrame(-1, first.size()), icon.renderFrame(icon.frameCount() - 1, first.size()));
        QCOMPARE(icon.renderFrame(icon.frameCount(), first.size()), first);
        const QImage saved = first;
        first.fill(Qt::red);
        QCOMPARE(icon.renderFrame(0, saved.size()), saved);
        QVERIFY(icon.renderFrame(0, QSize(0, 32)).isNull());
    }

    void messagesFinishEachCycle() {
        auto *timer = AnimTimer::instance();
        QSignalSpy ticks(timer, &AnimTimer::ticked);
        timer->play();
        QTest::qWait(1850);
        QVERIFY(playbackLog.filter("AnimTimer: playback finished").isEmpty());
        int count = ticks.count();
        QTest::qWait(100);
        QVERIFY(ticks.count() > count);
        QTRY_COMPARE_WITH_TIMEOUT(playbackLog.filter("AnimTimer: playback finished").size(), 1, 1000);
        QCOMPARE(shortIcon->currentFrame(), 0);
        QCOMPARE(longIcon->currentFrame(), 0);
        count = ticks.count();
        QTest::qWait(150);
        QCOMPARE(ticks.count(), count);
    }

    void anotherHoverRestartsDeadline() {
        auto *timer = AnimTimer::instance();
        QSignalSpy ticks(timer, &AnimTimer::ticked);
        timer->play();
        QTest::qWait(1400);
        timer->play();
        QTest::qWait(1000);
        QVERIFY(playbackLog.filter("AnimTimer: playback finished").isEmpty());
        int count = ticks.count();
        QTest::qWait(100);
        QVERIFY(ticks.count() > count);
        QTRY_COMPARE_WITH_TIMEOUT(playbackLog.filter("AnimTimer: playback finished").size(), 1, 1500);
        QCOMPARE(shortIcon->currentFrame(), 0);
        QCOMPARE(longIcon->currentFrame(), 0);
    }

    void pickerFinishesWithOrWithoutHover() {
        AnimTimer::instance()->registerIcon(":/animations/robot.tgs", shortIcon.get());
        EmojiPanel panel;
        panel.resize(320, 240);
        panel.findChild<QTabWidget *>()->setCurrentIndex(3);
        panel.show();
        QPushButton *button = nullptr;
        for (auto *candidate : panel.findChildren<QPushButton *>()) {
            if (candidate->toolTip() == ":robot:" && candidate->hasMouseTracking()) button = candidate;
        }
        QVERIFY(button);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(button, &leave);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QEnterEvent enter(QPointF(1, 1), QPointF(1, 1), QPointF(1, 1));
#else
        QEvent enter(QEvent::Enter);
#endif
        QApplication::sendEvent(button, &enter);
        QTest::qWait(1850);
        QVERIFY(playbackLog.filter("EmojiButton: playback finished").isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(playbackLog.filter("EmojiButton: playback finished").size(), 1, 1000);
        QTest::qWait(150);
        QCOMPARE(playbackLog.filter("EmojiButton: hover trigger").size(), 1);

        QApplication::sendEvent(button, &leave);
        QApplication::sendEvent(button, &enter);
        QTest::qWait(100);
        QApplication::sendEvent(button, &leave);
        QTest::qWait(1750);
        QCOMPARE(playbackLog.filter("EmojiButton: playback finished").size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(playbackLog.filter("EmojiButton: playback finished").size(), 2, 1000);
    }

    void visibleMessagesTriggerOnce() {
        builtinEmojiSet();
        QStandardItemModel model;
        for (int row = 0; row < 6; ++row) {
            auto *item = new QStandardItem("message");
            item->setSizeHint(QSize(200, 60));
            item->setData(row + 1, MsgIdRole);
            item->setData("test-channel", MsgChannelIdRole);
            item->setData(row == 0 || row == 5 ? QString::fromUtf8("🤖") : QString("text"), MsgTextRole);
            model.appendRow(item);
        }
        ChatView view;
        view.setModel(&model);
        view.resize(240, 140);
        QSignalSpy triggers(&view, &ChatView::animationTriggered);
        connect(&view, &ChatView::animationTriggered, AnimTimer::instance(), &AnimTimer::play);
        view.show();
        QTRY_COMPARE(triggers.count(), 1);
        view.viewport()->update();
        QTest::qWait(100);
        QCOMPARE(triggers.count(), 1);

        view.scrollToBottom();
        QTRY_COMPARE(triggers.count(), 2);
        view.scrollToTop();
        QTRY_COMPARE(triggers.count(), 3);
        view.scrollToBottom();
        QTRY_COMPARE(triggers.count(), 4);

        auto *message = new QStandardItem("new message");
        message->setSizeHint(QSize(200, 60));
        message->setData(7, MsgIdRole);
        message->setData("test-channel", MsgChannelIdRole);
        message->setData(QString::fromUtf8("🤖"), MsgTextRole);
        model.appendRow(message);
        view.scrollToBottom();
        QTRY_COMPARE(triggers.count(), 5);
        view.viewport()->update();
        QTest::qWait(100);
        QCOMPARE(triggers.count(), 5);
    }

    void hoverTriggersOnEntry() {
        builtinEmojiSet();
        QStandardItemModel model;
        auto *message = new QStandardItem("message");
        message->setSizeHint(QSize(200, 60));
        message->setData(1, MsgIdRole);
        message->setData(QString::fromUtf8("🤖"), MsgTextRole);
        model.appendRow(message);
        ChatView view;
        view.setModel(&model);
        view.resize(240, 140);
        view.show();
        QTest::qWait(50);
        QSignalSpy triggers(&view, &ChatView::animationTriggered);
        const QPointF point = view.visualRect(model.index(0, 0)).center();
        QMouseEvent move(QEvent::MouseMove, point, point, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &move);
        QCOMPARE(triggers.count(), 1);
        QApplication::sendEvent(view.viewport(), &move);
        QCOMPARE(triggers.count(), 1);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(view.viewport(), &leave);
        QCOMPARE(triggers.count(), 1);
        QApplication::sendEvent(view.viewport(), &move);
        QCOMPARE(triggers.count(), 2);
    }

    void inviteClickDoesNotActivateOnDrag() {
        QStandardItemModel model;
        auto* message = new QStandardItem;
        message->setSizeHint(QSize(200, 60));
        message->setData(QStringLiteral("utun://abc+/=="), MsgTextRole);
        model.appendRow(message);
        ChatView view;
        view.setModel(&model); view.resize(240, 140);
        view.setLinkHitTest([](const QModelIndex& index, const QPoint&) { return index.data(MsgTextRole).toString(); });
        view.show();
        QTest::qWait(30);
        const QPoint point = view.visualRect(model.index(0, 0)).center();
        QSignalSpy links(&view, &ChatView::linkActivated);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(links.count(), 1);
        QCOMPARE(links.first().first().toString(), QStringLiteral("utun://abc+/=="));
        QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, point);
        const QPointF dragged = point + QPoint(15, 0);
        QMouseEvent move(QEvent::MouseMove, dragged, dragged, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &move);
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, dragged.toPoint());
        QCOMPARE(links.count(), 1);
        QVERIFY(view.hasSelection());
    }
};

QTEST_MAIN(EmojiPlaybackTest)
#include "test_emoji_playback.moc"
