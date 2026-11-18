// Library.h — SD card folder listing for the browser.
#pragma once

#include <Arduino.h>
#include <FS.h>
#include <vector>

struct LibEntry
{
  String name;      // as on the card (UTF-8)
  String label;     // folded for display
  bool   dir;
};

namespace Library
{
  static constexpr size_t kMaxEntries = 600;

  /// Folders first, then .mp3 files, natural order ("2 x" before "10 x").
  /// Hidden entries (".", "._foo", "System Volume Information") are skipped.
  /// Returns false if the path can't be opened. truncated is set when the
  /// folder had more than kMaxEntries.
  bool list(fs::FS& fs, const String& path, std::vector<LibEntry>& out, bool& truncated);

  bool isMp3(const String& name);
  String join(const String& dir, const String& name);
  String parent(const String& path);
  String baseName(const String& path);
}
