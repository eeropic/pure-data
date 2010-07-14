/*
  pd~.c - embed a Pd process within Pd or Max.

  Copyright 2008 Miller Puckette
  BSD license; see README.txt in this distribution for details.
*/

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#ifdef NT
#pragma warning (disable: 4305 4244)
#endif

#ifdef MSP
#include "ext.h"
#include "z_dsp.h"
#include "math.h"
#include "ext_support.h"
#include "ext_proto.h"
#include "ext_obex.h"

typedef double t_floatarg;
#define w_symbol w_sym
#define A_SYMBOL A_SYM
#define getbytes t_getbytes
#define freebytes t_freebytes
#define ERROR error(
void *pd_tilde_class;
#define MAXPDSTRING 4096
#define DEFDACBLKSIZE 64
#endif /* MSP */

#ifdef PD
#include "m_pd.h"
#include "s_stuff.h"
static t_class *pd_tilde_class;
char *class_gethelpdir(t_class *c);
#define ERROR pd_error(x, 

#endif

#if defined(__linux__) || defined(__FreeBSD_kernel__) || defined(__GNU__)
#ifdef __x86_64__
static char pd_tilde_dllextent[] = ".l_ia64",
    pd_tilde_dllextent2[] = ".pd_linux";
#else
static char pd_tilde_dllextent[] = ".l_i386",
    pd_tilde_dllextent2[] = ".pd_linux";
#endif
#endif
#ifdef __APPLE__
static char pd_tilde_dllextent[] = ".d_fat",
    pd_tilde_dllextent2[] = ".pd_darwin";
#endif
#if defined(_WIN32) || defined(__CYGWIN__)
static char pd_tilde_dllextent[] = ".m_i386", pd_tilde_dllextent2[] = ".dll";
#endif

/* ------------------------ pd_tilde~ ----------------------------- */

#define MSGBUFSIZE 65536

typedef struct _pd_tilde
{
#ifdef PD
    t_object x_obj;
    t_clock *x_clock;
    t_outlet *x_outlet1;        /* for messages back from subproc */
    t_canvas *x_canvas;
#endif /* PD */
#ifdef MSP
    t_pxobject x_obj;
    void *x_outlet1;
    void *x_clock;
#endif /* MSP */
    FILE *x_infd;
    FILE *x_outfd;
    char *x_msgbuf;
    int x_msgbufsize;
    int x_infill;
    int x_childpid;
    int x_ninsig;
    int x_noutsig;
    int x_fifo;
    float x_sr;
    t_symbol *x_pddir;
    t_symbol *x_schedlibdir;
    t_sample **x_insig;
    t_sample **x_outsig;
} t_pd_tilde;

#ifdef MSP
static void *pd_tilde_new(t_symbol *s, long ac, t_atom *av);
static void pd_tilde_tick(t_pd_tilde *x);
static t_int *pd_tilde_perform(t_int *w);
static void pd_tilde_dsp(t_pd_tilde *x, t_signal **sp);
void pd_tilde_assist(t_pd_tilde *x, void *b, long m, long a, char *s);
static void pd_tilde_free(t_pd_tilde *x);
void pd_tilde_setup(void);
int main();
void pd_tilde_minvel_set(t_pd_tilde *x, void *attr, long ac, t_atom *av);
char *strcpy(char *s1, const char *s2);
#endif

static void pd_tilde_tick(t_pd_tilde *x);
static void pd_tilde_close(t_pd_tilde *x)
{
    if (x->x_outfd)
        fclose(x->x_outfd);
    if (x->x_infd)
        fclose(x->x_infd);
    if (x->x_childpid > 0)
        waitpid(x->x_childpid, 0, 0);
    if (x->x_msgbuf)
        free(x->x_msgbuf);
    x->x_infd = x->x_outfd = 0;
    x->x_childpid = -1;
    x->x_msgbuf = 0;
    x->x_msgbufsize = 0;
}

