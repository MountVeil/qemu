/*
 * Experimental RISC-V SmMPT functional model.
 *
 * This is a deliberately minimal baseline walker:
 *
 *   - no MPT cache;
 *   - no page-walk bypass;
 *   - no page-table provenance;
 *   - no mutation of MPT entries;
 *   - no enforcement hook in cpu_helper.c yet.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "smmpt.h"
#include "exec/memory.h"
#include "exec/page-protection.h"

/*
 * Current prototype MPTE layout:
 *
 *   bit 0      V
 *   bit 1      L
 *   bit 2      N
 *   bits 7:3   reserved
 *   bits 63:8  info
 *
 * For a non-leaf entry, info contains the child-table PPN.
 * For a leaf entry, info contains sixteen three-bit permission tuples.
 */
#define SMMPT_MPTE_VALID            BIT_ULL(0)
#define SMMPT_MPTE_LEAF             BIT_ULL(1)
#define SMMPT_MPTE_NAPOT            BIT_ULL(2)
#define SMMPT_MPTE_RESERVED_MASK    (0x1fULL << 3)
#define SMMPT_MPTE_INFO_SHIFT       8
#define SMMPT_MPTE_INFO_MASK        0x00ffffffffffffffULL

#define SMMPT_CHILD_PPN_MASK        0x00000fffffffffffULL
#define SMMPT_INDEX_MASK            0x1ffULL
#define SMMPT_ROOT64_INDEX_MASK     0xfffULL

#define SMMPT_PAGE_SHIFT            12
#define SMMPT_PERM_BITS             3
#define SMMPT_SUBREGIONS            16

/*
 * The current OpenSBI SmMPT prototype indexes RV64 levels at:
 *
 *   L1: PA[24:16]
 *   L2: PA[33:25]
 *   L3: PA[42:34]
 *   L4: PA[51:43]
 *   L5: PA[63:52] for mode 6:4
 *
 * A leaf divides the entry's covered range into sixteen subregions.  At the
 * lowest level, these are sixteen 4 KiB pages.
 */
static const unsigned int smmpt_rv64_level_shift[] = {
    16, 25, 34, 43, 52,
};

static int smmpt_mode_levels(RISCVSMMPTMode mode)
{
    switch (mode) {
    case RISCV_SMMPT_MODE_43:
        return 3;
    case RISCV_SMMPT_MODE_52:
        return 4;
    case RISCV_SMMPT_MODE_64:
        return 5;
    case RISCV_SMMPT_MODE_BARE:
    default:
        return 0;
    }
}

static uint64_t smmpt_index_mask(RISCVSMMPTMode mode, int level)
{
    /*
     * Only the top level of mode 6:4 uses a 12-bit index.  Every other
     * currently modelled level uses a 9-bit index.
     */
    if (mode == RISCV_SMMPT_MODE_64 && level == 5) {
        return SMMPT_ROOT64_INDEX_MASK;
    }

    return SMMPT_INDEX_MASK;
}

static RISCVSMMPTResult smmpt_read_entry(CPURISCVState *env,
                                         hwaddr entry_pa,
                                         uint64_t *entry)
{
    CPUState *cs = env_cpu(env);
    MemTxResult result;
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;

    env->smmpt_stats.entry_reads++;

    *entry = address_space_ldq(cs->as, entry_pa, attrs, &result);
    if (result != MEMTX_OK) {
        env->smmpt_stats.memory_errors++;
        return RISCV_SMMPT_MEMORY_ERROR;
    }

    return RISCV_SMMPT_OK;
}

RISCVSMMPTResult riscv_smmpt_decode_config(CPURISCVState *env,
                                           RISCVSMMPTConfig *config)
{
    target_ulong mmpt;
    uint64_t mode;
    uint64_t sdid;
    uint64_t ppn;

    if (!config) {
        return RISCV_SMMPT_INVALID_ENTRY;
    }

    memset(config, 0, sizeof(*config));
    mmpt = env->mmpt;

    if (riscv_cpu_mxl(env) == MXL_RV32) {
        mode = (mmpt & MMPT32_MODE_MASK) >> MMPT32_MODE_SHIFT;
        sdid = (mmpt & MMPT32_SDID_MASK) >> MMPT32_SDID_SHIFT;
        ppn = mmpt & MMPT32_PPN_MASK;

        /*
         * The first implementation target is RV64.  Decode RV32 state so that
         * CSR behaviour remains well-defined, but reject walking it for now.
         */
        if (mode > 1) {
            return RISCV_SMMPT_INVALID_MODE;
        }
    } else {
        mode = (mmpt & MMPT64_MODE_MASK) >> MMPT64_MODE_SHIFT;
        sdid = (mmpt & MMPT64_SDID_MASK) >> MMPT64_SDID_SHIFT;
        ppn = mmpt & MMPT64_PPN_MASK;

        if (mode > RISCV_SMMPT_MODE_64) {
            return RISCV_SMMPT_INVALID_MODE;
        }
    }

    config->mode = (RISCVSMMPTMode)mode;
    config->sdid = sdid;
    config->root_pa = (hwaddr)ppn << SMMPT_PAGE_SHIFT;
    config->enabled = mode != RISCV_SMMPT_MODE_BARE;

    if (!config->enabled) {
        return RISCV_SMMPT_BARE;
    }

    if (config->root_pa == 0) {
        return RISCV_SMMPT_INVALID_ENTRY;
    }

    return RISCV_SMMPT_OK;
}

