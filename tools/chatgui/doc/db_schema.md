# База данных децентрализованного чата (chatgui)

Чат децентрализованный — нет центрального сервера. Каждый узел хранит локальную SQLite БД
(`chats.db`, WAL mode). Синхронизация сообщений между узлами — через протокол `chat_sync`
поверх `etcp_router` (svc_id 0x30). Данные о других узлах поступают из NODEINFO
через conn_mgr.

Запись и изменение схемы выполняются только в потоке ядра; GUI читает через `DbManager`.
Актуальные определения всех таблиц находятся в `src/db_schema.c`; SQL-примеры ниже — историческая справка.
Перед запуском служб ядро проверяет таблицы, ограничения, индексы, представления и триггеры.
Пустая база создаётся в актуальном формате; несовместимая останавливает запуск без изменения данных.
Desktop/Android показывают причину и путь в интерфейсе, консольный utun — в stderr.
В диалоге GUI можно выбрать «Создать базу заново» либо «Выйти». Первый вариант останавливает сервис,
удаляет chats.db со всеми журналами и запускает сервис с пустой базой. Второй сохраняет старую базу.
При несовместимой схеме пользователь может выйти или явно создать пустую базу текущего формата.

## Обзор таблиц

| Таблица | Назначение |
|---------|-----------|
| `local_identity` | Наш собственный узел (id, ключи) |
| `nodes` | Все известные узлы (node_id, ключи, last_seen) |
| `node_addresses` | Адреса узлов (family, protocol, address, port, rtt, is_nat) |
| `accounts` | Профили узлов (display_name, avatar) |
| `channels` | Метаданные каналов/сетей (ключи, подпись, позиции скролла) |
| `msg_<ch>` | Per-channel таблица сообщений |
| `peers_<ch>` | Per-channel таблица участников (подписи входа, создателя, comment) |
| `ui_state` | Локальное состояние UI (key-value) |

---

## Таблицы

### `local_identity` — наш собственный узел

```sql
CREATE TABLE IF NOT EXISTS local_identity (
    id              INTEGER PRIMARY KEY CHECK (id = 1),
    name            TEXT NOT NULL,
    x25519_pubkey   BLOB NOT NULL,
    ed25519_pubkey  BLOB NOT NULL CHECK(length(ed25519_pubkey)=32),
    created_at      INTEGER DEFAULT (unixepoch()),
    updated_at      INTEGER DEFAULT (unixepoch())
);
```

### `nodes` — все известные узлы

```sql
CREATE TABLE IF NOT EXISTS nodes (
    node_id          INTEGER PRIMARY KEY,
    name             TEXT,
    x25519_pubkey    BLOB NOT NULL,
    ed25519_pubkey   BLOB,
    last_seen_at     INTEGER,
    rtt              INTEGER,
    created_at       INTEGER DEFAULT (unixepoch())
);
```

| Поле | Описание |
|------|---------|
| `node_id` | ID узла в сети uTun |
| `name` | Никнейм |
| `x25519_pubkey` | Публичный ключ для key exchange (32 байта) |
| `ed25519_pubkey` | Публичный ключ для подписей (32 байта), может быть NULL |
| `last_seen_at` | Unix-время последней активности |
| `rtt` | Среднее минимумов рабочих линков узла за интервал записи; 0.1 мс, NULL — нет замеров |

Статус online определяется через conn_mgr, а не полем в БД.

### `node_addresses` — адреса узлов

```sql
CREATE TABLE IF NOT EXISTS node_addresses (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    node_id     INTEGER NOT NULL REFERENCES nodes(node_id) ON DELETE CASCADE,
    family      INTEGER NOT NULL CHECK(family IN (4, 6)),
    protocol    INTEGER NOT NULL DEFAULT 1,
    address     BLOB NOT NULL,
    port        INTEGER NOT NULL CHECK(port > 0 AND port <= 65535),
    rtt         INTEGER,
    is_nat      INTEGER DEFAULT 0,
    created_at  INTEGER DEFAULT (unixepoch())
);
CREATE INDEX IF NOT EXISTS idx_na_node ON node_addresses(node_id);
```

