#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include <ArduinoJson.h>
#include "soc/rtc_cntl_reg.h"

// Camera pins for AI-Thinker ESP32-CAM
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

// Hardware pins - CHANGED BUZZER PIN!
#define BUZZER_PIN 13    // Changed from GPIO 12 to GPIO 13 (safe pin)
#define LED_PIN 4       // GPIO 4 is still fine

// WiFi credentials - UPDATE THESE!
const char* ssid = "Galaxy A05 2783";
const char* password = "lahiru123";

httpd_handle_t camera_httpd = NULL;

// Alarm control variables
bool alarm_active = false;
unsigned long alarm_start_time = 0;
bool buzzer_state = false;
unsigned long last_buzzer_toggle = 0;

// Status tracking
int total_drowsiness_alerts = 0;
String current_detection_status = "Normal";
unsigned long last_detection_time = 0;

// CORS enabled response
esp_err_t set_cors_headers(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Authorization");
    return ESP_OK;
}

// Video stream handler - SIMPLIFIED for stability
static esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    char * part_buf[64];
    
    static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=123456789000000000000987654321";
    static const char* _STREAM_BOUNDARY = "\r\n--123456789000000000000987654321\r\n";
    static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";
    
    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;
    
    set_cors_headers(req);
    
    Serial.println("Stream started");
    
    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("Camera capture failed");
            res = ESP_FAIL;
            break;
        }
        
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if (res == ESP_OK) {
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, fb->len);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }
        
        esp_camera_fb_return(fb);
        
        if (res != ESP_OK) {
            break;
        }
        
        delay(80);  // Slightly faster for better stream
    }
    
    return res;
}

// Single frame capture
static esp_err_t capture_handler(httpd_req_t *req) {
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    
    fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("Camera capture failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera capture failed");
        return ESP_FAIL;
    }
    
    set_cors_headers(req);
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    
    res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    
    return res;
}

// Alarm control endpoint
static esp_err_t alarm_handler(httpd_req_t *req) {
    char content[200];
    size_t recv_size = min(req->content_len, sizeof(content));
    
    int ret = httpd_req_recv(req, content, recv_size);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive data");
        return ESP_FAIL;
    }
    
    content[ret] = '\0';
    
    // Parse JSON command
    DynamicJsonDocument doc(512);  // Reduced size
    DeserializationError error = deserializeJson(doc, content);
    
    if (error) {
        Serial.println("Failed to parse JSON");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    String command = doc["command"];
    String status = doc["status"];
    
    Serial.printf("Command: %s, Status: %s\n", command.c_str(), status.c_str());
    
    // Handle alarm commands
    if (command == "ALARM_ON") {
        if (!alarm_active) {
            alarm_active = true;
            alarm_start_time = millis();
            total_drowsiness_alerts++;
            Serial.println("ALARM ACTIVATED!");
        }
    } else if (command == "ALARM_OFF") {
        if (alarm_active) {
            alarm_active = false;
            buzzer_state = false;
            digitalWrite(BUZZER_PIN, LOW);
            digitalWrite(LED_PIN, LOW);
            Serial.println("Alarm stopped");
        }
    }
    
    current_detection_status = status;
    last_detection_time = millis();
    
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    
    String response = "{\"status\":\"ok\",\"alarm_active\":" + String(alarm_active ? "true" : "false") + "}";
    
    return httpd_resp_send(req, response.c_str(), response.length());
}

// Status endpoint
static esp_err_t status_handler(httpd_req_t *req) {
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    
    String response = "{"
                     "\"status\":\"online\","
                     "\"alarm_active\":" + String(alarm_active ? "true" : "false") + ","
                     "\"total_alerts\":" + String(total_drowsiness_alerts) + ","
                     "\"free_heap\":" + String(ESP.getFreeHeap()) +
                     "}";
    
    return httpd_resp_send(req, response.c_str(), response.length());
}

// Test buzzer endpoint
static esp_err_t test_buzzer_handler(httpd_req_t *req) {
    Serial.println("Testing buzzer on GPIO 2...");
    
    for (int i = 0; i < 3; i++) {
        digitalWrite(BUZZER_PIN, HIGH);
        digitalWrite(LED_PIN, HIGH);
        Serial.printf("Buzzer ON - Test %d\n", i+1);
        delay(300);
        digitalWrite(BUZZER_PIN, LOW);
        digitalWrite(LED_PIN, LOW);
        Serial.printf("Buzzer OFF - Test %d\n", i+1);
        delay(300);
    }
    
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    String response = "{\"status\":\"buzzer_test_complete\"}";
    
    return httpd_resp_send(req, response.c_str(), response.length());
}

