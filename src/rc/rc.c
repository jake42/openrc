/*
   rc.c
   rc - manager for init scripts which control the startup, shutdown
   and the running of daemons.

   Also a multicall binary for various commands that can be used in shell
   scripts to query service state, mark service state and provide the
   einfo family of informational functions.
   */

/*
 * Copyright 2007-2008 Roy Marples
 * All rights reserved

 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

const char rc_copyright[] = "Copyright (c) 2007-2008 Roy Marples";

#define SYSLOG_NAMES

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <errno.h>
#include <dirent.h>
#include <ctype.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <termios.h>
#include <unistd.h>

#include "builtins.h"
#include "einfo.h"
#include "rc.h"
#include "rc-logger.h"
#include "rc-misc.h"
#include "rc-plugin.h"
#include "strlist.h"

#include "version.h"

#define INITSH                  RC_LIBDIR "/sh/init.sh"
#define INITEARLYSH             RC_LIBDIR "/sh/init-early.sh"
#define HALTSH                  RC_INITDIR "/halt.sh"

#define SHUTDOWN                "/sbin/shutdown"
#define SULOGIN                 "/sbin/sulogin"

#define INTERACTIVE             RC_SVCDIR "/interactive"

#define DEVBOOT					"/dev/.rcboot"

/* Cleanup anything in main */
#define CHAR_FREE(_item) if (_item) { \
	free (_item); \
	_item = NULL; \
}

extern char **environ;

static char *RUNLEVEL = NULL;
static char *PREVLEVEL = NULL;

static const char *applet = NULL;
static char *runlevel = NULL;
static char **env = NULL;
static char **newenv = NULL;
static char **coldplugged_services = NULL;
static char **stop_services = NULL;
static char **start_services = NULL;
static rc_depinfo_t *deptree = NULL;
static char *tmp = NULL;

struct termios *termios_orig = NULL;

typedef struct pidlist
{
	pid_t pid;
	struct pidlist *next;
} pidlist_t;
static pidlist_t *service_pids = NULL;

static const char *const types_n[] = { "needsme", NULL };
static const char *const types_nua[] = { "ineed", "iuse", "iafter", NULL };

static void clean_failed (void)
{
	DIR *dp;
	struct dirent *d;
	int i;
	char *path;

	/* Clean the failed services state dir now */
	if ((dp = opendir (RC_SVCDIR "/failed"))) {
		while ((d = readdir (dp))) {
			if (d->d_name[0] == '.' &&
			    (d->d_name[1] == '\0' ||
			     (d->d_name[1] == '.' && d->d_name[2] == '\0')))
				continue;

			i = strlen (RC_SVCDIR "/failed/") + strlen (d->d_name) + 1;
			path = xmalloc (sizeof (char) * i);
			snprintf (path, i, RC_SVCDIR "/failed/%s", d->d_name);
			if (path) {
				if (unlink (path))
					eerror ("%s: unlink `%s': %s", applet, path,
						strerror (errno));
				free (path);
			}
		}
		closedir (dp);
	}
}

static void cleanup (void)
{
	if (applet && strcmp (applet, "rc") == 0) {
		pidlist_t *pl = service_pids;

		rc_plugin_unload ();

		if (! rc_in_plugin && termios_orig) {
			tcsetattr (fileno (stdin), TCSANOW, termios_orig);
			free (termios_orig);
		}

		while (pl) {
			pidlist_t *p = pl->next;
			free (pl);
			pl = p;
		}

		rc_strlist_free (env);
		rc_strlist_free (newenv);
		rc_strlist_free (coldplugged_services);
		rc_strlist_free (stop_services);
		rc_strlist_free (start_services);
		rc_deptree_free (deptree);

		/* Clean runlevel start, stop markers */
		if (! rc_in_plugin && ! rc_in_logger) {
			rmdir (RC_STARTING);
			rmdir (RC_STOPPING);
			clean_failed ();

			rc_logger_close ();
		}

		free (runlevel);
	}
}

static int syslog_decode (char *name, CODE *codetab)
{
	CODE *c;

	if (isdigit ((int) *name))
		return (atoi (name));

	for (c = codetab; c->c_name; c++)
		if (! strcasecmp (name, c->c_name))
			return (c->c_val);

	return (-1);
}

