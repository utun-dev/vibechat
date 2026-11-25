/* Текстовые INI-обновления без перезаписи исходного файла до успешного commit. */
#include "config_updater.h"
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

extern "C" {
#include "../../../src/config_ini.h"
}

/* Тип секции регистронезависим, имя экземпляра сохраняет регистр, как в ядре. */
static QString normalized_section(const QString& section) {
    const int colon = section.indexOf(':');
    if (colon >= 0) return section.left(colon).trimmed().toLower() + ':' + section.mid(colon + 1).trimmed();
    const QString name = section.trimmed().toLower();
    return name == "chat" ? QString("chatserver") : name;
}

static QString section_name(const QString& line) {
    const QString text = line.trimmed();
    return text.startsWith('[') && text.endsWith(']') ? normalized_section(text.mid(1, text.size() - 2)) : QString();
}

static bool single_line(const QString& value) {
    return !value.contains('\n') && !value.contains('\r') && !value.contains(QChar(0));
}

/* Отказ до изменения lines: аргументы не могут создавать дополнительные строки/секции. */
bool config_update_in_lines(QStringList& lines, const QString& section,
                            const QString& key, const QString& value, bool replace) {
    const bool sectionValid = !section.trimmed().isEmpty() && section == section.trimmed() && single_line(section) &&
        !section.contains('[') && !section.contains(']');
    const bool keyValid = !key.trimmed().isEmpty() && key == key.trimmed() && single_line(key) && !key.contains('=') &&
        !key.contains('[') && !key.contains(']') && !key.startsWith('#') && !key.startsWith(';');
    const bool valueValid = single_line(value);
    if (!sectionValid || !keyValid || !valueValid) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI config update rejected section_valid=%d key_valid=%d value_single_line=%d",
                    sectionValid, keyValid, valueValid);
        return false;
    }
    const QString wanted = normalized_section(section);
    const bool controlAlias = wanted == "control" && (key == "control_ip" || key == "control_port" || key == "control_allow");
    const QString option = controlAlias ? key.mid(8) : key;
    const QString replacement = key + '=' + value;
    int lastSection = -1, lastKey = -1, replaced = 0;
    bool matching = false;
    for (int i = 0; i < lines.size(); ++i) {
        const QString name = section_name(lines[i]);
        if (!name.isEmpty()) {
            matching = name == wanted;
            if (matching) lastSection = i;
            continue;
        }
        if (!matching) continue;
        const int equal = lines[i].indexOf('=');
        if (equal <= 0) continue;
        QString candidate = lines[i].left(equal).trimmed();
        if (wanted == "control" && (candidate == "control_ip" || candidate == "control_port" || candidate == "control_allow"))
            candidate = candidate.mid(8);
        if (candidate != option) continue;
        lastKey = i;
        if (replace) { lines[i] = replacement; ++replaced; }
    }
    if (!replaced) {
        if (lastSection < 0) {
            if (!lines.isEmpty() && !lines.last().trimmed().isEmpty()) lines.append("");
            lines.append('[' + section + ']');
            lines.append(replacement);
        } else {
            lines.insert(lastKey >= 0 ? lastKey + 1 : lastSection + 1, replacement);
        }
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "GUI config prepared section=%s key=%s mode=%s replaced=%d",
                qPrintable(section), qPrintable(key), replace ? "replace" : "append", replaced);
    return true;
}

/* ENOENT допускает создание; ошибки чтения и непредставимый UTF-8 запрещают перезапись. */
bool config_read_lines(const QString& path, QStringList& lines, bool allowMissing, bool* ioError) {
    if (ioError) *ioError = false;
    config_error_clear();
    QFile input(path);
    const QFileInfo info(path);
    if (allowMissing && !info.exists() && !info.isSymLink()) { lines.clear(); return true; }
    if (!input.open(QIODevice::ReadOnly)) {
        if (ioError) *ioError = true;
        CONFIG_ERROR("Cannot open configuration %s: %s", qPrintable(path), qPrintable(input.errorString()));
        return false;
    }
    const QByteArray data = input.readAll();
    if (input.error() != QFileDevice::NoError) {
        if (ioError) *ioError = true;
        CONFIG_ERROR("Cannot read configuration %s: %s", qPrintable(path), qPrintable(input.errorString()));
        return false;
    }
    const QString text = QString::fromUtf8(data);
    if (text.toUtf8() != data) {
        CONFIG_ERROR("%s: configuration text is not valid UTF-8", qPrintable(path));
        return false;
    }
    if (config_ini_validate(data.constData(), size_t(data.size()), qPrintable(path))) return false;
    QStringList result;
    if (!text.isEmpty()) {
        result = text.split('\n');
        if (result.last().isEmpty()) result.removeLast();
        for (QString& line : result) if (line.endsWith('\r')) line.chop(1);
    }
    lines = result;
    DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "GUI config read path=%s bytes=%lld lines=%lld",
                qPrintable(path), (long long)data.size(), (long long)lines.size());
    return true;
}

