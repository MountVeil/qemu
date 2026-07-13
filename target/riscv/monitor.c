/*
 * QEMU monitor for RISC-V
 *
 * Copyright (c) 2019 Bin Meng <bmeng.cn@gmail.com>
 *
 * RISC-V specific monitor commands implementation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "cpu_bits.h"
#include "monitor/monitor.h"
#include "monitor/hmp-target.h"
#include "smmpt.h"

#ifdef TARGET_RISCV64
#define PTE_HEADER_FIELDS       "vaddr            paddr            "\
                                "size             attr\n"
#define PTE_HEADER_DELIMITER    "---------------- ---------------- "\
                                "---------------- -------\n"
#else
#define PTE_HEADER_FIELDS       "vaddr    paddr            size     attr\n"
#define PTE_HEADER_DELIMITER    "-------- ---------------- -------- -------\n"
#endif

/* Perform linear address sign extension */
static target_ulong addr_canonical(int va_bits, target_ulong addr)
{
#ifdef TARGET_RISCV64
    if (addr & (1UL << (va_bits - 1))) {
        addr |= (hwaddr)-(1L << va_bits);
    }
#endif

    return addr;
}

static void print_pte_header(Monitor *mon)
{
    monitor_printf(mon, PTE_HEADER_FIELDS);
    monitor_printf(mon, PTE_HEADER_DELIMITER);
}

static void print_pte(Monitor *mon, int va_bits, target_ulong vaddr,
                      hwaddr paddr, target_ulong size, int attr)
{
    /* sanity check on vaddr */
    if (vaddr >= (1UL << va_bits)) {
        return;
    }

    if (!size) {
        return;
    }

    monitor_printf(mon, TARGET_FMT_lx " " HWADDR_FMT_plx " " TARGET_FMT_lx
                   " %c%c%c%c%c%c%c\n",
                   addr_canonical(va_bits, vaddr),
                   paddr, size,
                   attr & PTE_R ? 'r' : '-',
                   attr & PTE_W ? 'w' : '-',
                   attr & PTE_X ? 'x' : '-',
                   attr & PTE_U ? 'u' : '-',
                   attr & PTE_G ? 'g' : '-',
                   attr & PTE_A ? 'a' : '-',
                   attr & PTE_D ? 'd' : '-');
}

static void walk_pte(Monitor *mon, hwaddr base, target_ulong start,
                     int level, int ptidxbits, int ptesize, int va_bits,
                     target_ulong *vbase, hwaddr *pbase, hwaddr *last_paddr,
                     target_ulong *last_size, int *last_attr)
{
    hwaddr pte_addr;
    hwaddr paddr;
    target_ulong last_start = -1;
    target_ulong pgsize;
    target_ulong pte;
    int ptshift;
    int attr;
    int idx;

    if (level < 0) {
        return;
    }

    ptshift = level * ptidxbits;
    pgsize = 1UL << (PGSHIFT + ptshift);

    for (idx = 0; idx < (1UL << ptidxbits); idx++) {
        pte_addr = base + idx * ptesize;
        cpu_physical_memory_read(pte_addr, &pte, ptesize);

        paddr = (hwaddr)(pte >> PTE_PPN_SHIFT) << PGSHIFT;
        attr = pte & 0xff;

        /* PTE has to be valid */
        if (attr & PTE_V) {
            if (attr & (PTE_R | PTE_W | PTE_X)) {
                /*
                 * A leaf PTE has been found
                 *
                 * If current PTE's permission bits differ from the last one,
                 * or the current PTE breaks up a contiguous virtual or
                 * physical mapping, address block together with the last one,
                 * print out the last contiguous mapped block details.
                 */
                if ((*last_attr != attr) ||
                    (*last_paddr + *last_size != paddr) ||
                    (last_start + *last_size != start)) {
                    print_pte(mon, va_bits, *vbase, *pbase,
                              *last_paddr + *last_size - *pbase, *last_attr);

                    *vbase = start;
                    *pbase = paddr;
                    *last_attr = attr;
                }

                last_start = start;
                *last_paddr = paddr;
                *last_size = pgsize;
            } else {
                /* pointer to the next level of the page table */
                walk_pte(mon, paddr, start, level - 1, ptidxbits, ptesize,
                         va_bits, vbase, pbase, last_paddr,
                         last_size, last_attr);
            }
        }

        start += pgsize;
    }

}

