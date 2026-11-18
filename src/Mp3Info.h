// Mp3Info.h — reads what the player needs up front from an MP3 file:
// ID3v2/ID3v1 title, artist and album, where the audio starts and ends, and
// the duration (Xing/Info or VBRI frame count for VBR, bitrate for CBR), plus
// the Xing seek table when there is one.
#pragma once

#include <Arduino.h>
#include <FS.h>

struct Mp3Info
{
  String title, artist, album;   // ASCII-folded for the display font

  uint32_t fileSize   = 0;
  uint32_t audioStart = 0;       // first MPEG frame
  uint32_t audioEnd   = 0;       // before any ID3v1 tag
  uint32_t sampleRate = 0;
  uint16_t bitrate    = 0;       // kbps (first frame, or average for VBR)
  uint8_t  channels   = 0;
  bool     vbr        = false;
  float    duration   = 0;       // seconds, 0 if unknown

  bool parse(fs::File& f);

  /// Byte offset to start decoding from for a time in seconds.
  uint32_t offsetFor(float seconds) const;

private:
  bool     _hasToc = false;
  uint8_t  _toc[100];
  uint32_t _span = 0;            // bytes the seek table / ratio maps over

  bool parseId3v2(fs::File& f, uint8_t ver, uint8_t flags, uint32_t end);
  bool findFirstFrame(fs::File& f);
};

/// Append a Unicode code point to s, folded to something the built-in fonts
/// can draw (accents dropped, smart quotes straightened, the rest -> '?').
void appendFolded(String& s, uint32_t cp);

/// UTF-8 in, folded ASCII out (for FAT long file names).
String foldUtf8(const String& in);
