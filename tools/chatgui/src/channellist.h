/* Список каналов и PM с unread/online; все методы и события используются в GUI-потоке.
 * DbManager заимствуется через QPointer; setDb/закрытие SQLite остаются обязанностью владельца БД.
 * loadChannels() читает самостоятельные снимки БД, bridge-события передаёт сюда внешний контроллер.
 * Собственные дочерние model/view/timer принадлежат виджету. Изменения ядра запрашиваются signals.
 * Контекстное меню хранит ID цели и проверяет наличие строки/поколение PM после вложенного event loop. */
#pragma once

#include <cstdint>
#include <QWidget>
#include <QListView>
#include <QList>
#include <QStandardItemModel>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QTimer>
#include <QPointer>

class DbManager;
class QLabel;
class QPushButton;
class CpuLoadIndicator;
class ClipboardExchange;
struct NavigationSnapshot;

class ChannelList : public QWidget {
    Q_OBJECT
public:
    /* Создаёт виджет с необязательной БД (без неё список пуст); parent владеет виджетом. */
    explicit ChannelList(DbManager* db = nullptr, QWidget *parent = nullptr);

    /* Читает один снимок в DB worker, повторные запросы объединяет; до результата сохраняет прежний список.
     * Готовый согласованный результат применяется и при новых обновлениях, затем читается следующий — без голодания под нагрузкой.
     * Ошибка сохраняет список. Выбор/online берутся при применении, исчезнувшая строка заменяется первой группой/PM.
     * Без backend разрешён только изолированный RAM-вариант; выбор испускает channelSelected. */
    void loadChannels();
    /* ID локальной активной рации; пустая строка снимает индикатор, выбор канала не меняется. */
    void setActiveRadioChannel(const QString& channel);
    /* Снимок общей рации: готовность PTT, фактический TX и общий замок; аудио управляет внешний контроллер. */
    void setRadioPttState(bool ready, bool transmitting, bool latched);
    /* Объединяет запросы в один запуск через 50 мс; таймер не периодический.
     * Отправляет owned-команды маршрута для PM последнего снимка, без SQL; недоступное ядро сбрасывает online. */
    void refreshDmPresence();
    /* Принимает заимствованный снимок [conv_len:1][UTF-8 conv][peer:8][group:8], числа native-endian.
     * Проверяет точную длину; online = ненулевые peer и group, это доступность BGP-маршрута. */
    void onDmCallRoute(const uint8_t* data, int len);
    /* Останавливает отложенный запрос и сбрасывает кеш/точки доступности PM; не отменяет уже отправленные команды. */
    void clearDmPresence();
    /* Отмечает ожидающее удаление поколения PM; повторное действие в меню недоступно до результата. */
    void setDmDeleting(const QString& convId, quint64 generation, bool deleting);
    /* Выбирает ID группы или "dm:<conv_id>"; если строка ещё загружается, запоминает запрос до результата.
     * Пустой/отсутствующий в готовом снимке ID сохраняет текущий выбор; более поздний выбор отменяет ожидание. */
    void selectChannel(const QString& channelId);
    /* Обновляет онлайн-счётчик существующей строки; отрицательное count отклоняется с предупреждением. */
    void setChannelPeersOnline(const QString& channelId, int count);
    /* Задаёт общий индикатор автоподключения для существующих и последующих строк групп. */
    void setAutoConnectActive(bool active);
    /* Показывает процент CPU, ограниченный диапазоном 0..100. */
    void setCpuLoad(int percent);
    /* Задаёт unread существующей строки; отрицательное count отклоняется, изменившаяся роль уведомляется один раз. */
    void setChannelUnreadCount(const QString& channelId, int count);
    /* Возвращает unread строки либо 0, если ID отсутствует. */
    int  getChannelUnreadCount(const QString& channelId) const;
    ClipboardExchange* clipboard() const { return m_clipboard; } // Заимствованный владелец отправки и входящих буферов.

signals:
    void radioPttPressed(); // Нажатие общей кнопки: начать передачу либо снять существующий замок.
    void radioPttReleased(); // Отпущено физическое удержание; замок остаётся.
    void radioPttLockRequested(); // Двойной щелчок: зафиксировать передачу.
    /* Выбрана строка (также после loadChannels); ID для PM имеет префикс "dm:". */
    void channelSelected(const QString& channelId);
    /* Запрос контроллеру подтвердить/удалить поколение PM; convId без префикса. */
    void dmDeleteRequested(const QString& convId, quint64 generation, const QString& name);
    /* Открыть файлы захваченной меню группы, если она по-прежнему присутствует в списке. */
    void filesRequested(const QString& channelId);
    /* Приняты локальные пути с CopyAction для выбранной группы при готовом ядре/своём node ID.
     * Сигнал отложен до следующей итерации Qt, paths копируются; проверка файлов/загрузка — у получателя. */
    void ownFilesDropped(const QString& channelId, const QStringList& paths);
    /* Кнопки footer: присоединиться, создать группу, настройки и монитор соединений. */
    void joinRequested();
    void openLinkRequested(); // Открыть произвольную ссылку через общий обработчик главного окна.
    void createGroupRequested();
    void settingsRequested();
    void connMonitorRequested();
    void networkTrafficRequested();

protected:
    void changeEvent(QEvent* event) override; // Обновляет растровые значки при смене палитры.
    /* Обрабатывает двойной щелчок/скрытие PTT, геометрию viewport и drag/drop в footer. */
    bool eventFilter(QObject* target, QEvent* event) override;

private:
    void updateRadioPttGeometry(); // Разместить единственную дочернюю кнопку над значком активной строки.
    void releaseRadioPtt(); // Снять только физическое удержание при скрытии/смене группы.
    QPushButton* m_radioPtt = nullptr; // Прозрачная кнопка над рисунком делегата; принадлежит viewport.
    bool m_radioTransmitting = false; // Фактический TX для повторного применения снимка списка.
    QString m_activeRadioChannel; // Не зависит от выбранной строки; сохраняется при обновлении модели.
    /* GUI: обновляет модель без SQL; settled=false сохраняет ожидающий выбор отсутствующей строки до следующего READ. */
    void applyNavigation(const NavigationSnapshot& snapshot, bool settled = true);
    void updateFooterVisibility(); // Скрывает version/traffic/monitor по очереди, если footer не помещается.
    QListView          *m_listView; /* Дочерний view; секции не выбираются мышью. */
    QStandardItemModel *m_model;    /* Дочерняя плоская модель, роли из channeldelegate.h. */
    QPointer<DbManager> m_db;       /* Не владеет БД; обнуляется при уничтожении QObject. */
    ClipboardExchange* m_clipboard; // Дочерняя панель своих устройств и уведомления участников по группам.
    QLabel             *m_header = nullptr; /* Заголовок без ограничения минимальной ширины; текст исчезает при нехватке места. */
    QWidget            *m_footer = nullptr; /* Дочерняя панель действий и приёма файлов. */
    QList<QWidget*>     m_footerOptional; /* Необязательные элементы в порядке скрытия при сужении. */
    QLabel             *m_dropHint; /* Дочерняя footer-подсказка, не перехватывает мышь. */
    bool                m_autoConnectActive = false; /* Общее состояние автоподключения групп. */
    QHash<QString, bool> m_dmOnline; /* Последние снимки доступности conv_id, сохраняются при reload. */
    QHash<QString, quint64> m_dmDeleting; /* conv_id → поколение ожидающего удаления */
    QStringList m_visibleDm; // conv_id последнего применённого снимка, источник запросов presence без SQL.
    QString m_selectAfterLoad; // Последний запрос выбора ещё отсутствующей строки; GUI-only.
    quint64 m_loadGeneration = 0; // Токен активного READ; очистка/смена БД отменяет действие старого callback.
    bool m_loadPending = false, m_reloadPending = false; // Не больше одного READ; изменения за время чтения требуют свежего снимка.
    QTimer* m_dmPresenceTimer; /* Дочерний single-shot, debounce 50 мс. */
    CpuLoadIndicator* m_cpuLoad; /* Дочерний индикатор CPU 0..100. */
};
