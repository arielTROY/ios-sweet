#pragma once
/*
 * Bare-metal USB CDC-ACM log gadget on the stallion DWC3 (see usblog.c).
 * Polled, single core, bounded waits only.
 */

#define USBLOG_E_ID		(-1)	/* GSNPSID not a DWC3 (controller off / wrong block) */
#define USBLOG_E_RESET		(-2)	/* DCTL.CSFTRST never cleared (PHY/clock not ready) */
#define USBLOG_E_EP0		(-3)	/* ep0 DEPCMD (DEPSTARTCFG/SETEPCONFIG/...) failed */
#define USBLOG_E_SETUP		(-4)	/* could not arm the first SETUP TRB */
#define USBLOG_E_RUN		(-5)	/* DSTS.DEVCTRLHLT stayed set after RUN_STOP */

/* Take the controller over from ABL; 0 = running (pull-up on, waiting for
 * the host), < 0 = one of USBLOG_E_*. Safe to call more than once. */
int usblog_init(void);
/* Service controller events and start bulk IN transfers. Call often. */
void usblog_poll(void);
/* Queue bytes for the host. Never blocks for more than ~20 ms; when the
 * ring is full and the host is not draining it, the oldest bytes go. Data
 * written before usblog_init() is kept and sent once the host connects. */
void usblog_write(const char *s, unsigned n);
/* 1 once the host has sent SET_CONFIGURATION(1). */
int usblog_connected(void);
/* Poll until everything queued was sent or ms elapsed. */
void usblog_flush(unsigned ms);
int usblog_wait_host(unsigned ms);
/* Disconnect and halt the controller so it stops DMA into the shim's
 * memory (call before handing the machine to another kernel). */
void usblog_stop(void);
