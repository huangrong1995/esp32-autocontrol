# ESP32 红外一键宏遥控器 —— 设计文档

- 日期:2026-10-24
- 状态:待确认(设计文档)
- 目标硬件:ESP32-WROOM-32 最小核心板 + 红外发射模块(DAT / VCC / GND,三线)

## 1. 意图与动机(Why)

用户有一台海信电视,日常想看特定节目时,需要在电视遥控器上完成"开机 → 等电视起来 → 方向/频道键选台 → 确认"一长串操作。用户希望把这些重复操作**录制成"宏"**,以后在手机上点一下,ESP32 就按录制时的顺序、间隔,自动把整套红外指令发完,电视自己走到想看的节目。

本工程的定位:一个**独立、轻量**的 ESP32 红外遥控器,一切交互通过手机浏览器完成。参考 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 的 ESP-IDF 工程搭建与编译方式初始化。

## 2. 目标 / 非目标 / 反目标

### 目标(Goals)
- 手机连上 ESP32 开的热点,浏览器打开网页即出现虚拟遥控器,点键即发红外。
- 红外信号库**内置在固件**里(按品牌/协议组织),用户逐个尝试后**配对**,配对结果持久化。
- 支持录制"宏":按键序列 + 每步等待时间,命名存储、多条并存。
- 支持一键回放宏,按序自动发送,可随时停止。
- 配对信息与宏数据**断电不丢**(NVS 存储)。

### 非目标(Non-Goals)
- 不做红外接收/学习(硬件只发不收;也不做逐键学习)。
- 不逐键单独配对(以"档"为单位,一档锁定整组键)。
- 不装手机 App,不做语音控制。
- 不做宏的复杂编辑(重排/插入步骤)——MVP 以"录制即所得"为准,后续可加。
- 不接入 xiaozhi-esp32 的音频/大模型体系——本工程是独立的最小工程,仅借鉴其工程结构与编译方法。

### 反目标(Anti-Goals,做到这些即算失败)
- 尝试后电视没反应,用户无法理解为什么(要求配对流程引导清晰、信号档可逐个试)。
- 回放节奏和录制时不一致,电视状态错乱(要求每步等待时间被忠实记录与回放)。
- 配对成功但断电丢失(要求持久化可靠)。
- 编译环境依赖一堆重型组件、编译困难(要求 MINIMAL_BUILD 裁剪,依赖最小化)。

## 3. 约束(Constraints)

- **芯片**:ESP32-WROOM-32(经典 xtensa ESP32,4MB Flash 最小核心板)。目标平台 `esp32`。
- **红外发射模块**:三线 DAT/VCC/GND 的 IR LED 发射板(非 HX1838 接收头)。VCC→3.3V,GND→GND,DAT→GPIO(**默认 GPIO4,Kconfig 可配置**)。DAT 驱动由 RMT 外设产生 38kHz 载波脉冲序列。
- **只发不收**:无法抓取原装遥控器码,只能靠内置库 + 逐个尝试配对。
- **ESP-IDF**:本机 `/home/hrong/workspace/code/esp-idf-6.0`(v6.0.3),与 xiaozhi-esp32 当前编译环境一致。编译流程:`idf.py set-target esp32 && idf.py build`。
- **RMT 驱动版本**:IDF 6.0 仅保留新版 RMT 驱动(`esp_driver_rmt`:`rmt_new_tx_channel`/`rmt_transmit` + 符号/字节编码器),旧版 `rmt_driver_install` API 已移除——编码层必须按新 API 实现。(builder 检查确认)
- **网络**:ESP32 开 **SoftAP**(默认 SSID `esp32-ir`,密码可配),手机连热点后浏览器访问 `http://192.168.4.1`。不依赖外部路由器/服务器,任何地方可用。
- **存储**:NVS 分区(存配对档 ID、宏数据)。

## 4. 硬件接线(待用户确认)

| 红外发射模块 | ESP32-WROOM-32 |
|---|---|
| VCC | 3.3V(部分模块可 5V,默认 3.3V) |
| GND | GND |
| DAT | **GPIO4**(默认;可在 menuconfig 改) |

> 说明:经典 ESP32 的 GPIO 输出电流有限(约 40mA/最大),大多数三线发射模块内置限流电阻可直接驱动;个别需要三极管的模块若发射距离不足,后续可加一级驱动,代码不受影响。

## 5. 总体架构与文件结构

参照 xiaozhi-esp32 的工程形态:

```
esp32-autocontrol/
├── CMakeLists.txt                    # 根:MINIMAL_BUILD ON, project(esp32_ir_remote)
├── sdkconfig.defaults                # 公共默认配置
├── sdkconfig.defaults.esp32          # 4MB Flash + 自定义分区表
├── partitions/
│   └── 4m.csv                        # nvs + factory + 预留
├── main/
│   ├── CMakeLists.txt                # 源码清单
│   ├── idf_component.yml             # 托管组件依赖(尽量少,首选 IDF 内置组件)
│   ├── Kconfig.projbuild             # IR_TX_GPIO、SoftAP 配置等
│   ├── main.cc                       # app_main:启动 WiFi + Web + IR
│   ├── ir/
│   │   ├── ir_encoder.h/.cc          # NEC 帧编码(引导码+32bit 数据)与 RMT 发送
│   │   └── ir_db.h/.cc               # 内置信号库(档表 + 按键映射)
│   ├── app/
│   │   ├── wifi_ap.cc/.h             # SoftAP 初始化
│   │   ├── web_server.cc/.h          # esp_http_server:静态页 + REST API
│   │   ├── remote_state.cc/.h        # 当前配对档、音量等会话状态
│   │   └── macro_engine.cc/.h        # 宏录制/存储/回放任务
│   └── web/
│       ├── index.html                # 虚拟遥控器 + 配对 + 宏管理 UI(嵌入固件)
│       └── ...(js/css 内联或分离)
```

依赖策略:优先使用 **ESP-IDF 内置组件**(`esp_http_server`、`nvs_flash`、`driver`/RMT、`wifi`)。不引入 LVGL、音频、大模型等重组件——这与 xiaozhi 的 MINIMAL_BUILD 思路一致,编译快、体积小。

## 6. 红外编码与内置信号库

### 6.1 编码层
- 用 **RMT 外设**产生 38kHz 载波与脉冲时序(经典 ESP32 的 RMT 支持硬件载波输出)。
- MVP 协议:**NEC**(海信等亚洲品牌 TV 主流),编码器按 NEC 时序:
  - 引导码 9ms mark + 4.5ms space;
  - 32 位数据帧:逻辑 1 = 562µs mark + 1.69ms space,逻辑 0 = 562µs mark + 562µs space;
  - 发送结束附最小间隙。
- 编码器做成协议可扩展的接口(后续如需 RC5/Sony 可加,不阻塞 MVP)。

### 6.2 信号库(ir_db)
每个"档"一个条目:

```c
typedef struct {
    const char *name;              // "Hisense-A (NEC 0x04)" 等,展示给用户
    uint16_t nec_address;         // NEC 地址码(如 0x04 / 0x00)
    // 按键 -> NEC 命令码映射
    struct { uint8_t key; uint8_t cmd; } map[IR_KEY_COUNT];
} ir_profile_t;
```

虚拟遥控器按键集合(MVP 固定):`POWER / VOL+ / VOL- / CH+ / CH- / MUTE / MENU / EXIT / OK / UP / DOWN / LEFT / RIGHT / 0-9`。

内置库内容(MVP):
- **海信档优先,多候选**:海信 NEC 地址 `0x04` 常见码组、`0x00` 广播地址码组,以及社区库整理的电源/音量/频道键码(来源:IRremoteESP8266、GitHub `Hisense-TV-IR-Codes` 等公开码库,实现时取真实码值,不编造)。
- 再放 1~2 个常见品牌通用 NEC 档(如 TCL/创维类)作为海信不中时的备选。
- 所有档完整覆盖 MVP 按键集;若有档缺个别键,该键在网页上置灰并提示。

### 6.3 配对流程
1. 网页展示信号档列表,每档旁一个"电源测试"按钮。
2. 用户点某档"电源" → ESP32 发该档的 POWER 码 → 用户看真电视是否有反应。
3. 有反应 → 网页上点"就用这档" → `POST /api/pair` 保存档 ID 到 NVS。
4. 之后整个虚拟遥控器所有键都用该档码组发送。

## 7. 网页 UI 与后端 API

### 7.1 页面结构(单页,手机竖屏)
- **遥控区**:网格按键(电源、数字、方向、确认、返回、音量、频道、静音、菜单)。
- **配对区**:信号档列表 + 逐档电源测试 + 配对按钮;显示当前已配对档。
- **宏区**:宏列表(名称、步骤数);"开始录制/停止并保存"、一键回放、删除。

### 7.2 REST API(JSON)
| 方法 | 路径 | 作用 |
|---|---|---|
| GET | `/api/status` | 当前配对档、宏列表 |
| GET | `/api/profiles` | 信号库档列表(含每档是否支持全部按键) |
| POST | `/api/pair` | body `{profile_id}` → 保存配对档 |
| POST | `/api/key` | body `{key}` → 立即发一个键(按当前配对档) |
| POST | `/api/key/raw` | body `{address, command}` → 手动发一条原始 NEC 帧(兜底:内置库全不中时,可把网上查到的真实码直接试,不需学码;界面"高级"折叠区提供) |
| POST | `/api/macro/start` | 开始录制(清空临时步骤) |
| POST | `/api/macro/step` | 录制中每按一个键调用,记录该键 + 距上一键的等待 |
| POST | `/api/macro/save` | body `{name}` → 存宏到 NVS |
| POST | `/api/macro/play` | body `{id}` → 启动回放任务 |
| POST | `/api/macro/stop` | 停止当前回放 |
| POST | `/api/macro/delete` | body `{id}` → 删除宏 |

