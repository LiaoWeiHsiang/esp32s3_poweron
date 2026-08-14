/**
 * @file main.c
 * @brief ESP32-S3 PC Power Controller (Tailscale REST API Status Monitoring)
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "cJSON.h"

#include "microlink.h"
#include "ml_config_httpd.h"

static const char *TAG = "main";

/* === Tailscale API 設定 ===
 * The API key is supplied through sdkconfig (git-ignored), never committed
 * in source code. Configure CONFIG_ML_TAILSCALE_API_KEY in menuconfig or
 * sdkconfig.credentials before building.
 */
#define TAILSCALE_TAILNET      "zxc741852741@gmail.com"  // Tailnet 名稱/Email

/* === 繼電器硬體設定 === */
#define RELAY_GPIO           GPIO_NUM_4   // 繼電器控制腳位
#define RELAY_ACTIVE_LEVEL   1            // 0: 低電位觸發, 1: 高電位觸發
#define RELAY_INACTIVE_LEVEL (!RELAY_ACTIVE_LEVEL)

/* === 板載 RGB LED === */
#define BOARD_RGB_GPIO       GPIO_NUM_48 

static TimerHandle_t relay_timer = NULL;
static httpd_handle_t server_handle = NULL;

/* WiFi network list — firmware-baked defaults (always tried first, not
 * user-editable) followed by the web UI's NVS-backed list (lower priority,
 * editable at http://<vpn-ip>/ -> WiFi Networks). Tries each in order until
 * one connects, and keeps cycling through the combined list on disconnect. */
#define WIFI_DEFAULT_COUNT   4
#define WIFI_MAX_NETWORKS    (WIFI_DEFAULT_COUNT + ML_CONFIG_MAX_WIFI_ENTRIES)

typedef struct {
    char ssid[33];
    char password[65];
} wifi_cred_t;

/* Built-in default networks — tried before anything added via the web UI. */
static const wifi_cred_t k_default_wifi_creds[WIFI_DEFAULT_COUNT] = {
    { "EnglishTsai",     "22222222" },
    { "Developer",       "22222222" },
    { "wifi-671",        "035596933" },
    { "wifi-671_2.4G",   "035596933" },
    //{ "Hydra",           "K5x48Vz3" },
};

static wifi_cred_t g_wifi_creds[WIFI_MAX_NETWORKS];
static size_t g_wifi_count = 0;
static size_t g_wifi_next_idx = 0;

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static microlink_t *ml = NULL;
static bool g_ml_started = false;

/* ============================================================================
 * 系統日誌與狀態追蹤 (System Tracker)
 * ========================================================================== */
typedef struct {
    uint32_t last_trigger_ms;
    char last_action[32];
    uint32_t total_triggers;
} relay_log_t;

static relay_log_t g_relay_log = {
    .last_trigger_ms = 0,
    .last_action = "無",
    .total_triggers = 0
};

/* ============================================================================
 * Tailscale API 裝置狀態結構與動態快取
 * ========================================================================== */
#define MAX_TS_DEVICES 16

typedef struct {
    char hostname[64];
    char user[64];
    char ip_str[32];
    bool is_online;
    char last_seen[32];
    bool is_self;
} ts_device_t;

static ts_device_t g_pcs[MAX_TS_DEVICES];
static size_t g_pc_count = 0;
static SemaphoreHandle_t g_pcs_mutex = NULL;

/* ============================================================================
 * Tailscale REST API 抓取邏輯
 * ========================================================================== */

typedef struct {
    char *buffer;
    int buffer_len;
} http_response_buffer_t;

esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    http_response_buffer_t *resp_buf = (http_response_buffer_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (resp_buf && resp_buf->buffer) {
            if (resp_buf->buffer_len + evt->data_len < 16384) {
                memcpy(resp_buf->buffer + resp_buf->buffer_len, evt->data, evt->data_len);
                resp_buf->buffer_len += evt->data_len;
                resp_buf->buffer[resp_buf->buffer_len] = '\0';
            }
        }
    }
    return ESP_OK;
}

