#!/usr/bin/env python3
"""Build the minimal Apple Device Tree that the SWEET xnu needs to boot.

Only what the early boot path actually reads (see NOTES.md). One kernel
serves every board; the ADT says which hardware is there.

  mkadt.py OUT.adt [--dump] [--qemu] [--highmem]          sweet (Redmi Note 10 Pro)
  mkadt.py OUT.adt --target stallion [--mem BASE:SIZE] [--fb BASE:W:H:STRIDE]
           [--panic-reset MS] [--reset-after S] [--dump]  Pixel 10a (Tensor G4)

sweet MMIO comes from the sweet live FDT; stallion constants come from the
zumapro/stallion DTS (STALLION-PORT.md, research/stallion-dts/).
"""
import hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

# Kernel-managed RAM: above every Qualcomm XPU carve-out (see NOTES.md)
PHYS_BASE = 0xb0000000
MEM_SIZE = 0x4d000000
# --highmem (phone only): the whole upper DDR bank. The live FDT memory node is
# <0x80000000 2G>, <0x1_00000000 0xfcc00000>; every fixed reserved-memory
# carve-out is in the low bank and the dynamic pools are limited below 4G.
HIGH_PHYS_BASE = 0x100000000
HIGH_MEM_SIZE = 0xfcc00000
# /arm-io children are relative to this; it must not be 0 (serial_init bails)
SOC_BASE = 0x01000000

GICD, GICD_SIZE = 0x17a00000, 0x10000
GICR, GICR_SIZE = 0x17a60000, 0x100000
FB, FB_SIZE = 0x9c000000, 0x1700000
SWEET_ECID = 0x5357454554000001  # 'SWEET' + 1, synthetic
FB_W, FB_H, FB_STRIDE = 1080, 2400, 1080 * 4
# Internal UFS host (ufshc@1d84000 "ufs_mem"); QEMU virt has none
UFS, UFS_SIZE = 0x01d84000, 0x3000
TIMEBASE = 19200000
VTIMER_INTID = 19
# QEMU virt (-M virt,gic-version=3 -cpu cortex-a76): GICD/GICR and the generic timer's CNTFRQ
QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE = 0x08000000, 0x080a0000, 62500000
QEMU_VTIMER_INTID = 27

BOOT_MANIFEST_HASH = hashlib.sha384(b'sweet boot manifest').digest()

# --- Pixel 10a "stallion" (Tensor G4 / zumapro), from the stallion DTS -----
# Kernel-managed RAM (TENTATIVE): the largest window of the first DRAM bank
# (memory@80000000, 2 GiB node; ABL patches the real banks, 8 GiB total) that
# no zumapro-rmem.dtsi carve-out touches: after aoc (0x94000000+48M) and the
# xhci_dma pool (0x97000000+4M), before seclog_mem (0xC3000000). Secure/TZ
# holes are not in the kernel DTS: check the live FDT /memory node (shim
# recon fdt_dump) before trusting this, and move to the upper bank with
# --mem once its base is known.
ST_PHYS_BASE = 0xa0000000
ST_MEM_SIZE = 0x22000000          # 544 MiB, ends 16 MiB below seclog_mem (0xC3000000)
ST_GICD, ST_GICD_SIZE = 0x10400000, 0x10000
ST_GICR, ST_GICR_SIZE = 0x10440000, 0x200000   # 8 redistributors x 0x20000 (GIC-600)
ST_TIMEBASE = 24576000            # armv8-timer clock-frequency
ST_VTIMER_INTID = 27              # architected PPI 11 (not Qualcomm's 19)
# ramoops_mem@fd3ff000: no record-size, so the console zone (0x200000) is first
ST_RAMLOG, ST_RAMLOG_SIZE = 0xfd3ff000, 0x200000
ST_PANIC_RESET_MS = 3000          # panic -> PSCI SYSTEM_RESET after 3 s
ST_RESET_AFTER_S = 0              # in-kernel hang reset off: the shim's CPU7 sentinel does it
# zumapro-cpu.dtsi: cpu0-3 Cortex-A520, cpu4-6 Cortex-A720, cpu7 Cortex-X4,
# MPIDR = n << 8 (Aff1); one XNU cluster like sweet (MAX_CPU_CLUSTERS 1).
ST_CPUS = [('arm,cortex-a520', 1950000000)] * 4 + [('arm,cortex-a720', 2600000000)] * 3 + \
    [('arm,cortex-x4', 3100000000)]

