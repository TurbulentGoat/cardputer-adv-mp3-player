// Player.h — decoding runs in its own task; the UI talks to it through a
// command queue and reads state back through atomics and a locked snapshot.
#pragma once

#include <Arduino.h>
#include <FS.h>
#include <atomic>
#include <memory>
#include <vector>

#include <AudioFileSourceFS.h>
#include <AudioGeneratorMP3.h>

#include "AudioOutputEQ.h"
#include "Mp3Info.h"

enum class RepeatMode : uint8_t { Off = 0, All = 1, One = 2 };
enum class PlayState  : uint8_t { Stopped = 0, Playing = 1, Paused = 2 };

struct TrackInfo
{
  String   path, title, artist, album;
  float    duration   = 0;
  uint16_t bitrate    = 0;
  uint32_t sampleRate = 0;
  uint8_t  channels   = 0;
  bool     vbr        = false;
  int      position   = 0;   // 1-based, in play order
  int      count      = 0;
};

class Player
{
public:
  Player();
  bool begin(fs::FS* fs);
  AudioOutputEQSpeaker& output() { return _out; }

  // ---- UI side: all of these just queue a command ----
  void playList(std::vector<String>&& paths, int startIndex);
  void togglePause();
  void next();
  void prev();
  void seekBy(float seconds);
  void stop();
  void setShuffle(bool on);
  void setRepeat(RepeatMode m);

  // ---- UI side: state ----
  PlayState  state()   const { return (PlayState)_state.load(); }
  float      elapsed() const { return _elapsedMs.load() / 1000.0f; }
  bool       shuffle() const { return _shuffle.load(); }
  RepeatMode repeat()  const { return (RepeatMode)_repeat.load(); }
  bool       loaded()  const { return _loaded.load(); }
  uint32_t   infoVersion() const { return _infoVersion.load(); }
  void       getInfo(TrackInfo& out);

private:
  enum CmdType : uint8_t { C_LIST, C_TOGGLE, C_NEXT, C_PREV, C_SEEK, C_STOP, C_SHUFFLE, C_REPEAT };
  struct Cmd { CmdType type; int i; float f; std::vector<String>* list; };

  static void taskEntry(void* arg) { ((Player*)arg)->run(); }
  void run();
  void handle(const Cmd& c);
  void send(const Cmd& c);

  bool openAt(int orderPos, float startSec, bool reuseInfo);
  bool startFrom(int orderPos, int dir);   // tries following tracks if one won't open
  void closeDecoder();
  void trackEnded();
  void step(int dir);
  void buildOrder(int current);
  void publishInfo();
  void publishElapsed();
  float currentElapsed() const;

  // audio-task-only state
  fs::FS* _fs = nullptr;
  AudioFileSourceFS* _src = nullptr;
  AudioGeneratorMP3 _mp3;
  AudioOutputEQSpeaker _out;
  Mp3Info _info;
  std::unique_ptr<std::vector<String>> _list;
  std::vector<uint16_t> _order;
  int _orderPos = 0;
  bool _decoding = false;
  bool _atEnd = false;
  float _timeBase = 0;
  uint32_t _frameBase = 0;

  // shared
  QueueHandle_t _q = nullptr;
  SemaphoreHandle_t _lock = nullptr;
  TrackInfo _shared;
  std::atomic<uint8_t>  _state { (uint8_t)PlayState::Stopped };
  std::atomic<uint32_t> _elapsedMs { 0 };
  std::atomic<bool>     _shuffle { false };
  std::atomic<uint8_t>  _repeat { (uint8_t)RepeatMode::All };
  std::atomic<bool>     _loaded { false };
  std::atomic<uint32_t> _infoVersion { 0 };
};

extern Player player;
