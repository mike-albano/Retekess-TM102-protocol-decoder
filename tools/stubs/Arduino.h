#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#define F(x) (x)
#define OUTPUT 1
#define INPUT 0
#define HIGH 1
#define LOW 0
typedef uint8_t byte;
void pinMode(int, int);
void digitalWrite(int, int);
unsigned long millis();
void delay(unsigned long);
void delayMicroseconds(unsigned int);
struct SerialC {
  void begin(unsigned long);
  int  available();
  int  read();
  int  printf(const char*, ...);
  void println(const char* = "");
  void println(int);
  void print(const char*);
};
extern SerialC Serial;
