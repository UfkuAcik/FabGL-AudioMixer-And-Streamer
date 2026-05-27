/*
  ============================================================
  FabGL Sound Mixer v2
  For OLIMEX ESP32-SBC-FabGL Board
  ============================================================
  Features:
    - 4 mixer channels (Sine/Square/Triangle/Sawtooth/Noise + 15 Drum/Synths)
    - SD card WAV player (8-bit mono, Resamples to 22050Hz)
    - WiFi audio streaming receiver (16-bit PCM, TCP port 8266)
    - 5 GUI themes (Classic Blue, Matrix, Amber Retro, Dark, Cyberpunk)
    - PS/2 keyboard and mouse control
    - VGA output at 400x300
    - NEW: Pause/Resume, 19 DSP SFX, 5-Band EQ, VU Meter, Resamplers
*/

#define private public
#include "fabgl.h"
#include "fabui.h"
#undef private
#include <WiFi.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <math.h>

using namespace fabgl;

// ── Pin Config ───────────────────────────────────────────────
#define SD_CS     13
#define SD_MISO   35
#define SD_MOSI   12
#define SD_CLK    14
#define STREAM_PORT 8266
#define CONTROL_PORT 8267

// ── Audio Config ─────────────────────────────────────────────
#define WAV_BUF_SIZE   32768 // Increased buffer for 2x speed stability
#define WAV_PLAY_FREQ  22050  // Enhanced sample rate (22050 Hz)

// ── Globals ──────────────────────────────────────────────────
fabgl::VGA16Controller  DisplayController;
fabgl::PS2Controller    kbdController;
SoundGenerator          soundGenerator(WAV_PLAY_FREQ); // Explicit sample rate
Preferences             prefs;
static volatile int     g_masterVol = 127;
static volatile bool    g_masterEnable = true;
static volatile int     g_vuPct = 0;
static fabgl::uiLabel*  g_title = nullptr;
static fabgl::uiLabel*  m_sig = nullptr;

struct WindowHack : public fabgl::uiWindow {
    static void moveTop(fabgl::uiWindow* parent, fabgl::uiWindow* child) {
        ((WindowHack*)parent)->moveChildOnTop(child);
    }
};

class ChannelFrame;
static ChannelFrame* g_chFrames[4] = {nullptr};
class MasterFrame;
static MasterFrame* g_masterFrame = nullptr;
class SDPlayerFrame;
static SDPlayerFrame* g_sdFrame = nullptr;

// ══════════════════════════════════════════════════════════════
//  Theme System
// ══════════════════════════════════════════════════════════════
struct Theme {
  const char * name;
  RGB888 bg, titleBg, titleBgActive, titleText, titleTextActive;
  RGB888 text, accent, frameBg, btnBg;
};

#define C_BLACK       RGB888(0,0,0)
#define C_BLUE        RGB888(0,0,170)
#define C_GREEN       RGB888(0,170,0)
#define C_CYAN        RGB888(0,170,170)
#define C_RED         RGB888(170,0,0)
#define C_MAGENTA     RGB888(170,0,170)
#define C_BROWN       RGB888(170,85,0)
#define C_LIGHTGRAY   RGB888(170,170,170)
#define C_DARKGRAY    RGB888(85,85,85)
#define C_BRIGHTBLUE  RGB888(85,85,255)
#define C_BRIGHTGREEN RGB888(85,255,85)
#define C_BRIGHTCYAN  RGB888(85,255,255)
#define C_BRIGHTRED   RGB888(255,85,85)
#define C_BRIGHTMAG   RGB888(255,85,255)
#define C_BRIGHTYEL   RGB888(255,255,85)
#define C_WHITE       RGB888(255,255,255)

static const Theme THEMES[] = {
  { "Matrix",       C_BLACK, C_GREEN, C_BRIGHTGREEN, C_BLACK, C_BLACK, C_BRIGHTGREEN, C_BRIGHTGREEN, C_BLACK, C_GREEN },
  { "Classic Blue", C_DARKGRAY, C_BLUE, C_BRIGHTBLUE, C_LIGHTGRAY, C_WHITE, C_WHITE, C_BRIGHTCYAN, C_BLUE, C_CYAN },
  { "Amber Retro",  C_BLACK, C_BROWN, C_BRIGHTYEL, C_WHITE, C_BLACK, C_BRIGHTYEL, C_BRIGHTYEL, C_BLACK, C_BROWN },
  { "Dark",         C_BLACK, C_DARKGRAY, C_LIGHTGRAY, C_WHITE, C_BLACK, C_WHITE, C_LIGHTGRAY, C_DARKGRAY, C_LIGHTGRAY },
  { "Cyberpunk",    C_BLACK, C_MAGENTA, C_BRIGHTMAG, C_WHITE, C_WHITE, C_BRIGHTMAG, C_BRIGHTMAG, C_BLACK, C_MAGENTA },
};
#define THEME_COUNT 5
static int g_themeIdx = 0;

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// ══════════════════════════════════════════════════════════════
//  WAV Header
// ══════════════════════════════════════════════════════════════
#pragma pack(push, 1)
struct WAVHeader {
  char riffTag[4]; uint32_t fileSize;
  char waveTag[4]; char fmtTag[4]; uint32_t fmtChunkSize;
  uint16_t audioFormat, numChannels;
  uint32_t sampleRate, byteRate;
  uint16_t blockAlign, bitsPerSample;
};
#pragma pack(pop)

class RingBufferGenerator : public fabgl::WaveformGenerator {
public:
  int8_t* m_buf;
  int m_size;
  volatile int m_head; 
  volatile int m_tail; 
  int m_curVol;
  
  volatile int m_rms;
  int m_rmsAcc;
  int m_rmsCount;

  RingBufferGenerator(int size) : m_size(size), m_head(0), m_tail(0), m_curVol(0), m_rms(0), m_rmsAcc(0), m_rmsCount(0) {
    m_buf = (int8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!m_buf) m_buf = (int8_t*)malloc(size); // fallback
  }
  ~RingBufferGenerator() {
    if (m_buf) free(m_buf);
  }

  void setFrequency(int value) override {}
  
  int getSample() override {
    // Volume Ramping (Pop-free)
    if (m_head == m_tail) {
      if (m_curVol > 0) m_curVol--;
    } else {
      if (m_curVol < volume()) m_curVol++;
      else if (m_curVol > volume()) m_curVol--;
    }

    int sample = 0;
    if (m_head != m_tail) {
      sample = m_buf[m_tail];
      m_tail = (m_tail + 1) & (m_size - 1); // Fast bitwise wrapping
    }

    int finalSample = (sample * m_curVol) / 127;

    // VU Meter calculation (Integer math only for ISR!)
    m_rmsAcc += finalSample * finalSample;
    m_rmsCount++;
    if (m_rmsCount >= 500) {
      // Fast integer square root approximation
      uint32_t val = m_rmsAcc / m_rmsCount;
      uint32_t raw = 0;
      uint32_t bit = 1UL << 30;
      while (bit > val) bit >>= 2;
      while (bit != 0) {
        if (val >= raw + bit) { val -= raw + bit; raw = (raw >> 1) + bit; }
        else { raw >>= 1; }
        bit >>= 2;
      }
      
      static int vu_smooth = 0;
      int iraw = (int)raw;
      if (iraw > vu_smooth) vu_smooth += (iraw - vu_smooth) / 2; // fast attack
      else                  vu_smooth += (iraw - vu_smooth) / 16; // slow release
      m_rms = vu_smooth;
      m_rmsAcc = 0; m_rmsCount = 0;
    }

    return finalSample;
  }

  int availableForWrite() volatile {
    int h = m_head, t = m_tail;
    if (h >= t) return m_size - 1 - (h - t);
    return t - h - 1;
  }

  int availableForRead() volatile {
    int h = m_head, t = m_tail;
    if (h >= t) return h - t;
    return m_size - (t - h);
  }

  void write(const int8_t* data, int len) {
    int avail = availableForWrite();
    if (len > avail) len = avail;
    if (len <= 0) return;
    int h = m_head;
    int toEnd = m_size - h;
    if (len <= toEnd) {
        memcpy(m_buf + h, data, len);
    } else {
        memcpy(m_buf + h, data, toEnd);
        memcpy(m_buf, data + toEnd, len - toEnd);
    }
    m_head = (h + len) % m_size;
  }
};

// ══════════════════════════════════════════════════════════════
//  Globals
// ══════════════════════════════════════════════════════════════
static volatile int         g_boostVal    = 1;
static int                  g_wavVol      = 127;

struct AppState {
  int masterVol; int masterEn;
  int boost;
  int eq[5];
  int fxType; int fxEn;
  int fxType2; int fxEn2;
  int chEn[4]; int chVol[4]; int chFreq[4]; int chType[4]; int chTempo[4];
  int           mediaSource;
  int           mediaState;
  int           mediaProg;
  int           mediaSelectReq;
  int           overEn;
  int           speed;
  int           agcEn;
};
volatile AppState g_state = { 127, 1, 1, {100,100,100,100,100}, 0, 0, 0, 0, {0,0,0,0}, {100,100,100,100}, {200,200,200,200}, {0,1,2,3}, {0,0,0,0}, 0, 0, 0, -1, 0, 100, 0 };
static bool g_ui_update_req = false;

class SystemFrame;
SystemFrame* g_sysFrame = nullptr;


// --- DSP State Variables ---
static float g_eqLow = 1.0f;
static float g_eqLowMid = 1.0f;
static float g_eqMid = 1.0f;
static float g_eqHighMid = 1.0f;
static float g_eqHigh = 1.0f;
static int   g_fxType = 0; 
static bool  g_fxEnable = false;
static int   g_fxType2 = 0;
static bool  g_fxEnable2 = false;
static bool  g_overEnable = false;
static bool  g_agcEnable = false; // AGC / Volume Fix

#define FX_BUF_SIZE 4096 // Power of 2 for bitmask optimization
static int8_t g_fxBuf[FX_BUF_SIZE];
static int g_fxPtr = 0;
static int8_t g_fxBuf2[FX_BUF_SIZE];
static int g_fxPtr2 = 0;

static float g_lp1 = 0.0f;
static float g_lp2 = 0.0f;
static float g_lp3 = 0.0f;
static float g_lp4 = 0.0f;

const float INV_WAV_FREQ = 1.0f / (float)WAV_PLAY_FREQ;

// ── Fast Math & Optimization ──────────────────────────────
static inline float IRAM_ATTR fast_sin(float x) {
    const float FAST_TWO_PI = 6.28318531f;
    const float INV_TWO_PI = 0.15915494f;
    x = x - FAST_TWO_PI * floorf(x * INV_TWO_PI + 0.5f);
    const float B = 1.27323954f;
    const float C = -0.40528473f;
    float y = B * x + C * x * fabsf(x);
    return 0.225f * (y * fabsf(y) - y) + y; // Smoothed parabolic
}
static inline float fast_cos(float x) {
    return fast_sin(x + 1.57079632f);
}

// ── Biquad IIR Filter ──────────────────────────────────────
struct Biquad {
    float a0, a1, a2, b1, b2;
    float z1, z2;
    void reset() { z1 = 0; z2 = 0; }
    inline float process(float in) {
        float out = in * a0 + z1;
        z1 = in * a1 + z2 - b1 * out;
        z2 = in * a2 - b2 * out;
        return out;
    }
    void setLowShelf(float Fs, float f0, float gainDB, float Q) {
        float A = powf(10.0f, gainDB / 40.0f);
        float w0 = 2.0f * 3.14159f * f0 / Fs;
        float alpha = fast_sin(w0) / (2.0f * Q);
        float cosw0 = fast_cos(w0);
        float a0_inv = 1.0f / ((A+1.0f) + (A-1.0f)*cosw0 + 2.0f*sqrtf(A)*alpha);
        a0 = A * ((A+1.0f) - (A-1.0f)*cosw0 + 2.0f*sqrtf(A)*alpha) * a0_inv;
        a1 = 2.0f * A * ((A-1.0f) - (A+1.0f)*cosw0) * a0_inv;
        a2 = A * ((A+1.0f) - (A-1.0f)*cosw0 - 2.0f*sqrtf(A)*alpha) * a0_inv;
        b1 = -2.0f * ((A-1.0f) + (A+1.0f)*cosw0) * a0_inv;
        b2 = ((A+1.0f) + (A-1.0f)*cosw0 - 2.0f*sqrtf(A)*alpha) * a0_inv;
    }
    void setHighShelf(float Fs, float f0, float gainDB, float Q) {
        float A = powf(10.0f, gainDB / 40.0f);
        float w0 = 2.0f * 3.14159f * f0 / Fs;
        float alpha = fast_sin(w0) / (2.0f * Q);
        float cosw0 = fast_cos(w0);
        float a0_inv = 1.0f / ((A+1.0f) - (A-1.0f)*cosw0 + 2.0f*sqrtf(A)*alpha);
        a0 = A * ((A+1.0f) + (A-1.0f)*cosw0 + 2.0f*sqrtf(A)*alpha) * a0_inv;
        a1 = -2.0f * A * ((A-1.0f) + (A+1.0f)*cosw0) * a0_inv;
        a2 = A * ((A+1.0f) + (A-1.0f)*cosw0 - 2.0f*sqrtf(A)*alpha) * a0_inv;
        b1 = 2.0f * ((A-1.0f) - (A+1.0f)*cosw0) * a0_inv;
        b2 = ((A+1.0f) - (A-1.0f)*cosw0 - 2.0f*sqrtf(A)*alpha) * a0_inv;
    }
    void setPeaking(float Fs, float f0, float gainDB, float Q) {
        float A = powf(10.0f, gainDB / 40.0f);
        float w0 = 2.0f * 3.14159f * f0 / Fs;
        float alpha = fast_sin(w0) / (2.0f * Q);
        float cosw0 = fast_cos(w0);
        float a0_inv = 1.0f / (1.0f + alpha / A);
        a0 = (1.0f + alpha * A) * a0_inv;
        a1 = -2.0f * cosw0 * a0_inv;
        a2 = (1.0f - alpha * A) * a0_inv;
        b1 = a1; 
        b2 = (1.0f - alpha / A) * a0_inv;
    }
};

static Biquad g_bqLow, g_bqLowMid, g_bqMid, g_bqHighMid, g_bqHigh;

static void updateEQ() {
    float fs = (float)WAV_PLAY_FREQ;
    auto toDB = [](float linear) {
        if (linear <= 0.01f) return -40.0f;
        return 20.0f * log10f(linear);
    };
    g_bqLow.setLowShelf(fs, 100.0f, toDB(g_eqLow), 0.707f);
    g_bqLowMid.setPeaking(fs, 300.0f, toDB(g_eqLowMid), 1.0f);
    g_bqMid.setPeaking(fs, 1000.0f, toDB(g_eqMid), 1.0f);
    g_bqHighMid.setPeaking(fs, 3000.0f, toDB(g_eqHighMid), 1.0f);
    g_bqHigh.setHighShelf(fs, 8000.0f, toDB(g_eqHigh), 0.707f);
}

class BoosterWrapper : public fabgl::WaveformGenerator {
  fabgl::WaveformGenerator* m_source;
public:
  BoosterWrapper(fabgl::WaveformGenerator* source) : m_source(source) {
    setSampleRate(source->sampleRate());
  }
  void setFrequency(int value) override { m_source->setFrequency(value); }
  int getSample() override {
    int s = m_source->getSample();
    int boosted = s * g_boostVal;
    if (boosted > 127) return 127;
    if (boosted < -128) return -128;
    return boosted;
  }
};

