/**
 * @file ml_zerocopy.c
 * @brief Zero-copy WireGuard receive path (high-throughput mode)
 *
 * Kconfig: CONFIG_ML_ZERO_COPY_WG
 *
 * Uses a raw lwIP UDP PCB instead of a BSD socket for the DISCO/WG UDP port.
 * The PCB recv callback runs in tcpip_thread and demultiplexes:
 *   - WireGuard packets → wireguardif_network_rx() directly (ZERO COPY)
 *   - DISCO packets → SPSC ring buffer → wg_mgr task
 *   - STUN packets → stun_rx_queue (still uses FreeRTOS queue)
 *
 * The net_io task's select() loop skips disco_sock4 when zero-copy is active.
 * STUN sockets remain BSD-based since STUN is low-frequency.
 *
 * Contributed by dj-oyu (https://github.com/dj-oyu/microlink)
 * Adapted for MicroLink V2 task architecture.
 */

#include "sdkconfig.h"

#ifdef CONFIG_ML_ZERO_COPY_WG

#include "microlink_internal.h"
#include "esp_log.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "wireguardif.h"
#include "wireguard.h"
#include <string.h>

static const char *TAG = "ml_zc";

/* DISCO magic bytes: "TS" + sparkles emoji UTF-8 */
static const uint8_t DISCO_MAGIC[6] = { 'T', 'S', 0xf0, 0x9f, 0x92, 0xac };

/* STUN magic cookie at bytes 4-7 */
#define STUN_MAGIC_COOKIE 0x2112A442

/* Forward declarations */
extern void wireguardif_network_rx(void *arg, struct udp_pcb *pcb,
                                    struct pbuf *p, const ip_addr_t *addr, u16_t port);
static void zc_send_in_tcpip(void *arg);

/* ============================================================================
 * PCB Receive Callback (runs in tcpip_thread — FAST PATH)
 *
 * Packet classification order matters for performance:
 * 1. WireGuard (most frequent in streaming) → zero-copy inject
 * 2. DISCO (rare control packets) → ring buffer
 * 3. STUN (very rare) → queue
 * ========================================================================== */

static void zc_pcb_recv_cb(void *arg, struct udp_pcb *pcb,
                            struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    microlink_t *ml = (microlink_t *)arg;
    if (!ml || !p) {
        if (p) pbuf_free(p);
        return;
    }

    size_t len = p->tot_len;

    /* Classification only ever looks at the first 8 bytes (STUN cookie) or
     * first 6 (DISCO magic); copy just that much out via pbuf_copy_partial
     * so a chained pbuf whose first segment is shorter than `len` can't be
     * read past its own p->len. (The actual payload handling below still
     * uses `p` itself via pbuf_copy_partial / wireguardif_network_rx, which
     * both already walk the full chain correctly.) */
    uint8_t hdr[8];
    size_t hdr_len = len < sizeof(hdr) ? len : sizeof(hdr);
    pbuf_copy_partial(p, hdr, hdr_len, 0);

    /* Classify packet */
    bool is_disco = (len >= 62 && hdr_len >= 6 && memcmp(hdr, DISCO_MAGIC, 6) == 0);

    bool is_stun = false;
    if (!is_disco && len >= 20 && hdr_len >= 8) {
        uint32_t cookie = ((uint32_t)hdr[4] << 24) | ((uint32_t)hdr[5] << 16) |
                          ((uint32_t)hdr[6] << 8) | (uint32_t)hdr[7];
        is_stun = (cookie == STUN_MAGIC_COOKIE);
    }

    if (is_disco) {
        /* DISCO → copy to SPSC ring buffer (small, rare packets) */
        uint8_t head = __atomic_load_n(&ml->zc.rx_head, __ATOMIC_RELAXED);
        uint8_t next = (head + 1) % ML_ZC_DISCO_RING_SIZE;
        uint8_t tail = __atomic_load_n(&ml->zc.rx_tail, __ATOMIC_ACQUIRE);

        if (next != tail) {
            ml_zc_disco_entry_t *entry = &ml->zc.rx_ring[head];
            entry->len = (len > ML_ZC_DISCO_MAX_PKT) ? ML_ZC_DISCO_MAX_PKT : len;
            pbuf_copy_partial(p, entry->data, entry->len, 0);
            entry->src_ip_nbo = ip_addr_get_ip4_u32(addr);
            entry->src_port = port;
            __atomic_store_n(&ml->zc.rx_head, next, __ATOMIC_RELEASE);
        }
        /* else: ring full, drop (DISCO is retried by design) */
        pbuf_free(p);

    } else if (is_stun) {
        /* STUN → heap-alloc + queue (low frequency, existing path) */
        uint8_t *pkt_data = malloc(len);
        if (pkt_data) {
            pbuf_copy_partial(p, pkt_data, len, 0);
            ml_rx_packet_t pkt = {
                .data = pkt_data,
                .len = len,
                .src_ip = ntohl(ip_addr_get_ip4_u32(addr)),
                .src_port = port,
                .via_derp = false,
            };
            if (xQueueSend(ml->stun_rx_queue, &pkt, 0) != pdTRUE) {
                free(pkt_data);
            }
        }
        pbuf_free(p);

    } else if (len >= 4 && ml->wg_netif) {
        /* WireGuard → ZERO COPY direct injection */
        struct netif *wg_netif = (struct netif *)ml->wg_netif;
        void *device = wg_netif->state;
        if (device) {
            /* wireguardif_network_rx takes ownership of pbuf */
            wireguardif_network_rx(device, NULL, p, addr, port);
            return;  /* DO NOT free — WG owns the pbuf now */
        }
        pbuf_free(p);

    } else {
        pbuf_free(p);
    }
}

