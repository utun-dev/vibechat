/* stream — публикация и приём трансляций CHAT-группы с подписью источника и восстановлением потерь через Reed–Solomon.
 * Источник: publish_open → publish_write или publish_block → publish_finish → close.
 * Приём: subscribe_open для событий байтового потока либо media_open → media_read для аудио/видео; затем close.
 * Одна живая сессия/RS-кольцо на группу и источник; эпоха и режим выбора поставщиков общие для её handles.
 * Все вызовы — uasync-поток. Для открытия нужны запущенный чат, живая CHAT-группа и проверенный собственный мембер.
 * Коды результатов — STREAM_* из stream_proto.h; открываемый *out заранее NULL и не меняется при отказе. */

/* Владение и callbacks:
 * Последний owner close закрывает сессию; ресурсы освобождаются после возврата активных операций/callbacks.
 * Publisher close после FIN сохраняет кольцо для оставшихся owners, без FIN отсоединяет их.
 * Отсоединённый handle и его media cursor также закрывает вызывающий, в том числе после destroy сервиса/ядра.
 * DATA/GAP/END относятся к общей позиции сессии и не повторяются для каждого handle; media cursors независимы.
 * CLOSED/AVAILABLE тоже относятся к сессии. Callback отложен, event/data заимствованы до его возврата.
 * Close handles из callback допустим; остановка сервиса/группы и destroy ядра — вне callbacks.
 * Источник и локальные подписки публикуются атомарно через опции собственного мембера. */
#ifndef STREAM_H
#define STREAM_H

#include "stream_buffer.h"
#include "stream_media_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

struct UTUN_INSTANCE;
struct STREAM_HANDLE;
struct stream_service;

/* Значения по умолчанию; действующие бюджеты читаются из [chatserver] через stream_limits_get().
 * Все трансляции экземпляра делят regular; temporary принадлежит пробам и перекрытиям. */
#define STREAM_REGULAR_LIMIT 192
#define STREAM_TEMP_LIMIT 64
#define STREAM_BACKGROUND_LIMIT 16
#define STREAM_PEER_TEMP_LIMIT 16
#define STREAM_PEER_LIMIT 64
#define STREAM_CONTROL_LIMIT 128
#define STREAM_EXTRA_RECEIVE_LIMIT 16

struct stream_limits {
    unsigned regular, temporary; // Постоянные DATA/резервы и временные операции всего экземпляра.
    unsigned background, peer_temporary; // Подмножества temporary: фоновые операции и операции с одним пиром.
    unsigned peers, control; // Число пиров и окно незавершённых запросов каждого направления к одному пиру.
    unsigned extra_receive; // Дополнительные принимаемые части сверх k для одной трансляции.
};
/* Заполняет out настройками экземпляра; NULL inst даёт defaults. Дочерние бюджеты ограничены temporary. */
void stream_limits_get(struct UTUN_INSTANCE *inst, struct stream_limits *out);

struct stream_parameters {
    uint8_t n, r;       // n — всего RS-частей, r — проверочных; 1<=n<=255, r<n; n-r<=настроенного regular.
    uint16_t block_size; // Максимум RS-кодограммы без внешнего заголовка; параметры проверяет stream_descriptor_validate.
};

enum stream_event_type { STREAM_EVENT_DATA, STREAM_EVENT_GAP, STREAM_EVENT_END, STREAM_EVENT_CLOSED, STREAM_EVENT_AVAILABLE };
enum stream_close_reason {
    STREAM_CLOSE_STOPPED, STREAM_CLOSE_GROUP, STREAM_CLOSE_SERVICE, STREAM_CLOSE_SOURCE_CHANGED,
    STREAM_CLOSE_MEMBER, STREAM_CLOSE_FAILED, STREAM_CLOSE_OFFLINE, // Новый viewer не открыт: у источника нет BGP-пути группы.
    STREAM_CLOSE_CAPACITY // Формат требует больше постоянных сетевых слотов, чем поддерживается; повтор сам по себе не поможет.
};