static void IRAM_ATTR applyDSP(int8_t* buffer, int len) {
    if (g_eqLow == 1.0f && g_eqLowMid == 1.0f && g_eqMid == 1.0f && g_eqHighMid == 1.0f && g_eqHigh == 1.0f && (!g_fxEnable || g_fxType == 0) && (!g_fxEnable2 || g_fxType2 == 0) && !g_overEnable) return;
    static unsigned int m_fxPhase = 0;
    
    static float lp1_1=0, lp1_2=0;
    static float lp2_1=0, lp2_2=0;
    static float lp3_1=0, lp3_2=0;
    static float lp4_1=0, lp4_2=0;
    
    static float dc_in_prev = 0, dc_out_prev = 0;
    
    for (int i=0; i<len; i++) {
      float s = (float)buffer[i] + 1e-9f; // Anti-denormal noise
      
      // --- 5-Band EQ (Biquad IIR) ---
      if (g_eqLow != 1.0f || g_eqLowMid != 1.0f || g_eqMid != 1.0f || g_eqHighMid != 1.0f || g_eqHigh != 1.0f) {
        s = g_bqLow.process(s);
        s = g_bqLowMid.process(s);
        s = g_bqMid.process(s);
        s = g_bqHighMid.process(s);
        s = g_bqHigh.process(s);
      }
      
      // --- Independent Overdrive ---
      if (g_overEnable) {
          s = s * 4.0f;
          if (s > 120.0f) s = 120.0f + (s - 120.0f) * 0.1f;
          else if (s < -80.0f) s = -80.0f + (s + 80.0f) * 0.1f;
          s = s / (1.0f + abs(s)/120.0f);
      }
      
      // --- 20 Effects ---
      if (g_fxEnable && g_fxType > 0) {
        m_fxPhase++;
        // --- Studio Upgrade 1: Smooth Sine LFO ---
        static float lfo_phase = 0.0f;
        float lfo_freq = 1.0f;
        if      (g_fxType == 8)  lfo_freq = 1.1f; // Chorus
        else if (g_fxType == 9)  lfo_freq = 2.2f; // Flanger
        else if (g_fxType == 14) lfo_freq = 1.5f; // Vibrato
        else if (g_fxType == 10) lfo_freq = 2.8f; // Auto-Wah
        else if (g_fxType == 11) lfo_freq = 4.0f; // Ring Mod
        
        lfo_phase += lfo_freq * INV_WAV_FREQ;
        if (lfo_phase >= 1.0f) lfo_phase -= 1.0f;
        
        float lfo = fast_sin(lfo_phase * 6.28318531f) * 0.5f + 0.5f; // Smooth 0.0 to 1.0
        
        if (g_fxType == 1) { // Echo (with feedback)
          float delayed = (float)g_fxBuf[g_fxPtr];
          float wet = s * 0.6f + delayed * 0.45f;
          int clamped = (int)wet;
          g_fxBuf[g_fxPtr] = (int8_t)(clamped > 127 ? 127 : (clamped < -128 ? -128 : clamped));
          g_fxPtr = (g_fxPtr + 1) % FX_BUF_SIZE;
          s = wet;
        } else if (g_fxType == 2) { // Reverb (Freeverb Schroeder Topology)
          static float c1_lp=0, c2_lp=0, c3_lp=0, c4_lp=0;
          static int16_t cb1[1557], cb2[1613], cb3[1493], cb4[1373];
          static int16_t ap1[227], ap2[73];
          static int p1=0, p2=0, p3=0, p4=0, pa1=0, pa2=0;
          
          float out_c1 = (float)cb1[p1];
          c1_lp = out_c1 * 0.8f + c1_lp * 0.2f;
          float in_cb1 = s + c1_lp * 0.84f;
          cb1[p1] = (int16_t)(in_cb1 > 32767.0f ? 32767.0f : (in_cb1 < -32768.0f ? -32768.0f : in_cb1));
          if (++p1 >= 1557) p1 = 0;
          
          float out_c2 = (float)cb2[p2];
          c2_lp = out_c2 * 0.8f + c2_lp * 0.2f;
          float in_cb2 = s + c2_lp * 0.84f;
          cb2[p2] = (int16_t)(in_cb2 > 32767.0f ? 32767.0f : (in_cb2 < -32768.0f ? -32768.0f : in_cb2));
          if (++p2 >= 1613) p2 = 0;

          float out_c3 = (float)cb3[p3];
          c3_lp = out_c3 * 0.8f + c3_lp * 0.2f;
          float in_cb3 = s + c3_lp * 0.84f;
          cb3[p3] = (int16_t)(in_cb3 > 32767.0f ? 32767.0f : (in_cb3 < -32768.0f ? -32768.0f : in_cb3));
          if (++p3 >= 1493) p3 = 0;

          float out_c4 = (float)cb4[p4];
          c4_lp = out_c4 * 0.8f + c4_lp * 0.2f;
          float in_cb4 = s + c4_lp * 0.84f;
          cb4[p4] = (int16_t)(in_cb4 > 32767.0f ? 32767.0f : (in_cb4 < -32768.0f ? -32768.0f : in_cb4));
          if (++p4 >= 1373) p4 = 0;

          float rev_raw = (out_c1 + out_c2 + out_c3 + out_c4) * 0.25f;
          
          // Studio Upgrade 5: Reverb High-Frequency Damping (Warmth)
          static float rev_lp = 0.0f;
          rev_lp += 0.4f * (rev_raw - rev_lp);
          float rev = rev_lp;

          float out_a1 = (float)ap1[pa1];
          float v1 = rev + out_a1 * 0.5f;
          ap1[pa1] = (int16_t)(v1 > 32767.0f ? 32767.0f : (v1 < -32768.0f ? -32768.0f : v1));
          rev = out_a1 - rev;
          if (++pa1 >= 227) pa1 = 0;

          float out_a2 = (float)ap2[pa2];
          float v2 = rev + out_a2 * 0.5f;
          ap2[pa2] = (int16_t)(v2 > 32767.0f ? 32767.0f : (v2 < -32768.0f ? -32768.0f : v2));
          rev = out_a2 - rev;
          if (++pa2 >= 73) pa2 = 0;

          s = s * 0.6f + rev * 0.4f;
        } else if (g_fxType == 3) { // Distort (Tube Analog Saturation)
          s = s * 4.0f;
          float x = s * 0.015f; 
          if (x > 1.0f) x = 1.0f; else if (x < -1.0f) x = -1.0f;
          s = (x - (x * x * x) * 0.3333f) * 120.0f; // Cubic Soft-Clipping
        } else if (g_fxType == 4) { // Fuzz (Asymmetrical Hard Clipper)
          s = s * 12.0f;
          float x = s * 0.02f;
          if (x > 1.0f) x = 1.0f; else if (x < -1.0f) x = -1.0f;
          s = (x - (x * x * x) * 0.3333f) * 120.0f;
          if (s > 70.0f) s = 70.0f; // Hard asymmetrical clip for ripping fuzz tone
        } else if (g_fxType == 5) { // Tremolo
          static float osc_i = 0.0f, osc_q = 1.0f;
          float freq = 2.0f * 3.14159f * 5.0f * INV_WAV_FREQ;
          osc_i += freq * osc_q;
          osc_q -= freq * osc_i;
          if (m_fxPhase % WAV_PLAY_FREQ == 0) { float mag = sqrtf(osc_i*osc_i + osc_q*osc_q); osc_i/=mag; osc_q/=mag; }
          float sineLfo = 0.5f + 0.5f * osc_i;
          s = s * (0.2f + 0.8f * sineLfo);
        } else if (g_fxType == 6) { // Bitcrush
          static float lastS = 0;
          if (m_fxPhase % 3 == 0) lastS = (int)(s / 16.0f) * 16.0f;
          s = lastS;
        } else if (g_fxType == 7) { // Decimate
          static float lastS = 0;
          if (m_fxPhase % 6 == 0) lastS = s;
          s = lastS;
        } else if (g_fxType == 8) { // Chorus
          float delay = 500.0f + 100.0f * lfo;
          int d_int = (int)delay; float d_frac = delay - d_int;
          int i1 = (g_fxPtr + FX_BUF_SIZE - d_int) % FX_BUF_SIZE;
          int i2 = (i1 + FX_BUF_SIZE - 1) % FX_BUF_SIZE;
          float tap = (float)g_fxBuf[i1] * (1.0f - d_frac) + (float)g_fxBuf[i2] * d_frac;
          
          g_fxBuf[g_fxPtr] = (int8_t)s;
          g_fxPtr = (g_fxPtr + 1) % FX_BUF_SIZE;
          s = s * 0.6f + tap * 0.4f;
        } else if (g_fxType == 9) { // Flanger
          float delay = 20.0f + 15.0f * lfo;
          int d_int = (int)delay; float d_frac = delay - d_int;
          int i1 = (g_fxPtr + FX_BUF_SIZE - d_int) % FX_BUF_SIZE;
          int i2 = (i1 + FX_BUF_SIZE - 1) % FX_BUF_SIZE;
          float tap = (float)g_fxBuf[i1] * (1.0f - d_frac) + (float)g_fxBuf[i2] * d_frac;
          
          g_fxBuf[g_fxPtr] = (int8_t)(s * 0.8f + tap * 0.5f);
          g_fxPtr = (g_fxPtr + 1) % FX_BUF_SIZE;
          s = s * 0.5f + tap * 0.5f;
        } else if (g_fxType == 10) { // Auto-Wah (Analog 2-Pole Resonant Filter)
          static float wah_lp = 0, wah_bp = 0;
          float f = 0.03f + 0.25f * lfo;
          wah_lp += f * wah_bp;
          wah_bp += f * (s - wah_lp - 0.4f * wah_bp);
          float out = s * 0.3f + wah_bp * 2.5f;
          if (out > 110.0f) out = 110.0f + (out - 110.0f) * 0.1f;
          else if (out < -110.0f) out = -110.0f + (out + 110.0f) * 0.1f;
          s = out;
        } else if (g_fxType == 11) { // Ring Mod
          float ringLfo = (float)(m_fxPhase % 100) / 50.0f;
          if (ringLfo > 1.0f) ringLfo = 2.0f - ringLfo;
          s = s * (ringLfo * 2.0f - 1.0f);
        } else if (g_fxType == 12) { // Sub-Octave
          static int sign = 1; static float prevS = 0;
          static float sub_lp = 0;
          if (s >= 0 && prevS < 0) sign = -sign;
          prevS = s;
          sub_lp += 0.15f * ((sign * 40.0f) - sub_lp);
          s = s * 0.9f + sub_lp * 0.3f;
        } else if (g_fxType == 13) { // Octave Fuzz
          static float hpS = 0;
          hpS += 0.3f * (s - hpS);
          float t = s - hpS;
          float oct = abs(t) * 2.0f - 64.0f;
          float xf = oct / 100.0f;
          if (xf > 1.0f) xf = 1.0f; else if (xf < -1.0f) xf = -1.0f;
          s = s * 0.5f + (xf - 0.33f * xf * xf * xf) * 80.0f * 0.6f;
        } else if (g_fxType == 14) { // Vibrato
          float delay = 150.0f + 50.0f * lfo;
          int d_int = (int)delay; float d_frac = delay - d_int;
          int i1 = (g_fxPtr + FX_BUF_SIZE - d_int) % FX_BUF_SIZE;
          int i2 = (i1 + FX_BUF_SIZE - 1) % FX_BUF_SIZE;
          float tap = (float)g_fxBuf[i1] * (1.0f - d_frac) + (float)g_fxBuf[i2] * d_frac;
          
          g_fxBuf[g_fxPtr] = (int8_t)s;
          g_fxPtr = (g_fxPtr + 1) % FX_BUF_SIZE;
          s = tap;
        } else if (g_fxType == 15) { // Telephone
          static float tel_lp=0, tel_hp=0;
          tel_lp += 0.3f * (s - tel_lp);
          tel_hp += 0.1f * (tel_lp - tel_hp);
          s = tel_lp - tel_hp;
        } else if (g_fxType == 16) { // Slapback
          float tap = (float)g_fxBuf[(g_fxPtr + FX_BUF_SIZE - 1500) % FX_BUF_SIZE];
          g_fxBuf[g_fxPtr] = (int8_t)s;
          g_fxPtr = (g_fxPtr + 1) % FX_BUF_SIZE;
          s = s * 0.6f + tap * 0.4f;
        } else if (g_fxType == 17) { // Noise Gate
          static float env = 0;
          float rect = abs(s);
          static float smoothGain = 1.0f;
          float targetGain = 1.0f;
          
          if (rect > env) env += 0.1f * (rect - env);
          else env += 0.002f * (rect - env);
          
          if (env < 55.0f) {
             targetGain = (env - 30.0f) / 25.0f;
             if (targetGain < 0.0f) targetGain = 0.0f;
          }
          
          smoothGain += 0.005f * (targetGain - smoothGain);
          s *= smoothGain;
        } else if (g_fxType == 18) { // Phaser (Studio 4-Stage Analog Phase 90)
          static float ph_x[4] = {0}, ph_y[4] = {0};
          static float ph_lfo_phase = 0.0f;
          ph_lfo_phase += 0.8f * INV_WAV_FREQ; // 0.8 Hz slow swirl
          if (ph_lfo_phase >= 1.0f) ph_lfo_phase -= 1.0f;
          float ph_lfo = fast_sin(ph_lfo_phase * 6.28318531f);
          
          float c = 0.5f + 0.45f * ph_lfo; // Sweeping coefficient
          float st = s;
          // 4 cascaded all-pass filters
          for(int p=0; p<4; p++) {
              float y_ph = c * st + ph_x[p] - c * ph_y[p];
              ph_x[p] = st;
              ph_y[p] = y_ph;
              st = y_ph;
          }
          s = s * 0.5f + st * 0.6f; // Mix dry and wet
        } else if (g_fxType == 19) { // Robotic
          s = s * (m_fxPhase % 20 < 10 ? 1 : -1);
        } else if (g_fxType == 20) { // Pitch Shift (Dual Delay Cross-Fade)
          float shift_speed = 1.5f; // Pitch shift up amount
          static float p_phase1 = 0.0f, p_phase2 = 0.5f;
          p_phase1 += shift_speed * INV_WAV_FREQ;
          if (p_phase1 >= 1.0f) p_phase1 -= 1.0f;
          p_phase2 += shift_speed * INV_WAV_FREQ;
          if (p_phase2 >= 1.0f) p_phase2 -= 1.0f;
          
          int depth = 800; // max delay
          int d1 = (int)(p_phase1 * depth);
          int d2 = (int)(p_phase2 * depth);
          
          // Cross-fade envelopes (triangle windows)
          float env1 = 1.0f - abs(p_phase1 - 0.5f) * 2.0f;
          float env2 = 1.0f - abs(p_phase2 - 0.5f) * 2.0f;
          
          int r1 = (g_fxPtr + FX_BUF_SIZE - d1) % FX_BUF_SIZE;
          int r2 = (g_fxPtr + FX_BUF_SIZE - d2) % FX_BUF_SIZE;
          
          float out_p = (float)g_fxBuf[r1] * env1 + (float)g_fxBuf[r2] * env2;
          
          g_fxBuf[g_fxPtr] = (int8_t)(s > 127.0f ? 127.0f : (s < -128.0f ? -128.0f : s));
          g_fxPtr = (g_fxPtr + 1) % FX_BUF_SIZE;
          s = out_p;
        } else if (g_fxType == 21) { // Lo-Fi
          static float lofi_lp=0; lofi_lp += 0.15f*(s-lofi_lp);
          float st = 256.0f/8; s = floor(lofi_lp/st)*st;
          static int lf_c=0; static float lf_h=0;
          if(++lf_c>=3){lf_h=s;lf_c=0;} s=lf_h;
        } else if (g_fxType == 22) { // Tape Wow (Linear Interpolation)
          static float tw_phase = 0.0f;
          tw_phase += 0.3f * INV_WAV_FREQ;
          if (tw_phase >= 1.0f) tw_phase -= 1.0f;
          float wow = fast_sin(tw_phase * 6.28318531f);
          
          float dp = 200.0f + 150.0f * wow;
          int d_int = (int)dp; float d_frac = dp - d_int;
          int i1 = (g_fxPtr + FX_BUF_SIZE - d_int) % FX_BUF_SIZE;
          int i2 = (i1 + FX_BUF_SIZE - 1) % FX_BUF_SIZE;
          float tap = (float)g_fxBuf[i1] * (1.0f - d_frac) + (float)g_fxBuf[i2] * d_frac;
          
          g_fxBuf[g_fxPtr] = (int8_t)(s > 127.0f ? 127.0f : (s < -128.0f ? -128.0f : s));
          g_fxPtr = (g_fxPtr + 1) % FX_BUF_SIZE;
          s = tap;
        } else if (g_fxType == 23) { // Stutter
          if(m_fxPhase%4000<2000){int rp=(g_fxPtr-2000+(m_fxPhase%2000)+FX_BUF_SIZE)%FX_BUF_SIZE;s=(float)g_fxBuf[rp];}
          g_fxBuf[g_fxPtr]=(int8_t)(s>127?127:(s<-128?-128:s));
          g_fxPtr=(g_fxPtr+1)%FX_BUF_SIZE;
        } else if (g_fxType == 24) { // Reverse Echo
          int rp=(g_fxPtr+1)%FX_BUF_SIZE; float rd=(float)g_fxBuf[rp];
          g_fxBuf[g_fxPtr]=(int8_t)(s>127?127:(s<-128?-128:s));
          g_fxPtr=(g_fxPtr+1)%FX_BUF_SIZE; s=s*0.7f+rd*0.4f;
        } else if (g_fxType == 25) { // Sample & Hold
          static float sh_v=0; static int sh_c=0;
          if(++sh_c>=500){sh_v=s;sh_c=0;} s=sh_v;
        } else if (g_fxType == 26) { // Comb Filter
          int rp=(g_fxPtr-100+FX_BUF_SIZE)%FX_BUF_SIZE;
          float dl=(float)g_fxBuf[rp]; float o=s+dl*0.7f;
          int cl=(int)o; g_fxBuf[g_fxPtr]=(int8_t)(cl>127?127:(cl<-128?-128:cl));
          g_fxPtr=(g_fxPtr+1)%FX_BUF_SIZE; s=o*0.5f;
        } else if (g_fxType == 27) { // Formant
          static float f1l=0,f1h=0,f2l=0,f2h=0;
          f1l+=0.08f*(s-f1l);f1h+=0.04f*(f1l-f1h); float b1=f1l-f1h;
          f2l+=0.25f*(s-f2l);f2h+=0.12f*(f2l-f2h); float b2=f2l-f2h;
          s=b1*1.5f+b2*1.2f;
        } else if (g_fxType == 28) { // Shimmer
          static float shm=0; shm+=0.3f*(s-shm);
          float hi=s-shm; float oct=abs(hi)*2.0f-64.0f;
          int rp=(g_fxPtr-1500+FX_BUF_SIZE)%FX_BUF_SIZE; float tl=(float)g_fxBuf[rp];
          float mx=s*0.5f+oct*0.15f+tl*0.35f; int cl=(int)mx;
          g_fxBuf[g_fxPtr]=(int8_t)(cl>127?127:(cl<-128?-128:cl));
          g_fxPtr=(g_fxPtr+1)%FX_BUF_SIZE; s=mx;
        } else if (g_fxType == 29) { // Radio
          static float rl=0,rh=0; rl+=0.35f*(s-rl);rh+=0.04f*(rl-rh);
          s=(rl-rh)*1.8f; s=s/(1.0f+abs(s)/50.0f);
          static unsigned int rs=42; rs=1103515245*rs+12345;
          if((rs>>24)>250) s+=((rs>>16)&0xFF)-128;
        } else if (g_fxType == 30) { // Wah Fixed
          static float wb=0; wb+=0.15f*(s-wb); s=wb*2.5f;
          s=s/(1.0f+abs(s)/80.0f);
        }
      }

      // --- SFX Slot 2 (independent, uses g_fxBuf2/g_fxPtr2) ---
      if (g_fxEnable2 && g_fxType2 > 0) {
        static unsigned int m_fxPhase2 = 0;
        m_fxPhase2++;
        int lfo2_period = 2000;
        if (g_fxType2 == 8)       lfo2_period = 20000;
        else if (g_fxType2 == 9)  lfo2_period = 10000;
        else if (g_fxType2 == 14) lfo2_period = 15000;
        else if (g_fxType2 == 10) lfo2_period = 8000;
        float lfo2 = (float)(m_fxPhase2 % lfo2_period) / (lfo2_period / 2.0f);
        if (lfo2 > 1.0f) lfo2 = 2.0f - lfo2;
        
        if (g_fxType2 == 1) { // Echo
          float delayed = (float)g_fxBuf2[g_fxPtr2];
          float wet = s * 0.6f + delayed * 0.45f;
          int cl = (int)wet;
          g_fxBuf2[g_fxPtr2] = (int8_t)(cl > 127 ? 127 : (cl < -128 ? -128 : cl));
          g_fxPtr2 = (g_fxPtr2 + 1) % FX_BUF_SIZE;
          s = wet;
        } else if (g_fxType2 == 2) { // Reverb
          static float c1_lp2=0, c2_lp2=0, c3_lp2=0, c4_lp2=0;
          static int16_t cb1_2[1557], cb2_2[1613], cb3_2[1493], cb4_2[1373];
          static int16_t ap1_2[227], ap2_2[73];
          static int p1_2=0, p2_2=0, p3_2=0, p4_2=0, pa1_2=0, pa2_2=0;
          float oc1=(float)cb1_2[p1_2]; c1_lp2+=0.3f*(oc1-c1_lp2); float ic1=s+c1_lp2*0.84f;
          cb1_2[p1_2]=(int16_t)(ic1>32767?32767:(ic1<-32768?-32768:ic1)); if(++p1_2>=1557)p1_2=0;
          float oc2=(float)cb2_2[p2_2]; c2_lp2+=0.3f*(oc2-c2_lp2); float ic2=s+c2_lp2*0.84f;
          cb2_2[p2_2]=(int16_t)(ic2>32767?32767:(ic2<-32768?-32768:ic2)); if(++p2_2>=1613)p2_2=0;
          float oc3=(float)cb3_2[p3_2]; c3_lp2+=0.3f*(oc3-c3_lp2); float ic3=s+c3_lp2*0.84f;
          cb3_2[p3_2]=(int16_t)(ic3>32767?32767:(ic3<-32768?-32768:ic3)); if(++p3_2>=1493)p3_2=0;
          float oc4=(float)cb4_2[p4_2]; c4_lp2+=0.3f*(oc4-c4_lp2); float ic4=s+c4_lp2*0.84f;
          cb4_2[p4_2]=(int16_t)(ic4>32767?32767:(ic4<-32768?-32768:ic4)); if(++p4_2>=1373)p4_2=0;
          float rev2 = (oc1+oc2+oc3+oc4)*0.25f;
          float oa1=(float)ap1_2[pa1_2]; float v1_2=rev2+oa1*0.5f;
          ap1_2[pa1_2]=(int16_t)(v1_2>32767?32767:(v1_2<-32768?-32768:v1_2)); rev2=oa1-rev2; if(++pa1_2>=227)pa1_2=0;
          float oa2=(float)ap2_2[pa2_2]; float v2_2=rev2+oa2*0.5f;
          ap2_2[pa2_2]=(int16_t)(v2_2>32767?32767:(v2_2<-32768?-32768:v2_2)); rev2=oa2-rev2; if(++pa2_2>=73)pa2_2=0;
          s = s * 0.6f + rev2 * 0.4f;
        } else if (g_fxType2 == 5) { // Tremolo
          static float osc_i2=0, osc_q2=1;
          float fr2=2.0f*3.14159f*5.0f/(float)WAV_PLAY_FREQ;
          osc_i2+=fr2*osc_q2; osc_q2-=fr2*osc_i2;
          if(m_fxPhase2%WAV_PLAY_FREQ==0){float m=sqrtf(osc_i2*osc_i2+osc_q2*osc_q2);osc_i2/=m;osc_q2/=m;}
          s = s * (0.2f + 0.8f * (0.5f + 0.5f * osc_i2));
        } else if (g_fxType2 >= 3 && g_fxType2 <= 4) { // Dist/Fuzz
          float limit = (g_fxType2==3) ? 40.0f : 25.0f;
          s = s / (1.0f + abs(s)/limit);
        } else if (g_fxType2 == 6) { // Bitcrush
          int bits = 4; float step2 = 256.0f / (1 << bits); s = floor(s / step2) * step2;
        } else if (g_fxType2 == 7) { // Decimate
          static float hold2 = 0; static int cnt2 = 0;
          if (++cnt2 >= 4) { hold2 = s; cnt2 = 0; } s = hold2;
        } else if (g_fxType2 == 8 || g_fxType2 == 9 || g_fxType2 == 14) { // Chorus/Flanger/Vibrato
          int depth = (g_fxType2==8)?800:(g_fxType2==9)?200:600;
          int idx = (int)(lfo2 * depth);
          int rp = (g_fxPtr2 - idx + FX_BUF_SIZE) % FX_BUF_SIZE;
          float delayed = (float)g_fxBuf2[rp];
          g_fxBuf2[g_fxPtr2] = (int8_t)(s > 127 ? 127 : (s < -128 ? -128 : s));
          g_fxPtr2 = (g_fxPtr2 + 1) % FX_BUF_SIZE;
          if (g_fxType2 == 14) s = delayed; else s = s * 0.7f + delayed * 0.3f;
        } else if (g_fxType2 == 10) { // Auto-Wah
          static float wahLP2=0;
          float cutoff = 0.05f + 0.4f * lfo2;
          wahLP2 += cutoff * (s - wahLP2);
          s = wahLP2;
        } else if (g_fxType2 == 11) { // Ring Mod
          float rm2 = (m_fxPhase2 % 50 < 25) ? 1.0f : -1.0f;
          s *= rm2;
        } else if (g_fxType2 == 15) { // Telephone
          static float tlp2=0, thp2=0;
          tlp2 += 0.35f * (s - tlp2); thp2 += 0.04f * (tlp2 - thp2);
          s = (tlp2 - thp2) * 1.5f;
        } else if (g_fxType2 == 16) { // Slapback
          int sp = (g_fxPtr2 - 1500 + FX_BUF_SIZE) % FX_BUF_SIZE;
          float dl = (float)g_fxBuf2[sp];
          g_fxBuf2[g_fxPtr2] = (int8_t)(s > 127 ? 127 : (s < -128 ? -128 : s));
          g_fxPtr2 = (g_fxPtr2 + 1) % FX_BUF_SIZE;
          s = s * 0.8f + dl * 0.4f;
        } else if (g_fxType2 == 18) { // Phaser
          static float ph2_x=0, ph2_y=0, osc_i2p=0, osc_q2p=1;
          float fr2=2.0f*3.14159f*1.5f/(float)WAV_PLAY_FREQ;
          osc_i2p+=fr2*osc_q2p; osc_q2p-=fr2*osc_i2p;
          if(m_fxPhase2%WAV_PLAY_FREQ==0){float m=sqrtf(osc_i2p*osc_i2p+osc_q2p*osc_q2p);osc_i2p/=m;osc_q2p/=m;}
          float c2=0.5f+0.4f*osc_i2p;
          float yp=c2*s+ph2_x-c2*ph2_y; ph2_x=s; ph2_y=yp;
          s=s*0.5f+yp*0.5f;
        } else if (g_fxType2 == 19) { // Robotic
          s = s * (m_fxPhase2 % 20 < 10 ? 1 : -1);
        } else if (g_fxType2 == 20) { // Pitch Shift
          int dp2=(int)(lfo2*600); int rp=(g_fxPtr2-dp2+FX_BUF_SIZE)%FX_BUF_SIZE;
          g_fxBuf2[g_fxPtr2]=(int8_t)(s>127?127:(s<-128?-128:s));
          g_fxPtr2=(g_fxPtr2+1)%FX_BUF_SIZE; s=(float)g_fxBuf2[rp];
        } else if (g_fxType2 == 21) { // Lo-Fi
          static float lf2_l=0; lf2_l+=0.15f*(s-lf2_l); float st2=32; s=floor(lf2_l/st2)*st2;
          static int lf2c=0; static float lf2h=0; if(++lf2c>=3){lf2h=s;lf2c=0;} s=lf2h;
        } else if (g_fxType2 == 22) { // Tape Wow
          static unsigned int tp2=0; tp2++;
          int dp2=(int)(200.0f+150.0f*fast_sin(2.0f*3.14159f*0.3f*tp2/(float)WAV_PLAY_FREQ));
          int rp=(g_fxPtr2-dp2+FX_BUF_SIZE)%FX_BUF_SIZE;
          g_fxBuf2[g_fxPtr2]=(int8_t)(s>127?127:(s<-128?-128:s));
          g_fxPtr2=(g_fxPtr2+1)%FX_BUF_SIZE; s=(float)g_fxBuf2[rp];
        } else if (g_fxType2 == 23) { // Stutter
          if(m_fxPhase2%4000<2000){int rp=(g_fxPtr2-2000+(m_fxPhase2%2000)+FX_BUF_SIZE)%FX_BUF_SIZE;s=(float)g_fxBuf2[rp];}
          g_fxBuf2[g_fxPtr2]=(int8_t)(s>127?127:(s<-128?-128:s)); g_fxPtr2=(g_fxPtr2+1)%FX_BUF_SIZE;
        } else if (g_fxType2 == 24) { // Reverse Echo
          int rp=(g_fxPtr2+1)%FX_BUF_SIZE; float rd2=(float)g_fxBuf2[rp];
          g_fxBuf2[g_fxPtr2]=(int8_t)(s>127?127:(s<-128?-128:s)); g_fxPtr2=(g_fxPtr2+1)%FX_BUF_SIZE;
          s=s*0.7f+rd2*0.4f;
        } else if (g_fxType2 == 25) { // S&H
          static float sh2=0; static int shc2=0; if(++shc2>=500){sh2=s;shc2=0;} s=sh2;
        } else if (g_fxType2 == 26) { // Comb
          int rp=(g_fxPtr2-100+FX_BUF_SIZE)%FX_BUF_SIZE; float dl2=(float)g_fxBuf2[rp]; float o2=s+dl2*0.7f;
          int cl2=(int)o2; g_fxBuf2[g_fxPtr2]=(int8_t)(cl2>127?127:(cl2<-128?-128:cl2));
          g_fxPtr2=(g_fxPtr2+1)%FX_BUF_SIZE; s=o2*0.5f;
        } else if (g_fxType2 == 27) { // Formant
          static float f1l2=0,f1h2=0,f2l2=0,f2h2=0;
          f1l2+=0.08f*(s-f1l2);f1h2+=0.04f*(f1l2-f1h2);
          f2l2+=0.25f*(s-f2l2);f2h2+=0.12f*(f2l2-f2h2);
          s=(f1l2-f1h2)*1.5f+(f2l2-f2h2)*1.2f;
        } else if (g_fxType2 == 28) { // Shimmer
          static float shm2=0; shm2+=0.3f*(s-shm2); float oct2=abs(s-shm2)*2.0f-64.0f;
          int rp=(g_fxPtr2-1500+FX_BUF_SIZE)%FX_BUF_SIZE; float tl2=(float)g_fxBuf2[rp];
          float mx2=s*0.5f+oct2*0.15f+tl2*0.35f; int cl2=(int)mx2;
          g_fxBuf2[g_fxPtr2]=(int8_t)(cl2>127?127:(cl2<-128?-128:cl2)); g_fxPtr2=(g_fxPtr2+1)%FX_BUF_SIZE; s=mx2;
        } else if (g_fxType2 == 29) { // Radio
          static float rl2=0,rh2=0; rl2+=0.35f*(s-rl2);rh2+=0.04f*(rl2-rh2);
          s=(rl2-rh2)*1.8f; s=s/(1.0f+abs(s)/50.0f);
        } else if (g_fxType2 == 30) { // Wah Fixed
          static float wb2=0; wb2+=0.15f*(s-wb2); s=wb2*2.5f; s=s/(1.0f+abs(s)/80.0f);
        }
      }
      
      // DC Blocker
      float dc_out = s - dc_in_prev + 0.995f * dc_out_prev;
      dc_in_prev = s;
      dc_out_prev = dc_out;
      s = dc_out;
      
      // AGC / Volume Fixer (Stable Volume)
      if (g_agcEnable) {
          static float agc_gain = 1.0f;
          static float peak = 0.0f;
          float abs_s = fabsf(s);
          if (abs_s > peak) peak = abs_s; 
          else peak += 0.0001f * (abs_s - peak); // Slow envelope
          
          if (peak < 5.0f) {
              agc_gain += 0.0001f * (1.0f - agc_gain); // Return to unity if silent
          } else {
              float target = 64.0f; // Target peak amplitude
              agc_gain += 0.0001f * (target - peak); 
          }
          
          if (agc_gain > 3.0f) agc_gain = 3.0f; // Max +9dB boost
          if (agc_gain < 0.25f) agc_gain = 0.25f; // Max -12dB cut
          
          s = s * agc_gain;
      }

      // Soft output limiter
      if      (s >  110.0f) s =  110.0f + (s - 110.0f) * 0.15f;
      else if (s < -110.0f) s = -110.0f + (s + 110.0f) * 0.15f;
      
      if (s > 127.0f) s = 127.0f;
      else if (s < -128.0f) s = -128.0f;
      
      buffer[i] = (int8_t)s;
    }
  }
  static RingBufferGenerator* g_wavGen      = nullptr;
