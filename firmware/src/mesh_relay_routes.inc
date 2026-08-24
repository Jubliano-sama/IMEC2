#line 4971 __BASE_FILE__
void mesh_relay_init(struct mesh_relay *relay,
                     enum mesh_relay_role role,
                     uint64_t local_id,
                     uint64_t gateway_id,
                     uint32_t route_epoch)
{
    if (relay == NULL) {
        return;
    }

    memset(relay, 0, sizeof(*relay));
    relay->role = role;
    relay->local_id = local_id;
    relay->gateway_id = gateway_id;
    relay->next_seq = 1u;
    if (route_epoch != 0u && (uint16_t)route_epoch == 0u) {
        route_epoch++;
    }
    route_table_init(&relay->upstream, route_epoch);
}

void mesh_gateway_ack_store_init(struct mesh_gateway_ack_store *store)
{
    if (store != NULL) {
        memset(store, 0, sizeof(*store));
    }
}

int mesh_relay_attach_gateway_ack_store(struct mesh_relay *relay,
                                        struct mesh_gateway_ack_store *store)
{
    if (relay == NULL || store == NULL ||
        relay->role != MESH_RELAY_ROLE_GATEWAY ||
        relay->local_id != relay->gateway_id) {
        return PROTO_ERR_ARG;
    }

    mesh_gateway_ack_store_init(store);
    relay->gateway_ack_store = store;
    return PROTO_OK;
}

void mesh_anchor_downlink_store_init(struct mesh_anchor_downlink_store *store)
{
    if (store != NULL) {
        memset(store, 0, sizeof(*store));
    }
}

int mesh_relay_attach_anchor_downlink_store(
    struct mesh_relay *relay,
    struct mesh_anchor_downlink_store *store)
{
    if (relay == NULL || store == NULL ||
        relay->role != MESH_RELAY_ROLE_ANCHOR ||
        relay->local_id == relay->gateway_id) {
        return PROTO_ERR_ARG;
    }

    mesh_anchor_downlink_store_init(store);
    relay->anchor_downlink_store = store;
    return PROTO_OK;
}

const struct mesh_downlink_entry *mesh_relay_find_downlink(const struct mesh_relay *relay,
                                                           uint64_t target_id)
{
    int index;

    if (relay == NULL || !id_is_unicast(target_id)) {
        return NULL;
    }

    index = downlink_index(relay, target_id);
    return index >= 0 ? mesh_relay_downlink_at(relay, (size_t)index) : NULL;
}

const struct mesh_downlink_entry *mesh_relay_find_current_downlink(
    const struct mesh_relay *relay,
    uint64_t target_id)
{
    const struct mesh_downlink_entry *selected = NULL;

    if (relay == NULL || !id_is_unicast(target_id) ||
        relay->upstream.current_epoch == 0u) {
        return NULL;
    }

    for (size_t i = 0u; i < mesh_relay_downlink_capacity(relay); i++) {
        const struct mesh_downlink_entry *entry =
            mesh_relay_downlink_at(relay, i);

        if (entry == NULL || !entry->valid ||
            entry->target_id != target_id ||
            entry->gateway_id != relay->gateway_id ||
            entry->route_epoch != relay->upstream.current_epoch ||
            !id_is_unicast(entry->next_hop_id) ||
            entry->next_hop_id == relay->local_id ||
            entry->hop_count == 0u ||
            entry->hop_count > MESH_NETWORK_MAX_HOPS ||
            entry->quality > 100u) {
            continue;
        }
        if (downlink_is_better(entry, selected)) {
            selected = entry;
        }
    }
    return selected;
}

int mesh_relay_note_gateway_control_reverse_route(
    struct mesh_relay *relay,
    const struct proto_packet *packet,
    uint64_t previous_hop_id,
    uint8_t link_quality,
    uint8_t origin_ttl,
    uint32_t now_ms)
{
    struct route_candidate candidate;
    struct route_candidate *stored_candidate = NULL;
    uint32_t preserved_hold_down_until_ms = 0u;
    uint8_t gateway_hop_count;
    uint8_t preserved_failure_count = 0u;
    bool preserve_failure_state = false;
    bool preserved_hold_down_valid = false;
    bool destination_valid;
    int ret;

    if (relay == NULL || packet == NULL ||
        relay->role != MESH_RELAY_ROLE_ANCHOR ||
        !id_is_unicast(relay->local_id) ||
        !id_is_unicast(relay->gateway_id) ||
        relay->local_id == relay->gateway_id ||
        packet->src_id != relay->gateway_id ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id ||
        relay->upstream.current_epoch == 0u ||
        link_quality > 100u ||
        origin_ttl == 0u || origin_ttl > MESH_NETWORK_MAX_HOPS ||
        packet->ttl == 0u || packet->ttl > origin_ttl) {
        return PROTO_ERR_ARG;
    }

    destination_valid = packet->msg_type == MSG_COMMAND &&
        (packet->dst_id == relay->local_id ||
         packet->dst_id == MESH_BROADCAST_ID);
    if (!destination_valid) {
        return PROTO_ERR_MALFORMED;
    }

    gateway_hop_count = origin_ttl - packet->ttl;
    if ((gateway_hop_count == 0u) !=
        (previous_hop_id == relay->gateway_id)) {
        return PROTO_ERR_MALFORMED;
    }

    candidate = (struct route_candidate) {
        .next_hop_id = previous_hop_id,
        .gateway_id = relay->gateway_id,
        .route_epoch = relay->upstream.current_epoch,
        .last_seen_ms = now_ms,
        .hop_count = gateway_hop_count,
        .link_quality = link_quality,
        .relay_capacity_state = RELAY_CAP_UNKNOWN,
        .valid = true,
    };
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        struct route_candidate *existing = &relay->upstream.candidates[i];

        if (existing->valid &&
            existing->route_epoch == relay->upstream.current_epoch &&
            existing->next_hop_id == previous_hop_id &&
            existing->gateway_id == relay->gateway_id) {
            preserved_failure_count = existing->failure_count;
            preserved_hold_down_until_ms = existing->hold_down_until_ms;
            preserved_hold_down_valid = existing->hold_down_valid;
            preserve_failure_state = true;
            break;
        }
    }
    ret = route_upsert_candidate(&relay->upstream, &candidate);
    if (ret == PROTO_OK) {
        if (preserve_failure_state) {
            for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
                struct route_candidate *existing =
                    &relay->upstream.candidates[i];

                if (existing->valid &&
                    existing->route_epoch == relay->upstream.current_epoch &&
                    existing->next_hop_id == previous_hop_id &&
                    existing->gateway_id == relay->gateway_id) {
                    stored_candidate = existing;
                    break;
                }
            }
            if (stored_candidate == NULL) {
                return PROTO_ERR_NOT_FOUND;
            }
            stored_candidate->failure_count = preserved_failure_count;
            stored_candidate->hold_down_until_ms =
                preserved_hold_down_until_ms;
            stored_candidate->hold_down_valid = preserved_hold_down_valid;
            ret = route_select_best_at(&relay->upstream, now_ms);
            if (ret != PROTO_OK && preserved_hold_down_valid) {
                /* A valid passive refresh may leave every known parent held.
                 * Keep the route evidence and let discovery run; absence of a
                 * currently selectable parent is not a malformed control. */
                ret = PROTO_OK;
            }
        }
        if (previous_hop_id == relay->gateway_id) {
            const struct route_candidate *stored = find_upstream_candidate(
                relay, previous_hop_id, relay->upstream.current_epoch);
            const struct mesh_route_path direct_path = {
                .node_ids = {relay->gateway_id, relay->local_id},
                .count = 2u,
            };

            ret = store_upstream_candidate_ancestry(relay,
                                                    stored,
                                                    &direct_path,
                                                    NULL);
            if (ret != PROTO_OK) {
                return ret;
            }
        }
        mesh_relay_note_route_discovery_ready(relay, relay->gateway_id);
    }
    return ret;
}