static void mem_info_svxx(Monitor *mon, CPUArchState *env)
{
    int levels, ptidxbits, ptesize, vm, va_bits;
    hwaddr base;
    target_ulong vbase;
    hwaddr pbase;
    hwaddr last_paddr;
    target_ulong last_size;
    int last_attr;

    if (riscv_cpu_mxl(env) == MXL_RV32) {
        base = (hwaddr)get_field(env->satp, SATP32_PPN) << PGSHIFT;
        vm = get_field(env->satp, SATP32_MODE);
    } else {
        base = (hwaddr)get_field(env->satp, SATP64_PPN) << PGSHIFT;
        vm = get_field(env->satp, SATP64_MODE);
    }

    switch (vm) {
    case VM_1_10_SV32:
        levels = 2;
        ptidxbits = 10;
        ptesize = 4;
        break;
    case VM_1_10_SV39:
        levels = 3;
        ptidxbits = 9;
        ptesize = 8;
        break;
    case VM_1_10_SV48:
        levels = 4;
        ptidxbits = 9;
        ptesize = 8;
        break;
    case VM_1_10_SV57:
        levels = 5;
        ptidxbits = 9;
        ptesize = 8;
        break;
    default:
        g_assert_not_reached();
    }

    /* calculate virtual address bits */
    va_bits = PGSHIFT + levels * ptidxbits;

    /* print header */
    print_pte_header(mon);

    vbase = -1;
    pbase = -1;
    last_paddr = -1;
    last_size = 0;
    last_attr = 0;

    /* walk page tables, starting from address 0 */
    walk_pte(mon, base, 0, levels - 1, ptidxbits, ptesize, va_bits,
             &vbase, &pbase, &last_paddr, &last_size, &last_attr);

    /* don't forget the last one */
    print_pte(mon, va_bits, vbase, pbase,
              last_paddr + last_size - pbase, last_attr);
}


