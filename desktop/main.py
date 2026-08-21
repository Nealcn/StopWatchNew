#!/usr/bin/env python3
"""VoiceStick 桌面端 — 将 AtomS3R 语音速记识别的文字自动粘贴到当前窗口"""

import sys
import os
import asyncio
import logging

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
