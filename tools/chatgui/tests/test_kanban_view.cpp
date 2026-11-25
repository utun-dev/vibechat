#include "../src/kanban_card_list.h"
#include <QtTest>

class KanbanViewTest : public QObject {
    Q_OBJECT
private slots:
    void deadlineUsesDaysAndRemainingHours() {
        const qint64 now = 1700000000000LL;
        QCOMPARE(kanbanDueText(0, now), QString());
        for (const auto& sample : {qMakePair(23, "23 h"), qMakePair(24, "1 d"), qMakePair(25, "1 d 1 h"), qMakePair(71, "2 d 23 h")}) {
            const qint64 duration = sample.first * 3600000LL;
            QCOMPARE(kanbanDueText((now + duration) * 1000, now), QString::fromLatin1(sample.second));
            QCOMPARE(kanbanDueText((now - duration) * 1000, now), QStringLiteral("−") + QString::fromLatin1(sample.second));
        }
        QCOMPARE(kanbanDueText((now + 3 * 86400000LL) * 1000, now), QStringLiteral("3 d"));
        QCOMPARE(kanbanDueText((now + 3 * 86400000LL - 1) * 1000, now), QStringLiteral("3 d"));
        QCOMPARE(kanbanDueText((now + 1) * 1000, now), QStringLiteral("1 h"));
        QCOMPARE(kanbanDueText((now - 1) * 1000, now), QStringLiteral("−1 h"));
    }
    void dashGapClosesThePerimeterWithoutASeam() {
        for (qreal width : {75.5, 220.0, 287.25, 1200.0}) for (qreal height : {56.0, 91.5, 800.25}) {
            QPainterPath path = kanbanRounded(QRectF(0, 0, width, height)); KanbanDash dash = kanbanDash(path);
            QVERIFY(dash.repeats > 0); QVERIFY(dash.gap > 4.0 && dash.gap < 8.0);
            QVERIFY(qAbs(dash.repeats * (8.0 + dash.gap) - path.length()) < 0.000001);
            path.translate(20, 20);
            QImage image(qCeil(width) + 40, qCeil(height) + 40, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
            { QPainter painter(&image); painter.setRenderHint(QPainter::Antialiasing); kanbanDrawDash(painter, path, Qt::black); }
            const qreal length = path.length();
            auto alpha = [&](qreal distance) {
                QPoint point = path.pointAtPercent(path.percentAtLength(distance)).toPoint(); return image.pixelColor(point).alpha();
            };
            auto ink = [&](qreal distance) {
                QPoint point = path.pointAtPercent(path.percentAtLength(distance)).toPoint(); int result = 0;
                for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++)
                    result = qMax(result, image.pixelColor(point + QPoint(x, y)).alpha());
                return result;
            };
            // На обоих концах замкнутого пути середина зазора, а за ней — полноценная линия.
            QVERIFY(alpha(0) < 32); QVERIFY(alpha(1) < 32); QVERIFY(alpha(length - 1) < 32);
            QByteArray sample = QString("size=%1x%2 gap=%3 seam=%4 next=%5 previous=%6")
                .arg(width).arg(height).arg(dash.gap).arg(alpha(0)).arg(ink(dash.gap / 2 + 4)).arg(ink(length - dash.gap / 2 - 4)).toUtf8();
            QVERIFY2(ink(dash.gap / 2 + 4) > 180, sample.constData());
            QVERIFY2(ink(length - dash.gap / 2 - 4) > 180, sample.constData());
        }
    }
    void selectionHasAnOuterOutlineWithoutHighlightingText() {
        CardList list(nullptr); list.resize(280, 200);
        auto* item = new QListWidgetItem("Text stays unchanged when selected", &list); item->setData(Qt::UserRole, "id");
        item->setData(Qt::UserRole + 1, QJsonObject{{"color", "#e45b5b"}});
        list.show(); QVERIFY(QTest::qWaitForWindowExposed(&list)); QApplication::processEvents();
        list.clearFocus(); QImage before = list.viewport()->grab().toImage();
        QRect rect = list.visualItemRect(item);
        QTest::mouseClick(list.viewport(), Qt::LeftButton, Qt::NoModifier, rect.center()); QVERIFY(item->isSelected());
        QImage selected = list.viewport()->grab().toImage();
        QCOMPARE(before.copy(rect.adjusted(8, 8, -8, -8)), selected.copy(rect.adjusted(8, 8, -8, -8)));
        int y = rect.center().y();
        QCOMPARE(selected.pixelColor(rect.left() + 6, y), QColor("#e45b5b"));
        QCOMPARE(selected.pixelColor(rect.left() + 5, y), QColor("#e45b5b"));
        QCOMPARE(before.copy(QRect(rect.left() + 2, rect.top() + 16, 3, rect.height() - 32)),
            selected.copy(QRect(rect.left() + 2, rect.top() + 16, 3, rect.height() - 32)));
        QVERIFY(before != selected); QCOMPARE(list.editTriggers(), QAbstractItemView::NoEditTriggers);
    }
    void previewAnimatesAndCancelPreservesOrder() {
        CardList list(nullptr); list.group = "42"; list.column = "column"; list.resize(280, 400);
        for (const QString& id : {QStringLiteral("one"), QStringLiteral("two"), QStringLiteral("three")}) {
            auto* item = new QListWidgetItem(id, &list); item->setData(Qt::UserRole, id); item->setSizeHint(QSize(230, 60));
        }
        int submissions = 0; QJsonObject submitted;
        list.submit = [&](QJsonObject command) { submissions++; submitted = command; };
        list.show(); QVERIFY(QTest::qWaitForWindowExposed(&list)); QApplication::processEvents();
        QImage original = list.viewport()->grab().toImage();
        list.previewDrag("two", 60);
        QMimeData mime; mime.setData(cardMime, QJsonDocument(QJsonObject{{"group", "42"}, {"entity", "two"}}).toJson());
        QDragEnterEvent enter(QPoint(25, 5), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list.viewport(), &enter); QVERIFY(enter.isAccepted());
        QDragMoveEvent move(QPoint(25, 5), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list.viewport(), &move); QVERIFY(move.isAccepted());
        auto* animation = list.findChild<QVariantAnimation*>("kanbanPlacement"); QVERIFY(animation);
        animation->pause(); animation->setCurrentTime(0); QImage start = list.viewport()->grab().toImage();
        animation->setCurrentTime(90); QImage middle = list.viewport()->grab().toImage();
        animation->setCurrentTime(180); QImage end = list.viewport()->grab().toImage();
        QVERIFY(start != middle); QVERIFY(middle != end); QCOMPARE(submissions, 0);
        QCOMPARE(list.item(0)->text(), QStringLiteral("one")); QCOMPARE(list.item(1)->text(), QStringLiteral("two"));
        list.previewDrag({}, 0); animation->pause(); animation->setCurrentTime(180);
        QCOMPARE(list.viewport()->grab().toImage(), original); QCOMPARE(submissions, 0);
        list.previewDrag("two", 60);
        QDragEnterEvent enterAgain(QPoint(25, 5), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list.viewport(), &enterAgain);
        QDragMoveEvent moveAgain(QPoint(25, 5), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list.viewport(), &moveAgain);
        QDropEvent drop(QPointF(25, 5), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list.viewport(), &drop);
        QCOMPARE(submissions, 1); QCOMPARE(submitted["action"].toString(), QStringLiteral("card_move"));
        QCOMPARE(submitted["entity"].toString(), QStringLiteral("two")); QCOMPARE(submitted["before"].toString(), QStringLiteral("one"));
        // Preview не переставляет локальную модель и не заменяет подтверждение ядра.
        QCOMPARE(list.item(0)->text(), QStringLiteral("one")); QCOMPARE(list.item(1)->text(), QStringLiteral("two"));
    }
    void previewMovesOtherColumnAndSupportsEmptyDrop() {
        CardList list(nullptr); list.group = "42"; list.column = "target"; list.resize(280, 350);
        auto* item = new QListWidgetItem("target card", &list); item->setData(Qt::UserRole, "anchor"); item->setSizeHint(QSize(230, 60));
        int submissions = 0; QJsonObject submitted;
        list.submit = [&](QJsonObject command) { submissions++; submitted = command; };
        list.show(); QVERIFY(QTest::qWaitForWindowExposed(&list)); QApplication::processEvents();
        list.previewDrag("source", 60);
        QMimeData mime; mime.setData(cardMime, QJsonDocument(QJsonObject{{"group", "42"}, {"entity", "source"}}).toJson());
        QDragEnterEvent enter(QPoint(20, 5), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list.viewport(), &enter); QVERIFY(enter.isAccepted());
        QDragMoveEvent move(QPoint(20, 5), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list.viewport(), &move);
        auto* animation = list.findChild<QVariantAnimation*>("kanbanPlacement"); QVERIFY(animation);
        animation->pause(); animation->setCurrentTime(0); QImage start = list.viewport()->grab().toImage();
        animation->setCurrentTime(180); QVERIFY(list.viewport()->grab().toImage() != start);
        QCOMPARE(list.count(), 1); QCOMPARE(submissions, 0);
        QDropEvent drop(QPointF(20, 5), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list.viewport(), &drop);
        QCOMPARE(submissions, 1); QCOMPARE(submitted["column"].toString(), QStringLiteral("target"));
        QCOMPARE(submitted["before"].toString(), QStringLiteral("anchor"));
        list.previewDrag({}, 0); list.clear(); list.previewDrag("source", 60);
        QDragEnterEvent emptyEnter(QPoint(20, 5), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list.viewport(), &emptyEnter); QVERIFY(emptyEnter.isAccepted());
        QDropEvent emptyDrop(QPointF(20, 5), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list.viewport(), &emptyDrop);
        QCOMPARE(submissions, 2); QVERIFY(submitted["before"].toString().isEmpty()); QCOMPARE(list.count(), 0);
    }

};
QTEST_MAIN(KanbanViewTest)
#include "test_kanban_view.moc"
