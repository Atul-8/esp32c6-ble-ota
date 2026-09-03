# interface 层：BLE 接线 + OTA transport（vendor ble_ota 桥接）
# ble_ota 为本地 vendor 组件（../components/ble_ota，ERR-006 修复），
# 本地组件不会像 manifest 依赖那样自动加入 main 的 REQUIRES，须显式声明
idf_component_register(SRCS "main.c" "ble_transport.c"
                       INCLUDE_DIRS "."
                       REQUIRES ota_core ota_shared ble_ota bt nvs_flash app_update esp_app_format
                       PRIV_REQUIRES esp_ringbuf)
