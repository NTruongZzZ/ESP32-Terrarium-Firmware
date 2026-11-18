#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>        // Watchdog Timer
#include <Preferences.h>         // Bộ nhớ Flash NVS lưu cài đặt
#include <OneWire.h>             // Thư viện OneWire cho cảm biến DS18B20
#include <DallasTemperature.h>   // Thư viện DallasTemperature
#include <time.h>                // Thư viện thời gian thực (NTP)
#include <esp_system.h>          // Kiểm tra lý do reset chip
#include <HTTPClient.h>          // Gửi Heartbeat HTTP tới Healthchecks.io

// ==================== CẤU HÌNH WiFi ====================
// WiFi chính
const char* ssid_main        = "Maya";       // <<< Đổi tên WiFi chính
const char* password_main    = "Phuloi1967@";        // <<< Đổi mật khẩu WiFi chính

// WiFi phụ (dự phòng khi mất kết nối WiFi chính)
const char* ssid_backup      = "Nhat Truong";           // <<< Đổi tên WiFi phụ
const char* password_backup  = "1234567890";      // <<< Đổi mật khẩu WiFi phụ

// ==================== CẤU HÌNH TELEGRAM ====================
#define BOT_TOKEN "8849643501:AAHL06ZDqYuafUT7Fk9BrA1gg4yzpYRHx6Y"
#define CHAT_ID   "8941867121"

WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);
Preferences prefs;

// ==================== CHÂN PIN (ESP32-C3 SuperMini) ====================
#define BATT_ADC         1   // ADC đọc điện áp pin
#define DC_ADC           2   // ADC đọc điện áp DC
#define TEMP_PIN         3   // Chân Data đọc nhiệt độ DS18B20 (1-Wire)
#define TEMP_ADC         TEMP_PIN // Giữ lại tên cũ để tương thích

#define LED1_PIN         20  // Đèn 1
#define LED2_PIN         21  // Đèn 2
#define LED3_PIN         10  // Đèn 3
#define PUMP_FILTER_PIN  4   // Bơm lọc
#define OXY_PIN          8   // Oxy (sục khí)
#define FAN_PIN          5   // Quạt

// Khởi tạo đối tượng giao tiếp OneWire và cảm biến DS18B20
OneWire oneWire(TEMP_PIN);
DallasTemperature sensors(&oneWire);

// ==================== NGƯỠNG & HẰNG SỐ ====================
// Điện áp pin 3S thực tế: 9.0V = 0%, 12.6V = 100%
#define BATT_MIN_V       9.0f   // 0% (3.0V/cell x 3, ngưỡng cạn bảo vệ pin)
#define BATT_MAX_V       12.6f  // 100% (4.2V/cell x 3, pin đầy)

// Ngưỡng cảnh báo pin (%)
#define BATT_LOW_PERCENT   10   // Cảnh báo pin rất yếu (<10%)
#define BATT_WARN_PERCENT  20   // Cảnh báo pin yếu (20%)
#define BATT_FULL_PERCENT  99   // Thông báo đầy pin

// Oxy cố định ở mức 20% công suất
#define OXY_FIXED_PERCENT  20

// Ngưỡng phát hiện có nguồn DC (ADC raw) — điều chỉnh theo mạch chia áp thực tế
#define DC_THRESHOLD     2900

// ADC ESP32-C3: 12-bit (0–4095), Vref đo đạc ~2.96V
// Giảm áp từ 12.6V xuống 3.1V => Hệ số chia áp = 4.0645
#define ADC_RESOLUTION   4095.0f
#define ADC_VREF         2.95f
#define VOLTAGE_DIVIDER_RATIO 5.4124f  // Hệ số cầu chia áp pin 3S

// ==================== CẤU HÌNH THEO DÕI NGUỒN & PIN KHI MẤT ĐIỆN ====================
#define MAX_OUTAGE_LOGS         20       // Lưu tối đa 20 lần mất điện gần nhất
#define CHARGE_TARGET_V         12.0f    // Điện áp tính sạc đầy (12.0V)
#define CHARGE_CURRENT_A        1.0f     // Dòng sạc cố định 1A
#define BATT_CAPACITY_MAH       10000.0f // Dung lượng pin 3S (10000mAh)

// Cấu hình thời gian NTP
#define NTP_SERVER_1            "pool.ntp.org"
#define NTP_SERVER_2            "time.google.com"
#define GMT_OFFSET_SEC          (7 * 3600) // Múi giờ Việt Nam UTC+7
#define DAYLIGHT_OFFSET         0

struct OutageRecord {
  uint16_t year;          // Năm mất điện (vd: 2026)
  uint8_t  month;         // Tháng mất điện (1-12)
  uint8_t  day;           // Ngày mất điện (1-31)
  uint8_t  start_h;       // Giờ bắt đầu mất điện
  uint8_t  start_m;       // Phút bắt đầu mất điện
  uint8_t  start_s;       // Giây bắt đầu mất điện
  uint8_t  end_h;         // Giờ có điện lại
  uint8_t  end_m;         // Phút có điện lại
  uint8_t  end_s;         // Giây có điện lại
  uint32_t duration_sec;  // Tổng số giây mất điện
  float    start_v;       // Pin khi vừa mất DC
  int      start_pct;     // % pin khi vừa mất DC
  float    end_v;         // Pin trước khi có DC
  int      end_pct;       // % pin trước khi có DC
};

OutageRecord outage_logs[MAX_OUTAGE_LOGS];
int outage_log_count = 0;

// Trạng thái theo dõi mất điện hiện tại
bool          outage_in_progress    = false;
unsigned long outage_start_ms       = 0;
time_t        outage_start_epoch    = 0;
uint16_t      outage_start_year     = 0;
uint8_t       outage_start_month    = 0;
uint8_t       outage_start_day      = 0;
uint8_t       outage_start_h        = 0;
uint8_t       outage_start_m        = 0;
uint8_t       outage_start_s        = 0;
float         outage_start_v        = 0.0f;
int           outage_start_pct      = 0;
float         outage_last_v         = 0.0f;
int           outage_last_pct       = 0;

// Cờ và nội dung thông báo có điện lại (chờ kết nối WiFi thành công mới gửi)
bool pending_restore_notify = false;
String restore_notify_msg   = "";

// ==================== CẤU HÌNH THEO DÕI LỖI RESET (20 LẦN GẦN NHẤT) ====================
#define MAX_ERROR_LOGS 20
struct ErrorRecord {
  uint8_t  day;          // Ngày (1-31)
  uint8_t  month;        // Tháng (1-12)
  uint16_t reason_code;  // Mã esp_reset_reason_t
  uint32_t uptime_sec;   // Số giây đã chạy trước khi reset
};
ErrorRecord error_logs[MAX_ERROR_LOGS];
int error_log_count = 0;

// Bộ nhớ RTC Slow RAM: giữ nguyên giá trị qua các lần reset (Brownout, Watchdog, Crash, SW reset)
#define RTC_MAGIC_KEY 0x54455252 // 'TERR'
RTC_DATA_ATTR uint32_t rtc_uptime_sec = 0;
RTC_DATA_ATTR uint8_t  rtc_day        = 0;
RTC_DATA_ATTR uint8_t  rtc_month      = 0;
RTC_DATA_ATTR uint32_t rtc_magic      = 0;

// ==================== THỜI GIAN ====================
#define BOT_CHECK_INTERVAL    1000   // Kiểm tra Telegram mỗi 1 giây
#define BATT_CHECK_INTERVAL   5000   // Đọc pin mỗi 5 giây
#define DC_CHECK_INTERVAL     500    // Kiểm tra DC mỗi 0.5 giây
#define TEMP_CHECK_INTERVAL   5000   // Đọc nhiệt độ mỗi 5 giây
// Chống spam thông báo: gửi tối đa 1 lần mỗi 5 phút cho cùng sự kiện
#define NOTIFY_COOLDOWN       300000UL
// Watchdog: tự khởi động lại nếu loop() bị treo quá 30 giây
#define WDT_TIMEOUT_S         30

// ==================== CẤU HÌNH HEARTBEAT (Healthchecks.io) ====================
#define HEARTBEAT_URL            "https://hc-ping.com/544b454e-2137-4d29-9a9f-624b767e43a3"
#define HEARTBEAT_INTERVAL_MS    60000UL  // Chu kỳ gửi định kỳ 60 giây khi thành công
#define HEARTBEAT_RETRY_MS       15000UL  // Chu kỳ thử lại 15 giây khi gửi thất bại hoặc mất mạng
#define HEARTBEAT_TIMEOUT_MS     2500     // Timeout HTTP 2.5 giây hạn chế ảnh hưởng motor, quạt, oxy

// ==================== BIẾN TRẠNG THÁI ====================
// Mức PWM (phần trăm 0–100) cho từng output
int led1_percent = 0;
int led2_percent = 0;
int led3_percent = 0;
int led3_outage_percent = 30; // Đèn 3 khi mất DC trong khung 17h30–5h30 VN (mặc định 30%)
int pump_percent = 100;   // Mức mặc định khi bơm chạy (có DC)
int fan_percent  = 20;    // Mức quạt tự do khi nhiệt độ < 28°C

// Cấu hình quạt tự động theo nhiệt độ (thay đổi được và có nhớ)
int fan_auto_28  = 30;    // Từ 28°C đến < 29°C (mặc định 30%)
int fan_auto_29  = 50;    // Từ 29°C đến <= 30°C (mặc định 50%)
int fan_auto_30  = 100;   // Trên 30°C (mặc định 100%)
int fan_auto_err = 30;    // Khi cảm biến lỗi giá trị (mặc định 30%)

// Oxy mặc định 20%, có thể chỉnh qua Telegram
int oxy_percent = OXY_FIXED_PERCENT;

// Trạng thái tự động
bool dc_available     = false;
bool pump_running     = false;
bool oxy_running      = false;
bool led_running      = false;

// Pin
float batt_voltage    = 0.0f;
int   batt_percent    = 0;

// Nhiệt độ DS18B20 (hỗ trợ nhiều cảm biến song song)
#define MAX_TEMP_SENSORS 8
float current_temperatures[MAX_TEMP_SENSORS];
int   temp_sensor_count   = 0;
float current_temperature = -127.0f; // Giữ lại tương thích cho cảm biến 1

