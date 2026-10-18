/*
 * usblog: USB CDC-ACM "log gadget" for the stallion shim (Pixel 10a,
 * Tensor G4 / zumapro). The Mac sees a standard serial port
 * (/dev/cu.usbmodem*, AppleUSBACM, no host software); every shim log line
 * goes out on its bulk IN endpoint. Polled, no interrupts, single core,
 * MMU off, bounded waits everywhere.
 *
 * Ported from the XNU driver that enumerated on the previous phone
 * (xnu/patches/0072..0077: Qualcomm DWC3 3.20a, high speed): event buffer,
 * DEPSTARTCFG/SETEPCONFIG/SETTRANSFRESOURCE, one-TRB transfers, and the
 * ep0 control state machine modelled on Linux drivers/usb/dwc3/ep0.c.
 *
 * ===================== DESIGN NOTE =====================
 *
 * Assumed ABL hand-off state (NOT verified on stallion yet)
 *   ABL runs fastboot on this DWC3 (0x11210000, DWC_usb31/32 core inside
 *   the Exynos "exynos9-dwusb" glue) right before `fastboot boot` jumps to
 *   us, so we assume it is still: in power domain pd_hsi0 = on, link/ref/
 *   bus clocks on, eUSB2 PHY + eUSB2 repeater initialised, VBUS/session
 *   valid forced by the glue, Type-C role = device. Only DWC3 core
 *   registers are written here: stop ABL's session (DCTL.RUN_STOP=0),
 *   device soft reset (DCTL.CSFTRST - not GCTL.CORESOFTRESET, which could
 *   lose ABL's PHY interface setup), then our own event buffer, ep0 and
 *   RUN_STOP. Global registers ABL programmed (GUSB2PHYCFG, GUSB3PIPECTL,
 *   GSBUSCFG0/1, GUCTL/1, FIFO sizes) are snapshotted and put back if the
 *   soft reset changed them. The link is forced to high speed (DCFG), so
 *   the USBDP combo (SuperSpeed) PHY is never needed.
 *
 * What to probe first on the device (all of it goes to the ramoops prz
 * via log_line(), so it is readable after the reset even if USB fails)
 *   1. "about to read GSNPSID" breadcrumb, then GSNPSID/GCTL/GSTS. If the
 *      breadcrumb is the last line, the read hung: HSI0 is powered off or
 *      the link clock is gated (the sentinel core resets the phone).
 *   2. ABL's DCTL/DSTS/DCFG/GEVNTADR: was it still running (RUN_STOP=1),
 *      at which speed, where its event buffer was.
 *   3. GUSB2PHYCFG0 (PHYIF/USBTRDTIM/SUSPHY), GUSB3PIPECTL0, GSBUSCFG0.
 *   4. CSFTRST time and whether any global register changed across it.
 *   5. After RUN_STOP: DSTS (DEVCTRLHLT must clear), then the first raw
 *      events (expect: USB reset 0x..101, connect done 0x..201, then ep0
 *      XferComplete 0x..0c2 for the first SETUP).
 *
 * Failure modes, in the order they would show up
 *   F1 hang on the first DWC3 read      -> pd_hsi0 off / CMU_HSI0 gated
 *                                          (glue list below, steps 1-2)
 *   F2 GSNPSID not 0x5533/0x3331/0x3332 -> USBLOG_E_ID, block off or not
 *                                          a DWC3 at this address
 *   F3 CSFTRST never clears             -> USBLOG_E_RESET; DWC_usb31
 *                                          >=1.90a waits for the PHY: PHY
 *                                          not clocked/powered (steps 3-6)
 *   F4 DEVCTRLHLT stays set after run   -> USBLOG_E_RUN; no PHY clock
 *   F5 running, but no USB reset event  -> no pull-up seen by the host or
 *      after plugging into the Mac         no session valid: VBUS override
 *                                          (step 4), eUSB2 repeater off
 *                                          (step 6), Type-C role (step 9)
 *   F6 GEVNTCOUNT moves but events      -> DMA writes blocked/redirected:
 *      read as 0 ("event buffer reads 0")  SysMMU or S2MPU HSI0 (step 8)
 *   F7 bus error bit in an ep event,    -> same as F6 (TRB/data DMA)
 *      GSTS.BUSERRADDRVLD
 *   F8 SETUPs arrive, host gives up     -> descriptor/ep0 bug; check the
 *                                          logged setup trace and macOS
 *                                          `log stream --predicate
 *                                          'subsystem=="com.apple.usb"'`
 *   F9 enumerates, no /dev/cu.usbmodem* -> ACM descriptors rejected
 *  F10 port opens, no text              -> TX gate (DTR) / bulk IN; the
 *                                          prz log shows tx byte counts
 *
 * Exynos glue / PHY steps that may be needed if the controller turns out
 * to be off (NOT used yet; addresses from research/stallion-dts
 * zuma-usb.dtsi, zumapro-pm-domains.dtsi, zuma-s2mpu.dtsi,
 * zumapro-sysmmu.dtsi, zuma-stallion-usb.dtsi; register fields from
 * upstream Linux phy-exynos5-usbdrd.c (exynos850/gs101 layout) and the
 * google-modules phy-exynos-usbdrd/eusb drivers - unverified on zumapro):
 *   1. Power domain pd_hsi0: PMU 0x15462A80 (CONFIGURATION +0x0, STATUS
 *      +0x4, bit0 = on in the gs101 layout); DT says need_smc=0x11010204,
 *      cal_id 0xB1380018, i.e. Linux switches it through EL3 (PMU is
 *      secure: reads/writes via SMC 0x82000504 PRIV_REG, args (addr,
 *      0=read/1=write/2=rmw, val)). sub_pd_hsi0 rails: vdd075 m_ldo8,
 *      vdd12 m_ldo9, vdd33 m_ldo19 (PMIC over ACPM: out of shim reach).
 *      -DUSBLOG_PD_PROBE logs the STATUS word through that SMC (read only).
 *   2. Clocks, CMU_HSI0 @0x11000000: GATE_USB32DRD_LINK (aclk),
 *      MUX_HSI0_USB20_REF (19.2 MHz PHY ref), VDOUT_CLK_TOP_HSI0_NOC (bus).
 *      Gate offsets are in the vendor cmucal tables (not in the tree).
 *   3. PHY isolation: PMU + pmu_offset 0x3EB0 = 0x15463EB0 (bit0 = PHY
 *      enable / un-isolate); DP: 0x15463EB4. Via the PRIV_REG SMC.
 *   4. Link control / VBUS override (the Exynos counterpart of Qualcomm's
 *      QSCRATCH UTMI_OTG_VBUS_VALID/SW_SESSVLD_SEL used in patch 0074):
 *      usbdrd "phycon" @0x11100000: LINKCTRL +0x04 (BUS_FILTER_BYPASS
 *      [7:4], FORCE_QACT bit8), CLKRST +0x20 (LINK_SW_RST bit0, PORT_RST
 *      bit1), UTMI +0x50 (FORCE_SLEEP b0, FORCE_SUSPEND b1, FORCE_BVALID
 *      b4, FORCE_VBUSVALID b5), HSP +0x54 (COMMONONN b8, EN_UTMISUSPEND b9,
 *      VBUSVLDEXT b12, VBUSVLDEXTSEL b13), HSP_TEST +0x5c (SIDDQ b24).
 *      DT is_not_vbus_pad=1: the vendor driver always forces VBUS valid in
 *      software, so set UTMI b4|b5 and HSP b12|b13 if F5 shows up.
 *   5. eUSB2 PHY @0x11110000 (+0x200), and 0x11120000 (+0x200): vendor
 *      phy-exynos-eusb CAL (phy_eusb_version 0x701); 0x11130000/0x11140000
 *      (+0x800 each) are the USBDP combo PCS/PMA (SuperSpeed only).
 *   6. eUSB2 repeater "samsung,eusb-repeater" on hsi2c_9 addr 0x3E, init
 *      pin gph1-0 (input), tune regs 0x50=0x0a 0x70=0x4f 0x71=0x3c 0x72=0x92
 *      0x73=0x86 0x77=0x00 0x78=0x0b 0x79=0x60, supply vdd33 m_ldo19.
 *   7. sysreg_hsi0 @0x11020000: vendor dwc3-exynos glue settings (bus
 *      sharability/coherency, USB SUB_CTRL); offsets not in the tree.
 *   8. DMA path: sysmmu_hsi0 @0x11040000 (secure 0x11050000; DT IOVA
 *      window 0..0xF_FFFFFFFF) and s2mpu_s0_hsi0 @0x11070000. If ABL leaves
 *      either translating/protecting only its own buffers, our DMA into
 *      .bss faults (F6/F7). The Linux reserved xhci_dma* regions
 *      (0x17BB5000, 0x17BC0000, 0x97000000) are for audio offload.
 *   9. Type-C: max77759 TCPC + goog_usb_role_sw pick the data role; ABL
 *      must have left the port as UFP/device.
 *
 * DMA: all buffers live in .bss; the shim runs from physical addresses
 * (MMU off), so a buffer's address is its DMA address (TRB/event buffer
 * address fields are 64-bit lo/hi anyway). With the MMU off every CPU
 * access is Device-nGnRnE (non-cacheable) so DMA is coherent; dma_sync()
 * still does dc civac + dsb so this keeps working with caches on.
 * dma_addr() is the hook for an IOMMU/offset later.
 */
