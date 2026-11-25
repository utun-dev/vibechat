#include "emojipanel.h"
#include "emoji.h"
#include "emojitabbar.h"
#include "animtimer.h"
#include "lottieicon.h"
#include "debug_ui.h"
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QPainter>
#include <QStyle>
#include <QTimer>
#include <QEvent>
#include <algorithm>

namespace {

constexpr int EmojiColumns = 9;
constexpr int EmojiButtonSize = 32;
constexpr int EmojiSize = 27;
constexpr int AnimatedEmojiSize = (EmojiSize * 3 + 1) / 2;
constexpr int AnimatedEmojiButtonSize = AnimatedEmojiSize + EmojiButtonSize - EmojiSize;
constexpr int EmojiSpacing = 1;
constexpr int EmojiMargin = 4;
constexpr int EmojiGridWidth = EmojiColumns * EmojiButtonSize + (EmojiColumns - 1) * EmojiSpacing + 2 * EmojiMargin;

class EmojiTabWidget : public QTabWidget {
public:
    using QTabWidget::setTabBar;
};

class EmojiButton : public QPushButton {
public:
    EmojiButton(const EmojiData &data, QWidget *parent = nullptr)
        : QPushButton(data.unicode, parent)
        , m_data(data)
    {
        setFixedSize(AnimatedEmojiButtonSize, AnimatedEmojiButtonSize);
        setProperty("animatedEmoji", true);
        setCursor(Qt::PointingHandCursor);
        if (data.type == EmojiType::Animation) {
            setMouseTracking(true);
            m_icon = AnimTimer::instance()->icon(data.animPath);
            m_timer.setInterval(33);
            connect(&m_timer, &QTimer::timeout, this, [this]() {
                if (m_icon && m_icon->isValid()) {
                    m_frame = (m_frame + 1) % m_icon->frameCount();
                    if (m_playback.elapsed() >= AnimTimer::PlaybackMs && m_frame == 0) {
                        GUI_DEBUG("EmojiButton: playback finished path=%s elapsed_ms=%lld frame=0",
                                  qPrintable(m_data.animPath), (long long)m_playback.elapsed());
                        m_timer.stop();
                        m_playback.invalidate();
                    }
                    update();
                }
            });
        }
    }
    const QString &unicode() const { return m_data.unicode; }

protected:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    void enterEvent(QEnterEvent *e) override {
#else
    void enterEvent(QEvent *e) override {
#endif
        QPushButton::enterEvent(e);
        if (m_icon && m_icon->isValid()) {
            GUI_DEBUG("EmojiButton: hover trigger path=%s running=%d frame=%d playback_ms=%d",
                      qPrintable(m_data.animPath), m_timer.isActive(), m_frame, AnimTimer::PlaybackMs);
            if (!m_timer.isActive()) m_frame = 0;
            m_playback.start();
            m_timer.start();
            update();
        }
    }
    void paintEvent(QPaintEvent *e) override {
        if (m_timer.isActive() && m_icon && m_icon->isValid()) {
            QPainter p(this);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            const int sz = AnimatedEmojiSize;
            QImage frame = m_icon->renderFrame(m_frame, QSize(sz, sz));
            int x = (width() - sz) / 2, y = (height() - sz) / 2;
            p.drawImage(x, y, frame);
        } else {
            QPushButton::paintEvent(e);
        }
    }

private:
    EmojiData m_data;
    LottieIcon *m_icon = nullptr;
    QTimer m_timer;
    int m_frame = 0;
    QElapsedTimer m_playback;
};

} // namespace

EmojiPanel::EmojiPanel(QWidget *parent)
    : QWidget(parent)
{
    setStyleSheet(QString("EmojiPanel { background: palette(window); }"
                         "QPushButton { border: none; border-radius: 4px; padding: 0; font-size: %1px; background: transparent; }"
                         "QPushButton[animatedEmoji=\"true\"] { font-size: %2px; }"
                         "QPushButton:hover { background: rgba(0,0,0,0.08); }").arg(EmojiSize).arg(AnimatedEmojiSize));
    setFixedWidth(EmojiGridWidth + style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this));
    buildPanel(builtinEmojiSet());
    if (parent) parent->installEventFilter(this);
    hide();
}

void EmojiPanel::showEvent(QShowEvent *event) {
    updateOverlayGeometry();
    raise();
    QWidget::showEvent(event);
    GUI_DEBUG("EmojiPanel: shown size=%dx%d grid_width=%d emoji_px=%d animation_px=%d gap_px=%d tabs=bottom",
              width(), height(), EmojiGridWidth, EmojiSize, AnimatedEmojiSize, EmojiSpacing);
}

