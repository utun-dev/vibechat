/* Модуль определяет тип NAT и доступность входящих для каждого рабочего сокета.
 * Результат — address_profile с двумя независимыми свойствами:
 * mapping: REAL_IP — адрес не меняется; EIM — внешний IP:порт одинаков для разных IP назначения;
 *          DEPENDENT (strict) — внешний IP:порт зависит от IP назначения.
 * inbound: OPEN — подтверждён вход; CONTACT — пакеты проходят после исходящего контакта (оценка filtered).
 * Принятое UDP/TCP-подключение с публичными локальным и удалённым IP сразу даёт REAL_IP+OPEN (direct/open).
 * Для этого правила прежний исходящий контакт с IP пира не препятствие; LAN и SOCKS его не подтверждают.
 * UNKNOWN в любом свойстве означает, что данных для его определения пока недостаточно.
 * Одно соединение может подтвердить NAT-адрес: он сохраняется в socket->nat_addr с типом UNKNOWN
 * и публикуется как NAT-кандидат. Это не адрес интерфейса и не подтверждение открытого входа.
 * Для UDP сочетания EIM+OPEN и EIM+CONTACT означают eim open и eim filtered.
 * Для TCP непубличный локальный IP сразу означает NAT; публичный IP сравнивается с наблюдением одного пира.
 * TCP не запускает активных проб входа: OPEN даёт только принятое подключение; внешний IP сохраняется с портом 0.
 * EIM+UNKNOWN отображается как EIM?: внешний адрес совпал, незнакомый вход не удалось проверить.
 * Если своего измерения mapping нет, совпадение у двух STUN-серверов на том же IP/интерфейсе даёт предварительный EIM?.
 * Его source=STUN, inbound=UNKNOWN; внешний порт берётся только из наблюдения рабочего сокета, а не из STUN.
 * create(instance) запускает автоматические асинхронные проверки через доступные узлы сети.
 * get_status(detector, socket, &status) возвращает результат в status.effective_profile и ход проверки в остальных полях.
 * request_check_*() запрашивает повторную проверку; destroy(detector) останавливает модуль и освобождает его ресурсы.
 * Файловая история контактов читается через общий DB worker, до ответа полнота неизвестна.
 * RW worker сохраняет скопированные пачки одной транзакцией; ошибка снижает доверие к истории,
 * повтор маркера неполноты отложен. Поздние результаты удалённого сокета/детектора игнорируются.
 * Destroy сразу снимает сеть/таймеры; контекст удерживается до последних ответов БД без ожидания в uasync.
 * Все вызовы — в uasync-потоке ядра; detector — его контекст NAT. Алгоритм: doc/nat_detection.txt. */
#ifndef NAT_DETECTION_H
#define NAT_DETECTION_H

#include <stddef.h>
#include <stdint.h>
#include "../lib/socket_compat.h"
#include "address_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

struct UTUN_INSTANCE;
struct ETCP_CONN;
struct ETCP_LINK;
struct ETCP_SOCKET;

/* Закрытый контекст: проверки сокетов, история исходящих IP, задачи помощника и таймеры. */
struct NAT_DETECTION;

/* Почему проверка не завершилась полностью; отсутствие ответа само по себе не определяет NAT-тип. */
enum nat_check_reason {
    NAT_REASON_NONE,                /* Нет причины ошибки. */
    NAT_REASON_NO_OBSERVER,         /* Нет подходящего узла для наблюдения внешнего адреса. */
    NAT_REASON_NO_HELPER,           /* Нет подходящего помощника для проверки входящих. */
    NAT_REASON_CONTROL_LOST,        /* Потерян маршрут для команд и результатов. */
    NAT_REASON_SEND_ERROR,          /* Не удалось отправить тестовый пакет. */
    NAT_REASON_TIMEOUT,             /* Проверка не завершилась за отведённое время. */
    NAT_REASON_ADDRESS_CHANGED,     /* Наблюдаемый адрес изменился; прежний профиль сброшен. */
    NAT_REASON_CONTACT_HISTORY,     /* IP уже был знаком или история неполна: вход не подтверждён как незнакомый. */
    NAT_REASON_PROXY,               /* Сокет работает через SOCKS: его собственный NAT не измеряется. */
    NAT_REASON_NO_LISTENER,         /* Резерв причины в управляющем протоколе; активных TCP-проб сейчас нет. */
    NAT_REASON_CANCELLED,           /* Проверка отменена. */
    NAT_REASON_INVALID_OBSERVATION, /* Наблюдение непригодно, в том числе из-за непубличного source IP. */
    NAT_REASON_DEPENDENT_MAPPING,   /* Определён strict, входящая доступность осталась неизвестной. */
    NAT_REASON_BUDGET               /* Локальная пауза по общему бюджету: текущая попытка будет повторена. */
};

