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
| `mini-qf-client.cc` | 核心连接引擎：`PreConnect`/`PostConnect`/`LoadChannels` 回调、独立 RDP 线程的事件循环、连接重试（3 次）、断线重连状态机、剪贴板（cliprdr）回调实现、GFX/disp 动态通道管理、命令行解析、OpenSSL 环境变量设置、分辨率计算 |
| `rdp-view-item.h` | `RdpViewItem`（QQuickItem）：**D3D11 原生纹理渲染管线**（`RdpFrameTexture` + QRhi）、鼠标/滚轮/键盘事件转发、光标显示/隐藏、剪贴板数据转换（文本/图片/文件） |
| `usb-manager.cc/.h` | 基于 libusb 的 USB 设备枚举与热插拔监听、设备选择状态管理，USB 重定向触发重连 |
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
- **USB 重定向**：工具栏 USB 弹窗选择设备（libusb 枚举，热插拔），选择确认后通过 `urbdrc` 动态通道以 `id:VID:PID` 重定向，并自动重连。
- **摄像头/麦克风重定向**：rdpecam（WMF 后端，`device:*`）、audin（WinMM，检测到麦克风才启用）。
- **磁盘重定向**：支持 `/drive:name,path` 与 `/drives`（枚举全部固定盘）。
- **连接参数**：兼容 FreeRDP 命令行（`/v:`、`/u:`、`/p:`、`/cert:ignore`、`/f`、`/clipboard:`、`/usb:`、`/drive:`），也支持 `.rdp` 文件；忽略 `.rdp` 内分辨率、强制使用窗口/屏幕尺寸。
- **稳定性**：TCP 连接超时 15s、瞬态失败自动重试 3 次、USB 变化自动重连、证书忽略验证、GFX 开启（H264 + AVC444/444v2、ThinClient）。

### 3.5 快捷键拦截（WH_KEYBOARD_LL 键盘钩子）

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

### 3.6 构建与部署（build-qf-client.ps1）
1. 依赖校验：FreeRDP install 目录、vcpkg toolchain、Qt 6.11.1、VS2022。
2. CMake + Ninja + MSVC 编译出 `qf-client.exe`。
3. 部署运行时到 build 目录：
   - FreeRDP DLL（freerdp3、freerdp-client3、winpr3）；
   - OpenSSL（libcrypto-3-x64、libssl-3-x64）+ **legacy.dll + openssl.cnf**（供 Win7 NTLM/MD4 使用，`main()` 中通过 `_putenv_s` 设置 `OPENSSL_MODULES`/`OPENSSL_CONF`）；
   - FFmpeg（avcodec/avformat/avutil 等）、OpenH264、libx264、zlib；
   - Qt 核心/Quick/Controls 相关 DLL、`platforms/qwindows.dll`、imageformats、iconengines、QML 模块（QtQml/QtQuick/...）、MSVC 运行时（VC143 CRT）。

### 3.7 运行示例
```
qf-client.exe /v:192.168.1.90 /u:administrator /p:123456 /cert:ignore /f
```

---

## 四、vdi-client-windows-main（VDI 管理客户端，VDIClient.exe）

### 4.1 项目性质
- 基于 **Qt 6（Widgets + Network）** 的 VDI 管理客户端，版本 **1.5.0**（CMake 中 `project(VDIClient VERSION 1.5.0)`）。
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
4. **USB 重定向**：libusb 枚举 + 热插拔回调 → urbdrc 通道 `id:vid:pid` → 触发 RDP 自动重连。
5. **多进程协作**：VDIClient.exe（管理面）与 qf-client.exe（数据面）解耦，通过 QProcess + 命令行参数（含 .rdp 文件）协作。
6. **构建链**：vcpkg（依赖）→ FreeRDP（build-freerdp.ps1）→ qf-client（build-qf-client.ps1）→ VDIClient（CMake 拷贝 bin/ 打包）。
7. **系统快捷键拦截（qf-client）**：`WH_KEYBOARD_LL` 低级键盘钩子仅在客户端窗口前台时启用，本地吞掉 Win/Win+字母/Alt+Tab/PrintScreen 及被本地热键抢占的 Ctrl+Space/Ctrl+Shift+Esc/Ctrl+Esc 并转发到 RDP 会话（详见 3.5）；`Win+L` 与 `Ctrl+Alt+Del` 属系统安全边界，用户态无法拦截——Win+L 锁本地机器（同 mstsc），Ctrl+Alt+Del 由工具栏按钮发送。键盘事件转发采用"映射表优先、盲区回退物理扫描码"策略，保证 Delete/方向键等扩展键的 RDP 扩展位正确。
