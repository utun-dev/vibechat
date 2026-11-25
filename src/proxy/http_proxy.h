/* Потоковый HTTP/1.x codec локального proxy. Не владеет сокетами/DNS/ETCP.
 * Заголовки проверяются целиком до пересылки; тело передаётся ограниченными порциями.
 * Для нового сообщения обнулить message/request, затем headers_feed → request/response_parse → body_feed.
 * parse меняет headers и сохраняет в fields указатели внутрь него: message нельзя копировать после parse.
 * emit вызывается синхронно, буфер заимствован только на время вызова; source_len нужен для возврата
 * ETCP credit после доставки результата. Ошибку feed/parse нельзя продолжать как успешное сообщение.
 * Возврат feed: 0=нужно продолжение, 1=заголовки/тело завершены, <0=отрицательный HTTP status.
 * HTTP_BODY_EOF завершает владелец по FIN, отдельной функции EOF у codec нет. */
#ifndef HTTP_PROXY_H
#define HTTP_PROXY_H

#include <stddef.h>
#include <stdint.h>

#define HTTP_PROXY_LINE_LIMIT 8192
#define HTTP_PROXY_HEADER_LIMIT 32768
#define HTTP_PROXY_FIELDS_LIMIT 512

// Результат: >=0 = принято целиком, <0 = ошибка вывода (codec вернёт -500); частичный приём не поддержан.
// source_len может отличаться от len; len=0/data=NULL допустимы при удалении исходных байтов.
typedef int (*http_proxy_emit)(void* arg, const uint8_t* data, size_t len, size_t source_len);

struct http_proxy_field { char* name; char* value; }; // Заимствованные NUL-строки внутри message.headers.
// NONE — исходное нулевое состояние; EOF — тело до внешнего FIN; DONE — точная граница сообщения.
enum http_proxy_body_state { HTTP_BODY_NONE, HTTP_BODY_LENGTH, HTTP_BODY_EOF, HTTP_BODY_CHUNK_SIZE,
                            HTTP_BODY_CHUNK_DATA, HTTP_BODY_CHUNK_CRLF, HTTP_BODY_TRAILERS, HTTP_BODY_DONE };

struct http_proxy_message {
    char headers[HTTP_PROXY_HEADER_LIMIT + 1]; // До parse — CRLF-блок, после — строки с NUL-разделителями.
    size_t header_len;                        // Число исходных байтов, включая CRLFCRLF.
    size_t first_line_len;                    // Длина стартовой строки с CRLF.
    struct http_proxy_field fields[HTTP_PROXY_FIELDS_LIMIT]; // Разобранные поля собственного headers.
    unsigned field_count;                    // Число заполненных fields.
    uint8_t headers_done;                    // headers_feed получил полный CRLFCRLF.
    uint8_t minor;                           // Версия исходного HTTP: 0 или 1.
    uint8_t close;                           // Запрос: Connection close; ответ: решение закрыть frontend.
    uint8_t chunked;                         // Единственный поддержанный Transfer-Encoding — chunked.
    uint8_t has_length;                      // Присутствует проверенный Content-Length (включая нулевой).
    uint8_t upgrade;                         // В Connection найден token upgrade.
    uint8_t decode_chunks;                   // Убрать framing chunked для ответа HTTP/1.0.
    unsigned status;                         // HTTP status ответа; у запроса 0.
    uint64_t length;                         // Content-Length; отсутствие различает has_length.
    enum http_proxy_body_state body;         // Состояние потокового разбора тела.
    uint64_t remaining;                      // Остаток Content-Length или текущего chunk.
    char line[HTTP_PROXY_LINE_LIMIT + 1];     // Неполная строка размера chunk или trailer.
    size_t line_len;                         // Сколько байт накоплено в line.
    size_t trailer_len;                      // Суммарный размер trailers, лимит HEADER_LIMIT.
    unsigned crlf_pos;                       // 0/1: сколько байт CRLF после chunk уже принято.
};

struct http_proxy_request {
    char host[256];                          // IPv4 literal или DNS-имя; IPv6 отклоняется с -502.
    char authority[264];                     // Нормализованный host:port для upstream Host.
    char upgrade[HTTP_PROXY_LINE_LIMIT + 1];  // Предложенные protocol[/version], пусто без Upgrade.
    uint16_t port;                           // Host byte order; HTTP default=80, CONNECT требует порт.
    uint8_t connect;                         // Метод CONNECT: после успеха используется сырой туннель.
    uint8_t head;                            // Метод HEAD: у ответа нет тела.
    uint8_t close;                           // Закрыть frontend: HTTP/1.0 или Connection close.
    uint8_t minor;                           // Версия клиента, определяет сериализацию ответа.
};

// Накапливает заголовки из заимствованного data; *used = число принятых байтов, хвост остаётся вызывающему.
// m первоначально обнулён; 1 = CRLFCRLF получен, 0 = ждать продолжения, <0 = -400/-414/-431.
int http_proxy_headers_feed(struct http_proxy_message* m, const uint8_t* data, size_t len, size_t* used);
// Однократно после headers_feed==1: проверяет запрос, заполняет обнулённый r и body в m.
// Обычный запрос требует absolute http:// URL; emit получает origin-form HTTP/1.1 headers.
// CONNECT проверяет authority и не вызывает emit. 0 = успех, <0 = отрицательный HTTP status.
int http_proxy_request_parse(struct http_proxy_message* m, struct http_proxy_request* r, http_proxy_emit emit, void* arg);
// Однократно после headers_feed==1; r — исходный запрос, close_client — решение закрыть frontend.
// Проверяет HEAD/Upgrade, задаёт body, выводит headers в версии клиента. 0 = успех, <0 = -502/-500.
// Для следующего промежуточного ответа владелец обнуляет m; r сохраняется до final response.
int http_proxy_response_parse(struct http_proxy_message* m, const struct http_proxy_request* r,
                              int close_client, http_proxy_emit emit, void* arg);
// После parse: передаёт тело из data через emit, *used = принятые байты до точной границы тела.
// Фильтрует trailers; 1 = DONE, 0 = продолжить, <0 = отрицательный HTTP status.
// Для HTTP_BODY_EOF возвращает 0, завершение по FIN проверяет владелец. Вывод не откатывается при ошибке.
int http_proxy_body_feed(struct http_proxy_message* m, const uint8_t* data, size_t len, size_t* used,
                         http_proxy_emit emit, void* arg);

#endif
