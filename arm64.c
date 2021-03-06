/*
 * arm64.c - core analysis suite
 *
 * Copyright (C) 2012-2015 David Anderson
 * Copyright (C) 2012-2015 Red Hat, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifdef ARM64

#include "defs.h"
#include <elf.h>

#define NOT_IMPLEMENTED(X) error((X), "%s: function not implemented\n", __func__)

static struct machine_specific arm64_machine_specific = { 0 };
static int arm64_verify_symbol(const char *, ulong, char);
static void arm64_parse_cmdline_args(void);
static void arm64_calc_phys_offset(void);
static void arm64_calc_virtual_memory_ranges(void);
static int arm64_kdump_phys_base(ulong *);
static ulong arm64_processor_speed(void);
static void arm64_init_kernel_pgd(void);
static int arm64_kvtop(struct task_context *, ulong, physaddr_t *, int);
static int arm64_uvtop(struct task_context *, ulong, physaddr_t *, int);
static int arm64_vtop_2level_64k(ulong, ulong, physaddr_t *, int);
static int arm64_vtop_3level_4k(ulong, ulong, physaddr_t *, int);
static ulong arm64_get_task_pgd(ulong);
static void arm64_stackframe_init(void);
static int arm64_eframe_search(struct bt_info *);
static int arm64_is_kernel_exception_frame(struct bt_info *, ulong);
static int arm64_in_exception_text(ulong);
static void arm64_back_trace_cmd(struct bt_info *);
static void arm64_print_text_symbols(struct bt_info *, struct arm64_stackframe *, FILE *);
static int arm64_print_stackframe_entry(struct bt_info *, int, struct arm64_stackframe *, FILE *);
static void arm64_display_full_frame(struct bt_info *, ulong);
static int arm64_unwind_frame(struct bt_info *, struct arm64_stackframe *);
static int arm64_get_dumpfile_stackframe(struct bt_info *, struct arm64_stackframe *);
static int arm64_in_kdump_text(struct bt_info *, struct arm64_stackframe *);
static int arm64_get_stackframe(struct bt_info *, struct arm64_stackframe *);
static void arm64_get_stack_frame(struct bt_info *, ulong *, ulong *);
static void arm64_print_exception_frame(struct bt_info *, ulong, int, FILE *ofp);
static void arm64_do_bt_reference_check(struct bt_info *, ulong, char *);
static int arm64_translate_pte(ulong, void *, ulonglong);
static ulong arm64_vmalloc_start(void);
static int arm64_is_task_addr(ulong);
static int arm64_dis_filter(ulong, char *, unsigned int);
static void arm64_cmd_mach(void);
static void arm64_display_machine_stats(void);
static int arm64_get_smp_cpus(void);
static void arm64_clear_machdep_cache(void);
static int arm64_in_alternate_stack(int, ulong);
static int arm64_get_kvaddr_ranges(struct vaddr_range *);
static int arm64_get_crash_notes(void);
static void arm64_calc_VA_BITS(void);
static int arm64_is_uvaddr(ulong, struct task_context *);


/*
 * Do all necessary machine-specific setup here. This is called several times
 * during initialization.
 */
void
arm64_init(int when)
{
	ulong value;
	struct machine_specific *ms;

#if defined(__x86_64__)
	if (ACTIVE())
		error(FATAL, "compiled for the ARM64 architecture\n");
#endif

	switch (when) {
	case SETUP_ENV:
		machdep->process_elf_notes = process_elf64_notes;
		break;

	case PRE_SYMTAB:
		machdep->machspec = &arm64_machine_specific;
		machdep->verify_symbol = arm64_verify_symbol;
		if (pc->flags & KERNEL_DEBUG_QUERY)
			return;
		machdep->verify_paddr = generic_verify_paddr;
		if (machdep->cmdline_args[0])
			arm64_parse_cmdline_args();
		machdep->flags |= MACHDEP_BT_TEXT;
		break;

	case PRE_GDB:
		if (!machdep->pagesize &&
		    kernel_symbol_exists("swapper_pg_dir") &&
		    kernel_symbol_exists("idmap_pg_dir")) {
			value = symbol_value("swapper_pg_dir") -
				symbol_value("idmap_pg_dir");
			/*
			 * idmap_pg_dir is 2 pages prior to 4.1,
			 * and 3 pages thereafter.  Only 4K and 64K 
			 * page sizes are supported.
			 */
			switch (value)
			{
			case (4096 * 2):
			case (4096 * 3):
				machdep->pagesize = 4096;
				break;
			case (65536 * 2):
			case (65536 * 3):
				machdep->pagesize = 65536;
				break;
			}
		} else if (ACTIVE())
			machdep->pagesize = memory_page_size();   /* host */

		machdep->pageshift = ffs(machdep->pagesize) - 1;
		machdep->pageoffset = machdep->pagesize - 1;
		machdep->pagemask = ~((ulonglong)machdep->pageoffset);

		arm64_calc_VA_BITS();
		machdep->machspec->page_offset = ARM64_PAGE_OFFSET;
		machdep->identity_map_base = ARM64_PAGE_OFFSET;
		machdep->machspec->userspace_top = ARM64_USERSPACE_TOP;
		machdep->machspec->modules_vaddr = ARM64_MODULES_VADDR;
		machdep->machspec->modules_end = ARM64_MODULES_END;
		machdep->machspec->vmalloc_start_addr = ARM64_VMALLOC_START;
		machdep->machspec->vmalloc_end = ARM64_VMALLOC_END;
		machdep->kvbase = ARM64_VMALLOC_START;
		machdep->machspec->vmemmap_vaddr = ARM64_VMEMMAP_VADDR;
		machdep->machspec->vmemmap_end = ARM64_VMEMMAP_END;

		switch (machdep->pagesize)
		{
		case 4096:
			machdep->flags |= VM_L3_4K;
			machdep->ptrs_per_pgd = PTRS_PER_PGD_L3_4K;
			if ((machdep->pgd = 
			    (char *)malloc(PTRS_PER_PGD_L3_4K * 8)) == NULL)
				error(FATAL, "cannot malloc pgd space.");
			if ((machdep->pmd = 
			    (char *)malloc(PTRS_PER_PMD_L3_4K * 8)) == NULL)
				error(FATAL, "cannot malloc pmd space.");
			if ((machdep->ptbl = 
			    (char *)malloc(PTRS_PER_PTE_L3_4K * 8)) == NULL)
				error(FATAL, "cannot malloc ptbl space.");
			machdep->pud = NULL;  /* not used */
			break;

		case 65536:
			machdep->flags |= VM_L2_64K;
			machdep->ptrs_per_pgd = PTRS_PER_PGD_L2_64K;
			if ((machdep->pgd = 
			    (char *)malloc(PTRS_PER_PGD_L2_64K * 8)) == NULL)
				error(FATAL, "cannot malloc pgd space.");
			if ((machdep->ptbl = 
			    (char *)malloc(PTRS_PER_PTE_L2_64K * 8)) == NULL)
				error(FATAL, "cannot malloc ptbl space.");
			machdep->pmd = NULL;  /* not used */
			machdep->pud = NULL;  /* not used */
			break;

		default:
			if (machdep->pagesize)
				error(FATAL, "invalid/unsupported page size: %d\n", 
					machdep->pagesize);
			else
				error(FATAL, "cannot determine page size\n");
		}

		machdep->last_pud_read = 0;  /* not used */
		machdep->last_pgd_read = 0;
		machdep->last_pmd_read = 0;
		machdep->last_ptbl_read = 0;
		machdep->clear_machdep_cache = arm64_clear_machdep_cache;

		machdep->stacksize = ARM64_STACK_SIZE;
		machdep->flags |= VMEMMAP;

		arm64_calc_phys_offset();
		
		machdep->uvtop = arm64_uvtop;
		machdep->kvtop = arm64_kvtop;
		machdep->is_kvaddr = generic_is_kvaddr;
		machdep->is_uvaddr = arm64_is_uvaddr;
		machdep->eframe_search = arm64_eframe_search;
		machdep->back_trace = arm64_back_trace_cmd;
		machdep->in_alternate_stack = arm64_in_alternate_stack;
		machdep->processor_speed = arm64_processor_speed;
		machdep->get_task_pgd = arm64_get_task_pgd;
		machdep->get_stack_frame = arm64_get_stack_frame;
		machdep->get_stackbase = generic_get_stackbase;
		machdep->get_stacktop = generic_get_stacktop;
		machdep->translate_pte = arm64_translate_pte;
		machdep->memory_size = generic_memory_size;
		machdep->vmalloc_start = arm64_vmalloc_start;
		machdep->get_kvaddr_ranges = arm64_get_kvaddr_ranges;
		machdep->is_task_addr = arm64_is_task_addr;
		machdep->dis_filter = arm64_dis_filter;
		machdep->cmd_mach = arm64_cmd_mach;
		machdep->get_smp_cpus = arm64_get_smp_cpus;
		machdep->line_number_hooks = NULL;
		machdep->value_to_symbol = generic_machdep_value_to_symbol;
		machdep->dump_irq = generic_dump_irq;
		machdep->show_interrupts = generic_show_interrupts;
		machdep->get_irq_affinity = generic_get_irq_affinity;
		machdep->dumpfile_init = NULL;
		machdep->verify_line_number = NULL;
		machdep->init_kernel_pgd = arm64_init_kernel_pgd;
		break;

	case POST_GDB:
		arm64_calc_virtual_memory_ranges();
		machdep->section_size_bits = _SECTION_SIZE_BITS;
		machdep->max_physmem_bits = _MAX_PHYSMEM_BITS;
		ms = machdep->machspec;

		if (THIS_KERNEL_VERSION >= LINUX(4,0,0)) {
			ms->__SWP_TYPE_BITS = 6;
			ms->__SWP_TYPE_SHIFT = 2;
			ms->__SWP_TYPE_MASK = ((1UL << ms->__SWP_TYPE_BITS) - 1);
			ms->__SWP_OFFSET_SHIFT = (ms->__SWP_TYPE_BITS + ms->__SWP_TYPE_SHIFT);
			ms->__SWP_OFFSET_BITS = 50;
			ms->__SWP_OFFSET_MASK = ((1UL << ms->__SWP_OFFSET_BITS) - 1);
			ms->PTE_PROT_NONE = (1UL << 58); 
			ms->PTE_FILE = 0;  /* unused */
		} else if (THIS_KERNEL_VERSION >= LINUX(3,13,0)) {
			ms->__SWP_TYPE_BITS = 6;
			ms->__SWP_TYPE_SHIFT = 3;
			ms->__SWP_TYPE_MASK = ((1UL << ms->__SWP_TYPE_BITS) - 1);
			ms->__SWP_OFFSET_SHIFT = (ms->__SWP_TYPE_BITS + ms->__SWP_TYPE_SHIFT);
			ms->__SWP_OFFSET_BITS = 49;
			ms->__SWP_OFFSET_MASK = ((1UL << ms->__SWP_OFFSET_BITS) - 1);
			ms->PTE_PROT_NONE = (1UL << 58); 
			ms->PTE_FILE = (1UL << 2);
		} else if (THIS_KERNEL_VERSION >= LINUX(3,11,0)) {
			ms->__SWP_TYPE_BITS = 6;
			ms->__SWP_TYPE_SHIFT = 4;
			ms->__SWP_TYPE_MASK = ((1UL << ms->__SWP_TYPE_BITS) - 1);
			ms->__SWP_OFFSET_SHIFT = (ms->__SWP_TYPE_BITS + ms->__SWP_TYPE_SHIFT);
			ms->__SWP_OFFSET_BITS = 0;  /* unused */ 
			ms->__SWP_OFFSET_MASK = 0;  /* unused */ 
			ms->PTE_PROT_NONE = (1UL << 2); 
			ms->PTE_FILE = (1UL << 3);
		} else {
			ms->__SWP_TYPE_BITS = 6;
			ms->__SWP_TYPE_SHIFT = 3;
			ms->__SWP_TYPE_MASK = ((1UL << ms->__SWP_TYPE_BITS) - 1);
			ms->__SWP_OFFSET_SHIFT = (ms->__SWP_TYPE_BITS + ms->__SWP_TYPE_SHIFT);
			ms->__SWP_OFFSET_BITS = 0;  /* unused */ 
			ms->__SWP_OFFSET_MASK = 0;  /* unused */
			ms->PTE_PROT_NONE = (1UL << 1); 
			ms->PTE_FILE = (1UL << 2);
		}

		if (symbol_exists("irq_desc"))
			ARRAY_LENGTH_INIT(machdep->nr_irqs, irq_desc,
				  "irq_desc", NULL, 0);
		else if (kernel_symbol_exists("nr_irqs"))
			get_symbol_data("nr_irqs", sizeof(unsigned int),
				&machdep->nr_irqs);

		if (!machdep->hz)
			machdep->hz = 100;

		arm64_stackframe_init();
		break;

	case POST_VM:
		/*
		 * crash_notes contains machine specific information about the
		 * crash. In particular, it contains CPU registers at the time
		 * of the crash. We need this information to extract correct
		 * backtraces from the panic task.
		 */
		if (!LIVE() && !arm64_get_crash_notes())
			error(WARNING, 
			    "cannot retrieve registers for active task%s\n\n",
				kt->cpus > 1 ? "s" : "");

		break;

	case LOG_ONLY:
		machdep->machspec = &arm64_machine_specific;
		arm64_calc_VA_BITS();
		arm64_calc_phys_offset();
		machdep->machspec->page_offset = ARM64_PAGE_OFFSET;
		break;
	}
}

