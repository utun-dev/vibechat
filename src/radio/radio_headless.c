// radio_headless.c — headless-аудио-сокет рации (PCM, без тактирования).
//
// TCP-сокет с бинарным фреймингом [type:1][group_ed25519:32][len:2 BE][data:len].
// Клиент привязывается к каналу (HELLO) и обменивается готовым PCM (int16 mono 48k):
//   клиент → ядро: FRAME (960 сэмплов) — кодируется в Opus и флудится подписчикам;
//   клиент → ядро: PTT_START / PTT_STOP — radio_talk_begin / radio_talk_end;
//   ядро → клиент: микшированный PCM всех говорящих (FRAME), таймер 20мс.
// Таблица говорящих: по Ed25519 декодер + PCM-ring; один пир — один
// поток; инстанс удаляется по FIN (дренаж) или по таймауту 2с. Без time-stretch —
// тактирование на стороне клиента (как в call_headless).
//
// Управление подпиской (вкл/выкл рации) — на control-сокете (chat_headless_control):
// radio_on / radio_off.
#include "radio_headless.h"
#include "radio.h"
#include "../utun_instance.h"
#include "../routing_layer/topo_group.h"
#include "../../lib/opus_codec.h"
#include "../../lib/u_async.h"
#include "../../lib/mem.h"
#include "../../lib/socket_compat.h"
#include "../../lib/debug_config.h"
#include "../../lib/platform_compat.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#define RH_ID "radio_headless"

#define RH_RECV_BUF_SIZE 65536
#define RH_SEND_BUF_SIZE 65536
#define RH_MAX_CLIENTS   16
#define RH_MAX_FRAME     2048

#define RH_SAMPLE_RATE       48000
#define RH_FRAME_SAMPLES     960                  /* 20мс @48k */
#define RH_PCM_FRAME_BYTES   (RH_FRAME_SAMPLES * (int)sizeof(int16_t))   /* 1920 */
#define RH_BITRATE           32000

#define RH_MAX_SOURCES       16
#define RH_RING_FRAMES       25                   /* ~500мс @20мс */
#define RH_RING_SAMPLES      (RH_RING_FRAMES * RH_FRAME_SAMPLES)
#define RH_MIX_TIMER_TB      200                  /* 20мс — период микширования */
#define RH_SOURCE_TIMEOUT_TB 20000                /* 2с без кадров → источник завершён */

struct rh_source {
    uint8_t src_ed[32];
    uint16_t stream_id;
    uint8_t  used;
    uint8_t  ending;
    uint64_t last_frame_tb;
    opus_codec_decoder_t* dec;
    int16_t* ring;              /* u_malloc(RH_RING_SAMPLES) */
    int      rd, wr, count;
};

struct radio_headless_client {
    socket_t fd;
    void*    socket_id;
    uint8_t  group_key[32];     /* Ключ из HELLO, нули до привязки. */
    uint8_t  recv_buf[RH_RECV_BUF_SIZE];
    int      recv_len;
    uint8_t  send_buf[RH_SEND_BUF_SIZE];
    int      send_len;
    int      send_offset;
    int      closing;
    struct radio_headless* rh;
    struct radio_headless_client* next;
};

struct radio_headless {
    struct UASYNC*          ua;
    struct UTUN_INSTANCE*   inst;
    struct radio_headless_client* clients;
    int                     client_count;
    socket_t                listen_fd;
    void*                   listen_sock_id;
    int                     running;
    uint64_t                group_id;               /* активный канал (0 = не задан) */
    uint64_t                enc_group_id;
    opus_codec_encoder_t*   enc;                    /* TX-кодер активного канала */
    struct rh_source        sources[RH_MAX_SOURCES];
    void*                   mix_timer;
    void*                   cleanup_timer; // отменяется вместе с mix_timer до освобождения контекста
    int32_t                 mix[RH_FRAME_SAMPLES];
    /* диагностика */
    uint32_t c_enc_frames, c_dec_frames, c_enc_dropped, c_dec_dropped;
};

