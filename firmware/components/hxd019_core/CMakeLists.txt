# hxd019_core — core 层：码库查询 + 会话状态机
# 依赖方向：hxd019_core -> hxd019_shared；禁止依赖 UART/driver/esp_log（构建后 grep 自查）
idf_component_register(SRCS "src/hxd019_codec.c" "src/hxd019_session.c"
                       INCLUDE_DIRS "include"
                       REQUIRES hxd019_shared)
