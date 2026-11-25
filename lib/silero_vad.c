#include "silero_vad.h"
#include "debug_config.h"
#include "mem.h"

#ifdef HAVE_SILERO_VAD

#include <stdio.h>
#include <string.h>

#include <onnxruntime/onnxruntime_c_api.h>

#include "silero_vad_model.inc"

/* Число float-элементов рекуррентного состояния модели: 2 * 1 * 128 */
#define SILERO_VAD_STATE_COUNT 256
/* ONNX-вход: 64 отсчёта предыдущего окна + 512 новых отсчётов @16кГц. */
#define SILERO_VAD_CONTEXT_SAMPLES 64
#define SILERO_VAD_INPUT_SAMPLES (SILERO_VAD_CONTEXT_SAMPLES + SILERO_VAD_WINDOW_SAMPLES)

struct silero_vad {
    const OrtApi* ort;
    OrtEnv* env;
    OrtSession* session;
    OrtMemoryInfo* mem_info;
    float state[SILERO_VAD_STATE_COUNT];   /* рекуррентное состояние между вызовами */
    float context[SILERO_VAD_CONTEXT_SAMPLES]; /* звуковой контекст предыдущего окна */
    uint8_t* model_data;                   /* байты .onnx, загруженные в память (для file-версии) */
    size_t model_len;
};

/* Освободить OrtStatus и залогировать его сообщение как ошибку */
static void vad_report_ort_error(const OrtApi* ort, OrtStatus* status, const char* what) {
    const char* msg = ort->GetErrorMessage(status);
    DEBUG_ERROR(DEBUG_CATEGORY_VAD, "%s: %s", what, msg ? msg : "unknown onnxruntime error");
    ort->ReleaseStatus(status);
}

/* Прочитать файл целиком в буфер. 0 при успехе: буфер пишется в out/out_len (u_malloc). */
static int vad_read_file(const char* path, uint8_t** out, size_t* out_len) {
    FILE* f;
    long size;
    uint8_t* buf;

    *out = NULL;
    *out_len = 0;

    f = fopen(path, "rb");
    if (!f) {
        DEBUG_ERROR(DEBUG_CATEGORY_VAD, "vad_read_file: cannot open model '%s'", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_VAD, "vad_read_file: fseek failed on '%s'", path);
        fclose(f);
        return -1;
    }
    size = ftell(f);
    if (size <= 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_VAD, "vad_read_file: empty or unreadable model '%s'", path);
        fclose(f);
        return -1;
    }
    rewind(f);

    buf = (uint8_t*)u_malloc((size_t)size);
    if (!buf) {
        DEBUG_ERROR(DEBUG_CATEGORY_VAD, "vad_read_file: OOM for %ld bytes", size);
        fclose(f);
        return -1;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        DEBUG_ERROR(DEBUG_CATEGORY_VAD, "vad_read_file: short read on '%s'", path);
        u_free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    *out = buf;
    *out_len = (size_t)size;
    return 0;
}

/* Общая инициализация детектора из байтов модели (файл или вшитая модель).
 * owned_buf передаётся детектору только при успехе; при ошибке остаётся у вызывающего.
 * Для вшитой модели owned_buf == NULL, её байты не освобождаются. */
