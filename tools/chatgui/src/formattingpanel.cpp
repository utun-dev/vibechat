#include "formattingpanel.h"
#include "debug_ui.h"
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QShowEvent>
#include <QVBoxLayout>

namespace {
constexpr int FormattingPanelWidth = 440;
}

FormattingPanel::FormattingPanel(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("messageFormattingPanel"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral(
        "FormattingPanel { background: palette(window); }"
        "FormattingPanel QLabel { background: transparent; border: none; }"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 0, 12, 0);
    layout->setSpacing(0);

    auto *title = new QLabel(QCoreApplication::translate("ChatGui", "Message formatting"), this);
    title->setObjectName(QStringLiteral("messageFormattingHelpTitle"));
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    title->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    const int lineHeight = title->fontMetrics().lineSpacing();
    layout->setContentsMargins(12, lineHeight, 12, 0);
    layout->addWidget(title, 0, Qt::AlignTop);
    layout->addSpacing(lineHeight);

    auto addText = [this, layout](const QString& value, const QString& objectName) {
        auto *label = new QLabel(value, this);
        label->setObjectName(objectName);
        label->setTextFormat(Qt::PlainText);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setWordWrap(true);
        layout->addWidget(label);
        return label;
    };

    const QString samples[] = {QCoreApplication::translate("ChatGui", "bold"),
        QCoreApplication::translate("ChatGui", "italic"), QCoreApplication::translate("ChatGui", "strikethrough")};
    const QString markers[] = {QStringLiteral("**%1**"), QStringLiteral("__%1__"), QStringLiteral("~~%1~~")};
    const char* names[] = {"Bold", "Italic", "Strike"};
    const QFont monospace(QStringLiteral("monospace"));
    for (int i = 0; i < 3; ++i) {
        auto *row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        auto *source = new QLabel(markers[i].arg(samples[i]), this);
        source->setObjectName(QStringLiteral("formatting%1Source").arg(QLatin1String(names[i])));
        source->setFont(monospace);
        source->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto *arrow = new QLabel(QString::fromUtf8("→"), this);
        auto *example = new QLabel(samples[i], this);
        example->setObjectName(QStringLiteral("formatting%1Example").arg(QLatin1String(names[i])));
        QFont exampleFont = example->font();
        if (i == 0) exampleFont.setBold(true);
        else if (i == 1) exampleFont.setItalic(true);
        else exampleFont.setStrikeOut(true);
        example->setFont(exampleFont);
        example->setTextInteractionFlags(Qt::TextSelectableByMouse);
        row->addWidget(source); row->addWidget(arrow); row->addWidget(example); row->addStretch(1);
        layout->addLayout(row);
    }
    addText(QCoreApplication::translate("ChatGui", "Use paired tags on one line, without spaces next to the text."),
            QStringLiteral("formattingInlineHint"));

    layout->addSpacing(lineHeight / 2);
    auto *codeTitle = addText(QCoreApplication::translate("ChatGui", "Code blocks"), QStringLiteral("formattingCodeTitle"));
    QFont codeTitleFont = codeTitle->font(); codeTitleFont.setBold(true); codeTitle->setFont(codeTitleFont);
    addText(QCoreApplication::translate("ChatGui", "Put ``` on separate lines, as shown:"), QStringLiteral("formattingCodeHint"));
    auto *code = addText(QStringLiteral("```cpp\nint value = 42;\n```"), QStringLiteral("formattingCodeExample"));
    code->setFont(monospace);
    code->setStyleSheet(QStringLiteral("background: palette(light); border-radius: 4px; padding: 6px;"));
    addText(QCoreApplication::translate("ChatGui",
        "Spaces around ``` and the language are allowed.\n"
        "The language after ``` is optional: the chat will try to detect it.\n"
        "Use ```text to disable highlighting."), QStringLiteral("formattingHelpDetails"));
    layout->addStretch(1);

    if (parent) parent->installEventFilter(this);
    hide();
}

void FormattingPanel::showEvent(QShowEvent *event) {
    updateOverlayGeometry();
    raise();
    QWidget::showEvent(event);
    GUI_DEBUG("FormattingPanel: shown rect=%d,%d %dx%d", x(), y(), width(), height());
}

bool FormattingPanel::eventFilter(QObject *object, QEvent *event) {
    if (object == parentWidget() && event->type() == QEvent::Resize && isVisible()) updateOverlayGeometry();
    return QWidget::eventFilter(object, event);
}

void FormattingPanel::updateOverlayGeometry() {
    auto *parent = parentWidget();
    setFixedWidth(parent ? qMin(FormattingPanelWidth, parent->width()) : FormattingPanelWidth);
    setFixedHeight(layout()->sizeHint().height());
    layout()->activate();
    auto *details = findChild<QLabel*>(QStringLiteral("formattingHelpDetails"));
    if (!details) { GUI_ERROR("FormattingPanel: cannot size panel without details label"); return; }
    setFixedHeight(details->geometry().bottom() + 1 + layout()->contentsMargins().bottom());
    layout()->activate();
    if (parent) move(qMax(0, parent->width() - width()), qMax(0, parent->height() - height()));
}
