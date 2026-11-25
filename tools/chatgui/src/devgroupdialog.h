/* Диалоги сопряжения устройств и отключение self. Все операции передаются в uasync; секреты не читаются GUI. */
#ifndef DEVGROUPDIALOG_H
#define DEVGROUPDIALOG_H
#include <QByteArray>
#include <QString>
class QWidget;
class QPushButton;
/* Получать снимки приглашений на главное окно без автоматического открытия диалога. */
void initDevgroupUi(QWidget* window);
/* Есть незавершённое приглашение; показать выбор приглашения и текущий шаг после явного действия в меню self. */
bool hasPendingDevicePairing();
void finishDevicePairing(QWidget* parent);
/* Сразу создать приглашение выбранному устройству и показать PIN. */
void inviteMyDevice(QWidget* parent, const QString& channel, const QByteArray& peer);
/* Отключить self; кнопка блокируется до результата, при отказе восстанавливается и показывается ошибка. */
void disconnectMyDevice(QPushButton* button);
/* Отправить новое имя своей группы в ядро; результат/ошибка привязаны к живому окну parent. */
void renameMyDeviceGroup(QWidget* parent, const QString& name);
#endif
