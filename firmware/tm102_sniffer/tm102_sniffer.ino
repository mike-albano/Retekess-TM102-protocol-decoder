/* ===========================================================================
 * TM102 Sniffer — Phase 1: find the channel, then discover the address
 * ---------------------------------------------------------------------------
 * Target : ESP32-WROOM-32 (ESP-32S, CP2102) + nRF24L01+PA+LNA on a regulated
 *          socket adapter.  Adapter VCC -> ESP32 VIN (5V).  See FINDINGS.md §11.
 *
 * RECEIVE ONLY.  This sketch never calls write()/startWrite() and never enables
 * the PA for transmit.  Agreed scope is RX-only until the protocol is decoded.
 *
 * WHY THIS SKETCH LOOKS THE WAY IT DOES  (all of it from FINDINGS.md)
 *
 *   - The radio in the Retekess host is an Si24R1 (FCC exhibit 6072498, Fig.5),
 *     which is an nRF24L01+ architectural clone.  So the air protocol is
 *     ordinary Enhanced ShockBurst: 1-byte preamble, 3-5 byte address,
 *     9-bit packet control field, payload, 1-2 byte CRC.  There is NO XN297
 *     scrambling and no fixed sync word to lock onto.
 *
 *   - FCC test report LCSA072222029EA gives the PHY exactly:
 *       1 Mbps GFSK, 16 channels, 3 MHz apart, 2420..2465 MHz.
 *     So we sweep 16 channels at ONE data rate, not 85 channels at three.
 *
 *   - The host's F4 menu (00-15) selects the channel, but the mapping from
 *     menu value to frequency is NOT confirmed.  Do not assume F4=05 means
 *     2435 MHz.  MODE_SCAN finds the live channel empirically instead.
 *
 *   - Address discovery uses the Travis Goodspeed promiscuous exploit: set the
 *     address width to 2 bytes (SETUP_AW=0, a value the datasheet calls
 *     illegal but which the silicon accepts), disable CRC, and use a bait
 *     address of 0x00 0x00 or 0xAA 0xAA.  Ambient RF noise then trips the
 *     receiver's sync detector and the chip shifts raw on-air bits into the
 *     RX FIFO.  Captures are bit-shifted garbage *containing* real packets;
 *     tools/analyze_capture.py hunts for the recurring address inside them.
 *
 *     The Si24R1 datasheet states the top address byte may not be 0x00, 0xAA,
 *     0x55, 0xA5, 0x5A or 0xFF.  That is exactly why those make good bait —
 *     no real device uses them — but it also means a capture will never
 *     contain a cleanly-framed packet starting at byte 0.
 *
 * SERIAL COMMANDS (115200 baud, newline-terminated)
 *   ?        help
 *   s        energy scan across the 16 certified channels (RPD), show deltas
 *   b        record the current RF environment as the baseline for 's'
 *   d        dump: promiscuous capture on the locked channel
 *   h        hop: promiscuous capture, cycling all 16 channels
 *   x        stop / idle
 *   c<n>     lock to certified channel INDEX n, 1..16   (e.g. c6)
 *   r<n>     lock to raw RF_CH n, 0..125                (e.g. r35)
 *   a0 / aa  bait address 0x0000 / 0xAAAA
 *   i        print radio register dump (verifies the clone accepted our pokes)
 * =========================================================================== */

#include <SPI.h>
#include <RF24.h>

// ---------------------------------------------------------------------------
// Pin map — identical in README.md and skills.md, and all free on a 38-pin board.
// GPIO5 is an ESP32 strapping pin that must read HIGH at boot; harmless here
// because SPI chip-select idles HIGH by design.
// ---------------------------------------------------------------------------
static const uint8_t PIN_CE   = 4;
static const uint8_t PIN_CSN  = 5;
static const uint8_t PIN_SCK  = 18;   // VSPI default
static const uint8_t PIN_MISO = 19;   // VSPI default
static const uint8_t PIN_MOSI = 23;   // VSPI default

RF24 radio(PIN_CE, PIN_CSN);

// ---------------------------------------------------------------------------
// The 16 certified channels, from FCC test report LCSA072222029EA page 8.
// Stored as RF_CH register values (RF_CH = MHz - 2400).
//
// NOTE: the report prints channel 6 as "2433 MHz", which breaks the otherwise
// exact 3 MHz spacing.  It is a typo for 2435 and both the buzzer and host
// reports contain it.  We use 35.  If the scan finds energy at 33 instead,
// the typo was real — worth knowing, so RF_CH 33 is included in the sweep.
// ---------------------------------------------------------------------------
static const uint8_t CHANNELS[] = {
  20, 23, 26, 29, 32, 35, 38, 41, 44, 47, 50, 53, 56, 59, 62, 65,
  33   // the "2433" the report actually prints — kept as a control
};
static const uint8_t N_CHANNELS = sizeof(CHANNELS) / sizeof(CHANNELS[0]);

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
static const char FW_VERSION[] = "v9 (addressed capture) 2026-08-26";

static const uint32_t SERIAL_BAUD      = 115200;
static const uint16_t SCAN_DWELL_MS    = 25;    // continuous listen per channel per sweep
static const uint16_t SCAN_SWEEPS      = 40;    // sweeps per 's' run (~17 s total)
static const uint32_t FIND_DWELL_MS    = 2500;  // 'f' mode: long dwell, one channel at a time
static const uint32_t WATCH_REPORT_MS  = 500;   // 'w' mode: summary interval
static const uint32_t HOP_DWELL_MS     = 15;    // dwell per channel in hop mode
static const uint32_t HEALTH_EVERY_MS  = 3000;  // SPI liveness check interval
static const uint8_t  PAYLOAD_SIZE     = 32;

// nRF24 register addresses we poke directly (see verifyRegisters()).
static const uint8_t REG_CONFIG   = 0x00;
static const uint8_t REG_EN_AA    = 0x01;
static const uint8_t REG_SETUP_AW = 0x03;
static const uint8_t REG_RF_CH    = 0x05;
static const uint8_t REG_RF_SETUP = 0x06;

enum Mode : uint8_t { MODE_IDLE, MODE_SCAN, MODE_DUMP, MODE_HOP, MODE_WATCH, MODE_FIND, MODE_LOCK,
                      MODE_MATCH, MODE_CLEAN };