// Thời gian
unsigned long lastBotCheck  = 0;
unsigned long lastBattCheck = 0;
unsigned long lastDCCheck   = 0;
unsigned long lastTempCheck = 0;
unsigned long lastHeartbeatAttempt = 0;
unsigned long heartbeatInterval    = 0; // Bắt đầu gửi ngay sau khi khởi động và có WiFi

// Chống spam thông báo
unsigned long lastLowBattNotify  = 0;
unsigned long lastWarnBattNotify = 0;
unsigned long lastFullBattNotify = 0;
unsigned long lastDCChangeNotify = 0;
bool prev_dc_available = false;

// ==================== LƯU & ĐỌC CÀI ĐẶT FLASH (NVS) ====================
void saveSetting(const char* key, int val) {
  prefs.begin("aquarium", false);
  prefs.putInt(key, val);
  prefs.end();
}

void loadSettings() {
  prefs.begin("aquarium", true); // Mở chế độ Read-only
  led1_percent        = prefs.getInt("led1", 0);
  led2_percent        = prefs.getInt("led2", 0);
  led3_percent        = prefs.getInt("led3", 0);
  led3_outage_percent = prefs.getInt("led3_outage", 30);
  fan_percent         = prefs.getInt("fan", 20);
  fan_auto_28         = prefs.getInt("fan_a28", 30);
  fan_auto_29         = prefs.getInt("fan_a29", 50);
  fan_auto_30         = prefs.getInt("fan_a30", 100);
  fan_auto_err        = prefs.getInt("fan_aerr", 30);
  pump_percent        = prefs.getInt("pump", 100);
  oxy_percent         = prefs.getInt("oxy", OXY_FIXED_PERCENT);
  prefs.end();
}

// ==================== HÀM TIỆN ÍCH MÀU SẮC (CỤC MÀU EMOJI) ====================
// Pin: > 30% màu xanh lá 🟢, <= 30% màu đỏ 🔴
String getBattColorIcon(int pct) {
  return (pct > 30) ? "🟢" : "🔴";
}

// Nhiệt độ: < 28°C màu xanh 🟢, 28 - 30°C màu vàng 🟡, > 30°C màu đỏ 🔴, lỗi ⚠️
String getTempColorIcon(float temp) {
  if (temp < -50.0f || temp > 125.0f || temp == 85.0f || temp == DEVICE_DISCONNECTED_C) {
    return "⚠️";
  }
  if (temp < 28.0f) {
    return "🟢";
  } else if (temp <= 30.0f) {
    return "🟡";
  } else {
    return "🔴";
  }
}

// Hàm lấy ngày, tháng, năm hiện tại qua hệ thống thời gian thực (NTP)
void getCurrentDate(uint8_t &d, uint8_t &m, uint16_t &y) {
  time_t now_sec;
  time(&now_sec);
  if (now_sec > 1600000000UL) { // Đã đồng bộ NTP
    struct tm timeinfo;
    localtime_r(&now_sec, &timeinfo);
    d = (uint8_t)timeinfo.tm_mday;
    m = (uint8_t)(timeinfo.tm_mon + 1);
    y = (uint16_t)(timeinfo.tm_year + 1900);
  } else {
    d = 0;
    m = 0;
    y = 0;
  }
}

void getCurrentDate(uint8_t &d, uint8_t &m) {
  uint16_t y = 0;
  getCurrentDate(d, m, y);
}

// Lưu & Đọc lịch sử 20 lần mất điện gần nhất trong Flash NVS
void saveOutageLogs() {
  prefs.begin("outage_logs_v3", false);
  prefs.putInt("count", outage_log_count);
  prefs.putBytes("logs", outage_logs, sizeof(outage_logs));
  prefs.end();
}

void loadOutageLogs() {
  prefs.begin("outage_logs_v3", true);
  outage_log_count = prefs.getInt("count", 0);
  if (outage_log_count < 0 || outage_log_count > MAX_OUTAGE_LOGS) {
    outage_log_count = 0;
  }
  if (outage_log_count > 0) {
    prefs.getBytes("logs", outage_logs, sizeof(outage_logs));
  }
  prefs.end();
}

void addOutageLog(uint16_t year, uint8_t month, uint8_t day,
                  uint8_t sh, uint8_t sm, uint8_t ss,
                  uint8_t eh, uint8_t em, uint8_t es,
                  uint32_t duration_sec,
                  float start_v, int start_pct,
                  float end_v, int end_pct) {
  int shift_count = (outage_log_count < MAX_OUTAGE_LOGS) ? outage_log_count : (MAX_OUTAGE_LOGS - 1);
  for (int i = shift_count; i > 0; i--) {
    outage_logs[i] = outage_logs[i - 1];
  }
  outage_logs[0].year         = year;
  outage_logs[0].month        = month;
  outage_logs[0].day          = day;
  outage_logs[0].start_h      = sh;
  outage_logs[0].start_m      = sm;
  outage_logs[0].start_s      = ss;
  outage_logs[0].end_h        = eh;
  outage_logs[0].end_m        = em;
  outage_logs[0].end_s        = es;
  outage_logs[0].duration_sec = duration_sec;
  outage_logs[0].start_v      = start_v;
  outage_logs[0].start_pct    = start_pct;
  outage_logs[0].end_v        = end_v;
  outage_logs[0].end_pct      = end_pct;

  if (outage_log_count < MAX_OUTAGE_LOGS) {
    outage_log_count++;
  }
  saveOutageLogs();
}

// ==================== XỬ LÝ NHẬT KÝ LỖI RESET ESP32 ====================
String getResetReasonName(uint16_t reason) {
  switch (reason) {
    case ESP_RST_BROWNOUT:  return "Sụt áp nguồn (Brownout)";
    case ESP_RST_TASK_WDT:  return "Treo tác vụ (Task WDT >30s)";
    case ESP_RST_INT_WDT:   return "Treo ngắt (Interrupt WDT)";
    case ESP_RST_WDT:       return "Watchdog Timer (RTC/Other)";
    case ESP_RST_PANIC:     return "Lỗi phần mềm (Crash/Panic)";
    case ESP_RST_SW:        return "Khởi động lại phần mềm (SW Reset)";
    case ESP_RST_POWERON:   return "Mất nguồn đột ngột bật lại (Power-on)";
    case ESP_RST_EXT:       return "Nút Reset / Chân EN ngoài";
    case ESP_RST_DEEPSLEEP: return "Thức dậy từ Deep Sleep";
    case ESP_RST_SDIO:      return "Reset qua SDIO";
    default:                return "Không xác định (" + String(reason) + ")";
  }
}

void saveErrorLogs() {
  prefs.begin("error_logs", false);
  prefs.putInt("count", error_log_count);
  prefs.putBytes("logs", error_logs, sizeof(error_logs));
  prefs.end();
}

void loadErrorLogs() {
  prefs.begin("error_logs", true);
  error_log_count = prefs.getInt("count", 0);
  if (error_log_count < 0 || error_log_count > MAX_ERROR_LOGS) {
    error_log_count = 0;
  }
  if (error_log_count > 0) {
    prefs.getBytes("logs", error_logs, sizeof(error_logs));
  }
  prefs.end();
}

void addErrorLog(uint8_t day, uint8_t month, uint16_t reason, uint32_t uptime_sec) {
  int shift = (error_log_count < MAX_ERROR_LOGS) ? error_log_count : (MAX_ERROR_LOGS - 1);
  for (int i = shift; i > 0; i--) {
    error_logs[i] = error_logs[i - 1];
  }
  error_logs[0].day         = day;
  error_logs[0].month       = month;
  error_logs[0].reason_code = reason;
  error_logs[0].uptime_sec  = uptime_sec;

  if (error_log_count < MAX_ERROR_LOGS) {
    error_log_count++;
  }
  saveErrorLogs();
}

// ==================== HÀM TÍNH THỜI GIAN SẠC ĐẦY (PHÚT) ĐẾN 12V ====================
// Dòng sạc cố định 1A (1000mA), Pin 3S dung lượng 10000mAh, tính sạc đến 12.0V
int calculateChargeTimeMinutes(float current_v) {
  if (current_v >= CHARGE_TARGET_V) return 0;
  if (current_v < BATT_MIN_V) current_v = BATT_MIN_V;

  // % pin mục tiêu tại 12V và % pin hiện tại (theo thang BATT_MIN_V 9.0V - BATT_MAX_V 12.6V)
  float target_pct = ((CHARGE_TARGET_V - BATT_MIN_V) / (BATT_MAX_V - BATT_MIN_V)) * 100.0f;
  float current_pct = ((current_v - BATT_MIN_V) / (BATT_MAX_V - BATT_MIN_V)) * 100.0f;
  float rem_pct = target_pct - current_pct;
  if (rem_pct <= 0.0f) return 0;

  // Dung lượng mAh cần sạc thêm để đạt 12.0V
  float rem_mah = (rem_pct / 100.0f) * BATT_CAPACITY_MAH;

  // Dòng sạc 1A (1000mA): thời gian (giờ) = rem_mah / 1000 => đổi ra phút (* 60)
  float hours = rem_mah / (CHARGE_CURRENT_A * 1000.0f);
  int minutes = (int)(hours * 60.0f + 0.5f);
  return (minutes < 0) ? 0 : minutes;
}

// ==================== HÀM XUẤT PWM ====================
void setPWM(int pin, int percent) {
  analogWriteResolution(pin, 8);
  analogWriteFrequency(pin, 25000); // PWM 25 kHz

  int duty = map(constrain(percent, 0, 100), 0, 100, 0, 255);
  analogWrite(pin, duty);
}

// ==================== HÀM KIỂM TRA KHUNG GIỜ BAN ĐÊM (17h30 - 5h30 VN) ====================
bool isNightTimeVN() {
  time_t now_sec;
  time(&now_sec);
  if (now_sec < 1600000000UL) {
    // Chưa đồng bộ NTP -> mặc định cho phép bật để an toàn
    return true;
  }
  struct tm ti;
  localtime_r(&now_sec, &ti);
  int h = ti.tm_hour;
  int m = ti.tm_min;

  // Từ 17h30 đến 23h59:
  if (h > 17 || (h == 17 && m >= 30)) return true;
  // Từ 00h00 đến 05h30:
  if (h < 5  || (h == 5  && m <= 30)) return true;

  return false;
}