static void parse_tailscale_json(const char *json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse Tailscale API JSON");
        return;
    }

    cJSON *devices = cJSON_GetObjectItem(root, "devices");
    if (!cJSON_IsArray(devices)) {
        cJSON_Delete(root);
        return;
    }

    if (xSemaphoreTake(g_pcs_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        g_pc_count = 0;
        int count = cJSON_GetArraySize(devices);

        for (int i = 0; i < count && i < MAX_TS_DEVICES; i++) {
            cJSON *dev = cJSON_GetArrayItem(devices, i);
            if (!dev) continue;

            ts_device_t *target = &g_pcs[g_pc_count];
            memset(target, 0, sizeof(ts_device_t));

            /* === 裝置名稱解析優化 === */
            cJSON *name = cJSON_GetObjectItem(dev, "name");
            cJSON *hostname = cJSON_GetObjectItem(dev, "hostname");
            
            char display_name[64] = {0};

            // 1. 優先使用 "name" (Tailscale 網域有名稱)
            if (cJSON_IsString(name) && strlen(name->valuestring) > 0) {
                strncpy(display_name, name->valuestring, sizeof(display_name) - 1);
                // 去除域名後綴
                char *dot = strchr(display_name, '.');
                if (dot) {
                    *dot = '\0';
                }
            } 
            // 2. 若 name 無效或裁切後為空，且 hostname 不是 localhost，才使用 hostname
            if ((strlen(display_name) == 0 || strcmp(display_name, "localhost") == 0) &&
                cJSON_IsString(hostname) && strlen(hostname->valuestring) > 0) {
                strncpy(display_name, hostname->valuestring, sizeof(display_name) - 1);
            }

            // 3. 若預設無名稱
            if (strlen(display_name) == 0) {
                snprintf(display_name, sizeof(display_name), "Device-%d", i + 1);
            }

            strncpy(target->hostname, display_name, sizeof(target->hostname) - 1);

            // User / Email
            cJSON *user = cJSON_GetObjectItem(dev, "user");
            if (cJSON_IsString(user)) {
                strncpy(target->user, user->valuestring, sizeof(target->user) - 1);
            }

            // Last Seen
            cJSON *last_seen = cJSON_GetObjectItem(dev, "lastSeen");
            if (cJSON_IsString(last_seen)) {
                strncpy(target->last_seen, last_seen->valuestring, sizeof(target->last_seen) - 1);
            }

            // Online status
            cJSON *connected = cJSON_GetObjectItem(dev, "connectedToControl");
            target->is_online = cJSON_IsBool(connected) ? cJSON_IsTrue(connected) : false;

            // IP 地址
            cJSON *addresses = cJSON_GetObjectItem(dev, "addresses");
            if (cJSON_IsArray(addresses) && cJSON_GetArraySize(addresses) > 0) {
                cJSON *ip = cJSON_GetArrayItem(addresses, 0);
                if (cJSON_IsString(ip)) {
                    strncpy(target->ip_str, ip->valuestring, sizeof(target->ip_str) - 1);
                }
            }

            // 判斷是否為本 ESP32 裝置
            if (strstr(target->hostname, CONFIG_ML_DEVICE_NAME) != NULL) {
                target->is_self = true;
            }

            g_pc_count++;
        }
        xSemaphoreGive(g_pcs_mutex);
    }

    cJSON_Delete(root);
}

static void fetch_tailscale_devices_task(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(5000));

    char *response_buffer = malloc(16384);
    if (!response_buffer) {
        ESP_LOGE(TAG, "Failed to allocate memory for Tailscale API response");
        vTaskDelete(NULL);
        return;
    }

    const char *url = "https://api.tailscale.com/api/v2/tailnet/-/devices";

    while (1) {
        http_response_buffer_t resp_user_data = {
            .buffer = response_buffer,
            .buffer_len = 0
        };

        esp_http_client_config_t config = {
            .url = url,
            .username = CONFIG_ML_TAILSCALE_API_KEY,
            .password = "",
            .auth_type = HTTP_AUTH_TYPE_BASIC,
            .event_handler = _http_event_handler,
            .user_data = &resp_user_data,
            .timeout_ms = 8000,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        esp_http_client_set_method(client, HTTP_METHOD_GET);

        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            int status_code = esp_http_client_get_status_code(client);
            if (status_code == 200) {
                parse_tailscale_json(response_buffer);
                ESP_LOGI(TAG, "Successfully updated %d Tailscale devices via API", (int)g_pc_count);
            } else {
                ESP_LOGE(TAG, "Tailscale API Error, Status: %d, Response: %s", status_code, response_buffer);
            }
        } else {
            ESP_LOGE(TAG, "HTTP GET Tailscale API request failed: %s", esp_err_to_name(err));
        }

        esp_http_client_cleanup(client);
        // 優化：間隔延長至 30 秒，減少背景流量與 CPU 負荷
        vTaskDelay(pdMS_TO_TICKS(30000));
    }

    free(response_buffer);
}