int mesh_relay_note_flood_parent_candidate(
    struct mesh_relay *relay,
    const struct proto_packet *packet,
    uint64_t previous_hop_id,
    uint8_t link_quality,
    uint8_t origin_ttl,
    uint32_t route_epoch,
    uint32_t now_ms)
{
    struct route_candidate candidate;
    struct route_candidate *stored_candidate = NULL;
    uint32_t preserved_hold_down_until_ms = 0u;
    uint8_t gateway_hop_count;
    uint8_t preserved_failure_count = 0u;
    bool preserve_failure_state = false;
    bool preserved_hold_down_valid = false;
    int ret;

    if (relay == NULL || packet == NULL ||
        relay->role != MESH_RELAY_ROLE_ANCHOR ||
        !id_is_unicast(relay->local_id) ||
        !id_is_unicast(relay->gateway_id) ||
        relay->local_id == relay->gateway_id ||
        packet->src_id != relay->gateway_id ||
        packet->msg_type != MSG_COMMAND ||
        (packet->dst_id != MESH_BROADCAST_ID &&
         packet->dst_id != relay->local_id) ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id ||
        route_epoch == 0u ||
        link_quality > 100u ||
        origin_ttl == 0u || origin_ttl > MESH_NETWORK_MAX_HOPS ||
        packet->ttl == 0u || packet->ttl > origin_ttl) {
        return PROTO_ERR_ARG;
    }

    gateway_hop_count = origin_ttl - packet->ttl;

    candidate = (struct route_candidate) {
        .next_hop_id = previous_hop_id,
        .gateway_id = relay->gateway_id,
        .route_epoch = route_epoch,
        .last_seen_ms = now_ms,
        .hop_count = gateway_hop_count,
        .link_quality = link_quality,
        .relay_capacity_state = RELAY_CAP_UNKNOWN,
        .valid = true,
    };
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        struct route_candidate *existing = &relay->upstream.candidates[i];

        if (existing->valid &&
            existing->route_epoch == route_epoch &&
            existing->next_hop_id == previous_hop_id &&
            existing->gateway_id == relay->gateway_id) {
            preserved_failure_count = existing->failure_count;
            preserved_hold_down_until_ms = existing->hold_down_until_ms;
            preserved_hold_down_valid = existing->hold_down_valid;
            preserve_failure_state = true;
            break;
        }
    }
    ret = route_upsert_candidate(&relay->upstream, &candidate);
    if (ret == PROTO_OK && preserve_failure_state) {
        for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
            struct route_candidate *existing =
                &relay->upstream.candidates[i];

            if (existing->valid &&
                existing->route_epoch == route_epoch &&
                existing->next_hop_id == previous_hop_id &&
                existing->gateway_id == relay->gateway_id) {
                stored_candidate = existing;
                break;
            }
        }
        if (stored_candidate == NULL) {
            return PROTO_ERR_NOT_FOUND;
        }
        stored_candidate->failure_count = preserved_failure_count;
        stored_candidate->hold_down_until_ms =
            preserved_hold_down_until_ms;
        stored_candidate->hold_down_valid = preserved_hold_down_valid;
        ret = route_select_best_at(&relay->upstream, now_ms);
        if (ret != PROTO_OK && preserved_hold_down_valid) {
            /* A valid passive refresh may leave every known parent held.
             * Keep the route evidence and let discovery run; absence of a
             * currently selectable parent is not a malformed control. */
            ret = PROTO_OK;
        }
    }
    if (ret == PROTO_OK && previous_hop_id == relay->gateway_id) {
        const struct route_candidate *stored = find_upstream_candidate(
            relay, previous_hop_id, relay->upstream.current_epoch);
        const struct mesh_route_path direct_path = {
            .node_ids = {relay->gateway_id, relay->local_id},
            .count = 2u,
        };

        ret = store_upstream_candidate_ancestry(relay,
                                                stored,
                                                &direct_path,
                                                NULL);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    if (ret == PROTO_OK) {
        mesh_relay_note_route_discovery_ready(relay, relay->gateway_id);
    }
    return ret;
}

void mesh_relay_remove_direct_gateway_route(struct mesh_relay *relay)
{
    bool removed = false;

    if (relay == NULL || relay->role == MESH_RELAY_ROLE_GATEWAY ||
        !id_is_unicast(relay->gateway_id)) {
        return;
    }

    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        struct route_candidate *candidate = &relay->upstream.candidates[i];

        if (candidate->valid &&
            candidate->route_epoch == relay->upstream.current_epoch &&
            candidate->gateway_id == relay->gateway_id &&
            candidate->next_hop_id == relay->gateway_id &&
            candidate->hop_count == 0u) {
            clear_upstream_candidate_ancestry_at(relay, i);
            memset(candidate, 0, sizeof(*candidate));
            removed = true;
        }
    }
    if (!removed) {
        return;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        struct mesh_relay_event_timing_entry *entry = &relay->event_timings[i];

        if (entry->valid && entry->next_hop_id == relay->gateway_id) {
            memset(entry, 0, sizeof(*entry));
        }
    }
    relay->upstream.selected_index = ROUTE_NO_SELECTION;
    (void)route_select_best(&relay->upstream);
}

uint8_t mesh_relay_expire_routes(struct mesh_relay *relay, uint32_t now_ms)
{
    uint8_t expired;

    if (relay == NULL) {
        return 0u;
    }

    expired = route_expire_stale(&relay->upstream,
                                 now_ms,
                                 ROUTE_CANDIDATE_MAX_AGE_MS);
    if (expired > 0u) {
        for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
            if (!relay->upstream.candidates[i].valid) {
                clear_upstream_candidate_ancestry_at(relay, i);
            }
        }
    }
    return expired;
}