bool EmojiPanel::eventFilter(QObject *object, QEvent *event) {
    if (object == parentWidget() && event->type() == QEvent::Resize && isVisible()) updateOverlayGeometry();
    return QWidget::eventFilter(object, event);
}

/* Панель занимает только свой прямоугольник поверх viewport, не отнимая место у истории. */
void EmojiPanel::updateOverlayGeometry() {
    auto *scroll = qobject_cast<QScrollArea *>(m_tabs->currentWidget());
    if (!scroll) { GUI_WARN("EmojiPanel: cannot size panel, category has no scroll page index=%d", m_tabs->currentIndex()); return; }
    const int contentHeight = scroll->widget()->layout()->sizeHint().height();
    const int desiredHeight = contentHeight + 2 * scroll->frameWidth() + m_tabs->tabBar()->sizeHint().height();
    auto *parent = parentWidget();
    const int availableHeight = parent ? qMin(320, parent->height() / 2) : desiredHeight;
    setFixedHeight(qMin(desiredHeight, availableHeight));
    if (parent) move(qMax(0, parent->width() - width()), parent->height() - height());
    GUI_DEBUG("EmojiPanel: geometry category=%d content_height=%d desired_height=%d available_height=%d rect=%d,%d %dx%d",
              m_tabs->currentIndex(), contentHeight, desiredHeight, availableHeight, x(), y(), width(), height());
}

void EmojiPanel::buildPanel(const QVector<EmojiCategory> &categories) {
    auto *tabs = new EmojiTabWidget();
    m_tabs = tabs;
    tabs->setDocumentMode(true);
    tabs->setTabBar(new EmojiTabBar(tabs));
    tabs->setTabPosition(QTabWidget::South);
    tabs->setStyleSheet("QTabWidget::pane { border: none; background: palette(base); }"
                       "QTabWidget::tab-bar { alignment: right; }");

    for (const auto &cat : categories) {
        const int index = tabs->addTab(buildCategoryPage(cat), cat.items.isEmpty() ? cat.name : cat.items.first().unicode);
        tabs->setTabToolTip(index, cat.name);
        tabs->tabBar()->setAccessibleTabName(index, cat.name);
    }
    connect(tabs, &QTabWidget::currentChanged, this, [this, tabs](int index) {
        GUI_DEBUG("EmojiPanel: category index=%d name=%s", index, qPrintable(tabs->tabToolTip(index)));
        updateOverlayGeometry();
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(tabs);
}

QWidget *EmojiPanel::buildCategoryPage(const EmojiCategory &cat) {
    auto *scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea { border: none; }");

    auto *container = new QWidget();
    container->setFixedWidth(EmojiGridWidth);
    auto *grid = new QGridLayout(container);
    grid->setContentsMargins(EmojiMargin, EmojiMargin, EmojiMargin, EmojiMargin);
    grid->setSpacing(EmojiSpacing);
    grid->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    const bool animated = std::any_of(cat.items.cbegin(), cat.items.cend(), [](const EmojiData &e) { return e.type == EmojiType::Animation; });
    const int buttonSize = animated ? AnimatedEmojiButtonSize : EmojiButtonSize;
    const int columns = (EmojiGridWidth - 2 * EmojiMargin + EmojiSpacing) / (buttonSize + EmojiSpacing);
    for (int i = 0; i < cat.items.size(); ++i) {
        const auto &e = cat.items[i];
        QPushButton *btn;
        if (e.type == EmojiType::Animation)
            btn = new EmojiButton(e, container);
        else {
            btn = new QPushButton(e.unicode, container);
            btn->setFixedSize(EmojiButtonSize, EmojiButtonSize);
            btn->setCursor(Qt::PointingHandCursor);
        }
        btn->setToolTip(e.shortcode);
        connect(btn, &QPushButton::clicked, this, &EmojiPanel::onEmojiClicked);
        grid->addWidget(btn, i / columns, i % columns);
    }
    grid->setRowStretch(grid->rowCount(), 1);

    scroll->setWidget(container);
    GUI_DEBUG("EmojiPanel: category built name=%s items=%d columns=%d button_px=%d emoji_px=%d",
              qPrintable(cat.name), int(cat.items.size()), columns, buttonSize, animated ? AnimatedEmojiSize : EmojiSize);
    return scroll;
}

void EmojiPanel::onEmojiClicked() {
    auto *btn = qobject_cast<QPushButton *>(sender());
    if (!btn) { GUI_WARN("EmojiPanel: click ignored, sender is not a button"); return; }
    auto *eb = dynamic_cast<EmojiButton *>(btn);
    const QString unicode = eb ? eb->unicode() : btn->text();
    GUI_DEBUG("EmojiPanel: selected shortcode=%s utf8=%s", qPrintable(btn->toolTip()), unicode.toUtf8().toHex().constData());
    emit emojiSelected(unicode);
}
