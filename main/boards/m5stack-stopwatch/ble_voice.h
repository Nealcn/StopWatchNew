/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <functional>
#include <string>

/**
 * @brief VoiceCube 桌面模式 BLE 服务（NimBLE GATT peripheral）
 *
 * 协议与 Nealcn/VoiceCube 完全兼容（UUID/帧格式一致），桌面端可复用其 bleak 客户端：
 *   Service:    8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100
 *   audio_tx:   8f2f0b84-...5101  (notify)  音频帧（16B 头 + Opus payload）
 *   state_tx:   8f2f0b84-...5102  (notify)  状态/鼠标事件 JSON
 *   control_rx: 8f2f0b84-...5103  (write)   桌面端下行（ASR 结果/粘贴回执）
 *
 * 本组件改编自 VoiceCube firmware/main/ble_service.c。
 * 生命周期：仅在进入 VoiceCube 桌面模式时 init/start，退出 stop（省电）。
 */
namespace framework {

struct BleAudioFrameHeader {
    uint8_t version;      // 1
    uint8_t type;         // 0x01 audio
    uint16_t header_len;  // 16
    uint32_t session_id;
    uint32_t seq;
    uint8_t flags;  // bit0=start, bit1=end
    uint8_t reserved;
    uint16_t payload_len;
} __attribute__((packed));

class BleVoice {
public:
    using ControlCallback = std::function<void(const std::string& json)>;
    using ConnectCallback = std::function<void(bool connected)>;

    static BleVoice& get();

    /* 初始化 NimBLE 并开始广播（设备名 VS-XXXX），可重复调用（已初始化则重启广播） */
    bool start();
    /* 停止广播（NimBLE 栈常驻，仅停广告；栈完全关闭待编译机验证后补充） */
    void stopAdvertising();

    /* 音频帧上行（16B 头 + Opus payload，len <= 400 参照 VoiceCube） */
    bool sendAudioFrame(const uint8_t* payload, uint16_t len, uint32_t sessionId, uint32_t seq, uint8_t flags);
    /* 状态/鼠标 JSON 上行 */
    bool sendStateJson(const std::string& json);

    bool isConnected() const;
    const char* deviceName() const;

    void setControlCallback(ControlCallback cb);
    void setConnectCallback(ConnectCallback cb);

private:
    BleVoice()          = default;
    ~BleVoice()         = default;
    BleVoice(const BleVoice&)            = delete;
    BleVoice& operator=(const BleVoice&) = delete;
};

}  // namespace framework
