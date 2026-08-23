// 表盘功能（第三功能）：原版 D:\stopwatch app_watch_face（4 种表盘：经典/简约/大数字/数字流）
// 本文件为 App 外壳（Enter/Exit/按键/定时器），视图层原样复用 watch_face_view/
#ifndef _WATCH_FACE_H_
#define _WATCH_FACE_H_

#include <atomic>
#include <memory>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <lvgl.h>

#include "board.h"
#include "display/display.h"
#include "watch_face_view/watch_face.h"

class WatchFaceApp {
public:
    WatchFaceApp() = default;

    void Enter() {
        if (active_.load()) return;
        DisplayLockGuard lock(Board::GetInstance().GetDisplay());
        manager_ = std::make_unique<view::WatchFaceManager>();
        manager_->init();
        // 1s 刷新定时器
        refresh_timer_ = lv_timer_create(&WatchFaceApp::RefreshTimerCb, 1000, this);
        active_.store(true);
        ESP_LOGI(TAG, "Watch face entered");
    }

    void Exit() {
        if (!active_.load()) return;
        DisplayLockGuard lock(Board::GetInstance().GetDisplay());
        if (refresh_timer_) {
            lv_timer_delete(refresh_timer_);
            refresh_timer_ = nullptr;
        }
        manager_.reset();  // 析构 WatchFaceManager → 清理表盘视图
        active_.store(false);
        ESP_LOGI(TAG, "Watch face exited");
    }

    bool IsActive() const { return active_.load(); }

    // 切换表盘样式（B 键下一个 / A 键上一个）
    void NextTheme() {
        if (!active_.load()) return;
        DisplayLockGuard lock(Board::GetInstance().GetDisplay());
        manager_->goNext();
    }
    void PrevTheme() {
        if (!active_.load()) return;
        DisplayLockGuard lock(Board::GetInstance().GetDisplay());
        manager_->goPrevious();
    }

private:
    static constexpr const char* TAG = "WatchFace";

    std::unique_ptr<view::WatchFaceManager> manager_;
    lv_timer_t* refresh_timer_ = nullptr;
    std::atomic<bool> active_{false};

    static void RefreshTimerCb(lv_timer_t* t) {
        auto* self = static_cast<WatchFaceApp*>(lv_timer_get_user_data(t));
        if (self->active_.load() && self->manager_) {
            self->manager_->update();
        }
    }
};

#endif // _WATCH_FACE_H_
