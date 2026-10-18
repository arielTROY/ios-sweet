#pragma once
#include <stdint.h>

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/* stallion helpers */
uintptr_t fdt_find_fb(uintptr_t fdt, uint32_t *w, uint32_t *h, uint32_t *stride);
void fdt_dump(uintptr_t fdt, void (*emit)(const char *));
