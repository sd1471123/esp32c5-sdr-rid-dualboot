# ESP32-C5 双固件项目：SDR 频谱仪 + C-RID 远程识别

把两个项目合成**一个可烧录的 16 MB 镜像**，开机用同一块 ST7789 屏和 5 个按键选择进入哪套固件：

- `←` 进入 **SDR 频谱仪**（esp-sdr `tft` 分支的 C5 固件）
- `→` 进入 **C-RID**（China Remote ID 扫描/模拟固件，新增本地 TFT 显示）

两套功能各自是**独立编译的 app 镜像**，运行时互不加载、互不干扰；只在每次开机时做一次选择。

## 目录

```
C5mix/
├── launcher/          开机选择菜单（factory 分区）
├── sdr/               esp-sdr tft 分支（ota_0 分区，已打合并补丁）
├── rid/               C-RID（ota_1/ota_2 分区，已加 TFT 显示）
├── common/
│   ├── panel/         共用的 ST7789 + 5 按键 + 蜂鸣器驱动（原样取自 sdr tft 分支）
│   └── dualboot/      启动目标切换 / OTA 状态确认
├── partitions/        合并后的 16MB 分区表（各工程各有一份拷贝）
├── tools/             一键构建 / 烧录脚本
└── docs/              架构说明
```

源码基线：
- `sdr/` ← `sd1471123/esp-sdr` 的 `tft` 分支，commit `c7288b0db6e4255ca98d7a76fb12c388ad356733`
- `rid/` ← `sd1471123/esp32-ChinaRemoteID_Scan_Sim`，commit `5c50d795af86dbcd2cedfc222d8b928bde49055e`

## 硬件接线（与 esp-sdr tft 分支完全一致）

| 屏幕 8 针 | ESP32-C5 |
|---|---|
| GND | GND |
| VCC | 3V3 |
| SCL | GPIO6 |
| SDI | GPIO7 |
| RESET | GPIO5 |
| AO / DC | GPIO4 |
| CS | GPIO10 |
| BL | GPIO23（**不要直连**，用 S8050 低边开关或接 3V3 常亮） |

| 功能 | GPIO |
|---|---|
| ← | GPIO0 |
| → | GPIO1 |
| B（频段） | GPIO8 |
| M（模式） | GPIO9 |
| R（旋转） | GPIO24 |
| 蜂鸣器 | GPIO25 |

屏是 3.2" 240×320 SPI ST7789（GMT032-8P-SPI），固件按 320×240 横屏使用。
背光 LED 约 90–95 mA，务必用三极管驱动；驱动里只暴露 10% / 100% 两档，避免 PWM 调制 3V3 造成水波纹。

## 按键用法

| 场景 | 按键 |
|---|---|
| 开机菜单 | `←` 或 `B` = 进 SDR；`→` 或 `M` = 进 C-RID；`R` = 切背光 |
| SDR 固件 | 原有全部操作不变：`←/→` 走信道，`B` 切频段，`M` 切模式，`R` 短按转屏 / 长按 **5 秒**恢复出厂 |
| SDR 固件 | 组合键：`B + ←/→` 调上限、`M + ←/→` 调下限、`R + ←/→` 切背光、`← + →` 同时静音（静音状态存 NVS，开机延续） |
| SDR 固件 | **新增**：`B + M` 同时按住 1.5 秒 → 回开机菜单 |
| C-RID 固件 | `←/→` 翻显示页（状态 / 目标 / 关于 / **配置**）；目标页 `M + ←/→` 或 `B + ←/→` 翻航迹；**目标页点按 `M` 或 `B` 进入该目标详情页**（12 行大字：MAC / RSSI+信道 / 协议 / 报文数+时效 / UAS ID / 机型 / 操作员 / 坐标 / 高度+AGL / 速度+航向 / 状态+UA 类型 / 飞手位置；标签固定不动、只刷数值），详情页 `←/→` 切换目标、再点按 `M` 或 `B` 返回列表；`R + ←/→` 切背光；`← + →` 同时静音（静音状态存 NVS，开机延续） |
| C-RID 固件 | **配置页**：列表态 `←/→` 选项目、点按 `B` 进 EDIT 页；**EDIT 页编辑完态**：点按 `B` 切下一项，字符串项（UAS ID / 机型名 / 操作员 ID）用 `←/→` 移动字符光标、数值项用 `←/→` 直接改值，长按 `B` 1 秒进改值态；**改值态**：`←/→` 改当前字符/数值、字符串项点按 `B` 移下一字符、`M` 退回编辑完态；列表态或编辑完态按 `M` = 保存全部并立即生效、退回状态页；12 项含收发模式、标准、频段、信道、UAS ID、机型名、操作员 ID、经纬度、海拔、速度、广播间隔 |
| C-RID 固件 | **新增**：`B + M` 同时按住 1.5 秒 → 回开机菜单（配置页里相当于放弃未保存的修改）；`R` 点按仅蜂鸣、长按无功能 |

