# ESP32 红外一键宏遥控器 —— 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 用 ESP32-WROOM-32 + 三线红外发射模块做一个"手机网页遥控 + 一键宏回放"的独立工程:手机连热点,浏览器里点键发红外;把"开机→选台→确认"整套操作录成宏,一键回放。

**Architecture:** 参考 xiaozhi-esp32 的 ESP-IDF v6.0 工程形态(`MINIMAL_BUILD` 裁剪 + `main/` 单组件 + 自定义分区表 + sdkconfig.defaults 分层),但只保留最小依赖(仅 IDF 内置组件,无 LVGL/音频)。数据流:`ir_db`(内置信号库)→ `ir_encoder`(NEC 编码 + RMT 发送)→ `web_server`(SoftAP + esp_http_server + REST API + 内嵌网页)→ `macro_engine`(录制/存储/回放任务)。配对与宏持久化在 NVS。

**Tech Stack:** ESP-IDF v6.0.3(本机 `/home/hrong/workspace/code/esp-idf-6.0`)、目标 `esp32`、C、RMT(esp_driver_rmt 新 API)、esp_http_server、nvs_flash、SoftAP。无第三方组件依赖。

**Spec:** [设计文档](docs/superpowers/specs/2026-10-24-esp32-ir-macro-remote-design.md)

## Global Constraints

(从 spec 逐条照抄,所有任务隐含遵守)

- 芯片 ESP32-WROOM-32(xtensa esp32),4MB Flash,目标 `esp32`。
- IDF = `/home/hrong/workspace/code/esp-idf-6.0`(v6.0.3);编译:`source <IDF>/export.sh && idf.py set-target esp32 && idf.py build`。
- **仅用 IDF 内置组件**,不引入第三方托管组件(不引入 LVGL/音频/大模型)。
- RMT 用 **IDF 6.0 新驱动 API**:`rmt_new_tx_channel` / `rmt_transmit`(旧 `rmt_driver_install` 已移除)。
- NEC 时序标准值:引导码 9ms mark + 4.5ms space;逻辑 1 = 562µs mark + 1690µs space;逻辑 0 = 562µs mark + 562µs space;载波 38kHz。
- 红外发射 DAT 脚默认 **GPIO4**,通过 Kconfig(`IR_TX_GPIO`)可配;VCC→3.3V,GND→GND。
- SoftAP 默认 SSID `esp32-ir`、无密码,手机连热点访问 `http://192.168.4.1`;SSID/密码 Kconfig 可配。
- NVS 分区 **32KB**;分区块 ≤ 约 500KB,单条宏 ≤ 100 步。
- 虚拟遥控器按键集合(MVP 固定):`POWER VOL+ VOL- CH+ CH- MUTE MENU EXIT OK UP DOWN LEFT RIGHT 0 1 2 3 4 5 6 7 8 9`。
- 回放中按任意键 → **立即中止回放并发出该键**(急停)。
- 宏首步为 POWER → 回放前网页**弹确认**("本宏从开机开始,电视现在是否已关闭?")。
- 内置库以**海信多地址家族(`0x04`/`0x00`/字节交换变体)+ 少量通用 NEC 档**为主;海信码值取自公开码库。
- `/api/key/raw`(手动输入 NEC 地址/命令)保留为**开发/码源验证调试接口**,不作为用户兜底流程。

## Review Focus

spec 隐含、但单靠任务自身测试很难覆盖的输入类,一行为一条,最可能先坑人的排最前。每条在所属任务的测试里钉住:

- **NEC 帧校验位/反码**:地址/命令反码错误会整帧无效。任务 2 的编码器测试用"真值帧"(海信 0x04 地址)校验输出位序列,并验证地址反码 = `~addr & 0xFF`。
- **RMT 载波频率**:配置错误(未设 38kHz)发射无效。**作为任务 2 的代码审查关注项**(host 自测够不到硬件),审查时核对 `ir_tx.cc` 内 `rmt_apply_carrier` 的 `frequency_hz == 38000`。
- **宏回放中按键**:回放任务被单键发送打断时不能死锁/双发。任务 4 测试"回放启动→立即按键→回放中止且键已发"。
- **宏首步 POWER 确认**:网页不发宏直到用户确认。任务 4 的 UI 逻辑测试强制该分支。
- **NVS 未配对 / 宏不存在**:`GET /api/status` 返回"未配对"空态、`play` 不存在的宏返回 404,不能崩溃。
- **SoftAP 启动失败 / 网页大文件**:HTTP 请求处理不当会 OOM。任务 3 用一次"发整个 index.html"的请求验证不崩溃、响应完整。

---
### Task 1: 工程骨架(编译通过的"最小空工程")

**Files:**
- Create: `CMakeLists.txt`
- Create: `sdkconfig.defaults`
- Create: `sdkconfig.defaults.esp32`
- Create: `partitions/4m.csv`
- Create: `main/CMakeLists.txt`
- Create: `main/idf_component.yml`
- Create: `main/Kconfig.projbuild`
- Create: `main/main.cc`
- Create: `main/ir/ir_encoder.h`
- Create: `main/ir/ir_encoder.cc`
- Create: `main/ir/ir_db.h`
- Create: `main/ir/ir_db.cc`
- Create: `main/app/remote_state.h`
- Create: `main/app/remote_state.cc`
- Create: `main/app/macro_engine.h`
- Create: `main/app/macro_engine.cc`
- Create: `main/app/wifi_ap.h`
- Create: `main/app/wifi_ap.cc`
- Create: `main/app/web_server.h`
- Create: `main/app/web_server.cc`
- Create: `main/web/index.html`
- Test: (编译 + 烧录冒烟,见步骤)

**Interfaces:**
- Produces: `ir_encoder.h` 的 `void ir_send_key(ir_key_t key)`(占位声明,任务 2 实现);`ir_db.h` 的 `const char *ir_db_get_profile_name(uint8_t profile_id)`(占位);`remote_state.h` 的 `typedef enum { ... } ir_key_t` 与按键集合;`web_server.h` 的 `void web_server_start(void)`;`wifi_ap.h` 的 `void wifi_ap_start(void)`;`macro_engine.h` 的 `void macro_engine_init(void)`。此时均为声明/空实现,保证链接通过。

- [ ] **Step 1: 写根 CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(esp32_ir_remote)
```

- [ ] **Step 2: 写 `sdkconfig.defaults`(公共)**

```ini
CONFIG_COMPILER_OPTIMIZATION_SIZE=y
CONFIG_COMPILER_CXX_EXCEPTIONS=n
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_HTTPD_MAX_REQ_HDR_LEN=2048
CONFIG_HTTPD_MAX_URI_LEN=2048
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/4m.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_ESP_TASK_WDT_TIMEOUT_S=20
```

- [ ] **Step 3: 写 `sdkconfig.defaults.esp32`**

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/4m.csv"
```

- [ ] **Step 4: 写 `partitions/4m.csv`**

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x8000,
phy_init, data, phy,     0x11000, 0x1000,
factory,  app,  factory, 0x12000, 0x1E0000,
```

> NVS 32KB(0x8000)。app 从 0x12000 起,占 ~1.9MB,4MB 分区剩余留给后续扩展。

- [ ] **Step 5: 写 `main/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS
        "main.cc"
        "ir/ir_encoder.cc"
        "ir/ir_db.cc"
        "app/remote_state.cc"
        "app/macro_engine.cc"
        "app/wifi_ap.cc"
        "app/web_server.cc"
    INCLUDE_DIRS "."
    REQUIRES driver esp_http_server nvs_flash esp_wifi esp_netif esp_event
    EMBED_FILES "web/index.html"
)
```

> 依赖只用内置组件。`REQUIRES` 里的组件在步骤 6 的占位实现被用到;若个别组件此阶段未实际 include,保留也无害(esp_http_server 等是 IDF 标准组件)。

- [ ] **Step 6: 写 `main/idf_component.yml`**

```yaml
## IDF Component Manager Manifest File
dependencies:
  idf:
    version: ">=6.0.1"
