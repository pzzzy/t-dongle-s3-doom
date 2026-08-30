// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_event.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "esp_input_cheat.h"
#include "esp_fidget.h"
#include "esp_input_remote.h"
#include "esp_video_settings.h"
#include "esp_web_control.h"
#include "esp_web_audio.h"

static const char *TAG = "doom_web";
static httpd_handle_t server;
static atomic_int active_ws_fd = ATOMIC_VAR_INIT(-1);
static atomic_bool audio_flush_scheduled = ATOMIC_VAR_INIT(false);
static portMUX_TYPE audio_queue_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_partition_mmap_handle_t audio_mmap_handle;
static const uint8_t *audio_pack;
static size_t audio_pack_size;

#define AUDIO_QUEUE_CAPACITY 64
#define AUDIO_PACKET_MAX 12
typedef struct {
    uint8_t length;
    uint8_t data[AUDIO_PACKET_MAX];
} audio_packet_t;
static audio_packet_t audio_queue[AUDIO_QUEUE_CAPACITY];
static uint8_t audio_queue_head;
static uint8_t audio_queue_tail;
#if CONFIG_TD_USB_HOST
#define USB_HOST_JSON "true"
#else
#define USB_HOST_JSON "false"
#endif
#if CONFIG_TD_AUDIO_PDM || CONFIG_TD_WEB_AUDIO
#define AUDIO_JSON "true"
#else
#define AUDIO_JSON "false"
#endif

static void flush_audio_work(void *arg);

static void schedule_audio_flush(void)
{
    if (!server || atomic_load_explicit(&active_ws_fd, memory_order_acquire) < 0)
        return;
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &audio_flush_scheduled, &expected, true,
            memory_order_acq_rel, memory_order_relaxed)) return;
    if (httpd_queue_work(server, flush_audio_work, NULL) != ESP_OK)
        atomic_store_explicit(&audio_flush_scheduled, false,
                              memory_order_release);
}

void esp_web_audio_publish(const uint8_t *message, size_t length)
{
    if (!message || !length || length > AUDIO_PACKET_MAX ||
        atomic_load_explicit(&active_ws_fd, memory_order_acquire) < 0) return;
    portENTER_CRITICAL(&audio_queue_lock);
    uint8_t next = (uint8_t)((audio_queue_head + 1) % AUDIO_QUEUE_CAPACITY);
    if (next == audio_queue_tail)
        audio_queue_tail = (uint8_t)((audio_queue_tail + 1) % AUDIO_QUEUE_CAPACITY);
    audio_packet_t *packet = &audio_queue[audio_queue_head];
    packet->length = (uint8_t)length;
    memcpy(packet->data, message, length);
    audio_queue_head = next;
    portEXIT_CRITICAL(&audio_queue_lock);
    schedule_audio_flush();
}

void esp_web_audio_reset_transport(void)
{
    portENTER_CRITICAL(&audio_queue_lock);
    audio_queue_tail = audio_queue_head;
    portEXIT_CRITICAL(&audio_queue_lock);
}

