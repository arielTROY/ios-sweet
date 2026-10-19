// SPDX-License-Identifier: GPL-2.0
/*
 * sweetmem - small root-only physical-memory peek/poke helper for the
 * Pixel 10a (arm64 GKI 6.1.145-android14-11).
 *
 * Exposes /proc/sweetmem (mode 0600, root-only). Protocol: write a one-line
 * command, then read() the result.
 *
 *   r  <phys_hex> <len_hex>     subsequent read() returns raw bytes of
 *                               physical memory [phys, phys+len). Uses
 *                               memremap(WB) then memremap(WC) for RAM /
 *                               reserved carveouts, falling back to
 *                               ioremap()+readl() word reads for MMIO.
 *   w32 <phys_hex> <val_hex>    ioremap + writel a 32-bit MMIO register;
 *                               read() returns "old=.. new=..".
 *   rl  <phys_hex> <count_dec>  read <count> 32-bit MMIO words via readl;
 *                               read() returns "addr: value" text lines.
 *
 * Bad input never crashes: it returns -EINVAL. Root (CAP_SYS_RAWIO) only.
 */

#define pr_fmt(fmt) "sweetmem: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/capability.h>
#include <linux/kstrtox.h>
#include <linux/minmax.h>
#include <linux/types.h>

#define SM_NAME		"sweetmem"
#define SM_MAX_LEN	(16UL << 20)	/* 16 MiB cap for a single 'r' window */
#define SM_CHUNK	(1UL << 20)	/* map/copy at most 1 MiB per read() */
#define SM_MAX_WORDS	4096U		/* cap for 'rl' */
#define SM_CMDBUF	256
#define SM_LINE		40		/* bytes per 'rl' output line, worst case */

enum sm_mode {
	SM_IDLE = 0,
	SM_RAW,		/* read() returns raw phys bytes */
	SM_TEXT,	/* read() returns st->text (w32 / rl) */
};

struct sm_state {
	enum sm_mode	mode;
	u64		phys;
	u64		len;
	char		*text;
	size_t		text_len;
};

static struct proc_dir_entry *sm_pde;

/* ---- raw physical read ------------------------------------------------- */

static ssize_t sm_read_raw(struct sm_state *st, char __user *ubuf,
			   size_t count, loff_t *ppos)
{
	u64 pos = *ppos;
	phys_addr_t pa;
	size_t n;
	void *map;
	char *bounce;
	ssize_t ret;

	if (pos >= st->len)
		return 0;

	n = min_t(u64, count, st->len - pos);
	n = min_t(size_t, n, SM_CHUNK);
	if (n == 0)
		return 0;

	pa = (phys_addr_t)(st->phys + pos);

	bounce = kvmalloc(n, GFP_KERNEL);
	if (!bounce)
		return -ENOMEM;

	/* System RAM and no-map reserved carveouts: direct mapping. */
	map = memremap(pa, n, MEMREMAP_WB);
	if (!map)
		map = memremap(pa, n, MEMREMAP_WC);

	if (map) {
		memcpy(bounce, map, n);
		memunmap(map);
	} else {
		/* MMIO: ioremap an aligned span and read 32-bit words. */
		u64 base = pa & ~0x3ULL;
		u64 end  = ALIGN(pa + n, 4);
		size_t span = (size_t)(end - base);
		size_t off  = (size_t)(pa - base);
		void __iomem *io;
		u8 *tmp;
		size_t j;

		tmp = kvmalloc(span, GFP_KERNEL);
		if (!tmp) {
			kvfree(bounce);
			return -ENOMEM;
		}
		io = ioremap(base, span);
		if (!io) {
			kvfree(tmp);
			kvfree(bounce);
			return -EFAULT;
		}
		for (j = 0; j < span; j += 4)
			*(u32 *)(tmp + j) = readl(io + j);
		iounmap(io);
		memcpy(bounce, tmp + off, n);
		kvfree(tmp);
	}

	if (copy_to_user(ubuf, bounce, n)) {
		ret = -EFAULT;
	} else {
		*ppos += n;
		ret = n;
	}
	kvfree(bounce);
	return ret;
}

/* ---- command parsing --------------------------------------------------- */

/* split up to 3 whitespace tokens; returns token count */
static int sm_split(char *s, char *tok[3])
{
	int n = 0;

	while (*s && n < 3) {
		while (*s == ' ' || *s == '\t')
			*s++ = '\0';
		if (!*s)
			break;
		tok[n++] = s;
		while (*s && *s != ' ' && *s != '\t' && *s != '\n')
			s++;
	}
	/* strip trailing newline on last token */
	while (*s) {
		if (*s == '\n' || *s == '\r')
			*s = '\0';
		s++;
	}
	return n;
}