RISCVSMMPTResult riscv_smmpt_lookup(CPURISCVState *env,
                                    hwaddr pa,
                                    RISCVSMMPTLookup *lookup)
{
    RISCVSMMPTConfig config;
    RISCVSMMPTResult result;
    hwaddr table_pa;
    uint64_t entry;
    uint64_t info;
    uint64_t index;
    uint64_t child_ppn;
    unsigned int shift;
    unsigned int subregion;
    int levels;
    int level;

    if (!lookup) {
        return RISCV_SMMPT_INVALID_ENTRY;
    }

    memset(lookup, 0, sizeof(*lookup));

    env->smmpt_stats.lookup_requests++;

    result = riscv_smmpt_decode_config(env, &config);
    if (result != RISCV_SMMPT_OK) {
        return result;
    }

    if (riscv_cpu_mxl(env) == MXL_RV32) {
        return RISCV_SMMPT_UNSUPPORTED;
    }

    levels = smmpt_mode_levels(config.mode);
    if (levels <= 0 ||
        levels > ARRAY_SIZE(smmpt_rv64_level_shift)) {
        return RISCV_SMMPT_INVALID_MODE;
    }

    table_pa = config.root_pa;

    for (level = levels; level >= 1; level--) {
        shift = smmpt_rv64_level_shift[level - 1];
        index = (pa >> shift) & smmpt_index_mask(config.mode, level);

        /*
         * Each RV64 MPTE is eight bytes.  Overflow is checked explicitly
         * before constructing the physical address.
         */
        if (index > (HWADDR_MAX - table_pa) / sizeof(uint64_t)) {
            return RISCV_SMMPT_INVALID_ENTRY;
        }

        lookup->entry_pa = table_pa + index * sizeof(uint64_t);
        lookup->level = level;

        result = smmpt_read_entry(env, lookup->entry_pa, &entry);
        if (result != RISCV_SMMPT_OK) {
            return result;
        }

        if (!(entry & SMMPT_MPTE_VALID)) {
            return RISCV_SMMPT_ACCESS_FAULT;
        }

        if (entry & SMMPT_MPTE_RESERVED_MASK) {
            return RISCV_SMMPT_INVALID_ENTRY;
        }

        /*
         * NAPOT leaf decoding is intentionally excluded from the first
         * baseline.  Silently interpreting it as a normal leaf could grant
         * permissions to the wrong physical range.
         */
        if (entry & SMMPT_MPTE_NAPOT) {
            return RISCV_SMMPT_UNSUPPORTED;
        }

        info = (entry >> SMMPT_MPTE_INFO_SHIFT) &
               SMMPT_MPTE_INFO_MASK;

        if (entry & SMMPT_MPTE_LEAF) {
            /*
             * Each leaf covers sixteen equal subregions.  The lowest-level
             * shift is 16, so its subregion shift is 12 and each tuple
             * describes one 4 KiB physical page.
             */
            if (shift < 4) {
                return RISCV_SMMPT_INVALID_ENTRY;
            }

            subregion = (pa >> (shift - 4)) &
                        (SMMPT_SUBREGIONS - 1);

            lookup->subregion = subregion;
            lookup->perm = (RISCVSMMPTPerm)(
                (info >> (subregion * SMMPT_PERM_BITS)) & 0x7);

            switch (lookup->perm) {
            case RISCV_SMMPT_PERM_NONE:
            case RISCV_SMMPT_PERM_R:
            case RISCV_SMMPT_PERM_RW:
            case RISCV_SMMPT_PERM_X:
            case RISCV_SMMPT_PERM_RX:
            case RISCV_SMMPT_PERM_RWX:
                return RISCV_SMMPT_OK;
            default:
                return RISCV_SMMPT_INVALID_ENTRY;
            }
        }

        if (level == 1) {
            /*
             * A directory at the lowest level cannot be followed further.
             */
            return RISCV_SMMPT_INVALID_ENTRY;
        }

        /*
         * OpenSBI stores a non-leaf child PPN in MPTE.info[55:2].
         * MPTE.info itself starts at raw entry bit 8, so the child PPN
         * occupies raw entry bits [61:10].
         */
        child_ppn = (info >> 2) & SMMPT_CHILD_PPN_MASK;
        if (child_ppn == 0) {
            return RISCV_SMMPT_INVALID_ENTRY;
        }

        table_pa = (hwaddr)child_ppn << SMMPT_PAGE_SHIFT;
    }

    return RISCV_SMMPT_INVALID_ENTRY;
}

