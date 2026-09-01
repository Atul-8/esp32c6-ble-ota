/*
 * interface 层：app_main — BLE OTA 启动接线
 *
 * 启动顺序（有依赖约束，不可调换）：
 *   NVS -> 进度日志 -> 版本/分区/镜像状态日志 -> 回滚确认(必须先于 BLE init：
 *   PENDING_VERIFY 状态下 esp_ota_begin 会拒绝) -> controller init(IDF v6 下
 *   ble_ota 组件的 host_init 只初始化 host 栈，见 nimble_port.c 源码核实) ->
 *   ringbuf -> host_init -> 设备名覆盖 -> 回调注册 -> ota_task -> 广播名锚点日志
 */
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"

#include "ble_ota.h"
#include "host/ble_gap.h"
#include "services/gap/ble_svc_gap.h"

#include "ota_rollback.h"
#include "ota_progress_store.h"
#include "ota_version.h"
#include "ota_task.h"

static const char *TAG = "APP";

#define OTA_RINGBUF_SIZE 8192

static const char *img_state_str(const esp_partition_t *p)
{
    esp_ota_img_states_t st;
    if (p == NULL || esp_ota_get_state_partition(p, &st) != ESP_OK) {
        return "UNKNOWN";
    }
    switch (st) {
    case ESP_OTA_IMG_UNDEFINED:       return "UNDEFINED";
    case ESP_OTA_IMG_VALID:           return "VALID";
    case ESP_OTA_IMG_PENDING_VERIFY:  return "PENDING_VERIFY";
    case ESP_OTA_IMG_INVALID:         return "INVALID";
    case ESP_OTA_IMG_ABORTED:         return "ABORTED";
    default:                          return "UNKNOWN";
    }
}

static void rollback_check(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const char *pre_state = img_state_str(running);

    ota_rollback_action_t action = ota_rollback_confirm();

    const char *action_str;
    switch (action) {
    case OTA_ROLLBACK_CONFIRMED:        action_str = "confirmed";      break;
    case OTA_ROLLBACK_REBOOT_REQUESTED: action_str = "would-rollback"; break;
    default:                            action_str = "no-action";      break;
    }
    /* 注：would-rollback 时 core 层已直接回滚重启，本行实际来不及打印 */
    ESP_LOGI(TAG, "[BLE_OTA] rollback check: %s -> %s", pre_state, action_str);
}

static void log_resume_info(void)
{
    uint32_t size = 0, offset = 0, sector = 0;
    if (ota_progress_load(&size, &offset, &sector) != ESP_OK) {
        size = 0;
        offset = 0;
    }
    ESP_LOGI(TAG, "[BLE_OTA] resume info: offset=%" PRIu32 "/size=%" PRIu32 " (from last session)",
             offset, size);
}

static void log_version_line(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "[BLE_OTA] version=%s, run_part=%s, img_state=%s",
             desc->version,
             running ? running->label : "none",
             img_state_str(running));
}

static void wait_adv_and_log(void)
{
    /* 组件在 host 同步回调里启动广播；等广播真正起来后打锚点（最多等 3s） */
    for (int i = 0; i < 300; i++) {
        if (ble_gap_adv_active()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "[BLE_OTA] adv started, name=%s", ble_svc_gap_device_name());
}

void app_main(void)
{
    /* 1. NVS（回滚自检与进度存储的前置） */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. 启动锚点：上次会话进度 */
    log_resume_info();

    /* 3. 启动锚点：版本 / 运行分区 / 镜像状态 */
    log_version_line();

    /* 4. 回滚确认：必须在 BLE 初始化之前（PENDING_VERIFY 下 esp_ota_begin 拒绝） */
    rollback_check();

    /* 5. BT controller：IDF v6 下 ble_ota 的 host_init 只做 esp_nimble_init()（仅 host），
     *    controller init/enable 必须由 app 完成（照官方 example 非 protocomm 分支） */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    /* 6. ringbuf（先于回调注册，回调会写入） */
    if (!ble_ota_ringbuf_init(OTA_RINGBUF_SIZE)) {
        ESP_LOGE(TAG, "[BLE_OTA] init ringbuf fail");
        return;
    }

    /* 7. NimBLE host + GATT 服务 + 开始广播（同步回调内异步起广播） */
    ESP_ERROR_CHECK(esp_ble_ota_host_init());

    /* 8. 覆盖组件硬编码的 "nimble-ble-ota"。host 同步需完成多轮 HCI 命令（毫秒级），
     *    此处紧随 host_init 返回执行，必然先于组件读取广播名。 */
    int rc = ble_svc_gap_device_name_set(OTA_BLE_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "[BLE_OTA] gap name set failed, rc=%d", rc);
    }

    /* 9. 注册固件数据回调 + 启动落盘任务 */
    ESP_ERROR_CHECK(esp_ble_ota_recv_fw_data_callback(ota_recv_fw_cb));
    ota_task_init();

    /* 10. 广播锚点 */
    wait_adv_and_log();
}