static void pd_tilde_readmessages(t_pd_tilde *x)
{
    int gotsomething = 0, setclock = 0, wasempty = (x->x_infill == 0);
    FILE *infd = x->x_infd;
    while (1)
    {
        int c = getc(infd);
        if (c == EOF)
        {
            ERROR "pd~: %s", strerror(errno));
            pd_tilde_close(x);
            break;
        }
        if (x->x_infill >= x->x_msgbufsize)
        {
            char *z = realloc(x->x_msgbuf, x->x_msgbufsize+MSGBUFSIZE);
            if (!z)
            {
                ERROR "pd~: failed to grow input buffer");
                pd_tilde_close(x);
                break;
            }
            x->x_msgbuf = z;
            x->x_msgbufsize += MSGBUFSIZE;
        }
        x->x_msgbuf[x->x_infill++] = c;
        if (c == ';')
        {
            if (!gotsomething)
                break;
            gotsomething = 0;
        }
        else if (!isspace(c))
            gotsomething = setclock = 1;
    }
    if (setclock)
        clock_delay(x->x_clock, 0);
    else if (wasempty)
        x->x_infill = 0;
}

static void pd_tilde_donew(t_pd_tilde *x, char *pddir, char *schedlibdir,
    char *patchdir, char *pdargs, int ninsig, int noutsig, int fifo,
    float samplerate)
{
    int i, pid, pipe1[2], pipe2[2];
    char cmdbuf[MAXPDSTRING], pdexecbuf[MAXPDSTRING], schedbuf[MAXPDSTRING];
    struct stat statbuf;
    x->x_infd = x->x_outfd = 0;
    x->x_childpid = -1;
    snprintf(pdexecbuf, MAXPDSTRING, "%s/bin/pd", pddir);
    if (stat(pdexecbuf, &statbuf) < 0)
    {
        snprintf(pdexecbuf, MAXPDSTRING, "%s/../../../bin/pd", pddir);
        if (stat(pdexecbuf, &statbuf) < 0)
        {
            snprintf(pdexecbuf, MAXPDSTRING, "%s/pd", pddir);
            if (stat(pdexecbuf, &statbuf) < 0)
            {
                ERROR "pd~: can't stat %s", pdexecbuf);
                goto fail1;
            }
        }
    }
    snprintf(schedbuf, MAXPDSTRING, "%s/pdsched%s", schedlibdir, 
        pd_tilde_dllextent);
    if (stat(schedbuf, &statbuf) < 0)
    {
        snprintf(schedbuf, MAXPDSTRING, "%s/pdsched%s", schedlibdir, 
            pd_tilde_dllextent2);
        if (stat(schedbuf, &statbuf) < 0)
        {
            ERROR "pd~: can't stat %s", schedbuf);
            goto fail1;
        }       
    }
    snprintf(cmdbuf, MAXPDSTRING,
"%s -schedlib %s/pdsched -path %s -inchannels %d -outchannels %d -r %g %s\n",
        pdexecbuf, schedlibdir, patchdir, ninsig, noutsig, samplerate, pdargs);
#if 0
#ifdef PD
    fprintf(stderr, "%s", cmdbuf);
#endif
    post("cmd: %s", cmdbuf);
