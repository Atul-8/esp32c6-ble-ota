/*
 * core 层：OTA 会话与写盘统一编排（ota_sink）——实现
 *
 * 设计文档：.ai/docs/design-ota-transport-abstraction.md §3/§3.1/§3.2（权威）
 *
 * 并发模型（与头文件注释一致，实现侧要点）：
 *   - 锁只保护状态转换；esp_ota_write / esp_ota_end 不持锁。
 *   - esp_ota_abort 延迟执行：epoch_invalidate 可被任意上下文（NimBLE host 任务）
 *     调用，为避免与泵任务的 esp_ota_write 并发触碰同一 handle（IDF 语义未定义），
 *     invalidate 只把 WRITING 置为 ABORT_PENDING；真正的 esp_ota_abort 在
 *     session_open（泵任务 lazy-open 前置）或 session_abort（泵任务显式路径）
 *     串行执行。
 *   - ABORT_PENDING / 失效 epoch 期间所有 write 以 STALE 短路（写路径前后双重
 *     epoch 校验）。会话收尾（finish/activate）不受 armed 标志影响——由 transport
 *     的数据量判定驱动（Stop/断连竞态处理见 ble_transport.c 文件头）。
 *
 * 事件回调在持锁/调用者上下文中直接调用，回调内禁止再进 sink API。
 */
#include "ota_sink.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

#include "ota_progress_store.h"
#include "ota_version.h"

/* 0 恒为非法 epoch（open 返回值从 1 起计，单调递增不复用） */
#define OTA_SINK_EPOCH_INVALID 0u

typedef enum {
    SINK_STATE_IDLE = 0,
    SINK_STATE_WRITING,       /* open 完成，esp_ota handle 有效 */
    SINK_STATE_ABORT_PENDING, /* invalidate 已到，等串行方（泵任务）执行 abort */
    SINK_STATE_VALIDATED,     /* finish 通过，等 activate */
    SINK_STATE_ACTIVATED,     /* activate 完成（reboot=true 时已不返回） */
} sink_state_t;

typedef struct {
    SemaphoreHandle_t     lock;
    ota_sink_event_cb_t   cb;
    void                 *cb_arg;

    sink_state_t          state;
    uint32_t              epoch;         /* 最近一次 open 派发的会话代数 */
    bool                  session_live;  /* 单写者会话存在（BUSY 判定依据） */
    uint32_t              next_epoch;    /* 下一个会话代数 */

    esp_ota_handle_t      handle;
    const esp_partition_t *target;

    uint32_t              image_size;
    uint32_t              bytes_written;
    uint32_t              sectors_done;
    bool                  progress_dirty; /* 本 sector NVS 保存失败（下 sector 重试） */
} ota_sink_t;

static ota_sink_t s_sink;

/* ---------- 内部工具（全部要求持锁调用） ---------- */

static void sink_fire(ota_sink_event_t evt, const void *data)
{
    if (s_sink.cb) {
        s_sink.cb(evt, data, s_sink.cb_arg);
    }
}

static void sink_fire_error(ota_sink_err_t code, int32_t idf_err)
{
    ota_sink_error_t err = { .code = code, .idf_err = idf_err };
    sink_fire(OTA_SINK_EVT_ERROR, &err);
}

/* 状态归位 IDLE（不清 handle——handle 生命周期由调用方按并发模型管理）。持锁调用。 */
static void sink_to_idle_locked(void)
{
    s_sink.state = SINK_STATE_IDLE;
    s_sink.session_live = false;
    /* epoch 值保留（仅供诊断），下一次 open 用 next_epoch 分配全新值 */
}

/* 持锁查询：本 epoch 是否仍是活动写会话 */
static bool sink_epoch_is_current_locked(uint32_t epoch)
{
    return s_sink.session_live && epoch == s_sink.epoch &&
           s_sink.state == SINK_STATE_WRITING;
}

/* ---------- 公开 API ---------- */

