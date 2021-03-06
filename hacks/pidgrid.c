/* pidgrid.c, Copyright (c) 2022 Robbie Huffman <robbie.huffman@nundrum.net>
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation.  No representations are made about the suitability of this
 * software for any purpose.  It is provided "as is" without express or 
 * implied warranty.
 *
   This draws the Linux system process table in a colorful way.
 *
 * Each line is a process, and each segment on that line is a snapshot
 * of that process over time. The width/height of the line reflects
 * the RSS of the most recent snapshot. Colors represent ownership
 * by root, nobody, system users, and human users.
 *
 */

#define _GNU_SOURCE
#include "screenhack.h"
#include <stdio.h>
#include <stdbool.h>
#include "utils/procs.c"
#include <search.h>

#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
# include "xdbe.h"
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */

#define MAXHIST 200
#define MAXUID 65534
#define ROOT 0
#define SYSMIN 1
#define SYSMAX 999
#define USERS 1000
#define NOBODY 65534

#define MAXPROCS 1000

struct proc_t_history {
	int tid;
	bool present;
	bool visible;
	proc_t processes[MAXHIST];
};

enum detailstates { waiting, growing, showing, shrinking, newpid };

struct state {
	Display *dpy;
	Window window;

	XGCValues gcv;      /* The structure to hold the GC data */
	Pixmap b, ba;	/* double-buffer to reduce flicker */
	Bool dbuf;
#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
	Bool dbeclear_p;
	XdbeBackBuffer backb;
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */

	XColor colors[255];
	XColor c_root[100];
	XColor c_root_oom[100];
	XColor c_nobody[100];
	XColor c_nobody_oom[100];
	XColor c_system[100];
	XColor c_system_oom[100];
	XColor c_sys[SYSMAX - SYSMIN + 1];
	XColor c_users[200];
	XColor c_users_oom[200];

	int c_user_current;
	int c_root_current;
	int c_system_current;
	int c_nobody_current;
	int ncolors;
	int max_depth;
	int min_height;
	int min_width;
	int line_width;

	XftFont *font;
	XftColor xft_fg;
	XftDraw *xftdraw;
	const char *s;
	int columns, rows;		/* characters */
	int left, right;		/* characters */
	int char_width, line_height;	/* pixels */
	int x, y;			/* characters */
	int mode;
	int hspace;			/* pixels */
	int vspace;			/* pixels */

	int delay;
	XWindowAttributes xgwa;
	GC fgc, bgc, textgc;

	int history_index;
	int history_index_last;

	int lastx;
	int currenty;

	int pan;
	int pandirection;
	int linger;
	int skipcount;
	int offbottom;

	enum detailstates detailstate;
	int detailpid;
	int detailsize;
	int showtime;

	void *pidtree;
	int nodecount;
	int nth;

};

static void walk_and_count(const void *what, const VISIT which, void *closure) {
	struct proc_t_history *pth;
	struct state *st;
	st = (struct state*) closure;
	pth = *(struct proc_t_history **)what;
	switch (which) {
		case preorder: return;
		case endorder: return;
		case postorder:
		case leaf: ;
	}
	if (!pth->visible) return;
	st->nodecount++;
}

static void walk_and_choose(const void *what, const VISIT which, void *closure) {
	struct proc_t_history *pth;
	struct state *st;
	st = (struct state*) closure;
	pth = *(struct proc_t_history **)what;
	switch (which) {
		case preorder: return;
		case endorder: return;
		case postorder:
		case leaf: ;
	}
	if (!pth->visible) return;
	st->nodecount++;
	if (st->nodecount == st->nth) {
		st->detailpid = pth->tid;
	}
}

