OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
    . = 0x80200000;

    .text : ALIGN(16)
    {
        *(.text.init)
        *(.text .text.*)
    }

    .rodata : ALIGN(16)
    {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(16)
    {
        *(.data .data.*)
    }

    .bss : ALIGN(4096)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /*
     * At MPT level 2, each permission tuple covers a 2-MiB physical
     * range. Keep the revocable page-table page outside the 2-MiB range
     * containing the test code, trap handler, and root page table.
     */
    . = 0x80400000;

    .target_pt : ALIGN(4096)
    {
        *(.target_pt)
    }

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