# The iOS userland is the iPhone X (iPhone10,3 / D22AP) IPSW. MobileGestalt
# derives product type, device class, home-button-less UI, artwork etc. from
# the root identity and /product of the real D22AP device tree. That tree is
# an Apple artifact (kept out of git): extract it from the IPSW with
#   tools/remotezip.py URL DeviceTree.d22ap.im4p DIR
#   ipsw img4 im4p extract DIR/Firmware/all_flash/DeviceTree.d22ap.im4p -o d22ap.adt
D22AP_ADT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../research/ipsw-x/d22ap.adt')


def d22ap_product(path=D22AP_ADT):
    """/product properties of the real D22AP tree, without the syscfg
    placeholders iBoot would fill in (flag bit set) and the node bookkeeping."""
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def build(seed=None, qemu=False, highmem=False, target='sweet', mem=None, fb=None,
          panic_reset_ms=None, reset_after_s=None):
    """qemu=True: same tree, but with QEMU virt's GIC addresses and CNTFRQ (tools/qemu-sweet.py).
    target='stallion': Pixel 10a; mem=(base, size), fb=(base, w, h, stride) override defaults."""
    st = target == 'stallion'
    assert target in ('sweet', 'stallion') and not (st and (qemu or highmem))
    phys_base, mem_size = (HIGH_PHYS_BASE, HIGH_MEM_SIZE) if highmem else (PHYS_BASE, MEM_SIZE)
    gicd, gicr, timebase = (QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE) if qemu else (GICD, GICR, TIMEBASE)
    gicd_size, gicr_size = GICD_SIZE, GICR_SIZE
    vtimer = QEMU_VTIMER_INTID if qemu else VTIMER_INTID
    if st:
        phys_base, mem_size = mem or (ST_PHYS_BASE, ST_MEM_SIZE)
        gicd, gicr, timebase = ST_GICD, ST_GICR, ST_TIMEBASE
        gicd_size, gicr_size, vtimer = ST_GICD_SIZE, ST_GICR_SIZE, ST_VTIMER_INTID
        assert phys_base % 0x2000000 == 0 and mem_size % 0x2000000 == 0, 'RAM must be 32MB-aligned'
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('firmware-version', cstr('sweet-shim')),
        ('system-firmware-version', cstr('sweet-shim')),
        # iBoot's SHA-384 of the boot manifest; /sbin/mount -P 1 exits unless
        # it exists (bind mounts /private/preboot/<hex>/usr/standalone/firmware,
        # tolerating a missing directory). Fixed stand-in value.
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        # libimg4 (cryptexd) aborts "failed to get crypto algorithm" without
        # it; real A11+ device trees say sha2-384.
        ('crypto-hash-method', cstr('sha2-384')),
        # MobileGestalt's screen class: display-scale (u32, default 0) comes
        # from /chosen, the pixel size from IOMobileFramebuffer. Scale 0 makes
        # SpringBoard's FBSDisplayMode "infxinf@0x" invalid. 1080x2400 @3x =
        # 360x800 points (like iPhone X class devices).
        ('display-scale', u32(3)),
        # XNU derives IOPlatformUUID (gethostuuid) from these on arm; without
        # them CFPreferences cannot use kCFPreferencesCurrentHost (no ByHost
        # files), so e.g. DataMigration never records LastSystemVersion and
        # migrates on every boot. Synthetic ECID (not the phone's); chip-id
        # of the A11 that the iPhone X userland expects.
        ('unique-chip-id', struct.pack('<Q', SWEET_ECID)),
        ('chip-id', u32(0x8015)),
        # Security status as on a dev-fused, undemoted board (tuple 0,0,1,1):
        # MobileGestalt reads these from /chosen, and mobileactivationd's
        # DeviceType then takes its native "Hactivation" branch (state
        # Activated without an activation record; no SEP, no baseband here).
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ] + ([
        # osfmk/arm64/sweet_early.c: panic -> PSCI SYSTEM_RESET after N ms
        # (boot-arg sweetpanicreset), unconditional reset N s after IOKit
        # platform start (boot-arg sweetreset; 0 = off).
        ('sweet-panic-reset', u32(ST_PANIC_RESET_MS if panic_reset_ms is None else panic_reset_ms)),
        ('sweet-reset-after', u32(ST_RESET_AFTER_S if reset_after_s is None else reset_after_s)),
    ] if st else []), [node('memory-map', [ph()]),
        # Local boot policy (kern_csr.c): SIP relaxed like `csrutil disable`
        # (lp-sip0: untrusted kexts, unrestricted FS, task_for_pid, kernel
        # debugger, dtrace, nvram), lp-sip1: unauthenticated (unsealed) root.
        # The iOS shared cache is in no trust cache, so dyld can only map it
        # with CSR_ALLOW_UNRESTRICTED_FS (vm_unix.c shared region checks).
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # SWEET SMP: all eight cores in physical-core order (0-5 Cortex-A55,
    # 6-7 Cortex-A76), one homogeneous cluster. The kernel defaults to one
    # and uses cpus=N to select an initial prefix (N <= 8).
    # QEMU retains its original single-CPU tree: its MPIDRs differ from sweet.
    cpu_nodes = []
    for core in range(1 if qemu else 8):
        # IONameMatch for SweetCPU (iokit/KernelConfigTables.cpp)
        if st:
            compat = cstr('google,zumapro-cpu') + cstr(ST_CPUS[core][0]) + cstr('ARM,v8')
            freq = ST_CPUS[core][1]
        else:
            compat = cstr('qcom,kryo470') + cstr('ARM,v8')
            freq = 2304000000 if core >= 6 else 1804800000
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', compat),
            ('reg', u32(core << 8)),  # actual MPIDR Aff1:Aff0, NOT logical ID
            ('cpu-id', u32(core)),   # descriptive: XNU assigns IDs by DT order
            ('cluster-id', u32(0)),  # descriptive; see ml_parse_cpu_topology
            ('cluster-type', u32(0)),  # homogeneous CLUSTER_TYPE_SMP, not AMP
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(core)),  # distinct per-core L2 nodes in live FDT
            ('l3-cache-id', u32(0)),     # shared L3
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(freq)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1866000000)),
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)),
                         ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(vtimer)),
        ('reg', u64(gicd - SOC_BASE, gicd_size, gicr - SOC_BASE, gicr_size)),
    ] + ([
        # pexpert/arm/pe_fiq.c: disable every non-secure SPI and all PPIs but
        # the vtimer/IPI before enabling Group 1 (nothing is IRQ driven yet;
        # Pixel ABL state is unknown, a stuck level SPI would storm sleh_irq).
        ('sweet-quiesce', u32(1)),
    ] if st else []))
    # Main display: IOMobileFramebuffer.framework counts /arm-io children
    # with this device_type and reads the canvas size from the node
    # (the presented iPhone X size, see SweetFramebuffer.cpp);
    # the kernel's AppleCLCD matches the compatible string.
    # stallion: AppleCLCD needs /sweet-fbcon, so only with --fb.
    mk_disp0 = lambda: node('disp0', [
        ph(),
        ('device_type', cstr('display-subsystem')),
        ('compatible', cstr('sweet,display')),
        ('canvas-width', u32(1125)),
        ('canvas-height', u32(2436)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('sweet-io')),
        ('compatible', cstr('sweet-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic,
        # No hardware: watchdogd checks IODeviceTree:/arm-io/wdt for no-pmu and
        # otherwise crashes trying to set the PMU boot stage (customer builds
        # ignore the -wdt_no_pmu boot-arg).
        node('wdt', [ph(), ('no-pmu', u32(1))])] + ([mk_disp0()] if (fb or not st) else []))

    fb_base, fb_w, fb_h, fb_stride = fb if fb else (FB, FB_W, FB_H, FB_STRIDE)
    fb_size = FB_SIZE if not fb else (fb_h * fb_stride + 0x3fff) & ~0x3fff
    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(fb_base, fb_size)),
        ('width', u32(fb_w)),
        ('height', u32(fb_h)),
        ('stride', u32(fb_stride)),
    ])

    # osfmk/arm64/sweet_early.c + pexpert/arm/pe_serial.c: console bytes go
    # to the ramoops console zone; Android shows them after the next warm
    # boot as /sys/fs/pstore/console-ramoops-0.
    mk_ramlog = lambda: node('sweet-ramlog', [
        ph(),
        ('compatible', cstr('ramoops-prz')),
        ('reg', u64(ST_RAMLOG, ST_RAMLOG_SIZE)),
    ])

    # bsd/dev/sweet_ufs.c: the UFS host ABL leaves running (absolute physical reg).
    ufs = node('sweet-ufs', [
        ph(),
        ('compatible', cstr('qcom,ufshc')),
        ('reg', u64(UFS, UFS_SIZE)),
    ])

    # pmap_compute_max_asids requires the node even when no overrides are set.
    # An absent pmap-max-asids property selects XNU's own MAX_ASIDS default.
    # No effaceable storage: keybagd then reads /private/var/keybags/systembag.kb
    # as a plain plist instead of decrypting it with the effaceable key.
    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    # MobileGestalt HomeButtonType = IODeviceTree:/buttons home-button-type
    # (D22AP: 2, no Home button; absent = 0 = Home-button device).
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])

    product_props = d22ap_product()
    product = [node('product', [ph()] + product_props)] if product_props else []

    # Identity of the iPhone X whose userland runs here: compatible[0] is
    # hw.model/hw.target (D22AP), model is hw.machine/hw.product
    # (iPhone10,3). 'xiaomi,sweet' stays for SweetPlatformExpert matching.
    if st:
        # Platform expert match: 'google,stallion' (KernelConfigTables.cpp).
        # No /sweet-ufs yet: bsd/dev/sweet_ufs.c is Qualcomm UFS, and its init
        # also starts the SPMI keys / GENI touch / USB drivers.
        return node('device-tree', [
            ph(),
            ('compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr('google,stallion') + cstr('google,zumapro') + cstr('AppleARM')),
            ('model', cstr('iPhone10,3')),
            ('target-type', cstr('D22')),
            ('target-sub-type', cstr('D22AP')),
            ('manufacturer', cstr('Apple Inc.')),
            ('#address-cells', u32(2)),
            ('#size-cells', u32(2)),
        ], [chosen, defaults, cpus, arm_io, mk_ramlog(), buttons] + product + ([fbcon] if fb else []))

    return node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr('xiaomi,sweet') + cstr('qcom,sm7150') + cstr('AppleARM')),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons] + product + ([] if qemu else [ufs]))


def _opt(name, conv=lambda v: int(v, 0)):
    if name not in sys.argv:
        return None
    return conv(sys.argv[sys.argv.index(name) + 1])


if __name__ == '__main__':
    ints = lambda v: tuple(int(x, 0) for x in v.split(':'))
    root = build(qemu='--qemu' in sys.argv, highmem='--highmem' in sys.argv,
                 target=_opt('--target', str) or 'sweet', mem=_opt('--mem', ints),
                 fb=_opt('--fb', ints), panic_reset_ms=_opt('--panic-reset'),
                 reset_after_s=_opt('--reset-after'))
    blob = adt.serialize(root)
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes', file=sys.stderr)