/*
 * Accept or reject a symbol from the kernel namelist.
 */
static int
arm64_verify_symbol(const char *name, ulong value, char type)
{
	if (!name || !strlen(name))
		return FALSE;

	if (((type == 'A') || (type == 'a')) && (highest_bit_long(value) != 63))
		return FALSE;

	if ((value == 0) && 
	    ((type == 'a') || (type == 'n') || (type == 'N') || (type == 'U')))
		return FALSE;

	if (STREQ(name, "$d") || STREQ(name, "$x"))
		return FALSE;
	
	if ((type == 'A') && STRNEQ(name, "__crc_"))
		return FALSE;

	if (!(machdep->flags & KSYMS_START) && STREQ(name, "idmap_pg_dir"))
		machdep->flags |= KSYMS_START;

	return TRUE;
}


void
arm64_dump_machdep_table(ulong arg)
{
	const struct machine_specific *ms;
	int others, i;

	others = 0;
	fprintf(fp, "               flags: %lx (", machdep->flags);
	if (machdep->flags & KSYMS_START)
		fprintf(fp, "%sKSYMS_START", others++ ? "|" : "");
	if (machdep->flags & PHYS_OFFSET)
		fprintf(fp, "%sPHYS_OFFSET", others++ ? "|" : "");
	if (machdep->flags & VM_L2_64K)
		fprintf(fp, "%sVM_L2_64K", others++ ? "|" : "");
	if (machdep->flags & VM_L3_4K)
		fprintf(fp, "%sVM_L3_4K", others++ ? "|" : "");
	if (machdep->flags & VMEMMAP)
		fprintf(fp, "%sVMEMMAP", others++ ? "|" : "");
	if (machdep->flags & KDUMP_ENABLED)
		fprintf(fp, "%sKDUMP_ENABLED", others++ ? "|" : "");
	if (machdep->flags & MACHDEP_BT_TEXT)
		fprintf(fp, "%sMACHDEP_BT_TEXT", others++ ? "|" : "");
	fprintf(fp, ")\n");

	fprintf(fp, "              kvbase: %lx\n", machdep->kvbase);
	fprintf(fp, "   identity_map_base: %lx\n", machdep->identity_map_base);
	fprintf(fp, "            pagesize: %d\n", machdep->pagesize);
	fprintf(fp, "           pageshift: %d\n", machdep->pageshift);
	fprintf(fp, "            pagemask: %lx\n", (ulong)machdep->pagemask);
	fprintf(fp, "          pageoffset: %lx\n", machdep->pageoffset);
	fprintf(fp, "           stacksize: %ld\n", machdep->stacksize);
	fprintf(fp, "                  hz: %d\n", machdep->hz);
	fprintf(fp, "                 mhz: %ld\n", machdep->mhz);
	fprintf(fp, "             memsize: %lld (0x%llx)\n",
		(ulonglong)machdep->memsize, (ulonglong)machdep->memsize);
	fprintf(fp, "                bits: %d\n", machdep->bits);
	fprintf(fp, "             nr_irqs: %d\n", machdep->nr_irqs);
	fprintf(fp, "       eframe_search: arm64_eframe_search()\n");
	fprintf(fp, "          back_trace: arm64_back_trace_cmd()\n");
	fprintf(fp, "  in_alternate_stack: arm64_in_alternate_stack()\n");
	fprintf(fp, "     processor_speed: arm64_processor_speed()\n");
	fprintf(fp, "               uvtop: arm64_uvtop()->%s()\n",
		machdep->flags & VM_L3_4K ? 
		"arm64_vtop_3level_4k" : "arm64_vtop_2level_64k");
	fprintf(fp, "               kvtop: arm64_kvtop()->%s()\n",
		machdep->flags & VM_L3_4K ? 
		"arm64_vtop_3level_4k" : "arm64_vtop_2level_64k");
	fprintf(fp, "        get_task_pgd: arm64_get_task_pgd()\n");
	fprintf(fp, "            dump_irq: generic_dump_irq()\n");
	fprintf(fp, "     get_stack_frame: arm64_get_stack_frame()\n");
	fprintf(fp, "       get_stackbase: generic_get_stackbase()\n");
	fprintf(fp, "        get_stacktop: generic_get_stacktop()\n");
	fprintf(fp, "       translate_pte: arm64_translate_pte()\n");
	fprintf(fp, "         memory_size: generic_memory_size()\n");
	fprintf(fp, "       vmalloc_start: arm64_vmalloc_start()\n");
	fprintf(fp, "   get_kvaddr_ranges: arm64_get_kvaddr_ranges()\n");
	fprintf(fp, "        is_task_addr: arm64_is_task_addr()\n");
	fprintf(fp, "       verify_symbol: arm64_verify_symbol()\n");
	fprintf(fp, "          dis_filter: arm64_dis_filter()\n");
	fprintf(fp, "            cmd_mach: arm64_cmd_mach()\n");
	fprintf(fp, "        get_smp_cpus: arm64_get_smp_cpus()\n");
	fprintf(fp, "           is_kvaddr: generic_is_kvaddr()\n");
	fprintf(fp, "           is_uvaddr: arm64_is_uvaddr()\n");
	fprintf(fp, "     value_to_symbol: generic_machdep_value_to_symbol()\n");
	fprintf(fp, "     init_kernel_pgd: arm64_init_kernel_pgd\n");
	fprintf(fp, "        verify_paddr: generic_verify_paddr()\n");
	fprintf(fp, "     show_interrupts: generic_show_interrupts()\n");
	fprintf(fp, "    get_irq_affinity: generic_get_irq_affinity()\n");
	fprintf(fp, "       dumpfile_init: (not used)\n");
	fprintf(fp, "   process_elf_notes: process_elf64_notes()\n");
	fprintf(fp, "  verify_line_number: (not used)\n");

	fprintf(fp, "  xendump_p2m_create: (n/a)\n");
	fprintf(fp, "xen_kdump_p2m_create: (n/a)\n");
        fprintf(fp, "  xendump_panic_task: (n/a)\n");
        fprintf(fp, "    get_xendump_regs: (n/a)\n");
	fprintf(fp, "   line_number_hooks: (not used)\n");
	fprintf(fp, "       last_pud_read: (not used)\n");
	fprintf(fp, "       last_pgd_read: %lx\n", machdep->last_pgd_read);
	fprintf(fp, "       last_pmd_read: ");
	if (PAGESIZE() == 65536)
		fprintf(fp, "(not used)\n");
	else
		fprintf(fp, "%lx\n", machdep->last_pmd_read);
	fprintf(fp, "      last_ptbl_read: %lx\n", machdep->last_ptbl_read);
	fprintf(fp, " clear_machdep_cache: arm64_clear_machdep_cache()\n");
	fprintf(fp, "                 pgd: %lx\n", (ulong)machdep->pgd);
	fprintf(fp, "                 pmd: %lx\n", (ulong)machdep->pmd);
	fprintf(fp, "                ptbl: %lx\n", (ulong)machdep->ptbl);
	fprintf(fp, "        ptrs_per_pgd: %d\n", machdep->ptrs_per_pgd);
	fprintf(fp, "   section_size_bits: %ld\n", machdep->section_size_bits);
	fprintf(fp, "    max_physmem_bits: %ld\n", machdep->max_physmem_bits);
	fprintf(fp, "   sections_per_root: %ld\n", machdep->sections_per_root);

	for (i = 0; i < MAX_MACHDEP_ARGS; i++) {
		fprintf(fp, "     cmdline_args[%d]: %s\n",
			i, machdep->cmdline_args[i] ?
			machdep->cmdline_args[i] : "(unused)");
	}

	ms = machdep->machspec;

	fprintf(fp, "            machspec: %lx\n", (ulong)ms);
	fprintf(fp, "               VA_BITS: %ld\n", ms->VA_BITS);
	fprintf(fp, "         userspace_top: %016lx\n", ms->userspace_top);
	fprintf(fp, "           page_offset: %016lx\n", ms->page_offset);
	fprintf(fp, "    vmalloc_start_addr: %016lx\n", ms->vmalloc_start_addr);
	fprintf(fp, "           vmalloc_end: %016lx\n", ms->vmalloc_end);
	fprintf(fp, "         modules_vaddr: %016lx\n", ms->modules_vaddr);
	fprintf(fp, "           modules_end: %016lx\n", ms->modules_end);
	fprintf(fp, "         vmemmap_vaddr: %016lx\n", ms->vmemmap_vaddr);
	fprintf(fp, "           vmemmap_end: %016lx\n", ms->vmemmap_end);
	fprintf(fp, "           phys_offset: %lx\n", ms->phys_offset);
	fprintf(fp, "__exception_text_start: %lx\n", ms->__exception_text_start);
	fprintf(fp, "  __exception_text_end: %lx\n", ms->__exception_text_end);
	fprintf(fp, "       panic_task_regs: %lx\n", (ulong)ms->panic_task_regs);
	fprintf(fp, "         PTE_PROT_NONE: %lx\n", ms->PTE_PROT_NONE);
	fprintf(fp, "              PTE_FILE: ");
	if (ms->PTE_FILE)
		fprintf(fp, "%lx\n", ms->PTE_FILE);
	else
		fprintf(fp, "(unused)\n");
        fprintf(fp, "       __SWP_TYPE_BITS: %ld\n", ms->__SWP_TYPE_BITS);
        fprintf(fp, "      __SWP_TYPE_SHIFT: %ld\n", ms->__SWP_TYPE_SHIFT);
        fprintf(fp, "       __SWP_TYPE_MASK: %lx\n", ms->__SWP_TYPE_MASK);
        fprintf(fp, "     __SWP_OFFSET_BITS: ");
	if (ms->__SWP_OFFSET_BITS)
        	fprintf(fp, "%ld\n", ms->__SWP_OFFSET_BITS);
	else
		fprintf(fp, "(unused)\n");
        fprintf(fp, "    __SWP_OFFSET_SHIFT: %ld\n", ms->__SWP_OFFSET_SHIFT);
	fprintf(fp, "     __SWP_OFFSET_MASK: ");
	if (ms->__SWP_OFFSET_MASK)
        	fprintf(fp, "%lx\n", ms->__SWP_OFFSET_MASK);
	else
		fprintf(fp, "(unused)\n");
	fprintf(fp, "     crash_kexec_start: %lx\n", ms->crash_kexec_start);
	fprintf(fp, "       crash_kexec_end: %lx\n", ms->crash_kexec_end);
	fprintf(fp, "  crash_save_cpu_start: %lx\n", ms->crash_save_cpu_start);
	fprintf(fp, "    crash_save_cpu_end: %lx\n", ms->crash_save_cpu_end);
}


