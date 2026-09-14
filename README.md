# 📦 HỆ THỐNG GIÁM SÁT KHO HÀNG THÔNG MINH ĐA TẦNG (ESP32 INDUSTRIAL IOT)

> **Hệ thống nhúng giám sát nhiệt độ & độ ẩm môi trường kho linh kiện điện tử cấp công nghiệp.**  
> Phát triển trên vi điều khiển **ESP32 Dual-Core (FreeRTOS)**, tích hợp cơ chế chống mất mát dữ liệu **Edge-Buffering (EEPROM + Flash)**, bảo mật cấu hình **Flash NVS**, giám sát chống treo hệ thống **Task Watchdog Timer (TWDT)**, điều khiển ngưỡng từ xa và cập nhật **FOTA an toàn qua Cloud với xác thực MD5 Checksum**.

---

## 🚀 1. Tính Năng Nổi Bật (Key Technical Highlights)

### 🧩 1.1. Kiến trúc RTOS Đa Nhiệm (FreeRTOS Dual-Core)
Hệ thống tận dụng tối đa 2 lõi xử lý (Xtensa Dual-Core 240MHz) của ESP32 để tách biệt các tác vụ theo thời gian thực:
- **Core 0 (Mạng & Giao thức):** Xử lý kết nối WiFi, giao thức bảo mật MQTTs (TLS 8883) qua HiveMQ Cloud và Portal cấu hình mạng.
- **Core 1 (Cảm biến, I/O & Lưu trữ):**
  - `TaskSensors`: Lấy mẫu cảm biến chu kỳ 1.5s, tính giá trị trung bình trượt và đẩy vào `sensorQueue` (Thread-Safe).
  - `TaskButton`: Bắt sự kiện phím bấm vật lý (Mute còi, nhấn giữ 5s để factory reset WiFi & NVS).
  - `TaskDisplay`: Làm mới màn hình LCD 16x2 hiển thị trạng thái hệ thống.
  - `TaskRecovery`: Tác vụ chạy nền (Background Worker) tự động phát hiện và bù dữ liệu offline lên Cloud khi mạng phục hồi.
- **Đồng bộ hóa Thread-Safe:** Sử dụng `SemaphoreHandle_t (Mutex)` để bảo vệ đường truyền bus I2C giữa các tác vụ (tránh tranh chấp dữ liệu giữa Cảm biến, EEPROM, RTC và LCD).

### 🛡️ 1.2. Khả năng Phục hồi & Chống Mất Dữ Liệu (Edge-Buffering Resilience)
- Khi mất kết nối WiFi/MQTT, hệ thống tự động chuyển sang chế độ ghi đệm dữ liệu:
  - **Tầng 1 - EEPROM 24C32 (I2C):** Lưu nhanh dữ liệu vào bộ nhớ tĩnh.
  - **Tầng 2 - SPI Flash (LittleFS):** Tự động mở rộng sang Flash khi EEPROM đầy.
- **Cơ chế Disaster Recovery:** Ngay khi mạng trực tuyến trở lại, `TaskRecovery` quét và đẩy lần lượt các bản ghi quá khứ lên Cloud topic `nhakho/recovery` kèm nhãn thời gian thực từ DS3231, sau đó xóa sạch vùng nhớ an toàn.

### 🔒 1.3. Quản lý Cấu hình & Bảo mật (Flash NVS / Preferences)
- Loại bỏ hoàn toàn hardcode thông tin kết nối và ngưỡng an toàn trong mã nguồn.
- Thông số WiFi, MQTT Broker (Host, Port, User, Pass), Device ID và các ngưỡng min/max được lưu trữ trong **Flash NVS (`sys_cfg`)**.
- Cho phép cấu hình linh hoạt qua **Captive Portal của WiFiManager** khi cài đặt mới hoặc thay đổi môi trường.

### ⏱️ 1.4. Giám sát Chống Treo Hệ thống (Task Watchdog Timer - TWDT)
- Tích hợp Watchdog phần cứng giám sát liên tục các Task trọng yếu (`TaskSensors`, `TaskNetwork`).
- Nếu xảy ra hiện tượng kẹt I2C bus hoặc starvation quá 20 giây, WDT tự động kích hoạt reset vi điều khiển để đưa hệ thống về trạng thái hoạt động an toàn.

