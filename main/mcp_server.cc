/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <algorithm>
#include <cstring>
#include <esp_pthread.h>

#include "application.h"
#include "display.h"
#include "oled_display.h"
#include "board.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "lvgl_display.h"

#include <sdkconfig.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "MCP"

// ==================== 看板卡片：聊天中"问一句、屏幕出图" ====================
// 服务器按内容渲染成 466×466 PNG（契约见 GET /render），设备下载后铺满屏显示，
// 一直留到用户下次说话/触摸（由 display->SetPreviewImage(nullptr) 收起）。
// 下载走独立任务 + MCP 异步回复，不阻塞协议任务和 Application 主任务。

static constexpr size_t kMaxCardBytes = 512 * 1024;

namespace {

bool IsUrlSafe(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

std::string UrlEncode(const std::string& s) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (IsUrlSafe(c)) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

// http://host:port/xiaozhi/ota/ → http://host:port
std::string OtaUrlToBase(const char* ota_url) {
    std::string u(ota_url ? ota_url : "");
    const std::string suffix = "/xiaozhi/ota/";
    size_t pos = u.find(suffix);
    if (pos != std::string::npos && pos > 0) {
        return u.substr(0, pos);
    }
    return "";
}

bool ProbeBase(const std::string& base) {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (http == nullptr) return false;
    http->SetTimeout(1500);
    if (!http->Open("GET", base + "/board/rev")) return false;
    bool ok = (http->GetStatusCode() == 200);
    http->Close();
    return ok;
}

// 与 OTA 链同源：记住的节点 → 探 LAN → DDNS
// ⚠️ NVS 的 key 硬上限 15 字符，且 Settings::SetString 内部是 ESP_ERROR_CHECK ——
//    key 超长会直接 abort 重启设备。所以这里用自己 9 字符的 key，
//    不去碰 ota.cc 那个 20 字符、本来就写不进去的 ota_url_last_success。
std::string CardBaseUrl() {
    Settings wifi("wifi", true);
    std::string remembered = wifi.GetString("card_base");
    if (!remembered.empty()) return remembered;

    std::string lan = OtaUrlToBase(CONFIG_OTA_LAN_URL);
    std::string ddns = OtaUrlToBase(CONFIG_OTA_URL);
    std::string base = (!lan.empty() && ProbeBase(lan)) ? lan : ddns;
    if (!base.empty()) {
        wifi.SetString("card_base", base);  // 下次免探测
    }
    return base;
}

std::string BuildRenderUrl(const PropertyList& props) {
    std::string title  = props["title"].value<std::string>();
    std::string big    = props["big"].value<std::string>();
    std::string lines  = props["lines"].value<std::string>();
    std::string footer = props["footer"].value<std::string>();
    std::string kind   = props["kind"].value<std::string>();

    if (title.empty() && big.empty() && lines.empty() && footer.empty()) {
        // 什么都没给：显示服务器上当前的看板图
        std::string base = CardBaseUrl();
        return base.empty() ? "" : base + "/board.png";
    }

    if (kind.empty()) kind = "info";
    std::string base = CardBaseUrl();
    if (base.empty()) return "";

    // lines 用 | 分隔；分隔符本身不编码，各段分别编码（服务器按 | 切分后再逐段解码）
    std::string enc_lines;
    size_t start = 0;
    while (true) {
        size_t bar = lines.find('|', start);
        std::string seg = lines.substr(start, (bar == std::string::npos)
                                              ? std::string::npos : bar - start);
        if (!seg.empty()) {
            if (!enc_lines.empty()) enc_lines += '|';
            enc_lines += UrlEncode(seg);
        }
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    if (enc_lines.empty()) {
        // 只给了 lines 之外的内容但 lines 为空，或给的行全是空串：当没有处理
        enc_lines.clear();
    }

    std::string url = base + "/render?kind=" + UrlEncode(kind);
    if (!title.empty())  url += "&title="  + UrlEncode(title);
    if (!big.empty())    url += "&big="    + UrlEncode(big);
    if (!enc_lines.empty()) url += "&lines=" + enc_lines;
    if (!footer.empty()) url += "&footer=" + UrlEncode(footer);
    return url;
}

// McpServer::ReplyResult 是「原样拼接」result，不加引号也不转义 —— 传裸文本会拼出
// 非法 JSON（"result":图片已显示在屏幕上），服务器解析不了，整轮对话就断在这里
// （症状：图出来了但助手不再出声）。
// 形状必须与 McpTool::Call() 一致：{"content":[{"type":"text","text":"..."}],"isError":false}
std::string McpTextResult(const std::string& text) {
    cJSON* result = cJSON_CreateObject();
    cJSON* content = cJSON_CreateArray();
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text.c_str());
    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(result, "content", content);
    cJSON_AddBoolToObject(result, "isError", false);
    char* s = cJSON_PrintUnformatted(result);
    std::string out(s != nullptr ? s : "{}");
    if (s != nullptr) cJSON_free(s);
    cJSON_Delete(result);
    return out;
}

struct CardFetchArgs {
    LvglDisplay* display;
    std::string url;
};

void CardFetchTask(void* arg) {
    auto* a = static_cast<CardFetchArgs*>(arg);
    bool ok = false;
    std::string err;

    do {
        auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
        if (http == nullptr) { err = "无法创建 HTTP"; break; }
        http->SetTimeout(10000);
        if (!http->Open("GET", a->url)) { err = "连不上服务器"; break; }
        int code = http->GetStatusCode();
        if (code != 200) { http->Close(); err = "HTTP " + std::to_string(code); break; }

        size_t len = http->GetBodyLength();
        if (len == 0 || len > kMaxCardBytes) {
            http->Close();
            err = "长度异常 " + std::to_string(len);
            break;
        }
        auto* data = static_cast<char*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM));
        if (data == nullptr) { http->Close(); err = "内存不足"; break; }

        size_t got = 0;
        while (got < len) {
            int n = http->Read(data + got, len - got);
            if (n <= 0) break;
            got += n;
        }
        http->Close();
        if (got != len) { heap_caps_free(data); err = "下载中断"; break; }

        try {
            auto image = std::make_unique<LvglAllocatedImage>(data, len);
            a->display->SetCardMode(true);
            a->display->SetPreviewImage(std::move(image));
            ok = true;
        } catch (const std::exception& e) {
            heap_caps_free(data);  // 构造抛异常时析构不会跑
            err = std::string("解码失败：") + e.what();
        }
    } while (false);