#include <stdint.h>
#include <stdarg.h>
#include "usblog.h"

void log_line(const char *line);

#define DWC3_BASE		0x11210000UL

/* ---- DWC3 global registers ---- */
#define GSBUSCFG0		0xc100
#define GSBUSCFG1		0xc104
#define GCTL			0xc110
#define  GCTL_PRTCAP_MASK	(3u << 12)
#define  GCTL_PRTCAP_DEVICE	(2u << 12)
#define GSTS			0xc118
#define  GSTS_BUSERRADDRVLD	(1u << 4)
#define GUCTL1			0xc11c
#define GSNPSID			0xc120
#define GUCTL			0xc12c
#define GBUSERRADDRLO		0xc130
#define GBUSERRADDRHI		0xc134
#define GHWPARAMS0		0xc140
#define GHWPARAMS1		0xc144
#define GHWPARAMS3		0xc14c
#define GVER_NUMBER		0xc1a0	/* DWC_usb31/32 only */
#define GUSB2PHYCFG0		0xc200
#define  USB2PHYCFG_SUSPHY	(1u << 6)
#define  USB2PHYCFG_ENBLSLPM	(1u << 8)
#define GUSB3PIPECTL0		0xc2c0
#define GTXFIFOSIZ(n)		(0xc300 + 4 * (n))
#define GRXFIFOSIZ0		0xc380
#define GEVNTADRLO		0xc400
#define GEVNTADRHI		0xc404
#define GEVNTSIZ		0xc408
#define  GEVNTSIZ_INTMASK	(1u << 31)
#define GEVNTCOUNT		0xc40c

/* ---- device registers ---- */
#define DCFG			0xc700
#define  DCFG_SPEED_MASK	7u
#define  DCFG_SPEED_HS		0u
#define  DCFG_SPEED_FS		1u
#define  DCFG_ADDR_MASK		(0x7fu << 3)
#define DCTL			0xc704
#define  DCTL_RUN_STOP		(1u << 31)
#define  DCTL_CSFTRST		(1u << 30)
#define  DCTL_ULSTCHNG_MASK	(0xfu << 5)
#define DEVTEN			0xc708
#define DSTS			0xc70c
#define  DSTS_DEVCTRLHLT	(1u << 22)
#define DALEPENA		0xc720
#define DEPCMDPAR2(n)		(0xc800 + 0x10 * (n))
#define DEPCMDPAR1(n)		(0xc804 + 0x10 * (n))
#define DEPCMDPAR0(n)		(0xc808 + 0x10 * (n))
#define DEPCMD(n)		(0xc80c + 0x10 * (n))

#define DEPCMD_SETEPCONFIG	1u
#define DEPCMD_SETTRANSFRESOURCE 2u
#define DEPCMD_SETSTALL		4u
#define DEPCMD_CLEARSTALL	5u
#define DEPCMD_STARTTRANSFER	6u
#define DEPCMD_ENDTRANSFER	8u
#define DEPCMD_DEPSTARTCFG	9u
#define DEPCMD_CMDACT		(1u << 10)
#define DEPCMD_FORCERM		(1u << 11)
#define DEPCMD_PARAM(x)		((uint32_t)(x) << 16)

/* DEPCFG fields (Linux dwc3 gadget.h) */
#define DEPCFG_TYPE(t)		((uint32_t)(t) << 1)
#define DEPCFG_MPS(m)		((uint32_t)(m) << 3)
#define DEPCFG_FIFO(f)		((uint32_t)(f) << 17)
#define DEPCFG_ACTION_MODIFY	(2u << 30)
#define DEPCFG_XFERCOMPLETE	(1u << 8)
#define DEPCFG_XFERNOTREADY	(1u << 10)
#define DEPCFG_BINTERVAL_M1(x)	((uint32_t)(x) << 16)
#define DEPCFG_EPNUM(n)		((uint32_t)(n) << 25)

