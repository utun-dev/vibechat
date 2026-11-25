/* GUI-страница устройств, Opus, компрессора, AEC/NS, jitter/видеобитрейта звонка и Opus рации.
 * После создания вызвать loadFromDb; форма применяется только через applyAndSave.
 * Оба теста используют выбранные устройства; тест динамика открывает независимый короткий playback.
 * INI сохраняется синхронно, ui_state/live chat_setting — асинхронно собственными снимками мосту. */
#pragma once
#include <QWidget>
#include <QComboBox>
#include <QProgressBar>
#include <QPushButton>
#include <QCheckBox>
#include <QSlider>
#include <QLabel>
#include <QTimer>
#include <memory>
class AudioDevice;
class DbManager;
class AudioRecorder;

class AudioDeviceSettingsPage : public QWidget {
    Q_OBJECT
public:
    /* Создаёт форму без чтения БД. db/recorder заимствованы и могут быть nullptr; recorder применяет NS/компрессор следующей записи. */
    explicit AudioDeviceSettingsPage(DbManager* db, AudioRecorder* recorder, QWidget* parent = nullptr);
    /* Секции единого медиа-черновика; dialog размещает их в разделах звонков и рации через Qt-layout.
     * Должны жить до завершения applyAndSave; controller и секции принадлежат одному диалогу. */
    QWidget* callPage() const { return m_callPage; }
    QWidget* radioCodecPage() const { return m_radioCodecPage; }
    ~AudioDeviceSettingsPage() override; // Останавливает тестовый capture и ждёт его worker перед удалением.
    void setConfigPath(const QString& path) { m_configPath = path; } // Путь INI для apply; пустой отключает запись файла.
    void loadFromDb(); // Перечисляет устройства и читает ui_state/defaults; отсутствующий ID устройства заменяется выбором auto.
    /* Применяет устройства/пресет и параметры следующей записи recorder, ставит снимки ui_state в очередь ядра.
     * Ошибка первой записи INI (диапазон видео) прерывает apply с предупреждением; остальные INI-ошибки логируются отдельно.
     * Числовые настройки отправляются в live chat_setting после сохранения INI либо сразу при пустом пути файла.
     * Не является транзакцией; возврат не подтверждает запись в БД или применение всех настроек ядром. */
    void applyAndSave();

private slots:
    void onTestMicToggled(); // Включает отдельный meter-only AudioRecorder на выбранном входе либо останавливает его.
    void onTestSpeaker(); // Открывает выбранный выход для тона 440 Гц/250 мс; повтор останавливает тест.
    void onLevelTimer(); // Читает RMS теста, отображает -70..0 dB как 0..100; завершает тест при остановке рекордера.

protected:
    void hideEvent(QHideEvent* event) override; // Остановить оба теста при уходе со страницы.
private:
    void stopSpeakerTest(); // Закрыть независимый тестовый выход до сброса context/уничтожения формы.
    QWidget *m_callPage, *m_radioCodecPage; // Секции единого черновика в Qt-дереве диалога.
    std::unique_ptr<AudioDevice> m_speakerTest; // Тестирует выбранный выход, не меняя SoundManager.
    QTimer* m_speakerTimer = nullptr; // Завершает короткий тест динамика.

    void stopMicTest(); // Останавливает таймер, освобождает тестовый recorder, сбрасывает кнопку и индикатор; повтор безопасен.
    void buildCompressorUI(class QVBoxLayout* layout); // Добавляет шумоподавление и компрессор с раскрываемыми подробностями.
    void updateCompressorLabels(); // Показывает усиление, времена анализа/упреждения, скорость роста и целевой уровень.

    DbManager* m_db; // Заимствованный источник ui_state; nullptr оставляет defaults/предыдущий снимок формы.
    AudioRecorder* m_recorder = nullptr; // Заимствованный рекордер сообщений; должен жить до последнего applyAndSave.
    AudioRecorder* m_testRecorder = nullptr; // Собственный QObject-child только на время теста микрофона.
    QString m_configPath; // Копия пути INI; чтение формы всегда идёт из ui_state, не из файла.
    QComboBox* m_outputCombo; // itemData: backend ID выхода; пусто — auto. Все элементы/таймер принадлежат странице.
    QComboBox* m_inputCombo; // itemData: backend ID входа; пусто — auto.
    QComboBox* m_codecPresetCombo; // itemData: пресет голосовых сообщений 0/1/2.
    QProgressBar* m_micLevelBar; // Отображение RMS теста, без сохранения звукового сообщения.
    QPushButton* m_testMicBtn; // Checkable: запрошен тест микрофона.
    QPushButton* m_testSpeakerBtn; // Короткий тест выбранного выхода; повтор останавливает тон.
    QTimer* m_levelTimer; // GUI: опрос тестового рекордера каждые 25 мс.
    bool m_micTestActive = false; // Страница запустила тест; не означает доступность аппаратного входа.
    QString m_savedOutputId; // Ниже saved*: последний load/apply, не подтверждение асинхронного сохранения.
    QString m_savedInputId; // Пустой ID означает auto.
    int m_savedCodecPreset = 1; // Стандартный пресет Opus.

