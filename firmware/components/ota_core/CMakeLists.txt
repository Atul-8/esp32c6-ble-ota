# core 层：OTA 业务逻辑（回滚确认、进度持久化）
# 依赖方向：ota_core -> ota_shared；禁止依赖 NimBLE/ble_ota/esp_log
idf_component_register(SRCS "src/ota_rollback.c" "src/ota_progress_store.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ota_shared nvs_flash esp_partition app_update esp_system)
