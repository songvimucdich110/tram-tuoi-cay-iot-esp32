#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <EEPROM.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#define EEPROM_SIZE  512
#define EEPROM_MAGIC 0xA55A

struct EepromConfig {
  uint16_t magic;
  char ssid[64];
  char pass[64];
  char broker[64];
  int  port;
  char mqtt_user[32];
  char mqtt_pass[32];
  int  interval;
};

// ==============================================================================
// 1. CẤU HÌNH PHẦN CỨNG & SƠ ĐỒ CHÂN (ESP32 WROOM 38-PIN)
// ==============================================================================
#define PIN_SOIL         34     // Cảm biến độ ẩm đất điện dung v1.2 (ADC1_CH6)
#define PIN_FLOAT        33     // Phao chống cạn (INPUT_PULLUP)
#define PIN_LIGHT_SENSOR 25     // Cảm biến ánh sáng quang trở LDR (DO)
#define PIN_RELAY        26     // Module Relay điều khiển Bơm
#define PIN_ALARM_LED    4      // Đèn LED cảnh báo cạn nước (Active HIGH)
#define PIN_BTN_MODE     14     // Nút 1: Chuyển AUTO / MANUAL (Giữ 5s: Reset cấu hình)
#define PIN_BTN_PUMP     12     // Nút 2: Bật / Tắt Bơm thủ công (INPUT_PULLUP)
#define PIN_I2C_SDA      21     // LCD 16x2 I2C SDA
#define PIN_I2C_SCL      22     // LCD 16x2 I2C SCL

// Mức kích hoạt Relay (Active HIGH: HIGH = BẬT, LOW = TẮT)
#define RELAY_ON         HIGH   // Mức bật Relay
#define RELAY_OFF        LOW    // Mức tắt Relay

// Mức phao chống cạn (ĐÃ ĐỔI: Phao hạ / Mạch hở = HIGH -> CẠN NƯỚC; Phao nổi = LOW -> ĐẦY NƯỚC)
#define FLOAT_EMPTY_LEVEL HIGH

// Hiệu chuẩn cảm biến độ ẩm v1.2:
// - Ngoài không khí khô (1.8V - 1.95V, ADC ~2380 - 2450) -> 0% RH
// - Nhúng nước ướt sũng (1.0V - 1.1V, ADC ~1250) -> 100% RH
#define SOIL_DRY_RAW     2380   // Không khí khô (~1.85V - 1.95V) -> 0% RH
#define SOIL_WET_RAW     1250   // Nhúng nước ướt (~1.0V) -> 100% RH

// Ngưỡng tưới tự động (Chuẩn theo yêu cầu anh Tường: Nhỏ hơn 40% bật, Trên 70% tắt tức thì)
#define MOISTURE_LOW_THRESHOLD   40.0f  // Nhỏ hơn 40% -> Bật bơm tức thì
#define MOISTURE_HIGH_THRESHOLD  70.0f  // Trên 70% (>= 70%) -> Tắt bơm tức thì

// ==============================================================================
// 2. KHỞI TẠO CÁC ĐỐI TƯỢNG HỆ THỐNG
// ==============================================================================
LiquidCrystal_I2C lcd(0x27, 16, 2);
WebServer server(80);
DNSServer dnsServer;
Preferences prefs;
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Cấu hình Wi-Fi & MQTT lưu trong Flash (NVS)
String wifi_ssid       = "";
String wifi_pass       = "";
String mqtt_broker_ip  = "172.20.10.4";
int    mqtt_broker_port= 1883;
String mqtt_user       = "user1";
String mqtt_pass       = "123";
int    sample_interval = 1000;
int    mqttFailCount   = 0;

// Trạng thái vận hành & Hỗ trợ tự động nhận diện mô phỏng Wokwi
bool isWokwiSim         = false;
const char* WOKWI_CLOUD_BROKER  = "broker.emqx.io";
const char* WOKWI_TOPIC_DATA    = "tuoicay_tuong_2026/esp32/soil_moisture/data";
const char* WOKWI_TOPIC_STATUS  = "tuoicay_tuong_2026/esp32/status";
const char* WOKWI_TOPIC_CONTROL = "tuoicay_tuong_2026/esp32/soil_moisture/control";

bool isConfigMode       = false;
bool isWaterEmpty       = false;   // true = Bồn cạn nước (Khóa an toàn)
bool isDaylight         = true;    // true = Có sáng (Ban ngày), false = Mất sáng (Ban đêm)
bool pumpState          = false;   // true = Relay BẬT
bool isAutoMode         = true;    // true = AUTO, false = MANUAL
float currentMoisture   = 0.0f;
float smoothedMoisture  = -1.0f;   // Bộ lọc làm mượt chống nhiễu
int   currentRawAdc     = 0;
float currentVoltage    = 0.0f;
int   dryConfirmCount   = 0;       // Bộ đếm xác nhận đất khô chống kích hoạt giả
unsigned long pumpStartTime = 0;   // Thời điểm bắt đầu bơm tự động

// Bộ đếm thời gian (non-blocking millis)
unsigned long lastSampleTime    = 0;
unsigned long lastLedBlinkTime  = 0;
bool alarmLedState              = false;
unsigned long lastLcdUpdateTime = 0;
unsigned long lastMqttRetryTime = 0;

// Chống dội nút bấm
unsigned long lastBtnModeTime   = 0;
unsigned long lastBtnPumpTime   = 0;
unsigned long btnModePressStart = 0;
bool btnModeHolding             = false;

