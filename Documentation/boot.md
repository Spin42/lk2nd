# lk2nd boot process

lk2nd provides mutliple ways to boot, each of which has advantages and limitations.

## Android boot.img

lk2nd is based on CAF lk and includes aboot implementation. It can boot an android
boot image stored at 512k offset from the start of the `boot` partition (in lk2nd)
or withoug the offset (for lk1st). The offset is required so the next stage image
doesn't overwrite the lk2nd itself.

## Fastboot boot

lk2nd provides a fastboot interface and allows one to boot an OS via USB. Same as
the previous option, this comes from CAF lk and uses android boot images.

```
fastboot boot boot.img
```

## extlinux.conf

lk2nd provides a very rudimentary support for `extlinux.conf` file, similar to
u-boot and depthcharge.

The file shall be stored at an `ext2` formated partition as `/extlinux/extlinux.conf`.
lk2nd will search for it on any partition that is bigger than 16MiB and on a `boot`
partition (offset by 512k in lk2nd).

Following commands are supported:

- `label <label>`       - Start a new boot entry.
- `default <label>`     - Set label that will be used to boot.
- `linux <kernel>`      - Path to the kernel image. (alt: `kernel`)
- `initrd <initramfs>`  - Path to the initramfs file.
- `fdt <devicetree>`    - Path to the devicetree. (alt: `devicetree`)
- `fdtdir <directory>`  - Path to automatically find the DT in. (alt: `devicetreedir`)
- `append <cmdline>`    - Cmdline to boot the kernel with.
- `fdtoverlays <files>` - A space separated list of DT overlays to apply. (alt: `devicetree-overlay`)

> [!NOTE]
> lk2nd includes only a very rudimentary extlinux support at this time.
> only commands listed above are considered and lk2nd will always boot the
> "default" label.

### Example extlinux.conf

The example below shows a "correct" extlinux.conf that includes additional
directives not supported by lk2nd. lk2nd will ignore unknown commands.

```
timeout 1
menu title Boot the OS
default MyOS

# Bootloader should pick the DT
label MyOS
    linux /vmlinuz
    initrd /initramfs
    fdtdir /dtbs
    append earlycon console=ttyMSM0,115200
```

## Generic A/B partition boot (RAUC-style)

lk2nd can perform A/B boot attempts using a U-Boot style environment stored inside a single underlying partition. This is compatible with the RAUC flow (decrement boot counters before attempting a slot; userspace resets counters after successful boot).

### Concepts

1. Environment partition: A single block device (e.g. `mmcblk0p20`) contains both the U-Boot environment and the two slot filesystems at different byte offsets.
2. Environment (uboot.env): Stored at a fixed offset+size (defaults: offset `0x10000`, size `0x20000`).
3. Slot offsets: Two byte offsets inside the same base device (example Fairphone 2: A=`0x00100000`, B=`0x04100000`). A subdevice is published at the chosen offset and mounted as an ext2 root.
4. Boot counters: `BOOT_A_LEFT`, `BOOT_B_LEFT` are decremented on each attempt; if a counter reaches 0 lk2nd switches to the next slot in `BOOT_ORDER`.

### Environment Variables

Required / interpreted variables in the U-Boot environment:
- `BOOT_ORDER`: Space separated list of slots to try in order (e.g. `A B`).
- `BOOT_A_LEFT`: Remaining attempts for slot A.
- `BOOT_B_LEFT`: Remaining attempts for slot B.

If a variable is missing, lk2nd initializes it with a default maximum attempt count.

### Slot Selection Flow
1. Read environment at configured offset.
2. Determine current slot: first slot in `BOOT_ORDER` with attempts left.
3. Decrement its counter and save the environment (pre-boot).
4. Publish a subdevice (`ab-slot`) starting at the slot offset and mount it.
5. Load `/extlinux/extlinux.conf` from that filesystem.
6. Select the label matching the slot.

### Constructing extlinux.conf for A/B

To keep identical `extlinux.conf` in both slots while still booting slot-specific content:
1. Provide a `default <base>` line (optional but recommended).
2. Define two labels suffixed with `_A` and `_B`:
   - The `<base>` name can be reused as kernel naming anchor.
   - lk2nd will force selection of `<default>_<slot>` if `default` is defined; otherwise it searches for any label ending in `_<slot>`.

Minimal example (same file copied into both slot filesystems):
```
default linux
label linux_A
  linux /vmlinuz
  initrd /initramfs
  fdtdir /dtbs
  append console=ttyMSM0,115200 root=/dev/slotA

label linux_B
  linux /vmlinuz
  initrd /initramfs
  fdtdir /dtbs
  append console=ttyMSM0,115200 root=/dev/slotB
```

Notes:
- Kernel/initrd/dtb paths can be identical if the payload inside each slot provides its own versions.
- If a slot-specific label is missing, lk2nd aborts the boot for that slot (fails fast instead of silently using the wrong label).
- If `default` is absent, lk2nd falls back to any label that ends with `_A` or `_B` depending on the active slot.

### Userspace Responsibilities
- After a successful boot into a slot, userspace should reset its boot counter (e.g. using `fw_setenv BOOT_A_LEFT <max>`).
- During an update, deploy the new root filesystem at the inactive slot offset and adjust counters to try that slot first (`fw_setenv BOOT_ORDER "B A"`).

## lk2nd cmdline arguments

lk2nd can read OS cmdline argument and make some decisions while booting it.

> [!IMPORTANT]
> lk2nd reads those values from the OS it boots, not from it's own cmdline.

- `lk2nd.pass-simplefb(=...)` - Add simplefb node to the dtb.
  If `autorefresh` is set, display autorefresh for command mode panels will be enabled.
  If `xrgb8888` or `rgb565` is set, the display mode will be switched to selected one.
  If `relocate` is set, the framebuffer address will be changed to a large reasonably
  safe region. Options can be combined. (i.e. `...=xrgb8888,autorefresh`)
- `lk2nd.pass-ramoops(=zap)` - Add ramoops node to the dtb. If `zap` is set, clear
  the region before booting. Use `fastboot oem ramoops ...` commands to get the data.
- `lk2nd.spin-table=force` - Force enable spintable even if PSCI is available.
