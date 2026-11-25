/* Контракт INI: границы значений, единый отказ файла/буфера, rollback памяти и безопасный dump. */
#include "config_parser.h"
#include "../src/routing_layer/topo_node.h"
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#include "../lib/platform_compat.h"
#include "../lib/socket_compat.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static unsigned fail_at, allocation;
static int inject(const char* where) { return fail_at && strstr(where, "config_parser.c") && ++allocation == fail_at; }
extern void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
extern void* __real_u_realloc_impl(void*, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t n, uint32_t size, const char* where) {
    return inject(where) ? NULL : __real_u_calloc_impl(n, size, where);
}
void* __wrap_u_realloc_impl(void* p, uint32_t size, const char* where) {
    return inject(where) ? NULL : __real_u_realloc_impl(p, size, where);
}

static struct utun_config* parse(const char* text) { return parse_config_from_buf(text, strlen(text), "parser-contract"); }

static void test_values(void) {
    struct utun_config* cfg = parse("");
    assert(cfg && cfg->global.crypto_tx_threads == 4 && cfg->global.crypto_rx_threads == 4);
    assert(cfg && cfg->global.chatserver_storage_unit_size == UINT64_C(100) * 1024 * 1024 &&
        cfg->global.chatserver_storage_total_size == UINT64_C(1024) * 1024 * 1024); free_config(cfg);
    cfg = parse("[global]\ncrypto_tx_threads=0\ncrypto_rx_threads=64\n");
    assert(cfg && cfg->global.crypto_tx_threads == 0 && cfg->global.crypto_rx_threads == 64); free_config(cfg);
    cfg = parse("[global]\ncrypto_tx_threads=1\ncrypto_rx_threads=0\n");
    assert(cfg && cfg->global.crypto_tx_threads == 1 && cfg->global.crypto_rx_threads == 0); free_config(cfg);
    cfg = parse("[routing]\nmy_subnet=::1/32\nroute_subnet=2001:db8::/0\nroute_subnet=192.0.2.0/0\n"
        "[control]\nport=0\nip=127.0.0.2\nallow=0.0.0.0/0\nallow=192.0.2.123/24\n"
        "[server:udp]\naddr=127.0.0.1:0\n[client:peer]\nlink=udp:127.0.0.2:65535\n"
        "[chat]\nstorage_total_size=18446744073709551615\nstorage_unit_size=17179869183G\n"
        "[tcp_proxy_client]\nenabled=no\nmtu=1280\n");
    assert(cfg && cfg->my_subnets->netmask == 32 && cfg->route_subnets->netmask == 0 && cfg->route_subnets->next->netmask == 0);
    assert(cfg->global.control_sock.ss_family == AF_INET &&
        ntohl(((struct sockaddr_in*)&cfg->global.control_sock)->sin_addr.s_addr) == 0x7f000002);
    assert(!cfg->global.control_allows[0].network && !cfg->global.control_allows[0].netmask);
    assert(cfg->global.control_allows[1].network == 0xc0000200 && cfg->global.control_allows[1].netmask == 0xffffff00);
    assert(cfg->clients->links->local_srv == cfg->servers && !strcmp(cfg->clients->links->server_name, "udp"));
    assert(cfg->global.chatserver_storage_total_size == UINT64_MAX);
    assert(cfg->global.chatserver_storage_unit_size == UINT64_C(17179869183) * 1024 * 1024 * 1024);
    assert(!cfg->global.tcp_proxy_client_enabled); free_config(cfg);
    cfg = parse("[routing]\nmy_subnet=::1\n[server:v6]\naddr=temporary_ipv6:65535\naddr=[::1]:0\n"
        "[control]\nip=::1\nport=65535\n[reality]\nversion=0.255.1\n");
    assert(cfg && cfg->my_subnets->netmask == 128 && !cfg->servers->ipv6_mode);
    assert(cfg->global.control_sock.ss_family == AF_INET6 && cfg->global.reality.version[1] == 255); free_config(cfg);
    cfg = parse("[chat]\n[nat]\n[tcp_proxy_client]\n[tcp_proxy_server]\n");
    assert(cfg && cfg->global.tcp_proxy_max_flows == 1024 && cfg->global.tcp_proxy_max_flows_per_node == 256);
    assert(cfg && cfg->global.chatserver_enabled && cfg->global.db_sync_enabled && cfg->global.nat_enabled &&
        cfg->global.tcp_proxy_client_enabled && cfg->global.tcp_proxy_server_enabled); free_config(cfg);
    cfg = parse("[tcp_proxy_server]\nallowed_group=utun\nallowed_group=e6a1785a9fa59bf5278e4d14c716dbe9782343092f301e02e8a6554998f13d64\nallowed_group=dd9d33df43dfd02381612a908ff3e1296e4862b7121d590a12edefbae7d73d28\nallowed_group=e6a1785a9fa59bf5278e4d14c716dbe9782343092f301e02e8a6554998f13d64\nbind_ip=127.0.0.1\n"
                "[tcp_proxy_client]\ngroup=utun\n");
    assert(cfg && cfg->global.tcp_proxy_group_count == 3 && !memcmp(cfg->global.tcp_proxy_groups[0], (uint8_t[32]){0}, 32) &&
           memcmp(cfg->global.tcp_proxy_groups[1], cfg->global.tcp_proxy_groups[2], 32) < 0); free_config(cfg);
    cfg = parse("[tcp_proxy_server]\nallowed_group=utun\nmax_flows=7\nmax_flows_per_node=3\n");
    assert(cfg && cfg->global.tcp_proxy_max_flows == 7 && cfg->global.tcp_proxy_max_flows_per_node == 3);
    assert(cfg && !cfg->global.tcp_proxy_group_count && !cfg->global.tcp_proxy_groups); free_config(cfg);
#ifdef __linux__
    cfg = parse("[tcp_proxy_server]\nnetif=late-vpn\nso_mark=4294967295\n");
    assert(cfg && !strcmp(cfg->global.tcp_proxy_server_netif, "late-vpn") && cfg->global.tcp_proxy_server_so_mark == UINT32_MAX); free_config(cfg);
#endif
    cfg = parse("[chat]\ncompressor_max_gain_db=0\n");
    assert(cfg && chat_setting_get_int_state(&cfg->global.chat_settings, "compressor_max_gain_db", -1) == 0); free_config(cfg);
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "PASS Android voice compressor configuration accepts and preserves zero gain");
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "PASS IPv4/IPv6 CIDR, control order, port bounds, size limits, link names and section defaults");
}