static struct radio_headless* rh_of(struct UTUN_INSTANCE* inst) {
    return inst ? (struct radio_headless*)inst->radio_headless : NULL;
}

static void rh_close_client(struct radio_headless_client* cli);

/* ── отправка фрейма клиенту (буферизация как в call_headless) ── */

static void rh_send_raw(struct radio_headless_client* cli, const uint8_t* data, size_t len) {
    struct radio_headless* rh = cli->rh;
    if (!cli || cli->closing || cli->fd == SOCKET_INVALID || !data || len == 0) return;
    if (cli->send_len > 0) {
        size_t space = sizeof(cli->send_buf) - (size_t)cli->send_len;
        if (len > space) return;
        memcpy(cli->send_buf + cli->send_len, data, len);
        cli->send_len += (int)len;
        return;
    }
    ssize_t r = send(cli->fd, data, (int)len, 0);
    if (r < 0 && (socket_get_error() == ERR_AGAIN || socket_get_error() == ERR_WOULDBLOCK)) {
        if (len <= sizeof(cli->send_buf)) { memcpy(cli->send_buf, data, len); cli->send_len = (int)len; cli->send_offset = 0; }
        if (rh && rh->ua) uasync_set_socket_write(rh->ua, cli->socket_id, 1);
        return;
    }
    if (r >= 0 && (size_t)r < len) {
        size_t remain = len - (size_t)r;
        if (remain <= sizeof(cli->send_buf)) { memcpy(cli->send_buf, data + r, remain); cli->send_len = (int)remain; cli->send_offset = 0; }
        if (rh && rh->ua) uasync_set_socket_write(rh->ua, cli->socket_id, 1);
    }
    if (r < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: send failed err=%d, closing client", RH_ID, socket_get_error());
        rh_close_client(cli);
    }
}

static void rh_write_frame(struct radio_headless_client* cli, uint8_t type, const uint8_t group_key[32],
                           const uint8_t* data, uint16_t len) {
    uint8_t frame[RADIO_AUDIO_HDR_SIZE + RH_MAX_FRAME];
    if (len > RH_MAX_FRAME) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: outgoing frame too big %u", RH_ID, len);
        return;
    }
    frame[0] = type;
    memcpy(frame + 1, group_key, 32);
    frame[RADIO_AUDIO_LEN_OFF] = (uint8_t)(len >> 8);
    frame[RADIO_AUDIO_LEN_OFF + 1] = (uint8_t)len;
    memcpy(frame + RADIO_AUDIO_HDR_SIZE, data, len);
    rh_send_raw(cli, frame, RADIO_AUDIO_HDR_SIZE + len);
}

/* ── Opus-кодер активного канала ── */

static opus_codec_encoder_t* rh_encoder(struct radio_headless* rh, uint64_t group_id) {
    if (rh->enc && rh->enc_group_id == group_id) return rh->enc;
    if (rh->enc) { opus_codec_encoder_destroy(rh->enc); rh->enc = NULL; }
    rh->enc = opus_codec_encoder_create(RH_SAMPLE_RATE, 1);
    if (rh->enc) {
        opus_codec_encoder_bitrate_set(rh->enc, RH_BITRATE);
        rh->enc_group_id = group_id;
    } else {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: encoder create failed", RH_ID);
    }
    return rh->enc;
}

/* ── таблица говорящих ── */

static struct rh_source* rh_src_find(struct radio_headless* rh, const uint8_t ed[32]) {
    for (int i = 0; i < RH_MAX_SOURCES; i++)
        if (rh->sources[i].used && !memcmp(rh->sources[i].src_ed, ed, 32)) return &rh->sources[i];
    return NULL;
}

static void rh_src_free(struct rh_source* s) {
    if (s->dec) { opus_codec_decoder_destroy(s->dec); s->dec = NULL; }
    if (s->ring) { u_free(s->ring); s->ring = NULL; }
    s->used = 0; s->ending = 0; s->rd = s->wr = s->count = 0;
}

