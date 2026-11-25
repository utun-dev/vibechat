/* Немодальный браузер файлов участников CHAT-группы на выбранном подписанном storage-узле.
 * Создать с живым DbManager, выбрать группу через setChannel, передавать события bridge в onResponse/onTransfer.
 * Все методы и доставка событий — GUI-поток; локальный обход делает MemberFileBatch, сеть/передачи — ядро.
 * Контекст и request_id отсекают старые ответы LIST/QUOTA; активная передача блокирует навигацию/смену группы.
 * Отказ постановки показывается сразу; отмена принятой команды приходит как обычное файловое событие.
 * Copy/drag публикуют локальные URI после полного Download; успешный временный экспорт остаётся в кеше.
 * WA_DeleteOnClose: хранить окно через QPointer. При закрытии batch сначала отменяется и завершает свой I/O. */
#pragma once
#include <QDialog>
#include <QHash>
#include <QByteArray>
#include <QStringList>
#include <QThreadPool>
#include "batch.h"
#include <cstdint>
class DbManager;
class QComboBox;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;
class QLabel;
class QProgressBar;
class QAction;
struct mf_message;

class MemberFileBrowser : public QDialog {
    Q_OBJECT
public:
    // db не передаёт владение и должен жить дольше окна; parent владеет диалогом по правилам QObject.
    explicit MemberFileBrowser(DbManager* db, QWidget* parent = nullptr);
    // Отменить ожидаемые запросы/передачу; дочерний batch при уничтожении ждёт своего локального worker.
    ~MemberFileBrowser() override;
    // Полный адрес группы в channel; пустой owner выбирает виртуальный корень всех участников.
    // Заполняет серверы/владельцев из БД и начинает LIST/QUOTA; при занятом окне сохраняет прежний контекст.
    void setChannel(const QString& channel, const QByteArray& owner = {});
    // Внешний drop: выбрать корень своей ноды, предложить сервер и запустить Upload абсолютных paths.
    // Занятое окно отклоняет новый drop; активная очередь сохраняет своё назначение.
    void uploadOwnFiles(const QString& channel, const QStringList& paths);
    // Заимствованный CHAT_EVT_MEMBER_FILES_RESPONSE: group:32/server:32 и wire mf_message.
    // Чужой контекст и устаревшие ID игнорируются; подходящий ответ сначала предлагается активному batch.
    void onResponse(const uint8_t* data, int length);
    // Заимствованный CHAT_EVT_MEMBER_FILES_TRANSFER; сначала batch, затем одиночная передача.
    void onTransfer(const uint8_t* data, int length);
signals:
    // Экспорт завершился любым исходом; ожидающий drag может прочитать готовый список либо пустой результат.
    void exportFinished();
protected:
    // Принимать только локальные Copy-drop в каталог своей ноды; показывать подсказку на дереве.
    bool eventFilter(QObject* target, QEvent* event) override;
    // При активном batch отложить закрытие до его finished, инициировав отмену.
    void closeEvent(QCloseEvent* event) override;
    // Escape/reject использует тот же порядок отмены batch перед закрытием.
    void reject() override;
private:
    void refresh(); // Отменить прежние LIST/QUOTA, очистить дерево и запросить снимок текущего каталога.
    void requestQuota(); // Запросить квоту выбранного owner, если предыдущий QUOTA уже завершился.
    void navigate(const QString& path); // Проверить серверный путь, сменить каталог и начать refresh.
    void enter(QTreeWidgetItem* item); // Открыть owner/каталог либо предложить локальное сохранение и открытие файла.
    void updateActions(); // Пересчитать доступность действий и блокировку навигации по состоянию окна.
    QByteArray owner() const; // Ключ владельца; пустой — виртуальный корень всех участников.
    QByteArray server() const; // Ключ storage; пустой — доступного сервера нет.
    QString selectedPath() const; // Путь текущей строки внутри owner; пустой при отсутствии строки.
    QByteArray selectedVersion() const; // Снимок текущей строки либо пустой массив.
    bool post(struct mf_message& message); // Назначить request_id/kind, отправить через bridge; false при отказе приёма команды.
    void upload(); // Выбрать локальные файлы диалогом и передать их в beginUploads.
    void beginUploads(const QStringList& paths, bool chooseServer); // Проверить контекст, при необходимости выбрать storage.
    QList<MemberFileEntry> selectedEntries() const; // Снимок выделения в порядке дерева, с remote/version/type.
    MemberFileBatch* createBatch(MemberFileBatch::Mode mode); // Создать дочернюю очередь с progress/conflict; caller задаёт finished.
    void download(bool open); // Предложить назначение: очередь для дерева/выделения, одиночная передача для файла.
    // clipboard=true публикует URI в буфер; иначе destination задаёт каталог, пустой destination запускает drag.
    void exportFile(bool clipboard, const QString& destination = {});
    void publishExport(const QStringList& paths); // Передать готовые локальные URI и Copy MIME системному буферу обмена.
    // Подготовить одиночную передачу с ожидаемым version; true означает публикацию команды через bridge.
    bool startTransfer(const QString& remote, const QString& local, bool upload, const QByteArray& version);
    void mutate(uint8_t command); // MKDIR/RENAME либо подтверждённая рекурсивная очередь Remove для своего owner.
    void cancel(); // Отменить batch, отложенный drop и одиночную передачу.
    DbManager* m_db; // Заимствованная БД: только чтение из GUI-потока.
    QByteArray m_group; // Ed25519 группы.
    quint64 m_listId = 0, m_transferId = 0; // Активные одиночные LIST/transfer.
    quint64 m_quotaId = 0, m_refreshGeneration = 0; // QUOTA и поколение refresh для отложенного повтора MF_BUSY.
    QByteArray m_transferGroup;
    QByteArray m_transferServer; // Контекст одиночной передачи для refresh после успеха.
    QString m_path, m_local; // Текущий каталог внутри owner и локальное назначение одиночного download.
    bool m_openAfter = false; // Открыть успешный одиночный download через QDesktopServices.
    MemberFileBatch* m_batch = nullptr; // Дочерняя очередь; сбрасывается при finished, затем deleteLater.
    bool m_closeAfterBatch = false; // Повторить закрытие после отмены/finished текущего batch.
    quint64 m_dropId = 0; // Отложенный drop/модальное подтверждение; 0 также означает отмену.
    quint64 m_exportId = 0; // Текущий Download для Copy/drag/экспорта в выбранный каталог.
    int m_staleRetries = 0; // Один автоматический повтор LIST после MF_STALE.
    QHash<quint64, uint8_t> m_actions; // Ожидаемые STAT/MKDIR/RENAME вне LIST/QUOTA/batch.
    // Все элементы интерфейса ниже принадлежат дереву QObject диалога.
    QComboBox* m_servers;
    QComboBox* m_owners;
    QLineEdit* m_location;
    QWidget* m_navigation;
    QTreeWidget* m_entries;
    QPushButton *m_upload, *m_download, *m_mkdir, *m_rename, *m_remove, *m_cancel;
    QLabel* m_status;
    QLabel* m_uploadStatus;
    QLabel* m_dropHint;
    QLabel* m_quota;
    QProgressBar* m_progress;
    QAction* m_copyAction;
};
