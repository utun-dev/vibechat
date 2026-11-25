/* ntp_node_time — обмен временем с прямыми ETCP-пирами, дополнение к интернет-NTP.
 * TIME_SYNC различает свои часы, сохранённую поправку и синхронизацию после текущего запуска.
 * Сохранённое время пира остаётся SAVED; свежая синхронизация пира даёт PEER, собственный NTP — SERVER.
 * Поправки каждого уровня сохраняет ntp_time; более низкий уровень не заменяет активный высокий.
 * Получение времени учитывает половину RTT. Повышение приоритета рассылается соседям;
 * обновление того же уровня не вызывает повторную рассылку, чтобы избежать циклов.
 * Узел с кэшем получает свежий ответ; два узла одного уровня не отвечают друг другу бесконечно.
 * Часы ОС не меняются. Поправка применяется через ntp_time_get_us/get_seconds; enabled интернет-NTP не мешает обмену.
 * init после создания очереди connections привязывает raw ID=0x04 и callbacks INIT существующих/новых соединений.
 * На INIT отправляется TIME_SYNC, при init модуля — также всем уже initialized соединениям.
 * destroy снимает binding и все свои callbacks, сохраняя глобальную поправку ntp_time.
 * Все вызовы — в uasync-потоке ядра, stop/destroy вне callbacks модуля/соединений. */
#ifndef NTP_NODE_TIME_H
#define NTP_NODE_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct UTUN_INSTANCE;

#define NTP_NODE_MAX_PEERS 32               // размер диагностического кеша, не предел числа соседей

#define NTP_NODE_DRIFT_WARN_US  2000000LL   // модуль изменения смещения строго >2 с: WARN
#define NTP_NODE_DRIFT_ERROR_US 10000000LL  // строго >10 с: ERROR (вместо WARN)

struct NTP_NODE_PEER {
    uint8_t ed25519[32];   // Идентичность пира из INIT текущего транспорта; при вытеснении исходное смещение сбрасывается
    int64_t offset_us;     // raw local_recv - sender_time, мкс; без поправки на сетевую задержку
    int has_offset;        // есть предыдущее значение для сравнения; изменение не обязательно дрейф часов
    uint64_t last_seen_tb; // последнее принятое сообщение, монотонное время в 0,1 мс; для выбора вытесняемой записи
};

struct NTP_NODE_TIME {
    struct NTP_NODE_PEER peers[NTP_NODE_MAX_PEERS]; // при заполнении вытесняется наиболее давно обновлённая запись
    int peer_count;        // число занятых записей: 0..NTP_NODE_MAX_PEERS; кеш не ограничивает синхронизацию/ответы
    int active;            // binding/подписки установлены; повторный init не дублирует их
};

// inst и inst->connections обязательны. 0 — успех/уже активен, -1 — ошибка; свои частичные регистрации снимаются.
// Если ID 0x04 уже занят, возвращает -1, сохраняя чужую регистрацию. После успешного init можно вызвать destroy.
int  ntp_node_time_init(struct UTUN_INSTANCE* inst);
// Снимает binding, подписку новых соединений и callbacks со всех текущих, в том числе pending; очищает кеш.
// Не меняет inst->ntp.synced/offset_us/last_sync_tb. NULL и повторный destroy допустимы.
void ntp_node_time_destroy(struct UTUN_INSTANCE* inst);
// Шлёт TIME_SYNC всем initialized соединениям с links_up; NULL допустим.
// При наличии поправки раздаёт её с признаком свежести, без неё запрашивает время.
// Получатель выбирает поправку по приоритету, с половиной среднего RTT всех links (включая нулевые/down).
// RTT — оценка задержки: точность времени и симметричность пути не гарантируются; при отсутствии links поправка RTT = 0.
void ntp_node_sync_peers(struct UTUN_INSTANCE* inst);

#ifdef __cplusplus
}
#endif

#endif // NTP_NODE_TIME_H
