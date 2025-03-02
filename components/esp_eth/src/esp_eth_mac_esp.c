/*
 * SPDX-FileCopyrightText: 2019-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <stdlib.h>
#include <sys/cdefs.h>
#include "driver/periph_ctrl.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "hal/cpu_hal.h"
#include "hal/emac_hal.h"
#include "hal/gpio_hal.h"
#include "soc/soc.h"
#include "soc/rtc.h"
#include "sdkconfig.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "hal/emac_ll.h"

static const char *TAG = "esp.emac";

#define PHY_OPERATION_TIMEOUT_US (1000)
#define MAC_STOP_TIMEOUT_US (2500) // this is absolute maximum for 10Mbps, it is 10 times faster for 100Mbps
#define FLOW_CONTROL_LOW_WATER_MARK (CONFIG_ETH_DMA_RX_BUFFER_NUM / 3)
#define FLOW_CONTROL_HIGH_WATER_MARK (FLOW_CONTROL_LOW_WATER_MARK * 2)

typedef struct {
    esp_eth_mac_t parent;
    esp_eth_mediator_t *eth;
    emac_hal_context_t hal;
    intr_handle_t intr_hdl;
    TaskHandle_t rx_task_hdl;
    uint32_t sw_reset_timeout_ms;
    uint32_t frames_remain;
    uint32_t free_rx_descriptor;
    uint32_t flow_control_high_water_mark;
    uint32_t flow_control_low_water_mark;
    int smi_mdc_gpio_num;
    int smi_mdio_gpio_num;
    eth_mac_clock_config_t clock_config;
    uint8_t addr[6];
    uint8_t *rx_buf[CONFIG_ETH_DMA_RX_BUFFER_NUM];
    uint8_t *tx_buf[CONFIG_ETH_DMA_TX_BUFFER_NUM];
    bool isr_need_yield;
    bool flow_ctrl_enabled; // indicates whether the user want to do flow control
    bool do_flow_ctrl;  // indicates whether we need to do software flow control
#ifdef CONFIG_PM_ENABLE
    esp_pm_lock_handle_t pm_lock;
#endif
} emac_esp32_t;

static esp_err_t esp_emac_alloc_driver_obj(const eth_mac_config_t *config, emac_esp32_t **emac_out_hdl, void **out_descriptors);
static void esp_emac_free_driver_obj(emac_esp32_t *emac, void *descriptors);
static esp_err_t emac_esp32_start(esp_eth_mac_t *mac);
static esp_err_t emac_esp32_stop(esp_eth_mac_t *mac);

static esp_err_t emac_esp32_set_mediator(esp_eth_mac_t *mac, esp_eth_mediator_t *eth)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(eth, ESP_ERR_INVALID_ARG, err, TAG, "can't set mac's mediator to null");
    emac_esp32_t *emac = __containerof(mac, emac_esp32_t, parent);
    emac->eth = eth;
    return ESP_OK;
err:
    return ret;
}

static esp_err_t emac_esp32_write_phy_reg(esp_eth_mac_t *mac, uint32_t phy_addr, uint32_t phy_reg, uint32_t reg_value)
{
    esp_err_t ret = ESP_OK;
    emac_esp32_t *emac = __containerof(mac, emac_esp32_t, parent);
    ESP_GOTO_ON_FALSE(!emac_ll_is_mii_busy(emac->hal.mac_regs), ESP_ERR_INVALID_STATE, err, TAG, "phy is busy");
    emac_ll_set_phy_data(emac->hal.mac_regs, reg_value);
    emac_hal_set_phy_cmd(&emac->hal, phy_addr, phy_reg, true);
    /* polling the busy flag */
    uint32_t to = 0;
    bool busy = true;
    do {
        esp_rom_delay_us(100);
        busy = emac_ll_is_mii_busy(emac->hal.mac_regs);
        to += 100;
    } while (busy && to < PHY_OPERATION_TIMEOUT_US);
    ESP_GOTO_ON_FALSE(!busy, ESP_ERR_TIMEOUT, err, TAG, "phy is busy");
    return ESP_OK;
