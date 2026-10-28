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

/* 부모와 자식은 각각 참조 하나씩을 소유한다. 죽은 자식이 자신의 thread 페이지를
 * 살려 둘 필요는 없고, 고아(orphan)는 부모를 기다리지 않는다. */
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

/* 실패할 수 있는 어떤 연산보다 먼저, 새 스레드 안에서만 호출된다. */
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
	/* 부모는 복사하는 동안 자신의 주소 공간과 파일이 바뀌지 않게 유지해야 한다. */
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
			list_remove (e); /* 허용된 단 한 번의 wait를 가져간다. */
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

/* 우리는 ELF 바이너리를 로드한다. 다음 정의들은 ELF 명세 [ELF1]에서
 * 거의 그대로 가져온 것이다. */

/* ELF 타입. [ELF1] 1-2 참고. */
#define EI_NIDENT 16

#define PT_NULL    0            /* 무시. */
#define PT_LOAD    1            /* 로드 가능한 세그먼트. */
#define PT_DYNAMIC 2            /* 동적 링킹 정보. */
#define PT_INTERP  3            /* 동적 로더의 이름. */
#define PT_NOTE    4            /* 보조 정보. */
#define PT_SHLIB   5            /* 예약됨. */
#define PT_PHDR    6            /* 프로그램 헤더 테이블. */
#define PT_STACK   0x6474e551   /* 스택 세그먼트. */

#define PF_X 1          /* 실행 가능. */
#define PF_W 2          /* 쓰기 가능. */
#define PF_R 4          /* 읽기 가능. */

/* 실행 파일 헤더. [ELF1] 1-4 ~ 1-8 참고.
 * ELF 바이너리의 맨 처음에 나온다. */
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

/* 약어 */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* FILE_NAME의 ELF 실행 파일을 현재 스레드로 로드한다.
 * 실행 파일의 진입점은 *RIP에,
 * 초기 스택 포인터는 *RSP에 저장한다.
 * 성공하면 true를, 아니면 false를 반환한다. */
 

 
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

	/* 실행 파일을 연다. */
	file = filesys_open (argv[0]);
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		goto done;
	}

	file_deny_write (file);

	/* 실행 파일 헤더를 읽고 검증한다. */
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

	/* 프로그램 헤더를 읽는다. */
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
				/* 이 세그먼트는 무시한다. */
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
						/* 일반 세그먼트.
						 * 앞부분은 디스크에서 읽고 나머지는 0으로 채운다. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* 전부 0.
						 * 디스크에서는 아무것도 읽지 않는다. */
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

	/* 스택을 설정한다. */
	if (!setup_stack (if_))
		goto done;

	/* 시작 주소. */
	if_->rip = ehdr.e_entry;

	/* 문자열, 정렬, argv[argc], 인자 포인터, 가짜 반환 주소는
	 * 모두 초기 스택 페이지 하나에 들어가야 한다. */
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


/* PHDR이 FILE 안의 유효하고 로드 가능한 세그먼트를 기술하는지
 * 검사해서, 그렇다면 true를 아니면 false를 반환한다. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset과 p_vaddr은 페이지 내 오프셋이 같아야 한다. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset은 FILE 안을 가리켜야 한다. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz는 최소한 p_filesz만큼은 커야 한다. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* 세그먼트는 비어 있으면 안 된다. */
	if (phdr->p_memsz == 0)
		return false;

	/* 가상 메모리 영역은 시작과 끝이 모두 사용자 주소 공간
	   범위 안에 있어야 한다. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* 영역이 커널 가상 주소 공간을 가로질러
	   "되돌아 넘어가서는(wrap around)" 안 된다. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* 페이지 0 매핑은 허용하지 않는다.
	   페이지 0을 매핑하는 것 자체가 좋지 않을 뿐 아니라, 허용하면
	   시스템 콜에 널 포인터를 넘기는 사용자 코드가
	   memcpy() 등의 널 포인터 assertion을 통해
	   커널을 panic시킬 가능성이 매우 높다. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* 문제없다. */
	return true;
}

#ifndef VM
/* 이 블록의 코드는 프로젝트 2 동안에만 사용된다.
 * 프로젝트 2 전체에 걸쳐 함수를 구현하고 싶다면,
 * #ifndef 매크로 밖에서 구현하라. */

/* load() 도우미 함수들. */
static bool install_page (void *upage, void *kpage, bool writable);

