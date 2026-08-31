# StackChan 固件所有改动清单

> 最后更新: 2026-08-30
> 所有改动基于 `main` 分支

---

## 改动的文件

| 文件 | 改动次数 | 说明 |
|------|---------|------|
| `main/application.h` | 1 | 新增 `listening_start_time_` 成员 |
| `main/application.cc` | 4 | 聆听超时/状态切换/TTS 停止行为 |
| `main/boards/common/wifi_board.h` | 2 | 新增 WiFi 保活定时器 + 连接状态标志 |
| `main/boards/common/wifi_board.cc` | 5 | WiFi 保活创建/销毁/回调/事件触发 |
| `main/boards/m5stack-core-s3/m5stack_core_s3.cc` | 1 | Idle 也保持面部追踪 |

---

## 改动详情

### 1. 聆听超时 (application.cc + application.h)

**目的**: 聊完后聆听 20 秒，没人说话自动回待命

**改动文件**: `main/application.h` + `main/application.cc`

**改动点**:
- `application.h` (152行): 新增 `int64_t listening_start_time_ = 0`
- `application.cc` (584-587行): TTS `stop` 处理中，`ManualStop` 模式改为进 `Listening` 并启动 20 秒计时，而非直接进 `Idle`
- `application.cc` (281-288行): 每 1 秒时钟节拍检查超时，20 秒无语音 → 回 `Idle`
- `application.cc` (794行): 用户说话/按按钮时重置计时器

**可能的冲突**: 如果有人改 TTS stop 处理逻辑（`ManualStop` 分支），会冲突。

---

### 2. Idle 也保持面部追踪 (m5stack_core_s3.cc)

**目的**: 待命时设备也能转头跟随

**改动文件**: `main/boards/m5stack-core-s3/m5stack_core_s3.cc`

**改动点**:
- 状态为 `Idle` 时，`face_tracker_->Resume()` 而非 `Pause()`
- `RestoreFaceTracker()` 始终 Resume 而非根据状态判断

**注意**: 这个改动和上一条"聆听超时"有**逻辑关联**——Idle 时面部追踪开着，但 20 秒超时后回 Idle 时面部追踪也保持。

---

### 3. WiFi 保活 (wifi_board.h + wifi_board.cc)

**目的**: 空闲时 WiFi 掉线后自动静默重连，保持推送通道

**改动文件**: `main/boards/common/wifi_board.h` + `wifi_board.cc`

**改动点**:
- `wifi_board.h` (13行): 新增 `wifi_keepalive_timer_` 定时器
- `wifi_board.h` (14行): 新增 `wifi_connected_` 标志
- `wifi_board.h` (43-47行): 新增 `OnWifiKeepalive` 回调声明
- `wifi_board.cc` (48-57行): 构造函数创建 keepalive 定时器
- `wifi_board.cc` (69-73行): 析构函数销毁
- `wifi_board.cc` (140-147行): `OnWifiKeepalive` 回调实现——检查 `wifi_connected_`，断线则 `TryWifiConnect()`
- `wifi_board.cc` (163行): `Connected` 事件 → 设 `wifi_connected_ = true`，启动 60s 周期定时器
- `wifi_board.cc` (177行): `Disconnected` 事件 → 设 `wifi_connected_ = false`

**注意**: 这个改动是独立的，不与其他冲突。

---

## 状态机切换一览

```
Starting
  ├→ WifiConfiguring ──→ Activating ──→ Upgrading ──→ Idle
  └→ Activating (跳过 WiFi 配置)

Idle (待命)
  ├→ Connecting → Listening (成功) → Speaking (识别到语音)
  │             └→ Idle (失败)
  ├→ Speaking (主动推送)
  ├→ Listening (按按钮)
  ├→ Activating (重新激活)
  └→ Upgrading (OTA)

Listening (聆听)
  ├→ Speaking (用户说话)
  └→ Idle (20秒超时，无人说话) ← 新增改动

Speaking (播报)
  ├→ Listening (播完 → 继续聆听 20 秒) ← 改动: 原来直接回 Idle
  └→ Idle (被中断/取消)
```

---

## 依赖关系

```
聆听超时 (application.cc)
  └── 依赖: 20秒后回 Idle，Idle 时面部追踪保持 (m5stack_core_s3.cc)

WiFi 保活 (wifi_board.cc)
  └── 独立，不依赖其他改动

面部追踪 (m5stack_core_s3.cc)
  └── 关联聆听超时: Idle 时面部追踪一直开着
```

---

## 编译/烧录说明

```bash
# 在本地编译
cd StackChan 项目目录
idf.py build
# 烧录
idf.py -p /dev/ttyUSB0 flash monitor
```

所有改动已提交 (commit `f73296f`)，需要 cherry-pick 到本地分支。