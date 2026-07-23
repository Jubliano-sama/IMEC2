#include "mesh_sim_internal.h"
#include "mesh_sim_interval.h"

#include <limits.h>
#include <string.h>

static bool radio_transmission_work_current(
    const struct mesh_sim_world *world,
    const struct mesh_sim_transmission *tx)
{
    return tx->valid && tx->node_index < world->role_count &&
           tx->work_epoch == world->roles[tx->node_index].work_epoch;
}

static bool radio_rx_window_work_current(
    const struct mesh_sim_world *world,
    const struct mesh_sim_rx_window *window)
{
    return window->valid && window->node_index < world->role_count &&
           window->work_epoch == world->roles[window->node_index].work_epoch;
}

static bool rx_extension_conflicts(const struct mesh_sim_world *world,
                                   size_t window_index,
                                   uint64_t old_end_us,
                                   uint64_t new_end_us);

enum dwm3000_timing_phy mesh_sim_radio_timing_phy(enum mesh_sim_phy phy)
{
    switch (phy) {
    case MESH_SIM_PHY_CHANNEL5_WAKE:
        return DWM3000_TIMING_PHY_CH5_WAKE;
    case MESH_SIM_PHY_CHANNEL5_RANGE:
        return DWM3000_TIMING_PHY_CH5_RANGE;
    case MESH_SIM_PHY_CHANNEL5_MESH_CONTROL:
        return DWM3000_TIMING_PHY_CH5_MESH_CONTROL;
    case MESH_SIM_PHY_CHANNEL9_MESH:
        return DWM3000_TIMING_PHY_CH9_MESH;
    default:
        return (enum dwm3000_timing_phy)-1;
    }
}

uint32_t mesh_sim_frame_duration_us(enum mesh_sim_phy phy, size_t frame_len)
{
    uint64_t total;

    if (mesh_sim_phy_profile(phy) == NULL || frame_len == 0u) {
        return 0u;
    }
    total = dwm3000_timing_airtime_us_ceil(mesh_sim_radio_timing_phy(phy), frame_len);
    return total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
}

static int radio_operation_conflicts(const struct mesh_sim_world *world,
                                     uint8_t node_index,
                                     uint64_t start_us,
                                     uint64_t end_us)
{
    for (size_t i = 0u; i < world->rx_window_count; i++) {
        const struct mesh_sim_rx_window *window = &world->rx_windows[i];

        if (window->valid && window->node_index == node_index &&
            mesh_sim_interval_overlaps(start_us, end_us,
                                       window->start_us, window->end_us)) {
            return MESH_SIM_ERR_RADIO_CONFLICT;
        }
    }
    for (size_t i = 0u; i < world->transmission_count; i++) {
        const struct mesh_sim_transmission *tx = &world->transmissions[i];

        if (tx->valid && tx->node_index == node_index &&
            mesh_sim_interval_overlaps(start_us, end_us, tx->start_us, tx->end_us)) {
            return MESH_SIM_ERR_RADIO_CONFLICT;
        }
    }
    return MESH_SIM_OK;
}

