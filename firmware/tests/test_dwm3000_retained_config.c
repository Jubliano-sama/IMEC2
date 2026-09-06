#include "dwm3000_driver.h"
#include "deca_device_api.h"
#include "deca_regs.h"
#include "deca_vals.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define IS_ENABLED(value) (value)
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define CONFIG_IMEC_MESH_ROUTE_TEST 0
#define DWM3000_ENABLE_SAME_CHANNEL_FAST_SWITCH 0
#define DWM3000_WAKE_IDLE_RC_TIMEOUT_US 100u
#define DWM3000_STATUS_POLL_INTERVAL_US 10u
#define LOG_WRN(...) ((void)0)
#define LOG_DBG(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define LOG_ERR(...) ((void)0)

enum dwm3000_phy_mode {
    DWM3000_PHY_NONE, DWM3000_PHY_RANGE, DWM3000_PHY_WAKE,
    DWM3000_PHY_MESH_PAYLOAD, DWM3000_PHY_WAKE_MESH_CONTROL,
};

static dwt_config_t wake_config = {
    .chan = 5, .txPreambLength = DWT_PLEN_4096, .rxPAC = DWT_PAC16,
    .txCode = 9, .rxCode = 9, .sfdType = DWT_SFD_DW_16,
    .dataRate = DWT_BR_850K, .phrMode = DWT_PHRMODE_STD,
    .phrRate = DWT_PHRRATE_STD, .sfdTO = 4097,
    .stsMode = DWT_STS_MODE_OFF,
};
static dwt_config_t default_config, wake_mesh_control_config, mesh_payload_config;
static bool radio_configured, radio_awake, radio_restored_from_sleep;
static bool radio_state_unknown;
static enum dwm3000_phy_mode active_phy_mode;
static struct dwm3000_driver_stats driver_stats;
static uint32_t hardware_channel, hardware_tune, hardware_system, hardware_tx;
static uint32_t now_us, spi_hz;
static unsigned int resets, restores, txrx_restores, reads;
static unsigned int fail_read;
static int port_error, reset_error;
static bool wake_pin, idle_confirmed;

static uint32_t k_cycle_get_32(void) { return now_us; }
static uint32_t k_cyc_to_us_floor32(uint32_t cycles) { return cycles; }
static void k_busy_wait(uint32_t delay) { now_us += delay; }
static void status_debug_printf(const char *format, ...) { (void)format; }

static void invalidate_radio_state_tagged(const char *tag)
{
    (void)tag;
    radio_configured = radio_awake = radio_restored_from_sleep = false;
    radio_state_unknown = true;
    active_phy_mode = DWM3000_PHY_NONE;
}

static int take_port_error(const char *operation)
{
    (void)operation;
    int error = port_error;
    port_error = 0;
    if (error < 0) {
        invalidate_radio_state_tagged(operation);
        driver_stats.spi_failures++;
    }
    return error;
}

uint32_t dwt_read32bitoffsetreg(int id, int offset)
{
    assert(offset == 0);
    assert(spi_hz == 32000000u && idle_confirmed);
    now_us += 4u; /* Register traffic consumes real modeled transfer time. */
    if (++reads == fail_read) {
        port_error = -EIO;
        return 0u;
    }
    switch (id) {
    case CHAN_CTRL_ID: return hardware_channel;
    case DTUNE0_ID: return hardware_tune;
    case SYS_CFG_ID: return hardware_system;
    case TX_FCTRL_ID: return hardware_tx;
    case SYS_STATUS_ID: return 0u;
    default: assert(false); return 0u;
    }
}

