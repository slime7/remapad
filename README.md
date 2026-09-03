# Remapad ESP32-S3 N16R8 (PocketJS UI)

Remapad 是面向 **ESP32-S3 (N16R8)** 硬件规格构建的高性能嵌入式 UI 设备工程。项目采用前后端分离架构：
- **UI 前端**：基于 **PocketJS** 构建的高性能响应式 UI 工程（支持原生 JSX 语法）。
- **设备固件**：基于 **ESP-IDF** 的嵌入式工程，针对 16MB Flash 与 8MB Octal PSRAM 进行了硬件适配。

---

## 硬件规格

| 硬件项 | 参数说明 |
| :--- | :--- |
| **主控芯片 (MCU)** | ESP32-S3 (Xtensa 双核 32 位 LX7 处理器，最高频率 240MHz) |
| **片外 Flash** | 16MB (支持高频高速模式) |
| **片外 PSRAM** | 8MB Octal PSRAM (OPI 模式，用于渲染帧缓冲与 JS 运行内存) |
| **显示基准** | 240 × 280 分辨率，RGB565 像素格式 (如常见 ST7789 屏幕模组) |

---

## 项目目录结构

```text
remapad/
├── .editorconfig            # 代码格式化与缩进风格配置
├── .gitignore               # Git 忽略配置
├── README.md                # 项目指南与开发文档
├── package.json             # 根工作区脚本 (支持一键 pnpm run build)
├── pnpm-workspace.yaml      # pnpm monorepo 工作区配置
├── ui/                      # PocketJS 前端工程 (JSX, Node.js + pnpm)
│   ├── package.json         # 前端依赖与构建脚本配置
│   ├── pocket.json          # PocketJS 应用元数据与固件契约配置 (240x280)
│   ├── pocket.config.js     # PocketJS 显示与屏幕参数配置
│   ├── jsconfig.json        # JavaScript / JSX 语法提示配置
│   ├── scripts/
│   │   ├── build.mjs        # 前端构建打包器 (生成 app.js, app.pocket 与 C 头文件)
│   │   └── dev.mjs          # 本地文件监听与自编译开发脚本
│   ├── src/
│   │   ├── index.jsx        # UI 挂载入口 (mount)
│   │   └── App.jsx          # 界面根组件 (基于 Solid JSX 与 PocketJS 原语)
│   └── dist/
│       ├── app.js           # 编译后的 JavaScript IIFE bundle
│       ├── app.pocket       # 符合 PocketJS 官方规范的二进制包 (PCKT)
│       └── app_pocket.h     # 自动导出的 C 静态头文件 (包含字节数组与长度)
└── firmware/                # ESP-IDF 固件工程
    ├── CMakeLists.txt       # CMake 项目顶层构建配置
    ├── partitions.csv       # 16MB Flash 自定义分区表
    ├── sdkconfig.defaults   # N16R8 硬件预设配置 (Flash 16MB + PSRAM 8MB OPI)
    └── main/
        ├── CMakeLists.txt   # 主程序组件配置
        ├── idf_component.yml# ESP-IDF 组件依赖清单
        └── main.c           # 固件入口与硬件自检逻辑
```

---

## 环境准备

### 前端工具链（已全面适配 Node.js + pnpm）
- **Node.js**：使用 v18 或更高版本（实测 Node.js v24 通过）。
- **包管理器**：使用 **pnpm**（已配置 pnpm 工作区与完整依赖）。

### 嵌入式工具链
- **ESP-IDF**：建议使用 v5.4 或更高版本。
- **Python**：v3.10 及以上（由 ESP-IDF 安装工具统一管理）。
- **硬件驱动**：确保电脑已识别开发板的 USB 串口 (如 CH343, CP2102 或芯片内置 USB JTAG/Serial)。

---

## 开发、打包与烧录指南

### 前端 UI 开发与浏览器模拟器预览

进入 `ui` 目录执行依赖安装与构建：

```powershell
cd ui

# 安装项目依赖 (优先使用 pnpm)
pnpm install

# 启动浏览器模拟器与热重载开发服务 (访问 http://127.0.0.1:8130)
pnpm run dev

# 打包嵌入式端资源包与 C 语言头文件
pnpm run build
```

打包产物将自动生成于 `ui/dist/` 目录下。

### 固件配置与编译

打开 PowerShell 终端并载入 ESP-IDF 环境变量：

```powershell
# 载入 ESP-IDF 环境变量 (根据本地安装路径调整)
. C:\Espressif\frameworks\esp-idf\export.ps1

# 进入固件工程目录
cd firmware

# 首次配置：指定目标芯片并载入 sdkconfig.defaults 硬件预设
idf.py set-target esp32s3

# 编译固件工程
idf.py build
```

### 固件烧录与串口监视

确保开发板已连接电脑，查询实际分配的 COM 端口（如 `COM3`）：

```powershell
# 执行烧录并进入串口监视器
idf.py -p COM3 flash monitor
```

- **下载模式说明**：若下载时未能自动复位进入烧录模式，可按住板载 **BOOT** 键，短按一次 **RST/EN** 键后再松开 BOOT 键。
- **退出监视器**：使用快捷键 `Ctrl + ]`。

---

## 内存架构与分区策略

### 8MB Octal PSRAM 配置
在 `firmware/sdkconfig.defaults` 中已预设启用高性能 OPI 模式 PSRAM：
- `CONFIG_SPIRAM=y`
- `CONFIG_SPIRAM_MODE_OCT=y`
- `CONFIG_SPIRAM_SPEED_80M=y`
- `CONFIG_SPIRAM_USE_MALLOC=y`

系统启动时，`main.c` 会自动检测并打印当前 Octal PSRAM 的可用字节数。

### 16MB Flash 分区规划 (`partitions.csv`)
| 分区名称 | 类型 | 子类型 | 偏移地址 | 分区大小 | 用途说明 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **nvs** | data | nvs | `0x9000` | 24 KB | 存储系统非易失性键值数据与配置 |
| **phy_init** | data | phy | `0xf000` | 4 KB | 射频物理层校准数据 |
| **factory** | app | factory | `0x10000` | 4 MB | 核心固件应用程序 |
| **storage** | data | spiffs | `0x410000` | 约 11.8 MB | 文件系统分区，用于存储 PocketJS 静态资产与界面包 |

---

## 常见问题与排错

- **PSRAM 初始化失败**：
  检查芯片丝印是否为 N16R8（带有 Octal PSRAM）。若使用的是 Quad PSRAM (如 N8R2/N16R2)，需将 `CONFIG_SPIRAM_MODE_OCT` 调整为 Quad 模式。
- **Flash 空间溢出警告**：
  本工程已针对 16MB Flash 定制分区，确保在 `idf.py set-target esp32s3` 后检查 `sdkconfig` 中 `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` 处于开启状态。
