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

// Rotation vector accuracy as reported by the sensor (0-3: Unreliable,
// Low, Medium, High). Shown in the web UI so users can tell a bad
// heading reading from magnetic interference apart from a real move.
uint8_t imuAccuracy = 0;

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
  imuAccuracy = imu.getQuatAccuracy();

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
  json += "\"imu\":\"" + String(imuModeName()) + "\",";
  json += "\"acc\":" + String(imuAccuracy);
  json += "}";
  webServer.send(200, "application/json", json);
}

void handleMode() {
  if (!imuReady) {
    webServer.send(409, "text/plain", "IMU not ready");
    return;
  }

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

// Daily-use view: live AZ/ALT, IMU mode toggle, language toggle, and a
// link to the WiFi settings page (kept off this page on purpose - it's
// what the captive portal opens, and a password/text input here used to
// grab autofocus/the keyboard on some phones the moment it popped up).
void handleRoot() {
  String html =
  "<html><head><meta charset='UTF-8'>"
  "<title>Telescope Status</title>"
  "<style>"
  "body{font-family:sans-serif;text-align:center;padding-top:40px;"
  "background:#1a1a1a;color:#eee;}"
  "h1{color:#ff6600;display:inline-block;margin:0 10px;}"
  ".val{font-size:3em;font-weight:bold;}"
  "button,a.btn{font-size:1.2em;padding:10px 20px;margin-top:20px;display:inline-block;}"
  "a.btn{color:#eee;text-decoration:none;border:1px solid #444;border-radius:4px;}"
  "#langBtn{font-size:0.9em;padding:4px 10px;margin:0;vertical-align:middle;}"
  "#wifiLink{font-size:0.9em;padding:6px 14px;margin-top:50px;opacity:0.7;}"
  "</style>"
  "</head><body>"

  "<h1 id='title'>Telescope Status</h1>"
  "<button id='langBtn' onclick='toggleLang()'>\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e</button>"
  "<div>AZ: <span id='az' class='val'>0</span>\xc2\xb0</div>"
  "<div>ALT: <span id='alt' class='val'>0</span>\xc2\xb0</div>"
  "<div style='margin-top:20px'><span id='imuLabel'>IMU:</span> <b id='imu'>?</b></div>"
  "<div id='accRow'><span id='accLabel'>Accuracy:</span> <b id='acc'>?</b></div>"
  "<br><button id='btn' onclick='toggleIMU()'>Switch</button>"
  "<br><a id='wifiLink' class='btn' href='/wifi'>WiFi Settings</a>"

  // Script goes last so getElementById() below finds elements that have
  // already been parsed - putting it in <head> meant applyLang() threw
  // trying to touch not-yet-existent elements, which silently aborted
  // the whole script and left AZ/ALT/IMU frozen at their placeholder text.
  "<script>"
  "const i18n={"
    "en:{title:'Telescope Status',imu_label:'IMU:',acc_label:'Accuracy:',"
      "switch_to_nocompass:'Switch to No-Compass',switch_to_compass:'Switch to Compass',"
      "mode_compass:'Compass Mode',mode_nocompass:'No-Compass Mode',mode_notfound:'Not Found',"
      "acc_0:'Unreliable',acc_1:'Low',acc_2:'Medium',acc_3:'High',"
      "wifi_link:'WiFi Settings',lang_btn:'\\u65e5\\u672c\\u8a9e'},"
    "ja:{title:'\\u671b\\u9060\\u93e1\\u30b9\\u30c6\\u30fc\\u30bf\\u30b9',imu_label:'IMU:',"
      "acc_label:'\\u7cbe\\u5ea6:',"
      "switch_to_nocompass:'\\u30b3\\u30f3\\u30d1\\u30b9\\u306a\\u3057\\u306b\\u5207\\u66ff',"
      "switch_to_compass:'\\u30b3\\u30f3\\u30d1\\u30b9\\u306b\\u5207\\u66ff',"
      "mode_compass:'\\u30b3\\u30f3\\u30d1\\u30b9\\u30e2\\u30fc\\u30c9',"
      "mode_nocompass:'\\u30b3\\u30f3\\u30d1\\u30b9\\u306a\\u3057\\u30e2\\u30fc\\u30c9',"
      "mode_notfound:'\\u672a\\u691c\\u51fa',"
      "acc_0:'\\u4fe1\\u983c\\u4e0d\\u53ef',acc_1:'\\u4f4e',acc_2:'\\u4e2d',acc_3:'\\u9ad8',"
      "wifi_link:'Wi-Fi\\u8a2d\\u5b9a',lang_btn:'English'}"
  "};"
  "let lang=localStorage.getItem('lang')||'en';"
  "let imuMode='';"
  "let imuAcc=-1;"
  "function t(k){return i18n[lang][k];}"
  "function applyLang(){"
    "document.title=t('title');"
    "document.getElementById('title').innerText=t('title');"
    "document.getElementById('imuLabel').innerText=t('imu_label');"
    "let btn=document.getElementById('btn');"
    "btn.disabled=(imuMode=='notfound');"
    "btn.innerText=imuMode=='nocompass'?t('switch_to_compass'):t('switch_to_nocompass');"
    "document.getElementById('wifiLink').innerText=t('wifi_link');"
    "document.getElementById('langBtn').innerText=t('lang_btn');"
    "document.getElementById('imu').innerText="
      "imuMode=='compass'?t('mode_compass'):"
      "imuMode=='nocompass'?t('mode_nocompass'):"
      "imuMode=='notfound'?t('mode_notfound'):'?';"
    "document.getElementById('accRow').style.display="
      "imuMode=='notfound'?'none':'block';"
    "document.getElementById('accLabel').innerText=t('acc_label');"
    "document.getElementById('acc').innerText="
      "imuAcc>=0?t('acc_'+imuAcc):'?';"
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
      "imuMode=d.imu.includes('Rotation')?'compass':d.imu.includes('Game')?'nocompass':'notfound';"
      "imuAcc=d.acc;"
      "applyLang();"
    "});"
  "}"
  "function toggleIMU(){"
    "if(imuMode=='notfound')return;"
    "let next=(imuMode=='compass')?'game':'rotation';"
    "fetch('/mode?imu='+next).then(()=>setTimeout(refresh,200));"
  "}"
  "applyLang();"
  "setInterval(refresh,500);"
  "</script>"

  "</body></html>";

  webServer.send(200, "text/html", html);
}

// Separate settings page (not shown by the captive portal) so a
// once-in-a-while task like changing the WiFi password doesn't get in
// the way of - or grab keyboard focus during - everyday use of the page
// above.
void handleWifiPage() {
  String html =
  "<html><head><meta charset='UTF-8'>"
  "<title>WiFi Settings</title>"
  "<style>"
  "body{font-family:sans-serif;text-align:center;padding-top:40px;"
  "background:#1a1a1a;color:#eee;}"
  "h1{color:#ff6600;font-size:1.4em;}"
  "button{font-size:1.2em;padding:10px 20px;margin-top:20px;}"
  "form{margin:20px auto 0;padding:20px;border:1px solid #444;background:#222;max-width:320px;}"
  "input{font-size:1em;padding:5px;margin:5px;width:80%;}"
  "a{color:#eee;}"
  "#langBtn{font-size:0.9em;padding:4px 10px;margin:0;vertical-align:middle;}"
  "</style>"
  "</head><body>"

  "<div><a id='backLink' href='/'>\xe2\x86\x90 Back</a> "
  "<button id='langBtn' onclick='toggleLang()' style='float:right'>\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e</button></div>"
  "<h1 id='title'>WiFi Settings</h1>"

  "<form action='/wifi' method='POST'>"
  "<input id='ssid' type='text' name='ssid' placeholder='SSID' value='" + htmlEscape(wifiSSID) + "' maxlength='32' required><br>"
  "<input id='pass' type='password' name='pass' placeholder='New Password' maxlength='63' required><br>"
  "<span id='chLabel'>Channel:</span> <select name='ch'>"
  "<option value='1'" + String(wifiChannel == 1 ? " selected" : "") + ">1ch</option>"
  "<option value='6'" + String(wifiChannel == 6 ? " selected" : "") + ">6ch</option>"
  "<option value='11'" + String(wifiChannel == 11 ? " selected" : "") + ">11ch</option>"
  "</select><br>"
  "<button id='updateBtn' type='submit'>Update WiFi</button>"
  "</form>"

  // Script goes last, after the elements above it (see handleRoot()).
  "<script>"
  "const i18n={"
    "en:{title:'WiFi Settings',back:'\\u2190 Back',ssid_ph:'SSID',pass_ph:'New Password',"
      "channel_label:'Channel:',update_btn:'Update WiFi',lang_btn:'\\u65e5\\u672c\\u8a9e'},"
    "ja:{title:'Wi-Fi\\u8a2d\\u5b9a',back:'\\u2190 \\u623b\\u308b',ssid_ph:'SSID',"
      "pass_ph:'\\u65b0\\u3057\\u3044\\u30d1\\u30b9\\u30ef\\u30fc\\u30c9',"
      "channel_label:'\\u30c1\\u30e3\\u30f3\\u30cd\\u30eb:',update_btn:'Wi-Fi\\u3092\\u66f4\\u65b0',lang_btn:'English'}"
  "};"
  "let lang=localStorage.getItem('lang')||'en';"
  "function t(k){return i18n[lang][k];}"
  "function applyLang(){"
    "document.title=t('title');"
    "document.getElementById('title').innerText=t('title');"
    "document.getElementById('backLink').innerText=t('back');"
    "document.getElementById('ssid').placeholder=t('ssid_ph');"
    "document.getElementById('pass').placeholder=t('pass_ph');"
    "document.getElementById('chLabel').innerText=t('channel_label');"
    "document.getElementById('updateBtn').innerText=t('update_btn');"
    "document.getElementById('langBtn').innerText=t('lang_btn');"
  "}"
  "function toggleLang(){"
    "lang=(lang=='en')?'ja':'en';"
    "localStorage.setItem('lang',lang);"
    "applyLang();"
  "}"
  "applyLang();"
  "</script>"

  "</body></html>";

  webServer.send(200, "text/html", html);
}

// ==================================================
// Factory reset (BOOT button)
// ==================================================
// Holding the DevKit's BOOT button (GPIO0, active LOW) for 5s clears the
// saved WiFi SSID/password/channel and reboots to the auto-generated
// defaults. This is the only way to recover if the WiFi password was
// changed and then forgotten - the web UI can't help at that point since
// you can no longer join the network to reach it.
constexpr int FACTORY_RESET_PIN = 0;
constexpr unsigned long FACTORY_RESET_HOLD_MS = 5000;
unsigned long factoryResetPressStart = 0;

void checkFactoryResetButton() {
  if (digitalRead(FACTORY_RESET_PIN) != LOW) {
    factoryResetPressStart = 0;
    return;
  }

  if (factoryResetPressStart == 0) {
    factoryResetPressStart = millis();
  } else if (millis() - factoryResetPressStart >= FACTORY_RESET_HOLD_MS) {
    Serial.println("Factory reset: clearing WiFi settings...");
    preferences.begin("wifi", false);
    preferences.clear();
    preferences.end();
    delay(200);
    ESP.restart();
  }
}

// ==================================================
// Setup
// ==================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(FACTORY_RESET_PIN, INPUT_PULLUP);

  // --- Prepare auto-generated SSID ---
  // Read the chip's factory-programmed eFuse MAC directly, rather than
  // via WiFi.softAPmacAddress(). The latter depends on the WiFi driver
  // being fully up, which is reliable after a cold power-on but was seen
  // to return an all-zero MAC (-> "IMUDSC_0000") right after ESP.restart()
  // (e.g. from the factory-reset button) - the eFuse read has no such
  // dependency.
  uint64_t chipId = ESP.getEfuseMac();
  char chipIdSuffix[5];
  snprintf(chipIdSuffix, sizeof(chipIdSuffix), "%04X", (uint16_t)(chipId & 0xFFFF));
  String defaultSSID = "IMUDSC_" + String(chipIdSuffix);

  WiFi.mode(WIFI_AP);

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
  webServer.on("/wifi", HTTP_GET, handleWifiPage);
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
  checkFactoryResetButton();

  WiFiClient client = skySafariServer.available();
  if (!client) return;

  while (client.connected()) {
    updatePosition();
    webServer.handleClient();
    dnsServer.processNextRequest();
    checkFactoryResetButton();

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