static int dwm3000_port_set_slow_spi(void) { spi_hz = 2000000u; return 0; }
static int dwm3000_port_set_fast_spi(void)
{
    assert(idle_confirmed);
    spi_hz = 32000000u;
    return 0;
}
static int dwm3000_port_wakeup(void)
{
    assert(spi_hz == 2000000u);
    wake_pin = true;
    now_us += 2500u;
    return 0;
}
static void dwm3000_port_clear_error(void) { port_error = 0; }
uint8_t dwt_checkidlerc(void)
{
    assert(spi_hz == 2000000u && wake_pin);
    idle_confirmed = true;
    return 1u;
}
void dwt_restore_common(void)
{
    assert(reads >= 4u && port_error == 0);
    restores++;
    now_us += 20u;
}
static int restore_txrx_after_sleep(enum dwm3000_phy_mode mode)
{
    assert(mode == DWM3000_PHY_WAKE || mode == DWM3000_PHY_RANGE ||
           mode == DWM3000_PHY_WAKE_MESH_CONTROL);
    txrx_restores++;
    now_us += 100u;
    return 0;
}
static int validate_device_identity(const char *operation)
{
    (void)operation;
    return 0; /* DEV_ID remains correct even for the corrupted PHY fixture. */
}
static int configure_radio_from_reset(enum dwm3000_phy_mode mode)
{
    resets++;
    now_us += 25000u;
    if (reset_error < 0) {
        invalidate_radio_state_tagged("reset-failure");
        return reset_error;
    }
    radio_configured = radio_awake = true;
    radio_restored_from_sleep = radio_state_unknown = false;
    active_phy_mode = mode;
    return 0;
}
static int apply_radio_config(const dwt_config_t *config,
                              enum dwm3000_phy_mode mode)
{
    (void)config;
    (void)mode;
    assert(false); /* Production deliberately disables the fast PHY switch. */
    return -EIO;
}

/* These include the unchanged production validator, wake sequence, and
 * ensure_phy_mode recovery policy. Only hardware and the reset boundary
 * above are faked; cached configuration and actual registers are independent. */
#include "dwm3000_retained_config_production.inc"

static void fixture(bool control)
{
    memset(&driver_stats, 0, sizeof(driver_stats));
    default_config = wake_config;
    wake_mesh_control_config = wake_config;
    wake_mesh_control_config.txPreambLength = DWT_PLEN_1024;
    wake_mesh_control_config.rxPAC = DWT_PAC8;
    wake_mesh_control_config.phrMode = DWT_PHRMODE_EXT;
    wake_mesh_control_config.phrRate = DWT_PHRRATE_DTA;
    wake_mesh_control_config.sfdTO = 1033;
    mesh_payload_config = wake_mesh_control_config;
    mesh_payload_config.chan = 9;
    radio_configured = true;
    radio_awake = radio_restored_from_sleep = radio_state_unknown = false;
    active_phy_mode = control ? DWM3000_PHY_WAKE_MESH_CONTROL : DWM3000_PHY_WAKE;
    /* Independent golden register values: observed healthy wake PHY, plus
     * the SDK register tuple for the production extended control PHY. */
    hardware_channel = 0x094cu;
    hardware_tune = control ? 0x0409101cu : 0x1001101du;
    hardware_system = control ? 0x30u : 0u;
    hardware_tx = control ? 0x280cu : 0x380cu;
    now_us = spi_hz = 0u;
    resets = restores = txrx_restores = reads = fail_read = 0u;
    port_error = reset_error = 0;
    wake_pin = idle_confirmed = false;
}

static void valid_retention_preserves_phy_without_reset(void)
{
    for (unsigned int control = 0u; control < 2u; control++) {
        fixture(control != 0u);
        enum dwm3000_phy_mode mode = active_phy_mode;
        assert(ensure_phy_mode(mode) == 0);
        assert(resets == 0u && restores == 1u && txrx_restores == 1u);
        assert(radio_configured && radio_awake && !radio_state_unknown);
        assert(active_phy_mode == mode && reads == 4u);
    }
    fixture(false);
    /* Equal desired wake/range configs remain eligible for retained reuse. */
    assert(ensure_phy_mode(DWM3000_PHY_RANGE) == 0);
    assert(resets == 0u && txrx_restores == 1u);
    fixture(false);
    assert(ensure_phy_mode(DWM3000_PHY_WAKE_MESH_CONTROL) == 0);
    assert(restores == 1u && txrx_restores == 0u && resets == 1u);
    assert(driver_stats.sleep_wake_failures == 0u);
}

