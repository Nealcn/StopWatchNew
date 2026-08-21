# M5StopWatch 固件改造进度记录

> 最后更新：2026-08-20（暂停时记录）
> 仓库：`D:\StopWatchNew\Stackchan-Newstep`（xiaozhi-esp32 本地副本，原 Stack-chan「土豆」）
> 目标：把固件改造成 M5StopWatch 专用固件，去掉设备交互（晃动/摸头）与红外功能，并精简存储

---

## 一、总体目标（用户确认）

1. 板级支持切换到 **M5StopWatch**（ESP32-S3 / 466×466 圆形 CO5300 AMOLED / ES8311 音频 / M5PM1 PMIC / M5IOE1 IO 扩展 / 2 物理按键 / CST820B 触摸——**不用**）
2. **删除** m5stack-core-s3 旧板（含伺服、摄像头、触摸、BMI270 晃动/举起检测、摸头、红外全部代码）
3. **删除** 红外（IR）模块
4. 存储瘦身：删除冗余、清理死代码
5. 用户决策记录：
   - 旧板：**直接删除**（非保留剥离）
   - 触摸屏：**仅用物理按键**（不初始化 CST820B）
   - 中文字体：**保持 20px**（`font_puhui_basic_20_4`），但**字幕位置上移**
   - 死代码：**清理**（emote + image_effects + new_jpeg）

---

## 二、已完成 ✅

### 阶段 1：板级移植（已烧录验证，设备正常启动）

**新增 `main/boards/m5stack-stopwatch/`**（4 文件，从上游 github.com/78/xiaozhi-esp32 的 `main/boards/m5stack/stopwatch/` 移植）：
- `m5stack_stopwatch.cc`（327 行，上游原样）——`M5StackStopwatchBoard : WifiBoard`：
  - `RoundLcdDisplay : SpiLcdDisplay`：圆形 UI（圆角掩码 `rounder_event_cb`、状态栏上移、字幕避开底部圆弧）
  - `StopwatchBacklight`：CO5300 命令 0x51 调亮度
  - 初始化序列：M5IOE1（0x4F）→ M5PM1（0x6E，Charge/Boost 使能）→ LCD/CODEC/PA/MOTOR 引脚 → QSPI 屏 → ES8311 → 按键
  - 按键1（GPIO2）：唤醒/切换对话（未联网时配网）；按键2（GPIO1）：音量 0→100→0
  - 电池：M5PM1 `readVbat` + 充电状态（3400–4200mV → 0–100%）
  - 无 IMU、无触摸初始化、无 IR、无摄像头 —— 天然符合要求
- `config.h`（65 行，上游原样 + 一处本地修改，见阶段 3）
- `config.json`（本地新建，扁平格式无 manufacturer 键）
- `README.md`（上游原样）

**删除**：
- `main/boards/m5stack-core-s3/`（14 文件：m5stack_core_s3.cc 2821 行、cores3_audio_codec、SCS 伺服库、ir_remote_screen、config 等）
- `main/ir/`（11 文件：ir_driver/ir_codec/ir_codec_nec/ir_store/ir_service + 头文件）

**构建接线**：
- `main/Kconfig.projbuild`：`BOARD_TYPE_M5STACK_CORE_S3` → `BOARD_TYPE_M5STACK_STOPWATCH`（bool "M5Stack Stopwatch"，depends on IDF_TARGET_ESP32S3）
- `main/CMakeLists.txt`：
  - 删 IR 源文件 5 条 + INCLUDE_DIRS 里的 `"ir"`
  - 板分支：`CONFIG_BOARD_TYPE_M5STACK_STOPWATCH` → `BOARD_TYPE "m5stack-stopwatch"`，字体 `font_puhui_basic_20_4` + `font_awesome_20_4` + `twemoji_64`（tab5 同款，本地字体资源已验证存在）
- `main/idf_component.yml`：
  - 删 `espressif/bmi270_sensor`（仅 core-s3 用）
  - 加 `m5stack/m5ioe1: ^1.0.8`、`m5stack/m5pm1: ^1.0.5`（组件注册表已下载，`managed_components/m5stack__m5ioe1|m5pm1`）
- `sdkconfig.defaults`：板型改 STOPWATCH、SPIRAM 改 OCT；删 GC0308 相机 3 行
- `sdkconfig.defaults.esp32s3`：SPIRAM_MODE_QUAD → OCT（此文件优先于 defaults）
- 删除旧 `sdkconfig` 后由 idf.py 重新生成（STOPWATCH=y、OCT=y、80M=y、自定义唤醒词保留）
- `build.bat` / `flash.bat`：`cd /d D:\Stackchan`（过期路径）→ `cd /d %~dp0`
- `main/application.cc:1081`：注释里过时的「SI12T/Motion 任务」措辞 →「外部任务」

