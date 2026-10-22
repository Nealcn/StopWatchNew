#include "lvgl_font.h"
#include <cbin_font.h>
#include <cstdio>
#include <cstdint>


LvglCBinFont::LvglCBinFont(void* data) {
    font_ = cbin_font_create(static_cast<uint8_t*>(data));
}

LvglCBinFont::~LvglCBinFont() {
    if (font_ != nullptr) {
        cbin_font_delete(font_);
    }
}

// assets 分区里的字体是 LVGL binfont 格式（以 "head"/"cmap" 标签开头），
// 必须用 binfont_loader；旧的 cbin_font_create 会把数据当 cbin 解析而失败。
// ⚠️ 不能用 lv_binfont_create_from_buffer()：它内部用 "%c:%zu-%u.%s" 拼 MEMFS 路径，
// 而本项目 CONFIG_NEWLIB_NANO_FORMAT=y —— newlib-nano 的 printf 不支持 %zu，
// 会在 _printf_i 里踩空指针崩溃（LoadProhibited，实测）。
// 这里按 MEMFS 的路径格式自己拼（只用 %c / %lu），再走通用的 lv_binfont_create()。
// 格式见 lv_fs.c:lv_fs_make_path_from_buffer 与 lv_fs_get_buffer_from_path：
//   <驱动器字母>:<缓冲区地址(十进制)>-<长度(十进制)>.<扩展名，解析时被忽略>
static lv_font_t* CreateBinFontFromBuffer(void* data, size_t size) {
    char path[64];
    snprintf(path, sizeof(path), "%c:%lu-%lu.bin",
             static_cast<char>(LV_FS_MEMFS_LETTER),
             static_cast<unsigned long>(reinterpret_cast<uintptr_t>(data)),
             static_cast<unsigned long>(size));
    return lv_binfont_create(path);
}

LvglBinFont::LvglBinFont(void* data, size_t size, const lv_font_t* fallback) {
    font_ = CreateBinFontFromBuffer(data, size);
    if (font_ != nullptr && fallback != nullptr && fallback != font_) {
        // 静态内置字体，生命周期比主题长；lv_binfont_destroy 不释放 fallback
        font_->fallback = fallback;
    }
}

LvglBinFont::~LvglBinFont() {
    if (font_ != nullptr) {
        lv_binfont_destroy(font_);
    }
}