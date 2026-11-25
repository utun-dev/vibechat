#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include <stdio.h>
#include <stdlib.h>
#include "../lib/platform_compat.h"



static void test_callback(int fd, void* arg) {
    (void)fd; (void)arg;
}

int main() {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_TRACE);
    debug_set_categories(DEBUG_CATEGORY_ALL);
    
    uasync_t* ua = uasync_create();
    if (!ua) return 1;
    
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "=== Simple Corruption Test ===");
    
    // Create just one socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Created socket with fd=%d", sock);
    
    void* id = uasync_add_socket(ua, sock, test_callback, NULL, NULL, "test", NULL);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Added socket: fd=%d, id=%p", sock, id);
    
    // Check if fd is still correct (internal struct socket_node not exposed)
    // struct socket_node* node = (struct socket_node*)id;
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Node fd after add: %d", sock);
    
    // Try to remove immediately (no polling)
    int result = uasync_remove_socket(ua, id);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Remove result: %d", result);
    
    close(sock);
    uasync_destroy(ua, 0);
    
    return 0;
}
