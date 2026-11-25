/* sliding_window_min.c */
#include "swm_min.h"
#include <stdlib.h>
#include <limits.h>

struct SlidingWindowMin {
    int* data;          /* кольцевой буфер значений */
    int* deq;           /* deque индексов (тоже кольцевой) */
    int  dq_head;
    int  dq_tail;
    int  deq_count;
    int  cur_pos;
    int  window_size;
};

SlidingWindowMin* swm_create(int window_size)
{
    if (window_size <= 0) {
        return NULL;
    }

    SlidingWindowMin* swm = (SlidingWindowMin*)malloc(sizeof(SlidingWindowMin));
    if (!swm) {
        return NULL;
    }

    swm->data = (int*)malloc((size_t)window_size * sizeof(int));
    if (!swm->data) {
        free(swm);
        return NULL;
    }

    swm->deq = (int*)malloc((size_t)window_size * sizeof(int));
    if (!swm->deq) {
        free(swm->data);
        free(swm);
        return NULL;
    }

    swm->window_size = window_size;
    swm->dq_head     = 0;
    swm->dq_tail     = 0;
    swm->deq_count   = 0;
    swm->cur_pos     = 0;

    return swm;
}

void swm_destroy(SlidingWindowMin* swm)
{
    if (swm) {
        free(swm->data);
        free(swm->deq);
        free(swm);
    }
}

void swm_add(SlidingWindowMin* swm, int val)
{
    if (!swm) return;

    int idx = swm->cur_pos;
    int w   = swm->window_size;

    /* записываем в кольцевой буфер */
    swm->data[idx % w] = val;

    /* 1. удаляем из начала deque индексы, которые уже вышли за окно */
    while (swm->deq_count > 0 && swm->deq[swm->dq_head] <= idx - w) {
        swm->dq_head = (swm->dq_head + 1) % w;
        swm->deq_count--;
    }

    /* 2. удаляем из хвоста все элементы, которые хуже текущего */
    while (swm->deq_count > 0) {
        int last_idx = swm->deq[(swm->dq_tail - 1 + w) % w];
        if (swm->data[last_idx % w] < val) {
            break;
        }
        swm->dq_tail = (swm->dq_tail - 1 + w) % w;
        swm->deq_count--;
    }

    /* 3. добавляем текущий индекс */
    swm->deq[swm->dq_tail] = idx;
    swm->dq_tail = (swm->dq_tail + 1) % w;
    swm->deq_count++;

    swm->cur_pos++;
}

int swm_get_min(const SlidingWindowMin* swm)
{
    if (!swm || swm->deq_count == 0) {
        return INT_MAX;
    }
    int min_idx = swm->deq[swm->dq_head];
    return swm->data[min_idx % swm->window_size];
}