```

- [ ] **Step 7: 写 `main/Kconfig.projbuild`**

```kconfig
menu "IR Remote Config"
config IR_TX_GPIO
    int "Infrared TX GPIO number"
    default 4
    range 0 39
    help
        GPIO number connected to the IR transmitter DAT pin.
config AP_SSID
    string "SoftAP SSID"
    default "esp32-ir"
config AP_PASSWORD
    string "SoftAP Password (empty = open)"
    default ""
endmenu
```

- [ ] **Step 8: 写 `main/ir/ir_encoder.h`(占位声明 + 按键枚举)**

```c
#pragma once
#include <stdint.h>

typedef enum {
    IR_KEY_POWER, IR_KEY_VOL_UP, IR_KEY_VOL_DOWN, IR_KEY_CH_UP, IR_KEY_CH_DOWN,
    IR_KEY_MUTE, IR_KEY_MENU, IR_KEY_EXIT, IR_KEY_OK,
    IR_KEY_UP, IR_KEY_DOWN, IR_KEY_LEFT, IR_KEY_RIGHT,
    IR_KEY_0, IR_KEY_1, IR_KEY_2, IR_KEY_3, IR_KEY_4,
    IR_KEY_5, IR_KEY_6, IR_KEY_7, IR_KEY_8, IR_KEY_9,
    IR_KEY_COUNT
} ir_key_t;

void ir_send_key(ir_key_t key);
```

- [ ] **Step 9: 写 `main/ir/ir_db.h`(占位)**

```c
#pragma once
#include <stdint.h>

#define IR_PROFILE_COUNT 4   /* 占位;任务 2 扩展 */
const char *ir_db_get_profile_name(uint8_t profile_id);
uint8_t ir_db_get_profile_count(void);
```

- [ ] **Step 10: 写 `main/app/remote_state.h`(占位)**

```c
#pragma once
#include <stdint.h>

uint8_t remote_state_get_profile(void);
void remote_state_set_profile(uint8_t profile_id);
```

- [ ] **Step 11: 写 `main/app/macro_engine.h`(占位)**

```c
#pragma once
void macro_engine_init(void);
```

- [ ] **Step 12: 写 `main/app/wifi_ap.h`(占位)**

```c
#pragma once
void wifi_ap_start(void);
```

- [ ] **Step 13: 写 `main/app/web_server.h`(占位)**

```c
#pragma once
void web_server_start(void);
```

- [ ] **Step 14: 写这些文件的空实现 + `main.cc`**

空实现(各 .cc 返回默认值/空函数,保证链接):`ir_encoder.cc` 的 `ir_send_key` 打一行日志(占位);`ir_db.cc` 返回 `"uninitialized"` / count 0;`remote_state.cc` 返回 0xFF(未配对);`macro_engine.cc` 空函数;`wifi_ap.cc` 打印"未实现";`web_server.cc` 打印"未实现"。

`main.cc`:

```c
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    printf("esp32-ir-remote boot OK\n");
    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
```

- [ ] **Step 15: 写 `main/web/index.html`(占位,任务 3 填充)**

一个最小可渲染占位页,`<h1>esp32-ir-remote</h1>`,无 JS。

- [ ] **Step 16: 编译验证**

```bash
source /home/hrong/workspace/code/esp-idf-6.0/export.sh
cd /home/hrong/workspace/code/esp32-autocontrol
idf.py set-target esp32
idf.py build
```

Expected: 构建成功,生成 `build/esp32_ir_remote.bin`。

- [ ] **Step 17: 烧录冒烟**

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Expected: 串口输出 `esp32-ir-remote boot OK`。(如无设备接 /dev/ttyUSB0,则以"构建成功"为通过标准并注明。)

- [ ] **Step 18: Commit**

```bash
git init && git add -A
git commit -m "feat: skeleton ESP32 IR remote project (M1)
Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---
### Task 2: NEC 编码器 + RMT 发送 + 内置信号库

