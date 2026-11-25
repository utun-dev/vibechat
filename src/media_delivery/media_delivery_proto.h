/* media_delivery_proto — wire-структуры и подкоманды доставки блоков медиа CHAT-группы.
 * Описывает запросы держателей, чанки, подтверждения наличия и репликацию между суперузлами.
 * Это формат payload сервиса router, без группового маршрутизационного заголовка; обработчики — media_delivery/download.
 * Сетевые данные требуют SIGNED|ENCRYPTED, control — SIGNED. Локальный узел и конечный peer должны быть
 * активными членами CHAT-группы (source=0, deleted=0, admitted=1); наличие общего ETCP не даёт доступа.
 * QUERY и передача блоков доступны обычным участникам; HAVE_BLOCK/PROCESSING принимает суперузел,
 * SYNC/CHANGED требуют роли суперузла с обеих сторон. Повторённый Ed25519 группы должен совпадать с адресом router.
 * Числа SYNC кодируются little endian; остальные packed-поля — native byte order текущего формата.
 * SERVE_REG/LEAVE меняют только вспомогательный served_nodes; текущий загрузчик их не посылает.
 * SERVE_ACK сейчас не отправляется, входящие SERVE_ACK/HAVE_BLOCK_ACK игнорируются; ожидания ACK и его retry нет. */
#ifndef MEDIA_DELIVERY_PROTO_H
#define MEDIA_DELIVERY_PROTO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ── подкоманды (первый байт данных после etcp_router) ── */

enum {
    MEDIA_SUBCMD_SERVE_REG      = 0x01,  // вспомогательный учёт узла в served_nodes
    MEDIA_SUBCMD_SERVE_ACK      = 0x02,  // объявлен, но не отправляется; приём игнорируется
    MEDIA_SUBCMD_SERVE_LEAVE    = 0x03,  // удалить вспомогательную запись served_nodes
    MEDIA_SUBCMD_QUERY          = 0x04,  // req-узел→суперузел: кто имеет блоки?
    MEDIA_SUBCMD_QUERY_RESP     = 0x05,  // суперузел→req-узел: список узлов
    MEDIA_SUBCMD_HAVE_BLOCK     = 0x06,  // узел→суперузел: у меня есть блок
    MEDIA_SUBCMD_HAVE_BLOCK_ACK = 0x07,  // суперузел→узел: подтверждение приёма
    MEDIA_SUBCMD_SYNC_REQ       = 0x08,  // получатель→суперузел: страница после курсора
    MEDIA_SUBCMD_SYNC_RESP      = 0x09,  // суперузел→получатель: страница каталога
    MEDIA_SUBCMD_BLOCK_REQ      = 0x0A,  // req-узел→блок-холдер: дай блок
    MEDIA_SUBCMD_BLOCK_CHUNK    = 0x0B,  // блок-холдер→req-узел: чанк данных (1KB)
    MEDIA_SUBCMD_BLOCK_DONE     = 0x0C,  // блок-холдер→req-узел: блок передан полностью
    MEDIA_SUBCMD_CANCEL         = 0x0D,  // req-узел→блок-холдер: отмена передачи блока
    MEDIA_SUBCMD_CHANGED        = 0x0E,  // суперузел→суперузел: каталог мог измениться (один байт)
    MEDIA_SUBCMD_BLOCK_OVERLOADED = 0x0F, // автор→req-узел: перегружен, попробуй позже
    MEDIA_SUBCMD_BLOCK_PROCESSING  = 0x10, // узел→суперузел: начал качать блок (status=processing)
    MEDIA_SUBCMD_BLOCK_RELAY_FULL  = 0x11, // relay-узел→req-узел: перегружен, вот список кому транслирую
};

/* ── ответные статусы ── */

#define MEDIA_ERR_NOT_SUPERNODE 0xFE /* Объявлен, в текущих ответах не используется. */

/* Один QUERY — одна полная пачка результатов, включая блоки без держателей.
 * Группу и end-to-end отправителя задаёт router. Пределы общие для обеих сторон. */
#define MEDIA_QUERY_MAX_BLOCKS 32
#define MEDIA_QUERY_MAX_HOLDERS 4
#define MEDIA_QUERY_OK 0
#define MEDIA_QUERY_DB_ERROR 1

/* Каждая попытка BLOCK_REQ получает новый UUID; все ответы/CANCEL повторяют его.
 * Группа и отправитель берутся только из router. Байты идут непрерывно от offset=0.
 * DONE подтверждает конец передачи; подпись берётся из метаданных сообщения, а не из ответа держателя. */
#define MEDIA_BLOCK_DATA_MAX 1024
#define MEDIA_BLOCK_SIZE_MAX (25U * 1024U * 1024U)
#define MEDIA_BLOCK_OK 0
#define MEDIA_BLOCK_UNAVAILABLE 1

