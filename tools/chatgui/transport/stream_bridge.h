/* Desktop live media: attach/detach в uasync до destroy ядра; остальные вызовы потокобезопасны.
 * Один общий publisher и независимые viewers; команды содержат IDs, не borrowed pointers ядра.
 * Плеер запрашивает ограниченный батч; каждый трек имеет свой бюджет, новые данные будят worker.
 * Watch до attach запоминает выбранную группу. Каталог идёт через CHAT_EVT_STREAM_SOURCES,
 * media pull читает ближайшие кадры из общего RS-кольца; QByteArray владеет копией payload.
 * notify вызывается под внутренними mutex: только будить worker, не входить повторно в bridge/API ядра. */
#pragma once

#include <QByteArray>
#include <QString>
#include <cstdint>
#include <deque>
#include <vector>
#include "../../../src/stream/stream_stats.h"
#include "../../../src/stream/stream_media.h"
#include "../../../src/stream/stream_media_buffer.h"
#include "../../../src/stream/stream_publisher.h"
#include "../../../src/stream/stream_viewer.h"

struct UTUN_INSTANCE;
enum class StreamRecordKind { Video, Audio, Gap, Opened, End, Closed, VideoEnd, AudioEnd };
struct StreamRecord {
    uint64_t id = 0, pts = 0; // ID viewer и PTS этого источника в микросекундах; PTS значим для media.
    StreamRecordKind kind = StreamRecordKind::Gap; // Media либо событие открытия/разрыва/завершения трека.
    stream_media_format format{}; // Формат VIDEO/AUDIO; для управляющих событий нули.
    int flags = 0, reason = 0; // VIDEO: STREAM_MEDIA_KEY; CLOSED: stream_close_reason.
    QByteArray data; // Owned access unit/Opus; у управляющих событий пусто, живёт независимо от viewer/ядра.
};

/* Создать общий publisher/viewers для inst с работающим stream-сервисом; uasync, вне callbacks.
 * true — managers готовы; false — неверный inst, повторный attach либо отказ создания, с логом нижнего слоя.
 * Сохранённые watches применяются к новому ядру; отдельный отказ watch логируется и не отменяет attach. */
bool gui_stream_attach(UTUN_INSTANCE* inst);
/* Только совпавший inst: отменить pending watch, снять watches, уничтожить managers; NULL/старое ядро — no-op.
 * uasync вне callbacks до destroy inst; mutex ждёт внешних callers. Выбор watches сохраняется для нового attach. */
void gui_stream_detach(UTUN_INSTANCE* inst);
/* owner=0 — выбранный чат; окно многопросмотра держит свой ненулевой owner. пустая группа снимает только его.
 * Последнее значение каждого owner сохраняется до attach/через detach; работающий bridge объединяет изменения в post.
 * true — выбор принят, не подтверждение доступности каталога. false — не выделен post, прежний выбор не меняется. */
bool gui_stream_watch(const QString& group, uint64_t owner = 0);
/* Ненулевые id/group/source/epoch; true — асинхронное открытие VIDEO|AUDIO принято, итог через Opened/Closed.
 * id уникален до обработки stop actor. notify может вызываться сразу; arg живёт до gui_stream_stop/detach.
 * Callback только будит worker, не вызывает bridge/API ядра. false — отказ открытия/установки notify. */
bool gui_stream_open(uint64_t id, const QString& group, const uint8_t source[32], uint64_t epoch,
                     void (*notify)(void*) = nullptr, void* arg = nullptr);
/* Синхронно разбудить/снять notify и асинхронно закрыть viewer; старый id допустим, полученные records сохраняются. */
void gui_stream_stop(uint64_t id);
/* true — одна готовая запись записана в record. false — record не меняется; при отсутствии готового ответа запрашивается выдача.
 * position<0 прогревает звук до KEY; иначе позиция playback в мкс. audioAhead=-1 отключает звук, иначе 0..60000000 мкс.
 * Это режим выдачи по одной записи: не смешивать с pull_batch на том же id без сброса reader. */
