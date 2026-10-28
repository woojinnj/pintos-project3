/* vm.c: 가상 메모리 객체를 위한 범용 인터페이스. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "lib/kernel/hash.h"
#include "threads/mmu.h"
#include "userprog/process.h"
/* 프레임 테이블 전역으로 선언 */
/* 1M바이트니까 1M/4KB 하면 256개여서 256개로 배열 선언하면 되지 않나? */
struct list frame_table;

/* 각 하위 시스템의 초기화 코드를 호출하여 가상 메모리 서브시스템을
 * 초기화한다. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* 프로젝트 4용 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* 위쪽 줄은 수정하지 말 것. */
	/* TODO: 여기에 코드를 작성할 것. */
	/* 프레임 테이블 초기화 */
	list_init(&frame_table);
}

// 비교 함수 (정렬 기준)
static bool 
my_list_less(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	struct frame *ea = list_entry(a, struct frame, elem);
	struct frame *eb = list_entry(b, struct frame, elem);
	return ea->kva < eb->kva;
}

/* 페이지의 타입을 가져온다. 페이지가 초기화된 이후의 타입을 알고 싶을 때
 * 유용한 함수이다.
 * 이 함수는 이미 완전히 구현되어 있다. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* 보조 함수들 */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* 초기화 함수와 함께 대기 중인(pending) 페이지 객체를 생성한다. 페이지를
 * 만들고 싶다면 직접 생성하지 말고 이 함수나
 * `vm_alloc_page`를 통해 생성할 것. */
/* upage = uva(user virtual address) - 지호 */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)
			
	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* upage가 이미 사용 중인지 확인한다. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: 페이지를 생성하고, VM 타입에 맞는 초기화 함수를 가져온 뒤,
		 * TODO: uninit_new를 호출하여 "uninit" 페이지 구조체를 만든다.
		 * TODO: uninit_new 호출 후에 필드를 수정해야 한다. */
		/* 페이지를 새로 할당해서 생성함 */
		struct page* new_page = malloc(sizeof(struct page));

		/* 생성됐는지 확인 */
		if(new_page == NULL) goto err;

		/* vm_type이 뭔지 보고 확인 */
		switch (VM_TYPE(type)) {
			/* type이 VM_ANON일때 */
			case VM_ANON:
				uninit_new(new_page, upage, init, type, aux, anon_initializer);
				break;
			/* type이 VM_FILE일때 */
			case VM_FILE:
				uninit_new(new_page, upage, init, type, aux, file_backed_initializer);
				break;
		}
		/* 페이지에 권한 인자에 있는 writable을 담음 */
		new_page->writable = writable;
		/* TODO: 페이지를 spt에 삽입한다. */
		bool result = spt_insert_page(spt, new_page);
		if(!result) goto err;
		return true;
	}
err:
	return false;
}

/* spt에서 VA를 찾아 페이지를 반환한다. 오류 시 NULL을 반환한다. */
/* 의심 */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
	
	struct page p;
	struct hash_elem *e;

	p.va = pg_round_down(va);
	e = hash_find (&spt->hash_table, &p.elem);
	return e != NULL ? hash_entry (e, struct page, elem) : NULL;
}

/* 검증을 거쳐 PAGE를 spt에 삽입한다. */
bool
spt_insert_page (struct supplemental_page_table *spt, struct page *page) {
	int succ = false;
	if (hash_insert(&spt->hash_table, &page->elem) == NULL) {
		succ = true;
	}
	
	return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	hash_delete(&spt->hash_table, &page->elem);
	vm_dealloc_page (page);
}

/* 쫓겨날(evict) struct frame을 가져온다. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: 교체(eviction) 정책은 자유롭게 정한다. */

	return victim;
}

/* 페이지 하나를 쫓아내고 해당 프레임을 반환한다.
 * 오류 시 NULL을 반환한다. */
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: victim을 swap out하고 쫓겨난 프레임을 반환한다. */

	return NULL;
}

