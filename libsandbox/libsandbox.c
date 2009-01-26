/*
 * libsandbox.c
 *
 * Main libsandbox related functions.
 *
 * Copyright 1999-2008 Gentoo Foundation
 * Licensed under the GPL-2
 *
 *  Partly Copyright (C) 1998-9 Pancrazio `Ezio' de Mauro <p@demauro.net>,
 *  as some of the InstallWatch code was used.
 */

/* Uncomment below to enable memory debugging. */
/* #define SB_MEM_DEBUG 1 */

#define open   xxx_open
#define open64 xxx_open64

#include "headers.h"

#ifdef SB_MEM_DEBUG
# include <mcheck.h>
#endif

#undef open
#undef open64

#include "sbutil.h"
#include "libsandbox.h"
#include "wrappers.h"
#include "sb_nr.h"

#define LOG_VERSION			"1.0"
#define LOG_STRING			"VERSION " LOG_VERSION "\n"
#define LOG_FMT_FUNC			"FORMAT: F - Function called\n"
#define LOG_FMT_ACCESS			"FORMAT: S - Access Status\n"
#define LOG_FMT_PATH			"FORMAT: P - Path as passed to function\n"
#define LOG_FMT_APATH			"FORMAT: A - Absolute Path (not canonical)\n"
#define LOG_FMT_RPATH			"FORMAT: R - Canonical Path\n"
#define LOG_FMT_CMDLINE			"FORMAT: C - Command Line\n"

#define PROC_DIR			"/proc"
#define PROC_SELF_FD			PROC_DIR "/self/fd"
#define PROC_SELF_CMDLINE		PROC_DIR "/self/cmdline"

char sandbox_lib[SB_PATH_MAX];

typedef struct {
	int show_access_violation;
	char **prefixes[5];
	int num_prefixes[5];
#define             deny_prefixes     prefixes[0]
#define         num_deny_prefixes num_prefixes[0]
#define             read_prefixes     prefixes[1]
#define         num_read_prefixes num_prefixes[1]
#define            write_prefixes     prefixes[2]
#define        num_write_prefixes num_prefixes[2]
#define          predict_prefixes     prefixes[3]
#define      num_predict_prefixes num_prefixes[3]
#define     write_denied_prefixes     prefixes[4]
#define num_write_denied_prefixes num_prefixes[4]
#define MAX_DYN_PREFIXES 4 /* the first 4 are dynamic */
} sbcontext_t;

static sbcontext_t sbcontext;
static char **cached_env_vars;
volatile int sandbox_on = 1;
static int sb_init = 0;
static int sb_path_size_warning = 0;

static char *resolve_path(const char *, int);
static int write_logfile(const char *, const char *, const char *,
						 const char *, const char *, bool);
static int check_prefixes(char **, int, const char *);
static void clean_env_entries(char ***, int *);
static void init_context(sbcontext_t *);
static void init_env_entries(char ***, int *, const char *, const char *, int);


/*
 * Initialize the shabang
 */

static char log_domain[] = "libsandbox";

__attribute__((destructor))
void libsb_fini(void)
{
	int x;

	sb_init = 0;

	if (NULL != cached_env_vars) {
		for (x = 0; x < MAX_DYN_PREFIXES; ++x) {
			if (NULL != cached_env_vars[x]) {
				free(cached_env_vars[x]);
				cached_env_vars[x] = NULL;
			}
		}
		free(cached_env_vars);
		cached_env_vars = NULL;
	}

	for (x = 0; x < MAX_DYN_PREFIXES; ++x)
		clean_env_entries(&(sbcontext.prefixes[x]),
				&(sbcontext.num_prefixes[x]));
}

__attribute__((constructor))
void libsb_init(void)
{
	int old_errno = errno;

#ifdef SB_MEM_DEBUG
	mtrace();
#endif

	rc_log_domain(log_domain);
	sb_set_open(libsb_open);

	/* Get the path and name to this library */
	get_sandbox_lib(sandbox_lib);

//	sb_init = 1;

	errno = old_errno;
}

