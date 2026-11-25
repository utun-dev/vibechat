/* Баланс логической очереди DATA: ключи представлены курсорами, содержимое остаётся в кольце.
 * A/D/X — накопительные packets/bytes; Q = A-D-X. D означает принятие ETCP, не доставку.
 * Содержимое/таймеры не выделяет; контекст принадлежит flow/peer/network и обслуживается одним uasync-потоком.
 * Хранит50 интервалов длительностью>=200мс: rates по последним10с, положительный A-D по последней1с.
 * Частичные интервалы взвешиваются по времени; X не уменьшает измеренный исходный избыток A-D.
 * Смена состава выдач исключает смешанный интервал из оценки роста и сбрасывает подтверждение перегрузки.
 * Владелец передаёт реальный возраст очереди и сам увеличивает rejected при отказах admission/control. */
#ifndef STREAM_LOAD_H
#define STREAM_LOAD_H

#include "stream_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STREAM_LOAD_PERIOD_MS 200
#define STREAM_LOAD_HISTORY 50
#define STREAM_LOAD_CONFIRM_MS 500
#define STREAM_LOAD_GRACE_MS 2000
#define STREAM_LOAD_MAX_AGE_MS 4000

struct stream_load_counts { uint64_t packets, bytes; }; // Число RS-частей и их байты; без копий DATA.
struct stream_load_interval {
    uint64_t begin_ms, end_ms, revision; // Локальные границы интервала; revision0 означает смешанный состав выдач.
    struct stream_load_counts arrivals, sent, removed; // Дельты A/D/X за этот интервал.
};
struct stream_load_snapshot {
    struct stream_load_counts arrivals, sent, removed, queued; // Накопительные A/D/X и текущий Q=A-D-X.
    uint64_t rejected, // Счётчик отказов владельца, не объём removed.
             input_rate, output_rate, growth_rate, // Байты/с: A,D за10с и max(A-D,0) за1с с текущей revision.
             oldest_age_ms, interval_ms; // Переданный возраст старейшей записи и длительность последнего завершённого интервала.
    int valid, pressure, confirmed; // Есть интервал; Q>0/age>200мс/(рост либо D=0); устойчиво>=500мс.
};
struct stream_load {
    struct stream_load_interval history[STREAM_LOAD_HISTORY]; // Кольцо законченных интервалов.
    struct stream_load_snapshot snapshot; // Rates последнего интервала с последними Q/age/pressure.
    struct stream_load_counts arrivals, sent, removed, queued; // Рабочие накопители A/D/X/Q.
    struct stream_load_counts previous_a, previous_d, previous_x; // Базы расчёта дельт следующего интервала.
    uint64_t rejected, previous_ms, revision, previous_revision, // Отказы, граница интервала и версии состава выдач.
        since_ms, last_d; // Начало непрерывного pressure и D последнего интервала.
    unsigned head, count; // Следующий слот истории и число занятых слотов.
    int candidate; // Удерживается непрерывная перегрузка с since_ms.
};

/* Обнулить накопители/историю, установить начальное время и revision1. Контекст обязателен. */
void stream_load_init(struct stream_load *load, uint64_t now_ms);
/* Добавить ненулевые packets/bytes в A/Q; OK/ERR_ARGUMENT, при переполнении ничего не меняется. */
int stream_load_add(struct stream_load *load, uint64_t packets, uint64_t bytes);
/* Убрать учтённые packets/bytes из Q: sent!=0 увеличивает D, иначе X. OK/ERR_STATE; отказ атомарен. */
int stream_load_remove(struct stream_load *load, uint64_t packets, uint64_t bytes, int sent);
/* После изменения состава выдач поднять revision, сбросить кандидата pressure и growth_rate, не накопители. */
void stream_load_changed(struct stream_load *load);
/* now_ms>=предыдущей границы; oldest_age_ms вычисляет владелец. Всегда обновляет live Q/pressure и копирует snapshot.
 * OK — записан новый интервал>=200мс; WAIT — ещё рано, прежние rates и актуальный Q всё равно возвращены.
 * ERR_ARGUMENT не меняет load/snapshot. Флаг confirmed требует непрерывного pressure>=500мс. */
int stream_load_poll(struct stream_load *load, uint64_t now_ms, uint64_t oldest_age_ms, struct stream_load_snapshot *snapshot);
#ifdef __cplusplus
}
#endif
#endif
