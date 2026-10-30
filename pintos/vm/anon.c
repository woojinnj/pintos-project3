/* anon.c: 디스크 이미지가 없는 페이지(일명 익명 페이지)의 구현. */

#include "vm/vm.h"
#include "devices/disk.h"
#include "vaddr.h"
#include <string.h>

/* 아래 줄은 수정하지 말 것 */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* 이 구조체는 수정하지 말 것 */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* 익명 페이지용 데이터를 초기화한다 */
void
vm_anon_init (void) {
	/* TODO: swap_disk를 설정한다. */
	swap_disk = NULL;
}

/* 파일 매핑을 초기화한다 */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* 핸들러를 설정한다 */
	page->operations = &anon_ops;

	/* anon_page를 가리키기는 하는데, 여기서는 0으로 밀어주는 작업만 하면 됨
	 * 혹시 모르니 남겨놓는 코드 줄 */
	struct anon_page *anon_page = &page->anon;
	/* kva에 해당하는 곳을 페이지 크기(4KB)만큼 0으로 설정하고, true 반환
	 * swap in/out 할 때 이 파일을 다시 봐야 함 */
	memset(kva, 0, PGSIZE);
	return true;
}

/* 스왑 디스크에서 내용을 읽어 페이지를 swap in한다. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;
}

/* 내용을 스왑 디스크에 써서 페이지를 swap out한다. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
}

/* 익명 페이지를 파괴한다. PAGE는 호출자가 해제한다. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;
}
