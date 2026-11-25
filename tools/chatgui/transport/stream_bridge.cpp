#include "stream_bridge.h"
#include <mutex>
#include <map>
#include <set>

extern "C" {
#include "../../../lib/debug_config.h"
#include "../../../lib/mem.h"
#include "../../../lib/u_async.h"
#include "../../../src/utun_instance.h"
#include "../../../src/stream/stream.h"
#include "../../../src/stream/stream_viewer.h"
}

namespace {
struct Bridge {
    std::mutex lock;                  // Ограничивает lifetime managers для callers GUI/workers.
    UTUN_INSTANCE* inst = nullptr;
    posted_task* pending = nullptr;
    posted_task* statisticsPending = nullptr;
    QString statisticsGroup;
    uint64_t statisticsEpoch = 0, statisticsNow = 0;
    std::vector<stream_stat_node> statistics;

    std::map<uint64_t, QString> watches;
    std::set<uint64_t> watchChanges;
    stream_viewers* viewers = nullptr;
    stream_publisher* publisher = nullptr;
} bridge;

/* Забираем payload из owned записи ядра; next остаётся у обходящего батч caller. */
StreamRecord takeRecord(uint64_t id, stream_viewer_record* r) {
    StreamRecord record;
    switch (r->kind) {
    case STREAM_VIEWER_VIDEO: record.kind = StreamRecordKind::Video; break;
    case STREAM_VIEWER_AUDIO: record.kind = StreamRecordKind::Audio; break;
    case STREAM_VIEWER_GAP: record.kind = StreamRecordKind::Gap; break;
    case STREAM_VIEWER_OPENED: record.kind = StreamRecordKind::Opened; break;
    case STREAM_VIEWER_END: record.kind = StreamRecordKind::End; break;
    case STREAM_VIEWER_CLOSED: record.kind = StreamRecordKind::Closed; break;
    case STREAM_VIEWER_VIDEO_END: record.kind = StreamRecordKind::VideoEnd; break;
    case STREAM_VIEWER_AUDIO_END: record.kind = StreamRecordKind::AudioEnd; break;
    }
    record.id = id; record.pts = r->pts; record.format = r->format; record.flags = r->flags; record.reason = r->reason;
    record.data = QByteArray(reinterpret_cast<const char*>(r->data), static_cast<int>(r->length));
    u_free(r); return record;
}

stream_address groupAddress(const QString& group) {
    const auto text = group.toLatin1();
    const auto key = QByteArray::fromHex(text);
    return key.size() == 32 && key.toHex() == text ? stream_address_read(reinterpret_cast<const uint8_t*>(key.constData())) : stream_address{};
}
void watchGroup(UTUN_INSTANCE* inst, const QString& group, uint64_t owner) {
    const auto route = stream_group_route(inst, groupAddress(group));
    if ((!group.isEmpty() && !route) || stream_watch_sources(inst, route, owner) != STREAM_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop source watch failed group=%s owner=%llu", qPrintable(group), (unsigned long long)owner);
        stream_watch_sources(inst, 0, owner);
    }
}
void statistics(void*) {
    std::lock_guard<std::mutex> guard(bridge.lock);
    bridge.statisticsPending = nullptr;
    stream_stat_node* nodes = nullptr; size_t count = 0;
    if (stream_stats_read(bridge.inst, stream_group_route(bridge.inst, groupAddress(bridge.statisticsGroup)), bridge.statisticsEpoch, &nodes, &count) == STREAM_OK) {
        bridge.statistics.clear();
        if (count) bridge.statistics.assign(nodes, nodes + count);
        bridge.statisticsNow = count ? nodes[0].now_ms : get_time_tb() / 10;
    }
    u_free(nodes);
}
void watch(void*) {
    std::lock_guard<std::mutex> guard(bridge.lock);
    bridge.pending = nullptr;
    for (auto owner : bridge.watchChanges) {
        const auto& group = bridge.watches.at(owner); watchGroup(bridge.inst, group, owner);
        if (group.isEmpty()) bridge.watches.erase(owner);
    }
    bridge.watchChanges.clear();
}
}

