/*
 * ESP32 ULP Rain Gauge Application
  *
 * Copyright (C) 2018 KIMATA Tetsuya <kimata@green-rabbit.net>
 *
 * This program is free software ; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2.
 */

#include "nvs_flash.h"
#include "soc/rtc.h"
#include "soc/sens_reg.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "lwip/sockets.h"

#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_task_wdt.h"
#include "esp_adc_cal.h"
#include "esp_deep_sleep.h"
#include "esp32/ulp.h"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "cJSON.h"

#include "ulp_main.h"

#include "wifi_config.h"
// wifi_config.h should define followings.
// #define WIFI_SSID "XXXXXXXX"            // WiFi SSID
// #define WIFI_PASS "XXXXXXXX"            // WiFi Password

////////////////////////////////////////////////////////////
// Configuration
#define FLUENTD_IP      "192.168.2.20"  // IP address of Fluentd
#define FLUENTD_PORT    8888            // Port of FLuentd
#define FLUENTD_TAG     "/test"       // Fluentd tag

#define WIFI_HOSTNAME   "ESP32-rain"    // module's hostname
#define SENSE_INTERVAL  60              // sensing interval
#define SENSE_BUF_COUNT 5               // buffering count
#define SENSE_BUF_MAX   60              // max buffering count

#define ADC_VREF        1128            // ADC calibration data

////////////////////////////////////////////////////////////
const gpio_num_t GAUGE_PIN    = GPIO_NUM_33;

#define BATTERY_ADC_CH  ADC1_CHANNEL_4  // GPIO 32
#define BATTERY_ADC_SAMPLE  33
#define BATTERY_ADC_DIV  1

////////////////////////////////////////////////////////////
#define WIFI_CONNECT_TIMEOUT 10

typedef struct sense_data {
    uint16_t rainfall;
} sense_data_t;

extern const uint8_t ulp_main_bin_start[] asm("_binary_ulp_main_bin_start");
extern const uint8_t ulp_main_bin_end[]   asm("_binary_ulp_main_bin_end");

#define TAG "ulp_rain_gauge"
#define EXPECTED_RESPONSE "HTTP/1.1 200 OK"
#define REQUEST "POST http://" FLUENTD_IP FLUENTD_TAG " HTTP/1.0\r\n" \
    "Content-Type: application/x-www-form-urlencoded\r\n" \
    "Content-Length: %d\r\n" \
    "\r\n" \
    "json=%s"

SemaphoreHandle_t wifi_conn_done = NULL;

//////////////////////////////////////////////////////////////////////
// Sensor Function
int cmp_volt(const uint32_t *a, const uint32_t *b)
{
    if (*a < *b) {
        return -1;
    } else if (*a == *b) {
        return 0;
    } else {
        return 1;
    }
}

uint32_t get_battery_voltage(void)
{
    uint32_t ad_volt_list[BATTERY_ADC_SAMPLE];
    esp_adc_cal_characteristics_t characteristics;

    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(BATTERY_ADC_CH, ADC_ATTEN_11db);
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, ADC_VREF,
                             &characteristics);

    for (uint32_t i = 0; i < BATTERY_ADC_SAMPLE; i++) {
        ESP_ERROR_CHECK(esp_adc_cal_get_voltage(BATTERY_ADC_CH,
                                                &characteristics, ad_volt_list + i));
    }

    qsort(ad_volt_list, BATTERY_ADC_SAMPLE, sizeof(uint32_t),
          (int (*)(const void *, const void *))cmp_volt);

    // mean value
    return ad_volt_list[BATTERY_ADC_SAMPLE >> 1] * BATTERY_ADC_DIV;
}

//////////////////////////////////////////////////////////////////////
// Fluentd Function
static int connect_server()
{
    struct sockaddr_in server;
    int sock;

    sock = socket(AF_INET, SOCK_STREAM, 0);

    server.sin_family = AF_INET;
    server.sin_port = htons(FLUENTD_PORT);
    server.sin_addr.s_addr = inet_addr(FLUENTD_IP);

    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) != 0) {
        ESP_LOGE(TAG, "FLUENTD CONNECT FAILED errno=%d", errno);
        return -1;
    }
    ESP_LOGI(TAG, "FLUENTD CONNECT SUCCESS");

    return sock;
}

static cJSON *sense_json(uint32_t battery_volt, wifi_ap_record_t *ap_record,
                         uint32_t wifi_con_msec)
{
    sense_data_t *sense_data = (sense_data_t *)&ulp_sense_data;

    cJSON *root = cJSON_CreateArray();

    for (uint32_t i = 0; i < ulp_sense_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "rain", sense_data[i].rainfall * 0.5);
        cJSON_AddStringToObject(item, "hostname", WIFI_HOSTNAME);
        cJSON_AddNumberToObject(item, "self_time", SENSE_INTERVAL * i); // negative offset

        if (i == 0) {
            cJSON_AddNumberToObject(item, "battery", battery_volt);
            cJSON_AddNumberToObject(item, "wifi_ch", ap_record->primary);
            cJSON_AddNumberToObject(item, "wifi_rssi", ap_record->rssi);
            cJSON_AddNumberToObject(item, "wifi_con_msec", wifi_con_msec);
        }

        cJSON_AddItemToArray(root, item);
    }

    return root;
}

