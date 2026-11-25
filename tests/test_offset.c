#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "../lib/mem.h"

typedef struct {
    int a;
    int b;
    int ref_count;
    uint8_t data[0];
} test_struct;

int main() {
    test_struct* entry = u_malloc(sizeof(test_struct) + 100);
    entry->a = 1;
    entry->b = 2;
    entry->ref_count = 3;
    
    void* data_ptr = entry->data;  // or (void*)(entry + 1)
    
    printf("entry pointer: %p\n", entry);
    printf("data pointer: %p\n", data_ptr);
    printf("sizeof(test_struct): %zu\n", sizeof(test_struct));
    printf("difference in bytes: %ld\n", (char*)data_ptr - (char*)entry);
    
    // Test different ways to convert back
    test_struct* back1 = (test_struct*)((char*)data_ptr - sizeof(test_struct));
    test_struct* back2 = (test_struct*)data_ptr - 1;
    test_struct* back3 = (test_struct*)data_ptr - 0;
    
    printf("back1 (char* subtraction): %p\n", back1);
    printf("back2 (pointer - 1): %p\n", back2);
    printf("back3 (pointer - 0): %p\n", back3);
    
    printf("back1 ref_count: %d\n", back1->ref_count);
    printf("back2 ref_count: %d\n", back2->ref_count);
    printf("back3 ref_count: %d\n", back3->ref_count);
    
    u_free(entry);
    return 0;
}