#endif
    if (pipe(pipe1) < 0)   
    {
        ERROR "pd~: can't create pipe");
        goto fail1;
    }
    if (pipe(pipe2) < 0)   
    {
        ERROR "pd~: can't create pipe");
        goto fail2;
    }
    if ((pid = fork()) < 0)
    {
        ERROR "pd~: can't fork");
        goto fail3;
    }
    else if (pid == 0)
    {
        /* child process */
        if (pipe2[1] == 0)
        {
            dup2(pipe2[1], 20);
            close(pipe2[1]);
            pipe2[1] = 20;
        }
        dup2(pipe1[0], 0);
        dup2(pipe2[1], 1);
        if (pipe1[0] >= 2)
            close(pipe1[0]);
        if (pipe1[1] >= 2)
            close(pipe1[1]);
        if (pipe2[0] >= 2)
            close(pipe2[0]);
        if (pipe2[1] >= 2)
            close(pipe2[1]);
        execl("/bin/sh", "sh", "-c", cmdbuf, (char*)0);
        _exit(1);
    }
        /* OK, we're parent */
    close(pipe1[0]);
    close(pipe2[1]);
    x->x_outfd = fdopen(pipe1[1], "w");
    x->x_infd = fdopen(pipe2[0], "r");
    x->x_childpid = pid;
    for (i = 0; i < fifo; i++)
        fprintf(x->x_outfd, "%s", ";\n0;\n");
    fflush(x->x_outfd);
    if (!(x->x_msgbuf = calloc(MSGBUFSIZE, 1)))
    {
        ERROR "pd~: can't allocate message buffer");
        goto fail3;
    }
    x->x_msgbufsize = MSGBUFSIZE;
    x->x_infill = 0;
    /* fprintf(stderr, "read...\n"); */
    pd_tilde_readmessages(x);
    /* fprintf(stderr, "... done.\n"); */
    return;
fail3:
    close(pipe2[0]);
    close(pipe2[1]);
    if (x->x_childpid > 0)
        waitpid(x->x_childpid, 0, 0);
fail2:
    close(pipe1[0]);
    close(pipe1[1]);
fail1:
    x->x_infd = x->x_outfd = 0;
    x->x_childpid = -1;
    return;
}

static t_int *pd_tilde_perform(t_int *w)
{
    t_pd_tilde *x = (t_pd_tilde *)(w[1]);
    int n = (int)(w[2]), i, j, numbuffill = 0, c;
    char numbuf[80];
    FILE *infd = x->x_infd;
    if (!infd)
        goto zeroit;
    fprintf(x->x_outfd, ";\n");
    if (!x->x_ninsig)
        fprintf(x->x_outfd, "0\n");
    else for (i = 0; i < x->x_ninsig; i++)
    {
        t_sample *fp = x->x_insig[i];
        for (j = 0; j < n; j++)
            fprintf(x->x_outfd, "%g\n", *fp++);
        for (; j < DEFDACBLKSIZE; j++)
            fprintf(x->x_outfd, "0\n");
    }
    fprintf(x->x_outfd, ";\n");
    fflush(x->x_outfd);
    i = j = 0;
    while (1)
    {
        while (1)
        {
            c = getc(infd);
            if (c == EOF)
            {
                if (errno)
                    ERROR "pd~: %s", strerror(errno));
                else ERROR "pd~: subprocess exited");
                pd_tilde_close(x);
                goto zeroit;
            }
            else if (!isspace(c) && c != ';')
            {
                if (numbuffill < (80-1))
                    numbuf[numbuffill++] = c;
            }
            else
            {
                t_sample z;
                if (numbuffill)
                {
                    numbuf[numbuffill] = 0;
                    if (sscanf(numbuf, "%f", &z) < 1)
                        continue;
                    if (i < x->x_noutsig)
                        x->x_outsig[i][j] = z;
                    if (++j >= DEFDACBLKSIZE)
                        j = 0, i++;
                }
                numbuffill = 0;
                break;
            }
        }
        /* message terminated */
        if (c == ';')
            break;
    }
    for (; i < x->x_noutsig; i++, j = 0)
    {
        for (; j < DEFDACBLKSIZE; j++)
            x->x_outsig[i][j] = 0;
    }
    pd_tilde_readmessages(x);
    return (w+3);
zeroit:
    for (i = 0; i < x->x_noutsig; i++)
    {
        for (j = 0; j < DEFDACBLKSIZE; j++)
            x->x_outsig[i][j] = 0;
    }
    return (w+3);
}

static void pd_tilde_dsp(t_pd_tilde *x, t_signal **sp)
{
    int i, n = (x->x_ninsig || x->x_noutsig ? sp[0]->s_n : 1);
    t_sample **g;
        
    for (i = 0, g = x->x_insig; i < x->x_ninsig; i++, g++)
        *g = (*(sp++))->s_vec;
    
    for (i = 0, g = x->x_outsig; i < x->x_noutsig; i++, g++)
        *g = (*(sp++))->s_vec;
    
    dsp_add(pd_tilde_perform, 2, x, n);
}

