// Проверки HTTP codec на каждом возможном разбиении headers и побайтовом chunked body.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "proxy/http_proxy.h"
#include "debug_config.h"
#include "mem.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
struct output { char bytes[100000]; size_t len, source; };

static int collect(void* arg, const uint8_t* data, size_t len, size_t source) {
    struct output* out = arg;
    CHECK(out->len + len < sizeof(out->bytes));
    if (len) memcpy(out->bytes + out->len, data, len);
    out->len += len; out->source += source; out->bytes[out->len] = 0;
    return 0;
}

static int request(const char* text, struct http_proxy_message* m, struct http_proxy_request* r, struct output* out) {
    memset(m, 0, sizeof(*m)); memset(r, 0, sizeof(*r)); memset(out, 0, sizeof(*out));
    size_t used;
    int ret = http_proxy_headers_feed(m, (const uint8_t*)text, strlen(text), &used);
    if (ret <= 0) return ret;
    return http_proxy_request_parse(m, r, collect, out);
}

static int response(const char* text, struct http_proxy_message* m, const struct http_proxy_request* r, struct output* out) {
    memset(m, 0, sizeof(*m)); memset(out, 0, sizeof(*out));
    size_t used;
    int ret = http_proxy_headers_feed(m, (const uint8_t*)text, strlen(text), &used);
    if (ret <= 0) return ret;
    return http_proxy_response_parse(m, r, r->close, collect, out);
}

static void header_tests(void) {
    const char text[] = "POST http://example.org:8080?x=1 HTTP/1.1\r\nHost: wrong.example\r\n"
                        "Proxy-Authorization: Basic secret\r\nConnection: X-Private, keep-alive\r\n"
                        "X-Private: secret\r\nAuthorization: Bearer origin\r\nContent-Length: 3\r\n\r\nabcNEXT";
    size_t header_len = strstr(text, "\r\n\r\n") + 4 - text;
    for (size_t split = 0; split <= header_len; split++) {
        struct http_proxy_message m = {0}; struct http_proxy_request r = {0}; struct output out = {0}; size_t used;
        CHECK(http_proxy_headers_feed(&m, (const uint8_t*)text, split, &used) == (split == header_len));
        CHECK(used == split && out.source == 0);
        CHECK(http_proxy_headers_feed(&m, (const uint8_t*)text + split, sizeof(text)-1-split, &used) == 1);
        CHECK(used == header_len - split);
        CHECK(http_proxy_request_parse(&m, &r, collect, &out) == 0);
        CHECK(!strcmp(r.host, "example.org") && r.port == 8080);
        CHECK(strstr(out.bytes, "POST /?x=1 HTTP/1.1\r\nHost: example.org:8080\r\n"));
        CHECK(!strstr(out.bytes, "secret") && !strstr(out.bytes, "wrong.example"));
        CHECK(strstr(out.bytes, "Authorization: Bearer origin\r\n"));
        CHECK(strstr(out.bytes, "Connection: close\r\n") && out.source == header_len);
        CHECK(http_proxy_body_feed(&m, (const uint8_t*)text + header_len, 7, &used, collect, &out) == 1);
        CHECK(used == 3 && out.source == header_len + 3 && !strcmp(out.bytes + out.len - 3, "abc"));
    }
    struct http_proxy_message m; struct http_proxy_request r; struct output out;
    CHECK(request("OPTIONS http://example.org HTTP/1.1\r\nHost: ignored\r\n\r\n", &m, &r, &out) == 0);
    CHECK(strstr(out.bytes, "OPTIONS * HTTP/1.1\r\n"));
    CHECK(request("CONNECT example.org:443 HTTP/1.1\r\nHost: ignored\r\n\r\n", &m, &r, &out) == 0);
    CHECK(r.connect && r.port == 443 && out.len == 0);
    const char* invalid[] = {
        "CONNECT example.org:443junk HTTP/1.1\r\nHost: x\r\n\r\n",
        "CONNECT example.org:443 GARBAGE\r\nHost: x\r\n\r\n",
        "CONNECT example.org HTTP/1.1\r\nHost: x\r\n\r\n",
        "CONNECT example.org:443 HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\n",
        "GET http://x/ HTTP/1.1\r\n\r\n",
        "GET http://x/ HTTP/1.1\r\nHost: x\r\nHost: x\r\n\r\n",
        "GET http://x/ HTTP/1.1\r\nHost : x\r\n\r\n",
        "GET http://x/ HTTP/1.1\nHost: x\n\n",
        "GET http://x/ HTTP/1.1\r\nHost: x\r\nX: a\r\n folded\r\n\r\n",
        "POST http://x/ HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n",
        "POST http://x/ HTTP/1.1\r\nHost: x\r\nContent-Length: 18446744073709551616\r\n\r\n",
        "POST http://x/ HTTP/1.1\r\nHost: x\r\nContent-Length: +1\r\n\r\n",
        "POST http://x/ HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "POST http://x/ HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked, chunked\r\n\r\n",
        "POST http://x/ HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\n\r\n",
        "GET http://x/ HTTP/1.1\r\nHost: x\r\nConnection: Host\r\n\r\n",
        "GET http://x/ HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: bad/version/extra\r\n\r\n",
        "GET http://user@x/ HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET http://x:65536/ HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET http://x:0/ HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET http://x/#fragment HTTP/1.1\r\nHost: x\r\n\r\n"
    };
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
        CHECK(request(invalid[i], &m, &r, &out) == -400 && out.len == 0);
    }
    CHECK(request("GET http://[::1]/ HTTP/1.1\r\nHost: [::1]\r\n\r\n", &m, &r, &out) == -502);
    char long_text[40000]; int n = snprintf(long_text, sizeof(long_text), "GET http://x/");
    memset(long_text+n, 'a', 3000); n += 3000;
    snprintf(long_text+n, sizeof(long_text)-n, " HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(request(long_text, &m, &r, &out) == 0);
    n = snprintf(long_text, sizeof(long_text), "POST http://x/ HTTP/1.1\r\nHost: x\r\nX-Pad: ");
    memset(long_text+n, 'a', 30000); n += 30000;
    snprintf(long_text+n, sizeof(long_text)-n, "\r\nContent-Length: 2000\r\n\r\n");
    CHECK(request(long_text, &m, &r, &out) == 0);
    n = snprintf(long_text, sizeof(long_text), "GET http://x/ HTTP/1.1\r\nHost: x\r\nX-Pad: ");
    memset(long_text+n, 'a', 33000); long_text[n+33000] = 0;
    CHECK(request(long_text, &m, &r, &out) == -431);
    puts("[PASS] HTTP strict headers, absolute URL/Host, hop fields, arbitrary splits and limits");
}

