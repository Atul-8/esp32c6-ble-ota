# BLE OTA 全链路深度审阅报告（TASK-003 重跑）

审阅对象：`F:\project\嵌入式项目\esp32c6ota`（固件 v1.0.1，真机 1.0.0→1.0.1 升级成功后）。以下所有结论均基于逐行精读与 IDF v6.0.2 源码核实（`esp_ota_begin`/`esp_ota_get_state_partition` 已在 WSL 内核对），官方 example 对照完成。

---

## 零、已验证为正确的部分（审阅基线）

- ERR-008 修复正确：`ble_ota_host.py:387-396` 帧合法性仅依赖 len==20 + CRC16 自校验，全零合法帧可过；`CRC16(18×0x00)=0x0000` 数学成立。
- ERR-009 修复完整：`ota_task.c` 主循环 take/give 逐路径核对——正常迭代（:139 无条件 give）、写失败（:111 give 后 goto）、完成（:130 give 后 break）、空 item（:134→:139）全部配对，count 恒 0/1，无泄漏路径残留。
- ERR-007 修复正确：fw_len 在数据分支内实时读（:118），与 example 循环条件实时读语义一致。
- 错误码一致性核对通过：START_OK=0x0001 / STOP_OK=0x0002 / sector 成功 0x0000 / 跳转 0x0002（extra=frame[4:6]=cur_sector），CRC XMODEM init 0x0000，与 `nimble_ota.c:187,390-399,541-545,564-568` 逐字节吻合。
- 分层合规：ota_core/ota_shared 0 个 printf/ESP_LOG（grep 核实，仅注释）；`main → ota_core → ota_shared` 单向，无 NimBLE 头渗入 core；PROJECT_VER 与 OTA_APP_VERSION 同为 1.0.1。
- 断电恢复的字节级结论：传输中断电 → 重启 → `esp_ota_begin(OTA_SIZE_UNKNOWN)` 全擦目标分区（IDF v6 `esp_ota_ops.c:193-198` 证实 erase_size=partition->size）+ 组件 `cur_sector` 静态归零 + 上位机 `last_acked_sector=-1` 从 sector 0 重发（`ble_ota_host.py:557`）——三方自洽，全量重传闭环成立，不会写坏镜像。**但这是"闭环=全量重传"，不是断点续传**（见 P1-6）。

---

## 一、发现分级清单

### P0（必须修）

**P0-1 传输会话错位链：BLE 断开/ACK 超时重发 → 设备 ATT 报错 → 重试全量重传 → ota_task 写偏移错位 → esp_ota_end 失败 → ota_task 自删，设备永久失去 OTA 能力（直到断电重启），且上位机报"成功"**

- 位置：`firmware/main/ota_task.c:59-159` + `firmware/components/ble_ota/src/nimble_ota.c:515-546`
- 触发路径（字节级推演）：
  1. 传输中途 BLE 断开（干扰/上位机崩溃/超距），设备**不重启**。ota_task 存活，`recv_len=N*4096` 已写入 flash，esp_ota handle 仍在。
  2. 用户重跑 flash：设备仍在广播 → 连接成功 → Start 命令。组件 Start 处理器（nimble_ota.c:527-529）只重置 `cur_sector/cur_packet/fw_buf`，**ota_task 的 recv_len、esp_ota 写偏移、saved_sectors 一概不知道新会话开始**。
  3. 上位机从 sector 0 重发，组件按 cur_sector=0 正常 ACK，ota_task 把 sector 0 数据写到 flash 偏移 N*4096 处（esp_ota_write 纯顺序写）→ **镜像整体错位**。
  4. `recv_len >= fw_len` 提前满足（或正好）→ esp_ota_end 镜像校验失败 → `OTA_ERROR`（ota_task.c:157-159）→ `vTaskDelete(NULL)`。此后组件照常收包、照常 ACK，**设备表面上一切正常**。
  5. 同样的错位也由"尾包 ACK 丢失→上位机 10s 超时重发同一 sector"触发：设备 cur_sector 已 +1，收到旧 sector → 走 sector error 分支（nimble_ota.c:196-222）→ 见 P0-3，ATT 错误杀掉会话 → 用户重跑 → 进入上述链条。