struct stream_event {
    struct stream_descriptor descriptor; // источник, группа и эпоха трансляции
    enum stream_event_type type;         // DATA, GAP, END, CLOSED или готовность медиа AVAILABLE
    uint64_t first_seq, last_seq;        // DATA/GAP: включительный диапазон пачек; END: последняя пачка, UINT64_MAX при пустом потоке
    const uint8_t *data;                 // DATA: восстановленные байты, заимствованы до возврата callback
    size_t length;                      // размер data в байтах
    int reason;                         // CLOSED: stream_close_reason
};
typedef void (*stream_event_fn)(struct UTUN_INSTANCE *inst, const struct stream_event *event, void *arg);

struct stream_info {
    struct stream_descriptor descriptor; // источник, эпоха и параметры RS
    struct stream_buffer_stats buffer; // текущее состояние общего кольца
    unsigned owners; // число локальных handles общей сессии
    int publisher; // 1 — сессия содержит локальный источник
    int finished; // 1 — источник завершён; оставшиеся данные ещё могут читаться
};

struct stream_candidate {
    struct stream_address address; // Узел с живым BGP-путём и проверенным членством/подпиской либо подтверждённой relay-помощью.
    uint8_t client_type; // CLIENT_TYPE_*; UINT8_MAX, если общей записи TOPO_NODE нет.
    uint8_t source;      // 1 — сам источник; 0 — промежуточный поставщик.
};

struct stream_selection_settings {
    uint32_t migration_confirm_ms; // Сколько непрерывно подтверждать улучшение; по умолчанию 2000.
    uint32_t minimum_hold_ms;      // Минимальное удержание основной выдачи перед переносом; по умолчанию 5000.
    uint32_t score_hysteresis_ms;  // Порог улучшения интегральной оценки; по умолчанию 20. Все поля 0..60000 мс.
};
/* Фаза общей доставки; состояние отдельного player и его оставшийся запас определяются отдельно. */
enum stream_delivery_phase {
    STREAM_DELIVERY_NONE,       // Ручная выдача/локальный publisher без автоматического selector.
    STREAM_DELIVERY_BOOTSTRAP,  // Ещё нет первой проверенной пачки; для media — полного KEY.
    STREAM_DELIVERY_CATCHUP,    // Догон после смены KEY либо превышения предела отставания при продолжающихся DATA.
    STREAM_DELIVERY_LIVE,       // Доставка в пределах заданного content lag; это не гарантия отсутствия пауз автора.
    STREAM_DELIVERY_RECOVERY,   // Поиск недостающих данных; причина паузы может быть на любом участке, включая автора.
    STREAM_DELIVERY_WAITING,    // Ожидание маршрута/DATA после grace либо целого бюджета k; просмотр сохранён.
    STREAM_DELIVERY_DRAINING,   // Подписанный END известен, оставшиеся DATA ещё принимаются.
    STREAM_DELIVERY_FINISHED    // Общий курсор прошёл подписанный конец; локальные players могут ещё доигрывать буфер.
};
struct stream_selection_info {
    uint64_t probes_started, cycles; // Начатые узловые INFO-пробы и номер цикла обхода.
    uint64_t migrations, failures;   // Завершённые переносы и отказы основных/заменяющих DATA-выдач.
    uint64_t restarts;               // Попытки восстановления после 5с без полной проверенной пачки.
    unsigned candidates, probes_active; // Текущий roster и незавершённые INFO-пробы.
    unsigned subflows, overlaps, providers; // Основные выдачи, заменяющие выдачи и узлы основных выдач.
    int automatic;                  // 1 — у общей сессии есть автоматический selector.
    unsigned working_parts, maximum_dependency; // Живые различные части m и максимум частей через общий промежуточный узел.
    uint64_t live_lag_ms, live_jumps; // Нижняя граница отставания по PTS содержимого; число переходов на свежий KEY по лимиту.
    int live_lag_valid, live_waiting_key; // PTS известны; лимит превышен, но общего доступного KEY пока нет.
    int protected_by_fec;           // r>0, m>=k=n-r; потеря любого промежуточного узла оставляет >=k частей. Автор исключён.
    enum stream_delivery_phase phase; // Фаза доставки, независимая от паузы/скорости локального player.
    uint64_t phase_since_ms;          // Монотонное время последней смены фазы;0 без автоматического selector.
};