static void walk_and_draw(const void *what, const VISIT which, void *closure){
	struct proc_t_history *pth;
	struct state *st;
	int i, ii, x, y, segw, height, spacing, totheight;
	int hsize, gap, viscount; /* variables  for bar segments */
	char text[1000] = {'\0'};
	char name[100] = {'\0'};
	int textsize;

	spacing = 3;

	st = (struct state*) closure;
	pth = *(struct proc_t_history **)what;

	switch (which) {
		case preorder: return;
		case endorder: return;
		case postorder:
		case leaf: ;
	}

	/* skip the "all zero" boring processes */
	if (pth->processes[st->history_index_last].rss == 0 ) return; 

	/* figure out height */
	if (pth->processes[st->history_index_last].rss > 100000) { height = 8; } 
	else if (pth->processes[st->history_index_last].rss > 10000) { height = 4; } else { height = 1; }
	totheight = height * 2 + spacing;

	y = st->currenty - st->pan;
	st->currenty += totheight;

	/* this pid is panned off top of screen */
	if (y + height < 0) {pth->visible = false; return;} 

	/* this is panned off the bottom; count how many are left undrawn */
	if (st->currenty > st->xgwa.height) { st->offbottom+=totheight; }

	if (y + height > st->xgwa.height) { pth->visible = false; return;} else { pth->visible = true; };


	if (pth->processes[st->history_index_last].uid == 0) {              /*root*/
		if (pth->processes[st->history_index_last].oom_score < 600)
		XSetForeground(st->dpy,st->fgc,st->c_root[st->c_root_current].pixel);
		else
		XSetForeground(st->dpy,st->fgc,st->c_root_oom[st->c_root_current].pixel);
		st->c_root_current++; 
		if (st->c_root_current++ >= 99) st->c_root_current = 0;
	} else if (pth->processes[st->history_index_last].uid == 65534)  {  /*nobody*/
		if (pth->processes[st->history_index_last].oom_score < 600)
		XSetForeground(st->dpy,st->fgc,st->c_nobody[st->c_nobody_current].pixel);
		else
		XSetForeground(st->dpy,st->fgc,st->c_nobody_oom[st->c_nobody_current].pixel);
		st->c_nobody_current++; 
		if (st->c_nobody_current++ >= 99) st->c_nobody_current = 0;
	} else if (pth->processes[st->history_index_last].uid < 1000)  {    /*system*/
		if (pth->processes[st->history_index_last].oom_score < 600)
		XSetForeground(st->dpy,st->fgc,st->c_system[st->c_system_current].pixel);
		else
		XSetForeground(st->dpy,st->fgc,st->c_system_oom[st->c_system_current].pixel);
		st->c_system_current++;
		if (st->c_system_current++ >= 99) st->c_system_current = 0;
	} else  {                                      /*users*/
		if (pth->processes[st->history_index_last].oom_score < 600)
		XSetForeground(st->dpy,st->fgc,st->c_users[st->c_user_current].pixel);
		else
		XSetForeground(st->dpy,st->fgc,st->c_users_oom[st->c_user_current].pixel);
		st->c_user_current++;
		if (st->c_user_current++ >= 99) st->c_user_current = 0;
	}

	hsize = 0;
	viscount = 0;
	for (ii=st->history_index_last; ii > st->history_index_last - MAXHIST; ii--) {
		i = (MAXHIST + ii) % MAXHIST;
		/*segw = log10(pth->processes[i].rss); */
		/*segw = segw * segw;*/
		segw = pth->processes[i].rss / st->xgwa.width;
		hsize += segw;
		viscount++;
		if (hsize > (st->xgwa.width)) { hsize-=segw; viscount--; break;};
	}
	if (viscount <= 0)  {
		viscount = 1;
		hsize = st->xgwa.width - 2;
		segw = hsize;
	}
		/*
		fprintf(stderr,"viscount: %i pid %i i %i ii %i segw %i hsize %i \n", 
			viscount, pth->tid, i, ii, segw, hsize);
			*/
	gap = (st->xgwa.width - hsize) / viscount;
	if (gap < 2) gap = 2;

	x = st->xgwa.width - (gap/2);
	for (ii=st->history_index_last; ii > st->history_index_last - MAXHIST; ii--) {
		i = (MAXHIST + ii) % MAXHIST;

		if (x <= 0) { break;}

		if (0 == pth->processes[i].rss) {
			segw = 1;
		} else {
			segw = pth->processes[i].rss / st->xgwa.width + 1;
		}

		/* Roughly...
		 R: ?????????
		 D: ?????????
		 Z: ???
		 T: ?????????
		 else: ?????????
		*/

		if (pth->processes[i].state == 'R') {          /*R = running  */
			XFillRectangle(st->dpy, st->b, st->fgc, 
					x - segw, y, 
					segw    , height) ;
			XFillRectangle(st->dpy, st->b, st->fgc, 
					x - segw + (segw / 3) , y + height + (height/2),
					(segw/3)    , (height/2)) ;

		} else if (pth->processes[i].state == 'D') {   /*D = uninterruptable sleep*/
			XFillRectangle(st->dpy, st->b, st->fgc, 
					x - segw + (segw/3), y + (height/2), 
					(segw/3)    , (height/2)) ;
			XFillRectangle(st->dpy, st->b, st->fgc, 
					x - segw, y+ height,
					segw    , height) ;

		} else if (pth->processes[i].state == 'Z') {           /*Z = zombie*/
			XFillRectangle(st->dpy, st->b, st->fgc, 
					x - segw - 1, y - 1, 
					segw + 1   , height * 2 + 1) ;
		} else if (pth->processes[i].state == 'T') {           /*T = suspended*/
			XDrawRectangle(st->dpy, st->b, st->fgc, 
					x - segw, y , 
					segw    , height * 2 ) ;
		}
		else {
			XFillRectangle(st->dpy, st->b, st->fgc, 
					x - segw, y + height,  
					segw    , height) ;
		}

		x -= (segw + gap);

	}

	if (st->detailpid == pth->tid) {
		switch (st->detailstate) {
			case waiting: 
				if (time(NULL) > st->showtime) { 
					st->detailstate = growing;
				}
				break;
			case growing:
				st->currenty += st->detailsize;
				if (st->detailsize < st->line_height) {
					st->detailsize++;
				} else {
					st->showtime = time(NULL) + 10;
					st->detailstate = showing;
				}
				break;
			case showing:
				st->currenty += st->detailsize;
				stat2name(pth->tid, name);
				/*fprintf(stderr,"got name %p %s\n", name, name);*/
				textsize = sprintf(text, "PID: %i UID: %i RSS: %lu VSIZE: %lu STATE: %c OOMSCORE: %i -- %s", 
						pth->tid, 
						pth->processes[st->history_index_last].uid, 
						pth->processes[st->history_index_last].rss, 
						pth->processes[st->history_index_last].vsize,
						pth->processes[st->history_index_last].state,
						pth->processes[st->history_index_last].oom_score,
						name
						);
				XftDrawStringUtf8 (st->xftdraw, &st->xft_fg, st->font,
						10, y + (height * 2) + st->line_height,
						(FcChar8 *) &text, textsize);
				if (time(NULL) > st->showtime) {
					st->detailstate = shrinking;
				}
				break;
			case shrinking:
				st->currenty += st->detailsize;
				st->detailsize--;
				if (st->detailsize <= 0) {
					st->detailstate = newpid;
					st->showtime = time(NULL) + 5;
				}
				break;
			case newpid:;
		}

	}


}

