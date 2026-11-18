#include "Player.h"
#include "Library.h"
#include <M5Unified.h>
#include <esp_random.h>

Player player;

Player::Player() : _out(&M5.Speaker, 0) {}

bool Player::begin(fs::FS* fs)
{
  _fs = fs;
  _src = new AudioFileSourceFS(*fs);
  _q = xQueueCreate(16, sizeof(Cmd));
  _lock = xSemaphoreCreateMutex();
  _out.setup();
  // Same core and priority as M5Unified's MP3 example: the UI loop (prio 1)
  // runs whenever the decoder is waiting on the speaker, which is most of the time.
  return _src && _q && _lock &&
         xTaskCreatePinnedToCore(taskEntry, "mp3", 12288, this, 2, nullptr, 1) == pdPASS;
}

// ---------------------------------------------------------------------------
// UI side
// ---------------------------------------------------------------------------
void Player::send(const Cmd& c)
{
  if (xQueueSend(_q, &c, 0) != pdTRUE && c.type == C_LIST) delete c.list;
}

void Player::playList(std::vector<String>&& paths, int startIndex)
{
  send({ C_LIST, startIndex, 0, new std::vector<String>(std::move(paths)) });
}
void Player::togglePause()    { send({ C_TOGGLE, 0, 0, nullptr }); }
void Player::next()           { send({ C_NEXT, 0, 0, nullptr }); }
void Player::prev()           { send({ C_PREV, 0, 0, nullptr }); }
void Player::seekBy(float s)  { send({ C_SEEK, 0, s, nullptr }); }
void Player::stop()           { send({ C_STOP, 0, 0, nullptr }); }
void Player::setShuffle(bool on) { _shuffle = on; send({ C_SHUFFLE, 0, 0, nullptr }); }
void Player::setRepeat(RepeatMode m) { _repeat = (uint8_t)m; }

void Player::getInfo(TrackInfo& out)
{
  xSemaphoreTake(_lock, portMAX_DELAY);
  out = _shared;
  xSemaphoreGive(_lock);
}

// ---------------------------------------------------------------------------
// Audio task
// ---------------------------------------------------------------------------
void Player::run()
{
  for (;;)
  {
    Cmd c;
    TickType_t wait = (_decoding && state() == PlayState::Playing) ? 0 : pdMS_TO_TICKS(20);
    while (xQueueReceive(_q, &c, wait) == pdTRUE)
    {
      handle(c);
      wait = 0;
    }
    if (_decoding && state() == PlayState::Playing)
    {
      // One call fills about one 512-frame buffer, then waits on the speaker.
      if (!_mp3.loop()) { closeDecoder(); trackEnded(); }
    }
    publishElapsed();
  }
}

void Player::handle(const Cmd& c)
{
  switch (c.type)
  {
    case C_LIST:
    {
      closeDecoder();
      _list.reset(c.list);
      int start = c.i;
      if (start < 0 || start >= (int)_list->size()) start = 0;
      buildOrder(start);
      _atEnd = false;
      startFrom(_orderPos, +1);
      break;
    }

    case C_TOGGLE:
      if (state() == PlayState::Playing)      _state = (uint8_t)PlayState::Paused;
      else if (state() == PlayState::Paused)  _state = (uint8_t)PlayState::Playing;
      else if (_list && !_list->empty())
      {
        if (_atEnd) { _orderPos = 0; _atEnd = false; }
        startFrom(_orderPos, +1);
      }
      break;

    case C_NEXT: step(+1); break;

    case C_PREV:
      if (_decoding && currentElapsed() > 3.0f) startFrom(_orderPos, +1);   // restart this one
      else step(-1);
      break;

    case C_SEEK:
    {
      if (!_decoding) break;
      // Held seek keys queue several of these: take them as one jump.
      float delta = c.f;
      Cmd more;
      while (xQueuePeek(_q, &more, 0) == pdTRUE && more.type == C_SEEK)
      {
        xQueueReceive(_q, &more, 0);
        delta += more.f;
      }
      float t = currentElapsed() + delta;
      if (t < 0) t = 0;
      if (_info.duration > 0 && t > _info.duration - 1.0f)
      {
        // Seeking past the end: same as finishing the track.
        closeDecoder();
        trackEnded();
        break;
      }
      PlayState keep = state();
      if (openAt(_orderPos, t, true)) _state = (uint8_t)keep;
      else { _state = (uint8_t)PlayState::Stopped; publishInfo(); }
      break;
    }

    case C_STOP:
      closeDecoder();
      _state = (uint8_t)PlayState::Stopped;
      _timeBase = 0;
      publishInfo();
      break;

    case C_SHUFFLE:
      if (_list && !_list->empty())
      {
        int cur = _order.empty() ? 0 : _order[_orderPos];
        buildOrder(cur);
        publishInfo();
      }
      break;

    case C_REPEAT:
      break;
  }
}