/*
 * Parse machine dependent command line arguments.
 *
 * Force the phys_offset address via:
 *
 *  --machdep phys_offset=<address>
 */
static void
arm64_parse_cmdline_args(void)
{
	int index, i, c, err;
	char *arglist[MAXARGS];
	char buf[BUFSIZE];
	char *p;
	ulong value = 0;

	for (index = 0; index < MAX_MACHDEP_ARGS; index++) {
		if (!machdep->cmdline_args[index])
			break;

		if (!strstr(machdep->cmdline_args[index], "=")) {
			error(WARNING, "ignoring --machdep option: %x\n",
				machdep->cmdline_args[index]);
			continue;
		}

		strcpy(buf, machdep->cmdline_args[index]);

		for (p = buf; *p; p++) {
			if (*p == ',')
				*p = ' ';
		}

		c = parse_line(buf, arglist);

		for (i = 0; i < c; i++) {
			err = 0;

			if (STRNEQ(arglist[i], "phys_offset=")) {
				int megabytes = FALSE;
				int flags = RETURN_ON_ERROR | QUIET;

				if ((LASTCHAR(arglist[i]) == 'm') ||
				    (LASTCHAR(arglist[i]) == 'M')) {
					LASTCHAR(arglist[i]) = NULLCHAR;
					megabytes = TRUE;
				}

				p = arglist[i] + strlen("phys_offset=");
				if (strlen(p)) {
					if (megabytes)
						value = dtol(p, flags, &err);
					else
						value = htol(p, flags, &err);
				}

				if (!err) {
					if (megabytes)
						value = MEGABYTES(value);

					machdep->machspec->phys_offset = value;

					error(NOTE,
					    "setting phys_offset to: 0x%lx\n\n",
						machdep->machspec->phys_offset);

					machdep->flags |= PHYS_OFFSET;
					continue;
				}
			}

			error(WARNING, "ignoring --machdep option: %s\n",
				arglist[i]);
		}
	}
}


static void
arm64_calc_phys_offset(void)
{
	struct machine_specific *ms = machdep->machspec;
	ulong phys_offset;

	if (machdep->flags & PHYS_OFFSET) /* --machdep override */
		return;

	/*
	 * Next determine suitable value for phys_offset. User can override this
	 * by passing valid '--machdep phys_offset=<addr>' option.
	 */
	ms->phys_offset = 0;

	if (ACTIVE()) {
		char buf[BUFSIZE];
		char *p1;
		int errflag;
		FILE *fp;

		if ((fp = fopen("/proc/iomem", "r")) == NULL)
			return;

		/*
		 * Memory regions are sorted in ascending order. We take the
		 * first region which should be correct for most uses.
		 */
		errflag = 1;
		while (fgets(buf, BUFSIZE, fp)) {
			if (strstr(buf, ": System RAM")) {
				clean_line(buf);
				errflag = 0;
				break;
			}
		}
		fclose(fp);

		if (errflag)
			return;

		if (!(p1 = strstr(buf, "-")))
			return;

		*p1 = NULLCHAR;

		phys_offset = htol(buf, RETURN_ON_ERROR | QUIET, &errflag);
		if (errflag)
			return;

		ms->phys_offset = phys_offset;
	} else if (DISKDUMP_DUMPFILE() && diskdump_phys_base(&phys_offset)) {
		ms->phys_offset = phys_offset;
	} else if (KDUMP_DUMPFILE() && arm64_kdump_phys_base(&phys_offset)) {
		ms->phys_offset = phys_offset;
	} else {
		error(WARNING,
			"phys_offset cannot be determined from the dumpfile.\n");
		error(CONT,
			"Using default value of 0.  If this is not correct, then try\n");
		error(CONT,
			"using the command line option: --machdep phys_offset=<addr>\n");
	}

	if (CRASHDEBUG(1))
		fprintf(fp, "using %lx as phys_offset\n", ms->phys_offset);
}


/*
 *  Borrow the 32-bit ARM functionality.
 */
static int
arm64_kdump_phys_base(ulong *phys_offset)
{
	return arm_kdump_phys_base(phys_offset);
}

static void
arm64_init_kernel_pgd(void)
{
	int i;
	ulong value;

	if (!kernel_symbol_exists("init_mm") ||
	    !readmem(symbol_value("init_mm") + OFFSET(mm_struct_pgd), KVADDR,
	    &value, sizeof(void *), "init_mm.pgd", RETURN_ON_ERROR)) {
		if (kernel_symbol_exists("swapper_pg_dir"))
			value = symbol_value("swapper_pg_dir");
		else {
			error(WARNING, "cannot determine kernel pgd location\n");
			return;
		}
	}

        for (i = 0; i < NR_CPUS; i++)
                vt->kernel_pgd[i] = value;
}

static int
arm64_kvtop(struct task_context *tc, ulong kvaddr, physaddr_t *paddr, int verbose)
{
	ulong kernel_pgd;

	if (!IS_KVADDR(kvaddr))
		return FALSE;

	if (!vt->vmalloc_start) {
		*paddr = VTOP(kvaddr);
		return TRUE;
	}

	if (!IS_VMALLOC_ADDR(kvaddr)) {
		*paddr = VTOP(kvaddr);
		if (!verbose)
			return TRUE;
	}

	kernel_pgd = vt->kernel_pgd[0];
	*paddr = 0;

	switch (machdep->flags & (VM_L2_64K|VM_L3_4K))
	{
	case VM_L2_64K:
		return arm64_vtop_2level_64k(kernel_pgd, kvaddr, paddr, verbose);
	case VM_L3_4K:
		return arm64_vtop_3level_4k(kernel_pgd, kvaddr, paddr, verbose);
	default:
		return FALSE;
	}
}

