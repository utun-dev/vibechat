/*
 * etcpmon_gui.c - ETCP Monitor GUI Implementation
 */
/* Must define this before any windows headers to exclude winsock.h */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
/* Must include winsock2.h before windows.h */
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include "etcpmon_gui.h"
#include "etcpmon_client.h"
#include "etcpmon_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
//#pragma comment(lib, "comctl32.lib")
//#pragma comment(lib, "user32.lib")
//#pragma comment(lib, "gdi32.lib")
#define WINDOW_WIDTH    1000
#define WINDOW_HEIGHT   1400
#define WINDOW_CLIENT_W 1250
#define UPDATE_INTERVAL 10
#define LIST_UPDATE_INTERVAL 1000
/* Global app pointer for callbacks */
static struct etcpmon_app* g_app = NULL;
/* Forward declarations */
static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK ActionResultWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
static void CreateControls(struct etcpmon_app* app);
static void OnConnect(struct etcpmon_app* app);
static void OnDisconnect(struct etcpmon_app* app);
static void OnConnectionSelect(struct etcpmon_app* app);
static void OnAction(struct etcpmon_app* app);
static void OnTimer(struct etcpmon_app* app);
static void UpdateUIState(struct etcpmon_app* app);
static void OnDebugRadio(struct etcpmon_app* app, int control_id);
static void RefreshDebugUI(struct etcpmon_app* app);
static void AddTooltip(HWND hTip, HWND hCtrl, const char* text);
static void CreateTooltips(struct etcpmon_app* app);
/* Callback functions for client events */
static void on_connected(void* user_data);
static void on_disconnected(void* user_data);
static void on_conn_list(struct etcpmon_conn_info* list, uint8_t count, void* user_data);
static void on_socket_list(struct etcpmon_socket_info* list, uint8_t count, void* user_data);
static void on_metrics(struct etcpmon_rsp_metrics* metrics,
                      struct etcpmon_link_metrics* links,
                      uint8_t links_count, void* user_data);
static void on_error(const char* msg, void* user_data);
static void on_action_result(const char* text, void* user_data);

