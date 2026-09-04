#ifndef APP_GATEWAY_BLE_STREAM_H
#define APP_GATEWAY_BLE_STREAM_H

#include "report.h"
#include "semantic_digest.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GATEWAY_BLE_STREAM_MAGIC 0x5747u
#define GATEWAY_BLE_STREAM_VERSION 1u
#define GATEWAY_BLE_STREAM_RECORD_HEADER_LEN 40u
#define GATEWAY_BLE_STREAM_RECORD_MAX_LEN \
    (GATEWAY_BLE_STREAM_RECORD_HEADER_LEN + PACKET_EXT_MAX_PAYLOAD_LEN)
#define GATEWAY_BLE_STREAM_PAYLOAD_MAX_LEN \
    (GATEWAY_BLE_STREAM_RECORD_MAX_LEN - GATEWAY_BLE_STREAM_RECORD_HEADER_LEN)
/* Six record slots: the RAM-admission ACK must not fail while a connecting
 * host still holds a few command events and receipts in the queue. The byte
 * pool, not the slot count, bounds click bursts. */
#define GATEWAY_BLE_STREAM_QUEUE_DEPTH 6u
/* Keep the gateway's static RAM below the deployable mesh policy while
 * retaining a 78-byte margin over the exact click/CIR burst bound below. */
#define GATEWAY_BLE_STREAM_RECORD_POOL_BYTES 1756u
#define GATEWAY_BLE_STREAM_RECORD_POOL_SAFETY_MARGIN_BYTES 78u
#define GATEWAY_BLE_STREAM_CLICK_CIR_TAIL_PAYLOAD_BYTES \
    (RANGE_REPORT_CIR_REMAINDER_PAYLOAD_BYTES + MESH_CH9_BATCH_METADATA_TLV_BYTES)
#define GATEWAY_BLE_STREAM_CLICK_CIR_BURST_BYTES \
    ((3u * GATEWAY_BLE_STREAM_RECORD_HEADER_LEN) + PACKET_MAX_PAYLOAD_LEN + \
     PACKET_EXT_MAX_PAYLOAD_LEN + GATEWAY_BLE_STREAM_CLICK_CIR_TAIL_PAYLOAD_BYTES)
#define GATEWAY_BLE_STREAM_RAM_BUDGET_BYTES 2304u
#define GATEWAY_BLE_RECOVERY_BACKOFF_BASE_MS 250u
#define GATEWAY_BLE_RECOVERY_BACKOFF_MAX_MS 30000u

enum gateway_ble_stream_class {
    GATEWAY_BLE_STREAM_CLASS_UNKNOWN = 0,
    GATEWAY_BLE_STREAM_CLASS_CLICK = 1,
    GATEWAY_BLE_STREAM_CLASS_RESULT = 2,
    GATEWAY_BLE_STREAM_CLASS_DIAGNOSTIC = 4,
    GATEWAY_BLE_STREAM_CLASS_STATUS = 5,
    GATEWAY_BLE_STREAM_CLASS_COMMAND_EVENT = 6,
};

enum gateway_ble_stream_record_type {
    GATEWAY_BLE_STREAM_RECORD_PACKET = 1,
};

enum gateway_ble_stream_drop_reason {
    GATEWAY_BLE_STREAM_DROP_NONE = 0,
    GATEWAY_BLE_STREAM_DROP_QUEUE_FULL = 1,
    GATEWAY_BLE_STREAM_DROP_TOO_LARGE = 2,
    GATEWAY_BLE_STREAM_DROP_NOT_READY = 3,
    GATEWAY_BLE_STREAM_DROP_PRIORITY = 4,
};

enum gateway_ble_stream_head_phase {
    GATEWAY_BLE_STREAM_HEAD_IDLE = 0,
    GATEWAY_BLE_STREAM_HEAD_SENDING = 1,
    /* Every ATT chunk reached the host; wait for the GUI's exact receipt. */
    GATEWAY_BLE_STREAM_HEAD_HOST_NOTIFIED = 2,
    /* The GUI receipt matched the exact stored record; mesh ACK handoff owns
     * the remaining custody and the record must not be resent on disconnect. */
    GATEWAY_BLE_STREAM_HEAD_HOST_ACCEPTED = 3,
};

struct gateway_ble_stream_diagnostics {
    uint32_t enqueue_attempts;
    uint32_t packets_sent;
    uint32_t bytes_sent;
    uint32_t drops_queue_full;
    uint32_t drops_too_large;
    uint32_t drops_not_ready;
    uint32_t drops_priority;
    uint8_t max_queue_depth_observed;
    uint32_t oldest_queued_age_ms;
    uint8_t last_dropped_packet_type;
    enum gateway_ble_stream_drop_reason last_drop_reason;
};

struct gateway_ble_stream_item {
    uint16_t offset;
    uint16_t len;
    uint8_t packet_type;
    uint8_t priority;
    bool retain_until_sent;
    bool host_custody_owner;
    uint32_t queued_at_ms;
    uint32_t received_at_ms;
    struct proto_packet packet;
};

struct gateway_ble_stream_state {
    uint8_t record_pool[GATEWAY_BLE_STREAM_RECORD_POOL_BYTES];
    /* An active reservation owns the final item slot but is not in count. */
    struct gateway_ble_stream_item items[GATEWAY_BLE_STREAM_QUEUE_DEPTH];
    uint16_t pool_used;
    uint16_t reservation_payload_len;
    uint8_t reservation_payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
    /*
     * The exact buffer the reservation digest was taken over.  A commit that
     * presents the same object and length is proven by that digest already,
     * which keeps a second SHA-256 pass over a whole report out of the
     * gateway receive-to-ACK path.  Any other buffer is still re-hashed and
     * compared.
     */
    const uint8_t *reservation_payload_source;
    uint8_t count;
    uint8_t head_send_phase;
    bool reservation_active;
    /* A semantic source item owns one host-custody payload boundary until
     * mesh ACK handoff and exact GUI receipt completion retire it. */
    bool host_custody_source_payload_active;
    struct gateway_ble_stream_diagnostics diagnostics;
};

