#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>

#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_generic_model_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_gatt_common_api.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <sys/time.h>

#include "ble_mesh_init.h"
#include "ble_mesh_nvs.h"
#include "mqtt_app.h"
#include "mqtt_client.h"
#include "sdcard.h"
#include "secrets.h"
#include "wifi_connect.h"

#include "sdkconfig.h"

#define TAG "GATEWAY"

#define CID_ESP 0x02E5

#define ESP_BLE_MESH_WIFI_CONFIG_MODEL_ID_CLIENT 0x0000
#define ESP_BLE_MESH_WIFI_CONFIG_MODEL_ID_SERVER 0x0001

#define ESP_BLE_MESH_WIFI_CONFIG_MODEL_OP_SEND ESP_BLE_MESH_MODEL_OP_3(0x00, CID_ESP)
#define ESP_BLE_MESH_WIFI_CONFIG_MODEL_OP_STATUS ESP_BLE_MESH_MODEL_OP_3(0x01, CID_ESP)

#define ESP_BLE_MESH_MQTT_CONFIG_MODEL_ID_CLIENT 0x0002
#define ESP_BLE_MESH_MQTT_CONFIG_MODEL_ID_SERVER 0x0003

#define ESP_BLE_MESH_MQTT_CONFIG_MODEL_OP_SEND ESP_BLE_MESH_MODEL_OP_3(0x02, CID_ESP)
#define ESP_BLE_MESH_MQTT_CONFIG_MODEL_OP_STATUS ESP_BLE_MESH_MODEL_OP_3(0x03, CID_ESP)

static nvs_handle_t NVS_HANDLE;

static char wifi_ssid_from_ble[WIFI_SSID_MAX_LEN];
static char wifi_pswd_from_ble[WIFI_PSWD_MAX_LEN];
static char mqtt_uri[MQTT_URI_MAX_LEN];
static char mqtt_username[MQTT_USERNAME_MAX_LEN];
static char mqtt_password[MQTT_PASSWORD_MAX_LEN];

static uint8_t dev_uuid[16] = {0xdd, 0xdd};

static esp_ble_mesh_client_t onoff_client;

static esp_ble_mesh_cfg_srv_t config_server = {
    .relay = ESP_BLE_MESH_RELAY_DISABLED,
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_ENABLED,
    .default_ttl = 7,
    /* 3 transmissions with 20ms interval */
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
};

ESP_BLE_MESH_MODEL_PUB_DEFINE(onoff_cli_pub, 2 + 1, ROLE_NODE);

static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
    ESP_BLE_MESH_MODEL_GEN_ONOFF_CLI(&onoff_cli_pub, &onoff_client),
};

static esp_ble_mesh_model_op_t wifi_config_model_op[] = {
    ESP_BLE_MESH_MODEL_OP(ESP_BLE_MESH_WIFI_CONFIG_MODEL_OP_SEND, 2),
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_op_t mqtt_config_model_op[] = {
    ESP_BLE_MESH_MODEL_OP(ESP_BLE_MESH_MQTT_CONFIG_MODEL_OP_SEND, 2),
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_t vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(CID_ESP, ESP_BLE_MESH_WIFI_CONFIG_MODEL_ID_SERVER, wifi_config_model_op, NULL, NULL),
    ESP_BLE_MESH_VENDOR_MODEL(CID_ESP, ESP_BLE_MESH_MQTT_CONFIG_MODEL_ID_SERVER, mqtt_config_model_op, NULL, NULL),
};

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, vnd_models),
};

static esp_ble_mesh_comp_t composition = {
    .cid = CID_ESP,
    .elements = elements,
    .element_count = ARRAY_SIZE(elements),
};

static esp_ble_mesh_prov_t provision = {
    .uuid = dev_uuid,
    // No OOB
    .output_size = 0,
    .output_actions = 0,
};

