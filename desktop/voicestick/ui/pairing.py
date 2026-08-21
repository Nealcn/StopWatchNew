"""设备配对对话框"""
import asyncio
import threading
import sys

from PyQt5.QtWidgets import (
    QDialog, QVBoxLayout, QListWidget, QPushButton,
    QHBoxLayout, QLabel, QListWidgetItem,
)
from PyQt5.QtCore import Qt, QTimer, pyqtSignal, QObject


class _ScanWorker(QObject):
    """在后台线程中运行 BLE 扫描"""
    finished = pyqtSignal(list)
    error = pyqtSignal(str)

    def run(self):
        try:
            # Windows BLE 需要 Selector 事件循环
            if sys.platform == "win32":
                asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
            loop = asyncio.new_event_loop()
            asyncio.set_event_loop(loop)

            from ..ble import BleClient
            ble = BleClient()
            devices = loop.run_until_complete(ble.scan(3.0))
            loop.close()
            self.finished.emit(devices)
        except Exception as e:
            self.error.emit(str(e))


class PairingDialog(QDialog):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._devices: list[dict] = []
        self.selected_address = ""
        self.selected_name = ""
        self._worker_thread = None
        self._worker = None

        self.setWindowTitle("配对设备")
        self.setFixedSize(420, 340)
        self._setup_ui()
        self._start_scan()

    def _setup_ui(self):
        layout = QVBoxLayout(self)

        self._hint = QLabel("正在扫描 Voice Cube 设备…")
        self._hint.setAlignment(Qt.AlignCenter)
        layout.addWidget(self._hint)

        self._list = QListWidget()
        self._list.setAlternatingRowColors(True)
        layout.addWidget(self._list)

        btn_layout = QHBoxLayout()
        self._scan_btn = QPushButton("重新扫描")
        self._scan_btn.clicked.connect(self._start_scan)
        self._pair_btn = QPushButton("配对")
        self._pair_btn.clicked.connect(self._pair)
        self._pair_btn.setEnabled(False)
        cancel_btn = QPushButton("取消")
        cancel_btn.clicked.connect(self.reject)

        btn_layout.addWidget(self._scan_btn)
        btn_layout.addStretch()
        btn_layout.addWidget(self._pair_btn)
        btn_layout.addWidget(cancel_btn)
        layout.addLayout(btn_layout)

    def _start_scan(self):
        self._list.clear()
        self._hint.setText("正在扫描…")
        self._scan_btn.setEnabled(False)
        self._pair_btn.setEnabled(False)

        self._worker = _ScanWorker()
        self._worker.finished.connect(self._on_scan_result)
        self._worker.error.connect(self._on_scan_error)

        from PyQt5.QtCore import QThread
        self._worker_thread = QThread()
        self._worker.moveToThread(self._worker_thread)
        self._worker_thread.started.connect(self._worker.run)
        self._worker_thread.finished.connect(self._worker_thread.deleteLater)
        self._worker_thread.start()

    def _on_scan_result(self, devices):
        self._worker_thread.quit()
        self._worker_thread.wait()
        self._scan_btn.setEnabled(True)

        self._list.clear()
        self._devices = devices

        if not devices:
            self._hint.setText("未找到设备，请确认 AtomS3R 已开机")
            self._list.addItem("(扫描结果为空)")
            return

        self._hint.setText(f"找到 {len(devices)} 个设备")
        for d in devices:
            name = d.get("name", "未知") or "未知"
            addr = d.get("address", "")
            rssi = d.get("rssi", 0)
            # 调试：打印所有发现的设备
            print(f"  [BLE扫描] {name}  {addr}  RSSI={rssi}")
            if name.startswith("VS-") or name.startswith("VC-"):
                item = QListWidgetItem(f"{name}  ({addr})  信号: {rssi}dBm")
                item.setData(Qt.UserRole, (addr, name))
                self._list.addItem(item)
            elif not name or name == "未知":
                # 无设备名的也显示（Windows 偶尔读不到名称）
                item = QListWidgetItem(f"(未知)  {addr}  信号: {rssi}dBm")
                item.setData(Qt.UserRole, (addr, addr[:8]))
                self._list.addItem(item)

        if self._list.count() > 0:
            self._pair_btn.setEnabled(True)
            self._list.setCurrentRow(0)

    def _on_scan_error(self, message):
        if self._worker_thread:
            self._worker_thread.quit()
            self._worker_thread.wait()
        self._scan_btn.setEnabled(True)
        self._list.clear()
        self._hint.setText("扫描失败")
        self._list.addItem(f"错误: {message}")

    def _pair(self):
        item = self._list.currentItem()
        if not item:
            return
        addr, name = item.data(Qt.UserRole)
        self.selected_address = addr
        self.selected_name = name
        self.accept()
