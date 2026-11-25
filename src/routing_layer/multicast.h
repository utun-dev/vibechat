/* multicast — отправка одного payload явно указанному списку узлов topo-группы.
 * Общая часть пути передаётся одним пакетом; при расхождении маршрутов пакет
 * разделяется на ветки с соответствующими подмножествами получателей.
 * Источник: open → сообщить свой дескриптор получателям → send → close.
 * Получатель: subscribe на источник/дескриптор до отправки DATA → on_data → unsubscribe.
 * Все операции — uasync; callbacks отложены, их данные заимствованы до возврата.
 * Доставка без сквозных ACK/повторов; потери приходят через on_loss, но сам отчёт тоже может потеряться. */

/* Подробные правила дескрипторов, подписок и владения.
 * Отправка:
 *   1. multicast_open(group, on_loss, arg) выделяет исходящий дескриптор.
 *      Номер 1..65535 уникален среди открытых дескрипторов данного ядра.
 *      Выдача последовательная: 1, 2, ... 65535, затем снова с 1, пропуская
 *      занятые номера. Счётчик общий для всех групп ядра; close его не откатывает.
 *      Группа и обработчик потерь закрепляются за ним до закрытия.
 *   2. Передать получателям свой ключ Ed25519 и номер дескриптора через протокол
 *      использующего сервиса. Получатели должны установить подписки до DATA.
 *   3. multicast_send(inst, descriptor, recipients, count, payload, len).
 *      Можно отправлять много пакетов; список адресатов задаётся для каждого.
 *      Список и payload копируются: после возврата их можно менять/освобождать.
 *   4. Перегрузки и локальные ошибки приходят в on_loss этого дескриптора.
 *      loss->descriptor указывает исходящий дескриптор, loss->recipients —
 *      только пострадавшую часть списка. Решение о снижении нагрузки принимает сервис.
 *   5. multicast_close(inst, descriptor) завершает использование дескриптора.
 *
 * Как принимать:
 *   1. Получить ключ Ed25519 источника и ЕГО исходящий дескриптор от сервиса.
 *   2. multicast_subscribe(group, src_node, src_descriptor, on_data, arg).
 *      Подписка локальная, ключ — (group_id, src_node, src_descriptor).
 *      Она не посылает JOIN и не добавляет нас в список адресатов у источника.
 *      Собственный исходящий дескриптор для приёма выделять не требуется.
 *   3. on_data вызывается только для пакетов, где мы указаны получателем.
 *      Без подписки локальная доставка тихо отбрасывается, без лога и отчёта.
 *      Транзит другим получателям продолжается независимо от наших подписок.
 *   4. multicast_unsubscribe(subscription) прекращает приём и освобождает handle.
 *
 * Минимальный порядок (на разных узлах; on_loss/on_data — callbacks сервиса):
 *   int fd = multicast_open(send_group, on_loss, sender_ctx);
 *   if (fd < 0) ...;                 // дескриптор не создан
 *   // сервис передаёт source_ed и (uint16_t)fd получателю
 *   struct multicast_subscription* sub =
 *       multicast_subscribe(recv_group, source_ed, (uint16_t)fd, on_data, receiver_ctx);
 *   if (!sub) ...;                   // подписка не создана
 *   // после готовности подписки, на источнике:
 *   int rc = multicast_send(send_inst, (uint16_t)fd, receivers, count, data, size);
 *   if (rc < 0) ...;                 // этот вызов не принят
 *   // при завершении, каждый на своём узле:
 *   multicast_unsubscribe(sub);
 *   multicast_close(send_inst, (uint16_t)fd);
 *
 * Условия использования:
 * - Все вызовы и callbacks выполняются в uasync-потоке соответствующего ядра.
 * - Callbacks отложены: open/subscribe/send/close/unsubscribe их не вызывают.
 *   В callback можно отправлять, закрывать свой дескриптор и отписывать себя.
 *   Уничтожать ядро/группу/модуль из callback нельзя.
 * - payload, loss и loss->recipients заимствованы до возврата из callback.
 *   arg принадлежит сервису: он должен жить до close/unsubscribe и завершения
 *   уже выполняющегося callback. После закрытия новых callbacks не будет.
 * - Подпиской владеет вызывающий: unsubscribe обязателен даже после удаления
 *   группы. Удаление группы/ядра деактивирует подписку, но handle остаётся
 *   валиден до unsubscribe; после unsubscribe использовать его нельзя.
 * - Удаление группы автоматически закрывает её исходящие дескрипторы.
 * - Успех send означает принятие к обработке, а не доставку. Возможны частичные
 *   потери; сквозных ACK/повторов нет, порядок при смене маршрута не гарантируется.
 *   Сам отчёт тоже может потеряться: отсутствие on_loss не подтверждает доставку.
 *
 * Локальные callbacks DATA тоже отложены; их общая очередь ограничена 256 КиБ
 * памяти. Отказ её выделения/переполнение считается MULTICAST_INTERNAL_ERROR.
 * Поколений нет: DATA, отчёты и подписки используют 16-битный номер дескриптора.
 * Новое ядро начинает выдачу с 1. После повторной выдачи номера запоздалый
 * сетевой пакет с тем же источником и номером неотличим от пакета нового владельца.
 */
