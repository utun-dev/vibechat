/* Сетевые выдачи raw ETCP с собственным NCD-владением пира; reverse/терминальные уведомления через router.
 * Один pump/waiter на пира для всех групп; DATA читается из кольца при отправке, ответы хранятся в ограниченной FIFO.
 * Новые запросы и DATA проверяют допуск группы; завершение известных операций доступно также после отзыва/закрытия контента.
 * Сохранённый INFO/ACCEPT повторяет проверку допуска при отправке; при отзыве возвращается REJECT.
 * Routed control требует подпись и шифрование конечных узлов; транзитные узлы доступ к группе не проверяют.
 * STREAM_RECEIVE принадлежит вызывающему даже после отмены/stop; close снимает только данную выдачу.
 * API выполняется в uasync-потоке. Внешние ручные операции требуют stream_subscribe_manual_open;
 * автоматический выбор/перенос поставщиков владеет своими handles через отдельный планировщик.
 * Бюджеты из [chatserver], общие для групп: defaults64 пира,192 regular+64 temporary,16 temporary на пира,128 control.
 * DATA одного пира ограничен общим regular; минимумk автоматического приёма резервируется целиком, временные операции отдельно.
 * Из временных слотов фоновые пробы/переносы занимают не более16; остальные доступны аварийному поиску.
 * Постоянный слот selector принадлежит субпотоку и сохраняется при потере поставщика до close владельцем.
 * INFO согласует окно запросов; WINDOW_UPDATE/APPLIED меняет его без потери уже отправленных запросов/ответов.
 * Ответы, закрытия, RENEW и OVERLOAD не занимают запросное окно; DATA получает очередь после не более8 control.
 * Close возвращает DATA-бюджет сразу; объект сохраняет отписку до передачи ETCP. Объекты пира ограничены regular+temporary.
 * История метрик освобождается при CLOSED; итоговый снимок остаётся доступным через stream_receive_get_info до close владельца.
 * INFO/connect ждёт до5с; сетевой срок начинается с передачи ETCP, ожидание окна не штрафует поставщика.
 * Downstream аренда6с начинается с передачи ACCEPT и продлевается каждые2с. Готовая INFO-проба требует явного close.
 * Измеренный CPU>80% сразу закрывает общий допуск новых выдач; CPU<=70% в течение500мс открывает его.
 * Очереди при свободном CPU ограничивают только соответствующих получателей через OVERLOAD/SELECT/CLOSED.
 * Снижение regular запускает OVERLOAD по числу слотов; рост очереди — по rate. Порции до64 обходят все выдачи/получателей.
 * Общий допуск сам не закрывает действующие выдачи; разгрузка требует безопасного SELECT или подтверждённой DATA замены.
 * OVERLOAD учитывает rate и требуемое число закрытий; временная неподтверждённая DATA не разрешает SELECT старой.
 * SELECT повторяется каждые500мс. OVERLOAD_END(id,outcome) завершает запрос, не закрывая DATA; epoch/id защищают от старой отмены.
 * При недостигнутой разгрузке и продолжающейся перегрузке cooling ограничен1с, после него запрос оценивается заново.
 * SEEK сохраняет слот и меняет wire handle: перенос общего старта на KEY разрешён при CPU/peer pressure.
 * Удаляет пропускаемую очередь; уже переданный, но не полученный KEY можно повторить в том же слоте.
 * Прежний handle больше не принимает DATA, повтор SEEK идемпотентен; отмена до ACCEPT снимает оба возможных ID.
 * После первого DATA тишину источника не считают истечением receive; восстановлением управляет selector/владелец. */
#ifndef STREAM_NETWORK_H
#define STREAM_NETWORK_H

#include "stream.h"
#include "stream_wire.h"
#include "stream_load.h"

