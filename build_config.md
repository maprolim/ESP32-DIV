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
- PSRAM: `Disabled` (`disabled`) — see "PSRAM" below
- Arduino Runs On: `Core 1` (`1`)
- Events Run On: `Core 1` (`1`)
- Erase All Flash Before Upload: `Disabled` (`none`)
- JTAG Adapter: `Disabled` (`default`)

## Build Command

```powershell
arduino-cli compile `
  --fqbn "esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,PSRAM=disabled,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default" `
  --libraries Libraries `
  --build-property "compiler.c.elf.extra_flags=-Wl,--allow-multiple-definition" `
  ESP32-DIV
```

## Upload

Replace `COM7` with the serial port shown by `arduino-cli board list`:

```powershell
arduino-cli compile --upload -p COM7 `
  --fqbn "esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,PSRAM=disabled,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default" `
  --libraries Libraries `
  --build-property "compiler.c.elf.extra_flags=-Wl,--allow-multiple-definition" `
  ESP32-DIV
```

## PSRAM

This board has **no PSRAM chip**. Both modes were tested on 2026-10-12 and
neither initialises:

| Setting | Boot reading | Meaning |
| --- | --- | --- |
| `PSRAM=opi` | `PSRAM ID read error: 0x00000000` | OPI data lines read all zero |
| `PSRAM=enabled` (QSPI) | `PSRAM ID read error: 0x00ffffff` | QSPI data lines floating |

Both readings mean "nothing is connected", so this is not a wrong-mode problem.
`PSRAM=disabled` is therefore intentional: it removes the two `E (...) spiram`
error lines from every boot and drops the dead initialisation attempt.

**Consequence:** the whole firmware runs from the internal SRAM. The build
reports ~43% of DRAM consumed by globals, and the boot log shows about **84 KB
of free heap** after BLE init. Keep that in mind before adding large in-RAM
buffers.

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

## Backlight and Status LED

Confirmed against the official ESP32-DIV schematics
(<https://github.com/cifertech/ESP32-DIV/wiki/Schematics>):

| Function | GPIO | Notes |
| --- | --- | --- |
| TFT backlight (`BACKLIGHT_PIN`) | 7 | via SI2302 transistor; PWM on LEDC channel 0 |
| NeoPixel / WS2812 (`NEOPIXEL_PIN`) | 1 | status LED above the display; the CYD has none |

The backlight is **not** driven by TFT_eSPI (`TFT_BL` is commented out in
`Libraries/User_Setup v2.h`); the sketch owns it through `setBrightness()`
in `ESP32-DIV.ino`, which writes the LEDC channel.

**Backlight polarity (measured on the device, looking at the backlight from
the side of the panel):** the backlight is **active-high**. `duty 255`
(`HIGH`) = brightest; `duty 0` (`LOW`) = dimmest. This matches the NPN/SI2302
low-side switch in the upstream schematic. A faint residual glow remains even
at `duty 0` (hardware minimum on this board).

**Screen off (Captive Portal, UP button):** `screenPowerOff()` fills the panel
**black** and then sets the LEDC channel to `BACKLIGHT_OFF_DUTY` (which is
**0** = LOW = dimmest). **Do NOT send `SLPIN`/`DISPOFF` (0x10/0x28) to the
panel:** on the ILI9341 both blank the panel to **white**, which stays visible
from ambient light even with the backlight off. So the practical "off" is
backlight at minimum + black panel (~90% of the screen's power is the
backlight; the panel's ~2-5 mA idle has no software cut-off on this board).
Waking sets the LEDC back to `settings().brightness`. While off, only UP is
read; the captive portal keeps serving (DNS + HTTP) and the NeoPixel blinks
every 5 s.
