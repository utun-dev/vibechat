/* Главное окно desktop-чата: связывает списки, bridge-события, UtunNode, media и звонки.
 * Координирует запуск/остановку ядра, tray и отдельные окна; UI-обработчики работают в GUI-потоке. */
#pragma once

#include <QMainWindow>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QQueue>
#include <QSet>
#include <QHash>
#include <QTimer>
#include <QPointer>
#include <QPair>
#include <QJsonObject>
#include <memory>

class QStackedWidget;
class KanbanView;
class ChannelList;
class MessageList;
class AccountList;
class DbManager;
class QSplitter;
class UtunNode;
class AudioRecorder;
class VideoPlayerWindow;
class StreamPlayerWindow;
class StreamGridWindow;
class StreamPublishEngine;
class PhoneMediaDispatcher;
class StreamPublishWindow;
class VideoRecordingWindow;
class ImageViewerWindow;
class CallWindow;
class CallAudioEngine;
class MemberFileBrowser;
class MessageNotification;
class QMessageBox;
struct MessageRow;

struct AutoPlayItem {
    QString channelId; // Канал снимка; msgId уникален только внутри него.
    qint64 msgId = 0; // Локальный rowid сообщения.
    bool downloadStarted = false; // Команда загрузки принята очередью; false допускает первую попытку.
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // db заимствован до деструктора; nullptr создаёт UI без UtunNode. cfgPath — профиль, dbPath — override каталога.
    explicit MainWindow(QWidget *parent, DbManager* db, const QString& cfgPath,
                         const QString& dbPath = QString(),
                         const QString& debugFile = QString(),
                         const QString& debugLevel = QString(),
                         const QString& debugCategories = QString());
    // Снять callbacks, остановить media, stop/join node и очистить анимации.
    ~MainWindow() override;

protected:
    // После активации окна передать фокус composer; Qt сначала завершает восстановление фокуса дочерних виджетов.
    void changeEvent(QEvent* event) override;
    // Сохранить геометрию и скрыть в tray; для выхода служит requestShutdown.
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void showShareInviteDialog(const QString& channelId);
    void showJoinDialog();
    void onCreateGroupRequested();
    void showSettings();
    void openVideo(const QString& filePath);
    void openImage(const QString& filePath);
    void showChannelSettings(const QString& channelId);
    // Дождаться конца/подготовки player, разрешить путь головы и запустить один voice.
    void autoPlayTimerTick();

public:
    // ch_id задан с длиной; initialSync обновляет историю без уведомления, звука и auto-play.
    void onMessageReceived(const char* ch_id, int ch_id_len, const uint8_t author[32], bool initialSync);
    // Передать бинарный CHAT_MEMBER_DISPLAY снимок списку участников и кэшу аватаров.
    void onMemberList(const QString& chId, int count, const uint8_t* data, int dataLen);
    // Обновить участника по codec chat_member.h; байты заимствованы до возврата.
    void onMemberUpdated(const QString& chId, const uint8_t* data);
    // Удалить nodeId из модели/кэша выбранного chId.
    void onMemberRemoved(const QString& chId, const QByteArray& ed);
    // Обновить статус поиска соседей канала.
    void onAutoConnectStatus(uint8_t status, uint16_t total, uint16_t totalNodes, uint16_t connected);
    // Обновить online-счётчик ch_id указанной длины.
    void onChannelPeersOnline(const char* ch_id, int ch_id_len, uint16_t online);
    // Перечитать список, сбросить исчезнувший target и применить отложенный выбор.
    void reloadChannels();
    // Завершение (channel,rowid): разбудить очередь, проверить готовый encrypted voice.
    void onAttachmentDownloadedForAutoPlay(const QString& chId, int64_t msgId);
    // Сохранить geometry/read/scroll и поставить stop после этих команд; OOM допускает прямой stop с логом.
    void requestShutdown();
    // Заимствованный QObject-владелец ядра либо nullptr; управление — в GUI.
    UtunNode* utunNode() const { return m_node; }

