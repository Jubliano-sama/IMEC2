#include "dwm3000_timing.h"

#include "protocol.h"
#include "uwb.h"

#include <limits.h>

static const struct dwm3000_phy_timing phy_profiles[] = {
    [DWM3000_TIMING_PHY_CH5_WAKE] = {
        .preamble_symbols = DWM3000_TIMING_CH5_PREAMBLE_SYMBOLS,
        .sfd_symbols = DWM3000_TIMING_CH5_SFD_SYMBOLS,
        .pac_symbols = DWM3000_TIMING_CH5_PAC_SYMBOLS,
        .sfd_timeout_symbols = DWM3000_TIMING_CH5_SFD_TIMEOUT_SYMBOLS,
        .max_frame_bytes_without_fcs =
            DWM3000_TIMING_STANDARD_PSDU_MAX_BYTES - DWM3000_TIMING_FCS_BYTES,
        .channel = DWM3000_TIMING_CHANNEL5,
        .phr_mode = DWM3000_TIMING_PHR_STANDARD,
    },
    [DWM3000_TIMING_PHY_CH5_RANGE] = {
        .preamble_symbols = DWM3000_TIMING_CH5_PREAMBLE_SYMBOLS,
        .sfd_symbols = DWM3000_TIMING_CH5_SFD_SYMBOLS,
        .pac_symbols = DWM3000_TIMING_CH5_PAC_SYMBOLS,
        .sfd_timeout_symbols = DWM3000_TIMING_CH5_SFD_TIMEOUT_SYMBOLS,
        .max_frame_bytes_without_fcs =
            DWM3000_TIMING_STANDARD_PSDU_MAX_BYTES - DWM3000_TIMING_FCS_BYTES,
        .channel = DWM3000_TIMING_CHANNEL5,
        .phr_mode = DWM3000_TIMING_PHR_STANDARD,
    },
    /*
     * The channel-5 mesh-control PHY is the wake PHY with an extended PHR and
     * a short acquisition train: wake_mesh_control_config in dwm3000_driver.c
     * keeps DWM3000_PHY_CHANNEL, the codes and DWM3000_PHY_SFD_TYPE (16-symbol
     * DW SFD) but overrides txPreambLength/rxPAC/sfdTO plus phrMode/phrRate.
     * Control frames are only exchanged between nodes that are already awake
     * and listening, so the wake PHY's 4096-symbol preamble bought nothing
     * here and cost 4185 us of SHR per frame; 1024 symbols cost 1059 us.
     * The wake and range profiles above keep the long train the duty-cycled
     * sniffers need.  test_dwm3000_models.c pins the resulting airtimes.
     */
    [DWM3000_TIMING_PHY_CH5_MESH_CONTROL] = {
        .preamble_symbols = DWM3000_TIMING_CH5_CONTROL_PREAMBLE_SYMBOLS,
        .sfd_symbols = DWM3000_TIMING_CH5_SFD_SYMBOLS,
        .pac_symbols = DWM3000_TIMING_CH5_CONTROL_PAC_SYMBOLS,
        .sfd_timeout_symbols = DWM3000_TIMING_CH5_CONTROL_SFD_TIMEOUT_SYMBOLS,
        .max_frame_bytes_without_fcs =
            DWM3000_TIMING_EXTENDED_PSDU_MAX_BYTES - DWM3000_TIMING_FCS_BYTES,
        .channel = DWM3000_TIMING_CHANNEL5,
        .phr_mode = DWM3000_TIMING_PHR_EXTENDED,
    },
    [DWM3000_TIMING_PHY_CH9_MESH] = {
        .preamble_symbols = DWM3000_TIMING_CH9_PREAMBLE_SYMBOLS,
        .sfd_symbols = DWM3000_TIMING_CH9_SFD_SYMBOLS,
        .pac_symbols = DWM3000_TIMING_CH9_PAC_SYMBOLS,
        .sfd_timeout_symbols = DWM3000_TIMING_CH9_SFD_TIMEOUT_SYMBOLS,
        .max_frame_bytes_without_fcs =
            DWM3000_TIMING_EXTENDED_PSDU_MAX_BYTES - DWM3000_TIMING_FCS_BYTES,
        .channel = DWM3000_TIMING_CHANNEL9,
        .phr_mode = DWM3000_TIMING_PHR_EXTENDED,
    },
};

_Static_assert(UWB_PHY_FCS_LEN == DWM3000_TIMING_FCS_BYTES,
               "airtime FCS must match the UWB driver");
