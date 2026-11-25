/* firewall — список разрешённых пар IPv4/порт из global_config, без изменения системного firewall.
 * init/обнуление → load_rules (копия и сортировка правил) → check → free; контекст использует один поток.
 * check: 1 разрешено, 0 запрещено; port=0 в правиле разрешает все порты IP, bypass_all разрешает всё.
 * Оба направления основного TUN проверяются по назначению; автоматического допуска ответов нет.
 * IP и порт передавать в host order, как в CFG_FIREWALL_RULE. */
#ifndef FIREWALL_H
#define FIREWALL_H

#include <stdint.h>
#include <stddef.h>
#include "config_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

struct firewall_ctx {
    struct CFG_FIREWALL_RULE *rules; // Собственная отсортированная копия правил.
    int count;                     // Число правил; пустой набор запрещает всё.
    int bypass_all;                // allow=all: разрешение независимо от списка.
};

// Обнулить новый/освобождённый ctx; повторный init живого контекста запрещён. Возврат: 0/-1.
int fw_init(struct firewall_ctx *ctx);
// Заменить правила копией cfg; ctx должен быть инициализирован. Ошибка (-1) сохраняет ctx целиком; успех — 0.
int fw_load_rules(struct firewall_ctx *ctx, const struct global_config *cfg);
// Проверить IP/порт назначения в host order: 1/0. port=0 проверяет только разрешение всего IP.
int fw_check(const struct firewall_ctx *ctx, uint32_t ip, uint16_t port);
// Проверить чистый IPv4-пакет длиной len по назначению: 1 разрешён, 0 запрещён, -1 повреждён/не IPv4.
// Порт относится к TCP/UDP; фрагменты и другие протоколы требуют allow=IP либо allow=all.
int fw_check_packet(const struct firewall_ctx *ctx, const uint8_t *packet, size_t len);
// Освободить и обнулить инициализированный ctx; NULL и повторный вызов допустимы.
void fw_free(struct firewall_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif
