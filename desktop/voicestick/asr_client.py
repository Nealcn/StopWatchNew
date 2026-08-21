"""ASR 客户端 — VoiceStick 云语音识别 WebSocket 协议"""
import asyncio
import logging
import uuid
from typing import Optional, Callable
from dataclasses import dataclass

import aiohttp
from aiohttp import WSMsgType

from . import asr_protocol as proto
from .ogg_opus_muxer import OggOpusMuxer

logger = logging.getLogger(__name__)


@dataclass
class AsrSegment:
    text: str
    is_final: bool = False
    segment_index: int = 0


class AsrClient:
    """VoiceStick 云 ASR 客户端"""

    def __init__(self, server_url: str, api_key: str = ""):
        self._server_url = server_url
        self._api_key = api_key
        self._session: Optional[aiohttp.ClientSession] = None
        self._ws: Optional[aiohttp.ClientWebSocketResponse] = None
        self._task: Optional[asyncio.Task] = None
        self._running = False
        self._session_id = ""
        self._muxer = OggOpusMuxer()
        self._start_lock = asyncio.Lock()
        # 会话启动同步机制：_recv_loop 收到响应后设置事件
        self._session_started_ev = asyncio.Event()
        self._session_start_ok = False

        self.on_partial: Optional[Callable[[str], None]] = None
        self.on_final: Optional[Callable[[str], None]] = None
        self.on_segment: Optional[Callable[[AsrSegment], None]] = None
        self.on_error: Optional[Callable[[str], None]] = None

    async def start(self) -> bool:
        """建立 WebSocket 连接（不创建会话，按需创建）"""
        try:
            headers = {
                "X-Api-Key": self._api_key,
                "X-Api-Request-Id": uuid.uuid4().hex,
                "X-Api-Sequence": "-1",
                "X-Api-Resource-Id": "volc.seedasr.sauc.duration",
            }
            self._session = aiohttp.ClientSession()
            try:
                self._ws = await asyncio.wait_for(
                    self._session.ws_connect(self._server_url, headers=headers, heartbeat=30.0, receive_timeout=120.0),
                    timeout=10.0,
                )
            except asyncio.TimeoutError:
                raise ConnectionError("连接超时")
            except aiohttp.ClientConnectorError as e:
                raise ConnectionError(f"无法连接服务器: {e}")
            except aiohttp.WSServerHandshakeError as e:
                raise ConnectionError(f"服务器拒绝连接（{e.status}），请检查 API Key")
            self._running = True

            await self._ws.send_bytes(proto.make_start_connection())
            if not await self._wait_for_event(proto.EVENT_CONNECTION_STARTED):
                raise ConnectionError("ASR 连接未确认")

            self._task = asyncio.create_task(self._recv_loop())
            logger.info("ASR 已连接: %s", self._server_url)
            return True
        except Exception as e:
            logger.error("ASR 连接失败: %s", e)
            if self.on_error:
                self.on_error(f"ASR 连接失败: {e}")
            await self._cleanup()
            return False

    async def start_session(self) -> bool:
        """开始 ASR 会话

        火山引擎每个连接只支持一次识别。录音前重新连接并发送配置。
        """
        async with self._start_lock:
            try:
                self._muxer.reset()
                self._session_id = uuid.uuid4().hex[:16]

                # 停止旧的 recv 循环和连接
                self._running = False
                if self._task:
                    self._task.cancel()
                    try:
                        await self._task
                    except asyncio.CancelledError:
                        pass
                    self._task = None
                await self._cleanup()

                # 新建连接并发送配置
                self._running = True
                headers = {
                    "X-Api-Key": self._api_key,
                    "X-Api-Request-Id": uuid.uuid4().hex,
                    "X-Api-Sequence": "-1",
                    "X-Api-Resource-Id": "volc.seedasr.sauc.duration",
                }
                self._session = aiohttp.ClientSession()
                self._ws = await asyncio.wait_for(
                    self._session.ws_connect(self._server_url, headers=headers,
                                             heartbeat=30.0, receive_timeout=120.0),
                    timeout=10.0,
                )
                await self._ws.send_bytes(proto.make_start_connection())

                # 等待服务器确认（直接接收，不用 recv_loop）
                if not await self._wait_for_event(proto.EVENT_CONNECTION_STARTED, timeout=10.0):
                    raise ConnectionError("ASR 连接未确认")

                # 启动 recv 循环处理后续结果
                self._task = asyncio.create_task(self._recv_loop())
                logger.info("ASR 会话已开始: %s", self._session_id)
                return True

            except asyncio.TimeoutError:
                logger.warning("ASR 会话创建超时")
                await self._cleanup()
                return False
            except Exception as e:
                logger.warning("ASR 会话创建失败: %s", e)
                await self._cleanup()
                return False

    async def stop_session(self):
        """结束 ASR 会话 — 保持连接等待服务器返回结果"""
        self._session_id = ""
        self._muxer.reset()

    async def stop(self):
        """关闭连接"""
        self._running = False
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
            self._task = None
        await self._cleanup()

    async def send_audio(self, opus_data: bytes, is_last: bool = False):
        if not self._ws or self._ws.closed or not self._running:
            return
        try:
            ogg_page = self._muxer.append(opus_data, is_last)
            frame = proto.make_audio_frame(ogg_page, is_last=is_last)
            await self._ws.send_bytes(frame)
        except Exception as e:
            logger.error("发送音频失败: %s", e)

    async def _recv_loop(self):
        while self._running and self._ws and not self._ws.closed:
            try:
                msg = await self._ws.receive()
                if msg.type == WSMsgType.BINARY:
                    self._handle_binary(msg.data)
                elif msg.type in (WSMsgType.CLOSED, WSMsgType.ERROR):
                    logger.info("ASR WebSocket 已断开")
                    break
            except asyncio.CancelledError:
                break
            except Exception as e:
                logger.error("ASR 接收错误: %s", e)
                break
        # 不自动重连 — start_session() 负责按需重建连接

    def _handle_binary(self, data: bytes):
        hex_preview = data[:80].hex()
        logger.info("📨 WS消息 %d字节: %s", len(data), hex_preview)

        # 先尝试事件响应格式
        ev = proto.parse_event_response(data)
        if ev:
            logger.info("  → 事件 id=%d 名称=%s payload=%s",
                         ev.event_id, ev.event_name, ev.payload_text[:200] if ev.payload_text else "(空)")
            self._handle_event(ev)
            return

        # 再尝试通用响应格式
        resp = proto.parse_response(data)
        if resp:
            logger.info("  → 响应 error=%s final=%s text=%s",
                         resp.is_error, resp.is_final, resp.text[:200] if resp.text else "(空)")
            self._handle_asr_response(resp)
            return

        # 无法解析的消息 — 打印 hex 排错
        logger.warning("  → ❌ 无法解析: %s", data.hex()[:200])

    def _handle_event(self, ev: proto.AsrEventResponse):
        if ev.event_id == proto.EVENT_ASR_INFO:
            text = proto._json_extract(ev.payload_text, "text") or ev.payload_text
            if text and self.on_partial:
                self.on_partial(text)
        elif ev.event_id == proto.EVENT_ASR_RESPONSE:
            text = proto._json_extract(ev.payload_text, "text") or ev.payload_text
            if text and self.on_final:
                self.on_final(text)
        elif ev.event_id == proto.EVENT_SESSION_STARTED:
            logger.info("ASR 会话已开始")
            self._session_start_ok = True
            self._session_started_ev.set()
        elif ev.event_id == proto.EVENT_SESSION_FINISHED:
            logger.info("ASR 会话结束")
        elif ev.event_id == proto.EVENT_CONNECTION_FAILED:
            err = proto._json_extract(ev.payload_text, "message") or "ASR 连接失败"
            if self.on_error:
                self.on_error(err)

    async def _restart_session(self):
        """ASR 超时后重启会话（通过重新连接 WebSocket）"""
        logger.info("ASR 会话超时，重新连接…")
        # 复用 start_session 的重连逻辑
        ok = await self.start_session()
        if ok:
            logger.info("ASR 会话重启成功")
        else:
            logger.warning("ASR 会话重启失败")

    def _handle_asr_response(self, resp: proto.AsrResponse):
        if resp.is_error:
            if "Timeout waiting next packet" in resp.text or "session has ended" in resp.text:
                logger.info("ASR 空闲超时（session 已结束，等待下次录音重新创建）")
                # 不自启重启 — 由 _process_audio 在下次录音时创建新会话
            elif self.on_error:
                self.on_error(resp.text)
            return
        if resp.is_final:
            logger.info("ASR 最终结果: %s", resp.text[:200])
            if resp.text and self.on_final:
                self.on_final(resp.text)
        else:
            logger.info("ASR 中间结果: %s", resp.text[:200])
            if resp.text and self.on_partial:
                self.on_partial(resp.text)

    async def _wait_for_event(self, target_event_id: int, timeout: float = 10.0) -> bool:
        """等待服务器响应。支持两种格式：
        1. VoiceStick 事件格式 (flags=0x04, event_id=50)
        2. Volcengine 标准格式 (flags=0x00, body_size + JSON)
        """
        if not self._ws:
            return False
        try:
            msg = await asyncio.wait_for(self._ws.receive(), timeout=timeout)
            if msg.type == WSMsgType.BINARY:
                logger.info("收到消息: %d 字节, 前32字节=%s", len(msg.data), msg.data[:32].hex())
                # 尝试 VoiceStick 事件格式
                ev = proto.parse_event_response(msg.data)
                if ev:
                    logger.info("事件 id=%d payload=%s", ev.event_id, ev.payload_text[:200])
                    return ev.event_id == target_event_id
                # 尝试 Volcengine 标准响应格式
                resp = proto.parse_response(msg.data)
                if resp:
                    logger.info("响应: error=%s text=%s", resp.is_error, resp.text[:200])
                    return not resp.is_error  # 任何非错误的响应都算成功
            return False
        except asyncio.TimeoutError:
            logger.warning("等待事件 %d 超时", target_event_id)
            return False
        except Exception as e:
            logger.warning("等待事件异常: %s", e)
            return False

    async def _cleanup(self):
        if self._ws and not self._ws.closed:
            try:
                await self._ws.close()
            except Exception:
                pass
            self._ws = None
        if self._session:
            await self._session.close()
            self._session = None
