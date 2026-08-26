# Antigravity Agent Instructions: Retekess TM102 Packet Sniffer

## 1. Project Overview
This workspace is dedicated to developing an Arduino/C++ firmware for an ESP32 (DevKit V1) interfaced with an nRF24L01+ (PA+LNA) module. The goal is to sniff, intercept, and decode proprietary 2.4 GHz GFSK packets originating from a Retekess TM102 buzzer system. 

## 2. Hardware & Pinout Specifications
The hardware is fully assembled and verified. The agent MUST strictly adhere to the following ESP32 GPIO pin mappings for SPI and radio control:

*   **VCC:** `VIN` (5V routed through external adapter board regulator)
*   **GND:** `GND`
*   **CE (Chip Enable):** `GPIO 4`
*   **CSN (Chip Select Not):** `GPIO 5`
*   **SCK (Serial Clock):** `GPIO 18`
*   **MISO:** `GPIO 19`
*   **MOSI:** `GPIO 23`

## 3. Core Dependencies
*   **Library:** `RF24` by TMRh20 (must be included as `<RF24.h>`).
*   **SPI Library:** `<SPI.h>`

## 4. Operational Requirements (The "Sniffer" Logic)

### A. Radio Initialization
*   Begin Serial at **115200 baud**.
*   Initialize the radio using `RF24 radio(4, 5);`.
*   Agent must implement a connection check (`radio.isChipConnected()`). If false, halt execution and print a clear debugging message. Do not proceed to the main loop if the radio is not responding.

### B. Promiscuous Mode (Crucial Step)
To intercept proprietary Retekess packets, the standard hardware filters on the nRF24L01+ must be intentionally bypassed. Agent must configure the radio as follows:
*   **Disable CRC:** `radio.disableCRC();` (Critical for catching non-standard packets).
*   **Disable Auto-Ack:** `radio.setAutoAck(false);` (We are listening silently, not acknowledging).
*   **Set Address Width:** Reduce to the minimum (typically 3 bytes or use `radio.setAddressWidth(3);` if supported, or configure raw registers) to maximize false-positive packet captures which we will filter in software.
*   **Data Rate:** Start at `RF24_1MBPS`. (Provide easy variables to switch to `RF24_2MBPS` or `RF24_250KBPS` if the Retekess system uses a different modulation rate).
*   **Payload Size:** Set to maximum (`radio.setPayloadSize(32);`) dynamic payloads are likely disabled on cheap hardware.

### C. Channel Hopping
The Retekess system operates between 2.405 GHz and 2.485 GHz.
*   Implement a non-blocking channel hopper (using `millis()`, not `delay()`).
*   Sweep channels `5` through `85`.
*   Dwell time per channel: ~10-20ms (configurable via a constant).
*   Implement a Serial command (e.g., typing 'L' + channel number) to pause the hopping and lock onto a specific channel once the user identifies the buzzer's frequency.

### D. Data Output Format
When a payload is intercepted (`radio.available()`), read the raw bytes and format the output cleanly for external parsing. 
*   **Required Format:** `[CH: <channel_num>] LEN: <length> | HEX: <XX XX XX XX ...>`
*   Ensure leading zeros are printed for hex values (e.g., `0A` instead of `A`).

## 5. Coding Standards & Agent Workflow
*   **No Blocking Code:** Do not use `delay()` in the main `loop()`. Use state machines and `millis()` for channel hopping.
*   **Modularity:** Separate the radio setup, the channel hopping logic, and the payload dumping into distinct, readable functions.
*   **Comments:** Heavily comment the register manipulation and RF24 configuration so the logic is transparent.
*   **Resilience:** If the SPI bus crashes, the code should ideally detect it and attempt to re-initialize `radio.begin()`.