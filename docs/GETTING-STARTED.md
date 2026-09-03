# Remapad 新手开发与上手指南

本文档指导开发者与协作 Agent 在本地搭建完整的 Remapad 开发环境，执行构建验证、本地实时仿真与固件烧录。

---

## 前置环境准备

在开始开发前，请确保电脑已安装以下工具链：

| 工具 / 环境 | 建议版本 | 用途说明 |
| :--- | :--- | :--- |
| **Bun** | v1.0 或更高版本（实测 v1.4 通过） | 前端开发服务器与打包脚本运行时 |
| **pnpm** | v9 或更高版本（优先推荐包管理器） | 工作区依赖安装与根目录任务调度 |
| **Python** | v3.10 或更高版本（实测 v3.13 通过） | ADR 架构决策工具及 ESP-IDF 构建脚本依赖 |
| **ESP-IDF** | v5.3 或更高版本 | 乐鑫官方芯片驱动、FreeRTOS 与硬件编译烧录套件 |
| **硬件设备** | ESP32-S3 (N16R8) + ST7789 屏幕 | 物理硬件板卡，配备 16MB Flash 与 8MB Octal PSRAM |

---

## 最短可运行步骤

### 步骤一：安装前端工程依赖
在项目根目录下执行：
```powershell
pnpm install
```

### 步骤二：启动浏览器模拟器（零刷机秒级开发）
启动本地 WebAssembly 仿真服务：
```powershell
cd ui ; bun run dev
```
打开浏览器访问 [http://127.0.0.1:8130](http://127.0.0.1:8130)，即可在带有 ESP32-S3 外观边框的模拟器中实时查看 240×280 屏幕渲染（60 FPS）。修改 [ui/src/App.jsx](../ui/src/App.jsx) 后保存，浏览器将自动热刷新。

### 步骤三：执行前端完整编译与资产导出
当完成界面调整后，执行全量编译：
```powershell
cd ui ; bun run build
```
构建器将自动完成 Tailwind 样式表光栅化、矢量字体烘焙、SVG 图像光栅化，输出 .pocket 包并全自动同步覆盖到 [firmware/main/app_pocket.h](../firmware/main/app_pocket.h)。

### 步骤四：固件工程编译与烧录
打开已载入 ESP-IDF 环境变量的终端（如从开始菜单打开 "ESP-IDF PowerShell"）：
```powershell
# 1. 进入固件目录
cd F:/private/remapad/firmware

# 2. 首次配置目标架构 (自动合并 sdkconfig.defaults)
idf.py set-target esp32s3

# 3. 编译完整固件
idf.py build

# 4. 连接开发板烧录并查看串口监控 (COMx 替换为实际端口)
idf.py -p COMx flash monitor
```

---

## 命令速查表

| 操作意图 | 执行命令 | 预期效果与产物 |
| :--- | :--- | :--- |
| **代码规范检查** | \`pnpm run lint\` | 使用 ESLint 检查全部源码风格规范 |
| **代码格式修复** | \`pnpm run lint:fix\` | 自动修复单引号、缩进、行尾空行等风格问题 |
| **启动开发服务器** | \`cd ui ; bun run dev\` | 监听 ui/src，启动 8130 端口模拟器并支持热重载 |
| **全量打包构建** | \`cd ui ; bun run build\` | 输出 app.js, app.pak, app.pocket 并同步 app_pocket.h |
| **新建架构决策** | \`python scripts/create_adr.py ...\` | 在 docs/adr 下自动按四位序号生成新 ADR |
| **清除编译缓存** | \`cd firmware ; idf.py fullclean\` | 清除 ESP-IDF 的 build 目录与临时对象缓存 |

---

## 关键文件快速索引

- **UI 根组件**：[ui/src/App.jsx](../ui/src/App.jsx)（页面整体布局、组件组合与交互逻辑）
- **UI 挂载入口**：[ui/src/index.jsx](../ui/src/index.jsx)（注入 prelude 虚拟 DOM 门面并挂载根组件）
- **前端打包器**：[ui/scripts/build.mjs](../ui/scripts/build.mjs)（Tailwind 编译、字体烘焙与资产打包主管道）
- **开发模拟器服务器**：[ui/scripts/dev.mjs](../ui/scripts/dev.mjs)（WebAssembly 宿主服务器、触控队列与 SSE 热重载）
- **固件主入口**：[firmware/main/main.c](../firmware/main/main.c)（硬件自检、PSRAM 校验与 PCKT 包魔数校验）
- **硬件配置预设**：[firmware/sdkconfig.defaults](../firmware/sdkconfig.defaults)（N16R8 专属 16MB Flash 与 8MB Octal PSRAM 参数）
- **Flash 分区表**：[firmware/partitions.csv](../firmware/partitions.csv)（16MB Flash 分区规划）

---

## 常见开发任务

### 添加新的文本并指定字号
在 PocketJS 体系中，字号必须映射为 Tailwind 插槽：
- \`text-xs\`：12px
- \`text-sm\`：14px
- \`text-base\`：16px
- \`text-lg\`：18px
- \`text-xl\`：20px
- \`text-2xl\`：24px
代码保存后，构建器会自动收集文本中出现的所有新字符编码，并在烘焙阶段自动生成对应插槽的点阵图集。

### 添加新的图片素材
1. 将图片（PNG 格式或 SVG 格式）放入 [ui/src/assets/images/](../ui/src/assets/images/) 目录中（如 \`icon.png\`）。
2. 在组件中直接使用：\`<Image class="w-8 h-8" src="icon.png" />\`。
3. 执行 \`pnpm run build\`，构建器会自动解析 PNG/SVG，生成纹理并打包入 \`app.pak\`。

---

## 常见故障排查

### 模拟器启动报端口占用 (\`EADDRINUSE 127.0.0.1:8130\`)
- **原因**：先前的 \`dev.mjs\` 进程在后台未被正常关闭。
- **排查与解决**：
  在 PowerShell 中查找并终止占用 8130 端口的进程：
  ```powershell
  Get-NetTCPConnection -LocalPort 8130 -State Listen | Select-Object OwningProcess
  Stop-Process -Id <PID> -Force
  ```

### 界面元素正常显示但文字全空
- **原因**：未加载字模图集（Font Atlas）或组件中直接使用了不受支持的任意内联 \`fontSize\`。
- **解决**：确保挂载时传入了包含字模的 \`app.pak\`，且文字字号严格使用 Tailwind 类名。

### ESP32-S3 启动日志输出 PSRAM 警告
- **现象**：串口监控输出 \`未检测到 PSRAM 或 PSRAM 初始化失败\`。
- **排查**：确认板载芯片丝印是否为 **ESP32-S3 N16R8**（Octal PSRAM）。若使用的是 N16R2 或 N8R2（Quad PSRAM），需进入固件目录运行 \`idf.py menuconfig\`，将 SPIRAM 模式由 \`Octal\` 修改为 \`Quad\`。