/*
 * debugging routine
static void print_proc(struct proc_t *p) {
	fprintf(stderr,"proc tid %i ppid %i uid %i rss %lu\n",
			p->tid, p->ppid, p->uid, p->rss);
}
static void print_pth(struct proc_t_history *pth){
	int i;
	fprintf(stderr,"pth tid %i present %d rss:", pth->tid, pth->present);
	for (i=0; i<MAXHIST; i++){
		fprintf(stderr, " %lu", pth->processes[i].rss);
	}
	fprintf(stderr,"\n");
}
*/

static int pid_compare(const void *a, const void *b ) {
	const struct proc_t_history *pa = a;
	const struct proc_t_history *pb = b;
	if (pa->tid > pb->tid) { return 1;}
	else if (pa->tid < pb->tid) { return -1;}
	else {return 0;}
}


static void
update_proctree(struct state *st) {

	int numprocs, i, j;
	struct proc_t_history **entry, *proto;
	proc_t processes[MAXPROCS];
	struct proc_t emptyproc;

	emptyproc.tid=0;
	emptyproc.ppid=0;
	emptyproc.uid=0;
	emptyproc.oom_score=0;
	emptyproc.oom_adj=0;
	emptyproc.rtprio=0;
	emptyproc.sched=0;
	emptyproc.tty=0;
	emptyproc.state='S';
	emptyproc.vsize=0;
	emptyproc.rss=0;

	numprocs = get_all_procs(processes, MAXPROCS);

	for(i=0; i<numprocs; i++){
		proto = calloc(1, sizeof *proto);
		proto->tid = processes[i].tid;
		if (proto->tid == 0) { continue;};
		proto->present = true;

		entry = tfind(proto, &st->pidtree, pid_compare);

		if (!entry) {
			for (j=0; j<MAXHIST; j++) { proto->processes[j] = emptyproc;}
			proto->processes[st->history_index] = processes[i];
			entry = tsearch(proto, &st->pidtree, pid_compare);
		} else {
			(*entry)->processes[st->history_index] = processes[i];
			free(proto);
		}

	}

	st->history_index_last = st->history_index;
	st->history_index++;
	if (st->history_index == MAXHIST) { st->history_index = 0;} 
}

	static void *
