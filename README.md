<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
-->
<!-- lang: en-GB -->

<h1 align="center">
  <br>
  <a href="https://git.eden-emu.dev/eden-emu/eden"><img src="https://github.com/user-attachments/assets/35031845-4bfa-4916-8bf5-bc3bce67d5e2" alt="Eglantine" width="200"></a>
  <br>
  <b>Eglantine</b>
  <br>
</h1>

<h4 align="center"><b>Eglantine</b> is a free and opensource (FOSS) Switch 2 emulator, derived from Eden, Yuzu and Sudachi - started by developer Camille LaVey.
It's written in C++ with portability in mind, with builds for Windows, Linux and macOS
</h4>

<p align="center">
    </a>
    <a href="https://discord.gg/XkS5NVPTUY">
        <img src="https://img.shields.io/discord/1367654015269339267?color=5865F2&label=Eglantine&logo=discord&logoColor=white"
            alt="Discord">
    </a>
  
</p>

> [!IMPORTANT]
> **This is an unofficial performance fork** focused on Mario Kart World. On top of the upstream
> `nx2` branch (merged up to date, including its Ocarina of Time fixes) it adds an experimental
> multithreaded GPU backend (a sequencer thread that records draws and a committer thread that
> submits them to Vulkan), an ASTC → BC7 transcoder, a disk cache for transcoded textures and
> VRAM-pressure fixes (resource relocation and a smaller render-target footprint so 8 GB cards
> spill to system memory less often). Branches: `main` (this work, cycle 13), `mkw-perf2`
> (single-threaded, stable).
>
> The new paths are **off unless you enable them** with environment variables:
>
> | Variable | What it does |
> |---|---|
> | `EDEN_NX2_MT=1` + `EDEN_NX2_MT_LITE=1` | multithreaded GPU backend |
> | `EDEN_NX2_ASTC_BC7=1` | transcode ASTC textures to BC7 (less VRAM than the default) |
> | `EDEN_NX2_ASTC_DISKCACHE=1` | keep transcoded textures on disk between runs |
>
> **Linux:**
> ```sh
> EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1 EDEN_NX2_ASTC_BC7=1 EDEN_NX2_ASTC_DISKCACHE=1 ./eglantine
> ```
> **Windows:** create `run-mt.bat` next to `eglantine.exe`:
> ```bat
> @echo off
> set EDEN_NX2_MT=1
> set EDEN_NX2_MT_LITE=1
> set EDEN_NX2_ASTC_BC7=1
> set EDEN_NX2_ASTC_DISKCACHE=1
> start "" "%~dp0eglantine.exe" %*
> ```
>
> Measured on a Ryzen 7 5800X + RTX 3070 Ti 8 GB at 2x resolution with all four enabled:
> ~40-48 fps in single-player races and ~28-30 fps in 2-player split screen. Results depend
> heavily on free VRAM: close browsers, video players and other GPU-heavy programs before playing,
> or the game can drop into a slow mode where resources spill to system memory. Developed and
> tested on Linux only; Windows and macOS builds come from CI and are untested. Use the Mailbox
> vsync mode if you are capped by FIFO.
>
> The user data folder is now `eglantine` instead of `eden` (upstream change); on first launch the
> migration prompt offers to copy your old `eden` data over. Keep a backup of your saves.


<p align="center">
  <a href="#compatibility">Compatibility</a> |
  <a href="#development">Development</a> |
  <a href="#building">Building</a> |
  <a href="#download">Download</a> |
  <a href="#support">Support</a> |
  <a href="#license">License</a>
</p>

## Compatibility

The emulator is capable of running Mario Kart World at decent/full speed depending on specs and configurations, and Splatoon Raiders at variable performances

A list of supported games will be available in future. Please be patient.

## Development

For development discussions, please join us on [Discord](https://discord.gg/XkS5NVPTUY)

If you would like to contribute, we are open to new developers and pull requests. Please ensure that your work is of a high standard and properly documented. You can also contact any of the developers on Discord or Stoat to learn more about the current state of the emulator.

## Documentation

We have a user manual! See our [User Handbook](./docs/user/README.md).

## Building

See the [General Build Guide](docs/Build.md)

For information on provided development tooling, see the [Tools directory](./tools)

## Download

You can download the latest releases from [here](https://github.com/Djihads80/Eglantine/releases).

## Support

We currently do not take in any donations for the time being

If you would prefer to support us in a different way, please join our [Discord](https://discord.gg/XkS5NVPTUY)

## Star History

<a href="https://www.star-history.com/?repos=djihads80%2Feglantine&type=date&legend=bottom-right">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=djihads80/eglantine&type=date&theme=dark&legend=bottom-right" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=djihads80/eglantine&type=date&legend=bottom-right" />
   <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=djihads80/eglantine&type=date&legend=bottom-right" />
 </picture>
</a>

## License

Eglantine is licensed under the GPLv3 (or any later version). Refer to the [LICENSE.txt](https://github.com/Djihads80/Eglantine/blob/nx2/LICENSE.txt) file.
