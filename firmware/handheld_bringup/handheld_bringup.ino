// S3 Handheld Rev A - bench bring-up test (ESP32-S3 SuperMini)
// Board: "ESP32S3 Dev Module", Tools > USB CDC On Boot: "Enabled", Serial Monitor 115200.
// Library: U8g2 (already installed). ESP32 Arduino core 3.x (prints a "legacy i2s driver" warning - harmless).
//
// On boot: I2C scan (OLED should answer at 0x3C). Then every 250 ms Serial prints joystick + buttons.
// OLED: joystick crosshair, raw X/Y, the 4 buttons + stick click, tone state.
// Onboard RGB LED: green while the stick is clicked, otherwise follows joystick X (blue..red).
// Buttons:
//   UP    = tone on/off (starts OFF)
//   RIGHT = next tone frequency (220/440/880/1760 Hz)
//   DOWN  = next volume (1..4, starts at 1 = quiet)
//   LEFT  = "joystick = pitch" mode on/off (X = pitch 110..1760 Hz, tone plays while on)
// Screen rotation: press LEFT + RIGHT together to step through 0/90/180/270 deg (saved in flash).
// On boot: a 0.4 s 880 Hz beep through the amp (speaker check).

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Preferences.h>
#include <esp_system.h>
#include <ESP_I2S.h>
#include <math.h>

// Pins - straight from the KiCad netlist (s3_handheld_revA)
const int PIN_SDA = 11, PIN_SCL = 12;
const int PIN_JOY_X = 10, PIN_JOY_Y = 9, PIN_JOY_SW = 8;   // joystick powered from 3V3
const int PIN_AMP_LRC = 3, PIN_AMP_BCLK = 2, PIN_AMP_DIN = 1;
const int PIN_RGB = 48;
const bool JOY_SW_ENABLED = true;   // re-enabled 10-02 after the GPIO8 solder blob was cleaned
const int BTN_PINS[4] = {7, 6, 4, 5};  // UP, RIGHT, DOWN, LEFT
const char *BTN_NAMES[4] = {"UP", "RIGHT", "DOWN", "LEFT"};

// GME128128-01-IIC needs the PIMORONI variant (x offset 0); the generic one wraps the picture by 32 px
U8G2_SH1107_PIMORONI_128X128_F_HW_I2C oled(U8G2_R0, /*reset=*/U8X8_PIN_NONE, /*clock=*/PIN_SCL, /*data=*/PIN_SDA);
bool oledOk = false;
Preferences prefs;
const u8g2_cb_t *ROTS[4] = {U8G2_R0, U8G2_R1, U8G2_R2, U8G2_R3};
uint8_t rot = 3;  // R3 = right way up on the milled board (Mark, 2026-10-01); LEFT+RIGHT cycles

void applyRotation() {
  oled.setDisplayRotation(ROTS[rot]);
  prefs.putUChar("orient", rot);
  Serial.printf("OLED rotation %d (%d deg)\n", rot, rot * 90);
}

I2SClass amp;
const int RATE = 44100;
const int BLOCK = 256;

volatile bool toneOn = false, joyPitch = false;
volatile int freqIdx = 1, volIdx = 0;
volatile float joyFreq = 440;
const float FREQS[4] = {220, 440, 880, 1760};
const float VOLS[4] = {0.03f, 0.08f, 0.20f, 0.45f};
volatile uint32_t ampWriteErrs = 0;
volatile uint32_t audioBlocks = 0;   // goes up ~172/s while the audio task is alive
const char *resetWhy = "?";

const char *resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_USB: return "USB";
    default: return "OTHER";
  }
}