| Поле | Описание |
|------|---------|
| `family` | 4=IPv4, 6=IPv6 |
| `protocol` | Битовая маска: 1=UDP (NODE_PROTO_UDP), 2=TCP (NODE_PROTO_TCP), 3=UDP+TCP |
| `address` | BLOB: 4 байта для IPv4, 16 байт для IPv6 |
| `port` | Порт |
| `rtt` | Среднее минимумов линков к этому endpoint за интервал записи; 0.1 мс, NULL — нет замеров |
| `is_nat` | 1 = узел за NAT |

### `accounts` — профили узлов

```sql
CREATE TABLE IF NOT EXISTS accounts (
    node_id         INTEGER PRIMARY KEY REFERENCES nodes(node_id),
    display_name    TEXT NOT NULL,
    avatar_color    TEXT DEFAULT '#4A90E2',
    avatar_letter   TEXT NOT NULL,
    is_contact      INTEGER DEFAULT 1,
    created_at      INTEGER DEFAULT (unixepoch())
);
```

### `channels` — метаданные каналов

```sql
CREATE TABLE IF NOT EXISTS channels (
    channel_id       TEXT PRIMARY KEY,
    name             TEXT NOT NULL,
    owner_node_id    BLOB,
    x25519_pubkey    BLOB NOT NULL,
    x25519_privkey   BLOB,
    ed25519_pubkey   BLOB NOT NULL,
    ed25519_privkey  BLOB,
    signature        BLOB NOT NULL,
    last_read_msg_id INTEGER,
    last_pos_msg_id  INTEGER,
    created_at       INTEGER DEFAULT (unixepoch())
);
```

| Поле | Описание |
|------|---------|
| `channel_id` | Уникальный ID канала (напр. `"ch:general"`) |
| `name` | Отображаемое имя (напр. `"#general"`) |
| `owner_node_id` | Публичный Ed25519 создателя канала (32 байта) |
| `x25519_pubkey` | Публичный ключ канала (32 байта) |
| `x25519_privkey` | Приватный ключ канала, NULL если локальный узел не админ |
| `ed25519_pubkey` | Публичный ключ подписи канала (32 байта) |
| `ed25519_privkey` | Приватный ключ подписи, NULL если локальный узел не админ |
| `signature` | Ed25519 подпись всех полей записи приватным ключом канала |
| `last_read_msg_id` | ID последнего прочитанного сообщения (локальное) |
| `last_pos_msg_id` | ID сообщения на позиции скролла (для восстановления при открытии) |

### `msg_<ch>` — per-channel таблица сообщений

Подписанный JSON `data` содержит `ct` и `d`; ответ дополнительно содержит `reply_to`
(64 строчных hex-символа глобального ID). Необязательное поле `reply_quote` хранит полный
UTF-8 снимок выделенного видимого текста: без повторной обработки разметки и без изменения
при правке оригинала. Отсутствие поля означает обычный ответ. Общий лимит JSON — 4000 байт.
Цитата также сохраняется в JSON задания вложения, чтобы восстановление READY-публикации её не теряло.

Для каждого канала создаётся отдельная таблица. Имя: `msg_` + sanitized `channel_id`
(не-буквоцифры заменяются на `_`).

```sql
CREATE TABLE IF NOT EXISTS "msg_<ch>" (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    node_id      INTEGER NOT NULL,
    content_type TEXT NOT NULL,
    data         BLOB NOT NULL,
    timestamp    INTEGER NOT NULL,
    datahash     INTEGER NOT NULL,
    chain_hash   BLOB NOT NULL,
    signature    BLOB,
    is_outgoing  INTEGER DEFAULT 0,
    is_read      INTEGER DEFAULT 0,
    sync_flags   INTEGER DEFAULT 0,
    UNIQUE(timestamp, datahash)
);
CREATE INDEX IF NOT EXISTS "idx_msg_<ch>_ts" ON "msg_<ch>"(timestamp, datahash);
```

