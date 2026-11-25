// call_headless.c — headless-аудио-сокет для P2P-звонков (PCM, без тактирования)
//
// TCP-сокет с бинарным фреймингом [type:1][call_id:8][len:2][data:len].
// Клиент привязывается к звонку (HELLO) и обменивается ГОТОВЫМ PCM (int16 mono 48k):
//   клиент → ядро: PCM-кадр (960 сэмплов) — кодируется в Opus и уходит пиру;
//   ядро → клиент: Opus пира декодируется в PCM и отдаётся клиенту.
// Тактирование/джиттер — на стороне клиента (ядро лишь кодирует/декодирует).
// Сигналинг — на control-сокете (chat_headless_control), медиа — здесь.
#include "call_headless.h"
#include "call.h"
#include "../utun_instance.h"
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

#define CH_ID "call_audio"

#define AUDIO_RECV_BUF_SIZE 65536
#define AUDIO_SEND_BUF_SIZE 65536
#define AUDIO_MAX_CLIENTS   16
#define AUDIO_MAX_FRAME     2048

#define CA_SAMPLE_RATE       48000
#define CA_FRAME_SAMPLES     960                  /* 20мс @48k */
#define CA_PCM_FRAME_BYTES   (CA_FRAME_SAMPLES * (int)sizeof(int16_t))   /* 1920 */
#define CA_BITRATE           32000

struct call_audio_client {
    socket_t fd;
    void*    socket_id;
    uint64_t call_id;               /* 0 = не привязан */
    uint8_t  recv_buf[AUDIO_RECV_BUF_SIZE];
    int      recv_len;
    uint8_t  send_buf[AUDIO_SEND_BUF_SIZE];
    int      send_len;
    int      send_offset;
    uint32_t pcm_dropped;           /* Целые исходящие PCM-кадры, отброшенные при переполнении. */
    uint64_t drop_log_tb;           /* Ограничение частоты WARN: не чаще раза в секунду. */
    int      closing;
    struct call_audio* ca;
    struct call_audio_client* next;
};

struct call_audio {
    struct UASYNC*          ua;
    struct UTUN_INSTANCE*   inst;
    struct call_audio_client* clients;
    int                     client_count;
    socket_t                listen_fd;
    void*                   listen_sock_id;
    int                     running;
    void*                   cleanup_timer;
    /* Opus-кодеки (одна активная звонка за раз — busy в call.c) */
    uint64_t                enc_call_id;
    opus_codec_encoder_t*   enc;
    uint64_t                dec_call_id;
    opus_codec_decoder_t*   dec;
    /* диагностика */
    uint32_t                c_enc_frames;
    uint32_t                c_dec_frames;
    uint32_t                c_enc_dropped;
    uint32_t                c_dec_dropped;
    uint32_t                c_pcm_dropped;
};

static struct call_audio* ca_of(struct UTUN_INSTANCE* inst) {
    return inst ? (struct call_audio*)inst->call_audio : NULL;
}

static void ca_close_client(struct call_audio_client* cli);

/* ── отправка фрейма клиенту (с буферизацией как в control-сокете) ── */

