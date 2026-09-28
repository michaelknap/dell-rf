# dell-rf

A Linux command-line tool for pairing Dell wireless mice and keyboards through
a Dell Universal USB receiver.

This project was prompted by the regrettable necessity of booting Windows (once
too often) to repair a corrupted pairing, and by the discovery that basic
receiver management apparently warrants an entire ~700 MB Windows application,
complete with telemetry and assorted other baggage.

## Tested hardware

- Dell Universal Receiver: USB `413c:4503`, releases `0240` and `0244`.
- MS3121W mouse.
- KB3121W keyboard.

Other models and receiver revisions are unverified.

[Protocol notes](docs/PROTOCOL-4503.md) describe reverse-engineered notes.

## Install

### Arch Linux

```sh
curl -fLO https://github.com/michaelknap/dell-rf/releases/download/v1.0.2/dell-rf-1.0.2-1-x86_64.pkg.tar.zst &&
sudo pacman -U ./dell-rf-1.0.2-1-x86_64.pkg.tar.zst
```

### Debian / Ubuntu

```sh
curl -fLO https://github.com/michaelknap/dell-rf/releases/download/v1.0.2/dell-rf_1.0.2-1_amd64.deb &&
sudo apt install ./dell-rf_1.0.2-1_amd64.deb
```

Packages are for x86_64/amd64, with Debian 12+ and Ubuntu 22.04+ supported.

Reconnect the receiver after installation.

## Use

Keep another input device available when unpairing your mouse or keyboard.

### List paired devices

```sh
sudo dell-rf slots
```

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
Add the path to select one, for example `sudo dell-rf slots /dev/hidraw2`.
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
