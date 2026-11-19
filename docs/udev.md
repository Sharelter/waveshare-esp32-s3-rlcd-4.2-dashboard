# Serial permissions (Linux)

`/dev/ttyACM0` is created as `root:dialout 0660`, so a normal user cannot open
it. **You only need this if you run `scripts/run.sh` by hand** — the systemd
service gets access through `SupplementaryGroups=dialout` and needs nothing
here.

Pick one.

## Option A — udev rule (recommended)

Applies to the board only, and survives re-enumeration.

```bash
printf 'SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", MODE="0666"\n' \
  | sudo tee /etc/udev/rules.d/99-esp32-serial.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=tty
```

Check it landed:

```bash
ls -l /etc/udev/rules.d/99-esp32-serial.rules
ls -l /dev/ttyACM0        # should now be crw-rw-rw-
```

`303a` is Espressif's USB vendor ID; it also matches the USB-Serial-JTAG
interface used by other ESP32-S3 dev boards.

**Why not `chmod 666`?** It works until the next re-enumeration — unplugging,
resetting, or flashing all wipe it. If a permission problem comes back "for no
reason", a temporary `chmod` is almost always why.

## Option B — put yourself in `dialout`

```bash
sudo usermod -aG dialout "$USER"
```

Then **log out and back in** (group membership is read at login; `newgrp dialout`
only affects the current shell). Verify with:

```bash
id -nG | tr ' ' '\n' | grep dialout
```

## If the port is grabbed by something else

`ModemManager` sometimes probes CDC-ACM devices and holds the port open:

```bash
sudo systemctl mask ModemManager
```

Also close any Arduino IDE serial monitor, `screen`, or `minicom` session.