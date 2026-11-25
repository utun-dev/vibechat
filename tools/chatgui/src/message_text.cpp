#include "message_text.h"
#include "code_language.h"
#include "messagedelegate.h"
#include "debug_ui.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <QCache>
#include <QCoreApplication>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QRegularExpression>
#include <KSyntaxHighlighting/AbstractHighlighter>
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Format>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/State>
#include <KSyntaxHighlighting/Theme>

bool MessageText::isCode(int position) const {
    auto next = std::upper_bound(code.cbegin(), code.cend(), position,
                                 [](int offset, const MessageCodeBlock& block) { return offset < block.start; });
    if (next == code.cbegin()) return false;
    const auto& block = *--next;
    return position <= block.start + block.length;
}

namespace {
struct Marker {
    int position, length, kind; // Позиция в строке после escapes; kind: 0=**, 1=__, 2=~~.
    bool open, close; // Открытие/закрытие допустимо только около непустого текста.
    int nextClose = -1; // Следующий закрывающий маркер того же вида; поиск за O(n).
};

// Разбираем строку отдельно: парные маркеры не пересекают newline и не вкладываются друг в друга.
void appendInline(MessageText& result, const QString& source) {
    QString line;
    QVector<Marker> markers;
    const auto links = messageLinks(source);
    int linkIndex = 0;
    for (int i = 0; i < source.size();) {
        while (linkIndex < links.size() && links[linkIndex].start + links[linkIndex].length <= i) ++linkIndex;
        const bool inLink = linkIndex < links.size() && links[linkIndex].start <= i;
        const QChar ch = source[i];
        if (!inLink && ch == '\\' && i + 1 < source.size() && QStringLiteral("*_~`\\").contains(source[i + 1])) {
            line += source[i + 1]; i += 2; result.changed = true; continue;
        }
        int count = 1;
        if (ch == '*' || ch == '_' || ch == '~') {
            while (i + count < source.size() && source[i + count] == ch) ++count;
            if (count == 2) {
                const bool open = !inLink && i + count < source.size() && !source[i + count].isSpace()
                    && (i == 0 || !source[i - 1].isLetterOrNumber());
                const bool close = i > 0 && !source[i - 1].isSpace()
                    && (!inLink || i + count == links[linkIndex].start + links[linkIndex].length)
                    && (i + count == source.size() || !source[i + count].isLetterOrNumber());
                markers.append({int(line.size()), count, ch == '*' ? 0 : ch == '_' ? 1 : 2, open, close});
            }
        }
        line += source.mid(i, count); i += count;
    }
    int next[3] = {-1, -1, -1};
    for (int i = int(markers.size()) - 1; i >= 0; --i) {
        auto& marker = markers[i]; marker.nextClose = next[marker.kind];
        if (marker.close) next[marker.kind] = i;
    }
    int copied = 0;
    for (int i = 0; i < markers.size(); ++i) {
        const auto& begin = markers[i];
        if (!begin.open || begin.nextClose < 0) continue;
        const auto& end = markers[begin.nextClose];
        result.text += line.mid(copied, begin.position - copied);
        QTextLayout::FormatRange span;
        span.start = result.text.size(); span.length = end.position - begin.position - begin.length;
        if (begin.kind == 0) span.format.setFontWeight(QFont::Bold);
        else if (begin.kind == 1) span.format.setFontItalic(true);
        else span.format.setFontStrikeOut(true);
        result.text += line.mid(begin.position + begin.length, span.length);
        result.styles.append(span); result.changed = true;
        copied = end.position + end.length; i = begin.nextClose;
    }
    result.text += line.mid(copied);
}

// Приглушаем слишком яркие токены светлой схемы и осветляем тёмные токены тёмной схемы.
// Фоны взяты с запасом относительно заливки code-блоков; контраст обычного текста — не ниже 4.5:1.
QColor readableSyntaxColor(QColor color, bool dark) {
    auto luminance = [](const QColor& value) {
        auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
        return .2126 * linear(value.redF()) + .7152 * linear(value.greenF()) + .0722 * linear(value.blueF());
    };
    const double background = luminance(dark ? QColor("#303030") : QColor("#ededed"));
    for (int step = 0; step < 32; ++step) {
        const double text = luminance(color);
        if ((qMax(text, background) + .05) / (qMin(text, background) + .05) >= 4.5) return color;
        color = dark ? color.lighter(120) : color.darker(120);
    }
    return dark ? QColor(Qt::white) : QColor(Qt::black);
}

// Собираем оба набора цветов за один разбор; размеры букв определяет только моноширинный шрифт блока.
class CodeHighlighter : public KSyntaxHighlighting::AbstractHighlighter {
public:
    MessageText& message;
    KSyntaxHighlighting::Theme light, dark;
    int offset = 0;
    int adjustedColors = 0;
    CodeHighlighter(MessageText& text, KSyntaxHighlighting::Repository& repository)
        : message(text), light(repository.defaultTheme(KSyntaxHighlighting::Repository::LightTheme)),
          dark(repository.defaultTheme(KSyntaxHighlighting::Repository::DarkTheme)) { setTheme(light); }