void Player::buildOrder(int current)
{
  int n = _list ? (int)_list->size() : 0;
  _order.resize(n);
  for (int i = 0; i < n; ++i) _order[i] = i;
  if (_shuffle && n > 1)
  {
    for (int i = n - 1; i > 0; --i) std::swap(_order[i], _order[esp_random() % (i + 1)]);
    for (int i = 0; i < n; ++i) if (_order[i] == current) { std::swap(_order[0], _order[i]); break; }
    _orderPos = 0;
  }
  else
  {
    _orderPos = (current >= 0 && current < n) ? current : 0;
  }
}

void Player::closeDecoder()
{
  if (_decoding)
  {
    _mp3.stop();          // also drains the output and closes the file
    _decoding = false;
  }
  else if (_src && _src->isOpen())
  {
    _src->close();
  }
}

bool Player::openAt(int orderPos, float startSec, bool reuseInfo)
{
  if (!_list || orderPos < 0 || orderPos >= (int)_order.size()) return false;
  closeDecoder();
  _orderPos = orderPos;
  const String& path = (*_list)[_order[orderPos]];

  if (!reuseInfo)
  {
    File f = _fs->open(path);
    if (!f) return false;
    bool ok = _info.parse(f);
    f.close();
    if (!ok) return false;
  }
  if (!_src->open(path.c_str())) return false;

  uint32_t off = _info.offsetFor(startSec);
  if (!_src->seek(off, SEEK_SET)) { _src->close(); return false; }

  _out.eq().reset();
  _frameBase = _out.frames();
  _timeBase = startSec > 0 ? startSec : 0;

  if (!_mp3.begin(_src, &_out))
  {
    if (_src->isOpen()) _src->close();
    return false;
  }
  _decoding = true;
  if (!reuseInfo) publishInfo();
  publishElapsed();
  return true;
}

bool Player::startFrom(int orderPos, int dir)
{
  int n = _order.size();
  for (int tries = 0; tries < n; ++tries)
  {
    int pos = ((orderPos + dir * tries) % n + n) % n;
    if (openAt(pos, 0, false))
    {
      _state = (uint8_t)PlayState::Playing;
      _loaded = true;
      return true;
    }
  }
  // Nothing in the list would open.
  _state = (uint8_t)PlayState::Stopped;
  _loaded = false;
  publishInfo();
  return false;
}

void Player::step(int dir)
{
  int n = _order.size();
  if (!n) return;
  _atEnd = false;
  startFrom(((_orderPos + dir) % n + n) % n, dir);
}

void Player::trackEnded()
{
  int n = _order.size();
  if (!n) return;
  RepeatMode rep = repeat();

  if (rep == RepeatMode::One) { startFrom(_orderPos, +1); return; }

  if (_orderPos + 1 < n) { startFrom(_orderPos + 1, +1); return; }

  if (rep == RepeatMode::All)
  {
    if (_shuffle && n > 1)
    {
      // New random order for the next lap, without repeating the last song first.
      int last = _order[_orderPos];
      buildOrder(-1);
      if (_order[0] == last) std::swap(_order[0], _order[1]);
    }
    startFrom(0, +1);
    return;
  }

  // End of the list.
  _state = (uint8_t)PlayState::Stopped;
  _atEnd = true;
  _timeBase = 0;
  publishInfo();
}

float Player::currentElapsed() const
{
  uint32_t rate = _out.rate();
  float t = _timeBase;
  if (_decoding && rate) t += (float)(_out.frames() - _frameBase) / rate;
  return t;
}

void Player::publishElapsed()
{
  float t = (_decoding || state() != PlayState::Stopped) ? currentElapsed() : 0;
  _elapsedMs = (uint32_t)(t * 1000.0f);
}

void Player::publishInfo()
{
  TrackInfo t;
  if (_list && !_order.empty() && _orderPos < (int)_order.size())
  {
    t.path = (*_list)[_order[_orderPos]];
    t.title = _info.title;
    if (t.title.isEmpty())
    {
      String b = Library::baseName(t.path);
      if (Library::isMp3(b)) b.remove(b.length() - 4);
      t.title = foldUtf8(b);
    }
    t.artist     = _info.artist;
    t.album      = _info.album;
    t.duration   = _info.duration;
    t.bitrate    = _info.bitrate;
    t.sampleRate = _info.sampleRate;
    t.channels   = _info.channels;
    t.vbr        = _info.vbr;
    t.position   = _orderPos + 1;
    t.count      = _order.size();
  }
  xSemaphoreTake(_lock, portMAX_DELAY);
  _shared = t;
  xSemaphoreGive(_lock);
  _infoVersion++;
}
