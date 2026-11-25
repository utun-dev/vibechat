/* GUI-черновик listen-сокетов: server-секции, auto_sockets/протоколы и разрешения LAN-приглашений/присутствия.
 * load заполняет модель ConfigServer, сигналы формы меняют её в памяти; save последовательно записывает INI.
 * Сокеты применяются после рестарта. LAN-флаги доступны вызывающему для отдельного live chat_setting запроса. */
#pragma once

#include <QWidget>
#include <QComboBox>
#include <QSpinBox>
#include <QString>
#include <QList>
#include "../transport/config_updater.h"

class QCheckBox;
class QLineEdit;
class QListWidget;
class QPushButton;

class NetworkSettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit NetworkSettingsPage(QWidget* parent = nullptr);

    /* Заменить черновик из server:*, global и chatserver INI; при невозможности открыть файл — пустой список/defaults.
     * Конструктор сам конфиг не загружает; интерфейсы/IPv4-подсказки перечисляются при создании страницы. */
    void loadFromConfig(const QString& configPath);
    /* Полностью переписать server-секции и сохранить auto/LAN-ключи через config_updater; ядро не меняет.
     * true после всех записей, false с логом при отказе; предыдущие успешные записи остаются, общей транзакции нет. */
    bool saveToConfig(const QString& configPath) const;
    /* Значения черновика для применения в ядре после успешного сохранения. */
    bool localInvitesEnabled() const;
    bool localBroadcastsEnabled() const;
    bool hideMySocketsChanged() const; // Сравнить черновик с загруженным значением для перезапуска после Save.

private slots:
    void onAutoSocketsToggled(bool on); // Состояние берётся из checkbox: переключить доступность ручной формы/auto-протоколов.
    void onAddServer(); // Добавить уникальный srvN: 0.0.0.0:1333, udp/public; выбрать новую строку.
    void onRemoveServer(); // Удалить выбранный ConfigServer и выбрать оставшуюся строку, если есть.
    void onServerSelected(); // Загрузить выбранную модель в поля формы без обратной записи сигналами.
    void onFieldChanged(); // Вне программного обновления записать форму в выбранный ConfigServer и подпись списка.

private:
    void populateInterfaces(); // Auto(any) и текущие UP-интерфейсы, кроме loopback.
    void populateAddresses(); // Auto(0.0.0.0) и IPv4 текущих UP-интерфейсов, кроме loopback; поле остаётся редактируемым.
    void applyAutoSocketsState(); // Auto запрещает ручное редактирование, сохраняя его модель для записи INI.
    void refreshServerList(int selectRow = -1); // Пересоздать подписи; выбрать selectRow либо первую строку при её наличии.
    void loadFormFromSelected(); // Копия полей выбранного server в виджеты, с m_updating для защиты от обратной записи.
    void updateSelectedServerFromForm(); // Перенести поля формы в выбранный server; собрать addr из IP-текста и порта.
    void updateSelectedListItemText(); // Обновить подпись выбранной строки без пересоздания всего списка.

    QComboBox* m_connectionProtocols; // Порядок и состав транспортов для подключений к нам.
    QComboBox* m_connectionMode; // Режим 1/2/3.
    QSpinBox* m_connectionSeconds; // Длительность этапа режима 2, секунды.
    QCheckBox*    m_autoSocketsCheck; // Дочерний контрол автоматического создания сокетов.
    QCheckBox* m_hideMySockets; // Принудительный private в ручном и автоматическом режимах.
    bool m_hideMySocketsInitially = false; // Значение при загрузке формы.
    QComboBox*    m_autoProtocolsCombo;  // протоколы автосокетов: TCP, UDP, TCP+UDP
    QCheckBox*    m_localInvitesCheck;    // разрешение входящих LAN-приглашений
    QCheckBox*    m_localBroadcastsCheck; // рассылка собственного присутствия в LAN
    QListWidget*  m_serverList; // Дочерний список; индексы совпадают с m_servers.
    QPushButton*  m_addBtn; // Дочерняя кнопка добавления server.
    QPushButton*  m_removeBtn; // Дочерняя кнопка удаления выбранного server.
    QLineEdit*    m_nameEdit; // Имя секции server:NAME, дочерний виджет.
    QComboBox*    m_ipCombo; // Редактируемый IP-текст; Auto преобразуется в 0.0.0.0, скобки IPv6 сохраняются в тексте.
    QSpinBox*     m_portSpin; // Порт 1..65535, дочерний виджет.
    QComboBox*    m_protoCombo; // Ручной server: UDP либо TCP, дочерний виджет.
    QComboBox*    m_typeCombo; // public/nat/private/local в item data, дочерний виджет.
    QComboBox*    m_interfaceCombo; // Первый пункт означает пустой netif, остальные — имена доступных интерфейсов.

    QList<ConfigServer> m_servers; // Собственный черновик server-секций, сохраняется также при выбранном auto_sockets.
    QString m_configPath; // Последний путь load; save получает свой путь отдельным аргументом.
    bool m_updating = false; // Подавляет запись сигналов формы обратно в модель при программном заполнении.
};