int mesh_relay_select_next_hop(const struct mesh_relay *relay,
                               uint64_t dst_id,
                               uint64_t *next_hop_id)
{
    const struct route_candidate *upstream;
    const struct mesh_downlink_entry *downlink;

    if (relay == NULL || next_hop_id == NULL || !id_is_unicast(dst_id)) {
        return PROTO_ERR_ARG;
    }
    if (dst_id == relay->local_id) {
        return PROTO_ERR_MALFORMED;
    }
    if (relay->role == MESH_RELAY_ROLE_CLICKER &&
        dst_id != relay->gateway_id) {
        return PROTO_ERR_NOT_FOUND;
    }

    if (dst_id == relay->gateway_id) {
        if (relay->role == MESH_RELAY_ROLE_GATEWAY) {
            return PROTO_ERR_MALFORMED;
        }
        upstream = route_selected(&relay->upstream);
        if (upstream == NULL) {
            return PROTO_ERR_NOT_FOUND;
        }
        *next_hop_id = upstream->next_hop_id;
        return PROTO_OK;
    }

    downlink = mesh_relay_find_downlink(relay, dst_id);
    if (downlink == NULL) {
        return PROTO_ERR_NOT_FOUND;
    }
    *next_hop_id = downlink->next_hop_id;
    return PROTO_OK;
}


void mesh_relay_reset_route_discovery(struct mesh_relay *relay)
{
    if (relay != NULL) {
        memset(&relay->route_discovery, 0, sizeof(relay->route_discovery));
    }
}

static bool downlink_depends_on_epoch(const struct mesh_relay *relay,
                                      uint64_t next_hop_id,
                                      uint32_t route_epoch)
{
    for (size_t i = 0u; i < mesh_relay_downlink_capacity(relay); i++) {
        const struct mesh_downlink_entry *entry =
            mesh_relay_downlink_at(relay, i);

        if (entry->valid &&
            entry->route_epoch == route_epoch &&
            entry->next_hop_id == next_hop_id) {
            return true;
        }
    }
    return false;
}

static void invalidate_failed_upstream_dependents(struct mesh_relay *relay,
                                                   uint64_t failed_parent_id,
                                                   uint32_t route_epoch)
{
    if (relay == NULL || failed_parent_id == 0u ||
        !id_is_unicast(failed_parent_id)) {
        return;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        struct mesh_relay_event_timing_entry *entry = &relay->event_timings[i];
        enum mesh_relay_channel9_direction direction;

        if (!entry->valid) {
            continue;
        }
        direction = (enum mesh_relay_channel9_direction)entry->direction;
        if (entry->next_hop_id == failed_parent_id ||
            (direction == MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM &&
             downlink_depends_on_epoch(relay,
                                       entry->next_hop_id,
                                       route_epoch))) {
            memset(entry, 0, sizeof(*entry));
        }
    }
    for (size_t i = 0u; i < mesh_relay_downlink_capacity(relay); i++) {
        struct mesh_downlink_entry *entry = downlink_at_mutable(relay, i);

        /*
         * A failed upstream parent invalidates only reverse routes that use
         * that same peer.  Other child routes remain the accepted return path
         * for gateway ACKs already in flight; their Channel-9 timing is
         * cleared above and can be repaired independently.
         */
        if (entry->valid && entry->route_epoch == route_epoch &&
            entry->next_hop_id == failed_parent_id) {
            memset(entry, 0, sizeof(*entry));
        }
    }
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        struct route_candidate *candidate = &relay->upstream.candidates[i];

        if (candidate->valid &&
            candidate->route_epoch == route_epoch &&
            candidate->next_hop_id == failed_parent_id) {
            candidate->channel9_timing_valid = false;
        }
    }
    mesh_relay_reset_route_discovery(relay);
}

int mesh_relay_abandon_upstream_parent_at(
    struct mesh_relay *relay,
    uint64_t expected_parent_id,
    uint32_t now_ms,
    enum route_delivery_action *action_out)
{
    const struct route_candidate *failed;
    enum route_delivery_action action;
    uint32_t route_epoch;

    if (relay == NULL || action_out == NULL ||
        !id_is_unicast(expected_parent_id)) {
        return PROTO_ERR_ARG;
    }
    route_epoch = relay->upstream.current_epoch;
    failed = find_upstream_candidate(relay,
                                     expected_parent_id,
                                     route_epoch);
    if (failed == NULL) {
        return PROTO_ERR_STALE;
    }
    action = route_abandon_candidate_at(&relay->upstream,
                                        expected_parent_id,
                                        relay->gateway_id,
                                        route_epoch,
                                        now_ms);
    invalidate_failed_upstream_dependents(relay,
                                          expected_parent_id,
                                          route_epoch);
    *action_out = action;
    return PROTO_OK;
}

void mesh_relay_invalidate_upstream_route(struct mesh_relay *relay)
{
    if (relay == NULL) {
        return;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        struct mesh_relay_event_timing_entry *entry = &relay->event_timings[i];
        bool upstream_peer = false;

        if (!entry->valid) {
            continue;
        }
        for (uint8_t j = 0u; j < ROUTE_MAX_CANDIDATES; j++) {
            const struct route_candidate *candidate = &relay->upstream.candidates[j];

            if (candidate->valid &&
                candidate->route_epoch == relay->upstream.current_epoch &&
                candidate->next_hop_id == entry->next_hop_id) {
                upstream_peer = true;
                break;
            }
        }
        if (upstream_peer) {
            memset(entry, 0, sizeof(*entry));
        }
    }
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        struct route_candidate *candidate = &relay->upstream.candidates[i];

        if (candidate->valid &&
            candidate->route_epoch == relay->upstream.current_epoch) {
            candidate->channel9_timing_valid = false;
        }
    }
    relay->upstream.selected_index = ROUTE_NO_SELECTION;
    mesh_relay_reset_route_discovery(relay);
}

void mesh_relay_invalidate_active_route_path(struct mesh_relay *relay)
{
    if (relay == NULL) {
        return;
    }

    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        struct route_candidate *candidate = &relay->upstream.candidates[i];

        if (candidate->valid &&
            candidate->route_epoch == relay->upstream.current_epoch) {
            candidate->channel9_timing_valid = false;
        }
    }
    relay->upstream.selected_index = ROUTE_NO_SELECTION;
    clear_all_downlinks(relay);
    memset(relay->event_timings, 0, sizeof(relay->event_timings));
    mesh_relay_reset_route_discovery(relay);
}