int canonicalize(const char *path, char *resolved_path)
{
	int old_errno = errno;
	char *retval;

	*resolved_path = '\0';

	/* If path == NULL, return or we get a segfault */
	if (NULL == path) {
		errno = EINVAL;
		return -1;
	}

	/* Do not try to resolve an empty path */
	if ('\0' == path[0]) {
		errno = old_errno;
		return 0;
	}

	retval = erealpath(path, resolved_path);

	if ((NULL == retval) && (path[0] != '/')) {
		/* The path could not be canonicalized, append it
		 * to the current working directory if it was not
		 * an absolute path
		 */

		if (ENAMETOOLONG == errno)
			return -1;

		if (NULL == egetcwd(resolved_path, SB_PATH_MAX - 2))
			return -1;
		size_t len = strlen(resolved_path);
		snprintf(resolved_path + len, SB_PATH_MAX - len, "/%s", path);

		if (NULL == erealpath(resolved_path, resolved_path)) {
			if (errno == ENAMETOOLONG) {
				/* The resolved path is too long for the buffer to hold */
				return -1;
			} else {
				/* Whatever it resolved, is not a valid path */
				errno = ENOENT;
				return -1;
			}
		}

	} else if ((NULL == retval) && (path[0] == '/')) {
		/* Whatever it resolved, is not a valid path */
		errno = ENOENT;
		return -1;
	}

	errno = old_errno;
	return 0;
}

static char *resolve_path(const char *path, int follow_link)
{
	char *dname, *bname;
	char *filtered_path;

	if (NULL == path)
		return NULL;

	save_errno();

	filtered_path = xmalloc(SB_PATH_MAX * sizeof(char));

	if (0 == follow_link) {
		if (-1 == canonicalize(path, filtered_path)) {
			free(filtered_path);
			filtered_path = NULL;
		}
	} else {
		/* Basically we get the realpath which should resolve symlinks,
		 * etc.  If that fails (might not exist), we try to get the
		 * realpath of the parent directory, as that should hopefully
		 * exist.  If all else fails, just go with canonicalize */
		if (NULL == realpath(path, filtered_path)) {
			char tmp_str1[SB_PATH_MAX];
			snprintf(tmp_str1, SB_PATH_MAX, "%s", path);

			dname = dirname(tmp_str1);

			/* If not, then check if we can resolve the
			 * parent directory */
			if (NULL == realpath(dname, filtered_path)) {
				/* Fall back to canonicalize */
				if (-1 == canonicalize(path, filtered_path)) {
					free(filtered_path);
					filtered_path = NULL;
				}
			} else {
				char tmp_str2[SB_PATH_MAX];
				/* OK, now add the basename to keep our access
				 * checking happy (don't want '/usr/lib' if we
				 * tried to do something with non-existing
				 * file '/usr/lib/cf*' ...) */
				snprintf(tmp_str2, SB_PATH_MAX, "%s", path);

				bname = basename(tmp_str2);
				size_t len = strlen(filtered_path);
				snprintf(filtered_path + len, SB_PATH_MAX - len, "%s%s",
					(filtered_path[len - 1] != '/') ? "/" : "",
					bname);
			}
		}
	}

	restore_errno();

	return filtered_path;
}

/*
 * Internal Functions
 */

