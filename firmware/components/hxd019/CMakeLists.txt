# hxd019 — interface 层：UART 收发 + 日志（ESP_LOG 只许本层）
# 依赖方向：hxd019 -> hxd019_core -> hxd019_shared；esp_driver_uart/esp_log 仅本层可见
# 注：IDF v6 driver 组件已拆分，uart 用 esp_driver_uart
idf_component_register(SRCS "src/hxd019_uart.c"
                       INCLUDE_DIRS "include"
                       REQUIRES hxd019_core hxd019_shared
                       PRIV_REQUIRES esp_driver_uart)
