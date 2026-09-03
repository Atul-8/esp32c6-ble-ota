/*
 * interface 层：BLE OTA transport（vendor ble_ota 组件桥接）——实现
 *
 * 设计文档：.ai/docs/design-ota-transport-abstraction.md §5.1/§C.1、§3.2 lazy-open（权威）
 * 真机修订（2026-09-03 回归 B 证据链驱动，见 .ai/errors/raw/ERR-013.md）：
 *   初版"断连只清 armed、泵任务持旧 epoch 等新数据"的设计存在三条缺陷：
 *   ① 泵任务 STALE 后清 armed → 重试 Start 的 lazy-open 永不触发（传输空转）；
 *   ② 重试 Start 时 ringbuf 残留旧会话 straggler（≤2 chunk）→ 被误写入新会话
 *      offset 0（镜像错位，P0-1 变体）；
 *   ③ 中途二次 Start（P1-4 触发器）同 ②。
 *   修订后的会话边界协议（确定性，无竞速窗口）：
 *   - START 回调（NimBLE 任务）：armed=true、清 open_blocked、**非阻塞排空
 *     ringbuf**（旧会话 straggler 全部丢弃；新会话 sector 0 最早在 Start ACK
 *     空口往返 ~100ms 后才入队，排空窗口充裕）、epoch_invalidate（旧会话作废）。
 *   - 泵任务：receive 带 100ms 超时；**数据路径不检查 armed**（Stop 与最后一
 *     chunk 的 ACK 竞速——Stop 可能在尾 chunk 写盘期间到达，不能因此丢数据/
 *     杀会话，"收尾竞态"见下）；**超时路径检查 armed**（armed=false 且无数据
 *     = 流已死 → abort 旧会话回 IDLE）；STALE/write-fail/open-fail 路径一律
 *     **不清 armed**（armed 所有权归会话回调：START 置位、STOP/DISCONNECT 清零）。
 *   - recv_fw_cb：!armed 或 open_blocked 时数据源丢弃（armed=false 期间的组件
 *     残余数据不入队）。
 *
 * 收尾竞态（Stop/断连为何不 invalidate——保留裁决）：正常成功时序 = 最后
 * sector ACK 发出 → host 立即发 Stop + 断连 → 设备泵任务还在 esp_ota_write
 * 尾块 / esp_ota_end（数百 ms）→ activate。若 Stop/断连作废会话，activate 必然
 * STALE 拒绝重启 → happy path 全灭。数据收满的判定在泵任务（bytes_pumped >=
 * fw_len），不依赖 armed。
 *
 * fw_len 捕获时机（ERR-007/META-004）：lazy-open 在首数据 chunk 时实时读
 * esp_ble_ota_get_fw_length()——此时 Start 处理必已完成（组件不接受未 Start 的
 * 数据写入），构成 happens-before 边，缓存合法；捕获后不受组件 Stop 处理器清零
 * ota_total_len 的影响。
 *
 * notify_sem 契约（META-001/ERR-005）：ble_ota v0.1.17 组件 Stop 处理器
 * `extern SemaphoreHandle_t notify_sem` 反向引用 app 全局符号——必须保持非 static。
 * 组件语义：Stop 处理 take 一枚 count（example 由 ota_task 创建后 give 1 枚作
 * "Stop 允许令牌"）。R8/P2-10 修复：泵任务不再用该信号量做逐 sector 互斥（旧
 * ERR-009 泄漏面整体消失），init 时创建并预置 1 枚 count，创建后永不删除。
 */
#include "ble_transport.h"

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"

#include "ble_ota.h"

#include "ota_sink.h"

#define BLE_TRANSPORT_TASK_SIZE     8192
#define BLE_TRANSPORT_TASK_PRIORITY 5
/* 设计文档 §5.1：ringbuf 只属 BLE 桥接；满则丢新数据，靠 sector ACK 缺失触发
 * 上位机重发自愈（R3），生产者（NimBLE host 任务）不阻塞。 */
#define BLE_TRANSPORT_RINGBUF_SIZE  8192
/* 泵任务 receive 超时：流死亡（armed=false 无数据）后 ≤100ms 收敛旧会话 */
#define BLE_TRANSPORT_POLL_MS       100

