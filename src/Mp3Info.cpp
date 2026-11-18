#include "Mp3Info.h"

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------
static const char kLatin1Fold[] =  // U+00C0 .. U+00FF
  "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs"
  "aaaaaaaceeeeiiiidnooooo/ouuuuyty";

void appendFolded(String& s, uint32_t cp)
{
  if (cp < 0x20) { if (cp == '\t') s += ' '; return; }
  if (cp < 0x7F) { s += (char)cp; return; }
  if (cp >= 0xC0 && cp <= 0xFF) { s += kLatin1Fold[cp - 0xC0]; return; }
  switch (cp)
  {
    case 0xA0: s += ' '; return;
    case 0xFEFF: return;                                    // stray BOM
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: s += '\''; return;
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: s += '"'; return;
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: s += '-'; return;
    case 0x2026: s += "..."; return;
    case 0x0152: s += "OE"; return;
    case 0x0153: s += "oe"; return;
    default: s += '?'; return;
  }
}

static void decodeUtf8(const uint8_t* p, size_t n, String& out)
{
  size_t i = 0;
  while (i < n)
  {
    uint8_t c = p[i];
    if (c == 0) break;
    uint32_t cp; int extra;
    if (c < 0x80)                { cp = c;        extra = 0; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { out += '?'; ++i; continue; }
    if (i + extra >= n) break;
    bool ok = true;
    for (int k = 1; k <= extra; ++k)
    {
      uint8_t cc = p[i + k];
      if ((cc & 0xC0) != 0x80) { ok = false; break; }
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (!ok) { out += '?'; ++i; continue; }
    appendFolded(out, cp);
    i += 1 + extra;
  }
}

String foldUtf8(const String& in)
{
  String out;
  out.reserve(in.length());
  decodeUtf8((const uint8_t*)in.c_str(), in.length(), out);
  return out;
}

/// ID3v2 text frame body: encoding byte, then the text.
static String decodeId3Text(const uint8_t* d, size_t n)
{
  String out;
  if (n < 2) return out;
  uint8_t enc = d[0];
  ++d; --n;
  if (enc == 0)          // ISO-8859-1
  {
    for (size_t i = 0; i < n && d[i]; ++i) appendFolded(out, d[i]);
  }
  else if (enc == 3)     // UTF-8
  {
    decodeUtf8(d, n, out);
  }
  else if (enc == 1 || enc == 2)   // UTF-16 with BOM / UTF-16BE
  {
    bool be = (enc == 2);
    size_t i = 0;
    if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE) { be = false; i = 2; }
    else if (n >= 2 && d[0] == 0xFE && d[1] == 0xFF) { be = true; i = 2; }
    while (i + 1 < n)
    {
      uint32_t u = be ? (d[i] << 8 | d[i + 1]) : (d[i] | d[i + 1] << 8);
      i += 2;
      if (u == 0) break;
      if (u >= 0xD800 && u <= 0xDBFF && i + 1 < n)
      {
        uint32_t lo = be ? (d[i] << 8 | d[i + 1]) : (d[i] | d[i + 1] << 8);
        if (lo >= 0xDC00 && lo <= 0xDFFF) { u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
      }
      appendFolded(out, u);
    }
  }
  out.trim();
  return out;
}

static String id3v1Field(const uint8_t* p, size_t n)
{
  String out;
  for (size_t i = 0; i < n && p[i]; ++i) appendFolded(out, p[i]);
  out.trim();
  return out;
}

// ---------------------------------------------------------------------------
// Bytes
// ---------------------------------------------------------------------------
static uint32_t be32(const uint8_t* b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
static uint32_t le32(const uint8_t* b) { return (uint32_t)b[3] << 24 | (uint32_t)b[2] << 16 | (uint32_t)b[1] << 8 | b[0]; }
static uint32_t syncsafe(const uint8_t* b)
{
  return (uint32_t)(b[0] & 0x7F) << 21 | (uint32_t)(b[1] & 0x7F) << 14 | (uint32_t)(b[2] & 0x7F) << 7 | (b[3] & 0x7F);
}

// ---------------------------------------------------------------------------
// MPEG frame header
// ---------------------------------------------------------------------------
static const uint16_t kBitrate[2][3][15] = {
  { // MPEG-1: layer I, II, III
    {0,32,64,96,128,160,192,224,256,288,320,352,384,416,448},
    {0,32,48,56, 64, 80, 96,112,128,160,192,224,256,320,384},
    {0,32,40,48, 56, 64, 80, 96,112,128,160,192,224,256,320} },
  { // MPEG-2 / 2.5: layer I, II, III
    {0,32,48,56,64,80,96,112,128,144,160,176,192,224,256},
    {0, 8,16,24,32,40,48, 56, 64, 80, 96,112,128,144,160},
    {0, 8,16,24,32,40,48, 56, 64, 80, 96,112,128,144,160} },
};
static const uint32_t kSampleRate[3] = { 44100, 48000, 32000 };

struct FrameHdr
{
  int ver;        // 0 = MPEG-1, 1 = MPEG-2, 2 = MPEG-2.5
  int layer;      // 1..3
  uint32_t kbps, rate, len, spf;
  bool mono;
};

static bool decodeHeader(const uint8_t* b, FrameHdr& h)
{
  if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0) return false;
  int vb = (b[1] >> 3) & 3;  if (vb == 1) return false;
  int lb = (b[1] >> 1) & 3;  if (lb == 0) return false;
  int bri = b[2] >> 4;       if (bri == 0 || bri == 15) return false;
  int sri = (b[2] >> 2) & 3; if (sri == 3) return false;
  h.ver   = (vb == 3) ? 0 : (vb == 2 ? 1 : 2);
  h.layer = 4 - lb;
  h.kbps  = kBitrate[h.ver ? 1 : 0][h.layer - 1][bri];
  h.rate  = kSampleRate[sri] >> h.ver;
  int pad = (b[2] >> 1) & 1;
  h.mono  = ((b[3] >> 6) & 3) == 3;
  if (h.layer == 1)                    { h.spf = 384;  h.len = (12 * h.kbps * 1000 / h.rate + pad) * 4; }
  else if (h.layer == 2 || h.ver == 0) { h.spf = 1152; h.len = 144 * h.kbps * 1000 / h.rate + pad; }
  else                                 { h.spf = 576;  h.len = 72 * h.kbps * 1000 / h.rate + pad; }
  return h.len >= 21;
}

// ---------------------------------------------------------------------------
// Mp3Info
// ---------------------------------------------------------------------------
bool Mp3Info::parseId3v2(fs::File& f, uint8_t ver, uint8_t flags, uint32_t end)
{
  uint32_t pos = 10;
  if (flags & 0x40)    // extended header
  {
    uint8_t e[4];
    f.seek(pos);
    if (f.read(e, 4) != 4) return false;
    pos += (ver == 4) ? syncsafe(e) : 4 + be32(e);
  }
  const uint32_t hdrLen = (ver == 2) ? 6 : 10;
  String albumArtist;
  uint8_t fh[10];
  uint8_t body[256];

  while (pos + hdrLen <= end)
  {
    f.seek(pos);
    if (f.read(fh, hdrLen) != hdrLen) break;
    if (fh[0] == 0) break;                          // padding
    char id[5] = {0};
    uint32_t size;
    uint8_t fmt = 0;
    if (ver == 2) { memcpy(id, fh, 3); size = (uint32_t)fh[3] << 16 | fh[4] << 8 | fh[5]; }
    else          { memcpy(id, fh, 4); size = (ver == 4) ? syncsafe(fh + 4) : be32(fh + 4); fmt = fh[9]; }
    pos += hdrLen;
    if (size == 0 || pos + size > end) break;

    String* dst = nullptr;
    if      (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) dst = &title;
    else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) dst = &artist;
    else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) dst = &album;
    else if (!strcmp(id, "TPE2") || !strcmp(id, "TP2")) dst = &albumArtist;

    bool usable = true;
    uint32_t skip = 0;
    if (ver == 3 && (fmt & 0xC0)) usable = false;   // compressed / encrypted
    if (ver == 4)
    {
      if (fmt & 0x0E) usable = false;               // compressed / encrypted / unsynchronised
      if (fmt & 0x01) skip = 4;                     // data length indicator
    }
    if (dst && usable && dst->isEmpty() && size > skip)
    {
      uint32_t n = size - skip;
      if (n > sizeof(body)) n = sizeof(body);
      f.seek(pos + skip);
      n = f.read(body, n);
      *dst = decodeId3Text(body, n);
    }
    pos += size;
    if (!title.isEmpty() && !artist.isEmpty() && !album.isEmpty()) break;
  }
  if (artist.isEmpty()) artist = albumArtist;
  return true;
}