**首次构建/烧录**（COM8，esp32s3）：
- 第一版 assets 超分区（30px 字体嵌入 2.5MB → 6.26MB > 4MB assets 分区）→ 用户选 20px + 分区重排
- 分区表 v1 第一次重排：ota 3.5MB×2 + assets 5.06MB（`partitions/v1/16m_stackchan.csv`）
- 全量烧录成功，**启动日志验证通过**：M5IOE1@0x4F、M5PM1@0x6E、LVGL、ES8311 Slave、WiFi 正常，无错误无崩溃

### 阶段 2：表情渲染修复（已烧录）

**问题**：表情图显示不出来。根因（已完整定位）：
- 打包：`scripts/build_default_assets.py` 把 `twemoji_64` 的 **PNG 原样**拷入 assets（mmap 格式，无转换）
- 运行时：`assets.cc:277` 用 `LvglRawImage` 包装 PNG 数据，其构造把 `header.cf` 设成 `LV_COLOR_FORMAT_RAW_ALPHA`、`w=0 h=0`
- LVGL 9.5 解码流程：`lv_bin_decoder`（注册最早）对 VARIABLE 源**只认 header**（cf 非 UNKNOWN 就认领），RAW_ALPHA + w=0 → 画出 0×0 → 不可见；lodepng 永远轮不到（GIF 走 `LvglGif` 的 `gd_open_gif_data` 直接解数据所以正常）
- 上游 xiaozhi-esp32 用 GIF 集合（noto-emoji）所以没暴露；本地 fork 用 PNG 集合（twemoji_64）→ 必现

**修复**（`main/display/lvgl_display/lvgl_image.cc` `LvglRawImage` 构造）：
- 删除 `cf=RAW_ALPHA` / `w=0` / `h=0` 三行，**保持 cf=UNKNOWN** → bin 解码器拒绝认领 → lodepng 按 PNG 魔数认领并正确解码（w/h 在 open 时由解码器链重新推导，预填 header 会被清零重算）
- 加注释说明原因
- 已重建、仅烧 app 分区（0x410000，旧布局），hash 校验通过

### 阶段 3：存储盘点与瘦身（进行中，见第三节）

**盘点结论（16MB 总容量）**：

| 项目 | 占用 | 结论 |
|---|---|---|
| 唤醒词模型 srmodels.bin（打包在 assets 内） | 3.99MB | 必需（土豆土豆） |
| **model 分区里的重复拷贝** @0x10000 | **4MB** | **纯冗余**！运行时只从 assets 加载（`assets.cc:91` → `srmodel_load`），分区那份从未读取（`esp_srmodel_init("model")` 仅是 assets 加载失败时的兜底） |
| assets 中文字体（20px 完整版） | 1.24MB | 必需 |
| assets 表情 PNG（21 个） | 88KB | 必需 |
| app 固件 | 2.77MB | 死代码：emote 119KB + image_effects 51KB + new_jpeg 25KB |
| OTA 双槽 | 3.5MB×2 | 必需 |

**已完成的瘦身修改**：
1. **删除 model 分区**（省 4MB）——`partitions/v1/16m_stackchan.csv` 新布局：
   ```
   nvs 0x9000, otadata 0xd000, phy 0xf000
   ota_0 0x10000 0x480000 (4.5MB)
   ota_1 0x490000 0x480000 (4.5MB)
   assets 0x910000 0x6F0000 (7MB)
   ```
   注意：删分区后 flash 命令不再烧 srmodels（模型只在 assets 里），**第一次需 USB 全量烧录新分区表**
2. **删除 emote 3D 表情系统**（-119KB + 连带组件）：
   - `main/assets.cc`：删 `#include "emote_display.h"`/`"expression_emote.h"`（6-7 行）+ `Assets::EmoteStrategy` 4 个方法（原 359-424 行；EmoteStrategy 在 `#if HAVE_LVGL` 之外无条件编译，属死代码——实际运行走 LvglStrategy，`HAVE_LVGL` 定义在 `display/display.h:7`）
   - `main/assets.h`：删 `EmoteStrategy` 类声明
   - `main/CMakeLists.txt`：删 `display/emote_display.cc` 源
   - `main/idf_component.yml`：删 `espressif2022/image_player`、`espressif2022/esp_emote_expression`
   - 删除文件 `main/display/emote_display.cc/.h`