// ---------------------------------------------------------------------------
// PHASE 2 — lock onto the real address.
//
// Captured 2026-08-26 on RF_CH 50: 1324 of 1523 signal-tagged packets carried
// the identical byte run, once aligned on the ESB preamble 0x55:
//
//        55 | 18 18 18 18 3A | <varies>
//     preamble      constant     payload
//
// No CRC variant validated over that (six polynomial/width combinations tested
// offline, all at chance level), so this system almost certainly runs with CRC
// disabled — which is legal and common in cheap designs.
//
// What we do NOT know is how those on-air bytes map to the address register.
// The nRF24 sends the address MSByte first but the register is loaded LSByte
// first, so the array is the reverse of the on-air order — and it is not worth
// arguing about when the chip can simply tell us. Each candidate below is
// tried for real; whichever actually receives is the right one.
// ---------------------------------------------------------------------------
struct Candidate { uint8_t aw; uint8_t addr[5]; const char *note; };

static const Candidate CANDS[] = {
  { 5, {0x3A,0x18,0x18,0x18,0x18}, "5B  on-air 18 18 18 18 3A  (reversed)" },
  { 5, {0x18,0x18,0x18,0x18,0x3A}, "5B  on-air 3A 18 18 18 18  (as-written)" },
  { 4, {0x18,0x18,0x18,0x18},      "4B  18 18 18 18            (symmetric)" },
  { 4, {0x3A,0x18,0x18,0x18},      "4B  on-air 18 18 18 3A     (reversed)" },
  { 3, {0x18,0x18,0x18},           "3B  18 18 18               (symmetric)" },
  { 3, {0x3A,0x18,0x18},           "3B  on-air 18 18 3A        (reversed)" },
  { 5, {0xFA,0x3A,0x18,0x18,0x18}, "5B  shifted one byte later" },
  { 4, {0xFA,0x3A,0x18,0x18},      "4B  shifted one byte later" },
};
static const uint8_t N_CANDS = sizeof(CANDS) / sizeof(CANDS[0]);
static const uint32_t LOCK_DWELL_MS = 5000;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static Mode     g_mode        = MODE_IDLE;
static uint8_t  g_channel     = 50;      // measured 2026-08-26: 2450 MHz
static uint8_t  g_hopIndex    = 0;
static uint32_t g_hopLastMs   = 0;
static uint32_t g_healthLastMs = 0;
static uint32_t g_packets     = 0;
static bool     g_baitAA      = false;   // false = 0x0000, true = 0xAAAA
static bool     g_gated       = true;    // print only packets that carried real signal
static uint32_t g_strong      = 0;       // packets captured with RPD high

static uint32_t g_scanHits[N_CHANNELS];
static uint32_t g_baseline[N_CHANNELS];
static bool     g_haveBaseline = false;
static bool     g_scanIsBaseline = false;   // is this run recording the baseline?
static uint16_t g_sweepsDone  = 0;
static uint8_t  g_scanIndex   = 0;          // channel cursor, so the sweep is non-blocking

// 'w' watch and 'f' find state
static uint32_t g_watchHits   = 0;
static uint32_t g_watchTotal  = 0;
static uint32_t g_watchLastMs = 0;
static uint32_t g_watchPkts   = 0;
static uint8_t  g_findIdx     = 0;
static uint8_t  g_findPass    = 0;
static uint32_t g_findStart   = 0;
static uint32_t g_findEnergy[N_CHANNELS];
static uint32_t g_findPkts[N_CHANNELS];
static uint8_t  g_lockIdx     = 0;
static uint32_t g_lockStart   = 0;
static uint32_t g_lockCount[8];
static bool     g_lockChosen  = false;

static char     g_line[24];
static uint8_t  g_lineLen = 0;

// ===========================================================================
// Raw SPI register access
// ---------------------------------------------------------------------------
// The RF24 library keeps read_register() private, but Si24R1 is a CLONE and we
// are relying on one deliberately out-of-spec register value (SETUP_AW = 0).
// Reading the registers back is the difference between "we configured it" and
// "it accepted the configuration" — worth the extra 20 lines.
//
// Safe to do alongside RF24: the library leaves CSN high between its own calls.
// ===========================================================================
static uint8_t nrfReadReg(uint8_t reg)
{
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CSN, LOW);
  SPI.transfer(reg & 0x1F);            // R_REGISTER = 000A AAAA
  uint8_t v = SPI.transfer(0xFF);
  digitalWrite(PIN_CSN, HIGH);
  SPI.endTransaction();
  return v;
}

// ===========================================================================
// Radio configuration
// ===========================================================================
static void configurePromiscuous()
{
  radio.setDataRate(RF24_1MBPS);        // FCC report: 1 Mbps, confirmed by 20dB BW
  radio.setPALevel(RF24_PA_MIN);        // RX-only; keep the PA quiet
  radio.setChannel(g_channel);

  radio.setAutoAck(false);              // listening silently, never acknowledging
  radio.disableCRC();                   // catch frames whose checksum we can't verify
  radio.setAddressWidth(2);             // -> SETUP_AW = 0. The exploit. Must precede
                                        //    openReadingPipe so only 2 bytes are written.
  radio.setPayloadSize(PAYLOAD_SIZE);   // static 32; dynamic payloads must stay OFF

  const uint8_t bait0[2] = { 0x00, 0x00 };
  const uint8_t baitA[2] = { 0xAA, 0xAA };
  radio.openReadingPipe(0, g_baitAA ? baitA : bait0);

  radio.startListening();
  radio.flush_rx();
}

static bool initRadio()
{
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CSN);
  pinMode(PIN_CSN, OUTPUT);
  digitalWrite(PIN_CSN, HIGH);

  if (!radio.begin()) {
    Serial.println(F("!! radio.begin() failed — SPI bus not responding."));
    return false;
  }
  if (!radio.isChipConnected()) {
    Serial.println(F("!! isChipConnected() false — check wiring and power."));
    Serial.println(F("   Most likely cause: adapter VCC on the ESP32 3.3V pin."));
    Serial.println(F("   It must go to VIN (5V) — a 3.3V LDO fed 3.3V sits in"));
    Serial.println(F("   dropout at ~2.9V and browns out. See FINDINGS.md §11."));
    return false;
  }
  configurePromiscuous();
  return true;
}

