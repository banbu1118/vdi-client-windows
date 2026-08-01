# VDI Client

A Qt6-based Virtual Desktop Infrastructure (VDI) client application that connects to virtual machines via RDP (Remote Desktop Protocol), providing users with remote desktop access functionality.

## Table of Contents

- [Project Overview](#project-overview)
- [Features](#features)
- [System Requirements](#system-requirements)
- [Project Structure](#project-structure)
- [Quick Start](#quick-start)
- [Build and Package](#build-and-package)
- [Usage Guide](#usage-guide)
- [Configuration Guide](#configuration-guide)
- [FAQ](#faq)
- [License](#license)
- [Contributing](#contributing)

## Project Overview

VDI Client is a modern desktop application designed for both enterprise and individual users, providing convenient virtual desktop access experience. Built on Qt6 framework, it uses FreeRDP as the RDP connection engine and supports multiple languages with rich feature sets.

### Technology Stack

- **Development Framework**: Qt 6.10.2
- **Programming Language**: C++17
- **Build Tool**: CMake 3.30.5
- **RDP Engine**: FreeRDP
- **Packaging Tool**: Inno Setup 6

## Features

### Core Features

- **RDP Connection**: Connect to virtual machines via RDP protocol
- **User Login**: Support username and password authentication
- **Connection Management**: Manage multiple virtual machine connections
- **Remote Desktop**: Complete remote desktop experience

### User Experience

- **Multi-language Support**: English, Simplified Chinese, Traditional Chinese, Japanese
- **Auto Login**: Support saving credentials for automatic login
- **Remember Password**: Securely store user passwords
- **Modern Interface**: Qt6-based modern user interface design
- **Responsive Layout**: Adapts to different screen sizes

### Advanced Features

- **Connection Template**: Customizable RDP connection parameters
- **Session Management**: Support concurrent multi-session connections
- **Auto Reconnect**: Automatic reconnection functionality
- **Logging**: Detailed connection and operation logs

## System Requirements

### Runtime Environment

- **Operating System**: Windows 10 or higher (Windows 11 recommended)
- **Architecture**: x64
- **Network**: Stable network connection required to access VDI server
- **Privileges**: May require administrator privileges to establish RDP connections

### Development Environment

- **Qt 6.10.2** (MinGW 64-bit)
- **CMake 3.30.5 or higher**
- **MinGW-w64** (installed with Qt)
- **Inno Setup 6** (for packaging)

## Project Structure

```
vdi-qt-bak-test/
├── bin/
│   ├── template.rdp           # RDP connection template file
│   └── wfreerdp.exe          # FreeRDP executable
├── resources/
│   ├── app.rc                 # Windows resource file
│   ├── app.ico                # Application icon
│   ├── logo.png               # Logo image
│   └── resources.qrc          # Qt resource file
├── src/
│   ├── main.cpp               # Application entry point
│   ├── loginwindow.cpp        # Login window implementation
│   └── loginwindow.h          # Login window header file
├── CMakeLists.txt             # CMake build configuration
├── build_and_package.bat      # Automated build script
├── installer.iss              # Inno Setup installer script
├── app.ico                    # Application icon
├── LICENSE                    # License file
├── README_ZH.md               # Chinese documentation
└── README_EN.md               # English documentation
```

## Quick Start

### Installing Application

1. Download the latest installer `VDIClient-Setup.exe`
2. Double-click to run the installer
3. Follow the installation wizard to complete installation
4. Launch the application and configure connection

### Basic Usage

1. Enter server address
2. Enter username and password
3. Click the "Connect" button
4. Wait for the connection to be established
5. Start using the remote desktop

## Build and Package

### Environment Setup

#### 1. Install Qt

Download URL: https://www.qt.io/download-qt-installer

**Installation Steps**:
- Run the Qt online installer
- Default installation (CMake and MinGW will be automatically installed during installation)

**Network Issue Solution**:
If installation fails due to network errors, you can use a domestic mirror:

```bash
qt-online-installer-windows-x64-4.10.0.exe --mirror https://mirrors.ustc.edu.cn/qtproject
```

#### 2. Install Inno Setup

Download URL: https://jrsoftware.org/isdl.php

**Installation Steps**:
- Download Inno Setup 6
- Run the installer
- Complete default installation

#### 4. Configure Environment Variables

Qt installation automatically installs MinGW. You need to configure the following environment variables:

**Add to system PATH**:
```
C:\Qt\Tools\mingw1310_64\bin
C:\Qt\6.10.2\mingw_64\bin
C:\Qt\Tools\CMake_64\bin
```

**Verify Installation**:
```bash
gcc --version
cmake --version
```

### Build Steps

Execute in the project root directory:

```bash
build_and_package.bat
```

The script will automatically complete the following steps:
1. Clean previous build files
2. Configure CMake project
3. Build the application
4. Deploy Qt dependencies
5. Generate installer package

**Output Location**:
- Installer: `installer_output/VDIClient-Setup.exe`

## Usage Guide

### Multi-language Switching

The application supports the following languages:
- English
- Simplified Chinese (简体中文)
- Traditional Chinese (繁體中文)
- Japanese (日本語)

Switching method: Select language option in the settings menu.

### Auto Login

1. Check the "Remember Password" option
2. Enter credentials and connect
3. Credentials will be automatically filled on next launch
4. Enable "Auto Login" for seamless connection

### Connection Issues

**Q: Unable to connect to virtual machine**

A: Check the following items:
- Whether network connection is normal
- Whether virtual machine address is correct (VDI server automatically retrieves virtual machine IP)
- Whether firewall allows RDP connection (default port 3389)
- Whether VDI server is accessible

## License

This project is licensed under the GNU Affero General Public License Version 3.0 (AGPL-3.0).

See the [LICENSE](LICENSE) file for details.

## Contributing

We welcome any form of contribution!

### Ways to Contribute

- Report bugs
- Propose new features
- Submit code improvements
- Improve documentation
- Translation work

### Development Workflow

1. Fork this repository
2. Create a feature branch (`git checkout -b feature/AmazingFeature`)
3. Commit your changes (`git commit -m 'Add some AmazingFeature'`)
4. Push to the branch (`git push origin feature/AmazingFeature`)
5. Open a Pull Request

## Contact

- Project Homepage: [GitHub Repository]
- Issue Tracker: [Issues]
- Email Contact: [1902802324@qq.com]

## Acknowledgments

Thanks to the following open source projects:

- [Qt Framework](https://www.qt.io/)
- [FreeRDP](https://www.freerdp.com/)
- [CMake](https://cmake.org/)
- [Inno Setup](https://jrsoftware.org/)

---

**Note**: This project is for learning and research purposes only. Please comply with relevant laws and regulations and software usage agreements.
