/* GUI-диалог настроек с категориями и общим Save: черновик редактируемых страниц загружается при создании.
 * Переключение категорий сохраняет черновик и обновляет диагностические страницы; виджетами владеет Qt-дерево диалога.
 * Save последовательно меняет INI/локальные настройки и ставит команды в мост: возможны частичные изменения при отказе.
 * Аватары и портал PTT имеют немедленные эффекты вне Save/Cancel; язык и listen-сокеты применяются после рестарта. */
#pragma once

#include <QDialog>
#include <QListWidget>
#include <QStackedWidget>
#include <QPushButton>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QString>

class QSpinBox;
class QLabel;
class BluetoothSettingsPage;
class NetworkSettingsPage;
class DatabaseSettingsPage;
class StatusPage;
class SoundSettingsPage;
class AudioDeviceSettingsPage;
class StorageSettingsPage;
class RadioSettingsPage;
class DbManager;
class UtunNode;
class AudioRecorder;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    /* configPath копируется; db/recorder заимствованы до уничтожения диалога, nullptr допустим.
     * node передаётся StatusPage через QPointer и может быть удалён раньше. Все вызовы — GUI. */
    explicit SettingsDialog(const QString& configPath, DbManager* db, UtunNode* node,
                            AudioRecorder* recorder, QWidget* parent = nullptr);
    bool requiresCoreRestart() const; // После успешного Save: изменился флаг скрытия сокетов.

private slots:
    void onCategoryChanged(int row); // Выбрать страницу; DB/status обновляются, редактируемый черновик сохраняется.
    /* Пройти запись/применение страниц и вызвать accept при завершении UI-шагов.
     * Команды владеют снимками значений; accept не подтверждает commit SQLite/применение ядром. Общей транзакции нет. */
    void onSave();
    // Запустить внешний редактор конфига; ошибки логируются/показываются, повторная загрузка — при создании нового диалога.
    void onEditConfig();

private:
    void addGroup(const QString& title); // Невыбираемый заголовок группы в навигации.
    void addPage(const QString& title, const QString& key, QWidget* page); // key — стабильный ID, независимый от позиции в меню.
    void loadProfileFromConfig(const QString& path); // Найти первую строку my_node_name=, заполнить поле и базу сравнения имени.

    QString               m_configPath; // Собственная копия пути INI.
    QListWidget*          m_categoryList; // Категории; UserRole хранит индекс страницы, UserRole+1 — стабильный ключ.
    QLabel*               m_pageTitle; // Заголовок текущей страницы над областью прокрутки.
    QStackedWidget*       m_pages; // Владение страницами через Qt-дерево, включая страницы с прокруткой.
    QPushButton*          m_saveBtn; // Общая кнопка Save, также default action диалога.
    QLineEdit*            m_nicknameEdit; // Черновик собственного отображаемого имени.
    QComboBox*            m_sortCombo; // Индекс сортировки: 0=алфавит, 1=последняя активность.
    QComboBox*            m_themeCombo; // dark/light; применяется после успешного Save без перезапуска.
    QComboBox*            m_languageCombo; // system/en/ru в item data; сохранённый язык для следующего запуска.
    QComboBox*            m_joinPolicyCombo; // Индекс join_policy: 0=авто, 1=спрашивать, 2=запрещать.
    QCheckBox*            m_autoConnectCheck; // Черновик group_autoconnect.
    QCheckBox*            m_publishCallStatus; // Разрешение публикации своего статуса звонка.
#ifdef Q_OS_LINUX
    QCheckBox*            m_startAtLoginCheck; // Черновик пользовательского XDG Autostart, только Linux.
    bool                  m_startAtLoginInitiallyEnabled; // Состояние файла при загрузке/последнем Save; лишних перезаписей нет.
#endif
    QSpinBox*             m_streamSubblocks; // Полное N RS-блока, 1..255; настройка следующей трансляции.
    QSpinBox*             m_streamRedundant; // Избыточные R подблоки, 0..N-1; maximum следует за N.
    QSpinBox*             m_streamStallTimeout; // Пауза поставщика до поиска резерва, мс; применяется при просмотре.
    QSpinBox*             m_streamMaxLiveLag; // Предел отставания содержимого до перехода на свежий KEY, мс.
    QSpinBox*             m_streamLimits[7]{}; // Бюджеты в порядке streamBudgetSettings; общий regular — первый.
    QString               m_oldNickname; // База сравнения имени: загрузка либо последняя попытка Save.
    NetworkSettingsPage*  m_networkPage; // Черновик listen-сокетов и LAN-флагов.
    DatabaseSettingsPage* m_databasePage; // Сведения/обслуживание SQLite, обновление при выборе категории.
    StatusPage*           m_statusPage; // Диагностика ядра/времени, обновление при выборе категории.
    SoundSettingsPage*      m_soundPage; // Черновик звуков событий.
    AudioDeviceSettingsPage* m_audioDevicePage; // Выбор устройств/аудиопараметров и тесты микрофона/динамика.
    StorageSettingsPage*     m_storagePage; // Черновик автозагрузки и лимитов media-хранилища.
    RadioSettingsPage*       m_radioPage; // Черновик активации рации и интерфейс PTT-хоткеев.
    BluetoothSettingsPage*   m_bluetoothPage;
};