static int do_e (int argc, char **argv)
{
	int retval = EXIT_SUCCESS;
	int i;
	int l = 0;
	char *message = NULL;
	char *p;
	int level = 0;

	if (strcmp (applet, "eval_ecolors") == 0) {
		printf ("GOOD='%s'\nWARN='%s'\nBAD='%s'\nHILITE='%s'\nBRACKET='%s'\nNORMAL='%s'\n",
			ecolor (ECOLOR_GOOD),
			ecolor (ECOLOR_WARN),
			ecolor (ECOLOR_BAD),
			ecolor (ECOLOR_HILITE),
			ecolor (ECOLOR_BRACKET),
			ecolor (ECOLOR_NORMAL));
		exit (EXIT_SUCCESS);
	}

	if (argc > 0) {

		if (strcmp (applet, "eend") == 0 ||
		    strcmp (applet, "ewend") == 0 ||
		    strcmp (applet, "veend") == 0 ||
		    strcmp (applet, "vweend") == 0)
		{
			errno = 0;
			retval = strtol (argv[0], NULL, 0);
			if (errno != 0)
				retval = EXIT_FAILURE;
			else {
				argc--;
				argv++;
			}
		} else if (strcmp (applet, "esyslog") == 0 ||
			   strcmp (applet, "elog") == 0) {
			char *dot = strchr (argv[0], '.');
			if ((level = syslog_decode (dot + 1, prioritynames)) == -1)
				eerrorx ("%s: invalid log level `%s'", applet, argv[0]);

			if (argc < 3)
				eerrorx ("%s: not enough arguments", applet);

			unsetenv ("EINFO_LOG");
			setenv ("EINFO_LOG", argv[1], 1);

			argc -= 2;
			argv += 2;
		}
	}

	if (argc > 0) {
		for (i = 0; i < argc; i++)
			l += strlen (argv[i]) + 1;

		message = xmalloc (l);
		p = message;

		for (i = 0; i < argc; i++) {
			if (i > 0)
				*p++ = ' ';
			memcpy (p, argv[i], strlen (argv[i]));
			p += strlen (argv[i]);
		}
		*p = 0;
	}

	if (! message)
		message = xstrdup ("");

	if (strcmp (applet, "einfo") == 0)
		einfo ("%s", message);
	else if (strcmp (applet, "einfon") == 0)
		einfon ("%s", message);
	else if (strcmp (applet, "ewarn") == 0)
		ewarn ("%s", message);
	else if (strcmp (applet, "ewarnn") == 0)
		ewarnn ("%s", message);
	else if (strcmp (applet, "eerror") == 0) {
		eerror ("%s", message);
		retval = 1;
	} else if (strcmp (applet, "eerrorn") == 0) {
		eerrorn ("%s", message);
		retval = 1;
	} else if (strcmp (applet, "ebegin") == 0)
		ebegin ("%s", message);
	else if (strcmp (applet, "eend") == 0)
		eend (retval, "%s", message);
	else if (strcmp (applet, "ewend") == 0)
		ewend (retval, "%s", message);
	else if (strcmp (applet, "esyslog") == 0)
		elog (level, "%s", message);
	else if (strcmp (applet, "veinfo") == 0)
		einfov ("%s", message);
	else if (strcmp (applet, "veinfon") == 0)
		einfovn ("%s", message);
	else if (strcmp (applet, "vewarn") == 0)
		ewarnv ("%s", message);
	else if (strcmp (applet, "vewarnn") == 0)
		ewarnvn ("%s", message);
	else if (strcmp (applet, "vebegin") == 0)
		ebeginv ("%s", message);
	else if (strcmp (applet, "veend") == 0)
		eendv (retval, "%s", message);
	else if (strcmp (applet, "vewend") == 0)
		ewendv (retval, "%s", message);
	else if (strcmp (applet, "eindent") == 0)
		eindent ();
	else if (strcmp (applet, "eoutdent") == 0)
		eoutdent ();
	else if (strcmp (applet, "veindent") == 0)
		eindentv ();
	else if (strcmp (applet, "veoutdent") == 0)
		eoutdentv ();
	else {
		eerror ("%s: unknown applet", applet);
		retval = EXIT_FAILURE;
	}

	free (message);
	return (retval);
}

static int do_service (int argc, char **argv)
{
	bool ok = false;
	char *service = NULL;

	if (argc > 0)
		service = argv[0];
	else
		service = getenv ("SVCNAME");

	if (! service || strlen (service) == 0)
		eerrorx ("%s: no service specified", applet);

	if (strcmp (applet, "service_started") == 0)
		ok = (rc_service_state (service) & RC_SERVICE_STARTED);
	else if (strcmp (applet, "service_stopped") == 0)
		ok = (rc_service_state (service) & RC_SERVICE_STOPPED);
	else if (strcmp (applet, "service_inactive") == 0)
		ok = (rc_service_state (service) & RC_SERVICE_INACTIVE);
	else if (strcmp (applet, "service_starting") == 0)
		ok = (rc_service_state (service) & RC_SERVICE_STARTING);
	else if (strcmp (applet, "service_stopping") == 0)
		ok = (rc_service_state (service) & RC_SERVICE_STOPPING);
	else if (strcmp (applet, "service_coldplugged") == 0)
		ok = (rc_service_state (service) & RC_SERVICE_COLDPLUGGED);
	else if (strcmp (applet, "service_wasinactive") == 0)
		ok = (rc_service_state (service) & RC_SERVICE_WASINACTIVE);
	else if (strcmp (applet, "service_started_daemon") == 0) {
		int idx = 0;
		char *d = argv[0];

		service = getenv ("SVCNAME");
		if (argc > 2) {
			service = argv[0];
			d = argv[1];
			sscanf (argv[2], "%d", &idx);
		} else if (argc == 2) {
			sscanf (argv[1], "%d", &idx);
		}
		exit (rc_service_started_daemon (service, d, idx) ? 0 : 1);
	} else
		eerrorx ("%s: unknown applet", applet);

	return (ok ? EXIT_SUCCESS : EXIT_FAILURE);
}

static int do_mark_service (int argc, char **argv)
{
	bool ok = false;
	char *svcname = getenv ("SVCNAME");
	char *service = NULL;

	if (argc > 0)
		service = argv[0];
	else
		service = getenv ("SVCNAME");

	if (! service || strlen (service) == 0)
		eerrorx ("%s: no service specified", applet);

	if (strcmp (applet, "mark_service_started") == 0)
		ok = rc_service_mark (service, RC_SERVICE_STARTED);
	else if (strcmp (applet, "mark_service_stopped") == 0)
		ok = rc_service_mark (service, RC_SERVICE_STOPPED);
	else if (strcmp (applet, "mark_service_inactive") == 0)
		ok = rc_service_mark (service, RC_SERVICE_INACTIVE);
	else if (strcmp (applet, "mark_service_starting") == 0)
		ok = rc_service_mark (service, RC_SERVICE_STARTING);
	else if (strcmp (applet, "mark_service_stopping") == 0)
		ok = rc_service_mark (service, RC_SERVICE_STOPPING);
	else if (strcmp (applet, "mark_service_coldplugged") == 0)
		ok = rc_service_mark (service, RC_SERVICE_COLDPLUGGED);
	else if (strcmp (applet, "mark_service_failed") == 0)
		ok = rc_service_mark (service, RC_SERVICE_FAILED);
	else
		eerrorx ("%s: unknown applet", applet);

	/* If we're marking ourselves then we need to inform our parent runscript
	   process so they do not mark us based on our exit code */
	if (ok && svcname && strcmp (svcname, service) == 0) {
		char *runscript_pid = getenv ("RC_RUNSCRIPT_PID");
		char *mtime;
		pid_t pid = 0;
		int l;

		if (runscript_pid && sscanf (runscript_pid, "%d", &pid) == 1)
			if (kill (pid, SIGHUP) != 0)
				eerror ("%s: failed to signal parent %d: %s",
					applet, pid, strerror (errno));

		/* Remove the exclusive time test. This ensures that it's not
		   in control as well */
		l = strlen (RC_SVCDIR "exclusive") +
			strlen (svcname) +
			strlen (runscript_pid) +
			4;
		mtime = xmalloc (l);
		snprintf (mtime, l, RC_SVCDIR "exclusive/%s.%s",
			  svcname, runscript_pid);
		if (exists (mtime) && unlink (mtime) != 0)
			eerror ("%s: unlink: %s", applet, strerror (errno));
		free (mtime);
	}

	return (ok ? EXIT_SUCCESS : EXIT_FAILURE);
}

