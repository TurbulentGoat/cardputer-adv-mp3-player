// Cardputer MP3 — an MP3 player for the M5Stack Cardputer ADV
//
// Plays .mp3 files from the microSD card through the speaker or headphone
// jack, with a folder browser, shuffle/repeat, seeking and a 3-band EQ.

#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <Preferences.h>
#include <bitset>
#include <vector>

#include "Library.h"
#include "Player.h"

// ---------------------------------------------------------------------------
// Look
// ---------------------------------------------------------------------------
static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
namespace col
{
  constexpr uint16_t bg     = rgb(10, 12, 16);
  constexpr uint16_t panel  = rgb(28, 32, 42);
  constexpr uint16_t panel2 = rgb(46, 52, 66);
  constexpr uint16_t text   = rgb(236, 236, 236);
  constexpr uint16_t dim    = rgb(130, 138, 152);
  constexpr uint16_t accent = rgb(255, 145, 40);
  constexpr uint16_t cool   = rgb(80, 200, 255);
  constexpr uint16_t folder = rgb(240, 196, 80);
  constexpr uint16_t good   = rgb(120, 220, 120);
  constexpr uint16_t warn   = rgb(255, 90, 80);
}

static constexpr int W = 240, H = 135;
// Bound to M5.Display, never M5Cardputer.Display: that one is a reference that
// only gets set when M5Cardputer is constructed, and ESP-IDF runs global
// constructors in reverse link order, so this file's globals come first.
// Taking its address here gave the canvas a null parent -> crash on first draw.
static M5Canvas cv(&M5.Display);

enum class Screen : uint8_t { Library, NowPlaying, Equalizer };
static Screen screen = Screen::Library;

// ---------------------------------------------------------------------------
// Settings (kept in NVS)
// ---------------------------------------------------------------------------
struct Settings
{
  int     volume     = 50;      // percent, 5 % steps
  int     eq[3]      = {0, 0, 0};
  bool    shuffle    = false;
  uint8_t repeat     = (uint8_t)RepeatMode::All;
  uint8_t brightness = 1;
  String  dir        = "/";
};
static Settings cfg;
static Preferences prefs;
static uint32_t saveDue = 0;
static const uint8_t kBrightness[] = { 40, 100, 180, 255 };

static void loadSettings()
{
  prefs.begin("mp3player", false);
  cfg.volume     = prefs.getUChar("vol", 50);
  cfg.eq[0]      = prefs.getChar("eqL", 0);
  cfg.eq[1]      = prefs.getChar("eqM", 0);
  cfg.eq[2]      = prefs.getChar("eqH", 0);
  cfg.shuffle    = prefs.getBool("shuf", false);
  cfg.repeat     = prefs.getUChar("rep", (uint8_t)RepeatMode::All) % 3;
  cfg.brightness = prefs.getUChar("bri", 1) % 4;
  cfg.dir        = prefs.getString("dir", "/");
  prefs.end();
  if (cfg.volume > 100) cfg.volume = 100;
  for (int& g : cfg.eq) g = constrain(g, -ThreeBandEQ::kMaxDb, ThreeBandEQ::kMaxDb);
}

static void saveSettings()
{
  prefs.begin("mp3player", false);
  prefs.putUChar("vol", cfg.volume);
  prefs.putChar("eqL", cfg.eq[0]);
  prefs.putChar("eqM", cfg.eq[1]);
  prefs.putChar("eqH", cfg.eq[2]);
  prefs.putBool("shuf", cfg.shuffle);
  prefs.putUChar("rep", cfg.repeat);
  prefs.putUChar("bri", cfg.brightness);
  prefs.putString("dir", cfg.dir);
  prefs.end();
}

static void markDirty() { saveDue = millis() + 3000; }   // batch writes, spare the flash

// ---------------------------------------------------------------------------
// Audio settings
// ---------------------------------------------------------------------------
static bool muted = false;

static void applyVolume()
{
  // M5.Speaker squares this, so even steps here already sound even.
  M5.Speaker.setVolume(muted ? 0 : (uint8_t)(cfg.volume * 255 / 100));
}

