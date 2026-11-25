// test_etcp_link_id.c - Unit test for etcp_find_free_local_link_id function
// Tests: empty connection, full allocation, random deletion/addition cycles

#include "etcp_connections.h"
#include "etcp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include "../lib/mem.h"

// Mock ETCP_LINK structure for testing (minimal version)
struct TEST_LINK {
    struct TEST_LINK* next;
    uint8_t local_link_id;
};

// Helper function to create a mock ETCP_CONN with empty links list
static struct ETCP_CONN* create_test_conn(void) {
    struct ETCP_CONN* conn = u_calloc(1, sizeof(struct ETCP_CONN));
    if (!conn) {
        fprintf(stderr, "Failed to allocate test connection\n");
        exit(1);
    }
    conn->links = NULL;
    return conn;
}

// Helper function to add a link with specific id to connection
static void add_link_with_id(struct ETCP_CONN* conn, uint8_t id) {
    struct ETCP_LINK* link = u_calloc(1, sizeof(struct ETCP_LINK));
    if (!link) {
        fprintf(stderr, "Failed to allocate test link\n");
        exit(1);
    }
    link->local_link_id = id;
    link->next = conn->links;
    conn->links = link;
}

// Helper function to remove link with specific id
static int remove_link_with_id(struct ETCP_CONN* conn, uint8_t id) {
    struct ETCP_LINK** pp = &conn->links;
    while (*pp) {
        if ((*pp)->local_link_id == id) {
            struct ETCP_LINK* to_remove = *pp;
            *pp = (*pp)->next;
            u_free(to_remove);
            return 1; // Success
        }
        pp = &(*pp)->next;
    }
    return 0; // Not found
}

// Helper function to count links
static int count_links(struct ETCP_CONN* conn) {
    int count = 0;
    struct ETCP_LINK* link = conn->links;
    while (link) {
        count++;
        link = link->next;
    }
    return count;
}

// Helper function to free all links
static void free_all_links(struct ETCP_CONN* conn) {
    struct ETCP_LINK* link = conn->links;
    while (link) {
        struct ETCP_LINK* next = link->next;
        u_free(link);
        link = next;
    }
    conn->links = NULL;
}

// Test 1: NULL argument should return -1
static void test_null_argument(void) {
    printf("Test 1: NULL argument... ");
    int result = etcp_find_free_local_link_id(NULL);
    assert(result == -1);
    printf("PASSED\n");
}

// Test 2: Empty connection should return 0
static void test_empty_connection(void) {
    printf("Test 2: Empty connection... ");
    struct ETCP_CONN* conn = create_test_conn();
    int result = etcp_find_free_local_link_id(conn);
    assert(result == 0);
    u_free(conn);
    printf("PASSED\n");
}

// Test 3: Fill all 256, randomly delete and add back (30 cycles)
static void test_random_deletion_addition(void) {
    printf("Test 3: Random deletion/addition cycles (30 iterations)...\n");
    
    for (int cycle = 0; cycle < 30; cycle++) {
        struct ETCP_CONN* conn = create_test_conn();
        uint8_t used[256] = {0};
        
        // Fill all 256 ids
        for (int i = 0; i < 256; i++) {
            add_link_with_id(conn, i);
            used[i] = 1;
        }
        
        assert(count_links(conn) == 256);
        
        // Random number of links to delete (30-100)
        int num_to_delete = 30 + (rand() % 71);
        int deleted_ids[100];
        int deleted_count = 0;
        
        // Randomly delete links
        while (deleted_count < num_to_delete) {
            int id = rand() % 256;
            if (used[id] && remove_link_with_id(conn, id)) {
                used[id] = 0;
                deleted_ids[deleted_count++] = id;
            }
        }
        
        assert(count_links(conn) == 256 - num_to_delete);
        
        // Add back the same number of links
        int added_ids[100];
        int added_count = 0;
        int prev_id = -1;
        
        while (added_count < num_to_delete) {
            int new_id = etcp_find_free_local_link_id(conn);
            
            // Verify the id is actually free
            assert(new_id >= 0 && new_id < 256);
            assert(used[new_id] == 0);
            
            // Verify ids are returned in ascending order (filling gaps from smallest)
            assert(new_id > prev_id);
            prev_id = new_id;
            
            // Add the link
            add_link_with_id(conn, new_id);
            used[new_id] = 1;
            added_ids[added_count++] = new_id;
        }
        
        assert(count_links(conn) == 256);
        
        // Verify all ids are marked as used
        for (int i = 0; i < 256; i++) {
            assert(used[i] == 1);
        }
        
        // Verify no duplicates
        uint8_t check[256] = {0};
        struct ETCP_LINK* link = conn->links;
        while (link) {
            assert(check[link->local_link_id] == 0); // No duplicate
            check[link->local_link_id] = 1;
            link = link->next;
        }
        
        // Cleanup
        free_all_links(conn);
        u_free(conn);
        
        if ((cycle + 1) % 10 == 0) {
            printf("  Completed %d cycles...\n", cycle + 1);
        }
    }
    
    printf("Test 3: PASSED (all 30 cycles)\n");
}

