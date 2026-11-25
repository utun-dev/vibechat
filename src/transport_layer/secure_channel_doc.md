# secure_channel

## 1. Назначение

Модуль реализует криптографический слой uTun: защищённый канал между двумя узлами на основе **X25519** (Elliptic Curve Diffie-Hellman) для обмена сессионным ключом и **AES-128-CCM** (Counter with CBC-MAC) для аутентифицированного шифрования трафика. Дополнительно предоставляет Ed25519 для цифровых подписей (аутентификация данных, происходящих от конкретного узла) и стриминговое шифрование (AES-128-CTR) для больших потоков данных, где нужна только конфиденциальность без аутентификации.

UDP ETCP использует пакетное AES-128-CCM; STCP использует отдельные AES-128-CTR API
этого модуля и CRC кадров. Сетевые handshake и подтверждение эпох выполняются выше, в transport_layer.

**Ключевые криптографические операции:**
1. **X25519 ECDH** — получение общего секрета из своего приватного и чужого публичного ключа
2. **AES-128-CCM** — аутентифицированное шифрование (confidentiality + integrity): nonce(13) + ciphertext + tag(16)
3. **SHA256-based обфускация pubkey** — скрытие публичного ключа при передаче в INIT-пакетах (XOR с SHA256(salt || peer_pubkey))
4. **Ed25519** — цифровые подписи данных (one-shot и стриминговая через SHA-512 аккумулятор)
5. **AES-128-CTR** — стриминговое шифрование без аутентификации (bulk data)
6. **CRC32** — дополнительный контроль целостности расшифрованных данных

---

## 2. Как пользоваться

### 2.1. Типовой сценарий: генерация → обмен → шифрование

```c
// ── Шаг 1: Загрузка или генерация ключей ──
struct SC_MYKEYS my_keys;
uint8_t ed[32], seed[32];

// Новое устройство
if (sc_identity_generate(my_keys.public_key, my_keys.private_key, ed, seed) != SC_OK) return -1;

// Для уже настроенного устройства вместо генерации загрузить my_public_key и my_private_key
// if (sc_identity_load(pubkey_hex, seed_hex, &my_keys, ed, seed) != SC_OK) return -1;

// ── Шаг 2: Инициализация контекста ──
sc_context_t ctx;
sc_init_ctx(&ctx, &my_keys);
// Теперь ctx.session_ready == 0 — ждём установки ключа пира

// ── Шаг 3: Установка публичного ключа пира (после получения из сети) ──
uint8_t peer_pubkey[SC_PUBKEY_SIZE];
// ... получить peer_pubkey из INIT-пакета / конфига ...
sc_set_peer_public_key(&ctx, peer_pubkey, SC_PEER_PUBKEY_BIN);  // или SC_PEER_PUBKEY_HEX
// Внутри: X25519 ECDH → shared_secret → SHA256(shared_secret || "uTun-v3-session") → session_key
// Теперь ctx.session_ready == 1

// ── Шаг 4: Шифрование / дешифрование ──
if (!plaintext_len) return SC_ERR_INVALID_ARG;
uint8_t ciphertext[plaintext_len + SC_NONCE_SIZE + SC_CRC32_SIZE + SC_TAG_SIZE];
size_t ct_len = 0;

// Шифрование: nonce(13) || AES-CCM(plaintext+crc32) || tag(16)
int status = sc_encrypt(&ctx, plaintext, plaintext_len, ciphertext, &ct_len);
if (status != SC_OK) return status;

// Дешифрование: проверка тега (аутентификация) → проверка CRC32 → возврат plaintext
uint8_t decrypted[plaintext_len];
size_t pt_len = 0;
status = sc_decrypt(&ctx, ciphertext, ct_len, decrypted, &pt_len);
if (status != SC_OK) return status;
```

ct_len/pt_len — выходные длины, не capacity буферов. Для encrypt требуется
plaintext_len + 33 байта, для decrypt — ciphertext_len - 33. ctx заимствует my_keys;
на каждый передаваемый CCM-пакет выбирается новый 13-байтный nonce через OpenSSL RAND_bytes.

### 2.2. Обфускация pubkey (INIT-пакеты)

