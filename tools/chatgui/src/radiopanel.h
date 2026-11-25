/* GUI-панель активной рации, сама аудиосессию не запускает: отображает singleton RadioAudioEngine.
 * После значков устройств показывает ники говорящих из MemberListModel: зелёные слышимые, красные приглушённые.
 * Замок общий с хоткеем в RadioAudioEngine: двойной левый клик фиксирует, следующее нажатие любого источника снимает фиксацию.
 * Короткий клик автоматики включает/выключает выбранный VAD/уровень; удержание 500мс либо правый клик открывают редактор.
 * Устройства общие через SoundManager, их ID сохраняются через core ui_state; режим/пороги — через настройки движка. */
#pragma once

#include <QWidget>
#include <QTimer>
#include <QPointer>

class QMenuBar;
class QPushButton;
class QLabel;
class QMenu;
class RadioActivationSettings;
class MemberListModel;

class RadioPanel : public QWidget {
    Q_OBJECT
public:
    /* Создаёт дочерние контролы и подписки на stateChanged/devicesChanged, сразу обновляет снимок UI. */
    explicit RadioPanel(QWidget* parent = nullptr);
    void setMembers(MemberListModel* members); // Заимствованная модель: ники, речь и подтверждённый mute; NULL снимает привязку.
    /* Снимает только удержание кнопки; общая фиксация принадлежит движку и переживает уничтожение панели. */
    ~RadioPanel() override;
private:
    /* Различает удержание автоматики и фиксацию PTT; скрытие/деактивация кнопки автоматики отменяют только её таймер. */
    bool eventFilter(QObject* watched, QEvent* event) override;
    void updateState(); // Готовность контролов, ошибки, режим и фактический TX; VAD-кнопка рисует текущую вероятность речи.
    void showActivationSettings(); // Лениво создаёт popup; настройки сохраняются автоматически, в том числе при закрытии.
    void updateDevices(); // Обновляет tooltip/accessibility из выбранных ID SoundManager.
    void updateSpeakers(); // Ники слышимых/приглушённых говорящих и общий индикатор речи; вызывается по изменениям модели.
    void updateDeviceIcons(); // Микрофон и динамик; во время речи динамик зелёный, с волной и более толстым контуром.
    void changeEvent(QEvent* event) override; // Перерисовывает значки при смене палитры.
    void populateDevices(QMenu* menu, bool capture); // Перечитывает список при открытии меню; пустой ID — default.
    void selectDevice(const QString& id, bool capture); // Планирует сохранение ui_state и сразу меняет общие устройства.
    QMenuBar* m_devices; // Общая навигация двух меню: наведение, стрелки и Esc, как в трансляции.
    QMenu *m_input, *m_output; // Дочерние меню устройств ввода/вывода.
    QPushButton *m_auto, *m_ptt; // Автоматика с измерителем VAD и кнопочный PTT.
    QLabel* m_status; // Ошибка/состояние рации с tooltip подробностей настроек.
    QPointer<MemberListModel> m_members; // Не владеет моделью участников выбранного канала.
    QString m_state; // Текущая ошибка/состояние; обычные приём и передача не подписываются словами.
    bool m_speaking = false; // Есть говорящий участник либо собственный фактический TX.
    QTimer m_autoHold; // Однократный порог долгого нажатия автоматики, 500мс.
    bool m_autoLongPressed = false; // Подавляет обычное переключение при отпускании после долгого нажатия.
    QMenu* m_activationMenu = nullptr; // Дочерний popup, создаётся при первом открытии.
    RadioActivationSettings* m_activationSettings = nullptr; // Компактный редактор, принадлежит содержимому popup.
};