bool Mp3Info::findFirstFrame(fs::File& f)
{
  static constexpr uint32_t kChunk = 2048;
  static constexpr uint32_t kMaxScan = 512 * 1024;
  static uint8_t buf[kChunk + 4];    // only the audio task parses

  uint32_t limit = audioEnd;
  if (limit > audioStart + kMaxScan) limit = audioStart + kMaxScan;

  for (uint32_t base = audioStart; base + 4 <= limit; base += kChunk)
  {
    f.seek(base);
    size_t n = f.read(buf, kChunk + 4);
    if (n < 4) break;
    for (size_t i = 0; i + 4 <= n; ++i)
    {
      if (buf[i] != 0xFF) continue;
      FrameHdr h;
      if (!decodeHeader(buf + i, h)) continue;
      uint32_t pos = base + i;
      uint32_t next = pos + h.len;
      if (next + 4 <= audioEnd)    // a real frame is followed by another like it
      {
        uint8_t nb[4];
        FrameHdr h2;
        f.seek(next);
        if (f.read(nb, 4) != 4 || !decodeHeader(nb, h2)
            || h2.ver != h.ver || h2.layer != h.layer || h2.rate != h.rate) continue;
      }

      audioStart = pos;
      sampleRate = h.rate;
      channels   = h.mono ? 1 : 2;
      bitrate    = h.kbps;
      _span      = audioEnd - audioStart;

      // Xing / Info (LAME) or VBRI header in the first frame
      uint8_t fr[200];
      f.seek(pos);
      size_t fn = f.read(fr, sizeof(fr));
      uint32_t xo = 4 + (h.ver == 0 ? (h.mono ? 17 : 32) : (h.mono ? 9 : 17));
      uint32_t frames = 0, bytes = 0;
      if (xo + 8 <= fn && (!memcmp(fr + xo, "Xing", 4) || !memcmp(fr + xo, "Info", 4)))
      {
        vbr = (fr[xo] == 'X');
        uint32_t fl = be32(fr + xo + 4);
        uint32_t p = xo + 8;
        if ((fl & 1) && p + 4 <= fn)   { frames = be32(fr + p); p += 4; }
        if ((fl & 2) && p + 4 <= fn)   { bytes  = be32(fr + p); p += 4; }
        if ((fl & 4) && p + 100 <= fn) { memcpy(_toc, fr + p, 100); _hasToc = true; }
      }
      else if (36 + 18 <= fn && !memcmp(fr + 36, "VBRI", 4))
      {
        vbr = true;
        bytes  = be32(fr + 36 + 10);
        frames = be32(fr + 36 + 14);
      }
      if (bytes && bytes <= _span) _span = bytes;

      if (frames)
      {
        duration = (float)frames * h.spf / h.rate;
        if (duration > 0) bitrate = (uint16_t)(_span * 8.0f / duration / 1000.0f + 0.5f);   // average; the Info frame itself can use a higher rate
      }
      else if (h.kbps)
      {
        duration = (audioEnd - audioStart) * 8.0f / (h.kbps * 1000.0f);
      }
      return true;
    }
  }
  return false;
}

