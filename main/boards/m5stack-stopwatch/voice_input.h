// 语音输入模式（VoiceCube 兼容）：按住说话 → 云 ASR → 屏上预览 → 确认后桌面端粘贴
// 协议参考 D:\stopwatch 的 app_voicecube + ble_voice（Nealcn/VoiceCube 兼容）
#ifndef _VOICE_INPUT_H_
#define _VOICE_INPUT_H_

#include <atomic>
#include <cstring>
#include <functional>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <lvgl.h>

#include "board.h"
#include "application.h"
#include "ble_voice.h"
#include "display/display.h"
#include "lvgl_theme.h"
#include "codecs/es8311_audio_codec.h"
#include "encoder/impl/esp_opus_enc.h"
#include "esp_ae_rate_cvt.h"

// 中文字体（已编译进固件）
LV_FONT_DECLARE(font_puhui_basic_20_4);

class VoiceInputApp {
public:
    VoiceInputApp() = default;

    void Enter() {
        if (active_.load()) return;
        auto& app = Application::GetInstance();

        // 1) mic 独占：停掉 xiaozhi 侧输入（AudioInputTask 阻塞在事件组）
        app.GetAudioService().EnableVoiceProcessing(false);
        app.GetAudioService().EnableWakeWordDetection(false);
        app.GetAudioService().SetVoiceInputActive(true);

        // 2) 启动 BLE 语音服务
        framework::BleVoice::get().setControlCallback([this](const std::string& json) { OnControl(json); });
        framework::BleVoice::get().setConnectCallback([this](bool connected) {
            // 连接状态变化 → 主任务更新 UI（NimBLE 任务上下文，不可直接操作 LVGL）
            Application::GetInstance().Schedule([this, connected]() {
                if (active_.load() && state_ == State::Idle) {
                    SetState(State::Idle); // 触发 UI 刷新（Idle 文案按连接状态显示）
                }
            });
        });
        framework::BleVoice::get().start();

        // 3) 创建 UI（lazy，须在 SetupUI 之后）
        {
            DisplayLockGuard lock(Board::GetInstance().GetDisplay());
            CreateUI();
        }

        active_.store(true);
        SetState(State::Idle);
        ESP_LOGI(TAG, "Voice input mode entered");
    }

    void Exit() {
        if (!active_.load()) return;
        StopRecording();
        framework::BleVoice::get().stopAdvertising();
        framework::BleVoice::get().setControlCallback(nullptr);
        auto& app = Application::GetInstance();
        app.GetAudioService().SetVoiceInputActive(false);
        // 恢复 xiaozhi 音频管线（唤醒词由状态机/按键路径恢复）
        if (app.GetDeviceState() == kDeviceStateIdle) {
            app.GetAudioService().EnableWakeWordDetection(true);
        }
        {
            DisplayLockGuard lock(Board::GetInstance().GetDisplay());
            DestroyUI();
        }
        active_.store(false);
        ESP_LOGI(TAG, "Voice input mode exited");
    }

    bool IsActive() const { return active_.load(); }

    // 按住说话（按键 1 按下/松开）
    void OnRecordButtonDown() {
        if (!active_.load()) return;
        if (state_ == State::Rec) return;
        if (state_ == State::Preview || state_ == State::Pasting) return; // 预览确认中不打断
        StartRecording();
    }

    void OnRecordButtonUp() {
        if (!active_.load()) return;
        if (state_ == State::Rec) {
            StopRecording();
        }
    }

    // 清除当前录音/预览（按键 2）：终止录音并回到 Idle，通知桌面端结束会话
    void ClearRecording() {
        if (!active_.load()) return;
        if (state_ == State::Idle) return;
        if (state_ == State::Rec) {
            record_stop_.store(true);
            if (record_task_ != nullptr) {
                vTaskDelete(record_task_);
                record_task_ = nullptr;
            }
        }
        char json[96];
        snprintf(json, sizeof(json),
                 "{\"event\":\"button_up\",\"button\":\"primary\",\"duration_ms\":0,\"session_id\":%lu}",
                 (unsigned long)session_id_);
        framework::BleVoice::get().sendStateJson(json);
        preview_text_.clear();
        SetState(State::Idle);
    }


