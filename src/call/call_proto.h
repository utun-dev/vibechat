// call_proto.h — wire-формат сигналинга (ETCP_RT_ID_CALL 0x36) и аудио (ETCP_RT_ID_CALL_AUDIO 0x3F).
// CALL_MEDIA несёт только CALL_TRACK_AUDIO; видео использует call_video.h и отдельные каналы
// ETCP_RT_ID_CALL_VIDEO_CONTROL 0x3D / ETCP_RT_ID_CALL_VIDEO 0x3E (call_video_transport.h).
//
// Приём/отправка через etcp_router. На проводе первый байт payload = subcmd.
// Сигналинг и медиа требуют ROUTE_CRYPTO_SIGN|ROUTE_CRYPTO_ENCRYPT: шифрование тела
// и подпись заголовка/шифртекста между конечными узлами, независимо от маршрута.
// Структуры упакованы без padding; целые поля копируются в порядке байтов хоста, без htons/ntohs.
// Здесь фиксирован один аудиотрек, согласования аудиоформата нет: отправитель объявляет Opus48k mono/20мс/32кбит/с.
// call.c проверяет размер/число треков, но не настраивает decoder по полям descriptor; кодеки создаёт frontend.
#ifndef CALL_PROTO_H
#define CALL_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── подкоманды (первый байт payload после svc_id) ── */
enum {
    CALL_SUBCMD_INVITE  = 0x01,  /* call_pkt_invite: call_id, num_tracks, force, expires_us, один track */
    CALL_SUBCMD_RINGING = 0x02,  /* {call_id:8} */
    CALL_SUBCMD_ACCEPT  = 0x03,  /* {call_id:8} */
    CALL_SUBCMD_DECLINE = 0x04,  /* {call_id:8, reason:1} */
    CALL_SUBCMD_BUSY    = 0x05,  /* {call_id:8} */
    CALL_SUBCMD_CANCEL  = 0x06,  /* {call_id:8} */
    CALL_SUBCMD_HANGUP  = 0x07,  /* {call_id:8, reason:1} */
    CALL_SUBCMD_END_ACK = 0x08,  /* {call_id:8}: подтверждение HANGUP/CANCEL/DECLINE */
    CALL_SUBCMD_SELECT = 0x0a, /* {call_id:8}: инициатор выбрал адресата */
    CALL_SUBCMD_SELECTED = 0x0b, /* {call_id:8}: выбранный адресат подтвердил подключение */
    CALL_SUBCMD_PHONE_BUSY = 0x09, /* {call_id:8}: ожидание решения, не завершение */
    CALL_SUBCMD_MEDIA   = 0x10,  /* call_pkt_media + payload[] */
};

/* Типы треков; в этом протоколе INVITE/MEDIA отправляют только AUDIO. Видео согласуется отдельно. */
enum { CALL_TRACK_AUDIO = 0x01, CALL_TRACK_VIDEO = 0x02 };
enum { CALL_CODEC_OPUS = 0x01 };

/* ── причины завершения (ENDED) ── */
enum {
    CALL_REASON_LOCAL_HANGUP  = 0,
    CALL_REASON_REMOTE_HANGUP = 1,
    CALL_REASON_DECLINE       = 2,
    CALL_REASON_BUSY          = 3,
    CALL_REASON_CANCEL        = 4,
    CALL_REASON_NO_TRAFFIC    = 5,
    CALL_REASON_RING_TIMEOUT  = 6,
    CALL_REASON_CONNECT_FAIL  = 7,
    CALL_REASON_POLICY        = 9, /* запрет настройкой устройства, не ручной отказ всей группы */
    CALL_REASON_PHONE_BUSY    = 8, /* автоматический отказ: абонент разговаривает по телефону */
};

#pragma pack(push, 1)

struct call_pkt_track {
    uint8_t  type;        /* Отправитель ставит CALL_TRACK_AUDIO. */
    uint8_t  codec;       /* CALL_CODEC_OPUS. */
    uint32_t sample_rate; /* 48000 Гц. */
    uint8_t  channels;    /* 1, mono. */
    uint32_t bitrate;     /* 32000 бит/с; информационное поле, не настройка удалённого encoder. */
};