static int
arm64_uvtop(struct task_context *tc, ulong uvaddr, physaddr_t *paddr, int verbose)
{
        ulong user_pgd;

        readmem(tc->mm_struct + OFFSET(mm_struct_pgd), KVADDR,
                &user_pgd, sizeof(long), "user pgd", FAULT_ON_ERROR);

	*paddr = 0;

	switch (machdep->flags & (VM_L2_64K|VM_L3_4K))
	{
	case VM_L2_64K:
		return arm64_vtop_2level_64k(user_pgd, uvaddr, paddr, verbose);
	case VM_L3_4K:
		return arm64_vtop_3level_4k(user_pgd, uvaddr, paddr, verbose);
	default:
		return FALSE;
	}
}

#define PMD_TYPE_MASK   3
#define PMD_TYPE_SECT   1
#define PMD_TYPE_TABLE  2
#define SECTION_PAGE_MASK_2MB    (~((MEGABYTES(2))-1))
#define SECTION_PAGE_MASK_512MB  (~((MEGABYTES(512))-1))

static int 
arm64_vtop_2level_64k(ulong pgd, ulong vaddr, physaddr_t *paddr, int verbose)
{
	ulong *pgd_base, *pgd_ptr, pgd_val;
	ulong *pte_base, *pte_ptr, pte_val;

        if (verbose)
                fprintf(fp, "PAGE DIRECTORY: %lx\n", pgd);

	pgd_base = (ulong *)pgd;
	FILL_PGD(pgd_base, KVADDR, PTRS_PER_PGD_L2_64K * sizeof(ulong));
	pgd_ptr = pgd_base + (((vaddr) >> PGDIR_SHIFT_L2_64K) & (PTRS_PER_PGD_L2_64K - 1));
        pgd_val = ULONG(machdep->pgd + PAGEOFFSET(pgd_ptr));
        if (verbose) 
                fprintf(fp, "   PGD: %lx => %lx\n", (ulong)pgd_ptr, pgd_val);
	if (!pgd_val)
		goto no_page;

	/* 
	 * #define __PAGETABLE_PUD_FOLDED 
	 * #define __PAGETABLE_PMD_FOLDED 
	 */

	if ((pgd_val & PMD_TYPE_MASK) == PMD_TYPE_SECT) {
		ulong sectionbase = pgd_val & SECTION_PAGE_MASK_512MB;
		if (verbose) {
			fprintf(fp, "  PAGE: %lx  (512MB)\n\n", sectionbase);
			arm64_translate_pte(pgd_val, 0, 0);
		}
		*paddr = sectionbase + (vaddr & ~SECTION_PAGE_MASK_512MB);
		return TRUE;
	}

	pte_base = (ulong *)PTOV(pgd_val & PHYS_MASK & (s32)machdep->pagemask);
	FILL_PTBL(pte_base, KVADDR, PTRS_PER_PTE_L2_64K * sizeof(ulong));
	pte_ptr = pte_base + (((vaddr) >> machdep->pageshift) & (PTRS_PER_PTE_L2_64K - 1));
        pte_val = ULONG(machdep->ptbl + PAGEOFFSET(pte_ptr));
        if (verbose) 
                fprintf(fp, "   PTE: %lx => %lx\n", (ulong)pte_ptr, pte_val);
	if (!pte_val)
		goto no_page;

	if (pte_val & PTE_VALID) {
		*paddr = (PAGEBASE(pte_val) & PHYS_MASK) + PAGEOFFSET(vaddr);
		if (verbose) {
			fprintf(fp, "  PAGE: %lx\n\n", PAGEBASE(*paddr));
			arm64_translate_pte(pte_val, 0, 0);
		}
	} else {
		if (IS_UVADDR(vaddr, NULL))
			*paddr = pte_val;
		if (verbose) {
			fprintf(fp, "\n");
			arm64_translate_pte(pte_val, 0, 0);
		}
		goto no_page;
	}

	return TRUE;
no_page:
	return FALSE;
}

static int 
arm64_vtop_3level_4k(ulong pgd, ulong vaddr, physaddr_t *paddr, int verbose)
{
	ulong *pgd_base, *pgd_ptr, pgd_val;
	ulong *pmd_base, *pmd_ptr, pmd_val;
	ulong *pte_base, *pte_ptr, pte_val;

        if (verbose)
                fprintf(fp, "PAGE DIRECTORY: %lx\n", pgd);

	pgd_base = (ulong *)pgd;
	FILL_PGD(pgd_base, KVADDR, PTRS_PER_PGD_L3_4K * sizeof(ulong));
	pgd_ptr = pgd_base + (((vaddr) >> PGDIR_SHIFT_L3_4K) & (PTRS_PER_PGD_L3_4K - 1));
        pgd_val = ULONG(machdep->pgd + PAGEOFFSET(pgd_ptr));
        if (verbose) 
                fprintf(fp, "   PGD: %lx => %lx\n", (ulong)pgd_ptr, pgd_val);
	if (!pgd_val)
		goto no_page;

	/* 
	 * #define __PAGETABLE_PUD_FOLDED 
	 */

	pmd_base = (ulong *)PTOV(pgd_val & PHYS_MASK & (s32)machdep->pagemask);
	FILL_PMD(pmd_base, KVADDR, PTRS_PER_PMD_L3_4K * sizeof(ulong));
	pmd_ptr = pmd_base + (((vaddr) >> PMD_SHIFT_L3_4K) & (PTRS_PER_PMD_L3_4K - 1));
        pmd_val = ULONG(machdep->pmd + PAGEOFFSET(pmd_ptr));
        if (verbose) 
                fprintf(fp, "   PMD: %lx => %lx\n", (ulong)pmd_ptr, pmd_val);
	if (!pmd_val)
		goto no_page;

	if ((pmd_val & PMD_TYPE_MASK) == PMD_TYPE_SECT) {
		ulong sectionbase = pmd_val & SECTION_PAGE_MASK_2MB;
		if (verbose) {
			fprintf(fp, "  PAGE: %lx  (2MB)\n\n", sectionbase);
			arm64_translate_pte(pmd_val, 0, 0);
		}
		*paddr = sectionbase + (vaddr & ~SECTION_PAGE_MASK_2MB);
		return TRUE;
	}

	pte_base = (ulong *)PTOV(pmd_val & PHYS_MASK & (s32)machdep->pagemask);
	FILL_PTBL(pte_base, KVADDR, PTRS_PER_PTE_L3_4K * sizeof(ulong));
	pte_ptr = pte_base + (((vaddr) >> machdep->pageshift) & (PTRS_PER_PTE_L3_4K - 1));
        pte_val = ULONG(machdep->ptbl + PAGEOFFSET(pte_ptr));
        if (verbose) 
                fprintf(fp, "   PTE: %lx => %lx\n", (ulong)pte_ptr, pte_val);
	if (!pte_val)
		goto no_page;

	if (pte_val & PTE_VALID) {
		*paddr = (PAGEBASE(pte_val) & PHYS_MASK) + PAGEOFFSET(vaddr);
		if (verbose) {
			fprintf(fp, "  PAGE: %lx\n\n", PAGEBASE(*paddr));
			arm64_translate_pte(pte_val, 0, 0);
		}
	} else {
		if (IS_UVADDR(vaddr, NULL))
			*paddr = pte_val;
		if (verbose) {
			fprintf(fp, "\n");
			arm64_translate_pte(pte_val, 0, 0);
		}
		goto no_page;
	}

	return TRUE;
no_page:
	return FALSE;
}

static ulong 
arm64_get_task_pgd(ulong task)
{
	struct task_context *tc;
	ulong pgd;

	if ((tc = task_to_context(task)) &&
	    readmem(tc->mm_struct + OFFSET(mm_struct_pgd), KVADDR,
	    &pgd, sizeof(long), "user pgd", RETURN_ON_ERROR))
		return pgd;
	else
		return NO_TASK;
}

static ulong 
arm64_processor_speed(void) 
{
	return 0;
};


/*
 *  Gather and verify all of the backtrace requirements.
 */
