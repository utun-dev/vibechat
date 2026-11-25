/* Черновик разрешений desktop BLE, сохранение INI и применение через UtunNode.
 * Состояние радио и найденные узлы читаются как копия раз в секунду. */
#pragma once
#include <QWidget>
#include <QPointer>

class UtunNode;
class QCheckBox;
class QLabel;
class QListWidget;
class BluetoothSettingsPage : public QWidget {
public:
    BluetoothSettingsPage(const QString& path, UtunNode* node, QWidget* parent = nullptr);
    bool save(); // false при ошибке записи; причина залогирована, диалог показывает вызывающий.
private:
    void refresh();
    QString m_path;
    QPointer<UtunNode> m_node;
    QCheckBox *m_enabled, *m_advertise, *m_incoming, *m_invites;
    QLabel *m_status, *m_advertisingStatus, *m_power;
    QListWidget* m_peers;
};