/* ============================================================================
 * PCB Init/Deinit (called via tcpip_callback for thread safety)
 * ========================================================================== */

typedef struct {
    microlink_t *ml;
    err_t result;
    SemaphoreHandle_t done;
} zc_setup_ctx_t;

static void zc_pcb_create_in_tcpip(void *arg) {
    zc_setup_ctx_t *ctx = (zc_setup_ctx_t *)arg;
    microlink_t *ml = ctx->ml;

    ml->zc.pcb = udp_new();
    if (!ml->zc.pcb) {
        ctx->result = ERR_MEM;
        xSemaphoreGive(ctx->done);
        return;
    }

    ctx->result = udp_bind(ml->zc.pcb, IP_ADDR_ANY, 0);
    if (ctx->result != ERR_OK) {
        udp_remove(ml->zc.pcb);
        ml->zc.pcb = NULL;
        xSemaphoreGive(ctx->done);
        return;
    }

    ml->zc.local_port = ml->zc.pcb->local_port;

    /* DSCP 46 (EF) → WMM AC_VO for low-latency WiFi scheduling */
    ml->zc.pcb->tos = 0xB8;

    /* Bind to WiFi netif (skip WG netif) */
    extern struct netif *netif_list;
    for (struct netif *n = netif_list; n != NULL; n = n->next) {
        if (n != (struct netif *)ml->wg_netif) {
            udp_bind_netif(ml->zc.pcb, n);
            break;
        }
    }

    /* Register receive callback */
    udp_recv(ml->zc.pcb, zc_pcb_recv_cb, ml);

    ctx->result = ERR_OK;
    xSemaphoreGive(ctx->done);
}

static void zc_pcb_remove_in_tcpip(void *arg) {
    zc_setup_ctx_t *ctx = (zc_setup_ctx_t *)arg;
    if (ctx->ml->zc.pcb) {
        udp_remove(ctx->ml->zc.pcb);
        ctx->ml->zc.pcb = NULL;
    }
    xSemaphoreGive(ctx->done);
}

/* ============================================================================
 * Public API
 * ========================================================================== */

esp_err_t ml_zerocopy_init(microlink_t *ml) {
    ESP_LOGI(TAG, "Initializing zero-copy WG path");

    /* On a re-init (rebind), wg_mgr_task's drain loop may still be catching
     * up on DISCO entries queued before the old PCB was removed. Give it a
     * brief chance to empty the ring before we reset the indices under it —
     * bounded, since nothing new can arrive once the old PCB is gone. */
    int waited_ms = 0;
    while (__atomic_load_n(&ml->zc.rx_head, __ATOMIC_ACQUIRE) !=
           __atomic_load_n(&ml->zc.rx_tail, __ATOMIC_ACQUIRE)) {
        vTaskDelay(pdMS_TO_TICKS(2));
        waited_ms += 2;
        if (waited_ms >= 100) break;
    }

    /* Clear ring buffer state */
    memset(&ml->zc, 0, sizeof(ml->zc));
    /* portMUX_TYPE's "unlocked" value is NOT all-zero bits (see spinlock.h) —
     * must be set explicitly, the memset above does not make it valid. */
    ml->zc.tx_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    ml->zc.closing = false;

    /* Create PCB in tcpip_thread */
    zc_setup_ctx_t ctx = {
        .ml = ml,
        .result = ERR_IF,
        .done = xSemaphoreCreateBinary(),
    };
    if (!ctx.done) return ESP_ERR_NO_MEM;

    err_t err = tcpip_callback(zc_pcb_create_in_tcpip, &ctx);
    if (err != ERR_OK) {
        vSemaphoreDelete(ctx.done);
        return ESP_FAIL;
    }

    xSemaphoreTake(ctx.done, portMAX_DELAY);
    vSemaphoreDelete(ctx.done);

    if (ctx.result != ERR_OK) {
        ESP_LOGE(TAG, "PCB creation failed: %d", ctx.result);
        return ESP_FAIL;
    }

    /* Store the port so coord can advertise it in endpoints */
    ml->disco_local_port = ml->zc.local_port;

    /* Mark disco_sock4 as unused — net_io won't select() on it */
    ml->disco_sock4 = -1;

    ESP_LOGI(TAG, "Zero-copy WG active on port %d", ml->zc.local_port);
    return ESP_OK;
}

