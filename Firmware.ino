/**
 * ============================================================================
 * DỰ ÁN: HỆ THỐNG GIÁM SÁT KHO HÀNG THÔNG MINH ĐA TẦNG (ESP32 INDUSTRIAL IOT)
 * Tác giả: Nhóm Kỹ Thuật IoT Kho Linh Kiện
 * Phiên bản Firmware: v2.1.0
 * 
 * ĐẶC TÍNH KỸ THUẬT:
 * - FreeRTOS Dual-Core Architecture (5 Tasks: Sensors, Network, Button, Display, Recovery)
 * - Quản lý Cấu hình & Bảo mật: Flash NVS (Preferences) & WiFiManager Custom Parameters
 * - Watchdog Timer (TWDT): Tự động phát hiện và phục hồi khi treo Task / Deadlock
 * - Cơ chế Edge-Buffering Đa Cấp: EEPROM 24C32 (I2C) + Flash LittleFS (SPI)
 * - Tự động bù dữ liệu (Disaster Recovery Backfill) khi mạng WiFi/MQTT phục hồi
 * - Điều khiển & Cấu hình ngưỡng động từ xa qua MQTT (Dynamic Remote Thresholds)
 * - Nâng cấp Firmware từ xa an toàn (Safe FOTA with MD5 Checksum Verification)
 * - Báo cáo sức khỏe hệ thống (System Diagnostics: Free Heap, Min Heap, RSSI, Uptime)
 * - Khả năng chống kẹt Bus I2C (I2C Bus Recovery / Clock Pumping)
 * ============================================================================
 */

#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <RTClib.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <HTTPUpdate.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_task_wdt.h>

// ==========================================
// 1. ĐỊNH NGHĨA PHẦN CỨNG & THÔNG SỐ HỆ THỐNG
// ==========================================
#define FIRMWARE_VERSION    "v2.1.0"
#define WDT_TIMEOUT_SECONDS 20        // Watchdog Timeout (20 giây)

// Chân GPIO Phần cứng
#define I2C_SDA_PIN         21
#define I2C_SCL_PIN         22
#define BUZZER_PIN          18
#define BTN_MUTE_PIN        19
#define BTN_RESET_PIN       23

// Địa chỉ & Dung lượng bộ nhớ I2C
#define EEPROM_I2C_ADDR     0x57
#define EEPROM_SIZE         4096      // 4KB (24C32)
#define LOG_FILENAME        "/offline.dat"

// Giá trị mặc định (Fallback nếu chưa cấu hình qua NVS/WiFiManager)
#define DEFAULT_MQTT_SERVER "c7b38d664d464ca79fc4feb942741794.s1.eu.hivemq.cloud"
#define DEFAULT_MQTT_PORT   8883
#define DEFAULT_MQTT_USER   "IOT_NHIETDO_DOAM"
#define DEFAULT_MQTT_PASS   "My21052004"
#define DEFAULT_DEVICE_ID   "ESP32_KHO_01"

#define DEFAULT_TEMP_MIN    20.0f
#define DEFAULT_TEMP_MAX    40.0f
#define DEFAULT_HUM_MIN     40.0f
#define DEFAULT_HUM_MAX     80.0f

// Cấu trúc dữ liệu bản ghi Offline (12 bytes/bản ghi)
struct LogRecord {
  uint32_t timestamp;
  float temp;
  float hum;
};

// ==========================================
// 2. BIẾN TOÀN CỤC & ĐỐI TƯỢNG HỆ THỐNG
// ==========================================
// Đối tượng NVS & Thiết bị
Preferences prefs;
Adafruit_AHTX0 aht;
RTC_DS3231 rtc;
LiquidCrystal_I2C lcd(0x27, 16, 2);
SemaphoreHandle_t i2cMutex;
QueueHandle_t sensorQueue;
WiFiClientSecure espClient;
PubSubClient mqtt(espClient);

// Cấu hình NVS đang hoạt động
char g_mqttServer[128] = DEFAULT_MQTT_SERVER;
int  g_mqttPort        = DEFAULT_MQTT_PORT;
char g_mqttUser[64]    = DEFAULT_MQTT_USER;
char g_mqttPass[64]    = DEFAULT_MQTT_PASS;
char g_deviceId[32]    = DEFAULT_DEVICE_ID;