/* QSaveFile владеет соседним temp и удаляет его при ошибке; прямой fallback запрещён. */
bool config_write_lines(const QString& path, const QStringList& lines) {
    QByteArray data = lines.join('\n').toUtf8();
    if (!data.isEmpty() && !data.endsWith('\n')) data.append('\n');
    if (config_ini_validate(data.constData(), size_t(data.size()), qPrintable(path))) return false;
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    const char *phase = "open";
    bool saved = output.open(QIODevice::WriteOnly);
    if (saved) { phase = "write"; saved = output.write(data) == data.size(); }
    if (saved) { phase = "commit"; saved = output.commit(); }
    if (!saved) {
        const int error = errno;
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI config save failed path=%s phase=%s bytes=%lld: %s; errno=%d (%s)",
                    qPrintable(path), phase, (long long)data.size(), qPrintable(output.errorString()), error, strerror(error));
        return false;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "GUI config saved path=%s bytes=%lld", qPrintable(path), (long long)data.size());
    return true;
}

bool config_update_option(const QString& filePath, const QString& section,
                          const QString& param, const QString& value, int flags) {
    if (flags != CONFIG_UPDATE_REPLACE && flags != CONFIG_UPDATE_APPEND) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI config update rejected flags=%d path=%s", flags, qPrintable(filePath));
        return false;
    }
    QStringList lines;
    if (!config_read_lines(filePath, lines, true) ||
        !config_update_in_lines(lines, section, param, value, flags == CONFIG_UPDATE_REPLACE)) return false;
    return config_write_lines(filePath, lines);
}

bool config_rewrite_servers(const QString& filePath, const QList<ConfigServer>& servers) {
    QStringList lines;
    if (!config_read_lines(filePath, lines, true)) return false;

    QStringList block;
    int index = 0;
    for (const ConfigServer& server : servers) {
        const struct { const char *field; bool valid; } checks[] = {
            {"name", !server.name.trimmed().isEmpty() && server.name == server.name.trimmed() && !server.name.contains(':') &&
                     !server.name.contains('[') && !server.name.contains(']') && single_line(server.name)},
            {"addr", single_line(server.addr)}, {"transport", single_line(server.transport)},
            {"type", single_line(server.type)}, {"netif", single_line(server.netif)}
        };
        for (const auto& check : checks) {
            if (check.valid) continue;
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI config rejected malformed server index=%d field=%s path=%s",
                        index, check.field, qPrintable(filePath));
            return false;
        }
        block << ("[server:" + server.name + ']') << ("addr=" + server.addr);
        block << ("transport=" + (server.transport.isEmpty() ? QString("udp") : server.transport));
        block << ("type=" + (server.type.isEmpty() ? QString("public") : server.type));
        if (!server.netif.isEmpty()) block << ("netif=" + server.netif);
        block << "";
        ++index;
    }

    // Удаляем только server-секции целиком, сохраняя прочие строки в прежнем порядке.
    QStringList filtered;
    bool serverSection = false;
    for (const QString& line : lines) {
        const QString section = section_name(line);
        if (!section.isEmpty()) serverSection = section.startsWith("server:");
        if (!serverSection) filtered << line;
    }
    int insertPos = filtered.size();
    bool globalSection = false;
    for (int i = 0; i < filtered.size(); ++i) {
        const QString section = section_name(filtered[i]);
        if (section.isEmpty()) continue;
        if (globalSection) { insertPos = i; break; }
        globalSection = section == "global";
    }
    for (int i = block.size() - 1; i >= 0; --i) filtered.insert(insertPos, block[i]);
    DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "GUI config prepared servers path=%s count=%lld", qPrintable(filePath), (long long)servers.size());
    return config_write_lines(filePath, filtered);
}
