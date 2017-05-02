#ifndef __LIBTLOAD_H__
#define __LIBTLOAD_H__

#include <stdint.h>
#include <sys/types.h>
#include <limits.h>

#define MAX_THREADS 1024
#define CACHE_LINE_SIZE 64

#ifdef LIBTLOAD_SUPPORT_PAPI
	#define PAPI_MAX_EVENTS 10
#endif

#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)

#define ASSERT(V) ASSERT_PRINTF(V, "bye!\n")

#define ASSERT_PRINTF(V, ...) \
	if (unlikely(!(V))) { \
		dprintf("sanity error!\nfile %s at line %u assertion failed!\n%s\n", __FILE__, __LINE__, #V); \
		dprintf(__VA_ARGS__); \
		exit(1); \
	}

#define dprintf(...) printf("task load lib: " __VA_ARGS__)
#define stat_printf(tid, name, value) printf("task load lib: statistic of thread %u %s %llu\n", (tid), name, (value))

typedef uint16_t thread_stat_t;

enum {
	THREAD_AVL,
	THREAD_ALIVE,
	THREAD_DEAD
};

typedef struct thread_t {
	pid_t kernel_tid;
	pid_t kernel_pid;
	uint32_t order_id;
	uint32_t pos;
	uint32_t alive_pos;
	thread_stat_t stat;

#ifdef LIBTLOAD_SUPPORT_PAPI
	int EventSet;
	long long values[PAPI_MAX_EVENTS];
	int event_count;
#endif
}  __attribute__ ((aligned(CACHE_LINE_SIZE))) thread_t;

static inline uint32_t libtload_get_total_nthreads ()
{
	extern uint32_t libtload_nthreads_total;
	return libtload_nthreads_total;
}

thread_t* libtload_get_current_thread ();
uint32_t libtload_get_current_thread_order_id ();

char* libtload_env_get_str(char *envname);
char* libtload_strtok (char *str, char *tok, char del, uint32_t bsize);


typedef struct statstruct_proc {
  int           pid;                      /** The process id. **/
  char          exName [_POSIX_PATH_MAX]; /** The filename of the executable **/
  char          state; /** 1 **/          /** R is running, S is sleeping, 
			   D is sleeping in an uninterruptible wait,
			   Z is zombie, T is traced or stopped **/
  unsigned      euid,                      /** effective user id **/
                egid;                      /** effective group id */					     
  int           ppid;                     /** The pid of the parent. **/
  int           pgrp;                     /** The pgrp of the process. **/
  int           session;                  /** The session id of the process. **/
  int           tty;                      /** The tty the process uses **/
  int           tpgid;                    /** (too long) **/
  unsigned int	flags;                    /** The flags of the process. **/
  unsigned int	minflt;                   /** The number of minor faults **/
  unsigned int	cminflt;                  /** The number of minor faults with childs **/
  unsigned int	majflt;                   /** The number of major faults **/
  unsigned int  cmajflt;                  /** The number of major faults with childs **/
  int           utime;                    /** user mode jiffies **/
  int           stime;                    /** kernel mode jiffies **/
  int		cutime;                   /** user mode jiffies with childs **/
  int           cstime;                   /** kernel mode jiffies with childs **/
  int           counter;                  /** process's next timeslice **/
  int           priority;                 /** the standard nice value, plus fifteen **/
  unsigned int  timeout;                  /** The time in jiffies of the next timeout **/
  unsigned int  itrealvalue;              /** The time before the next SIGALRM is sent to the process **/
  int           starttime; /** 20 **/     /** Time the process started after system boot **/
  unsigned int  vsize;                    /** Virtual memory size **/
  unsigned int  rss;                      /** Resident Set Size **/
  unsigned int  rlim;                     /** Current limit in bytes on the rss **/
  unsigned int  startcode;                /** The address above which program text can run **/
  unsigned int	endcode;                  /** The address below which program text can run **/
  unsigned int  startstack;               /** The address of the start of the stack **/
  unsigned int  kstkesp;                  /** The current value of ESP **/
  unsigned int  kstkeip;                 /** The current value of EIP **/
  int		signal;                   /** The bitmap of pending signals **/
  int           blocked; /** 30 **/       /** The bitmap of blocked signals **/
  int           sigignore;                /** The bitmap of ignored signals **/
  int           sigcatch;                 /** The bitmap of catched signals **/
  unsigned int  wchan;  /** 33 **/        /** (too long) **/
  int		sched, 		  /** scheduler **/
                sched_priority;		  /** scheduler priority **/
		
} procinfo;

#endif
