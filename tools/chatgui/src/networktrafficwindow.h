/* Отдельное окно сетевого трафика: по три графика на ETCP_CONN, одна легенда для inflight/TX/RX.
 * Только GUI-поток; живые ETCP-объекты здесь не читаются. Закрытие удаляет окно, скрытие останавливает опрос. */
#pragma once

#include <QDialog>
#include <QMap>
#include <cstdint>

class QLabel;
class QTimer;
class QVBoxLayout;
class TrafficConnection;

class NetworkTrafficWindow : public QDialog {
public:
    // Показать/поднять единственный экземпляр; он владеет независимым callback моста.
    static NetworkTrafficWindow* showMonitor();
    ~NetworkTrafficWindow() override;
    // Копировать и проверить целиком снимок chat_network_stats.h; data заимствован на время вызова.
    void onSnapshot(const uint8_t* data, int len);
protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void closeEvent(QCloseEvent*) override;
private:
    NetworkTrafficWindow();
    void onTimerTick();
    QTimer* m_timer; // Опрос каждые 250 мс только при видимом окне.
    QVBoxLayout* m_rows; // Вертикальные панели внутри scroll area, принадлежат окну.
    QLabel* m_empty; // Ожидание, отсутствие подключений или ошибка сбора.
    QMap<quint64, TrafficConnection*> m_connections; // Панели по lifetime ID, а не по номеру строки/peer_id.
    uint64_t m_requestId = 0; // Последний принятый мостом запрос; старые ответы игнорируются.
};
