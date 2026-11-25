/* Обычный TLS 1.3 и HTTP-запрос проходят через REALITY fallback на локальный HTTPS. */
#define OPENSSL_API_COMPAT 0x10100000L
#include "stcp_server.h"
#include "utun_instance.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/err.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "FAIL line=%d: %s", __LINE__, #x); return 1; } } while (0)
struct https_peer {
    struct UASYNC *ua;
    SSL *ssl;
    void *socket_id;
    socket_t sock;
    uint8_t ready, done, failed;
    uint8_t response[256]; size_t response_len;
};
struct https_server { struct UASYNC *ua; SSL_CTX *ctx; socket_t sock; struct https_peer peer; };
static const char response[] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello";

static void https_progress(socket_t sock, void *arg) {
    struct https_peer *p = arg;
    int rc;
    if (!p->ready) {
        rc = SSL_accept(p->ssl);
        if (rc == 1) p->ready = 1;
        else {
            int error = SSL_get_error(p->ssl, rc);
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                uasync_set_socket_write(p->ua, p->socket_id, error == SSL_ERROR_WANT_WRITE);
                return;
            }
            DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "HTTPS handshake failed: ssl=%d", error);
            p->failed = 1;
            return;
        }
    }
    if (!p->done) {
        uint8_t request[512];
        rc = SSL_read(p->ssl, request, sizeof(request));
        if (rc > 0) {
            if (rc < 4 || memcmp(request, "GET ", 4)) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "invalid HTTP request"); p->failed = 1; return; }
            rc = SSL_write(p->ssl, response, sizeof(response) - 1);
            if (rc != sizeof(response) - 1) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "HTTPS response write failed"); p->failed = 1; return; }
            p->done = 1;
            uasync_set_socket_write(p->ua, p->socket_id, 0);
            DEBUG_INFO(DEBUG_CATEGORY_REALITY, "local HTTPS received HTTP request through relay");
        } else {
            int error = SSL_get_error(p->ssl, rc);
            if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
                DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "HTTPS read failed: ssl=%d openssl=%lu", error, ERR_get_error());
                p->failed = 1;
            }
            uasync_set_socket_write(p->ua, p->socket_id, error == SSL_ERROR_WANT_WRITE);
        }
    }
    (void)sock;
}
static void https_accept(socket_t sock, void *arg) {
    struct https_server *s = arg;
    struct https_peer *p = &s->peer;
    p->sock = accept(sock, NULL, NULL); p->ua = s->ua;
    if (p->sock == SOCKET_INVALID) { p->failed = 1; return; }
    socket_set_nonblocking(p->sock);
    p->ssl = SSL_new(s->ctx);
    if (!p->ssl || SSL_set_fd(p->ssl, p->sock) != 1) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "HTTPS SSL setup failed"); p->failed = 1; return; }
    p->socket_id = uasync_add_socket_t(s->ua, p->sock, https_progress, https_progress, NULL, "test_https", p);
    if (!p->socket_id) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "HTTPS socket registration failed"); p->failed = 1; }
}
static void unexpected_stcp(struct stcp_conn *c, void *arg) {
    (void)c; (void)arg;
    DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "ordinary HTTPS client entered STCP");
}
static int client_step(SSL *client, int rc) {
    if (rc > 0) return rc;
    int error = SSL_get_error(client, rc);
    return error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE ? 0 : -1;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    size_t allocated = u_get_allocated_count();
    struct UASYNC *ua = uasync_create(); CHECK(ua);
    struct UTUN_INSTANCE inst = {0}; inst.ua = ua;
    struct SC_MYKEYS keys; uint8_t ed[32], seed[32];
    CHECK(sc_identity_generate(keys.public_key, keys.private_key, ed, seed) == SC_OK);
    EVP_PKEY_CTX *key_ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL); EVP_PKEY *key = NULL;
    CHECK(key_ctx && EVP_PKEY_keygen_init(key_ctx) == 1 && EVP_PKEY_keygen(key_ctx, &key) == 1);
    X509 *cert = X509_new(); CHECK(cert);
    CHECK(X509_set_version(cert, 2) && ASN1_INTEGER_set(X509_get_serialNumber(cert), 1));
    CHECK(X509_gmtime_adj(X509_getm_notBefore(cert), 0) && X509_gmtime_adj(X509_getm_notAfter(cert), 3600));
    X509_NAME *name = X509_get_subject_name(cert);
    CHECK(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"localhost", -1, -1, 0));
    CHECK(X509_set_issuer_name(cert, name) && X509_set_pubkey(cert, key) && X509_sign(cert, key, NULL) > 0);
    struct https_server server = {0}; server.ua = ua; server.peer.sock = SOCKET_INVALID;
    server.ctx = SSL_CTX_new(TLS_server_method()); SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
    CHECK(server.ctx && client_ctx && SSL_CTX_use_certificate(server.ctx, cert) && SSL_CTX_use_PrivateKey(server.ctx, key));
    CHECK(SSL_CTX_set_min_proto_version(server.ctx, TLS1_3_VERSION) && SSL_CTX_set_min_proto_version(client_ctx, TLS1_3_VERSION));
    CHECK(SSL_CTX_set1_groups_list(client_ctx, "X25519") && SSL_CTX_set_ciphersuites(client_ctx, "TLS_AES_128_GCM_SHA256"));
    SSL_CTX_set_num_tickets(server.ctx, 0); SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    server.sock = socket(AF_INET, SOCK_STREAM, 0); CHECK(server.sock != SOCKET_INVALID);
    struct sockaddr_in address = {0}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(!bind(server.sock, (struct sockaddr *)&address, sizeof(address)) && !listen(server.sock, 1));
    socklen_t size = sizeof(address); CHECK(!getsockname(server.sock, (struct sockaddr *)&address, &size));
    socket_set_nonblocking(server.sock);
    CHECK(uasync_add_socket_t(ua, server.sock, https_accept, NULL, NULL, "test_https_listener", &server));
    struct reality_config cfg; reality_config_set_defaults(&cfg);
    cfg.enabled = 1; cfg.short_id_count = 1;
    uint8_t pub[32]; CHECK(!reality_generate_keypair(cfg.private_key, pub));
    snprintf(cfg.dest, sizeof(cfg.dest), "127.0.0.1:%u", ntohs(address.sin_port));
    struct stcp_server *reality = stcp_server_create(ua, 0, &keys, ed, &inst, unexpected_stcp, NULL, NULL, NULL, AF_INET);
    CHECK(reality); stcp_server_set_reality(reality, &cfg);
    uint16_t port = stcp_server_get_port(reality); CHECK(port);
    DEBUG_INFO(DEBUG_CATEGORY_REALITY, "test listeners: REALITY=127.0.0.1:%u HTTPS=%s", port, cfg.dest);
    socket_t sock = socket(AF_INET, SOCK_STREAM, 0); CHECK(sock != SOCKET_INVALID);
    address.sin_port = htons(port); CHECK(!connect(sock, (struct sockaddr *)&address, sizeof(address)));
    socket_set_nonblocking(sock);
    SSL *client = SSL_new(client_ctx); CHECK(client && SSL_set_fd(client, sock));
    CHECK(SSL_set_tlsext_host_name(client, "localhost"));
    int ready = 0;
    for (unsigned i = 0; !ready && !server.peer.failed && i < 1000; i++) {
        ready = client_step(client, SSL_connect(client)); CHECK(ready >= 0); uasync_poll(ua, 1);
    }
    CHECK(ready == 1 && SSL_version(client) == TLS1_3_VERSION);
    const char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    CHECK(SSL_write(client, request, sizeof(request) - 1) == sizeof(request) - 1);
    uint8_t data[256]; size_t received = 0;
    for (unsigned i = 0; received < sizeof(response) - 1 && !server.peer.failed && i < 1000; i++) {
        int n = client_step(client, SSL_read(client, data + received, sizeof(data) - received));
        CHECK(n >= 0); received += n; uasync_poll(ua, 1);
    }
    DEBUG_INFO(DEBUG_CATEGORY_REALITY, "HTTPS result: ready=%u done=%u failed=%u received=%zu", server.peer.ready, server.peer.done, server.peer.failed, received);
    CHECK(!server.peer.failed && received == sizeof(response) - 1 && !memcmp(data, response, received));
    CHECK(inst.reality_owner.relay_count == 1);
    SSL_free(client); socket_close_wrapper(sock);
    stcp_server_destroy(reality); reality_owner_cleanup(&inst.reality_owner);
    if (server.peer.sock != SOCKET_INVALID) {
        uasync_remove_socket_t(ua, server.peer.sock); SSL_free(server.peer.ssl); socket_close_wrapper(server.peer.sock);
    }
    uasync_remove_socket_t(ua, server.sock); socket_close_wrapper(server.sock);
    SSL_CTX_free(client_ctx); SSL_CTX_free(server.ctx); X509_free(cert); EVP_PKEY_free(key); EVP_PKEY_CTX_free(key_ctx);
    uasync_drain_immediate(ua); uasync_destroy(ua, 1); CHECK(u_get_allocated_count() == allocated);
    DEBUG_INFO(DEBUG_CATEGORY_REALITY, "real TLS 1.3 + HTTP over fallback passed without leaked resources");
    return 0;
}
