# VDIClient 项目资料汇总

> 本文档汇总了 `c:\Users\Administrator\Desktop\VDIClient` 下三个项目的调研资料，说明各项目的定位、技术栈、功能与三者之间的协作关系。

## 一、项目总览与关系

```
vdi-client-windows-main（VDI 管理客户端 / 前台 UI）
        │  通过 QProcess 启动
        ▼
qfreerdp-windows（RDP 渲染客户端，产出 qf-client.exe）
        │  链接/依赖
        ▼
freerdp-3.28.0（RDP 协议引擎 / 底层库）
```

| 项目 | 定位 | 技术栈 | 主要产物 |
|---|---|---|---|
| freerdp-3.28.0 | RDP 协议开源实现（底层库） | C / CMake / Ninja / MSVC | freerdp3.dll、winpr3.dll、freerdp-client3.dll、wfreerdp.exe |
| qfreerdp-windows | 基于 Qt Quick 的 RDP 远程桌面客户端 | C++20 / Qt 6.11.1 (QML) / FreeRDP 3 / spdlog / libusb / QRhi+D3D11 | qf-client.exe |
| vdi-client-windows-main | 基于 Qt Widgets 的 VDI 管理客户端 | C++17 / Qt 6 (Widgets/Network) / CMake | VDIClient.exe |

**协作流程**：用户在 `vdi-client-windows-main`（VDIClient.exe）中输入 VDI 服务器地址、用户名、密码登录 → 客户端通过 HTTPS API 获取虚拟机列表并管理虚拟机（开关机/重启/还原）→ 点击"连接"后从服务器获取 RDP 文件与连接命令 → 以子进程方式启动 `qf-client.exe` 传递 `.rdp` 文件与参数 → `qf-client` 内部基于 FreeRDP 建立到虚拟机的 RDP 连接并全屏渲染桌面。

---

## 二、freerdp-3.28.0（RDP 协议库）

### 2.1 项目性质
- **FreeRDP**：Remote Desktop Protocol（RDP）的自由开源实现，Apache 许可证。
- 版本 **3.28.0**（见 `.source_version`）。
- 官方站点：https://www.freerdp.com/ ，源码：https://github.com/FreeRDP/FreeRDP

### 2.2 目录结构（关键部分）
| 目录 | 说明 |
|---|---|
| `libfreerdp/` | 核心协议栈：`core/`（RDP/GCG/MCS/TPKT/NLA/安全/编解码器协商）、`codec/`（RFX/NS codec、H264、AV1、ZGfx 等）、`cache/`、`gdi/`（GDI 图形绘制与图形缓存）、`crypto/`、`utils/` |
| `winpr/` | Windows 便携运行时库（线程、I/O、剪贴板 wClipboard 等跨平台抽象） |
| `channels/` | RDP 虚拟通道：rdpdr、printer、客户端通道 addin 等 |
| `client/` | 各平台客户端示例：X11、Wayland、macOS、Windows、SDL、iOS |
| `server/` | 服务端：shadow（屏幕共享）、proxy、Windows 服务端 |
| `rdtk/` | 简易 UI 工具包 |
| `include/freerdp/` | 公共 API 头文件 |
| `build/` | CMake/Ninja 编译产物（`.ninja_log`、`CMakeCache.txt`） |
| `install/` | 安装产物：`bin/`（freerdp3.dll、winpr3.dll、wfreerdp.exe 等）、`lib/`（freerdp3.lib、winpr3.lib） |

### 2.3 构建方式（build-freerdp.ps1）
- 工具链：**Visual Studio 2022 + MSVC（x64）+ Ninja + CMake + vcpkg**（vcpkg 位于 `C:\Users\Administrator\Desktop\workspace\vcpkg`）。
- 输出安装目录：`freerdp-3.28.0\install`，作为下游的 `CMAKE_PREFIX_PATH`。
- 启用的关键选项：
  - `WITH_WASAPI`、`WITH_FFMPEG`、`WITH_SWSCALE`、`WITH_OPENH264`、`WITH_OPENSSL`、`WITH_SSE2`、`WITH_DSP_FFMPEG`
  - 通道：`CHANNEL_URBDRC=ON`（USB 重定向）、`CHANNEL_RDPECAM_CLIENT=ON`（摄像头重定向）、`CHANNEL_GEOMETRY=ON`
  - `WITH_SERVER=OFF`、`WITH_PROXY=OFF`、`WITH_CLIENT_SDL=OFF`（仅编译客户端所需部分）

### 2.4 在项目中的作用
- 为 qfreerdp 提供完整 RDP 客户端能力：连接建立（NLA/证书）、图形编码（GFX/H264/AVC444）、虚拟通道（cliprdr、disp、rdpsnd、audin、urbdrc、rdpecam）、GDI 绘制、指针/光标支持等。
- qfreerdp 的 `CMakeLists.txt` 通过 `find_package(FreeRDP 3 / WinPR 3 / FreeRDP-Client 3)` 引用其 `install` 目录，链接 `freerdp`、`freerdp-client`、`winpr` 三个库。

### 2.5 本地补丁（相对上游 3.28.0）

**当前 `freerdp-3.28.0/` 已非上游原样，共有四处本地补丁：`libfreerdp/common/settings.c` 的功能性修复（2.5.1）、`channels/rdpgfx/client/rdpgfx_codec.c` 的日志治理（2.5.2）、`channels/rdpecam/client/camera_device_main.c` 的摄像头候选格式表（2.5.3）、`libfreerdp/codec/video.c` 的 H.264 码控（2.5.4）；若重新解压源码包，四处补丁都会丢失，需重新应用并重新编译。**

**产物归属**：2.5.1/2.5.4 落在 `libfreerdp`（产物 **`freerdp3.dll`**），2.5.2/2.5.3 落在 `channels/`（OBJECT 库，对象文件链入 **`freerdp-client3.dll`**）。两处 DLL 都要同步到 `freerdp-3.28.0/install/bin/` 与 `qfreerdp-windows/build/`，否则打包带的是旧库。

#### 2.5.1 `settings.c`：`freerdp_addin_argv_new()` 的 NULL 参数（功能性修复）

- **位置**：`libfreerdp/common/settings.c` 的 `freerdp_addin_argv_new()`。
- **问题**：该函数对参数数组逐项执行 `_strdup()`，任一元素为 `NULL` 即 `goto fail` 并返回 `NULL`。而 `RDPDR_DRIVE` 的 `automount` 语义恰恰是"第 3 个参数为 `nullptr`"——同文件 `freerdp_device_new()` 中 `if (count > 2) device->u.drive.automount = (args[2] == nullptr);`。WinPR 的 `_strdup(NULL)` 返回 `NULL`，于是整张参数表构造失败。
- **后果**：`rdpdr` 通道的热插拔装载路径 `first_hotplug()` → `rdpdr_load_drive(rdpdr, name, path, TRUE)` 中，`freerdp_device_new()` 返回 `NULL`，`rdpdr_load_drive()` 走 `if (!drive.device) goto fail;` **静默返回 `FALSE`（无任何日志）**，磁盘重定向整体失效。仅影响通配路径（`/drives`、`drivestoredirect:*`、`+drives`）；显式 `/drive:name,path` 因 `automount=FALSE`、第 3 个参数非空而不受影响——这也是"mstsc 能重定向、客户端 `+drives` 不行"的原因。
- **补丁**：在参数循环中跳过 `NULL` 元素（`calloc` 已把该槽位置零，语义不变），仅增加两行：
  - `if (!argv[x]) continue;`