pidgrid_init (Display *dpy, Window window)
{
	int colorcount, hue;
	char *fontname, *colorname;

	struct state *st;
	XGCValues gcv;

	st = (struct state *) calloc (1, sizeof(*st));

	st->dpy = dpy;
	st->window = window;

	st->delay = get_integer_resource (st->dpy, "delay", "Integer");
	st->dbuf = get_boolean_resource (st->dpy, "doubleBuffer", "Boolean");

	XGetWindowAttributes (dpy, window, &st->xgwa);


# ifdef HAVE_JWXYZ	/* Don't second-guess Quartz's double-buffering */
	st->dbuf = False;
# endif

	if (st->dbuf)
	{
#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
		if (get_boolean_resource(st->dpy,"useDBE","Boolean"))
		{
			st->dbeclear_p = get_boolean_resource (st->dpy, "useDBEClear",
					"Boolean");
			if (st->dbeclear_p)
				st->b = xdbe_get_backbuffer (st->dpy, st->window, XdbeBackground);
			else
				st->b = xdbe_get_backbuffer (st->dpy, st->window, XdbeUndefined);
			st->backb = st->b;
		}
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */

		if (!st->b)
		{
			st->ba = XCreatePixmap (st->dpy, st->window, st->xgwa.width, st->xgwa.height,st->xgwa.depth);
			st->b = st->ba;
		}
	}
	else
	{
		st->b = st->window;
	}
	if (st->ba) XFillRectangle (st->dpy, st->ba, st->bgc, 0, 0, st->xgwa.width, st->xgwa.height);


	gcv.foreground = get_pixel_resource(dpy, st->xgwa.colormap, "foreground", "Foreground");
	gcv.background = get_pixel_resource(dpy, st->xgwa.colormap, "background", "Background");
	st->fgc = XCreateGC (st->dpy, st->b, GCForeground, &st->gcv);
	st->bgc = XCreateGC (st->dpy, st->b, GCForeground, &st->gcv);
	st->textgc = XCreateGC (st->dpy, st->b, GCForeground, &st->gcv);
	colorname = get_string_resource(st->dpy, "background","Background");
	/*if (!colorname) colorname = strdup("black");*/
	XftColorAllocName(st->dpy, st->xgwa.visual, st->xgwa.colormap, colorname, &st->xft_fg);
	XSetForeground(st->dpy, st->bgc, gcv.background);

	/*
	if (st->ncolors <= 2)
		mono_p = True;
	   if (!mono_p)
	   {
	   GC tmp = st->fgc;
	   st->fgc = st->bgc;
	   st->bgc = tmp;
	   }
	   */

	st->pandirection = 1;
	st->linger = 20;

	st->detailstate = newpid;
	st->detailpid = 1;
	st->detailsize = 0;
	st->showtime = time(NULL) + 5;

    fontname = get_string_resource (st->dpy, "font", "Font");
	if (!fontname) fontname = strdup("HeavyData Nerd Font 10");
	st->font = load_xft_font_retry(st->dpy, screen_number (st->xgwa.screen), fontname);
	if (!st->font) abort();
	if (fontname) free (fontname);

	colorname = get_string_resource(st->dpy, "foreground","Foreground");
	if (!colorname) colorname = strdup("white");
	XftColorAllocName(st->dpy, st->xgwa.visual, st->xgwa.colormap, colorname, &st->xft_fg);
	XSetForeground(st->dpy, st->textgc, gcv.background);
	XftColorAllocName (st->dpy, st->xgwa.visual, st->xgwa.colormap, colorname,
			&st->xft_fg);
	free (colorname);
	st->xftdraw = XftDrawCreate (dpy, st->b, st->xgwa.visual,
			st->xgwa.colormap);

   {
	   XGlyphInfo overall;
	   XftTextExtentsUtf8 (st->dpy, st->font, (FcChar8 *) "N", 1, &overall);
	   st->char_width = overall.xOff;
	   st->line_height = st->font->ascent + st->font->descent + 1;
   }

	st->c_user_current = 0;
	st->c_root_current = 0;
	st->c_system_current = 0;
	/*users*/
	colorcount=100;
	hue = get_integer_resource(st->dpy, "usersHue", "Integer");
	make_color_loop(st->xgwa.screen, st->xgwa.visual, st->xgwa.colormap,
			hue, 1, 1.0,
			hue, 1, 0.5,
			hue, 1, 0.4,
			st->c_users, &colorcount, true, false);
	colorcount=100;
	make_color_loop(st->xgwa.screen, st->xgwa.visual, st->xgwa.colormap,
			hue, 0.5, 1.0,
			hue, 0.5, 0.5,
			hue, 0.5, 0.4,
			st->c_users_oom, &colorcount, true, false);
	/*root*/
	colorcount=100;
	hue = get_integer_resource(st->dpy, "rootHue", "Integer");
	make_color_loop(st->xgwa.screen, st->xgwa.visual, st->xgwa.colormap,
			hue, 1, 1.0,
			hue, 1, 0.5,
			hue, 1, 0.4,
			st->c_root, &colorcount, true, false);
	colorcount=100;
	make_color_loop(st->xgwa.screen, st->xgwa.visual, st->xgwa.colormap,
			hue, 0.5, 1.0,
			hue, 0.5, 0.5,
			hue, 0.5, 0.4,
			st->c_root_oom, &colorcount, true, false);
	/*system*/
	colorcount=100;
	hue = get_integer_resource(st->dpy, "systemHue", "Integer");
	make_color_loop(st->xgwa.screen, st->xgwa.visual, st->xgwa.colormap,
			hue, 1.0, 1.0,
			hue, 1.0, 0.5,
			hue, 1.0, 0.4,
			st->c_system, &colorcount, true, false);
	colorcount=100;
	make_color_loop(st->xgwa.screen, st->xgwa.visual, st->xgwa.colormap,
			hue, 0.5, 1.0,
			hue, 0.5, 0.5,
			hue, 0.5, 0.4,
			st->c_system_oom, &colorcount, true, false);
	/*nobody*/
	colorcount=100;
	hue = get_integer_resource(st->dpy, "nobodyHue", "Integer");
	make_color_loop(st->xgwa.screen, st->xgwa.visual, st->xgwa.colormap,
			hue, 1.0, 1.0,
			hue, 1.0, 0.5,
			hue, 1.0, 0.4,
			st->c_nobody, &colorcount, true, false);
	colorcount=100;
	make_color_loop(st->xgwa.screen, st->xgwa.visual, st->xgwa.colormap,
			hue, 0.5, 1.0,
			hue, 0.5, 0.5,
			hue, 0.5, 0.4,
			st->c_nobody_oom, &colorcount, true, false);

	st->lastx = st->xgwa.width;
	st->currenty = 1;

	st->pidtree = NULL;
	update_proctree(st);

	return st;
}

	static unsigned long