void ota_sink_init(ota_sink_event_cb_t cb, void *user_arg)
{
    if (s_sink.lock != NULL) {
        return; /* 只初始化一次 */
    }
    s_sink.cb = cb;
    s_sink.cb_arg = user_arg;
    s_sink.lock = xSemaphoreCreateMutex();
    s_sink.state = SINK_STATE_IDLE;
    s_sink.epoch = OTA_SINK_EPOCH_INVALID;
    s_sink.next_epoch = 1;
    s_sink.session_live = false;
    s_sink.handle = 0;
}

ota_sink_err_t ota_sink_session_open(const ota_sink_session_cfg_t *cfg, uint32_t *out_epoch)
{
    if (cfg == NULL || out_epoch == NULL || cfg->image_size == 0) {
        return OTA_SINK_ERR_BAD_ARG;
    }

    /* 前置：串行方先兑现 ABORT_PENDING（本函数按约束在泵任务上下文执行） */
    xSemaphoreTake(s_sink.lock, portMAX_DELAY);
    if (s_sink.state == SINK_STATE_ABORT_PENDING) {
        if (s_sink.handle != 0) {
            (void)esp_ota_abort(s_sink.handle); /* 失败无从恢复，begin 全擦兜底 */
            s_sink.handle = 0;
        }
        sink_to_idle_locked();
    }

    /* 单写者互斥（ADR-004-2）：先到先得，不抢占 */
    if (s_sink.session_live) {
        xSemaphoreGive(s_sink.lock);
        return OTA_SINK_ERR_BUSY;
    }

    /* 选槽基准 = running partition（修 P1-5：otadata 指向坏槽时
     * get_boot_partition 与实际运行分区可能不一致，example 的 boot 基准会选错槽） */
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        xSemaphoreGive(s_sink.lock);
        return OTA_SINK_ERR_PARTITION;
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(running);
    if (target == NULL) {
        xSemaphoreGive(s_sink.lock);
        return OTA_SINK_ERR_PARTITION;
    }

    /* size 校验（修 P1-4）：目标分区容量上限，设备侧收拢 */
    if (cfg->image_size > target->size) {
        xSemaphoreGive(s_sink.lock);
        return OTA_SINK_ERR_SIZE_INVALID;
    }

    /* begin 时机后移（修 P1-7）：仅真正 open 会话才擦，且按已知 size 部分擦除
     * （不再 OTA_SIZE_UNKNOWN 开机全擦 1.75MB） */
    esp_err_t ier = esp_ota_begin(target, cfg->image_size, &s_sink.handle);
    if (ier != ESP_OK) {
        s_sink.handle = 0;
        xSemaphoreGive(s_sink.lock);
        /* P2-9：idf_err 原值上抛（含 PENDING_VERIFY 拒绝），transport 层
         * esp_err_to_name 打日志 */
        sink_fire_error(OTA_SINK_ERR_BEGIN_FAIL, (int32_t)ier);
        return OTA_SINK_ERR_BEGIN_FAIL;
    }

    /* 会话登记：epoch 单调递增分配 */
    s_sink.target = target;
    s_sink.image_size = cfg->image_size;
    s_sink.bytes_written = 0;
    s_sink.sectors_done = 0;
    s_sink.progress_dirty = false;
    s_sink.epoch = s_sink.next_epoch++;
    s_sink.session_live = true;
    s_sink.state = SINK_STATE_WRITING;

    uint32_t epoch = s_sink.epoch;
    ota_sink_progress_t prog = {
        .epoch = epoch,
        .target_label = target->label,
        .image_size = s_sink.image_size,
        .bytes_written = 0,
        .sectors_done = 0,
        .resumed = cfg->resume,
    };
    xSemaphoreGive(s_sink.lock);

    sink_fire(OTA_SINK_EVT_SESSION_START, &prog);
    *out_epoch = epoch;
    return OTA_SINK_OK;
}

