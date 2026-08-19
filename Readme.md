# IMU DSC

This project implements a **Push-To telescope controller** using an **ESP32** and **BNO086 (IMU)**. It emulates the "Basic Encoder (BBox)" protocol, allowing you to visualize your telescope's real-time orientation directly in **SkySafari**.

## Features
* **BNO086 Integration**: High-precision orientation using the Rotation Vector sensor.
* **SkySafari Compatibility**: Emulates the BBox (Tangent) protocol over Wi-Fi.
* **SoftAP Mode**: The ESP32 acts as an Access Point—no router needed in the field.
* **Configurable Wi-Fi**: Customize the SoftAP SSID/password/channel from the web UI; settings persist across reboots.
* **Captive Portal**: The settings page pops up automatically when you join the Wi-Fi network, like a hotel/cafe Wi-Fi login page.
* **Low Latency**: Optimized sensor polling for smooth tracking.

## Hardware Requirements
* **ESP32** (e.g., DevKit V1)
* **BNO08x (BNO086/BNO085) IMU**
* **Telescope** (Alt-Azimuth mount)

### Pin Mapping (Default)
| ESP32 Pin | BNO086 Pin | Description |
|:---:|:---:|:--- |
| **GPIO 21** | SDA | I2C Data |
| **GPIO 22** | SCL | I2C Clock |
| **3.3V** | VCC | Power |
| **GND** | GND | Ground |

## Software Dependencies
* **ESP32 Arduino core 3.3.0 or later** (Boards Manager > esp32). Needed for DHCP option 114 (RFC 8910) captive-portal support - the sketch won't compile on older cores.
* **SparkFun BNO08x Arduino Library** (by SparkFun Electronics), via the Arduino Library Manager.

## Web Interface
The ESP32 hosts a built-in web server for real-time monitoring and configuration. It stays reachable even if the IMU fails to initialize or isn't connected - the Wi-Fi/web/captive-portal stack always comes up first.

1. Connect to the **Wi-Fi network** (default SSID: `IMUDSC_XXXX`, where `XXXX` is derived from the ESP32's MAC address; default password: `12345678`).
2. A captive portal should open the status/settings page automatically on most phones and laptops (via DHCP option 114 and, as a fallback, DNS/HTTP redirect probing). If it doesn't, open your web browser and navigate to `http://192.168.4.1`. iOS/Android sometimes cache a network as "no portal" from a previous connection - if it doesn't pop up, try "Forget This Network" and reconnecting.
3. **Features**:
    * **Live Monitoring**: View current Altitude and Azimuth values.
    * **IMU Mode Switch**: Toggle between Rotation Vector (magnetometer-assisted) and Game Rotation Vector (no magnetometer).
    * **WiFi Settings**: Change the SoftAP SSID, password (8-63 chars), and channel (persisted, requires reboot).
    * **Language**: Toggle the UI between English and Japanese (button top of page; choice is remembered).

    Note: there's no zero-point/orientation calibration step, and none is needed—SkySafari's own "Align" (below) is what maps the device's readings to the real sky, regardless of where the IMU's zero happens to be.

## SkySafari Setup
To connect SkySafari to your telescope, follow these steps:

1. **Connect Wi-Fi**: Connect your smartphone/tablet to the ESP32's Wi-Fi network (default SSID `IMUDSC_XXXX`, password `12345678`; check the Web Interface if you've customized it).
2. **Equipment Selection**:
    * **Scope Type**: `Basic Encoder System`
    * **Mount Type**: `Alt-Az. Push-To`
3. **Communication Settings**:
    * **IP Address**: `192.168.4.1`
    * **Port**: `4030`
4. **Encoder Settings**:
    * **Steps per Revolution**: Enable `Get Automatically`

## Important Notes
* **Alignment**: After connecting, use the "Align" feature in SkySafari on a known star to synchronize the IMU with the sky.
* **Magnetic Interference**: Keep the BNO086 away from large metal objects or motors to avoid heading drift.
* **Leveling**: Ensure your telescope base is as level as possible for the best accuracy.
* **Power Supply**: Power the ESP32 from a proper 5V source (a USB power bank/wall adapter) rather than a computer's USB port. A computer port can current-limit or sag under the brief power draw of Wi-Fi transmission, which was observed to intermittently prevent the BNO086 from initializing (looks like "IMU not found" or a hang at boot) - a dedicated power bank resolved it.

## License
This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