C-RID 的网页配置入口（`https://192.168.4.1`）和 BLE 数据通道完全保留，TFT 只是多一路本地显示。

## 分区布局（16 MB Flash）

| 分区 | 类型 | 偏移 | 大小 | 内容 |
|---|---|---|---|---|
| factory | app | `0x20000` | 1 MB | launcher 菜单 |
| sdr | app/ota_0 | `0x120000` | 2 MB | SDR 固件 |
| rid_a | app/ota_1 | `0x320000` | 3 MB | C-RID 固件 A |
| rid_b | app/ota_2 | `0x620000` | 3 MB | C-RID 固件 B（网页 OTA 目标） |
| ridlog | data/0x40 | `0x920000` | 4 MB | C-RID 接收轨迹 |
| nvs / phy_init / otadata | data | `0x9000` 起 | — | 配置 / 射频校准 / 启动槽 |

**为什么每次开机都能回到菜单**：launcher 放在 `factory`；被选中的固件启动后立刻调用
`esp_ota_set_boot_partition(factory)`，在 ESP-IDF 里这等价于擦空 `otadata`，
于是下一次冷启动 bootloader 又去启动 factory。详见 `docs/ARCHITECTURE.md`。

## 构建与烧录

需要 ESP-IDF **6.x**（本机为 `D:\esp-idf-v6.1`），且已装好 C5 工具链。脚本会自动剥离 MSYS 的
`MSYSTEM` 变量，所以可以直接从 Git Bash 跑。

```bash
# Git Bash：先设好 IDF_PATH / IDF_PYTHON_ENV_PATH，或先 source ESP-IDF 的 export.sh
bash tools/build_all.sh
bash tools/flash_all.sh -p COM6          # 整片烧录，会清 NVS
```