    auto& mcp = McpServer::GetInstance();
    if (ok) {
        ESP_LOGI(TAG, "show_card: %s", a->url.c_str());
        // 回给服务器的一律 ASCII：中文只在串口日志里，避免再踩编码的坑
        mcp.ReplyAsyncResult(McpTextResult("ok"));
    } else {
        ESP_LOGE(TAG, "show_card failed: %s (%s)", err.c_str(), a->url.c_str());
        mcp.ReplyAsyncResult(McpTextResult("failed"));
    }
    delete a;
    vTaskDelete(nullptr);
}

}  // namespace

McpServer::McpServer() {
}

McpServer::~McpServer() {
    for (auto tool : tools_) {
        delete tool;
    }
    tools_.clear();
}

void McpServer::AddCommonTools() {
    // *Important* To speed up the response time, we add the common tools to the beginning of
    // the tools list to utilize the prompt cache.
    // **重要** 为了提升响应速度，我们把常用的工具放在前面，利用 prompt cache 的特性。

    // Backup the original tools list and restore it after adding the common tools.
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();

    // Do not add custom tools here.
    // Custom tools must be added in the board's InitializeTools function.

    AddTool("self.get_device_status",
        "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
        "Use this tool for: \n"
        "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
        "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            return board.GetDeviceStatusJson();
        });

    AddTool("self.audio_speaker.set_volume", 
        "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.",
        PropertyList({
            Property("volume", kPropertyTypeInteger, 0, 100)
        }), 
        [&board](const PropertyList& properties) -> ReturnValue {
            auto codec = board.GetAudioCodec();
            codec->SetOutputVolume(properties["volume"].value<int>());
            return true;
        });
    
    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.screen.set_brightness",
            "Set the brightness of the screen.",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }),
            [backlight](const PropertyList& properties) -> ReturnValue {
                uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                backlight->SetBrightness(brightness, true);
                return true;
            });
    }