err:
    return ret;
}

static esp_err_t emac_esp32_read_phy_reg(esp_eth_mac_t *mac, uint32_t phy_addr, uint32_t phy_reg, uint32_t *reg_value)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(reg_value, ESP_ERR_INVALID_ARG, err, TAG, "can't set reg_value to null");
    emac_esp32_t *emac = __containerof(mac, emac_esp32_t, parent);
    ESP_GOTO_ON_FALSE(!emac_ll_is_mii_busy(emac->hal.mac_regs), ESP_ERR_INVALID_STATE, err, TAG, "phy is busy");
    emac_hal_set_phy_cmd(&emac->hal, phy_addr, phy_reg, false);
    /* polling the busy flag */
    uint32_t to = 0;
    bool busy = true;
    do {
        esp_rom_delay_us(100);
        busy = emac_ll_is_mii_busy(emac->hal.mac_regs);
        to += 100;
    } while (busy && to < PHY_OPERATION_TIMEOUT_US);
    ESP_GOTO_ON_FALSE(!busy, ESP_ERR_TIMEOUT, err, TAG, "phy is busy");
    /* Store value */
    *reg_value = emac_ll_get_phy_data(emac->hal.mac_regs);
    return ESP_OK;
err:
    return ret;
}

static esp_err_t emac_esp32_set_addr(esp_eth_mac_t *mac, uint8_t *addr)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(addr, ESP_ERR_INVALID_ARG, err, TAG, "can't set mac addr to null");
    emac_esp32_t *emac = __containerof(mac, emac_esp32_t, parent);
    memcpy(emac->addr, addr, 6);
    emac_hal_set_address(&emac->hal, emac->addr);
    return ESP_OK;
err:
    return ret;
}

static esp_err_t emac_esp32_get_addr(esp_eth_mac_t *mac, uint8_t *addr)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(addr, ESP_ERR_INVALID_ARG, err, TAG, "can't set mac addr to null");
    emac_esp32_t *emac = __containerof(mac, emac_esp32_t, parent);
    memcpy(addr, emac->addr, 6);
    return ESP_OK;
err:
    return ret;
}

static esp_err_t emac_esp32_set_link(esp_eth_mac_t *mac, eth_link_t link)
{
    esp_err_t ret = ESP_OK;
    emac_esp32_t *emac = __containerof(mac, emac_esp32_t, parent);
    switch (link) {
    case ETH_LINK_UP:
        ESP_GOTO_ON_ERROR(esp_intr_enable(emac->intr_hdl), err, TAG, "enable interrupt failed");
        emac_esp32_start(mac);
        break;
    case ETH_LINK_DOWN:
        ESP_GOTO_ON_ERROR(esp_intr_disable(emac->intr_hdl), err, TAG, "disable interrupt failed");
        emac_esp32_stop(mac);
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_INVALID_ARG, err, TAG, "unknown link status");
        break;
    }
    return ESP_OK;
err:
    return ret;
}

static esp_err_t emac_esp32_set_speed(esp_eth_mac_t *mac, eth_speed_t speed)
{
    esp_err_t ret = ESP_ERR_INVALID_ARG;
    emac_esp32_t *emac = __containerof(mac, emac_esp32_t, parent);
    if (speed >= ETH_SPEED_10M && speed < ETH_SPEED_MAX) {
        emac_ll_set_port_speed(emac->hal.mac_regs, speed);
        ESP_LOGD(TAG, "working in %dMbps", speed == ETH_SPEED_10M ? 10 : 100);
        return ESP_OK;
    }
    return ret;
}

static esp_err_t emac_esp32_set_duplex(esp_eth_mac_t *mac, eth_duplex_t duplex)
{
    esp_err_t ret = ESP_ERR_INVALID_ARG;
    emac_esp32_t *emac = __containerof(mac, emac_esp32_t, parent);
    if (duplex == ETH_DUPLEX_HALF || duplex == ETH_DUPLEX_FULL) {
        emac_ll_set_duplex(emac->hal.mac_regs, duplex);
        ESP_LOGD(TAG, "working in %s duplex", duplex == ETH_DUPLEX_HALF ? "half" : "full");
        return ESP_OK;
    }
    return ret;
}

