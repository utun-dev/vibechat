/* chat_whisper — опциональное распознавание русского текста голосовых сообщений (HAVE_WHISPER).
 * Вызывать transcribe в uasync-потоке работающего chat-сервиса. Модель и занятость per-instance;
 * модель лениво загружается в worker и сохраняется до destroy. Отказ допускает повтор.
 * Один запрос на instance: занятость возвращает BUSY через callback, скрытой очереди нет.
 * WAV PCM16 mono/stereo 8..48 kHz либо native-endian контейнер voice_file_encode;
 * максимум 10 минут/128 MiB входного файла, результат до 64 KiB. Ресемплинг — линейный, 16 kHz mono.
 * Путь модели: явная настройка whisper_model_path (без fallback при отказе), иначе ggml-base.bin
 * в текущем каталоге → /usr/share/utun/models/ → ~/.local/share/utun/models/.
 * При изменении пути следующий запрос заменяет модель только после успешной загрузки новой.
 * При stop: destroy отсоединяет владельца, затем media_async_destroy ждёт workers/callbacks;
 * instance и uasync живут до завершения media_async_destroy. */
#ifndef CHAT_WHISPER_H
#define CHAT_WHISPER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

struct UTUN_INSTANCE;
#define CHAT_WHISPER_BUSY (-3)
#define CHAT_WHISPER_MAX_DURATION_MS 600000u
#define CHAT_WHISPER_MAX_TEXT 65536u

/* Ровно один callback, если done задан. err: 0 — текст (возможно пустой), -1 — отказ,
 * -2=MEDIA_ASYNC_CANCELLED — остановка, -3=BUSY. При ошибке text=NULL.
 * Может прийти синхронно;channel/text/reply заимствованы до возврата callback, arg принадлежит caller. */
typedef void (*chat_whisper_done_fn)(void* arg, const char* channel, const char* text, int err, const uint8_t reply[32]);

/* Аргументы обязательны, кроме arg;channel — канонический ID1..INT64_MAX,reply — постоянный message_id.
 * channel/reply/путь модели копируются до возврата, входной regular-файл открывается до запуска worker.
 * path не нужен после возврата;arg/instance должны жить до callback. Настройки фиксируются при приёме.
 * work не обращается к chat_core/SQLite/uasync;done исполняется в потоке экземпляра.
 * NULL done даёт только ERROR;прочие отказы завершаются через done. Отключённая сборка также возвращает -1. */
void chat_whisper_transcribe(struct UTUN_INSTANCE* inst, const char* path, const char* channel,
    const uint8_t reply[32], chat_whisper_done_fn done, void* arg);

/* Отсоединить владельца и отменить публикацию результата. Повтор/NULL допустимы.
 * Не ждёт worker;его модель освобождается в done после join, callback получает CANCELLED.
 * Новые запросы во время stop запрещает chat_started=0;вызывать до media_async_destroy и освобождения chat_core. */
void chat_whisper_destroy(struct UTUN_INSTANCE* inst);
#ifdef __cplusplus
}
#endif
#endif
