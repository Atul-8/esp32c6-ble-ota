# hxd019_shared — shared 层：HXD019EU 协议常量/类型/帧构建纯函数
# 零硬件依赖（只 stdint/stddef/string）；被 hxd019_core 与 hxd019（interface）依赖
# 帧格式/温度编码经 Kconfig 注入编译期宏（CONFIG_HXD019_STATE_FMT_11KEY / CONFIG_HXD019_TEMP_ENC_MINUS16）
idf_component_register(SRCS "src/hxd019_frame.c"
                       INCLUDE_DIRS "include"
                       REQUIRES "")

if(CONFIG_HXD019_SELFTEST)
    # 自测源不在正式 SRCS 中，Kconfig 开启后追加（printf 仅自测路径）
    target_sources(${COMPONENT_LIB} PRIVATE "test/test_hxd019_frame.c")
endif()