static int do_value (int argc, char **argv)
{
	bool ok = false;
	char *service = getenv ("SVCNAME");

	if (! service)
		eerrorx ("%s: no service specified", applet);

	if (argc < 1 || ! argv[0] || strlen (argv[0]) == 0)
		eerrorx ("%s: no option specified", applet);

	if (strcmp (applet, "service_get_value") == 0 ||
	    strcmp (applet, "get_options") == 0)
	{
		char *option = rc_service_value_get (service, argv[0]);
		if (option) {
			printf ("%s", option);
			free (option);
			ok = true;
		}
	} else if (strcmp (applet, "service_set_value") == 0 ||
		   strcmp (applet, "save_options") == 0)
		ok = rc_service_value_set (service, argv[0], argv[1]);
	else
		eerrorx ("%s: unknown applet", applet);

	return (ok ? EXIT_SUCCESS : EXIT_FAILURE);
}

static int do_shell_var (int argc, char **argv)
{
	int i;

	for (i = 0; i < argc; i++) {
		char *p = argv[i];

		if (i != 0)
			putchar (' ');

		while (*p) {
			char c = *p++;
			if (! isalnum ((int) c))
				c = '_';
			putchar (c);
		}
	}
	putchar ('\n');

	return (EXIT_SUCCESS);
}

#ifdef __linux__
static char *proc_getent (const char *ent)
{
	FILE *fp;
	char *proc;
	char *p;
	char *value = NULL;
	int i;

	if (! exists ("/proc/cmdline"))
		return (NULL);

	if (! (fp = fopen ("/proc/cmdline", "r"))) {
		eerror ("failed to open `/proc/cmdline': %s", strerror (errno));
		return (NULL);
	}

	if ((proc = rc_getline (fp)) &&
	    (p = strstr (proc, ent)))
	{
		i = p - proc;
		if (i == '\0' || proc[i - 1] == ' ') {
			p += strlen (ent);
			if (*p == '=')
				p++;
			value = xstrdup (strsep (&p, " "));
		}
	} else
		errno = ENOENT;
	free (proc);
	fclose (fp);

	return (value);
}
#endif

static char read_key (bool block)
{
	struct termios termios;
	char c = 0;
	int fd = fileno (stdin);

	if (! isatty (fd))
		return (false);

	/* Now save our terminal settings. We need to restore them at exit as we
	   will be changing it for non-blocking reads for Interactive */
	if (! termios_orig) {
		termios_orig = xmalloc (sizeof (struct termios));
		tcgetattr (fd, termios_orig);
	}

	tcgetattr (fd, &termios);
	termios.c_lflag &= ~(ICANON | ECHO);
	if (block)
		termios.c_cc[VMIN] = 1;
	else {
		termios.c_cc[VMIN] = 0;
		termios.c_cc[VTIME] = 0;
	}
	tcsetattr (fd, TCSANOW, &termios);

	read (fd, &c, 1);

	tcsetattr (fd, TCSANOW, termios_orig);

	return (c);
}

static bool want_interactive (void)
{
	char c;
	static bool gotinteractive;
	static bool interactive;

	if (rc_yesno (getenv ("EINFO_QUIET")))
		return (false);

	if (PREVLEVEL &&
	    strcmp (PREVLEVEL, "N") != 0 &&
	    strcmp (PREVLEVEL, "S") != 0 &&
	    strcmp (PREVLEVEL, "1") != 0)
		return (false);

	if (! gotinteractive) {
		gotinteractive = true;
		interactive = rc_conf_yesno ("rc_interactive");
	}
	if (! interactive)
		return (false);

	c = read_key (false);
	return ((c == 'I' || c == 'i') ? true : false);
}

static void mark_interactive (void)
{
	FILE *fp = fopen (INTERACTIVE, "w");
	if (fp)
		fclose (fp);
}

static void sulogin (bool cont)
{
#ifdef __linux__
	char *e = getenv ("RC_SYS");

	/* VPS systems cannot do a sulogin */
	if (e && strcmp (e, "VPS") == 0) {
		execl ("/sbin/halt", "/sbin/halt", "-f", (char *) NULL);
		eerrorx ("%s: unable to exec `/sbin/halt': %s", applet, strerror (errno));
	}
#endif

	newenv = env_filter ();

	if (cont) {
		int status = 0;
#ifdef __linux__
		char *tty = ttyname (STDOUT_FILENO);
#endif

		pid_t pid = vfork ();

		if (pid == -1)
			eerrorx ("%s: vfork: %s", applet, strerror (errno));
		if (pid == 0) {
#ifdef __linux__
			if (tty)
				execle (SULOGIN, SULOGIN, tty, (char *) NULL, newenv);
			else
				execle (SULOGIN, SULOGIN, (char *) NULL, newenv);

			eerror ("%s: unable to exec `%s': %s", applet, SULOGIN,
				strerror (errno));
#else
			execle ("/bin/sh", "/bin/sh", (char *) NULL, newenv);
			eerror ("%s: unable to exec `/bin/sh': %s", applet,
				strerror (errno));
#endif
			_exit (EXIT_FAILURE);
		}
		waitpid (pid, &status, 0);
	} else {
		rc_logger_close ();

#ifdef __linux__
		execle ("/sbin/sulogin", "/sbin/sulogin", (char *) NULL, newenv);
		eerrorx ("%s: unable to exec `/sbin/sulogin': %s", applet, strerror (errno));
#else
		exit (EXIT_SUCCESS);
#endif
	}
}

static void single_user (void)
{
	rc_logger_close ();

#ifdef __linux__
	execl ("/sbin/telinit", "/sbin/telinit", "S", (char *) NULL);
	eerrorx ("%s: unable to exec `/sbin/telinit': %s",
		 applet, strerror (errno));
#else
	if (kill (1, SIGTERM) != 0)
		eerrorx ("%s: unable to send SIGTERM to init (pid 1): %s",
			 applet, strerror (errno));
	exit (EXIT_SUCCESS);
#endif
}

