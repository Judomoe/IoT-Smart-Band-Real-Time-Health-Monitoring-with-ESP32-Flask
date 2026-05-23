# IoT Smart Band — Comprehensive Health Monitoring ⌚

A full-stack IoT project implementing a wearable smart band prototype that collects
real-time physiological data and visualizes it through a web dashboard.

**By:** Mohammed Mustafa Kamel, Moustafa Ahmed Mahmoud, Abduallah Mohamed Hussein  
**Course:** Microprocessor Systems — Arab Academy for Science & Technology

---

## System Architecture
[ESP32 Maquette]
├── MAX30105 → Heart Rate + SpO₂
├── MPU6050  → Steps + Temperature
└── SSD1306  → OLED Display (4 pages)
|
MQTT (HiveMQ broker)
|
[Flask Backend]
├── User auth (signup/login)
├── CSV data storage per user
└── Web Dashboard
├── dashboard.html  → raw data table
├── graphs.html     → Chart.js visualizations
└── fitness.html    → BMI & calorie calculator

---

## Hardware

| Component | Role |
|-----------|------|
| ESP32 Dev Module | Main microcontroller + Wi-Fi |
| MAX30105 | Heart rate (BPM) + SpO₂ |
| MPU6050 | Step counting + temperature |
| SSD1306 OLED (128×64) | Local display |
| Push button (GPIO 14) | Page navigation |

**I2C Bus:** SDA → GPIO 21, SCL → GPIO 22

### OLED Pages
| Page | Content |
|------|---------|
| 0 | Analog clock + temperature |
| 1 | IR waveform + BPM |
| 2 | Walking animation + step count |
| 3 | SpO₂ beaker fill animation |

---

## MQTT Topics

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `esp32/bruh/data` | ESP32 → Server | JSON health payload (every 5s) |
| `esp32/bruh/status` | ESP32 → Server | Online status |
| `watch/ids` | ESP32 → Server | Watch binding request |
| `watch/acks` | Server → ESP32 | Binding confirmation |

**Broker:** HiveMQ (`broker.hivemq.com:1883`)

---

## Flask Web App

### Features
- User signup linked to a Watch ID (verified via MQTT ACK)
- Per-user CSV data storage (`<watch_id>.csv`)
- Live dashboard with auto-refreshing Chart.js graphs
- Fitness calculator (BMI, calories burned, sleep, hydration)

### Routes
- `/` → Login
- `/signup` → Register with Watch ID
- `/dashboard/<id>` → Raw data table
- `/graphs/<id>` → Health trend graphs
- `/fitness` → Fitness calculator

---

## How to Run

### ESP32
1. Install libraries: `PubSubClient`, `Adafruit_MPU6050`, `MAX30105`, `ArduinoJson`, `Adafruit_SSD1306`
2. Flash `firmware/smart_band.ino` via Arduino IDE
3. Update Wi-Fi credentials if needed

### Flask Server
```bash
pip install flask paho-mqtt
python server/app.py
```
Then open: `http://localhost:5000`

---

## Project Structure
Smart-Band/
├── firmware/
│   └── smart_band.ino       # ESP32 firmware
├── server/
│   ├── app.py               # Flask backend
│   └── templates/
│       ├── login.html
│       ├── signup.html
│       ├── dashboard.html
│       ├── graphs.html
│       └── fitness.html
├── data/                    # Sample CSV files
└── README.md

## Future Work
- BLE support to replace Wi-Fi for power efficiency
- OTA firmware updates
- Database (InfluxDB/PostgreSQL) to replace CSV storage
- Mobile app (iOS/Android)
- TinyML for anomaly detection and health insights
- MQTT over TLS + password hashing for security