static void applyEq() { player.output().eq().setGains(cfg.eq[0], cfg.eq[1], cfg.eq[2]); }

// ---------------------------------------------------------------------------
// Screen power
// ---------------------------------------------------------------------------
static constexpr uint32_t kAutoOffMs = 60000;   // screen off after 1 min idle while playing
static bool screenOn = true;
static uint32_t lastInput = 0;

static void setScreen(bool on)
{
  screenOn = on;
  M5.Display.setBrightness(on ? kBrightness[cfg.brightness] : 0);
}

// ---------------------------------------------------------------------------
// Toast
// ---------------------------------------------------------------------------
static String toastText;
static int toastBar = -1;
static uint32_t toastUntil = 0;

static void toast(const String& s, int barPct = -1, uint32_t ms = 1100)
{
  toastText = s;
  toastBar = barPct;
  toastUntil = millis() + ms;
}

// ---------------------------------------------------------------------------
// Library browser
// ---------------------------------------------------------------------------
static String curDir = "/";
static std::vector<LibEntry> entries;
static bool listTruncated = false;
static bool listFailed = false;
static int sel = 0, topRow = 0;
static constexpr int kRowH = 15, kRows = 6, kListY = 29;

static bool hasUpRow() { return curDir != "/"; }
static int rowCount() { return (int)entries.size() + (hasUpRow() ? 1 : 0); }

static void openDir(const String& path, const String& selectName = String())
{
  std::vector<LibEntry> tmp;
  bool trunc = false;
  if (!Library::list(SD, path, tmp, trunc))
  {
    if (path != "/") { openDir("/"); return; }
    listFailed = true;
  }
  else listFailed = false;
  curDir = path;
  entries.swap(tmp);
  listTruncated = trunc;
  sel = 0;
  topRow = 0;
  if (selectName.length())
  {
    for (size_t i = 0; i < entries.size(); ++i)
      if (entries[i].name == selectName) { sel = i + (hasUpRow() ? 1 : 0); break; }
  }
  if (cfg.dir != curDir) { cfg.dir = curDir; markDirty(); }
}

static void goUp()
{
  if (!hasUpRow()) return;
  String from = Library::baseName(curDir);
  openDir(Library::parent(curDir), from);
}

