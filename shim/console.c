/*
 * Minimal text console on the ABL splash framebuffer. With the MMU off
 * every access is Device memory, so stay aligned (-mstrict-align).
 */
#include "console.h"
#include "font.h"

#define SCALE		3
#define CH_W		(8 * SCALE)
#define CH_H		(16 * SCALE)
#define MARGIN		24
#define LABEL_COLS	8

#ifdef TARGET_STALLION
volatile uint32_t *con_fb = 0;	/* set by con_set_fb after FDT detection */
#else
volatile uint32_t *con_fb = (volatile uint32_t *)FB_BASE;
#endif
static uint32_t con_bg;
static int row;
static char cur_line[96];
static int cur_col;

/* Off-screen log sink (UART + ramoops prz on stallion; no-op on sweet). */
void log_line(const char *line) __attribute__((weak));
void log_line(const char *line) { (void)line; }

void con_set_fb(uintptr_t pa)
{
	con_fb = (volatile uint32_t *)pa;
}

static void flush_line(void)
{
	cur_line[cur_col] = 0;
	log_line(cur_line);
	cur_col = 0;
}

void con_clear(uint32_t bg)
{
	if (con_fb)
		for (int y = 0; y < FB_H; y++) {
			volatile uint32_t *line = con_fb + y * (FB_STRIDE / 4);
			for (int x = 0; x < FB_W; x++)
				line[x] = bg;
		}
	con_bg = bg;
	row = 0;
}

static void put_char(int col, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	if (cur_col < (int)sizeof(cur_line) - 1)
		cur_line[cur_col++] = ch;
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = MARGIN + col * CH_W, y0 = MARGIN + row * CH_H;
	if (!con_fb || x0 + CH_W > FB_W || y0 + CH_H > FB_H)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? fg : con_bg;
			for (int sy = 0; sy < SCALE; sy++) {
				volatile uint32_t *p = con_fb + (y0 + gy * SCALE + sy) * (FB_STRIDE / 4)
						       + x0 + gx * SCALE;
				for (int sx = 0; sx < SCALE; sx++)
					p[sx] = c;
			}
		}
}

static int put_str(int col, const char *s, uint32_t fg)
{
	while (*s)
		put_char(col++, *s++, fg);
	return col;
}

static void fmt_hex(char *buf, uint64_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = 0; i < digits; i++)
		buf[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xf];
	buf[digits] = 0;
}

void con_skip(void)
{
	flush_line();
	row++;
}

void con_str(const char *label, const char *s)
{
	put_str(0, label, YELLOW);
	put_str(LABEL_COLS, s, WHITE);
	flush_line();
	row++;
}

void con_hex(const char *label, uint64_t v)
{
	char buf[17];
	fmt_hex(buf, v, 16);
	con_str(label, buf);
}

void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char buf[9];
	int col = put_str(0, label, YELLOW);
	col = col < LABEL_COLS ? LABEL_COLS : col + 1;
	col = put_str(col, a, YELLOW);
	fmt_hex(buf, va, 8);
	col = put_str(col, buf, WHITE);
	if (b) {
		col = put_str(col + 1, b, YELLOW);
		fmt_hex(buf, vb, 8);
		put_str(col, buf, WHITE);
	}
	flush_line();
	row++;
}
