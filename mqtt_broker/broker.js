/**
 * ==========================================================================
 * MQTT BROKER ALL-IN-ONE (NODE.JS + AEDES + SQLITE + MYSQL + WEB SERVER)
 * THIẾT KẾ CHUẨN THEO BÀI THỰC HÀNH TH6, TH7 & SƠ ĐỒ KHỐI HỆ THỐNG IOT
 * ==========================================================================
 * - Cổng 1883 (TCP)      : Dành cho bo mạch ESP32 thật & Wokwi kết nối MQTT
 * - Cổng 9001 (WebSocket): Dành cho Web Dashboard nhận & gửi dữ liệu thời gian thực
 * - Cổng 3000 (HTTP Web) : Phục vụ giao diện Web Dashboard và API REST
 * - Cơ sở dữ liệu Kép    : 
 *      + SQLite (dulieu_iot.db): Tự động lưu không cần cài đặt, đúng chuẩn file mẫu
 *      + MySQL (iot_db -> LOG) : Tự động lưu nếu có dịch vụ MySQL đang chạy
 * ==========================================================================
 */

const aedes = require('aedes')();
const net = require('net');
const http = require('http');
const fs = require('fs');
const path = require('path');
const os = require('os');
const ws = require('websocket-stream');
const sqlite3 = require('sqlite3').verbose();
const QRCode = require('qrcode');
let mqttLib = null;
try {
  mqttLib = require('mqtt');
} catch (e) {}
let mysql = null;
try {
  mysql = require('mysql2');
} catch (e) {
  console.log('ℹ️ Thư viện mysql2 chưa được nạp, hệ thống sẽ ưu tiên SQLite.');
}

const TCP_PORT = 1883;
const WS_PORT = 9001;
const HTTP_PORT = 3000;

// ==========================================================================
// 1. TỰ ĐỘNG LẤY ĐỊA CHỈ IP LAN MÁY TÍNH (BỎ QUA 169.254.x.x)
// ==========================================================================
function getLocalIPs() {
  const interfaces = os.networkInterfaces();
  const ips = [];
  for (const name of Object.keys(interfaces)) {
    for (const iface of interfaces[name]) {
      if (iface.family === 'IPv4' && !iface.internal) {
        if (!iface.address.startsWith('169.254.')) {
          ips.push({ name, address: iface.address });
        }
      }
    }
  }
  return ips;
}

// ==========================================================================
// 2. KHỞI TẠO CƠ SỞ DỮ LIỆU SQLITE (dulieu_iot.db - THEO FILE MẪU BÁO CÁO)
// ==========================================================================
const dbPath = path.join(__dirname, 'dulieu_iot.db');
const sqliteDb = new sqlite3.Database(dbPath, (err) => {
  if (err) {
    console.error('❌ [SQLITE LỖI] Không thể mở file dulieu_iot.db:', err.message);
  } else {
    console.log('✅ [SQLITE KẾT NỐI] Đã mở thành công cơ sở dữ liệu:', dbPath);
    sqliteDb.run(`
      CREATE TABLE IF NOT EXISTS nhat_ky_tuoicay (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        device TEXT,
        moisture REAL,
        raw_adc INTEGER,
        voltage REAL,
        water_level TEXT,
        water_empty INTEGER,
        pump_status INTEGER,
        mode TEXT,
        alert TEXT,
        light_status TEXT DEFAULT 'CO SANG',
        created_at DATETIME DEFAULT (datetime('now', 'localtime'))
      )
    `, (tableErr) => {
      if (tableErr) console.error('❌ Lỗi tạo bảng SQLite:', tableErr.message);
      else {
        // Tự động bổ sung cột light_status nếu bảng cũ chưa có
        sqliteDb.run("ALTER TABLE nhat_ky_tuoicay ADD COLUMN light_status TEXT DEFAULT 'CO SANG'", () => {});
        console.log('✅ [SQLITE BẢNG] Bảng "nhat_ky_tuoicay" đã sẵn sàng ghi nhận!');
      }
    });
  }
});

// ==========================================================================
// 3. KẾT NỐI MYSQL DATABASE (iot_db -> LOG) & TỰ ĐỘNG CẬP NHẬT CỘT CẢM BIẾN
// ==========================================================================
let mysqlPool = null;
let mysqlConnected = false;

