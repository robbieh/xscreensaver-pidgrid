
#define PROCPATHLEN 64
#define buffGRW 1024

typedef struct proc_t {
	int
		tid,        /* task id, aka PID */
		ppid,       /* parent PID */
		uid,        /* user ID, effective */
		oom_score,  /* OOM killer scroe */
		oom_adj,    /* OOM killer adjustment */
		rtprio,     /* real-time priority */
		sched,      /* scheduling class */
		tty         /* tty */
		;
	char
		state       /* char code for process state */
		;	
	unsigned long
		vsize,      /* virtual size */
		rss         /* resident set size */
        ;
	char
		*cmdline
		;
} proc_t;

int stat2name(int pid, char *name);
int get_all_procs(proc_t p[], int maxprocs);
int simple_readproc(char *parth, proc_t *p);
