#ifndef APP_NVS_STORAGE_H
#define APP_NVS_STORAGE_H

#include <zephyr/fs/nvs.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * Every application journal in storage_partition must use this one NVS
 * geometry and this one mounted nvs_fs instance. Two nvs_fs objects maintain
 * independent allocation cursors and locks, so mounting the same partition
 * twice can overwrite a record written by the other owner.
 */
#define APP_NVS_STORAGE_SECTOR_SIZE 4096u
#define APP_NVS_ID_MESH_OUTBOX 0x0101u
#define APP_NVS_ID_MESH_CLICK_HANDOFF 0x0107u
#define APP_NVS_ID_MESH_ROUTE_STATE_CLICKER 0x01A6u
#define APP_NVS_ID_MESH_ROUTE_STATE_ANCHOR 0x01A7u
#define APP_NVS_ID_MESH_ROUTE_STATE_GATEWAY 0x01A8u
#define APP_NVS_ID_CLICK_EVENT_SEQUENCE 0x0201u
#define APP_NVS_ROUTE_STATE_RECORD_SIZE 40u

int app_nvs_storage_init(void);
bool app_nvs_storage_ready(void);
struct nvs_fs *app_nvs_storage_fs(void);

#endif
