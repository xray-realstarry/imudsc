#include <WiFi.h>
#include <Wire.h>
#include <WebServer.h>
#include <SparkFun_BNO08x_Arduino_Library.h>
#include <Preferences.h>

// ==================================================
// Wi-Fi Settings (SoftAP)
// ==================================================
Preferences preferences;
String wifiSSID = "ESP32_Telescope";
String wifiPASS = "12345678";
int wifiChannel = 11;
WiFiServer skySafariServer(4030);

// ==================================================
// Web Settings
// ==================================================
WebServer webServer(80);

// ==================================================
// IMU
// ==================================================
BNO08x imu;

// ==================================================
// IMU Mode
// ==================================================
enum ImuMode {
  IMU_ROTATION,
  IMU_GAME
};

ImuMode imuMode = IMU_ROTATION;

const char* imuModeName() {
  return (imuMode == IMU_ROTATION)
         ? "Rotation (Mag)"
         : "Game (No Mag)";
}

// ==================================================
// Encoder settings (BBox)
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
  while (deg < 0) deg += 360.0f;
  while (deg >= 360.0f) deg -= 360.0f;
  return deg;
}

// ==================================================
// IMU mode switch
// ==================================================
void setImuMode(ImuMode mode) {
  // --- stop both reports ---
  imu.enableReport(SENSOR_REPORTID_ROTATION_VECTOR, 0);
  imu.enableReport(SENSOR_REPORTID_GAME_ROTATION_VECTOR, 0);

  if (mode == IMU_ROTATION) {
    imu.enableRotationVector(20);
    Serial.println("IMU: Rotation Vector (Mag)");
  } else {
    imu.enableGameRotationVector(20);
    Serial.println("IMU: Game Rotation Vector (No Mag)");
  }
  imuMode = mode;
}

// ==================================================
// Update position from IMU
// ==================================================
void updatePosition() {
  if (!imu.getSensorEvent()) return;

  if (imuMode == IMU_ROTATION &&
      imu.getSensorEventID() != SENSOR_REPORTID_ROTATION_VECTOR) return;

  if (imuMode == IMU_GAME &&
      imu.getSensorEventID() != SENSOR_REPORTID_GAME_ROTATION_VECTOR) return;

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
  snprintf(buf, sizeof(buf), "%+06ld\t%+06ld\r",
           az_counter, alt_counter);
  c.write((uint8_t*)buf, strlen(buf));
}

void sendResolution(WiFiClient &c) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%ld-%ld\r", AZ_RES, ALT_RES);
  c.write((uint8_t*)buf, strlen(buf));
}

// ==================================================
// Web Handlers
// ==================================================
void handleData() {
  String json = "{";
  json += "\"az\":" + String(current_az_deg) + ",";
  json += "\"alt\":" + String(current_alt_deg) + ",";
  json += "\"imu\":\"" + String(imuModeName()) + "\"";
  json += "}";
  webServer.send(200, "application/json", json);
}

void handleMode() {
  if (!webServer.hasArg("imu")) {
    webServer.send(400, "text/plain", "missing imu");
    return;
  }

  String m = webServer.arg("imu");
  if (m == "rotation") setImuMode(IMU_ROTATION);
  else if (m == "game") setImuMode(IMU_GAME);
  else {
    webServer.send(400, "text/plain", "bad mode");
    return;
  }

  webServer.send(200, "text/plain", "OK");
}

void handleWifiSettings() {
  if (!webServer.hasArg("ssid") || !webServer.hasArg("pass")) {
    webServer.send(400, "text/plain", "missing ssid or pass");
    return;
  }

  String newSSID = webServer.arg("ssid");
  String newPASS = webServer.arg("pass");
  int newCH = 11;
  if (webServer.hasArg("ch")) {
    newCH = webServer.arg("ch").toInt();
  }

  // Add validation for WPA2 password minimum length
  if (newPASS.length() < 8) {
    webServer.send(400, "text/plain", "Password must be at least 8 characters");
    return;
  }

  preferences.begin("wifi", false);
  preferences.putString("ssid", newSSID);
  preferences.putString("pass", newPASS);
  preferences.putInt("ch", newCH);
  preferences.end();

  webServer.send(200, "text/plain", "WiFi settings updated. Rebooting...");
  delay(1000);
  ESP.restart();
}

