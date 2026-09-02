# META 规则汇总（跨项目共享错题本）

### META-001-DEPENDENCY: 第三方组件 undefined reference 可能是要求 app 提供符号的隐式契约

- **规则**: 引入第三方组件（managed_components）后链接报 `undefined reference to <符号>` 时，先在组件源码中 grep `extern <符号>`；命中则该符号不是漏链接库，而是组件要求 app 全局定义（反向依赖）。改造官方 example 代码时，禁止 static 化任何被组件 extern 引用的符号
- **适用场景**: 对接 espressif 组件管理器上的第三方组件；改造官方 example 代码为本项目分层结构
- **源错误**: ERR-005（ble_ota v0.1.17 的 `notify_sem`：组件 Stop 处理器 `extern SemaphoreHandle_t notify_sem` 反向引用 app 全局）
- **检查方式**: 链接失败时 `grep -rn "extern.*<符号>" managed_components/`；代码审查时对 example 改造 diff 检查全局符号的 static 化
- **类别(category)**: DEPENDENCY
- **关联层(layer)**: interface, core
- **关联专家(applies_to)**: embedded-firmware-engineer
- **触发关键词(keywords)**: undefined reference, extern, managed_components, ble_ota, notify_sem, 链接错误, 隐式契约
- **embedding**: (预留)

### META-002-BUILD: IDF v6 下 ble_ota 组件 host_init 不含 controller 初始化

- **规则**: esp-idf v6 下 `esp_ble_ota_host_init()`（nimble 分支）只调用 `esp_nimble_init()`（仅 host 栈，见 nimble_port.c 源码）；controller init/enable 必须由 app 显式调用 `esp_bt_controller_init/enable`（照官方 example 非 protocomm 分支）。旧文档"host_init 内部含 controller init"对 v6 不成立
- **适用场景**: ESP-IDF v6 + ble_ota 组件（NimBLE 模式）集成
- **源错误**: TASK-002 编码期源码核实发现（未成错，记录防线）；`nimble_port_init()` 才含 controller init，组件未调用它
- **检查方式**: app_main 审查：esp_bt_controller_init/enable 是否先于 esp_ble_ota_host_init()；遗漏的运行症状是 host 同步永不完成、广播不启动
- **类别(category)**: BUILD
- **关联层(layer)**: interface
- **关联专家(applies_to)**: embedded-firmware-engineer
- **触发关键词(keywords)**: ble_ota, esp_ble_ota_host_init, esp_nimble_init, controller init, NimBLE, IDF v6
- **embedding**: (预留)

### META-003-ASYNC: 异步回调中读取的全局配置，写入必须先于 init 或挂接同步点

- **规则**: 组件/库在异步回调（host sync、连接事件等）里读取全局配置（设备名、广播参数等）时，app 侧写入必须先于 init 调用，或显式挂接到同步点之后（注册同步回调内再改+重启受影响的外设状态）；禁止以"我的写代码在函数返回之前执行"推断竞态胜出。验收外设行为以对端实测（空口广播、抓包）为准，日志锚点只反映软件视角
- **适用场景**: NimBLE/Bluedroid 广播名与广播数据、任何"init 启动后台任务+回调消费配置"的框架集成
- **源错误**: ERR-006（ble_ota 组件 esp_ble_ota_on_sync() 内构造广播读取 GAP 名；app_main host_init 后才 name_set，真机广播定格为组件默认名 nimble-ble-ota）
- **检查方式**: 审查 init 与配置写入的顺序：配置 → init；或存在"同步事件 → 改配置 → 重建外设状态"的显式链路。对广播类配置用 BLE 扫描实测名称
- **类别(category)**: ASYNC
- **关联层(layer)**: interface, core
- **关联专家(applies_to)**: embedded-firmware-engineer, pc-host-engineer
- **触发关键词(keywords)**: 竞态, race, 异步回调, 广播名, adv data, nimble, on_sync, ble_svc_gap_device_name_set, 时序
- **embedding**: (预留)

### META-004-CONCURRENCY: 命令注入型跨任务参数禁止启动时缓存