// Handle OPTIONS for CORS
static esp_err_t options_handler(httpd_req_t *req) {
    set_cors_headers(req);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// Updated HTML page with LARGER video display
static const char PROGMEM INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32-CAM Test</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { 
            font-family: Arial; 
            text-align: center; 
            margin: 20px; 
            background: #f5f5f5;
        }
        .container {
            max-width: 900px;
            margin: 0 auto;
        }
        /* LARGER VIDEO STREAM */
        img { 
            width: 100%; 
            max-width: 640px; 
            height: auto;
            border: 3px solid #333; 
            border-radius: 10px;
            box-shadow: 0 4px 8px rgba(0,0,0,0.3);
        }
        button { 
            padding: 15px 25px; 
            margin: 10px; 
            font-size: 18px; 
            background: #007bff;
            color: white;
            border: none;
            border-radius: 5px;
            cursor: pointer;
        }
        button:hover {
            background: #0056b3;
        }
        .status { 
            background: #ffffff; 
            padding: 15px; 
            margin: 15px 0; 
            border-radius: 8px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
            font-size: 16px;
        }
        .alert {
            background: #dc3545;
            color: white;
        }
        h1 {
            color: #333;
            margin-bottom: 30px;
        }
        h3 {
            color: #555;
            margin-top: 30px;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>ESP32-CAM Mobile Integration</h1>
        
        <button onclick="testBuzzer()">🔊 Test Buzzer (GPIO 2)</button>
        <button onclick="testAlarm()">⚠️ Test Alarm (5s)</button>
        
        <div class="status" id="status">Status: Ready - Buzzer on GPIO 2</div>
        
        <h3>📹 Live Stream (Larger View)</h3>
        <img src="/stream" alt="Camera Stream Loading..." id="streamImg">
        
        <div class="status">
            <strong>📡 API Endpoints:</strong><br>
            Stream: <code>/stream</code> | 
            Capture: <code>/capture</code> | 
            Alarm: <code>/alarm</code> | 
            Status: <code>/status</code>
        </div>
    </div>
    
    <script>
        function testBuzzer() {
            const button = event.target;
            button.disabled = true;
            button.textContent = '🔊 Testing...';
            document.getElementById('status').innerHTML = '<strong>Testing buzzer on GPIO 2...</strong>';
            
            fetch('/test-buzzer')
            .then(response => response.json())
            .then(data => {
                document.getElementById('status').innerHTML = '<strong>✅ Buzzer test completed!</strong>';
                button.disabled = false;
                button.textContent = '🔊 Test Buzzer (GPIO 2)';
            })
            .catch(error => {
                document.getElementById('status').innerHTML = '<strong>❌ Buzzer test failed</strong>';
                button.disabled = false;
                button.textContent = '🔊 Test Buzzer (GPIO 2)';
            });
        }
        
        function testAlarm() {
            const button = event.target;
            button.disabled = true;
            button.textContent = '⚠️ Alarming...';
            document.getElementById('status').className = 'status alert';
            document.getElementById('status').innerHTML = '<strong>🚨 ALARM ACTIVE - Testing for 5 seconds...</strong>';
            
            fetch('/alarm', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({command: 'ALARM_ON', status: 'Test'})
            });
            
            setTimeout(() => {
                fetch('/alarm', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({command: 'ALARM_OFF', status: 'Normal'})
                });
                document.getElementById('status').className = 'status';
                document.getElementById('status').innerHTML = '<strong>✅ Alarm test completed!</strong>';
                button.disabled = false;
                button.textContent = '⚠️ Test Alarm (5s)';
            }, 5000);
        }
        
        // Auto-refresh status
        setInterval(() => {
            fetch('/status')
            .then(response => response.json())
            .then(data => {
                if (!document.getElementById('status').innerHTML.includes('Testing') && 
                    !document.getElementById('status').innerHTML.includes('ALARM ACTIVE')) {
                    document.getElementById('status').innerHTML = 
                        `<strong>Status:</strong> Online | <strong>Alerts:</strong> ${data.total_alerts} | <strong>Memory:</strong> ${data.free_heap} bytes`;
                }
            })
            .catch(error => console.log('Status update failed'));
        }, 3000);
    </script>
