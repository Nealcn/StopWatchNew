# StopWatchNew: M5StopWatch 智能手表固件

基于 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 的 M5Stack **StopWatch** 手表固件（ESP32-S3 + 466×466 圆形 AMOLED）。保留土豆脸表情系统与云端 AI 对话，并在此基础上重构为**多功能手表系统**：主页面 Launcher + 语音转文字（VoiceCube 兼容）+ 表盘 + 设置。

## 硬件

| 部件 | 型号 |
|---|---|
| SoC | ESP32-S3（8MB PSRAM，16MB Flash） |
| 屏幕 | CO5300 466×466 圆形 AMOLED（QSPI） |
| 音频 | ES8311 编解码 + 麦克风/喇叭 |
| 触摸 | CST820B 电容触摸 |
| 按键 | 按键 1 / 按键 2（侧边） |
| 电源 | M5PM1 PMIC + M5IOE1 IO 扩展 |

## 功能一览

### 主页面（Launcher）

- 开机显示主页面，4 个功能图标：**AI 对话 / 语音转文字 / 表盘 / 设置**
- 按键 1/2 切换功能，**触摸点击图标**进入
- **A+B 双键同时按**随时返回主页面

### AI 对话（土豆脸）

- 土豆脸表情系统：21 种表情（开心/生气/思考/害羞…）+ 叠加效果（脸红/爱心眼/气泡…）
- **触摸屏幕随机表情反馈**：每次触摸随机换一个表情，2 秒后恢复
- 小智云端流式对话（ASR/LLM/TTS），唤醒词「土豆土豆」
- 对话字幕 + 状态栏（WiFi/电量）

### 语音转文字（VoiceCube 兼容）

- **按住按键 1 说话** → 环形点波动画（中心圆 + 12 环绕圆点随音量波动）
- 音频经 BLE 发送到桌面端 → 云端 ASR → 识别结果**屏幕预览** → 确认后粘贴到当前窗口
- 状态颜色：已连接=绿 / 录音中=红 / 识别中=黄 / 结果=蓝
- **按键 2** 清除当前录音
- 桌面端见 [desktop/voicestick](desktop/voicestick)（多设备切换、点击文字编辑）

### 表盘

- 4 种表盘样式：经典指针 / 简约 / 大数字 / 数字流
- 按键 1 上一个 / 按键 2 下一个，1 秒自动刷新

### 设置

- 设备：亮度 / 音量 / WiFi 配网
- 时间与日期：手动设置
- 系统：清除 AI 配置（恢复出厂联网状态）

## 交互速查

| 操作 | 效果 |
|---|---|
| 按键 1 / 2 | 主页面切换功能；页面内导航 |
| 触摸点击图标 | 进入功能 |
| A+B 同时按 | 返回主页面（任何页面） |
| 按住按键 1（语音页） | 录音 |
| 触摸屏幕（AI 对话页） | 随机表情反馈 |

## 编译 & 烧录

### 环境

- ESP-IDF v5.5.4（IDF_PATH=D:\esp-idf）
- Windows：`_idf_build_env.bat` 封装了环境变量（含 ESP_ROM_ELF_DIR）

### 编译

```bat
_idf_build_env.bat build
```

### 烧录

```bat
_idf_build_env.bat -p COM8 flash
```

（手表串口以实际为准，一般 COM8）

## 目录结构（本项目新增部分）

```
main/boards/m5stack-stopwatch/
├── m5stack_stopwatch.cc      # 板级初始化（显示/触摸/按键/A+B 组合键）
├── launcher.h                # 主页面（4 功能图标 + 位移动画）
├── potato_face.h             # 土豆脸表情系统 + 触摸随机表情
├── voice_input.h             # 语音转文字（BLE + 环形点波 UI）
├── watch_face.h + watch_face_view/   # 表盘（原版 app_watch_face）
├── setup.h + setup_view/ + setup_workers/  # 设置（原版 app_setup）
├── ble_voice.cc/h            # NimBLE 语音服务（VoiceCube 兼容）
└── cst820.cc/h               # CST820B 触摸驱动

desktop/voicestick/           # 语音转文字桌面端（PyQt5 + BLE + 云端 ASR）
```

## 文档

- [docs/stopwatch改造进度.md](docs/stopwatch改造进度.md) — 改造过程与排障记录（含 Launcher/触摸/语音输入/图标居中/字体等排障）
- 其余 docs/ 为历史开发文档（旧机器人项目遗留）

## License

基于 xiaozhi-esp32（MIT），其余见各组件 LICENSE。
