# hid-santroller (DKMS)

Out-of-tree build of the Linux HID driver for controllers and instruments
running the [Santroller](https://github.com/Santroller/Santroller) firmware.
It adds rumble, player LEDs, the RGB indicator LED and the DJ Hero turntable
euphoria LED, and names the gamepad after the active profile.

`hid-santroller.c` is copied unchanged from the kernel tree
(`drivers/hid/hid-santroller.c`). `hid-ids.h` stands in for the in-tree header
of the same name and backfills `hid_warn_ratelimited()` on kernels before 6.17.

## Requirements

- Linux 6.5 or newer
- Headers for the running kernel (`linux-headers` on Arch, `linux-headers-$(uname -r)` on Debian/Ubuntu, `kernel-devel` on Fedora)
- `dkms`
- A kernel built with `CONFIG_LEDS_CLASS_MULTICOLOR` and `CONFIG_INPUT_FF_MEMLESS` (most distro kernels have both)

## Install

```sh
sudo cp -r . /usr/src/hid-santroller-0.1.0
sudo dkms install hid-santroller/0.1.0
```

DKMS rebuilds the module on every kernel update. Plug in the controller and the
module loads automatically; if a controller was already connected, it gets
rebound from `hid-generic` as soon as the module loads.

On systems with Secure Boot, DKMS signs the module with its MOK key, which has
to be enrolled once (`mokutil --import /var/lib/dkms/mok.pub`).

## Uninstall

```sh
sudo dkms remove hid-santroller/0.1.0 --all
sudo rm -rf /usr/src/hid-santroller-0.1.0
```

## Building by hand

```sh
make
sudo modprobe -a ff-memless led-class-multicolor   # insmod doesn't load dependencies
sudo insmod hid-santroller.ko
```

## Checking it works

```sh
ls /sys/bus/hid/drivers/santroller/       # bound devices
ls /sys/class/leds/ | grep input          # player / RGB / euphoria LEDs
cat /sys/bus/hid/devices/*/subtype        # controller subtype from the firmware
```

Only HID mode is handled here. In XInput mode, the controller is driven by
`xpad` instead.