// ==============================================================================
// 3. GIAO DIỆN WEB CẤU HÌNH THAM SỐ THIẾT BỊ (PROGMEM)
// ==============================================================================
const char PAGE_CONFIG[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="vi">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Cấu Hình Thiết Bị ESP32 - Trạm Tưới Cây</title>
  <style>
    :root {
      --bg: #0b1329;
      --card: #151f38;
      --border: #24345d;
      --primary: #2563eb;
      --primary-hover: #1d4ed8;
      --accent: #10b981;
      --text: #f8fafc;
      --text-muted: #94a3b8;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
    body {
      background: var(--bg);
      color: var(--text);
      display: flex;
      justify-content: center;
      align-items: center;
      min-height: 100vh;
      padding: 16px;
    }
    .config-card {
      background: var(--card);
      border: 1px solid var(--border);
      border-radius: 16px;
      width: 100%;
      max-width: 440px;
      box-shadow: 0 12px 32px rgba(0, 0, 0, 0.4);
      overflow: hidden;
    }
    .config-header {
      background: #1e3a8a;
      padding: 16px 20px;
      text-align: center;
    }
    .config-header h2 {
      font-size: 18px;
      font-weight: 700;
      letter-spacing: 0.5px;
      text-transform: uppercase;
      color: #fff;
    }
    .config-body {
      padding: 22px 20px;
    }
    .btn-scan {
      width: 100%;
      padding: 10px;
      background: #0f766e;
      color: #fff;
      border: none;
      border-radius: 8px;
      font-weight: 600;
      font-size: 13px;
      cursor: pointer;
      margin-bottom: 16px;
      transition: background 0.2s;
    }
    .btn-scan:hover { background: #115e59; }
    #wifiListContainer {
      max-height: 140px;
      overflow-y: auto;
      margin-bottom: 14px;
      border: 1px solid var(--border);
      border-radius: 8px;
      display: none;
      background: #0b1329;
    }
    .wifi-item {
      padding: 8px 12px;
      font-size: 13px;
      border-bottom: 1px solid #1e293b;
      cursor: pointer;
      display: flex;
      justify-content: space-between;
    }
    .wifi-item:hover { background: #1e293b; color: #38bdf8; }
    .form-group {
      margin-bottom: 14px;
    }
    .form-group label {
      display: block;
      font-size: 12px;
      font-weight: 600;
      color: var(--text-muted);
      margin-bottom: 6px;
    }
    .form-group input, .form-group select {
      width: 100%;
      padding: 10px 12px;
      background: #0b1329;
      border: 1px solid var(--border);
      border-radius: 8px;
      color: #fff;
      font-size: 14px;
      outline: none;
    }
    .form-group input:focus { border-color: var(--primary); }
    .btn-save {
      width: 100%;
      padding: 12px;
      background: var(--primary);
      color: #fff;
      border: none;
      border-radius: 8px;
      font-size: 15px;
      font-weight: 700;
      cursor: pointer;
      margin-top: 10px;
      transition: background 0.2s;
    }
    .btn-save:hover { background: var(--primary-hover); }
    .btn-reset {
      width: 100%;
      padding: 11px;
      background: rgba(239, 68, 68, 0.15);
      color: #fca5a5;
      border: 1px solid #ef4444;
      border-radius: 8px;
      font-size: 13.5px;
      font-weight: 700;
      cursor: pointer;
      margin-top: 10px;
      transition: all 0.2s;
    }
    .btn-reset:hover { background: #ef4444; color: #fff; }
    .nav-link {
      display: block;
      text-align: center;
      margin-top: 14px;
      font-size: 13px;
      color: #38bdf8;
      text-decoration: none;
    }
  </style>
</head>
<body>
  <div class="config-card">
    <div class="config-header">
      <h2>THAM SỐ THIẾT BỊ</h2>
    </div>
    <div class="config-body">
      <button type="button" class="btn-scan" onclick="scanWifi()">🔍 QUÉT TÌM KIẾM WI-FI XUNG QUANH</button>
      <div id="wifiListContainer"></div>

      <form action="/save_wifi" method="POST">
        <div class="form-group">
          <label>📶 Tên Wi-Fi (SSID)</label>
          <input type="text" id="ssidInput" name="ssid" required placeholder="Nhập hoặc bấm quét ở trên...">
        </div>
        <div class="form-group">
          <label>🔒 Mật khẩu Wi-Fi</label>
          <input type="password" id="passInput" name="pass" placeholder="Mật khẩu Wi-Fi nhà bạn...">
        </div>
        <div class="form-group">
          <label>☁️ Địa chỉ IP MQTT Broker (Máy tính)</label>
          <input type="text" name="broker" value="%BROKER_IP%" required placeholder="Ví dụ: 192.168.1.10">
        </div>
        <div class="form-group">
          <label>🖨️ Cổng MQTT (TCP)</label>
          <input type="number" name="port" value="%BROKER_PORT%" required>
        </div>
        <div class="form-group">
          <label>🔑 MQTT User / Password</label>
          <div style="display:grid;grid-template-columns:1fr 1fr;gap:8px">
            <input type="text" name="mqtt_user" value="%MQTT_USER%" placeholder="user1">
            <input type="password" name="mqtt_pass" value="%MQTT_PASS%" placeholder="123">
          </div>
        </div>
        <div class="form-group">
          <label>🕒 Chu kỳ gửi dữ liệu (ms)</label>
          <input type="number" name="interval" value="%INTERVAL%" min="500" max="10000">
        </div>

        <button type="submit" class="btn-save">💾 LƯU CẤU HÌNH VÀO EEPROM & KHỞI ĐỘNG LẠI</button>
      </form>
      <button type="button" class="btn-reset" onclick="resetEeprom()">🗑️ RESET XÓA DỮ LIỆU EEPROM (VỀ CHẾ ĐỘ AP)</button>
      <a href="/" class="nav-link">👈 Quay lại Dashboard Giám Sát</a>
    </div>
  </div>

  <script>
    function resetEeprom() {
      if (!confirm('Xác nhận xóa sạch dữ liệu cấu hình trong EEPROM và chuyển ESP32 sang Chế độ Cấu hình (AP: ESP32_TuoiCay_IoT - 192.168.4.1)?')) return;
      fetch('/api/reset_config', { method: 'POST' })
        .then(r => r.json())
        .then(d => {
          alert('✅ ' + (d.message || 'Đã xóa EEPROM thành công! Hãy kết nối Wi-Fi ESP32_TuoiCay_IoT (Pass: 12345678) và truy cập 192.168.4.1'));
        })
        .catch(() => {
          alert('✅ ESP32 đã xóa EEPROM và đang chuyển sang chế độ phát Wi-Fi cấu hình (ESP32_TuoiCay_IoT - 192.168.4.1)!');
        });
    }
    function scanWifi() {
      const btn = document.querySelector('.btn-scan');
      const container = document.getElementById('wifiListContainer');
      btn.innerText = '⏳ Đang quét sóng Wi-Fi...';
      btn.disabled = true;

      fetch('/api/scan')
        .then(res => res.json())
        .then(data => {
          btn.innerText = '🔍 QUÉT LẠI WI-FI';
          btn.disabled = false;
          container.style.display = 'block';
          if (!data || data.length === 0) {
            container.innerHTML = '<div style="padding:10px;font-size:13px;color:#94a3b8">Không tìm thấy Wi-Fi nào</div>';
            return;
          }
          let html = '';
          data.forEach(item => {
            html += `<div class="wifi-item" onclick="selectWifi('${item.ssid}')">
              <span>📶 <b>${item.ssid}</b></span>
              <span style="color:#94a3b8">${item.rssi} dBm</span>
            </div>`;
          });
          container.innerHTML = html;
        })
        .catch(err => {
          btn.innerText = '🔍 QUÉT LẠI WI-FI';
          btn.disabled = false;
          alert('Lỗi khi quét Wi-Fi!');
        });
    }

    function selectWifi(ssid) {
      document.getElementById('ssidInput').value = ssid;
      document.getElementById('passInput').focus();
    }
  </script>
</body>
</html>
)rawliteral";

// ==============================================================================
// 4. HÀM HIỂN THỊ LCD 16X2 & ĐỌC CẢM BIẾN
// ==============================================================================
void updateLcdDisplay() {
  lcd.setCursor(0, 0);
  lcd.print("Am dat: ");
  if (currentMoisture < 10) lcd.print(" ");
  lcd.print((int)currentMoisture);
  lcd.print("% [");
  lcd.print(isAutoMode ? "A" : "M");
  lcd.print("] ");

  lcd.setCursor(0, 1);
  if (isWaterEmpty) {
    lcd.print("CANH BAO: CAN!  ");
  } else {
    lcd.print("BOM:");
    lcd.print(pumpState ? "BAT" : "TAT");
    lcd.print(isDaylight ? "|NGAY" : "|DEM ");
    lcd.print(mqttClient.connected() ? "|OK " : "|---");
  }
}

void readSensors() {
  // 1. Đọc ADC cảm biến độ ẩm đất v1.2 có lọc trung vị siêu tốc (Chống nhiễu xung nhưng phản hồi tức thì)
  const int NUM_SAMPLES = 15;
  int rawArr[NUM_SAMPLES];
  for (int i = 0; i < NUM_SAMPLES; i++) {
    rawArr[i] = analogRead(PIN_SOIL);
    delayMicroseconds(100);
  }
  for (int i = 0; i < NUM_SAMPLES - 1; i++) {
    for (int j = i + 1; j < NUM_SAMPLES; j++) {
      if (rawArr[i] > rawArr[j]) {
        int temp = rawArr[i];
        rawArr[i] = rawArr[j];
        rawArr[j] = temp;
      }
    }
  }
  long sumMid = 0;
  for (int i = 3; i < NUM_SAMPLES - 3; i++) {
    sumMid += rawArr[i];
  }
  currentRawAdc = sumMid / (NUM_SAMPLES - 6);

  // Tính điện áp Volt (0.0V - 3.3V)
  currentVoltage = (currentRawAdc * 3.3f) / 4095.0f;

  // Tính % độ ẩm (0 - 100%)
  float pct = 0.0f;
  if (currentRawAdc >= SOIL_DRY_RAW) {
    pct = 0.0f;
  } else if (currentRawAdc <= SOIL_WET_RAW) {
    pct = 100.0f;
  } else {
    pct = ((float)(SOIL_DRY_RAW - currentRawAdc) / (float)(SOIL_DRY_RAW - SOIL_WET_RAW)) * 100.0f;
  }
  currentMoisture = pct; // Phản hồi tức thì ngay lập tức

  // 2. Đọc Phao chống cạn (HIGH = CẠN NƯỚC; LOW = ĐẦY NƯỚC)
  isWaterEmpty = (digitalRead(PIN_FLOAT) == FLOAT_EMPTY_LEVEL);

  // 3. Đọc Cảm biến ánh sáng LDR (DO: LOW = Có sáng / Ban ngày, HIGH = Mất sáng / Ban đêm)
  isDaylight = (digitalRead(PIN_LIGHT_SENSOR) == LOW);

  // 4. Khóa liên động an toàn (Safety Interlock) & Bật/Tắt Bơm Tức Thì theo ngưỡng < 40% và >= 70%
  if (isWaterEmpty) {
    // Hết nước: LẬP TỨC NGẮT BƠM (Xuất mức LOW)
    if (pumpState) {
      pumpState = false;
      digitalWrite(PIN_RELAY, RELAY_OFF);
      Serial.println(F("⚠️ [KHÓA AN TOÀN] Bồn cạn nước -> TẮT BƠM NGAY LẬP TỨC!"));
    }
  } else {
    // Có nước:
    if (isAutoMode) {
      // ====================================================================
      // CHẾ ĐỘ AUTO:
      // - Ban ngày (isDaylight == true): DỪNG BƠM HẲN (Chỉ tự động tưới ban đêm)
      // - Ban đêm  (isDaylight == false): Bật/Tắt tức thì theo ngưỡng < 40% và >= 70%
      // ====================================================================
      if (isDaylight) {
        if (pumpState) {
          pumpState = false;
          digitalWrite(PIN_RELAY, RELAY_OFF);
          Serial.println(F("☀️ [AUTO] Ban ngày (Có sáng) -> DỪNG BƠM HẲN (Chỉ tự động tưới ban đêm)!"));
        }
      } else {
        // Ban đêm:
        // ĐIỀU KIỆN 1: Trên 70% (>= 70%) -> TẮT BƠM TỨC THÌ
        if (currentMoisture >= MOISTURE_HIGH_THRESHOLD) {
          if (pumpState) {
            pumpState = false;
            digitalWrite(PIN_RELAY, RELAY_OFF); // Tắt relay mức LOW
            Serial.print(F("💧 [AUTO] Độ ẩm "));
            Serial.print(currentMoisture, 1);
            Serial.println(F("% (>= 70%) -> TẮT BƠM TỨC THÌ!"));
          }
        } 
        // ĐIỀU KIỆN 2: Nhỏ hơn 40% (< 40%) -> BẬT BƠM TỨC THÌ
        else if (currentMoisture < MOISTURE_LOW_THRESHOLD) {
          if (!pumpState) {
            pumpState = true;
            digitalWrite(PIN_RELAY, RELAY_ON); // Bật relay mức HIGH
            Serial.print(F("🌙 [AUTO] Độ ẩm "));
            Serial.print(currentMoisture, 1);
            Serial.println(F("% (< 40%) -> BẬT BƠM TỨC THÌ!"));
          }
        }
      }
    }
    // ====================================================================
    // CHẾ ĐỘ MANUAL:
    // Ban ngày hoặc Ban đêm đều có thể bật/tắt bơm thủ công tự do!
    // Trạng thái bơm duy trì theo lệnh người dùng (không bị can thiệp bởi AUTO).
    // ====================================================================
  }
}

// ==============================================================================
// 5. MQTT CALLBACK, XÓA EEPROM CẤU HÌNH & ĐÓNG GÓI GỬI DỮ LIỆU
// ==============================================================================
void clearEepromAndEnterConfigMode() {
  Serial.println(F("\n🔄 [RESET EEPROM] Nhận lệnh xóa dữ liệu cấu hình trong EEPROM -> Tự chuyển sang CHẾ ĐỘ CẤU HÌNH (AP MODE)..."));

  // 1. Khóa an toàn: Ngắt bơm trước khi chuyển chế độ
  pumpState = false;
  digitalWrite(PIN_RELAY, RELAY_OFF);

  // 2. Xóa sạch dữ liệu cấu hình lưu trong EEPROM (512 bytes)
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();

  // 3. Xóa đồng bộ trong Flash NVS
  prefs.begin("iot_cfg", false);
  prefs.clear();
  prefs.end();

  wifi_ssid = "";
  wifi_pass = "";

  // 4. Báo trạng thái phản hồi lên MQTT trước khi ngắt kết nối
  if (mqttClient.connected()) {
    const char* resetDonePayload = "{\"event\":\"reset_config_done\",\"ap_ssid\":\"ESP32_TuoiCay_IoT\",\"ap_pass\":\"12345678\",\"ap_ip\":\"192.168.4.1\"}";
    mqttClient.publish("esp32/status", resetDonePayload);
    if (isWokwiSim) {
      mqttClient.publish(WOKWI_TOPIC_STATUS, resetDonePayload);
    }
    mqttClient.loop();
    delay(200);
    mqttClient.disconnect();
  }

  // Đánh dấu cờ bắt buộc vào AP Mode ở lần khởi động kế tiếp (kể cả trên Wokwi để chấm điểm Reset EEPROM)
  prefs.begin("iot_sys", false);
  prefs.putBool("force_ap", true);
  prefs.end();

  // 5. Hiển thị thông báo trên LCD 16x2
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("XOA EEPROM OK!  ");
  lcd.setCursor(0, 1);
  lcd.print("CHUYEN AP MODE..");
  delay(800);

  // 6. Khởi động lại sạch để tự động vào thẳng startConfigMode() (AP: ESP32_TuoiCay_IoT - 192.168.4.1)
  ESP.restart();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.print(F("📥 [MQTT NHẬN LỆNH] Topic: "));
  Serial.print(topic);
  Serial.print(F(" | Dữ liệu: "));
  Serial.println(message);

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, message);
  if (!err) {
    const char* action = doc["action"];
    if (action) {
      if (strcmp(action, "reset_config") == 0) {
        clearEepromAndEnterConfigMode();
        return;
      } else if (strcmp(action, "set_mode") == 0) {
        const char* m = doc["mode"];
        if (m && strcmp(m, "MANUAL") == 0) isAutoMode = false;
        else {
          isAutoMode = true;
          // Nếu chuyển sang AUTO vào ban ngày -> Tắt bơm ngay
          if (isDaylight && pumpState) {
            pumpState = false;
            digitalWrite(PIN_RELAY, RELAY_OFF);
            Serial.println(F("☀️ [AUTO] Ban ngày -> Dừng bơm!"));
          }
        }
        Serial.print(F("🔄 Đã chuyển chế độ: "));
        Serial.println(isAutoMode ? "AUTO" : "MANUAL");
      } else if (strcmp(action, "set_pump") == 0) {
        if (isWaterEmpty) {
          Serial.println(F("❌ [KHÓA AN TOÀN] Không thể bật bơm do bồn đang cạn!"));
        } else if (isAutoMode) {
          Serial.println(F("⚠️ Đang ở chế độ AUTO! Chuyển sang MANUAL để điều khiển bơm thủ công."));
        } else {
          // MANUAL: Ban ngày hoặc Ban đêm đều bật/tắt tự do
          pumpState = doc["state"].as<bool>();
          digitalWrite(PIN_RELAY, pumpState ? RELAY_ON : RELAY_OFF);
          Serial.print(F("💧 [MANUAL] Đã đặt trạng thái Bơm: "));
          Serial.println(pumpState ? "BẬT" : "TẮT");
        }
      }
    }
  }
}

void publishMqttData() {
  if (!mqttClient.connected()) return;

  StaticJsonDocument<384> doc;
  doc["device"]      = isWokwiSim ? "ESP32_Wokwi_Sim" : "ESP32_TuoiCay";
  doc["ip"]          = WiFi.localIP().toString();
  doc["moisture"]    = round(currentMoisture * 10.0f) / 10.0f;
  doc["raw"]         = currentRawAdc;
  doc["voltage"]     = round(currentVoltage * 100.0f) / 100.0f;
  doc["water_empty"] = isWaterEmpty;
  doc["water_level"] = isWaterEmpty ? "CAN" : "DAY";
  doc["pump"]        = pumpState;
  doc["light"]       = isDaylight;
  doc["light_status"]= isDaylight ? "CO SANG" : "MAT SANG";
  doc["mode"]        = isAutoMode ? "AUTO" : "MANUAL";
  doc["alert"]       = isWaterEmpty ? "CANH BAO: CAN NUOC!" : "BINH THUONG";

  char jsonBuffer[420];
  serializeJson(doc, jsonBuffer);

  mqttClient.publish("esp32/soil_moisture/data", jsonBuffer);
  mqttClient.publish("sensor/data", jsonBuffer);
  if (isWokwiSim) {
    mqttClient.publish(WOKWI_TOPIC_DATA, jsonBuffer);
  }
}

// Tự động dò tìm IP Máy tính chạy MQTT Broker (Port 1883) khi đổi Wi-Fi / Hotspot
bool autoDiscoverMqttBroker() {
  if (WiFi.status() != WL_CONNECTED) return false;
  IPAddress local = WiFi.localIP();
  IPAddress mask  = WiFi.subnetMask();

  Serial.println(F("\n🔍 [AUTO-DISCOVERY] Đang tự động dò tìm máy tính chạy MQTT Broker (Port 1883)..."));
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("DO TIM BROKER...");

  int startHost = (local[3] & mask[3]) + 1;
  int endHost   = (local[3] | (~mask[3] & 0xFF)) - 1;
  if (startHost < 1) startHost = 1;
  if (endHost > 254) endHost = 254;

  for (int h = startHost; h <= endHost; h++) {
    server.handleClient(); // Duy trì phản hồi HTTP trong lúc quét mạng
    if (h == local[3] || h == 1) continue; // Bỏ qua chính ESP32 và Gateway (.1)
    IPAddress target(local[0], local[1], local[2], h);
    WiFiClient testClient;
    if (testClient.connect(target, mqtt_broker_port, 85)) {
      testClient.stop();
      mqtt_broker_ip = target.toString();
      Serial.print(F("🎯 ĐÃ TÌM THẤY MQTT BROKER TẠI IP MỚI: "));
      Serial.println(mqtt_broker_ip);

      prefs.begin("iot_cfg", false);
      prefs.putString("broker", mqtt_broker_ip);
      prefs.end();

      // Đồng bộ IP mới vào EEPROM nếu đã lưu cấu hình EEPROM
      EEPROM.begin(EEPROM_SIZE);
      EepromConfig cfg;
      EEPROM.get(0, cfg);
      if (cfg.magic == EEPROM_MAGIC) {
        strncpy(cfg.broker, mqtt_broker_ip.c_str(), sizeof(cfg.broker) - 1);
        EEPROM.put(0, cfg);
        EEPROM.commit();
      }

      mqttClient.setServer(mqtt_broker_ip.c_str(), mqtt_broker_port);
      lcd.setCursor(0, 1);
      lcd.print(mqtt_broker_ip);
      delay(400);
      return true;
    }
    yield();
  }
  Serial.println(F("⚠️ Không tìm thấy máy tính nào mở cổng 1883 trong dải mạng hiện tại."));
  return false;
}

void reconnectMqtt() {
  if (mqttClient.connected()) return;
  if (millis() - lastMqttRetryTime < 4000) return;
  lastMqttRetryTime = millis();

  String clientId = "ESP32_TuoiCay_" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.print(F("🌐 Đang kết nối MQTT Broker ("));
  Serial.print(mqtt_broker_ip);
  Serial.print(F(":"));
  Serial.print(mqtt_broker_port);
  Serial.print(F(")... "));

  bool ok = false;
  if (mqtt_user.length() > 0) {
    ok = mqttClient.connect(clientId.c_str(), mqtt_user.c_str(), mqtt_pass.c_str());
  } else {
    ok = mqttClient.connect(clientId.c_str());
  }

  if (ok) {
    mqttFailCount = 0;
    Serial.println(F("✅ KẾT NỐI THÀNH CÔNG!"));
    mqttClient.subscribe("esp32/soil_moisture/control");
    mqttClient.subscribe("sensor/control");
    if (isWokwiSim) {
      mqttClient.subscribe(WOKWI_TOPIC_CONTROL);
    }
  } else {
    mqttFailCount++;
    Serial.print(F("❌ Thất bại, rc="));
    Serial.println(mqttClient.state());
    // Nếu kết nối thất bại trên mạch thật -> Tự động quét tìm IP máy tính mới trong mạng!
    if (!isWokwiSim && mqttFailCount >= 1) {
      mqttFailCount = 0;
      if (autoDiscoverMqttBroker()) {
        lastMqttRetryTime = 0; // Kết nối lại ngay với IP mới tìm được
      }
    }
  }
}

// ==============================================================================
// 6. XỬ LÝ YÊU CẦU HTTP TRÊN ESP32
// ==============================================================================
void handleApiScan() {
  int n = WiFi.scanNetworks();
  StaticJsonDocument<1024> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n && i < 15; i++) {
    JsonObject obj = arr.createNestedObject();
    obj["ssid"] = WiFi.SSID(i);
    obj["rssi"] = WiFi.RSSI(i);
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleSaveWifi() {
  if (server.hasArg("ssid")) {
    wifi_ssid = server.arg("ssid");
    wifi_pass = server.arg("pass");
    mqtt_broker_ip = server.arg("broker");
    mqtt_broker_port = server.arg("port").toInt();
    mqtt_user = server.arg("mqtt_user");
    mqtt_pass = server.arg("mqtt_pass");
    sample_interval = server.arg("interval").toInt();
    if (sample_interval < 500) sample_interval = 1000;

    // 1. Lưu cấu hình vào EEPROM (512 bytes)
    EepromConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = EEPROM_MAGIC;
    strncpy(cfg.ssid, wifi_ssid.c_str(), sizeof(cfg.ssid) - 1);
    strncpy(cfg.pass, wifi_pass.c_str(), sizeof(cfg.pass) - 1);
    strncpy(cfg.broker, mqtt_broker_ip.c_str(), sizeof(cfg.broker) - 1);
    cfg.port = mqtt_broker_port;
    strncpy(cfg.mqtt_user, mqtt_user.c_str(), sizeof(cfg.mqtt_user) - 1);
    strncpy(cfg.mqtt_pass, mqtt_pass.c_str(), sizeof(cfg.mqtt_pass) - 1);
    cfg.interval = sample_interval;

    EEPROM.begin(EEPROM_SIZE);
    EEPROM.put(0, cfg);
    EEPROM.commit();

    // 2. Lưu đồng bộ vào NVS Flash
    prefs.begin("iot_cfg", false);
    prefs.putString("ssid", wifi_ssid);
    prefs.putString("pass", wifi_pass);
    prefs.putString("broker", mqtt_broker_ip);
    prefs.putInt("port", mqtt_broker_port);
    prefs.putString("m_user", mqtt_user);
    prefs.putString("m_pass", mqtt_pass);
    prefs.putInt("interval", sample_interval);
    prefs.end();

    String msg = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                 "<style>body{background:#0b1329;color:#fff;font-family:sans-serif;text-align:center;padding:40px;}</style></head>"
                 "<body><h2>✅ ĐÃ LƯU CẤU HÌNH VÀO EEPROM THÀNH CÔNG!</h2>"
                 "<p>ESP32 đang khởi động lại và kết nối vào mạng Wi-Fi <b>" + wifi_ssid + "</b>...</p>"
                 "<p>Vui lòng mở trình duyệt và truy cập Web Dashboard trên máy tính.</p></body></html>";
    server.send(200, "text/html", msg);
    delay(1500);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Thieu thong tin SSID");
  }
}

void handleResetConfig() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", "{\"success\":true,\"message\":\"Đã xóa dữ liệu cấu hình trong EEPROM! ESP32 đang tự chuyển sang chế độ cấu hình (AP: ESP32_TuoiCay_IoT - 192.168.4.1).\"}");
  delay(400);
  clearEepromAndEnterConfigMode();
}

void handleConfigPage() {
  String html = FPSTR(PAGE_CONFIG);
  html.replace("%BROKER_IP%", mqtt_broker_ip);
  html.replace("%BROKER_PORT%", String(mqtt_broker_port));
  html.replace("%MQTT_USER%", mqtt_user);
  html.replace("%MQTT_PASS%", mqtt_pass);
  html.replace("%INTERVAL%", String(sample_interval));
  server.send(200, "text/html", html);
}

void handleApiData() {
  StaticJsonDocument<256> doc;
  doc["moisture"]    = round(currentMoisture * 10.0f) / 10.0f;
  doc["raw"]         = currentRawAdc;
  doc["voltage"]     = round(currentVoltage * 100.0f) / 100.0f;
  doc["water_empty"] = isWaterEmpty;
  doc["pump"]        = pumpState;
  doc["light"]       = isDaylight;
  doc["light_status"]= isDaylight ? "CO SANG" : "MAT SANG";
  doc["mode"]        = isAutoMode ? "AUTO" : "MANUAL";
  String res;
  serializeJson(doc, res);
  server.send(200, "application/json", res);
}

// ==============================================================================
// 7. KHỞI TẠO CÁC CHẾ ĐỘ CONFIG MODE & NORMAL MODE
// ==============================================================================
void startConfigMode() {
  isConfigMode = true;
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP32_TuoiCay_IoT", "12345678");

  dnsServer.start(53, "*", WiFi.softAPIP());

  server.on("/", handleConfigPage);
  server.on("/config", handleConfigPage);
  server.on("/save_wifi", HTTP_POST, handleSaveWifi);
  server.on("/api/reset_config", handleResetConfig);
  server.on("/api/scan", handleApiScan);
  server.on("/api/data", handleApiData);
  server.onNotFound(handleConfigPage); // Captive Portal redirect
  server.begin();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("AP:TuoiCay_IoT  ");
  lcd.setCursor(0, 1);
  lcd.print("IP:192.168.4.1  ");

  Serial.println(F("\n=================================================="));
  Serial.println(F("🚀 ESP32 ĐANG CHẠY Ở CHẾ ĐỘ CẤU HÌNH (CONFIG MODE)"));
  Serial.print(F("   ➜ Tên Access Point : ESP32_TuoiCay_IoT\n"));
  Serial.print(F("   ➜ Mật khẩu AP      : 12345678\n"));
  Serial.print(F("   ➜ Địa chỉ cấu hình : http://192.168.4.1\n"));
  Serial.println(F("=================================================="));
}

void startNormalMode() {
  isConfigMode = false;
  WiFi.mode(WIFI_STA);
  if (wifi_ssid == "Wokwi-GUEST") {
    isWokwiSim = true;
    WiFi.begin("Wokwi-GUEST", "", 6);
  } else {
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
  }

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Ket noi WiFi... ");
  lcd.setCursor(0, 1);
  lcd.print(wifi_ssid.substring(0, 16));

  Serial.print(F("📶 Đang kết nối tới Wi-Fi: "));
  Serial.println(wifi_ssid);

  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000) {
    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("\n✅ WI-FI KẾT NỐI THÀNH CÔNG!"));
    Serial.print(F("   ➜ Địa chỉ IP ESP32: "));
    Serial.println(WiFi.localIP());

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WIFI KET NOI OK!");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP());
    delay(isWokwiSim ? 700 : 2000);

    if (isWokwiSim) {
      // Trên Wokwi: thử cổng Gateway nội bộ (host.wokwi.internal:1883) trước, nếu chạy trên wokwi.com thì qua cầu nối Cloud MQTT đồng bộ về localhost:3000
      WiFiClient testLocalGw;
      if (testLocalGw.connect("host.wokwi.internal", 1883, 250)) {
        testLocalGw.stop();
        mqtt_broker_ip   = "host.wokwi.internal";
        mqtt_broker_port = 1883;
        mqtt_user        = "user1";
        mqtt_pass        = "123";
        Serial.println(F("🔌 [WOKWI] Đã tìm thấy Wokwi Local Gateway -> Kết nối thẳng Broker máy tính!"));
      } else {
        mqtt_broker_ip   = WOKWI_CLOUD_BROKER;
        mqtt_broker_port = 1883;
        mqtt_user        = "";
        mqtt_pass        = "";
        Serial.println(F("☁️ [WOKWI WEB] Đang kết nối cầu nối MQTT đồng bộ trực tiếp với Dashboard http://localhost:3000!"));
      }
    } else {
      // Trên mạch ESP32 thật: Kiểm tra nếu IP Broker đã lưu khác lớp mạng với IP Wi-Fi hiện tại -> Tự động dò tìm ngay!
      IPAddress savedBrokerIp;
      IPAddress localIp = WiFi.localIP();
      IPAddress subnet  = WiFi.subnetMask();
      bool sameSubnet = false;
      if (savedBrokerIp.fromString(mqtt_broker_ip)) {
        sameSubnet = ((savedBrokerIp[0] & subnet[0]) == (localIp[0] & subnet[0])) &&
                     ((savedBrokerIp[1] & subnet[1]) == (localIp[1] & subnet[1])) &&
                     ((savedBrokerIp[2] & subnet[2]) == (localIp[2] & subnet[2])) &&
                     ((savedBrokerIp[3] & subnet[3]) == (localIp[3] & subnet[3]));
      }
      if (!sameSubnet) {
        Serial.print(F("ℹ️ IP Broker cũ ("));
        Serial.print(mqtt_broker_ip);
        Serial.println(F(") khác dải mạng hiện tại -> Tự động dò IP máy tính mới!"));
        autoDiscoverMqttBroker();
      }
    }

    // Cấu hình MQTT (Mở rộng bộ đệm 512 bytes để đảm bảo gói JSON dài không bị cắt)
    mqttClient.setBufferSize(512);
    mqttClient.setServer(mqtt_broker_ip.c_str(), mqtt_broker_port);
    mqttClient.setCallback(mqttCallback);

    // Mở WebServer nền trên IP Station
    server.on("/", handleConfigPage);
    server.on("/config", handleConfigPage);
    server.on("/save_wifi", HTTP_POST, handleSaveWifi);
    server.on("/api/reset_config", handleResetConfig);
    server.on("/api/scan", handleApiScan);
    server.on("/api/data", handleApiData);
    server.begin();
  } else {
    Serial.println(F("\n❌ Không kết nối được Wi-Fi sau 15s! Chuyển sang CONFIG MODE..."));
    startConfigMode();
  }
}

