/*
 * core 层：OTA 会话与写盘统一编排（ota_sink）
 *
 * 设计文档：.ai/docs/design-ota-transport-abstraction.md §3（权威）
 *
 * 职责收拢（ADR-004-1）：会话状态机 IDLE → OPEN → WRITING → VALIDATED → ACTIVATED、
 * 单写者互斥（先到先得不抢占）、会话代数 epoch（P0-1 修复核心）、进度 NVS 编排。
 * transport（BLE/WiFi/USB）只调本接口，不再自行触碰 esp_ota_ops 写盘族。
 *
 * 并发模型（重要，调用方必读）：
 *   - esp_ota_write 与 esp_ota_abort 对同一 handle 并发调用在 IDF 中语义未定义。
 *     本模块把所有 handle 清理动作（abort）约束在"写路径串行方"上下文执行
 *     （BLE=transport 泵任务）：epoch_invalidate 被任意上下文（如 NimBLE host
 *     任务的会话回调）调用时只做状态标记（WRITING → ABORT_PENDING），真正的
 *     esp_ota_abort 延迟到下一次 session_open / 显式 session_abort 时由泵任务
 *     串行执行。
 *   - 互斥锁只保护状态转换（§3.1 锁粒度）；esp_ota_write/esp_ota_end 不持锁
 *     （同步落盘/校验可达百 ms 级），写盘前后用锁内快照的 epoch 校验会话归属。
 *   - 事件回调在持锁/调用者上下文中直接执行，回调内禁止再进 sink API。
 *
 * 规则：禁止 ESP_LOG/printf（状态经返回值/事件回调上报，延续 ota_core 纪律）；
 *       禁止 include 任何 transport 头（NimBLE/http_client/usb_serial_jtag）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 错误码 ---------- */
typedef enum {
    OTA_SINK_OK = 0,
    OTA_SINK_ERR_BAD_ARG,        /* 参数非法（NULL 等） */
    OTA_SINK_ERR_BUSY,           /* 已有活动会话（单写者互斥拒绝，不抢占） */
    OTA_SINK_ERR_NO_SESSION,     /* finish/activate 状态机位置不对 */
    OTA_SINK_ERR_SESSION_STALE,  /* 会话代数已过期（P0-1 兜底信号，数据不落盘） */
    OTA_SINK_ERR_SIZE_INVALID,   /* image_size==0 或 > 目标分区容量（修 P1-4） */
    OTA_SINK_ERR_PARTITION,      /* 选槽失败 / 目标即 running 槽（修 P1-5） */
    OTA_SINK_ERR_BEGIN_FAIL,     /* esp_ota_begin 失败（P2-9：idf_err 带原值） */
    OTA_SINK_ERR_WRITE_FAIL,     /* esp_ota_write 失败 */
    OTA_SINK_ERR_VALIDATE_FAIL,  /* esp_ota_end 镜像校验失败（会话错位/坏镜像最终暴露点） */
    OTA_SINK_ERR_ACTIVATE_FAIL,  /* set_boot 失败 */
    OTA_SINK_ERR_NVS,            /* progress 持久化失败（不致命，会话继续） */
} ota_sink_err_t;

/* ---------- 事件 ---------- */
typedef enum {
    OTA_SINK_EVT_SESSION_START,  /* esp_ota_begin 成功，开始写盘 */
    OTA_SINK_EVT_PROGRESS,       /* 每写满 1 sector（4096B）一次 */
    OTA_SINK_EVT_ERROR,          /* 任一失败路径，data = ota_sink_error_t* */
    OTA_SINK_EVT_VALIDATED,      /* esp_ota_end 校验通过（镜像落盘完整） */
    OTA_SINK_EVT_ACTIVATED,      /* set_boot 完成（reboot 与否由 transport 决定） */
} ota_sink_event_t;

typedef struct {
    uint32_t   epoch;         /* 会话代数（SESSION_START 事件携带，日志锚���用） */
    const char *target_label; /* 目标分区名（如 "ota_1"），日志锚点用 */
    uint32_t   image_size;    /* 会话声明的固件总长 */
    uint32_t   bytes_written; /* 已写入字节 */
    uint32_t   sectors_done;  /* 已完成 sector 数 */
    bool       resumed;       /* 本次是否为续传会话（PR-1 恒 false，WiFi PR-2 启用） */
} ota_sink_progress_t;