bool setupAmp() {
  amp.setPins(PIN_AMP_BCLK, PIN_AMP_LRC, PIN_AMP_DIN);  // bclk, ws (LRC), dout
  bool ok = amp.begin(I2S_MODE_STD, RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  Serial.printf("AMP I2S (ESP_I2S) begin=%s  BCLK %d LRC %d DIN %d\n", ok ? "OK" : "FAILED", PIN_AMP_BCLK, PIN_AMP_LRC, PIN_AMP_DIN);
  return ok;
}

void audioTask(void *) {
  static int16_t outBuf[BLOCK * 2];
  float phase = 0;
  for (;;) {
    float vol = VOLS[volIdx];
    float f = joyPitch ? joyFreq : FREQS[freqIdx];
    float step = 2.0f * PI * f / RATE;
    bool on = toneOn || joyPitch;
    for (int i = 0; i < BLOCK; i++) {
      float v = 0;
      if (on) { v = sinf(phase) * vol; phase += step; if (phase > 2 * PI) phase -= 2 * PI; }
      int16_t s = (int16_t)(v * 32767);
      outBuf[i * 2] = s; outBuf[i * 2 + 1] = s;
    }
    if (amp.write((uint8_t *)outBuf, sizeof(outBuf)) != sizeof(outBuf)) ampWriteErrs++;
    audioBlocks++;
  }
}

void setLed(uint8_t r, uint8_t g, uint8_t b) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  rgbLedWrite(PIN_RGB, r, g, b);
#else
  neopixelWrite(PIN_RGB, r, g, b);
#endif
}

void i2cScan() {
  Wire.begin(PIN_SDA, PIN_SCL);
  int found = 0;
  Serial.print("I2C scan (SDA 11, SCL 12):");
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf(" 0x%02X", a); found++; if (a == 0x3C || a == 0x3D) oledOk = true; }
  }
  Serial.println(found ? "" : " nothing found - check OLED VCC/GND/SDA/SCL and the header orientation");
}

void drawOled(int jx, int jy, bool pressed[4], bool click) {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 9, "S3 Handheld revA");
  char r[8]; snprintf(r, sizeof r, "R%d", rot); oled.drawStr(110, 9, r);
  // joystick box 64x64 at the left, crosshair at the stick position
  const int bx = 0, by = 14, bw = 64;
  oled.drawFrame(bx, by, bw, bw);
  int cx = bx + 1 + jx * (bw - 3) / 4095, cy = by + 1 + jy * (bw - 3) / 4095;
  if (click) oled.drawDisc(cx, cy, 4); else oled.drawCircle(cx, cy, 4);
  char line[24];
  snprintf(line, sizeof line, "X %4d", jx); oled.drawStr(70, 24, line);
  snprintf(line, sizeof line, "Y %4d", jy); oled.drawStr(70, 36, line);
  oled.drawStr(70, 48, click ? "CLICK" : "");
  // button diamond at the bottom right
  const int dx = 96, dy = 100, s = 12;
  const int bxy[4][2] = {{0, -s}, {s, 0}, {0, s}, {-s, 0}};  // UP RIGHT DOWN LEFT
  for (int i = 0; i < 4; i++) {
    if (pressed[i]) oled.drawBox(dx + bxy[i][0] - 4, dy + bxy[i][1] - 4, 9, 9);
    else oled.drawFrame(dx + bxy[i][0] - 4, dy + bxy[i][1] - 4, 9, 9);
  }
  snprintf(line, sizeof line, "%s %s", (toneOn || joyPitch) ? "TONE" : "tone",
           joyPitch ? "joy" : String((int)FREQS[freqIdx]).c_str());
  oled.drawStr(0, 92, line);
  snprintf(line, sizeof line, "vol %d", volIdx + 1); oled.drawStr(0, 104, line);
  if (ampWriteErrs) oled.drawStr(0, 116, "AMP ERR");
  snprintf(line, sizeof line, "up %lus %s", millis() / 1000, resetWhy);
  oled.drawStr(0, 127, line);
  oled.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(1500);  // give native USB time to enumerate
  resetWhy = resetReason();
  Serial.printf("\n=== S3 Handheld Rev A bring-up === last reset: %s\n", resetWhy);
  for (int i = 0; i < 4; i++) pinMode(BTN_PINS[i], INPUT_PULLUP);
  if (JOY_SW_ENABLED) pinMode(PIN_JOY_SW, INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_JOY_X, ADC_11db);
  analogSetPinAttenuation(PIN_JOY_Y, ADC_11db);

  i2cScan();
  if (oledOk) {
    oled.setBusClock(400000);
    oled.begin();
    oled.setContrast(200);
    prefs.begin("handheld", false);
    rot = prefs.getUChar("orient", 3) & 3;
    applyRotation();
    Serial.println("OLED found - drawing");
  } else {
    Serial.println("!! no OLED at 0x3C/0x3D - continuing without it");
  }

  if (!setupAmp()) Serial.println("!! I2S setup failed - see codes above");
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, NULL, 5, NULL, 0);
  // boot beep: 880 Hz, volume 3, 0.4 s
  freqIdx = 2; volIdx = 2; toneOn = true;
  delay(400);
  toneOn = false; freqIdx = 1; volIdx = 0;
  Serial.println("UP=tone  RIGHT=freq  DOWN=volume  LEFT=joystick-pitch mode\n");
}