/* FILE의 오프셋 OFS부터 시작하는 세그먼트를 주소 UPAGE에 로드한다.
 * 총 READ_BYTES + ZERO_BYTES 바이트의 가상 메모리를 다음과 같이
 * 초기화한다:
 *
 * - UPAGE의 READ_BYTES 바이트는 FILE의 오프셋 OFS부터
 * 읽어 와야 한다.
 *
 * - UPAGE + READ_BYTES의 ZERO_BYTES 바이트는 0으로 채워야 한다.
 *
 * 이 함수가 초기화한 페이지는 WRITABLE이 true이면 사용자 프로세스가
 * 쓸 수 있어야 하고, 아니면 읽기 전용이어야 한다.
 *
 * 성공하면 true를, 메모리 할당 오류나 디스크 읽기 오류가 발생하면
 * false를 반환한다. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* 이 페이지를 어떻게 채울지 계산한다.
		 * FILE에서 PAGE_READ_BYTES 바이트를 읽고
		 * 마지막 PAGE_ZERO_BYTES 바이트는 0으로 채운다. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* 메모리 페이지를 하나 얻는다. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* 이 페이지를 로드한다. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* 페이지를 프로세스의 주소 공간에 추가한다. */
		if (!install_page (upage, kpage, writable)) {
			palloc_free_page (kpage);
			return false;
		}

		/* 다음으로 진행한다. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* USER_STACK에 0으로 채운 페이지를 매핑해 최소한의 스택을 만든다 */
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

/* 사용자 가상 주소 UPAGE에서 커널 가상 주소 KPAGE로의
 * 매핑을 페이지 테이블에 추가한다.
 * WRITABLE이 true이면 사용자 프로세스가 페이지를 수정할 수 있고,
 * 아니면 읽기 전용이다.
 * UPAGE는 이미 매핑되어 있으면 안 된다.
 * KPAGE는 아마도 palloc_get_page()로 사용자 풀에서
 * 얻은 페이지여야 한다.
 * 성공하면 true를, UPAGE가 이미 매핑되어 있거나
 * 메모리 할당에 실패하면 false를 반환한다. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* 그 가상 주소에 아직 페이지가 없는지 확인한 뒤
	 * 우리 페이지를 거기에 매핑한다. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* 여기서부터의 코드는 프로젝트 3 이후에 사용된다.
 * 프로젝트 2만을 위해 함수를 구현하고 싶다면
 * 위쪽 블록에서 구현하라. */

static bool
lazy_load_segment(struct page *page, void *aux)
{
	/* TODO: 파일로부터 세그먼트를 로드하는 작업을 수행해야 합니다. */
	/* TODO: 이 함수는 주소 VA에 대한 첫 페이지 폴트가 발생할 때 호출됩니다. */
	/* TODO: 이 함수를 호출할 때 VA는 사용 가능한 상태입니다. */
	struct file_info *info = (struct file_info *)aux;
	
	/* 파일의 읽을 위치를 ofs로 설정한다 */
	file_seek(info->file,info->ofs);
	/* 잘못된 read 발생시 false 반환 */
	if(file_read(info->file, page->frame->kva, info->read_bytes)!= (int)(info->read_bytes)){
		palloc_free_page(page->frame->kva);
		return false;
	}
	/*  */
	memset(page->frame->kva+info->read_bytes,0,info->zero_bytes);
	return true;
}

/* FILE의 오프셋 OFS부터 시작하는 세그먼트를 주소 UPAGE에 로드한다.
 * 총 READ_BYTES + ZERO_BYTES 바이트의 가상 메모리를 다음과 같이
 * 초기화한다:
 *
 * - UPAGE의 READ_BYTES 바이트는 FILE의 오프셋 OFS부터
 * 읽어 와야 한다.
 *
 * - UPAGE + READ_BYTES의 ZERO_BYTES 바이트는 0으로 채워야 한다.
 *
 * 이 함수가 초기화한 페이지는 WRITABLE이 true이면 사용자 프로세스가
 * 쓸 수 있어야 하고, 아니면 읽기 전용이어야 한다.
 *
 * 성공하면 true를, 메모리 할당 오류나 디스크 읽기 오류가 발생하면
 * false를 반환한다. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	/* 의심 */
	struct file_info *info = malloc(sizeof(struct file_info));
	if(!info) return false;
	while (read_bytes > 0 || zero_bytes > 0) {
		/* 이 페이지를 어떻게 채울지 계산한다.
		 * FILE에서 PAGE_READ_BYTES 바이트를 읽고
		 * 마지막 PAGE_ZERO_BYTES 바이트는 0으로 채운다. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: lazy_load_segment에 정보를 넘기기 위해 aux를 설정한다. */
		/* file_info에 load_segment 인자 세팅후 aux에 시작 주소 전달 */
		void *aux = NULL;
		info->file = file;
		info->ofs = ofs;
		info->upage = upage;
		info->read_bytes = page_read_bytes;
		info->zero_bytes = page_zero_bytes;
		info->writable = writable;
		aux = (void *)info;

		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux)){
			free(info); 
			return false;
		}
		/* 다음으로 진행한다. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
		ofs += PGSIZE
	}
	free(info); 
	return true;
}

/* USER_STACK에 스택 PAGE를 하나 만든다. 성공하면 true를 반환한다. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: stack_bottom에 스택을 매핑하고 즉시 해당 페이지를 claim한다.
	 * TODO: 성공하면 rsp를 그에 맞게 설정한다.
	 * TODO: 해당 페이지가 스택임을 표시해야 한다. */
	/* TODO: 여기에 코드를 작성하세요 */

	return success;
}
#endif /* VM */
