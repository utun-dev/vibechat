/* Разметка, реальные языковые определения, кеш и метрики текста без участия сетевого ядра. */
#include "../src/message_text.h"
#include "../src/messagedelegate.h"
#include "../../../lib/debug_config.h"
#include <QtTest>
#include <cmath>
#include <QStandardItemModel>

class MessageTextTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { debug_enable_console(true); debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_DEBUG); }

    void parse_data() {
        QTest::addColumn<QString>("source"); QTest::addColumn<QString>("visible");
        QTest::addColumn<int>("styles"); QTest::addColumn<int>("blocks");
        QTest::newRow("plain") << "hello\nworld" << "hello\nworld" << 0 << 0;
        QTest::newRow("styles") << "**жирный** __наклонный__ ~~удалённый~~" << "жирный наклонный удалённый" << 3 << 0;
        QTest::newRow("italic") << "__чччч__" << "чччч" << 1 << 0;
        QTest::newRow("single-asterisk") << "*hello*" << "*hello*" << 0 << 0;
        QTest::newRow("unclosed") << "**hello __world ~~bye" << "**hello __world ~~bye" << 0 << 0;
        QTest::newRow("no-multiline-style") << "**hello\nworld**" << "**hello\nworld**" << 0 << 0;
        QTest::newRow("empty-style") << "**** ** ** ____ __ __ ~~~~" << "**** ** ** ____ __ __ ~~~~" << 0 << 0;
        QTest::newRow("no-nesting") << "**hello __world__ **" << "**hello world **" << 1 << 0;
        QTest::newRow("nested-literal") << "**hello __world__**" << "hello __world__" << 1 << 0;
        QTest::newRow("escapes") << R"(\*hello\* \\ ~~bye~~)" << "*hello* \\ bye" << 1 << 0;
        QTest::newRow("escaped-italic") << R"(\_\_hello\_\_)" << "__hello__" << 0 << 0;
        QTest::newRow("multiline-italic") << "__hello\nworld__" << "__hello\nworld__" << 0 << 0;
        QTest::newRow("code") << "before\n```cpp\n  int x;\n\t/* **literal** */\n\n```\nafter"
            << "before\n  int x;\n\t/* **literal** */\n\nafter" << 0 << 1;
        QTest::newRow("empty-code") << "```\n```" << "" << 0 << 1;
        QTest::newRow("windows-newlines") << "```cpp\r\nint x;\r\n```" << "int x;" << 0 << 1;
        QTest::newRow("blank-code") << "```\n\n\n```" << "\n" << 0 << 1;
        QTest::newRow("two-blocks") << "```c\nint x;\n```\n**bold**\n```python\npass\n```"
            << "int x;\nbold\npass" << 1 << 2;
        QTest::newRow("unclosed-code") << "```cpp\nint x;" << "```cpp\nint x;" << 0 << 0;
        QTest::newRow("inline-fence") << "text ``` code ```" << "text ``` code ```" << 0 << 0;
        QTest::newRow("indented-fence") << " ```\nx\n ```" << "x" << 0 << 1;
        QTest::newRow("spaced-language") << "``` cpp \n  int x;  \n```   " << "  int x;  " << 0 << 1;
        QTest::newRow("tabbed-language") << "\t```\tCPP\t\r\n\tint x;\r\n  ```\t" << "\tint x;" << 0 << 1;
        QTest::newRow("spaced-empty-code") << "``` \t\n \t```  " << "" << 0 << 1;
        QTest::newRow("spaced-blocks") << "``` c  \nint x;\n ``` \ntext\n ``` js \nconst x = 1;\n```\t"
            << "int x;\ntext\nconst x = 1;" << 0 << 2;
        QTest::newRow("closing-with-text") << "``` cpp \nint x;\n``` end " << "``` cpp \nint x;\n``` end " << 0 << 0;
        QTest::newRow("trailing-fence-text") << "```cpp extra\nx\n```" << "```cpp extra\nx\n```" << 0 << 0;
        QTest::newRow("no-word-markup") << "some__identifier__ a**b**c" << "some__identifier__ a**b**c" << 0 << 0;
        QTest::newRow("italic-url") << "__https://example.org__" << "https://example.org" << 1 << 0;
        QTest::newRow("italic-in-code") << "```\n__чччч__\n```" << "__чччч__" << 0 << 1;
        QTest::newRow("url-markers") << "https://example.com/*path*/ **https://example.org**"
            << "https://example.com/*path*/ https://example.org" << 1 << 0;
    }
    void parse() {
        QFETCH(QString, source); QFETCH(QString, visible); QFETCH(int, styles); QFETCH(int, blocks);
        const auto parsed = parseMessageText(source);
        QCOMPARE(parsed.text, visible); QCOMPARE(parsed.styles.size(), styles); QCOMPARE(parsed.code.size(), blocks);
        for (const auto& style : parsed.styles) {
            QVERIFY(style.start >= 0); QVERIFY(style.length > 0); QVERIFY(style.start + style.length <= parsed.text.size());
        }
    }

    void stylePropertiesAndCodeLinks() {
        const auto parsed = parseMessageText("**bold** __чччч__ ~~strike~~\n```cpp\nhttps://example.com\n```\nhttps://example.org");
        QCOMPARE(parsed.styles[0].format.fontWeight(), QFont::Bold);
        QVERIFY(parsed.styles[1].format.fontItalic()); QVERIFY(parsed.styles[2].format.fontStrikeOut());
        QCOMPARE(parsed.links.size(), 1); QCOMPARE(parsed.links[0].url, QString("https://example.org"));
        QVERIFY(parsed.isCode(parsed.text.indexOf("https://example.com")));
        QVERIFY(!parsed.isCode(parsed.links[0].start));
    }

    void syntaxContrast() {
        QStandardItemModel model(1, 1); const auto index = model.index(0, 0);
        model.setData(index, "```cpp\n// comment\nint main() { return 42; }\nconst char* text = \"string\";\n```", MsgTextRole);
        const auto parsed = formattedMessageText(index); QVERIFY(parsed);
        auto luminance = [](const QColor& color) {
            auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
            return .2126 * linear(color.redF()) + .7152 * linear(color.greenF()) + .0722 * linear(color.blueF());
        };
        for (bool dark : {false, true}) {
            const auto& tokens = dark ? parsed->darkSyntax : parsed->lightSyntax;
            QVERIFY(!tokens.isEmpty());
            const double background = luminance(dark ? QColor("#303030") : QColor("#ededed"));
            for (const auto& token : tokens) {
                const QColor color = token.format.foreground().color();
                const double text = luminance(color);
                QVERIFY2((qMax(text, background) + .05) / (qMin(text, background) + .05) >= 4.5, qPrintable(color.name()));
            }
        }
    }

    void syntaxAndCache() {
        QStandardItemModel model(1, 1); const auto index = model.index(0, 0);
        model.setData(index, "__чччч__", MsgTextRole);
        const auto italic = formattedMessageText(index); QVERIFY(italic);
        QCOMPARE(italic->text, QString("чччч")); QCOMPARE(italic->styles.size(), 1);
        QVERIFY(italic->styles[0].format.fontItalic());
        const QString source = "```cpp\n/* comment\n   continued */\nint x = 42;\n```";
        model.setData(index, source, MsgTextRole);
        const auto parsed = formattedMessageText(index); QVERIFY(parsed);
        QCOMPARE(parsed->text, QString("/* comment\n   continued */\nint x = 42;"));
        QVERIFY(!parsed->lightSyntax.isEmpty()); QVERIFY(!parsed->darkSyntax.isEmpty());
        auto colorAt = [&](int position) {
            QColor color;
            for (const auto& token : parsed->lightSyntax)
                if (position >= token.start && position < token.start + token.length) color = token.format.foreground().color();
            return color;
        };
        QVERIFY(colorAt(0).isValid()); QCOMPARE(colorAt(0), colorAt(parsed->text.indexOf("continued")));
        QVERIFY(colorAt(0) != colorAt(parsed->text.indexOf("int")));
        QCOMPARE(formattedMessageText(index).data(), parsed.data());
        QCOMPARE(index.data(MsgTextRole).toString(), source);
        model.setData(index, " \t``` \tCPP \t\n  int x = 42;  \n ``` \t", MsgTextRole);
        const auto spaced = formattedMessageText(index); QVERIFY(spaced);
        QCOMPARE(spaced->text, QString("  int x = 42;  ")); QCOMPARE(spaced->code.front().languageName, QString("C++"));
        QVERIFY(!spaced->lightSyntax.isEmpty());
        model.setData(index, "```unknown_language\nint x;\n```", MsgTextRole);
        const auto unknown = formattedMessageText(index); QVERIFY(unknown); QVERIFY(unknown->lightSyntax.isEmpty());
        QCOMPARE(unknown->code.front().languageName, QString("unknown_language"));
        model.setData(index, "```\nint x;\n```", MsgTextRole);
        const auto plainCode = formattedMessageText(index); QVERIFY(plainCode); QVERIFY(plainCode->lightSyntax.isEmpty());
        QVERIFY(plainCode->code.front().languageName.isEmpty());
        model.setData(index, "```\n```", MsgTextRole);
        const auto emptyCode = formattedMessageText(index); QVERIFY(emptyCode);
        QTextLayout emptyLayout; emptyLayout.setFont(QApplication::font());
        QVERIFY(layoutFormattedMessage(emptyLayout, *emptyCode, 200, false) > 0);
        QCOMPARE(emptyLayout.lineCount(), 1); QVERIFY(emptyCode->isCode(emptyLayout.lineAt(0).textStart()));
        model.setData(index, "**invitation**", MsgTextRole); model.setData(index, "utun://abc", MsgInviteLinkRole);
        QVERIFY(!formattedMessageText(index));
        model.setData(index, QString(), MsgInviteLinkRole); model.setData(index, "poll", MsgContentTypeRole);
        QVERIFY(!formattedMessageText(index));
    }

    void layoutPreservesTextAndThemeGeometry() {
        QStandardItemModel model(1, 1); const auto index = model.index(0, 0);
        model.setData(index, "**Текст 😀**\n```cpp\n\tint value = 123456789; // длинная строка\n\n```\n__конец__", MsgTextRole);
        const auto parsed = formattedMessageText(index); QVERIFY(parsed);
        const auto original = parsed->text;
        for (int width : {80, 200, 600}) {
            QTextLayout light, dark; light.setFont(QApplication::font()); dark.setFont(light.font());
            const qreal height = layoutFormattedMessage(light, *parsed, width, false);
            QCOMPARE(layoutFormattedMessage(dark, *parsed, width, true), height);
            QCOMPARE(light.lineCount(), dark.lineCount());
            DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "formatted layout: width=%d lines=%d height=%.1f", width, light.lineCount(), height);
            QVERIFY(light.lineCount() >= 4);
            for (int i = 0; i < light.lineCount(); ++i) {
                QCOMPARE(light.lineAt(i).textStart(), dark.lineAt(i).textStart());
                QCOMPARE(light.lineAt(i).naturalTextWidth(), dark.lineAt(i).naturalTextWidth());
                QVERIFY(light.lineAt(i).y() + light.lineAt(i).height() <= height);
            }
            QCOMPARE(parsed->text, original);
        }
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "message formatting: real C++ tokens, multiline state, theme geometry and cache verified");
    }

    void automaticSyntax() {
        QStandardItemModel model(1, 1); const auto index = model.index(0, 0);
        const QVector<QPair<QString, QString>> samples = {
            {"c", "#include <stdio.h>\nint main(void) { return 0; }"},
            {"cpp", "std::vector<int> values;"},
            {"java", "package demo;\npublic class Example { public static void main(String[] args) {} }"},
            {"kotlin", "fun main() { println(42) }"},
            {"rust", "fn main() { println!(\"Hello\"); }"},
            {"php", "<?php\necho 'Hello';"},
            {"js", "const f = (x) => console.log(x);"},
            {"css", ".example { color: red; margin: 1px; }"},
            {"html", "<!doctype html>\n<html><body>Hello</body></html>"},
        };
        const QHash<QString, QString> names = {{"c", "C"}, {"cpp", "C++"}, {"java", "Java"}, {"kotlin", "Kotlin"},
            {"rust", "Rust"}, {"php", "PHP"}, {"js", "JavaScript"}, {"css", "CSS"}, {"html", "HTML"}};
        for (const auto& sample : samples) {
            const QString source = "```\n" + sample.second + "\n```";
            model.setData(index, source, MsgTextRole);
            const auto detected = formattedMessageText(index); QVERIFY(detected);
            QVERIFY2(!detected->lightSyntax.isEmpty(), qPrintable(sample.first));
            QVERIFY2(!detected->darkSyntax.isEmpty(), qPrintable(sample.first));
            QCOMPARE(detected->text, sample.second); QVERIFY(detected->code.front().language.isEmpty());
            QCOMPARE(detected->code.front().languageName, names.value(sample.first));
            QCOMPARE(index.data(MsgTextRole).toString(), source);
            QCOMPARE(formattedMessageText(index).data(), detected.data());
            model.setData(index, "```" + sample.first + "\n" + sample.second + "\n```", MsgTextRole);
            const auto explicitCode = formattedMessageText(index); QVERIFY(explicitCode);
            QCOMPARE(explicitCode->code.front().languageName, detected->code.front().languageName);
            QCOMPARE(explicitCode->lightSyntax.size(), detected->lightSyntax.size());
            for (const QString& label : {QString("text"), QString("plain"), QString("unsupported")}) {
                model.setData(index, "```" + label + "\n" + sample.second + "\n```", MsgTextRole);
                const auto plain = formattedMessageText(index); QVERIFY(plain); QVERIFY(plain->lightSyntax.isEmpty());
                QCOMPARE(plain->code.front().languageName, label == "unsupported" ? label : QString("text"));
            }
        }
        // Явный язык сильнее автоматического определения по тексту.
        model.setData(index, "```js\nstd::vector<int> values;\n```", MsgTextRole);
        const auto explicitJs = formattedMessageText(index); QVERIFY(explicitJs);
        QCOMPARE(explicitJs->code.front().language, QString("js"));
    }
};
QTEST_MAIN(MessageTextTest)
#include "test_message_text.moc"