static void activateSelection()
{
  int n = rowCount();
  if (!n) return;
  if (hasUpRow() && sel == 0) { goUp(); return; }
  const LibEntry& e = entries[sel - (hasUpRow() ? 1 : 0)];
  if (e.dir) { openDir(Library::join(curDir, e.name)); return; }

  // Play this folder's MP3s, starting from the selected one.
  std::vector<String> paths;
  int start = 0;
  for (const auto& x : entries)
  {
    if (x.dir) continue;
    if (&x == &e) start = paths.size();
    paths.push_back(Library::join(curDir, x.name));
  }
  player.playList(std::move(paths), start);
  screen = Screen::NowPlaying;
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------
static String fitText(const String& s, int maxW)
{
  if (cv.textWidth(s) <= maxW) return s;
  int lo = 0, hi = s.length();
  while (lo < hi)
  {
    int mid = (lo + hi + 1) / 2;
    if (cv.textWidth(s.substring(0, mid) + "...") <= maxW) lo = mid; else hi = mid - 1;
  }
  String out = s.substring(0, lo);
  out.trim();
  return out + "...";
}

static String fmtTime(float sec)
{
  if (sec < 0) sec = 0;
  uint32_t s = (uint32_t)sec;
  char b[16];
  if (s >= 3600) snprintf(b, sizeof(b), "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
  else snprintf(b, sizeof(b), "%u:%02u", s / 60, s % 60);
  return b;
}

static int batteryLevel = -1;

static void drawPill(int& xr, const char* label, uint16_t fg, uint16_t bgc)
{
  int w = cv.textWidth(label) + 6;
  xr -= w;
  cv.fillRoundRect(xr, 2, w, 10, 3, bgc);
  cv.setTextColor(fg);
  cv.setTextDatum(middle_center);
  cv.drawString(label, xr + w / 2, 7);
  xr -= 3;
}

static void drawHeader(const char* title)
{
  cv.fillRect(0, 0, W, 14, col::panel);
  cv.setFont(&fonts::DejaVu9);
  cv.setTextDatum(middle_left);
  cv.setTextColor(col::dim);
  cv.drawString(title, 5, 7);

  int xr = W - 4;
  // battery
  if (batteryLevel >= 0)
  {
    int bx = xr - 18;
    uint16_t c = batteryLevel <= 15 ? col::warn : col::text;
    cv.drawRoundRect(bx, 3, 16, 8, 2, c);
    cv.fillRect(bx + 16, 5, 2, 4, c);
    cv.fillRect(bx + 2, 5, (12 * batteryLevel) / 100, 4, batteryLevel <= 15 ? col::warn : col::good);
    xr = bx - 3;
    char b[8];
    snprintf(b, sizeof(b), "%d%%", batteryLevel);
    cv.setTextDatum(middle_right);
    cv.setTextColor(col::dim);
    cv.drawString(b, xr, 7);
    xr -= cv.textWidth(b) + 7;
  }
  // volume
  {
    char b[8];
    snprintf(b, sizeof(b), "%d", muted ? 0 : cfg.volume);
    cv.setTextDatum(middle_right);
    cv.setTextColor(muted ? col::warn : col::dim);
    cv.drawString(b, xr, 7);
    xr -= cv.textWidth(b) + 2;
    uint16_t c = muted ? col::warn : col::dim;
    cv.fillRect(xr - 9, 5, 3, 4, c);
    cv.fillTriangle(xr - 7, 7, xr - 3, 2, xr - 3, 12, c);
    xr -= 14;
  }
  // modes
  RepeatMode rep = player.repeat();
  if (rep != RepeatMode::Off) drawPill(xr, rep == RepeatMode::One ? "RPT1" : "RPT", col::bg, col::cool);
  if (player.shuffle())      drawPill(xr, "SHUF", col::bg, col::accent);
}

static void drawHint(const char* s)
{
  cv.setFont(&fonts::DejaVu9);
  cv.setTextDatum(bottom_center);
  cv.setTextColor(col::dim);
  cv.drawString(s, W / 2, H - 1);
}

static void drawCentered(const char* a, const char* b = nullptr, int y = 62)
{
  cv.setTextDatum(middle_center);
  cv.setFont(&fonts::DejaVu12);
  cv.setTextColor(col::text);
  cv.drawString(a, W / 2, y);
  if (b)
  {
    cv.setFont(&fonts::DejaVu9);
    cv.setTextColor(col::dim);
    cv.drawString(b, W / 2, y + 16);
  }
}

static void iconFolder(int x, int y, uint16_t c)
{
  cv.fillRect(x, y + 3, 5, 2, c);
  cv.fillRoundRect(x, y + 4, 12, 8, 1, c);
}

static void iconNote(int x, int y, uint16_t c)
{
  cv.fillCircle(x + 3, y + 10, 2, c);
  cv.drawFastVLine(x + 5, y + 2, 9, c);
  cv.drawLine(x + 5, y + 2, x + 9, y + 4, c);
  cv.drawLine(x + 5, y + 3, x + 9, y + 5, c);
}

static void iconUp(int x, int y, uint16_t c)
{
  cv.fillTriangle(x + 5, y + 2, x, y + 8, x + 10, y + 8, c);
  cv.fillRect(x + 3, y + 8, 5, 4, c);
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------
static TrackInfo info;
static uint32_t infoVer = UINT32_MAX;

static void drawLibrary()
{
  drawHeader("LIBRARY");

  cv.setFont(&fonts::DejaVu9);
  cv.setTextDatum(top_left);
  cv.setTextColor(col::cool);
  String p = foldUtf8(curDir);
  if (cv.textWidth(p) > 228)
  {
    while (p.length() > 1 && cv.textWidth("..." + p) > 228) p.remove(0, 1);
    p = "..." + p;
  }
  cv.drawString(p, 6, 16);

  int n = rowCount();
  if (listFailed) { drawCentered("Can't read the card", "Check it's FAT32 / exFAT"); return; }
  if (n == 0) { drawCentered("No folders or MP3s here"); drawHint("BKSP up  TAB now playing"); return; }

  if (sel < topRow) topRow = sel;
  if (sel >= topRow + kRows) topRow = sel - kRows + 1;

  bool up = hasUpRow();
  cv.setFont(&fonts::DejaVu12);
  for (int i = topRow; i < n && i < topRow + kRows; ++i)
  {
    int y = kListY + (i - topRow) * kRowH;
    bool isSel = (i == sel);
    if (isSel)
    {
      cv.fillRoundRect(2, y, 230, kRowH - 1, 3, col::panel2);
      cv.fillRect(2, y + 3, 2, kRowH - 7, col::accent);
    }
    uint16_t tc = col::text;
    String label;
    if (up && i == 0)
    {
      iconUp(9, y, col::dim);
      label = "..";
      tc = col::dim;
    }
    else
    {
      const LibEntry& e = entries[i - (up ? 1 : 0)];
      if (e.dir) iconFolder(8, y, col::folder);
      else
      {
        bool playing = player.loaded() && info.path == Library::join(curDir, e.name);
        if (playing) tc = col::accent;
        iconNote(9, y, playing ? col::accent : col::dim);
      }
      label = e.label;
    }
    cv.setTextDatum(middle_left);
    cv.setTextColor(tc);
    cv.drawString(fitText(label, 200), 25, y + kRowH / 2);
  }

  if (n > kRows)
  {
    int trackH = kRows * kRowH - 2;
    int th = max(8, trackH * kRows / n);
    int ty = kListY + (trackH - th) * topRow / (n - kRows);
    cv.fillRoundRect(235, kListY, 3, trackH, 1, col::panel);
    cv.fillRoundRect(235, ty, 3, th, 1, col::dim);
  }
  drawHint(listTruncated ? "showing first 600 items" : "ENT play/open  BKSP up  TAB playing");
}

static void drawTransportIcon(int cx, int cy, PlayState st)
{
  cv.fillCircle(cx, cy, 12, col::accent);
  if (st == PlayState::Playing)
  {
    cv.fillRect(cx - 5, cy - 6, 4, 12, col::bg);
    cv.fillRect(cx + 1, cy - 6, 4, 12, col::bg);
  }
  else
  {
    cv.fillTriangle(cx - 4, cy - 7, cx - 4, cy + 7, cx + 7, cy, col::bg);
  }
}

static void drawSkipIcon(int cx, int cy, bool fwd)
{
  int d = fwd ? 1 : -1;
  cv.fillTriangle(cx - 5 * d, cy - 5, cx - 5 * d, cy + 5, cx + 2 * d, cy, col::text);
  cv.fillRect(fwd ? cx + 2 : cx - 4, cy - 5, 2, 11, col::text);
}

static void drawNowPlaying()
{
  drawHeader("NOW PLAYING");
  if (!player.loaded())
  {
    drawCentered("Nothing playing", "TAB to pick some music");
    drawHint("TAB library  E equalizer");
    return;
  }

  // title, scrolling when it doesn't fit
  cv.setFont(&fonts::FreeSansBold9pt7b);
  cv.setTextDatum(top_left);
  cv.setTextColor(col::text);
  const int tx = 8, tw = 224;
  int w = cv.textWidth(info.title);
  cv.setClipRect(tx, 17, tw, 22);
  if (w <= tw) cv.drawString(info.title, tx, 19);
  else
  {
    const int gap = 40, hold = 60;
    int period = w + gap;
    int t = (millis() / 28) % (period + hold);
    int off = t < hold ? 0 : t - hold;
    cv.drawString(info.title, tx - off, 19);
    cv.drawString(info.title, tx - off + period, 19);
  }
  cv.clearClipRect();

  cv.setFont(&fonts::DejaVu12);
  cv.setTextColor(col::accent);
  cv.drawString(fitText(info.artist.length() ? info.artist : String("Unknown artist"), tw), tx, 41);
  cv.setFont(&fonts::DejaVu9);
  cv.setTextColor(col::dim);
  if (info.album.length()) cv.drawString(fitText(info.album, tw), tx, 56);

  // progress
  float el = player.elapsed();
  float du = info.duration;
  if (du > 0 && el > du) el = du;
  const int px = 8, py = 72, pw = 224;
  cv.fillRoundRect(px, py, pw, 4, 2, col::panel2);
  int fill = du > 0 ? (int)(pw * el / du) : 0;
  if (fill > 0) cv.fillRoundRect(px, py, fill, 4, 2, col::accent);
  if (du > 0) cv.fillCircle(px + fill, py + 1, 4, col::text);

  cv.setTextColor(col::dim);
  cv.setTextDatum(top_left);
  cv.drawString(fmtTime(el), px, 81);
  cv.setTextDatum(top_right);
  cv.drawString(du > 0 ? fmtTime(du) : String("--:--"), px + pw, 81);
  cv.setTextDatum(top_center);
  char pos[16];
  snprintf(pos, sizeof(pos), "%d / %d", info.position, info.count);
  cv.drawString(pos, W / 2, 81);

  // transport
  drawSkipIcon(88, 105, false);
  drawTransportIcon(W / 2, 105, player.state());
  drawSkipIcon(152, 105, true);

  // format, left
  cv.setTextDatum(top_left);
  cv.setTextColor(col::dim);
  char fmt[24];
  snprintf(fmt, sizeof(fmt), "%s %uk", info.vbr ? "VBR" : "MP3", info.bitrate);
  cv.drawString(fmt, 8, 96);
  snprintf(fmt, sizeof(fmt), "%.1f kHz %s", info.sampleRate / 1000.0f, info.channels == 1 ? "mono" : "");
  cv.drawString(fmt, 8, 107);

  // EQ, right: three little bars around a centre line
  bool flat = !cfg.eq[0] && !cfg.eq[1] && !cfg.eq[2];
  cv.setTextDatum(top_right);
  cv.drawString(flat ? "EQ flat" : "EQ", 232, 96);
  if (!flat)
  {
    for (int b = 0; b < 3; ++b)
    {
      int x = 206 + b * 9, cy = 112;
      int hgt = cfg.eq[b] * 6 / ThreeBandEQ::kMaxDb;
      cv.drawFastHLine(x - 1, cy, 7, col::panel2);
      if (hgt > 0) cv.fillRect(x, cy - hgt, 5, hgt, col::accent);
      else if (hgt < 0) cv.fillRect(x, cy + 1, 5, -hgt, col::cool);
    }
  }

  drawHint(",/ seek  ;. vol  N/P skip  E eq");
}

static int eqSel = 0;

static void drawEqualizer()
{
  drawHeader("EQUALIZER");
  static const char* names[3] = { "LOW", "MID", "HIGH" };
  static const char* freqs[3] = { "200 Hz", "1 kHz", "4 kHz" };
  const int cx[3] = { 48, 120, 192 };
  const int top = 30, bottom = 90, zero = 60;   // 30 px = 12 dB

  for (int b = 0; b < 3; ++b)
  {
    bool s = (b == eqSel);
    int g = cfg.eq[b];
    if (s) cv.drawRoundRect(cx[b] - 32, 16, 64, 105, 6, col::panel2);

    char v[12];
    snprintf(v, sizeof(v), g > 0 ? "+%d dB" : "%d dB", g);
    cv.setFont(&fonts::DejaVu9);
    cv.setTextDatum(top_center);
    cv.setTextColor(s ? col::accent : col::dim);
    cv.drawString(v, cx[b], 18);

    cv.fillRoundRect(cx[b] - 3, top, 6, bottom - top + 1, 3, col::panel2);
    for (int d = -12; d <= 12; d += 6)
    {
      int ty = zero - d * (zero - top) / 12;
      int len = d == 0 ? 6 : 3;
      cv.drawFastHLine(cx[b] - 6 - len, ty, len, col::dim);
      cv.drawFastHLine(cx[b] + 7, ty, len, col::dim);
    }
    int vy = zero - g * (zero - top) / 12;
    if (vy != zero) cv.fillRect(cx[b] - 3, min(vy, zero), 6, abs(vy - zero) + 1, s ? col::accent : col::cool);
    cv.fillRoundRect(cx[b] - 11, vy - 3, 22, 7, 3, s ? col::accent : col::text);

    cv.setFont(&fonts::DejaVu12);
    cv.setTextColor(s ? col::accent : col::text);
    cv.drawString(names[b], cx[b], 96);
    cv.setFont(&fonts::DejaVu9);
    cv.setTextColor(col::dim);
    cv.drawString(freqs[b], cx[b], 109);
  }
  drawHint(",/ band  ;. adjust  0 reset  F flat");
}

static void drawToast()
{
  if ((int32_t)(millis() - toastUntil) >= 0) return;
  int w = 150, h = toastBar >= 0 ? 36 : 26;
  int x = (W - w) / 2, y = (H - h) / 2 + 6;
  cv.fillRoundRect(x, y, w, h, 6, col::panel);
  cv.drawRoundRect(x, y, w, h, 6, col::accent);
  cv.setFont(&fonts::DejaVu12);
  cv.setTextDatum(top_center);
  cv.setTextColor(col::text);
  cv.drawString(toastText, W / 2, y + 6);
  if (toastBar >= 0)
  {
    int bw = w - 24;
    cv.fillRoundRect(x + 12, y + h - 11, bw, 4, 2, col::panel2);
    cv.fillRoundRect(x + 12, y + h - 11, bw * toastBar / 100, 4, 2, col::accent);
  }
}

static void render()
{
  cv.fillScreen(col::bg);
  switch (screen)
  {
    case Screen::Library:    drawLibrary(); break;
    case Screen::NowPlaying: drawNowPlaying(); break;
    case Screen::Equalizer:  drawEqualizer(); break;
  }
  drawToast();
  cv.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
enum : uint8_t { K_UP = 0x80, K_DOWN, K_LEFT, K_RIGHT, K_ENTER, K_BKSP, K_TAB, K_ESC };

static uint8_t normKey(char c)
{
  switch (c)
  {
    case ';': case ':': return K_UP;        // the arrow keys, with or without Fn
    case '.': case '>': return K_DOWN;
    case ',': case '<': return K_LEFT;
    case '/': case '?': return K_RIGHT;
    case '`': case '~': return K_ESC;
    case '+': return '=';
    case '_': return '-';
    case ')': return '0';
  }
  return (uint8_t)tolower((unsigned char)c);
}

static void readKeys(std::bitset<256>& now)
{
  auto& ks = M5Cardputer.Keyboard.keysState();
  for (char c : ks.word) now.set(normKey(c));
  if (ks.enter) now.set(K_ENTER);
  if (ks.backspace || ks.del) now.set(K_BKSP);
  if (ks.tab) now.set(K_TAB);
  if (ks.esc) now.set(K_ESC);
  if (ks.up) now.set(K_UP);
  if (ks.down) now.set(K_DOWN);
  if (ks.left) now.set(K_LEFT);
  if (ks.right) now.set(K_RIGHT);
}

static bool repeatable(uint8_t k)
{
  return k == K_UP || k == K_DOWN || k == K_LEFT || k == K_RIGHT || k == '=' || k == '-';
}

static void changeVolume(int d)
{
  muted = false;
  cfg.volume = constrain(cfg.volume + d, 0, 100);
  applyVolume();
  toast(String("Volume ") + cfg.volume + "%", cfg.volume, 900);
  markDirty();
}

static void backFromEq() { screen = player.loaded() ? Screen::NowPlaying : Screen::Library; }

static void onKey(uint8_t k, bool rep)
{
  // ---- everywhere ----
  switch (k)
  {
    case ' ':
      if (rep) return;
      if (!player.loaded() && screen == Screen::Library) activateSelection();
      else player.togglePause();
      return;
    case 'n': if (!rep) player.next(); return;
    case 'p': if (!rep) player.prev(); return;
    case '=': changeVolume(+5); return;
    case '-': changeVolume(-5); return;
    case 'm':
      if (rep) return;
      muted = !muted;
      applyVolume();
      toast(muted ? "Muted" : "Unmuted");
      return;
    case 's':
      if (rep) return;
      cfg.shuffle = !cfg.shuffle;
      player.setShuffle(cfg.shuffle);
      toast(cfg.shuffle ? "Shuffle on" : "Shuffle off");
      markDirty();
      return;
    case 'r':
    {
      if (rep) return;
      cfg.repeat = (cfg.repeat + 1) % 3;
      player.setRepeat((RepeatMode)cfg.repeat);
      static const char* names[3] = { "Repeat off", "Repeat all", "Repeat one" };
      toast(names[cfg.repeat]);
      markDirty();
      return;
    }
    case 'e':
      if (rep) return;
      if (screen == Screen::Equalizer) backFromEq(); else screen = Screen::Equalizer;
      return;
    case 'l': if (!rep) screen = Screen::Library; return;
    case K_TAB:
      if (rep) return;
      screen = (screen == Screen::Library) ? Screen::NowPlaying : Screen::Library;
      return;
    case 'd': if (!rep) setScreen(false); return;
    case 'b':
      if (rep) return;
      cfg.brightness = (cfg.brightness + 1) % 4;
      setScreen(true);
      toast("Brightness", kBrightness[cfg.brightness] * 100 / 255, 900);
      markDirty();
      return;
  }

  // ---- per screen ----
  if (screen == Screen::Library)
  {
    int n = rowCount();
    switch (k)
    {
      case K_UP:    if (n) { if (sel > 0) --sel; else if (!rep) sel = n - 1; } break;
      case K_DOWN:  if (n) { if (sel < n - 1) ++sel; else if (!rep) sel = 0; } break;
      case K_LEFT:  sel = max(0, sel - kRows); break;
      case K_RIGHT: sel = max(0, min(n - 1, sel + kRows)); break;
      case K_ENTER: if (!rep) activateSelection(); break;
      case K_BKSP:  if (!rep) goUp(); break;
      case K_ESC:   if (!rep && player.loaded()) screen = Screen::NowPlaying; break;
    }
  }
  else if (screen == Screen::NowPlaying)
  {
    switch (k)
    {
      case K_UP:    changeVolume(+5); break;
      case K_DOWN:  changeVolume(-5); break;
      case K_LEFT:  player.seekBy(-10); break;
      case K_RIGHT: player.seekBy(+10); break;
      case K_ENTER: if (!rep) player.togglePause(); break;
      case K_BKSP:
      case K_ESC:   if (!rep) screen = Screen::Library; break;
    }
  }
  else  // Equalizer
  {
    switch (k)
    {
      case K_LEFT:  if (eqSel > 0) --eqSel; break;
      case K_RIGHT: if (eqSel < 2) ++eqSel; break;
      case K_UP:
      case K_DOWN:
        cfg.eq[eqSel] = constrain(cfg.eq[eqSel] + (k == K_UP ? 1 : -1), -ThreeBandEQ::kMaxDb, ThreeBandEQ::kMaxDb);
        applyEq();
        markDirty();
        break;
      case '0':
        if (rep) break;
        cfg.eq[eqSel] = 0;
        applyEq();
        markDirty();
        break;
      case 'f':
        if (rep) break;
        cfg.eq[0] = cfg.eq[1] = cfg.eq[2] = 0;
        applyEq();
        markDirty();
        toast("EQ flat");
        break;
      case K_ENTER:
      case K_BKSP:
      case K_ESC:   if (!rep) backFromEq(); break;
    }
  }
}

static std::bitset<256> prevKeys;
static uint32_t pressedAt[256];
static uint32_t lastFire[256];

static void pollInput()
{
  std::bitset<256> now;
  readKeys(now);
  uint32_t t = millis();
  bool any = false;
  for (int k = 0; k < 256; ++k)
  {
    if (!now[k]) continue;
    if (!prevKeys[k])
    {
      pressedAt[k] = lastFire[k] = t;
      any = true;
      if (screenOn) onKey(k, false);
    }
    else if (screenOn && repeatable(k) && t - pressedAt[k] > 400 && t - lastFire[k] >= 75)
    {
      lastFire[k] = t;
      onKey(k, true);
    }
  }
  if (any)
  {
    lastInput = t;
    if (!screenOn) setScreen(true);   // first key only wakes the screen
  }
  prevKeys = now;
}

// ---------------------------------------------------------------------------
// SD card
// ---------------------------------------------------------------------------
static bool mountSD()
{
  int sck  = M5.getPin(m5::pin_name_t::sd_spi_sclk);
  int miso = M5.getPin(m5::pin_name_t::sd_spi_miso);
  int mosi = M5.getPin(m5::pin_name_t::sd_spi_mosi);
  int cs   = M5.getPin(m5::pin_name_t::sd_spi_cs);
  if (sck < 0 || sck > 48)   sck = 40;    // Cardputer / ADV wiring
  if (miso < 0 || miso > 48) miso = 39;
  if (mosi < 0 || mosi > 48) mosi = 14;
  if (cs < 0 || cs > 48)     cs = 12;
  SPI.begin(sck, miso, mosi, cs);
  if (SD.begin(cs, SPI, 25000000)) return true;
  SD.end();
  return SD.begin(cs, SPI, 10000000);
}

static void splash(const char* a, const char* b)
{
  cv.fillScreen(col::bg);
  cv.setFont(&fonts::FreeSansBold9pt7b);
  cv.setTextDatum(middle_center);
  cv.setTextColor(col::accent);
  cv.drawString("Cardputer MP3", W / 2, 34);
  drawCentered(a, b, 70);
  cv.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// Arduino
// ---------------------------------------------------------------------------
void setup()
{
  auto m5cfg = M5.config();
  M5Cardputer.begin(m5cfg, true);
  M5Cardputer.Display.setRotation(1);

  cv.setColorDepth(16);
  if (!cv.createSprite(W, H)) { cv.setColorDepth(8); cv.createSprite(W, H); }

  loadSettings();
  setScreen(true);

  M5.Speaker.begin();
  applyVolume();

  splash("Mounting SD card...", nullptr);
  while (!mountSD())
  {
    splash("No microSD card", "Insert a FAT32 card with MP3s");
    delay(1000);
  }

  if (!player.begin(&SD))
  {
    splash("Audio start failed", "Not enough memory?");
    for (;;) delay(1000);
  }
  player.setShuffle(cfg.shuffle);
  player.setRepeat((RepeatMode)cfg.repeat);
  applyEq();

  openDir(cfg.dir.length() ? cfg.dir : String("/"));
  batteryLevel = M5.Power.getBatteryLevel();
  lastInput = millis();
}

void loop()
{
  M5Cardputer.update();

  if (M5Cardputer.BtnA.wasPressed())   // G0 button: play / pause, screen on or off
  {
    lastInput = millis();
    player.togglePause();
  }
  pollInput();

  uint32_t v = player.infoVersion();
  if (v != infoVer) { infoVer = v; player.getInfo(info); }

  uint32_t now = millis();
  if (screenOn && player.state() == PlayState::Playing && now - lastInput > kAutoOffMs) setScreen(false);

  static uint32_t lastBatt = 0;
  if (now - lastBatt > 5000) { lastBatt = now; batteryLevel = M5.Power.getBatteryLevel(); }

  if (saveDue && (int32_t)(now - saveDue) >= 0) { saveDue = 0; saveSettings(); }

  static uint32_t lastDraw = 0;
  if (screenOn && now - lastDraw >= 40) { lastDraw = now; render(); }

  delay(5);
}
