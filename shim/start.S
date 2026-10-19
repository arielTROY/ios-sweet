/*
 * sweet boot shim entry: arm64 Linux Image header so Qualcomm ABL treats
 * us like a kernel. ABL enters at EL1, MMU/D-cache off, x0 = DTB pointer.
 */
	.section .text.head, "ax"
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0			/* text_offset */
	.quad	_end			/* image_size (linked at 0) */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0			/* res2 */
	.quad	0			/* res3 */
	.quad	0			/* res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 (PE offset) */

	.text
	.globl _start
_start:
	msr	daifset, #0xf
#ifdef SHIM_DIAG
	/*
	 * Diag mode (sweetjump "go" from Linux with x0 = DIAG_MAGIC): run
	 * diag_main() as a subroutine at EL2, MMU off, on our own stack, and
	 * eret back into the hyp stub's exception context, i.e. to Linux,
	 * with x0 = phys addr of the RAM log and x1 = its length. ELR_EL2 /
	 * SPSR_EL2 must survive; SP_EL2 is saved and restored.
	 */
	movz	x9, #0x4741
	movk	x9, #0x4944, lsl #16
	movk	x9, #0x5753, lsl #32	/* 0x0000_5753_4944_4741 "SWDIAG" */
	cmp	x0, x9
	b.ne	8f
	/* Linux's callee-saved x19-x30 and its shadow-call-stack x18 must
	 * survive: the shim's C code is not built with -ffixed-x18. */
	adr	x9, diag_save
	stp	x18, x19, [x9, #0]
	stp	x20, x21, [x9, #16]
	stp	x22, x23, [x9, #32]
	stp	x24, x25, [x9, #48]
	stp	x26, x27, [x9, #64]
	stp	x28, x29, [x9, #80]
	str	x30, [x9, #96]
	mov	x19, sp			/* stub's SP_EL2 */
	mrs	x20, elr_el2
	mrs	x21, spsr_el2
	adr	x9, _stack_top
	mov	sp, x9
	adr	x9, _bss_start		/* zero .bss */
	adr	x10, _bss_end
1:	cmp	x9, x10
	b.hs	2f
	str	xzr, [x9], #8
	b	1b
2:	bl	diag_main		/* returns x0 = log PA, x1 = len */
	msr	elr_el2, x20
	msr	spsr_el2, x21
	mov	sp, x19
	adr	x9, diag_save
	ldp	x18, x19, [x9, #0]
	ldp	x20, x21, [x9, #16]
	ldp	x22, x23, [x9, #32]
	ldp	x24, x25, [x9, #48]
	ldp	x26, x27, [x9, #64]
	ldp	x28, x29, [x9, #80]
	ldr	x30, [x9, #96]
	isb
	eret
8:
#endif
#if defined(PROBE6) || defined(PROBE7)
	/* DRAM survival in DSS crash-dump carve-outs (no-map, only written
	 * on a real crash dump): markers at log_slcdump+1M and
	 * log_preslcdump+1M. PROBE6 exits via watchdog, PROBE7 via PSCI. */
	movz	x9, #0x4d45
	movk	x9, #0x4d54, lsl #16
	movk	x9, #0x4545, lsl #32
	movk	x9, #0x5753, lsl #48
	movz	x2, #0x0000
	movk	x2, #0xdb51, lsl #16	/* 0xdb510000 */
	str	x9, [x2]
	str	x2, [x2, #8]
	movz	x2, #0x0000
	movk	x2, #0xdc61, lsl #16	/* 0xdc610000 */
	str	x9, [x2]
	str	x2, [x2, #8]
	dsb	sy
#ifdef PROBE6
	b	.
#endif
#define CANARY0
#endif
#if defined(PROBE4) || defined(PROBE5)
	/* Does DRAM survive our exit? Write "SWEETMEM" + address at three
	 * spots; PROBE4 then PSCI-resets, PROBE5 lets the watchdog fire.
	 * Read back from Android with sweetmem.ko. */
	movz	x9, #0x4d45
	movk	x9, #0x4d54, lsl #16
	movk	x9, #0x4545, lsl #32
	movk	x9, #0x5753, lsl #48	/* "SWEETMEM" little-endian? see below */
	movz	x2, #0x0000
	movk	x2, #0xc000, lsl #16
	movk	x2, #0x9, lsl #32	/* 0x9c0000000 */
	str	x9, [x2]
	str	x2, [x2, #8]
	movz	x2, #0x0000
	movk	x2, #0xf000, lsl #16
	movk	x2, #0x9, lsl #32	/* 0x9f0000000 */
	str	x9, [x2]
	str	x2, [x2, #8]
	movz	x2, #0x0000
	movk	x2, #0x8010, lsl #16
	movk	x2, #0x8, lsl #32	/* 0x880100000 */
	str	x9, [x2]
	str	x2, [x2, #8]
	dsb	sy
#ifdef PROBE5
	b	.
#endif
#define CANARY0
#endif
#ifdef PROBE3
	/* SysMMU HSI0 (in front of DWC3 DMA): CTRL +0x0 / CTRL_VM(0)
	 * +0x8000, bit 0 = enable. Either set -> reset now (~44 s);
	 * both clear -> feed WDT 60 s, then reset (~104 s). */
	movz	x2, #0x0000
	movk	x2, #0x1104, lsl #16
	ldr	w3, [x2]
	movz	x2, #0x8000
	movk	x2, #0x1104, lsl #16
	ldr	w7, [x2]
	orr	w3, w3, w7
	tbnz	w3, #0, 9f
	movz	x2, #0x0008
	movk	x2, #0x1006, lsl #16
	mrs	x4, cntpct_el0
	movz	x5, #0x57e4, lsl #16	/* 60 s */
	add	x5, x4, x5
	mov	x6, #1
	lsl	x6, x6, #31
1:	movz	w3, #0xffff
	str	w3, [x2]
	subs	x6, x6, #1
	b.eq	9f
	mrs	x4, cntpct_el0
	cmp	x4, x5
	b.lo	1b
9:
#define CANARY0
#endif
#ifdef PROBE2
	/* USB PHY power/isolation (PMU+0x3eb0, bit 0 = enable, as
	 * exynos-usbdrd's pmu_offset). Set -> reset now (~30 s);
	 * clear -> feed WDT 60 s, then reset (~90 s). */
	movz	x2, #0x3eb0
	movk	x2, #0x1546, lsl #16
	ldr	w3, [x2]
	tbnz	w3, #0, 9f
	movz	x2, #0x0008
	movk	x2, #0x1006, lsl #16
	mrs	x4, cntpct_el0
	movz	x5, #0x57e4, lsl #16	/* 60 s */
	add	x5, x4, x5
	mov	x6, #1
	lsl	x6, x6, #31
1:	movz	w3, #0xffff
	str	w3, [x2]
	subs	x6, x6, #1
	b.eq	9f
	mrs	x4, cntpct_el0
	cmp	x4, x5
	b.lo	1b
9:
#define CANARY0
#endif
#ifdef PROBE1
	/* Is ABL's DWC3 still clocked after handoff? Timing answer:
	 * GSNPSID looks like DWC3 -> reset now (~30 s to adb);
	 * readable but odd -> feed WDT 60 s, then reset (~90 s);
	 * read hangs -> ABL's watchdog recovers (much longer). */
	movz	x2, #0xc120
	movk	x2, #0x1121, lsl #16	/* DWC3 GSNPSID */
	ldr	w3, [x2]
	lsr	w3, w3, #16
	mov	w4, #0x5533
	cmp	w3, w4
	b.eq	9f
	mov	w4, #0x3331
	cmp	w3, w4
	b.eq	9f
	mov	w4, #0x3332
	cmp	w3, w4
	b.eq	9f
	/* odd value: feed cl0 WTCNT for 60 s */
	movz	x2, #0x0008
	movk	x2, #0x1006, lsl #16
	mrs	x4, cntpct_el0
	movz	x5, #0x57e4, lsl #16	/* 60 s * 24.576 MHz = 0x57e40000 */
	add	x5, x4, x5
	mov	x6, #1
	lsl	x6, x6, #31
1:	movz	w3, #0xffff
	str	w3, [x2]
	subs	x6, x6, #1
	b.eq	9f
	mrs	x4, cntpct_el0
	cmp	x4, x5
	b.lo	1b
9:
#define CANARY0
#endif
#ifdef CANARY11
	/* Linux-launch proof: take over the watchdogs. cl1 off; cl0 armed
	 * with its longest timeout (prescaler 255, /128, reset on: 0xffff
	 * ticks of 24.576 MHz / 32768 = ~87 s). Then spin. Return after
	 * ~90 s => this code ran; ~20-30 s => Linux's watchdog setup did. */
	movz	x2, #0x0000
	movk	x2, #0x1007, lsl #16	/* WDT cl1 */
	str	wzr, [x2]		/* WTCON = 0 */
	movz	x2, #0x0000
	movk	x2, #0x1006, lsl #16	/* WDT cl0 */
	str	wzr, [x2]		/* WTCON = 0 */
	mov	w3, #1
	str	w3, [x2, #0xc]		/* WTCLRINT */
	movz	w3, #0xffff
	str	w3, [x2, #4]		/* WTDAT */
	str	w3, [x2, #8]		/* WTCNT */
	movz	w3, #0xff39		/* prescaler 0xff, /128, enable, reset */
	str	w3, [x2]
	dsb	sy
#ifdef CANARY12
	/* sweetjump "go" test: return into the stub's exception context. */
	ldr	w0, [x2]
	movz	x1, #0x5eed
	eret
#endif
	b	.
#endif
#ifdef CANARY10
	/* 1-bit channel calibration: reboot mode := bootloader via EL3
	 * PRIV_REG, then PSCI reset. Fastboot = channel works (the CANARY0
	 * baseline without this write lands in Android). */
	movz	x0, #0x0504
	movk	x0, #0x8200, lsl #16
	movz	x1, #0x0810
	movk	x1, #0x1546, lsl #16
	mov	x2, #1
	mov	x3, #0xfc
	mov	x4, #0
	smc	#0
#define CANARY0
#endif
#ifdef CANARY9
	/* Timing proof: feed ABL's armed watchdog (cl0 WTCNT, plain
	 * non-secure write as in s3c2410_wdt) for ~90 s, then stop and let
	 * it fire. If this code runs, the phone comes back ~90 s later than
	 * the CANARY0 baseline. */
	movz	x2, #0x0008
	movk	x2, #0x1006, lsl #16	/* WDT cl0 WTCNT */
	mrs	x4, cntpct_el0
	movz	x5, #0x83d6, lsl #16	/* 90 s * 24.576 MHz = 0x83d60000 */
	add	x5, x4, x5
	mov	x6, #1
	lsl	x6, x6, #31		/* iteration cap if CNTPCT is frozen */
	movz	x7, #0x0008
	movk	x7, #0x1007, lsl #16	/* WDT cl1 WTCNT (Linux arms it too) */
1:	movz	w3, #0xffff
	str	w3, [x2]
	str	w3, [x7]
	subs	x6, x6, #1
	b.eq	2f
	mrs	x4, cntpct_el0
	cmp	x4, x5
	b.lo	1b
2:	b	2b
#endif
#ifdef CANARY8
	/* Clean exit without any reset: hand control back to ABL. ABL's
	 * handoff (0xe2b4c) cleared SCTLR_EL1.M|C on its identity map and
	 * called us from its EL2 stub (blr x7; eret). Restore M|C and
	 * return: the stub erets into ABL, which takes its "kernel
	 * returned" error path (0xe4b0c) and stays in its own world. x30,
	 * ELR_EL2 and SPSR_EL2 must be untouched up to here. */
	mrs	x9, sctlr_el1
	mov	x10, #5			/* M | C */
	orr	x9, x9, x10
	msr	sctlr_el1, x9
	isb
	ret
#endif
#ifdef CANARY7
	/* Reset the way BL31's plat_gs_system_reset does, but through the
	 * EL3 PRIV_REG service and without its notify loop/spinlock (the
	 * likely stall in CANARY5/6): mode := bootloader, then PS_HOLD low
	 * (PMU+0x3e9c bit 8; cold, PMIC restarts: the PIN|PO resets that
	 * always came back to fastboot), then SWRESET (PMU+0x3a00 = 2),
	 * then PSCI. */
	movz	x0, #0x0504
	movk	x0, #0x8200, lsl #16	/* SMC_CMD_PRIV_REG */
	movz	x1, #0x0810
	movk	x1, #0x1546, lsl #16	/* PMU + SYSIP_DAT0 */
	mov	x2, #1			/* write */
	mov	x3, #0xfc		/* REBOOT_MODE_BOOTLOADER */
	mov	x4, #0
	smc	#0
	movz	x0, #0x0504
	movk	x0, #0x8200, lsl #16
	movz	x1, #0x3e9c
	movk	x1, #0x1546, lsl #16	/* PMU + PS_HOLD control */
	mov	x2, #2			/* read-modify-write */
	mov	x3, #0x100		/* mask: bit 8 */
	mov	x4, #0			/* value: low */
	smc	#0
	movz	x0, #0x0504
	movk	x0, #0x8200, lsl #16
	movz	x1, #0x3a00
	movk	x1, #0x1546, lsl #16	/* PMU + SWRESET */
	mov	x2, #1
	mov	x3, #2
	mov	x4, #0
	smc	#0
	movz	x0, #0x0009
	movk	x0, #0x8400, lsl #16	/* PSCI SYSTEM_RESET */
	smc	#0
	b	.
#endif
#ifdef CANARY6
	/* Reset ladder. Reboot mode := bootloader first, so whichever
	 * reset fires (incl. ABL's armed watchdog) should land in fastboot.
	 * Then PSCI SYSTEM_RESET, SYSTEM_RESET2(warm), PMU SWRESET via EL3
	 * PRIV_REG; then spin for the watchdog. RST_STAT / "Reboot reason"
	 * in the next `fastboot oem dmesg` tell which one worked. */
	movz	x0, #0x0504
	movk	x0, #0x8200, lsl #16	/* SMC_CMD_PRIV_REG */
	movz	x1, #0x0810
	movk	x1, #0x1546, lsl #16	/* PMU + SYSIP_DAT0 */
	mov	x2, #1
	mov	x3, #0xfc		/* REBOOT_MODE_BOOTLOADER */
	mov	x4, #0
	smc	#0
	movz	x0, #0x0009
	movk	x0, #0x8400, lsl #16	/* PSCI SYSTEM_RESET */
	mov	x1, #0
	mov	x2, #0
	mov	x3, #0
	smc	#0
	movz	x0, #0x0012
	movk	x0, #0xc400, lsl #16	/* PSCI SYSTEM_RESET2 */
	mov	x1, #0			/* SYSTEM_WARM_RESET */
	mov	x2, #0
	smc	#0
	movz	x0, #0x0504
	movk	x0, #0x8200, lsl #16	/* SMC_CMD_PRIV_REG */
	movz	x1, #0x3a00
	movk	x1, #0x1546, lsl #16	/* PMU + SWRESET (gs101 syscon-reboot) */
	mov	x2, #1
	mov	x3, #2
	mov	x4, #0
	smc	#0
	b	.
#endif
#ifdef CANARY5
	/* Canary: reboot mode := normal (SYSIP_DAT0 via EL3 PRIV_REG
	 * write), then reset. Landing in Android instead of fastboot
	 * proves this code ran. */
	movz	x0, #0x0504
	movk	x0, #0x8200, lsl #16	/* SMC_CMD_PRIV_REG */
	movz	x1, #0x0810
	movk	x1, #0x1546, lsl #16	/* PMU + SYSIP_DAT0 */
	mov	x2, #1			/* write */
	mov	x3, #0			/* REBOOT_MODE_NORMAL */
	mov	x4, #0
	smc	#0
#define CANARY0
#endif
#ifdef CANARY4
	/* Canary: marker A in the DSS EMERGENCY_REASON word, which ABL
	 * prints as "Reboot reason" on the next boot; spin; marker B;
	 * reset. A reason of 0x5e01a000 means we were killed mid-spin. */
	movz	x2, #0x0300
	movk	x2, #0xd810, lsl #16	/* 0xd8100300 */
	movz	w3, #0xa000
	movk	w3, #0x5e01, lsl #16
	str	w3, [x2]
	dsb	sy
	mov	x1, #1
	lsl	x1, x1, #34
1:	subs	x1, x1, #1
	b.ne	1b
	movz	w3, #0xb000
	movk	w3, #0x5e01, lsl #16
	str	w3, [x2]
	dsb	sy
#define CANARY0
#endif
#ifdef CANARY3
	/* Canary: burn ~2^36 iterations (tens of seconds) before the
	 * reset, so a slow return proves this code ran. */
	mov	x1, #1
	lsl	x1, x1, #36
1:	subs	x1, x1, #1
	b.ne	1b
#define CANARY0
#endif
#ifdef CANARY0
	/* Canary: prove the bootloader jumps here, touch nothing, reset. */
	movz	x0, #0x0009
	movk	x0, #0x8400, lsl #16	/* PSCI SYSTEM_RESET */
	smc	#0
	b	.
#endif
	mov	x19, x0			/* DTB pointer from ABL */
	mov	x20, x1			/* sweetjump: payload page list (PA) */
	mov	x21, x2			/* sweetjump: number of pages */
#ifdef TARGET_STALLION
	/* Pixel ABL enters at EL2 (arm64 boot protocol). Drop to EL1h so
	 * the rest of the shim runs the same as on Qualcomm's EL1 entry. */
	mrs	x9, CurrentEL
	cmp	x9, #(2 << 2)
	b.ne	9f
	/* HCR first: RW (EL1 is aarch64), E2H=0, so the EL2 registers below
	 * have their non-VHE layout even if ABL ran with VHE. */
	mov	x9, #(1 << 31)
	msr	hcr_el2, x9
	isb
	mrs	x9, cnthctl_el2
	orr	x9, x9, #3		/* EL1PCTEN | EL1PCEN */
	msr	cnthctl_el2, x9
	/* XNU runs on CNTV; its PSCI secondaries zero CNTVOFF too
	 * (sweet_secondary_entry), so the boot CPU must match. */
	msr	cntvoff_el2, xzr
	msr	vttbr_el2, xzr		/* no stage 2 */
	msr	cptr_el2, xzr		/* no FP/ASIMD traps */
	/* ICC_SRE_EL2: SRE | Enable. Without Enable, XNU's ICC_SRE_EL1
	 * access in pe_init_fiq traps to EL2 (ABL's vectors) and hangs. */
	mrs	x9, S3_4_C12_C9_5
	orr	x9, x9, #0x1		/* SRE */
	orr	x9, x9, #0x8		/* Enable */
	msr	S3_4_C12_C9_5, x9
	isb
	mov	x9, #(3 << 20)		/* CPACR FPEN */
	msr	cpacr_el1, x9
	movz	x9, #0x0800		/* SCTLR_RES1 */
	movk	x9, #0x30d0, lsl #16
	msr	sctlr_el1, x9
	mov	x9, #0x3c5		/* SPSR: EL1h, DAIF masked */
	msr	spsr_el2, x9
	adr	x9, 9f
	msr	elr_el2, x9
	eret
9:
#endif
	msr	spsel, #1
	adr	x1, _stack_top
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	/* zero .bss */
	adr	x1, _bss_start
	adr	x2, _bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b
2:	mov	x0, x19
	mov	x1, x20
	mov	x2, x21
	bl	shim_main
3:	wfe
	b	3b

/*
 * Sentinel core (stallion): PSCI CPU_ON target that resets the whole
 * phone once CNTPCT passes sentinel_deadline, whatever the boot CPU is
 * doing (a load from an unclocked block stalls only that core). No
 * stack, no MMU, works at EL2 or EL1. The boot CPU moves the deadline
 * with sentinel_kick(); 0 means disarmed.
 */
	.globl sentinel_entry
sentinel_entry:
	msr	daifset, #0xf
	mrs	x9, CurrentEL
	cmp	x9, #(2 << 2)
	b.ne	1f
	mrs	x9, cnthctl_el2
	orr	x9, x9, #3
	msr	cnthctl_el2, x9
1:	adr	x2, sentinel_deadline
	movz	x10, #0x0008
	movk	x10, #0x1006, lsl #16	/* WDT cl0 WTCNT */
	movz	x11, #0x0008
	movk	x11, #0x1007, lsl #16	/* WDT cl1 WTCNT */
	movz	w12, #0xffff
2:	isb
	/* Until the deadline keep whoever armed the watchdogs (ABL, or
	 * Linux's 30 s ones when sweetjump launched us) from resetting the
	 * phone under a kernel that doesn't feed them (XNU). */
	str	w12, [x10]
	str	w12, [x11]
	ldr	x1, [x2]
	cbz	x1, 2b
	mrs	x3, cntpct_el0
	cmp	x3, x1
	b.lo	2b
	/* Timing log: wait 5 s per log line the boot CPU printed (feeding
	 * ABL's watchdog), so the phone's return time tells how far the
	 * shim got: lines = (t_back - t_android - deadline) / 5. */
	adr	x6, log_lines
	ldr	w6, [x6]
	cmp	w6, #40
	b.ls	3f
	mov	w6, #40
3:	movz	x7, #0x0753, lsl #16	/* 5 s * 24.576 MHz = 0x07530000 */
	mul	x7, x7, x6
	add	x7, x1, x7
	movz	x8, #0x0008
	movk	x8, #0x1006, lsl #16	/* WDT cl0 WTCNT */
	movz	w9, #0xffff
4:	str	w9, [x8]
	isb
	mrs	x3, cntpct_el0
	cmp	x3, x7
	b.lo	4b
	movz	x0, #0x0009
	movk	x0, #0x8400, lsl #16	/* PSCI SYSTEM_RESET */
	smc	#0
	b	2b

	.data
	.balign	8
diag_save:
	.skip	112
	.globl sentinel_deadline
sentinel_deadline:
	.quad	0
	.text

/* PSCI CPU_ON target. x0 = context id = cpu index (1..7). */
	.globl secondary_entry
secondary_entry:
	msr	daifset, #0xf
	msr	spsel, #1
	adr	x1, _sec_stacks
	add	x1, x1, x0, lsl #12
	add	x1, x1, #0x1000
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	bl	secondary_main
4:	wfe
	b	4b

/*
 * Exception vectors. Each slot stashes x29/x30, loads its slot number
 * and jumps to the common path, which saves x0-x28 and calls
 * exc_handler(slot, regs). IRQs return; everything else ends in the
 * on-screen panic.
 */
.macro vent slot
	.balign	0x80
	stp	x29, x30, [sp, #-16]!
	mov	x29, #\slot
	b	exc_common
.endm

	.balign	0x800
	.globl vectors
vectors:
	vent 0
	vent 1
	vent 2
	vent 3
	vent 4
	vent 5
	vent 6
	vent 7
	vent 8
	vent 9
	vent 10
	vent 11
	vent 12
	vent 13
	vent 14
	vent 15

exc_common:
	sub	sp, sp, #(30 * 8)
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	str	x28, [sp, #224]
	mov	x0, x29
	mov	x1, sp
	bl	exc_handler
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldr	x28, [sp, #224]
	add	sp, sp, #(30 * 8)
	ldp	x29, x30, [sp], #16
	eret
