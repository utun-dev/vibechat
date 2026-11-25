/* GUI-вид плоской истории: фон, собственное выделение/clipboard, ссылки и прокрутка; данные и delegate задаёт владелец.
 * Модель предоставляет Msg*Role из messagedelegate.h. MessageDelegate рисует текст и вычисляет UTF-16 границы выделения.
 * Для ссылок установить hit-test, для клавиш composer/окна — установить этот объект их eventFilter.
 * Сигналы анимации/последнего окна только уведомляют владельца: сам вид не запускает таймеры emoji и не загружает историю. */
#pragma once

#include <QListView>
#include <QPersistentModelIndex>
#include <QPoint>
#include <QPushButton>
#include <QSet>
#include <functional>

struct ChatTextSelection {
    QPersistentModelIndex index; // Строка начала drag, для которой delegate вычисляет диапазон текста.
    QPoint clickPos; // Точка press относительно visualRect(index).topLeft().
    QPoint dragPos; // Текущая/последняя точка drag в той же системе координат.
    QString source; // Исходный текст при начале drag; изменение сообщения делает прежнее выделение непригодным для цитаты.
    int startChar = -1, endChar = -1; // UTF-16 диапазон; вычисляется при чтении состояния, независимо от paint.
    bool active = false; // Есть drag-выделение, в том числе после release; не равно isSelecting().
};

/* Контракт геометрии выделения; самостоятельный ChatView не зависит от реализации MessageDelegate. */
class ChatSelectionDelegate {
public:
    virtual ~ChatSelectionDelegate() = default;
    virtual QPair<int, int> selectionRange(const QStyleOptionViewItem& option, const QModelIndex& index,
                                          const QPoint& first, const QPoint& last) const = 0;
};

class ChatView : public QListView {
    Q_OBJECT
public:
    /* parent владеет виджетом; создаёт свой вертикальный scrollbar и кнопку вниз, но не модель/MessageDelegate. */
    explicit ChatView(QWidget *parent = nullptr);

    /* Загрузить QPixmap из файла/ресурса, затемнить чёрным с alpha=64/255 и запросить repaint мозаичного фона.
     * Ошибка загрузки даёт WARN и возврат без запроса repaint. */
    void setBackgroundImage(const QString &path);
    void setHistoricalWindow(bool enabled); // Старое окно истории: кнопка вниз видима и на нижней позиции; данные не меняются.

    bool hasSelection() const { return m_selStart.isValid(); } // Проверяет собственное выделение, не QItemSelectionModel.
    QModelIndex selStart() const { return QModelIndex(m_selStart); } // Строка начала drag; может быть ниже selEnd.
    QModelIndex selEnd()   const { return QModelIndex(m_selEnd); } // Последняя строка drag; индексы могут стать invalid у модели.
    bool isSelecting()     const { return m_selecting; } // Кнопка ещё удерживается и drag уже преодолел порог 5 px.
    const ChatTextSelection &textSelection() const; // Актуальная геометрия; заимствована до следующего изменения вида.
    QString selectedText(const QModelIndex& index) const; // Только непустой фрагмент одного неизменённого сообщения; иначе пусто.

    bool isRowSelected(const QModelIndex &idx) const; // Включительный диапазон номеров строк; idx ожидается из той же модели.
    /* Сохранить hit-test ссылки: fn(index, viewport point) → строка ссылки либо пусто. Пустой fn отключает ссылки.
     * Callback вызывается в GUI при hover/release; hover может передать invalid index.
     * Вызывающий обеспечивает lifetime захваченных объектов на время их использования callback. */
    void setLinkHitTest(std::function<QString(const QModelIndex&, const QPoint&)> fn) { m_linkHitTest = std::move(fn); }
    // Кнопка Copy под viewport point для hover-курсора; callback живёт по тем же правилам, что hit-test ссылки.
    void setCodeCopyHitTest(std::function<bool(const QModelIndex&, const QPoint&)> fn) { m_codeCopyHitTest = std::move(fn); }

signals:
    void animationTriggered(); // Новое видимое сочетание сообщение/путь анимации или вход курсора в строку с анимацией.
    void linkActivated(const QString& link); // Левый click без drag: press/release на одном index, ссылка получена hit-test.
    void latestRequested(); // End без модификаторов либо кнопка вниз, до локального scrollToBottom/перемещения scrollbar.

protected:
    /* Up/Down/PgUp/PgDown/End без модификаторов от внешнего объекта; обрабатывает только visible/enabled вид. */
    bool eventFilter(QObject *object, QEvent *event) override;
    bool viewportEvent(QEvent *event) override; // Leave снимает hover-index, чтобы новый вход снова запустил анимацию.
    void paintEvent(QPaintEvent *event) override; // Фон/обычное рисование, затем снимок видимых анимаций по ролям текста/реакций.
    void resizeEvent(QResizeEvent *event) override; // Планирует layout строк и обновляет кнопку вниз.
    void scrollContentsBy(int dx, int dy) override; // Базовая прокрутка и repaint viewport для tiled-фона/анимаций.
    void mousePressEvent(QMouseEvent *event) override; // Левый press сбрасывает прежнее выделение и запоминает строку/точку.
    void mouseMoveEvent(QMouseEvent *event) override; // Hover/cursor ссылки либо drag: диапазон строк и точки текстового выделения.
    void mouseReleaseEvent(QMouseEvent *event) override; // Ссылка при click, иначе завершение drag с сохранением выделения.
    void keyPressEvent(QKeyEvent *event) override; // Клавиши прокрутки, Copy собственного выделения, затем базовый QListView.

private:
    /* Прокрутить историю по клавише без модификаторов; true, если событие принято. Фокус сохраняется. */
    bool scrollWithKey(QKeyEvent *event);
    /* Одна строка: выбранный UTF-16 фрагмент либо весь видимый текст; несколько — [time] author: text по возрастанию row.
     * Пишет только QClipboard::Clipboard, без selection clipboard; без собственного выделения ничего не меняет. */
    void copySelection();
    void updateScrollDownBtn(); // Видимость по historical/atBottom; выше низа позиционирует кнопку справа внизу viewport.

    QPixmap m_background; // Собственные пиксели уже с затемнением, рисуются мозаикой без масштабирования.
    QPersistentModelIndex m_selStart; // Начало собственного выделения, Qt отслеживает изменения индекса модели.
    QPersistentModelIndex m_selEnd; // Последняя строка диапазона, направление drag сохраняется.
    mutable ChatTextSelection m_textSel; // Точки и вычисляемый при чтении диапазон; active сохраняется после release.
    QPersistentModelIndex m_lastHovered; // Для однократного animationTriggered при входе в другую строку.
    QSet<QString> m_visibleAnimations; // Ключ channel/id/outgoing/path на последнем paint; удаление из видимости само не даёт сигнал.
    QPersistentModelIndex m_pressIndex; // Кандидат click/начала drag, снимается при release.
    QPoint m_pressPos; // Координата события press для порога drag и проверки click.
    bool m_selecting = false; // Идёт drag собственного выделения, не состояние стандартной модели выбора.
    bool m_historicalWindow = false; // Выбран исторический снимок, владелец должен обработать latestRequested.
    std::function<QString(const QModelIndex&, const QPoint&)> m_linkHitTest; // Собственная копия callback и его захваченных значений.
    std::function<bool(const QModelIndex&, const QPoint&)> m_codeCopyHitTest; // Не связывает самостоятельный вид с реализацией delegate.
    QPushButton *m_scrollDownBtn = nullptr; // QObject-child вида: latestRequested и затем локальная прокрутка вниз.
};
