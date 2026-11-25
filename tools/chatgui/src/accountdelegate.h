/* Строка участника: аватар, имя и три действия — звук рации, вызов рации, обычный звонок.
 * GUI-поток, модель принадлежит view. DisplayRole — имя, DecorationRole — аватар,
 * UserRole+1 — подпись, +2 — self, +3 — CHAT_MEMBER_FLAG_*, +4 — storage;
 * MemberListModel::OnlineRole/RadioSpeakingRole определяют доступность и индикацию.
 * Аватар доступен всем, mute — чужим участникам независимо от online, вызовы — online-пирам.
 * Нику оставляется около семи символов (56px); при сужении исчезают вызов рации, звонок, затем динамик.
 * Видимые действия стоят справа; единый расчёт используется для paint, кликов и подсказок.
 * Собственная строка показывает состояние рации без кнопок mute/вызова.
 * Под раскрытие зарезервирована общая колонка; заголовок группы получает аватар и действия
 * через GroupedMemberModel. Ветка от раскрытого заголовка заканчивается уголком у последнего устройства.
 * Трубка/рация группы видны при наличии online-устройства,
 * в том числе self; вызовы себе недоступны. Их состояния общие для группы.
 * Для выбранной Kanban-карточки callback assignmentState добавляет справа кнопку назначения конкретного устройства.
 * Сигнал только при LeftButton release в том же действии/строке после press:
 * MouseMove со смещением >=5 по Manhattan или без LeftButton отменяет жест, включая возврат курсора назад. */
#pragma once

#include <QStyledItemDelegate>
#include <QPersistentModelIndex>
#include <functional>

class AccountDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit AccountDelegate(QObject *parent = nullptr); // parent владеет по правилам QObject

    std::function<int(const QString&)> assignmentState; // Заимствованный GUI callback: -1 скрыть, 0 назначить, 1 снять.
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, // 160 × MemberListModel::kRowHeight, независимо от данных
                   const QModelIndex &index) const override;
    bool editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option,
                     const QModelIndex& index) override;
    bool helpEvent(QHelpEvent* event, QAbstractItemView* view, const QStyleOptionViewItem& option,
                   const QModelIndex& index) override;
signals:
    // index заимствует модель; для отложенной работы получатель сохраняет node ID, не QModelIndex через reset.
    void radioCallRequested(const QModelIndex& index);
    void callRequested(const QModelIndex& index);
    void avatarRequested(const QModelIndex& index);
    void radioMuteRequested(const QModelIndex& index);
    void assignmentRequested(const QString& ed25519);
    void groupToggleRequested(const QModelIndex& index);
private:
    static constexpr int ClickDistance = 5;
    enum class Action { None, Avatar, RadioMute, RadioCall, Call, ToggleGroup, Assign };
    int assignment(const QModelIndex& index) const; // Заголовки devgroup не являются отдельным исполнителем.
    int visibleActionCount(const QStyleOptionViewItem& option, const QModelIndex& index) const; // 0..3, учитывает значки ролей перед ником.
    static QRect actionRect(const QRect& row, Action action);
    static Action actionAt(const QPoint& position, const QRect& row, int actionCount, bool assignment = false); // Скрытые действия не участвуют в hit test.
    QPersistentModelIndex m_pressedIndex; /* строка, на которой начался клик */
    QPoint m_pressedPosition; // координаты viewport при press
    QByteArray m_pressedPeer; // смена online-устройства между press/release отменяет действие
    Action m_pressedAction = Action::None; // ожидаемое действие; сбрасывается при release/отмене жеста
};
