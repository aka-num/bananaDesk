# bananaDesk

[简体中文](#zh-cn) · [English](#english) · [下载 / Downloads](https://github.com/aka-num/bananaDesk/releases)

<a id="english"></a>

## English


bananaDesk is a LAN-based remote desktop project for Windows and Linux that allows users to easily connect to Linux graphical desktops from their familiar computers to perform daily tasks.

While Linux does feature graphical desktops, options that offer both solid support and a good user experience are currently limited. Services like Sunlogin (Xiangrikui), for instance, often involve waiting in queues or failing to establish timely connections when using the free tier.

bananaDesk makes it convenient for individuals to manage their computers across Windows and Linux environments.

### Current scope

**The project currently supports remote control on a local network only. Devices must be able to establish a direct network connection. Public-network NAT traversal and relay services are not implemented.**

- Desktop viewing, keyboard input, and mouse control between Windows and Linux.
- A separate, resizable control window, with a fullscreen mode that shows only the remote desktop.
- Automatic two-way plain-text clipboard synchronization.
- Persistent connection codes with manual reset: reuse a code after restarting sharing under the same OS user, IP address and port; reset invalidates the old code.
- File uploads and downloads, plus files actively sent by the computer being controlled.

The Linux computer being controlled currently requires **X11**; Wayland is not supported yet. Windows packages target Intel/AMD 64-bit devices. The current Linux package is built for Ubuntu 22.04/X11 and is not a universal installer for every distribution. The project is still evolving, and compatibility and performance need more validation across real devices.

### Getting started

1. Download the package for your system from [Releases](https://github.com/aka-num/bananaDesk/releases) and extract it completely. On Windows, run `bananaDesk.exe`. On Linux, use the included `launch.sh` and ensure the required system dependencies are available.
2. On the computer you want to control, open “共享本机” (Share this computer) and click “开始共享” (Start sharing). On the controlling computer, open “连接远程桌面” (Connect to a remote desktop), paste the connection code, and connect. Share connection codes only with people you trust. These English labels explain the current Chinese UI; they do not indicate an English interface.

### Possible future direction

Future work may explore access across different networks through NAT traversal and a relay when direct connections are unavailable. This is a possible direction, not an implemented feature, and there is no committed release schedule.

---
<a id="zh-cn"></a>

## 中文



bananaDesk 是一个面向 Windows 和 Linux 的局域网远程桌面项目，能方便地从熟悉的电脑连接 Linux 图形桌面，完成日常操作。

Linux 本身有图形桌面，但目前，Linux 支持和使用体验都合适的选择比较有限。使用向日葵免费服务时，经常需要排队、无法及时连接的情况。

bananaDesk 方便个人在 Windows 和 Linux 之间操作自己的电脑。

### 当前支持的范围

**目前仅支持内网／局域网远控，需要设备之间能够直接建立网络连接。公网穿透和中继尚未实现。**

- Windows 与 Linux 之间的桌面查看、键盘和鼠标控制。
- 独立控制窗口，可调整大小，也可切换到只显示远程画面的全屏模式。
- 自动双向纯文本剪贴板同步。
- 固定共享码与手动重置：同一系统用户、IP 和端口下，重新开启共享可继续用原码；重置使旧码失效。
- 文件上传、下载，以及被控端主动选择文件发送给控制端。

Linux 被控端目前需要 **X11**，暂不支持 Wayland。Windows 发布包面向 Intel／AMD 64 位设备，Linux 发布包目前针对 Ubuntu 22.04／X11 构建，并非适用于所有发行版的通用安装包。项目仍在迭代，跨设备兼容性和性能还需要更多实际使用验证。

### 如何开始

1. 从 [Releases](https://github.com/aka-num/bananaDesk/releases) 下载适合系统的运行包并完整解压。Windows 运行 `bananaDesk.exe`；Linux 使用包内的 `launch.sh`，并确保系统依赖已满足。
2. 在被控电脑打开“共享本机”，点击“开始共享”；在控制电脑打开“连接远程桌面”，粘贴连接码并连接。只向你信任的人提供连接码。

### 后续可能做什么

后续可能探索公网访问，例如 NAT 穿透，以及无法直连时的中继，让不同网络中的设备也能连接。这是可能的后续方向，目前尚未实现，也没有确定的发布时间表。




