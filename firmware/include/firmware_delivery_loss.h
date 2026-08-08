#ifndef FIRMWARE_DELIVERY_LOSS_H
#define FIRMWARE_DELIVERY_LOSS_H

#include <stdbool.h>
#include <stdint.h>

struct mesh_outbound;

struct fw_delivery_loss_state {
    uint32_t lost_count;
};

struct fw_delivery_loss_store_result {
    bool replaced_existing;
    uint32_t lost_count;
};

struct fw_delivery_loss_attach_result {
    bool tlv_attached;
    bool tlv_updated;
    bool lost_count_pending;
    uint32_t lost_count;
    int ret;
};

void fw_delivery_loss_init(struct fw_delivery_loss_state *state);
void fw_delivery_loss_note_store(
    struct fw_delivery_loss_state *state,
    bool existing_valid,
    const struct mesh_outbound *existing,
    const struct mesh_outbound *replacement,
    struct fw_delivery_loss_store_result *result);
void fw_delivery_loss_note_drop(
    struct fw_delivery_loss_state *state,
    struct fw_delivery_loss_store_result *result);
int fw_delivery_loss_attach(
    const struct fw_delivery_loss_state *state,
    struct mesh_outbound *out,
    struct fw_delivery_loss_attach_result *result);
void fw_delivery_loss_note_sent(struct fw_delivery_loss_state *state,
                                const struct mesh_outbound *sent);
uint32_t fw_delivery_loss_count(const struct fw_delivery_loss_state *state);

#endif
