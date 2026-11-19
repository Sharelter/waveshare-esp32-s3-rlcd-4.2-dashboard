// SPDX-License-Identifier: Apache-2.0
// Wire protocol between the host collector and the ESP32-S3-RLCD-4.2 dashboard.
//
// Frame layout (little endian):
//   A5 5A <len:u8> <payload:len bytes> <crc16:u16>
//   crc16 = CRC-16/CCITT-FALSE over the payload bytes only.
//
// Every frame carries the full dashboard state; the device renders whatever it
// last received.  Payload is a fixed-size packed struct so both ends can use a
// single memcpy.
#pragma once

#include <stdint.h>

#define DASH_MAGIC0 0xA5
#define DASH_MAGIC1 0x5A

#define DASH_MAX_IFACES 4
#define DASH_MAX_CPUS 32
#define DASH_IFNAME_LEN 8
#define DASH_HOST_LEN 16

#define DASH_NO_TEMP INT16_MIN

typedef struct __attribute__((packed)) {
  char name[DASH_IFNAME_LEN];  // NUL padded
  uint32_t rx_bps;             // bytes/second
  uint32_t tx_bps;             // bytes/second
  uint8_t up;                  // 0/1 operstate
  uint8_t pad[3];
} dash_iface_t;

typedef struct __attribute__((packed)) {
  uint16_t seq;
  uint16_t ncpu;
  uint16_t cpu_total;                       // 0..1000 == 0.0 .. 100.0 %
  uint8_t cpu_load[DASH_MAX_CPUS];          // 0..100 per logical cpu
  uint32_t mem_used_mb;
  uint32_t mem_total_mb;
  uint32_t swap_used_mb;
  uint32_t swap_total_mb;
  uint16_t load1_x100;
  uint16_t load5_x100;
  uint16_t load15_x100;
  int16_t temp_x10;                         // degrees C * 10
  uint32_t uptime_s;
  uint8_t niface;
  uint8_t flags;
  uint8_t hour, minute, second;
  char host[DASH_HOST_LEN];
  dash_iface_t iface[DASH_MAX_IFACES];
} dash_payload_t;

#define DASH_PAYLOAD_LEN ((uint8_t)sizeof(dash_payload_t))
#define DASH_FRAME_LEN (5 + DASH_PAYLOAD_LEN)  // magic+len + payload + crc

// The Python collector packs the very same layout; a mismatch here is a bug.
static_assert(sizeof(dash_payload_t) == 167, "dash_payload_t layout changed");
static_assert(sizeof(dash_iface_t) == 20, "dash_iface_t layout changed");

static inline uint16_t dash_crc16(const uint8_t *data, uint32_t len) {
  uint16_t crc = 0xFFFF;
  for (uint32_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}