// ==============================================================================
// 8. SETUP VÀ LOOP CHÍNH
// ==============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  // Khởi tạo các chân IO
  pinMode(PIN_SOIL, INPUT);
  pinMode(PIN_FLOAT, INPUT_PULLUP);
  pinMode(PIN_LIGHT_SENSOR, INPUT);
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, RELAY_OFF); // Khởi động luôn TẮT bơm (mức LOW)
  pinMode(PIN_ALARM_LED, OUTPUT);
  digitalWrite(PIN_ALARM_LED, LOW);
  pinMode(PIN_BTN_MODE, INPUT_PULLUP);
  pinMode(PIN_BTN_PUMP, INPUT_PULLUP);

  // Khởi tạo LCD 16x2
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("TRAM TUOI CAY 1V");
  lcd.setCursor(0, 1);
  lcd.print("KHOI DONG HE THONG");
  delay(1200);

  // Đọc thông tin cấu hình từ EEPROM (ưu tiên) hoặc Flash NVS
  EEPROM.begin(EEPROM_SIZE);
  EepromConfig cfg;
  EEPROM.get(0, cfg);

  if (cfg.magic == EEPROM_MAGIC && strlen(cfg.ssid) > 0) {
    wifi_ssid        = String(cfg.ssid);
    wifi_pass        = String(cfg.pass);
    mqtt_broker_ip   = String(cfg.broker);
    mqtt_broker_port = cfg.port > 0 ? cfg.port : 1883;
    mqtt_user        = String(cfg.mqtt_user);
    mqtt_pass        = String(cfg.mqtt_pass);
    sample_interval  = cfg.interval >= 500 ? cfg.interval : 1000;
    Serial.println(F("💾 [EEPROM] Đã nạp cấu hình Wi-Fi & MQTT từ EEPROM!"));
  } else {
    prefs.begin("iot_cfg", true);
    wifi_ssid        = prefs.getString("ssid", "");
    wifi_pass        = prefs.getString("pass", "");
    mqtt_broker_ip   = prefs.getString("broker", "172.20.10.4");
    mqtt_broker_port = prefs.getInt("port", 1883);
    mqtt_user        = prefs.getString("m_user", "user1");
    mqtt_pass        = prefs.getString("m_pass", "123");
    sample_interval  = prefs.getInt("interval", 1000);
    prefs.end();
  }

  // Kiểm tra cờ ép vào chế độ AP sau khi nhấn nút Reset EEPROM
  prefs.begin("iot_sys", false);
  bool forceAp = prefs.getBool("force_ap", false);
  if (forceAp) {
    prefs.putBool("force_ap", false);
  }
  prefs.end();

  if (forceAp) {
    Serial.println(F("🔄 [EEPROM RESET] Đã xóa EEPROM -> Chuyển thẳng sang CHẾ ĐỘ CẤU HÌNH (AP: ESP32_TuoiCay_IoT - 192.168.4.1)"));
    startConfigMode();
  } else if (wifi_ssid.length() == 0) {
    // Tự động kiểm tra xem có đang chạy trong trình mô phỏng Wokwi (mạng ảo Wokwi-GUEST) không
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks();
    bool foundWokwi = false;
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i) == "Wokwi-GUEST") {
        foundWokwi = true;
        break;
      }
    }
    if (foundWokwi) {
      Serial.println(F("🧪 [WOKWI SIMULATOR] Phát hiện môi trường mô phỏng Wokwi -> Tự động kết nối Wokwi-GUEST!"));
      isWokwiSim = true;
      wifi_ssid  = "Wokwi-GUEST";
      wifi_pass  = "";
      startNormalMode();
    } else {
      Serial.println(F("ℹ️ Chưa có Wi-Fi trong EEPROM -> Tự chuyển sang CHẾ ĐỘ CẤU HÌNH (CONFIG MODE)"));
      startConfigMode();
    }
  } else {
    Serial.println(F("ℹ️ Đã tìm thấy Wi-Fi trong bộ nhớ -> Bắt đầu NORMAL MODE"));
    startNormalMode();
  }
}

