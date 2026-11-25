/*
 * WinAPI GUI etcpmon: init(app, instance) → create_window → run → cleanup.
 * Caller владеет app; все вызовы, Winsock-клиент и callbacks работают в одном GUI-потоке.
 * init регистрирует классы окон и запускает Winsock; create_window выделяет client, создаёт
 * контролы/таймер 10 мс и сразу начинает connect к 192.168.40.1:9091. run показывает окно.
 * GUI опрашивает списки раз в секунду; метрики автоматически запрашивает client раз в 100 мс.
 */

#ifndef ETCPMON_GUI_H
#define ETCPMON_GUI_H

/* Exclude winsock from windows.h to avoid conflict with winsock2.h */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

/* Windows headers */
#include <windows.h>
#include <stdint.h>

/* Client structure is defined in etcpmon_client.h */
struct etcpmon_client;
struct etcpmon_conn_info;
struct etcpmon_socket_info;
struct etcpmon_rsp_metrics;
struct etcpmon_link_metrics;

/* Graph constants */
#include "etcpmon_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Window class name */
#define ETCPMON_WINDOW_CLASS "ETCPMonMainWindow"

/* Control IDs */
#define IDC_STATIC              -1
#define IDC_EDIT_ADDR           100
#define IDC_EDIT_PORT           101
#define IDC_BTN_CONNECT         102
#define IDC_BTN_DISCONNECT      103
#define IDC_LIST_CONNECTIONS    104
#define IDC_LIST_SOCKETS        106
#define IDC_TIMER_UPDATE        105

/* Action control IDs */
#define IDC_EDIT_ACTION         107
#define IDC_BTN_ACTION          108

/* ETCP Metrics control IDs */
#define IDC_EDIT_ETCP_NAME      200
#define IDC_EDIT_ETCP_RTT_LAST  201
#define IDC_EDIT_ETCP_RTT_AVG10 202
#define IDC_EDIT_ETCP_RTT_AVG100 203
#define IDC_EDIT_ETCP_JITTER    204
#define IDC_EDIT_ETCP_BYTES_SENT 205
#define IDC_EDIT_ETCP_RETRANS   206
#define IDC_EDIT_ETCP_ACKS      207
#define IDC_EDIT_ETCP_INFLIGHT  208
#define IDC_EDIT_ETCP_LINKS     209
#define IDC_EDIT_ETCP_RX_DUP    210
#define IDC_EDIT_ETCP_TX_DUP    211
#define IDC_EDIT_ETCP_MAX_INFLIGHT 212

/* TUN Metrics control IDs */
#define IDC_EDIT_TUN_READ_BYTES  300
#define IDC_EDIT_TUN_WRITE_BYTES 301
#define IDC_EDIT_TUN_READ_PKTS   302
#define IDC_EDIT_TUN_WRITE_PKTS  303
#define IDC_EDIT_TUN_READ_ERRS   304
#define IDC_EDIT_TUN_WRITE_ERRS  305

/* Routing Metrics control IDs */
#define IDC_EDIT_RT_ROUTED      306
#define IDC_EDIT_RT_DROPPED     307
#define IDC_EDIT_RT_TUN_IN_Q_PKTS 308
#define IDC_EDIT_RT_TUN_IN_Q_BYTES 309
#define IDC_EDIT_RT_TUN_OUT_Q_PKTS 310
#define IDC_EDIT_RT_TUN_OUT_Q_BYTES 311
#define IDC_EDIT_RT_COUNT       312
#define IDC_EDIT_RT_LOCAL       313
#define IDC_EDIT_RT_LEARNED     314
#define IDC_EDIT_RT_BGP_SENDERS 315
#define IDC_EDIT_RT_BGP_NODES   316