char *egetcwd(char *buf, size_t size)
{
	struct stat st;
	char *tmpbuf, *oldbuf = buf;
	int old_errno;

	/* Need to disable sandbox, as on non-linux libc's, opendir() is
	 * used by some getcwd() implementations and resolves to the sandbox
	 * opendir() wrapper, causing infinit recursion and finially crashes.
	 */
	sandbox_on = 0;
	errno = 0;
	tmpbuf = libsb_getcwd(buf, size);
	sandbox_on = 1;

	/* We basically try to figure out if we can trust what getcwd()
	 * returned.  If one of the following happens kernel/libc side,
	 * bad things will happen, but not much we can do about it:
	 *  - Invalid pointer with errno = 0
	 *  - Truncated path with errno = 0
	 *  - Whatever I forgot about
	 */
	if ((tmpbuf) && (errno == 0)) {
		old_errno = errno;
		lstat(buf, &st);

		if (errno == ENOENT) {
		  	/* If getcwd() allocated the buffer, free it. */
			if (NULL == oldbuf)
				free(tmpbuf);

			/* If lstat() failed with eerror = ENOENT, then its
			 * possible that we are running on an older kernel
			 * which had issues with returning invalid paths if
			 * they got too long.  Return with errno = ENAMETOOLONG,
			 * so that canonicalize() and check_syscall() know
			 * what the issue is.
			 */
		  	errno = ENAMETOOLONG;
			return NULL;
		} else if (errno != 0) {
		  	/* If getcwd() allocated the buffer, free it. */
			if (NULL == oldbuf)
				free(tmpbuf);

			/* Not sure if we should quit here, but I guess if
			 * lstat() fails, getcwd could have messed up. Not
			 * sure what to do about errno - use lstat()'s for
			 * now.
			 */
			return NULL;
		}

		errno = old_errno;
	} else if (errno != 0) {

		/* Make sure we do not return garbage if the current libc or
		 * kernel's getcwd() is buggy.
		 */
		return NULL;
	}

	return tmpbuf;
}

static char *getcmdline(void)
{
	rc_dynbuf_t *proc_data;
	struct stat st;
	char *buf;
	size_t n;
	int fd;

	if (-1 == stat(PROC_SELF_CMDLINE, &st)) {
		/* Don't care if it does not exist */
		errno = 0;
		return NULL;
	}

	proc_data = rc_dynbuf_new();

	fd = sb_open(PROC_SELF_CMDLINE, O_RDONLY, 0);
	if (fd < 0) {
		SB_EERROR("ISE ", "Failed to open '%s'!\n", PROC_SELF_CMDLINE);
		return NULL;
	}

	/* Read PAGE_SIZE at a time -- whenever EOF or an error is found
	 * (don't care) give up and return.
	 * XXX: Some linux kernels especially needed read() to read PAGE_SIZE
	 *      at a time. */
	do {
		n = rc_dynbuf_write_fd(proc_data, fd, getpagesize());
		if (-1 == n) {
			SB_EERROR("ISE ", "Failed to read from '%s'!\n", PROC_SELF_CMDLINE);
			goto error;
		}
	} while (0 < n);

	sb_close(fd);

	rc_dynbuf_replace_char(proc_data, '\0', ' ');

	buf = rc_dynbuf_read_line(proc_data);
	if (NULL == buf)
		goto error;

	rc_dynbuf_free(proc_data);

	return buf;

error:
	if (NULL != proc_data)
		rc_dynbuf_free(proc_data);

	return NULL;
}

