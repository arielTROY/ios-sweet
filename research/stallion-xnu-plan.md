# XNU patch series vs. Pixel 10a "stallion" (Tensor G4 / zumapro)

Written 2026-10-18 on branch `worktree-agent-a473518a4dc260d56` (based on fork
`main` b5550ed). Sources: `xnu/patches/0001-0080` (+ upstream-only 0081),
`research/stallion-dts/` (zumapro/stallion DTS), `STALLION-PORT.md`.
Status words: **observed** = read in code/DTS; **guess** = not verified on the
phone. Nothing here was run on the device.

## Target switch: one kernel, ADT/boot_args driven

The series is NOT forked. All 80 patches are under `ARM64_BOARD_CONFIG_SWEET` /
`SWEET` (one machine config, one CI build, `kernel.development.sweet`).
Board differences already flow mostly through the ADT that `tools/mkadt.py`
builds (GIC reg, vtimer INTID, fbcon, UFS reg, CPUs, identity). The few
hard-coded sweet facts that would run on stallion before any driver choice
(early framebuffer 0x9c000000 in `start.s`/`sweet_early.c`, the TTBR1 alias)
are removed by **0082**; board-only drivers are gated by the presence of ADT
nodes, so a stallion ADT simply omits `/sweet-ufs`, `/sweet-fbcon`, `disp0`:

| Mechanism | Used for |
|---|---|
| `boot_args->Video` (shim fills it; 0 = none) | early framebuffer console (start.s stage-0 band, `sweet_early.c`) |
| ADT `/sweet-ramlog` reg | RAM log (ramoops zone) - stallion only |
| ADT `/chosen sweet-panic-reset`, `sweet-reset-after` (+ boot-args `sweetpanicreset=`, `sweetreset=`) | panic/hang -> PSCI SYSTEM_RESET - stallion only |
| ADT `/arm-io/gic virtual-timer-intid`, `sweet-quiesce` | 19 on sweet, 27 on stallion; SPI/PPI quiesce on stallion |
| ADT root compatible `google,stallion`, cpu compatible `google,zumapro-cpu` | SweetPlatformExpert / SweetCPU personalities (0083) |
| ADT `/sweet-ufs` present | starts sweet_ufs + everything chained to it (log ring, sampler, keys, touch, USB) |
| `tools/mkadt.py --target stallion` | builds the stallion ADT from the DTS constants |

Rule for future stallion drivers: add them as new ADT-gated drivers (e.g.
`/sweet-ufs compatible = "samsung,exynos-ufs"`, `/sweet-keys compatible =
"gpio-keys,exynos"`), never `#ifdef STALLION`. A compile-time `STALLION`
machine config would double CI time and fork every SWEET-guarded hunk.

## Patch table

Category: **G** = generic Apple-compat / toolchain (board independent),
**S** = sweet-hardware-specific, **M** = mixed (generic logic with sweet
constants or a sweet-only assumption). "Guard" = what keeps it off/right on
stallion in a single tree.

