/*
 * QEMU RISC-V SMMTT (Security Manage Memory Tracking Table)
 *
 * This provides a RISC-V Security Manage Memory Tracking Table interface
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "cpu.h"
#include "trace.h"
#include "exec/exec-all.h"
#include "exec/page-protection.h"
#include "smmtt.h"
#include "mtt_cache.h"

 /*
  * Need a entry union to complete a MTT search
  */
typedef union {
    uint64_t base;
    smmtt_l3_t mtt_l3;
    smmtt_l2_t mtt_l2;
    smmtt_l1 mtt_l1;
} smmtt_entry;

bool check_mtt_permission(CPURISCVState *env, hwaddr pa,
                          target_ulong size, int required_privs,
                          int *allowed_privs, target_ulong mode)
{
    uint8_t sdid = (env->mttp) >> MTTP_SDID_SHIFT;

    int mtt_access = -1;
    if (required_privs == PAGE_READ) mtt_access = ACCESS_LOAD;
    else if (required_privs == PAGE_WRITE) mtt_access = ACCESS_STORE;
    else if (required_privs == PAGE_EXEC) mtt_access = ACCESS_FETCH;

    // check cache
    if (mtt_cache_lookup(pa, sdid, mtt_access)) {
        if (allowed_privs) {
            *allowed_privs = 0;
            if (mtt_access == ACCESS_LOAD)  *allowed_privs |= PAGE_READ;
            if (mtt_access == ACCESS_STORE) *allowed_privs |= PAGE_WRITE;
            if (mtt_access == ACCESS_FETCH) *allowed_privs |= PAGE_EXEC;
        }
        return true;
    }

    // cache miss → fallback to MTT walk
    bool ok = smmtt_hart_has_privs(env, pa, size, required_privs,
                                   allowed_privs, mode);

    if (ok) {
        uint8_t perms = 0;
        if (*allowed_privs & PAGE_READ)  perms |= PERM_R;
        if (*allowed_privs & PAGE_WRITE) perms |= PERM_W;
        if (*allowed_privs & PAGE_EXEC)  perms |= PERM_X;
        mtt_cache_insert(pa, sdid, perms);
    }

    return ok;
}

// bool check_mtt_permission(CPURISCVState *env, hwaddr pa,
//                           target_ulong size, int required_privs,
//                           int *allowed_privs, target_ulong mode)
// {
//     uint8_t sdid = (env->mttp) >> MTTP_SDID_SHIFT;

//     int mtt_access = -1;
//     if (required_privs == PAGE_READ) mtt_access = ACCESS_LOAD;
//     else if (required_privs == PAGE_WRITE) mtt_access = ACCESS_STORE;
//     else if (required_privs == PAGE_EXEC) mtt_access = ACCESS_FETCH;

//     // cache
//     if (mtt_cache_lookup(pa, sdid, mtt_access)) {
//         uint8_t perms = 0;
//         if (mtt_access == ACCESS_LOAD)  perms |= PERM_R;
//         if (mtt_access == ACCESS_STORE) perms |= PERM_W;
//         if (mtt_access == ACCESS_FETCH) perms |= PERM_X;
//         return true;
//     }

//     // cache miss，lookup MTT
//     bool ok = smmtt_hart_has_privs(env, pa, size, required_privs,
//                                    allowed_privs, mode);

//     if (ok) {
//         uint8_t perms = 0;
//         if (*allowed_privs & PAGE_READ)  perms |= PERM_R;
//         if (*allowed_privs & PAGE_WRITE) perms |= PERM_W;
//         if (*allowed_privs & PAGE_EXEC)  perms |= PERM_X;

//         mtt_cache_insert(pa, sdid, perms);
//     }

//     return ok;
// }

static int smmtt_decode_mttp(CPURISCVState* env, int* level) {
    smmtt_mode_t mode = get_field(env->mttp, MTTP_MODE_MASK);

    switch (mode)
    {
    case SMMTT_BARE:
        *level = -1;
        break;

#if defined(TARGET_RISCV32)
    case SMMTT_34:
#elif defined(TARGET_RISCV64)
    case SMMTT_46:
#endif
        *level = 2;
        break;

#if defined(TARGET_RISCV64)
    case SMMTT_56:
        *level = 3;
        break;
#endif
    default:
        return -1;
    }
    return 0;
}

static int mttl1_privs_from_perms(uint64_t perms, int* privs)
{
    switch ((smmtt_perms_mtt_l1_dir_t)perms) {
    case MTT_PERM_DISALLOW:
        *privs = 0;
        break;
    case MTT_PERM_ALLOW_RX:
        *privs = (PAGE_READ | PAGE_EXEC);
        break;
    case MTT_PERM_ALLOW_RW:
        *privs = (PAGE_READ | PAGE_WRITE);
        break;
    case MTT_PERM_ALLOW_RWX:
        *privs = (PAGE_READ | PAGE_WRITE | PAGE_EXEC);
        break;
    }
    return 0;
}