/* Принимает целый ограниченный фрейм; short write сохраняет его остаток перед следующими кадрами. */
static void ca_send_raw(struct call_audio_client* cli, const uint8_t* data, size_t len) {
    if (!cli || cli->closing || cli->fd == SOCKET_INVALID || !data || len == 0) return;
    struct call_audio* ca = cli->ca;
    if (cli->send_len > 0) {
        size_t space = sizeof(cli->send_buf) - (size_t)cli->send_len;
        if (len > space) {
            ++cli->pcm_dropped; ++ca->c_pcm_dropped;
            uint64_t now = get_time_tb();
            if (cli->pcm_dropped == 1 || now - cli->drop_log_tb >= 10000) {
                cli->drop_log_tb = now;
                DEBUG_WARN(DEBUG_CATEGORY_CALL,
                    "%s: PCM frame dropped call=%016llx fd=%d frame=%zu queued=%d offset=%d capacity=%zu dropped=%u total=%u",
                    CH_ID, (unsigned long long)cli->call_id, (int)cli->fd, len, cli->send_len, cli->send_offset,
                    sizeof(cli->send_buf), cli->pcm_dropped, ca->c_pcm_dropped);
            }
            return;
        }
        memcpy(cli->send_buf + cli->send_len, data, len);
        cli->send_len += (int)len;
        return;
    }
    ssize_t r = send(cli->fd, data, (int)len, 0);
    if (r < 0 && (socket_get_error() == ERR_AGAIN || socket_get_error() == ERR_WOULDBLOCK)) {
        if (len <= sizeof(cli->send_buf)) { memcpy(cli->send_buf, data, len); cli->send_len = (int)len; cli->send_offset = 0; }
        if (ca && ca->ua) uasync_set_socket_write(ca->ua, cli->socket_id, 1);
        return;
    }
    if (r >= 0 && (size_t)r < len) {
        size_t remain = len - (size_t)r;
        if (remain <= sizeof(cli->send_buf)) { memcpy(cli->send_buf, data + r, remain); cli->send_len = (int)remain; cli->send_offset = 0; }
        if (ca && ca->ua) uasync_set_socket_write(ca->ua, cli->socket_id, 1);
    }
    if (r < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: send failed err=%d, closing client", CH_ID, socket_get_error());
        ca_close_client(cli);
    }
}

/* Собрать header+PCM до обращения к очереди, чтобы overflow не оставил часть фрейма. */
static void ca_write_frame(struct call_audio_client* cli, uint8_t type, uint64_t call_id,
                           const uint8_t* data, uint16_t len) {
    if (len > CA_PCM_FRAME_BYTES || (len && !data)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: invalid output frame call=%016llx type=%u len=%u data=%p",
                    CH_ID, (unsigned long long)call_id, type, len, (const void*)data);
        return;
    }
    uint8_t frame[CALL_AUDIO_HDR_SIZE + CA_PCM_FRAME_BYTES];
    frame[0] = type;
    memcpy(frame + 1, &call_id, 8);
    frame[9] = (uint8_t)(len >> 8);
    frame[10] = (uint8_t)(len & 0xFF);
    if (len) memcpy(frame + CALL_AUDIO_HDR_SIZE, data, len);
    ca_send_raw(cli, frame, CALL_AUDIO_HDR_SIZE + len);
}

/* ── Opus-кодеки (одна активная звонка за раз) ── */

static opus_codec_encoder_t* ca_encoder(struct call_audio* ca, uint64_t call_id) {
    if (ca->enc && ca->enc_call_id == call_id) return ca->enc;
    if (ca->enc) { opus_codec_encoder_destroy(ca->enc); ca->enc = NULL; }
    ca->enc = opus_codec_encoder_create(CA_SAMPLE_RATE, 1);
    if (ca->enc) {
        opus_codec_encoder_bitrate_set(ca->enc, CA_BITRATE);
        ca->enc_call_id = call_id;
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: encoder created for call_id=%016llx", CH_ID, (unsigned long long)call_id);
    } else {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: encoder create failed", CH_ID);
    }
    return ca->enc;
}

static opus_codec_decoder_t* ca_decoder(struct call_audio* ca, uint64_t call_id) {
    if (ca->dec && ca->dec_call_id == call_id) return ca->dec;
    if (ca->dec) { opus_codec_decoder_destroy(ca->dec); ca->dec = NULL; }
    ca->dec = opus_codec_decoder_create(CA_SAMPLE_RATE, 1);
    if (ca->dec) {
        ca->dec_call_id = call_id;
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: decoder created for call_id=%016llx", CH_ID, (unsigned long long)call_id);
    } else {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: decoder create failed", CH_ID);
    }
    return ca->dec;
}

/* ── приём медиа от пира → PCM клиенту ── */

