"""文本注入（剪贴板 + SendInput）"""
import ctypes
import ctypes.wintypes
import struct
import time

# Win32 API 常量
CF_UNICODETEXT = 13
GMEM_MOVEABLE = 0x0002
VK_CONTROL = 0x11
VK_V = 0x56
VK_RETURN = 0x0D
INPUT_KEYBOARD = 1
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_UNICODE = 0x0004

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32


def _open_clipboard(hwnd: int = 0) -> bool:
    for _ in range(200):
        if user32.OpenClipboard(ctypes.c_void_p(hwnd)):
            return True
        time.sleep(0.001)
    return False


def _set_clipboard_text(text: str) -> bool:
    if not _open_clipboard():
        return False
    try:
        user32.EmptyClipboard()
        wide = (text + "\0").encode("utf-16-le")
        h_mem = kernel32.GlobalAlloc(GMEM_MOVEABLE, len(wide))
        if not h_mem:
            return False
        p = kernel32.GlobalLock(h_mem)
        if p:
            ctypes.memmove(p, wide, len(wide))
            kernel32.GlobalUnlock(h_mem)
        user32.SetClipboardData(CF_UNICODETEXT, h_mem)
        return True
    finally:
        user32.CloseClipboard()


def copy_to_clipboard(text: str) -> bool:
    """仅复制到剪贴板，不粘贴"""
    return _set_clipboard_text(text)


def _get_clipboard_text() -> str:
    """读取当前剪贴板"""
    if not _open_clipboard():
        return ""
    try:
        h_data = user32.GetClipboardData(CF_UNICODETEXT)
        if not h_data:
            return ""
        p = kernel32.GlobalLock(h_data)
        if not p:
            return ""
        try:
            size = kernel32.GlobalSize(h_data)
            buf = ctypes.create_unicode_buffer(size // 2)
            ctypes.memmove(buf, p, size)
            return buf.value or ""
        finally:
            kernel32.GlobalUnlock(h_data)
    finally:
        user32.CloseClipboard()


def _send_key(vk: int, down: bool):
    """发送按键事件 — 使用 ctypes 结构体（修复 memmove 类型错误）"""
    flags = 0 if down else KEYEVENTF_KEYUP

    class KEYBDINPUT(ctypes.Structure):
        _fields_ = [
            ("wVk", ctypes.wintypes.WORD),
            ("wScan", ctypes.wintypes.WORD),
            ("dwFlags", ctypes.wintypes.DWORD),
            ("time", ctypes.wintypes.DWORD),
            ("dwExtraInfo", ctypes.wintypes.ULONG),
        ]

    class INPUT(ctypes.Structure):
        _fields_ = [
            ("type", ctypes.wintypes.DWORD),
            ("ki", KEYBDINPUT),
        ]

    inp = INPUT()
    inp.type = INPUT_KEYBOARD
    inp.ki.wVk = vk
    inp.ki.wScan = 0
    inp.ki.dwFlags = flags
    inp.ki.time = 0
    inp.ki.dwExtraInfo = 0

    user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def _send_ctrl_v():
    _send_key(VK_CONTROL, True)
    time.sleep(0.02)
    _send_key(VK_V, True)
    time.sleep(0.02)
    _send_key(VK_V, False)
    time.sleep(0.02)
    _send_key(VK_CONTROL, False)


def _send_enter():
    _send_key(VK_RETURN, True)
    time.sleep(0.02)
    _send_key(VK_RETURN, False)


def paste_text(text: str, press_enter: bool = False) -> bool:
    """粘贴文本到当前焦点窗口"""
    old_clip = _get_clipboard_text()

    if not _set_clipboard_text(text):
        return False

    time.sleep(0.03)
    _send_ctrl_v()
    time.sleep(0.05)

    if press_enter:
        time.sleep(0.05)
        _send_enter()

    if old_clip:
        time.sleep(0.1)
        _set_clipboard_text(old_clip)

    return True


def type_text_direct(text: str, press_enter: bool = False) -> bool:
    """直接输入文本到当前光标位置（使用 SendInput Unicode，不操作剪贴板）"""
    class KEYBDINPUT(ctypes.Structure):
        _fields_ = [
            ("wVk", ctypes.wintypes.WORD),
            ("wScan", ctypes.wintypes.WORD),
            ("dwFlags", ctypes.wintypes.DWORD),
            ("time", ctypes.wintypes.DWORD),
            ("dwExtraInfo", ctypes.wintypes.ULONG),
        ]

    class INPUT(ctypes.Structure):
        _fields_ = [
            ("type", ctypes.wintypes.DWORD),
            ("ki", KEYBDINPUT),
        ]

    def _send_unicode_unit(code: int):
        inp = INPUT()
        inp.type = INPUT_KEYBOARD
        inp.ki.wVk = 0
        inp.ki.wScan = code & 0xFFFF
        inp.ki.dwFlags = KEYEVENTF_UNICODE
        inp.ki.time = 0
        inp.ki.dwExtraInfo = 0
        user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
        inp.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP
        user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))

    for ch in text:
        cp = ord(ch)
        if cp < 0x10000:
            _send_unicode_unit(cp)
        else:
            # Surrogate pair for non-BMP characters
            cp -= 0x10000
            _send_unicode_unit(0xD800 + (cp >> 10))
            _send_unicode_unit(0xDC00 + (cp & 0x3FF))
        time.sleep(0.001)  # small delay between characters

    if press_enter:
        import copy
        inp = INPUT()
        inp.type = INPUT_KEYBOARD
        inp.ki.wVk = VK_RETURN
        inp.ki.wScan = 0
        inp.ki.dwFlags = 0
        inp.ki.time = 0
        inp.ki.dwExtraInfo = 0
        user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
        time.sleep(0.02)
        inp.ki.dwFlags = KEYEVENTF_KEYUP
        user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))

    return True