static void bad_retention_recovers_before_any_restore(void)
{
    for (unsigned int cross = 0u; cross < 2u; cross++) {
        for (unsigned int fault = 0u; fault < 12u; fault++) {
            fixture(false);
            switch (fault) {
            case 0u: hardware_channel = 0x094eu; break;
            case 1u: hardware_tune = 0x0041101cu; break;
            case 2u: hardware_tx = 0x1c0cu; break;
            case 3u: hardware_system ^= SYS_CFG_PHR_MODE_BIT_MASK; break;
            case 4u: hardware_channel ^= CHAN_CTRL_RF_CHAN_BIT_MASK; break;
            case 5u: hardware_channel ^= CHAN_CTRL_RX_PCODE_BIT_MASK; break;
            case 6u: hardware_tune ^= DTUNE0_PRE_PAC_SYM_BIT_MASK; break;
            case 7u: hardware_tune ^= DTUNE0_RX_SFD_TOC_BIT_MASK; break;
            case 8u: hardware_system ^= SYS_CFG_CP_SPC_BIT_MASK; break;
            case 9u: hardware_system ^= SYS_CFG_PDOA_MODE_BIT_MASK; break;
            case 10u: hardware_tx ^= TX_FCTRL_TXBR_BIT_MASK; break;
            default:
                hardware_channel = 0x094eu;
                hardware_tune = 0x0041101cu;
                hardware_tx = 0x1c0cu;
                break;
            }
            enum dwm3000_phy_mode requested = cross ?
                DWM3000_PHY_WAKE_MESH_CONTROL : DWM3000_PHY_WAKE;
            assert(ensure_phy_mode(requested) == 0);
            assert(resets == 1u && restores == 0u && txrx_restores == 0u);
            assert(driver_stats.sleep_wake_failures == 1u);
            assert(radio_configured && radio_awake && !radio_state_unknown);
            assert(active_phy_mode == requested);
        }
    }
}

static void every_read_error_requires_recovery_and_failed_reset_stays_closed(void)
{
    for (unsigned int cross = 0u; cross < 2u; cross++) {
        for (unsigned int read = 1u; read <= 4u; read++) {
            for (unsigned int failed_reset = 0u; failed_reset < 2u; failed_reset++) {
                fixture(false);
                fail_read = read;
                reset_error = failed_reset ? -EIO : 0;
                enum dwm3000_phy_mode requested = cross ?
                    DWM3000_PHY_WAKE_MESH_CONTROL : DWM3000_PHY_WAKE;
                assert(ensure_phy_mode(requested) == reset_error);
                assert(resets == 1u && restores == 0u && txrx_restores == 0u);
                assert(driver_stats.sleep_wake_failures == 1u);
                assert(reads == read);
                if (failed_reset) {
                    assert(!radio_configured && !radio_awake && radio_state_unknown);
                    assert(active_phy_mode == DWM3000_PHY_NONE);
                }
            }
        }
    }
}

static void mutable_frame_fields_do_not_invalidate_retention(void)
{
    fixture(false);
    hardware_tx ^= TX_FCTRL_TXFLEN_BIT_MASK | TX_FCTRL_TXB_OFFSET_BIT_MASK |
                   TX_FCTRL_TR_BIT_MASK;
    assert(ensure_phy_mode(DWM3000_PHY_WAKE) == 0);
    assert(resets == 0u && restores == 1u);
}

int main(void)
{
    valid_retention_preserves_phy_without_reset();
    bad_retention_recovers_before_any_restore();
    every_read_error_requires_recovery_and_failed_reset_stays_closed();
    mutable_frame_fields_do_not_invalidate_retention();
    puts("DWM3000 production retained-PHY recovery passed");
    return 0;
}