/* TRB control word */
#define TRB_HWO			(1u << 0)
#define TRB_LST			(1u << 1)
#define TRB_ISP_IMI		(1u << 10)
#define TRB_IOC			(1u << 11)
#define TRBCTL_NORMAL		(1u << 4)
#define TRBCTL_SETUP		(2u << 4)
#define TRBCTL_STATUS2		(3u << 4)
#define TRBCTL_STATUS3		(4u << 4)
#define TRBCTL_DATA		(5u << 4)

/* Physical endpoints: 2n = OUT, 2n+1 = IN */
#define EP0_OUT			0
#define EP0_IN			1
#define EP_BOUT			2	/* 0x01 bulk OUT (host -> phone, discarded) */
#define EP_BIN			3	/* 0x81 bulk IN  (log text) */
#define EP_INTR			5	/* 0x82 interrupt IN (ACM notifications, idle) */
#define NEPS			6

#define EVT_SIZE		4096
#define TX_CHUNK		8192
#define RING_SIZE		65536	/* power of two */
#define TX_DTR_GRACE_MS		3000	/* send even without DTR after this */

/* ---- DMA memory (.bss, physical == DMA address) ---- */
static volatile uint32_t evtbuf[EVT_SIZE / 4] __attribute__((aligned(4096)));
static volatile uint32_t trbs[NEPS][4] __attribute__((aligned(64)));
static volatile uint8_t setup_buf[8] __attribute__((aligned(64)));
static volatile uint8_t ep0_buf[512] __attribute__((aligned(64)));
static volatile uint8_t rx_buf[512] __attribute__((aligned(64)));
static volatile uint8_t tx_buf[TX_CHUNK] __attribute__((aligned(64)));

static char ring[RING_SIZE];

enum { S_SETUP, S_DATA, S_WAIT_STATUS2, S_WAIT_STATUS3, S_STATUS };

static struct {
	int ready, in_poll, err;
	int configured, fs, dtr, stalled;
	int ep0_state, ep0_in;
	uint8_t ep0_bm, ep0_req;
	uint8_t active[NEPS], rsc[NEPS];
	uint32_t evt_pos, nevents, zero_events, nsetup;
	uint32_t wr, rd, dropped, tx_len;
	uint32_t tx_bytes, tx_xfers, rx_bytes;
	uint64_t cfg_time, tfreq;
	uint8_t line_coding[7];
} u;

/* ---- low level ---- */

static inline uint32_t rd32(uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(DWC3_BASE + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static inline void wr32(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(DWC3_BASE + off) = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

static inline uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v) :: "memory");
	return v;
}

static uint64_t us_ticks(uint32_t us)
{
	return u.tfreq / 1000000 * us + (u.tfreq % 1000000) * us / 1000000;
}

/* Iteration cap in case CNTPCT does not tick: each loop is >= ~10 ns. */
#define SPIN_CAP(us)		((uint64_t)(us) * 200 + 1000)

static void udelay(uint32_t us)
{
	uint64_t end = ticks() + us_ticks(us);
	for (uint64_t i = 0; i < SPIN_CAP(us) && ticks() < end; i++)
		;
}

/* Wait until (reg & mask) == want; 0 on success, -1 after us. */
static int wait_reg(uint32_t off, uint32_t mask, uint32_t want, uint32_t us, uint32_t *took_us)
{
	uint64_t t0 = ticks(), end = t0 + us_ticks(us);
	int rc = -1;
	for (uint64_t i = 0; i < SPIN_CAP(us); i++) {
		if ((rd32(off) & mask) == want) {
			rc = 0;
			break;
		}
		if (ticks() >= end)
			break;
	}
	if (took_us && u.tfreq)
		*took_us = (uint32_t)((ticks() - t0) * 1000000 / u.tfreq);
	return rc;
}

static uint32_t ms_since(uint64_t t)
{
	return u.tfreq ? (uint32_t)((ticks() - t) * 1000 / u.tfreq) : 0;
}

static inline uint64_t dma_addr(const volatile void *p)
{
	return (uint64_t)(uintptr_t)p;	/* MMU off, no IOMMU: identity */
}

/* Clean+invalidate a DMA range. A no-op in effect while the MMU is off
 * (Device memory is never cached), required once caches are on. */
static void dma_sync(const volatile void *p, uint32_t n)
{
#ifndef USBLOG_NO_CACHE_MAINT
	uint64_t ctr;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	uintptr_t line = 4UL << ((ctr >> 16) & 0xf);
	uintptr_t a = (uintptr_t)p & ~(line - 1), end = (uintptr_t)p + n;
	__asm__ volatile("dsb sy" ::: "memory");
	for (; a < end; a += line)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
#else
	(void)p;
	(void)n;
#endif
	__asm__ volatile("dsb sy" ::: "memory");
}

/* ---- tiny log formatter: %x (8 hex digits), %u, %s ---- */

static void ulog(const char *fmt, ...)
{
	char b[200];
	unsigned n = 0;
	const char *pre = "usblog: ";
	va_list ap;

	while (*pre)
		b[n++] = *pre++;
	va_start(ap, fmt);
	for (; *fmt && n < sizeof(b) - 24; fmt++) {
		if (*fmt != '%') {
			b[n++] = *fmt;
			continue;
		}
		fmt++;
		if (*fmt == 'x') {
			uint32_t v = va_arg(ap, uint32_t);
			for (int s = 28; s >= 0; s -= 4)
				b[n++] = "0123456789abcdef"[(v >> s) & 0xf];
		} else if (*fmt == 'u') {
			uint32_t v = va_arg(ap, uint32_t);
			char d[10];
			int k = 0;
			do {
				d[k++] = (char)('0' + v % 10);
				v /= 10;
			} while (v && k < 10);
			while (k)
				b[n++] = d[--k];
		} else if (*fmt == 's') {
			const char *s = va_arg(ap, const char *);
			while (s && *s && n < sizeof(b) - 24)
				b[n++] = *s++;
		} else if (*fmt == 0) {
			break;
		} else {
			b[n++] = *fmt;
		}
	}
	va_end(ap);
	b[n] = 0;
	log_line(b);
}

/* ---- endpoint commands and transfers ---- */