static int write_logfile(const char *logfile, const char *func, const char *path,
						 const char *apath, const char *rpath, bool access)
{
	struct stat log_stat;
	int stat_ret;
	int logfd;

	stat_ret = lstat(logfile, &log_stat);
	/* Do not care about failure */
	errno = 0;
	if ((0 == stat_ret) &&
	    (0 == S_ISREG(log_stat.st_mode))) {
		SB_EERROR("SECURITY BREACH", "  '%s' %s\n", logfile,
			"already exists and is not a regular file!");
		abort();
	} else {
		logfd = sb_open(logfile, O_APPEND | O_WRONLY |
				O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP |
				S_IROTH);
		if (logfd >= 0) {
			char *cmdline;

			if (0 != stat_ret) {
				SB_WRITE(logfd, LOG_STRING, strlen(LOG_STRING), error);
				SB_WRITE(logfd, LOG_FMT_FUNC, strlen(LOG_FMT_FUNC), error);
				SB_WRITE(logfd, LOG_FMT_ACCESS, strlen(LOG_FMT_ACCESS), error);
				SB_WRITE(logfd, LOG_FMT_PATH, strlen(LOG_FMT_PATH), error);
				SB_WRITE(logfd, LOG_FMT_APATH, strlen(LOG_FMT_APATH), error);
				SB_WRITE(logfd, LOG_FMT_RPATH, strlen(LOG_FMT_RPATH), error);
				SB_WRITE(logfd, LOG_FMT_CMDLINE, strlen(LOG_FMT_CMDLINE), error);
				SB_WRITE(logfd, "\n", 1, error);
			} else {
				/* Already have data in the log, so add a newline to space the
				 * log entries.
				 */
				SB_WRITE(logfd, "\n", 1, error);
			}

			SB_WRITE(logfd, "F: ", 3, error);
			SB_WRITE(logfd, func, strlen(func), error);
			SB_WRITE(logfd, "\n", 1, error);
			SB_WRITE(logfd, "S: ", 3, error);
			if (access)
				SB_WRITE(logfd, "allow", 5, error);
			else
				SB_WRITE(logfd, "deny", 4, error);
			SB_WRITE(logfd, "\n", 1, error);
			SB_WRITE(logfd, "P: ", 3, error);
			SB_WRITE(logfd, path, strlen(path), error);
			SB_WRITE(logfd, "\n", 1, error);
			SB_WRITE(logfd, "A: ", 3, error);
			SB_WRITE(logfd, apath, strlen(apath), error);
			SB_WRITE(logfd, "\n", 1, error);
			SB_WRITE(logfd, "R: ", 3, error);
			SB_WRITE(logfd, rpath, strlen(rpath), error);
			SB_WRITE(logfd, "\n", 1, error);

			cmdline = getcmdline();
			if (NULL != cmdline) {
				SB_WRITE(logfd, "C: ", 3, error);
				SB_WRITE(logfd, cmdline, strlen(cmdline),
					 error);
				SB_WRITE(logfd, "\n", 1, error);

				free(cmdline);
			} else if (0 != errno) {
				goto error;
			}

			sb_close(logfd);
		} else {
			goto error;
		}
	}

	return 0;

error:
	return -1;
}

static void init_context(sbcontext_t *context)
{
	memset(context, 0x00, sizeof(*context));
	context->show_access_violation = 1;
}

static void clean_env_entries(char ***prefixes_array, int *prefixes_num)
{
	int old_errno = errno;
	int i = 0;

	if (NULL != *prefixes_array) {
		for (i = 0; i < *prefixes_num; i++) {
			if (NULL != (*prefixes_array)[i]) {
				free((*prefixes_array)[i]);
				(*prefixes_array)[i] = NULL;
			}
		}
		if (NULL != *prefixes_array)
			free(*prefixes_array);
		*prefixes_array = NULL;
		*prefixes_num = 0;
	}

	errno = old_errno;
}

#define pfx_num		(*prefixes_num)
#define pfx_array	(*prefixes_array)
#define pfx_item	((*prefixes_array)[(*prefixes_num)])

static void init_env_entries(char ***prefixes_array, int *prefixes_num, const char *env, const char *prefixes_env, int warn)
{
	char *token = NULL;
	char *rpath = NULL;
	char *buffer = NULL;
	char *buffer_ptr = NULL;
	int prefixes_env_length = strlen(prefixes_env);
	int num_delimiters = 0;
	int i = 0;
	int old_errno = errno;

	if (NULL == prefixes_env) {
		/* Do not warn if this is in init stage, as we might get
		 * issues due to LD_PRELOAD already set (bug #91431). */
		if (1 == sb_init)
			fprintf(stderr,
				"libsandbox:  The '%s' env variable is not defined!\n",
				env);
		if (pfx_array) {
			for (i = 0; i < pfx_num; i++)
				free(pfx_item);
			free(pfx_array);
		}
		pfx_num = 0;

		goto done;
	}

	for (i = 0; i < prefixes_env_length; i++) {
		if (':' == prefixes_env[i])
			num_delimiters++;
	}

	/* num_delimiters might be 0, and we need 2 entries at least */
	pfx_array = xmalloc(((num_delimiters * 2) + 2) * sizeof(char *));
	buffer = strdup(prefixes_env);
	buffer_ptr = buffer;

#ifdef HAVE_STRTOK_R
	token = strtok_r(buffer_ptr, ":", &buffer_ptr);
#else
	token = strtok(buffer_ptr, ":");
#endif

	while ((NULL != token) && (strlen(token) > 0)) {
		pfx_item = resolve_path(token, 0);
		/* We do not care about errno here */
		errno = 0;
		if (NULL != pfx_item) {
			pfx_num++;

			/* Now add the realpath if it exists and
			 * are not a duplicate */
			rpath = xmalloc(SB_PATH_MAX * sizeof(char));
			pfx_item = realpath(*(&(pfx_item) - 1), rpath);
			if ((NULL != pfx_item) &&
			    (0 != strcmp(*(&(pfx_item) - 1), pfx_item))) {
				pfx_num++;
			} else {
				free(rpath);
				pfx_item = NULL;
			}
		}

#ifdef HAVE_STRTOK_R
		token = strtok_r(NULL, ":", &buffer_ptr);
#else
		token = strtok(NULL, ":");
#endif
	}

	free(buffer);

done:
	errno = old_errno;
	return;
}