// ==================== HÀM CẬP NHẬT ĐÈN THEO NGUỒN ĐIỆN & KHUNG GIỜ ====================
// Khi có điện: Đèn 1, 2, 3 bật theo % cài đặt của người dùng.
// Khi mất điện: Đèn 1, 2 luôn tắt (tiết kiệm pin).
//               Đèn 3: sáng 30% (hoặc mức led3_outage_percent) nếu trong khoảng 17h30–5h30 VN; ngoài giờ luôn tắt.
void updateLEDs() {
  if (dc_available) {
    setPWM(LED1_PIN, led1_percent);
    setPWM(LED2_PIN, led2_percent);
    setPWM(LED3_PIN, led3_percent);
  } else {
    setPWM(LED1_PIN, 0);
    setPWM(LED2_PIN, 0);
    if (isNightTimeVN()) {
      setPWM(LED3_PIN, led3_outage_percent);
    } else {
      setPWM(LED3_PIN, 0);
    }
  }
}

// ==================== HÀM ĐIỀU KHIỂN QUẠT THEO NHIỆT ĐỘ ====================
// Dưới 28 độ: cho tự do điều khiển (fan_percent).
// Từ 28 đến < 29 độ: buộc chạy fan_auto_28 (mặc định 30%).
// Từ 29 đến <= 30 độ: buộc chạy fan_auto_29 (mặc định 50%).
// Trên 30 độ: buộc chạy fan_auto_30 (mặc định 100%).
// Lỗi giá trị: buộc chạy fan_auto_err (mặc định 30%).
int getRequiredFanPercent() {
  float max_temp = -127.0f;
  bool has_valid = false;
  for (int i = 0; i < temp_sensor_count; i++) {
    float t = current_temperatures[i];
    if (t != DEVICE_DISCONNECTED_C && t > -50.0f && t < 125.0f && t != 85.0f) {
      if (!has_valid || t > max_temp) {
        max_temp = t;
        has_valid = true;
      }
    }
  }

  // Case bị lỗi giá trị hoặc không có cảm biến
  if (!has_valid) {
    return fan_auto_err;
  }

  if (max_temp < 28.0f) {
    return fan_percent; // Tự do điều khiển
  } else if (max_temp < 29.0f) {
    return fan_auto_28;  // Buộc chạy 30%
  } else if (max_temp <= 30.0f) {
    return fan_auto_29;  // Buộc chạy 50%
  } else {
    return fan_auto_30;  // Buộc chạy 100%
  }
}

void updateFanSpeed() {
  int target = getRequiredFanPercent();
  setPWM(FAN_PIN, target);
}

// ==================== HÀM PHÂN TÍCH SỐ % TỪ LỆNH ====================
// Trích số phần trăm từ chuỗi lệnh, vd: "/led1 50" → 50, "/led1" → -1
int parsePercent(const String& text) {
  int lastSpace = text.lastIndexOf(' ');
  if (lastSpace < 0) return -1;
  String numStr = text.substring(lastSpace + 1);
  numStr.trim();
  if (numStr.length() == 0) return -1;
  for (unsigned int i = 0; i < numStr.length(); i++) {
    if (!isDigit(numStr.charAt(i))) return -1;
  }
  int val = numStr.toInt();
  return constrain(val, 0, 100);
}

// ==================== HÀM ĐỌC ĐIỆN ÁP PIN ====================
float readBatteryVoltage() {
  long sum = 0;
  for (int i = 0; i < 16; i++) {
    sum += analogRead(BATT_ADC);
    delayMicroseconds(50);
  }
  float raw = sum / 16.0f;
  float voltage = (raw / ADC_RESOLUTION) * ADC_VREF * VOLTAGE_DIVIDER_RATIO;
  return voltage;
}

// ==================== HÀM TÍNH % PIN ====================
int batteryPercent(float voltage) {
  if (voltage <= BATT_MIN_V) return 0;
  if (voltage >= BATT_MAX_V) return 100;
  return (int)(((voltage - BATT_MIN_V) / (BATT_MAX_V - BATT_MIN_V)) * 100.0f);
}

// ==================== HÀM KIỂM TRA NGUỒN DC ====================
bool isDCAvailable() {
  long sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += analogRead(DC_ADC);
    delayMicroseconds(50);
  }
  int raw = sum / 8;
  return (raw > DC_THRESHOLD);
}

// ==================== HÀM ĐỌC NHIỆT ĐỘ DS18B20 ====================
void readTemperature() {
  temp_sensor_count = sensors.getDeviceCount();
  if (temp_sensor_count <= 0) {
    current_temperature = -127.0f;
    temp_sensor_count = 0;
    sensors.requestTemperatures();
    return;
  }
  if (temp_sensor_count > MAX_TEMP_SENSORS) {
    temp_sensor_count = MAX_TEMP_SENSORS;
  }

  for (int i = 0; i < temp_sensor_count; i++) {
    float temp = sensors.getTempCByIndex(i);
    if (temp != DEVICE_DISCONNECTED_C &&
        temp > -50.0f && temp < 125.0f && temp != 85.0f) {
      current_temperatures[i] = temp;
    } else {
      current_temperatures[i] = -127.0f;
    }
  }
  current_temperature = current_temperatures[0];

  // Bắt đầu lần chuyển đổi kế tiếp; interval 5 giây đủ cho 12-bit (750 ms)
  sensors.requestTemperatures();
}

// Hàm hiển thị danh sách nhiệt độ theo thứ tự thiết bị DS18B20 kèm icon màu
String buildTemperatureStatus() {
  if (temp_sensor_count <= 0) {
    return "🌡️ Nhiệt độ: Lỗi / Chưa cắm ⚠️\n";
  }
  if (temp_sensor_count == 1) {
    float t = current_temperatures[0];
    if (t > -50.0f && t < 125.0f && t != 85.0f) {
      return "🌡️ Nhiệt độ: " + String(t, 1) + "°C " + getTempColorIcon(t) + "\n";
    } else {
      return "🌡️ Nhiệt độ: Lỗi / Chưa cắm ⚠️\n";
    }
  }
  // Nhiều thiết bị song song: hiển thị theo thứ tự
  String s = "";
  for (int i = 0; i < temp_sensor_count; i++) {
    float t = current_temperatures[i];
    s += "🌡️ Nhiệt độ " + String(i + 1) + ": ";
    if (t > -50.0f && t < 125.0f && t != 85.0f) {
      s += String(t, 1) + "°C " + getTempColorIcon(t) + "\n";
    } else {
      s += "Lỗi / Mất kết nối ⚠️\n";
    }
  }
  return s;
}

// ==================== HÀM ĐIỀU KHIỂN BƠM & OXY (TỰ ĐỘNG) ====================
void updatePumpOxy() {
  if (dc_available) {
    // Có điện DC → Bơm lọc chạy theo mức đã đặt, tắt oxy
    setPWM(PUMP_FILTER_PIN, pump_percent);
    setPWM(OXY_PIN, 0);
    pump_running = true;
    oxy_running  = false;
  } else {
    // Mất điện DC → Tắt bơm, oxy chạy cố định 20% (backup pin)
    setPWM(PUMP_FILTER_PIN, 0);
    setPWM(OXY_PIN, oxy_percent);
    pump_running = false;
    oxy_running  = true;
  }
}

// ==================== HÀM GỬI THÔNG BÁO TELEGRAM ====================
void sendTelegramNotify(const String& message) {
  bot.sendMessage(CHAT_ID, message, "");
}

// ==================== HÀM TẠO TIN NHẮN TRẠNG THÁI ====================
String buildStatusMessage() {
  String msg = "🐟 *TRẠNG THÁI HỆ THỐNG*\n";
  msg += "━━━━━━━━━━━━━━━━━━━━\n";
  msg += "📶 WiFi: " + (WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "Mất kết nối ❌") + "\n";
  msg += "🔋 Pin: " + String(batt_voltage, 2) + "V (" + String(batt_percent) + "% " + getBattColorIcon(batt_percent) + ")\n";
  msg += "⚡ Nguồn DC: " + String(dc_available ? "Có ✅" : "Mất ❌") + "\n";
  if (!dc_available && outage_in_progress) {
    uint32_t cur_sec = (millis() - outage_start_ms) / 1000UL;
    int ch = cur_sec / 3600;
    int cm = (cur_sec % 3600) / 60;
    int cs = cur_sec % 60;
    char durBuf[32];
    snprintf(durBuf, sizeof(durBuf), "%02d : %02d : %02d", ch, cm, cs);
    msg += "⏱️ Đã mất điện: " + String(durBuf) + "\n";
  }
  msg += "🔎 DS18B20: " + String(temp_sensor_count) + " thiết bị\n";
  msg += buildTemperatureStatus();
  msg += "━━━━━━━━━━━━━━━━━━━━\n";
  msg += "🔵 Bơm lọc: " + String(pump_running ? String(pump_percent) + "% ✅" : "Tắt ❌") + "\n";
  msg += "🫧 Oxy: " + String(oxy_running ? String(oxy_percent) + "% ✅" : "Tắt ❌") + " (" + String(oxy_percent) + "%)\n";
  msg += "━━━━━━━━━━━━━━━━━━━━\n";
  if (dc_available) {
    msg += "💡 Đèn 1: " + String(led1_percent > 0 ? String(led1_percent) + "%" : "Tắt ❌") + "\n";
    msg += "💡 Đèn 2: " + String(led2_percent > 0 ? String(led2_percent) + "%" : "Tắt ❌") + "\n";
    msg += "💡 Đèn 3: " + String(led3_percent > 0 ? String(led3_percent) + "%" : "Tắt ❌") + "\n";
  } else {
    msg += "💡 Đèn 1: Tắt ❌ (mất điện" + String(led1_percent > 0 ? ", lưu " + String(led1_percent) + "%)" : ")") + "\n";
    msg += "💡 Đèn 2: Tắt ❌ (mất điện" + String(led2_percent > 0 ? ", lưu " + String(led2_percent) + "%)" : ")") + "\n";
    if (isNightTimeVN()) {
      msg += "💡 Đèn 3: " + String(led3_outage_percent) + "% ✅ (17h30–5h30)\n";
    } else {
      msg += "💡 Đèn 3: Tắt ❌ (ngoài giờ 17h30–5h30, lưu " + String(led3_outage_percent) + "%)\n";
    }
  }
  int actual_fan = getRequiredFanPercent();
  msg += "🌀 Quạt: " + String(actual_fan > 0 ? String(actual_fan) + "% ✅" : "Tắt ❌");
  if (actual_fan != fan_percent) {
    msg += " (Tự động theo nhiệt độ | Tự do: " + String(fan_percent) + "%)\n";
  } else {
    msg += " (Tự do: " + String(fan_percent) + "%)\n";
  }
  return msg;
}

