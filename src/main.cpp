#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include "esp_http_server.h"
#include <ArduinoJson.h>
#include "soc/rtc_cntl_reg.h"
#include <Preferences.h>

// ======================== CAMERA PINS ========================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ======================== HARDWARE PINS ========================
#define BUZZER_PIN 13
#define LED_PIN 4
#define RESET_BUTTON_PIN 12  // Optional: Hold for 5s to reset WiFi

// ======================== WiFi Configuration ========================
#define AP_SSID "RoadSafe-AI-Setup"
#define AP_PASSWORD "12345678"
#define WIFI_TIMEOUT 30000  // 30 seconds

// ======================== UDP DISCOVERY CONFIGURATION ========================
#define DISCOVERY_PORT 9999
#define DEVICE_NAME "RoadSafe-AI-ESP32CAM"

WiFiUDP udp;
bool discoveryEnabled = false;

Preferences preferences;
String saved_ssid = "";
String saved_password = "";
bool wifi_configured = false;

httpd_handle_t camera_httpd = NULL;
httpd_handle_t config_httpd = NULL;

// ======================== STATUS VARIABLES ========================
bool alarm_active = false;
unsigned long alarm_start_time = 0;
bool buzzer_state = false;
unsigned long last_buzzer_toggle = 0;

int total_drowsiness_alerts = 0;
String current_detection_status = "Normal";
unsigned long last_detection_time = 0;

// ====================== UDP DISCOVERY FUNCTIONS ======================

void setupUDPDiscovery() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("⚠️ Cannot start UDP discovery - not connected to WiFi");
        return;
    }

    if (udp.begin(DISCOVERY_PORT)) {
        discoveryEnabled = true;
        Serial.println("\n========================================");
        Serial.println("📡 UDP DISCOVERY ENABLED");
        Serial.println("========================================");
        Serial.printf("  Listening on port: %d\n", DISCOVERY_PORT);
        Serial.printf("  Device name: %s\n", DEVICE_NAME);
        Serial.printf("  IP address: %s\n", WiFi.localIP().toString().c_str());
        Serial.println("========================================\n");
    } else {
        Serial.println("❌ UDP Discovery setup failed!");
        discoveryEnabled = false;
    }
}

void handleUDPDiscovery() {
    if (!discoveryEnabled) return;

    int packetSize = udp.parsePacket();
    if (packetSize) {
        // Read incoming packet
        char incomingPacket[255];
        int len = udp.read(incomingPacket, 255);
        if (len > 0) {
            incomingPacket[len] = '\0';
        }

        String message = String(incomingPacket);
        
        // Check if it's a discovery request
        if (message == "ROADSAFE_DISCOVER") {
            Serial.println("\n📡 Discovery request received!");
            Serial.printf("   From: %s:%d\n", udp.remoteIP().toString().c_str(), udp.remotePort());

            // Prepare response with IP address
            String response = "ROADSAFE_RESPONSE:";
            response += WiFi.localIP().toString();
            response += ":";
            response += DEVICE_NAME;

            // Send response back to requester
            udp.beginPacket(udp.remoteIP(), udp.remotePort());
            udp.write((uint8_t*)response.c_str(), response.length());
            udp.endPacket();

            Serial.printf("   Sent: %s\n", response.c_str());
        }
    }
}

// ====================== HELPER FUNCTIONS ======================
esp_err_t set_cors_headers(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    return ESP_OK;
}

// ====================== WiFi CREDENTIALS STORAGE ======================
void saveWiFiCredentials(String ssid, String password) {
    preferences.begin("wifi", false);
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.putBool("configured", true);
    preferences.end();
    Serial.println("✓ WiFi credentials saved!");
}

bool loadWiFiCredentials() {
    preferences.begin("wifi", true);
    saved_ssid = preferences.getString("ssid", "");
    saved_password = preferences.getString("password", "");
    wifi_configured = preferences.getBool("configured", false);
    preferences.end();
    
    return wifi_configured && saved_ssid.length() > 0;
}

