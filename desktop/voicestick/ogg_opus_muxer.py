"""Ogg Opus 封装器 — 将 Opus 帧封装成 Ogg Opus 流"""
import struct
from typing import Optional


def _crc_table() -> list[int]:
    table = []
    for i in range(256):
        crc = i << 24
        for _ in range(8):
            crc = (crc << 1) ^ (0x04C11DB7 if crc & 0x80000000 else 0)
            crc &= 0xFFFFFFFF
        table.append(crc)
    return table

CRC_TABLE = _crc_table()


def _ogg_crc(data: bytes) -> int:
    crc = 0
    for b in data:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ CRC_TABLE[((crc >> 24) ^ b) & 0xFF]
    return crc


SERIAL = 0x5643  # "VC" identifier


class OggOpusMuxer:
    """将 Opus 帧逐个封装成 Ogg Opus 页面"""

    def __init__(self, sample_rate: int = 16000, channels: int = 1):
        self._sample_rate = sample_rate
        self._channels = channels
        self._sequence = 0
        self._granule = 0
        self._wrote_headers = False

    def reset(self):
        self._sequence = 0
        self._granule = 0
        self._wrote_headers = False

    def append(self, opus_payload: bytes, is_last: bool = False) -> bytes:
        """添加一帧 Opus 数据，返回 Ogg 页面字节"""
        out = b""
        if not self._wrote_headers:
            out += self._make_page(self._opus_head(), 0, 0x02)
            out += self._make_page(self._opus_tags(), 0, 0x00)
            self._wrote_headers = True
        # Granule: 每帧 960 个样本 (20ms @ 48kHz), 根据实际采样率换算
        self._granule += 960 * 48000 // self._sample_rate
        flags = 0x04 if is_last else 0x00
        out += self._make_page(opus_payload, self._granule, flags)
        return out

    def finish(self) -> bytes:
        """结束流，返回最终空页面"""
        out = b""
        if not self._wrote_headers:
            out += self._make_page(self._opus_head(), 0, 0x02)
            out += self._make_page(self._opus_tags(), 0, 0x00)
            self._wrote_headers = True
        out += self._make_page(b"", self._granule, 0x04)
        return out

    def _opus_head(self) -> bytes:
        data = b"OpusHead"  # magic
        data += struct.pack("<BB", 1, self._channels)  # version, channels
        data += struct.pack("<H", 312)  # pre-skip
        data += struct.pack("<I", self._sample_rate)  # input sample rate
        data += struct.pack("<h", 0)  # output gain
        data += struct.pack("<B", 0)  # mapping family
        return data

    def _opus_tags(self) -> bytes:
        data = b"OpusTags"  # magic
        vendor = b"VoiceStick"
        data += struct.pack("<I", len(vendor))
        data += vendor
        data += struct.pack("<I", 0)  # user comment list length
        return data

    def _make_page(self, packet: bytes, granule: int, header_type: int) -> bytes:
        page = b"OggS"  # capture pattern
        page += struct.pack("<B", 0)  # version
        page += struct.pack("<B", header_type)  # header type
        page += struct.pack("<Q", granule)  # granule position
        page += struct.pack("<I", SERIAL)  # bitstream serial number
        page += struct.pack("<I", self._sequence)  # page sequence number
        page += struct.pack("<I", 0)  # CRC (placeholder)
        if packet:
            # Ogg格式: page_segments (1B) + lacing_values数组 + 数据段
            # lacing值 255=续段, 0-254=段结束
            segs = []
            rem = len(packet)
            while rem > 255:
                segs.append(255)
                rem -= 255
            segs.append(rem)
            page += struct.pack("<B", len(segs))  # page_segments
            page += struct.pack(f"<{len(segs)}B", *segs)  # lacing_values
            page += packet
        else:
            page += struct.pack("<B", 0)  # page_segments=0 (空页)
        # 计算并填入 CRC
        crc = _ogg_crc(page)
        page = page[:22] + struct.pack("<I", crc) + page[26:]
        self._sequence += 1
        return page