static BoosterWrapper*      g_boosterGen  = nullptr;
static volatile bool        g_wavPlaying  = false;
static volatile bool        g_wavStop     = false;
  static volatile float       g_playbackSpeed = 1.0f;
static volatile bool        g_wavPaused   = false;
static volatile int         g_wavProgress = 0;
static bool                 g_wavPlayReq  = false;
static volatile int         g_wavSeekPct  = -1;
static TaskHandle_t         g_wavTask     = nullptr;
static char                 g_wavPath[128];

static float g_resampAcc = 1.0f;
static float g_resampFilt = 0.0f;
static float g_resampPrev = 0.0f;

static WiFiServer*          g_tcpServer    = nullptr;
static volatile bool        g_streamActive = false;
static volatile bool        g_wifiReady    = false;
static TaskHandle_t         g_streamTask   = nullptr;
static RingBufferGenerator* g_streamGen    = nullptr;
static BoosterWrapper*      g_streamBooster= nullptr;

// ── WAV Helpers ──────────────────────────────────────────────
static bool wavParseHeader(FILE* f, WAVHeader* h, uint32_t* dataBytes) {
  if (fread(h, 1, sizeof(WAVHeader), f) != sizeof(WAVHeader)) return false;
  if (memcmp(h->riffTag,"RIFF",4) || memcmp(h->waveTag,"WAVE",4)) return false;
  if (h->audioFormat != 1) return false;
  if (h->fmtChunkSize > 16) fseek(f, h->fmtChunkSize - 16, SEEK_CUR);
  char tag[4]; uint32_t sz;
  while (fread(tag,1,4,f)==4) {
    if (fread(&sz,1,4,f)!=4) return false;
    if (!memcmp(tag,"data",4)) { *dataBytes=sz; return true; }
    fseek(f, sz, SEEK_CUR);
  }
  return false;
}

