/* История и composer выбранного канала/PM, read/scroll, загрузки, feedback, рация и каталог streams.
 * setFocus передаёт фокус полю сообщения через focusProxy composer.
 * Все методы/callbacks — GUI. Сетевые операции передаются мосту с владением запросом и привязкой instance.
 * Модель держит самостоятельные Qt-значения; DbManager/AudioRecorder заимствованы.
 * PM снимки проверяются по conv/generation, канальные строки — по message_id/version.
 * Полная история ограничена 200 строками; окно старой цитаты защищено от вытеснения свежими сообщениями.
 * messagefeedback.cpp реализует опросы/реакции этого же класса. */
#pragma once

#include <QWidget>
#include <QStandardItemModel>
#include <QHash>
#include <QTimer>
#include <QSet>
#include <QThreadPool>
#include <QJsonObject>
#include <QPointer>
#include <QImage>
#include <memory>
#include "stream_sources.h"

class ChatView;
class InputBar;
class EmojiPanel;
class FormattingPanel;
class DbManager;
class ChannelList;
class AudioRecorder;
class QLabel;
class QPushButton;
class RadioPanel;
class MemberListModel;
class QDialog;
class QMenu;
class QTreeWidget;
struct MessageRow;

class MessageList : public QWidget {
    Q_OBJECT
public:
    // db/recorder заимствованы; Qt parent владеет виджетом. nullptr db допустим для пустого UI.
    explicit MessageList(DbManager* db = nullptr, AudioRecorder* recorder = nullptr,
                         QWidget *parent = nullptr);

    // Сохранить канал, сменить target; для dm:<id> зафиксировать generation и запросить снимок.
    void loadChannel(const QString& channelId);
    // Перечитать только заголовок группы; история, прокрутка и черновик сохраняются.
    void refreshChannelTitle();
    // Переход по постоянному ID; false означает отсутствие цели в локальном снимке.
    bool jumpToMessage(const QString& messageId);
    // Добавить отсутствующие сообщения (до 200); не вытеснять историческое окно цитаты.
    void refresh(bool outgoing = false); // Новое своё сообщение возвращает к последним; входящее — только из близкой к низу позиции.
    // Перечитать имена, обновить авторов и превью цитат.
    void reloadLocalNicks();
    // Для CHAT поставить read/верхнюю позицию в ui_state; окно старой цитаты сохраняет свежий watermark. PM пропускается.
    void saveChannelState();
    // После остановки AnimTimer удалить общие LottieIcon; не вызывать при их отрисовке.
    static void cleanupAnimations();
    // Обновить только (chId,msgId) текущей модели без сетевого запроса.
    void updateMessageProgress(const QString& chId, int64_t msgId, int blocksDone, int numBlocks);
    // Перечитать путь/роли; отложенный пользовательский play требует прежний target.
    void onAttachmentDownloaded(const QString& chId, int64_t msgId);
    // [conv_len,conv,generation,JSON]: чужое/старое поколение не применяется.
    void onDmMessages(const uint8_t* data, int len);
    // Только успешное пользовательское открытие; snapshot сохраняется до запуска просмотрщика.
    void onAttachmentOpened(const QString& path);
    /* Блокировать отправки ожидающего удаления; forget очищает UI только для удалённого поколения и возвращает true. */
    void setDmDeleting(const QString& convId, quint64 generation, bool deleting);
    bool forgetDmHistory(const QString& convId, quint64 generation);
    // Запросить адресата/общую группу; отказ очищает доступность кнопки.
    void refreshDmCallRoute();
    // [conv_len,conv,peer,group] только для выбранного PM; group=0 отключает звонок.
    void onDmCallRoute(const uint8_t* data, int len);
    // Сбросить peer/group и показать offline.
    void clearDmCallRoute();
    // Проверить codec и передать target/job/state composer.
    void onAttachmentPreparation(const uint8_t* data, int len);
    // Запросить свежие реакции/опрос для изменённого сообщения текущего канала.
    void onFeedbackChanged(const uint8_t* data, int len);
    // Проверить результат/JSON-снимок; чужой канал не меняет модель.
    void onFeedbackResult(const uint8_t* data, int len);
    // 9 байт (request_id LE,success) подтверждают/отклоняют зафиксированный черновик.
    void onMessageSubmitted(const uint8_t* data, int len);
    // Применить новую версию подписанного item с сохранением scroll и превью ответов.
    void onMessageUpdated(const uint8_t* data, int len);
    // Обновить encrypted-вложение и диалог выдачи ключей текущего канала.
    void onAttachmentKeysChanged(const uint8_t* data, int len);
    void setRadioMembers(MemberListModel* members); // Заимствованная модель для ников/речи/mute в панели рации.
    bool enableRadioForCall(quint64 id); /* Только после SELECT; учитывает собственное включение попытки. */
    void finishRadioCall(quint64 id, bool accepted); /* Отбой откатывает только собственное ещё не принятое включение. */
    bool enableRadio(); /* идемпотентное включение рации выбранного канала, false при ошибке аудио */
    void disableRadioForPhone(); // Снять подписку фактической группы и PTT, независимо от выбранного чата.
    // Применить проверенный полный каталог выбранной CHAT-группы.
    void onStreamSources(const uint8_t* data, int len);
    // Очистить каталог/кнопку remote streams.
    void clearStreams();
    // Наблюдать CHAT-группу; пустой target/PM передаёт 0 и снимает наблюдение.
    void watchStreams();
    /* group=0: обычная кнопка запуска; иначе красная кнопка управления своей публикацией. GUI-поток. */
    void setStreamPublisherGroup(const QString& group);
    /* Обновить число зрителей активного publisher; снимок другой группы игнорируется. GUI-поток. */
    void setStreamPublisherViewers(const QString& group, unsigned viewers);