void mesh_relay_note_route_discovery_ready(struct mesh_relay *relay,
                                           uint64_t target_id)
{
    if (relay == NULL || !id_is_unicast(target_id)) {
        return;
    }
    if (relay->route_discovery.active &&
        relay->route_discovery.target_id == target_id) {
        mesh_relay_reset_route_discovery(relay);
    }
}

static void mesh_relay_clear_routes_at_epoch(struct mesh_relay *relay,
                                             uint32_t epoch)
{
    route_table_init(&relay->upstream, epoch);
    clear_all_upstream_candidate_ancestry(relay);
    clear_all_downlinks(relay);
    memset(relay->event_timings, 0, sizeof(relay->event_timings));
    mesh_relay_reset_route_discovery(relay);
}

void mesh_relay_clear_routes_preserve_epoch(struct mesh_relay *relay)
{
    if (relay == NULL) {
        return;
    }

    mesh_relay_clear_routes_at_epoch(relay, relay->upstream.current_epoch);
}

void mesh_relay_invalidate_routes(struct mesh_relay *relay)
{
    uint32_t next_epoch;

    if (relay == NULL) {
        return;
    }

    next_epoch = relay->upstream.current_epoch + 1u;
    if ((uint16_t)next_epoch == 0u) {
        next_epoch++;
    }
    mesh_relay_clear_routes_at_epoch(relay, next_epoch);
    result_offer_reservation_clear(relay);
}

static int mesh_relay_require_channel9_event_for_slot(const struct mesh_relay *relay,
                                                      uint64_t next_hop_id,
                                                      const struct mesh_channel5_requirements *requirements,
                                                      uint32_t now_ms,
                                                      bool require_local_tx,
                                                      struct mesh_event_plan *plan)
{
    struct mesh_event_timing tx_timing;
    const struct mesh_event_timing *timing;
    int index;
    int ret;

    if (relay == NULL || requirements == NULL || plan == NULL ||
        !id_is_unicast(next_hop_id)) {
        return PROTO_ERR_ARG;
    }

    index = relay_channel9_timing_index(relay, next_hop_id);
    if (index < 0) {
        return PROTO_ERR_STALE;
    }

    timing = &relay->event_timings[index].timing;
    if (require_local_tx && !mesh_event_timing_local_tx_slot(timing)) {
        /*
         * The retained cadence alternates ownership. A transmitter whose
         * next event belongs to the peer must wait for the immediately
         * following local-TX event, not report an unscheduled BUSY and fall
         * into a slower outer retry cadence that can remain phase-locked to
         * peer-RX slots forever. Keep the shared timing immutable; the RX
         * owner or the normal elapsed-event service will advance it before
         * the returned TX plan becomes due.
         */
        tx_timing = *timing;
        tx_timing.next_event_time_ms += tx_timing.event_interval_ms;
        tx_timing.event_counter++;
        timing = &tx_timing;
    }

    ret = mesh_event_plan_channel9(timing,
                                   requirements,
                                   now_ms,
                                   plan);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (plan->action == MESH_EVENT_PLAN_START ||
        plan->action == MESH_EVENT_PLAN_CLIP) {
        if (require_local_tx &&
            !mesh_event_timing_local_tx_slot(timing)) {
            return PROTO_ERR_BUSY;
        }
        return PROTO_OK;
    }
    if (plan->action == MESH_EVENT_PLAN_REFRESH_CONTACT_CH5) {
        return PROTO_ERR_STALE;
    }
    return PROTO_ERR_BUSY;
}

int mesh_relay_require_channel9_event(const struct mesh_relay *relay,
                                      uint64_t next_hop_id,
                                      const struct mesh_channel5_requirements *requirements,
                                      uint32_t now_ms,
                                      struct mesh_event_plan *plan)
{
    return mesh_relay_require_channel9_event_for_slot(relay,
                                                      next_hop_id,
                                                      requirements,
                                                      now_ms,
                                                      false,
                                                      plan);
}

int mesh_relay_require_channel9_tx_event(const struct mesh_relay *relay,
                                         uint64_t next_hop_id,
                                         const struct mesh_channel5_requirements *requirements,
                                         uint32_t now_ms,
                                         struct mesh_event_plan *plan)
{
    return mesh_relay_require_channel9_event_for_slot(relay,
                                                      next_hop_id,
                                                      requirements,
                                                      now_ms,
                                                      true,
                                                      plan);
}

uint8_t mesh_relay_expire_channel9_timings(struct mesh_relay *relay,
                                           uint32_t now_ms)
{
    uint8_t expired = 0u;

    if (relay == NULL) {
        return 0u;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        struct mesh_relay_event_timing_entry *entry = &relay->event_timings[i];

        if (entry->valid && !mesh_event_timing_usable(&entry->timing, now_ms)) {
            route_set_channel9_timing_valid(&relay->upstream,
                                            entry->next_hop_id,
                                            relay->gateway_id,
                                            false,
                                            now_ms);
            entry->valid = false;
            expired++;
        }
    }

    return expired;
}