int etcpmon_gui_init(struct etcpmon_app* app, HINSTANCE hInstance) {
    if (!app) return -1;
    
    memset(app, 0, sizeof(*app));
    app->hInstance = hInstance;
    g_app = app;
    
    /* Initialize common controls */
    INITCOMMONCONTROLSEX iccex;
    iccex.dwSize = sizeof(iccex);
    iccex.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&iccex);
    
    /* === НОВОЕ === */
    app->need_initial_request = 1;
    app->last_selected_peer_id = 0;
    app->isConnected = 0;

    /* Register window class */
    WNDCLASSEXA wcex;
    memset(&wcex, 0, sizeof(wcex));
    wcex.cbSize = sizeof(wcex);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wcex.lpszClassName = ETCPMON_WINDOW_CLASS;
    wcex.hIconSm = LoadIcon(NULL, IDI_APPLICATION);
    
    if (!RegisterClassExA(&wcex)) {
        return -1;
    }
    
    /* Register Graph window class */
    WNDCLASSEXA wcexGraph;
    memset(&wcexGraph, 0, sizeof(wcexGraph));
    wcexGraph.cbSize = sizeof(wcexGraph);
    wcexGraph.style = CS_HREDRAW | CS_VREDRAW;
    wcexGraph.lpfnWndProc = GraphWndProc;
    wcexGraph.hInstance = hInstance;
    wcexGraph.hIcon = NULL;
    wcexGraph.hCursor = LoadCursor(NULL, IDC_CROSS);
    wcexGraph.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcexGraph.lpszClassName = "ETCPMonGraph";
    wcexGraph.hIconSm = NULL;
    
    RegisterClassExA(&wcexGraph);

    /* Register ActionResult window class */
    WNDCLASSEXA wcexAR;
    memset(&wcexAR, 0, sizeof(wcexAR));
    wcexAR.cbSize = sizeof(wcexAR);
    wcexAR.style = CS_HREDRAW | CS_VREDRAW;
    wcexAR.lpfnWndProc = ActionResultWndProc;
    wcexAR.hInstance = hInstance;
    wcexAR.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcexAR.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcexAR.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wcexAR.lpszClassName = "ETCPMonActionResult";
    wcexAR.hIconSm = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExA(&wcexAR);
    
    /* Initialize Winsock */
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return -1;
    }
    
    return 0;
}
HWND etcpmon_gui_create_window(struct etcpmon_app* app) {
    if (!app) return NULL;
    
    /* Create main window */
    app->hWndMain = CreateWindowExA(
        0,
        ETCPMON_WINDOW_CLASS,
        "ETCP Monitor",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT,
        WINDOW_CLIENT_W, WINDOW_HEIGHT,
        NULL, NULL, app->hInstance, app
    );
    
    if (!app->hWndMain) {
        return NULL;
    }
    
    /* Create status bar */
    app->hWndStatus = CreateWindowExA(
        0, STATUSCLASSNAME, NULL,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0,
        app->hWndMain, (HMENU)IDC_STATIC, app->hInstance, NULL
    );
    
    SetTimer(app->hWndMain, IDC_TIMER_UPDATE, UPDATE_INTERVAL, NULL);
    
    /* Initialize client */
    app->client = (struct etcpmon_client*)malloc(sizeof(struct etcpmon_client));
    if (app->client) {
        etcpmon_client_init(app->client);
        etcpmon_client_set_callbacks(app->client,
                                      on_connected, on_disconnected,
                                      on_conn_list, on_socket_list, on_metrics, on_error,
                                      on_action_result,
                                      app);
    }
    
    CreateControls(app);
    CreateTooltips(app);

    for (int i = 0; i < GRAPH_METRICS_COUNT; i++) {
        app->hGraphPens[i] = CreatePen(PS_SOLID, 2, graph_colors[i]);
    }
    
    /* Set default values */
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ADDR, "192.168.40.1");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_PORT, "9091");
    UpdateUIState(app);
    OnConnect(app);
    
    return app->hWndMain;
}
static void CreateControls(struct etcpmon_app* app) {
    HWND hWnd = app->hWndMain;
    HINSTANCE hInst = app->hInstance;
    int x = 10, y = 10;
    
    /* Connection group */
    CreateWindowExA(0, "BUTTON", "Connection",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        x, y, WINDOW_WIDTH - 20, 60, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    
    /* Address label and edit */
    CreateWindowExA(0, "STATIC", "Server:",
        WS_CHILD | WS_VISIBLE,
        x + 10, y + 20, 40, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    
    app->hEditAddr = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        x + 55, y + 18, 120, 22, hWnd, (HMENU)IDC_EDIT_ADDR, hInst, NULL);
    
    /* Port label and edit */
    CreateWindowExA(0, "STATIC", "Port:",
        WS_CHILD | WS_VISIBLE,
        x + 185, y + 20, 30, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    
    app->hEditPort = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_NUMBER,
        x + 220, y + 18, 60, 22, hWnd, (HMENU)IDC_EDIT_PORT, hInst, NULL);
    
    /* Connect/Disconnect buttons */
    app->hBtnConnect = CreateWindowExA(0, "BUTTON", "Connect",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x + 290, y + 18, 80, 24, hWnd, (HMENU)IDC_BTN_CONNECT, hInst, NULL);
    
    app->hBtnDisconnect = CreateWindowExA(0, "BUTTON", "Disconnect",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
        x + 380, y + 18, 80, 24, hWnd, (HMENU)IDC_BTN_DISCONNECT, hInst, NULL);
    
    y += 70;
    
    /* Connections list */
    CreateWindowExA(0, "STATIC", "Connections:",
        WS_CHILD | WS_VISIBLE,
        x, y, 100, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    
    app->hListConnections = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
        WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
        x, y + 20, 300, 120, hWnd, (HMENU)IDC_LIST_CONNECTIONS, hInst, NULL);

    /* Sockets list */
    CreateWindowExA(0, "STATIC", "Sockets:",
        WS_CHILD | WS_VISIBLE,
        x + 320, y, 100, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);

    app->hListSockets = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
        WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
        x + 320, y + 20, 550, 120, hWnd, (HMENU)IDC_LIST_SOCKETS, hInst, NULL);

    /* Action input */
    CreateWindowExA(0, "STATIC", "Action:",
        WS_CHILD | WS_VISIBLE,
        x + 880, y, 80, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);

    app->hEditAction = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        x + 880, y + 20, 110, 22, hWnd, (HMENU)IDC_EDIT_ACTION, hInst, NULL);

    app->hBtnAction = CreateWindowExA(0, "BUTTON", "Send",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x + 880, y + 44, 60, 24, hWnd, (HMENU)IDC_BTN_ACTION, hInst, NULL);

    /* Graph area */
    y += 140;
    app->hGraphWnd = CreateWindowExA(
        WS_EX_CLIENTEDGE,
        "ETCPMonGraph", "",
        WS_CHILD | WS_VISIBLE | WS_BORDER,
        10, y, GRAPH_WIDTH, GRAPH_HEIGHT,
        hWnd, (HMENU)IDC_GRAPH, hInst, app);
    
    /* Channel blocks below graph (checkbox + name в одной строке, значение ниже) */
    y += GRAPH_HEIGHT + 8;
    
    const char* channel_short_names[] = {
        "InflB", "Cwnd", "minRTT", "bwHi",
        "Pace", "bwLo", "infHi", "Mode"
    };
    
    int block_w = 200;
    int block_h = 48;      /* компактная высота */
    int cols = 4;
    int spacing = 6;
    
    for (int i = 0; i < GRAPH_METRICS_COUNT; i++) {
        int col = i % cols;
        int row = i / cols;
        int bx = 10 + col * (block_w + spacing);
        int by = y + row * (block_h + spacing);
        
        /* Галочка слева */
        app->hChannelCheck[i] = CreateWindowExA(0, "BUTTON", "",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BST_CHECKED,
            bx, by, 20, 18,
            hWnd, (HMENU)(UINT_PTR)(IDC_CH_CHECK_0 + i), hInst, NULL);
        
        /* Название сразу справа от галочки */
        app->hChannelName[i] = CreateWindowExA(0, "STATIC", channel_short_names[i],
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            bx + 24, by + 2, block_w - 30, 16,
            hWnd, (HMENU)(UINT_PTR)(IDC_CH_NAME_0 + i), hInst, NULL);
        
        /* Значение под названием */
        app->hChannelValue[i] = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
            bx, by + 21, block_w, 22,
            hWnd, (HMENU)(UINT_PTR)(IDC_CH_VALUE_0 + i), hInst, NULL);
    }
    
    int channel_grid_h = ((GRAPH_METRICS_COUNT + cols - 1) / cols) * (block_h + spacing) - spacing;
    y += channel_grid_h;
    
    /* ETCP Metrics group */
    y = 485;
    CreateWindowExA(0, "BUTTON", "ETCP Metrics",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        x, y, (WINDOW_WIDTH - 40) / 2, 210, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    
    int mx = x + 10, my = y + 20;
    
    CreateWindowExA(0, "STATIC", "Name:",
        WS_CHILD | WS_VISIBLE, mx, my, 80, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditEtcpName = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
        mx + 85, my, 310, 20, hWnd, (HMENU)IDC_EDIT_ETCP_NAME, hInst, NULL);
    
    my += 25;
    CreateWindowExA(0, "STATIC", "RTT Last:",
        WS_CHILD | WS_VISIBLE, mx, my, 80, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditEtcpRttLast = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY,
        mx + 85, my, 100, 20, hWnd, (HMENU)IDC_EDIT_ETCP_RTT_LAST, hInst, NULL);
    
    CreateWindowExA(0, "STATIC", "RTT Avg 10:",
        WS_CHILD | WS_VISIBLE, mx + 200, my, 80, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditEtcpRttAvg10 = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY,
        mx + 285, my, 100, 20, hWnd, (HMENU)IDC_EDIT_ETCP_RTT_AVG10, hInst, NULL);
    
    my += 25;
    CreateWindowExA(0, "STATIC", "RTTAvg100:",
        WS_CHILD | WS_VISIBLE, mx, my, 80, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditEtcpRttAvg100 = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY,
        mx + 85, my, 100, 20, hWnd, (HMENU)IDC_EDIT_ETCP_RTT_AVG100, hInst, NULL);
    
    CreateWindowExA(0, "STATIC", "Jitter:",
        WS_CHILD | WS_VISIBLE, mx + 200, my, 80, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditEtcpJitter = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY,
        mx + 285, my, 100, 20, hWnd, (HMENU)IDC_EDIT_ETCP_JITTER, hInst, NULL);
    
    my += 25;
    CreateWindowExA(0, "STATIC", "Bytes Sent:",
        WS_CHILD | WS_VISIBLE, mx, my, 80, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditEtcpBytesSent = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY,
        mx + 85, my, 100, 20, hWnd, (HMENU)IDC_EDIT_ETCP_BYTES_SENT, hInst, NULL);
    
    CreateWindowExA(0, "STATIC", "Retrans:",
        WS_CHILD | WS_VISIBLE, mx + 200, my, 80, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditEtcpRetrans = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY,
        mx + 285, my, 100, 20, hWnd, (HMENU)IDC_EDIT_ETCP_RETRANS, hInst, NULL);
    
    my += 25;
    CreateWindowExA(0, "STATIC", "RX Dup:",
        WS_CHILD | WS_VISIBLE, mx, my, 80, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditEtcpRxDup = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY,
        mx + 85, my, 100, 20, hWnd, (HMENU)IDC_EDIT_ETCP_RX_DUP, hInst, NULL);

    CreateWindowExA(0, "STATIC", "TX Dup:",
        WS_CHILD | WS_VISIBLE, mx + 200, my, 80, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditEtcpTxDup = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY,
        mx + 285, my, 100, 20, hWnd, (HMENU)IDC_EDIT_ETCP_TX_DUP, hInst, NULL);

    my += 25;
    CreateWindowExA(0, "STATIC", "ACKs:",
        WS_CHILD | WS_VISIBLE, mx, my, 80, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditEtcpAcks = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY,
        mx + 85, my, 100, 20, hWnd, (HMENU)IDC_EDIT_ETCP_ACKS, hInst, NULL);

    CreateWindowExA(0, "STATIC", "Inflight:",
        WS_CHILD | WS_VISIBLE, mx + 200, my, 80, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditEtcpInflight = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY,
        mx + 285, my, 100, 20, hWnd, (HMENU)IDC_EDIT_ETCP_INFLIGHT, hInst, NULL);

    my += 25;
    CreateWindowExA(0, "STATIC", "Links:",
        WS_CHILD | WS_VISIBLE, mx, my, 80, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditEtcpLinks = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY,
        mx + 85, my, 100, 20, hWnd, (HMENU)IDC_EDIT_ETCP_LINKS, hInst, NULL);
    
    CreateWindowExA(0, "STATIC", "MaxInfl:",
        WS_CHILD | WS_VISIBLE, mx + 200, my, 80, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditEtcpMaxInflight = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY,
        mx + 285, my, 100, 20, hWnd, (HMENU)IDC_EDIT_ETCP_MAX_INFLIGHT, hInst, NULL);
    
    /* TUN/Routing Metrics group - right of ETCP Metrics */
    int group_w = (WINDOW_WIDTH - 40) / 2;
    x = 10 + group_w + 20; y = 485;
    CreateWindowExA(0, "BUTTON", "TUN/Routing Metrics",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        x, y, group_w, 210, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    
    mx = x + 10; my = y + 20;
    
    CreateWindowExA(0, "STATIC", "Read Bytes:",
        WS_CHILD | WS_VISIBLE, mx, my, 80, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditTunReadBytes = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY,
        mx + 85, my, 100, 20, hWnd, (HMENU)IDC_EDIT_TUN_READ_BYTES, hInst, NULL);
    
    CreateWindowExA(0, "STATIC", "Write Bytes:",
        WS_CHILD | WS_VISIBLE, mx + 200, my, 90, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditTunWriteBytes = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY,
        mx + 285, my, 100, 20, hWnd, (HMENU)IDC_EDIT_TUN_WRITE_BYTES, hInst, NULL);
    
    my += 25;
    CreateWindowExA(0, "STATIC", "Read Pkts:",
        WS_CHILD | WS_VISIBLE, mx, my, 80, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditTunReadPkts = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY,
        mx + 85, my, 100, 20, hWnd, (HMENU)IDC_EDIT_TUN_READ_PKTS, hInst, NULL);
    
    CreateWindowExA(0, "STATIC", "Write Pkts:",
        WS_CHILD | WS_VISIBLE, mx + 200, my, 90, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditTunWritePkts = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY,
        mx + 285, my, 100, 20, hWnd, (HMENU)IDC_EDIT_TUN_WRITE_PKTS, hInst, NULL);
    
    my += 25;
    CreateWindowExA(0, "STATIC", "Read Errs:",
        WS_CHILD | WS_VISIBLE, mx, my, 80, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditTunReadErrs = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY,
        mx + 85, my, 100, 20, hWnd, (HMENU)IDC_EDIT_TUN_READ_ERRS, hInst, NULL);
    
    CreateWindowExA(0, "STATIC", "Write Errs:",
        WS_CHILD | WS_VISIBLE, mx + 200, my, 90, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditTunWriteErrs = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY,
        mx + 285, my, 100, 20, hWnd, (HMENU)IDC_EDIT_TUN_WRITE_ERRS, hInst, NULL);
    
    /* Routing section */
    my += 30;
    CreateWindowExA(0, "STATIC", "Routed:",
        WS_CHILD | WS_VISIBLE, mx, my, 60, 18, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditRtRouted = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        mx + 65, my - 2, 80, 18, hWnd, (HMENU)IDC_EDIT_RT_ROUTED, hInst, NULL);
    
    CreateWindowExA(0, "STATIC", "Dropped:",
        WS_CHILD | WS_VISIBLE, mx + 155, my, 60, 18, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditRtDropped = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        mx + 220, my - 2, 80, 18, hWnd, (HMENU)IDC_EDIT_RT_DROPPED, hInst, NULL);
    
    my += 25;
    CreateWindowExA(0, "STATIC", "TunInQ:",
        WS_CHILD | WS_VISIBLE, mx, my, 60, 18, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditRtTunInQPkts = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        mx + 65, my - 2, 50, 18, hWnd, (HMENU)IDC_EDIT_RT_TUN_IN_Q_PKTS, hInst, NULL);
    app->hEditRtTunInQBytes = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        mx + 120, my - 2, 60, 18, hWnd, (HMENU)IDC_EDIT_RT_TUN_IN_Q_BYTES, hInst, NULL);
    
    CreateWindowExA(0, "STATIC", "TunOutQ:",
        WS_CHILD | WS_VISIBLE, mx + 190, my, 60, 18, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditRtTunOutQPkts = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        mx + 255, my - 2, 50, 18, hWnd, (HMENU)IDC_EDIT_RT_TUN_OUT_Q_PKTS, hInst, NULL);
    app->hEditRtTunOutQBytes = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        mx + 310, my - 2, 60, 18, hWnd, (HMENU)IDC_EDIT_RT_TUN_OUT_Q_BYTES, hInst, NULL);
    
    my += 25;
    CreateWindowExA(0, "STATIC", "RT:",
        WS_CHILD | WS_VISIBLE, mx, my, 30, 18, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditRtCount = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        mx + 35, my - 2, 50, 18, hWnd, (HMENU)IDC_EDIT_RT_COUNT, hInst, NULL);
    
    CreateWindowExA(0, "STATIC", "Local:",
        WS_CHILD | WS_VISIBLE, mx + 95, my, 40, 18, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditRtLocal = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        mx + 140, my - 2, 50, 18, hWnd, (HMENU)IDC_EDIT_RT_LOCAL, hInst, NULL);
    
    CreateWindowExA(0, "STATIC", "Learned:",
        WS_CHILD | WS_VISIBLE, mx + 200, my, 55, 18, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditRtLearned = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        mx + 260, my - 2, 50, 18, hWnd, (HMENU)IDC_EDIT_RT_LEARNED, hInst, NULL);
    
    my += 25;
    CreateWindowExA(0, "STATIC", "SndList:",
        WS_CHILD | WS_VISIBLE, mx, my, 55, 18, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditRtBgpSenders = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        mx + 60, my - 2, 45, 18, hWnd, (HMENU)IDC_EDIT_RT_BGP_SENDERS, hInst, NULL);
    
    CreateWindowExA(0, "STATIC", "Nodes:",
        WS_CHILD | WS_VISIBLE, mx + 130, my, 45, 18, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditRtBgpNodes = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        mx + 130+50, my - 2, 45, 18, hWnd, (HMENU)IDC_EDIT_RT_BGP_NODES, hInst, NULL);
    
    /* Links list */
    y = 710;
    CreateWindowExA(0, "STATIC", "Links:",
        WS_CHILD | WS_VISIBLE,
        10, y, 100, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    
    app->hListLinks = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
        WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
        10, y + 20, WINDOW_WIDTH - 20, 160, hWnd, (HMENU)IDC_LIST_LINKS, hInst, NULL);
    
    /* Queues & Errors group - below Links */
    y = 900;
    CreateWindowExA(0, "BUTTON", "Queues & Errors",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        10, y, WINDOW_WIDTH - 20, 280, hWnd, (HMENU)IDC_STATIC_QUEUES, hInst, NULL);
    
    int qy = y + 22;
    int q_col1 = 20;
    int q_col2 = 300;
    int q_col3 = 580;
    
    /* Queue metrics - row 1 */
    CreateWindowExA(0, "STATIC", "InQ:",
        WS_CHILD | WS_VISIBLE, q_col1, qy, 50, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditQInQBytes = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 55, qy - 2, 60, 18, hWnd, (HMENU)IDC_EDIT_Q_IN_Q_BYTES, hInst, NULL);
    app->hEditQInQPkts = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 120, qy - 2, 50, 18, hWnd, (HMENU)IDC_EDIT_Q_IN_Q_PKTS, hInst, NULL);
    
    CreateWindowExA(0, "STATIC", "InSend:",
        WS_CHILD | WS_VISIBLE, q_col2, qy, 60, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditQInSendBytes = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col2 + 65, qy - 2, 60, 18, hWnd, (HMENU)IDC_EDIT_Q_IN_SEND_BYTES, hInst, NULL);
    app->hEditQInSendPkts = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col2 + 130, qy - 2, 50, 18, hWnd, (HMENU)IDC_EDIT_Q_IN_SEND_PKTS, hInst, NULL);
    
    CreateWindowExA(0, "STATIC", "WaitAck:",
        WS_CHILD | WS_VISIBLE, q_col3, qy, 65, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditQWaitAckBytes = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col3 + 70, qy - 2, 60, 18, hWnd, (HMENU)IDC_EDIT_Q_WAIT_ACK_BYTES, hInst, NULL);
    app->hEditQWaitAckPkts = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col3 + 135, qy - 2, 50, 18, hWnd, (HMENU)IDC_EDIT_Q_WAIT_ACK_PKTS, hInst, NULL);
    app->hEditQWaitAckOptInfl = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col3 + 190, qy - 2, 60, 18, hWnd, (HMENU)IDC_EDIT_WAITACK_OPT_INFL, hInst, NULL);
    
    /* Queue metrics - row 2 */
    qy += 28;
    CreateWindowExA(0, "STATIC", "AckQ:",
        WS_CHILD | WS_VISIBLE, q_col1, qy, 50, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditQAckBytes = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 55, qy - 2, 60, 18, hWnd, (HMENU)IDC_EDIT_Q_ACK_BYTES, hInst, NULL);
    app->hEditQAckPkts = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 120, qy - 2, 50, 18, hWnd, (HMENU)IDC_EDIT_Q_ACK_PKTS, hInst, NULL);
    
    CreateWindowExA(0, "STATIC", "RecvQ:",
        WS_CHILD | WS_VISIBLE, q_col2, qy, 60, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditQRecvBytes = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col2 + 65, qy - 2, 60, 18, hWnd, (HMENU)IDC_EDIT_Q_RECV_BYTES, hInst, NULL);
    app->hEditQRecvPkts = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col2 + 130, qy - 2, 50, 18, hWnd, (HMENU)IDC_EDIT_Q_RECV_PKTS, hInst, NULL);
    
    CreateWindowExA(0, "STATIC", "OutQ:",
        WS_CHILD | WS_VISIBLE, q_col3, qy, 50, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditQOutBytes = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col3 + 55, qy - 2, 60, 18, hWnd, (HMENU)IDC_EDIT_Q_OUT_BYTES, hInst, NULL);
    app->hEditQOutPkts = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col3 + 120, qy - 2, 50, 18, hWnd, (HMENU)IDC_EDIT_Q_OUT_PKTS, hInst, NULL);
    
    /* Error counters - row 3 */
    qy += 30;
    CreateWindowExA(0, "STATIC", "Errors:",
        WS_CHILD | WS_VISIBLE, q_col1, qy, 50, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditErrReinit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 55, qy - 2, 55, 18, hWnd, (HMENU)IDC_EDIT_ERR_REINIT, hInst, NULL);
    app->hEditErrReset = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 115, qy - 2, 55, 18, hWnd, (HMENU)IDC_EDIT_ERR_RESET, hInst, NULL);
    app->hEditErrPktFmt = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 175, qy - 2, 55, 18, hWnd, (HMENU)IDC_EDIT_ERR_PKT_FMT, hInst, NULL);
    
    /* Timer flags - same row, to the right */
    CreateWindowExA(0, "STATIC", "Timers:",
        WS_CHILD | WS_VISIBLE, q_col2, qy, 50, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditTimerRetrans = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col2 + 55, qy - 2, 50, 18, hWnd, (HMENU)IDC_EDIT_TIMER_RETRANS, hInst, NULL);
    app->hEditTimerAckResp = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col2 + 110, qy - 2, 50, 18, hWnd, (HMENU)IDC_EDIT_TIMER_ACK_RESP, hInst, NULL);

    /* System resources - below timer flags */
    qy += 28;
    CreateWindowExA(0, "STATIC", "Timeouts:",
        WS_CHILD | WS_VISIBLE, q_col1, qy, 60, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditActiveTimeouts = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 65, qy - 2, 50, 18, hWnd, (HMENU)IDC_EDIT_ACTIVE_TIMEOUTS, hInst, NULL);

    CreateWindowExA(0, "STATIC", "Memory:",
        WS_CHILD | WS_VISIBLE, q_col2, qy, 50, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditBusyMemory = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col2 + 60, qy - 2, 60, 18, hWnd, (HMENU)IDC_EDIT_BUSY_MEMORY, hInst, NULL);
    
    /* WaitAck queue state */
    qy += 30;
    CreateWindowExA(0, "STATIC", "WaitAck:",
        WS_CHILD | WS_VISIBLE, q_col1, qy, 60, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditWaitAckSuspend = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 65, qy - 2, 60, 18, hWnd, (HMENU)IDC_EDIT_WAITACK_SUSP, hInst, NULL);
    app->hEditWaitAckCb = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 130, qy - 2, 60, 18, hWnd, (HMENU)IDC_EDIT_WAITACK_CB, hInst, NULL);
    app->hEditWaitAckTimer = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 195, qy - 2, 60, 18, hWnd, (HMENU)IDC_EDIT_WAITACK_TIMER, hInst, NULL);
    
    /* ID metrics - row 4 */
