/* Resolve Rust's statically populated GOT before x86's default discard rule. */
SECTION_DATA_PROLOGUE(.fh_rust_got,,)
{
	*(.got)
	*(.got.plt)
	*(.igot)
	*(.igot.plt)
} GROUP_DATA_LINK_IN(RAMABLE_REGION, ROMABLE_REGION)

/* The Rust ax-percpu template is copied into host-owned CPU-local areas. */
SECTION_DATA_PROLOGUE(.percpu,,)
{
	. = ALIGN(64);
	_percpu_load_start = .;
	KEEP(*(.percpu))
	KEEP(*(.percpu.*))
	_percpu_load_end = .;
} GROUP_DATA_LINK_IN(RAMABLE_REGION, ROMABLE_REGION)
