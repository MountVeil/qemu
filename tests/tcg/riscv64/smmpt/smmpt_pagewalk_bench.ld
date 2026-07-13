OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
    . = 0x80200000;

    .text : ALIGN(4)
    {
        *(.text.init)
        *(.text .text.*)
    }

    .rodata : ALIGN(8)
    {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(8)
    {
        *(.data .data.*)
    }

    .bss (NOLOAD) : ALIGN(4096)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    .target_pt (NOLOAD) : ALIGN(4096)
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