#ifdef HAVE_LVGL
    auto display = board.GetDisplay();
    if (display && display->GetTheme() != nullptr) {
        AddTool("self.screen.set_theme",
            "Set the theme of the screen. The theme can be `light` or `dark`.",
            PropertyList({
                Property("theme", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto theme_name = properties["theme"].value<std::string>();
                auto& theme_manager = LvglThemeManager::GetInstance();
                auto theme = theme_manager.GetTheme(theme_name);
                if (theme != nullptr) {
                    display->SetTheme(theme);
                    return true;
                }
                return false;
            });
    }

    auto camera = board.GetCamera();
    if (camera) {
        AddTool("self.camera.take_photo",
            "Always remember you have a camera. If the user asks you to see something, use this tool to take a photo and then explain it.\n"
            "Args:\n"
            "  `question`: The question that you want to ask about the photo.\n"
            "Return:\n"
            "  A JSON object that provides the photo information.",
            PropertyList({
                Property("question", kPropertyTypeString)
            }),
            [camera](const PropertyList& properties) -> ReturnValue {
                // Lower the priority to do the camera capture
                TaskPriorityReset priority_reset(1);

                if (!camera->Capture()) {
                    throw std::runtime_error("Failed to capture photo");
                }
                auto question = properties["question"].value<std::string>();
                return camera->Explain(question);
            });
    }
#endif

    // Restore the original tools list to the end of the tools list
    tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
}

void McpServer::AddUserOnlyTools() {
    // System tools
    AddUserOnlyTool("self.get_system_info",
        "Get the system information",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& board = Board::GetInstance();
            return board.GetSystemInfoJson();
        });

    AddUserOnlyTool("self.reboot", "Reboot the system",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            app.Schedule([&app]() {
                ESP_LOGW(TAG, "User requested reboot");
                vTaskDelay(pdMS_TO_TICKS(1000));

                app.Reboot();
            });
            return true;
        });

    // 震动探针：模型不可见，只能经 /mcp/device/call 驱动 —— 用来验证马达 PWM 通道
    AddUserOnlyTool("self.motor.vibrate", "Vibrate the motor briefly",
        PropertyList({
            Property("duration", kPropertyTypeInteger, 10, 3000),   // ms
            Property("strength", kPropertyTypeInteger, 0, 100)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            Board::GetInstance().Vibrate(
                (uint16_t)properties["duration"].value<int>(),
                (uint8_t)properties["strength"].value<int>());
            return true;
        });

    // Firmware upgrade
    AddUserOnlyTool("self.upgrade_firmware", "Upgrade firmware from a specific URL. This will download and install the firmware, then reboot the device.",
        PropertyList({
            Property("url", kPropertyTypeString, "The URL of the firmware binary file to download and install")
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto url = properties["url"].value<std::string>();
            ESP_LOGI(TAG, "User requested firmware upgrade from URL: %s", url.c_str());
            
            auto& app = Application::GetInstance();
            app.Schedule([url, &app]() {
                bool success = app.UpgradeFirmware(url);
                if (!success) {
                    ESP_LOGE(TAG, "Firmware upgrade failed");
                }
            });
            
            return true;
        });

    // Display control
#ifdef HAVE_LVGL
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display) {
        AddUserOnlyTool("self.screen.get_info", "Information about the screen, including width, height, etc.",
            PropertyList(),
            [display](const PropertyList& properties) -> ReturnValue {
                cJSON *json = cJSON_CreateObject();
                cJSON_AddNumberToObject(json, "width", display->width());
                cJSON_AddNumberToObject(json, "height", display->height());
                if (dynamic_cast<OledDisplay*>(display)) {
                    cJSON_AddBoolToObject(json, "monochrome", true);
                } else {
                    cJSON_AddBoolToObject(json, "monochrome", false);
                }
                return json;
            });

#if CONFIG_LV_USE_SNAPSHOT
        
        AddUserOnlyTool("self.screen.preview_image", "Preview an image on the screen",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);

                if (!http->Open("GET", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                int status_code = http->GetStatusCode();
                if (status_code != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(status_code));
                }

                size_t content_length = http->GetBodyLength();
                char* data = (char*)heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
                if (data == nullptr) {
                    throw std::runtime_error("Failed to allocate memory for image: " + url);
                }
                size_t total_read = 0;
                while (total_read < content_length) {
                    int ret = http->Read(data + total_read, content_length - total_read);
                    if (ret < 0) {
                        heap_caps_free(data);
                        throw std::runtime_error("Failed to download image: " + url);
                    }
                    if (ret == 0) {
                        break;
                    }
                    total_read += ret;
                }
                http->Close();

                auto image = std::make_unique<LvglAllocatedImage>(data, content_length);
                display->SetPreviewImage(std::move(image));
                return true;
            });
#endif // CONFIG_LV_USE_SNAPSHOT

        // 看板卡片：聊天中"问一句、屏幕出图"。与上面 preview_image 的区别：
        // ①对 LLM 可见（AddTool 而非 AddUserOnlyTool）②铺满屏 ③不自动消失
        // ④回调里不下载（下载走 CardFetchTask + 异步回复，否则会卡住 Application 主任务）
        auto* card_tool = AddTool("self.screen.show_card",
            "在手表屏幕上显示一张图片卡片，并一直留到用户下次说话或触摸屏幕。\n"
            "当用户想「看」某样东西时使用：天气、题目、单词、日程、图表、清单等。\n"
            "给 title/big/lines/footer 由服务器渲染成卡片；只给 url 则直接显示该地址的图片；\n"
            "什么都不给则显示服务器上当前的看板图。\n"
            "Args:\n"
            "  `title`: 标题（顶部小字）\n"
            "  `big`: 主文案（居中大号字，如 `13°`）\n"
            "  `lines`: 若干行小字，用 | 分隔\n"
            "  `footer`: 底部一行小字\n"
            "  `kind`: 版式，info / math / hanzi，默认 info\n"
            "  `url`: 直接指定的图片地址（给了就忽略上面的内容参数）\n"
            "Return:\n"
            "  显示结果。",
            PropertyList({
                Property("title",  kPropertyTypeString, std::string("")),
                Property("big",    kPropertyTypeString, std::string("")),
                Property("lines",  kPropertyTypeString, std::string("")),
                Property("footer", kPropertyTypeString, std::string("")),
                Property("kind",   kPropertyTypeString, std::string("")),
                Property("url",    kPropertyTypeString, std::string(""))
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                std::string url = properties["url"].value<std::string>();
                if (url.empty()) {
                    url = BuildRenderUrl(properties);
                }
                if (url.empty()) {
                    // 异常消息也会原样拼进 JSON（ReplyError），保持 ASCII
                    throw std::runtime_error("show_card: cannot resolve image url");
                }
                auto* args = new CardFetchArgs{display, url};
                if (xTaskCreateWithCaps(CardFetchTask, "card_fetch", 8192, args, 3, nullptr,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
                    delete args;
                    throw std::runtime_error("show_card: task create failed");
                }
                return std::string(kAsyncMarker);  // 已异步启动，等下由 ReplyAsyncResult 回复
            });
        if (card_tool != nullptr) {
            card_tool->set_async(true);
        }
    }
#endif // HAVE_LVGL

    // Assets download url (always registered — Settings storage works regardless of partition layout)
    AddUserOnlyTool("self.assets.set_download_url", "Set the download url for the assets",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                Settings settings("assets", true);
                settings.SetString("download_url", url);
                return true;
            });
}

