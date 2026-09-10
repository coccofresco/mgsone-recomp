#pragma once

// ps1Runtime -- SPU (Sound Processing Unit)
// 24 voice channels, ADPCM decoding, ADSR envelopes, reverb, 512KB Sound RAM

#include <array>
#include <deque>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace ps1::spu {

// Constants
static constexpr uint32_t NUM_VOICES = 24;
static constexpr uint32_t SOUND_RAM_SIZE = 512 * 1024; // 512KB
static constexpr uint32_t SAMPLE_RATE = 44100;
static constexpr uint32_t ADPCM_BLOCK_SIZE = 16; // bytes per ADPCM block
static constexpr uint32_t ADPCM_SAMPLES_PER_BLOCK =
    28; // samples decoded per block

// ADSR Phase
enum class AdsrPhase : uint8_t { Attack, Decay, Sustain, Release, Off };

// Voice State
struct Voice {
  // Registers (set via I/O writes)
  int16_t volumeLeft = 0;
  int16_t volumeRight = 0;
  uint16_t pitch = 0;     // 4.12 fixed-point (1.0 = 0x1000 = 44100 Hz)
  uint16_t startAddr = 0; // in 8-byte units in SPU RAM
  uint16_t adsrLo = 0;    // Attack/Decay rates
  uint16_t adsrHi = 0;    // Sustain level / Sustain/Release rates
  uint16_t currentVolume = 0;
  uint16_t repeatAddr = 0; // Loop address in 8-byte units

  // Internal state
  bool keyOn = false;
  bool keyOff = false;
  bool loopFlag = false;
  bool endFlag = false;

  AdsrPhase adsrPhase = AdsrPhase::Off;
  int32_t adsrVolume = 0; // 0..0x7FFF
  int32_t adsrTarget = 0;
  int32_t adsrRate = 0;

  // ADPCM decode state
  int16_t prevSample1 = 0;   // s(n-1)
  int16_t prevSample2 = 0;   // s(n-2)
  uint32_t currentAddr = 0;  // byte address in SPU RAM
  uint32_t pitchCounter = 0; // 4.12 fixed-point accumulator

  // Decoded sample buffer (28 samples from current ADPCM block)
  int16_t decodedSamples[ADPCM_SAMPLES_PER_BLOCK] = {};
  uint32_t decodedIndex = 0; // current position in decoded buffer

  // Noise mode
  bool noiseMode = false;

  // Pitch modulation (use output of voice N-1)
  bool pitchMod = false;
};

// Reverb Config
struct ReverbConfig {
  uint16_t regs[32] = {}; // 32 reverb working area registers
  uint16_t workAreaStart = 0;
  int16_t volumeLeft = 0;
  int16_t volumeRight = 0;
};

// SPU Class
class SPU {
public:
  struct Snapshot {
    std::array<Voice, NUM_VOICES> voices{};
    uint16_t spuCtrl = 0;
    uint16_t spuStat = 0;
    int16_t mainVolL = 0;
    int16_t mainVolR = 0;
    uint16_t irqAddr = 0;
    bool irqPending = false;
    uint32_t keyOnLatch = 0;
    uint32_t keyOffLatch = 0;
    uint32_t noiseOn = 0;
    uint32_t pitchModOn = 0;
    uint32_t reverbOn = 0;
    uint32_t endxFlags = 0;
    uint32_t transferAddr = 0;
    uint16_t transferCtrl = 0;
    ReverbConfig reverb{};
    int32_t noiseLevel = 0;
    uint32_t noiseTimer = 0;
    uint32_t noiseStep = 0;
    std::vector<int16_t> xaBuffer;
    std::vector<int16_t> cdDaBuffer;
    uint32_t xaReadPos = 0;
    uint32_t cdDaReadPos = 0;
    uint64_t generatedSampleCount = 0;
    uint64_t clockSamplesPending = 0;
    bool firstNonzeroOutputSeen = false;
  };

  SPU();
  ~SPU();

  void reset();
  Snapshot snapshot() const;
  void restore(const Snapshot &state);

  // Register I/O -- called by Memory I/O routing
  void writeRegister(uint32_t addr, uint16_t val);
  uint16_t readRegister(uint32_t addr) const;

  // Tick: generate samples for the audio callback
  // outputBuffer is interleaved stereo S16 (left, right, left, right...)
  void generateSamples(int16_t *outputBuffer, uint32_t numSamples);

  // Guest-clock mode: SDL consumes PCM only; the game thread advances the
  // SPU up to each IRQ boundary before dispatching/rearming its callback.
  void enableGuestClock() { guestClock_ = true; }
  bool guestClockEnabled() const { return guestClock_; }
  void accrueSamples(uint32_t count);
  bool serviceClock();

  // Sound RAM access (for DMA ch4)
  uint8_t *soundRamPtr() { return soundRam_; }
  const uint8_t *soundRamPtr() const { return soundRam_; }
  void writeSoundRam(uint32_t addr, uint16_t val);
  uint16_t readSoundRam(uint32_t addr) const;