### 🌐 1.5. Nâng cấp Firmware từ xa An toàn (Safe FOTA with MD5 Checksum)
- Tiếp nhận lệnh OTA từ Cloud Backend.
- Tự động kiểm tra tính toàn vẹn gói tin bằng mã băm **MD5 Checksum** (`httpUpdate.setMD5()`) trước khi ghi đè vào phân vùng Flash, ngăn chặn rủi ro nạp firmware lỗi.

### 🔧 1.6. Chống Kẹt Bus I2C (I2C Bus Recovery)
- Cơ chế giải phóng đường truyền I2C (Clock Pumping 9 xung clock) tự động phát hiện và xử lý hiện tượng Slave giữ chân SDA ở mức LOW.

---

## 📐 2. Sơ đồ Kiến trúc Hệ thống (System Architecture)

```mermaid
graph TD
    subgraph "ESP32 Embedded System (Dual-Core FreeRTOS)"
        subgraph "Core 1 - Realtime Sensing & Storage"
            Sensors["Cảm biến AHT10 / DS3231"] -->|I2C Mutex| TaskSen["TaskSensors (Đo mẫu & Lọc số liệu)"]
            TaskBtn["TaskButton (Mute / Reset 5s)"]
            TaskLcd["TaskDisplay (LCD 16x2)"]
            TaskRec["TaskRecovery (Bù dữ liệu Offline)"]
            
            TaskSen -->|xQueue| NetQueue[("sensorQueue (FreeRTOS)")]
            TaskSen -.->|Mất mạng| EEPROM["EEPROM 24C32 (4KB)"]
            EEPROM -.->|Tràn bộ nhớ| Flash["LittleFS Flash (/offline.dat)"]
        end
        
        subgraph "Core 0 - Networking & Security"
            NetQueue --> TaskNet["TaskNetwork (WiFi + MQTT TLS)"]
            TWDT["Task Watchdog Timer (20s)"] -.->|Giám sát| TaskSen
            TWDT -.->|Giám sát| TaskNet
            NVS[("Flash NVS (Preferences)")] <-->|Đọc/Ghi| TaskNet
        end
    end

    subgraph "Cloud & Backend Infrastructure"
        TaskNet <==>|MQTTs TLS (Port 8883)| HiveMQ["HiveMQ Cloud Broker"]
        HiveMQ <==> NodeServer["Node.js / Express Backend"]
        NodeServer <==>|WebSocket (Socket.IO)| Dashboard["Web Dashboard (Chart.js + Control Panel)"]
        NodeServer -->|REST API| FB["Messenger Alert (CallMeBot)"]
        NodeServer -.->|Ghi bản ghi| GSheets["Google Sheets Logging"]
        NodeServer -->|GitHub API| GitHubOTA["Firmware Storage (GitHub)"]
    end
```

---

## 🔌 3. Sơ đồ Nối dây Phần cứng (Hardware Pinout)

| Linh Kiện | Chân Module | Chân ESP32 | Chức Năng |
| :--- | :--- | :--- | :--- |
| **AHT10 / DS3231 / LCD / EEPROM** | SDA | **GPIO 21** | Đường truyền dữ liệu I2C Bus |
| **AHT10 / DS3231 / LCD / EEPROM** | SCL | **GPIO 22** | Đường tạo xung Clock I2C Bus |
| **Còi Báo Động (Buzzer 5V)** | VCC / Signal | **GPIO 18** | Đầu ra cảnh báo âm thanh |
| **Nút Bấm Mute Còi** | Chân 1 | **GPIO 19** (Pull-up) | Tắt tiếng còi tức thời |
| **Nút Reset Cấu Hình** | Chân 1 | **GPIO 23** (Pull-up) | Giữ 5s xóa cấu hình WiFi & NVS |
| **Nguồn Cung Cấp** | VIN / GND | **5V / GND** | Nguồn hệ thống ổn định |

---

## 📡 4. Chuẩn Giao Thức MQTT (MQTT API Specifications)

