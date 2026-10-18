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

## Live memory map (measured 2026-10-19, root `/proc/iomem` + `/sys/firmware/fdt`)

- DRAM: 0x80000000-0xffffffff (2 GB, many firmware carve-outs) and
  **0x880000000-0x9ffffffff (6 GB; memory@880000000/900000000/980000000)** with only
  0x9bdb50000-0x9c99fffff and 0x9c9a15000-0x9c9ad5fff reserved.
  => XNU RAM window: 0x880000000 + 4.5 GB (to 0x9a0000000) is clean; use it instead of the
  DTS-based 0xA0000000+544M guess in mkadt.
- ABL loads the kernel at 0x80080000, DTB at 0x8a000000, initrd 0x86000000-0x87f8b4a1.
- No framebuffer/splash node in the live FDT (confirmed).
- Raw copies: research/stallion-ko/{iomem.txt,live-fdt.dtb} (gitignored: contain IMEI/MACs).

## Hardware address table (extracted from stallion/zumapro DTS 2026-10-18)

Sources: LineageOS `android_kernel_google_gs-6.1_devices` /
`gs-6.6_devices` (lineage-24.0), `google/stallion/dts/` +
`google/zumapro/dts/` + `google/zuma/dts/`; raw copies in
`research/stallion-dts/` (gitignored), full table in
`research/stallion-dts/EXTRACTED.md`.

| Item | Address | Source |
|---|---|---|
| DRAM base | `memory@80000000`, 2 GB in first bank node (8 GB total; more bank nodes exist) | zumapro.dtsi |
| Reserved mem | 22 carve-outs: gsa 0x90200000, aoc 0x94000000, modem cp_rmem 0xEA400000/0xE8000000, seclog 0xC3000000, ramoops 0xfd3ff000, ... | zumapro-rmem.dtsi |
| GICD | 0x10400000 + 0x10000 (GIC-600 class, v3) | zumapro.dtsi |
| GICR | 0x10440000 + 0x200000 (8x 0x20000) | zumapro.dtsi |
| Timer | armv8-timer PPIs 13/14/11/10 (vtimer = INTID 27), cntfrq **24.576 MHz** | zumapro.dtsi |
| CPUs | 8 cores, MPIDR 0x0..0x700, clusters 4+3+1 (ppi_cluster0 cpu0-6, cluster1 cpu7) | zumapro-cpu.dtsi |
| Debug UART | serial_0 @ **0x10870000** (Exynos USI UART; earlycon in bootargs) | zumapro.dtsi |
| USI blocks | PERIC0 0x10880000-0x108D0000, PERIC1 0x10C9xxxx, APM 0x155C-5E, HSI0 0x111B-1F; **spi_20 @ 0x111D0000** | zumapro-usi.dtsi |
| UFS | hci 0x13200000, vs_hci 0x13201100, unipro 0x13280000, ufsp 0x132A0000, phy 0x13204000, cport 0x13208000; sysreg_ufs 0x13020000 | zuma-ufs.dtsi |
| USB DWC3 | 0x11210000 + 0x10000; usbdrd phy 0x11100000 (+0x1111-4x); stallion: super-speed + gen1 | zuma-usb.dtsi |
| Buttons | gpio-keys: power GPA4[0], voldown GPA4[1], volup GPA6[2] | stallion dtsi |
| PMU / syscon | pmu_system_controller **0x15460000** (PSHOLD lives here) | zumapro.dtsi |
| ACPM/PMIC | acpm_ipc mailbox 0x15110000, APM SRAM 0x15700000+0x58000; s2mpg14 (bus0 i2c @0x15500000 reg 0x1f), s2mpg15 (bus1 @0x15510000 reg 0x2f) | zumapro-pmic.dtsi |
| Display | decon0/1/2 @ 0x19470000/0x19471000/0x19472000, dsim0 0x19440000, DP 0x110F0000; panel 1080x2424 | stallion display dtsi |
| Bootloader FB | **no DT node — ABL injects it at boot**; gs101 used ~0xfac00000 a8r8g8b8 | must dump live FDT on device |
| Touch | FocalTech FT3683U on SPI spi_20 (0x111D0000), rails VLDO25M 1.8V / VLDO4M 3.3V (s2mpg), reset GPP1[1], irq GPN0[0] | stallion touch dtsi |

Timer PPIs are the standard architected ones (vtimer INTID 27), NOT
Qualcomm's 19/18. GIC-600: probe SecurityExtn/Group1 handling in shim
diagnostics as done on sweet.

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

## XNU milestone 1 (2026-10-18, code only)

See `research/stallion-xnu-plan.md`: patch-by-patch table and the
milestone 1 design (same kernel prints to the ramoops RAM log; panic ->
PSCI SYSTEM_RESET; hangs -> shim CPU7 sentinel). Implemented as xnu
0082/0083, `mkadt.py --target stallion` and shim loader changes.

## Next concrete action

Clone the stallion kernel device tree and extract the real DTB:

    git clone --depth 1 -b android-gs-stallion-6.1-android16 \
        https://android.googlesource.com/kernel/devices/google/zumapro research/stallion-kernel

then grep zumapro.dtsi / stallion dtsi for `memory@`, `reserved-memory`,
`interrupt-controller@`, `serial_0`, `spi_20`, `usbdrd31`, `decon`,
`cont_splash_mem`, `cpus`. That resolves all "verify" cells above.
