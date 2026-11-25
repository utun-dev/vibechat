/* GUI-проекция desktop INI: идентичность, control и параметры интерфейса.
 * Все вызовы синхронные, в одном потоке; доступ к файлу сериализует вызывающий.
 * load проверяет физический текст и собственные поля GUI без DNS/сетевого I/O.
 * Полную схему сетевых опций проверяет parse_config при запуске ядра.
 * save обновляет только поля этой модели, сохраняя остальные секции/комментарии;
 * публикация через общий config_updater с проверкой записи и commit. */
#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H

#include "../../../src/config_updater.h"
#include <cstdint>
#include <QString>
#include <QStringList>

class NodeConfig {
public:
    explicit NodeConfig(const QString& path);

    bool exists() const; // существует ли путь; доступность проверяется чтением
    bool create(const QString& nodeName); // новый файл: X25519/id, TCP+UDP, control 127.0.0.1:9999; Linux-пути абсолютные
    bool prepare(bool replaceInvalid = false); // подготовить ключи с инициализированным WinSock, затем load; причина отказа в errors()
    config_profile_status startupStatus() const { return m_startupStatus; }
    QString startupMessage() const; // Локализованная причина, путь и место ошибки для диалога запуска.
    bool load(); // false при I/O/повреждённом тексте, прежняя модель сохраняется; true => проверить hasErrors()
    bool save(); // false при неверном поле/I/O; весь набор правок публикуется одной записью

    QString nodeName() const { return m_nodeName; }
    void setNodeName(const QString& name) { m_nodeName = name; }
    QString nodeId() const { return m_nodeId; } // Публичный Ed25519 из seed, 64 HEX либо пусто
    QString pubKey() const { return m_pubKey; }
    QString privKey() const { return m_privKey; } // секрет; не выводить в диагностику

    QString controlAddr() const { return m_controlAddr; } // host:port либо [IPv6]:port; пусто => control не задан
    void setControlAddr(const QString& address) { m_controlAddr = address; }
    quint16 controlPort() const; // 0 при отсутствии/ошибке; явный нулевой порт также допустим
    void setControlPort(quint16 port); // сохраняет host; при пустом адресе использует 127.0.0.1

    bool autoSockets() const { return m_autoSocketsValue != "no"; }
    void setAutoSockets(bool enabled) { m_autoSocketsValue = enabled ? "yes" : "no"; }
    QString autoSocketProtocols() const { return m_autoSocketProtocols; }
    void setAutoSocketProtocols(const QString& protocols) { m_autoSocketProtocols = protocols; }

    QString filePath() const { return m_path; }

    bool utunEnabled() const { return m_utunEnabled; }
    int bluetoothFlags() const { return m_bluetoothFlags; } // 1=BLE, 2=объявление, 4=входящие
    void setUtunEnabled(bool enabled) { m_utunEnabled = enabled; }
    QString dbPath() const { return m_dbPath; }
    void setDbPath(const QString& path) { m_dbPath = path; }
    QString debugFile() const { return m_debugFile; }
    void setDebugFile(const QString& path) { m_debugFile = path; }
    QString debugLevel() const { return m_debugLevel; } // пустой при отсутствии, save использует info
    QString debugCategories() const { return m_debugCategories; } // [debug] как category=level через запятую
    QString language() const { return m_language; }
    void setLanguage(const QString& language) { m_language = language; }

    QString theme() const { return m_theme; } // dark/light
    void setTheme(const QString& theme) { m_theme = theme; }

    /* load заменяет модель целиком; удалённые из файла значения не сохраняются.
     * Ошибки собственных полей не мешают прочитать остальные; save проверяет текущие значения заново. */
    bool hasErrors() const { return !m_errors.isEmpty(); }
    const QStringList& errors() const { return m_errors; }

    bool generateIdentity(); // успех заменяет всю идентичность; ошибка сохраняет прежнюю, файл не меняет

private:
    bool read(bool checkIdentity); // false при подготовке: идентичность проверяет общий updater.
    config_profile_status m_startupStatus = CONFIG_PROFILE_READY;
    int m_errorLine = 0;
    QString m_errorOption;
    QString m_path; // путь INI, не каталог данных
    QString m_nodeName;
    QString m_nodeId;
    QString m_pubKey;
    QString m_privKey;
    QString m_controlAddr;
    bool m_utunEnabled = false; // явно запускаемый дополнительный UTUN-сервис
    int m_bluetoothFlags = 0;
    QString m_autoSocketsValue = QStringLiteral("no"); // yes/no/android, последний режим сохраняется до явного setter
    QString m_autoSocketProtocols = QStringLiteral("tcp+udp");
    QString m_dbPath; // абсолютный либо относительно каталога конфига; пустой save заменяет на chat_data
    QString m_debugFile; // путь журнала GUI
    QString m_debugLevel;
    QString m_debugCategories;
    QString m_language = QStringLiteral("system"); // system/en/ru
    QString m_theme = QStringLiteral("dark"); // Тема существующих и новых профилей по умолчанию.
    QStringList m_errors; // ошибки последней загрузки/подготовки, без секретных значений
};
#endif // NODE_CONFIG_H
