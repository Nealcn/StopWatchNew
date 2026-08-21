#include "wifi_board.h"
#include "backlight.h"
#include "display/lcd_display.h"
#include "esp_lcd_co5300.h"
#include "potato_face.h"
#include "launcher.h"
#include "voice_input.h"
#include "cst820.h"
#include "codecs/es8311_audio_codec.h"
#include "application.h"
#include "button.h"
#include "M5IOE1.h"
#include "M5PM1.h"
#include "config.h"
#include "assets/lang_config.h"
#include "protocols/protocol.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <wifi_manager.h>
#include <lvgl.h>

#define TAG "M5StackStopwatch"
#define LCD_OPCODE_WRITE_CMD (0x02ULL)

// 组合键（A+B 同时按住）返回主页面：两键按下间隔窗口与触发后屏蔽窗口
#define COMBO_WINDOW_US    (500 * 1000)
#define COMBO_SUPPRESS_US  (1000 * 1000)

// CO5300 AMOLED: initialize at full brightness, then restore the saved setting.
static const co5300_lcd_init_cmd_t vendor_specific_init[] = {
    // {cmd, { data }, data_size, delay_ms}
    {0xFE, (uint8_t []){0x00}, 0, 0},
    {0xC4, (uint8_t []){0x80}, 1, 0},
    {0x3A, (uint8_t []){0x55}, 0, 10}, // RGB565
    {0x35, (uint8_t []){0x00}, 0, 10},
    {0x53, (uint8_t []){0x20}, 1, 10},
    {0x51, (uint8_t []){0xFF}, 1, 10},
    {0x63, (uint8_t []){0xFF}, 1, 10},
    {0x2A, (uint8_t []){0x00, 0x00, 0x01, 0xD1}, 4, 0}, // Column address: 0-465
    {0x2B, (uint8_t []){0x00, 0x00, 0x01, 0xD1}, 4, 0}, // Row address: 0-465
    {0x11, (uint8_t []){0x00}, 0, 120}, // Exit sleep
};

class StopwatchBacklight : public Backlight {
public:
    StopwatchBacklight(esp_lcd_panel_io_handle_t panel_io, Display* display)
        : panel_io_(panel_io), display_(display) {}

protected:
    void SetBrightnessImpl(uint8_t brightness) override {
        DisplayLockGuard lock(display_);
        uint8_t data[] = {static_cast<uint8_t>((255 * brightness) / 100)};
        int lcd_cmd = 0x51;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;

        esp_err_t err = esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, data, sizeof(data));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set display brightness: %s", esp_err_to_name(err));
        }
    }

private:
    esp_lcd_panel_io_handle_t panel_io_;
    Display* display_;
};

