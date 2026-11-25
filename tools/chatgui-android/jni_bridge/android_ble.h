/* Android BLE lifetime: detach вызывается в worker перед уничтожением instance/uasync.
 * Радио живёт за JNI; транспорт и ll_queue принадлежат worker. */
#ifndef ANDROID_BLE_H
#define ANDROID_BLE_H
struct ble_transport;
struct ble_etcp;
/* Заимствованный общий драйвер: таблица соседей и connect через ble_etcp.h. Только worker, до detach. */
struct ble_etcp* android_ble_driver(void);
/* Заимствованный радио handle либо NULL. Только worker, до detach.
 * В штатном режиме TX/RX принадлежат драйверу ETCP; сырые пакеты доступны только instrumentation echo. */
struct ble_transport* android_ble_transport(void);
/* Worker: уничтожить transport и mailbox до instance/uasync; повторный вызов безопасен.
 * Android SDK stop асинхронно передаётся HandlerThread: поздние события отбрасываются по generation,
 * неудачные SDK releases остаются у Kotlin cleanup owner до успешного повтора. */
void android_ble_detach(void);
#endif
