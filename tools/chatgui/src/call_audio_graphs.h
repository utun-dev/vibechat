/* Отдельное окно истории звонка. Только GUI-поток; native отдаёт независимые копии.
 * Таймер работает лишь при видимом окне. После завершения звонка сохраняется последний снимок. */
#pragma once
#include <QWidget>
#include "../../../src/call/call_audio_history.h"
class QTimer;
class CallAudioGraphCanvas;
class CallAudioGraphs : public QWidget {
public:
    explicit CallAudioGraphs(quint64 callId, QWidget* diagnostics, QWidget* parent = nullptr);
    void setSnapshot(const call_audio_history& snapshot); // Копирует готовый native-снимок, не читает живые очереди.
protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
private:
    quint64 m_callId;
    QTimer* m_timer;
    CallAudioGraphCanvas* m_canvas; // Графики слева; текстовый diagnostics передаётся правой панели во владение.
};