- **规则**: 由外部命令（对端协议命令、其他任务）写入、任务循环消费的参数，消费任务禁止在启动时一次性缓存；必须每次实时读取，或显式等待"已注入"信号后再消费。缓存合法的前提是存在明确的 happens-before 边（信号量/队列传递）
- **适用场景**: OTA fw_length、采样配置、任何"Start 命令带参数 → worker 循环用参数"模式
- **源错误**: ERR-007（ota_task.c:65 启动时缓存 esp_ble_ota_get_fw_length() 恒为 0；官方 example 在循环条件实时调用。首 sector 落盘即 recv_len>=0 提前 esp_ota_end）
- **检查方式**: 审查 worker 任务入口处的 const 缓存提取 diff：被缓存的值是否由另一个任务/对端命令在任务启动后才写入？是则改为实时读或加就绪等待
- **类别(category)**: CONCURRENCY
- **关联层(layer)**: core, interface
- **关联专家(applies_to)**: embedded-firmware-engineer, pc-host-engineer
- **触发关键词(keywords)**: fw_length, 缓存, 跨任务, happens-before, ota_task, example 移植, 时序屏障
- **embedding**: (预留)

### META-005-DATA_INTEGRITY: ACK 帧合法性只能由自校验字段判定，禁止外观检查（非零/固定头）

- **规则**: 解析带校验和的应答帧时，帧有效性判定只能依赖与载荷绑定的自校验字段（CRC、长度、magic 与载荷的绑定关系）；禁止用"字节非零""与载荷无关的固定头值"等外观检查。存在全零/全 0xFF 合法帧的协议中（如零填充 ACK 且 CRC(全零)=0），"看起来像坏帧"的帧必须先过 CRC 再下结论。同一协议不同通道的帧格式（有无帧头）必须分别核对源码，禁止由一个通道的格式泛化另一个
- **适用场景**: 任何带尾部 CRC 的二进制 ACK/应答帧解析；上位机/下位机协议对接联调
- **源错误**: ERR-008（ble_ota RECV_FW sector ACK 无固定头、frame[0:2]=回显 sector；上位机误用 COMMAND ACK 的 0x0003 头检查，sector 0 全零成功 ACK 被拒→重发→传输中止）
- **检查方式**: 代码审查 ACK 解析分支：是否只出现 len + CRC 判定？是否存在与 CRC 并列的"magic/非零"判定？对每个判定构造极端帧（全零、全 FF、CRC 破坏）做单测；核对协议两端源码逐字节填充位置（含"回显值 vs 期望值"语义）
- **类别(category)**: DATA_INTEGRITY
- **关联层(layer)**: interface, core
- **关联专家(applies_to)**: pc-host-engineer, embedded-firmware-engineer
- **触发关键词(keywords)**: ACK, CRC16, XMODEM, 全零帧, 坏帧误判, 固定头, 回显 sector, 帧格式, 协议联调
- **embedding**: (预留)

### META-006-CONCURRENCY: example 的信号量 take/give 配对是组件回路的隐式协议，移植必须逐路径保全

- **规则**: 从官方 example 移植"信号量包裹临界段"的循环体时，take/give 配对是流水线两端共同依赖的隐式协议：正常迭代路径必须在循环体末尾无条件 give，任何一条路径（continue/break/goto/错误分支）丢失 give 都会让 count 单调泄漏，最终把生产者-消费者两端任务先后永久阻塞。症状特征：消费任务先死于自体 take（日志停在中途），生产者随后死于队满的 portMAX_DELAY send（对端表现为"最后一包 Unreachable"），全程无 panic、WDT 不复位、广播不恢复。互斥用法下动态 count 恒在 0/1 值域——count 单调递减即为泄漏
- **适用场景**: example 移植、生产者-消费者 ringbuf 流水线、组件与应用共享同步原语（如 ble_ota 的 notify_sem 同时被组件 Stop 处理器 take，META-001 隐式契约）
- **源错误**: ERR-009（ota_task.c 移植时丢失 example 循环末尾的无条件 give：ota_task 处理完 sector 0 后永久阻塞在自体 take → ringbuf 塞满 → NimBLE host 任务阻塞在 xRingbufferSend(portMAX_DELAY) → BLE 链路无应答、不广播，仅硬复位可恢复）
- **检查方式**: 代码审查循环内每个 take：从循环入口到每个出口（含 goto）逐路径核对 give 可达；diff 对照 example 时把同步原语语句逐行对齐；审查"互斥信号量"的动态 count 是否可能脱离 0/1 值域
- **类别(category)**: CONCURRENCY
- **关联层(layer)**: core, interface
- **关联专家(applies_to)**: embedded-firmware-engineer
- **触发关键词(keywords)**: 信号量泄漏, notify_sem, take/give 配对, 静默挂死, 任务阻塞, example 移植, 组件契约, portMAX_DELAY, 死锁
- **embedding**: (预留)