int mesh_sim_schedule_rx(struct mesh_sim_world *world,
                         uint8_t node_index,
                         uint64_t start_us,
                         uint64_t end_us,
                         uint8_t channel,
                         enum mesh_sim_phy phy,
                         uint16_t *window_index)
{
    struct mesh_sim_rx_window *window;
    uint16_t index;
    int ret;

    if (!mesh_sim_node_index_valid(world, node_index) || end_us <= start_us ||
        start_us < world->now_us || mesh_sim_phy_profile(phy) == NULL) {
        return MESH_SIM_ERR_ARG;
    }
    ret = radio_operation_conflicts(world, node_index, start_us, end_us);
    if (ret != MESH_SIM_OK) {
        return mesh_sim_fail(world, ret);
    }
    ret = mesh_sim_telemetry_reserve_rx_window(world, &index);
    if (ret != MESH_SIM_OK) {
        return mesh_sim_fail(world, ret);
    }
    window = &world->rx_windows[index];
    *window = (struct mesh_sim_rx_window) {
        .start_us = start_us,
        .end_us = end_us,
        .start_rctu = dwm3000_timing_us_to_rctu_floor(start_us),
        .end_rctu = dwm3000_timing_us_to_rctu_floor(end_us),
        .initial_end_rctu = dwm3000_timing_us_to_rctu_floor(end_us),
        .work_epoch = world->roles[node_index].work_epoch,
        .node_index = node_index,
        .channel = channel,
        .phy = phy,
        .connection_event_index = UINT16_MAX,
        .continuous_operation = true,
        .valid = true,
    };
    if (window_index != NULL) {
        *window_index = index;
    }
    ret = mesh_sim_scheduler_schedule(world, SIM_EVENT_RX_START, start_us, index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return mesh_sim_scheduler_schedule(world, SIM_EVENT_RX_END, end_us, index);
}

int mesh_sim_schedule_rx_observe_phy_activity(
    struct mesh_sim_world *world,
    uint8_t node_index,
    uint64_t start_us,
    uint64_t end_us,
    uint8_t channel,
    enum mesh_sim_phy phy,
    uint16_t *window_index)
{
    uint16_t index;
    int ret;

    ret = mesh_sim_schedule_rx(world,
                               node_index,
                               start_us,
                               end_us,
                               channel,
                               phy,
                               &index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    world->rx_windows[index].observe_phy_activity = true;
    if (window_index != NULL) {
        *window_index = index;
    }
    return MESH_SIM_OK;
}

int mesh_sim_schedule_rx_extend_on_activity(struct mesh_sim_world *world,
                                            uint8_t node_index,
                                            uint64_t start_us,
                                            uint64_t acquisition_end_us,
                                            uint32_t completion_us,
                                            uint8_t channel,
                                            enum mesh_sim_phy phy,
                                            uint16_t *window_index)
{
    uint16_t index;
    int ret;

    if (completion_us == 0u) {
        return MESH_SIM_ERR_ARG;
    }
    ret = mesh_sim_schedule_rx(world,
                               node_index,
                               start_us,
                               acquisition_end_us,
                               channel,
                               phy,
                               &index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    world->rx_windows[index].extend_on_activity = true;
    world->rx_windows[index].activity_completion_us = completion_us;
    if (window_index != NULL) {
        *window_index = index;
    }
    return MESH_SIM_OK;
}

int mesh_sim_start_anchor_low_duty(struct mesh_sim_world *world,
                                   uint8_t node_index,
                                   uint64_t work_start_us)
{
    if (!mesh_sim_node_index_valid(world, node_index) ||
        world->roles[node_index].role != MESH_SIM_ROLE_ANCHOR ||
        work_start_us < world->now_us) {
        return MESH_SIM_ERR_ARG;
    }
    return mesh_sim_scheduler_schedule(world,
                          SIM_EVENT_LOW_DUTY_START,
                          work_start_us,
                          node_index);
}

int mesh_sim_runtime_submit(struct mesh_sim_world *world,
                            uint8_t node_index,
                            enum mesh_runtime_work_kind kind,
                            uint64_t token,
                            uint64_t ready_us)
{
    if (!mesh_sim_node_index_valid(world, node_index) ||
        !world->roles[node_index].relay_initialized ||
        ready_us < world->now_us) {
        return MESH_SIM_ERR_ARG;
    }
    return mesh_runtime_submit(&world->roles[node_index].runtime,
                               kind,
                               token,
                               ready_us);
}

int mesh_sim_runtime_reserve_transit(struct mesh_sim_world *world,
                                     uint8_t node_index,
                                     const struct mesh_outbound *outbound,
                                     uint64_t ready_us)
{
    if (!mesh_sim_node_index_valid(world, node_index) || outbound == NULL ||
        !world->roles[node_index].relay_initialized ||
        ready_us < world->now_us) {
        return MESH_SIM_ERR_ARG;
    }
    return mesh_runtime_reserve_transit(&world->roles[node_index].runtime,
                                        outbound,
                                        ready_us);
}

int mesh_sim_runtime_set_action_duration(
    struct mesh_sim_world *world,
    uint8_t node_index,
    enum mesh_runtime_work_kind kind,
    uint32_t duration_us)
{
    if (!mesh_sim_node_index_valid(world, node_index) ||
        kind < MESH_RUNTIME_WORK_GATEWAY_COMMAND ||
        kind > MESH_RUNTIME_WORK_TRANSIT) {
        return MESH_SIM_ERR_ARG;
    }
    world->roles[node_index].runtime_action_duration_us[kind] = duration_us;
    return MESH_SIM_OK;
}

static enum mesh_runtime_radio_owner runtime_action_owner(
    enum mesh_runtime_action_kind action)
{
    switch (action) {
    case MESH_RUNTIME_ACTION_RUN_GATEWAY_COMMAND:
        return MESH_RUNTIME_RADIO_GATEWAY_COMMAND;
    case MESH_RUNTIME_ACTION_START_LOCAL_CLICK:
        return MESH_RUNTIME_RADIO_DS_TWR;
    case MESH_RUNTIME_ACTION_RUN_TRANSIT:
        return MESH_RUNTIME_RADIO_TRANSIT;
    case MESH_RUNTIME_ACTION_REPAIR_SELECTED_EVENT:
        return MESH_RUNTIME_RADIO_LOW_DUTY_SCAN;
    default:
        return MESH_RUNTIME_RADIO_NONE;
    }
}

static enum mesh_runtime_work_kind runtime_action_work_kind(
    enum mesh_runtime_action_kind action)
{
    switch (action) {
    case MESH_RUNTIME_ACTION_RUN_GATEWAY_COMMAND:
        return MESH_RUNTIME_WORK_GATEWAY_COMMAND;
    case MESH_RUNTIME_ACTION_START_LOCAL_CLICK:
        return MESH_RUNTIME_WORK_LOCAL_CLICK;
    case MESH_RUNTIME_ACTION_REPAIR_SELECTED_EVENT:
        return MESH_RUNTIME_WORK_EVENT_REPAIR;
    case MESH_RUNTIME_ACTION_RUN_TRANSIT:
    default:
        return MESH_RUNTIME_WORK_TRANSIT;
    }
}

int mesh_sim_radio_process_runtime_boundary(struct mesh_sim_world *world,
                                    uint8_t node_index)
{
    struct mesh_sim_role_instance *node = &world->roles[node_index];
    struct mesh_runtime_action action;
    uint32_t duration_us;
    int ret;

    ret = mesh_runtime_run_boundary(&node->runtime, world->now_us, &action);
    if (ret != MESH_RUNTIME_OK) {
        return mesh_sim_fail(world, ret);
    }
    if (action.kind == MESH_RUNTIME_ACTION_WAIT_SAFE_BOUNDARY) {
        return mesh_sim_scheduler_schedule(world,
                              SIM_EVENT_RUNTIME_BOUNDARY,
                              action.runnable_at_us,
                              node_index);
    }
    if (action.kind == MESH_RUNTIME_ACTION_NONE) {
        return MESH_SIM_OK;
    }
    duration_us = node->runtime_action_duration_us[
        runtime_action_work_kind(action.kind)];
    if (duration_us == 0u) {
        return mesh_sim_scheduler_schedule(world,
                              SIM_EVENT_RUNTIME_BOUNDARY,
                              world->now_us,
                              node_index);
    }
    ret = mesh_runtime_claim_radio(&node->runtime,
                                   runtime_action_owner(action.kind),
                                   world->now_us,
                                   world->now_us + duration_us);
    if (ret != MESH_RUNTIME_OK) {
        return mesh_sim_fail(world, ret);
    }
    return mesh_sim_scheduler_schedule(world,
                          SIM_EVENT_RUNTIME_RADIO_RELEASE,
                          world->now_us + duration_us,
                          node_index);
}

int mesh_sim_radio_process_anchor_low_duty_start(struct mesh_sim_world *world,
                                         uint8_t node_index)
{
    struct mesh_sim_role_instance *node = &world->roles[node_index];
    struct dwm3000_runtime_interval prepare;
    struct dwm3000_runtime_interval rx;
    uint32_t arm_us = dwm3000_runtime_spi_transfer_us(
        DWM3000_RUNTIME_SPI_FAST,
        DWM3000_RUNTIME_RX_ARM_TRANSFER_BYTES);
    uint16_t window_index;
    uint64_t rx_end_us;
    int ret;

    ret = dwm3000_runtime_prepare_phy(&node->dwm3000,
                                      DWM3000_TIMING_PHY_CH5_WAKE,
                                      world->now_us,
                                      &prepare);
    if (ret != DWM3000_RUNTIME_OK) {
        return mesh_sim_fail(world, ret);
    }
    if (prepare.end_us > UINT64_MAX - arm_us -
        MESH_RADIO_ANCHOR_SCAN_RX_US) {
        return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    rx_end_us = prepare.end_us + arm_us + MESH_RADIO_ANCHOR_SCAN_RX_US;
    ret = dwm3000_runtime_arm_rx(&node->dwm3000,
                                 prepare.end_us,
                                 rx_end_us,
                                 &rx);
    if (ret != DWM3000_RUNTIME_OK) {
        return mesh_sim_fail(world, ret);
    }
    ret = mesh_sim_schedule_rx_extend_on_activity(
        world,
        node_index,
        rx.start_us,
        rx.end_us,
        MESH_RADIO_ACTIVITY_COMPLETION_US,
        UWB_CHANNEL_WAKE_CONTACT,
        MESH_SIM_PHY_CHANNEL5_WAKE,
        &window_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    world->rx_windows[window_index].periodic_low_duty = true;
    world->rx_windows[window_index].dwm_runtime_owned = true;
    return mesh_sim_trace_add(world,
                          prepare.end_us,
                          node->id,
                          0u,
                          MESH_SIM_TRANSITION_RADIO_RECONFIGURED,
                          0u,
                          (uint32_t)(prepare.end_us - prepare.start_us));
}

uint16_t mesh_sim_radio_max_propagation_for_tx(
    const struct mesh_sim_world *world,
    uint8_t node_index)
{
    uint16_t maximum = 0u;

    for (size_t i = 0u; i < world->role_count; i++) {
        if (world->reachable[node_index][i] &&
            world->propagation_us[node_index][i] > maximum) {
            maximum = world->propagation_us[node_index][i];
        }
    }
    return maximum;
}

static bool fault_config_active(const struct mesh_sim_fault_config *config)
{
    return config->frame_loss_permyriad != 0u ||
           config->ack_loss_permyriad != 0u ||
           config->duplicate_permyriad != 0u ||
           config->delay_permyriad != 0u;
}

static bool fault_rate_selected(uint32_t sample, uint16_t rate)
{
    return sample % MESH_SIM_FAULT_RATE_SCALE < rate;
}

static uint64_t receiver_bit(uint8_t receiver_index)
{
    return UINT64_C(1) << receiver_index;
}

static void prepare_transmission_faults(struct mesh_sim_world *world,
                                        struct mesh_sim_transmission *tx)
{
    const struct mesh_sim_fault_config *config = &world->fault_config;

    tx->fault_active = fault_config_active(config);
    if (!tx->fault_active) {
        return;
    }
    for (uint8_t receiver_index = 0u;
         receiver_index < world->role_count;
         receiver_index++) {
        uint32_t loss_sample;
        uint32_t ack_loss_sample;
        uint32_t duplicate_sample;
        uint32_t delay_sample;
        uint32_t delay_value_sample;
        uint64_t bit;

        if (receiver_index == tx->node_index ||
            !world->reachable[tx->node_index][receiver_index]) {
            continue;
        }
        bit = receiver_bit(receiver_index);
        loss_sample = mesh_sim_fault_random(world);
        ack_loss_sample = mesh_sim_fault_random(world);
        duplicate_sample = mesh_sim_fault_random(world);
        delay_sample = mesh_sim_fault_random(world);
        delay_value_sample = mesh_sim_fault_random(world);
        world->fault_stats.receiver_decisions++;
        tx->fault_decision_ordinal[receiver_index] =
            world->fault_stats.receiver_decisions;
        if (fault_rate_selected(loss_sample,
                                config->frame_loss_permyriad)) {
            tx->fault_frame_loss_receivers |= bit;
        }
        if (fault_rate_selected(ack_loss_sample,
                                config->ack_loss_permyriad)) {
            tx->fault_ack_loss_receivers |= bit;
        }
        if (fault_rate_selected(duplicate_sample,
                                config->duplicate_permyriad)) {
            tx->fault_duplicate_receivers |= bit;
        }
        if (fault_rate_selected(delay_sample, config->delay_permyriad)) {
            tx->fault_extra_delay_us[receiver_index] =
                delay_value_sample % config->max_extra_delay_us + 1u;
            world->fault_stats.delay_injections++;
        }
    }
}

static uint64_t transmission_arrival_end_us(
    const struct mesh_sim_world *world,
    const struct mesh_sim_transmission *tx,
    uint8_t receiver_index)
{
    uint64_t arrival_end_rctu =
        tx->end_rctu +
        world->propagation_rctu[tx->node_index][receiver_index] +
        dwm3000_timing_us_to_rctu_ceil(
            tx->fault_extra_delay_us[receiver_index]);

    return dwm3000_timing_rctu_to_us_ceil(arrival_end_rctu);
}

static uint64_t first_fault_evaluation_us(
    const struct mesh_sim_world *world,
    const struct mesh_sim_transmission *tx)
{
    uint64_t first_us = UINT64_MAX;

    for (uint8_t receiver_index = 0u;
         receiver_index < world->role_count;
         receiver_index++) {
        uint64_t arrival_end_us;

        if (receiver_index == tx->node_index ||
            !world->reachable[tx->node_index][receiver_index]) {
            continue;
        }
        arrival_end_us = transmission_arrival_end_us(world,
                                                      tx,
                                                      receiver_index);
        if (arrival_end_us < first_us) {
            first_us = arrival_end_us;
        }
    }
    return first_us == UINT64_MAX ? tx->end_us : first_us;
}

int mesh_sim_schedule_raw_tx(struct mesh_sim_world *world,
                             uint8_t node_index,
                             uint64_t start_us,
                             uint8_t channel,
                             enum mesh_sim_phy phy,
                             const uint8_t *frame,
                             size_t frame_len,
                             bool protocol_frame,
                             uint16_t *transmission_index)
{
    struct mesh_sim_transmission *tx;
    uint32_t duration_us;
    uint16_t index;
    int ret;

    if (!mesh_sim_node_index_valid(world, node_index) || frame == NULL || frame_len == 0u ||
        frame_len > PACKET_EXT_MAX_LEN || start_us < world->now_us ||
        mesh_sim_phy_profile(phy) == NULL) {
        return MESH_SIM_ERR_ARG;
    }
    duration_us = mesh_sim_frame_duration_us(phy, frame_len);
    if (duration_us == 0u) {
        return MESH_SIM_ERR_ARG;
    }
    ret = radio_operation_conflicts(world,
                                    node_index,
                                    start_us,
                                    start_us + duration_us);
    if (ret != MESH_SIM_OK) {
        return mesh_sim_fail(world, ret);
    }
    ret = mesh_sim_telemetry_reserve_transmission(world, &index);
    if (ret != MESH_SIM_OK) {
        return mesh_sim_fail(world, ret);
    }
    tx = &world->transmissions[index];
    memset(tx, 0, sizeof(*tx));
    tx->start_us = start_us;
    tx->start_rctu = dwm3000_timing_us_to_rctu_floor(start_us);
    tx->rmarker_rctu = tx->start_rctu +
                       dwm3000_timing_shr_rctu(mesh_sim_radio_timing_phy(phy));
    tx->end_rctu = tx->start_rctu +
                   dwm3000_timing_airtime_rctu(mesh_sim_radio_timing_phy(phy), frame_len);
    tx->end_us = dwm3000_timing_rctu_to_us_ceil(tx->end_rctu);
    tx->work_epoch = world->roles[node_index].work_epoch;
    tx->node_index = node_index;
    tx->channel = channel;
    tx->phy = phy;
    tx->frame_len = (uint16_t)frame_len;
    tx->connection_event_index = UINT16_MAX;
    tx->protocol_frame = protocol_frame;
    tx->valid = true;
    memcpy(tx->frame, frame, frame_len);
    if (protocol_frame) {
        struct proto_packet decoded_packet;
        const uint8_t *decoded_payload;
        size_t decoded_payload_len;

        if (proto_packet_decode(frame,
                                frame_len,
                                &decoded_packet,
                                &decoded_payload,
                                &decoded_payload_len) == PROTO_OK) {
            tx->protocol_msg_type = decoded_packet.msg_type;
        }
    }
    prepare_transmission_faults(world, tx);
    if (transmission_index != NULL) {
        *transmission_index = index;
    }
    ret = mesh_sim_scheduler_schedule(world, SIM_EVENT_TX_START, tx->start_us, index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    ret = mesh_sim_scheduler_schedule(world, SIM_EVENT_TX_END, tx->end_us, index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return mesh_sim_scheduler_schedule(
        world,
        SIM_EVENT_TX_EVALUATE,
        tx->fault_active ?
            first_fault_evaluation_us(world, tx) :
            tx->end_us +
                mesh_sim_radio_max_propagation_for_tx(world, node_index),
        index);
}

int mesh_sim_schedule_packet_tx(struct mesh_sim_world *world,
                                uint8_t node_index,
                                uint64_t start_us,
                                uint8_t channel,
                                enum mesh_sim_phy phy,
                                const struct proto_packet *packet,
                                const uint8_t *payload,
                                size_t payload_len,
                                uint16_t *transmission_index)
{
    uint8_t frame[PACKET_EXT_MAX_LEN];
    size_t frame_len = 0u;
    uint16_t index;
    int ret;

    if (packet == NULL || packet->payload_len != payload_len ||
        (payload_len > 0u && payload == NULL)) {
        return MESH_SIM_ERR_ARG;
    }
    ret = proto_packet_encode(packet,
                              payload,
                              frame,
                              sizeof(frame),
                              &frame_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_sim_schedule_raw_tx(world,
                                   node_index,
                                   start_us,
                                   channel,
                                   phy,
                                   frame,
                                   frame_len,
                                   true,
                                   &index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    world->transmissions[index].protocol_msg_type = packet->msg_type;
    if (transmission_index != NULL) {
        *transmission_index = index;
    }
    return mesh_sim_connection_process_local_control_packet(
        world, node_index, mesh_sim_time_ms(start_us), packet, payload,
        payload_len);
}

int mesh_sim_outbound_radio(const struct mesh_outbound *outbound,
                            uint8_t *channel,
                            enum mesh_sim_phy *phy)
{
    if (outbound == NULL || channel == NULL || phy == NULL) {
        return MESH_SIM_ERR_ARG;
    }
    if (outbound->radio_channel == UWB_CHANNEL_WAKE_CONTACT) {
        *channel = UWB_CHANNEL_WAKE_CONTACT;
        *phy = MESH_SIM_PHY_CHANNEL5_MESH_CONTROL;
        return MESH_SIM_OK;
    }
    if (outbound->radio_channel == UWB_CHANNEL_MESH_PAYLOAD) {
        *channel = UWB_CHANNEL_MESH_PAYLOAD;
        *phy = MESH_SIM_PHY_CHANNEL9_MESH;
        return MESH_SIM_OK;
    }
    return MESH_SIM_ERR_ARG;
}

static int schedule_outbound_tx_on_radio(
    struct mesh_sim_world *world,
    uint8_t node_index,
    uint64_t start_us,
    const struct mesh_outbound *outbound,
    uint8_t channel,
    enum mesh_sim_phy phy,
    uint16_t connection_event_index,
    uint16_t *scheduled_transmission_index)
{
    uint16_t transmission_index;
    int ret;

    ret = mesh_sim_schedule_packet_tx(world,
                                      node_index,
                                      start_us,
                                      channel,
                                      phy,
                                      &outbound->packet,
                                      outbound->payload,
                                      outbound->payload_len,
                                      &transmission_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    world->transmissions[transmission_index].has_outbound = true;
    world->transmissions[transmission_index].outbound = *outbound;
    world->transmissions[transmission_index].connection_event_index =
        connection_event_index;
    if (scheduled_transmission_index != NULL) {
        *scheduled_transmission_index = transmission_index;
    }
    return MESH_SIM_OK;
}

int mesh_sim_radio_schedule_channel9_runtime_rx(
    struct mesh_sim_world *world,
    uint8_t node_index,
    uint64_t event_end_us,
    uint64_t latest_ready_us,
    uint16_t connection_event_index)
{
    struct mesh_sim_role_instance *node = &world->roles[node_index];
    struct dwm3000_runtime_interval prepare;
    struct dwm3000_runtime_interval rx;
    uint16_t window_index;
    int ret;

    ret = dwm3000_runtime_prepare_phy(&node->dwm3000,
                                      DWM3000_TIMING_PHY_CH9_MESH,
                                      world->now_us,
                                      &prepare);
    if (ret != DWM3000_RUNTIME_OK) {
        return mesh_sim_fail(world, ret);
    }
    ret = dwm3000_runtime_arm_rx(&node->dwm3000,
                                 prepare.end_us,
                                 event_end_us,
                                 &rx);
    if (ret != DWM3000_RUNTIME_OK) {
        return mesh_sim_fail(world, ret);
    }
    if (rx.start_us > latest_ready_us) {
        return mesh_sim_fail(world, MESH_SIM_ERR_RADIO_DEADLINE);
    }
    ret = mesh_sim_schedule_rx(world,
                               node_index,
                               rx.start_us,
                               event_end_us,
                               UWB_CHANNEL_MESH_PAYLOAD,
                               MESH_SIM_PHY_CHANNEL9_MESH,
                               &window_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    world->rx_windows[window_index].dwm_runtime_owned = true;
    world->rx_windows[window_index].connection_event_index =
        connection_event_index;
    return mesh_sim_trace_add(world,
                          prepare.end_us,
                          node->id,
                          0u,
                          MESH_SIM_TRANSITION_RADIO_RECONFIGURED,
                          0u,
                          prepare.end_us - prepare.start_us > UINT32_MAX ?
                              UINT32_MAX :
                              (uint32_t)(prepare.end_us - prepare.start_us));
}

int mesh_sim_direct_gateway_arm_rx(struct mesh_sim_world *world,
                                   uint8_t gateway_index,
                                   uint64_t rx_ready_by_us,
                                   uint64_t rx_end_us)
{
    if (world == NULL || !mesh_sim_node_index_valid(world, gateway_index) ||
        world->roles[gateway_index].role != MESH_SIM_ROLE_GATEWAY ||
        rx_ready_by_us < world->now_us || rx_end_us <= rx_ready_by_us) {
        return MESH_SIM_ERR_ARG;
    }
    return mesh_sim_radio_schedule_channel9_runtime_rx(
        world, gateway_index, rx_end_us, rx_ready_by_us, UINT16_MAX);
}

int mesh_sim_radio_schedule_channel9_runtime_tx(
    struct mesh_sim_world *world,
    uint8_t node_index,
    uint64_t desired_air_start_us,
    uint64_t event_end_us,
    const struct mesh_outbound *outbound,
    uint16_t connection_event_index)
{
    struct mesh_sim_role_instance *node = &world->roles[node_index];
    struct dwm3000_runtime_interval prepare;
    struct dwm3000_runtime_interval write;
    struct dwm3000_runtime_interval tx;
    uint32_t command_us;
    uint32_t duration_us;
    uint64_t start_command_us;
    size_t frame_len;
    uint16_t transmission_index;
    int ret;

    frame_len = proto_packet_encoded_len(outbound->payload_len);
    duration_us = mesh_sim_frame_duration_us(MESH_SIM_PHY_CHANNEL9_MESH,
                                             frame_len);
    command_us = dwm3000_runtime_spi_transfer_us(
        DWM3000_RUNTIME_SPI_FAST,
        DWM3000_RUNTIME_TX_START_TRANSFER_BYTES);
    if (frame_len == 0u || duration_us == 0u || command_us == 0u ||
        desired_air_start_us < command_us ||
        desired_air_start_us > UINT64_MAX - duration_us ||
        desired_air_start_us + duration_us > event_end_us) {
        return mesh_sim_fail(world, MESH_SIM_ERR_FRAME_TOO_LONG);
    }

    ret = dwm3000_runtime_prepare_phy(&node->dwm3000,
                                      DWM3000_TIMING_PHY_CH9_MESH,
                                      world->now_us,
                                      &prepare);
    if (ret != DWM3000_RUNTIME_OK) {
        return mesh_sim_fail(world, ret);
    }
    ret = dwm3000_runtime_write_frame(&node->dwm3000,
                                      frame_len,
                                      prepare.end_us,
                                      &write);
    if (ret != DWM3000_RUNTIME_OK) {
        return mesh_sim_fail(world, ret);
    }
    start_command_us = desired_air_start_us - command_us;
    if (write.end_us > start_command_us) {
        return mesh_sim_fail(world, MESH_SIM_ERR_RADIO_DEADLINE);
    }
    ret = dwm3000_runtime_start_tx(&node->dwm3000,
                                   start_command_us,
                                   desired_air_start_us + duration_us,
                                   &tx);
    if (ret != DWM3000_RUNTIME_OK) {
        return mesh_sim_fail(world, ret);
    }
    if (tx.start_us != desired_air_start_us) {
        return mesh_sim_fail(world, MESH_SIM_ERR_RADIO_DEADLINE);
    }
    ret = schedule_outbound_tx_on_radio(world,
                                        node_index,
                                        tx.start_us,
                                        outbound,
                                        UWB_CHANNEL_MESH_PAYLOAD,
                                        MESH_SIM_PHY_CHANNEL9_MESH,
                                        connection_event_index,
                                        &transmission_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    world->transmissions[transmission_index].dwm_runtime_owned = true;
    return mesh_sim_trace_add(world,
                          prepare.end_us,
                          node->id,
                          outbound->next_hop_id,
                          MESH_SIM_TRANSITION_RADIO_RECONFIGURED,
                          outbound->packet.msg_type,
                          prepare.end_us - prepare.start_us > UINT32_MAX ?
                              UINT32_MAX :
                              (uint32_t)(prepare.end_us - prepare.start_us));
}

int mesh_sim_schedule_outbound_tx(struct mesh_sim_world *world,
                                  uint8_t node_index,
                                  uint64_t start_us,
                                  const struct mesh_outbound *outbound,
                                  uint16_t *transmission_index)
{
    enum mesh_sim_phy phy;
    uint8_t channel;
    int ret;

    ret = mesh_sim_outbound_radio(outbound, &channel, &phy);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return schedule_outbound_tx_on_radio(world,
                                         node_index,
                                         start_us,
                                         outbound,
                                         channel,
                                         phy,
                                         UINT16_MAX,
                                         transmission_index);
}

static bool transmission_collides_at(const struct mesh_sim_world *world,
                                     size_t tx_index,
                                     uint8_t receiver_index,
                                     uint64_t arrival_start,
                                     uint64_t arrival_end,
                                     const struct mesh_sim_rx_window *window)
{
    const struct mesh_sim_transmission *target = &world->transmissions[tx_index];

    for (size_t i = 0u; i < world->transmission_count; i++) {
        const struct mesh_sim_transmission *other = &world->transmissions[i];
        uint64_t other_start;
        uint64_t other_end;

        if (i == tx_index || !other->valid ||
            other->channel != target->channel ||
            !world->reachable[other->node_index][receiver_index]) {
            continue;
        }
        other_start = other->start_rctu +
                      world->propagation_rctu[other->node_index][receiver_index] +
                      dwm3000_timing_us_to_rctu_ceil(
                          other->fault_extra_delay_us[receiver_index]);
        other_end = other->end_rctu +
                    world->propagation_rctu[other->node_index][receiver_index] +
                    dwm3000_timing_us_to_rctu_ceil(
                        other->fault_extra_delay_us[receiver_index]);
        if (mesh_sim_interval_overlaps(arrival_start, arrival_end,
                                       other_start, other_end) &&
            mesh_sim_interval_overlaps(window->start_rctu, window->end_rctu,
                                       other_start, other_end)) {
            return true;
        }
    }
    return false;
}

static bool protocol_msg_is_ack(uint8_t msg_type)
{
    return msg_type == MSG_MESH_HOP_ACK ||
           msg_type == MSG_GATEWAY_ACK ||
           msg_type == MSG_ROUTE_REPLY_ACK ||
           msg_type == MSG_GATEWAY_COLLECTION_EACK;
}

static int trace_fault_decision(struct mesh_sim_world *world,
                                const struct mesh_sim_transmission *tx,
                                uint8_t receiver_index,
                                enum mesh_sim_transition_kind kind,
                                uint32_t detail)
{
    int ret = mesh_sim_trace_add(world,
                                 world->now_us,
                                 world->roles[receiver_index].id,
                                 world->roles[tx->node_index].id,
                                 kind,
                                 tx->protocol_msg_type,
                                 detail);

    if (ret == MESH_SIM_OK && world->transition_count > 0u) {
        world->transitions[world->transition_count - 1u]
            .fault_decision_ordinal =
                tx->fault_decision_ordinal[receiver_index];
    }
    return ret;
}

static enum mesh_sim_rx_outcome partial_outcome(
    enum mesh_sim_phy phy,
    uint64_t arrival_start_rctu,
    uint64_t preamble_end_rctu,
    uint64_t sfd_end_rctu,
    const struct mesh_sim_rx_window *window)
{
    uint64_t overlap_start = window->start_rctu > arrival_start_rctu ?
                             window->start_rctu : arrival_start_rctu;
    uint64_t overlap_end = window->end_rctu < preamble_end_rctu ?
                           window->end_rctu : preamble_end_rctu;
    uint64_t preamble_overlap = overlap_end > overlap_start ?
                                overlap_end - overlap_start : 0u;
    uint64_t detect_min = dwm3000_timing_pac_rctu(mesh_sim_radio_timing_phy(phy));

    if (preamble_overlap < detect_min) {
        return MESH_SIM_RX_PREAMBLE_ONLY;
    }
    if (window->end_rctu < sfd_end_rctu) {
        return MESH_SIM_RX_SFD_TIMEOUT;
    }
    return MESH_SIM_RX_FRAME_TIMEOUT;
}

enum mesh_sim_radio_state mesh_sim_radio_post_operation_state(
    const struct mesh_sim_role_instance *node,
    uint64_t now_us)
{
    if (node != NULL && node->runtime.radio_owner != MESH_RUNTIME_RADIO_NONE &&
        now_us < node->runtime.radio_busy_until_us) {
        return MESH_SIM_RADIO_IDLE;
    }
    return MESH_SIM_RADIO_SLEEP;
}

static int hold_channel5_for_wake_claim(
    struct mesh_sim_world *world,
    uint8_t receiver_index,
    struct mesh_sim_rx_window *window,
    const struct uwb_wake_claim_frame *claim,
    mesh_sim_radio_runtime_claim_handler runtime_claim_handler,
    void *runtime_claim_context)
{
    struct mesh_sim_role_instance *receiver = &world->roles[receiver_index];
    uint64_t wake_hold_us;
    uint64_t ownership_us;
    uint64_t wake_hold_end_us;
    uint64_t ownership_end_us;
    size_t window_index;
    int ret;

    if (window == NULL || claim == NULL) {
        return MESH_SIM_ERR_ARG;
    }
    wake_hold_us = (uint64_t)claim->wake_train_ends_in_ms * 1000u;
    ownership_us = (uint64_t)claim->claimed_duration_ms * 1000u;
    if (world->now_us > UINT64_MAX - wake_hold_us ||
        world->now_us > UINT64_MAX - ownership_us) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    wake_hold_end_us = world->now_us + wake_hold_us;
    ownership_end_us = world->now_us + ownership_us;
    if (ownership_end_us < wake_hold_end_us) {
        ownership_end_us = wake_hold_end_us;
    }
    if (ownership_end_us <= world->now_us) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }

    if (receiver->runtime.radio_owner == MESH_RUNTIME_RADIO_DS_TWR) {
        if (ownership_end_us > receiver->runtime.radio_busy_until_us) {
            receiver->runtime.radio_busy_until_us = ownership_end_us;
            ret = mesh_sim_scheduler_schedule(world,
                                 SIM_EVENT_RUNTIME_RADIO_RELEASE,
                                 ownership_end_us,
                                 receiver_index);
            if (ret != MESH_SIM_OK) {
                return ret;
            }
        }
    } else {
        ret = runtime_claim_handler(runtime_claim_context,
                                    receiver_index,
                                    MESH_RUNTIME_RADIO_DS_TWR,
                                    world->now_us,
                                    ownership_end_us);
        if (ret != MESH_SIM_OK) {
            return mesh_sim_fail(world, ret);
        }
    }

    window_index = (size_t)(window - world->rx_windows);
    if (wake_hold_end_us > window->end_us) {
        if (rx_extension_conflicts(world,
                                   window_index,
                                   window->end_us,
                                   wake_hold_end_us)) {
            return mesh_sim_fail(world, MESH_SIM_ERR_RADIO_CONFLICT);
        }
        if (window->periodic_low_duty) {
            ret = dwm3000_runtime_extend_rx(&receiver->dwm3000,
                                            wake_hold_end_us);
            if (ret != DWM3000_RUNTIME_OK) {
                return mesh_sim_fail(world, ret);
            }
        }
        window->end_us = wake_hold_end_us;
        window->end_rctu = dwm3000_timing_us_to_rctu_ceil(wake_hold_end_us);
        ret = mesh_sim_scheduler_schedule(world,
                             SIM_EVENT_RX_END,
                             wake_hold_end_us,
                             (uint16_t)window_index);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    window->wake_claim_handoff = true;
    receiver->resume_low_duty_after_ds_twr = window->periodic_low_duty;
    return mesh_sim_trace_add(world,
                          world->now_us,
                          receiver->id,
                          claim->clicker_id,
                          MESH_SIM_TRANSITION_WAKE_CLAIM_OWNED,
                          claim->attempt_index,
                          claim->claimed_duration_ms);
}

static int append_reception(struct mesh_sim_world *world,
                            size_t tx_index,
                            uint8_t receiver_index,
                             struct mesh_sim_rx_window *window,
                             enum mesh_sim_rx_outcome outcome,
                             uint64_t arrival_start,
                             uint64_t arrival_end,
                             mesh_sim_radio_packet_handler packet_handler,
                             mesh_sim_radio_runtime_claim_handler runtime_claim_handler,
                             void *runtime_claim_context)
{
    struct mesh_sim_transmission *tx = &world->transmissions[tx_index];
    struct mesh_sim_role_instance *receiver = &world->roles[receiver_index];
    struct mesh_sim_reception *reception;
    const uint8_t *decoded_payload = NULL;
    size_t decoded_payload_len = 0u;
    uint16_t reception_index;
    enum mesh_sim_transition_kind transition_kind;
    int ret = PROTO_OK;

    ret = mesh_sim_telemetry_reserve_reception(world, &reception_index);
    if (ret != MESH_SIM_OK) {
        return mesh_sim_fail(world, ret);
    }
    reception = &world->receptions[reception_index];
    memset(reception, 0, sizeof(*reception));
    reception->start_rctu = arrival_start;
    reception->end_rctu = arrival_end;
    reception->start_us = dwm3000_timing_rctu_to_us_floor(arrival_start);
    reception->end_us = dwm3000_timing_rctu_to_us_ceil(arrival_end);
    reception->source_id = world->roles[tx->node_index].id;
    reception->receiver_id = receiver->id;
    reception->outcome = outcome;
    reception->phy = tx->phy;
    reception->channel = tx->channel;
    reception->protocol_status = PROTO_ERR_BAD_LENGTH;

    if (outcome == MESH_SIM_RX_DECODED && tx->protocol_frame) {
        ret = proto_packet_decode(tx->frame,
                                  tx->frame_len,
                                  &reception->packet,
                                  &decoded_payload,
                                  &decoded_payload_len);
        reception->protocol_status = ret;
        if (ret != PROTO_OK || decoded_payload_len > sizeof(reception->payload)) {
            reception->outcome = MESH_SIM_RX_DECODE_ERROR;
            outcome = MESH_SIM_RX_DECODE_ERROR;
        } else {
            reception->payload_len = (uint16_t)decoded_payload_len;
            if (decoded_payload_len > 0u) {
                memcpy(reception->payload, decoded_payload, decoded_payload_len);
            }
        }
    } else if (outcome == MESH_SIM_RX_DECODED) {
        reception->protocol_status = PROTO_OK;
        if (receiver->anchor_initialized &&
            tx->phy == MESH_SIM_PHY_CHANNEL5_WAKE &&
            (tx->protocol_msg_type == 0u ||
             tx->protocol_msg_type == MSG_UWB_WAKE_CLAIM)) {
            struct uwb_wake_claim_frame claim;
            enum uwb_anchor_claim_decision decision;

            ret = uwb_decode_wake_claim(tx->frame, tx->frame_len, &claim);
            if (ret == PROTO_OK) {
                ret = uwb_anchor_accept_wake_claim(&receiver->anchor_session,
                                                   &claim,
                                                   mesh_sim_time_ms(world->now_us),
                                                   &decision);
                if (ret == PROTO_OK &&
                    (decision == UWB_ANCHOR_CLAIM_ACCEPTED ||
                     decision == UWB_ANCHOR_CLAIM_REPLACED_BY_PRIORITY)) {
                    ret = hold_channel5_for_wake_claim(world,
                                                       receiver_index,
                                                       window,
                                                       &claim,
                                                       runtime_claim_handler,
                                                       runtime_claim_context);
                }
            }
            if (ret != PROTO_OK) {
                reception->protocol_status = ret;
                reception->outcome = MESH_SIM_RX_DECODE_ERROR;
                outcome = MESH_SIM_RX_DECODE_ERROR;
            }
        }
    }

    if (outcome == MESH_SIM_RX_DECODED) {
        if (window->dwm_runtime_owned && tx->frame_len > 0u) {
            window->decoded_frame_len = tx->frame_len;
        }
        receiver->decoded_frames++;
        transition_kind = MESH_SIM_TRANSITION_RX_DECODED;
    } else if (outcome == MESH_SIM_RX_COLLISION) {
        receiver->collision_frames++;
        transition_kind = MESH_SIM_TRANSITION_RX_COLLISION;
        if (receiver->anchor_initialized &&
            tx->phy == MESH_SIM_PHY_CHANNEL5_WAKE) {
            uwb_anchor_note_wake_decode_failure(&receiver->anchor_session,
                                                UWB_WAKE_DECODE_CRC_FAILURE);
        }
    } else {
        receiver->partial_frames++;
        transition_kind = MESH_SIM_TRANSITION_RX_PARTIAL;
        if (receiver->anchor_initialized &&
            tx->phy == MESH_SIM_PHY_CHANNEL5_WAKE) {
            enum uwb_wake_decode_failure failure = UWB_WAKE_DECODE_FRAME_TIMEOUT;

            if (outcome == MESH_SIM_RX_PREAMBLE_ONLY) {
                failure = UWB_WAKE_DECODE_PREAMBLE_ONLY;
            } else if (outcome == MESH_SIM_RX_SFD_TIMEOUT) {
                failure = UWB_WAKE_DECODE_SFD_TIMEOUT;
            }
            uwb_anchor_note_wake_decode_failure(&receiver->anchor_session, failure);
        }
    }

    ret = mesh_sim_trace_add(world,
                         world->now_us,
                         receiver->id,
                         world->roles[tx->node_index].id,
                         transition_kind,
                         reception->packet.msg_type,
                         (uint32_t)outcome);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    if (outcome == MESH_SIM_RX_DECODED && tx->protocol_frame) {
        if (tx->connection_event_index != UINT16_MAX &&
            tx->connection_event_index < world->connection_event_count &&
            world->connection_events[tx->connection_event_index].receiver_index ==
            receiver_index) {
            world->connection_events[tx->connection_event_index].decoded = true;
        }
        return packet_handler(world,
                              receiver_index,
                              tx->node_index,
                              &reception->packet,
                              reception->payload,
                              reception->payload_len);
    }
    (void)window;
    return MESH_SIM_OK;
}

int mesh_sim_radio_evaluate_transmission(
    struct mesh_sim_world *world,
    size_t tx_index,
    mesh_sim_radio_packet_handler packet_handler,
    mesh_sim_radio_runtime_claim_handler runtime_claim_handler,
    void *runtime_claim_context)
{
    struct mesh_sim_transmission *tx = &world->transmissions[tx_index];
    enum dwm3000_timing_phy production_phy = mesh_sim_radio_timing_phy(tx->phy);
    uint64_t next_evaluation_us = UINT64_MAX;

    if (!radio_transmission_work_current(world, tx)) {
        return MESH_SIM_OK;
    }

    for (uint8_t receiver_index = 0u;
         receiver_index < world->role_count;
         receiver_index++) {
        uint64_t propagation;
        uint64_t arrival_start;
        uint64_t arrival_end;
        uint64_t preamble_end;
        uint64_t sfd_end;
        uint64_t bit;

        if (receiver_index == tx->node_index ||
            !world->reachable[tx->node_index][receiver_index]) {
            continue;
        }
        bit = receiver_bit(receiver_index);
        if (tx->fault_active &&
            (tx->fault_evaluated_receivers & bit) != 0u) {
            continue;
        }
        propagation = world->propagation_rctu[tx->node_index][receiver_index];
        arrival_start = tx->start_rctu + propagation +
                        dwm3000_timing_us_to_rctu_ceil(
                            tx->fault_extra_delay_us[receiver_index]);
        arrival_end = tx->end_rctu + propagation +
                      dwm3000_timing_us_to_rctu_ceil(
                          tx->fault_extra_delay_us[receiver_index]);
        if (tx->fault_active) {
            uint64_t arrival_end_us =
                dwm3000_timing_rctu_to_us_ceil(arrival_end);

            if (arrival_end_us > world->now_us) {
                if (arrival_end_us < next_evaluation_us) {
                    next_evaluation_us = arrival_end_us;
                }
                continue;
            }
            tx->fault_evaluated_receivers |= bit;
            if (tx->fault_extra_delay_us[receiver_index] != 0u) {
                int ret = trace_fault_decision(
                    world,
                    tx,
                    receiver_index,
                    MESH_SIM_TRANSITION_FAULT_DELAYED,
                    tx->fault_extra_delay_us[receiver_index]);

                if (ret != MESH_SIM_OK) {
                    return ret;
                }
            }
        }
        preamble_end = arrival_start +
                       dwm3000_timing_preamble_rctu(production_phy);
        sfd_end = preamble_end + dwm3000_timing_sfd_rctu(production_phy);

        for (size_t i = 0u; i < world->rx_window_count; i++) {
            struct mesh_sim_rx_window *window = &world->rx_windows[i];
            uint64_t preamble_overlap_start;
            uint64_t preamble_overlap_end;
            enum mesh_sim_rx_outcome outcome;
            bool complete;
            bool phy_match;
            int ret;

            if (!radio_rx_window_work_current(world, window) ||
                window->node_index != receiver_index ||
                window->channel != tx->channel) {
                continue;
            }
            phy_match = window->phy == tx->phy;
            if (!phy_match && !window->observe_phy_activity) {
                continue;
            }
            preamble_overlap_start = window->start_rctu > arrival_start ?
                                     window->start_rctu : arrival_start;
            preamble_overlap_end = window->end_rctu < preamble_end ?
                                   window->end_rctu : preamble_end;
            if (preamble_overlap_end <= preamble_overlap_start) {
                continue;
            }
            if (preamble_overlap_end - preamble_overlap_start >=
                dwm3000_timing_pac_rctu(production_phy)) {
                window->preamble_detected = true;
            }
            complete = window->start_rctu <= arrival_start &&
                       window->end_rctu >= arrival_end;
            if (transmission_collides_at(world,
                                         tx_index,
                                         receiver_index,
                                         arrival_start,
                                         arrival_end,
                                         window)) {
                outcome = MESH_SIM_RX_COLLISION;
            } else if (complete) {
                /* A same-channel frame with the wrong PHR/PHY is visible as
                 * RF activity, but the DW3000 cannot decode it. */
                outcome = phy_match ? MESH_SIM_RX_DECODED :
                                      MESH_SIM_RX_DECODE_ERROR;
            } else {
                outcome = partial_outcome(tx->phy,
                                          arrival_start,
                                          preamble_end,
                                          sfd_end,
                                          window);
            }
            if (outcome == MESH_SIM_RX_DECODED &&
                world->directed_rx_failures[tx->node_index][receiver_index] > 0u) {
                world->directed_rx_failures[tx->node_index][receiver_index]--;
                outcome = world->directed_rx_failure_outcome[
                    tx->node_index][receiver_index];
            }
            if (outcome == MESH_SIM_RX_DECODED && tx->fault_active) {
                enum mesh_sim_transition_kind drop_kind;

                if (tx->protocol_frame &&
                    protocol_msg_is_ack(tx->protocol_msg_type) &&
                    (tx->fault_ack_loss_receivers & bit) != 0u) {
                    drop_kind = MESH_SIM_TRANSITION_FAULT_ACK_DROPPED;
                    world->fault_stats.ack_losses++;
                    outcome = MESH_SIM_RX_DECODE_ERROR;
                } else if ((tx->fault_frame_loss_receivers & bit) != 0u) {
                    drop_kind = MESH_SIM_TRANSITION_FAULT_FRAME_DROPPED;
                    world->fault_stats.frame_losses++;
                    outcome = MESH_SIM_RX_DECODE_ERROR;
                } else {
                    drop_kind = MESH_SIM_TRANSITION_COUNT;
                }
                if (drop_kind != MESH_SIM_TRANSITION_COUNT) {
                    ret = trace_fault_decision(world,
                                               tx,
                                               receiver_index,
                                               drop_kind,
                                               0u);
                    if (ret != MESH_SIM_OK) {
                        return ret;
                    }
                }
            }
            ret = append_reception(world,
                                   tx_index,
                                   receiver_index,
                                    window,
                                   outcome,
                                   arrival_start,
                                   arrival_end,
                                   packet_handler,
                                   runtime_claim_handler,
                                   runtime_claim_context);
            if (ret != MESH_SIM_OK) {
                return ret;
            }
            if (outcome == MESH_SIM_RX_DECODED && tx->protocol_frame &&
                tx->fault_active &&
                (tx->fault_duplicate_receivers & bit) != 0u) {
                ret = trace_fault_decision(
                    world,
                    tx,
                    receiver_index,
                    MESH_SIM_TRANSITION_FAULT_DUPLICATED,
                    1u);
                if (ret != MESH_SIM_OK) {
                    return ret;
                }
                world->fault_stats.duplicates++;
                ret = append_reception(world,
                                       tx_index,
                                       receiver_index,
                                       window,
                                       outcome,
                                       arrival_start,
                                       arrival_end,
                                       packet_handler,
                                       runtime_claim_handler,
                                       runtime_claim_context);
                if (ret != MESH_SIM_OK) {
                    return ret;
                }
            }
            break;
        }
    }
    if (next_evaluation_us != UINT64_MAX) {
        return mesh_sim_scheduler_schedule(world,
                                           SIM_EVENT_TX_EVALUATE,
                                           next_evaluation_us,
                                           (uint16_t)tx_index);
    }
    return MESH_SIM_OK;
}

static bool rx_extension_conflicts(const struct mesh_sim_world *world,
                                   size_t window_index,
                                   uint64_t old_end_us,
                                   uint64_t new_end_us)
{
    const struct mesh_sim_rx_window *target = &world->rx_windows[window_index];

    for (size_t i = 0u; i < world->rx_window_count; i++) {
        const struct mesh_sim_rx_window *other = &world->rx_windows[i];

        if (i != window_index && other->valid &&
            other->node_index == target->node_index &&
            mesh_sim_interval_overlaps(old_end_us,
                                       new_end_us,
                                       other->start_us,
                                       other->end_us)) {
            return true;
        }
    }
    for (size_t i = 0u; i < world->transmission_count; i++) {
        const struct mesh_sim_transmission *other = &world->transmissions[i];

        if (other->valid && other->node_index == target->node_index &&
            mesh_sim_interval_overlaps(old_end_us,
                                       new_end_us,
                                       other->start_us,
                                       other->end_us)) {
            return true;
        }
    }
    return false;
}

int mesh_sim_radio_note_preamble_at_tx_start(struct mesh_sim_world *world,
                                     const struct mesh_sim_transmission *tx)
{
    enum dwm3000_timing_phy production_phy = mesh_sim_radio_timing_phy(tx->phy);
    uint64_t preamble_rctu = dwm3000_timing_preamble_rctu(production_phy);
    uint64_t detect_rctu = dwm3000_timing_pac_rctu(production_phy);

    for (size_t i = 0u; i < world->rx_window_count; i++) {
        struct mesh_sim_rx_window *window = &world->rx_windows[i];
        uint64_t arrival_start;
        uint64_t preamble_end;
        uint64_t detection_time;

        if (!window->valid || window->node_index == tx->node_index ||
            !world->reachable[tx->node_index][window->node_index] ||
            window->channel != tx->channel ||
            (window->phy != tx->phy && !window->observe_phy_activity)) {
            continue;
        }
        arrival_start = tx->start_rctu +
                        world->propagation_rctu[tx->node_index][window->node_index] +
                        dwm3000_timing_us_to_rctu_ceil(
                            tx->fault_extra_delay_us[window->node_index]);
        preamble_end = arrival_start + preamble_rctu;
        if (mesh_sim_interval_overlaps(window->start_rctu,
                                       window->end_rctu,
                                       arrival_start,
                                       preamble_end)) {
            uint64_t overlap_start = window->start_rctu > arrival_start ?
                                     window->start_rctu : arrival_start;

            detection_time = overlap_start + detect_rctu;
            if (detection_time <= window->end_rctu) {
                window->preamble_detected = true;
                if (window->extend_on_activity) {
                    uint64_t extension_rctu = dwm3000_timing_us_to_rctu_ceil(
                        window->activity_completion_us);
                    uint64_t new_end_rctu = detection_time + extension_rctu;

                    if (new_end_rctu > window->end_rctu) {
                        uint64_t old_end_us = window->end_us;
                        uint64_t new_end_us =
                            dwm3000_timing_rctu_to_us_ceil(new_end_rctu);
                        int ret;

                        if (rx_extension_conflicts(world,
                                                   i,
                                                   old_end_us,
                                                   new_end_us)) {
                            return mesh_sim_fail(world,
                                            MESH_SIM_ERR_RADIO_CONFLICT);
                        }
                        if (window->periodic_low_duty) {
                            ret = dwm3000_runtime_extend_rx(
                                &world->roles[window->node_index].dwm3000,
                                new_end_us);
                            if (ret != DWM3000_RUNTIME_OK) {
                                return mesh_sim_fail(world, ret);
                            }
                        }
                        window->end_rctu = new_end_rctu;
                        window->end_us = new_end_us;
                        ret = mesh_sim_scheduler_schedule(world,
                                             SIM_EVENT_RX_END,
                                             new_end_us,
                                             (uint16_t)i);
                        if (ret != MESH_SIM_OK) {
                            return ret;
                        }
                        ret = mesh_sim_trace_add(
                            world,
                            dwm3000_timing_rctu_to_us_ceil(detection_time),
                            world->roles[window->node_index].id,
                            world->roles[tx->node_index].id,
                            MESH_SIM_TRANSITION_RX_WINDOW_EXTENDED,
                            0u,
                            window->activity_completion_us);
                        if (ret != MESH_SIM_OK) {
                            return ret;
                        }
                    }
                }
            }
        }
    }
    return MESH_SIM_OK;
}