float g_tempMin = DEFAULT_TEMP_MIN;
float g_tempMax = DEFAULT_TEMP_MAX;
float g_humMin  = DEFAULT_HUM_MIN;
float g_humMax  = DEFAULT_HUM_MAX;

// MQTT Topics
char topic_telemetry[64];
char topic_cmd[64];
char topic_recovery[64];

// Trạng thái vận hành thời gian thực
float g_temp = 0.0f, g_hum = 0.0f;
bool  g_isMuted = false;
bool  g_wifiConnected = false;
char  g_storageStatus = ' '; // ' ' (Rỗng), 'E' (EEPROM), 'F' (Flash LittleFS)
uint16_t eeWritePtr = 0;
bool  isEEPROMFull = false;

// Khai báo nguyên mẫu hàm
void loadConfiguration();
void saveThresholds(float tMin, float tMax, float hMin, float hMax);
void scanStorage();
void updateStorageDisplayStatus();
void recoverI2CBus(int sdaPin, int sclPin);
uint32_t getStoredRecordCount();

// ==========================================
// 3. QUẢN LÝ CẤU HÌNH (NVS PREFERENCES)
// ==========================================
void loadConfiguration() {
  prefs.begin("sys_cfg", false);

  if (prefs.isKey("mqtt_server")) {
    prefs.getString("mqtt_server", g_mqttServer, sizeof(g_mqttServer));
    g_mqttPort = prefs.getInt("mqtt_port", DEFAULT_MQTT_PORT);
    prefs.getString("mqtt_user", g_mqttUser, sizeof(g_mqttUser));
    prefs.getString("mqtt_pass", g_mqttPass, sizeof(g_mqttPass));
    prefs.getString("dev_id", g_deviceId, sizeof(g_deviceId));
    
    g_tempMin = prefs.getFloat("t_min", DEFAULT_TEMP_MIN);
    g_tempMax = prefs.getFloat("t_max", DEFAULT_TEMP_MAX);
    g_humMin  = prefs.getFloat("h_min", DEFAULT_HUM_MIN);
    g_humMax  = prefs.getFloat("h_max", DEFAULT_HUM_MAX);
    Serial.println("[NVS] Đã nạp cấu hình hệ thống từ Flash NVS.");
  } else {
    Serial.println("[NVS] Chưa có cấu hình NVS, sử dụng giá trị mặc định.");
  }

  prefs.end();

  // Khởi tạo các topic MQTT theo Device ID
  snprintf(topic_telemetry, sizeof(topic_telemetry), "nhakho/%s/telemetry", g_deviceId);
  snprintf(topic_cmd, sizeof(topic_cmd), "nhakho/%s/cmd", g_deviceId);
  snprintf(topic_recovery, sizeof(topic_recovery), "nhakho/%s/recovery", g_deviceId);
}

void saveThresholds(float tMin, float tMax, float hMin, float hMax) {
  g_tempMin = tMin;
  g_tempMax = tMax;
  g_humMin  = hMin;
  g_humMax  = hMax;

  prefs.begin("sys_cfg", false);
  prefs.putFloat("t_min", g_tempMin);
  prefs.putFloat("t_max", g_tempMax);
  prefs.putFloat("h_min", g_humMin);
  prefs.putFloat("h_max", g_humMax);
  prefs.end();

  Serial.printf("[NVS] Đã lưu ngưỡng mới: Temp [%.1f - %.1f] | Hum [%.1f - %.1f]\n", 
                g_tempMin, g_tempMax, g_humMin, g_humMax);
}

// ==========================================
// 4. PHỤC HỒI PHẦN CỨNG I2C BUS (I2C RECOVERY)
// ==========================================
void recoverI2CBus(int sdaPin, int sclPin) {
  pinMode(sdaPin, INPUT_PULLUP);
  pinMode(sclPin, OUTPUT);

  // Nếu SDA bị kẹt LOW bởi Slave device, phát 9 xung Clock trên SCL để giải phóng
  for (int i = 0; i < 9; i++) {
    digitalWrite(sclPin, HIGH);
    delayMicroseconds(5);
    digitalWrite(sclPin, LOW);
    delayMicroseconds(5);
  }

  // Tạo điều kiện STOP trên bus I2C
  digitalWrite(sclPin, HIGH);
  delayMicroseconds(5);
  pinMode(sdaPin, OUTPUT);
  digitalWrite(sdaPin, LOW);
  delayMicroseconds(5);
  digitalWrite(sdaPin, HIGH);
  delayMicroseconds(5);
  
  pinMode(sdaPin, INPUT);
  pinMode(sclPin, INPUT);
}