// ── Жизненный цикл сервиса ──

// Чат создаёт контекст после core start/DB/groups и очищает старые свои stream-опции; повторный init успешен.
int stream_service_init(struct UTUN_INSTANCE *inst);

// Остановить трансляции и отсоединить handles; вызывающий закрывает оставшиеся handles сам. Вне callbacks.
void stream_service_destroy(struct UTUN_INSTANCE *inst);

// Отсоединить трансляции группы при её удалении; handles остаются у владельцев до close. Вне callbacks.
void stream_cancel_group(struct UTUN_INSTANCE *inst, uint64_t group_id);

// ── События и каталог источников ──

// Заменить единственный обработчик/arg сервиса; arg живёт до замены/снятия. NULL отключает события, не приём/индексацию.
void stream_set_event_callback(struct UTUN_INSTANCE *inst, stream_event_fn callback, void *arg);

/* Uasync: READ имён через общий DB worker, затем online/подтверждённые профили из RAM.
 * done(arg,result,json) ровно один раз, отказ до принятия может ответить синхронно; done обязателен.
 * При OK json принадлежит callback (u_free), иначе NULL. inst/arg живут до ответа/закрытия DB owner.
 * JSON: id(Ed25519 в 64 hex-символах),epoch(hex string),name,self; имя local_nick, затем общее имя узла.
 * Порядок — SQL ORDER BY ed25519_pubkey, пустой каталог "[]"; WAIT — группа недоступна.
 * До65535 текущих строк, имя не длиннее MS_USERINFO_MAX; частичные результаты не выдаются.
 * Только автономная :memory: без worker допускает немедленное RAM-чтение. */
typedef void (*stream_sources_done_fn)(void* arg, int result, char* json);
void stream_sources_read(struct UTUN_INSTANCE* inst, uint64_t group_id, stream_sources_done_fn done, void* arg);

/* Разрешает Ed25519 CHAT-группы во внутренний маршрут. Только uasync. 0 — группа недоступна. */
uint64_t stream_group_route(struct UTUN_INSTANCE *inst, struct stream_address group);
/* Независимое наблюдение owner: начальный снимок и изменения через CHAT_EVT_STREAM_SOURCES.
 * group_id=0 снимает только этого owner; owner=0 разрешён для выбранного канала frontend.
 * Недоступный каталог имеет valid=0 и не означает завершение источников.
 * Payload: group Ed25519:32, valid:1, JSON без завершающего NUL. Одинаковые снимки объединяются.
 * Замена группы сбрасывает прежний снимок owner. Не создаёт подписку/буфер и не удерживает ACTIVE. Только uasync. */
int stream_watch_sources(struct UTUN_INSTANCE *inst, uint64_t group_id, uint64_t owner);

// ── Согласование с активностью ядра и рацией ──

// Любой owner живой сессии, включая relay и завершённую сессию, удерживает ACTIVE; после закрытия восстанавливается запрос UI.
int stream_service_needs_activity(const struct UTUN_INSTANCE *inst);

// Только локальный viewer этой группы/эпохи подавляет рацию источника со включённым звуком.
int stream_radio_muted(struct UTUN_INSTANCE *inst, uint64_t group_id, const uint8_t ed[32]);

// ── Открытие источника и подписок ──

/* Открыть источник; *out заранее NULL. parameters=NULL использует STREAM_DEFAULT_N/R: 8 частей без избыточности.
 * Максимальный размер части задаёт STREAM_RS_DEFAULT_SIZE; одна подпись защищает всю пачку внутри RS.
 * Переданные параметры заменяют defaults целиком. Одна публикация этого узла в группе; эпоха случайная и ненулевая.
 * Существующая завершённая публикация отсоединяется перед новым open; её owners получают SOURCE_CHANGED.
 * k=n-r>stream_regular_limit возвращает STREAM_ERR_CAPACITY до изменения профиля/старой публикации.
 * При успехе handle закрывает вызывающий. */