#ifndef UTUN_MULTICAST_H
#define UTUN_MULTICAST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct UTUN_INSTANCE;
struct TOPO_GROUP;
struct multicast_ctx;
struct multicast_subscription;  /* непрозрачный handle локальной подписки; освобождает вызывающий */

#define MULTICAST_DESCRIPTOR_INVALID 0
#define MULTICAST_QUEUE_BYTES (256u * 1024u) /* одна FIFO на ETCP-соединение для всех рассылок */
#define MULTICAST_REPORT_BYTES (16u * 1024u) /* общий бюджет накопленных отчётов и окон троттлинга */
#define MULTICAST_REPORT_INTERVAL_TB 3000    /* 300 мс; timebase = 0.1 мс */

// Лимит протокола multicast включает payload, адресатов и резерв всего пути.
#define MULTICAST_MAX_DGRAM_SIZE (16u * 1024u)
#define MULTICAST_MAX_VISITED 64
#define MULTICAST_KEY_SIZE 32
#define MULTICAST_GROUP_OFF 1
#define MULTICAST_DESCRIPTOR_OFF 33
#define MULTICAST_COUNT_OFF 35
#define MULTICAST_SOURCE_OFF 37
#define MULTICAST_HEADER_SIZE (MULTICAST_SOURCE_OFF + MULTICAST_KEY_SIZE)
#define MULTICAST_PATH_RESERVE ((1 + MULTICAST_KEY_SIZE) * MULTICAST_MAX_VISITED + 1)
#define MULTICAST_REPORT_HEADER_SIZE (25 + 2 * MULTICAST_KEY_SIZE)

enum multicast_loss_reason {
    MULTICAST_OVERLOAD = 1,       /* очередь выбранного исходящего соединения заполнена */
    MULTICAST_NO_ROUTE,           /* только локальное уведомление источнику */
    MULTICAST_PATH_LOOP,          /* локальное уведомление; транзит логирует потерю */
    MULTICAST_INTERNAL_ERROR      /* выделение памяти / отказ транспорта */
};

/* Полная Ed25519-идентичность источника, адресата или участника пути. */
struct multicast_key { uint8_t ed[MULTICAST_KEY_SIZE]; };

struct multicast_loss {
    uint64_t group_id;            /* группа исходной рассылки */
    struct multicast_key source;  /* источник исходного DATA */
    struct multicast_key src, dst; /* узел потери и выбранный next hop; dst=0 без маршрута */
    uint16_t descriptor;          /* исходящий дескриптор, callback которого получает этот отчёт */
    uint16_t recipient_count;    /* число ключей в recipients */
    const struct multicast_key* recipients;   /* заимствованный массив полных ключей */
    uint32_t packets;            /* потери для каждого получателя этого списка */
    enum multicast_loss_reason reason; /* перегрузка либо локальная причина потери */
    uint8_t prefix_len;           /* 0..16 значимых байт */
    uint8_t prefix[16];           /* начало payload первого потерянного пакета */
};

/* Приём для конкретной подписки. len может быть 0; payload нельзя сохранять без копирования.
 * subscription можно передать в unsubscribe прямо здесь; после этого handle недействителен.
 * src_node/src_descriptor/group известны сервису из subscribe и могут храниться в arg. */