**Files:**
- Modify: `main/ir/ir_encoder.h`
- Modify: `main/ir/ir_encoder.cc`
- Modify: `main/ir/ir_db.h`
- Modify: `main/ir/ir_db.cc`
- Modify: `main/CMakeLists.txt`(如需)
- Create: `main/ir/test_ir_encoder.c`(本地自测,编译期宏开关,非托管单测)

**Interfaces:**
- Consumes: `ir_key_t`(任务 1);`remote_state_get_profile()`(任务 1)。
- Produces: `ir_encoder.h` 最终:`void ir_send_key(ir_key_t key)`(实现:取当前配对档→查码→发 NEC 帧);`void ir_send_raw_nec(uint16_t address, uint8_t command)`(调试/raw 用);`void ir_set_profile(uint8_t profile_id)`(当前发送用档)。
  `ir_db.h` 最终:`typedef struct { const char *name; uint16_t nec_address; uint8_t cmds[IR_KEY_COUNT]; } ir_profile_t;` + `const ir_profile_t *ir_db_get_profile(uint8_t id)` + `uint8_t ir_db_get_profile_count(void)` + **`uint8_t ir_db_find_key(const ir_profile_t *p, ir_key_t key, uint16_t *addr, uint8_t *cmd)`**(返回 0=未定义该键,`*addr`/`*cmd` 输出地址与命令;见步骤)。

- [ ] **Step 1: 写 IR DB 数据结构(`ir_db.h` 最终版)**

```c
#pragma once
#include <stdint.h>
#include "ir_encoder.h"   /* IR_KEY_COUNT */

typedef struct {
    const char *name;
    uint16_t nec_address;
    uint8_t cmds[IR_KEY_COUNT];   /* 未定义键 = 0xFF */
} ir_profile_t;

const ir_profile_t *ir_db_get_profile(uint8_t profile_id);
uint8_t ir_db_get_profile_count(void);
```

- [ ] **Step 2: 填充 `ir_db.cc` 内置库(海信多家族 + 通用 NEC)**

按公开码库整理真实值(IRremoteESP8266、GitHub Hisense-IR-Codes 等;实现者需查证真实码值,不编造),至少包含:

```
profile[0]: "Hisense-A (NEC 0x04)"      addr 0x04   power=0x1F 等(查证)
profile[1]: "Hisense-B (NEC 0x00)"      addr 0x00   power=...(查证)
profile[2]: "Hisense-C (NEC swapped)"   addr 字节交换变体(查证)
profile[3]: "Generic NEC TV"            addr 0x04   通用键码
```

> 步骤给出码表**形状**;具体码值以公开库为准,这是本任务唯一"去查"的授权点。若某键未知,`cmds[键]=0xFF`。

- [ ] **Step 3: 实现 `ir_send_raw_nec(address, command)`——纯编码器(无硬件)**

编码器把 NEC 帧转成 `rmt_symbol_t[]`(引导码 9ms/4.5ms + 地址高字节 + 地址低字节(反码) + 命令高字节 + 命令低字节(反码) + EOF)。实现时引用本机 IDF 头:`rmt_types.h` 的 `rmt_symbol_t`、`driver/rmt_tx.h`。

- [ ] **Step 4: 实现 RMT 发送通道初始化 + `ir_send_key`**

```c
static void rmt_tx_init(void)   /* 只初始化一次 */
rmt_new_tx_channel({.gpio_num = CONFIG_IR_TX_GPIO, .mem_block_symbols = 64,
                     .clk_src = RMT_CLK_SRC_DEFAULT}, &chan);
rmt_apply_carrier(chan, &(rmt_carrier_config_t){
                     .frequency_hz = 38000, .duty_cycle = 33.0f, .flags.always_on = false});
rmt_enable(chan);
```

> 载波用 **`rmt_apply_carrier()`**(IDF 6.0 新 API,通道创建后单独设置;`rmt_transmit_config_t` 中已无 carrier 字段)。IDF 6.0 默认 38kHz 目标可在 Kconfig 预留 `IR_CARRIER_HZ`(默认 38000)。

