/* file.c: 메모리에 매핑된 파일 객체(mmap된 객체)의 구현. */

#include "vm/vm.h"
#include "threads/vaddr.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

/* 이 구조체는 수정하지 말 것 */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* 파일 VM의 초기화 함수 */
void
vm_file_init (void) {
}

/* 파일 기반 페이지를 초기화한다 */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* 핸들러를 설정한다 */
	page->operations = &file_ops;

	struct file_page *file_page = &page->file;
}

/* 파일에서 내용을 읽어 페이지를 swap in한다. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
}

/* 내용을 파일에 writeback하여 페이지를 swap out한다. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

/* 파일 기반 페이지를 파괴한다. PAGE는 호출자가 해제한다. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page = &page->file;
	/* 더티 비트 확인후 writeback 처리 */
	// if()
}

/* mmap을 수행한다 */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	//vm_alloc_page_with_initializer(VM_UNINIT,addr,writable,file_backed_initializer,)
	/* close해도 매핑이 살아있도록 함 */
	struct file* mfile = file_reopen(file);
	if(mfile == NULL){
		return NULL;
	}
	/* 읽을 바이트 계산 -> 파일 총 크기 - offset 은 파일에서 읽을 크기임 */
	size_t read_byte = file_length(mfile) - offset;
	/* 읽을 바이트가 요청한 길이(length)보다 많이 읽지는 않아야함 */
	if(read_byte>length){
		read_byte = length;
	}
	/* 총 페이지 개수와 할당 성공한 페이지 개수 */
	int page_cnt = (length + PGSIZE - 1) / PGSIZE;
	/* file_info 멤버와 이름을 맞춰 헷갈림 방지와 addr보존을 위해 따로 지정 루프에서 움직이는 커서가 될 예정 */
	void* upage = addr; /* upage = user page 줄인거니까 헷갈리지말아용ㅎㅎ~~**^^&& */
	int success = 0;
	for (int i = page_cnt; i>0; i-- ) {
		size_t page_read = read_byte < PGSIZE ? read_byte : PGSIZE;
		struct file_info* info = malloc(sizeof(struct file_info));
		/* file info 구조체를 할당하다가 실패하면 롤백 프로시저를 실행하게 해야 함 */
		if (info == NULL) {
			goto fail;
		}
	}

/* 여기서 이미 할당된 페이지랑 file_info 구조체들 싹다 반환시키고 NULL 리턴해야돼요 */
fail:
	
}

/* munmap을 수행한다 */
void
do_munmap (void *addr) {

}
