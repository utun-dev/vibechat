/* Роли плоской модели ChannelList и её делегат; использовать только в GUI-потоке.
 * Установить через QListView::setItemDelegate(), QObject-parent владеет делегатом.
 * Строка канала/PM: аватар, состояние связи, название, одна строка последнего сообщения и unread.
 * Заголовок секции рисуется без этих индикаторов; запрет выбора задаётся flags модели.
 * Всё рисование ограничено option.rect и сохраняет исходное состояние QPainter. */
#pragma once

#include <QStyledItemDelegate>

enum ChannelDataRole {
    ChannelNameRole             = Qt::DisplayRole,   /* QString: название/имя собеседника/заголовок секции. */
    ChannelIconRole             = Qt::DecorationRole, /* QIcon: аватар, область 32×32. */
    ChannelLastMessageRole      = Qt::UserRole + 1,  /* QString: превью; первая ": " отделяет цветное имя автора. */
    ChannelChannelIdRole        = Qt::UserRole + 2,  /* QString: ID канала или "dm:<conv_id>" для PM. */
    ChannelPeersOnlineRole      = Qt::UserRole + 3,  /* int >= 0: зелёная полоса и счётчик онлайн-пиров, максимум "9+". */
    ChannelAutoConnectActiveRole = Qt::UserRole + 4, /* bool: жёлтая полоса, если нет онлайн-пиров. */
    ChannelUnreadCountRole      = Qt::UserRole + 6,  /* int >= 0: непрочитанные; 0 скрывает значок, максимум "99+". */
    ChannelLastMsgAtRole        = Qt::UserRole + 7,  /* qint64: timestamp для списка, делегат его не рисует. */
    ChannelDmOnlineRole         = Qt::UserRole + 11, /* bool: зелёная/красная точка PM по наличию живого BGP-пути. */
    ChannelDmGenerationRole     = Qt::UserRole + 12, /* quint64: поколение истории PM для списка, не рисуется. */
    ChannelClipboardIconRole    = Qt::UserRole + 13, /* QIcon: жёлтый значок полученного от участника буфера. */
    ChannelClipboardSenderRole  = Qt::UserRole + 14, /* QString: ник отправителя, под значком при достаточной ширине. */
    ChannelRadioTransmittingRole = Qt::UserRole + 16, /* bool: фактическая передача активной рации. */
    ChannelRadioActiveRole      = Qt::UserRole + 15, /* bool: локальная рация включена в этой группе. */
    ChannelIsSectionRole        = Qt::UserRole + 10  /* bool: рисовать только название секции на фоне window. */
};

class ChannelDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    /* Создаёт делегат; parent необязателен, без него освобождает вызывающий. */
    explicit ChannelDelegate(QObject *parent = nullptr);

    /* Рисует роли index в option.rect активным заимствованным painter, учитывая выбор/hover и палитру. */
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    /* Возвращает 200×56 для всех строк, включая секции; view задаёт фактическую ширину. */
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;
    static QRect radioRect(const QStyleOptionViewItem& option, const QModelIndex& index); // Общая геометрия рисунка и кнопки PTT.
    static QRect clipboardRect(const QStyleOptionViewItem& option, const QModelIndex& index); // Единая область уведомления для paint/click.
    bool editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option, const QModelIndex& index) override;
signals:
    void clipboardClicked(const QString& channel); // Нажата именно область уведомления существующей группы.
};
