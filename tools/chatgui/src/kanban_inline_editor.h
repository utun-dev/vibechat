/* Текст внутри пузыря, палитра под ним; общий внешний пунктир охватывает обе области.
 * finished вызывается при уходе фокуса за весь редактор; Escape отменяет черновик. */
#pragma once
#include "kanban_paint.h"
#include <QApplication>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QPushButton>
#include <QButtonGroup>
#include <QLabel>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QTimer>
#include <QPointer>
#include <QJsonArray>
#include <QJsonObject>
#include <functional>
extern "C" {
#include "debug_config.h"
}

class KanbanInlineEditor final : public QWidget {
public:
    std::function<void()> finished, cancelled;
    std::function<void(int)> heightChanged;
    explicit KanbanInlineEditor(const QJsonObject& card, const QJsonArray& colors, QWidget* parent)
        : QWidget(parent), m_bubble(new QWidget(this)), m_text(new QTextEdit(m_bubble)), m_colors(new QButtonGroup(this)), m_palette(new QWidget(this)),
          m_grid(new QGridLayout(m_palette)), m_error(new QLabel(this)), m_color(card["color"].toString()) {
        setObjectName("kanbanInlineEditor"); setFocusPolicy(Qt::StrongFocus); setFocusProxy(m_text);
        auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0); layout->setAlignment(Qt::AlignTop);
        m_bubble->setObjectName("kanbanEditBubble"); m_bubble->setMinimumHeight(56);
        auto* bubbleLayout = new QVBoxLayout(m_bubble); bubbleLayout->setContentsMargins(16, 13, 16, 13); bubbleLayout->setSpacing(6);
        layout->addWidget(m_bubble);
        if (card["conflict"].toBool()) {
            auto* label = new QLabel(trc("Resolve text conflict"), m_bubble); bubbleLayout->addWidget(label);
            for (const auto& value : card["variants"].toArray()) {
                auto* variant = new QTextEdit(m_bubble); variant->setReadOnly(true); variant->setPlainText(value.toObject()["text"].toString());
                variant->setFixedHeight(80); bubbleLayout->addWidget(variant);
            }
        }
        m_text->setObjectName("kanbanInlineText"); m_text->setAcceptRichText(false); m_text->setFrameShape(QFrame::NoFrame);
        m_text->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); m_text->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_text->document()->setDocumentMargin(0); m_text->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        m_text->setStyleSheet("QTextEdit{background:transparent;border:none;padding:0;}"); m_text->setPlainText(card["text"].toString());
        bubbleLayout->addWidget(m_text); m_palette->setObjectName("kanbanPalette");
        m_grid->setContentsMargins(16, 4, 16, 6); m_grid->setSpacing(3); m_colors->setExclusive(true);
        int i = 0;
        for (const auto& value : colors) {
            QString color = value.toString(); auto* button = new QPushButton(m_palette);
            button->setObjectName(QStringLiteral("kanbanColor_%1").arg(i)); button->setProperty("color", color);
            button->setFixedSize(16, 16); button->setCheckable(true); button->setFocusPolicy(Qt::TabFocus);
            button->setAccessibleName(trc("Card border color") + " " + color); button->setToolTip(button->accessibleName());
            button->setStyleSheet(QStringLiteral("QPushButton{background:%1;border:1px solid palette(mid);border-radius:1px;}"
                "QPushButton:checked{border:3px solid palette(text);}").arg(color));
            if (m_color.isEmpty()) m_color = color;
            button->setChecked(color == m_color); m_colors->addButton(button, i++); m_buttons.append(button);
            connect(button, &QPushButton::clicked, this, [this, color] {
                m_color = color; update();
                DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "inline card color selected color=%s background_tint=10%%", qPrintable(color));
            });
        }
        layout->addWidget(m_palette); m_error->setObjectName("kanbanEditError"); m_error->setWordWrap(true);
        m_error->setContentsMargins(16, 0, 16, 6); m_error->hide(); layout->addWidget(m_error);
        connect(m_text->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, [this] { fit(); });
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget* old, QWidget* now) {
            DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "inline focus old=%s/%s new=%s/%s inside=%d",
                old ? old->metaObject()->className() : "none", old ? qPrintable(old->objectName()) : "",
                now ? now->metaObject()->className() : "none", now ? qPrintable(now->objectName()) : "", contains(now));
            if (contains(now)) m_started = true;
            if (contains(old) && !contains(now)) QTimer::singleShot(0, this, [this] { if (!contains(QApplication::focusWidget())) finish(); });
        });
        qApp->installEventFilter(this);
    }
    ~KanbanInlineEditor() override { qApp->removeEventFilter(this); disconnect(qApp, nullptr, this, nullptr); }
    QString text() const { return m_text->toPlainText(); }
    QString color() const { return m_color; }
    QTextEdit* textEdit() const { return m_text; }
    void focusAt(QPoint point) {
        m_started = true; m_finishing = false; layout()->activate();
        m_text->setFocus(Qt::MouseFocusReason); m_text->setTextCursor(m_text->cursorForPosition(m_text->viewport()->mapFrom(this, point)));
    }
    void restore(const QString& text, const QString& color) {
        m_text->setPlainText(text); m_color = color;
        for (auto* button : m_buttons) button->setChecked(button->property("color").toString() == color);
        fit(); update();
    }
    void setBusy(bool busy) {
        m_busy = busy; m_finishing = busy; m_text->setReadOnly(busy); m_palette->setEnabled(!busy);
        m_error->setText(busy ? trc("Saving…") : QString()); m_error->setVisible(busy); fit();
    }
    void showError(const QString& text) { setBusy(false); m_error->setText(text); m_error->show(); fit(); }
    void finish() {
        if (!m_started || m_busy || m_finishing) return;
        m_finishing = true; auto callback = finished; if (callback) callback();
    }
