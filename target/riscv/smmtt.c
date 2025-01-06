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
    for (;level >= 0 && !find;level--) {
        index = (addr & mtt_masks[level]) >> mtt_shifts[level];
        if (level != 0) {
            mtt_ppn = mtt_ppn + index * MTT_ADDRESS_BYTES;
            entry.base = address_space_ldl(cs->as, mtt_ppn, MEMTXATTRS_UNSPECIFIED, &r);
        }

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
            // Do nothing but need to find the right entry.
        case 0:
            switch (get_field(entry.mtt_l1, MTT_PERM_MASK << index))
            {
            case MTT_PERM_DISALLOW:
                *allowed_privs = 0;
                break;
            case MTT_PERM_ALLOW_RX:
                *allowed_privs = (PAGE_READ | PAGE_EXEC);
                break;
            case MTT_PERM_ALLOW_RW:
                *allowed_privs = (PAGE_READ | PAGE_WRITE);
                break;
            case MTT_PERM_ALLOW_RWX:
                *allowed_privs = (PAGE_READ | PAGE_WRITE | PAGE_EXEC);
                break;
            default:
                return false;
                break;
            }
        default:
            break;
        }
    }

    return (privs & *allowed_privs) == privs;

}