pidgrid_draw (Display *dpy, Window window, void *closure)
{
	struct state *st;
	st = (struct state *) closure;

	XFillRectangle (dpy, st->b, st->bgc, 0, 0, st->xgwa.width, st->xgwa.height);

	/*
	   st->lastx = st->xgwa.width;
	   st->currenty = 1;
	   st->history[st->history_index].numprocs = get_all_procs(&st->history[st->history_index].processes);
	   */
	update_proctree(st);
	st->c_user_current = 0;
	st->c_root_current = 0;
	st->c_system_current = 0;
	st->currenty=0;
	st->skipcount=0;
	st->offbottom=0;
	twalk_r(st->pidtree,walk_and_draw,st); /* this is where drawing happens */

	if (st->offbottom > 0) {
		if (st->linger > 0) { st->linger--; }
		else {
			st->pan += st->pandirection;
			if (st->pan >= st->currenty - st->xgwa.height)
				{ st->pandirection = -1; st->linger=30; }
			if (st->pan <= 0 ) 
				{ st->pandirection = 1; st->linger=30;}
		}
	}

	if (st->detailstate == newpid ||
		st->showtime < time(NULL) - 15 ) {
		st->nodecount = 0;
		twalk_r(st->pidtree,walk_and_count,st); 

		st->nth = random()%st->nodecount;
		
		st->nodecount = 0;
		twalk_r(st->pidtree,walk_and_choose,st); 
		st->detailstate = waiting;
		st->showtime = time(NULL) + 5;
		
	}

#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
	if (st->backb)
	{
		XdbeSwapInfo info[1];
		info[0].swap_window = st->window;
		info[0].swap_action = (st->dbeclear_p ? XdbeBackground : XdbeUndefined);
		XdbeSwapBuffers (st->dpy, info, 1);
	}
	else
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */
		if (st->dbuf)
		{
			XCopyArea (st->dpy, st->b, st->window, st->bgc, 0, 0,
					st->xgwa.width, st->xgwa.height, 0, 0);
		}


	return 10000 * st->delay;
}

	static void
