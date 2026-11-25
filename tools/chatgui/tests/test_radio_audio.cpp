// test_radio_audio.cpp — тест аудио-движка рации (jitter + микшер).
//
// Проверяет сервисный слой src/radio/radio_audio.c без полного UTUN_INSTANCE:
//   1. single-source: подаём кодированные Opus-кадры через radio_audio_on_frame,
//      radio_audio_pull_pcm выдаёт декодированный ненулевой PCM (jitter+decode).
//   2. mixing: два источника с большими амплитудами — сумма насыщается (soft-clip),
//      что доказывает микширование нескольких источников.
//
// Сборка/запуск: через CMake (tools/chatgui), линкуется utun_voice + utun.

#include "radio/radio_audio.h"

extern "C" {
#include "../../lib/opus_codec.h"
}

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

#define GID 0x1234567890abcdefULL
#define SRC_A 0xAAAAULL
#define SRC_B 0xBBBBULL

static int G_PASSED = 0, G_FAILED = 0, G_TOTAL = 0;
#define TEST(n)  do { G_TOTAL++; printf("  %-58s", n); fflush(stdout); } while(0)
#define OK()     do { G_PASSED++; printf("OK\n"); } while(0)
#define FAIL(f,...) do { G_FAILED++; printf("FAIL: " f "\n", ##__VA_ARGS__); } while(0)

/* закодировать N кадров постоянного PCM и подать через on_frame */
static void push_const(uint64_t src, uint16_t stream, int value, int nframes) {
    opus_codec_encoder_t* enc = opus_codec_encoder_create(48000, 1);
    if (!enc) { FAIL("encoder create failed"); return; }
    opus_codec_encoder_bitrate_set(enc, 32000);

    int16_t pcm[960];
    for (int i = 0; i < 960; i++) pcm[i] = (int16_t)value;

    uint8_t ed[32] = {}; memcpy(ed, &src, sizeof(src));
    for (int f = 0; f < nframes; f++) {
        uint8_t opus[256];
        int len = opus_codec_encode(enc, pcm, 960, opus, (int)sizeof(opus));
        if (len <= 0) { FAIL("encode failed"); break; }
        radio_audio_on_frame(NULL, GID, ed, stream, (uint16_t)f, 0, opus, len, NULL);
    }
    opus_codec_encoder_destroy(enc);
}

/* закодировать n синусоидальных кадров (seq 0..n-1, freq[i]) одним encoder'ом. */
static int encode_frames(const int* freqs, int n, uint8_t pkt[][256], int* lens) {
    opus_codec_encoder_t* enc = opus_codec_encoder_create(48000, 1);
    if (!enc) { FAIL("encoder create failed"); return 0; }
    opus_codec_encoder_bitrate_set(enc, 32000);
    int ok = 1;
    for (int f = 0; f < n; f++) {
        int16_t pcm[960];
        for (int i = 0; i < 960; i++)
            pcm[i] = (int16_t)(sin(2.0 * M_PI * freqs[f] * (double)i / 48000.0) * 10000.0);
        lens[f] = opus_codec_encode(enc, pcm, 960, pkt[f], 256);
        if (lens[f] <= 0) { ok = 0; break; }
    }
    opus_codec_encoder_destroy(enc);
    return ok;
}

/* подать кадры pkt[idx[i]] с seq[i] через on_frame */
static void feed_frames(const uint8_t pkt[][256], const int* lens, const int* seqs, const int* idx, int n) {
    uint8_t ed[32] = {}; uint64_t src = SRC_A; memcpy(ed, &src, sizeof(src));
    for (int i = 0; i < n; i++)
        radio_audio_on_frame(NULL, GID, ed, 1, (uint16_t)seqs[i], 0, pkt[idx[i]], lens[idx[i]], NULL);
}

/* прогнать burst и собрать npulls*960 сэмплов выхода */
static void run_burst(const uint8_t pkt[][256], const int* lens, const int* seqs, const int* idx,
                      int n, int16_t* result, int npulls) {
    radio_audio_start(GID, 1);
    feed_frames(pkt, lens, seqs, idx, n);
    int16_t out[960];
    int pos = 0;
    for (int p = 0; p < npulls; p++) {
        radio_audio_pull_pcm(GID, out, 960);
        for (int i = 0; i < 960; i++) result[pos++] = out[i];
    }
    radio_audio_stop();
}

/* вытянуть pull-цикл, вернуть max |sample| */
static int pull_max_abs(int npulls) {
    int mx = 0;
    int16_t out[960];
    for (int p = 0; p < npulls; p++) {
        int n = radio_audio_pull_pcm(GID, out, 960);
        (void)n;
        for (int i = 0; i < 960; i++) {
            int a = out[i] < 0 ? -out[i] : out[i];
            if (a > mx) mx = a;
        }
    }
    return mx;
}