void McpServer::AddTool(McpTool* tool) {
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool* t) { return t->name() == tool->name(); }) != tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
    tools_.push_back(tool);
}

McpTool* McpServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    auto tool = new McpTool(name, description, properties, callback);
    AddTool(tool);
    return tool;
}

void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    auto tool = new McpTool(name, description, properties, callback);
    tool->set_user_only(true);
    AddTool(tool);
}

void McpServer::ParseMessage(const std::string& message) {
    cJSON* json = cJSON_Parse(message.c_str());
    if (json == nullptr) {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision)) {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url)) {
            auto camera = Board::GetInstance().GetCamera();
            if (camera) {
                std::string url_str = std::string(url->valuestring);
                std::string token_str;
                if (cJSON_IsString(token)) {
                    token_str = std::string(token->valuestring);
                }
                camera->SetExplainUrl(url_str, token_str);
            }
        }
    }
}

void McpServer::ParseMessage(const cJSON* json) {
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }
    
    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "Missing method");
        return;
    }
    
    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0) {
        return;
    }
    
    // Check params
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;
    
    if (method_str == "initialize") {
        if (cJSON_IsObject(params)) {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities)) {
                ParseCapabilities(capabilities);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message);
    } else if (method_str == "tools/list") {
        std::string cursor_str = "";
        bool list_user_only_tools = false;
        if (params != nullptr) {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor)) {
                cursor_str = std::string(cursor->valuestring);
            }
            auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
            if (cJSON_IsBool(with_user_tools)) {
                list_user_only_tools = with_user_tools->valueint == 1;
            }
        }
        GetToolsList(id_int, cursor_str, list_user_only_tools);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, "Invalid arguments");
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments);
    } else {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
}