静态资源由 `esp_http_server` 从固件内嵌数据(EMBED_FILES)提供。

## 8. 宏引擎

- **录制**:`POST /api/macro/start` 后,每个 `POST /api/macro/step` 记录 `{key, wait_ms}`;`wait_ms` = 该键距上一键按下之间的实际等待(录制者当时停顿多久就记多久)。按键立即照常发射(录制时用户能实时看到电视反应)。
- **存储**:宏 = `{name, steps:[{key, wait_ms}...]}`;每条宏一个 NVS blob,宏元数据列表存 NVS。MVP 容量:支持约 20 条宏、每条 ≤ 100 步(4MB Flash 足够)。
- **回放**:独立 FreeRTOS 任务,按序:先 `wait_ms` 再发对应键;每步之间严格复现录制节奏。支持 `POST /api/macro/stop` 随时中断。回放时网页显示当前执行到哪一步。
- **回放中按键的中断行为(已确认)**:回放中按任意键 → 立即中止回放并发出该键(急停)。开发者如需忽略/排队模式,后续可扩展。
- **开机宏保护(已确认)**:网页检测到宏首步是 POWER 时,回放前弹确认("本宏从开机开始,电视现在是否已关闭?")。
- **键按下语义**:单次按下 = 发一帧(NEC 完整 32 位帧);不模拟长按重复(电视电源/音量单发即可;需要时后续加长按)。宏回放与单键发送共用同一编码/发送函数。

## 9. 持久化(NVS)

- `pair_profile`(u8):当前配对档 ID,默认 0xFF(未配对)。
- `macro_index`(blob):宏名列表 + id 映射。
- `macro_<id>`(blob):宏步骤数据。
- NVS 分区尺寸 **32KB**(builder 核实:20 条宏 × 100 步 + 索引绰绰有余)。

## 10. 编译方法(照 xiaozhi-esp32)

```bash
cd esp32-autocontrol
source /home/hrong/workspace/code/esp-idf-6.0/export.sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

- `sdkconfig.defaults`:公共项(尺寸优化、组件裁剪)。
- `sdkconfig.defaults.esp32`:Flash 4MB、`CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/4m.csv"`。
- 根 CMakeLists 打开 `MINIMAL_BUILD ON`,去掉重组件,保证首次编译快。

## 11. 里程碑(MVP 范围)

1. **M1 骨架**:工程初始化(CMake、分区表、sdkconfig、main.cc 空转、编译通过、烧录打印日志)。
2. **M2 红外发送**:NEC 编码器 + RMT 发送 + 海信候选档码表;串口命令测试发"电源"。
3. **M3 Web + 配对**:SoftAP、静态页面、REST API、信号档列表与电源测试、配对持久化。
4. **M4 宏**:录制/存储/回放/停止,网页宏管理。
5. **M5 打磨**:错误提示、回放进度显示、文档(README 接线与编译)。

> 若 M2 阶段在真实电视上验证某海信候选档有效,即可提前固化该档为首选。

## 12. 留给 Builder 的决定(不需用户回答)

- DAT GPIO 默认值若与用户实际接线不符,通过 Kconfig 改即可(设计默认 GPIO4)。
- Web UI 的视觉细节、REST 具体 JSON 字段命名。
- NVS 键名与宏 ID 分配策略(自增 u8)。
- 信号库中海信码组的具体码值选取与排序。

## 13. 用户确认的决定(2026-10-24)

1. **红外发射模块 DAT 默认 GPIO4**(Kconfig 可改;未接线,用户确认默认)。
2. **SoftAP 默认 `esp32-ir`**,无密码优先,均 Kconfig 可配。
3. **宏 MVP 以"录制即所得"为准**,不做步骤重排(后续可加)。
4. **内置库范围**:海信档优先;用户暂无其它设备需求,先做海信 + 少量通用 NEC 档。
5. **回放中按键 → 中止回放并立刻发该键**(急停语义,见 §8)。
6. **内置库全不中的兜底 → 持续扩展内置库**(从公开码库增加品牌/地址家族);`/api/key/raw` 保留为开发与码源验证调试接口,不作为面向用户的兜底流程。
7. **开机宏(首步 POWER)→ 回放前网页弹确认**(见 §8)。
8. **海信具体型号未提供**(用户可提供 MAC,但 MAC 无法确定红外码组,不采用);按"海信多地址家族(`0x04`/`0x00`/字节交换变体)并列 + 逐个尝试"处理。用户若稍后补型号,优先级排序可再优化。