При отправке INIT-пакета публичный ключ не передаётся в открытом виде. Вместо этого он обфусцируется через XOR с SHA256-хэшем от соли и публичного ключа пира:

```c
uint8_t salt[SC_PUBKEY_ENC_SALT_SIZE];   // 8 случайных байт
random_bytes(salt, sizeof(salt));

uint8_t obfuscated[SC_PUBKEY_SIZE];       // 32 байта
sc_obfuscate_pubkey(salt, peer_pubkey, my_pubkey, obfuscated);

// Передаётся в пакете: salt(8) || obfuscated_pubkey(32) = 40 байт (SC_PUBKEY_ENC_SIZE)
```

**Формат пакета с обфусцированным pubkey:**
- Заголовок и данные шифруются AES-CCM
- Блок `salt(8) + obfuscated_pubkey(32)` добавляется **после зашифрованных данных, без шифрования**

**Деобфускация на приёмной стороне:**
```c
// Получатель знает salt из пакета и свой публичный ключ
sc_obfuscate_pubkey(salt, my_pubkey, received_obfuscated, decrypted_pubkey);
// XOR обратим: obfuscated XOR SHA256(salt||my_pubkey) = исходный pubkey отправителя
```

В реализации `sc_obfuscate_pubkey()` вычисляются два SHA256-хэша, но 32-байтная
маска использует только первый, `SHA256(salt||peer_pubkey)`. Второй хэш на результат не влияет.

### 2.3. Стриминговое шифрование (AES-128-CTR)

Для больших объёмов данных, где не нужна посылочная аутентификация:

```c
struct sc_stream_state stream = {0};
uint8_t salt[SC_PUBKEY_ENC_SALT_SIZE];
if (random_bytes(salt, sizeof(salt)) != 0) return -1;
if (sc_stream_init(ctx.session_key, &stream, stream_id, salt) != SC_OK) return -1;

sc_stream_xor(&stream, data_chunk1, len1);  // XOR in-place (шифрует/дешифрует)
sc_stream_xor(&stream, data_chunk2, len2);

sc_stream_cleanup(&stream);
```

Nonce — первые 12 байт `SHA256(session_key || stream_id_LE || salt(8) || "uTun3-stream")`; последние 4 байта IV нулевые.
Получатель использует соль отправителя. Ключ должен быть заранее выведен через ECDH; state нулевой или после cleanup.
Повторная инициализация активного state отклоняется. STCP передаёт соль в существующем открытом блоке handshake.

### 2.4. Ed25519 подписи

**One-shot:**
```c
uint8_t sig[SC_SIGN_SIZE];  // 64 байта
sc_ed25519_sign(privkey, msg, msg_len, sig);
int ok = sc_ed25519_verify(pubkey, msg, msg_len, sig);  // SC_OK при успехе
```

**Стриминговая (потоковая) подпись:** SHA-512 аккумулятор + one-shot Ed25519 в финале. Подходит для данных любого размера (память O(1)):
```c
struct sc_stream_sign_state state = {0};
uint8_t stream_sig[SC_SIGN_SIZE];
size_t sig_len = sizeof(stream_sig);
int result;

// Подпись
if ((result = sc_stream_sign_init(ed_seed, &state)) != SC_OK) goto stream_done;
if ((result = sc_stream_sign_update(&state, chunk1, len1)) != SC_OK) goto stream_done;
if ((result = sc_stream_sign_update(&state, chunk2, len2)) != SC_OK) goto stream_done;
if ((result = sc_stream_sign_final(&state, stream_sig, &sig_len)) != SC_OK) goto stream_done;

// Верификация
if ((result = sc_stream_sign_verify_init(&state, ed25519_pubkey)) != SC_OK) goto stream_done;
if ((result = sc_stream_sign_update(&state, chunk1, len1)) != SC_OK) goto stream_done;
if ((result = sc_stream_sign_update(&state, chunk2, len2)) != SC_OK) goto stream_done;
result = sc_stream_sign_verify(&state, stream_sig, sig_len);

stream_done:
sc_stream_sign_cleanup(&state);  // Безопасно после ошибок, final/verify и предыдущей очистки.
// result: SC_OK при успехе, отрицательный SC_ERR_* при ошибке.
```

