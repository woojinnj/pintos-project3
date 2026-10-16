#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif

/* The parent and child each own one reference.  A dead child need not keep
 * its thread page alive, and an orphan never waits for its parent. */
struct child_status {
	tid_t tid;
	int exit_status;
	unsigned references;
	bool fork_ok;
	struct semaphore loaded;
	struct semaphore exited;
	struct list_elem elem;
};

struct process_start {
	struct thread *parent;
	struct child_status *status;
	struct intr_frame frame;
	char *command;
};

static void process_cleanup (void);
static bool load (char *file_name, struct intr_frame *if_);
static void initd (void *aux);
static void __do_fork (void *aux);

static void
status_release (struct child_status *status) {
	enum intr_level old = intr_disable ();
	bool last = --status->references == 0;
	intr_set_level (old);
	if (last)
		free (status);
}

static struct process_start *
start_create (void) {
	struct process_start *start = calloc (1, sizeof *start);
	struct child_status *status = calloc (1, sizeof *status);
	if (start == NULL || status == NULL) {
		free (start);
		free (status);
		return NULL;
	}
	status->references = 2;
	status->exit_status = -1;
	sema_init (&status->loaded, 0);
	sema_init (&status->exited, 0);
	start->parent = thread_current ();
	start->status = status;
	return start;
}

static tid_t
start_thread (const char *name, thread_func *entry,
		struct process_start *start) {
	struct child_status *status = start->status;
	list_push_back (&thread_current ()->children, &status->elem);
	tid_t tid = thread_create (name, PRI_DEFAULT, entry, start);
	if (tid == TID_ERROR) {
		list_remove (&status->elem);
		free (status);
		free (start);
	} else
		status->tid = tid;
	return tid;
}

/* Called only in the new thread, before any operation that can fail. */
static void
process_init (struct process_start *start) {
	struct thread *current = thread_current ();
	current->child_status = start->status;
	current->user_process = true;
	start->status->tid = current->tid;
#ifdef VM
	supplemental_page_table_init (&current->spt);
#endif
}

tid_t
process_create_initd (const char *file_name) {
	char *copy = palloc_get_page (0);
	struct process_start *start = start_create ();
	if (copy == NULL || start == NULL) {
		if (copy != NULL)
			palloc_free_page (copy);
		if (start != NULL) {
			free (start->status);
			free (start);
		}
		return TID_ERROR;
	}
	strlcpy (copy, file_name, PGSIZE);
	start->command = copy;
	char name[16];
	size_t length = strcspn (file_name, " ");
	if (length >= sizeof name)
		length = sizeof name - 1;
	memcpy (name, file_name, length);
	name[length] = '\0';
	tid_t tid = start_thread (name, initd, start);
	if (tid == TID_ERROR)
		palloc_free_page (copy);
	return tid;
}

static void
initd (void *aux) {
	struct process_start *start = aux;
	char *command = start->command;
	process_init (start);
	free (start);
	if (!syscall_files_init ()) {
		palloc_free_page (command);
		syscall_exit (-1);
	}
	if (process_exec (command) < 0)
		syscall_exit (-1);
	NOT_REACHED ();
}

tid_t
process_fork (const char *name, struct intr_frame *if_) {
	struct process_start *start = start_create ();
	if (start == NULL)
		return TID_ERROR;
	start->frame = *if_;
	struct child_status *status = start->status;
	tid_t tid = start_thread (name, __do_fork, start);
	if (tid == TID_ERROR)
		return TID_ERROR;
	/* The parent must keep its address space and files stable during copying. */
	sema_down (&status->loaded);
	if (!status->fork_ok) {
		process_wait (tid);
		return TID_ERROR;
	}
	return tid;
}

#ifndef VM
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *parent = aux;
	if (!is_user_vaddr (va) || !is_user_pte (pte))
		return true;
	void *page = palloc_get_page (PAL_USER);
	if (page == NULL)
		return false;
	memcpy (page, pml4_get_page (parent->pml4, va), PGSIZE);
	if (!pml4_set_page (thread_current ()->pml4, va, page,
			is_writable (pte))) {
		palloc_free_page (page);
		return false;
	}
	return true;
}
#endif

static void
__do_fork (void *aux) {
	struct process_start *start = aux;
	struct thread *parent = start->parent;
	struct thread *current = thread_current ();
	struct intr_frame frame = start->frame;
	process_init (start);
	free (start);
	current->pml4 = pml4_create ();
	if (current->pml4 == NULL)
		goto error;
	process_activate (current);
#ifdef VM
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif
	if (!syscall_files_copy (parent))
		goto error;
	lock_acquire (&filesys_lock);
	if (parent->executable != NULL)
		current->executable = file_duplicate (parent->executable);
	bool executable_ok = parent->executable == NULL || current->executable != NULL;
	lock_release (&filesys_lock);
	if (!executable_ok)
		goto error;
	frame.R.rax = 0;
	current->child_status->fork_ok = true;
	sema_up (&current->child_status->loaded);
	do_iret (&frame);
error:
	sema_up (&current->child_status->loaded);
	syscall_exit (-1);
}