| # | Title | Cat | Stallion: what must change / status | Guard |
|---|---|---|---|---|
| 0001 | new clang: drop removed auto-var-init flag | G | none | - |
| 0002 | new clang: backport UIO_STACKBUF | G | none | - |
| 0003 | framebuffer "serial" console (`sweet-fbcon` ADT node) | G | none in code. Needs a CPU-visible FB: ABL injects it into the live FDT (no DTS node); gs101 used ~0xfac00000 a8r8g8b8. Pass `mkadt --fb BASE:1080:2424:STRIDE` once the shim recon shows it | ADT node optional |
| 0004 | SWEET machine config (SWEET.h, IRQ path, GIC Group 1) | M | `-march=armv8.2-a+crypto` runs on A520/A720/X4 (ARMv9.2 superset); no PAC kept (plain arm64). `SWEET_VTIMER_INTID 19` is only the default, ADT sets 27 (0017). Group-1-only IRQ handling assumes EL3 owns Group 0 (GICD_TYPER.SecurityExtn=1) - same model on Tensor TF-A, **verify in shim recon** (test_gic). GICR stride 0x20000 = GIC-600. `MAX_CPUS 8`, `MAX_CPU_CLUSTERS 1` (4+3+1 shown as one SMP cluster) | runtime ADT |
| 0005 | non-PAC build fix | G | none | - |
| 0006 | MASTER.arm64.sweet (open-source asm/pmap) | G | none | - |
| 0007 | stub Apple-internal headers | G | none | - |
| 0008 | fill closed-lib gaps, kpc stub | G | none (set/way cache loops: per-core only, worked on DynamIQ sweet) | - |
| 0009 | drop Apple-SoC-only exports | G | none | - |
| 0010 | early framebuffer debug | S | hard-coded 0x9c000000 / 1080x2400 in start.s + sweet_early.c would scribble on stallion RAM. **Fixed by 0082**: geometry from `boot_args->Video`, mapped only when non-zero | 0082 |
| 0011 | physmap alignment padding | G | none | - |
| 0012 | keep debug console after TTBR0 | S | TTBR1 alias of 0x9c000000. **Fixed by 0082** (alias of whatever block the loader gave; second alias for the RAM log) | 0082 |
| 0013 | kmem RNG without corecrypto | G | none | - |
| 0014 | kernel HMAC PRNG | G | none (seed = ADT random-seed from mkadt) | - |
| 0015 | unavailable trust cache runtime | G | none | - |
| 0016 | device-tree platform expert | M | IONameMatch `xiaomi,sweet`. **0083** adds a `google,stallion` personality | 0083 |
| 0017 | virtual-timer IRQ from ADT | G | ADT `virtual-timer-intid = 27` (architected PPI 11), cntfrq 24.576 MHz in cpu `timebase-frequency` (mkadt) | ADT |
| 0018 | boot CPU driver (SweetCPU) | M | IONameMatch `qcom,kryo470`. **0083** adds `google,zumapro-cpu` | 0083 |
| 0019 | built-in pthread | G | none | - |
| 0020 | portable AES-CBC | G | none | - |
| 0021 | mockfs ramdisk root | G | none | - |
| 0022 | ramdisk root without IOMedia | G | none | - |
| 0023 | built-in SHA-256 for libkern | G | none | - |
| 0024 | mockfs virtual ramdisk I/O | G | none | - |
| 0025 | built-in AMFI, no entitlements | G | none | - |
| 0026 | built-in HFS | G | none | - |
| 0027 | BearSSL MD5 | G | none | - |
| 0028 | UFS root device (`bsd/dev/sweet_ufs.c`) | M | Command layer is standard UFSHCI (UTRL, polled SCSI, GPT lookup) and should carry over. Assumes the Qualcomm host left enabled/link up (HCE/HCS checks are generic). Exynos UFS: std HCI 0x13200000+0x200, vs_hci 0x13201100, unipro 0x13280000, FMP/ufsp 0x132A0000, phy 0x13204000, sysreg_ufs 0x13020000. Expected deltas (guess, from Linux ufs-exynos): PRDT byte-granularity quirk (UTRD offsets/lengths and PRDT in bytes), FMP-extended PRDT entries (bypass/zero crypto fields), no UTRLCLR, IO-coherency via sysreg, and the UFS S2MPU must pass our DMA buffer. Also: `sweet_ufs_init` is the trigger for log ring/sampler/keys/touch/USB - split into a per-board start table before enabling it on stallion | ADT `/sweet-ufs` absent on stallion; add `compatible` switch later |
| 0029 | unsealed root volume | G | none | - |
| 0030 | UFS writes | M | follows 0028 (generic UFSHCI) | with 0028 |
| 0031 | read-write root | G | none | - |
| 0032 | BearSSL SHA-1 | G | none | - |
| 0033 | BearSSL SHA-384/512 | G | none | - |
| 0034 | UFS DMA above 4 GiB | M | UFSHCI CAP.64AS check is generic; stallion: check CAP and the S2MPU window | with 0028 |
| 0035 | AMFI entitlements, platform binaries | G | none | - |
| 0036 | AMFI CS_SIGNED | G | none | - |
| 0037 | config: bindfs | G | none | - |
| 0038 | AppleKeyStore | G | none | - |
| 0039 | coredump default off | G | none | - |
| 0040 | IOWatchdog (software) | G | none | - |
| 0041 | HFS link notes | G | none | - |
| 0042 | short exec log | G | none | - |
| 0043 | console crash log | G | none | - |
| 0044 | crash log reason | G | none | - |
| 0045 | IOWatchdog PMU primary | G | none | - |
| 0046 | IOSurface + IOMobileFramebuffer (AppleCLCD) | M | software compositor into the `sweet-fbcon` buffer. Needs a scanning CPU-visible FB on stallion (ABL's, if DECON stays in video mode - open question) else a DECON0/DSIM0 driver (0x19470000 / 0x19440000). mkadt emits `disp0` only with `--fb` | ADT `disp0` |
| 0047 | persistent console log ring | M | 4 MiB ring is generic; persistence = sweet_ufs writes to the userdata tail. Stallion milestone 1: ramoops RAM log instead (0082); later the same userdata-tail ring once Exynos UFS works | via 0028 |
| 0048 | /dev/null tee | G | none | - |
| 0049 | IOKit lookup trace | G | none | - |
| 0050 | permissive Sandbox policy | G | none | - |
| 0051 | crash heap strings | G | none | - |
| 0052 | HFS RENAME_SWAP | G | none | - |
| 0053 | crash scan SpringBoard | G | none | - |
| 0054 | process sampler | G | none (started from UFS init) | - |
| 0055 | AKS ext state, sampler depth | G | none | - |
| 0056 | throttled FB present | G | rate tuning only (DECON refresh 60/120 Hz) | - |
| 0057 | no external resolver | G | none | - |
| 0058 | HID event services (IOHIDFamily-1915) | G | none | - |
| 0059 | iPhone X identity | G | none (mkadt stallion keeps D22AP / iPhone10,3) | - |
| 0060 | keys + touch drivers | S | sweet: SPMI PMIC PON (0x0c440000/0x0e600000/0x0c40a000) + GENI SE7 I2C (0x00a84000), TLMM. Stallion: **buttons = gpio-keys** power GPA4[0], vol-down GPA4[1], vol-up GPA6[2] in the ALIVE pinctrl @0x154D0000 (EINT, active low; bank offsets from the Linux gs101/zuma pinctrl table - read GPx_DAT polled, no writes). **Touch = FocalTech FT3683U over SPI spi_20** (USI HSI0 USI2 @0x111D0000; reset GPP1[1] @pinctrl 0x10840000, irq GPN0[0]); new USI-SPI + FTS report parser driving the existing SweetHIDTouch | started only by sweet_ufs |
| 0061 | iPhone X display size | M | panel 1080x2400/395 ppi -> stallion 1080x2424 (~6.3"); canvas 1125x2436 centering and touch mapping constants | - |
| 0062 | touch power (RPMh) + battery (SPMI) | S | RPMh L11C vote + PM6150 SPMI ADC. Stallion: touch rails VLDO25M 1.8 V / VLDO4M 3.3 V on s2mpg14/15, reachable only via **ACPM** (AP2APM mailbox 0x15110000, APM SRAM 0x15700000, ipc-ch 2 for the PMIC buses) - first check whether ABL leaves them on. Battery: fake 100 % (default) until a fuel-gauge driver exists | SweetBattery real mode only with `sweetbattery=1` |
| 0063 | Goodix GENI FIFO packing | S | not used on stallion | with 0060 |
| 0064 | PRIV_WORK_INTERVAL grant | G | none | - |
| 0065 | SMP via PSCI | M | PSCI smc CPU_ON is generic; `sweet_secondary_entry` already handles EL2 entry (Pixel firmware enters secondaries at EL2: sets HCR/CNTVOFF/ICC_SRE_EL2). MPIDR = Aff1 core id 0x000-0x700 matches zumapro. Note: the shim's CPU7 **sentinel** (b5550ed) owns MPIDR 0x700 - keep `cpus<=7` while it runs | `cpus=N` (default 1) |
| 0066 | crash fault region | G | none | - |
| 0067 | console mute | G | none | - |
| 0068 | surface lookup cached | G | none | - |
| 0069 | faster UFS (spin/sleep, cache) | M | generic UFSHCI polling; follows 0028 | with 0028 |
| 0070 | pid_hibernate syscall | G | none | - |
| 0071 | touch edge flags | G | edge width in pixels, re-tune for 2424 lines | - |
| 0072 | USB probe | S | sweet: GCC GDSC/clocks, DWC3 0x0a600000, QUSB2 0x088e2000. Stallion: DWC3 core 0x11210000 (+0x10000; global regs at +0xc100 as usual), USBDRD PHY 0x11100000.., eUSB2 repeater on hsi2c_9 addr 0x3E, power domain pd_hsi0, CMU_HSI0 clocks. ABL fastboot uses it, so probably left powered - probe read-only first, as on sweet | started by sweet_ufs |
| 0073 | USB device mode EP0 | M | DWC3 programming is generic; base from ADT instead of constant | with 0072 |
| 0074 | USB VBUS override | S | Qualcomm QSCRATCH 0x0a6f8800. Exynos: force VBUSVALID/BVALID in the USBDRD PHY link-control registers (Linux phy-exynos-usbdrd) | with 0072 |
| 0075 | USB CDC-ECM ue0 | G | none | - |
| 0076 | USB rx mbuf | G | none | - |
| 0077 | USB live log | G | none - becomes the main stallion log channel once USB works | - |
| 0078 | all 8 cores | M | 6xA55+2xA76 comments/limits; zumapro 4xA520+3xA720+1xX4 = same MPIDR shape. Exposed as one homogeneous cluster (fine for bring-up) | `cpus=N` |
| 0079 | display power (fade) | G | software only; real panel off = DSIM DCS + backlight later | - |
| 0080 | net trace | G | none | - |
| 0081 | (upstream main only, fdad0de) USB link state | G | none; not in the fork yet - the new stallion patches start at 0082 to avoid a number clash | - |
| **0082** | **stallion milestone 1: board-neutral early console, RAM log, PSCI reset** | new | see below | ADT + boot_args |
| **0083** | **stallion milestone 1: GIC quiesce, IOKit personalities** | new | see below | ADT |

Count: 60 G, 7 S, 13 M among 0001-0080 (the S/M rows are exactly the
places that touch hardware: early FB, platform/CPU match, UFS, FB/IOMFB,
keys/touch/power, SMP, USB).

### Hardware deltas in one place

| Item | sweet | stallion | Where it lands |
|---|---|---|---|
| GIC | GICD 0x17a00000, GICR 0x17a60000 | GICD 0x10400000 (+0x10000), GICR 0x10440000 (+0x200000, 8 x 0x20000) | mkadt (done) |
| Timer | vtimer INTID 19, 19.2 MHz | INTID 27, 24.576 MHz | mkadt (done) |
| CPUs | 6xA55 + 2xA76, MPIDR n<<8 | 4xA520 + 3xA720 + 1xX4, MPIDR n<<8, PSCI smc, EL2 entry | mkadt (done), 0083 match |
| RAM | 0xb0000000 / highmem 0x1_00000000 | DRAM 0x80000000, 8 GiB; carve-outs gsa 0x90200000, gxp/tpu 0x913-0x940, aoc 0x94000000+48M, xhci_dma 0x97000000+4M, seclog 0xC3000000, cp 0xE8000000-0xEB000000, cp_msi 0xF6200000, dss/ramoops/bldr 0xfd3f0000-0xfd900000 | mkadt default 0xA0000000+544M (guess-safe window); confirm with Android `/proc/iomem` (stallion-run.sh pulls it) and move to the upper bank |
| Early debug | FB 0x9c000000 + UFS log ring | **ramoops console zone 0xfd3ff000 (2 MiB)**, read via `/sys/fs/pstore/console-ramoops-0` (tentative: needs warm reset to keep DRAM and ABL not to wipe it) | 0082 + mkadt |
| Panic | spin with panic on screen | PSCI SYSTEM_RESET (smc 0x84000009) after 3 s; shim CPU7 sentinel resets hangs | 0082 + shim |
| UFS | Qualcomm UFSHCI 0x01d84000 | Exynos UFS 0x13200000 (+vs_hci/unipro/FMP), S2MPU | future patch |
| FB | cont_splash 0x9c000000 1080x2400 | ABL-injected, ~0xfac00000? 1080x2424, DECON0 0x19470000 | `--fb` once known |
| Buttons | SPMI PMIC PON | gpio-keys GPA4[0]/GPA4[1]/GPA6[2] (ALIVE pinctrl 0x154D0000) | future patch |
| Touch | Goodix GT9896 on GENI SE7 I2C | FocalTech FT3683U on USI SPI spi_20 0x111D0000 | future patch |
| Regulators | RPMh (BCM/ARC/VRM) | ACPM IPC (mailbox 0x15110000, SRAM 0x15700000) -> s2mpg14/15 | future patch |
| USB | DWC3 0x0a600000 + QSCRATCH + QUSB2 | DWC3 0x11210000 + Exynos USBDRD PHY 0x11100000 + eUSB2 repeater | future patch |
| Reset reg | PS_HOLD 0x0c264000 | PSCI only (PMU 0x15460000 has SWRESET; not used) | 0082 |

## Milestone 1 design: "same kernel prints to a RAM log on stallion"

Goal: `fastboot boot` one image (shim + appended SWEETXNU payload), XNU runs
as far as it can, every early mark / kprintf / console line lands in the
ramoops console zone, and the phone always comes back to Android by itself
(panic -> PSCI reset; hang -> shim sentinel reset). Then
`tools/stallion-run.sh` pulls `/sys/fs/pstore/console-ramoops-0`.

Smallest set of changes (all implemented on this branch):

1. **Shim** (`shim/start.S`, `shim/loader.c`, `shim/shim.c`)
   - EL2 -> EL1 drop now also: HCR_EL2 first (clears E2H), `CNTVOFF_EL2 = 0`
     (XNU uses CNTV; its secondaries zero it too), `VTTBR_EL2 = 0`,
     `ICC_SRE_EL2 |= SRE|Enable` (without Enable, XNU's ICC_SRE_EL1 access in
     pe_init_fiq traps to EL2 and hangs).
   - boot_args->Video = the FB found in the live FDT (complete node with
     w/h/stride only), else 0 - XNU then never touches a guessed FB.
   - Overlap checks: whole appended payload (not just its first byte) and
     the shim image itself vs. [phys_base, phys_base+mem_size).
   - Before jumping: `sentinel_kick(SENTINEL_XNU_S)` (default 120 s) so the
     CPU7 sentinel (b5550ed) resets the phone if XNU hangs anywhere,
     including before XNU has its own panic path.
2. **XNU 0082** (board neutral; sweet unchanged because its ADT has none of
   the new properties and its shim passes the same Video values):
   - `osfmk/arm64/start.s`: stage-0 band and the V=P FB block from
     `boot_args->Video`; skipped when 0, never overwrites a used V=P slot.
   - `osfmk/arm64/sweet_early.c`: FB geometry from boot_args; RAM log writer
     (ramoops prz format, wraps like Linux, continues after the shim's text);
     V=P Device block for the zone right after `PE_init_platform`
     (`sweet_early_attach`), 4 KiB replay buffer for earlier output; reset
     policy (`sweet_panic_reset`, `sweet_reset_watchdog_start`) and PSCI
     SYSTEM_RESET.
   - `osfmk/arm64/arm_vm_init.c`: TTBR1 aliases for FB (+32 MiB) and RAM
     log (+64 MiB) below gVirtBase, each only if the slot is free.
   - `pexpert/arm/pe_serial.c` + `protos.h`: `SERIAL_SWEET_RAMLOG` serial
     device (permanent `ml_io_map`), so kprintf/printf/panic log all go there.
   - `osfmk/kern/debug.c` `panic_spin_forever`, `osfmk/arm64/machine_routines.c`
     `halt_all_cpus`: call `sweet_panic_reset()` (no-op unless the ADT asks).
   - `osfmk/arm/arm_init.c`: `sweet_early_setup(args)` at entry,
     `sweet_early_attach(args)` after `PE_init_platform`.
   - `SweetPlatformExpert::start`: arms the optional reset watchdog.
3. **XNU 0083**: GIC quiesce (ADT `sweet-quiesce`), IOKit personalities
   for `google,stallion` / `google,zumapro-cpu`.
4. **`tools/mkadt.py --target stallion`**: GIC/timer/CPU constants from the
   DTS, RAM 0xA0000000+0x22000000 (override `--mem`), `/sweet-ramlog`
   0xfd3ff000+0x200000, `/chosen sweet-panic-reset = 3000`,
   `sweet-reset-after = 0` (sentinel does it), no `/sweet-ufs`, no FB unless
   `--fb`, root compatible `D22AP|iPhone10,3|google,stallion|google,zumapro`.
   sweet output is byte-identical to before (checked with a fixed seed for
   default, `--qemu`, `--highmem`).

Expected RAM log after one run (success criterion):
shim recon lines -> `--- SWEET XNU RAM log ---` -> `SWEET EARLY 1 .. 20`
marks -> `kprintf initialized`, `SWEET GIC: virtual timer INTID 27`,
`SWEET GIC: quiesced N SPIs` -> IOKit (`SWEET: starting device-tree platform
expert`) -> BSD waits for a root device (none given) -> sentinel reset at
120 s. Any panic before that ends in `SWEET: panic: PSCI SYSTEM_RESET`.
The last mark reached tells where it died if the log simply stops.

Build/run (needs a CI kernel with 0082/0083; nothing was built yet):

    python3 tools/mkadt.py shim/out-st/m1.adt --target stallion   # --mem B:S from /proc/iomem
    python3 tools/mkpayload.py kernel.development.sweet shim/out-st/m1.adt \
        shim/out-st/payload-m1.bin --cmdline "debug=0xc serial=3 cpus=1 keepsyms=1"
    shim/build-stallion.sh shim/out-st/payload-m1.bin
    tools/stallion-run.sh shim/out-st/shim-boot-st.img

### Uncertain / to verify (in this order)

1. **RAM window** 0xA0000000-0xC2000000 is a guess from the kernel DTS;
   TrustZone/GSA/DSS holes are invisible there. Use `/proc/iomem` "System
   RAM" from the recon pull (or the live FDT /memory dump) before the
   first XNU run.
2. **pstore survival**: does ABL keep 0xfd3ff000 across a PSCI
   SYSTEM_RESET and does Android's ramoops show it? Milestone A (shim only)
   answers this; if not, SYSTEM_RESET2 warm (0xC4000012/0x84000012, type 0)
   or the exynos PMU swreset are alternatives.
3. **Kernel size in a v4 `fastboot boot`**: shim + ~40-60 MiB payload; ABL
   may cap the kernel size or load address window. A RELEASE kernel or a
   stripped payload helps.
4. **Where ABL loads the Image**: the loader refuses to boot if the shim or
   payload overlaps XNU RAM (logged as "shim overlaps kernel memory").
5. **GIC**: SecurityExtn / Group handling identical to sweet assumed; GICR
   frame size 0x20000 assumed (GIC-600 without VLPI).
6. **ICC_SRE_EL2** write at EL2: fine architecturally; if ABL runs pKVM-like
   EL2 code that is ignored. Not tested.
7. **FB**: none until the live FDT shows one. XNU's early FB code is
   untested with a non-sweet geometry.
8. The VM_MIN+64 MiB TTBR1 slot for the RAM-log alias is new; if not free
   the log pauses between arm_vm_init and serial_init (marks 8-16 only on
   FB), never corrupts anything.
9. Local verification only: the series (0001-0083) applies to vanilla
   xnu-8796.141.3 sources without fuzz (`patch -F0`, every touched file), and the 10
   touched compilation units compile to objects with Xcode clang against
   the CI build tree of run 39158169212 (`tools/xnu-cc.py` method, objects
   + undefined-symbol check). No CI build, no link, no boot.

## After milestone 1 (suggested order)

1. Live memory map -> `--mem` on the upper bank; `cpus=7` (sentinel keeps CPU7).
2. Exynos UFS variant of sweet_ufs (read-only first, `ufsroot=` probe), with
   the per-board driver start table; then the userdata-tail log ring (0047)
   works on stallion too.
3. FB: ABL framebuffer if scanning, else DECON/DSIM.
4. Buttons (ALIVE GPIO polled), then USB (DWC3 + Exynos PHY) for live log
   and networking, then ACPM regulators + FocalTech SPI touch.

## USB (live FDT facts, 2026-10-19)
- usb@11210000 `samsung,exynos9-dwusb`, dr_mode peripheral, power-domain pd_hsi0, sysreg-hsi0,
  is_not_vbus_pad=1 (software VBUS/session override needed), maximum-speed super-speed, force_gen1.
- **DMA through a SysMMU** (`iommus`, dma-window 0..0xf_ffffffff) and S2MPU (`s2mpus` on the PHY):
  shim/XNU DMA by physical address only works if the SysMMU is bypassed/disabled after ABL.
- phy@11100000 `samsung,exynos-usbdrd-phy` regs 0x11100000/0x11110000/0x11120000 (0x200 each),
  0x11130000/0x11140000 (0x800, combo/DP), phy_version 0x600, eusb 0x701, sub 0x801, ref clock
  19.2 MHz (0x124f800), **PMU isolation at PMU+0x3eb0 (USB) / +0x3eb4 (DP)**.
- eUSB2 repeater: I2C device 0x3E (`samsung,eusb-repeater`) with tune params.
- Plan: dump these blocks from Android via sweetmem.ko (Linux-configured, gadget running) and from the
  shim after handoff (timing channel), diff, replay the missing steps in shim/usblog.c.