static int build_route_request_with_timing_flags_id(
    struct mesh_relay *relay,
    uint64_t target_id,
    const struct mesh_event_timing *proposed_channel9_timing,
    uint32_t timing_reference_ms,
    uint8_t request_flags,
    uint16_t route_reply_rx_delay_ms,
    struct mesh_outbound *out,
    uint32_t now_ms,
    uint32_t request_id)
{
    struct route_discovery_fields fields = {0};
    uint32_t route_epoch;
    uint32_t session_id;
    size_t payload_len = 0u;
    int ret;

    if (relay == NULL || out == NULL ||
        !id_is_unicast(relay->local_id) ||
        !id_is_unicast(target_id) ||
        target_id == relay->local_id ||
        (relay->role == MESH_RELAY_ROLE_CLICKER &&
         target_id != relay->gateway_id) ||
        (request_flags & ~MESH_ROUTE_REQ_ALLOWED_FLAGS) != 0u ||
        MESH_ROUTE_REQ_REQUIRED_HOPS_DECODE(request_flags) >
            MESH_NETWORK_MAX_HOPS) {
        return PROTO_ERR_ARG;
    }

    (void)mesh_relay_expire_routes(relay, now_ms);
    memset(out, 0, sizeof(*out));

    route_epoch = relay->upstream.current_epoch;
    session_id = request_id;
    if (session_id == 0u) {
        session_id = now_ms != 0u ? now_ms : route_epoch;
    }
    if (session_id == 0u) {
        session_id = 1u;
    }

    fields.origin_id = relay->local_id;
    fields.target_id = target_id;
    fields.path.node_ids[0] = relay->local_id;
    fields.path.count = 1u;
    fields.route_epoch = route_epoch;
    fields.flood_epoch_id = session_id;
    fields.slot_seed = route_discovery_slot_seed(relay->local_id,
                                                 target_id,
                                                 session_id,
                                                 route_epoch);
    fields.flood_profile_version = MESH_ROUTE_DISCOVERY_FLOOD_PROFILE_VERSION;
    fields.hop_count = 0u;
    fields.quality = 100u;
    fields.relay_capacity_state = relay_current_capacity_state(relay);
    fields.queue_free_hint = relay_current_queue_free_hint(relay);
    fields.channel9_busy_hint = relay_active_channel9_timing_count(relay);
    fields.capacity_validity_interval_ms =
        relay_current_capacity_validity_interval_ms(relay);
    fields.request_flags = request_flags;
    fields.route_reply_rx_delay_ms = route_reply_rx_delay_ms;
    if (proposed_channel9_timing != NULL) {
        fields.proposed_channel9_timing = *proposed_channel9_timing;
        fields.proposed_channel9_timing_valid = true;
        fields.timing_reference_ms = timing_reference_ms;
    } else {
        memset(&fields.proposed_channel9_timing,
               0,
               sizeof(fields.proposed_channel9_timing));
        fields.proposed_channel9_timing_valid = false;
        fields.timing_reference_ms = 0u;
    }

    ret = append_route_discovery_tlvs(out->payload,
                                      sizeof(out->payload),
                                      &payload_len,
                                      &fields);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet.msg_type = MSG_ROUTE_REQ;
    out->packet.flags = 0u;
    out->packet.src_id = relay->local_id;
    out->packet.dst_id = MESH_BROADCAST_ID;
    out->packet.session_id = session_id;
    out->packet.seq = relay_next_seq(relay);
    out->packet.ttl = route_request_ttl_for_attempt(0u, false);
    out->packet.payload_len = (uint16_t)payload_len;
    out->payload_len = (uint16_t)payload_len;
    out->next_hop_id = MESH_BROADCAST_ID;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->queued_at_ms = now_ms;
    out->earliest_tx_ms = now_ms;
    out->queued_at_valid = true;
    out->earliest_tx_valid = true;
    return PROTO_OK;
}

int mesh_relay_build_route_request_with_timing_flags(
    struct mesh_relay *relay,
    uint64_t target_id,
    const struct mesh_event_timing *proposed_channel9_timing,
    uint32_t timing_reference_ms,
    uint8_t request_flags,
    uint16_t route_reply_rx_delay_ms,
    struct mesh_outbound *out,
    uint32_t now_ms)
{
    return build_route_request_with_timing_flags_id(relay,
                                                    target_id,
                                                    proposed_channel9_timing,
                                                    timing_reference_ms,
                                                    request_flags,
                                                    route_reply_rx_delay_ms,
                                                    out,
                                                    now_ms,
                                                    now_ms);
}

int mesh_relay_build_route_request_with_timing(
    struct mesh_relay *relay,
    uint64_t target_id,
    const struct mesh_event_timing *proposed_channel9_timing,
    uint32_t timing_reference_ms,
    struct mesh_outbound *out,
    uint32_t now_ms)
{
    return mesh_relay_build_route_request_with_timing_flags(relay,
                                                            target_id,
                                                            proposed_channel9_timing,
                                                            timing_reference_ms,
                                                            0u,
                                                            0u,
                                                            out,
                                                            now_ms);
}

int mesh_relay_build_route_request(struct mesh_relay *relay,
                                   uint64_t target_id,
                                   struct mesh_outbound *out,
                                   uint32_t now_ms)
{
    return mesh_relay_build_route_request_with_timing(relay,
                                                     target_id,
                                                     NULL,
                                                     0u,
                                                     out,
                                                     now_ms);
}

int mesh_relay_capture_gateway_route_adv_snapshot_with_policy(
    struct mesh_relay *relay,
    uint32_t gateway_route_seq,
    uint32_t now_ms,
    const struct operation_policy_set *operation_policy,
    struct mesh_gateway_route_adv_snapshot *snapshot)
{
    struct mesh_gateway_route_adv_snapshot captured = {0};
    uint8_t operation_policy_len = 0u;
    int ret;

    if (relay == NULL || snapshot == NULL ||
        relay->role != MESH_RELAY_ROLE_GATEWAY ||
        relay->local_id != relay->gateway_id ||
        !id_is_unicast(relay->local_id) ||
        gateway_route_seq == 0u) {
        return PROTO_ERR_ARG;
    }

    if (operation_policy != NULL) {
        ret = mesh_relay_operation_policy_encode_complete(
            operation_policy,
            captured.operation_policy_tlvs,
            &operation_policy_len);
        if (ret != PROTO_OK) {
            return ret;
        }
        captured.operation_policy_tlvs_len = operation_policy_len;
        captured.operation_policy_present = true;
    }
    captured.gateway_route_seq = gateway_route_seq;
    captured.queued_at_ms = now_ms;
    captured.gateway_epoch = (uint16_t)relay->upstream.current_epoch;
    captured.packet_seq = relay_next_seq(relay);
    captured.gateway_capacity_state = relay_current_capacity_state(relay);
    captured.capacity_validity_interval_ms =
        relay_current_capacity_validity_interval_ms(relay);
    captured.valid = true;
    *snapshot = captured;
    return PROTO_OK;
}

int mesh_relay_capture_gateway_route_adv_snapshot(
    struct mesh_relay *relay,
    uint32_t gateway_route_seq,
    uint32_t now_ms,
    struct mesh_gateway_route_adv_snapshot *snapshot)
{
    return mesh_relay_capture_gateway_route_adv_snapshot_with_policy(
        relay, gateway_route_seq, now_ms, NULL, snapshot);
}

int mesh_relay_build_gateway_route_adv_from_snapshot(
    const struct mesh_relay *relay,
    const struct mesh_gateway_route_adv_snapshot *snapshot,
    struct mesh_outbound *out)
{
    struct gateway_route_adv_fields fields;
    size_t payload_len = 0u;
    bool operation_policy_present;
    int ret;

    if (relay == NULL || snapshot == NULL || out == NULL ||
        relay->role != MESH_RELAY_ROLE_GATEWAY ||
        relay->local_id != relay->gateway_id ||
        !id_is_unicast(relay->local_id) ||
        !snapshot->valid || snapshot->gateway_route_seq == 0u ||
        snapshot->gateway_capacity_state > RELAY_CAP_BLACK) {
        return PROTO_ERR_ARG;
    }
    if (snapshot->operation_policy_tlvs_len >
        sizeof(snapshot->operation_policy_tlvs)) {
        return PROTO_ERR_MALFORMED;
    }

