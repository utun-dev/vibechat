/* IPv4 NAT через отдельный TUN и сервис ETCP_RT_ID_NAT в UTUN-router.
 * nat_via_ed задаёт удалённого провайдера; без адреса узел сам выполняет eim_nat для клиентов.
 * Приём ограничен группой UTUN; клиент принимает ответы только от настроенного провайдера.
 * init создаёт TUN/обработчики по nat_enabled; destroy снимает их и освобождает ресурсы.
 * TUN-очередь содержит [prefix:1][IP]; tun_write принимает только IP без prefix.
 * Отправка роутеру: [svc_id:1][IP], владение entry/dgram передаётся ему даже при ошибке.
 * Используется исходный TUN entry: NAT не копирует IP и не выделяет новый пакет.
 * FIFO/ожидания разделены по адресату: router send_q проверяется до передачи владения.
 * При заполнении буфера одного пира новые пакеты этого пира отбрасываются; общий лимит приостанавливает TUN.
 * EAGAIN записи в TUN удерживает один пакет, приём из router recv_q приостанавливается до его доставки.
 * У провайдера mapping исходящего пакета удерживается до записи в TUN, чтобы внешний порт не переиспользовали.
 * Раз в 100мс проверяются 128 mapping: полный проход <=51.2с при своевременном исполнении таймера.
 * Idle-timeout TCP=7440с, UDP=120с, ICMP=60с; статические записи не истекают.
 * Приём использует общий ROUTER_SVC_HDR_SIZE, источник — Ed25519 в ROUTER_SVC_SRC_ED_OFF.
 * Клиент не создаёт таблицу eim_nat: inst->nat.initialized служит флагом готовности транспорта.
 * Контекст принадлежит UTUN_INSTANCE; все вызовы и callbacks работают в его uasync-потоке. */
#ifndef NAT_TRANSPORT_H
#define NAT_TRANSPORT_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stdint.h>
#include <stddef.h>

#define NAT_PENDING_PER_PEER 64    // Дополнительный буфер одного получателя; переполнение локально отбрасывается с диагностикой.
#define NAT_PENDING_TOTAL 1024     // Общий предел: остановить чтение TUN до освобождения места.

struct tun_if;
struct ll_entry;
struct ll_queue;

struct nat_transport_ctx {
    struct tun_if* nat_tun;  // Отдельный принадлежащий модулю TUN, MTU=TUN_MTU_DEFAULT.
    struct ll_queue* peers;  // Внутренний хеш получателей с раздельными FIFO и router waiters.
    struct ll_entry* blocked_packet; // При общем backpressure остаётся в output_queue TUN.
    uint8_t blocked_ed[32];       // Полный адрес удержанного пакета, переживает замену mapping.
    size_t pending_count;    // Пакеты в FIFO получателей и их активных ожиданиях, <= NAT_PENDING_TOTAL.
    struct ll_entry* tun_pending; // Единственный принятый IP-пакет, ожидающий доступности TUN для записи.
    uint16_t tun_pending_port; // Удержанный mapping провайдера при EAGAIN; 0 у клиента/без удержания.
    void* maintenance_timer; // Порционная очистка mapping, восстановление ожиданий и уборка получателей.
    void* tun_retry_timer;   // Повтор записи TUN при EAGAIN; отменяется до tun_close.
    uint64_t dropped, last_drop_log; // Счётчик отбрасываний и ограничение частоты диагностики перегрузки.
    int initialized;        // TUN, binding и callback созданы; 0 при отключённом NAT/после destroy.
};

struct UTUN_INSTANCE;

/* inst и inst->config обязательны, ядро/роутер уже созданы; перед повторным init вызвать destroy.
 * Создать отдельный TUN и binding ETCP_RT_ID_NAT; провайдер также создаёт eim_nat.
 * 0 — успех, включая выключенный NAT; -1 — ошибка, созданные NAT-ресурсы освобождены.
 * Настройка внешних маршрутов/доступа в интернет выполняется отдельно от этого API. */
int  nat_transport_init(struct UTUN_INSTANCE* inst);
/* Отменить таймеры/waiters, освободить удержанные пакеты до TUN-пула, закрыть каналы NAT и уничтожить eim_nat.
 * Остальные router-сервисы сохраняются. NULL и повторный вызов допустимы.
 * Вызвать до освобождения роутера/uasync, вне выполняемого NAT callback. */
void nat_transport_destroy(struct UTUN_INSTANCE* inst);


#ifdef __cplusplus
}
#endif
#endif
