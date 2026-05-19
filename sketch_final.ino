/*
 * ============================================================
 *  Smart Home Climate Control AIoT System
 *  Module: LD7182 – AI for IoT
 *  Platform: ESP32-S3 (Wokwi Simulation)
 *
 *  PIN MAP — matches diagram.json exactly:
 *    DHT22 #1 (Living Room) DATA → GPIO 5
 *    DHT22 #2 (Bedroom)     DATA → GPIO 4
 *    LCD SDA                     → GPIO 21
 *    LCD SCL                     → GPIO 20  ← NOTE: GPIO20 not 22
 *    Green  LED → 220Ω → GPIO 16
 *    Yellow LED → 220Ω → GPIO 17
 *    Red    LED → 220Ω → GPIO 18
 *    Buzzer                      → GPIO 19
 *
 *  ThingSpeak Fields:
 *    Field 1 → Living Room Temperature (°C)
 *    Field 2 → Living Room Humidity (%)
 *    Field 3 → Bedroom Temperature (°C)
 *    Field 4 → Bedroom Humidity (%)
 *    Field 5 → Z-Score
 *    Field 6 → Alert Level (0=Normal, 1=Warning, 2=Anomaly)
 * ============================================================
 */

#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <math.h>

// ── ⚠️  YOUR CREDENTIALS ─────────────────────────────────────
#define WIFI_SSID       "Wokwi-GUEST"   // Wokwi built-in WiFi (no password needed)
#define WIFI_PASSWORD   ""
#define THINGSPEAK_API  "CCYVMPL4ODM3HQPG"
// ─────────────────────────────────────────────────────────────

#define THINGSPEAK_URL  "http://api.thingspeak.com/update"

// ── Pin Definitions — matched to your diagram.json ───────────
#define DHT_PIN_1       5       // Living Room DHT22 → GPIO5
#define DHT_PIN_2       4       // Bedroom DHT22    → GPIO4
#define DHT_TYPE        DHT22

#define SDA_PIN         21      // LCD SDA → GPIO21
#define SCL_PIN         20      // LCD SCL → GPIO20 (your diagram uses GPIO20)

#define LED_NORMAL      16      // Green  LED
#define LED_WARN        17      // Yellow LED
#define LED_ALERT       18      // Red    LED
#define BUZZER_PIN      19      // Buzzer

// ── TinyML Model Parameters ──────────────────────────────────
#define WINDOW_SIZE     10
#define ZSCORE_WARN     2.0f
#define ZSCORE_ALERT    3.0f
#define ALPHA           0.05f

// ── ThingSpeak: free tier minimum 15 seconds between updates ─
#define THINGSPEAK_INTERVAL 15000UL

// ── Pre-trained baseline (WHO indoor climate guidelines) ──────
float baseline_temp_mean = 22.0f;
float baseline_temp_std  = 1.5f;
float baseline_hum_mean  = 55.0f;
float baseline_hum_std   = 5.0f;

// ── Welford online statistics ─────────────────────────────────
float online_temp_mean = 22.0f;
float online_temp_M2   = 2.25f;
float online_hum_mean  = 55.0f;
float online_hum_M2    = 25.0f;
int   window_count     = 0;

// ── Objects ───────────────────────────────────────────────────
DHT dht1(DHT_PIN_1, DHT_TYPE);
DHT dht2(DHT_PIN_2, DHT_TYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ── State ─────────────────────────────────────────────────────
enum AlertLevel { NORMAL, WARNING, ANOMALY };
AlertLevel current_alert = NORMAL;

unsigned long last_read_ms       = 0;
unsigned long last_thingspeak_ms = 0;
unsigned long loop_counter       = 0;

const unsigned long READ_INTERVAL = 2000UL;

// Latest readings — shared across functions
float g_temp1 = 0, g_hum1 = 0;
float g_temp2 = 0, g_hum2 = 0;
float g_zscore = 0;

// ─────────────────────────────────────────────────────────────
//  Wi-Fi Connection
// ─────────────────────────────────────────────────────────────
void connect_wifi() {
  Serial.print("[WiFi] Connecting");
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Connecting WiFi ");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected! IP: " + WiFi.localIP().toString());
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Connected!");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP().toString());
    delay(2000);
  } else {
    Serial.println("\n[WiFi] FAILED — offline mode.");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Failed!    ");
    lcd.setCursor(0, 1);
    lcd.print("Offline mode    ");
    delay(2000);
  }
}