static const char *TAG = "BLE_OTA";

static RingbufHandle_t s_ringbuf = NULL;
static volatile bool s_armed = false;        /* START 置位 / STOP+DISCONNECT 清零 */
static volatile bool s_open_blocked = false; /* open 失败 latch：数据源门控，START 清除 */

/* 注意：必须是非 static 全局——ble_ota v0.1.17 组件 Stop 处理器
 * `extern SemaphoreHandle_t notify_sem` 反向引用 app 侧此符号（组件隐式契约，
 * META-001/ERR-005）。R8/P2-10：初始化一次永不删除。 */
SemaphoreHandle_t notify_sem = NULL;

/* ---------- sink 事件回调（泵任务上下文执行） ---------- */

static void transport_sink_cb(ota_sink_event_t evt, const void *data, void *user_arg)
{
    (void)user_arg;
    switch (evt) {
    case OTA_SINK_EVT_SESSION_START: {
        const ota_sink_progress_t *p = (const ota_sink_progress_t *)data;
        ESP_LOGI(TAG, "[OTA_SINK] session open epoch=%" PRIu32 " target=%s size=%" PRIu32,
                 p->epoch, p->target_label ? p->target_label : "?", p->image_size);
        break;
    }
    case OTA_SINK_EVT_PROGRESS:
        /* 每 sector 一条会淹没传输速率，进度由 NVS/上位机侧承担，不打日志 */
        break;
    case OTA_SINK_EVT_ERROR: {
        const ota_sink_error_t *e = (const ota_sink_error_t *)data;
        ESP_LOGE(TAG, "[OTA_SINK] error code=%d idf_err=%s", (int)e->code,
                 e->idf_err ? esp_err_to_name((esp_err_t)e->idf_err) : "-");
        break;
    }
    case OTA_SINK_EVT_VALIDATED:
        ESP_LOGI(TAG, "[OTA_SINK] finish ok");
        break;
    case OTA_SINK_EVT_ACTIVATED:
        /* BLE 现状语义：activate(reboot=true) 内部 esp_restart，本行通常来不及
         * 打印；保留锚点以防未来 transport 改为先回 ACK 再重启 */
        ESP_LOGI(TAG, "[OTA_SINK] activated, rebooting");
        break;
    default:
        break;
    }
}

/* ---------- vendor 会话边界回调（NimBLE host 任务上下文，必须轻、不阻塞） ---------- */

/* 非阻塞排空 ringbuf：丢弃旧会话残留 chunk（Start 后新数据 ≥100ms 才入队，
 * 窗口充裕）。xRingbufferReceive timeout=0 为原子取item，多消费者安全。 */
static void ringbuf_flush(void)
{
    if (s_ringbuf == NULL) {
        return;
    }
    size_t flushed = 0;
    for (;;) {
        size_t item_size = 0;
        void *item = xRingbufferReceive(s_ringbuf, &item_size, 0);
        if (item == NULL) {
            break;
        }
        flushed += item_size;
        vRingbufferReturnItem(s_ringbuf, item);
    }
    if (flushed > 0) {
        ESP_LOGW(TAG, "[OTA_SINK] dropped %" PRIu32 " stale ringbuf bytes at session start",
                 (uint32_t)flushed);
    }
}

static void transport_session_cb(esp_ble_ota_session_evt_t evt)
{
    switch (evt) {
    case ESP_BLE_OTA_SESSION_START:
        /* 会话边界①（P0-1 主防线）：新 Start = 一切旧会话作废。
         * 顺序：先排空旧数据（必须在 armed=true 前？否——排空与 armed 无序依赖，
         * 但必须在 invalidate 前后皆可；新数据尚未入队）。 */
        s_armed = true;
        s_open_blocked = false;
        ringbuf_flush();
        ota_sink_epoch_invalidate(); /* 非阻塞：ABORT_PENDING 延迟到泵任务/open 兑现 */
        ESP_LOGW(TAG, "[OTA_SINK] old session invalidated by ble (Start)");
        break;
    case ESP_BLE_OTA_SESSION_STOP:
        /* 会话边界②：只清 armed——收尾竞态见文件头注释。数据路径不检查 armed，
         * 尾 chunk 照常写完并 finish+activate；流死亡的收敛在泵任务超时路径。 */
        s_armed = false;
        ESP_LOGW(TAG, "[OTA_SINK] session stop by ble");
        break;
    case ESP_BLE_OTA_SESSION_DISCONNECT:
        /* 会话边界③：断连（P0-1 原触发器）。同 Stop：只清 armed，泵任务收敛。 */
        s_armed = false;
        ESP_LOGW(TAG, "[OTA_SINK] session disconnect by ble");
        break;
    default:
        break;
    }
}