#ifdef __cplusplus
extern "C" {
#endif

struct stream_network;
struct STREAM_RECEIVE;
struct stream_receive_slot;
enum stream_receive_state { STREAM_RECEIVE_CONNECTING, STREAM_RECEIVE_INFO, STREAM_RECEIVE_REQUESTED,
                            STREAM_RECEIVE_ACTIVE, STREAM_RECEIVE_ENDED, STREAM_RECEIVE_CLOSED };
struct stream_receive_info {
    struct stream_address peer; // Ed25519 непосредственного поставщика.
    uint64_t handle,       // ID DATA-запроса;0 до SUBSCRIBE и у INFO-only пробы.
             start,       // Запрошенный, после ACCEPT подтверждённый старт; UINT64_MAX для запроса с конца.
             next,        // Следующий DATA-index после ACCEPT.
             blocks,      // Принятые RS-части, включая глобальные DUPLICATE/EXPIRED; не число проверенных FEC-пачек.
             gaps,        // Принятые сообщения GAP.
             local_epoch, peer_epoch; // Поколения stream-peer, подтверждённые INFO_RESP; не source_epoch/ETCP reset_id.
    enum stream_receive_state state; // CONNECTING → INFO → REQUESTED → ACTIVE → ENDED; CLOSED — также после detach.
    int reason; // STREAM_REASON_* для CLOSED; последнее состояние/метаданные сохраняются до close.
    struct stream_metric_snapshot metrics, supplier_metrics; // Собственный приём и последний снимок поставщика из INFO_RESP.
    uint32_t rtt_ms; // INFO RTT от передачи в ETCP до ответа; очередь ETCP входит в измерение.
    int metrics_valid, supplier_metrics_valid, rtt_valid, close_requested; // Validity снимков/RTT; selected в активном OVERLOAD.
    int supplier_overloaded; // Поставщик прислал OVERLOAD для этой выдачи, даже если SELECT пока не может её освободить.
    int relocate_requested; // Эта необходимая выдача выбрана для переноса, чтобы покрыть недостающую разгрузку.
    uint64_t overload_id; // Последний peer-запрос, включая завершённый; id монотонен внутри peer-эпохи.
    int overload_outcome; // 0 — активен/неизвестен; STREAM_OVERLOAD_* — завершение либо локальное истечение срока.
    struct stream_path path; // От автора до поставщика, без получателя; меняется при приходе DATA.
    uint64_t first_available, last_available, // Включительные границы предложенного субпотока; UINT64_MAX/UINT64_MAX — пусто.
             info_ms, info_order, // Время последнего INFO/ACCEPT в локальных мс; порядок принятого INFO_RESP в network.
             supplier_queue_bytes, progress_age_ms; // Общая DATA-очередь поставщика к нам и возраст его прогресса субпотока.
    uint64_t key_seq, key_last_seq; // Включительный диапазон последнего полного KEY, если key_valid=1
    uint64_t live_head_us, media_pts_us, key_pts_us; // PTS публикации автора, полного видео поставщика и KEY; UINT64_MAX — неизвестно.
    uint64_t requested_seq, contiguous_last, supplier_queue_age_ms; // Точная INFO-позиция, конец непрерывного диапазона и возраст очереди.
    int requested_present; // Запрошенная часть действительно имеется; диапазон сам по себе не подтверждает наличие.
    uint64_t active_ms, progress_ms, data_next, subscribe_ms; // ACCEPT, последняя новая DATA, её seq+1 и передача SUBSCRIBE/SEEK в ETCP, мс.
    int recovery_ready; // Получена DATA наблюдаемой точной позиции; INFO/ACCEPT/GAP этот флаг не устанавливают.
    uint64_t recovery_ms; // Монотонный момент этой DATA; сравнение одного индекса у двух поставщиков не зависит от базы lag.
    uint32_t path_rtt_ms; // RTT пути до поставщика, без последнего перехода к нам.
    uint16_t free_slots, cpu_basis_points; // Свободные peer/global flow-слоты и CPU0..10000 при cpu_valid.
    uint8_t subblock; // Индекс RS-части0..n-1.
    int info_ready, probe_only, accepting, // Ответ получен; INFO-only; поставщик не в pressure/cooling (слоты проверяются отдельно).
        path_rtt_valid, progress_valid, cpu_valid, key_valid; // Достоверность соответствующих полей предложения, не подписи пачки.
};
/* INFO без выделения remote handle и без DATA; close обязателен также после detach.
 * out заранее NULL; OK, LOCAL_BUSY для местного лимита, WAIT для недоступного/неподписанного поставщика, отрицательная ошибка.
 * После info_ready можно тем же NCD-владением последовательно опросить остальные субпотоки.
 * seq — точная позиция либо UINT64_MAX для общего обзора. Использует только фоновый бюджет, deadline5с.
 * В той же peer/stream/sub позиции разрешена одна проба; незавершённый INFO DATA-запроса тоже занимает её. */
int stream_probe_open(struct STREAM_HANDLE *subscription, struct stream_address peer, uint8_t subblock, uint64_t seq, struct STREAM_RECEIVE **out);
/* Аварийный INFO для точной позиции: доступен весь временный резерв. Отсутствие части возвращается без DATA.
 * Владелец обязан ограничить ожидание своим таймером: обычный5-секундный deadline здесь отключён. */
int stream_probe_open_at(struct STREAM_HANDLE *subscription, struct stream_address peer, uint8_t subblock, uint64_t seq, struct STREAM_RECEIVE **out);
/* Превратить готовую пробу в DATA-выдачу с seq из подтверждённого непрерывного диапазона, без повторного INFO.
 * slot — постоянный резерв данного субпотока; handle остаётся тем же. OK/LOCAL_BUSY/WAIT/ERR_STATE.
 * ACCEPT повторно проверяет реальное наличие и допуск поставщика; INFO не резервирует ресурс удалённого узла.
 * Ожидание DATA ограничивает владелец с момента subscribe_ms, затем от progress_ms; обычный5-секундный deadline отключён. */
int stream_probe_subscribe(struct STREAM_RECEIVE *probe, uint64_t seq, struct stream_receive_slot *slot);
/* Наблюдать DATA точной seq для подтверждения восстановления; UINT64_MAX снимает наблюдение.
 * После получения будит selector; только живой DATA-handle, NULL допустим. */
void stream_receive_watch(struct STREAM_RECEIVE *receive, uint64_t seq);

/* Перенести общий старт ACTIVE-выдачи вперёд на seq с новым wire handle в прежнем слоте; не создаёт дополнительной подписки.
 * seq больше прежнего start и не меньше локального consumer cursor. Уже отправленная, но не принятая часть KEY повторяется.
 * INFO должен предварительно подтвердить общий KEY. Принимается независимо от запрета новых выдач по CPU/очереди.
 * OK — SEEK ожидает места запросного окна; ID меняется при передаче ETCP, затем ACCEPT/DATA либо CLOSED.
 * Ошибка до принятия намерения сохраняет текущую выдачу; отмена ожидающего SEEK не создаёт нового wire handle. */
int stream_receive_seek(struct STREAM_RECEIVE *receive, uint64_t seq);
/* Повторить INFO готовой пробы для subblock/seq (UINT64_MAX — обзор); пригоден только новый info_ready.
 * OK, WAIT при конфликте pending INFO, ERR_STATE для неверного handle/subblock. */
int stream_probe_next(struct STREAM_RECEIVE *probe, uint8_t subblock, uint64_t seq);
/* out заранее NULL; seq=UINT64_MAX: первая DATA-выдача с конца, остальные ждут общий anchor первой принятой части.
 * Перед SUBSCRIBE свежий INFO проверяет точный seq и допуск; отсутствие уже опубликованной части закрывает handle.
 * Следующая ещё не созданная часть допустима; INFO не резервирует удалённый ресурс, ACCEPT проверяет его повторно.
 * Подпись всей пачки проверяется после RS decode; границы INFO сами не гарантируют наличие каждой части.
 * peer должен быть online-мембером с подпиской либо подтверждённой relay-помощью.
 * Второй DATA-flow peer+stream+sub запрещён; INFO-проба допускается после выделения DATA handle.
 * OK — локальный handle создан, не ACCEPT; LOCAL_BUSY — местный лимит; WAIT — поставщик недоступен; отрицательный результат — ошибка. */
int stream_receive_open(struct STREAM_HANDLE *subscription, struct stream_address peer, uint8_t subblock, uint64_t seq, struct STREAM_RECEIVE **out);
/* Зарезервировать постоянный слот selector для subblock; out заранее NULL. OK/LOCAL_BUSY/отрицательная ошибка.
 * Слот держится независимо от транспорта; повторные попытки одного субпотока используют тот же слот.
 * Только uasync. Освободить после закрытия всех использующих его DATA-handles. */
int stream_receive_slot_open(struct STREAM_HANDLE *subscription, uint8_t subblock, struct stream_receive_slot **out);
struct stream_receive_reservation;
/* Атомарно занять k постоянных слотов до начала автоматического DATA. Повтор при LOCAL_BUSY; out заранее NULL.
 * Слоты любой части этой эпохи используют резерв, дополнительные части расходуют свободный regular. */
int stream_receive_reserve(struct UTUN_INSTANCE *inst, const struct stream_descriptor *descriptor,
                           struct stream_receive_reservation **out);
/* Освободить резерв после всех его slots. NULL допустим, destroy network отсоединяет память владельца. */
void stream_receive_unreserve(struct stream_receive_reservation *reservation);
/* Можно ли освободить слот сверх k для ожидающего допуска/нового лимита; downstream и работающие k сохраняются. */
int stream_receive_slot_surplus(struct stream_receive_slot *slot);
/* Закрыть слот без DATA-handles; NULL допустим. После остановки network освобождает только память владельца. */
void stream_receive_slot_close(struct stream_receive_slot *slot);
/* Приём в зарезервированный слот; вторая одновременная выдача расходует временный бюджет фонового переноса.
 * slot соответствует subscription/subblock, out заранее NULL. OK/LOCAL_BUSY/WAIT/отрицательная ошибка. */
int stream_receive_reserved_open(struct STREAM_HANDLE *subscription, struct stream_receive_slot *slot,
                                 struct stream_address peer, uint64_t seq, struct STREAM_RECEIVE **out);
/* Освободить свой handle, в том числе после END/CLOSED/stop; NULL допустим. Общий NCD/ETCP не принадлежит этому handle. */
void stream_receive_close(struct STREAM_RECEIVE *receive);
/* Отменить текущие upstream данного локального экземпляра и сбросить anchor; новые выдачи открывает вызывающий.
 * Буфер/подписка/downstream и нижняя граница consumer cursor сохраняются: уже выданные DATA не повторяются.
 * Старые receive handles становятся CLOSED и всё равно требуют close. Только manual/internal subscription. */
int stream_receive_restart(struct STREAM_HANDLE *subscription);
/* Скопировать info даже после detach, обновив локальные метрики; OK/ERR_ARGUMENT.
 * info_ready может сохраняться в CLOSED: живое состояние проверяется отдельно. */
int stream_receive_get_info(struct STREAM_RECEIVE *receive, struct stream_receive_info *info);
struct stream_peer_load_info {
    struct stream_load_snapshot load; // Снимок rates последнего poll с текущими накопителями/остатком DATA.
    // Локальный OVERLOAD: ID/требуемый и освобождённый вклад/срок; затем общие ETCP-очереди и неподтверждённые байты.
    uint64_t request_id, required_rate, released_rate, deadline_ms, transport_queued_bytes, inflight_bytes;
    uint64_t shortfall_rate; // Недостигнутая разгрузка истёкшего запроса, байт/с;0 после разгрузки/новой оценки. Причина неизвестна.
    uint32_t transport_queued_packets; // send_input_q + input_queue; bytes включает также normalizer через etcp_tx_queued.
    double cpu_percent; // Средняя загрузка uasync либо0 при неизвестной оценке.
    uint64_t callback_late_ms; // Опоздание общего100-мс network tick.
    unsigned required_count, released_count; // Обязательное и фактическое число снимаемых downstream; оба порога OVERLOAD обязательны.
    int global_pressure, request_active, accepting; // Запас CPU исчерпан; запрос пира; нет CPU/peer pressure и cooling.
    int request_outcome; // 0 — активный/ещё не было запроса, иначе stream_overload_outcome; не означает отказ всей трансляции.
};
/* Только существующий stream-peer; OK/ERR_STATE, выход не меняется при отказе. ETCP-статистика общая с другими сервисами. */
int stream_peer_get_load(struct UTUN_INSTANCE *inst, struct stream_address peer, struct stream_peer_load_info *info);
/* Оставшиеся RS-части всех локальных выдач точной публикации; принятые ETCP части сюда не входят. */
uint64_t stream_network_pending_bytes(struct UTUN_INSTANCE *inst, const struct stream_descriptor *descriptor);
/* Снимок позиций downstream в кольце точной эпохи, худшие получатели первыми. Только uasync.
 * out обязателен; STREAM_OK либо ошибка аргументов/отсутствия локальной подписки. Кольцо не сканируется. */
int stream_network_buffer_positions(struct UTUN_INSTANCE *inst, const struct stream_descriptor *descriptor,
                                    struct stream_buffer_positions *out);
/* Худший целый входящий путь по сумме известных длительностей, без склейки разных ветвей.
 * После разрыва используется сохранённая телеметрия; при её отсутствии OK с count0. Полный descriptor обязателен.
 * Возраст/STALE/DOWN сохраняются; ERR_STATE не меняет out. */
int stream_network_telemetry(struct UTUN_INSTANCE *inst, const struct stream_descriptor *descriptor, struct stream_telemetry *out);

/* Внутренний lifecycle: занять свободные raw/router bindings, подписку REINIT и timer100мс; out заранее NULL.
 * OK либо ERR_STATE/MEMORY/CRYPTO. Отказ снимает свои ресурсы; descriptor/flow здесь ещё нет. */
int stream_network_init(struct UTUN_INSTANCE *inst, struct stream_network **out);
/* Вне callbacks/pump: снять bindings/REINIT/timer/waiters/NCD, отсоединить caller-owned receives. NULL допустим. */
void stream_network_destroy(struct stream_network *network);
/* Разбудить peer pumps после изменения кольца/END; не выполняет отправку синхронно. NULL/stopping игнорируются. */
void stream_network_changed(struct stream_network *network);
/* Объединённый отложенный проход управления; не запускает selector внутри packet/NCD callbacks. */
void stream_network_wake(struct stream_network *network);
/* Закрыть flows/relay-запросы точной group/source/epoch до освобождения её кольца; чужие группы/ETCP остаются. */
void stream_network_cancel(struct stream_network *network, const struct stream_descriptor *descriptor);
/* После изменения принятого профиля self/пира закрыть недопущенные flows указанного канала; кольцо не освобождает. */
void stream_network_membership_changed(struct stream_network *network, const char *channel);
/* Observer кольца: учесть добавление/вытеснение части ещё не переданной downstream; DATA не копируется/не отправляется. */
void stream_network_cache_event(struct stream_network *network, const struct stream_descriptor *descriptor,
                                const uint8_t *wire, size_t length, int added);
#ifdef __cplusplus
}
#endif
#endif
