#pragma once
#include <stdint.h>

#ifdef TARGET_STALLION
/* Pixel 10a: panel 1080x2424; FB base is discovered from the ABL FDT at
 * runtime (con_set_fb), falling back to this default guess. */
#define FB_BASE		0xfac00000UL	/* gs101 pattern; runtime-detected */
#define FB_W		1080
#define FB_H		2424
#define FB_STRIDE	(FB_W * 4)	/* a8r8g8b8 */
#else
#define FB_BASE		0x9c000000UL	/* reserved-memory cont_splash_region */
#define FB_W		1080
#define FB_H		2400
#define FB_STRIDE	(FB_W * 4)	/* a8r8g8b8, matches mainline simplefb */
#endif

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U

extern volatile uint32_t *con_fb;

void con_set_fb(uintptr_t pa);	/* stallion: set base before first output */
void log_line(const char *line); /* off-screen sink; weak no-op on sweet */
void con_clear(uint32_t bg);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