function saveRecordToMysql(rec, cb) {
  if (!mysqlPool || !mysqlConnected) {
    if (cb) cb(new Error('MySQL chưa kết nối'));
    return;
  }
  const statusText = rec.pumpStatus ? 'BOM: BAT' : 'BOM: TAT';
  const ledRedText = rec.waterEmpty ? 'BAO CAN' : 'TAT';
  mysqlPool.query(`
    INSERT INTO LOG (device, raw_adc, voltage, water_level, water_empty, pump_status, mode, light_status, alert, status, led_red)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
  `, [
    rec.device, rec.rawAdc, rec.voltage,
    rec.waterLevel, rec.waterEmpty, rec.pumpStatus, rec.mode,
    rec.lightStatus, rec.alert, statusText, ledRedText
  ], (err, res) => {
    if (!err && res) {
      console.log(`   🐬 [MYSQL ĐÃ LƯU] ID: ${res.insertId} | iot_db.LOG | Raw: ${rec.rawAdc} | Áp: ${rec.voltage}V | Bơm: ${rec.pumpStatus ? 'BẬT' : 'TẮT'}`);
    } else if (err) {
      console.error(`   ❌ [MYSQL LỖI LƯU]:`, err.message);
    }
    if (cb) cb(err, res);
  });
}

if (mysql) {
  try {
    // Đảm bảo Database iot_db luôn tồn tại trước khi mở Pool
    const bootstrapConn = mysql.createConnection({
      host: 'localhost',
      user: 'root',
      password: '123456'
    });
    bootstrapConn.query('CREATE DATABASE IF NOT EXISTS iot_db CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci', () => {
      bootstrapConn.end();

      mysqlPool = mysql.createPool({
        host: 'localhost',
        user: 'root',
        password: '123456',
        database: 'iot_db',
        waitForConnections: true,
        connectionLimit: 5,
        queueLimit: 0
      });

      mysqlPool.getConnection((err, conn) => {
        if (err) {
          mysqlConnected = false;
          console.log('ℹ️ [MYSQL CHƯA BẬT] Bỏ qua MySQL, hệ thống hoạt động với SQLite:', err.message);
        } else {
          mysqlConnected = true;
          console.log('✅ [MYSQL KẾT NỐI] Đã kết nối MySQL Database (iot_db) thành công!');
          conn.query(`
            CREATE TABLE IF NOT EXISTS LOG (
              id INT AUTO_INCREMENT PRIMARY KEY,
              device VARCHAR(50),
              raw_adc INT DEFAULT 0,
              voltage FLOAT DEFAULT 0,
              water_level VARCHAR(20) DEFAULT 'DAY',
              water_empty INT DEFAULT 0,
              pump_status INT DEFAULT 0,
              mode VARCHAR(20) DEFAULT 'AUTO',
              light_status VARCHAR(20) DEFAULT 'CO SANG',
              alert VARCHAR(60) DEFAULT 'BINH THUONG',
              status VARCHAR(50) DEFAULT '',
              led_red VARCHAR(20) DEFAULT '',
              created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
          `, () => {
            // Tự động bổ sung các cột cần thiết và xóa 3 cột dư (temperature, humidity, moisture)
            const alterCols = [
              "ALTER TABLE LOG DROP COLUMN temperature",
              "ALTER TABLE LOG DROP COLUMN humidity",
              "ALTER TABLE LOG DROP COLUMN moisture",
              "ALTER TABLE LOG ADD COLUMN voltage FLOAT DEFAULT 0",
              "ALTER TABLE LOG ADD COLUMN water_level VARCHAR(20) DEFAULT 'DAY'",
              "ALTER TABLE LOG ADD COLUMN water_empty INT DEFAULT 0",
              "ALTER TABLE LOG ADD COLUMN pump_status INT DEFAULT 0",
              "ALTER TABLE LOG ADD COLUMN mode VARCHAR(20) DEFAULT 'AUTO'",
              "ALTER TABLE LOG ADD COLUMN light_status VARCHAR(20) DEFAULT 'CO SANG'",
              "ALTER TABLE LOG ADD COLUMN alert VARCHAR(60) DEFAULT 'BINH THUONG'",
              "ALTER TABLE LOG ADD COLUMN status VARCHAR(50) DEFAULT ''",
              "ALTER TABLE LOG ADD COLUMN led_red VARCHAR(20) DEFAULT ''"
            ];
            let pending = alterCols.length;
            alterCols.forEach((sql) => {
              conn.query(sql, () => {
                pending--;
                if (pending === 0) {
                  conn.release();
                  console.log('✅ [MYSQL TABLE] Bảng "iot_db.LOG" đã đồng bộ đầy đủ cột dữ liệu!');
                }
              });
            });
          });
        }
      });
    });
  } catch (err) {
    // Không bắt buộc MySQL
  }
}

