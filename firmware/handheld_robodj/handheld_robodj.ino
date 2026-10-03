/*
  HANDHELD ROBO DJ - robot voice + turntable scratching + 16-step sequencer (s3_handheld_revA)
  =========================================================================================
  The robot voice is SAM (Software Automatic Mouth, 1982) - the C core from earlephilhower/ESP8266SAM
  (GPL v3) is bundled in this folder. At boot SAM pre-renders a bank of words into memory; a
  "turntable deck" then plays, scratches, stutters and reverses them over a drum machine.

  Diamond:   UP = SHIFT    LEFT = VOICE    RIGHT = BEAT    DOWN = FX
  Live hits play the instant you press. With REC on they're quantized into the 16 steps.

  VOICE (LEFT)    tap = say the word   tap-tap = next word   tap-tap-tap = random word
                  hold = SCRATCH it (tempo-synced, style below)
  BEAT  (RIGHT)   tap = kick   tap-tap = snare   tap-tap-tap = clap    hold = hi-hat roll
  FX    (DOWN)    tap = next FX (CLEAN CRUSH ECHO GRAIN SWARM VOX CHOIR)
                  GRAIN = slow-stretched cloud of grains at mixed pitches; SWARM = dense detuned robot choir
                  VOX = 16-band vocoder on a chord + some dry voice (words stay clear)
                  CHOIR = the pure vocoder (bold, words melt into the chord)
                  the vocoder chord follows a progression, one chord per bar
                  double = GROOVE on/off    hold = STUTTER (r-r-r-robot)
  SHIFT + VOICE   tap = REC on/off   double = clear voice track   triple = clear all   hold = REVERSE
  SHIFT + BEAT    tap = next scratch style (BABY CHIRP TRANSFORM TEAR FLARE)
                  double = tempo (100 110 120 80 90)   triple = chord progression   hold = TAPE STOP
  SHIFT + FX      tap = volume (4 steps)   double = next robot voice (re-renders the words)
  SHIFT alone     tap = MUTATE (new words, same rhythm)   double = AUTO (random beat + words)
                  hold = UNDERWATER

  Board: "ESP32S3 Dev Module", USB CDC On Boot: Enabled, PSRAM: "QSPI PSRAM" (works without, fewer words).
  Libraries: U8g2. ESP32 core 3.x (ESP_I2S).
*/

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Preferences.h>
#include <ESP_I2S.h>
#include "esp_dsp.h"
#include "SamData.h"
#include "sam.h"
#include "reciter.h"

// ---------------- pins ----------------
#define PIN_SDA   11
#define PIN_SCL   12
#define AMP_BCLK  2
#define AMP_LRC   3
#define AMP_DIN   1
#define PIN_RGB   48
#define PIN_SHIFT 7     // UP
#define PIN_BEAT  6     // RIGHT
#define PIN_FX    4     // DOWN
#define PIN_VOICE 5     // LEFT

#define FS     22050          // SAM's native rate
#define INV_FS (1.0f / FS)
#define BLOCK  128

const float VOLS[4] = {0.06f, 0.12f, 0.22f, 0.35f};
volatile int volIdx = 1;

// ---------------- types first (Arduino auto-prototypes) ----------------
struct Btn {
  uint8_t pin;
  bool off, down, cmd, longOn, other;
  uint32_t tDown, tUp, tChange, cmdDeadline;
  int taps, cmdTaps;
};
struct Word { const char* text; uint32_t off, len; };
struct Step { int8_t word; uint8_t drum; uint8_t flags; bool skip; };
struct Voice { const char* name; uint8_t speed, pitch, throat, mouth; };

enum { DR_KICK = 1, DR_SNARE = 2, DR_CLAP = 4, DR_HAT = 8 };
enum { SF_SCRATCH = 1, SF_REV = 2 };
enum { FX_CLEAN, FX_CRUSH, FX_ECHO, FX_GRAIN, FX_SWARM, FX_VOX, FX_CHOIR, FX_COUNT };
const char* FX_NAMES[] = {"CLEAN", "CRUSH", "ECHO", "GRAIN", "SWARM", "VOX", "CHOIR"};
enum { SC_BABY, SC_CHIRP, SC_TRANS, SC_TEAR, SC_FLARE, SC_COUNT };
const char* SC_NAMES[] = {"BABY", "CHIRP", "TRANSFORM", "TEAR", "FLARE"};
const int TEMPOS[] = {100, 110, 120, 80, 90};
const Voice VOICES[] = {
  {"ROBOT", 92, 60, 190, 190}, {"SAM", 72, 64, 128, 128}, {"ELF", 72, 64, 110, 160},
  {"E.T.", 100, 64, 150, 200}, {"STUFFY", 82, 72, 110, 105}, {"OLD LADY", 82, 32, 145, 145},
};
const int NVOICES = 6;

Word bank[] = {
  {"HELLO"}, {"ROBOT"}, {"YEAH"}, {"ERROR"}, {"DANCE"}, {"BEEP"}, {"BOOP"}, {"SYSTEM"},
  {"ONLINE"}, {"WOW"}, {"FRESH"}, {"SCRATCH"}, {"ONE"}, {"TWO"}, {"THREE"}, {"FOUR"},
  {"OH NO"}, {"COMPUTER"}, {"PARTY"}, {"FUNKY"}, {"AWESOME"}, {"DOES NOT COMPUTE"},
};
const int NWORDS = sizeof(bank) / sizeof(bank[0]);

U8G2_SH1107_PIMORONI_128X128_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);
Preferences prefs;
I2SClass amp;

void drawFrame();

// =====================================================================
// SAM -> sample pool
// =====================================================================
SamData* samdata;
int8_t* pool = nullptr;
uint32_t poolSize = 0, poolPos = 0;
volatile bool bankBusy = true;
volatile int voiceIdx = 0;
int renderProgress = 0;

static void samByte(void*, unsigned char b) { if (poolPos < poolSize) pool[poolPos++] = (int8_t)((int)b - 128); }

