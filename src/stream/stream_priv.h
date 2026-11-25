/* Внутренний вход проверенного DATA-dispatcher: авторизация пира/handle/пути делается сетевым протоколом.
 * Подпись источника проверяется кольцом. Отсутствие локальной подписки отсекается до crypto/FEC.
 * Внешний frontend использует stream.h, а не прямую вставку сетевых блоков.
 * Все вызовы — uasync; key задаёт group/source/epoch, n/r/size берутся из живой локальной сессии. */
#ifndef STREAM_PRIV_H
#define STREAM_PRIV_H

#include "stream.h"
#include "stream_stats.h"
#include "stream_network.h"

/* Внутренние снимки и отправка, без callbacks и прямого соединения с автором. */
void stream_stats_forget(struct UTUN_INSTANCE *inst, uint64_t group);
void stream_network_stats(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, struct stream_stat_sample *sample);
void stream_network_report(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, struct stream_stat_sample *sample);

struct stream_local {
    struct stream_descriptor descriptor; // Фактический полный descriptor сессии, а не непроверенные поля входного key.
    struct stream_buffer *buffer; // Заимствованное общее RS-кольцо.
    struct stream_media_store *media; // Заимствованный индекс, NULL для непрозрачного байтового потока
    uint64_t start, last, next; // Общий anchor, подписанная граница END и следующий consumer index; см. флаги ниже.
    uint64_t progress_ms; // Монотонное время последней полной проверенной пачки, не прихода отдельной RS-части
    uint64_t live_head_us; // Максимальная известная PTS публикации автора этой эпохи; UINT64_MAX — неизвестно, пауза её не растит.
    int anchored, decoded, publisher, finished, relay_only; // Anchor есть; пачка проверена; свой source; END принят; все owners relay.
    int waiting; // Нет BGP и новых DATA >=30с: сеть освобождена до возвращения маршрута, локальные курсоры сохранены.
    const uint8_t *end; // STREAM_END_SIZE заимствованных байтов при finished, иначе NULL; last=UINT64_MAX для пустого END.
    struct stream_frame_timing pending; // Ещё не упакованные publisher-кадры; отсутствуют у relay
};
/* Сетевой контекст либо NULL; все заимствования ниже живут до следующей мутации/закрытия сессии. */
struct stream_network *stream_network_get(struct UTUN_INSTANCE *inst);
/* Обойти живые selector с временным внутренним handle; now_ms — монотонные мс, handle не публикуется caller. */
void stream_optimizers_tick(struct UTUN_INSTANCE *inst, uint64_t now_ms);
/* Найти живую точную эпоху: OK либо WAIT; при WAIT local не меняется. closing/invalidated сессии не выдаются. */
int stream_local_get(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, struct stream_local *local);
/* Сохранить монотонную нижнюю границу публикации из авторизованного DATA/INFO. UINT64_MAX игнорируется;
 * подсказка не подписана и не заменяет подтверждение KEY. У publisher её задаёт только собственная публикация. */
void stream_local_head(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, uint64_t pts_us);
/* Publisher обновляет диагностику своей ограниченной очереди в uasync; время не включает ETCP. */
void stream_publish_pending(struct STREAM_HANDLE *handle, const struct stream_frame_timing *pending);
/* По запросу media-получателя создать общий индекс поставщика; поиск истории выполняется отложенно с бюджетом. */
int stream_local_media_prepare(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key);
/* Скопировать inst/descriptor только manual receiver или временному selector handle; OK/ERR_STATE. */
int stream_handle_context(struct STREAM_HANDLE *handle, struct UTUN_INSTANCE **inst, struct stream_descriptor *descriptor);
/* Вставить часть после проверки peer/handle выше: проверить формат/путь, дописать self в сохранённый путь.
 * Отсутствующая подписка даёт WAIT до crypto; свой publisher и DATA за подписанным END отклоняются.
 * Возвращает результат кольца; OK/DUPLICATE могут поставить отложенную доставку. Путь не входит в подпись. */
int stream_ingest_path(struct UTUN_INSTANCE *inst, const uint8_t *wire, size_t length, const struct stream_path *path);
/* Проверить подпись/точную эпоху END и согласованность last с кольцом/прежним END; OK, WAIT либо ошибка.
 * Завершение не освобождает handles/кольцо; END доставляется после DATA. */
int stream_ingest_end(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, const uint8_t *wire, size_t length);
/* Монотонно поднять границу пройденных всеми выбранными upstream позиций и разбудить delivery. */
void stream_local_progress(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, uint64_t before);
/* Сбросить anchor/decoded/consumer start для новой попытки; сохранить кольцо, media cursors и нижнюю границу уже выданного.
 * Сам не закрывает network flows: вызывающий сначала отменяет старые receive handles. OK/ERR_STATE. */
/* start=UINT64_MAX ждёт первую DATA; точный start фиксирует общий KEY сразу, сохраняя уже принятые части и явный GAP. */
int stream_local_restart(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, uint64_t start);
/* Общая метрика приёма конкретного субпотока; не отдельного поставщика. OK/WAIT либо ошибка poll. */
int stream_local_metrics(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, uint8_t sub, uint64_t now_ms,
                         struct stream_metric_snapshot *snapshot);
/* Внутреннее владение точной эпохой; close обязателен и после detach. Режим существующего экземпляра сохраняется. */
int stream_relay_open(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, struct STREAM_HANDLE **out);
/* 1 для исходящего READY до handoff deadline либо BOUND, иначе0; не заменяет проверку членства/подписи DATA. */
int stream_relay_confirmed(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, struct stream_address peer);
/* Вызывается selector для общей сессии: запросить/отменить помощь online-канального supernode.
 * Один READ worker на точную эпоху; повтор обновляет needed, отмена/stop не ждут диска.
 * После READ повторно проверяются живая сессия, роли/маршруты; SQL-порядок кандидатов сохраняется.
 * Relay-only сессия не запрашивает помощь; недопущенные/неуспешные попытки имеют tombstone/cooldown60с. */
void stream_relay_demand(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, int needed, uint64_t now_ms);
/* Подтверждённый READY будит selector точной эпохи и отменяет backoff этого пира. */
void stream_relay_ready(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, struct stream_address peer);

/* Локальный/тестовый ввод без непосредственного relay: path={source}, далее обычный ingest_path. Не внешний dispatcher. */
int stream_ingest_block(struct UTUN_INSTANCE *inst, const uint8_t *wire, size_t length);

#endif
