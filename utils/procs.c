/* procs.c, Copyright (c) 2022 Robbie Huffman <robbie.huffman@nundrum.net>
 * Heavily taken from:
 * https://gitlab.com/procps-ng/procps
 *
 * Reads the Linux system process table from /proc into an array
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <ctype.h>

#include "procs.h"

struct utlbuf_s {
	char *buf;
	int siz;
} utlbuf_s;

static inline void oomscore2proc(const char *S, proc_t *P)
{
	    sscanf(S, "%d", &P->oom_score);
}

static inline void oomadj2proc(const char *S, proc_t *P)
{
	    sscanf(S, "%d", &P->oom_adj);
}

static int stat2proc (const char *S, proc_t *P) {
	char *tmp;

	P->rtprio = -1;
	P->sched = -1;

	/* printf ("parsing %s\n", S); */
	sscanf(S, "%d", &P->tid);

	S = strchr(S, '(');
	if (!S) return 0;
	S++;
	tmp = strrchr(S, ')');
	if (!tmp || !tmp[1]) return 0;
	S = tmp +2;


	sscanf(S,
		   "%c "                      /* state */
		   "%d %*d %*d %d %*d "       /* ppid, pgrp, sid, tty_nr, tty_pgrp */
		   "%*u %*u %*u %*u %*u "/* flags, min_flt, cmin_flt, maj_flt, cmaj_flt */
		   "%*u %*u %*u %*u " /* utime, stime, cutime, cstime */
		   "%*d %*d "                 /* priority, nice */
		   "%*d "                     /* num_threads */
		   "%*u "                    /* 'alarm' == it_real_value (obsolete, always 0) */
		   "%*u "                   /* start_time */
		   "%lu "                     /* vsize */
		   "%lu "                     /* rss */
		   "%*u %*u %*u %*u %*u %*u " /* rsslim, start_code, end_code, start_stack, esp, eip */
		   "%*s %*s %*s %*s "         /* pending, blocked, sigign, sigcatch                      <=== DISCARDED */
		   "%*u %*u %*u "            /* 0 (former wchan), 0, 0                                  <=== Placeholders only */
		   "%*d %*d "                 /* exit_signal, task_cpu */
		   "%d %d "                   /* rt_priority, policy (sched) */
		   "%*u %*u %*u",       /* blkio_ticks, gtime, cgtime */
		   &P->state,
		   &P->ppid,
		   &P->tty,
		   &P->vsize,
		   &P->rss,
		   &P->rtprio,
		   &P->sched
		);

	 /* printf ("parsed tid %i uid %i state %c vsize %li\n", P->tid, P->uid, P->state, P->vsize);  */
	return 0;
}

static int file2str(const char *directory, const char *what, struct utlbuf_s *ub) {
	char path[PROCPATHLEN];
	int fd,num,tot_read=0,len;
	/* printf ("dir %s what %s \n", directory, what);  */

	if (ub->buf) ub->buf[0] = '\0';
	else {
		ub->buf = calloc(1, (ub->siz = buffGRW));
		if (!ub->buf) return -1;
	}

	len = snprintf(path, sizeof path, "%s/%s", directory, what);
	if (len <= 0 || (size_t)len >= sizeof path) return -1;
	if (-1 == (fd = open(path, O_RDONLY, 0))) return -1;
	while (0 < (num = read(fd, ub->buf + tot_read, ub->siz - tot_read))) {
		tot_read += num;
		if (tot_read < ub->siz) break;
		if (ub->siz >= INT_MAX - buffGRW) {
			tot_read--;
			break;
		}
		if (!(ub->buf = realloc(ub->buf, (ub->siz += buffGRW)))) {
			close(fd);
			return -1;
		}
	};
	/* printf ("content %s\n", ub->buf); */

	ub->buf[tot_read] = '\0';
	close(fd);
	if (tot_read < 1) return -1;
	return tot_read;
}


int simple_readproc(char *path, proc_t *p) {
	static __thread struct utlbuf_s ub = { NULL, 0 };
	static __thread struct stat sb;

	int rc, i;
	char fullpath[PROCPATHLEN];
	char procpath[PROCPATHLEN];

	rc = 0;

	/* filter out those non-pid dirs */
	for (i=0; i<strlen(path); i++) {
		if (! isdigit(path[i])) return -1;
	}

	snprintf(fullpath, PROCPATHLEN, "/proc/%s/stat", path);

	if (stat(fullpath, &sb) == -1) return -1;

	p->uid = sb.st_uid;

	snprintf(procpath, PROCPATHLEN, "/proc/%s", path);
	if (file2str(procpath, "stat", &ub) == -1) goto next_proc;
	rc += stat2proc(ub.buf, p);
	if (file2str(procpath, "oom_score", &ub) != -1) oomscore2proc(ub.buf, p);
	if (file2str(procpath, "oom_score_adj", &ub) != -1) oomadj2proc(ub.buf, p);


next_proc:
	return rc;
}

int stat2name (int pid, char *name) {
	char path[PROCPATHLEN];
	static __thread struct utlbuf_s ub = { NULL, 0 };
	int rc;
	char *tmp;

	fprintf(stderr,"pid: %i   ", pid);
	snprintf(path, sizeof path, "/proc/%i", pid);
	fprintf(stderr,"path: %s   ", path);
	rc = file2str(path,"stat",  &ub);
	if (rc <= 0) return rc;

	ub.buf = strchr(ub.buf, '(');
	if (!ub.buf) return 0;
	ub.buf++;
	tmp = strrchr(ub.buf, ')');
	if (!tmp || !tmp[1]) return 0;

	fprintf(stderr,"ub.buf %p tmp %p diff %lu ", ub.buf, tmp, (tmp - ub.buf));

	strncpy(name, ub.buf, (tmp - ub.buf));
	name[tmp - ub.buf + 1] = 0;
	fprintf(stderr,"name %s\n", name);

	return (tmp - ub.buf);
}

/*
proc_t *readproc(PROCTAB *restrict const PT, proc_t *restrict p) {
	proc_t *ret;

	free_acquired(p);

	for(;;) {
		if (errno == ENOMEM) goto out;
		if (!PT->finder(PT,p)) goto out;

		ret = PT->reader(PT,p);
		if(ret) return ret;
	}

out:
	return NULL;

}
*/

/* returns count of proccess */
int get_all_procs(proc_t p[], int maxprocs){
	DIR *procfs;
	struct dirent *pdir;
	int counter, rc;

	counter = 0;
	procfs = opendir("/proc");
	while ((pdir = readdir(procfs)) != NULL){
		/* printf ("counter %i with %s\n", counter, pdir->d_name);  */
		rc = simple_readproc(pdir->d_name, &p[counter]);
		if (rc == -1) { continue;};
		/*
		fprintf(stderr,"readproc rc %i tid %i ppid %i state %c rss %lu oom %i oomadj %i\n",
				rc, p->tid, p->ppid, p->state, p->rss, p->oom_score, p->oom_adj);
		*/
		if (p[counter].tid == 0) { continue;};
		counter++;
		if (counter == maxprocs) { break;};
	}
	closedir(procfs);
	return counter;
}