static int ep_cmd(unsigned ep, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2)
{
	/* Databook 6.3.2.5: no endpoint command with the USB2 PHY suspended
	 * (Linux dwc3_send_gadget_ep_cmd clears SUSPHY/ENBLSLPM around it). */
	uint32_t phy = rd32(GUSB2PHYCFG0);
	int restore = (phy & (USB2PHYCFG_SUSPHY | USB2PHYCFG_ENBLSLPM)) != 0;
	if (restore)
		wr32(GUSB2PHYCFG0, phy & ~(USB2PHYCFG_SUSPHY | USB2PHYCFG_ENBLSLPM));

	wr32(DEPCMDPAR0(ep), p0);
	wr32(DEPCMDPAR1(ep), p1);
	wr32(DEPCMDPAR2(ep), p2);
	wr32(DEPCMD(ep), cmd | DEPCMD_CMDACT);
	int rc = wait_reg(DEPCMD(ep), DEPCMD_CMDACT, 0, 10000, 0);
	uint32_t v = rd32(DEPCMD(ep));
	if (rc) {
		ulog("ep%u cmd %u timeout DEPCMD=%x", ep, cmd & 0xf, v);
	} else if ((v >> 12) & 0xf) {
		ulog("ep%u cmd %u status %u", ep, cmd & 0xf, (v >> 12) & 0xf);
		rc = -1;
	}
	if (restore)
		wr32(GUSB2PHYCFG0, phy);
	return rc;
}

static int start_trb(unsigned ep, const volatile void *buf, uint32_t len, uint32_t ctl)
{
	volatile uint32_t *t = trbs[ep];
	uint64_t a = dma_addr(buf);

	if (u.active[ep]) {
		ulog("ep%u start while active", ep);
		return -1;
	}
	t[0] = (uint32_t)a;
	t[1] = (uint32_t)(a >> 32);
	t[2] = len & 0xffffff;
	t[3] = ctl | TRB_HWO | TRB_LST | TRB_ISP_IMI | TRB_IOC;
	dma_sync(buf, len ? len : 1);
	dma_sync(t, 16);
	uint64_t ta = dma_addr(t);
	if (ep_cmd(ep, DEPCMD_STARTTRANSFER, (uint32_t)(ta >> 32), (uint32_t)ta, 0))
		return -1;
	u.active[ep] = 1;
	u.rsc[ep] = (uint8_t)((rd32(DEPCMD(ep)) >> 16) & 0x7f);
	return 0;
}

static void end_transfer(unsigned ep)
{
	if (!u.active[ep])
		return;
	/* No CMDIOC: poll CMDACT, then the 100 us Linux waits in that case. */
	ep_cmd(ep, DEPCMD_ENDTRANSFER | DEPCMD_FORCERM | DEPCMD_PARAM(u.rsc[ep]), 0, 0, 0);
	udelay(100);
	u.active[ep] = 0;
	trbs[ep][3] &= ~TRB_HWO;
}

/* ---- descriptors ---- */

#define USB_VID			0x1209	/* pid.codes test VID, like sweet */
#define USB_PID			0x0001

static const uint8_t dev_desc[18] = {
	18, 1, 0x00, 0x02,		/* USB 2.00 */
	0xef, 0x02, 0x01,		/* misc / common / IAD */
	64,				/* ep0 max packet */
	USB_VID & 0xff, USB_VID >> 8, USB_PID & 0xff, USB_PID >> 8,
	0x00, 0x02,			/* bcdDevice 2.00 (sweet ECM was 1.00) */
	1, 2, 3, 1,
};

static const uint8_t qual_desc[10] = {
	10, 6, 0x00, 0x02, 0xef, 0x02, 0x01, 64, 1, 0,
};

#define CFG_LEN			75
#define CFG_INT_BINTERVAL	51
#define CFG_BIN_MPS		65
#define CFG_BOUT_MPS		72
static const uint8_t cfg_tmpl[CFG_LEN] = {
	9, 2, CFG_LEN, 0, 2, 1, 0, 0x80, 250,	/* config: 2 interfaces, 500 mA */
	8, 0x0b, 0, 2, 0x02, 0x02, 0x01, 0,	/* IAD: interfaces 0-1, CDC ACM */
	9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,	/* if 0: comm, ACM, AT cmds, 1 ep */
	5, 0x24, 0x00, 0x10, 0x01,		/* header, CDC 1.10 */
	5, 0x24, 0x01, 0x00, 1,			/* call management: data if 1 */
	4, 0x24, 0x02, 0x02,			/* ACM: line coding + control line state */
	5, 0x24, 0x06, 0, 1,			/* union: control 0, data 1 */
	7, 5, 0x82, 0x03, 16, 0, 9,		/* ep 0x82 interrupt IN, 16 B, bInterval */
	9, 4, 1, 0, 2, 0x0a, 0x00, 0x00, 0,	/* if 1: CDC data, 2 eps */
	7, 5, 0x81, 0x02, 0x00, 0x02, 0,	/* ep 0x81 bulk IN, 512 */
	7, 5, 0x01, 0x02, 0x00, 0x02, 0,	/* ep 0x01 bulk OUT, 512 */
};

static uint32_t build_config(volatile uint8_t *d, int fs, uint8_t type)
{
	for (unsigned i = 0; i < CFG_LEN; i++)
		d[i] = cfg_tmpl[i];
	d[1] = type;
	if (fs) {
		d[CFG_INT_BINTERVAL] = 32;	/* ms */
		d[CFG_BIN_MPS] = 64;
		d[CFG_BIN_MPS + 1] = 0;
		d[CFG_BOUT_MPS] = 64;
		d[CFG_BOUT_MPS + 1] = 0;
	}
	return CFG_LEN;
}

/* Strings 1..3 back to back: a pointer table would need absolute
 * relocations, which the position-independent shim cannot have. */
static const char strtab[] = "ios-sweet\0stallion shim log\0STALLION\0";

static const char *string_for(unsigned idx)
{
	const char *s = strtab;
	if (idx < 1 || idx > 3)
		return 0;
	while (--idx)
		while (*s++)
			;
	return s;
}

/* None of these lengths is a multiple of 64, so no ep0 ZLP is ever needed. */
static uint32_t build_string(volatile uint8_t *d, unsigned idx)
{
	if (idx == 0) {
		d[0] = 4; d[1] = 3; d[2] = 0x09; d[3] = 0x04;	/* en-US */
		return 4;
	}
	const char *s = string_for(idx);
	if (!s)
		return 0;
	uint32_t n = 2;
	for (; *s && n < 62; s++) {
		d[n++] = (uint8_t)*s;
		d[n++] = 0;
	}
	d[0] = (uint8_t)n;
	d[1] = 3;
	return n;
}

/* ---- ep0 ---- */

static void ep0_start_setup(void)
{
	u.ep0_state = S_SETUP;
	if (start_trb(EP0_OUT, setup_buf, 8, TRBCTL_SETUP))
		ulog("cannot arm SETUP");
}

static void ep0_stall_restart(void)
{
	end_transfer(EP0_IN);
	end_transfer(EP0_OUT);
	ep_cmd(EP0_OUT, DEPCMD_SETSTALL, 0, 0, 0);
	ep0_start_setup();
}