static int sm_do_rl(struct sm_state *st, u64 pa, u32 words)
{
	void __iomem *io;
	char *buf;
	size_t cap, used = 0;
	u32 i;

	if (words == 0 || words > SM_MAX_WORDS)
		return -EINVAL;
	if (pa & 0x3ULL)
		return -EINVAL;

	io = ioremap((phys_addr_t)pa, (size_t)words * 4);
	if (!io)
		return -EFAULT;

	cap = (size_t)words * SM_LINE + 1;
	buf = kvmalloc(cap, GFP_KERNEL);
	if (!buf) {
		iounmap(io);
		return -ENOMEM;
	}
	for (i = 0; i < words; i++) {
		u32 v = readl(io + i * 4);

		used += scnprintf(buf + used, cap - used,
				  "0x%016llx: 0x%08x\n",
				  (unsigned long long)(pa + i * 4), v);
	}
	iounmap(io);

	kvfree(st->text);
	st->text = buf;
	st->text_len = used;
	st->mode = SM_TEXT;
	return 0;
}

static int sm_do_w32(struct sm_state *st, u64 pa, u32 val)
{
	void __iomem *io;
	char *buf;
	u32 oldv, newv;

	if (pa & 0x3ULL)
		return -EINVAL;

	io = ioremap((phys_addr_t)pa, 4);
	if (!io)
		return -EFAULT;

	oldv = readl(io);
	writel(val, io);
	newv = readl(io);
	iounmap(io);

	buf = kvmalloc(96, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	kvfree(st->text);
	st->text = buf;
	st->text_len = scnprintf(buf, 96,
				 "0x%016llx: old=0x%08x new=0x%08x\n",
				 (unsigned long long)pa, oldv, newv);
	st->mode = SM_TEXT;
	return 0;
}

/* ---- file ops ---------------------------------------------------------- */

static ssize_t sm_write(struct file *f, const char __user *ubuf,
			size_t count, loff_t *ppos)
{
	struct sm_state *st = f->private_data;
	char kbuf[SM_CMDBUF];
	char *tok[3] = { NULL, NULL, NULL };
	u64 a = 0, b = 0;
	int ntok, rc;

	if (!st)
		return -EINVAL;
	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	if (count == 0 || count >= SM_CMDBUF)
		return -EINVAL;
	if (copy_from_user(kbuf, ubuf, count))
		return -EFAULT;
	kbuf[count] = '\0';

	ntok = sm_split(kbuf, tok);
	if (ntok < 3 || !tok[0] || !tok[1] || !tok[2])
		return -EINVAL;

	if (kstrtoull(tok[1], 16, &a))
		return -EINVAL;

	if (!strcmp(tok[0], "r")) {
		if (kstrtoull(tok[2], 16, &b))
			return -EINVAL;
		if (b == 0 || b > SM_MAX_LEN)
			return -EINVAL;
		st->mode = SM_RAW;
		st->phys = a;
		st->len  = b;
		kvfree(st->text);
		st->text = NULL;
		st->text_len = 0;
	} else if (!strcmp(tok[0], "w32")) {
		u32 v;

		if (kstrtou32(tok[2], 16, &v))
			return -EINVAL;
		rc = sm_do_w32(st, a, v);
		if (rc)
			return rc;
	} else if (!strcmp(tok[0], "rl")) {
		u32 w;

		if (kstrtou32(tok[2], 10, &w))
			return -EINVAL;
		rc = sm_do_rl(st, a, w);
		if (rc)
			return rc;
	} else {
		return -EINVAL;
	}

	/* reset read position for the new command */
	*ppos = 0;
	return count;
}

static ssize_t sm_read(struct file *f, char __user *ubuf,
		       size_t count, loff_t *ppos)
{
	struct sm_state *st = f->private_data;

	if (!st)
		return -EINVAL;

	switch (st->mode) {
	case SM_RAW:
		return sm_read_raw(st, ubuf, count, ppos);
	case SM_TEXT:
		return simple_read_from_buffer(ubuf, count, ppos,
					       st->text, st->text_len);
	default:
		return -EINVAL;
	}
}

static int sm_open(struct inode *ino, struct file *f)
{
	struct sm_state *st;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;

	st = kvmalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	memset(st, 0, sizeof(*st));
	st->mode = SM_IDLE;
	f->private_data = st;
	return 0;
}

static int sm_release(struct inode *ino, struct file *f)
{
	struct sm_state *st = f->private_data;

	if (st) {
		kvfree(st->text);
		kvfree(st);
		f->private_data = NULL;
	}
	return 0;
}

static const struct proc_ops sm_pops = {
	.proc_open	= sm_open,
	.proc_release	= sm_release,
	.proc_read	= sm_read,
	.proc_write	= sm_write,
	.proc_lseek	= default_llseek,
};

static int __init sweetmem_init(void)
{
	sm_pde = proc_create(SM_NAME, 0600, NULL, &sm_pops);
	if (!sm_pde) {
		pr_err("failed to create /proc/%s\n", SM_NAME);
		return -ENOMEM;
	}
	pr_info("loaded: /proc/%s (0600, CAP_SYS_RAWIO); cmds: r/w32/rl\n",
		SM_NAME);
	return 0;
}

static void __exit sweetmem_exit(void)
{
	if (sm_pde)
		remove_proc_entry(SM_NAME, NULL);
	pr_info("unloaded\n");
}

module_init(sweetmem_init);
module_exit(sweetmem_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("sweet");
MODULE_DESCRIPTION("root-only physical memory peek/poke via /proc/sweetmem");