static void test_rejections(void) {
    const char* bad[] = {
        "[global]\nmy_node_id=1234\n", "[nat]\nnat_via=0x1234\n", "[tcp_proxy_client]\nvia_node=1234\n",
        "[tcp_proxy_client]\nvia_node=0000000000000000000000000000000000000000000000000000000000000000\n",
        "[chatserver]\nallowed_groups=1234\n", "[tcp_proxy_server]\nallowed_group=42\n",
        "[tcp_proxy_server]\nmax_flows=0\n", "[tcp_proxy_server]\nmax_flows_per_node=-1\n",
        "[tcp_proxy_server]\nmax_flows=2147483648\n",
        "[tcp_proxy_server]\nallowed_group=utun,42\n", "[tcp_proxy_client]\ngroup=42\n",
        "[tcp_proxy_server]\nallowed_group=0\n", "[tcp_proxy_server]\nallowed_group=9223372036854775808\n",
        "[tcp_proxy_server]\nbind_ip=example\n", "[tcp_proxy_server]\nnetif=net/0\n", "[tcp_proxy_server]\nso_mark=-1\n",
#ifndef __FreeBSD__
        "[tcp_proxy_server]\nfib=2\n",
#endif
        "[global]\ncrypto_tx_threads=-1\n", "[global]\ncrypto_rx_threads=65\n", "[global]\ncrypto_tx_threads=4x\n",
        "[routing]\nmy_subnet=::1/129\n", "[routing]\nmy_subnet=::1/32junk\n",
        "[routing]\nmy_subnet=127.0.0.1/256\n", "[routing]\nmy_subnet=127.0.0.1/-1\n",
        "[routing]\nmy_subnet=127.0.0.1/\n", "[routing]\nmy_subnet=127.0.0.1/33\n",
        "[routing]\nwrong=1\n", "[control]\nallow=127.0.0.1/256\n", "[control]\nallow=::1/0\n",
        "[control]\nport=65536\n", "[control]\nport=80x\n", "[control]\nport=\n", "[control]\nport=http\n",
        "[control]\nport=1000\nip=\n", "[server:s]\naddr=127.0.0.1:65536\n",
        "[server:s]\naddr=temporary_ipv6:-1\n", "[server:s]\naddr=permanent_ipv6:65536\n",
        "[server:s]\ntransport=typo\n", "[server:s]\ntype=typo\n", "[client:c]\nlink=broken\n",
        "[client:c]\nshort_id=bad\n", "[reality]\nversion=1.2.3tail\n", "[reality]\nversion=256.0.0\n",
        "[reality]\nversion=9999999999999999999999.1.1\n", "[reality]\nshort_ids=01\n",
        "[socks]\naddr=:1080\n", "[socks]\naddr=127.0.0.1:1080x\n", "[socks]\naddr=127.0.0.1:-1\n",
        "[chat]\nstorage_total_size=-1\n", "[chat]\nstorage_unit_size=1MB\n", "[chat]\ncompressor_max_gain_db=-1\n",
        "[chat]\nstorage_total_size=18446744073709551616\n", "[chat]\nstorage_total_size=17179869184G\n",
        "[allowed_keys]\nkey=bad\n", "[network:n]\nid=100000000000000\n", "[network:n]\npubkey=1g\n",
        "[network:n]\nid=1\n", "[ntp]\nserver=\n", "[tcp_proxy_client]\nforward=0 -> 1.2.3.4:80\n",
        "[server:s]\ninvalid\n", "[global]\nkeepalive_interval=65536\n", "[global]\nclient_type=256\n",
        "[global]\nmtu=1280junk\n", "[global]\ndb_sync_ttl=-1\n", "[global]\nauto_sockets=maybe\n",
        "[global]\nkeepalive_timeout=999999999999999999999999\n", "[reality]\ntime_window=3601\n",
        "[reality]\nrelay_idle_timeout=214749\n", "[tcp_proxy_server]\nconnect_timeout=-1\n",
        "[tcp_proxy_client]\nforward=80junk -> 127.0.0.1:80\n", "[tcp_proxy_client]\nforward=80 -> localhost:80\n",
        "[ntp]\ninterval=214749\n", "[ntp]\nenabled=maybe\n", "[socks]\nenabled=maybe\n"
    };
    char path[] = "/tmp/utun-parser-contract-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); close(fd);
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        size_t before = u_get_allocated_count();
        assert(!parse(bad[i]) && u_get_allocated_count() == before);
        FILE* file = fopen(path, "wb"); assert(file && fputs(bad[i], file) >= 0 && !fclose(file));
        assert(!parse_config(path) && u_get_allocated_count() == before);
    }
    unlink(path);
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "PASS %zu invalid configurations rejected identically from file/buffer without leaks", sizeof(bad) / sizeof(*bad));
}

