require('dotenv').config();
const express = require('express');
const mqtt = require('mqtt');
const { GoogleSpreadsheet } = require('google-spreadsheet');
const { JWT } = require('google-auth-library');
const multer = require('multer');
const path = require('path');
const fs = require('fs');
const http = require('http');
const socketIo = require('socket.io');
const crypto = require('crypto');

const app = express();
const server = http.createServer(app);
const io = socketIo(server);

const PORT = process.env.PORT || 3001;
const SPREADSHEET_ID = process.env.SPREADSHEET_ID;

let serviceAccount = null;
const serviceAccountPath = path.join(__dirname, 'service-account.json');
if (fs.existsSync(serviceAccountPath)) {
    try {
        serviceAccount = JSON.parse(fs.readFileSync(serviceAccountPath, 'utf8'));
    } catch (e) {
        console.warn("⚠️ Không thể đọc service-account.json:", e.message);
    }
} else {
    console.warn("⚠️ Không tìm thấy file service-account.json. Chế độ Google Sheets sẽ tạm tắt.");
}

// ==========================================
// 🔔 CẤU HÌNH THÔNG BÁO FB MESSENGER (CallMeBot) VÀ CÁC NGƯỠNG AN TOÀN
// ==========================================
const FB_API_KEY = process.env.FB_API_KEY || "xxxxx"; 

// Các ngưỡng an toàn mặc định
let TEMP_MIN = 20.0;
let TEMP_MAX = 40.0;
let HUM_MIN = 40.0;
let HUM_MAX = 80.0;

let lastAlertTime = 0; 

// Hàm gửi tin nhắn cảnh báo
async function sendMessengerAlert(alertDetails, temp, hum, timeStr) {
    if (!FB_API_KEY || FB_API_KEY === "xxxxx") {
        console.warn("⚠️ Chưa cấu hình FB_API_KEY. Bỏ qua gửi Facebook Messenger.");
        return;
    }
    
    // Cooldown 15 phút chống Spam
    if (Date.now() - lastAlertTime < 900000) return;

    const message = `🚨 CẢNH BÁO NHÀ KHO 🚨\n${alertDetails}\n🌡️ Nhiệt độ hiện tại: ${temp}°C\n💧 Độ ẩm hiện tại: ${hum}%\n⏰ Thời gian: ${timeStr}`;
    const url = `https://api.callmebot.com/facebook/send.php?apikey=${FB_API_KEY}&text=${encodeURIComponent(message)}`;

    try {
        const response = await fetch(url);
        if (response.ok) {
            console.log("💬 [MESSENGER] Đã gửi tin nhắn cảnh báo thành công:\n", alertDetails);
            lastAlertTime = Date.now(); 
        }
    } catch (error) {
        console.error("❌ [MESSENGER] Lỗi kết nối gửi tin nhắn:", error);
    }
}

// ==========================================
// CẤU HÌNH MQTT HIVEMQ
// ==========================================
const mqttOptions = {
    host: process.env.HIVEMQ_HOST || "c7b38d664d464ca79fc4feb942741794.s1.eu.hivemq.cloud",
    port: 8883,
    protocol: 'mqtts',
    username: process.env.HIVEMQ_USERNAME || "IOT_NHIETDO_DOAM",
    password: process.env.HIVEMQ_PASSWORD || "My21052004",
};

const upload = multer({ storage: multer.memoryStorage() });

let doc = null;
if (serviceAccount && SPREADSHEET_ID) {
    const serviceAccountAuth = new JWT({
        email: serviceAccount.client_email,
        key: serviceAccount.private_key,
        scopes: ['https://www.googleapis.com/auth/spreadsheets'],
    });
    doc = new GoogleSpreadsheet(SPREADSHEET_ID, serviceAccountAuth);
}

let lastReceivedData = { temp: null, hum: null, time: 0 };
let lastKnownState = { 
    temp: null, 
    hum: null, 
    timeStr: null, 
    alarm_muted: null,
    free_heap: null,
    rssi: null,
    uptime: null,
    fw_ver: null,
    device_id: null
};

io.on('connection', (socket) => {
    if (lastKnownState.temp !== null) {
        socket.emit('mqttData', {
            ...lastKnownState,
            time: lastKnownState.timeStr
        });
    }
});

const client = mqtt.connect(mqttOptions);
client.on('connect', () => {
    console.log('Connected to HiveMQ Broker (TLS)');
    // Đăng ký topic tiêu chuẩn và wildcard cho multi-device
    client.subscribe('nhakho/telemetry');
    client.subscribe('nhakho/recovery');
    client.subscribe('nhakho/+/telemetry');
    client.subscribe('nhakho/+/recovery');
});

