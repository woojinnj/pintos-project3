#ifndef VM_FILE_H
#define VM_FILE_H
#include "filesys/file.h"
#include "vm/vm.h"

struct page;
enum vm_type;

struct file_page {
	struct file* file;	/* 파일 정보 */
	off_t offset;		/* 파일 수정 위치 */
	size_t read_byte;	/* 읽은 바이트 길이 */
};

/* 파일 처리는 mmap구현 끝나고 처리하는게 가능함 */
void vm_file_init (void);
bool file_backed_initializer (struct page *page, enum vm_type type, void *kva);
void *do_mmap(void *addr, size_t length, int writable,
		struct file *file, off_t offset);
void do_munmap (void *va);
#endif
