/* Язык определяется только по тексту. Необязательная внешняя выборка: CODE_LANGUAGE_CORPUS=/path/manifest.json. */
#include "../src/code_language.h"
#include "../src/debug_ui.h"
#include <QtTest>
#include <QFile>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>

class CodeLanguageTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { debug_enable_console(true); debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_DEBUG); }
    void samples_data() {
        QTest::addColumn<QString>("text"); QTest::addColumn<QString>("language");
        QTest::newRow("c") << "#include <stdio.h>\nint main(void) { puts(\"hello\"); }" << "C";
        QTest::newRow("cpp") << "#include <stdio.h>\nint main() { std::cout << 42; }" << "C++";
        QTest::newRow("java") << "package example;\npublic class Hello { public static void main(String[] args) {} }" << "Java";
        QTest::newRow("java-method") << "public static Result parse(String input) { return new Result(input); }" << "Java";
        QTest::newRow("cpp-method") << "class Example { public: void run() {} };" << "C++";
        QTest::newRow("kotlin") << "fun main() { println(\"hello\") }" << "Kotlin";
        QTest::newRow("rust") << "fn main() { println!(\"hello\"); }" << "Rust";
        QTest::newRow("rust-std") << "use std::{borrow::Cow, collections::BTreeSet};" << "Rust";
        QTest::newRow("c-macros") << "#include \"local.h\"\n#define COUNT 4\nunsigned long value;" << "C";
        QTest::newRow("php") << "<?php\necho 'hello';" << "PHP/PHP";
        QTest::newRow("php-fragment") << "function hello($name) { $message = $name; return $message; }" << "PHP/PHP";
        QTest::newRow("javascript") << "const greet = (name) => console.log(name);" << "JavaScript";
        QTest::newRow("css") << ".hello { color: red; margin: 1px; }" << "CSS";
        QTest::newRow("html") << "<!doctype html>\n<html><script>const f = () => 1;</script></html>" << "HTML";
        QTest::newRow("html-fragment") << "<div class=\"hello\"><span>Hello</span></div>" << "HTML";
        QTest::newRow("html-css") << "<style>.x { color: red; margin: 1px; }</style><p>Hello</p>" << "HTML";
        QTest::newRow("html-js") << "<script>document.addEventListener('load', function() {});</script>" << "HTML";
        QTest::newRow("html-script-list") << "<script src=\"test.js\"></script>" << "HTML";
        QTest::newRow("ambiguous-let-arrow") << "let x = match value { Value::A => true, Value::B => false };" << "";
        QTest::newRow("empty") << "" << "";
        QTest::newRow("ambiguous") << "int x = 42;" << "";
        QTest::newRow("prose") << "Java and Kotlin have class and function declarations; use CSS for color." << "";
        QTest::newRow("comments") << "/* fn main() { println!(\"hello\"); } */\n// std::vector<int> x;" << "";
        QTest::newRow("strings") << "\"std::vector<int>\"\n'fun main() {}'" << "";
        QTest::newRow("template-string") << "const code = `std::vector<int>\nfn main() {}`;" << "";
        QTest::newRow("multiline-string") << "\"\"\"\nfun main() {}\n\"\"\"" << "";
        QTest::newRow("incomplete-string") << "\"std::vector<int>" << "";
        QTest::newRow("json") << "{\"code\": \"fn main() {}\", \"color\": \"red\"}" << "";
        QTest::newRow("conflicting") << "fn test() {}\nfun main() {}" << "";
        QTest::newRow("repeated-weak") << QString("const value = 1;\n").repeated(200) << "";
        QTest::newRow("bounded") << (QString(16384, ' ') + "std::cout << 42;") << "";
        QTest::newRow("crlf") << "#include <stdio.h>\r\nint main(void) { return 0; }" << "C";
    }
    void samples() { QFETCH(QString, text); QFETCH(QString, language); QCOMPARE(detectCodeLanguage(text), language); }

    void corpus() {
        const QString manifest = qEnvironmentVariable("CODE_LANGUAGE_CORPUS");
        if (manifest.isEmpty()) QSKIP("Set CODE_LANGUAGE_CORPUS to run the downloaded GitHub corpus");
        QFile file(manifest); QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        QCOMPARE(error.error, QJsonParseError::NoError); QVERIFY(document.isArray());
        const QDir directory = QFileInfo(manifest).absoluteDir();
        struct Counts { int correct = 0, unknown = 0, wrong = 0; } full, snippets;
        QElapsedTimer elapsed; elapsed.start();
        for (const auto& item : document.array()) {
            const auto entry = item.toObject(); const QString path = entry["file"].toString();
            QFile source(directory.filePath(path)); QVERIFY2(source.open(QIODevice::ReadOnly), qPrintable(path));
            const auto bytes = source.readAll();
            QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()), entry["sha256"].toString());
            const QString text = QString::fromUtf8(bytes), expected = entry["language"].toString();
            const auto check = [&](const QString& code, const QString& expected, const char* kind, Counts& counts) {
                const QString actual = detectCodeLanguage(code);
                if (actual == expected) ++counts.correct;
                else if (actual.isEmpty()) ++counts.unknown;
                else ++counts.wrong;
                GUI_INFO("corpus: file=%s kind=%s expected=%s actual=%s", qPrintable(path), kind,
                         qPrintable(expected), actual.isEmpty() ? "unknown" : qPrintable(actual));
            };
            check(text, expected, "full", full);
            const auto lines = text.split('\n');
            // Фиксированные окна, не выбираемые по результату: начало, середина, конец по 20 строк.
            int window = 0;
            const auto labels = entry["snippets"].toArray();
            for (int start : {0, qMax(0, int(lines.size()) / 2 - 10), qMax(0, int(lines.size()) - 20)}) {
                const QString label = labels.isEmpty() ? expected : labels[window].toString();
                check(lines.mid(start, 20).join('\n'), label, "snippet", snippets); ++window;
            }
        }
        GUI_INFO("corpus totals: full correct=%d unknown=%d wrong=%d; snippets correct=%d unknown=%d wrong=%d",
                 full.correct, full.unknown, full.wrong, snippets.correct, snippets.unknown, snippets.wrong);
        GUI_INFO("corpus elapsed: %lld ms including input/SHA-256/logging", (long long)elapsed.elapsed());
        QVERIFY(full.correct > 0); QCOMPARE(full.wrong, 0); QCOMPARE(snippets.wrong, 0);
    }
};
QTEST_GUILESS_MAIN(CodeLanguageTest)
#include "test_code_language.moc"