- **验证**：打补丁后客户端日志出现 `Loading device service drive [C] (static)`、`registered [    drive] device #1:     C`、`send [PAKID_CORE_DEVICELIST_ANNOUNCE] [2]`，服务端回 `PAKID_CORE_DEVICE_REPLY ... status=0x00000000`。
- **注意**：补丁落在 `libfreerdp`，编译产物是 **`freerdp3.dll`**（不是 `freerdp-client3.dll`），部署与打包时必须一并更新。

#### 2.5.2 `rdpgfx_codec.c`：关闭每帧 codec 日志（日志治理）

- **位置**：`channels/rdpgfx/client/rdpgfx_codec.c` 的 `rdpgfx_decode()`（按 `cmd->codecId` 分支的四条 INFO）。
- **问题**：AV1 / AVC420 / AVC444 / OTHER 四条 `WLog_Print(..., WLOG_INFO, "rdpgfx_decode: codec=…")` 位于**每帧解码**路径上——服务端每发一帧就打印一次，AVC444 场景下 30~60 行/秒。
- **影响**：日志走 WinPR 默认 Console appender，落地是 `fprintf(stdout, …)`，且该输出**在持有 appender 临界区期间完成**（`WLog_Write()`），而终端渲染是这条链上最贵的一环。后果是解码线程每帧多一次格式化 + I/O、延迟抖动，并与其他线程（输入/rdpdr/urbdrc）的日志互相排队；若终端被"标记/选择"阻塞、或输出被重定向到慢速管道/磁盘，解码线程会**持锁阻塞**，表现为画面冻结、键鼠无响应。仅影响性能与体验，**不涉及协议或画面正确性**。
- **补丁**：用块注释包住这四条 INFO，**保留同名 `WLog_ERROR`**（如 `rdpgfx_decode_AVC444 failed with error …`）。`case` 标签、解码调用与 `logSurfaceCommand()` 均未改动。
- **验证**：重编后二进制扫描确认不再含 `rdpgfx_decode: codec=`，仍含 `rdpgfx_decode_AVC444 failed`。
- **注意**：补丁落在 `channels/rdpgfx/client`，该目标为 **OBJECT 库**（`channels/CMakeLists.txt` 的 `add_library(... OBJECT ...)`），对象文件链入 **`freerdp-client3.dll`**（**不是 `freerdp3.dll`**）。增量重编只需 `cmake --build build --target freerdp-client`（仅重编该 .c 并重链 DLL），但要同步 `freerdp-3.28.0/install/bin/` 与 `qfreerdp-windows/build/` 两处副本，否则打包带的是旧库。
- **不改源码的备选**：命令行加 `/log-filters:com.freerdp.channels.rdpgfx.client:ERROR`，代价是该 tag 下**所有** INFO 一并被压掉。

#### 2.5.3 `camera_device_main.c`：摄像头候选格式表（H264 优先 + 补 NV12/I420）

- **位置**：`channels/rdpecam/client/camera_device_main.c` 的 `getSupportedFormats()`。
- **机制**：该函数生成"摄像头侧 → 网络侧"的格式对候选表。双层循环**外层 `i` 是网络侧输出格式、内层 `j` 是摄像头侧输入格式**，因此表内**首项即最终选中的格式对**；HAL 再用候选的输入格式去匹配摄像头原生 MF 子类型，命中第一个即选定，并把上报列表中所有条目的 `Format` 统一改写为该候选的输出格式。
- **问题 1（带宽，20~30 Mbps）**：原候选表为 `{MJPG, H264, YUY2}`，展开后首项是 `(MJPG→MJPG)`。`src == dst` 时 `freerdp_video_sample_convert()` 只做**原样拷贝、完全不编码**，于是上行就是摄像头自身的 MJPEG 码流——外接 1080p 摄像头实测占用 20~30 Mbps，且客户端 CPU 零开销（`video_get_h264_bitrate()` 算出的 2700 kbps 只是打印出来，从未生效）。
- **问题 2（兼容性，NV12 机型不可用）**：候选表只含 3 种格式，而 `ecamToVideoFormat()` / `ecamToMfSubtype()` 其实**早已实现** NV12、I420、RGB24、RGB32 的映射与转换；HAL 侧又设了 `MF_READWRITE_DISABLE_CONVERTERS=TRUE`（禁止 MF 自动插入转换器），所以**原生只出 NV12 的机型（部分笔记本内建、Surface 系、红外摄像头）会直接被拒**，报"不支持任何兼容格式"。
- **补丁**：
  1. `baseAvailable` 改为 `{H264, MJPG, YUY2, NV12, I420}`，候选顺序变为 `(H264,H264) > (MJPG,H264) > (YUY2,H264) > (NV12,H264) > (I420,H264) > …`，即"**原生 H264 直通 > 软件转码为 H264 > 原样直通**"。
  2. **新增 `isNetworkFormat()` 白名单，跳过以 NV12/I420 为输出（dst）的组合**。这一步必须做：外层 `i` 就是上行输出格式，NV12/I420 是**未压缩原始 YUV**（1080p NV12 ≈ 3 MB/帧 ≈ 750 Mbps），一旦被选为输出会把整帧裸数据推给服务端，比改造前更糟。白名单维持原有三种输出格式不变。
- **行为影响**：对已支持的摄像头**完全等价**（先命中 `(MJPG,H264)` 或 `(YUY2,H264)`，与改造前一致）；只有纯 NV12/I420 设备从"直接报错"变为"走 NV12→H264 转码"。
- **验证**：日志 `[ecam_dev_print_media_type]: Format:` 由 `2`(MJPEG) 变为 `1`(H264)；实测带宽 20~30 Mbps → **2.71 Mbps**（详见 3.8）。
- **注意**：补丁落在 `channels/rdpecam/client`，该目标为 **OBJECT 库**，对象文件链入 **`freerdp-client3.dll`**；增量重编 `cmake --build build --target freerdp-client`。

#### 2.5.4 `video.c`：H.264 码控 CQP → VBR

- **位置**：`libfreerdp/codec/video.c` 的 `freerdp_video_context_reconfigure()`。
- **问题**：该函数在设置目标码率（`H264_CONTEXT_OPTION_BITRATE`）之后，把码控模式写死为 `H264_RATECONTROL_CQP` 并固定 `QP = 26`。两个编码后端都会因此**忽略码率**——OpenH264 走 `RC_OFF_MODE`（码率字段不参与）、libavcodec 走 `"qp"` 选项。结果是码率只由 QP 决定、随画面复杂度自由浮动，1080p 动态画面可冲到 20~30 Mbps，**码率完全不可控**。
- **补丁**：改为 `H264_RATECONTROL_VBR`，并删除已失效的 `H264_CONTEXT_OPTION_QP` 设置（VBR 下两个后端都不读它）。总改动 2 行。
- **后端行为**：libavcodec → `bit_rate`；OpenH264 → `RC_BITRATE_MODE` + `iTargetBitrate` + `bEnableFrameSkip`（超码率丢帧）。两条路径都已核对。
- **影响面**：该函数在源码树内的**唯一调用者**是 rdpecam 的 `ecam_encoder_context_init()`，不影响 rdpgfx 的屏幕编码。
- **注意**：补丁落在 `libfreerdp`，产物是 **`freerdp3.dll`**（不是 `freerdp-client3.dll`）；增量重编 `cmake --build build --target freerdp`。

