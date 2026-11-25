// net_emulator.c - Network emulator for testing utun with delay, loss, reordering
#define _POSIX_C_SOURCE 200809L
#include "net_emulator.h"
#include "u_async.h"
#include "mem.h"
#include "debug_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/time.h>

#define MAX_LINE_LEN 1024
#define TIME_BASE_MS 0.1  // u_async timebase is 0.1ms

// Packet buffer with timestamp for delayed sending
typedef struct {
    uint8_t data[MAX_PACKET_SIZE];
    size_t len;
    struct sockaddr_in from_addr;
    struct sockaddr_in to_addr;
    endpoint_t *endpoint;
    struct timeval send_time;  // When to send this packet
} delayed_packet_t;

// Global emulator state
static net_emulator_t g_emulator = {0};
static int g_packet_counter = 0;

// Helper function to trim whitespace
static char* trim(char *str) {
    if (!str) return NULL;
    
    // Trim leading spaces
    while (isspace((unsigned char)*str)) str++;
    
    // Trim trailing spaces
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';
    
    return str;
}

// Parse IP:port from string like "127.0.0.1:20000"
static int parse_ip_port(const char *str, struct sockaddr_in *addr) {
    if (!str || !addr) return -1;
    
    char ip[64];
    int port;
    
    // Find colon
    const char *colon = strchr(str, ':');
    if (!colon) return -1;
    
    // Extract IP
    size_t ip_len = colon - str;
    if (ip_len >= sizeof(ip)) return -1;
    strncpy(ip, str, ip_len);
    ip[ip_len] = '\0';
    
    // Parse port
    port = atoi(colon + 1);
    if (port <= 0 || port > 65535) return -1;
    
    // Convert IP
    if (inet_pton(AF_INET, ip, &addr->sin_addr) != 1) return -1;
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    
    return 0;
}

// Parse delay range like "100-130"
static int parse_delay_range(const char *str, delay_range_t *range) {
    if (!str || !range) return -1;
    
    const char *dash = strchr(str, '-');
    if (!dash) {
        // Single value
        range->min_ms = atoi(str);
        range->max_ms = range->min_ms;
    } else {
        char min_str[32], max_str[32];
        size_t min_len = dash - str;
        strncpy(min_str, str, min_len);
        min_str[min_len] = '\0';
        strncpy(max_str, dash + 1, sizeof(max_str) - 1); max_str[sizeof(max_str) - 1] = '\0';
        
        range->min_ms = atoi(min_str);
        range->max_ms = atoi(max_str);
    }
    
    if (range->min_ms < 0 || range->max_ms < range->min_ms) return -1;
    return 0;
}

// Parse probability and delay from "prob=40,delay=100-130"
static int parse_rule(const char *line, rule_t *rule) {
    if (!line || !rule) return -1;
    
    const char *prob_start = strstr(line, "prob=");
    if (!prob_start) return -1;
    
    const char *comma = strchr(prob_start, ',');
    if (!comma) return -1;
    
    // Parse probability
    char prob_str[32];
    strncpy(prob_str, prob_start + 5, comma - (prob_start + 5));
    prob_str[comma - (prob_start + 5)] = '\0';
    rule->probability_percent = atoi(prob_str);
    if (rule->probability_percent < 0 || rule->probability_percent > 100) return -1;
    
    // Parse delay
    const char *delay_start = strstr(comma, "delay=");
    if (!delay_start) return -1;
    
    char delay_str[64];
    strncpy(delay_str, delay_start + 6, sizeof(delay_str) - 1); delay_str[sizeof(delay_str) - 1] = '\0';
    trim(delay_str);
    
    // Remove trailing ) if present (from comment)
    char *paren = strchr(delay_str, ')');
    if (paren) *paren = '\0';
    
    if (parse_delay_range(delay_str, &rule->delay) < 0) return -1;
    
    return 0;
}