- 后果：一次链路抖动后，该设备在重启前所有 OTA 重试必然失败；且因为 P0-2 的存在，上位机会把失败报成成功。真机 45.9s/161 sectors 的成功运行未覆盖任何一条链路抖动路径。
- 修复建议（组件已 vendor 本地化，可打补丁，成本低）：
  - vendor 补丁：`ble_ota_start_write_chr` Start 分支增加会话代数计数器（如 `esp_ble_ota_get_session()`，每次 Start +1）；
  - ota_task 数据循环内检测代数变化 → `esp_ota_abort(s_out_handle)` + 重新 `esp_ota_begin` + `recv_len=0, saved_sectors=0`；
  - 兜底：esp_ota_end 失败路径不要只 `vTaskDelete`——至少打出可 grep 的失败锚点（如 `OTA_END_FAIL`），并考虑 esp_restart 自愈。

**P0-2 上位机"OTA SUCCESS"判定无设备侧依据——设备侧任何落盘失败都会被报成成功**

- 位置：`host/ble_ota_host.py:580-609`
- 问题：传输完成后以"2s×3 次扫描内重新扫到同名广播"判成功。但设备**没有重启也在广播**（P0-1 的死任务态、esp_ota_begin 失败态、esp_ota_write 失败态全都如此）→ 扫描必然命中 → 打印 `OTA SUCCESS`。
- 更尖锐的一点：正常成功时设备在收到最后一 sector 后立即 esp_restart，Stop 命令必然 ACK 超时（`ble_ota_host.py:272-276` 自己都写了 "device may be rebooting"）；而**Stop ACK 正常返回 0x0002 恰是"设备没重启"的最强失败信号**，当前只当 warning 打印后继续走成功分支。成功判定信号被完全用反。
- 修复建议：
  - 传输完成后 Stop ACK **正常返回** → 直接判 FAIL，输出"设备接受了全部 sector 但未激活重启，检查设备日志"；
  - Stop 超时 + 3-5s 内重新扫到 → 判 PASS；
  - 进一步加固：重连后读版本特征（当前 NimBLE GATT db 无 DIS 服务，可考虑 vendor 补丁在 OTA_BAR(0x8021) 读路径返回版本串），从"扫到广播"升级为"验证新版本"。
- 附带：同名设备多台同测时会误判到别的设备，`--mac` 匹配可缓解但默认 name 匹配不设防。

**P0-3 设备 0x0002 跳转 NACK 全部伴随 ATT 错误响应——上位机 sector-jump"断点续传"分支实际不可达（死代码）**

- 位置：`nimble_ota.c:196-222`（sector error：发 NACK 后 `return ESP_ERR_INVALID_STATE`）、`nimble_ota.c:633-637`（gatt handler 将非 OK 转成 `BLE_ATT_ERR_UNLIKELY`）vs `ble_ota_host.py:406-412`（jump 分支）
- 核实结论：nimble_ota.c 中**每一条** 0x0002 NACK 路径（sector index 错、EOF-after-seq-error、解密失败）都先发 indication 再 return 非 OK → GATT 层回 ATT 错误 → bleak 的 `write_gatt_char` 在等待 ACK indication 之前就抛 BleakError → `send_sector` 的 `except BaseException` 分支（:378-385）取消并丢弃 waiter → **NACK indication 即使到达也被丢弃**。jump 分支只有"写成功但设备 NACK"才会走到，而组件不存在这条路径。
- 连带影响：模块 docstring `ble_ota_host.py:50-51` 写的 "packet seq err -> device silently drops the packet and sends NO ack; our sector-end ACK timeout covers recovery" 与实现相反——设备直接回 ATT 错误，会话立刻死，根本轮不到 ACK 超时。文档与实现不符（并入本条修）。
- 修复建议：`_write_packets` 抛 BleakError 时先收割可能已到达的 NACK indication（读 `_pending` future 结果）再决定是 jump 还是 abort；或明确降级：把 jump 逻辑标注为协议预留、当前不可达，错误处理统一走"断开→重连→Start 重置"路径（该路径依赖 P0-1 的修复才安全）。