static int check_prefixes(char **prefixes, int num_prefixes, const char *path)
{
	if (!prefixes)
		return 0;

	size_t i;
	for (i = 0; i < num_prefixes; ++i)
		if (prefixes[i] && !strncmp(path, prefixes[i], strlen(prefixes[i])))
			return 1;

	return 0;
}

static int check_access(sbcontext_t *sbcontext, int sb_nr, const char *func, const char *abs_path, const char *resolv_path)
{
	int old_errno = errno;
	int result = 0;
	int retval;

	retval = check_prefixes(sbcontext->deny_prefixes,
				sbcontext->num_deny_prefixes, resolv_path);
	if (1 == retval)
		/* Fall in a read/write denied path, Deny Access */
		goto out;

	/* Hardcode denying write to log dir */
	if (0 == strcmp(resolv_path, SANDBOX_LOG_LOCATION))
		goto out;

	if (sbcontext->read_prefixes &&
	    (sb_nr == SB_NR_ACCESS_RD ||
	     sb_nr == SB_NR_OPEN_RD   ||
	   /*sb_nr == SB_NR_POPEN     ||*/
	     sb_nr == SB_NR_OPENDIR   ||
	   /*sb_nr == SB_NR_SYSTEM    ||
	     sb_nr == SB_NR_EXECL     ||
	     sb_nr == SB_NR_EXECLP    ||
	     sb_nr == SB_NR_EXECLE    ||
	     sb_nr == SB_NR_EXECV     ||
	     sb_nr == SB_NR_EXECVP    ||*/
	     sb_nr == SB_NR_EXECVE))
	{
		retval = check_prefixes(sbcontext->read_prefixes,
					sbcontext->num_read_prefixes, resolv_path);
		if (1 == retval) {
			/* Fall in a readable path, Grant Access */
			result = 1;
			goto out;
		}

		/* If we are here, and still no joy, and its the access() call,
		 * do not log it, but just return -1 */
		if (sb_nr == SB_NR_ACCESS_RD) {
			sbcontext->show_access_violation = 0;
			goto out;
		}
	}

	if (sb_nr == SB_NR_ACCESS_WR   ||
	    sb_nr == SB_NR_OPEN_WR     ||
	    sb_nr == SB_NR_CREAT       ||
	    sb_nr == SB_NR_CREAT64     ||
	    sb_nr == SB_NR_MKDIR       ||
	  /*sb_nr == SB_NR_MKNOD       ||*/
	    sb_nr == SB_NR___XMKNOD    ||
	    sb_nr == SB_NR_MKFIFO      ||
	    sb_nr == SB_NR_LINK        ||
	    sb_nr == SB_NR_SYMLINK     ||
	    sb_nr == SB_NR_RENAME      ||
	    sb_nr == SB_NR_LUTIMES     ||
	    sb_nr == SB_NR_UTIMENSAT   ||
	    sb_nr == SB_NR_UTIME       ||
	    sb_nr == SB_NR_UTIMES      ||
	    sb_nr == SB_NR_FUTIMESAT   ||
	    sb_nr == SB_NR_UNLINK      ||
	    sb_nr == SB_NR_UNLINKAT    ||
	    sb_nr == SB_NR_RMDIR       ||
	    sb_nr == SB_NR_CHOWN       ||
	    sb_nr == SB_NR_FCHOWNAT    ||
	    sb_nr == SB_NR_LCHOWN      ||
	    sb_nr == SB_NR_CHMOD       ||
	    sb_nr == SB_NR_FCHMODAT    ||
	    sb_nr == SB_NR_TRUNCATE    ||
	  /*sb_nr == SB_NR_FTRUNCATE   ||*/
	    sb_nr == SB_NR_TRUNCATE64/*||
	    sb_nr == SB_NR_FTRUNCATE64*/)
	{

		retval = check_prefixes(sbcontext->write_denied_prefixes,
					sbcontext->num_write_denied_prefixes,
					resolv_path);
		if (1 == retval)
			/* Falls in a write denied path, Deny Access */
			goto out;

		retval = check_prefixes(sbcontext->write_prefixes,
					sbcontext->num_write_prefixes, resolv_path);
		if (1 == retval) {
			/* Falls in a writable path, Grant Access */
			result = 1;
			goto out;
		}

		/* XXX: Hack to enable us to remove symlinks pointing
		 * to protected stuff.  First we make sure that the
		 * passed path is writable, and if so, check if its a
		 * symlink, and give access only if the resolved path
		 * of the symlink's parent also have write access. */
		struct stat st;
		if ((sb_nr == SB_NR_UNLINK ||
		     sb_nr == SB_NR_UNLINKAT ||
		     sb_nr == SB_NR_LCHOWN ||
		     sb_nr == SB_NR_RENAME ||
		     sb_nr == SB_NR_SYMLINK) &&
		    ((-1 != lstat(abs_path, &st)) && (S_ISLNK(st.st_mode))))
		{
			/* Check if the symlink unresolved path have access */
			retval = check_prefixes(sbcontext->write_prefixes,
						sbcontext->num_write_prefixes, abs_path);
			if (1 == retval) { /* Does have write access on path */
				char tmp_buf[SB_PATH_MAX];
				char *dname, *rpath;

				snprintf(tmp_buf, SB_PATH_MAX, "%s", abs_path);

				dname = dirname(tmp_buf);
				/* Get symlink resolved path */
				rpath = resolve_path(dname, 1);
				if (NULL == rpath)
					/* Don't really worry here about
					 * memory issues */
					goto unlink_hack_end;

				/* Now check if the symlink resolved path have access */
				retval = check_prefixes(sbcontext->write_prefixes,
							sbcontext->num_write_prefixes,
							rpath);
				free(rpath);
				if (1 == retval) {
					/* Does have write access on path, so
					 * enable the hack as it is a symlink */
					result = 1;
					goto out;
				}
			}
		}
 unlink_hack_end: ;

		/* XXX: Hack to allow writing to '/proc/self/fd' (bug #91516)
		 *      It needs to be here, as for each process '/proc/self'
		 *      will differ ... */
		char proc_self_fd[SB_PATH_MAX];
		if ((0 == strcmp(resolv_path, PROC_DIR)) &&
		    (NULL != realpath(PROC_SELF_FD, proc_self_fd)))
		{
			if (0 == strcmp(resolv_path, proc_self_fd)) {
				result = 1;
				goto out;
			}
		}

		retval = check_prefixes(sbcontext->predict_prefixes,
					sbcontext->num_predict_prefixes, resolv_path);
		if (1 == retval) {
			/* Is a known access violation, so deny access,
			 * and do not log it */
			sbcontext->show_access_violation = 0;
			goto out;
		}

		/* If we are here, and still no joy, and its the access() call,
		 * do not log it, but just return -1 */
		if (sb_nr == SB_NR_ACCESS_WR) {
			sbcontext->show_access_violation = 0;
			goto out;
		}
	}

out:
	errno = old_errno;

	return result;
}