RISCVSMMPTResult riscv_smmpt_check_access(
    CPURISCVState *env,
    hwaddr pa,
    MMUAccessType access_type,
    int *page_prot)
{
    /*
     * No-MPT comparison policy. Return before decoding MMPT or reading any
     * MPT entry, keeping lookup_requests and entry_reads at zero.
     */
    if (env->smmpt_policy == RISCV_SMMPT_POLICY_DISABLED) {
        if (page_prot) {
            *page_prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        }
        return RISCV_SMMPT_SKIPPED_BY_POLICY;
    }

    RISCVSMMPTLookup lookup;
    RISCVSMMPTResult result;
    int prot;

    if (!page_prot) {
        return RISCV_SMMPT_INVALID_ENTRY;
    }

    *page_prot = 0;

    result = riscv_smmpt_lookup(env, pa, &lookup);
    if (result != RISCV_SMMPT_OK) {
        return result;
    }

    prot = riscv_smmpt_perm_to_page_prot(lookup.perm);
    *page_prot = prot;

    if (!((prot >> access_type) & 1)) {
        return RISCV_SMMPT_ACCESS_FAULT;
    }

    return RISCV_SMMPT_OK;
}

static unsigned int smmpt_ptac_index(hwaddr page_pa)
{
    return (page_pa >> TARGET_PAGE_BITS) &
           (RISCV_SMMPT_PTAC_ENTRIES - 1);
}

RISCVSMMPTResult riscv_smmpt_check_pte_fetch(
    CPURISCVState *env,
    hwaddr pte_pa,
    int *page_prot)
{
    RISCVSMMPTConfig config;
    RISCVSMMPTResult result;
    hwaddr page_pa;
    unsigned int index;

    if (env->smmpt_policy != RISCV_SMMPT_POLICY_PROVENANCE) {
        if (env->smmpt_policy == RISCV_SMMPT_POLICY_STRICT) {
            env->smmpt_stats.pte_fetch_full_lookups++;
        }

        return riscv_smmpt_check_access(env, pte_pa, MMU_DATA_LOAD,
                                        page_prot);
    }

    /*
     * Decode the current MMPT identity before consulting PTAC. This does not
     * perform an MPT memory access or increment lookup_requests.
     */
    result = riscv_smmpt_decode_config(env, &config);
    if (result != RISCV_SMMPT_OK) {
        return result;
    }

    page_pa = pte_pa & TARGET_PAGE_MASK;
    index = smmpt_ptac_index(page_pa);

    env->smmpt_stats.ptac_lookups++;

    if (env->smmpt_ptac[index].valid &&
        env->smmpt_ptac[index].page_pa == page_pa &&
        env->smmpt_ptac[index].sdid == config.sdid &&
        env->smmpt_ptac[index].root_pa == config.root_pa &&
        env->smmpt_ptac[index].generation ==
            env->smmpt_ptac_generation &&
        (env->smmpt_ptac[index].page_prot & PAGE_READ)) {
        env->smmpt_stats.ptac_hits++;
        env->smmpt_stats.pte_fetch_reuses++;

        if (page_prot) {
            *page_prot = env->smmpt_ptac[index].page_prot;
        }

        return RISCV_SMMPT_OK;
    }

    env->smmpt_stats.ptac_misses++;
    env->smmpt_stats.pte_fetch_full_lookups++;

    result = riscv_smmpt_check_access(env, pte_pa, MMU_DATA_LOAD,
                                      page_prot);
    if (result != RISCV_SMMPT_OK) {
        return result;
    }

    env->smmpt_ptac[index].valid = true;
    env->smmpt_ptac[index].page_pa = page_pa;
    env->smmpt_ptac[index].sdid = config.sdid;
    env->smmpt_ptac[index].root_pa = config.root_pa;
    env->smmpt_ptac[index].generation = env->smmpt_ptac_generation;
    env->smmpt_ptac[index].page_prot = *page_prot;

    env->smmpt_stats.ptac_fills++;

    return RISCV_SMMPT_OK;
}