// ==========================================
// 5. GIAO TIẾP BỘ NHỚ LƯU TRỮ (EEPROM & FLASH)
// ==========================================
bool writeEEPROM_Safe(uint16_t addr, uint8_t* data, size_t len) {
  size_t written = 0;
  while (written < len) {
    size_t pos = (addr + written) % 32;
    size_t canWrite = 32 - pos;
    size_t chunk = (len - written < canWrite) ? (len - written) : canWrite;
    
    Wire.beginTransmission(EEPROM_I2C_ADDR);
    Wire.write((int)((addr + written) >> 8));   
    Wire.write((int)((addr + written) & 0xFF));
    for (size_t i = 0; i < chunk; i++) Wire.write(data[written + i]);
    if (Wire.endTransmission() != 0) return false;
    
    vTaskDelay(pdMS_TO_TICKS(10));
    written += chunk;
  }
  return true;
}

void readEEPROM(uint16_t addr, uint8_t* dest, size_t len) {
  Wire.beginTransmission(EEPROM_I2C_ADDR);
  Wire.write((int)(addr >> 8));
  Wire.write((int)(addr & 0xFF));
  Wire.endTransmission();
  Wire.requestFrom(EEPROM_I2C_ADDR, (int)len);
  for (size_t i = 0; i < len; i++) { 
    if(Wire.available()) dest[i] = Wire.read();
  }
}

uint32_t getStoredRecordCount() {
  uint32_t count = eeWritePtr / sizeof(LogRecord);
  if (LittleFS.exists(LOG_FILENAME)) {
    File f = LittleFS.open(LOG_FILENAME, "r");
    if (f) {
      count += (f.size() / sizeof(LogRecord));
      f.close();
    }
  }
  return count;
}

void updateStorageDisplayStatus() {
  bool hasFlash = LittleFS.exists(LOG_FILENAME);
  bool hasEeprom = (eeWritePtr > 0);
  if (hasFlash) {
    g_storageStatus = 'F';
  } else if (hasEeprom) {
    g_storageStatus = 'E';
  } else {
    g_storageStatus = ' ';
  }
}

void scanStorage() {
  eeWritePtr = 0;
  LogRecord rec;
  while (eeWritePtr + sizeof(LogRecord) <= EEPROM_SIZE) {
    readEEPROM(eeWritePtr, (uint8_t*)&rec, sizeof(LogRecord));
    if (rec.timestamp == 0xFFFFFFFF || rec.timestamp == 0) break;
    eeWritePtr += sizeof(LogRecord);
  }
  isEEPROMFull = (eeWritePtr + sizeof(LogRecord) > EEPROM_SIZE);
  updateStorageDisplayStatus();
}