_Static_assert(UWB_PHY_EXTENDED_FRAME_MAX_LEN ==
                   DWM3000_TIMING_EXTENDED_PSDU_MAX_BYTES,
               "airtime extended PSDU limit must match UWB framing");
_Static_assert(UWB_RF_SCOPE_WIRE_LEN + UWB_MESH_MAX_FRAME_LEN +
                   UWB_PHY_FCS_LEN ==
                   DWM3000_TIMING_EXTENDED_PSDU_MAX_BYTES,
               "maximum mesh frame must consume the modeled extended PSDU");
_Static_assert(PACKET_EXT_MAX_LEN <= UWB_MESH_MAX_PACKET_LEN,
               "extended protocol packet must fit the modeled UWB frame");

static uint64_t mul_div_floor(uint64_t value, uint64_t multiplier, uint64_t divisor)
{
    uint64_t quotient;
    uint64_t remainder;

    if (divisor == 0u) {
        return UINT64_MAX;
    }
    quotient = value / divisor;
    remainder = value % divisor;
    if (quotient > UINT64_MAX / multiplier) {
        return UINT64_MAX;
    }
    return quotient * multiplier + (remainder * multiplier) / divisor;
}

static uint64_t mul_div_ceil(uint64_t value, uint64_t multiplier, uint64_t divisor)
{
    uint64_t floor_value;
    uint64_t remainder;

    floor_value = mul_div_floor(value, multiplier, divisor);
    if (floor_value == UINT64_MAX || divisor == 0u) {
        return UINT64_MAX;
    }
    remainder = value % divisor;
    return remainder != 0u && (remainder * multiplier) % divisor != 0u ?
           floor_value + 1u : floor_value;
}

const struct dwm3000_phy_timing *dwm3000_timing_phy_profile(
    enum dwm3000_timing_phy phy)
{
    if (phy < DWM3000_TIMING_PHY_CH5_WAKE ||
        phy > DWM3000_TIMING_PHY_CH9_MESH) {
        return NULL;
    }
    return &phy_profiles[phy];
}

uint64_t dwm3000_timing_preamble_rctu(enum dwm3000_timing_phy phy)
{
    const struct dwm3000_phy_timing *profile = dwm3000_timing_phy_profile(phy);

    return profile == NULL ? 0u :
           (uint64_t)profile->preamble_symbols *
               DWM3000_TIMING_PRF64_CHIPS_PER_SHR_SYMBOL *
               DWM3000_TIMING_RCTU_PER_CHIP;
}

uint64_t dwm3000_timing_sfd_rctu(enum dwm3000_timing_phy phy)
{
    const struct dwm3000_phy_timing *profile = dwm3000_timing_phy_profile(phy);

    return profile == NULL ? 0u :
           (uint64_t)profile->sfd_symbols *
               DWM3000_TIMING_PRF64_CHIPS_PER_SHR_SYMBOL *
               DWM3000_TIMING_RCTU_PER_CHIP;
}

uint64_t dwm3000_timing_shr_rctu(enum dwm3000_timing_phy phy)
{
    uint64_t preamble = dwm3000_timing_preamble_rctu(phy);
    uint64_t sfd = dwm3000_timing_sfd_rctu(phy);

    return preamble == 0u || sfd == 0u ? 0u : preamble + sfd;
}

uint64_t dwm3000_timing_pac_rctu(enum dwm3000_timing_phy phy)
{
    const struct dwm3000_phy_timing *profile = dwm3000_timing_phy_profile(phy);

    return profile == NULL ? 0u :
           (uint64_t)profile->pac_symbols *
               DWM3000_TIMING_PRF64_CHIPS_PER_SHR_SYMBOL *
               DWM3000_TIMING_RCTU_PER_CHIP;
}

