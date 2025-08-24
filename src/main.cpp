#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include "esp_timer.h"
#include "img_converters.h"

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

// WiFi credentials - UPDATE THESE!
const char* ssid = "SLT FIBRE";
const char* password = "96132005";

// Motion detection variables
bool motion_detection_enabled = true;
int motion_count = 0;
unsigned long last_motion_time = 0;
static uint8_t *prev_frame = NULL;
static size_t prev_frame_size = 0;

// LED pin (built-in flash LED for motion indication)
#define LED_PIN 4

httpd_handle_t camera_httpd = NULL;

// Simplified HTML interface
static const char PROGMEM INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width,initial-scale=1">
    <title>ESP32-CAM Motion Detection</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            background: #1a1a1a;
            color: white;
            text-align: center;
            margin: 0;
            padding: 20px;
        }
        h1 { color: #ff4444; }
        .container { max-width: 800px; margin: 0 auto; }
        .controls {
            background: #333;
            padding: 20px;
            border-radius: 10px;
            margin: 20px 0;
        }
        .switch {
            position: relative;
            display: inline-block;
            width: 60px;
            height: 34px;
            margin: 0 10px;
        }
        .switch input { opacity: 0; width: 0; height: 0; }
        .slider {
            position: absolute;
            cursor: pointer;
            top: 0; left: 0; right: 0; bottom: 0;
            background-color: #ccc;
            transition: .4s;
            border-radius: 34px;
        }
        .slider:before {
            position: absolute;
            content: "";
            height: 26px; width: 26px;
            left: 4px; bottom: 4px;
            background-color: white;
            transition: .4s;
            border-radius: 50%;
        }
        input:checked + .slider { background-color: #ff4444; }
        input:checked + .slider:before { transform: translateX(26px); }
        
        .video-container {
            border: 3px solid #ff4444;
            border-radius: 10px;
            display: inline-block;
            position: relative;
            margin: 20px 0;
        }
        #stream {
            max-width: 100%;
            height: auto;
            display: block;
        }
        .status {
            background: #444;
            padding: 15px;
            border-radius: 8px;
            margin: 20px 0;
            text-align: left;
        }
        .motion-alert {
            position: absolute;
            top: 10px;
            left: 10px;
            background: rgba(255, 68, 68, 0.9);
            color: white;
            padding: 10px 20px;
            border-radius: 5px;
            font-weight: bold;
            display: none;
            animation: blink 1s infinite;
        }
        @keyframes blink {
            0%, 50% { opacity: 1; }
            51%, 100% { opacity: 0.5; }
        }
        .stats {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 20px;
            margin: 20px 0;
        }
        .stat-box {
            background: #333;
            padding: 15px;
            border-radius: 8px;
        }
        .stat-value {
            font-size: 2em;
            color: #ff4444;
            font-weight: bold;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>🎥 ESP32-CAM Motion Detection</h1>
        
        <div class="controls">
            <label><strong>Motion Detection:</strong></label>
            <label class="switch">
                <input type="checkbox" id="motionToggle" checked>
                <span class="slider"></span>
            </label>
            <span id="toggleStatus">ON</span>
        </div>
        
        <div class="video-container">
            <img id="stream" src="/stream">
            <div id="motionAlert" class="motion-alert">🚨 MOTION DETECTED!</div>
        </div>
        
        <div class="stats">
            <div class="stat-box">
                <div>Total Detections</div>
                <div class="stat-value" id="motionCount">0</div>
            </div>
            <div class="stat-box">
                <div>Last Detection</div>
                <div class="stat-value" id="lastMotion">Never</div>
            </div>
        </div>
        
        <div class="status">
            <h3>📊 System Status</h3>
            <p><strong>Detection Status:</strong> <span id="detectionStatus">Active</span></p>
            <p><strong>Stream Status:</strong> <span id="streamStatus">Loading...</span></p>
            <p><strong>Instructions:</strong> Move in front of the camera to trigger motion detection</p>
        </div>
    </div>

    <script>
        const motionToggle = document.getElementById('motionToggle');
        const toggleStatus = document.getElementById('toggleStatus');
        const motionAlert = document.getElementById('motionAlert');
        const streamImg = document.getElementById('stream');
        
        let lastMotionCount = 0;
        
        // Handle stream load
        streamImg.onload = function() {
            document.getElementById('streamStatus').textContent = 'Active';
        };
        
        streamImg.onerror = function() {
            document.getElementById('streamStatus').textContent = 'Error - Retrying...';
            setTimeout(() => {
                streamImg.src = '/stream?' + Date.now();
            }, 3000);
        };
        
        // Handle motion detection toggle
        motionToggle.addEventListener('change', function() {
            const enabled = this.checked;
            toggleStatus.textContent = enabled ? 'ON' : 'OFF';
            
            fetch(`/control?var=motion_detect&val=${enabled ? 1 : 0}`)
            .then(response => response.text())
            .then(() => {
                document.getElementById('detectionStatus').textContent = enabled ? 'Active' : 'Disabled';
            });
        });
        
        // Update status every 2 seconds
        function updateStatus() {
            fetch('/status')
            .then(response => response.json())
            .then(data => {
                document.getElementById('motionCount').textContent = data.motion_count;
                
                // Check for new motion detection
                if (data.motion_count > lastMotionCount) {
                    lastMotionCount = data.motion_count;
                    showMotionAlert();
                    document.getElementById('lastMotion').textContent = 'Just now';
                }
                
                // Update last detection time
                if (data.last_motion > 0) {
                    const secondsAgo = Math.floor((Date.now() - data.last_motion) / 1000);
                    if (secondsAgo > 60) {
                        document.getElementById('lastMotion').textContent = Math.floor(secondsAgo / 60) + 'm ago';
                    } else if (secondsAgo > 5) {
                        document.getElementById('lastMotion').textContent = secondsAgo + 's ago';
                    }
                }
            })
            .catch(error => console.error('Status update failed:', error));
        }
        
        function showMotionAlert() {
            motionAlert.style.display = 'block';
            setTimeout(() => {
                motionAlert.style.display = 'none';
            }, 3000);
        }
        
        // Start status updates
        setInterval(updateStatus, 2000);
        updateStatus();
    </script>
</body>
</html>
)rawliteral";

// Real motion detection function
bool detectRealMotion(camera_fb_t *fb) {
    if (!fb || fb->len == 0) return false;
    
    // Initialize previous frame on first run
    if (prev_frame == NULL) {
        prev_frame = (uint8_t*)ps_malloc(fb->len);
        if (prev_frame == NULL) {
            Serial.println("Failed to allocate memory for motion detection");
            return false;
        }
        memcpy(prev_frame, fb->buf, fb->len);
        prev_frame_size = fb->len;
        return false; // No motion on first frame
    }
    
    // Check if frame size changed
    if (prev_frame_size != fb->len) {
        free(prev_frame);
        prev_frame = (uint8_t*)ps_malloc(fb->len);
        if (prev_frame == NULL) return false;
        memcpy(prev_frame, fb->buf, fb->len);
        prev_frame_size = fb->len;
        return false;
    }
    
    // Calculate motion by comparing frames
    uint32_t motion_pixels = 0;
    uint32_t total_pixels = 0;
    const uint32_t step = 50; // Sample every 50th pixel for efficiency
    const uint8_t threshold = 15; // Motion sensitivity threshold
    
    for (uint32_t i = 0; i < fb->len && i < prev_frame_size; i += step) {
        uint8_t current = fb->buf[i];
        uint8_t previous = prev_frame[i];
        
        if (abs((int)current - (int)previous) > threshold) {
            motion_pixels++;
        }
        total_pixels++;
    }
    
    // Update previous frame
    memcpy(prev_frame, fb->buf, fb->len);
    
    // Calculate motion percentage
    float motion_percentage = (float)motion_pixels / total_pixels * 100.0;
    
    // Motion detected if more than 5% of pixels changed
    if (motion_percentage > 5.0) {
        motion_count++;
        last_motion_time = millis();
        
        // Flash LED to indicate motion
        digitalWrite(LED_PIN, HIGH);
        
        Serial.printf("🚨 MOTION DETECTED! Pixels changed: %.1f%% (%d/%d)\n", 
                     motion_percentage, motion_pixels, total_pixels);
        
        // Turn off LED after 200ms
        static unsigned long led_off_time = 0;
        led_off_time = millis() + 200;
        
        return true;
    }
    
    // Turn off LED if enough time passed
    static unsigned long last_led_check = 0;
    if (millis() - last_led_check > 100) {
        if (millis() - last_motion_time > 200) {
            digitalWrite(LED_PIN, LOW);
        }
        last_led_check = millis();
    }
    
    return false;
}

// Stream handler with motion detection
static esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    char * part_buf[64];
    
    static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=123456789000000000000987654321";
    static const char* _STREAM_BOUNDARY = "\r\n--123456789000000000000987654321\r\n";
    static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";
    
    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;
    
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    Serial.println("📹 Stream started");
    
    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("❌ Camera capture failed");
            res = ESP_FAIL;
            break;
        }
        
        // Perform motion detection
        if (motion_detection_enabled) {
            detectRealMotion(fb);
        }
        
        // Send stream boundary
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        
        // Send image headers
        if (res == ESP_OK) {
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, fb->len);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        
        // Send image data
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }
        
        esp_camera_fb_return(fb);
        
        if (res != ESP_OK) {
            break;
        }
    }
    
    Serial.println("📹 Stream ended");
    return res;
}