// ---------------------------------------------------------------------------
// Read the registers back and say plainly whether the exploit took hold.
// ---------------------------------------------------------------------------
static bool verifyRegisters()
{
  uint8_t cfg = nrfReadReg(REG_CONFIG);
  uint8_t aa  = nrfReadReg(REG_EN_AA);
  uint8_t aw  = nrfReadReg(REG_SETUP_AW);
  uint8_t ch  = nrfReadReg(REG_RF_CH);
  uint8_t rf  = nrfReadReg(REG_RF_SETUP);

  Serial.println(F("--- radio registers ---"));
  Serial.printf("  CONFIG   0x%02X   CRC %s, PWR_UP %s, PRIM_RX %s\n",
                cfg, (cfg & 0x08) ? "ON (BAD)" : "off (good)",
                (cfg & 0x02) ? "yes" : "no", (cfg & 0x01) ? "yes" : "no");
  Serial.printf("  EN_AA    0x%02X   auto-ack %s\n",
                aa, aa ? "ENABLED (BAD)" : "disabled (good)");
  Serial.printf("  SETUP_AW 0x%02X   address width %s\n",
                aw, (aw == 0) ? "2 bytes — exploit ACTIVE (good)"
                              : "NOT 2 bytes — exploit INACTIVE (BAD)");
  Serial.printf("  RF_CH    0x%02X   %u MHz\n", ch, 2400 + ch);
  Serial.printf("  RF_SETUP 0x%02X   rate %s\n",
                rf, ((rf & 0x28) == 0x00) ? "1 Mbps (good)"
                  : ((rf & 0x28) == 0x08) ? "2 Mbps (wrong)" : "250 kbps (wrong)");
  Serial.printf("  bait address: %s\n", g_baitAA ? "AA AA" : "00 00");

  Serial.println(F("-----------------------"));
  return (aw == 0) && !(cfg & 0x08) && (aa == 0) && ((rf & 0x28) == 0x00);
}

// ---------------------------------------------------------------------------
// Plain-English verdict, so nobody has to read hex to know if this is working.
// ---------------------------------------------------------------------------
static void selfTest()
{
  bool ok = verifyRegisters();
  Serial.println();
  Serial.println(F("============================================================"));
  if (ok) {
    Serial.println(F("  SELF-TEST: PASS"));
    Serial.println(F("  The radio is configured and listening correctly."));
    Serial.println();
    Serial.println(F("  Channel is already known: RF_CH 50 (2450 MHz)."));
    Serial.println(F(""));
    Serial.println(F("  DO THIS NEXT:"));
    Serial.println(F("    1. Put the board about 1 metre from the controller."));
    Serial.println(F("    2. Type  k  and press Enter."));
    Serial.println(F("    3. Press General then Clear every ~2 s for 40 seconds."));
    Serial.println(F("       It tries 8 candidate addresses and stops on its own."));
    Serial.println(F("    4. It names a winner and starts clean capture."));
    Serial.println(F(""));
    Serial.println(F("  ('f' re-hunts the channel, only needed if F4 changes.)"));
  } else {
    Serial.println(F("  SELF-TEST: FAIL"));
    Serial.println(F("  The radio did not accept its settings. Capturing now"));
    Serial.println(F("  would produce nothing at all, so stop here."));
    Serial.println();
    Serial.println(F("  CHECK, IN THIS ORDER:"));
    Serial.println(F("    1. Is the module's VCC on the ESP32 pin marked VIN?"));
    Serial.println(F("       It must NOT be on the 3.3V pin."));
    Serial.println(F("    2. Are CE=4, CSN=5, SCK=18, MISO=19, MOSI=23?"));
    Serial.println(F("    3. Is the radio pushed fully into its adapter socket?"));
    Serial.println(F("    Then press the board's reset button."));
  }
  Serial.println(F("============================================================"));
}

// ===========================================================================
// MODE_SCAN — which of the 16 channels is actually in use?
// ---------------------------------------------------------------------------
// The RPD (Received Power Detector) latches when the current channel carries
// more than about -64 dBm.  Sampling it across the channel list is Nordic's
// "poor man's spectrum analyser" and is far more reliable than hoping to catch
// a packet: it answers the channel question in seconds.
//
// Caveat: 2420-2465 MHz sits squarely under WiFi channels 1-9, so an idle scan
// will show plenty of energy that has nothing to do with the buzzers.  That is
// what the baseline is for — record the room with 'b', then scan while
// pressing a buzzer and read the DELTA, not the absolute counts.
// ===========================================================================
// Listen CONTINUOUSLY for the dwell and only restart RX *after* a detection.
//
// The first version of this restarted RX before every sample, which was
// actively harmful: each restart costs ~130 us of settling during which the
// receiver detects nothing, so it spent roughly half its life deaf — and RPD
// latches, so resetting it constantly discarded the very property that makes
// it useful.  Staying in RX and clearing only after a hit raises the effective
// listening duty from about 50% to near 100%.
//
// No transmission occurs: the TX FIFO is never loaded and CE stays low in
// standby, so the PA is never keyed.
static uint32_t servicePackets(bool show);   // defined below; used by watch/find

static uint32_t listenRPD(uint8_t rfch, uint32_t dwellMs)
{
  uint32_t hits = 0;
  radio.setChannel(rfch);
  radio.startListening();
  delayMicroseconds(150);                 // one settle, not one per sample
  const uint32_t t0 = millis();
  while (millis() - t0 < dwellMs) {
    if (radio.testRPD()) {
      hits++;
      radio.stopListening();              // clear the latch, then resume
      radio.startListening();
      delayMicroseconds(150);
    }
  }
  radio.stopListening();
  return hits;
}

static void scanStep()
{
  g_scanHits[g_scanIndex] += listenRPD(CHANNELS[g_scanIndex], SCAN_DWELL_MS);

  g_scanIndex++;
  if (g_scanIndex >= N_CHANNELS) {
    g_scanIndex = 0;
    g_sweepsDone++;
    if ((g_sweepsDone % 10) == 0) {
      Serial.printf("  ... %u/%u sweeps\n", g_sweepsDone, SCAN_SWEEPS);
    }
  }
}

