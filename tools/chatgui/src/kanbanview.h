/* Desktop-доска: снимки/команды выполняются ядром через мост uasync.
 * Виджет хранит только UI-снимок, drag-and-drop и родительские версии редактора.
 * Уведомления приходят в GUI-потоке; поздние ответы другой группы игнорируются. */
#pragma once
#include <QWidget>
#include <cstdint>
#include <QJsonObject>
#include <QHash>
#include <QPointer>
#include <QSet>
class DbManager;
class QVBoxLayout;
class QScrollArea;
class CardList;
class QListWidgetItem;
class KanbanInlineEditor;
class QLabel;
class QDialog;
class QTextEdit;
class QPushButton;
class EmojiPanel;
class InputBar;
struct attachment_send_req;
class QComboBox;
class QDateTimeEdit;
class QCheckBox;
class QListWidget;
class QAbstractItemModel;
class ChatView;
class QStandardItemModel;
class KanbanView : public QWidget {
    Q_OBJECT
public:
    explicit KanbanView(QWidget* parent = nullptr, DbManager* db = nullptr);
    ~KanbanView() override;
    void setGroup(const QString& group);
    // Открыть карточку после свежего снимка; message задаёт окно обсуждения вокруг постоянного ID.
    void openTarget(const QString& card, const QString& message = {});
    void finishEditing(); // Ставит сохранение активной карточки в очередь перед остановкой ядра.
    QWidget* detailsPanel() const { return m_details; } // Главное окно размещает справа; один экземпляр на доску.
    void setMembers(QAbstractItemModel* members);
    void setVideoRecordingEnabled(bool enabled); // Возможности приложения; открывает запись фиксированной карточки через MainWindow.
    void onMessageSubmitted(const uint8_t* data, int size); // Подтверждения общего sender; чужие request игнорирует composer.
    void onAttachmentPreparation(const uint8_t* data, int size); // Индикатор подготовки общего composer.

    int assignmentState(const QString& ed25519) const; // -1: нельзя назначать; 0: свободен; 1: исполнитель выбранной карточки.
    void toggleAssignee(const QString& ed25519); // Проверяет права и отправляет assign/unassign с текущим grant.

signals:
    void linkActivated(const QString& link);
    void targetUnavailable(const QString& reason);
    void videoRecordingRequested(const QString& group, const QString& card, const QString& title); // Снимок карточки до открытия панели.
    void assignmentsChanged(); // Обновляет значки участников после выбора карточки/ответа ядра.
    void cardSelected(bool selected); // Выбор/закрытие переключает layout главного окна.
    void inviteRequested(const QString& group);
    void openImageRequested(const QString& path);
    void openVideoRequested(const QString& path);
    void settingsRequested(const QString& group);
private:
    quint64 send(QJsonObject command, const QString& group = {});
    void reload();
    void receive(const uint8_t* data, int size);
    void render();
    void readWidths(); // Один READ отсутствующих локальных ширин; готовые значения применяются без SQL/пересоздания редактора.
    void edit(const QJsonObject& card);
    void beginInline(CardList* list, QListWidgetItem* item, QPoint point);
    void finishInline();
    void closeInline();
    void continueEditing();
    void selectCard(const QString& id, bool load = true);
    void refreshDetails();
    void loadMessages(const QString& before = {});
    void renderMessages();
    void submitAttachment(const QString& target, attachment_send_req* request); // Забирает собственный request, фиксирует карточку.
    QPointer<DbManager> m_db; // Не владеет менеджером; локальные ui_state читаются отдельным worker, запись через мост uasync.
    QHash<QString, int> m_widths; // Локальные ширины group/column, включая ещё не исполненные записи.
    QSet<QString> m_widthReads; // Ключи ожидающих READ, исключают повторные запросы при новых снимках доски.
    quint64 m_widthGeneration = 0; // Смена SQLite-привязки отменяет прежние результаты и очищает кеш.
    QString m_selectedCard;
    QString m_targetCard, m_targetMessage, m_historyAround, m_jumpMessage;
    quint64 m_targetRevision = 0; // Отменяет отложенную прокрутку при смене карточки/цели.
    quint64 m_targetBoardRequest = 0; // Только свежий ответ конкретного snapshot разрешает ссылку.
    QWidget* m_details;
    QTextEdit* m_chatInput;
    InputBar* m_composer = nullptr; // Общий ввод чата: буфер изображений, preview, emoji, файлы и видео.
    EmojiPanel* m_emojiPanel = nullptr; // Overlay поверх истории, принадлежит её viewport.

    QComboBox* m_cardAuthor;
    QDateTimeEdit* m_due;
    QCheckBox* m_hasDue;
    QLabel *m_created, *m_assigned;
    ChatView* m_messages;
    QStandardItemModel* m_messageModel;
    QPointer<QAbstractItemModel> m_members;
    QHash<QString, QString> m_chatDrafts;
    QHash<QString, QString> m_dueDrafts;
    QHash<quint64, QPair<QString, QString>> m_dueRequests; // Ключ карточки и отправленный срок; новый черновик не стирается ответом.
    QString m_detailKey, m_messageBefore, m_chatRefreshUntil;
    QJsonObject m_detailCard;
    QHash<QString, QJsonObject> m_chatRows;
    quint64 m_chatRequest = 0;
    struct ChatSubmission { quint64 composer; QString key, text; }; // Соответствие ответа Kanban снимку черновика InputBar.
    QHash<quint64, ChatSubmission> m_chatSubmissions;
    bool m_fillingDetails = false, m_chatReloadPending = false;
    QString m_group;
    QJsonObject m_board;
    quint64 m_next = 0, m_displayed = 0;
    QHash<quint64, QString> m_pending, m_requestGroups;
    struct Draft { QJsonObject card; QString text, color, column; bool creating = false; };
    QHash<QString, Draft> m_drafts;
    QPointer<KanbanInlineEditor> m_inline;
    QPointer<CardList> m_editList;
    QListWidgetItem* m_editItem = nullptr;
    Draft m_draft;
    QString m_editGroup, m_nextColumn, m_nextCard;
    QPoint m_nextPoint;
    QVBoxLayout* m_layout;
    QScrollArea* m_scroll;
    QLabel* m_status;
    quint64 m_editRequest = 0;
    bool m_destroying = false, m_dragging = false, m_pointerDown = false, m_renderPending = false;
};
