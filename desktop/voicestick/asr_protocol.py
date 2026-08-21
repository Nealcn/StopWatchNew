"""VoiceStick ASR WebSocket 二进制协议"""
import json
import struct
from typing import Optional
from dataclasses import dataclass

# 消息类型
MESSAGE_EVENT = 0x01       # 事件帧（StartConnection 等）
MESSAGE_TASK = 0x02        # 任务帧（音频数据）
MESSAGE_RESPONSE = 0x09    # 服务端响应
MESSAGE_EVENT_RESPONSE = 0x0B  # 服务端事件响应
MESSAGE_ERROR = 0x0F       # 服务端错误

# 事件 ID（客户端 → 服务端）
EVENT_START_CONNECTION = 1
EVENT_FINISH_CONNECTION = 2
EVENT_START_SESSION = 100
EVENT_CANCEL_SESSION = 101
EVENT_FINISH_SESSION = 102
EVENT_TASK_REQUEST = 200

# 事件 ID（服务端 → 客户端）
EVENT_CONNECTION_STARTED = 50
EVENT_CONNECTION_FAILED = 51
EVENT_CONNECTION_FINISHED = 52
EVENT_SESSION_STARTED = 150
EVENT_SESSION_CANCELED = 151
EVENT_SESSION_FINISHED = 152
EVENT_USAGE_RESPONSE = 154
EVENT_ASR_INFO = 450       # 部分结果
EVENT_ASR_RESPONSE = 451   # 最终结果
EVENT_ASR_END = 459        # 识别结束


@dataclass
class AsrResponse:
    is_error: bool = False
    is_final: bool = False
    text: str = ""
    upgrade_url: Optional[str] = None


@dataclass
class AsrEventResponse:
    event_id: int = 0
    event_name: str = ""
    session_id: str = ""
    payload_text: str = ""


def _pack_frame(message_type: int, event_id: int, session_id: str, payload: bytes) -> bytes:
    """打包二进制事件帧（匹配 C++ 协议）"""
    frame = bytearray()
    frame.append(0x11)
    frame.append((message_type << 4) | 0x04)
    frame.append(0x10)
    frame.append(0x00)
    frame.extend(struct.pack(">I", event_id))
    if session_id:
        sid_bytes = session_id.encode("utf-8")
        frame.extend(struct.pack(">I", len(sid_bytes)))
        frame.extend(sid_bytes)
    frame.extend(struct.pack(">I", len(payload)))
    frame.extend(payload)
    return bytes(frame)


def make_start_connection() -> bytes:
    """Volcengine Full Client Request (type 0x01) — 连接/会话配置

    参考官方 SDK 和 talky 项目的标准 JSON 格式：
    {"user":..., "audio":..., "request":...}
    无 namespace/event/req_params 嵌套。
    """
    payload = {
        "user": {"uid": "voice-stick-local"},
        "audio": {"format": "ogg", "codec": "opus", "rate": 16000, "bits": 16, "channel": 1},
        "request": {
            "model_name": "bigmodel",
            "enable_nonstream": True,
            "enable_ddc": True,
            "resource_id": "volc.seedasr.sauc.duration",
            "show_utterances": False,
            "result_type": "full",
            "enable_punc": True,
            "enable_itn": True,
        },
    }
    payload_bytes = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    # 标准 format: [header 4B] + [body_size 4B BE] + [JSON body]
    # 参考: struct.pack('!BBBB', 0b0001_0001, 0b0001_0000, 0b0001_0000, 0)
    header = bytes([0x11, 0x10, 0x10, 0x00])
    return header + struct.pack(">I", len(payload_bytes)) + payload_bytes


def make_finish_connection() -> bytes:
    """kFinishConnection (event=2)"""
    payload = json.dumps({"namespace": "BidirectionalASR", "event": 0}).encode("utf-8")
    return _pack_frame(MESSAGE_EVENT, EVENT_FINISH_CONNECTION, "", payload)


def make_start_session(session_id: str, hotwords: list[str] = None) -> bytes:
    """kStartSession (event=100)"""
    payload = {
        "user": {"uid": "voice-stick-local"},
        "audio": {"format": "ogg", "codec": "opus", "rate": 16000, "bits": 16, "channel": 1},
        "request": {
            "model_name": "bigmodel",
            "enable_nonstream": True,
            "show_utterances": False,
            "result_type": "full",
            "enable_ddc": True,
            "resource_id": "volc.seedasr.sauc.duration",
        },
    }
    if hotwords:
        payload["request"]["corpus"] = {"context": json.dumps({"hotwords": [{"word": w} for w in hotwords]})}
    return _pack_frame(MESSAGE_EVENT, EVENT_START_SESSION, session_id,
                       json.dumps(payload, ensure_ascii=False).encode("utf-8"))