    /* звонки */
    // Создать исходящее окно после принятия START мостом; chId может быть dm:<conv_id>.
    void onCallRequested(const QString& chId, const QByteArray& peer, const QString& name, bool devices = false);
    // Показать входящий callId с именем caller из DbManager; повтор ID не создаёт окно.
    void onCallIncoming(const QString& chId, const QByteArray& caller, quint64 callId);
    // Обновить состояние существующего исходящего окна и тон.
    void onCallRinging(quint64 callId);
    // Активировать callId/audio; при отказе capture послать hangup.
    void onCallAccepted(quint64 callId, const QByteArray& peer);
    // Завершить audio/окно, при PHONE_BUSY показать причину.
    void onCallEnded(quint64 callId, uint8_t reason);
    // Залогировать протокольную ошибку и закрыть окно callId.
    void onCallError(quint64 callId, uint8_t err);
    // Обновить диагностический BGP-путь существующего окна.
    void onCallPath(quint64 callId, const QString& path);
    // Снять callId из map до deleteLater; повтор безопасен.
    void closeCallWindow(quint64 callId);
    // Передать START для CHAT-канала и peer с новым ненулевым ID.
    void onRadioCallRequested(const QString& channel, const QByteArray& peer, bool devices = false);
    // Применить 27-байтный radio_call event, управлять диалогом/рингтоном и READY/ACCEPT.
    void onRadioCallEvent(const uint8_t* data, int len);

    /* DM (p2p чат) */
    // Скопировать общую группу/peer/имя в команду; PM выбирается после DM_CONV_UPDATED.
    void onPmRequested(const QString& chId, const QByteArray& peer, const QString& name, bool groups = false);
    // Открыть отправку свежего invite фиксированному peer; source может быть пустым из PM.
    void onInviteToPmRequested(const QString& source, const QByteArray& peer, const QString& name);
    // Общий вход URL из сообщений, диалога и внешнего запуска.
    void onMessageLinkActivated(const QString& link);
    // Выбрать только видимое совпавшее поколение истории PM.
    void onDmConvUpdated(const QString& convId, quint64 generation);
    void onDmStartResult(quint64 request, const QString& convId, quint64 generation, bool success);
    // Обновить PM/уведомление; выбранной беседе запросить снимок.
    void onDmMsgReceived(const QString& convId, const QByteArray& author);
    // Подтвердить удаление конкретного generation и заблокировать его composer.
    void onDmDeleteRequested(const QString& convId, quint64 generation, const QString& name);
    // Применить только зарегистрированный request/conv/generation без очистки новой истории.
    void onDmDeleteResult(quint64 request, const QString& convId, quint64 generation, bool success);

private:
    void showOpenLinkDialog();
    void applyObjectLink(const QString& channel);
    void objectLinkUnavailable(const QString& reason);
    QString m_pendingObjectLink, m_objectLinkUrl;
    quint64 m_objectLinkRevision = 0; // Отмена старого перехода/повтора после навигации пользователя.
    void setupUi();
    // Поставить owned geometry/maximized/splitter в ui_state.
    void saveWindowGeometry();
    // Восстановить размеры в пределах доступных экранов.
    void restoreWindowGeometry();
    // Сохранить общую геометрию фото/видео и идентификатор физического монитора в кэш и owned ui_state-команду.
    void saveMediaWindowGeometry(QWidget* window);
    // Восстановить геометрию/режим на прежнем физическом мониторе; без данных — обычное окно по центру текущего.
    void restoreMediaWindowGeometry(QWidget* window);
    void setupTray();
    void showIncomingMessageNotification(const QString& target, const QString& author);
    // Запустить UtunNode; stopping отсоединяет SQLite до native teardown.
    void setupNode();
    void setupMessaging();
    void setupSoundFromConfig();
    // Один MainWindow получает bridge events в GUI; деструктор снимает callbacks.
    void setupBridgeCallbacks();
    // Связать списки/composer/окна с командами и локальными действиями.
    void setupConnects();
    void onShutdownCheck();
    // Проверить последнюю запись или msgId>0; повторный/свой voice не добавлять.
    void autoPlayCheckNewMessage(const QString& channelId, qint64 msgId = 0);
    QString autoPlayMediaDir() const;
    // Путь обычного voice/проверенного plaintext VOICE; неизвестный encrypted MIME даёт пусто.
    QString autoPlayFilePath(const QString& channelId, const MessageRow& message) const;
    // Открыть CHAT browser с owner/общим списком; DM не поддерживается.
    void showMemberFiles(const QString& channel, const QByteArray& owner = {});
    void showStreamPublisher(const QString& group, StreamGridWindow* returnTo = nullptr); // Управление; закрытие возвращает фокус сетке.
    // Общая панель записи CHAT/DM/KANBAN; card/title — снимок исходной карточки, пусто для обычного чата.
    void showVideoRecorder(const QString& target, quint64 generation, const QByteArray& reply,
                           const QString& card = {}, const QString& title = {}, const QByteArray& quote = {});
    void watchCallMedia(CallWindow* window); // Обновлять предупреждение в окне дозвона без владения самим звонком.