// ==========================================================================
// 4. XÁC THỰC TÀI KHOẢN MQTT (THEO BÀI TH6)
// ==========================================================================
aedes.authenticate = (client, username, password, callback) => {
  // Cho phép kết nối ẩn danh nếu không cấu hình tài khoản
  if (!username && !password) {
    return callback(null, true);
  }

  const user = username ? username.toString() : '';
  const pass = password ? password.toString() : '';

  const isValidUser = (
    (user === 'user1' && pass === '123') ||
    (user === 'user1' && pass === '123456') ||
    (user === 'admin' && pass === '123456') ||
    (user === 'admin' && pass === '1234') ||
    (user === 'Nvtuong' && pass === 'Tuongquadeptrai2005.')
  );

  if (isValidUser) {
    console.log(`🔑 [XÁC THỰC THÀNH CÔNG] Client: ${client.id} (User: ${user})`);
    callback(null, true);
  } else {
    console.log(`❌ [XÁC THỰC THẤT BẠI] Client: ${client.id} (User: ${user}) - Sai thông tin!`);
    const error = new Error('Auth error: Wrong username or password');
    error.returnCode = 4;
    callback(error, false);
  }
};

// ==========================================================================
// 5. THEO DÕI HOẠT ĐỘNG (LOGGING & DATA INGESTION)
// ==========================================================================
aedes.on('client', (client) => {
  const time = new Date().toLocaleTimeString();
  console.log(`\n🟢 [${time}] [KẾT NỐI MỚI] Client ID: ${client.id}`);
});

aedes.on('clientDisconnect', (client) => {
  const time = new Date().toLocaleTimeString();
  console.log(`🔴 [${time}] [NGẮT KẾT NỐI] Client ID: ${client.id}`);
});

aedes.on('subscribe', (subscriptions, client) => {
  if (client) {
    const time = new Date().toLocaleTimeString();
    subscriptions.forEach((sub) => {
      console.log(`📡 [${time}] [SUBSCRIBE] ${client.id} đăng ký Topic: "${sub.topic}" (QoS: ${sub.qos})`);
    });
  }
});

let latestSensorData = null;
let lastEspPacketTime = 0;
let lanSweepRunning = false;

// Chủ động quét mạng LAN để đánh thức bảng ARP (2.4GHz <-> 5GHz) và tìm IP ESP32 tự động
function sweepLanForEsp32() {
  if (lanSweepRunning) return;
  if (Date.now() - lastEspPacketTime < 8000) return;
  const ips = getLocalIPs();
  if (!ips || ips.length === 0) return;

  lanSweepRunning = true;
  const primaryIp = ips[0].address;
  const parts = primaryIp.split('.');
  if (parts.length !== 4) {
    lanSweepRunning = false;
    return;
  }
  const prefix = `${parts[0]}.${parts[1]}.${parts[2]}.`;
  const selfHost = Number(parts[3]);
  let remaining = 0;

  for (let h = 2; h <= 254; h++) {
    if (h === selfHost) continue;
    remaining++;
    const targetIp = prefix + h;
    const sock = new net.Socket();
    sock.setTimeout(1200);
    sock.on('connect', () => {
      sock.destroy();
      // Kiểm tra xem có phải cổng HTTP /api/data của ESP32 không
      const req = http.get(`http://${targetIp}/api/data`, { timeout: 11000 }, (res) => {
        let body = '';
        res.on('data', chunk => { body += chunk.toString(); });
        res.on('end', () => {
          try {
            const json = JSON.parse(body);
            if (json && json.moisture !== undefined) {
              json.ip = targetIp;
              json.device = json.device || 'ESP32_TuoiCay';
              latestSensorData = json;
              if (Date.now() - lastEspPacketTime > 5000) {
                console.log(`🔍 [LAN AUTO-DISCOVERY] Đã tìm thấy mạch ESP32 tại IP: ${targetIp} (Cùng mạng LAN)`);
                const payload = Buffer.from(JSON.stringify(json));
                aedes.publish({ topic: 'esp32/soil_moisture/data', payload, qos: 0, retain: false }, () => {});
              }
            }
          } catch (e) {}
        });
      });
      req.on('timeout', () => req.destroy());
      req.on('error', () => {});
    });
    sock.on('timeout', () => sock.destroy());
    sock.on('error', () => sock.destroy());
    sock.on('close', () => {
      if (--remaining <= 0) {
        lanSweepRunning = false;
      }
    });
    sock.connect(80, targetIp);
  }
}
setTimeout(sweepLanForEsp32, 1500);
setInterval(sweepLanForEsp32, 10000);

