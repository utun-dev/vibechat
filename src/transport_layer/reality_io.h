/* reality_io — защищённая внешняя оболочка STCP с TLS-подобным handshake и записями.
 * STCP запускает client_start/server_start; ready разрешает внутренний STCP-handshake и последующий обмен через read/send.
 * Cleanup освобождает состояние соединения; owner_cleanup — общие ресурсы instance. Все вызовы — uasync-поток владельца.
 * Буфер оболочки отделён от внутреннего буфера STCP; TLS-совместимость этим модулем не обеспечивается. */

/* Протокол внешней оболочки:
 * CH -> SH + CCS + encrypted(EE/cover Certificate/cover CertificateVerify/Server Finished)
 *    -> CCS + encrypted(Client Finished) -> encrypted(STCP handshake и последующий поток).
 * Проверка стороны использует Finished, cover Certificate/CertificateVerify не являются PKI.
 * Root = HKDF-SHA256(AuthKey || X25519(client_ephemeral, server_ephemeral), SHA256(CH || SH),
 *                   "uTun-reality-record-v2"); в transcript входят handshake-заголовки, но не record-заголовки.
 * От root раздельно выводятся client-traffic/server-traffic и их key/iv/finished-ключи.
 * После собственного SH fallback запрещён. До него relay получает весь исходный буфер без нормализации.
 * Списки relay и кеш replay принадлежат instance; owner_cleanup закрывает только эти общие ресурсы.
 * Состояния активных STCP-соединений освобождаются отдельно через reality_io_cleanup. */
#ifndef REALITY_IO_H
#define REALITY_IO_H
#include "reality.h"
#include "etcp_padding.h"
#ifdef __cplusplus
extern "C" {
#endif
struct stcp_conn;
struct reality_io;
struct stream_transport_config;
struct reality_replay_cache;
struct reality_relay;
struct reality_owner {
    struct reality_replay_cache *replay; // ленивый кеш 1024 Hello до истечения timestamp + window
    struct reality_relay *relays; // активные relay, не активные STCP-соединения
    struct reality_relay *closed_relays; // закрытые контексты до call_soon или синхронного owner_cleanup
    unsigned relay_count; // число активных relay; предупреждение выше 64, жёсткого лимита нет
};

// ── Запуск защищённого соединения ──

// Создаёт оболочку и ставит ClientHello в очередь STCP; now — Unix-секунды, padding копируется (можно NULL).
// ready вызывается после проверки Finished, не внутри start. 0/-1; ошибка полностью удаляет новое I/O.
// c и ready обязательны, c->reality_io должен быть NULL; соединение остаётся собственностью вызывающего.
int reality_io_client_start(struct stcp_conn *c, const struct reality_client_config *cfg, uint32_t now,
                            const struct ETCP_PADDING *padding, void (*ready)(struct stcp_conn *));

// Создаёт серверную оболочку ожидания Hello с настройками c; 0/-1, ready обязателен.
int reality_io_server_start(struct stcp_conn *c, void (*ready)(struct stcp_conn *));

// ── Состояние и обмен данными ──

// Число ещё не обработанных wire-байтов; NULL/отсутствие оболочки даёт 0.
size_t reality_io_buffered(const struct stcp_conn *c);
/* После выхода из read/resume: копирует позиции ключей и заимствует wire-хвост до create транспорта.
 * 0 — готово/нет REALITY, 1 — ждём полный handshake message, -1 — неверное состояние. */
int reality_io_export(struct stcp_conn *c, struct stream_transport_config *config);

// Статическая диагностическая строка состояния, "none" для NULL/отсутствия оболочки.
const char* reality_io_state_name(const struct stcp_conn *c);

// Неблокирующий recv и обработка записей: 1 — приняты байты, 0 — пока нет, -1 — EOF/ошибка/close_notify.
// Ошибки закрывают STCP; close_notify запускает штатный finish. Обработчики STCP могут закрыть c.
int reality_io_read(struct stcp_conn *c);

// Обрабатывает буфер после снятия rx_paused; учитывает finish/close_notify; NULL допустим.
void reality_io_resume(struct stcp_conn *c);

// Передаёт plaintext после Finished; 0 — отправлено, 1 — есть очередь, -1 — ошибка.
// При успехе забирает и u_free(data); при ошибке буфер остаётся вызывающему, соединение следует закрыть:
// часть записей уже могла попасть в очередь. Для пустой записи допустим NULL/0.
int reality_io_send(struct stcp_conn *c, uint8_t *data, size_t len);

// Один раз ставит close_notify: 0 — готово/не требуется, 1 — очередь, -1 — ошибка (требуется закрытие).
int reality_io_close_notify(struct stcp_conn *c);

// ── Освобождение соединения и общих ресурсов ──

// Стирает ключи и освобождает оболочку, не закрывая fd; повторный вызов и NULL безопасны.
void reality_io_cleanup(struct stcp_conn *c);

// Закрывает relay, отменяет их отложенное освобождение и освобождает replay-кеш синхронно.
// До уничтожения UASYNC, вне callbacks relay; owner изначально обнулён, NULL допустим.
void reality_owner_cleanup(struct reality_owner *owner);

#ifdef __cplusplus
}
#endif
#endif