### 📤 4.1. Telemetry định kỳ (`nhakho/telemetry` hoặc `nhakho/{device_id}/telemetry`)
```json
{
  "device_id": "ESP32_KHO_01",
  "temp": 28.5,
  "hum": 65.2,
  "timestamp": 1726330000,
  "alarm_muted": 0,
  "free_heap": 182340,
  "min_heap": 165000,
  "rssi": -62,
  "uptime": 3600,
  "fw_ver": "v2.1.0",
  "storage_cnt": 0
}
```

### 📥 4.2. Lệnh Điều Khiển & Cấu Hình (`nhakho/cmd` hoặc `nhakho/{device_id}/cmd`)
- **Cập nhật ngưỡng an toàn:**
  ```json
  { "cmd": "SET_THRESHOLDS", "temp_min": 18.0, "temp_max": 35.0, "hum_min": 45.0, "hum_max": 75.0 }
  ```
- **Nâng cấp Firmware OTA kèm mã MD5:**
  ```json
  { "cmd": "UPDATE_FIRMWARE", "url": "https://.../firmware.bin", "md5": "d41d8cd98f00b204e9800998ecf8427e" }
  ```
- **Điều khiển còi:**
  ```json
  { "cmd": "MUTE_BUZZER" }  // hoặc "UNMUTE_BUZZER"
  ```
- **Truy vấn chẩn đoán hệ thống:**
  ```json
  { "cmd": "GET_DIAGNOSTICS" }
  ```

---

## 🛠️ 5. Hướng Dẫn Cài Đặt & Biên Dịch (Getting Started)

### 5.1. Biên dịch Firmware (Arduino IDE / PlatformIO)
1. Cài đặt các thư viện phụ thuộc:
   - `Adafruit AHTX0`
   - `RTClib` (Adafruit)
   - `LiquidCrystal_I2C`
   - `WiFiManager` (tzapu)
   - `PubSubClient` (Nick O'Leary)
   - `ArduinoJson` (v6.x)
2. Chọn Board: **ESP32 Dev Module** (Phân vùng: *Default 4MB with spiffs/littlefs*).
3. Nạp file `Firmware.ino` vào mạch ESP32 qua cổng USB.
4. Khi mạch khởi động lần đầu, kết nối vào mạng WiFi AP: `ESP32_KHO_ESP32_KHO_01` (192.168.4.1) để cấu hình WiFi và các thông số MQTT/Ngưỡng cảnh báo.

### 5.2. Chạy Backend & Dashboard
```bash
# Cài đặt thư viện Node.js
npm install

# Khởi chạy server
npm start
```
Truy cập giao diện tại: `http://localhost:3001`

---

## 💼 6. Cách Đưa Dự Án Này Vào CV (Mẫu Trình Bày Chuẩn)

> ### **HỆ THỐNG GIÁM SÁT KHO HÀNG THÔNG MINH ĐA TẦNG (INDUSTRIAL ESP32 IOT)**
> **Vai trò:** Kỹ sư Lập trình Nhúng & Hệ thống IoT (Embedded & IoT Systems)  
> **Công nghệ:** C/C++, ESP32 FreeRTOS, MQTTs (TLS), NVS Storage, LittleFS, Node.js, Socket.IO, Chart.js.
> 
> - **Kiến trúc FreeRTOS Đa Nhiệm:** Thiết kế và triển khai 5 Tasks chạy song song trên 2 Core vi xử lý ESP32, sử dụng Mutex đồng bộ hóa bus I2C và Queue trao đổi dữ liệu thread-safe.
> - **Cơ chế Edge-Buffering & Data Recovery:** Phát triển giải pháp lưu đệm offline đa cấp (I2C EEPROM 4KB + Flash LittleFS), tự động phát hiện mất mạng và bù dữ liệu (Auto-backfilling) khi phục hồi kết nối, loại bỏ 100% nguy cơ thất thoát dữ liệu.
> - **Quản lý Cấu hình & Độ tin cậy cao:** Ứng dụng Flash NVS (Preferences) cấu hình tham số động, tích hợp Task Watchdog Timer (TWDT) chống deadlock và cơ chế phục hồi bus I2C khi phần cứng bị treo.
> - **Bảo mật & FOTA Update:** Xây dựng tính năng nâng cấp Firmware từ xa an toàn tích hợp xác thực mã băm MD5 Checksum và giám sát sức khỏe thiết bị thời gian thực (Free Heap, WiFi RSSI, Uptime).