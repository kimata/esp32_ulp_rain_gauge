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
#include "driver/adc.h"
#include "lwip/sockets.h"

#include "esp32/ulp.h"

#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "esp_log.h"

#include "esp_adc_cal.h"
#include "esp_event.h"
#include "esp_sleep.h"
#include "esp_spi_flash.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "cJSON.h"

#include "ulp_main.h"

#include "wifi_config.h"
// wifi_config.h should define followings.
// #define WIFI_SSID "XXXXXXXX"            // WiFi SSID
// #define WIFI_PASS "XXXXXXXX"            // WiFi Password

/* #define ADC_CALIB_MODE */

////////////////////////////////////////////////////////////
// Configuration
#define FLUENTD_IP      "192.168.0.10"  // IP address of Fluentd
#define FLUENTD_PORT    8888            // Port of FLuentd
#define FLUENTD_TAG     "/sensor"       // Fluentd tag

#define WIFI_HOSTNAME   "ESP32-rain"    // module's hostname
#define SENSE_INTERVAL  60              // sensing interval
#define SENSE_BUF_COUNT 5               // buffering count
#define SENSE_BUF_FULL  30              // full buffering count
#define SENSE_BUF_MAX   120             // max buffering count

#define COUNT_MM        0.5             // 1 count = 0.5mm
#define RAINFALL_MAX_PH 200             // maximum rainfall per hour

#define ADC_VREF        1100            // ADC calibration data

////////////////////////////////////////////////////////////
const gpio_num_t GAUGE_PIN    = GPIO_NUM_25;
const uint16_t   GAUGE_PIN_ULP= 6;

#define BATTERY_ADC_CH  ADC1_CHANNEL_5  // GPIO 33
#define BATTERY_ADC_SAMPLE  33
#define BATTERY_ADC_DIV     2.015

#define BATTERY_THRESHOLD 2400              // battery threshold

////////////////////////////////////////////////////////////
#define WIFI_CONNECT_TIMEOUT 5

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
// Error Handling
static void _error_check_failed(esp_err_t rc, const char *file, int line,
                                const char *function, const char *expression)
{
    ets_printf("ESP_ERROR_CHECK failed: esp_err_t 0x%x", rc);
#ifdef CONFIG_ESP_ERR_TO_NAME_LOOKUP
    ets_printf(" (%s)", esp_err_to_name(rc));
#endif //CONFIG_ESP_ERR_TO_NAME_LOOKUP
    ets_printf(" at 0x%08x\n", (intptr_t)__builtin_return_address(0) - 3);
    if (spi_flash_cache_enabled()) { // strings may be in flash cache
        ets_printf("file: \"%s\" line %d\nfunc: %s\nexpression: %s\n", file, line, function, expression);
    }
}

#define ERROR_RETURN(x, fail) do {                                      \
        esp_err_t __err_rc = (x);                                       \
        if (__err_rc != ESP_OK) {                                       \
            _error_check_failed(__err_rc, __FILE__, __LINE__,           \
                                __ASSERT_FUNC, #x);                     \
            return fail;                                                \
        }                                                               \
    } while(0);

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
    uint32_t voltage;

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
    voltage =  ad_volt_list[BATTERY_ADC_SAMPLE >> 1] * BATTERY_ADC_DIV;

    ESP_LOGI(TAG, "BATTERY: %.2f V", voltage / 1000.0);

    return voltage;
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