/* Снимок состояния одного сокета для диагностики; получение снимка не запускает проверку. */
struct nat_check_status {
    struct address_profile measured_profile;  /* Сохранённый результат измерения NAT и входящих этого сокета. */
    struct address_profile effective_profile; /* Профиль, выбранный для использования и публикации узлом. */
    uint64_t generation;           /* Поколение сокета: отличает переоткрытие с тем же socket ID. */
    uint64_t round;                /* Уникальный ID текущей или последней попытки. */
    uint64_t group_id;             /* ID группы, через которую передаются команды и результаты. */
    uint8_t observer[32];          /* Ed25519 наблюдателя (B), нули до назначения. */
    uint8_t helper[32];            /* Ed25519 помощника (C), нули при его отсутствии. */
    uint32_t rounds;               /* Сколько попыток начато за время жизни состояния этого сокета. */
    uint32_t helper_reselections;  /* Сколько раз назначен переход к следующей паре кандидатов. */
    uint8_t phase;                /* 0 — ожидание; иначе внутренний номер текущего этапа. */
    uint8_t reason;               /* Причина завершения или неполного результата: enum nat_check_reason. */
    uint8_t history_complete;     /* 1 — история исходящих IP загружена без известных потерь. */
    uint8_t cycling;              /* 1 — идёт обход кандидатов, включая паузу по бюджету; phase=0 во время паузы. */
};

/* Создать детектор для instance с готовым маршрутизатором и группами; автоматические проверки включены.
 * Возвращает контекст или NULL при ошибке, снимая частично установленные обработчики.
 * Ядро сохраняет результат в instance->nat_det до обработки событий: управляющий RX использует это поле.
 * instance должен существовать до destroy(); повторное создание при занятом router-binding запрещено. */
struct NAT_DETECTION *nat_detection_create(struct UTUN_INSTANCE *instance);

/* Остановить проверки, снять обработчики и освободить detector; NULL допустим.
 * Вызывать до teardown ETCP/router и вне callbacks детектора; затем обнулить instance->nat_det.
 * Ядро и рабочие сокеты не освобождаются; внутренние probe_only-сокеты принадлежат детектору. */
void nat_detection_destroy(struct NAT_DETECTION *detector);

/* Сообщить detector об изменении адресов/готовности групп; пересмотр кандидатов отложен и объединяет повторные вызовы. */
void nat_detection_topology_changed(struct NAT_DETECTION *detector);

/* Отменить проверки, помощь другим узлам и наблюдения в группе group_id до закрытия её управляющих каналов. */
void nat_detection_cancel_group(struct NAT_DETECTION *detector, uint64_t group_id);

/* uasync после commit допуска CHAT: отменить пробы/наблюдения с отозванными участниками этой группы.
 * Сохранённый профиль рабочего сокета и операции других групп остаются; контракт UTUN не меняется. */
void nat_detection_revalidate_members(struct NAT_DETECTION *detector, uint64_t group_id);

/* На сколько отложить фоновые UDP-пробы, чтобы сохранить незнакомые IP для измерения detector.
 * Возвращает задержку в timebase (1 единица = 100 мкс), 0 — откладывать не нужно. */
uint64_t nat_detection_probe_delay(struct NAT_DETECTION *detector);

/* Отсрочка первого INIT на дополнительный публичный IP пира в единицах timebase.
 * Возвращает 0, когда линк можно пробовать сразу; текущий транспорт не меняется. */
uint64_t nat_detection_udp_init_delay(struct NAT_DETECTION *detector, const struct ETCP_LINK *link);

/* Возвращает 0, если ответ на INIT проверенного пира открыл бы фильтр на зарезервированном IP.
 * source — фактический адрес INIT, connection — уже существующее соединение с этим пиром. */
int nat_detection_udp_init_allowed(struct NAT_DETECTION *detector, const struct ETCP_CONN *connection,
                                   struct ETCP_SOCKET *socket, const struct sockaddr_storage *source);

/* Можно ли сейчас обрабатывать PING на socket с ответом, не испортив проверку незнакомого входа.
 * peer_key — проверенный ключ отправителя (32 байта), source — фактический IP:порт, flags — флаги PING,
 * tag/length — его пользовательские данные/размер. Возвращает 1 — разрешено, 0 — пропустить без ответа. */
int nat_detection_ping_allowed(struct NAT_DETECTION *detector, struct ETCP_SOCKET *socket,
                               const uint8_t peer_key[32], const struct sockaddr_storage *source,
                               uint8_t flags, const uint8_t *tag, size_t length);

/* Сообщить о готовности link после UDP INIT или STCP handshake. Принятый вход с двумя публичными IP
 * подтверждает direct/open у принимающего сокета; иначе при отсутствии результата планируется измерение.
 * Для TCP фактический локальный IP должен совпадать с IP интерфейса этого сокета. */
void nat_detection_link_ready(struct NAT_DETECTION *detector, struct ETCP_LINK *link);

