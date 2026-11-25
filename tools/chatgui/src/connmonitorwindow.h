/* Немодальное окно диагностики ETCP: список соединений и текстовые метрики выбранного узла.
 * Открывать через showMonitor(): возвращает существующее окно или создаёт новое без parent.
 * Указатель заимствован до close, которое удаляет окно (WA_DeleteOnClose). Фабрика гарантирует
 * один экземпляр-получатель глобальных callbacks gui_bridge для всех точек входа GUI.
 * Все методы работают в GUI-потоке; ядро собирает снимки асинхронно в uasync.
 * Таймер 100 мс запускается при создании/show и останавливается при hide. Выделенный текст
 * или нажатая кнопка мыши приостанавливают новые запросы, но не применение готовых ответов. */
#pragma once

#include <QDialog>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QTimer>
#include <cstdint>

class ConnMonitorWindow : public QDialog {
    Q_OBJECT
public:
    // Показать/поднять единственный монитор; повторный вызов не меняет его callbacks и выбор.
    static ConnMonitorWindow* showMonitor();
    ~ConnMonitorWindow() override;

    // Применить полный список [count:uint16][entry:46B]* (count<=250), native endian.
    // Неверная длина не меняет список/выбор. data заимствован на время вызова.
    void onConnList(const uint8_t* data, int len);
    // Показать len байт UTF-8 без требования завершающего NUL, сохранив позицию прокрутки.
    // data заимствован на время вызова; без выбранного узла ответ игнорируется.
    void onConnMetrics(const uint8_t* data, int len);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onTimerTick();
    void onConnSelected(QListWidgetItem* current, QListWidgetItem* previous);

private:
    ConnMonitorWindow(); // Создание принадлежит фабрике; окно само удаляется при закрытии.
    void requestMetricsForSelected();

    QListWidget*      m_connList; // Список узлов, Qt::UserRole содержит полный node_id; принадлежит окну.
    QPlainTextEdit*   m_details;  // Только чтение; выделение текста приостанавливает опрос.
    QTimer*           m_timer;   // Опрос каждые 100 мс; принадлежит окну.
    uint64_t          m_selectedNodeId = 0; // Узел для запросов метрик; 0 — выбор отсутствует.
};