static void flush_audio_work(void *arg)
{
    (void)arg;
    for (;;) {
        audio_packet_t packet;
        int have_packet = 0;
        portENTER_CRITICAL(&audio_queue_lock);
        if (audio_queue_tail != audio_queue_head) {
            packet = audio_queue[audio_queue_tail];
            audio_queue_tail = (uint8_t)((audio_queue_tail + 1) %
                                         AUDIO_QUEUE_CAPACITY);
            have_packet = 1;
        }
        portEXIT_CRITICAL(&audio_queue_lock);
        if (!have_packet) break;

        int fd = atomic_load_explicit(&active_ws_fd, memory_order_acquire);
        if (fd < 0) break;
        httpd_ws_frame_t frame = {
            .final = true,
            .fragmented = false,
            .type = HTTPD_WS_TYPE_BINARY,
            .payload = packet.data,
            .len = packet.length,
        };
        if (httpd_ws_send_frame_async(server, fd, &frame) != ESP_OK) break;
    }
    atomic_store_explicit(&audio_flush_scheduled, false, memory_order_release);
    portENTER_CRITICAL(&audio_queue_lock);
    int pending = audio_queue_tail != audio_queue_head;
    portEXIT_CRITICAL(&audio_queue_lock);
    if (pending) schedule_audio_flush();
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
           (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static void map_audio_pack(void)
{
    const esp_partition_t *partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "audio");
    if (!partition) {
        ESP_LOGW(TAG, "browser audio partition not present");
        return;
    }
    const void *mapped = NULL;
    if (esp_partition_mmap(partition, 0, partition->size,
                           ESP_PARTITION_MMAP_DATA, &mapped,
                           &audio_mmap_handle) != ESP_OK) {
        ESP_LOGW(TAG, "could not map browser audio partition");
        return;
    }
    const uint8_t *pack = mapped;
    if (memcmp(pack, "DWAP", 4) || pack[4] != 1) {
        ESP_LOGW(TAG, "browser audio pack missing; flash it at 0x%lx",
                 (unsigned long)partition->address);
        esp_partition_munmap(audio_mmap_handle);
        audio_mmap_handle = 0;
        return;
    }
    unsigned sfx_count = pack[6] | pack[7] << 8;
    unsigned music_count = pack[8] | pack[9] << 8;
    unsigned entry_size = pack[10] | pack[11] << 8;
    unsigned entries = sfx_count + music_count;
    if (entry_size != 16 || 16 + entries * entry_size > partition->size) {
        ESP_LOGW(TAG, "invalid browser audio pack table");
        esp_partition_munmap(audio_mmap_handle);
        audio_mmap_handle = 0;
        return;
    }
    size_t used = read_le32(pack + 12);
    for (unsigned i = 0; i < entries; ++i) {
        const uint8_t *entry = pack + 16 + i * entry_size;
        size_t end = (size_t)read_le32(entry) + read_le32(entry + 4);
        if (end > used) used = end;
    }
    if (used > partition->size) {
        ESP_LOGW(TAG, "browser audio pack exceeds partition");
        esp_partition_munmap(audio_mmap_handle);
        audio_mmap_handle = 0;
        return;
    }
    audio_pack = pack;
    audio_pack_size = used;
    ESP_LOGI(TAG, "browser audio pack: %u bytes", (unsigned)used);
}

static void load_settings(void)
{
    nvs_handle_t nvs;
    uint8_t brightness = 100;
    uint8_t rotated = 0;
    if (nvs_open("doom", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "brightness", &brightness);
        nvs_get_u8(nvs, "rotated", &rotated);
        nvs_close(nvs);
    }
    esp_video_set_brightness(brightness);
    esp_video_set_rotated(rotated);
}

static esp_err_t settings_handler(httpd_req_t *req)
{
    if (req->method == HTTP_POST) {
        if (req->content_len <= 0 || req->content_len >= 96)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "invalid settings body");
        char body[96];
        int received = httpd_req_recv(req, body, req->content_len);
        if (received <= 0) return ESP_FAIL;
        body[received] = 0;
        bool changed = false;
        char *field = strstr(body, "\"brightness\"");
        if (field && (field = strchr(field, ':'))) {
            char *end;
            long value = strtol(field + 1, &end, 10);
            if (end == field + 1 || value < 5 || value > 100)
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                           "brightness must be 5..100");
            esp_video_set_brightness((unsigned)value);
            changed = true;
        }

        field = strstr(body, "\"rotated\"");
        if (field && (field = strchr(field, ':'))) {
            ++field;
            while (*field == ' ' || *field == '\t') ++field;
            if (!strncmp(field, "true", 4)) esp_video_set_rotated(1);
            else if (!strncmp(field, "false", 5)) esp_video_set_rotated(0);
            else return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                            "rotated must be boolean");
            changed = true;
        }
        if (!changed)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "no recognized setting");

        nvs_handle_t nvs;
        if (nvs_open("doom", NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_u8(nvs, "brightness",
                       (uint8_t)esp_video_get_brightness());
            nvs_set_u8(nvs, "rotated", (uint8_t)esp_video_get_rotated());
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    }

    char json[144];
    int length = snprintf(json, sizeof(json),
                          "{\"brightness\":%u,\"rotated\":%s,\"usbHostBuild\":%s,"
                          "\"audio\":%s}",
                          esp_video_get_brightness(),
                          esp_video_get_rotated() ? "true" : "false",
                          USB_HOST_JSON, AUDIO_JSON);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, length);
}

extern const unsigned char web_index_html_start[]
        asm("_binary_index_html_start");
extern const unsigned char web_index_html_end[]
        asm("_binary_index_html_end");

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)web_index_html_start,
                           web_index_html_end - web_index_html_start);
}