    memset(&fields, 0, sizeof(fields));
    ret = mesh_relay_operation_policy_parse_exact(
        snapshot->operation_policy_tlvs,
        snapshot->operation_policy_tlvs_len,
        &fields.operation_policy,
        fields.operation_policy_tlvs,
        &fields.operation_policy_tlvs_len,
        &operation_policy_present);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (operation_policy_present != snapshot->operation_policy_present) {
        return PROTO_ERR_MALFORMED;
    }
    fields.operation_policy_present = operation_policy_present;

    memset(out, 0, sizeof(*out));
    fields.gateway_id = relay->gateway_id;
    fields.path.node_ids[0] = relay->gateway_id;
    fields.path.count = 1u;
    fields.gateway_epoch = snapshot->gateway_epoch;
    fields.gateway_route_seq = snapshot->gateway_route_seq;
    fields.hop_count = 0u;
    fields.path_quality_min = 100u;
    fields.route_cost = gateway_route_cost(fields.hop_count, fields.path_quality_min);
    fields.gateway_capacity_state = snapshot->gateway_capacity_state;
    fields.capacity_validity_interval_ms =
        snapshot->capacity_validity_interval_ms;
    fields.flood_profile_version = MESH_ROUTE_DISCOVERY_FLOOD_PROFILE_VERSION;
    fields.flood_epoch_id = snapshot->gateway_route_seq;
    fields.slot_seed = gateway_route_adv_slot_seed(fields.gateway_id,
                                                   fields.gateway_route_seq,
                                                   fields.gateway_epoch);
    fields.random_backoff_max_ms =
        MESH_GATEWAY_ROUTE_ADV_FORWARD_JITTER_MAX_MS;
    fields.random_backoff_slot_ms =
        MESH_GATEWAY_ROUTE_ADV_FORWARD_JITTER_SLOT_MS;
    fields.flood_retry_count = FLOOD_DEFAULT_RETRY_COUNT;

    ret = append_gateway_route_adv_tlvs(out->payload,
                                        sizeof(out->payload),
                                        &payload_len,
                                        &fields);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet.msg_type = MSG_GATEWAY_ROUTE_ADV;
    out->packet.flags = 0u;
    out->packet.src_id = relay->gateway_id;
    out->packet.dst_id = MESH_BROADCAST_ID;
    out->packet.session_id = snapshot->gateway_route_seq;
    out->packet.seq = snapshot->packet_seq;
    out->packet.ttl = FLOOD_EPOCH_GLOBAL_TTL;
    out->packet.payload_len = (uint16_t)payload_len;
    out->payload_len = (uint16_t)payload_len;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->next_hop_id = MESH_BROADCAST_ID;
    out->queued_at_ms = snapshot->queued_at_ms;
    out->earliest_tx_ms = snapshot->queued_at_ms;
    out->flood_retry_count = fields.flood_retry_count;
    out->queued_at_valid = true;
    out->earliest_tx_valid = true;
    return PROTO_OK;
}

int mesh_relay_build_gateway_route_adv_with_policy(
    struct mesh_relay *relay,
    uint32_t gateway_route_seq,
    uint32_t now_ms,
    const struct operation_policy_set *operation_policy,
    struct mesh_outbound *out)
{
    struct mesh_gateway_route_adv_snapshot snapshot;
    int ret = mesh_relay_capture_gateway_route_adv_snapshot_with_policy(
        relay, gateway_route_seq, now_ms, operation_policy, &snapshot);

    if (ret != PROTO_OK) {
        return ret;
    }
    return mesh_relay_build_gateway_route_adv_from_snapshot(relay,
                                                            &snapshot,
                                                            out);
}

int mesh_relay_build_gateway_route_adv(struct mesh_relay *relay,
                                       uint32_t gateway_route_seq,
                                       uint32_t now_ms,
                                       struct mesh_outbound *out)
{
    return mesh_relay_build_gateway_route_adv_with_policy(
        relay, gateway_route_seq, now_ms, NULL, out);
}

bool mesh_relay_route_discovery_backoff_pending(
    const struct mesh_relay *relay,
    uint64_t target_id,
    uint32_t now_ms,
    uint32_t *remaining_ms)
{
    if (remaining_ms != NULL) {
        *remaining_ms = 0u;
    }
    if (relay == NULL || !relay->route_discovery.active ||
        relay->route_discovery.target_id != target_id ||
        relay->route_discovery.attempts == 0u ||
        deadline_reached(now_ms,
                         relay->route_discovery.next_request_ms)) {
        return false;
    }
    if (remaining_ms != NULL) {
        *remaining_ms = relay->route_discovery.next_request_ms - now_ms;
    }
    return true;
}

int mesh_relay_prepare_route_request_with_timing_flags(
    struct mesh_relay *relay,
    uint64_t target_id,
    const struct mesh_event_timing *proposed_channel9_timing,
    uint32_t timing_reference_ms,
    uint8_t request_flags,
    uint16_t route_reply_rx_delay_ms,
    uint32_t now_ms,
    uint32_t random_value,
    struct mesh_outbound *out)
{
    uint32_t delay_ms;
    int ret;

    if (relay == NULL || out == NULL ||
        !id_is_unicast(target_id) ||
        target_id == relay->local_id ||
        (relay->role == MESH_RELAY_ROLE_CLICKER &&
         target_id != relay->gateway_id) ||
        (request_flags & ~MESH_ROUTE_REQ_ALLOWED_FLAGS) != 0u ||
        MESH_ROUTE_REQ_REQUIRED_HOPS_DECODE(request_flags) >
            MESH_NETWORK_MAX_HOPS) {
        return PROTO_ERR_ARG;
    }

    if (!relay->route_discovery.active ||
        relay->route_discovery.target_id != target_id ||
        relay->route_discovery.required_hop_count !=
            MESH_ROUTE_REQ_REQUIRED_HOPS_DECODE(request_flags)) {
        memset(&relay->route_discovery, 0, sizeof(relay->route_discovery));
        relay->route_discovery.active = true;
        relay->route_discovery.target_id = target_id;
        relay->route_discovery.required_hop_count =
            MESH_ROUTE_REQ_REQUIRED_HOPS_DECODE(request_flags);
        relay->route_discovery.next_request_id = now_ms != 0u ? now_ms :
                                                 relay->upstream.current_epoch;
        if (relay->route_discovery.next_request_id == 0u) {
            relay->route_discovery.next_request_id = 1u;
        }
    }

    if (mesh_relay_route_discovery_backoff_pending(relay,
                                                   target_id,
                                                   now_ms,
                                                   NULL)) {
        return PROTO_ERR_BUSY;
    }

