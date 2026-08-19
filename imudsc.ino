#include <WiFi.h>
#include <Wire.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_netif.h>
#include <SparkFun_BNO08x_Arduino_Library.h>
#include <Preferences.h>

// ==================================================
// Wi-Fi Settings (SoftAP)
// ==================================================
Preferences preferences;
String wifiSSID = "";
String wifiPASS = "";
int wifiChannel = 11;
WiFiServer skySafariServer(4030);

constexpr char DEFAULT_WIFI_PASS[]  = "12345678";
constexpr int  DEFAULT_WIFI_CHANNEL = 11;

// ==================================================
// Captive Portal
// ==================================================
constexpr int DNS_PORT = 53;
DNSServer dnsServer;

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
// Set from imuInitTask (a separate FreeRTOS task); read from loop(). IMU
// init can block for a long time (or, occasionally, indefinitely - see
// imuInitTask), so it must never run on the same task as the Wi-Fi/web/
// DNS/BBox servers, and this flag needs to be visible across that task
// boundary.
volatile bool imuReady = false;

const char* imuModeName() {
  if (!imuReady) return "Not Found";
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
  // --- Stop both reports ---
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
// IMU init (runs on its own task)
// ==================================================
// imu.begin() has been observed to hang indefinitely (SparkFun library,
// SH2 handshake) if the sensor was left mid-stream by a prior firmware
// and the ESP32 alone gets reset (no power cycle). Running it on its own
// task - instead of inline in setup() - keeps that hang from blocking
// the Wi-Fi/web/DNS/BBox servers, which all come up first in setup().
void imuInitTask(void *pvParameters) {
  Wire.begin(21, 22);
  Wire.setClock(400000);

  if (imu.begin(0x4A) || imu.begin(0x4B)) {
    setImuMode(IMU_ROTATION);
    imuReady = true;
    Serial.println("IMU ready");
  } else {
    Serial.println("ERROR: BNO08x not found - continuing without IMU");
  }

  vTaskDelete(NULL);
}

// ==================================================
// Update position from IMU
// ==================================================
void updatePosition() {
  if (!imuReady) return;
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

  // --- Apply zero-point calibration ---
  current_az_deg  = normalize360(yaw);
  current_alt_deg = pitch;

  // --- Degrees -> encoder counts ---
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

// SSID: 1-32 bytes (802.11 limit). Password: 8-63 chars (WPA2-PSK ASCII limit).
bool isValidWifiSSID(const String &ssid) {
  return ssid.length() >= 1 && ssid.length() <= 32;
}

bool isValidWifiPassword(const String &pass) {
  return pass.length() >= 8 && pass.length() <= 63;
}

bool isValidWifiChannel(int ch) {
  return ch >= 1 && ch <= 13;
}

// Escapes text before embedding it into a single-quoted HTML attribute.
String htmlEscape(const String &s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += c;
    }
  }
  return out;
}