bool sayToPool(const char* text, const Voice& v) {
  char input[256];
  int n = 0;
  for (; text[n] && n < 200; n++) input[n] = toupper((int)text[n]);
  input[n] = 0;
  strcat(input, "[");
  samdata = new SamData;
  if (!samdata) return false;
  EnableSingmode(0);
  SetSpeed(v.speed); SetPitch(v.pitch); SetThroat(v.throat); SetMouth(v.mouth);
  bool ok = TextToPhonemes(input);
  if (ok) { SetInput(input); SAMMain(samByte, nullptr); }
  delete samdata;
  return ok;
}

void renderBank(int vi) {
  bankBusy = true;
  delay(30);                                   // let the audio task notice
  poolPos = 0;
  for (int i = 0; i < NWORDS; i++) {
    renderProgress = i;
    drawFrame();
    uint32_t start = poolPos;
    bank[i].off = start; bank[i].len = 0;
    if (poolSize - poolPos < FS / 2) continue; // pool full: skip the rest
    if (!sayToPool(bank[i].text, VOICES[vi])) { poolPos = start; continue; }
    uint32_t a = start, b = poolPos;            // trim silence at both ends
    while (a < b && abs(pool[a]) < 4) a++;
    while (b > a && abs(pool[b - 1]) < 4) b--;
    bank[i].off = a; bank[i].len = b > a + 8 ? b - a : 0;
  }
  renderProgress = NWORDS;
  Serial.printf("voice %s: %u bytes for %d words\n", VOICES[vi].name, poolPos, NWORDS);
  bankBusy = false;
}

static inline float wordAt(int w, float p) {
  const Word& wd = bank[w];
  if (p < 0 || p >= wd.len - 1) return 0;
  int i = (int)p; float f = p - i;
  float a = pool[wd.off + i], b = pool[wd.off + i + 1];
  return (a + (b - a) * f) * (1.0f / 128.0f);
}

// =====================================================================
// DSP helpers + drums (from the improv engine)
// =====================================================================
float sineTab[1025];
static inline float wrap1(float p) { return p - floorf(p); }
static inline float fsin(float ph) {
  float x = ph * 1024.0f; int i = (int)x; float f = x - i; i &= 1023;
  return sineTab[i] + (sineTab[i + 1] - sineTab[i]) * f;
}
static uint32_t nstate = 22222;
static inline float nz() { nstate ^= nstate << 13; nstate ^= nstate >> 17; nstate ^= nstate << 5; return (int32_t)nstate * (1.0f / 2147483648.0f); }
static inline float dcoef(float sec) { return expf(-1.0f / (sec * FS)); }
static inline float softclip(float x) { if (x > 3) return 1; if (x < -3) return -1; return x * (27 + x * x) / (27 + 9 * x * x); }

struct Kick {
  float ph = 0, env = 0, penv = 0, d = 0.9997f;
  void trig(float a) { env = a; penv = 1; ph = 0; d = dcoef(0.35f); }
  inline float s(float rate) {
    if (env < 0.0001f) return 0;
    float f = (48 + 110 * penv) * rate;
    penv *= 0.9965f;
    ph = wrap1(ph + f * INV_FS);
    float o = fsin(ph) * env; env *= d; return o;
  }
};
struct Noise {                                     // 0 hat, 1 snare, 2 clap
  float env = 0, d = 0.99f, prev = 0, tenv = 0, ph = 0, lp = 0, bp = 0;
  int type = 0;
  void trig(float a, float len, int t) { env = a; d = dcoef(len); type = t; tenv = 1; }
  inline float s() {
    if (env < 0.0001f) return 0;
    float n = nz(), o;
    if (type == 0) { o = n - prev; prev = n; }
    else if (type == 1) { o = n * 0.6f + fsin(ph) * tenv; ph = wrap1(ph + 190 * INV_FS); tenv *= 0.9985f; }
    else { float f = 0.42f; lp += f * bp; bp += f * (n - lp - 0.6f * bp); o = bp * 1.6f; }
    o *= env; env *= d; return o;
  }
};
Kick kick; Noise snare, hat, clap;

void hitDrums(uint8_t bits) {
  if (bits & DR_KICK) kick.trig(0.9f);
  if (bits & DR_SNARE) snare.trig(0.5f, 0.18f, 1);
  if (bits & DR_CLAP) clap.trig(0.45f, 0.1f, 2);
  if (bits & DR_HAT) hat.trig(0.18f, 0.03f, 0);
}

#define DLY_N 16384
float* dlyBuf = nullptr;
int dlyW = 0;

// =====================================================================
// Instrument state
// =====================================================================
Step seq[16];
volatile int curStep = 0, curWord = 1, fx = FX_CLEAN, scStyle = SC_BABY, tempoIdx = 0;
volatile bool rec = false, groove = true, stutterOn = false, reverseOn = false, tapeOn = false, underOn = false;
volatile bool scratchHeld = false;
volatile float tapeRate = 1, stepFrac = 0;
char msg[24] = ""; volatile uint32_t msgAt = 0;
void flash(const char* m) { strncpy(msg, m, sizeof(msg) - 1); msgAt = millis(); }

// the turntable deck
struct Deck {
  int word = -1;
  float pos = 0, rate = 1, gate = 1, base = 0;
  bool on = false, scratch = false, rev = false;
  int scratchSteps = 0;                          // >0: sequencer-triggered scratch, counts down
} deck;
volatile float deckVis = 0, outPeak = 0;
volatile float scopeBuf[128];

void deckPlay(int w, bool rev) {
  if (bankBusy || w < 0 || bank[w].len == 0) return;
  deck.word = w; deck.rev = rev; deck.scratch = false; deck.scratchSteps = 0;
  deck.pos = rev ? bank[w].len - 2 : 0;
  deck.on = true;
}
void deckScratch(int w, int steps) {
  if (bankBusy || w < 0 || bank[w].len == 0) return;
  deck.word = w; deck.scratch = true; deck.scratchSteps = steps; deck.base = 0; deck.pos = 0; deck.on = true;
}

int nextWord(int w) { for (int k = 1; k <= NWORDS; k++) { int c = (w + k) % NWORDS; if (bank[c].len) return c; } return w; }
int randomWord() { for (int k = 0; k < 40; k++) { int c = random(NWORDS); if (bank[c].len) return c; } return curWord; }