    ret = build_route_request_with_timing_flags_id(
        relay,
        target_id,
        proposed_channel9_timing,
        timing_reference_ms,
        request_flags,
        route_reply_rx_delay_ms,
        out,
        now_ms,
        relay->route_discovery.next_request_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    relay->route_discovery.current_request_id = out->packet.session_id;
    out->packet.ttl = route_request_ttl_for_attempt(relay->route_discovery.attempts, false);

    if (relay->route_discovery.attempts < UINT8_MAX) {
        relay->route_discovery.attempts++;
    }
    relay->route_discovery.next_request_id++;
    if (relay->route_discovery.next_request_id == 0u) {
        relay->route_discovery.next_request_id = 1u;
    }
    delay_ms = mesh_relay_route_discovery_backoff_ms(relay->route_discovery.attempts,
                                                     random_value);
    relay->route_discovery.next_request_ms = now_ms + delay_ms;
    return PROTO_OK;
}

int mesh_relay_prepare_route_request_with_timing(
    struct mesh_relay *relay,
    uint64_t target_id,
    const struct mesh_event_timing *proposed_channel9_timing,
    uint32_t timing_reference_ms,
    uint32_t now_ms,
    uint32_t random_value,
    struct mesh_outbound *out)
{
    return mesh_relay_prepare_route_request_with_timing_flags(relay,
                                                              target_id,
                                                              proposed_channel9_timing,
                                                              timing_reference_ms,
                                                              0u,
                                                              0u,
                                                              now_ms,
                                                              random_value,
                                                              out);
}

int mesh_relay_prepare_route_request(struct mesh_relay *relay,
                                     uint64_t target_id,
                                     uint32_t now_ms,
                                     uint32_t random_value,
                                     struct mesh_outbound *out)
{
    return mesh_relay_prepare_route_request_with_timing(relay,
                                                       target_id,
                                                       NULL,
                                                       0u,
                                                       now_ms,
                                                       random_value,
                                                       out);
}

int mesh_relay_note_direct_gateway_route(struct mesh_relay *relay,
                                         uint32_t now_ms)
{
    const struct mesh_route_path direct_path = {
        .node_ids = {relay != NULL ? relay->gateway_id : 0u,
                     relay != NULL ? relay->local_id : 0u},
        .count = 2u,
    };
    const struct route_candidate *candidate;
    int ret;

    if (relay == NULL ||
        relay->role == MESH_RELAY_ROLE_GATEWAY ||
        !id_is_unicast(relay->local_id) ||
        !id_is_unicast(relay->gateway_id) ||
        relay->local_id == relay->gateway_id) {
        return PROTO_ERR_ARG;
    }

    ret = upsert_reactive_route(relay,
                                relay->gateway_id,
                                relay->gateway_id,
                                relay->upstream.current_epoch,
                                0u,
                                100u,
                                RELAY_CAP_UNKNOWN,
                                0u,
                                0u,
                                0u,
                                now_ms,
                                NULL,
                                0u,
                                NULL);
    if (ret == PROTO_OK || ret == PROTO_ERR_NOT_FOUND) {
        ret = route_record_candidate_success_at(&relay->upstream,
                                                relay->gateway_id,
                                                relay->gateway_id,
                                                now_ms);
    }
    if (ret == PROTO_OK) {
        candidate = find_upstream_candidate(relay,
                                            relay->gateway_id,
                                            relay->upstream.current_epoch);
        ret = store_upstream_candidate_ancestry(relay,
                                                candidate,
                                                &direct_path,
                                                NULL);
    }
    if (ret == PROTO_OK) {
        mesh_relay_note_route_discovery_ready(relay, relay->gateway_id);
    }
    return ret;
}

int mesh_relay_build_route_reply_for_request(struct mesh_relay *relay,
                                             const struct proto_packet *packet,
                                             const uint8_t *payload,
                                             size_t payload_len,
                                             uint64_t previous_hop_id,
                                             uint32_t now_ms,
                                             uint32_t random_value,
                                             struct mesh_outbound *out)
{
    struct route_discovery_fields fields = {0};
    const struct route_candidate *selected = NULL;
    const struct mesh_route_path *selected_path = NULL;
    struct mesh_route_path direct_selected_path = {0};
    uint32_t reply_earliest_tx_ms;
    uint32_t reply_epoch;
    int ret;

    if (relay == NULL || packet == NULL || out == NULL ||
        (payload_len > 0u && payload == NULL) ||
        packet->msg_type != MSG_ROUTE_REQ) {
        return PROTO_ERR_ARG;
    }

    ret = validate_route_request_packet(relay,
                                        packet,
                                        payload,
                                        payload_len,
                                        previous_hop_id,
                                        now_ms,
                                        &fields);
    if (ret != PROTO_OK) {
        return ret;
    }

    selected = route_selected(&relay->upstream);
    if (selected != NULL) {
        selected_path = upstream_candidate_ancestry(relay, selected);
        if (selected_path == NULL &&
            selected->next_hop_id == relay->gateway_id &&
            selected->gateway_id == relay->gateway_id &&
            selected->hop_count == 0u) {
            direct_selected_path.node_ids[0] = relay->gateway_id;
            direct_selected_path.node_ids[1] = relay->local_id;
            direct_selected_path.count = 2u;
            selected_path = &direct_selected_path;
        }
    }
    if (fields.target_id != relay->local_id &&
        !(fields.target_id == relay->gateway_id && selected != NULL)) {
        return PROTO_ERR_NOT_FOUND;
    }
    if (fields.target_id == relay->gateway_id && selected != NULL &&
        (selected->next_hop_id == previous_hop_id ||
         selected_path == NULL ||
         mesh_route_paths_intersect(selected_path, &fields.path))) {
        return PROTO_ERR_NOT_FOUND;
    }
    if (!route_request_reply_hops_allowed(
            &fields,
            fields.target_id == relay->gateway_id && selected != NULL ?
                (uint8_t)(selected->hop_count + 1u) : 0u)) {
        return PROTO_ERR_NOT_FOUND;
    }

    reply_epoch = fields.route_epoch;
    ret = route_epoch_select_newer(reply_epoch,
                                   relay->upstream.current_epoch,
                                   &reply_epoch);
    if (ret == PROTO_OK && selected != NULL) {
        ret = route_epoch_select_newer(reply_epoch,
                                       selected->route_epoch,
                                       &reply_epoch);
    }
    if (ret != PROTO_OK) {
        return ret;
    }

    reply_earliest_tx_ms = route_reply_earliest_tx_ms(
        now_ms,
        fields.route_reply_rx_delay_ms,
        random_value);
    if (fields.proposed_channel9_timing_valid) {
        /* Keep the compact delay anchored to the scheduled RF reply, not to
         * the earlier request decode that precedes responder-slot jitter. */
        fields.timing_reference_ms = reply_earliest_tx_ms;
    }