---

## 三、qfreerdp-windows（RDP 渲染客户端，qf-client.exe）

### 3.1 项目性质
- 基于 **Qt 6（QML/Quick）** 的 RDP 客户端，是 VDI 方案中真正建立 RDP 连接与渲染桌面的内核。
- 语言标准：**C++20**（CMake 中 `CMAKE_CXX_STANDARD 20`）。
- 输出：`qf-client.exe`（全屏远程桌面）。

### 3.2 依赖
- Qt **6.11.1**（msvc2022_64），模块：Core、Gui（+GuiPrivate 提供 QRhi 头文件）、Qml、Quick。
- 自编译 **FreeRDP 3.28.0**（路径 `../freerdp-3.28.0/install`）。
- **spdlog**（+fmt）日志库；**libusb-1.0**（USB 设备枚举）；**WinMM**（麦克风设备检测）、**setupapi**。
- 编译需 `rc.exe`（Windows SDK）。

### 3.3 源码构成（src/）
| 文件 | 职责 |
|---|---|
| `mini-qf-client.cc` | 核心连接引擎：`PreConnect`/`PostConnect`/`LoadChannels` 回调、独立 RDP 线程的事件循环、连接重试（3 次）、断线重连状态机、剪贴板（cliprdr）回调实现、GFX/disp 动态通道管理、命令行解析、OpenSSL 环境变量设置、分辨率计算；磁盘重定向模式判定（`updateDriveRedirectState()`）；USB 重定向参数构造（`id:` / `addr:` 单参数 `#` 分隔，见 3.7）；`WM_DEVICECHANGE` 去抖监听（插拔后自动刷新 USB 列表，见 3.7） |
| `rdp-view-item.h` | `RdpViewItem`（QQuickItem）：**D3D11 原生纹理渲染管线**（`RdpFrameTexture` + QRhi）、鼠标/滚轮/键盘事件转发、光标显示/隐藏、剪贴板数据转换（文本/图片/文件） |
| `usb-manager.cc/.h` | 基于 libusb 的 USB 设备枚举、选择状态管理与重定向触发重连；设备过滤规则（仅"全部接口都是 HID/Audio"才隐藏，见 3.7）；"USB 设备 ↔ 盘符"映射（SetupAPI 卷/磁盘类设备枚举 + `CM_Get_Parent` 回溯父设备链取 VID/PID）与"已磁盘重定向"置灰状态；复合设备（存储接口 + 其它接口）判定 |
| `clipboard-entry.h` | 远程文件剪贴板：解析 `FileGroupDescriptorW`，通过 `FILECONTENTS_SIZE/RANGE` 分块下载远程文件到本地临时目录 |
| `qf_channel_client_handler.c` | 剪贴板通道 addin 的 handler（`cliprdr_VirtualChannelEntryEx`） |
| `ref-tmp/` | 从 FreeRDP 源码拷贝的 cliprdr 相关源文件（编译时用 `/FORCE:MULTIPLE` 容忍重复符号） |
| `main.qml` | 全屏 UI：RDP 渲染区域 + 顶部悬浮工具栏（固定/发送 Ctrl+Alt+Del/USB 设备选择/最小化/全屏切换/关闭）+ USB 设备选择弹窗 |
| `qf_util.h` | `to_freerdp_key_code()`（Qt 键码→RDP 扫描码映射）、`client_t` 共享上下文、剪贴板自定义格式（PNG/FileGroupDescriptorW） |
| `qf_log.h` | spdlog 封装的格式化日志（`qf::log::info/warn/error`），可用 `SPDLOG_LEVEL` 环境变量调整级别 |

### 3.4 核心功能
- **全屏 RDP 渲染**：FreeRDP 线程解码帧写入 GDI buffer → 脏矩形拷贝到 CPU staging buffer → GUI 线程触发 `update()` → `beforeRendering` 中通过 `ID3D11DeviceContext::UpdateSubresource` 零拷贝上传到 D3D11 纹理 → QSGSimpleTextureNode 渲染。
- **动态分辨率**：基于窗口实际物理尺寸（考虑 HiDPI 的 devicePixelRatio，按 4 对齐）通过 disp 通道 `SendMonitorLayout` 下发，并跟踪 GFX_RESET 结果；连接前也设置 monitor layout 与 DesktopWidth/Height。
- **剪贴板双向**：文本（CF_UNICODETEXT）、图片（CF_DIB/DIBV5/PNG）、文件（FileGroupDescriptorW 双向传输）。
- **USB 重定向**：工具栏 USB 弹窗选择设备（libusb 枚举 + `WM_DEVICECHANGE` 插拔自动刷新），确认后通过 `urbdrc` 动态通道重定向并自动重连；默认发 `id:VID:PID`，检测到同型号多支时整次连接改为 `addr:bus:addr`（详见 3.7）。已被磁盘重定向接管的存储类设备在列表中**置灰并标注盘符**、不可勾选；复合设备（存储接口之外还有自定义接口）**不整体置灰**，仅标注提示（详见 3.6.2、3.7）。
- **摄像头/麦克风重定向**：rdpecam（WMF 后端，`device:*`；格式协商、转码与码率见 3.8）、audin（WinMM，检测到麦克风才启用）。
- **磁盘重定向**：支持 `/drive:name,path`（显式路径）与 `/drives`（通配，重定向本机**所有**有盘符的卷：系统盘、内置固定盘、U 盘、移动硬盘、网络盘，不做收敛）；与 USB 透传互斥，详见 3.6。
- **连接参数**：兼容 FreeRDP 命令行（`/v:`、`/u:`、`/p:`、`/cert:ignore`、`/f`、`/clipboard:`、`/usb:`、`/drive:`），也支持 `.rdp` 文件；忽略 `.rdp` 内分辨率、强制使用窗口/屏幕尺寸。
- **稳定性**：TCP 连接超时 15s、瞬态失败自动重试 3 次、USB 变化自动重连、证书忽略验证、GFX 开启（H264 + AVC444/444v2、ThinClient）。

### 3.5 键鼠输入转发（键盘钩子 + 鼠标按键映射）

**机制**：客户端窗口处于前台（获得焦点）时安装 `WH_KEYBOARD_LL` 低级键盘钩子，离开前台自动卸载（不影响其他应用）。钩子在系统把按键路由给任何窗口/本地 shell 之前执行——对需要拦截的按键返回非零以在本地吞掉，同时把按键序列以 RDP 扫描码通过 `freerdp_input_send_keyboard_event_ex` 转发给远端会话。实现位于 `rdp-view-item.h`（`enableKeyboardHook` / `handleLowLevelKey` / `forwardRdpKey`）。

