/*
 * Just enough flattened-device-tree parsing to find what ABL put in
 * /chosen (initrd location). Big-endian, 4-byte aligned tokens.
 */
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	/* byte loads are fine on Device memory as long as they are aligned to 1 */
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	int depth = 0, in_chosen = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			in_chosen = depth == 2 && streq(name, "chosen");
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (in_chosen)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_chosen && streq((const char *)(strings + nameoff), prop)) {
				if (len == 4)
					*out = be32(val);
				else if (len == 8)
					*out = (uint64_t)be32(val) << 32 | be32(val + 4);
				else
					return 0;
				return 1;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return 0;
		}
	}
}

/*
 * Walk the whole tree once, calling cb for every node and every property.
 * cb(kind, node_name, prop_name, val_ptr, val_len): kind 0=node, 1=prop.
 * Returns 1 if the walk completed.
 */
static int fdt_walk(uintptr_t fdt,
    void (*cb)(int kind, const char *node, const char *prop, uintptr_t val, uint32_t len))
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);
	uintptr_t strings = fdt + be32(fdt + 12);
	char node[64];

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			int i = 0;
			while (name[i] && i < 63) { node[i] = name[i]; i++; }
			node[i] = 0;
			cb(0, node, 0, 0, 0);
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
		case FDT_END:
			if (tok == FDT_END)
				return 1;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			cb(1, node, (const char *)(strings + nameoff), val, len);
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:
			return 0;
		}
	}
}

/* context for the framebuffer search */
static uintptr_t fb_reg, fb_w, fb_h, fb_stride;
static int fb_found;

static int has_sub(const char *s, const char *sub)
{
	while (*s) {
		const char *a = s, *b = sub;
		while (*a && *b && *a == *b) { a++; b++; }
		if (!*b)
			return 1;
		s++;
	}
	return 0;
}

static int is_fb_node(const char *node)
{
	return has_sub(node, "framebuffer") || has_sub(node, "splash") ||
	       has_sub(node, "simplefb") || has_sub(node, "simple-fb");
}

static uint32_t rd_be32(uintptr_t a) { return __builtin_bswap32(*(volatile uint32_t *)a); }

static void fb_cb(int kind, const char *node, const char *prop, uintptr_t val, uint32_t len)
{
	if (kind == 0) {
		if (is_fb_node(node))
			fb_found |= 1;	/* candidate node, maybe */
		return;
	}
	if (!is_fb_node(node))
		return;
	if (streq(prop, "reg") && len >= 8) {
		fb_reg = ((uintptr_t)rd_be32(val) << 32) | rd_be32(val + 4);
		if (fb_reg < 0x1000)	/* reg may be two 32-bit cells */
			fb_reg = rd_be32(val);
		fb_found |= 2;
	} else if (streq(prop, "width") && len >= 4) {
		fb_w = rd_be32(val);
	} else if (streq(prop, "height") && len >= 4) {
		fb_h = rd_be32(val);
	} else if (streq(prop, "stride") && len >= 4) {
		fb_stride = rd_be32(val);
	}
}

uintptr_t fdt_find_fb(uintptr_t fdt, uint32_t *w, uint32_t *h, uint32_t *stride)
{
	fb_reg = fb_w = fb_h = fb_stride = 0;
	fb_found = 0;
	fdt_walk(fdt, fb_cb);
	if (!(fb_found & 2))
		return 0;
	if (w) *w = fb_w;
	if (h) *h = fb_h;
	if (stride) *stride = fb_stride;
	return fb_reg;
}

/* context for the full dump */
static void (*dump_emit)(const char *);

static void emit_hex(char *buf, uint64_t v)
{
	static const char hex[] = "0123456789ABCDEF";
	buf[0] = hex[(v >> 28) & 0xf]; buf[1] = hex[(v >> 24) & 0xf];
	buf[2] = hex[(v >> 20) & 0xf]; buf[3] = hex[(v >> 16) & 0xf];
	buf[4] = hex[(v >> 12) & 0xf]; buf[5] = hex[(v >> 8) & 0xf];
	buf[6] = hex[(v >> 4) & 0xf]; buf[7] = hex[v & 0xf];
	buf[8] = 0;
}

static void dump_cb(int kind, const char *node, const char *prop, uintptr_t val, uint32_t len)
{
	char line[96];
	int i = 0;
	if (kind == 0) {
		const char *p = "NODE ";
		while (*p) line[i++] = *p++;
		while (*node && i < 60) line[i++] = *node++;
		line[i] = 0;
		dump_emit(line);
		return;
	}
	/* only reg and compatible to keep the log small */
	if (!streq(prop, "reg") && !streq(prop, "compatible"))
		return;
	const char *p = "  ";
	while (*p) line[i++] = *p++;
	p = node;
	while (*p && i < 40) line[i++] = *p++;
	line[i++] = ' ';
	p = prop;
	while (*p) line[i++] = *p++;
	line[i++] = '=';
	if (streq(prop, "reg") && len >= 8) {
		emit_hex(line + i, ((uint64_t)rd_be32(val) << 32) | rd_be32(val + 4));
		i += 8;
		if (len >= 16) {
			line[i++] = '+';
			emit_hex(line + i, ((uint64_t)rd_be32(val + 8) << 32) | rd_be32(val + 12));
			i += 8;
		}
	} else {
		const char *s = (const char *)val;
		while (*s && i < 92) line[i++] = *s++;
	}
	line[i] = 0;
	dump_emit(line);
}

void fdt_dump(uintptr_t fdt, void (*emit)(const char *))
{
	dump_emit = emit;
	if (!fdt_walk(fdt, dump_cb))
		emit("FDT: bad magic or truncated");
}