//    qy += 30;
    CreateWindowExA(0, "STATIC", "IDs:",
        WS_CHILD | WS_VISIBLE, q_col2, qy, 30, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditIdNextTx = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col2 + 35, qy - 2, 65, 18, hWnd, (HMENU)IDC_EDIT_ID_NEXT_TX, hInst, NULL);
    app->hEditIdLastRx = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col2 + 105, qy - 2, 65, 18, hWnd, (HMENU)IDC_EDIT_ID_LAST_RX, hInst, NULL);
    app->hEditIdLastDel = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col2 + 175, qy - 2, 65, 18, hWnd, (HMENU)IDC_EDIT_ID_LAST_DEL, hInst, NULL);
    app->hEditIdRxAckTill = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col2 + 245, qy - 2, 65, 18, hWnd, (HMENU)IDC_EDIT_ID_RX_ACK_TILL, hInst, NULL);
    
    /* Normalizer - row 5 */
    qy += 28;
    CreateWindowExA(0, "STATIC", "Norm:",
        WS_CHILD | WS_VISIBLE, q_col1, qy, 40, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditNormInPkts = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 45, qy - 2, 40, 18, hWnd, (HMENU)IDC_EDIT_NORM_IN_PKTS, hInst, NULL);
    app->hEditNormInBytes = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 90, qy - 2, 55, 18, hWnd, (HMENU)IDC_EDIT_NORM_IN_BYTES, hInst, NULL);
    
    app->hEditNormOutPkts = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 150, qy - 2, 40, 18, hWnd, (HMENU)IDC_EDIT_NORM_OUT_PKTS, hInst, NULL);
    app->hEditNormOutBytes = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 195, qy - 2, 55, 18, hWnd, (HMENU)IDC_EDIT_NORM_OUT_BYTES, hInst, NULL);
    
    app->hEditNormAllocErr = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 255, qy - 2, 40, 18, hWnd, (HMENU)IDC_EDIT_NORM_ALLOC_ERR, hInst, NULL);
    app->hEditNormLogicErr = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 300, qy - 2, 40, 18, hWnd, (HMENU)IDC_EDIT_NORM_LOGIC_ERR, hInst, NULL);
    
    CreateWindowExA(0, "STATIC", "frag:",
        WS_CHILD | WS_VISIBLE, q_col1 + 345, qy, 30, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditNormFragSize = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 380, qy - 2, 40, 18, hWnd, (HMENU)IDC_EDIT_NORM_FRAG_SIZE, hInst, NULL);
    
    CreateWindowExA(0, "STATIC", "buf:",
        WS_CHILD | WS_VISIBLE, q_col1 + 425, qy, 30, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditNormDataPtr = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 460, qy - 2, 40, 18, hWnd, (HMENU)IDC_EDIT_NORM_DATA_PTR, hInst, NULL);
    app->hEditNormDataSize = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 505, qy - 2, 40, 18, hWnd, (HMENU)IDC_EDIT_NORM_DATA_SIZE, hInst, NULL);
    
    /* Normalizer cumulative totals - row 2 */
    qy += 25;
    CreateWindowExA(0, "STATIC", "NormTot:",
        WS_CHILD | WS_VISIBLE, q_col1, qy, 50, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditNormInTotPkts = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 55, qy - 2, 60, 18, hWnd, (HMENU)IDC_EDIT_NORM_IN_TOT_PKTS, hInst, NULL);
    app->hEditNormInTotBytes = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 120, qy - 2, 70, 18, hWnd, (HMENU)IDC_EDIT_NORM_IN_TOT_BYTES, hInst, NULL);
    
    app->hEditNormOutTotPkts = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 200, qy - 2, 60, 18, hWnd, (HMENU)IDC_EDIT_NORM_OUT_TOT_PKTS, hInst, NULL);
    app->hEditNormOutTotBytes = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 265, qy - 2, 70, 18, hWnd, (HMENU)IDC_EDIT_NORM_OUT_TOT_BYTES, hInst, NULL);

    /* ACK Debug section */
    qy += 25;
    CreateWindowExA(0, "STATIC", "ACK Debug:",
        WS_CHILD | WS_VISIBLE, q_col1, qy, 70, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    CreateWindowExA(0, "STATIC", "HitInf:",
        WS_CHILD | WS_VISIBLE, q_col1 + 80, qy, 40, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditAckHitInf = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 125, qy - 2, 60, 18, hWnd, (HMENU)IDC_EDIT_ACK_HIT_INF, hInst, NULL);
    CreateWindowExA(0, "STATIC", "HitSndQ:",
        WS_CHILD | WS_VISIBLE, q_col1 + 195, qy, 50, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditAckHitSndq = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col1 + 250, qy - 2, 60, 18, hWnd, (HMENU)IDC_EDIT_ACK_HIT_SNDQ, hInst, NULL);

    CreateWindowExA(0, "STATIC", "Miss:",
        WS_CHILD | WS_VISIBLE, q_col2 + 80, qy, 40, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditAckMiss = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col2 + 125, qy - 2, 60, 18, hWnd, (HMENU)IDC_EDIT_ACK_MISS, hInst, NULL);
    CreateWindowExA(0, "STATIC", "LnkWait:",
        WS_CHILD | WS_VISIBLE, q_col2 + 195, qy, 50, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditCntLinkWait = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col2 + 250, qy - 2, 60, 18, hWnd, (HMENU)IDC_EDIT_CNT_LINK_WAIT, hInst, NULL);

    CreateWindowExA(0, "STATIC", "TxState:",
        WS_CHILD | WS_VISIBLE, q_col3 + 80, qy, 40, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    app->hEditTxState = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        q_col3 + 125, qy - 2, 60, 18, hWnd, (HMENU)IDC_EDIT_TX_STATE, hInst, NULL);


    qy += 22;
    CreateWindowExA(0, "STATIC", "Debug:",
        WS_CHILD | WS_VISIBLE, q_col1 + 30, qy, 50, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
    for (int i = 0; i < 8; i++) {
        app->hEditDebug[i] = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
            q_col1 + 85 + i * 65, qy - 2, 60, 18, hWnd, (HMENU)((UINT_PTR)(IDC_EDIT_DEBUG_0 + i)), hInst, NULL);
    }

    /* ========================================================================
     * Router Congestion group — below Queues & Errors
     * ======================================================================== */
    {
        int ry = 1181;
        int r_col1 = 20, r_col2 = 260, r_col3 = 500, r_col4 = 740;
        int lw = 85, ew = 70;  /* label width, edit width */
        CreateWindowExA(0, "BUTTON", "Router Congestion",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, ry, WINDOW_WIDTH - 20, 160, hWnd, (HMENU)IDC_GB_ROUTER, hInst, NULL);
        /* Row 1 */
        int ry1 = ry + 22;
        CreateWindowExA(0, "STATIC", "Conns:", WS_CHILD | WS_VISIBLE, r_col1, ry1, 50, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
        app->hEditRtrConns = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER, r_col1 + 55, ry1 - 2, 50, 18, hWnd, (HMENU)IDC_EDIT_RTR_CONNS, hInst, NULL);
        CreateWindowExA(0, "STATIC", "Inflight:", WS_CHILD | WS_VISIBLE, r_col2, ry1, 55, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
        app->hEditRtrInflight = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER, r_col2 + 60, ry1 - 2, 70, 18, hWnd, (HMENU)IDC_EDIT_RTR_INFLIGHT, hInst, NULL);
        CreateWindowExA(0, "STATIC", "SendQ:", WS_CHILD | WS_VISIBLE, r_col3, ry1, 55, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
        app->hEditRtrSendQ = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER, r_col3 + 55, ry1 - 2, 70, 18, hWnd, (HMENU)IDC_EDIT_RTR_SENDQ, hInst, NULL);
        CreateWindowExA(0, "STATIC", "RecvQ:", WS_CHILD | WS_VISIBLE, r_col4, ry1, 55, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
        app->hEditRtrRecvQ = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER, r_col4 + 55, ry1 - 2, 70, 18, hWnd, (HMENU)IDC_EDIT_RTR_RECVQ, hInst, NULL);
        /* Row 2 */
        int ry2 = ry + 47;
        CreateWindowExA(0, "STATIC", "Pkts Sent:", WS_CHILD | WS_VISIBLE, r_col1, ry2, lw, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
        app->hEditRtrPktsSent = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER, r_col1 + lw, ry2 - 2, ew, 18, hWnd, (HMENU)IDC_EDIT_RTR_PKTS_SENT, hInst, NULL);
        CreateWindowExA(0, "STATIC", "Send Err:", WS_CHILD | WS_VISIBLE, r_col2, ry2, lw, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
        app->hEditRtrSendErr = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER, r_col2 + lw, ry2 - 2, ew, 18, hWnd, (HMENU)IDC_EDIT_RTR_SEND_ERR, hInst, NULL);
        CreateWindowExA(0, "STATIC", "Pkts Rcvd:", WS_CHILD | WS_VISIBLE, r_col3, ry2, lw, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
        app->hEditRtrPktsRcvd = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER, r_col3 + lw, ry2 - 2, ew, 18, hWnd, (HMENU)IDC_EDIT_RTR_PKTS_RCVD, hInst, NULL);
        CreateWindowExA(0, "STATIC", "SignFail:", WS_CHILD | WS_VISIBLE, r_col4, ry2, lw, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
        app->hEditRtrSignFail = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER, r_col4 + lw, ry2 - 2, ew, 18, hWnd, (HMENU)IDC_EDIT_RTR_SIGN_FAIL, hInst, NULL);
        /* Row 3 */
        int ry3 = ry + 72;
        CreateWindowExA(0, "STATIC", "ACK Sent:", WS_CHILD | WS_VISIBLE, r_col1, ry3, lw, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
        app->hEditRtrAckSent = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER, r_col1 + lw, ry3 - 2, ew, 18, hWnd, (HMENU)IDC_EDIT_RTR_ACK_SENT, hInst, NULL);
        CreateWindowExA(0, "STATIC", "ACK Rcvd:", WS_CHILD | WS_VISIBLE, r_col2, ry3, lw, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
        app->hEditRtrAckRecv = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER, r_col2 + lw, ry3 - 2, ew, 18, hWnd, (HMENU)IDC_EDIT_RTR_ACK_RECV, hInst, NULL);
        CreateWindowExA(0, "STATIC", "Stale ACK:", WS_CHILD | WS_VISIBLE, r_col3, ry3, lw, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
        app->hEditRtrStaleAck = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER, r_col3 + lw, ry3 - 2, ew, 18, hWnd, (HMENU)IDC_EDIT_RTR_STALE_ACK, hInst, NULL);
        /* Row 4 */
        int ry4 = ry + 97;
        CreateWindowExA(0, "STATIC", "Dup Drop:", WS_CHILD | WS_VISIBLE, r_col1, ry4, lw, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
        app->hEditRtrDupDrop = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER, r_col1 + lw, ry4 - 2, ew, 18, hWnd, (HMENU)IDC_EDIT_RTR_DUP_DROP, hInst, NULL);
        CreateWindowExA(0, "STATIC", "OOB Drop:", WS_CHILD | WS_VISIBLE, r_col2, ry4, lw, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
        app->hEditRtrOobDrop = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER, r_col2 + lw, ry4 - 2, ew, 18, hWnd, (HMENU)IDC_EDIT_RTR_OOB_DROP, hInst, NULL);
        /* Row 5: RTT / Jitter */
        int ry5 = ry + 122;
        CreateWindowExA(0, "STATIC", "RTT:", WS_CHILD | WS_VISIBLE, r_col1, ry5, 40, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
        app->hEditRtrRtt = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER, r_col1 + 45, ry5 - 2, 70, 18, hWnd, (HMENU)IDC_EDIT_RTR_RTT, hInst, NULL);
        CreateWindowExA(0, "STATIC", "Avg RTT:", WS_CHILD | WS_VISIBLE, r_col2, ry5, 65, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
        app->hEditRtrRttAvg = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER, r_col2 + 65, ry5 - 2, 70, 18, hWnd, (HMENU)IDC_EDIT_RTR_RTT_AVG, hInst, NULL);
        CreateWindowExA(0, "STATIC", "Jitter:", WS_CHILD | WS_VISIBLE, r_col3, ry5, 55, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
        app->hEditRtrJitter = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER, r_col3 + 55, ry5 - 2, 70, 18, hWnd, (HMENU)IDC_EDIT_RTR_JITTER, hInst, NULL);
        CreateWindowExA(0, "STATIC", "MinRTT:", WS_CHILD | WS_VISIBLE, r_col4, ry5, 60, 16, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
        app->hEditRtrMinrtt = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER, r_col4 + 60, ry5 - 2, 70, 18, hWnd, (HMENU)IDC_EDIT_RTR_MINRTT, hInst, NULL);
    }

    /* ========================================================================
     * Debug Levels section — right column
     * ======================================================================== */
    {
        int dy = 485;
        app->hDebugGroupBox = CreateWindowExA(0, "BUTTON", "Debug Levels",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            1005, dy, 240, 545, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
        dy += 20;
        const char* header_texts[6] = {
            "N\r\nO\r\nN\r\nE", "E\r\nR\r\nR\r\nO\r\nR", "W\r\nA\r\nR\r\nN",
            "I\r\nN\r\nF\r\nO", "D\r\nE\r\nB\r\nU\r\nG", "T\r\nR\r\nA\r\nC\r\nE"
        };
        app->debug_cat_count = 0;

        CreateWindowExA(0, "STATIC", "Global:", WS_CHILD | WS_VISIBLE,
            1015, dy + 2, 55, 20, hWnd, (HMENU)IDC_STATIC, hInst, NULL);
        for (int l = 0; l < 6; l++) {
            int lx = 1100 + l * 22;
            DWORD style = WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON;
            if (l == 0) style |= WS_GROUP;
            app->hDebugGlobalRadio[l] = CreateWindowExA(0, "BUTTON", "",
                style, lx, dy, 20, 20, hWnd, (HMENU)(UINT_PTR)(IDC_DEBUG_GLOBAL_BASE + l), hInst, NULL);
        }

        dy += 22;
        for (int l = 0; l < 6; l++) {
            int lx = 1100 + l * 22;
            app->hDebugHeaderEdit[l] = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", header_texts[l],
                WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER | ES_MULTILINE,
                lx, dy, 22, 60, hWnd, (HMENU)(UINT_PTR)(IDC_DEBUG_HEADER_BASE + l), hInst, NULL);
        }
    }

}
static void AddTooltip(HWND hTip, HWND hCtrl, const char* text)
{
    if (!hTip || !hCtrl || !text) return;
    TOOLINFOA ti;
    memset(&ti, 0, sizeof(ti));
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = GetParent(hCtrl);
    ti.uId = (UINT_PTR)hCtrl;
    ti.lpszText = (LPSTR)text;
    SendMessageA(hTip, TTM_ADDTOOL, 0, (LPARAM)&ti);
}
static void CreateTooltips(struct etcpmon_app* app)
{
    if (!app || !app->hWndMain) return;

    app->hToolTip = CreateWindowExA(
        WS_EX_TOPMOST, TOOLTIPS_CLASSA, NULL,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        app->hWndMain, NULL, app->hInstance, NULL
    );

    if (!app->hToolTip) return;

    SendMessageA(app->hToolTip, TTM_SETMAXTIPWIDTH, 0, 400);

    /* ETCP Metrics */
    AddTooltip(app->hToolTip, app->hEditEtcpRttLast,
        "etcp.rtt_last / ETCP Connection\r\nRound-trip time of the last packet (RTT), units 0.1 ms");
    AddTooltip(app->hToolTip, app->hEditEtcpRttAvg10,
        "etcp.rtt_avg_10 / ETCP Connection\r\nAverage RTT over last 10 packets, units 0.1 ms");
    AddTooltip(app->hToolTip, app->hEditEtcpJitter,
        "etcp.jitter / ETCP Connection\r\nRTT spread (max - min), units 0.1 ms");
    AddTooltip(app->hToolTip, app->hEditEtcpBytesSent,
        "etcp.bytes_sent_total / ETCP Connection\r\nTotal bytes sent since connection start");
    AddTooltip(app->hToolTip, app->hEditEtcpRetrans,
        "etcp.retrans_count / ETCP Connection\r\nNumber of retransmissions (resent packets)");
    AddTooltip(app->hToolTip, app->hEditEtcpRxDup,
        "etcp.rx_dup_count / ETCP Connection\r\nLocal received duplicate packets counter");
    AddTooltip(app->hToolTip, app->hEditEtcpTxDup,
        "etcp.tx_dup_count / ETCP Connection\r\nRemote duplicate packets counter (from ACK)");
    AddTooltip(app->hToolTip, app->hEditEtcpAcks,
        "etcp.ack_count / ETCP Connection\r\nNumber of received ACK packets (confirmations)");
    AddTooltip(app->hToolTip, app->hEditEtcpInflight,
        "etcp.unacked_bytes / ETCP Connection\r\nBytes in flight: sent but not yet acknowledged");
    AddTooltip(app->hToolTip, app->hEditEtcpLinks,
        "etcp.links_count / ETCP Connection\r\nNumber of active links (physical connections)");
    AddTooltip(app->hToolTip, app->hEditEtcpMaxInflight,
        "etcp.optimal_inflight / ETCP Max Inflight\r\nSum of all link inflight_lim_bytes (congestion window cap)");

    /* TUN Metrics */
    AddTooltip(app->hToolTip, app->hEditTunReadBytes,
        "tun.bytes_read / TUN Interface\r\nTotal bytes read from TUN interface");
    AddTooltip(app->hToolTip, app->hEditTunWriteBytes,
        "tun.bytes_written / TUN Interface\r\nTotal bytes written to TUN interface");
    AddTooltip(app->hToolTip, app->hEditTunReadPkts,
        "tun.packets_read / TUN Interface\r\nTotal packets read from TUN interface");
    AddTooltip(app->hToolTip, app->hEditTunWritePkts,
        "tun.packets_written / TUN Interface\r\nTotal packets written to TUN interface");
    AddTooltip(app->hToolTip, app->hEditTunReadErrs,
        "tun.read_errors / TUN Interface\r\nRead errors from TUN interface");
    AddTooltip(app->hToolTip, app->hEditTunWriteErrs,
        "tun.write_errors / TUN Interface\r\nWrite errors to TUN interface");

    /* Routing Metrics */
    AddTooltip(app->hToolTip, app->hEditRtRouted,
        "tun.routed_packets / Routing\r\nNumber of successfully routed packets");
    AddTooltip(app->hToolTip, app->hEditRtDropped,
        "tun.dropped_packets / Routing\r\nNumber of dropped (not routed) packets");
    AddTooltip(app->hToolTip, app->hEditRtTunInQPkts,
        "tun.tun_in_q_packets / Routing\r\nPackets in TUN input queue");
    AddTooltip(app->hToolTip, app->hEditRtTunInQBytes,
        "tun.tun_in_q_bytes / Routing\r\nBytes in TUN input queue");
    AddTooltip(app->hToolTip, app->hEditRtTunOutQPkts,
        "tun.tun_out_q_packets / Routing\r\nPackets in TUN output queue");
    AddTooltip(app->hToolTip, app->hEditRtTunOutQBytes,
        "tun.tun_out_q_bytes / Routing\r\nBytes in TUN output queue");
    AddTooltip(app->hToolTip, app->hEditRtCount,
        "tun.rt_count / Routing\r\nTotal entries in routing table");
    AddTooltip(app->hToolTip, app->hEditRtLocal,
        "tun.rt_local / Routing\r\nLocal (static) routes");
    AddTooltip(app->hToolTip, app->hEditRtLearned,
        "tun.rt_learned / Routing\r\nLearned (dynamic/BGP) routes");
    AddTooltip(app->hToolTip, app->hEditRtBgpSenders,
        "tun.rt_bgp_senders / Routing BGP\r\nNumber of BGP senders in list");
    AddTooltip(app->hToolTip, app->hEditRtBgpNodes,
        "tun.rt_bgp_nodes / Routing BGP\r\nNumber of BGP nodes");

    /* Queue Metrics */
    AddTooltip(app->hToolTip, app->hEditQInQBytes,
        "etcp.input_queue_bytes / ETCP Queue\r\nBytes in input queue (received from TUN, waiting to send)");
    AddTooltip(app->hToolTip, app->hEditQInQPkts,
        "etcp.input_queue_packets / ETCP Queue\r\nPackets in input queue");
    AddTooltip(app->hToolTip, app->hEditQInSendBytes,
        "etcp.input_send_q_bytes / ETCP Queue\r\nBytes in send queue (ready to send over links)");
    AddTooltip(app->hToolTip, app->hEditQInSendPkts,
        "etcp.input_send_q_packets / ETCP Queue\r\nPackets in send queue");
    AddTooltip(app->hToolTip, app->hEditQWaitAckBytes,
        "etcp.input_wait_ack_bytes / ETCP Queue\r\nBytes waiting for ACK (sent, awaiting confirmation)");
    AddTooltip(app->hToolTip, app->hEditQWaitAckPkts,
        "etcp.input_wait_ack_packets / ETCP Queue\r\nPackets waiting for ACK");
    AddTooltip(app->hToolTip, app->hEditQWaitAckOptInfl,
        "etcp.optimal_inflight / ETCP Congestion Control\r\nTarget (optimal) congestion window in bytes");
    AddTooltip(app->hToolTip, app->hEditQAckBytes,
        "etcp.ack_q_bytes / ETCP Queue\r\nBytes in ACK queue (acknowledgments ready to send)");
    AddTooltip(app->hToolTip, app->hEditQAckPkts,
        "etcp.ack_q_packets / ETCP Queue\r\nPackets in ACK queue");
    AddTooltip(app->hToolTip, app->hEditQRecvBytes,
        "etcp.recv_q_bytes / ETCP Queue\r\nBytes in receive queue (received, awaiting delivery to TUN)");
    AddTooltip(app->hToolTip, app->hEditQRecvPkts,
        "etcp.recv_q_packets / ETCP Queue\r\nPackets in receive queue");
    AddTooltip(app->hToolTip, app->hEditQOutBytes,
        "etcp.output_queue_bytes / ETCP Queue\r\nBytes in output queue (ready to write to TUN)");
    AddTooltip(app->hToolTip, app->hEditQOutPkts,
        "etcp.output_queue_packets / ETCP Queue\r\nPackets in output queue");

    /* Error Counters */
    AddTooltip(app->hToolTip, app->hEditErrReinit,
        "etcp.reinit_count / ETCP Errors\r\nConnection reinitialization counter");
    AddTooltip(app->hToolTip, app->hEditErrReset,
        "etcp.reset_count / ETCP Errors\r\nConnection reset counter");
    AddTooltip(app->hToolTip, app->hEditErrPktFmt,
        "etcp.pkt_format_errors / ETCP Errors\r\nPacket format errors (invalid headers)");

    /* Timer Flags */
    AddTooltip(app->hToolTip, app->hEditTimerRetrans,
        "etcp.retrans_timer_active / ETCP Timers\r\nRetransmission timer active (ON/OFF)");
    AddTooltip(app->hToolTip, app->hEditTimerAckResp,
        "etcp.ack_resp_timer_active / ETCP Timers\r\nDelayed ACK response timer active");

    /* WaitAck queue state */
    AddTooltip(app->hToolTip, app->hEditWaitAckSuspend,
        "etcp.wait_ack_cb_suspended / ETCP WaitAck\r\nCallback suspended (waiting for ACK blocks transmission)");
    AddTooltip(app->hToolTip, app->hEditWaitAckCb,
        "etcp.wait_ack_cb_set / ETCP WaitAck\r\nCallback set (waiting-for-ACK handler exists)");
    AddTooltip(app->hToolTip, app->hEditWaitAckTimer,
        "etcp.wait_ack_resume_timeout / ETCP WaitAck\r\nResume transmission timer active");

    /* ID Metrics */
    AddTooltip(app->hToolTip, app->hEditIdNextTx,
        "etcp.next_tx_id / ETCP Sequence\r\nNext transmit ID (TX Sequence Number)");
    AddTooltip(app->hToolTip, app->hEditIdLastRx,
        "etcp.last_rx_id / ETCP Sequence\r\nLast received ID (RX Sequence Number)");
    AddTooltip(app->hToolTip, app->hEditIdLastDel,
        "etcp.last_delivered_id / ETCP Sequence\r\nLast delivered ID (to TUN)");
    AddTooltip(app->hToolTip, app->hEditIdRxAckTill,
        "etcp.rx_ack_till / ETCP Sequence\r\nUp to which ID reception is confirmed (ACK from peer)");

    /* Normalizer */
    AddTooltip(app->hToolTip, app->hEditNormInPkts,
        "etcp.norm_input_pkts / Packet Normalizer\r\nPackets at normalizer input (current)");
    AddTooltip(app->hToolTip, app->hEditNormInBytes,
        "etcp.norm_input_bytes / Packet Normalizer\r\nBytes at normalizer input (current)");
    AddTooltip(app->hToolTip, app->hEditNormOutPkts,
        "etcp.norm_output_pkts / Packet Normalizer\r\nPackets at normalizer output (current)");
    AddTooltip(app->hToolTip, app->hEditNormOutBytes,
        "etcp.norm_output_bytes / Packet Normalizer\r\nBytes at normalizer output (current)");
    AddTooltip(app->hToolTip, app->hEditNormAllocErr,
        "etcp.norm_alloc_errors / Packet Normalizer\r\nMemory allocation errors in normalizer");
    AddTooltip(app->hToolTip, app->hEditNormLogicErr,
        "etcp.norm_logic_errors / Packet Normalizer\r\nLogic errors in normalizer");
    AddTooltip(app->hToolTip, app->hEditNormFragSize,
        "etcp.norm_frag_size / Packet Normalizer\r\nCurrent fragment size");
    AddTooltip(app->hToolTip, app->hEditNormDataPtr,
        "etcp.norm_data_ptr / Packet Normalizer\r\nData pointer in buffer");
    AddTooltip(app->hToolTip, app->hEditNormDataSize,
        "etcp.norm_data_size / Packet Normalizer\r\nData size in buffer");
    AddTooltip(app->hToolTip, app->hEditNormInTotPkts,
        "etcp.norm_in_total_pkts / Packet Normalizer\r\nTotal packets processed at input (cumulative)");
    AddTooltip(app->hToolTip, app->hEditNormInTotBytes,
        "etcp.norm_in_total_bytes / Packet Normalizer\r\nTotal bytes processed at input (cumulative)");
    AddTooltip(app->hToolTip, app->hEditNormOutTotPkts,
        "etcp.norm_out_total_pkts / Packet Normalizer\r\nTotal packets processed at output (cumulative)");
    AddTooltip(app->hToolTip, app->hEditNormOutTotBytes,
        "etcp.norm_out_total_bytes / Packet Normalizer\r\nTotal bytes processed at output (cumulative)");

    /* ACK Debug counters */
    AddTooltip(app->hToolTip, app->hEditAckHitInf,
        "etcp.cnt_ack_hit_inf / ETCP ACK Debug\r\nACK hit inflight window (expected ACK)");
    AddTooltip(app->hToolTip, app->hEditAckHitSndq,
        "etcp.cnt_ack_hit_sndq / ETCP ACK Debug\r\nACK hit send queue");
    AddTooltip(app->hToolTip, app->hEditAckMiss,
        "etcp.cnt_ack_miss / ETCP ACK Debug\r\nACK not found (unexpected ACK)");
    AddTooltip(app->hToolTip, app->hEditCntLinkWait,
        "etcp.cnt_link_wait / ETCP ACK Debug\r\nLink wait counter");
    AddTooltip(app->hToolTip, app->hEditTxState,
        "etcp.tx_state / ETCP ACK Debug\r\nTransmission state (bitmask)");

    /* Debug counters */
    for (int i = 0; i < 8; i++) {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "etcp.debug[%d] / ETCP Debug\r\nDebug field (version dependent)", i);
        AddTooltip(app->hToolTip, app->hEditDebug[i], buf);
    }

    /* System resources */
    AddTooltip(app->hToolTip, app->hEditActiveTimeouts,
        "etcp.active_timeouts / System Resources\r\nActive timeouts in system (async)");
    AddTooltip(app->hToolTip, app->hEditBusyMemory,
        "etcp.busy_memory_blocks / System Resources\r\nBusy memory blocks (memory pool)");
}
static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    struct etcpmon_app* app = NULL;
    
    if (message == WM_CREATE) {
        CREATESTRUCTA* cs = (CREATESTRUCTA*)lParam;
        app = (struct etcpmon_app*)cs->lpCreateParams;
        SetWindowLongPtrA(hWnd, GWLP_USERDATA, (LONG_PTR)app);
    } else {
        app = (struct etcpmon_app*)GetWindowLongPtrA(hWnd, GWLP_USERDATA);
    }
    
    switch (message) {
        case WM_COMMAND:
            if (app) {
                WORD ctrl_id = LOWORD(wParam);
                if ((ctrl_id >= IDC_DEBUG_GLOBAL_BASE && ctrl_id <= IDC_DEBUG_GLOBAL_BASE + 5) ||
                    (ctrl_id >= IDC_DEBUG_ROW_BASE && ctrl_id < IDC_DEBUG_ROW_BASE + IDC_DEBUG_MAX_CAT * 8)) {
                    OnDebugRadio(app, ctrl_id);
                } else {
                    switch (ctrl_id) {
                        case IDC_BTN_CONNECT:
                            OnConnect(app);
                            break;
                        case IDC_BTN_DISCONNECT:
                            OnDisconnect(app);
                            break;
                        case IDC_LIST_CONNECTIONS:
                            if (HIWORD(wParam) == LBN_SELCHANGE) {
                                OnConnectionSelect(app);
                            }
                            break;
                        case IDC_BTN_ACTION:
                            OnAction(app);
                            break;
                    }
                }
            }
            break;
            
        case WM_SIZE:
            if (app && app->hWndStatus) {
                SendMessage(app->hWndStatus, WM_SIZE, 0, 0);
            }
            break;
        case WM_TIMER:
            if (wParam == IDC_TIMER_UPDATE && app) {
                OnTimer(app);
                return 0;
            }
            break;
            
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
            
        default:
            return DefWindowProcA(hWnd, message, wParam, lParam);
    }
    
    return 0;
}
static void OnConnect(struct etcpmon_app* app) {
    if (!app || !app->client) return;
    
    char addr[64];
    char port_str[16];
    
    GetDlgItemTextA(app->hWndMain, IDC_EDIT_ADDR, addr, sizeof(addr));
    GetDlgItemTextA(app->hWndMain, IDC_EDIT_PORT, port_str, sizeof(port_str));
    
    uint16_t port = (uint16_t)atoi(port_str);
    if (port == 0) port = 9090;
    
    etcpmon_gui_set_status(app, "Connecting...");
    
    if (etcpmon_client_connect(app->client, addr, port) == 0) {
        app->isConnected = 1;
        app->need_initial_request = 1;
        UpdateUIState(app);
    } else {
        etcpmon_gui_set_status(app, "Connection failed");
    }
}
static void OnDisconnect(struct etcpmon_app* app) {
    if (!app || !app->client) return;
    
    etcpmon_client_disconnect(app->client);
    on_disconnected(app);
}