int quantStep() { return (curStep + (stepFrac > 0.5f ? 1 : 0)) & 15; }
void recVoice(uint8_t flags) {
  if (!rec) return;
  int q = quantStep();
  seq[q].word = curWord; seq[q].flags = flags;
  if (q != curStep) seq[q].skip = true;         // we just played it live
}
void recDrum(uint8_t bit) {
  if (!rec) return;
  int q = quantStep();
  seq[q].drum |= bit;
  if (q != curStep) seq[q].skip = true;
}

void clearVoices() { for (auto& s : seq) { s.word = -1; s.flags = 0; } }
void clearAll() { for (auto& s : seq) s = {-1, 0, 0, false}; }
void mutate() { for (auto& s : seq) if (s.word >= 0) s.word = randomWord(); }
void autoPattern() {
  clearAll();
  static const uint8_t K[4][16] = {
    {1,0,0,0, 0,0,0,0, 1,0,1,0, 0,0,0,0}, {1,0,0,1, 0,0,1,0, 0,0,1,0, 0,0,0,0},
    {1,0,0,0, 0,0,0,1, 0,0,1,0, 0,1,0,0}, {1,0,1,0, 0,0,0,0, 1,0,0,0, 0,0,1,0}};
  int k = random(4);
  for (int i = 0; i < 16; i++) {
    if (K[k][i]) seq[i].drum |= DR_KICK;
    if (i == 4 || i == 12) seq[i].drum |= random(3) ? DR_SNARE : DR_CLAP;
    if ((i & 1) == 0 || random(5) == 0) seq[i].drum |= DR_HAT;
  }
  int a = randomWord(), b = randomWord();
  int hits = 3 + random(3);
  for (int h = 0; h < hits; h++) {
    int st = random(16);
    seq[st].word = random(3) ? a : b;
    seq[st].flags = random(4) == 0 ? SF_SCRATCH : (random(6) == 0 ? SF_REV : 0);
  }
  groove = false;
  flash("AUTO");
}

