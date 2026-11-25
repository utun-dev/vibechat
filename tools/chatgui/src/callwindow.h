/* GUI-представление одного звонка. Владелец задаёт ID/имя/состояние и удаляет окно по closed (обычно deleteLater).
 * Все методы/сигналы — GUI-поток. CallVideoEngine и отдельное окно видео принадлежат этому QWidget;
 * аудио обслуживает общий CallAudioEngine, его native lifecycle координирует MainWindow. */
#pragma once

#include <QWidget>
#include <QImage>
#include <cstdint>
#include "video_capture.h"

class QLabel;
class QPushButton;
class QMenu;
class QMenuBar;
class QTimer;
class QSpinBox;
class CallVideoEngine;
class CallVideoWindow;
class CallLocalPreview;
class CallAudioGraphs;

/* GUI-окно звонка: Outgoing (соединение), Ringing (доставлен вызов), Incoming (принять/отклонить),
 * Active (разговор/таймер), Ended (завершён), WaitDecision (явное разрешение звонить занятому телефону).
 * Команды передаются через typed API моста без заимствованного instance/heap payload в GUI.
 * Кнопки при отказе постановки сохраняют состояние и показывают ошибку.
 * Close переводит локальное состояние в Ended, останавливает видео/таймер и отправляет Decline для Incoming,
 * Hangup для остальных живых состояний; повторный close/закрытие Ended не отправляет команду.
 * Окно закрывается и при отказе очереди с WARN; завершение native-сессии подтверждают события ядра. */
class CallWindow : public QWidget {
    Q_OBJECT
public:
    enum State { Outgoing, Ringing, Incoming, Active, Ended, WaitDecision, Preparing, Answering };

    explicit CallWindow(QWidget* parent = nullptr); // Создаёт UI и дочерний видеодвижок, но не native-звонок.
    ~CallWindow() override; // Снимает video callbacks и останавливает движок; native-команда отправляется только через closeEvent/кнопки.

    void setCallId(quint64 id) { m_callId = id; } // Ненулевой ID до действий пользователя; setter сам не запускает сессию.
    quint64 callId() const { return m_callId; } // ID для команд моста и сигнала closed.

    void setPeerName(const QString& name); // Имя, буквенная аватарка и заголовок отдельного видеоокна.
    void setBgpPath(const QString& path); // Текст пути; показывается в диагностике Active при непустой строке.
    void setConnectionType(uint8_t type, const QByteArray& nextHop); // 0/1/2/3 — нет/direct/reverse/indirect; nextHop в tooltip только для 3.
    // rtt/buffer/minimum/maximum/reserve — мс; tempo=100 для 1x, dropped/underruns — счётчики; min/max за 10с.
    void setAudioStats(int rtt, int buffer, int tempo, int dropped, int underruns, int minimum, int maximum, int reserve);
    void setState(State s); // Локальное GUI-состояние; каждый Active сбрасывает длительность/mute, Ended останавливает видео/таймер.
    void setMediaWarning(const QString& text); // Подпись последствий принятия; пустая скрывает её.
    State state() const { return m_state; } // Текущее локальное состояние представления.

signals:
    void closed(quint64 callId); // После локальной очистки/постановки команды; удаления QWidget и ожидания native-ответа здесь нет.

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onAccept(); // После принятия команды мостом ждёт CALL_ACCEPTED; локальный ответ не запускает разговор.
    void onDecline(); // Ставит Decline; закрытие по native-результату выполняет MainWindow.
    void onHangup(); // Ставит Hangup; при отказе сохраняет окно и показывает ошибку.
    void updateDuration(); // Только Active: прошедшее wall-clock время в минуты:секунды.
    void onMuteToggled(bool checked); // Меняет mute общего аудиодвижка, иконку и подпись.
    void onDeviceChanged(); // Применяет backend ID выбранного аудиовыхода m_output.

private:
    void updateButtons(); // Видимость действий по State, затем общая раскладка видео.
    void populateOutputDevices(); // Выходы SoundManager: пустой ID — default, текущий выбор из CallAudioEngine.
    void refreshVideoModes(); // Уникальные размеры выбранной камеры, затем доступные FPS; сохраняет текущий допустимый режим.
    void updateVideoMenuButtons(); // Текущие camera/codec/resolution/FPS/bitrate/output внутри кнопок, полные названия в подсказках.
    void applyVideo(); // Применяет выбранную камеру/режим; без доступного режима отправку видео отключает.
    void updateVideoPresentation(); // Отдельное remote-видео, локальный preview, настройки и диагностические подписи.
    CallVideoEngine* m_video; // Дочерний TX/RX/показ; start по таймеру 100мс только при Active и совпадающем активном аудиозвонке.
    QPushButton* m_videoEnabled; // Круглая checkable-кнопка отправки видео; наличие camera mode проверяется отдельно.
    QMenuBar* m_videoMenuBar; // Общий стиль трансляции: кнопки 48x48, значки 40x36, переключение раскрытых меню одним кликом.
    QMenu *m_camera, *m_resolution, *m_fps, *m_codec, *m_bitrate, *m_output; // checked QAction/data хранит выбранный вариант.
    VideoCaptureDevices m_cameras; // Последний полный снимок worker; режимы не переносятся между разными ID камер.
    QList<StreamVideoMode> m_modes; // Допустимые пары resolution/FPS текущей камеры.
    QSpinBox *m_videoMin, *m_videoMax; // Диапазон 300..4000 кбит/с; native port получает бит/с.
    CallLocalPreview* m_localPreview; // Локальный кадр занимает свободную область между верхней панелью и кнопками, сохраняя пропорции.
    QLabel* m_videoStatus; // Статус/ошибка видеодвижка.
    QWidget *m_videoControls, *m_panel; // Сворачиваемые настройки и центральная панель шириной до 640.
    CallVideoWindow* m_videoWindow; // Дочернее Qt::Window; закрытие скрывает картинку, звонок продолжает работать.
    QPushButton *m_settingsBtn, *m_showVideoBtn; // Раскрытие настроек и повторное открытие remote-видео.
    QPushButton* m_diagnosticsBtn; // Открывает общее окно графиков и текстовой диагностики.
    QWidget* m_diagnostics; // Текст: при открытии передаётся правой панели окна диагностики, данные обновляются и в скрытом окне.
    CallAudioGraphs* m_audioGraphs = nullptr; // Ленивое дочернее окно; сохраняет последний снимок после звонка.
    QImage m_remoteImage, m_localImage; // Последние shared-кадры; видеодвижок очищает их при stop.
    bool m_receivingVideo = false; // Active + непустой remote-кадр; не признак видимости отдельного окна.
    quint64 m_videoCall = 0; // ID, с которым запущен видеодвижок; 0 после Ended/close.
    bool m_videoRangeReady = false; // Начальный native bitrate range уже прочитан после start видео.