static esp_err_t emac_esp32_set_promiscuous(esp_eth_mac_t *mac, bool enable)
{
    emac_esp32_t *emac = __containerof(mac, emac_esp32_t, parent);
    emac_ll_promiscuous_mode_enable(emac->hal.mac_regs, enable);
    return ESP_OK;
}

static esp_err_t emac_esp32_enable_flow_ctrl(esp_eth_mac_t *mac, bool enable)
{
    emac_esp32_t *emac = __containerof(mac, emac_esp32_t, parent);
    emac->flow_ctrl_enabled = enable;
    return ESP_OK;
}

static esp_err_t emac_esp32_set_peer_pause_ability(esp_eth_mac_t *mac, uint32_t ability)
{
    emac_esp32_t *emac = __containerof(mac, emac_esp32_t, parent);
    // we want to enable flow control, and peer does support pause function
    // then configure the MAC layer to enable flow control feature
    if (emac->flow_ctrl_enabled && ability) {
        emac_hal_enable_flow_ctrl(&emac->hal, true);
        emac->do_flow_ctrl = true;
    } else {
        emac_hal_enable_flow_ctrl(&emac->hal, false);
        emac->do_flow_ctrl = false;
        ESP_LOGD(TAG, "Flow control not enabled for the link");
    }
    return ESP_OK;
}

static esp_err_t emac_esp32_transmit(esp_eth_mac_t *mac, uint8_t *buf, uint32_t length)
{
    esp_err_t ret = ESP_OK;
    emac_esp32_t *emac = __containerof(mac, emac_esp32_t, parent);
    uint32_t sent_len = emac_hal_transmit_frame(&emac->hal, buf, length);
    ESP_GOTO_ON_FALSE(sent_len == length, ESP_ERR_NO_MEM, err, TAG, "insufficient TX buffer size");
    return ESP_OK;
err:
    return ret;
}

static esp_err_t emac_esp32_receive(esp_eth_mac_t *mac, uint8_t *buf, uint32_t *length)
{
    esp_err_t ret = ESP_OK;
    uint32_t expected_len = *length;
    emac_esp32_t *emac = __containerof(mac, emac_esp32_t, parent);
    ESP_GOTO_ON_FALSE(buf && length, ESP_ERR_INVALID_ARG, err, TAG, "can't set buf and length to null");
    uint32_t receive_len = emac_hal_receive_frame(&emac->hal, buf, expected_len, &emac->frames_remain, &emac->free_rx_descriptor);
    /* we need to check the return value in case the buffer size is not enough */
    ESP_GOTO_ON_FALSE(expected_len >= receive_len, ESP_ERR_INVALID_SIZE, err, TAG, "received buffer longer than expected");
    *length = receive_len;
    return ESP_OK;
err:
    *length = expected_len;
    return ret;
}

