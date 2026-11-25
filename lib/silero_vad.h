/**
 * Silero VAD (Voice Activity Detector) — C-обёртка над ONNX Runtime.
 *
 * Запускает официальную стриминговую модель silero_vad.onnx (v5, ~2.3 МБ),
 * которая по окну аудио возвращает вероятность наличия речи [0..1].
 * Рекуррентное состояние модели хранится внутри объекта и переносится между
 * вызовами. Обёртка также хранит 64 последних отсчёта аудио и добавляет их
 * перед новым окном: ONNX получает 576 отсчётов, публичный API принимает 512.
 *
 * Требования к входному аудио:
 *   - частота дискретизации 16 кГц (модель поддерживает только 16k);
 *   - моно;
 *   - окно ровно SILERO_VAD_WINDOW_SAMPLES = 512 сэмплов (32 мс);
 *   - float-сэмплы в диапазоне примерно [-1;1] (для int16 есть
 *     silero_vad_process_pcm16, который сам делит на 32768).
 *
 * Порог и гистерезис (задержки начала/конца речи) намеренно оставлены
 * вызывающему — радио/звонок сами решают, как интерпретировать вероятность.
 *
 * Зависимость: libonnxruntime (C API). Сборка включается опцией
 * `--with-silero-vad` (см. configure.ac) или макросом HAVE_SILERO_VAD в
 * CMake-сборках. Без HAVE_SILERO_VAD модуль компилируется в пустые стабы
 * (все create-функции возвращают NULL, process — ошибку), чтобы исходники lib/
 * можно было GLOB-ить во всех сборках (Android/chatgui) без onnxruntime.
 *
 * Лицензия модели: MIT (Silero Team, https://github.com/snakers4/silero-vad).
 */
#ifndef SILERO_VAD_H
#define SILERO_VAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Фиксированное окно стриминговой модели: 512 сэмплов @ 16 кГц = 32 мс */
#define SILERO_VAD_WINDOW_SAMPLES 512
#define SILERO_VAD_SAMPLE_RATE    16000

typedef struct silero_vad silero_vad_t;

/**
 * Создать детектор, загрузив ONNX-модель из файла model_path.
 * Возвращает NULL при ошибке (подробности — в лог категории "vad").
 */
silero_vad_t* silero_vad_create(const char* model_path);

/**
 * Создать детектор из модели, вшитой в бинарник (silero_vad_model_data в
 * silero_vad.c). Не требует файла на диске. NULL при ошибке.
 */
silero_vad_t* silero_vad_create_default(void);

/* Освободить детектор. NULL безопасен. */
void silero_vad_destroy(silero_vad_t* vad);

/* Сбросить рекуррентное состояние и звуковой контекст (начало нового потока / разрыв захвата). */
void silero_vad_reset(silero_vad_t* vad);

/**
 * Обработать окно из 512 float-сэмплов @ 16 кГц (примерно [-1;1]).
 * При успехе возвращает 0 и заполняет *prob вероятностью речи [0..1];
 * при ошибке — отрицательное значение (проб не трогается).
 */
int silero_vad_process(silero_vad_t* vad, const float* samples, float* prob);

/**
 * То же, но вход — int16 PCM: сэмплы преобразуются к float делением на 32768.
 */
int silero_vad_process_pcm16(silero_vad_t* vad, const int16_t* pcm, float* prob);

#ifdef __cplusplus
}
#endif

#endif /* SILERO_VAD_H */
