#pragma once
#include <cstdint>
#define MSBFIRST 1
#define SPI_MODE0 0
struct SPISettings { SPISettings(uint32_t, uint8_t, uint8_t); };
struct SPIClass {
  void begin(int, int, int, int);
  void beginTransaction(SPISettings);
  void endTransaction();
  uint8_t transfer(uint8_t);
};
extern SPIClass SPI;