### P1（建议修）

**P1-4 Start 命令无任何 size 校验（设备、上位机两侧都没有）**
- 位置：`nimble_ota.c:515-546`（ota_total_len 照单全收：size=0 ACK 成功、size>1.75MB 分区照 ACK、传输中二次 Start 静默重置组件状态且不通知 ota_task——后者是 P0-1 的一个人为触发器）；`ble_ota_host.py:507-547`（`len(fw)==0` 直通：total_sectors=0 → 立即"transfer complete" → Stop → 设备不重启 → 又是 P0-2 假成功）。
- 修复：host 侧 `if not fw: return 1` 一行；设备侧 Start ACK 后由 app 校验 `esp_ble_ota_get_fw_length() <= 分区大小`，超限打错误锚点并主动 Stop（组件不知道分区大小，只能 app 层做）。

**P1-5 目标槽选择用 `esp_ota_get_boot_partition()` 而非 running partition（继承 example 的上游缺陷）**
- 位置：`ota_task.c:72-90`。otadata 指向坏槽、bootloader 回退启动另一槽时，`get_boot_partition` 与实际运行分区不一致 → `esp_ota_get_next_update_partition` 可能选出 running 槽 → `esp_ota_begin` 返回 `ESP_ERR_OTA_PARTITION_CONFLICT`（IDF v6 `esp_ota_ops.c:160-162` 证实）→ P0-1 同款死任务态。改用 `esp_ota_get_running_partition()` 一行修复。

**P1-6 "断点续传"名实不符（文档/注释层面的承诺超出实现）**
- 位置：`ota_progress_store.c:9-12` 头注释、`.ai/STRUCTURE.md` "续传由上位机配合 Indicate ACK 实现"。实际：跨重启=全量重传（设备侧全擦 + cur_sector 归零 + host 无 resume 参数）；会话内跳转=死代码（P0-3）。进度 NVS 当前纯可观测。建议文档改写为"进度落盘仅用于可观测性，恢复策略=全量重传"，避免后续维护者基于错误前提开发。

**P1-7 `esp_ota_begin(OTA_SIZE_UNKNOWN)` 每次上电全擦 1.75MB**
- 位置：`ota_task.c:92`（IDF v6 证实 OTA_SIZE_UNKNOWN → erase 整分区）。每次普通开机（不做 OTA）也执行全擦：开机时延 ~1-2s + flash 无谓磨损 + 擦除期间 flash 总线占用影响 BLE 启动。建议改为 Start 之后再 begin（需处理 fw_len 时序，正好与 P0-1 的会话检测合并设计），或用已知 size + OTA_WITH_SEQUENTIAL_WRITES。

**P1-8 上位机异常清理不完整**
- 位置：`ble_ota_host.py:614-617`。外层只捕获 `(BleakError, asyncio.TimeoutError, OSError)`；`plan_sector_packets` 的 ValueError（MTU 过小）、KeyboardInterrupt 直接裸抛，跳过 `ota.disconnect()`，WinRT 侧连接悬挂到 GC。建议 try/finally 包住整个会话。另外 `--mtu` 参数（:639-640）收了不生效，纯误导，删掉或实现。

### P2（记录即可）