// ==================== HÀM KẾT NỐI WIFI (CÓ QUÉT DỰ PHÒNG) ====================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  Serial.print("📶 Đang kết nối WiFi chính: ");
  Serial.println(ssid_main);

  WiFi.begin(ssid_main, password_main);
  int wifi_timeout = 0;
  while (WiFi.status() != WL_CONNECTED && wifi_timeout < 20) { // Đợi ~10 giây
    delay(500);
    Serial.print(".");
    wifi_timeout++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Đã kết nối WiFi chính!");
    Serial.print("   SSID: ");
    Serial.println(WiFi.SSID());
    Serial.print("   IP: ");
    Serial.println(WiFi.localIP());
    return;
  }

  // Không kết nối được WiFi chính -> Quét WiFi xung quanh để kết nối mạng phụ
  Serial.println("\n⚠️ Không kết nối được WiFi chính!");
  Serial.println("🔍 Đang quét WiFi xung quanh để tìm mạng dự phòng...");
  int n = WiFi.scanNetworks();
  Serial.printf("🔍 Quét thấy %d mạng WiFi.\n", n);

  bool found_backup = false;
  for (int i = 0; i < n; i++) {
    Serial.printf("   [%d] %s (%d dBm)\n", i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i));
    if (WiFi.SSID(i) == ssid_backup) {
      found_backup = true;
    }
  }

  if (found_backup) {
    Serial.print("📶 Tìm thấy WiFi phụ: ");
    Serial.println(ssid_backup);
  } else {
    Serial.print("📶 Thử kết nối trực tiếp WiFi phụ: ");
    Serial.println(ssid_backup);
  }

  WiFi.disconnect();
  WiFi.begin(ssid_backup, password_backup);
  wifi_timeout = 0;
  while (WiFi.status() != WL_CONNECTED && wifi_timeout < 20) {
    delay(500);
    Serial.print(".");
    wifi_timeout++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Đã kết nối WiFi phụ thành công!");
    Serial.print("   SSID: ");
    Serial.println(WiFi.SSID());
    Serial.print("   IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n❌ Không kết nối được cả 2 WiFi! Bot Telegram sẽ không hoạt động.");
  }
}