static void ca_media_recv(struct UTUN_INSTANCE* inst, uint64_t call_id,
                          const uint8_t* opus, int len, void* arg) {
    struct call_audio* ca = (struct call_audio*)arg;
    (void)inst;
    if (!ca || !opus || len <= 0) return;

    opus_codec_decoder_t* dec = ca_decoder(ca, call_id);
    if (!dec) return;

    int16_t pcm[CA_FRAME_SAMPLES];
    int n = opus_codec_decode(dec, opus, len, pcm, CA_FRAME_SAMPLES);
    if (n <= 0) {
        ca->c_dec_dropped++;
        return;
    }
    ca->c_dec_frames++;

    for (struct call_audio_client* c = ca->clients; c; c = c->next) {
        if (c->call_id == call_id && !c->closing) {
            ca_write_frame(c, CALL_AUDIO_FRAME, call_id, (const uint8_t*)pcm, (uint16_t)(n * (int)sizeof(int16_t)));
            break;
        }
    }
}

/* ── разбор входящих фреймов ── */

static void ca_parse_frames(struct call_audio_client* cli) {
    struct call_audio* ca = cli->ca;
    uint8_t* p = cli->recv_buf;
    int avail = cli->recv_len;
    int consumed = 0;
    while (avail - consumed >= CALL_AUDIO_HDR_SIZE) {
        uint8_t type = p[consumed];
        uint64_t call_id; memcpy(&call_id, p + consumed + 1, 8);
        uint16_t flen = (uint16_t)((p[consumed + 9] << 8) | p[consumed + 10]);
        if (flen > AUDIO_MAX_FRAME) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: frame too big %u — closing", CH_ID, flen);
            ca_close_client(cli);
            return;
        }
        if (avail - consumed < (int)(CALL_AUDIO_HDR_SIZE + flen)) break; /* нужен ещё кусок */
        const uint8_t* data = p + consumed + CALL_AUDIO_HDR_SIZE;
        if (type == CALL_AUDIO_HELLO) {
            cli->call_id = call_id;
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: audio client bound to call_id=%016llx", CH_ID, (unsigned long long)call_id);
        } else if (type == CALL_AUDIO_FRAME) {
            if (flen != CA_PCM_FRAME_BYTES) {
                ca->c_enc_dropped++;
                DEBUG_WARN(DEBUG_CATEGORY_CALL, "%s: bad PCM frame size=%u (expect %d) — dropped",
                           CH_ID, flen, CA_PCM_FRAME_BYTES);
            } else {
                opus_codec_encoder_t* enc = ca_encoder(ca, call_id);
                int16_t pcm[CA_FRAME_SAMPLES]; memcpy(pcm, data, sizeof(pcm)); // wire header 11 байт не выровнен для int16_t
                uint8_t opus[256];
                int olen = enc ? opus_codec_encode(enc, pcm, CA_FRAME_SAMPLES, opus, (int)sizeof(opus)) : -1;
                if (olen <= 0) {
                    ca->c_enc_dropped++;
                    DEBUG_WARN(DEBUG_CATEGORY_CALL, "%s: encode failed (olen=%d) call_id=%016llx",
                               CH_ID, olen, (unsigned long long)call_id);
                } else {
                    ca->c_enc_frames++;
                    if (call_send_media(ca->inst, call_id, opus, olen, NULL) != 0)
                        DEBUG_WARN(DEBUG_CATEGORY_CALL, "%s: send_media failed call_id=%016llx len=%d",
                                   CH_ID, (unsigned long long)call_id, olen);
                }
            }
        } else {
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "%s: unknown frame type=0x%02x — closing", CH_ID, type);
            ca_close_client(cli);
            return;
        }
        consumed += CALL_AUDIO_HDR_SIZE + flen;
    }
    if (consumed > 0) {
        memmove(cli->recv_buf, cli->recv_buf + consumed, (size_t)(cli->recv_len - consumed));
        cli->recv_len -= consumed;
    }
}

/* ── socket callbacks ── */

static void ca_write_callback(socket_t fd, void* arg) {
    struct call_audio_client* cli = (struct call_audio_client*)arg;
    struct call_audio* ca = cli ? cli->ca : NULL;
    if (!cli || cli->closing || fd != cli->fd) return;
    if (cli->send_len <= 0) { if (ca && ca->ua) uasync_set_socket_write(ca->ua, cli->socket_id, 0); return; }
    ssize_t r = send(cli->fd, cli->send_buf + cli->send_offset, cli->send_len - cli->send_offset, 0);
    if (r < 0 && (socket_get_error() == ERR_AGAIN || socket_get_error() == ERR_WOULDBLOCK)) return;
    if (r <= 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: send failed (r=%zd err=%d) — closing client", CH_ID, r, socket_get_error());
        ca_close_client(cli);
        return;
    }
    cli->send_offset += (int)r;
    if (cli->send_offset >= cli->send_len) { cli->send_len = 0; cli->send_offset = 0; if (ca && ca->ua) uasync_set_socket_write(ca->ua, cli->socket_id, 0); }
}