  // Load 512KB of Sound RAM from a save-state buffer.
  void loadSoundRam(const uint8_t *data);

  // Transfer (manual via registers)
  void setTransferAddr(uint16_t val);
  void writeTransferData(uint16_t val);

  // XA-ADPCM input from CD-ROM
  void pushXaSamples(const int16_t *samples, uint32_t count);

  // CD-DA audio input
  void pushCdDaSamples(const int16_t *samples, uint32_t count);

  // IRQ
  bool hasIrq() const { return irqPending_; }
  void clearIrq() { irqPending_ = false; }

  // Thread safety for audio callback
  std::mutex &getMutex() { return mutex_; }

  // Test-only hook (ps1Test/runtime/test_spu.cpp): decode a single 16-byte
  // SPU-ADPCM block in isolation, using the same algorithm as
  // advanceAdpcmBlock() (see spu.cpp for citations), so decoded PCM output
  // can be checked against hand-worked values without routing through
  // pitch/ADSR/mixing.
  std::array<int16_t, ADPCM_SAMPLES_PER_BLOCK>
  decodeAdpcmBlockForTest(const uint8_t block[ADPCM_BLOCK_SIZE],
                          int16_t prevSample1, int16_t prevSample2) const;

  // Test-only accessor (ps1Test/runtime/test_spu.cpp): a voice's ADPCM
  // playback/loop state. Needed because an End+Mute block can force
  // AdsrPhase::Off within a sample or two of being read, which would
  // otherwise make the repeat-address jump unobservable through audio
  // output or through registers (there is no hardware-readable "current
  // address" register).
  struct VoiceDebugState {
    uint32_t currentAddr; // byte address in SPU RAM of the next block to read
    uint16_t repeatAddr;  // loop target, in 8-byte units
    bool loopFlag;
    bool endFlag;
  };
  VoiceDebugState debugVoiceState(uint32_t voiceIdx) const;

private:
  uint32_t generateSamplesLocked(int16_t *output, uint32_t count, bool stopAtIrq);
  bool guestClock_ = false;
  uint64_t clockSamplesPending_ = 0;
  std::deque<int16_t> playbackQueue_;
  // Voices
  Voice voices_[NUM_VOICES];

  // Sound RAM
  uint8_t soundRam_[SOUND_RAM_SIZE];

  // Control Registers
  uint16_t spuCtrl_ = 0; // SPUCNT (0x1F801D80)
  uint16_t spuStat_ = 0; // SPUSTAT (0x1F801D86)
  int16_t mainVolL_ = 0; // Main Volume Left
  int16_t mainVolR_ = 0; // Main Volume Right
  uint16_t irqAddr_ = 0; // IRQ trigger address (in 8-byte units)
  bool irqPending_ = false;

  // Key on/off latches
  uint32_t keyOnLatch_ = 0;
  uint32_t keyOffLatch_ = 0;
  struct KeyEvent {
    uint32_t mask;
    bool keyOn;
  };
  std::vector<KeyEvent> pendingKeyEvents_;

  // Noise / Pitch Mod / Reverb on flags
  uint32_t noiseOn_ = 0;
  uint32_t pitchModOn_ = 0;
  uint32_t reverbOn_ = 0;
  uint32_t endxFlags_ = 0; // End of sample flags

  // Transfer
  uint32_t transferAddr_ = 0; // current address for manual transfer
  uint16_t transferCtrl_ = 0;

  // Reverb
  ReverbConfig reverb_;

  // Noise Generator
  int32_t noiseLevel_ = 0;
  uint32_t noiseTimer_ = 0;
  uint32_t noiseStep_ = 0;

  // XA/CD-DA buffers
  std::vector<int16_t> xaBuffer_;
  std::vector<int16_t> cdDaBuffer_;
  uint32_t xaReadPos_ = 0;
  uint32_t cdDaReadPos_ = 0;
  uint64_t generatedSampleCount_ = 0;
  bool firstNonzeroOutputSeen_ = false;

  // Thread safety
  mutable std::mutex mutex_;

  // Internal Methods
  void processVoice(uint32_t voiceIdx, int32_t &outL, int32_t &outR,
                    int16_t prevVoiceOutput);
  int16_t decodeAdpcmSample(Voice &v);
  void advanceAdpcmBlock(Voice &v);
  void tickAdsr(Voice &v);
  void keyOnVoice(uint32_t idx);
  void keyOffVoice(uint32_t idx);
  void tickNoiseGenerator();

  // ADPCM filter coefficients (5 pairs)
  static constexpr int32_t ADPCM_FILTER_POS[5] = {0, 60, 115, 98, 122};
  static constexpr int32_t ADPCM_FILTER_NEG[5] = {0, 0, -52, -55, -60};
};

} // namespace ps1::spu