static silero_vad_t* silero_vad_create_from_bytes(const uint8_t* data, size_t len,
                                                  const uint8_t* owned_buf, const char* label) {
    silero_vad_t* vad = NULL;
    const OrtApi* ort;
    OrtStatus* status;
    OrtSessionOptions* opts = NULL;

    if (!data || len == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_VAD, "silero_vad_create(%s): empty model", label);
        return NULL;
    }

    ort = OrtGetApiBase() ? OrtGetApiBase()->GetApi(ORT_API_VERSION) : NULL;
    if (!ort) {
        DEBUG_ERROR(DEBUG_CATEGORY_VAD, "silero_vad_create(%s): onnxruntime C API unavailable", label);
        return NULL;
    }

    vad = (silero_vad_t*)u_calloc(1, sizeof(*vad));
    if (!vad) {
        DEBUG_ERROR(DEBUG_CATEGORY_VAD, "silero_vad_create(%s): OOM", label);
        return NULL;
    }
    vad->ort = ort;
    vad->model_len = len;

    status = ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "utun_silero_vad", &vad->env);
    if (status) { vad_report_ort_error(ort, status, "CreateEnv"); goto fail; }

    status = ort->CreateSessionOptions(&opts);
    if (status) { vad_report_ort_error(ort, status, "CreateSessionOptions"); goto fail; }
    status = ort->SetIntraOpNumThreads(opts, 1);
    if (status) { vad_report_ort_error(ort, status, "SetIntraOpNumThreads"); }
    status = ort->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL);
    if (status) { vad_report_ort_error(ort, status, "SetSessionGraphOptimizationLevel"); }

    status = ort->CreateSessionFromArray(vad->env, data, len, opts, &vad->session);
    if (status) { vad_report_ort_error(ort, status, "CreateSessionFromArray"); goto fail; }

    status = ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &vad->mem_info);
    if (status) { vad_report_ort_error(ort, status, "CreateCpuMemoryInfo"); goto fail; }

    ort->ReleaseSessionOptions(opts);
    vad->model_data = (uint8_t*)owned_buf;

    DEBUG_INFO(DEBUG_CATEGORY_VAD, "silero_vad_create(%s): ok bytes=%zu window=%d context=%d input=%d", label, len,
               SILERO_VAD_WINDOW_SAMPLES, SILERO_VAD_CONTEXT_SAMPLES, SILERO_VAD_INPUT_SAMPLES);
    return vad;

fail:
    if (opts) ort->ReleaseSessionOptions(opts);
    silero_vad_destroy(vad);
    return NULL;
}

silero_vad_t* silero_vad_create(const char* model_path) {
    uint8_t* buf = NULL;
    size_t len = 0;
    silero_vad_t* vad;

    if (!model_path || !*model_path) {
        DEBUG_ERROR(DEBUG_CATEGORY_VAD, "silero_vad_create: empty model path");
        return NULL;
    }
    if (vad_read_file(model_path, &buf, &len) != 0) return NULL;
    vad = silero_vad_create_from_bytes(buf, len, buf, model_path);
    if (!vad) u_free(buf);
    return vad;
}

silero_vad_t* silero_vad_create_default(void) {
    return silero_vad_create_from_bytes(silero_vad_model_data, silero_vad_model_len, NULL, "embedded");
}

void silero_vad_destroy(silero_vad_t* vad) {
    if (!vad) return;
    const OrtApi* ort = vad->ort;
    if (ort) {
        if (vad->mem_info) ort->ReleaseMemoryInfo(vad->mem_info);
        if (vad->session) ort->ReleaseSession(vad->session);
        if (vad->env) ort->ReleaseEnv(vad->env);
    }
    if (vad->model_data) u_free(vad->model_data);
    DEBUG_DEBUG(DEBUG_CATEGORY_VAD, "silero_vad_destroy");
    u_free(vad);
}

void silero_vad_reset(silero_vad_t* vad) {
    if (!vad) return;
    memset(vad->state, 0, sizeof(vad->state));
    memset(vad->context, 0, sizeof(vad->context));
    DEBUG_DEBUG(DEBUG_CATEGORY_VAD, "silero_vad_reset: recurrent state and audio context cleared");
}