// ==========================================
// 6. TASK 5: BUTTONS (RESET 5S & MUTE CÒI)
// ==========================================
void TaskButton(void *pv) {
  uint32_t pressStart = 0;
  bool pressing = false;
  
  while(1) {
    if (digitalRead(BTN_RESET_PIN) == LOW) {
      if (!pressing) { 
        pressing = true;
        pressStart = millis(); 
      } else if (millis() - pressStart >= 5000) {
        Serial.println("\n[BUTTON] Phát hiện nhấn giữ nút 5s! Xóa cấu hình WiFi & NVS...");
        g_wifiConnected = false; 
        
        // Xóa NVS
        prefs.begin("sys_cfg", false);
        prefs.clear();
        prefs.end();

        WiFiManager wm;
        wm.resetSettings();
        Serial.println("[BUTTON] Đã xóa toàn bộ cấu hình. Khởi động lại...");
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP.restart();
      }
    } else { 
      pressing = false;
    }

    if (digitalRead(BTN_MUTE_PIN) == LOW) { 
      g_isMuted = true;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ==========================================
// 7. TASK 1: SENSORS (CORE 1 - Đo mẫu & Đẩy Queue)
// ==========================================
void TaskSensors(void *pv) {
  // Đăng ký Task vào Watchdog Timer
  esp_task_wdt_add(NULL);

  float sumT = 0.0f, sumH = 0.0f;
  int sampleCount = 0;
  uint32_t lastSendTime = millis();
  uint32_t lastNtp = 0;

  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xFrequency = pdMS_TO_TICKS(1500);

  while(1) {
    // Reset Watchdog cho TaskSensors
    esp_task_wdt_reset();

    sensors_event_t h_ev, t_ev;
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(500))) {
      if (aht.getEvent(&h_ev, &t_ev)) {
        g_temp = t_ev.temperature;
        g_hum  = h_ev.relative_humidity;
        sumT  += g_temp; 
        sumH  += g_hum; 
        sampleCount++;
      } else {
        Serial.println("[SENSOR] Cảnh báo: Lỗi đọc cảm biến AHT10!");
      }
      xSemaphoreGive(i2cMutex);
    }

    // Logic kiểm tra ngưỡng cảnh báo (dựa trên cấu hình NVS động)
    bool isTempDanger = (g_temp < g_tempMin || g_temp > g_tempMax);
    bool isHumDanger  = (g_hum < g_humMin || g_hum > g_humMax);

    if ((isTempDanger || isHumDanger) && !g_isMuted) {
      digitalWrite(BUZZER_PIN, HIGH);
    } else {
      digitalWrite(BUZZER_PIN, LOW);
      // Tự động khôi phục chế độ bình thường khi thông số an toàn
      if (!isTempDanger && !isHumDanger) {
        g_isMuted = false;
      }
    }

    // Đẩy dữ liệu vào Queue mỗi 60 giây
    if (millis() - lastSendTime >= 60000) {
      lastSendTime += 60000;
      float avgT = g_temp;
      float avgH = g_hum;
      if (sampleCount > 0) {
        avgT = sumT / (float)sampleCount;
        avgH = sumH / (float)sampleCount;
      }
      
      DateTime now;
      if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(100))) { 
        now = rtc.now(); 
        xSemaphoreGive(i2cMutex);
      }
      
      LogRecord r = { now.unixtime(), avgT, avgH };
      xQueueSend(sensorQueue, &r, 0); 
      
      sumT = 0.0f; sumH = 0.0f; sampleCount = 0;
    }

    // Đồng bộ thời gian qua NTP định kỳ 12 giờ
    if (g_wifiConnected && (millis() - lastNtp > 43200000 || lastNtp == 0)) {
      configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com");
      struct tm info;
      if (getLocalTime(&info, 2000)) {
        if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(500))) {
          rtc.adjust(DateTime(info.tm_year + 1900, info.tm_mon + 1, info.tm_mday, 
                              info.tm_hour, info.tm_min, info.tm_sec));
          xSemaphoreGive(i2cMutex);
          lastNtp = millis();
          Serial.println("[NTP] Đã đồng bộ thời gian từ Server vào DS3231.");
        }
      }
    }
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
  }
}

