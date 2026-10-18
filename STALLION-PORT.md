# Porting ios-sweet to Pixel 10a "stallion" (Tensor G4 / zumapro)

Kickoff 2026-10-18, fork `arielTROY/ios-sweet`. This file is the port plan;
session-by-session progress goes in `NOTES.md` as usual.

## Device facts (researched, sources in agent reports)

- Pixel 10a codename **stallion**, shipped March 2026, Tensor **G4** (zumapro /
  S5P9875, Exynos-derived), 8 GB LPDDR5X, UFS 3.1.
- CPU: 1x Cortex-X4 + 3x A720 + 4x A520, 3 clusters, ARMv9.2, PSCI via `smc`,
  kernel entry at EL2 (standard arm64 boot protocol, x0 = DTB).
- Ships Android 16, Linux 6.1 GKI, boot.img **header v4**: `boot` = kernel only;
  ramdisks in `init_boot` / `vendor_boot` / **`vendor_kernel_boot`**. A/B.
- Bootloader unlockable (`fastboot flashing unlock`), vbmeta
  `--disable-verity --disable-verification` required for custom images.
- **`fastboot boot` caveat (Pixel 7+): the bootloader ignores the ramdisk
  inside the sent image** and composes it from on-device `init_boot` +
  `vendor_boot` + `vendor_kernel_boot`. Kernel-only fastboot boot works;
  our shim-as-ramdisk payload trick does NOT. Options: embed payload in the
  kernel image itself, or flash a modified `vendor_kernel_boot` (verify).
- **UART exists:** `fastboot oem uart enable`, USB-C SBU pins @1.8V 115200
  (USB-Cereal cable). Debug UART MMIO **0x10870000** (Exynos USI UART,
  from zumapro BoardConfig). This is the single biggest advantage over sweet.
- Kernel sources: `kernel/devices/google/zumapro` + `google/stallion` on
  android.googlesource.com (`stallion_defconfig`); mirrors on
  LineageOS (`android_kernel_google_gs-6.1_devices`, stallion imported in
  commit cafda38), CalyxOS gitlab, GrapheneOS `17-stallion`.

## Hardware address table (zuma/zumapro; verify against live DTB)

| Item | Address | Confidence |
|---|---|---|
| DRAM base | 0x80000000 | confirmed (family) |
| Kernel load | ~0x80200000 | verify |
| GICD | 0x10400000 (+0x10000) | gs101/zuma, near-certain |
| GICR | 0x10440000 (+0x100000) | same |
| Debug UART (USI) | 0x10870000 | confirmed BoardConfig |
| UFS HCI | 0x13200000 | confirmed BoardConfig (`13200000.ufs`) |
| PMU / PSHOLD | 0x17460000 + 0x3E9C | gs101; verify |
| Bootloader FB | ~0xfac00000 a8r8g8b8 stride 4320 | gs101-confirmed, verify on stallion (1080x2424) |
| DECON/DPU | 0x1C300000 region (cal_9845; zumapro=cal_9875 may differ) | verify |
| Touch | FocalTech FT3683U on **SPI** (spi_20 USI), rails VLDO25M 1.8V / VLDO4M 3.3V, reset GPP1[1], irq GPN0[0] | confirmed stallion dtsi |
| Buttons | gpio-keys EINT (VolDn GPA7[3], VolUp GPA8[1], Power GPA10[1] on gs101) | verify pins |
| USB | DWC3 `usbdrd31` + combo phy in HSI0 block | address unverified |
| Regulators | s2mpg14/15 PMIC behind ACPM mailbox I2C (`acpm_ipc`) | confirmed |

Timer PPIs on Tensor are the standard architected ones (13/14/11/10; vtimer
IRQ = PPI 11 / INTID 27), NOT Qualcomm's 19/18. GIC is GIC-600; must probe
SecurityExtn/Group1 handling as done on sweet.

## Porting strategy

The design inherits sweet's core trick: **piggyback on bootloader-initialized
hardware** (ABL leaves FB scanning, UFS link up, GIC configured). Each
assumption must be re-probed on stallion — shim diagnostic mode exists for
exactly this.

### Phase 0 — recon (no XNU yet)
1. Get the stallion DTB: `git clone` the zumapro/stallion kernel tree OR
   extract `vendor_kernel_boot`/`dtbo` from the factory image and `dtc -I dtb`.
   Resolve every "verify" in the table above.
2. Unlock BL, `fastboot oem uart enable`, USB-Cereal -> serial console.
3. Back up partitions (persist-equivalents, boot, init_boot, vendor_boot,
   vendor_kernel_boot, dtbo, vbmeta) via a recovery/adb path. Per AGENTS.md:
   confirm with user before any destructive step.
4. Shim bring-up: port `shim/` to boot header v4, run shim diagnostics over
   **UART** (add UART console first — huge win vs. framebuffer-only):
   dump live FDT from x0, probe GICD/GICR, probe UFS HCI regs, check FB.

### Phase 1 — shim + kernel to a console
5. Rework payload delivery (ramdisk ignored by fastboot boot): simplest is
   append SWEETXNU payload into the kernel Image's own page-aligned tail, or
   stuff it into a modified vendor_kernel_boot ramdisk and flash it.
6. mkadt: new constants (GIC, FB, UFS, UART node?, mem banks from live DTB,
   MPIDR tri-cluster, TIMEBASE=cntfrq, VTIMER_INTID=27).
7. XNU patches: rerun series 0001-0080 concept; most generic patches carry
   over. New-work items:
   - early debug: UART first (better than FB), then fbcon via simplefb.
   - GIC path: reuse SweetGIC, fix Group handling per probe results.
   - UFS: new exynos variant — `vs_hci`/`unipro`/`ufsp` blocks at 0x13200000;
     UTP command layer is standard UFSHCI so the sweet driver's upper half
     carries over; link init differs (hope ABL leaves link up like Xiaomi's).
   - SMP: PSCI smc conduit fine; tri-cluster MPIDR aff1 mapping.
   - Buttons: plain EINT GPIOs (simpler than SPMI); touch: SPI USI +
     ACPM-regulator rails (new regulator stack: acpm_ipc mailbox -> PMIC I2C).
   - USB: DWC3 behind Exynos glue; defer.
   - Rootfs: userdata partition still exists; keep iOS root in `userdata`,
     log ring in its tail (same as sweet).

### Phase 2 — userland (mostly inherited)
8. Same iPhone X 16.7.16 identity/root image pipeline (mkiosroot,
   sweet-root-tweaks, ADT /product copy). Should port wholesale once the
   kernel reaches launchd.

## Open questions / risks

- Does stallion ABL leave a CPU-visible linear FB live in video mode?
  (gs101 did at 0xfac00000; if not, DECON bring-up is a big extra task.)
- Does `fastboot boot` even reach our Image on stallion; payload plan.
- Anti-rollback: May-2026 ARB bump — check whether it covers 10a before
  downgrading/booting older builds.
- UFS link state after ABL handoff.
- EL2 entry: shim may need to drop to EL1 or run at EL2 (XNU expects EL1).

## Next concrete action

Clone the stallion kernel device tree and extract the real DTB:

    git clone --depth 1 -b android-gs-stallion-6.1-android16 \
        https://android.googlesource.com/kernel/devices/google/zumapro research/stallion-kernel

then grep zumapro.dtsi / stallion dtsi for `memory@`, `reserved-memory`,
`interrupt-controller@`, `serial_0`, `spi_20`, `usbdrd31`, `decon`,
`cont_splash_mem`, `cpus`. That resolves all "verify" cells above.
