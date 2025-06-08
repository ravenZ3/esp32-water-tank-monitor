# Smart Water Tank Monitoring System: ESP8266-Based, MQTT-Enabled, Solar-Powered IoT Architecture

A fully autonomous, energy-efficient, and cloud-integrated system for real-time water tank level monitoring and control, leveraging the ESP8266 microcontroller platform, MQTT over Wi-Fi (via HiveMQ Cloud), and a solar-powered battery solution. Engineered for rooftop deployment, the design prioritizes low-power operation, resilience in outdoor environments, and seamless communication between distributed nodes.

---

## &#x20;System Overview

This system comprises two **ESP8266-based nodes**:

* **Measurement Node (Tank-Side):** Utilizes a **JSN-SR04T waterproof ultrasonic transducer** to determine water level. Sensor data is published to an MQTT broker.
* **Control Node (Ground-Side):** Subscribes to the cloud topic and displays data or actuates devices, such as a water pump via a **relay module**.

The Measurement Node is powered by a solar energy harvesting system: a 6V solar panel charges a 18650 Li-ion cell via a TP4056 charge controller. The voltage is then stepped up using an MT3608 DC-DC converter to meet operational requirements.

---

## Core Features

* **Energy-autonomous design** utilizing solar recharging and battery storage
* **MQTT-based message exchange** through HiveMQ Cloud
* **Robust waterproof level sensing** via JSN-SR04T
* **Wireless data relaying** using either MQTT or ESP-NOW protocols
* **Remote actuator control** via relay interface
* **Real-time visualization compatibility** with MQTT dashboard tools

---

## ⚙️ System Architecture Diagram

```text
   [JSN-SR04T Ultrasonic]         [Relay Control Node]
           │                             ▲
   [Measurement ESP8266] ─ MQTT ─► [Control ESP8266]
           │                             │
  [TP4056 + MT3608 + Solar]     [USB/External Power Source]
```

* **Data Pathway:** Ultrasonic reading → MQTT publish → Subscription by control node → Display or relay action
* **Energy System:** Solar panel → TP4056 → 18650 Battery → MT3608 boost to 5V
* **Cloud Broker:** HiveMQ Cloud (optionally TLS-enabled)

---

## 🧱 Hardware Components

| Component                 | Function & Role                                        |
| ------------------------- | ------------------------------------------------------ |
| ESP8266 NodeMCU (x2)      | Wi-Fi-enabled microcontrollers for sensing and control |
| JSN-SR04T                 | Waterproof distance sensor for liquid level detection  |
| TP4056                    | Lithium-ion charging module with protection            |
| 18650 Battery             | Primary energy storage unit                            |
| MT3608                    | Boost converter to elevate voltage for 5V peripherals  |
| 1N5819 Diode              | Schottky diode for input reverse current protection    |
| Solar Panel (6V 1.1W)     | Photovoltaic input for energy harvesting               |
| Relay Module              | Interface for switching high-power devices             |
| Capacitors (100uF, 0.1uF) | Voltage smoothing and noise suppression                |

---

## 📡 Communication Stack

### Mode 1: MQTT (Primary Protocol)

* **MQTT Broker:** HiveMQ Cloud
* **Publisher (Tank Node):** Topic `/tank/level`
* **Subscriber (Control Node):** Subscribes and responds accordingly

### Mode 2: ESP-NOW (Secondary Option)

* Peer-to-peer, infrastructure-less communication
* Operates independently of internet access, ideal for latency-critical tasks

---

## 🔌 Electrical Wiring: Measurement Node

**TP4056 Module:**

* `IN+` / `IN–` → Solar Panel terminals
* `B+` / `B–` → 18650 Li-ion battery
* `OUT+` / `OUT–` → Input terminals of MT3608

**MT3608 Module:**

* `IN+` / `IN–` → Output from TP4056
* `OUT+` → Common 5V rail (for sensor and ESP)
* `OUT–` → GND

**ESP8266 NodeMCU:**

* `VIN` → MT3608 `OUT+`
* `GND` → Shared GND rail
* `Trig` → GPIO14 (D5)
* `Echo` → GPIO12 (D6)

**Capacitive Decoupling:**

* 100uF electrolytic capacitor across MT3608 `OUT+` and GND
* 0.1uF ceramic capacitor near ESP VCC and GND for high-frequency noise suppression

**Reverse Polarity Protection:**

* 1N5819 diode between solar + and TP4056 `IN+` (cathode to panel +)

---

## 📋 Cloud Configuration: HiveMQ Broker

1. Register at [HiveMQ Cloud](https://www.hivemq.com/cloud/)
2. Deploy a cluster instance and retrieve:

   * Hostname
   * Port (1883 non-TLS, 8883 TLS)
   * Credentials (username & password)
3. Validate communication using MQTTX or Mosquitto client
4. Implement connectivity using `PubSubClient` on ESP8266

---

## 🧠 Power Efficiency Strategy

* ESP8266 operates in **deep sleep** to minimize idle power consumption
* JSN-SR04T is only activated during measurement intervals
* Solar charging ensures sustainability under varying environmental conditions
* Component-level optimization reduces quiescent and peak currents

---

## 📁 Repository Structure

```text
smart-water-tank-monitor/
├── README.md
├── code/
│   ├── sender_node.ino
│   ├── receiver_node.ino
│   └── test_code/
│       └── jsn_sr04t_test.ino
├── docs/
│   ├── wiring-diagram.png
│   └── photo-enclosure.jpg
├── hardware/
│   └── schematic.fritzing
└── LICENSE
```

---

## 🔬 Potential Extensions

* Integration with Blynk or custom web dashboard for UI
* Sensor upgrade to capacitive or piezoresistive variants
* Full TLS encryption for secure MQTT communication
* OTA (Over-the-Air) firmware upgrade mechanism
* Data archival via Firebase, Thingspeak, or InfluxDB

---

## 📜 Licensing Terms

Distributed under the MIT License — unrestricted use and modification permitted.

---

## 🙋‍♂️ Acknowledgment

Developed and maintained by [Ramjan Khandelwal](https://github.com/ravenZ3). For inquiries or collaboration, initiate contact via GitHub.

---

Empower your infrastructure with real-time, autonomous water management. Install once, monitor forever.
# smart-water-tank-monitor
