/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "ble_voice.h"
#include <esp_log.h>

#include <cstring>

#include <esp_mac.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <host/ble_hs.h>
#include <host/util/util.h>
#include <services/gap/ble_svc_gap.h>

// 改编自 VoiceCube firmware/main/ble_service.c（NimBLE GATT peripheral）

namespace framework {

namespace {

constexpr const char* _tag        = "BleVoice";
constexpr uint8_t _frame_type     = 0x01;
constexpr uint16_t _frame_header  = 16;
constexpr uint8_t _state_type     = 0x10;
constexpr uint16_t _max_payload   = 400;  // Opus 帧上限（参照 VoiceCube）

// ---- 128-bit UUIDs (little-endian, 与 VoiceCube 桌面端一致) ----
constexpr ble_uuid128_t _svc_uuid = BLE_UUID128_INIT(
    0x00, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88, 0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);
constexpr ble_uuid128_t _chr_audio_tx = BLE_UUID128_INIT(
    0x01, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88, 0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);
constexpr ble_uuid128_t _chr_state_tx = BLE_UUID128_INIT(
    0x02, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88, 0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);
constexpr ble_uuid128_t _chr_control_rx = BLE_UUID128_INIT(
    0x03, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88, 0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);

bool _initialized     = false;
bool _connected       = false;
uint16_t _conn_handle = 0xffff;
uint16_t _audio_attr_handle = 0;
uint16_t _state_attr_handle = 0;
char _device_name[20] = {};

BleVoice::ControlCallback _control_cb;
BleVoice::ConnectCallback _connect_cb;

int ble_gap_event_cb(struct ble_gap_event* event, void* arg);
int ble_svc_access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt, void* arg);
void nimble_host_task(void* param);

// ---- GATT Service Definition ----
const struct ble_gatt_svc_def _gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){{
                                                           .uuid        = &_chr_audio_tx.u,
                                                           .access_cb   = ble_svc_access_cb,
                                                           .arg = (void*)1,
                                                           .flags = BLE_GATT_CHR_F_NOTIFY,
                                                       },
                                                       {
                                                           .uuid      = &_chr_state_tx.u,
                                                           .access_cb = ble_svc_access_cb,
                                                           .arg = (void*)2,
                                                           .flags = BLE_GATT_CHR_F_NOTIFY,
                                                       },
                                                       {
                                                           .uuid      = &_chr_control_rx.u,
                                                           .access_cb = ble_svc_access_cb,
                                                           .arg = (void*)3,
                                                           .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                                                       },
                                                       {
                                                           0,  // terminator
                                                       }},
    },
    {
        0,  // no more services
    },
};

int ble_svc_access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt, void* arg)
{
    int rc;
    uintptr_t chr_id = (uintptr_t)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        rc = 0;
        break;

    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        if (chr_id == 3 && _control_cb) {
            // control_rx: 桌面端下行（ASR 结果/粘贴回执）
            const uint8_t* data = ctxt->om->om_data;
            size_t len          = ctxt->om->om_len;
            char* json          = static_cast<char*>(malloc(len + 1));
            if (json != nullptr) {
                memcpy(json, data, len);
                json[len] = '\0';
                _control_cb(std::string(json));
                free(json);
            }
        }
        rc = 0;
        break;

    default:
        rc = BLE_ATT_ERR_UNLIKELY;
        break;
    }
    return rc;
}

void start_advertising()
{
    ble_gap_adv_stop();  // 防止 BLE_HS_EBUSY

    struct ble_hs_adv_fields adv_fields;
    memset(&adv_fields, 0, sizeof(adv_fields));
    adv_fields.name                = (uint8_t*)ble_svc_gap_device_name();
    adv_fields.name_len            = strlen(ble_svc_gap_device_name());
    adv_fields.name_is_complete    = 1;
    adv_fields.uuids128            = (ble_uuid128_t*)&_svc_uuid;
    adv_fields.num_uuids128        = 1;
    adv_fields.uuids128_is_complete = 1;
    adv_fields.tx_pwr_lvl_is_present = 1;
    adv_fields.tx_pwr_lvl          = -3;

    if (ble_gap_adv_set_fields(&adv_fields) != 0) {
        ESP_LOGE(_tag, "set adv fields failed");
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min  = 160;  // 快速广播（~100ms）
    adv_params.itvl_max  = 200;

    int rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, nullptr, BLE_HS_FOREVER, &adv_params, ble_gap_event_cb, nullptr);
    if (rc == 0) {
        ESP_LOGI(_tag, "advertising started as %s", _device_name);
    } else {
        ESP_LOGE(_tag, "advertising start failed: %d", rc);
    }
}

int ble_gap_event_cb(struct ble_gap_event* event, void* arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            _conn_handle = event->connect.conn_handle;
            _connected   = true;
            ESP_LOGI(_tag, "connected, handle=%d", _conn_handle);
            if (_connect_cb) {
                _connect_cb(true);
            }
            // 8s 监督超时：音频流压力下够长，桌面端消失后能快速发现并重新广播
            struct ble_gap_upd_params params = {
                .itvl_min = 12,
                .itvl_max = 24,
                .latency = 0,
                .supervision_timeout = 800,
                .min_ce_len = 0,
                .max_ce_len = 0,
            };
            ble_gap_update_params(_conn_handle, &params);
        } else {
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(_tag, "disconnected, reason=%d", event->disconnect.reason);
        _connected   = false;
        _conn_handle = 0xffff;
        if (_connect_cb) {
            _connect_cb(false);
        }
        start_advertising();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        {
            int cur_notify = event->subscribe.cur_notify;
            ESP_LOGI(_tag, "subscribe handle=%d cur=%d", event->subscribe.attr_handle, cur_notify);
        }
        break;

    default:
        break;
    }
    return 0;
}