typedef void (*multicast_recv_fn)(struct multicast_subscription* subscription, const uint8_t* payload, size_t len, void* arg);

/* Уведомление конкретному исходящему дескриптору. loss->src/dst — перегруженный
 * участок. packets относится к каждому ключу в recipients.
 * Допустим multicast_close(inst, loss->descriptor) из этого callback. */
typedef void (*multicast_loss_fn)(struct UTUN_INSTANCE* inst, const struct multicast_loss* loss, void* arg);

struct multicast_stats {
    uint64_t sent;                /* ветки DATA, переданные ETCP */
    uint64_t delivered;           /* вызовы обработчиков входящих подписок */
    uint64_t unsubscribed;        /* тихие отбрасывания локальной доставки без подписки */
    uint64_t overload_packets;   /* ветки, отброшенные из-за лимита исходящей FIFO */
    uint64_t no_route_packets;   /* ветки без доступного маршрута */
    uint64_t loop_packets;       /* петли и превышения длины пройденного пути */
    uint64_t invalid_packets;    /* некорректные DATA/отчёты */
    uint64_t internal_errors;    /* отказы выделения памяти или передачи транспорту */
    uint64_t reports_sent;       /* переданные отчёты, включая локальные уведомления */
    uint64_t reports_received;   /* принятые отчёты */
    uint64_t reports_evicted;    /* записи отчётов, вытесненные из бюджета 16 КиБ */
    size_t queued_bytes;         /* DATA в FIFO модуля, без уже переданного ETCP */
    size_t report_bytes;         /* память накопленных отчётов и окон, без переданного router */
    size_t delivery_bytes;       /* память DATA, ожидающих локальные callbacks; максимум 256 КиБ */
    unsigned queues;             /* лениво созданные FIFO, в том числе временно опустевшие */
    unsigned descriptors;        /* открытые исходящие дескрипторы */
    unsigned subscriptions;      /* активные входящие подписки */
};

// ── API отправителя ──

/* Открыть исходящий дескриптор в работающей group. on_loss обязателен; arg может быть NULL.
 * Выдать следующий свободный номер по циклическому порядку 1..65535, начиная после
 * последнего выданного; 0 не выдаётся. Закрытые номера используются только при обходе.
 * Возврат: 1..65535 — выделенный номер, <0 — ошибка (включая отсутствие свободных номеров).
 * До проверки результата не приводить его к uint16_t. Подписку на приём не создаёт. */
int multicast_open(struct TOPO_GROUP* group, multicast_loss_fn on_loss, void* arg);

/* Отправить через открытый descriptor, принадлежащий inst; группа берётся из дескриптора.
 * recipients: count>0 ненулевых ключей Ed25519; повторы удаляются, собственный ID допустим.
 * len может быть 0; при len>0 payload обязателен. Размер заголовка, payload, списка и полного
 * резерва пройденного пути должен помещаться в 16 КиБ. Лимиты count и полного
 * размера проверяются до удаления дублей из входного списка.
 * 0=принято к обработке; <0=вызов не принят, ничего не отправлено.
 * После принятия отдельные ветки могут потеряться; уведомления идут в on_loss.
 * Обе входные области копируются и после возврата больше не используются. */
int multicast_send(struct UTUN_INSTANCE* inst, uint16_t descriptor, const struct multicast_key* recipients,
                   uint16_t count, const void* payload, size_t len);

/* Закрыть исходящий descriptor данного inst: убрать его из поиска, отменить
 * ещё не переданные DATA и ожидающие локальные callbacks этого дескриптора.
 * Уже переданное транспорту/другим узлам отозвать нельзя. Удалённые подписки
 * автоматически не закрываются. Сохранённый номер после close не использовать.
 * Для отсутствующего дескриптора — диагностируемая ошибка без побочных действий. */
void multicast_close(struct UTUN_INSTANCE* inst, uint16_t descriptor);

// ── API получателя ──

/* Подписаться на локальную доставку (group_id, src_node, src_descriptor).
 * src_node — 32 байта Ed25519 источника, src_descriptor!=0, on_data обязателен; arg может быть NULL.
 * Возврат: принадлежащий вызывающему handle либо NULL с диагностикой ошибки.
 * Повторная подписка на тот же ключ запрещена; сначала unsubscribe прежней.
 * Никаких сетевых запросов не отправляет и пропущенные ранее DATA не воспроизводит. */