// Lưu dữ liệu vào Database khi có gói tin Publish
aedes.on('publish', (packet, client) => {
  if (packet.topic.startsWith('$SYS')) return;

  const time = new Date().toLocaleTimeString();
  const sender = client ? client.id : 'BROKER_INTERNAL';
  const payloadStr = packet.payload ? packet.payload.toString() : '';

  // Bỏ qua log ping hoặc message rỗng
  if (!payloadStr.trim()) return;

  console.log(`📥 [${time}] [PUBLISH] Từ: [${sender}] | Topic: "${packet.topic}"`);
  console.log(`   ➜ Dữ liệu: ${payloadStr}`);

  // Kiểm tra gói tin cảm biến
  const isSensorTopic = (
    packet.topic === 'esp32/soil_moisture/data' ||
    packet.topic === 'sensor/data' ||
    packet.topic.includes('soil') ||
    packet.topic.includes('sensor')
  );

  if (isSensorTopic && payloadStr.trim().startsWith('{')) {
    try {
      const data = JSON.parse(payloadStr);
      const device = data.device || sender || 'ESP32_TuoiCay';
      const moisture = (data.moisture !== undefined) ? Number(data.moisture) : (data.hum || 0);
      const rawAdc = (data.raw !== undefined) ? Number(data.raw) : 0;
      const voltage = (data.voltage !== undefined) ? Number(data.voltage) : 0;
      const waterLevel = data.water_level || (data.water_empty ? 'CẠN' : 'ĐẦY');
      const waterEmpty = (data.water_empty === true || data.water_empty === 1) ? 1 : 0;
      const pumpStatus = (data.pump === true || data.pump === 1 || data.pump === 'BAT' || data.pump === 'ON') ? 1 : 0;
      const mode = data.mode || 'AUTO';
      const alert = data.alert || (waterEmpty ? 'CANH BAO: CAN NUOC' : 'BINH THUONG');
      const lightStatus = data.light_status || (data.light === false ? 'MAT SANG' : 'CO SANG');
      latestSensorData = data;
      if (sender && sender.startsWith('ESP32')) {
        lastEspPacketTime = Date.now();
      }

      // 1. Ghi vào SQLite (bảng nhat_ky_tuoicay)
      sqliteDb.run(`
        INSERT INTO nhat_ky_tuoicay (device, moisture, raw_adc, voltage, water_level, water_empty, pump_status, mode, alert, light_status)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
      `, [device, moisture, rawAdc, voltage, waterLevel, waterEmpty, pumpStatus, mode, alert, lightStatus], function (err) {
        if (err) {
          console.error(`   ❌ [SQLITE LỖI LƯU]:`, err.message);
        } else {
          console.log(`   💾 [SQLITE ĐÃ LƯU] ID: ${this.lastID} | Thiết bị: ${device} | Ẩm: ${moisture}% | Bơm: ${pumpStatus ? 'BẬT' : 'TẮT'} | Bồn: ${waterLevel} | AS: ${lightStatus}`);
        }
      });

      // 2. Ghi trực tiếp vào MySQL (iot_db -> bảng LOG)
      saveRecordToMysql({
        device, moisture, rawAdc, voltage,
        waterLevel, waterEmpty, pumpStatus, mode, lightStatus, alert
      });
    } catch (parseErr) {
      // Payload không phải JSON hợp lệ
    }
  }

  // Chuyển tiếp lệnh điều khiển từ Web Dashboard sang mô phỏng Wokwi Online (nếu đang chạy)
  if (packet.topic === 'esp32/soil_moisture/control' && wokwiCloudClient && wokwiCloudClient.connected) {
    wokwiCloudClient.publish(WOKWI_TOPIC_CONTROL, payloadStr);
    console.log(`☁️ [WOKWI BRIDGE] Đã chuyển lệnh điều khiển sang Wokwi Simulator: ${payloadStr}`);
  }
});