static int wavConvertChunk(FILE* f, const WAVHeader* h, int8_t* out, int outLen, uint32_t* rem, bool flush = false) {
  static uint8_t fileBuf[8192];
  static int bufAvail = 0;
  static int bufPtr = 0;
  if (flush) { bufAvail = 0; bufPtr = 0; return 0; }
  
  int written = 0;
  int bps = (h->bitsPerSample/8) * h->numChannels;
  float step = ((float)h->sampleRate * g_playbackSpeed) / WAV_PLAY_FREQ;
  
  while (written < outLen && *rem >= (uint32_t)bps) {
    if (g_resampAcc >= step) {
      float frac = 1.0f - (g_resampAcc - step) / step;
      if (frac < 0.0f) frac = 0.0f; else if (frac > 1.0f) frac = 1.0f;
      float interp = g_resampPrev + frac * (g_resampFilt - g_resampPrev);
      int v = (int)interp;
      out[written++] = (int8_t)(v > 127 ? 127 : (v < -128 ? -128 : v));
      g_resampAcc -= step;
      continue; 
    }
    
    if (bufAvail < bps) {
      if (bufAvail > 0 && bufPtr > 0) memmove(fileBuf, fileBuf + bufPtr, bufAvail);
      bufPtr = 0;
      uint32_t toRead = 8192 - bufAvail;
      if (toRead > *rem) toRead = *rem;
      int rd = fread(fileBuf + bufAvail, 1, toRead, f);
      if (rd <= 0) break;
      bufAvail += rd;
    }
    
    if (bufAvail < bps) break;
    
    uint8_t raw[4];
    for(int i=0; i<bps; i++) raw[i] = fileBuf[bufPtr++];
    bufAvail -= bps;
    *rem -= bps;
    
    float nextSample = 0;
    if (h->bitsPerSample == 16) {
      int16_t L = (int16_t)(raw[0]|(raw[1]<<8));
      int16_t R = (h->numChannels==2) ? (int16_t)(raw[2]|(raw[3]<<8)) : L;
      nextSample = (float)(((int32_t)L+R)/2 >> 8);
    } else {
      uint8_t L=raw[0], R=(h->numChannels==2)?raw[1]:L;
      nextSample = (float)(((int)L+R)/2 - 128);
    }
    
    g_resampPrev = g_resampFilt;
    g_resampFilt = nextSample;
    g_resampAcc += 1.0f;
  }
  return written;
}



// 🎧 Media Player Task 🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧🎧
static void wavPlayerTask(void*) {
  Serial.printf("[WAV] wavPlayerTask started, path=%s\n", g_wavPath);
  FILE* f = fopen(g_wavPath, "rb");
  if (!f) { Serial.printf("[WAV] fopen FAILED!\n"); g_wavPlaying=false; g_wavTask=nullptr; vTaskDelete(nullptr); return; }
  Serial.printf("[WAV] fopen OK\n");
  WAVHeader hdr; uint32_t dataBytes=0;
  if (!wavParseHeader(f,&hdr,&dataBytes)) {
    fclose(f); g_wavPlaying=false; g_wavTask=nullptr; vTaskDelete(nullptr); return;
  }
  uint32_t total=dataBytes, rem=dataBytes;
  uint32_t dataStartPos = ftell(f);
  g_wavPlaying=true; g_wavProgress=0;
  g_wavSeekPct = -1;
  g_state.mediaState = 1;
  g_ui_update_req = true;
  
  g_wavGen = new RingBufferGenerator(WAV_BUF_SIZE);
  g_boosterGen = new BoosterWrapper(g_wavGen);
  soundGenerator.attach(g_boosterGen); 
  g_wavGen->setVolume(g_wavVol); 
  g_wavGen->enable(true);
  g_boosterGen->enable(true);
  
  int8_t* chunkBuf = (int8_t*)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!chunkBuf) {
    chunkBuf = (int8_t*)malloc(8192); // Fallback
  }
  if (!chunkBuf) {
    if (g_boosterGen) { g_boosterGen->enable(false); soundGenerator.detach(g_boosterGen); delete g_boosterGen; g_boosterGen=nullptr; }
    g_wavGen->enable(false); delete g_wavGen; g_wavGen=nullptr;
    fclose(f); g_wavPlaying=false; g_wavTask=nullptr; vTaskDelete(nullptr); return;
  }

  while (!g_wavStop && rem>0) {
    if (g_wavSeekPct >= 0) {
       uint32_t jumpBytes = (uint32_t)(((float)g_wavSeekPct / 100.0f) * total);
       jumpBytes &= ~3; // Align to 4 bytes
       fseek(f, dataStartPos + jumpBytes, SEEK_SET);
       rem = total - jumpBytes;
       g_wavSeekPct = -1;
       g_wavGen->m_curVol = 0; // Anti-click: volume ramp will fade in
       g_wavGen->m_head = g_wavGen->m_tail = 0; // Flush buffer
         wavConvertChunk(f, &hdr, chunkBuf, 8192, &rem, true);
    }
    if (g_wavPaused) {
       vTaskDelay(pdMS_TO_TICKS(50));
       continue;
    }
    while (g_wavGen->availableForWrite() < 8192 && !g_wavStop && !g_wavPaused) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (g_wavStop) break;
    
    int n = wavConvertChunk(f, &hdr, chunkBuf, 8192, &rem, false);
    if (n <= 0) break;
    
    applyDSP(chunkBuf, n);
    
    g_wavGen->write(chunkBuf, n);
    g_wavProgress = (int)((float)(total-rem)/total*100);
  }
  
  free(chunkBuf);
  
  while(g_wavGen->availableForRead() > 0 && !g_wavStop) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  
  if (g_wavGen) { 
    if (g_boosterGen) {
       g_boosterGen->enable(false); soundGenerator.detach(g_boosterGen); delete g_boosterGen; g_boosterGen=nullptr;
    }
    g_wavGen->enable(false); 
    delete g_wavGen; 
    g_wavGen=nullptr; 
  }
  
  fclose(f); g_wavPlaying=false;
  g_wavProgress = g_wavStop ? 0 : 100;
  g_wavStop=false; g_wavTask=nullptr; vTaskDelete(nullptr);
}

static void wavPlay(const char* path) {
  Serial.printf("[WAV] wavPlay called, path=%s, g_wavPlaying=%d\n", path, g_wavPlaying);
  if (g_wavPlaying) {
    g_wavStop = true;
    Serial.printf("[WAV] Stopping previous playback...\n");
    while(g_wavPlaying) vTaskDelay(pdMS_TO_TICKS(10));
    Serial.printf("[WAV] Previous playback stopped.\n");
  }
  strncpy(g_wavPath, path, 127); g_wavPath[127]=0; 
  g_wavStop=false; g_wavPaused=false;
  if (g_boosterGen) g_boosterGen->enable(true);
  if (g_wavGen) g_wavGen->enable(true);
  g_resampAcc = 1.0f; g_resampFilt = 0.0f; g_resampPrev = 0.0f;
  Serial.printf("[WAV] Creating wavPlayerTask... freeHeap=%d\n", ESP.getFreeHeap());
  BaseType_t ret = xTaskCreatePinnedToCore(wavPlayerTask,"wavPlay",4096,nullptr,1,&g_wavTask,0);
  Serial.printf("[WAV] wavPlayerTask create ret=%d (1=OK), handle=%p\n", ret, g_wavTask);
}
static void wavStopFn() { g_wavStop = true; }
static void wavPauseFn() { 
  g_wavPaused = !g_wavPaused; g_state.mediaState = (g_wavPaused ? 2 : 1); 
  if (g_boosterGen) g_boosterGen->enable(!g_wavPaused);
  if (g_wavGen) g_wavGen->enable(!g_wavPaused);
}

// ── WiFi Control Task moved down ──────────────────────────────