def make_finish_session(session_id: str) -> bytes:
    """kFinishSession (event=102)"""
    payload = json.dumps({"namespace": "BidirectionalASR", "event": 0}).encode("utf-8")
    return _pack_frame(MESSAGE_EVENT, EVENT_FINISH_SESSION, session_id, payload)


def make_audio_frame(ogg_data: bytes, is_last: bool = False) -> bytes:
    """
    火山引擎 Audio-only Request (type 0x02)

    格式:
      [4字节头部] + [body_size 4B big-endian] + [裸 Ogg Opus 数据]

    火山引擎协议每个消息都有 body_size 字段（包括音频帧）。
    body_size 等于后续 Ogg 数据的长度。

    非最后一帧: 头部 0x11,0x20,0x10,0x00 + body_size + audio
    最后一帧:   头部 0x11,0x22,0x10,0x00 + body_size + audio (last标记)
    """
    if is_last:
        header = bytes([0x11, 0x22, 0x10, 0x00])
    else:
        header = bytes([0x11, 0x20, 0x10, 0x00])
    return header + struct.pack(">I", len(ogg_data)) + ogg_data


def parse_response(data: bytes) -> Optional[AsrResponse]:
    """解析服务端二进制响应"""
    if len(data) < 4:
        return None
    message_type = data[1] >> 4
    flags = data[1] & 0x0f
    offset = (data[0] & 0x0f) * 4
    if offset > len(data):
        return None

    if message_type == MESSAGE_RESPONSE:
        if flags in (0x01, 0x03):
            if len(data) < offset + 4:
                return None
            offset += 4
        if len(data) < offset + 4:
            return None
        payload_size = struct.unpack_from(">I", data, offset)[0]
        offset += 4
        if len(data) < offset + payload_size:
            return None
        body = data[offset:offset + payload_size].decode("utf-8", errors="replace")
        # 提取 text 字段
        text = _json_extract(body, "text")
        return AsrResponse(is_error=False, is_final=(flags == 0x03), text=text or body)

    if message_type == MESSAGE_ERROR:
        if len(data) < offset + 8:
            return None
        code = struct.unpack_from(">I", data, offset)[0]
        offset += 4
        msg_size = struct.unpack_from(">I", data, offset)[0]
        offset += 4
        if len(data) < offset + msg_size:
            return None
        msg = data[offset:offset + msg_size].decode("utf-8", errors="replace")
        detail = _json_extract(msg, "message") or _json_extract(msg, "error") or msg
        response = AsrResponse(is_error=True, text=f"ASR {code}: {detail}")
        upgrade = _json_extract(msg, "upgrade_url")
        if upgrade:
            response.upgrade_url = upgrade
        return response

    return None


def parse_event_response(data: bytes) -> Optional[AsrEventResponse]:
    """解析服务端事件响应"""
    if len(data) < 4:
        return None
    message_type = data[1] >> 4
    if message_type not in (MESSAGE_RESPONSE, MESSAGE_EVENT_RESPONSE):
        return None
    flags = data[1] & 0x0f
    compression = data[2] & 0x0f
    if flags != 0x04 or compression != 0x00:
        return None
    offset = (data[0] & 0x0f) * 4
    if len(data) < offset + 4:
        return None
    event_id = struct.unpack_from(">I", data, offset)[0]

    result = AsrEventResponse(event_id=event_id)

    offset += 4
    if len(data) >= offset + 4:
        sid_size = struct.unpack_from(">I", data, offset)[0]
        offset += 4
        if len(data) >= offset + sid_size:
            result.session_id = data[offset:offset + sid_size].decode("utf-8", errors="replace")
            offset += sid_size

    if len(data) >= offset + 4:
        payload_size = struct.unpack_from(">I", data, offset)[0]
        offset += 4
        if len(data) >= offset + payload_size:
            result.payload_text = data[offset:offset + payload_size].decode("utf-8", errors="replace")

    return result


def _json_extract(text: str, key: str) -> str:
    """简单 JSON 字段提取"""
    import re
    m = re.search(r'"{0}"\s*:\s*"((?:\\.|[^"\\])*)"'.format(key), text)
    if m:
        return m.group(1).replace("\\n", "\n").replace("\\t", "\t").replace("\\\"", "\"")
    return ""
