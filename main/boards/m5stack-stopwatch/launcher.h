// 主页面（Launcher 主菜单）：手动索引 + 位移动画（图标永远居中，无滚动边缘问题）
// 交互：按键 1/2 切换功能，触摸点击进入
#ifndef _LAUNCHER_H_
#define _LAUNCHER_H_

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <lvgl.h>

#include "board.h"
#include "display/display.h"
#include "ui_font.h"
#include "icon_ai_chat.h"
#include "icon_voicecube.h"
#include "icon_watch_face.h"
#include "icon_setup.h"

class LauncherScreen {
public:
    // app 索引
    static constexpr int kAppAiChat = 0;
    static constexpr int kAppVoiceInput = 1;
    static constexpr int kAppWatchFace = 2;
    static constexpr int kAppSetup = 3;

    LauncherScreen() = default;

    // app 点击回调（int app_id；在 LVGL 任务上下文触发，回调内不要直接做 LVGL 操作，
    // 建议用 Application::Schedule 转发到主任务）
    void SetOnAppClicked(std::function<void(int)> cb) { on_app_clicked_ = std::move(cb); }

    // 显示/隐藏主页面。线程安全：内部持 DisplayLockGuard，可被按键回调任务调用。
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
            lv_obj_move_to_index(panel_, lv_obj_get_child_cnt(lv_screen_active()) - 1);
            // 面板 gap 6px 导致画面右移，图标 x 补偿 -6
        } else {
            lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
        }
        visible_.store(visible);
    }

    bool IsVisible() const { return visible_.load(); }

    // 按键切换功能（上/下一个）
    void Next() { MoveTo(current_index_ + 1); }
    void Prev() { MoveTo(current_index_ - 1); }

private:
    static constexpr int kIconGap = 466;
    static constexpr int kMoveAnimMs = 250;
    // 图标 200x200 在 466 屏居中（esp_lcd gap=6 已在驱动层消化，无需补偿）
    static constexpr int kIconCenterX = (466 - 200) / 2;
    static constexpr int kIconCenterY = (466 - 200) / 2;

    void MoveTo(int index) {
        if (!created_ || !visible_.load()) return;
        int count = (int)icon_holders_.size();
        if (count == 0) return;
        current_index_ = ((index % count) + count) % count;
        DisplayLockGuard lock(Board::GetInstance().GetDisplay());
        // 所有图标平移动画（绝对坐标）：当前选中的居中
        for (int i = 0; i < count; i++) {
            int target_x = (i - current_index_) * kIconGap + kIconCenterX;
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, icon_holders_[i]);
            lv_anim_set_exec_cb(&a, [](void* var, int32_t v) {
                lv_obj_set_x(static_cast<lv_obj_t*>(var), v);
            });
            lv_anim_set_values(&a, lv_obj_get_x(icon_holders_[i]), target_x);
            lv_anim_set_time(&a, kMoveAnimMs);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_start(&a);
        }
    }

    void Create() {
        lv_obj_t* screen = lv_screen_active();

        panel_ = lv_obj_create(screen);
        lv_obj_set_size(panel_, 466, 466);
        lv_obj_align(panel_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(panel_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_border_width(panel_, 0, 0);
        lv_obj_set_style_radius(panel_, 0, 0);
        lv_obj_set_style_pad_all(panel_, 0, 0);  // 关键：默认主题 card 样式带 16px padding，不清则所有子对象坐标偏右下 16px
        lv_obj_clear_flag(panel_, LV_OBJ_FLAG_SCROLLABLE);

        AddApp(&icon_ai_chat, "AI 对话", kAppAiChat);
        AddApp(&icon_voicecube, "语音输入", kAppVoiceInput);
        AddApp(&icon_watch_face, "表盘", kAppWatchFace);
        AddApp(&icon_setup, "设置", kAppSetup);

        lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
        created_ = true;
    }

    void AddApp(const lv_image_dsc_t* icon, const char* name, int app_id) {
        lv_obj_t* holder = lv_obj_create(panel_);
        lv_obj_set_size(holder, 200, 200);
        // 绝对坐标定位（与动画一致，保证点击命中区域 = 显示位置）
        lv_obj_set_pos(holder, app_id * kIconGap + kIconCenterX, kIconCenterY);
        lv_obj_set_style_bg_opa(holder, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(holder, 0, 0);
        lv_obj_clear_flag(holder, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* img = lv_img_create(holder);
        lv_image_set_src(img, icon);
        lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);

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
    std::vector<lv_obj_t*> icon_holders_;
    std::vector<std::unique_ptr<AppClickCtx>> click_ctxs_;
    std::function<void(int)> on_app_clicked_;
    std::atomic<bool> visible_{false};
    int current_index_ = 0;
    bool created_ = false;
};

#endif // _LAUNCHER_H_