static void OnConnectionSelect(struct etcpmon_app* app) {
    if (!app || !app->hListConnections || !app->client) return;
    
    int idx = (int)SendMessage(app->hListConnections, LB_GETCURSEL, 0, 0);
    if (idx == LB_ERR) return;
    
    LRESULT list_idx = SendMessage(app->hListConnections, LB_GETITEMDATA, idx, 0);
    if (list_idx < 0 || list_idx >= app->client->conn_count) return;
    uint64_t peer_id = app->client->conn_list[list_idx].peer_node_id;
    if (peer_id == 0) return;
    
    /* Только если действительно изменилось — иначе спамим сервер */
    if (peer_id != app->last_selected_peer_id) {
        if (etcpmon_client_select_connection(app->client, peer_id) != 0) {
            OnDisconnect(app);
            etcpmon_gui_set_status(app, "Failed to select connection");
            return;
        }
        app->last_selected_peer_id = peer_id;
        etcpmon_gui_clear_metrics(app);   /* очищаем старые цифры */
        
        etcpmon_gui_set_status(app, "Connection selected");
    }
}

static void OnAction(struct etcpmon_app* app) {
    if (!app || !app->client || !app->hEditAction) return;

    char action[33];
    GetWindowTextA(app->hEditAction, action, sizeof(action));
    action[sizeof(action) - 1] = '\0';

    if (strlen(action) > 0) {
        if (etcpmon_client_send_action(app->client, action) == 0) SetWindowTextA(app->hEditAction, "");
        else etcpmon_gui_set_status(app, "Failed to queue Action");
    }
}

