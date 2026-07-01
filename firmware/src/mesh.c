#include "mesh.h"

static bool ids_are_valid(uint64_t src_id, uint64_t dst_id)
{
    return src_id != 0u && dst_id != 0u && src_id != dst_id;
}

static bool command_status_valid(enum command_status status)
{
    return status >= COMMAND_OK && status <= COMMAND_INTERNAL_ERROR;
}

static bool event_control_type_valid(uint8_t msg_type)
{
    return msg_type == MSG_MESH_EVENT_PROPOSE ||
           msg_type == MSG_MESH_EVENT_ACCEPT ||
           msg_type == MSG_MESH_EVENT_UPDATE ||
           msg_type == MSG_MESH_EVENT_END;
}

static bool time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static uint32_t time_until_deadline(uint32_t now_ms, uint32_t deadline_ms)
{
    if (time_reached(now_ms, deadline_ms)) {
        return 1u;
    }
    return deadline_ms - now_ms;
}

static uint32_t nonzero_deadline_after(uint32_t now_ms, uint32_t delay_ms)
{
    uint32_t deadline_ms = now_ms + (delay_ms == 0u ? 1u : delay_ms);

    return deadline_ms == 0u ? 1u : deadline_ms;
}

static void counter_add(uint32_t *counter, uint32_t delta)
{
    if (counter == NULL) {
        return;
    }
    if (UINT32_MAX - *counter < delta) {
        *counter = UINT32_MAX;
    } else {
        *counter += delta;
    }
}

static bool mesh_event_params_valid(const struct mesh_event_params *params)
{
    return params != NULL &&
           params->event_interval_ms > 0u &&
           params->event_window_ms > 0u &&
           params->first_event_time_ms > 0u &&
           params->guard_ms > 0u &&
           params->max_missed_events > 0u &&
           params->supervision_timeout_ms >= params->event_interval_ms;
}