// ── WiFi Stream Task ─────────────────────────────────────────
static void wifiStreamTask(void*) {
  g_tcpServer = new WiFiServer(STREAM_PORT);
  g_tcpServer->begin();
  while (true) {
    WiFiClient client = g_tcpServer->available();
    if (client) {
      g_streamActive = true;
      uint8_t hdr[12];
      int rd = 0;
      while (rd < 12 && client.connected()) {
        int n = client.read(hdr+rd, 12-rd);
        if (n > 0) rd += n; else vTaskDelay(1);
      }
      if (rd == 12 && hdr[0]=='S' && hdr[1]=='M' && hdr[2]=='I' && hdr[3]=='X') {
        uint32_t srcRate = *(uint32_t*)(hdr+4);
        uint32_t bitsIn  = *(uint32_t*)(hdr+8);
        
        g_streamGen = new RingBufferGenerator(WAV_BUF_SIZE);
        g_streamBooster = new BoosterWrapper(g_streamGen);
        soundGenerator.attach(g_streamBooster); 
        g_streamGen->setVolume(100); 
        g_streamGen->enable(true);
        g_streamBooster->enable(true);
        
        int8_t* chunkBuf = (int8_t*)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!chunkBuf) {
          chunkBuf = (int8_t*)malloc(8192); // Fallback
        }
        float step = (float)srcRate / WAV_PLAY_FREQ;
        float s_acc = 0.0f;
        
        while (client.connected() && g_streamActive) {
          int avail = client.available();
          if (avail <= 0) { vTaskDelay(5); continue; }
          
          while (g_streamGen->availableForWrite() < 8192 && g_streamActive && client.connected()) {
             vTaskDelay(pdMS_TO_TICKS(5));
          }
          if (!g_streamActive || !client.connected()) break;

          uint8_t tmpBuf[1024];
          int written = 0;
          while (written < 8192 && client.connected()) {
            int chunk = client.available();
            if (chunk <= 0) { vTaskDelay(2); continue; }
            if (chunk > (int)sizeof(tmpBuf)) chunk = sizeof(tmpBuf);
            if (written + chunk > 8192) chunk = 8192 - written;
            int nr = client.read(tmpBuf, chunk);
            
            // Push resampler for stream rate matching
            float step = ((float)srcRate * g_playbackSpeed) / WAV_PLAY_FREQ;
            for (int i = 0; i < nr; i += (bitsIn==16?2:1)) {
               int8_t samp;
               if (bitsIn==16) samp = (int8_t)( ((int16_t)(tmpBuf[i] | (tmpBuf[i+1]<<8))) >> 8 );
               else samp = (int8_t)(tmpBuf[i] - 128);
               
               s_acc += 1.0f;
               while (s_acc >= step && written < 8192) {
                  chunkBuf[written++] = samp;
                  s_acc -= step;
               }
            }
            if (written >= 4096) break;
          }
          if (written > 0) {
            applyDSP(chunkBuf, written);
            g_streamGen->write(chunkBuf, written);
          }
        }
        
        if (chunkBuf) free(chunkBuf);
        if (g_streamGen) { 
          if (g_streamBooster) { g_streamBooster->enable(false); soundGenerator.detach(g_streamBooster); delete g_streamBooster; g_streamBooster=nullptr; }
          g_streamGen->enable(false); delete g_streamGen; g_streamGen=nullptr; 
        }
      }
      client.stop();
      g_streamActive = false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ══════════════════════════════════════════════════════════════
//  applyTheme
// ══════════════════════════════════════════════════════════════
static void applyThemeToFrame(uiFrame* frame, const Theme& t) {
  frame->frameStyle().backgroundColor          = t.frameBg;
  frame->frameStyle().titleBackgroundColor      = t.titleBg;
  frame->frameStyle().activeTitleBackgroundColor= t.titleBgActive;
  frame->frameStyle().titleColor                = t.titleText;
  frame->frameStyle().activeTitleColor          = t.titleTextActive;

  frame->frameStyle().buttonColor                    = t.titleBg;
  frame->frameStyle().activeButtonColor              = t.titleBgActive;
  frame->frameStyle().mouseOverButtonColor           = t.titleTextActive;
  frame->frameStyle().mouseOverBackgroundButtonColor = t.accent;

  frame->windowStyle().borderColor        = t.titleBg;
  frame->windowStyle().activeBorderColor  = t.titleBgActive;
  frame->windowStyle().focusedBorderColor = t.titleBgActive;
}

// ══════════════════════════════════════════════════════════════
//  ChannelFrame
// ══════════════════════════════════════════════════════════════
class DrumGenerator : public fabgl::WaveformGenerator {
    int m_type;
    unsigned int m_phase;
    unsigned int m_phaseAcc;
    unsigned int m_seed;
  public:
    int m_env;
    DrumGenerator() : m_type(5), m_phase(0), m_phaseAcc(0), m_env(0), m_seed(1) {
      setSampleRate(WAV_PLAY_FREQ);
    }
    void setType(int type) { m_type = type; }
    void trigger() {
      m_phase = 0;
      m_phaseAcc = 0;
      m_env = 65536; // Represents 1.0 in 16.16 fixed point
    }
    void setFrequency(int value) override {}
    
    int getSample() override {
      if (m_env <= 10) return 0;
      m_phase++;
      
      int out = 0;
      m_seed = (1103515245 * m_seed + 12345);
      int noise = (m_seed >> 24) - 128; // -128 to 127
      
      if (m_type == 5) { // Kick
         unsigned int freq = 150 + (m_env >> 8); // Pitch sweep
         m_phaseAcc += freq;
         out = (m_phaseAcc % 65536) < 32768 ? 127 : -128;
         m_env = m_env - (m_env >> 12) - 1; // Decay ~200ms
      } 
      else if (m_type == 6) { // Snare
         unsigned int freq = 600 + (m_env >> 10);
         m_phaseAcc += freq;
         int tri = (m_phaseAcc % 65536) < 32768 ? 127 : -128;
         out = (tri / 2) + (noise / 2);
         m_env = m_env - (m_env >> 12) - 1;
      }
      else if (m_type == 7) { // HiHat
         out = noise;
         m_env = m_env - (m_env >> 10) - 1; // ~50ms
      }
      else if (m_type == 8) { // Crash
         out = noise;
         m_env = m_env - (m_env >> 15) - 1; // ~1.5s
      }
      else if (m_type == 9) { // Tom
         unsigned int freq = 300 + (m_env >> 8);
         m_phaseAcc += freq;
         out = (m_phaseAcc % 65536) < 32768 ? 127 : -128;
         m_env = m_env - (m_env >> 13) - 1; // ~300ms
      }
      else if (m_type == 10) { // Clap
         out = noise;
         if (m_phase < 500 || (m_phase > 1000 && m_phase < 1500) || (m_phase > 2000 && m_phase < 2500)) {
         } else { out = out / 4; }
         m_env = m_env - (m_env >> 11) - 1; // ~100ms
      }
      else if (m_type == 11) { // Cowbell
         m_phaseAcc += 1600;
         unsigned int phase2 = m_phase * 2400;
         int sq1 = (m_phaseAcc % 65536) < 32768 ? 127 : -128;
         int sq2 = (phase2 % 65536) < 32768 ? 127 : -128;
         out = (sq1 + sq2) / 2;
         m_env = m_env - (m_env >> 12) - 1; // ~200ms
      }
      else if (m_type == 12) { // Ride
         out = noise;
         m_env = m_env - (m_env >> 15) - 1; // ~1.5s
      }
      else if (m_type == 13) { // Woodblock
         m_phaseAcc += 2400;
         out = (m_phaseAcc % 65536) < 32768 ? 127 : -128;
         m_env = m_env - (m_env >> 10) - 1; // ~50ms
      }
      else if (m_type == 14) { // Bongo
         unsigned int freq = 1000 + (m_env >> 9);
         m_phaseAcc += freq;
         out = (m_phaseAcc % 65536) < 32768 ? 127 : -128;
         m_env = m_env - (m_env >> 11) - 1; // ~150ms
      }
      else if (m_type == 15) { // Conga
         unsigned int freq = 600 + (m_env >> 9);
         m_phaseAcc += freq;
         out = (m_phaseAcc % 65536) < 32768 ? 127 : -128;
         m_env = m_env - (m_env >> 12) - 1; // ~200ms
      }
      else if (m_type == 16) { // Tambourine
         out = noise;
         if (m_phase % 4000 < 1000) { } else { out = out / 4; }
         m_env = m_env - (m_env >> 11) - 1; // ~100ms
      }
      else if (m_type == 17) { // Shaker
         out = noise / 2;
         m_env = m_env - (m_env >> 10) - 1; // ~50ms
      }
      else if (m_type == 18) { // Laser Zap
         unsigned int freq = 300 + (m_env >> 3); // Sweep down fast
         m_phaseAcc += freq;
         out = (m_phaseAcc % 65536) < 32768 ? 127 : -128;
         m_env = m_env - (m_env >> 11) - 1; // ~150ms
      }
      else if (m_type == 19) { // Bell
         m_phaseAcc += 6000;
         out = (m_phaseAcc % 65536) < 32768 ? 127 : -128;
         m_env = m_env - (m_env >> 15) - 1; // ~1.5s
      }
      else if (m_type == 20) { // Rim Shot
         m_phaseAcc += 3200;
         out = (m_phaseAcc % 65536) < 32768 ? 127 : -128;
         out = (out + noise) / 2;
         m_env = m_env - (m_env >> 9) - 1; // ~25ms very short
      }
      else if (m_type == 21) { // Floor Tom
         unsigned int freq = 120 + (m_env >> 9);
         m_phaseAcc += freq;
         out = (m_phaseAcc % 65536) < 32768 ? 127 : -128;
         m_env = m_env - (m_env >> 13) - 1; // ~400ms
      }
      else if (m_type == 22) { // Guiro
         if (m_phase % 600 < 300) out = noise; else out = 0;
         m_env = m_env - (m_env >> 13) - 1;
      }
      else if (m_type == 23) { // Maracas
         out = noise / 3;
         m_env = m_env - (m_env >> 9) - 1; // ~25ms
      }
      else if (m_type == 24) { // 808 Kick
         unsigned int freq = 80 + (m_env >> 7);
         m_phaseAcc += freq;
         int tri = abs((int)(m_phaseAcc % 65536) - 32768) / 128 - 128;
         out = tri;
         m_env = m_env - (m_env >> 13) - 1; // ~400ms deep
      }
      else if (m_type == 25) { // 808 Clap
         out = noise;
         int burst = (m_phase < 300 || (m_phase > 700 && m_phase < 1000) || (m_phase > 1400 && m_phase < 1700)) ? 1 : 0;
         if (!burst) out = out / 6;
         m_env = m_env - (m_env >> 12) - 1;
      }
      else if (m_type == 26) { // Timbale
         m_phaseAcc += 2800;
         out = (m_phaseAcc % 65536) < 32768 ? 100 : -100;
         out += noise / 4;
         m_env = m_env - (m_env >> 11) - 1;
      }
      else if (m_type == 27) { // Agogo
         m_phaseAcc += 5000;
         out = (m_phaseAcc % 65536) < 32768 ? 127 : -128;
         m_env = m_env - (m_env >> 13) - 1;
      }
      else if (m_type == 28) { // Triangle Hit
         m_phaseAcc += 8000;
         int tri = abs((int)(m_phaseAcc % 65536) - 32768) / 128 - 128;
         out = tri;
         m_env = m_env - (m_env >> 14) - 1; // ~800ms ring
      }
      else if (m_type == 29) { // FM Bell
         unsigned int carrier = 4000;
         unsigned int modulator = 7000;
         int mod = abs((int)((m_phase * modulator) % 65536) - 32768) * 160 / 32768 - 80;
         m_phaseAcc += carrier + mod;
         out = (m_phaseAcc % 65536) < 32768 ? 127 : -128;
         m_env = m_env - (m_env >> 14) - 1;
      }
      else if (m_type == 30) { // Siren
         int lfo = abs((int)((m_phase * 3) % 65536) - 32768) * 2000 / 32768 - 1000;
         unsigned int freq = 1500 + lfo;
         m_phaseAcc += freq;
         out = (m_phaseAcc % 65536) < 32768 ? 127 : -128;
         m_env = m_env - (m_env >> 14) - 1;
      }
      else if (m_type == 31) { // Zap Down
         unsigned int freq = 100 + (m_env >> 2);
         m_phaseAcc += freq;
         out = (m_phaseAcc % 65536) < 32768 ? 127 : -128;
         m_env = m_env - (m_env >> 10) - 1;
      }
      else if (m_type == 32) { // Metallic
         m_phaseAcc += 3700;
         unsigned int p2 = m_phase * 5100;
         unsigned int p3 = m_phase * 7300;
         int sq1 = (m_phaseAcc % 65536) < 32768 ? 80 : -80;
         int sq2 = (p2 % 65536) < 32768 ? 60 : -60;
         int sq3 = (p3 % 65536) < 32768 ? 40 : -40;
         out = (sq1 + sq2 + sq3) / 3;
         m_env = m_env - (m_env >> 12) - 1;
      }
      else if (m_type == 33) { // Power Kick
         unsigned int freq = 100 + (m_env >> 6);
         m_phaseAcc += freq;
         out = (m_phaseAcc % 65536) < 32768 ? 127 : -128;
         out = out / 2 + noise / 8; // add body
         m_env = m_env - (m_env >> 12) - 1;
      }
      else if (m_type == 34) { // Buzz
         m_phaseAcc += 400;
         out = (m_phaseAcc % 65536) < 16384 ? 127 : -128; // narrow pulse
         m_env = m_env - (m_env >> 13) - 1;
      }
      int vol = (m_env * this->volume()) >> 16;
      return (out * vol) / 127;
    }
};
class ChannelFrame : public uiFrame {
    public:
      void processEvent(fabgl::uiEvent * event) override {
        fabgl::uiFrame::processEvent(event);
        if (event->id == fabgl::UIEVT_MOUSEBUTTONDOWN || event->id == fabgl::UIEVT_SETFOCUS) {
          if (g_title) WindowHack::moveTop(this->app()->rootWindow(), g_title);
          if (m_sig) WindowHack::moveTop(this->app()->rootWindow(), m_sig);
        }
      }
      SineWaveformGenerator     m_sine;
      SquareWaveformGenerator   m_square;
      TriangleWaveformGenerator m_tri;
      SawtoothWaveformGenerator m_saw;
      NoiseWaveformGenerator    m_noise;
      DrumGenerator             m_drum;
      WaveformGenerator*        m_cur = nullptr;
      uiLabel *m_volLbl, *m_freqLbl;
      uiSlider *m_volSl, *m_freqSl;
      uiComboBox* m_typeCB;
      uiButton* m_enBtn;
      uiButton* m_tempoBtn;
      int m_id;
      int m_tempoState = 0;
      bool m_seqToggle = false;
      uint32_t m_lastTempoTrigger = 0;
      
      ChannelFrame(uiWindow* p, int ch)
        : uiFrame(p, "", Point(2, 2 + ch*52), Size(292, 54)), m_id(ch) {
      frameProps().resizeable=false; frameProps().hasCloseButton=false;
      frameProps().hasMaximizeButton=false; frameProps().hasMinimizeButton=false;
      frameProps().moveable=false;
      frameStyle().titleFont = &fabgl::FONT_std_12;
      const char* titles[] = {"Signal Generation Channel 1", "Signal Generation Channel 2", "Signal Generation Channel 3", "Signal Generation Channel 4"};
      setTitle(titles[ch]);
      applyThemeToFrame(this, THEMES[g_themeIdx]);
  
      m_volLbl = new uiLabel(this, "Vol: 100", Point(4,17));
      m_volSl = new uiSlider(this, Point(57,17), Size(113,14), uiOrientation::Horizontal);
      m_volSl->onChange = [&](){ m_volLbl->setTextFmt("Vol:%3d", m_volSl->position());
        if(m_cur) m_cur->setVolume(m_volSl->position()); g_state.chVol[m_id] = m_volSl->position(); };
      m_volSl->setup(0,127,16); m_volSl->setPosition(100);
  
      new uiStaticLabel(this,"Type:",Point(175,17));
      m_typeCB = new uiComboBox(this, Point(206,17), Size(82,15), 40);
      m_typeCB->listBoxStyle().textFont = &fabgl::FONT_std_12;
      m_typeCB->textEditStyle().textFont = &fabgl::FONT_std_12;
      m_typeCB->items().append("Sine"); m_typeCB->items().append("Square");
      m_typeCB->items().append("Triangle"); m_typeCB->items().append("Sawtooth");
      m_typeCB->items().append("Noise");
      m_typeCB->items().append("Kick"); m_typeCB->items().append("Snare");
      m_typeCB->items().append("HiHat"); m_typeCB->items().append("Crash");
      m_typeCB->items().append("Tom"); m_typeCB->items().append("Clap");
      m_typeCB->items().append("Cowbell");
      m_typeCB->items().append("Ride"); m_typeCB->items().append("Woodblk");
      m_typeCB->items().append("Bongo"); m_typeCB->items().append("Conga");
      m_typeCB->items().append("Tambor"); m_typeCB->items().append("Shaker");
      m_typeCB->items().append("Laser"); m_typeCB->items().append("Bell");
      m_typeCB->items().append("RimSh"); m_typeCB->items().append("FlrTom");
      m_typeCB->items().append("Guiro"); m_typeCB->items().append("Maracs");
      m_typeCB->items().append("808K"); m_typeCB->items().append("808Cl");
      m_typeCB->items().append("Timbal"); m_typeCB->items().append("Agogo");
      m_typeCB->items().append("TriHit"); m_typeCB->items().append("FMBel");
      m_typeCB->items().append("Siren"); m_typeCB->items().append("ZapDn");
      m_typeCB->items().append("Metal"); m_typeCB->items().append("PwrK");
      m_typeCB->items().append("Buzz");
      m_typeCB->selectItem(ch%5);
      m_typeCB->onChange = [&](){ Serial.printf("[GEN] SetGen %d\n", m_typeCB->selectedItem()); setGen(m_typeCB->selectedItem()); g_state.chType[m_id] = m_typeCB->selectedItem(); };
  
      m_freqLbl = new uiLabel(this, "Frq:200", Point(4,34));
      m_freqSl = new uiSlider(this, Point(57,34), Size(113,14), uiOrientation::Horizontal);
      m_freqSl->onChange = [&](){ 
        m_freqLbl->setTextFmt("Frq:%d", m_freqSl->position());
        if(m_cur && m_typeCB->selectedItem() < 5) m_cur->setFrequency(m_freqSl->position()); g_state.chFreq[m_id] = m_freqSl->position(); 
      };
      m_freqSl->setup(20,8000,500); m_freqSl->setPosition(150+ch*80);
      
      m_enBtn = new uiButton(this, "Enable", Point(175,34), Size(50, 14));
      m_enBtn->onClick = [&](){
         if (!m_cur) return;
         bool nv = !m_cur->enabled();
         m_cur->enable(nv);
         if (nv && m_typeCB->selectedItem() >= 5) {
             m_drum.trigger();
         }
         m_enBtn->setText(nv ? "Disable" : "Enable"); g_state.chEn[m_id] = (nv?1:0);
      };
      
      m_tempoBtn = new uiButton(this, "Tmp: OFF", Point(228,34), Size(60, 14));
      m_tempoBtn->onClick = [&](){
         setTempo((m_tempoState + 1) % 5);
      };
      
      setGen(ch%5);
    }
    
    void setTempo(int val) {
       m_tempoState = val % 5; g_state.chTempo[m_id] = m_tempoState;
       const char* tNames[] = {"Tmp: OFF", "Tmp: 0.5s", "Tmp: 1s", "Tmp: 2s", "Tmp: 4s"};
       m_tempoBtn->setText(tNames[m_tempoState]);
       if (m_tempoState == 0) {
          if (m_cur) m_cur->setVolume(m_volSl->position());
       }
    }
    
    void setGen(int i) {
      Serial.printf("[GEN] Init Gen %d\n", i);
      if(m_cur){ m_cur->enable(false); soundGenerator.detach(m_cur); }
      if (i >= 5) {
         m_drum.setType(i);
         m_cur = &m_drum;
         app()->showWindow(m_freqLbl, false);
         app()->showWindow(m_freqSl, false);
      } else {
         switch(i){ case 0:m_cur=&m_sine;break; case 1:m_cur=&m_square;break;
           case 2:m_cur=&m_tri;break; case 3:m_cur=&m_saw;break; case 4:m_cur=&m_noise;break; }
         app()->showWindow(m_freqLbl, true);
         app()->showWindow(m_freqSl, true);
      }
      soundGenerator.attach(m_cur);
      m_cur->setVolume(m_volSl->position());
      if (i < 5) m_cur->setFrequency(m_freqSl->position());
      m_cur->enable(false); // Default disabled
      if (m_enBtn) m_enBtn->setText("Enable");
    }
};
class MasterFrame : public uiFrame {
  public:
    void processEvent(fabgl::uiEvent * event) override {
        fabgl::uiFrame::processEvent(event);
        if (event->id == fabgl::UIEVT_MOUSEBUTTONDOWN || event->id == fabgl::UIEVT_SETFOCUS) {
            if (g_title) WindowHack::moveTop(this->app()->rootWindow(), g_title);
            if (m_sig) WindowHack::moveTop(this->app()->rootWindow(), m_sig);
        }
    }
    uiSlider* m_vs;
    uiButton* m_enBtn;
    uiProgressBar* m_vuMeter = nullptr;
    
    MasterFrame(uiWindow* p)
      : uiFrame(p, "Master", Point(292, 102), Size(102, 108)) {
      frameStyle().titleFont=&fabgl::FONT_std_12;
      frameProps().resizeable=false; frameProps().hasCloseButton=false;
      frameProps().hasMaximizeButton=false; frameProps().hasMinimizeButton=false;
      frameProps().moveable=false;
      applyThemeToFrame(this, THEMES[g_themeIdx]);
  
      // Row 1: Enable & REC
      m_enBtn = new uiButton(this, "EN", Point(4, 16), Size(28, 16));
      m_enBtn->onClick = [=]() {
          g_masterEnable = !g_masterEnable; g_state.masterEn = g_masterEnable;
          m_enBtn->setText(g_masterEnable ? "EN" : "DIS");
          soundGenerator.play(g_masterEnable);
          parent()->repaint();
      };
      
      
      
      auto vl = new uiLabel(this, "Volume: 127", Point(4, 38));
      m_vs = new uiSlider(this, Point(4, 52), Size(90, 14), uiOrientation::Horizontal);
      m_vs->onChange = [&,vl](){ 
        vl->setTextFmt("Volume: %3d", m_vs->position());
        g_masterVol = m_vs->position(); g_state.masterVol = g_masterVol;
        soundGenerator.setVolume(m_vs->position()); 
      };
      m_vs->setup(0, 127, 16); m_vs->setPosition(127);

      // Row 3: VU Meter
      new uiStaticLabel(this, "VU Meter:", Point(4, 74));
      m_vuMeter = new uiProgressBar(this, Point(4, 88), Size(90, 14));
      m_vuMeter->setPercentage(0);

    }
  };

  class SystemFrame : public uiFrame {
  public:
    void processEvent(fabgl::uiEvent * event) override {
        fabgl::uiFrame::processEvent(event);
        if (event->id == fabgl::UIEVT_MOUSEBUTTONDOWN || event->id == fabgl::UIEVT_SETFOCUS) {
            if (g_title) WindowHack::moveTop(this->app()->rootWindow(), g_title);
            if (m_sig) WindowHack::moveTop(this->app()->rootWindow(), m_sig);
        }
    }
    uiComboBox* m_themeCB;
    uiButton* m_rb;

    SystemFrame(uiWindow* p)
      : uiFrame(p, "System", Point(292, 2), Size(102, 100)) {
      frameStyle().titleFont = &fabgl::FONT_std_12;
      frameProps().resizeable=false; frameProps().hasCloseButton=false;
      frameProps().hasMaximizeButton=false; frameProps().hasMinimizeButton=false;
      frameProps().moveable=false;
      applyThemeToFrame(this, THEMES[g_themeIdx]);

      // Row 1: Theme
      new uiStaticLabel(this, "Theme:", Point(4, 18));
      m_themeCB = new uiComboBox(this, Point(4, 32), Size(90, 15), 50);
      m_themeCB->listBoxStyle().textFont = &fabgl::FONT_std_12;
      m_themeCB->textEditStyle().textFont = &fabgl::FONT_std_12;
      for (int i=0; i<THEME_COUNT; i++) m_themeCB->items().append(THEMES[i].name);
      m_themeCB->selectItem(0);

      // Row 2: WiFi
      new uiStaticLabel(this, "WiFi:", Point(4, 54));
      auto wl = new uiLabel(this, "Off", Point(32, 54));
      if (g_wifiReady) {
        wl->setText(WiFi.localIP().toString().c_str());
        wl->labelStyle().textColor = C_BRIGHTGREEN;
      }

      // Row 3: Reset
      m_rb = new uiButton(this, "Reset", Point(4, 70), Size(90, 16));
      m_rb->onClick = [=](){
        auto cf = new uiFrame(app()->rootWindow(), "Reset", Point(120, 100), Size(160, 80));
        applyThemeToFrame(cf, THEMES[g_themeIdx]);
        cf->frameProps().hasCloseButton = false;
        cf->frameStyle().titleFont = &fabgl::FONT_std_12;
        new uiStaticLabel(cf, "Restart ESP32?", Point(30, 20));
        auto yb = new uiButton(cf, "Yes", Point(20, 45), Size(50, 20));
        auto nb = new uiButton(cf, "No", Point(80, 45), Size(50, 20));
        yb->windowStyle().focusedBorderColor = THEMES[g_themeIdx].titleBgActive;
        nb->windowStyle().focusedBorderColor = THEMES[g_themeIdx].titleBgActive;
        yb->buttonStyle().mouseDownBackgroundColor = THEMES[g_themeIdx].accent;
        nb->buttonStyle().mouseDownBackgroundColor = THEMES[g_themeIdx].accent;
        yb->onClick = [](){ ESP.restart(); };
        nb->onClick = [cf](){ cf->app()->showWindow(cf, false); };
        cf->app()->setFocusedWindow(yb);
      };
    }
  };

// ══════════════════════════════════════════════════════════════
//  SDPlayerFrame
// ══════════════════════════════════════════════════════════════
  class SDPlayerFrame : public uiFrame {
  public:
    uiFileBrowser* m_fb; uiLabel* m_st; uiSlider* m_pb; uiTimerHandle m_tmr;
    uiLabel* m_timePct;
    uiButton* m_playBtn; uiButton* m_stopBtn; uiButton* m_pauseBtn; uiButton* m_srcBtn; uiButton* m_agcBtn;
    uiSlider* m_boostSl;
    uiSlider* m_eqLowSl; uiSlider* m_eqLowMidSl; uiSlider* m_eqMidSl; uiSlider* m_eqHighMidSl; uiSlider* m_eqHighSl;
    uiComboBox* m_fxCB; uiButton* m_fxEnBtn; uiButton* m_speedBtn;
    uiComboBox* m_fxCB2; uiButton* m_fxEnBtn2;
    uiButton* m_overBtn;
    bool m_wasPlaying = false;
  
    SDPlayerFrame(uiWindow* p)
      : uiFrame(p, "Media Player                                       Src   AGC      SFX1  SFX2            Equalizer", Point(2,210), Size(392, 72)) {
      frameStyle().titleFont=&fabgl::FONT_std_12;
      frameProps().resizeable=false; frameProps().hasCloseButton=false;
      frameProps().hasMaximizeButton=false; frameProps().hasMinimizeButton=false;
      frameProps().moveable=false;
      applyThemeToFrame(this, THEMES[g_themeIdx]);
  
      m_fb = new uiFileBrowser(this,Point(4,16),Size(64,48)); 
      m_fb->setDirectory("/SD/SONGS");
  
      m_playBtn = new uiButton(this,"Ply",Point(72,16),Size(30,16));
      m_playBtn->onClick = [=]() { if(g_state.mediaSource==0) onPlay(); else g_state.mediaState=1; };
      
      m_pauseBtn = new uiButton(this,"Pau",Point(104,16),Size(32,16));
      m_pauseBtn->onClick = [=]() { if(g_state.mediaSource==0) wavPauseFn(); else g_state.mediaState=(g_state.mediaState==1?2:1); };
  
      m_stopBtn = new uiButton(this,"Stp",Point(138,16),Size(30,16));
      m_stopBtn->onClick = [=]() { if(g_state.mediaSource==0) wavStopFn(); else g_state.mediaState=0; };
  
      
      m_srcBtn = new uiButton(this, "SD", Point(170,16), Size(24,16));
      m_srcBtn->onClick = [=]() {
         wavStopFn(); g_state.mediaState = 0;
         g_state.mediaSource = (g_state.mediaSource == 0) ? 1 : 0;
         m_srcBtn->setText(g_state.mediaSource == 0 ? "SD" : "Wi-Fi");
         g_ui_update_req = true;
      };

      m_agcBtn = new uiButton(this, "Fix", Point(196,16), Size(20,16));
      m_agcBtn->onClick = [=]() {
         g_agcEnable = !g_agcEnable;
         g_state.agcEn = g_agcEnable; g_ui_update_req = true;
         m_agcBtn->buttonStyle().backgroundColor = g_agcEnable ? RGB888(255, 128, 0) : THEMES[g_themeIdx].btnBg;
         parent()->repaint();
      };

      m_st = new uiLabel(this,"Ready",Point(164,36));
      m_st->labelStyle().textColor = THEMES[g_themeIdx].text;
      
      new uiStaticLabel(this, "Bst:", Point(72, 36));
      m_boostSl = new uiSlider(this, Point(98, 36), Size(62, 14), uiOrientation::Horizontal);
      m_boostSl->setup(1, 10, 1); m_boostSl->setPosition(g_boostVal);
      m_boostSl->onChange = [=]() { g_boostVal = m_boostSl->position(); g_state.boost = g_boostVal; };

      // Overdrive moved to row 3, right of Speed, smaller
      m_overBtn = new uiButton(this, "OD:OFF", Point(258, 52), Size(36, 14));
      m_overBtn->onClick = [=]() {
         g_overEnable = !g_overEnable;
         g_state.overEn = g_overEnable; g_ui_update_req = true;
         m_overBtn->setText(g_overEnable ? "OD:ON" : "OD:OFF");
         m_overBtn->buttonStyle().backgroundColor = g_overEnable ? RGB888(255, 128, 0) : THEMES[g_themeIdx].btnBg;
         parent()->repaint();
      };
      
      new uiStaticLabel(this, "Time:", Point(72, 54));
      m_timePct = new uiLabel(this, "0%", Point(102, 54));
      m_pb = new uiSlider(this, Point(126, 54), Size(90, 14), uiOrientation::Horizontal);
      m_pb->setup(0, 100, 0);
      m_pb->setPosition(0);
      m_pb->sliderStyle().gripColor = THEMES[g_themeIdx].accent;
      m_pb->sliderStyle().rangeColor = THEMES[g_themeIdx].accent;
      m_pb->sliderStyle().slideColor = C_DARKGRAY;
      m_pb->onChange = [=]() {
         if (g_state.mediaSource == 1) {
         // Send MEDIA_SEEK over telemetry, handled by Python
         g_state.mediaProg = m_pb->position();
      }
      else if (g_wavPlaying && !g_wavStop && abs(m_pb->position() - g_wavProgress) > 1) {
             g_wavSeekPct = m_pb->position();
             g_wavProgress = g_wavSeekPct;
         }
      };
      
      // FX1 Enable + Select (Row 1, y=16)
      m_fxEnBtn = new uiButton(this, "OFF", Point(220, 16), Size(24, 16));
      m_fxEnBtn->onClick = [=]() {
         g_fxEnable = !g_fxEnable; g_state.fxEn = g_fxEnable ? 1 : 0;
         m_fxEnBtn->setText(g_fxEnable ? "ON" : "OFF");
         parent()->repaint();
      };

      m_fxCB = new uiComboBox(this, Point(246, 16), Size(48, 16), 50);
      m_fxCB->listBoxStyle().textFont = &fabgl::FONT_std_12;
      m_fxCB->textEditStyle().textFont = &fabgl::FONT_std_12;
      m_fxCB->listBoxStyle().selectedBackgroundColor = THEMES[g_themeIdx].accent;
      m_fxCB->listBoxStyle().focusedSelectedBackgroundColor = THEMES[g_themeIdx].accent;
      const char* fxNames[] = {"None", "Echo", "Revrb", "Dist", "Fuzz", "Trem", "BitC", "Decim", "Chor", "Flang", "Wah", "Ring", "Sub", "OctF", "Vibr", "Tele", "Slap", "Gate", "Phas", "Robot", "PtSh", "LoFi", "Tape", "Stut", "RvEch", "S&H", "Comb", "Formt", "Shmmr", "Radio", "WahF"};
      for (int i=0; i<31; i++) m_fxCB->items().append(fxNames[i]);
      m_fxCB->selectItem(g_fxType);
      m_fxCB->onChange = [=]() {
         g_fxType = m_fxCB->selectedItem(); g_state.fxType = g_fxType;
      };
      
      // FX2 Enable + Select (Row 2, y=34)
      m_fxEnBtn2 = new uiButton(this, "OFF", Point(220, 34), Size(24, 16));
      m_fxEnBtn2->onClick = [=]() {
         g_fxEnable2 = !g_fxEnable2; g_state.fxEn2 = g_fxEnable2 ? 1 : 0;
         m_fxEnBtn2->setText(g_fxEnable2 ? "ON" : "OFF");
         parent()->repaint();
      };

      m_fxCB2 = new uiComboBox(this, Point(246, 34), Size(48, 16), 50);
      m_fxCB2->listBoxStyle().textFont = &fabgl::FONT_std_12;
      m_fxCB2->textEditStyle().textFont = &fabgl::FONT_std_12;
      m_fxCB2->listBoxStyle().selectedBackgroundColor = THEMES[g_themeIdx].accent;
      m_fxCB2->listBoxStyle().focusedSelectedBackgroundColor = THEMES[g_themeIdx].accent;
      for (int i=0; i<31; i++) m_fxCB2->items().append(fxNames[i]);
      m_fxCB2->selectItem(g_fxType2);
      m_fxCB2->onChange = [=]() {
         g_fxType2 = m_fxCB2->selectedItem(); g_state.fxType2 = g_fxType2;
      };
      
      // Speed button (Row 3 left, y=52, smaller)
      m_speedBtn = new uiButton(this, "Spd:1x", Point(220, 52), Size(36, 14));
      m_speedBtn->onClick = [=]() {
         if (g_playbackSpeed == 0.25f) g_playbackSpeed = 0.5f;
         else if (g_playbackSpeed == 0.5f) g_playbackSpeed = 0.75f;
         else if (g_playbackSpeed == 0.75f) g_playbackSpeed = 1.0f;
         else if (g_playbackSpeed == 1.0f) g_playbackSpeed = 1.25f;
         else if (g_playbackSpeed == 1.25f) g_playbackSpeed = 1.5f;
         else if (g_playbackSpeed == 1.5f) g_playbackSpeed = 1.75f;
         else if (g_playbackSpeed == 1.75f) g_playbackSpeed = 2.0f;
         else if (g_playbackSpeed == 2.0f) g_playbackSpeed = 0.25f;
         
         g_state.speed = (int)(g_playbackSpeed * 100); g_ui_update_req = true;
         char buf[16];
         if (g_state.speed == 100 || g_state.speed == 0) strcpy(buf, "Spd:1x");
         else sprintf(buf, "S:%d%%", g_state.speed);
         m_speedBtn->setText(buf);
      };
  
      // EQ Sliders moved to the right (X: 296+)
      int eq_x = 296;
      m_eqLowSl = new uiSlider(this, Point(eq_x, 16), Size(12, 40), uiOrientation::Vertical);
      m_eqLowSl->setup(0, 200, 100); m_eqLowSl->setPosition(100);
      m_eqLowSl->onChange = [=]() { g_eqLow = m_eqLowSl->position() / 100.0f; g_state.eq[0] = m_eqLowSl->position(); updateEQ(); };
      new uiStaticLabel(this, "L", Point(eq_x+1, 58));
  
      m_eqLowMidSl = new uiSlider(this, Point(eq_x+18, 16), Size(12, 40), uiOrientation::Vertical);
      m_eqLowMidSl->setup(0, 200, 100); m_eqLowMidSl->setPosition(100);
      m_eqLowMidSl->onChange = [=]() { g_eqLowMid = m_eqLowMidSl->position() / 100.0f; g_state.eq[1] = m_eqLowMidSl->position(); updateEQ(); };
      new uiStaticLabel(this, "LM", Point(eq_x+16, 58));
  
      m_eqMidSl = new uiSlider(this, Point(eq_x+38, 16), Size(12, 40), uiOrientation::Vertical);
      m_eqMidSl->setup(0, 200, 100); m_eqMidSl->setPosition(100);
      m_eqMidSl->onChange = [=]() { g_eqMid = m_eqMidSl->position() / 100.0f; g_state.eq[2] = m_eqMidSl->position(); updateEQ(); };
      new uiStaticLabel(this, "M", Point(eq_x+39, 58));
  
      m_eqHighMidSl = new uiSlider(this, Point(eq_x+58, 16), Size(12, 40), uiOrientation::Vertical);
      m_eqHighMidSl->setup(0, 200, 100); m_eqHighMidSl->setPosition(100);
      m_eqHighMidSl->onChange = [=]() { g_eqHighMid = m_eqHighMidSl->position() / 100.0f; g_state.eq[3] = m_eqHighMidSl->position(); updateEQ(); };
      new uiStaticLabel(this, "HM", Point(eq_x+56, 58));
  
      m_eqHighSl = new uiSlider(this, Point(eq_x+78, 16), Size(12, 40), uiOrientation::Vertical);
      m_eqHighSl->setup(0, 200, 100); m_eqHighSl->setPosition(100);
      m_eqHighSl->onChange = [=]() { g_eqHigh = m_eqHighSl->position() / 100.0f; g_state.eq[4] = m_eqHighSl->position(); updateEQ(); };
      new uiStaticLabel(this, "H", Point(eq_x+79, 58));
      
      // Apply theme colors to EQ sliders
      m_eqLowSl->sliderStyle().gripColor = THEMES[g_themeIdx].accent;
      m_eqLowMidSl->sliderStyle().gripColor = THEMES[g_themeIdx].accent;
      m_eqMidSl->sliderStyle().gripColor = THEMES[g_themeIdx].accent;
      m_eqHighMidSl->sliderStyle().gripColor = THEMES[g_themeIdx].accent;
      m_eqHighSl->sliderStyle().gripColor = THEMES[g_themeIdx].accent;
      
      m_eqLowSl->sliderStyle().rangeColor = THEMES[g_themeIdx].accent;
      m_eqLowMidSl->sliderStyle().rangeColor = THEMES[g_themeIdx].accent;
      m_eqMidSl->sliderStyle().rangeColor = THEMES[g_themeIdx].accent;
      m_eqHighMidSl->sliderStyle().rangeColor = THEMES[g_themeIdx].accent;
      m_eqHighSl->sliderStyle().rangeColor = THEMES[g_themeIdx].accent;
      
      m_eqLowSl->sliderStyle().slideColor = C_DARKGRAY;
      m_eqLowMidSl->sliderStyle().slideColor = C_DARKGRAY;
      m_eqMidSl->sliderStyle().slideColor = C_DARKGRAY;
      m_eqHighMidSl->sliderStyle().slideColor = C_DARKGRAY;
      m_eqHighSl->sliderStyle().slideColor = C_DARKGRAY;
      
                  m_tmr = app()->setTimer(this, 200);
    }
  
    void processEvent(uiEvent* ev) override {
        if (ev->id == UIEVT_TIMER && ev->params.timerHandle == m_tmr) {
            if (g_ui_update_req) {
               g_ui_update_req = false;
               if (g_masterFrame) {
                  g_masterFrame->m_enBtn->setText(g_state.masterEn ? "EN" : "DIS");
                  g_masterFrame->m_vs->setPosition(g_state.masterVol);
                  if (g_masterEnable != g_state.masterEn) {
                      g_masterEnable = g_state.masterEn;
                      soundGenerator.play(g_masterEnable);
                  }
                  g_masterVol = g_state.masterVol;
               }
               for (int i=0; i<4; i++) {
                   if (g_chFrames[i]) {
                       g_chFrames[i]->m_volSl->setPosition(g_state.chVol[i]);
                       g_chFrames[i]->m_freqSl->setPosition(g_state.chFreq[i]);
                       g_chFrames[i]->m_typeCB->selectItem(g_state.chType[i]);
                       g_chFrames[i]->m_enBtn->setText(g_state.chEn[i] ? "Disable" : "Enable");
                       g_chFrames[i]->setTempo(g_state.chTempo[i]);
                       if (g_chFrames[i]->m_cur) {
                           g_chFrames[i]->m_cur->enable(g_state.chEn[i]);
                           g_chFrames[i]->m_cur->setVolume(g_state.chVol[i]);
                           if (g_state.chType[i] < 5) g_chFrames[i]->m_cur->setFrequency(g_state.chFreq[i]);
                       }
                   }
               }
               g_boostVal = g_state.boost; m_boostSl->setPosition(g_boostVal);
               g_eqLow = g_state.eq[0]/100.0f; m_eqLowSl->setPosition(g_state.eq[0]);
               g_eqLowMid = g_state.eq[1]/100.0f; m_eqLowMidSl->setPosition(g_state.eq[1]);
               g_eqMid = g_state.eq[2]/100.0f; m_eqMidSl->setPosition(g_state.eq[2]);
               g_eqHighMid = g_state.eq[3]/100.0f; m_eqHighMidSl->setPosition(g_state.eq[3]);
               g_eqHigh = g_state.eq[4]/100.0f; m_eqHighSl->setPosition(g_state.eq[4]);
               updateEQ();
               g_fxType = g_state.fxType; m_fxCB->selectItem(g_fxType);
               g_fxEnable = g_state.fxEn; m_fxEnBtn->setText(g_fxEnable ? "ON" : "OFF");
               g_fxType2 = g_state.fxType2; m_fxCB2->selectItem(g_fxType2);
               g_fxEnable2 = g_state.fxEn2; m_fxEnBtn2->setText(g_fxEnable2 ? "ON" : "OFF");
               if (m_srcBtn) m_srcBtn->setText(g_state.mediaSource == 0 ? "SD" : "Wi-Fi");
               g_overEnable = g_state.overEn; 
               if (m_overBtn) {
                   m_overBtn->setText(g_overEnable ? "OD:ON" : "OD:OFF");
                   m_overBtn->buttonStyle().backgroundColor = g_overEnable ? RGB888(255, 128, 0) : THEMES[g_themeIdx].btnBg;
               }
               g_playbackSpeed = g_state.speed / 100.0f;
               if (m_speedBtn) {
                   char buf[16];
                   if (g_state.speed == 100 || g_state.speed == 0) strcpy(buf, "Spd:1x");
                   else sprintf(buf, "S:%d%%", g_state.speed);
                   m_speedBtn->setText(buf);
               }
               
               if (g_wavPlayReq) {
                  Serial.printf("[DBG] g_wavPlayReq detected, calling onPlay()...\n");
                  g_wavPlayReq = false;
                  onPlay();
               }

               if (g_state.mediaSelectReq >= 0 && m_fb) {
                  m_fb->selectItem(g_state.mediaSelectReq);
                  g_state.mediaSelectReq = -1;
               }
               g_playbackSpeed = g_state.speed / 100.0f;
               g_agcEnable = g_state.agcEn;
               if (m_agcBtn) m_agcBtn->buttonStyle().backgroundColor = g_agcEnable ? RGB888(255, 128, 0) : THEMES[g_themeIdx].btnBg;
               
               // No need to call repaint() for sliders, but uiButton setText requires a repaint to update visuals!
               parent()->repaint();
            }
            if (g_masterEnable) {
               int total_rms = 0;
               if (g_wavPlaying && g_wavGen && !g_wavPaused) {
                  total_rms += (g_wavGen->m_rms * g_boostVal);
               }
               if (g_streamActive && g_streamGen && g_state.mediaState == 1) {
                  total_rms += (g_streamGen->m_rms * g_boostVal);
               }
               for (int i=0; i<4; i++) {
                   if (g_chFrames[i] && g_chFrames[i]->m_cur && g_chFrames[i]->m_cur->enabled()) {
                      if (g_chFrames[i]->m_typeCB->selectedItem() >= 5 && g_chFrames[i]->m_drum.m_env <= 10) {
                          continue; // Drum has finished playing, no VU contribution
                      }
                      total_rms += (g_chFrames[i]->m_cur->volume() / 2); 
                   }
               }
               int finalRms = (total_rms * g_masterVol) / 127;
               int pct = min(100, finalRms * 100 / 64); 
               
               if (g_masterFrame && g_masterFrame->m_vuMeter) {
                  if (pct < 60) g_masterFrame->m_vuMeter->progressBarStyle().foregroundColor = RGB888(0, 255, 0);
                  else if (pct < 80) g_masterFrame->m_vuMeter->progressBarStyle().foregroundColor = RGB888(255, 255, 0);
                  else g_masterFrame->m_vuMeter->progressBarStyle().foregroundColor = RGB888(255, 0, 0);
                  
                  g_masterFrame->m_vuMeter->setPercentage(pct);
               }
               g_vuPct = pct;
            } else {
               if (g_masterFrame && g_masterFrame->m_vuMeter) g_masterFrame->m_vuMeter->setPercentage(0);
               g_vuPct = 0;
            }
      
            if (g_wavPlaying) {
              m_pb->setPosition(g_wavProgress);
              char pctBuf[8]; sprintf(pctBuf, "%d%%", g_wavProgress);
              m_timePct->setText(pctBuf);
              m_playBtn->setText("Play");
              m_pauseBtn->setText(g_wavPaused ? "Resume" : "Pause");
              m_st->setText(g_wavPaused ? "Paused" : "Playing..."); 
              m_st->labelStyle().textColor = THEMES[g_themeIdx].accent;
            } else {
              m_st->setText(g_wavProgress>=100?"Done":"Ready");
              m_st->labelStyle().textColor = g_wavProgress>=100 ? RGB888(0,128,255) : THEMES[g_themeIdx].text;
              m_pb->setPosition(g_wavProgress);
              char pctBuf[8]; sprintf(pctBuf, "%d%%", g_wavProgress);
              m_timePct->setText(pctBuf);
              m_playBtn->setText("Play");
              m_pauseBtn->setText("Pause");
              
              if (m_wasPlaying && !g_wavStop && g_wavProgress >= 100) {
                 int idx = m_fb->firstSelectedItem();
                 if (idx >= 0 && idx < m_fb->count() - 1) {
                     m_fb->selectItem(idx + 1);
                     onPlay();
                 }
              }
            }
            m_wasPlaying = g_wavPlaying;
      
            if (g_streamActive) { m_st->setText("Streaming..."); m_st->labelStyle().textColor=THEMES[g_themeIdx].accent; }
            m_st->update();
        }
        uiFrame::processEvent(ev);
        if (ev->id == UIEVT_MOUSEBUTTONDOWN || ev->id == UIEVT_SETFOCUS) {
            if (g_title) WindowHack::moveTop(this->app()->rootWindow(), g_title);
            if (m_sig) WindowHack::moveTop(this->app()->rootWindow(), m_sig);
        }
    }

  public:
    void onPlay() {
      Serial.printf("[PLAY] onPlay called, mediaSource=%d\n", g_state.mediaSource);
      if (!m_fb->filename() || !strlen(m_fb->filename())) {
        Serial.printf("[PLAY] ERROR: No file selected!\n");
        m_st->setText("Err: No file"); m_st->labelStyle().textColor=RGB888(255,0,0); m_st->update(); return; }
      const char* fn = m_fb->filename(); int len=strlen(fn);
      Serial.printf("[PLAY] filename=%s, len=%d\n", fn, len);
      if (len<4 || strcasecmp(fn+len-4,".wav")) {
        Serial.printf("[PLAY] ERROR: Not a WAV file!\n");
        m_st->setText("Err: Not WAV"); m_st->labelStyle().textColor=RGB888(255,0,0); m_st->update(); return; }
      int pl = m_fb->content().getFullPath(fn);
      char* fpStr = (char*)malloc(pl + 2);
      m_fb->content().getFullPath(fn, fpStr, pl + 1);
      fpStr[pl] = '\0';
      Serial.printf("[PLAY] Full path=%s, calling wavPlay...\n", fpStr);
      m_st->setText("Loading..."); m_st->labelStyle().textColor=RGB888(255,255,0); m_st->update();
      wavPlay(fpStr);
      free(fpStr);
      Serial.printf("[PLAY] wavPlay returned.\n");
    }
  };
  static WiFiServer* g_tcpCtrlServer = nullptr;

static void wifiControlTask(void*) {
  g_tcpCtrlServer = new WiFiServer(CONTROL_PORT);
  g_tcpCtrlServer->begin();
  
  AppState last_state;
  memset(&last_state, 0, sizeof(AppState));

  while (true) {
    WiFiClient client = g_tcpCtrlServer->available();
    if (client) {
      client.setTimeout(2);
      while (client.connected()) {
        WiFiClient newClient = g_tcpCtrlServer->available();
        if (newClient) {
           client.stop();
           client = newClient;
           client.setTimeout(2);
        }
        
        while (client.available()) {
            String line = client.readStringUntil('\n');
            line.trim();
            if (line == "GET_DIR") {
               if (g_sdFrame && g_sdFrame->m_fb) {
                 client.print("TEL:DIR:");
                 for(int i=0; i<g_sdFrame->m_fb->count(); i++) { 
                    client.print(g_sdFrame->m_fb->content().get(i)->name);
                    if (i < g_sdFrame->m_fb->count()-1) client.print("|"); 
                 }
                 client.print("\n");
               }
            } else if (line == "GET_ALL_STATES") {
                // Ignore, we send telemetry automatically
            } else if (line.startsWith("SET_")) {
                int ch, val;
                if (sscanf(line.c_str(), "SET_CH_VOL:%d:%d", &ch, &val) == 2 && ch >= 0 && ch < 4) { g_state.chVol[ch] = val; g_ui_update_req = true; }
                else if (sscanf(line.c_str(), "SET_CH_FREQ:%d:%d", &ch, &val) == 2 && ch >= 0 && ch < 4) { g_state.chFreq[ch] = val; g_ui_update_req = true; }
                else if (sscanf(line.c_str(), "SET_CH_TYPE:%d:%d", &ch, &val) == 2 && ch >= 0 && ch < 4) { g_state.chType[ch] = val; g_ui_update_req = true; }
                else if (sscanf(line.c_str(), "SET_CH_EN:%d:%d", &ch, &val) == 2 && ch >= 0 && ch < 4) { g_state.chEn[ch] = val; g_ui_update_req = true; }
                else if (sscanf(line.c_str(), "SET_CH_TEMPO:%d:%d", &ch, &val) == 2 && ch >= 0 && ch < 4) { g_state.chTempo[ch] = val; g_ui_update_req = true; }
                else if (sscanf(line.c_str(), "SET_MASTER_VOL:%d", &val) == 1) { g_state.masterVol = val; g_ui_update_req = true; }
                else if (sscanf(line.c_str(), "SET_MASTER_EN:%d", &val) == 1) { g_state.masterEn = val; g_ui_update_req = true; }
                else if (sscanf(line.c_str(), "SET_BOOST:%d", &val) == 1) { g_state.boost = val; g_ui_update_req = true; }
                else if (sscanf(line.c_str(), "SET_EQ_LOW:%d", &val) == 1) { g_state.eq[0] = val; g_ui_update_req = true; }
                else if (sscanf(line.c_str(), "SET_EQ_LOWMID:%d", &val) == 1) { g_state.eq[1] = val; g_ui_update_req = true; }
                else if (sscanf(line.c_str(), "SET_EQ_MID:%d", &val) == 1) { g_state.eq[2] = val; g_ui_update_req = true; }
                else if (sscanf(line.c_str(), "SET_EQ_HIGHMID:%d", &val) == 1) { g_state.eq[3] = val; g_ui_update_req = true; }
                else if (sscanf(line.c_str(), "SET_EQ_HIGH:%d", &val) == 1) { g_state.eq[4] = val; g_ui_update_req = true; }
                else if (sscanf(line.c_str(), "SET_FX_TYPE:%d", &val) == 1) { g_state.fxType = val; g_ui_update_req = true; }
                else if (sscanf(line.c_str(), "SET_FX_EN:%d", &val) == 1) { g_state.fxEn = val; g_ui_update_req = true; }
                else if (sscanf(line.c_str(), "SET_FX_TYPE2:%d", &val) == 1) { g_state.fxType2 = val; g_ui_update_req = true; }
                else if (sscanf(line.c_str(), "SET_FX_EN2:%d", &val) == 1) { g_state.fxEn2 = val; g_ui_update_req = true; }
                else if (sscanf(line.c_str(), "SET_MEDIA_SRC:%d", &val) == 1) { 
                    wavStopFn(); g_state.mediaState = 0; 
                    g_state.mediaSource = val; 
                    g_ui_update_req = true; 
                }
                else if (sscanf(line.c_str(), "SET_OVERDRIVE:%d", &val) == 1) { g_state.overEn = val; g_overEnable = val; g_ui_update_req = true; }
                else if (sscanf(line.c_str(), "SET_SPEED:%d", &val) == 1) { g_state.speed = val; g_playbackSpeed = val / 100.0f; g_ui_update_req = true; }
                else if (sscanf(line.c_str(), "SET_AGC:%d", &val) == 1) { g_state.agcEn = val; g_agcEnable = val; g_ui_update_req = true; }
                else if (sscanf(line.c_str(), "SET_THEME:%d", &val) == 1) { 
                    if (val >= 0 && val < THEME_COUNT) {
                        g_themeIdx = val; 
                        if (g_sysFrame && g_sysFrame->m_themeCB) {
                            g_sysFrame->m_themeCB->selectItem(val);
                            g_sysFrame->m_themeCB->onChange();
                        }
                    }
                    g_ui_update_req = true; 
                }
            } else if (line == "RESET") {
                ESP.restart();
            } else if (line == "MEDIA_PLAY") {
                if (g_state.mediaSource == 0) { g_wavPlayReq = true; g_ui_update_req = true; }
                else { g_state.mediaState = 1; g_ui_update_req = true; }
            } else if (line == "MEDIA_PAUSE") {
                if (g_state.mediaSource == 0) { wavPauseFn(); g_ui_update_req = true; }
                else { g_state.mediaState = (g_state.mediaState == 1 ? 2 : 1); g_ui_update_req = true; }
            } else if (line == "MEDIA_STOP") {
                wavStopFn(); g_state.mediaState = 0; g_ui_update_req = true;
            } else if (line.startsWith("MEDIA_SELECT:")) {
                int idx; if (sscanf(line.c_str(), "MEDIA_SELECT:%d", &idx) == 1) { g_state.mediaSelectReq = idx; g_ui_update_req = true; }
            } else if (line.startsWith("MEDIA_SEEK:")) {
                int pct; if (sscanf(line.c_str(), "MEDIA_SEEK:%d", &pct) == 1) { 
                    if (g_state.mediaSource == 0) {
                        g_wavSeekPct = pct; g_wavProgress = pct; g_state.mediaProg = pct; g_ui_update_req = true;
                    } else {
                        g_state.mediaProg = pct; g_ui_update_req = true; // Tell Python to seek
                    }
                }
            }
        }
        
        static uint32_t lastTel = 0;
        if (millis() - lastTel > 50) {
           lastTel = millis();
           if (g_state.masterVol != last_state.masterVol) { client.printf("TEL:M_VOL:%d\n", g_state.masterVol); last_state.masterVol = g_state.masterVol; }
           if (g_state.masterEn != last_state.masterEn) { client.printf("TEL:M_EN:%d\n", g_state.masterEn); last_state.masterEn = g_state.masterEn; }
           if (g_state.boost != last_state.boost) { client.printf("TEL:BOOST:%d\n", g_state.boost); last_state.boost = g_state.boost; }
           for(int i=0;i<5;i++) { if (g_state.eq[i] != last_state.eq[i]) { client.printf("TEL:EQ_%d:%d\n", i, g_state.eq[i]); last_state.eq[i] = g_state.eq[i]; } }
           if (g_state.fxType != last_state.fxType) { client.printf("TEL:FX_TYPE:%d\n", g_state.fxType); last_state.fxType = g_state.fxType; }
           if (g_state.fxEn != last_state.fxEn) { client.printf("TEL:FX_EN:%d\n", g_state.fxEn); last_state.fxEn = g_state.fxEn; }
           if (g_state.fxType2 != last_state.fxType2) { client.printf("TEL:FX_TYPE2:%d\n", g_state.fxType2); last_state.fxType2 = g_state.fxType2; }
           if (g_state.fxEn2 != last_state.fxEn2) { client.printf("TEL:FX_EN2:%d\n", g_state.fxEn2); last_state.fxEn2 = g_state.fxEn2; }
           if (g_state.mediaSource != last_state.mediaSource) { client.printf("TEL:MEDIA_SRC:%d\n", g_state.mediaSource); last_state.mediaSource = g_state.mediaSource; }
           if (g_state.mediaState != last_state.mediaState) { client.printf("TEL:MEDIA_STATE:%d\n", g_state.mediaState); last_state.mediaState = g_state.mediaState; }
           if (g_state.mediaProg != last_state.mediaProg) { client.printf("TEL:PROG:%d\n", g_state.mediaProg); last_state.mediaProg = g_state.mediaProg; }
           if (g_state.overEn != last_state.overEn) { client.printf("TEL:OVERDRIVE:%d\n", g_state.overEn); last_state.overEn = g_state.overEn; }
           if (g_state.speed != last_state.speed) { client.printf("TEL:SPEED:%d\n", g_state.speed); last_state.speed = g_state.speed; }
           if (g_state.agcEn != last_state.agcEn) { client.printf("TEL:AGC_EN:%d\n", g_state.agcEn); last_state.agcEn = g_state.agcEn; }
           
           for(int i=0; i<4; i++) {
               if (g_state.chVol[i] != last_state.chVol[i]) { client.printf("TEL:CH_VOL:%d:%d\n", i, g_state.chVol[i]); last_state.chVol[i] = g_state.chVol[i]; }
               if (g_state.chFreq[i] != last_state.chFreq[i]) { client.printf("TEL:CH_FREQ:%d:%d\n", i, g_state.chFreq[i]); last_state.chFreq[i] = g_state.chFreq[i]; }
               if (g_state.chType[i] != last_state.chType[i]) { client.printf("TEL:CH_TYPE:%d:%d\n", i, g_state.chType[i]); last_state.chType[i] = g_state.chType[i]; }
               if (g_state.chEn[i] != last_state.chEn[i]) { client.printf("TEL:CH_EN:%d:%d\n", i, g_state.chEn[i]); last_state.chEn[i] = g_state.chEn[i]; }
               if (g_state.chTempo[i] != last_state.chTempo[i]) { client.printf("TEL:CH_TEMPO:%d:%d\n", i, g_state.chTempo[i]); last_state.chTempo[i] = g_state.chTempo[i]; }
           }
           client.printf("TEL:VU:%d\n", g_vuPct);
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
      }
      client.stop();
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}


// ══════════════════════════════════════════════════════════════
//  SoundMixerApp
// ══════════════════════════════════════════════════════════════
const int PIANO_FREQS[7] = {261, 293, 329, 349, 392, 440, 494};
const char* PIANO_LABELS[7] = {"Z", "X", "C", "V", "B", "N", "M"};
const VirtualKey PIANO_VKS[7] = {VK_z, VK_x, VK_c, VK_v, VK_b, VK_n, VK_m};

class SoundMixerApp : public uiApp {
  ChannelFrame* m_ch[4];
  MasterFrame*  m_master;
  SystemFrame*  m_sys;
  SDPlayerFrame* m_sd;

  fabgl::SineWaveformGenerator* m_pianoSine[7];
  fabgl::SawtoothWaveformGenerator* m_pianoSaw[7];
  uiLabel* m_pianoKeys[7] = {nullptr};
  bool m_pianoActive[7] = {false};

  void processEvent(uiEvent* ev) override {
    if (ev->id == UIEVT_KEYDOWN || ev->id == UIEVT_KEYUP) {
       for (int i = 0; i < 7; i++) {
          if (ev->params.key.VK == PIANO_VKS[i]) {
             bool isDown = (ev->id == UIEVT_KEYDOWN);
             if (m_pianoActive[i] != isDown) {
                m_pianoActive[i] = isDown;
                if (isDown) {
                   soundGenerator.attach(m_pianoSine[i]);
                   soundGenerator.attach(m_pianoSaw[i]);
                   m_pianoSine[i]->enable(true);
                   m_pianoSaw[i]->enable(true);
                   if (m_pianoKeys[i]) m_pianoKeys[i]->labelStyle().backgroundColor = THEMES[g_themeIdx].accent;
                } else {
                   m_pianoSine[i]->enable(false);
                   m_pianoSaw[i]->enable(false);
                   soundGenerator.detach(m_pianoSine[i]);
                   soundGenerator.detach(m_pianoSaw[i]);
                   if (m_pianoKeys[i]) m_pianoKeys[i]->labelStyle().backgroundColor = C_WHITE;
                }
             }
             return; // Handled piano key, do not process further
          }
       }
    }
    
    if (ev->id == UIEVT_KEYDOWN) {
      if (ev->params.key.VK == VK_RETURN) {
        ev->params.key.VK = VK_SPACE;
      }
      if (ev->params.key.VK == VK_TAB && !ev->params.key.CTRL && !ev->params.key.SHIFT) {
        uiWindow* lst[] = {
        m_ch[0]->m_typeCB, m_ch[0]->m_volSl, m_ch[0]->m_freqSl, m_ch[0]->m_enBtn,
        m_ch[1]->m_typeCB, m_ch[1]->m_volSl, m_ch[1]->m_freqSl, m_ch[1]->m_enBtn,
        m_ch[2]->m_typeCB, m_ch[2]->m_volSl, m_ch[2]->m_freqSl, m_ch[2]->m_enBtn,
        m_ch[3]->m_typeCB, m_ch[3]->m_volSl, m_ch[3]->m_freqSl, m_ch[3]->m_enBtn,
        m_master->m_vs, m_master->m_enBtn, m_sys->m_themeCB, m_sys->m_rb,
        m_sd->m_fb, m_sd->m_playBtn, m_sd->m_stopBtn, m_sd->m_pauseBtn, m_sd->m_boostSl
      };
      const int N = sizeof(lst)/sizeof(lst[0]);
        uiWindow* cur = focusedWindow();
        int idx = -1;
        for (int i=0; i<N; i++) {
          if (lst[i] == cur || (cur && cur->parent() == lst[i])) { idx = i; break; }
        }
        setFocusedWindow(lst[(idx + 1) % N]);
        return;
      }
    }
    uiApp::processEvent(ev);
  }

  void init() override {
    rootWindow()->frameStyle().backgroundColor = THEMES[g_themeIdx].bg;

    for (int i=0; i<4; ++i) {
      m_ch[i] = new ChannelFrame(rootWindow(), i);
      g_chFrames[i] = m_ch[i];
    }  m_master = new MasterFrame(rootWindow());
    g_masterFrame = m_master;
    m_sys = new SystemFrame(rootWindow());
    g_sysFrame = m_sys;
    m_sd     = new SDPlayerFrame(rootWindow());
    g_sdFrame = m_sd;
    
    g_title = new uiLabel(rootWindow(), "Sound Center", Point(155, 4));
    m_sig = new uiLabel(rootWindow(), "made w/ <3 by @UfkuAcik", Point(2, 282), Size(200, 15));
    if (m_sig) {
        WindowHack::moveTop(rootWindow(), m_sig);
    }

    // Initialize Polyphonic Organ UI
    for (int i = 0; i < 7; i++) {
       m_pianoKeys[i] = new uiLabel(rootWindow(), PIANO_LABELS[i], Point(210 + i * 22, 282), Size(20, 15));
       m_pianoKeys[i]->labelStyle().textColor = C_BLACK;
       m_pianoKeys[i]->labelStyle().backgroundColor = C_WHITE;
    }

    m_sys->m_themeCB->onChange = [&]() {
      g_themeIdx = m_sys->m_themeCB->selectedItem();
      const Theme& t = THEMES[g_themeIdx];
      rootWindow()->frameStyle().backgroundColor = t.bg;
      for (int i=0; i<4; i++) {
        applyThemeToFrame(m_ch[i], t);
        m_ch[i]->m_volSl->sliderStyle().gripColor = t.accent;
        m_ch[i]->m_volSl->sliderStyle().slideColor = C_DARKGRAY;
        m_ch[i]->m_volSl->sliderStyle().rangeColor = t.accent;
        m_ch[i]->m_freqSl->sliderStyle().gripColor = t.accent;
        m_ch[i]->m_freqSl->sliderStyle().slideColor = C_DARKGRAY;
        m_ch[i]->m_freqSl->sliderStyle().rangeColor = t.accent;
        m_ch[i]->m_typeCB->listBoxStyle().selectedBackgroundColor = t.accent;
        m_ch[i]->m_typeCB->listBoxStyle().focusedSelectedBackgroundColor = t.accent;
      }
      applyThemeToFrame(m_master, t);
      applyThemeToFrame(m_sys, t);
      m_master->m_vs->sliderStyle().gripColor = t.accent;
      m_master->m_vs->sliderStyle().slideColor = C_DARKGRAY;
      m_master->m_vs->sliderStyle().rangeColor = t.accent;
      m_sys->m_themeCB->listBoxStyle().selectedBackgroundColor = t.accent;
      m_sys->m_themeCB->listBoxStyle().focusedSelectedBackgroundColor = t.accent;
      m_sys->m_rb->buttonStyle().mouseDownBackgroundColor = t.accent;
      if (m_sd->m_boostSl) {
         m_sd->m_boostSl->sliderStyle().gripColor = t.accent;
         m_sd->m_boostSl->sliderStyle().rangeColor = t.accent;
         m_sd->m_boostSl->sliderStyle().slideColor = C_DARKGRAY;
         m_sd->m_boostSl->windowStyle().focusedBorderColor = t.accent;
      }
      if (m_sd->m_pb) {
         m_sd->m_pb->sliderStyle().gripColor = t.accent;
         m_sd->m_pb->sliderStyle().rangeColor = t.accent;
      }
      if (m_sd->m_fxCB) {
         m_sd->m_fxCB->listBoxStyle().selectedBackgroundColor = t.accent;
         m_sd->m_fxCB->listBoxStyle().focusedSelectedBackgroundColor = t.accent;
         m_sd->m_fxCB->windowStyle().focusedBorderColor = t.accent;
      }
      if (m_sd->m_fxCB2) {
         m_sd->m_fxCB2->listBoxStyle().selectedBackgroundColor = t.accent;
         m_sd->m_fxCB2->listBoxStyle().focusedSelectedBackgroundColor = t.accent;
         m_sd->m_fxCB2->windowStyle().focusedBorderColor = t.accent;
      }
      if (m_sd->m_eqLowSl) {
         m_sd->m_eqLowSl->sliderStyle().gripColor = t.accent; m_sd->m_eqLowSl->sliderStyle().rangeColor = t.accent;
         m_sd->m_eqLowMidSl->sliderStyle().gripColor = t.accent; m_sd->m_eqLowMidSl->sliderStyle().rangeColor = t.accent;
         m_sd->m_eqMidSl->sliderStyle().gripColor = t.accent; m_sd->m_eqMidSl->sliderStyle().rangeColor = t.accent;
         m_sd->m_eqHighMidSl->sliderStyle().gripColor = t.accent; m_sd->m_eqHighMidSl->sliderStyle().rangeColor = t.accent;
         m_sd->m_eqHighSl->sliderStyle().gripColor = t.accent; m_sd->m_eqHighSl->sliderStyle().rangeColor = t.accent;
      }
      m_sys->m_themeCB->windowStyle().focusedBorderColor = t.accent;
      m_master->m_enBtn->windowStyle().focusedBorderColor = t.accent;
      m_sys->m_rb->windowStyle().focusedBorderColor = t.accent;
      m_master->m_vs->windowStyle().focusedBorderColor = t.accent;
      
      applyThemeToFrame(m_sd, t);
      if (g_title) g_title->labelStyle().textColor = t.accent;
      if (m_sig) {
         m_sig->labelStyle().textColor = t.accent;
         m_sig->labelStyle().backgroundColor = t.bg;
      }
      m_sd->m_fb->listBoxStyle().selectedBackgroundColor = t.accent;
      m_sd->m_fb->listBoxStyle().focusedSelectedBackgroundColor = t.accent;
      m_sd->m_fb->windowStyle().focusedBorderColor = t.accent;
      m_sd->m_playBtn->windowStyle().focusedBorderColor = t.accent;
      m_sd->m_pauseBtn->windowStyle().focusedBorderColor = t.accent;
      m_sd->m_stopBtn->windowStyle().focusedBorderColor = t.accent;
      
      for(int i=0; i<4; i++) {
        if (m_ch[i]->m_enBtn) m_ch[i]->m_enBtn->windowStyle().focusedBorderColor = t.accent;
        m_ch[i]->m_typeCB->windowStyle().focusedBorderColor = t.accent;
      }
      
      m_sd->m_playBtn->buttonStyle().mouseDownBackgroundColor = t.accent;
      m_sd->m_stopBtn->buttonStyle().mouseDownBackgroundColor = t.accent;
      m_sd->m_pauseBtn->buttonStyle().mouseDownBackgroundColor = t.accent;
      
      rootWindow()->repaint();
    };

    m_sys->m_themeCB->selectItem(g_themeIdx);
    m_sys->m_themeCB->onChange();

    // Set frequencies (sampleRate is already 22050 because we never stopped soundGenerator in setup)
    for (int i = 0; i < 7; i++) {
       m_pianoSine[i] = new fabgl::SineWaveformGenerator();
       m_pianoSaw[i] = new fabgl::SawtoothWaveformGenerator();
       
       m_pianoSine[i]->setSampleRate(WAV_PLAY_FREQ);
       m_pianoSine[i]->setFrequency(PIANO_FREQS[i]);
       m_pianoSine[i]->setVolume(45); // Main body
       m_pianoSine[i]->enable(false);
       
       m_pianoSaw[i]->setSampleRate(WAV_PLAY_FREQ);
       m_pianoSaw[i]->setFrequency(PIANO_FREQS[i]);
       m_pianoSaw[i]->setVolume(12); // Harmonics
       m_pianoSaw[i]->enable(false);
    }
  }
} app;

// ══════════════════════════════════════════════════════════════
//  setup() / loop()
// ══════════════════════════════════════════════════════════════

static void tempoTask(void*) {
   while (true) {
      vTaskDelay(pdMS_TO_TICKS(100));
      uint32_t now = millis();
      for (int i=0; i<4; i++) {
         if (g_chFrames[i] && g_chFrames[i]->m_tempoState > 0) {
            if (g_chFrames[i]->m_cur && g_chFrames[i]->m_cur->enabled()) {
               int state = g_chFrames[i]->m_tempoState;
               int ms = state == 1 ? 500 : (state == 2 ? 1000 : (state == 3 ? 2000 : 4000));
               if (now - g_chFrames[i]->m_lastTempoTrigger >= ms) {
                  g_chFrames[i]->m_lastTempoTrigger = now;
                  if (g_chFrames[i]->m_typeCB->selectedItem() >= 5) {
                     g_chFrames[i]->m_drum.trigger();
                  } else {
                     g_chFrames[i]->m_seqToggle = !g_chFrames[i]->m_seqToggle;
                     if (g_chFrames[i]->m_cur) {
                        g_chFrames[i]->m_cur->setVolume(g_chFrames[i]->m_seqToggle ? g_chFrames[i]->m_volSl->position() : 0);
                     }
                  }
               }
            }
         }
      }
   }
}


void setup() {
  Serial.begin(115200); delay(500);

  // Force early allocation of I2S DMA buffers in internal SRAM
  // before WiFi and VGA consume all available internal DMA memory.
  // This prevents the StoreProhibited crash when PSRAM is enabled.
  soundGenerator.play(true);
  soundGenerator.play(false);

  Serial.printf("[SETUP] Starting...\n");
  xTaskCreatePinnedToCore(tempoTask, "tempoTask", 4096, nullptr, 1, nullptr, 1);

  prefs.begin("SndMixer", false);

  kbdController.begin(PS2Preset::KeyboardPort0_MousePort1, KbdMode::GenerateVirtualKeys);
  DisplayController.begin();
  DisplayController.setResolution(VGA_400x300_60Hz);

  Canvas cv(&DisplayController);
  cv.clear();
  cv.drawText(100, 140, "Sound Center loading...");
  cv.waitCompletion();

  fabgl::FileBrowser::mountSDCard(true, "/SD", 4, SD_CS, SD_MISO, SD_MOSI, SD_CLK);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i=0; i<20 && WiFi.status()!=WL_CONNECTED; i++) { delay(500); }
  if (WiFi.status() == WL_CONNECTED) {
    g_wifiReady = true;
    xTaskCreatePinnedToCore(wifiStreamTask,"wifiStream",5120,nullptr,1,&g_streamTask,0); // Core 0 (PRO) for network
    xTaskCreatePinnedToCore(wifiControlTask,"wifiCtrl",4096,nullptr,1,nullptr,0); // Core 0 (PRO) for network
  } else {
    esp_wifi_start(); 
  }

}

void loop() {
  app.runAsync(&DisplayController, 8192).joinAsyncRun();
}

