int stream_publish_open(struct UTUN_INSTANCE *inst, uint64_t group_id, const struct stream_parameters *parameters,
                        struct STREAM_HANDLE **out);

// Меняет подписанное состояние звука, не формат медиатреков. Только uasync; raw publisher начинает с 0.
int stream_publish_set_audio_enabled(struct STREAM_HANDLE *handle, int enabled);

/* Параметры берутся из подписанного профиля источника; дубликаты локальных владельцев делят один буфер.
 * Уже открытая сессия может использоваться и после FIN; при её отсутствии нужен published в профиле.
 * WAIT — профиль/публикация ещё не готовы либо у источника нет живого BGP-пути этой группы, включая reuse сессии.
 * Новая автоматическая сессия отклоняет k=n-r>stream_regular_limit с STREAM_ERR_CAPACITY до выделения ресурсов.
 * Потеря последнего пути сохраняет DATA. После30с без BGP и новых частей освобождается сеть; reader/буфер остаются.
 * Возврат BGP возобновляет automatic приём; manual владелец заново открывает выдачи. Отзыв прав/смена эпохи закрывают сразу.
 * Режим первой сессии общий: manual и automatic не смешиваются. Локальный publisher допускает оба вида readers. */
int stream_subscribe_open(struct UTUN_INSTANCE *inst, uint64_t group_id, struct stream_address source, struct STREAM_HANDLE **out);

// Ручные выдачи для диагностики/явного управления; режим общий для всех owners экземпляра и не смешивается с automatic.
int stream_subscribe_manual_open(struct UTUN_INSTANCE *inst, uint64_t group_id, struct stream_address source, struct STREAM_HANDLE **out);

// ── Выбор поставщиков и состояние handles ──

// По умолчанию: подтверждение улучшения 2000 мс, удержание 5000 мс, гистерезис интегральной оценки 20 мс.
int stream_selection_configure(struct STREAM_HANDLE *handle, const struct stream_selection_settings *settings);

// Скопировать снимок последнего прохода selector; без selector — нули. STREAM_OK/STREAM_ERR_STATE.
int stream_selection_get_info(const struct STREAM_HANDLE *handle, struct stream_selection_info *info);

// Освободить свой handle, в том числе после CLOSED/удаления группы; NULL допустим. После close указатель недействителен.
void stream_close(struct STREAM_HANDLE *handle);

// 1 — связан с незакрытой и неинвалидированной сессией, включая FIN; 0 — иначе. Свежесть данных не проверяется.
int stream_handle_active(const struct STREAM_HANDLE *handle);

// Причина отсоединения из stream_close_reason; для NULL — STREAM_CLOSE_STOPPED.
int stream_handle_reason(const struct STREAM_HANDLE *handle);

// Скопировать состояние общей сессии и кольца; STREAM_OK или STREAM_ERR_STATE.
int stream_get_info(const struct STREAM_HANDLE *handle, struct stream_info *info);

// ── Чтение аудио/видео ──

/* Медиаплеер использует тот же RS-buffer, но собственные позиции чтения.
 * tracks=0 подготавливает только общий индекс; VIDEO или VIDEO|AUDIO создаёт cursor, если его ещё нет.
 * Существующий cursor сохраняет прежние tracks; tracks=0 не отключает его. История индексируется отложенно с бюджетом.
 * Все вызовы — uasync. Заимствованный packet действует до следующего read/close.
 * LIVE DATA/GAP/END callbacks описывают сетевой прогресс; конец отложенного просмотра получает media_read. */
int stream_media_enable(struct STREAM_HANDLE *handle, unsigned tracks);