// Parse configuration file
int parse_emulator_config(const char *filename, net_emulator_t *emulator) {
    if (!filename || !emulator) return -1;
    
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "Failed to open config file: %s\n", filename);
        return -1;
    }
    
    char line[MAX_LINE_LEN];
    endpoint_t *current_endpoint = NULL;
    
    while (fgets(line, sizeof(line), fp)) {
        // Remove newline
        line[strcspn(line, "\n")] = '\0';
        char *trimmed = trim(line);
        
        // Skip empty lines and comments starting with ;
        if (strlen(trimmed) == 0 || trimmed[0] == ';') {
            continue;
        }
        
        // Check for endpoint section [ip:port]
        if (trimmed[0] == '[' && trimmed[strlen(trimmed) - 1] == ']') {
            // Extract ip:port inside brackets
            char endpoint_str[128];
            strncpy(endpoint_str, trimmed + 1, sizeof(endpoint_str) - 1);
            endpoint_str[sizeof(endpoint_str) - 1] = '\0';
            endpoint_str[strcspn(endpoint_str, "]")] = '\0';
            trim(endpoint_str);
            
            // Remove any comments in parentheses
            char *paren = strchr(endpoint_str, '(');
            if (paren) *paren = '\0';
            trim(endpoint_str);
            
            // Check if we have space for more endpoints
            if (emulator->endpoint_count >= MAX_ENDPOINTS) {
                fprintf(stderr, "Too many endpoints, maximum is %d\n", MAX_ENDPOINTS);
                fclose(fp);
                return -1;
            }
            
            current_endpoint = &emulator->endpoints[emulator->endpoint_count];
            memset(current_endpoint, 0, sizeof(endpoint_t));
            current_endpoint->sockfd = -1;
            
            // Parse listen address
            if (parse_ip_port(endpoint_str, &current_endpoint->listen_addr) < 0) {
                fprintf(stderr, "Invalid endpoint format: %s\n", endpoint_str);
                fclose(fp);
                return -1;
            }
            
            // Calculate target address (port - 10000 for net_debug mode)
            uint16_t listen_port = ntohs(current_endpoint->listen_addr.sin_port);
            uint16_t target_port = listen_port;
            if (listen_port >= 10000) {
                target_port = listen_port - 10000;
                printf("[CONFIG] Forwarding %s:%u -> %s:%u (net_debug mode)\n",
                       inet_ntoa(current_endpoint->listen_addr.sin_addr), listen_port,
                       inet_ntoa(current_endpoint->listen_addr.sin_addr), target_port);
            } else {
                printf("[CONFIG] Forwarding %s:%u -> %s:%u (direct)\n",
                       inet_ntoa(current_endpoint->listen_addr.sin_addr), listen_port,
                       inet_ntoa(current_endpoint->listen_addr.sin_addr), target_port);
            }
            
            current_endpoint->target_addr = current_endpoint->listen_addr;
            current_endpoint->target_addr.sin_port = htons(target_port);
            
            emulator->endpoint_count++;
            continue;
        }
        
        // Parse rule if we're in an endpoint section
        if (current_endpoint) {
            // Check if line contains prob= (it's a rule)
            if (strstr(trimmed, "prob=")) {
                if (current_endpoint->rule_count >= MAX_RULES_PER_ENDPOINT) {
                    fprintf(stderr, "Too many rules for endpoint, maximum is %d\n", 
                           MAX_RULES_PER_ENDPOINT);
                    fclose(fp);
                    return -1;
                }
                
                rule_t *rule = &current_endpoint->rules[current_endpoint->rule_count];
                if (parse_rule(trimmed, rule) == 0) {
                    current_endpoint->total_probability += rule->probability_percent;
                    current_endpoint->rule_count++;
                    
                    if (current_endpoint->total_probability > 100) {
                        fprintf(stderr, "Total probability exceeds 100%% for endpoint %s:%u\n",
                               inet_ntoa(current_endpoint->listen_addr.sin_addr),
                               ntohs(current_endpoint->listen_addr.sin_port));
                        fclose(fp);
                        return -1;
                    }
                } else {
                    fprintf(stderr, "Invalid rule format: %s\n", trimmed);
                }
            }
            // Ignore lines in parentheses (comments)
        }
    }
    
    fclose(fp);
    
    // Validate configuration
    for (int i = 0; i < emulator->endpoint_count; i++) {
        endpoint_t *ep = &emulator->endpoints[i];
        printf("[CONFIG] Endpoint %s:%u has %d rules (total prob=%d%%)\n",
               inet_ntoa(ep->listen_addr.sin_addr), ntohs(ep->listen_addr.sin_port),
               ep->rule_count, ep->total_probability);
    }
    
    return 0;
}