static esp_err_t store_wifi_config(const char *ssid, const char *password) {
  esp_err_t err = nvs_open("wifi", NVS_READWRITE, &NVS_HANDLE);
  if (err != ESP_OK) {
    return ESP_FAIL;
  }
  nvs_set_str(NVS_HANDLE, "ssid", ssid);
  nvs_set_str(NVS_HANDLE, "password", password);
  err = nvs_commit(NVS_HANDLE);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "could not commit wifi config");
    return ESP_FAIL;
  }
  nvs_close(NVS_HANDLE);
  return ESP_OK;
}
static esp_err_t restore_wifi_config(void) {
  esp_err_t err = ESP_OK;
  err = nvs_open("wifi", NVS_READWRITE, &NVS_HANDLE);
  if (err != ESP_OK) {
    return ESP_FAIL;
  }
  size_t size = sizeof(wifi_ssid_from_ble);
  err = nvs_get_str(NVS_HANDLE, "ssid", wifi_ssid_from_ble, &size);
  if (err != ESP_OK) {
    return ESP_FAIL;
  }
  size = sizeof(wifi_pswd_from_ble);
  err = nvs_get_str(NVS_HANDLE, "password", wifi_pswd_from_ble, &size);
  if (err != ESP_OK) {
    return ESP_FAIL;
  }
  nvs_close(NVS_HANDLE);

  return ESP_OK;
}

static esp_err_t store_mqtt_config(const char *uri, const char *username, const char *password) {
  esp_err_t err = nvs_open("mqtt", NVS_READWRITE, &NVS_HANDLE);
  if (err != ESP_OK) {
    return ESP_FAIL;
  }
  nvs_set_str(NVS_HANDLE, "uri", uri);
  nvs_set_str(NVS_HANDLE, "username", username);
  nvs_set_str(NVS_HANDLE, "password", password);
  err = nvs_commit(NVS_HANDLE);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "could not commit mqtt config");
    return ESP_FAIL;
  }
  nvs_close(NVS_HANDLE);
  return ESP_OK;
}
static esp_err_t restore_mqtt_config(void) {
  esp_err_t err = ESP_OK;

  err = nvs_open("mqtt", NVS_READWRITE, &NVS_HANDLE);
  if (err != ESP_OK) {
    return ESP_FAIL;
  }
  size_t size = sizeof(mqtt_uri);
  err = nvs_get_str(NVS_HANDLE, "uri", mqtt_uri, &size);
  if (err != ESP_OK) {
    return ESP_FAIL;
  }
  size = sizeof(mqtt_username);
  err = nvs_get_str(NVS_HANDLE, "username", mqtt_username, &size);
  if (err != ESP_OK) {
    return ESP_FAIL;
  }
  size = sizeof(mqtt_password);
  err = nvs_get_str(NVS_HANDLE, "password", mqtt_password, &size);
  if (err != ESP_OK) {
    return ESP_FAIL;
  }
  nvs_close(NVS_HANDLE);

  return ESP_OK;
}

static void prov_complete(uint16_t net_idx, uint16_t addr, uint8_t flags, uint32_t iv_index) {
  ESP_LOGI(TAG, "net_idx: 0x%04x, addr: 0x%04x", net_idx, addr);
  ESP_LOGI(TAG, "flags: 0x%02x, iv_index: 0x%08x", flags, iv_index);
}