static void rh_member_mute(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32], int muted, void* arg) {
    (void)inst;
    struct radio_headless* rh = arg;
    if (!rh || !rh->running || rh->group_id != group_id || !muted) return;
    for (int i = 0; i < RH_MAX_SOURCES; i++) {
        struct rh_source* s = &rh->sources[i];
        if (!s->used || memcmp(ed, s->src_ed, 32)) continue;
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: clear muted source grp=%016llx src=%s",
                    RH_ID, (unsigned long long)group_id, sc_public_key_hex(ed, (char[65]){0}));
        rh_src_free(s);
    }
}

static struct rh_source* rh_src_acquire(struct radio_headless* rh, const uint8_t ed[32], uint16_t stream) {
    struct rh_source* s = rh_src_find(rh, ed);
    if (s) {
        if (s->stream_id != stream) {
            s->stream_id = stream;
            s->ending = 0; s->rd = s->wr = s->count = 0;
            DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: source reset src=%s stream=%u", RH_ID, sc_public_key_hex(ed, (char[65]){0}), stream);
        }
        return s;
    }
    for (int i = 0; i < RH_MAX_SOURCES; i++) {
        if (rh->sources[i].used) continue;
        s = &rh->sources[i];
        s->dec = opus_codec_decoder_create(RH_SAMPLE_RATE, 1);
        if (!s->dec) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: decoder create failed", RH_ID); return NULL; }
        s->ring = u_malloc(RH_RING_SAMPLES * sizeof(int16_t));
        if (!s->ring) { opus_codec_decoder_destroy(s->dec); s->dec = NULL; return NULL; }
        memcpy(s->src_ed, ed, 32);
        s->stream_id = stream;
        s->used = 1; s->ending = 0; s->rd = s->wr = s->count = 0;
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: new source src=%s stream=%u", RH_ID, sc_public_key_hex(ed, (char[65]){0}), stream);
        return s;
    }
    DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: too many sources (>=%d), drop src=%s", RH_ID, RH_MAX_SOURCES, sc_public_key_hex(ed, (char[65]){0}));
    return NULL;
}

static void rh_src_push(struct rh_source* s, const int16_t* pcm, int n) {
    int cap = RH_RING_SAMPLES;
    if (n > cap) { pcm += n - cap; n = cap; }
    if (s->count + n > cap) {
        int overflow = s->count + n - cap;
        s->rd = (s->rd + overflow) % cap;
        s->count -= overflow;
    }
    for (int i = 0; i < n; i++) {
        s->ring[s->wr] = pcm[i];
        s->wr = (s->wr + 1) % cap;
    }
    s->count += n;
}

/* ── приём Opus-кадра пира (frame cb) → декод → ring источника ── */

static void rh_frame_cb(struct UTUN_INSTANCE* inst, uint64_t group_id,
                        const uint8_t src_ed[32], uint16_t stream_id, uint16_t seq,
                        uint8_t fin, const uint8_t* opus, int len, void* arg) {
    struct radio_headless* rh = (struct radio_headless*)arg;
    if (!rh || !rh->running || !opus || len <= 0) return;

    /* активный канал: первый кадр фиксирует group_id (узел слушает один канал) */
    if (rh->group_id == 0) rh->group_id = group_id;
    if (group_id != rh->group_id) return;
    if (radio_member_silenced(inst, group_id, src_ed)) {
        if (seq == 0) DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: muted burst grp=%016llx src=%s stream=%u",
            RH_ID, (unsigned long long)group_id, sc_public_key_hex(src_ed, (char[65]){0}), stream_id);
        return;
    }

    struct rh_source* s = rh_src_acquire(rh, src_ed, stream_id);
    if (!s) return;
    int16_t pcm[RH_FRAME_SAMPLES];
    int n = opus_codec_decode(s->dec, opus, len, pcm, RH_FRAME_SAMPLES);
    if (n <= 0) { rh->c_dec_dropped++; return; }
    rh_src_push(s, pcm, n);
    rh->c_dec_frames++;
    s->last_frame_tb = get_time_tb();
    if (fin) s->ending = 1;
}

