"""VoiceStick BLE 协议解析"""
import struct
import json
from dataclasses import dataclass
from typing import Optional

# UUIDs
SERVICE_UUID = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100"
AUDIO_TX_UUID = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5101"
STATE_TX_UUID = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5102"
CONTROL_RX_UUID = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5103"
OTA_RX_UUID = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5104"
OTA_STATE_UUID = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5105"

AUDIO_FRAME_HEADER_SIZE = 16


@dataclass
class AudioFrame:
    session_id: int = 0
    seq: int = 0
    flags: int = 0
    payload: bytes = b""

    def is_start(self) -> bool:
        return bool(self.flags & 0x01)

    def is_end(self) -> bool:
        return bool(self.flags & 0x02)


@dataclass
class StateEvent:
    event: str = ""
    button: str = ""
    session_id: Optional[int] = None
    duration_ms: Optional[int] = None
    hardware: str = ""
    firmware_version: str = ""


def parse_audio_frame(data: bytes) -> Optional[AudioFrame]:
    """解析 BLE 音频帧 (小端序)"""
    if len(data) < AUDIO_FRAME_HEADER_SIZE:
        return None
    version, type_, header_len, reserved_hdr = struct.unpack_from("BBBB", data, 0)
    if version != 1 or type_ != 0x01:
        return None
    session_id, seq, flags, reserved, payload_len = struct.unpack_from("<IIBBH", data, 4)
    payload = data[AUDIO_FRAME_HEADER_SIZE:AUDIO_FRAME_HEADER_SIZE + payload_len] if payload_len else b""
    return AudioFrame(session_id=session_id, seq=seq, flags=flags, payload=payload)


def parse_state_event(data: bytes) -> Optional[StateEvent]:
    """解析 BLE 状态 JSON"""
    try:
        obj = json.loads(data.decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError):
        return None
    ev = StateEvent(event=obj.get("event", ""))
    ev.button = obj.get("button", "")
    ev.session_id = obj.get("session_id")
    ev.duration_ms = obj.get("duration_ms")
    ev.hardware = obj.get("hardware", "")
    ev.firmware_version = obj.get("firmware_version", "")
    return ev


def ui_state_payload(state: str, text: str = "") -> bytes:
    """生成 UI 状态控制命令"""
    payload = json.dumps({"event": "ui_state", "state": state, "text": text}, ensure_ascii=False)
    return payload.encode("utf-8")


def device_id_from_name(name: str) -> Optional[str]:
    """从设备名 VS-XXXX 提取 device_id"""
    if name.startswith("VS-"):
        return name[3:]
    return None