// ==========================================
// 8. TASK 2: NETWORK (CORE 0 - WiFi, MQTT & NVS Config)
// ==========================================
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  char msg[384];
  if (len >= sizeof(msg)) len = sizeof(msg) - 1;
  memcpy(msg, payload, len); msg[len] = '\0';
  
  StaticJsonDocument<384> doc; 
  DeserializationError err = deserializeJson(doc, msg);
  if (err) {
    Serial.printf("[MQTT] Lỗi parse JSON: %s\n", err.c_str());
    return;
  }
  
  const char* cmd = doc["cmd"];
  if (!cmd) return;
  
  // 1. NÂNG CẤP FIRMWARE TỪ XA (SAFE FOTA WITH MD5)
  if (String(cmd) == "UPDATE_FIRMWARE") {
    const char* url = doc["url"];
    const char* md5 = doc["md5"]; // Mã băm MD5 tùy chọn để bảo mật

    if (url) {
      Serial.printf("[OTA] Bắt đầu tải Firmware từ xa: %s\n", url);
      httpUpdate.rebootOnUpdate(false);
      
      if (md5 && strlen(md5) > 0) {
        Serial.printf("[OTA] Thiết lập xác thực MD5: %s\n", md5);
        httpUpdate.setMD5(md5);
      }
      
      WiFiClientSecure clientSecure; 
      clientSecure.setInsecure();
      t_httpUpdate_return ret = httpUpdate.update(clientSecure, url);
      
      StaticJsonDocument<192> reply;
      if (ret == HTTP_UPDATE_OK) {
        Serial.println("[OTA] Nạp Firmware thành công! Khởi động lại sau 2 giây...");
        reply["status"] = "OTA_SUCCESS";
        reply["message"] = "Firmware updated successfully";
      } else {
        Serial.printf("[OTA] Thất bại! Mã lỗi: (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
        reply["status"] = "OTA_FAILED";
        reply["error_code"] = httpUpdate.getLastError();
        reply["error_msg"] = httpUpdate.getLastErrorString();
      }

      if (mqtt.connected()) {
        char buf[192];
        serializeJson(reply, buf);
        mqtt.publish(topic_telemetry, buf);
        mqtt.publish("nhakho/telemetry", buf); // fallback topic
      }

      if (ret == HTTP_UPDATE_OK) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        ESP.restart();
      }
    }
  } 
  // 2. CẤU HÌNH NGƯỠNG TỪ XA (DYNAMIC THRESHOLDS)
  else if (String(cmd) == "SET_THRESHOLDS") {
    float newTMin = doc.containsKey("temp_min") ? doc["temp_min"].as<float>() : g_tempMin;
    float newTMax = doc.containsKey("temp_max") ? doc["temp_max"].as<float>() : g_tempMax;
    float newHMin = doc.containsKey("hum_min")  ? doc["hum_min"].as<float>()  : g_humMin;
    float newHMax = doc.containsKey("hum_max")  ? doc["hum_max"].as<float>()  : g_humMax;

    saveThresholds(newTMin, newTMax, newHMin, newHMax);

    // Phản hồi gói tin ACK xác nhận cấu hình
    StaticJsonDocument<192> reply;
    reply["status"] = "THRESHOLDS_UPDATED";
    reply["is_ack"] = true;
    reply["temp_min"] = g_tempMin;
    reply["temp_max"] = g_tempMax;
    reply["hum_min"]  = g_humMin;
    reply["hum_max"]  = g_humMax;

    char buffer[192];
    serializeJson(reply, buffer);
    mqtt.publish(topic_telemetry, buffer);
    mqtt.publish("nhakho/telemetry", buffer);
  }
  // 3. TRUY VẤN THÔNG TIN CHẨN ĐOÁN (SYSTEM DIAGNOSTICS)
  else if (String(cmd) == "GET_DIAGNOSTICS") {
    StaticJsonDocument<256> diag;
    diag["is_ack"]      = true;
    diag["device_id"]   = g_deviceId;
    diag["fw_version"]  = FIRMWARE_VERSION;
    diag["free_heap"]   = ESP.getFreeHeap();
    diag["min_heap"]    = ESP.getMinFreeHeap();
    diag["rssi"]        = WiFi.RSSI();
    diag["uptime_sec"]  = millis() / 1000;
    diag["storage_cnt"] = getStoredRecordCount();
    diag["t_min"]       = g_tempMin;
    diag["t_max"]       = g_tempMax;
    diag["h_min"]       = g_humMin;
    diag["h_max"]       = g_humMax;

    char buffer[256];
    serializeJson(diag, buffer);
    mqtt.publish(topic_telemetry, buffer);
    mqtt.publish("nhakho/telemetry", buffer);
  }
  // 4. RESET CẤU HÌNH MẠNG WIFI
  else if (String(cmd) == "RESET_WIFI") {
    g_wifiConnected = false;
    WiFiManager wm; 
    wm.resetSettings();
    vTaskDelay(pdMS_TO_TICKS(1000)); 
    ESP.restart();
  }
  // 5. ĐIỀU KHIỂN CÒI BÁO TỪ XA (MUTE / UNMUTE)
  else if (String(cmd) == "MUTE_BUZZER" || String(cmd) == "UNMUTE_BUZZER") {
    g_isMuted = (String(cmd) == "MUTE_BUZZER"); 
    Serial.println(g_isMuted ? "[MQTT] Đã TẮT còi báo từ xa!" : "[MQTT] Đã BẬT còi báo từ xa!");

    StaticJsonDocument<128> reply;
    reply["alarm_muted"] = g_isMuted ? 1 : 0;
    reply["is_ack"] = true;
    
    char buffer[128]; 
    serializeJson(reply, buffer);
    mqtt.publish(topic_telemetry, buffer); 
    mqtt.publish("nhakho/telemetry", buffer);
  }
}