/*
 * Ownership-only state for the direct COBS-frame ring in app_gateway_ble.c.
 * The frame storage stays with the Zephyr transport, while this small helper
 * makes the important rule native-testable: selecting a head does not remove
 * it, and a cancelled transfer restarts that same head.
 */
struct gateway_ble_direct_queue_state {
    uint8_t head;
    uint8_t count;
    bool head_active;
};

typedef int (*gateway_ble_stream_send_fn)(const uint8_t *record,
                                          size_t record_len,
                                          void *ctx);
typedef int (*gateway_ble_custody_send_fn)(const uint8_t *frame,
                                           size_t frame_len,
                                           void *ctx);
typedef void (*gateway_ble_custody_wait_fn)(void *ctx);

void gateway_ble_stream_init(struct gateway_ble_stream_state *state);
bool gateway_ble_should_stream_packet(uint8_t msg_type,
                                      uint8_t flags,
                                      enum gateway_ble_stream_class packet_class);
enum gateway_ble_stream_class gateway_ble_stream_classify_packet(uint8_t msg_type,
                                                                 uint8_t flags);
int gateway_ble_stream_enqueue_packet(struct gateway_ble_stream_state *state,
                                      const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint32_t received_at_ms,
                                      uint32_t now_ms,
                                      bool ble_ready);
int gateway_ble_stream_enqueue_retained_packet(
    struct gateway_ble_stream_state *state,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t received_at_ms,
    uint32_t now_ms,
    bool ble_ready);
int gateway_ble_stream_reserve_packet(struct gateway_ble_stream_state *state,
                                      const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint32_t received_at_ms,
                                      uint32_t now_ms,
                                      bool ble_ready);
int gateway_ble_stream_commit_reservation(
    struct gateway_ble_stream_state *state,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);
int gateway_ble_stream_commit_bundle_projection_reservation(
    struct gateway_ble_stream_state *state,
    const struct proto_packet *packet,
    const uint8_t *raw_payload,
    size_t raw_payload_len,
    uint8_t accepted_record_mask);
void gateway_ble_stream_cancel_reservation(
    struct gateway_ble_stream_state *state);
unsigned int gateway_ble_stream_drain(struct gateway_ble_stream_state *state,
                                      gateway_ble_stream_send_fn send_fn,
                                      void *send_ctx,
                                      uint32_t now_ms,
                                      bool ble_ready,
                                      unsigned int max_records);
int gateway_ble_stream_peek(const struct gateway_ble_stream_state *state,
                            const uint8_t **record,
                            size_t *record_len);
int gateway_ble_stream_begin_send(struct gateway_ble_stream_state *state,
                                  uint8_t *record,
                                  size_t record_cap,
                                  size_t *record_len);
int gateway_ble_stream_begin_send_view(struct gateway_ble_stream_state *state,
                                       const uint8_t **record,
                                       size_t *record_len);
void gateway_ble_stream_cancel_send(struct gateway_ble_stream_state *state);
/* Move a retained head from SENDING to HOST_NOTIFIED after ATT completion. */
int gateway_ble_stream_mark_host_notified(
    struct gateway_ble_stream_state *state);
/* Rewind only an unreceipted host notification so the exact record can resend. */
int gateway_ble_stream_rewind_host_notification(
    struct gateway_ble_stream_state *state);
/* Accept the exact host receipt boundary; only HOST_NOTIFIED may advance. */
int gateway_ble_stream_accept_host_receipt(
    struct gateway_ble_stream_state *state);
void gateway_ble_stream_mark_sent(struct gateway_ble_stream_state *state,
                                  uint32_t now_ms);
void gateway_ble_stream_get_diagnostics(
    const struct gateway_ble_stream_state *state,
    uint32_t now_ms,
    struct gateway_ble_stream_diagnostics *diagnostics);
uint8_t gateway_ble_stream_depth(const struct gateway_ble_stream_state *state);
int gateway_ble_stream_head_packet(const struct gateway_ble_stream_state *state,
                                   struct proto_packet *packet);
uint32_t gateway_ble_recovery_backoff_ms(uint8_t retry_round,
                                         uint32_t random_value);
void gateway_ble_direct_queue_init(
    struct gateway_ble_direct_queue_state *state);
int gateway_ble_direct_queue_enqueue(
    struct gateway_ble_direct_queue_state *state,
    uint8_t capacity,
    uint8_t *slot);
int gateway_ble_direct_queue_begin(
    struct gateway_ble_direct_queue_state *state,
    uint8_t capacity,
    uint8_t *slot);
void gateway_ble_direct_queue_cancel(
    struct gateway_ble_direct_queue_state *state);
int gateway_ble_direct_queue_complete(
    struct gateway_ble_direct_queue_state *state,
    uint8_t capacity);
bool gateway_ble_custody_error_retryable(int error);
bool gateway_ble_work_handoff_requires_reset(int schedule_result);
/*
 * Retry one immutable frame until the lower layer accepts custody. Temporary
 * connection, CCC, quiet-window, or capacity refusal never changes the frame
 * or returns it to the producer.
 */
int gateway_ble_send_frame_retained(
    const uint8_t *frame,
    size_t frame_len,
    gateway_ble_custody_send_fn send_fn,
    gateway_ble_custody_wait_fn wait_fn,
    void *ctx,
    uint32_t *retry_count);

#ifdef __cplusplus
}
#endif

#endif
