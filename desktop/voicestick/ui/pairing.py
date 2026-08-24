"""设备配对对话框 — 搜索过滤 + 详细信息"""
import asyncio
import sys

from PyQt5.QtWidgets import (
    QDialog, QVBoxLayout, QListWidget, QPushButton,
    QHBoxLayout, QLabel, QListWidgetItem, QLineEdit,
    QWidget, QGridLayout,
)
from PyQt5.QtCore import Qt, pyqtSignal, QObject
from PyQt5.QtGui import QFont


def _signal_bar(rssi: int) -> str:
    """RSSI → 信号强度可视化条"""
    if rssi >= -50:
        return "▂▄▆█"  # 满格
    elif rssi >= -65:
        return "▂▄▆ "  # 三格
    elif rssi >= -80:
        return "▂▄  "  # 两格
    else:
        return "▂   "  # 一格


def _manufacturer_name(mid: str) -> str:
    """已知厂商 ID → 名称"""
    known = {
        "76": "Apple",
        "89": "Samsung",
        "117": "Xiaomi",
        "224": "Espressif(M5Stack)",
        "957": "Espressif",
        "1658": "Huawei",
        "1514": "Huawei",
        "6": "Microsoft",
        "166": "Sony",
    }
    return known.get(mid, f"厂商#{mid}")


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


class _DeviceItemWidget(QWidget):
    """设备列表项的自定义控件 — 名称 地址 信号 厂商信息 多行展示"""

    def __init__(self, device: dict, parent=None):
        super().__init__(parent)
        self._device = device
        self._setup_ui()

    def _setup_ui(self):
        layout = QGridLayout(self)
        layout.setContentsMargins(6, 4, 6, 4)
        layout.setVerticalSpacing(1)
        layout.setHorizontalSpacing(8)

        name = self._device.get("name", "未知") or "未知"
        addr = self._device.get("address", "")
        rssi = self._device.get("rssi", 0)
        mfr = self._device.get("manufacturer_data", {})
        services = self._device.get("service_uuids", [])

        # 第一行：名称 + 信号强度 + 地址
        name_label = QLabel(name)
        name_font = QFont()
        name_font.setBold(True)
        name_label.setFont(name_font)

        # 信号强度指示
        signal_text = _signal_bar(rssi)
        signal_label = QLabel(f"{signal_text}  {rssi}dBm")
        signal_label.setStyleSheet("color: #666; font-size: 11px;")
        signal_label.setAlignment(Qt.AlignRight | Qt.AlignVCenter)

        addr_label = QLabel(addr)
        addr_label.setStyleSheet("color: #888; font-size: 11px;")

        layout.addWidget(name_label, 0, 0)
        layout.addWidget(addr_label, 0, 1)
        layout.addWidget(signal_label, 0, 2)

        row = 1

        # 第二行：厂商信息（如果有）
        if mfr:
            mfr_parts = []
            for mid, mdata in mfr.items():
                mfr_name = _manufacturer_name(mid)
                mfr_parts.append(f"{mfr_name}")
            if mfr_parts:
                mfr_label = QLabel("🏭 " + " | ".join(mfr_parts))
                mfr_label.setStyleSheet("color: #999; font-size: 10px;")
                layout.addWidget(mfr_label, row, 0, 1, 3)
                row += 1

        # 第三行：服务 UUID（如果有）
        if services:
            svc_str = ", ".join(s[:8] + "..." for s in services)
            svc_label = QLabel(f"📡 {svc_str}")
            svc_label.setStyleSheet("color: #999; font-size: 10px;")
            layout.addWidget(svc_label, row, 0, 1, 3)
            row += 1

        # 第四行（可选）：设备类型标识
        if name.startswith(("VS-", "VC-")):
            type_label = QLabel("🎤 语音设备")
            type_label.setStyleSheet("color: #4a9; font-size: 10px; font-weight: bold;")
            layout.addWidget(type_label, row, 0, 1, 3)

    def device(self) -> dict:
        return self._device