void riscv_smmpt_record_check(CPURISCVState *env,
                              RISCVSMMPTCheckKind kind,
                              RISCVSMMPTResult result)
{
    switch (kind) {
    case RISCV_SMMPT_CHECK_FINAL:
        env->smmpt_stats.final_checks++;
        break;
    case RISCV_SMMPT_CHECK_PTE_FETCH:
        env->smmpt_stats.pte_fetch_checks++;
        break;
    case RISCV_SMMPT_CHECK_AD_UPDATE:
        env->smmpt_stats.ad_update_checks++;
        break;
    default:
        g_assert_not_reached();
    }

    switch (result) {
    case RISCV_SMMPT_OK:
        env->smmpt_stats.allowed++;
        break;
    case RISCV_SMMPT_BARE:
        env->smmpt_stats.bare_skips++;
        break;
    case RISCV_SMMPT_SKIPPED_BY_POLICY:
        env->smmpt_stats.policy_skips++;
        break;
    case RISCV_SMMPT_ACCESS_FAULT:
        env->smmpt_stats.denied++;
        break;
    case RISCV_SMMPT_MEMORY_ERROR:
        /*
         * smmpt_read_entry() already records the physical-read error.
         * It is also a denied access at the enforcement site.
         */
        env->smmpt_stats.denied++;
        break;
    case RISCV_SMMPT_INVALID_MODE:
    case RISCV_SMMPT_INVALID_ENTRY:
    case RISCV_SMMPT_UNSUPPORTED:
        env->smmpt_stats.invalid_results++;
        env->smmpt_stats.denied++;
        break;
    default:
        env->smmpt_stats.invalid_results++;
        env->smmpt_stats.denied++;
        break;
    }
}

void riscv_smmpt_reset_stats(CPURISCVState *env)
{
    memset(&env->smmpt_stats, 0, sizeof(env->smmpt_stats));
}

void riscv_smmpt_reset_ptac(CPURISCVState *env)
{
    memset(env->smmpt_ptac, 0, sizeof(env->smmpt_ptac));
    env->smmpt_ptac_generation = 1;
}

void riscv_smmpt_invalidate_ptac(CPURISCVState *env)
{
    memset(env->smmpt_ptac, 0, sizeof(env->smmpt_ptac));

    env->smmpt_ptac_generation++;
    if (env->smmpt_ptac_generation == 0) {
        env->smmpt_ptac_generation = 1;
    }

    env->smmpt_stats.ptac_invalidations++;
}

int riscv_smmpt_perm_to_page_prot(RISCVSMMPTPerm perm)
{
    int prot = 0;

    if (perm & RISCV_SMMPT_PERM_R) {
        prot |= PAGE_READ;
    }
    if (perm & 0x2) {
        prot |= PAGE_WRITE;
    }
    if (perm & RISCV_SMMPT_PERM_X) {
        prot |= PAGE_EXEC;
    }

    return prot;
}

const char *riscv_smmpt_result_name(RISCVSMMPTResult result)
{
    switch (result) {
    case RISCV_SMMPT_OK:
        return "ok";
    case RISCV_SMMPT_BARE:
        return "bare";
    case RISCV_SMMPT_ACCESS_FAULT:
        return "access-fault";
    case RISCV_SMMPT_INVALID_MODE:
        return "invalid-mode";
    case RISCV_SMMPT_INVALID_ENTRY:
        return "invalid-entry";
    case RISCV_SMMPT_UNSUPPORTED:
        return "unsupported";
    case RISCV_SMMPT_MEMORY_ERROR:
        return "memory-error";
    case RISCV_SMMPT_SKIPPED_BY_POLICY:
        return "policy-disabled";
    default:
        return "unknown";
    }
}

const char *riscv_smmpt_policy_name(uint8_t policy)
{
    switch (policy) {
    case RISCV_SMMPT_POLICY_DISABLED:
        return "disabled";
    case RISCV_SMMPT_POLICY_STRICT:
        return "strict";
    case RISCV_SMMPT_POLICY_PROVENANCE:
        return "provenance";
    default:
        return "invalid";
    }
}