    // 完整中文字体（assets 加载的 font_puhui_common_20_4.bin，6649 字）；
    // basic 版（800 字）缺常用字（如"识别"的"识/别"）会显示空白
    static const lv_font_t* GetTextFont() {
        auto* display = Board::GetInstance().GetDisplay();
        auto* theme = static_cast<LvglTheme*>(display->GetTheme());
        if (theme && theme->text_font() && theme->text_font()->font()) {
            return theme->text_font()->font();
        }
        return &font_puhui_basic_20_4;
    }

private:
    static constexpr const char* TAG = "VoiceInput";


    enum class State : uint8_t { Idle, Rec, Asr, Preview, Pasting };

    static constexpr int kChunkMs = 100;          // 录音块 100ms
    static constexpr int kSrcChunkSamples = 2400; // 24k * 100ms
    static constexpr int kDstChunkSamples = 1600; // 16k * 100ms
    static constexpr int kFrameSamples = 960;     // 16k * 60ms
    static constexpr uint8_t kFlagStart = 0x01;
    static constexpr uint8_t kFlagEnd = 0x02;

    // ---- UI ----
    lv_obj_t* panel_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* mic_group_ = nullptr;      // 环形点波图形组（中心圆 + 环绕圆点）
    lv_obj_t* mic_core_ = nullptr;       // 中心圆
    lv_obj_t* mic_dots_[12] = {};        // 环绕圆点（环形点波动画）
    int32_t wave_phase_ = 0;             // 点波动画相位
    lv_timer_t* wave_timer_ = nullptr;   // 点波动画定时器
    lv_obj_t* preview_label_ = nullptr;
    lv_obj_t* hint_label_ = nullptr;
    lv_obj_t* confirm_btn_ = nullptr;
    lv_obj_t* cancel_btn_ = nullptr;

    // ---- 录音 ----
    TaskHandle_t record_task_ = nullptr;
    std::atomic<bool> record_stop_{false};
    uint32_t session_id_ = 0;
    uint32_t seq_ = 0;
    int64_t record_start_us_ = 0;
    void* opus_enc_ = nullptr;
    void* rate_cvt_ = nullptr;

    State state_ = State::Idle;
    std::atomic<bool> active_{false};
    std::string preview_text_;