client.on('message', async (topic, message) => {
    try {
        const data = JSON.parse(message.toString());
        
        // 1. Phản hồi nhanh ACK hoặc cấu hình
        if (data.is_ack) {
            if (data.alarm_muted !== undefined) {
                lastKnownState.alarm_muted = data.alarm_muted;
            }
            if (data.status === 'THRESHOLDS_UPDATED') {
                TEMP_MIN = data.temp_min !== undefined ? data.temp_min : TEMP_MIN;
                TEMP_MAX = data.temp_max !== undefined ? data.temp_max : TEMP_MAX;
                HUM_MIN = data.hum_min !== undefined ? data.hum_min : HUM_MIN;
                HUM_MAX = data.hum_max !== undefined ? data.hum_max : HUM_MAX;
                console.log(`[CONFIG] Đã đồng bộ ngưỡng mới từ ESP32: T[${TEMP_MIN}-${TEMP_MAX}] H[${HUM_MIN}-${HUM_MAX}]`);
            }
            io.emit('mqttData', data);
            return; 
        }

        // 2. OTA Events
        if (data.status === 'OTA_SUCCESS') {
            io.emit('otaEvent', { success: true, message: '✓ Mạch đã nạp Code OTA thành công và khởi động lại!' });
            return; 
        } else if (data.status === 'OTA_FAILED') {
            io.emit('otaEvent', { success: false, message: `Nạp Code thất bại! Mã lỗi: ${data.error_code || ''} (${data.error_msg || ''})` });
            return; 
        }

        // 3. Xử lý số liệu đo đạc (Telemetry & Diagnostics)
        const nowMs = Date.now();
        let timestamp;
        let isRecovery = topic.includes('recovery');

        if (isRecovery && data.timestamp) {
            timestamp = new Date(data.timestamp * 1000).toLocaleString('vi-VN', { timeZone: 'Asia/Ho_Chi_Minh' });
        } else {
            timestamp = new Date().toLocaleString('vi-VN', { timeZone: 'Asia/Ho_Chi_Minh' });
        }

        const suffix = isRecovery ? ' [Recovery]' : '';
        let timeStr = timestamp.split(' ')[0];
        
        // Gửi toàn bộ telemetry (kèm diagnostics) tới Web Dashboard
        io.emit('mqttData', { ...data, time: timeStr });

        if (!isRecovery && data.temp != null && data.hum != null) {
            lastKnownState = { 
                temp: data.temp, 
                hum: data.hum, 
                timeStr: timeStr, 
                alarm_muted: data.alarm_muted !== undefined ? data.alarm_muted : lastKnownState.alarm_muted,
                free_heap: data.free_heap || lastKnownState.free_heap,
                rssi: data.rssi || lastKnownState.rssi,
                uptime: data.uptime || lastKnownState.uptime,
                fw_ver: data.fw_ver || lastKnownState.fw_ver,
                device_id: data.device_id || lastKnownState.device_id
            };
            
            // 🟢 KIỂM TRA ĐIỀU KIỆN ĐỂ GỬI MESSENGER
            const currentTemp = parseFloat(data.temp);
            const currentHum = parseFloat(data.hum);
            let alertDetails = "";

            if (currentTemp > TEMP_MAX) alertDetails += `⚠️ LỖI: NHIỆT ĐỘ QUÁ CAO (>${TEMP_MAX}°C)\n`;
            else if (currentTemp < TEMP_MIN) alertDetails += `⚠️ LỖI: NHIỆT ĐỘ QUÁ THẤP (<${TEMP_MIN}°C)\n`;

            if (currentHum > HUM_MAX) alertDetails += `⚠️ LỖI: ĐỘ ẨM QUÁ CAO (>${HUM_MAX}%)\n`;
            else if (currentHum < HUM_MIN) alertDetails += `⚠️ LỖI: ĐỘ ẨM QUÁ THẤP (<${HUM_MIN}%)\n`;

            // Nếu có ít nhất 1 lỗi xảy ra thì kích hoạt cảnh báo
            if (alertDetails !== "") {
                sendMessengerAlert(alertDetails, currentTemp, currentHum, timestamp);
            }
        }

        // Chống spam Google Sheets & Ghi bản ghi nếu có cấu hình
        if (doc) {
            if (!isRecovery) {
                if (lastReceivedData.temp === data.temp && 
                    lastReceivedData.hum === data.hum && 
                    (nowMs - lastReceivedData.time < 45000)) {
                    return; 
                }
                lastReceivedData = { temp: data.temp, hum: data.hum, time: nowMs };
            }

            try {
                await doc.loadInfo();
                const sheet = doc.sheetsByTitle['Data'];
                if (sheet) {
                    await sheet.addRow({
                        Timestamp: `${timestamp}${suffix}`,
                        Temperature: parseFloat(data.temp).toFixed(1).replace('.', ','),
                        Humidity: parseFloat(data.hum).toFixed(1).replace('.', ',')
                    });
                }
            } catch (sheetErr) {
                console.warn('Lỗi ghi Google Sheets:', sheetErr.message);
            }
        }
    } catch (err) {
        console.error('Lỗi xử lý bản ghi MQTT:', err);
    }
});