| # | 位置 | 问题 |
|---|------|------|
| P2-9 | `ota_task.c:92-95` | `esp_ota_begin` 失败分支未特判 `ESP_ERR_OTA_ROLLBACK_INVALID_STATE`，也不打印 `esp_err_to_name`。当前架构下该错误不可达（rollback_check 先于 BLE init，见下文专门分析），但若将来有人调整启动顺序，这里就是隐雷——失败时至少应打印错误名 |
| P2-10 | `ota_task.c:154` vs `nimble_ota.c:555-556` | `vSemaphoreDelete(notify_sem)` 与组件 Stop 处理器 take 存在微秒级竞态窗口（esp_ota_end 校验耗时数百 ms，Stop 大概率落在窗口前；与上游 example 同款缺陷） |
| P2-11 | `ota_progress_store.c` | blob 只有 magic 无 CRC：位翻转时 load 出脏数据——影响面仅限 resume info 日志（可观测性），可接受；`main.c:72-81` 未区分 NOT_FOUND（无记录）与真实读错误 |
| P2-12 | `ble_ota_host.py:313-329` | 尾包尺寸角落：当最终 sector 数据长度恰为 chunk 整数倍时（4096%chunk==0，如 MTU=521/265/137/73 组合），尾包 3+chunk+2=mtu-1 超过 ATT write 上限 mtu-3 → 写失败。MTU 517 下 4096%511=8 安全 |
| P2-13 | `main.c:37` / `ota_task.c:28` | OTA_RINGBUF_SIZE 两处独立定义（后者定义未使用）；shared 常量应归 `ota_version.h`。同理 4096/517 等魔法数字：固件侧集中良好（ota_version.h），host 侧集中良好（协议常量区），两端各自集中但互不引用——协议值建议在 docs 层出一份对照表（WORKSTATE 已列 issue #10） |
| P2-14 | `ble_ota_host.py:111,272` | ACK_TIMEOUT_S=10s 用作 Stop 等待偏长（成功路径必超时，白等 10s）；可给 Stop 单独 3s |
| P2-15 | `nimble_ota.c:664,676` | 安全：特征无 `BLE_GATT_CHR_F_WRITE_ENC`，sm_bonding 配置形同虚设，任意人可连可推固件；无镜像签名（esp_ota_end 仅验结构完整性+自身 hash，非信任链）。当前版本接受，建议 v1.1：WRITE_ENC + 配对 → esp_secure_boot/签名校验。NVS 进度无 CRC 的错误影响面见 P2-11 |
| P2-16 | `ota_task.c:121-127` | 每 sector 一次 `nvs_open/set_blob/commit/close`（161 次/OTA），NVS 磨损与耗时均可在 OTA 结束时合并一次（WORKSTATE 已列速率优化项） |

### 专项回答：PENDING_VERIFY 与 esp_ota_begin 拒绝路径覆盖分析（任务点名项）

- **拒绝分支是否可达**：`esp_ota_ops.c:166-172` 证实 PENDING_VERIFY 下 `esp_ota_begin` 返回 `ESP_ERR_OTA_ROLLBACK_INVALID_STATE`。本架构中 `rollback_check()`（main.c:122）先于 controller/host init，mark valid 完成后 BLE 才起 → 广播可连时 running 槽必然已非 PENDING_VERIFY → ota_task.c:92 的拒绝分支**当前不可达**。结论：覆盖方式是"设计性消除"而非"代码处理"，成立，但依赖启动顺序不变这一隐含前提（main.c:4-8 注释已自我声明，P2-9 补防御即可）。
- **PENDING_VERIFY 期间用户重启**：mark valid 前断电/复位 → 下次上电仍 PENDING_VERIFY → rollback_check 再次执行，自检过则确认、不过则回滚。无状态恶化路径，**覆盖正确**。
- **mark valid 本身失败**：`ota_rollback.c:81-83` 走 `mark_invalid_rollback_and_reboot`，不会停留在 PENDING_VERIFY 开 BLE。正确。
- **首烧 otadata 全 FF**：`esp_ota_get_state_partition` 对无有效 otadata 记录返回 `ESP_ERR_NOT_FOUND`（`esp_ota_ops.c:1183-1203` 核实）→ `img_state_str` 显示 **"UNKNOWN"**（不是 UNDEFINED），`ota_rollback_confirm` 返回 NO_ACTION 不做自检不 mark valid。行为良性（无状态可确认），仅日志锚点观感问题（P2 级，可把 NOT_FOUND 映射为 "UNDEFINED(otadata-empty)"）。

