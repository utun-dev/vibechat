// test_etcp_minimal.c - Minimal ETCP traffic debugging test
// This is the most basic test to verify ETCP packet processing works

#include "etcp.h"
#include "../lib/debug_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Test basic packet analysis
static void test_packet_analysis() {
    printf("=== Testing Basic Packet Analysis ===\n");
    
    // Simple test packet with one section
    uint8_t test_packet[] = {
        0x00,  // ETCP_SECTION_PAYLOAD
        0x00, 0x06,  // Length: 6 bytes
        0x00, 0x01,  // Packet ID: 1
        0x48, 0x65, 0x6C, 0x6C, 0x6F  // "Hello"
    };
    
    printf("Test packet: ");
    for (int i = 0; i < sizeof(test_packet); i++) {
        printf("%02X ", test_packet[i]);
    }
    printf("\n");
    
    // Manual section parsing
    if (sizeof(test_packet) >= 3) {
        uint8_t section_type = test_packet[0];
        uint16_t section_len = (test_packet[1] << 8) | test_packet[2];
        
        printf("Section type: 0x%02X\n", section_type);
        printf("Section length: %u\n", section_len);
        printf("Total packet size: %zu\n", sizeof(test_packet));
        
        if (section_type == 0x00) { // PAYLOAD
            printf("This is a PAYLOAD section\n");
            if (section_len >= 2 && section_len + 3 <= sizeof(test_packet)) {
                uint16_t packet_id = (test_packet[3] << 8) | test_packet[4];
                printf("Packet ID: %u\n", packet_id);
                printf("Payload size: %u bytes\n", section_len - 2);
                
                printf("Payload data: ");
                for (int i = 0; i < section_len - 2; i++) {
                    printf("%02X ", test_packet[5 + i]);
                }
                printf("\n");
                
                printf("Payload as text: ");
                for (int i = 0; i < section_len - 2; i++) {
                    printf("%c", test_packet[5 + i]);
                }
                printf("\n");
            }
        }
    }
}

// Test multiple sections
static void test_multi_section_packet() {
    printf("\n=== Testing Multi-Section Packet ===\n");
    
    // Packet with TIMESTAMP + ACK + PAYLOAD sections
    uint8_t multi_packet[] = {
        // TIMESTAMP section
        0x06, 0x00, 0x02, 0x12, 0x34,
        // ACK section  
        0x01, 0x00, 0x09, 0x02, 0x00, 0x01, 0x56, 0x78, 0x00, 0x02, 0x56, 0x79,
        // PAYLOAD section
        0x00, 0x00, 0x08, 0x00, 0x03, 0x48, 0x65, 0x6C, 0x6C, 0x6F, 0x21  // "Hello!"
    };
    
    printf("Multi-section packet: ");
    for (int i = 0; i < sizeof(multi_packet); i++) {
        printf("%02X ", multi_packet[i]);
    }
    printf("\n");
    
    // Parse each section
    int pos = 0;
    int section_count = 0;
    
    while (pos < sizeof(multi_packet) - 2) {
        if (pos + 3 > sizeof(multi_packet)) break;
        
        uint8_t section_type = multi_packet[pos];
        uint16_t section_len = (multi_packet[pos+1] << 8) | multi_packet[pos+2];
        
        printf("\nSection %d at position %d:\n", section_count, pos);
        printf("  Type: 0x%02X ", section_type);
        
        switch (section_type) {
            case 0x00: printf("(PAYLOAD)"); break;
            case 0x01: printf("(ACK)"); break;
            case 0x06: printf("(TIMESTAMP)"); break;
            default: printf("(UNKNOWN)"); break;
        }
        printf("\n");
        printf("  Length: %u bytes\n", section_len);
        
        if (pos + 3 + section_len > sizeof(multi_packet)) {
            printf("  ERROR: Section extends beyond packet boundary\n");
            break;
        }
        
        // Section-specific analysis
        uint8_t* section_data = multi_packet + pos + 3;
        
        switch (section_type) {
            case 0x00: { // PAYLOAD
                if (section_len >= 2) {
                    uint16_t packet_id = (section_data[0] << 8) | section_data[1];
                    printf("  Packet ID: %u\n", packet_id);
                    printf("  Payload: ");
                    for (int i = 2; i < section_len; i++) {
                        printf("%c", section_data[i]);
                    }
                    printf("\n");
                }
                break;
            }
            case 0x01: { // ACK
                if (section_len >= 1) {
                    uint8_t ack_count = section_data[0];
                    printf("  ACK count: %u\n", ack_count);
                    for (int i = 0; i < ack_count && i*4+5 <= section_len; i++) {
                        uint16_t ack_id = (section_data[1+i*4] << 8) | section_data[2+i*4];
                        uint16_t ack_ts = (section_data[3+i*4] << 8) | section_data[4+i*4];
                        printf("    ACK[%d]: id=%u, ts=%u\n", i, ack_id, ack_ts);
                    }
                }
                break;
            }
            case 0x06: { // TIMESTAMP
                if (section_len == 2) {
                    uint16_t ts = (section_data[0] << 8) | section_data[1];
                    printf("  Timestamp: %u\n", ts);
                }
                break;
            }
        }
        
        pos += 3 + section_len;
        section_count++;
    }
    
    printf("\nTotal sections parsed: %d\n", section_count);
}

// Test connection establishment packets
static void test_connection_packets() {
    printf("\n=== Testing Connection Establishment Packets ===\n");
    
    struct ETCP_INIT_REQUEST_PKT request = {.code=ETCP_INIT_REQUEST, .mtu={0x05, 0xdc}, .keepalive={0, 100}};
    struct ETCP_INIT_RESPONSE_PKT response = {.code=ETCP_INIT_RESPONSE, .mtu={0x05, 0xdc}};
    memset(request.ed25519_pubkey, 0x22, sizeof(request.ed25519_pubkey));
    memset(response.ed25519_pubkey, 0x11, sizeof(response.ed25519_pubkey));
    printf("INIT_REQUEST peer=%s mtu=%u keepalive=%u body=%zu\n", sc_public_key_hex(request.ed25519_pubkey, (char[65]){0}),
        (request.mtu[0] << 8) | request.mtu[1], (request.keepalive[0] << 8) | request.keepalive[1], sizeof(request));
    printf("INIT_RESPONSE peer=%s mtu=%u body=%zu\n", sc_public_key_hex(response.ed25519_pubkey, (char[65]){0}),
        (response.mtu[0] << 8) | response.mtu[1], sizeof(response));
}

int main() {
    printf("=== Minimal ETCP Traffic Flow Debugging ===\n");
    
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_DEBUG);
    debug_enable_category(DEBUG_CATEGORY_ETCP);
    debug_enable_function_name(1);
    
    test_packet_analysis();
    test_multi_section_packet();
    test_connection_packets();
    
    printf("\n=== Test completed ===\n");
    return 0;
}