// Control handler
static esp_err_t cmd_handler(httpd_req_t *req) {
    char*  buf;
    size_t buf_len;
    char variable[32] = {0,};
    char value[32] = {0,};

    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = (char*)malloc(buf_len);
        if(!buf) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) == ESP_OK &&
                httpd_query_key_value(buf, "val", value, sizeof(value)) == ESP_OK) {
            } else {
                free(buf);
                httpd_resp_send_404(req);
                return ESP_FAIL;
            }
        } else {
            free(buf);
            httpd_resp_send_404(req);
            return ESP_FAIL;
        }
        free(buf);
    } else {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    int val = atoi(value);
    
    if(!strcmp(variable, "motion_detect")) {
        motion_detection_enabled = val;
        Serial.printf("Motion detection %s\n", val ? "ENABLED" : "DISABLED");
        
        if (!val) {
            digitalWrite(LED_PIN, LOW); // Turn off LED when disabled
        }
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

// Status handler
static esp_err_t status_handler(httpd_req_t *req) {
    static char json_response[200];
    
    snprintf(json_response, sizeof(json_response),
        "{\"motion_detect\":%s,\"motion_count\":%d,\"last_motion\":%lu}",
        motion_detection_enabled ? "true" : "false",
        motion_count,
        last_motion_time
    );
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json_response, strlen(json_response));
}

// Index page handler
static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, (const char *)INDEX_HTML, strlen(INDEX_HTML));
}