/* ============================================================================
 * Relay Hardware Logic
 * ========================================================================== */

static void relay_timer_callback(TimerHandle_t xTimer) {
    gpio_set_level(RELAY_GPIO, RELAY_INACTIVE_LEVEL);
    ESP_LOGI(TAG, "Relay RELEASED (Power button released)");
}

static void init_relay_hardware(void) {
    gpio_set_level(RELAY_GPIO, RELAY_INACTIVE_LEVEL);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RELAY_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    relay_timer = xTimerCreate("relay_tmr", pdMS_TO_TICKS(1000), pdFALSE, NULL, relay_timer_callback);
}

static void trigger_relay_async(uint32_t duration_ms, const char *action_desc) {
    if (!relay_timer) return;
    gpio_set_level(RELAY_GPIO, RELAY_ACTIVE_LEVEL);
    ESP_LOGI(TAG, "Relay ACTIVATED for %lu ms (%s)", (unsigned long)duration_ms, action_desc);

    g_relay_log.last_trigger_ms = (uint32_t)(esp_timer_get_time() / 1000000);
    snprintf(g_relay_log.last_action, sizeof(g_relay_log.last_action), "%s (%lums)", action_desc, (unsigned long)duration_ms);
    g_relay_log.total_triggers++;

    xTimerChangePeriod(relay_timer, pdMS_TO_TICKS(duration_ms), 0);
    xTimerStart(relay_timer, 0);
}

/* ============================================================================
 * HTTP Server Handlers
 * ========================================================================== */

static uint32_t parse_ms_param(httpd_req_t *req, uint32_t default_ms) {
    char buf[64];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[32];
        if (httpd_query_key_value(buf, "ms", param, sizeof(param)) == ESP_OK) {
            uint32_t ms = atoi(param);
            if (ms > 0 && ms <= 20000) {
                return ms;
            }
        }
    }
    return default_ms;
}