static void body_tests(void) {
    struct http_proxy_message m; struct http_proxy_request r; struct output out;
    const char header[] = "POST http://x/ HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: X-Hop\r\n\r\n";
    const char body[] = "3 ; foo=\"x\\\"y\"\r\nabc\r\n2\r\nde\r\n0\r\nX-End: yes\r\nX-Hop: secret\r\nProxy-Authorization: secret\r\n\r\n";
    CHECK(request(header, &m, &r, &out) == 0);
    size_t head_out = out.len;
    for (size_t i = 0; i < sizeof(body)-1; i++) {
        size_t used;
        CHECK(http_proxy_body_feed(&m, (const uint8_t*)body+i, 1, &used, collect, &out) == (i == sizeof(body)-2));
        CHECK(used == 1 && out.source <= strlen(header)+i+1);
    }
    CHECK(out.source == strlen(header)+strlen(body));
    CHECK(!strcmp(out.bytes+head_out, "3 ; foo=\"x\\\"y\"\r\nabc\r\n2\r\nde\r\n0\r\nX-End: yes\r\n\r\n"));
    const char* bad[] = {"Z\r\n", "10000000000000000\r\n", "1\r\na!", "1\na", "0\r\nContent-Length: 1\r\n\r\n",
                         "0\r\nHost: x\r\n\r\n", "0\r\n folded\r\n\r\n", "1;foo=\"bad\r\n"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        size_t used; CHECK(request(header, &m, &r, &out) == 0);
        CHECK(http_proxy_body_feed(&m, (const uint8_t*)bad[i], strlen(bad[i]), &used, collect, &out) == -400);
    }
    r = (struct http_proxy_request){.minor=0,.close=1};
    const char reply[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    CHECK(response(reply, &m, &r, &out) == 0 && m.decode_chunks);
    CHECK(!strstr(out.bytes, "Transfer-Encoding") && strstr(out.bytes, "HTTP/1.0 200 OK\r\n"));
    head_out = out.len;
    for (size_t i = 0; i < sizeof(body)-1; i++) {
        size_t used; CHECK(http_proxy_body_feed(&m, (const uint8_t*)body+i, 1, &used, collect, &out) >= 0);
    }
    CHECK(!strcmp(out.bytes+head_out, "abcde") && out.source == strlen(reply)+strlen(body));
    puts("[PASS] chunked boundaries, extensions/trailers, source byte credit and HTTP/1.0 decoding");
}

static void response_tests(void) {
    struct http_proxy_message m; struct output out; struct http_proxy_request r = {.minor=1};
    CHECK(response("HTTP/1.1 100 Continue\r\n\r\n", &m, &r, &out) == 0 && m.body == HTTP_BODY_DONE);
    CHECK(response("HTTP/1.1 204 No Content\r\n\r\n", &m, &r, &out) == 0 && m.body == HTTP_BODY_DONE);
    CHECK(response("HTTP/1.1 304 Not Modified\r\nContent-Length: 999\r\n\r\n", &m, &r, &out) == 0 && m.body == HTTP_BODY_DONE);
    CHECK(response("HTTP/1.1 200 OK\r\n\r\n", &m, &r, &out) == 0 && m.body == HTTP_BODY_EOF && m.close);
    CHECK(strstr(out.bytes, "Connection: close\r\n"));
    r.head = 1;
    CHECK(response("HTTP/1.1 200 OK\r\nContent-Length: 999\r\n\r\n", &m, &r, &out) == 0 && m.body == HTTP_BODY_DONE);
    r.head = 0;
    CHECK(response("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n", &m, &r, &out) == -502 && out.len == 0);
    CHECK(response("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n", &m, &r, &out) == -502);
    CHECK(response("HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n", &m, &r, &out) == -502);
    strcpy(r.upgrade, "websocket, example/1.2");
    CHECK(response("HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: example/1.2\r\n\r\n", &m, &r, &out) == 0);
    CHECK(strstr(out.bytes, "Connection: Upgrade\r\nUpgrade: example/1.2\r\n"));
    CHECK(response("HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: other\r\n\r\n", &m, &r, &out) == -502);
    r.minor = 0; r.close = 1; r.upgrade[0] = 0;
    CHECK(response("HTTP/1.1 100 Continue\r\n\r\n", &m, &r, &out) == 0 && out.len == 0 && out.source > 0);
    puts("[PASS] HEAD/1xx/204/304/EOF response framing and negotiated Upgrade");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    header_tests(); body_tests(); response_tests();
    CHECK(u_get_allocated_count() == 0);
    return 0;
}