static int check_syscall(sbcontext_t *sbcontext, int sb_nr, const char *func, const char *file)
{
	char *absolute_path = NULL;
	char *resolved_path = NULL;
	char *log_path = NULL, *debug_log_path = NULL;
	int old_errno = errno;
	int result = 1;
	int access = 0, debug = 0, verbose = 1;

	absolute_path = resolve_path(file, 0);
	if (NULL == absolute_path)
		goto error;
	resolved_path = resolve_path(file, 1);
	if (NULL == resolved_path)
		goto error;

	log_path = getenv(ENV_SANDBOX_LOG);
	if (is_env_on(ENV_SANDBOX_DEBUG)) {
		debug_log_path = getenv(ENV_SANDBOX_DEBUG_LOG);
		debug = 1;
	}

	if (is_env_off(ENV_SANDBOX_VERBOSE)) {
		verbose = 0;
	}

	result = check_access(sbcontext, sb_nr, func, absolute_path, resolved_path);

	if (1 == verbose) {
		if ((0 == result) && (1 == sbcontext->show_access_violation)) {
			SB_EERROR("ACCESS DENIED", "  %s:%*s%s\n",
				func, (int)(10 - strlen(func)), "", absolute_path);
		} else if ((1 == debug) && (1 == sbcontext->show_access_violation)) {
			SB_EINFO("ACCESS ALLOWED", "  %s:%*s%s\n",
				func, (int)(10 - strlen(func)), "", absolute_path);
		} else if ((1 == debug) && (0 == sbcontext->show_access_violation)) {
			SB_EWARN("ACCESS PREDICTED", "  %s:%*s%s\n",
				func, (int)(10 - strlen(func)), "", absolute_path);
		}
	}

	if ((0 == result) && (1 == sbcontext->show_access_violation))
		access = 1;

	if ((NULL != log_path) && (1 == access)) {
		if (-1 == write_logfile(log_path, func, file, absolute_path,
								resolved_path, (access == 1) ? 0 : 1)) {
			if (0 != errno)
				goto error;
		}
	}

	if ((NULL != debug_log_path) && (1 == debug)) {
		if (-1 == write_logfile(debug_log_path, func, file, absolute_path,
								resolved_path, (access == 1) ? 0 : 1)) {
			if (0 != errno)
				goto error;
		}
	}

	if (NULL != absolute_path)
		free(absolute_path);
	if (NULL != resolved_path)
		free(resolved_path);

	errno = old_errno;

	return result;

error:
	if (NULL != absolute_path)
		free(absolute_path);
	if (NULL != resolved_path)
		free(resolved_path);

	/* The path is too long to be canonicalized, so just warn and let the
	 * function handle it (see bug #94630 and #21766 for more info) */
	if (ENAMETOOLONG == errno) {
		if (0 == sb_path_size_warning) {
			SB_EWARN("PATH LENGTH", "  %s:%*s%s\n",
			      func, (int)(10 - strlen(func)), "", file);
			sb_path_size_warning = 1;
		}

		return 1;
	}

	/* If we get here, something bad happened */
	SB_EERROR("ISE ", "Unrecoverable error: %s\n", strerror(errno));
	abort();
}

