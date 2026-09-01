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
