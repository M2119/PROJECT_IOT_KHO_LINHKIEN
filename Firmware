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

// ==========================================
// 1. CẤU HÌNH HỆ THỐNG & PHẦN CỨNG
// ==========================================
#define BUZZER_PIN    18
#define BTN_MUTE_PIN  19
#define BTN_RESET_PIN 23

// 🟢 CÁC NGƯỠNG CẢNH BÁO AN TOÀN MỚI
#define TEMP_MIN      20.0
#define TEMP_MAX      40.0
#define HUM_MIN       40.0
#define HUM_MAX       90.0

#define EEPROM_I2C_ADDR 0x57
#define EEPROM_SIZE     4096 
#define LOG_FILENAME    "/offline.dat"

// Thông tin MQTT HiveMQ
const char* mqtt_server     = "c7b38d664d464ca79fc4feb942741794.s1.eu.hivemq.cloud";
const int   mqtt_port       = 8883;
const char* mqtt_user       = "IOT_NHIETDO_DOAM";
const char* mqtt_pass       = "My21052004";             
const char* topic_telemetry = "nhakho/telemetry";
const char* topic_cmd       = "nhakho/cmd";
const char* topic_recovery  = "nhakho/recovery";

// Cấu trúc dữ liệu bản ghi (12 bytes)
struct LogRecord {
  uint32_t timestamp;
  float temp;
  float hum;
};

// Khởi tạo các đối tượng
Adafruit_AHTX0 aht;
RTC_DS3231 rtc;
LiquidCrystal_I2C lcd(0x27, 16, 2);
SemaphoreHandle_t i2cMutex;
QueueHandle_t sensorQueue;
WiFiClientSecure espClient;
PubSubClient mqtt(espClient);

// Biến trạng thái hệ thống
float g_temp = 0.0, g_hum = 0.0;
bool  g_isMuted = false;
bool  g_wifiConnected = false;
char  g_storageStatus = ' '; // ' ' (Trống), 'E' (EEPROM), 'F' (Flash)
uint16_t eeWritePtr = 0;
bool  isEEPROMFull = false;

// Khai báo hàm
void scanStorage();
void updateStorageDisplayStatus();

// ==========================================
// 2. HÀM GIAO TIẾP BỘ NHỚ (EEPROM & FLASH)
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
// 3. TASK 5: BUTTONS (RESET 5S & MUTE)
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
        Serial.println("\n[BUTTON] Phát hiện giữ nút 5s! Xóa cấu hình WiFi...");
        g_wifiConnected = false; 
        WiFiManager wm;
        wm.resetSettings();
        Serial.println("[BUTTON] Đã xóa WiFi. Tự động mở AP: ESP32_T-H-Realtimer");
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
// 4. TASK 1: SENSORS (CORE 1 - Đo mẫu & đẩy vào Queue)
// ==========================================
void TaskSensors(void *pv) {
  float sumT = 0.0, sumH = 0.0;
  int sampleCount = 0;
  uint32_t lastSendTime = millis();
  uint32_t lastNtp = 0;

  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xFrequency = pdMS_TO_TICKS(1500);

  while(1) {
    sensors_event_t h_ev, t_ev;
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(500))) {
      aht.getEvent(&h_ev, &t_ev);
      xSemaphoreGive(i2cMutex);
      g_temp = t_ev.temperature;
      g_hum  = h_ev.relative_humidity;
      
      sumT  += g_temp; 
      sumH  += g_hum; 
      sampleCount++;
    }

    // 🟢 LOGIC CÒI BÁO ĐỘNG MỚI (KIỂM TRA CẢ NHIỆT ĐỘ VÀ ĐỘ ẨM)
    bool isTempDanger = (g_temp < TEMP_MIN || g_temp > TEMP_MAX);
    bool isHumDanger  = (g_hum < HUM_MIN || g_hum > HUM_MAX);

    if ((isTempDanger || isHumDanger) && !g_isMuted) {
      digitalWrite(BUZZER_PIN, HIGH);
    } else {
      digitalWrite(BUZZER_PIN, LOW);
      // Tự động thoát trạng thái Mute khi CẢ 2 thông số đều đã trở lại vùng an toàn
      if (!isTempDanger && !isHumDanger) {
        g_isMuted = false;
      }
    }

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
      
      sumT = 0.0; sumH = 0.0; sampleCount = 0;
    }

    if (g_wifiConnected && (millis() - lastNtp > 43200000 || lastNtp == 0)) {
      configTime(7*3600, 0, "pool.ntp.org");
      struct tm info;
      if (getLocalTime(&info)) {
        if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(500))) {
          rtc.adjust(DateTime(info.tm_year+1900, info.tm_mon+1, info.tm_mday, info.tm_hour, info.tm_min, info.tm_sec));
          xSemaphoreGive(i2cMutex);
          lastNtp = millis();
        }
      }
    }
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
  }
}