// vocoder chord progression (one chord per bar)
struct Prog { const char* name; int8_t root[4]; uint8_t q[4]; };
enum { Q_MAJ, Q_MIN, Q_MAJ7, Q_MIN7, Q_DOM7, Q_SUS4 };
const int8_t QI[6][4] = {{0, 4, 7, -1}, {0, 3, 7, -1}, {0, 4, 7, 11}, {0, 3, 7, 10}, {0, 4, 7, 10}, {0, 5, 7, -1}};
const char* QN[6] = {"", "m", "maj7", "m7", "7", "sus4"};
const char* NOTE[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
const Prog PROGS[] = {
  {"EPIC",  {9, 5, 0, 7},  {Q_MIN, Q_MAJ, Q_MAJ, Q_MAJ}},       // Am F C G
  {"POP",   {0, 7, 9, 5},  {Q_MAJ, Q_MAJ, Q_MIN, Q_MAJ}},       // C G Am F
  {"DREAM", {0, 9, 5, 7},  {Q_MAJ7, Q_MIN7, Q_MAJ7, Q_DOM7}},   // Cmaj7 Am7 Fmaj7 G7
  {"SPACE", {0, 10, 8, 7}, {Q_MAJ, Q_MAJ, Q_MAJ, Q_SUS4}},      // C Bb Ab Gsus4
};
const int NPROGS = 4;
volatile int progIdx = 0, progPos = 0;
void vocChord(int i);

// =====================================================================
// Gestures (run inside the audio task every 5.8 ms: low latency)
// =====================================================================
Btn bS = {PIN_SHIFT}, bV = {PIN_VOICE}, bB = {PIN_BEAT}, bF = {PIN_FX};

int edge(Btn& b, uint32_t now) {
  if (b.off) return 0;
  bool raw = digitalRead(b.pin) == LOW;
  if (raw != b.down && now - b.tChange > 15) {
    b.down = raw; b.tChange = now;
    if (raw) { b.tDown = now; return 1; }
    return -1;
  }
  return 0;
}

void inputTick() {
  uint32_t now = millis();
  int e;

  // ---- SHIFT alone ----
  e = edge(bS, now);
  if (e == 1) bS.other = false;
  if (e == -1) {
    if (bS.longOn) { underOn = false; bS.longOn = false; }
    else if (!bS.other) { bS.cmdTaps++; bS.cmdDeadline = now + 280; }
  }
  if (bS.down && !bS.other && !bS.longOn && now - bS.tDown > 700) { bS.longOn = true; underOn = true; }
  if (bS.cmdTaps && !bS.down && now > bS.cmdDeadline) {
    if (bS.cmdTaps == 1) { mutate(); flash("MUTATE"); } else autoPattern();
    bS.cmdTaps = 0;
  }

  // ---- VOICE ----
  e = edge(bV, now);
  if (e == 1) {
    if (bS.down) { bV.cmd = true; bS.other = true; }
    else {
      bV.cmd = false;
      bV.taps = (now - bV.tUp < 260) ? bV.taps + 1 : 1;
      if (bV.taps == 2) curWord = nextWord(curWord);
      if (bV.taps >= 3) curWord = randomWord();
      deckPlay(curWord, reverseOn);
      recVoice(reverseOn ? SF_REV : 0);
    }
  }
  if (bV.down && !bV.longOn && now - bV.tDown > 300) {
    bV.longOn = true;
    if (bV.cmd) reverseOn = true;
    else { scratchHeld = true; deckScratch(curWord, 0); }
  }
  if (e == -1) {
    if (bV.cmd) {
      if (bV.longOn) reverseOn = false;
      else { bV.cmdTaps++; bV.cmdDeadline = now + 280; }
    } else if (scratchHeld) { scratchHeld = false; deck.scratch = false; deck.on = false; }
    bV.longOn = false; bV.tUp = now;
  }
  if (scratchHeld && rec) { int q = quantStep(); seq[q].word = curWord; seq[q].flags = SF_SCRATCH; }
  if (bV.cmdTaps && !bV.down && now > bV.cmdDeadline) {
    if (bV.cmdTaps == 1) { rec = !rec; flash(rec ? "REC" : "rec off"); }
    else if (bV.cmdTaps == 2) { clearVoices(); flash("voices cleared"); }
    else { clearAll(); groove = false; flash("all cleared"); }
    bV.cmdTaps = 0;
  }

  // ---- BEAT ----
  e = edge(bB, now);
  if (e == 1) {
    if (bS.down) { bB.cmd = true; bS.other = true; }
    else {
      bB.cmd = false;
      bB.taps = (now - bB.tUp < 260) ? bB.taps + 1 : 1;
      uint8_t bit = bB.taps == 1 ? DR_KICK : (bB.taps == 2 ? DR_SNARE : DR_CLAP);
      hitDrums(bit); recDrum(bit);
    }
  }
  if (bB.down && !bB.longOn && now - bB.tDown > 350) {
    bB.longOn = true;
    if (bB.cmd) tapeOn = true;
  }
  if (e == -1) {
    if (bB.cmd) { if (bB.longOn) tapeOn = false; else { bB.cmdTaps++; bB.cmdDeadline = now + 280; } }
    bB.longOn = false; bB.tUp = now;
  }
  if (bB.cmdTaps && !bB.down && now > bB.cmdDeadline) {
    if (bB.cmdTaps == 1) { scStyle = (scStyle + 1) % SC_COUNT; flash(SC_NAMES[scStyle]); }
    else if (bB.cmdTaps == 2) { tempoIdx = (tempoIdx + 1) % 5; static char t[12]; snprintf(t, sizeof t, "%d BPM", TEMPOS[tempoIdx]); flash(t); }
    else { progIdx = (progIdx + 1) % NPROGS; vocChord(progPos); flash(PROGS[progIdx].name); }
    bB.cmdTaps = 0;
  }

  // ---- FX ----
  e = edge(bF, now);
  if (e == 1) { bF.cmd = bS.down; if (bF.cmd) bS.other = true; }
  if (bF.down && !bF.longOn && now - bF.tDown > 350) {
    bF.longOn = true;
    if (!bF.cmd) stutterOn = true;
  }
  if (e == -1) {
    if (bF.longOn) stutterOn = false;
    else { bF.cmdTaps++; bF.cmdDeadline = now + 280; }
    bF.longOn = false;
  }
  if (bF.cmdTaps && !bF.down && now > bF.cmdDeadline) {
    if (bF.cmd) {
      if (bF.cmdTaps == 1) { volIdx = (volIdx + 1) % 4; static char v[12]; snprintf(v, sizeof v, "volume %d/4", volIdx + 1); flash(v); }
      else voiceIdx = (voiceIdx + 1) % NVOICES;  // loop() sees the change and re-renders
    } else {
      if (bF.cmdTaps == 1) { fx = (fx + 1) % FX_COUNT; flash(FX_NAMES[fx]); }
      else { groove = !groove; flash(groove ? "groove on" : "groove off"); }
    }
    bF.cmdTaps = 0;
  }
}

// =====================================================================
// Audio
// =====================================================================
// scratch hand position (0..1) and crossfader gate for phase 0..1 of one stroke cycle
static inline void scratchShape(int style, float ph, float& x, float& g) {
  switch (style) {
    case SC_CHIRP: x = 0.5f - 0.5f * cosf(2 * PI * ph); g = ph < 0.45f ? 1 : 0; break;
    case SC_TRANS: { float p2 = ph * 0.5f; x = 0.5f - 0.5f * cosf(2 * PI * p2); g = ((int)(ph * 8)) & 1 ? 0 : 1; break; }
    case SC_TEAR:
      if (ph < 0.3f) x = ph / 0.3f;
      else if (ph < 0.55f) x = 1 - (ph - 0.3f) / 0.25f * 0.5f;
      else if (ph < 0.65f) x = 0.5f;
      else x = 0.5f - (ph - 0.65f) / 0.35f * 0.5f;
      g = 1; break;
    case SC_FLARE: x = 0.5f - 0.5f * cosf(2 * PI * ph); g = (fabsf(ph - 0.25f) < 0.04f || fabsf(ph - 0.75f) < 0.04f) ? 0 : 1; break;
    default: x = 0.5f - 0.5f * cosf(2 * PI * ph); g = 1; break;
  }
}

// ---- granular voice FX: overlapping windowed grains read from the word ----
struct Grain { int word; float pos, inc, age, len; bool on; };
Grain grains[10];
volatile float grainVis[10];
int grainTimer = 0;

void spawnGrain(bool swarm) {
  if (deck.word < 0) return;
  for (auto& g : grains) if (!g.on) {
    static const float GP[6] = {1.0f, 1.0f, 0.5f, 1.5f, 2.0f, 0.75f};
    g.word = deck.word;
    g.len = swarm ? FS * (0.03f + 0.02f * random(100) / 100.0f) : FS * (0.05f + 0.04f * random(100) / 100.0f);
    float spray = (random(200) - 100) / 100.0f * FS * (swarm ? 0.012f : 0.03f);
    g.pos = max(0.0f, deck.pos + spray);
    g.inc = swarm ? 1.0f + (random(200) - 100) / 100.0f * 0.12f : GP[random(6)];
    if (deck.rev) g.inc = -g.inc;
    g.age = 0; g.on = true;
    return;
  }
}

static inline float grainSum() {
  float v = 0;
  for (int i = 0; i < 10; i++) {
    Grain& g = grains[i];
    if (!g.on) continue;
    float w = fsin(g.age / g.len * 0.5f);              // sine window, 0..1..0
    v += wordAt(g.word, g.pos) * w * w;
    g.pos += g.inc * tapeRate;
    if (++g.age >= g.len) g.on = false;
  }
  return v;
}

// ---- vocoder (from Robo Choir): the deck's voice shapes a chord across 16 bands ----
#define NB 16
struct VOsc { float ph = 0, ph2 = 0, f = 110, target = 110; bool on = false; };
VOsc vosc[5];
float coefM[NB][5], coefC[NB][5], wM[NB][2], wC[NB][2], bandEnv[NB];
float vBuf[BLOCK], dBuf[BLOCK], carBuf[BLOCK], noiseBuf[BLOCK], tmpM[BLOCK], tmpC[BLOCK], tmpC2[BLOCK], vocOut[BLOCK];
float vocAgc = 6;

void setupVocoder() {
  for (int b = 0; b < NB; b++) {
    float f = 150.0f * powf(5500.0f / 150.0f, b / (float)(NB - 1));
    dsps_biquad_gen_bpf_f32(coefM[b], f / FS, 5.0f);
    dsps_biquad_gen_bpf_f32(coefC[b], f / FS, 5.0f);
  }
}

void vocChord(int i) {
  const Prog& p = PROGS[progIdx];
  int root = p.root[i & 3], q = p.q[i & 3];
  for (int k = 0; k < 4; k++) {
    int iv = QI[q][k];
    if (iv < 0) { vosc[k].on = false; continue; }
    int n = 48 + root + iv;
    while (n < 52) n += 12;
    while (n > 67) n -= 12;
    vosc[k].target = 440.0f * powf(2.0f, (n - 69) / 12.0f);
    if (!vosc[k].on) vosc[k].f = vosc[k].target;
    vosc[k].on = true;
  }
  int b = 36 + root; if (b > 43) b -= 12;
  vosc[4].target = 440.0f * powf(2.0f, (b - 69) / 12.0f);
  if (!vosc[4].on) vosc[4].f = vosc[4].target;
  vosc[4].on = true;
}

void chordLabel(char* out, size_t n) {
  const Prog& p = PROGS[progIdx];
  snprintf(out, n, "%s%s", NOTE[p.root[progPos & 3]], QN[p.q[progPos & 3]]);
}

static inline float blep(float t, float dt) {
  if (t < dt) { t /= dt; return t + t - t * t - 1; }
  if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
  return 0;
}

// replaces vBuf with the vocoded voice (plus some dry voice when "clear" is set)
void vocodeBlock(bool clear) {
  for (int n = 0; n < BLOCK; n++) {
    float c = 0;
    for (int k = 0; k < 5; k++) {
      VOsc& o = vosc[k];
      if (!o.on) continue;
      o.f += (o.target - o.f) * 0.0015f;
      float dt = o.f * INV_FS, dt2 = dt * 1.007f;
      o.ph += dt; if (o.ph >= 1) o.ph -= 1;
      o.ph2 += dt2; if (o.ph2 >= 1) o.ph2 -= 1;
      c += (2 * o.ph - 1 - blep(o.ph, dt) + 2 * o.ph2 - 1 - blep(o.ph2, dt2)) * (k == 4 ? 0.8f : 0.5f);
    }
    carBuf[n] = c;
    noiseBuf[n] = nz();
  }
  memset(vocOut, 0, sizeof vocOut);
  for (int b = 0; b < NB; b++) {
    dsps_biquad_f32(vBuf, tmpM, BLOCK, coefM[b], wM[b]);
    if (b >= NB - 3) for (int n = 0; n < BLOCK; n++) tmpC[n] = carBuf[n] * 0.4f + noiseBuf[n] * 1.2f;
    else memcpy(tmpC, carBuf, sizeof tmpC);
    dsps_biquad_f32(tmpC, tmpC2, BLOCK, coefC[b], wC[b]);
    float env = bandEnv[b];
    for (int n = 0; n < BLOCK; n++) {
      float a = fabsf(tmpM[n]);
      env += (a - env) * (a > env ? 0.02f : 0.0025f);
      vocOut[n] += tmpC2[n] * env;
    }
    bandEnv[b] = env;
  }
  float pk = 0.0001f;
  for (int n = 0; n < BLOCK; n++) pk = max(pk, fabsf(vocOut[n]));
  float want = min(40.0f, 0.55f / pk);
  if (want < vocAgc) vocAgc += (want - vocAgc) * 0.3f;
  else if (pk > 0.002f) vocAgc += (want - vocAgc) * 0.01f;
  for (int n = 0; n < BLOCK; n++) vBuf[n] = vocOut[n] * vocAgc + (clear ? vBuf[n] * 0.45f : 0);
}

void setupAmp() {
  amp.setPins(AMP_BCLK, AMP_LRC, AMP_DIN);
  bool ok = amp.begin(I2S_MODE_STD, FS, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  Serial.printf("I2S %s\n", ok ? "OK" : "FAILED");
}

void audioTask(void*) {
  static int16_t out[BLOCK * 2];
  float stepPos = 0, crushHold = 0, underLP = 0, under = 0, deckLP = 0, gateSm = 1;
  int crushCnt = 0;
  float stutterBase = -1;
  for (;;) {
    inputTick();
    tapeRate += ((tapeOn ? 0.0f : 1.0f) - tapeRate) * (tapeOn ? 0.03f : 0.08f);
    under += ((underOn ? 1.0f : 0.0f) - under) * 0.05f;
    float stepLen = FS * 60.0f / TEMPOS[tempoIdx] / 4.0f;
    float master = VOLS[volIdx];
    bool busy = bankBusy;
    if (busy) deck.on = false;
    if (!stutterOn) stutterBase = -1;

    for (int n = 0; n < BLOCK; n++) {
      // ---- sequencer clock ----
      stepPos += tapeRate;
      if (stepPos >= stepLen) {
        stepPos -= stepLen;
        int s = (curStep + 1) & 15;
        curStep = s;
        if (s == 0) { progPos = (progPos + 1) & 3; vocChord(progPos); }
        Step& st = seq[s];
        uint8_t d = st.skip ? 0 : st.drum;
        if (groove) {
          if (s == 0 || s == 8 || s == 10) d |= DR_KICK;
          if (s == 4 || s == 12) d |= DR_SNARE;
          if ((s & 1) == 0) d |= DR_HAT;
        }
        if (bB.longOn && !bB.cmd) d |= DR_HAT;   // hi-hat roll while BEAT is held
        if (bB.longOn && !bB.cmd && rec) seq[s].drum |= DR_HAT;
        hitDrums(d);
        if (deck.scratch && deck.scratchSteps > 0 && --deck.scratchSteps == 0) { deck.scratch = false; deck.on = false; }
        if (!st.skip && st.word >= 0 && !scratchHeld && !busy) {
          if (st.flags & SF_SCRATCH) deckScratch(st.word, 1);
          else deckPlay(st.word, st.flags & SF_REV);
        }
        st.skip = false;
        if (stutterOn && deck.word >= 0 && !busy) {        // retrigger every 16th from where we were
          if (stutterBase < 0) stutterBase = deck.on ? deck.pos : 0;
          deck.pos = stutterBase; deck.on = true; deck.scratch = false;
        }
      }
      stepFrac = stepPos / stepLen;

      // ---- deck ----
      float v = 0, gTarget = 1;
      if (deck.on && deck.word >= 0) {
        const Word& w = bank[deck.word];
        if (deck.scratch) {
          float ph = wrap1(((curStep & 1) + stepPos / stepLen) * 0.5f);   // one stroke per 8th note
          float x, g;
          scratchShape(scStyle, ph, x, g);
          float depth = min((float)w.len - 2, FS * 0.22f);
          float target = deck.base + x * depth;
          deck.pos += (target - deck.pos) * 0.35f;            // the hand has a little mass
          gTarget = g;
        } else if (fx == FX_GRAIN || fx == FX_SWARM) {
          bool sw = fx == FX_SWARM;
          deck.pos += (deck.rev ? -1 : 1) * (sw ? 0.8f : 0.33f) * tapeRate;   // the read pointer crawls
          if (deck.pos < 0 || deck.pos >= w.len - 1) deck.on = false;
          if (--grainTimer <= 0) { spawnGrain(sw); grainTimer = sw ? FS / 160 : FS / 55; }
        } else {
          deck.pos += (deck.rev ? -1 : 1) * deck.rate * tapeRate;
          if (deck.pos < 0 || deck.pos >= w.len - 1) deck.on = false;
          if (stutterOn && stutterBase >= 0 && deck.pos > stutterBase + stepLen * 0.5f) gTarget = 0;
        }
        if (!(fx == FX_GRAIN || fx == FX_SWARM) || deck.scratch) v = wordAt(deck.word, deck.pos) * 1.3f;
      }
      if (fx == FX_GRAIN || fx == FX_SWARM) v += grainSum() * (fx == FX_SWARM ? 0.55f : 0.8f);   // tails ring on
      gateSm += (gTarget - gateSm) * 0.25f;
      v *= gateSm;
      deckLP += 0.6f * (v - deckLP);                          // take the 8-bit fizz off
      v = deckLP;

      // ---- voice FX ----
      switch (fx) {
        case FX_CRUSH: if (++crushCnt >= 4) { crushCnt = 0; crushHold = roundf(v * 6) / 6; } v = crushHold; break;
        default: break;
      }
      vBuf[n] = v;
      dBuf[n] = kick.s(tapeRate) * 0.9f + snare.s() * 0.45f + hat.s() * 0.5f + clap.s() * 0.5f;
    }

    // ---- vocoder on the whole block ----
    if (fx == FX_VOX || fx == FX_CHOIR) vocodeBlock(fx == FX_VOX);

    // ---- pass 2: echo + mix ----
    float blockPeak = 0;
    for (int n = 0; n < BLOCK; n++) {
      float v = vBuf[n];
      float echo = 0;
      if (dlyBuf) {
        int dl = (int)(stepLen * 3);
        if (dl >= DLY_N) dl = DLY_N - 1;
        int r = dlyW - dl; if (r < 0) r += DLY_N;
        echo = dlyBuf[r];
        dlyBuf[dlyW] = (fx == FX_ECHO ? v : (fx == FX_CHOIR ? v * 0.3f : 0)) + echo * 0.5f;
        if (++dlyW >= DLY_N) dlyW = 0;
      }

      float x = v * 0.9f + echo * 0.6f + dBuf[n];
      underLP += (1.0f - under * 0.95f) * (x - underLP);
      float y = softclip(underLP * 1.2f);
      scopeBuf[n] = y;
      if (fabsf(y) > blockPeak) blockPeak = fabsf(y);
      int16_t s = (int16_t)(y * master * 32000);
      out[2 * n] = s; out[2 * n + 1] = s;
    }
    outPeak = blockPeak;
    deckVis = deck.on ? deck.pos : -1;
    for (int i = 0; i < 10; i++) grainVis[i] = grains[i].on && grains[i].word == deck.word ? grains[i].pos : -1;
    amp.write((uint8_t*)out, sizeof(out));
  }
}

// =====================================================================
// Visuals: sci-fi HUD - SIG scope, DECK waveform, word readout, SEQ lanes, status chips
// =====================================================================
float envStrip[60];
int envWord = -2;

void buildEnvStrip(int w) {
  envWord = w;
  for (int k = 0; k < 60; k++) envStrip[k] = 0;
  if (w < 0 || bank[w].len == 0) return;
  uint32_t len = bank[w].len;
  for (int k = 0; k < 60; k++) {
    uint32_t a = len * k / 60, b = len * (k + 1) / 60;
    int pk = 0;
    for (uint32_t i = a; i < b; i += 4) pk = max(pk, abs((int)pool[bank[w].off + i]));
    envStrip[k] = pk / 128.0f;
  }
}

// corner brackets instead of a full frame
void brackets(int x, int y, int w, int h) {
  const int l = 5;
  oled.drawHLine(x, y, l);             oled.drawVLine(x, y, l);
  oled.drawHLine(x + w - l, y, l);     oled.drawVLine(x + w - 1, y, l);
  oled.drawHLine(x, y + h - 1, l);     oled.drawVLine(x, y + h - l, l);
  oled.drawHLine(x + w - l, y + h - 1, l); oled.drawVLine(x + w - 1, y + h - l, l);
}

// small inverted label tab
int tab(int x, int y, const char* t) {
  oled.setFont(u8g2_font_4x6_tf);
  int w = oled.getStrWidth(t) + 4;
  oled.drawBox(x, y, w, 7);
  oled.setDrawColor(0); oled.drawStr(x + 2, y + 6, t); oled.setDrawColor(1);
  return w;
}

void dottedH(int x, int y, int w) { for (int i = 0; i < w; i += 2) oled.drawPixel(x + i, y); }

void drawTopBar() {
  int w = tab(0, 0, "ROBO//DJ");
  oled.setFont(u8g2_font_4x6_tf);
  char t[12]; snprintf(t, sizeof t, "%dBPM", TEMPOS[tempoIdx]);
  oled.drawStr(w + 4, 6, t);
  int bx = w + 6 + oled.getStrWidth(t);
  int beat = curStep / 4;
  for (int i = 0; i < 4; i++) {                    // beat lights
    if (i == beat) oled.drawBox(bx + i * 5, 1, 4, 4); else oled.drawFrame(bx + i * 5, 1, 4, 4);
  }
  const char* vn = VOICES[voiceIdx].name;
  oled.drawStr(128 - oled.getStrWidth(vn), 6, vn);
  dottedH(0, 8, 128);
}

void drawSigPane() {                               // live output scope + level meter
  const int x = 0, y = 11, w = 62, h = 40, mid = y + 22;
  brackets(x, y, w, h);
  tab(x + 2, y + 2, "SIG");
  for (int i = 0; i < 52; i += 4) oled.drawPixel(x + 4 + i, mid);
  int py = mid;
  for (int i = 0; i < 52; i++) {
    float v = scopeBuf[i * 128 / 52];
    int yy = mid - (int)(v * 14);
    yy = constrain(yy, y + 9, y + h - 4);
    if (i) oled.drawLine(x + 3 + i, py, x + 4 + i, yy);
    py = yy;
  }
  // level meter: segmented, right edge of the pane
  static float lvl = 0;
  lvl = max((float)outPeak, lvl * 0.9f);
  int segs = (int)(lvl * 8 + 0.5f);
  for (int i = 0; i < 8; i++) {
    int sy = y + h - 5 - i * 4;
    if (i < segs) oled.drawBox(x + w - 5, sy, 3, 3); else oled.drawPixel(x + w - 4, sy + 1);
  }
}

void drawDeckPane() {                              // word waveform strip + playhead + grains
  const int x = 65, y = 11, w = 63, h = 40, mid = y + 23;
  brackets(x, y, w, h);
  tab(x + 2, y + 2, "DECK");
  int wd = deck.on ? deck.word : curWord;
  if (wd != envWord) buildEnvStrip(wd);
  for (int k = 0; k < 60; k++) {
    int a = (int)(envStrip[k] * 12);
    if (a) oled.drawVLine(x + 2 + k, mid - a, a * 2 + 1); else oled.drawPixel(x + 2 + k, mid);
  }
  float pos = deckVis;
  if (pos >= 0 && wd >= 0 && bank[wd].len) {
    int px = x + 2 + (int)(pos / bank[wd].len * 59);
    oled.setDrawColor(2); oled.drawVLine(px, y + 9, h - 11); oled.setDrawColor(1);   // XOR playhead
    oled.drawTriangle(px - 2, y + h - 1, px + 2, y + h - 1, px, y + h - 4);
  }
  if (wd >= 0 && bank[wd].len) for (int i = 0; i < 10; i++) {
    float gp = grainVis[i];
    if (gp < 0) continue;
    oled.drawPixel(x + 2 + (int)(gp / bank[wd].len * 59), y + 10 + (i % 3) * 2);
  }
  oled.setFont(u8g2_font_4x6_tf);
  const char* mode = deck.scratch ? "SCRATCH" : (deck.on ? ((fx == FX_GRAIN || fx == FX_SWARM) ? "GRAIN" : "PLAY") : "IDLE");
  oled.drawStr(x + w - 2 - oled.getStrWidth(mode), y + 8, mode);
}

void drawReadout() {                               // [ WORD ]_ + FX / scratch / chord line
  oled.setFont(u8g2_font_6x10_tf);
  char wq[28]; snprintf(wq, sizeof wq, "[ %s ]", bank[curWord].text);
  int ww = oled.getStrWidth(wq), wx = 64 - ww / 2;
  oled.drawStr(wx, 63, wq);
  if ((millis() / 400) & 1) oled.drawBox(wx + ww + 2, 55, 4, 8);   // blinking cursor
  oled.setFont(u8g2_font_4x6_tf);
  char ch[12] = "--";
  if (fx == FX_VOX || fx == FX_CHOIR) chordLabel(ch, sizeof ch);
  char info[40]; snprintf(info, sizeof info, "FX:%s SCR:%s CH:%s", FX_NAMES[fx], SC_NAMES[scStyle], ch);
  oled.drawStr(64 - oled.getStrWidth(info) / 2, 71, info);
}

void drawSeqPane() {                               // 16 steps, VOX + DRM lanes, XOR playhead column
  const int y = 75, h = 42;
  brackets(0, y, 128, h);
  int tw = tab(2, y + 2, "SEQ");
  oled.setFont(u8g2_font_4x6_tf);
  char sc[12]; snprintf(sc, sizeof sc, "0x%02X", curStep);
  oled.drawStr(2 + tw + 3, y + 8, sc);
  oled.drawStr(100, y + 8, "VOX/DRM");
  const int lv = y + 12, ld = y + 26;              // lane tops
  for (int s = 0; s < 16; s++) {
    int x = 4 + s * 7 + (s / 4);                   // small gap every beat
    const Step& st = seq[s];
    if (st.word >= 0) {
      if (st.flags & SF_SCRATCH) { oled.drawLine(x, lv + 9, x + 2, lv + 1); oled.drawLine(x + 2, lv + 1, x + 5, lv + 9); }
      else if (st.flags & SF_REV) oled.drawFrame(x, lv, 6, 10);
      else oled.drawBox(x, lv, 6, 10);
    } else { oled.drawPixel(x + 2, lv + 5); }
    uint8_t d = st.drum;
    if (groove) { if (s == 0 || s == 8 || s == 10) d |= DR_KICK; if (s == 4 || s == 12) d |= DR_SNARE; if ((s & 1) == 0) d |= DR_HAT; }
    if (d & DR_KICK) oled.drawBox(x, ld + 2, 6, 6);
    else if (d & (DR_SNARE | DR_CLAP)) oled.drawFrame(x, ld + 2, 6, 6);
    if (d & DR_HAT) oled.drawPixel(x + 2, ld + 10);
    if (s % 4 == 0) oled.drawVLine(x - 1, ld + 12, 2);
    if (s == curStep) { oled.setDrawColor(2); oled.drawBox(x - 1, lv - 2, 8, ld + 14 - lv); oled.setDrawColor(1); }
  }
}

void drawChips() {                                 // status chips along the bottom
  struct { const char* t; bool on; } chips[6] = {
    {"REC", rec}, {"GRV", groove}, {"STUT", stutterOn}, {"REV", reverseOn}, {"TAPE", tapeOn}, {"UNDR", underOn}};
  oled.setFont(u8g2_font_4x6_tf);
  for (int i = 0; i < 6; i++) {
    int x = i * 21 + 1, w = 20;
    if (chips[i].on && !(i == 0 && ((millis() / 300) & 1))) {   // REC blinks
      oled.drawBox(x, 120, w, 8);
      oled.setDrawColor(0); oled.drawStr(x + (w - oled.getStrWidth(chips[i].t)) / 2, 126, chips[i].t); oled.setDrawColor(1);
    } else {
      dottedH(x, 120, w); dottedH(x, 127, w);
      oled.drawStr(x + (w - oled.getStrWidth(chips[i].t)) / 2, 126, chips[i].t);
    }
  }
}

void drawAlert(const char* m) {
  oled.setFont(u8g2_font_6x10_tf);
  char t[32]; snprintf(t, sizeof t, ">> %s", m);
  int w = oled.getStrWidth(t) + 10, x = 64 - w / 2;
  oled.setDrawColor(0); oled.drawBox(x - 2, 33, w + 4, 20); oled.setDrawColor(1);
  oled.drawFrame(x - 2, 33, w + 4, 20); oled.drawFrame(x, 35, w, 16);
  oled.drawStr(x + 5, 46, t);
}

bool anyOff = false;
void drawFrame() {
  oled.clearBuffer();
  if (bankBusy) {                                  // boot / voice change
    tab(0, 0, "ROBO//DJ");
    oled.setFont(u8g2_font_4x6_tf);
    oled.drawStr(0, 30, "> VOICE CORE");
    char v[24]; snprintf(v, sizeof v, "> LOADING %s", VOICES[voiceIdx].name);
    oled.drawStr(0, 40, v);
    char wtxt[32]; snprintf(wtxt, sizeof wtxt, "> %s", bank[min(renderProgress, NWORDS - 1)].text);
    oled.drawStr(0, 50, wtxt);
    brackets(10, 60, 108, 14);
    int segs = 20 * renderProgress / NWORDS;
    for (int i = 0; i < 20; i++) { if (i < segs) oled.drawBox(13 + i * 5, 63, 4, 8); else oled.drawPixel(15 + i * 5, 67); }
    char pc[8]; snprintf(pc, sizeof pc, "%d%%", 100 * renderProgress / NWORDS);
    oled.drawStr(64 - oled.getStrWidth(pc) / 2, 84, pc);
    oled.sendBuffer();
    return;
  }
  drawTopBar();
  drawSigPane();
  drawDeckPane();
  drawReadout();
  drawSeqPane();
  drawChips();
  if (millis() - msgAt < 1000) drawAlert(msg);
  if (anyOff && millis() < 6000) {
    char o[32]; snprintf(o, sizeof o, "OFF:%s%s%s%s", bS.off ? " UP" : "", bV.off ? " LEFT" : "", bB.off ? " RIGHT" : "", bF.off ? " DOWN" : "");
    drawAlert(o);
  }
  oled.sendBuffer();
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Btn* all[4] = {&bS, &bV, &bB, &bF};
  for (auto b : all) pinMode(b->pin, INPUT_PULLUP);
  for (int i = 0; i <= 1024; i++) sineTab[i] = sinf(2.0f * PI * i / 1024.0f);
  rgbLedWrite(PIN_RGB, 0, 0, 0);

  delay(50);                                   // a button stuck "pressed" at power-up is ignored
  int low[4] = {0};
  for (int i = 0; i < 20; i++) { for (int k = 0; k < 4; k++) low[k] += digitalRead(all[k]->pin) == LOW; delay(5); }
  for (int k = 0; k < 4; k++) { all[k]->off = low[k] > 15; anyOff |= all[k]->off; }

  oled.setBusClock(400000);
  oled.begin();
  oled.setContrast(120);
  prefs.begin("handheld", true);
  uint8_t rot = prefs.getUChar("orient", 3) & 3;
  prefs.end();
  const u8g2_cb_t* ROT[4] = {U8G2_R0, U8G2_R1, U8G2_R2, U8G2_R3};
  oled.setDisplayRotation(ROT[rot]);

  // sample pool: PSRAM if the build has it, otherwise what internal RAM can spare
  if (psramFound()) { poolSize = 1024 * 1024; pool = (int8_t*)ps_malloc(poolSize); dlyBuf = (float*)ps_calloc(DLY_N, sizeof(float)); }
  if (!pool) { poolSize = 150 * 1024; pool = (int8_t*)malloc(poolSize); }
  if (!dlyBuf) dlyBuf = (float*)calloc(DLY_N, sizeof(float));
  Serial.printf("robo dj: PSRAM %s, pool %u KB, free heap %u\n", psramFound() ? "yes" : "no", poolSize / 1024, ESP.getFreeHeap());

  nstate = esp_random() | 1;
  randomSeed(esp_random());
  clearAll();
  // a little demo loop so it does something straight away
  seq[0].word = 1; seq[6].word = 1; seq[6].flags = SF_SCRATCH; seq[8].word = 4; seq[12].word = 2;

  setupVocoder();
  vocChord(0);
  setupAmp();
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 10, nullptr, 0);
  drawFrame();
  renderBank(voiceIdx);
  curWord = bank[1].len ? 1 : nextWord(0);
  deckPlay(0, false);                          // "HELLO"
}

void loop() {
  static int renderedVoice = 0;
  if (voiceIdx != renderedVoice) {
    renderedVoice = voiceIdx;
    renderBank(renderedVoice);
    flash(VOICES[renderedVoice].name);
    deckPlay(curWord, false);
  }
  drawFrame();
  delay(5);
}
