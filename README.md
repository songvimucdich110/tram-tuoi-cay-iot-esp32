# Hệ thống Trạm Tưới Cây Tự Động Thông Minh Ứng Dụng Công Nghệ IoT

- **Học phần:** Kỹ thuật IoT (Mã học phần: `850324`)
- **Trường:** Đại học Sài Gòn (SGU) – Khoa Kỹ thuật và Công nghệ
- **Giảng viên hướng dẫn:** ThS. Trần Minh Nhật
- **Sinh viên thực hiện:** Nông Vĩnh Tường – **MSSV:** `3123510056` – **Lớp:** `DKD1231`

---

## 1. Tổ chức cây thư mục mã nguồn

```text
tram-tuoi-cay-iot-esp32/
├── README.md
├── esp32_tuoicay_iot/          # Phân hệ Firmware vi điều khiển ESP32 & mô phỏng Wokwi
│   ├── platformio.ini          # Cấu hình biên dịch PlatformIO & thư viện nhúng
│   ├── diagram.json            # Sơ đồ đấu nối mạch mô phỏng Wokwi Simulator
│   ├── wokwi.toml              # Liên kết tệp nhị phân Firmware với Wokwi
│   └── src/
│       └── main.cpp            # Mã nguồn chính trên ESP32-WROOM-32 (1.108 dòng)
└── mqtt_broker/                # Phân hệ Máy chủ trung tâm Node.js & Web Dashboard
    ├── package.json            # Khai báo các gói thư viện Node.js (aedes, ws, mysql2, sqlite3)
    ├── broker.js               # Máy chủ Aedes MQTT Broker (1883/9001), HTTP (3000) & CSDL kép (765 dòng)
    └── public/
        └── index.html          # Giao diện Web Dashboard giám sát & điều khiển thời gian thực (3.160 dòng)
```

---

## 2. Hướng dẫn cài đặt và vận hành nhanh

### Bước 1: Khởi chạy Máy chủ Node.js & Web Dashboard
```bash
cd mqtt_broker
npm install
node broker.js
```
- **MQTT Broker TCP (ESP32):** cổng `1883` (Tài khoản mặc định: `user1` / `123`)
- **MQTT over WebSocket (Web):** cổng `9001`
- **Giao diện Web Dashboard:** truy cập `http://localhost:3000`

### Bước 2: Biên dịch và nạp Firmware cho ESP32 (hoặc mô phỏng Wokwi)
1. Mở thư mục `esp32_tuoicay_iot` bằng **Visual Studio Code** (cài đặt tiện ích **PlatformIO IDE** và **Wokwi Simulator**).
2. Biên dịch và nạp xuống bo mạch ESP32 thật qua cáp USB (`COM`):
   ```bash
   pio run -t upload
   ```
3. Ở lần cấp nguồn đầu tiên (hoặc sau khi nhấn giữ phím cứng `MODE` 5 giây để xóa cấu hình `EEPROM`), kết nối vào mạng Wi-Fi phát ra từ mạch `ESP32_TuoiCay_IoT` (mật khẩu `12345678`) và mở `http://192.168.4.1` để cấu hình Wi-Fi & địa chỉ IP máy chủ MQTT.