int is_sandbox_on(void)
{
	int result;
	save_errno();

	/* $SANDBOX_ACTIVE is an env variable that should ONLY
	 * be used internal by sandbox.c and libsanbox.c.  External
	 * sources should NEVER set it, else the sandbox is enabled
	 * in some cases when run in parallel with another sandbox,
	 * but not even in the sandbox shell.
	 */
	char *sb_env_active = getenv(ENV_SANDBOX_ACTIVE);
	if ((1 == sandbox_on) &&
	    sb_env_active &&
	    is_env_on(ENV_SANDBOX_ON) &&
	    (0 == strcmp(sb_env_active, SANDBOX_ACTIVE)))
		result = 1;
	else
		result = 0;
	restore_errno();
	return result;
}

int before_syscall(int dirfd, int sb_nr, const char *func, const char *file)
{
	int old_errno = errno;
	int result = 1;
//	static sbcontext_t sbcontext;
	char *at_file_buf = NULL;

	if (file == NULL || file[0] == '\0') {
		/* The file/directory does not exist */
		errno = ENOENT;
		return 0;
	}

	/* The *at style functions have the following semantics:
	 *	- dirfd = AT_FDCWD: same as non-at func: file is based on CWD
	 *	- file is absolute: dirfd is ignored
	 *	- otherwise, file is relative to dirfd
	 * Since maintaining fd state based on open's, we'll just utilize
	 * the kernel doing it for us with /proc/<pid>/fd/ ...
	 */
	if (dirfd != AT_FDCWD && file[0] != '/') {
		at_file_buf = xmalloc(50 + strlen(file));
		sprintf(at_file_buf, "/proc/%i/fd/%i/%s", getpid(), dirfd, file);
		file = at_file_buf;
	}

	if (0 == sb_init) {
		init_context(&sbcontext);
		cached_env_vars = xcalloc(4, sizeof(char *));
		sb_init = 1;
	}

	char *sb_env_names[4] = {
		ENV_SANDBOX_DENY,
		ENV_SANDBOX_READ,
		ENV_SANDBOX_WRITE,
		ENV_SANDBOX_PREDICT,
	};

	size_t i;
	for (i = 0; i < ARRAY_SIZE(sb_env_names); ++i) {
		char *sb_env = getenv(sb_env_names[i]);

		if ((!sb_env && cached_env_vars[i]) ||
		    !cached_env_vars[i] ||
		    strcmp(cached_env_vars[i], sb_env) != 0)
		{
			clean_env_entries(&(sbcontext.prefixes[i]),
				&(sbcontext.num_prefixes[i]));

			if (cached_env_vars[i])
				free(cached_env_vars[i]);

			if (sb_env) {
				init_env_entries(&(sbcontext.prefixes[i]),
					&(sbcontext.num_prefixes[i]), sb_env_names[i], sb_env, 1);
				cached_env_vars[i] = strdup(sb_env);
			} else
				cached_env_vars[i] = NULL;
		}
	}

	/* Might have been reset in check_access() */
	sbcontext.show_access_violation = 1;

	result = check_syscall(&sbcontext, sb_nr, func, file);

	if (at_file_buf)
		free(at_file_buf);

	errno = old_errno;

	if (0 == result) {
		if ((NULL != getenv(ENV_SANDBOX_PID)) && (is_env_on(ENV_SANDBOX_ABORT)))
			kill(atoi(getenv(ENV_SANDBOX_PID)), SIGUSR1);

		/* FIXME: Should probably audit errno, and enable some other
		 *        error to be returned (EINVAL for invalid mode for
		 *        fopen() and co, ETOOLONG, etc). */
		errno = EACCES;
	}

	return result;
}

