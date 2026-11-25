/* Диалог входа в канал по utun://: проверяет ссылку, отправляет запрос ядру и ждёт CONNECT_RESULT.
 * joined сообщает об успешном входе, а не только о создании транспорта к connection-узлу.
 * Только GUI-поток; диалог занимает единственный connect_result callback bridge и снимает его в деструкторе.
 * Работает как activeModalWidget: не совмещать с другим владельцем этого callback.
 * Закрытие снимает получение результатов, но не отменяет уже принятый JOIN в ядре. */
#pragma once

#include <QByteArray>
#include <QDialog>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <cstdint>

class UtunNode;
class DbManager;

class JoinDialog : public QDialog {
    Q_OBJECT
public:
    // node/db — заимствованный контекст; autoJoinLink заполняет поле и ставит попытку JOIN на следующий оборот Qt-loop.
    explicit JoinDialog(UtunNode* node, DbManager* db, QWidget* parent = nullptr,
                        const QString& autoJoinLink = QString());
    ~JoinDialog() override;

signals:
    void joined(const QString& channelId); // Десятичный ID успешно принятого канала; испускается перед accept().

private slots:
    void onConnectClicked();
    void onLinkTextChanged();

private:
    void setStatus(const QString& text, bool isError = false);
    void onConnectResult(const QByteArray& peer, const QByteArray& group, int result, uint64_t request);

    UtunNode*  m_node;          // Контекст вызывающего кода, диалог им не владеет.
    DbManager* m_db;            // Контекст БД без передачи владения.
    QLineEdit* m_linkEdit;      // Дочернее поле исходной ссылки; read-only во время JOIN.
    QLabel*    m_statusLabel;   // Дочерний виджет предпросмотра/состояния/ошибки.
    QPushButton* m_connectBtn;  // Повторная отправка запрещена до результата.
    QPushButton* m_cancelBtn;   // Закрывает диалог, не отменяет запрос ядра.
    QTimer*     m_decodeTimer;  // Дочерний single-shot 400мс для предпросмотра после изменения текста.
    bool       m_connecting = false; // Ожидается результат текущей попытки.
    uint64_t   m_request = 0;       // Корреляция bridge; принимается только совпадающий request/node/channel.
    QByteArray m_pendingPeer;      // X25519 connection-узла из отправленной ссылки.
};