static esp_err_t audio_pack_handler(httpd_req_t *req)
{
    if (!audio_pack || !audio_pack_size)
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                                   "audio pack is not flashed");

    size_t start = 0;
    size_t end = audio_pack_size - 1;
    int partial = 0;
    size_t header_length = httpd_req_get_hdr_value_len(req, "Range");
    if (header_length && header_length < 64) {
        char range[64];
        if (httpd_req_get_hdr_value_str(req, "Range", range,
                                        sizeof(range)) == ESP_OK &&
            !strncmp(range, "bytes=", 6)) {
            char *cursor = range + 6;
            char *parse_end;
            unsigned long first = strtoul(cursor, &parse_end, 10);
            if (parse_end != cursor && *parse_end == '-') {
                start = first;
                cursor = parse_end + 1;
                if (*cursor) {
                    unsigned long last = strtoul(cursor, &parse_end, 10);
                    if (parse_end != cursor) end = last;
                }
                partial = 1;
            }
        }
    }
    if (start >= audio_pack_size || start > end) {
        httpd_resp_set_status(req, "416 Range Not Satisfiable");
        return httpd_resp_sendstr(req, "range outside audio pack");
    }
    if (end >= audio_pack_size) end = audio_pack_size - 1;

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
    if (partial) {
        char content_range[48];
        snprintf(content_range, sizeof(content_range), "bytes %u-%u/%u",
                 (unsigned)start, (unsigned)end, (unsigned)audio_pack_size);
        httpd_resp_set_status(req, "206 Partial Content");
        httpd_resp_set_hdr(req, "Content-Range", content_range);
    }
    return httpd_resp_send(req, (const char *)audio_pack + start,
                           end - start + 1);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char json[256];
    unsigned free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                  MALLOC_CAP_8BIT);
    unsigned largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                         MALLOC_CAP_8BIT);
    int length = snprintf(json, sizeof(json),
                          "{\"version\":\"%s\",\"fps\":35,"
                          "\"heap\":%u,\"largest\":%u,\"remote\":%s,"
                          "\"fidget\":%s,\"kills\":%u,\"weapon\":\"%s\"}",
                          esp_app_get_description()->version, free_heap, largest,
                          esp_input_remote_active() ? "true" : "false",
                          esp_fidget_active() ? "true" : "false",
                          esp_fidget_kills(), esp_fidget_weapon_name());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, length);
}

static bool valid_cheat(const char *code)
{
    static const char *const fixed[] = {
        "iddqd", "idkfa", "idfa", "idspispopd", "iddt", "idbehold",
        "idbeholdv", "idbeholds", "idbeholdi", "idbeholdr",
        "idbeholda", "idbeholdl", "idchoppers", "idmypos",
    };
    for (unsigned i = 0; i < sizeof(fixed) / sizeof(fixed[0]); ++i)
        if (!strcmp(code, fixed[i])) return true;

    size_t prefix;
    if (!strncmp(code, "idclev", 6)) prefix = 6;
    else if (!strncmp(code, "idmus", 5)) prefix = 5;
    else return false;

    // Ultimate DOOM has episodes 1..4 and maps 1..9.
    return strlen(code) == prefix + 2
        && code[prefix] >= '1' && code[prefix] <= '4'
        && code[prefix + 1] >= '1' && code[prefix + 1] <= '9';
}

static esp_err_t cheat_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= 24)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "invalid cheat");

    char code[24];
    int total = 0;
    while (total < req->content_len) {
        int received = httpd_req_recv(req, code + total,
                                      req->content_len - total);
        if (received <= 0) return ESP_FAIL;
        total += received;
    }
    code[total] = 0;
    if (!valid_cheat(code))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "unsupported cheat");

    esp_err_t result = esp_input_queue_cheat(code);
    if (result == ESP_ERR_NO_MEM)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "cheat queue busy");
    if (result != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "invalid cheat");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "{\"queued\":true}");
}

static esp_err_t websocket_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        atomic_store_explicit(&active_ws_fd, fd, memory_order_release);
        esp_input_remote_disconnect();
        esp_web_audio_reset_transport();
        esp_web_audio_sync_state();
        ESP_LOGI(TAG, "controller connected on socket %d", fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    esp_err_t result = httpd_ws_recv_frame(req, &frame, 0);
    if (result != ESP_OK) return result;

    uint8_t payload[8];
    if (frame.len > sizeof(payload)) return ESP_ERR_INVALID_SIZE;
    frame.payload = payload;
    result = httpd_ws_recv_frame(req, &frame, frame.len);
    if (result != ESP_OK) return result;

    if (frame.type == HTTPD_WS_TYPE_BINARY && frame.len == 4) {
        uint32_t mask = (uint32_t)payload[0] |
                        (uint32_t)payload[1] << 8 |
                        (uint32_t)payload[2] << 16 |
                        (uint32_t)payload[3] << 24;
        esp_input_remote_update(mask);
        static const uint8_t ack_payload = 0xac;
        httpd_ws_frame_t ack = {
            .final = true,
            .fragmented = false,
            .type = HTTPD_WS_TYPE_BINARY,
            .payload = (uint8_t *)&ack_payload,
            .len = 1,
        };
        result = httpd_ws_send_frame(req, &ack);
    }
    return result;
}