/* Router Congestion control IDs */
#define IDC_GB_ROUTER           350
#define IDC_EDIT_RTR_CONNS      351
#define IDC_EDIT_RTR_INFLIGHT   352
#define IDC_EDIT_RTR_SENDQ      353
#define IDC_EDIT_RTR_RECVQ      354
#define IDC_EDIT_RTR_PKTS_SENT  355
#define IDC_EDIT_RTR_SEND_ERR   356
#define IDC_EDIT_RTR_PKTS_RCVD  357
#define IDC_EDIT_RTR_ACK_SENT   358
#define IDC_EDIT_RTR_ACK_RECV   359
#define IDC_EDIT_RTR_DUP_DROP   360
#define IDC_EDIT_RTR_OOB_DROP   361
#define IDC_EDIT_RTR_STALE_ACK  362
#define IDC_EDIT_RTR_SIGN_FAIL  363
#define IDC_EDIT_RTR_RTT        364
#define IDC_EDIT_RTR_RTT_AVG    365
#define IDC_EDIT_RTR_JITTER     366
#define IDC_EDIT_RTR_MINRTT     367

/* Link list control ID */
#define IDC_LIST_LINKS          400

/* Queue & Error Metrics control IDs */
#define IDC_STATIC_QUEUES       600
#define IDC_EDIT_Q_IN_Q_BYTES   601
#define IDC_EDIT_Q_IN_Q_PKTS   602
#define IDC_EDIT_Q_IN_SEND_BYTES 603
#define IDC_EDIT_Q_IN_SEND_PKTS 604
#define IDC_EDIT_Q_WAIT_ACK_BYTES 605
#define IDC_EDIT_Q_WAIT_ACK_PKTS 606
#define IDC_EDIT_Q_ACK_BYTES    607
#define IDC_EDIT_Q_ACK_PKTS     608
#define IDC_EDIT_Q_RECV_BYTES   609
#define IDC_EDIT_Q_RECV_PKTS    610
#define IDC_EDIT_Q_OUT_BYTES    611
#define IDC_EDIT_Q_OUT_PKTS     612

#define IDC_STATIC_ERRORS       620
#define IDC_EDIT_ERR_REINIT     621
#define IDC_EDIT_ERR_RESET      622
#define IDC_EDIT_ERR_PKT_FMT   623

#define IDC_STATIC_TIMERS       630
#define IDC_EDIT_TIMER_RETRANS  631
#define IDC_EDIT_TIMER_ACK_RESP 632

/* WaitAck queue state IDs */
#define IDC_EDIT_WAITACK_SUSP   650
#define IDC_EDIT_WAITACK_CB     651
#define IDC_EDIT_WAITACK_TIMER  652
#define IDC_EDIT_WAITACK_OPT_INFL 653

/* ID Metrics control IDs */
#define IDC_STATIC_IDS          640
#define IDC_EDIT_ID_NEXT_TX    641
#define IDC_EDIT_ID_LAST_RX    642
#define IDC_EDIT_ID_LAST_DEL   643
#define IDC_EDIT_ID_RX_ACK_TILL 644

/* Normalizer IDs */
#define IDC_STATIC_NORM        660
#define IDC_EDIT_NORM_IN_PKTS 661
#define IDC_EDIT_NORM_IN_BYTES 662
#define IDC_EDIT_NORM_OUT_PKTS 663
#define IDC_EDIT_NORM_OUT_BYTES 664
#define IDC_EDIT_NORM_ALLOC_ERR 665
#define IDC_EDIT_NORM_LOGIC_ERR 666
#define IDC_EDIT_NORM_FRAG_SIZE 667
#define IDC_EDIT_NORM_DATA_PTR  668
#define IDC_EDIT_NORM_DATA_SIZE 669
#define IDC_EDIT_NORM_IN_TOT_PKTS   670
#define IDC_EDIT_NORM_IN_TOT_BYTES  671
#define IDC_EDIT_NORM_OUT_TOT_PKTS  672
#define IDC_EDIT_NORM_OUT_TOT_BYTES 673