static void process_sense_data(uint32_t connect_msec, uint32_t battery_volt)
{
    wifi_ap_record_t ap_record;
    char buffer[sizeof(EXPECTED_RESPONSE)];

    ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&ap_record));

    int sock = connect_server();
    if (sock == -1) {
        return;
    }

    cJSON *json = sense_json(battery_volt, &ap_record, connect_msec);
    char *json_str = cJSON_PrintUnformatted(json);

    do {
        if (dprintf(sock, REQUEST, strlen("json=") + strlen(json_str), json_str) < 0) {
            ESP_LOGE(TAG, "FLUENTD POST FAILED");
            break;
        }

        bzero(buffer, sizeof(buffer));
        read(sock, buffer, sizeof(buffer)-1);

        if (strcmp(buffer, EXPECTED_RESPONSE) != 0) {
            ESP_LOGE(TAG, "FLUENTD POST FAILED");
            break;
        }
        ESP_LOGI(TAG, "FLUENTD POST SUCCESSFUL");
    } while (0);

    close(sock);
    cJSON_Delete(json);
}

//////////////////////////////////////////////////////////////////////
// Wifi Function
static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        ESP_LOGI(TAG, "SYSTEM_EVENT_STA_START");
        ESP_ERROR_CHECK(tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, WIFI_HOSTNAME));
        ESP_ERROR_CHECK(esp_wifi_connect());
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        ESP_LOGI(TAG, "SYSTEM_EVENT_STA_GOT_IP");
        xSemaphoreGive(wifi_conn_done);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        ESP_LOGI(TAG, "SYSTEM_EVENT_STA_DISCONNECTED");
        xSemaphoreGive(wifi_conn_done);
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void init_wifi()
{
    esp_err_t ret;

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    tcpip_adapter_init();

    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

#ifdef WIFI_SSID
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    wifi_config_t wifi_config_cur;
    ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &wifi_config_cur));

    if (strcmp((const char *)wifi_config_cur.sta.ssid, (const char *)wifi_config.sta.ssid) ||
        strcmp((const char *)wifi_config_cur.sta.password, (const char *)wifi_config.sta.password)) {
        ESP_LOGI(TAG, "SAVE WIFI CONFIG");
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    }
#endif
}

static esp_err_t wifi_connect()
{
    xSemaphoreTake(wifi_conn_done, portMAX_DELAY);
    ESP_ERROR_CHECK(esp_wifi_start());
    if (xSemaphoreTake(wifi_conn_done, 10000 / portTICK_RATE_MS) == pdTRUE) {
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "WIFI CONNECT TIMECOUT")
        return ESP_FAIL;
    }
}

static void wifi_disconnect()
{
    ESP_ERROR_CHECK(esp_wifi_disconnect());
    ESP_ERROR_CHECK(esp_wifi_stop());
}

//////////////////////////////////////////////////////////////////////
// ULP Function
static void init_ulp_program()
{
    gpio_num_t gpio_num = GAUGE_PIN;
    ESP_ERROR_CHECK(rtc_gpio_init(GAUGE_PIN));
    rtc_gpio_set_direction(gpio_num, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis(gpio_num);
    rtc_gpio_pullup_dis(gpio_num);
    rtc_gpio_hold_en(gpio_num);

    ESP_ERROR_CHECK(
        ulp_load_binary(
            0, ulp_main_bin_start,
            (ulp_main_bin_end - ulp_main_bin_start) / sizeof(uint32_t)
        )
    );

    ulp_gpio_num = 8;
}

//////////////////////////////////////////////////////////////////////
// Sensing
static bool sense_data_all_zero()
{
    sense_data_t *sense_data = (sense_data_t *)&ulp_sense_data;

    for (uint32_t i = 0; i < ulp_sense_count; i++) {
        if (sense_data[i].rainfall != 0) {
            return false;
        }
    }
    return true;
}

static bool handle_ulp_sense_data()
{
    uint16_t edge_count;

    edge_count = ulp_edge_count;
    ulp_edge_count = 0;

    sense_data_t *sense_data = (sense_data_t *)&ulp_sense_data;

    sense_data[ulp_sense_count++].rainfall = edge_count;
    ulp_edge_count = 0;

    // check if it began to rain
    if ((edge_count != 0) && ulp_predecessing_zero) {
        ulp_predecessing_zero = false;
        return true;
    }
    // check if it is raining and buffer is filled
    if ((ulp_sense_count >= SENSE_BUF_COUNT) && (!sense_data_all_zero())) {
        ulp_predecessing_zero = false;
        return true;
    }

    // check if the buffer is full
    if (ulp_sense_count >= SENSE_BUF_MAX) {
        ulp_predecessing_zero = sense_data_all_zero();
        return true;
    }

    return false;
}

//////////////////////////////////////////////////////////////////////
void app_main()
{
    uint32_t time_start;
    uint32_t battery_volt;
    uint32_t connect_msec;

    vSemaphoreCreateBinary(wifi_conn_done);

    battery_volt = get_battery_voltage();

    esp_log_level_set("wifi", ESP_LOG_ERROR);

    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
        if (handle_ulp_sense_data()) {
            ESP_LOGI(TAG, "Send to fluentd");
            time_start = xTaskGetTickCount();
            init_wifi();
            if (wifi_connect() == ESP_OK) {
                connect_msec = (xTaskGetTickCount() - time_start) * portTICK_PERIOD_MS;
                process_sense_data(connect_msec, battery_volt);
            }
            wifi_disconnect();
            ulp_sense_count = 0;
        }
    } else {
        init_ulp_program();
        ESP_ERROR_CHECK(ulp_run((&ulp_entry - RTC_SLOW_MEM) / sizeof(uint32_t)));
    }

    ESP_LOGI(TAG, "Go to sleep");
    ESP_ERROR_CHECK(esp_deep_sleep_enable_timer_wakeup(SENSE_INTERVAL * 1000000LL));
    esp_deep_sleep_start();
}