void McpServer::ReplyResult(int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::ReplyError(int id, const std::string& message) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools) {
    const int max_payload_size = 8000;
    std::string json = "{\"tools\":[";
    
    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";
    
    while (it != tools_.end()) {
        // 如果我们还没有找到起始位置，继续搜索
        if (!found_cursor) {
            if ((*it)->name() == cursor) {
                found_cursor = true;
            } else {
                ++it;
                continue;
            }
        }

        if (!list_user_only_tools && (*it)->user_only()) {
            ++it;
            continue;
        }
        
        // 添加tool前检查大小
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size) {
            // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
            next_cursor = (*it)->name();
            break;
        }
        
        json += tool_json;
        ++it;
    }
    
    if (json.back() == ',') {
        json.pop_back();
    }
    
    if (json.back() == '[' && !tools_.empty()) {
        // 如果没有添加任何tool，返回错误
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }
    
    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments) {
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(), 
                                 [&tool_name](const McpTool* tool) { 
                                     return tool->name() == tool_name; 
                                 });
    
    if (tool_iter == tools_.end()) {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    try {
        for (auto& argument : arguments) {
            bool found = false;
            if (cJSON_IsObject(tool_arguments)) {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                    argument.set_value<int>(value->valueint);
                    found = true;
                } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                }
            }

            if (!argument.has_default_value() && !found) {
                ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                ReplyError(id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "tools/call: %s", e.what());
        ReplyError(id, e.what());
        return;
    }

    // Use main thread to call the tool
    auto& app = Application::GetInstance();
    app.Schedule([this, id, tool_iter, arguments = std::move(arguments)]() {
        try {
            std::string result = (*tool_iter)->Call(arguments);
            // 异步工具:回调返回 kAsyncMarker 表示已异步启动,挂起本调用稍后由外部回复
            if ((*tool_iter)->async() && result.find(McpServer::kAsyncMarker) != std::string::npos) {
                pending_async_.push(id);
                return;
            }
            ReplyResult(id, result);
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "tools/call: %s", e.what());
            ReplyError(id, e.what());
        }
    });
}

bool McpServer::ReplyAsyncResult(const std::string& result) {
    if (pending_async_.empty()) {
        ESP_LOGW(TAG, "ReplyAsyncResult: no pending async tool call");
        return false;
    }
    int id = pending_async_.front();
    pending_async_.pop();
    ReplyResult(id, result);
    return true;
}
