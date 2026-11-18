// AudioOutputEQ.h
//
// ESP8266Audio output that runs decoded PCM through a 3-band EQ and hands it
// to M5.Speaker. Buffer handshake follows M5Unified's AudioOutputM5Speaker
// example (MIT): a buffer is refilled only after the speaker task releases it.
#pragma once

#include <AudioOutput.h>
#include <M5Unified.h>
#include <atomic>
#include <math.h>

// ---------------------------------------------------------------------------
// 3-band EQ: low shelf, mid peak, high shelf (RBJ cookbook biquads).
// Gains are set from the UI task; the audio task picks them up on its next
// sample, so there is no locking on the audio path.
// ---------------------------------------------------------------------------
class ThreeBandEQ
{
public:
  static constexpr float kLowHz  = 200.0f;
  static constexpr float kMidHz  = 1000.0f;
  static constexpr float kMidQ   = 0.7f;
  static constexpr float kHighHz = 4000.0f;
  static constexpr int   kMaxDb  = 12;

  /// UI side. dB, clamped to +-kMaxDb.
  void setGains(int low, int mid, int high)
  {
    _req[0] = clampDb(low);
    _req[1] = clampDb(mid);
    _req[2] = clampDb(high);
    _version.fetch_add(1, std::memory_order_release);
  }

  /// Audio side: recompute coefficients if the gains or the rate changed.
  inline void prepare(uint32_t rate)
  {
    if (!rate) rate = 44100;   // decoder hasn't reported its rate yet
    uint32_t v = _version.load(std::memory_order_acquire);
    if (v == _applied && rate == _rate) return;
    _applied = v;
    _rate = rate;
    // Keep every corner frequency well under Nyquist (8-16 kHz MPEG-2.5 files).
    const float nyq = 0.45f * _rate;
    const float fLow = fminf(kLowHz, nyq), fMid = fminf(kMidHz, nyq), fHigh = fminf(kHighHz, nyq);

    int g[3] = { _req[0], _req[1], _req[2] };
    int maxBoost = 0;
    for (int i = 0; i < 3; ++i) { if (g[i] > maxBoost) maxBoost = g[i]; }
    // Auto pre-gain: pull the level down by the largest boost so a boosted
    // band can't push full-scale material into clipping.
    _preamp = powf(10.0f, -maxBoost / 20.0f);

    _band[0].on = g[0] != 0; if (_band[0].on) _band[0].lowShelf (_rate, fLow,  g[0]);
    _band[1].on = g[1] != 0; if (_band[1].on) _band[1].peaking  (_rate, fMid,  kMidQ, g[1]);
    _band[2].on = g[2] != 0; if (_band[2].on) _band[2].highShelf(_rate, fHigh, g[2]);
    _active = _band[0].on || _band[1].on || _band[2].on;
  }

  /// Clear filter history (track change / seek), so nothing rings across.
  void reset()
  {
    for (auto& b : _band) { b.z1[0] = b.z1[1] = b.z2[0] = b.z2[1] = 0.0f; }
  }

  inline void process(int16_t& l, int16_t& r)
  {
    if (!_active) return;
    float x[2] = { l * _preamp, r * _preamp };
    for (auto& b : _band)
    {
      if (!b.on) continue;
      for (int c = 0; c < 2; ++c)
      {
        // transposed direct form II
        float y = b.b0 * x[c] + b.z1[c];
        b.z1[c] = b.b1 * x[c] - b.a1 * y + b.z2[c];
        b.z2[c] = b.b2 * x[c] - b.a2 * y;
        x[c] = y;
      }
    }
    l = sat16(x[0]);
    r = sat16(x[1]);
  }

private:
  struct Biquad
  {
    bool on = false;
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1[2] = {0, 0}, z2[2] = {0, 0};