    QCheckBox* m_compressorEnabledCb; // Разрешение компрессора для следующей записи.
    QSlider* m_compressorMaxGainSlider; // Gain 0..50 dB.
    QSlider* m_compressorLookbackSlider; // Окно анализа 50..500 мс.
    QSlider* m_compressorLookaheadSlider; // Упреждение 20..500 мс.
    QSlider* m_compressorRiseRateSlider; // Rise rate 2..10 x/sec.
    QSlider* m_compressorTargetLevelSlider; // Целевой уровень -40..0 dBFS.
    QCheckBox* m_compressorSpeechOnlyCb; // Общий VAD: повышать gain только при речи, PCM и снижение gain не блокировать.
    class QSpinBox* m_compressorVadGain; // Общий предел собственного компрессора перед VAD, 1..60 dB.
    QLabel* m_compressorMaxGainLabel; // Текущее значение ползунка gain.
    QLabel* m_compressorLookbackLabel; // Текущее окно анализа, мс.
    QLabel* m_compressorLookaheadLabel; // Текущее упреждение, мс.
    QLabel* m_compressorRiseRateLabel; // Текущее значение ползунка rise rate.
    QLabel* m_compressorTargetLevelLabel; // Текущий целевой уровень, dBFS.
    int m_savedCompressorEnabled = 0; // Снимок разрешения компрессора.
    int m_savedCompressorMaxGainDb = 20; // Снимок gain, dB.
    int m_savedCompressorLookbackMs = 200; // Снимок окна анализа, мс; хранится только в ui_state.
    int m_savedCompressorLookaheadMs = 100; // Снимок упреждения, мс; хранится только в ui_state.
    int m_savedCompressorRiseRate = 10; // Снимок rise rate, x/sec.
    int m_savedCompressorTargetLevelDb = 0; // Снимок целевого уровня, dBFS; хранится только в ui_state.
    int m_savedCompressorSpeechOnly = 0; // Снимок общей опции роста gain только при речи.
    int m_savedCompressorVadGain = 10; // Сохранённый предел служебного AGC VAD.

    class QDoubleSpinBox* m_jitterReserveSpin; // Максимальный jitter reserve 0.5..2.0 сек; сохраняется в миллисекундах.
    class QSpinBox* m_videoMinimumSpin; // Нижняя граница видеобитрейта звонка, kbit/s; не выше maximum.
    class QSpinBox* m_videoMaximumSpin; // Верхняя граница, kbit/s; диапазон сохраняется как minimum:maximum.
    QCheckBox* m_aecEnabledCb; // Разрешение AEC звонков; не применяется к тесту микрофона.
    int m_savedAecEnabled = 1; // Снимок AEC.
    QCheckBox* m_noiseSuppressionEnabledCb; // Полный обход NS при выключении, включая его задержку.
    class QSpinBox* m_noiseSuppressionSpin; // Предел спектрального подавления -40..-1 dB.
    int m_savedNoiseSuppressionEnabled = 1; // NS по умолчанию включён для звонков/рации/записи.
    int m_savedNoiseSuppressionDb = -20; // Начальное подавление -20 dB.

    QComboBox* m_radioOpusModeCombo; // itemData: VOIP=0, AUDIO=1.
    QComboBox* m_radioOpusBitrateCombo; // itemData: 16/32/64/96/128 kbit/s.
    QCheckBox* m_radioCaptureStereoCb; // Stereo capture рации; воспроизведение всегда stereo.
    int m_savedRadioOpusMode = 0; // Снимок режима Opus рации.
    int m_savedRadioOpusBitrate = 32; // Снимок битрейта, kbit/s.
    int m_savedRadioCaptureStereo = 0; // Снимок stereo capture, 0/1.
};
