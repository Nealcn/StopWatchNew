// 主页面（Launcher 主菜单）：参考 D:\stopwatch 项目 app_launcher 形态简化实现
// 形态：全屏黑底 + 水平滚动 app 图标行 + 名称标签
// （多 app 无限滚动参考 D:\stopwatch\main\apps\app_launcher\view\view.cpp：
//   LV_SCROLL_SNAP_CENTER + 5 份拷贝 + 边界传送回绕）
#ifndef _LAUNCHER_H_
#define _LAUNCHER_H_

#include <atomic>
#include <functional>
#include <vector>

#include <lvgl.h>

#include "board.h"
#include "display/display.h"
#include "lvgl_theme.h"
#include "icon_ai_chat.h"
#include "icon_voicecube.h"

// 中文字体（已编译进固件，lcd_display.cc 同符号）
LV_FONT_DECLARE(font_puhui_basic_20_4);

class LauncherScreen {
public:
    // app 索引
    static constexpr int kAppAiChat = 0;
    static constexpr int kAppVoiceInput = 1;

    LauncherScreen() = default;

    // app 点击回调（int app_id；在 LVGL 任务上下文触发，回调内不要直接做 LVGL 操作，
    // 建议用 Application::Schedule 转发到主任务）
    void SetOnAppClicked(std::function<void(int)> cb) { on_app_clicked_ = std::move(cb); }

    // 显示/隐藏主页面。线程安全：内部持 DisplayLockGuard，可被按键回调任务调用。
    // 首次调用时 lazy-create（须在 Display::SetupUI 之后，由调用点保证）。
    void SetVisible(bool visible) {
        Display* display = Board::GetInstance().GetDisplay();
        DisplayLockGuard lock(display);
        if (!created_) {
            Create();
        }
        if (visible == visible_.load()) {
            return;
        }
        if (visible) {
            lv_obj_remove_flag(panel_, LV_OBJ_FLAG_HIDDEN);
            // 置顶，盖住对话 UI
            lv_obj_move_to_index(panel_, lv_obj_get_child_cnt(lv_screen_active()) - 1);
        } else {
            lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
        }
        visible_.store(visible);
    }

    bool IsVisible() const { return visible_.load(); }

private:
    void Create() {
        lv_obj_t* screen = lv_screen_active();

        // 全屏黑色面板（水平滚动图标行）
        panel_ = lv_obj_create(screen);
        lv_obj_set_size(panel_, 466, 466);
        lv_obj_align(panel_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(panel_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_border_width(panel_, 0, 0);
        lv_obj_set_style_radius(panel_, 0, 0);
        lv_obj_set_style_pad_all(panel_, 0, 0);
        lv_obj_set_scrollbar_mode(panel_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_scroll_dir(panel_, LV_DIR_HOR);
        lv_obj_add_flag(panel_, LV_OBJ_FLAG_SCROLL_ONE);
        lv_obj_set_scroll_snap_x(panel_, LV_SCROLL_SNAP_CENTER);

        // 应用图标（间距 = 屏宽 466，居中显示当前 app）
        AddApp(&icon_ai_chat, "AI 对话", kAppAiChat);
        AddApp(&icon_voicecube, "语音输入", kAppVoiceInput);

        // 名称标签（跟随滚动显示当前图标名——简单版固定显示第一个，多 app 滚动标签后续扩展）
        label_ = lv_label_create(panel_);
        lv_obj_add_flag(label_, LV_OBJ_FLAG_FLOATING);
        // 完整中文字体（basic 版缺字）
        auto* theme = static_cast<LvglTheme*>(Board::GetInstance().GetDisplay()->GetTheme());
        const lv_font_t* text_font = (theme && theme->text_font() && theme->text_font()->font())
                                         ? theme->text_font()->font()
                                         : &font_puhui_basic_20_4;
        lv_obj_set_style_text_font(label_, text_font, 0);
        lv_obj_set_style_text_color(label_, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(label_, "AI 对话");
        lv_obj_align(label_, LV_ALIGN_CENTER, 0, 155);

        // 初始隐藏，由 SetVisible 控制
        lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
        created_ = true;
    }

    void AddApp(const lv_image_dsc_t* icon, const char* name, int app_id) {
        // 图标容器（200x200，透明）
        lv_obj_t* holder = lv_obj_create(panel_);
        lv_obj_set_size(holder, 200, 200);
        lv_obj_align(holder, LV_ALIGN_CENTER, app_id * 466, -15);
        lv_obj_set_style_bg_opa(holder, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(holder, 0, 0);
        lv_obj_clear_flag(holder, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* img = lv_img_create(holder);
        lv_image_set_src(img, icon);
        lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);

        // 点击进入 app（触摸屏）
        auto ctx = std::make_unique<AppClickCtx>();
        ctx->self = this;
        ctx->app_id = app_id;
        lv_obj_add_flag(holder, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(holder, &LauncherScreen::OnIconClicked, LV_EVENT_CLICKED, ctx.get());
        click_ctxs_.push_back(std::move(ctx));

        icon_holders_.push_back(holder);
    }

    struct AppClickCtx {
        LauncherScreen* self;
        int app_id;
    };

    static void OnIconClicked(lv_event_t* e) {
        auto* ctx = static_cast<AppClickCtx*>(lv_event_get_user_data(e));
        if (ctx->self->on_app_clicked_) {
            ctx->self->on_app_clicked_(ctx->app_id);
        }
    }

    lv_obj_t* panel_ = nullptr;
    lv_obj_t* label_ = nullptr;
    std::vector<lv_obj_t*> icon_holders_;
    std::vector<std::unique_ptr<AppClickCtx>> click_ctxs_;
    std::function<void(int)> on_app_clicked_;
    std::atomic<bool> visible_{false};
    bool created_ = false;
};

#endif // _LAUNCHER_H_
