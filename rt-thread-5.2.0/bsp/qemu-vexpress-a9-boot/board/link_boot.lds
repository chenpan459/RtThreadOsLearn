OUTPUT_FORMAT("elf32-littlearm", "elf32-littlearm", "elf32-littlearm")
OUTPUT_ARCH(arm)
MEMORY
{
    ROM (rx)  : ORIGIN = 0x40000000, LENGTH = 512k
    /* Boot RAM high — App loads at RT_APP_LOAD_ADDR 0x60010000 */
    RAM (rwx) : ORIGIN = 0x61000000, LENGTH = 4M
}
SECTIONS
{
    . = ORIGIN(ROM);

    __text_start = .;
    .text :
    {
        KEEP(*(.isr_vector))
        *(.vectors)
        *(.text)
        *(.text.*)
        . = ALIGN(4);
        __fsymtab_start = .;
        KEEP(*(FSymTab))
        __fsymtab_end = .;
        . = ALIGN(4);
        __vsymtab_start = .;
        KEEP(*(VSymTab))
        __vsymtab_end = .;
        . = ALIGN(4);
        __rt_init_start = .;
        KEEP(*(SORT(.rti_fn*)))
        __rt_init_end = .;
    } > ROM
    __text_end = .;

    .text.isr :
    {
        *(.text.isr)
    } > ROM

    __rodata_start = .;
    .rodata :
    {
        *(.rodata)
        *(.rodata.*)
    } > ROM
    __rodata_end = .;

    .ARM.exidx :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > ROM

    . = ALIGN(8);
    .data :
    {
        _sdata = .;
        *(.data)
        *(.data.*)
        _edata = .;
    } > RAM AT> ROM
    _sidata = LOADADDR(.data);
    __data_start = _sdata;
    __data_end = _edata;

    . = ALIGN(4);
    __bss_start = ADDR(.bss);
    .bss (NOLOAD) :
    {
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4);
    } > RAM
    __bss_end = ADDR(.bss) + SIZEOF(.bss);

    _end = __bss_end;
}
