# Протокол proxy

Актуальное описание TCP/UDP/ICMP, router-метаданных, окон, half-close и проверок:
[doc/proxy_protocol.md](../../doc/proxy_protocol.md).

TCP ожидает реальный CONNECTED и использует WINDOW; FIN завершает одно направление.
Прежние схемы без CONNECTED, с общим TCP service ID и 9-байтовым CLOSE_ALL не соответствуют
реализации. Общие определения и контракт proxy_flow находятся в [proxy_protocol.h](proxy_protocol.h).

Описание отдельных модулей:

- [TCP client](tcp_proxy_client_doc.md) — TUN/lwIP, маршрутизация к exit, приём ответов.
- [TCP server](tcp_proxy_server_doc.md) — OS connect, окна, очереди, FIN/CLOSE/ERROR.
- [SOCKS и HTTP](socks_proxy_doc.md) — listeners, DNS, HTTP framing/keep-alive/туннели.
- [UDP](udp_proxy_doc.md) — per-instance flows и восстановление ответа в TUN.
- [ICMP](icmp_proxy_doc.md) — raw socket, сопоставление echo, тестовый fallback.

Контракты функций, буферов и полей HTTP codec — в [http_proxy.h](http_proxy.h).
