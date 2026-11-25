// pkt_normalizer.c - Implementation of packet normalizer for ETCP
#include "pkt_normalizer.h"
#include "etcp.h"  // For ETCP_CONN and related structures
#include "etcp_api.h"  // For etcp_recv callback
#include "etcp_profile.h"
#include "routing.h"  // For routing_add_conn/routing_del_conn
#include "utun_instance.h"  // For UTUN_INSTANCE
#include "ll_queue.h"  // For queue operations
#include "u_async.h"   // For UASYNC
#include <stdlib.h>
#include <string.h>
#include <stdio.h>     // For debugging (can be removed if not needed)
#include "../lib/debug_config.h"  // For DEBUG macros
#include "../lib/mem.h"

// Forward declarations
static void packer_cb(struct ll_queue* q, void* arg);
static void pn_flush_cb(void* arg);
static void etcp_input_ready_cb(struct ll_queue* q, void* arg);
static void pn_unpacker_cb(struct ll_queue* q, void* arg);
static int pn_send_to_etcp(struct PKTNORM* pn);

// Initialization
struct PKTNORM* pn_init(struct ETCP_CONN* etcp) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!etcp) return NULL;

    if (etcp->mtu<200) {
      DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "ETCP MTU error=%d", etcp->mtu);
      }

    struct PKTNORM* pn = u_calloc(1, sizeof(struct PKTNORM));
    if (!pn) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "calloc failed");
        return NULL;
    }

    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "pn_init:[%s] init %p, mtu=%d", etcp->log_name, etcp, etcp->mtu);


    pn->etcp = etcp;
    pn->ua = etcp->instance->ua;
    int max_udp = IP_UDP_OVERHEAD_V4;
    for (struct ETCP_LINK* l = etcp->links; l; l = l->next)
        if (l->remote_addr.ss_family == AF_INET6) max_udp = IP_UDP_OVERHEAD_V6;
    pn->frag_size = etcp->mtu - max_udp - SC_ENCRYPT_OVERHEAD - ETCP_PAYLOAD_HEADER_SIZE - ACK_REZERV;
    int min_frag = max_udp + SC_ENCRYPT_OVERHEAD + ETCP_PAYLOAD_HEADER_SIZE + ACK_REZERV;
    if (etcp->mtu < min_frag || pn->frag_size > etcp->mtu) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "MTU %d too small (min %d)", etcp->mtu, min_frag);
        pn->frag_size = etcp->mtu;
    }
    pn->tx_wait_time = 10;

    pn->input = queue_new(pn->ua, 0, 0, 0, "pn_input");  // No hash needed
    if (!pn->input) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "queue_new(input) failed");
        pn_deinit(pn);
        return NULL;
    }
    queue_set_threshold(pn->input, 0, 0);    // транзитные очереди: ждать полного освобождения
    queue_set_waiter_defer(pn->input, 1);     // defer чтобы избежать рекурсии drain_cb

    pn->output = queue_new(pn->ua, 0, 0, 0, "pn_output"); // No hash needed
    if (!pn->output) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "queue_new(output) failed");
        pn_deinit(pn);
        return NULL;
    }

    queue_set_callback(pn->input, packer_cb, pn);
    queue_set_callback(etcp->output_queue, pn_unpacker_cb, pn);
    queue_set_callback(pn->output, etcp_int_recv, etcp);

    pn->data = NULL;
    pn->recvpart = NULL;
    pn->flush_timer = NULL;

    pn->input_waiter_handle.internal = NULL;

    return pn;
}

// Deinitialization
void pn_deinit(struct PKTNORM* pn) {
    if (!pn) return;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] normalizer deinit", pn->etcp->log_name);
    routing_del_conn(pn->etcp);
    queue_set_callback(pn->etcp->output_queue, NULL, NULL);
    pn_reset(pn);
    if (pn->input) queue_free(pn->input);
    if (pn->output) queue_free(pn->output);
    u_free(pn);
}

// Reset unpacker state
void pn_unpacker_reset_state(struct PKTNORM* pn) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!pn) return;
    
    // Reset recvpart
    if (pn->recvpart) {
        queue_dgram_free(pn->recvpart);
        queue_entry_free(pn->recvpart);
        pn->recvpart = NULL;
    }
}

