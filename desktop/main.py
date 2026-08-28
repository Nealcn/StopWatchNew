#!/usr/bin/env python3
"""VoiceStick 桌面端 — 将 AtomS3R 语音速记识别的文字自动粘贴到当前窗口"""

import sys
import os
import asyncio
import logging
import traceback


def _excepthook(exc_type, exc, tb):
    """全局未捕获异常兜底：写入 crash.log 并打印，避免静默消失"""
    text = "".join(traceback.format_exception(exc_type, exc, tb))
    logging.critical("未捕获异常导致退出:\n%s", text)
    try:
        log_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "crash.log")
        with open(log_path, "a", encoding="utf-8") as f:
            f.write("=== 崩溃时间 ===\n")
            f.write(text + "\n")
    except Exception:
        pass
    sys.__excepthook__(exc_type, exc, tb)


# 兜底所有未捕获异常（PyQt5 槽内异常会直接 terminate 进程，先落盘再退出）
sys.excepthook = _excepthook

# Windows BLE (bleak) 需要 Selector 事件循环
if sys.platform == "win32":
    asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())

# 确保能找到 voicestick 包
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PyQt5.QtWidgets import QApplication
from PyQt5.QtCore import Qt
from PyQt5.QtGui import QFont

from voicestick.app import VoiceStickApp


def main():
    # 日志
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
        handlers=[
            logging.StreamHandler(),
        ],
    )

    # 高 DPI 支持
    QApplication.setHighDpiScaleFactorRoundingPolicy(Qt.HighDpiScaleFactorRoundingPolicy.PassThrough)
    QApplication.setAttribute(Qt.AA_EnableHighDpiScaling, True)

    app = QApplication(sys.argv)
    app.setApplicationName("Voice Cube")
    app.setQuitOnLastWindowClosed(False)  # 托盘常驻

    # 设置中文字体
    font = QFont("Microsoft YaHei", 9)
    app.setFont(font)

    # 启动
    vs = VoiceStickApp(app)
    vs.start()

    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