// Test 4: All 256 occupied should return -1
static void test_all_occupied(void) {
    printf("Test 4: All 256 occupied... ");
    struct ETCP_CONN* conn = create_test_conn();
    
    // Fill all 256
    for (int i = 0; i < 256; i++) {
        add_link_with_id(conn, i);
    }
    
    int result = etcp_find_free_local_link_id(conn);
    assert(result == -1);
    
    free_all_links(conn);
    u_free(conn);
    printf("PASSED\n");
}

// Test 5: Delete specific ids (5, 10, 100), should return 5
static void test_specific_deletion(void) {
    printf("Test 5: Delete ids 5, 10, 100... ");
    struct ETCP_CONN* conn = create_test_conn();
    
    // Fill all 256
    for (int i = 0; i < 256; i++) {
        add_link_with_id(conn, i);
    }
    
    // Delete specific ids
    assert(remove_link_with_id(conn, 5));
    assert(remove_link_with_id(conn, 10));
    assert(remove_link_with_id(conn, 100));
    
    // Should return 5 (smallest free)
    int result = etcp_find_free_local_link_id(conn);
    assert(result == 5);
    
    free_all_links(conn);
    u_free(conn);
    printf("PASSED\n");
}

// Test 6: Delete id 0, should return 0
static void test_delete_zero(void) {
    printf("Test 6: Delete id 0... ");
    struct ETCP_CONN* conn = create_test_conn();
    
    // Fill all 256
    for (int i = 0; i < 256; i++) {
        add_link_with_id(conn, i);
    }
    
    // Delete id 0
    assert(remove_link_with_id(conn, 0));
    
    // Should return 0
    int result = etcp_find_free_local_link_id(conn);
    assert(result == 0);
    
    free_all_links(conn);
    u_free(conn);
    printf("PASSED\n");
}

// Test 7: Delete all, should return 0
static void test_delete_all(void) {
    printf("Test 7: Delete all... ");
    struct ETCP_CONN* conn = create_test_conn();
    
    // Fill all 256
    for (int i = 0; i < 256; i++) {
        add_link_with_id(conn, i);
    }
    
    // Delete all
    free_all_links(conn);
    
    // Should return 0
    int result = etcp_find_free_local_link_id(conn);
    assert(result == 0);
    
    u_free(conn);
    printf("PASSED\n");
}

int main(void) {
    printf("=== ETCP Link ID Unit Tests ===\n\n");
    
    // Initialize random seed
    srand(time(NULL));
    
    test_null_argument();
    test_empty_connection();
    test_random_deletion_addition();
    test_all_occupied();
    test_specific_deletion();
    test_delete_zero();
    test_delete_all();
    
    printf("\n=== All tests PASSED ===\n");
    return 0;
}