    ret = build_route_reply(relay,
                            &fields,
                            fields.target_id == relay->gateway_id ? selected : NULL,
                            fields.target_id == relay->gateway_id ? selected_path : NULL,
                            previous_hop_id,
                            packet->session_id,
                            reply_epoch,
                            out);
    if (ret == PROTO_OK) {
        out->earliest_tx_ms = reply_earliest_tx_ms;
        out->earliest_tx_valid = true;
    }
    return ret;
}

static uint8_t relay_duplicate_count(const struct mesh_relay *relay)
{
    uint8_t count = 0u;

    for (size_t i = 0u; i < MESH_RELAY_DUP_CACHE_SIZE; i++) {
        if (relay->duplicates[i].valid && count < UINT8_MAX) {
            count++;
        }
    }
    return count;
}

static uint8_t relay_parent_hold_down_count(const struct mesh_relay *relay)
{
    uint8_t count = 0u;

    for (size_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        const struct route_candidate *candidate = &relay->upstream.candidates[i];

        if (candidate->valid &&
            candidate->hold_down_valid &&
            count < UINT8_MAX) {
            count++;
        }
    }
    return count;
}

static uint8_t relay_collection_pending_count(const struct mesh_relay *relay)
{
    uint8_t count = relay->result_bundle.active ? relay->result_bundle.record_count : 0u;

    if (relay->outbox_record.valid &&
        relay->outbox_record.delivery_state == MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK &&
        count < UINT8_MAX) {
        count++;
    }
    return count;
}

static int mesh_relay_append_telemetry_tlvs(const struct mesh_relay *relay,
                                            uint8_t *payload,
                                            size_t payload_cap,
                                            size_t *offset)
{
    int ret;

    ret = tlv_append_u8(payload,
                        payload_cap,
                        offset,
                        TLV_MESH_DUPLICATE_COUNT,
                        relay_duplicate_count(relay));
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload,
                        payload_cap,
                        offset,
                        TLV_COLLECTION_PENDING_COUNT,
                        relay_collection_pending_count(relay));
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload,
                        payload_cap,
                        offset,
                        TLV_PARENT_HOLDDOWN_COUNT,
                        relay_parent_hold_down_count(relay));
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload,
                        payload_cap,
                        offset,
                        TLV_ROUTE_DISCOVERY_ATTEMPTS,
                        relay->route_discovery.attempts);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload,
                        payload_cap,
                        offset,
                        TLV_OUTBOX_DELIVERY_STATE,
                        relay->outbox_record.delivery_state);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload,
                        payload_cap,
                        offset,
                        TLV_FLOOD_SUPPRESSION_COUNT,
                        relay->diagnostics.flood_suppression_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload,
                        payload_cap,
                        offset,
                        TLV_ROUTE_REPLY_RETRY_COUNT,
                        relay->diagnostics.route_reply_retry_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u8(payload,
                         payload_cap,
                         offset,
                         TLV_BUSY_RESPONSE_COUNT,
                         relay->diagnostics.busy_response_count);
}

int mesh_relay_append_status_tlvs(const struct mesh_relay *relay,
                                  uint8_t *payload,
                                  size_t payload_cap,
                                  size_t *offset)
{
    const struct route_candidate *selected;
    int ret;

    if (relay == NULL || payload == NULL || offset == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_append_u64(payload, payload_cap, offset, TLV_GATEWAY_ID, relay->gateway_id);
    if (ret != PROTO_OK) {
        return ret;
    }

    if (relay->role == MESH_RELAY_ROLE_GATEWAY) {
        ret = tlv_append_u32(payload,
                             payload_cap,
                             offset,
                             TLV_ROUTE_EPOCH,
                             relay->upstream.current_epoch);
        if (ret != PROTO_OK) {
            return ret;
        }
        ret = tlv_append_u8(payload, payload_cap, offset, TLV_HOP_COUNT, 0u);
        if (ret != PROTO_OK) {
            return ret;
        }
        ret = tlv_append_u8(payload, payload_cap, offset, TLV_QUALITY, 100u);
        if (ret != PROTO_OK) {
            return ret;
        }
        return mesh_relay_append_telemetry_tlvs(relay, payload, payload_cap, offset);
    }

    selected = route_selected(&relay->upstream);
    if (selected == NULL || selected->hop_count == UINT8_MAX) {
        ret = tlv_append_u8(payload,
                            payload_cap,
                            offset,
                            TLV_REASON,
                            (uint8_t)(-PROTO_ERR_NOT_FOUND));
        if (ret != PROTO_OK) {
            return ret;
        }
        return mesh_relay_append_telemetry_tlvs(relay, payload, payload_cap, offset);
    }

    ret = tlv_append_u64(payload, payload_cap, offset, TLV_NEXT_HOP_ID, selected->next_hop_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset, TLV_ROUTE_EPOCH, selected->route_epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset, TLV_HOP_COUNT, selected->hop_count + 1u);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset, TLV_QUALITY, selected->link_quality);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset, TLV_RETRY_COUNT, selected->failure_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    return mesh_relay_append_telemetry_tlvs(relay, payload, payload_cap, offset);
}

bool mesh_relay_tx_active(const struct mesh_relay *relay)
{
    return relay != NULL && relay->pending.state != MESH_RELAY_TX_IDLE;
}

bool mesh_relay_tx_active_local_collection_result(const struct mesh_relay *relay)
{
    return relay != NULL &&
           pending_is_local_collection_result(relay, &relay->pending);
}

bool mesh_relay_result_bundle_pending(const struct mesh_relay *relay)
{
    return relay != NULL &&
           relay->result_bundle.active &&
           relay->result_bundle.record_count > 0u;
}

uint32_t mesh_relay_result_bundle_due_ms(const struct mesh_relay *relay)
{
    if (!mesh_relay_result_bundle_pending(relay)) {
        return UINT32_MAX;
    }
    return relay->result_bundle.due_ms;
}

void mesh_relay_result_bundle_note_forwarded(struct mesh_relay *relay,
                                             const struct mesh_outbound *out)
{
    struct result_bundle_header bundle;

    if (relay == NULL || out == NULL ||
        !mesh_relay_result_bundle_pending(relay) ||
        out->packet.msg_type != MSG_RESULT_BUNDLE ||
        out->packet.src_id != relay->local_id ||
        out->packet.dst_id != relay->gateway_id ||
        out->payload_len == 0u ||
        result_bundle_header_from_tlvs(out->payload,
                                       out->payload_len,
                                       &bundle) != PROTO_OK) {
        return;
    }

    if (bundle.gateway_id == relay->result_bundle.gateway_id &&
        bundle.gateway_epoch == relay->result_bundle.gateway_epoch &&
        bundle.command_seq == relay->result_bundle.command_seq &&
        bundle.collection_epoch_id == relay->result_bundle.collection_epoch_id &&
        bundle.record_count == relay->result_bundle.record_count) {
        result_bundle_clear(&relay->result_bundle);
    }
}