/* ---------- 数据源（NimBLE host 任务上下文） ---------- */

void ble_ota_transport_recv_cb(uint8_t *buf, uint32_t length)
{
    if (s_ringbuf == NULL || !s_armed || s_open_blocked) {
        return; /* 会话外/被拒后的组件残余数据：源头上丢弃 */
    }
    /* 满则丢：NimBLE host 回调上下文禁阻塞（ERR-009 教训）；丢 sector → 该
     * sector 尾包 ACK 不回 → 上位机 ACK 超时重发自愈（R3）。 */
    (void)xRingbufferSend(s_ringbuf, (void *)buf, length, 0);
}

/* ---------- 泵任务（会话串行方：open/write/finish/abort 全在此上下文） ---------- */

static void ble_transport_task(void *arg)
{
    uint32_t epoch = 0;         /* 当前会话代数（0=未 open） */
    uint32_t fw_len = 0;        /* lazy-open 时捕获（META-004 合法缓存，见头注释） */
    uint32_t bytes_pumped = 0;  /* 本会话已喂给 sink 的字节数 */
    uint8_t *data = NULL;
    size_t item_size = 0;

    for (;;) {
        data = (uint8_t *)xRingbufferReceive(s_ringbuf, &item_size,
                                             (TickType_t)pdMS_TO_TICKS(BLE_TRANSPORT_POLL_MS));

        if (data == NULL) {
            /* 超时：无数据。armed 已被 Stop/断连清除且会话还挂着 → 流已死，
             * abort 旧会话回 IDLE（"session abort by ble" 锚点，回归 B 证据）。 */
            if (epoch != 0 && !s_armed) {
                ESP_LOGW(TAG, "[OTA_SINK] session abort by ble");
                (void)ota_sink_session_abort(epoch);
                epoch = 0;
                fw_len = 0;
                bytes_pumped = 0;
            }
            continue;
        }
        if (item_size == 0) {
            vRingbufferReturnItem(s_ringbuf, (void *)data);
            continue;
        }

        /* lazy-open：armed 且未 open → 开会话（首个数据才 begin，修 P1-7） */
        if (epoch == 0 && s_armed) {
            ota_sink_session_cfg_t cfg = {
                .image_size = (uint32_t)esp_ble_ota_get_fw_length(),
                .source_tag = "ble",
                .resume = false,
            };
            if (cfg.image_size == 0) {
                /* Start 已到但长度 0（异常帧）：拒绝并门控数据源（修 P1-4） */
                ESP_LOGE(TAG, "[OTA_SINK] session open rejected: size=0");
                s_open_blocked = true;
                vRingbufferReturnItem(s_ringbuf, (void *)data);
                continue;
            }
            ota_sink_err_t err = ota_sink_session_open(&cfg, &epoch);
            if (err != OTA_SINK_OK) {
                /* size 超分区/选槽失败/BUSY/begin 拒绝：门控数据源，等下一次
                 * Start 重试（错误锚点已由 sink 事件回调打，含 idf_err 原名 P2-9）。
                 * 注意不清 armed——armed 归会话回调所有。 */
                ESP_LOGE(TAG, "[OTA_SINK] session open rejected: %d", (int)err);
                s_open_blocked = true;
                epoch = 0;
                vRingbufferReturnItem(s_ringbuf, (void *)data);
                continue;
            }
            fw_len = cfg.image_size;
            bytes_pumped = 0;
        }

        if (epoch != 0) {
            /* 数据路径不检查 armed：Stop 与尾 chunk 的收尾竞态（文件头注释）——
             * 会话有效性由 sink 的 epoch 校验兜底（STALE）。 */
            ota_sink_err_t werr = ota_sink_write(epoch, data, item_size);
            vRingbufferReturnItem(s_ringbuf, (void *)data);
            if (werr == OTA_SINK_OK || werr == OTA_SINK_ERR_NVS) {
                bytes_pumped += (uint32_t)item_size;

                /* 收尾判定：数据泵满即收尾，不等 Stop */
                if (fw_len > 0 && bytes_pumped >= fw_len) {
                    ota_sink_err_t ferr = ota_sink_finish(epoch);
                    if (ferr == OTA_SINK_OK) {
                        (void)ota_sink_activate(epoch, true); /* esp_restart 不返回 */
                        /* activate 失败（set_boot 错误，罕见）：可重试状态 */
                        ESP_LOGE(TAG, "[OTA_SINK] activate failed, session dropped");
                        epoch = 0;
                        fw_len = 0;
                        bytes_pumped = 0;
                    } else {
                        /* finish FAIL：sink 内部已 abort 回 IDLE（设备自愈，不再旧
                         * ota_task 的任务自删）。err 锚点已由事件回调打。 */
                        ESP_LOGE(TAG, "[OTA_SINK] finish FAIL (err=%d)", (int)ferr);
                        epoch = 0;
                        fw_len = 0;
                        bytes_pumped = 0;
                    }
                }
            } else if (werr == OTA_SINK_ERR_SESSION_STALE) {
                /* 写入层防御触发（P0-1 三层防御之 2）：旧 epoch 数据丢弃。
                 * epoch 归零后，下一个 chunk 将 lazy-open 新会话（若 armed）。 */
                ESP_LOGW(TAG, "[OTA_SINK] STALE write dropped (epoch old=%" PRIu32 ")",
                         epoch);
                (void)ota_sink_session_abort(epoch);
                epoch = 0;
                fw_len = 0;
                bytes_pumped = 0;
            } else {
                /* WRITE_FAIL 等：本会话不可信，abort 自愈（锚点由事件回调打） */
                ESP_LOGE(TAG, "[OTA_SINK] write fail (%d), session aborted by ble",
                         (int)werr);
                (void)ota_sink_session_abort(epoch);
                epoch = 0;
                fw_len = 0;
                bytes_pumped = 0;
            }
        } else {
            /* 未 armed 的数据（Start 前残余/被门控漏网）：直接丢弃 */
            vRingbufferReturnItem(s_ringbuf, (void *)data);
        }
    }
}