void ml_zerocopy_deinit(microlink_t *ml) {
    if (!ml->zc.pcb) return;

    ESP_LOGI(TAG, "Shutting down zero-copy WG path");

    /* 1. Block any new send from claiming a TX slot (see the matching
     *    critical section in ml_zerocopy_send()). */
    taskENTER_CRITICAL(&ml->zc.tx_lock);
    ml->zc.closing = true;
    taskEXIT_CRITICAL(&ml->zc.tx_lock);

    /* 2. Drain: wait for every send that already claimed a slot (and so is
     *    either about to run, or already queued, in tcpip_thread) to finish
     *    executing zc_send_in_tcpip() and advance tx_tail. Once head==tail,
     *    no callback anywhere still holds a reference to ml->zc.pcb. */
    int waited_ms = 0;
    while (__atomic_load_n(&ml->zc.tx_head, __ATOMIC_ACQUIRE) !=
           __atomic_load_n(&ml->zc.tx_tail, __ATOMIC_ACQUIRE)) {
        vTaskDelay(pdMS_TO_TICKS(5));
        waited_ms += 5;
        if (waited_ms >= 2000) {
            ESP_LOGW(TAG, "TX drain timed out after %dms — proceeding anyway", waited_ms);
            break;
        }
    }

    /* 3. Only now is it safe to actually free the PCB. */
    zc_setup_ctx_t ctx = {
        .ml = ml,
        .done = xSemaphoreCreateBinary(),
    };
    if (!ctx.done) return;

    tcpip_callback(zc_pcb_remove_in_tcpip, &ctx);
    xSemaphoreTake(ctx.done, portMAX_DELAY);
    vSemaphoreDelete(ctx.done);
}

esp_err_t ml_zerocopy_send(microlink_t *ml, const uint8_t *data, size_t len,
                            uint32_t dest_ip, uint16_t dest_port) {
    if (len > ML_MAX_PACKET_SIZE) return ESP_ERR_INVALID_ARG;

    /* Everything that decides "is the PCB still alive, may I claim a slot"
     * happens under tx_lock, and ml_zerocopy_deinit() sets `closing` under
     * the same lock before it starts tearing the PCB down. That makes the
     * two mutually exclusive: either this send claims a slot before closing
     * is set (deinit's drain below then waits for it to finish, so the PCB
     * outlives it), or it sees closing==true and bails out before touching
     * the PCB at all. Either way, a stale `ctx->pcb` can never reach
     * zc_send_in_tcpip() after the PCB has been freed. */
    taskENTER_CRITICAL(&ml->zc.tx_lock);
    if (ml->zc.closing || !ml->zc.pcb) {
        taskEXIT_CRITICAL(&ml->zc.tx_lock);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t head = ml->zc.tx_head;
    uint8_t next = (head + 1) % ML_ZC_TX_POOL_SIZE;
    uint8_t tail = __atomic_load_n(&ml->zc.tx_tail, __ATOMIC_ACQUIRE);
    if (next == tail) {
        taskEXIT_CRITICAL(&ml->zc.tx_lock);
        return ESP_ERR_NO_MEM;  /* TX pool full */
    }

    ml_zc_tx_ctx_t *ctx = &ml->zc.tx_pool[head];
    ctx->pcb = ml->zc.pcb;
    memcpy(ctx->data, data, len);
    ctx->len = len;
    IP_SET_TYPE_VAL(ctx->dest, IPADDR_TYPE_V4);
    ip4_addr_set_u32(ip_2_ip4(&ctx->dest), htonl(dest_ip));
    ctx->port = dest_port;

    __atomic_store_n(&ml->zc.tx_head, next, __ATOMIC_RELEASE);
    taskEXIT_CRITICAL(&ml->zc.tx_lock);

    /* Schedule send in tcpip_thread */
    tcpip_callback(zc_send_in_tcpip, ctx);
    return ESP_OK;
}

/* TX callback — runs in tcpip_thread */
static void zc_send_in_tcpip(void *arg) {
    ml_zc_tx_ctx_t *ctx = (ml_zc_tx_ctx_t *)arg;
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, ctx->len, PBUF_RAM);
    if (p) {
        memcpy(p->payload, ctx->data, ctx->len);
        /* udp_sendto() never takes ownership of our pbuf (success or failure) —
         * caller must always free its own reference. */
        udp_sendto(ctx->pcb, p, &ctx->dest, ctx->port);
        pbuf_free(p);
    }

    /* Find the ml context from the PCB arg to advance tail.
     * We use the parent struct offset trick since ctx is inside ml->zc.tx_pool. */
    microlink_t *ml = (microlink_t *)ctx->pcb->recv_arg;
    if (ml) {
        uint8_t tail = __atomic_load_n(&ml->zc.tx_tail, __ATOMIC_RELAXED);
        __atomic_store_n(&ml->zc.tx_tail,
                         (uint8_t)((tail + 1) % ML_ZC_TX_POOL_SIZE),
                         __ATOMIC_RELEASE);
    }
}

#endif /* CONFIG_ML_ZERO_COPY_WG */
