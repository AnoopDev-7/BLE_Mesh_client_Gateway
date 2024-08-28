#include "mqtt_app.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "mqtt_client.h"
#include "sdcard.h"
#include "secrets.h"
#include "wifi_connect.h"

static const char *TAG = "MQTT";
static esp_mqtt_client_handle_t client;
static EventGroupHandle_t s_mqtt_event_group;
static const char *mqtt_file = "mqttfile.txt";
TaskHandle_t send_messages_from_file_task_handle;

#define MQTT_CONNECTED_BIT BIT0

static char mqtt_broker_uri[MQTT_URI_MAX_LEN];
static char mqtt_username[MQTT_USERNAME_MAX_LEN];
static char mqtt_password[MQTT_PASSWORD_MAX_LEN];

void mqtt_send_messages_from_file(void *pvParameters) {
  FILE *f = sd_open_file_for_read(mqtt_file);
  if (f) {
    ESP_LOGI(TAG, "Begin sending messages from file");
    char buffer[SD_MAX_LINE_LENGTH];
    char topic[SD_MAX_LINE_LENGTH];
    char data[SD_MAX_LINE_LENGTH];
    while (sd_read_line_from_file(f, buffer, SD_MAX_LINE_LENGTH) == ESP_OK) {
      strcpy(topic, strtok(buffer, "|"));
      strcpy(data, strtok(NULL, "|"));
      mqtt_send_message(buffer, data);
    }
    sd_close_file(f);
    sd_clear_file(mqtt_file);
  }

  ESP_LOGI(TAG, "End sending messages from file");
  vTaskDelete(NULL);
}

/**
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
  ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
  esp_mqtt_event_handle_t event = event_data;
  switch ((esp_mqtt_event_id_t)event_id) {
  case MQTT_EVENT_CONNECTED:
    xEventGroupSetBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
    ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
    if (sd_get_file_size(mqtt_file) > 0) {
      xTaskCreate(mqtt_send_messages_from_file, "msg_file", 4096, NULL, 3, &send_messages_from_file_task_handle);
    }
    break;
  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
    xEventGroupClearBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
    break;
  case MQTT_EVENT_SUBSCRIBED:
    ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
    break;
  case MQTT_EVENT_UNSUBSCRIBED:
    ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
    break;
  case MQTT_EVENT_PUBLISHED:
    ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
    break;
  case MQTT_EVENT_DATA:
    ESP_LOGI(TAG, "MQTT_EVENT_DATA");
    printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
    printf("DATA=%.*s\r\n", event->data_len, event->data);
    break;
  case MQTT_EVENT_ERROR:
    ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
    break;
  default:
    ESP_LOGI(TAG, "Other event id:%d", event->event_id);
    break;
  }
}

static void mqtt_init() {
  esp_mqtt_client_config_t mqtt_cfg = {
      .broker.address.uri = mqtt_broker_uri,
      .credentials.username = mqtt_username,
      .credentials.authentication.password = mqtt_password,
  };

  client = esp_mqtt_client_init(&mqtt_cfg);
  /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
  esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
  if (wifi_is_connected()) {
    esp_mqtt_client_start(client);
  }
}

void on_wifi_status_change(int status) {
  EventBits_t bits = xEventGroupGetBits(s_mqtt_event_group);
  if (status) {
    if (!(bits & MQTT_CONNECTED_BIT)) {
      ESP_LOGI(TAG, "Starting MQTT client");
      mqtt_init();
    }
  } else {
    if (bits & MQTT_CONNECTED_BIT) {
      ESP_LOGI(TAG, "Stopping MQTT client");
      esp_mqtt_client_destroy(client);
    }
  }
}

void mqtt_send_message(const char *topic, const char *data) {
  if (s_mqtt_event_group) {
    EventBits_t bits = xEventGroupGetBits(s_mqtt_event_group);
    if (bits & MQTT_CONNECTED_BIT) {
      esp_mqtt_client_publish(client, topic, data, 0, 1, 0);
      return;
    }
  }
  char buffer[SD_MAX_LINE_LENGTH];
  snprintf(buffer, SD_MAX_LINE_LENGTH, "%s|%s", topic, data);
  sd_append_to_file(mqtt_file, buffer);
}

void mqtt_app_start(const char *broker_uri, size_t broker_uri_len, const char *username, size_t username_len,
                    const char *password, size_t password_len) {
  snprintf(mqtt_broker_uri, broker_uri_len, "%s", broker_uri);
  snprintf(mqtt_username, username_len, "%s", username);
  snprintf(mqtt_password, password_len, "%s", password);

  s_mqtt_event_group = xEventGroupCreate();
  mqtt_init();
  wifi_register_on_status_change_callback(on_wifi_status_change);
}