ota_sink_err_t ota_sink_write(uint32_t epoch, const void *data, size_t len)
{
    if (data == NULL || len == 0) {
        return OTA_SINK_ERR_BAD_ARG;
    }

    xSemaphoreTake(s_sink.lock, portMAX_DELAY);
    /* 写入层防御（P0-1 三层防御之 2）：epoch 过期/中止在即 → 数据不落盘 */
    if (!sink_epoch_is_current_locked(epoch)) {
        xSemaphoreGive(s_sink.lock);
        return OTA_SINK_ERR_SESSION_STALE;
    }
    esp_ota_handle_t handle = s_sink.handle;
    xSemaphoreGive(s_sink.lock);

    /* 不持锁写盘（§3.1 锁粒度）：esp_ota_write 同步落盘可达百 ms 级。
     * 此窗口内若 invalidate 到达，handle 进入 ABORT_PENDING，本块仍写入被
     * abort 的旧 handle——数据从未越过 epoch 关卡进入新会话，无害。 */
    esp_err_t ier = esp_ota_write(handle, data, len);
    if (ier != ESP_OK) {
        sink_fire_error(OTA_SINK_ERR_WRITE_FAIL, (int32_t)ier);
        return OTA_SINK_ERR_WRITE_FAIL;
    }

    xSemaphoreTake(s_sink.lock, portMAX_DELAY);
    if (!sink_epoch_is_current_locked(epoch)) {
        /* 写盘期间会话已被 invalidate/abort：字节计入了被丢弃的旧会话，按
         * STALE 上抛让泵任务收敛到新会话。 */
        xSemaphoreGive(s_sink.lock);
        return OTA_SINK_ERR_SESSION_STALE;
    }

    s_sink.bytes_written += (uint32_t)len;
    ota_sink_err_t ret = OTA_SINK_OK;

    /* 每 sector：发 PROGRESS + progress NVS 保存（与 ota_task 现状行为等价） */
    while (s_sink.bytes_written - s_sink.sectors_done * OTA_SECTOR_SIZE >= OTA_SECTOR_SIZE) {
        s_sink.sectors_done++;
        ota_sink_progress_t prog = {
            .epoch = s_sink.epoch,
            .target_label = s_sink.target ? s_sink.target->label : "?",
            .image_size = s_sink.image_size,
            .bytes_written = s_sink.bytes_written,
            .sectors_done = s_sink.sectors_done,
            .resumed = false,
        };
        sink_fire(OTA_SINK_EVT_PROGRESS, &prog);

        esp_err_t perr = ota_progress_save(s_sink.image_size,
                                           s_sink.sectors_done * OTA_SECTOR_SIZE,
                                           s_sink.sectors_done);
        if (perr != ESP_OK) {
            s_sink.progress_dirty = true; /* 不致命：会话继续，下 sector 重试 */
            ret = OTA_SINK_ERR_NVS;
        } else {
            s_sink.progress_dirty = false;
        }
    }
    xSemaphoreGive(s_sink.lock);
    return ret;
}

void ota_sink_epoch_invalidate(void)
{
    xSemaphoreTake(s_sink.lock, portMAX_DELAY);
    if (s_sink.state == SINK_STATE_WRITING) {
        s_sink.state = SINK_STATE_ABORT_PENDING; /* esp_ota_abort 延迟到串行方 */
    }
    s_sink.session_live = false; /* 立即生效：BUSY 释放、旧 epoch 全部作废 */
    xSemaphoreGive(s_sink.lock);
}

uint32_t ota_sink_epoch_current(void)
{
    xSemaphoreTake(s_sink.lock, portMAX_DELAY);
    uint32_t e = s_sink.session_live ? s_sink.epoch : OTA_SINK_EPOCH_INVALID;
    xSemaphoreGive(s_sink.lock);
    return e;
}

ota_sink_err_t ota_sink_session_abort(uint32_t epoch)
{
    /* 约束：写路径串行方上下文调用（泵任务），不得与 ota_sink_write 并发 */
    xSemaphoreTake(s_sink.lock, portMAX_DELAY);
    if (!s_sink.session_live || epoch != s_sink.epoch) {
        xSemaphoreGive(s_sink.lock);
        return OTA_SINK_ERR_SESSION_STALE;
    }
    if (s_sink.state == SINK_STATE_WRITING && s_sink.handle != 0) {
        (void)esp_ota_abort(s_sink.handle);
        s_sink.handle = 0;
    }
    sink_to_idle_locked();
    xSemaphoreGive(s_sink.lock);
    return OTA_SINK_OK;
}