    void set(float nb0, float nb1, float nb2, float na0, float na1, float na2)
    {
      b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0;
      a1 = na1 / na0; a2 = na2 / na0;
    }
    void peaking(float fs, float f0, float q, float db)
    {
      float A = powf(10.0f, db / 40.0f);
      float w = 2.0f * (float)M_PI * f0 / fs;
      float cw = cosf(w), alpha = sinf(w) / (2.0f * q);
      set(1 + alpha * A, -2 * cw, 1 - alpha * A,
          1 + alpha / A, -2 * cw, 1 - alpha / A);
    }
    void lowShelf(float fs, float f0, float db)
    {
      float A = powf(10.0f, db / 40.0f);
      float w = 2.0f * (float)M_PI * f0 / fs;
      float cw = cosf(w), alpha = sinf(w) / 2.0f * sqrtf(2.0f); // S = 1
      float k = 2.0f * sqrtf(A) * alpha;
      set(A * ((A + 1) - (A - 1) * cw + k),
          2 * A * ((A - 1) - (A + 1) * cw),
          A * ((A + 1) - (A - 1) * cw - k),
          (A + 1) + (A - 1) * cw + k,
          -2 * ((A - 1) + (A + 1) * cw),
          (A + 1) + (A - 1) * cw - k);
    }
    void highShelf(float fs, float f0, float db)
    {
      float A = powf(10.0f, db / 40.0f);
      float w = 2.0f * (float)M_PI * f0 / fs;
      float cw = cosf(w), alpha = sinf(w) / 2.0f * sqrtf(2.0f);
      float k = 2.0f * sqrtf(A) * alpha;
      set(A * ((A + 1) + (A - 1) * cw + k),
          -2 * A * ((A - 1) + (A + 1) * cw),
          A * ((A + 1) + (A - 1) * cw - k),
          (A + 1) - (A - 1) * cw + k,
          2 * ((A - 1) - (A + 1) * cw),
          (A + 1) - (A - 1) * cw - k);
    }
  };

  static int clampDb(int v) { return v > kMaxDb ? kMaxDb : (v < -kMaxDb ? -kMaxDb : v); }
  static inline int16_t sat16(float v)
  {
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)lrintf(v);
  }

  Biquad _band[3];
  std::atomic<int> _req[3] = { {0}, {0}, {0} };
  std::atomic<uint32_t> _version { 1 };
  uint32_t _applied = 0;
  uint32_t _rate = 0;
  float _preamp = 1.0f;
  bool _active = false;
};

// ---------------------------------------------------------------------------
// Speaker output
// ---------------------------------------------------------------------------
class AudioOutputEQSpeaker : public AudioOutput
{
public:
  explicit AudioOutputEQSpeaker(m5::Speaker_Class* spk, uint8_t virtual_channel = 0)
  : _spk(spk), _ch(virtual_channel) {}

  /// After M5.begin(), before anything plays.
  void setup() { _spk->setBufferReleaseCallback(this, bufferReleased); }

  ThreeBandEQ& eq() { return _eq; }

  bool begin() override { return true; }

  bool ConsumeSample(int16_t sample[2]) override
  {
    // Check for room first: a rejected sample is offered again, and the EQ
    // must see every sample exactly once.
    if (_pos >= kBufSize) { flush(); return false; }
    _eq.prepare(hertz);
    int16_t l = sample[0], r = sample[1];
    _eq.process(l, r);
    _buf[_idx][_pos++] = l;
    _buf[_idx][_pos++] = r;
    return true;
  }

  void flush() override
  {
    if (!_pos) return;
    _busy[_idx] = true;
    if (_spk->playRaw(_buf[_idx], _pos, hertz, true, 1, _ch)) { _frames += _pos / 2; }
    else { _busy[_idx] = false; }
    if (++_idx >= kBufCount) _idx = 0;
    _pos = 0;
    while (_busy[_idx]) { vTaskDelay(1); }
  }

  bool stop() override
  {
    flush();
    for (size_t i = 0; i < kBufCount; ++i) { while (_busy[i]) { vTaskDelay(1); } }
    return true;
  }

  /// Stereo frames handed to the speaker so far (at the decoder's rate).
  uint32_t frames() const { return _frames; }
  uint32_t rate() const { return hertz; }

private:
  static constexpr size_t kBufCount = 3;
  static constexpr size_t kBufSize = 1024;   // int16 values = 512 stereo frames

  static void bufferReleased(void* args, const void* data, uint8_t)
  {
    auto me = (AudioOutputEQSpeaker*)args;
    for (size_t i = 0; i < kBufCount; ++i) { if (data == me->_buf[i]) me->_busy[i] = false; }
  }

  m5::Speaker_Class* _spk;
  uint8_t _ch;
  ThreeBandEQ _eq;
  int16_t _buf[kBufCount][kBufSize];
  std::atomic<bool> _busy[kBufCount] = { {false}, {false}, {false} };
  size_t _pos = 0;
  size_t _idx = 0;
  uint32_t _frames = 0;
};
