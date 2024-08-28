#ifndef _MQTT_APP_H_
#define _MQTT_APP_H_

#include <stddef.h>

#define MQTT_URI_MAX_LEN 32
#define MQTT_USERNAME_MAX_LEN 32
#define MQTT_PASSWORD_MAX_LEN 32

void mqtt_app_start(const char *broker_uri, size_t broker_uri_len, const char *username, size_t username_len,
                    const char *password, size_t password_len);
void mqtt_send_message(const char *topic, const char *data);

#endif // _MQTT_APP_H_