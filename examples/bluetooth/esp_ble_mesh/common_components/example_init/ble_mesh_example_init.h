/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#ifndef _BLE_MESH_EXAMPLE_INIT_H_
#define _BLE_MESH_EXAMPLE_INIT_H_

#ifdef __cplusplus
extern "C" {
#endif /**< __cplusplus */

#include "esp_err.h"

void ble_mesh_get_dev_uuid(uint8_t *dev_uuid);

esp_err_t bluetooth_init(void);

#ifdef __cplusplus
}
#endif /**< __cplusplus */

#endif /* _BLE_MESH_EXAMPLE_INIT_H_ */
