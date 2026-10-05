/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

/*
 * Bluetooth LE light sync packet, shared by the gateway (sender) and the
 * satellite lights in ../sync_light (receivers). It is the body of a
 * manufacturer specific data field in a non-connectable advertisement.
 * All multi-byte fields are little endian, as on every ESP32 chip.
 */
#define LIGHT_SYNC_COMPANY_ID   0x02E5      /* Espressif Systems */
#define LIGHT_SYNC_MAGIC_0      'M'
#define LIGHT_SYNC_MAGIC_1      'L'
#define LIGHT_SYNC_VERSION      1

typedef struct __attribute__((packed))
{
    uint16_t company;
    uint8_t magic[2];
    uint8_t version;
    uint8_t group;          /* satellites follow only their own group */
    uint8_t seq;            /* increases with every change */
    uint16_t level[4];      /* R, G, B, W before gamma, 0..65535, brightness included */
} light_sync_packet_t;