/* Репликация положительных объявлений, не снимок удалённой файловой системы.
 * Группа/источник — только router; обе стороны должны быть суперузлами этой CHAT-группы.
 * Все числа SYNC — little endian. ID ограничены INT64_MAX; epoch/UUID — непрозрачные 16 байт.
 * Новый запрос имеет новый UUID; timeout повторяет тот же запрос. Ответ принимается только
 * для текущего UUID. Эпоха сохраняется в БД источника и меняется при пересоздании каталога.
 * При несовпадении ожидаемой эпохи источник читает с нуля. OK применяется вместе с RX-курсором
 * одной транзакцией. CHANGED — подсказка; периодический SYNC восстанавливает потерянные подсказки. */
#define MEDIA_SYNC_MAX_ENTRIES 12 /* Подписанные строки: 53 + 12*141 = 1745 байт. */
#define MEDIA_SYNC_OK 0
#define MEDIA_SYNC_DB_ERROR 1
#define MEDIA_SYNC_CURSOR_INVALID 2

/* ── packed-структуры пакетов ── */

#pragma pack(push, 1)

struct media_pkt_serve_reg {
    uint8_t  subcmd;       // MEDIA_SUBCMD_SERVE_REG
    uint8_t group_ed[32];
};

struct media_pkt_serve_ack {
    uint8_t  subcmd;       // MEDIA_SUBCMD_SERVE_ACK
    uint8_t group_ed[32];
    uint8_t  status;       // 0=OK
};

struct media_pkt_serve_leave {
    uint8_t  subcmd;       // MEDIA_SUBCMD_SERVE_LEAVE
    uint8_t group_ed[32];
};

struct media_pkt_query {
    uint8_t  subcmd;       // MEDIA_SUBCMD_QUERY
    uint8_t  media_id[16];
    uint8_t  request_id[16]; // Новый случайный UUID для каждой попытки; не средство аутентификации.
    uint16_t num_blocks;    // 1..MEDIA_QUERY_MAX_BLOCKS
    // далее: block_ids[num_blocks * 16]
};

#define MEDIA_QUERY_HDR_SIZE  (sizeof(struct media_pkt_query))  // 35

struct media_pkt_query_resp_entry {
    uint8_t  block_id[16];
    uint8_t  num_holders;  // 0..MEDIA_QUERY_MAX_HOLDERS; ноль — держатели не найдены.
    uint8_t holders[MEDIA_QUERY_MAX_HOLDERS][32]; // Уникальные Ed25519; неиспользуемые слоты нулевые.
};

struct media_pkt_query_resp {
    uint8_t  subcmd;       // MEDIA_SUBCMD_QUERY_RESP
    uint8_t  media_id[16];
    uint8_t  request_id[16]; // Точное эхо QUERY, включая ответ с ошибкой.
    uint8_t  status;        // MEDIA_QUERY_OK / MEDIA_QUERY_DB_ERROR
    uint16_t num_blocks;    // OK: столько же, сколько в QUERY; ошибка: 0.
    // Далее результаты всех блоков в порядке запроса; частичных успешных ответов нет.
};

#define MEDIA_QUERY_RESP_HDR_SIZE (sizeof(struct media_pkt_query_resp))  // 36; максимум с записями — 4676 байт.

struct media_pkt_have_block {
    uint8_t  subcmd;       // MEDIA_SUBCMD_HAVE_BLOCK
    uint8_t group_ed[32];
    uint8_t  media_id[16];
    uint8_t  block_id[16];
    uint32_t chunk;
    int64_t  timestamp;    // unix epoch
    uint64_t version;      // Сохранённая версия объявления держателя, 1..INT64_MAX.
    uint8_t signature[64]; // Подпись держателя, сохраняется при репликации S.
};

#define MEDIA_HAVE_BLOCK_SIZE (sizeof(struct media_pkt_have_block))

struct media_pkt_have_block_ack {
    uint8_t  subcmd;       // MEDIA_SUBCMD_HAVE_BLOCK_ACK
    uint8_t  media_id[16];
    uint8_t  block_id[16];
    uint8_t  status;       // 0=сохранено изменение, 1=повтор/старое объявление, 2=ошибка БД.
};

struct media_pkt_sync_req {
    uint8_t subcmd;             // MEDIA_SUBCMD_SYNC_REQ
    uint8_t request_id[16];     // UUID текущего запроса страницы.
    uint8_t source_epoch[16];   // Ожидаемая эпоха источника; нули для первого запроса.
    uint64_t after_id;          // Уже атомарно сохранённый RX-курсор.
};