/* ── периодический микшер: sum источников → PCM клиентам ── */

static void rh_mix_tick(void* arg) {
    struct radio_headless* rh = (struct radio_headless*)arg;
    rh->mix_timer = NULL;
    if (!rh || !rh->running) return;

    if (rh->group_id != 0) {
        memset(rh->mix, 0, sizeof(rh->mix));
        uint64_t now = get_time_tb();
        int any = 0;

        for (int i = 0; i < RH_MAX_SOURCES; i++) {
            struct rh_source* s = &rh->sources[i];
            if (!s->used) continue;
            if (!s->ending && s->last_frame_tb && (now - s->last_frame_tb > RH_SOURCE_TIMEOUT_TB)) {
                DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: source timeout src=%s", RH_ID, sc_public_key_hex(s->src_ed, (char[65]){0}));
                rh_src_free(s);
                continue;
            }
            int take = s->count < RH_FRAME_SAMPLES ? s->count : RH_FRAME_SAMPLES;
            for (int k = 0; k < take; k++) {
                rh->mix[k] += s->ring[s->rd];
                s->rd = (s->rd + 1) % RH_RING_SAMPLES;
            }
            s->count -= take;
            if (take > 0) any = 1;
            if (s->ending && s->count == 0) {
                DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: source ended src=%s", RH_ID, sc_public_key_hex(s->src_ed, (char[65]){0}));
                rh_src_free(s);
            }
        }

        /* шлём PCM клиентам только когда есть что микшировать (тишина — неявно) */
        if (any) {
            int16_t pcm[RH_FRAME_SAMPLES];
            for (int k = 0; k < RH_FRAME_SAMPLES; k++) {
                int32_t v = rh->mix[k];
                if (v > 32767) v = 32767;
                else if (v < -32768) v = -32768;
                pcm[k] = (int16_t)v;
            }
            struct TOPO_GROUP* group = topo_groups_find(rh->inst->topo_groups, rh->group_id);
            const uint8_t* key = group && group->group_type == TOPO_GROUP_TYPE_CHAT ? topo_group_wire_id(group) : NULL;
            if (key) {
                for (struct radio_headless_client* c = rh->clients; c; c = c->next) {
                    if (!c->closing && !memcmp(c->group_key, key, 32))
                        rh_write_frame(c, RADIO_AUDIO_FRAME, key, (const uint8_t*)pcm, RH_PCM_FRAME_BYTES);
                }
            }
        }
    }

    rh->mix_timer = uasync_set_timeout(rh->ua, RH_MIX_TIMER_TB, rh, rh_mix_tick, "rh_mix");
    if (!rh->mix_timer) DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: mix timer scheduling failed", RH_ID);
}

/* ── разбор входящих фреймов ── */

