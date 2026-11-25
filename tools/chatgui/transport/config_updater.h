/* Текстовые правки UTF-8 INI для desktop. Вызовы синхронные; вызывающий исключает
 * параллельные изменения одного файла. Это не проверка схемы/значений ядра.
 * Файловые операции проверяют чтение, UTF-8, NUL и предел строки 1022 байта;
 * запись через соседний temp QSaveFile без direct-write fallback, с проверкой commit.
 * До успешной замены исходный файл не усекается; ошибки логируются в CONFIG.
 * Неправленные строки сохраняются, окончания строк нормализуются в LF. */
#ifndef GUI_CONFIG_UPDATER_H
#define GUI_CONFIG_UPDATER_H

#include <QString>
#include <QStringList>
#include <QList>

enum ConfigUpdateFlag {
    CONFIG_UPDATE_REPLACE = 0,  // заменить все вхождения ключа в подходящих секциях
    CONFIG_UPDATE_APPEND  = 1   // добавить ещё одно значение, сохранив прежние
};

/* Один listen-сокет = секция [server:name]. */
struct ConfigServer {
    QString name;        // суффикс секции: "lan", "lan6"
    QString addr;        // "0.0.0.0:1333" | "[::]:1333"
    QString transport;   // "udp" | "tcp"
    QString type;        // "public" | "nat" | "private" | "local"
    QString netif;       // "" = не привязывать к интерфейсу
};

/* Проверенное чтение/сохранение для пакетных правок, одна запись на весь набор.
 * ioError при передаче сообщает об отказе открытия/чтения файла.
 * read заменяет lines только при успехе; allowMissing принимает отсутствующий файл как пустой.
 * write нормализует LF и добавляет конечный LF непустому тексту, проверяет физический INI-формат.
 * true — успех; false — ошибка с CONFIG-диагностикой. Правила владения и записи см. в начале файла. */
bool config_read_lines(const QString& filePath, QStringList& lines, bool allowMissing = false, bool* ioError = nullptr);
bool config_write_lines(const QString& filePath, const QStringList& lines);

/* Изменить принадлежащие вызывающему lines, без файлового I/O; true — подготовлено.
 * section/key непустые, без внешних пробелов; аргументы не содержат CR/LF/NUL.
 * Значение — сырой INI-текст без экранирования. Ошибочные аргументы дают false без правок.
 * Тип секции без учёта регистра, суффикс :name с учётом; chat == chatserver;
 * в control пары ip/control_ip, port/control_port, allow/control_allow равнозначны.
 * replace меняет все дубли; иначе вставка после последнего ключа. При отсутствии ключа
 * используется последняя подходящая секция, при отсутствии секции она создаётся в конце.
 * Физические ограничения файла проверяются при файловой записи, не этой функцией. */
bool config_update_in_lines(QStringList& lines, const QString& section,
                            const QString& key, const QString& value, bool replace);

/* Прочитать filePath (отсутствующий файл допустим), применить правку и сохранить.
 * flags — ConfigUpdateFlag; true только после успешного commit, false с логом ошибки.
 * Как и read/modify/write в целом, не обеспечивает атомарность нескольких отдельных вызовов. */
bool config_update_option(const QString& filePath, const QString& section,
                          const QString& param, const QString& value,
                          int flags = CONFIG_UPDATE_REPLACE);

/* Полностью заменить server-секции; вставить блок после первого global, иначе в конец.
 * Пустой servers удаляет все server-секции. Остальные секции не изменяются.
 * В новом блоке одна пустая строка после каждой секции, transport/type по умолчанию udp/public.
 * Имена/поля не могут добавлять заголовки или строки; адреса и enum-значения проверяет вызывающий/ядро.
 * Возврат и файловые гарантии — как у config_update_option. */
bool config_rewrite_servers(const QString& filePath, const QList<ConfigServer>& servers);

#endif // GUI_CONFIG_UPDATER_H