Перед первым использованием состояние обнуляют. `init` требует пустого состояния и создаёт ресурсы
до его заполнения: отказ оставляет все поля пустыми. При корректных входных аргументах повторный `init`
активного состояния возвращает `SC_ERR_INVALID_ARG`, сохраняя ресурсы и накопленный хеш;
для новой операции сначала нужен `cleanup`.
Для подписи передают seed Ed25519 устройства.

`final` проверяет capacity не менее 64 до финализации. NULL аргументы, недостаточная capacity и неверный
режим не меняют состояние или выходной буфер. После начала финализации `final/verify` освобождают
состояние при любом результате, в том числе неверной подписи. При ошибке EVP update нужна явная очистка.
`cleanup` обнуляет все поля и допускает повторные вызовы и NULL.

Устройство хранит 32-байтный seed Ed25519. `sc_identity_generate` выбирает seed с нулевым битом знака публичного Ed25519. `sc_identity_from_seed` восстанавливает обе пары ключей. `sc_identity_check` проверяет по публичным данным каноничность точки Ed25519 и соответствие преобразованному X25519. Оба API подписи принимают seed Ed25519.

### 2.5. Деривация node_id

```c
uint64_t node_id = sc_derive_node_id_from_pubkey(public_key);
// SHA256(X25519 public_key[32]) → первые 8 байт memcpy в uint64_t хоста, старший бит обнулён
```

### 2.6. SHA256-based транскодирование

```c
sc_sha_transcode(key, key_len, data, data_len);
// data[i] ^= SHA256(key)[i % 32] — простой XOR с SHA256-хэшем ключа
```

---

## 3. API

### 3.1. Константы

| Константа | Значение | Описание |
|---|---|---|
| `SC_PRIVKEY_SIZE` | 32 | Размер приватного ключа X25519 (байт) |
| `SC_PUBKEY_SIZE` | 32 | Размер публичного ключа X25519 (байт) |
| `SC_HASH_SIZE` | 32 | Размер SHA256-хэша (байт) |
| `SC_NONCE_SIZE` | **13** | Фиксированный размер nonce пакетного протокола этого модуля |
| `SC_SHARED_SECRET_SIZE` | 32 | Размер общего секрета X25519 (= SC_HASH_SIZE) |
| `SC_SESSION_KEY_SIZE` | 16 | Размер сессионного ключа AES-128 (байт) |
| `SC_TAG_SIZE` | 16 | Размер аутентификационного тега CCM (байт) |
| `SC_CRC32_SIZE` | 4 | Размер CRC32 в зашифрованном пакете (байт) |
| `SC_PUBKEY_ENC_SALT_SIZE` | 8 | Размер соли для обфускации pubkey (байт) |
| `SC_PUBKEY_ENC_SIZE` | 40 | Полный размер обфусцированного блока: salt(8) + obfuscated(32) |
| `SC_STREAM_NONCE_SIZE` | 12 | Размер nonce для AES-128-CTR (байт) |
| `SC_SIGN_SIZE` | 64 | Размер Ed25519 подписи (байт) |
| `SC_OK` | 0 | Успех |
| `SC_ERR_INVALID_ARG` | -1 | Неверные аргументы |
| `SC_ERR_CRYPTO` | -2 | Криптографическая ошибка (OpenSSL) |
| `SC_ERR_NOT_INITIALIZED` | -3 | Контекст/сессия не инициализирована |
| `SC_ERR_AUTH_FAILED` | -4 | Ошибка аутентификации (неверный тег/подпись) |
| `SC_ERR_CRC_FAILED` | -5 | Ошибка CRC32 (повреждение данных) |
| `SC_PEER_PUBKEY_BIN` | 0 | Флаг: ключ в бинарном формате |
| `SC_PEER_PUBKEY_HEX` | 1 | Флаг: ключ в hex-строке |

### 3.2. Структуры

**`struct SC_MYKEYS`** — локальная пара ключей узла:
- `uint8_t private_key[32]` — приватный ключ X25519
- `uint8_t public_key[32]` — публичный ключ X25519

