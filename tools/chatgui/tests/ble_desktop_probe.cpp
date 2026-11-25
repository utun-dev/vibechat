/* Ручная проверка desktop frontend на настоящем радио с Android BlePeerProbe initiator=true. */
#include "../transport/desktop_ble.h"
#include "../../../lib/u_async.h"
#include "../../../lib/debug_config.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    debug_set_category_level(DEBUG_CATEGORY_DISCOVERY, DEBUG_LEVEL_DEBUG);
    UASYNC* ua = uasync_create();
    if (!ua) return 1;
    bool ready = false, advertised = false, discovered = false, connected = false;
    {
        const uint8_t key[BLE_PUBLIC_KEY_SIZE] = {0x56, 0x43, 0x42, 0x4c, 0x45, 0x54, 0x45, 0x53};
        DesktopBle radio(ua, key);
        radio.apply(7, "DesktopGUI");
        QElapsedTimer timer; timer.start();
        while (timer.elapsed() < 45000 && !(ready && advertised && discovered && connected)) {
            uasync_poll(ua, 100);
            const auto state = radio.snapshot();
            ready |= state.state == BLE_READY;
            advertised |= state.advertising == BLE_READY;
            discovered |= !state.peers.isEmpty();
            connected |= state.connections > 0;
        }
        radio.apply(0, "DesktopGUI");
        if (radio.transport() || radio.snapshot().state != BLE_CLOSED) return 1;
    }
    uasync_destroy(ua, 0);
    printf("RESULT desktop_frontend ready=%d advertised=%d discovered=%d connected=%d\n", ready, advertised, discovered, connected);
    return ready && advertised && discovered && connected ? 0 : 1;
}