    void highlight(const MessageCodeBlock& block, const KSyntaxHighlighting::Definition& definition) {
        adjustedColors = 0;
        setDefinition(definition);
        KSyntaxHighlighting::State state;
        const int end = block.start + block.length;
        for (offset = block.start; offset < end;) {
            int newline = message.text.indexOf('\n', offset);
            if (newline < 0 || newline > end) newline = end;
            state = highlightLine(message.text.mid(offset, newline - offset), state);
            offset = newline + 1;
        }
        GUI_DEBUG("message code palette: language=%s adjusted_tokens=%d minimum_contrast=4.5",
                  qPrintable(definition.name()), adjustedColors);
    }
protected:
    void applyFormat(int start, int length, const KSyntaxHighlighting::Format& format) override {
        if (length <= 0) return;
        for (bool useDark : {false, true}) {
            const QColor color = format.textColor(useDark ? dark : light);
            if (!color.isValid()) continue;
            QTextLayout::FormatRange span; span.start = offset + start; span.length = length;
            const QColor readable = readableSyntaxColor(color, useDark);
            adjustedColors += readable != color;
            span.format.setForeground(readable);
            (useDark ? message.darkSyntax : message.lightSyntax).append(span);
        }
    }
};
}

MessageText parseMessageText(const QString& source) {
    MessageText result;
    QString normalized = source; normalized.replace("\r\n", "\n");
    const auto lines = normalized.split('\n');
    static const QRegularExpression fence(QStringLiteral("^```[ \\t]*([A-Za-z0-9_+#.-]{0,32})$"));
    QVector<int> closing(lines.size(), -1);
    int next = -1;
    for (int i = int(lines.size()) - 1; i >= 0; --i) {
        closing[i] = next;
        if (lines[i].trimmed() == "```") next = i;
    }
    bool hasLine = false;
    for (int i = 0; i < lines.size(); ++i) {
        if (hasLine) result.text += '\n';
        hasLine = true;
        const auto opening = fence.match(lines[i].trimmed());
        if (opening.hasMatch() && closing[i] >= 0) {
            MessageCodeBlock block{int(result.text.size()), 0, opening.captured(1).toLower()};
            const int last = closing[i];
            for (int row = i + 1; row < last; ++row) {
                if (row > i + 1) result.text += '\n';
                result.text += lines[row];
            }
            block.length = result.text.size() - block.start;
            result.code.append(block); result.changed = true; i = last;
        } else appendInline(result, lines[i]);
    }
    // Ссылки ищем в готовом тексте, а код целиком исключаем из распознавания адресов.
    for (const auto& link : messageLinks(result.text)) {
        if (!result.isCode(link.start)) result.links.append(link);
    }
    return result;
}