static void
arm64_stackframe_init(void)
{
	long task_struct_thread;
	long thread_struct_cpu_context;
	long context_sp, context_pc, context_fp;
	struct syment *sp1, *sp1n, *sp2, *sp2n;

	STRUCT_SIZE_INIT(note_buf, "note_buf_t");
	STRUCT_SIZE_INIT(elf_prstatus, "elf_prstatus");
	MEMBER_OFFSET_INIT(elf_prstatus_pr_pid, "elf_prstatus", "pr_pid");
	MEMBER_OFFSET_INIT(elf_prstatus_pr_reg, "elf_prstatus", "pr_reg");

	machdep->machspec->__exception_text_start = 
		symbol_value("__exception_text_start");
	machdep->machspec->__exception_text_end = 
		symbol_value("__exception_text_end");

	if ((sp1 = kernel_symbol_search("crash_kexec")) &&
	    (sp1n = next_symbol(NULL, sp1)) && 
	    (sp2 = kernel_symbol_search("crash_save_cpu")) &&
	    (sp2n = next_symbol(NULL, sp2))) {
		machdep->machspec->crash_kexec_start = sp1->value;
		machdep->machspec->crash_kexec_end = sp1n->value;
		machdep->machspec->crash_save_cpu_start = sp2->value;
		machdep->machspec->crash_save_cpu_end = sp2n->value;
		machdep->flags |= KDUMP_ENABLED;
	}

	task_struct_thread = MEMBER_OFFSET("task_struct", "thread");
	thread_struct_cpu_context = MEMBER_OFFSET("thread_struct", "cpu_context");

	if ((task_struct_thread == INVALID_OFFSET) ||
	    (thread_struct_cpu_context == INVALID_OFFSET)) {
		error(INFO, 
		    "cannot determine task_struct.thread.context offset\n");
		return;
	}

	/*
	 *  Pay for the convenience of using a hardcopy of a kernel structure.
	 */
	if (offsetof(struct arm64_stackframe, sp) != 
	    MEMBER_OFFSET("stackframe", "sp")) {
		error(INFO, "builtin stackframe.sp offset incorrect!\n");
		return;
	}
	if (offsetof(struct arm64_stackframe, fp) != 
	    MEMBER_OFFSET("stackframe", "fp")) {
		error(INFO, "builtin stackframe.fp offset incorrect!\n");
		return;
	}
	if (offsetof(struct arm64_stackframe, pc) != 
	    MEMBER_OFFSET("stackframe", "pc")) {
		error(INFO, "builtin stackframe.pc offset incorrect!\n");
		return;
	}

	context_sp = MEMBER_OFFSET("cpu_context", "sp");
	context_fp = MEMBER_OFFSET("cpu_context", "fp");
	context_pc = MEMBER_OFFSET("cpu_context", "pc");
	if (context_sp == INVALID_OFFSET) {
		error(INFO, "cannot determine cpu_context.sp offset\n");
		return;
	}
	if (context_fp == INVALID_OFFSET) {
		error(INFO, "cannot determine cpu_context.fp offset\n");
		return;
	}
	if (context_pc == INVALID_OFFSET) {
		error(INFO, "cannot determine cpu_context.pc offset\n");
		return;
	}
	ASSIGN_OFFSET(task_struct_thread_context_sp) =
		task_struct_thread + thread_struct_cpu_context + context_sp;
	ASSIGN_OFFSET(task_struct_thread_context_fp) =
		task_struct_thread + thread_struct_cpu_context + context_fp;
	ASSIGN_OFFSET(task_struct_thread_context_pc) =
		task_struct_thread + thread_struct_cpu_context + context_pc;
}

#define KERNEL_MODE (1)
#define USER_MODE   (2)

#define USER_EFRAME_OFFSET (304)

/*
 * PSR bits
 */
#define PSR_MODE_EL0t   0x00000000
#define PSR_MODE_EL1t   0x00000004
#define PSR_MODE_EL1h   0x00000005
#define PSR_MODE_EL2t   0x00000008
#define PSR_MODE_EL2h   0x00000009
#define PSR_MODE_EL3t   0x0000000c
#define PSR_MODE_EL3h   0x0000000d
#define PSR_MODE_MASK   0x0000000f

static int
arm64_is_kernel_exception_frame(struct bt_info *bt, ulong stkptr)
{
        struct arm64_pt_regs *regs;

        regs = (struct arm64_pt_regs *)&bt->stackbuf[(ulong)(STACK_OFFSET_TYPE(stkptr))];

	if (INSTACK(regs->sp, bt) && INSTACK(regs->regs[29], bt) && 
	    !(regs->pstate & (0xffffffff00000000ULL | PSR_MODE32_BIT)) &&
	    is_kernel_text(regs->pc) &&
	    is_kernel_text(regs->regs[30])) {
		switch (regs->pstate & PSR_MODE_MASK)
		{
		case PSR_MODE_EL1t:
		case PSR_MODE_EL1h:
			return TRUE;
		}
	}

	return FALSE;
}

static int 
arm64_eframe_search(struct bt_info *bt)
{
	ulong ptr, count;

	count = 0;
	for (ptr = bt->stackbase; ptr < bt->stacktop - SIZE(pt_regs); ptr++) {
		if (arm64_is_kernel_exception_frame(bt, ptr)) {
			fprintf(fp, "\nKERNEL-MODE EXCEPTION FRAME AT: %lx\n", ptr); 
			arm64_print_exception_frame(bt, ptr, KERNEL_MODE, fp);
			count++;
		}
	}

	if (is_kernel_thread(bt->tc->task))
		return count;

	ptr = bt->stacktop - USER_EFRAME_OFFSET;
	fprintf(fp, "%sUSER-MODE EXCEPTION FRAME AT: %lx\n", 
		count++ ? "\n" : "", ptr); 
	arm64_print_exception_frame(bt, ptr, USER_MODE, fp);

	return count;
}

static int
arm64_in_exception_text(ulong ptr)
{
	struct machine_specific *ms = machdep->machspec;

        return((ptr >= ms->__exception_text_start) &&
               (ptr < ms->__exception_text_end));
}

#define BACKTRACE_CONTINUE        (1)
#define BACKTRACE_COMPLETE_KERNEL (2)
#define BACKTRACE_COMPLETE_USER   (3)

static int 
arm64_print_stackframe_entry(struct bt_info *bt, int level, struct arm64_stackframe *frame, FILE *ofp)
{
	char *name, *name_plus_offset;
	ulong symbol_offset;
	struct syment *sp;
	struct load_module *lm;
	char buf[BUFSIZE];

        name = closest_symbol(frame->pc);
        name_plus_offset = NULL;

        if (bt->flags & BT_SYMBOL_OFFSET) {
                sp = value_search(frame->pc, &symbol_offset);
                if (sp && symbol_offset)
                        name_plus_offset =
                                value_to_symstr(frame->pc, buf, bt->radix);
        }

	if (bt->flags & BT_FULL) {
		arm64_display_full_frame(bt, frame->sp);
		bt->frameptr = frame->sp;
	}

        fprintf(ofp, "%s#%d [%8lx] %s at %lx", level < 10 ? " " : "", level,
                frame->sp, name_plus_offset ? name_plus_offset : name, frame->pc);

	if (BT_REFERENCE_CHECK(bt))
		arm64_do_bt_reference_check(bt, frame->pc, name);

	if (module_symbol(frame->pc, NULL, &lm, NULL, 0))
		fprintf(ofp, " [%s]", lm->mod_name);

	fprintf(ofp, "\n");

	if (bt->flags & BT_LINE_NUMBERS) {
		get_line_number(frame->pc, buf, FALSE);
		if (strlen(buf))
			fprintf(ofp, "    %s\n", buf);
	}

	if (STREQ(name, "start_kernel") || STREQ(name, "secondary_start_kernel") ||
	    STREQ(name, "kthread") || STREQ(name, "kthreadd"))
		return BACKTRACE_COMPLETE_KERNEL;

	return BACKTRACE_CONTINUE;
}

static void
arm64_display_full_frame(struct bt_info *bt, ulong sp)
{
	int i, u_idx;
	ulong *up;
	ulong words, addr;
	char buf[BUFSIZE];

	if (bt->frameptr == sp)
		return;

	if (!INSTACK(sp, bt) || !INSTACK(bt->frameptr, bt))
		return;

	words = (sp - bt->frameptr) / sizeof(ulong);

	addr = bt->frameptr;
	u_idx = (bt->frameptr - bt->stackbase)/sizeof(ulong);
	for (i = 0; i < words; i++, u_idx++) {
		if (!(i & 1)) 
			fprintf(fp, "%s    %lx: ", i ? "\n" : "", addr);

		up = (ulong *)(&bt->stackbuf[u_idx*sizeof(ulong)]);
		fprintf(fp, "%s ", format_stack_entry(bt, buf, *up, 0));

		addr += sizeof(ulong);
	}
	fprintf(fp, "\n");
}

static int 
arm64_unwind_frame(struct bt_info *bt, struct arm64_stackframe *frame)
{
	unsigned long high, low, fp;
	unsigned long stack_mask;
	
	stack_mask = (unsigned long)(ARM64_STACK_SIZE) - 1;
	fp = frame->fp;

	low  = frame->sp;
	high = (low + stack_mask) & ~(stack_mask);

	if (fp < low || fp > high || fp & 0xf)
		return FALSE;

	frame->sp = fp + 0x10;
	frame->fp = GET_STACK_ULONG(fp);
	frame->pc = GET_STACK_ULONG(fp + 8);

	return TRUE;
}

static void 
arm64_back_trace_cmd(struct bt_info *bt)
{
	struct arm64_stackframe stackframe;
	int level;
	ulong exception_frame;
	FILE *ofp;

	ofp = BT_REFERENCE_CHECK(bt) ? pc->nullfp : fp;

	/*
	 *  stackframes are created from 3 contiguous stack addresses:
	 *
	 *     x: contains stackframe.fp -- points to next triplet
	 *   x+8: contains stackframe.pc -- text return address
	 *  x+16: is the stackframe.sp address 
	 */

	if (bt->flags & BT_KDUMP_ADJUST) {
		stackframe.fp = GET_STACK_ULONG(bt->bptr - 8);
		stackframe.pc = GET_STACK_ULONG(bt->bptr);
		stackframe.sp = bt->bptr + 8;
	} else if (bt->hp && bt->hp->esp) {
		stackframe.fp = GET_STACK_ULONG(bt->hp->esp - 8);
		stackframe.pc = bt->hp->eip ? 
			bt->hp->eip : GET_STACK_ULONG(bt->hp->esp);
		stackframe.sp = bt->hp->esp + 8;
	} else {
		stackframe.sp = bt->stkptr;
		stackframe.pc = bt->instptr;
		stackframe.fp = bt->frameptr;
	}

	if (bt->flags & BT_TEXT_SYMBOLS) {
		arm64_print_text_symbols(bt, &stackframe, ofp);
                if (BT_REFERENCE_FOUND(bt)) {
                        print_task_header(fp, task_to_context(bt->task), 0);
			arm64_print_text_symbols(bt, &stackframe, fp);
                        fprintf(fp, "\n");
                }
		return;
        }

	if (!(bt->flags & BT_KDUMP_ADJUST)) {
		if (bt->flags & BT_USER_SPACE)
			goto complete_user;

		if (DUMPFILE() && is_task_active(bt->task)) {
			exception_frame = stackframe.fp - SIZE(pt_regs);
			if (arm64_is_kernel_exception_frame(bt, exception_frame))
				arm64_print_exception_frame(bt, exception_frame, 
					KERNEL_MODE, ofp);
		}
	}

	level = exception_frame = 0;
	while (1) {
		bt->instptr = stackframe.pc;

		switch (arm64_print_stackframe_entry(bt, level, &stackframe, ofp))
		{
		case BACKTRACE_COMPLETE_KERNEL:
			return;
		case BACKTRACE_COMPLETE_USER:
			goto complete_user;
		case BACKTRACE_CONTINUE:
			break;
		}

		if (exception_frame) {
			arm64_print_exception_frame(bt, exception_frame, KERNEL_MODE, ofp);
			exception_frame = 0;
		}

		if (!arm64_unwind_frame(bt, &stackframe))
			break;

		if (arm64_in_exception_text(bt->instptr) && INSTACK(stackframe.fp, bt))
			exception_frame = stackframe.fp - SIZE(pt_regs);

		level++;
	}

	if (is_kernel_thread(bt->tc->task)) 
		return;

complete_user:
	exception_frame = bt->stacktop - USER_EFRAME_OFFSET;
	arm64_print_exception_frame(bt, exception_frame, USER_MODE, ofp);
	if ((bt->flags & (BT_USER_SPACE|BT_KDUMP_ADJUST)) == BT_USER_SPACE)
		fprintf(ofp, " #0 [user space]\n");
}