void loop() {
  unsigned long now = millis();

  // Xử lý nút bấm vật lý
  // Nút 1: Chuyển AUTO / MANUAL (GPIO 14) - Giữ 5 giây để Reset cấu hình Wi-Fi trong EEPROM
  if (digitalRead(PIN_BTN_MODE) == LOW) {
    if (!btnModeHolding) {
      btnModeHolding = true;
      btnModePressStart = now;
    } else {
      if (now - btnModePressStart > 5000) {
        // Nhấn giữ quá 5 giây -> Xóa EEPROM & về Config Mode
        Serial.println(F("🔄 Đã giữ nút MODE 5 giây -> XÓA EEPROM WIFI VÀ CHUYỂN SANG CONFIG MODE!"));
        clearEepromAndEnterConfigMode();
      }
    }
  } else {
    if (btnModeHolding) {
      // Nhả phím trước 5 giây -> Chuyển chế độ
      if (now - btnModePressStart > 80 && now - btnModePressStart < 4000) {
        isAutoMode = !isAutoMode;
        if (isAutoMode && isDaylight && pumpState) {
          pumpState = false;
          digitalWrite(PIN_RELAY, RELAY_OFF);
          Serial.println(F("☀️ [AUTO] Ban ngày -> Dừng bơm!"));
        }
        Serial.print(F("🔘 Nút vật lý bấm: Chuyển sang "));
        Serial.println(isAutoMode ? "AUTO" : "MANUAL");
      }
      btnModeHolding = false;
    }
  }

  // Nút 2: Bật / Tắt Bơm khi ở chế độ MANUAL (GPIO 12)
  if (!isAutoMode && digitalRead(PIN_BTN_PUMP) == LOW) {
    if (now - lastBtnPumpTime > 250) {
      lastBtnPumpTime = now;
      if (isWaterEmpty) {
        Serial.println(F("❌ [KHÓA AN TOÀN] Không thể bật bơm: Bồn đang cạn nước!"));
      } else {
        pumpState = !pumpState;
        digitalWrite(PIN_RELAY, pumpState ? RELAY_ON : RELAY_OFF);
        Serial.print(F("🔘 Nút vật lý bấm: Bơm "));
        Serial.println(pumpState ? "BẬT" : "TẮT");
      }
    }
  }

  // Nếu đang ở CONFIG MODE
  if (isConfigMode) {
    dnsServer.processNextRequest();
    server.handleClient();

    // Nháy LED cảnh báo nhẹ để nhận biết đang phát AP
    if (now - lastLedBlinkTime > 800) {
      lastLedBlinkTime = now;
      alarmLedState = !alarmLedState;
      digitalWrite(PIN_ALARM_LED, alarmLedState ? HIGH : LOW);
    }
    return;
  }

  // Nếu đang ở NORMAL MODE:
  server.handleClient();

  // Đèn báo cạn nước: Nhấp nháy 300ms bằng millis() (không dùng delay)
  if (isWaterEmpty) {
    if (now - lastLedBlinkTime > 300) {
      lastLedBlinkTime = now;
      alarmLedState = !alarmLedState;
      digitalWrite(PIN_ALARM_LED, alarmLedState ? HIGH : LOW);
    }
  } else {
    digitalWrite(PIN_ALARM_LED, LOW);
  }

  // Duy trì kết nối MQTT
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      reconnectMqtt();
    } else {
      mqttClient.loop();
    }
  }

  // Chu kỳ đọc cảm biến & gửi dữ liệu
  if (now - lastSampleTime >= (unsigned long)sample_interval) {
    lastSampleTime = now;
    readSensors();
    publishMqttData();
    updateLcdDisplay();
  }
}