static cJSON *sense_json(uint32_t battery_volt, wifi_ap_record_t *ap_info,
                         uint32_t wifi_con_msec)
{
    sense_data_t *sense_data = (sense_data_t *)&ulp_sense_data;

    cJSON *root = cJSON_CreateArray();

    for (uint32_t i = 0; i < ulp_sense_count; i++) {
        cJSON *item = cJSON_CreateObject();
        uint32_t index = ulp_sense_count - i - 1;
        float rainfall = sense_data[i].rainfall * COUNT_MM;

        if (rainfall > (RAINFALL_MAX_PH/60)) {
          rainfall = RAINFALL_MAX_PH / 60.0;
        }

        cJSON_AddNumberToObject(item, "rain", rainfall);
        cJSON_AddStringToObject(item, "hostname", WIFI_HOSTNAME);
        cJSON_AddNumberToObject(item, "self_time", SENSE_INTERVAL * index); // negative offset

        if (index == 0) {
            cJSON_AddNumberToObject(item, "battery", battery_volt);
            cJSON_AddNumberToObject(item, "wifi_ch", ap_info->primary);
            cJSON_AddNumberToObject(item, "wifi_rssi", ap_info->rssi);
            cJSON_AddNumberToObject(item, "wifi_con_msec", wifi_con_msec);
            if (ulp_sense_count < SENSE_BUF_FULL) {
                cJSON_AddNumberToObject(item, "retry", 0);
            } else {
                cJSON_AddNumberToObject(item, "retry", ulp_sense_count - SENSE_BUF_FULL);
            }
        }

        cJSON_AddItemToArray(root, item);
    }

    return root;
}

static bool process_sense_data(uint32_t battery_volt, wifi_ap_record_t *ap_info, uint32_t connect_msec)
{
    char buffer[sizeof(EXPECTED_RESPONSE)];
    bool result = false;

    int sock = connect_server();
    if (sock == -1) {
        return false;
    }

    cJSON *json = sense_json(battery_volt, ap_info, connect_msec);
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

        result = true;
    } while (0);

    close(sock);
    cJSON_Delete(json);

    return result;
}

//////////////////////////////////////////////////////////////////////
// Wifi Function
static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    static uint32_t retry = 0;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        retry++;
        esp_wifi_connect();
        ESP_LOGI(TAG, "Retry to connect to the AP (n=%d)", retry);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        retry = 0;
        xSemaphoreGive(wifi_conn_done);
    }
}

static bool wifi_init()
{
    esp_err_t ret;

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ERROR_RETURN(nvs_flash_erase(), false);
        ret = nvs_flash_init();
    }
    ERROR_RETURN(ret, false);

    ERROR_RETURN(esp_netif_init(), false);
    ERROR_RETURN(esp_event_loop_create_default(), false);

    esp_netif_t *esp_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ERROR_RETURN(esp_wifi_init(&cfg), false);
    ERROR_RETURN(esp_wifi_set_mode(WIFI_MODE_STA), false);
    ERROR_RETURN(esp_wifi_set_storage(WIFI_STORAGE_RAM), false);

#ifdef WIFI_SSID
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    wifi_config_t wifi_config_cur;
    ERROR_RETURN(esp_wifi_get_config(WIFI_IF_STA, &wifi_config_cur), false);

    if (strcmp((const char *)wifi_config_cur.sta.ssid, (const char *)wifi_config.sta.ssid) ||
        strcmp((const char *)wifi_config_cur.sta.password, (const char *)wifi_config.sta.password)) {
        ESP_LOGI(TAG, "SAVE WIFI CONFIG");
        ERROR_RETURN(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config), false);
    }
#endif

    ERROR_RETURN(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            &event_handler, NULL),
                 false);

    ERROR_RETURN(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            &event_handler, NULL),
                 false);

    ERROR_RETURN(esp_netif_set_hostname(esp_netif, WIFI_HOSTNAME), false);

    return true;
}

static bool wifi_connect(wifi_ap_record_t *ap_info)
{
    xSemaphoreTake(wifi_conn_done, portMAX_DELAY);

    ERROR_RETURN(esp_wifi_start(), false);
    if (xSemaphoreTake(wifi_conn_done, WIFI_CONNECT_TIMEOUT * 1000 / portTICK_RATE_MS) == pdTRUE) {
        ERROR_RETURN(esp_wifi_sta_get_ap_info(ap_info), false);
        return true;
    } else {
        ESP_LOGE(TAG, "WIFI CONNECT TIMECOUT");
        return false;
    }
}