/* ---------- 初始化 ---------- */

bool ble_ota_transport_init(uint32_t ringbuf_size)
{
    if (s_ringbuf != NULL) {
        return true; /* 只初始化一次 */
    }
    if (ringbuf_size == 0) {
        ringbuf_size = BLE_TRANSPORT_RINGBUF_SIZE;
    }

    s_ringbuf = xRingbufferCreate(ringbuf_size, RINGBUF_TYPE_BYTEBUF);
    if (s_ringbuf == NULL) {
        return false;
    }

    /* notify_sem（META-001 契约 + R8/P2-10）：初值 1——组件 Stop 处理器 take 一枚
     * 作为"Stop 处理令牌"（example ota_task 创建后 give 1 的语义等价保留）。泵任务
     * 不再用它做逐 sector 互斥（ERR-009 泄漏面整体消失）。创建一次永不删除。 */
    if (notify_sem == NULL) {
        notify_sem = xSemaphoreCreateCounting(100, 1);
    }
    if (notify_sem == NULL) {
        return false; /* 罕见（堆不足）：设备侧属启动失败路径 */
    }

    ota_sink_init(transport_sink_cb, NULL);

    if (xTaskCreate(&ble_transport_task, "ble_transport", BLE_TRANSPORT_TASK_SIZE,
                    NULL, BLE_TRANSPORT_TASK_PRIORITY, NULL) != pdPASS) {
        return false;
    }

    /* vendor 会话边界回调（~25 行补丁的 app 侧对接点） */
    esp_ble_ota_set_session_cb(transport_session_cb);
    return true;
}