// ---------------------------------------------------------------------------
// MODE_WATCH ('w') — live monitor on ONE channel.  Quiet: counts packets but
// never prints them, so the commentary stays readable.
// ---------------------------------------------------------------------------
static void serviceWatch()
{
  if (radio.testRPD()) {
    g_watchHits++; g_watchTotal++;
    radio.stopListening(); radio.startListening();
    delayMicroseconds(150);
  }
  g_watchPkts += servicePackets(false);

  uint32_t now = millis();
  if (now - g_watchLastMs >= WATCH_REPORT_MS) {
    Serial.printf("  RF_CH %u (%u MHz)   energy %lu   packets %lu%s\n",
                  g_channel, 2400 + g_channel,
                  (unsigned long)g_watchHits, (unsigned long)g_watchPkts,
                  g_watchHits ? "   <== ACTIVITY" : "");
    g_watchHits = 0; g_watchPkts = 0;
    g_watchLastMs = now;
  }
}

// ---------------------------------------------------------------------------
// MODE_FIND ('f') — walk every channel, twice, then stop and report.
//
// Bounded on purpose.  An unbounded loop asks someone to keep pressing buttons
// for an unknown length of time, which is not a reasonable thing to ask.  Two
// passes is 17 channels x 2.5 s x 2 = 85 seconds; the sketch says so up front,
// counts down, and stops by itself.
//
// Two independent signals are recorded per channel:
//   energy  — RPD trips (>-64 dBm).  Strong evidence, but needs a close, loud
//             signal, and may be stubbed out on some clone modules.
//   packets — promiscuous captures.  Noise triggers these constantly so the
//             baseline is high and only a clear excess means anything.  This
//             is the fallback if RPD turns out not to work on this hardware.
// ---------------------------------------------------------------------------
static const uint8_t FIND_PASSES = 2;

static void findReport()
{
  uint32_t bestE = 0, bestP = 0, totE = 0, totP = 0;
  uint8_t  bE = 0, bP = 0;
  for (uint8_t i = 0; i < N_CHANNELS; i++) {
    totE += g_findEnergy[i]; totP += g_findPkts[i];
    if (g_findEnergy[i] > bestE) { bestE = g_findEnergy[i]; bE = CHANNELS[i]; }
    if (g_findPkts[i]   > bestP) { bestP = g_findPkts[i];   bP = CHANNELS[i]; }
  }

  Serial.println(F("\n================ HUNT RESULT ================"));
  Serial.println(F("  RF_CH    MHz    energy   packets"));
  for (uint8_t i = 0; i < N_CHANNELS; i++) {
    Serial.printf("   %3u    %4u   %7lu   %7lu%s\n",
                  CHANNELS[i], 2400 + CHANNELS[i],
                  (unsigned long)g_findEnergy[i], (unsigned long)g_findPkts[i],
                  (bestE && g_findEnergy[i] == bestE) ? "   <== STRONGEST" : "");
  }
  Serial.println(F("============================================"));

  if (totE == 0) {
    Serial.println(F("\nNo energy on ANY channel. Two causes, different fixes:"));
    Serial.println(F("  (a) Too far away. Energy detection needs a strong signal."));
    Serial.println(F("      Put the board within half a metre of the CONTROLLER"));
    Serial.println(F("      (not a buzzer) and run 'f' again."));
    Serial.println(F("  (b) This clone module may not implement the energy"));
    Serial.println(F("      detector at all. If (a) changes nothing, that is it,"));
    Serial.printf ("      and we use the packet column instead (busiest RF_CH %u).\n", bP);
    if (totP == 0) {
      Serial.println(F("\n  Packets are ALSO zero — the radio is not receiving at"));
      Serial.println(F("  all. Type 'i' and check the self-test."));
    }
  } else {
    Serial.printf("\nStrongest: RF_CH %u (%u MHz), %lu energy hits.\n",
                  bE, 2400 + bE, (unsigned long)bestE);
    Serial.printf("Lock and capture with:   r%u   then   d\n", bE);
  }
  Serial.println(F("Hunt finished. Nothing more will print until you type something.\n"));
}

static void serviceFind()
{
  uint32_t now = millis();

  if (g_findStart == 0) {
    g_findStart = now;
    g_channel = CHANNELS[g_findIdx];
    radio.stopListening();
    radio.setChannel(g_channel);
    radio.startListening();
    uint16_t done = g_findPass * N_CHANNELS + g_findIdx;
    uint16_t left = (uint16_t)(((uint32_t)(N_CHANNELS * FIND_PASSES) - done)
                               * FIND_DWELL_MS / 1000);
    Serial.printf("  [%u/%u] RF_CH %-3u (%u MHz)   ~%us left — keep pressing\n",
                  done + 1, N_CHANNELS * FIND_PASSES,
                  g_channel, 2400 + g_channel, left);
  }

  if (radio.testRPD()) {
    g_findEnergy[g_findIdx]++;
    radio.stopListening(); radio.startListening();
    delayMicroseconds(150);
  }
  g_findPkts[g_findIdx] += servicePackets(false);

  if (now - g_findStart >= FIND_DWELL_MS) {
    g_findStart = 0;
    g_findIdx++;
    if (g_findIdx >= N_CHANNELS) {
      g_findIdx = 0;
      g_findPass++;
      if (g_findPass >= FIND_PASSES) { findReport(); g_mode = MODE_IDLE; }
      else Serial.println(F("  --- pass 2 of 2, keep going ---"));
    }
  }
}