void clearWiFiCredentials() {
    preferences.begin("wifi", false);
    preferences.clear();
    preferences.end();
    Serial.println("✓ WiFi credentials cleared!");
}

// ====================== WiFi SETUP PAGE HTML ======================
const char setup_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>RoadSafe AI - WiFi Setup</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
            padding: 20px;
        }
        .container {
            background: white;
            border-radius: 20px;
            box-shadow: 0 20px 60px rgba(0,0,0,0.3);
            max-width: 500px;
            width: 100%;
            overflow: hidden;
        }
        .header {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            padding: 30px;
            text-align: center;
        }
        .header h1 {
            font-size: 28px;
            margin-bottom: 10px;
        }
        .header p {
            opacity: 0.9;
            font-size: 14px;
        }
        .icon {
            width: 60px;
            height: 60px;
            margin: 0 auto 15px;
            background: rgba(255,255,255,0.2);
            border-radius: 15px;
            display: flex;
            align-items: center;
            justify-content: center;
            font-size: 30px;
        }
        .content {
            padding: 30px;
        }
        .form-group {
            margin-bottom: 20px;
        }
        label {
            display: block;
            margin-bottom: 8px;
            color: #333;
            font-weight: 500;
            font-size: 14px;
        }
        input, select {
            width: 100%;
            padding: 12px 15px;
            border: 2px solid #e0e0e0;
            border-radius: 10px;
            font-size: 16px;
            transition: all 0.3s;
        }
        input:focus, select:focus {
            outline: none;
            border-color: #667eea;
            box-shadow: 0 0 0 3px rgba(102, 126, 234, 0.1);
        }
        .btn {
            width: 100%;
            padding: 15px;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            border: none;
            border-radius: 10px;
            font-size: 16px;
            font-weight: 600;
            cursor: pointer;
            transition: transform 0.2s;
        }
        .btn:hover {
            transform: translateY(-2px);
            box-shadow: 0 5px 15px rgba(102, 126, 234, 0.4);
        }
        .btn:active {
            transform: translateY(0);
        }
        .scanning {
            text-align: center;
            padding: 20px;
            color: #666;
        }
        .spinner {
            border: 3px solid #f3f3f3;
            border-top: 3px solid #667eea;
            border-radius: 50%;
            width: 40px;
            height: 40px;
            animation: spin 1s linear infinite;
            margin: 0 auto 15px;
        }
        @keyframes spin {
            0% { transform: rotate(0deg); }
            100% { transform: rotate(360deg); }
        }
        .status {
            margin-top: 20px;
            padding: 15px;
            border-radius: 10px;
            text-align: center;
            display: none;
        }
        .status.success {
            background: #d4edda;
            color: #155724;
            border: 1px solid #c3e6cb;
        }
        .status.error {
            background: #f8d7da;
            color: #721c24;
            border: 1px solid #f5c6cb;
        }
        .info-box {
            background: #e7f3ff;
            border-left: 4px solid #2196F3;
            padding: 15px;
            border-radius: 8px;
            margin-bottom: 20px;
        }
        .info-box p {
            color: #0c5fa8;
            font-size: 13px;
            line-height: 1.5;
        }
        .password-toggle {
            position: relative;
        }
        .toggle-btn {
            position: absolute;
            right: 12px;
            top: 50%;
            transform: translateY(-50%);
            background: none;
            border: none;
            cursor: pointer;
            font-size: 18px;
            padding: 5px;
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <div class="icon">🚗</div>
            <h1>RoadSafe AI</h1>
            <p>Driver Drowsiness Detection System</p>
        </div>
        <div class="content">
            <div class="info-box">
                <p><strong>📡 Setup Required</strong><br>
                Connect your ESP32-CAM to your WiFi network to enable drowsiness detection monitoring.</p>
            </div>

            <div id="scanningDiv" class="scanning">
                <div class="spinner"></div>
                <p>Scanning for WiFi networks...</p>
            </div>

            <form id="wifiForm" style="display: none;">
                <div class="form-group">
                    <label for="ssid">WiFi Network</label>
                    <select id="ssid" name="ssid" required>
                        <option value="">Select a network...</option>
                    </select>
                </div>

                <div class="form-group">
                    <label for="password">Password</label>
                    <div class="password-toggle">
                        <input type="password" id="password" name="password" 
                               placeholder="Enter WiFi password" required>
                        <button type="button" class="toggle-btn" onclick="togglePassword()">👁️</button>
                    </div>
                </div>

                <button type="submit" class="btn">Connect to WiFi</button>
            </form>

            <div id="status" class="status"></div>
        </div>
    </div>

    <script>
        // Scan for WiFi networks
        fetch('/scan')
            .then(response => response.json())
            .then(data => {
                document.getElementById('scanningDiv').style.display = 'none';
                document.getElementById('wifiForm').style.display = 'block';
                
                const select = document.getElementById('ssid');
                data.networks.forEach(network => {
                    const option = document.createElement('option');
                    option.value = network.ssid;
                    option.textContent = `${network.ssid} (${network.rssi} dBm) ${network.encryption}`;
                    select.appendChild(option);
                });
            })
            .catch(error => {
                document.getElementById('scanningDiv').innerHTML = 
                    '<p style="color: #f44336;">Failed to scan networks. Please refresh the page.</p>';
            });

        // Handle form submission
        document.getElementById('wifiForm').addEventListener('submit', function(e) {
            e.preventDefault();
            
            const ssid = document.getElementById('ssid').value;
            const password = document.getElementById('password').value;
            const statusDiv = document.getElementById('status');
            
            statusDiv.style.display = 'block';
            statusDiv.className = 'status';
            statusDiv.textContent = '⏳ Connecting to WiFi...';
            
            fetch('/connect', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ ssid: ssid, password: password })
            })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    statusDiv.className = 'status success';
                    statusDiv.innerHTML = `✓ Connected successfully!<br>IP: ${data.ip}<br>Redirecting...`;
                    setTimeout(() => {
                        window.location.href = `http://${data.ip}`;
                    }, 3000);
                } else {
                    statusDiv.className = 'status error';
                    statusDiv.textContent = '✗ Connection failed: ' + data.message;
                }
            })
            .catch(error => {
                statusDiv.className = 'status error';
                statusDiv.textContent = '✗ Error: ' + error.message;
            });
        });

        function togglePassword() {
            const input = document.getElementById('password');
            input.type = input.type === 'password' ? 'text' : 'password';
        }
    </script>