struct call_pkt_invite {
    uint8_t  subcmd;      /* CALL_SUBCMD_INVITE */
    uint64_t call_id;
    uint8_t  num_tracks;  /* Ровно1; размер пакета = sizeof(call_pkt_invite), 38 байт. */
    uint8_t  force;       /* 1 = явно подтверждённое продолжение при телефонной занятости */
    int64_t  expires_us;  /* Unix us с NTP-поправкой: отправка now+2с, приём требует now<expires<=now+4с. */
    struct call_pkt_track tracks[1];  /* Один descriptor (11 байт), без списка произвольной длины. */
};

struct call_pkt_ringing { uint8_t subcmd; uint64_t call_id; };
struct call_pkt_accept  { uint8_t subcmd; uint64_t call_id; };
struct call_pkt_decline { uint8_t subcmd; uint64_t call_id; uint8_t reason; };
struct call_pkt_busy    { uint8_t subcmd; uint64_t call_id; };
struct call_pkt_cancel  { uint8_t subcmd; uint64_t call_id; };
struct call_pkt_hangup  { uint8_t subcmd; uint64_t call_id; uint8_t reason; };
struct call_pkt_end_ack { uint8_t subcmd; uint64_t call_id; };

/* Одна Router-кодограмма содержит 1..10 Opus-кадров одной capture epoch.
 * После header: повторяются [call_pkt_audio_frame][opus length байт]. Без старого формата. */
struct call_pkt_media {
    uint8_t subcmd;          /* CALL_SUBCMD_MEDIA */
    uint64_t call_id;        /* ACTIVE-звонок; peer/group проверяет call.c. */
    uint8_t track_id;        /* CALL_TRACK_AUDIO */
    uint64_t capture_epoch;  /* Общая эпоха источника; с известным PTS ненулевая. */
    uint32_t codec_delay_samples; /* Opus lookahead при 48кГц. */
    uint32_t sent_ms;        /* Монотонное время подачи пачки в Router; одно измерение jitter на пачку. */
    uint8_t count;           /* 1..CALL_AUDIO_BATCH_MAX */
};
struct call_pkt_audio_frame {
    uint16_t seq;            /* Исходный номер кадра, допускает пропуски/переполнение. */
    int64_t capture_pts_us;  /* PTS первого сэмпла; INT64_MIN при отсутствии. */
    uint16_t length;         /* Непустой Opus, полностью помещается в кодограмме. */
};

#pragma pack(pop)

#define CALL_MEDIA_HDR_SIZE sizeof(struct call_pkt_media)   /* 27 */
#define CALL_AUDIO_FRAME_HDR_SIZE sizeof(struct call_pkt_audio_frame) /* 12 */
#define CALL_AUDIO_BATCH_MAX 10u
#define CALL_MEDIA_MAX_BYTES 1024u
#define CALL_AUDIO_MAX_OPUS (CALL_MEDIA_MAX_BYTES - CALL_MEDIA_HDR_SIZE - CALL_AUDIO_FRAME_HDR_SIZE)

/* ── параметры аудио (Opus 48kHz mono, 20ms) ── */
#define CALL_OPUS_SAMPLE_RATE 48000
#define CALL_OPUS_FRAME_MS    20
#define CALL_OPUS_FRAME_SAMPLES (CALL_OPUS_SAMPLE_RATE * CALL_OPUS_FRAME_MS / 1000)  /* 960 */

/* ── таймауты и ёмкости (timebase 0.1ms) ── */
#define CALL_RING_TIMEOUT_TB  600000  /* 60с от начала, включая ожидание решения */
#define CALL_INVITE_RETRY_TB  3000    /* 300мс — кодограмма INVITE (OUTGOING/RINGING/WAIT_DECISION) */
#define CALL_RING_DEAD_TB     20000   /* 2с — вызываемый: нет кодограммы → вызов сорван */
#define CALL_NO_ROUTE_MAX_STRIKES 20  /* 20 × 300мс ≈ 6с без маршрута → fail-fast CONNECT_FAIL */
#define CALL_NO_TRAFFIC_TB    200000  /* 20с — нет входящих медиа → HANGUP */
#define CALL_TEARDOWN_DELAY_TB 5000   /* Не используется; без своей end-команды CM/роль callee удерживают запись2.1с. */
#define CALL_END_RETRY_TB      3000   /* 300мс — повтор финальной команды до END_ACK */
#define CALL_END_TIMEOUT_TB  200000   /* 20с — предел удержания завершённой сессии без END_ACK */
#define CALL_MAX_TX_FRAMES    750     /* 15с: 10с перебоя + 5с запаса */

#ifdef __cplusplus
}
#endif
#endif /* CALL_PROTO_H */