// ─────────────────────────────────────────────────────────────
//  ThingSpeak Upload
// ─────────────────────────────────────────────────────────────
void upload_to_thingspeak(float t1, float h1, float t2, float h2,
                           float zscore, AlertLevel al) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ThingSpeak] Skipped — no WiFi.");
    return;
  }

  HTTPClient http;
  String url = String(THINGSPEAK_URL);
  url += "?api_key=" + String(THINGSPEAK_API);
  url += "&field1=" + String(t1, 2);
  url += "&field2=" + String(h1, 2);
  url += "&field3=" + String(t2, 2);
  url += "&field4=" + String(h2, 2);
  url += "&field5=" + String(zscore, 3);
  url += "&field6=" + String((int)al);

  http.begin(url);
  int code = http.GET();

  if (code == 200) {
    Serial.print("[ThingSpeak] OK — Entry ID: ");
    Serial.println(http.getString());
    lcd.setCursor(0, 1);
    lcd.print("Cloud: Sent OK  ");
  } else {
    Serial.print("[ThingSpeak] FAILED — HTTP: ");
    Serial.println(code);
    lcd.setCursor(0, 1);
    lcd.print("Cloud: Error    ");
  }
  http.end();
  delay(300);
}

// ─────────────────────────────────────────────────────────────
//  Welford Online Statistics Update
// ─────────────────────────────────────────────────────────────
void update_online_stats(float new_temp, float new_hum) {
  window_count++;

  float delta_t     = new_temp - online_temp_mean;
  online_temp_mean += delta_t / window_count;
  online_temp_M2   += delta_t * (new_temp - online_temp_mean);

  float delta_h     = new_hum - online_hum_mean;
  online_hum_mean  += delta_h / window_count;
  online_hum_M2    += delta_h * (new_hum - online_hum_mean);

  if (window_count >= WINDOW_SIZE) {
    float online_temp_std = sqrtf(online_temp_M2 / window_count);
    float online_hum_std  = sqrtf(online_hum_M2  / window_count);

    baseline_temp_mean = (1.0f - ALPHA) * baseline_temp_mean + ALPHA * online_temp_mean;
    baseline_temp_std  = (1.0f - ALPHA) * baseline_temp_std  + ALPHA * online_temp_std;
    baseline_hum_mean  = (1.0f - ALPHA) * baseline_hum_mean  + ALPHA * online_hum_mean;
    baseline_hum_std   = (1.0f - ALPHA) * baseline_hum_std   + ALPHA * online_hum_std;
  }
}

// ─────────────────────────────────────────────────────────────
//  TinyML Z-Score Inference
// ─────────────────────────────────────────────────────────────
float compute_zscore(float value, float mean, float std_dev) {
  if (std_dev < 0.001f) return 0.0f;
  return fabsf((value - mean) / std_dev);
}

AlertLevel run_inference(float temp, float hum, float &z_out) {
  float z_temp = compute_zscore(temp, baseline_temp_mean, baseline_temp_std);
  float z_hum  = compute_zscore(hum,  baseline_hum_mean,  baseline_hum_std);
  z_out = max(z_temp, z_hum);

  Serial.print("  [AI] Z_temp="); Serial.print(z_temp, 3);
  Serial.print("  Z_hum=");       Serial.print(z_hum, 3);
  Serial.print("  Z_max=");       Serial.println(z_out, 3);

  if (z_out >= ZSCORE_ALERT) return ANOMALY;
  if (z_out >= ZSCORE_WARN)  return WARNING;
  return NORMAL;
}

// ─────────────────────────────────────────────────────────────
//  LCD Display
// ─────────────────────────────────────────────────────────────
void lcd_display(float t1, float h1, float t2, float h2, AlertLevel al) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("LR:");
  lcd.print(t1, 1);
  lcd.print("C ");
  lcd.print((int)h1);
  lcd.print("%");

  lcd.setCursor(0, 1);
  lcd.print("BR:");
  lcd.print(t2, 1);
  lcd.print("C ");
  if      (al == ANOMALY) lcd.print("ALERT!");
  else if (al == WARNING) lcd.print("WARN ");
  else                    lcd.print("OK   ");
}

// ─────────────────────────────────────────────────────────────
//  LED and Buzzer Outputs
// ─────────────────────────────────────────────────────────────
void update_outputs(AlertLevel al) {
  digitalWrite(LED_NORMAL, LOW);
  digitalWrite(LED_WARN,   LOW);
  digitalWrite(LED_ALERT,  LOW);

  switch (al) {
    case NORMAL:
      digitalWrite(LED_NORMAL, HIGH);
      noTone(BUZZER_PIN);
      break;
    case WARNING:
      digitalWrite(LED_WARN, HIGH);
      if ((millis() / 1000) % 2 == 0) tone(BUZZER_PIN, 1000, 200);
      else                            noTone(BUZZER_PIN);
      break;
    case ANOMALY:
      digitalWrite(LED_ALERT, HIGH);
      tone(BUZZER_PIN, 2000, 500);
      break;
  }
}