static void
arm64_print_text_symbols(struct bt_info *bt, struct arm64_stackframe *frame, FILE *ofp)
{
	int i;
	ulong *up;
	struct load_module *lm;
	char buf1[BUFSIZE];
	char buf2[BUFSIZE];
	char *name;
	ulong start;

	if (bt->flags & BT_TEXT_SYMBOLS_ALL)
		start = bt->stackbase;
	else {
		start = frame->sp - 8;
		fprintf(ofp, "%sSTART: %s at %lx\n",
			space(VADDR_PRLEN > 8 ? 14 : 6),
			bt->flags & BT_SYMBOL_OFFSET ?
			value_to_symstr(frame->pc, buf2, bt->radix) :
			closest_symbol(frame->pc), frame->pc);
	}

	for (i = (start - bt->stackbase)/sizeof(ulong); i < LONGS_PER_STACK; i++) {
		up = (ulong *)(&bt->stackbuf[i*sizeof(ulong)]);
		if (is_kernel_text(*up)) {
			name = closest_symbol(*up);
			fprintf(ofp, "  %s[%s] %s at %lx",
				bt->flags & BT_ERROR_MASK ?
				"  " : "",
				mkstring(buf1, VADDR_PRLEN, 
				RJUST|LONG_HEX,
				MKSTR(bt->stackbase + 
				(i * sizeof(long)))),
				bt->flags & BT_SYMBOL_OFFSET ?
				value_to_symstr(*up, buf2, bt->radix) :
				name, *up);
			if (module_symbol(*up, NULL, &lm, NULL, 0))
				fprintf(ofp, " [%s]", lm->mod_name);
			fprintf(ofp, "\n");
			if (BT_REFERENCE_CHECK(bt))
				arm64_do_bt_reference_check(bt, *up, name);
		}
	}
}

static int
arm64_in_kdump_text(struct bt_info *bt, struct arm64_stackframe *frame)
{
	ulong *ptr, *start, *base;
	struct machine_specific *ms;

	if (!(machdep->flags & KDUMP_ENABLED))
		return FALSE;

	base = (ulong *)&bt->stackbuf[(ulong)(STACK_OFFSET_TYPE(bt->stackbase))];
	if (bt->flags & BT_USER_SPACE)
		start = (ulong *)&bt->stackbuf[(ulong)(STACK_OFFSET_TYPE(bt->stacktop))];
	else {
		if (INSTACK(frame->fp, bt))
			start = (ulong *)&bt->stackbuf[(ulong)(STACK_OFFSET_TYPE(frame->fp))];
		else 
			start = (ulong *)&bt->stackbuf[(ulong)(STACK_OFFSET_TYPE(bt->stacktop))];
	}

	ms = machdep->machspec;
	for (ptr = start - 8; ptr >= base; ptr--) {
		if ((*ptr >= ms->crash_kexec_start) && (*ptr < ms->crash_kexec_end)) {
			bt->bptr = ((ulong)ptr - (ulong)base) + bt->tc->thread_info;
			if (CRASHDEBUG(1))
				fprintf(fp, "%lx: %lx (crash_kexec)\n", bt->bptr, *ptr);
			return TRUE;
		}
		if ((*ptr >= ms->crash_save_cpu_start) && (*ptr < ms->crash_save_cpu_end)) {
			bt->bptr = ((ulong)ptr - (ulong)base) + bt->tc->thread_info;
			if (CRASHDEBUG(1))
				fprintf(fp, "%lx: %lx (crash_save_cpu)\n", bt->bptr, *ptr);
			return TRUE;
		}
	} 

	return FALSE;
}

static int
arm64_get_dumpfile_stackframe(struct bt_info *bt, struct arm64_stackframe *frame)
{
	struct machine_specific *ms = machdep->machspec;
	struct arm64_pt_regs *ptregs;

	if (!ms->panic_task_regs)
		return FALSE;

	ptregs = &ms->panic_task_regs[bt->tc->processor];
	frame->sp = ptregs->sp;
	frame->pc = ptregs->pc;
	frame->fp = ptregs->regs[29];

	if (!is_kernel_text(frame->pc) && 
	    in_user_stack(bt->tc->task, frame->sp))
		bt->flags |= BT_USER_SPACE;

	if (arm64_in_kdump_text(bt, frame))
		bt->flags |= BT_KDUMP_ADJUST;

	return TRUE;
}

static int
arm64_get_stackframe(struct bt_info *bt, struct arm64_stackframe *frame) 
{
	if (!fill_task_struct(bt->task))
		return FALSE;

	frame->sp = ULONG(tt->task_struct + OFFSET(task_struct_thread_context_sp));
	frame->pc = ULONG(tt->task_struct + OFFSET(task_struct_thread_context_pc));
	frame->fp = ULONG(tt->task_struct + OFFSET(task_struct_thread_context_fp));

	return TRUE;
}

static void
arm64_get_stack_frame(struct bt_info *bt, ulong *pcp, ulong *spp)
{
	int ret;
	struct arm64_stackframe stackframe;

	if (DUMPFILE() && is_task_active(bt->task))
		ret = arm64_get_dumpfile_stackframe(bt, &stackframe);
	else
		ret = arm64_get_stackframe(bt, &stackframe);

	if (!ret) {
		error(WARNING, 
			"cannot determine starting stack frame for task %lx\n",
				bt->task);
		return;
	}

	bt->frameptr = stackframe.fp;
	if (pcp)
		*pcp = stackframe.pc;
	if (spp)
		*spp = stackframe.sp;
}

static void
arm64_print_exception_frame(struct bt_info *bt, ulong pt_regs, int mode, FILE *ofp)
{
	int i, r, rows, top_reg, is_64_bit;
	struct arm64_pt_regs *regs;
	struct syment *sp;
	ulong LR, SP, offset;
	char buf[BUFSIZE];

	if (CRASHDEBUG(1))
		fprintf(ofp, "pt_regs: %lx\n", pt_regs);

	regs = (struct arm64_pt_regs *)&bt->stackbuf[(ulong)(STACK_OFFSET_TYPE(pt_regs))];

	if ((mode == USER_MODE) && (regs->pstate & PSR_MODE32_BIT)) {
		LR = regs->regs[14];
		SP = regs->regs[13];
		top_reg = 12;
		is_64_bit = FALSE;
		rows = 4;
	} else {
		LR = regs->regs[30];
		SP = regs->sp;
		top_reg = 29;
		is_64_bit = TRUE;
		rows = 3;
	}

	switch (mode) {
	case USER_MODE: 
		if (is_64_bit)
			fprintf(ofp, 
			    "     PC: %016lx   LR: %016lx   SP: %016lx\n    ",
				(ulong)regs->pc, LR, SP);
		else
			fprintf(ofp, 
			    "     PC: %08lx  LR: %08lx  SP: %08lx  PSTATE: %08lx\n    ",
				(ulong)regs->pc, LR, SP, (ulong)regs->pstate);
		break;

	case KERNEL_MODE:
		fprintf(ofp, "     PC: %016lx  ", (ulong)regs->pc);
		if (is_kernel_text(regs->pc) &&
		    (sp = value_search(regs->pc, &offset))) {
			fprintf(ofp, "[%s", sp->name);
			if (offset)
				fprintf(ofp, (*gdb_output_radix == 16) ?
				    "+0x%lx" : "+%ld", 
					offset);
			fprintf(ofp, "]\n");
		} else
			fprintf(ofp, "[unknown or invalid address]\n");

		fprintf(ofp, "     LR: %016lx  ", LR);
		if (is_kernel_text(LR) &&
		    (sp = value_search(LR, &offset))) {
			fprintf(ofp, "[%s", sp->name);
			if (offset)
				fprintf(ofp, (*gdb_output_radix == 16) ?
				    "+0x%lx" : "+%ld", 
					offset);
			fprintf(ofp, "]\n");
		} else
			fprintf(ofp, "[unknown or invalid address]\n");

		fprintf(ofp, "     SP: %016lx  PSTATE: %08lx\n    ", 
			SP, (ulong)regs->pstate);
		break;
	}

	for (i = top_reg, r = 1; i >= 0; r++, i--) {
		fprintf(ofp, "%sX%d: ", 
			i < 10 ? " " : "", i);
		fprintf(ofp, is_64_bit ? "%016lx" : "%08lx",
			(ulong)regs->regs[i]);
		if ((i == 0) || ((r % rows) == 0))
			fprintf(ofp, "\n    ");
		else
			fprintf(ofp, "%s", is_64_bit ? "  " : " "); 
	}

	if (is_64_bit) {
		fprintf(ofp, "ORIG_X0: %016lx  SYSCALLNO: %lx",
			(ulong)regs->orig_x0, (ulong)regs->syscallno);
		if (mode == USER_MODE)
			fprintf(ofp, "  PSTATE: %08lx", (ulong)regs->pstate);
		fprintf(ofp, "\n");
	}

	if (is_kernel_text(regs->pc) && (bt->flags & BT_LINE_NUMBERS)) {
		get_line_number(regs->pc, buf, FALSE);
		if (strlen(buf))
			fprintf(ofp, "    %s\n", buf);
	}

	if (BT_REFERENCE_CHECK(bt)) {
		arm64_do_bt_reference_check(bt, regs->pc, NULL);
		arm64_do_bt_reference_check(bt, LR, NULL);
		arm64_do_bt_reference_check(bt, SP, NULL);
		arm64_do_bt_reference_check(bt, regs->pstate, NULL);
		for (i = 0; i <= top_reg; i++)
			arm64_do_bt_reference_check(bt, regs->regs[i], NULL);
		if (is_64_bit) {
			arm64_do_bt_reference_check(bt, regs->orig_x0, NULL);
			arm64_do_bt_reference_check(bt, regs->syscallno, NULL);
		}
	}
}