int
process_exec (void *f_name) {
	struct intr_frame frame = {0};
	frame.ds = frame.es = frame.ss = SEL_UDSEG;
	frame.cs = SEL_UCSEG;
	frame.eflags = FLAG_IF | FLAG_MBS;
	process_cleanup ();
	bool success = load (f_name, &frame);
	palloc_free_page (f_name);
	if (!success)
		return -1;
	do_iret (&frame);
	NOT_REACHED ();
}

int
process_wait (tid_t child_tid) {
	struct list *children = &thread_current ()->children;
	for (struct list_elem *e = list_begin (children);
			e != list_end (children); e = list_next (e)) {
		struct child_status *status = list_entry (e, struct child_status, elem);
		if (status->tid == child_tid) {
			list_remove (e); /* Claim the only permitted wait. */
			sema_down (&status->exited);
			int result = status->exit_status;
			status_release (status);
			return result;
		}
	}
	return -1;
}

void
process_exit (void) {
	struct thread *current = thread_current ();
	if (current->user_process)
		printf ("%s: exit(%d)\n", current->name, current->exit_status);
	syscall_files_close ();
	process_cleanup ();
	while (!list_empty (&current->children)) {
		struct child_status *status = list_entry (
				list_pop_front (&current->children), struct child_status, elem);
		status_release (status);
	}
	if (current->child_status != NULL) {
		struct child_status *status = current->child_status;
		status->exit_status = current->exit_status;
		sema_up (&status->exited);
		status_release (status);
		current->child_status = NULL;
	}
}

static void
process_cleanup (void) {
	struct thread *current = thread_current ();
	if (current->executable != NULL) {
		lock_acquire (&filesys_lock);
		file_close (current->executable);
		current->executable = NULL;
		lock_release (&filesys_lock);
	}
#ifdef VM
	supplemental_page_table_kill (&current->spt);
#endif
	uint64_t *pml4 = current->pml4;
	if (pml4 != NULL) {
		current->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

void
process_activate (struct thread *next) {
	pml4_activate (next->pml4);
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
 

 
static bool
load (char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	char **argv = palloc_get_page (0);
	int argc = 0;
	char *token, *save_ptr;
	if (argv == NULL)
		return false;
	for (token = strtok_r (file_name, " ", &save_ptr); token != NULL;
			token = strtok_r (NULL, " ", &save_ptr)) {
		if (argc == PGSIZE / sizeof *argv) {
			palloc_free_page (argv);
			return false;
		}
		argv[argc++] = token;
	}
	if (argc == 0) {
		palloc_free_page (argv);
		return false;
	}

	lock_acquire (&filesys_lock);
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (t);

	/* Open executable file. */
	file = filesys_open (argv[0]);
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		goto done;
	}

	file_deny_write (file);

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* Strings, alignment, argv[argc], argument pointers, fake return address
	 * must all fit in the single initial stack page. */
	size_t bytes = 0;
	for (i = 0; i < argc; i++)
		bytes += strlen (argv[i]) + 1;
	if (ROUND_UP (bytes, 8) + (argc + 2) * sizeof (char *) > PGSIZE)
		goto done;
	for (i = argc - 1; i >= 0; i--) {
		size_t length = strlen (argv[i]) + 1;
		if_->rsp -= length;
		memcpy ((void *) if_->rsp, argv[i], length);
		argv[i] = (char *) if_->rsp;
	}
	if_->rsp &= ~7ULL;
	if_->rsp -= sizeof (char *);
	*(char **) if_->rsp = NULL;
	for (i = argc - 1; i >= 0; i--) {
		if_->rsp -= sizeof (char *);
		*(char **) if_->rsp = argv[i];
	}
	if_->R.rdi = argc;
	if_->R.rsi = if_->rsp;
	if_->rsp -= sizeof (void *);
	*(void **) if_->rsp = NULL;
	t->executable = file;
	file = NULL;
	success = true;

done:
	file_close (file);
	lock_release (&filesys_lock);
	palloc_free_page (argv);
	return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
/* 사용자 프로그램이 사용할 스택 메모리를 준비하고, 스택 포인(rsp)를 초기화하는 함수 */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	return success;
}
#endif /* VM */