// ─────────────────────────────────────────────────────────────
//  Serial Logging
// ─────────────────────────────────────────────────────────────
void log_serial(float t1, float h1, float t2, float h2,
                float zscore, AlertLevel al) {
  Serial.print("[");
  Serial.print(millis());
  Serial.print("ms] #");
  Serial.print(loop_counter);
  Serial.print("  LR: T="); Serial.print(t1, 1);
  Serial.print("C H=");     Serial.print(h1, 0);
  Serial.print("%  BR: T="); Serial.print(t2, 1);
  Serial.print("C H=");     Serial.print(h2, 0);
  Serial.print("%  Z=");    Serial.print(zscore, 3);
  Serial.print("  >> ");
  if      (al == ANOMALY) Serial.println("ANOMALY");
  else if (al == WARNING) Serial.println("WARNING");
  else                    Serial.println("NORMAL");
}

// ─────────────────────────────────────────────────────────────
//  Setup
// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("============================================");
  Serial.println("  Smart Home Climate AIoT System");
  Serial.println("  TinyML Z-Score Anomaly Detector");
  Serial.println("  ESP32-S3 + ThingSpeak");
  Serial.println("============================================");
  Serial.println("[PIN MAP]");
  Serial.println("  DHT22 Living Room → GPIO5");
  Serial.println("  DHT22 Bedroom     → GPIO4");
  Serial.println("  LCD SDA           → GPIO21");
  Serial.println("  LCD SCL           → GPIO20");
  Serial.println("  Green LED         → GPIO16");
  Serial.println("  Yellow LED        → GPIO17");
  Serial.println("  Red LED           → GPIO18");
  Serial.println("  Buzzer            → GPIO19");
  Serial.println("============================================\n");

  // Output pins
  pinMode(LED_NORMAL, OUTPUT);
  pinMode(LED_WARN,   OUTPUT);
  pinMode(LED_ALERT,  OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  // Startup blink — all LEDs 3x
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_NORMAL, HIGH);
    digitalWrite(LED_WARN,   HIGH);
    digitalWrite(LED_ALERT,  HIGH);
    tone(BUZZER_PIN, 1500, 100);
    delay(150);
    digitalWrite(LED_NORMAL, LOW);
    digitalWrite(LED_WARN,   LOW);
    digitalWrite(LED_ALERT,  LOW);
    delay(150);
  }

  // I2C — SDA=GPIO21, SCL=GPIO20
  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Climate AIoT    ");
  lcd.setCursor(0, 1);
  lcd.print("LD7182 Starting ");
  delay(1500);

  dht1.begin();  // GPIO5 — Living Room
  dht2.begin();  // GPIO4 — Bedroom

  connect_wifi();

  Serial.print("[MODEL] Baseline: T=");
  Serial.print(baseline_temp_mean); Serial.print("C +/-");
  Serial.print(baseline_temp_std);
  Serial.print("  H="); Serial.print(baseline_hum_mean);
  Serial.print("% +/-"); Serial.println(baseline_hum_std);
  Serial.println("[SYSTEM] Ready — inference loop starting\n");
}

// ─────────────────────────────────────────────────────────────
//  Main Loop
// ─────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  if (now - last_read_ms < READ_INTERVAL) {
    update_outputs(current_alert);
    delay(10);
    return;
  }
  last_read_ms = now;
  loop_counter++;

  // Read sensors
  g_temp1 = dht1.readTemperature();  // Living Room
  g_hum1  = dht1.readHumidity();
  g_temp2 = dht2.readTemperature();  // Bedroom
  g_hum2  = dht2.readHumidity();

  if (isnan(g_temp1) || isnan(g_hum1)) {
    Serial.println("[WARN] Living Room sensor failed — using baseline.");
    g_temp1 = baseline_temp_mean; g_hum1 = baseline_hum_mean;
  }
  if (isnan(g_temp2) || isnan(g_hum2)) {
    Serial.println("[WARN] Bedroom sensor failed — using baseline.");
    g_temp2 = baseline_temp_mean; g_hum2 = baseline_hum_mean;
  }

  float avg_temp = (g_temp1 + g_temp2) / 2.0f;
  float avg_hum  = (g_hum1  + g_hum2)  / 2.0f;

  // TinyML Pipeline
  update_online_stats(avg_temp, avg_hum);
  current_alert = run_inference(avg_temp, avg_hum, g_zscore);

  // Local outputs
  log_serial(g_temp1, g_hum1, g_temp2, g_hum2, g_zscore, current_alert);
  lcd_display(g_temp1, g_hum1, g_temp2, g_hum2, current_alert);
  update_outputs(current_alert);

  // ThingSpeak upload every 15 seconds
  if (now - last_thingspeak_ms >= THINGSPEAK_INTERVAL) {
    last_thingspeak_ms = now;
    Serial.println("[ThingSpeak] Uploading...");
    upload_to_thingspeak(g_temp1, g_hum1, g_temp2, g_hum2,
                         g_zscore, current_alert);
    lcd_display(g_temp1, g_hum1, g_temp2, g_hum2, current_alert);
  }
}
