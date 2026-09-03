/**
 * @file hxd019_uart.c
 * @brief HXD019EU interface 层实现：UART 收发 + RX 任务 + 会话整合
 *
 * 日志锚点（唯一允许 ESP_LOG 的层）：
 *   [HXD019] tx frame: xx xx xx ...
 *   [HXD019] match result: group=%u
 *   [HXD019] learn data: ...
 *
 * RX 协议说明：datasheet 只明确"匹配后芯片通过串口上传码库号（如 830）"，
 * 未定义应答帧完整格式（帧头/长度/校验）。RX 任务按"原始字节流 + 松散解析"实现：
 *  - 收到疑似匹配应答（含可解析 2B 码组号）时上抛回调并打锚点日志
 *  - 同时保留原始字节回调通道（学习数据透传）
 *  TODO(联调)：真机抓包确认应答帧格式后收紧解析（帧头/长度/校验）。
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "hxd019.h"
#include "hxd019_frame.h"
#include "hxd019_session.h"
#include "sdkconfig.h"

static const char *TAG = "HXD019";  /* 锚点前缀：ESP_LOGx 输出 "[HXD019] ..." */

#define HXD019_BAUDRATE      57600
#define HXD019_UART_PORT     CONFIG_HXD019_UART_NUM
#define HXD019_TX_GPIO       CONFIG_HXD019_UART_TX
#define HXD019_RX_GPIO       CONFIG_HXD019_UART_RX
#define HXD019_RX_TASK_STACK CONFIG_HXD019_RX_TASK_STACK
#define HXD019_RX_TASK_PRIO  CONFIG_HXD019_RX_TASK_PRIO
#define HXD019_RX_BUF        256
#define HXD019_TX_TIMEOUT_MS 100

static SemaphoreHandle_t s_tx_mutex;      /* 发送串行化 */
static hxd019_session_t s_sess;           /* 会话：绑定码组 + 状态缓存 */
static hxd019_match_cb_t s_match_cb;
static void *s_match_ctx;
static hxd019_learn_cb_t s_learn_cb;
static void *s_learn_ctx;
static volatile bool s_running;

static void rx_task(void *arg);

/* ---------------- 字段 mutate 辅助（便捷函数共用一套发帧路径） ---------------- */
enum { F_POWER, F_LRSWING, F_SLEEP, F_AUXHEAT, F_LIGHT, F_ECO };
struct bool_arg { uint8_t field; bool on; };
struct u8_arg  { uint8_t val; };

static void mut_bool(hxd019_ac_state_t *st, void *arg)
{
    const struct bool_arg *p = arg;
    switch (p->field) {
    case F_POWER:   st->power = p->on ? HXD019_POWER_ON : HXD019_POWER_OFF; break;
    case F_LRSWING: st->swing_auto = p->on ? HXD019_LRSWING_ON : HXD019_LRSWING_OFF; break;
    case F_SLEEP:   st->sleep = p->on ? 1 : 0; break;
    case F_AUXHEAT: st->aux_heat = p->on ? 1 : 0; break;
    case F_LIGHT:   st->light = p->on ? 1 : 0; break;
    case F_ECO:     st->eco = p->on ? 1 : 0; break;
    default: break;
    }
}

static void mut_temp(hxd019_ac_state_t *st, void *arg)
{
    st->temp_c = ((struct u8_arg *)arg)->val;
}

static void mut_u8_mode(hxd019_ac_state_t *st, void *arg)  { st->mode  = ((struct u8_arg *)arg)->val; }
static void mut_u8_fan(hxd019_ac_state_t *st, void *arg)   { st->fan   = ((struct u8_arg *)arg)->val; }
static void mut_u8_swing(hxd019_ac_state_t *st, void *arg) { st->swing = ((struct u8_arg *)arg)->val; }