    void setVideoRecordingEnabled(bool enabled); // Возможности приложения передаются composer.
signals:
    void videoRecordingRequested(const QString& target, quint64 generation, const QByteArray& reply, const QByteArray& quote);
    void videoRecordingSubmitted(quint64 request, bool success, bool tooLarge); // Подтверждение отдельной панели записи.
    void videoRecordingInvalidated(const QString& target, quint64 generation);
    void readPositionChanged(const QString& channelId, qint64 lastReadMsgId);
    void callRequested(const QString& target, const QByteArray& peer, const QString& name);
    void openVideoRequested(const QString& filePath);
    void streamRequested(const QString& group, const QByteArray& source, quint64 epoch, const QString& name);
    void allStreamsRequested(const QString& group);
    void streamPublishRequested(const QString& group);
    void openImageRequested(const QString& filePath);
    void inviteRequested(const QString& sourceChannel, const QByteArray& peer, const QString& name);
    void ownFilesRequested(const QString& channelId);
    void filesRequested(const QString& channelId);
    void shareInviteRequested(const QString& channelId);
    void channelSettingsRequested(const QString& channelId);
    void linkActivated(const QString& link);

protected:
    void changeEvent(QEvent* event) override; // Обновляет растровые значки при смене палитры.
    bool eventFilter(QObject* object, QEvent* event) override; // Resize/Show viewport планируют превью, paint ничего не загружает.
private:
    // Qt children: view/model/composer/panel. Внешний db не уничтожается этим виджетом.
    ChatView           *m_view;
    QStandardItemModel *m_model;
    InputBar           *m_inputBar;
    EmojiPanel         *m_emojiPanel;
    FormattingPanel    *m_formattingPanel;
    DbManager          *m_db;
    QString              m_currentChannelId; // Decimal CHAT id, dm:<conv_id> или пусто.
    quint64 m_currentDmGeneration = 0; /* Поколение выбранного PM; 0 для канала или отсутствующей беседы. */
    QHash<QString, quint64> m_dmDeleting; // Ожидающие удаления conv → generation.
    struct DmFileAction { QString conv; QByteArray author; quint64 seq, generation; };
    QHash<QString, DmFileAction> m_openingDmFiles; // Независим от смены выбранной беседы во время открытия.
    bool downloadDmFile(const QModelIndex& index);
    QHash<QByteArray, QColor> m_avatarCache; // Цвета авторов текущей модели.
    qint64                m_lastReadMsgId = 0; // Принятый мостом локальный watermark выбранного target.
    QTimer*               m_readDebounce; // Single-shot 300ms после scroll/обновления.
    qint64                m_savedTopMsgId = 0; // Якорь при перезагрузке того же канала.
    bool                  m_wasAtBottom = false; // Восстановить bottom после reload, если пользователь уже был в конце.
    QMap<QByteArray, QString> m_localNicks; // Локальные имена участников выбранного канала.
    AudioRecorder* m_recorder = nullptr;
    QLabel*        m_peerStatus = nullptr;
    QLabel*        m_channelTitle = nullptr;   /* шапка: название канала */
    QPushButton*   m_callBtn = nullptr;
    QPushButton*   m_inviteBtn = nullptr;
    QPushButton*   m_groupMenuButton = nullptr;
    QPushButton* m_keyRequestsButton = nullptr;
    QPointer<QDialog> m_keyRequestsDialog; // Немодальный список запросов ключей текущего канала.
    QTreeWidget* m_keyRequestsList = nullptr; // Child диалога; используется только при живом m_keyRequestsDialog.
    void refreshKeyRequests();
    void showKeyRequests();
    QByteArray m_callPeer; // Ed25519 адресата кнопки PM call.
    QByteArray m_callGroup; // Полный ключ общей READY-группы.
    QPushButton*   m_radioBtn = nullptr;       /* шапка: переключатель рации */
    QPushButton* m_streamBtn = nullptr; // Удержание/правая кнопка раскрывают список; короткое левое нажатие открывает все трансляции.
    QMenu* m_streamMenu = nullptr; // Дочернее немодальное меню, привязанное к снимку выбранной группы.
    QTimer m_streamHold; // Удержание 500мс открывает список и подавляет открытие всех при отпускании.
    bool m_streamPressed = false, m_streamLongPressed = false; // Нажатие над кнопкой, включая события при захвате мыши меню.
    void showStreamMenu(); // Раскрывает вниз список чужих трансляций; актуальность источника проверяется при выборе.
    QPushButton* m_streamPublishBtn = nullptr; // Запуск либо управление собственной публикацией, слева от рации.
    QLabel* m_streamViewerBadge = nullptr; // Зелёный овал поверх активной камеры; Qt child кнопки, пропускает нажатия.
    QPushButton* m_pollBtn = nullptr; // Создание опроса, справа от рации; только для группы.
    QPushButton* m_ownFilesBtn = nullptr; // Собственные файлы выбранной группы.
    void updateHeaderIcons(); // Контурные значки учитывают палитру; активная публикация выделяется красным.
    QString m_streamPublisherGroup; // Канал локального publisher, независимо от снимков каталога.
    unsigned m_streamPublisherViewers = 0; // Зрители текущей публикации по свежему снимку actor.
    QList<LiveStreamSource> m_streamSources; // Проверенный полный снимок выбранной группы.
    RadioPanel*    m_radioPanel = nullptr;    /* верхняя панель устройств, VAD и PTT */
    quint64 m_radioCallOwner = 0; // Сбрасывается ручным переключением.
    QString m_radioCallGroup; // Группа сессии, включённой приглашением; навигация её не меняет.
    QHash<QString, QJsonObject> m_feedback; // message_id hex → последний полный снимок реакций/опроса.
    QPointer<QDialog> m_pollDialog; // Один диалог голосования; уничтожение owner безопасно для exec.
    QString m_pollDialogMessage, m_pendingPoll; // Открытый message_id и ожидаемый запрос состояния.
    quint64 m_feedbackSequence = 0; // Корреляция userAction, 0 у фоновых запросов.
    QByteArray m_historyAnchor; // 32-байтный оригинал в историческом окне; пусто — свежая история.
    void updateReply(QStandardItem* item, const QByteArray& original, const QString& quote);
    void refreshReplies();
    // Разрешить подписанный ID в выбранном канале, загрузить окно и подсветить оригинал.
    quint64 m_jumpRevision = 0; // Поздний scroll старой ссылки не применяется после другого перехода/смены канала.