// Start web server
void startCameraServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 16384;
    
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL
    };
    
    httpd_uri_t cmd_uri = {
        .uri = "/control",
        .method = HTTP_GET,
        .handler = cmd_handler,
        .user_ctx = NULL
    };
    
    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL
    };
    
    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL
    };
    
    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(camera_httpd, &index_uri);
        httpd_register_uri_handler(camera_httpd, &cmd_uri);
        httpd_register_uri_handler(camera_httpd, &status_uri);
        httpd_register_uri_handler(camera_httpd, &stream_uri);
        Serial.println("✅ Web server started successfully");
    } else {
        Serial.println("❌ Failed to start web server");
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println();
    Serial.println("🚀 ESP32-CAM Real Motion Detection System");
    Serial.println("==========================================");
    
    // Initialize LED pin
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    
    // Test LED
    Serial.println("Testing LED...");
    for (int i = 0; i < 3; i++) {
        digitalWrite(LED_PIN, HIGH);
        delay(200);
        digitalWrite(LED_PIN, LOW);
        delay(200);
    }
    
    // Camera configuration
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
    
    // Frame size based on PSRAM
    if(psramFound()){
        config.frame_size = FRAMESIZE_VGA;
        config.jpeg_quality = 10;
        config.fb_count = 2;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
        Serial.println("✅ PSRAM found - Using VGA resolution");
    } else {
        config.frame_size = FRAMESIZE_QVGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
        config.grab_mode = CAMERA_GRAB_LATEST;
        Serial.println("⚠️  No PSRAM - Using QVGA resolution");
    }
    
    // Initialize camera
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("❌ Camera init failed with error 0x%x\n", err);
        return;
    }
    
    Serial.println("✅ Camera initialized successfully!");
    
    // Configure camera sensor
    sensor_t * s = esp_camera_sensor_get();
    if (s != NULL) {
        // Optimize for motion detection
        s->set_framesize(s, FRAMESIZE_VGA);
        s->set_quality(s, 12);
        s->set_brightness(s, 1);
        s->set_contrast(s, 0);
        s->set_saturation(s, 0);
        s->set_gainceiling(s, (gainceiling_t)0);
        s->set_colorbar(s, 0);
        s->set_whitebal(s, 1);        // Fixed AWB function
        s->set_gain_ctrl(s, 1);       // Fixed AGC function  
        s->set_exposure_ctrl(s, 1);   // Fixed AEC function
        s->set_hmirror(s, 0);
        s->set_vflip(s, 0);
        s->set_awb_gain(s, 1);
        s->set_agc_gain(s, 0);
        s->set_aec_value(s, 300);
        s->set_special_effect(s, 0);
        s->set_wb_mode(s, 0);
        s->set_ae_level(s, 0);
        s->set_dcw(s, 1);
        s->set_bpc(s, 0);
        s->set_wpc(s, 1);
        s->set_raw_gma(s, 1);
        s->set_lenc(s, 1);
        
        Serial.println("✅ Camera sensor configured for motion detection");
    }
    
    // Connect to WiFi
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.println("✅ WiFi connected!");
    
    // Start web server
    startCameraServer();
    
    Serial.println("==========================================");
    Serial.print("🌐 Open your browser and go to: http://");
    Serial.println(WiFi.localIP());
    Serial.println("==========================================");
    Serial.println("✨ Features:");
    Serial.println("• Real-time motion detection");
    Serial.println("• LED flash on motion");
    Serial.println("• Live video streaming");
    Serial.println("• Web interface controls");
    Serial.println("• Motion statistics");
    Serial.println("==========================================");
    Serial.println("🎯 READY! Move in front of camera to test!");
}

void loop() {
    static unsigned long lastReport = 0;
    
    // Report status every 30 seconds
    if (millis() - lastReport > 30000) {
        lastReport = millis();
        Serial.printf("📊 Status: Detection %s | Total motions: %d | Free heap: %d bytes | Uptime: %lu minutes\n",
                     motion_detection_enabled ? "ON" : "OFF",
                     motion_count,
                     ESP.getFreeHeap(),
                     millis() / 60000);
    }
    
    delay(100);
}