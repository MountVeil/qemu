/*
 * QEMU RISC-V SMMTT (Security Manage Memory Tracking Table)
 *
 * This provides a RISC-V Security Manage Memory Tracking Table interface
 */

#ifndef RISCV_SMMTT_H
#define RISCV_SMMTT_H

#include "cpu.h"

typedef enum {
    SMMTT_BARE = 0,
#if defined(TARGET_RISCV32)
    SMMTT_34,
#else
    SMMTT_46,
    SMMTT_56
#endif
} smmtt_mode_t;

/*
 * MTTP CSR MASK and SHIFT
 */
#if defined(TARGET_RISCV32)
#define MTTP_MODE_MASK	    (0x3ULL << 30)
#define MTTP_SDID_MASK	    (0x3FULL << 24)
#define MTTP_PPN_MASK	    (0x3FFFFFULL)
#define MTTP_MODE_SHIFT		30
#define MTTP_SDID_SHIFT		24
#else
#define MTTP_MODE_MASK	    (0xFULL << 60)
#define MTTP_SDID_MASK	    (0x3FULL << 54)
#define MTTP_PPN_MASK		(0x00000FFFFFFFFFFFULL)
#define MTTP_MODE_SHIFT		60
#define MTTP_SDID_SHIFT		54
#endif


/*
 * MTT MASK and SHIFT
 * This will help find the right index in different MTT level.
 */
#if defined(TARGET_RISCV32)
#define MTT_L2_MASK             (0x1FFULL << 25)
#define MTT_L1_MASK             (0x3FF << 15)
#define MTT_L1_PAGE_MASK        (0x7 << 12)
#else
#define MTT_L3_MASK             (0x3FFULL << 46)
#define MTT_L2_MASK             (0x1FFFFFULL << 25)
#define MTT_L1_MASK             (0x1FF << 16)
#define MTT_L1_PAGE_MASK        (0xF << 12)
#endif

static const unsigned long long mtt_masks[] = {
        MTT_L1_PAGE_MASK,
        MTT_L1_MASK,
        MTT_L2_MASK,
        #if defined(TARGET_RISCV64)
        MTT_L3_MASK
        #endif
};

#define MTT_PERMS_MASK  (0b11ULL)
#define MTT_PERMS_BITS  (2)

#define MTT_PERM_FIELD(idx) \
    MTT_PERMS_MASK << (MTT_PERMS_BITS * (idx))

#define MTT_L2_2M_PAGES_SHIFT       21
#define MTT_L2_4M_PAGES_SHIFT       22

#define MTT_L2_XM_PAGES_MASK        0b11
#define MTT_L2_XM_PAGES_DISALLOW    0b00
#define MTT_L2_XM_PAGES_ALLOW_RX    0b01
#define MTT_L2_XM_PAGES_ALLOW_RW    0b10
#define MTT_L2_XM_PAGES_ALLOW_RWX   0b11

#define MTT_PERM_MASK               0b0011
#define MTT_PERM_DISALLOW           0b0000
#define MTT_PERM_ALLOW_RX           0b0001
#define MTT_PERM_ALLOW_RW           0b0010
#define MTT_PERM_ALLOW_RWX          0b0011

typedef enum {
    SMMTT_TYPE_1G_DISALLOW = 0b000,
    SMMTT_TYPE_1G_ALLOW_RX = 0b001,
    SMMTT_TYPE_1G_ALLOW_RW = 0b010,
    SMMTT_TYPE_1G_ALLOW_RWX = 0b011,
    SMMTT_TYPE_MTT_L1_DIR = 0b100,
    SMMTT_TYPE_4M_PAGES = 0b101,
    SMMTT_TYPE_2M_PAGES = 0b110
} smmtt_l2_type;

typedef enum {
    SMMTT_PERMS_MTT_L1_DIR_DISALLOWED = 0b00,
    SMMTT_PERMS_MTT_L1_DIR_ALLOW_RX = 0b01,
    SMMTT_PERMS_MTT_L1_DIR_ALLOW_RW = 0b10,
    SMMTT_PERMS_MTT_L1_DIR_ALLOW_RWX = 0b11,
} smmtt_perms_mtt_l1_dir_t;

typedef struct {
    uint64_t mtt_l2_ppn : 44;
    uint64_t zero : 20;
} smmtt_l3_t;

typedef uint64_t smmtt_l1;
#if defined(TARGET_RISCV32)
typedef struct {
    uint64_t info : 22;
    uint64_t type : 3;
    uint64_t zero : 7;
} smmtt_l2_t;
#else
typedef struct {
    uint64_t info : 44;
    uint64_t type : 3;
    uint64_t zero : 17;
} smmtt_l2_t;
#endif

bool smmtt_hart_has_privs(CPURISCVState* env, hwaddr addr,
                          target_ulong size, int privs,
                          int *allowed_privs, target_ulong mode);

#endif