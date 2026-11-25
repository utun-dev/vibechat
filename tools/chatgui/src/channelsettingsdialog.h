/* Настройки одной группы в GUI-потоке: autoplay, звук, аватар, приглашение узла и локальное удаление.
 * DbManager заимствуется через QPointer; его уничтожение закрывает диалог с reject.
 * Save отправляет две независимые owned-команды ядру; accept означает приём обеих в очередь,
 * а не завершение SQL. Ошибки commit логируются ядром, повторное сохранение идемпотентно.
 * При отказе очереди окно остаётся открытым для повторной попытки; ранее принятая команда может выполниться.
 * Изменения аватара применяются сразу через GroupAvatarManager и не отменяются кнопкой Cancel. */
#pragma once

#include <QDialog>
#include <QCheckBox>
#include <QString>
#include <QPointer>

class DbManager;
class QLabel;

class ChannelSettingsDialog : public QDialog {
    Q_OBJECT
public:
    /* channelId — каноническая десятичная строка ID 1..INT64_MAX; parent владеет диалогом.
     * Без открытой db или при недопустимом ID только просмотр: Save/Delete недоступны. Дочерние поля принадлежат диалогу. */
    explicit ChannelSettingsDialog(const QString& channelId, DbManager* db, QWidget* parent = nullptr);

signals:
    void inviteByLinkRequested(const QString& channelId); // Открыть приглашение узла для этой группы; не закрывает настройки.

private slots:
    /* Читает поля и передаёт autoplay/звук в поток ядра; отказы выделения/очереди показывает без закрытия. */
    void onSave();
    /* Подтверждает локальное удаление (Cancel по умолчанию), заново проверяет наличие группы и очередь.
     * Принятие команды закрывает окно; CHAT_EVT_CHANNEL_DELETED приходит контроллеру от ядра. */
    void onDelete();

private:
    QString m_channelId;             /* Собственная копия ID целевой группы. */
    QPointer<DbManager> m_db;         /* Не владеет QObject и SQLite. */
    bool m_validChannel = false;     /* ID канонический и в диапазоне ядра 1..INT64_MAX. */
    QCheckBox* m_autoPlayVoice;       /* Дочерний autoplay-флаг; по умолчанию false. */
    QCheckBox* m_soundOnMessage;      /* Дочерний sound-флаг; по умолчанию true. */
    QLabel* m_status;                /* Дочерняя строка ошибки подготовки/отправки команды. */
};