// Reset packer and unpacker state (for reconnection)
void pn_reset(struct PKTNORM* pn) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!pn) return;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] normalizer discard: input=%d output=%d packed=%u partial_rx=%u",
        pn->etcp->log_name, queue_entry_count(pn->input), queue_entry_count(pn->output), pn->data_ptr,
        pn->recvpart ? pn->recvpart->len : 0);
    queue_waiter_cancel(pn->etcp->input_queue, &pn->input_waiter_handle);
    queue_clear(pn->input); queue_clear(pn->output);

    // Cancel flush timer
    if (pn->flush_timer) {
        uasync_call_soon_cancel(pn->ua, pn->flush_timer);
        pn->flush_timer = NULL;
    }

    // Reset packer state
    if (pn->data) {
        memory_pool_free(pn->etcp->instance->data_pool, pn->data);
        pn->data = NULL;
    }
    pn->data_ptr = 0;
    pn->data_size = 0;

    // Free pending if any
    if (pn->pending) {
        queue_dgram_free(pn->pending);
        queue_entry_free(pn->pending);
        pn->pending = NULL;
    }
    pn->pending_in_ptr = 0;

    // Reset unpacker state
    pn_unpacker_reset_state(pn);

    queue_resume_callback(pn->input);
    queue_resume_callback(pn->output);
}

// Send data to packer (copies and adds to input queue or pending, triggering callback) используется только в юниттесте
void pn_packer_send(struct PKTNORM* pn, uint8_t* data, uint16_t len) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!pn || !data || len == 0) return;

    struct ll_entry* entry = ll_alloc_lldgram(len);
    if (!entry) {
        pn->alloc_errors++;
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "ll_alloc_lldgram failed");
        return;
    }
    memcpy(entry->dgram, data, len);
    entry->len = len;
    entry->dgram_pool = NULL;

    // Cancel flush timer if active
    if (pn->flush_timer) {
        uasync_call_soon_cancel(pn->ua, pn->flush_timer);
        pn->flush_timer = NULL;
    }

    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "PUT to input");
    int ret = queue_data_put(pn->input, entry);
    pn->in_total_pkts++;
    pn->in_total_bytes += len;
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "PUT to input end");
    // DEBUG_INFO(DEBUG_CATEGORY_ETCP, "queue_data_put returned %d, input count=%d", ret, queue_entry_count(pn->input));
}


// Internal: Packer callback
static void packer_cb(struct ll_queue* q, void* arg) {
    ETCP_PROFILE_SCOPE(ETCP_PROF_TX_STACK);
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    struct PKTNORM* pn = (struct PKTNORM*)arg;
    if (!pn || pn->etcp->close_requested || pn->etcp->reinit_pending || pn->etcp->session_required) return;
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "input_q->pn: waiting etcp input threshold");
    pn->etcp->callbacks_running++;
    if (queue_waiter_wait(pn->etcp->input_queue, &pn->input_waiter_handle, etcp_input_ready_cb, pn) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] normalizer waiter OOM; restarting stream", pn->etcp->log_name);
        etcp_conn_fatal_reinit(pn->etcp, "normalizer waiter OOM");
    }
    pn->etcp->callbacks_running--;
}

// Helper to send block to ETCP as ETCP_FRAGMENT
static int pn_send_to_etcp(struct PKTNORM* pn) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!pn || !pn->data || pn->data_ptr == 0) return 0;
    ETCP_PROFILE_MARK(ETCP_PROF_NORMALIZED, pn->etcp);
    uint32_t reset = pn->etcp->reset_count;

    // DEBUG_INFO(DEBUG_CATEGORY_ETCP, "pn_send_to_etcp");
    // Allocate ETCP_FRAGMENT from io_pool
    struct ETCP_FRAGMENT* frag = (struct ETCP_FRAGMENT*)queue_entry_new_from_pool(pn->etcp->io_pool);
    if (!frag) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] normalizer fragment OOM bytes=%u; restarting stream",
            pn->etcp->log_name, pn->data_ptr);
        pn->alloc_errors++;
        etcp_conn_fatal_reinit(pn->etcp, "normalizer TX fragment OOM");
        return -1;
    }
    
    frag->seq = 0;
    frag->timestamp = 0;
    frag->ll.dgram      = pn->data;
    frag->ll.len        = pn->data_ptr;
    frag->ll.dgram_pool = pn->etcp->instance->data_pool;
    frag->ll.memlen     = pn->etcp->instance->data_pool->object_size;

    // Зануляем pn->data ДО queue_data_put: последняя синхронно гонит input_queue_cb → send_q → wait_ack,
    // в которой может ре-энтрантно сработать close → pn_deinit → free(pn->data).
    // Иначе pn_deinit освободит буфер, уже переданный во INFLIGHT-запись (double free).
    pn->data = NULL;
    pn->data_ptr = 0;

    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] normalizer TX fragment bytes=%u capacity=%u frag_size=%u input=%d",
                pn->etcp->log_name, frag->ll.len, frag->ll.memlen, pn->frag_size, queue_entry_count(pn->input));
    if (debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_DUMP)) log_dump(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_DUMP, "NORM->ETCP", frag->ll.dgram, frag->ll.len);

    if (queue_data_put(pn->etcp->input_queue, (struct ll_entry*)frag) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] normalizer TX queue rejected fragment; restarting stream", pn->etcp->log_name);
        etcp_conn_fatal_reinit(pn->etcp, "normalizer TX queue failure");
        return -1;
    }
    return pn->etcp->close_requested || pn->etcp->reinit_pending || pn->etcp->reset_count != reset ? -1 : 0;
}