`ir_send_key(key)`:读 `remote_state_get_profile()` → `ir_db_get_profile()` → `ir_db_find_key(profile, key, &addr, &cmd)` 取 addr+cmd → `ir_send_raw_nec()`;若 `ir_db_find_key` 返回 0(未定义该键),打日志并跳过发送。

- [ ] **Step 5: 写自测 `main/ir/test_ir_encoder.c`(编译期开关,`#ifdef IR_SELFTEST`)**

```c
#include <stdio.h>
#include <string.h>
/* 断言宏 + 两个自测:
   1) ir_send_raw_nec(0x04, 0x1F) 产生的符号序列与"已知真值帧"逐符号比对(写死预期数组);
   2) 反码字段:地址低字节 == ~0x04&0xFF,命令低字节 == ~0x1F&0xFF。
   通过则打印 "IR_SELFTEST_PASS"。 */
```

> 真值帧:引导码 + `0x04 0xFB 0x1F 0xE0` 的 NEC 时序。**该自测仅覆盖纯编码层(`ir_send_raw_nec` 的符号序列与反码),不调用 RMT 硬件**(载波配置作为代码审查关注项,见本计划 Review Focus 第 2 行)。这正是自测可测的边界——把 RMT 硬件调用放进 host 自测会引入无法链接的驱动依赖,且无法在无实机环境断言。

- [ ] **Step 6: 跑自测**

```bash
# 在 CMakeLists 临时加 -DIR_SELFTEST,或独立编译该文件跑 host 自测
# 实现者用自己顺手的方式;标准:输出 IR_SELFTEST_PASS
```

Expected: `IR_SELFTEST_PASS`。

- [ ] **Step 7: 烧录冒烟(可选,无实机则跳过)**

串口命令触发 `ir_send_key(IR_KEY_POWER)` 打日志"sent NEC addr=... cmd=..."。若有示波器/红外接收,可验证载波。

- [ ] **Step 8: Commit**

```bash
git add main/ir/
git commit -m "feat: NEC encoder + RMT TX + Hisense profile DB (M2)
Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---
### Task 3: SoftAP + Web 服务器 + REST API + 配对持久化

**Files:**
- Modify: `main/app/wifi_ap.cc`
- Modify: `main/app/web_server.cc`
- Modify: `main/app/remote_state.cc`
- Modify: `main/web/index.html`(实现配对 UI)
- Create: `main/web/app.js`(如需;若内联则并入 index.html)
- Modify: `main/CMakeLists.txt`(EMBED_FILES 增加 app.js)
- Create: `main/app/test_web_logic.c`(编译期自测:HTTP 分发逻辑纯函数)

**Interfaces:**
- Consumes: `ir_send_key` / `ir_send_raw_nec`(任务 2);`ir_db_get_profile_count` / `ir_db_get_profile`(任务 2);`remote_state_*`(任务 1)。
- Produces: `web_server.h`:`void web_server_start(void)`(实现:初始化 httpd + 注册 handler);REST 路由:`GET /`(index.html)、`GET /api/status`、`GET /api/profiles`、`POST /api/pair`、`POST /api/key`、`POST /api/key/raw`。`remote_state`:`nvs` 持久化 `pair_profile`。

- [ ] **Step 1: `remote_state.cc` 实现 NVS 持久化**

```c
/* 用 nvs_flash 打开 "pair" 命名空间 */
esp_err_t remote_state_set_profile(uint8_t id);   /* nvs_set_u8("pair_profile", id) + commit */
uint8_t remote_state_get_profile(void);           /* nvs_get_u8,默认 0xFF=未配对 */
```

- [ ] **Step 2: `wifi_ap.cc` 实现 SoftAP**

```c
void wifi_ap_start(void);
/* esp_netif_init + event loop + nvs_flash_init + esp_wifi AP config
   SSID=CONFIG_AP_SSID, password=CONFIG_AP_PASSWORD(空=开放)
   IP 192.168.4.1 */