ota_sink_err_t ota_sink_finish(uint32_t epoch)
{
    xSemaphoreTake(s_sink.lock, portMAX_DELAY);
    if (!s_sink.session_live || epoch != s_sink.epoch) {
        xSemaphoreGive(s_sink.lock);
        return OTA_SINK_ERR_SESSION_STALE;
    }
    if (s_sink.state != SINK_STATE_WRITING) {
        xSemaphoreGive(s_sink.lock);
        return OTA_SINK_ERR_NO_SESSION;
    }

    esp_ota_handle_t handle = s_sink.handle;
    s_sink.handle = 0;
    xSemaphoreGive(s_sink.lock);

    /* 不持锁做 esp_ota_end（镜像 hash 校验数百 ms 级；此窗口内会话仍 live，
     * 但泵任务在收到全部数据后才调 finish，无并发写）。 */
    esp_err_t ier = esp_ota_end(handle);

    xSemaphoreTake(s_sink.lock, portMAX_DELAY);
    if (ier != ESP_OK) {
        /* 收尾层防御（P0-1 三层防御之 3）：校验失败=错位/坏镜像最终暴露点。
         * 不再像旧 ota_task 那样任务自删——清理回 IDLE，sink 常驻自愈。
         * end 失败后 handle 仍需 abort 释放（IDF v6：end 失败时 esp_ota_ops
         * 内部已清理 context，再 abort 返回错误无害）。 */
        (void)esp_ota_abort(handle);
        sink_to_idle_locked();
        xSemaphoreGive(s_sink.lock);
        sink_fire_error(OTA_SINK_ERR_VALIDATE_FAIL, (int32_t)ier);
        return OTA_SINK_ERR_VALIDATE_FAIL;
    }
    s_sink.state = SINK_STATE_VALIDATED;
    xSemaphoreGive(s_sink.lock);

    sink_fire(OTA_SINK_EVT_VALIDATED, NULL);
    return OTA_SINK_OK;
}

ota_sink_err_t ota_sink_activate(uint32_t epoch, bool reboot_now)
{
    xSemaphoreTake(s_sink.lock, portMAX_DELAY);
    if (!s_sink.session_live || epoch != s_sink.epoch) {
        xSemaphoreGive(s_sink.lock);
        return OTA_SINK_ERR_SESSION_STALE;
    }
    if (s_sink.state != SINK_STATE_VALIDATED) {
        xSemaphoreGive(s_sink.lock);
        return OTA_SINK_ERR_NO_SESSION;
    }

    const esp_partition_t *target = s_sink.target;
    s_sink.state = SINK_STATE_ACTIVATED;
    xSemaphoreGive(s_sink.lock);

    esp_err_t ier = esp_ota_set_boot_partition(target);
    if (ier != ESP_OK) {
        xSemaphoreTake(s_sink.lock, portMAX_DELAY);
        s_sink.state = SINK_STATE_VALIDATED; /* otadata 未动，transport 可重试 */
        xSemaphoreGive(s_sink.lock);
        sink_fire_error(OTA_SINK_ERR_ACTIVATE_FAIL, (int32_t)ier);
        return OTA_SINK_ERR_ACTIVATE_FAIL;
    }

    /* progress 清零（激活成功语义，与 ota_task 现状等价） */
    (void)ota_progress_clear();

    xSemaphoreTake(s_sink.lock, portMAX_DELAY);
    s_sink.session_live = false; /* 会话完成关闭 */
    xSemaphoreGive(s_sink.lock);

    sink_fire(OTA_SINK_EVT_ACTIVATED, NULL);

    if (reboot_now) {
        esp_restart(); /* 不返回 */
    }
    return OTA_SINK_OK;
}