static bool set_ksoftlevel (const char *level)
{
	FILE *fp;

	if (! level ||
	    strcmp (level, getenv ("RC_BOOTLEVEL")) == 0 ||
	    strcmp (level, RC_LEVEL_SINGLE) == 0 ||
	    strcmp (level, RC_LEVEL_SYSINIT) == 0)
	{
		if (exists (RC_KSOFTLEVEL) &&
		    unlink (RC_KSOFTLEVEL) != 0)
			eerror ("unlink `%s': %s", RC_KSOFTLEVEL, strerror (errno));
		return (false);
	}

	if (! (fp = fopen (RC_KSOFTLEVEL, "w"))) {
		eerror ("fopen `%s': %s", RC_KSOFTLEVEL, strerror (errno));
		return (false);
	}

	fprintf (fp, "%s", level);
	fclose (fp);
	return (true);
}

static int get_ksoftlevel (char *buffer, int buffer_len)
{
	FILE *fp;
	int i = 0;

	if (! exists (RC_KSOFTLEVEL))
		return (0);

	if (! (fp = fopen (RC_KSOFTLEVEL, "r"))) {
		eerror ("fopen `%s': %s", RC_KSOFTLEVEL, strerror (errno));
		return (-1);
	}

	if (fgets (buffer, buffer_len, fp)) {
		i = strlen (buffer) - 1;
		if (buffer[i] == '\n')
			buffer[i] = 0;
	}

	fclose (fp);
	return (i);
}

static void add_pid (pid_t pid)
{
	pidlist_t *sp = service_pids;
	if (sp) {
		while (sp->next)
			sp = sp->next;
		sp->next = xmalloc (sizeof (pidlist_t));
		sp = sp->next;
	} else
		sp = service_pids = xmalloc (sizeof (pidlist_t));
	memset (sp, 0, sizeof (pidlist_t));
	sp->pid = pid;
}

static void remove_pid (pid_t pid)
{
	pidlist_t *last = NULL;
	pidlist_t *pl;

	for (pl = service_pids; pl; pl = pl->next) {
		if (pl->pid == pid) {
			if (last)
				last->next = pl->next;
			else
				service_pids = pl->next;
			free (pl);
			break;
		}
		last = pl;
	}
}

static void wait_for_services ()
{
	while (waitpid (0, 0, 0) != -1);
}

static void handle_signal (int sig)
{
	int serrno = errno;
	char signame[10] = { '\0' };
	pidlist_t *pl;
	pid_t pid;
	int status = 0;
	struct winsize ws;

	switch (sig) {
		case SIGCHLD:
			do {
				pid = waitpid (-1, &status, WNOHANG);
				if (pid < 0) {
					if (errno != ECHILD)
						eerror ("waitpid: %s", strerror (errno));
					return;
				}
			} while (! WIFEXITED (status) && ! WIFSIGNALED (status));

			/* Remove that pid from our list */
			if (pid > 0)
				remove_pid (pid);
			break;

		case SIGWINCH:
			if (rc_logger_tty >= 0) {
				ioctl (STDIN_FILENO, TIOCGWINSZ, &ws);
				ioctl (rc_logger_tty, TIOCSWINSZ, &ws);
			}
			break;

		case SIGINT:
			if (! signame[0])
				snprintf (signame, sizeof (signame), "SIGINT");
		case SIGTERM:
			if (! signame[0])
				snprintf (signame, sizeof (signame), "SIGTERM");
		case SIGQUIT:
			if (! signame[0])
				snprintf (signame, sizeof (signame), "SIGQUIT");
			eerrorx ("%s: caught %s, aborting", applet, signame);
		case SIGUSR1:
			eerror ("rc: Aborting!");
			/* Kill any running services we have started */

			signal (SIGCHLD, SIG_IGN);
			for (pl = service_pids; pl; pl = pl->next)
				kill (pl->pid, SIGTERM);

			/* Notify plugins we are aborting */
			rc_plugin_run (RC_HOOK_ABORT, NULL);

			/* Only drop into single user mode if we're booting */
			if ((PREVLEVEL &&
			     (strcmp (PREVLEVEL, "S") == 0 ||
			      strcmp (PREVLEVEL, "1") == 0)) ||
			    (RUNLEVEL &&
			     (strcmp (RUNLEVEL, "S") == 0 ||
			      strcmp (RUNLEVEL, "1") == 0)))
				single_user ();

			exit (EXIT_FAILURE);
			break;

		default:
			eerror ("%s: caught unknown signal %d", applet, sig);
	}

	/* Restore errno */
	errno = serrno;
}

static void run_script (const char *script)
{
	int status = 0;
	pid_t pid = vfork ();

	if (pid < 0)
		eerrorx ("%s: vfork: %s", applet, strerror (errno));
	else if (pid == 0) {
		execl (script, script, (char *) NULL);
		eerror ("%s: unable to exec `%s': %s",
			script, applet, strerror (errno));
		_exit (EXIT_FAILURE);
	}

	do {
		pid_t wpid = waitpid (pid, &status, 0);
		if (wpid < 1)
			eerror ("waitpid: %s", strerror (errno));
	} while (! WIFEXITED (status) && ! WIFSIGNALED (status));

	if (! WIFEXITED (status) || ! WEXITSTATUS (status) == 0)
		eerrorx ("%s: failed to exec `%s'", applet, script);
}

#include "_usage.h"
#define getoptstring "o:" getoptstring_COMMON
static struct option longopts[] = {
	{ "override", 1, NULL, 'o' },
	longopts_COMMON
};
static const char * const longopts_help[] = {
	"override the next runlevel to change into\nwhen leaving single user or boot runlevels",
	longopts_help_COMMON
};
#include "_usage.c"