</body>
</html>
)rawliteral";

// ====================== MAIN CAMERA PAGE HTML ======================
const char camera_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>RoadSafe AI - Live Feed</title>
    <style>
        body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; margin: 0; background: #0f172a; color: #e2e8f0; display: flex; flex-direction: column; align-items: center; min-height: 100vh; padding: 30px 15px; }
        h1 { margin-bottom: 5px; }
        p { margin-top: 0; color: #94a3b8; }
        .frame { max-width: 100%; width: 420px; border-radius: 14px; box-shadow: 0 18px 40px rgba(15,23,42,0.6); overflow: hidden; background: #1e293b; border: 1px solid #334155; }
        img { width: 100%; display: block; }
        .stats { margin-top: 24px; display: grid; gap: 12px; width: 420px; max-width: 100%; }
        .card { padding: 16px 18px; border-radius: 12px; background: #1e293b; border: 1px solid #334155; display: flex; justify-content: space-between; align-items: center; }
        .label { color: #94a3b8; font-size: 14px; }
        .value { font-size: 18px; font-weight: 600; }
        @media (max-width: 480px) {
            body { padding: 20px 12px; }
        }
        .status-indicator { width: 12px; height: 12px; border-radius: 50%; margin-right: 8px; }
        .status-row { display: flex; align-items: center; }
    </style>
</head>
<body>
    <h1>RoadSafe AI</h1>
    <p>Live stream from ESP32-CAM</p>
    <div class="frame">
        <img id="stream" src="/stream" alt="Live stream" loading="lazy"/>
    </div>
    <div class="stats">
        <div class="card">
            <span class="label">Status</span>
            <span class="status-row"><span id="statusLed" class="status-indicator" style="background:#f87171"></span><span class="value" id="statusText">Loading…</span></span>
        </div>
        <div class="card">
            <span class="label">WiFi</span>
            <span class="value" id="wifiSsid">-</span>
        </div>
        <div class="card">
            <span class="label">IP</span>
            <span class="value" id="ipAddr">-</span>
        </div>
        <div class="card">
            <span class="label">RSSI</span>
            <span class="value" id="rssi">-</span>
        </div>
        <div class="card">
            <span class="label">Alerts</span>
            <span class="value" id="alerts">0</span>
        </div>
    </div>
    <script>
        async function refreshStatus() {
            try {
                const res = await fetch('/status');
                if (!res.ok) throw new Error('HTTP ' + res.status);
                const data = await res.json();
                document.getElementById('statusText').textContent = data.status || 'online';
                document.getElementById('statusLed').style.background = data.alarm_active ? '#fb923c' : '#34d399';
                document.getElementById('wifiSsid').textContent = data.wifi_ssid || '-';
                document.getElementById('ipAddr').textContent = data.ip || '-';
                document.getElementById('alerts').textContent = data.alerts ?? '-';
                document.getElementById('rssi').textContent = data.rssi ? data.rssi + ' dBm' : '-';
            } catch (err) {
                document.getElementById('statusText').textContent = 'offline';
                document.getElementById('statusLed').style.background = '#f87171';
            }
        }
        refreshStatus();
        setInterval(refreshStatus, 4000);
    </script>
</body>
</html>
)rawliteral";

// ====================== SCAN NETWORKS HANDLER ======================
static esp_err_t scan_handler(httpd_req_t *req) {
    set_cors_headers(req);
    
    int n = WiFi.scanNetworks();
    
    String json = "{\"networks\":[";
    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        json += "{";
        json += "\"ssid\":\"" + WiFi.SSID(i) + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
        json += "\"encryption\":\"" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "Open" : "Secured") + "\"";
        json += "}";
    }
    json += "]}";
    
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json.c_str(), json.length());
}

