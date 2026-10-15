/* SPDX-License-Identifier: MIT */
/* A libc-free PID 1 used to prove that the nested Linux guest reached userspace. */

typedef unsigned long size_t;

struct utsname {
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
	char domainname[65];
};

static long syscall0(long number)
{
	long result;
	__asm__ volatile("syscall"
			 : "=a"(result)
			 : "a"(number)
			 : "rcx", "r11", "memory");
	return result;
}

static long syscall1(long number, long arg1)
{
	long result;
	__asm__ volatile("syscall"
			 : "=a"(result)
			 : "a"(number), "D"(arg1)
			 : "rcx", "r11", "memory");
	return result;
}

static long syscall3(long number, long arg1, long arg2, long arg3)
{
	long result;
	__asm__ volatile("syscall"
			 : "=a"(result)
			 : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3)
			 : "rcx", "r11", "memory");
	return result;
}

static size_t string_length(const char *text)
{
	size_t length = 0;
	while (text[length] != '\0') {
		++length;
	}
	return length;
}

static void write_text(const char *text)
{
	size_t remaining = string_length(text);
	while (remaining != 0) {
		long written = syscall3(1, 1, (long)text, (long)remaining);
		if (written <= 0) {
			return;
		}
		text += written;
		remaining -= (size_t)written;
	}
}

static void serial_putc(char value)
{
	unsigned char ready;

	do {
		__asm__ volatile("inb %%dx, %%al"
				 : "=a"(ready)
				 : "d"((unsigned short)0x3fd));
	} while ((ready & 0x20) == 0);
	__asm__ volatile("outb %%al, %%dx"
			 :
			 : "a"((unsigned char)value), "d"((unsigned short)0x3f8));
}

static void serial_write_text(const char *text)
{
	while (*text != '\0') {
		if (*text == '\n') {
			serial_putc('\r');
		}
		serial_putc(*text++);
	}
}

static __attribute__((noreturn)) void stop_with_failure(const char *reason)
{
	write_text("FH_LINUX_GUEST result=FAIL reason=");
	write_text(reason);
	write_text("\n");
	for (;;) {
		__asm__ volatile("ud2");
	}
}

__attribute__((noreturn)) void _start(void)
{
	struct utsname name;
	long pid = syscall0(39);

	if (syscall1(63, (long)&name) != 0) {
		stop_with_failure("uname");
	}

	/* iopl(3) permits synchronous proof over COM1 and the shutdown port. */
	if (syscall1(172, 3) != 0) {
		stop_with_failure("iopl");
	}
	serial_write_text("FH_LINUX_GUEST userspace-entered\n");
	if (pid != 1) {
		serial_write_text("FH_LINUX_GUEST result=FAIL reason=not-pid-1\n");
		for (;;) {
			__asm__ volatile("ud2");
		}
	}
	serial_write_text("FH_LINUX_GUEST uname=");
	serial_write_text(name.sysname);
	serial_write_text(" ");
	serial_write_text(name.release);
	serial_write_text(" ");
	serial_write_text(name.machine);
	serial_write_text("\n");
	serial_write_text("FH_LINUX_GUEST pid=1 result=PASS\n");
	serial_write_text("FH_LINUX_GUEST requesting-system-down\n");
	__asm__ volatile(
		"xor %%eax, %%eax\n\t"
		"mov $0x2000, %%ax\n\t"
		"mov $0x604, %%dx\n\t"
		"outw %%ax, %%dx"
		:
		:
		: "rax", "rdx", "memory");

	stop_with_failure("system-down-returned");
}