// ==========================================
// 5. TASK 2: NETWORK (CORE 0 - Xử lý WiFi, MQTT)
// ==========================================
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  char msg[256];
  memcpy(msg, payload, len); msg[len] = '\0';
  StaticJsonDocument<256> doc; deserializeJson(doc, msg);
  
  const char* cmd = doc["cmd"];
  
  if (cmd && String(cmd) == "UPDATE_FIRMWARE") {
    const char* url = doc["url"];
    if (url) {
      Serial.println("[OTA] Bat dau tai Firmware tu xa...");
      httpUpdate.rebootOnUpdate(false);
      WiFiClientSecure c; c.setInsecure();
      t_httpUpdate_return ret = httpUpdate.update(c, url);
      
      if (ret == HTTP_UPDATE_OK) {
        Serial.println("[OTA] Thanh cong! Dang bao cao len Server...");
        if (!mqtt.connected()) {
          String id = "ESP-" + String(random(0xffff), HEX);
          mqtt.connect(id.c_str(), mqtt_user, mqtt_pass);
        }
        mqtt.publish(topic_telemetry, "{\"status\": \"OTA_SUCCESS\"}");
        vTaskDelay(pdMS_TO_TICKS(2000)); 
        ESP.restart();
      } else {
        Serial.println("[OTA] That bai!");
        if (!mqtt.connected()) {
          String id = "ESP-" + String(random(0xffff), HEX);
          mqtt.connect(id.c_str(), mqtt_user, mqtt_pass);
        }
        mqtt.publish(topic_telemetry, "{\"status\": \"OTA_FAILED\"}");
      }
    }
  } 
  else if (cmd && String(cmd) == "RESET_WIFI") {
    g_wifiConnected = false;
    WiFiManager wm; wm.resetSettings();
    vTaskDelay(pdMS_TO_TICKS(1000)); ESP.restart();
  }
  // 🟢 NÂNG CẤP LỆNH MUTE KÈM GÓI TIN PHẢN HỒI NHANH (ACK)
  else if (cmd && (String(cmd) == "MUTE_BUZZER" || String(cmd) == "UNMUTE_BUZZER")) {
    g_isMuted = (String(cmd) == "MUTE_BUZZER"); 
    Serial.println(g_isMuted ? "[MQTT] Da TAT coi bao tu xa!" : "[MQTT] Da BAT coi bao tu xa!");

    StaticJsonDocument<128> reply;
    reply["alarm_muted"] = g_isMuted ? 1 : 0;
    reply["is_ack"] = true; // Báo Server cập nhật Giao diện ngay lập tức
    
    char buffer[128]; 
    serializeJson(reply, buffer);
    mqtt.publish(topic_telemetry, buffer); 
  }
}

