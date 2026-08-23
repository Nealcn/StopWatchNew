// 兼容 stub：D:\stopwatch 的 mooncake_log（转 ESP_LOG，C 变参实现）
#pragma once
#include <cstdarg>
#include <cstdio>
#include <string_view>
#include <esp_log.h>

namespace mclog {
inline void tagInfo(const std::string_view tag, const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOGI(tag.data(), "%s", buf);
}
inline void tagWarn(const std::string_view tag, const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOGW(tag.data(), "%s", buf);
}
inline void tagError(const std::string_view tag, const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOGE(tag.data(), "%s", buf);
}
}  // namespace mclog
