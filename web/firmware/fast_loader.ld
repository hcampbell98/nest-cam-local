ENTRY(_start)
SECTIONS {
    . = 0xc0090000;
    .text : {
        *(.text.entry)
        *(.text*)
        *(.rodata*)
    }
    .data : { *(.data*) }
    /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.eh_frame) }
}