typedef struct {
    ota_sink_err_t code;
    int32_t        idf_err;   /* 底层 esp_err_t 原值（0 表示无底层错误） */
} ota_sink_error_t;

typedef void (*ota_sink_event_cb_t)(ota_sink_event_t evt, const void *data, void *user_arg);

/* ---------- 会话配置 ---------- */
typedef struct {
    uint32_t   image_size;    /* 必须 >0 且 <= 目标分区 size（BLE=Start 帧 / WiFi=Content-Length / USB=START 帧） */
    const char *source_tag;   /* "ble"/"wifi"/"usb"，仅用于 transport 侧日志 */
    bool       resume;        /* true: 读 progress NVS 续写（PR-2 用）；false: 全新 begin（按已知 size 部分擦除，修 P1-7） */
} ota_sink_session_cfg_t;

/* ---------- 生命周期 ---------- */

/* app_main 早期注册一次（须在 NVS init 之后调用——内部要读分区表与 progress blob）。 */
void ota_sink_init(ota_sink_event_cb_t cb, void *user_arg);

/*
 * 打开会话：选槽（基于 running partition，修 P1-5）→ size 校验（修 P1-4）→
 * 单写者互斥（否则 BUSY，不抢占）→ esp_ota_begin（已知 size 部分擦除，修 P1-7 开机全擦）。
 * 成功时 *out_epoch = 本会话代数（0 恒为非法值），后续 write/finish/activate/abort 必须携带。
 * 仅在 transport 收到 START 语义后的首个数据到来时调用（lazy-open）。
 */
ota_sink_err_t ota_sink_session_open(const ota_sink_session_cfg_t *cfg, uint32_t *out_epoch);

/*
 * 顺序字节流写入。epoch 不等于当前会话 → 返回 OTA_SINK_ERR_SESSION_STALE，数据不落盘
 * （错位数据在进入 esp_ota_write 之前被拦截——P0-1 三层防御之写入层）。
 * 返回 OTA_SINK_ERR_NVS 仅为进度保存失败（可观测性降级），会话继续。
 * 内部完成：字节计数 → 每满 1 sector 发 PROGRESS + progress NVS 保存。
 */
ota_sink_err_t ota_sink_write(uint32_t epoch, const void *data, size_t len);

/*
 * 中止当前会话（须携带当前 epoch）：esp_ota_abort + 进度保留（可观测）+ 回 IDLE。
 * 约束：须在写路径串行方上下文调用（BLE=泵任务），不得与 ota_sink_write 并发。
 */
ota_sink_err_t ota_sink_session_abort(uint32_t epoch);

/*
 * esp_ota_end 镜像校验。失败时内部 abort 并回 IDLE + EVT_ERROR(VALIDATE_FAIL)——
 * sink 常驻等下一个会话（设备自愈，替代旧 ota_task 的 vTaskDelete 自删）。
 */
ota_sink_err_t ota_sink_finish(uint32_t epoch);

/* set_boot 激活 + progress 清零。reboot_now=true 时 esp_restart（BLE 现状语义）。 */
ota_sink_err_t ota_sink_activate(uint32_t epoch, bool reboot_now);

/* ---------- 会话代数（P0-1 修复核心） ---------- */

/*
 * 会话边界失效：transport 的"新会话开始"事件必须调用（BLE：vendor 会话回调的
 * Start；USB：ENTER/ABORT 帧；WiFi：新 HTTP 请求中止旧请求）。
 * 新会话开始 = 一切旧会话作废：此后所有携带旧 epoch 的 write 返回 STALE——
 * 错位数据不落盘（P0-1 修复的事件层+写入层组合）。
 * 注意：BLE 的 Stop/断连事件【不】调用本函数（会话收尾竞态，见 ble_transport.c
 * 文件头"收尾竞态"），它们只清 transport 侧 armed 标志；旧会话的兜底失效由
 * 下一次 Start 的 invalidate 完成。
 * 可在任意任务上下文调用（含 NimBLE host 任务），非阻塞（延迟 abort）。
 */
void ota_sink_epoch_invalidate(void);

/* 当前活动会话代数（0 = 无活动会话）。 */
uint32_t ota_sink_epoch_current(void);

#ifdef __cplusplus
}
#endif
