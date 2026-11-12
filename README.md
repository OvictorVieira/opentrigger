# OpenTrigger

Open-source drum trigger plug-in (VST3, Audio Unit, Standalone). It listens to a recorded or live drum track, detects each
hit and plays a sampled instrument in its place, layered with the original sound.

Status: beta (`0.1.0-beta.1`). macOS and Windows installers are published on the Releases page.

## What it does

- 8 slots, each with its own volume, pan, tune, mute, solo and polarity.
- Detection with threshold, sensitivity, detail, suppress, retrigger, low/high cut and gate; fixed 192-sample look-ahead
  (reported to the host as plug-in latency).
- Smart Trigger: listens to a stretch of the track and proposes the detection settings; nothing changes until you apply it.
- Stroke Type menu: choose how the piece is hit (Hit, Cross Stick, ...) and a VeloSplit type that switches style by velocity.
- Instrument browser with pictures, library filter, hear-before-you-load preview.
- Mixer per piece: faders, pan, mute, solo and polarity for each mic, and seven extra stereo outputs so each mic can go to
  its own track in the DAW from a single plug-in instance (see "Mixer and multi-output").
- Own instrument format, `.otci` (FLAC audio, cover image, velocity layers, round robins, several mic positions).
- Protected packs: `.otpack` bundles instruments with the audio encrypted (XChaCha20-Poly1305), checksum and tamper
  checks on import. See "Protected audio" below.

## Mixer and multi-output

Choose a kit piece, open **MIXER** in the slot panel: every mic of that piece (for the chosen stroke type) has a channel
strip with fader, pan, mute, solo, polarity (PHASE) and an output. A mic at the bottom of its fader is off and costs no
memory. **MIXED** sets the mic levels so that together they sound like the Mixed sound (calibrated against that piece's Full
Mix); **FULL MIX** goes back to the single mixed sample.

Each strip can go to Main or to one of seven extra stereo outputs ("Out 2" to "Out 8"), so each mic can be recorded on its own
track without duplicating the plug-in. Enable the extra outputs in the DAW: in REAPER give the track more channels (for
example 16), open the plug-in's routing (pin connector) and map Out 2, Out 3 ... to track channels. An output the DAW has not
enabled plays on Main, so nothing goes silent. Outputs carry only the sampled sound (no dry signal) at the Output Volume.
All mics of a hit use the same velocity layer and take, so their phase stays as recorded.

## Instrument libraries

**This repository contains no sample libraries.** OpenTrigger plays `.otci` instruments you place in
`Documents/OpenTrigger/Library/` (the Browser has Import and Open Folder buttons). Make your own with
`tools/otci/otci.py` (WAV folder to `.otci`). Only convert and share audio you have the right to use.

## Protected audio

Packs can be written in two modes:

- **Sealed** (no pass phrase): the audio is encrypted on disk and cannot be opened with ordinary audio tools, but the
  key is derived from a constant in this open-source code. It stops casual copying; it is not a secret.
- **Pass phrase**: the key is derived with Argon2id from your phrase. It is as strong as the phrase.

## Build

Requires CMake 3.22+ and a C++17 compiler (Xcode on macOS, Visual Studio 2022 on Windows). JUCE 8.0.4 is fetched by CMake.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Tests: `ot_tests`, `ot_guitest`, `ot_hosttest` (targets built from `tests/`). Command-line pack tool: `ot_pack`.

Installers: `installer/mac/build_pkg.sh` (macOS `.pkg`) and `installer/windows/OpenTrigger.iss` (Inno Setup); the
`Build installers` GitHub workflow builds both.

macOS builds are ad-hoc signed and not notarized. If macOS blocks the plug-in: `xattr -cr /Library/Audio/Plug-Ins/VST3/OpenTrigger.vst3`.

## License

GNU Affero General Public License v3.0 (see `LICENSE`). OpenTrigger is built on [JUCE](https://juce.com), used under
its AGPLv3 option, so the whole plug-in is distributed under the AGPL. Third-party notices are in `THIRD_PARTY_NOTICES.md`.
