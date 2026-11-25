/*
 * Отрисовка восьми BBR-каналов из кольцевой истории etcpmon_client в GUI-потоке.
 * GraphWndProc получает etcpmon_app через lpCreateParams при CreateWindowEx;
 * история и GDI-перья принадлежат app/client, модуль их не освобождает.
 * Один пиксель X соответствует сэмплу; временная шкала неравномерна при задержке ответов.
 */

#ifndef ETCPMON_GRAPH_H
#define ETCPMON_GRAPH_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Graph constants */
#define GRAPH_WIDTH            860
#define GRAPH_HEIGHT           150
#define CHANNEL_LIST_HEIGHT     80
#define GRAPH_METRICS_COUNT     8
#define GRAPH_HISTORY_SIZE     860

/* Forward declarations */
struct etcpmon_app;
struct metrics_history;

/* Цвета/короткие имена в порядке GRAPH_METRIC_* из etcpmon_client.h. */
extern const COLORREF graph_colors[GRAPH_METRICS_COUNT];
extern const char* graph_metric_names[GRAPH_METRICS_COUNT];

/* WinAPI wndproc: WM_PAINT рисует историю, движение мыши выбирает сэмпл, уход выключает курсор. */
LRESULT CALLBACK GraphWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
/* Нарисовать app в HDC hdc внутри client rect rc через временный off-screen bitmap.
 * Менее двух сэмплов — Waiting for data; масштабы видимой части обновляются не чаще 200 мс. */
void DrawGraph(HDC hdc, RECT* rc, struct etcpmon_app* app);
/* Записать индексы отмеченных каналов в selected[GRAPH_METRICS_COUNT], их число — в *count.
 * Если галочек нет, выбирает первые четыре канала; для NULL app число равно нулю. */
void GetSelectedChannels(struct etcpmon_app* app, int* selected, int* count);
/* Обновить поля значений app по физическому индексу cursor_sample в кольце; неверный индекс даёт "-".
 * Типы/единицы описаны в metrics_history; байты форматируются в B/KiB/MiB, скорости — B/s/KiB/s/MiB/s.
 * Сравнивает с текущим текстом контрола, не хранит отдельного кеша и не добавляет сэмплы. */
void UpdateChannelValues(struct etcpmon_app* app, int cursor_sample);

#ifdef __cplusplus
}
#endif

#endif /* ETCPMON_GRAPH_H */
