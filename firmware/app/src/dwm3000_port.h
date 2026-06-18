#ifndef DWM3000_PORT_H
#define DWM3000_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DWM3000_DEV_ID_A0 0xDECA0300u
#define DWM3000_DEV_ID_A0_PDOA 0xDECA0310u
#define DWM3000_DEV_ID_B0 0xDECA0301u
#define DWM3000_DEV_ID_B0_PDOA 0xDECA0311u
#define DWM3000_DEV_ID_C0 0xDECA0302u
#define DWM3000_DEV_ID_C0_PDOA 0xDECA0312u

int dwm3000_port_init(void);
int dwm3000_port_set_slow_spi(void);
int dwm3000_port_set_fast_spi(void);
uint32_t dwm3000_port_current_spi_hz(void);
int dwm3000_port_hw_reset(void);
int dwm3000_port_wakeup(void);
int dwm3000_port_prepare_systemoff(void);
bool dwm3000_port_dev_id_supported(uint32_t dev_id);
int dwm3000_port_read_dev_id(uint32_t *dev_id);
int dwm3000_port_transceive(const uint8_t *tx, uint8_t *rx, size_t len);
int dwm3000_port_write(const uint8_t *header, size_t header_len,
                       const uint8_t *body, size_t body_len);
int dwm3000_port_write_with_crc(const uint8_t *header, size_t header_len,
                                const uint8_t *body, size_t body_len,
                                uint8_t crc8);
int dwm3000_port_read(const uint8_t *header, size_t header_len,
                      uint8_t *body, size_t body_len);

#endif