**`struct secure_channel` (sc_context_t)** — контекст защищённого канала между двумя конкретными узлами:
- `pk` — заимствованный указатель на `SC_MYKEYS` до конца использования контекста
- `peer_public_key[32]` — публичный ключ пира (заполняется после ECDH)
- `session_key[16]` — производный сессионный ключ AES-128
- `initialized` — флаг: `sc_init_ctx()` вызван
- `peer_key_set` — флаг: `sc_set_peer_public_key()` вызван успешно
- `session_ready` — флаг: можно шифровать/дешифровать
- `tx_counter` — счётчик успешных encrypt; генерация nonce от него не зависит
- `rx_counter` — счётчик успешных decrypt после проверки CCM tag и CRC

**`struct sc_stream_state`** — состояние стримингового шифра (AES-128-CTR):
- `ectx` — `EVP_CIPHER_CTX*` (opaque)
- `initialized` — флаг инициализации

**`struct sc_stream_sign_state`** — состояние стриминговой подписи (Ed25519 + SHA-512):
- `md_ctx` — `EVP_MD_CTX*` (SHA-512 аккумулятор, opaque)
- `pkey` — `EVP_PKEY*` (Ed25519 ключ, opaque)
- `initialized` — 1 после успешного init; 0 в пустом состоянии
- `is_sign` — в активном состоянии 1 = подпись, 0 = верификация; после cleanup равен 0

### 3.3. Функции

#### Инициализация ключей и контекста

| Функция | Описание |
|---|---|
| `sc_init_ctx(ctx, mykeys)` | Заимствует локальные ключи, инициализирует флаги и счётчики; ECDH выполняется при установке ключа пира. |
| `sc_generate_keypair(keys)` | Генерирует новую пару X25519 через `EVP_PKEY_keygen`. Случайный приватный ключ. |
| `sc_init_local_keys(mykeys, pub_hex, priv_hex)` | Загружает сырую пару X25519 из HEX и проверяет публичный ключ на ненулевое значение. Для полей INI устройства используется `sc_identity_load`. |
| `sc_set_peer_public_key(ctx, peer_pubkey, mode)` | **Ключевая операция:** выполняет X25519 ECDH, выводит session_key, устанавливает `session_ready=1`. mode: `SC_PEER_PUBKEY_BIN` или `SC_PEER_PUBKEY_HEX`. |
| `sc_compute_public_key_from_private(privkey, pubkey_out)` | Вычисляет публичный ключ из приватного (X25519). |

**Вывод session_key:** `SHA256(X25519_shared_secret || "uTun-v3-session") → truncate to 16 bytes`

#### Шифрование / дешифрование

| Функция | Описание |
|---|---|
| `sc_encrypt(ctx, plaintext, pt_len, ciphertext, &ct_len)` | AES-128-CCM шифрование. Формат вывода: `nonce(13) \|\| AES-CCM(plaintext + CRC32(plaintext)) \|\| tag(16)`. Инкрементирует `tx_counter`. |
| `sc_decrypt(ctx, ciphertext, ct_len, plaintext, &pt_len)` | AES-128-CCM дешифрование. Проверяет CCM-тег → проверяет CRC32. Инкрементирует `rx_counter` при успехе. |

**Nonce:** 13 случайных байт из OpenSSL `RAND_bytes` на каждый encrypt.
При отказе RNG возвращается `SC_ERR_CRYPTO`, выходная длина остаётся нулевой.
Nonce не зависит от часов, счётчиков контекста или параллельных экземпляров ядра.

**Проверки при дешифровании:**
1. `ciphertext_len >= SC_NONCE_SIZE + SC_TAG_SIZE + SC_CRC32_SIZE` (минимум 13+16+4 = 33 байта)
2. CCM-тег: несовпадение → `SC_ERR_AUTH_FAILED` (пакет подделан или повреждён)
3. CRC32: несовпадение → `SC_ERR_CRC_FAILED` (повреждение данных после расшифровки)

#### Обфускация pubkey

| Функция | Описание |
|---|---|
| `sc_obfuscate_pubkey(salt, peer_pubkey, pubkey, output)` | `output[i] = pubkey[i] ^ SHA256(salt \|\| peer_pubkey)[i]` для i < 32. XOR обратим — повторный вызов с теми же salt/peer_pubkey восстанавливает исходный pubkey. |

#### Стриминговое шифрование (AES-128-CTR)

