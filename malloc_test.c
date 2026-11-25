#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BLOCK_SIZE 10

static int counts[] = {0, 100, 1000, 10000, 100000};
static int n_counts = 5;

static long read_vm_field(const char *field) {
    char path[64], buf[256];
    snprintf(path, sizeof(path), "/proc/%d/status", getpid());
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long val = -1;
    while (fgets(buf, sizeof(buf), f)) {
        if (strncmp(buf, field, strlen(field)) == 0) {
            sscanf(buf + strlen(field) + 1, "%ld", &val);
            break;
        }
    }
    fclose(f);
    return val;
}

static void measure(const char *label) {
    long rss = read_vm_field("VmRSS");
    long data = read_vm_field("VmData");
    long size = read_vm_field("VmSize");
    printf("  %-8s  VmRSS=%6ld kB  VmData=%6ld kB  VmSize=%6ld kB\n", label, rss, data, size);
}

int main(void) {
    printf("Block size: %d bytes\n", BLOCK_SIZE);

    for (int c = 0; c < n_counts; c++) {
        int n = counts[c];
        printf("\n--- %d blocks (requested %d bytes) ---\n", n, n * BLOCK_SIZE);
        measure("before");

        void **ptrs = malloc(n * sizeof(void*));
        for (int i = 0; i < n; i++) {
            ptrs[i] = malloc(BLOCK_SIZE);
            memset(ptrs[i], 0, BLOCK_SIZE);
        }

        measure("after ");
        free(ptrs);
    }
    return 0;
}