static void emac_esp32_rx_task(void *arg)
{
    emac_esp32_t *emac = (emac_esp32_t *)arg;
    uint8_t *buffer = NULL;
    while (1) {
        // block indefinitely until got notification from underlay event
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        do {
            /* set max expected frame len */
            uint32_t frame_len = ETH_MAX_PACKET_SIZE;
            buffer = emac_hal_alloc_recv_buf(&emac->hal, &frame_len);
            /* we have memory to receive the frame of maximal size previously defined */
            if (buffer != NULL) {
                uint32_t recv_len = emac_hal_receive_frame(&emac->hal, buffer, EMAC_HAL_BUF_SIZE_AUTO, &emac->frames_remain, &emac->free_rx_descriptor);
                if (recv_len == 0) {
                    ESP_LOGE(TAG, "frame copy error");
                    free(buffer);
                    /* ensure that interface to EMAC does not get stuck with unprocessed frames */
                    emac_hal_flush_recv_frame(&emac->hal, &emac->frames_remain, &emac->free_rx_descriptor);
                } else if (frame_len > recv_len) {
                    ESP_LOGE(TAG, "received frame was truncated");
                    free(buffer);
                } else {
                    ESP_LOGD(TAG, "receive len= %d", recv_len);
                    emac->eth->stack_input(emac->eth, buffer, recv_len);
                }
            /* if allocation failed and there is a waiting frame */
            } else if (frame_len) {
                ESP_LOGE(TAG, "no mem for receive buffer");
                /* ensure that interface to EMAC does not get stuck with unprocessed frames */
                emac_hal_flush_recv_frame(&emac->hal, &emac->frames_remain, &emac->free_rx_descriptor);
            }
#if CONFIG_ETH_SOFT_FLOW_CONTROL
            // we need to do extra checking of remained frames in case there are no unhandled frames left, but pause frame is still undergoing
            if ((emac->free_rx_descriptor < emac->flow_control_low_water_mark) && emac->do_flow_ctrl && emac->frames_remain) {
                emac_ll_pause_frame_enable(emac->hal.ext_regs, true);
            } else if ((emac->free_rx_descriptor > emac->flow_control_high_water_mark) || !emac->frames_remain) {
                emac_ll_pause_frame_enable(emac->hal.ext_regs, false);
            }
#endif
        } while (emac->frames_remain);
    }
    vTaskDelete(NULL);
}

static void emac_esp32_init_smi_gpio(emac_esp32_t *emac)
{
    if (emac->smi_mdc_gpio_num >= 0) {
        /* Setup SMI MDC GPIO */
        gpio_set_direction(emac->smi_mdc_gpio_num, GPIO_MODE_OUTPUT);
        esp_rom_gpio_connect_out_signal(emac->smi_mdc_gpio_num, EMAC_MDC_O_IDX, false, false);
        gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[emac->smi_mdc_gpio_num], PIN_FUNC_GPIO);
    }
    if (emac->smi_mdio_gpio_num >= 0) {
        /* Setup SMI MDIO GPIO */
        gpio_set_direction(emac->smi_mdio_gpio_num, GPIO_MODE_INPUT_OUTPUT);
        esp_rom_gpio_connect_out_signal(emac->smi_mdio_gpio_num, EMAC_MDO_O_IDX, false, false);
        esp_rom_gpio_connect_in_signal(emac->smi_mdio_gpio_num, EMAC_MDI_I_IDX, false);
        gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[emac->smi_mdio_gpio_num], PIN_FUNC_GPIO);
    }
}

static void emac_config_apll_clock(void)
{
    /* apll_freq = xtal_freq * (4 + sdm2 + sdm1/256 + sdm0/65536)/((o_div + 2) * 2) */
    rtc_xtal_freq_t rtc_xtal_freq = rtc_clk_xtal_freq_get();
    switch (rtc_xtal_freq) {
    case RTC_XTAL_FREQ_40M: // Recommended
        /* 50 MHz = 40MHz * (4 + 6) / (2 * (2 + 2) = 50.000 */
        /* sdm0 = 0, sdm1 = 0, sdm2 = 6, o_div = 2 */
        rtc_clk_apll_enable(true, 0, 0, 6, 2);
        break;
    case RTC_XTAL_FREQ_26M:
        /* 50 MHz = 26MHz * (4 + 15 + 118 / 256 + 39/65536) / ((3 + 2) * 2) = 49.999992 */
        /* sdm0 = 39, sdm1 = 118, sdm2 = 15, o_div = 3 */
        rtc_clk_apll_enable(true, 39, 118, 15, 3);
        break;
    case RTC_XTAL_FREQ_24M:
        /* 50 MHz = 24MHz * (4 + 12 + 255 / 256 + 255/65536) / ((2 + 2) * 2) = 49.499977 */
        /* sdm0 = 255, sdm1 = 255, sdm2 = 12, o_div = 2 */
        rtc_clk_apll_enable(true, 255, 255, 12, 2);
        break;
    default: // Assume we have a 40M xtal
        rtc_clk_apll_enable(true, 0, 0, 6, 2);
        break;
    }
}

