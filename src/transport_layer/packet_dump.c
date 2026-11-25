#include "packet_dump.h"
#include "../lib/debug_config.h"
#include <stdio.h>
#include <string.h>

static const char* tcp_flags_to_str(uint8_t flags) {
    static char buf[32];
    static const char* const names[] = {"FIN", "SYN", "RST", "PSH", "ACK", "URG", "ECE", "CWR"};
    size_t pos = 0;
    for (unsigned i = 0; i < 8; i++) {
        if (!(flags & (1u << i))) continue;
        if (pos) buf[pos++] = ',';
        memcpy(buf + pos, names[i], 3);
        pos += 3;
    }
    buf[pos] = '\0';
    return pos ? buf : "none";
}

char* dump_ip_packet_to_buffer(const uint8_t* data, size_t len) {
    static char buffer[1024];
    if (!data) {
        snprintf(buffer, sizeof(buffer), "Missing IP packet: len=%zu", len);
        goto invalid;
    }
    if (len < 20) {
        snprintf(buffer, sizeof(buffer), "Packet too small: %zu bytes", len);
        goto invalid;
    }
    if ((data[0] >> 4) != 4) {
        snprintf(buffer, sizeof(buffer), "Non-IPv4 packet: version=%u len=%zu", (unsigned)(data[0] >> 4), len);
        goto invalid;
    }

    size_t ihl = (data[0] & 0x0f) * 4u;
    if (ihl < 20) {
        snprintf(buffer, sizeof(buffer), "Invalid IP header length: ihl=%zu len=%zu", ihl, len);
        goto invalid;
    }
    if (ihl > len) {
        snprintf(buffer, sizeof(buffer), "Truncated IP header: ihl=%zu len=%zu", ihl, len);
        goto invalid;
    }
    size_t total_len = ((unsigned)data[2] << 8) | data[3];
    if (total_len < ihl) {
        snprintf(buffer, sizeof(buffer), "Invalid IP total length: total=%zu ihl=%zu", total_len, ihl);
        goto invalid;
    }
    if (total_len > len) {
        snprintf(buffer, sizeof(buffer), "Truncated IPv4 packet: total=%zu len=%zu", total_len, len);
        goto invalid;
    }

    char src[24], dst[24], details[128] = "", fragment[64] = "";
    snprintf(src, sizeof(src), "%u.%u.%u.%u", data[12], data[13], data[14], data[15]);
    snprintf(dst, sizeof(dst), "%u.%u.%u.%u", data[16], data[17], data[18], data[19]);
    uint8_t protocol = data[9];
    const char* name = protocol == 6 ? "TCP" : protocol == 17 ? "UDP" : protocol == 1 ? "ICMP" : "UNKNOWN";
    unsigned frag = ((unsigned)data[6] << 8) | data[7];
    unsigned frag_offset = (frag & 0x1fff) * 8u;
    int more_fragments = (frag & 0x2000) != 0;
    if (frag_offset || more_fragments)
        snprintf(fragment, sizeof(fragment), " fragment offset=%u MF=%d", frag_offset, more_fragments);

    const uint8_t* payload = data + ihl;
    size_t payload_len = total_len - ihl;
    unsigned required = protocol == 6 ? 20 : (protocol == 17 || protocol == 1) ? 8 : 0;
    if (!frag_offset && payload_len < required) {
        snprintf(details, sizeof(details), "%s header: available=%zu required=%u",
                 more_fragments ? "partial" : "truncated", payload_len, required);
        if (more_fragments) DEBUG_DEBUG(DEBUG_CATEGORY_DUMP, "packet_dump: %s %s", name, details);
        else DEBUG_WARN(DEBUG_CATEGORY_DUMP, "packet_dump: %s %s", name, details);
    } else if (!frag_offset && (protocol == 6 || protocol == 17)) {
        unsigned src_port = ((unsigned)payload[0] << 8) | payload[1];
        unsigned dst_port = ((unsigned)payload[2] << 8) | payload[3];
        size_t src_len = strlen(src), dst_len = strlen(dst);
        snprintf(src + src_len, sizeof(src) - src_len, ":%u", src_port);
        snprintf(dst + dst_len, sizeof(dst) - dst_len, ":%u", dst_port);
        if (protocol == 6) {
            uint32_t seq = ((uint32_t)payload[4] << 24) | ((uint32_t)payload[5] << 16) |
                           ((uint32_t)payload[6] << 8) | payload[7];
            uint32_t ack = ((uint32_t)payload[8] << 24) | ((uint32_t)payload[9] << 16) |
                           ((uint32_t)payload[10] << 8) | payload[11];
            unsigned window = ((unsigned)payload[14] << 8) | payload[15];
            snprintf(details, sizeof(details), "[%s] seq=%u ack=%u win=%u", tcp_flags_to_str(payload[13]), seq, ack, window);
        } else {
            unsigned udp_len = ((unsigned)payload[4] << 8) | payload[5];
            snprintf(details, sizeof(details), "len=%u", udp_len);
        }
    } else if (!frag_offset && protocol == 1) {
        unsigned type = payload[0];
        if (type == 0 || type == 8) {
            unsigned id = ((unsigned)payload[4] << 8) | payload[5];
            unsigned seq = ((unsigned)payload[6] << 8) | payload[7];
            snprintf(details, sizeof(details), "%s seq=%u id=%u", type == 0 ? "echo_reply" : "echo_request", seq, id);
        } else snprintf(details, sizeof(details), "type=%u code=%u", type, payload[1]);
    }

    /* Максимум 32 байта, три пробела между группами и завершающий NUL. */
    static const char hex_digits[] = "0123456789abcdef";
    char hex[68];
    size_t count = payload_len < 32 ? payload_len : 32, pos = 0;
    for (size_t i = 0; i < count; i++) {
        if (i && i % 8 == 0) hex[pos++] = ' ';
        hex[pos++] = hex_digits[payload[i] >> 4];
        hex[pos++] = hex_digits[payload[i] & 0x0f];
    }
    hex[pos] = '\0';
    snprintf(buffer, sizeof(buffer), "IPv4 %s %s -> %s (%zu bytes)%s%s%s data: %s",
             name, src, dst, total_len, details[0] ? " " : "", details, fragment, hex);
    return buffer;

invalid:
    DEBUG_WARN(DEBUG_CATEGORY_DUMP, "packet_dump: %s", buffer);
    return buffer;
}