// ==========================================================================
// 5B. CẦU NỐI TỰ ĐỘNG ĐỒNG BỘ MÔ PHỎNG WOKWI ONLINE <-> LOCAL BROKER
// ==========================================================================
const WOKWI_CLOUD_URL     = 'mqtt://broker.emqx.io:1883';
const WOKWI_TOPIC_DATA    = 'tuoicay_tuong_2026/esp32/soil_moisture/data';
const WOKWI_TOPIC_STATUS  = 'tuoicay_tuong_2026/esp32/status';
const WOKWI_TOPIC_CONTROL = 'tuoicay_tuong_2026/esp32/soil_moisture/control';
let wokwiCloudClient = null;

if (mqttLib) {
  try {
    wokwiCloudClient = mqttLib.connect(WOKWI_CLOUD_URL, {
      clientId: 'LocalBroker_WokwiBridge_' + Math.random().toString(16).slice(2, 8),
      reconnectPeriod: 5000,
      connectTimeout: 8000
    });

    wokwiCloudClient.on('connect', () => {
      console.log('✅ [WOKWI BRIDGE] Đã sẵn sàng đồng bộ 2 chiều với trình mô phỏng Wokwi Online!');
      wokwiCloudClient.subscribe([WOKWI_TOPIC_DATA, WOKWI_TOPIC_STATUS]);
    });

    wokwiCloudClient.on('message', (topic, message) => {
      const payload = Buffer.from(message);
      if (topic === WOKWI_TOPIC_DATA) {
        aedes.publish({ topic: 'esp32/soil_moisture/data', payload, qos: 0, retain: false }, () => {});
      } else if (topic === WOKWI_TOPIC_STATUS) {
        aedes.publish({ topic: 'esp32/status', payload, qos: 0, retain: false }, () => {});
      }
    });

    wokwiCloudClient.on('error', () => {
      // Bỏ qua êm nếu máy tính không có mạng Internet (chế độ LAN thuần vẫn chạy 100%)
    });
  } catch (e) {}
}

// ==========================================================================
// 6. KHỞI TẠO HTTP WEB SERVER (PORT 3000) & WEBSOCKET (PORT 9001) & TCP (PORT 1883)
// ==========================================================================

// HTTP Server phục vụ Web Dashboard và API REST
const publicDir = fs.existsSync(path.join(__dirname, 'public', 'index.html'))
  ? path.join(__dirname, 'public')
  : __dirname;
