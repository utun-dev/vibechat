/* GUI-форма звуков уведомлений: общий enable/volume и восемь событий SoundManager.
 * После создания вызвать loadFromDb; applyAndSave применяет значения локально и асинхронно сохраняет через GUI bridge. */
#pragma once

#include <QWidget>

class DbManager;
class QCheckBox;
class QSlider;
class QLabel;

class SoundSettingsPage : public QWidget {
    Q_OBJECT
public:
    /* db заимствован до уничтожения формы; nullptr допустим, но loadFromDb тогда ничего не делает. */
    explicit SoundSettingsPage(DbManager* db, QWidget* parent = nullptr);

    /* GUI: заполнить контролы из ui_state (с defaults), не меняя SoundManager. */
    void loadFromDb();
    /* GUI: немедленно обновить SoundManager и передать мосту отдельные снимки key/value.
     * Отказ постановки освобождает запрос здесь, отмена — в мосте, выполнение — в callback ядра.
     * Возврат не подтверждает запись в БД; частичные ошибки логируются, локальные значения сохраняются. */
    void applyAndSave();

private:
    struct EventRow {
        const char* key;                     // Статическое имя события и суффикс ключей ui_state.
        const char* label;                   // Статический исходный текст для перевода.
        QCheckBox*   check = nullptr;        // Дочерний контрол включения события.
        QSlider*     slider = nullptr;       // Дочерний контрол громкости 0..100.
        QLabel*      volLabel = nullptr;     // Дочерняя подпись громкости в процентах.
    };

    DbManager*   m_db = nullptr;             // Заимствованный читатель БД, нужен только loadFromDb.
    QCheckBox*   m_enabledCheck;             // Общий enable звуков, дочерний виджет.
    QSlider*     m_volumeSlider;             // Общая громкость 0..100, дочерний виджет.
    QLabel*      m_volumeLabel;              // Общая громкость в процентах, дочерний виджет.
    EventRow     m_events[8];                // Имена и контролы восьми событий уведомлений.
};