struct media_pkt_sync_entry {
    uint64_t id;                // ID строки у источника, строго возрастают в странице.
    uint8_t block_id[16];
    uint8_t holder[32];         // Ed25519 держателя; уникален с block_id внутри группы.
    uint32_t chunk;             // Неизменяемый номер блока.
    int64_t timestamp;          // Unix-время объявления, неотрицательное.
    uint8_t status;             // 0=PROCESSING, 1=COMPLETE; COMPLETE имеет приоритет.
    uint64_t version;           // Версия самого держателя, не ID посредника.
    uint8_t signature[64];      // Подпись holder над group и всеми полями кроме id.
};

struct media_pkt_sync_resp {
    uint8_t subcmd;             // MEDIA_SUBCMD_SYNC_RESP
    uint8_t request_id[16];     // Точное эхо UUID запроса.
    uint8_t status;             // MEDIA_SYNC_*; ошибка не продвигает курсор, записей нет.
    uint8_t source_epoch[16];
    uint64_t from_id;           // after_id запроса при совпадении эпох, иначе 0.
    uint64_t next_id;           // При more — последний ID страницы; иначе верхняя граница снимка.
    uint8_t more;               // 0/1; 1 требует полной страницы MEDIA_SYNC_MAX_ENTRIES.
    uint16_t num_entries;       // 0..MEDIA_SYNC_MAX_ENTRIES; далее media_pkt_sync_entry[num_entries].
};

#define MEDIA_SYNC_REQ_SIZE sizeof(struct media_pkt_sync_req)       // 41
#define MEDIA_SYNC_RESP_HDR_SIZE sizeof(struct media_pkt_sync_resp) // 53
#define MEDIA_SYNC_PACKET_MAX (MEDIA_SYNC_RESP_HDR_SIZE + MEDIA_SYNC_MAX_ENTRIES * sizeof(struct media_pkt_sync_entry)) // 1745

struct media_pkt_block_req {
    uint8_t  subcmd;       // MEDIA_SUBCMD_BLOCK_REQ
    uint8_t  media_id[16];
    uint8_t  block_id[16];
    uint8_t  transfer_id[16]; // UUID попытки; одинаков только при дублировании запроса.
    uint32_t chunk;
};

#define MEDIA_BLOCK_REQ_SIZE (sizeof(struct media_pkt_block_req))

struct media_pkt_block_chunk {
    uint8_t  subcmd;       // MEDIA_SUBCMD_BLOCK_CHUNK
    uint8_t  media_id[16];
    uint8_t  block_id[16];
    uint8_t  transfer_id[16];
    uint32_t chunk;
    uint32_t offset;       // смещение в блоке
    uint16_t data_len;     // 1..1024
    // далее: data[data_len]
};

#define MEDIA_BLOCK_CHUNK_HDR_SIZE (sizeof(struct media_pkt_block_chunk))  // 59

struct media_pkt_block_done {
    uint8_t  subcmd;       // MEDIA_SUBCMD_BLOCK_DONE
    uint8_t  media_id[16];
    uint8_t  block_id[16];
    uint8_t  transfer_id[16];
    uint32_t chunk;
    uint32_t total_size;   // полный размер блока
    uint8_t  status;       // MEDIA_BLOCK_OK / MEDIA_BLOCK_UNAVAILABLE; ошибка не подтверждает байты.
};

#define MEDIA_BLOCK_DONE_SIZE (sizeof(struct media_pkt_block_done))

struct media_pkt_cancel {
    uint8_t  subcmd;       // MEDIA_SUBCMD_CANCEL
    uint8_t  media_id[16];
    uint8_t  block_id[16];
    uint8_t  transfer_id[16];
    uint32_t chunk;
};

struct media_pkt_block_overloaded {
    uint8_t  subcmd;       // MEDIA_SUBCMD_BLOCK_OVERLOADED
    uint8_t  media_id[16];
    uint8_t  block_id[16];
    uint8_t  transfer_id[16];
    uint32_t chunk;
    uint32_t retry_after_ms; // рекомендованная задержка перед повтором (ms), 0 = default 2000
};

struct media_pkt_block_relay_full {
    uint8_t  subcmd;       // MEDIA_SUBCMD_BLOCK_RELAY_FULL
    uint8_t  media_id[16];
    uint8_t  block_id[16];
    uint8_t  transfer_id[16];
    uint32_t chunk;
    uint16_t num_relay_nodes; // 1..5
    // далее: Ed25519 каждого получателя, по 32 байта
};

#define MEDIA_RELAY_FULL_HDR_SIZE (sizeof(struct media_pkt_block_relay_full))  // 55

#pragma pack(pop)

#ifdef __cplusplus
}
#endif
#endif