```

- [ ] **Step 3: `web_server.cc` 实现静态页 + REST**

```c
void web_server_start(void);
/* httpd_start + 注册 handler:
   GET /           → 回 index.html(用 _binary_index_html_start/_end)
   GET /api/status → {"paired_profile":N,"paired_name":"...","macros":[]}  (宏列表任务4填)
   GET /api/profiles → [{id,name,keys_known:bool},...]
   POST /api/pair  → body {"profile_id":N} → remote_state_set_profile
   POST /api/key   → body {"key":"POWER"|...} → ir_send_key
   POST /api/key/raw → body {"address":0x04,"command":0x1F} → ir_send_raw_nec  */
/* 所有 handler 返回 application/json;错误码 400/404 */
```

- [ ] **Step 4: 写 `index.html` 配对 UI**

竖屏单页:配对区(档列表 + 每档"电源测试"按钮 + "就用这档"按钮)、遥控区(网格键)、宏区(占位)。点击电源测试→`POST /api/key`;确认配对→`POST /api/pair`。显示当前配对档名。

- [ ] **Step 5: 写 HTTP 分发逻辑纯函数自测 `test_web_logic.c`**

```c
/* 提取路由/解析逻辑为不依赖 httpd 的纯函数(如 parse_key_name→ir_key_t),
   用断言测:
   - key 字符串映射("POWER"→IR_KEY_POWER,"UP"→IR_KEY_UP,"VOL+"→IR_KEY_VOL_UP);
   - 非法 key 返回 -1(不崩溃);
   - 未配对时 GET /api/status 返回 paired_profile=0xFF 且 paired_name 空。 */
```

覆盖 Review Focus 第 5、6 行。

- [ ] **Step 6: 跑自测**

```bash
# 同任务 2 的 host 自测方式
Expected: 自测 PASS。
```

- [ ] **Step 7: 编译 + 烧录冒烟**

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Expected: 串口显示 SoftAP 启动日志;手机可连 `esp32-ir` 热点、浏览器打开 `192.168.4.1` 看到配对页。无实机则以构建 + 自测通过为准。

- [ ] **Step 8: Commit**

```bash
git add main/app main/web main/CMakeLists.txt
git commit -m "feat: SoftAP + web server + pairing persistence (M3)
Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---
### Task 4: 宏引擎(录制/存储/回放/中断)

**Files:**
- Modify: `main/app/macro_engine.h`
- Modify: `main/app/macro_engine.cc`
- Modify: `main/app/web_server.cc`(新增宏路由)
- Modify: `main/web/index.html`(宏 UI + 回放进度 + 开机宏确认)
- Modify: `main/CMakeLists.txt`
- Create: `main/app/test_macro_engine.c`(自测)

**Interfaces:**
- Consumes: `ir_send_key`(任务 2);`remote_state`(任务 3);已有 REST 基础设施(任务 3)。
- Produces: `macro_engine.h` 最终接口:
  `esp_err_t macro_start_recording(void);`
  `esp_err_t macro_record_step(ir_key_t key, uint32_t wait_ms);`
  `esp_err_t macro_save(const char *name);`
  `esp_err_t macro_play(uint8_t id);`
  `esp_err_t macro_stop(void);`
  `esp_err_t macro_delete(uint8_t id);`
  `uint8_t macro_get_count(void);`
  `const macro_info_t *macro_get_list(void);`(`macro_info_t` 含 id/name/step_count/first_key)

- [ ] **Step 1: 定义宏数据结构 + 存储布局(NVS)**

```c
typedef struct { ir_key_t key; uint32_t wait_ms; } macro_step_t;
typedef struct { uint8_t id; char name[24]; uint16_t step_count; ir_key_t first_key; } macro_info_t;
/* NVS 命名空间 "macros":macro_index(blob,宏列表)、macro_<id>(blob,steps)。
   id 自增 0..;删除后复用/compact 策略:最简单=id 不回收,直到 255 再 full-wipe(或读侧过滤)。 */
```

- [ ] **Step 2: 录制状态机**