/* Атомарно открыть viewer выбранной эпохи с собственным cursor. CONFLICT означает смену источника.
 * Требует опубликованную выбранную эпоху в профиле, даже при наличии старого локального кольца.
 * tracks=VIDEO или VIDEO|AUDIO; out заранее NULL, при отказе остаётся NULL. Закрытие через stream_close. */
int stream_media_open(struct UTUN_INSTANCE *inst, uint64_t group_id, struct stream_address source, uint64_t epoch,
                       unsigned tracks, struct STREAM_HANDLE **out);

struct stream_audio_meter;
/* Получить свою ссылку на счётчик плеера, NULL без аудиокурсора. Handle жив до возврата; release через stream_audio_meter_release. */
struct stream_audio_meter *stream_media_audio_meter(struct STREAM_HANDLE *handle);

/* Прочитать ближайшую медиазапись: want_video=1 разрешает видео, звук ограничен position_us+audio_ahead_us.
 * position_us<0 — прогрев перед KEY; audio_ahead_us=-1 выключает чтение звука, иначе 0..60000000 мкс.
 * При позднем начале звука окно считается минимум от первого Opus; видео выдаётся независимо от часов показа.
 * STREAM_OK — packet заполнен, его kind также сообщает конец трека/потока; WAIT/MORE — повторить позже.
 * GAP/EXPIRED требуют новой точки KEY через resync; отрицательный результат — ошибка. Буфер packet заимствован. */
int stream_media_read(struct STREAM_HANDLE *handle, int64_t position_us, int want_video, int64_t audio_ahead_us,
                       struct stream_media_packet *packet);

// Снимок курсора с текущим звуком источника: при No audio audio_enabled=0, ready_us ограничен только видео. OK/ERR_*.
int stream_media_get_status(struct STREAM_HANDLE *handle, struct stream_media_status *status);

// Сбросить только свой медиакурсор для поиска новой точки KEY; STREAM_OK или STREAM_ERR_STATE.
int stream_media_resync(struct STREAM_HANDLE *handle);

// ── Публикация байтового потока ──

// Максимальная порция одного write/block в байтах; 0 — неактивный handle или не источник.
size_t stream_publish_capacity(const struct STREAM_HANDLE *handle);

/* Скопировать length <= stream_publish_capacity(handle) байт в источник; accepted обязателен даже при ошибке.
 * accepted обнуляется до проверок и сохраняет принятую часть при отказе. Нулевой write допускает data=NULL.
 * Неполная пачка ждёт до 100мс. Входной буфер остаётся у вызывающего; STREAM_OK либо отрицательная ошибка.
 * Ошибка, кроме ERR_ARGUMENT, закрывает сессию; handle всё равно закрывает вызывающий. */
int stream_publish_write(struct STREAM_HANDLE *handle, const uint8_t *data, size_t length, size_t *accepted);

/* Скопировать одну непустую порцию размером <= capacity как отдельную FEC-пачку; STREAM_OK или отрицательная ошибка.
 * Входной буфер остаётся у вызывающего. Не смешивать с publish_write в одном запуске. */
int stream_publish_block(struct STREAM_HANDLE *handle, const uint8_t *data, size_t length);

// Идемпотентно снять публикацию, сбросить остаток и подписать END; DATA перед END. Owner/кольцо сохраняются до close.
int stream_publish_finish(struct STREAM_HANDLE *handle);

// ── Кандидаты для сетевых выдач ──

/* Проверенные online-поставщики этой группы/эпохи: подписка либо подтверждённая relay-помощь.
 * Мобильный промежуточный узел допускается только с подписанной ролью supernode; сам источник допускается.
 * Relay-only сессия не выбирает другие relay-only узлы. out заранее NULL, count обязателен; массив освобождается u_free().
 * При пустом результате out=NULL/count=0. Наличие кандидата не гарантирует совместимый прямой транспорт. */
int stream_get_candidates(const struct STREAM_HANDLE *handle, struct stream_candidate **out, size_t *count);

#ifdef __cplusplus
}
#endif
#endif
