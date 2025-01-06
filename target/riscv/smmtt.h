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
#elif defined(TARGET_RISCV64)
    SMMTT_46,
    SMMTT_56
#endif
} smmtt_mode_t;

/*
 * MTTP CSR MASK and SHIFT
 */

#define MTTP32_MODE_MASK	    (0x3ULL << 30)
#define MTTP32_SDID_MASK	    (0x3FULL << 24)
#define MTTP32_PPN_MASK	        (0x3FFFFFULL)
#define MTTP32_MODE_SHIFT		30
#define MTTP32_SDID_SHIFT		24

#define MTTP64_MODE_MASK	    (0xFULL << 60)
#define MTTP64_SDID_MASK	    (0x3FULL << 54)
#define MTTP64_PPN_MASK		    (0x00000FFFFFFFFFFFULL)
#define MTTP64_MODE_SHIFT		60
#define MTTP64_SDID_SHIFT		54

#if defined(TARGET_RISCV32)
#define MTTP_MODE_MASK			MTTP32_MODE_MASK
#define MTTP_SDID_MASK			MTTP32_SDID_MASK
#define MTTP_PPN_MASK			MTTP32_PPN_MASK
#define MTTP_MODE_SHIFT			MTTP32_MODE_SHIFT
#define MTTP_SDID_SHIFT			MTTP32_SDID_SHIFT
#elif defined(TARGET_RISCV64)
#define MTTP_MODE_MASK			MTTP64_MODE_MASK
#define MTTP_SDID_MASK			MTTP64_SDID_MASK
#define MTTP_PPN_MASK			MTTP64_PPN_MASK
#define MTTP_MODE_SHIFT			MTTP64_MODE_SHIFT
#define MTTP_SDID_SHIFT			MTTP64_SDID_SHIFT
#endif


/*
 * MTT MASK and SHIFT
 * This will help find the right index in different MTT level.
 */

#define MTT32_L2_MASK             (0x1FFULL << 25)
#define MTT32_L1_MASK             (0x3FF << 15)
#define MTT32_L1_PAGE_MASK        (0x7 << 12)

#define MTT64_L3_MASK             (0x3FFULL << 46)
#define MTT64_L2_MASK             (0x1FFFFFULL << 25)
#define MTT64_L1_MASK             (0x1FF << 16)
#define MTT64_L1_PAGE_MASK        (0xF << 12)

#if defined(TARGET_RISCV32)
#define MTT_L2_MASK               MTT32_L2_MASK
#define MTT_L1_MASK               MTT32_L1_MASK
#define MTT_L1_PAGE_MASK          MTT32_L1_PAGE_MASK
static const unsigned long long mtt_masks[] = {
        MTT_L1_PAGE_MASK, MTT_L1_MASK, MTT_L2_MASK
};
#elif defined(TARGET_RISCV64)
#define MTT_L3_MASK               MTT64_L3_MASK
#define MTT_L2_MASK               MTT64_L2_MASK
#define MTT_L1_MASK               MTT64_L1_MASK
#define MTT_L1_PAGE_MASK          MTT64_L1_PAGE_MASK
static const unsigned long long mtt_masks[] = {
        MTT_L1_PAGE_MASK, MTT_L1_MASK, MTT_L2_MASK, MTT_L3_MASK
};
#endif

#define MTT32_L2_SHIFT            25
#define MTT32_L1_SHIFT            15
#define MTT32_L1_PAGE_SHIFT       12

#define MTT64_L3_SHIFT            46
#define MTT64_L2_SHIFT            25
#define MTT64_L1_SHIFT            16
#define MTT64_L1_PAGE_SHIFT       12

#if defined(TARGET_RISCV32)
#define MTT_L2_SHIFT              MTT32_L2_SHIFT
#define MTT_L1_SHIFT              MTT32_L1_SHIFT
#define MTT_L1_PAGE_SHIFT         MTT32_L1_PAGE_SHIFT
static const unsigned int mtt_shifts[] = {
        MTT_L1_PAGE_SHIFT, MTT_L1_SHIFT, MTT_L2_SHIFT
};
#elif defined(TARGET_RISCV64)
#define MTT_L3_SHIFT              MTT64_L3_SHIFT
#define MTT_L2_SHIFT              MTT64_L2_SHIFT
#define MTT_L1_SHIFT              MTT64_L1_SHIFT
#define MTT_L1_PAGE_SHIFT         MTT64_L1_PAGE_SHIFT
static const unsigned int mtt_shifts[] = {
        MTT_L1_PAGE_SHIFT, MTT_L1_SHIFT, MTT_L2_SHIFT, MTT_L3_SHIFT
};
#endif

#if defined(TARGET_RISCV32)
#define MTT_ADDRESS_BYTES           4
#elif defined(TARGET_RISCV64)
#define MTT_ADDRESS_BYTES           8
#endif

#define MTT_L2_2M_PAGES_SHIFT       21
#define MTT_L2_4M_PAGES_SHIFT       22

#define MTT_L2_XM_PAGES_MASK        0b11
#define MTT_L2_XM_PAGES_DISALLOW    0b00
#define MTT_L2_XM_PAGES_ALLOW_RX    0b01
#define MTT_L2_XM_PAGES_ALLOW_RW    0b10
#define MTT_L2_XM_PAGES_ALLOW_RWX   0b11

#define MTT_PERM_MASK               0b1111
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

typedef struct {
    uint64_t mtt_l2_ppn : 44;
    uint64_t zero : 20;
} smmtt_l3_t;

typedef uint64_t smmtt_l1;

typedef struct {
    uint64_t info : 44;
    uint64_t type : 2;
    uint64_t zero : 18;
} smmtt_l2_t;

/*
 * Need a entry union to complete a MTT search
 */
typedef union {
    uint64_t base;
    smmtt_l3_t mtt_l3;
    smmtt_l2_t mtt_l2;
    smmtt_l1 mtt_l1;
} smmtt_entry;

bool smmtt_hart_has_privs(CPURISCVState* env, hwaddr addr,
                          target_ulong size, int privs,
                          int *allowed_privs, target_ulong mode);

#endif