/* Страница диагностики настроек: ручной снимок NTP/ETCP и часы с поправкой UtunNode.
 * Все методы и callbacks работают в GUI. Одновременно используется одна страница:
 * конструктор регистрирует её как получателя глобального callback статуса gui_bridge.
 * refreshStatus запускает часы и асинхронный запрос в uasync; готовый текст копируется в Qt.
 * Без работающего ядра показывает явное состояние, поздние ответы после stop игнорируются.
 * Часы обновляются по границе секунды до уничтожения страницы, в том числе при скрытой странице. */
#pragma once

#include <QWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QFrame>
#include <QTimer>
#include <QPointer>

class UtunNode;

class StatusPage : public QWidget {
    Q_OBJECT
public:
    // node заимствован через QPointer; может быть NULL/удалён раньше страницы. parent владеет виджетами.
    explicit StatusPage(UtunNode* node, QWidget* parent = nullptr);
    ~StatusPage() override;
    void refreshStatus(); // Запросить снимок активного ядра; при недоступности не оставлять Loading.
    void setText(const QString& text); // Применить ответ живого ядра; text копируется в документ Qt.

private slots:
    void onRefreshClicked();

private:
    void updateClock();

    QPlainTextEdit* m_textEdit;   // Только чтение; полный текст последнего снимка/состояния ядра.
    QPushButton*    m_refreshBtn; // Ручной запрос снимка.
    QLabel*         m_clockLabel; // Локальное время с NTP-поправкой, если узел ещё существует.
    QFrame*         m_clockFrame; // Рамка часов; принадлежит странице.
    QTimer*         m_clockTimer; // Одноразовый precise timer до следующей секунды; принадлежит странице.
    QPointer<UtunNode> m_node;    // Автоматически обнуляется при удалении владельца ядра.
};

