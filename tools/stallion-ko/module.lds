/* arm64 module sections the kernel's loader insists on (from the kernel's
 * arch/arm64/include/asm/module.lds.h with CONFIG_ARM64_MODULE_PLTS). The
 * loader turns them into NOBITS PLT areas sized at load time. */
SECTIONS {
	.plt 0 : { BYTE(0) }
	.init.plt 0 : { BYTE(0) }
	.text.ftrace_trampoline 0 : { BYTE(0) }
}