/* Сообщить об утрате/перезапуске connection; запланировать пересмотр маршрутов и отмену зависимых проверок при их потере. */
void nat_detection_cancel_for_conn(struct NAT_DETECTION *detector, struct ETCP_CONN *connection);

/* Сообщить о неудаче прямого подключения connection к узлу с известной входящей доступностью.
 * reverse_done != 0 — обратное подключение тоже исчерпано; только тогда перепроверяется filtered (CONTACT). */
void nat_detection_connection_failed(struct NAT_DETECTION *detector, struct ETCP_CONN *connection, int reverse_done);

/* Удалить состояние socket и отменить связанные задачи; вызвать до освобождения самого сокета.
 * Для внутреннего probe_only также снимает ссылку владельца: последующий destroy не удалит сокет повторно. */
void nat_detection_socket_removed(struct NAT_DETECTION *detector, struct ETCP_SOCKET *socket);

/* Учесть исходящую отправку с socket на destination (IP:порт) до передачи пакета транспорту.
 * История хранится по IP; контакт с помощником во время проверки лишает вход статуса незнакомого. */
void nat_detection_note_send(struct NAT_DETECTION *detector, struct ETCP_SOCKET *socket,
                              const struct sockaddr_storage *destination);

/* Учесть observed — наш IP:порт, сообщённый пиром при установлении link.
 * Непубличное наблюдение не меняет интернет-профиль; изменение внешнего адреса сбрасывает прежнее измерение. */
void nat_detection_observed_by_peer(struct NAT_DETECTION *detector, struct ETCP_LINK *link,
                                    const struct sockaddr_storage *observed);

/* Учесть IP, сообщённый по защищённому TCP link: local — фактический локальный адрес соединения,
 * observed — адрес, увиденный пиром. Публичные IP сравниваются без портов; сохраняемый внешний порт всегда 0.
 * Непубличный источник/наблюдение и другая семья адресов не меняют результат; вызов — в потоке ядра. */
void nat_detection_tcp_observed(struct NAT_DETECTION *detector, struct ETCP_LINK *link,
                                 const struct sockaddr_storage *local, const struct sockaddr_storage *observed);

/* Обработать запрос наблюдения, полученный на socket: source — реальный IP:порт отправителя,
 * peer_key — его проверенный ключ (32 байта), tag/length — тег попытки/его размер в байтах.
 * Непубличный source отклоняется; результат возвращается через защищённый управляющий канал. */
void nat_detection_record_observation(struct NAT_DETECTION *detector, struct ETCP_SOCKET *socket,
                                      const uint8_t peer_key[32], const struct sockaddr_storage *source,
                                      const uint8_t *tag, size_t length);

/* Обработать входящую пробу помощника на проверяемом socket, не отвечая ему по UDP.
 * peer_key — проверенный ключ (32 байта), source — фактический IP:порт, tag/length — тег попытки/размер в байтах.
 * Проверяет подлинность, текущую попытку, публичность и незнакомость IP; неподходящего помощника заменяет. */
void nat_detection_receive_ingress(struct NAT_DETECTION *detector, struct ETCP_SOCKET *socket,
                                   const uint8_t peer_key[32], const struct sockaddr_storage *source,
                                   const uint8_t *tag, size_t length);

/* Запланировать новые проверки всех рабочих сокетов detector, отменив текущие; действует и в ручном режиме. */
void nat_detection_request_check_all(struct NAT_DETECTION *detector);

/* Запланировать проверку socket через конкретный observer (открытый Ed25519), заменив текущую попытку.
 * Возвращает 0 — запрос принят, -1 — сокет/детектор недоступен или observer отсутствует или некорректен; результат придёт позже. */
int nat_detection_request_check_socket(struct NAT_DETECTION *detector, struct ETCP_SOCKET *socket, const uint8_t observer[32]);

/* Только для тестов: allow != 0 допускает непубличные IPv4 и IPv6 loopback; по умолчанию такие источники запрещены. */
void nat_detection_set_allow_local(struct NAT_DETECTION *detector, int allow);

/* automatic != 0 включает фоновые проверки (по умолчанию); 0 оставляет ручной запуск request_check_*().
 * Уже начатые проверки и помощь другим узлам продолжаются в обоих режимах. */
void nat_detection_set_automatic(struct NAT_DETECTION *detector, int automatic);

/* Записать в status снимок состояния socket; не создаёт состояние и не начинает проверку.
 * Возвращает 0 — снимок заполнен, -1 — состояния нет или аргументы непригодны. */
int nat_detection_get_status(struct NAT_DETECTION *detector, struct ETCP_SOCKET *socket, struct nat_check_status *status);

/* Вернуть постоянную строку для reason (enum nat_check_reason); неизвестному значению соответствует "invalid_reason". */
const char *nat_check_reason_name(uint8_t reason);

#ifdef __cplusplus
}
#endif

#endif