int main() {
    printf("=== test_radio_audio ===\n"); fflush(stdout);

    TEST("RX-only: захват выключен, приём декодируется"); {
        int rc = radio_audio_start(GID, 0);
        push_const(SRC_A, 1, 10000, 8);
        int mx = pull_max_abs(16);
        int active = radio_audio_active();
        int talking = radio_audio_transmitting();
        radio_audio_stop();
        if (rc == 0 && active && !talking && mx > 1000) OK();
        else FAIL("rc=%d active=%d talking=%d max=%d", rc, active, talking, mx);
    }

    TEST("TX stop/start mono/stereo сохраняет накопленный RX PCM"); {
        const int npulls = 16;
        int freqs[6] = {300, 1000, 2000, 3000, 4000, 5000};
        uint8_t pkt[6][256];
        int lens[6], seqs[6] = {0, 1, 2, 3, 4, 5};
        int16_t ref[npulls * 960], result[npulls * 960];
        int ok = 0;
        if (encode_frames(freqs, 6, pkt, lens)) {
            run_burst(pkt, lens, seqs, seqs, 6, ref, npulls);
            ok = radio_audio_start(GID, 0) == 0;
            feed_frames(pkt, lens, seqs, seqs, 6);
            for (int ch = 1; ch <= 2; ch++) {
                ok = (radio_audio_capture_start(GID, ch, 0) == 0) && ok;
                radio_audio_capture_stop();
                ok = radio_audio_active() && !radio_audio_transmitting() && ok;
            }
            for (int i = 0; i < npulls; i++) radio_audio_pull_pcm(GID, result + i * 960, 960);
            ok = memcmp(ref, result, sizeof(ref)) == 0 && ok;
            radio_audio_stop();
        }
        if (ok) OK(); else FAIL("TX lifecycle изменил RX или не сменил формат захвата");
    }

    TEST("single-source: decode через jitter-буфер выдаёт ненулевой PCM"); {
        radio_audio_start(GID, 1);
        push_const(SRC_A, 1, 10000, 8);
        int mx = pull_max_abs(16);
        radio_audio_stop();
        if (mx > 1000) OK(); else FAIL("max|pcm|=%d (ожидали > 1000)", mx);
    }

    TEST("mixing: два источника складываются (soft-clip)"); {
        radio_audio_start(GID, 1);
        push_const(SRC_A, 1, 20000, 8);
        push_const(SRC_B, 1, 20000, 8);
        int mx = pull_max_abs(16);
        radio_audio_stop();
        if (mx > 30000) OK(); else FAIL("max|pcm|=%d (ожидали > 30000 — сумма двух источников)", mx);
    }

    TEST("reorder: seq 0,2,1,3,5,4 воспроизводятся в порядке 0,1,2,3,4,5"); {
        const int npulls = 16;
        int freqs[6] = {300, 1000, 2000, 3000, 4000, 5000};
        uint8_t pkt[6][256];
        int lens[6];
        int ok = 0;
        if (encode_frames(freqs, 6, pkt, lens)) {
            int seqs_in[6]  = {0, 1, 2, 3, 4, 5}, idx_in[6]  = {0, 1, 2, 3, 4, 5};
            int seqs_out[6] = {0, 2, 1, 3, 5, 4}, idx_out[6] = {0, 2, 1, 3, 5, 4};
            int16_t buf_in[npulls * 960], buf_out[npulls * 960];
            run_burst(pkt, lens, seqs_in, idx_in, 6, buf_in, npulls);
            run_burst(pkt, lens, seqs_out, idx_out, 6, buf_out, npulls);
            ok = (memcmp(buf_in, buf_out, sizeof(buf_in)) == 0);
        }
        if (ok) OK(); else FAIL("out-of-order должен дать тот же PCM, что и in-order (reorder не сработал)");
    }

    TEST("late seq: кадр с seq < next_seq дропается"); {
        const int npulls = 16;
        int freqs[6] = {300, 1000, 2000, 3000, 4000, 5000};
        uint8_t pkt[6][256];
        int lens[6];
        int ok = 0;
        if (encode_frames(freqs, 6, pkt, lens)) {
            int seqs_ref[6]  = {0, 1, 2, 3, 4, 5}, idx_ref[6] = {0, 1, 2, 3, 4, 5};
            int seqs_late[7] = {0, 1, 2, 3, 4, 5, 0}, idx_late[7] = {0, 1, 2, 3, 4, 5, 0};  /* последний — поздний дубль seq=0 */
            int16_t buf_ref[npulls * 960], buf_late[npulls * 960];
            run_burst(pkt, lens, seqs_ref, idx_ref, 6, buf_ref, npulls);
            run_burst(pkt, lens, seqs_late, idx_late, 7, buf_late, npulls);
            ok = (memcmp(buf_ref, buf_late, sizeof(buf_ref)) == 0);
        }
        if (ok) OK(); else FAIL("поздний дубль seq=0 должен быть дропнут (PCM совпадает с эталоном)");
    }

    printf("\n%d/%d passed, %d failed\n", G_PASSED, G_TOTAL, G_FAILED);
    return G_FAILED > 0 ? 1 : 0;
}