static void pd_tilde_pdtilde(t_pd_tilde *x, t_symbol *s,
    int argc, t_atom *argv)
{
    t_symbol *sel = ((argc > 0 && argv->a_type == A_SYMBOL) ?
        argv->a_w.w_symbol : gensym("?")), *schedlibdir;
    char *patchdir;
    if (sel == gensym("start"))
    {
        char pdargstring[MAXPDSTRING];
        if (x->x_infd)
            pd_tilde_close(x);
        pdargstring[0] = 0;
        argc--; argv++;
#ifdef PD
        while (argc--)
        {
            atom_string(argv++, pdargstring + strlen(pdargstring), 
                MAXPDSTRING - strlen(pdargstring));
            if (strlen(pdargstring) < MAXPDSTRING-1)
                strcat(pdargstring, " ");
        }
        patchdir = canvas_getdir(x->x_canvas)->s_name;
#endif
#ifdef MSP
        while (argc--)
        {
                /* because Mac pathnames sometimes have an evil preceeding
                colon character, we test for and silently eat them */
            if (argv->a_type == A_SYM)
                strncat(pdargstring, (*argv->a_w.w_sym->s_name == ':'?
                    argv->a_w.w_sym->s_name+1 : argv->a_w.w_sym->s_name),
                    MAXPDSTRING - strlen(pdargstring)-3);
            else if (argv->a_type == A_LONG)
                snprintf(pdargstring+strlen(pdargstring),
                    MAXPDSTRING - strlen(pdargstring)-3, "%ld",
                        argv->a_w.w_long);
            else if (argv->a_type == A_FLOAT)
                snprintf(pdargstring+strlen(pdargstring),
                    MAXPDSTRING - strlen(pdargstring)-3, "%f",
                        argv->a_w.w_float);
            strcat(pdargstring, " ");
            argv++;
        }
        patchdir = ".";
#endif
        schedlibdir = x->x_schedlibdir;
        if (schedlibdir == gensym(".") && x->x_pddir != gensym("."))
        {
            char *pds = x->x_pddir->s_name, scheddirstring[MAXPDSTRING];
            int l = strlen(pds);
            if (l >= 4 && (!strcmp(pds+l-3, "bin") || !strcmp(pds+l-4, "bin/")))
                snprintf(scheddirstring, MAXPDSTRING, "%s/../extra/pd~", pds);
            else snprintf(scheddirstring, MAXPDSTRING, "%s/extra/pd~", pds);
            schedlibdir = gensym(scheddirstring);
        }
        pd_tilde_donew(x, x->x_pddir->s_name, schedlibdir->s_name,
            patchdir, pdargstring, x->x_ninsig, x->x_noutsig, x->x_fifo,
                x->x_sr);
    }
    else if (sel == gensym("stop"))
    {
        if (x->x_infd)
            pd_tilde_close(x);
    }
    else if (sel == gensym("pddir"))
    {
        if ((argc > 1) && argv[1].a_type == A_SYMBOL)
        {
            t_symbol *sym = argv[1].a_w.w_symbol;
#ifdef MSP
            if (sym->s_name[0] == ':')
                sym = gensym(s->s_name+1);
#endif
            x->x_pddir = sym;
        }
        else ERROR "pd~ pddir: needs symbol argument");
    }
    else ERROR "pd~: unknown control message: %s", sel->s_name);
}

static void pd_tilde_free(t_pd_tilde *x)
{
#ifdef MSP
    dsp_free((t_pxobject *)x);
#endif
    pd_tilde_close(x);
    clock_free(x->x_clock);
}

/* -------------------------- Pd glue ------------------------- */
#ifdef PD

