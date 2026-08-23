"""BLE 客户端 — 扫描、连接、收发"""
import asyncio
import logging
from typing import Optional, Callable

import bleak
from bleak import BleakScanner, BleakClient
from bleak.backends.device import BLEDevice

from .protocol import (
    SERVICE_UUID, AUDIO_TX_UUID, STATE_TX_UUID, CONTROL_RX_UUID,
    parse_audio_frame, parse_state_event, AudioFrame, StateEvent,
)

logger = logging.getLogger(__name__)


class BleClient:
    def __init__(self):
        self._client: Optional[BleakClient] = None
        self._device_id = ""
        self._device_name = ""
        self._last_address = ""
        self._audio_char = None
        self._state_char = None
        self._control_char = None

        # 回调
        self.on_audio_frame: Optional[Callable[[AudioFrame], None]] = None
        self.on_state_event: Optional[Callable[[StateEvent], None]] = None
        self.on_connected: Optional[Callable[[], None]] = None
        self.on_disconnected: Optional[Callable[[], None]] = None
        self.on_error: Optional[Callable[[str], None]] = None

    @property
    def is_connected(self) -> bool:
        return self._client is not None and self._client.is_connected

    @property
    def device_id(self) -> str:
        return self._device_id

    @property
    def device_name(self) -> str:
        return self._device_name

    async def scan(self, timeout: float = 5.0) -> list[dict]:
        """扫描 BLE 设备，返回全部结果（配对对话框自行过滤）"""
        devices = []
        scanner = BleakScanner()

        async with scanner:
            await asyncio.sleep(timeout)

        discovered = scanner.discovered_devices
        if isinstance(discovered, dict):
            discovered = discovered.values()
        for d in discovered:
            name = d.name or ""
            rssi = getattr(d, 'rssi', 0) or 0
            logger.debug("BLE扫描到: %s (%s) RSSI=%d", name, d.address, rssi)
            devices.append({
                "address": d.address,
                "name": name,
                "rssi": rssi,
            })
        logger.info("BLE扫描完成: 发现 %d 个设备", len(devices))
        return devices

    async def connect(self, address: str, name: str = "", timeout: float = 15.0):
        """连接 BLE 设备。timeout: 配对/手动连接用默认 15s;自动重连传 8s,
        避免设备正在重启时直连挂满 15 秒才进扫描回退。"""
        if self.is_connected:
            logger.info("BLE 已连接，跳过重复连接: %s", self._device_name)
            return
        self._last_address = address
        self._device_name = name
        device_id = name[3:] if name.startswith(("VS-", "VC-")) else address.replace(":", "")[-4:]
        self._device_id = device_id

        def _disconnect_callback(client):
            logger.info("BLE 断开连接: %s", self._device_name)
            if self.on_disconnected:
                self.on_disconnected()

        self._client = BleakClient(address, disconnected_callback=_disconnect_callback)

        try:
            await self._client.connect(timeout=timeout)
            logger.info("BLE 已连接: %s (%s)", self._device_name, address)
        except Exception as e:
            logger.error("BLE 连接失败: %s", e)
            if self.on_error:
                self.on_error(f"连接失败: {e}")
            self._client = None
            return

        # 发现服务和特征
        try:
            for service in self._client.services:
                if service.uuid.lower() == SERVICE_UUID:
                    for char in service.characteristics:
                        cu = char.uuid.lower()
                        if cu == AUDIO_TX_UUID:
                            self._audio_char = char
                        elif cu == STATE_TX_UUID:
                            self._state_char = char
                        elif cu == CONTROL_RX_UUID:
                            self._control_char = char
        except Exception as e:
            logger.error("服务发现失败: %s", e)
            if self.on_error:
                self.on_error(f"服务发现失败: {e}")
            await self.disconnect()
            return

        if not all([self._audio_char, self._state_char, self._control_char]):
            missing = []
            if not self._audio_char: missing.append("audio_tx")
            if not self._state_char: missing.append("state_tx")
            if not self._control_char: missing.append("control_rx")
            err = f"缺少必要特征: {', '.join(missing)}"
            logger.error(err)
            if self.on_error:
                self.on_error(err)
            await self.disconnect()
            return

        # 订阅通知
        try:
            await self._client.start_notify(
                self._audio_char.uuid,
                self._on_audio_notify
            )
            await self._client.start_notify(
                self._state_char.uuid,
                self._on_state_notify
            )
        except Exception as e:
            logger.error("订阅通知失败: %s", e)
            if self.on_error:
                self.on_error(f"订阅通知失败: {e}")
            await self.disconnect()
            return

        logger.info("BLE 服务就绪: %s", self._device_name)
        if self.on_connected:
            self.on_connected(self._device_name)

    async def disconnect(self):
        if self._client and self._client.is_connected:
            try:
                await self._client.disconnect()
            except Exception:
                pass
        self._client = None
        self._audio_char = None
        self._state_char = None
        self._control_char = None

    async def send_control(self, data: bytes):
        """发送控制命令 (write without response)"""
        if not self._client or not self._client.is_connected or not self._control_char:
            logger.warning("无法发送控制命令：未连接")
            return
        try:
            await self._client.write_gatt_char(
                self._control_char.uuid, data, response=False
            )
        except Exception as e:
            logger.error("发送控制命令失败: %s", e)

    async def send_ui_state(self, state: str, text: str = ""):
        """发送 UI 状态到设备"""
        from .protocol import ui_state_payload
        payload = ui_state_payload(state, text)
        await self.send_control(payload)

    def _on_audio_notify(self, sender, data: bytearray):
        frame = parse_audio_frame(bytes(data))
        if not frame:
            logger.warning("音频帧解析失败: %d 字节, 头=%s", len(data), data[:8].hex())
            return
        logger.debug("音频帧: session=%d seq=%d flags=0x%x payload=%d字节",
                      frame.session_id, frame.seq, frame.flags, len(frame.payload))
        if self.on_audio_frame:
            self.on_audio_frame(frame)

    def _on_state_notify(self, sender, data: bytearray):
        logger.info("收到状态通知: %d 字节: %s", len(data), bytes(data)[:120])
        event = parse_state_event(bytes(data))
        if event:
            logger.info("解析为事件: %s button=%s", event.event, event.button)
            if self.on_state_event:
                self.on_state_event(event)
