# CM5 Carrier Board Bringup

## Context

Custom CM5 carrier board is not booting cleanly on first attempts. Debug UART at TP35/TP36 (DEBUG_UART_TX/RX) + TP46 (GND), 115200 baud, has proven the module executes and reaches the kernel handoff via BL31, but kernel output is silent because the default `serial0` alias does not map to the Pi 5/CM5 dedicated debug UART (UART10 / `ttyAMA10`).

The "known good" NVMe (cloned from SD on the original Pi 5 hub) boots on both the Pi 5 and is presumed to boot on the CM5. A fresh Pi OS image flashed directly to NVMe did **not** boot on the Pi 5 (reasons unclear — possibly first-boot state / NVMe detection timing). So we will clone SD → NVMe instead of flashing a fresh image, matching the known-working recipe.

## Goal

1. Boot the CM5 carrier reliably from NVMe.
2. Get kernel UART output on TP35/TP36 so we can diagnose any remaining issues.
3. Make it easy to swap drives between Pi 5 and CM5 without bricking the hub.

## Steps

### 1. Change boot order on the Pi 5 hub (SD preferred)

Run on the hub:
```bash
sudo rpi-eeprom-config --edit
```
Change `BOOT_ORDER=0xf146` → `BOOT_ORDER=0xf641`.
Reads right-to-left: `1`=SD first, `4`=USB, `6`=NVMe, `f`=restart.
Save and reboot.

**Rationale**: with SD first, swapping NVMes in/out won't brick the hub — it always falls back to SD.

### 2. Clone SD → NVMe on the Pi 5

With the target NVMe plugged in (the one we want to use in the CM5):
```bash
sudo apt install -y rpi-clone
sudo rpi-clone nvme0n1
```

`rpi-clone` handles partition sizing, filesystem resize, and regenerates partition UUIDs so the clone boots cleanly.

Fallback (manual):
```bash
sudo dd if=/dev/mmcblk0 of=/dev/nvme0n1 bs=4M status=progress conv=fsync
sudo parted /dev/nvme0n1 resizepart 2 100%
sudo resize2fs /dev/nvme0n1p2
```

### 3. Apply debug-UART kernel config to the cloned NVMe

While still on the Pi 5 (NVMe plugged in, hub booted from SD):
```bash
sudo mkdir -p /mnt/boot
sudo mount /dev/nvme0n1p1 /mnt/boot

# Enable kernel UART console on CM5 dedicated debug UART
sudo sed -i 's|console=serial0,115200|earlycon console=ttyAMA10,115200|' /mnt/boot/cmdline.txt

# If the cloned cmdline doesn't have console=serial0, append the console args instead:
# sudo sed -i 's|$| earlycon console=ttyAMA10,115200|' /mnt/boot/cmdline.txt

# Ensure enable_uart=1 is present
grep -q "^enable_uart=1" /mnt/boot/config.txt || echo "enable_uart=1" | sudo tee -a /mnt/boot/config.txt

cat /mnt/boot/cmdline.txt
sudo umount /mnt/boot
```

**Why**: Bootloader UART works via fixed debug UART routing, but the kernel's `console=serial0` alias maps to UART0/GPIO14-15, not the debug UART at TP35/TP36. Using `ttyAMA10` targets UART10 directly. `earlycon` gets output from the very first kernel instruction, before the full serial driver loads.

### 4. Move NVMe to CM5 and capture UART

1. Safely shut down the hub, remove NVMe.
2. Install NVMe in the CM5 carrier's M.2 slot.
3. Connect debug probe at 115200 8N1:
   - Probe RX ← TP35 (CM5 TX)
   - Probe TX → TP36 (CM5 RX, optional)
   - Probe GND ↔ TP46
4. Power on. Expect to see:
   - Bootloader banner (`BOOTSYS release ...`)
   - PCIe scan + NVMe enumeration
   - Kernel loaded, BL31 banner
   - **Kernel boot messages** (new — this is what we're solving for)
   - Systemd startup
   - Login prompt

### 5. If kernel still silent after BL31

Fallbacks to try, in order:
1. Try `console=ttyS0,115200` instead of `ttyAMA10` (8250 driver).
2. Add `loglevel=8 debug ignore_loglevel` to cmdline.
3. Add `uart_2ndstage=1` to config.txt for more bootloader verbosity.
4. Plan to bodge USB slave + nRPIBOOT to enable `rpiboot` — allows editing EEPROM (`UART_CONSOLE=1` for guaranteed bootloader UART on all boot stages) and flashing eMMC directly.

## Done criteria

- Hub boots reliably from SD regardless of which NVMe is inserted.
- CM5 carrier boots the cloned NVMe.
- Kernel boot messages visible on TP35 at 115200 baud.
- Login prompt appears on UART (no network available on this carrier — no wifi, no Ethernet).

## Known gotchas

- CM5 module has **no wifi** on this SKU — UART is the only diagnostic channel.
- `POWER_OFF_ON_HALT=1` in EEPROM: if the OS requests shutdown, the module powers off and the next boot may land in `partition 63` (halt state) until power is cycled. This is normal.
- `serial0` ≠ debug UART on Pi 5/CM5. Always use `ttyAMA10` for kernel console on TP35/36.
- Pre-existing working NVMe uses SD-derived install — do not wipe it until the cloned one is proven.
