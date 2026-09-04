#ifndef DWM3000_TIMING_H
#define DWM3000_TIMING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Production DW3000 PHY constants. Keep the Zephyr driver tied to these with
 * build assertions; native tests use the same values without Decawave headers.
 */
#define DWM3000_TIMING_CHANNEL5 5u
#define DWM3000_TIMING_CHANNEL9 9u
#define DWM3000_TIMING_CH5_PREAMBLE_SYMBOLS 4096u
#define DWM3000_TIMING_CH9_PREAMBLE_SYMBOLS 1024u
#define DWM3000_TIMING_CH5_SFD_SYMBOLS 16u
#define DWM3000_TIMING_CH9_SFD_SYMBOLS 8u
#define DWM3000_TIMING_CH5_PAC_SYMBOLS 32u
#define DWM3000_TIMING_CH9_PAC_SYMBOLS 8u
#define DWM3000_TIMING_CH5_SFD_TIMEOUT_SYMBOLS 4073u
#define DWM3000_TIMING_CH9_SFD_TIMEOUT_SYMBOLS 1025u
/*
 * The channel-5 mesh-control PHY keeps the wake channel, codes and 16-symbol
 * DW SFD but shortens the acquisition train: control frames only ever reach a
 * receiver that is already listening, so the 4096-symbol preamble the
 * duty-cycled sniffers need is pure airtime there.  Timeout derivation matches
 * the driver: preamble + 1 SFD search symbol + SFD - PAC.
 */
#define DWM3000_TIMING_CH5_CONTROL_PREAMBLE_SYMBOLS 1024u
#define DWM3000_TIMING_CH5_CONTROL_PAC_SYMBOLS 8u
#define DWM3000_TIMING_CH5_CONTROL_SFD_TIMEOUT_SYMBOLS 1033u
#define DWM3000_TIMING_IMMEDIATE_PREAMBLE_TIMEOUT_PAC 5u
#define DWM3000_TIMING_DELAYED_PREAMBLE_TIMEOUT_PAC 0u
#define DWM3000_TIMING_PHR_BITS 21u
#define DWM3000_TIMING_RS_DATA_BITS_PER_BLOCK 330u
#define DWM3000_TIMING_RS_PARITY_BITS_PER_BLOCK 48u
#define DWM3000_TIMING_FCS_BYTES 2u
#define DWM3000_TIMING_STANDARD_PSDU_MAX_BYTES 127u
#define DWM3000_TIMING_EXTENDED_PSDU_MAX_BYTES 1023u

/* Code 9 selects the 64 MHz PRF timing used by every production PHY here. */
#define DWM3000_TIMING_PRF64_CHIPS_PER_SHR_SYMBOL 508u
#define DWM3000_TIMING_850K_CHIPS_PER_BIT_SYMBOL 512u
#define DWM3000_TIMING_RCTU_PER_CHIP 128u
#define DWM3000_TIMING_RCTU_PER_SECOND UINT64_C(63897600000)

#define DWM3000_TIMING_DX_TIME_QUANTUM_RCTU 512u
#define DWM3000_TIMING_TX_ANTENNA_DELAY_RCTU 16345u
#define DWM3000_TIMING_RX_ANTENNA_DELAY_RCTU 16345u
#define DWM3000_TIMING_UUS_TO_RCTU 63898u

enum dwm3000_timing_phy {
    DWM3000_TIMING_PHY_CH5_WAKE = 0,
    DWM3000_TIMING_PHY_CH5_RANGE = 1,
    DWM3000_TIMING_PHY_CH5_MESH_CONTROL = 2,
    DWM3000_TIMING_PHY_CH9_MESH = 3,
};

enum dwm3000_timing_phr_mode {
    DWM3000_TIMING_PHR_STANDARD = 0,
    DWM3000_TIMING_PHR_EXTENDED = 1,
};

struct dwm3000_phy_timing {
    uint16_t preamble_symbols;
    uint16_t sfd_symbols;
    uint16_t pac_symbols;
    uint16_t sfd_timeout_symbols;
    uint16_t max_frame_bytes_without_fcs;
    uint8_t channel;
    enum dwm3000_timing_phr_mode phr_mode;
};

struct dwm3000_air_interval {
    uint64_t start_rctu;
    uint64_t rmarker_rctu;
    uint64_t end_rctu;
};

const struct dwm3000_phy_timing *dwm3000_timing_phy_profile(
    enum dwm3000_timing_phy phy);
uint64_t dwm3000_timing_shr_rctu(enum dwm3000_timing_phy phy);
uint64_t dwm3000_timing_preamble_rctu(enum dwm3000_timing_phy phy);
uint64_t dwm3000_timing_sfd_rctu(enum dwm3000_timing_phy phy);
uint64_t dwm3000_timing_pac_rctu(enum dwm3000_timing_phy phy);
/* frame_bytes_without_fcs is the protocol frame length. The mandatory RF
 * scope prefix and hardware FCS are added by this model. */
uint64_t dwm3000_timing_airtime_rctu(enum dwm3000_timing_phy phy,
                                     size_t frame_bytes_without_fcs);
uint64_t dwm3000_timing_airtime_us_ceil(enum dwm3000_timing_phy phy,
                                       size_t frame_bytes_without_fcs);
uint64_t dwm3000_timing_rctu_to_us_floor(uint64_t rctu);
uint64_t dwm3000_timing_rctu_to_us_ceil(uint64_t rctu);
uint64_t dwm3000_timing_us_to_rctu_floor(uint64_t us);
uint64_t dwm3000_timing_us_to_rctu_ceil(uint64_t us);
uint64_t dwm3000_timing_uus_to_rctu(uint32_t uus);
uint64_t dwm3000_timing_quantize_delayed_raw_rmarker(uint64_t requested_rctu);
int dwm3000_timing_delayed_tx_interval(enum dwm3000_timing_phy phy,
                                       size_t frame_bytes_without_fcs,
                                       uint64_t requested_raw_rmarker_rctu,
                                       uint64_t propagation_rctu,
                                       struct dwm3000_air_interval *arrival);

#ifdef __cplusplus
}
#endif

#endif