// Internal: Renew sndpart buffer
static int pn_buf_renew(struct PKTNORM* pn) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (pn->data) {
        int remain = pn->data_size - pn->data_ptr;
        if (remain < 3 && pn_send_to_etcp(pn) < 0) return -1;
    }
    if (!pn->data) {
        pn->data = memory_pool_alloc(pn->etcp->instance->data_pool);
        if (!pn->data) {
            pn->alloc_errors++;
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] normalizer buffer OOM; restarting stream", pn->etcp->log_name);
            etcp_conn_fatal_reinit(pn->etcp, "normalizer TX buffer OOM");
            return -1;
        }
        int size=pn->etcp->instance->data_pool->object_size;
        if (size>pn->frag_size) size=pn->frag_size;
        pn->data_size = size;
        pn->data_ptr=0;
        // DEBUG_INFO(DEBUG_CATEGORY_ETCP, "new bufer size=%d bytes",size);
    }
    return 0;
}

// Internal: Process input when etcp->input_queue is ready (empty)
static void etcp_input_ready_cb(struct ll_queue* q, void* arg) {
    ETCP_PROFILE_SCOPE(ETCP_PROF_TX_STACK);
    struct PKTNORM* pn = (struct PKTNORM*)arg;
    if (!pn || !pn->etcp || pn->etcp->close_requested || pn->etcp->reinit_pending || pn->etcp->session_required) return;
    struct ETCP_CONN* conn = pn->etcp;
    uint32_t reset = conn->reset_count;
    conn->callbacks_running++;
//    if (pn->etcp->initialized==0) return;// начинаем обработку очередей только когда готов etcp

    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "before get");
    struct ll_entry* in_dgram = queue_data_get(pn->input);
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "after get");
    if (!in_dgram) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Q EMPTY");
        queue_resume_callback(pn->input);
        conn->callbacks_running--;
        return;
        }
    pn->in_total_pkts++;
    pn->in_total_bytes += in_dgram->len;

    if (debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_DUMP)) log_dump(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_DUMP, "->NORM", in_dgram->dgram, in_dgram->len);

    if (pn_buf_renew(pn) < 0) goto exit;
    // DEBUG_INFO(DEBUG_CATEGORY_ETCP, "new pkt hdrpos=%d",pn->data_ptr);
    if (!pn->data) goto exit;  // Allocation failed
    pn->data[pn->data_ptr++] = in_dgram->len & 0xFF;
    pn->data[pn->data_ptr++] = (in_dgram->len >> 8) & 0xFF;

    uint16_t in_ptr = 0;
    while (in_ptr < in_dgram->len) {
        int remain = pn->data_size - pn->data_ptr;
        int avail = in_dgram->len - in_ptr;
        if (avail < remain) remain = avail;
        // DEBUG_INFO(DEBUG_CATEGORY_ETCP, "copy %d bytes (in_ptr=%d, out_ptr=%d)",remain, in_ptr, pn->data_ptr);
        memcpy(pn->data + pn->data_ptr, in_dgram->dgram + in_ptr, remain);
        pn->data_ptr += remain;
        in_ptr += remain;
        if (pn_buf_renew(pn) < 0) goto exit;
    }

exit:
    queue_dgram_free(in_dgram);
    queue_entry_free(in_dgram);
    if (conn->close_requested || conn->reinit_pending || conn->reset_count != reset) {
        conn->callbacks_running--; return;
    }

    // Cancel flush timer if active
    if (pn->flush_timer) {
        uasync_call_soon_cancel(pn->ua, pn->flush_timer);
        pn->flush_timer = NULL;
    }

    // Set flush timer if no more input
    if (queue_entry_count(pn->input) == 0) {
        pn->flush_timer = uasync_call_soon(pn->ua/*, pn->tx_wait_time*/, pn, pn_flush_cb);
        if (!pn->flush_timer) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] normalizer flush scheduling OOM; restarting stream", conn->log_name);
            etcp_conn_fatal_reinit(conn, "normalizer flush OOM");
        }
    }

    queue_resume_callback(pn->input);
    conn->callbacks_running--;
}