static void OnDebugRadio(struct etcpmon_app* app, int control_id) {
    static int in_handler = 0;
    if (in_handler) return;
    if (!app || !app->client) return;
    in_handler = 1;

    if (control_id >= IDC_DEBUG_GLOBAL_BASE && control_id <= IDC_DEBUG_GLOBAL_BASE + 5) {
        int level = control_id - IDC_DEBUG_GLOBAL_BASE;
        for (int l = 0; l <= 5; l++)
            SendMessage(app->hDebugGlobalRadio[l], BM_SETCHECK, (l == level) ? BST_CHECKED : BST_UNCHECKED, 0);
        app->client->debug_global_level = (uint8_t)level;
    } else if (control_id >= IDC_DEBUG_ROW_BASE) {
        int flat = control_id - IDC_DEBUG_ROW_BASE;
        int row = flat / 8;
        int level = flat % 8;
        if (row >= app->debug_cat_count || level > 5) { in_handler = 0; return; }
        for (int l = 0; l <= 5; l++) {
            if (app->hDebugCatRadio[row][l])
                SendMessage(app->hDebugCatRadio[row][l], BM_SETCHECK, (l == level) ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        app->client->debug_category_levels[row] = (uint8_t)level;
    }

    etcpmon_client_send_debug_config(app->client, app->client->debug_global_level,
                                      app->debug_cat_count, app->client->debug_category_levels);
    in_handler = 0;
}

static void RecreateDebugCats(struct etcpmon_app* app) {
    if (!app || !app->client || !app->hWndMain) return;
    struct etcpmon_client* cl = app->client;
    if (cl->debug_category_count == 0) return;

    HINSTANCE hInst = app->hInstance;
    HWND hWnd = app->hWndMain;
    int ncat = cl->debug_category_count;
    if (ncat > IDC_DEBUG_MAX_CAT) ncat = IDC_DEBUG_MAX_CAT;
    app->debug_cat_count = ncat;

    int dy = 485 + 20 + 22 + 65; // GroupBox top + global row + header row + header height
    const char* names = cl->debug_category_names;
    const char* p = names;
    for (int r = 0; r < ncat; r++) {
        const char* end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char buf[64];
        size_t copy = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
        memcpy(buf, p, copy); buf[copy] = '\0';

        app->hDebugCatLabel[r] = CreateWindowExA(0, "STATIC", buf,
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
            1015, dy + 1, 80, 16, hWnd, (HMENU)(UINT_PTR)(IDC_DEBUG_NAME_BASE + r), hInst, NULL);
        for (int l = 0; l < 6; l++) {
            int lx = 1100 + l * 22;
            DWORD style = WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON;
            if (l == 0) style |= WS_GROUP;
            app->hDebugCatRadio[r][l] = CreateWindowExA(0, "BUTTON", "",
                style, lx, dy, 20, 16, hWnd, (HMENU)(UINT_PTR)(IDC_DEBUG_ROW_BASE + r * 8 + l), hInst, NULL);
        }
        dy += 18;

        p = end ? end + 1 : p + len;
    }
}

static void RefreshDebugUI(struct etcpmon_app* app) {
    if (!app || !app->client) return;
    struct etcpmon_client* cl = app->client;

    if (app->debug_cat_count == 0 && cl->debug_category_count > 0)
        RecreateDebugCats(app);

    for (int l = 0; l <= 5; l++) {
        if (app->hDebugGlobalRadio[l])
            SendMessage(app->hDebugGlobalRadio[l], BM_SETCHECK,
                       (l == cl->debug_global_level) ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    int n = cl->debug_category_count;
    if (n > app->debug_cat_count) n = app->debug_cat_count;
    for (int r = 0; r < n; r++) {
        int level = cl->debug_category_levels[r];
        if (level > 5) level = 0;
        for (int l = 0; l <= 5; l++) {
            if (app->hDebugCatRadio[r][l])
                SendMessage(app->hDebugCatRadio[r][l], BM_SETCHECK,
                           (l == level) ? BST_CHECKED : BST_UNCHECKED, 0);
        }
    }
}

static void OnTimer(struct etcpmon_app* app) {
    if (!app || !app->client) return;
    if (!app->client->connected) return;

    int processed = etcpmon_client_process(app->client);
    if (processed < 0) {
        OnDisconnect(app);          // если связь оборвалась
        return;
    }

    if (!app->client->fully_connected) return;

    DWORD now = GetTickCount();
    if (app->need_initial_request || now - app->last_poll_time >= LIST_UPDATE_INTERVAL) {
        if (etcpmon_client_request_list(app->client) != 0 || etcpmon_client_request_socket_list(app->client) != 0 ||
            (app->need_initial_request && etcpmon_client_request_debug_config(app->client) != 0)) {
            OnDisconnect(app);
            etcpmon_gui_set_status(app, "Failed to request monitor data");
            return;
        }
        app->last_poll_time = now;
        app->need_initial_request = 0;
    }

    if (app->isConnected && app->client->debug_config_dirty) {
        RefreshDebugUI(app);
        app->client->debug_config_dirty = 0;
    }

}

static void UpdateUIState(struct etcpmon_app* app) {
    if (!app) return;
    
    EnableWindow(app->hEditAddr, !app->isConnected);
    EnableWindow(app->hEditPort, !app->isConnected);
    EnableWindow(app->hBtnConnect, !app->isConnected);
    EnableWindow(app->hBtnDisconnect, app->isConnected);
}

/* Client callbacks */
static void on_connected(void* user_data) {
    struct etcpmon_app* app = (struct etcpmon_app*)user_data;
    
    app->isConnected = 1;
    etcpmon_gui_set_status(app, "Connected to server");
    
}

void etcpmon_gui_update_graph(struct etcpmon_app* app, struct etcpmon_rsp_metrics* metrics) {
    if (!app || !metrics) return;
    
    if (app->hGraphWnd) {
        InvalidateRect(app->hGraphWnd, NULL, FALSE);
    }
    
    if (!app->graph_cursor_active && app->client) {
        struct metrics_history* h = etcpmon_client_get_history(app->client);
        if (h && h->count > 0) {
            int last_idx = (h->head - 1 + GRAPH_HISTORY_SIZE) % GRAPH_HISTORY_SIZE;
            UpdateChannelValues(app, last_idx);
        }
    }
}
static void on_disconnected(void* user_data) {
    struct etcpmon_app* app = (struct etcpmon_app*)user_data;
    app->isConnected = 0;
    app->need_initial_request = 1;
    app->last_selected_peer_id = 0;
    app->last_poll_time = 0;
    SendMessage(app->hListConnections, LB_RESETCONTENT, 0, 0);
    SendMessage(app->hListSockets, LB_RESETCONTENT, 0, 0);
    etcpmon_gui_clear_metrics(app);
    UpdateUIState(app);
    etcpmon_gui_set_status(app, "Disconnected");
}

static void on_conn_list(struct etcpmon_conn_info* list, uint8_t count, void* user_data) {
    struct etcpmon_app* app = (struct etcpmon_app*)user_data;

    etcpmon_gui_update_conn_list(app, list, count);

    if (count > 0) {
        OnConnectionSelect(app);
    } else if (app->last_selected_peer_id != 0) {
        if (etcpmon_client_select_connection(app->client, 0) != 0) {
            OnDisconnect(app);
            return;
        }
        app->last_selected_peer_id = 0;
        etcpmon_gui_clear_metrics(app);
        etcpmon_gui_set_status(app, "No connections");
    }
}

static void on_socket_list(struct etcpmon_socket_info* list, uint8_t count, void* user_data) {
    struct etcpmon_app* app = (struct etcpmon_app*)user_data;
    etcpmon_gui_update_socket_list(app, list, count);
}

static void on_metrics(struct etcpmon_rsp_metrics* metrics, 
                      struct etcpmon_link_metrics* links, 
                      uint8_t links_count, void* user_data) {
    struct etcpmon_app* app = (struct etcpmon_app*)user_data;
    if (app) {
        etcpmon_gui_update_metrics(app, metrics, links, links_count);
        if (!app->graph_cursor_active) {
            struct metrics_history* h = etcpmon_client_get_history(app->client);
            if (h && h->count > 0) {
                int last_idx = (h->head - 1 + GRAPH_HISTORY_SIZE) % GRAPH_HISTORY_SIZE;
                UpdateChannelValues(app, last_idx);
            }
        }
    }
}

static void on_error(const char* msg, void* user_data) {
    struct etcpmon_app* app = (struct etcpmon_app*)user_data;
    etcpmon_gui_set_status(app, msg);
}

static LRESULT CALLBACK ActionResultWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            HWND hEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
                0, 0, 0, 0, hWnd, (HMENU)1, ((CREATESTRUCTA*)lParam)->hInstance, NULL);
            HDC hdc = GetDC(hWnd);
            int logpix = GetDeviceCaps(hdc, LOGPIXELSY);
            ReleaseDC(hWnd, hdc);
            HFONT hFont = CreateFontA(-MulDiv(10, logpix, 72),
                0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN,
                "Consolas");
            SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            SetWindowLongPtrA(hWnd, GWLP_USERDATA, (LONG_PTR)hEdit);
            SetPropA(hWnd, "Font", (HANDLE)hFont);
            break;
        }
        case WM_SIZE:
            if (GetWindowLongPtrA(hWnd, GWLP_USERDATA))
                MoveWindow((HWND)GetWindowLongPtrA(hWnd, GWLP_USERDATA), 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
            break;
        case WM_CLOSE:
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        case WM_DESTROY: {
            HFONT hFont = (HFONT)GetPropA(hWnd, "Font");
            if (hFont) DeleteObject(hFont);
            RemovePropA(hWnd, "Font");
            break;
        }
        default:
            return DefWindowProcA(hWnd, message, wParam, lParam);
    }
    return 0;
}

static void on_action_result(const char* text, void* user_data) {
    struct etcpmon_app* app = (struct etcpmon_app*)user_data;
    if (!app || !app->hWndMain || !text || !text[0]) return;

    if (!IsWindow(app->hActionResultWnd)) {
        RECT rc;
        GetWindowRect(app->hWndMain, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        app->hActionResultWnd = CreateWindowExA(0, "ETCPMonActionResult", "Action Result",
            WS_OVERLAPPEDWINDOW | WS_SIZEBOX,
            rc.left, rc.top, w, h,
            NULL, NULL, app->hInstance, NULL);

        app->hActionResultEdit = (HWND)GetWindowLongPtrA(app->hActionResultWnd, GWLP_USERDATA);
    }
    ShowWindow(app->hActionResultWnd, SW_RESTORE);
    SetForegroundWindow(app->hActionResultWnd);
    UpdateWindow(app->hActionResultWnd);

    if (IsWindow(app->hActionResultEdit)) {
        size_t tlen = strlen(text);
        char* crlf = (char*)malloc(tlen * 2 + 1);
        if (crlf) {
            size_t out = 0;
            for (size_t i = 0; i < tlen; i++) {
                if (text[i] == '\n') crlf[out++] = '\r';
                crlf[out++] = text[i];
            }
            crlf[out] = '\0';
            SendMessageA(app->hActionResultEdit, EM_SETSEL, -1, -1);
            SendMessageA(app->hActionResultEdit, EM_REPLACESEL, FALSE, (LPARAM)crlf);
            free(crlf);
        }

        int lines = (int)SendMessage(app->hActionResultEdit, EM_GETLINECOUNT, 0, 0);
        SendMessage(app->hActionResultEdit, EM_LINESCROLL, 0, lines);
    }
}
int etcpmon_gui_run(struct etcpmon_app* app) {
    if (!app || !app->hWndMain) return -1;
    ShowWindow(app->hWndMain, SW_SHOW);
    UpdateWindow(app->hWndMain);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN && app) {
            HWND hFocus = GetFocus();
            if (hFocus == app->hEditAddr || hFocus == app->hEditPort) {
                if (msg.wParam == VK_TAB) {
                    if (hFocus == app->hEditAddr) {
                        SetFocus(app->hEditPort);
                        int len = GetWindowTextLengthA(app->hEditPort);
                        SendMessageA(app->hEditPort, EM_SETSEL, len, len);
                    } else {
                        SetFocus(app->hEditAddr);
                        int len = GetWindowTextLengthA(app->hEditAddr);
                        SendMessageA(app->hEditAddr, EM_SETSEL, len, len);
                    }
                    continue;
                }
                if (msg.wParam == VK_RETURN) {
                    if (!app->isConnected) OnConnect(app);
                    continue;
                }
                if (msg.wParam == VK_ESCAPE) {
                    if (app->isConnected) OnDisconnect(app);
                    continue;
                }
            }
            if (hFocus == app->hEditAction) {
                if (msg.wParam == VK_RETURN) {
                    OnAction(app);
                    continue;
                }
            }
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
void etcpmon_gui_cleanup(struct etcpmon_app* app) {
    if (!app) return;

    if (app->hToolTip) {
        DestroyWindow(app->hToolTip);
        app->hToolTip = NULL;
    }

    for (int i = 0; i < GRAPH_METRICS_COUNT; i++) {
        if (app->hGraphPens[i]) DeleteObject(app->hGraphPens[i]);
    }
    if (app->client) {
        etcpmon_client_cleanup(app->client);
        free(app->client);
        app->client = NULL;
    }
    WSACleanup();
    g_app = NULL;
}
void etcpmon_gui_set_status(struct etcpmon_app* app, const char* text) {
    if (!app || !app->hWndStatus) return;
    SendMessageA(app->hWndStatus, SB_SETTEXTA, 0, (LPARAM)text);
}
void etcpmon_gui_update_conn_list(struct etcpmon_app* app,
                                   struct etcpmon_conn_info* list,
                                   uint8_t count) {
    if (!app || !app->hListConnections) return;

    int selected = count ? 0 : -1;
    int changed = SendMessage(app->hListConnections, LB_GETCOUNT, 0, 0) != count;
    for (uint8_t i = 0; i < count; i++) {
        if (list[i].peer_node_id == app->last_selected_peer_id) selected = i;
        char display[64], previous[64];
        snprintf(display, sizeof(display), "%s (%016llX)", list[i].name, (unsigned long long)list[i].peer_node_id);
        if (!changed && (SendMessage(app->hListConnections, LB_GETTEXTLEN, i, 0) >= (LRESULT)sizeof(previous) ||
            SendMessageA(app->hListConnections, LB_GETTEXT, i, (LPARAM)previous) == LB_ERR || strcmp(display, previous) != 0)) {
            changed = 1;
        }
    }
    if (!changed) {
        SendMessage(app->hListConnections, LB_SETCURSEL, selected, 0);
        return;
    }

    int top = (int)SendMessage(app->hListConnections, LB_GETTOPINDEX, 0, 0);
    SendMessage(app->hListConnections, WM_SETREDRAW, FALSE, 0);
    SendMessage(app->hListConnections, LB_RESETCONTENT, 0, 0);

    /* Add new items */
    for (uint8_t i = 0; i < count; i++) {
        char display[64];
        snprintf(display, sizeof(display), "%s (%016llX)",
                list[i].name, (unsigned long long)list[i].peer_node_id);

        int idx = (int)SendMessageA(app->hListConnections, LB_ADDSTRING, 0, (LPARAM)display);
        if (idx != LB_ERR && idx != LB_ERRSPACE) {
            /* Индекс сохраняет полный 64-битный ID и в 32-битной Windows. */
            SendMessage(app->hListConnections, LB_SETITEMDATA, idx, i);
        } else if (app->client && app->client->log_file) {
            fprintf(app->client->log_file, "[ERROR] Failed to add connection row: index=%u result=%d\n", i, idx);
        }
    }
    SendMessage(app->hListConnections, LB_SETCURSEL, selected, 0);
    if (top >= 0 && top < count) SendMessage(app->hListConnections, LB_SETTOPINDEX, top, 0);
    SendMessage(app->hListConnections, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(app->hListConnections, NULL, TRUE);
    if (app->client && app->client->log_file) {
        fprintf(app->client->log_file, "[LOG] Connections updated: count=%u selected_row=%d previous_peer=%016llX\n",
                count, selected, (unsigned long long)app->last_selected_peer_id);
        fflush(app->client->log_file);
    }
}

static const char* config_type_to_string(uint8_t config_type) {
    switch (config_type) {
        case CFG_SERVER_TYPE_PUBLIC:    return "PUBLIC";
        case CFG_SERVER_TYPE_NAT:       return "NAT";
        case CFG_SERVER_TYPE_PRIVATE:   return "Private";
        case CFG_SERVER_TYPE_LOCAL:     return "Local";
        case CFG_SERVER_TYPE_UNKNOWN:   return "Unknown";
        default:                        return "?";
    }
}


void etcpmon_gui_update_socket_list(struct etcpmon_app* app,
                                     struct etcpmon_socket_info* list,
                                     uint8_t count) {
    if (!app || !app->hListSockets) return;

    SendMessage(app->hListSockets, LB_RESETCONTENT, 0, 0);

    for (uint8_t i = 0; i < count; i++) {
        if (list[i].ip[0] == '\0') continue;
        if (list[i].name[0] == '\0') continue;

        char display[256];
        snprintf(display, sizeof(display), "%s%s%s:%u | %s policy=%s | %s mapping=%s inbound=%s | external=%s:%u",
                 list[i].is_ipv6 ? "[" : "", list[i].ip, list[i].is_ipv6 ? "]" : "", list[i].port,
                 list[i].is_tcp ? "TCP" : "UDP", config_type_to_string(list[i].config_type),
                 address_profile_source_name(list[i].source),
                 address_mapping_name(list[i].mapping), address_inbound_name(list[i].inbound),
                 list[i].nat_ip[0] ? list[i].nat_ip : "unknown", list[i].nat_port);
        SendMessageA(app->hListSockets, LB_ADDSTRING, 0, (LPARAM)display);
    }
}
/* Обновляет EDIT только если текст действительно изменился → убирает моргание */
static BOOL UpdateEditIfChanged(HWND hDlg, int nIDDlgItem, const char* fmt, ...)
{
    char new_text[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(new_text, sizeof(new_text), fmt, args);
    va_end(args);
    char old_text[128] = {0};
    GetDlgItemTextA(hDlg, nIDDlgItem, old_text, sizeof(old_text));
    if (strcmp(old_text, new_text) == 0)
        return FALSE;
    SetDlgItemTextA(hDlg, nIDDlgItem, new_text);
    InvalidateRect(GetDlgItem(hDlg, nIDDlgItem), NULL, FALSE);
    return TRUE;
}
void etcpmon_gui_update_metrics(struct etcpmon_app* app,
                                 struct etcpmon_rsp_metrics* metrics,
                                 struct etcpmon_link_metrics* links,
                                 uint8_t links_count)
{
    if (!app || !metrics) return;
    HWND hMain = app->hWndMain;
    /* ETCP Metrics — обновляем ТОЛЬКО при изменении */
    UpdateEditIfChanged(hMain, IDC_EDIT_ETCP_RTT_LAST,   "%.1f ms", metrics->etcp.rtt_last/10.f);
    UpdateEditIfChanged(hMain, IDC_EDIT_ETCP_RTT_AVG10,  "%.1f ms", metrics->etcp.rtt_avg_10/10.f);
    UpdateEditIfChanged(hMain, IDC_EDIT_ETCP_JITTER,     "%.1f ms", metrics->etcp.jitter/10.f);
    UpdateEditIfChanged(hMain, IDC_EDIT_ETCP_BYTES_SENT, "%llu",  (unsigned long long)metrics->etcp.bytes_sent_total);
    UpdateEditIfChanged(hMain, IDC_EDIT_ETCP_RETRANS,    "%u",    metrics->etcp.retrans_count);
    UpdateEditIfChanged(hMain, IDC_EDIT_ETCP_RX_DUP,     "%u",    metrics->etcp.rx_dup_count);
    UpdateEditIfChanged(hMain, IDC_EDIT_ETCP_TX_DUP,     "%u",    metrics->etcp.tx_dup_count);
    UpdateEditIfChanged(hMain, IDC_EDIT_ETCP_ACKS,       "%u",    metrics->etcp.ack_count);
    UpdateEditIfChanged(hMain, IDC_EDIT_ETCP_INFLIGHT,   "%u bytes", metrics->etcp.unacked_bytes);
    UpdateEditIfChanged(hMain, IDC_EDIT_ETCP_MAX_INFLIGHT, "%u bytes", metrics->etcp.optimal_inflight);
    UpdateEditIfChanged(hMain, IDC_EDIT_ETCP_LINKS,      "%u",    metrics->etcp.links_count);
    /* TUN Metrics */
    UpdateEditIfChanged(hMain, IDC_EDIT_TUN_READ_BYTES,  "%llu", (unsigned long long)metrics->tun.bytes_read);
    UpdateEditIfChanged(hMain, IDC_EDIT_TUN_WRITE_BYTES, "%llu", (unsigned long long)metrics->tun.bytes_written);
    UpdateEditIfChanged(hMain, IDC_EDIT_TUN_READ_PKTS,   "%u",   metrics->tun.packets_read);
    UpdateEditIfChanged(hMain, IDC_EDIT_TUN_WRITE_PKTS,  "%u",   metrics->tun.packets_written);
    UpdateEditIfChanged(hMain, IDC_EDIT_TUN_READ_ERRS,   "%u",   metrics->tun.read_errors);
    UpdateEditIfChanged(hMain, IDC_EDIT_TUN_WRITE_ERRS,  "%u",   metrics->tun.write_errors);
    
    /* Routing Metrics */
    UpdateEditIfChanged(hMain, IDC_EDIT_RT_ROUTED,     "%llu", (unsigned long long)metrics->tun.routed_packets);
    UpdateEditIfChanged(hMain, IDC_EDIT_RT_DROPPED,    "%llu", (unsigned long long)metrics->tun.dropped_packets);
    UpdateEditIfChanged(hMain, IDC_EDIT_RT_TUN_IN_Q_PKTS,  "%u", metrics->tun.tun_in_q_packets);
    UpdateEditIfChanged(hMain, IDC_EDIT_RT_TUN_IN_Q_BYTES, "%u", metrics->tun.tun_in_q_bytes);
    UpdateEditIfChanged(hMain, IDC_EDIT_RT_TUN_OUT_Q_PKTS, "%u", metrics->tun.tun_out_q_packets);
    UpdateEditIfChanged(hMain, IDC_EDIT_RT_TUN_OUT_Q_BYTES, "%u", metrics->tun.tun_out_q_bytes);
    UpdateEditIfChanged(hMain, IDC_EDIT_RT_COUNT,      "%u", metrics->tun.rt_count);
    UpdateEditIfChanged(hMain, IDC_EDIT_RT_LOCAL,      "%u", metrics->tun.rt_local);
    UpdateEditIfChanged(hMain, IDC_EDIT_RT_LEARNED,    "%u", metrics->tun.rt_learned);
    UpdateEditIfChanged(hMain, IDC_EDIT_RT_BGP_SENDERS, "%u", metrics->tun.rt_bgp_senders);
    UpdateEditIfChanged(hMain, IDC_EDIT_RT_BGP_NODES,   "%u", metrics->tun.rt_bgp_nodes);
    
    /* Router Congestion Metrics */
    UpdateEditIfChanged(hMain, IDC_EDIT_RTR_CONNS,      "%u", metrics->router.total_conns);
    UpdateEditIfChanged(hMain, IDC_EDIT_RTR_INFLIGHT,   "%u", metrics->router.total_inflight);
    UpdateEditIfChanged(hMain, IDC_EDIT_RTR_SENDQ,      "%u", metrics->router.total_send_q);
    UpdateEditIfChanged(hMain, IDC_EDIT_RTR_RECVQ,      "%u", metrics->router.total_recv_q);
    UpdateEditIfChanged(hMain, IDC_EDIT_RTR_PKTS_SENT,  "%u", metrics->router.pkts_sent);
    UpdateEditIfChanged(hMain, IDC_EDIT_RTR_SEND_ERR,   "%u", metrics->router.pkts_send_err);
    UpdateEditIfChanged(hMain, IDC_EDIT_RTR_PKTS_RCVD,  "%u", metrics->router.pkts_rcvd);
    UpdateEditIfChanged(hMain, IDC_EDIT_RTR_ACK_SENT,   "%u", metrics->router.ack_sent);
    UpdateEditIfChanged(hMain, IDC_EDIT_RTR_ACK_RECV,   "%u", metrics->router.ack_recv);
    UpdateEditIfChanged(hMain, IDC_EDIT_RTR_DUP_DROP,   "%u", metrics->router.dup_dropped);
    UpdateEditIfChanged(hMain, IDC_EDIT_RTR_OOB_DROP,   "%u", metrics->router.oob_dropped);
    UpdateEditIfChanged(hMain, IDC_EDIT_RTR_STALE_ACK,  "%u", metrics->router.stale_ack);
    UpdateEditIfChanged(hMain, IDC_EDIT_RTR_SIGN_FAIL,  "%u", metrics->router.sign_fail);
    UpdateEditIfChanged(hMain, IDC_EDIT_RTR_RTT,       "%.1f ms", metrics->router.rtt_last/10.f);
    UpdateEditIfChanged(hMain, IDC_EDIT_RTR_RTT_AVG,   "%.1f ms", metrics->router.rtt_avg/10.f);
    UpdateEditIfChanged(hMain, IDC_EDIT_RTR_JITTER,    "%.1f ms", metrics->router.jitter/10.f);
    UpdateEditIfChanged(hMain, IDC_EDIT_RTR_MINRTT,    "%.1f ms", metrics->router.minrtt/10.f);
    
    /* Queue Metrics */
    UpdateEditIfChanged(hMain, IDC_EDIT_Q_IN_Q_BYTES,   "%u", metrics->etcp.input_queue_bytes);
    UpdateEditIfChanged(hMain, IDC_EDIT_Q_IN_Q_PKTS,    "%u", metrics->etcp.input_queue_packets);
    UpdateEditIfChanged(hMain, IDC_EDIT_Q_IN_SEND_BYTES, "%u", metrics->etcp.input_send_q_bytes);
    UpdateEditIfChanged(hMain, IDC_EDIT_Q_IN_SEND_PKTS,  "%u", metrics->etcp.input_send_q_packets);
    UpdateEditIfChanged(hMain, IDC_EDIT_Q_WAIT_ACK_BYTES, "%u", metrics->etcp.input_wait_ack_bytes);
    UpdateEditIfChanged(hMain, IDC_EDIT_Q_WAIT_ACK_PKTS, "%u", metrics->etcp.input_wait_ack_packets);
    UpdateEditIfChanged(hMain, IDC_EDIT_WAITACK_OPT_INFL, "%u", metrics->etcp.optimal_inflight);
    UpdateEditIfChanged(hMain, IDC_EDIT_Q_ACK_BYTES,     "%u", metrics->etcp.ack_q_bytes);
    UpdateEditIfChanged(hMain, IDC_EDIT_Q_ACK_PKTS,      "%u", metrics->etcp.ack_q_packets);
    UpdateEditIfChanged(hMain, IDC_EDIT_Q_RECV_BYTES,    "%u", metrics->etcp.recv_q_bytes);
    UpdateEditIfChanged(hMain, IDC_EDIT_Q_RECV_PKTS,     "%u", metrics->etcp.recv_q_packets);
    UpdateEditIfChanged(hMain, IDC_EDIT_Q_OUT_BYTES,     "%u", metrics->etcp.output_queue_bytes);
    UpdateEditIfChanged(hMain, IDC_EDIT_Q_OUT_PKTS,      "%u", metrics->etcp.output_queue_packets);
    
    /* Error Counters */
    UpdateEditIfChanged(hMain, IDC_EDIT_ERR_REINIT,     "%u", metrics->etcp.reinit_count);
    UpdateEditIfChanged(hMain, IDC_EDIT_ERR_RESET,      "%u", metrics->etcp.reset_count);
    UpdateEditIfChanged(hMain, IDC_EDIT_ERR_PKT_FMT,    "%u", metrics->etcp.pkt_format_errors);
    
    /* Timer Flags */
    UpdateEditIfChanged(hMain, IDC_EDIT_TIMER_RETRANS,  "%s", metrics->etcp.retrans_timer_active ? "ON" : "OFF");
    UpdateEditIfChanged(hMain, IDC_EDIT_TIMER_ACK_RESP, "%s", metrics->etcp.ack_resp_timer_active ? "ON" : "OFF");
    
    /* WaitAck queue state */
    UpdateEditIfChanged(hMain, IDC_EDIT_WAITACK_SUSP, "%s", metrics->etcp.wait_ack_cb_suspended ? "ON" : "OFF");
    UpdateEditIfChanged(hMain, IDC_EDIT_WAITACK_CB,   "%s", metrics->etcp.wait_ack_cb_set ? "ON" : "OFF");
    UpdateEditIfChanged(hMain, IDC_EDIT_WAITACK_TIMER, "%s", metrics->etcp.wait_ack_resume_timeout ? "ON" : "OFF");
    
    /* ID Metrics */
    UpdateEditIfChanged(hMain, IDC_EDIT_ID_NEXT_TX,    "%u", metrics->etcp.next_tx_id);
    UpdateEditIfChanged(hMain, IDC_EDIT_ID_LAST_RX,    "%u", metrics->etcp.last_rx_id);
    UpdateEditIfChanged(hMain, IDC_EDIT_ID_LAST_DEL,   "%u", metrics->etcp.last_delivered_id);
    UpdateEditIfChanged(hMain, IDC_EDIT_ID_RX_ACK_TILL, "%u", metrics->etcp.rx_ack_till);
    
    /* Normalizer */
    UpdateEditIfChanged(hMain, IDC_EDIT_NORM_IN_PKTS,   "%u", metrics->etcp.norm_input_pkts);
    UpdateEditIfChanged(hMain, IDC_EDIT_NORM_IN_BYTES,  "%u", metrics->etcp.norm_input_bytes);
    UpdateEditIfChanged(hMain, IDC_EDIT_NORM_OUT_PKTS,  "%u", metrics->etcp.norm_output_pkts);
    UpdateEditIfChanged(hMain, IDC_EDIT_NORM_OUT_BYTES, "%u", metrics->etcp.norm_output_bytes);
    UpdateEditIfChanged(hMain, IDC_EDIT_NORM_ALLOC_ERR, "%u", metrics->etcp.norm_alloc_errors);
    UpdateEditIfChanged(hMain, IDC_EDIT_NORM_LOGIC_ERR, "%u", metrics->etcp.norm_logic_errors);
    UpdateEditIfChanged(hMain, IDC_EDIT_NORM_FRAG_SIZE, "%u", metrics->etcp.norm_frag_size);
    UpdateEditIfChanged(hMain, IDC_EDIT_NORM_DATA_PTR,  "%u", metrics->etcp.norm_data_ptr);
    UpdateEditIfChanged(hMain, IDC_EDIT_NORM_DATA_SIZE, "%u", metrics->etcp.norm_data_size);
    UpdateEditIfChanged(hMain, IDC_EDIT_NORM_IN_TOT_PKTS,  "%llu", (unsigned long long)metrics->etcp.norm_in_total_pkts);
    UpdateEditIfChanged(hMain, IDC_EDIT_NORM_IN_TOT_BYTES, "%llu", (unsigned long long)metrics->etcp.norm_in_total_bytes);
    UpdateEditIfChanged(hMain, IDC_EDIT_NORM_OUT_TOT_PKTS, "%llu", (unsigned long long)metrics->etcp.norm_out_total_pkts);
    UpdateEditIfChanged(hMain, IDC_EDIT_NORM_OUT_TOT_BYTES, "%llu", (unsigned long long)metrics->etcp.norm_out_total_bytes);
    
    /* ACK Debug counters */
    UpdateEditIfChanged(hMain, IDC_EDIT_ACK_HIT_INF,   "%u", metrics->etcp.cnt_ack_hit_inf);
    UpdateEditIfChanged(hMain, IDC_EDIT_ACK_HIT_SNDQ, "%u", metrics->etcp.cnt_ack_hit_sndq);
    UpdateEditIfChanged(hMain, IDC_EDIT_ACK_MISS,     "%u", metrics->etcp.cnt_ack_miss);
    UpdateEditIfChanged(hMain, IDC_EDIT_CNT_LINK_WAIT, "%u", metrics->etcp.cnt_link_wait);
    UpdateEditIfChanged(hMain, IDC_EDIT_TX_STATE,     "%u", metrics->etcp.tx_state);
    for (int i = 0; i < 8; i++) {
        UpdateEditIfChanged(hMain, IDC_EDIT_DEBUG_0 + i, "%u", metrics->etcp.debug[i]);
    }

    /* System resources */
    UpdateEditIfChanged(hMain, IDC_EDIT_ACTIVE_TIMEOUTS, "%u", metrics->etcp.active_timeouts);
    UpdateEditIfChanged(hMain, IDC_EDIT_BUSY_MEMORY,      "%u", metrics->etcp.busy_memory_blocks);
    
    /* Links list */
    if (app->hListLinks) {
        SendMessage(app->hListLinks, LB_RESETCONTENT, 0, 0);
        for (uint8_t i = 0; i < links_count; i++) {
            char line1[320];
            char line2[256];
            
            snprintf(line1, sizeof(line1),
                "Link %u: Status=%s, EncErr=%u, DecErr=%u, "
                "SndErr=%u, RcvErr=%u, Enc=%llu, Dec=%llu, "
                "BW=%u Kbps, NAT=%u,",
                links[i].local_link_id,
                links[i].status ? "UP" : "DOWN",
                links[i].encrypt_errors,
                links[i].decrypt_errors,
                links[i].send_errors,
                links[i].recv_errors,
                (unsigned long long)links[i].total_encrypted,
                (unsigned long long)links[i].total_decrypted,
                links[i].bandwidth,
                links[i].nat_changes_count);
            
            snprintf(line2, sizeof(line2),
                "       RTT=%.1f ms, avg10=%.1f ms, TT=%.1f ms Timers: Init=%s, KA=%s, Shaper=%s | "
                "KAsent=%u, KArecv=%u, InB/L/P=%u/%u/%u",
                (float)links[i].rtt_last/10.0f,
                (float)links[i].rtt_avg10/10.0f,
                (float)links[i].tt_last/10.f,
                links[i].init_timer_active ? "ON" : "OFF",
                links[i].keepalive_timer_active ? "ON" : "OFF",
                links[i].shaper_timer_active ? "ON" : "OFF",
                links[i].keepalive_sent,
                links[i].keepalive_recv,
                links[i].inflight_bytes,
                links[i].inflight_lim_bytes,
                links[i].inflight_packets);
            
            char line3[320];
            char line4[256];
            const char* mode_names[] = {"STARTUP","DRAIN","PROBE_BW","PROBE_RTT"};
            const char* cycle_names[] = {"UP","DOWN","CRUISE","REFILL"};
            snprintf(line3, sizeof(line3),
                "       BBR: mode=%s cycle=%s full_bw=%s loss=%s pace=%.1f KiB/s rtt=%u us gain=%.2f",
                mode_names[links[i].bbr_mode > 3 ? 0 : links[i].bbr_mode],
                cycle_names[links[i].bbr_cycle_idx > 3 ? 0 : links[i].bbr_cycle_idx],
                links[i].bbr_full_bw_reached ? "yes" : "no",
                links[i].bbr_loss_in_round ? "yes" : "no",
                (float)links[i].bbr_pacing_rate / 1024.0f,
                links[i].bbr_min_rtt_us,
                (float)links[i].bbr_pacing_gain / 256.0f);
            snprintf(line4, sizeof(line4),
                "       bwHi=%.1f KiB/s bwLo=%.1f KiB/s infHi=%u infLo=%u",
                (float)links[i].bbr_bw_hi / 1024.0f,
                (float)links[i].bbr_bw_lo / 1024.0f,
                links[i].bbr_inflight_hi,
                links[i].bbr_inflight_lo);
            
            SendMessageA(app->hListLinks, LB_ADDSTRING, 0, (LPARAM)line1);
            SendMessageA(app->hListLinks, LB_ADDSTRING, 0, (LPARAM)line2);
            SendMessageA(app->hListLinks, LB_ADDSTRING, 0, (LPARAM)line3);
            SendMessageA(app->hListLinks, LB_ADDSTRING, 0, (LPARAM)line4);
        }
        InvalidateRect(app->hListLinks, NULL, FALSE);
    }

    /* График — принудительно перерисовываем */
    if (app->hGraphWnd) {
        InvalidateRect(app->hGraphWnd, NULL, TRUE);   /* TRUE = стираем фон */
        UpdateWindow(app->hGraphWnd);                 /* сразу отрисовать */
    }
}

void etcpmon_gui_clear_metrics(struct etcpmon_app* app) {
    if (!app) return;
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ETCP_NAME, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ETCP_RTT_LAST, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ETCP_RTT_AVG10, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ETCP_RTT_AVG100, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ETCP_JITTER, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ETCP_BYTES_SENT, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ETCP_RETRANS, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ETCP_RX_DUP, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ETCP_TX_DUP, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ETCP_ACKS, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ETCP_INFLIGHT, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ETCP_LINKS, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ETCP_MAX_INFLIGHT, "");
    
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_TUN_READ_BYTES, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_TUN_WRITE_BYTES, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_TUN_READ_PKTS, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_TUN_WRITE_PKTS, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_TUN_READ_ERRS, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_TUN_WRITE_ERRS, "");
    
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RT_ROUTED, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RT_DROPPED, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RT_TUN_IN_Q_PKTS, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RT_TUN_IN_Q_BYTES, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RT_TUN_OUT_Q_PKTS, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RT_TUN_OUT_Q_BYTES, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RT_COUNT, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RT_LOCAL, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RT_LEARNED, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RT_BGP_SENDERS, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RT_BGP_NODES, "");
    
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RTR_CONNS, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RTR_INFLIGHT, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RTR_SENDQ, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RTR_RECVQ, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RTR_PKTS_SENT, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RTR_SEND_ERR, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RTR_PKTS_RCVD, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RTR_ACK_SENT, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RTR_ACK_RECV, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RTR_DUP_DROP, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RTR_OOB_DROP, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RTR_STALE_ACK, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RTR_SIGN_FAIL, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RTR_RTT, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RTR_RTT_AVG, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RTR_JITTER, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_RTR_MINRTT, "");
    
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_Q_IN_Q_BYTES, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_Q_IN_Q_PKTS, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_Q_IN_SEND_BYTES, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_Q_IN_SEND_PKTS, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_Q_WAIT_ACK_BYTES, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_Q_WAIT_ACK_PKTS, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_WAITACK_OPT_INFL, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_Q_ACK_BYTES, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_Q_ACK_PKTS, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_Q_RECV_BYTES, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_Q_RECV_PKTS, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_Q_OUT_BYTES, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_Q_OUT_PKTS, "");
    
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ERR_REINIT, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ERR_RESET, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ERR_PKT_FMT, "");
    
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_TIMER_RETRANS, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_TIMER_ACK_RESP, "");
    
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_WAITACK_SUSP, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_WAITACK_CB, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_WAITACK_TIMER, "");
    
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ID_NEXT_TX, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ID_LAST_RX, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ID_LAST_DEL, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ID_RX_ACK_TILL, "");
    
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_NORM_IN_PKTS, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_NORM_IN_BYTES, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_NORM_OUT_PKTS, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_NORM_OUT_BYTES, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_NORM_ALLOC_ERR, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_NORM_LOGIC_ERR, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_NORM_FRAG_SIZE, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_NORM_DATA_PTR, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_NORM_DATA_SIZE, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_NORM_IN_TOT_PKTS, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_NORM_IN_TOT_BYTES, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_NORM_OUT_TOT_PKTS, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_NORM_OUT_TOT_BYTES, "");

    /* ACK Debug counters */
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ACK_HIT_INF, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ACK_HIT_SNDQ, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_ACK_MISS, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_CNT_LINK_WAIT, "");
    SetDlgItemTextA(app->hWndMain, IDC_EDIT_TX_STATE, "");
    for (int i = 0; i < 8; i++) {
        SetDlgItemTextA(app->hWndMain, IDC_EDIT_DEBUG_0 + i, "");
    }
    
    if (app->hListLinks) {
        SendMessage(app->hListLinks, LB_RESETCONTENT, 0, 0);
    }
    if (app->client) {
        memset(&app->client->history, 0, sizeof(app->client->history));
        free(app->client->last_metrics);
        app->client->last_metrics = NULL;
        free(app->client->last_links);
        app->client->last_links = NULL;
        app->client->last_links_count = 0;
    }
    app->graph_cursor_active = 0;
    for (int i = 0; i < GRAPH_METRICS_COUNT; i++) SetWindowTextA(app->hChannelValue[i], "");
    if (app->hGraphWnd) InvalidateRect(app->hGraphWnd, NULL, TRUE);
}
