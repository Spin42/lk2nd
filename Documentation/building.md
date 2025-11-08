# Building lk2nd

## Requirements
- `make`
- Python 3: `python3`
- ARM (32 bit) GCC tool chain
  - Arch Linux: `arm-none-eabi-gcc`
  - Alpine Linux and postmarketOS: `gcc-arm-none-eabi`
  - Debian and Ubuntu: `gcc-arm-none-eabi`
  - Fedora: `arm-none-eabi-gcc-cs`
- [Device Tree Compiler](https://git.kernel.org/pub/scm/utils/dtc/dtc.git)
  - Arch Linux: `dtc`
  - Alpine Linux and postmarketOS: `dtc`
  - Debian and Ubuntu: `device-tree-compiler`
  - Fedora: `dtc`
- libfdt
  - Alpine Linux and postmarketOS: `dtc-dev`
  - Debian and Ubuntu: `libfdt-dev`
  - Fedora: `libfdt-devel`
- GNU tar
  - Alpine Linux and postmarketOS: `tar`
- Optional requirements when using image signing
  - Arch Linux: `python-pyasn1-modules python-pycryptodome`
  - Alpine Linux and postmarketOS: `py3-asn1-modules py3-pycryptodome`
  - Debian and Ubuntu: `python3-pyasn1-modules python3-pycryptodome`
  - Fedora: `python3-pyasn1-modules python3-pycryptodomex`

## Building lk2nd

Check targets.md for the make target you should use below.
(It depends on the SoC of your device.)

```
$ make TOOLCHAIN_PREFIX=arm-none-eabi- lk2nd-msmXXXX
```

Replace `TOOLCHAIN_PREFIX` with the path to your tool chain.
`lk2nd.img` is built and placed into `build-lk2nd-msmXXXX/lk2nd.img`.

## Building lk2nd in headless mode

In case you have a device that doesn't have a screen but still want to boot linux on it. You can define `LK2ND_SKIP_GDSC_CHECK` to `1` when invoking `make`.

```
$ make TOOLCHAIN_PREFIX=arm-none-eabi- LK2ND_SKIP_GDSC_CHECK=1 lk2nd-msmXXXX
```

This will print `Skipping GDSC check for headless boot` during the boot sequence and continue booting normally.

## Building lk1st

**Note:** Unlike lk2nd, lk1st is still experimental and therefore not described
here yet.

## Additional build flags

lk2nd build system provides few additional compile time settings that you can add
to the `make` cmdline to enable some additional features.

### General settings

#### `DEBUG=` - Log level

Set to 0 to suppress most of the logging, set to 2 to enable excessive log messages
(may slow down boot). Default is 1.

#### `DEBUG_FBCON=` - Enable logging to the display

Set to 1 to make lk2nd print the logs on the screen.

#### `LK2ND_VERSION=` - Override lk2nd version string

By default lk2nd build system will try to get the version from git. If you need
to override the version (i.e. if you want to package lk2nd build), set this varable.

#### `LK2ND_FORCE_FASTBOOT=` - Force lk2nd to boot into fastboot menu

By setting this option to 1 lk2nd will always enter the menu upon boot instead of
continuing with the usual workflow. This is useful for debugging and development.

#### `LK2ND_FASTBOOT_DELAY=` - Add delay before booting via `fastboot boot` (ms)

This can help with debugging on devices with carkit uart.
You need to switch the cable before starting linux to see all the logs.

#### `MENU_COUNTDOWN_SECONDS=` - Boot menu countdown duration

Set the number of seconds to wait for keypress during boot countdown before continuing normal boot (default: 10). The countdown is displayed when `UMS_ENABLE=1` or other conditions trigger the boot menu.

```
$ make TOOLCHAIN_PREFIX=arm-none-eabi- MENU_COUNTDOWN_SECONDS=5 lk2nd-msmXXXX
```

#### `UMS_ENABLE=` - Enable USB Mass Storage mode

Set to 1 to enable USB Mass Storage support. When enabled, lk2nd displays a countdown during early boot. Press any key during the countdown to open the fastboot menu on the serial console, where you can select "USB Storage" to expose a partition as a USB mass storage device for direct access from a PC.

```
$ make TOOLCHAIN_PREFIX=arm-none-eabi- UMS_ENABLE=1 lk2nd-msmXXXX
```

#### `UMS_PARTITION=` - Partition to export in UMS mode

Set the partition name to expose when entering USB Mass Storage mode via the menu (default: `userdata`). Can be any valid partition name like `system`, `userdata`, `cache`, etc.

```
$ make TOOLCHAIN_PREFIX=arm-none-eabi- UMS_ENABLE=1 UMS_PARTITION=system lk2nd-msmXXXX
```

#### `LK2ND_SERIAL_MENU=` - Force menu on serial console

Set to 1 to always render the fastboot/lk2nd menu on the serial console instead of the framebuffer. Useful for headless devices or debugging via UART. The menu automatically falls back to serial when no display is available; this flag forces serial output regardless.

```
$ make TOOLCHAIN_PREFIX=arm-none-eabi- LK2ND_SERIAL_MENU=1 lk2nd-msmXXXX
```

#### Complete headless + UMS example

For headless devices (no display) with UMS support:

```
$ make TOOLCHAIN_PREFIX=arm-none-eabi- \
     LK2ND_SKIP_GDSC_CHECK=1 \
     LK2ND_SERIAL_MENU=1 \
     UMS_ENABLE=1 \
     MENU_COUNTDOWN_SECONDS=10 \
     UMS_PARTITION=userdata \
     lk2nd-msmXXXX
```

### lk2nd specific

#### `LK2ND_ADTBS=`, `LK2ND_QCDTBS=`, `LK2ND_DTBS=` - Only build listed dtbs

### lk1st specific

#### `LK2ND_COMPATIBLE=` - Board compatible

Set the board compatible value.

#### `LK2ND_BUNDLE_DTB=` - Board dtb file

Use this dtb for lk1st build.

#### `LK2ND_DISPLAY=` - Display panel driver

Set specific panel driver. By default it uses `cont-splash`.

### Signing of images

#### `SIGN_BOOTIMG=` - Sign `lk2nd.img` after build

Set to 1 to have the resulting `lk2nd.img` be signed for AVB1. By default
images are not signed.

#### `BOOTIMG_CERT=` and `BOOTIMG_KEY=` - Set signing credentials used to sign images

Set the signing certificate and private key to use for signing boot images.
If signing is enabled and no keys are specified a pair of default test keys
from the AOSP 13 source tree located in `lk2nd/certs/` are used to sign the
image.
`BOOTIMG_CERT` is expected to be in X.509 PEM format and `BOOTIMG_KEY` is
expected to be in PKCS#8 format.
