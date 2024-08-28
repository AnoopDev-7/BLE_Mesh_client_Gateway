#ifndef _WIFI_CONNECT_H_
#define _WIFI_CONNECT_H_

#include <stddef.h>

#define WIFI_SSID_MAX_LEN 32
#define WIFI_PSWD_MAX_LEN 32

typedef void (*wifi_status_cb_t)(int status);

void wifi_init_sta(const char *ssid, size_t ssid_len, const char *password, size_t password_len);
int wifi_is_connected(void);
void wifi_register_on_status_change_callback(wifi_status_cb_t callback);

#endif // _WIFI_CONNECT_H_