**拦截并转发到 VM 的按键**：

| 按键 | 本地行为 | 远端（VM）行为 |
|---|---|---|
| `Win` | 吞掉（本地开始菜单不弹出） | 打开开始菜单（裸 Win，仅在未接组合键时转发一次） |
| `Win+R` | 吞掉 | 打开"运行"对话框 |
| `Win+E` | 吞掉 | 打开资源管理器 |
| `Win+D` | 吞掉 | 显示桌面 |
| `Win+I` | 吞掉 | 打开设置 |
| `Win+M` | 吞掉 | 最小化所有窗口 |
| `Win+X` | 吞掉 | 打开快速链接菜单 |
| `Win+S` | 吞掉 | 打开搜索 |
| `Win+Shift+S` | 吞掉 | 打开截图工具（区域截图） |
| `Win+Q` | 吞掉 | 打开搜索（Win11 与 Win+S 等价） |
| `Win+V` | 吞掉 | 打开剪贴板历史 |
| `Win+G` | 吞掉 | 打开游戏栏/录屏 |
| `Win+P` | 吞掉 | 打开投影设置 |
| `Win+A` | 吞掉 | 打开快速设置 |
| `Win+W` | 吞掉 | 打开小组件 |
| `Win+T` | 吞掉 | 循环聚焦任务栏（VM 内） |
| `Win+B` | 吞掉 | 聚焦通知区域 |
| `Win+U` | 吞掉 | 打开辅助功能设置 |
| `Win+数字键 1-9/0` | 吞掉 | 启动/切换任务栏第 N 个应用（VM 内） |
| `Win+Home` | 吞掉 | 最小化除当前窗口外所有窗口 |
| `Win+.` | 吞掉 | 打开表情面板 |
| `Win+PrintScreen` | 吞掉 | 截图并保存到本地图片目录（VM 内） |
| `PrintScreen` | 吞掉（本地不触发截图） | 截取 VM 全屏（同 mstsc） |
| `Alt+PrintScreen` | 吞掉 | 截取 VM 当前活动窗口 |
| `Alt+Tab` | 吞掉（本地任务切换器不弹出） | 切换窗口；按住 Alt 期间可连续按 Tab 移动选择、自由挑选窗口，松开 Alt 确认 |
| `Alt+Shift+Tab` | 吞掉 | 反向切换窗口 |
| `Ctrl+Space` | 吞掉（本地输入法不切换） | 切换输入法 / IDE 代码补全（VM 内） |
| `Ctrl+Shift+Esc` | 吞掉（本地任务管理器不弹出） | 打开任务管理器（VM 内） |
| `Ctrl+Esc` | 吞掉（本地开始菜单不弹出） | 打开开始菜单（VM 内） |

- 组合键支持任意松开顺序，不会在远端残留卡住的按键；`Win+Shift+S` 由 Shift 状态动态加入转发序列，远端 Shift 会在物理 Shift 弹起时同步释放。
- 可拦截的 Win 组合登记在 `isInterceptedWinCombo()`，增删只需改该 switch；字母键扫描码使用 `kScancodesAtoZ[]` 常量表，特殊键（`VK_HOME` / `VK_SNAPSHOT` / `VK_OEM_PERIOD`）在 `winComboScanCode()` 单独映射，数字键按主键盘数字行 1-9 连续（0x02-0x0A）、0 在末尾（0x0B）处理（PS/2 扫描码非字母序排列，不能用线性偏移）。
- `PrintScreen` / `Alt+PrintScreen` 在 Win 未按下时由独立分支转发（`RDP_SCANCODE_PRINTSCREEN`，扩展键），复用 Alt 状态跟踪。
- `Ctrl+Alt+Enter` 由钩子**本地拦截**，切换客户端本地全屏/窗口模式（mstsc 风格，触发 `toggleFullscreenRequested()` 信号 → QML `toggleDisplayMode()`），**不转发 VM**。
- `Ctrl+Space` / `Ctrl+Shift+Esc` / `Ctrl+Esc` 属"本地系统/输入法以注册热键抢先消费、Qt 根本收不到"的组合键。处理方式与 Alt+Tab 同策略：修饰键（Ctrl/Shift）经 Qt 透传转发（远端修饰状态始终真实），钩子只吞触发键（Space/Esc）并转发，因此无释放顺序卡键问题；弹窗（modal）打开时不拦截，弹窗内本地快捷键仍可用。

**不拦截（系统边界或另有处理）**：

| 按键 | 原因与行为 |
|---|---|
| `Ctrl+Alt+Del` | 系统 SAK（Secure Attention Key），用户态无法捕获；由工具栏"发送 Ctrl+Alt+Del"按钮通过 `sendCtrlAltDelete()` 主动发送给远端 |
| `Win+L` | 系统安全热键，winlogon/Secure Desktop 在内核层截获，用户态钩子收不到；按 `Win+L` 锁**本地**机器（与 mstsc 行为一致） |
| `Win+Tab` / `Win+方向键` | 由 DWM 在系统层处理，用户态钩子无法可靠拦截 |
| `Alt+Esc` | 系统保留的 shell 窗口循环组合，未拦截，本地生效 |

#### 3.5.1 鼠标按键与滚轮（中键）

**实现**：`rdp-view-item.h`（`RdpViewItem`）的 `mousePressEvent` / `mouseReleaseEvent` / `mouseMoveEvent` / `hoverMoveEvent` / `wheelEvent`。坐标先经 `mouseEventScaleSend()` 按远端桌面尺寸换算，再通过 `freerdp_input_send_mouse_event()` 下发。

**按键映射**（`rdpButtonFlags()`，Qt 按键 → RDP 标志位）：

| Qt 按键 | RDP 标志 | 值 | 含义 |
|---|---|---|---|
| `Qt::LeftButton` | `PTR_FLAGS_BUTTON1` | 0x1000 | 左键 |
| `Qt::RightButton` | `PTR_FLAGS_BUTTON2` | 0x2000 | 右键 |
| `Qt::MiddleButton` | `PTR_FLAGS_BUTTON3` | 0x4000 | **中键（滚轮按下）** |
| 其它（侧键等） | `0` | — | 返回 0 时 `event->ignore()`，既不在本地吞掉也不转发 |

- **按下 / 弹起**：按下发 `rdpButtonFlags(button) | PTR_FLAGS_DOWN`，弹起只发键位、不带 `DOWN`。RDP 侧三个键位（`BUTTON1/2/3`）互不相同，必须逐一对应，不能二选一。
- **移动**：`mouseMoveEvent` / `hoverMoveEvent` 只发 `PTR_FLAGS_MOVE`，**不带按键位**——按住哪个键由服务端自行维护状态，与 FreeRDP 官方 Windows 客户端 `wf_event.c` 的 `WM_MOUSEMOVE` 处理一致。因此"按住某键拖动"的正确性完全取决于按下/弹起的映射是否准确。
- **滚轮**：`wheelEvent` 发 `PTR_FLAGS_WHEEL`，负向增量附加 `PTR_FLAGS_WHEEL_NEGATIVE`，低 8 位（`WheelRotationMask`）携带 `|delta|`；与按键映射相互独立。