// ====================== CONNECT HANDLER ======================
static esp_err_t connect_handler(httpd_req_t *req) {
    char content[200];
    int ret = httpd_req_recv(req, content, sizeof(content));
    if (ret <= 0) return ESP_FAIL;
    content[ret] = '\0';

    DynamicJsonDocument doc(512);
    deserializeJson(doc, content);

    String ssid = doc["ssid"].as<String>();
    String password = doc["password"].as<String>();

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    // Try to connect
    WiFi.begin(ssid.c_str(), password.c_str());
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        attempts++;
    }

    String response;
    if (WiFi.status() == WL_CONNECTED) {
        saveWiFiCredentials(ssid, password);
        
        // Start UDP discovery after WiFi connection
        setupUDPDiscovery();
        
        response = "{\"success\":true,\"ip\":\"" + WiFi.localIP().toString() + "\"}";
    } else {
        response = "{\"success\":false,\"message\":\"Failed to connect\"}";
    }

    return httpd_resp_send(req, response.c_str(), response.length());
}

// ====================== SETUP PAGE HANDLER ======================
static esp_err_t setup_handler(httpd_req_t *req) {
    set_cors_headers(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, setup_html, strlen(setup_html));
}

// ====================== INDEX PAGE HANDLER ======================
static esp_err_t index_handler(httpd_req_t *req) {
    set_cors_headers(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, camera_html, strlen(camera_html));
}

// ======================== STREAM HANDLER ========================
static esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    char part_buf[64];
    
    static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=123456789";
    static const char* _STREAM_BOUNDARY = "\r\n--123456789\r\n";
    static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";
    
    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;
    set_cors_headers(req);

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) { res = ESP_FAIL; break; }
        
        res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        if (res == ESP_OK) {
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, fb->len);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }

        esp_camera_fb_return(fb);
        if (res != ESP_OK) break;
    }
    return res;
}

