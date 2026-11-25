/* GUI-настройки рации: автоматика у кнопки VAD и отдельная системная вкладка передачи/PTT.
 * Редактор автоматики сразу сохраняет изменения; измерения/ошибки не заменяют редактируемые значения.
 * Вызывающий сохраняет hotkey() и doublePressEnabled() в ui_state radio_ptt_hotkey/radio_ptt_double_press.
 * Wayland-портал сохраняет назначения сам; видимая вкладка временно запрещает portal-PTT. */
#pragma once

#include "ptt_key_chord.h"
#include <QWidget>
#include <QMap>
#include <QTimer>

class DbManager;
class QPushButton;
class QLabel;
class QHideEvent;
class QShowEvent;
class QSpinBox;
class QCheckBox;
class QButtonGroup;
class RadioLevelMeter;

/* Настройки автоматики popup RadioPanel: изменения применяются без отдельного подтверждения. */
class RadioActivationSettings : public QWidget {
    Q_OBJECT
public:
    explicit RadioActivationSettings(QWidget* parent = nullptr);
    /* Загружает снимок движка; несохранённые изменения и текущая операция не перетираются. */
    void loadSettings();
private:
    QMap<QString, int> values() const; // Режим, пороги, усиление VAD и задержка окончания речи.
    bool eventFilter(QObject* object, QEvent* event) override; // Откладывает применение порогов до отпускания мыши или закрытия popup.
    void settingsEdited(); // Обновляет шкалы; при перетаскивании сохраняется только последнее значение после отпускания.
    void applyPending(); // Отправляет изменения, когда ядро свободно и мышь отпущена; ошибка не вызывает бесконечного повторения.
    void updateControls(); // Проверяет пороги/ошибку и обновляет отметки шкал, не загружает заново настройки.
    void updateMeters(); // Только текущие измерения/подписи из движка, без изменения черновика.
    bool m_loaded = false, m_pending = false; // Готовность полей и изменения, ещё не переданные движку.
    bool m_dragging = false; // Перетаскивается порог одной из шкал: захват микрофона продолжает работать с прежними настройками.
    QTimer m_applyTimer; // Объединяет ввод чисел за 100мс; отпускание маркера планирует применение без задержки.
    QButtonGroup* m_mode; // Дочерняя группа: 1=VAD, 2=уровень, 3=VAD + уровень.
    QSpinBox *m_threshold, *m_hangover, *m_levelOn, *m_levelOff; // %, мс, on/off dBFS; усиление VAD — в общих аудионастройках.
    RadioLevelMeter *m_levelMeter, *m_vadMeter; // Дочерние шкалы уровня/вероятности с отметками черновых порогов.
    QLabel *m_levelText, *m_vadText, *m_gainText, *m_error; // Дочерние подписи измерений и ошибки.
};

/* Системная вкладка: параметры передачи и одно PTT-сочетание Windows/X11 либо системное назначение Wayland. */
class RadioSettingsPage : public QWidget {
    Q_OBJECT
public:
    /* db заимствуется, может быть nullptr; нужна только при loadFromDb. Менеджер hotkeys должен быть инициализирован отдельно. */
    explicit RadioSettingsPage(DbManager* db, QWidget* parent = nullptr);
    /* Отменяет запись клавиш; видимая portal-вкладка снимает свой запрет portal-PTT. */
    ~RadioSettingsPage() override;
    /* Перезагружает параметры передачи, флаг фиксации и сочетание из БД, отменяя запись; portal перечитывает подпись. */
    void loadFromDb();
    /* Кодированное физическое сочетание; в режиме portal — пустая строка. */
    QString hotkey() const;
    /* Черновик фиксации двойным нажатием только для хоткея; кнопочный PTT не меняет. */
    bool doublePressEnabled() const;
    /* Сначала сохраняет TX-компрессор/усиление и размер пачки; затем применяет фиксацию и сочетание в памяти менеджера.
     * БД не сохраняет, завершения uasync-настроек не ждёт; false оставляет хоткей неприменённым. */
    bool apply();

protected:
    bool eventFilter(QObject* object, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void updatePortalLabels(); // Описание/статус и доступность системной кнопки, только portal-ветка.
    void loadAudioSettings(); // Загружает параметры передачи; по stateChanged вызывается только для первого готового снимка.
    bool m_audioLoaded = false; // Получен первый снимок системных настроек передачи.
    QSpinBox *m_txGain, *m_burstFrames; // Усиление передачи в dB и 1..10 Opus-кадров в пачке.
    QCheckBox* m_compressor; // Общий переключатель компрессора передачи и VAD.
    QCheckBox* m_aec; // Независимое от звонка эхоподавление рации.
    QWidget* m_audioSettings; // Контейнер системных параметров передачи, блокируется на время сохранения.
    QLabel* m_audioError; // Ошибка чтения/сохранения настроек движка.
    QLabel* m_portalStatus = nullptr; // Дочерняя подпись; ненулевая только в portal-ветке.
    void startCapture(); // Отменяет прежнюю запись и начинает запись единственного сочетания.
    void finishCapture(bool save); // Сохраняет candidate только в черновик либо отменяет; снимает eventFilter/режим записи.
    void updateLabel(); // Показывает физическое сочетание через менеджер.
    DbManager* m_db; // Заимствованная БД, записью hotkeys владеет SettingsDialog.
    PttKeyChord m_chord; // Черновик единственного физического сочетания; пустой для portal.
    QPushButton* m_button; // Дочерняя кнопка записи либо системного назначения.
    QLabel* m_label; // Дочерняя подпись сочетания.
    QCheckBox* m_doublePress; // Фиксация только хоткея по двойному нажатию, по умолчанию включена.
    PttChordCapture m_capture; // Самый большой одновременный снимок текущей физической записи.
    bool m_capturing = false; // Активна запись физического сочетания.
};