static void rh_parse_frames(struct radio_headless_client* cli) {
    struct radio_headless* rh = cli->rh;
    uint8_t* p = cli->recv_buf;
    int avail = cli->recv_len;
    int consumed = 0;
    while (avail - consumed >= RADIO_AUDIO_HDR_SIZE) {
        const uint8_t* header = p + consumed;
        uint8_t type = header[0];
        uint16_t flen = (uint16_t)((header[RADIO_AUDIO_LEN_OFF] << 8) | header[RADIO_AUDIO_LEN_OFF + 1]);
        if (flen > RH_MAX_FRAME) {
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: frame too big %u — closing", RH_ID, flen);
            rh_close_client(cli);
            return;
        }
        if (avail - consumed < (int)(RADIO_AUDIO_HDR_SIZE + flen)) break;
        struct TOPO_GROUP* group = topo_groups_find_wire(rh->inst->topo_groups, header + 1);
        if (!group || group->group_type != TOPO_GROUP_TYPE_CHAT || (type != RADIO_AUDIO_HELLO && memcmp(cli->group_key, header + 1, 32)) ||
            (type != RADIO_AUDIO_FRAME && flen)) {
            DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: invalid audio scope or control size type=0x%02x group=%s size=%u bound=%s",
                RH_ID, type, sc_public_key_hex(header + 1, (char[65]){0}), flen, sc_public_key_hex(cli->group_key, (char[65]){0}));
            rh_close_client(cli);
            return;
        }
        uint64_t group_id = group->group_id;
        const uint8_t* data = header + RADIO_AUDIO_HDR_SIZE;

        switch (type) {
        case RADIO_AUDIO_HELLO:
            memcpy(cli->group_key, header + 1, 32);
            if (rh->group_id == 0) rh->group_id = group_id;
            DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: client bound group=%s", RH_ID, sc_public_key_hex(header + 1, (char[65]){0}));
            break;
        case RADIO_AUDIO_PTT_START:
            radio_talk_begin(rh->inst, group_id);
            break;
        case RADIO_AUDIO_PTT_STOP:
            radio_talk_end(rh->inst, group_id);
            break;
        case RADIO_AUDIO_FRAME:
            if (flen != RH_PCM_FRAME_BYTES) {
                rh->c_enc_dropped++;
                DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: bad PCM frame size=%u (expect %d) — dropped",
                           RH_ID, flen, RH_PCM_FRAME_BYTES);
            } else {
                opus_codec_encoder_t* enc = rh_encoder(rh, group_id);
                int16_t pcm[RH_FRAME_SAMPLES]; memcpy(pcm, data, sizeof(pcm)); // Последовательность байтов может быть не выровнена.
                uint8_t opus[256];
                int olen = enc ? opus_codec_encode(enc, pcm, RH_FRAME_SAMPLES, opus, (int)sizeof(opus)) : -1;
                if (olen <= 0) {
                    rh->c_enc_dropped++;
                    DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: encode failed (olen=%d) group=0x%016llx",
                               RH_ID, olen, (unsigned long long)group_id);
                } else {
                    rh->c_enc_frames++;
                    if (radio_talk_send(rh->inst, group_id, opus, olen) != 0)
                        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: talk_send failed group=0x%016llx len=%d",
                                   RH_ID, (unsigned long long)group_id, olen);
                }
            }
            break;
        default:
            DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: unknown frame type=0x%02x — closing", RH_ID, type);
            rh_close_client(cli);
            return;
        }
        consumed += RADIO_AUDIO_HDR_SIZE + flen;
    }
    if (consumed > 0) {
        memmove(cli->recv_buf, cli->recv_buf + consumed, (size_t)(cli->recv_len - consumed));
        cli->recv_len -= consumed;
    }
}

/* ── socket callbacks ── */

static void rh_write_callback(socket_t fd, void* arg) {
    struct radio_headless_client* cli = (struct radio_headless_client*)arg;
    struct radio_headless* rh = cli ? cli->rh : NULL;
    if (!cli || cli->closing || fd != cli->fd) return;
    if (cli->send_len <= 0) { if (rh && rh->ua) uasync_set_socket_write(rh->ua, cli->socket_id, 0); return; }
    ssize_t r = send(cli->fd, cli->send_buf + cli->send_offset, cli->send_len - cli->send_offset, 0);
    if (r < 0 && (socket_get_error() == ERR_AGAIN || socket_get_error() == ERR_WOULDBLOCK)) return;
    if (r <= 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: send failed (r=%zd err=%d) — closing client", RH_ID, r, socket_get_error());
        rh_close_client(cli);
        return;
    }
    cli->send_offset += (int)r;
    if (cli->send_offset >= cli->send_len) { cli->send_len = 0; cli->send_offset = 0; if (rh && rh->ua) uasync_set_socket_write(rh->ua, cli->socket_id, 0); }
}

static void rh_close_client(struct radio_headless_client* cli) {
    struct radio_headless* rh = cli ? cli->rh : NULL;
    if (!cli || cli->closing) return;
    cli->closing = 1;
    if (rh && rh->ua && cli->socket_id) uasync_remove_socket_t(rh->ua, cli->fd);
    if (cli->fd != SOCKET_INVALID) { socket_close_wrapper(cli->fd); cli->fd = SOCKET_INVALID; }
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: client closed group=%s", RH_ID, sc_public_key_hex(cli->group_key, (char[65]){0}));
}

