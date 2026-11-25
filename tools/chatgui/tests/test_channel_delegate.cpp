/* Рисование настоящего делегата: границы строки, pixel-шрифты и цвета состояния. */
#include "../src/channeldelegate.h"
#include <QtTest>
#include <QPainter>
#include <QScopeGuard>
#include <QStandardItemModel>

class ChannelDelegateTest : public QObject {
    Q_OBJECT
private slots:
    void bounds_data() {
        QTest::addColumn<int>("width");
        QTest::addColumn<bool>("section");
        QTest::addColumn<bool>("pixelFont");
        for (int width : {16, 80, 240}) {
            for (bool section : {false, true}) {
                for (bool pixelFont : {false, true})
                    QTest::newRow(qPrintable(QString("%1-section%2-pixel%3").arg(width).arg(section).arg(pixelFont)))
                        << width << section << pixelFont;
            }
        }
    }

    void bounds() {
        QFETCH(int, width); QFETCH(bool, section); QFETCH(bool, pixelFont);
        QStandardItemModel model(1, 1);
        const QModelIndex index = model.index(0, 0);
        model.setData(index, QString(500, 'N'), ChannelNameRole);
        model.setData(index, QString(500, 'W') + ": body\nsecond line", ChannelLastMessageRole);
        model.setData(index, 100, ChannelUnreadCountRole);
        model.setData(index, 10, ChannelPeersOnlineRole);
        model.setData(index, section, ChannelIsSectionRole);
        model.setData(index, true, ChannelRadioActiveRole);
        QStyleOptionViewItem option;
        option.rect = QRect(20, 20, width, 56);
        option.font = QApplication::font();
        if (pixelFont) option.font.setPixelSize(14);
        option.state = QStyle::State_Enabled | QStyle::State_Selected;
        QImage image(600, 120, QImage::Format_ARGB32_Premultiplied);
        const QColor sentinel(17, 41, 73);
        image.fill(sentinel);
        QPainter painter(&image);
        painter.setPen(Qt::magenta);
        painter.setFont(option.font);
        painter.setClipRect(image.rect().adjusted(1, 1, -1, -1));
        const QRegion clip = painter.clipRegion();
        const QFont font = painter.font();
        const QPen pen = painter.pen();
        static QStringList fontWarnings;
        fontWarnings.clear();
        const auto oldHandler = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &, const QString &text) {
            if (type == QtWarningMsg && text.startsWith("QFont::")) fontWarnings.append(text);
        });
        auto restoreHandler = qScopeGuard([&] { qInstallMessageHandler(oldHandler); });
        ChannelDelegate delegate;
        delegate.paint(&painter, option, index);
        QCOMPARE(painter.clipRegion(), clip);
        QCOMPARE(painter.font(), font);
        QCOMPARE(painter.pen(), pen);
        painter.end();
        QVERIFY2(fontWarnings.isEmpty(), qPrintable(fontWarnings.join('\n')));
        for (int y = 0; y < image.height(); y++) {
            for (int x = 0; x < image.width(); x++) {
                if (!option.rect.contains(x, y))
                    QVERIFY2(image.pixelColor(x, y) == sentinel, qPrintable(QString("paint escaped row at %1,%2").arg(x).arg(y)));
            }
        }
        QCOMPARE(delegate.sizeHint(option, index), QSize(200, 56));
    }

    void statusColors() {
        QStandardItemModel model(1, 1);
        const QModelIndex index = model.index(0, 0);
        QStyleOptionViewItem option;
        option.rect = QRect(0, 0, 240, 56);
        option.font = QApplication::font();
        ChannelDelegate delegate;
        const auto render = [&] {
            QImage image(option.rect.size(), QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            delegate.paint(&painter, option, index);
            return image;
        };
        model.setData(index, 1, ChannelPeersOnlineRole);
        QCOMPARE(render().pixelColor(1, 28), QColor("#4CAF50"));
        model.setData(index, 0, ChannelPeersOnlineRole);
        model.setData(index, true, ChannelAutoConnectActiveRole);
        QCOMPARE(render().pixelColor(1, 28), QColor("#FFC107"));
        model.setData(index, false, ChannelAutoConnectActiveRole);
        QCOMPARE(render().pixelColor(1, 28), option.palette.base().color());
        model.setData(index, "dm:conversation", ChannelChannelIdRole);
        model.setData(index, false, ChannelDmOnlineRole);
        QCOMPARE(render().pixelColor(16, 14), QColor("#b42332"));
        model.setData(index, true, ChannelDmOnlineRole);
        QCOMPARE(render().pixelColor(16, 14), QColor("#226d38"));
    }
};

QTEST_MAIN(ChannelDelegateTest)
#include "test_channel_delegate.moc"