static void pd_tilde_tick(t_pd_tilde *x)
{
    int messstart = 0, i, n;
    t_atom *vec;
    t_binbuf *b;
    if (!x->x_msgbuf)
        return;
    b = binbuf_new();
    binbuf_text(b, x->x_msgbuf, x->x_infill);
    /* binbuf_print(b); */
    n = binbuf_getnatom(b);
    vec = binbuf_getvec(b);
    for (i = 0; i < n; i++)
    {
        if (vec[i].a_type == A_SEMI)
        {
            if (i > messstart && vec[messstart].a_type == A_SYMBOL)
                outlet_anything(x->x_outlet1, vec[messstart].a_w.w_symbol,
                    i-(messstart+1), vec+(messstart+1));
            else if (i > messstart)
                outlet_list(x->x_outlet1, 0, i-messstart, vec+messstart);
            messstart = i+1;
        }
    }
    binbuf_free(b);
    x->x_infill = 0;
}

static void pd_tilde_anything(t_pd_tilde *x, t_symbol *s,
    int argc, t_atom *argv)
{
    char msgbuf[MAXPDSTRING];
    if (!x->x_outfd)
        return;
    fprintf(x->x_outfd, "%s ", s->s_name);
    while (argc--)
    {
        atom_string(argv++, msgbuf, MAXPDSTRING);
        fprintf(x->x_outfd, "%s ", msgbuf);
    }
    fprintf(x->x_outfd, ";\n");
}

static void *pd_tilde_new(t_symbol *s, int argc, t_atom *argv)
{
    t_pd_tilde *x = (t_pd_tilde *)pd_new(pd_tilde_class);
    int ninsig = 2, noutsig = 2, j, fifo = 5;
    float sr = sys_getsr();
    t_sample **g;
    t_symbol *pddir = sys_libdir,
        *scheddir = gensym(class_gethelpdir(pd_tilde_class));
    /* fprintf(stderr, "pd %s, sched %s\n", pddir->s_name, scheddir->s_name); */
    while (argc > 0)
    {
        t_symbol *firstarg = atom_getsymbolarg(0, argc, argv);
        if (!strcmp(firstarg->s_name, "-sr") && argc > 1)
        {
            sr = atom_getfloatarg(1, argc, argv);
            argc -= 2; argv += 2;
        }
        else if (!strcmp(firstarg->s_name, "-ninsig") && argc > 1)
        {
            ninsig = atom_getfloatarg(1, argc, argv);
            argc -= 2; argv += 2;
        }
        else if (!strcmp(firstarg->s_name, "-noutsig") && argc > 1)
        {
            noutsig = atom_getfloatarg(1, argc, argv);
            argc -= 2; argv += 2;
        }
        else if (!strcmp(firstarg->s_name, "-fifo") && argc > 1)
        {
            fifo = atom_getfloatarg(1, argc, argv);
            argc -= 2; argv += 2;
        }
        else if (!strcmp(firstarg->s_name, "-pddir") && argc > 1)
        {
            pddir = atom_getsymbolarg(1, argc, argv);
            argc -= 2; argv += 2;
        }
        else if (!strcmp(firstarg->s_name, "-scheddir") && argc > 1)
        {
            scheddir = atom_getsymbolarg(1, argc, argv);
            argc -= 2; argv += 2;
        }
        else break;
    }

    if (argc)
    {
        pd_error(x,
"usage: pd~ [-sr #] [-ninsig #] [-noutsig #] [-fifo #] [-pddir <>]");
        post(
"... [-scheddir <>]");
    }

    x->x_clock = clock_new(x, (t_method)pd_tilde_tick);
    x->x_insig = (t_sample **)getbytes(ninsig * sizeof(*x->x_insig));
    x->x_outsig = (t_sample **)getbytes(noutsig * sizeof(*x->x_outsig));
    x->x_ninsig = ninsig;
    x->x_noutsig = noutsig;
    x->x_fifo = fifo;
    x->x_sr = sr;
    x->x_pddir = pddir;
    x->x_schedlibdir = scheddir;
    x->x_infd = 0;
    x->x_outfd = 0;
    x->x_outfd = 0;
    x->x_childpid = -1;
    x->x_msgbuf = 0;
    x->x_canvas = canvas_getcurrent();
    for (j = 1, g = x->x_insig; j < ninsig; j++, g++)
        inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    x->x_outlet1 = outlet_new(&x->x_obj, 0);
    for (j = 0, g = x->x_outsig; j < noutsig; j++, g++)
        outlet_new(&x->x_obj, &s_signal);
    signal(SIGPIPE, SIG_IGN);

    return (x);
}