QSharedPointer<const MessageText> formattedMessageText(const QModelIndex& index) {
    const QString type = index.data(MsgContentTypeRole).toString();
    if ((!type.isEmpty() && type != "text/plain") || !index.data(MsgInviteLinkRole).toString().isEmpty()) return {};
    const QString source = index.data(MsgTextRole).toString();
    if (!source.contains("**") && !source.contains("__") && !source.contains("~~") && !source.contains('`') && !source.contains('\\'))
        return {};
    // Ограничение учитывает текст ключа, видимый текст и диапазоны; shared pointer жив до конца paint/hit-test.
    static QCache<QString, QSharedPointer<const MessageText>> cache(8 * 1024 * 1024);
    if (const auto* cached = cache.object(source)) return *cached;
    auto message = QSharedPointer<MessageText>::create(parseMessageText(source));
    if (!message->code.isEmpty()) {
        static KSyntaxHighlighting::Repository repository;
        CodeHighlighter highlighter(*message, repository);
        static const QHash<QString, QString> aliases = {{"cpp", "C++"}, {"c++", "C++"}, {"c", "C"},
            {"py", "Python"}, {"js", "JavaScript"}, {"ts", "TypeScript"}, {"sh", "Bash"}, {"shell", "Bash"},
            {"cs", "C#"}, {"csharp", "C#"}, {"kt", "Kotlin"}, {"rs", "Rust"}, {"php", "PHP/PHP"}, {"yml", "YAML"}};
        for (auto& block : message->code) {
            block.languageName = block.language;
            if (block.language == "text" || block.language == "plain") { block.languageName = "text"; continue; }
            const QString language = block.language.isEmpty()
                ? detectCodeLanguage(QStringView(message->text).mid(block.start, block.length)) : block.language;
            if (language.isEmpty()) continue;
            block.languageName = language;
            auto definition = repository.definitionForName(aliases.value(language, language));
            if (!definition.isValid()) {
                for (const auto& candidate : repository.definitions()) {
                    if (candidate.name().compare(language, Qt::CaseInsensitive) == 0) { definition = candidate; break; }
                }
            }
            if (definition.isValid()) {
                block.languageName = definition.name() == "PHP/PHP" ? QStringLiteral("PHP") : definition.name();
                highlighter.highlight(block, definition);
                GUI_DEBUG("message code: language=%s explicit=%d chars=%d", qPrintable(block.languageName),
                          !block.language.isEmpty(), block.length);
            }
            else GUI_DEBUG("message code: unknown language=%s; using plain code", qPrintable(language));
        }
    }
    QSharedPointer<const MessageText> prepared = message->changed ? message : QSharedPointer<const MessageText>();
    const qint64 cost = 256 + 2LL * (source.size() + message->text.size())
        + 256LL * (message->styles.size() + message->lightSyntax.size() + message->darkSyntax.size())
        + 128LL * (message->code.size() + message->links.size());
    cache.insert(source, new QSharedPointer<const MessageText>(prepared), int(qMin<qint64>(cost, INT_MAX)));
    GUI_DEBUG("message text prepared: source=%d visible=%d styles=%d code=%d tokens=%d changed=%d cache_cost=%lld",
              int(source.size()), int(message->text.size()), int(message->styles.size()), int(message->code.size()),
              int(message->lightSyntax.size()), int(message->changed), (long long)cost);
    return prepared;
}

qreal layoutFormattedMessage(QTextLayout& layout, const MessageText& message, int width, bool dark) {
    QString display = message.text; display.replace('\n', QChar(0x2028)); layout.setText(display);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (layout.font().pointSizeF() > 0) mono.setPointSizeF(layout.font().pointSizeF());
    else mono.setPixelSize(layout.font().pixelSize());
    QVector<QTextLayout::FormatRange> formats = message.styles;
    for (const auto& block : message.code) {
        QTextLayout::FormatRange span; span.start = block.start; span.length = block.length;
        span.format.setFont(mono); formats.append(span);
    }
    formats += dark ? message.darkSyntax : message.lightSyntax;
    for (const auto& link : message.links) {
        QTextLayout::FormatRange span; span.start = link.start; span.length = link.length;
        span.format.setForeground(dark ? QColor("#79baff") : QColor("#155da8")); span.format.setFontUnderline(true); formats.append(span);
    }
    layout.setFormats(formats);
    QTextOption option; option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    option.setTabStopDistance(4 * QFontMetricsF(mono).horizontalAdvance(' ')); layout.setTextOption(option);
    layout.beginLayout();
    qreal height = 0;
    int block = 0;
    while (true) {
        auto line = layout.createLine();
        if (!line.isValid()) break;
        if (block < message.code.size() && message.code[block].start == line.textStart()) {
            height += QFontMetricsF(layout.font()).height() + 8; ++block;
        }
        line.setLineWidth(qMax(1, width)); line.setPosition(QPointF(0, height)); height += line.height();
    }
    layout.endLayout();
    return qMax(height, QFontMetricsF(layout.font()).height());
}

QRectF messageCodeCopyRect(const QTextLayout& layout, const MessageCodeBlock& block, int width) {
    const auto line = layout.lineForTextPosition(block.start);
    if (!line.isValid()) return {};
    const QFontMetricsF metrics(layout.font());
    const qreal height = metrics.height() + 8;
    const qreal labelWidth = qMax(metrics.horizontalAdvance(QCoreApplication::translate("ChatGui", "Copy")),
                                 metrics.horizontalAdvance(QCoreApplication::translate("ChatGui", "Copied")));
    const qreal buttonWidth = qMin(qreal(width), labelWidth + 16);
    return QRectF(width - buttonWidth, line.y() - height + 2, buttonWidth, height - 4);
}
