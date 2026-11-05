# Cardputer MP3

An MP3 player firmware for the M5Stack Cardputer ADV. It plays `.mp3` files
from the microSD card through the built-in speaker or the 3.5 mm headphone
jack, with a folder browser, shuffle and repeat, seeking, and a 3-band
equaliser.

<p align="center"> <img src="https://github.com/user-attachments/assets/fbefc593-042a-42ab-9c6d-a4289ba03ddc" alt="IMG_20261105_232914595" width="320"> </p>

## Features

- Folder browser for the whole SD card, folders first, natural sort order
  (track 2 comes before track 10)
- Plays every MP3 in a folder, starting from the track you pick
- Shuffle, repeat all, repeat one
- Seek 10 seconds back or forward, hold the key to scrub
- 3-band equaliser (low, mid, high), +/- 12 dB per band
- Title, artist and album from ID3v2.2, 2.3, 2.4 and ID3v1 tags
- Accurate duration and seeking for both CBR and VBR files
- Speaker or headphones (plugging into the 3.5 mm jack mutes the speaker in
  hardware)
- Screen turns off after 1 minute without input while playing, to save battery
- Volume, EQ, shuffle, repeat, brightness and the last folder are remembered
  across power cycles

## Hardware

- M5Stack Cardputer ADV (StampS3A, ESP32-S3FN8, ES8311 audio codec, TCA8418
  keyboard controller)
- A microSD card formatted FAT32 or exFAT

The libraries it uses also support the original Cardputer, so it may run
there too, but it has not been tried on one.

## Preparing the SD card

Copy MP3 files onto the card in any folder layout you like, for example:

```
/Music/Artist/Album/01 - First Song.mp3
/Music/Artist/Album/02 - Second Song.mp3
/Podcasts/episode-12.mp3
```

- Only files ending in `.mp3` are listed.
- Hidden files and folders (names starting with `.`, such as the `._` files
  macOS leaves behind) and `System Volume Information` are skipped.
- A folder shows at most 600 items.
- Files without tags show their file name instead.

## Controls

The arrow keys work with or without Fn: `;` up, `.` down, `,` left, `/` right.

### Anywhere

| Key | Action |
|---|---|
| `Space` or the G0 button | Play / pause |
| `N` | Next track |
| `P` | Previous track (restarts the current track if it has played for more than 3 seconds) |
| `=` / `-` | Volume up / down in 5 % steps |
| `M` | Mute / unmute |
| `S` | Shuffle on / off |
| `R` | Cycle repeat mode: all, one, off |
| `Tab` | Switch between Library and Now Playing |
| `E` | Open / close the equaliser |
| `L` | Library |
| `B` | Cycle screen brightness (4 levels) |
| `D` | Turn the screen off |

When the screen is off, the first key press only turns it back on. The G0
button still plays and pauses with the screen off.

### Library

| Key | Action |
|---|---|
| `;` / `.` | Move up / down |
| `,` / `/` | Page up / down |
| `Enter` | Open the folder, or play the folder starting from this track |
| `Backspace` | Go up one folder |
| `` ` `` (Esc) | Back to Now Playing |

### Now Playing

| Key | Action |
|---|---|
| `;` / `.` | Volume up / down |
| `,` / `/` | Seek back / forward 10 seconds (hold to keep seeking) |
| `Enter` | Play / pause |
| `Backspace` | Library |

### Equaliser

| Key | Action |
|---|---|
| `,` / `/` | Select band |
| `;` / `.` | Raise / lower the selected band by 1 dB |
| `0` | Reset the selected band to 0 dB |
| `F` | Reset all bands to flat |
| `Enter`, `Backspace` or `` ` `` | Back |

## Playback behaviour

- With repeat off, playback stops after the last track. `Space` then starts
  the folder again from the top.
- With repeat all and shuffle on, each pass through the folder gets a new
  random order, and the song that just finished is never played first.
- Tracks that cannot be read are skipped automatically.
- Changing shuffle while playing keeps the current track and reorders the
  rest.

## Equaliser

| Band | Filter | Centre / corner | Range |
|---|---|---|---|
| Low | Low shelf | 200 Hz | +/- 12 dB |
| Mid | Peak (Q 0.7) | 1 kHz | +/- 12 dB |
| High | High shelf | 4 kHz | +/- 12 dB |

When any band is boosted, the whole signal is turned down by the size of the
largest boost before filtering. This keeps loud passages from clipping, so a
boost changes the tone rather than adding distortion. It also means the
overall level drops as you boost, so turn the volume up to compensate.

With every band at 0 dB the equaliser is bypassed entirely.

## Installing

Either:

Save the BIN to the Downloads folder to install via Launcher, or
Flash the full image without using Launcher.
<p align="center"> <img src="https://github.com/user-attachments/assets/fbefc593-042a-42ab-9c6d-a4289ba03ddc" alt="IMG_20261105_232914595" width="320"> <img src="https://github.com/user-attachments/assets/637206fc-8b0d-4a3e-ab58-865da29e83a9" alt="IMG_20261105_232923801" width="320"> </p> <p align="center"> <img src="https://github.com/user-attachments/assets/96a574ef-d261-40c1-88b3-e79774ffa823" alt="IMG_20261105_232931284" width="180"> <img src="https://github.com/user-attachments/assets/f1452105-da92-45fa-bda8-2b10d25eeb43" alt="IMG_20261105_232937334" width="180"> <img src="https://github.com/user-attachments/assets/c594b73a-87a5-47e5-9cc3-661611f4496e" alt="IMG_20261105_232945418" width="180"> </p>

## Troubleshooting

**"No microSD card" stays on screen**
Check the card is pushed fully in and formatted FAT32 or exFAT.

**"Can't read the card"**
The card mounted but the folder could not be listed. Reformat the card or
try another.

**A file does not show up**
Make sure the name ends in `.mp3` and does not start with a dot, and that
the folder has fewer than 600 items.

**No sound**
Check the volume and mute (`M`) in the header bar. If headphones are plugged
in, the speaker is switched off by the hardware.

**Titles show `?` characters**
Tag and file name text is shown with the built-in fonts, which only cover
basic Latin characters. Accented letters are shown without their accents;
other scripts appear as `?`.

## Limitations

- MP3 only. WAV, FLAC and AAC files are not listed.
- The display fonts cover basic Latin characters only.
- The play queue is the current folder; there is no playlist file support.
- Elapsed time after a seek in a VBR file without a seek table is an
  estimate.

## Licence

ESP8266Audio is licensed under GPL-3.0, so any binary built from this
project is covered by GPL-3.0 if you distribute it. M5GFX, M5Unified and
M5Cardputer are MIT licensed.
