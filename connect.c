#ifndef _GNU_SOURCE
	#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <dlfcn.h>
#include <unistd.h>
#include <asm/unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#if defined(_OPENMP)
	#include <omp.h>
#endif

#include "lib.h"

#ifdef LIBTLOAD_SUPPORT_PAPI
	#include "papi.h"
#endif

#include "spinlock.h"

#define BUFFER_SIZE 2048

static const char *stat_str[] = {
	"avl",
	"alive",
	"dead"
};

typedef struct real_args_t {
	void *arg;
	void *(*fn)(void*);
	uint32_t order;
} real_args_t;

thread_t libtload_threads[MAX_THREADS] __attribute__ ((aligned(CACHE_LINE_SIZE)));
uint32_t libtload_nthreads_total = 0;
static uint32_t alive_nthreads = 0;
static spinlock_t threads_lock = SPINLOCK_INIT;
static spinlock_t first_lock = SPINLOCK_INIT;

static thread_t *alive_threads[MAX_THREADS];
thread_t *libtload_threads_by_order[MAX_THREADS];

static real_args_t vec[MAX_THREADS];
static int veci = 0;
static uint32_t order = 0;
static int started = 0;
static uint64_t s_jiffies = 0;
procinfo *pinfo;
#define WRAPPER_LABEL(LABEL)                     LABEL
#define REAL_LABEL(LABEL)                        libtload_connect_real_##LABEL
#define DECLARE_WRAPPER(LABEL, RETTYPE, ...)     static RETTYPE (*REAL_LABEL(LABEL)) (__VA_ARGS__) = NULL

DECLARE_WRAPPER(pthread_create, int, pthread_t*, const pthread_attr_t*, void *(*)(void*), void*);
DECLARE_WRAPPER(pthread_exit, void, void*);

