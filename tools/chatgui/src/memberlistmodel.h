/* Плоская Qt-модель участников одного канала, только GUI-поток с QApplication.
 * Хранит полные снимки CHAT_EVT_MEMBER_*, без чтения живых структур ядра/БД.
 * Последовательный массив совпадает с порядком строк; member(ed) возвращает независимую копию.
 * Полный снимок проверяется/готовится до reset, обновление одной записи заменяет её целиком.
 * Без isOwner удалённые записи скрыты, смена канала очищает снимки и эфемерную речь.
 * Запросы идут через owned bridge API: остановка ядра отменяет их и освобождает аргументы. */
#ifndef MEMBERLISTMODEL_H
#define MEMBERLISTMODEL_H

#include <QAbstractListModel>
#include <QVector>
#include <QSet>
#include <QString>
#include <QPixmap>
#include <QTimer>
#include <stdint.h>
#include <optional>
#include "../../../src/chat/chat_member.h"

struct MemberListItem {
    chat_member_display snapshot{}; // полная запись выбранного канала; источник всех live-полей
    QPixmap avatar;                 // производное изображение строки
    QString name;                   // UTF-8 имя из snapshot для Qt
};

class MemberListModel : public QAbstractListModel {
    Q_OBJECT
public:
    /* Высота строки 36 px, между аватарками остаётся 2 px. */
    static constexpr int kRowHeight = 36;
    static constexpr int kAvatarSize = kRowHeight - 2;
    static constexpr int Ed25519Role = Qt::UserRole + 17; // полный адрес для подписанных команд
    static constexpr int ProtocolWarningRole = Qt::UserRole + 16; // текст несовместимости либо пусто
    static constexpr int RadioSpeakingRole = Qt::UserRole + 5;
    static constexpr int OnlineRole = Qt::UserRole + 6; /* живой статус BGP для кнопок вызова */
    static constexpr int DevgroupRole = Qt::UserRole + 12; // ключ devgroup в hex, пусто без группы
    static constexpr int AvatarEdRole = Qt::UserRole + 13; // владелец показываемого аватара
    static constexpr int ActiveDevgroupRole = Qt::UserRole + 14; // ключ devgroup только неудалённых устройств для фильтра деталей

    explicit MemberListModel(QObject* parent = nullptr); // parent владеет; собственный timer опрашивает RTT каждые 2 с

    int  rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    /* channelId — 64 hex-символа публичного Ed25519 группы, пустой очищает модель. Смена owner-политики
     * скрывает удалённых сразу; полный refresh асинхронный, при недоступном ядре сохраняет снимок. */
    void setChannel(const QString& channelId, bool isOwner);
    void refresh(); // запрос полного списка, без очистки показанных строк
    void refreshRtt(); // запрос полных MEMBER_UPDATED, включая все live-поля, не только RTT
    QByteArray memberKeyAt(int row) const; // пусто вне диапазона
    bool isOnlineAt(int row) const; // false вне диапазона; online из последнего снимка, не из БД
    /* Копия записи: указатели в row-cache нельзя сохранять через reset/обновления. */
    std::optional<chat_member_display> member(const QByteArray& ed) const;
    /* Единый формат фактического подключения для строки и деталей. Bn/BGP(n hop) считает посредников.
     * detailed добавляет пару NAT «наш узел — участник» для DIR; неизвестный тип обозначен ?, RTT не округляется до целых. */
    QString routeText(const chat_member_display& member, bool detailed) const;

    /* GUI callbacks, data заимствован на время вызова. Другой chId игнорируется.
     * List требует ровно count*CHAT_MEMBER_DISPLAY_SIZE байтов; (0,NULL,0) очищает список.
     * Updated требует доступный полный CHAT_MEMBER_DISPLAY_SIZE: длину проверяет caller/bridge codec.
     * NULL/ошибка списка сохраняют прежнюю модель; неизвестный nodeId добавляется в конец.
     * Removed сдвигает следующие строки с корректными Qt-уведомлениями; отсутствие допустимо. */
    void onMemberList(const QString& chId, int count, const uint8_t* data, int dataLen);
    void onMemberUpdated(const QString& chId, const uint8_t* data);
    void onMemberRemoved(const QString& chId, const QByteArray& ed);

    /* Эфемерное состояние рации; вызывается в GUI-потоке, независимо от обновлений строк. */
    void setRadioState(const QString& groupId, bool active, bool transmitting);
    void onRadioTalk(const QString& groupId, const QByteArray& peer, bool on);

signals:
    /* Пустой ключ — полный снимок/смена канала; иначе изменившийся или удалённый участник. */
    void snapshotChanged(const QByteArray& ed);

private:

    QString m_channelId; // выбранный канал, публикуется до modelReset/snapshotChanged
    bool m_isOwner = false; // разрешает показывать удалённых участников
    QVector<MemberListItem> m_members; // собственные снимки, индекс массива == номер строки
    QSet<QByteArray> m_radioTalkers; // START может прийти до загрузки строки; STOP/stop radio/смена канала очищают
    bool m_radioActive = false; // приём START только для активной рации выбранного канала
    bool m_selfTransmitting = false; // PTT/VAD из аудио-движка, не состояние удалённых источников
    QTimer* m_rttTimer; // QObject-child, останавливается при уничтожении модели
};

/* Представление поверх снимков: заголовки не являются устройствами; аватар берётся у первого
 * участника с опубликованной аватаркой, иначе у первого участника группы, кнопки действуют на первое online-устройство. RADIO/PHONE-флаги заголовка
 * объединяют состояния всех неудалённых участников; mute/self относятся к цели кнопок.
 * Self/его группа всегда первые, в раскрытой группе self тоже первый; порядок остальных сохраняется.
 * Удалённые вынесены в последнюю свёрнутую группу без аватара и действий; их devgroup не учитывается. Источник заимствован. */
class GroupedMemberModel : public QAbstractListModel {
    Q_OBJECT
public:
    static constexpr int GroupHeaderRole = Qt::UserRole + 7;
    static constexpr int GroupExpandedRole = Qt::UserRole + 8;
    static constexpr int ActionPeerRole = Qt::UserRole + 18; // Ed25519 устройства для кнопок; пусто при отсутствии online
    static constexpr int GroupBranchRole = Qt::UserRole + 10; // 0 — без ветки; 1 — продолжение; 2 — последнее устройство
    static constexpr int CallAvailableRole = Qt::UserRole + 11; // Есть доступный адресат кроме локального узла.
    static constexpr int DeletedGroupRole = Qt::UserRole + 15; // служебный заголовок «Удалённые», без цели действий
    static constexpr int DisclosureWidth = 24; // одинаковая левая колонка во всех строках
    explicit GroupedMemberModel(MemberListModel* source, QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    int sourceRow(int row) const; // -1 для заголовка/неверной строки
    int actionSourceRow(int row) const; // заголовок devgroup: первое online-устройство; «Удалённые»/нет цели — -1
    int avatarSourceRow(int row) const; // devgroup: первое устройство с аватаркой независимо от online, иначе первое; «Удалённые»/неверная строка — -1
    bool toggle(int row); // true — обработан заголовок
private:
    void rebuild(bool forceReset = false);
    MemberListModel* m_source;
    QVector<int> m_rows; // >=0 — строка источника, <0 — заголовок группы строки -value-1
    QSet<QByteArray> m_expanded;
};
#endif