3. **字幕位置上移**：`main/boards/m5stack-stopwatch/config.h` 的 `DISPLAY_CHAT_BAR_BOTTOM_OFF` 66 → **90**（20px 文本底部不再被圆弧裁剪：圆在文本边缘 x=±175 处下缘 y=387，文本底 y=376）
4. **jpeg 编码链清理**（-76KB，进行中，见下）：

---

## 三、当前状态：⏸ 暂停中（构建失败，待修复）

**正在进行**：删除 JPEG 编码链（`esp_image_effects` 51KB + `esp_new_jpeg` 25KB），已做修改：
- `main/mcp_server.cc`：删 `self.screen.snapshot` 工具（原 189-242 行，`#if CONFIG_LV_USE_SNAPSHOT` 块内；**保留** `self.screen.preview_image` 工具——它走解码路径不依赖编码器）
- `main/display/lvgl_display/lvgl_display.cc`：删 `SnapshotToJpeg()` 方法（原 234-274 行）+ `#include "jpg/image_to_jpeg.h"`
- `main/display/lvgl_display/lvgl_display.h`：删 `virtual bool SnapshotToJpeg(...)` 声明
- `main/CMakeLists.txt`：删 `display/lvgl_display/jpg/image_to_jpeg.cpp`（2 处）+ `display/lvgl_display/jpg/jpeg_to_image.c`（2 处）
- `main/idf_component.yml`：删 `espressif/esp_image_effects`、`espressif/esp_new_jpeg`

**❌ 当前构建失败**（`idf_build6.log`，BUILD_EXIT=2）：
```
main/boards/common/esp_video.cc:13:10: fatal error: esp_imgfx_color_convert.h: No such file or directory
```
原因：`esp_video.cc` 编译时引用已删除的 esp_image_effects 头文件。它（连同 `esp32_camera.cc`、`rndis_board.cc`）在 CMakeLists 里按目标无条件编译，但**从未被链接**（stopwatch 无相机/USB 网络，symbols 无引用）——属于死代码。

**✅ 已于 2026-08-21 完成（用户指示暂不烧录）**：
1. `main/CMakeLists.txt` 删除了 esp_video/rndis_board/esp32_camera 三个编译块
2. 附加清理（镜像未链接的同类死代码）：
   - 删除文件：`boards/common/esp32_camera.cc/.h`、`esp_video.cc/.h`、`rndis_board.cc/.h`、`display/lvgl_display/jpg/`（image_to_jpeg.cpp/.h、jpeg_to_image.c/.h）、`sd/` 整个目录（sd_card/sd_photo）
   - `main/CMakeLists.txt`：删 `sd/sd_card.cc`、`sd/sd_photo.cc` 源条目；INCLUDE_DIRS 删 `"sd"` 和 `"display/lvgl_display/jpg"`
   - `main/assets.cc`：`#else`（无 LVGL）分支里的 `EmoteStrategy` 引用改为 `strategy_ = nullptr`（否则 HAVE_LVGL 关闭时编译失败）
   - 保留 `boards/common/camera.h`（`board.h` 接口需要）
3. 重新构建：**BUILD_EXIT=0**，死代码符号零残留（emote/image_effects/new_jpeg 均不在镜像）
4. 最终 app：**2,685,040 字节（2.56MB）**，对比起点 2,770,240 减 **84KB**；libmain 196,288 → 190,206

**⏳ 剩余（待用户安排烧录）**：全量烧录（分区表已变！新命令含：bootloader/partition-table/ota_data/xiaozhi@0x10000/assets@0x910000，不再有 srmodels）→ 启动日志验证

---

## 三.5、土豆脸表情系统移植（2026-08-21，已编译未烧录）

**背景**：用户要求显示原版 Stack-chan 土豆脸（旧板 M5StackAvatarDisplay），而非 emoji PNG。
**源码找回**：旧板源码在 `D:\StackchanNew\Stackchan-Newstep\main\boards\m5stack-core-s3\m5stack_core_s3.cc`（2821 行，与删除的本地版一致，含 `shizhou_avatar::LvglAvatar` 396-1026 行原版实现）。
**移植内容**（新建 `main/boards/m5stack-stopwatch/potato_face.h`，768 行）：
- `shizhou_avatar::LvglAvatar` 原样移植（LVGL canvas 绘制土豆脸：眨眼/呼吸/注视扫视/说话张嘴动画，21 种表情 + Overlay 装饰）
  - 唯一修改：canvas align `TOP_LEFT` → `CENTER`（watch 居中显示）