void TaskNetwork(void *pv) {
  WiFiManager wm;
  wm.setConfigPortalBlocking(false);
  if (!wm.autoConnect("ESP32_T-H-Realtimer")) {
    Serial.println("[WIFI] Mở AP ngầm: ESP32_T-H-Realtimer.");
  }

  LogRecord rec;
  while(1) {
    wm.process();

    if (WiFi.status() == WL_CONNECTED) {
      g_wifiConnected = true;
      if (!mqtt.connected()) {
        String id = "ESP-" + String(random(0xffff), HEX);
        if (mqtt.connect(id.c_str(), mqtt_user, mqtt_pass)) {
          mqtt.subscribe(topic_cmd);
        }
      }
      mqtt.loop();
    } else { 
      g_wifiConnected = false;
    }
    
    if (xQueueReceive(sensorQueue, &rec, 0) == pdPASS) {
      if (g_wifiConnected && mqtt.connected()) {
        StaticJsonDocument<128> doc;
        doc["temp"] = rec.temp; 
        doc["hum"]  = rec.hum;
        doc["timestamp"] = rec.timestamp;
        doc["alarm_muted"] = g_isMuted ? 1 : 0; 
        
        char buf[128]; 
        serializeJson(doc, buf);
        mqtt.publish(topic_telemetry, buf);
      } else {
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
// 6. TASK 3: DISPLAY (LCD 1.5S)
// ==========================================
void TaskDisplay(void *pv) {
  while(1) {
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(200))) {
      DateTime now = rtc.now();
      lcd.setCursor(0,0);
      lcd.printf("T:%.1fC H:%.1f%%  ", g_temp, g_hum);
      lcd.setCursor(0,1);
      lcd.printf("%02d:%02d  %c   %c    ", now.hour(), now.minute(), 
                 g_storageStatus, (g_wifiConnected ? 'W' : 'X'));
      xSemaphoreGive(i2cMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(1500)); 
  }
}

// ==========================================
// 7. TASK 4: RECOVERY (CHẠY NỀN)
// ==========================================
void TaskRecovery(void *pv) {
  while(1) {
    if (g_wifiConnected && mqtt.connected()) {
      bool isEepromAllSent = true;
      if (eeWritePtr > 0) {
        LogRecord r;
        for (uint16_t a = 0; a < eeWritePtr; a += sizeof(r)) {
          if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(100))) { 
            readEEPROM(a, (uint8_t*)&r, sizeof(r));
            xSemaphoreGive(i2cMutex); 
          }
          StaticJsonDocument<128> d; 
          d["temp"] = r.temp; 
          d["hum"]  = r.hum;
          d["timestamp"] = r.timestamp;
          char b[128]; serializeJson(d, b);
          
          if (!mqtt.publish(topic_recovery, b)) { 
            isEepromAllSent = false;
            break; 
          }
          vTaskDelay(pdMS_TO_TICKS(200));
        }

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

      if (isEepromAllSent && LittleFS.exists(LOG_FILENAME)) {
        File f = LittleFS.open(LOG_FILENAME, "r");
        if (f) {
          LogRecord r; 
          bool isFlashAllSent = true;
          while(f.available() >= sizeof(r)) {
            f.read((uint8_t*)&r, sizeof(r));
            StaticJsonDocument<128> d;
            d["temp"] = r.temp; 
            d["hum"]  = r.hum; 
            d["timestamp"] = r.timestamp;
            char b[128]; serializeJson(d, b);
            if (!mqtt.publish(topic_recovery, b)) { 
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
// 8. SETUP & LOOP
// ==========================================
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BTN_MUTE_PIN, INPUT_PULLUP);
  pinMode(BTN_RESET_PIN, INPUT_PULLUP);
  
  Wire.begin(21, 22);
  i2cMutex = xSemaphoreCreateMutex();
  
  sensorQueue = xQueueCreate(10, sizeof(LogRecord));

  LittleFS.begin(true);
  lcd.init(); 
  lcd.backlight();
  espClient.setInsecure();
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setCallback(mqttCallback);
  
  if (!aht.begin()) Serial.println("[HW] Lỗi AHT10");
  if (!rtc.begin()) Serial.println("[HW] Lỗi DS3231");
  
  scanStorage();
  
  xTaskCreatePinnedToCore(TaskNetwork,  "Net", 8192, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(TaskButton,   "Btn", 2048, NULL, 4, NULL, 1);
  xTaskCreatePinnedToCore(TaskSensors,  "Sen", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskDisplay,  "Lcd", 2048, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskRecovery, "Rec", 4096, NULL, 1, NULL, 1);
}

void loop() { 
  vTaskDelete(NULL);
}