static void ca_close_client(struct call_audio_client* cli) {
    struct call_audio* ca = cli ? cli->ca : NULL;
    if (!cli || cli->closing) return;
    cli->closing = 1;
    if (ca && ca->ua && cli->socket_id) uasync_remove_socket_t(ca->ua, cli->fd);
    if (cli->fd != SOCKET_INVALID) { socket_close_wrapper(cli->fd); cli->fd = SOCKET_INVALID; }
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: audio client closed call_id=%016llx pcm_drop=%u",
               CH_ID, (unsigned long long)cli->call_id, cli->pcm_dropped);
}

static void ca_read_callback(socket_t fd, void* arg) {
    struct call_audio_client* cli = (struct call_audio_client*)arg;
    if (!cli || cli->closing) return;
    uint8_t tmp[4096];
    ssize_t r = recv(fd, tmp, (int)sizeof(tmp), 0);
    if (r <= 0) { ca_close_client(cli); return; }
    if (cli->recv_len + (int)r > AUDIO_RECV_BUF_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: recv buffer overflow — closing", CH_ID);
        ca_close_client(cli);
        return;
    }
    memcpy(cli->recv_buf + cli->recv_len, tmp, (size_t)r);
    cli->recv_len += (int)r;
    ca_parse_frames(cli);
}