</body>
</html>
)rawliteral";

static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

void startCameraServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;  // Reduced stack size
    
    httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL};
    httpd_uri_t stream_uri = {.uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL};
    httpd_uri_t capture_uri = {.uri = "/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL};
    httpd_uri_t alarm_uri = {.uri = "/alarm", .method = HTTP_POST, .handler = alarm_handler, .user_ctx = NULL};
    httpd_uri_t status_uri = {.uri = "/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL};
    httpd_uri_t test_buzzer_uri = {.uri = "/test-buzzer", .method = HTTP_GET, .handler = test_buzzer_handler, .user_ctx = NULL};
    httpd_uri_t options_uri = {.uri = "/*", .method = HTTP_OPTIONS, .handler = options_handler, .user_ctx = NULL};
    
    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(camera_httpd, &index_uri);
        httpd_register_uri_handler(camera_httpd, &stream_uri);
        httpd_register_uri_handler(camera_httpd, &capture_uri);
        httpd_register_uri_handler(camera_httpd, &alarm_uri);
        httpd_register_uri_handler(camera_httpd, &status_uri);
        httpd_register_uri_handler(camera_httpd, &test_buzzer_uri);
        httpd_register_uri_handler(camera_httpd, &options_uri);
        Serial.println("HTTP server started");
    }
}

void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    Serial.begin(115200);
    Serial.setDebugOutput(false);  // Disable debug output to save memory
    delay(1000);
    
    Serial.println("ESP32-CAM Starting...");
    Serial.println("IMPORTANT: Buzzer moved to GPIO 2 (was GPIO 12)");
    
    // Initialize pins - BUZZER NOW ON GPIO 2
    pinMode(BUZZER_PIN, OUTPUT);  // GPIO 2
    pinMode(LED_PIN, OUTPUT);     // GPIO 4
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(LED_PIN, LOW);
    
    // Enhanced hardware test with buzzer
    Serial.println("Testing hardware...");
    digitalWrite(LED_PIN, HIGH);
    digitalWrite(BUZZER_PIN, HIGH);
    Serial.println("LED and Buzzer ON");
    delay(500);
    digitalWrite(LED_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);
    Serial.println("LED and Buzzer OFF");
    Serial.println("Hardware test complete - If you heard a beep, buzzer works!");
    
    // UPDATED camera configuration for LARGER stream
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
    
    // LARGER resolution for bigger stream window
    if (psramFound()) {
        config.frame_size = FRAMESIZE_VGA;     // 640x480 - Much larger!
        config.jpeg_quality = 10;              // Better quality
        config.fb_count = 1;                   // Single buffer for stability
        Serial.println("PSRAM found - Using VGA (640x480) for larger stream");
    } else {
        config.frame_size = FRAMESIZE_HVGA;    // 480x320 - Larger than before
        config.jpeg_quality = 12;
        config.fb_count = 1;
        Serial.println("No PSRAM - Using HVGA (480x320) for larger stream");
    }
    
    Serial.println("Initializing camera...");
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed: 0x%x\n", err);
        return;
    }
    Serial.println("Camera OK");
    
    // Connect WiFi
    WiFi.begin(ssid, password);
    Serial.print("Connecting WiFi");
    
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println(" Connected!");
    
    startCameraServer();
    
    Serial.println("========================================");
    Serial.print("Camera Ready! Use 'http://");
    Serial.print(WiFi.localIP());
    Serial.println("' to connect");
    Serial.println("========================================");
    Serial.println("🔊 BUZZER IS NOW ON GPIO 2 (not GPIO 12)");
    Serial.println("📹 STREAM SIZE: VGA (640x480) for larger view");
    Serial.println("========================================");
}

void loop() {
    // Handle buzzer alarm with improved pattern
    if (alarm_active) {
        unsigned long current_time = millis();
        if (current_time - last_buzzer_toggle > 400) {  // Slightly faster beeping
            buzzer_state = !buzzer_state;
            digitalWrite(BUZZER_PIN, buzzer_state ? HIGH : LOW);
            digitalWrite(LED_PIN, buzzer_state ? HIGH : LOW);
            last_buzzer_toggle = current_time;
            
            if (buzzer_state) {
                Serial.println("🔊 BUZZ!");
            }
        }
    }
    
    delay(50);
}