**此前缺陷（已修复）**：按下/弹起原为 `(event->button() == Qt::LeftButton) ? PTR_FLAGS_BUTTON1 : PTR_FLAGS_BUTTON2` 的二选一写法，**中键因此被当作右键发送**（弹起同样发右键）。在 3D 设计软件中按住滚轮拖动本应平移/环绕视图，实际变成右键拖动，表现为画面无响应或弹出右键菜单。现改为三键逐一映射，中键正确走 `PTR_FLAGS_BUTTON3`。

### 3.6 磁盘重定向（`/drives` 通配）与 USB 互斥

**背景**：部分 U 盘（尤其固态 U 盘）走 `urbdrc` USB 透传时在 Win10 虚拟机内无法识别，而磁盘重定向（rdpdr + `drive` 设备服务）走的是另一条更稳定的通道。因此策略是——**只要本次连接启用了磁盘重定向，就把本机全部有盘符的卷都通过磁盘重定向送进 VM；已被磁盘重定向的存储设备不再出现在 USB 透传列表里**。

#### 3.6.1 重定向全走 FreeRDP 官方机制

qf-client **不再做任何私有展开**，只依赖官方链路：

| 服务端下发 | FreeRDP 解析结果 | 后续行为 |
|---|---|---|
| `/drives` | `FreeRDP_RedirectDrives=TRUE` | `freerdp_client_load_addins()` 加入设备 `{"drive","media","*"}` |
| `drivestoredirect:s:*` / `*` | `FreeRDP_DrivesToRedirect="*"` | 同上 |
| `/drive:name,path` | 显式设备（`Path` 为具体路径） | 只重定向该路径，不触发通配枚举 |

连接时 `rdpdr` 通道 `rdpdr_add_devices()` 遇到 `Path == "*"` 即调用 `first_hotplug()`：

