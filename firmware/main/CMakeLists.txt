# interface 层：BLE 接线 + OTA 落盘任务
# ble_ota 组件由 main/idf_component.yml 声明，组件管理器自动加入 main 的依赖
idf_component_register(SRCS "main.c" "ota_task.c"
                       INCLUDE_DIRS "."
                       REQUIRES ota_core ota_shared bt nvs_flash app_update esp_app_format
                       PRIV_REQUIRES esp_ringbuf)