    quint64 m_callId = 0; // ID UI-сессии, по нему MainWindow индексирует окна.
    State   m_state = Outgoing; // Локальное состояние окна, не подтверждение готовности native-транспорта.
    qint64  m_startMs = 0; // Wall-clock QDateTime в мс на последнем переходе Active.

    QLabel*      m_avatarLabel; // Буквенная аватарка 80×80.
    QLabel*      m_peerLabel; // Имя собеседника.
    QLabel*      m_statusLabel; // Состояние вызова либо ошибка capture/постановки команды.
    QLabel*      m_mediaWarning = nullptr; // Независима от статуса дозвона.
    QLabel*      m_durationLabel; // Минуты:секунды с последнего Active.
    QLabel*      m_bgpPathLabel; // BGP-путь, скрыт вне Active.
    QLabel*      m_connectionLabel; // Тип соединения и relay tooltip.
    QLabel*      m_statsLabel; // Буфер/tempo/RTT/счётчики и доступная диагностика output latency.
    QPushButton* m_acceptBtn; // Accept для Incoming; Active устанавливается после принятия команды мостом.
    QPushButton* m_continueBtn; /* явное разрешение начать вызов занятого телефона */
    QPushButton* m_declineBtn; // Decline для Incoming; результат закрывает окно через MainWindow.
    QPushButton* m_hangupBtn; // Завершение остальных живых состояний.
    QPushButton* m_muteBtn; // Checkable mute общего CallAudioEngine, виден при Active.
    QTimer*      m_durationTimer; // GUI 1с; запускается в Active, останавливается в Ended/close.

    QWidget* m_acceptCell; // Кнопка Accept и её подпись, показываются вместе.
    QWidget* m_declineCell; // Кнопка Decline и подпись.
    QWidget* m_hangupCell; // Кнопка Hangup и подпись.
    QWidget* m_muteCell; // Кнопка mute и подпись.
    QWidget* m_videoCell; // Кнопка камеры и подпись справа от mute в общей центрированной строке.
    QWidget* m_settingsCell; // Шестерёнка настроек видео и подпись справа от камеры.
    QWidget* m_diagnosticsCell; // Кнопка диагностики и подпись справа от настроек в нижней строке действий.
    QLabel*  m_muteCaption; // Меняется между Mute/Unmute.
};
