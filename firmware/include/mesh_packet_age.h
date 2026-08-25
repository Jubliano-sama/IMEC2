#ifndef MESH_PACKET_AGE_H
#define MESH_PACKET_AGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t mesh_packet_age_at_air_arrival(uint32_t base_age_ms,
                                        uint32_t queued_at_ms,
                                        bool queued_at_valid,
                                        uint32_t tx_snapshot_ms,
                                        uint64_t frame_airtime_us);

#ifdef __cplusplus
}
#endif

#endif