void TaskNetwork(void *pv) {
  // Đăng ký Task vào Watchdog Timer
  esp_task_wdt_add(NULL);

  WiFiManager wm;
  wm.setConfigPortalBlocking(false);

  // Thêm các trường cấu hình nâng cao vào Captive Portal
  char customPortStr[8];
  snprintf(customPortStr, sizeof(customPortStr), "%d", g_mqttPort);
  char customTMinStr[8], customTMaxStr[8], customHMinStr[8], customHMaxStr[8];
  snprintf(customTMinStr, sizeof(customTMinStr), "%.1f", g_tempMin);
  snprintf(customTMaxStr, sizeof(customTMaxStr), "%.1f", g_tempMax);
  snprintf(customHMinStr, sizeof(customHMinStr), "%.1f", g_humMin);
  snprintf(customHMaxStr, sizeof(customHMaxStr), "%.1f", g_humMax);

  WiFiManagerParameter custom_mqtt_server("server", "MQTT Server Host", g_mqttServer, 128);
  WiFiManagerParameter custom_mqtt_port("port", "MQTT Port", customPortStr, 8);
  WiFiManagerParameter custom_mqtt_user("user", "MQTT Username", g_mqttUser, 64);
  WiFiManagerParameter custom_mqtt_pass("pass", "MQTT Password", g_mqttPass, 64);
  WiFiManagerParameter custom_device_id("device_id", "Mã Định Danh (Device ID)", g_deviceId, 32);
  WiFiManagerParameter custom_temp_min("t_min", "Ngưỡng Nhiệt Độ Min (°C)", customTMinStr, 8);
  WiFiManagerParameter custom_temp_max("t_max", "Ngưỡng Nhiệt Độ Max (°C)", customTMaxStr, 8);
  WiFiManagerParameter custom_hum_min("h_min", "Ngưỡng Độ Ẩm Min (%)", customHMinStr, 8);
  WiFiManagerParameter custom_hum_max("h_max", "Ngưỡng Độ Ẩm Max (%)", customHMaxStr, 8);

  wm.addParameter(&custom_mqtt_server);
  wm.addParameter(&custom_mqtt_port);
  wm.addParameter(&custom_mqtt_user);
  wm.addParameter(&custom_mqtt_pass);
  wm.addParameter(&custom_device_id);
  wm.addParameter(&custom_temp_min);
  wm.addParameter(&custom_temp_max);
  wm.addParameter(&custom_hum_min);
  wm.addParameter(&custom_hum_max);

  char apName[32];
  snprintf(apName, sizeof(apName), "ESP32_KHO_%s", g_deviceId);
  if (!wm.autoConnect(apName)) {
    Serial.printf("[WIFI] Mở AP ngầm: %s để cấu hình.\n", apName);
  }

  LogRecord rec;
  while(1) {
    // Reset Watchdog cho TaskNetwork
    esp_task_wdt_reset();

    wm.process();

    if (WiFi.status() == WL_CONNECTED) {
      g_wifiConnected = true;
      if (!mqtt.connected()) {
        String id = "ESP-" + String(g_deviceId) + "-" + String(random(0xffff), HEX);
        if (mqtt.connect(id.c_str(), g_mqttUser, g_mqttPass)) {
          Serial.println("[MQTT] Đã kết nối HiveMQ Broker bảo mật!");
          mqtt.subscribe(topic_cmd);
          mqtt.subscribe("nhakho/cmd"); // Topic chung
        }
      }
      mqtt.loop();
    } else { 
      g_wifiConnected = false;
    }
    
    // Nhận dữ liệu đo đạc từ Queue
    if (xQueueReceive(sensorQueue, &rec, 0) == pdPASS) {
      if (g_wifiConnected && mqtt.connected()) {
        StaticJsonDocument<256> doc;
        doc["device_id"]   = g_deviceId;
        doc["temp"]        = rec.temp; 
        doc["hum"]         = rec.hum;
        doc["timestamp"]   = rec.timestamp;
        doc["alarm_muted"] = g_isMuted ? 1 : 0; 
        
        // Mở rộng thông số sức khỏe hệ thống (System Diagnostics)
        doc["free_heap"]   = ESP.getFreeHeap();
        doc["min_heap"]    = ESP.getMinFreeHeap();
        doc["rssi"]        = WiFi.RSSI();
        doc["uptime"]      = millis() / 1000;
        doc["fw_ver"]      = FIRMWARE_VERSION;
        doc["storage_cnt"] = getStoredRecordCount();
        
        char buf[256]; 
        serializeJson(doc, buf);
        mqtt.publish(topic_telemetry, buf);
        mqtt.publish("nhakho/telemetry", buf); // Duy trì tương thích với server hiện tại
      } else {
        // Ghi vào bộ nhớ đệm Offline nếu mất mạng (EEPROM -> Flash LittleFS)
        if (!isEEPROMFull) {
           if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(200))) {
             writeEEPROM_Safe(eeWritePtr, (uint8_t*)&rec, sizeof(rec));
             eeWritePtr += sizeof(rec);
             if (eeWritePtr + sizeof(rec) > EEPROM_SIZE) isEEPROMFull = true;
             updateStorageDisplayStatus();
             xSemaphoreGive(i2cMutex);
           }
        } else {
           File f = LittleFS.open(LOG_FILENAME, "a");
           if (f) { 
             f.write((uint8_t*)&rec, sizeof(rec)); 
             f.close(); 
             updateStorageDisplayStatus();
           }
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ==========================================
// 9. TASK 3: DISPLAY (LCD 16x2 REFRESH 1.5S)
// ==========================================
void TaskDisplay(void *pv) {
  while(1) {
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(200))) {
      DateTime now = rtc.now();
      lcd.setCursor(0, 0);
      lcd.printf("T:%.1fC H:%.1f%%  ", g_temp, g_hum);
      lcd.setCursor(0, 1);
      lcd.printf("%02d:%02d  %c   %c   ", now.hour(), now.minute(), 
                 g_storageStatus, (g_wifiConnected ? 'W' : 'X'));
      xSemaphoreGive(i2cMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(1500)); 
  }
}

// ==========================================
// 10. TASK 4: RECOVERY (TỰ ĐỘNG BÙ DỮ LIỆU OFFLINE)
// ==========================================
void TaskRecovery(void *pv) {
  while(1) {
    if (g_wifiConnected && mqtt.connected()) {
      bool isEepromAllSent = true;

      // 1. Phục hồi dữ liệu từ EEPROM
      if (eeWritePtr > 0) {
        LogRecord r;
        for (uint16_t a = 0; a < eeWritePtr; a += sizeof(r)) {
          if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(100))) { 
            readEEPROM(a, (uint8_t*)&r, sizeof(r));
            xSemaphoreGive(i2cMutex); 
          }
          StaticJsonDocument<160> d; 
          d["device_id"] = g_deviceId;
          d["temp"]      = r.temp; 
          d["hum"]       = r.hum;
          d["timestamp"] = r.timestamp;
          d["recovery"]  = true;

          char b[160]; 
          serializeJson(d, b);
          
          if (!mqtt.publish(topic_recovery, b) && !mqtt.publish("nhakho/recovery", b)) { 
            isEepromAllSent = false;
            break; 
          }
          vTaskDelay(pdMS_TO_TICKS(200));
        }

        // Xóa sạch vùng nhớ EEPROM khi đã gửi thành công
        if (isEepromAllSent) {
          if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(500))) {
            uint32_t zero = 0;
            for (uint16_t a = 0; a < eeWritePtr; a += sizeof(r)) {
              writeEEPROM_Safe(a, (uint8_t*)&zero, 4);
            }
            eeWritePtr = 0; 
            isEEPROMFull = false; 
            updateStorageDisplayStatus();
            xSemaphoreGive(i2cMutex);
          }
        }
      }

      // 2. Phục hồi dữ liệu từ Flash LittleFS (nếu có)
      if (isEepromAllSent && LittleFS.exists(LOG_FILENAME)) {
        File f = LittleFS.open(LOG_FILENAME, "r");
        if (f) {
          LogRecord r; 
          bool isFlashAllSent = true;
          while(f.available() >= sizeof(r)) {
            f.read((uint8_t*)&r, sizeof(r));
            StaticJsonDocument<160> d; 
            d["device_id"] = g_deviceId;
            d["temp"]      = r.temp; 
            d["hum"]       = r.hum; 
            d["timestamp"] = r.timestamp;
            d["recovery"]  = true;

            char b[160]; 
            serializeJson(d, b);
            if (!mqtt.publish(topic_recovery, b) && !mqtt.publish("nhakho/recovery", b)) { 
              isFlashAllSent = false;
              break; 
            }
            vTaskDelay(pdMS_TO_TICKS(200));
          }
          f.close();
          if (isFlashAllSent) { 
            LittleFS.remove(LOG_FILENAME);
            updateStorageDisplayStatus();
          }
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(15000));
  }
}