// ======================== CAPTURE HANDLER ========================
static esp_err_t capture_handler(httpd_req_t *req) {
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) return ESP_FAIL;
    
    set_cors_headers(req);
    httpd_resp_set_type(req, "image/jpeg");
    
    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return res;
}

// ======================== ALARM HANDLER ========================
static esp_err_t alarm_handler(httpd_req_t *req) {
    char content[200];
    int ret = httpd_req_recv(req, content, sizeof(content));
    if (ret <= 0) return ESP_FAIL;
    content[ret] = '\0';

    DynamicJsonDocument doc(512);
    deserializeJson(doc, content);

    String command = doc["command"];

    if (command == "ALARM_ON") {
        alarm_active = true;
        total_drowsiness_alerts++;
    } else if (command == "ALARM_OFF") {
        alarm_active = false;
        digitalWrite(BUZZER_PIN, LOW);
        digitalWrite(LED_PIN, LOW);
    }

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", strlen("{\"status\":\"ok\"}"));
}

// ======================== STATUS HANDLER ========================
static esp_err_t status_handler(httpd_req_t *req) {
    set_cors_headers(req);

    String json = "{";
    json += "\"status\":\"online\",";
    json += "\"alarm_active\":" + String(alarm_active ? "true" : "false") + ",";
    json += "\"alerts\":" + String(total_drowsiness_alerts) + ",";
    json += "\"wifi_ssid\":\"" + WiFi.SSID() + "\",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"udp_discovery\":" + String(discoveryEnabled ? "true" : "false") + ",";
    json += "\"free_heap\":" + String(ESP.getFreeHeap());
    json += "}";

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json.c_str(), json.length());
}

// ====================== RESET WIFI HANDLER ======================
static esp_err_t reset_handler(httpd_req_t *req) {
    set_cors_headers(req);
    clearWiFiCredentials();
    
    String response = "{\"success\":true,\"message\":\"WiFi reset. Device will restart...\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response.c_str(), response.length());
    
    delay(1000);
    ESP.restart();
    return ESP_OK;
}

// ======================== START CAMERA SERVER ========================
void startCameraServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;

    httpd_uri_t stream_uri = {"/stream", HTTP_GET, stream_handler, NULL};
    httpd_uri_t capture_uri = {"/capture", HTTP_GET, capture_handler, NULL};
    httpd_uri_t alarm_uri = {"/alarm", HTTP_POST, alarm_handler, NULL};
    httpd_uri_t status_uri = {"/status", HTTP_GET, status_handler, NULL};
    httpd_uri_t index_uri = {"/", HTTP_GET, index_handler, NULL};

    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(camera_httpd, &index_uri);
        httpd_register_uri_handler(camera_httpd, &stream_uri);
        httpd_register_uri_handler(camera_httpd, &capture_uri);
        httpd_register_uri_handler(camera_httpd, &alarm_uri);
        httpd_register_uri_handler(camera_httpd, &status_uri);
    }
}

// ======================== START CONFIG SERVER ========================
void startConfigServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    httpd_uri_t setup_uri = {"/", HTTP_GET, setup_handler, NULL};
    httpd_uri_t scan_uri = {"/scan", HTTP_GET, scan_handler, NULL};
    httpd_uri_t connect_uri = {"/connect", HTTP_POST, connect_handler, NULL};
    httpd_uri_t reset_uri = {"/reset", HTTP_POST, reset_handler, NULL};

    if (httpd_start(&config_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(config_httpd, &setup_uri);
        httpd_register_uri_handler(config_httpd, &scan_uri);
        httpd_register_uri_handler(config_httpd, &connect_uri);
        httpd_register_uri_handler(config_httpd, &reset_uri);
    }
}

// ======================== INIT CAMERA ========================
void initCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;

    // ============================================
    // OPTIMIZED FOR 25-30 FPS!
    // ============================================
    config.xclk_freq_hz = 20000000;        // 20MHz for better performance
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_QVGA;    // 320x240 - FAST!
    config.jpeg_quality = 12;              // Good balance
    config.fb_count = 2;                   // Double buffering
    // ============================================

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed: 0x%x\n", err);
        return;
    }

    // Sensor optimizations
    sensor_t * s = esp_camera_sensor_get();
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_gain_ctrl(s, 1);
    s->set_lenc(s, 1);
    s->set_dcw(s, 1);

    Serial.println("✓ Camera initialized (QVGA 25-30 FPS mode)");
}

