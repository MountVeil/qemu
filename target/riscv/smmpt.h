/*
 * Experimental RISC-V SmMPT functional model.
 *
 * This header describes the baseline MPT lookup interface only.  It does not
 * implement an MPT cache, page-walk bypass, or translation authorization.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TARGET_RISCV_SMMPT_H
#define TARGET_RISCV_SMMPT_H

#include "cpu.h"

/*
 * MMPT mode values shared with the current OpenSBI prototype.
 *
 * RV32 supports BARE and 3:4.  RV64 supports BARE, 4:3, 5:2, and 6:4.
 */
typedef enum RISCVSMMPTMode {
    RISCV_SMMPT_MODE_BARE = 0,
    RISCV_SMMPT_MODE_43   = 1,
    RISCV_SMMPT_MODE_52   = 2,
    RISCV_SMMPT_MODE_64   = 3,
} RISCVSMMPTMode;

/*
 * Experimental enforcement policy used to compare:
 *
 *   disabled   - No SmMPT enforcement.
 *   strict     - Complete MPT lookup at every enforcement site.
 *   provenance - Translation-aware policy; currently aliases strict.
 */
typedef enum RISCVSMMPTPolicy {
    RISCV_SMMPT_POLICY_DISABLED   = 0,
    RISCV_SMMPT_POLICY_STRICT     = 1,
    RISCV_SMMPT_POLICY_PROVENANCE = 2,
} RISCVSMMPTPolicy;

/* Three-bit permission tuple stored in an MPT leaf entry. */
typedef enum RISCVSMMPTPerm {
    RISCV_SMMPT_PERM_NONE = 0x0,
    RISCV_SMMPT_PERM_R    = 0x1,
    RISCV_SMMPT_PERM_RW   = 0x3,
    RISCV_SMMPT_PERM_X    = 0x4,
    RISCV_SMMPT_PERM_RX   = 0x5,
    RISCV_SMMPT_PERM_RWX  = 0x7,
} RISCVSMMPTPerm;

typedef enum RISCVSMMPTResult {
    RISCV_SMMPT_OK = 0,
    RISCV_SMMPT_BARE,
    RISCV_SMMPT_ACCESS_FAULT,
    RISCV_SMMPT_INVALID_MODE,
    RISCV_SMMPT_INVALID_ENTRY,
    RISCV_SMMPT_UNSUPPORTED,
    RISCV_SMMPT_MEMORY_ERROR,
    RISCV_SMMPT_SKIPPED_BY_POLICY,
} RISCVSMMPTResult;

typedef struct RISCVSMMPTConfig {
    RISCVSMMPTMode mode;
    uint32_t sdid;
    hwaddr root_pa;
    bool enabled;
} RISCVSMMPTConfig;

typedef struct RISCVSMMPTLookup {
    RISCVSMMPTPerm perm;
    hwaddr entry_pa;
    int level;
    unsigned int subregion;
} RISCVSMMPTLookup;

typedef enum RISCVSMMPTCheckKind {
    RISCV_SMMPT_CHECK_FINAL,
    RISCV_SMMPT_CHECK_PTE_FETCH,
    RISCV_SMMPT_CHECK_AD_UPDATE,
} RISCVSMMPTCheckKind;

/*
 * Decode env->mmpt into a functional configuration.
 *
 * This routine performs encoding validation but does not access guest memory.
 */
RISCVSMMPTResult riscv_smmpt_decode_config(
    CPURISCVState *env,
    RISCVSMMPTConfig *config);

/*
 * Walk the MPT for a physical address.
 *
 * On success, lookup->perm contains the leaf permission tuple.  This function
 * performs no caching and does not alter CPU or guest state.
 */
RISCVSMMPTResult riscv_smmpt_lookup(
    CPURISCVState *env,
    hwaddr pa,
    RISCVSMMPTLookup *lookup);

/*
 * Check the final translated physical address against SmMPT.
 *
 * RISCV_SMMPT_BARE means that SmMPT is disabled and callers must preserve
 * ordinary QEMU behaviour.  RISCV_SMMPT_OK returns the MPT-derived PAGE_*
 * mask in page_prot.
 */
RISCVSMMPTResult riscv_smmpt_check_access(
    CPURISCVState *env,
    hwaddr pa,
    MMUAccessType access_type,
    int *page_prot);

/*
 * Check a physical PTE fetch.
 *
 * Strict mode performs a complete MPT lookup. Provenance mode may reuse a
 * matching PTAC authorization. Disabled mode skips SmMPT enforcement.
 */
RISCVSMMPTResult riscv_smmpt_check_pte_fetch(
    CPURISCVState *env,
    hwaddr pte_pa,
    int *page_prot);

/*
 * Record the result of one enforcement-site SmMPT check.
 *
 * MPT table-walk and entry-read counts are collected internally by the
 * walker.  This helper records why the lookup was requested and its outcome.
 */
void riscv_smmpt_record_check(
    CPURISCVState *env,
    RISCVSMMPTCheckKind kind,
    RISCVSMMPTResult result);

void riscv_smmpt_reset_stats(CPURISCVState *env);
void riscv_smmpt_reset_ptac(CPURISCVState *env);
void riscv_smmpt_invalidate_ptac(CPURISCVState *env);

/* Convert a three-bit SmMPT permission tuple to QEMU PAGE_* protection bits. */
int riscv_smmpt_perm_to_page_prot(RISCVSMMPTPerm perm);

const char *riscv_smmpt_result_name(RISCVSMMPTResult result);
const char *riscv_smmpt_policy_name(uint8_t policy);

#endif /* TARGET_RISCV_SMMPT_H */