// ===========================================================================
// Scan report (used by 'b' / 's')
// ===========================================================================
static void scanReport()
{
  uint32_t maxPossible = 1;
  for (uint8_t i = 0; i < N_CHANNELS; i++)
    if (g_scanHits[i] > maxPossible) maxPossible = g_scanHits[i];

  const bool cmp = g_haveBaseline && !g_scanIsBaseline;
  Serial.println();
  Serial.printf("=== %s - %u sweeps x %u ms per channel ===\n",
                g_scanIsBaseline ? "BASELINE" : "ENERGY SCAN",
                g_sweepsDone, SCAN_DWELL_MS);
  Serial.println(F(" idx  RF_CH   MHz   hits    %    vs base  profile"));

  uint32_t best = 0; uint8_t bestIdx = 0;
  for (uint8_t i = 0; i < N_CHANNELS; i++) {
    int32_t d = cmp ? (int32_t)g_scanHits[i] - (int32_t)g_baseline[i]
                    : (int32_t)g_scanHits[i];
    if (d > (int32_t)best) { best = d; bestIdx = i; }
  }

  for (uint8_t i = 0; i < N_CHANNELS; i++) {
    uint32_t pct = (g_scanHits[i] * 100UL) / maxPossible;
    int32_t  d   = cmp ? (int32_t)g_scanHits[i] - (int32_t)g_baseline[i] : 0;
    char bar[26]; uint8_t n = pct / 4; if (n > 25) n = 25;
    memset(bar, '#', n); bar[n] = '\0';
    Serial.printf(" %3u    %3u  %4u  %5lu  %3lu%%  %+7ld  %s%s\n",
                  (i == 16) ? 0 : i + 1, CHANNELS[i], 2400 + CHANNELS[i],
                  (unsigned long)g_scanHits[i], (unsigned long)pct, (long)d, bar,
                  (i == 16) ? " <- report typo control" : "");
  }

  uint32_t grand = 0;
  for (uint8_t i = 0; i < N_CHANNELS; i++) grand += g_scanHits[i];
  if (grand == 0) {
    Serial.println(F("\n*** EVERY channel read zero. That is not 'no traffic' - a"));
    Serial.println(F("    normal room is never silent here. Either the energy"));
    Serial.println(F("    detector is not working, or the bursts are far too short"));
    Serial.println(F("    for a sweep to catch. Use 'f' instead.\n"));
  }

  if (cmp) {
    Serial.printf("\nStrongest rise vs baseline: RF_CH %u (%u MHz).\n",
                  CHANNELS[bestIdx], 2400 + CHANNELS[bestIdx]);
  }
  Serial.println(F("Reminder: WiFi lives here too. Trust the delta, not the level.\n"));
}

// ===========================================================================
// Packet output. `show` gates the hex: hunt and watch must NOT print, or the
// commentary is buried under a torrent of noise captures (promiscuous mode
// triggers on static constantly - thousands of packets a minute).
// ===========================================================================
// The gate is the important part here.
//
// Promiscuous capture triggers on static thousands of times a minute, and this
// system transmits less than 1% of the time, so raw dumps are almost entirely
// noise — the first 23,000 captured packets contained nothing but static.
//
// But RPD latches when a packet is received, so reading it immediately before
// draining the FIFO tells us whether THAT packet arrived on top of a signal
// stronger than -64 dBm. Real controller traffic clears that bar easily at a
// metre; thermal noise never does. Gating on it turns a haystack into a
// handful of straws.
//
// Every line is tagged RPD:1 or RPD:0 either way, so nothing is silently
// discarded and the analyser can filter after the fact.
static uint32_t servicePackets(bool show)
{
  uint8_t buf[PAYLOAD_SIZE];
  uint32_t n = 0;
  while (radio.available()) {
    const bool strong = radio.testRPD();
    radio.read(buf, PAYLOAD_SIZE);
    g_packets++;
    if (strong) g_strong++;
    n++;
    if (!show) continue;
    if (g_gated && !strong) continue;

    // Format required by skills.md section 4D, leading zeros preserved.
    char hex[PAYLOAD_SIZE * 3 + 1];
    for (uint8_t i = 0; i < PAYLOAD_SIZE; i++) sprintf(&hex[i * 3], "%02X ", buf[i]);
    hex[PAYLOAD_SIZE * 3 - 1] = '\0';
    Serial.printf("[CH: %u] LEN: %u | HEX: %s | T:%lu RPD:%d\n",
                  g_channel, PAYLOAD_SIZE, hex, (unsigned long)millis(), strong ? 1 : 0);
  }
  return n;
}

// ===========================================================================
// MODE_HOP - cycle channels while dumping.
// ===========================================================================
static void serviceHop()
{
  uint32_t now = millis();
  if (now - g_hopLastMs < HOP_DWELL_MS) return;
  g_hopLastMs = now;
  g_hopIndex = (g_hopIndex + 1) % N_CHANNELS;
  g_channel  = CHANNELS[g_hopIndex];
  radio.stopListening();
  radio.setChannel(g_channel);
  radio.startListening();
}

// ===========================================================================
// MODE_LOCK — try each candidate address for real and see which one receives.
//
// This is the decisive test. In promiscuous mode the radio triggers on static
// thousands of times a minute; with a REAL address it triggers only on frames
// that actually carry that address. So a candidate that yields a steady trickle
// of packets while you press buttons — and near-silence otherwise — is correct,
// and a wrong one yields essentially nothing.
// ===========================================================================
static void applyCandidate(uint8_t i)
{
  radio.stopListening();
  radio.setDataRate(RF24_1MBPS);
  radio.setChannel(g_channel);
  radio.setAutoAck(false);
  radio.disableCRC();                 // nothing validated offline; system runs CRC-off
  radio.setAddressWidth(CANDS[i].aw); // must precede openReadingPipe
  radio.setPayloadSize(PAYLOAD_SIZE);
  radio.openReadingPipe(0, CANDS[i].addr);
  radio.startListening();
  radio.flush_rx();
}

static void lockReport()
{
  uint32_t best = 0; uint8_t bi = 0;
  Serial.println(F("\n============== LOCK-ON RESULT =============="));
  for (uint8_t i = 0; i < N_CANDS; i++) {
    Serial.printf("  %-42s %5lu packets%s\n", CANDS[i].note,
                  (unsigned long)g_lockCount[i],
                  g_lockCount[i] ? "  <==" : "");
    if (g_lockCount[i] > best) { best = g_lockCount[i]; bi = i; }
  }
  Serial.println(F("==========================================="));
  if (best == 0) {
    Serial.println(F("\nNothing received on any candidate."));
    Serial.println(F("Either no buttons were pressed during the test, or the"));
    Serial.println(F("address bytes need re-deriving. Re-run 'd' and send me"));
    Serial.println(F("a fresh capture."));
  } else {
    Serial.printf("\nWINNER: %s  (%lu packets)\n", CANDS[bi].note, (unsigned long)best);
    Serial.printf("Now capturing on it. Every line below is a REAL frame —\n");
    Serial.println(F("no static, no bit-shifting. Press buttons and watch."));
    applyCandidate(bi);
    g_gated = false;                  // real address: everything received matters
    g_mode = MODE_DUMP;
    g_packets = 0; g_strong = 0;
  }
}

