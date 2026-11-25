/* Панель участников и диагностика выбранного узла, только GUI-поток.
 * Владеет MemberListModel/виджетами/timer; DbManager заимствован через QPointer и может отсутствовать.
 * Live online/RTT/имя берутся из модели, статические даты/адреса — чтением SQLite;
 * маршруты/линки/сокеты — асинхронным MEMBER_DETAIL через owned bridge API.
 * Несколько панелей делят callbacks MEMBER_DETAIL/RADIO_MUTE_RESULT: снимаются после последней панели.
 * Ответ применяется только при совпадении request_id/group/node; ID общие для процесса, не сбрасываются.
 * Видимая карточка опрашивается раз в секунду, одновременно один запрос, timeout 5 с.
 * Скрытие/смена выбора/канала и недоступность ядра снимают ожидаемый токен; старый ответ игнорируется.
 * Меню и диалоги допускают обновление модели/закрытие панели во вложенном Qt event loop. */
#pragma once

#include <functional>
#include <QWidget>
#include <QListView>
#include <QTextEdit>
#include <QLabel>
#include <QTimer>
#include <QByteArray>
#include <QVector>
#include <QElapsedTimer>
#include <QPointer>
#include <stdint.h>

class MemberListModel;
class GroupedMemberModel;
class DbManager;
class QSortFilterProxyModel;
class QPushButton;
class QTreeWidget;

struct DetailAddress {
    int family = 0; // 4/6, не AF_INET/AF_INET6
    int protocol = 0; // битовая маска UDP/TCP/REALITY из node_addresses
    QByteArray addr; // собственные байты IP в сетевом порядке, минимум 4/16
    int port = 0; // host order
    int socket_id = 0; // ID сокета внутри узла; связь с локальным снимком только для self
    int addr_type = 0; // классификация адреса TOPO_ADDR_*
    int nat_mapping = 0, nat_inbound = 0, nat_source = 0; // независимые классификации NAT/источник диагностики
    QString ifname; // из локального снимка сокетов только для self; для удалённых узлов пусто
};

struct MemberLinkSnap {
    int direction = 0; // 0=неизвестно, 1=исходящее, 2=входящее физическое подключение
    bool hasIp = false; // IP-путь имеет адреса/порты; внешний пакетный канал — нет
    QString protocol; // подпись транспорта из снимка
    int linkState = 0, linkStatus = 0; // физическое состояние ETCP, не online в группе
    int localFamily = 0, family = 0; // 0=не задан, иначе 4/6
    QByteArray localAddr, addr; // собственные 16-байтовые адреса из wire
    uint16_t localPort = 0, port = 0; // host order
    char sockName[20]{}; // проверенная NUL-строка из codec
};

struct MemberSockSnap {
    int sockId = 0; // ID сокета ядра
    uint16_t port = 0; // host order
    int linksUp = 0, linksDown = 0; // количество физических линков
    QString ifname; // собственная UTF-8 строка интерфейса
};

class AccountList : public QWidget {
    Q_OBJECT
public:
    explicit AccountList(DbManager* db = nullptr, QWidget *parent = nullptr); // без db доступны снимки/действия, редактирование выключено
    ~AccountList();

    void setChannel(const QString& channelId, quint64 dmGeneration = 0); // CHAT или dm:ID; GROUPS generation берётся из БД либо caller.
    void refresh(); // асинхронно запросить список, показанные строки сохраняются до ответа
    void setAssignmentState(std::function<int(const QString&)> state); // GUI callback состояния выбранной карточки; обновляет оба списка.
    void refreshAssignments(); // Перерисовать кнопки после подтверждения назначения ядром.
    void setDetailsEnabled(bool enabled); // Режим карточки Kanban: только список, без выбора/опроса подробностей.
    void onDmMembers(const uint8_t* data, int len); // Полный CHAT_EVT_DM_MEMBERS; чужое/устаревшее поколение игнорируется.
    void onMemberDetailData(const uint8_t* data, int len); // целый codec-пакет, заимствован на время вызова; неверный/старый не меняет карточку

    /* GUI callbacks; данные заимствованы. Updated содержит полный CHAT_MEMBER_DISPLAY_SIZE,
     * длину проверяет вызывающий. Формат/фильтрацию участников реализует MemberListModel. */
    void onMemberList(const QString& chId, int count, const uint8_t* data, int dataLen);
    void onMemberUpdated(const QString& chId, const uint8_t* data);
    void onMemberRemoved(const QString& chId, const QByteArray& ed);

