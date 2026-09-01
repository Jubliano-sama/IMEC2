#ifndef MESH_RADIO_TIMING_H
#define MESH_RADIO_TIMING_H

/* Production-candidate connected-routing defaults. */
#define MESH_RADIO_ANCHOR_SCAN_RX_US 10000u
#define MESH_RADIO_ANCHOR_SCAN_RESCHEDULE_MS 380u
#define MESH_RADIO_ACTIVITY_COMPLETION_US 15000u
#define MESH_RADIO_WAKE_TRAIN_MS 500u
/* Here-I-Am is the ordinary 500 ms wake train with an activation suffix;
 * matching CLAIM stays wake-free. The separate malformed-frame listener may
 * remain armed for 1,000 ms, but it must not lengthen the transmitted train. */
#define MESH_RADIO_ENUMERATION_ACTIVATION_WAKE_TRAIN_MS \
    MESH_RADIO_WAKE_TRAIN_MS
/* Keep activation dense while avoiding a fixed transmitter/receiver cadence. */
#define MESH_RADIO_ENUMERATION_WAKE_GAP_JITTER_MAX_US 1000u
/* A real DW3000 send has bounded host/SPI/status work between the modeled
 * frame airtime and the following random gap. The low-duty acquisition slice
 * must span that complete start-to-start gap, not just airtime plus jitter. */
#define MESH_RADIO_WAKE_TX_HOST_GAP_MAX_US 5000u
#define MESH_RADIO_DISCOVERY_SLOT_US 12000u
#define MESH_RADIO_EVENT_WINDOW_MS 120u
#define MESH_RADIO_EVENT_GUARD_MS 60u
#define MESH_RADIO_EVENT_INTERVAL_MS 640u
#define MESH_RADIO_EVENT_FIRST_DELAY_MS 500u
#define MESH_RADIO_EVENT_ACCEPT_DELAY_MS 80u
#define MESH_RADIO_EVENT_CONTROL_REFERENCE_MS 10u
#define MESH_RADIO_EVENT_RETUNE_GUARD_MS MESH_RADIO_EVENT_GUARD_MS
#define MESH_RADIO_EVENT_TX_OFFSET_MS 15u
#define MESH_RADIO_EVENT_ACCEPT_REALIGN_SLOP_MS 20u
#define MESH_RADIO_EVENT_MAX_MISSES 8u
#define MESH_RADIO_EVENT_SUPERVISION_MS 300000u
#define MESH_RADIO_EVENT_RX_LATE_GUARD_MS MESH_RADIO_EVENT_GUARD_MS
#define MESH_RADIO_WAKE_SNIFF_US 20000u
#define MESH_RADIO_WAKE_POLITENESS_CHECK_US 20000u
#define MESH_RADIO_WAKE_OPPORTUNITIES 4u
#define MESH_RADIO_CONTROL_FOLLOWUP_SCAN_MS 20u
#define MESH_ROUTE_TEST_CH5_GAP_SCAN_MS 100u

/* Atomic gateway control skips the courtesy sniffs, but it still needs the
 * post-wake retune boundary before the typed frame. Here-I-Am relays and
 * other polite wake users use this conservative two-sniff envelope. */
#define MESH_RADIO_CONTROL_RELAY_WAKE_ENVELOPE_MS \
    (MESH_RADIO_WAKE_TRAIN_MS + MESH_RADIO_EVENT_RETUNE_GUARD_MS)
#define MESH_RADIO_POLITE_RELAY_WAKE_ENVELOPE_MS \
    (MESH_RADIO_CONTROL_RELAY_WAKE_ENVELOPE_MS + \
     (2u * (MESH_RADIO_WAKE_POLITENESS_CHECK_US / 1000u)))

#endif