void handleWifiSettings() {
  if (!webServer.hasArg("ssid") || !webServer.hasArg("pass")) {
    webServer.send(400, "text/plain", "missing ssid or pass");
    return;
  }

  String newSSID = webServer.arg("ssid");
  String newPASS = webServer.arg("pass");

  if (!isValidWifiSSID(newSSID)) {
    webServer.send(400, "text/plain", "SSID must be 1-32 characters");
    return;
  }
  if (!isValidWifiPassword(newPASS)) {
    webServer.send(400, "text/plain", "Password must be 8-63 characters");
    return;
  }

  int newCH = DEFAULT_WIFI_CHANNEL;
  if (webServer.hasArg("ch")) {
    newCH = webServer.arg("ch").toInt();
    if (!isValidWifiChannel(newCH)) {
      webServer.send(400, "text/plain", "Channel must be 1-13");
      return;
    }
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

// Answers OS captive-portal connectivity checks (Apple/Android/Windows all
// probe a well-known URL right after joining Wi-Fi) with a redirect to the
// root page, so phones/laptops pop the settings page open automatically.
// A small HTML body with a meta-refresh + JS redirect is included as a
// fallback for clients that render the response body instead of following
// the Location header.
void handleCaptivePortal() {
  String target = String("http://") + WiFi.softAPIP().toString() + "/";
  String body =
    "<html><head><meta http-equiv='refresh' content='0;url=" + target + "'>"
    "<script>location.replace('" + target + "');</script></head>"
    "<body>Redirecting to <a href='" + target + "'>" + target + "</a></body></html>";
  webServer.sendHeader("Location", target, true);
  webServer.send(302, "text/html", body);
}

// RFC 8910/8908: advertise the captive portal URI via DHCP option 114, so
// modern OSes (iOS/iPadOS 14+, recent Android) can detect the portal
// directly from the DHCP offer instead of relying solely on the DNS/HTTP
// probe tricks above - this isn't defeated by a client routing its
// connectivity-check DNS query elsewhere (e.g. iCloud Private Relay).
// Requires ESP32 Arduino core 3.3.0+ (ESP-IDF 5.4+, update via Boards
// Manager) - ESP_NETIF_CAPTIVEPORTAL_URI does not exist on older cores,
// so this is a compile-time requirement, not a runtime fallback.
void setupCaptivePortalDhcpOption() {
  String uri = String("http://") + WiFi.softAPIP().toString() + "/";
  esp_netif_t *apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (!apNetif) {
    Serial.println("WARNING: DHCP captive portal option: no AP netif");
    return;
  }

  esp_netif_dhcps_stop(apNetif);
  esp_err_t err = esp_netif_dhcps_option(apNetif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                          (void *)uri.c_str(), uri.length());
  esp_netif_dhcps_start(apNetif);

  if (err == ESP_OK) {
    Serial.println("DHCP captive portal option (114) set: " + uri);
  } else {
    Serial.println("WARNING: DHCP captive portal option failed: " + String(esp_err_to_name(err)));
  }
}

void handleRoot() {
  String html =
  "<html><head><meta charset='UTF-8'>"
  "<title>Telescope Status</title>"
  "<style>"
  "body{font-family:sans-serif;text-align:center;padding-top:40px;"
  "background:#1a1a1a;color:#eee;}"
  "h1{color:#ff6600;display:inline-block;margin:0 10px;}"
  ".val{font-size:3em;font-weight:bold;}"
  "button{font-size:1.2em;padding:10px 20px;margin-top:20px;}"
  "form{margin-top:40px;padding:20px;border:1px solid #444;background:#222;}"
  "input{font-size:1em;padding:5px;margin:5px;}"
  "#langBtn{font-size:0.9em;padding:4px 10px;margin:0;vertical-align:middle;}"
  "</style>"

  "<script>"
  "const i18n={"
    "en:{title:'Telescope Status',imu_label:'IMU:',"
      "switch_to_game:'Switch to Game',switch_to_rotation:'Switch to Rotation',"
      "mode_rotation:'Rotation (Mag)',mode_game:'Game (No Mag)',"
      "wifi_heading:'WiFi Settings',ssid_ph:'SSID',pass_ph:'New Password',"
      "channel_label:'Channel:',update_btn:'Update WiFi',lang_btn:'\\u65e5\\u672c\\u8a9e'},"
    "ja:{title:'\\u671b\\u9060\\u93e1\\u30b9\\u30c6\\u30fc\\u30bf\\u30b9',imu_label:'IMU:',"
      "switch_to_game:'\\u30b2\\u30fc\\u30e0\\u30e2\\u30fc\\u30c9\\u306b\\u5207\\u66ff',"
      "switch_to_rotation:'\\u56de\\u8ee2\\u30e2\\u30fc\\u30c9\\u306b\\u5207\\u66ff',"
      "mode_rotation:'\\u56de\\u8ee2 (\\u78c1\\u6c17\\u3042\\u308a)',"
      "mode_game:'\\u30b2\\u30fc\\u30e0 (\\u78c1\\u6c17\\u306a\\u3057)',"
      "wifi_heading:'Wi-Fi\\u8a2d\\u5b9a',ssid_ph:'SSID',pass_ph:'\\u65b0\\u3057\\u3044\\u30d1\\u30b9\\u30ef\\u30fc\\u30c9',"
      "channel_label:'\\u30c1\\u30e3\\u30f3\\u30cd\\u30eb:',update_btn:'Wi-Fi\\u3092\\u66f4\\u65b0',lang_btn:'English'}"
  "};"
  "let lang=localStorage.getItem('lang')||'en';"
  "let imuMode='';"
  "function t(k){return i18n[lang][k];}"
  "function applyLang(){"
    "document.title=t('title');"
    "document.getElementById('title').innerText=t('title');"
    "document.getElementById('imuLabel').innerText=t('imu_label');"
    "document.getElementById('btn').innerText="
      "imuMode=='game'?t('switch_to_rotation'):t('switch_to_game');"
    "document.getElementById('wifiHeading').innerText=t('wifi_heading');"
    "document.getElementById('ssid').placeholder=t('ssid_ph');"
    "document.getElementById('pass').placeholder=t('pass_ph');"
    "document.getElementById('chLabel').innerText=t('channel_label');"
    "document.getElementById('updateBtn').innerText=t('update_btn');"
    "document.getElementById('langBtn').innerText=t('lang_btn');"
    "document.getElementById('imu').innerText="
      "imuMode=='game'?t('mode_game'):(imuMode=='rotation'?t('mode_rotation'):'?');"
  "}"
  "function toggleLang(){"
    "lang=(lang=='en')?'ja':'en';"
    "localStorage.setItem('lang',lang);"
    "applyLang();"
  "}"
  "function refresh(){"
    "fetch('/data').then(r=>r.json()).then(d=>{"
      "document.getElementById('az').innerText=d.az.toFixed(2);"
      "document.getElementById('alt').innerText=d.alt.toFixed(2);"
      "imuMode=d.imu.includes('Rotation')?'rotation':'game';"
      "applyLang();"
    "});"
  "}"
  "function toggleIMU(){"
    "let next=(imuMode=='rotation')?'game':'rotation';"
    "fetch('/mode?imu='+next).then(()=>setTimeout(refresh,200));"
  "}"
  "applyLang();"
  "setInterval(refresh,500);"
  "</script></head><body>"

  "<h1 id='title'>Telescope Status</h1>"
  "<button id='langBtn' onclick='toggleLang()'>\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e</button>"
  "<div>AZ: <span id='az' class='val'>0</span>\xc2\xb0</div>"
  "<div>ALT: <span id='alt' class='val'>0</span>\xc2\xb0</div>"
  "<div style='margin-top:20px'><span id='imuLabel'>IMU:</span> <b id='imu'>?</b></div>"
  "<button id='btn' onclick='toggleIMU()'>Switch</button>"

  "<form action='/wifi' method='POST'>"
  "<h2 id='wifiHeading'>WiFi Settings</h2>"
  "<input id='ssid' type='text' name='ssid' placeholder='SSID' value='" + htmlEscape(wifiSSID) + "' maxlength='32' required><br>"
  "<input id='pass' type='password' name='pass' placeholder='New Password' maxlength='63' required><br>"
  "<span id='chLabel'>Channel:</span> <select name='ch'>"
  "<option value='1'" + String(wifiChannel == 1 ? " selected" : "") + ">1ch</option>"
  "<option value='6'" + String(wifiChannel == 6 ? " selected" : "") + ">6ch</option>"
  "<option value='11'" + String(wifiChannel == 11 ? " selected" : "") + ">11ch</option>"
  "</select><br>"
  "<button id='updateBtn' type='submit'>Update WiFi</button>"
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

  // --- Prepare auto-generated SSID ---
  // Initialize Wi-Fi in AP mode first to get the MAC address
  WiFi.mode(WIFI_AP);
  String mac = WiFi.softAPmacAddress(); // e.g., "AA:BB:CC:11:22:33"
  mac.replace(":", "");                 // Remove colons -> "AABBCC112233"
  
  // Create default SSID using the last 4 characters
  // e.g., "IMUDSC_" + "2233" -> "IMUDSC_2233"
  String defaultSSID = "IMUDSC_" + mac.substring(8); 

  preferences.begin("wifi", false);

  // Use the generated defaultSSID as the fallback
  wifiSSID = preferences.getString("ssid", defaultSSID);
  wifiPASS = preferences.getString("pass", DEFAULT_WIFI_PASS);
  wifiChannel = preferences.getInt("ch", DEFAULT_WIFI_CHANNEL);
  preferences.end();

  // Guard against invalid/stale NVS data (e.g. from a build predating
  // validation) so the AP always comes up and stays reachable to fix it.
  if (!isValidWifiSSID(wifiSSID)) {
    Serial.println("WARNING: Stored SSID invalid, falling back to " + defaultSSID);
    wifiSSID = defaultSSID;
  }
  if (!isValidWifiPassword(wifiPASS)) {
    Serial.println("WARNING: Stored password invalid, falling back to default");
    wifiPASS = DEFAULT_WIFI_PASS;
  }
  if (!isValidWifiChannel(wifiChannel)) {
    wifiChannel = DEFAULT_WIFI_CHANNEL;
  }

  // Display currently loaded settings to Serial Monitor (for debugging)
  Serial.println("-------------------------");
  Serial.println("Attempting to start AP with:");
  Serial.println("SSID: [" + wifiSSID + "]");
  Serial.println("PASS length: " + String(wifiPASS.length()));
  Serial.println("CH:   [" + String(wifiChannel) + "]");
  Serial.println("-------------------------");

  // Bring up Wi-Fi / the captive portal / web UI / BBox server *before*
  // touching the IMU, so the device stays reachable on the network even
  // if the sensor is missing or its init hangs (see updatePosition(),
  // which no-ops until imuReady is set below).
  WiFi.setSleep(false);
  WiFi.softAP(wifiSSID.c_str(), wifiPASS.c_str(), wifiChannel);

  // Captive portal: answer every DNS query with our own IP, and redirect
  // any unrecognized HTTP path there too, so connecting to the Wi-Fi
  // network pops the settings page open automatically. Also advertise it
  // via DHCP option 114 for OSes that support the modern RFC 8910 path.
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  setupCaptivePortalDhcpOption();

  skySafariServer.begin();

  webServer.on("/", handleRoot);
  webServer.on("/data", handleData);
  webServer.on("/mode", handleMode);
  webServer.on("/wifi", HTTP_POST, handleWifiSettings);
  webServer.onNotFound(handleCaptivePortal);
  webServer.begin();

  Serial.println("SkySafari BBox Encoder Ready");

  // IMU init runs on its own task so a hang there can't block the servers
  // started above (see imuInitTask).
  xTaskCreatePinnedToCore(imuInitTask, "imuInit", 4096, NULL, 1, NULL, 1);
}

// ==================================================
// Main loop
// ==================================================
void loop() {
  updatePosition();
  webServer.handleClient();
  dnsServer.processNextRequest();

  WiFiClient client = skySafariServer.available();
  if (!client) return;

  while (client.connected()) {
    updatePosition();
    webServer.handleClient();
    dnsServer.processNextRequest();

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
