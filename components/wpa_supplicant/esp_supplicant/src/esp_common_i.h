/*
 * SPDX-FileCopyrightText: 2020-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESP_COMMON_I_H
#define ESP_COMMON_I_H

#include "utils/includes.h"

struct wpa_funcs;

#ifdef CONFIG_WPA_11KV_SUPPORT
struct ieee_mgmt_frame {
	u8 sender[ETH_ALEN];
	u8 channel;
	int8_t rssi;
	size_t len;
	u8 payload[0];
};

typedef struct {
    uint32_t id;
    uint32_t data;
} supplicant_event_t;

#define SUPPLICANT_API_LOCK() xSemaphoreTakeRecursive(s_supplicant_api_lock, portMAX_DELAY)
#define SUPPLICANT_API_UNLOCK() xSemaphoreGiveRecursive(s_supplicant_api_lock)

#define SUPPLICANT_TASK_STACK_SIZE (6144 + TASK_STACK_SIZE_ADD)
enum SIG_SUPPLICANT {
	SIG_SUPPLICANT_RX_ACTION,
	SIG_SUPPLICANT_SCAN_DONE,
	SIG_SUPPLICANT_DEL_TASK,
	SIG_SUPPLICANT_MAX,
};

int esp_supplicant_post_evt(uint32_t evt_id, uint32_t data);
void esp_get_tx_power(uint8_t *tx_power);
void esp_set_scan_ie(void);
#else

#include "esp_rrm.h"
#include "esp_wnm.h"
#include "esp_mbo.h"

static inline void esp_set_scan_ie(void) { }

#endif
int esp_supplicant_common_init(struct wpa_funcs *wpa_cb);
void esp_supplicant_common_deinit(void);
void esp_supplicant_unset_all_appie(void);
void esp_set_assoc_ie(uint8_t *bssid, const u8 *ies, size_t ies_len, bool add_mdie);
void supplicant_sta_conn_handler(uint8_t* bssid);
void supplicant_sta_disconn_handler(void);
#endif