const httpServer = http.createServer((req, res) => {
  // CORS Header cho phép Web truy cập từ mọi IP trong mạng LAN
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, DELETE, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');

  if (req.method === 'OPTIONS') {
    res.writeHead(204);
    res.end();
    return;
  }

  // API 1: Lấy dữ liệu cảm biến mới nhất thời gian thực
  if (req.url === '/api/latest') {
    res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
    res.end(JSON.stringify(latestSensorData || {}));
    return;
  }

  // API Wokwi: Trả về bộ 3 file mô phỏng Wokwi (sketch.ino, diagram.json, libraries.txt) để Copy 1 chạm
  if (req.url === '/api/wokwi-files') {
    try {
      const wokwiDir = fs.existsSync(path.join(__dirname, 'wokwi', 'sketch.ino'))
        ? path.join(__dirname, 'wokwi')
        : path.resolve(__dirname, '..', 'esp32_tuoicay_iot', 'wokwi');
      const sketch = fs.readFileSync(path.join(wokwiDir, 'sketch.ino'), 'utf8');
      const diagram = fs.readFileSync(path.join(wokwiDir, 'diagram.json'), 'utf8');
      const libraries = fs.readFileSync(path.join(wokwiDir, 'libraries.txt'), 'utf8');
      res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
      res.end(JSON.stringify({
        sketch,
        diagram,
        libraries,
        wokwi_bridge_connected: Boolean(wokwiCloudClient && wokwiCloudClient.connected)
      }));
    } catch (err) {
      res.writeHead(500, { 'Content-Type': 'application/json; charset=utf-8' });
      res.end(JSON.stringify({ error: err.message }));
    }
    return;
  }

  // API 2: Điều khiển Bơm / Chế độ qua HTTP fallback
  if (req.url.startsWith('/api/control')) {
    try {
      const u = new URL(req.url, `http://${req.headers.host || 'localhost'}`);
      const action = u.searchParams.get('action');
      let cmd = {};
      if (action === 'toggle_mode') {
        const nextMode = (latestSensorData && latestSensorData.mode === 'AUTO') ? 'MANUAL' : 'AUTO';
        cmd = { action: 'set_mode', mode: nextMode };
      } else if (action === 'toggle_pump') {
        const nextPump = !(latestSensorData && latestSensorData.pump);
        cmd = { action: 'set_pump', state: nextPump };
      } else if (action === 'reset_config') {
        cmd = { action: 'reset_config' };
        if (latestSensorData && latestSensorData.ip) {
          http.get(`http://${latestSensorData.ip}/api/reset_config`, () => {}).on('error', () => {});
        }
      }
      const payload = Buffer.from(JSON.stringify(cmd));
      aedes.publish({ topic: 'esp32/soil_moisture/control', payload, qos: 0, retain: false }, () => {});
      aedes.publish({ topic: 'sensor/control', payload, qos: 0, retain: false }, () => {});
      res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
      res.end(JSON.stringify({ success: true, cmd }));
    } catch(e) {
      res.writeHead(500, { 'Content-Type': 'application/json; charset=utf-8' });
      res.end(JSON.stringify({ error: e.message }));
    }
    return;
  }

  // API 3: Lấy danh sách 50 bản ghi lịch sử mới nhất từ SQLite
  if (req.url === '/api/history') {
    sqliteDb.all(`
      SELECT * FROM nhat_ky_tuoicay 
      ORDER BY id DESC 
      LIMIT 50
    `, [], (err, rows) => {
      if (err) {
        res.writeHead(500, { 'Content-Type': 'application/json; charset=utf-8' });
        res.end(JSON.stringify({ error: err.message }));
      } else {
        res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
        res.end(JSON.stringify(rows));
      }
    });
    return;
  }

  // API 3B: Lấy lịch sử dữ liệu từ MySQL (Database: iot_db -> Bảng: LOG)
  if (req.url.startsWith('/api/mysql/history')) {
    if (!mysqlPool || !mysqlConnected) {
      res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
      res.end(JSON.stringify({ connected: false, database: 'iot_db', table: 'LOG', total: 0, rows: [] }));
      return;
    }
    mysqlPool.query('SELECT COUNT(*) AS total FROM LOG', (cErr, cRes) => {
      const total = (!cErr && cRes && cRes[0]) ? cRes[0].total : 0;
      mysqlPool.query(`
        SELECT id, device, ROUND(GREATEST(0, LEAST(100, (2380 - raw_adc) * 100.0 / (2380 - 1250))), 1) AS moisture, raw_adc, voltage, water_level, water_empty, pump_status, mode, light_status, alert, status, created_at
        FROM LOG
        ORDER BY id DESC
        LIMIT 50
      `, (err, rows) => {
        if (err) {
          res.writeHead(500, { 'Content-Type': 'application/json; charset=utf-8' });
          res.end(JSON.stringify({ connected: false, error: err.message, rows: [] }));
        } else {
          res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
          res.end(JSON.stringify({ connected: true, database: 'iot_db', table: 'LOG', total, rows: rows || [] }));
        }
      });
    });
    return;
  }

  // API 3C: Gửi thủ công mẫu dữ liệu hiện tại vào MySQL (iot_db.LOG)
  if (req.url.startsWith('/api/mysql/push')) {
    let body = '';
    req.on('data', chunk => { body += chunk.toString(); });
    req.on('end', () => {
      let src = latestSensorData || {};
      if (body.trim().startsWith('{')) {
        try { src = Object.assign({}, src, JSON.parse(body)); } catch (e) {}
      }
      const waterEmpty = (src.water_empty === true || src.water_empty === 1) ? 1 : 0;
      const pumpStatus = (src.pump === true || src.pump === 1 || src.pump === 'BAT') ? 1 : 0;
      const rec = {
        device: src.device || 'ESP32_WebPush',
        moisture: src.moisture !== undefined ? Number(src.moisture) : 52,
        rawAdc: src.raw !== undefined ? Number(src.raw) : 1800,
        voltage: src.voltage !== undefined ? Number(src.voltage) : 1.45,
        waterLevel: src.water_level || (waterEmpty ? 'CẠN' : 'ĐẦY'),
        waterEmpty,
        pumpStatus,
        mode: src.mode || 'AUTO',
        lightStatus: src.light_status || (src.light === false ? 'MAT SANG' : 'CO SANG'),
        alert: src.alert || (waterEmpty ? 'CANH BAO: CAN NUOC' : 'BINH THUONG')
      };
      saveRecordToMysql(rec, (err, result) => {
        if (err) {
          res.writeHead(500, { 'Content-Type': 'application/json; charset=utf-8' });
          res.end(JSON.stringify({ success: false, error: err.message }));
        } else {
          res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
          res.end(JSON.stringify({ success: true, insertId: result ? result.insertId : null }));
        }
      });
    });
    return;
  }

  // API 3D: Xóa dữ liệu trong MySQL (Xóa toàn bộ bảng LOG hoặc xóa 1 dòng theo ?id=...)
  if (req.url.startsWith('/api/mysql/clear')) {
    if (!mysqlPool || !mysqlConnected) {
      res.writeHead(500, { 'Content-Type': 'application/json; charset=utf-8' });
      res.end(JSON.stringify({ success: false, error: 'MySQL chưa kết nối!' }));
      return;
    }
    const u = new URL(req.url, `http://${req.headers.host || 'localhost'}`);
    const rowId = u.searchParams.get('id');
    if (rowId) {
      mysqlPool.query('DELETE FROM LOG WHERE id = ?', [Number(rowId)], (err, result) => {
        if (err) {
          res.writeHead(500, { 'Content-Type': 'application/json; charset=utf-8' });
          res.end(JSON.stringify({ success: false, error: err.message }));
        } else {
          console.log(`🗑️ [MYSQL XÓA DÒNG] Đã xóa bản ghi ID #${rowId} khỏi bảng iot_db.LOG`);
          res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
          res.end(JSON.stringify({ success: true, deletedId: Number(rowId), affectedRows: result.affectedRows }));
        }
      });
    } else {
      mysqlPool.query('TRUNCATE TABLE LOG', (err) => {
        if (err) {
          // Dự phòng nếu quyền user không cho TRUNCATE thì dùng DELETE
          mysqlPool.query('DELETE FROM LOG', (err2) => {
            if (err2) {
              res.writeHead(500, { 'Content-Type': 'application/json; charset=utf-8' });
              res.end(JSON.stringify({ success: false, error: err2.message }));
            } else {
              console.log('🗑️ [MYSQL XÓA SẠCH] Đã xóa toàn bộ dữ liệu bảng iot_db.LOG!');
              res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
              res.end(JSON.stringify({ success: true, message: 'Đã xóa sạch toàn bộ dữ liệu trong MySQL (iot_db.LOG)!' }));
            }
          });
        } else {
          console.log('🗑️ [MYSQL TRUNCATE] Đã xóa sạch toàn bộ dữ liệu và reset ID bảng iot_db.LOG về 1!');
          res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
          res.end(JSON.stringify({ success: true, message: 'Đã xóa sạch toàn bộ dữ liệu MySQL và đặt lại ID từ 1!' }));
        }
      });
    }
    return;
  }

  // API 2: Lấy thông tin trạng thái Broker & IP
  if (req.url === '/api/info') {
    const ips = getLocalIPs();
    const primaryIP = ips.length > 0 ? ips[0].address : '127.0.0.1';
    let hostHeader = req.headers.host;
    if (!hostHeader || hostHeader.includes('localhost') || hostHeader.includes('127.0.0.1')) {
      hostHeader = `${primaryIP}:${HTTP_PORT}`;
    }
    const currentUrl = `http://${hostHeader}`;
    res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
    res.end(JSON.stringify({
      broker: 'NodeJS Aedes MQTT Broker',
      tcp_port: TCP_PORT,
      ws_port: WS_PORT,
      http_port: HTTP_PORT,
      lan_ip: primaryIP,
      url: currentUrl,
      ips: ips,
      connected_clients: aedes.connectedClients
    }));
    return;
  }

  // API 4: Tạo ảnh mã QR Code động cho điện thoại quét truy cập
  if (req.url.startsWith('/api/qr')) {
    const ips = getLocalIPs();
    const primaryIP = ips.length > 0 ? ips[0].address : '127.0.0.1';
    let hostHeader = req.headers.host;
    if (!hostHeader || hostHeader.includes('localhost') || hostHeader.includes('127.0.0.1')) {
      hostHeader = `${primaryIP}:${HTTP_PORT}`;
    }
    const targetUrl = `http://${hostHeader}`;

    QRCode.toBuffer(targetUrl, { width: 320, margin: 2, color: { dark: '#0b1329', light: '#ffffff' } }, (err, buffer) => {
      if (err) {
        res.writeHead(500);
        res.end('Error generating QR code');
      } else {
        res.writeHead(200, {
          'Content-Type': 'image/png',
          'Cache-Control': 'no-cache, no-store, must-revalidate'
        });
        res.end(buffer);
      }
    });
    return;
  }

  // Phục vụ file tĩnh trong thư mục public/
  let reqPath = req.url.split('?')[0];
  if (reqPath === '/' || reqPath === '') reqPath = '/index.html';
  const filePath = path.join(publicDir, reqPath);

  // Phân loại MIME Type
  const ext = path.extname(filePath).toLowerCase();
  const mimeTypes = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'application/javascript; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.json': 'application/json; charset=utf-8',
    '.png': 'image/png',
    '.jpg': 'image/jpeg',
    '.svg': 'image/svg+xml',
    '.ico': 'image/x-icon'
  };
  const contentType = mimeTypes[ext] || 'application/octet-stream';

  fs.readFile(filePath, (err, content) => {
    if (err) {
      if (err.code === 'ENOENT') {
        res.writeHead(404, { 'Content-Type': 'text/html; charset=utf-8' });
        res.end('<h1>404 Không tìm thấy file</h1><p>Hãy kiểm tra thư mục public/' + reqPath + '</p>');
      } else {
        res.writeHead(500);
        res.end('Lỗi máy chủ: ' + err.code);
      }
    } else {
      res.writeHead(200, { 'Content-Type': contentType });
      res.end(content);
    }
  });
});