// ======================== SETUP ========================
void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    Serial.begin(115200);
    delay(1000);

    pinMode(BUZZER_PIN, OUTPUT);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(LED_PIN, LOW);

#if defined(RESET_BUTTON_PIN) && RESET_BUTTON_PIN >= 0
    pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
#endif

    Serial.println("\n\n================================");
    Serial.println("  RoadSafe AI - ESP32-CAM");
    Serial.println("  Drowsiness Detection System");
    Serial.println("  WITH UDP DISCOVERY");
    Serial.println("================================\n");

    // Initialize camera first
    initCamera();

    // Try to load saved WiFi credentials
    if (loadWiFiCredentials()) {
        Serial.println("✓ Found saved WiFi credentials");
        Serial.print("  SSID: ");
        Serial.println(saved_ssid);
        Serial.println("  Connecting...");

        WiFi.mode(WIFI_STA);
        WiFi.begin(saved_ssid.c_str(), saved_password.c_str());

        unsigned long startAttempt = millis();
        while (WiFi.status() != WL_CONNECTED && 
               millis() - startAttempt < WIFI_TIMEOUT) {
            delay(500);
            Serial.print(".");
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("✓ Connected to WiFi!");
            Serial.print("  IP Address: ");
            Serial.println(WiFi.localIP());
            Serial.print("  Signal: ");
            Serial.print(WiFi.RSSI());
            Serial.println(" dBm");

            // Start UDP Discovery
            setupUDPDiscovery();

            startCameraServer();

            Serial.println("\n========================================");
            Serial.println("  CAMERA STREAM READY!");
            Serial.print("  http://");
            Serial.println(WiFi.localIP());
            Serial.println("========================================\n");
        } else {
            Serial.println("✗ Failed to connect to saved WiFi");
            Serial.println("  Starting setup mode...");
            clearWiFiCredentials();
            ESP.restart();
        }
    } else {
        // No saved credentials - start AP mode
        Serial.println("ℹ No WiFi configured - Starting setup mode");
        
        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID, AP_PASSWORD);
        
        IPAddress IP = WiFi.softAPIP();
        Serial.println("\n========================================");
        Serial.println("  SETUP MODE ACTIVE");
        Serial.println("========================================");
        Serial.print("  Network: ");
        Serial.println(AP_SSID);
        Serial.print("  Password: ");
        Serial.println(AP_PASSWORD);
        Serial.print("  Setup URL: http://");
        Serial.println(IP);
        Serial.println("========================================\n");

        startConfigServer();
    }
}

// ======================== LOOP ========================
void loop() {
    // Handle UDP Discovery packets
    handleUDPDiscovery();

    // Alarm handling
    if (alarm_active) {
        unsigned long t = millis();
        if (t - last_buzzer_toggle > 400) {
            buzzer_state = !buzzer_state;
            digitalWrite(BUZZER_PIN, buzzer_state);
            digitalWrite(LED_PIN, buzzer_state);
            last_buzzer_toggle = t;
        }
    }

    // Optional: Check for reset button (hold for 5 seconds)
    #if defined(RESET_BUTTON_PIN) && RESET_BUTTON_PIN >= 0
    static unsigned long buttonPressTime = 0;
    static bool buttonPressed = false;
    
    if (digitalRead(RESET_BUTTON_PIN) == LOW) {
        if (!buttonPressed) {
            buttonPressed = true;
            buttonPressTime = millis();
        } else if (millis() - buttonPressTime > 5000) {
            Serial.println("Reset button held - Clearing WiFi...");
            clearWiFiCredentials();
            ESP.restart();
        }
    } else {
        buttonPressed = false;
    }
    #endif

    delay(10);
}