int before_syscall_access(int dirfd, int sb_nr, const char *func, const char *file, int flags)
{
	const char *ext_func;
	if (flags & W_OK)
		sb_nr = SB_NR_ACCESS_WR, ext_func = "access_wr";
	else
		sb_nr = SB_NR_ACCESS_RD, ext_func = "access_rd";
	return before_syscall(dirfd, sb_nr, ext_func, file);
}

int before_syscall_open_int(int dirfd, int sb_nr, const char *func, const char *file, int flags)
{
	const char *ext_func;
	if ((flags & O_WRONLY) || (flags & O_RDWR))
		sb_nr = SB_NR_OPEN_WR, ext_func = "open_wr";
	else
		sb_nr = SB_NR_OPEN_RD, ext_func = "open_rd";
	return before_syscall(dirfd, sb_nr, ext_func, file);
}

int before_syscall_open_char(int dirfd, int sb_nr, const char *func, const char *file, const char *mode)
{
	if (NULL == mode)
		return 0;

	const char *ext_func;
	if ((*mode == 'r') && ((0 == (strcmp(mode, "r"))) ||
	     /* The strspn accept args are known non-writable modifiers */
	     (strlen(++mode) == strspn(mode, "xbtmce"))))
		sb_nr = SB_NR_OPEN_RD, ext_func = "open_rd";
	else
		sb_nr = SB_NR_OPEN_WR, ext_func = "open_wr";
	return before_syscall(dirfd, sb_nr, ext_func, file);
}
