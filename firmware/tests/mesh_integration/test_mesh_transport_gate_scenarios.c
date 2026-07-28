#include "app_mesh_radio_owner.h"
#include "app_mesh_transport_gate.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum operation {
    OP_NONE = 0,
    OP_PAUSE,
    OP_ABORT_REQUEST,
    OP_ABORT_RELEASE,
    OP_RESUME,
};

static bool owner_paused;
static int pause_ret;
static int abort_request_ret;
static int abort_release_ret;
static int resume_ret;
static uint32_t next_token;
static uint32_t watchdog_stop_count;
static enum operation operations[8];
static size_t operation_count;

static void note_operation(enum operation operation)
{
    assert(operation_count < sizeof(operations) / sizeof(operations[0]));
    operations[operation_count++] = operation;
}

bool app_mesh_radio_owner_paused(void)
{
    return owner_paused;
}

int app_mesh_radio_owner_pause(
    struct app_mesh_radio_owner_pause_lease *lease)
{
    note_operation(OP_PAUSE);
    if (pause_ret < 0) {
        return pause_ret;
    }
    if (lease->generation == 0u) {
        lease->generation = ++next_token;
    }
    owner_paused = true;
    return 0;
}

int app_mesh_radio_owner_abort_request(
    enum app_mesh_radio_abort_kind kind,
    struct app_mesh_radio_owner_abort_lease *lease)
{
    note_operation(OP_ABORT_REQUEST);
    assert(kind == APP_MESH_RADIO_ABORT_TRANSPORT_PAUSE);
    if (abort_request_ret < 0) {
        return abort_request_ret;
    }
    if (lease->token == 0u) {
        lease->token = ++next_token;
        lease->kind = kind;
    }
    return 0;
}

int app_mesh_radio_owner_abort_release(
    struct app_mesh_radio_owner_abort_lease *lease)
{
    note_operation(OP_ABORT_RELEASE);
    if (abort_release_ret < 0) {
        return abort_release_ret;
    }
    lease->token = 0u;
    lease->kind = APP_MESH_RADIO_ABORT_NONE;
    return 0;
}

int app_mesh_radio_owner_resume(
    struct app_mesh_radio_owner_pause_lease *lease)
{
    note_operation(OP_RESUME);
    if (resume_ret < 0) {
        return resume_ret;
    }
    lease->generation = 0u;
    owner_paused = false;
    return 0;
}

void app_watchdog_stop_feeding(void)
{
    watchdog_stop_count++;
}

static void reset_capture(void)
{
    pause_ret = 0;
    abort_request_ret = 0;
    abort_release_ret = 0;
    resume_ret = 0;
    watchdog_stop_count = 0u;
    operation_count = 0u;
}

static void test_exact_pause_abort_resume_order(void)
{
    reset_capture();
    assert(app_mesh_transport_gate_pause() == 0);
    assert(app_mesh_transport_gate_paused());
    assert(app_mesh_transport_gate_request_abort() == 0);
    assert(app_mesh_transport_gate_resume() == 0);
    assert(!app_mesh_transport_gate_paused());
    assert(watchdog_stop_count == 0u);
    assert(operation_count == 4u);
    assert(operations[0] == OP_PAUSE);
    assert(operations[1] == OP_ABORT_REQUEST);
    assert(operations[2] == OP_ABORT_RELEASE);
    assert(operations[3] == OP_RESUME);
}

static void test_pause_and_abort_fail_closed(void)
{
    reset_capture();
    pause_ret = -EIO;
    assert(app_mesh_transport_gate_pause() == -EIO);
    assert(watchdog_stop_count == 1u);

    reset_capture();
    assert(app_mesh_transport_gate_pause() == 0);
    abort_request_ret = -ENOSPC;
    assert(app_mesh_transport_gate_request_abort() == -ENOSPC);
    assert(app_mesh_transport_gate_paused());
    assert(watchdog_stop_count == 1u);

    abort_request_ret = 0;
    assert(app_mesh_transport_gate_resume() == 0);
    assert(!app_mesh_transport_gate_paused());
}

static void test_resume_failures_keep_state_recoverable_and_fail_closed(void)
{
    reset_capture();
    assert(app_mesh_transport_gate_pause() == 0);
    assert(app_mesh_transport_gate_request_abort() == 0);
    abort_release_ret = -ESTALE;
    assert(app_mesh_transport_gate_resume() == -ESTALE);
    assert(app_mesh_transport_gate_paused());
    assert(watchdog_stop_count == 1u);

    abort_release_ret = 0;
    assert(app_mesh_transport_gate_resume() == 0);
    assert(!app_mesh_transport_gate_paused());

    reset_capture();
    assert(app_mesh_transport_gate_pause() == 0);
    assert(app_mesh_transport_gate_request_abort() == 0);
    resume_ret = -ESTALE;
    assert(app_mesh_transport_gate_resume() == -ESTALE);
    assert(app_mesh_transport_gate_paused());
    assert(watchdog_stop_count == 1u);

    resume_ret = 0;
    assert(app_mesh_transport_gate_resume() == 0);
    assert(!app_mesh_transport_gate_paused());
}

int main(void)
{
    test_exact_pause_abort_resume_order();
    test_pause_and_abort_fail_closed();
    test_resume_failures_keep_state_recoverable_and_fail_closed();
    return 0;
}
