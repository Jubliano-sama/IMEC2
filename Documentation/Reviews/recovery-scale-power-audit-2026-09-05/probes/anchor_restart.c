/* Reuse the existing application seam and its extracted production functions. */
#define main existing_hia_test_main
#include "../../../../firmware/tests/test_app_hia_table_survey.c"
#undef main

int main(void)
{
    reset_fixture();
    assert(prearm(NEW_EPOCH) == 0);
    assert(apply_table(NEW_EPOCH) == APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    struct survey_control control = start_control();
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND, .src_id = GATEWAY_ID,
    };
    assert(app_survey_anchor_apply_control(&packet, &control) == 0);
    assert(anchor_state.active);
    now_ms += 100u;
    /* The gateway has restarted; this anchor and its survey lease have not. */
    int hia = prearm(NEW_EPOCH + 100u);
    int roster = app_survey_anchor_note_ram_roster(entries, 2u, 2u,
        NEW_EPOCH + 100u, TABLE_SEQ + 100u, &table_commitment);
    printf("new_hia=%d new_table_roster=%d old_survey_active=%d "
           "old_stop=%llu now=%u\n", hia, roster, anchor_state.active,
           (unsigned long long)anchor_state.self_stop_ms, now_ms);
    assert(hia == 0 && roster == -EBUSY && anchor_state.active);
    now_ms = (uint32_t)anchor_state.self_stop_ms;
    assert(anchor_rx_expire_locked(now_ms));
    roster = app_survey_anchor_note_ram_roster(entries, 2u, 2u,
        NEW_EPOCH + 100u, TABLE_SEQ + 100u, &table_commitment);
    printf("after_old_lease_expiry new_table_roster=%d\n", roster);
    assert(roster == 0);
    return 0;
}
