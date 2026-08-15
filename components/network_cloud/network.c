#include "network.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "peripheral_config.h"
#include <string.h>

// UDP Logging
#include "lwip/sockets.h"
#include <stdarg.h>

static int g_udp_log_sock = -1;
static struct sockaddr_in g_udp_log_addr;
static vprintf_like_t s_original_vprintf = NULL;

#define LOG_PREFIX "[PER] " 

static int udp_log_vprintf(const char *fmt, va_list args) {
    char buf[512];
    int len = vsnprintf(buf, sizeof(buf)-1, fmt, args);
    if (len > 0 && g_udp_log_sock >= 0) {
        char packet[530];
        int plen = snprintf(packet, sizeof(packet), "%s%s", LOG_PREFIX, buf);
        sendto(g_udp_log_sock, packet, plen, MSG_DONTWAIT, (struct sockaddr *)&g_udp_log_addr, sizeof(g_udp_log_addr));
    }
    if (s_original_vprintf) {
        return s_original_vprintf(fmt, args);
    }
    return len;
}

static void start_udp_logging() {
    if (g_udp_log_sock >= 0) return; // Already started

    g_udp_log_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (g_udp_log_sock < 0) return;
    
    int flags = fcntl(g_udp_log_sock, F_GETFL, 0);
    fcntl(g_udp_log_sock, F_SETFL, flags | O_NONBLOCK);

    int opt_val = 1;
    setsockopt(g_udp_log_sock, SOL_SOCKET, SO_BROADCAST, &opt_val, sizeof(opt_val));

    g_udp_log_addr.sin_family = AF_INET;
    g_udp_log_addr.sin_port = htons(5555);
    g_udp_log_addr.sin_addr.s_addr = inet_addr("255.255.255.255"); // Broadcast
    
    s_original_vprintf = esp_log_set_vprintf(udp_log_vprintf);
    ESP_LOGI("UDP_LOG", "Started UDP Broadcast Logging on port 5555");
}

static const char *TAG = "PERIPHERAL_NET";

#include "secrets.h"

static bool s_wifi_connected = false;

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        ESP_LOGI(TAG, "Network initialization completed.");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;
    }
}

void network_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi init finished. Connecting to %s...", WIFI_SSID);
}

bool network_is_connected(void) {
    return s_wifi_connected;
}

esp_err_t network_upload_audio_to_cloud(const uint8_t *pcm_buf, size_t pcm_size) {
    if (!pcm_buf || pcm_size == 0) return ESP_FAIL;
    if (!s_wifi_connected) {
        ESP_LOGE(TAG, "WiFi not connected!");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "[AUD] Sending HTTP POST to %s (size: %zu bytes)", HF_SPACE_URL, pcm_size);

    esp_http_client_config_t config = {
        .url = HF_SPACE_URL,
        .timeout_ms = 60000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    const char *boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
    char content_type[128];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_method(client, HTTP_METHOD_POST);

    const char *header_part_fmt = 
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"audio_file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    char header_part[256];
    snprintf(header_part, sizeof(header_part), header_part_fmt, boundary);

    const char *footer_part_fmt = "\r\n--%s--\r\n";
    char footer_part[128];
    snprintf(footer_part, sizeof(footer_part), footer_part_fmt, boundary);

    uint8_t wav_header[44] = {
        'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
        16, 0, 0, 0, 1, 0, 1, 0, 0x80, 0x3E, 0x00, 0x00, 0x00, 0x7D, 0x00, 0x00, 
        2, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0 
    };
    uint32_t data_len = pcm_size;
    uint32_t file_len = data_len + 36;
    memcpy(&wav_header[4], &file_len, 4);
    memcpy(&wav_header[40], &data_len, 4);

    int total_len = strlen(header_part) + sizeof(wav_header) + pcm_size + strlen(footer_part);

    esp_err_t err = esp_http_client_open(client, total_len);
    if (err == ESP_OK) {
        esp_http_client_write(client, header_part, strlen(header_part));
        esp_http_client_write(client, (const char *)wav_header, sizeof(wav_header));
        esp_http_client_write(client, (const char *)pcm_buf, pcm_size);
        esp_http_client_write(client, footer_part, strlen(footer_part));

        ESP_LOGI(TAG, "[AUD] Uploaded. Waiting for AI response...");
        int content_length = esp_http_client_fetch_headers(client);
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP POST Status = %d, length = %d", status_code, content_length);

        if (status_code == 200) {
            char read_buf[2048];
            int read_len;
            bool is_first_chunk = true;
            extern void audio_feed_ringbuffer(const uint8_t *data, size_t len);
            
            while ((read_len = esp_http_client_read(client, read_buf, sizeof(read_buf))) > 0) {
                int offset = 0;
                if (is_first_chunk && read_len >= 44) {
                    if (strncmp(read_buf, "RIFF", 4) == 0) offset = 44;
                    is_first_chunk = false;
                }
                if (read_len - offset > 0) {
                    audio_feed_ringbuffer((const uint8_t *)(read_buf + offset), read_len - offset);
                }
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}
