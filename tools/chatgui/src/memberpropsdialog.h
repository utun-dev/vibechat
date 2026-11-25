/* GUI-диалог локального ника и подписанных прав участника; оба изменения фиксируются одной транзакцией ядра.
 * Accepted означает подтверждённую запись, отказ оставляет черновик. Закрытие снимает callback, не отменяя принятую запись.
 * Передача admin-ключа запрашивается отдельно с подтверждением пользователя; ключи остаются в ядре. */
#pragma once

#include <QDialog>
#include <QCheckBox>
#include <QLineEdit>
#include <QPushButton>
#include <QString>
#include <QPointer>
#include <stdint.h>

class DbManager;
class QLabel;

class MemberPropsDialog : public QDialog {
    Q_OBJECT
public:
    /* db заимствован через QPointer, parent владеет диалогом; channelId/ed фиксируют адресата.
     * memberName/flags — начальный снимок отображения; актуальные права, JSON/ver и локальный ник читаются из БД. */
    explicit MemberPropsDialog(DbManager* db, const QString& channelId,
                                const QByteArray& ed, const QString& memberName,
                                uint8_t flags, bool isSelf, QWidget* parent = nullptr);

private slots:
    void onSave(); // Снимок формы передаётся ядру один раз до результата; private key в запрос не включается.
    void onTransferAdmin(); // После Yes ставит собственный запрос ядру, не подтверждает получение ключа адресатом.

private:
    void setSaving(bool saving); // Блокирует редактирование/повторную передачу ключа, оставляя возможность закрыть форму.
    void finishSave(int result); // Успех закрывает форму; ошибка восстанавливает ввод и показывает отказ.
    QByteArray m_ed25519; // Ключ участника на момент открытия формы.
    QPointer<DbManager> m_db; // Заимствованная read-only проекция; не передаётся в worker.
    QString    m_channelId; // Собственный ID канала.
    uint64_t   m_nodeId = 0; // Локальный индекс для записи свойств.
    bool m_haveAdminKey = false; // На момент открытия БД содержала ключ; ядро проверяет полномочия заново.
    bool m_isOwn = false; // Собственный ник не редактируется, передача ключа себе запрещена.
    uint8_t    m_flags; // Резервные флаги отображения при неудачном чтении; такую форму сохранять нельзя.
    int        m_currentVer; // Версия owner-блока при открытии: предотвращает перезапись устаревшей формой.
    QString    m_memberName; // Имя в предупреждении о передаче ключа.

    QLineEdit* m_nickEdit; // Локальный неподписанный псевдоним, пустой снимает его.
    QCheckBox* m_adminCb; // Желаемая роль admin; историю и подпись строит ядро.
    QCheckBox* m_superCb; // supernode=yes/no.
    QCheckBox* m_storageCb; // storage=yes/no.
    QCheckBox* m_verifiedCb; // verified=yes/no.
    QCheckBox* m_deletedCb; // deleted=yes/no.
    QPushButton* m_transferBtn; // Доступна с admin-ключом для другого участника.
    QPushButton* m_saveBtn; // Отключена во время записи.
    QLabel* m_errorLabel; // Ошибка без технических подробностей; причина остаётся в логе.
    QObject* m_saveContext = nullptr; // Собственный context запроса; уничтожение снимает callback в мосте.
    bool m_loaded = false; // Начальный SQL-снимок прочитан; иначе Save заблокирован.
};