bool Mp3Info::parse(fs::File& f)
{
  *this = Mp3Info();
  fileSize = f.size();
  if (fileSize < 128) return false;
  audioStart = 0;
  audioEnd = fileSize;

  uint8_t h[10];
  f.seek(0);
  if (f.read(h, 10) == 10 && h[0] == 'I' && h[1] == 'D' && h[2] == '3' && h[3] >= 2 && h[3] <= 4)
  {
    uint32_t tagSize = syncsafe(h + 6);
    uint32_t tagEnd = 10 + tagSize + ((h[5] & 0x10) ? 10 : 0);
    if (tagEnd < fileSize)
    {
      audioStart = tagEnd;
      if (!(h[5] & 0x80)) parseId3v2(f, h[3], h[5], 10 + tagSize);   // skip whole-tag unsync
    }
  }

  // ID3v1 at the very end
  uint8_t t[128];
  f.seek(fileSize - 128);
  if (f.read(t, 128) == 128 && t[0] == 'T' && t[1] == 'A' && t[2] == 'G')
  {
    audioEnd = fileSize - 128;
    if (title.isEmpty())  title  = id3v1Field(t + 3, 30);
    if (artist.isEmpty()) artist = id3v1Field(t + 33, 30);
    if (album.isEmpty())  album  = id3v1Field(t + 63, 30);
  }

  // APEv2 tag before that
  if (audioEnd >= audioStart + 32)
  {
    uint8_t a[32];
    f.seek(audioEnd - 32);
    if (f.read(a, 32) == 32 && !memcmp(a, "APETAGEX", 8))
    {
      uint32_t total = le32(a + 12) + ((le32(a + 20) & 0x80000000u) ? 32 : 0);
      if (total < audioEnd - audioStart) audioEnd -= total;
    }
  }

  return findFirstFrame(f);
}

uint32_t Mp3Info::offsetFor(float seconds) const
{
  if (seconds <= 0 || duration <= 0) return audioStart;
  float frac = seconds / duration;
  if (frac > 0.995f) frac = 0.995f;
  float pos = frac;
  if (_hasToc)
  {
    float pct = frac * 100.0f;
    int a = (int)pct;
    if (a > 99) a = 99;
    float fa = _toc[a];
    float fb = (a < 99) ? _toc[a + 1] : 256.0f;
    pos = (fa + (fb - fa) * (pct - a)) / 256.0f;
  }
  uint32_t off = audioStart + (uint32_t)(pos * _span);
  return off < audioEnd ? off : audioEnd - 1;
}