uint64_t dwm3000_timing_airtime_rctu(enum dwm3000_timing_phy phy,
                                    size_t frame_bytes_without_fcs)
{
    const struct dwm3000_phy_timing *profile = dwm3000_timing_phy_profile(phy);
    size_t physical_frame_bytes_without_fcs;
    uint64_t data_bits;
    uint64_t rs_blocks;
    uint64_t coded_bits;
    uint64_t shr_chips;
    uint64_t phr_chips;
    uint64_t data_chips;
    uint64_t total_chips;

    if (profile == NULL || frame_bytes_without_fcs == 0u ||
        frame_bytes_without_fcs >
            profile->max_frame_bytes_without_fcs - UWB_RF_SCOPE_WIRE_LEN) {
        return 0u;
    }

    physical_frame_bytes_without_fcs =
        frame_bytes_without_fcs + UWB_RF_SCOPE_WIRE_LEN;
    data_bits = ((uint64_t)physical_frame_bytes_without_fcs +
                 DWM3000_TIMING_FCS_BYTES) * 8u;
    rs_blocks = (data_bits + DWM3000_TIMING_RS_DATA_BITS_PER_BLOCK - 1u) /
                DWM3000_TIMING_RS_DATA_BITS_PER_BLOCK;
    coded_bits = data_bits + rs_blocks * DWM3000_TIMING_RS_PARITY_BITS_PER_BLOCK;
    shr_chips = ((uint64_t)profile->preamble_symbols + profile->sfd_symbols) *
                DWM3000_TIMING_PRF64_CHIPS_PER_SHR_SYMBOL;
    phr_chips = DWM3000_TIMING_PHR_BITS *
                DWM3000_TIMING_850K_CHIPS_PER_BIT_SYMBOL;
    data_chips = coded_bits * DWM3000_TIMING_850K_CHIPS_PER_BIT_SYMBOL;
    total_chips = shr_chips + phr_chips + data_chips;
    return total_chips * DWM3000_TIMING_RCTU_PER_CHIP;
}

uint64_t dwm3000_timing_rctu_to_us_floor(uint64_t rctu)
{
    /* 1 us is exactly 638976 / 10 RCTU. */
    return mul_div_floor(rctu, 10u, 638976u);
}

uint64_t dwm3000_timing_rctu_to_us_ceil(uint64_t rctu)
{
    return mul_div_ceil(rctu, 10u, 638976u);
}

uint64_t dwm3000_timing_us_to_rctu_floor(uint64_t us)
{
    return mul_div_floor(us, 638976u, 10u);
}

uint64_t dwm3000_timing_us_to_rctu_ceil(uint64_t us)
{
    return mul_div_ceil(us, 638976u, 10u);
}

uint64_t dwm3000_timing_airtime_us_ceil(enum dwm3000_timing_phy phy,
                                       size_t frame_bytes_without_fcs)
{
    uint64_t airtime = dwm3000_timing_airtime_rctu(phy, frame_bytes_without_fcs);

    return airtime == 0u ? 0u : dwm3000_timing_rctu_to_us_ceil(airtime);
}

uint64_t dwm3000_timing_uus_to_rctu(uint32_t uus)
{
    return (uint64_t)uus * DWM3000_TIMING_UUS_TO_RCTU;
}

uint64_t dwm3000_timing_quantize_delayed_raw_rmarker(uint64_t requested_rctu)
{
    return requested_rctu & ~(uint64_t)(DWM3000_TIMING_DX_TIME_QUANTUM_RCTU - 1u);
}

int dwm3000_timing_delayed_tx_interval(enum dwm3000_timing_phy phy,
                                       size_t frame_bytes_without_fcs,
                                       uint64_t requested_raw_rmarker_rctu,
                                       uint64_t propagation_rctu,
                                       struct dwm3000_air_interval *arrival)
{
    uint64_t airtime;
    uint64_t shr;
    uint64_t raw_rmarker;
    uint64_t rmarker;

    if (arrival == NULL) {
        return PROTO_ERR_ARG;
    }
    airtime = dwm3000_timing_airtime_rctu(phy, frame_bytes_without_fcs);
    shr = dwm3000_timing_shr_rctu(phy);
    if (airtime == 0u || shr == 0u) {
        return PROTO_ERR_BAD_LENGTH;
    }
    raw_rmarker = dwm3000_timing_quantize_delayed_raw_rmarker(
        requested_raw_rmarker_rctu);
    if (raw_rmarker > UINT64_MAX - DWM3000_TIMING_TX_ANTENNA_DELAY_RCTU) {
        return PROTO_ERR_ARG;
    }
    rmarker = raw_rmarker + DWM3000_TIMING_TX_ANTENNA_DELAY_RCTU;
    if (rmarker < shr || rmarker - shr > UINT64_MAX - propagation_rctu) {
        return PROTO_ERR_ARG;
    }

    arrival->start_rctu = rmarker - shr + propagation_rctu;
    arrival->rmarker_rctu = rmarker + propagation_rctu;
    if (arrival->start_rctu > UINT64_MAX - airtime) {
        return PROTO_ERR_ARG;
    }
    arrival->end_rctu = arrival->start_rctu + airtime;
    return PROTO_OK;
}