```c
/* 录制中标志 + 临时 steps 数组(上限 100)。
   macro_record_step(key, wait_ms):wait_ms 由调用方(web handler)用
   esp_timer_get_time() 距上一次 step 计算后传入。
   录制同时照常 ir_send_key(key)(用户实时看反应)。
   macro_save(name):把临时 steps 写 NVS + 更新 macro_index。 */
```

- [ ] **Step 3: 回放任务(FreeRTOS 独立任务)**

```c
/* 状态机:IDLE / PLAYING / RECORDING。
   回放任务按序:先 sleep(wait_ms) 再 ir_send_key(key)。
   macro_stop():置停止标志,任务退出。
   回放中按任意单键 → 置停止标志 + 直接 ir_send_key(该键)(急停语义)。
   宏首步 first_key==IR_KEY_POWER 且未确认 → web 层拦截(见步骤 5)。 */
```

覆盖 Review Focus 第 3 行。

- [ ] **Step 4: 写回放/录制中断自测 `test_macro_engine.c`**

```c
/* host 自测(用假 send_key 替换真实 RMT,编译宏开关):
   - 录制 3 步(wait_ms 已知)→ save → 读取回放步序与 wait_ms 一致;
   - 回放启动 → 立即触发"按键中断" → 回放中止且最后一次 send 的是该键;
   - 回放不存在的 id 返回错误。 */
```

- [ ] **Step 5: 网页宏 UI + 开机宏确认**

```html
<!-- 宏列表(名称/步骤数) + 播放/删除;
     回放中显示"执行到第 N 步";
     播放前:若 first_key==POWER 弹确认框,确认后才 POST /api/macro/play。 -->
```

- [ ] **Step 6: 补 REST 宏路由**

```c
/* POST /api/macro/start  POST /api/macro/step
   POST /api/macro/save   POST /api/macro/play
   POST /api/macro/stop   POST /api/macro/delete
   GET  /api/status 的 macros 列表填充真实数据。 */
```

覆盖 Review Focus 第 5 行(未配对/宏不存在 404)。

- [ ] **Step 7: 跑自测**

Expected: `test_macro_engine.c` 全部 PASS。

- [ ] **Step 8: 编译 + 烧录冒烟**

```bash
idf.py build && idf.py -p /dev/ttyUSB0 flash monitor
```

Expected: 网页能录/存/列/删/播宏;回放可被任意键打断;POWER 开头宏弹确认。无实机以构建 + 自测为准。

- [ ] **Step 9: Commit**

```bash
git add main/app main/web main/CMakeLists.txt
git commit -m "feat: macro record/replay/interrupt engine (M4)
Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---
### Task 5: 打磨 + README + 收尾

**Files:**
- Create: `README.md`(接线图 + 编译烧录 + 使用流程 + 常见问题)
- Modify: `main/web/index.html`(错误提示、空态、加载态)
- Modify: `main/app/web_server.cc`(统一 JSON 错误码与日志)
- Modify(如需要):`docs/`

**Interfaces:**
- Consumes: 任务 1-4 全部。
- Produces: 可交付使用的工程 + 用户文档。

- [ ] **Step 1: 写 `README.md`**

接线、编译(export + set-target + build)、烧录、使用(配对→测试→确认→录制→回放)、常见问题(信号不中→换档/查公开码库;中断回放;分区说明)。

- [ ] **Step 2: 网页打磨**

所有 REST 错误显示人性化中文提示;配对区空态("未配对,请从列表选档测试");回放/录制中按钮状态互斥。

- [ ] **Step 3: 全量自测 + 构建复核**

```bash
# 三个自测文件全跑
# idf.py build 全绿
```

- [ ] **Step 4: 全工程走查(Review Focus 复查)**

逐个过 Review Focus 六条:真值帧校验、载波频率断言、回放中断、POWER 确认、未配对空态、大文件请求。

- [ ] **Step 5: Commit**

```bash
git add README.md main/web main/app
git commit -m "chore: polish UI, docs, final review (M5)
Co-Authored-By: Claude Code <noreply@anthropic.com>"
```
