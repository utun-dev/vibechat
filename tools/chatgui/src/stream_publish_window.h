/* GUI управления StreamPublishEngine: окно не владеет engine, он должен жить дольше окна (в MainWindow это отдельный объект).
 * Закрытие/Esc скрывает окно и сообщает closed, публикация продолжает работать. Повторное открытие MainWindow создаёт новое окно.
 * settings — компактный JSON пользовательского выбора; здесь нет БД, сохранение выполняет владелец по settingsChanged.
 * Пока engine активен, UI синхронизируется с wanted config; start/stop/reconfigure подтверждаются асинхронно.
 * Видимое окно включает локальное превью выбранной камеры; скрытие/уничтожение освобождает её при отсутствии публикации.
 * Отсутствие HW encoder открывает немодальный диалог согласия на CPU для поколения запроса; отказ/удаление диалога отвечает false. */
#pragma once
#include <QWidget>
#include <QJsonObject>
#include "stream_publish_engine.h"
class QLabel;
class QPushButton;
class QCheckBox;
class QMenu;
class QMenuBar;
class QToolButton;
class QAction;
class QCloseEvent;
class StreamBufferIndicator;
class StreamPublishWindow : public QWidget {
    Q_OBJECT
public:
    // engine обязателен; group используется при start, действующий actor остаётся в своей группе. settings восстанавливаются только до start.
    StreamPublishWindow(StreamPublishEngine* engine, const QString& group, QWidget* parent = nullptr, const QString& settings = {});
    ~StreamPublishWindow() override; // Снимает запрос локального превью; активная публикация продолжает работать.
signals:
    void settingsChanged(const QString& settings); // Новый компактный JSON, только при изменении выбора; runtime handles не сохраняются.
    void microphoneChanged(const QString& id); // Новый общий выбор SoundManager; владелец сохраняет audio_input_id в ui_state.
    void closed(); // Принят closeEvent; MainWindow использует сигнал для возврата фокуса в grid, без остановки публикации.
protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
private:
    StreamPublishEngine* m_engine; // Заимствован; QWidget-дерево владеет только дочерними controls.
    QString m_group; // Группа для следующего запуска из этого окна.
    QList<StreamVideoSource> m_sources; // Текущие camera/screen; data пунктов m_source содержит индекс источника.
    QList<StreamVideoMode> m_modes; // Допустимые пары resolution/FPS выбранного источника.
    QMenuBar* m_menuBar; // Верхний ряд: Qt переключает раскрытые меню одним кликом, повторный клик закрывает меню.
    QMenu *m_source, *m_resolution, *m_fps, *m_codec, *m_bitrate, *m_rotation; // Выбор хранится в checked QAction/data.
    QMenu *m_microphone, *m_maxQp; // Аудиоисточник (включая «Без звука») и шесть вариантов QP: auto/18/23/28/35/40.
    QAction* m_noAudio; // Пункт без захвата микрофона; остальные пункты m_microphone содержат device id, пустой — default.
    QToolButton* m_sound; // Checked означает передачу звука; mute меняется без перезапуска активной трансляции.
    QCheckBox* m_cursor; // Cursor portal можно менять только до start.
    QCheckBox* m_adaptiveQp; // Доступен при явном maxQp; engine проверяет поддержку live QP у encoder.
    QLabel *m_status, *m_stats, *m_inputChannels; // Статус/ошибка, bitrate/QP/очереди/ACK и фактические входные каналы.
    QPushButton* m_start; // Start/Stop; отключена во время завершения actor либо при отсутствии доступных source/mode/codec.
    StreamBufferIndicator* m_buffer; // Диапазоны получателей по истории source: oldest → live; tooltip содержит peer/subflows.
    QWidget* m_preview; // VideoRenderWidget с CPU QImage при HAVE_STREAM_CAPTURE, иначе текст о недоступности capture.
    bool m_updating = false; // Защита от применения/сохранения промежуточного состояния при перестроении controls.
    bool m_audioMuted = false; // Сохранённый mute для следующего запуска; во время трансляции синхронизируется с engine.
    QJsonObject m_settings; // Последний сохранённый выбор; source id в base64, окно по имени, режим/codec/audio/cursor/QP отдельно.
    StreamPublishConfig selection() const; // Снимок controls: auto bitrate рассчитан, screen nativeSize и допустимость QP учтены.
    void refreshSources(); // Сохраняет выбранный handle; пропавший active-source не подменяется другим, до start возможен первый доступный.
    // Режимы camera/screen; выбирает близкий previous либо native screen.
    void refreshModes(const StreamPublishConfig& previous, bool maximum = false);
    void updateMenuButtons(); // Обновляет иконки со значениями и подсказки верхнего ряда после выбора/синхронизации.
    void refreshCodecs(); // Перечитывает engine.codecs(), сохраняет текущий/saved codec, недоступный заменяет первым.
    void refreshMicrophones(); // Перечитывает устройства и общий с рацией выбор; «Без звука» относится только к трансляции.
    void synchronize(); // Читает engine config/state без apply; настраивает controls и ограничения изменения audio/cursor/QP.
    void updatePreview(); // Передаёт видимость и выбор камеры; экран/окно не захватываются до явного start.
    void apply(); // Для выбранного режима: reconfigure только active-engine, затем сохранение выбора; не во время m_updating.
    void restoreSettings(); // До start восстанавливает доступные значения JSON; некорректные/отсутствующие диагностируются и заменяются.
    void saveSettings(); // Сериализует пользовательские настройки без runtime handles/вычисленного auto bitrate и эмитит лишь изменение JSON.
};