static esp_err_t emac_esp32_init(esp_eth_mac_t *mac)
{
    esp_err_t ret = ESP_OK;
    emac_esp32_t *emac = __containerof(mac, emac_esp32_t, parent);
    esp_eth_mediator_t *eth = emac->eth;

    /* init gpio used by smi interface */
    emac_esp32_init_smi_gpio(emac);
    ESP_GOTO_ON_ERROR(eth->on_state_changed(eth, ETH_STATE_LLINIT, NULL), err, TAG, "lowlevel init failed");
    /* software reset */
    emac_ll_reset(emac->hal.dma_regs);
    uint32_t to = 0;
    for (to = 0; to < emac->sw_reset_timeout_ms / 10; to++) {
        if (emac_ll_is_reset_done(emac->hal.dma_regs)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_GOTO_ON_FALSE(to < emac->sw_reset_timeout_ms / 10, ESP_ERR_TIMEOUT, err, TAG, "reset timeout");
    /* set smi clock */
    emac_hal_set_csr_clock_range(&emac->hal, esp_clk_apb_freq());
    /* init mac registers by default */
    emac_hal_init_mac_default(&emac->hal);
    /* init dma registers by default */
    emac_hal_init_dma_default(&emac->hal);
    /* get emac address from efuse */
    ESP_GOTO_ON_ERROR(esp_read_mac(emac->addr, ESP_MAC_ETH), err, TAG, "fetch ethernet mac address failed");
    /* set MAC address to emac register */
    emac_hal_set_address(&emac->hal, emac->addr);
#ifdef CONFIG_PM_ENABLE
    esp_pm_lock_acquire(emac->pm_lock);
#endif
    return ESP_OK;
err:
    eth->on_state_changed(eth, ETH_STATE_DEINIT, NULL);
    periph_module_disable(PERIPH_EMAC_MODULE);
    return ret;
}

static esp_err_t emac_esp32_deinit(esp_eth_mac_t *mac)
{
    emac_esp32_t *emac = __containerof(mac, emac_esp32_t, parent);
    esp_eth_mediator_t *eth = emac->eth;
#ifdef CONFIG_PM_ENABLE
    esp_pm_lock_release(emac->pm_lock);
#endif
    emac_hal_stop(&emac->hal);
    eth->on_state_changed(eth, ETH_STATE_DEINIT, NULL);
    return ESP_OK;
}

static esp_err_t emac_esp32_start(esp_eth_mac_t *mac)
{
    emac_esp32_t *emac = __containerof(mac, emac_esp32_t, parent);
    /* reset descriptor chain */
    emac_hal_reset_desc_chain(&emac->hal);
    emac_hal_start(&emac->hal);
    return ESP_OK;
}

static esp_err_t emac_esp32_stop(esp_eth_mac_t *mac)
{
    emac_esp32_t *emac = __containerof(mac, emac_esp32_t, parent);
    esp_err_t ret = ESP_OK;
    int32_t to = 0;
    do {
        if ((ret = emac_hal_stop(&emac->hal)) == ESP_OK) {
            break;
        }
        to += 25;
        esp_rom_delay_us(25);
    } while (to < MAC_STOP_TIMEOUT_US);
    return ret;
}

static esp_err_t emac_esp32_del(esp_eth_mac_t *mac)
{
    emac_esp32_t *emac = __containerof(mac, emac_esp32_t, parent);
    esp_emac_free_driver_obj(emac, emac->hal.descriptors);
    periph_module_disable(PERIPH_EMAC_MODULE);
    return ESP_OK;
}

// To achieve a better performance, we put the ISR always in IRAM
IRAM_ATTR void emac_isr_default_handler(void *args)
{
    emac_hal_context_t *hal = (emac_hal_context_t *)args;
    emac_esp32_t *emac = __containerof(hal, emac_esp32_t, hal);
    BaseType_t high_task_wakeup = pdFALSE;
    uint32_t intr_stat = emac_ll_get_intr_status(hal->dma_regs);
    emac_ll_clear_corresponding_intr(hal->dma_regs, intr_stat);

#if EMAC_LL_CONFIG_ENABLE_INTR_MASK & EMAC_LL_INTR_RECEIVE_ENABLE
    if (intr_stat & EMAC_LL_DMA_RECEIVE_FINISH_INTR) {
        /* notify receive task */
        vTaskNotifyGiveFromISR(emac->rx_task_hdl, &high_task_wakeup);
        if (high_task_wakeup == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
#endif
}

static void esp_emac_free_driver_obj(emac_esp32_t *emac, void *descriptors)
{
    if (emac) {
        if (emac->rx_task_hdl) {
            vTaskDelete(emac->rx_task_hdl);
        }
        if (emac->intr_hdl) {
            esp_intr_free(emac->intr_hdl);
        }
        for (int i = 0; i < CONFIG_ETH_DMA_TX_BUFFER_NUM; i++) {
            free(emac->tx_buf[i]);
        }
        for (int i = 0; i < CONFIG_ETH_DMA_RX_BUFFER_NUM; i++) {
            free(emac->rx_buf[i]);
        }
#ifdef CONFIG_PM_ENABLE
        if (emac->pm_lock) {
            esp_pm_lock_delete(emac->pm_lock);
        }
#endif
        free(emac);
    }
    if (descriptors) {
        free(descriptors);
    }
}

static esp_err_t esp_emac_alloc_driver_obj(const eth_mac_config_t *config, emac_esp32_t **emac_out_hdl, void **out_descriptors)
{
    esp_err_t ret = ESP_OK;
    emac_esp32_t *emac = NULL;
    void *descriptors = NULL;
    if (config->flags & ETH_MAC_FLAG_WORK_WITH_CACHE_DISABLE) {
        emac = heap_caps_calloc(1, sizeof(emac_esp32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    } else {
        emac = calloc(1, sizeof(emac_esp32_t));
    }
    ESP_GOTO_ON_FALSE(emac, ESP_ERR_NO_MEM, err, TAG, "no mem for esp emac object");
    /* alloc memory for ethernet dma descriptor */
    uint32_t desc_size = CONFIG_ETH_DMA_RX_BUFFER_NUM * sizeof(eth_dma_rx_descriptor_t) +
                         CONFIG_ETH_DMA_TX_BUFFER_NUM * sizeof(eth_dma_tx_descriptor_t);
    descriptors = heap_caps_calloc(1, desc_size, MALLOC_CAP_DMA);
    ESP_GOTO_ON_FALSE(descriptors, ESP_ERR_NO_MEM, err, TAG, "no mem for descriptors");
    /* alloc memory for ethernet dma buffer */
    for (int i = 0; i < CONFIG_ETH_DMA_RX_BUFFER_NUM; i++) {
        emac->rx_buf[i] = heap_caps_calloc(1, CONFIG_ETH_DMA_BUFFER_SIZE, MALLOC_CAP_DMA);
        ESP_GOTO_ON_FALSE(emac->rx_buf[i], ESP_ERR_NO_MEM, err, TAG, "no mem for RX DMA buffers");
    }
    for (int i = 0; i < CONFIG_ETH_DMA_TX_BUFFER_NUM; i++) {
        emac->tx_buf[i] = heap_caps_calloc(1, CONFIG_ETH_DMA_BUFFER_SIZE, MALLOC_CAP_DMA);
        ESP_GOTO_ON_FALSE(emac->tx_buf[i], ESP_ERR_NO_MEM, err, TAG, "no mem for TX DMA buffers");
    }
    /* alloc PM lock */
#ifdef CONFIG_PM_ENABLE
    ESP_GOTO_ON_ERROR(esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "emac_esp32", &emac->pm_lock), err, TAG, "create pm lock failed");
#endif
    /* create rx task */
    BaseType_t core_num = tskNO_AFFINITY;
    if (config->flags & ETH_MAC_FLAG_PIN_TO_CORE) {
        core_num = cpu_hal_get_core_id();
    }
    BaseType_t xReturned = xTaskCreatePinnedToCore(emac_esp32_rx_task, "emac_rx", config->rx_task_stack_size, emac,
                           config->rx_task_prio, &emac->rx_task_hdl, core_num);
    ESP_GOTO_ON_FALSE(xReturned == pdPASS, ESP_FAIL, err, TAG, "create emac_rx task failed");

    *out_descriptors = descriptors;
    *emac_out_hdl = emac;
    return ESP_OK;
err:
    esp_emac_free_driver_obj(emac, descriptors);
    return ret;
}

static esp_err_t esp_emac_config_data_interface(const eth_mac_config_t *config, emac_esp32_t *emac)
{
    esp_err_t ret = ESP_OK;
    switch (config->interface) {
    case EMAC_DATA_INTERFACE_MII:
        emac->clock_config = config->clock_config;
        /* MII interface GPIO initialization */
        emac_hal_iomux_init_mii();
        /* Enable MII clock */
        emac_ll_clock_enable_mii(emac->hal.ext_regs);
        break;
    case EMAC_DATA_INTERFACE_RMII:
        // by default, the clock mode is selected at compile time (by Kconfig)
        if (config->clock_config.rmii.clock_mode == EMAC_CLK_DEFAULT) {
#if CONFIG_ETH_RMII_CLK_INPUT
#if CONFIG_ETH_RMII_CLK_IN_GPIO == 0
            emac->clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
            emac->clock_config.rmii.clock_gpio = CONFIG_ETH_RMII_CLK_IN_GPIO;
#else
#error "ESP32 EMAC only support input RMII clock to GPIO0"
#endif // CONFIG_ETH_RMII_CLK_IN_GPIO == 0
#elif CONFIG_ETH_RMII_CLK_OUTPUT
            emac->clock_config.rmii.clock_mode = EMAC_CLK_OUT;
#if CONFIG_ETH_RMII_CLK_OUTPUT_GPIO0
            emac->clock_config.rmii.clock_gpio = 0;
#elif CONFIG_ETH_RMII_CLK_OUT_GPIO
            emac->clock_config.rmii.clock_gpio = CONFIG_ETH_RMII_CLK_OUT_GPIO;
#endif // CONFIG_ETH_RMII_CLK_OUTPUT_GPIO0
#else
#error "Unsupported RMII clock mode"
#endif
        } else {
            emac->clock_config = config->clock_config;
        }
        /* RMII interface GPIO initialization */
        emac_hal_iomux_init_rmii();
        /* If ref_clk is configured as input */
        if (emac->clock_config.rmii.clock_mode == EMAC_CLK_EXT_IN) {
            ESP_GOTO_ON_FALSE(emac->clock_config.rmii.clock_gpio == EMAC_CLK_IN_GPIO,
                              ESP_ERR_INVALID_ARG, err, TAG, "ESP32 EMAC only support input RMII clock to GPIO0");
            emac_hal_iomux_rmii_clk_input();
            emac_ll_clock_enable_rmii_input(emac->hal.ext_regs);
        } else if (emac->clock_config.rmii.clock_mode == EMAC_CLK_OUT) {
            ESP_GOTO_ON_FALSE(emac->clock_config.rmii.clock_gpio == EMAC_APPL_CLK_OUT_GPIO ||
                              emac->clock_config.rmii.clock_gpio == EMAC_CLK_OUT_GPIO ||
                              emac->clock_config.rmii.clock_gpio == EMAC_CLK_OUT_180_GPIO,
                              ESP_ERR_INVALID_ARG, err, TAG, "invalid EMAC clock output GPIO");
            emac_hal_iomux_rmii_clk_ouput(emac->clock_config.rmii.clock_gpio);
            if (emac->clock_config.rmii.clock_gpio == EMAC_APPL_CLK_OUT_GPIO) {
                REG_SET_FIELD(PIN_CTRL, CLK_OUT1, 6);
            }
            /* Enable RMII clock */
            emac_ll_clock_enable_rmii_output(emac->hal.ext_regs);
            emac_config_apll_clock();
        } else {
            ESP_GOTO_ON_FALSE(false, ESP_ERR_INVALID_ARG, err, TAG, "invalid EMAC clock mode");
        }
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_INVALID_ARG, err, TAG, "invalid EMAC Data Interface:%d", config->interface);
    }
err:
    return ret;
}

esp_eth_mac_t *esp_eth_mac_new_esp32(const eth_mac_config_t *config)
{
    esp_err_t ret_code = ESP_OK;
    esp_eth_mac_t *ret = NULL;
    void *descriptors = NULL;
    emac_esp32_t *emac = NULL;
    ESP_GOTO_ON_FALSE(config, NULL, err, TAG, "can't set mac config to null");
    ret_code = esp_emac_alloc_driver_obj(config, &emac, &descriptors);
    ESP_GOTO_ON_FALSE(ret_code == ESP_OK, NULL, err, TAG, "alloc driver object failed");

    /* enable APB to access Ethernet peripheral registers */
    periph_module_enable(PERIPH_EMAC_MODULE);
    /* initialize hal layer driver */
    emac_hal_init(&emac->hal, descriptors, emac->rx_buf, emac->tx_buf);
    /* alloc interrupt */
    if (config->flags & ETH_MAC_FLAG_WORK_WITH_CACHE_DISABLE) {
        ret_code = esp_intr_alloc(ETS_ETH_MAC_INTR_SOURCE, ESP_INTR_FLAG_IRAM,
                                  emac_isr_default_handler, &emac->hal, &(emac->intr_hdl));
    } else {
        ret_code = esp_intr_alloc(ETS_ETH_MAC_INTR_SOURCE, 0,
                                  emac_isr_default_handler, &emac->hal, &(emac->intr_hdl));
    }
    ESP_GOTO_ON_FALSE(ret_code == ESP_OK, NULL, err, TAG, "alloc emac interrupt failed");
    ret_code = esp_emac_config_data_interface(config, emac);
    ESP_GOTO_ON_FALSE(ret_code == ESP_OK, NULL, err_interf, TAG, "config emac interface failed");

    emac->sw_reset_timeout_ms = config->sw_reset_timeout_ms;
    emac->smi_mdc_gpio_num = config->smi_mdc_gpio_num;
    emac->smi_mdio_gpio_num = config->smi_mdio_gpio_num;
    emac->flow_control_high_water_mark = FLOW_CONTROL_HIGH_WATER_MARK;
    emac->flow_control_low_water_mark = FLOW_CONTROL_LOW_WATER_MARK;
    emac->parent.set_mediator = emac_esp32_set_mediator;
    emac->parent.init = emac_esp32_init;
    emac->parent.deinit = emac_esp32_deinit;
    emac->parent.start = emac_esp32_start;
    emac->parent.stop = emac_esp32_stop;
    emac->parent.del = emac_esp32_del;
    emac->parent.write_phy_reg = emac_esp32_write_phy_reg;
    emac->parent.read_phy_reg = emac_esp32_read_phy_reg;
    emac->parent.set_addr = emac_esp32_set_addr;
    emac->parent.get_addr = emac_esp32_get_addr;
    emac->parent.set_speed = emac_esp32_set_speed;
    emac->parent.set_duplex = emac_esp32_set_duplex;
    emac->parent.set_link = emac_esp32_set_link;
    emac->parent.set_promiscuous = emac_esp32_set_promiscuous;
    emac->parent.set_peer_pause_ability = emac_esp32_set_peer_pause_ability;
    emac->parent.enable_flow_ctrl = emac_esp32_enable_flow_ctrl;
    emac->parent.transmit = emac_esp32_transmit;
    emac->parent.receive = emac_esp32_receive;
    return &(emac->parent);

err_interf:
    periph_module_disable(PERIPH_EMAC_MODULE);
err:
    esp_emac_free_driver_obj(emac, descriptors);
    return ret;
}