static bool wifi_stop()
{
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler);
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler);

    esp_wifi_disconnect();
    esp_wifi_stop();

    return true;
}

//////////////////////////////////////////////////////////////////////
// ULP Function
static void init_ulp_program()
{
    ESP_ERROR_CHECK(
        ulp_load_binary(
            0, ulp_main_bin_start,
            (ulp_main_bin_end - ulp_main_bin_start) / sizeof(uint32_t)
        )
    );
    ulp_gpio_num = GAUGE_PIN_ULP;
    ulp_predecessing_zero = true;

    ESP_ERROR_CHECK(rtc_gpio_init(GAUGE_PIN));
    ESP_ERROR_CHECK(rtc_gpio_set_direction(GAUGE_PIN, RTC_GPIO_MODE_INPUT_ONLY));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(GAUGE_PIN));
    ESP_ERROR_CHECK(rtc_gpio_pullup_dis(GAUGE_PIN));
    ESP_ERROR_CHECK(rtc_gpio_hold_en(GAUGE_PIN));

    REG_SET_FIELD(SENS_ULP_CP_SLEEP_CYC0_REG, SENS_SLEEP_CYCLES_S0,
                  rtc_clk_slow_freq_get_hz());

    // MEMO: EDP-IDF のサンプル(ulp_example_main.c)にある下記を，
    // 両端子共に open の手元の ESP32 に行うと消費電流が増えた．詳細不明．
    /* GPIO12: select flash voltage */
    /* GPIO15: suppress boot messages */
    /* rtc_gpio_isolate(GPIO_NUM_12); */
    /* rtc_gpio_isolate(GPIO_NUM_15); */
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

    ESP_LOGI(TAG, "SENSE_COUNT: %d / %d", ulp_sense_count, SENSE_BUF_FULL);
    if (edge_count != 0) {
        ESP_LOGW(TAG, "EDGE_COUNT = %d", edge_count);
    }

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
    if (ulp_sense_count >= SENSE_BUF_FULL) {
        ulp_predecessing_zero = sense_data_all_zero();
        return true;
    }

    return false;
}


//////////////////////////////////////////////////////////////////////
void app_main()
{
    wifi_ap_record_t ap_info;
    uint32_t time_start;
    uint32_t battery_volt;
    uint32_t connect_msec;

#ifdef ADC_CALIB_MODE
    ESP_ERROR_CHECK(adc2_vref_to_gpio(GPIO_NUM_25));
#else
    vSemaphoreCreateBinary(wifi_conn_done);

    battery_volt = get_battery_voltage();

    esp_log_level_set("wifi", ESP_LOG_ERROR);

    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
        if (handle_ulp_sense_data()) {
            bool status = false;
            ESP_LOGI(TAG, "Send to fluentd");
            time_start = xTaskGetTickCount();

            if (wifi_init() && wifi_connect(&ap_info)) {
                connect_msec = (xTaskGetTickCount() - time_start) * portTICK_PERIOD_MS;
                status = process_sense_data(battery_volt, &ap_info, connect_msec);
            }
            wifi_stop();

            if (status) {
                ulp_sense_count = 0;
            } else if (ulp_sense_count >= SENSE_BUF_MAX) {
                // count has reached max
                ulp_sense_count = 0;
            }
        }
    } else {
        init_ulp_program();
        ulp_watch_state = 2;
        ulp_sense_count = 0;

        // run ulp program every 30ms
        ulp_set_wakeup_period(0, 1000*30); // 30ms
        ESP_ERROR_CHECK(ulp_run((&ulp_entry - RTC_SLOW_MEM) / sizeof(uint32_t)));
    }

    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(SENSE_INTERVAL * 1000000LL));

    ESP_LOGI(TAG, "Go to sleep");
    vTaskDelay(10 / portTICK_RATE_MS); // wait 10ms for flush UART

    esp_deep_sleep_start();
#endif
}