void pd_tilde_setup(void)
{
    pd_tilde_class = class_new(gensym("pd~"), (t_newmethod)pd_tilde_new,
        (t_method)pd_tilde_free, sizeof(t_pd_tilde), 0, A_GIMME, 0);
    class_addmethod(pd_tilde_class, nullfn, gensym("signal"), 0);
    class_addmethod(pd_tilde_class, (t_method)pd_tilde_dsp, gensym("dsp"), 0);
    class_addmethod(pd_tilde_class, (t_method)pd_tilde_pdtilde, gensym("pd~"), A_GIMME, 0);
    class_addanything(pd_tilde_class, pd_tilde_anything);
    post("pd~ version 0.2");
}
#endif

/* -------------------------- MSP glue ------------------------- */
#ifdef MSP

#define LOTS 10000

static void pd_tilde_tick(t_pd_tilde *x)
{
    int messstart = 0, i, n = 0;
    t_atom vec[LOTS];
    long z1 = 0, z2 = 0;
    void *b;
    if (!x->x_msgbuf)
        return;
    b = binbuf_new();
    binbuf_text(b, &x->x_msgbuf, x->x_infill);
    /* binbuf_print(b); */
    while (!binbuf_getatom(b, &z1, &z2, vec+n))
    if (++n >= LOTS)
        break;
    for (i = 0; i < n; i++)
    {
        if (vec[i].a_type == A_SEMI)
        {
            if (i > messstart + 1)
            {
                void *whom;
                if (vec[messstart].a_type == A_SYM)
                    outlet_anything(x->x_outlet1, vec[messstart].a_w.w_sym,
                        i-messstart-1, vec+(messstart+1));
                else if (vec[messstart].a_type == A_FLOAT && i == messstart+1)
                    outlet_float(x->x_outlet1, vec[messstart].a_w.w_float);
                else if (vec[messstart].a_type == A_LONG && i == messstart+1)
                    outlet_int(x->x_outlet1, vec[messstart].a_w.w_long);
                else outlet_list(x->x_outlet1, gensym("list"),
                    i-messstart, vec+(messstart));
            }
            messstart = i+1;
        }
    }
    binbuf_free(b);
    x->x_infill = 0;
}

static void pd_tilde_anything(t_pd_tilde *x, t_symbol *s,
    long ac, t_atom *av)
{
    char msgbuf[MAXPDSTRING], *sp, *ep = msgbuf+MAXPDSTRING;
    if (!x->x_outfd)
        return;
    msgbuf[0] = 0;
    strncpy(msgbuf, s->s_name, MAXPDSTRING);
    msgbuf[MAXPDSTRING-1] = 0;
    sp = msgbuf + strlen(msgbuf);
    while (ac--)
    {
        if (sp < ep-1)
            sp[0] = ' ', sp[1] = 0, sp++;
        if (sp < ep - 80)
        {
            if (av->a_type == A_SYM && strlen(av->a_w.w_sym->s_name) < ep - sp-20)
                strcpy(sp, av->a_w.w_sym->s_name);
            else if (av->a_type == A_LONG)
                sprintf(sp, "%ld" ,av->a_w.w_long);
            else if (av->a_type == A_FLOAT)
                sprintf(sp, "%g" ,av->a_w.w_float);
        }
        sp += strlen(sp);
        av++;
    }
    fprintf(x->x_outfd, "%s;\n", msgbuf);
}

