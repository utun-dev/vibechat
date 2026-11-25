/* Настройки media-хранилища: автозагрузка, лимит файла и общий размер.
 * GUI: loadFromDb читает черновик, applyAndSave передаёт снимки ui_state мосту и сохраняет INI.
 * При заданном INI в ядро ставится live-обновление только autoload; лимиты размера используются после перезапуска. */
#pragma once

#include <QWidget>

class DbManager;
class QCheckBox;
class QSpinBox;

class StorageSettingsPage : public QWidget {
    Q_OBJECT
public:
    /* db заимствован и живёт дольше страницы; nullptr допустим. Создание не загружает сохранённые значения. */
    explicit StorageSettingsPage(DbManager* db, QWidget* parent = nullptr);

    /* Сохранить путь для последующего apply; пустой путь оставляет только запросы ui_state. */
    void setConfigPath(const QString& path) { m_configPath = path; }
    /* Заполнить форму из ui_state: defaults autoload=1, файл=10MB, всего=1GB; spinbox ограничивают значения своим диапазоном.
     * При db==nullptr ничего не меняет. INI и текущее состояние ядра здесь не читаются. */
    void loadFromDb();
    /* Передать три снимка ui_state; при заданном пути последовательно сохранить три INI-ключа.
     * Live autoload ставится после успешной записи его INI-ключа. Частичные ошибки логируются, остальные ключи обрабатываются.
     * Асинхронные запросы принадлежат мосту, включая отмену; возврат не подтверждает запись SQLite/применение в ядре. */
    void applyAndSave();

private:
    DbManager*  m_db = nullptr; // Заимствованный читатель ui_state, нужен loadFromDb.
    QString     m_configPath; // Собственная копия пути INI, пусто — без файловой записи/live autoload.
    QCheckBox*  m_autoloadCheck; // Дочерний контрол автозагрузки.
    QSpinBox*   m_autoloadMaxSize; // Дочерний контрол лимита одного файла, 1..500 MB.
    QSpinBox*   m_maxStorageSize; // Дочерний контрол общего лимита, 1..100 GB.
};