void ble_sync_cb(void)
{
    int rc = ble_gatts_find_chr(&_svc_uuid.u, &_chr_audio_tx.u, nullptr, &_audio_attr_handle);
    if (rc != 0) {
        ESP_LOGW(_tag, "audio_tx handle lookup: %d", rc);
    }
    rc = ble_gatts_find_chr(&_svc_uuid.u, &_chr_state_tx.u, nullptr, &_state_attr_handle);
    if (rc != 0) {
        ESP_LOGW(_tag, "state_tx handle lookup: %d", rc);
    }
    ESP_LOGI(_tag, "handles: audio=%d state=%d", _audio_attr_handle, _state_attr_handle);
    start_advertising();
}

void ble_reset_cb(int reason)
{
    ESP_LOGW(_tag, "BLE host reset: %d", reason);
}

void nimble_host_task(void* param)
{
    (void)param;
    nimble_port_run();  // blocks until nimble_port_stop()
    nimble_port_freertos_deinit();
}

}  // namespace

BleVoice& BleVoice::get()
{
    static BleVoice _instance;
    return _instance;
}

bool BleVoice::start()
{
    if (!_initialized) {
        // 设备名 VS-XXXX（MAC 后 2 字节）
        uint8_t mac[6] = {};
        esp_read_mac(mac, ESP_MAC_BT);
        snprintf(_device_name, sizeof(_device_name), "VS%02X%02X", mac[4], mac[5]);

        int rc = nimble_port_init();
        if (rc != 0) {
            ESP_LOGE(_tag, "nimble_port_init failed: %d", rc);
            return false;
        }

        ble_hs_cfg.reset_cb = ble_reset_cb;
        ble_hs_cfg.sync_cb  = ble_sync_cb;

        rc = ble_gatts_count_cfg(_gatt_svcs);
        if (rc != 0) {
            ESP_LOGE(_tag, "ble_gatts_count_cfg failed: %d", rc);
            return false;
        }
        rc = ble_gatts_add_svcs(_gatt_svcs);
        if (rc != 0) {
            ESP_LOGE(_tag, "ble_gatts_add_svcs failed: %d", rc);
            return false;
        }
        ble_svc_gap_device_name_set(_device_name);
        nimble_port_freertos_init(nimble_host_task);
        _initialized = true;
        ESP_LOGI(_tag, "NimBLE service initialized: %s", _device_name);
    } else {
        // 已初始化：重新广播（断开后或模式重入）
        start_advertising();
    }
    return true;
}

void BleVoice::stopAdvertising()
{
    if (_initialized) {
        ble_gap_adv_stop();
    }
}

bool BleVoice::sendAudioFrame(const uint8_t* payload, uint16_t len, uint32_t sessionId, uint32_t seq, uint8_t flags)
{
    if (!_connected || len > _max_payload) {
        return false;
    }
    uint16_t frame_size = _frame_header + len;
    uint8_t* frame      = static_cast<uint8_t*>(malloc(frame_size));
    if (frame == nullptr) {
        return false;
    }
    auto* hdr           = reinterpret_cast<BleAudioFrameHeader*>(frame);
    hdr->version        = 1;
    hdr->type           = _frame_type;
    hdr->header_len     = _frame_header;
    hdr->session_id     = sessionId;
    hdr->seq            = seq;
    hdr->flags          = flags;
    hdr->reserved       = 0;
    hdr->payload_len    = len;
    if (len > 0) {
        memcpy(frame + _frame_header, payload, len);
    }

    struct os_mbuf* om = ble_hs_mbuf_from_flat(frame, frame_size);
    free(frame);
    if (om == nullptr) {
        return false;
    }
    int rc = ble_gatts_notify_custom(_conn_handle, _audio_attr_handle, om);
    if (rc != 0) {
        ESP_LOGW(_tag, "notify audio failed: %d", rc);
        return false;
    }
    return true;
}

bool BleVoice::sendStateJson(const std::string& json)
{
    if (!_connected) {
        return false;
    }
    struct os_mbuf* om = ble_hs_mbuf_from_flat(json.data(), json.size());
    if (om == nullptr) {
        return false;
    }
    int rc = ble_gatts_notify_custom(_conn_handle, _state_attr_handle, om);
    if (rc != 0) {
        ESP_LOGW(_tag, "notify state failed: %d", rc);
        return false;
    }
    return true;
}

bool BleVoice::isConnected() const
{
    return _connected;
}

const char* BleVoice::deviceName() const
{
    return _device_name;
}

void BleVoice::setControlCallback(ControlCallback cb)
{
    _control_cb = std::move(cb);
}

void BleVoice::setConnectCallback(ConnectCallback cb)
{
    _connect_cb = std::move(cb);
}

}  // namespace framework
