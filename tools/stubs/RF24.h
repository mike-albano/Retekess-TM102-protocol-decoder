#pragma once
#include <cstdint>
typedef enum { RF24_PA_MIN = 0, RF24_PA_LOW, RF24_PA_HIGH, RF24_PA_MAX } rf24_pa_dbm_e;
typedef enum { RF24_1MBPS = 0, RF24_2MBPS, RF24_250KBPS } rf24_datarate_e;
typedef enum { RF24_CRC_DISABLED = 0, RF24_CRC_8, RF24_CRC_16 } rf24_crclength_e;
class RF24 {
public:
  RF24(uint16_t ce, uint16_t csn);
  bool begin();
  bool isChipConnected();
  void setChannel(uint8_t);
  void setDataRate(rf24_datarate_e);
  void setPALevel(uint8_t);
  void setAutoAck(bool);
  void disableCRC();
  void setCRCLength(rf24_crclength_e);
  void setAddressWidth(uint8_t);
  void setPayloadSize(uint8_t);
  void openReadingPipe(uint8_t, const uint8_t*);
  void closeReadingPipe(uint8_t);
  void startListening();
  void stopListening();
  void flush_rx();
  bool available();
  void read(void*, uint8_t);
  bool testRPD();
};