void hmp_info_smmpt(Monitor *mon, const QDict *qdict)
{
    CPUArchState *env;
    RISCVSMMPTConfig config;
    RISCVSMMPTResult result;
    double lookups_per_tlb;
    double entries_per_tlb;
    double entries_per_lookup;

    env = mon_get_cpu_env(mon);
    if (!env) {
        monitor_printf(mon, "No CPU available\n");
        return;
    }

    result = riscv_smmpt_decode_config(env, &config);

    monitor_printf(mon, "SmMPT configuration\n");
    monitor_printf(mon, "  policy:           %s (%u)\n",
                   riscv_smmpt_policy_name(env->smmpt_policy),
                   (unsigned int)env->smmpt_policy);
    monitor_printf(mon, "  mmpt:             0x" TARGET_FMT_lx "\n",
                   env->mmpt);

    if (result == RISCV_SMMPT_BARE) {
        monitor_printf(mon, "  mode:             BARE\n");
        monitor_printf(mon, "  enabled:          no\n");
        monitor_printf(mon, "  sdid:             %u\n", config.sdid);
        monitor_printf(mon, "  root-pa:          " HWADDR_FMT_plx "\n",
                       config.root_pa);
    } else if (result == RISCV_SMMPT_OK) {
        monitor_printf(mon, "  mode:             %u\n",
                       (unsigned int)config.mode);
        monitor_printf(mon, "  enabled:          yes\n");
        monitor_printf(mon, "  sdid:             %u\n", config.sdid);
        monitor_printf(mon, "  root-pa:          " HWADDR_FMT_plx "\n",
                       config.root_pa);
    } else {
        monitor_printf(mon, "  decode-result:    %s\n",
                       riscv_smmpt_result_name(result));
    }

    lookups_per_tlb = env->smmpt_stats.tlb_fills ?
        (double)env->smmpt_stats.lookup_requests /
        (double)env->smmpt_stats.tlb_fills : 0.0;

    entries_per_tlb = env->smmpt_stats.tlb_fills ?
        (double)env->smmpt_stats.entry_reads /
        (double)env->smmpt_stats.tlb_fills : 0.0;

    entries_per_lookup = env->smmpt_stats.lookup_requests ?
        (double)env->smmpt_stats.entry_reads /
        (double)env->smmpt_stats.lookup_requests : 0.0;

    monitor_printf(mon, "\nSmMPT statistics\n");
    monitor_printf(mon, "  tlb-fills:         %" PRIu64 "\n",
                   env->smmpt_stats.tlb_fills);
    monitor_printf(mon, "  lookup-requests:   %" PRIu64 "\n",
                   env->smmpt_stats.lookup_requests);
    monitor_printf(mon, "  entry-reads:       %" PRIu64 "\n",
                   env->smmpt_stats.entry_reads);

    monitor_printf(mon, "\nCheck sites\n");
    monitor_printf(mon, "  final-checks:      %" PRIu64 "\n",
                   env->smmpt_stats.final_checks);
    monitor_printf(mon, "  pte-fetch-checks:  %" PRIu64 "\n",
                   env->smmpt_stats.pte_fetch_checks);
    monitor_printf(mon, "  ad-update-checks:  %" PRIu64 "\n",
                   env->smmpt_stats.ad_update_checks);

    monitor_printf(mon, "\nResults\n");
    monitor_printf(mon, "  allowed:           %" PRIu64 "\n",
                   env->smmpt_stats.allowed);
    monitor_printf(mon, "  denied:            %" PRIu64 "\n",
                   env->smmpt_stats.denied);
    monitor_printf(mon, "  bare-skips:        %" PRIu64 "\n",
                   env->smmpt_stats.bare_skips);
    monitor_printf(mon, "  policy-skips:      %" PRIu64 "\n",
                   env->smmpt_stats.policy_skips);
    monitor_printf(mon, "  memory-errors:     %" PRIu64 "\n",
                   env->smmpt_stats.memory_errors);
    monitor_printf(mon, "  invalid-results:   %" PRIu64 "\n",
                   env->smmpt_stats.invalid_results);

    monitor_printf(mon, "\nPTAC\n");
    monitor_printf(mon, "  generation:        %" PRIu64 "\n",
                   env->smmpt_ptac_generation);
    monitor_printf(mon, "  lookups:           %" PRIu64 "\n",
                   env->smmpt_stats.ptac_lookups);
    monitor_printf(mon, "  hits:              %" PRIu64 "\n",
                   env->smmpt_stats.ptac_hits);
    monitor_printf(mon, "  misses:            %" PRIu64 "\n",
                   env->smmpt_stats.ptac_misses);
    monitor_printf(mon, "  fills:             %" PRIu64 "\n",
                   env->smmpt_stats.ptac_fills);
    monitor_printf(mon, "  invalidations:     %" PRIu64 "\n",
                   env->smmpt_stats.ptac_invalidations);
    monitor_printf(mon, "  mmpt-invalidations:%" PRIu64 "\n",
                   env->smmpt_stats.mmpt_change_invalidations);
    monitor_printf(mon, "  mfence-invalidates:%" PRIu64 "\n",
                   env->smmpt_stats.mfence_mcpa_invalidations);
    monitor_printf(mon, "  pte-full-lookups:  %" PRIu64 "\n",
                   env->smmpt_stats.pte_fetch_full_lookups);
    monitor_printf(mon, "  pte-reuses:        %" PRIu64 "\n",
                   env->smmpt_stats.pte_fetch_reuses);

    monitor_printf(mon, "\nAmplification\n");
    monitor_printf(mon, "  lookups/tlb-fill:  %.4f\n", lookups_per_tlb);
    monitor_printf(mon, "  entries/tlb-fill:  %.4f\n", entries_per_tlb);
    monitor_printf(mon, "  entries/lookup:    %.4f\n", entries_per_lookup);
}

void hmp_smmpt_reset_stats(Monitor *mon, const QDict *qdict)
{
    CPUArchState *env;

    env = mon_get_cpu_env(mon);
    if (!env) {
        monitor_printf(mon, "No CPU available\n");
        return;
    }

    /*
     * Reset instrumentation only. Do not modify MMPT, SDID, MPT contents,
     * PTAC entries, PTAC generation, TLB contents, or architectural state.
     */
    riscv_smmpt_reset_stats(env);

    monitor_printf(mon, "SmMPT statistics reset for selected CPU\n");
}

void hmp_info_mem(Monitor *mon, const QDict *qdict)
{
    CPUArchState *env;

    env = mon_get_cpu_env(mon);
    if (!env) {
        monitor_printf(mon, "No CPU available\n");
        return;
    }

    if (!riscv_cpu_cfg(env)->mmu) {
        monitor_printf(mon, "S-mode MMU unavailable\n");
        return;
    }

    if (riscv_cpu_mxl(env) == MXL_RV32) {
        if (!(env->satp & SATP32_MODE)) {
            monitor_printf(mon, "No translation or protection\n");
            return;
        }
    } else {
        if (!(env->satp & SATP64_MODE)) {
            monitor_printf(mon, "No translation or protection\n");
            return;
        }
    }

    mem_info_svxx(mon, env);
}
