// tun_windows.c - Wintun implementation

#if defined(_WIN32)

#include "../lib/debug_config.h"
#include "../lib/u_async.h"
#include "tun_if.h"
#include "packet_dump.h"
#include "wintun.h"
#include "ll_queue.h"
#include "etcp.h"
#include <windows.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <errno.h>
#include "../lib/mem.h"

// Wintun function pointers
static HMODULE wintun_dll = NULL;
static WINTUN_CREATE_ADAPTER_FUNC          *WintunCreateAdapter = NULL;
static WINTUN_CLOSE_ADAPTER_FUNC           *WintunCloseAdapter = NULL;
static WINTUN_OPEN_ADAPTER_FUNC            *WintunOpenAdapter = NULL;
static WINTUN_GET_ADAPTER_LUID_FUNC        *WintunGetAdapterLUID = NULL;
static WINTUN_START_SESSION_FUNC           *WintunStartSession = NULL;
static WINTUN_END_SESSION_FUNC             *WintunEndSession = NULL;
static WINTUN_GET_READ_WAIT_EVENT_FUNC     *WintunGetReadWaitEvent = NULL;
static WINTUN_RECEIVE_PACKET_FUNC          *WintunReceivePacket = NULL;
static WINTUN_RELEASE_RECEIVE_PACKET_FUNC  *WintunReleaseReceivePacket = NULL;
static WINTUN_ALLOCATE_SEND_PACKET_FUNC    *WintunAllocateSendPacket = NULL;
static WINTUN_SEND_PACKET_FUNC             *WintunSendPacket = NULL;

static int wintun_load_dll(void)
{
    if (wintun_dll) return 0;

    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(NULL, path, MAX_PATH)) {
        for (int i = (int)wcslen(path)-1; i >= 0; i--) {
            if (path[i] == L'\\' || path[i] == L'/') {
                path[i+1] = L'\0';
                wcscat(path, L"wintun.dll");
                wintun_dll = LoadLibraryW(path);
                break;
            }
        }
    }
    if (!wintun_dll) wintun_dll = LoadLibraryA("wintun.dll");

    if (!wintun_dll) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Cannot load wintun.dll; place it beside the executable: error=%lu", GetLastError());
        return -1;
    }

    WintunCreateAdapter         = (WINTUN_CREATE_ADAPTER_FUNC*)GetProcAddress(wintun_dll, "WintunCreateAdapter");
    WintunCloseAdapter          = (WINTUN_CLOSE_ADAPTER_FUNC*)GetProcAddress(wintun_dll, "WintunCloseAdapter");
    WintunOpenAdapter           = (WINTUN_OPEN_ADAPTER_FUNC*)GetProcAddress(wintun_dll, "WintunOpenAdapter");
    WintunGetAdapterLUID        = (WINTUN_GET_ADAPTER_LUID_FUNC*)GetProcAddress(wintun_dll, "WintunGetAdapterLUID");
    WintunStartSession          = (WINTUN_START_SESSION_FUNC*)GetProcAddress(wintun_dll, "WintunStartSession");
    WintunEndSession            = (WINTUN_END_SESSION_FUNC*)GetProcAddress(wintun_dll, "WintunEndSession");
    WintunGetReadWaitEvent      = (WINTUN_GET_READ_WAIT_EVENT_FUNC*)GetProcAddress(wintun_dll, "WintunGetReadWaitEvent");
    WintunReceivePacket         = (WINTUN_RECEIVE_PACKET_FUNC*)GetProcAddress(wintun_dll, "WintunReceivePacket");
    WintunReleaseReceivePacket  = (WINTUN_RELEASE_RECEIVE_PACKET_FUNC*)GetProcAddress(wintun_dll, "WintunReleaseReceivePacket");
    WintunAllocateSendPacket    = (WINTUN_ALLOCATE_SEND_PACKET_FUNC*)GetProcAddress(wintun_dll, "WintunAllocateSendPacket");
    WintunSendPacket            = (WINTUN_SEND_PACKET_FUNC*)GetProcAddress(wintun_dll, "WintunSendPacket");

    if (!WintunCreateAdapter || !WintunCloseAdapter || !WintunOpenAdapter ||
        !WintunGetAdapterLUID || !WintunStartSession || !WintunEndSession ||
        !WintunGetReadWaitEvent || !WintunReceivePacket || !WintunReleaseReceivePacket ||
        !WintunAllocateSendPacket || !WintunSendPacket) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to resolve Wintun functions");
        FreeLibrary(wintun_dll);
        wintun_dll = NULL;
        return -1;
    }
    return 0;
}