| Поле | Описание |
|------|---------|
| `node_id` | Автор сообщения |
| `content_type` | MIME-тип: `"text/plain"`, `"image/png"`, ... |
| `data` | Содержимое сообщения (BLOB) |
| `timestamp` | Время отправки в микросекундах (первая часть ключа sync) |
| `datahash` | Первые 8 байт SHA256(data), вторая часть ключа sync |
| `chain_hash` | SHA256 цепочки (32 байта) — как в blockchain, связывает записи по порядку |
| `signature` | Ed25519 подпись автора (64 байта), пока заглушка (64 нуля) |
| `is_outgoing` | 1 = отправлено нами |
| `is_read` | 1 = прочитано |
| `sync_flags` | Битовая маска: 0x01 = WAS_SENT (запись подтверждена пирами) |

**Дедупликация:** уникальный индекс `(timestamp, datahash)` предотвращает повторную вставку
одного и того же сообщения при получении через разные пути.

**Chain hash:** `chain_hash = SHA256(prev_chain_hash || timestamp || datahash)`.
Обеспечивает проверку целостности порядка сообщений при синхронизации.

### `peers_<ch>` — per-channel таблица участников

```sql
-- Полная актуальная схема: src/db_schema.c, шаблон peers.
-- Подписи вступления/профиля/приглашения и adm_tags хранятся в peers_<ch>.
-- Узловой RTT читается из nodes.rtt, адресный — из node_addresses.rtt.
```

| Поле | Описание |
|------|---------|
| `join_sig` | Ed25519 подпись присоединяющегося: `sign(sk_user, channel_id || node_id)` |
| `signed_by`, `signature` | Пригласивший узел и подпись приглашения |

### `ui_state` — локальное состояние UI

```sql
CREATE TABLE IF NOT EXISTS ui_state (
    key   TEXT PRIMARY KEY,
    value TEXT
);
```

Key-value хранилище для произвольных настроек UI (позиции окон, выбранный канал, ...).

---

## Формат binary-записей sync (gui_bridge / chat_sync)

Записи передаются между потоками и по сети в компактном бинарном формате:

### Insert record (для GUI_OP_INSERT_RECORD / PUSH wire)
```
[timestamp:8 LE][datahash:8 LE][node_id:8 LE][ct_len:1][content_type:ct_len][data_len:4 LE][data:data_len][chain_hash:32]
```

### NodeInfo (GUI_OP_LOAD_NODEINFO)
```
[x25519_pubkey:32][ed25519_pubkey:32][addr_count:1]([family:1][proto:1][addr_len:1][addr:4|16][port:2 LE][rtt:2 LE])...
```

### SEND_DATA cursor record (GUI_OP_CURSOR_NEXT)
```
[ts:8 LE][dh:8 LE][node_id:8 LE][ct_len:1][ct:var][data_len:4 LE][data:var]
```

---

## Порядок синхронизации (chat_sync)

1. **Peer online** → для каждого общего канала: `INIT_SYNC(my_count, last_chain_hash)`
2. **INIT_RESP** → сравнение chain_hash на позиции `min(counts)-1`. Если match → synced.
   Иначе sparse hashes (-1, -2, -4, -8, ...) для поиска точки расхождения.
3. **SEND_DATA** → обмен недостающими записями пакетами до 32 штук
4. **SYNC_DONE** → финальная сверка count + last_chain_hash
5. **PUSH** → реал-тайм доставка новых сообщений (→ ACK_PUSH)
6. **TTL** → раз в час удаление своих неподтверждённых записей старше 24ч

Все операции с БД асинхронные: `chat_sync (uasync)` → `gui_bridge_call()` →
GUI-поток (SQL) → `uasync_post()` → callback в chat_sync.
