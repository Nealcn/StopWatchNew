"""VoiceStick 主应用 — 系统托盘 + 窗口管理"""
import asyncio
import logging
import sys
import threading
from typing import Optional

from PyQt5.QtWidgets import (
    QApplication, QSystemTrayIcon, QMenu, QMessageBox, QAction,
)
from PyQt5.QtCore import QTimer, Qt, QObject, pyqtSignal
from PyQt5.QtGui import QIcon, QFont, QPixmap, QPainter, QColor, QPen

from .config import AppConfig
from .ble import BleClient
from .asr_client import AsrClient
from .coordinator import Coordinator
from .ui.floatball import FloatingBallWindow
from .ui.settings import SettingsDialog
from .ui.pairing import PairingDialog

logger = logging.getLogger(__name__)


class _ClipboardBridge(QObject):
    """跨线程剪贴板桥：从后台线程安全地设置主线程剪贴板"""
    copy_requested = pyqtSignal(str)

    def __init__(self):
        super().__init__()
        self.copy_requested.connect(QApplication.clipboard().setText)


class VoiceStickApp:
    def __init__(self, qapp: QApplication):
        self._qapp = qapp
        self._config = AppConfig.load()
        self._switching_device = False  # 切换设备中，不触发自动重连

        # BLE + ASR + 协调器
        self._ble = BleClient()
        self._asr = AsrClient(self._config.asr_server_url, self._config.asr_api_key)
        self._coordinator = Coordinator(self._ble, self._asr)
        # 跨线程剪贴板桥（必须在主线程创建 QObject）
        self._clipboard_bridge = _ClipboardBridge()
        self._coordinator.clipboard_callback = self._clipboard_bridge.copy_requested.emit
        # 保存 coordinator 的 BLE 回调，避免被 app.py 覆盖后丢失
        self._coord_on_connected = self._ble.on_connected
        self._coord_on_disconnected = self._ble.on_disconnected

        # UI
        self._tray: Optional[QSystemTrayIcon] = None
        self._tray_menu: Optional[QMenu] = None
        self._floatball = FloatingBallWindow()
        self._floatball.position_changed.connect(self._save_floatball_pos)
        self._floatball.reconnect_requested.connect(self._force_reconnect)
        # LLM callbacks for 整理/翻译 buttons
        self._floatball.set_llm_callbacks(
            polish_cb=self._polish_text,
            translate_cb=self._translate_text,
        )

        # 状态
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._loop_thread: Optional[threading.Thread] = None

        # 配置协调器
        self._coordinator.paste_on_final = self._config.paste_on_final
        self._coordinator.configure_translation(
            enabled=self._config.enable_translation,
            api_key=self._config.llm_api_key,
            base_url=self._config.llm_base_url,
            model=self._config.llm_model,
            target_language=self._config.translation_target,
        )
        self._coordinator.configure_polish(
            enabled=self._config.enable_polish,
            prompt=self._config.polish_prompt,
            before_translate=(self._config.polish_position == "before_translate"),
        )
        self._coordinator.on_status = self._on_status
        self._coordinator.on_partial_text = self._on_partial_text
        self._coordinator.on_final_text = self._on_final_text
        self._coordinator.on_session_cancelled = self._on_session_cancelled

        # BLE 回调
        self._ble.on_connected = self._on_ble_connected
        self._ble.on_disconnected = self._on_ble_disconnected

    # ---- 生命周期 ----

    def start(self):
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)

        self._setup_tray()
        self._floatball.load_pos(self._config.floatball_x, self._config.floatball_y)
        self._floatball.show()
        self._coordinator.on_status("启动中…")

        # asyncio 事件循环运行在后台线程
        self._loop_thread = threading.Thread(target=self._run_loop, daemon=True)
        self._loop_thread.start()

        # 异步初始化
        asyncio.run_coroutine_threadsafe(self._init_async(), self._loop)

    def _run_loop(self):
        """后台线程：驱动 asyncio 事件循环"""
        asyncio.set_event_loop(self._loop)
        self._loop.run_forever()

    async def _init_async(self):
        """异步初始化（后台执行，不阻塞）"""
        await self._coordinator.start()
        if self._config.paired_device_ids:
            # 先直连上次的地址，不行再扫
            asyncio.create_task(self._auto_reconnect())

    def shutdown(self):
        if self._loop:
            self._loop.call_soon_threadsafe(self._loop.stop)
            if self._loop_thread:
                self._loop_thread.join(timeout=3)
            self._loop.close()

    # ---- 系统托盘 ----

    def _setup_tray(self):
        self._tray_menu = QMenu()

        status_action = self._tray_menu.addAction("状态: 启动中")
        status_action.setEnabled(False)
        self._status_action = status_action

        self._tray_menu.addSeparator()

        devices_menu = self._tray_menu.addMenu("设备")
        scan_action = devices_menu.addAction("扫描配对…")
        scan_action.triggered.connect(self._show_pairing)
        devices_menu.addSeparator()
        switch_menu = devices_menu.addMenu("切换设备")
        for dev_id in self._config.paired_device_ids:
            act = switch_menu.addAction(dev_id)
            act.triggered.connect(lambda checked=False, did=dev_id: self._switch_device(did))
        self._devices_menu = devices_menu
        self._switch_menu = switch_menu

        self._tray_menu.addSeparator()

        settings_action = self._tray_menu.addAction("设置")
        settings_action.triggered.connect(self._show_settings)

        about_action = self._tray_menu.addAction("关于")
        about_action.triggered.connect(self._show_about)

        self._tray_menu.addSeparator()

        quit_action = self._tray_menu.addAction("退出")
        quit_action.triggered.connect(self._quit)

        # 创建麦克风图标 (16x16)
        icon = self._make_icon()
        self._tray = QSystemTrayIcon(icon, self._qapp.activeWindow())
        self._tray.setContextMenu(self._tray_menu)
        self._tray.setToolTip("Voice Cube")
        self._tray.activated.connect(self._on_tray_activated)
        self._tray.show()

    @staticmethod
    def _make_icon() -> QIcon:
        """绘制麦克风托盘图标（深色背景 + 白色符号，确保可见）"""
        pm = QPixmap(22, 22)
        pm.fill(QColor(0x20, 0x80, 0xC0))  # 蓝色背景
        p = QPainter(pm)
        p.setRenderHint(QPainter.Antialiasing)
        p.setPen(QPen(QColor(255, 255, 255), 2))
        # 简化麦克风
        p.drawEllipse(8, 3, 6, 6)       # 话筒头
        p.drawRect(7, 10, 8, 6)          # 话筒身
        p.drawLine(11, 16, 11, 19)      # 底座
        p.drawLine(7, 19, 15, 19)       # 底座横线
        p.end()
        return QIcon(pm)

    def _on_tray_activated(self, reason):
        if reason == QSystemTrayIcon.DoubleClick:
            self._show_settings()

    # ---- 操作 ----

    def _switch_device(self, device_id: str):
        """切换到指定已配对设备：取消自动重连 → 扫描 → 连接"""
        # 取消正在运行的自动重连，防止并发冲突
        old_task = getattr(self, "_reconnect_task", None)
        if old_task is not None and not old_task.done():
            old_task.cancel()
        self._reconnect_task = None
        self._switching_device = True

        async def _do_switch():
            try:
                devices = await self._ble.scan(5.0)
                target = None
                for d in devices:
                    name = d.get("name", "") or ""
                    addr = d.get("address", "")
                    dev_id = name[3:] if (name.startswith("VS-") or name.startswith("VC-")) else addr.replace(":", "")[-4:]
                    if dev_id == device_id:
                        target = d
                        break
                if target is None:
                    logger.error("未找到设备 %s（请确认已开机并在语音输入模式）", device_id)
                    self._coordinator.on_status("设备未找到")
                    return
                self._config.last_connected_address = target["address"]
                self._config.last_connected_name = target.get("name", "")
                self._config.save()
                if self._ble.is_connected:
                    await self._ble.disconnect()
                self._coordinator.on_status("连接中…")
                await self._ble.connect(target["address"], self._config.last_connected_name)
                if not self._ble.is_connected:
                    self._coordinator.on_status("连接失败")
                else:
                    logger.info("已切换到设备 %s", self._config.last_connected_name)
            except asyncio.CancelledError:
                logger.debug("设备切换被取消")
                raise
            finally:
                self._switching_device = False

        asyncio.run_coroutine_threadsafe(_do_switch(), self._loop)

    def _show_pairing(self):
        dialog = PairingDialog(self._qapp.activeWindow())
        if dialog.exec() and dialog.selected_address:
            addr = dialog.selected_address
            name = dialog.selected_name
            device_id = name[3:] if (name.startswith("VS-") or name.startswith("VC-")) else addr.replace(":", "")[-4:]
            if device_id not in self._config.paired_device_ids:
                self._config.paired_device_ids.append(device_id)
                self._config.save()

            self._coordinator.on_status("连接中…")

            async def connect():
                await self._ble.connect(addr, name)
                if not self._ble.is_connected:
                    self._coordinator.on_status("连接失败")

            asyncio.run_coroutine_threadsafe(connect(), self._loop)

    def _show_settings(self):
        dialog = SettingsDialog(self._config, self._qapp.activeWindow())
        if dialog.exec() and dialog.changed:
            self._coordinator.paste_on_final = self._config.paste_on_final
            self._coordinator.configure_translation(
                enabled=self._config.enable_translation,
                api_key=self._config.llm_api_key,
                base_url=self._config.llm_base_url,
                model=self._config.llm_model,
                target_language=self._config.translation_target,
            )
            self._coordinator.configure_polish(
                enabled=self._config.enable_polish,
                prompt=self._config.polish_prompt,
                before_translate=(self._config.polish_position == "before_translate"),
            )

    def _show_about(self):
        QMessageBox.about(
            self._qapp.activeWindow(),
            "关于 VoiceStick",
            "VoiceStick Windows 桌面端 (Python 重写版)\n\n"
            "将 AtomS3R 语音速记助手识别的文字自动粘贴到当前窗口。\n\n"
            "协议: MIT"
        )

    def _quit(self):
        self._tray.hide()
        self.shutdown()
        self._qapp.quit()

    # ---- 回调 ----

    def _on_status(self, status: str):
        self._status_action.setText(f"状态: {status}")
        self._tray.setToolTip(f"Voice Cube — {status}")
        self._floatball.set_status(status)
        # "已复制" 状态 1 秒后自动恢复 "就绪"（替代 coordinator 中阻塞的 asyncio.sleep）
        if status == "已复制":
            QTimer.singleShot(1000, lambda: self._on_status("就绪"))

    def _on_partial_text(self, text: str):
        self._floatball.set_partial_text(text)

    def _on_final_text(self, text: str):
        self._floatball.set_final_text(text)

    def _on_session_cancelled(self):
        """会话被取消（右键清除）：清空悬浮球文字"""
        self._floatball.clear_text()

    def _on_ble_connected(self, device_name: str):
        # 先执行 coordinator 的逻辑（重置 _recording、启动保活等）
        if self._coord_on_connected:
            self._coord_on_connected()
        # 再执行 app.py 的逻辑
        self._status_action.setText(f"已连接: {device_name}")
        self._tray.setToolTip(f"Voice Cube — {device_name}")
        self._floatball.set_connected(True)
        # 持久化上次连接的设备，重启后可直接重连
        self._config.last_connected_address = self._ble._last_address
        self._config.last_connected_name = self._ble._device_name
        self._config.save()

    def _on_ble_disconnected(self):
        if self._coord_on_disconnected:
            self._coord_on_disconnected()
        self._status_action.setText("状态: 已断开（重连中…）")
        self._floatball.set_connected(False)
        # 切换设备时由 _do_switch 负责连接，不触发自动重连
        if self._switching_device:
            return
        self._schedule_reconnect()

    def _polish_text(self, text: str):
        """同步包装：调用 LLM 润色（通过后台事件循环）"""
        import asyncio
        from .llm_translation_client import LLMTranslationResult
        if not self._coordinator._translator.is_configured:
            return LLMTranslationResult("", error="LLM 未配置（设置中填写 API Key）")
        prompt = self._config.polish_prompt
        fut = asyncio.run_coroutine_threadsafe(
            self._coordinator._translator.polish(text, prompt), self._loop)
        return fut.result(timeout=15)

    def _translate_text(self, text: str):
        """同步包装：调用 LLM 翻译（通过后台事件循环）"""
        import asyncio
        from .llm_translation_client import LLMTranslationResult
        if not self._coordinator._translator.is_configured:
            return LLMTranslationResult("", error="LLM 未配置（设置中填写 API Key）")
        fut = asyncio.run_coroutine_threadsafe(
            self._coordinator._translator.translate(text), self._loop)
        return fut.result(timeout=15)

    def _save_floatball_pos(self):
        x, y = self._floatball.save_pos()
        self._config.floatball_x = x
        self._config.floatball_y = y
        self._config.save()


    async def _auto_reconnect(self):
        """断连后持续自动重连：直连 → 扫描交替，退避 2s→5s→10s，
        直到连上或应用退出。设备崩溃重启/重启较慢时不再放弃。
        注意：此协程可能在外部被取消（_force_reconnect/_switch_device），
        必须用 try/except CancelledError 保证退出干净。"""
        delay = 2.0
        try:
            while not self._ble.is_connected:
                if self._switching_device:
                    return
                addr = self._ble._last_address or self._config.last_connected_address
                name = self._ble._device_name or self._config.last_connected_name
                if not addr and not self._config.paired_device_ids:
                    return  # 没有可连的设备
                if addr:
                    await self._ble.connect(addr, name, timeout=8.0)
                if self._ble.is_connected:
                    return
                if not self._switching_device and self._config.paired_device_ids:
                    await self._scan_and_connect(retries=1)  # 每轮一次扫描
                if self._ble.is_connected:
                    return
                await asyncio.sleep(delay)
                delay = min(delay * 2, 10.0)
        except asyncio.CancelledError:
            logger.debug("自动重连被取消")
            raise

    def _schedule_reconnect(self):
        """启动/复用自动重连任务（防止断连风暴产生多个并发循环）"""
        current = getattr(self, "_reconnect_task", None)
        if current is not None and not current.done():
            return
        self._reconnect_task = asyncio.run_coroutine_threadsafe(
            self._auto_reconnect(), self._loop)

    def _force_reconnect(self):
        """双击小球强制重连：取消当前重连任务，立即开始一轮新的扫描+连接"""
        old_task = getattr(self, "_reconnect_task", None)
        if old_task is not None and not old_task.done():
            old_task.cancel()
        self._reconnect_task = None
        self._coordinator.on_status("重连中…")
        self._reconnect_task = asyncio.run_coroutine_threadsafe(
            self._force_reconnect_async(), self._loop)

    async def _force_reconnect_async(self):
        """立即强制扫描+连接，不等待退避"""
        try:
            if self._ble.is_connected:
                return
            addr = self._ble._last_address or self._config.last_connected_address
            name = self._ble._device_name or self._config.last_connected_name
            if addr:
                await self._ble.connect(addr, name, timeout=8.0)
            if not self._ble.is_connected and self._config.paired_device_ids:
                await self._scan_and_connect(retries=2)
            if self._ble.is_connected:
                self._coordinator.on_status("已连接")
            else:
                self._coordinator.on_status("重连失败")
                self._schedule_reconnect()
        except asyncio.CancelledError:
            logger.debug("强制重连被取消")
            raise

    async def _scan_and_connect(self, retries=3):
        """扫描并连接第一个已配对设备（扫描不到自动重试）"""
        if self._ble.is_connected:
            return
        paired = set(self._config.paired_device_ids)
        try:
            for attempt in range(retries):
                if self._ble.is_connected:
                    return
                await asyncio.sleep(1)  # 每次尝试前等设备就绪(广播间隔已加快,1s 足够)
                devices = await self._ble.scan(3.0 if attempt == 0 else 5.0)
                for d in devices:
                    name = d.get("name", "") or ""
                    addr = d.get("address", "")
                    dev_id = name[3:] if (name.startswith("VS-") or name.startswith("VC-")) else addr.replace(":", "")[-4:]
                    if dev_id in paired:
                        await self._ble.connect(d["address"], name)
                        return
                if attempt < retries - 1:
                    logger.info("扫描未匹配到设备，%.0f 秒后重试...", 3.0)
                    await asyncio.sleep(3)
        except asyncio.CancelledError:
            logger.debug("扫描连接被取消")
            raise
