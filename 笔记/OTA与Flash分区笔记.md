# OMI Glass 固件 —— OTA 与 Flash 分区笔记

> **本文件的作用**：完整讲清三件事 ——
> ① 本项目的 OTA 升级是怎么跑起来的；
> ② 这块 8 MB Flash 的分区表到底是怎么切的；
> ③ 为什么"分区之后会有一块区是完全空的"。
> 面向第一次接手本项目固件、想搞懂 OTA 与分区关系的人。
>
> **整理日期**：2026-10-08
> **依据**：`partitions_ota.csv`、`src/ota.cpp`、`src/ota.h`、`src/config.h`、
> `src/app.cpp` 的 `configure_ble()`，以及实际编译产物 `.pio/build/seeed_xiao_esp32s3/partitions.bin`。

## 目录

1. [先纠正一个说法：8 MB 并没有"分成两个区"](#1-先纠正一个说法8-mb-并没有分成两个区)
2. [真实分区表逐项解读](#2-真实分区表逐项解读)
3. [为什么要两个 app 分区 —— OTA 的 A/B 双槽](#3-为什么要两个-app-分区--ota-的-ab-双槽)
4. [那块"完全空的区"到底是什么](#4-那块完全空的区到底是什么)
5. [本项目的 OTA 完整逻辑](#5-本项目的-ota-完整逻辑)
6. [第一次烧录 vs OTA 升级：两条完全不同的路](#6-第一次烧录-vs-ota-升级两条完全不同的路)
7. [由此暴露的两个问题](#7-由此暴露的两个问题)
8. [常见坑与排错](#8-常见坑与排错)

---

## 1. 先纠正一个说法：8 MB 并没有"分成两个区"

这块 XIAO ESP32S3 Sense 的 Flash 是 **8 MB**（8,388,608 字节，即 `0x800000`）。

但打开 `partitions_ota.csv` 会发现，**分区表只覆盖了前 4 MB**，后面 4 MB 一个字节都没分配。
所以准确的描述是：

> **8 MB 里，只有 4 MB 被切成 5 个分区用于实际功能，另外 4 MB 完全闲置。**

"两个区"这个印象，多半来自这两处之一 —— 两处都真实存在，但含义完全不同：

| 你可能看到的"两个区" | 实际含义 | 详见 |
|---|---|---|
| `app0` / `app1` 两个**应用槽** | OTA 的 A/B 双槽设计，一个跑、一个当备胎 | [第 3 节](#3-为什么要两个-app-分区--ota-的-ab-双槽) |
| 前 4 MB 已用 / 后 4 MB 全空 | 分区表没写满，尾部 4 MB 是**未分配**的裸 Flash | [第 4.2 节](#42-后半块-4-mb分区表压根没定义) |

---

## 2. 真实分区表逐项解读

`partitions_ota.csv` 原文（这就是 PlatformIO 实际编译进设备的表）：

```csv
# Name,   Type, SubType, Offset,   Size,    Flags
nvs,      data, nvs,     0x9000,   0x5000,
otadata,  data, ota,     0xe000,   0x2000,
app0,     app,  ota_0,   0x10000,  0x1C0000,
app1,     app,  ota_1,   0x1D0000, 0x1C0000,
spiffs,   data, spiffs,  0x390000, 0x70000,
```

我在编译产物上反解出的实际值（与 CSV 一致）：

| 名称 | 类型 | 子类型 | 起始地址 | 大小 | 结束地址 | 作用 |
|---|---|---|---|---|---|---|
| *(bootloader)* | — | — | `0x001000` | ~28 KB | `0x008000` | 二级引导程序（**不在 CSV 里**，由工具链自动烧） |
| *(分区表)* | — | — | `0x008000` | 4 KB | `0x009000` | 分区表本体（**不在 CSV 里**） |
| `nvs` | data | nvs | `0x009000` | 20 KB | `0x00E000` | 非易失键值存储（WiFi 校准数据等） |
| `otadata` | data | ota | `0x00E000` | 8 KB | `0x010000` | **OTA 启动决策区**（下面细讲） |
| `app0` | app | ota_0 | `0x010000` | 1792 KB | `0x1D0000` | 应用槽 A ← **当前固件装在这里** |
| `app1` | app | ota_1 | `0x1D0000` | 1792 KB | `0x390000` | 应用槽 B ← OTA 备胎 |
| `spiffs` | data | spiffs | `0x390000` | 448 KB | `0x400000` | SPIFFS 文件系统（存 `speaker_template.bin` 声纹模板） |
| **（无）** | — | — | `0x400000` | **4 MB** | `0x800000` | **未分配，全空** ⬅ 见第 4.2 节 |

内存映射图（按比例）：

```
0x000000 ┌───────────────────────────┐
         │ bootloader (0x1000 起)     │
0x008000 ├───────────────────────────┤
         │ 分区表 (4 KB)              │
0x009000 ├───────────────────────────┤
         │ nvs            20 KB      │
0x00E000 ├───────────────────────────┤
         │ otadata         8 KB      │  ← OTA 的"方向盘"
0x010000 ├───────────────────────────┤
         │                           │
         │ app0  1792 KB (1.75 MB)   │  ← 现在跑的就是这份
         │ ota_0                     │
0x1D0000 ├───────────────────────────┤
         │                           │
         │ app1  1792 KB (1.75 MB)   │  ← 空的（串口烧录不会碰它）
         │ ota_1                     │
0x390000 ├───────────────────────────┤
         │ spiffs        448 KB      │
0x400000 ├───────────────────────────┤
         │                           │
         │   未分配  4 MB（全 0xFF）  │  ← 完全空的那块
         │                           │
0x800000 └───────────────────────────┘
```

> **注意**：`platformio.ini` 里写了 `board_build.partitions = partitions_ota.csv`，
> 这一行**覆盖**了板卡自带的 `default_8MB.csv`。所以最终生效的是上面这张（只用到 4 MB 的）表，
> 而不是 8 MB 的标准表。这一点是理解"为什么有 4 MB 空着"的关键。

---

## 3. 为什么要两个 app 分区 —— OTA 的 A/B 双槽

### 3.1 核心约束：OTA 时不能覆盖"正在运行"的固件

ESP32 的代码是**直接在 Flash 上执行**的（XIP，eXecute In Place），不像 PC 那样先把程序加载到内存。
所以当固件正在运行时，那块 Flash 区域是**被 CPU 持续读取的**——你没法一边跑它一边把它自己擦掉重写。

结论很硬：**要升级，就必须写到另一块地方去。**

于是分区表里放两个同样大的 app 槽：

- **`app0`（ota_0）** —— 当前正在运行的那份。
- **`app1`（ota_1）** —— 接收新固件的地方。

这就是所谓的 **A/B 双槽（A/B slots）** 布局。

### 3.2 切换由谁决定？`otadata`

写完新固件后，怎么让设备下次从新槽启动？靠 `otadata` 这个 8 KB 分区。

它由两个 4 KB 扇区组成（互为备份，各存一份相同的记录），每条记录里有：

- **`ota_seq`** —— 序列号，谁的数字大就启动谁；
- **`ota_state`** —— 状态（新固件是否已验证）；
- **CRC 校验** —— 防止记录损坏。

启动时序：

```
上电
 └─ bootloader 读 otadata
     ├─ 记录损坏 / 全 0xFF → 走默认，启动 app0
     └─ 有效记录 → 比较 ota_0 与 ota_1 的 ota_seq
                   → 启动 seq 大的那个槽
```

所以 **"切换固件"本质上就是改写 `otadata` 里那几十个字节**，而不是搬动固件。这也是 OTA 能这么快完成切换的原因。

### 3.3 一个必须澄清的点：双槽 ≠ 自动回滚

很多人以为"有 A/B 双槽，新固件崩了会自动退回旧的"。**本项目不成立。**

分两种情况：

| 失败时机 | 会怎样 | 能否自愈 |
|---|---|---|
| 下载中断 / 长度对不上 / `Update.write` 报错 | 新固件根本没写完，`otadata` 没被改，**继续跑旧固件** | ✅ 天然保住 |
| 新固件**写完且校验通过**，但启动后崩溃 / 卡死 | `otadata` 已经指向新槽，**会一直卡在坏固件上** | ❌ 只能串口重刷 |

要实现第二种的自动回滚，需要 **bootloader 开启 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`**，
并且新固件在启动成功后主动调用 `esp_ota_mark_app_valid_cancel_rollback()` 来"确认验收"；
只有这样，未确认的固件才会在下次重启时被 bootloader 判定失败并切回旧槽。

**本项目没有实现这套确认机制**（`src/ota.cpp` 里搜不到 `mark_app_valid`），
所以"新固件能跑起来但会崩"是最危险的情况 —— 直接变砖，只能靠 USB 线重刷。

---

## 4. 那块"完全空的区"到底是什么

有**两个**"空"要分开讲，它们经常被混为一谈。

### 4.1 `app1` —— 空的，因为从没用过 OTA

`app1` 是 OTA 备胎槽。而**通过 USB 串口烧录时，esptool 只写 `app0`，完全不碰 `app1`**。

所以只要你这块板子从来没成功做过一次 OTA，`app1` 里就是一片 `0xFF`（Flash 擦除后的原始状态）。
用 `esptool read_flash` 读 `0x1D0000` 附近，会看到全是 `FF`。

**这是完全正常的**，不是分区表配错了。它空着就是在等一个 OTA。

### 4.2 后半块 4 MB —— 分区表压根没定义

再看 `partitions_ota.csv`：最后一个分区 `spiffs` 在 `0x390000`，大小 `0x70000`，
结束于 **`0x400000`** —— 正好 4 MB 边界。

而 Flash 是 8 MB。**`0x400000` 到 `0x800000` 这 4 MB，没有任何分区条目指向它。**

这不是"某个分区被留空了"，而是**这一段地址根本不在 ESP-IDF 的管理范围内**：

- 分区表是"白名单"机制。bootloader 和 `Update` 库只会去分区表里登记过的地址读写；
- 没登记的区域**谁也访问不到**——不能用 `Update` 写进去，也没法挂成文件系统；
- 它既不是坏块，也不是保留区，就是单纯的**没被分配**。

**为什么会出现这种情况？** 这是历史遗留：这张表很可能是从早期 4 MB Flash 的配置里沿用下来的
（4 MB 芯片刚好填满），后来硬件换成 8 MB，但分区表没有跟着扩。

**代价很直接**：`app0` 只有 1.75 MB，而这恰恰是本项目 Flash 占用率高达 **96.2%** 的原因
（1,765,881 B / 1,835,008 B）—— 明明旁边躺着 4 MB 没人用。详见第 7 节。

---

## 5. 本项目的 OTA 完整逻辑

一句话概括：**BLE 当遥控器 → WiFi 当数据通道 → `Update` 库当刷写器。**

代码集中在 `src/ota.cpp`（约 400 行），`app.cpp` 只负责注册 GATT 特征和调用。

### 5.1 三个 BLE 特征（服务 `19B10010`）

| 特征 | UUID | 属性 | 作用 |
|---|---|---|---|
| OTA Control | `19B10011` | `READ \| WRITE` | 手机写命令；读当前状态 |
| OTA Data | `19B10012` | `READ \| NOTIFY` | 设备回报进度（带 CCCD `BLE2902`） |

> ⚠️ 与命令特征 `19B10003` 同样的隐患：这里只声明了 `PROPERTY_WRITE`（有响应写），
> **没有 `PROPERTY_WRITE_NR`**。客户端若用"无响应写"发 OTA 命令会被 GATT 层直接拒收。

### 5.2 命令集（手机写 `19B10011`）

定义在 `src/config.h`：

| 命令 | 值 | 负载格式 |
|---|---|---|
| `OTA_CMD_SET_WIFI` | `0x01` | `[cmd, ssid_len, ssid..., pass_len, pass...]` |
| `OTA_CMD_START_OTA` | `0x02` | 无（需先 SET_WIFI + SET_URL） |
| `OTA_CMD_CANCEL_OTA` | `0x03` | 无 |
| `OTA_CMD_GET_STATUS` | `0x04` | 无 |
| `OTA_CMD_SET_URL` | `0x05` | `[cmd, url_len_hi, url_len_lo, url...]`（长度 2 字节大端） |

### 5.3 主流程

```
① 手机 --BLE 写 19B10011--> SET_WIFI   : 把 SSID/密码存进 wifiSSID / wifiPassword
② 手机 --BLE 写 19B10011--> SET_URL    : 把固件下载地址存进 firmwareURL
③ 手机 --BLE 写 19B10011--> START_OTA
     └─ 校验：WiFi 和 URL 都已设置？没有就报 ERROR 返回
     └─ xTaskCreate(ota_task, 栈 8192, 优先级 5)   ← 独立任务，不阻塞主循环
④ ota_task → connect_wifi()                   : 连 WiFi（最长 15 s，可中途取消）
⑤ ota_task → download_and_install_firmware()
     ├─ HTTP(S) GET 那个 URL（跟随重定向，超时 30 s）
     ├─ Update.begin(contentLength)           ← 自动选中"当前没在跑"的那个 app 槽
     ├─ 循环：读 1024 B → Update.write()      ← 边下边写进 app1，每 5% notify 一次进度
     ├─ Update.end(true)                      ← 校验 + 把 otadata 指向新槽
⑥ ota_notify_status(REBOOTING) → 停 WiFi → ESP.restart()
⑦ 重启后 bootloader 读 otadata → 启动新固件
```

**关键点**：手机**只提供"网络钥匙 + 下载地址"**，剩下的连网、下载、刷写全由设备自己完成。
所以手机端不需要实现任何固件传输协议，只要发三条命令。

### 5.4 状态机（经 `19B10012` notify，2 字节 `[status, progress]`）

| 状态值 | 含义 |
|---|---|
| `0x00` | `IDLE` 空闲 |
| `0x10` / `0x11` / `0x12` | WiFi 连接中 / 已连接 / 失败 |
| `0x20` / `0x21` / `0x22` | 下载中 / 下载完成 / 下载失败 |
| `0x30` / `0x31` / `0x32` | 安装中 / 安装完成 / 安装失败 |
| `0x40` | 正在重启 |
| `0xFF` | 出错 |

### 5.5 实现细节与设计取舍

- **独立任务**：`START_OTA` 会 `xTaskCreate` 一个 `ota_task`，所以 `ota_loop()` 是空的，
  OTA 全程不阻塞 `loop_app()`（音频采集/编码/BLE 照常跑）。
- **可取消**：`otaCancelled` 是全局标志，`ota_cancel()` 只把它置 `true`；
  任务在连网循环、下载循环里各自检查它，**不强行 kill 任务**（避免半途释放资源）。
- **粒度**：下载缓冲 1024 B，进度按 5% 上报一次（`progress % 5 == 0`）。
- **WiFi 用完即关**：无论成功失败都会 `WiFi.disconnect(true)` / `WiFi.mode(WIFI_OFF)`，
  避免 WiFi 常驻增加功耗。
- **HTTPS 跳过证书校验**：`secureClient->setInsecure()`（源码里也标了 TODO）。
  见第 7 节。

---

## 6. 第一次烧录 vs OTA 升级：两条完全不同的路

这两个概念极易混淆，但对分区的影响完全不同：

| | 首次/串口烧录（PlatformIO） | OTA 升级 |
|---|---|---|
| 通道 | USB 线 | BLE 下命令 + WiFi 下载 |
| 谁在写 Flash | PC 上的 esptool | 设备自己（`Update` 库） |
| 写入哪个 **app** 槽 | **永远 `app0`（`0x10000`）** | **总是"当前没在跑"的那个**（`app1`） |
| 会碰 `app1` 吗 | ❌ 不会 | ✅ 会 |
| 会改 `otadata` 吗 | 通常重置为从 `app0` 启动 | ✅ 会指向新槽 |
| 固件来源 | `.pio/build/.../firmware.bin` | `v2.3.2/omiglass_firmware_v2.3.2.bin` 之类，需可被 HTTP 访问 |

**由此得出两条实用结论**：

1. 如果板子"变砖"了（`otadata` 指向一个坏固件），**插 USB 重新烧录就能救回来** ——
   因为 esptool 会把 `app0` 重写并重置启动指向。OTA 失败基本不会造成不可恢复的损坏。
2. OTA 用的 `.bin` **必须是应用镜像（`firmware.bin`），不是合并镜像**。
   合并镜像（`firmware-merged.bin` / `.uf2`）包含 bootloader 和分区表，`Update.write()` 认不了。
   `releases/omi_glass_firmware.uf2` 那种就是给 USB/UF2 刷机用的，**不能用于 OTA**。

---

## 7. 由此暴露的两个问题

### 7.1 🔴 Flash 96.2% 用满，但旁边有 4 MB 闲置

`app0` 只有 **1.75 MB**（1,835,008 B），当前固件已经占掉 **1,765,881 B，即 96.2%**。
而 `0x400000–0x800000` 的 **4 MB 完全没人用**。

这不是"固件太大"，是**分区表没写满**。修法很简单：重写 `partitions_ota.csv`，把两个 app 槽放大，例如：

```csv
# Name,   Type, SubType, Offset,   Size,    Flags
nvs,      data, nvs,     0x9000,   0x5000,
otadata,  data, ota,     0xe000,   0x2000,
app0,     app,  ota_0,   0x10000,  0x300000,   # 3 MB
app1,     app,  ota_1,   0x310000, 0x300000,   # 3 MB
spiffs,   data, spiffs,  0x610000, 0x1F0000,   # 1.94 MB
```

这样 app 槽从 1.75 MB 翻到 3 MB，Flash 占用率立刻从 96% 降到 56% 左右，
后续加功能不再捉襟见肘。

> **注意**：改分区表**必须整片重刷**（`pio run -t erase` + `upload`），不能只做 OTA ——
> 因为分区布局变了，旧固件里的 `otadata` 记录会失效。改之前先确认能连上 USB。
> 另外 `spiffs` 里的声纹模板会随擦除丢失，需要重新录入。

### 7.2 ⚠️ OTA 下载跳过 HTTPS 证书校验

`src/ota.cpp:269`：

```cpp
secureClient->setInsecure();   // 跳过证书校验（源码注释里标了 TODO）
```

后果：中间人可以把固件 URL 劫持到自己的服务器，让设备刷进任意固件。
**这是整个 OTA 链路最严重的安全口子**，产品化之前必须改成校验证书
（用 `setCACert()` 固定 CA，或用 `WiFiClientSecure` 的证书指纹校验）。

### 7.3 ⚠️ OTA 控制特征同样缺少 `PROPERTY_WRITE_NR`

见 5.1 节的说明，与 `19B10003` 是同一个问题。

---

## 8. 常见坑与排错

| 现象 | 原因 | 处理 |
|---|---|---|
| OTA 一直停在 `0x12`（WiFi 失败） | SSID/密码错，或路由器是 5 GHz（ESP32 只支持 2.4 GHz） | 换 2.4 GHz 频段重试 |
| 停在 `0x22`（下载失败） | URL 不可达 / 是合并镜像 / 服务器要鉴权 | 先用浏览器确认该 URL 能下到 `firmware.bin` |
| `Update.begin()` 失败（报 INSTALL_FAILED） | 固件比 app 槽还大 —— 本项目 app 槽只有 1.75 MB，**余量仅 3.8%** | 精简固件，或按 7.1 扩分区 |
| OTA 成功但设备起不来 | 新固件能写进去却启动崩溃，**且没有自动回滚**（见 3.3） | 只能插 USB 重刷 |
| OTA 后行为没变 | 手机连的还是旧句柄，或没真正重启 | 看串口是否打印 `OTA: Rebooting now!` |
| 想确认某块地址是不是空的 | 读 Flash 比对 | `esptool.py read_flash 0x400000 0x1000 dump.bin`，全 `FF` 即未使用 |

**验证分区表的两种方法**：

```bash
# 方法 1：反解编译产物（推荐，不用连板子）
#    .pio/build/seeed_xiao_esp32s3/partitions.bin 里每条记录 32 字节，
#    以 AA 50 开头，依次是 type / subtype / offset(4B LE) / size(4B LE) / name(16B)

# 方法 2：问设备自己（需连上板子）
platformio run -t upload   # 先确保能烧录
# 然后读 otadata / 分区表打印
```

---

## 附：一句话总结

> **8 MB 的 Flash 只切了 4 MB 出来用**，切出的 `app0`/`app1` 是 OTA 的 A/B 双槽；
> `app1` 目前全空是因为**从没做过 OTA**，而尾部那 4 MB 全空是因为**分区表压根没写到那里**。
> OTA 的机制是"BLE 下命令 → WiFi 下载 → 写进另一个槽 → 改 `otadata` → 重启切换"，
> 它只能救"下载失败"，救不了"新固件启动崩溃"。
