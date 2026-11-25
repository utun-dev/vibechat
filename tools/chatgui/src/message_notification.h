/* Одно уведомление о сообщениях: не берёт фокус, ждёт активности и скрывается через 10 секунд.
 * До первой активности автоматического закрытия нет; дальнейшая активность не продлевает начатый отсчёт.
 * Новое сообщение заменяет содержимое и заново ждёт активности. Только GUI-поток, один внешний владелец QWidget.
 * Окно верхнего уровня без parent, не удаляет себя при close; все дочерние widgets/monitor принадлежат ему.
 * На Wayland размещение/поверхность контролирует compositor, показ на текущем desktop — best effort. */
#pragma once

#include <QWidget>
#include <QTimer>

class QLabel;
class QScreen;
class UserActivityMonitor;

class MessageNotification : public QWidget {
    Q_OBJECT
public:
    MessageNotification();
    ~MessageNotification() override;
    // target — ключ владельца (десятичный channel_id либо dm:conv_id), sender/channel — только отображаемые строки.
    // Экран: trayGeometry.center() → preferredScreen → primaryScreen; без пригодного экрана окно скрывается.
    // preferredScreen заимствован на вызов; содержимое/target копируются, старый countdown отменяется.
    void showMessage(const QString& target, const QString& sender, const QString& channel,
                     const QRect& trayGeometry, QScreen* preferredScreen = nullptr);
    // Скрыть без activated, остановить timer/monitor; reason — строка причины для лога, например "close-button".
    void dismiss(const char* reason);

signals:
    // Только левый клик по телу: передаёт последний target после скрытия; кнопка закрытия сигнал не испускает.
    void activated(const QString& target);

protected:
    void mouseReleaseEvent(QMouseEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    QString m_target; // Последний target для activated, прежние сообщения не накапливаются.
    QLabel* m_body = nullptr; // Дочерний QLabel с plain text отправителя и канала, без тела сообщения.
    UserActivityMonitor* m_activity = nullptr; // Дочернее одноразовое наблюдение первой активности после showMessage.
    QTimer m_hideTimer; // Single-shot 10с от первой активности; hide/dismiss/showMessage отменяют его.
};
