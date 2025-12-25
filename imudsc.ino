#include <WiFi.h>
#include <Wire.h>
#include <SparkFun_BNO08x_Arduino_Library.h>

const char* ssid = "ESP32_Telescope";
const char* password = "12345678";
WiFiServer server(4030);
BNO08x myIMU;

float current_az = 0.0;
float current_alt = 0.0;

/**
 * Update current Azimuth and Altitude from IMU sensor
 */
void updatePosition() {
  if (myIMU.getSensorEvent() && myIMU.getSensorEventID() == SENSOR_REPORTID_ROTATION_VECTOR) {
    // Get raw orientation in degrees
    float raw_az = myIMU.getYaw() * 180.0 / PI;
    current_alt = myIMU.getPitch() * 180.0 / PI;

    // --- FIX: Reverse the horizontal direction ---
    // Subtracting from 360 flips the left/right movement
    current_az = 360.0 - raw_az;

    // Normalize Azimuth to 0-359.99
    while (current_az < 0) current_az += 360;
    while (current_az >= 360) current_az -= 360;
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  Wire.setClock(400000); 

  // Initialize BNO08x IMU
  if (!myIMU.begin(0x4A) && !myIMU.begin(0x4B)) {
    while (1) { Serial.println("IMU Error"); delay(1000); }
  }
  
  // Enable Rotation Vector with 10ms interval (100Hz)
  myIMU.enableRotationVector(10); 

  WiFi.softAP(ssid, password);
  server.begin();
  Serial.println("High-Resolution Encoder System Ready (Reversed Az)");
}

void loop() {
  updatePosition(); 

  WiFiClient client = server.available();
  if (client) {
    client.setNoDelay(true);
    
    while (client.connected()) {
      updatePosition(); 

      if (client.available()) {
        char c = client.read();
        
        if (c == 'q' || c == 'Q') {
          // Resolution: 1 degree = 100 steps
          // Ensure SkySafari's "Encoder Steps Per Revolution" is set to +36000
          long az_steps = (long)(current_az * 100.0);
          long alt_steps = (long)(current_alt * 100.0);

          client.print(String(az_steps));
          client.print("\t");
          client.print(String(alt_steps));
          client.print("\r");
        }
      }
    }
  }
}