/* ACK Debug IDs */
#define IDC_STATIC_ACKDBG         680
#define IDC_EDIT_ACK_HIT_INF     681
#define IDC_EDIT_ACK_HIT_SNDQ    682
#define IDC_EDIT_ACK_MISS        683
#define IDC_EDIT_CNT_LINK_WAIT   684
#define IDC_EDIT_TX_STATE        685
#define IDC_EDIT_DEBUG_0         686
#define IDC_EDIT_DEBUG_1         687
#define IDC_EDIT_DEBUG_2         688
#define IDC_EDIT_DEBUG_3         689
#define IDC_EDIT_DEBUG_4         690
#define IDC_EDIT_DEBUG_5         691
#define IDC_EDIT_DEBUG_6         692
#define IDC_EDIT_DEBUG_7         693

/* System resources IDs */
#define IDC_EDIT_ACTIVE_TIMEOUTS  700
#define IDC_EDIT_BUSY_MEMORY      701

/* Debug Levels control IDs */
#define IDC_DEBUG_MAX_CAT        64
#define IDC_DEBUG_GLOBAL_BASE    1000  /* +0..5 radio: NONE(0)..TRACE(5) */
#define IDC_DEBUG_HEADER_BASE    1010  /* +0..5 header EDIT */
#define IDC_DEBUG_ROW_BASE       1100  /* + row*8 + level */
#define IDC_DEBUG_NAME_BASE      2000  /* + row: category name label */

/* Graph control ID */
#define IDC_GRAPH              500

/* Channel block IDs */
#define IDC_CH_NAME_0         510
#define IDC_CH_VALUE_0        520
#define IDC_CH_CHECK_0        530
#define IDC_CH_NAME_1         511
#define IDC_CH_VALUE_1        521
#define IDC_CH_CHECK_1        531
#define IDC_CH_NAME_2         512
#define IDC_CH_VALUE_2        522
#define IDC_CH_CHECK_2        532
#define IDC_CH_NAME_3         513
#define IDC_CH_VALUE_3        523
#define IDC_CH_CHECK_3        533
#define IDC_CH_NAME_4         514
#define IDC_CH_VALUE_4        524
#define IDC_CH_CHECK_4        534
#define IDC_CH_NAME_5         515
#define IDC_CH_VALUE_5        525
#define IDC_CH_CHECK_5        535
#define IDC_CH_NAME_6         516
#define IDC_CH_VALUE_6        526
#define IDC_CH_CHECK_6        536
#define IDC_CH_NAME_7         517
#define IDC_CH_VALUE_7        527
#define IDC_CH_CHECK_7        537

/* Состояние одного приложения; HWND контролов принадлежат окнам, перья и client — app.
 * Порядок hChannel* совпадает с GRAPH_METRIC_*; интерфейс не предназначен для нескольких app одновременно. */
struct etcpmon_app {
    HINSTANCE hInstance;
    HWND hWndMain;
    HWND hWndStatus;
    HWND hToolTip;

    HPEN hGraphPens[GRAPH_METRICS_COUNT];

    /* Connection controls */
    HWND hEditAddr;
    HWND hEditPort;
    HWND hBtnConnect;
    HWND hBtnDisconnect;
    HWND hListConnections;
    HWND hListSockets;
    HWND hEditAction;
    HWND hBtnAction;

    /* Action result popup */
    HWND hActionResultWnd;
    HWND hActionResultEdit;

    /* ETCP metrics controls */
    HWND hEditEtcpName;
    HWND hEditEtcpRttLast;
    HWND hEditEtcpRttAvg10;
    HWND hEditEtcpRttAvg100;
    HWND hEditEtcpJitter;
    HWND hEditEtcpBytesSent;
    HWND hEditEtcpRetrans;
    HWND hEditEtcpRxDup;
    HWND hEditEtcpTxDup;
    HWND hEditEtcpAcks;
    HWND hEditEtcpInflight;
    HWND hEditEtcpLinks;
    HWND hEditEtcpMaxInflight;
    
    /* TUN metrics controls */
    HWND hEditTunReadBytes;
    HWND hEditTunWriteBytes;
    HWND hEditTunReadPkts;
    HWND hEditTunWritePkts;
    HWND hEditTunReadErrs;
    HWND hEditTunWriteErrs;
    