pidgrid_reshape (Display *dpy, Window window, void *closure, 
		unsigned int w, unsigned int h)
{
	struct state *st = (struct state *) closure;
	st->xgwa.width = w;
	st->xgwa.height = h;
}

	static Bool
pidgrid_event (Display *dpy, Window window, void *closure, XEvent *event)
{
	return False;
}

	static void
pidgrid_free (Display *dpy, Window window, void *closure)
{
	struct state *st = (struct state *) closure;
	XFreeGC (dpy, st->fgc);
	XFreeGC (dpy, st->bgc);
	tdestroy(st->pidtree, free);
	free (st);
}


static const char *pidgrid_defaults [] = {
	".background:		black",
	".foreground:		white",
	".usersHue:		    120",
	".rootHue:		    270",
	".systemHue:		250",
	".nobodyHue:		50",
	".delay:		    5",
	".font:		        HeavyData Nerd Font 10",
#ifdef HAVE_MOBILE
	"*ignoreRotation:     True",
#endif
#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
	".doubleBuffer: True",
	"*useDBEClear:	True",
	"*useDBE:		True",
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */
	0
};

static XrmOptionDescRec pidgrid_options [] = {
	{ "-delay",		".delay",	XrmoptionSepArg, 0 },
    { "-db",		".doubleBuffer", XrmoptionNoArg,  "True" },
    { "-no-db",		".doubleBuffer", XrmoptionNoArg,  "False" },
    { "-db",		".doubleBuffer", XrmoptionNoArg,  "True" },
    { "-background",".background", XrmoptionSepArg,  0 },
    { "-foreground",".foreground", XrmoptionSepArg,  0 },
    { "-users",		".usersHue", XrmoptionSepArg,  0 },
    { "-root",		".rootHue", XrmoptionSepArg,  0 },
    { "-system",	".systemHue", XrmoptionSepArg,  0 },
    { "-nobody",	".nobodyHue", XrmoptionSepArg,  0 },
    { "-font",	".font", XrmoptionSepArg,  0 },
	{ 0, 0, 0, 0 }
};

XSCREENSAVER_MODULE ("PidGrid", pidgrid)
