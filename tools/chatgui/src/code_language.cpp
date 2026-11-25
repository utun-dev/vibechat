#include "code_language.h"
#include "debug_ui.h"
#include <QRegularExpression>
#include <array>

namespace {
enum Language { C, Cpp, Java, Kotlin, Rust, Php, JavaScript, Css, Html, LanguageCount };
constexpr const char* names[] = {"C", "C++", "Java", "Kotlin", "Rust", "PHP/PHP", "JavaScript", "CSS", "HTML"};
struct Rule {
    Language language;
    int weight;
    QRegularExpression pattern;
};

// Каждый признак учитывается один раз: повторение общего слова не повышает уверенность.
const Rule rules[] = {
    {C, 3, QRegularExpression(R"(^\s*#\s*include[ \t]+)", QRegularExpression::MultilineOption)},
    {C, 2, QRegularExpression(R"(^\s*#\s*(?:define|ifdef|ifndef)\s+\w+)", QRegularExpression::MultilineOption)},
    {C, 2, QRegularExpression(R"(\bunsigned\s+(?:(?:long|int|char)\s+)?\w+\s*[;=])")},
    {C, 4, QRegularExpression(R"(\b(?:void|int|char|double|float|size_t)\s+\**\s*\w+\s*\([^;{}]*\)\s*\{)")},
    {C, 2, QRegularExpression(R"(\b(?:typedef\s+(?:struct|enum)|malloc\s*\(|calloc\s*\(|sizeof\s*\())")},
    {C, 2, QRegularExpression(R"(\b(?:const\s+)?(?:char|void|int)\s*\*+\s*\w+)")},
    {Cpp, 8, QRegularExpression(R"(\bstd\s*::|\b(?:static_cast|dynamic_cast|reinterpret_cast|const_cast)\s*<)")},
    {Cpp, 7, QRegularExpression(R"(^\s*#\s*include\s*<(?:iostream|vector|string|memory|algorithm|map|utility|array|set|unordered_map)>)",
                               QRegularExpression::MultilineOption)},
    {Cpp, 6, QRegularExpression(R"(\btemplate\s*<|\bnamespace\s+[\w:]+\s*\{)")},
    {Cpp, 4, QRegularExpression(R"(\b(?:class|struct)\s+\w+(?:\s*:\s*[^\n{]+)?\s*\{)")},
    {Cpp, 3, QRegularExpression(R"(\b(?:public|private|protected)\s*:|\b\w+::~?\w+\s*\()")},
    {Java, 3, QRegularExpression(R"(^\s*(?:package|import)\s+(?:static\s+)?[\w.*]+\s*;)", QRegularExpression::MultilineOption)},
    {Java, 4, QRegularExpression(R"(\b(?:public|protected|private)\s+(?:(?:static|final|abstract)\s+)*(?:class|interface|enum)\s+\w+)")},
    {Java, 7, QRegularExpression(R"(\b(?:public|protected|private)\s+(?:(?:static|final|synchronized)\s+)*)"
                                R"((?:void|boolean|int|long|double|float|char|byte|short|[A-Z]\w*)\s+\w+\s*\()")},
    {Java, 4, QRegularExpression(R"(\b(?:implements|extends)\s+[A-Z]\w*|\bSystem\.(?:out|err)\.)")},
    {Kotlin, 6, QRegularExpression(R"(\bfun\s+(?:<[^>\n]+>\s*)?[\w.]+\s*\()")},
    {Kotlin, 3, QRegularExpression(R"(^\s*(?:(?:private|public|internal|override|const)\s+)*(?:val|var)\s+\w+\s*[:=])",
                                  QRegularExpression::MultilineOption)},
    {Kotlin, 5, QRegularExpression(R"(\b(?:data\s+class|sealed\s+class|companion\s+object)\b|\bwhen\s*\([^\n]*\)\s*\{)")},
    {Rust, 7, QRegularExpression(R"(\bfn\s+\w+\s*(?:<[^>\n]+>)?\s*\()")},
    {Rust, 7, QRegularExpression(R"(^\s*(?:pub(?:\([^)]*\))?\s+)?(?:use\s+[\w:]+\s*(?:;|::|\{)|impl(?:\s*<[^>\n]+>)?\s+\w+))",
                                QRegularExpression::MultilineOption)},
    {Rust, 3, QRegularExpression(R"(\blet\s+mut\s+|\b(?:println|vec|assert_eq|format)!\s*[(\[]|#\[derive\s*\()")},
    {Php, 4, QRegularExpression(R"(\$[a-zA-Z_]\w*\s*(?:->|\[|=))")},
    {Php, 4, QRegularExpression(R"(\bfunction\s+\w+\s*\([^\n)]*\$|\b(?:public|private|protected)\s+(?:static\s+)?function\b)")},
    {Php, 3, QRegularExpression(R"(\b(?:echo|require_once|include_once)\b|\barray\s*\(|\$this\b)")},
    {JavaScript, 4, QRegularExpression(R"(\b(?:const|var)\s+[a-zA-Z_$][\w$]*\s*=)")},
    {JavaScript, 2, QRegularExpression(R"(\blet\s+[a-zA-Z_$][\w$]*\s*=)")},
    {JavaScript, 3, QRegularExpression(R"(=>|\bfunction\s*[\w$]*\s*\()")},
    {JavaScript, 4, QRegularExpression(R"(\b(?:module\.exports|exports\.\w+|console\.\w+|document\.\w+)\b)")},
    {JavaScript, 4, QRegularExpression(R"(^\s*(?:export\s+(?:default|function|const|class)\b|import\s+[^;\n]+\s+from\b))",
                                      QRegularExpression::MultilineOption)},
    {Css, 3, QRegularExpression(R"((?:^|[}\n])\s*[.#]?[\w*-][\w\s.#:>+~,*\[\]="'-]*\s*\{)")},
    {Css, 4, QRegularExpression(R"((?:^|[;{\n])\s*(?:color|background(?:-color)?|font(?:-size|-family|-weight)?|display|position)\s*:[^;{}\n]+;)")},
    {Css, 4, QRegularExpression(R"((?:^|[;{\n])\s*(?:margin(?:-\w+)?|padding(?:-\w+)?|border(?:-\w+)?|width|height|opacity|align-items))"
                               R"(\s*:[^;{}\n]+;)")},
};
}

QString detectCodeLanguage(QStringView source) {
    const QString sample = source.left(16384).toString();
    // Явные заголовки контейнеров важнее вложенного CSS/JS/PHP-кода.
    static const QRegularExpression php(R"(^\s*<\?php\b)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression html(R"(^\s*(?:<!doctype\s+html\b|<html\b))", QRegularExpression::CaseInsensitiveOption);
    if (php.match(sample).hasMatch() || html.match(sample).hasMatch()) {
        const QString language = QString::fromLatin1(names[php.match(sample).hasMatch() ? Php : Html]);
        GUI_DEBUG("code language: container=%s sampled=%d", qPrintable(language), int(sample.size()));
        return language;
    }
    // Убираем строки и комментарии перед поиском конструкций, сохраняя границы строк.
    static const QRegularExpression ignored(
        R"re(/\*[\s\S]*?(?:\*/|$)|<!--[\s\S]*?(?:-->|$)|//[^\n]*|"""[\s\S]*?(?:"""|$))re"
        R"re(|`(?:\\[\s\S]|[^`\\])*(?:`|$)|"(?:\\.|[^"\\\n])*(?:"|$)|'(?:\\.|[^'\\\n])*(?:'|$))re");
    QString code = sample;
    auto matches = ignored.globalMatch(sample);
    while (matches.hasNext()) {
        const auto match = matches.next();
        for (int i = match.capturedStart(); i < match.capturedEnd(); ++i) if (code[i] != '\n') code[i] = ' ';
    }
    std::array<int, LanguageCount> scores{};
    for (const auto& rule : rules) if (rule.pattern.match(code).hasMatch()) scores[rule.language] += rule.weight;
    // Структурные теги отличают HTML с вложенным JS/CSS; одиночный div внутри JSX недостаточен.
    static const QRegularExpression structuralTags(R"(</?(?:html|head|body|script|style)\b[^<>]*>)",
                                                   QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tags(R"(<(div|span|p|a|ul|li|table|section|form|button|h[1-6])\b[^<>]*>[\s\S]*?</\1\s*>)",
                                        QRegularExpression::CaseInsensitiveOption);
    if (structuralTags.match(code).hasMatch() || (scores[JavaScript] == 0 && tags.match(code).hasMatch())) {
        GUI_DEBUG("code language: result=HTML markup tags sampled=%d", int(sample.size()));
        return QStringLiteral("HTML");
    }
    // std:: и Type::method встречаются также в Rust; use/fn/impl отличают его от C++.
    if (scores[Rust] >= 7) scores[Cpp] = 0;
    // C является подмножеством C++: специфичные конструкции C++ имеют приоритет.
    if (scores[Cpp] >= 6) scores[C] = 0;
    int best = 0, second = 0;
    for (int i = 1; i < LanguageCount; ++i) if (scores[i] > scores[best]) best = i;
    for (int i = 0; i < LanguageCount; ++i) if (i != best && scores[i] > second) second = scores[i];
    const bool accepted = scores[best] >= 6 && scores[best] - second >= 3;
    GUI_DEBUG("code language: result=%s candidate=%s score=%d runner_up=%d sampled=%d total=%lld",
              accepted ? names[best] : "unknown", names[best], scores[best], second, int(sample.size()), (long long)source.size());
    return accepted ? QString::fromLatin1(names[best]) : QString();
}