// Internal: Flush callback on timeout
static void pn_flush_cb(void* arg) {
    ETCP_PROFILE_SCOPE(ETCP_PROF_TX_STACK);
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    struct PKTNORM* pn = (struct PKTNORM*)arg;
    if (!pn) return;

    pn->flush_timer = NULL;
    if (pn->etcp->close_requested || pn->etcp->reinit_pending || pn->etcp->session_required) return;
    pn->etcp->callbacks_running++;
    pn_send_to_etcp(pn);
    pn->etcp->callbacks_running--;
}


// Internal: Unpacker callback (assembles fragments into original packets)
static void pn_unpacker_cb(struct ll_queue* q, void* arg) {
    ETCP_PROFILE_SCOPE(ETCP_PROF_RX_STACK);
    struct PKTNORM* pn = arg;
    if (!pn || pn->etcp->close_requested || pn->etcp->reinit_pending) return;
    struct ETCP_CONN* conn = pn->etcp;
    uint32_t reset = conn->reset_count;
    conn->callbacks_running++;
    while (!conn->close_requested && !conn->reinit_pending && conn->reset_count == reset) {
        struct ETCP_FRAGMENT* frag = (struct ETCP_FRAGMENT*)queue_data_get(q);
        if (!frag) break;
        uint8_t* payload = frag->ll.dgram;
        uint16_t len = frag->ll.len, ptr = 0;
        DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] unpack fragment seq=%u bytes=%u", conn->log_name, frag->seq, len);
        while (ptr < len && !conn->close_requested && !conn->reinit_pending && conn->reset_count == reset) {
            if (!pn->recvpart) {
                if (len - ptr < 2) {
                    pn->logic_errors++;
                    DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] short normalizer header seq=%u offset=%u len=%u",
                        conn->log_name, frag->seq, ptr, len);
                    etcp_conn_fatal_reinit(conn, "normalizer desync"); break;
                }
                uint16_t size = payload[ptr] | (payload[ptr + 1] << 8);
                if (!size || size > PKTNORM_MAX_DGRAM_SIZE) {
                    pn->logic_errors++;
                    DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] invalid normalizer size=%u seq=%u offset=%u fragment_len=%u "
                        "reset=%u epochs=%016llx/%016llx", conn->log_name, size, frag->seq, ptr, len, conn->reset_count,
                        (unsigned long long)conn->reset_id, (unsigned long long)conn->peer_reset_id);
                    log_dump(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_ETCP, "Invalid normalizer fragment", payload, len < 64 ? len : 64);
                    etcp_conn_fatal_reinit(conn, "bad fragment size"); break;
                }
                pn->recvpart = ll_alloc_lldgram(size);
                if (!pn->recvpart) {
                    pn->alloc_errors++;
                    DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] normalizer RX OOM seq=%u offset=%u size=%u; restarting stream",
                        conn->log_name, frag->seq, ptr, size);
                    etcp_conn_fatal_reinit(conn, "normalizer RX OOM"); break;
                }
                ptr += 2;
            }
            uint16_t remain = pn->recvpart->memlen - pn->recvpart->len;
            uint16_t copy = remain < len - ptr ? remain : len - ptr;
            memcpy(pn->recvpart->dgram + pn->recvpart->len, payload + ptr, copy);
            pn->recvpart->len += copy; ptr += copy;
            if (pn->recvpart->len == pn->recvpart->memlen) {
                struct ll_entry* out = pn->recvpart;
                pn->recvpart = NULL;
                pn->out_total_pkts++; pn->out_total_bytes += out->len;
                DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] normalizer deliver bytes=%u", conn->log_name, out->len);
                if (queue_data_put(pn->output, out) < 0) {
                    DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] normalizer output rejected message; restarting stream", conn->log_name);
                    etcp_conn_fatal_reinit(conn, "normalizer RX queue failure");
                }
                // Callback приложения мог сбросить поток или запросить close; условие цикла проверяется до чтения старых данных.
            }
        }
        queue_dgram_free(&frag->ll); queue_entry_free(&frag->ll);
    }
    if (!conn->close_requested) queue_resume_callback(q);
    conn->callbacks_running--;
}