### META-007-BUILD: 构造函数/初始化路径修改后必须立即实例化冒烟；方法中部锚定编辑必查方法尾部完整性

- **规则**: 两条互补纪律。① 对既有方法做文本替换编辑时，old_string 锚点必须覆盖完整方法边界（def 行到方法最后一行），禁止在方法体中部锚定——锚点之后、下一方法之前的原方法体行会被新内容"吸收"进错误的作用域；② 任何触及构造函数/模块级初始化路径的修改，回归必须在改动后立即执行最小实例化冒烟（构造对象 + 调用一个触及全部新初始化属性的方法），不得攒批到最后统一验证
- **适用场景**: 重构类构造函数、给 __init__ 加参数、编辑工具做方法级替换、任何"在方法间插入新方法"的改动
- **源错误**: ERR-010（重构 BleakOtaClient.__init__ 加 event_cb 时，尾部 _pending/mtu_size/stats 初始化行被吸收进新插入的 emit() 方法体，属性构造后缺失，真机 dry-run 才暴露）
- **检查方式**: 审查 diff：新增方法的上一行必须是完整初始化行集，原构造函数尾部行未位移；改动后立即 `python -c "import X; X.Y()"` 级别冒烟，验证 hasattr 或构造后首方法不 AttributeError
- **类别(category)**: BUILD
- **关联层(layer)**: core, interface
- **关联专家(applies_to)**: pc-host-engineer, embedded-firmware-engineer
- **触发关键词(keywords)**: 构造函数, __init__, 重构, Edit 锚点, 方法吸收, AttributeError, 实例化冒烟, 初始化缺失
- **embedding**: (预留)

### META-008-CONCURRENCY: tkinter 资产仅主线程可触碰，worker 任务签名必须是纯数据

- **规则**: "主线程 UI + worker 线程"架构中，tkinter Variable/控件/Text 等资产与 UI 线程的 Tcl 解释器绑定，跨线程调用直接抛 `RuntimeError: main thread is not in main loop`（不是竞态，是根本禁止）。投递给 worker 的闭包/回调必须在投递方（主线程）就地解包为纯 Python 值（bool/str/dict），worker 侧禁止出现任何 `.get()`/`.cget()`/控件方法；反向更新只走队列轮询或 `after()`。类似约束同样适用于 Tk 主循环未运行时（`update()` 泵模型）跨线程访问
- **适用场景**: tkinter/PySimpleGUI 等 GUI 与 asyncio/线程 worker 混合的上位机；任何"UI 线程 + 后台任务"参数传递
- **源错误**: ERR-011（ble_ota_gui.py on_start 闭包内 dry_var.get()，worker 线程执行时崩溃，烧录启动即失败）
- **检查方式**: 审查 worker.post/submit 调用：闭包体引用的每个名字是否为主线程快照的纯值？grep worker 执行路径上的 tkinter 方法调用（Variable.get、控件 config 等）；GUI 集成必须做一次 headless 驱动冒烟（模拟按钮回调 + update() 泵）验证线程边界
- **类别(category)**: CONCURRENCY
- **关联层(layer)**: presentation, interface
- **关联专家(applies_to)**: pc-host-engineer
- **触发关键词(keywords)**: tkinter, Variable.get, 跨线程, main thread is not in main loop, GUI worker, 线程边界, 队列轮询, headless 冒烟
- **embedding**: (预留)
