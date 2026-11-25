/* Локальный псевдоним участника: GUI только читает SQLite, запись подтверждает поток ядра.
 * Отказ сохраняет форму и введённый текст; закрытие формы снимает callback, но не отменяет принятую запись. */
#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QString>
#include <QPointer>

class DbManager;
class QLabel;
class QPushButton;

class RenameDialog : public QDialog {
    Q_OBJECT
public:
    /* db заимствован через QPointer; channelId/ed выбирают строку, displayName — заголовок/подсказка, parent владеет диалогом. */
    explicit RenameDialog(DbManager* db, const QString& channelId,
                          const QByteArray& ed, const QString& displayName,
                          QWidget* parent = nullptr);

private slots:
    void onSave(); // Передать собственный снимок ника в uasync; повтор до результата игнорируется.

private:
    void finishSave(int result); // GUI: 0 закрывает с Accepted, отказ возвращает редактирование и показывает ошибку.
    QByteArray m_ed25519; // Ключ участника на момент открытия формы.
    QPointer<DbManager> m_db; // Заимствованная read-only проекция, может исчезнуть во время запроса.
    QString    m_channelId; // Собственный снимок канала.
    quint64    m_nodeId = 0; // Локальный индекс для записи псевдонима.
    QLineEdit* m_edit; // Локальный черновик; пробелы по краям удаляются при сохранении.
    QLabel* m_errorLabel; // Ошибка чтения/сохранения без технических подробностей.
    QPushButton* m_saveBtn; // Отключена до результата.
    QObject* m_saveContext = nullptr; // Собственный context одного запроса; удаляется при результате/закрытии.
    bool m_loaded = false; // Ник успешно прочитан; при ошибке чтения сохранение запрещено.
};