struct multicast_subscription* multicast_subscribe(struct TOPO_GROUP* group, const uint8_t src_node[MULTICAST_KEY_SIZE], uint16_t src_descriptor,
                                                    multicast_recv_fn on_data, void* arg);

/* Прекратить доставку, отменить ожидающие callbacks и освободить handle.
 * NULL допустим. Работает и для подписки, деактивированной удалением группы/ядра.
 * После вызова handle недействителен; повторный вызов с тем же указателем запрещён.
 * Из собственного on_data допускается; память освобождается после его завершения. */
void multicast_unsubscribe(struct multicast_subscription* subscription);

// ── Диагностика ──

/* Снимок общих счётчиков ядра в *out. 0=успех, <0=неверный аргумент/модуль не создан.
 * Не открывает дескрипторов или подписок, счётчики не сбрасывает. */
int multicast_get_stats(struct UTUN_INSTANCE* inst, struct multicast_stats* out);

// ── Внутренняя интеграция с ядром ──

/* Далее — интеграция с ядром; сервисы используют open/send/close и subscribe/unsubscribe.
 * init вызывается ядром один раз после etcp_router_init; 0=успех, <0=ошибка.
 * destroy отменяет работу, закрывает дескрипторы, деактивирует подписки; повтор допустим.
 * Вызывать до уничтожения router/ETCP и вне callbacks модуля. */
int multicast_init(struct UTUN_INSTANCE* inst);

void multicast_destroy(struct UTUN_INSTANCE* inst);

// Запланировать пересмотр ожидающих DATA после изменения маршрутов; повторные вызовы объединяются.
void multicast_routes_changed(struct UTUN_INSTANCE* inst);

/* uasync после commit допуска CHAT: убрать запрещённые доставки, подписки, отчёты и ветки общих FIFO.
 * Отцепленный subscription остаётся у владельца до unsubscribe; UTUN сохраняет прежние правила.
 * Self/сосед/указанный source проверяются по текущему допуску; raw source не является сквозной подписью. */
void multicast_revalidate_members(struct UTUN_INSTANCE* inst, uint64_t group_id);

/* Перед удалением группы: закрыть её дескрипторы, деактивировать подписки,
 * убрать её DATA/отчёты из общих очередей, сохранив работу остальных групп. */
void multicast_close_group(struct TOPO_GROUP* group);

/* Wire DATA. Числовые поля little-endian, кроме short31 в сетевом порядке.
 * cmd:1 | group_key:32 | descriptor:2 | count:2 | source:32 | recipients:32*count |
 * hops:1 | visited | payload.
 * Каждый visited — short31 (4 байта) или 0x80 и 32 байта Ed25519.
 * В очереди хранятся полные ключи. Перед отправкой формат выбирается по текущему индексу группы.
 * Заголовок (45), адресаты, payload и резерв пути (1 + 64*33) вместе занимают не более 16384 байт.
 * Один список содержит до 436 адресатов, чтобы все они поместились в один защищённый OVERLOAD.
 * Ключи получателей лексикографически возрастают. Первый visited=source, последний=ETCP peer.
 * Приём разрешён от ACCEPT-сессии своей группы, до обязательного завершения
 * её таблицы. Каждый получатель добавляет себя до локальной доставки/транзита;
 * повторное посещение и вход с hops=64 отбрасываются целиком, включая локальную доставку.
 *
 * Wire OVERLOAD — payload отдельного router-сервиса:
 * descriptor:2 | src:32 | dst:32 | packets:4 | count:2 | prefix_len:1 |
 * prefix:16 | recipients:32*count.
 * src/dst — концы перегруженного участка; router dst — исходный источник DATA.
 * Разные наборы потерянных получателей не смешиваются. Префикс относится
 * к первому потерянному пакету записи; для payload короче 16 байт остаток нулевой.
 * Первая потеря сообщается сразу, последующие накапливаются до конца окна 300 мс.
 * Таймер отправляет остаток независимо от поступления новых DATA.
 */

#ifdef __cplusplus
}
#endif
#endif
