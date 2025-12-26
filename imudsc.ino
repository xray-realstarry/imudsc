#include <WiFi.h>
#include <Wire.h>
#include <SparkFun_BNO08x_Arduino_Library.h>

// ---- Wi-Fi Settings ----
const char* ssid = "ESP32_Telescope";
const char* password = "12345678"; // WPA2 password
WiFiServer server(4030);

// ---- IMU ----
BNO08x myIMU;
float current_az = 0.0;
float current_alt = 0.0;

// ---- TCP Client ----
WiFiClient client;

// ---- Update telescope position from BNO08x ----
void updatePosition() {
  if (myIMU.getSensorEvent() == true) {
    uint8_t reportID = myIMU.getSensorEventID();

    if (reportID == SENSOR_REPORTID_ROTATION_VECTOR ||
        reportID == SENSOR_REPORTID_AR_VR_STABILIZED_ROTATION_VECTOR) {

      const float radToDeg = 180.0 / PI;
      float raw_az = myIMU.getYaw() * radToDeg;
      float raw_alt = myIMU.getPitch() * radToDeg;

      // Normalize Azimuth 0-360
      current_az = 360.0 - raw_az;
      while (current_az < 0) current_az += 360.0;
      while (current_az >= 360.0) current_az -= 360.0;

      // Normalize Altitude 0-360
      current_alt = raw_alt;
      while (current_alt < 0) current_alt += 360.0;
      while (current_alt >= 360.0) current_alt -= 360.0;
    }
  }
}

// ---- Send encoder steps to client ----
void sendEncoder(WiFiClient &c) {
  long az_steps  = (long)(current_az  * 100.0);
  long alt_steps = (long)(current_alt * 100.0);

  az_steps  = (az_steps  % 36000 + 36000) % 36000;
  alt_steps = (alt_steps % 36000 + 36000) % 36000;

  c.print(az_steps);
  c.print("\t");
  c.print(alt_steps);
  c.print("\r\n");
  c.flush();  // Ensure packet is sent immediately

  //Serial.print("Sent: ");
  //Serial.print(az_steps);
  //Serial.print("\t");
  //Serial.println(alt_steps);
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  Wire.setClock(400000);

  // ---- Initialize IMU ----
  if (!myIMU.begin(0x4A) && !myIMU.begin(0x4B)) {
    while (1) {
      Serial.println(F("IMU Error: Could not find BNO08x"));
      delay(1000);
    }
  }

  if (myIMU.enableARVRStabilizedRotationVector(20)) {
    Serial.println(F("AR/VR Stabilized Rotation vector enabled"));
  } else if (myIMU.enableRotationVector(20)) {
    Serial.println(F("Rotation vector enabled (Fallback)"));
  } else {
    Serial.println(F("Failed to enable any rotation vector"));
  }

  // ---- Wi-Fi SoftAP ----
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAP(ssid, password, 1, false, 1); // WPA2, channel=1, max 1 client
  server.begin();
  Serial.println("SkySafari Encoder Server Ready (Reversed Az)");
}

void loop() {
  updatePosition();

  // ---- Accept client ----
  if (!client || !client.connected()) {
    client = server.available();
    if (client) {
      client.setNoDelay(true);
      //Serial.println("SkySafari Connected");
    }
  }

  // ---- Handle incoming requests ----
  if (client && client.connected()) {
    while (client.available()) {
      char c = client.read();

      //Serial.print("Received: ");
      //if (c == '\r') Serial.println("\\r");
      //else if (c == '\n') Serial.println("\\n");
      //else Serial.println(c);

      // Respond to Q/q immediately
      if (c == 'Q' || c == 'q') {
        sendEncoder(client);
      }
      // Ignore other characters
    }
  }
}
