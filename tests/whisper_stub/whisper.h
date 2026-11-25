/* Минимальный собственный stub whisper API для test_chat_whisper.c и тестируемого chat_whisper.c.
 * Подключается только через include path тестовой цели; это не ABI заголовок настоящей libwhisper.
 * Backend управляется globals теста: отказ загрузки, отказ/ожидание inference, число сегментов, счётчики lifecycle.
 * Проверяет подготовленный PCM и порядок освобождения; распознавание речи/настоящую модель не выполняет. */
#ifndef TEST_WHISPER_H
#define TEST_WHISPER_H
struct whisper_context; // Собственный контекст stub, освобождается whisper_free после завершения inference.
struct whisper_context_params { int use_gpu; }; // Stub требует use_gpu=0, default_params возвращает нули.
struct whisper_full_params {
    const char* language; // Заимствованный язык; тестовый backend проверяет "ru".
    int n_threads, no_timestamps, single_segment; // Только поля, используемые chat_whisper.c, не layout настоящей библиотеки.
    int print_progress, print_realtime, print_timestamps, print_special; // Флаги вывода, default_params обнуляет их.
};
#define WHISPER_SAMPLING_GREEDY 0
// Вернуть обнулённые параметры; единственный mode теста — CPU.
struct whisper_context_params whisper_context_default_params(void);
// Непустой путь используется как идентификатор, модель с диска не читается; NULL при внедрённом отказе, иначе owned context.
struct whisper_context* whisper_init_from_file_with_params(const char*, struct whisper_context_params);
// Освободить ненулевой контекст и обновить счётчики теста; не вызывать во время whisper_full.
void whisper_free(struct whisper_context*);
// Вернуть нули; strategy принимается для совместимости вызова внутри теста и игнорируется.
struct whisper_full_params whisper_full_default_params(int);
// Заимствованный mono PCM float 16kHz с count>0, нормированный в [-1,1]; 0 — успех, заданный тестом код — отказ.
int whisper_full(struct whisper_context*, struct whisper_full_params, const float*, int);
// Число результатов: 0 либо 2 по настройке теста; context обязателен.
int whisper_full_n_segments(struct whisper_context*);
// Заимствованные литералы "hello" и " world" для индексов 0/1; не освобождать и не использовать вне диапазона сегментов.
const char* whisper_full_get_segment_text(struct whisper_context*, int);
#endif