// Create UDP socket for endpoint
static int create_endpoint_socket(endpoint_t *ep) {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }
    
    // Enable address reuse
    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(sockfd);
        return -1;
    }
    
    // Bind to listen address
    if (bind(sockfd, (struct sockaddr*)&ep->listen_addr, sizeof(ep->listen_addr)) < 0) {
        perror("bind");
        close(sockfd);
        return -1;
    }
    
    // Set non-blocking
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags < 0) {
        perror("fcntl GETFL");
        close(sockfd);
        return -1;
    }
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl SETFL");
        close(sockfd);
        return -1;
    }
    
    ep->sockfd = sockfd;
    return 0;
}

// Forward declaration
static void send_delayed_packet(void *user_arg);

// Socket callback for incoming packets
static void socket_callback(int fd, void *user_arg) {
    endpoint_t *ep = (endpoint_t*)user_arg;
    if (!ep) return;
    
    uint8_t buffer[MAX_PACKET_SIZE];
    struct sockaddr_in from_addr;
    socklen_t addr_len = sizeof(from_addr);
    
    ssize_t nread = recvfrom(fd, buffer, sizeof(buffer), 0,
                            (struct sockaddr*)&from_addr, &addr_len);
    if (nread <= 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("recvfrom");
        }
        return;
    }
    
    g_packet_counter++;
    printf("[PACKET #%d] Received %zd bytes from %s:%u -> %s:%u\n",
           g_packet_counter, nread,
           inet_ntoa(from_addr.sin_addr), ntohs(from_addr.sin_port),
           inet_ntoa(ep->target_addr.sin_addr), ntohs(ep->target_addr.sin_port));
    
    // Apply rules to decide fate of packet
    int random_val = rand() % 100;
    int cumulative_prob = 0;
    rule_t *selected_rule = NULL;
    
    for (int i = 0; i < ep->rule_count; i++) {
        cumulative_prob += ep->rules[i].probability_percent;
        if (random_val < cumulative_prob) {
            selected_rule = &ep->rules[i];
            break;
        }
    }
    
    // If no rule selected (random_val >= total_probability), packet is dropped
    if (!selected_rule) {
        printf("[PACKET #%d] DROPPED (no matching rule)\n", g_packet_counter);
        return;
    }
    
    // Calculate delay
    int delay_ms = selected_rule->delay.min_ms;
    if (selected_rule->delay.max_ms > selected_rule->delay.min_ms) {
        delay_ms += rand() % (selected_rule->delay.max_ms - selected_rule->delay.min_ms + 1);
    }
    
    printf("[PACKET #%d] Applying delay: %d ms (rule prob=%d%%)\n",
           g_packet_counter, delay_ms, selected_rule->probability_percent);
    
    // Allocate packet buffer for delayed sending
    delayed_packet_t *packet = u_malloc(sizeof(delayed_packet_t));
    if (!packet) {
        fprintf(stderr, "Failed to allocate packet buffer\n");
        return;
    }
    
    memcpy(packet->data, buffer, nread);
    packet->len = nread;
    packet->from_addr = from_addr;
    packet->to_addr = ep->target_addr;
    packet->endpoint = ep;
    
    // Calculate send time
    gettimeofday(&packet->send_time, NULL);
    packet->send_time.tv_sec += delay_ms / 1000;
    packet->send_time.tv_usec += (delay_ms % 1000) * 1000;
    if (packet->send_time.tv_usec >= 1000000) {
        packet->send_time.tv_sec++;
        packet->send_time.tv_usec -= 1000000;
    }
    
    // Create timeout for delayed sending
    // Convert delay to u_async timebase (0.1ms)
    int delay_tb = delay_ms * 10;
    if (!ep->emulator || !ep->emulator->ua) {
        u_free(packet);
        return;
    }
    uasync_set_timeout(ep->emulator->ua, delay_tb, packet, (timeout_callback_t)send_delayed_packet, "netem_delay");
}