static void rh_read_callback(socket_t fd, void* arg) {
    struct radio_headless_client* cli = (struct radio_headless_client*)arg;
    if (!cli || cli->closing) return;
    uint8_t tmp[4096];
    ssize_t r = recv(fd, tmp, (int)sizeof(tmp), 0);
    if (r <= 0) { rh_close_client(cli); return; }
    if (cli->recv_len + (int)r > RH_RECV_BUF_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: recv buffer overflow — closing", RH_ID);
        rh_close_client(cli);
        return;
    }
    memcpy(cli->recv_buf + cli->recv_len, tmp, (size_t)r);
    cli->recv_len += (int)r;
    rh_parse_frames(cli);
}

static void rh_accept_callback(socket_t fd, void* arg) {
    struct radio_headless* rh = (struct radio_headless*)arg;
    struct sockaddr_storage addr; socklen_t alen = sizeof(addr);
    socket_t cfd = accept(fd, (struct sockaddr*)&addr, &alen);
    if (cfd == SOCKET_INVALID) {
        int e = socket_get_error();
        if (e != ERR_AGAIN && e != ERR_WOULDBLOCK) DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: accept failed: %s", RH_ID, socket_strerror(e));
        return;
    }
    socket_set_nonblocking(cfd);
    if (rh->client_count >= RH_MAX_CLIENTS) { socket_close_wrapper(cfd); DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: max clients reached", RH_ID); return; }

    struct radio_headless_client* cli = u_calloc(1, sizeof(*cli));
    if (!cli) { socket_close_wrapper(cfd); return; }
    cli->fd = cfd;
    cli->rh = rh;
    cli->socket_id = uasync_add_socket_t(rh->ua, cfd, rh_read_callback, rh_write_callback, NULL, "radio_client", cli);
    if (!cli->socket_id) { u_free(cli); socket_close_wrapper(cfd); DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: failed to register client fd", RH_ID); return; }

    cli->next = rh->clients; rh->clients = cli; rh->client_count++;
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: client connected fd=%d total=%d", RH_ID, (int)cfd, rh->client_count);
}

/* ── периодическая очистка закрытых клиентов ── */

static void rh_cleanup(void* arg) {
    struct radio_headless* rh = (struct radio_headless*)arg;
    rh->cleanup_timer = NULL;
    if (!rh->running) return;
    struct radio_headless_client** prev = &rh->clients;
    while (*prev) {
        if ((*prev)->closing) {
            struct radio_headless_client* dead = *prev; *prev = dead->next;
            if (dead->fd != SOCKET_INVALID) socket_close_wrapper(dead->fd);
            u_free(dead); rh->client_count--;
        } else prev = &(*prev)->next;
    }
    rh->cleanup_timer = uasync_set_timeout(rh->ua, 5000, rh, rh_cleanup, "rh_cleanup");
    if (!rh->cleanup_timer) DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: cleanup timer scheduling failed", RH_ID);
}

/* ── init / destroy ── */

int radio_headless_init(struct UASYNC* ua, struct UTUN_INSTANCE* inst,
                        const char* bind_ip, int port) {
    if (!ua || !inst || !bind_ip || port < 0 || port > 65535) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: invalid init arguments port=%d", RH_ID, port); return -1;
    }
    struct radio_headless* rh = rh_of(inst);
    if (rh && rh->running) return 0;

    if (!rh) {
        rh = u_calloc(1, sizeof(*rh));
        if (!rh) return -1;
        inst->radio_headless = rh;
    }
    rh->ua = ua; rh->inst = inst;

    rh->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (rh->listen_fd == SOCKET_INVALID) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: socket() failed: %s", RH_ID, socket_strerror(socket_get_error())); return -1; }
    socket_set_reuseaddr(rh->listen_fd, 1);
    socket_set_nonblocking(rh->listen_fd);

    struct sockaddr_in sin; memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET; sin.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_ip, &sin.sin_addr) != 1) { socket_close_wrapper(rh->listen_fd); rh->listen_fd = SOCKET_INVALID; return -1; }
    if (bind(rh->listen_fd, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: bind(%s:%d) failed: %s", RH_ID, bind_ip, port, socket_strerror(socket_get_error()));
        socket_close_wrapper(rh->listen_fd); rh->listen_fd = SOCKET_INVALID; return -1;
    }
    if (listen(rh->listen_fd, 5) < 0) { socket_close_wrapper(rh->listen_fd); rh->listen_fd = SOCKET_INVALID; return -1; }
    uint16_t bound_port = socket_get_bound_port(rh->listen_fd);
    if (!bound_port) { socket_close_wrapper(rh->listen_fd); rh->listen_fd = SOCKET_INVALID; return -1; }

    rh->listen_sock_id = uasync_add_socket_t(ua, rh->listen_fd, rh_accept_callback, NULL, NULL, "radio_listen", rh);
    if (!rh->listen_sock_id) { socket_close_wrapper(rh->listen_fd); rh->listen_fd = SOCKET_INVALID; return -1; }

    radio_set_frame_cb(inst, rh_frame_cb, rh);
    radio_set_mute_cb(inst, rh_member_mute, rh);
    rh->cleanup_timer = uasync_set_timeout(ua, 5000, rh, rh_cleanup, "rh_cleanup");
    rh->mix_timer = uasync_set_timeout(ua, RH_MIX_TIMER_TB, rh, rh_mix_tick, "rh_mix");
    if (!rh->cleanup_timer || !rh->mix_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: startup timer scheduling failed cleanup=%p mix=%p", RH_ID, rh->cleanup_timer, rh->mix_timer);
        radio_headless_destroy(inst); return -1;
    }

    rh->running = 1;
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: audio socket listening on %s:%u", RH_ID, bind_ip, bound_port);
    return 0;
}