bool gui_stream_pull(uint64_t id, StreamRecord& record, int64_t position = -1,
                      bool wantVideo = true, int64_t audioAhead = STREAM_MEDIA_READ_AHEAD_US);
/* true — готовый батч добавлен в records, прежние записи не удаляются. false — новых записей пока нет либо viewer недоступен.
 * При пустом ответе запрашивает до STREAM_VIEWER_BATCH_RECORDS записей: videoBudget=0..16 кадров и audioAhead звука.
 * position/audioAhead как у pull. Вызывать после расходования нужного трека; удерживаемое видео не мешает запросить только звук.
 * Запрос исполняется асинхронно, false не отличает ожидание от отказа demand; ошибки scheduling логируются ядром. */
bool gui_stream_pull_batch(uint64_t id, std::deque<StreamRecord>& records, int64_t position,
                            unsigned videoBudget, int64_t audioAhead);
/* Своя ссылка на счётчик аудио viewer, NULL без открытого аудиокурсора. */
stream_audio_meter* gui_stream_audio_meter(uint64_t id);
/* true — последний снимок viewer, сохраняется после End/Closed до stop; до обновления actor/после resync — нули.
 * false — ID/manager недоступен, выход не меняется. */
bool gui_stream_status(uint64_t id, stream_media_status& status);
/* true — снимок цепочки; раз в 200мс запрашивает обновление в uasync без ожидания его результата.
 * До первого снимка нули; false не меняет telemetry. */
bool gui_stream_telemetry(uint64_t id, stream_viewer_telemetry& telemetry);
/* Очистить готовый ответ и запросить поиск ближайшего KEY только этого viewer; true — принято. */
bool gui_stream_resync(uint64_t id);
/* Есть прикреплённое ядро/managers; не означает открытие viewer/publisher или готовность сетевых маршрутов. */
bool gui_stream_ready();
/* Единственный publisher независим от viewers. Ненулевые id/group и допустимый format; true — принято OPENING.
 * После Ended/Failed возможен новый id, результат открытия читается publish_status. */
bool gui_stream_publish_open(uint64_t id, const QString& group, const stream_media_format& format);
/* true — состояние audio принято в OPENING/LIVE без смены эпохи/аудиокодека; включение требует audio codec в format.
 * Команда публикует состояние, producer отдельно заглушает свой PCM при mute. */
bool gui_stream_publish_audio(uint64_t id, bool enabled);
/* Настроить Auto/ручной bitrate и получить текущий target ядра, бит/с; -1 при закрытом id/ошибке. */
int gui_stream_publish_video_rate(uint64_t id, bool automatic, int nominal, int minimum);
/* 1 — data скопирован в очередь, 0 — нужен KEY/перегрузка, -1 — ошибка/старый id; приём не гарантирует доставку.
 * pts<=INT64_MAX строго растёт отдельно по трекам; video требует полный access unit и format, audio — один Opus.
 * Новый video revision принимается с KEY; format/key для audio игнорируются. data/format заимствованы только на вызов. */
int gui_stream_publish_feed(uint64_t id, bool video, uint64_t pts, bool key, const QByteArray& data,
                            const stream_media_format* format = nullptr);
/* Запретить feed, дренировать очередь с TTL и удержать FIN 2с; в OPENING отменить открытие. Повтор/старый id допустимы. */
void gui_stream_publish_stop(uint64_t id);
/* true — последний снимок, включая ENDED/FAILED; false — id/manager недоступен, status не меняется. */
bool gui_stream_publish_status(uint64_t id, stream_publisher_status& status);

/* Owned снимок истории автора; обновление асинхронно, now — часы ядра снимка. false: ядро не готово. */
bool gui_stream_statistics(const QString& group, uint64_t epoch, std::vector<stream_stat_node>& nodes, uint64_t& now);