void handleRoot() {
  String html =
  "<html><head><meta charset='UTF-8'>"
  "<title>Telescope Status</title>"
  "<style>"
  "body{font-family:sans-serif;text-align:center;padding-top:40px;"
  "background:#1a1a1a;color:#eee;}"
  "h1{color:#ff6600;}"
  ".val{font-size:3em;font-weight:bold;}"
  "button{font-size:1.2em;padding:10px 20px;margin-top:20px;}"
  "form{margin-top:40px;padding:20px;border:1px solid #444;background:#222;}"
  "input{font-size:1em;padding:5px;margin:5px;}"
  "</style>"

  "<script>"
  "let imuMode='';"
  "function refresh(){"
    "fetch('/data').then(r=>r.json()).then(d=>{"
      "document.getElementById('az').innerText=d.az.toFixed(2);"
      "document.getElementById('alt').innerText=d.alt.toFixed(2);"
      "document.getElementById('imu').innerText=d.imu;"
      "imuMode=d.imu.includes('Rotation')?'rotation':'game';"
      "document.getElementById('btn').innerText="
        "imuMode=='rotation'?'Switch to Game':'Switch to Rotation';"
    "});"
  "}"
  "function toggleIMU(){"
    "let next=(imuMode=='rotation')?'game':'rotation';"
    "fetch('/mode?imu='+next).then(()=>setTimeout(refresh,200));"
  "}"
  "setInterval(refresh,500);"
  "</script></head><body>"

  "<h1>Telescope Status</h1>"
  "<div>AZ: <span id='az' class='val'>0</span>°</div>"
  "<div>ALT: <span id='alt' class='val'>0</span>°</div>"
  "<div style='margin-top:20px'>IMU: <b id='imu'>?</b></div>"
  "<button id='btn' onclick='toggleIMU()'>Switch</button>"

  "<form action='/wifi' method='POST'>"
  "<h2>WiFi Settings</h2>"
  "<input type='text' name='ssid' placeholder='SSID' value='" + wifiSSID + "' required><br>"
  "<input type='password' name='pass' placeholder='Password' value='" + wifiPASS + "' required><br>"
  "Channel: <select name='ch'>"
  "<option value='1'" + String(wifiChannel == 1 ? " selected" : "") + ">1ch</option>"
  "<option value='6'" + String(wifiChannel == 6 ? " selected" : "") + ">6ch</option>"
  "<option value='11'" + String(wifiChannel == 11 ? " selected" : "") + ">11ch</option>"
  "</select><br>"
  "<button type='submit'>Update WiFi</button>"
  "</form>"

  "</body></html>";

  webServer.send(200, "text/html", html);
}

// ==================================================
// Setup
// ==================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  preferences.begin("wifi", false);
  wifiSSID = preferences.getString("ssid", "ESP32_Telescope");
  wifiPASS = preferences.getString("pass", "12345678");
  wifiChannel = preferences.getInt("ch", 11);
  preferences.end();

  Wire.begin(21, 22);
  Wire.setClock(400000);

  if (!imu.begin(0x4A) && !imu.begin(0x4B)) {
    Serial.println("ERROR: BNO08x not found");
    while (true);
  }

  setImuMode(IMU_ROTATION);

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAP(wifiSSID.c_str(), wifiPASS.c_str(), wifiChannel);

  skySafariServer.begin();

  webServer.on("/", handleRoot);
  webServer.on("/data", handleData);
  webServer.on("/mode", handleMode);
  webServer.on("/wifi", HTTP_POST, handleWifiSettings);
  webServer.begin();

  Serial.println("SkySafari BBox Encoder Ready");
}

// ==================================================
// Main loop
// ==================================================
void loop() {
  updatePosition();
  webServer.handleClient();

  WiFiClient client = skySafariServer.available();
  if (!client) return;

  while (client.connected()) {
    updatePosition();
    webServer.handleClient();

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