static int ep_addr_to_phys(uint16_t addr)
{
	return ((addr & 0x0f) << 1) | ((addr & 0x80) ? 1 : 0);
}

static void deconfigure(void)
{
	end_transfer(EP_BOUT);
	end_transfer(EP_BIN);
	end_transfer(EP_INTR);
	wr32(DALEPENA, rd32(DALEPENA) & ~((1u << EP_BOUT) | (1u << EP_BIN) | (1u << EP_INTR)));
	u.configured = 0;
	u.dtr = 0;
	u.tx_len = 0;	/* an aborted chunk is lost (<= TX_CHUNK bytes) */
}

static void rx_arm(void)
{
	start_trb(EP_BOUT, rx_buf, sizeof(rx_buf), TRBCTL_NORMAL);
}

static int configure(void)
{
	uint32_t mps = u.fs ? 64 : 512;
	uint32_t bint = u.fs ? 0 : 8;	/* DEPCFG bInterval-1 (Linux uses 0 for FS interrupt) */

	if (u.configured)
		deconfigure();
	if (ep_cmd(EP0_OUT, DEPCMD_DEPSTARTCFG | DEPCMD_PARAM(2), 0, 0, 0) ||
	    ep_cmd(EP_BOUT, DEPCMD_SETEPCONFIG, DEPCFG_TYPE(2) | DEPCFG_MPS(mps),
		   DEPCFG_XFERCOMPLETE | DEPCFG_XFERNOTREADY | DEPCFG_EPNUM(EP_BOUT), 0) ||
	    ep_cmd(EP_BOUT, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    ep_cmd(EP_BIN, DEPCMD_SETEPCONFIG, DEPCFG_TYPE(2) | DEPCFG_MPS(mps) | DEPCFG_FIFO(EP_BIN >> 1),
		   DEPCFG_XFERCOMPLETE | DEPCFG_XFERNOTREADY | DEPCFG_EPNUM(EP_BIN), 0) ||
	    ep_cmd(EP_BIN, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    ep_cmd(EP_INTR, DEPCMD_SETEPCONFIG, DEPCFG_TYPE(3) | DEPCFG_MPS(16) | DEPCFG_FIFO(EP_INTR >> 1),
		   DEPCFG_XFERCOMPLETE | DEPCFG_XFERNOTREADY | DEPCFG_BINTERVAL_M1(bint) | DEPCFG_EPNUM(EP_INTR), 0) ||
	    ep_cmd(EP_INTR, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) {
		ulog("data endpoint setup failed");
		return -1;
	}
	wr32(DALEPENA, rd32(DALEPENA) | (1u << EP_BOUT) | (1u << EP_BIN) | (1u << EP_INTR));
	u.configured = 1;
	u.cfg_time = ticks();
	rx_arm();
	ulog("configured (%s), DALEPENA=%x", u.fs ? "full speed" : "high speed", rd32(DALEPENA));
	return 0;
}

static void ep_clear_halt(unsigned ep)
{
	int was = u.active[ep];
	end_transfer(ep);
	ep_cmd(ep, DEPCMD_CLEARSTALL, 0, 0, 0);
	if (!u.configured)
		return;
	if (ep == EP_BOUT)
		rx_arm();
	else if (ep == EP_BIN && was && u.tx_len)
		start_trb(EP_BIN, tx_buf, u.tx_len, TRBCTL_NORMAL);	/* resend */
}

static int std_request(uint8_t bm, uint8_t req, uint16_t val, uint16_t idx, uint32_t *n)
{
	volatile uint8_t *d = ep0_buf;
	unsigned recip = bm & 0x1f;

	switch (req) {
	case 0:		/* GET_STATUS */
		d[0] = 0;
		d[1] = 0;
		*n = 2;
		return 1;
	case 1:		/* CLEAR_FEATURE */
	case 3:		/* SET_FEATURE */
		if (recip == 2 && val == 0) {	/* ENDPOINT_HALT */
			int ep = ep_addr_to_phys(idx);
			if (ep >= 2 && ep < NEPS && u.configured) {
				if (req == 1)
					ep_clear_halt((unsigned)ep);
				else
					ep_cmd((unsigned)ep, DEPCMD_SETSTALL, 0, 0, 0);
			}
		}
		return 1;	/* device remote wakeup / test mode: ack, ignore */
	case 5:		/* SET_ADDRESS (DWC3 applies it after the status stage) */
		wr32(DCFG, (rd32(DCFG) & ~DCFG_ADDR_MASK) | ((uint32_t)(val & 0x7f) << 3));
		ulog("address %u", val & 0x7f);
		return 1;
	case 6:		/* GET_DESCRIPTOR */
		switch (val >> 8) {
		case 1:
			for (unsigned i = 0; i < sizeof(dev_desc); i++)
				d[i] = dev_desc[i];
			*n = sizeof(dev_desc);
			return 1;
		case 2:
			*n = build_config(d, u.fs, 2);
			return 1;
		case 3:
			*n = build_string(d, val & 0xff);
			return *n != 0;
		case 6:		/* DEVICE_QUALIFIER */
			for (unsigned i = 0; i < sizeof(qual_desc); i++)
				d[i] = qual_desc[i];
			*n = sizeof(qual_desc);
			return 1;
		case 7:		/* OTHER_SPEED_CONFIGURATION */
			*n = build_config(d, !u.fs, 7);
			return 1;
		default:	/* BOS etc.: stall (USB 2.00 device) */
			return 0;
		}
	case 8:		/* GET_CONFIGURATION */
		d[0] = u.configured ? 1 : 0;
		*n = 1;
		return 1;
	case 9:		/* SET_CONFIGURATION */
		if (val == 0) {
			deconfigure();
			return 1;
		}
		return val == 1 && configure() == 0;
	case 10:	/* GET_INTERFACE */
		d[0] = 0;
		*n = 1;
		return idx <= 1;
	case 11:	/* SET_INTERFACE: only alt 0 exists */
		return val == 0 && idx <= 1;
	default:
		return 0;
	}
}

static int class_request(uint8_t bm, uint8_t req, uint16_t val, uint32_t *n)
{
	switch (req) {
	case 0x20:	/* SET_LINE_CODING: 7 bytes in the data stage */
		return bm == 0x21;
	case 0x21:	/* GET_LINE_CODING */
		for (unsigned i = 0; i < 7; i++)
			ep0_buf[i] = u.line_coding[i];
		*n = 7;
		return bm == 0xa1;
	case 0x22:	/* SET_CONTROL_LINE_STATE: bit0 DTR, bit1 RTS */
		if (u.dtr != (val & 1))
			ulog("DTR %u (host %s the port)", val & 1, (val & 1) ? "opened" : "closed");
		u.dtr = val & 1;
		return 1;
	case 0x23:	/* SEND_BREAK */
		return 1;
	default:
		return 0;
	}
}

static void ep0_setup(void)
{
	dma_sync(setup_buf, 8);
	uint8_t bm = setup_buf[0], req = setup_buf[1];
	uint16_t val = (uint16_t)(setup_buf[2] | setup_buf[3] << 8);
	uint16_t idx = (uint16_t)(setup_buf[4] | setup_buf[5] << 8);
	uint16_t len = (uint16_t)(setup_buf[6] | setup_buf[7] << 8);
	uint32_t n = 0;
	int ok;

	if (u.nsetup++ < 64)
		ulog("setup %x len %u", (uint32_t)bm | (uint32_t)req << 8 | (uint32_t)val << 16, len);
	u.ep0_in = (bm & 0x80) != 0;
	u.ep0_bm = bm;
	u.ep0_req = req;
	switch (bm & 0x60) {
	case 0x00:
		ok = std_request(bm, req, val, idx, &n);
		break;
	case 0x20:
		ok = class_request(bm, req, val, &n);
		break;
	default:
		ok = 0;
		break;
	}
	/* OUT data is only accepted for SET_LINE_CODING (7 bytes). */
	if (ok && !u.ep0_in && len && !(bm == 0x21 && req == 0x20 && len <= 64))
		ok = 0;
	if (!ok) {
		if (u.nsetup <= 64)
			ulog("stall");
		ep0_stall_restart();
		return;
	}
	if (len == 0) {
		u.ep0_state = S_WAIT_STATUS2;	/* status stage on XferNotReady */
		return;
	}
	u.ep0_state = S_DATA;
	if (u.ep0_in) {
		if (n > len)
			n = len;
		if (start_trb(EP0_IN, ep0_buf, n, TRBCTL_DATA))
			ep0_stall_restart();
	} else if (start_trb(EP0_OUT, ep0_buf, 64, TRBCTL_DATA)) {
		ep0_stall_restart();
	}
}

static void ep0_out_data_done(void)
{
	dma_sync(ep0_buf, 64);
	if (u.ep0_bm == 0x21 && u.ep0_req == 0x20) {
		for (unsigned i = 0; i < 7; i++)
			u.line_coding[i] = ep0_buf[i];
	}
}

static void ep0_event(unsigned ep, unsigned type, unsigned st)
{
	if (type == 1) {		/* XferComplete */
		u.active[ep] = 0;
		if (st & 1)
			ulog("ep%u bus error (DMA blocked?) GSTS=%x", ep, rd32(GSTS));
		switch (u.ep0_state) {
		case S_SETUP:
			ep0_setup();
			break;
		case S_DATA:
			if (!u.ep0_in)
				ep0_out_data_done();
			u.ep0_state = S_WAIT_STATUS3;
			break;
		case S_STATUS:
			ep0_start_setup();
			break;
		default:
			break;
		}
	} else if (type == 3 && (st & 3) == 2) {	/* XferNotReady, control status */
		if (u.ep0_state == S_WAIT_STATUS2 || u.ep0_state == S_WAIT_STATUS3) {
			uint32_t ctl = u.ep0_state == S_WAIT_STATUS3 ? TRBCTL_STATUS3 : TRBCTL_STATUS2;
			u.ep0_state = S_STATUS;
			if (start_trb(ep, ep0_buf, 0, ctl))
				ep0_stall_restart();
		}
	}
}

/* ---- events ---- */

static void ep_event(uint32_t ev)
{
	unsigned ep = (ev >> 1) & 0x1f;
	unsigned type = (ev >> 6) & 0xf;
	unsigned st = (ev >> 12) & 0xf;

	if (ep <= 1) {
		ep0_event(ep, type, st);
		return;
	}
	if (type != 1 || ep >= NEPS)
		return;
	u.active[ep] = 0;
	if (st & 1)
		ulog("ep%u bus error (DMA blocked?) GSTS=%x", ep, rd32(GSTS));
	if (ep == EP_BOUT) {
		dma_sync(trbs[EP_BOUT], 16);
		u.rx_bytes += sizeof(rx_buf) - (trbs[EP_BOUT][2] & 0xffffff);
		if (u.configured)
			rx_arm();
	} else if (ep == EP_BIN) {
		u.tx_bytes += u.tx_len;
		u.tx_xfers++;
		u.tx_len = 0;
		u.stalled = 0;
	}
}

static void dev_event(uint32_t ev)
{
	unsigned type = (ev >> 8) & 0xf;

	switch (type) {
	case 0:		/* disconnect */
		ulog("disconnect (tx %u bytes in %u transfers)", u.tx_bytes, u.tx_xfers);
		deconfigure();
		if (u.ep0_state != S_SETUP)
			ep0_stall_restart();
		break;
	case 1:		/* USB reset */
		if (u.ep0_state != S_SETUP)
			ep0_stall_restart();
		deconfigure();
		wr32(DCFG, rd32(DCFG) & ~DCFG_ADDR_MASK);
		ulog("bus reset");
		break;
	case 2: {	/* connect done */
		uint32_t spd = rd32(DSTS) & 7;
		u.fs = spd == 1 || spd == 3;
		/* ep0 max packet 64 at high/full speed (modify action) */
		ep_cmd(EP0_OUT, DEPCMD_SETEPCONFIG, DEPCFG_TYPE(0) | DEPCFG_MPS(64) | DEPCFG_ACTION_MODIFY,
		       DEPCFG_XFERCOMPLETE | DEPCFG_XFERNOTREADY | DEPCFG_EPNUM(EP0_OUT), 0);
		ep_cmd(EP0_IN, DEPCMD_SETEPCONFIG, DEPCFG_TYPE(0) | DEPCFG_MPS(64) | DEPCFG_ACTION_MODIFY,
		       DEPCFG_XFERCOMPLETE | DEPCFG_XFERNOTREADY | DEPCFG_EPNUM(EP0_IN), 0);
		ulog("connect done, speed %u (%s)", spd,
		     spd == 0 ? "high" : u.fs ? "full" : "unexpected");
		break;
	}
	case 11:
		ulog("event buffer overflow");
		break;
	default:
		break;
	}
}

static void handle_event(uint32_t ev)
{
	if (ev == 0) {
		if (u.zero_events++ == 0)
			ulog("event buffer reads 0: controller DMA writes not landing (SysMMU/S2MPU?)");
		return;
	}
	if (u.nevents++ < 48)
		ulog("event %x", ev);
	if (!(ev & 1))
		ep_event(ev);
	else if (((ev >> 1) & 0x7f) == 0)
		dev_event(ev);
}

/* ---- TX ---- */

static int tx_gate_open(void)
{
	/* Wait for the host to open the tty (DTR) so the start of the log is
	 * not swallowed by a driver that is not reading yet; send anyway
	 * after a grace period. */
	return u.configured && (u.dtr || ms_since(u.cfg_time) >= TX_DTR_GRACE_MS);
}

static void tx_kick(void)
{
	if (!tx_gate_open() || u.active[EP_BIN] || u.tx_len)
		return;
	if (u.dropped) {
		uint32_t d = u.dropped;
		u.dropped = 0;
		ulog("(%u bytes dropped while the ring was full)", d);
	}
	uint32_t n = u.wr - u.rd;
	if (n == 0)
		return;
	if (n > TX_CHUNK)
		n = TX_CHUNK;
	uint32_t mps = u.fs ? 64 : 512;
	if (n % mps == 0)
		n--;		/* a short last packet ends the transfer: no ZLP needed */
	for (uint32_t i = 0; i < n; i++)
		tx_buf[i] = (uint8_t)ring[(u.rd + i) & (RING_SIZE - 1)];
	u.rd += n;
	u.tx_len = n;
	if (start_trb(EP_BIN, tx_buf, n, TRBCTL_NORMAL))
		u.tx_len = 0;	/* lost; the ep_cmd failure is logged */
}

/* ---- API ---- */

void wdt_kick(void);

void usblog_poll(void)
{
	wdt_kick();
	if (!u.ready || u.in_poll)
		return;
	u.in_poll = 1;
	uint32_t cnt = rd32(GEVNTCOUNT) & 0xffff;
	if (cnt) {
		uint32_t todo = cnt > EVT_SIZE ? EVT_SIZE : cnt;
		dma_sync(evtbuf, EVT_SIZE);
		for (uint32_t done = 0; done < todo; done += 4) {
			uint32_t ev = evtbuf[u.evt_pos / 4];
			evtbuf[u.evt_pos / 4] = 0;	/* so a stale slot reads 0 */
			u.evt_pos = (u.evt_pos + 4) % EVT_SIZE;
			handle_event(ev);
		}
		dma_sync(evtbuf, EVT_SIZE);
		wr32(GEVNTCOUNT, cnt);
	}
	tx_kick();
	u.in_poll = 0;
}

void usblog_write(const char *s, unsigned n)
{
	if (!s)
		return;
	for (unsigned i = 0; i < n; i++) {
		if (u.wr - u.rd >= RING_SIZE) {
			/* full: give a reading host up to 20 ms, once per stall */
			if (u.ready && !u.in_poll && !u.stalled && tx_gate_open()) {
				uint64_t end = ticks() + us_ticks(20000);
				for (uint32_t k = 0; k < 200000 && u.wr - u.rd >= RING_SIZE &&
				     ticks() < end; k++)
					usblog_poll();
				if (u.wr - u.rd >= RING_SIZE)
					u.stalled = 1;
			}
			if (u.wr - u.rd >= RING_SIZE) {
				u.rd++;
				u.dropped++;
			}
		}
		ring[u.wr & (RING_SIZE - 1)] = s[i];
		u.wr++;
	}
	if (u.ready && !u.in_poll)
		usblog_poll();
}

int usblog_connected(void)
{
	return u.configured;
}

void usblog_flush(unsigned ms)
{
	if (!u.ready)
		return;
	uint64_t end = ticks() + us_ticks(ms > 60000 ? 60000000 : ms * 1000);
	for (uint64_t i = 0; i < SPIN_CAP(ms * 1000) && ticks() < end; i++) {
		usblog_poll();
		if (!u.configured || (u.wr == u.rd && !u.active[EP_BIN] && !u.tx_len))
			break;
	}
}

void usblog_stop(void)
{
	if (!u.ready || u.in_poll)
		return;
	ulog("stopping (tx %u bytes, %u dropped)", u.tx_bytes, u.dropped);
	usblog_flush(200);
	u.in_poll = 1;
	deconfigure();
	end_transfer(EP0_IN);
	end_transfer(EP0_OUT);
	wr32(DEVTEN, 0);
	wr32(DCTL, rd32(DCTL) & ~(DCTL_RUN_STOP | DCTL_ULSTCHNG_MASK));
	/* The core only halts once its pending events were acknowledged. */
	uint64_t end = ticks() + us_ticks(100000);
	for (uint32_t i = 0; i < 1000000 && ticks() < end; i++) {
		uint32_t cnt = rd32(GEVNTCOUNT) & 0xffff;
		if (cnt)
			wr32(GEVNTCOUNT, cnt);
		if (rd32(DSTS) & DSTS_DEVCTRLHLT)
			break;
	}
	u.ready = 0;
	u.in_poll = 0;
	u.configured = 0;
	ulog("stopped, DSTS=%x", rd32(DSTS));
}

static int id_ok(uint32_t id)
{
	uint32_t hi = id >> 16;
	return hi == 0x5533 || hi == 0x3331 || hi == 0x3332;
}

int usblog_init(void)
{
	static const uint32_t keep[] = {
		GUSB2PHYCFG0, GUSB3PIPECTL0, GSBUSCFG0, GSBUSCFG1, GUCTL, GUCTL1,
		GTXFIFOSIZ(0), GTXFIFOSIZ(1), GTXFIFOSIZ(2), GTXFIFOSIZ(3), GRXFIFOSIZ0,
	};
	uint32_t saved[sizeof(keep) / sizeof(keep[0])];
	uint32_t took = 0;

	if (u.ready)
		return 0;
	uint64_t f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	u.tfreq = f ? f : 24576000;
	u.line_coding[0] = 0x00;	/* 115200 8N1 */
	u.line_coding[1] = 0xc2;
	u.line_coding[2] = 0x01;
	u.line_coding[6] = 8;

#ifdef USBLOG_PD_PROBE
	{
		/* pd_hsi0 STATUS through the EL3 PMU read service (unverified). */
		register uint64_t x0 __asm__("x0") = 0x82000504;
		register uint64_t x1 __asm__("x1") = 0x15462A84;
		register uint64_t x2 __asm__("x2") = 0;
		register uint64_t x3 __asm__("x3") = 0;
		__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :
				 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
				   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		ulog("pd_hsi0 STATUS via smc: %x", (uint32_t)x0);
	}
#endif
	ulog("init: about to read GSNPSID @%x (a hang here = HSI0 off/unclocked)",
	     (uint32_t)(DWC3_BASE + GSNPSID));
	uint32_t id = rd32(GSNPSID);
	ulog("GSNPSID=%x GCTL=%x GSTS=%x", id, rd32(GCTL), rd32(GSTS));
	if (!id_ok(id)) {
		ulog("not a DWC3: giving up");
		return u.err = USBLOG_E_ID;
	}
	if ((id >> 16) != 0x5533)
		ulog("DWC_usb3%u VER_NUMBER=%x", (id >> 16) & 0xf, rd32(GVER_NUMBER));
	ulog("HWPARAMS0=%x 1=%x 3=%x GSBUSCFG0=%x", rd32(GHWPARAMS0), rd32(GHWPARAMS1),
	     rd32(GHWPARAMS3), rd32(GSBUSCFG0));
	ulog("GUSB2PHYCFG0=%x GUSB3PIPECTL0=%x GUCTL=%x", rd32(GUSB2PHYCFG0),
	     rd32(GUSB3PIPECTL0), rd32(GUCTL));
	ulog("ABL: DCFG=%x DCTL=%x DSTS=%x DEVTEN=%x DALEPENA=%x", rd32(DCFG), rd32(DCTL),
	     rd32(DSTS), rd32(DEVTEN), rd32(DALEPENA));
	ulog("ABL: GEVNTADR=%x%x SIZ=%x COUNT=%x", rd32(GEVNTADRHI), rd32(GEVNTADRLO),
	     rd32(GEVNTSIZ), rd32(GEVNTCOUNT));

	/* 1. Stop ABL's session so the host sees fastboot detach cleanly. */
	uint32_t dctl = rd32(DCTL);
	if (dctl & DCTL_RUN_STOP) {
		wr32(DEVTEN, 0);
		wr32(DCTL, dctl & ~(DCTL_RUN_STOP | DCTL_ULSTCHNG_MASK));
		int rc = wait_reg(DSTS, DSTS_DEVCTRLHLT, DSTS_DEVCTRLHLT, 500000, &took);
		ulog("run/stop cleared: %s after %u us, DSTS=%x", rc ? "NOT halted" : "halted",
		     took, rd32(DSTS));
	}

	/* 2. Device role (core register only; ABL should already have it). */
	uint32_t gctl = rd32(GCTL);
	if ((gctl & GCTL_PRTCAP_MASK) != GCTL_PRTCAP_DEVICE) {
		wr32(GCTL, (gctl & ~GCTL_PRTCAP_MASK) | GCTL_PRTCAP_DEVICE);
		ulog("GCTL port capability was %u, set device: GCTL=%x",
		     (gctl >> 12) & 3, rd32(GCTL));
	}

	/* 3. Device-core soft reset; keep ABL's global setup. */
	for (unsigned i = 0; i < sizeof(keep) / sizeof(keep[0]); i++)
		saved[i] = rd32(keep[i]);
	wr32(DCTL, DCTL_CSFTRST);
	if (wait_reg(DCTL, DCTL_CSFTRST, 0, 1000000, &took)) {
		ulog("CSFTRST stuck after %u us: DCTL=%x DSTS=%x", took, rd32(DCTL), rd32(DSTS));
		return u.err = USBLOG_E_RESET;
	}
	udelay(50000);	/* DWC_usb31 <= 1.80a: 50 ms before touching the PHY side */
	ulog("soft reset done in %u us", took);
	for (unsigned i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
		uint32_t now_v = rd32(keep[i]);
		if (now_v != saved[i]) {
			ulog("reg %x changed by reset %x -> %x: restored", keep[i] + (uint32_t)DWC3_BASE,
			     saved[i], now_v);
			wr32(keep[i], saved[i]);
		}
	}
	/* Even if ABL never ran, give the host >=100 ms of disconnect. */
	udelay(100000);

	/* 4. Our event buffer (interrupt masked: polled). */
	uint64_t ea = dma_addr(evtbuf);
	for (unsigned i = 0; i < EVT_SIZE / 4; i++)
		evtbuf[i] = 0;
	dma_sync(evtbuf, EVT_SIZE);
	wr32(GEVNTADRLO, (uint32_t)ea);
	wr32(GEVNTADRHI, (uint32_t)(ea >> 32));
	wr32(GEVNTSIZ, GEVNTSIZ_INTMASK | EVT_SIZE);
	wr32(GEVNTCOUNT, rd32(GEVNTCOUNT) & 0xffff);
	u.evt_pos = 0;

	/* 5. High speed (or full speed for testing), address 0. */
#ifdef USBLOG_FORCE_FS
	uint32_t spd = DCFG_SPEED_FS;
#else
	uint32_t spd = DCFG_SPEED_HS;
#endif
	wr32(DCFG, (rd32(DCFG) & ~(DCFG_SPEED_MASK | DCFG_ADDR_MASK)) | spd);
	/* disconnect, USB reset, connect done */
	wr32(DEVTEN, 0x7);

	/* 6. ep0 OUT/IN: control, max packet 512 until connect done. */
	if (ep_cmd(EP0_OUT, DEPCMD_DEPSTARTCFG, 0, 0, 0) ||
	    ep_cmd(EP0_OUT, DEPCMD_SETEPCONFIG, DEPCFG_TYPE(0) | DEPCFG_MPS(512),
		   DEPCFG_XFERCOMPLETE | DEPCFG_XFERNOTREADY | DEPCFG_EPNUM(EP0_OUT), 0) ||
	    ep_cmd(EP0_OUT, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    ep_cmd(EP0_IN, DEPCMD_SETEPCONFIG, DEPCFG_TYPE(0) | DEPCFG_MPS(512),
		   DEPCFG_XFERCOMPLETE | DEPCFG_XFERNOTREADY | DEPCFG_EPNUM(EP0_IN), 0) ||
	    ep_cmd(EP0_IN, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) {
		ulog("ep0 setup failed");
		return u.err = USBLOG_E_EP0;
	}
	wr32(DALEPENA, (1u << EP0_OUT) | (1u << EP0_IN));
	u.ep0_state = S_SETUP;
	if (start_trb(EP0_OUT, setup_buf, 8, TRBCTL_SETUP)) {
		ulog("first SETUP TRB failed");
		return u.err = USBLOG_E_SETUP;
	}

	/* 7. Run: pull-up on D+, the host starts enumerating. */
	wr32(DCTL, (rd32(DCTL) & ~DCTL_ULSTCHNG_MASK) | DCTL_RUN_STOP);
	if (wait_reg(DSTS, DSTS_DEVCTRLHLT, 0, 500000, &took)) {
		ulog("still halted after run: DSTS=%x DCTL=%x", rd32(DSTS), rd32(DCTL));
		return u.err = USBLOG_E_RUN;
	}
	u.ready = 1;
	ulog("running after %u us: DSTS=%x DCFG=%x, evt @%x, ring %u bytes queued",
	     took, rd32(DSTS), rd32(DCFG), (uint32_t)ea, u.wr - u.rd);
	return 0;
}