class PairingDialog(QDialog):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._devices: list[dict] = []
        self._filtered_devices: list[dict] = []
        self.selected_address = ""
        self.selected_name = ""
        self._worker_thread = None
        self._worker = None

        self.setWindowTitle("配对设备")
        self.setFixedSize(520, 440)
        self._setup_ui()
        self._start_scan()

    def _setup_ui(self):
        layout = QVBoxLayout(self)
        layout.setSpacing(6)

        # 搜索框
        search_layout = QHBoxLayout()
        self._search_input = QLineEdit()
        self._search_input.setPlaceholderText("🔍 搜索设备名称或地址…")
        self._search_input.setClearButtonEnabled(True)
        self._search_input.textChanged.connect(self._filter_devices)
        search_layout.addWidget(self._search_input, 1)

        self._scan_btn = QPushButton("🔄 重新扫描")
        self._scan_btn.clicked.connect(self._start_scan)
        self._scan_btn.setFixedWidth(110)
        search_layout.addWidget(self._scan_btn)
        layout.addLayout(search_layout)

        # 状态提示
        self._hint = QLabel("正在扫描 Voice Cube 设备…")
        self._hint.setAlignment(Qt.AlignCenter)
        self._hint.setStyleSheet("color: #666; padding: 2px;")
        layout.addWidget(self._hint)

        # 设备列表
        self._list = QListWidget()
        self._list.setAlternatingRowColors(True)
        self._list.setSpacing(2)
        self._list.itemSelectionChanged.connect(self._on_selection_changed)
        layout.addWidget(self._list)

        # 按钮行
        btn_layout = QHBoxLayout()
        self._count_label = QLabel("")
        self._count_label.setStyleSheet("color: #999; font-size: 11px;")
        self._pair_btn = QPushButton("配对")
        self._pair_btn.clicked.connect(self._pair)
        self._pair_btn.setEnabled(False)
        self._pair_btn.setFixedWidth(100)
        cancel_btn = QPushButton("取消")
        cancel_btn.clicked.connect(self.reject)
        cancel_btn.setFixedWidth(80)

        btn_layout.addWidget(self._count_label)
        btn_layout.addStretch()
        btn_layout.addWidget(self._pair_btn)
        btn_layout.addWidget(cancel_btn)
        layout.addLayout(btn_layout)

    def _start_scan(self):
        self._list.clear()
        self._filtered_devices = []
        self._search_input.clear()
        self._hint.setText("正在扫描…")
        self._scan_btn.setEnabled(False)
        self._pair_btn.setEnabled(False)
        self._count_label.setText("")

        self._worker = _ScanWorker()
        self._worker.finished.connect(self._on_scan_result)
        self._worker.error.connect(self._on_scan_error)

        from PyQt5.QtCore import QThread
        self._worker_thread = QThread()
        self._worker.moveToThread(self._worker_thread)
        self._worker_thread.started.connect(self._worker.run)
        self._worker_thread.finished.connect(self._worker_thread.deleteLater)
        self._worker_thread.start()

    def _filter_devices(self, keyword: str):
        """根据搜索关键字过滤设备列表"""
        self._list.clear()
        kw = keyword.strip().lower()
        if not kw:
            filtered = list(self._devices)
        else:
            filtered = [
                d for d in self._devices
                if kw in d.get("name", "").lower()
                or kw in d.get("address", "").lower()
                or kw in d.get("address", "").replace(":", "").lower()
                or kw in d.get("address", "").replace(":", "").lower()[-4:]
            ]
        self._filtered_devices = filtered

        if not self._devices:
            self._count_label.setText("")
            return

        for d in filtered:
            item = QListWidgetItem()
            widget = _DeviceItemWidget(d)
            item.setSizeHint(widget.sizeHint())
            item.setData(Qt.UserRole, (d.get("address", ""), d.get("name", "")))
            self._list.addItem(item)
            self._list.setItemWidget(item, widget)

        total = len(self._devices)
        shown = len(filtered)
        if keyword.strip():
            self._count_label.setText(f"显示 {shown}/{total} 个设备")
        else:
            self._count_label.setText(f"共 {total} 个设备")

        if filtered:
            self._pair_btn.setEnabled(True)
            self._list.setCurrentRow(0)
        else:
            self._pair_btn.setEnabled(False)

    def _on_selection_changed(self):
        self._pair_btn.setEnabled(self._list.currentItem() is not None)

    def _on_scan_result(self, devices):
        self._worker_thread.quit()
        self._worker_thread.wait()
        self._scan_btn.setEnabled(True)

        self._list.clear()
        self._devices = devices

        if not devices:
            self._hint.setText("未找到设备，请确认设备已开机并处于语音输入模式")
            self._list.addItem("(扫描结果为空)")
            self._count_label.setText("")
            return

        self._hint.setText(f"找到 {len(devices)} 个设备（VS-/VC- 前缀为语音设备）")
        self._filter_devices(self._search_input.text())

    def _on_scan_error(self, message):
        if self._worker_thread:
            self._worker_thread.quit()
            self._worker_thread.wait()
        self._scan_btn.setEnabled(True)
        self._list.clear()
        self._hint.setText("扫描失败")
        self._list.addItem(f"错误: {message}")
        self._count_label.setText("")

    def _pair(self):
        item = self._list.currentItem()
        if not item:
            return
        addr, name = item.data(Qt.UserRole)
        self.selected_address = addr
        self.selected_name = name
        self.accept()