PowerShell：

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_all.ps1
powershell -ExecutionPolicy Bypass -File tools\flash_all.ps1 -p COM6
```

烧录地址（C5 的 bootloader 在 **0x2000**，不是 0x0）：

```
0x2000   out/bootloader.bin
0x8000   out/partition-table.bin
0x10000  out/ota_data_initial.bin     # 空的 otadata，保证开机进菜单
0x20000  out/launcher.bin
0x120000 out/sdr.bin
0x320000 out/rid_a.bin
0x620000 out/rid_b.bin
```

本机已用 ESP-IDF 6.1 实际编译通过三个工程：launcher `0x39340`、SDR `0xd85b0`、
RID `0x1b1eb0` 字节（加本地配置页后增大）。RID 构建末尾会出现一条
`Part 'factory' 0/0 @ 0x20000 size 0x100000 (overflow ...)` 警告——那是共用分区表里
1 MB 的 launcher 槽装不下 RID 镜像，**是预期的**：RID 只会烧进 3 MB 的 `rid_a` / `rid_b`，
不影响使用。

## 各工程改了什么

**launcher（新建）**
- 复用 sdr tft 分支的 `st7789.c` / `buttons.c` / `buzzer.c` / 字库，接线一字不改。
- 画两行大字菜单，按左/右键设置启动分区后重启。

**sdr/**
- `sdkconfig.defaults.esp32c5`：2 MB → 16 MB Flash，指向合并分区表，打开回滚。
- `main/families/c5_c6_c61/receiver.c`：启动时确认 OTA 状态并把启动目标还给 launcher。
- `main/local_ui/ui.c`：新增 B+M 长按回菜单；其余 UI 逻辑未动。
- 根目录 CMake 增加 `../common/dualboot` 组件。

**rid/**
- `sdkconfig.defaults` / 分区表：指向合并分区表。
- `main/crid_ota.c`：网页 OTA 的写入目标从“自动轮换三个 ota 槽”改为**只在 rid_a / rid_b 之间轮换**，
  避免 OTA 把 SDR 固件覆盖掉。
- `main/crid-sim.c`：启动确认 OTA 状态 + 交还启动目标；启动本地 TFT 任务。
- `main/crid_lcd.c`（新增）：四页本地显示（状态 / 接收目标 / 关于 / **配置**）；配置页用按键改 12 项参数并保存到 NVS，保存后复用网页那套逻辑重建 AP / 应用收发开关，立即生效。
- 根目录 CMake 增加 `../common/panel` 与 `../common/dualboot` 组件。

## 已知边界

- 两套固件**从不同时运行**；RID 的 Wi-Fi/BLE 与 SDR 的射频/PHY 不会互相抢资源。
- NVS 共用一个分区，但命名空间不同（`sdr_ui` / `crid` / `cridlog`），设置互不覆盖。
- 从网页 OTA 升级 C-RID 时，上传的必须是 `rid/` 工程产出的 `remoteid_scanner.bin`；
  升级后重启会先回到开机菜单（这正是“每次开机选择”的预期行为）。
- SDR 原仓库自带的 `flash.sh` / `merge_full.py` 是单固件 2 MB 布局用的，**不要再使用**，
  请用 `tools/` 下的新脚本。
- 背光电流和长杜邦线导致 8 MHz SPI 不稳的问题与上游相同；需要时可把 `st7789.c` 的
  `LCD_SPI_HZ` 降到 4 MHz 重新编译。

## 来源与许可

本仓库是把两套既有固件合并成"一次烧录、开机二选一"的整合工程，**不是从零重写**，
上游代码的版权与许可是这份工作成立的前提：

| 目录 | 来源 | 许可证 |
|---|---|---|
| `sdr/` | [sd1471123/esp-sdr](https://github.com/sd1471123/esp-sdr) 的 `tft` 分支，commit `c7288b0` | GPL-3.0 |
| `sdr/components/esp-dsp` | [espressif/esp-dsp](https://github.com/espressif/esp-dsp)（vendored，随仓库一起分发，不需要联网取子模块） | Apache-2.0 |
| `rid/` | [sd1471123/esp32-ChinaRemoteID_Scan_Sim](https://github.com/sd1471123/esp32-ChinaRemoteID_Scan_Sim)，commit `5c50d79` | MIT（原始声明保留在 `rid/LICENSE`） |
| `launcher/`、`common/`、`tools/`、`partitions/`、`docs/` | 本工程新增（开机菜单、共用面板驱动、双启动切换、构建脚本） | GPL-3.0 |

因为 `sdr/` 是 GPL-3.0 代码的衍生作品，**整个仓库按 GPL-3.0 分发**（根目录 `LICENSE`）。
MIT 的 `rid/` 代码可以并入 GPL-3.0 的组合作品，方向是兼容的；反过来不成立，所以不能整体改成 MIT。

上游各文件的版权头一律保留未改。如果你把本项目再分发或二次开发，请同样遵守 GPL-3.0
（提供对应源码、保留声明、衍生作品同样开源）。
