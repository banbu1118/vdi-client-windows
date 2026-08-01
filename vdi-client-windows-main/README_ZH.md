# VDI Client

一个基于 Qt6 的虚拟桌面基础设施（VDI）客户端应用程序，通过 RDP（远程桌面协议）连接到虚拟机，为用户提供远程桌面访问功能。

## 目录

- [项目概述](#项目概述)
- [功能特性](#功能特性)
- [系统要求](#系统要求)
- [项目结构](#项目结构)
- [快速开始](#快速开始)
- [构建和打包](#构建和打包)
- [使用说明](#使用说明)
- [配置说明](#配置说明)
- [常见问题](#常见问题)
- [许可证](#许可证)
- [贡献](#贡献)

## 项目概述

VDI Client 是一个现代化的桌面应用程序，专为企业和个人用户设计，提供便捷的虚拟桌面访问体验。基于 Qt6 框架开发，采用 FreeRDP 作为 RDP 连接引擎，支持多种语言和丰富的功能特性。

### 技术栈

- **开发框架**: Qt 6.10.2
- **编程语言**: C++17
- **构建工具**: CMake 3.30.5
- **RDP 引擎**: FreeRDP
- **打包工具**: Inno Setup 6

## 功能特性

### 核心功能

- **RDP 连接**: 通过 RDP 协议连接到虚拟机
- **用户登录**: 支持用户名和密码认证
- **连接管理**: 管理多个虚拟机连接
- **远程桌面**: 完整的远程桌面体验

### 用户体验

- **多语言支持**: 英语、简体中文、繁体中文、日语
- **自动登录**: 支持保存凭据实现自动登录
- **记住密码**: 安全地存储用户密码
- **现代化界面**: 基于 Qt6 的现代化用户界面设计
- **响应式布局**: 自适应不同屏幕尺寸

### 高级特性

- **连接模板**: 可自定义 RDP 连接参数
- **会话管理**: 支持多会话并发连接
- **断线重连**: 自动重连功能
- **日志记录**: 详细的连接和操作日志

## 系统要求

### 运行环境

- **操作系统**: Windows 10 或更高版本（推荐 Windows 11）
- **架构**: x64
- **网络**: 需要稳定的网络连接以访问 VDI 服务器
- **权限**: 可能需要管理员权限以建立 RDP 连接

### 开发环境

- **Qt 6.10.2** (MinGW 64-bit)
- **CMake 3.30.5 或更高版本**
- **MinGW-w64** (随 Qt 安装)
- **Inno Setup 6** (用于打包)

## 项目结构

```
vdi-qt-bak-test/
├── bin/
│   ├── template.rdp           # RDP 连接模板文件
│   └── wfreerdp.exe          # FreeRDP 可执行文件
├── resources/
│   ├── app.rc                 # Windows 资源文件
│   ├── app.ico                # 应用程序图标
│   ├── logo.png               # Logo 图片
│   └── resources.qrc          # Qt 资源文件
├── src/
│   ├── main.cpp               # 应用程序入口点
│   ├── loginwindow.cpp        # 登录窗口实现
│   └── loginwindow.h          # 登录窗口头文件
├── CMakeLists.txt             # CMake 构建配置
├── build_and_package.bat      # 自动化构建脚本
├── installer.iss              # Inno Setup 安装程序脚本
├── app.ico                    # 应用程序图标
├── LICENSE                    # 许可证文件
├── README_ZH.md               # 中文说明文档
└── README_EN.md               # 英文说明文档
```

## 快速开始

### 安装应用

1. 下载最新的安装包 `VDIClient-Setup.exe`
2. 双击运行安装程序
3. 按照安装向导完成安装
4. 启动应用程序并配置连接

### 基本使用

1. 输入服务器地址
2. 输入用户名和密码
3. 点击"连接"按钮
4. 等待连接建立
5. 开始使用远程桌面

## 构建和打包

### 环境准备

#### 1. 安装 Qt

下载地址：https://www.qt.io/download-qt-installer

**安装步骤**：
- 运行 Qt 在线安装程序
- 默认安装，安装时会自动安装cmake和mingw

**网络问题解决方案**：
如果网络错误导致安装失败，可以使用国内镜像源：

```bash
qt-online-installer-windows-x64-4.10.0.exe --mirror https://mirrors.ustc.edu.cn/qtproject
```

#### 2. 安装 Inno Setup

下载地址：https://jrsoftware.org/isdl.php

**安装步骤**：
- 下载 Inno Setup 6
- 运行安装程序
- 完成默认安装

#### 43. 配置环境变量

Qt 安装时会自动安装 MinGW，需要配置以下环境变量：

**添加到系统 PATH**：
```
C:\Qt\Tools\mingw1310_64\bin
C:\Qt\6.10.2\mingw_64\bin
C:\Qt\Tools\CMake_64\bin
```

**验证安装**：
```bash
gcc --version
cmake --version
```

### 构建步骤

在项目根目录下执行：

```bash
build_and_package.bat
```

脚本会自动完成以下步骤：
1. 清理之前的构建文件
2. 配置 CMake 项目
3. 编译应用程序
4. 部署 Qt 依赖库
5. 生成安装程序包

**输出位置**：
- 安装程序：`installer_output/VDIClient-Setup.exe`

## 使用说明

### 多语言切换

应用程序支持以下语言：
- 英语 (English)
- 简体中文 (简体中文)
- 繁体中文 (繁體中文)
- 日语 (日本語)

切换方法：在设置菜单中选择语言选项。

### 自动登录

1. 勾选"记住密码"选项
2. 输入凭据后连接
3. 下次启动时会自动填充凭据
4. 可以启用"自动登录"实现无感连接


### 连接问题

**Q: 无法连接到虚拟机**

A: 检查以下项目：
- 网络连接是否正常
- 虚拟机地址是否正确（VDI服务器自动获取虚拟机ip）
- 防火墙是否允许 RDP 连接（默认端口 3389）
- VDI 服务器是否可访问

## 许可证

本项目采用 GNU Affero General Public License Version 3.0 (AGPL-3.0) 开源许可证。

详见 [LICENSE](LICENSE) 文件。

## 贡献

我们欢迎任何形式的贡献！

### 贡献方式

- 报告 Bug
- 提出新功能建议
- 提交代码改进
- 改进文档
- 翻译工作

### 开发流程

1. Fork 本仓库
2. 创建特性分支 (`git checkout -b feature/AmazingFeature`)
3. 提交更改 (`git commit -m 'Add some AmazingFeature'`)
4. 推送到分支 (`git push origin feature/AmazingFeature`)
5. 开启 Pull Request

## 联系方式

- 项目主页：[GitHub Repository]
- 问题反馈：[Issues]
- 邮件联系：[1902802324@qq.com]

## 致谢

感谢以下开源项目：

- [Qt Framework](https://www.qt.io/)
- [FreeRDP](https://www.freerdp.com/)
- [CMake](https://cmake.org/)
- [Inno Setup](https://jrsoftware.org/)

---

**注意**: 本项目仅供学习和研究使用，请遵守相关法律法规和软件使用协议。
