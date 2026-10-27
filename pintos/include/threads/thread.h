#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#ifdef VM
#include "vm/vm.h"
#endif


/* 스레드 생애주기의 상태들. */
enum thread_status {
	THREAD_RUNNING,     /* 실행 중인 스레드. */
	THREAD_READY,       /* 실행 중은 아니지만 실행할 준비가 된 상태. */
	THREAD_BLOCKED,     /* 이벤트가 일어나기를 기다리는 상태. */
	THREAD_DYING        /* 곧 제거될 상태. */
};

/* 스레드 식별자 타입.
   원하는 타입으로 다시 정의해도 된다. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* tid_t의 에러 값. */

/* 스레드 우선순위. */
#define PRI_MIN 0                       /* 가장 낮은 우선순위. */
#define PRI_DEFAULT 31                  /* 기본 우선순위. */
#define PRI_MAX 63                      /* 가장 높은 우선순위. */

bool priority_sort(const struct list_elem *a,const struct list_elem *b, void *aux UNUSED);
void priority_thread_yield(void);

/* 커널 스레드 또는 사용자 프로세스.
 *
 * 각 스레드 구조체는 자신만의 4 kB 페이지에 저장된다.
 * 스레드 구조체 자체는 페이지의 맨 아래(오프셋 0)에
 * 놓인다. 페이지의 나머지는 스레드의 커널 스택용으로
 * 예약되며, 커널 스택은 페이지의 맨 위(오프셋 4 kB)에서
 * 아래쪽으로 자란다. 그림으로 보면 다음과 같다:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * 이로부터 두 가지 결론이 나온다:
 *
 *    1. 첫째, `struct thread'가 너무 커지면 안 된다.
 *       너무 커지면 커널 스택을 위한 공간이
 *       부족해진다. 기본 `struct thread'는 겨우
 *       몇 바이트 크기다. 아마 1 kB를 한참
 *       밑돌게 유지하는 것이 좋다.
 *
 *    2. 둘째, 커널 스택이 너무 커지면 안 된다.
 *       스택이 넘치면 스레드 상태가
 *       손상된다. 따라서 커널 함수는 큰 구조체나 배열을
 *       static이 아닌 지역 변수로 할당해서는 안 된다.
 *       대신 malloc()이나 palloc_get_page()로
 *       동적으로 할당하라.
 *
 * 이 두 문제 중 어느 쪽이든 첫 증상은 아마
 * thread_current()의 assertion 실패일 것이다. 이 함수는
 * 실행 중인 스레드의 `struct thread'에 있는 `magic' 멤버가
 * THREAD_MAGIC으로 설정되어 있는지 검사한다. 스택 오버플로는 보통 이 값을
 * 바꿔 놓아 assertion이 발동하게 한다. */
/* `elem' 멤버는 두 가지 용도가 있다. 실행 큐(thread.c)의
 * 원소가 될 수도 있고, 세마포어 대기 리스트(synch.c)의
 * 원소가 될 수도 있다. 이 두 방식을 함께 쓸 수 있는 것은
 * 둘이 상호 배타적이기 때문이다. ready 상태의 스레드만
 * 실행 큐에 있고, blocked 상태의 스레드만
 * 세마포어 대기 리스트에 있다. */
struct thread {
	/* thread.c가 소유. */
	tid_t tid;                          /* 스레드 식별자. */
	enum thread_status status;          /* 스레드 상태. */
	char name[16];                      /* 이름 (디버깅용). */
	int priority;                       /* 우선순위. */

	/* thread.c와 synch.c가 공유. */
	struct list_elem elem;
	struct list_elem sleep_elem;		/* List element(연결 리스트 형태). */

	int my_priority;					// 내 우선순위 보존용
	struct list donation_list;			// 기부받은 스레드 목록
	struct lock *lock_to_stop;			// 어떤 lock때문에 멈춰 있는지
	struct list_elem donation_elem;		// 다른 스레드의 기부해준 스레드 목록에 들어가기 위함 

	int nice;							// nice 정의 (다른 스레드에게 얼마나 양보적인지)(범위는 -20 ~ 20)
	int recent_cpu;						// recent_cpu 정의 (각 프로세스가 최근에 cpu 시간을 얼마나 사용했는지)
	struct list_elem allelem;			// all_list에 넣기 위함

#ifdef USERPROG
	/* userprog/process.c가 소유. */
	uint64_t *pml4;                     /* 페이지 맵 레벨 4 */
	struct list children;               /* 아직 wait하지 않은 자식의 상태 기록. */
	struct child_status *child_status;  /* 이 프로세스의 부모와 공유. */
	struct file_handle **fd_table;      /* 따로 할당한 디스크립터 테이블. */
	struct file *executable;            /* exec나 exit 전까지 쓰기를 막는다. */
	int exit_status;
	bool user_process;
#endif
#ifdef VM
	/* 스레드가 소유한 전체 가상 메모리를 위한 테이블. */
	struct supplemental_page_table spt;
#endif
	int64_t timer_tick;

	/* thread.c가 소유. */
	struct intr_frame tf;               /* 컨텍스트 스위칭용 정보 */
	unsigned magic;                     /* 스택 오버플로를 감지한다. */
};

/* false(기본값)이면 라운드 로빈 스케줄러를 사용한다.
   true이면 다단계 피드백 큐 스케줄러를 사용한다.
   커널 명령줄 옵션 "-o mlfqs"로 제어한다. */
// 커널 옵션으로 -mlfqs를 전달하면 thread_mlfqs가 true로 설정됨. 기본값은 false
// thread_mlfqs가 false이면, 우선순위 스케줄러 + 기부(Donation) 방식으로 작동함
// thread_mlfqs가 true이면 MLFQS(다단계 피드백 큐 스케줄러)방식으로 작동함
extern bool thread_mlfqs;

extern int load_avg;					// load_avg 정의

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void mlfqs_calculate_priority (struct thread *t);
void mlfqs_calculate_recent_cpu (struct thread *t);
void mlfqs_calculate_load_avg (void);
void mlfqs_increment_recent_cpu (void);
void mlfqs_recalculate_all_priority (void);
void mlfqs_recalculate_all_recent_cpu (void);

void do_iret (struct intr_frame *tf);

#endif /* threads/thread.h */
