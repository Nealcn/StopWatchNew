// 运行时中文字体（assets 加载的 GB2312 全字库 font_puhui_common_20_4.bin）
#ifndef _UI_FONT_H_
#define _UI_FONT_H_

#include <lvgl.h>
#include "board.h"
#include "lvgl_theme.h"

inline const lv_font_t* GetTextFont() {
    auto* display = Board::GetInstance().GetDisplay();
    auto* theme = static_cast<LvglTheme*>(display->GetTheme());
    if (theme && theme->text_font() && theme->text_font()->font()) {
        return theme->text_font()->font();
    }
    return nullptr;
}

#endif // _UI_FONT_H_
