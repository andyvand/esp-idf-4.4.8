/*
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_netif.h"
#include "sys/queue.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_netif_private.h"
#include <string.h>

//
// Purpose of this module is to provide list of esp-netif structures
//  - this module has no dependency on a specific network stack (lwip)
//

static const char *TAG = "esp_netif_objects";

typedef struct slist_netifs_s slist_netifs_t;
struct slist_netifs_s {
    esp_netif_t *netif;
    SLIST_ENTRY(slist_netifs_s) next;
};

SLIST_HEAD(slisthead, slist_netifs_s) s_head = { .slh_first = NULL, };

static size_t s_esp_netif_counter = 0;
static xSemaphoreHandle  s_list_lock = NULL;

ESP_EVENT_DEFINE_BASE(IP_EVENT);

esp_err_t esp_netif_objects_init(void)
{
    if (s_list_lock != NULL) {
        // already initialized
        return ESP_OK;
    }
    s_list_lock = xSemaphoreCreateMutex();
    if (s_list_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void esp_netif_objects_deinit(void)
{
    vSemaphoreDelete(s_list_lock);
    s_list_lock = NULL;
}

esp_err_t esp_netif_list_lock(void)
{
    if (s_list_lock) {
        xSemaphoreTake(s_list_lock, portMAX_DELAY);
    } else {
        ESP_LOGD(TAG, "%s list not locked (s_list_lock not initialized)", __func__);
    }
    return ESP_OK;
}

void esp_netif_list_unlock(void)
{
    if (s_list_lock) {
        xSemaphoreGive(s_list_lock);
    } else {
        ESP_LOGD(TAG, "%s list not unlocked (s_list_lock not initialized)", __func__);
    }
}

//
// List manipulation functions
//
esp_err_t esp_netif_add_to_list(esp_netif_t *netif)
{
    esp_err_t ret;
    struct slist_netifs_s *item = calloc(1, sizeof(struct slist_netifs_s));
    ESP_LOGD(TAG, "%s %p", __func__, netif);
    if (item == NULL) {
        return ESP_ERR_NO_MEM;
    }
    item->netif = netif;

    if ((ret = esp_netif_list_lock()) != ESP_OK) {
        free(item);
        return ret;
    }

    SLIST_INSERT_HEAD(&s_head, item, next);
    ++s_esp_netif_counter;
    ESP_LOGD(TAG, "%s netif added successfully (total netifs: %d)", __func__, s_esp_netif_counter);
    esp_netif_list_unlock();
    return ESP_OK;
}


esp_err_t esp_netif_remove_from_list(esp_netif_t *netif)
{
    struct slist_netifs_s *item;
    esp_err_t ret;
    if ((ret = esp_netif_list_lock()) != ESP_OK) {
        return ret;
    }
    ESP_LOGV(TAG, "%s %p", __func__, netif);

    SLIST_FOREACH(item, &s_head, next) {
        if (item->netif == netif) {
            SLIST_REMOVE(&s_head, item, slist_netifs_s, next);
            assert(s_esp_netif_counter > 0);
            --s_esp_netif_counter;
            ESP_LOGD(TAG, "%s netif successfully removed (total netifs: %d)", __func__, s_esp_netif_counter);
            free(item);
            esp_netif_list_unlock();
            return ESP_OK;
        }
    }
    esp_netif_list_unlock();
    return ESP_ERR_NOT_FOUND;
}

size_t esp_netif_get_nr_of_ifs(void)
{
    return s_esp_netif_counter;
}

esp_netif_t* esp_netif_next(esp_netif_t* netif)
{
    esp_err_t ret;
    esp_netif_t* result;
    if ((ret = esp_netif_list_lock()) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to lock esp-netif list with %d", ret);
        return NULL;
    }
    result = esp_netif_next_unsafe(netif);
    esp_netif_list_unlock();
    return result;
}

esp_netif_t* esp_netif_next_unsafe(esp_netif_t* netif)
{
    ESP_LOGV(TAG, "%s %p", __func__, netif);
    struct slist_netifs_s *item;
    // Getting the first netif if argument is NULL
    if (netif == NULL) {
        item = SLIST_FIRST(&s_head);
        return (item == NULL) ? NULL : item->netif;
    }
    // otherwise the next one (after the supplied netif)
    SLIST_FOREACH(item, &s_head, next) {
        if (item->netif == netif) {
            item = SLIST_NEXT(item, next);
            return (item == NULL) ? NULL : item->netif;
        }
    }
    return NULL;
}

bool esp_netif_is_netif_listed(esp_netif_t *esp_netif)
{
    struct slist_netifs_s *item;
    esp_err_t ret;
    if ((ret = esp_netif_list_lock()) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to lock esp-netif list with %d", ret);
        return false;
    }

    SLIST_FOREACH(item, &s_head, next) {
        if (item->netif == esp_netif) {
            esp_netif_list_unlock();
            return true;
        }
    }
    esp_netif_list_unlock();
    return false;
}

esp_netif_t *esp_netif_get_handle_from_ifkey(const char *if_key)
{
    struct slist_netifs_s *item;
    esp_err_t ret;
    if ((ret = esp_netif_list_lock()) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to lock esp-netif list with %d", ret);
        return NULL;
    }

    SLIST_FOREACH(item, &s_head, next) {
        esp_netif_t *esp_netif = item->netif;
        if (strcmp(if_key, esp_netif_get_ifkey(esp_netif)) == 0) {
            esp_netif_list_unlock();
            return esp_netif;
        }
    }
    esp_netif_list_unlock();
    return NULL;
}