class M5StackStopwatchBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    M5PM1 pmic_;
    M5IOE1 ioe_;
    Button button1_;
    Button button2_;
    PotatoFaceDisplay* display_;
    StopwatchBacklight* backlight_;
    LauncherScreen launcher_;
    VoiceInputApp voice_input_;
    Cst820 touch_;
    // 组合键（A+B）检测状态
    int64_t btn1_down_us_ = 0;
    int64_t btn2_down_us_ = 0;
    int64_t last_combo_us_ = -1000000000LL;

    // LVGL 触摸输入读取回调
    static void TouchReadCb(lv_indev_t* indev, lv_indev_data_t* data) {
        auto* self = static_cast<M5StackStopwatchBoard*>(lv_indev_get_driver_data(indev));
        if (self->touch_.read() && self->touch_.getFingerNum() > 0) {
            data->state = LV_INDEV_STATE_PR;
            data->point.x = self->touch_.getX();
            data->point.y = self->touch_.getY();
        } else {
            data->state = LV_INDEV_STATE_REL;
        }
    }

    void InitializeTouch() {
        // CST820B 触摸复位（经 IO 扩展器）
        ioe_.pinMode(IOE_PIN_TOUCH_RST, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_TOUCH_RST, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_TOUCH_RST, LOW);
        vTaskDelay(pdMS_TO_TICKS(10));
        ioe_.digitalWrite(IOE_PIN_TOUCH_RST, HIGH);
        vTaskDelay(pdMS_TO_TICKS(50));

        if (!touch_.begin(i2c_bus_, 0x15)) {
            ESP_LOGE(TAG, "CST820 touch init failed");
            return;
        }

        // 注册 LVGL 触摸输入设备（lv_init 已在 InitializeDisplay 中执行）
        lv_indev_t* indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_driver_data(indev, this);
        lv_indev_set_read_cb(indev, &M5StackStopwatchBoard::TouchReadCb);
        ESP_LOGI(TAG, "Touch initialized (CST820)");
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {.enable_internal_pullup = 1},
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        if (ioe_.begin(i2c_bus_, M5IOE1_I2C_ADDR, M5IOE1_I2C_FREQ_100K, M5IOE1_INT_MODE_POLLING) != M5IOE1_OK) {
            ESP_LOGE(TAG, "M5IOE1 begin failed");
            return;
        }

        if (pmic_.begin(i2c_bus_, M5PM1_DEFAULT_ADDR, M5PM1_I2C_FREQ_100K) != M5PM1_OK) {
            ESP_LOGE(TAG, "M5PM1 begin failed");
            return;
        }

        pmic_.setChargeEnable(true);
        pmic_.setBoostEnable(true);
        pmic_.pinMode(PMIC_PIN_CHARGE_STATE, INPUT);
        pmic_.pinMode(PMIC_PIN_CHARGE_PROG, OUTPUT);
        pmic_.digitalWrite(PMIC_PIN_CHARGE_PROG, LOW); // Set charge current to 425mA

        ioe_.pinMode(IOE_PIN_LCD_POWER, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_LCD_POWER, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_LCD_POWER, HIGH);

        ioe_.pinMode(IOE_PIN_LCD_RST, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_LCD_RST, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_LCD_RST, HIGH);

        ioe_.pinMode(IOE_PIN_CODEC_POWER, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_CODEC_POWER, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_CODEC_POWER, HIGH);

        ioe_.pinMode(IOE_PIN_PA_EN, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_PA_EN, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_PA_EN, HIGH);

        ioe_.pinMode(IOE_PIN_MOTOR, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_MOTOR, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_MOTOR, LOW);

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = DISPLAY_QSPI_SCK;
        buscfg.data0_io_num = DISPLAY_QSPI_D0;
        buscfg.data1_io_num = DISPLAY_QSPI_D1;
        buscfg.data2_io_num = DISPLAY_QSPI_D2;
        buscfg.data3_io_num = DISPLAY_QSPI_D3;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_QSPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        ioe_.digitalWrite(IOE_PIN_LCD_RST, LOW);
        vTaskDelay(pdMS_TO_TICKS(10));
        ioe_.digitalWrite(IOE_PIN_LCD_RST, HIGH);
        vTaskDelay(pdMS_TO_TICKS(120));

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_QSPI_CS;
        io_config.dc_gpio_num = GPIO_NUM_NC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_QSPI_HOST, &io_config, &panel_io));

        co5300_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(co5300_lcd_init_cmd_t),
            .flags = {.use_qspi_interface = 1},
        };

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = &vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(panel_io, &panel_config, &panel));

        esp_lcd_panel_set_gap(panel, 0x06, 0);
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

        display_ = new PotatoFaceDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                       DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                       DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new StopwatchBacklight(panel_io, display_);
        backlight_->RestoreBrightness();
    }

    // A+B 同时按住 → 返回主页面
    void MaybeTriggerCombo() {
        if (btn1_down_us_ == 0 || btn2_down_us_ == 0) return;
        if (llabs(btn1_down_us_ - btn2_down_us_) > COMBO_WINDOW_US) return;
        if (esp_timer_get_time() - last_combo_us_ < COMBO_SUPPRESS_US) return;
        last_combo_us_ = esp_timer_get_time();
        HandleGoHome();
    }

    void HandleGoHome() {
        auto& app = Application::GetInstance();
        // 语音输入模式：退出
        if (voice_input_.IsActive()) {
            voice_input_.Exit();
        }
        // 结束当前对话（若有）
        auto state = app.GetDeviceState();
        if (state == kDeviceStateListening) {
            app.StopListening();
        } else if (state == kDeviceStateSpeaking) {
            app.AbortSpeaking(kAbortReasonNone);
            app.SetDeviceState(kDeviceStateIdle);
        } else if (state == kDeviceStateConnecting) {
            app.SetDeviceState(kDeviceStateIdle);
        }
        // 显示主页面
        ShowLauncher(true);
        // 主页面期间关唤醒词（Idle 时）
        if (app.GetDeviceState() == kDeviceStateIdle) {
            app.GetAudioService().EnableWakeWordDetection(false);
        }
    }

    void InitializeButtons() {
        // 主页面图标点击（触摸）→ 进入对应 app
        // 回调在 LVGL 任务上下文触发，经 Schedule 转发到主任务执行
        launcher_.SetOnAppClicked([this](int app_id) {
            Application::GetInstance().Schedule([this, app_id]() {
                auto& app = Application::GetInstance();
                if (app_id == LauncherScreen::kAppVoiceInput) {
                    // 语音输入模式
                    ShowLauncher(false);
                    voice_input_.Enter();
                    return;
                }
                // AI 对话
                ShowLauncher(false);
                if (app.GetDeviceState() == kDeviceStateIdle) {
                    app.GetAudioService().EnableWakeWordDetection(true);
                }
                app.ToggleChatState();
            });
        });

        // 组合键检测：记录两键按下时刻，双键同时按下触发返回主页面
        button1_.OnPressDown([this]() {
            if (voice_input_.IsActive()) {
                // 语音输入模式：按住说话
                voice_input_.OnRecordButtonDown();
                return;
            }
            btn1_down_us_ = esp_timer_get_time();
            MaybeTriggerCombo();
        });
        button2_.OnPressDown([this]() {
            btn2_down_us_ = esp_timer_get_time();
            MaybeTriggerCombo();
        });
        button1_.OnPressUp([this]() {
            btn1_down_us_ = 0;
            if (voice_input_.IsActive()) {
                voice_input_.OnRecordButtonUp();
            }
        });
        button2_.OnPressUp([this]() { btn2_down_us_ = 0; });

        // Button1: 语音模式屏蔽单击；主页面时进入对话；否则 wake / toggle conversation
        button1_.OnClick([this]() {
            if (voice_input_.IsActive()) return; // 语音输入模式：按住说话已处理
            if (esp_timer_get_time() - last_combo_us_ < COMBO_SUPPRESS_US) return; // 组合键释放屏蔽
            auto& app = Application::GetInstance();
            if (launcher_.IsVisible()) {
                // 主页面：隐藏主页面 + 恢复唤醒词 + 开始对话
                // （协议未就绪/Starting 态时 ToggleChatState 内部安全返回）
                ShowLauncher(false);
                if (app.GetDeviceState() == kDeviceStateIdle) {
                    app.GetAudioService().EnableWakeWordDetection(true);
                }
                app.ToggleChatState();
                return;
            }
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiManager::GetInstance().IsConnected()) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        // Button2: volume 0 -> 10 -> ... -> 100 -> 0
        button2_.OnClick([this]() {
            if (esp_timer_get_time() - last_combo_us_ < COMBO_SUPPRESS_US) return; // 组合键释放屏蔽
            auto* codec = GetAudioCodec();
            int volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(std::string(Lang::Strings::VOLUME) + ":" + std::to_string(volume) + "%");
        });
    }

public:
    M5StackStopwatchBoard()
        : i2c_bus_(nullptr),
          button1_(BUTTON1_GPIO),
          button2_(BUTTON2_GPIO),
          display_(nullptr),
          backlight_(nullptr) {
        InitializeI2c();
        InitializeSpi();
        InitializeDisplay();
        InitializeTouch();
        InitializeButtons();
    }

    AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            i2c_bus_,
            I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_GPIO_PA,
            AUDIO_CODEC_ES8311_ADDR,
            false);
        return &audio_codec;
    }

    Display* GetDisplay() override {
        return display_;
    }

    Backlight* GetBacklight() override {
        return backlight_;
    }

    // Launcher（主页面）接口
    bool IsLauncherVisible() const override {
        return launcher_.IsVisible();
    }

    void ShowLauncher(bool show) override {
        launcher_.SetVisible(show);
    }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        uint16_t voltage_mv = 0;
        if (pmic_.readVbat(&voltage_mv) != M5PM1_OK) {
            return false;
        }

        int charge_state_level = pmic_.digitalRead(PMIC_PIN_CHARGE_STATE);
        if (charge_state_level < 0) {
            // Charge status read failed; leave charging/discharging unknown
            charging = false;
            discharging = false;
        } else {
            // M5PM1 charge status is active low: 0 means charging, 1 means not charging
            charging = (charge_state_level == 0);
            discharging = !charging;
        }

        const int BATTERY_MIN_VOLTAGE = 3400;
        const int BATTERY_MAX_VOLTAGE = 4200;
        if (voltage_mv < BATTERY_MIN_VOLTAGE) {
            level = 0;
        } else if (voltage_mv > BATTERY_MAX_VOLTAGE) {
            level = 100;
        } else {
            level = ((voltage_mv - BATTERY_MIN_VOLTAGE) * 100) / (BATTERY_MAX_VOLTAGE - BATTERY_MIN_VOLTAGE);
        }
        return true;
    }
};

DECLARE_BOARD(M5StackStopwatchBoard);