/* 基准状态 + 字段修改 + 构帧 + 发送 + 提交缓存 */
static hxd019_err_t send_state_with_key(uint8_t key,
                                        void (*mutate)(hxd019_ac_state_t *st, void *arg),
                                        void *arg)
{
    hxd019_ac_state_t st;
    bool is_default;
    hxd019_session_base_state(&s_sess, &st, &is_default);
    st.key = key;
    if (mutate != NULL) {
        mutate(&st, arg);
    }
    hxd019_frame_t f;
    hxd019_err_t err = hxd019_build_ac_state(s_sess.code_group, &st, &f);
    if (err != HXD019_OK) {
        return err;
    }
    err = hxd019_send_frame(&f);
    if (err == HXD019_OK) {
        hxd019_session_commit(&s_sess, &st);
    }
    return err;
}

/* ---------------- RX 任务：匹配应答/学习数据 ---------------- */
static void rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[HXD019_RX_BUF];
    while (s_running) {
        int n = uart_read_bytes(HXD019_UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(200));
        if (n <= 0) {
            continue;
        }
        if (s_learn_cb != NULL) {
            s_learn_cb(buf, (size_t)n, s_learn_ctx);
        }
        /*
         * 松散匹配解析（TODO 联调收紧）：datasheet 仅说"通过串口上传码库号 830"。
         * 假设上传帧内含大端 2B 码组号（30 XX GG LL 或裸 GG LL），做最简扫描：
         * 取前 2 个非 0x30/非 0x20/非 0x70 字节为大端码组候选，值域 1-2047 过滤。
         */
        uint16_t group = 0;
        for (int i = 0; i + 1 < n; i++) {
            uint8_t b0 = buf[i], b1 = buf[i + 1];
            if (b0 == 0x30 || b0 == 0x20 || b0 == 0x70) {
                continue;  /* 命令/头字节不像码组高字节 */
            }
            uint16_t g = ((uint16_t)b0 << 8) | b1;
            if (g >= 1 && g <= 2047) {
                group = g;
                break;
            }
        }
        if (group != 0) {
            ESP_LOGI(TAG, "match result: group=%u", group);  /* 锚点 */
            if (s_match_cb != NULL) {
                s_match_cb(group, s_match_ctx);
            }
        } else {
            ESP_LOGI(TAG, "learn data: %d bytes", n);        /* 锚点 */
        }
    }
    vTaskDelete(NULL);
}