app.use(express.static('public'));
app.use(express.json()); 

app.post('/control-buzzer', (req, res) => {
    try {
        const { command } = req.body; 
        if (command === 'MUTE_BUZZER' || command === 'UNMUTE_BUZZER') {
            client.publish('nhakho/cmd', JSON.stringify({ cmd: command }));
            res.send('Đã gửi lệnh điều khiển còi!');
        } else {
            res.status(400).send('Lệnh không hợp lệ');
        }
    } catch (err) {
        res.status(500).send(err.message);
    }
});

app.post('/set-thresholds', (req, res) => {
    try {
        const { temp_min, temp_max, hum_min, hum_max } = req.body;
        const payload = {
            cmd: "SET_THRESHOLDS",
            temp_min: parseFloat(temp_min),
            temp_max: parseFloat(temp_max),
            hum_min: parseFloat(hum_min),
            hum_max: parseFloat(hum_max)
        };
        client.publish('nhakho/cmd', JSON.stringify(payload));
        res.send('Đã phát lệnh cập nhật ngưỡng an toàn tới thiết bị!');
    } catch (err) {
        res.status(500).send(err.message);
    }
});

app.post('/get-diagnostics', (req, res) => {
    try {
        client.publish('nhakho/cmd', JSON.stringify({ cmd: "GET_DIAGNOSTICS" }));
        res.send('Đã yêu cầu chẩn đoán sức khỏe hệ thống từ ESP32!');
    } catch (err) {
        res.status(500).send(err.message);
    }
});

app.post('/reset-wifi', (req, res) => {
    try {
        client.publish('nhakho/cmd', JSON.stringify({ cmd: "RESET_WIFI" }));
        res.send('Đã gửi lệnh xóa WiFi!');
    } catch (err) {
        res.status(500).send(err.message);
    }
});

app.post('/upload-ota', upload.single('firmware'), async (req, res) => {
    if (!req.file) return res.status(400).send('Không tìm thấy file firmware .bin');
    const token = process.env.GITHUB_TOKEN;
    const owner = process.env.GITHUB_OWNER;
    const repo = process.env.GITHUB_REPO;
    const branch = process.env.GITHUB_BRANCH || 'main';
    const targetPath = 'public/ota/firmware.bin';
    const apiUrl = `https://api.github.com/repos/${owner}/${repo}/contents/${targetPath}`;
    const fileBase64 = req.file.buffer.toString('base64');
    
    // Tính toán mã băm MD5 để gửi xuống ESP32 xác thực toàn vẹn
    const md5Hash = crypto.createHash('md5').update(req.file.buffer).digest('hex');
    let fileSha = null;

    try {
        const getRes = await fetch(`${apiUrl}?ref=${branch}`, {
            headers: { 'Authorization': `token ${token}`, 'Accept': 'application/vnd.github.v3+json', 'User-Agent': 'NodeJS-OTA' }
        });
        if (getRes.ok) fileSha = (await getRes.json()).sha; 
        const putPayload = { message: `OTA Update ${new Date().toLocaleTimeString()} [MD5: ${md5Hash}]`, content: fileBase64, branch: branch };
        if (fileSha) putPayload.sha = fileSha; 
        const putRes = await fetch(apiUrl, {
            method: 'PUT',
            headers: { 'Authorization': `token ${token}`, 'Accept': 'application/vnd.github.v3+json', 'Content-Type': 'application/json', 'User-Agent': 'NodeJS-OTA' },
            body: JSON.stringify(putPayload)
        });
        if (!putRes.ok) throw new Error(await putRes.text());
        const rawOtaUrl = `https://raw.githubusercontent.com/${owner}/${repo}/${branch}/${targetPath}`;
        
        // Gửi lệnh OTA kèm mã băm MD5
        client.publish('nhakho/cmd', JSON.stringify({ 
            cmd: "UPDATE_FIRMWARE", 
            url: rawOtaUrl,
            md5: md5Hash 
        }));
        res.send(`Đã đẩy firmware lên Github (MD5: ${md5Hash}) và phát lệnh nạp OTA!`);
    } catch (err) {
        res.status(500).send(`Lỗi đẩy file lên GitHub: ${err.message}`);
    }
});

server.listen(PORT, () => console.log(`Server Render đang chạy tại Port ${PORT}`));