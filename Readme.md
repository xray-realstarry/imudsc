# IMU DSC

This project implements a **Push-To telescope controller** using an **ESP32** and **BNO086 (IMU)**. It emulates the "Basic Encoder (BBox)" protocol, allowing you to visualize your telescope's real-time orientation directly in **SkySafari**.

## Features
* **BNO086 Integration**: High-precision orientation using the Rotation Vector sensor.
* **SkySafari Compatibility**: Emulates the BBox (Tangent) protocol over Wi-Fi.
* **SoftAP Mode**: The ESP32 acts as an Access Point—no router needed in the field.
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
Please install the following library via the Arduino Library Manager:
* **SparkFun BNO08x Arduino Library** (by SparkFun Electronics)

## SkySafari Setup
To connect SkySafari to your telescope, follow these steps:

1. **Connect Wi-Fi**: Connect your smartphone/tablet to the network **"ESP32_Telescope"** (Password: `12345678`).
2. **Equipment Selection**:
    * **Scope Type**: `Basic Encoder System`
    * **Mount Type**: `Alt-Az. Push-To`
3. **Communication Settings**:
    * **IP Address**: `192.168.4.1`
    * **Port**: `4030`
4. **Encoder Settings**:
    * **Steps per Revolution**: `36000` (for both axes)

## Important Notes
* **Alignment**: After connecting, use the "Align" feature in SkySafari on a known star to synchronize the IMU with the sky.
* **Magnetic Interference**: Keep the BNO086 away from large metal objects or motors to avoid heading drift.
* **Leveling**: Ensure your telescope base is as level as possible for the best accuracy.

## License
This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.