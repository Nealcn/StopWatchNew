// 设置功能（第四功能）：原版 D:\stopwatch app_setup（SelectMenuPage + workers 原样复用）
// 本文件为 App 外壳（菜单项适配本固件：亮度/音量/时间/日期/关于/清除配置）
#ifndef _SETUP_H_
#define _SETUP_H_

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <lvgl.h>

#include "board.h"
#include "display/display.h"
#include "wifi_board.h"
#include "setup_view/view.h"
#include "setup_workers/workers.h"
#include "settings.h"

class SetupApp {
public:
    SetupApp() = default;

    void Enter() {
        if (active_.load()) return;
        DisplayLockGuard lock(Board::GetInstance().GetDisplay());
        BuildMenu();
        menu_timer_ = lv_timer_create(&SetupApp::MenuTimerCb, 100, this);
        active_.store(true);
        ESP_LOGI(TAG, "Setup entered");
    }

    void Exit() {
        if (!active_.load()) return;
        DisplayLockGuard lock(Board::GetInstance().GetDisplay());
        if (menu_timer_) {
            lv_timer_delete(menu_timer_);
            menu_timer_ = nullptr;
        }
        worker_.reset();
        menu_page_.reset();
        sections_.clear();
        active_.store(false);
        ESP_LOGI(TAG, "Setup exited");
    }

    bool IsActive() const { return active_.load(); }

private:
    static constexpr const char* TAG = "Setup";

    std::vector<view::SelectMenuPage::MenuSection> sections_;
    std::unique_ptr<view::SelectMenuPage> menu_page_;
    std::unique_ptr<setup_workers::WorkerBase> worker_;
    lv_timer_t* menu_timer_ = nullptr;
    std::atomic<bool> active_{false};

    void OpenWorker(std::unique_ptr<setup_workers::WorkerBase> w) {
        worker_ = std::move(w);
    }

    void BuildMenu() {
        sections_.clear();
        sections_ = {
            {
                "Device",
                {
                    {"Brightness",
                     [this]() { OpenWorker(std::make_unique<setup_workers::BrightnessWorker>()); }},
                    {"Volume",
                     [this]() { OpenWorker(std::make_unique<setup_workers::VolumeWorker>()); }},
                    {"WiFi Setup",
                     [this]() {
                         // 先退出设置界面（配网 Alert 显示在可见层），再进入配网 AP
                         Exit();
                         auto* wifi_board = dynamic_cast<WifiBoard*>(&Board::GetInstance());
                         if (wifi_board) {
                             wifi_board->EnterWifiConfigMode();
                         }
                     }},
                },
            },
            {
                "Date & Time",
                {
                    {"Set Time",
                     [this]() { OpenWorker(std::make_unique<setup_workers::SetTimeWorker>()); }},
                    {"Set Date",
                     [this]() { OpenWorker(std::make_unique<setup_workers::SetDateWorker>()); }},
                },
            },
            {
                "System",
                {
                    // 挂绳模式：整屏 180° 反转。标签显示当前状态，重新进设置时刷新。
                    {std::string("Screen: ") +
                         (Board::GetInstance().IsScreenRotated180() ? "Rotated 180" : "Normal"),
                     []() {
                         bool target = !Board::GetInstance().IsScreenRotated180();
                         Board::GetInstance().SetScreenRotated180(target);
                         Board::GetInstance().GetDisplay()->ShowNotification(
                             target ? "Screen rotated 180" : "Screen normal");
                     }},
                    {"Clear AI Config",
                     [this]() {
                         Settings ws_settings("websocket", true);
                         ws_settings.EraseKey("url");
                         ws_settings.EraseKey("host");
                         ws_settings.EraseKey("port");
                         ws_settings.EraseKey("token");
                         Settings mqtt_settings("mqtt", true);
                         mqtt_settings.EraseAll();
                         ESP_LOGI(TAG, "AI config cleared, will re-activate");
                     }},
                },
            },
        };
        menu_page_ = std::make_unique<view::SelectMenuPage>(sections_);
    }

    static void MenuTimerCb(lv_timer_t* t) {
        auto* self = static_cast<SetupApp*>(lv_timer_get_user_data(t));
        if (!self->active_.load()) return;
        // LVGL 任务上下文：菜单 update + worker update
        if (self->menu_page_) {
            self->menu_page_->update();
        }
        if (self->worker_) {
            self->worker_->update();
            if (self->worker_->isDone()) {
                self->worker_.reset();
                self->BuildMenu();  // 重建菜单返回
            }
        }
    }
};

#endif // _SETUP_H_
