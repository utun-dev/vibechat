/* Выбор поддерживаемого языка интерфейса независимо от формата дат и чисел QLocale.
 * Не устанавливает QTranslator и не меняет системную locale: возвращает код en/ru. */
#pragma once

#include <QString>
#include <QStringList>
#include <QLocale>

/* preference=en/ru выбирает язык явно; прочие значения (включая system) используют systemLanguages.
 * По умолчанию берётся QLocale::system().uiLanguages(). Проверка идёт по порядку списка,
 * без учёта регистра/региона (- и _); первый поддерживаемый язык побеждает, иначе возвращается en. */
inline QString resolveGuiLanguage(const QString& preference, const QStringList& systemLanguages = QLocale::system().uiLanguages()) {
    if (preference == QStringLiteral("en") || preference == QStringLiteral("ru")) return preference;
    for (const QString& language : systemLanguages) {
        const QString code = language.section('-', 0, 0).section('_', 0, 0).toLower();
        if (code == QStringLiteral("en") || code == QStringLiteral("ru")) return code;
    }
    return QStringLiteral("en");
}