bool gui_stream_attach(UTUN_INSTANCE* inst) {
    std::lock_guard<std::mutex> guard(bridge.lock);
    if (!inst || !inst->stream || bridge.inst) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "desktop stream attach rejected"); return false;
    }
    if (stream_publisher_create(&bridge.publisher, inst) < 0) return false;
    if (stream_viewers_create(&bridge.viewers, inst) < 0) {
        stream_publisher_destroy(bridge.publisher); bridge.publisher = nullptr; return false;
    }
    bridge.inst = inst;
    for (const auto& [owner, group] : bridge.watches) watchGroup(inst, group, owner);
    bridge.watchChanges.clear();
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop stream bridge attached"); return true;
}

void gui_stream_detach(UTUN_INSTANCE* inst) {
    std::lock_guard<std::mutex> guard(bridge.lock);
    if (!inst || bridge.inst != inst) return;
    if (bridge.statisticsPending) uasync_cancel_post(inst->ua, bridge.statisticsPending);
    bridge.statisticsPending = nullptr; bridge.statistics.clear(); bridge.statisticsGroup.clear(); bridge.statisticsEpoch = 0;
    if (bridge.pending) uasync_cancel_post(inst->ua, bridge.pending);
    for (const auto& [owner, group] : bridge.watches) stream_watch_sources(inst, 0, owner);
    bridge.pending = nullptr;
    stream_viewers_destroy(bridge.viewers); bridge.viewers = nullptr;
    stream_publisher_destroy(bridge.publisher); bridge.publisher = nullptr;
    bridge.inst = nullptr; bridge.watchChanges.clear();
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop stream bridge detached");
}

bool gui_stream_watch(const QString& group, uint64_t owner) {
    std::lock_guard<std::mutex> guard(bridge.lock);
    if (bridge.inst && !bridge.pending) {
        auto* task = static_cast<posted_task*>(u_calloc(1, sizeof(posted_task)));
        if (!task) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "desktop watch task allocation failed"); return false; }
        task->callback = watch; bridge.pending = task;
        uasync_post_reserved(bridge.inst->ua, task);
    }
    bridge.watches[owner] = group; bridge.watchChanges.insert(owner); return true;
}

bool gui_stream_open(uint64_t id, const QString& group, const uint8_t source[32], uint64_t epoch, void (*notify)(void*), void* arg) {
    std::lock_guard<std::mutex> guard(bridge.lock);
    if (stream_viewer_open(bridge.viewers, id, groupAddress(group), stream_address_read(source), epoch, STREAM_MEDIA_TRACK_VIDEO | STREAM_MEDIA_TRACK_AUDIO) < 0) return false;
    if (notify && stream_viewer_set_notify(bridge.viewers, id, notify, arg) < 0) {
        stream_viewer_stop(bridge.viewers, id); return false;
    }
    return true;
}

void gui_stream_stop(uint64_t id) {
    std::lock_guard<std::mutex> guard(bridge.lock); stream_viewer_stop(bridge.viewers, id);
}

bool gui_stream_pull(uint64_t id, StreamRecord& record, int64_t position, bool wantVideo, int64_t audioAhead) {
    std::lock_guard<std::mutex> guard(bridge.lock);
    auto* r = stream_viewer_pull(bridge.viewers, id);
    if (!r) {
        if (bridge.viewers) stream_viewer_request(bridge.viewers, id, position, wantVideo, audioAhead, 1);
        return false;
    }
    if (r->next) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "desktop single reader received batch id=%llu", (unsigned long long)id);
        stream_viewer_records_free(r); stream_viewer_resync(bridge.viewers, id); return false;
    }
    record = takeRecord(id, r); return true;
}