int main (int argc, char **argv)
{
	const char *bootlevel = NULL;
	char *newlevel = NULL;
	char *service = NULL;
	char **deporder = NULL;
	char **tmplist;
	int i = 0;
	int j = 0;
	bool going_down = false;
	bool interactive = false;
	int depoptions = RC_DEP_STRICT | RC_DEP_TRACE;
	char ksoftbuffer [PATH_MAX];
	char pidstr[6];
	int opt;
	DIR *dp;
	struct dirent *d;
	bool parallel;
	int regen = 0;

	applet = basename_c (argv[0]);
	atexit (cleanup);
	if (! applet)
		eerrorx ("arguments required");

	if (argc > 1 && (strcmp (argv[1], "--version") == 0)) {
		printf ("%s (OpenRC"
#ifdef BRANDING
			" " BRANDING
#endif
			") version " VERSION "\n", applet);
		exit (EXIT_SUCCESS);
	}

	/* These used to be programs in their own right, so we shouldn't
	 * touch argc or argv for them */
	if (strcmp (applet, "fstabinfo") == 0)
		exit (fstabinfo (argc, argv));
	else if (strcmp (applet, "mountinfo") == 0)
		exit (mountinfo (argc, argv));
	else if (strcmp (applet, "rc-depend") == 0)
		exit (rc_depend (argc, argv));
	else if (strcmp (applet, "rc-status") == 0)
		exit (rc_status (argc, argv));
	else if (strcmp (applet, "rc-update") == 0 ||
		 strcmp (applet, "update-rc") == 0)
		exit (rc_update (argc, argv));
	else if (strcmp (applet, "runscript") == 0)
		exit (runscript (argc, argv));
	else if (strcmp (applet, "start-stop-daemon") == 0)
		exit (start_stop_daemon (argc, argv));
	else if (strcmp (applet, "checkpath") == 0)
		exit (checkpath (argc, argv));

	argc--;
	argv++;

	/* Handle multicall stuff */
	if (applet[0] == 'e' || (applet[0] == 'v' && applet[1] == 'e'))
		exit (do_e (argc, argv));

	if (strcmp (applet, "service_get_value") == 0 ||
	    strcmp (applet, "service_set_value") == 0 ||
	    strcmp (applet, "get_options") == 0 ||
	    strcmp (applet, "save_options") == 0)
		exit (do_value (argc, argv));

	if (strncmp (applet, "service_", strlen ("service_")) == 0)
		exit (do_service (argc, argv));

	if (strncmp (applet, "mark_service_", strlen ("mark_service_")) == 0)
		exit (do_mark_service (argc, argv));

	if (strcmp (applet, "is_newer_than") == 0 ||
	    strcmp (applet, "is_older_than") == 0)
	{
		bool match = false;
		if (argc < 2)
			exit (EXIT_FAILURE);
		if (strcmp (applet, "is_newer_than") == 0)
			match = true;
		while (optind < argc) {
			if (rc_newer_than (argv[0], argv[optind++]) != match)
				exit (EXIT_FAILURE);
		}
		exit (EXIT_SUCCESS);
	}; 

	if (strcmp (applet, "is_runlevel_start") == 0)
		exit (rc_runlevel_starting () ? 0 : 1);
	else if (strcmp (applet, "is_runlevel_stop") == 0)
		exit (rc_runlevel_stopping () ? 0 : 1);

	if (strcmp (applet, "shell_var") == 0)
		exit (do_shell_var (argc, argv));

	if (strcmp (applet, "rc-abort") == 0) {
		char *p = getenv ("RC_PID");
		pid_t pid = 0;

		if (p && sscanf (p, "%d", &pid) == 1) {
			if (kill (pid, SIGUSR1) != 0)
				eerrorx ("rc-abort: failed to signal parent %d: %s",
					 pid, strerror (errno));
			exit (EXIT_SUCCESS);
		}
		exit (EXIT_FAILURE);
	}

	if (strcmp (applet, "rc" ) != 0)
		eerrorx ("%s: unknown applet", applet);

	/* Change dir to / to ensure all scripts don't use stuff in pwd */
	chdir ("/");

	/* RUNLEVEL is set by sysvinit as is a magic number
	   RC_SOFTLEVEL is set by us and is the name for this magic number
	   even though all our userland documentation refers to runlevel */
	RUNLEVEL = getenv ("RUNLEVEL");
	PREVLEVEL = getenv ("PREVLEVEL");

	/* Ensure our environment is pure
	   Also, add our configuration to it */
	env = env_filter ();
	tmplist = env_config ();
	rc_strlist_join (&env, tmplist);
	rc_strlist_free (tmplist);

	if (env) {
		char *p;

#ifdef __linux__
		/* clearenv isn't portable, but there's no harm in using it
		   if we have it */
		clearenv ();
#else
		char *var;
		/* No clearenv present here then.
		   We could manipulate environ directly ourselves, but it seems that
		   some kernels bitch about this according to the environ man pages
		   so we walk though environ and call unsetenv for each value. */
		while (environ[0]) {
			tmp = xstrdup (environ[0]);
			p = tmp;
			var = strsep (&p, "=");
			unsetenv (var);
			free (tmp);
		}
		tmp = NULL;
#endif

		STRLIST_FOREACH (env, p, i)
			if (strcmp (p, "RC_SOFTLEVEL") != 0 && strcmp (p, "SOFTLEVEL") != 0)
				putenv (p);

		/* We don't free our list as that would be null in environ */
	}

	argc++;
	argv--;
	while ((opt = getopt_long (argc, argv, getoptstring,
				   longopts, (int *) 0)) != -1)
	{
		switch (opt) {
			case 'o':
				if (strlen (optarg) == 0)
					optarg = NULL;
				exit (set_ksoftlevel (optarg) ? EXIT_SUCCESS : EXIT_FAILURE);
				case_RC_COMMON_GETOPT
		}
	}

	newlevel = argv[optind++];

	/* OK, so we really are the main RC process
	   Only root should be able to run us */
	if (geteuid () != 0)
		eerrorx ("%s: root access required", applet);

	/* Enable logging */
	setenv ("EINFO_LOG", "rc", 1);

	/* Export our PID */
	snprintf (pidstr, sizeof (pidstr), "%d", getpid ());
	setenv ("RC_PID", pidstr, 1);

	/* Load current softlevel */
	bootlevel = getenv ("RC_BOOTLEVEL");
	runlevel = rc_runlevel_get ();

	rc_logger_open (newlevel ? newlevel : runlevel);

	/* Setup a signal handler */
	signal (SIGINT, handle_signal);
	signal (SIGQUIT, handle_signal);
	signal (SIGTERM, handle_signal);
	signal (SIGUSR1, handle_signal);
	signal (SIGWINCH, handle_signal);

	if (! rc_yesno (getenv ("EINFO_QUIET")))
		interactive = exists (INTERACTIVE);
	rc_plugin_load ();

	/* Check we're in the runlevel requested, ie from
	   rc single
	   rc shutdown
	   rc reboot
	   */
	if (newlevel) {
		if (strcmp (newlevel, RC_LEVEL_SYSINIT) == 0 &&
		    RUNLEVEL &&
		    (strcmp (RUNLEVEL, "S") == 0 ||
		     strcmp (RUNLEVEL, "1") == 0))
		{
			/* OK, we're either in runlevel 1 or single user mode */
			struct utsname uts;
#ifdef __linux__
			char *cmd;
#endif

			/* exec init-early.sh if it exists
			 * This should just setup the console to use the correct
			 * font. Maybe it should setup the keyboard too? */
			if (exists (INITEARLYSH))
				run_script (INITEARLYSH);

			uname (&uts);
			printf ("\n   %sOpenRC %s" VERSION "%s is starting up %s%s%s\n\n",
				ecolor (ECOLOR_GOOD), ecolor (ECOLOR_HILITE),
				ecolor (ECOLOR_NORMAL), ecolor (ECOLOR_BRACKET),
#ifdef BRANDING
				BRANDING
#else
				""
#endif
				, ecolor (ECOLOR_NORMAL));

			if (! rc_yesno (getenv ("EINFO_QUIET")) &&
			    rc_conf_yesno ("rc_interactive"))
				printf ("Press %sI%s to enter interactive boot mode\n\n",
					ecolor (ECOLOR_GOOD), ecolor (ECOLOR_NORMAL));

			setenv ("RC_SOFTLEVEL", newlevel, 1);
			rc_plugin_run (RC_HOOK_RUNLEVEL_START_IN, newlevel);
			run_script (INITSH);

#ifdef __linux__
			/* If we requested a softlevel, save it now */
			set_ksoftlevel (NULL);
			if ((cmd = proc_getent ("softlevel"))) {
				set_ksoftlevel (cmd);
				free (cmd);
			}
#endif

			rc_plugin_run (RC_HOOK_RUNLEVEL_START_OUT, newlevel);

			if (want_interactive ())
				mark_interactive ();

			exit (EXIT_SUCCESS);
		} else if (strcmp (newlevel, RC_LEVEL_SINGLE) == 0) {
			if (! RUNLEVEL ||
			    (strcmp (RUNLEVEL, "S") != 0 &&
			     strcmp (RUNLEVEL, "1") != 0))
			{
				/* Remember the current runlevel for when we come back */
				set_ksoftlevel (runlevel);
				single_user ();
			}
		} else if (strcmp (newlevel, RC_LEVEL_REBOOT) == 0) {
			if (! RUNLEVEL ||
			    strcmp (RUNLEVEL, "6") != 0)
			{
				rc_logger_close ();
				execl (SHUTDOWN, SHUTDOWN, "-r", "now", (char *) NULL);
				eerrorx ("%s: unable to exec `" SHUTDOWN "': %s",
					 applet, strerror (errno));
			}
		} else if (strcmp (newlevel, RC_LEVEL_SHUTDOWN) == 0) {
			if (! RUNLEVEL ||
			    strcmp (RUNLEVEL, "0") != 0)
			{
				rc_logger_close ();
				execl (SHUTDOWN, SHUTDOWN,
#ifdef __linux__
				       "-h",
#else
				       "-p",
#endif
				       "now", (char *) NULL);
				eerrorx ("%s: unable to exec `" SHUTDOWN "': %s",
					 applet, strerror (errno));
			}
		}
	}

	/* Now we start handling our children */
	signal (SIGCHLD, handle_signal);

	/* We should only use ksoftlevel if we were in single user mode
	   If not, we need to erase ksoftlevel now. */
	if (PREVLEVEL &&
	    (strcmp (PREVLEVEL, "1") == 0 ||
	     strcmp (PREVLEVEL, "S") == 0 ||
	     strcmp (PREVLEVEL, "N") == 0))
	{
		/* Try not to join boot and ksoftlevels together */
		if (! newlevel ||
		    strcmp (newlevel, getenv ("RC_BOOTLEVEL")) != 0)
			if (get_ksoftlevel (ksoftbuffer, sizeof (ksoftbuffer)))
				newlevel = ksoftbuffer;
	} else if (! RUNLEVEL ||
		   (strcmp (RUNLEVEL, "1") != 0 &&
		    strcmp (RUNLEVEL, "S") != 0 &&
		    strcmp (RUNLEVEL, "N") != 0))
	{
		set_ksoftlevel (NULL);
	}

	if (newlevel &&
	    (strcmp (newlevel, RC_LEVEL_REBOOT) == 0 ||
	     strcmp (newlevel, RC_LEVEL_SHUTDOWN) == 0 ||
	     strcmp (newlevel, RC_LEVEL_SINGLE) == 0))
	{
		going_down = true;
		rc_runlevel_set (newlevel);
		setenv ("RC_SOFTLEVEL", newlevel, 1);

#ifdef __FreeBSD__
		/* FIXME: we shouldn't have todo this */
		/* For some reason, wait_for_services waits for the logger proccess
		 * to finish as well, but only on FreeBSD. We cannot allow this so
		 * we stop logging now. */
		rc_logger_close ();
#endif

		rc_plugin_run (RC_HOOK_RUNLEVEL_STOP_IN, newlevel);
	} else {
		rc_plugin_run (RC_HOOK_RUNLEVEL_STOP_IN, runlevel);
	}

	/* Check if runlevel is valid if we're changing */
	if (newlevel && strcmp (runlevel, newlevel) != 0 && ! going_down) {
		if (! rc_runlevel_exists (newlevel))
			eerrorx ("%s: is not a valid runlevel", newlevel);
	}

	/* Load our deptree now */
	if ((deptree = _rc_deptree_load (&regen)) == NULL)
		eerrorx ("failed to load deptree");

	/* Clean the failed services state dir now */
	clean_failed ();

	mkdir (RC_STOPPING, 0755);

#ifdef __linux__
	/* udev likes to start services before we're ready when it does
	   its coldplugging thing. runscript knows when we're not ready so it
	   stores a list of coldplugged services in DEVBOOT for us to pick up
	   here when we are ready for them */
	if ((dp = opendir (DEVBOOT))) {
		while ((d = readdir (dp))) {
			if (d->d_name[0] == '.' &&
			    (d->d_name[1] == '\0' ||
			     (d->d_name[1] == '.' && d->d_name[2] == '\0')))
				continue;

			if (rc_service_exists (d->d_name) &&
			    service_plugable (d->d_name))
				rc_service_mark (d->d_name, RC_SERVICE_COLDPLUGGED);

			i = strlen (DEVBOOT "/") + strlen (d->d_name) + 1;
			tmp = xmalloc (sizeof (char) * i);
			snprintf (tmp, i, DEVBOOT "/%s", d->d_name);
			if (tmp) {
				if (unlink (tmp))
					eerror ("%s: unlink `%s': %s", applet, tmp,
						strerror (errno));
				free (tmp);
			}
		}
		closedir (dp);
		rmdir (DEVBOOT);
	}
#else
	/* BSD's on the other hand populate /dev automagically and use devd.
	   The only downside of this approach and ours is that we have to hard code
	   the device node to the init script to simulate the coldplug into
	   runlevel for our dependency tree to work. */
	if (newlevel && strcmp (newlevel, bootlevel) == 0 &&
	    (strcmp (runlevel, RC_LEVEL_SINGLE) == 0 ||
	     strcmp (runlevel, RC_LEVEL_SYSINIT) == 0) &&
	    rc_conf_yesno ("rc_coldplug"))
	{
#if defined(__DragonFly__) || defined(__FreeBSD__)
		/* The net interfaces are easy - they're all in net /dev/net :) */
		if ((dp = opendir ("/dev/net"))) {
			while ((d = readdir (dp))) {
				i = (strlen ("net.") + strlen (d->d_name) + 1);
				tmp = xmalloc (sizeof (char) * i);
				snprintf (tmp, i, "net.%s", d->d_name);
				if (rc_service_exists (tmp) &&
				    service_plugable (tmp))
					rc_service_mark (tmp, RC_SERVICE_COLDPLUGGED);
				CHAR_FREE (tmp);
			}
			closedir (dp);
		}
#endif

		/* The mice are a little more tricky.
		   If we coldplug anything else, we'll probably do it here. */
		if ((dp = opendir ("/dev"))) {
			while ((d = readdir (dp))) {
				if (strncmp (d->d_name, "psm", 3) == 0 ||
				    strncmp (d->d_name, "ums", 3) == 0)
				{
					char *p = d->d_name + 3;
					if (p && isdigit ((int) *p)) {
						i = (strlen ("moused.") + strlen (d->d_name) + 1);
						tmp = xmalloc (sizeof (char) * i);
						snprintf (tmp, i, "moused.%s", d->d_name);
						if (rc_service_exists (tmp) &&
						    service_plugable (tmp))
							rc_service_mark (tmp, RC_SERVICE_COLDPLUGGED);
						CHAR_FREE (tmp);
					}
				}
			}
			closedir (dp);
		}
	}
#endif

	/* Build a list of all services to stop and then work out the
	   correct order for stopping them */
	stop_services = rc_services_in_state (RC_SERVICE_STARTING);

	tmplist = rc_services_in_state (RC_SERVICE_INACTIVE);
	rc_strlist_join (&stop_services, tmplist);
	rc_strlist_free (tmplist);

	tmplist = rc_services_in_state (RC_SERVICE_STARTED);
	rc_strlist_join (&stop_services, tmplist);
	rc_strlist_free (tmplist);

	deporder = rc_deptree_depends (deptree, types_nua,
				       (const char **) stop_services,
				       runlevel, depoptions | RC_DEP_STOP);

	rc_strlist_free (stop_services);
	stop_services = deporder;
	deporder = NULL;
	rc_strlist_reverse (stop_services);

	/* Load our list of coldplugged services */
	coldplugged_services = rc_services_in_state (RC_SERVICE_COLDPLUGGED);

	/* Load our start services now.
	   We have different rules dependent on runlevel. */
	if (newlevel && strcmp (newlevel, bootlevel) == 0) {
		if (coldplugged_services) {
			bool quiet = rc_yesno (getenv ("EINFO_QUIET"));

			if (! quiet)
				einfon ("Device initiated services:");
			STRLIST_FOREACH (coldplugged_services, service, i) {
				if (! quiet)
					printf (" %s", service);
				rc_strlist_add (&start_services, service);
			}
			if (! quiet)
				printf ("\n");
		}
		tmplist = rc_services_in_runlevel (newlevel ? newlevel : runlevel);
		rc_strlist_join (&start_services, tmplist);
		rc_strlist_free (tmplist);
	} else {
		/* Store our list of coldplugged services */
		tmplist = rc_services_in_state (RC_SERVICE_COLDPLUGGED);
		rc_strlist_join (&coldplugged_services, tmplist);
		rc_strlist_free (tmplist);
		if (strcmp (newlevel ? newlevel : runlevel, RC_LEVEL_SINGLE) != 0 &&
		    strcmp (newlevel ? newlevel : runlevel, RC_LEVEL_SHUTDOWN) != 0 &&
		    strcmp (newlevel ? newlevel : runlevel, RC_LEVEL_REBOOT) != 0)
		{
			/* We need to include the boot runlevel services if we're not in it */
			tmplist = rc_services_in_runlevel (bootlevel);
			rc_strlist_join (&start_services, tmplist);
			rc_strlist_free (tmplist);
			tmplist = rc_services_in_runlevel (newlevel ? newlevel : runlevel);
			rc_strlist_join (&start_services, tmplist);
			rc_strlist_free (tmplist);

			STRLIST_FOREACH (coldplugged_services, service, i)
				rc_strlist_add (&start_services, service);

		}
	}

	/* Save out softlevel now */
	if (going_down)
		rc_runlevel_set (newlevel);

	parallel = rc_conf_yesno ("rc_parallel");

	/* Now stop the services that shouldn't be running */
	STRLIST_FOREACH (stop_services, service, i) {
		bool found = false;
		char *conf = NULL;
		char **stopdeps = NULL;
		char *svc1 = NULL;
		char *svc2 = NULL;
		int k;

		if (rc_service_state (service) & RC_SERVICE_STOPPED)
			continue;

		/* We always stop the service when in these runlevels */
		if (going_down) {
			pid_t pid = rc_service_stop (service);
			if (pid > 0 && ! parallel)
				rc_waitpid (pid);
			continue;
		}

		/* If we're in the start list then don't bother stopping us */
		STRLIST_FOREACH (start_services, svc1, j)
			if (strcmp (svc1, service) == 0) {
				found = true;
				break;
			}

		/* Unless we would use a different config file */
		if (found) {
			int len;
			if (! newlevel)
				continue;

			len = strlen (service) + strlen (runlevel) + 2;
			tmp = xmalloc (sizeof (char) * len);
			snprintf (tmp, len, "%s.%s", service, runlevel);
			conf = rc_strcatpaths (RC_CONFDIR, tmp, (char *) NULL);
			found = exists (conf);
			CHAR_FREE (conf);
			CHAR_FREE (tmp);
			if (! found) {
				len = strlen (service) + strlen (newlevel) + 2;
				tmp = xmalloc (sizeof (char) * len);
				snprintf (tmp, len, "%s.%s", service, newlevel);
				conf = rc_strcatpaths (RC_CONFDIR, tmp, (char *) NULL);
				found = exists (conf);
				CHAR_FREE (conf);
				CHAR_FREE (tmp);
				if (!found)
					continue;
			}
		} else {
			/* Allow coldplugged services not to be in the runlevels list */
			if (rc_service_state (service) & RC_SERVICE_COLDPLUGGED)
				continue;
		}

		/* We got this far! Or last check is to see if any any service that
		   going to be started depends on us */
		rc_strlist_add (&stopdeps, service);
		deporder = rc_deptree_depends (deptree, types_n,
					       (const char **) stopdeps,
					       runlevel, RC_DEP_STRICT);
		rc_strlist_free (stopdeps);
		stopdeps = NULL;
		found = false;
		STRLIST_FOREACH (deporder, svc1, j) {
			STRLIST_FOREACH (start_services, svc2, k)
				if (strcmp (svc1, svc2) == 0) {
					found = true;
					break;
				}
			if (found)
				break;
		}
		rc_strlist_free (deporder);
		deporder = NULL;

		/* After all that we can finally stop the blighter! */
		if (! found) {
			pid_t pid;

			if ((pid = rc_service_stop (service)) > 0) {
				add_pid (pid);

				if (! parallel) {
					rc_waitpid (pid);
					remove_pid (pid);
				}
			}
		}
	}

	/* Wait for our services to finish */
	wait_for_services ();

	/* Notify the plugins we have finished */
	rc_plugin_run (RC_HOOK_RUNLEVEL_STOP_OUT, runlevel);

	rmdir (RC_STOPPING);

	/* Store the new runlevel */
	if (newlevel) {
		rc_runlevel_set (newlevel);
		free (runlevel);
		runlevel = xstrdup (newlevel);
		setenv ("RC_SOFTLEVEL", runlevel, 1);
	}

	/* Run the halt script if needed */
	if (strcmp (runlevel, RC_LEVEL_SHUTDOWN) == 0 ||
	    strcmp (runlevel, RC_LEVEL_REBOOT) == 0)
	{
		rc_logger_close ();
		execl (HALTSH, HALTSH, runlevel, (char *) NULL);
		eerrorx ("%s: unable to exec `%s': %s",
			 applet, HALTSH, strerror (errno));
	}

	/* Single user is done now */
	if (strcmp (runlevel, RC_LEVEL_SINGLE) == 0) {
		if (exists (INTERACTIVE))
			unlink (INTERACTIVE);
		sulogin (false);
	}

	mkdir (RC_STARTING, 0755);
	rc_plugin_run (RC_HOOK_RUNLEVEL_START_IN, runlevel);

	/* Re-add our coldplugged services if they stopped */
	STRLIST_FOREACH (coldplugged_services, service, i)
		rc_service_mark (service, RC_SERVICE_COLDPLUGGED);

	/* Order the services to start */
	deporder = rc_deptree_depends (deptree, types_nua,
				       (const char **) start_services,
				       runlevel, depoptions | RC_DEP_START);
	rc_strlist_free (start_services);
	start_services = deporder;
	deporder = NULL;

#ifdef __linux__
	/* mark any services skipped as started */
	if (PREVLEVEL && strcmp (PREVLEVEL, "N") == 0) {
		if ((service = proc_getent ("noinitd"))) {
			char *p = service;
			char *token;

			while ((token = strsep (&p, ",")))
				rc_service_mark (token, RC_SERVICE_STARTED);
			free (service);
		}
	}
#endif

	STRLIST_FOREACH (start_services, service, i) {
		if (rc_service_state (service) & RC_SERVICE_STOPPED)	{
			pid_t pid;

			if (! interactive)
				interactive = want_interactive ();

			if (interactive) {
interactive_retry:
				printf ("\n");
				einfo ("About to start the service %s", service);
				eindent ();
				einfo ("1) Start the service\t\t2) Skip the service");
				einfo ("3) Continue boot process\t\t4) Exit to shell");
				eoutdent ();
interactive_option:
				switch (read_key (true)) {
					case '1': break;
					case '2': continue;
					case '3': interactive = false; break;
					case '4': sulogin (true); goto interactive_retry;
					default: goto interactive_option;
				}
			}

			/* Remember the pid if we're running in parallel */
			if ((pid = rc_service_start (service)) > 0) {
				add_pid (pid);

				if (! parallel) {
					rc_waitpid (pid);
					remove_pid (pid);
				}
			}
		}
	}

	/* Wait for our services to finish */
	wait_for_services ();

	rc_plugin_run (RC_HOOK_RUNLEVEL_START_OUT, runlevel);

#ifdef __linux__
	/* mark any services skipped as stopped */
	if (PREVLEVEL && strcmp (PREVLEVEL, "N") == 0) {
		if ((service = proc_getent ("noinitd"))) {
			char *p = service;
			char *token;

			while ((token = strsep (&p, ",")))
				rc_service_mark (token, RC_SERVICE_STOPPED);
			free (service);
		}
	}
#endif

	/* Store our interactive status for boot */
	if (interactive && strcmp (runlevel, bootlevel) == 0)
		mark_interactive ();
	else {
		if (exists (INTERACTIVE))
			unlink (INTERACTIVE);
	}

	/* If we're in the boot runlevel and we regenerated our dependencies
	 * we need to delete them so that they are regenerated again in the
	 * default runlevel as they may depend on things that are now available */
	if (regen && strcmp (runlevel, bootlevel) == 0)
		unlink (RC_DEPTREE);

	return (EXIT_SUCCESS);
}