// ==================== XỬ LÝ LỆNH TELEGRAM ====================
void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = bot.messages[i].chat_id;
    String text    = bot.messages[i].text;

    // Chỉ phản hồi đúng chat_id của chủ
    if (chat_id != CHAT_ID) {
      bot.sendMessage(chat_id, "⛔ Bạn không có quyền sử dụng bot này.", "");
      continue;
    }

    text.trim();
    text.toLowerCase();

    // Telegram group/bot command may arrive as /status@botname.
    // Remove only the @botname part, preserving any command arguments.
    int atPos = text.indexOf('@');
    if (atPos >= 0) {
      int spacePos = text.indexOf(' ', atPos);
      if (spacePos >= 0) {
        text.remove(atPos, spacePos - atPos);
      } else {
        text.remove(atPos);
      }
    }

    // ---------- LỆNH TRẠNG THÁI ----------
    // Send as plain text to avoid Markdown parsing failures from dynamic values.
    if (text == "/start" || text == "/status" || text == "status") {
      bot.sendMessage(CHAT_ID, buildStatusMessage(), "");
    }
    // ---------- LỆNH ĐỌC NHIỆT ĐỘ DS18B20 ----------
    else if (text == "/temp" || text == "/nhietdo" || text == "temp") {
      String msg = "🌡️ *THÔNG TIN NHIỆT ĐỘ*\n";
      msg += "━━━━━━━━━━━━━━━━━━━━\n";
      msg += "DS18B20 phát hiện: " + String(temp_sensor_count) + " thiết bị\n";
      if (temp_sensor_count <= 0) {
        msg += "⚠️ Cảm biến DS18B20 không phản hồi hoặc chưa cắm.";
      } else if (temp_sensor_count == 1) {
        float t = current_temperatures[0];
        if (t > -50.0f && t < 125.0f && t != 85.0f) {
          msg += "Nhiệt độ nước: *" + String(t, 1) + "°C* " + getTempColorIcon(t);
        } else {
          msg += "⚠️ Cảm biến DS18B20 không phản hồi.";
        }
      } else {
        for (int i = 0; i < temp_sensor_count; i++) {
          float t = current_temperatures[i];
          msg += "Cảm biến " + String(i + 1) + ": ";
          if (t > -50.0f && t < 125.0f && t != 85.0f) {
            msg += "*" + String(t, 1) + "°C* " + getTempColorIcon(t) + "\n";
          } else {
            msg += "Lỗi / Mất kết nối ⚠️\n";
          }
        }
      }
      bot.sendMessage(CHAT_ID, msg, "Markdown");
    }
    // ---------- ĐIỀU KHIỂN ĐÈN 1 (PWM %) ----------
    else if (text.startsWith("/led1") || text.startsWith("led1")) {
      int val = parsePercent(text);
      if (val < 0) {
        if (text.endsWith("on"))  val = 100;
        else if (text.endsWith("off")) val = 0;
      }
      if (val >= 0) {
        led1_percent = val;
        saveSetting("led1", led1_percent);
        if (dc_available) {
          setPWM(LED1_PIN, led1_percent);
          bot.sendMessage(CHAT_ID, "💡 Đèn 1: *" + String(led1_percent) + "%* (Đã lưu) 💾", "Markdown");
        } else {
          setPWM(LED1_PIN, 0);
          bot.sendMessage(CHAT_ID, "💡 Đèn 1: *" + String(led1_percent) + "%* (Đã lưu, đang tắt do mất điện) 💾", "Markdown");
        }
      } else {
        bot.sendMessage(CHAT_ID, "💡 Đèn 1 hiện tại: *" + String(led1_percent) + "%*\nGõ: `/led1 50` để đặt 50%", "Markdown");
      }
    }
    // ---------- ĐIỀU KHIỂN ĐÈN 2 (PWM %) ----------
    else if (text.startsWith("/led2") || text.startsWith("led2")) {
      int val = parsePercent(text);
      if (val < 0) {
        if (text.endsWith("on"))  val = 100;
        else if (text.endsWith("off")) val = 0;
      }
      if (val >= 0) {
        led2_percent = val;
        saveSetting("led2", led2_percent);
        if (dc_available) {
          setPWM(LED2_PIN, led2_percent);
          bot.sendMessage(CHAT_ID, "💡 Đèn 2: *" + String(led2_percent) + "%* (Đã lưu) 💾", "Markdown");
        } else {
          setPWM(LED2_PIN, 0);
          bot.sendMessage(CHAT_ID, "💡 Đèn 2: *" + String(led2_percent) + "%* (Đã lưu, đang tắt do mất điện) 💾", "Markdown");
        }
      } else {
        bot.sendMessage(CHAT_ID, "💡 Đèn 2 hiện tại: *" + String(led2_percent) + "%*\nGõ: `/led2 50` để đặt 50%", "Markdown");
      }
    }
    // ---------- ĐIỀU KHIỂN ĐÈN 3 KHI MẤT ĐIỆN DC (17h30 - 5h30 VN) ----------
    else if (text.startsWith("/led3_outage") || text.startsWith("led3_outage") || text.startsWith("/led3_matdien")) {
      int val = parsePercent(text);
      if (val >= 0) {
        led3_outage_percent = val;
        saveSetting("led3_outage", led3_outage_percent);
        updateLEDs();
        bot.sendMessage(CHAT_ID, "🌙 Đèn 3 khi mất DC (17h30–5h30): *" + String(led3_outage_percent) + "%* (Đã lưu) 💾", "Markdown");
      } else {
        bot.sendMessage(CHAT_ID, "🌙 Đèn 3 khi mất DC (17h30–5h30) hiện tại: *" + String(led3_outage_percent) + "%*\nGõ: `/led3_outage 40` để đặt 40%", "Markdown");
      }
    }
    // ---------- ĐIỀU KHIỂN ĐÈN 3 (PWM %) ----------
    else if (text.startsWith("/led3") || text.startsWith("led3")) {
      int val = parsePercent(text);
      if (val < 0) {
        if (text.endsWith("on"))  val = 100;
        else if (text.endsWith("off")) val = 0;
      }
      if (val >= 0) {
        led3_percent = val;
        saveSetting("led3", led3_percent);
        updateLEDs();
        bot.sendMessage(CHAT_ID, "💡 Đèn 3: *" + String(led3_percent) + "%* (Đã lưu) 💾", "Markdown");
      } else {
        bot.sendMessage(CHAT_ID, "💡 Đèn 3 hiện tại: *" + String(led3_percent) + "%*\nGõ: `/led3 50` để đặt 50%", "Markdown");
      }
    }
    // ---------- TẤT CẢ ĐÈN (PWM %) ----------
    else if (text.startsWith("/led_all") || text.startsWith("led_all") || text.startsWith("led all")) {
      int val = parsePercent(text);
      if (val < 0) {
        if (text.endsWith("on"))  val = 100;
        else if (text.endsWith("off")) val = 0;
      }
      if (val >= 0) {
        led1_percent = led2_percent = led3_percent = val;
        saveSetting("led1", val);
        saveSetting("led2", val);
        saveSetting("led3", val);
        updateLEDs();
        String msg = "💡 Tất cả đèn: *" + String(val) + "%* (Đã lưu) 💾";
        if (!dc_available) msg += "\n*(Đèn 1 & 2 tạm tắt do mất DC; Đèn 3 sáng nếu trong 17h30–5h30)*";
        bot.sendMessage(CHAT_ID, msg, "Markdown");
      } else {
        bot.sendMessage(CHAT_ID, "💡 Đèn 1: " + String(led1_percent) + "% | Đèn 2: " + String(led2_percent) + "% | Đèn 3: " + String(led3_percent) + "%\nGõ: `/led_all 50` để đặt tất cả 50%", "Markdown");
      }
    }
    // ---------- CẤU HÌNH QUẠT TỰ ĐỘNG THEO NHIỆT ĐỘ ----------
    else if (text.startsWith("/fan_auto") || text.startsWith("fan_auto")) {
      if (text.startsWith("/fan_auto 28") || text.startsWith("fan_auto 28")) {
        int val = parsePercent(text);
        if (val >= 0) {
          fan_auto_28 = val;
          saveSetting("fan_a28", fan_auto_28);
          updateFanSpeed();
          bot.sendMessage(CHAT_ID, "⚙️ Quạt tự động (28°C–29°C): *" + String(fan_auto_28) + "%* (Đã lưu) 💾", "Markdown");
        } else {
          bot.sendMessage(CHAT_ID, "⚙️ Quạt (28°C–29°C) hiện tại: *" + String(fan_auto_28) + "%*\nGõ: `/fan_auto 28 35` để đặt 35%", "Markdown");
        }
      }
      else if (text.startsWith("/fan_auto 29") || text.startsWith("fan_auto 29")) {
        int val = parsePercent(text);
        if (val >= 0) {
          fan_auto_29 = val;
          saveSetting("fan_a29", fan_auto_29);
          updateFanSpeed();
          bot.sendMessage(CHAT_ID, "⚙️ Quạt tự động (29°C–30°C): *" + String(fan_auto_29) + "%* (Đã lưu) 💾", "Markdown");
        } else {
          bot.sendMessage(CHAT_ID, "⚙️ Quạt (29°C–30°C) hiện tại: *" + String(fan_auto_29) + "%*\nGõ: `/fan_auto 29 60` để đặt 60%", "Markdown");
        }
      }
      else if (text.startsWith("/fan_auto 30") || text.startsWith("fan_auto 30")) {
        int val = parsePercent(text);
        if (val >= 0) {
          fan_auto_30 = val;
          saveSetting("fan_a30", fan_auto_30);
          updateFanSpeed();
          bot.sendMessage(CHAT_ID, "⚙️ Quạt tự động (>30°C): *" + String(fan_auto_30) + "%* (Đã lưu) 💾", "Markdown");
        } else {
          bot.sendMessage(CHAT_ID, "⚙️ Quạt (>30°C) hiện tại: *" + String(fan_auto_30) + "%*\nGõ: `/fan_auto 30 100` để đặt 100%", "Markdown");
        }
      }
      else if (text.startsWith("/fan_auto err") || text.startsWith("fan_auto err")) {
        int val = parsePercent(text);
        if (val >= 0) {
          fan_auto_err = val;
          saveSetting("fan_aerr", fan_auto_err);
          updateFanSpeed();
          bot.sendMessage(CHAT_ID, "⚙️ Quạt khi cảm biến lỗi: *" + String(fan_auto_err) + "%* (Đã lưu) 💾", "Markdown");
        } else {
          bot.sendMessage(CHAT_ID, "⚙️ Quạt khi cảm biến lỗi hiện tại: *" + String(fan_auto_err) + "%*\nGõ: `/fan_auto err 30` để đặt 30%", "Markdown");
        }
      }
      else {
        String msg = "🌀 *CẤU HÌNH QUẠT TỰ ĐỘNG THEO NHIỆT ĐỘ*\n";
        msg += "━━━━━━━━━━━━━━━━━━━━\n";
        msg += "• Dưới 28°C: Tự do điều khiển (*" + String(fan_percent) + "%*)\n";
        msg += "• Từ 28°C đến <29°C: *" + String(fan_auto_28) + "%*\n";
        msg += "• Từ 29°C đến <=30°C: *" + String(fan_auto_29) + "%*\n";
        msg += "• Trên 30°C: *" + String(fan_auto_30) + "%*\n";
        msg += "• Cảm biến lỗi: *" + String(fan_auto_err) + "%*\n";
        msg += "━━━━━━━━━━━━━━━━━━━━\n";
        msg += "⚡ Quạt thực tế đang chạy: *" + String(getRequiredFanPercent()) + "%*\n\n";
        msg += "👉 *Lệnh thay đổi:*\n";
        msg += "`/fan_auto 28 XX` — Đổi mức khi 28–29°C\n";
        msg += "`/fan_auto 29 XX` — Đổi mức khi 29–30°C\n";
        msg += "`/fan_auto 30 XX` — Đổi mức khi > 30°C\n";
        msg += "`/fan_auto err XX` — Đổi mức khi cảm biến lỗi";
        bot.sendMessage(CHAT_ID, msg, "Markdown");
      }
    }
    // ---------- ĐIỀU KHIỂN QUẠT (PWM %, tự do khi < 28°C) ----------
    else if (text.startsWith("/fan") || text.startsWith("fan")) {
      int val = parsePercent(text);
      if (val < 0) {
        if (text.endsWith("on"))  val = 100;
        else if (text.endsWith("off")) val = 0;
      }
      if (val >= 0) {
        fan_percent = val;
        saveSetting("fan", fan_percent);
        updateFanSpeed();
        int actual = getRequiredFanPercent();
        if (actual != fan_percent) {
          bot.sendMessage(CHAT_ID, "🌀 Quạt: mức tự do *" + String(fan_percent) + "%* (Đã lưu) 💾\n⚠️ Hiện tại nhiệt độ cao, quạt đang buộc chạy *" + String(actual) + "%* để làm mát!", "Markdown");
        } else {
          bot.sendMessage(CHAT_ID, "🌀 Quạt: *" + String(fan_percent) + "%* (Đã lưu) 💾", "Markdown");
        }
      } else {
        int actual = getRequiredFanPercent();
        String msg = "🌀 Quạt mức tự do (<28°C): *" + String(fan_percent) + "%*\n";
        msg += "⚡ Quạt thực tế đang chạy: *" + String(actual) + "%*";
        if (actual != fan_percent) msg += " (Tự động theo nhiệt độ)";
        msg += "\nGõ: `/fan 50` để đặt mức tự do 50%\nGõ: `/fan_auto` để xem ngưỡng tự động";
        bot.sendMessage(CHAT_ID, msg, "Markdown");
      }
    }
    // ---------- ĐIỀU KHIỂN BƠM LỌC (PWM %) ----------
    else if (text.startsWith("/pump") || text.startsWith("pump")) {
      int val = parsePercent(text);
      if (val < 0) {
        if (text.endsWith("on"))  val = 100;
        else if (text.endsWith("off")) val = 0;
      }
      if (val >= 0) {
        pump_percent = val;
        saveSetting("pump", pump_percent);
        if (dc_available) {
          setPWM(PUMP_FILTER_PIN, pump_percent);
        }
        bot.sendMessage(CHAT_ID, "🔵 Bơm lọc: *" + String(pump_percent) + "%* (Đã lưu) 💾" + String(dc_available ? "" : " (chờ có DC)"), "Markdown");
      } else {
        bot.sendMessage(CHAT_ID, "🔵 Bơm lọc hiện tại: *" + String(pump_percent) + "%*\nGõ: `/pump 80` để đặt 80%", "Markdown");
      }
    }
    // ---------- ĐIỀU KHIỂN OXY (PWM %, mặc định 20%) ----------
    else if (text.startsWith("/oxy") || text.startsWith("oxy")) {
      int val = parsePercent(text);
      if (val >= 0) {
        oxy_percent = val;
        saveSetting("oxy", oxy_percent);
        if (oxy_running) {
          setPWM(OXY_PIN, oxy_percent);
        }
        bot.sendMessage(CHAT_ID, "🫧 Oxy: *" + String(oxy_percent) + "%* (Đã lưu) 💾" + String(oxy_running ? " ✅" : " (chờ mất DC)"), "Markdown");
      } else {
        String msg = "🫧 Oxy hiện tại: *" + String(oxy_percent) + "%*\n";
        msg += String(oxy_running ? "Đang chạy ✅ (không có DC)" : "Đang tắt ❌ (có DC)");
        msg += "\nGõ: `/oxy 30` để đổi sang 30%";
        bot.sendMessage(CHAT_ID, msg, "Markdown");
      }
    }
    // ---------- LỆNH ĐỌC PIN ----------
    else if (text == "/battery" || text == "/pin" || text == "pin") {
      String msg = "🔋 *THÔNG TIN PIN*\n";
      msg += "Điện áp: " + String(batt_voltage, 2) + "V\n";
      msg += "Mức pin: " + String(batt_percent) + "% " + getBattColorIcon(batt_percent) + "\n";
      if (batt_percent <= BATT_LOW_PERCENT)
        msg += "🚨 *Pin rất yếu! Cần sạc ngay!*";
      else if (batt_percent <= BATT_WARN_PERCENT)
        msg += "⚠️ *Pin yếu (dưới 20%)! Nên sạc sớm.*";
      else if (batt_percent >= BATT_FULL_PERCENT)
        msg += "✅ Pin đã đầy.";
      bot.sendMessage(CHAT_ID, msg, "Markdown");
    }
    // ---------- LỆNH XEM NHẬT KÝ MẤT ĐIỆN (20 LẦN GẦN NHẤT) ----------
    else if (text == "/log" || text == "log") {
      if (outage_log_count == 0) {
        bot.sendMessage(CHAT_ID, "📋 Hiện chưa có dữ liệu mất điện nào được lưu.", "");
      } else {
        String logMsg = "";
        for (int i = 0; i < outage_log_count; i++) {
          int total_drain = outage_logs[i].start_pct - outage_logs[i].end_pct;
          if (total_drain < 0) total_drain = 0;

          char dateBuf[32];
          if (outage_logs[i].year > 0) {
            snprintf(dateBuf, sizeof(dateBuf), "%02d / %02d / %04d", outage_logs[i].day, outage_logs[i].month, outage_logs[i].year);
          } else {
            snprintf(dateBuf, sizeof(dateBuf), "%02d / %02d", outage_logs[i].day, outage_logs[i].month);
          }

          char timeBuf[64];
          snprintf(timeBuf, sizeof(timeBuf), "%02d : %02d : %02d đến %02d : %02d : %02d",
                   outage_logs[i].start_h, outage_logs[i].start_m, outage_logs[i].start_s,
                   outage_logs[i].end_h, outage_logs[i].end_m, outage_logs[i].end_s);

          uint32_t dur = outage_logs[i].duration_sec;
          char durBuf[32];
          snprintf(durBuf, sizeof(durBuf), "%02d : %02d : %02d", dur / 3600, (dur % 3600) / 60, dur % 60);

          logMsg += "🔵 " + String(i + 1) + ". Ngày " + String(dateBuf) + "\n";
          logMsg += "Mất điện từ " + String(timeBuf) + ".\n";
          logMsg += "Tổng thời gian:          " + String(durBuf) + "\n";
          logMsg += "Pin khi mất điện:    " + String(outage_logs[i].start_v, 2) + " V (" + String(outage_logs[i].start_pct) + "% " + getBattColorIcon(outage_logs[i].start_pct) + ")\n";
          logMsg += "Pin khi có điện:       " + String(outage_logs[i].end_v, 2) + " V (" + String(outage_logs[i].end_pct) + "% " + getBattColorIcon(outage_logs[i].end_pct) + ")\n";
          logMsg += "Tổng pin tiêu thụ:               " + String(total_drain) + "%\n";
          logMsg += "━━━━━━━━━━━━━━━━━━━━\n";
        }
        bot.sendMessage(CHAT_ID, logMsg, "");
      }
    }
    // ---------- LỆNH XOÁ NHẬT KÝ MẤT ĐIỆN ----------
    else if (text == "/clear_log" || text == "clear_log") {
      outage_log_count = 0;
      saveOutageLogs();
      bot.sendMessage(CHAT_ID, "🗑️ Đã xoá toàn bộ nhật ký mất điện.", "");
    }
    // ---------- LỆNH XEM NHẬT KÝ LỖI RESET (20 LẦN GẦN NHẤT) ----------
    else if (text == "/error" || text == "error") {
      if (error_log_count == 0) {
        bot.sendMessage(CHAT_ID, "📋 Hiện chưa có ghi nhận lỗi reset nào.", "");
      } else {
        String msg = "⚠️ *NHẬT KÝ LỖI RESET (" + String(error_log_count) + " lần gần nhất):*\n";
        msg += "━━━━━━━━━━━━━━━━━━━━\n";
        for (int i = 0; i < error_log_count; i++) {
          char dateBuf[16];
          if (error_logs[i].day > 0 && error_logs[i].month > 0) {
            snprintf(dateBuf, sizeof(dateBuf), "%02d / %02d", error_logs[i].day, error_logs[i].month);
          } else {
            snprintf(dateBuf, sizeof(dateBuf), "-- / --");
          }
          msg += "🔵 " + String(i + 1) + ". Ngày " + String(dateBuf) + ", bị lỗi " + getResetReasonName(error_logs[i].reason_code) + " trong " + String(error_logs[i].uptime_sec) + " giây.\n";
          msg += "━━━━━━━━━━━━━━━━━━━━\n";
        }
        bot.sendMessage(CHAT_ID, msg, "");
      }
    }
    // ---------- LỆNH XOÁ NHẬT KÝ LỖI RESET ----------
    else if (text == "/clear_error" || text == "clear_error") {
      error_log_count = 0;
      saveErrorLogs();
      bot.sendMessage(CHAT_ID, "🗑️ Đã xoá toàn bộ nhật ký lỗi reset.", "");
    }
    // ---------- LỆNH HELP (CHIA CÁC TỆP LỆNH) ----------
    else if (text.startsWith("/help") || text == "help" || text == "?" || text == "menu") {
      String helpMsg = "📋 *DANH SÁCH LỆNH ĐIỀU KHIỂN*\n";
      helpMsg += "━━━━━━━━━━━━━━━━━━━━\n";
      helpMsg += "🌐 *LỆNH CHUNG (DÙNG MỌI LÚC):*\n";
      helpMsg += "📊 `/status` — Xem trạng thái toàn hệ thống\n";
      helpMsg += "🔋 `/battery` — Xem pin & nguồn\n";
      helpMsg += "🌡️ `/temp` — Xem nhiệt độ các cảm biến\n";
      helpMsg += "📜 `/log` — Xem nhật ký mất điện (20 lần gần nhất)\n";
      helpMsg += "⚠️ `/error` — Xem nhật ký lỗi reset chip\n";
      helpMsg += "🗑️ `/clear_log` — Xoá nhật ký mất điện\n";
      helpMsg += "🗑️ `/clear_error` — Xoá nhật ký lỗi reset\n";
      helpMsg += "━━━━━━━━━━━━━━━━━━━━\n";
      helpMsg += "⚡ *LỆNH KHI CÓ ĐIỆN DC:*\n";
      helpMsg += "🔵 `/pump XX` — Bơm lọc (0–100%) *(chỉ chạy khi có DC)*\n";
      helpMsg += "💡 `/led1 XX` — Đèn 1 (0–100%) *(mất DC tự tắt)*\n";
      helpMsg += "💡 `/led2 XX` — Đèn 2 (0–100%) *(mất DC tự tắt)*\n";
      helpMsg += "💡 `/led3 XX` — Đèn 3 khi có điện (0–100%)\n";
      helpMsg += "💡 `/led_all XX` — Chỉnh tất cả đèn (0–100%)\n";
      helpMsg += "━━━━━━━━━━━━━━━━━━━━\n";
      helpMsg += "🔴 *LỆNH KHI MẤT ĐIỆN DC (CHẠY PIN):*\n";
      helpMsg += "🫧 `/oxy XX` — Oxy chạy backup pin (mặc định 20%)\n";
      helpMsg += "🌙 `/led3_outage XX` — Đèn 3 mất DC (17h30–5h30, mặc định 30%)\n";
      helpMsg += "*(Đèn 1 & 2 luôn tắt, Bơm lọc tắt để tiết kiệm pin)*\n";
      helpMsg += "━━━━━━━━━━━━━━━━━━━━\n";
      helpMsg += "🌀 *CÀI ĐẶT QUẠT & TỰ ĐỘNG THEO NHIỆT ĐỘ:*\n";
      helpMsg += "🌀 `/fan XX` — Mức quạt tự do khi nhiệt độ < 28°C\n";
      helpMsg += "⚙️ `/fan_auto` — Xem các ngưỡng quạt tự động\n";
      helpMsg += "⚙️ `/fan_auto 28 XX` — Quạt khi 28–29°C (mặc định 30%)\n";
      helpMsg += "⚙️ `/fan_auto 29 XX` — Quạt khi 29–30°C (mặc định 50%)\n";
      helpMsg += "⚙️ `/fan_auto 30 XX` — Quạt khi > 30°C (mặc định 100%)\n";
      helpMsg += "⚙️ `/fan_auto err XX` — Quạt khi cảm biến lỗi (mặc định 30%)\n";
      helpMsg += "━━━━━━━━━━━━━━━━━━━━\n";
      helpMsg += "📌 *Ghi chú:*\n";
      helpMsg += "• XX là số từ 0 đến 100 (đặt 0 để tắt, hoặc `on`/`off`)\n";
      helpMsg += "• Mọi cài đặt tự động lưu vào Flash 💾";
      bot.sendMessage(CHAT_ID, helpMsg, "Markdown");
    }
    // ---------- LỆNH KHÔNG NHẬN DẠNG ----------
    else {
      bot.sendMessage(CHAT_ID, "❓ Lệnh không hợp lệ.\nGõ /help để xem danh sách lệnh.", "");
    }
  }
}