1. `GetLogicalDrives()` 取本机盘符位图，逐盘调用 `check_path()` 过滤，条件为
   `GetDriveTypeA ∈ {DRIVE_FIXED, DRIVE_REMOVABLE, DRIVE_CDROM, DRIVE_REMOTE}` 且 `GetVolumeInformationA` 成功。
   - **不做收敛**：系统盘、内置固定盘、U 盘 2.0/3.0、固态 U 盘、移动硬盘、网络盘一并纳入（例如实测枚举到 `C:\`(type=3)、`Z:\`(type=4 网络盘)）。
   - 未格式化卷、`D:\`(光驱无盘, type=5) 等被自然排除。
2. 每个通过的盘调用 `rdpdr_load_drive(rdpdr, name, path, TRUE)`（`automount=TRUE`）→ `devman_load_device_service()` 装载 `drive` 设备服务。
3. `first_hotplug()` 执行时通道仍在 `RDPDR_CHANNEL_STATE_INITIAL`，设备先注册进 devman 缓存；待协商到 `READY` 并收到 `PAKID_CORE_USER_LOGGEDON` 后，统一以 `PAKID_CORE_DEVICELIST_ANNOUNCE` 公告，服务端逐个回 `PAKID_CORE_DEVICE_REPLY status=0`。
4. 连接后由 rdpdr 官方热插拔线程监听 `WM_DEVICECHANGE`（`DBT_DEVICEARRIVAL` / `DBT_DEVICEREMOVECOMPLETE`）完成插入自动出现盘符、拔出自动消失。

> **前置依赖**：该通配路径依赖 §2.5.1 的 FreeRDP 本地补丁，否则 `first_hotplug()` 枚举通过后 `rdpdr_load_drive()` 会静默失败，VM 内不会有任何盘符。

**已移除的旧实现**：旧版 qf-client 自己解析 `/drives` 并展开成若干 `/drive:` 私有参数，只收 `DRIVE_FIXED`、是静态快照（插入/拔出不生效），且会与官方 `first_hotplug()` 对同一盘符重复注册（同名同路径、`automount` 一真一假）。现已整体删除。

#### 3.6.2 与 USB 的互斥（列表置灰）

- **模式判定**：`updateDriveRedirectState()` 在每次连接/重连的 `PreConnect` 中读取服务端下发的配置，得到 `DriveRedirectMode` 与被重定向盘符集合：

  | 判定依据 | 模式 | 含义 |
  |---|---|---|
  | `DrivesToRedirect` 含 `*` | `Wildcard` | 所有有盘符的卷都已重定向 |
  | `DrivesToRedirect` 形如 `C,D` / `C:,D:` / `label(C:\)` | `Explicit` | 仅解析出的盘符被重定向 |
  | `RedirectDrives = TRUE`（即 `/drives`） | `Wildcard` | 同上 |
  | 仅 `/drive:name,path` | `Explicit` | 取 path 首字符作为盘符 |

- **状态下发**：结果通过 `USBManager::setDiskRedirectState(wildcard, letters)` 同步给 USB 管理器。
- **盘符映射**：USB 侧用 SetupAPI 枚举**卷设备** → 关联**磁盘设备** → `CM_Get_Parent()` 回溯父设备链取 USB 设备的 VID/PID，把"盘符 → VID:PID"落到 `DeviceInfo::diskRedirected` / `driveLetters`。
- **置灰规则**：`Wildcard` 下所有"已挂载卷"的 USB 存储设备置灰；`Explicit` 下仅命中盘符的置灰。置灰项**不可勾选**，也没有"改用 USB 透传"的逃生开关；但**映射失败或判断不出来的一律保留 USB 选项**——MTP/PTP、加密狗、U 盾等无盘符设备继续走 USB 透传，USB 列表不因本方案而收缩。**额外的置灰例外**：设备除存储接口外还存在其它接口（自定义 0xFF / HID / Audio 等）时，视为复合设备，**不整体置灰**，保留 USB 勾选项（见 3.7.3）。
- **实时刷新**：`VolumeChangeFilter`（`QAbstractNativeEventFilter`）监听 `WM_DEVICECHANGE` 的 `DBT_DEVICEARRIVAL` / `DBT_DEVICEREMOVECOMPLETE` / `DBT_DEVNODES_CHANGED`，**去抖 400 ms** 后触发 `USBManager::enumerate()`（重跑 libusb 枚举，其内部会重建"USB ↔ 盘符"映射并 `emit deviceListChanged`），再补一次 `refreshDiskRedirectMap()` 兜底。此过滤器与 rdpdr 官方热插拔线程相互独立：官方线程负责把盘符送进 VM，此处负责让 USB 弹窗的列表与标注实时跟随（详见 3.7.4）。
- **UI**：`main.qml` 通过 `usbManager.isDiskRedirected(i)` / `usbManager.deviceDriveLetters(i)` 渲染置灰与盘符标注（如 `E:, F:`），通过 `usbManager.isStorageComposite(i)` 渲染复合设备提示 `⚠ 含存储接口`。

#### 3.6.3 验收要点

1. 固态 U 盘插入后 VM 内自动出现盘符并可读写；
2. 拔出后 VM 内盘符消失；
3. 同一个盘不会同时以"USB 设备 + 盘符"两种形态出现（**例外**：复合设备按 3.7.3 保留 USB 勾选项，若用户两边都选则可能出现两份，属预期行为）；
4. 未格式化/加密盘、手机等无盘符设备仍能在 USB 列表里选到。

### 3.7 USB 透传（urbdrc）：过滤、选择与插拔刷新

#### 3.7.1 设备过滤规则（`shouldShowDevice()`）

`bDeviceClass == 0x09`（Hub）与 `0xE0`（无线/蓝牙控制器）直接隐藏；其余按**接口类别**判定——遍历当前配置的每个接口（取首个 altsetting 的 `bInterfaceClass`），**仅当所有接口都属于 HID(0x03) / Audio(0x01) 时才隐藏**。只要出现其它类别（含厂商自定义 0xFF、Mass Storage 0x08）就保留。读不到接口描述符（权限/后端限制）时保守保留，避免漏掉设备。

- 效果：键盘、鼠标、耳机仍被隐藏；**带 HID 子接口的加密狗能出现在列表里**。
- 副作用：带厂商接口的游戏鼠标/宏键盘也会出现——仅凭类码无法把它们与"狗"区分开。
- 同时把"是否存在非存储接口"记入 `DeviceInfo::hasNonStorageInterface`，供 3.7.3 的置灰例外使用（libusb 描述符已在手，无额外开销）。

#### 3.7.2 选择语法（`id:` 与 `addr:`）

FreeRDP 官方语法（`client/common/cmdline.h`）：`/usb:[dbg,][id:<vid>:<pid>#...,][addr:<bus>:<addr>#...,][auto]`。

- **默认 `id:VID:PID`**；同一 VID:PID 出现多支时无法区分，此时**整次连接**改用 `addr:bus:addr`（十六进制，取自 `libusb_get_bus_number()` / `libusb_get_device_address()`，经 `USBManager::selectedDevices()` 带出）；取不到 bus/addr 时回退 `id:` 并告警。
- 两条硬约束（源自 urbdrc 实现 `channels/urbdrc/client/libusb/libusb_udevman.c`）：
  1. **`id:` 与 `addr:` 二选一**——`udevman_listener_created_callback()` 先看 `devices_vid_pid`，命中即 `return`，`devices_addr` 永不生效。所以不能"只对其中一支切换模式"，只能整体切换。
  2. **多个设备必须写进同一条参数、用 `#` 分隔**——解析时对 `devices_vid_pid` / `devices_addr` 是直接赋值，写多个同类参数只会保留最后一个。
- **`serial:` 上游不支持**：urdrc 只解析 `id` / `addr` / `dev` / `device` / `auto` / `dbg` / `sys`，没有序列号分支。
- `addr:` 是**动态**的：重插或换 USB 口后 bus/addr 会变，需重新选择；配合 3.7.4 的自动刷新与重连兜底。

#### 3.7.3 与磁盘重定向的互斥（含复合设备例外）

置灰与盘符标注的判定见 3.6.2。补充要点：设备**除存储接口外还存在其它接口**（自定义 0xFF / HID / Audio 等）时视为复合设备，**不整体置灰**、保留 USB 勾选项，UI 标注 `⚠ 含存储接口`。注意 urbdrc 是**整设备**透传、不做接口级切分，所以复合设备若两个选项同时生效，VM 内可能同时出现 USB 设备与盘符（因此默认不勾选，由用户显式决定）。

#### 3.7.4 插拔自动刷新（替代失效的 libusb hotplug）

`libusb_hotplug_register_callback()` 在 Windows 上实测返回 `LIBUSB_ERROR_NOT_SUPPORTED`（FreeRDP 的 urbdrc 同样打印 `Platform does not support libusb hotplug`），因此插上设备后列表不会自动更新。改以 Win32 `WM_DEVICECHANGE` 为触发源：`DBT_DEVNODES_CHANGED` 会广播给所有顶层窗口，**无需 `RegisterDeviceNotification`**；`VolumeChangeFilter`（`QAbstractNativeEventFilter`）收到后**去抖 400 ms**，再调用 `USBManager::enumerate()`（内部重建"USB ↔ 盘符"映射并 `emit deviceListChanged`）与 `refreshDiskRedirectMap()`。`enumerate()` 本身已是"后台线程 + `m_enumRunning` 去重"，不会阻塞 QML 线程。

#### 3.7.5 通道参数的写入方式（易踩坑）

`freerdp_client_add_dynamic_channel()` 对**已存在**的通道直接返回 `TRUE` 且不追加任何参数（`client/common/cmdline.c`）。而工具栏要修改的恰恰是 CLI / `.rdp` 已注册的 `urbdrc` 通道，因此 `PreConnect` 中必须**先 `freerdp_client_del_dynamic_channel(settings, URBDRC_CHANNEL_NAME)` 再重新添加**，否则勾选不会生效。未勾选任何设备时不改动通道，沿用 CLI / `.rdp` 的原始参数。

#### 3.7.6 前置依赖

- **UsbDk 必须安装**（`libusb_set_option(LIBUSB_OPTION_USE_USBDK)`；安装包内置 `UsbDk_1.0.22_x64.msi`），否则设备枚举/占用可能失败。
- 加密狗常被厂商驱动独占，透传前可能需要在**本机**停掉厂商服务。

### 3.8 摄像头重定向：格式选择与转码码率

**协商链路**：`getSupportedFormats()` 生成"摄像头侧格式 → 网络侧格式"候选表 → HAL（WMF）用候选的**输入格式**匹配摄像头原生 MF 子类型（`GetMediaTypeDescriptions`，内部 `GetNativeMediaType` 遍历全部原生类型）→ 命中**第一个**候选即选定格式对 → 上报媒体类型列表中所有条目的 `Format` 统一改写为该候选的**输出格式** → 每帧经 `ecam_encoder_compress()` → `freerdp_video_sample_convert()` 转换/编码后上行。

**候选表顺序（本地补丁，详见 §2.5.3）**：

| 序 | 格式对（输入→输出） | 适配的摄像头 |
|---|---|---|
| 0 | H264 → H264 | 原生 H264 的会议摄像头，**直通零编码** |
| 1 | MJPG → H264 | 主流 USB 摄像头（解 MJPEG 后重编码） |
| 2 | YUY2 → H264 | 仅出 YUY2 的设备 |
| 3 | NV12 → H264 | 仅出 NV12 的设备（部分笔记本内建 / Surface 系 / 红外） |
| 4 | I420 → H264 | 仅出 I420 的设备 |
| 5+ | MJPG→MJPG、MJPG→YUY2、YUY2→YUY2 等 | 原始/直通组合，**实际不可达**（前面必先命中 `→H264`） |

**为什么"顺序"就是"带宽"**：`src == dst` 时 `freerdp_video_sample_convert()` 走原样拷贝分支，**一帧都不编码**；而 H264 输出的候选排在最前，任何被支持的摄像头都会先命中 `→H264`，从而进入转码链路。

**码率**：目标码率由 `h264_get_max_bitrate(height)` 按高度查表（1080→2700、720→1250、480→700、360→400、240→170、180→140 kbps）；`ecam_encoder_context_init()` 传 `bitrate=0` 触发自动计算，再用 **VBR** 下发才能真正生效（本地补丁，详见 §2.5.4）。若仍是 CQP，码率随画面复杂度自由浮动、不可控。

**实测（外接 USB 摄像头 `vid_4a54&pid_5232`，1920×1080@30fps）**：

| 项目 | 改造前 | 改造后 |
|---|---|---|
| 上报格式 | `Format: 2`（MJPEG） | `Format: 1`（H264） |
| 上行带宽 | 20~30 Mbps | **2.71 Mbps** |
| 客户端开销 | 无（原样直通） | 每帧 MJPEG 解码 + 色彩转换 + H.264 软编 |

改造后统计口径（`/log-level:DEBUG` 抓包聚合）：5912 帧 / 63.78 MB / 平均 11.05 KB/帧 / 帧大小 0.71~54.76 KB（关键帧）。全程仅 1 帧因摄像头吐出残缺 JPEG 被丢弃（`No JPEG data found`；客户端设了 `AV_EF_EXPLODE`，坏帧整帧丢弃而不硬解，属预期容错）。

**日志观察点**：`[ecam_dev_print_media_type]: Format: 1`（1=H264、2=MJPG、3=YUY2、4=NV12、5=I420）确认选中的输出格式；`[video_get_h264_bitrate]: Auto-calculated H.264 bitrate: 2700 kbps` 确认目标码率。注意后者每帧打印一次（`ecam_encoder_compress()` 每帧都会调 `ecam_encoder_context_init()`），DEBUG 级别下日志量较大。

**兼容性（按闸门判定）**：

| 摄像头 | 原生格式 | 结果 |
|---|---|---|
| 主流 USB 摄像头 | MJPG + YUY2 | ✅ MJPG→H264 |
| 工业相机 / 低端设备 | 仅 YUY2 | ✅ YUY2→H264 |
| 会议摄像头 | 原生 H264 | ✅ 直通，零 CPU |
| 笔记本内建 / Surface 系 | 仅 NV12 或 I420 | ✅ NV12/I420→H264（§2.5.3 新增） |
| 虚拟相机 / 采集卡 | 仅 RGB24/RGB32 | ❌ 报"不支持任何兼容格式"（映射与转换已实现但未纳入候选表） |
| 红外/深度（Windows Hello） | NV12，多流 | ⚠️ 格式可协商，但只读第一条视频流 |
| 仅 DirectShow 的虚拟摄像头/老采集卡 | — | ❌ MF 枚举不到，设备列表里根本不出现 |

**已知缺口与风险**：

1. **行跨度（stride）假设**：原始格式按"行紧密排列"推算平面地址（`freerdp_video_fill_plane_info()` 用 `av_image_fill_pointers` 按 width 推 stride），未读取 MF 的 `MF_MT_DEFAULT_STRIDE`。若某驱动样本带行填充或负跨度，NV12/YUY2 画面会出现**斜切/错位**。
2. **H264 直通不做码流形态校验**：不检查起始码/SPS-PPS。若摄像头输出的 H264 形态服务端不认，表现为黑屏且日志无线索。
3. **服务端需具备 H264 解码能力**：现在**默认**输出 H264，且客户端**没有**"被服务端拒绝就退回 MJPEG"的回退逻辑。目标环境若为异构 Windows 版本群，需单独评估。
4. **只支持第一条视频流**：HAL 固定用 `MF_SOURCE_READER_FIRST_VIDEO_STREAM`，传入的 `streamIndex` 仅记录不生效；每设备只维护一个 stream 对象（按 deviceId 索引）。
5. **独占占用**：摄像头被本机其它程序（Teams/相机应用）占用时打不开，仅 5 次重试 × 400ms。
6. **本地 USB 侧不受优化**：始终按摄像头"第一条匹配的原生类型"取流（`stream->nativeMediaType`，**忽略**服务端请求的尺寸），带宽优化只作用于 RDP 上行；服务端选小分辨率时靠 sws 静默缩放，本地仍跑满原生分辨率。
7. **帧率整数除法**：`fr = FrameRateNumerator / FrameRateDenominator`（29.97→29），且对分母为 0 无兜底——服务端下发的媒体类型会校验分母非 0，但列表首项（摄像头原生值）直接作为 `currMediaType` 时不校验。
8. **上报列表带重复**：同一分辨率会按命中的候选数重复出现（原 25 条），服务端可接受但不规范。

### 3.9 构建与部署（build-qf-client.ps1）
1. 依赖校验：FreeRDP install 目录、vcpkg toolchain、Qt 6.11.1、VS2022。
2. CMake + Ninja + MSVC 编译出 `qf-client.exe`。
3. 部署运行时到 build 目录：
   - FreeRDP DLL（freerdp3、freerdp-client3、winpr3）；
   - OpenSSL（libcrypto-3-x64、libssl-3-x64）+ **legacy.dll + openssl.cnf**（供 Win7 NTLM/MD4 使用，`main()` 中通过 `_putenv_s` 设置 `OPENSSL_MODULES`/`OPENSSL_CONF`）；
   - FFmpeg（avcodec/avformat/avutil 等）、OpenH264、libx264、zlib；
   - Qt 核心/Quick/Controls 相关 DLL、`platforms/qwindows.dll`、imageformats、iconengines、QML 模块（QtQml/QtQuick/...）、MSVC 运行时（VC143 CRT）。

### 3.10 运行示例
```
qf-client.exe /v:192.168.1.90 /u:administrator /p:123456 /cert:ignore /f
```

---

## 四、vdi-client-windows-main（VDI 管理客户端，VDIClient.exe）

### 4.1 项目性质
- 基于 **Qt 6（Widgets + Network）** 的 VDI 管理客户端，版本 **1.6.1**（CMake 中 `project(VDIClient VERSION 1.6.1)`，安装包版本同步见 `installer.iss` 的 `MyAppVersion`）。
- 语言标准：**C++17**，构建工具 CMake（仓库内 `build/` 为 MSVC 的 VS 工程产物，含 `VDIClient.sln`）。
- 功能定位：登录 → 虚拟机列表管理 → 拉起 RDP 客户端（qf-client.exe）完成远程连接。

### 4.2 源码构成
| 文件 | 职责 |
|---|---|
| `src/main.cpp` | 入口：创建 `QApplication`，切换到 exe 所在目录，最大化显示 `LoginWindow` |
| `src/loginwindow.cpp/.h` | 登录窗口 + 虚拟机列表管理窗口（QStackedWidget 切换），全部 UI 与 API 交互逻辑 |
| `resources/`（README 描述） | app.rc、app.ico、logo.png、resources.qrc |
| `bin/` | 部署好的运行时：`qf-client.exe` 及全部依赖 DLL（与 qfreerdp 的 build 产物一致） |
| `build/` | 编译产物 + `bin/` 拷贝 + `Release/VDIClient.exe` |

### 4.3 功能特性
- **登录认证**：输入服务器（域名/IP[:端口]，缺省补 `:443`）→ `api/v1/auth/health` 健康检查 → `api/v1/auth/login` 登录获取 token。
- **虚拟机管理**：`api/v1/users/<user>/vms` 获取列表，每个 VM 支持：
  - 开机 `vm/<id>/start`、关机 `vm/<id>/shutdown`、重启 `vm/<id>/restart`；
  - 还原 `vm/<id>/rollback`（通过 `vm/<id>/hasmilestone` 判断是否显示还原按钮）；
  - 状态查询 `vm/<id>/currentstatus`（running/stopped/paused）。
- **RDP 连接（关键）**：
  1. `vm/<id>/rdp` 下载 RDP 文件 → 保存到 `%LOCALAPPDATA%/<App>` 的 `template.rdp`；
  2. `vm/<id>/login` 获取连接命令（字符串）；
  3. 解析命令参数（去掉首项程序名），把其中的 `template.rdp` 替换为本地完整路径；
  4. 在 `bin/` 目录下以 **QProcess 启动 `qf-client.exe`** 并传入参数；
  5. 通过 `finished`/`errorOccurred` 信号监控子进程退出。
- **其他**：修改密码 `users/password`、心跳 `users/heartbeat`（15s 一次）、token 过期（HTTP 401）自动返回登录页、多语言（English/简体中文/繁體中文/日本語，内嵌翻译字典）、记住密码与自动登录（QSettings 存储）、HTTPS 关闭证书校验（开发环境）。
- **打包**：CMake 会拷贝 `bin/`（含 qf-client.exe 全套运行时）与 `drivers/UsbDk_1.0.22_x64.msi`（USB DK 驱动，README 提及用 Inno Setup 6 打包为 VDIClient-Setup.exe）。

### 4.4 与 README 的差异说明
- 仓库 README_ZH 描述的项目结构（`src/` 含 loginwindow、`bin/` 含 template.rdp/wfreerdp.exe、`build_and_package.bat`、`installer.iss`）为旧版 `vdi-qt-bak-test` 的说明；当前实际仓库源码仅有 `main.cpp` + `loginwindow`，RDP 客户端已从 wfreerdp.exe 切换为 **qf-client.exe**。

---

## 五、关键技术要点汇总

1. **RDP 渲染链路（qf-client）**：FreeRDP GDI buffer → CPU staging buffer（脏矩形）→ D3D11 纹理（`UpdateSubresource` 零拷贝）→ Qt 场景图。渲染与 RDP 线程分离，通过 `QMetaObject::invokeMethod(QueuedConnection)` 做线程切换。
2. **动态分辨率**：disp 通道 `SendMonitorLayout` + GFX 图形管道（H264/AVC444）协同，带 300ms 防抖与 GFX_RESET 一致性跟踪。
3. **剪贴板文件传输**：自实现 FileGroupDescriptorW 解析/序列化，远端→本地按 64KB 分块下载，含路径安全校验（拒绝绝对路径与 `..`）。
4. **USB 重定向**：libusb 枚举 → 过滤（仅"全部接口都是 HID/Audio"才隐藏，故带 HID 子接口的加密狗可见）→ 工具栏勾选 → urbdrc 动态通道（默认 `id:VID:PID`，同型号多支时整体切 `addr:bus:addr`；多设备写在**同一条**参数里用 `#` 分隔）→ 触发 RDP 自动重连。插拔刷新改由 `WM_DEVICECHANGE` 去抖触发（libusb hotplug 在 Windows 上不可用）。改动通道参数前必须先 `freerdp_client_del_dynamic_channel()`，否则对已存在的通道添加参数是空操作（详见 §3.7）。
5. **磁盘重定向（/drives）**：全走 FreeRDP 官方机制——`/drives` → `FreeRDP_RedirectDrives=TRUE` → 设备 `{"drive","media","*"}` → rdpdr `first_hotplug()` 枚举所有有盘符的卷（`check_path()` 过滤 FIXED/REMOVABLE/CDROM/REMOTE）→ `rdpdr_load_drive(automount=TRUE)` → 通道 READY + `USER_LOGGEDON` 后统一 `DEVICELIST_ANNOUNCE`；插拔由 rdpdr 官方 `WM_DEVICECHANGE` 线程热重定向（详见 §3.6）。**与 USB 互斥**：已纳入磁盘重定向的 USB 存储设备在 USB 列表置灰并标注盘符、禁止勾选；无盘符设备（U 盾/加密狗/MTP）仍走 USB 透传；**复合设备（存储接口之外还有自定义/HID 接口）不整体置灰**，保留勾选项并标注 `⚠ 含存储接口`。
6. **多进程协作**：VDIClient.exe（管理面）与 qf-client.exe（数据面）解耦，通过 QProcess + 命令行参数（含 .rdp 文件）协作。
7. **构建链**：vcpkg（依赖）→ FreeRDP（build-freerdp.ps1，**含 §2.5 本地补丁**）→ qf-client（build-qf-client.ps1）→ VDIClient（CMake 拷贝 bin/ 打包）。改 FreeRDP 源码后必须重编并同步 `freerdp3.dll` / `freerdp-client3.dll` / `winpr3.dll`，否则包里的客户端仍带旧库。
8. **系统快捷键拦截（qf-client）**：`WH_KEYBOARD_LL` 低级键盘钩子仅在客户端窗口前台时启用，本地吞掉 Win/Win+字母/Alt+Tab/PrintScreen 及被本地热键抢占的 Ctrl+Space/Ctrl+Shift+Esc/Ctrl+Esc 并转发到 RDP 会话（详见 3.5）；`Win+L` 与 `Ctrl+Alt+Del` 属系统安全边界，用户态无法拦截——Win+L 锁本地机器（同 mstsc），Ctrl+Alt+Del 由工具栏按钮发送。键盘事件转发采用"映射表优先、盲区回退物理扫描码"策略，保证 Delete/方向键等扩展键的 RDP 扩展位正确。
9. **鼠标按键映射（qf-client）**：Qt 三键逐一映射到 RDP 键位（左/右/中 → `PTR_FLAGS_BUTTON1/2/3`，按下额外带 `PTR_FLAGS_DOWN`），未知键返回 0 后 `event->ignore()` 直接放行；移动事件只发 `PTR_FLAGS_MOVE`、**不带按键位**，拖动状态由服务端维护；滚轮走 `PTR_FLAGS_WHEEL` + 低 8 位增量（负向附加 `PTR_FLAGS_WHEEL_NEGATIVE`）。此前中键被误当作右键（二选一写法），导致 3D 软件中按住滚轮拖动无效（详见 3.5.1）。
10. **摄像头重定向（rdpecam）**：带宽取决于**候选格式表的顺序**——`src == dst` 时 `freerdp_video_sample_convert()` 只做原样拷贝、**完全不编码**，所以把 `→H264` 的候选排在首位才进入转码链路。本地补丁把候选表改为 `H264 优先 + 补 NV12/I420`，并把 H.264 码控由 CQP 改为 VBR 让目标码率真正生效（详见 §2.5.3、§2.5.4、§3.8）。实测外接 1080p 摄像头由 20~30 Mbps 降至 **2.71 Mbps**。NV12/I420 只作为**输入**格式，由 `isNetworkFormat()` 白名单排除在上行输出之外（裸 YUV 上行 1080p 约 750 Mbps）。
