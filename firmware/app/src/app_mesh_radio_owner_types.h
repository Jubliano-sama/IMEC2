#ifndef APP_MESH_RADIO_OWNER_TYPES_H
#define APP_MESH_RADIO_OWNER_TYPES_H

#include <stdint.h>

enum app_mesh_radio_client {
    APP_MESH_RADIO_CLIENT_NONE = 0,
    APP_MESH_RADIO_CLIENT_CLICKER,
    APP_MESH_RADIO_CLIENT_ANCHOR_SCAN,
    APP_MESH_RADIO_CLIENT_ANCHOR_CLICK,
    APP_MESH_RADIO_CLIENT_SURVEY,
    APP_MESH_RADIO_CLIENT_MESH_TX,
    APP_MESH_RADIO_CLIENT_MESH_RX,
    APP_MESH_RADIO_CLIENT_ML,
    APP_MESH_RADIO_CLIENT_HIGH_DEBUG,
    APP_MESH_RADIO_CLIENT_COUNT,
};

enum app_mesh_radio_abort_kind {
    APP_MESH_RADIO_ABORT_NONE = 0,
    APP_MESH_RADIO_ABORT_HOST_COMMAND,
    APP_MESH_RADIO_ABORT_SCHEDULED_DELIVERY,
    APP_MESH_RADIO_ABORT_INLINE_CONTROL,
    APP_MESH_RADIO_ABORT_TRANSPORT_PAUSE,
    APP_MESH_RADIO_ABORT_SURVEY_ABORT,
    APP_MESH_RADIO_ABORT_KIND_COUNT,
};

struct app_mesh_radio_owner_lease {
    uint32_t generation;
    enum app_mesh_radio_client client;
};

struct app_mesh_radio_owner_pause_lease {
    uint32_t generation;
};

struct app_mesh_radio_owner_handoff_lease {
    uint32_t generation;
    uintptr_t identity;
};

struct app_mesh_radio_owner_abort_lease {
    uint32_t token;
    enum app_mesh_radio_abort_kind kind;
};

#endif