int main()
{       
    t_class *c;

    c = class_new("pd_tilde~", (method)pd_tilde_new, (method)pd_tilde_free, sizeof(t_pd_tilde), (method)0L, A_GIMME, 0);

    class_addmethod(c, (method)pd_tilde_dsp, "dsp", A_CANT, 0);
    class_addmethod(c, (method)pd_tilde_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)pd_tilde_pdtilde, "pd~", A_GIMME, 0);
    class_addmethod(c, (method)pd_tilde_anything, "anything", A_GIMME, 0);
    class_dspinit(c);

    class_register(CLASS_BOX, c);
    pd_tilde_class = c;
    post("pd~ version 0.2");
    return (0);
}

static void *pd_tilde_new(t_symbol *s, long ac, t_atom *av)
{
    int ninsig = 2, noutsig = 2, fifo = 5, j;
    float sr = sys_getsr();
    t_symbol *pddir = gensym("."), *scheddir = gensym(".");
    t_pd_tilde *x;

    if (x = (t_pd_tilde *)object_alloc(pd_tilde_class))
    {
        while (ac > 0 && av[0].a_type == A_SYM)
        {
            char *flag = av[0].a_w.w_sym->s_name;
            if (!strcmp(flag, "-sr") && ac > 1)
            {
                sr = (av[1].a_type == A_FLOAT ? av[1].a_w.w_float :
                    (av[1].a_type == A_LONG ? av[1].a_w.w_long : 0));
                ac -= 2; av += 2;
            }
            else if (!strcmp(flag, "-ninsig") && ac > 1)
            {
                ninsig = (av[1].a_type == A_FLOAT ? av[1].a_w.w_float :
                    (av[1].a_type == A_LONG ? av[1].a_w.w_long : 0));
                ac -= 2; av += 2;
            }
            else if (!strcmp(flag, "-noutsig") && ac > 1)
            {
                noutsig = (av[1].a_type == A_FLOAT ? av[1].a_w.w_float :
                    (av[1].a_type == A_LONG ? av[1].a_w.w_long : 0));
                ac -= 2; av += 2;
            }
            else if (!strcmp(flag, "-fifo") && ac > 1)
            {
                fifo = (av[1].a_type == A_FLOAT ? av[1].a_w.w_float :
                    (av[1].a_type == A_LONG ? av[1].a_w.w_long : 0));
                ac -= 2; av += 2;
            }
            else if (!strcmp(flag, "-pddir") && ac > 1)
            {
                pddir = (av[1].a_type == A_SYM ? av[1].a_w.w_sym : gensym("."));
                ac -= 2; av += 2;
            }
            else if (!strcmp(flag, "-scheddir") && ac > 1)
            {
                scheddir = (av[1].a_type == A_SYM ? av[1].a_w.w_sym : gensym("."));
                ac -= 2; av += 2;
            }
            else break;
        }
        if (ac)
            post("pd~: warning: ignoring extra arguments");
        dsp_setup((t_pxobject *)x, ninsig);
        x->x_outlet1 = outlet_new(&x->x_obj, 0);
        for (j = 0; j < noutsig; j++)
            outlet_new((t_pxobject *)x, "signal");
        x->x_clock = clock_new(x, (method)pd_tilde_tick);
        x->x_insig = (t_sample **)getbytes(ninsig * sizeof(*x->x_insig));
        x->x_outsig = (t_sample **)getbytes(noutsig * sizeof(*x->x_outsig));
        x->x_ninsig = ninsig;
        x->x_noutsig = noutsig;
        x->x_fifo = fifo;
        x->x_sr = sr;
        x->x_pddir = pddir;
        x->x_schedlibdir = scheddir;
        x->x_infd = 0;
        x->x_outfd = 0;
        x->x_outfd = 0;
        x->x_childpid = -1;
        x->x_msgbuf = 0;
    }
    return (x);
}

void pd_tilde_assist(t_pd_tilde *x, void *b, long m, long a, char *s)
{
}

#endif /* MSP */