int silero_vad_process(silero_vad_t* vad, const float* samples, float* prob) {
    const OrtApi* ort;
    OrtStatus* status;
    OrtValue* in_input = NULL;
    OrtValue* in_state = NULL;
    OrtValue* in_sr = NULL;
    OrtValue* out_prob = NULL;
    OrtValue* out_state = NULL;
    const char* input_names[3];
    const char* output_names[2];
    const OrtValue* inputs[3];
    OrtValue* outputs[2];
    int64_t shape_input[2] = {1, SILERO_VAD_INPUT_SAMPLES};
    int64_t shape_state[3] = {2, 1, 128};
    int64_t shape_prob[2] = {1, 1};
    int64_t sr_value = SILERO_VAD_SAMPLE_RATE;
    float prob_local = 0.0f;
    float next_state[SILERO_VAD_STATE_COUNT];
    float input[SILERO_VAD_INPUT_SAMPLES];
    int rc = -1;

    if (!vad || !samples || !prob) {
        DEBUG_ERROR(DEBUG_CATEGORY_VAD, "silero_vad_process: invalid args");
        return -1;
    }
    ort = vad->ort;
    memcpy(input, vad->context, sizeof(vad->context));
    memcpy(input + SILERO_VAD_CONTEXT_SAMPLES, samples, SILERO_VAD_WINDOW_SAMPLES * sizeof(float));

    status = ort->CreateTensorWithDataAsOrtValue(vad->mem_info, input,
            sizeof(input), shape_input, 2,
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in_input);
    if (status) { vad_report_ort_error(ort, status, "input tensor"); return -1; }

    status = ort->CreateTensorWithDataAsOrtValue(vad->mem_info, vad->state,
            SILERO_VAD_STATE_COUNT * sizeof(float), shape_state, 3,
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in_state);
    if (status) { vad_report_ort_error(ort, status, "state tensor"); goto done; }

    status = ort->CreateTensorWithDataAsOrtValue(vad->mem_info, &sr_value,
            sizeof(sr_value), NULL, 0,
            ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in_sr);
    if (status) { vad_report_ort_error(ort, status, "sr tensor"); goto done; }

    status = ort->CreateTensorWithDataAsOrtValue(vad->mem_info, &prob_local,
            sizeof(float), shape_prob, 2,
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &out_prob);
    if (status) { vad_report_ort_error(ort, status, "output tensor"); goto done; }

    status = ort->CreateTensorWithDataAsOrtValue(vad->mem_info, next_state,
            SILERO_VAD_STATE_COUNT * sizeof(float), shape_state, 3,
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &out_state);
    if (status) { vad_report_ort_error(ort, status, "stateN tensor"); goto done; }

    input_names[0] = "input";
    input_names[1] = "state";
    input_names[2] = "sr";
    inputs[0] = in_input;
    inputs[1] = in_state;
    inputs[2] = in_sr;

    output_names[0] = "output";
    output_names[1] = "stateN";
    outputs[0] = out_prob;
    outputs[1] = out_state;

    status = ort->Run(vad->session, NULL, input_names, inputs, 3, output_names, 2, outputs);
    if (status) {
        vad_report_ort_error(ort, status, "Run");
        goto done;
    }

    memcpy(vad->state, next_state, sizeof(next_state));
    memcpy(vad->context, input + SILERO_VAD_INPUT_SAMPLES - SILERO_VAD_CONTEXT_SAMPLES, sizeof(vad->context));
    *prob = prob_local;
    rc = 0;

done:
    if (in_input) ort->ReleaseValue(in_input);
    if (in_state) ort->ReleaseValue(in_state);
    if (in_sr) ort->ReleaseValue(in_sr);
    if (out_prob) ort->ReleaseValue(out_prob);
    if (out_state) ort->ReleaseValue(out_state);
    return rc;
}

int silero_vad_process_pcm16(silero_vad_t* vad, const int16_t* pcm, float* prob) {
    float samples[SILERO_VAD_WINDOW_SAMPLES];
    int i;

    if (!pcm) {
        DEBUG_ERROR(DEBUG_CATEGORY_VAD, "silero_vad_process_pcm16: pcm == NULL");
        return -1;
    }
    for (i = 0; i < SILERO_VAD_WINDOW_SAMPLES; i++) {
        samples[i] = (float)pcm[i] / 32768.0f;
    }
    return silero_vad_process(vad, samples, prob);
}

#else  /* !HAVE_SILERO_VAD — пустые стабы (сборки без onnxruntime) */

silero_vad_t* silero_vad_create(const char* model_path) {
    (void)model_path;
    DEBUG_ERROR(DEBUG_CATEGORY_VAD, "silero_vad_create: built without onnxruntime (HAVE_SILERO_VAD not defined)");
    return NULL;
}

silero_vad_t* silero_vad_create_default(void) {
    DEBUG_ERROR(DEBUG_CATEGORY_VAD, "silero_vad_create_default: built without onnxruntime (HAVE_SILERO_VAD not defined)");
    return NULL;
}

void silero_vad_destroy(silero_vad_t* vad) {
    (void)vad;
}

void silero_vad_reset(silero_vad_t* vad) {
    (void)vad;
}

int silero_vad_process(silero_vad_t* vad, const float* samples, float* prob) {
    (void)vad; (void)samples; (void)prob;
    return -1;
}

int silero_vad_process_pcm16(silero_vad_t* vad, const int16_t* pcm, float* prob) {
    (void)vad; (void)pcm; (void)prob;
    return -1;
}

#endif /* HAVE_SILERO_VAD */