// ==================== HÀM GỬI HEARTBEAT (Healthchecks.io) ====================
void sendHeartbeat() {
  // Chỉ gửi Heartbeat khi đang có nguồn DC (khi mất DC ngắt không gửi)
  if (!dc_available) {
    return;
  }

  // Nếu mất Wi-Fi hoặc Internet, tự thử lại khi kết nối phục hồi
  if (WiFi.status() != WL_CONNECTED) {
    heartbeatInterval = HEARTBEAT_RETRY_MS;
    lastHeartbeatAttempt = millis();
    return;
  }

  WiFiClientSecure client;
  client.setInsecure(); // Bỏ qua kiểm tra chứng chỉ SSL/TLS
  client.setTimeout(2500); // Timeout kết nối socket 2.5s

  HTTPClient http;
  http.setTimeout(HEARTBEAT_TIMEOUT_MS); // Timeout HTTP để không làm gián đoạn điều khiển quạt, oxy, đèn
  http.begin(client, HEARTBEAT_URL);

  esp_task_wdt_reset(); // Feed watchdog trước khi gửi request
  int httpCode = http.GET();
  esp_task_wdt_reset(); // Feed watchdog sau khi nhận phản hồi
  http.end();
  client.stop();

  lastHeartbeatAttempt = millis();

  // Chỉ xác nhận Heartbeat thành công khi Healthchecks.io trả HTTP 200
  if (httpCode == 200) {
    heartbeatInterval = HEARTBEAT_INTERVAL_MS; // Chu kỳ gửi định kỳ 60 giây
    Serial.println("💓 [HEARTBEAT] Gửi thành công tới Healthchecks.io (HTTP 200)");
  } else {
    heartbeatInterval = HEARTBEAT_RETRY_MS;    // Gửi thất bại -> thử lại mỗi 15 giây
    Serial.printf("⚠️ [HEARTBEAT] Gửi thất bại (Code: %d), thử lại sau 15s\n", httpCode);
  }
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n🐟 Khởi động hệ thống bể cá...");

  // Đọc cài đặt đã lưu từ bộ nhớ Flash NVS
  loadSettings();
  loadOutageLogs(); // Đọc nhật ký mất điện từ Flash NVS
  loadErrorLogs();  // Đọc nhật ký lỗi reset từ Flash NVS

  // Kiểm tra lý do khởi động lại ESP32
  esp_reset_reason_t rst_reason = esp_reset_reason();
  Serial.printf("🔍 [RESET] Lý do reset: %d - %s\n", (int)rst_reason, getResetReasonName(rst_reason).c_str());

  uint32_t last_uptime = 0;
  uint8_t err_day = rtc_day;
  uint8_t err_month = rtc_month;

  if (rtc_magic == RTC_MAGIC_KEY) {
    last_uptime = rtc_uptime_sec;
  }

  // Ghi nhận lỗi nếu reset bất thường (Brownout, Watchdog, Panic, SW restart, hoặc Power-on khi đang chạy)
  if (rst_reason == ESP_RST_BROWNOUT ||
      rst_reason == ESP_RST_TASK_WDT ||
      rst_reason == ESP_RST_INT_WDT  ||
      rst_reason == ESP_RST_WDT      ||
      rst_reason == ESP_RST_PANIC    ||
      rst_reason == ESP_RST_SW       ||
      (rst_reason == ESP_RST_POWERON && last_uptime > 5)) {
    addErrorLog(err_day, err_month, (uint16_t)rst_reason, last_uptime);
    Serial.printf("⚠️ [RESET] Đã ghi nhận lỗi: %s (chạy %lu giây)\n", getResetReasonName(rst_reason).c_str(), last_uptime);
  }

  // Khởi tạo lại bộ đếm uptime RTC cho phiên làm việc mới
  rtc_uptime_sec = 0;
  rtc_magic = RTC_MAGIC_KEY;

  // Khôi phục công suất ban đầu của quạt và bơm/oxy
  setPWM(FAN_PIN, fan_percent);
  setPWM(PUMP_FILTER_PIN, 0);
  setPWM(OXY_PIN, 0);

  // Cấu hình ADC (ESP32-C3: attenuation 11dB cho dải ~0–2.5V)
  analogSetAttenuation(ADC_11db);
  analogReadResolution(12);

  // Đọc pin lần đầu
  batt_voltage = readBatteryVoltage();
  batt_percent = batteryPercent(batt_voltage);

  // Đọc trạng thái DC lần đầu và cập nhật đầu ra (bơm, oxy, đèn)
  dc_available = isDCAvailable();
  prev_dc_available = dc_available;
  if (!dc_available) {
    outage_in_progress = true;
    outage_start_ms = millis();
    time_t start_sec;
    time(&start_sec);
    if (start_sec > 1600000000UL) {
      outage_start_epoch = start_sec;
      struct tm ti;
      localtime_r(&start_sec, &ti);
      outage_start_year  = (uint16_t)(ti.tm_year + 1900);
      outage_start_month = (uint8_t)(ti.tm_mon + 1);
      outage_start_day   = (uint8_t)ti.tm_mday;
      outage_start_h     = (uint8_t)ti.tm_hour;
      outage_start_m     = (uint8_t)ti.tm_min;
      outage_start_s     = (uint8_t)ti.tm_sec;
    } else {
      outage_start_epoch = 0;
      outage_start_year  = 0;
      outage_start_month = 0;
      outage_start_day   = 0;
      outage_start_h     = 0;
      outage_start_m     = 0;
      outage_start_s     = 0;
    }
    outage_start_v = batt_voltage;
    outage_start_pct = batt_percent;
    outage_last_v = batt_voltage;
    outage_last_pct = batt_percent;
    Serial.println("🔴 Khởi động khi không có nguồn DC! Bắt đầu đếm thời gian mất điện.");
  }
  updatePumpOxy();
  updateLEDs();
  updateFanSpeed();

  // Kết nối WiFi (có quét mạng phụ khi mất WiFi chính)
  connectWiFi();

  // Đồng bộ thời gian thực qua giao thức NTP (Internet)
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET, NTP_SERVER_1, NTP_SERVER_2);

  // Bỏ qua xác thực SSL (UniversalTelegramBot cần)
  secured_client.setInsecure();

  // Khởi động cảm biến nhiệt độ DS18B20 trên GPIO3
  sensors.begin();
  sensors.setResolution(12);

  temp_sensor_count = sensors.getDeviceCount();
  if (temp_sensor_count > 0) {
    if (temp_sensor_count > MAX_TEMP_SENSORS) temp_sensor_count = MAX_TEMP_SENSORS;
    // Đo lần đầu ở chế độ blocking để có dữ liệu ngay khi khởi động.
    sensors.setWaitForConversion(true);
    sensors.requestTemperatures();
    for (int i = 0; i < temp_sensor_count; i++) {
      float t = sensors.getTempCByIndex(i);
      if (t != DEVICE_DISCONNECTED_C && t > -50.0f && t < 125.0f && t != 85.0f) {
        current_temperatures[i] = t;
      } else {
        current_temperatures[i] = -127.0f;
      }
    }
    current_temperature = current_temperatures[0];
  } else {
    current_temperature = -127.0f;
  }

  // Chuyển sang non-blocking cho các lần đo định kỳ trong loop().
  sensors.setWaitForConversion(false);
  sensors.requestTemperatures();

  // Cập nhật công suất quạt theo nhiệt độ khởi động
  updateFanSpeed();

  // Gửi thông báo khởi động
  if (WiFi.status() == WL_CONNECTED) {
    String startMsg = "🐟 *HỆ THỐNG BỂ CÁ ĐÃ KHỞI ĐỘNG*\n";
    startMsg += "━━━━━━━━━━━━━━━━━━━━\n";
    startMsg += "📶 WiFi: " + WiFi.SSID() + " (" + WiFi.localIP().toString() + ")\n";
    startMsg += "🔋 Pin: " + String(batt_voltage, 2) + "V (" + String(batt_percent) + "% " + getBattColorIcon(batt_percent) + ")\n";
    startMsg += "⚡ DC: " + String(dc_available ? "Có ✅" : "Mất ❌") + "\n";
    startMsg += "🔎 DS18B20: " + String(temp_sensor_count) + " thiết bị\n";
    startMsg += buildTemperatureStatus();
    if (rst_reason != ESP_RST_POWERON) {
      startMsg += "⚠️ Reset trước đó: " + getResetReasonName(rst_reason) + " (" + String(last_uptime) + "s)\n";
    }
    startMsg += "━━━━━━━━━━━━━━━━━━━━\n";
    startMsg += "Gõ /help để xem lệnh.";
    sendTelegramNotify(startMsg);
  }

  // Khởi tạo Watchdog Timer
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true   // Tự reset chip khi timeout
  };
  esp_task_wdt_reconfigure(&wdt_config);
  esp_task_wdt_add(NULL);  // Đăng ký task loop() vào WDT
  Serial.println("🛡️ Watchdog: " + String(WDT_TIMEOUT_S) + "s");

  Serial.println("✅ Hệ thống sẵn sàng!");
}

