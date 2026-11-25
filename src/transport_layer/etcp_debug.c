#include "etcp_debug.h"
#include "etcp.h"
#include "../lib/debug_config.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

IP_STR ip_to_string(uint32_t ip) {
    uint8_t* b = (uint8_t*)&ip;
    IP_STR ip_str;
    snprintf(ip_str.a, sizeof(ip_str.a), "%u.%u.%u.%u", b[3], b[2], b[1], b[0]);
    return ip_str;
}

// Строит дамп только при включённой категории; длинные цепочки секций безопасно усекаются.
void etcp_dump_pkt_sections(struct ETCP_DGRAM* pkt, struct ETCP_LINK* link, int is_send) {
    if (!pkt || !debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_DUMP)) return;

    char buf[256];
    int pos = 0;
    const char* dir = is_send ? "SEND" : "RECV";

    pos += snprintf(buf + pos, sizeof(buf) - pos, "ETCP PKT: %s ts=%u", dir, pkt->timestamp);

    uint8_t* data = pkt->data;
    uint16_t len = pkt->data_len;

    uint32_t ack_last_delivered = 0;
    int ack_count = 0;
    uint16_t ch_ret_ts = 0;
    uint16_t ch_recv_ts = 0;
    uint32_t payload_seq = 0;
    uint16_t payload_size = 0;
    int has_channel_ts = 0;
    int has_payload = 0;

    uint16_t offset = 0;
    while (offset < len && pos < (int)sizeof(buf) - 1) {
        uint8_t type = data[offset];

        switch (type) {
            case ETCP_SECTION_ACK: {
                if (offset + 2 > len) { offset = len; break; }
                ack_count = data[offset + 1];
                if (offset + 8 > len) { offset = len; break; }
                ack_last_delivered = (uint32_t)data[offset + 2] | ((uint32_t)data[offset + 3] << 8) |
                                     ((uint32_t)data[offset + 4] << 16) | ((uint32_t)data[offset + 5] << 24);
                uint16_t ack_rx_dup = data[offset + 6] | (data[offset + 7] << 8);
                pos += snprintf(buf + pos, sizeof(buf) - pos, " ack=%u/%d dup=%u", ack_last_delivered, ack_count, ack_rx_dup);
                unsigned section_len = etcp_ack_section_size((uint8_t)ack_count);
                if (offset + section_len > len) {
                    DEBUG_WARN(DEBUG_CATEGORY_DUMP, "Short ACK section: count=%d remaining=%u required=%u",
                        ack_count, len-offset, section_len);
                    offset = len; break;
                }
                if (ack_count && pos < (int)sizeof(buf) - 1) {
                    const uint8_t* point = data + offset + ETCP_ACK_BASE_SIZE + ack_count * ETCP_ACK_ENTRY_SIZE;
                    uint64_t bytes = 0, time = 0, created = 0;
                    for (unsigned i = 0; i < 8; i++) {
                        bytes |= (uint64_t)point[i] << (8*i); time |= (uint64_t)point[i+8] << (8*i);
                        created |= (uint64_t)point[i+16] << (8*i);
                    }
                    pos += snprintf(buf + pos, sizeof(buf) - pos, " rx=%llu/%lluus ACKtime=%llu",
                        (unsigned long long)bytes, (unsigned long long)time, (unsigned long long)created);
                }
                offset += section_len;
                break;
            }
            case ETCP_SECTION_TIMESTAMP: {
                if (offset + 5 > len) { offset = len; break; }
                ch_ret_ts = data[offset + 1] | (data[offset + 2] << 8);
                ch_recv_ts = data[offset + 3] | (data[offset + 4] << 8);
                has_channel_ts = 1;
                pos += snprintf(buf + pos, sizeof(buf) - pos, " ch_ts=%u/%u", ch_ret_ts, ch_recv_ts);
                offset += 5;
                break;
            }
            case ETCP_SECTION_ACK_REQUEST:
                pos += snprintf(buf + pos, sizeof(buf) - pos, " till_query");
                offset++;
                break;
            case ETCP_SECTION_PAYLOAD: {
                if (offset + ETCP_PAYLOAD_HEADER_SIZE > len) { offset = len; break; }
                payload_seq = (uint32_t)data[offset + 1] | ((uint32_t)data[offset + 2] << 8) |
                              ((uint32_t)data[offset + 3] << 16) | ((uint32_t)data[offset + 4] << 24);
                payload_size = len - offset - ETCP_PAYLOAD_HEADER_SIZE;
                uint16_t attempt_no = data[offset + 5] | ((uint16_t)data[offset + 6] << 8);
                has_payload = 1;
                pos += snprintf(buf + pos, sizeof(buf) - pos, " payload=%u(seq=%u attempt=%u)",
                    payload_size, payload_seq, attempt_no);
                offset = len;
                break;
            }
            default:
                offset = len;
                break;
        }
    }

    if (offset < len || pos >= (int)sizeof(buf)) {
        const char marker[] = " [truncated]";
        memcpy(buf + sizeof(buf) - sizeof(marker), marker, sizeof(marker));
        DEBUG_WARN(DEBUG_CATEGORY_DUMP, "ETCP packet dump truncated: dir=%s data_len=%u offset=%u buffer=%zu",
                   dir, len, offset, sizeof(buf));
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_DUMP, "%s", buf);
}