| Функция | Описание |
|---|---|
| `sc_stream_init(session_key, state, stream_id, salt)` | Инициализирует CTR-контекст с выведенным ключом и солью отправителя. Nonce из `SHA256(session_key \|\| stream_id_LE \|\| salt \|\| "uTun3-stream")`. |
| `sc_stream_xor(state, data, len)` | XOR данных с keystream in-place. Нулевая длина — no-op. |
| `sc_stream_cleanup(state)` | Освобождает `EVP_CIPHER_CTX`. |

#### Ed25519 подписи (one-shot)

| Функция | Описание |
|---|---|
| `sc_ed25519_sign(privkey[32], msg, msg_len, sig_out[64])` | Подпись сообщения. `privkey` — 32-байтный Ed25519-приватный ключ (НЕ X25519). |
| `sc_ed25519_verify(pubkey[32], msg, msg_len, sig[64])` | `SC_OK` — подпись верна; `SC_ERR_AUTH_FAILED` — не совпала; `SC_ERR_CRYPTO` — отказ allocation/init/операции; `SC_ERR_INVALID_ARG` — NULL вход. |

Ошибка криптографической операции не является доказательством повреждения подписанных данных.
Проверка Ed25519 отдельно обрабатывает создание ресурсов, VerifyInit и результат Verify.
Деривация node_id возвращает 0 с ERROR при отказе SHA256 и не читает неопределённый hash.

#### Ed25519 подписи (стриминг)

| Функция | Описание |
|---|---|
| `sc_stream_sign_init(seed, state)` | Инициализирует подпись в пустом state переданным 32-байтным seed Ed25519. |
| `sc_stream_sign_verify_init(state, ed25519_pubkey)` | Инициализирует верификацию с переданным Ed25519 pubkey[32] в пустом state; ошибка сохраняет state. |
| `sc_stream_sign_update(state, data, len)` | SHA-512 update (инкрементальный). Нулевая длина — no-op. |
| `sc_stream_sign_final(state, sig_out, &sig_len)` | SHA-512 → one-shot Ed25519 подпись хэша. На входе capacity sig_out ≥64, при успехе на выходе 64. После начала финализации выполняет cleanup при любом результате. |
| `sc_stream_sign_verify(state, sig, sig_len)` | SHA-512 → one-shot Ed25519 проверка. После начала финализации выполняет cleanup при любом результате. Возвращает `SC_OK` при успехе, отрицательный `SC_ERR_*` при ошибке. |
| `sc_stream_sign_cleanup(state)` | Освобождает ресурсы, обнуляет все поля; безопасен для NULL, пустого state и повторных вызовов. |

**Потоковая схема:** Ed25519 внутри делает SHA-512 от данных, поэтому для стриминга нужно было бы грузить все данные в память. Вместо этого: `SHA-512(данные)` делается инкрементально через `EVP_DigestUpdate`, затем 64-байтный хэш подписывается one-shot `EVP_DigestSign`. Результат: `Ed25519(SHA-512(данные))` — консистентно с обеих сторон. Память O(1).

#### Деривация

| Функция | Описание |
|---|---|
| `sc_identity_from_seed(seed, x, x_private, ed)` | Восстанавливает ключи устройства из канонического seed Ed25519. |
| `sc_derive_node_id_from_pubkey(public_key)` | SHA256 бинарного X25519 pubkey[32] → первые 8 байт memcpy в uint64_t хоста, старший бит обнулён. При NULL возвращает 0 и логирует ошибку. |

#### Утилиты

| Функция | Описание |
|---|---|
| `sc_sha_transcode(key, key_len, data, data_len)` | `data[i] ^= SHA256(key)[i % 32]`. Простой XOR с хэшем ключа. |

### 3.4. Зависимости

| Зависимость | Использование |
|---|---|
| **OpenSSL EVP** (`openssl/evp.h`) | X25519 keygen/derive (`EVP_PKEY`), AES-128-CCM, AES-128-CTR, Ed25519 sign/verify, SHA-512 |
| **OpenSSL SHA** (`openssl/sha.h`) | SHA256 для session_key, CTR IV, обфускации, transcode и node_id; SHA512 для Ed25519 seed |
| **OpenSSL RAND** (`openssl/rand.h`) | `RAND_bytes` для случайного CCM nonce каждого пакета |
| **`../lib/debug_config.h`** | `DEBUG_ERROR`, `DEBUG_WARN`, `DEBUG_INFO`, `DEBUG_DEBUG` (категория `DEBUG_CATEGORY_CRYPTO`) |
| **`../lib/platform_compat.h`** | Общие платформенные определения; CCM nonce генерируется OpenSSL RAND_bytes |
| **`crc32.h`** (`crc32_calc`) | Контроль целостности в шифрованных пакетах |