// WebSocket Server (Port 9001) chạy trên cùng hoặc độc lập
const wsHttpServer = http.createServer();
ws.createServer({ server: wsHttpServer }, aedes.handle);

// TCP Server (Port 1883) dành cho ESP32
const tcpServer = net.createServer(aedes.handle);

// Khởi chạy đồng bộ các cổng (Lắng nghe cả IPv4 & IPv6)
tcpServer.listen(TCP_PORT, () => {
  wsHttpServer.listen(WS_PORT, () => {
    httpServer.listen(HTTP_PORT, () => {
      const ips = getLocalIPs();
      const primaryIP = ips.length > 0 ? ips[0].address : '127.0.0.1';
      const mobileUrl = `http://${primaryIP}:${HTTP_PORT}`;

      QRCode.toString(mobileUrl, { type: 'terminal', small: true }, (err, qrAscii) => {
        console.log('='.repeat(78));
        console.log('   🚀 MQTT BROKER + SQLITE + WEB DASHBOARD ĐÃ KHỞI CHẠY THÀNH CÔNG!');
        console.log('='.repeat(78));
        console.log(`[*] Cổng TCP (ESP32 kết nối MQTT)   : ${TCP_PORT}`);
        console.log(`[*] Cổng WebSocket (Web Dashboard)   : ${WS_PORT}`);
        console.log(`[*] Cổng Web HTTP (Mở trên Laptop)   : ${HTTP_PORT}`);
        console.log(`[*] Địa chỉ IP máy tính trong LAN   : ${primaryIP}`);
        console.log(`[*] Cơ sở dữ liệu SQLite             : ${dbPath}`);
        console.log('-'.repeat(78));
        console.log('🌐 ĐƯỜNG DẪN TRUY CẬP WEB DASHBOARD:');
        console.log(`   ➜ Trực tiếp trên Laptop           : http://localhost:${HTTP_PORT}`);
        console.log(`   ➜ Điện thoại cùng mạng Wi-Fi      : ${mobileUrl}`);
        console.log('-'.repeat(78));
        if (!err && qrAscii) {
          console.log('📱 MÃ QR DÀNH CHO ĐIỆN THOẠI (BẬT CAMERA QUÉT NGAY):');
          console.log(qrAscii);
          console.log('-'.repeat(78));
        }
        console.log('⚙️ ĐỊA CHỈ NHẬP VÀO TRANG CẤU HÌNH ESP32 (192.168.4.1):');
        console.log(`   ➜ Broker IP                       : ${primaryIP}`);
        console.log(`   ➜ Broker Port                     : ${TCP_PORT}`);
        console.log(`   ➜ MQTT User / Pass                : user1 / 123  (hoặc để trống)`);
        console.log('='.repeat(78));
        console.log('⏳ Đang lắng nghe dữ liệu từ ESP32 và Web Dashboard...\n');
      });
    });
  });
});

tcpServer.on('error', (err) => {
  if (err.code === 'EADDRINUSE') {
    console.error(`❌ Cổng ${TCP_PORT} đang bị chiếm dụng (có thể Mosquitto đang chạy)!`);
  } else {
    console.error('❌ Lỗi TCP Server:', err.message);
  }
});
