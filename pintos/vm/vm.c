/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "lib/kernel/hash.h"
#include "threads/mmu.h"
/* 프레임 테이블 전역으로 선언 */
/* 1M바이트니까 1M/4KB 하면 256개여서 256개로 배열 선언하면 되지 않나? */
struct list frame_table;

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
	/* 프레임 테이블 초기화 */
	supplemental_page_table_init(thread_current()->spt);
	list_init(&frame_table);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
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

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */

		/* TODO: Insert the page into the spt. */
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
	struct page *page = NULL;
	/* 순회할 해쉬 테이블, 버킷을 담고있음 */
	struct hash_iterator i;

	// 해시 반복자 초기화
	hash_first(&i, spt->hash_table);
	// 해시 안의 모든 hash_elem을 순회, 중간에 끝날 수 있음, hash_next에서 다음 요소가 없으면 NULL을 반환함
	while (hash_next(&i)) {
		// 현재 페이지를 hash_cur를 사용해서 가져옴
		page = hash_entry(hash_cur(&i), struct page, elem);
		
		// 지금 가리키고 있는 페이지의 가상 주소와 인자로 주어진 가상 주소가 같으면 이거 반환
		if(page->va == va) {
			break;
		}
	}
	return page;
	/* TODO: Fill this function. */
	// for(int i=0; i<spt->spt_table->bucket_cnt; i++){
	// 	struct list_elem *cur_elem = list_begin(&spt->spt_table->buckets[i]);
	// 	struct page *cur_page = hash_entry(list_entry(cur_elem, struct hash_elem, list_elem), struct page, elem);
	// 	if(cur_page->va == va){
	// 		return cur_page;		
	// 	}
	// }
	// if(spt==NULL || va == NULL) return NULL;
	// int hash_va = hash_int((uint64_t)va) % spt->spt_table->bucket_cnt;
	// struct hash_elem *find_elem = list_elem_to_hash_elem(list_begin(&spt->spt_table->buckets[hash_va]));
	// if(find_elem)
	// page = hash_entry(find_elem, struct page, elem);
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt, struct page *page) {
	int succ = false;
	/* TODO: Fill this function. */
	// uint64_t idx = hash_int((uint64_t)page->va)%spt->hash_table->bucket_cnt;
	// struct hash_elem *find_elem = list_elem_to_hash_elem(list_begin(&spt->hash_table->buckets[idx]));
	// struct hash_iterator i;

	// 해시 반복자 초기화
	// hash_first(&i, spt->hash_table);
	// 해시 안의 모든 hash_elem을 순회, 중간에 끝날 수 있음, hash_next에서 다음 요소가 없으면 NULL을 반환함
	// while (hash_next(&i)) {
	// 	// 현재 페이지를 hash_cur를 사용해서 가져오고 비교
	// 	struct page *cur_page = hash_entry(hash_cur(&i), struct page, elem);
	// 	if(cur_page == page) {
	// 		return succ;
	// 	}
	// }
	if (hash_insert(spt->hash_table, &page->elem) == NULL) {
		succ = true;
	}
	
	return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	vm_dealloc_page (page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame = NULL;
	/* TODO: Fill this function. */
	/* User Page 가져옴 */
	void *new_addr = palloc_get_page(PAL_USER);
	frame = malloc(sizeof(*frame));

	/* frame 초기화 */
	frame->kva = new_addr;		/* kva에 new_addr 넣어줌 */
	frame->page = NULL;			/* page에 NULL을 넣어줌 */
	/* frame_table에 elem넣어줌 */
	list_push_back(&frame_table, frame->elem);	

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr) {

}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */

	return vm_do_claim_page (page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va) {
	struct page *page = NULL;
	/* TODO: Fill this function */
	// 페이지 할당받고 동적할당 실패 시 false
	page = malloc(sizeof(struct page));
	if (page == NULL) {
		return false;
	}

	page->va = va;
	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	/* 수정 필요 가능성 높음 */
	pml4_set_page (thread_current()->pml4, page->va, frame->kva, thread_current()->pml4);  /* pml4가 들어가는게 아니면 임시로 read/write다 되는 true로 바꿔넣기 */
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


/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	hash_init(spt->hash_table, my_hash, my_less, NULL);
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
}