protected:
    void resizeEvent(QResizeEvent* event) override { QWidget::resizeEvent(event); fit(); }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        auto bubble = kanbanBubble(m_bubble->geometry()); QColor color(m_color);
        painter.setBrush(kanbanFill(palette(), color)); painter.setPen(QPen(color.isValid() ? color : palette().mid().color(), 2));
        painter.drawPath(kanbanRounded(bubble));
        kanbanDrawDash(painter, kanbanSelection(kanbanBubble(rect())), palette().highlight().color());
    }
    void mousePressEvent(QMouseEvent* event) override { focusAt(event->pos()); event->accept(); }
    bool eventFilter(QObject* watched, QEvent* event) override {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (event->type() == QEvent::KeyPress && contains(widget) && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape && !m_busy) {
            m_finishing = true; auto callback = cancelled; if (callback) callback(); return true;
        }
        // Оконная доставка предшествует доставке QWidget: только QWidget определяет, был ли клик снаружи.
        if (event->type() == QEvent::MouseButtonPress && widget && !contains(widget) && contains(QApplication::focusWidget())) {
            DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "inline outside press target=%s name=%s", watched->metaObject()->className(),
                qPrintable(watched->objectName()));
            QTimer::singleShot(0, this, [this] { finish(); });
        }
        return QWidget::eventFilter(watched, event);
    }
private:
    static QString trc(const char* text) { return QCoreApplication::translate("ChatGui", text); }
    bool contains(QWidget* widget) const { return widget && (widget == this || isAncestorOf(widget)); }
    void fit() {
        if (m_sizing) return; m_sizing = true;
        int columns = width() - 32 >= 225 ? 12 : 6;
        if (columns != m_columns) {
            if (m_columns) m_grid->setColumnStretch(m_columns, 0);
            m_columns = columns;
            for (int i = 0; i < m_buttons.size(); i++) m_grid->addWidget(m_buttons[i], i / columns, i % columns, Qt::AlignLeft);
            m_grid->setColumnStretch(columns, 1);
        }
        int height = qMax(fontMetrics().lineSpacing(), qCeil(m_text->document()->size().height()));
        m_text->setFixedHeight(height); m_bubble->layout()->activate(); layout()->activate();
        int needed = qMax(64, layout()->sizeHint().height());
        if (heightChanged) heightChanged(needed);
        m_sizing = false;
    }
    QWidget* m_bubble; // Область пузыря без палитры; геометрия нужна для рисования контура.
    QTextEdit* m_text;
    QButtonGroup* m_colors;
    QWidget* m_palette;
    QGridLayout* m_grid;
    QLabel* m_error;
    QList<QPushButton*> m_buttons;
    QString m_color;
    int m_columns = 0;
    bool m_started = false, m_busy = false, m_finishing = false, m_sizing = false;
};
