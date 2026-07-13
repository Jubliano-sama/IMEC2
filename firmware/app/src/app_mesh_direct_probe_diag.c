#include "app_mesh_direct_probe_diag.h"

#include "app_board.h"

#include <hal/nrf_power.h>

#include <zephyr/kernel.h>

#define DIRECT_PROBE_BREADCRUMB_MAGIC UINT32_C(0x44504742)

struct direct_probe_breadcrumb {
    uint32_t magic;
    uint32_t phase;
    uint32_t attempt;
    uint32_t sequence;
    uint32_t timestamp_ms;
};

static volatile struct direct_probe_breadcrumb direct_probe_breadcrumb
    __attribute__((section(".noinit")));

void app_mesh_direct_probe_breadcrumb_note(enum app_mesh_direct_probe_phase phase,
                                           uint8_t attempt,
                                           uint16_t sequence)
{
    direct_probe_breadcrumb.magic = DIRECT_PROBE_BREADCRUMB_MAGIC;
    direct_probe_breadcrumb.phase = (uint32_t)phase;
    direct_probe_breadcrumb.attempt = attempt;
    direct_probe_breadcrumb.sequence = sequence;
    direct_probe_breadcrumb.timestamp_ms = k_uptime_get_32();
}

void app_mesh_direct_probe_breadcrumb_boot_diagnostics(void)
{
    uint32_t reset_reason = 0u;

#if NRF_POWER_HAS_RESETREAS
    reset_reason = nrf_power_resetreas_get(NRF_POWER);
#endif
    if (direct_probe_breadcrumb.magic != DIRECT_PROBE_BREADCRUMB_MAGIC) {
        status_debug_printf("DBG_DIRECT_GW_PROBE_BOOT resetreas=0x%08x phase=none\n",
                            reset_reason);
        return;
    }

    status_debug_printf("DBG_DIRECT_GW_PROBE_BOOT resetreas=0x%08x phase=%u attempt=%u seq=%u at=%u\n",
                        reset_reason,
                        direct_probe_breadcrumb.phase,
                        direct_probe_breadcrumb.attempt,
                        direct_probe_breadcrumb.sequence,
                        direct_probe_breadcrumb.timestamp_ms);
}
