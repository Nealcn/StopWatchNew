#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_


#include "protocol.h"

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();
    ~WebsocketProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;

private:
    EventGroupHandle_t event_group_handle_;
    std::unique_ptr<WebSocket> websocket_;
    int version_ = 1;

    // ==== 空闲保活: 断线自动重连 ====
    bool deliberate_close_ = false;       // 主动关闭(不重连)
    TaskHandle_t reconnect_task_ = nullptr;
    int reconnect_delay_sec_ = 30;        // 重连退避: 30s 起步, 上限 300s

    // ==== 连接保活: 定时 WebSocket Ping ====
    TaskHandle_t keep_alive_task_ = nullptr;  // 每 30s 发 ping 防 NAT 断线

    bool ConnectInternal();
    void ScheduleReconnect();
    static void ReconnectTask(void* arg);
    static void KeepAliveTask(void* arg);

    void ParseServerHello(const cJSON* root);
    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
};

#endif
