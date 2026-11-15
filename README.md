# FlowCast 无感投屏

![license](https://img.shields.io/badge/License-GPLv3-blue.svg)

![stars](https://img.shields.io/github/stars/zwkit/FlowCast?style=social)

![release](https://img.shields.io/github/v/release/zwkit/FlowCast)

![build](https://img.shields.io/github/actions/workflow/status/zwkit/FlowCast/apk.yml?label=build)

**把安卓电视 / 安卓盒子变成一台 AirPlay 接收器，专门用来投 iPhone。**

FlowCast 是一款免费、开源、纯净无广告的投屏软件：装在电视上，iPhone 在相同 Wi-Fi 下点「屏幕镜像」即可把画面和声音投上去，无需在手机上安装任何 App，也不需要登录或会员。

它基于 [UxPlay](https://github.com/FDH2/UxPlay) 的 AirPlay/RAOP 协议实现，把 Android 设备变成兼容 AirPlay 的显示与扬声器，支持 iOS / iPadOS、macOS 以及其它发送端实现，是面向 Android 与 Android TV 的开源 AirPlay 接收端。

> [!WARNING]
> 受 DRM 保护的内容（例如 Apple TV App 里的影片）不支持投屏。

---

## 为什么用 FlowCast

- **专门投 iPhone**：手机不用装 App、不用注册账号，系统自带的「屏幕镜像」直接可用。
- **完全无广告**：没有开屏、贴片、信息流广告，也不含任何广告 / 统计 / 追踪 SDK，不上传使用数据。
- **扫码投送链接播放**：电视上显示一张二维码，iPhone 扫码打开局域网页面，粘贴一个视频直链或直播流地址即可让电视直接播放，还能在手机上暂停 / 继续 / 停止。
- **免费且开源**：GPLv3 许可，代码公开，可自行编译。

---

## 功能一览

### 投屏与镜像

- 屏幕镜像：H.264 / H.265（HEVC）视频解码
- 音频投送：AAC-ELD、AAC-LC、ALAC 解码
- 双设备同时投屏并排显示（50/50，间距随信号源宽高比；可在 设置 → 连接 关闭）
- 视频 / 音乐播放：HLS、远程播放控制、曲目信息与封面
- 画中画、自动分辨率与模式切换
- 分辨率、过扫描、帧率控制
- 音频延迟调节与软件解码回退
- 音画同步：以音频为主时钟，支持手动 ±500 ms 微调
- 发送端静音时自动切断残留音频，声音随画面一起停
- 实时调试浮层（帧率、码率、编码、分辨率、帧数、音量等）
- Android 原生媒体会话与通知栏控制
- 可选 PIN 码认证
- 通过 USB 网络共享，把 Android 设备作为 macOS / iPadOS 的有线副屏
- 适配 Android TV：方向键导航、遥控快进快退

### 扫码投送链接（Web Push）

在接收端主界面会显示一个二维码。用 iPhone 扫码后会打开一个局域网页面：

1. 粘贴视频地址（支持 `http(s)` 直链、`rtsp`、`rtmp(s)`、`udp`、`rtp` 流）；
2. 点「播放」，电视立即开始播放；
3. 页面上可暂停 / 继续 / 停止，并实时显示播放进度。

安全说明：二维码里带有**每次启动服务都会重新生成的随机令牌**，所有请求都必须带上它；旧截图里的二维码会失效。页面仅限局域网访问，链接校验在电视端统一完成，不支持 `file://` 等本地路径，避免暴露电视上的文件。

### 附带：Windows 投屏发送端

仓库里的 [`windows-client/`](windows-client) 附带一个 Windows 端发送程序源码，可把 PC 桌面与系统声音作为 AirPlay 信号源投到 FlowCast 接收端（单个 `flowcast.exe`，无需驱动、无需安装）。详见 [windows-client/README.md](windows-client/README.md)。

---

## 兼容性

| 项目 | 要求 |
| --- | --- |
| 接收端 | Android 7.0（API 24）及以上，含 Android TV / 电视盒子 |
| 发送端 | 同一局域网内的 iPhone / iPad（iOS、iPadOS）、macOS，或其它 AirPlay 发送端 |
| 网络 | 设备在同一子网，允许 mDNS / 组播 |
| PC 发送端 | Windows（见 `windows-client/`） |

---

## 安装与使用

1. 在 [Releases](https://github.com/zwkit/FlowCast/releases/latest) 下载对应 ABI 的 release APK（`arm64-v8a` / `armeabi-v7a` / `x86_64` 或通用包），安装到电视 / 盒子。
2. 打开 FlowCast，授予所需权限，保持接收端处于前台服务运行状态。
3. iPhone 与电视连到同一个 Wi-Fi，控制中心 → 「屏幕镜像」→ 选择电视名称即可投屏。
4. 扫码投送链接：在接收端界面用手机扫描二维码，按页面提示粘贴视频地址并播放。

> 提示：部分设备默认 USB 网络共享为 RNDIS，若要用作有线副屏，可在 ADB 下执行 `adb shell settings put global tether_force_usb_functions 1` 切换到 CDC-NCM。

---

## 自行构建

工具链：JDK 21、Android SDK `compileSdk 37`、NDK `27.0.12077973`。所有原生依赖都随仓库分发（已打补丁的 UxPlay、libplist、OpenSSL、FFmpeg 预编译静态库），**不需要 git submodule**。

```bash
./gradlew assembleDebug          # 调试包
./gradlew :app:testDebugUnitTest # 单元测试
```

Release 签名通过 [`app/build.gradle.kts`](app/build.gradle.kts) 配置：密码来自环境变量 `FLOWCAST_KEYSTORE_PASSWORD` / `FLOWCAST_KEY_PASSWORD` 或 `local.properties`。`local.properties`、`*.keystore` 等已被 [.gitignore](.gitignore) 排除，**请勿提交密钥或密码**。

开发者向的实现细节、ADB 配置、音画同步机制、补丁与再构建脚本说明见 [docs/TECHNICAL.md](docs/TECHNICAL.md)。持续集成与可复现构建见 [.github/workflows/apk.yml](.github/workflows/apk.yml)。

---

## 目录结构

```
app/                    Android 接收端（Kotlin + C/C++ JNI）
app/src/main/cpp/        UxPlay 协议核心、JNI 桥接、vendored 原生依赖与补丁
app/libs/               离线构建用的两个 jar（二维码 ZXing、media3 RTSP）
windows-client/         Windows 投屏发送端源码
tools/                  FFmpeg / OpenSSL 再构建脚本、补丁工具、测试
docs/TECHNICAL.md       开发者技术说明
fastlane/               应用商店上架元数据
.github/workflows/      自动构建与发布
```

---

## 致谢

- [UxPlay](https://github.com/FDH2/UxPlay) —— AirPlay / RAOP 服务端实现
- [FFmpeg](https://ffmpeg.org) —— 无损音频解码
- [Next Player](https://github.com/anilbeesetti/nextplayer) —— 视频播放器参考
- [jqssun/android-airplay-server](https://github.com/jqssun/android-airplay-server) —— 本项目的前身

本项目是 [jqssun/android-airplay-server](https://github.com/jqssun/android-airplay-server) 的品牌重命名发行版：应用 ID 改为 `io.github.zwkit.flowcast`，Kotlin 命名空间因 JNI 符号耦合保持不变。

---

许可：[GPLv3](LICENSE) · 本项目与 Apple Inc. 无任何关联。