bool gui_stream_pull_batch(uint64_t id, std::deque<StreamRecord>& records, int64_t position,
                            unsigned videoBudget, int64_t audioAhead) {
    std::lock_guard<std::mutex> guard(bridge.lock);
    auto* r = stream_viewer_pull(bridge.viewers, id);
    if (!r) {
        if (bridge.viewers) stream_viewer_request(bridge.viewers, id, position, videoBudget, audioAhead, STREAM_VIEWER_BATCH_RECORDS);
        return false;
    }
    while (r) { auto* next = r->next; records.push_back(takeRecord(id, r)); r = next; }
    return true;
}

stream_audio_meter* gui_stream_audio_meter(uint64_t id) {
    std::lock_guard<std::mutex> guard(bridge.lock);
    return stream_viewer_audio_meter(bridge.viewers, id);
}

bool gui_stream_status(uint64_t id, stream_media_status& status) {
    std::lock_guard<std::mutex> guard(bridge.lock);
    return stream_viewer_status(bridge.viewers, id, &status) == 0;
}

bool gui_stream_telemetry(uint64_t id, stream_viewer_telemetry& telemetry) {
    std::lock_guard<std::mutex> guard(bridge.lock);
    return stream_viewer_get_telemetry(bridge.viewers, id, &telemetry) == 0;
}

bool gui_stream_resync(uint64_t id) {
    std::lock_guard<std::mutex> guard(bridge.lock);
    return stream_viewer_resync(bridge.viewers, id) == 0;
}

bool gui_stream_ready() { std::lock_guard<std::mutex> guard(bridge.lock); return bridge.inst != nullptr; }

bool gui_stream_publish_open(uint64_t id, const QString& group, const stream_media_format& format) {
    std::lock_guard<std::mutex> guard(bridge.lock);
    return stream_publisher_open(bridge.publisher, id, groupAddress(group), &format) == 0;
}
int gui_stream_publish_feed(uint64_t id, bool video, uint64_t pts, bool key, const QByteArray& data,
                            const stream_media_format* format) {
    std::lock_guard<std::mutex> guard(bridge.lock);
    return stream_publisher_feed(bridge.publisher, id, video, pts, key,
        reinterpret_cast<const uint8_t*>(data.constData()), static_cast<size_t>(data.size()), format);
}
bool gui_stream_publish_audio(uint64_t id, bool enabled) {
    std::lock_guard<std::mutex> guard(bridge.lock);
    return stream_publisher_set_audio_enabled(bridge.publisher, id, enabled) == 0;
}
int gui_stream_publish_video_rate(uint64_t id, bool automatic, int nominal, int minimum) {
    std::lock_guard<std::mutex> guard(bridge.lock);
    return stream_publisher_video_rate(bridge.publisher, id, automatic, nominal, minimum);
}
void gui_stream_publish_stop(uint64_t id) {
    std::lock_guard<std::mutex> guard(bridge.lock); stream_publisher_stop(bridge.publisher, id);
}
bool gui_stream_publish_status(uint64_t id, stream_publisher_status& status) {
    std::lock_guard<std::mutex> guard(bridge.lock);
    return stream_publisher_snapshot(bridge.publisher, id, &status) == 0;
}

bool gui_stream_statistics(const QString& group, uint64_t epoch, std::vector<stream_stat_node>& nodes, uint64_t& now) {
    std::lock_guard<std::mutex> guard(bridge.lock);
    if (!bridge.inst || group.isEmpty() || !epoch) return false;
    if (bridge.statisticsGroup != group || bridge.statisticsEpoch != epoch) {
        bridge.statistics.clear(); bridge.statisticsGroup = group; bridge.statisticsEpoch = epoch;
    }
    nodes = bridge.statistics; now = bridge.statisticsNow;
    if (!bridge.statisticsPending) {
        auto* task = static_cast<posted_task*>(u_calloc(1, sizeof(posted_task)));
        if (!task) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "statistics GUI task allocation failed"); return false; }
        task->callback = statistics; bridge.statisticsPending = task; uasync_post_reserved(bridge.inst->ua, task);
    }
    return true;
}
