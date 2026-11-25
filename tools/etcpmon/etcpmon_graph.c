/*
 * etcpmon_graph.c - ETCP Monitor Graph (20 SPS, 1px=1sample, left scroll, low CPU)
 * 
 * Изменения по твоему запросу:
 *   - Вертикальная линия курсора остаётся
 *   - Строка с значениями под курсором ("RTT-L: 45.2 ...") полностью убрана
 *   - Значения в блоках под графиком всегда живые (обновляются в on_metrics)
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "etcpmon_graph.h"
#include "etcpmon_client.h"
#include <stdlib.h>
#include <stdio.h>
#include "etcpmon_gui.h"
const COLORREF graph_colors[GRAPH_METRICS_COUNT] = {
    RGB(255,0,0),   RGB(0,200,0),   RGB(0,128,255),   RGB(255,128,0),
    RGB(160,0,200), RGB(0,180,180), RGB(255,0,128), RGB(200,200,0)
};
const char* graph_metric_names[GRAPH_METRICS_COUNT] = {
    "InflB","Cwnd","minRTT","bwHi",
    "Pace","bwLo","infHi","Mode"
};
static void RecalculateMinMax(struct metrics_history* hist, int vis, int oldest)
{
    for (int m = 0; m < GRAPH_METRICS_COUNT; m++) {
        float mn = 1e9f;
        float mx = -1e9f;
        for (int i = 0; i < vis; i++) {
            float v = hist->values[m][(oldest + i) % GRAPH_HISTORY_SIZE];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        if (mx - mn < 0.001f) {
            if (mn > 0.0f) { mn *= 0.9f; mx *= 1.1f; }
            else { mx = 1.0f; }
        }
        hist->min_val[m] = mn;
        hist->max_val[m] = mx;
    }
}
void GetSelectedChannels(struct etcpmon_app* app, int* selected, int* count) {
    *count = 0;
    if (!app) return;
    for (int i = 0; i < GRAPH_METRICS_COUNT; i++) {
        if (app->hChannelCheck[i] && 
            SendMessageA(app->hChannelCheck[i], BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selected[(*count)++] = i;
        }
    }
    if (*count == 0) 
        for (int i = 0; i < 4 && i < GRAPH_METRICS_COUNT; i++) 
            selected[(*count)++] = i;
}
void DrawGraph(HDC hdc, RECT* rc, struct etcpmon_app* app)
{
    if (!app || !rc) return;
    int w = rc->right - rc->left;
    int h = rc->bottom - rc->top;
    /* Off-screen буфер */
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);
    SetBkMode(memDC, TRANSPARENT);
    /* Фон */
    HBRUSH bg = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
    FillRect(memDC, rc, bg);
    DeleteObject(bg);
    /* Нет данных */
    if (!app->client || !etcpmon_client_get_history(app->client) || 
        etcpmon_client_get_history(app->client)->count < 2)
    {
        SetTextColor(memDC, RGB(128, 128, 128));
        DrawTextA(memDC, "Waiting for data...", -1, rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        goto end;
    }
    struct metrics_history* hist = etcpmon_client_get_history(app->client);
    /* Выбранные каналы */
    int sel[GRAPH_METRICS_COUNT];
    int sc = 0;
    GetSelectedChannels(app, sel, &sc);
    int pad = 2;
    int gl = pad;
    int gr = w - pad;
    int gt = pad;
    int gb = h - pad;
    int gh = gb - gt;
    HPEN old;
    /* === Сетка === */
    HPEN grid = CreatePen(PS_DOT, 1, GetSysColor(COLOR_BTNSHADOW));
    old = (HPEN)SelectObject(memDC, grid);
    for (int i = 1; i < 5; i++) {
        int y = pad + (h - 2 * pad) * i / 5;
        MoveToEx(memDC, pad, y, NULL);
        LineTo(memDC, w - pad, y);
    }
    SelectObject(memDC, old);
    DeleteObject(grid);
    /* Сколько точек видно */
    int vis = (hist->count < gr - gl) ? hist->count : (gr - gl);
    int oldest = (hist->head - vis + GRAPH_HISTORY_SIZE) % GRAPH_HISTORY_SIZE;
    int startx = gr - vis;
    /* Пересчёт min/max 5 раз в секунду */
    DWORD now = GetTickCount();
    if (now - hist->last_minmax_update >= 200) {
        RecalculateMinMax(hist, vis, oldest);
        hist->last_minmax_update = now;
    }
    /* Буфер точек */
    POINT* points = (POINT*)malloc((vis + 1) * sizeof(POINT));
    /* Рисуем графики */
    for (int ch = 0; ch < sc; ch++) {
        int m = sel[ch];
        float mn = hist->min_val[m];
        float mx = hist->max_val[m];
        SelectObject(memDC, app->hGraphPens[m]);
        for (int i = 0; i < vis; i++) {
            float v = hist->values[m][(oldest + i) % GRAPH_HISTORY_SIZE];
            int x = startx + i;
            int y = gb - (int)((v - mn) / (mx - mn + 1e-9f) * (float)gh);
            if (y < gt) y = gt;
            if (y > gb) y = gb;
            points[i].x = x;
            points[i].y = y;
        }
        Polyline(memDC, points, vis);
    }
    free(points);
    /* === ВЕРТИКАЛЬНАЯ ЛИНИЯ КУРСОРА (только линия, без текста) === */
    if (app->graph_cursor_active) {
        HPEN cp = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));
        old = (HPEN)SelectObject(memDC, cp);
        MoveToEx(memDC, app->graph_cursor_x, gt, NULL);
        LineTo(memDC, app->graph_cursor_x, gb);
        SelectObject(memDC, old);
        DeleteObject(cp);

        int cursor_rel_x = app->graph_cursor_x - startx;
        if (cursor_rel_x >= 0 && cursor_rel_x < vis) {
            int cursor_idx = (oldest + cursor_rel_x) % GRAPH_HISTORY_SIZE;
            UpdateChannelValues(app, cursor_idx);
        }
    }
