#pragma once

#include <lvgl.h>


class LvglFont {
public:
    virtual const lv_font_t* font() const = 0;
    virtual ~LvglFont() = default;
};

// Built-in font
class LvglBuiltInFont : public LvglFont {
public:
    LvglBuiltInFont(const lv_font_t* font) : font_(font) {}
    virtual const lv_font_t* font() const override { return font_; }

private:
    const lv_font_t* font_;
};


class LvglCBinFont : public LvglFont {
public:
    LvglCBinFont(void* data);
    virtual ~LvglCBinFont();
    virtual const lv_font_t* font() const override { return font_; }

private:
    lv_font_t* font_;
};

// LVGL binfont 格式（assets 分区里的 font_*.bin 就是这种；cbin 加载器读不了）。
// 数据通过 MEMFS 读取，不要求 buffer 对齐；loader 会把所有表深拷贝到 LVGL 堆，
// 所以 buffer 只需在构造期间有效（之后解除 mmap 也不影响字体）。
class LvglBinFont : public LvglFont {
public:
    // fallback：新字体没覆盖到的字形（assets 的 common 字体缺「聆」「浏」等）回退到它。
    // 传静态内置字体即可——lv_binfont_destroy 不会释放 fallback。
    LvglBinFont(void* data, size_t size, const lv_font_t* fallback = nullptr);
    virtual ~LvglBinFont();
    virtual const lv_font_t* font() const override { return font_; }

private:
    lv_font_t* font_ = nullptr;
};
