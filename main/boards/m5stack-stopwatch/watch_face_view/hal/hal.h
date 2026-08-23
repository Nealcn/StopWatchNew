// 兼容 stub：D:\stopwatch 的 GetHAL()（表盘/设置视图使用）
// 适配本固件：亮度→Backlight、音量→AudioCodec、时间→settimeofday
#pragma once
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include <esp_log.h>
#include <esp_timer.h>
#include <sys/time.h>
#include <lvgl.h>

#include "board.h"
#include "audio/audio_codec.h"

struct TimeHms {
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
    bool isValid() const { return true; }
};

struct DateYmd {
    uint16_t year = 2026;
    uint8_t month = 1;
    uint8_t day = 1;
    static bool isLeapYear(uint16_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
    static uint8_t daysInMonth(uint16_t y, uint8_t m) {
        static const uint8_t d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        if (m == 2 && isLeapYear(y)) return 29;
        return d[m - 1];
    }
    bool isValid() const { return year >= 2020 && month >= 1 && month <= 12 && day >= 1 && day <= daysInMonth(year, month); }
};

// ButtonConfig（ButtonWorker 声明需要；本固件菜单未使用按键设置）
class Hal {
public:
    struct ButtonConfig {
        bool sfxEnabled = true;
        bool vibrateEnabled = true;
    };
};

class HalStub {
public:
    uint32_t millis() const { return (uint32_t)(esp_timer_get_time() / 1000); }
    lv_indev_t* lvTouchpad = nullptr;
    Hal::ButtonConfig btn_config_;

    // 按键配置（ButtonWorker 使用；本固件菜单未提供按键设置，仅存储）
    void setButtonConfig(Hal::ButtonConfig config, bool saveToSettings = false) { btn_config_ = config; }
    const Hal::ButtonConfig& getButtonConfig(bool loadFromSettings = false) { return btn_config_; }

    // 亮度
    void setBackLightBrightness(int brightness, bool saveToSettings = false) {
        auto* bl = Board::GetInstance().GetBacklight();
        if (bl) bl->SetBrightness((uint8_t)brightness, saveToSettings);
    }
    int getBackLightBrightness(bool loadFromSettings = false) {
        auto* bl = Board::GetInstance().GetBacklight();
        return bl ? bl->brightness() : 0;
    }

    // 音量
    void setSpeakerVolume(int volume, bool saveToSettings = false) {
        auto* codec = Board::GetInstance().GetAudioCodec();
        if (codec) codec->SetOutputVolume(volume);
    }
    int getSpeakerVolume(bool loadFromSettings = false) {
        auto* codec = Board::GetInstance().GetAudioCodec();
        return codec ? codec->output_volume() : 0;
    }

    // 时间/日期（settimeofday + time()）
    TimeHms getTimeHms() {
        TimeHms t;
        time_t now = time(nullptr);
        struct tm* tm = localtime(&now);
        if (tm && tm->tm_year >= 2025 - 1900) {
            t.hour = tm->tm_hour;
            t.minute = tm->tm_min;
            t.second = tm->tm_sec;
        }
        return t;
    }
    bool setTimeHms(const TimeHms& time) {
        return ApplyTime(time.hour, time.minute, time.second);
    }
    DateYmd getDateYmd() {
        DateYmd d;
        time_t now = time(nullptr);
        struct tm* tm = localtime(&now);
        if (tm && tm->tm_year >= 2025 - 1900) {
            d.year = tm->tm_year + 1900;
            d.month = tm->tm_mon + 1;
            d.day = tm->tm_mday;
        }
        return d;
    }
    bool setDateYmd(const DateYmd& date) {
        return ApplyTime(date.year, date.month, date.day, true);
    }

private:
    bool ApplyTime(int h, int m, int s, bool date_only = false) {
        time_t now = time(nullptr);
        struct tm tm;
        localtime_r(&now, &tm);
        if (date_only) {
            tm.tm_year = h - 1900;
            tm.tm_mon = m - 1;
            tm.tm_mday = s;
        } else {
            tm.tm_hour = h;
            tm.tm_min = m;
            tm.tm_sec = s;
        }
        struct timeval tv = {};
        tv.tv_sec = mktime(&tm);
        return settimeofday(&tv, nullptr) == 0;
    }
};

inline HalStub& GetHAL() {
    static HalStub hal;
    return hal;
}
