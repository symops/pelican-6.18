// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek reserved-memory remap helper (vendor)
 *
 * The vendor RTD129x trees use reserved-memory nodes with:
 *   compatible = "rsvmem-remap";
 *   save_remap_name = "rbus" | "common" | "ringbuf";
 *
 * and expect these regions to be ioremapped early.
 *
 * This is *not* an upstream binding. It's here to keep the vendor DTS
 * semantics working while porting WD My Cloud Home (RTD1295) to 6.x.
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>
#include <linux/slab.h>
#include <linux/string.h>

struct rtk_rsvmem_map {
	struct list_head list;
	char *name;
	void __iomem *addr;
	phys_addr_t base;
	phys_addr_t size;
};

static LIST_HEAD(rtk_rsvmem_maps);

#include <linux/soc/realtek/rtk_rsvmem.h>

void __iomem *rtk_rsvmem_remap_get(const char *name)
{
	struct rtk_rsvmem_map *m;

	if (!name)
		return NULL;

	list_for_each_entry(m, &rtk_rsvmem_maps, list) {
		if (!strcmp(m->name, name))
			return m->addr;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(rtk_rsvmem_remap_get);

static int __init rtk_rsvmem_remap(struct reserved_mem *rmem)
{
	const char *save;
	struct rtk_rsvmem_map *m;
	void __iomem *addr;

	if (!rmem || !rmem->size)
		return 0;

	save = of_get_flat_dt_prop(rmem->fdt_node, "save_remap_name", NULL);
	if (!save) {
		pr_warn("rtk-rsvmem: %s: missing save_remap_name\n",
			rmem->name ? rmem->name : "<unnamed>");
		return 0;
	}

	addr = ioremap(rmem->base, rmem->size);
	if (!addr) {
		pr_err("rtk-rsvmem: %s/%s: ioremap failed for %pa+%pa\n",
		       rmem->name ? rmem->name : "<unnamed>", save,
		       &rmem->base, &rmem->size);
		return 0;
	}

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m) {
		iounmap(addr);
		return 0;
	}

	m->name = kstrdup(save, GFP_KERNEL);
	if (!m->name) {
		kfree(m);
		iounmap(addr);
		return 0;
	}

	m->addr = addr;
	m->base = rmem->base;
	m->size = rmem->size;
	list_add_tail(&m->list, &rtk_rsvmem_maps);

	pr_info("rtk-rsvmem: %s/%s mapped %pa+%pa -> %p\n",
		rmem->name ? rmem->name : "<unnamed>", m->name,
		&m->base, &m->size, m->addr);

	return 0;
}

RESERVEDMEM_OF_DECLARE(rtk_rsvmem_remap, "rsvmem-remap", rtk_rsvmem_remap);

