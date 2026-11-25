/* Диалог выдачи invite-ссылки и QR: запрашивает кандидатов и зарегистрированную ссылку у ядра.
 * requestId/channel связывают асинхронный ответ с текущим запросом; URL отображается после LINK_READY.
 * Только GUI-поток, при готовом bridge/core; одновременно один ShareInviteDialog.
 * Занимает callbacks candidates/link_ready/local_devices/local_delivery и снимает их в деструкторе.
 * Закрытие отменяет построение и отзывает ключ текущего запроса окна.
 * Локальная доставка подтверждает получение ссылки устройством, а не вступление в канал. */
#pragma once

#include <QByteArray>
#include <QDialog>
#include <QString>
#include <stdint.h>

class DbManager;
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;

class ShareInviteDialog : public QDialog {
    Q_OBJECT
public:
    // channelId — десятичный ID; db заимствован для имён узлов (nullptr допускается, тогда показываются ID).
    explicit ShareInviteDialog(const QString& channelId, DbManager* db, QWidget* parent = nullptr);
    ~ShareInviteDialog();

private slots:
    void onNodeSelected(int index);

private:
    void requestLink(const QByteArray& target);
    void revokeLink();
    void setLink(const QString& link);
    void onCandidates(const char* chId, int chIdLen, const uint8_t* data, int len);
    void onLinkReady(const char* chId, int chIdLen, uint64_t requestId, const char* link);
    void onLocalDevices(const uint8_t* data, int len);
    void onLocalDelivery(const uint8_t* data, int len);
    void sendLocalInvite();
    void updateLocalSend();

    uint64_t    m_requestId = 0;   // Только последний запрос построения ссылки; запоздалые LINK_READY игнорируются.
    bool        m_finished = false; // Закрытое окно больше не запрашивает ссылки.
    QString     m_channelId;       // Канал, для которого строится ссылка.
    DbManager*  m_db;              // Заимствованная БД, не закрывается диалогом.
    QComboBox*  m_nodeCombo;       // Дочерний список connection-узлов, первым идёт auto-кандидат.
    QLineEdit*  m_linkEdit;        // Готовая ссылка, только чтение; очищается при новом запросе.
    QLabel*     m_qrLabel;         // Дочерний QR готовой ссылки.
    QWidget*    m_localRow;             // выбор устройства; скрыт, пока устройств нет
    QComboBox*  m_devices;         // Дочерний список устройств LAN, данные пункта — бинарный адрес транспорта.
    QPushButton* m_sendLocal;      // Активна при готовой ссылке/устройстве и отсутствии ожидающей доставки.
    QLabel*     m_delivery;        // Дочерний статус доставки ссылки в LAN.
    QByteArray  m_localAddress;  // Адрес ожидаемого ACK.
    uint64_t    m_localRequest = 0;     // текущая доставка; 0 — нет ожидания ACK
};
