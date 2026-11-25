/* Предложить владельцу utun:// ссылки вступить в текущий канал: передаёт исходную ссылку в gui_bridge_offer_link.
 * channelId задаёт наш канал, channelId/joinKey самой ссылки не заменяют его. Успех — вступление адресата.
 * Только GUI-поток; модальный диалог занимает единственный connect_result callback, доставка идёт activeModalWidget.
 * При закрытии callback снимается, уже принятый OFFER/JOIN в ядре не отменяется. */
#pragma once

#include <QByteArray>
#include <QDialog>
#include <QString>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>

class UtunNode;

class InviteByLinkDialog : public QDialog {
    Q_OBJECT
public:
    // channelId — десятичный ID нашего канала; node заимствован и нужен для проверки ссылки на самого себя (может быть nullptr).
    explicit InviteByLinkDialog(const QString& channelId, UtunNode* node, QWidget* parent = nullptr, const QString& link = QString());
    ~InviteByLinkDialog() override;

private slots:
    void onInviteLinkTextChanged();
    void onSendInvite();
    void onConnectResult(const QByteArray& peer, const QByteArray& group, int result, uint64_t request);

private:
    void setInviteStatus(const QString& text, bool isError = false);

    QString     m_channelId;       // Неизменный канал назначения приглашения.
    UtunNode*   m_node;            // Заимствованный узел GUI; не освобождается диалогом.
    QLineEdit*  m_inviteLinkEdit = nullptr; // Дочерние widgets принадлежат диалогу; поле исходной ссылки адресата.
    QLabel*     m_inviteStatusLabel = nullptr; // Предпросмотр и результат последней попытки.
    QPushButton* m_inviteSendBtn = nullptr;    // Заблокирована до результата; успех закрывает диалог через 400мс.
    bool        m_inviteSending = false;      // Ожидание ответа; текст поля не меняет уже отправленный target.
    uint64_t    m_request = 0;                 // Корреляция bridge, проверяется вместе с target node_id.
    QByteArray  m_inviteTarget; // Зафиксированный X25519 адресата.
};
