#ifndef VM_VM_H
#define VM_VM_H
#include <stdbool.h>
#include "threads/palloc.h"	
#include "lib/kernel/hash.h"

enum vm_type {
	/* 초기화되지 않은 페이지 */
	VM_UNINIT = 0,
	/* 파일과 무관한 페이지, 즉 익명(anonymous) 페이지 */
	VM_ANON = 1,
	/* 파일과 연관된 페이지 */
	VM_FILE = 2,
	/* 페이지 캐시를 담는 페이지, 프로젝트 4용 */
	VM_PAGE_CACHE = 3,

	/* 상태를 저장하는 비트 플래그 */

	/* 정보를 저장하기 위한 보조 비트 플래그 마커. int에 들어가는 값까지
	 * 마커를 더 추가해도 된다. */
	VM_MARKER_0 = (1 << 3),
	VM_MARKER_1 = (1 << 4),

	/* 이 값을 넘기지 말 것. */
	VM_MARKER_END = (1 << 31),
};

#include "vm/uninit.h"
#include "vm/anon.h"
#include "vm/file.h"
#ifdef EFILESYS
#include "filesys/page_cache.h"
#endif

struct page_operations;
struct thread;

#define VM_TYPE(type) ((type) & 7)

/* "page"의 표현.
 * 일종의 "부모 클래스"이며, 네 개의 "자식 클래스"인
 * uninit_page, file_page, anon_page, 페이지 캐시(프로젝트 4)를 가진다.
 * 이 구조체에 미리 정의된 멤버는 삭제/수정하지 말 것. */
struct page {
	const struct page_operations *operations;
	void *va;              /* 유저 공간 기준 주소 */
	struct frame *frame;   /* 프레임에 대한 역참조 */

	/* 직접 구현하는 부분 */
	struct hash_elem elem; /* 해시 충돌 체인 */
	/* 타입별 데이터는 union에 묶여 있다.
	 * 각 함수는 현재 union의 종류를 자동으로 판별한다. */
	union {
		struct uninit_page uninit;
		struct anon_page anon;
		struct file_page file;
#ifdef EFILESYS
		struct page_cache page_cache;
#endif
	};
};

/* "frame"의 표현 */
struct frame {
	void *kva;
	struct page *page;
	struct list_elem *elem;
};

/* 페이지 연산을 위한 함수 테이블.
 * C에서 "인터페이스"를 구현하는 한 가지 방법이다.
 * "메서드" 테이블을 구조체 멤버에 넣어 두고,
 * 필요할 때마다 호출한다. */
struct page_operations {
	bool (*swap_in) (struct page *, void *);
	bool (*swap_out) (struct page *);
	void (*destroy) (struct page *);
	enum vm_type type;
};

#define swap_in(page, v) (page)->operations->swap_in ((page), v)
#define swap_out(page) (page)->operations->swap_out (page)
#define destroy(page) \
	if ((page)->operations->destroy) (page)->operations->destroy (page)

/* 현재 프로세스의 메모리 공간의 표현.
 * 이 구조체에는 특정 설계를 강제하지 않는다.
 * 설계는 전적으로 자유이다. */
struct supplemental_page_table {
	struct hash *hash_table;
};

#include "threads/thread.h"
void supplemental_page_table_init (struct supplemental_page_table *spt);
bool supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src);
void supplemental_page_table_kill (struct supplemental_page_table *spt);
struct page *spt_find_page (struct supplemental_page_table *spt,
		void *va);
bool spt_insert_page (struct supplemental_page_table *spt, struct page *page);
void spt_remove_page (struct supplemental_page_table *spt, struct page *page);

void vm_init (void);
bool vm_try_handle_fault (struct intr_frame *f, void *addr, bool user,
		bool write, bool not_present);

#define vm_alloc_page(type, upage, writable) \
	vm_alloc_page_with_initializer ((type), (upage), (writable), NULL, NULL)
bool vm_alloc_page_with_initializer (enum vm_type type, void *upage,
		bool writable, vm_initializer *init, void *aux);
void vm_dealloc_page (struct page *page);
bool vm_claim_page (void *va);
enum vm_type page_get_type (struct page *page);

#endif  /* VM_VM_H */