- `RoundLcdDisplay` 从 m5stack_stopwatch.cc 迁入（保持自包含）
- 新增 `PotatoFaceDisplay : RoundLcdDisplay`：
  - SetupUI：隐藏 emoji 盒 + 创建土豆脸画布（屏幕 55% = 256px，居中，避开顶部状态栏与底部字幕）
  - SetEmotion：MapEmotion + OverlayFor（原版映射表）
  - SetChatMessage：assistant 消息触发说话张嘴动画（按字数 120ms/字，800ms-15s）
  - 去掉了 servo/face_tracker/灯联动（stopwatch 无这些硬件）
- `m5stack_stopwatch.cc`：RoundLcdDisplay → PotatoFaceDisplay（删本地类定义，include potato_face.h）
**构建**：BUILD_EXIT=0，xiaozhi.bin 2,693,216（+8.2KB）
**状态**：⏸ 已编译，**未烧录**（用户指示改完先不烧录）

## 四、遗留问题 / 备注

1. **OTA 服务器**：分区表变化后首次必须 USB 烧录；之后 OTA 走 esp_ota 按分区标签写入，无影响。旧 `partitions/v1/16m_stackchan.csv` 注释已更新
2. **`esp_srmodel_init("model")` 兜底失效**：model 分区删除后，assets 加载失败时无兜底——可接受（assets 是字体/表情/模型的共同来源，坏了应用本身也无法工作）
3. **字体**：保持 20px（用户决定）。若日后想升 30px（assets 内容 +1.26MB），7MB assets 分区放得下，只需改 CMakeLists 两行
4. **唤醒词**：保留自定义「土豆土豆」（CONFIG_USE_CUSTOM_WAKE_WORD=y）。若改用 AFE 唤醒词（上游 stopwatch 配置），模型可缩到 ~1MB，再省 3MB
5. **LV_USE_SNAPSHOT**：LVGL 截图能力保留开启（无害），只是删了「截图上传」工具（`self.screen.snapshot`）；`self.screen.preview_image` 保留（走解码路径，不依赖被删组件）
6. **恢复方法**：删掉的功能如需恢复，从上游 xiaozhi-esp32 拉回对应代码即可（仓库非 git，无版本历史，本 md 为唯一变更记录）
7. **CMakeLists 里 `EMOTE_RESOLUTION` 变量与其他板分支的 emote 引用**：保留未动（仅 ESP_VOCAT/ESP_BOX 等不可构建板型用，`CONFIG_FLASH_EXPRESSION_ASSETS` 分支不执行）

## 五、关键文件清单（当前状态）

| 文件 | 状态 |
|---|---|
| `main/boards/m5stack-stopwatch/` | 新增（potato_face.h 为本地移植，其余 4 文件来自上游 + config.h 本地修改） |
| `main/Kconfig.projbuild` | 板型已换 STOPWATCH |
| `main/CMakeLists.txt` | 已删 IR/emote/image_to_jpeg/jpeg_to_image/相机/视频/RNDIS/SD 源条目 |
| `main/idf_component.yml` | 已删 bmi270/image_player/esp_emote_expression/image_effects/new_jpeg，已加 m5ioe1/m5pm1 |
| `main/display/lvgl_display/lvgl_image.cc` | LvglRawImage 修复（cf 保持 UNKNOWN） |
| `main/display/lvgl_display/lvgl_display.cc/.h` | 已删 SnapshotToJpeg |
| `main/assets.cc/.h` | 已删 EmoteStrategy（#else 分支置 nullptr） |
| `main/mcp_server.cc` | 已删 snapshot 工具 |
| `main/application.cc` | 注释措辞更新 |
| `partitions/v1/16m_stackchan.csv` | 新布局（无 model 分区） |
| `sdkconfig.defaults` / `.esp32s3` | STOPWATCH + OCT |
| `build.bat` / `flash.bat` | 路径修复 |
| 已删除文件/目录 | `main/display/emote_display.cc/.h`、`main/ir/`、`main/boards/m5stack-core-s3/`、`main/sd/`、`main/boards/common/esp32_camera.*/esp_video.*/rndis_board.*`、`main/display/lvgl_display/jpg/` |
| 保留（接口需要） | `main/boards/common/camera.h`（board.h 引用） |

## 六、构建/烧录环境备忘

- IDF 环境在 `build.bat` 内（IDF_PATH=D:\esp-idf，工具链 D:\Espressif）；临时辅助脚本 `_idf_build_env.bat`（项目根，用完可删）供命令行调用
- 烧录串口：**COM8**（flash.bat 里原 COM6 已过时，需改）
- 构建命令：`python D:\esp-idf\tools\idf.py build`（cmd 环境，MSYSTEM 需清空）
- 大小查看：`idf.py size-components`
- 启动日志抓取：`esptool` 复位后读 COM8 115200
