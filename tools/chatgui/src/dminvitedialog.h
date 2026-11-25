/* Выбор группы для приглашения фиксированного собеседника в PM.
 * Показывает только группы, где локальный узел — участник; отправка выполняется dm_invite_trampoline.
 * Только GUI-поток: использовать exec(), обработчик результата выбирает activeModalWidget.
 * Занимает единственный dm_invite_result callback bridge; не совмещать с другим его владельцем.
 * Деструктор снимает callback и, если pending/core доступны, ставит отмену регистрации по request_id.
 * Уже отправленное PM-сообщение закрытием не отзывается. */
#pragma once

#include <QByteArray>
#include <QDialog>
#include <QString>
#include <cstdint>

class DbManager;
class QComboBox;
class QLabel;
class QPushButton;

class DmInviteDialog : public QDialog {
    Q_OBJECT
public:
    // db обязателен и используется при создании; peer — Ed25519 получателя, name — имя для UI/PM.
    // source — контекстный ID группы/маршрута для DM. Аргументы строк копируются, DB не переходит во владение диалога.
    DmInviteDialog(DbManager* db, const QByteArray& peer, const QString& name, const QString& source, QWidget* parent = nullptr);
    ~DmInviteDialog() override;
    // Читать после Accepted: conv_id без префикса dm:, непустой только после успешного результата отправки.
    QString conversationId() const { return m_convId; }

private:
    void sendInvite();
    void onResult(const uint8_t* data, int len);
    QByteArray m_peer; // Фиксированный Ed25519 получателя; сверяется в ответе.
    quint64 m_requestId = 0; // ID текущего запроса.
    QString m_name, m_source, m_convId; // Имя/контекст адресата и conv_id успешной личной беседы.
    QComboBox* m_groups; // Дочерний список доступных групп, блокируется на время регистрации/отправки.
    QLabel* m_status;   // Дочерний статус/ошибка без изменения выбранного адресата.
    QPushButton* m_send; // Дочерняя кнопка, недоступна до выбора группы и пока pending.
    bool m_pending = false; // Есть принятый запрос; снимается только совпадающим результатом.
};
