# ESP32-DIV V2 Build Configuration

This is the reproducible Arduino CLI recipe used for the ESP32-DIV V2 firmware.
Run commands from the repository root (the directory containing this file).

## Verification

This recipe was successfully compiled and uploaded to an ESP32-S3 revision
v0.2 through its CP210x serial interface on 2026-10-10. The resulting firmware
used 1,837,417 bytes of the 3,145,728-byte application partition.

## Toolchain

- Arduino CLI: `1.5.1`
- ESP32 Arduino core: `esp32:esp32` version `2.0.17`
- Sketch directory: `ESP32-DIV`
- Project-local libraries directory: `Libraries`

Install the required core with:

```powershell
arduino-cli core install esp32:esp32@2.0.17
```

## Board Options

- Board/FQBN: `esp32:esp32:esp32s3` (ESP32S3 Dev Module)
- Upload Speed: `921600`
- USB Mode: `Hardware CDC and JTAG` (`hwcdc`)
- USB CDC On Boot: `Disabled` (`default`)
- Upload Mode: `UART0 / Hardware CDC` (`default`)
- CPU Frequency: `240 MHz (WiFi)` (`240`)
- Flash Mode: `QIO 80 MHz` (`qio`)
- Flash Size: `16 MB` (`16M`)
- Partition Scheme: `16M Flash (3MB APP/9.9MB FATFS)` (`app3M_fat9M_16MB`)
- Core Debug Level: `None` (`none`)
- PSRAM: `OPI PSRAM` (`opi`)
- Arduino Runs On: `Core 1` (`1`)
- Events Run On: `Core 1` (`1`)
- Erase All Flash Before Upload: `Disabled` (`none`)
- JTAG Adapter: `Disabled` (`default`)

## Build Command

```powershell
arduino-cli compile `
  --fqbn "esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,PSRAM=opi,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default" `
  --libraries Libraries `
  --build-property "compiler.c.elf.extra_flags=-Wl,--allow-multiple-definition" `
  ESP32-DIV
```

## Upload

Replace `COM7` with the serial port shown by `arduino-cli board list`:

```powershell
arduino-cli compile --upload -p COM7 `
  --fqbn "esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,PSRAM=opi,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default" `
  --libraries Libraries `
  --build-property "compiler.c.elf.extra_flags=-Wl,--allow-multiple-definition" `
  ESP32-DIV
```

## Known Linker Requirement

`wifi.cpp` overrides `ieee80211_raw_frame_sanity_check`, which is also provided
by the ESP32 core's `libnet80211.a`. With core `2.0.17`, linking therefore
requires:

```text
-Wl,--allow-multiple-definition
```

Without it, the final link fails with a `multiple definition of
ieee80211_raw_frame_sanity_check` error even though compilation of the source
files succeeded.

## When a Full Rebuild Is Required

Arduino CLI caches compiled objects. Builds are normally incremental when this
recipe remains unchanged. A full rebuild is expected after changing the board,
ESP32 core, flash size, partition scheme, PSRAM mode, build flags, libraries, or
after clearing the Arduino build cache.