### 3.5. Границы транспортной сессии

`sc_set_peer_public_key` выводит ключ через X25519 и может заменить ключ пира в существующем контексте.
Счётчики encrypt/decrypt служат статистикой, а не идентификатором сессии или защитой от повторов.
Согласование эпох и допуск DATA выполняет `etcp_session`; дедупликацию DATA обеспечивает ETCP.

---

## 4. Формат зашифрованного пакета

```
┌──────────────┬─────────────────────────────────────┬──────────────┐
│ nonce (13)   │ AES-CCM(plaintext || CRC32(pt))     │ tag (16)     │
│ 13 bytes     │ pt_len + 4 bytes                    │ 16 bytes     │
└──────────────┴─────────────────────────────────────┴──────────────┘
```

- **nonce:** 13 случайных байт из OpenSSL RAND_bytes для каждого пакета
- **ciphertext:** AES-128-CCM in-place шифрование `plaintext || CRC32(plaintext)`
- **tag:** 16 байт, аутентификационный тег CCM (обеспечивает integrity + authenticity)

**Порядок проверки при дешифровании:** CCM-тег → если ОК, извлечение plaintext → CRC32 проверка.

### Формат INIT-пакета

```
┌───────────────────────────────────────────────────────┬──────────────────────────────────────┐
│ AES-CCM(header(3) || data)  +  auth tag              │ salt(8) || obfuscated_pubkey(32)     │
│ Зашифрованная часть (включает auth tag)               │ Незашифрованная часть (SC_PUBKEY_ENC_SIZE=40) │
└───────────────────────────────────────────────────────┴──────────────────────────────────────┘
```

- Зашифрованная часть: ETCP-заголовок (timestamp uint16 + flag_up uint8) + данные, всё зашифровано AES-128-CCM с auth tag
- Незашифрованная часть: `salt(8) + pubkey XOR SHA256(salt || expected_peer_pubkey)`
- Получатель восстанавливает pubkey: `obfuscated XOR SHA256(salt || my_pubkey) = sender_pubkey` (XOR обратим)

---

## 5. Состояния и flow

```
┌─────────────┐     sc_init_ctx()      ┌──────────────┐
│ Не создан   │ ──────────────────────→│ initialized  │
└─────────────┘                        │ session_ready=0│
                                       └──────┬───────┘
                                              │ sc_set_peer_public_key()
                                              │ (X25519 ECDH → session_key)
                                              ↓
                                       ┌──────────────┐
                                       │ session_ready=1│
                                       │ tx_counter=0  │
                                       │ rx_counter=0  │
                                       └──────┬───────┘
                                              │ sc_encrypt() / sc_decrypt()
                                              ↓
                                       tx_counter++, rx_counter++
```

- `session_ready` становится 1 только после успешного ECDH через `sc_set_peer_public_key()`
- Успешный `sc_set_peer_public_key` сохраняет ключ текущего пира и заново вычисляет session_key
- Счётчики не синхронизируются между сторонами — каждая сторона считает свои tx/rx независимо

---

## 6. Примечания по безопасности

- **CCM nonce** — 13 байт из OpenSSL RAND_bytes; при отказе CSPRNG encrypt возвращает ошибку без готового пакета.
- **CRC32 — не криптографическая защита.** Он дополнительно ловит битовые ошибки после расшифровки (belt-and-suspenders), но основная защита целостности — CCM-тег.
- **Обфускация pubkey — не шифрование.** Это XOR с детерминированным хэшем, обратима для любого, кто знает salt и второй pubkey. Защищает от пассивного наблюдения (не видно публичный ключ в открытом виде), но не от активной подмены.
- X25519, AES-CCM, AES-CTR и Ed25519 выполняются через OpenSSL EVP.
