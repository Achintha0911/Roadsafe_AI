#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include "esp_http_server.h"
#include <ArduinoJson.h>
#include "soc/rtc_cntl_reg.h"
#include <Preferences.h>

// ======================== CAMERA PINS (AI-Thinker) ========================
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
#define RESET_BUTTON_PIN 12

// ======================== WiFi Configuration ========================
#define AP_SSID "RoadSafe-AI-Setup"
#define AP_PASSWORD "1234567800"
#define WIFI_TIMEOUT 30000

// ======================== UDP DISCOVERY ========================
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

// ======================== STATE MACHINE ========================
// Instead of scattered booleans, we use a clear state machine:
//   MONITORING  → stream is active, app is analyzing frames
//   ALARM_ON    → stream stopped, buzzer sounding, waiting for driver
//   ALARM_OFF   → transitioning back to MONITORING
enum DeviceState {
    STATE_MONITORING,
    STATE_ALARM_ACTIVE
};

volatile DeviceState deviceState = STATE_MONITORING;
volatile bool stream_must_stop = false;
volatile bool stream_running = false;

unsigned long alarm_start_time = 0;
int total_drowsiness_alerts = 0;

// ======================== BUZZER TASK (Core 0) ========================
TaskHandle_t buzzerTaskHandle = NULL;

