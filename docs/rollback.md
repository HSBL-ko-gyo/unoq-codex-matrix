# Rollback and uninstall

## What rollback can and cannot restore

The uninstaller removes only this project's Linux service, installed commands,
private virtual environment, and Hook entries. It preserves unrelated Hooks,
Codex settings, installed packages, journal history, Arduino Router, and the
configuration file unless `--purge` is explicitly requested.

The installer cannot read back or archive the previously programmed STM32
application. Removing the Linux service therefore does **not** restore prior MCU
firmware. Flash a separately known-good sketch if firmware rollback is required.

## Normal uninstall

From the retained checkout:

```bash
cd /home/arduino/src/unoq-codex-matrix
sudo ./scripts/uninstall.sh
```

This operation is idempotent. It stops and disables
`unoq-codex-matrix.service`, removes only Hook handlers whose command exactly
matches `/usr/local/bin/unoq-codex-matrix-hook` (or the marked inline block),
removes project-owned command symlinks and `/opt/unoq-codex-matrix`, and keeps:

```text
/etc/unoq-codex-matrix/config.json
installed system packages
systemd journal
currently programmed STM32 firmware
all unrelated Codex Hooks and settings
```

To remove the project configuration as well:

```bash
cd /home/arduino/src/unoq-codex-matrix
sudo ./scripts/uninstall.sh --purge
```

`--purge` affects only `/etc/unoq-codex-matrix/config.json` and its now-empty
project directory.

## Remove only the Hook entries

Keep the daemon installed but detach it from Codex with:

```bash
cd /home/arduino/src/unoq-codex-matrix
sudo -u arduino python3 scripts/uninstall-hooks.py --home /home/arduino
```

The script validates the resulting JSON or TOML and creates another timestamped
backup before changing an existing file. Review `/hooks` after removal.

## Restore a timestamped Codex backup

Hook installation creates backups alongside files that already exist:

```text
/home/arduino/.codex/hooks.json.bak.<UTC timestamp>
/home/arduino/.codex/config.toml.bak.<UTC timestamp>
```

List candidates and choose the intended timestamp manually:

```bash
sudo -u arduino ls -l /home/arduino/.codex/hooks.json.bak.*
sudo -u arduino ls -l /home/arduino/.codex/config.toml.bak.*
```

Before overwriting anything, validate the exact selected backup. Replace the
example timestamp below with the chosen filename:

```bash
python3 -m json.tool /home/arduino/.codex/hooks.json.bak.20260816T000000Z >/dev/null
python3 -c 'import sys,tomllib; tomllib.load(open(sys.argv[1], "rb"))' /home/arduino/.codex/config.toml.bak.20260816T000000Z
```

Restore only the file that needs rollback:

```bash
sudo install -o arduino -g arduino -m 0600 /home/arduino/.codex/hooks.json.bak.20260816T000000Z /home/arduino/.codex/hooks.json
sudo install -o arduino -g arduino -m 0600 /home/arduino/.codex/config.toml.bak.20260816T000000Z /home/arduino/.codex/config.toml
```

Then open `/hooks` in Codex and review the resulting definitions and trust state.
Do not restore an unvalidated file and do not copy `auth.json`.

## Manual Linux cleanup when the checkout is unavailable

Prefer the uninstaller above because it preserves configuration and removes only
matching Hooks. If the checkout is genuinely unavailable, first remove this
project's Hook entries through a recovered copy of `uninstall-hooks.py` or edit
only the exact matching handler after making and validating a backup.

Then inspect the three symlinks before removal:

```bash
readlink /usr/local/bin/unoq-codex-matrix
readlink /usr/local/bin/unoq-codex-matrixd
readlink /usr/local/bin/unoq-codex-matrix-hook
```

Only when each link points inside `/opt/unoq-codex-matrix`, remove these exact
project paths:

```bash
sudo systemctl disable --now unoq-codex-matrix.service
sudo rm -f /etc/systemd/system/unoq-codex-matrix.service
sudo systemctl daemon-reload
sudo rm -f /usr/local/bin/unoq-codex-matrix
sudo rm -f /usr/local/bin/unoq-codex-matrixd
sudo rm -f /usr/local/bin/unoq-codex-matrix-hook
sudo rm -rf -- /opt/unoq-codex-matrix
```

Do not remove `/run/unoq-codex-matrix` recursively while the service is running;
systemd removes its runtime directory after shutdown. Do not alter
`arduino-router.service` or its socket permissions.

## MCU firmware rollback

Compile a known-good, independently retained UNO Q sketch before writing it:

```bash
arduino-cli compile -b arduino:zephyr:unoq /path/to/known-good-sketch
arduino-cli upload -b arduino:zephyr:unoq /path/to/known-good-sketch
```

Use the actual supported upload arguments for the installed core. Never upload
an uncompiled sketch and never guess which previous application was on the MCU.
After upload, confirm Router health and the expected behavior of that sketch.

If no known-good sketch exists, leave the current MCU firmware in place. Once
the daemon is removed or stopped, its heartbeat expires and this project's
firmware shows OFFLINE without affecting Codex.

## Verification after rollback

```bash
systemctl status unoq-codex-matrix.service --no-pager
systemctl status arduino-router.service --no-pager
test ! -e /usr/local/bin/unoq-codex-matrix-hook
```

Expected results are that the project service is absent/inactive, Router remains
active, and the project Hook command is absent. Inspect `/hooks` to confirm all
unrelated entries remain. Network, SSH, desktop, Steam/FEX, GPU, kernel,
bootloader, existing repositories, and global Git configuration require no
rollback because this project does not change them.