static int find_u8_tlv(const uint8_t *payload, size_t payload_len, uint8_t type, uint8_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    int ret;

    ret = tlv_find(payload, payload_len, type, &tlv_value, &tlv_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (tlv_len != sizeof(uint8_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = tlv_value[0];
    return PROTO_OK;
}

static int find_u16_tlv(const uint8_t *payload, size_t payload_len, uint8_t type, uint16_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    int ret;

    ret = tlv_find(payload, payload_len, type, &tlv_value, &tlv_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (tlv_len != sizeof(uint16_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = proto_get_u16_le(tlv_value);
    return PROTO_OK;
}

static int find_u32_tlv(const uint8_t *payload, size_t payload_len, uint8_t type, uint32_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    int ret;

    ret = tlv_find(payload, payload_len, type, &tlv_value, &tlv_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (tlv_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = proto_get_u32_le(tlv_value);
    return PROTO_OK;
}

int mesh_event_timing_negotiate(struct mesh_event_timing *timing,
                                const struct mesh_event_params *params,
                                bool channel5_contact_refreshed)
{
    if (timing == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!mesh_event_params_valid(params)) {
        return PROTO_ERR_MALFORMED;
    }
    if (!channel5_contact_refreshed) {
        return PROTO_ERR_BUSY;
    }

    timing->mesh_channel = MESH_EVENT_CHANNEL;
    timing->event_interval_ms = params->event_interval_ms;
    timing->event_window_ms = params->event_window_ms;
    timing->next_event_time_ms = params->first_event_time_ms;
    timing->event_counter = 0u;
    timing->guard_ms = params->guard_ms;
    timing->peer_clock_skew_estimate_ppm = params->peer_clock_skew_estimate_ppm;
    timing->max_missed_events = params->max_missed_events;
    timing->missed_event_count = 0u;
    timing->supervision_timeout_ms = params->supervision_timeout_ms;
    timing->last_successful_ch9_event_ms = params->first_event_time_ms;
    timing->local_tx_on_even_events = true;
    timing->route_fresh = true;
    timing->timing_fresh = true;
    timing->fallback_required = false;
    return PROTO_OK;
}

bool mesh_event_timing_usable(const struct mesh_event_timing *timing,
                              uint32_t now_ms)
{
    if (timing == NULL ||
        timing->mesh_channel != MESH_EVENT_CHANNEL ||
        timing->event_interval_ms == 0u ||
        timing->event_window_ms == 0u ||
        timing->guard_ms == 0u ||
        timing->supervision_timeout_ms == 0u ||
        !timing->route_fresh ||
        !timing->timing_fresh ||
        timing->fallback_required) {
        return false;
    }

    return !time_reached(now_ms,
                         timing->last_successful_ch9_event_ms +
                         timing->supervision_timeout_ms);
}

void mesh_event_timing_set_local_first_slot_tx(struct mesh_event_timing *timing,
                                               bool local_first_slot_tx)
{
    if (timing == NULL) {
        return;
    }

    timing->local_tx_on_even_events = local_first_slot_tx;
}

bool mesh_event_timing_local_tx_slot(const struct mesh_event_timing *timing)
{
    bool even_event;

    if (timing == NULL) {
        return false;
    }

    even_event = (timing->event_counter & 1u) == 0u;
    return timing->local_tx_on_even_events == even_event;
}

bool mesh_event_timing_local_rx_slot(const struct mesh_event_timing *timing)
{
    return timing != NULL && !mesh_event_timing_local_tx_slot(timing);
}

uint32_t mesh_event_guard_start_ms(const struct mesh_event_timing *timing)
{
    uint16_t guard_ms;

    if (timing == NULL) {
        return 0u;
    }

    guard_ms = timing->guard_ms;
    if (timing->next_event_time_ms <= guard_ms) {
        return 1u;
    }
    return timing->next_event_time_ms - guard_ms;
}

int mesh_event_plan_channel9(const struct mesh_event_timing *timing,
                             const struct mesh_channel5_requirements *requirements,
                             uint32_t now_ms,
                             struct mesh_event_plan *plan)
{
    uint32_t start_ms;
    uint32_t end_ms;
    uint32_t latest_end_ms = 0u;
    uint16_t retune_guard_ms;
    uint32_t guard_start_ms;

    if (timing == NULL || requirements == NULL || plan == NULL) {
        return PROTO_ERR_ARG;
    }

    plan->start_ms = 0u;
    plan->end_ms = 0u;
    plan->window_ms = 0u;
    if (!mesh_event_timing_usable(timing, now_ms)) {
        plan->action = MESH_EVENT_PLAN_REFRESH_CONTACT_CH5;
        return PROTO_OK;
    }

    retune_guard_ms = requirements->retune_guard_ms > timing->guard_ms ?
                      requirements->retune_guard_ms : timing->guard_ms;
    start_ms = timing->next_event_time_ms;
    guard_start_ms = timing->next_event_time_ms <= retune_guard_ms ?
                     1u :
                     timing->next_event_time_ms - retune_guard_ms;
    plan->start_ms = start_ms;
    plan->window_ms = timing->event_window_ms;
    plan->end_ms = start_ms + timing->event_window_ms;

    if (!time_reached(now_ms, guard_start_ms)) {
        plan->action = MESH_EVENT_PLAN_WAIT;
        return PROTO_OK;
    }

    if (requirements->click_epoch_active ||
        requirements->discovery_active ||
        requirements->ranging_active ||
        (requirements->active_until_ms != 0u &&
         time_reached(requirements->active_until_ms, start_ms))) {
        plan->action = MESH_EVENT_PLAN_DEFER_CH5_ACTIVE;
        return PROTO_OK;
    }

    if (requirements->next_required_scan_start_ms != 0u &&
        !time_reached(requirements->next_required_scan_start_ms,
                      start_ms + retune_guard_ms)) {
        plan->action = MESH_EVENT_PLAN_SKIP_CH5_SCAN_GUARD;
        plan->window_ms = 0u;
        plan->end_ms = start_ms;
        return PROTO_OK;
    }
    if (requirements->next_required_scan_start_ms > retune_guard_ms) {
        latest_end_ms = requirements->next_required_scan_start_ms - retune_guard_ms;
    }
    end_ms = start_ms + timing->event_window_ms;
    if (latest_end_ms != 0u && time_reached(end_ms, latest_end_ms + 1u)) {
        if (time_reached(latest_end_ms, start_ms + 1u)) {
            plan->action = MESH_EVENT_PLAN_CLIP;
            plan->window_ms = (uint16_t)(latest_end_ms - start_ms);
            plan->end_ms = latest_end_ms;
            return PROTO_OK;
        }
        plan->action = MESH_EVENT_PLAN_SKIP_CH5_SCAN_GUARD;
        plan->window_ms = 0u;
        plan->end_ms = start_ms;
        return PROTO_OK;
    }

    plan->action = MESH_EVENT_PLAN_START;
    return PROTO_OK;
}

void mesh_event_note_success(struct mesh_event_timing *timing,
                             uint32_t event_start_ms)
{
    if (timing == NULL) {
        return;
    }

    timing->last_successful_ch9_event_ms = event_start_ms;
    timing->next_event_time_ms = event_start_ms + timing->event_interval_ms;
    timing->event_counter++;
    timing->missed_event_count = 0u;
    timing->timing_fresh = true;
    timing->fallback_required = false;
}

void mesh_event_note_local_tx(struct mesh_event_timing *timing,
                              uint32_t event_start_ms)
{
    if (timing == NULL) {
        return;
    }

    timing->next_event_time_ms = event_start_ms + timing->event_interval_ms;
    timing->event_counter++;
    timing->timing_fresh = true;
    timing->fallback_required = false;
}

void mesh_event_note_observed_packet(struct mesh_event_timing *timing,
                                     uint32_t planned_event_start_ms,
                                     uint32_t observed_packet_ms)
{
    if (timing == NULL) {
        return;
    }

    (void)observed_packet_ms;
    if (timing->event_counter > 0u &&
        timing->last_successful_ch9_event_ms == planned_event_start_ms) {
        return;
    }
    mesh_event_note_success(timing, planned_event_start_ms);
}

void mesh_event_note_missed(struct mesh_event_timing *timing,
                            struct mesh_event_diagnostics *diagnostics)
{
    if (timing == NULL) {
        return;
    }

    if (timing->missed_event_count < UINT8_MAX) {
        timing->missed_event_count++;
    }
    timing->next_event_time_ms += timing->event_interval_ms;
    timing->event_counter++;
    counter_add(diagnostics == NULL ? NULL : &diagnostics->ch9_event_misses, 1u);
}

uint8_t mesh_event_skip_elapsed(struct mesh_event_timing *timing,
                                uint32_t now_ms,
                                struct mesh_event_diagnostics *diagnostics)
{
    uint8_t skipped = 0u;

    if (timing == NULL ||
        timing->event_interval_ms == 0u ||
        timing->event_window_ms == 0u) {
        return 0u;
    }

    while (skipped < UINT8_MAX) {
        uint32_t event_end_ms = timing->next_event_time_ms + timing->event_window_ms;

        if (!time_reached(now_ms, event_end_ms)) {
            break;
        }
        mesh_event_note_missed(timing, diagnostics);
        skipped++;
    }

    return skipped;
}

void mesh_event_note_channel_switch(struct mesh_event_diagnostics *diagnostics,
                                    bool pll_ready,
                                    bool late_channel5_return)
{
    if (diagnostics == NULL) {
        return;
    }

    counter_add(&diagnostics->channel_switches, 1u);
    if (!pll_ready) {
        counter_add(&diagnostics->pll_ready_failures, 1u);
    }
    if (late_channel5_return) {
        counter_add(&diagnostics->late_channel5_returns, 1u);
    }
}

void mesh_event_note_plan_action(struct mesh_event_diagnostics *diagnostics,
                                 enum mesh_event_plan_action action)
{
    if (diagnostics == NULL) {
        return;
    }

    if (action == MESH_EVENT_PLAN_DEFER_CH5_ACTIVE ||
        action == MESH_EVENT_PLAN_SKIP_CH5_SCAN_GUARD ||
        action == MESH_EVENT_PLAN_CLIP) {
        counter_add(&diagnostics->mesh_deferrals, 1u);
    }
    if (action == MESH_EVENT_PLAN_DEFER_CH5_ACTIVE ||
        action == MESH_EVENT_PLAN_SKIP_CH5_SCAN_GUARD) {
        counter_add(&diagnostics->channel5_preemptions, 1u);
    }
}

void mesh_event_note_report_latency(struct mesh_event_diagnostics *diagnostics,
                                    uint32_t latency_ms)
{
    if (diagnostics == NULL) {
        return;
    }

    counter_add(&diagnostics->ch9_report_latency_ms, latency_ms);
}

int mesh_append_event_timing_tlvs_at(uint8_t *payload,
                                     size_t payload_cap,
                                     size_t *offset,
                                     const struct mesh_event_timing *timing,
                                     uint32_t now_ms)
{
    int ret;

    if (timing == NULL || timing->mesh_channel != MESH_EVENT_CHANNEL) {
        return PROTO_ERR_MALFORMED;
    }

    ret = tlv_append_u8(payload, payload_cap, offset, TLV_MESH_CHANNEL, timing->mesh_channel);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset,
                         TLV_MESH_EVENT_INTERVAL_MS,
                         timing->event_interval_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_MESH_EVENT_WINDOW_MS,
                         timing->event_window_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset,
                         TLV_MESH_NEXT_EVENT_TIME_MS,
                         time_until_deadline(now_ms, timing->next_event_time_ms));
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset,
                         TLV_MESH_EVENT_COUNTER,
                         timing->event_counter);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_MESH_EVENT_GUARD_MS,
                         timing->guard_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_MESH_CLOCK_SKEW_PPM,
                         (uint16_t)timing->peer_clock_skew_estimate_ppm);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset,
                        TLV_MESH_MAX_MISSED_EVENTS,
                        timing->max_missed_events);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u32(payload, payload_cap, offset,
                          TLV_MESH_SUPERVISION_TIMEOUT_MS,
                          timing->supervision_timeout_ms);
}

int mesh_append_event_timing_tlvs(uint8_t *payload,
                                  size_t payload_cap,
                                  size_t *offset,
                                  const struct mesh_event_timing *timing)
{
    return mesh_append_event_timing_tlvs_at(payload, payload_cap, offset, timing, 0u);
}

int mesh_event_timing_from_tlvs_at(struct mesh_event_timing *timing,
                                   const uint8_t *payload,
                                   size_t payload_len,
                                   uint32_t now_ms,
                                   bool channel5_contact_refreshed)
{
    struct mesh_event_params params = {0};
    uint8_t mesh_channel = 0u;
    uint16_t clock_skew = 0u;
    uint32_t event_counter = 0u;
    uint32_t event_delay_ms = 0u;
    int ret;

    if (timing == NULL || payload == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = find_u8_tlv(payload, payload_len, TLV_MESH_CHANNEL, &mesh_channel);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (mesh_channel != MESH_EVENT_CHANNEL) {
        return PROTO_ERR_MALFORMED;
    }
    ret = find_u32_tlv(payload,
                       payload_len,
                       TLV_MESH_EVENT_INTERVAL_MS,
                       &params.event_interval_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload,
                       payload_len,
                       TLV_MESH_EVENT_WINDOW_MS,
                       &params.event_window_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u32_tlv(payload,
                       payload_len,
                       TLV_MESH_NEXT_EVENT_TIME_MS,
                       &event_delay_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    params.first_event_time_ms = nonzero_deadline_after(now_ms, event_delay_ms);
    ret = find_u16_tlv(payload,
                       payload_len,
                       TLV_MESH_EVENT_GUARD_MS,
                       &params.guard_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload,
                       payload_len,
                       TLV_MESH_CLOCK_SKEW_PPM,
                       &clock_skew);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u8_tlv(payload,
                      payload_len,
                      TLV_MESH_MAX_MISSED_EVENTS,
                      &params.max_missed_events);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u32_tlv(payload,
                       payload_len,
                       TLV_MESH_SUPERVISION_TIMEOUT_MS,
                       &params.supervision_timeout_ms);
    if (ret != PROTO_OK) {
        return ret;
    }

    params.peer_clock_skew_estimate_ppm = (int16_t)clock_skew;
    ret = mesh_event_timing_negotiate(timing, &params, channel5_contact_refreshed);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (find_u32_tlv(payload, payload_len, TLV_MESH_EVENT_COUNTER, &event_counter) ==
        PROTO_OK) {
        timing->event_counter = event_counter;
    }
    return PROTO_OK;
}

int mesh_event_timing_from_tlvs(struct mesh_event_timing *timing,
                                const uint8_t *payload,
                                size_t payload_len,
                                bool channel5_contact_refreshed)
{
    return mesh_event_timing_from_tlvs_at(timing,
                                          payload,
                                          payload_len,
                                          0u,
                                          channel5_contact_refreshed);
}

int mesh_init_event_control(struct proto_packet *packet,
                            uint8_t msg_type,
                            uint64_t local_id,
                            uint64_t peer_id,
                            uint32_t session_id,
                            uint16_t seq,
                            uint8_t payload_len)
{
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!event_control_type_valid(msg_type) ||
        !ids_are_valid(local_id, peer_id) ||
        session_id == 0u ||
        seq == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = msg_type;
    packet->flags = 0u;
    packet->src_id = local_id;
    packet->dst_id = peer_id;
    packet->session_id = session_id;
    packet->seq = seq;
    packet->ttl = MESH_DEFAULT_TTL;
    packet->payload_len = payload_len;
    return PROTO_OK;
}

int mesh_append_requested_seq(uint8_t *payload,
                                   size_t payload_cap,
                                   size_t *offset,
                                   uint16_t requested_seq)
{
    return tlv_append_u16(payload, payload_cap, offset, TLV_REQUESTED_MSG_SEQ, requested_seq);
}

int mesh_append_command_id(uint8_t *payload,
                                size_t payload_cap,
                                size_t *offset,
                                enum command_id command_id)
{
    return tlv_append_u16(payload, payload_cap, offset, TLV_COMMAND_ID, (uint16_t)command_id);
}

int mesh_append_command_result(uint8_t *payload,
                                    size_t payload_cap,
                                    size_t *offset,
                                    enum command_id command_id,
                                    enum command_status status,
                                    uint8_t reason)
{
    int ret;

    if (!command_status_valid(status)) {
        return PROTO_ERR_MALFORMED;
    }

    ret = mesh_append_command_id(payload, payload_cap, offset, command_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset, TLV_COMMAND_STATUS, (uint16_t)status);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u8(payload, payload_cap, offset, TLV_REASON, reason);
}

int mesh_init_gateway_ack(struct proto_packet *packet,
                               uint64_t gateway_id,
                               uint64_t original_src_id,
                               uint32_t session_id,
                               uint16_t ack_seq,
                               uint8_t payload_len)
{
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!ids_are_valid(gateway_id, original_src_id) ||
        session_id == 0u ||
        ack_seq == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = MSG_GATEWAY_ACK;
    packet->flags = FLAG_GATEWAY_ACK;
    packet->src_id = gateway_id;
    packet->dst_id = original_src_id;
    packet->session_id = session_id;
    packet->seq = ack_seq;
    packet->ttl = MESH_GATEWAY_ACK_TTL;
    packet->payload_len = payload_len;
    return PROTO_OK;
}

int mesh_init_command(struct proto_packet *packet,
                           uint64_t gateway_id,
                           uint64_t target_id,
                           uint32_t session_id,
                           uint16_t seq,
                           uint8_t payload_len)
{
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!ids_are_valid(gateway_id, target_id) ||
        session_id == 0u ||
        seq == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = MSG_COMMAND;
    packet->flags = 0u;
    packet->src_id = gateway_id;
    packet->dst_id = target_id;
    packet->session_id = session_id;
    packet->seq = seq;
    packet->ttl = MESH_DEFAULT_TTL;
    packet->payload_len = payload_len;
    return PROTO_OK;
}

int mesh_init_command_result(struct proto_packet *packet,
                                  uint64_t target_id,
                                  uint64_t gateway_id,
                                  uint32_t session_id,
                                  uint16_t seq,
                                  uint8_t payload_len,
                                  bool diagnostic)
{
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!ids_are_valid(target_id, gateway_id) ||
        session_id == 0u ||
        seq == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = MSG_COMMAND_RESULT;
    packet->flags = FLAG_GATEWAY_ACK_REQUIRED;
    if (diagnostic) {
        packet->flags |= FLAG_DIAGNOSTIC;
    }
    packet->src_id = target_id;
    packet->dst_id = gateway_id;
    packet->session_id = session_id;
    packet->seq = seq;
    packet->ttl = MESH_DEFAULT_TTL;
    packet->payload_len = payload_len;
    return PROTO_OK;
}
