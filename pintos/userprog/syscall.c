#include "userprog/syscall.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "devices/input.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "userprog/process.h"
#include "threads/flags.h"
#include "intrinsic.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

struct lock filesys_lock;

/* dup2로 만든 별칭(alias)은 한 프로세스 안에서 읽기/쓰기 위치와 수명을 공유한다.
 * fork는 서로 다른 핸들마다 하나씩 복제하며, 별칭 관계는 자식의 테이블에도 유지한다. */
struct file_handle {
	struct file *file;
	unsigned references;
};
#define STDIN_HANDLE ((struct file_handle *) 1)
#define STDOUT_HANDLE ((struct file_handle *) 2)

#define MSR_STAR 0xc0000081
#define MSR_LSTAR 0xc0000082
#define MSR_SYSCALL_MASK 0xc0000084

void
syscall_init (void) {
	lock_init (&filesys_lock);
	write_msr (MSR_STAR, ((uint64_t) SEL_UCSEG - 0x10) << 48 |
			((uint64_t) SEL_KCSEG) << 32);
	write_msr (MSR_LSTAR, (uint64_t) syscall_entry);
	write_msr (MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

void
syscall_exit (int status) {
	thread_current ()->exit_status = status;
	thread_exit ();
	NOT_REACHED ();
}

/* 사용자 메모리에 접근하거나 파일 시스템 락을 잡기 전에 모든 페이지를
 * 검사한다. 범위 검사는 정수 wraparound와 커널 메모리도 거부한다. */
static void
check_buffer (const void *buffer, size_t size, bool writable) {
	if (size == 0)
		return;
	uintptr_t start = (uintptr_t) buffer;
	if (start == 0 || start >= (uintptr_t) KERN_BASE ||
			size > (uintptr_t) KERN_BASE - start)
		syscall_exit (-1);
	uintptr_t end = start + size - 1;
	for (uintptr_t page = (uintptr_t) pg_round_down (buffer);;
			page += PGSIZE) {
		uint64_t *pte = pml4e_walk (thread_current ()->pml4, page, 0);
		if (pte == NULL || (*pte & (PTE_P | PTE_U)) != (PTE_P | PTE_U) ||
				(writable && !is_writable (pte)))
			syscall_exit (-1);
		if (end - page < PGSIZE)
			break;
	}
}

static void
check_string (const char *string) {
	/* 종료 문자를 찾기 전에 각 페이지를 먼저 검증한다. */
	for (;;) {
		check_buffer (string, 1, false);
		size_t length = PGSIZE - pg_ofs (string);
		if (memchr (string, '\0', length) != NULL)
			return;
		string += length;
	}
}

static struct file_handle *
get_handle (int fd) {
	if (fd < 0 || fd >= FD_COUNT || thread_current ()->fd_table == NULL)
		return NULL;
	return thread_current ()->fd_table[fd];
}

static bool
is_file (struct file_handle *handle) {
	return handle != NULL && handle != STDIN_HANDLE && handle != STDOUT_HANDLE;
}

/* 호출자가 filesys_lock을 쥐고 있어야 한다. */
static void
close_fd (int fd) {
	struct file_handle *handle = get_handle (fd);
	if (handle == NULL)
		return;
	thread_current ()->fd_table[fd] = NULL;
	if (is_file (handle) && --handle->references == 0) {
		file_close (handle->file);
		free (handle);
	}
}

bool
syscall_files_init (void) {
	struct thread *current = thread_current ();
	current->fd_table = palloc_get_page (PAL_ZERO);
	if (current->fd_table == NULL)
		return false;
	current->fd_table[0] = STDIN_HANDLE;
	current->fd_table[1] = STDOUT_HANDLE;
	return true;
}

bool
syscall_files_copy (struct thread *parent) {
	struct thread *current = thread_current ();
	current->fd_table = palloc_get_page (PAL_ZERO);
	if (current->fd_table == NULL)
		return false;
	bool success = true;
	lock_acquire (&filesys_lock);
	for (int i = 0; i < FD_COUNT; i++) {
		struct file_handle *source = parent->fd_table[i];
		if (!is_file (source)) {
			current->fd_table[i] = source;
			continue;
		}
		int j;
		for (j = 0; j < i; j++)
			if (parent->fd_table[j] == source)
				break;
		if (j < i) {
			current->fd_table[i] = current->fd_table[j];
			current->fd_table[i]->references++;
			continue;
		}
		struct file_handle *copy = malloc (sizeof *copy);
		if (copy == NULL) {
			success = false;
			break;
		}
		copy->file = file_duplicate (source->file);
		if (copy->file == NULL) {
			free (copy);
			success = false;
			break;
		}
		copy->references = 1;
		current->fd_table[i] = copy;
	}
	lock_release (&filesys_lock);
	return success;
}

void
syscall_files_close (void) {
	struct thread *current = thread_current ();
	if (current->fd_table == NULL)
		return;
	lock_acquire (&filesys_lock);
	for (int fd = 0; fd < FD_COUNT; fd++)
		close_fd (fd);
	lock_release (&filesys_lock);
	palloc_free_page (current->fd_table);
	current->fd_table = NULL;
}

static int
sys_open (const char *name) {
	check_string (name);
	int fd;
	for (fd = 2; fd < FD_COUNT; fd++)
		if (get_handle (fd) == NULL)
			break;
	if (fd == FD_COUNT)
		return -1;
	struct file_handle *handle = malloc (sizeof *handle);
	if (handle == NULL)
		return -1;
	lock_acquire (&filesys_lock);
	handle->file = filesys_open (name);
	lock_release (&filesys_lock);
	if (handle->file == NULL) {
		free (handle);
		return -1;
	}
	handle->references = 1;
	thread_current ()->fd_table[fd] = handle;
	return fd;
}

static int
sys_read (int fd, void *buffer, unsigned size) {
	check_buffer (buffer, size, true);
	struct file_handle *handle = get_handle (fd);
	if (handle == NULL || handle == STDOUT_HANDLE || size > INT_MAX)
		return -1;
	if (handle == STDIN_HANDLE) {
		for (unsigned i = 0; i < size; i++)
			((uint8_t *) buffer)[i] = input_getc ();
		return size;
	}
	lock_acquire (&filesys_lock);
	int result = file_read (handle->file, buffer, size);
	lock_release (&filesys_lock);
	return result;
}

static int
sys_write (int fd, const void *buffer, unsigned size) {
	check_buffer (buffer, size, false);
	struct file_handle *handle = get_handle (fd);
	if (handle == NULL || handle == STDIN_HANDLE || size > INT_MAX)
		return -1;
	if (handle == STDOUT_HANDLE) {
		if (size != 0)
			putbuf (buffer, size);
		return size;
	}
	lock_acquire (&filesys_lock);
	int result = file_write (handle->file, buffer, size);
	lock_release (&filesys_lock);
	return result;
}

static int
sys_dup2 (int oldfd, int newfd) {
	struct file_handle *handle = get_handle (oldfd);
	if (handle == NULL || newfd < 0 || newfd >= FD_COUNT)
		return -1;
	if (oldfd == newfd)
		return newfd;
	lock_acquire (&filesys_lock);
	if (is_file (handle))
		handle->references++;
	close_fd (newfd);
	thread_current ()->fd_table[newfd] = handle;
	lock_release (&filesys_lock);
	return newfd;
}

void
syscall_handler (struct intr_frame *f) {
	int fd = f->R.rdi;
	struct file_handle *handle;
	const char *name;
	switch (f->R.rax) {
		case SYS_HALT:
			power_off ();
			NOT_REACHED ();
		case SYS_EXIT:
			syscall_exit (f->R.rdi);
		case SYS_FORK:
			name = (const char *) f->R.rdi;
			check_string (name);
			f->R.rax = process_fork (name, f);
			break;
		case SYS_EXEC: {
			name = (const char *) f->R.rdi;
			check_string (name);
			size_t length = strnlen (name, PGSIZE);
			if (length == PGSIZE)
				syscall_exit (-1);
			/* exec가 호출자의 주소 공간을 없애기 전에 먼저 복사한다. */
			char *command = palloc_get_page (0);
			if (command == NULL)
				syscall_exit (-1);
			memcpy (command, name, length + 1);
			if (process_exec (command) < 0)
				syscall_exit (-1);
			NOT_REACHED ();
		}
		case SYS_WAIT:
			f->R.rax = process_wait (f->R.rdi);
			break;
		case SYS_CREATE:
		case SYS_REMOVE:
			name = (const char *) f->R.rdi;
			check_string (name);
			lock_acquire (&filesys_lock);
			if (f->R.rax == SYS_CREATE)
				/* off_t는 부호 있는 타입이고 syscall의 size는 부호 없는 타입이다. */
				f->R.rax = (unsigned) f->R.rsi <= INT_MAX &&
					filesys_create (name, (unsigned) f->R.rsi);
			else
				f->R.rax = filesys_remove (name);
			lock_release (&filesys_lock);
			break;
		case SYS_OPEN:
			f->R.rax = sys_open ((const char *) f->R.rdi);
			break;
		case SYS_FILESIZE:
		case SYS_SEEK:
		case SYS_TELL: {
			handle = get_handle (fd);
			if (!is_file (handle)) {
				f->R.rax = -1;
				break;
			}
			lock_acquire (&filesys_lock);
			if (f->R.rax == SYS_FILESIZE)
				f->R.rax = file_length (handle->file);
			else if (f->R.rax == SYS_TELL)
				f->R.rax = file_tell (handle->file);
			else if ((unsigned) f->R.rsi <= INT_MAX)
				file_seek (handle->file, (unsigned) f->R.rsi);
			lock_release (&filesys_lock);
			break;
		}
		case SYS_READ:
			f->R.rax = sys_read (fd, (void *) f->R.rsi, f->R.rdx);
			break;
		case SYS_WRITE:
			f->R.rax = sys_write (fd, (const void *) f->R.rsi, f->R.rdx);
			break;
		case SYS_CLOSE:
			lock_acquire (&filesys_lock);
			close_fd (fd);
			lock_release (&filesys_lock);
			break;
		case SYS_DUP2:
			f->R.rax = sys_dup2 (fd, f->R.rsi);
			break;
		default:
			syscall_exit (-1);
	}
}