    // Qt-дети главного окна: списки, splitter, tray/menu. DbManager — внешний заимствованный QObject.
    ChannelList     *m_channelList;
    void loadGroupContent(const QString& channel);
    QStackedWidget* m_contentStack = nullptr;
    KanbanView* m_kanban = nullptr;
    QWidget* m_unknownGroup = nullptr;
    MessageList     *m_messageList;
    AccountList     *m_accountList;
    QSplitter       *m_splitter = nullptr;
    QSplitter* m_leftColumns = nullptr; // Каналы; в режиме карточки участники находятся под ними.
    QStackedWidget* m_rightColumns = nullptr; // Участники либо подробности выбранной карточки.
    bool m_cardLayout = false;
    QList<int> m_normalColumnSizes, m_cardColumnSizes; // Независимые размеры двух режимов.
    QList<int> m_cardLeftSizes; // Высоты каналов и участников в режиме карточки.
    QSystemTrayIcon *m_trayIcon;
    QMenu           *m_trayMenu;
    std::unique_ptr<MessageNotification> m_messageNotification; // Единственное transient-уведомление.
    DbManager       *m_db;
    UtunNode        *m_node = nullptr; // Собственный QObject: worker join до удаления.
    // INI/override каталога и настройки общего logger, передаваемые до старта node.
    QString          m_cfgPath;
    QString          m_cfgDbPath;
    QString          m_cfgDebugFile;
    QString          m_cfgDebugLevel;
    QString          m_cfgDebugCategories;
    QString          m_pendingChannelSelect; // Выбор после появления канала/PM в SQLite.
    QString          m_currentChannelId; // Decimal CHAT id или dm:<conv_id>, пусто без выбора.
    quint64 m_dmDeleteSequence = 0; // Локальная корреляция подтверждённых удалений.
    quint64 m_dmStartSequence = 0, m_pendingDmStart = 0; // Только последний явный запрос открывает беседу.
    QHash<quint64, QPair<QString, quint64>> m_dmDeleteRequests; /* request_id → conv_id и удаляемое поколение */
    AudioRecorder    *m_recorder = nullptr; // Qt child, shutdown до остановки ядра.
    // Отдельные media windows; raw pointers обнуляются destroyed/closed callbacks, QPointer — автоматически.
    QPointer<VideoPlayerWindow> m_videoPlayer;
    QPointer<StreamPlayerWindow> m_streamPlayer;
    QPointer<StreamGridWindow> m_streamGrid;
    QPointer<VideoRecordingWindow> m_videoRecording; // Единственная локальная запись/черновик, удаляется до SoundManager/ядра.
    StreamPublishEngine* m_streamPublisher = nullptr; // Собственный publisher, удаляется до node.
    PhoneMediaDispatcher* m_phoneMedia = nullptr; // Единственный владелец перехода к разговору.
    QString m_radioBeforePhone; // Восстановить только прослушивание после полного release телефона.
    QPointer<StreamPublishWindow> m_streamPublishWindow;
    QPointer<ImageViewerWindow> m_imageViewer;
    QJsonObject m_mediaWindowState; // Общий снимок фото/видео: обычный rect, режим, имя/serial и начало области монитора.
    bool m_mediaWindowStateLoaded = false; // Снимок прочитан из открытой БД либо уже обновлён при закрытии окна.
    QPointer<MemberFileBrowser> m_fileBrowser;

    QQueue<AutoPlayItem> m_autoPlayQueue; // Последовательный playback чужих voice, ожидание загрузки головы.
    QTimer*              m_autoPlayTimer = nullptr; // 200ms, активен при непустой очереди.
    QTimer*              m_cpuLoadTimer = nullptr; // GUI polling 333ms.
    bool                 m_cpuLoadPending = false; // Не более одного ожидающего CPU snapshot.
    QTimer*              m_shutdownTimer = nullptr; // POSIX: проверка signal flag, 200ms.
    QSet<QPair<QString, qint64>> m_autoPlayDownloading; // (channel,rowid) ожидающей загрузки.
    QHash<QString, qint64> m_lastAutoPlayId; // Подавляет повторное добавление последнего сообщения канала.

    QHash<quint64, CallWindow*> m_activeCalls;   /* call_id → окно звонка */
    QPointer<QMessageBox> m_radioCallWindow; // Один адресный вызов в рацию; диалог удаляется при закрытии.
    quint64 m_radioCallId = 0; // 0 без незавершённого приглашения.
    bool m_radioCallAnswered = false; // finished не дублирует уже отправленное решение кнопки.
};