void loop() {
  static uint32_t loopStart = 0, loopMax = 0;
  uint32_t nowMs = millis();
  if (loopStart) loopMax = max(loopMax, nowMs - loopStart);
  loopStart = nowMs;
  static bool prev[4] = {true, true, true, true};
  static uint32_t lastEdge[4] = {0};
  bool pressed[4];
  for (int i = 0; i < 4; i++) {
    bool now = digitalRead(BTN_PINS[i]);
    pressed[i] = !now;
    if (now != prev[i] && millis() - lastEdge[i] > 30) {
      lastEdge[i] = millis();
      prev[i] = now;
      if (!now) {
        if (i == 0) toneOn = !toneOn;
        if (i == 1) freqIdx = (freqIdx + 1) % 4;
        if (i == 2) volIdx = (volIdx + 1) % 4;
        if (i == 3) joyPitch = !joyPitch;
        Serial.printf(">> %s  tone=%s %.0fHz vol=%d joyPitch=%s\n", BTN_NAMES[i],
                      toneOn ? "ON" : "off", FREQS[freqIdx], volIdx + 1, joyPitch ? "ON" : "off");
      }
    }
  }
  // LEFT + RIGHT together: next screen rotation
  static bool comboPrev = false;
  bool combo = pressed[1] && pressed[3];
  if (combo && !comboPrev && oledOk) { rot = (rot + 1) & 3; applyRotation(); }
  comboPrev = combo;

  bool click = JOY_SW_ENABLED && !digitalRead(PIN_JOY_SW);
  int jx = analogRead(PIN_JOY_X), jy = analogRead(PIN_JOY_Y);
  joyFreq = 110.0f * powf(2.0f, jx / 4095.0f * 4.0f);  // 4 octaves, 110..1760 Hz

  static uint32_t lastLed = 0;
  if (millis() - lastLed >= 20) {
    lastLed = millis();
    uint8_t v = jx * 255 / 4095;
    if (click) setLed(0, 40, 0); else setLed(v / 4, 0, (255 - v) / 4);
  }

  static uint32_t lastDraw = 0;
  if (oledOk && millis() - lastDraw >= 50) {
    lastDraw = millis();
    drawOled(jx, jy, pressed, click);
  }

  static uint32_t lastPrint = 0;
  if (millis() - lastPrint >= 250) {
    lastPrint = millis();
    static int xmin = 4095, xmax = 0, ymin = 4095, ymax = 0;
    xmin = min(xmin, jx); xmax = max(xmax, jx); ymin = min(ymin, jy); ymax = max(ymax, jy);
    Serial.printf("JOY X %4d (%4d..%4d) Y %4d (%4d..%4d) SW=%d | BTN", jx, xmin, xmax, jy, ymin, ymax, click);
    for (int i = 0; i < 4; i++) Serial.printf(" %s=%d", BTN_NAMES[i], pressed[i]);
    if (ampWriteErrs) Serial.printf(" | AMP ERR %lu", (unsigned long)ampWriteErrs);
    Serial.printf(" | up %lus loopMax %lums audio %lu tone=%d", millis() / 1000, (unsigned long)loopMax,
                  (unsigned long)audioBlocks, (int)(toneOn || joyPitch));
    loopMax = 0;
    Serial.println();
  }
}