    bool sendFeedback(QJsonObject command, bool userAction = true); // true — запрос принят; ch фиксируется до модального UI.
    void setupFeedback(QStandardItem* item, const MessageRow& message, const QString& channel);
    // Проверить UTF-8 лимиты и передать опрос в исходный канал диалога.
    void createPoll();
    // Показать один диалог опроса или запросить недостающий снимок.
    void showPoll(const QString& messageId);
    void applyFeedback(const QJsonObject& state);
    QPointer<QMenu> m_messageMenu; // Popup принадлежит истории, закрывается при загрузке канала.
    void showMessageMenu(const QModelIndex& index, const QPoint& globalPos); // Реакции, копирование и ответ.
    void replyToMessage(const QString& channel, const QString& messageId); // Разрешить актуальный оригинал и открыть composer.
    void requestReaction(const QModelIndex& index, int emoji);
    QWidget* createReactionStrip(QMenu* menu, const QString& channel, const QString& messageId); // Строка с колесом и тонкой полосой.

    QString authorName(const QByteArray& key) const;
    void restoreScrollPosition(const QString& channelId);
    // Максимальный rowid полностью видимых строк; watermark меняется после принятия записи.
    void updateReadPosition();
    bool nearHistoryBottom() const; // До изменения модели: осталось не больше десяти строк текущего шрифта.
    void scrollHistoryToBottom(); // Синхронный layout и bottom, повтор после Qt layout с проверкой канала/поколения/окна.
    void showLatestMessages(); // Покинуть окно старой цитаты и прокрутить последние сообщения вниз.
    // Persistent index/QPointer защищают menu от замены модели/уничтожения UI.
    void onMessageContextMenu(const QPoint& pos);
    // Запросить выбранное поколение PM; ожидающее удаление блокирует запрос.
    void requestDmMessages(const QString& channelId);
    // Не более двух FFmpeg jobs; GUI callback защищён QPointer.
    // Owned-подписка в uasync и local audio; ошибка восстанавливает кнопку.
    void onRadioToggled(bool on);
    void updateRadioUi();
    static bool isDmChannel(const QString& channelId);
    static QString dmConvId(const QString& channelId);
};