// Callback for sending delayed packet
static void send_delayed_packet(void *user_arg) {
    delayed_packet_t *packet = (delayed_packet_t*)user_arg;
    if (!packet) return;
    
    ssize_t nsent = sendto(packet->endpoint->sockfd, packet->data, packet->len, 0,
                          (struct sockaddr*)&packet->to_addr, sizeof(packet->to_addr));
    if (nsent != (ssize_t)packet->len) {
        perror("sendto");
        printf("[PACKET] Failed to forward packet\n");
    } else {
        printf("[PACKET] Forwarded %zd bytes to %s:%u\n",
               nsent, inet_ntoa(packet->to_addr.sin_addr), ntohs(packet->to_addr.sin_port));
    }
    
    u_free(packet);
}

// Initialize network emulator
int net_emulator_init(net_emulator_t *emulator) {
    if (!emulator) return -1;
    
    // Initialize random seed
    srand(time(NULL));
    
    // Initialize u_async instance
    emulator->ua = uasync_create();
    if (!emulator->ua) {
        fprintf(stderr, "Failed to create uasync instance\n");
        return -1;
    }
    
    // Create sockets for all endpoints
    for (int i = 0; i < emulator->endpoint_count; i++) {
        endpoint_t *ep = &emulator->endpoints[i];
        if (create_endpoint_socket(ep) < 0) {
            fprintf(stderr, "Failed to create socket for endpoint %s:%u\n",
                   inet_ntoa(ep->listen_addr.sin_addr), ntohs(ep->listen_addr.sin_port));
            uasync_destroy(emulator->ua, 0);
            emulator->ua = NULL;
            return -1;
        }
        
        ep->emulator = emulator;
        
        // Add socket to u_async
        if (!uasync_add_socket(emulator->ua, ep->sockfd, socket_callback, NULL, NULL, "emulator", ep)) {
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "net_emulator_init: socket registration failed fd=%d endpoint=%d", ep->sockfd, i);
            net_emulator_cleanup(emulator);
            return -1;
        }
        printf("[INIT] Listening on %s:%u\n",
               inet_ntoa(ep->listen_addr.sin_addr), ntohs(ep->listen_addr.sin_port));
    }
    
    emulator->running = 1;
    return 0;
}

// Run network emulator main loop
void net_emulator_run(net_emulator_t *emulator) {
    if (!emulator || !emulator->running || !emulator->ua) return;
    
    printf("[EMULATOR] Starting main loop. Press Ctrl+C to stop.\n");
    uasync_mainloop(emulator->ua);  // Возвращается после uasync_stop.
}

// Clean up emulator resources
void net_emulator_cleanup(net_emulator_t *emulator) {
    if (!emulator) return;
    
    for (int i = 0; i < emulator->endpoint_count; i++) {
        endpoint_t *ep = &emulator->endpoints[i];
        if (ep->sockfd >= 0) {
            close(ep->sockfd);
            ep->sockfd = -1;
        }
    }
    
    if (emulator->ua) {
        uasync_destroy(emulator->ua, 0);
        emulator->ua = NULL;
    }
    
    emulator->running = 0;
}

// Main function
int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    debug_config_init();
    
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <config_file>\n", argv[0]);
        fprintf(stderr, "Example: %s net_emulator.conf\n", argv[0]);
        return 1;
    }
    
    printf("Network Emulator for utun net_debug mode\n");
    printf("=========================================\n");
    
    // Parse configuration
    if (parse_emulator_config(argv[1], &g_emulator) < 0) {
        fprintf(stderr, "Failed to parse configuration\n");
        return 1;
    }
    
    if (g_emulator.endpoint_count == 0) {
        fprintf(stderr, "No endpoints configured\n");
        return 1;
    }
    
    // Initialize emulator
    if (net_emulator_init(&g_emulator) < 0) {
        fprintf(stderr, "Failed to initialize emulator\n");
        net_emulator_cleanup(&g_emulator);
        return 1;
    }
    
    // Run main loop
    net_emulator_run(&g_emulator);
    
    // Cleanup (never reached)
    net_emulator_cleanup(&g_emulator);
    return 0;
}
