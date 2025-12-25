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
 * Update current Azimuth and Altitude from IMU sensor.
 * Adjusted for SparkFun BNO08x library function names and constants.
 */
void updatePosition() {
  // Use getSensorEvent() to check for new data and update internal states
  if (myIMU.getSensorEvent() == true) {
    uint8_t reportID = myIMU.getSensorEventID();
    
    // Check if the report is either standard or AR/VR stabilized rotation vector
    // Note: Constants fixed with "AR_VR" as per compiler suggestion
    if (reportID == SENSOR_REPORTID_ROTATION_VECTOR || 
        reportID == SENSOR_REPORTID_AR_VR_STABILIZED_ROTATION_VECTOR) {
      
      const float radToDeg = 180.0 / PI;

      // Retrieve orientation data
      float raw_az = myIMU.getYaw() * radToDeg;
      current_alt = myIMU.getPitch() * radToDeg;

      // --- FIX: Reverse the horizontal direction for telescope mount logic ---
      current_az = 360.0 - raw_az;

      // Normalize Azimuth to ensure it stays within 0.0 - 359.99 range
      while (current_az < 0) current_az += 360;
      while (current_az >= 360) current_az -= 360;
    }
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  Wire.setClock(400000); 

  // Initialize BNO08x IMU (Try both default addresses)
  if (!myIMU.begin(0x4A) && !myIMU.begin(0x4B)) {
    while (1) { 
      Serial.println(F("IMU Error: Could not find BNO08x")); 
      delay(1000); 
    }
  }
  
  // Try to enable Stabilized Rotation Vector first
  if (myIMU.enableARVRStabilizedRotationVector(10) == true) { 
    Serial.println(F("AR/VR Stabilized Rotation vector enabled"));
  }
  else if (myIMU.enableRotationVector(10) == true) { 
    // If Stabilized fails, fall back to standard Rotation Vector
    Serial.println(F("Rotation vector enabled (Fallback)"));
  }
  else {
    Serial.println(F("Failed to enable any rotation vector"));
  }

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
      // Small delay to allow ESP32 to handle background tasks (like WiFi)
      delay(5);
    }
  }
}