// ===================================================================
// Helper: set IP + MTU + bring interface up
// ===================================================================
static int wintun_configure_interface(WINTUN_ADAPTER_HANDLE adapter, const char* ip_str, int mtu)
{
    NET_LUID luid;
    WintunGetAdapterLUID(adapter, &luid);

    char ip_only[64] = {0};
    int prefix = 32;
    if (ip_str) {
        strncpy(ip_only, ip_str, sizeof(ip_only)-1);
        char* slash = strchr(ip_only, '/');
        if (slash) {
            *slash = '\0';
            prefix = atoi(slash + 1);
            if (prefix < 0 || prefix > 32) prefix = 32;
        }
    }

    struct in_addr addr;
    if (inet_pton(AF_INET, ip_only, &addr) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Invalid IP: %s", ip_str);
        return -1;
    }

    // IP address
    MIB_UNICASTIPADDRESS_ROW row = {0};
    InitializeUnicastIpAddressEntry(&row);
    row.Address.si_family = AF_INET;
    row.Address.Ipv4.sin_family = AF_INET;
    row.Address.Ipv4.sin_addr.s_addr = addr.s_addr;
    row.InterfaceLuid = luid;
    row.OnLinkPrefixLength = (UINT8)prefix;
    row.DadState = IpDadStatePreferred;
    row.SuffixOrigin = IpSuffixOriginManual;
    row.PrefixOrigin = IpPrefixOriginManual;

    DWORD err = CreateUnicastIpAddressEntry(&row);
    if (err != NO_ERROR && err != ERROR_OBJECT_ALREADY_EXISTS) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "CreateUnicastIpAddressEntry failed: %lu", err);
        return -1;
    }

    // MTU
    if (mtu > 0) {
        MIB_IPINTERFACE_ROW ifRow = {0};
        InitializeIpInterfaceEntry(&ifRow);
        ifRow.Family = AF_INET;
        ifRow.InterfaceLuid = luid;
        err = GetIpInterfaceEntry(&ifRow);
        if (err != NO_ERROR) { DEBUG_ERROR(DEBUG_CATEGORY_TUN, "GetIpInterfaceEntry failed: %lu", err); return -1; }
        ifRow.NlMtu = mtu;
        err = SetIpInterfaceEntry(&ifRow);
        if (err != NO_ERROR) { DEBUG_ERROR(DEBUG_CATEGORY_TUN, "SetIpInterfaceEntry MTU failed: %lu", err); return -1; }
    }

    // Bring UP
    NET_IFINDEX idx = 0;
    err = ConvertInterfaceLuidToIndex(&luid, &idx);
    if (err != NO_ERROR || !idx) { DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to get Wintun index: %lu", err); return -1; }
    MIB_IFROW ifr = {0}; ifr.dwIndex = idx;
    err = GetIfEntry(&ifr);
    if (err != NO_ERROR) { DEBUG_ERROR(DEBUG_CATEGORY_TUN, "GetIfEntry failed: %lu", err); return -1; }
    ifr.dwAdminStatus = MIB_IF_ADMIN_STATUS_UP;
    err = SetIfEntry(&ifr);
    if (err != NO_ERROR) { DEBUG_ERROR(DEBUG_CATEGORY_TUN, "SetIfEntry UP failed: %lu", err); return -1; }
    return 0;
}

// ===================================================================
// Platform functions
// ===================================================================
int tun_platform_init(struct tun_if* tun, const char* ifname, const char* ip_str, int mtu)
{
    if (wintun_load_dll() != 0) return -1;

    wchar_t wname[128] = L"utun";
    if (ifname && ifname[0])
        MultiByteToWideChar(CP_UTF8, 0, ifname, -1, wname, 128);

    WINTUN_ADAPTER_HANDLE adapter = WintunCreateAdapter(wname, L"utun", NULL);
    if (!adapter) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "WintunCreateAdapter failed: %lu", GetLastError());
        return -1;
    }

    WINTUN_SESSION_HANDLE session = WintunStartSession(adapter, 0x400000); // 4 MiB ring
    if (!session) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "WintunStartSession failed: %lu", GetLastError());
        WintunCloseAdapter(adapter);
        return -1;
    }

    if (wintun_configure_interface(adapter, ip_str, mtu) != 0) {
        WintunEndSession(session);
        WintunCloseAdapter(adapter);
        return -1;
    }

    tun->platform_handle = session;
    tun->adapter_handle  = adapter;
    strncpy(tun->ifname, ifname && ifname[0] ? ifname : "utun", sizeof(tun->ifname)-1);

    NET_LUID luid;
    WintunGetAdapterLUID(adapter, &luid);
    NET_IFINDEX idx = 0;
    if (ConvertInterfaceLuidToIndex(&luid, &idx) == NO_ERROR) {
        tun->ifindex = idx;
        DEBUG_INFO(DEBUG_CATEGORY_TUN, "Wintun interface index: %lu", (unsigned long)idx);
    } else {
        tun->ifindex = 0;
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to get interface index from LUID");
        tun_platform_cleanup(tun); return -1;
    }

    return 0;
}