/*
 *  Check a frame for a requested reference.
 */
static void
arm64_do_bt_reference_check(struct bt_info *bt, ulong text, char *name)
{
	ulong offset;
	struct syment *sp = NULL;

	if (!name)
		sp = value_search(text, &offset); 
	else if (!text)
		sp = symbol_search(name);

        switch (bt->ref->cmdflags & (BT_REF_SYMBOL|BT_REF_HEXVAL))
        {
        case BT_REF_SYMBOL:
                if (name) {
			if (STREQ(name, bt->ref->str))
                        	bt->ref->cmdflags |= BT_REF_FOUND;
		} else {
			if (sp && !offset && STREQ(sp->name, bt->ref->str))
                        	bt->ref->cmdflags |= BT_REF_FOUND;
		}
                break;

        case BT_REF_HEXVAL:
                if (text) {
			if (bt->ref->hexval == text) 
                        	bt->ref->cmdflags |= BT_REF_FOUND;
		} else if (sp && (bt->ref->hexval == sp->value))
                       	bt->ref->cmdflags |= BT_REF_FOUND;
		else if (!name && !text && (bt->ref->hexval == 0))
			bt->ref->cmdflags |= BT_REF_FOUND;
                break;
        }
}

/*
 *  Translate a PTE, returning TRUE if the page is present.
 *  If a physaddr pointer is passed in, don't print anything.
 */
static int
arm64_translate_pte(ulong pte, void *physaddr, ulonglong unused)
{
	int c, others, len1, len2, len3;
	ulong paddr;
	char buf1[BUFSIZE];
        char buf2[BUFSIZE];
        char buf3[BUFSIZE];
	char ptebuf[BUFSIZE];
	char physbuf[BUFSIZE];
        char *arglist[MAXARGS];
	int page_present;

	paddr = pte & PHYS_MASK & (s32)machdep->pagemask;
       	page_present = pte & (PTE_VALID | machdep->machspec->PTE_PROT_NONE);

        if (physaddr) {
		*((ulong *)physaddr) = paddr;
		return page_present;
	}
        
	sprintf(ptebuf, "%lx", pte);
	len1 = MAX(strlen(ptebuf), strlen("PTE"));
	fprintf(fp, "%s  ", mkstring(buf1, len1, CENTER|LJUST, "PTE"));

        if (!page_present) { 
                swap_location(pte, buf1);
                if ((c = parse_line(buf1, arglist)) != 3)
                        error(FATAL, "cannot determine swap location\n");

                len2 = MAX(strlen(arglist[0]), strlen("SWAP"));
                len3 = MAX(strlen(arglist[2]), strlen("OFFSET"));

                fprintf(fp, "%s  %s\n",
                        mkstring(buf2, len2, CENTER|LJUST, "SWAP"),
                        mkstring(buf3, len3, CENTER|LJUST, "OFFSET"));

                strcpy(buf2, arglist[0]);
                strcpy(buf3, arglist[2]);
                fprintf(fp, "%s  %s  %s\n",
                        mkstring(ptebuf, len1, CENTER|RJUST, NULL),
                        mkstring(buf2, len2, CENTER|RJUST, NULL),
                        mkstring(buf3, len3, CENTER|RJUST, NULL));
                return page_present;
        }

        sprintf(physbuf, "%lx", paddr);
        len2 = MAX(strlen(physbuf), strlen("PHYSICAL"));
        fprintf(fp, "%s  ", mkstring(buf1, len2, CENTER|LJUST, "PHYSICAL"));

        fprintf(fp, "FLAGS\n");

        fprintf(fp, "%s  %s  ",
                mkstring(ptebuf, len1, CENTER|RJUST, NULL),
                mkstring(physbuf, len2, CENTER|RJUST, NULL));
        fprintf(fp, "(");
        others = 0;

	if (pte) {
		if (pte & PTE_VALID)
			fprintf(fp, "%sVALID", others++ ? "|" : "");
		if (pte & machdep->machspec->PTE_FILE)
			fprintf(fp, "%sFILE", others++ ? "|" : "");
		if (pte & machdep->machspec->PTE_PROT_NONE)
			fprintf(fp, "%sPROT_NONE", others++ ? "|" : "");
		if (pte & PTE_USER)
			fprintf(fp, "%sUSER", others++ ? "|" : "");
		if (pte & PTE_RDONLY)
			fprintf(fp, "%sRDONLY", others++ ? "|" : "");
		if (pte & PTE_SHARED)
			fprintf(fp, "%sSHARED", others++ ? "|" : "");
		if (pte & PTE_AF)
			fprintf(fp, "%sAF", others++ ? "|" : "");
		if (pte & PTE_NG)
			fprintf(fp, "%sNG", others++ ? "|" : "");
		if (pte & PTE_PXN)
			fprintf(fp, "%sPXN", others++ ? "|" : "");
		if (pte & PTE_UXN)
			fprintf(fp, "%sUXN", others++ ? "|" : "");
		if (pte & PTE_DIRTY)
			fprintf(fp, "%sDIRTY", others++ ? "|" : "");
		if (pte & PTE_SPECIAL)
			fprintf(fp, "%sSPECIAL", others++ ? "|" : "");
	} else {
                fprintf(fp, "no mapping");
        }

        fprintf(fp, ")\n");

	return (page_present);
}

static ulong
arm64_vmalloc_start(void)
{
	return machdep->machspec->vmalloc_start_addr;
}

/*
 *  Not so accurate since thread_info introduction.
 */
static int
arm64_is_task_addr(ulong task)
{
	if (tt->flags & THREAD_INFO)
		return IS_KVADDR(task);
	else
		return (IS_KVADDR(task) && (ALIGNED_STACK_OFFSET(task) == 0));
}

/*
 * Filter dissassembly output if the output radix is not gdb's default 10
 */
static int
arm64_dis_filter(ulong vaddr, char *inbuf, unsigned int output_radix)
{
	char buf1[BUFSIZE];
	char buf2[BUFSIZE];
	char *colon, *p1;
	int argc;
	char *argv[MAXARGS];
	ulong value;

	if (!inbuf)
		return TRUE;

	console("IN: %s", inbuf);

	colon = strstr(inbuf, ":");

	if (colon) {
		sprintf(buf1, "0x%lx <%s>", vaddr,
			value_to_symstr(vaddr, buf2, output_radix));
		sprintf(buf2, "%s%s", buf1, colon);
		strcpy(inbuf, buf2);
	}

	strcpy(buf1, inbuf);
	argc = parse_line(buf1, argv);

	if ((FIRSTCHAR(argv[argc-1]) == '<') &&
	    (LASTCHAR(argv[argc-1]) == '>')) {
		p1 = rindex(inbuf, '<');
		while ((p1 > inbuf) && !STRNEQ(p1, " 0x"))
			p1--;

		if (!STRNEQ(p1, " 0x"))
			return FALSE;
		p1++;

		if (!extract_hex(p1, &value, NULLCHAR, TRUE))
			return FALSE;

		sprintf(buf1, "0x%lx <%s>\n", value,
			value_to_symstr(value, buf2, output_radix));

		sprintf(p1, "%s", buf1);
	}

	console("    %s", inbuf);

	return TRUE;
}

/*
 * Machine dependent command.
 */
static void
arm64_cmd_mach(void)
{
	int c;

	while ((c = getopt(argcnt, args, "cm")) != -1) {
		switch (c) {
		case 'c':
		case 'm':
			option_not_supported(c);
			break;

		default:
			argerrs++;
			break;
		}
	}

	if (argerrs)
		cmd_usage(pc->curcmd, SYNOPSIS);

	arm64_display_machine_stats();
}

static void
arm64_display_machine_stats(void)
{
	struct new_utsname *uts;
	char buf[BUFSIZE];
	ulong mhz;

	uts = &kt->utsname;

	fprintf(fp, "       MACHINE TYPE: %s\n", uts->machine);
	fprintf(fp, "        MEMORY SIZE: %s\n", get_memory_size(buf));
	fprintf(fp, "               CPUS: %d\n", get_cpus_to_display());
	if ((mhz = machdep->processor_speed()))
		fprintf(fp, "    PROCESSOR SPEED: %ld Mhz\n", mhz);
	fprintf(fp, "                 HZ: %d\n", machdep->hz);
	fprintf(fp, "          PAGE SIZE: %d\n", PAGESIZE());
	fprintf(fp, "KERNEL VIRTUAL BASE: %lx\n", machdep->machspec->page_offset);
	fprintf(fp, "KERNEL VMALLOC BASE: %lx\n", machdep->machspec->vmalloc_start_addr);
	fprintf(fp, "KERNEL MODULES BASE: %lx\n", machdep->machspec->modules_vaddr);
        fprintf(fp, "KERNEL VMEMMAP BASE: %lx\n", machdep->machspec->vmemmap_vaddr);
	fprintf(fp, "  KERNEL STACK SIZE: %ld\n", STACKSIZE());
}

