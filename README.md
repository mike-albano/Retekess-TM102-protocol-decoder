> ## ⚠️ SUPERSEDED IN PART — read `FINDINGS.md` first
>
> This document was written before the FCC filings were retrieved. Its
> hardware wiring and project intent still stand, but several technical
> assumptions below have been **disproved by the certification documents**:
>
> | This README says | Actual, per FCC filings |
> |---|---|
> | "We think 2 Mbps, might be 1 Mbps" | **1 Mbps**, confirmed by the measured 20 dB bandwidth |
> | "Might operate on Channel 50" | **16 channels only**, 2420–2465 MHz at 3 MHz spacing. Ch 50 is one of them (index 11); the host's `F4` menu selects which |
> | "Sniff by setting a 2-byte MAC of `0x00`/`0xAA` and reading noise" | Right *technique*, wrong *expectation* — the radio is an **Si24R1 (nRF24L01+ clone)**, and its address may never begin with `0x00`/`0xAA`, so those captures contain no framed packets. Use them to hunt for a recurring address, then lock on |
> | Wiring: "VCC → 3.3V, do NOT connect to 5V" | Correct for a *bare* module. This build uses a **PA+LNA module on a regulated socket adapter**, so **VCC → VIN (5V)** — see `FINDINGS.md` §12 |
>
> Do not code against the numbers in this file. `FINDINGS.md` carries the
> verified values and cites its sources.

# Retekess TM102 RF Protocol Sniffer

This document outlines the hardware configuration, firmware, and methodology to use to enable intercepting and capturing raw over-the-air (OTA) RF frames from the Retekess TM102 Answer Buzzer System.

The system uses GFSK modulation on the 2.4GHz ISM band. Because the manufacturer does not broadcast using standard BLE or WiFi packets, we use a technique pioneered by Travis Goodspeed to trick a standard nRF24L01+ transceiver into dumping raw, promiscuous RF data to an ESP32 microcontroller via SPI.

## Hardware Requirements

To replicate this sniffing setup, the following has been set up and connected to this computer:
1. **ESP32 Microcontroller** (e.g., ESP32-WROOM-32 dev board)
2. **nRF24L01+ Transceiver Module** (Ensuring it is the '+' version, as older versions lack the required features for promiscuous mode).
3. **Jumper Wires**

### Wiring Diagram (ESP32 to nRF24L01+)

| nRF24L01+ Pin | ESP32 Pin | Function |
| :--- | :--- | :--- |
| VCC | 3.3V | Power (Do NOT connect to 5V) |
| GND | GND | Ground |
| CE | GPIO 4 | Chip Enable |
| CSN | GPIO 5 | SPI Chip Select |
| SCK | GPIO 18 | SPI Clock |
| MOSI | GPIO 23 | SPI Master Out Slave In |
| MISO | GPIO 19 | SPI Master In Slave Out |
| IRQ | Not Connected | Interrupt Request |

*(Note: Depending on your specific ESP32 board, the default VSPI pins might vary slightly. Adjust your firmware accordingly if needed).*

## The Travis Goodspeed Promiscuous Exploit

By default, the nRF24L01+ is designed to only pass packets to the microcontroller if the packet's MAC address perfectly matches the receiver's configured MAC address, and the CRC check passes. 

To sniff unknown MAC addresses (like the Retekess system), we use a deliberate misconfiguration exploit:
1. **Disable CRC Checking:** We turn off CRC validation so the chip doesn't drop packets that fail checksums.
2. **Shorten the MAC Address:** We configure the receiver to expect the shortest possible MAC address (2 bytes).
3. **Use a Generic MAC:** We set the receiver's MAC address to a common RF noise pattern, such as `0x00 00` or `0xAA AA`.

Because the 2.4GHz spectrum is noisy, ambient RF static will frequently naturally align with `0x00 00` or `0xAA AA`. When this happens, the nRF24L01+ believes a valid packet has started and immediately begins dumping the subsequent raw, on-air bits over the SPI bus to the ESP32. 

By constantly dumping this raw bitstream, we successfully capture the Retekess packets hidden within the noise.

## Firmware Configuration

The ESP32 must be flashed with firmware (typically written in C++ using the Arduino IDE and the `RF24` library) that configures the nRF24L01+ for this exploit.

Key configuration steps in the firmware, which has already occured:
1. **Initialize Radio:** Begin SPI communication with the nRF24L01+.
2. **Set Data Rate:** We think the Retekess system operates at 2mbps, but it might operate at 1mbps. We are not sure which one it is, and you should look for lab reports or a public reference that can confirm the data rate.
3. **Disable Auto-Acknowledge:** `radio.setAutoAck(false)`
7. **Set Channel:** The Retekess system might operates on Channel 50 (2450 MHz). `radio.setChannel(50)` but we are not sure of this. The channels are configurable on the retekess controller.
- If you want to listen on other channels, that is possible.
8. **Loop & Print:** Continuously read the payload buffer and print it to the Serial monitor over USB.

## Serial Output Format

When connected to the ESP32 via Serial (baud rate 115200), the output will appear as a continuous stream of hex strings representing the raw bits pulled from the air.

Example Output:
```
HEX: 1D 7C 36 FF A0 EF 3E FC 4E 8D FA 4F 5A 5D FC 51 01 54 FF FF 54 8D EB FA F4 AF EF BD F7 CE BE DD 
HEX: 8C 0C 0C 0C 1D 79 3B 7F 77 31 DD 4A FF 4E B9 A0 E7 3E F4 4E B5 FA 0F 5A 55 FC 00 01 00 FF EF AA 
HEX: 46 06 06 06 0E BC 1D BF BB 98 EE 25 7F A7 5C D0 73 9F 7A 27 5A FD 07 AD 2A FE 00 00 80 7F F7 55 
```

## Mandatory Reading
You MUST read the following research and artifacts, before proceeding with Next Steps:

- "Retekess FCC ID RF Analysis.pdf"
- Every picture in the directory "pics" contains serial numbers, FCC IDs and other relevant information. Analyze each picture, given the following conditions:
1) Each buzzer picture is labelled as "buzzer-<increment>", for example "buzzer-1.jpg", "buzzer-2.jpg", etc.
2) Their are 2 controller pictures, one of the front labelled "controller-front.jpg" and one of the back labelled "controller-back.jpg"


## Next Steps: Identifying Button Presses
The goal is to identify each button press from the controller, and which buzzer buzzes in first.

Develop the raw capture pipeline to acquire the data. The next challenge is analyzing the raw `HEX:` dumps to mathematically identify the exact byte sequences or payloads that correspond to specific buzzer physical button presses. 

*(Analysis methodologies and payload decoding are left to the researcher, as the current captures require advanced differential analysis to separate genuine buzzer uplinks or targeted downlinks from the ambient RF static and continuous Controller spam).*