    MemberListModel* model() const { return m_model; } // заимствован до уничтожения панели

signals:
    void assignmentRequested(const QString& ed25519);
    void memberRenamed(); // приняты изменения rename/properties, список уже запрошен заново
    void callRequested(const QString& channelId, const QByteArray& ed, const QString& name, bool devices = false);
    void radioCallRequested(const QString& channelId, const QByteArray& peer, bool devices = false);
    void pmRequested(const QString& channelId, const QByteArray& ed, const QString& name, bool groups);
    void inviteRequested(const QString& channelId, const QByteArray& ed, const QString& name);
    void filesRequested(const QString& channelId, const QByteArray& ed);
    void clipboardRequested(const QString& group, const QByteArray& node, const QString& name); // Только конкретный допущенный участник выбранной группы.

private slots:
    void onMemberClicked(const QModelIndex& index);
    void onMemberDoubleClicked(const QModelIndex& index);
    void onMemberContextMenu(const QPoint& pos);
    void onDetailTimerTick();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void clearMemberDetail();
    void readDetailMetadata();
    void onSnapshotChanged(const QByteArray& ed);
    void showMemberDetail(const QByteArray& ed);
    void updateDetailDisplay();
    void requestMemberDetail();
    void editMember(const QByteArray& ed, bool properties);
    void showGroupDetail(QString group); // список участников выбранной подгруппы, без копирования снимков
    void updateGroupDetail(); // актуализировать список после изменения общего снимка
    void renameGroup(QString group); // только своя devgroup; повторная проверка после диалога
    QByteArray ownGroupNode(const QString& group) const; // Ed25519 устройства текущего пользователя
    bool canLinkMyDevice(const QByteArray& ed) const; // Чужой неудалённый узел с живым BGP-маршрутом и без devgroup.
    void confirmMyDeviceLink(const QByteArray& ed); // Объяснение, затем явный старт с повторной проверкой выбранного канала/узла.
    static QString relativeTimeStr(qint64 timestamp);
    static QString addrStr(const QByteArray& addr, int family);

    QListView       *m_listView;
    MemberListModel *m_model;
    GroupedMemberModel* m_grouped;
    QPointer<DbManager> m_db; // не владеет ни объектом, ни SQLite; доступ к handle сериализует внешний lifecycle
    QLabel          *m_header;
    QWidget         *m_detailPanel;
    bool m_detailsEnabled = true;
    QLabel          *m_detailHeader;
    QTextEdit      *m_detailText;
    QListView      *m_groupDevices;
    QSortFilterProxyModel* m_groupDevicesModel;
    QString         m_detailGroup; // hex devgroup в нижней панели; пусто при выборе отдельного устройства
    QTimer          *m_detailTimer; // child, 1 с; запускается только для выбранной видимой карточки
    QString          m_channelId; // захватывается меню/диалогом и перепроверяется после exec
    QTreeWidget* m_dmDevices; // Два раздела текущего состава GROUPS; не снимок доставки сообщения.
    QTimer* m_dmTimer; // Опрос текущего состава/online раз в 2 с только для видимой панели.
    quint64 m_dmGeneration = 0; // 0 — обычная панель или DEVICE; ненулевое привязывает запросы к истории.
    QByteArray m_dmSnapshot; // Повторный неизменившийся снимок не перестраивает видимые строки.

    QByteArray        m_detailEd;
    qint64            m_detailLastSeen = 0, m_detailCreated = 0; // Unix seconds, 0=неизвестно
    QString m_detailConnectionOrder; // политика из подписанного снимка, пусто — неизвестна
    QVector<DetailAddress> m_detailAddrs; // прочитанные/проверенные записи SQLite

    QElapsedTimer     m_detailClock;          // монотонный срок запроса
    quint64           m_detailRequest = 0;    // ожидаемый request_id; 0 = нет запроса
    qint64            m_detailRequestedAt = 0; // m_detailClock.elapsed() при постановке запроса
    bool              m_detailDirty = false;  // снимок участника изменился после отправки запроса
    QVector<QByteArray> m_detailHops;            // доп. путь; сбрасывается при следующем member-событии
    QVector<MemberLinkSnap> m_snapLinks; // копии последнего принятого ответа, не указатели ядра
    QVector<MemberSockSnap> m_snapSocks;
};
