#include <WiFi.h>
#include <Wire.h>
#include <SparkFun_BNO08x_Arduino_Library.h>

// ==================================================
// Wi-Fi Settings (SoftAP)
// ==================================================
constexpr char WIFI_SSID[] = "ESP32_Telescope";
constexpr char WIFI_PASS[] = "12345678";
WiFiServer server(4030);

// ==================================================
// IMU
// ==================================================
BNO08x imu;

// ==================================================
// BBox encoder settings
// ==================================================
constexpr float AZ_STEPS_PER_DEG  = 100.0f;
constexpr float ALT_STEPS_PER_DEG = 100.0f;

constexpr long AZ_RES  = long(360.0f * AZ_STEPS_PER_DEG);
constexpr long ALT_RES = long(360.0f * ALT_STEPS_PER_DEG);

// ==================================================
// Current angles (degrees, astronomy convention)
// ==================================================
float current_az_deg  = 0.0f;
float current_alt_deg = 0.0f;

// ==================================================
// Encoder counters (BBox wrapped)
// ==================================================
long az_counter  = 0;
long alt_counter = 0;

// ==================================================
// Utility
// ==================================================
float normalize360(float deg) {
  while (deg < 0)   deg += 360.0f;
  while (deg >= 360.0f) deg -= 360.0f;
  return deg;
}

// ==================================================
// Update telescope position from BNO086
// ==================================================
void updatePosition() {
  if (!imu.getSensorEvent()) return;
  if (imu.getSensorEventID() != SENSOR_REPORTID_ROTATION_VECTOR) return;

  // --- IMU raw angles (deg) ---
  float yaw   = imu.getYaw()   * 180.0f / PI;
  float pitch = imu.getPitch() * 180.0f / PI;

  // --- IMU -> astronomy sign convention ---
  yaw   = -yaw;
  pitch = -pitch;

  // --- apply zero-point calibration ---
  current_az_deg  = normalize360(yaw);
  current_alt_deg = pitch;

  // --- degrees -> encoder counts ---
  az_counter  = long(current_az_deg  * AZ_STEPS_PER_DEG);
  alt_counter = long(current_alt_deg * ALT_STEPS_PER_DEG);

  // --- BBox wrap ---
  az_counter  = (az_counter  % AZ_RES  + AZ_RES)  % AZ_RES;
  alt_counter = (alt_counter % ALT_RES + ALT_RES) % ALT_RES;
}

// ==================================================
// BBox protocol responses
// ==================================================
void sendPosition(WiFiClient &c) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%+06ld\t%+06ld\r", az_counter, alt_counter);
  c.write((uint8_t*)buf, strlen(buf));
}

void sendResolution(WiFiClient &c) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%ld-%ld\r", AZ_RES, ALT_RES);
  c.write((uint8_t*)buf, strlen(buf));
}

// ==================================================
// Setup
// ==================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Wire.begin(21, 22);
  Wire.setClock(400000);

  if (!imu.begin(0x4A) && !imu.begin(0x4B)) {
    Serial.println("ERROR: BNO08x not found");
    while (true);
  }

  imu.enableRotationVector(20);

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAP(WIFI_SSID, WIFI_PASS);
  server.begin();

  Serial.println("SkySafari BBox Encoder Ready");
}

// ==================================================
// Main loop
// ==================================================
void loop() {
  updatePosition();

  WiFiClient client = server.available();
  if (!client) return;

  while (client.connected()) {
    updatePosition();
    if (!client.available()) continue;

    char cmd = client.read();
    switch (cmd) {
      case 'Q': sendPosition(client);   break;
      case 'H': sendResolution(client); break;
      default:  break;
    }
    yield();
  }

  client.stop();
}
