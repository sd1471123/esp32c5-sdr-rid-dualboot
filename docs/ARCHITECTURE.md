# 合并架构说明

## 三个镜像，而不是一个“二合一”固件

SDR 与 RID 各自保持独立 ELF：SDR 直接操作 C5 的 PHY/RF、带自定义 `sram_guard.ld`
和 `--wrap=phy_chip_set_chan_ana`；RID 用 Wi-Fi softAP + BLE + HTTPS + mbedTLS 并依赖
OTA 回滚。把两者链进同一个 app 会让链接脚本、PHY hook 和回滚逻辑互相踩，所以采用
**三个 app 分区 + 一个 launcher** 的方案：功能完全隔离，只有一个在运行。

## 启动时序

```
上电 / 复位
   │  bootloader 读 otadata
   ├─ otadata 为空 ───────────► factory（launcher 菜单）
   └─ otadata 指向 ota_x ─────► 上一次选的固件（正常不会持久，见下）

launcher：画菜单 → 按 ←/→ → esp_ota_set_boot_partition(sdr | rid_a) → esp_restart()

被选中的 app：
   1) dualboot_confirm_running()   把 OTA 的 PENDING_VERIFY 确认掉
   2) dualboot_release_to_launcher()  esp_ota_set_boot_partition(factory)
      → ESP-IDF 官方实现会擦空 otadata
   3) 正常跑 SDR 或 RID 的业务

下一次上电：otadata 又是空的 → 再次进入 launcher 菜单
```

关键点已经对着 ESP-IDF v6.x 的 `esp_ota_ops.c` 核实：
`esp_ota_set_boot_partition_internal()` 对 factory 分区执行的是
`esp_partition_erase_range(otadata, 0, size)`；而 bootloader 在 otadata 无效时启动 factory。

## 为什么 RID 的 OTA 必须改

原工程用 `esp_ota_get_next_update_partition(NULL)`：在只有两个 ota 槽时轮换没问题；
合并后有三个 ota 槽（`sdr` / `rid_a` / `rid_b`），从 `rid_b` 升级时它会挑到 `sdr`，
一升级就把频谱仪固件刷没了。`crid_ota.c` 改为按 label 查找，只会在 `rid_a` 与 `rid_b`
之间选“当前没在跑”的那个。

## 回滚的取舍

共享 bootloader 由 launcher 工程产出，打开了 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`。
三个 app 在 `app_main` 最开头都会调 `dualboot_confirm_running()`，所以被新 OTA 写进去的
RID 镜像只要能跑到 `app_main` 就会被确认，不会在下一次启动时被回滚掉。
代价是“启动后立刻把启动目标还给 launcher”，因此 RID 原本那种“所有服务都起来才确认”的
严格语义被放宽了——这是“每次开机都选菜单”必然要付出的取舍。

## 共用驱动

`common/panel/` 里的 `st7789.c`、`buttons.c`、`buzzer.c`、两份字库都是 sdr tft 分支的
原文件，未做逻辑修改；`panel_draw.c` 只是把画字/画框的辅助函数抽出来给 launcher 和 RID 共用。
因此屏幕初始化序列、MADCTL、5 ms 长按/连发/组合键时序都和原 SDR 固件一致。