static esp_err_t power_click_handler(httpd_req_t *req) {
    uint32_t ms = parse_ms_param(req, 1000);
    trigger_relay_async(ms, "Pulse/Click");

    // 優化：直接回傳寫好的固定 JSON，避免寫入過多格式化字串造成延遲
    const char *resp = "{\"status\":\"success\",\"action\":\"pulse\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t power_hold_handler(httpd_req_t *req) {
    uint32_t ms = parse_ms_param(req, 5000);
    trigger_relay_async(ms, "Hold/Force Shutdown");

    const char *resp = "{\"status\":\"success\",\"action\":\"hold\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t pc_status_api_handler(httpd_req_t *req) {
    char *resp = malloc(4096);
    if (!resp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    int offset = snprintf(resp, 4096, "{\"pcs\":[");

    if (xSemaphoreTake(g_pcs_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        for (size_t i = 0; i < g_pc_count; i++) {
            offset += snprintf(resp + offset, 4096 - offset,
                "%s{\"name\":\"%s\",\"email\":\"%s\",\"ip\":\"%s\",\"online\":%s,\"last_seen\":\"%s\",\"is_self\":%s}",
                (i == 0) ? "" : ",",
                g_pcs[i].hostname, 
                g_pcs[i].user, 
                g_pcs[i].ip_str,
                g_pcs[i].is_online ? "true" : "false",
                g_pcs[i].last_seen,
                g_pcs[i].is_self ? "true" : "false");
        }
        xSemaphoreGive(g_pcs_mutex);
    }

    snprintf(resp + offset, 4096 - offset, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    free(resp);
    return ESP_OK;
}

static esp_err_t sysinfo_api_handler(httpd_req_t *req) {
    wifi_ap_record_t ap_info;
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }

    uint32_t uptime_sec = (uint32_t)(esp_timer_get_time() / 1000000);
    uint32_t free_heap = esp_get_free_heap_size();

    char resp[512];
    snprintf(resp, sizeof(resp),
        "{"
        "\"uptime_sec\":%lu,"
        "\"rssi\":%d,"
        "\"free_heap\":%lu,"
        "\"last_trigger_sec\":%lu,"
        "\"last_action\":\"%s\","
        "\"total_triggers\":%lu"
        "}",
        (unsigned long)uptime_sec, rssi, (unsigned long)free_heap,
        (unsigned long)g_relay_log.last_trigger_ms,
        g_relay_log.last_action,
        (unsigned long)g_relay_log.total_triggers);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ============================================================================
 * Web Dashboard HTML
 * ========================================================================== */

static esp_err_t index_html_handler(httpd_req_t *req) {
    static const char html_page[] = R"html(
<!DOCTYPE html><html><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32-S3 Power Controller</title>
<style>
body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; text-align: center; padding: 20px 10px; background: #121212; color: #fff; margin: 0; }
.card { background: #1e1e1e; max-width: 450px; margin: 0 auto 20px auto; padding: 20px; border-radius: 16px; box-shadow: 0 4px 20px rgba(0,0,0,0.5); text-align: left; }
h2 { margin: 0 0 15px 0; font-size: 20px; text-align: center; color: #3b82f6; }
h3 { margin: 0 0 10px 0; font-size: 15px; color: #aaa; border-bottom: 1px solid #333; padding-bottom: 5px; }
.btn { width: 100%; border: none; color: white; padding: 14px; font-size: 16px; font-weight: 600; border-radius: 10px; cursor: pointer; margin-bottom: 12px; box-sizing: border-box; transition: background 0.2s; }
.btn-click { background-color: #2563eb; }
.btn-click:active { background-color: #1d4ed8; transform: scale(0.98); }
.btn-hold { background-color: #dc2626; }
.btn-hold:active { background-color: #b91c1c; transform: scale(0.98); }
#msg { color: #10b981; font-size: 14px; min-height: 20px; text-align: center; font-weight: 500; margin-bottom: 10px; }
.pc-list, .sys-info { list-style: none; padding: 0; margin: 0; font-size: 13px; }
.pc-item { display: flex; justify-content: space-between; align-items: center; padding: 10px 0; border-bottom: 1px solid #2a2a2a; }
.status-dot { display: inline-block; width: 10px; height: 10px; border-radius: 50%; margin-right: 6px; }
.online { background-color: #10b981; box-shadow: 0 0 8px #10b981; }
.offline { background-color: #ef4444; }
.self-dot { background-color: #3b82f6; box-shadow: 0 0 8px #3b82f6; }
.info-row { display: flex; justify-content: space-between; padding: 6px 0; color: #ccc; }
.info-val { font-weight: 600; color: #fff; }
.duration-control { margin-bottom: 15px; background: #2a2a2a; padding: 12px; border-radius: 10px; display: flex; flex-direction: column; gap: 8px; }
.duration-label { font-size: 13px; color: #aaa; font-weight: 500; }
.duration-inputs { display: flex; gap: 8px; align-items: center; }
.duration-inputs select, .duration-inputs input { background: #181818; border: 1px solid #444; border-radius: 8px; color: #fff; padding: 8px; font-size: 14px; outline: none; }
.duration-inputs select { flex: 1; }
.duration-inputs input { width: 90px; text-align: center; }
.email-tag { color: #888; font-size: 11px; margin-top: 2px; display: block; }
</style></head><body>

<div class="card">
<h2>ESP32-S3 開機控制面板</h2>
<div class="duration-control">
  <div class="duration-label">⏱️ 設定觸發按下時間：</div>
  <div class="duration-inputs">
    <select id="presetSec" onchange="onPresetChange()">
      <option value="1" selected>1 秒 (一般開機/短按)</option>
      <option value="0.5">0.5 秒 (極短脈衝)</option>
      <option value="2">2 秒 (長短按)</option>
      <option value="3">3 秒 (強迫重啟)</option>
      <option value="5">5 秒 (強制關機)</option>
      <option value="custom">自訂秒數...</option>
    </select>
    <input type="number" id="customSecInput" value="1" min="0.1" max="20" step="0.1" placeholder="秒數">
    <span style="font-size:13px;color:#aaa;">秒</span>
  </div>
</div>

<button class="btn btn-click" onclick="sendSelectedDuration()">⚡ 開機 / 按鈕觸發</button>
<button class="btn btn-hold" onclick="confirmHold()">🚨 強制關機 (固定 5 秒)</button>
<div id="msg"></div>
</div>

<div class="card">
<h3>🖥️ Tailscale API 網域裝置監控</h3>
<div id="pcList" class="pc-list">載入中...</div>
</div>

<div class="card">
<h3>📊 系統資訊 & 運作日誌</h3>
<div class="sys-info">
<div class="info-row"><span>ESP32 運作時間 (Uptime)</span><span id="uptime" class="info-val">--</span></div>
<div class="info-row"><span>Wi-Fi 訊號 (RSSI)</span><span id="rssi" class="info-val">--</span></div>
<div class="info-row"><span>記憶體殘量 (Free Heap)</span><span id="heap" class="info-val">--</span></div>
<div class="info-row"><span>上次繼電器動作</span><span id="lastAction" class="info-val">--</span></div>
<div class="info-row"><span>累計觸發次數</span><span id="totalTriggers" class="info-val">--</span></div>
</div>
</div>

<script>
function onPresetChange(){
  var sel = document.getElementById('presetSec').value;
  var input = document.getElementById('customSecInput');
  if(sel !== 'custom'){ input.value = sel; }
}
function getSelectedMs(){
  var sec = parseFloat(document.getElementById('customSecInput').value);
  if(isNaN(sec) || sec <= 0) sec = 1;
  return Math.round(sec * 1000);
}
function sendSelectedDuration(){
  var ms = getSelectedMs();
  sendCmd('/power/api?ms=' + ms);
}
function sendCmd(path){
  var msg = document.getElementById('msg');
  msg.style.color = '#f59e0b';
  msg.innerText = '觸發中...';
  fetch(path, { cache: 'no-store' })
  .then(r => r.json())
  .then(d => {
    msg.style.color = '#10b981';
    msg.innerText = '觸發成功';
    setTimeout(updateSysInfo, 500);
  })
  .catch(e => {
    msg.style.color = '#ef4444';
    msg.innerText = '觸發失敗: ' + e.message;
  });
}
function confirmHold(){
  if(confirm('警告：這將會強制關閉電腦（長按 5 秒），可能造成未儲存資料遺失。確定要執行嗎？')){
    sendCmd('/power/hold/api?ms=5000');
  }
}
function formatUptime(sec){
  var d = Math.floor(sec / 86400);
  var h = Math.floor((sec % 86400) / 3600);
  var m = Math.floor((sec % 3600) / 60);
  var s = sec % 60;
  return (d>0?d+'天 ':'') + (h<10?'0':'')+h + ':' + (m<10?'0':'')+m + ':' + (s<10?'0':'')+s;
}
function updatePcStatus(){
  fetch('/api/pc_status', { cache: 'no-store' })
  .then(r => r.json())
  .then(d => {
    var html = '';
    d.pcs.forEach(pc => {
      var dotClass = pc.is_self ? 'self-dot' : (pc.online ? 'online' : 'offline');
      var statusText = pc.is_self ? '本機 (Controller)' : (pc.online ? 'Tailscale 在線' : '離線');
      var statusColor = pc.is_self ? '#3b82f6' : (pc.online ? '#10b981' : '#ef4444');
      
      html += '<div class="pc-item">' +
              '<div>' +
                '<span><span class="status-dot ' + dotClass + '"></span><strong>' + pc.name + '</strong></span>' +
                '<span class="email-tag">📧 ' + pc.email + ' | 🌐 ' + pc.ip + '</span>' +
              '</div>' +
              '<span style="color:' + statusColor + ';font-weight:600;white-space:nowrap;">' + statusText + '</span>' +
              '</div>';
    });
    document.getElementById('pcList').innerHTML = html;
  }).catch(()=>{});
}
function updateSysInfo(){
  fetch('/api/sysinfo', { cache: 'no-store' })
  .then(r => r.json())
  .then(d => {
    document.getElementById('uptime').innerText = formatUptime(d.uptime_sec);
    document.getElementById('rssi').innerText = d.rssi + ' dBm';
    document.getElementById('heap').innerText = (d.free_heap / 1024).toFixed(1) + ' KB';
    document.getElementById('lastAction').innerText = d.last_action;
    document.getElementById('totalTriggers').innerText = d.total_triggers + ' 次';
  }).catch(()=>{});
}

// 優化：將輪詢整合並錯開請求，降低 ESP32 負擔 (改為 8 秒一輪)
function pollAllData() {
  updateSysInfo();
  setTimeout(updatePcStatus, 500); // 錯開 500ms
}

updatePcStatus(); 
updateSysInfo();
setInterval(pollAllData, 8000);
</script></body></html>
)html";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html_page, sizeof(html_page) - 1);
    return ESP_OK;
}

/* ============================================================================
 * HTTP Server Handlers
 * ========================================================================== */

/* 如果 microlink 範例中有提供其專屬的 index/status handler (例如 microlink_web_handler)，
 * 請在此處引入或宣告 */
// extern esp_err_t microlink_http_index_handler(httpd_req_t *req);

static void start_custom_web_server(void) {
    /* MicroLink's config httpd (ml_config_httpd_start, called from microlink_start())
     * already owns port 80 and serves "/" plus the config API endpoints. Register
     * our extra URIs on that same server instead of starting a second one - two
     * httpd_start() calls on the same port fail the second time. */
    server_handle = ml_config_httpd_get_handle();
    if (!server_handle) {
        ESP_LOGE(TAG, "Failed to attach custom HTTP handlers: MicroLink config httpd not running");
        return;
    }

    httpd_uri_t uri_power      = { .uri = "/power",        .method = HTTP_GET, .handler = index_html_handler };
    httpd_uri_t uri_click      = { .uri = "/power/api",    .method = HTTP_GET, .handler = power_click_handler };
    httpd_uri_t uri_hold       = { .uri = "/power/hold/api",.method = HTTP_GET, .handler = power_hold_handler };
    httpd_uri_t uri_pc_status  = { .uri = "/api/pc_status", .method = HTTP_GET, .handler = pc_status_api_handler };
    httpd_uri_t uri_sysinfo    = { .uri = "/api/sysinfo",   .method = HTTP_GET, .handler = sysinfo_api_handler };

    httpd_register_uri_handler(server_handle, &uri_power);
    httpd_register_uri_handler(server_handle, &uri_click);
    httpd_register_uri_handler(server_handle, &uri_hold);
    httpd_register_uri_handler(server_handle, &uri_pc_status);
    httpd_register_uri_handler(server_handle, &uri_sysinfo);

    ESP_LOGI(TAG, "Custom HTTP handlers attached to MicroLink config server!");
    ESP_LOGI(TAG, "   - MicroLink Dashboard: http://<IP>/");
    ESP_LOGI(TAG, "   - PC Power Control   : http://<IP>/power");
}

static void turn_off_board_rgb(void) {
    gpio_reset_pin(BOARD_RGB_GPIO);
    gpio_set_direction(BOARD_RGB_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_RGB_GPIO, 0);
}

/* ============================================================================
 * WiFi Init
 * ========================================================================== */

static void wifi_creds_init(void) {
    g_wifi_count = 0;

    /* 1. Firmware-baked defaults — always tried first, highest priority. */
    for (int i = 0; i < WIFI_DEFAULT_COUNT; i++) {
        wifi_cred_t *slot = &g_wifi_creds[g_wifi_count];
        strncpy(slot->ssid, k_default_wifi_creds[i].ssid, sizeof(slot->ssid) - 1);
        strncpy(slot->password, k_default_wifi_creds[i].password, sizeof(slot->password) - 1);
        g_wifi_count++;
    }

    /* 2. Web UI / NVS list — lower priority, user-editable, appended after. */
    ml_config_wifi_list_t nvs_list;
    if (ml_config_get_wifi_list(&nvs_list)) {
        for (int i = 0; i < nvs_list.count && g_wifi_count < WIFI_MAX_NETWORKS; i++) {
            if (nvs_list.entries[i].ssid[0] == '\0') {
                continue;
            }
            wifi_cred_t *slot = &g_wifi_creds[g_wifi_count];
            strncpy(slot->ssid, nvs_list.entries[i].ssid, sizeof(slot->ssid) - 1);
            strncpy(slot->password, nvs_list.entries[i].pass, sizeof(slot->password) - 1);
            g_wifi_count++;
        }
    }

    ESP_LOGI(TAG, "WiFi: %d network(s) configured (%d built-in default + %d from web UI)",
             (int)g_wifi_count, WIFI_DEFAULT_COUNT, (int)g_wifi_count - WIFI_DEFAULT_COUNT);
}

static void wifi_set_config(size_t idx) {
    if (g_wifi_count == 0) {
        return;
    }
    idx %= g_wifi_count;

    wifi_config_t wifi_config = {
        .sta = { .threshold.authmode = WIFI_AUTH_WPA2_PSK },
    };
    strncpy((char *)wifi_config.sta.ssid, g_wifi_creds[idx].ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, g_wifi_creds[idx].password, sizeof(wifi_config.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_LOGI(TAG, "WiFi: trying \"%s\" (%d/%d)", g_wifi_creds[idx].ssid, (int)idx + 1, (int)g_wifi_count);
}

static void wifi_try_next(void) {
    if (g_wifi_count == 0) {
        ESP_LOGE(TAG, "WiFi: no networks configured, cannot connect");
        return;
    }
    wifi_set_config(g_wifi_next_idx);
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        wifi_try_next();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason=%d), cycling to next network in list...", disc ? disc->reason : -1);
        if (g_wifi_count > 0) {
            g_wifi_next_idx = (g_wifi_next_idx + 1) % g_wifi_count;
        }
        wifi_try_next();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);

        if (g_ml_started && ml) {
            ESP_LOGI(TAG, "Wi-Fi (re)connected — rebinding MicroLink/Tailscale");
            esp_err_t rb_err = microlink_rebind(ml);
            if (rb_err != ESP_OK) {
                ESP_LOGE(TAG, "microlink_rebind failed: %s", esp_err_to_name(rb_err));
            }
        }
    }
}

static void wifi_init(void) {
    wifi_creds_init();

    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_set_config(g_wifi_next_idx);
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

/* ============================================================================
 * Main
 * ========================================================================== */

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP32-S3 Remote Power Controller Starting...");

    g_pcs_mutex = xSemaphoreCreateMutex();

    init_relay_hardware();
    turn_off_board_rgb();
    wifi_init();

    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    microlink_config_t config = {
        .auth_key = CONFIG_ML_TAILSCALE_AUTH_KEY,
        .device_name = CONFIG_ML_DEVICE_NAME,
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = true,
        .max_peers = CONFIG_ML_MAX_PEERS,
        .wifi_tx_power_dbm = 20,
    };

    ml = microlink_init(&config);
    if (!ml) {
        ESP_LOGE(TAG, "Failed to initialize MicroLink");
        return;
    }

    {
        char ssids[WIFI_DEFAULT_COUNT][33];
        char passwords[WIFI_DEFAULT_COUNT][65];
        for (int i = 0; i < WIFI_DEFAULT_COUNT; i++) {
            strncpy(ssids[i], k_default_wifi_creds[i].ssid, sizeof(ssids[i]) - 1);
            ssids[i][sizeof(ssids[i]) - 1] = '\0';
            strncpy(passwords[i], k_default_wifi_creds[i].password, sizeof(passwords[i]) - 1);
            passwords[i][sizeof(passwords[i]) - 1] = '\0';
        }
        microlink_set_default_wifi_list(ml, ssids, passwords, WIFI_DEFAULT_COUNT);
    }

    ESP_ERROR_CHECK(microlink_start(ml));
    g_ml_started = true;

    start_custom_web_server();

    // 優化：將 API Task 優先級降為 2 (避免搶占 HTTPd 5 與其他即時 Task)
    xTaskCreate(fetch_tailscale_devices_task, "ts_api_task", 8192, NULL, 2, NULL);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}