static void ca_accept_callback(socket_t fd, void* arg) {
    struct call_audio* ca = (struct call_audio*)arg;
    struct sockaddr_storage addr; socklen_t alen = sizeof(addr);
    socket_t cfd = accept(fd, (struct sockaddr*)&addr, &alen);
    if (cfd == SOCKET_INVALID) {
        int e = socket_get_error();
        if (e != ERR_AGAIN && e != ERR_WOULDBLOCK) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: accept failed: %s", CH_ID, socket_strerror(e));
        return;
    }
    socket_set_nonblocking(cfd);
    if (ca->client_count >= AUDIO_MAX_CLIENTS) { socket_close_wrapper(cfd); DEBUG_WARN(DEBUG_CATEGORY_CALL, "%s: max clients reached", CH_ID); return; }

    struct call_audio_client* cli = u_calloc(1, sizeof(*cli));
    if (!cli) { socket_close_wrapper(cfd); return; }
    cli->fd = cfd;
    cli->ca = ca;
    cli->socket_id = uasync_add_socket_t(ca->ua, cfd, ca_read_callback, ca_write_callback, NULL, "call_client", cli);
    if (!cli->socket_id) { u_free(cli); socket_close_wrapper(cfd); DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: failed to register client fd", CH_ID); return; }

    cli->next = ca->clients; ca->clients = cli; ca->client_count++;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: audio client connected fd=%d total=%d", CH_ID, (int)cfd, ca->client_count);
}

/* ── периодическая очистка закрытых клиентов ── */

static void ca_cleanup(void* arg) {
    struct call_audio* ca = (struct call_audio*)arg;
    ca->cleanup_timer = NULL;
    if (!ca->running) return;
    struct call_audio_client** prev = &ca->clients;
    while (*prev) {
        if ((*prev)->closing) {
            struct call_audio_client* dead = *prev; *prev = dead->next;
            if (dead->fd != SOCKET_INVALID) socket_close_wrapper(dead->fd);
            u_free(dead); ca->client_count--;
        } else prev = &(*prev)->next;
    }
    ca->cleanup_timer = uasync_set_timeout(ca->ua, 5000, ca, ca_cleanup, "ca_cleanup");
    if (!ca->cleanup_timer) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: cleanup timer scheduling failed", CH_ID);
}

/* ── init / destroy ── */

int call_headless_init(struct UASYNC* ua, struct UTUN_INSTANCE* inst,
                       const char* bind_ip, int port) {
    if (!ua || !inst || !bind_ip || port < 0 || port > 65535) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: invalid init arguments port=%d", CH_ID, port); return -1;
    }
    struct call_audio* ca = ca_of(inst);
    if (ca && ca->running) return 0;

    if (!ca) {
        ca = u_calloc(1, sizeof(*ca));
        if (!ca) return -1;
        inst->call_audio = ca;
    }
    ca->ua = ua; ca->inst = inst;

    ca->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ca->listen_fd == SOCKET_INVALID) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: socket() failed: %s", CH_ID, socket_strerror(socket_get_error())); return -1; }
    socket_set_reuseaddr(ca->listen_fd, 1);
    socket_set_nonblocking(ca->listen_fd);

    struct sockaddr_in sin; memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET; sin.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_ip, &sin.sin_addr) != 1) { socket_close_wrapper(ca->listen_fd); ca->listen_fd = SOCKET_INVALID; return -1; }
    if (bind(ca->listen_fd, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: bind(%s:%d) failed: %s", CH_ID, bind_ip, port, socket_strerror(socket_get_error()));
        socket_close_wrapper(ca->listen_fd); ca->listen_fd = SOCKET_INVALID; return -1;
    }
    if (listen(ca->listen_fd, 5) < 0) { socket_close_wrapper(ca->listen_fd); ca->listen_fd = SOCKET_INVALID; return -1; }
    uint16_t bound_port = socket_get_bound_port(ca->listen_fd);
    if (!bound_port) { socket_close_wrapper(ca->listen_fd); ca->listen_fd = SOCKET_INVALID; return -1; }

    ca->listen_sock_id = uasync_add_socket_t(ua, ca->listen_fd, ca_accept_callback, NULL, NULL, "call_listen", ca);
    if (!ca->listen_sock_id) { socket_close_wrapper(ca->listen_fd); ca->listen_fd = SOCKET_INVALID; return -1; }

    call_set_media_recv_cb(inst, ca_media_recv, ca);
    ca->cleanup_timer = uasync_set_timeout(ua, 5000, ca, ca_cleanup, "ca_cleanup");
    if (!ca->cleanup_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: cleanup timer scheduling failed", CH_ID);
        call_headless_destroy(inst); return -1;
    }

    ca->running = 1;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: audio socket listening on %s:%u", CH_ID, bind_ip, bound_port);
    return 0;
}

uint16_t call_headless_get_port(struct UTUN_INSTANCE* inst) {
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: get port without instance", CH_ID); return 0; }
    struct call_audio* ca = ca_of(inst);
    return ca && ca->running ? socket_get_bound_port(ca->listen_fd) : 0;
}

void call_headless_destroy(struct UTUN_INSTANCE* inst) {
    struct call_audio* ca = ca_of(inst);
    if (!ca) return;
    ca->running = 0;
    if (ca->cleanup_timer) { uasync_cancel_timeout(ca->ua, ca->cleanup_timer); ca->cleanup_timer = NULL; }
    call_set_media_recv_cb(inst, NULL, NULL);
    if (ca->listen_sock_id && ca->ua) uasync_remove_socket_t(ca->ua, ca->listen_fd);
    if (ca->listen_fd != SOCKET_INVALID) { socket_close_wrapper(ca->listen_fd); ca->listen_fd = SOCKET_INVALID; }
    if (ca->enc) { opus_codec_encoder_destroy(ca->enc); ca->enc = NULL; }
    if (ca->dec) { opus_codec_decoder_destroy(ca->dec); ca->dec = NULL; }
    struct call_audio_client* c = ca->clients;
    while (c) {
        struct call_audio_client* n = c->next;
        ca_close_client(c);
        u_free(c);
        c = n;
    }
    ca->clients = NULL; ca->client_count = 0;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: audio socket destroyed (enc=%u dec=%u enc_drop=%u dec_drop=%u pcm_drop=%u)",
               CH_ID, ca->c_enc_frames, ca->c_dec_frames, ca->c_enc_dropped, ca->c_dec_dropped, ca->c_pcm_dropped);
    inst->call_audio = NULL; u_free(ca);
}