static void close_session(httpd_handle_t handle, int sockfd)
{
    (void)handle;
    int active = atomic_load_explicit(&active_ws_fd, memory_order_acquire);
    if (sockfd == active) {
        esp_input_remote_disconnect();
        esp_web_audio_reset_transport();
        atomic_store_explicit(&active_ws_fd, -1, memory_order_release);
        ESP_LOGI(TAG, "controller disconnected; all remote keys released");
    }
    close(sockfd);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id,
                       void *event_data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *event = event_data;
        ESP_LOGI(TAG, "station connected, AID=%d", event->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *event = event_data;
        ESP_LOGI(TAG, "station disconnected, AID=%d", event->aid);
        esp_input_remote_disconnect();
    }
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 4096;
    /* Safari commonly holds its WebSocket plus two keep-alive HTTP lanes, and
     * a reload briefly overlaps the old generation. Five slots prevent LRU
     * eviction of the live control socket during audio range traffic. */
    config.max_open_sockets = 5;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;
    config.close_fn = close_session;

    esp_err_t result = httpd_start(&server, &config);
    if (result != ESP_OK) return result;

    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
    };
    const httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_handler,
    };
    const httpd_uri_t websocket_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = websocket_handler,
        .is_websocket = true,
    };
    const httpd_uri_t settings_get_uri = {
        .uri = "/api/settings",
        .method = HTTP_GET,
        .handler = settings_handler,
    };
    const httpd_uri_t settings_post_uri = {
        .uri = "/api/settings",
        .method = HTTP_POST,
        .handler = settings_handler,
    };
    const httpd_uri_t cheat_uri = {
        .uri = "/api/cheat",
        .method = HTTP_POST,
        .handler = cheat_handler,
    };
    const httpd_uri_t audio_pack_uri = {
        .uri = "/audio.pack",
        .method = HTTP_GET,
        .handler = audio_pack_handler,
    };

    if ((result = httpd_register_uri_handler(server, &index_uri)) != ESP_OK ||
        (result = httpd_register_uri_handler(server, &status_uri)) != ESP_OK ||
        (result = httpd_register_uri_handler(server, &websocket_uri)) != ESP_OK ||
        (result = httpd_register_uri_handler(server, &settings_get_uri)) != ESP_OK ||
        (result = httpd_register_uri_handler(server, &settings_post_uri)) != ESP_OK ||
        (result = httpd_register_uri_handler(server, &cheat_uri)) != ESP_OK ||
        (result = httpd_register_uri_handler(server, &audio_pack_uri)) != ESP_OK) {
        httpd_stop(server);
        server = NULL;
    }
    return result;
}

esp_err_t esp_web_control_start(void)
{
    static int attempted;
    if (attempted) return server ? ESP_OK : ESP_FAIL;
    attempted = 1;

    esp_err_t result = nvs_flash_init();
    if (result != ESP_OK) return result;

    result = esp_netif_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
    if (!esp_netif_create_default_wifi_ap()) return ESP_ERR_NO_MEM;

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if ((result = esp_wifi_init(&init)) != ESP_OK) return result;
    if ((result = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             wifi_event, NULL)) != ESP_OK)
        return result;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    wifi_config_t config = {0};
    snprintf((char *)config.ap.ssid, sizeof(config.ap.ssid),
             "T-Dongle-DOOM-%02X%02X", mac[4], mac[5]);
    config.ap.ssid_len = strlen((char *)config.ap.ssid);
    memcpy(config.ap.password, "ripandtear", sizeof("ripandtear"));
    config.ap.channel = 6;
    config.ap.max_connection = 1;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.pmf_cfg.required = false;

    if ((result = esp_wifi_set_mode(WIFI_MODE_AP)) != ESP_OK ||
        (result = esp_wifi_set_config(WIFI_IF_AP, &config)) != ESP_OK ||
        (result = esp_wifi_start()) != ESP_OK)
        return result;
    load_settings();
    map_audio_pack();
    if ((result = start_http_server()) != ESP_OK) return result;

    printf("WEB: join %s, password ripandtear, open http://192.168.4.1/\n",
           config.ap.ssid);
    printf("WEB: free internal heap: %u, largest block: %u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                             MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                      MALLOC_CAP_8BIT));
    return ESP_OK;
}
