# dell-rf

A Linux command-line tool for pairing Dell wireless mice and keyboards through
a Dell Universal USB receiver.

> [!NOTE]
> This is an **independent, unofficial project** and is not affiliated with,
> endorsed by, or supported by Dell Technologies. Dell Technologies, Dell, and
> other trademarks are trademarks of Dell Inc. or its subsidiaries.

This project was prompted by the regrettable necessity of booting Windows (once
too often) to repair a corrupted pairing, and by the discovery that basic
receiver management apparently warrants an entire ~700 MB Windows application,
complete with telemetry and assorted other baggage.

## Hardware compatibility

Other peripherals and `413c:4503` receiver revisions may work but are unverified; please [report compatibility results](https://github.com/michaelknap/dell-rf/issues/new).

### Receivers

| Receiver | USB ID | Release |
| --- | --- | --- |
| Dell Universal Receiver | `413c:4503` | `0240` |
| Dell Universal Receiver | `413c:4503` | `0244` |

### Peripherals

| Peripheral | Type | Model reported by receiver |
| --- | --- | --- |
| Dell MS3121W | Mouse | `MS3121W` |
| Dell KB3121W | Keyboard | `KB3121W` |
| Dell KB700 | Keyboard | `KB7221W` <sup>[#1](https://github.com/michaelknap/dell-rf/issues/1)</sup> |
| Dell MS5320W | Mouse | `MS5320W` <sup>[#1](https://github.com/michaelknap/dell-rf/issues/1)</sup> |

[Protocol notes](docs/PROTOCOL-4503.md) describe the reverse-engineered
receiver protocol.

## Install

### Arch Linux

```sh
curl -fLO https://github.com/michaelknap/dell-rf/releases/download/v1.1.0/dell-rf-1.1.0-1-x86_64.pkg.tar.zst &&
sudo pacman -U ./dell-rf-1.1.0-1-x86_64.pkg.tar.zst
```

### Debian / Ubuntu

```sh
curl -fLO https://github.com/michaelknap/dell-rf/releases/download/v1.1.0/dell-rf_1.1.0-1_amd64.deb &&
sudo apt install ./dell-rf_1.1.0-1_amd64.deb
```

Packages are for x86_64/amd64, with Debian 12+ and Ubuntu 22.04+ supported.

Reconnect the receiver after installation.

## Use

Keep another input device available when unpairing your mouse or keyboard.

### List paired devices

```sh
sudo dell-rf slots
```

### Check battery levels

```sh
sudo dell-rf battery
```

It reports values from 0 to 100 as percentages, `unavailable` for any other
value, and `unsupported` when the slot does not advertise battery reporting.

### Unpair

Use the slot number shown by `slots`. For example, to remove slot 1:

```sh
sudo dell-rf unpair 1
```

Check the device shown, then type `unpair 1` to confirm.

### Pair

For a mouse:

```sh
sudo dell-rf pair mouse
```

For a keyboard:

```sh
sudo dell-rf pair keyboard
```

Turn the device off, hold a button or key, and turn it on. Release the button or
key when the device appears. Check the device shown, then type `pair` to confirm.
The pairing window lasts 30 seconds. Unpair an already paired device first.

After pairing or unpairing, check the result:

```sh
sudo dell-rf slots
```

If several receivers are connected, the command lists their compatible paths.
Add the path to select one, for example `sudo dell-rf slots /dev/hidraw2` or
`sudo dell-rf battery /dev/hidraw2`.
If an outcome is unverified, check `slots` before making another change.

For all commands:

```sh
dell-rf --help
```

## Build

Install build tools for your distribution:

- Arch: `sudo pacman -S --needed base-devel git`
- Debian/Ubuntu: `sudo apt install build-essential git`

Then compile and install:

```sh
git clone https://github.com/michaelknap/dell-rf.git
cd dell-rf
make
sudo make install
sudo udevadm control --reload-rules
```

---

## License

Licensed under the [MIT License](LICENSE).