static int smmtt_decode_mtt_l1(hwaddr addr, smmtt_l1 entry, int* privs)
{
    target_ulong offset = get_field(addr, MTT_L1_PAGE_MASK);
    uint64_t field = MTT_PERM_FIELD(offset);
    uint64_t perms = get_field(entry, field);

    return mttl1_privs_from_perms(perms, privs);
}

static int smmtt_decode_mtt_l2(hwaddr* mtt_ppn, hwaddr addr,
                        int* privs, bool* find, smmtt_entry entry) {
    smmtt_l2_type type = (smmtt_l2_type)entry.mtt_l2.type;
    target_ulong index;
    switch (type)
    {
    case SMMTT_TYPE_1G_DISALLOW:
        *find = true;
        *privs = 0;
        break;
    case SMMTT_TYPE_1G_ALLOW_RX:
        *find = true;
        *privs = (PAGE_READ | PAGE_EXEC);
        break;
    case SMMTT_TYPE_1G_ALLOW_RW:
        *find = true;
        *privs = (PAGE_READ | PAGE_WRITE);
        break;
    case SMMTT_TYPE_1G_ALLOW_RWX:
        *find = true;
        *privs = (PAGE_READ | PAGE_WRITE | PAGE_EXEC);
        break;
    case SMMTT_TYPE_MTT_L1_DIR:
        *find = false;
        *mtt_ppn = entry.mtt_l2.info << PGSHIFT;
        break;
#if defined(TARGET_RISCV32)
    case SMMTT_TYPE_4M_PAGES:
        index = (addr & MTT_L2_MASK) >> MTT_L2_4M_PAGES_SHIFT;
#elif defined(TARGET_RISCV64)
    case SMMTT_TYPE_2M_PAGES:
        index = (addr & MTT_L2_MASK) >> MTT_L2_2M_PAGES_SHIFT;
#endif
        *find = true;
        switch (get_field(entry.mtt_l2.info, MTT_L2_XM_PAGES_MASK << index))
        {
        case MTT_L2_XM_PAGES_DISALLOW:
            *find = true;
            *privs = 0;
            break;
        case MTT_L2_XM_PAGES_ALLOW_RX:
            *find = true;
            *privs = (PAGE_READ | PAGE_EXEC);
            break;
        case MTT_L2_XM_PAGES_ALLOW_RW:
            *find = true;
            *privs = (PAGE_READ | PAGE_WRITE);
            break;
        case MTT_L2_XM_PAGES_ALLOW_RWX:
            *find = true;
            *privs = (PAGE_READ | PAGE_WRITE | PAGE_EXEC);
            break;
        default:
            return -1;
        }
        break;
    default:
        return -1;
        break;
    }
    return 0;
}


bool smmtt_hart_has_privs(CPURISCVState* env, hwaddr addr,
                        target_ulong size, int privs,
                        int *allowed_privs, target_ulong mode)
{
    int level = 0;
    int ret = 0;
    bool find = false;
    target_ulong index;
    CPUState* cs = env_cpu(env);
    MemTxResult r;


    if (mode == PRV_M || !riscv_cpu_cfg(env)->ext_smmtt) {
        *allowed_privs = (PAGE_READ | PAGE_WRITE | PAGE_EXEC);
        return true;
    }

    // Decode mttp for smmtt level
    ret = smmtt_decode_mttp(env, &level);
    if (ret < 0) {
        // No matching data
        return false;
    }

    *allowed_privs = 0;
    if (level == -1) {
        // SMMTT mode is BARE
        *allowed_privs = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
    }

    //Get Top level MTT, and MTT is 4K-aligned.
    hwaddr mtt_ppn = (hwaddr)get_field(env->mttp, MTTP_PPN_MASK) << PGSHIFT;

    smmtt_entry entry = {
        .base = 0,
    };
    // Find mtt leaf node
    for (;level > 0 && !find;level--) {
        index = get_field(addr, mtt_masks[level]);
        mtt_ppn = mtt_ppn + index * sizeof(target_long);
        entry.base = address_space_ldq(cs->as, mtt_ppn, MEMTXATTRS_UNSPECIFIED, &r);

        switch (level)
        {
        case 3:
            if (r || entry.mtt_l3.zero != 0) {
                return false;
            }
            mtt_ppn = entry.mtt_l3.mtt_l2_ppn << PGSHIFT;
            break;
        case 2:
            if (r || entry.mtt_l2.zero != 0) {
                return false;
            }
            ret = smmtt_decode_mtt_l2(&mtt_ppn, addr, allowed_privs, &find, entry);
            if (ret < 0) {
                return false;
            }
            break;
        case 1:
            ret = smmtt_decode_mtt_l1(addr, entry.mtt_l1, allowed_privs);
            if (ret < 0) {
                return false;
            }
        default:
            break;
        }
    }

    return (privs & *allowed_privs) == privs;

}