void tun_platform_cleanup(struct tun_if* tun)
{
    if (tun->platform_handle) {
        WintunEndSession((WINTUN_SESSION_HANDLE)tun->platform_handle);
        tun->platform_handle = NULL;
    }
    if (tun->adapter_handle) {
        WintunCloseAdapter((WINTUN_ADAPTER_HANDLE)tun->adapter_handle);
        tun->adapter_handle = NULL;
    }
}

ssize_t tun_platform_write(struct tun_if* tun, const uint8_t* buf, size_t len) {
    if (!tun->platform_handle) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Wintun %s has no session", tun->ifname);
        errno = ENODEV;
        return -1;
    }
    BYTE* pkt = WintunAllocateSendPacket((WINTUN_SESSION_HANDLE)tun->platform_handle, (DWORD)len);
    if (!pkt) {
        DWORD error = GetLastError();
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Wintun %s allocate send failed: len=%zu error=%lu", tun->ifname, len, error);
        errno = error == ERROR_BUFFER_OVERFLOW ? EAGAIN : EIO;
        return -1;
    }
    memcpy(pkt, buf, len);
    WintunSendPacket((WINTUN_SESSION_HANDLE)tun->platform_handle, pkt);
    return (ssize_t)len;
}

int tun_platform_get_poll_fd(struct tun_if* tun) { (void)tun; return -1; }

/* Один принятый пакет ждёт main thread; stop имеет приоритет в обоих ожиданиях. */
static DWORD WINAPI tun_read_thread_proc(LPVOID arg) {
    struct tun_if* tun = arg;
    HANDLE permit[2] = {tun->stop_event, tun->read_permit};
    HANDLE ready[2] = {tun->stop_event, tun->read_event};
    for (;;) {
        DWORD result = WaitForMultipleObjects(2, permit, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0) break;
        if (result != WAIT_OBJECT_0 + 1) { tun_reader_error(tun, "permit wait", GetLastError()); break; }
        result = WaitForMultipleObjects(2, ready, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0) break;
        if (result != WAIT_OBJECT_0 + 1) { tun_reader_error(tun, "read wait", GetLastError()); break; }
        DWORD size = 0;
        BYTE* packet = WintunReceivePacket((WINTUN_SESSION_HANDLE)tun->platform_handle, &size);
        if (!packet) {
            DWORD error = GetLastError();
            if (error != ERROR_NO_MORE_ITEMS) { tun_reader_error(tun, "receive", error); break; }
            if (!SetEvent(tun->read_permit)) { tun_reader_error(tun, "empty receive permit", GetLastError()); break; }
            continue;
        }
        int posted = tun_post_packet(tun, packet, size);
        WintunReleaseReceivePacket((WINTUN_SESSION_HANDLE)tun->platform_handle, packet);
        if (posted != 0 && !SetEvent(tun->read_permit)) { tun_reader_error(tun, "retry permit", GetLastError()); break; }
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_TUN, "Wintun %s reader stopped", tun->ifname);
    return 0;
}

int tun_windows_start_reader(struct tun_if* tun) {
    tun->read_event = WintunGetReadWaitEvent((WINTUN_SESSION_HANDLE)tun->platform_handle);
    if (!tun->read_event) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Wintun %s read event failed: %lu", tun->ifname, GetLastError());
        return -1;
    }
    tun->stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!tun->stop_event) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Wintun %s stop event failed: %lu", tun->ifname, GetLastError());
        return -1;
    }
    tun->read_permit = CreateEvent(NULL, FALSE, TRUE, NULL);
    if (!tun->read_permit) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Wintun %s permit event failed: %lu", tun->ifname, GetLastError());
        return -1;
    }
    tun->read_thread = CreateThread(NULL, 0, tun_read_thread_proc, tun, 0, NULL);
    if (!tun->read_thread) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Wintun %s read thread failed: %lu", tun->ifname, GetLastError());
        return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_TUN, "Wintun %s reader started with output backpressure", tun->ifname);
    return 0;
}

#endif // _WIN32