static void serviceLock()
{
  uint32_t now = millis();
  if (g_lockStart == 0) {
    g_lockStart = now;
    applyCandidate(g_lockIdx);
    Serial.printf("  [%u/%u] %s\n", g_lockIdx + 1, N_CANDS, CANDS[g_lockIdx].note);
  }
  g_lockCount[g_lockIdx] += servicePackets(false);

  if (now - g_lockStart >= LOCK_DWELL_MS) {
    Serial.printf("        -> %lu packets\n", (unsigned long)g_lockCount[g_lockIdx]);
    g_lockStart = 0;
    g_lockIdx++;
    if (g_lockIdx >= N_CANDS) { lockReport(); if (g_mode == MODE_LOCK) g_mode = MODE_IDLE; }
  }
}


// ===========================================================================
// MODE_MATCH / MODE_CLEAN — receive the way the real hardware does.
//
// Everything up to here was promiscuous: 2-byte bait address, CRC off, 32-byte
// static payload. That works, but it hands us corrupted frames, because a run
// of FF bytes in the payload walks the receiver's data slicer off its DC point
// and isolated bits drop out. See FINDINGS.md, "The noise model".
//
// This mode does the opposite. It configures the radio the way a legitimate
// node on this network would: the real 5-byte address, CRC turned on, and a
// fixed payload length. The nRF24 then validates every frame in hardware and
// hands us only clean ones. Nothing corrupt gets through.
//
// We don't know the payload length or the CRC width yet, so we sweep. If any
// configuration delivers a steady stream while the controller is beaconing,
// that IS the length and that IS the CRC width — a wrong guess yields silence,
// because the checksum can only pass when the framing is exactly right.
//
// If NOTHING is received at any setting, that is informative too: either CRC
// really is off, or the transmitter uses Enhanced ShockBurst and puts a 9-bit
// packet-control field between the address and the payload, which our receiver
// (auto-ack off) does not expect.
//
// Still RX-only. setAutoAck(false) guarantees the radio never sends an ACK.
// ---------------------------------------------------------------------------
static const uint8_t REAL_ADDR[5] = { 0x3A, 0x18, 0x18, 0x18, 0x18 };
// Register order is LSByte-first and the nRF24 sends the HIGH byte first, so
// this array is on-air  18 18 18 18 3A  — settled by the lock-on test.

struct MatchCfg { uint8_t len; uint8_t crcBits; };
static const MatchCfg MATCH_CFGS[] = {
  {  6, 16 }, {  7, 16 }, {  8, 16 }, {  9, 16 },
  { 10, 16 }, { 11, 16 }, { 12, 16 },
  {  6,  8 }, {  7,  8 }, {  8,  8 }, {  9,  8 },
  { 10,  8 }, { 11,  8 }, { 12,  8 },
};
static const uint8_t N_MATCH_CFGS = sizeof(MATCH_CFGS) / sizeof(MATCH_CFGS[0]);
static const uint32_t MATCH_DWELL_MS = 4000;

static uint8_t  g_matchIdx    = 0;
static uint32_t g_matchStart  = 0;
static uint32_t g_matchCount[N_MATCH_CFGS];
static uint8_t  g_cleanLen    = 8;      // length in use once we settle on one

static void configureMatched(uint8_t len, uint8_t crcBits)
{
  radio.stopListening();
  radio.setDataRate(RF24_1MBPS);
  radio.setPALevel(RF24_PA_MIN);
  radio.setChannel(g_channel);
  radio.setAutoAck(false);                    // never transmit an ACK
  if (crcBits == 0)       radio.disableCRC();
  else                    radio.setCRCLength(crcBits == 16 ? RF24_CRC_16 : RF24_CRC_8);
  radio.setAddressWidth(5);
  radio.setPayloadSize(len);
  radio.openReadingPipe(1, REAL_ADDR);
  // Pipe 0 is still enabled from the promiscuous config, and its address
  // register still holds the 2-byte bait padded out to five bytes — which is
  // close enough to all-zeros that idle noise matches it constantly. Those
  // false frames fill the 3-deep RX FIFO and push real ones out: the first
  // v7 run kept only 575 of an expected ~3500 frames. Close it.
  radio.closeReadingPipe(0);
  radio.startListening();
  radio.flush_rx();
  g_cleanLen = len;
}

// Print clean frames. Same line format as the promiscuous dump so the existing
// analyser keeps working; RPD is still tagged, but every line here has already
// passed a hardware checksum, so RPD is only informational.
static uint32_t serviceClean(bool show)
{
  uint8_t buf[32];
  uint32_t n = 0;
  while (radio.available()) {
    const bool strong = radio.testRPD();
    radio.read(buf, g_cleanLen);
    g_packets++;
    if (strong) g_strong++;
    n++;
    if (!show) continue;
    char hex[32 * 3 + 1];
    for (uint8_t i = 0; i < g_cleanLen; i++) sprintf(&hex[i * 3], "%02X ", buf[i]);
    hex[g_cleanLen * 3 - 1] = '\0';
    Serial.printf("[CH: %u] LEN: %u | HEX: %s | T:%lu RPD:%d\n",
                  g_channel, g_cleanLen, hex, (unsigned long)millis(), strong ? 1 : 0);
  }
  return n;
}

static void matchReport()
{
  Serial.println(F("\n=== matched-receive sweep ==="));
  Serial.println(F("  payload  CRC   frames"));
  uint8_t best = 0;
  for (uint8_t i = 0; i < N_MATCH_CFGS; i++) {
    Serial.printf("  %5u    %2u    %lu%s\n", MATCH_CFGS[i].len, MATCH_CFGS[i].crcBits,
                  (unsigned long)g_matchCount[i],
                  g_matchCount[i] > 0 ? "   <<<" : "");
    if (g_matchCount[i] > g_matchCount[best]) best = i;
  }
  if (g_matchCount[best] == 0) {
    Serial.println(F("\nNothing received at any setting."));
    Serial.println(F("That means CRC really is off, or the controller uses Enhanced"));
    Serial.println(F("ShockBurst with a packet-control field. Fall back to 'k' then 'd'."));
    g_mode = MODE_IDLE;
    return;
  }
  Serial.printf("\nWinner: payload %u bytes, CRC-%u, %lu frames.\n",
                MATCH_CFGS[best].len, MATCH_CFGS[best].crcBits,
                (unsigned long)g_matchCount[best]);
  Serial.println(F("Every frame below has passed a hardware checksum. Capturing now."));
  Serial.println(F("Press x to stop.\n"));
  configureMatched(MATCH_CFGS[best].len, MATCH_CFGS[best].crcBits);
  g_packets = 0; g_strong = 0;
  g_mode = MODE_CLEAN;
}