    /* Routing metrics controls */
    HWND hEditRtRouted;
    HWND hEditRtDropped;
    HWND hEditRtTunInQPkts;
    HWND hEditRtTunInQBytes;
    HWND hEditRtTunOutQPkts;
    HWND hEditRtTunOutQBytes;
    HWND hEditRtCount;
    HWND hEditRtLocal;
    HWND hEditRtLearned;
    HWND hEditRtBgpSenders;
    HWND hEditRtBgpNodes;
    
    /* Router Congestion controls */
    HWND hEditRtrConns;
    HWND hEditRtrInflight;
    HWND hEditRtrSendQ;
    HWND hEditRtrRecvQ;
    HWND hEditRtrPktsSent;
    HWND hEditRtrSendErr;
    HWND hEditRtrPktsRcvd;
    HWND hEditRtrAckSent;
    HWND hEditRtrAckRecv;
    HWND hEditRtrDupDrop;
    HWND hEditRtrOobDrop;
    HWND hEditRtrStaleAck;
    HWND hEditRtrSignFail;
    HWND hEditRtrRtt;
    HWND hEditRtrRttAvg;
    HWND hEditRtrJitter;
    HWND hEditRtrMinrtt;
    
    /* Links list */
    HWND hListLinks;
    
    /* Queue metrics controls */
    HWND hEditQInQBytes;
    HWND hEditQInQPkts;
    HWND hEditQInSendBytes;
    HWND hEditQInSendPkts;
    HWND hEditQWaitAckBytes;
    HWND hEditQWaitAckPkts;
    HWND hEditQWaitAckOptInfl;
    HWND hEditQAckBytes;
    HWND hEditQAckPkts;
    HWND hEditQRecvBytes;
    HWND hEditQRecvPkts;
    HWND hEditQOutBytes;
    HWND hEditQOutPkts;
    
    /* Error counters */
    HWND hEditErrReinit;
    HWND hEditErrReset;
    HWND hEditErrPktFmt;
    
    /* Timer flags */
    HWND hEditTimerRetrans;
    HWND hEditTimerAckResp;
    
    /* WaitAck queue state */
    HWND hEditWaitAckSuspend;
    HWND hEditWaitAckCb;
    HWND hEditWaitAckTimer;
    
    /* ID metrics */
    HWND hEditIdNextTx;
    HWND hEditIdLastRx;
    HWND hEditIdLastDel;
    HWND hEditIdRxAckTill;
    
    /* Normalizer */
    HWND hEditNormInPkts;
    HWND hEditNormInBytes;
    HWND hEditNormOutPkts;
    HWND hEditNormOutBytes;
    HWND hEditNormAllocErr;
    HWND hEditNormLogicErr;
    HWND hEditNormFragSize;
    HWND hEditNormDataPtr;
    HWND hEditNormDataSize;
    HWND hEditNormInTotPkts;
    HWND hEditNormInTotBytes;
    HWND hEditNormOutTotPkts;
    HWND hEditNormOutTotBytes;

    /* ACK Debug counters */
    HWND hEditAckHitInf;
    HWND hEditAckHitSndq;
    HWND hEditAckMiss;
    HWND hEditCntLinkWait;
    HWND hEditTxState;
    HWND hEditDebug[8];

    /* System resources */
    HWND hEditActiveTimeouts;
    HWND hEditBusyMemory;
    
    /* Graph controls */
    HWND hGraphWnd;
    HWND hChannelName[GRAPH_METRICS_COUNT];
    HWND hChannelValue[GRAPH_METRICS_COUNT];
    HWND hChannelCheck[GRAPH_METRICS_COUNT];
    
    /* Cursor (вертикальная линия остаётся) */
    int graph_cursor_x;
    int graph_cursor_y;
    int graph_cursor_active;
    
