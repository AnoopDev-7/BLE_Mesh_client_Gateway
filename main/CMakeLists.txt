set(srcs "main.c" "ble_mesh_init.c" "ble_mesh_nvs.c" "wifi_connect.c" "mqtt_app.c" "sdcard.c")

idf_component_register(SRCS "sdcard.c" "${srcs}" INCLUDE_DIRS  ".")
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