static void serviceMatch()
{
  const uint32_t now = millis();
  if (g_matchStart == 0) {
    for (uint8_t i = 0; i < N_MATCH_CFGS; i++) g_matchCount[i] = 0;
    g_matchIdx = 0;
    g_matchStart = now;
    Serial.printf("\nMatched-receive sweep: %u settings x %lu s = %lu s total.\n",
                  N_MATCH_CFGS, (unsigned long)(MATCH_DWELL_MS / 1000),
                  (unsigned long)(N_MATCH_CFGS * MATCH_DWELL_MS / 1000));
    Serial.println(F("Keep pressing General then Clear the whole time.\n"));
    configureMatched(MATCH_CFGS[0].len, MATCH_CFGS[0].crcBits);
  }

  g_matchCount[g_matchIdx] += serviceClean(false);

  if (now - g_matchStart >= MATCH_DWELL_MS) {
    Serial.printf("  [%2u/%2u] payload %2u  CRC-%2u  -> %lu\n",
                  g_matchIdx + 1, N_MATCH_CFGS, MATCH_CFGS[g_matchIdx].len,
                  MATCH_CFGS[g_matchIdx].crcBits, (unsigned long)g_matchCount[g_matchIdx]);
    g_matchIdx++;
    g_matchStart = now;
    if (g_matchIdx >= N_MATCH_CFGS) { matchReport(); return; }
    configureMatched(MATCH_CFGS[g_matchIdx].len, MATCH_CFGS[g_matchIdx].crcBits);
  }
}


// ===========================================================================
// MODE_ADDR — the capture mode everything from here on should use.
//
// The 2026-08-26 sweep settled two things. CRC-16 received nothing at any
// length; CRC-8 received a handful of frames at five mutually incompatible
// lengths, which is what chance looks like (a CRC-8 passes 1 in 256 times, and
// the correlator triggers constantly). A controller beaconing every 30 ms
// would have delivered ~130 frames in each 4 s window, not five. So the
// checksum really is off — the original finding stands.
//
// But the sweep proved something better on the way past. Those chance frames
// came out byte-aligned at p0 (F9 77 …, FA 76 FE .. .. 73 …), exactly matching
// the layout worked out from the 3-byte lock. That means the full 40-bit
// address is right, the byte order is right, and there is NO packet-control
// field between address and payload.
//
// So: the real 5-byte address, CRC off, and a payload window long enough to
// cover the whole frame. Every delivered frame has passed a hardware match on
// all 40 address bits, so there are no false positives to filter — unlike the
// promiscuous mode, where most of what arrives is static. And it starts at p0,
// so no bytes have to be shaved off in software.
//
// Still RX-only.
// ---------------------------------------------------------------------------
static const uint8_t ADDR_LEN = 12;   // 8 payload + 2 trailer + 2 of margin

static void serviceAddrStart()
{
  configureMatched(ADDR_LEN, 0);      // 0 = CRC off
  g_packets = 0; g_strong = 0;
  Serial.println(F("\nAddressed capture: 5-byte address 18 18 18 18 3A, CRC off."));
  Serial.println(F("Every line below matched all 40 address bits in hardware."));
  Serial.println(F("Byte 0 is payload p0. Press x to stop.\n"));
  g_mode = MODE_CLEAN;
}

// ===========================================================================
// Serial command handling
// ===========================================================================
static void printHelp()
{
  Serial.printf("\n=== TM102 sniffer — RX only — %s ===\n", FW_VERSION);
  Serial.println(F("  f        HUNT: 2.5 s per channel, live. Best way to find the channel."));
  Serial.println(F("  w        WATCH one channel live for energy"));
  Serial.println(F("  s        statistical energy scan (poor fit for bursty traffic)"));
  Serial.println(F("  b        record current environment as baseline"));
  Serial.println(F("  d        dump capture on locked channel"));
  Serial.println(F("  g        toggle the signal gate (default ON: drop static)"));
  Serial.println(F("  k        LOCK-ON: try each candidate address for real"));
  Serial.println(F("  n        CAPTURE: real 5-byte address, CRC off. <-- use this one"));
  Serial.println(F("  m        CRC sweep (already run; answer was: no CRC)"));
  Serial.println(F("  h        hop across all channels while dumping"));
  Serial.println(F("  x        stop"));
  Serial.println(F("  c<n>     lock certified channel index n (1-16)"));
  Serial.println(F("  r<n>     lock raw RF_CH n (0-125)"));
  Serial.println(F("  a0 / aa  bait address 0x0000 / 0xAAAA"));
  Serial.println(F("  i        dump radio registers"));
  Serial.printf("current: RF_CH %u (%u MHz), bait %s, packets %lu\n\n",
                g_channel, 2400 + g_channel, g_baitAA ? "AAAA" : "0000",
                (unsigned long)g_packets);
}

static void lockChannel(uint8_t rfch)
{
  if (rfch > 125) { Serial.println(F("!! RF_CH out of range")); return; }
  g_channel = rfch;
  radio.stopListening();
  radio.setChannel(g_channel);
  radio.startListening();
  radio.flush_rx();
  Serial.printf("locked RF_CH %u (%u MHz)\n", g_channel, 2400 + g_channel);
}