    /* Debug Levels controls */
    HWND hDebugGroupBox;
    HWND hDebugGlobalRadio[6];                  /* 0=NONE..5=TRACE */
    HWND hDebugHeaderEdit[6];                   /* вертикальный текст: T\nR\nA\nC\nE итд */
    HWND hDebugCatLabel[IDC_DEBUG_MAX_CAT];     /* метки категорий */
    HWND hDebugCatRadio[IDC_DEBUG_MAX_CAT][6];  /* [row][level] радио-кнопки */
    int   debug_cat_count;

    /* Выделенный create_window клиент; cleanup освобождает его и завершает Winsock. */
    struct etcpmon_client* client;

    int need_initial_request;          /* 1 = после готовности TCP запросить списки и debug config */
    uint64_t last_selected_peer_id;    /* чтобы не спамить SELECT_CONN каждый тик */
    
    /* Timer */
    UINT_PTR updateTimer;
    DWORD last_poll_time;              /* GetTickCount последнего опроса списков; период 1000 мс */
    int isConnected;                   /* Включает CONNECTING; блокирует редактирование адреса */
};

/* Обнулить caller-owned app, зарегистрировать классы с hInstance, запустить Winsock.
 * 0 = успех, -1 = ошибка; основное окно и TCP-клиент ещё не созданы. */
int etcpmon_gui_init(struct etcpmon_app* app, HINSTANCE hInstance);

/* Создать главное окно, контролы, client и таймер; начать connect к адресу по умолчанию.
 * Возвращает HWND (также app->hWndMain) или NULL при ошибке создания окна. Показывает окно run. */
HWND etcpmon_gui_create_window(struct etcpmon_app* app);

/* Показать app->hWndMain и обрабатывать сообщения/сеть до WM_QUIT.
 * Возвращает wParam сообщения выхода, -1 для отсутствующего app/окна. */
int etcpmon_gui_run(struct etcpmon_app* app);

/* После завершения run освободить tooltip, перья, client и Winsock; сам app не освобождает. */
void etcpmon_gui_cleanup(struct etcpmon_app* app);

/* Скопировать text в status bar app; состояние соединения не меняет. */
void etcpmon_gui_set_status(struct etcpmon_app* app, const char* text);

/* Показать list[count], сохранить выбор по last_selected_peer_id или выбрать первую запись.
 * list заимствован на время вызова, count=0 очищает список; SELECT здесь не отправляется. */
void etcpmon_gui_update_conn_list(struct etcpmon_app* app,
                                   struct etcpmon_conn_info* list,
                                   uint8_t count);

/* Показать list[count] локальных сокетов; пустые name/ip пропускаются. Данные заимствованы на время вызова. */
void etcpmon_gui_update_socket_list(struct etcpmon_app* app,
                                     struct etcpmon_socket_info* list,
                                     uint8_t count);

/* Показать metrics и links[links_count], перерисовать график из уже обновлённой истории client.
 * Входные снимки заимствованы на время вызова; новый сэмпл в историю не добавляет. */
void etcpmon_gui_update_metrics(struct etcpmon_app* app,
                                struct etcpmon_rsp_metrics* metrics,
                                struct etcpmon_link_metrics* links,
                                uint8_t links_count);

/* Очистить поля/линки/график и освободить кеши метрик client, обнулить его историю.
 * Транспорт, selected_peer_id и pending не меняет; не вызывать с используемыми далее указателями на кеш.
 * Повторный вывод прежних величин после очистки обновляет сами контролы, отдельного кеша текста нет. */
void etcpmon_gui_clear_metrics(struct etcpmon_app* app);

/* Запросить перерисовку графика app и обновить значения последнего сэмпла при неактивном курсоре.
 * metrics служит проверкой ненулевого аргумента; данные берутся из истории client, сэмпл не добавляется. */
void etcpmon_gui_update_graph(struct etcpmon_app* app,
                              struct etcpmon_rsp_metrics* metrics);

#ifdef __cplusplus
}
#endif

#endif /* ETCPMON_GUI_H */
