#include "driver_mpu6500_basic.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/err.h"
#include "lwip/sys.h"

#include "info.h"

#define BUZZER GPIO_NUM_4
#define BUTTON GPIO_NUM_17

uint8_t res;
uint32_t i;
mpu6500_address_t addr;
float sum_of_accel;
uint8_t (*g_gpio_irq)(void) = NULL;
static float gs_accel_g[3];
static float gs_gyro_dps[3];
uint16_t len;
bool fall;
bool trending_down;
uint8_t count;
TickType_t drop;

#define ESP_MAXIMUM_RETRY 5
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#define ESP_H2E_IDENTIFIER ""

static EventGroupHandle_t s_wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char *TAG = "wifi";
static const char *MQTAG = "mqtt";

static int s_retry_num = 0;

static void log_error_if_nonzero(const char *message, int error_code) {
  if (error_code != 0) {
    ESP_LOGE(MQTAG, "Last error %s: 0x0x", message, error_code);
  }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
  ESP_LOGD(TAG,
           "Event dispatched from event loop base=%s, event_id=%" PRIi32 "",
           base, event_id);
  esp_mqtt_event_handle_t event = event_data;
  esp_mqtt_client_handle_t client = event->client;
  int msg_id;
  switch ((esp_mqtt_event_id_t)event_id) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
    msg_id = esp_mqtt_client_publish(client, FEED, "FALL", 4, 1, 0);
    ESP_LOGI(MQTAG, "sent publish successful, msg_id=%d", msg_id);
    break;

  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGI(MQTAG, "MQTT_EVENT_DISCONNECTED");
    break;

  case MQTT_EVENT_PUBLISHED:
    ESP_LOGI(MQTAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
    break;
  case MQTT_EVENT_DATA:
    ESP_LOGI(MQTAG, "MQTT_EVENT_DATA");
    printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
    printf("DATA=%.*s\r\n", event->data_len, event->data);
    break;
  case MQTT_EVENT_ERROR:
    ESP_LOGI(MQTAG, "MQTT_EVENT_ERROR");
    if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
      log_error_if_nonzero("reported from esp-tls",
                           event->error_handle->esp_tls_last_esp_err);
      log_error_if_nonzero("reported from tls stack",
                           event->error_handle->esp_tls_stack_err);
      log_error_if_nonzero("captured as transport's socket errno",
                           event->error_handle->esp_transport_sock_errno);
      ESP_LOGI(MQTAG, "Last errno string (%s)",
               strerror(event->error_handle->esp_transport_sock_errno));
    }
    break;
  default:
    ESP_LOGI(MQTAG, "Other event id:%d", event->event_id);
    break;
  }
}

static void mqtt_app_start(void) {
  esp_mqtt_client_config_t mqtt_cfg = {
      .broker.address.uri = BROKER_URI,
      .credentials.username = BROKER_USER,
      .credentials.authentication.password = BROKER_PASS,
  };

  esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
  /* The last argument may be used to pass data to the event handler, in this
   * example mqtt_event_handler */
  esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event_handler,
                                 NULL);
  esp_mqtt_client_start(client);
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (s_retry_num < ESP_MAXIMUM_RETRY) {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(TAG, "retry to connect to the AP");
    } else {
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
    ESP_LOGI(TAG, "connect to the AP fail");
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

void wifi_init_sta(void) {
  s_wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());

  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

  wifi_config_t wifi_config = {
      .sta =
          {
              .ssid = WIFI_SSID,
              .password = WIFI_PASS,
              /* Authmode threshold resets to WPA2 as default if password
               * matches WPA2 standards (password len => 8). If you want to
               * connect the device to deprecated WEP/WPA networks, Please set
               * the threshold value to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set
               * the password with length and format matching to
               * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
               */
              .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
              .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
              .sae_h2e_identifier = ESP_H2E_IDENTIFIER,
#ifdef CONFIG_ESP_WIFI_WPA3_COMPATIBLE_SUPPORT
              .disable_wpa3_compatible_mode = 0,
#endif
          },
  };
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "wifi_init_sta finished.");

  /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or
   * connection failed for the maximum number of re-tries (WIFI_FAIL_BIT). The
   * bits are set by event_handler() (see above) */
  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                         (WIFI_CONNECTED_BIT | WIFI_FAIL_BIT),
                                         pdFALSE, pdFALSE, portMAX_DELAY);

  /* xEventGroupWaitBits() returns the bits before the call returned, hence we
   * can test which event actually happened. */
  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "connected to ap SSID:%s password:%s", WIFI_SSID, WIFI_PASS);
  } else if (bits & WIFI_FAIL_BIT) {
    ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s", WIFI_SSID,
             WIFI_PASS);
  } else {
    ESP_LOGE(TAG, "UNEXPECTED EVENT");
  }
}

void app_main() {
  gpio_set_direction(BUTTON, GPIO_MODE_INPUT);
  gpio_set_pull_mode(BUTTON, GPIO_PULLUP_ONLY);
  gpio_set_direction(BUZZER, GPIO_MODE_OUTPUT);
  gpio_set_level(BUZZER, 0);
  addr = MPU6500_ADDRESS_AD0_LOW;
  if (mpu6500_basic_init(MPU6500_INTERFACE_IIC, addr) != 0) {
    g_gpio_irq = NULL;
    // (void)gpio_interrupt_deinit();

    return;
  }

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  esp_log_level_set("mqtt:", ESP_LOG_VERBOSE);
  esp_log_level_set("wifi:", ESP_LOG_VERBOSE);
  ESP_ERROR_CHECK(esp_netif_init());

  fall = false;
  count = 0;
  while (!fall) {
    taskYIELD();
    len = 128;

    if (mpu6500_basic_read(gs_accel_g, gs_gyro_dps) != 0) {
      return;
    }

    sum_of_accel =
        (fabsf(gs_accel_g[0]) + fabsf(gs_accel_g[1]) + fabsf(gs_accel_g[2]));
    printf("Sum Vec: %2.2fg\n", sum_of_accel);

    trending_down = sum_of_accel < 0.95 ? true : false;
    if (trending_down) {
      count++;
      if (count == 9) {
        drop = pdTICKS_TO_MS(xTaskGetTickCount());
      }
    } else {
      count = 0;
      const TickType_t currTime = pdTICKS_TO_MS(xTaskGetTickCount());
      if (currTime - drop < 300 && sum_of_accel > 2.5) {
        fall = true;
      }
    }
  }

  printf("FALL DETECTED\n");
  // when there's been a detection there should be a grace period of like 5
  // seconds.

  const TickType_t immTime = pdTICKS_TO_MS(xTaskGetTickCount());
  TickType_t currTime = pdTICKS_TO_MS(xTaskGetTickCount());
  gpio_set_level(BUZZER,
                 1); // sound buzzer to ensure actual fall, not bad detection.
  bool escaped = false;
  int currBtn = 1;
  while (currTime - immTime < 5000) {
    taskYIELD();
    printf("awaiting...%lu/%lu\n", (currTime - immTime), (uint32_t)5000);
    currTime = pdTICKS_TO_MS(xTaskGetTickCount());
    currBtn = gpio_get_level(BUTTON);
    if (currBtn == 0) {
      // btn press
      escaped = true;
      break;
    }
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
  printf("last button press state: %d", currBtn);
  gpio_set_level(BUZZER, 0);
  if (!escaped) {
    // only init radios if we need to to save pwr!
    wifi_init_sta();
    mqtt_app_start(); // this will pub
  }

  fflush(stdout);
  esp_restart();
}