// ==================== LOOP ====================
void loop() {
  esp_task_wdt_reset();  // Feed watchdog mỗi vòng loop
  unsigned long now = millis();

  // Cập nhật uptime RTC mỗi giây và ngày tháng khi có NTP
  rtc_uptime_sec = now / 1000UL;
  static unsigned long lastRtcSync = 0;
  if (now - lastRtcSync >= 30000UL || rtc_day == 0) {
    lastRtcSync = now;
    uint8_t d = 0, m = 0;
    getCurrentDate(d, m);
    if (d > 0) {
      rtc_day = d;
      rtc_month = m;
      if (error_log_count > 0 && error_logs[0].day == 0) {
        error_logs[0].day = d;
        error_logs[0].month = m;
        saveErrorLogs();
      }
    }
  }

  // ===== 1. KIỂM TRA NGUỒN DC (mỗi 0.5 giây) =====
  if (now - lastDCCheck >= DC_CHECK_INTERVAL) {
    lastDCCheck = now;
    bool current_dc = isDCAvailable();

    // Phát hiện thay đổi trạng thái DC (Cắm vào hoặc Rút ra)
    if (current_dc != prev_dc_available) {
      dc_available = current_dc;
      prev_dc_available = dc_available;
      updatePumpOxy();
      updateLEDs(); // Tắt Đèn 1, 2 khi mất điện; bật lại khi có điện; Đèn 3 giữ nguyên

      // Thông báo ngay lập tức khi đổi trạng thái
      if (dc_available) {
        Serial.println("⚡ [DC] Đã có nguồn DC!");
        // Khởi động lại gửi Heartbeat ngay khi có điện lại
        lastHeartbeatAttempt = 0;
        heartbeatInterval    = 0;

        if (outage_in_progress) {
          outage_in_progress = false;
          unsigned long elapsed_ms = now - outage_start_ms;
          uint32_t duration_sec = elapsed_ms / 1000UL;

          uint16_t ev_year = outage_start_year;
          uint8_t  ev_month = outage_start_month;
          uint8_t  ev_day   = outage_start_day;
          uint8_t  sh = outage_start_h, sm = outage_start_m, ss = outage_start_s;
          uint8_t  eh = 0, em = 0, es = 0;

          time_t end_sec;
          time(&end_sec);
          if (end_sec > 1600000000UL) {
            struct tm ti_end;
            localtime_r(&end_sec, &ti_end);
            eh = (uint8_t)ti_end.tm_hour;
            em = (uint8_t)ti_end.tm_min;
            es = (uint8_t)ti_end.tm_sec;

            // Nếu lúc mất điện chưa đồng bộ được giờ NTP, tính lùi start_sec = end_sec - duration_sec
            if (outage_start_epoch < 1600000000UL || ev_year == 0) {
              time_t calc_start = end_sec - duration_sec;
              struct tm ti_start;
              localtime_r(&calc_start, &ti_start);
              ev_year  = (uint16_t)(ti_start.tm_year + 1900);
              ev_month = (uint8_t)(ti_start.tm_mon + 1);
              ev_day   = (uint8_t)ti_start.tm_mday;
              sh       = (uint8_t)ti_start.tm_hour;
              sm       = (uint8_t)ti_start.tm_min;
              ss       = (uint8_t)ti_start.tm_sec;
            }
          }

          float start_v   = outage_start_v;
          int   start_pct = outage_start_pct;
          float end_v     = outage_last_v;
          int   end_pct   = outage_last_pct;

          int ss_minutes = calculateChargeTimeMinutes(end_v);

          // Lưu vào lịch sử 20 lần gần nhất
          addOutageLog(ev_year, ev_month, ev_day, sh, sm, ss, eh, em, es, duration_sec, start_v, start_pct, end_v, end_pct);

          char timeBuf[64];
          snprintf(timeBuf, sizeof(timeBuf), "%02d : %02d : %02d đến %02d : %02d : %02d", sh, sm, ss, eh, em, es);

          uint32_t dur = duration_sec;
          char durBuf[32];
          snprintf(durBuf, sizeof(durBuf), "%02d : %02d : %02d", dur / 3600, (dur % 3600) / 60, dur % 60);

          // Định dạng thông báo có điện theo đúng mẫu yêu cầu:
          restore_notify_msg = "🟢 ⚡ *Hiện đã có điện!*\n";
          restore_notify_msg += "Mất điện từ " + String(timeBuf) + ".\n";
          restore_notify_msg += "Thời gian mất điện    " + String(durBuf) + "\n";
          restore_notify_msg += "Pin khi mất điện:    " + String(start_v, 2) + " V (" + String(start_pct) + "% " + getBattColorIcon(start_pct) + ")\n";
          restore_notify_msg += "Pin khi có điện:       " + String(end_v, 2) + " V (" + String(end_pct) + "% " + getBattColorIcon(end_pct) + ")\n";
          restore_notify_msg += "Hiện đang sạc lại dự tính " + String(ss_minutes) + " phút sẽ đạt 12V.";

          pending_restore_notify = true;

          // Nếu WiFi đã kết nối thì gửi ngay, nếu chưa thì sẽ tự gửi khi WiFi kết nối ở mục 4
          if (WiFi.status() == WL_CONNECTED) {
            if (bot.sendMessage(CHAT_ID, restore_notify_msg, "")) {
              pending_restore_notify = false;
              Serial.println("✅ Đã gửi thông báo có điện lại tới Telegram!");
            }
          }
        } else {
          String msg = "🟢 ⚡ *Nguồn DC đã có!*\n";
          msg += "🔵 Bơm lọc: " + String(pump_percent) + "% ✅\n";
          msg += "🫧 Oxy: Tắt ❌\n";
          msg += "💡 Đèn 1: " + String(led1_percent > 0 ? String(led1_percent) + "% ✅" : "Tắt ❌") + "\n";
          msg += "💡 Đèn 2: " + String(led2_percent > 0 ? String(led2_percent) + "% ✅" : "Tắt ❌") + "\n";
          msg += "💡 Đèn 3: " + String(led3_percent > 0 ? String(led3_percent) + "% ✅" : "Tắt ❌");
          sendTelegramNotify(msg);
        }
      } else {
        Serial.println("🔴 [DC] Mất nguồn DC!");

        outage_in_progress = true;
        outage_start_ms = now;

        time_t start_sec;
        time(&start_sec);
        if (start_sec > 1600000000UL) {
          outage_start_epoch = start_sec;
          struct tm ti;
          localtime_r(&start_sec, &ti);
          outage_start_year  = (uint16_t)(ti.tm_year + 1900);
          outage_start_month = (uint8_t)(ti.tm_mon + 1);
          outage_start_day   = (uint8_t)ti.tm_mday;
          outage_start_h     = (uint8_t)ti.tm_hour;
          outage_start_m     = (uint8_t)ti.tm_min;
          outage_start_s     = (uint8_t)ti.tm_sec;
        } else {
          outage_start_epoch = 0;
          outage_start_year  = 0;
          outage_start_month = 0;
          outage_start_day   = 0;
          outage_start_h     = 0;
          outage_start_m     = 0;
          outage_start_s     = 0;
        }

        // Đọc điện áp pin ngay khi vừa mất DC
        batt_voltage = readBatteryVoltage();
        batt_percent = batteryPercent(batt_voltage);
        outage_start_v = batt_voltage;
        outage_start_pct = batt_percent;
        outage_last_v = batt_voltage;
        outage_last_pct = batt_percent;

        bool isNight = isNightTimeVN();
        String msg = "🔴 *Mất nguồn DC!*\n";
        msg += "🔵 Bơm lọc: Tắt ❌\n";
        msg += "🫧 Oxy: " + String(oxy_percent) + "% ✅ (backup pin)\n";
        msg += "💡 Đèn 1 & 2: Tắt (tiết kiệm pin)\n";
        if (isNight) {
          msg += "💡 Đèn 3: " + String(led3_outage_percent) + "% ✅ (17h30–5h30)\n";
        } else {
          msg += "💡 Đèn 3: Tắt ❌ (ngoài khung giờ 17h30–5h30)\n";
        }
        msg += "🔋 Pin: " + String(outage_start_v, 2) + "V (" + String(outage_start_pct) + "% " + getBattColorIcon(outage_start_pct) + ")";
        sendTelegramNotify(msg);
      }
    }
  }

  // ===== 2. KIỂM TRA PIN (mỗi 5 giây) =====
  if (now - lastBattCheck >= BATT_CHECK_INTERVAL) {
    lastBattCheck = now;
    batt_voltage = readBatteryVoltage();
    batt_percent = batteryPercent(batt_voltage);

    // Cập nhật điện áp và % pin lần cuối check trong thời gian mất điện
    if (outage_in_progress) {
      outage_last_v = batt_voltage;
      outage_last_pct = batt_percent;
    }

    // Cảnh báo pin rất yếu (<= 10%)
    if (batt_percent <= BATT_LOW_PERCENT) {
      if (now - lastLowBattNotify >= NOTIFY_COOLDOWN || lastLowBattNotify == 0) {
        lastLowBattNotify = now;
        String msg = "🪫 *CẢNH BÁO: PIN RẤT YẾU!*\n";
        msg += "🔋 " + String(batt_voltage, 2) + "V (" + String(batt_percent) + "% " + getBattColorIcon(batt_percent) + ")\n";
        msg += "🚨 Mức pin dưới " + String(BATT_LOW_PERCENT) + "%, cần sạc pin ngay lập tức!";
        sendTelegramNotify(msg);
        Serial.println("🚨 Pin rất yếu: " + String(batt_percent) + "%");
      }
    }
    // Cảnh báo pin yếu (<= 20%)
    else if (batt_percent <= BATT_WARN_PERCENT) {
      if (now - lastWarnBattNotify >= NOTIFY_COOLDOWN || lastWarnBattNotify == 0) {
        lastWarnBattNotify = now;
        String msg = "⚠️ *CẢNH BÁO: PIN CÒN " + String(batt_percent) + "%!*\n";
        msg += "🔋 " + String(batt_voltage, 2) + "V (" + String(batt_percent) + "% " + getBattColorIcon(batt_percent) + ")\n";
        msg += "⚠️ Mức pin đã xuống dưới " + String(BATT_WARN_PERCENT) + "%, vui lòng chuẩn bị cắm sạc!";
        sendTelegramNotify(msg);
        Serial.println("⚠️ Cảnh báo pin 20%: " + String(batt_percent) + "%");
      }
    }

    // Thông báo pin đầy
    if (batt_percent >= BATT_FULL_PERCENT) {
      if (now - lastFullBattNotify >= NOTIFY_COOLDOWN || lastFullBattNotify == 0) {
        lastFullBattNotify = now;
        String msg = "🔋 *PIN ĐÃ ĐẦY!*\n";
        msg += "🔋 " + String(batt_voltage, 2) + "V (" + String(batt_percent) + "% " + getBattColorIcon(batt_percent) + ")\n";
        msg += "✅ Pin đã sạc đầy.";
        sendTelegramNotify(msg);
        Serial.println("✅ Pin đầy: " + String(batt_percent) + "%");
      }
    }
  }

  // ===== 3. KIỂM TRA NHIỆT ĐỘ DS18B20 & ĐIỀU KHIỂN QUẠT TỰ ĐỘNG (mỗi 5 giây) =====
  if (now - lastTempCheck >= TEMP_CHECK_INTERVAL) {
    lastTempCheck = now;
    readTemperature();
    updateFanSpeed(); // Tự động điều chỉnh quạt theo nhiệt độ
  }

  // Cập nhật Đèn 3 khi mất điện nếu qua mốc 17h30 hoặc 5h30 (kiểm tra mỗi 10 giây)
  static unsigned long lastLED3AutoCheck = 0;
  if (!dc_available && (now - lastLED3AutoCheck >= 10000UL)) {
    lastLED3AutoCheck = now;
    updateLEDs();
  }

  // ===== 4. KIỂM TRA LỆNH TELEGRAM & DUY TRÌ KẾT NỐI WIFI (mỗi 1 giây) =====
  if (now - lastBotCheck >= BOT_CHECK_INTERVAL) {
    lastBotCheck = now;
    if (WiFi.status() == WL_CONNECTED) {
      // Gửi thông báo khôi phục nguồn điện nếu đang chờ có kết nối WiFi
      if (pending_restore_notify) {
        if (bot.sendMessage(CHAT_ID, restore_notify_msg, "")) {
          pending_restore_notify = false;
          restore_notify_msg = "";
          Serial.println("✅ Đã gửi thông báo có điện lại tới Telegram sau khi kết nối WiFi!");
        }
      }

      int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
      while (numNewMessages) {
        handleNewMessages(numNewMessages);
        numNewMessages = bot.getUpdates(bot.last_message_received + 1);
      }
    } else {
      // Mất kết nối WiFi -> Thử quét và kết nối lại
      static unsigned long lastReconnect = 0;
      if (now - lastReconnect >= 30000) {  // Thử lại mỗi 30 giây
        lastReconnect = now;
        Serial.println("📶 Mất kết nối WiFi! Đang quét WiFi xung quanh...");
        esp_task_wdt_reset();

        int n = WiFi.scanNetworks();
        bool found_main = false;
        bool found_backup = false;
        for (int i = 0; i < n; i++) {
          if (WiFi.SSID(i) == ssid_main) found_main = true;
          if (WiFi.SSID(i) == ssid_backup) found_backup = true;
        }

        WiFi.disconnect();
        if (found_main) {
          Serial.printf("📶 Tìm thấy WiFi chính (%s), đang kết nối lại...\n", ssid_main);
          WiFi.begin(ssid_main, password_main);
        } else if (found_backup) {
          Serial.printf("📶 Tìm thấy WiFi phụ (%s), đang kết nối...\n", ssid_backup);
          WiFi.begin(ssid_backup, password_backup);
        } else {
          Serial.println("📶 Không quét thấy mạng lưu sẵn, thử lại WiFi chính...");
          WiFi.begin(ssid_main, password_main);
        }
        esp_task_wdt_reset();
      }
    }
  }

  // ===== 5. HEARTBEAT (Healthchecks.io) =====
  // Chỉ gửi khi có nguồn DC, chu kỳ 60 giây (thử lại mỗi 15 giây nếu gửi lỗi hoặc mất mạng)
  if (dc_available && (now - lastHeartbeatAttempt >= heartbeatInterval)) {
    sendHeartbeat();
  }
}