static void ble_mesh_provisioning_cb(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t *param) {
  switch (event) {
  case ESP_BLE_MESH_PROV_REGISTER_COMP_EVT:
    ESP_LOGI(TAG, "ESP_BLE_MESH_PROV_REGISTER_COMP_EVT, err_code %d", param->prov_register_comp.err_code);
    break;
  case ESP_BLE_MESH_NODE_PROV_ENABLE_COMP_EVT:
    ESP_LOGI(TAG, "ESP_BLE_MESH_NODE_PROV_ENABLE_COMP_EVT, err_code %d", param->node_prov_enable_comp.err_code);
    break;
  case ESP_BLE_MESH_NODE_PROV_LINK_OPEN_EVT:
    ESP_LOGI(TAG, "ESP_BLE_MESH_NODE_PROV_LINK_OPEN_EVT, bearer %s",
             param->node_prov_link_open.bearer == ESP_BLE_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT");
    break;
  case ESP_BLE_MESH_NODE_PROV_LINK_CLOSE_EVT:
    ESP_LOGI(TAG, "ESP_BLE_MESH_NODE_PROV_LINK_CLOSE_EVT, bearer %s",
             param->node_prov_link_close.bearer == ESP_BLE_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT");
    break;
  case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
    ESP_LOGI(TAG, "ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT");
    prov_complete(param->node_prov_complete.net_idx, param->node_prov_complete.addr, param->node_prov_complete.flags,
                  param->node_prov_complete.iv_index);
    ESP_LOGI(TAG, "Try restore credentials");
    esp_err_t err = restore_wifi_config();
    if (err == ESP_OK) {
      wifi_init_sta(wifi_ssid_from_ble, WIFI_SSID_MAX_LEN, wifi_pswd_from_ble, WIFI_PSWD_MAX_LEN);
    }
    err = restore_mqtt_config();
    if (err == ESP_OK) {
      mqtt_app_start(mqtt_uri, MQTT_URI_MAX_LEN, mqtt_username, MQTT_USERNAME_MAX_LEN, mqtt_password,
                     MQTT_PASSWORD_MAX_LEN);
    }
    break;
  case ESP_BLE_MESH_NODE_PROV_RESET_EVT:
    break;
  case ESP_BLE_MESH_NODE_SET_UNPROV_DEV_NAME_COMP_EVT:
    ESP_LOGI(TAG, "ESP_BLE_MESH_NODE_SET_UNPROV_DEV_NAME_COMP_EVT, err_code %d",
             param->node_set_unprov_dev_name_comp.err_code);
    break;
  default:
    break;
  }
}

static void ble_mesh_generic_client_cb(esp_ble_mesh_generic_client_cb_event_t event,
                                       esp_ble_mesh_generic_client_cb_param_t *param) {
  ESP_LOGI(TAG, "Generic client, event %u, error code %d, opcode is 0x%04x", event, param->error_code,
           param->params->opcode);

  switch (event) {
  case ESP_BLE_MESH_GENERIC_CLIENT_GET_STATE_EVT:
    ESP_LOGI(TAG, "ESP_BLE_MESH_GENERIC_CLIENT_GET_STATE_EVT");
    if (param->params->opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET) {
      ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET, onoff %d", param->status_cb.onoff_status.present_onoff);
    }
    break;
  case ESP_BLE_MESH_GENERIC_CLIENT_SET_STATE_EVT:
    ESP_LOGI(TAG, "ESP_BLE_MESH_GENERIC_CLIENT_SET_STATE_EVT");
    if (param->params->opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET) {
      ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET, onoff %d", param->status_cb.onoff_status.present_onoff);
    }
    break;
  case ESP_BLE_MESH_GENERIC_CLIENT_PUBLISH_EVT:
    ESP_LOGI(TAG, "ESP_BLE_MESH_GENERIC_CLIENT_PUBLISH_EVT");
    ESP_LOGI(TAG, "addr: %04x, status: %d", param->params->ctx.addr, param->status_cb.onoff_status.present_onoff);
    char topic[16];
    char status[2];
    snprintf(topic, 16, "ble_mesh/%04x", param->params->ctx.addr);
    snprintf(status, 2, "%d", param->status_cb.onoff_status.present_onoff);
    mqtt_send_message(topic, status);
    break;
  case ESP_BLE_MESH_GENERIC_CLIENT_TIMEOUT_EVT:
    break;
  default:
    break;
  }
}

static void ble_mesh_config_server_cb(esp_ble_mesh_cfg_server_cb_event_t event,
                                      esp_ble_mesh_cfg_server_cb_param_t *param) {
  if (event == ESP_BLE_MESH_CFG_SERVER_STATE_CHANGE_EVT) {
    switch (param->ctx.recv_op) {
    case ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD:
      ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD");
      ESP_LOGI(TAG, "net_idx 0x%04x, app_idx 0x%04x", param->value.state_change.appkey_add.net_idx,
               param->value.state_change.appkey_add.app_idx);
      ESP_LOG_BUFFER_HEX("AppKey", param->value.state_change.appkey_add.app_key, 16);
      break;
    case ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND:
      ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND");
      ESP_LOGI(TAG, "elem_addr 0x%04x, app_idx 0x%04x, cid 0x%04x, mod_id 0x%04x",
               param->value.state_change.mod_app_bind.element_addr, param->value.state_change.mod_app_bind.app_idx,
               param->value.state_change.mod_app_bind.company_id, param->value.state_change.mod_app_bind.model_id);
      if (param->value.state_change.mod_app_bind.company_id == 0xFFFF &&
          param->value.state_change.mod_app_bind.model_id == ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_CLI) {
      }
      break;
    case ESP_BLE_MESH_MODEL_OP_MODEL_SUB_ADD:
      ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_MODEL_SUB_ADD");
      ESP_LOGI(TAG, "elem_addr 0x%04x, sub_addr 0x%04x, cid 0x%04x, mod_id 0x%04x",
               param->value.state_change.mod_sub_add.element_addr, param->value.state_change.mod_sub_add.sub_addr,
               param->value.state_change.mod_sub_add.company_id, param->value.state_change.mod_sub_add.model_id);
      break;
    case ESP_BLE_MESH_MODEL_OP_MODEL_SUB_DELETE:
      ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_MODEL_SUB_DELETE");
      ESP_LOGI(TAG, "elem_addr 0x%04x, del_addr 0x%04x, cid 0x%04x, mod_id 0x%04x",
               param->value.state_change.mod_sub_add.element_addr, param->value.state_change.mod_sub_add.sub_addr,
               param->value.state_change.mod_sub_add.company_id, param->value.state_change.mod_sub_add.model_id);

    default:
      break;
    }
  }
}

static void example_ble_mesh_custom_model_cb(esp_ble_mesh_model_cb_event_t event,
                                             esp_ble_mesh_model_cb_param_t *param) {
  switch (event) {
  case ESP_BLE_MESH_MODEL_OPERATION_EVT:
    ESP_LOGI(TAG, "Received message for Custom Model %d", param->model_operation.model->vnd.vnd_model_id);
    if (param->model_operation.opcode == ESP_BLE_MESH_WIFI_CONFIG_MODEL_OP_SEND) {
      uint16_t status = 0;
      ESP_LOGI(TAG, "Wifi config message received");
      if (param->model_operation.length == 0) {
        status = 1; // invalid message
        esp_err_t err = esp_ble_mesh_server_model_send_msg(&vnd_models[0], param->model_operation.ctx,
                                                           ESP_BLE_MESH_WIFI_CONFIG_MODEL_OP_STATUS, sizeof(status),
                                                           (uint8_t *)&status);
        ESP_LOGE(TAG, "Invalid WiFi Config message length: %zu", param->model_operation.length);
        if (err) {
          ESP_LOGE(TAG, "Failed to send message 0x%06x", ESP_BLE_MESH_WIFI_CONFIG_MODEL_OP_STATUS);
        }
        return;
      }

      ESP_LOGI(TAG, "Recv 0x%06" PRIx32 ", msg %s, len %d", param->model_operation.opcode, param->model_operation.msg,
               param->model_operation.length);
      // copy message in a null terminated string
      size_t message_size = sizeof(char) * param->model_operation.length + 1;
      char *message = malloc(message_size);
      if (!message) {
        status = 2; // out of memory
        esp_err_t err = esp_ble_mesh_server_model_send_msg(&vnd_models[1], param->model_operation.ctx,
                                                           ESP_BLE_MESH_MQTT_CONFIG_MODEL_OP_STATUS, sizeof(status),
                                                           (uint8_t *)&status);
        ESP_LOGE(TAG, "Could not allocate memory for WiFi Config message");
        if (err) {
          ESP_LOGE(TAG, "Failed to send message 0x%06x", ESP_BLE_MESH_MQTT_CONFIG_MODEL_OP_STATUS);
        }
        return;
      }

      snprintf(message, message_size, "%s", param->model_operation.msg);
      char *e = strchr(message, '.');
      if (!e) {
        status = 3; // no separator
        esp_err_t err = esp_ble_mesh_server_model_send_msg(&vnd_models[0], param->model_operation.ctx,
                                                           ESP_BLE_MESH_WIFI_CONFIG_MODEL_OP_STATUS, sizeof(status),
                                                           (uint8_t *)&status);
        ESP_LOGE(TAG, "No separator for WiFi Config message");
        if (err) {
          ESP_LOGE(TAG, "Failed to send message 0x%06x", ESP_BLE_MESH_WIFI_CONFIG_MODEL_OP_STATUS);
        }
        free(message);
        return;
      }

      int index = (int)(e - message);
      size_t wifi_ssid_from_ble_size = (sizeof(char) * index) + 1;
      size_t wifi_pswd_from_ble_size = (sizeof(char) * strlen(message) - (index + 1)) + 1;
      if (wifi_ssid_from_ble_size > WIFI_SSID_MAX_LEN || wifi_pswd_from_ble_size > WIFI_PSWD_MAX_LEN) {
        status = 4; // invalid parameters length
        esp_err_t err = esp_ble_mesh_server_model_send_msg(&vnd_models[0], param->model_operation.ctx,
                                                           ESP_BLE_MESH_WIFI_CONFIG_MODEL_OP_STATUS, sizeof(status),
                                                           (uint8_t *)&status);
        ESP_LOGE(TAG, "Invalid parameters length for WiFi Config message");
        if (err) {
          ESP_LOGE(TAG, "Failed to send message 0x%06x", ESP_BLE_MESH_WIFI_CONFIG_MODEL_OP_STATUS);
        }
        free(message);
        return;
      }

      snprintf(wifi_ssid_from_ble, wifi_ssid_from_ble_size, "%s", message);
      snprintf(wifi_pswd_from_ble, wifi_pswd_from_ble_size, "%s", &message[index + 1]);
      free(message);

      ESP_LOGI(TAG, "message %s, idx: %d, ssid %s, pswd %s, ssid_size %zu, pswd_size %zu", message, index,
               wifi_ssid_from_ble, wifi_pswd_from_ble, wifi_ssid_from_ble_size, wifi_pswd_from_ble_size);
      esp_err_t err = esp_ble_mesh_server_model_send_msg(&vnd_models[0], param->model_operation.ctx,
                                                         ESP_BLE_MESH_WIFI_CONFIG_MODEL_OP_STATUS, sizeof(status),
                                                         (uint8_t *)&status);
      if (err) {
        ESP_LOGE(TAG, "Failed to send message 0x%06x", ESP_BLE_MESH_WIFI_CONFIG_MODEL_OP_STATUS);
      }
      store_wifi_config(wifi_ssid_from_ble, wifi_pswd_from_ble);
      wifi_init_sta(wifi_ssid_from_ble, wifi_ssid_from_ble_size - 1, wifi_pswd_from_ble, wifi_pswd_from_ble_size - 1);
    }

    if (param->model_operation.opcode == ESP_BLE_MESH_MQTT_CONFIG_MODEL_OP_SEND) {
      ESP_LOGI(TAG, "MQTT config message received");
      uint16_t status = 0;
      if (param->model_operation.length == 0) {
        status = 1; // invalid message
        esp_err_t err = esp_ble_mesh_server_model_send_msg(&vnd_models[1], param->model_operation.ctx,
                                                           ESP_BLE_MESH_MQTT_CONFIG_MODEL_OP_STATUS, sizeof(status),
                                                           (uint8_t *)&status);
        ESP_LOGE(TAG, "Invalid MQTT Config message length: %zu", param->model_operation.length);
        if (err) {
          ESP_LOGE(TAG, "Failed to send message 0x%06x", ESP_BLE_MESH_MQTT_CONFIG_MODEL_OP_STATUS);
        }
        return;
      }

      ESP_LOGI(TAG, "Recv 0x%06" PRIx32 ", msg %s, len %d", param->model_operation.opcode, param->model_operation.msg,
               param->model_operation.length);
      // copy message in a null terminated string
      size_t message_size = sizeof(char) * param->model_operation.length + 1;
      char *message = malloc(message_size);
      if (!message) {
        status = 2; // out of memory
        esp_err_t err = esp_ble_mesh_server_model_send_msg(&vnd_models[1], param->model_operation.ctx,
                                                           ESP_BLE_MESH_MQTT_CONFIG_MODEL_OP_STATUS, sizeof(status),
                                                           (uint8_t *)&status);
        ESP_LOGE(TAG, "Could not allocate memory for MQTT Config message");
        if (err) {
          ESP_LOGE(TAG, "Failed to send message 0x%06x", ESP_BLE_MESH_MQTT_CONFIG_MODEL_OP_STATUS);
        }
        return;
      }

      snprintf(message, message_size, "%s", param->model_operation.msg);
      char *broker_uri = strtok(message, "|");
      char *username = strtok(NULL, "|");
      char *password = strtok(NULL, "|");
      if (!broker_uri || !username || !password) {
        status = 3; // invalid parameters
        esp_err_t err = esp_ble_mesh_server_model_send_msg(&vnd_models[1], param->model_operation.ctx,
                                                           ESP_BLE_MESH_MQTT_CONFIG_MODEL_OP_STATUS, sizeof(status),
                                                           (uint8_t *)&status);
        ESP_LOGE(TAG, "Invalid parameters for MQTT Config message");
        if (err) {
          ESP_LOGE(TAG, "Failed to send message 0x%06x", ESP_BLE_MESH_MQTT_CONFIG_MODEL_OP_STATUS);
        }
        free(message);
        return;
      }

      snprintf(mqtt_uri, strlen(broker_uri) + 1, "%s", broker_uri);
      snprintf(mqtt_username, strlen(username) + 1, "%s", username);
      snprintf(mqtt_password, strlen(password) + 1, "%s", password);
      free(message);

      esp_err_t err = esp_ble_mesh_server_model_send_msg(&vnd_models[1], param->model_operation.ctx,
                                                         ESP_BLE_MESH_MQTT_CONFIG_MODEL_OP_STATUS, sizeof(status),
                                                         (uint8_t *)&status);
      if (err) {
        ESP_LOGE(TAG, "Failed to send message 0x%06x", ESP_BLE_MESH_MQTT_CONFIG_MODEL_OP_STATUS);
      }

      ESP_LOGI(TAG, "MQTT uri: %s, username: %s, password: %s", mqtt_uri, mqtt_username, mqtt_password);
      store_mqtt_config(mqtt_uri, mqtt_username, mqtt_password);
      mqtt_app_start(mqtt_uri, strlen(mqtt_uri), mqtt_username, strlen(mqtt_username), mqtt_password,
                     strlen(mqtt_password));
    }
    break;
  case ESP_BLE_MESH_MODEL_SEND_COMP_EVT:
    if (param->model_send_comp.err_code) {
      ESP_LOGE(TAG, "Failed to send message 0x%06" PRIx32, param->model_send_comp.opcode);
      break;
    }
    ESP_LOGI(TAG, "Send 0x%06" PRIx32, param->model_send_comp.opcode);
    break;
  default:
    break;
  }
}

static esp_err_t ble_mesh_init(void) {
  esp_err_t err = ESP_OK;

  esp_ble_mesh_register_prov_callback(ble_mesh_provisioning_cb);
  esp_ble_mesh_register_generic_client_callback(ble_mesh_generic_client_cb);
  esp_ble_mesh_register_config_server_callback(ble_mesh_config_server_cb);
  esp_ble_mesh_register_custom_model_callback(example_ble_mesh_custom_model_cb);

  err = esp_ble_mesh_init(&provision, &composition);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize mesh stack (err %d)", err);
    return err;
  }

  err = esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to enable mesh node (err %d)", err);
    return err;
  }

  ESP_LOGI(TAG, "BLE Mesh Node initialized");

  return err;
}

void app_main(void) {
  esp_err_t err;

  ESP_LOGI(TAG, "Initializing...");

  err = nvs_flash_init();
  nvs_flash_erase();
  err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);

  err = bluetooth_init();
  if (err) {
    ESP_LOGE(TAG, "esp32_bluetooth_init failed (err %d)", err);
    return;
  }

  ble_mesh_get_dev_uuid(dev_uuid);

  /* Initialize the Bluetooth Mesh Subsystem */
  err = ble_mesh_init();
  if (err) {
    ESP_LOGE(TAG, "Bluetooth mesh init failed (err %d)", err);
  }

  esp_ble_gatt_set_local_mtu(200);

  sd_init();
}