static int
arm64_get_smp_cpus(void)
{
	int cpus;
	
	if ((cpus = get_cpus_present()))
		return cpus;
	else
		return MAX(get_cpus_online(), get_highest_cpu_online()+1);
}


/*
 * Retrieve task registers for the time of the crash.
 */
static int
arm64_get_crash_notes(void)
{
	struct machine_specific *ms = machdep->machspec;
	ulong crash_notes;
	Elf64_Nhdr *note;
	ulong offset;
	char *buf, *p;
	ulong *notes_ptrs;
	ulong i;

	if (!symbol_exists("crash_notes"))
		return FALSE;

	crash_notes = symbol_value("crash_notes");

	notes_ptrs = (ulong *)GETBUF(kt->cpus*sizeof(notes_ptrs[0]));

	/*
	 * Read crash_notes for the first CPU. crash_notes are in standard ELF
	 * note format.
	 */
	if (!readmem(crash_notes, KVADDR, &notes_ptrs[kt->cpus-1], 
	    sizeof(notes_ptrs[kt->cpus-1]), "crash_notes", RETURN_ON_ERROR)) {
		error(WARNING, "cannot read crash_notes\n");
		FREEBUF(notes_ptrs);
		return FALSE;
	}

	if (symbol_exists("__per_cpu_offset")) {
		/* 
		 * Add __per_cpu_offset for each cpu to form the notes pointer.
		 */
		for (i = 0; i<kt->cpus; i++)
			notes_ptrs[i] = notes_ptrs[kt->cpus-1] + kt->__per_cpu_offset[i];	
	}

	buf = GETBUF(SIZE(note_buf));

	if (!(ms->panic_task_regs = malloc(kt->cpus * sizeof(struct arm64_pt_regs))))
		error(FATAL, "cannot malloc panic_task_regs space\n");
	
	for  (i = 0; i < kt->cpus; i++) {

		if (!readmem(notes_ptrs[i], KVADDR, buf, SIZE(note_buf), 
		    "note_buf_t", RETURN_ON_ERROR)) {
			error(WARNING, "failed to read note_buf_t\n");
			goto fail;
		}

		/*
		 * Do some sanity checks for this note before reading registers from it.
		 */
		note = (Elf64_Nhdr *)buf;
		p = buf + sizeof(Elf64_Nhdr);

		if (note->n_type != NT_PRSTATUS) {
			error(WARNING, "invalid note (n_type != NT_PRSTATUS)\n");
			goto fail;
		}
		if (p[0] != 'C' || p[1] != 'O' || p[2] != 'R' || p[3] != 'E') {
			error(WARNING, "invalid note (name != \"CORE\"\n");
			goto fail;
		}

		/*
		 * Find correct location of note data. This contains elf_prstatus
		 * structure which has registers etc. for the crashed task.
		 */
		offset = sizeof(Elf64_Nhdr);
		offset = roundup(offset + note->n_namesz, 4);
		p = buf + offset; /* start of elf_prstatus */

		BCOPY(p + OFFSET(elf_prstatus_pr_reg), &ms->panic_task_regs[i],
		      sizeof(struct arm64_pt_regs));
	}

	FREEBUF(buf);
	FREEBUF(notes_ptrs);
	return TRUE;

fail:
	FREEBUF(buf);
	FREEBUF(notes_ptrs);
	free(ms->panic_task_regs);
	ms->panic_task_regs = NULL;
	return FALSE;
}

static void
arm64_clear_machdep_cache(void) {
	/*
	 * TBD: probably not necessary...
	 */
	return;
}

static int
arm64_in_alternate_stack(int cpu, ulong stkptr)
{
	NOT_IMPLEMENTED(INFO);
	return FALSE;
}


static int
compare_kvaddr(const void *v1, const void *v2)
{
        struct vaddr_range *r1, *r2;

        r1 = (struct vaddr_range *)v1;
        r2 = (struct vaddr_range *)v2;

        return (r1->start < r2->start ? -1 :
                r1->start == r2->start ? 0 : 1);
}

static int
arm64_get_kvaddr_ranges(struct vaddr_range *vrp)
{
	int cnt;

	cnt = 0;

	vrp[cnt].type = KVADDR_UNITY_MAP;
	vrp[cnt].start = machdep->machspec->page_offset;
	vrp[cnt++].end = vt->high_memory;

	vrp[cnt].type = KVADDR_VMALLOC;
	vrp[cnt].start = machdep->machspec->vmalloc_start_addr;
	vrp[cnt++].end = last_vmalloc_address();

	if (st->mods_installed) {
		vrp[cnt].type = KVADDR_MODULES;
		vrp[cnt].start = lowest_module_address();
		vrp[cnt++].end = roundup(highest_module_address(), 
			PAGESIZE());
	}

	if (machdep->flags & VMEMMAP) {
		vrp[cnt].type = KVADDR_VMEMMAP;
		vrp[cnt].start = machdep->machspec->vmemmap_vaddr;
		vrp[cnt++].end = vt->node_table[vt->numnodes-1].mem_map +
			(vt->node_table[vt->numnodes-1].size * SIZE(page));
	}

	qsort(vrp, cnt, sizeof(struct vaddr_range), compare_kvaddr);

	return cnt;
}

/*
 *  Include both vmalloc'd, module and vmemmap address space as VMALLOC space.
 */
int
arm64_IS_VMALLOC_ADDR(ulong vaddr)
{
	struct machine_specific *ms = machdep->machspec;
	
        return ((vaddr >= ms->vmalloc_start_addr && vaddr <= ms->vmalloc_end) ||
                ((machdep->flags & VMEMMAP) &&
                 (vaddr >= ms->vmemmap_vaddr && vaddr <= ms->vmemmap_end)) ||
                (vaddr >= ms->modules_vaddr && vaddr <= ms->modules_end));
}

static void 
arm64_calc_VA_BITS(void)
{
	int bitval;
	struct syment *sp;
	ulong value;

	if (!(sp = symbol_search("swapper_pg_dir")) &&
	    !(sp = symbol_search("idmap_pg_dir")) &&
	    !(sp = symbol_search("_text")) &&
	    !(sp = symbol_search("stext"))) { 
		for (sp = st->symtable; sp < st->symend; sp++) {
			if (highest_bit_long(sp->value) == 63)
				break;
		}
	}

	if (sp) 
		value = sp->value;
	else
		value = kt->vmcoreinfo.log_buf_SYMBOL;  /* crash --log */

	for (bitval = highest_bit_long(value); bitval; bitval--) {
		if ((value & (1UL << bitval)) == 0) {
			machdep->machspec->VA_BITS = bitval + 2;
			break;
		}
	}

	if (CRASHDEBUG(1))
		fprintf(fp, "VA_BITS: %ld\n", machdep->machspec->VA_BITS);

}

/*
 *  The size and end of the vmalloc range is dependent upon the kernel's
 *  VMEMMAP_SIZE value, and the vmemmap range is dependent upon the end
 *  of the vmalloc range as well as the VMEMMAP_SIZE:
 *
 *  #define VMEMMAP_SIZE    ALIGN((1UL << (VA_BITS - PAGE_SHIFT)) * sizeof(struct page), PUD_SIZE)
 *  #define VMALLOC_START   (UL(0xffffffffffffffff) << VA_BITS)
 *  #define VMALLOC_END     (PAGE_OFFSET - PUD_SIZE - VMEMMAP_SIZE - SZ_64K)
 *
 *  Since VMEMMAP_SIZE is dependent upon the size of a struct page,
 *  the two ranges cannot be determined until POST_GDB.
 */

#define ALIGN(x, a) __ALIGN_KERNEL((x), (a))
#define __ALIGN_KERNEL(x, a)            __ALIGN_KERNEL_MASK(x, (typeof(x))(a) - 1)
#define __ALIGN_KERNEL_MASK(x, mask)    (((x) + (mask)) & ~(mask))
#define SZ_64K                          0x00010000

static void
arm64_calc_virtual_memory_ranges(void)
{
	struct machine_specific *ms = machdep->machspec;
	ulong vmemmap_start, vmemmap_end, vmemmap_size;
	ulong vmalloc_end;
	ulong PUD_SIZE = UNINITIALIZED;

	if (THIS_KERNEL_VERSION < LINUX(3,17,0))  /* use original hardwired values */
		return;

	STRUCT_SIZE_INIT(page, "page");

        switch (machdep->flags & (VM_L2_64K|VM_L3_4K))
        {
        case VM_L2_64K:
		PUD_SIZE = PGDIR_SIZE_L2_64K;
		break;
        case VM_L3_4K:
		PUD_SIZE = PGDIR_SIZE_L3_4K;
		break;
        }

	vmemmap_size = ALIGN((1UL << (ms->VA_BITS - machdep->pageshift)) * SIZE(page), PUD_SIZE);
	vmalloc_end = (ms->page_offset - PUD_SIZE - vmemmap_size - SZ_64K);
	vmemmap_start = vmalloc_end + SZ_64K;
	vmemmap_end = vmemmap_start + vmemmap_size;

	ms->vmalloc_end = vmalloc_end - 1;
	ms->vmemmap_vaddr = vmemmap_start;
	ms->vmemmap_end = vmemmap_end - 1;
}

static int
arm64_is_uvaddr(ulong addr, struct task_context *tc)
{
        return (addr < ARM64_USERSPACE_TOP);
}


ulong
arm64_swp_type(ulong pte)
{
	struct machine_specific *ms = machdep->machspec;

	pte >>= ms->__SWP_TYPE_SHIFT;
	pte &= ms->__SWP_TYPE_MASK;
	return pte;
}

ulong
arm64_swp_offset(ulong pte)
{
	struct machine_specific *ms = machdep->machspec;

	pte >>= ms->__SWP_OFFSET_SHIFT;
	if (ms->__SWP_OFFSET_MASK)
		pte &= ms->__SWP_OFFSET_MASK;
	return pte;
}

#endif  /* ARM64 */