uint16_t radio_headless_get_port(struct UTUN_INSTANCE* inst) {
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: get port without instance", RH_ID); return 0; }
    struct radio_headless* rh = rh_of(inst);
    return rh && rh->running ? socket_get_bound_port(rh->listen_fd) : 0;
}

void radio_headless_destroy(struct UTUN_INSTANCE* inst) {
    struct radio_headless* rh = rh_of(inst);
    if (!rh) return;
    rh->running = 0;
    radio_set_frame_cb(inst, NULL, NULL);
    radio_set_mute_cb(inst, NULL, NULL);
    if (rh->mix_timer && rh->ua) { uasync_cancel_timeout(rh->ua, rh->mix_timer); rh->mix_timer = NULL; }
    if (rh->cleanup_timer && rh->ua) { uasync_cancel_timeout(rh->ua, rh->cleanup_timer); rh->cleanup_timer = NULL; }
    if (rh->listen_sock_id && rh->ua) uasync_remove_socket_t(rh->ua, rh->listen_fd);
    if (rh->listen_fd != SOCKET_INVALID) { socket_close_wrapper(rh->listen_fd); rh->listen_fd = SOCKET_INVALID; }
    if (rh->enc) { opus_codec_encoder_destroy(rh->enc); rh->enc = NULL; }
    for (int i = 0; i < RH_MAX_SOURCES; i++) if (rh->sources[i].used) rh_src_free(&rh->sources[i]);
    struct radio_headless_client* c = rh->clients;
    while (c) {
        struct radio_headless_client* n = c->next;
        rh_close_client(c);
        u_free(c);
        c = n;
    }
    rh->clients = NULL; rh->client_count = 0;
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: audio socket destroyed (enc=%u dec=%u enc_drop=%u dec_drop=%u)",
               RH_ID, rh->c_enc_frames, rh->c_dec_frames, rh->c_enc_dropped, rh->c_dec_dropped);
    inst->radio_headless = NULL; u_free(rh);
}
