#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "userprog/syscall.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "intrinsic.h"

/* 처리한 페이지 폴트의 횟수. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);

/* 사용자 프로그램이 일으킬 수 있는
   인터럽트의 핸들러를 등록한다.

   실제 유닉스 계열 OS에서는 이 인터럽트들 대부분이
   [SV-386] 3-24, 3-25에 설명된 대로 시그널 형태로 사용자 프로세스에
   전달되지만, 우리는 시그널을 구현하지 않는다.
   대신 이 인터럽트들이 단순히 사용자 프로세스를
   죽이도록 만든다.

   페이지 폴트는 예외다. 여기서는 다른 예외와 똑같이
   취급하지만, 가상 메모리를 구현하려면 이 부분을
   바꿔야 한다.

   각 예외에 대한 설명은 [IA32-v3a] 5.15절 "Exception and Interrupt
   Reference"를 참고하라. */
void
exception_init (void) {
	/* 이 예외들은 INT, INT3, INTO, BOUND 명령어를 통해
	   사용자 프로그램이 명시적으로 일으킬 수 있다. 따라서
	   DPL==3으로 설정하여, 사용자 프로그램이 이 명령어들로
	   예외를 호출하는 것을 허용한다. */
	intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
	intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
	intr_register_int (5, 3, INTR_ON, kill,
			"#BR BOUND Range Exceeded Exception");

	/* 이 예외들은 DPL==0이라 사용자 프로세스가 INT 명령어로
	   직접 호출할 수 없다. 그래도 간접적으로는
	   발생할 수 있다. 예를 들어 #DE는 0으로 나누면
	   발생한다. */
	intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
	intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
	intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
	intr_register_int (7, 0, INTR_ON, kill,
			"#NM Device Not Available Exception");
	intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
	intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
	intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
	intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
	intr_register_int (19, 0, INTR_ON, kill,
			"#XF SIMD Floating-Point Exception");

	/* 대부분의 예외는 인터럽트를 켠 채로 처리할 수 있다.
	   페이지 폴트는 폴트 주소가 CR2에 저장되어 있어 보존해야 하므로
	   인터럽트를 꺼야 한다. */
	intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* 예외 통계를 출력한다. */
void
exception_print_stats (void) {
	printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* 사용자 프로세스가 (아마도) 일으킨 예외의 핸들러. */
static void
kill (struct intr_frame *f) {
	/* 이 인터럽트는 (아마도) 사용자 프로세스가 일으킨 것이다.
	   예를 들어 프로세스가 매핑되지 않은 가상 메모리에
	   접근하려 했을 수 있다(페이지 폴트). 지금은 단순히
	   사용자 프로세스를 죽인다. 나중에는 페이지 폴트를
	   커널에서 처리하고 싶어질 것이다. 실제 유닉스 계열 운영체제는
	   대부분의 예외를 시그널로 프로세스에 돌려주지만,
	   우리는 시그널을 구현하지 않는다. */

	/* 인터럽트 프레임의 코드 세그먼트 값으로
	   예외가 어디서 발생했는지 알 수 있다. */
	switch (f->cs) {
		case SEL_UCSEG:
			/* 사용자 코드 세그먼트이므로 예상대로 사용자 예외다.
			   사용자 프로세스를 죽인다. */
			syscall_exit (-1);

		case SEL_KCSEG:
			/* 커널 코드 세그먼트이므로 커널 버그를 뜻한다.
			   커널 코드는 예외를 일으켜서는 안 된다. (페이지 폴트는
			   커널 예외를 일으킬 수 있지만 여기까지
			   와서는 안 된다.) 이를 알리려고 커널을 panic시킨다. */
			intr_dump_frame (f);
			PANIC ("Kernel bug - unexpected interrupt in kernel");

		default:
			/* 그 밖의 코드 세그먼트? 일어나서는 안 된다.
			   커널을 panic시킨다. */
			printf ("Interrupt %#04llx (%s) in unknown segment %04x\n",
					f->vec_no, intr_name (f->vec_no), f->cs);
			thread_exit ();
	}
}

/* 페이지 폴트 핸들러. 가상 메모리를 구현하려면 채워 넣어야 하는
   뼈대 코드다. 프로젝트 2의 일부 해법에서도
   이 코드를 수정해야 할 수 있다.

   진입 시점에 폴트가 난 주소는 CR2(Control Register
   2)에 있고, 폴트에 대한 정보는 exception.h의 PF_* 매크로에
   설명된 형식으로 F의 error_code 멤버에 있다.
   여기 예제 코드는 그 정보를 해석하는 방법을 보여준다.
   이 둘에 대한 더 자세한 내용은
   [IA32-v3a] 5.15절 "Exception and Interrupt Reference"의
   "Interrupt 14--Page Fault Exception (#PF)" 설명에서 찾을 수 있다. */
static void
page_fault (struct intr_frame *f) {
	bool not_present;  /* true: 페이지가 없음, false: 읽기 전용 페이지에 쓰기. */
	bool write;        /* true: 쓰기 접근, false: 읽기 접근. */
	bool user;         /* true: 사용자 접근, false: 커널 접근. */
	void *fault_addr;  /* 폴트 주소. */

	/* 폴트를 일으킨 주소, 즉 폴트를 일으키려고 접근한 가상 주소를
	   얻는다. 코드를 가리킬 수도, 데이터를 가리킬 수도 있다.
	   폴트를 일으킨 명령어의 주소(f->rip)와
	   반드시 같지는 않다. */
	
	fault_addr = (void *) rcr2();

	/* 인터럽트를 다시 켠다. (CR2가 바뀌기 전에 읽는 것을 보장하려고
	   잠시 꺼 두었을 뿐이다.) */
	intr_enable ();


	/* 원인을 판단한다. */
	not_present = (f->error_code & PF_P) == 0;
	write = (f->error_code & PF_W) != 0;
	user = (f->error_code & PF_U) != 0;

#ifdef VM
	/* 프로젝트 3 이후용. */
	if (vm_try_handle_fault (f, fault_addr, user, write, not_present))
		return;
#endif

	/* 페이지 폴트를 센다. */
	page_fault_cnt++;

	if (user)
		syscall_exit (-1);

	/* 예상치 못한 커널 폴트는 여전히 커널 버그다. */
	printf ("Page fault at %p: %s error %s page in %s context.\n",
			fault_addr,
			not_present ? "not present" : "rights violation",
			write ? "writing" : "reading",
			user ? "user" : "kernel");
	kill (f);
}