static void handleCommand(char *cmd)
{
  switch (cmd[0]) {
    case '?': printHelp(); break;
    case 'i': selfTest(); break;

    case 'b':
      memset(g_scanHits, 0, sizeof(g_scanHits));
      g_sweepsDone = 0; g_scanIndex = 0;
      g_scanIsBaseline = true; g_mode = MODE_SCAN;
      Serial.println(F("recording BASELINE — keep the buzzers idle and untouched..."));
      break;

    case 's':
      memset(g_scanHits, 0, sizeof(g_scanHits));
      g_sweepsDone = 0; g_scanIndex = 0;
      g_scanIsBaseline = false; g_mode = MODE_SCAN;
      Serial.println(F("scanning — press a buzzer over and over, now..."));
      break;

    case 'd':
      g_mode = MODE_DUMP; g_packets = 0; g_strong = 0;
      configurePromiscuous();
      Serial.printf("DUMP on RF_CH %u (%u MHz), bait %s, gate %s\n",
                    g_channel, 2400 + g_channel, g_baitAA ? "AAAA" : "0000",
                    g_gated ? "ON (only real signal)" : "OFF (everything)");
      if (g_gated) Serial.println(F("Quiet output is normal — most captures are static and get dropped."));
      break;

    case 'n':
      serviceAddrStart();
      break;

    case 'm':
      g_mode = MODE_MATCH; g_matchIdx = 0; g_matchStart = 0;
      g_packets = 0; g_strong = 0;
      break;

    case 'k':
      g_mode = MODE_LOCK; g_lockIdx = 0; g_lockStart = 0;
      memset(g_lockCount, 0, sizeof(g_lockCount));
      Serial.println(F("\n=== LOCK-ON TEST: 40 seconds, 8 candidates, 5 s each ==="));
      Serial.printf("Channel RF_CH %u (%u MHz).\n", g_channel, 2400 + g_channel);
      Serial.println(F("Press General then Clear every ~2 s for the WHOLE test."));
      Serial.println(F("If a candidate is right it will start receiving.\n"));
      break;

    case 'g':
      g_gated = !g_gated;
      Serial.printf("gate %s\n", g_gated ? "ON — only packets with real signal"
                                          : "OFF — printing everything, including static");
      break;

    case 'h':
      g_mode = MODE_HOP; g_packets = 0; g_hopIndex = 0;
      configurePromiscuous();
      Serial.println(F("HOP across all certified channels"));
      break;

    case 'w':
      g_mode = MODE_WATCH; g_watchHits = 0; g_watchTotal = 0; g_watchPkts = 0;
      g_watchLastMs = millis();
      configurePromiscuous();
      Serial.printf("WATCHING RF_CH %u (%u MHz). Press General on the controller.\n",
                    g_channel, 2400 + g_channel);
      Serial.println(F("A '*** ENERGY' line means this is the right channel."));
      break;

    case 'f':
      g_mode = MODE_FIND;
      g_findIdx = 0; g_findPass = 0; g_findStart = 0;
      memset(g_findEnergy, 0, sizeof(g_findEnergy));
      memset(g_findPkts,   0, sizeof(g_findPkts));
      configurePromiscuous();
      Serial.println(F("\n=== HUNT: 85 seconds, then it stops by itself. ==="));
      Serial.println(F("Put the board within ~1 m of the CONTROLLER."));
      Serial.println(F("Press General, then Clear, about every 2 seconds, and"));
      Serial.println(F("DO NOT STOP until it says it has finished.\n"));
      break;

    case 'x':
      g_mode = MODE_IDLE;
      Serial.printf("stopped. %lu packets seen, %lu of them on real signal.\n",
                    (unsigned long)g_packets, (unsigned long)g_strong);
      if (g_packets && !g_strong) {
        Serial.println(F("None carried real signal. Either nothing was transmitting,"));
        Serial.println(F("or the board drifted too far from the controller."));
      }
      break;

    case 'c': {
      int n = atoi(cmd + 1);
      if (n < 1 || n > 16) { Serial.println(F("!! index must be 1-16")); break; }
      lockChannel(CHANNELS[n - 1]);
      break;
    }
    case 'r': lockChannel((uint8_t)atoi(cmd + 1)); break;

    case 'a':
      g_baitAA = (cmd[1] == 'a' || cmd[1] == 'A');
      configurePromiscuous();
      Serial.printf("bait address now %s\n", g_baitAA ? "AA AA" : "00 00");
      break;

    default: Serial.println(F("? unknown — press ? for help")); break;
  }
}

static void serviceSerial()
{
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (g_lineLen) { g_line[g_lineLen] = '\0'; handleCommand(g_line); g_lineLen = 0; }
    } else if (g_lineLen < sizeof(g_line) - 1) {
      g_line[g_lineLen++] = c;
    }
  }
}

// ===========================================================================
// Resilience — if the SPI bus falls over, say so and rebuild the radio.
// ===========================================================================
static void serviceHealth()
{
  uint32_t now = millis();
  if (now - g_healthLastMs < HEALTH_EVERY_MS) return;
  g_healthLastMs = now;

  if (!radio.isChipConnected()) {
    Serial.println(F("!! radio stopped responding — reinitialising"));
    if (initRadio()) {
      Serial.println(F("   recovered"));
      if (g_mode == MODE_DUMP || g_mode == MODE_HOP) configurePromiscuous();
    }
  }
}

// ===========================================================================
void setup()
{
  Serial.begin(SERIAL_BAUD);
  delay(300);

  // Note: no WiFi.mode(WIFI_OFF) / btStop() here, deliberately. On the Arduino
  // ESP32 core neither stack starts unless a sketch starts it, and this one
  // never does — so both radios are already silent. Calling WiFi.mode() would
  // initialise the WiFi driver in order to switch it off, which is the one
  // thing we are trying to avoid, and drags in ~200 KB of flash for nothing.
  Serial.println(F("\n\nTM102 sniffer — ESP32 + nRF24L01+PA+LNA — RX ONLY"));
  Serial.printf("FIRMWARE %s\n", FW_VERSION);
  Serial.println(F("Own WiFi and Bluetooth radios: OFF"));
  Serial.println(F("PHY from FCC LCSA072222029EA: 1 Mbps GFSK, 16 ch, 2420-2465 MHz"));

  if (!initRadio()) {
    Serial.println(F("HALTED — fix the hardware, then reset the board."));
    while (true) delay(1000);          // skills.md §4A: do not enter the main loop
  }

  printHelp();
  selfTest();
}

void loop()
{
  serviceSerial();
  serviceHealth();

  switch (g_mode) {
    case MODE_SCAN:
      scanStep();
      if (g_sweepsDone >= SCAN_SWEEPS) {
        scanReport();
        if (g_scanIsBaseline) {
          memcpy(g_baseline, g_scanHits, sizeof(g_baseline));
          g_haveBaseline = true;
          Serial.println(F("Baseline stored. Now run 's' while pressing a buzzer."));
        }
        g_mode = MODE_IDLE;
      }
      break;

    case MODE_LOCK:  serviceLock();  break;
    case MODE_MATCH: serviceMatch(); break;
    case MODE_CLEAN:                 serviceClean(true); break;
    case MODE_WATCH: serviceWatch(); break;
    case MODE_FIND:  serviceFind();  break;
    case MODE_HOP:  serviceHop();  servicePackets(true); break;
    case MODE_DUMP:                servicePackets(true); break;
    case MODE_IDLE: default:                        break;
  }
}