end:
    /* Копируем на экран */
    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memDC);
}
void UpdateChannelValues(struct etcpmon_app* app, int history_idx)
{
    if (!app || !app->client) return;
    struct metrics_history* h = etcpmon_client_get_history(app->client);
    if (!h || !h->count || history_idx < 0 || history_idx >= GRAPH_HISTORY_SIZE) {
        for (int i = 0; i < GRAPH_METRICS_COUNT; i++) {
            if (app->hChannelValue[i])
                SetWindowTextA(app->hChannelValue[i], "-");
        }
        return;
    }
    for (int i = 0; i < GRAPH_METRICS_COUNT; i++) {
        if (!app->hChannelValue[i]) continue;
        float val = h->values[i][history_idx];
        char buf[32] = {0};
        if (i == GRAPH_METRIC_BW_HI || i == GRAPH_METRIC_BW_LO || i == GRAPH_METRIC_PACING_RATE) {
            if (val >= 1024*1024) snprintf(buf, sizeof(buf), "%.1f MiB/s", val/(1024.*1024.));
            else if (val >= 1024) snprintf(buf, sizeof(buf), "%.1f KiB/s", val/1024.);
            else                 snprintf(buf, sizeof(buf), "%.0f B/s", val);
        } else if (i == GRAPH_METRIC_INFLIGHT_BYTES || i == GRAPH_METRIC_INFLIGHT_LIM || i == GRAPH_METRIC_INFLIGHT_HI) {
            if (val >= 1024*1024) snprintf(buf, sizeof(buf), "%.1f MiB", val/(1024.*1024.));
            else if (val >= 1024) snprintf(buf, sizeof(buf), "%.1f KiB", val/1024.);
            else                  snprintf(buf, sizeof(buf), "%.0f B", val);
        } else if (i == GRAPH_METRIC_MIN_RTT) {
            snprintf(buf, sizeof(buf), "%.1f ms", val);
        } else if (i == GRAPH_METRIC_BBR_MODE) {
            static const char* mnames[] = {"STARTUP","DRAIN","PROBE_BW","PROBE_RTT"};
            int m = (int)val; if (m < 0) m = 0; if (m > 3) m = 3;
            snprintf(buf, sizeof(buf), "%s", mnames[m]);
        } else {
            snprintf(buf, sizeof(buf), "%.1f", val);
        }
        char current[32];
        GetWindowTextA(app->hChannelValue[i], current, sizeof(current));
        if (strcmp(buf, current) == 0) continue;
        SendMessageA(app->hChannelValue[i], WM_SETREDRAW, FALSE, 0);
        SetWindowTextA(app->hChannelValue[i], buf);
        SendMessageA(app->hChannelValue[i], WM_SETREDRAW, TRUE, 0);
        InvalidateRect(app->hChannelValue[i], NULL, FALSE);
    }
}
LRESULT CALLBACK GraphWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    struct etcpmon_app* app = (struct etcpmon_app*)GetWindowLongPtrA(hWnd, GWLP_USERDATA);
    switch (message) {
        case WM_CREATE: {
            CREATESTRUCTA* cs = (CREATESTRUCTA*)lParam;
            app = cs->lpCreateParams;
            SetWindowLongPtrA(hWnd, GWLP_USERDATA, (LONG_PTR)app);
            if (app) app->hGraphWnd = hWnd;
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);
            DrawGraph(hdc, &rc, app);
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!app) break;
            
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            RECT rc;
            GetClientRect(hWnd, &rc);
            
            BOOL inside = (x >= 0 && x < rc.right && y >= 0 && y < rc.bottom);
            
            if (inside) {
                app->graph_cursor_x = x;
                app->graph_cursor_active = 1;
                
                static DWORD last_invalidate = 0;
                DWORD now = GetTickCount();
                if (now - last_invalidate >= 10) {
                    InvalidateRect(hWnd, NULL, FALSE);
                    last_invalidate = now;
                }

                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
                TrackMouseEvent(&tme);
            } else {
                app->graph_cursor_active = 0;
                InvalidateRect(hWnd, NULL, FALSE);
            }
            break;
        }
        case WM_MOUSELEAVE:
            if (app) {
                app->graph_cursor_active = 0;
                InvalidateRect(hWnd, NULL, FALSE);
            }
            break;
        default: 
            return DefWindowProcA(hWnd, message, wParam, lParam);
    }
    return 0;
}