/* palloc()으로 프레임을 얻는다. 사용 가능한 페이지가 없으면 페이지를 쫓아내고
 * 그 프레임을 반환한다. 항상 유효한 주소를 반환한다. 즉, 유저 풀
 * 메모리가 가득 차면 이 함수가 프레임을 쫓아내서 사용 가능한 메모리
 * 공간을 확보한다. */
static struct frame *
vm_get_frame (void) {
	struct frame *frame = NULL;
	/* TODO: 이 함수를 구현할 것. */
	/* User Page 가져옴 */
	void *new_addr = palloc_get_page(PAL_USER);
	if(!new_addr) return NULL;
	frame = malloc(sizeof(*frame));
	if(!frame) return NULL;
	/* frame 초기화 */
	frame->kva = new_addr;		/* kva에 new_addr 넣어줌 */
	frame->page = NULL;			/* page에 NULL을 넣어줌 */
	/* frame_table에 elem넣어줌 */
	list_insert_ordered(&frame_table, &frame->elem, my_list_less, NULL);	

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* 스택을 확장한다. */
static void
vm_stack_growth (void *addr) {

}

/* 쓰기 보호된 페이지에서 발생한 폴트를 처리한다. */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* 성공하면 true를 반환한다. */
bool
vm_try_handle_fault (struct intr_frame *f , void *addr , 
	bool user , bool write , bool not_present ) {

	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page = NULL;
	int result;
	/* TODO: 폴트가 유효한지 검증한다. */
	/* TODO: 여기에 코드를 작성할 것. */

	/* 쓰기 권한이 없거나 사용자 주소가 아니면 false */
	/* 이 조건에 안 걸리고 페이지가 존재해도 false */
	if ( !write || !user || !not_present ) {
    	return false;
	}
	else if (not_present) {
    	page = spt_find_page(spt, addr);
	}
	return vm_do_claim_page (page);
}

/* 페이지를 해제한다.
 * 이 함수는 수정하지 말 것. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* VA에 할당된 페이지를 claim한다. */
bool
vm_claim_page (void *va) {
	struct page *page = spt_find_page(&thread_current()->spt, va);
	/* TODO: 이 함수를 구현할 것. */
	/* 만약 못찾아서 NULL이 반환되면 false로 종료 */
	if(!page)
		return false;
	return vm_do_claim_page (page);
}

/* PAGE를 claim하고 MMU를 설정한다. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	/* 서로 연결한다. */
	frame->page = page;
	page->frame = frame;

	/* TODO: 페이지의 VA를 프레임의 PA에 매핑하도록 페이지 테이블 엔트리를 삽입한다. */
	/* 수정 필요 가능성 높음 */
	bool result = pml4_set_page (thread_current()->pml4, page->va, frame->kva, page->writable);  /* pml4가 들어가는게 아니면 임시로 read/write다 되는 true로 바꿔넣기 */
	if(!result) return false;
	return swap_in (page, frame->kva);
}

// 해시 함수
static uint64_t my_hash(const struct hash_elem *e, void *aux UNUSED) {
    struct page *entry = hash_entry(e, struct page, elem);
    return hash_int(entry->va);
}
// 비교 함수 (정렬 기준)
static bool my_less(const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED) {
	struct page *ea = hash_entry(a, struct page, elem);
	struct page *eb = hash_entry(b, struct page, elem);
	return ea->va < eb->va;
}


/* 새 supplemental page table을 초기화한다. */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	hash_init(&spt->hash_table, my_hash, my_less, NULL);
}

/* src의 supplemental page table을 dst로 복사한다. */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
}

/* supplemental page table이 보유한 자원을 해제한다. */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: 스레드가 보유한 supplemental_page_table을 모두 파괴하고
	 * TODO: 수정된 내용을 모두 저장 장치에 writeback한다. */
}