// ==========================================
// 11. SETUP & KHỞI TẠO HỆ THỐNG
// ==========================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n==================================================");
  Serial.printf("  KHỞI ĐỘNG HỆ THỐNG GIÁM SÁT KHO HÀNG (%s)\n", FIRMWARE_VERSION);
  Serial.println("==================================================");

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BTN_MUTE_PIN, INPUT_PULLUP);
  pinMode(BTN_RESET_PIN, INPUT_PULLUP);
  
  // Khởi tạo và nạp cấu hình từ Flash NVS
  loadConfiguration();

  // Khởi tạo cơ chế Phục hồi Bus I2C trước khi giao tiếp
  recoverI2CBus(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // Tạo Mutex bảo vệ I2C Bus & Queue truyền dữ liệu cảm biến
  i2cMutex = xSemaphoreCreateMutex();
  sensorQueue = xQueueCreate(10, sizeof(LogRecord));

  // Khởi tạo hệ thống file LittleFS & Màn hình LCD
  LittleFS.begin(true);
  lcd.init(); 
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Smart Storage");
  lcd.setCursor(0, 1);
  lcd.printf("FW: %s", FIRMWARE_VERSION);

  // Cấu hình MQTT & TLS
  espClient.setInsecure();
  mqtt.setServer(g_mqttServer, g_mqttPort);
  mqtt.setCallback(mqttCallback);
  
  // Khởi tạo các cảm biến I2C
  if (!aht.begin()) Serial.println("[HW] Cảnh báo: Lỗi khởi tạo cảm biến AHT10!");
  if (!rtc.begin()) Serial.println("[HW] Cảnh báo: Lỗi khởi tạo đồng hồ DS3231!");
  
  // Quét vùng nhớ EEPROM & Flash
  scanStorage();

  // Khởi tạo Watchdog Timer cho hệ thống (TWDT)
  esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true);

  // Tạo 5 Tasks FreeRTOS phân bổ trên 2 Nhân Vi xử lý (Core 0 & Core 1)
  xTaskCreatePinnedToCore(TaskNetwork,  "TaskNet",  8192, NULL, 3, NULL, 0); // Core 0: Network & MQTT
  xTaskCreatePinnedToCore(TaskButton,   "TaskBtn",  2048, NULL, 4, NULL, 1); // Core 1: Phím bấm
  xTaskCreatePinnedToCore(TaskSensors,  "TaskSen",  4096, NULL, 2, NULL, 1); // Core 1: Cảm biến & Còi
  xTaskCreatePinnedToCore(TaskDisplay,  "TaskLcd",  2048, NULL, 1, NULL, 1); // Core 1: Màn hình LCD
  xTaskCreatePinnedToCore(TaskRecovery, "TaskRec",  4096, NULL, 1, NULL, 1); // Core 1: Bù dữ liệu Offline
}

void loop() { 
  // FreeRTOS đã đảm nhận tất cả tác vụ, loop() có thể giải phóng
  vTaskDelete(NULL);
}