static void test_allocations(void) {
    const char* text = "[global]\nmy_node_name=allocation\n[server:a]\naddr=127.0.0.1:0\n[client:b]\nlink=a:127.0.0.2:1\n"
        "[routing]\nmy_subnet=127.0.0.1/32\n[firewall]\nallow=127.0.0.1\n[control]\nallow=127.0.0.1/32\n"
        "[allowed_keys]\nkey=0101010101010101010101010101010101010101010101010101010101010101\n"
        "[tcp_proxy_server]\nallowed_group=utun\nallowed_group=e6a1785a9fa59bf5278e4d14c716dbe9782343092f301e02e8a6554998f13d64\n[ntp]\nserver=localhost\n[network:n]\n"
        "pubkey=e6a1785a9fa59bf5278e4d14c716dbe9782343092f301e02e8a6554998f13d64\n"
        "signing_key=0303030303030303030303030303030303030303030303030303030303030303\n";
    size_t baseline = u_get_allocated_count();
    unsigned tested;
    for (tested = 1; tested < 64; tested++) {
        allocation = 0; fail_at = tested;
        struct utun_config* cfg = parse(text);
        unsigned calls = allocation; fail_at = 0;
        if (calls < tested) { assert(cfg); free_config(cfg); break; }
        assert(!cfg && u_get_allocated_count() == baseline);
    }
    assert(tested > 8 && tested < 64 && u_get_allocated_count() == baseline);
    struct utun_config* cfg = parse(text); assert(cfg);
    strcpy(cfg->global.my_private_key_hex, "0404040404040404040404040404040404040404040404040404040404040404");
    char path[] = "/tmp/utun-parser-dump-XXXXXX"; int fd = mkstemp(path); assert(fd >= 0); close(fd);
    debug_enable_file_output(path, 1); print_config(cfg); debug_disable_file_output(); debug_enable_console(1);
    FILE* file = fopen(path, "rb"); assert(file);
    char output[8192]; size_t n = fread(output, 1, sizeof(output) - 1, file); output[n] = '\0'; fclose(file); unlink(path);
    assert(strstr(output, "[network:n]") && !strstr(output, cfg->global.my_private_key_hex) && !strstr(output, cfg->networks->signing_key_hex));
    assert(update_config_keys("/dev/full", cfg->global.my_private_key_hex, cfg->networks->pubkey_hex) < 0);
    free_config(cfg); assert(u_get_allocated_count() == baseline);
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "PASS %u allocation faults, redacted independent network dump and failed key append", tested - 1);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO); assert(!socket_platform_init());
    struct utun_config* policy = parse("[global]\nconnection_order=RUT2-1\n");
    assert(policy && !strcmp(policy->global.connection_order, "RUT2-1")); free_config(policy);
    assert(!parse("[global]\nconnection_order=TT1\n"));
    policy = parse("[global]\nhide_my_sockets=yes\n");
    assert(policy && policy->global.hide_my_sockets == 1); free_config(policy);
    policy = parse("[global]\nhide_my_sockets=no\n");
    assert(policy && policy->global.hide_my_sockets == 0); free_config(policy);
    policy = parse("[global]\n");
    assert(policy && policy->global.hide_my_sockets == 0); free_config(policy);
    assert(!parse("[global]\nhide_my_sockets=invalid\n"));
    test_values(); test_rejections(); test_allocations(); socket_platform_cleanup(); return 0;
}