---

## 二、生产就绪度评估

当前链路在"理想电磁环境 + 单次顺利传输"的实验室路径上是可信的：协议字节级两端吻合、0 重传真机升级成功、回滚/进度/启动顺序设计自洽、ERR-007/008/009 修复经验证无残留。但**它尚未获得"链路不顺利"场景的任何豁免权**：一次 BLE 断连或一次尾包 ACK 丢失就会触发 P0-1 错位链，且设备侧失败一律被 P0-2 报成"OTA SUCCESS"——即这套工具在最能体现价值的故障场景下会提供反向信号。加上 ota_task 失败即自删、设备无自愈，本版本定位应明确为**"开发联调工具 + 手动看护下的产线烧录"，不可作为无人值守远程升级方案**；修完 P0-1/P0-2/P0-3（三者为同一条故障链的三个断面，合计改动量不大：vendor 补丁会话代数 + host 成功判定反转为约 100 行量级）并跑过 Top5 测试后，可升级为"有人看护的现场升级"。

## 三、测试覆盖缺口 Top5（按 风险×成本 排序）

1. **传输中 BLE 断连 → 不重启重试**（触发 P0-1/P0-2/P0-3 全链）：传输到 1/3 处拔上位机蓝牙/杀进程 → 重连重试 → 验证设备日志错位写入、esp_ota_end 失败锚点、host 是否假成功。成本最低、收益最高，纯 host 侧可做。
2. **尾包 ACK 丢失重试**：用 `-v` + 断点注入或防火墙式丢包模拟 sector N ACK 超时 → 重发 → 验证 ATT Unlikely Error 与后续恢复路径。
3. **异常格式 bin 矩阵**：空文件 / 随机数据 / 截断 bin / 超分区大小（>1.75MB）/ 错 chip id——预期 esp_ota_end 失败，实际观察假成功（直接驱动 P0-2 修复验收）。含 size=0 与二次 Start 注入。
4. **ota_1→ota_0 反向 + 多轮连续 OTA（≥5 轮）**：验证槽位交替、otadata 翻转、进度 NVS 反复写擦、notify_sem/内存无泄漏（WORKSTATE 仅做过单轮 1.0.0→1.0.1）。
5. **回滚实战（issue #9，已在遗留清单）**：刷一个自检必失败版本（如 selfcheck 堆阈值临时调高）→ 验证 PENDING_VERIFY→自动回滚→旧版本 VALID 的完整链路；顺带覆盖"PENDING_VERIFY 期间重启"与"ota_data 全 FF 首烧"两个边界。

---

**涉及文件**（均为绝对路径）：
`F:\project\嵌入式项目\esp32c6ota\firmware\main\main.c`、`F:\project\嵌入式项目\esp32c6ota\firmware\main\ota_task.c`、`F:\project\嵌入式项目\esp32c6ota\firmware\components\ota_core\src\ota_rollback.c`、`F:\project\嵌入式项目\esp32c6ota\firmware\components\ota_core\src\ota_progress_store.c`、`F:\project\嵌入式项目\esp32c6ota\firmware\components\ble_ota\src\nimble_ota.c`、`F:\project\嵌入式项目\esp32c6ota\host\ble_ota_host.py`、`F:\project\嵌入式项目\esp32c6ota\firmware\partitions.csv`、`F:\project\嵌入式项目\esp32c6ota\firmware\sdkconfig.defaults`

本次为只读审阅，未改动任何代码。三个 P0 本质是同一条"链路不顺利"故障链的三个断面（设备侧会话错位 → 设备侧失败不可见 → 上位机反向判定），建议作为一组修复、用 Top5 测试项 1-3 一并验收。