static thread_t dummy_thread = {
	.order_id = 0
};
static __thread thread_t *thread = (thread_t*)&dummy_thread;
thread_t *first_thread;
#define ATTACH_FUNC_(FUNC, MUST) \
	if (unlikely(REAL_LABEL(FUNC) == NULL)) {\
		REAL_LABEL(FUNC) = dlsym(RTLD_NEXT, #FUNC); \
		if (REAL_LABEL(FUNC) == NULL) {\
			dprintf("failed to attach " #FUNC "\n"); \
			if (MUST) \
				exit(1); \
		}\
	}

#define ATTACH_FUNC_VERSION_(FUNC, VERSION, MUST) \
	if (unlikely(REAL_LABEL(FUNC) == NULL)) {\
		REAL_LABEL(FUNC) = dlvsym(RTLD_NEXT, #FUNC, VERSION); \
		if (REAL_LABEL(FUNC) == NULL) {\
			dprintf("failed to attach " #FUNC "\n"); \
			if (MUST) \
				exit(1); \
		}\
	}

#define ATTACH_FUNC(FUNC) ATTACH_FUNC_##FUNC

#define ATTACH_FUNC_pthread_create ATTACH_FUNC_(pthread_create, 1)
#define ATTACH_FUNC_pthread_exit ATTACH_FUNC_(pthread_exit, 1)

static inline uint32_t get_next_order_id ()
{
	uint32_t tmp;
	tmp = __sync_fetch_and_add(&order, 1);
	return tmp;
}

static void init()
{
	uint32_t i;
	
	for (i=0; i<MAX_THREADS; i++) {
		libtload_threads[i].stat = THREAD_AVL;
		alive_threads[i] = NULL;
		libtload_threads_by_order[i] = NULL;
	}
}

thread_t* libtload_get_current_thread ()
{
	return thread;
}

uint32_t libtload_get_current_thread_order_id ()
{
	return thread->order_id;
}

static uint64_t get_thread_load (pid_t kpid, pid_t ktid)
{
	char buffer[BUFFER_SIZE];
	int fd;
	ssize_t num_read;
	int32_t i;
	char *p;
	uint64_t jiffies_user;

	sprintf(buffer, "/proc/%u/task/%u/stat", (uint32_t)kpid, (uint32_t)ktid);
	fd = open(buffer, O_RDONLY, 0);
	if (fd == -1) {
		dprintf("ERROR! can't open %s\n", buffer);
		return 0;
	}
	
	num_read = read(fd, buffer, BUFFER_SIZE);
	close(fd);
	
	if (num_read == BUFFER_SIZE) {
		dprintf("ERROR! file size is too long to fit in our buffer (size %u)\n", BUFFER_SIZE);
		return 0;
	}

	buffer[num_read] = 0;

	p = strrchr(buffer, ')') + 1;

	for (i=3; i!=14; i++)
		p = strchr(p+1, ' ');

	p++;
/*	printf("buffer: %s\n", buffer);*/
/*	printf("p: %s\n", p);*/
	jiffies_user = strtoull(p, NULL, 10);
	
/*	long jiffies_sys = atol(strchr(ptrUsr,' ') + 1) ;*/

	return jiffies_user;
}

static thread_t* thread_created (uint32_t order)
{
	thread_t *t;
	pid_t ktid, kpid;
	
	kpid = getpid();
	ktid = syscall(__NR_gettid);
	
	spinlock_lock(&threads_lock);

	ASSERT_PRINTF(libtload_nthreads_total < MAX_THREADS, "Maximum number of threads reached! (%u)\n", MAX_THREADS)
	
	t = &libtload_threads[libtload_nthreads_total];

	ASSERT(t->stat == THREAD_AVL)
	
	alive_threads[alive_nthreads] = t;
	libtload_threads_by_order[order] = t;
	t->alive_pos = alive_nthreads;

	t->stat = THREAD_ALIVE;
	t->pos = libtload_nthreads_total;

	t->kernel_pid = kpid;
	t->kernel_tid = ktid;
	t->order_id = order;

	__sync_synchronize();

	alive_nthreads++;
	libtload_nthreads_total++;
	
	spinlock_unlock(&threads_lock);
	
	//dprintf("thread created %u kpid %u ktid %u\n", t->order_id, (uint32_t)t->kernel_pid, (uint32_t)t->kernel_tid);

	return t;
}

static void thread_destroyed (thread_t *t)
{
	uint32_t old_pos, new_pos;
	
	spinlock_lock(&threads_lock);
	
	alive_nthreads--;
	
	old_pos = alive_nthreads;
	new_pos = t->alive_pos;
	
	ASSERT(new_pos <= old_pos)
	
	alive_threads[old_pos]->alive_pos = new_pos;
	alive_threads[new_pos] = alive_threads[old_pos];
	alive_threads[old_pos] = NULL;
	
	t->stat = THREAD_DEAD;
	
	spinlock_unlock(&threads_lock);

	//dprintf("thread destroyed (order_id=%u old_pos=%u new_pos=%u)\n", t->order_id, old_pos, new_pos);
}

static void* create_head (void *arg)
{
	real_args_t *a = arg;
	void *r;
	
	thread = thread_created(a->order);
#ifdef LIBTLOAD_SUPPORT_PAPI
	libtload_papi_thread_init(thread);
#endif
	r = a->fn(a->arg);
#ifdef LIBTLOAD_SUPPORT_PAPI
	libtload_papi_thread_finish(thread);
#endif
	thread_destroyed(thread);
	
	return r;
}

int WRAPPER_LABEL(pthread_create) (pthread_t *a_thread, const pthread_attr_t *attr, void *(*start_routine)(void*), void *arg)
{
	real_args_t *args;
	int pos;
	
/*	if (unlikely(!initialized)) {*/
/*		dprintf("skipping tracking of thread\n");*/
/*		return REAL_LABEL(pthread_create)(thread, attr, start_routine, arg);*/
/*	}*/

	pos = __sync_fetch_and_add(&veci, 1);

	args = &vec[pos];

	args->arg = arg;
	args->fn = start_routine;
	args->order = get_next_order_id();

	spinlock_lock(&first_lock);
	if(!started)
    {  
		started=1;
		pinfo = (procinfo*)malloc(sizeof(procinfo));
	    get_proc_info(getpid(), pinfo);	
		dprintf("statistics of process vsize %d\n", pinfo->vsize);
		s_jiffies = get_thread_load(thread->kernel_pid, thread->kernel_tid);
		#ifdef LIBTLOAD_SUPPORT_PAPI
			libtload_papi_init();
		#endif

		#ifdef LIBTLOAD_SUPPORT_PAPI
			libtload_papi_thread_init(thread);
		#endif
}
	spinlock_unlock(&first_lock);
	return REAL_LABEL(pthread_create)(a_thread, attr, create_head, args);
}

void WRAPPER_LABEL(pthread_exit) (void *retval)
{
	if (unlikely(thread == &dummy_thread)) {
		REAL_LABEL(pthread_exit)(retval);
		return;
	}

#ifdef LIBTLOAD_SUPPORT_PAPI
	libtload_papi_thread_finish(thread);
#endif

	thread_destroyed(thread);
	REAL_LABEL(pthread_exit)(retval);
}

static void __attribute__((constructor)) triggered_on_app_start ()
{
	ATTACH_FUNC(pthread_create)
	ATTACH_FUNC(pthread_exit)
	
	ASSERT((sizeof(thread_t) % CACHE_LINE_SIZE) == 0)
	ASSERT(sizeof(unsigned long) == sizeof(void*))
	

	init();
	thread = thread_created( get_next_order_id() );
	//first_thread = thread_created( 0 );
	//dprintf("initialized!\n");
}

static void __attribute__((destructor)) triggered_on_app_end ()
{
	uint32_t i;
	thread_t *t;
	uint64_t load;
	
	//dprintf("app ended, %u threads were created\n", libtload_nthreads_total);

#ifdef LIBTLOAD_SUPPORT_PAPI
{
	for (i=0; i<MAX_THREADS; i++) {
		if (libtload_threads[i].stat == THREAD_ALIVE) {
			libtload_papi_thread_finish(&libtload_threads[i]);
		}
	}
	libtload_papi_finish();
}
#endif

		t = libtload_threads_by_order[0];
		
		if (likely(t) && t->stat == THREAD_ALIVE) {
			load = get_thread_load(t->kernel_pid, t->kernel_tid);
			dprintf("starting jiffies: %d\n", s_jiffies);
			stat_printf(t->order_id, "jiffies", load - s_jiffies);
		}
		
	for (i=1; i<libtload_nthreads_total; i++) {
		t = libtload_threads_by_order[i];
		
		if (likely(t) && t->stat == THREAD_ALIVE) {
			load = get_thread_load(t->kernel_pid, t->kernel_tid);
			stat_printf(t->order_id, "jiffies", load);
		}
	}
}

int get_proc_info(pid_t pid, procinfo * pinfo)
{
  char szFileName [_POSIX_PATH_MAX],
    szStatStr [2048],
    *s, *t;
  FILE *fp;
  struct stat st;
  
  if (NULL == pinfo) {
    errno = EINVAL;
    return -1;
  }

  sprintf (szFileName, "/proc/%u/stat", (unsigned) pid);
  
  if (-1 == access (szFileName, R_OK)) {
    return (pinfo->pid = -1);
  } /** if **/

  if (-1 != stat (szFileName, &st)) {
  	pinfo->euid = st.st_uid;
  	pinfo->egid = st.st_gid;
  } else {
  	pinfo->euid = pinfo->egid = -1;
  }
  
  
  if ((fp = fopen (szFileName, "r")) == NULL) {
    return (pinfo->pid = -1);
  } /** IF_NULL **/
  
  if ((s = fgets (szStatStr, 2048, fp)) == NULL) {
    fclose (fp);
    return (pinfo->pid = -1);
  }

  /** pid **/
  sscanf (szStatStr, "%u", &(pinfo->pid));
  s = strchr (szStatStr, '(') + 1;
  t = strchr (szStatStr, ')');
  strncpy (pinfo->exName, s, t - s);
  pinfo->exName [t - s] = '\0';
  
  sscanf (t + 2, "%c %d %d %d %d %d %u %u %u %u %u %d %d %d %d %d %d %u %u %d %u %u %u %u %u %u %u %u %d %d %d %d %u",
	  /*       1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33*/
	  &(pinfo->state),
	  &(pinfo->ppid),
	  &(pinfo->pgrp),
	  &(pinfo->session),
	  &(pinfo->tty),
	  &(pinfo->tpgid),
	  &(pinfo->flags),
	  &(pinfo->minflt),
	  &(pinfo->cminflt),
	  &(pinfo->majflt),
	  &(pinfo->cmajflt),
	  &(pinfo->utime),
	  &(pinfo->stime),
	  &(pinfo->cutime),
	  &(pinfo->cstime),
	  &(pinfo->counter),
	  &(pinfo->priority),
	  &(pinfo->timeout),
	  &(pinfo->itrealvalue),
	  &(pinfo->starttime),
	  &(pinfo->vsize),
	  &(pinfo->rss),
	  &(pinfo->rlim),
	  &(pinfo->startcode),
	  &(pinfo->endcode),
	  &(pinfo->startstack),
	  &(pinfo->kstkesp),
	  &(pinfo->kstkeip),
	  &(pinfo->signal),
	  &(pinfo->blocked),
	  &(pinfo->sigignore),
	  &(pinfo->sigcatch),
	  &(pinfo->wchan));
  fclose (fp);
  return 0;
}