/* ---------------- 初始化 / 反初始化 ---------------- */
hxd019_err_t hxd019_init(void)
{
    if (s_tx_mutex != NULL) {
        return HXD019_OK;  /* 已初始化 */
    }
    const uart_config_t cfg = {
        .baud_rate = HXD019_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t uerr = uart_driver_install(HXD019_UART_PORT, HXD019_RX_BUF, 0, 0, NULL, 0);
    if (uerr != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install fail: %s", esp_err_to_name(uerr));
        return HXD019_ERR_ARG;
    }
    uerr = uart_param_config(HXD019_UART_PORT, &cfg);
    if (uerr == ESP_OK) {
        uerr = uart_set_pin(HXD019_UART_PORT, HXD019_TX_GPIO, HXD019_RX_GPIO,
                            UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (uerr != ESP_OK) {
        ESP_LOGE(TAG, "uart config fail: %s", esp_err_to_name(uerr));
        uart_driver_delete(HXD019_UART_PORT);
        return HXD019_ERR_ARG;
    }

    s_tx_mutex = xSemaphoreCreateMutex();
    hxd019_session_init(&s_sess, NULL);  /* F_code 钩子默认桩 */
    s_running = true;

    if (xTaskCreate(rx_task, "hxd019_rx", HXD019_RX_TASK_STACK, NULL,
                    HXD019_RX_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "rx task create fail");
        s_running = false;
        vSemaphoreDelete(s_tx_mutex);
        s_tx_mutex = NULL;
        uart_driver_delete(HXD019_UART_PORT);
        return HXD019_ERR_ARG;
    }
    ESP_LOGI(TAG, "init ok uart=%d tx=%d rx=%d 57600-8N1",
             HXD019_UART_PORT, HXD019_TX_GPIO, HXD019_RX_GPIO);
    return HXD019_OK;
}

void hxd019_deinit(void)
{
    if (s_tx_mutex == NULL) {
        return;
    }
    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(50));  /* 给 RX 任务退出窗口 */
    uart_driver_delete(HXD019_UART_PORT);
    vSemaphoreDelete(s_tx_mutex);
    s_tx_mutex = NULL;
}

/* ---------------- 帧收发 ---------------- */
hxd019_err_t hxd019_send_frame(const hxd019_frame_t *frame)
{
    if (frame == NULL || frame->len == 0 || frame->len > HXD019_FRAME_MAX) {
        return HXD019_ERR_ARG;
    }
    if (s_tx_mutex == NULL) {
        return HXD019_ERR_ARG;  /* 未初始化 */
    }
    char hex[HXD019_FRAME_MAX * 3 + 1];
    for (size_t i = 0; i < frame->len; i++) {
        snprintf(&hex[i * 3], 4, "%02X ", frame->buf[i]);
    }
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(HXD019_TX_TIMEOUT_MS)) != pdTRUE) {
        return HXD019_ERR_ARG;
    }
    int n = uart_write_bytes(HXD019_UART_PORT, frame->buf, frame->len);
    if (n == (int)frame->len) {
        uart_wait_tx_idle_polling(HXD019_UART_PORT);  /* IDF v6：轮询等 TX FIFO 排空 */
    }
    xSemaphoreGive(s_tx_mutex);
    if (n != (int)frame->len) {
        ESP_LOGE(TAG, "tx short write %d/%u", n, (unsigned)frame->len);
        return HXD019_ERR_ARG;
    }
    ESP_LOGI(TAG, "tx frame: %s", hex);   /* 锚点：[HXD019] tx frame: 30 06 03 3E 81 */
    return HXD019_OK;
}

hxd019_err_t hxd019_ac_simple(uint8_t func_code)
{
    if (s_sess.code_group == 0) {
        ESP_LOGW(TAG, "ac_simple: no code group bound");
        return HXD019_ERR_NOENT;
    }
    hxd019_frame_t f;
    hxd019_err_t err = hxd019_build_ac_simple(s_sess.code_group, func_code, &f);
    return (err == HXD019_OK) ? hxd019_send_frame(&f) : err;
}

hxd019_err_t hxd019_bind_group(uint16_t code_group)
{
    if (s_tx_mutex == NULL) {
        return HXD019_ERR_ARG;
    }
    hxd019_session_bind(&s_sess, code_group);
    ESP_LOGI(TAG, "bind code group %u", code_group);
    return HXD019_OK;
}

/* ---------------- 空调便捷函数（完整状态帧 + 会话缓存） ---------------- */
hxd019_err_t hxd019_ac_power(bool on)
{
    struct bool_arg a = { F_POWER, on };
    return send_state_with_key(HXD019_KEY_POWER, mut_bool, &a);
}

hxd019_err_t hxd019_ac_temp(uint8_t temp_c)
{
    hxd019_ac_state_t st;
    bool is_default;
    hxd019_session_base_state(&s_sess, &st, &is_default);
    uint8_t key = (temp_c >= st.temp_c) ? HXD019_KEY_TEMP_INC : HXD019_KEY_TEMP_DEC;
    struct u8_arg a = { temp_c };
    return send_state_with_key(key, mut_temp, &a);
}

hxd019_err_t hxd019_ac_temp_step(int8_t delta)
{
    hxd019_ac_state_t st;
    bool is_default;
    hxd019_session_base_state(&s_sess, &st, &is_default);
    int t = (int)st.temp_c + delta;
    if (t < HXD019_TEMP_MIN_C) {
        t = HXD019_TEMP_MIN_C;
    }
    if (t > HXD019_TEMP_MAX_C) {
        t = HXD019_TEMP_MAX_C;
    }
    struct u8_arg a = { (uint8_t)t };
    return send_state_with_key(
        (delta >= 0) ? HXD019_KEY_TEMP_INC : HXD019_KEY_TEMP_DEC, mut_temp, &a);
}