    void CreateUI() {
        lv_obj_t* screen = lv_screen_active();
        panel_ = lv_obj_create(screen);
        lv_obj_set_size(panel_, 466, 466);
        lv_obj_align(panel_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(panel_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_border_width(panel_, 0, 0);
        lv_obj_set_style_radius(panel_, 0, 0);
        lv_obj_clear_flag(panel_, LV_OBJ_FLAG_SCROLLABLE);

        status_label_ = lv_label_create(panel_);
        lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, 16);
        lv_obj_set_style_text_font(status_label_, GetTextFont(), 0);
        lv_obj_set_style_text_color(status_label_, lv_color_hex(0x9AA5B5), 0);

        // 环形点波图形组（中心圆 + 12 个环绕圆点，录音时点沿径向波动）
        mic_group_ = lv_obj_create(panel_);
        lv_obj_remove_style_all(mic_group_);
        lv_obj_set_size(mic_group_, 300, 300);
        lv_obj_align(mic_group_, LV_ALIGN_CENTER, 0, -35);
        lv_obj_clear_flag(mic_group_, LV_OBJ_FLAG_SCROLLABLE);

        mic_core_ = lv_obj_create(mic_group_);
        lv_obj_remove_style_all(mic_core_);
        lv_obj_set_size(mic_core_, 130, 130);
        lv_obj_center(mic_core_);
        lv_obj_set_style_radius(mic_core_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(mic_core_, LV_OPA_COVER, 0);

        for (int i = 0; i < 12; i++) {
            mic_dots_[i] = lv_obj_create(mic_group_);
            lv_obj_remove_style_all(mic_dots_[i]);
            lv_obj_set_size(mic_dots_[i], 14, 14);
            lv_obj_set_style_radius(mic_dots_[i], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(mic_dots_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(mic_dots_[i], lv_color_hex(0x9AA5B5), 0);
            // 初始位置：均布在半径 105 的圆环上
            double rad = i * (2.0 * 3.14159265 / 12.0);
            lv_obj_set_pos(mic_dots_[i], (int)(150 + 105 * cos(rad) - 7), (int)(150 + 105 * sin(rad) - 7));
        }
        // 点波动画：50ms 一帧
        wave_timer_ = lv_timer_create(&VoiceInputApp::WaveTimerCb, 50, this);

        // 识别结果预览（仅 Preview 态显示）
        preview_label_ = lv_label_create(panel_);
        lv_obj_align(preview_label_, LV_ALIGN_CENTER, 0, -25);
        lv_obj_set_style_text_font(preview_label_, GetTextFont(), 0);
        lv_obj_set_style_text_color(preview_label_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_width(preview_label_, 400);
        lv_obj_set_style_text_align(preview_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(preview_label_, LV_OBJ_FLAG_HIDDEN);

        // 提示文字
        hint_label_ = lv_label_create(panel_);
        lv_obj_align(hint_label_, LV_ALIGN_BOTTOM_MID, 0, -112);
        lv_obj_set_style_text_font(hint_label_, GetTextFont(), 0);
        lv_obj_set_style_text_color(hint_label_, lv_color_hex(0x6B7686), 0);

        // 确认/取消按钮
        confirm_btn_ = lv_btn_create(panel_);
        lv_obj_set_size(confirm_btn_, 130, 52);
        lv_obj_align(confirm_btn_, LV_ALIGN_BOTTOM_MID, -110, -60);
        lv_obj_set_style_bg_color(confirm_btn_, lv_color_hex(0x2E6BE6), 0);
        lv_obj_add_flag(confirm_btn_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(confirm_btn_, [](lv_event_t* e) {
            static_cast<VoiceInputApp*>(lv_event_get_user_data(e))->OnConfirmClicked();
        }, LV_EVENT_CLICKED, this);
        lv_obj_t* confirm_label = lv_label_create(confirm_btn_);
        lv_label_set_text(confirm_label, "确认");
        lv_obj_set_style_text_font(confirm_label, GetTextFont(), 0);
        lv_obj_center(confirm_label);

        cancel_btn_ = lv_btn_create(panel_);
        lv_obj_set_size(cancel_btn_, 130, 52);
        lv_obj_align(cancel_btn_, LV_ALIGN_BOTTOM_MID, 110, -60);
        lv_obj_set_style_bg_color(cancel_btn_, lv_color_hex(0x444444), 0);
        lv_obj_add_flag(cancel_btn_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(cancel_btn_, [](lv_event_t* e) {
            static_cast<VoiceInputApp*>(lv_event_get_user_data(e))->OnCancelClicked();
        }, LV_EVENT_CLICKED, this);
        lv_obj_t* cancel_label = lv_label_create(cancel_btn_);
        lv_label_set_text(cancel_label, "取消");
        lv_obj_set_style_text_font(cancel_label, GetTextFont(), 0);
        lv_obj_center(cancel_label);

        lv_obj_move_to_index(panel_, lv_obj_get_child_cnt(screen) - 1);
    }

    void DestroyUI() {
        if (wave_timer_) {
            lv_timer_delete(wave_timer_);
            wave_timer_ = nullptr;
        }
        if (panel_) {
            lv_obj_delete(panel_);
            panel_ = nullptr;
            status_label_ = nullptr;
            mic_group_ = nullptr;
            mic_core_ = nullptr;
            for (auto& d : mic_dots_) d = nullptr;
            preview_label_ = hint_label_ = confirm_btn_ = cancel_btn_ = nullptr;
        }
    }

    void SetState(State s) {
        state_ = s;
        if (!active_.load()) return;
        DisplayLockGuard lock(Board::GetInstance().GetDisplay());
        if (!panel_) return;
        UpdateUI();
    }

    void UpdateUI() {
        const char* status = "";
        const char* hint = "";
        uint32_t color = 0x9AA5B5;
        bool show_preview = false;
        switch (state_) {
            case State::Idle:
                status = framework::BleVoice::get().isConnected() ? "已连接桌面端" : "等待连接电脑…";
                hint = "按住按键 1 说话";
                break;
            case State::Rec:
                status = "录音中…";
                hint = "松开按键 1 结束";
                color = 0xFF7A8A;
                break;
            case State::Asr:
                status = "识别中…";
                hint = "";
                color = 0xFFC46A;
                break;
            case State::Preview:
                status = "识别结果（未粘贴）";
                hint = "点确认粘贴，或取消";
                color = 0x7AC4FF;
                show_preview = true;
                break;
            case State::Pasting:
                status = "已粘贴";
                hint = "";
                color = 0x7AC4FF;
                break;
        }
        lv_label_set_text(status_label_, status);
        lv_label_set_text(hint_label_, hint);
        lv_obj_set_style_bg_color(mic_core_, lv_color_hex(color), 0);
        for (int i = 0; i < 12; i++) {
            lv_obj_set_style_bg_color(mic_dots_[i], lv_color_hex(color), 0);
        }
        if (show_preview) {
            lv_obj_add_flag(mic_group_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(preview_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(confirm_btn_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(cancel_btn_, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(preview_label_, preview_text_.c_str());
        } else {
            if (mic_group_) lv_obj_clear_flag(mic_group_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(preview_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(confirm_btn_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(cancel_btn_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // 环形点波动画：录音/识别时点沿径向波动，空闲时轻微呼吸
    static void WaveTimerCb(lv_timer_t* t) {
        auto* self = static_cast<VoiceInputApp*>(lv_timer_get_user_data(t));
        self->UpdateWave();
    }

    void UpdateWave() {
        if (!active_.load() || !mic_group_ || !panel_) return;
        double amplitude = 4.0;
        switch (state_) {
            case State::Rec: amplitude = 18.0; break;
            case State::Asr: amplitude = 11.0; break;
            case State::Pasting: amplitude = 7.0; break;
            default: amplitude = 4.0; break;
        }
        wave_phase_++;
        for (int i = 0; i < 12; i++) {
            double rad = i * (2.0 * 3.14159265 / 12.0);
            double w = sin(wave_phase_ * 0.35 + i * 0.9) * amplitude;
            int r = (int)(105 + w);
            lv_obj_set_pos(mic_dots_[i], (int)(150 + r * cos(rad) - 7), (int)(150 + r * sin(rad) - 7));
        }
    }

    // ---- 录音 ----
    void StartRecording() {
        if (record_task_ != nullptr) return;
        record_stop_.store(false);
        seq_ = 0;
        ++session_id_;
        record_start_us_ = esp_timer_get_time();
        // 兼容 VoiceCube 原版桌面端：button_down 触发 ASR 会话
        char json[96];
        snprintf(json, sizeof(json),
                 "{\"event\":\"button_down\",\"button\":\"primary\",\"session_id\":%lu}",
                 (unsigned long)session_id_);
        framework::BleVoice::get().sendStateJson(json);
        SetState(State::Rec);
        if (xTaskCreateWithCaps(RecordTaskEntry, "vc_record", 24576, this, 3, &record_task_,
                                MALLOC_CAP_SPIRAM) != pdPASS) {
            record_task_ = nullptr;
            ESP_LOGE(TAG, "record task create failed");
            SetState(State::Idle);
        }
    }

    void StopRecording() {
        if (record_task_ == nullptr) return;
        record_stop_.store(true);
        for (int i = 0; i < 50 && record_task_ != nullptr; ++i) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (record_task_ != nullptr) {
            vTaskDelete(record_task_);
            record_task_ = nullptr;
        }
        // 兼容 VoiceCube 原版桌面端：button_up 触发流结束
        uint32_t duration_ms = (uint32_t)((esp_timer_get_time() - record_start_us_) / 1000);
        char json[128];
        snprintf(json, sizeof(json),
                 "{\"event\":\"button_up\",\"button\":\"primary\",\"duration_ms\":%lu,\"session_id\":%lu}",
                 (unsigned long)duration_ms, (unsigned long)session_id_);
        framework::BleVoice::get().sendStateJson(json);
        // 进入识别等待（桌面端 ASR 返回后进入 Preview）
        SetState(State::Asr);
    }

    static void RecordTaskEntry(void* arg) {
        auto* self = static_cast<VoiceInputApp*>(arg);
        self->RecordLoop();
    }

    void RecordLoop() {
        auto* codec = Board::GetInstance().GetAudioCodec();

        // 重采样器 24k→16k
        esp_ae_rate_cvt_cfg_t cvt_cfg = {};
        cvt_cfg.src_rate = 24000;
        cvt_cfg.dest_rate = 16000;
        cvt_cfg.channel = 1;
        cvt_cfg.bits_per_sample = 16;
        cvt_cfg.complexity = 2;
        cvt_cfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
        if (esp_ae_rate_cvt_open(&cvt_cfg, &rate_cvt_) != ESP_OK) {
            ESP_LOGE(TAG, "rate cvt open failed");
            record_task_ = nullptr;
            vTaskDelete(nullptr);
            return;
        }

        // Opus 编码器 16k/mono/60ms，DTX 关闭（ASR 会吞语音尾部）
        esp_opus_enc_config_t enc_cfg = {};
        enc_cfg.sample_rate = 16000;
        enc_cfg.channel = ESP_AUDIO_MONO;
        enc_cfg.bits_per_sample = 16;
        enc_cfg.bitrate = 28000;
        enc_cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
        enc_cfg.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;
        enc_cfg.complexity = 3;
        enc_cfg.enable_fec = false;
        enc_cfg.enable_dtx = false;
        enc_cfg.enable_vbr = true;
        if (esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &opus_enc_) != ESP_OK) {
            ESP_LOGE(TAG, "opus enc open failed");
            esp_ae_rate_cvt_close(rate_cvt_);
            rate_cvt_ = nullptr;
            record_task_ = nullptr;
            vTaskDelete(nullptr);
            return;
        }

        codec->EnableInput(true);
        ESP_LOGI(TAG, "recording start, session=%u", session_id_);

        int16_t frame_pcm[kFrameSamples];
        uint8_t opus_out[400];
        size_t frame_fill = 0;

        ESP_LOGI(TAG, "record loop started");
        uint32_t frame_count = 0;
        // 小块循环读（10ms/160 samples，与 AudioService 一致；大块读会阻塞在 I2S DMA）
        constexpr int kReadSamples = 160;
        std::vector<int16_t> read_buf(kReadSamples);
        std::vector<int16_t> chunk(kSrcChunkSamples);
        size_t chunk_fill = 0;
        while (!record_stop_.load()) {
            if (!codec->InputData(read_buf)) {
                ESP_LOGW(TAG, "InputData failed");
                continue;
            }
            memcpy(&chunk[chunk_fill], read_buf.data(), kReadSamples * sizeof(int16_t));
            chunk_fill += kReadSamples;
            if (chunk_fill < kSrcChunkSamples) {
                continue;
            }
            chunk_fill = 0;

            // 重采样 → 16k（先查所需输出大小，避免缓冲不足静默失败）
            uint32_t max_out = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(rate_cvt_, kSrcChunkSamples, &max_out);
            std::vector<int16_t> resampled(max_out > 0 ? max_out : kDstChunkSamples);
            uint32_t out_samples = max_out;
            if (esp_ae_rate_cvt_process(rate_cvt_, chunk.data(), kSrcChunkSamples, resampled.data(),
                                        &out_samples) != ESP_OK) {
                ESP_LOGW(TAG, "rate cvt failed");
                continue;
            }

            // 攒满 960 samples（60ms）编码一帧
            size_t i = 0;
            while (i < out_samples && !record_stop_.load()) {
                size_t need = kFrameSamples - frame_fill;
                size_t take = (out_samples - i < need) ? (out_samples - i) : need;
                memcpy(&frame_pcm[frame_fill], &resampled[i], take * sizeof(int16_t));
                frame_fill += take;
                i += take;
                if (frame_fill == kFrameSamples) {
                    esp_audio_enc_in_frame_t enc_in = {};
                    enc_in.buffer = reinterpret_cast<uint8_t*>(frame_pcm);
                    enc_in.len = kFrameSamples * sizeof(int16_t);
                    esp_audio_enc_out_frame_t enc_out = {};
                    enc_out.buffer = opus_out;
                    enc_out.len = sizeof(opus_out);
                    if (esp_opus_enc_process(opus_enc_, &enc_in, &enc_out) == ESP_OK && enc_out.encoded_bytes > 0) {
                        uint8_t flags = (seq_ == 0) ? kFlagStart : 0;
                        bool sent = framework::BleVoice::get().sendAudioFrame(opus_out, enc_out.encoded_bytes, session_id_, seq_, flags);
                        if (++frame_count % 30 == 0) {
                            ESP_LOGI(TAG, "sent frame seq=%u len=%u ok=%d", seq_, (unsigned)enc_out.encoded_bytes, (int)sent);
                        }
                        ++seq_;
                    }
                    frame_fill = 0;
                }
            }
        }

        // 结束帧
        if (seq_ > 0) {
            framework::BleVoice::get().sendAudioFrame(nullptr, 0, session_id_, seq_, kFlagEnd);
            ESP_LOGI(TAG, "recording done, session=%u frames=%u", session_id_, seq_);
        }

        codec->EnableInput(false);
        esp_opus_enc_close(opus_enc_);
        opus_enc_ = nullptr;
        esp_ae_rate_cvt_close(rate_cvt_);
        rate_cvt_ = nullptr;
        record_task_ = nullptr;
        vTaskDelete(nullptr);
    }

    // ---- BLE 下行（NimBLE 任务上下文）----
    void OnControl(const std::string& json) {
        // 解析 event
        if (json.find("\"asr\"") != std::string::npos) {
            auto pos = json.find("\"text\"");
            if (pos != std::string::npos) {
                auto start = json.find('"', pos + 7);
                auto end = json.find('"', start + 1);
                if (start != std::string::npos && end != std::string::npos) {
                    std::string text = json.substr(start + 1, end - start - 1);
                    Application::GetInstance().Schedule([this, text]() {
                        preview_text_ = text;
                        SetState(State::Preview);
                    });
                }
            }
        } else if (json.find("\"paste_result\"") != std::string::npos) {
            bool ok = json.find("\"ok\":true") != std::string::npos;
            Application::GetInstance().Schedule([this, ok]() {
                if (ok) {
                    preview_text_.clear();
                    SetState(State::Pasting);
                }
            });
        }
    }

    void OnConfirmClicked() {
        // 确认粘贴（LVGL 任务上下文，发送不涉及 LVGL）
        framework::BleVoice::get().sendStateJson("{\"event\":\"paste_request\"}");
        SetState(State::Pasting);
    }

    void OnCancelClicked() {
        preview_text_.clear();
        SetState(State::Idle);
    }
};

#endif // _VOICE_INPUT_H_