void buzzerTask(void * parameter) {
    // This task owns GPIO 13 toggling — runs on Core 0
    // completely independent of camera/HTTP on Core 1
    
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    
    Serial.println("🔊 Buzzer task running on Core 0");

    for (;;) {
        if (deviceState == STATE_ALARM_ACTIVE && !stream_running) {
            // Stream has stopped — GPIO 13 is free — BUZZ!
            // Re-initialize pin each time alarm starts (reclaim from HSPI)
            pinMode(BUZZER_PIN, OUTPUT);
            
            digitalWrite(BUZZER_PIN, HIGH);
            vTaskDelay(pdMS_TO_TICKS(350));
            
            digitalWrite(BUZZER_PIN, LOW);
            vTaskDelay(pdMS_TO_TICKS(150));
            
        } else if (deviceState == STATE_ALARM_ACTIVE && stream_running) {
            // Alarm requested but stream hasn't stopped yet — just wait
            vTaskDelay(pdMS_TO_TICKS(10));
            
        } else {
            // STATE_MONITORING — ensure buzzer is OFF
            digitalWrite(BUZZER_PIN, LOW);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ====================== UDP DISCOVERY ======================
void setupUDPDiscovery() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (udp.begin(DISCOVERY_PORT)) {
        discoveryEnabled = true;
        Serial.printf("📡 UDP Discovery on port %d\n", DISCOVERY_PORT);
    }
}

void handleUDPDiscovery() {
    if (!discoveryEnabled) return;
    int packetSize = udp.parsePacket();
    if (packetSize) {
        char incomingPacket[255];
        int len = udp.read(incomingPacket, 255);
        if (len > 0) incomingPacket[len] = '\0';
        if (String(incomingPacket) == "ROADSAFE_DISCOVER") {
            String response = "ROADSAFE_RESPONSE:" + WiFi.localIP().toString() + ":" + DEVICE_NAME;
            udp.beginPacket(udp.remoteIP(), udp.remotePort());
            udp.write((uint8_t*)response.c_str(), response.length());
            udp.endPacket();
        }
    }
}

// ====================== CORS HELPER ======================
esp_err_t set_cors_headers(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    return ESP_OK;
}

// ====================== WiFi STORAGE ======================
void saveWiFiCredentials(String ssid, String password) {
    preferences.begin("wifi", false);
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.putBool("configured", true);
    preferences.end();
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
}

// ====================== SETUP PAGE HTML ======================
const char setup_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>RoadSafe AI - WiFi Setup</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; display: flex; align-items: center; justify-content: center; padding: 20px; }
        .container { background: white; border-radius: 20px; box-shadow: 0 20px 60px rgba(0,0,0,0.3); max-width: 500px; width: 100%; overflow: hidden; }
        .header { background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; padding: 30px; text-align: center; }
        .header h1 { font-size: 28px; margin-bottom: 10px; }
        .header p { opacity: 0.9; font-size: 14px; }
        .icon { width: 60px; height: 60px; margin: 0 auto 15px; background: rgba(255,255,255,0.2); border-radius: 15px; display: flex; align-items: center; justify-content: center; font-size: 30px; }
        .content { padding: 30px; }
        .form-group { margin-bottom: 20px; }
        label { display: block; margin-bottom: 8px; color: #333; font-weight: 500; font-size: 14px; }
        input, select { width: 100%; padding: 12px 15px; border: 2px solid #e0e0e0; border-radius: 10px; font-size: 16px; transition: all 0.3s; }
        input:focus, select:focus { outline: none; border-color: #667eea; }
        .btn { width: 100%; padding: 15px; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; border: none; border-radius: 10px; font-size: 16px; font-weight: 600; cursor: pointer; }
        .scanning { text-align: center; padding: 20px; color: #666; }
        .spinner { border: 3px solid #f3f3f3; border-top: 3px solid #667eea; border-radius: 50%; width: 40px; height: 40px; animation: spin 1s linear infinite; margin: 0 auto 15px; }
        @keyframes spin { 0% { transform: rotate(0deg); } 100% { transform: rotate(360deg); } }
        .status { margin-top: 20px; padding: 15px; border-radius: 10px; text-align: center; display: none; }
        .status.success { background: #d4edda; color: #155724; }
        .status.error { background: #f8d7da; color: #721c24; }
        .info-box { background: #e7f3ff; border-left: 4px solid #2196F3; padding: 15px; border-radius: 8px; margin-bottom: 20px; }
        .info-box p { color: #0c5fa8; font-size: 13px; line-height: 1.5; }
        .password-toggle { position: relative; }
        .toggle-btn { position: absolute; right: 12px; top: 50%; transform: translateY(-50%); background: none; border: none; cursor: pointer; font-size: 18px; }
    </style>
</head>
<body>
    <div class="container">
        <div class="header"><div class="icon">🚗</div><h1>RoadSafe AI</h1><p>Driver Drowsiness Detection System</p></div>
        <div class="content">
            <div class="info-box"><p><strong>📡 Setup Required</strong><br>Connect your ESP32-CAM to your WiFi network.</p></div>
            <div id="scanningDiv" class="scanning"><div class="spinner"></div><p>Scanning for WiFi networks...</p></div>
            <form id="wifiForm" style="display: none;">
                <div class="form-group"><label for="ssid">WiFi Network</label><select id="ssid" name="ssid" required><option value="">Select a network...</option></select></div>
                <div class="form-group"><label for="password">Password</label><div class="password-toggle"><input type="password" id="password" name="password" placeholder="Enter WiFi password" required><button type="button" class="toggle-btn" onclick="togglePassword()">👁️</button></div></div>
                <button type="submit" class="btn">Connect to WiFi</button>
            </form>
            <div id="status" class="status"></div>
        </div>
    </div>
    <script>
        fetch('/scan').then(r=>r.json()).then(data=>{document.getElementById('scanningDiv').style.display='none';document.getElementById('wifiForm').style.display='block';const s=document.getElementById('ssid');data.networks.forEach(n=>{const o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+' dBm) '+n.encryption;s.appendChild(o);});}).catch(()=>{document.getElementById('scanningDiv').innerHTML='<p style="color:#f44336;">Failed to scan. Refresh.</p>';});
        document.getElementById('wifiForm').addEventListener('submit',function(e){e.preventDefault();const ssid=document.getElementById('ssid').value,pw=document.getElementById('password').value,st=document.getElementById('status');st.style.display='block';st.className='status';st.textContent='⏳ Connecting...';fetch('/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:ssid,password:pw})}).then(r=>r.json()).then(d=>{if(d.success){st.className='status success';st.innerHTML='✓ Connected! IP: '+d.ip+'<br>Redirecting...';setTimeout(()=>{window.location.href='http://'+d.ip;},3000);}else{st.className='status error';st.textContent='✗ '+d.message;}}).catch(err=>{st.className='status error';st.textContent='✗ '+err.message;});});
        function togglePassword(){const i=document.getElementById('password');i.type=i.type==='password'?'text':'password';}
    </script>
</body>
</html>
)rawliteral";

// ====================== CAMERA PAGE HTML ======================
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
        p.sub { margin-top: 0; color: #94a3b8; }
        .frame { max-width: 100%; width: 420px; border-radius: 14px; box-shadow: 0 18px 40px rgba(15,23,42,0.6); overflow: hidden; background: #1e293b; border: 1px solid #334155; position: relative; min-height: 240px; }
        .frame img { width: 100%; display: block; }
        .paused-overlay { position: absolute; top: 0; left: 0; right: 0; bottom: 0; background: rgba(15,23,42,0.9); display: none; align-items: center; justify-content: center; flex-direction: column; }
        .paused-overlay.show { display: flex; }
        .paused-overlay .icon { font-size: 48px; margin-bottom: 10px; animation: pulse 1s infinite alternate; }
        .paused-overlay p { color: #f87171; font-weight: 600; font-size: 16px; }
        @keyframes pulse { from { opacity: 0.6; } to { opacity: 1; } }
        .stats { margin-top: 24px; display: grid; gap: 12px; width: 420px; max-width: 100%; }
        .card { padding: 16px 18px; border-radius: 12px; background: #1e293b; border: 1px solid #334155; display: flex; justify-content: space-between; align-items: center; }
        .label { color: #94a3b8; font-size: 14px; }
        .value { font-size: 18px; font-weight: 600; }
        .status-indicator { width: 12px; height: 12px; border-radius: 50%; margin-right: 8px; }
        .status-row { display: flex; align-items: center; }
    </style>
</head>
<body>
    <h1>RoadSafe AI</h1>
    <p class="sub">Live stream from ESP32-CAM</p>
    <div class="frame">
        <img id="stream" src="/stream" alt="Live stream"/>
        <div id="pausedOverlay" class="paused-overlay">
            <div class="icon">🚨</div>
            <p>DROWSINESS DETECTED</p>
            <p style="color:#94a3b8;font-size:13px;margin-top:8px;">Buzzer active — Stream paused</p>
        </div>
    </div>
    <div class="stats">
        <div class="card"><span class="label">Status</span><span class="status-row"><span id="statusLed" class="status-indicator" style="background:#f87171"></span><span class="value" id="statusText">Loading…</span></span></div>
        <div class="card"><span class="label">Alarm</span><span class="value" id="alarmStatus">OFF</span></div>
        <div class="card"><span class="label">Stream</span><span class="value" id="streamStatus">-</span></div>
        <div class="card"><span class="label">WiFi</span><span class="value" id="wifiSsid">-</span></div>
        <div class="card"><span class="label">IP</span><span class="value" id="ipAddr">-</span></div>
        <div class="card"><span class="label">RSSI</span><span class="value" id="rssi">-</span></div>
        <div class="card"><span class="label">Alerts</span><span class="value" id="alerts">0</span></div>
    </div>
    <script>
        let wasAlarming = false;
        async function refreshStatus(){
            try{
                const r=await fetch('/status');
                const d=await r.json();
                const alarming = d.device_state === 'ALARM_ACTIVE';
                
                document.getElementById('statusText').textContent=d.status||'online';
                document.getElementById('statusLed').style.background=alarming?'#f87171':'#34d399';
                document.getElementById('alarmStatus').textContent=alarming?'🔊 ACTIVE':'OFF';
                document.getElementById('alarmStatus').style.color=alarming?'#f87171':'#34d399';
                document.getElementById('streamStatus').textContent=d.stream_running?'LIVE':'PAUSED';
                document.getElementById('streamStatus').style.color=d.stream_running?'#34d399':'#fb923c';
                document.getElementById('wifiSsid').textContent=d.wifi_ssid||'-';
                document.getElementById('ipAddr').textContent=d.ip||'-';
                document.getElementById('alerts').textContent=d.alerts??'-';
                document.getElementById('rssi').textContent=d.rssi?d.rssi+' dBm':'-';
                
                // Show/hide paused overlay
                document.getElementById('pausedOverlay').classList.toggle('show', alarming);
                
                // When alarm turns OFF, reconnect stream
                if(wasAlarming && !alarming){
                    const img = document.getElementById('stream');
                    img.src = '/stream?t=' + Date.now();
                }
                wasAlarming = alarming;
            }catch(e){
                document.getElementById('statusText').textContent='offline';
                document.getElementById('statusLed').style.background='#f87171';
            }
        }
        refreshStatus();
        setInterval(refreshStatus, 2000);
    </script>
</body>
</html>
)rawliteral";

// ====================== HTTP HANDLERS ======================

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
    WiFi.begin(ssid.c_str(), password.c_str());
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) { delay(500); attempts++; }
    String response;
    if (WiFi.status() == WL_CONNECTED) {
        saveWiFiCredentials(ssid, password);
        setupUDPDiscovery();
        response = "{\"success\":true,\"ip\":\"" + WiFi.localIP().toString() + "\"}";
    } else {
        response = "{\"success\":false,\"message\":\"Failed to connect\"}";
    }
    return httpd_resp_send(req, response.c_str(), response.length());
}

static esp_err_t setup_handler(httpd_req_t *req) {
    set_cors_headers(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, setup_html, strlen(setup_html));
}

static esp_err_t index_handler(httpd_req_t *req) {
    set_cors_headers(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, camera_html, strlen(camera_html));
}

// ======================== STREAM HANDLER ========================
// This is the critical handler. It runs in a tight loop sending MJPEG frames.
// When stream_must_stop becomes true, it breaks out IMMEDIATELY,
// freeing GPIO 13 for the buzzer.
static esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    char part_buf[64];
    static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=frame";
    static const char* _STREAM_BOUNDARY = "\r\n--frame\r\n";
    static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

    // Don't allow stream to start if alarm is active
    if (deviceState == STATE_ALARM_ACTIVE) {
        set_cors_headers(req);
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Stream paused: alarm active", 27);
        return ESP_OK;
    }

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;
    set_cors_headers(req);

    stream_running = true;
    stream_must_stop = false;
    Serial.println("📹 === STREAM STARTED ===");

    while (true) {
        // *** CHECK STOP FLAG FIRST — before any camera/GPIO operations ***
        if (stream_must_stop) {
            Serial.println("📹 Stream received STOP signal");
            break;
        }

        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("📹 Camera frame failed");
            res = ESP_FAIL;
            break;
        }

        // Check again after camera capture (capture takes time)
        if (stream_must_stop) {
            esp_camera_fb_return(fb);
            Serial.println("📹 Stream received STOP signal (post-capture)");
            break;
        }

        res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        if (res == ESP_OK) {
            size_t hlen = snprintf(part_buf, 64, _STREAM_PART, fb->len);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }

        esp_camera_fb_return(fb);

        if (res != ESP_OK) {
            Serial.println("📹 Stream send failed (client disconnected?)");
            break;
        }

        // Small yield so other tasks get CPU time
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    stream_running = false;
    Serial.println("📹 === STREAM STOPPED ===");

    return res;
}

// ======================== CAPTURE HANDLER ========================
static esp_err_t capture_handler(httpd_req_t *req) {
    // Single frame capture - also blocked during alarm
    if (deviceState == STATE_ALARM_ACTIVE) {
        set_cors_headers(req);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"alarm_active\"}", 23);
        return ESP_OK;
    }

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
    if (ret <= 0) {
        Serial.println("❌ Alarm: no body received");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    DynamicJsonDocument doc(512);
    DeserializationError error = deserializeJson(doc, content);
    if (error) {
        Serial.printf("❌ Alarm: JSON error: %s\n", error.c_str());
        return ESP_FAIL;
    }

    String command = doc["command"];
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    if (command == "ALARM_ON") {
        Serial.println("\n🚨🚨🚨 ALARM_ON RECEIVED 🚨🚨🚨");

        // STEP 1: Signal stream to stop
        stream_must_stop = true;
        Serial.println("   → stream_must_stop = true");

        // STEP 2: Wait for stream to actually stop
        // The stream checks stream_must_stop every frame (~30-50ms)
        // We give it up to 3 seconds
        unsigned long waitStart = millis();
        while (stream_running && (millis() - waitStart < 3000)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (stream_running) {
            Serial.println("   ⚠️ Stream didn't stop in 3s — forcing state anyway");
            // Even if stream is stuck, we set state so buzzer task
            // will activate the moment it does stop
        } else {
            Serial.printf("   ✅ Stream stopped in %lu ms\n", millis() - waitStart);
        }

        // STEP 3: Transition to alarm state
        // The buzzer task on Core 0 will see this and start buzzing
        // once it confirms stream_running == false
        deviceState = STATE_ALARM_ACTIVE;
        total_drowsiness_alerts++;
        alarm_start_time = millis();

        Serial.printf("   🔊 Alarm ACTIVE (alert #%d)\n", total_drowsiness_alerts);
        Serial.println("   📹 Stream stopped → GPIO 13 free → Buzzer task will buzz\n");

        String response = "{\"status\":\"ok\",\"alarm_active\":true,\"stream_stopped\":true,\"alerts\":" + String(total_drowsiness_alerts) + "}";
        return httpd_resp_send(req, response.c_str(), response.length());

    } else if (command == "ALARM_OFF") {
        Serial.println("\n🔇🔇🔇 ALARM_OFF RECEIVED 🔇🔇🔇");

        // STEP 1: Deactivate alarm — buzzer task will stop buzzing
        deviceState = STATE_MONITORING;
        stream_must_stop = false;

        // STEP 2: Explicitly ensure buzzer is OFF
        pinMode(BUZZER_PIN, OUTPUT);
        digitalWrite(BUZZER_PIN, LOW);

        Serial.println("   🔇 Buzzer OFF");
        Serial.println("   📹 App/browser can reconnect to /stream now\n");

        String response = "{\"status\":\"ok\",\"alarm_active\":false,\"stream_stopped\":false,\"alerts\":" + String(total_drowsiness_alerts) + "}";
        return httpd_resp_send(req, response.c_str(), response.length());

    } else {
        Serial.printf("⚠️ Unknown alarm command: '%s'\n", command.c_str());
        String response = "{\"status\":\"error\",\"message\":\"unknown command\"}";
        return httpd_resp_send(req, response.c_str(), response.length());
    }
}

// ======================== TEST ALARM HANDLER ========================
static esp_err_t test_alarm_handler(httpd_req_t *req) {
    set_cors_headers(req);

    Serial.println("\n🧪 TEST ALARM — stopping stream first...");

    // Stop stream
    stream_must_stop = true;
    unsigned long waitStart = millis();
    while (stream_running && (millis() - waitStart < 3000)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    Serial.printf("   Stream stopped: %s\n", stream_running ? "NO (timeout)" : "YES");

    // Reclaim pin and test
    pinMode(BUZZER_PIN, OUTPUT);
    for (int i = 0; i < 3; i++) {
        Serial.printf("   Beep %d\n", i + 1);
        digitalWrite(BUZZER_PIN, HIGH);
        delay(300);
        digitalWrite(BUZZER_PIN, LOW);
        delay(200);
    }

    // Allow stream to resume
    stream_must_stop = false;
    Serial.println("🧪 Test done — stream can reconnect\n");

    String response = "{\"test\":\"completed\",\"buzzer_pin\":" + String(BUZZER_PIN) + ",\"beeps\":3}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response.c_str(), response.length());
}

// ======================== STATUS HANDLER ========================
static esp_err_t status_handler(httpd_req_t *req) {
    set_cors_headers(req);

    String stateStr = (deviceState == STATE_ALARM_ACTIVE) ? "ALARM_ACTIVE" : "MONITORING";

    String json = "{";
    json += "\"status\":\"online\",";
    json += "\"device_state\":\"" + stateStr + "\",";
    json += "\"alarm_active\":" + String(deviceState == STATE_ALARM_ACTIVE ? "true" : "false") + ",";
    json += "\"stream_running\":" + String(stream_running ? "true" : "false") + ",";
    json += "\"alerts\":" + String(total_drowsiness_alerts) + ",";
    json += "\"wifi_ssid\":\"" + WiFi.SSID() + "\",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"buzzer_pin\":" + String(BUZZER_PIN) + ",";
    json += "\"free_heap\":" + String(ESP.getFreeHeap());
    json += "}";

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json.c_str(), json.length());
}

// ====================== RESET HANDLER ======================
static esp_err_t reset_handler(httpd_req_t *req) {
    set_cors_headers(req);
    clearWiFiCredentials();
    String response = "{\"success\":true,\"message\":\"Restarting...\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response.c_str(), response.length());
    delay(1000);
    ESP.restart();
    return ESP_OK;
}

// ======================== START SERVERS ========================
void startCameraServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_uri_handlers = 10;

    httpd_uri_t index_uri     = {"/",           HTTP_GET,  index_handler,      NULL};
    httpd_uri_t stream_uri    = {"/stream",     HTTP_GET,  stream_handler,     NULL};
    httpd_uri_t capture_uri   = {"/capture",    HTTP_GET,  capture_handler,    NULL};
    httpd_uri_t alarm_uri     = {"/alarm",      HTTP_POST, alarm_handler,      NULL};
    httpd_uri_t test_uri      = {"/test_alarm", HTTP_GET,  test_alarm_handler, NULL};
    httpd_uri_t status_uri    = {"/status",     HTTP_GET,  status_handler,     NULL};
    httpd_uri_t reset_uri     = {"/reset",      HTTP_POST, reset_handler,      NULL};

    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(camera_httpd, &index_uri);
        httpd_register_uri_handler(camera_httpd, &stream_uri);
        httpd_register_uri_handler(camera_httpd, &capture_uri);
        httpd_register_uri_handler(camera_httpd, &alarm_uri);
        httpd_register_uri_handler(camera_httpd, &test_uri);
        httpd_register_uri_handler(camera_httpd, &status_uri);
        httpd_register_uri_handler(camera_httpd, &reset_uri);

        Serial.println("✅ Server started:");
        Serial.println("   GET  /           → Web UI");
        Serial.println("   GET  /stream     → MJPEG stream");
        Serial.println("   GET  /capture    → Single JPEG");
        Serial.println("   POST /alarm      → {\"command\":\"ALARM_ON\"} or {\"command\":\"ALARM_OFF\"}");
        Serial.println("   GET  /test_alarm → Test buzzer (3 beeps)");
        Serial.println("   GET  /status     → Device status JSON");
        Serial.println("   POST /reset      → Clear WiFi & restart in AP mode");
    }
}

void startConfigServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    httpd_uri_t setup_uri   = {"/",        HTTP_GET,  setup_handler,   NULL};
    httpd_uri_t scan_uri    = {"/scan",    HTTP_GET,  scan_handler,    NULL};
    httpd_uri_t connect_uri = {"/connect", HTTP_POST, connect_handler, NULL};
    httpd_uri_t reset_uri   = {"/reset",   HTTP_POST, reset_handler,   NULL};

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
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 12;
    config.fb_count = 2;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("❌ Camera init failed: 0x%x\n", err);
        return;
    }

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

    Serial.println("✓ Camera initialized");
}

// ======================== SETUP ========================
void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n\n╔════════════════════════════════════════╗");
    Serial.println("║   RoadSafe AI - ESP32-CAM              ║");
    Serial.println("║   Stream-Stop Buzzer Architecture      ║");
    Serial.println("╚════════════════════════════════════════╝\n");

    // Init buzzer
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    Serial.printf("✓ Buzzer on GPIO %d\n", BUZZER_PIN);

    // Startup buzzer test (before camera init claims GPIO)
    Serial.println("🔊 Startup buzzer test...");
    for (int i = 0; i < 3; i++) {
        digitalWrite(BUZZER_PIN, HIGH);
        delay(100);
        digitalWrite(BUZZER_PIN, LOW);
        delay(100);
    }
    Serial.println("✓ Buzzer OK\n");

    // Start buzzer task on Core 0
    xTaskCreatePinnedToCore(
        buzzerTask,
        "BuzzerTask",
        2048,
        NULL,
        3,              // Priority 3 (higher = more responsive buzzer)
        &buzzerTaskHandle,
        0               // Core 0 (separate from camera/HTTP on Core 1)
    );

#if defined(RESET_BUTTON_PIN) && RESET_BUTTON_PIN >= 0
    pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
#endif

    initCamera();

    if (loadWiFiCredentials()) {
        Serial.printf("✓ Saved WiFi: %s — connecting...\n", saved_ssid.c_str());

        WiFi.mode(WIFI_STA);
        WiFi.begin(saved_ssid.c_str(), saved_password.c_str());

        unsigned long startAttempt = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < WIFI_TIMEOUT) {
            delay(500);
            Serial.print(".");
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("✓ Connected! IP: %s  Signal: %d dBm\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());

            setupUDPDiscovery();
            startCameraServer();

            Serial.println("\n╔════════════════════════════════════════╗");
            Serial.printf("║  http://%-30s  ║\n", WiFi.localIP().toString().c_str());
            Serial.println("╠════════════════════════════════════════╣");
            Serial.println("║  FLOW:                                 ║");
            Serial.println("║  1. App reads /stream                  ║");
            Serial.println("║  2. App detects drowsiness             ║");
            Serial.println("║  3. App sends POST /alarm ALARM_ON     ║");
            Serial.println("║  4. ESP stops stream, buzzer sounds    ║");
            Serial.println("║  5. Driver responds via app            ║");
            Serial.println("║  6. App sends POST /alarm ALARM_OFF    ║");
            Serial.println("║  7. App reconnects to /stream          ║");
            Serial.println("╚════════════════════════════════════════╝\n");
        } else {
            Serial.println("✗ Connection failed — restarting in AP mode");
            clearWiFiCredentials();
            ESP.restart();
        }
    } else {
        Serial.println("ℹ No WiFi — Starting AP setup mode");
        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID, AP_PASSWORD);
        Serial.printf("  Network: %s  Password: %s\n", AP_SSID, AP_PASSWORD);
        Serial.printf("  URL: http://%s\n\n", WiFi.softAPIP().toString().c_str());
        startConfigServer();
    }
}

// ======================== LOOP ========================
void loop() {
    handleUDPDiscovery();

    // Reset button check
    #if defined(RESET_BUTTON_PIN) && RESET_BUTTON_PIN >= 0
    static unsigned long buttonPressTime = 0;
    static bool buttonPressed = false;

    if (digitalRead(RESET_BUTTON_PIN) == LOW) {
        if (!buttonPressed) {
            buttonPressed = true;
            buttonPressTime = millis();
        } else if (millis() - buttonPressTime > 5000) {
            Serial.println("🔄 Reset button — clearing WiFi...");
            clearWiFiCredentials();
            ESP.restart();
        }
    } else {
        buttonPressed = false;
    }
    #endif

    delay(10);
}