hxd019_err_t hxd019_ac_mode(uint8_t mode)
{
    struct u8_arg a = { mode };
    return send_state_with_key(HXD019_KEY_MODE, mut_u8_mode, &a);
}

hxd019_err_t hxd019_ac_fan(uint8_t fan)
{
    struct u8_arg a = { fan };
    return send_state_with_key(HXD019_KEY_FAN, mut_u8_fan, &a);
}

hxd019_err_t hxd019_ac_swing(uint8_t swing)
{
    struct u8_arg a = { swing };
    return send_state_with_key(HXD019_KEY_SWING, mut_u8_swing, &a);
}

hxd019_err_t hxd019_ac_lrswing(bool on)
{
    struct bool_arg a = { F_LRSWING, on };
    return send_state_with_key(HXD019_KEY_LRSWING, mut_bool, &a);
}

hxd019_err_t hxd019_ac_sleep(bool on)
{
#if HXD019_FMT_IS_11KEY
    struct bool_arg a = { F_SLEEP, on };
    return send_state_with_key(HXD019_KEY_SLEEP, mut_bool, &a);
#else
    (void)on;
    return HXD019_ERR_KEY7;
#endif
}

hxd019_err_t hxd019_ac_aux_heat(bool on)
{
#if HXD019_FMT_IS_11KEY
    struct bool_arg a = { F_AUXHEAT, on };
    return send_state_with_key(HXD019_KEY_HEAT, mut_bool, &a);
#else
    (void)on;
    return HXD019_ERR_KEY7;
#endif
}

hxd019_err_t hxd019_ac_light(bool on)
{
#if HXD019_FMT_IS_11KEY
    struct bool_arg a = { F_LIGHT, on };
    return send_state_with_key(HXD019_KEY_LIGHT, mut_bool, &a);
#else
    (void)on;
    return HXD019_ERR_KEY7;
#endif
}

hxd019_err_t hxd019_ac_eco(bool on)
{
#if HXD019_FMT_IS_11KEY
    struct bool_arg a = { F_ECO, on };
    return send_state_with_key(HXD019_KEY_ECO, mut_bool, &a);
#else
    (void)on;
    return HXD019_ERR_KEY7;
#endif
}

hxd019_err_t hxd019_ac_send_key(uint8_t key)
{
    /* 快捷键 0x0C-0x0F：芯片内部重置相关值（与上层主控值无关），状态段照发当前基准 */
    return send_state_with_key(key, NULL, NULL);
}

/* ---------------- 非空调 / 学习 / 匹配 ---------------- */
hxd019_err_t hxd019_send_av_key(uint8_t fmt_index,
                                const uint8_t key_code[HXD019_AV_KEY_LEN],
                                const uint8_t com_code[HXD019_AV_COM_LEN])
{
    hxd019_frame_t f;
    hxd019_err_t err = hxd019_build_av(fmt_index, key_code, com_code, &f);
    return (err == HXD019_OK) ? hxd019_send_frame(&f) : err;
}

hxd019_err_t hxd019_start_learn(hxd019_learn_cb_t cb, void *ctx)
{
    s_learn_cb = cb;
    s_learn_ctx = ctx;
    hxd019_frame_t f;
    hxd019_err_t err = hxd019_build_learn(&f);
    return (err == HXD019_OK) ? hxd019_send_frame(&f) : err;
}

hxd019_err_t hxd019_match(hxd019_match_cb_t cb, void *ctx)
{
    s_match_cb = cb;
    s_match_ctx = ctx;
    hxd019_frame_t f;
    hxd019_err_t err = hxd019_build_match(&f);
    return (err == HXD019_OK) ? hxd019_send_frame(&f) : err;
}
