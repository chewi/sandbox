/*
 *  Path sandbox for the gentoo linux portage package system, initially
 *  based on the ROCK Linux Wrapper for getting a list of created files
 *
 *  to integrate with bash, bash should have been built like this
 *
 *  ./configure --prefix=<prefix> --host=<host> --without-gnu-malloc
 *
 *  it's very important that the --enable-static-link option is NOT specified
 *
 *  Copyright (C) 2001 Geert Bevin, Uwyn, http://www.uwyn.com
 *  Distributed under the terms of the GNU General Public License, v2 or later 
 *  Author : Geert Bevin <gbevin@uwyn.com>
 *
 *  Post Bevin leaving Gentoo ranks:
 *  --------------------------------
 *    Ripped out all the wrappers, and implemented those of InstallWatch.
 *    Losts of cleanups and bugfixes.  Implement a execve that forces $LIBSANDBOX
 *    in $LD_PRELOAD.  Reformat the whole thing to look  somewhat like the reworked
 *    sandbox.c from Brad House <brad@mainstreetsoftworks.com>.
 *
 *    Martin Schlemmer <azarah@gentoo.org> (18 Aug 2002)
 *
 *  Partly Copyright (C) 1998-9 Pancrazio `Ezio' de Mauro <p@demauro.net>,
 *  as some of the InstallWatch code was used.
 *
 *
 *  $Header$
 *
 */

/* Uncomment below to enable the use of strtok_r(). */
#define REENTRANT_STRTOK 1

/* Uncomment below to enable memory debugging. */
/* #define SB_MEM_DEBUG 1 */

#define open   xxx_open
#define open64 xxx_open64

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/param.h>
#include <unistd.h>
#include <utime.h>

#include "config.h"
#include "localdecls.h"

#ifdef SB_MEM_DEBUG
# include <mcheck.h>
#endif

#undef open
#undef open64

#include "sandbox.h"

/* Macros to check if a function should be executed */
#define FUNCTION_SANDBOX_SAFE(_func, _path) \
	((0 == is_sandbox_on()) || (1 == before_syscall(_func, _path)))

#define FUNCTION_SANDBOX_SAFE_ACCESS(_func, _path, _flags) \
	((0 == is_sandbox_on()) || (1 == before_syscall_access(_func, _path, _flags)))

#define FUNCTION_SANDBOX_SAFE_OPEN_INT(_func, _path, _flags) \
	((0 == is_sandbox_on()) || (1 == before_syscall_open_int(_func, _path, _flags)))

#define FUNCTION_SANDBOX_SAFE_OPEN_CHAR(_func, _path, _mode) \
	((0 == is_sandbox_on()) || (1 == before_syscall_open_char(_func, _path, _mode)))

/* Macro to check if a wrapper is defined, if not
 * then try to resolve it again. */
#define check_dlsym(_name) \
{ \
	int old_errno = errno; \
	if (!true_ ## _name) \
		true_ ## _name = get_dlsym(symname_ ## _name, symver_ ## _name); \
	errno = old_errno; \
}

static char sandbox_lib[SB_PATH_MAX];

typedef struct {
	int show_access_violation;
	char **deny_prefixes;
	int num_deny_prefixes;
	char **read_prefixes;
	int num_read_prefixes;
	char **write_prefixes;
	int num_write_prefixes;
	char **predict_prefixes;
	int num_predict_prefixes;
	char **write_denied_prefixes;
	int num_write_denied_prefixes;
} sbcontext_t;

static sbcontext_t sbcontext;
static char **cached_env_vars;
static int sb_init = 0;
static int sb_path_size_warning = 0;

void __attribute__ ((constructor)) libsb_init(void);
void __attribute__ ((destructor)) libsb_fini(void);

static void *get_dlsym(const char *, const char *);
static int canonicalize(const char *, char *);
static char *resolve_path(const char *, int);
static int check_prefixes(char **, int, const char *);
static int check_access(sbcontext_t *, const char *, const char *, const char *);
static int check_syscall(sbcontext_t *, const char *, const char *);
static int before_syscall(const char *, const char *);
static int before_syscall_access(const char *, const char *, int);
static int before_syscall_open_int(const char *, const char *, int);
static int before_syscall_open_char(const char *, const char *, const char *);
static void clean_env_entries(char ***, int *);
static void init_context(sbcontext_t *);
static void init_env_entries(char ***, int *, const char *, const char *, int);
static int is_sandbox_on();

/*
 * Initialize the shabang
 */

static void *libc_handle = NULL;

void __attribute__ ((destructor)) libsb_fini(void)
{
	int x;

	sb_init = 0;
	
	if(NULL != cached_env_vars) {
		for(x=0; x < 4; x++) {
			if(NULL != cached_env_vars[x]) {
				free(cached_env_vars[x]);
				cached_env_vars[x] = NULL;
			}
		}
		free(cached_env_vars);
		cached_env_vars = NULL;
	}
	
	clean_env_entries(&(sbcontext.deny_prefixes),
			&(sbcontext.num_deny_prefixes));
	clean_env_entries(&(sbcontext.read_prefixes),
			&(sbcontext.num_read_prefixes));
	clean_env_entries(&(sbcontext.write_prefixes),
			&(sbcontext.num_write_prefixes));
	clean_env_entries(&(sbcontext.predict_prefixes),
			&(sbcontext.num_predict_prefixes));
}

void __attribute__ ((constructor)) libsb_init(void)
{
	int old_errno = errno;

#ifdef SB_MEM_DEBUG
	mtrace();
#endif

	/* Get the path and name to this library */
	get_sandbox_lib(sandbox_lib);

//	sb_init = 1;

	errno = old_errno;
}

static void *get_dlsym(const char *symname, const char *symver)
{
	void *symaddr = NULL;

	if (NULL == libc_handle) {
#ifdef BROKEN_RTLD_NEXT
		libc_handle = dlopen(LIBC_VERSION, RTLD_LAZY);
		if (!libc_handle) {
			fprintf(stderr, "libsandbox:  Can't dlopen libc: %s\n",
				dlerror());
			exit(EXIT_FAILURE);
		}
#else
		libc_handle = RTLD_NEXT;
#endif
	}

	if (NULL == symver)
		symaddr = dlsym(libc_handle, symname);
	else
		symaddr = dlvsym(libc_handle, symname, symver);
	if (!symaddr) {
		fprintf(stderr, "libsandbox:  Can't resolve %s: %s\n",
			symname, dlerror());
		exit(EXIT_FAILURE);
	}

	return symaddr;
}

static int canonicalize(const char *path, char *resolved_path)
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
		snprintf((char *)(resolved_path + strlen(resolved_path)),
			SB_PATH_MAX - strlen(resolved_path), "/%s", path);

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
	int old_errno = errno;
	char tmp_str1[SB_PATH_MAX], tmp_str2[SB_PATH_MAX];
	char *dname, *bname;
	char *filtered_path;

	if (NULL == path)
		return NULL;

	filtered_path = malloc(SB_PATH_MAX * sizeof(char));
	if (NULL == filtered_path)
		return NULL;

	if (0 == follow_link) {
		if (-1 == canonicalize(path, filtered_path))
			return NULL;
	} else {
		/* Basically we get the realpath which should resolve symlinks,
		 * etc.  If that fails (might not exist), we try to get the
		 * realpath of the parent directory, as that should hopefully
		 * exist.  If all else fails, just go with canonicalize */
		if (NULL == realpath(path, filtered_path)) {
			snprintf(tmp_str1, SB_PATH_MAX, "%s", path);
			
			dname = dirname(tmp_str1);
			
			/* If not, then check if we can resolve the
			 * parent directory */
			if (NULL == realpath(dname, filtered_path)) {
				/* Fall back to canonicalize */
				if (-1 == canonicalize(path, filtered_path))
					return NULL;
			} else {
				/* OK, now add the basename to keep our access
				 * checking happy (don't want '/usr/lib' if we
				 * tried to do something with non-existing
				 * file '/usr/lib/cf*' ...) */
				snprintf(tmp_str2, SB_PATH_MAX, "%s", path);

				bname = basename(tmp_str2);
				snprintf((char *)(filtered_path + strlen(filtered_path)),
					SB_PATH_MAX - strlen(filtered_path), "%s%s",
					(filtered_path[strlen(filtered_path) - 1] != '/') ? "/" : "",
					bname);
			}
		}
	}
	
	errno = old_errno;

	return filtered_path;
}

/*
 * Wrapper Functions
 */

#define chmod_decl(_name) \
\
extern int _name(const char *, mode_t); \
static int (*true_ ## _name) (const char *, mode_t) = NULL; \
\
int _name(const char *path, mode_t mode) \
{ \
	int result = -1; \
\
	if FUNCTION_SANDBOX_SAFE("chmod", path) { \
		check_dlsym(_name); \
		result = true_ ## _name(path, mode); \
	} \
\
	return result; \
}

#define chown_decl(_name) \
\
extern int _name(const char *, uid_t, gid_t); \
static int (*true_ ## _name) (const char *, uid_t, gid_t) = NULL; \
\
int _name(const char *path, uid_t owner, gid_t group) \
{ \
	int result = -1; \
\
	if FUNCTION_SANDBOX_SAFE("chown", path) { \
		check_dlsym(_name); \
		result = true_ ## _name(path, owner, group); \
	} \
\
	return result; \
}

#define creat_decl(_name) \
\
extern int _name(const char *, mode_t); \
/* static int (*true_ ## _name) (const char *, mode_t) = NULL; */ \
\
int _name(const char *pathname, mode_t mode) \
{ \
	int result = -1; \
\
	if FUNCTION_SANDBOX_SAFE("creat", pathname) { \
		check_dlsym(open_DEFAULT); \
		result = true_open_DEFAULT(pathname, O_CREAT | O_WRONLY | O_TRUNC, mode); \
	} \
\
	return result; \
}

#define fopen_decl(_name) \
\
extern FILE *_name(const char *, const char *); \
static FILE * (*true_ ## _name) (const char *, const char *) = NULL; \
\
FILE *_name(const char *pathname, const char *mode) \
{ \
	FILE *result = NULL; \
\
	if FUNCTION_SANDBOX_SAFE_OPEN_CHAR("fopen", pathname, mode) { \
		check_dlsym(_name); \
		result = true_ ## _name(pathname, mode); \
	} \
\
	return result; \
}

#define lchown_decl(_name) \
\
extern int _name(const char *, uid_t, gid_t); \
static int (*true_ ## _name) (const char *, uid_t, gid_t) = NULL; \
\
int _name(const char *path, uid_t owner, gid_t group) \
{ \
	int result = -1; \
\
	if FUNCTION_SANDBOX_SAFE("lchown", path) { \
		check_dlsym(_name); \
		result = true_ ## _name(path, owner, group); \
	} \
\
	return result; \
}

#define link_decl(_name) \
\
extern int _name(const char *, const char *); \
static int (*true_ ## _name) (const char *, const char *) = NULL; \
\
int _name(const char *oldpath, const char *newpath) \
{ \
	int result = -1; \
\
	if FUNCTION_SANDBOX_SAFE("link", newpath) { \
		check_dlsym(_name); \
		result = true_ ## _name(oldpath, newpath); \
	} \
\
	return result; \
}

#define mkdir_decl(_name) \
\
extern int _name(const char *, mode_t); \
static int (*true_ ## _name) (const char *, mode_t) = NULL; \
\
int _name(const char *pathname, mode_t mode) \
{ \
	struct stat st; \
	int result = -1, my_errno = errno; \
	char canonic[SB_PATH_MAX]; \
\
	if (-1 == canonicalize(pathname, canonic)) \
		/* Path is too long to canonicalize, do not fail, but just let 
		 * the real function handle it (see bug #94630 and #21766). */ \
		if (ENAMETOOLONG != errno) \
			return -1; \
\
	/* XXX: Hack to prevent errors if the directory exist,
	 * and are not writable - we rather return EEXIST rather
	 * than failing */ \
	if (0 == lstat(canonic, &st)) { \
		errno = EEXIST; \
		return -1; \
	} \
	errno = my_errno; \
\
	if FUNCTION_SANDBOX_SAFE("mkdir", pathname) { \
		check_dlsym(_name); \
		result = true_ ## _name(pathname, mode); \
	} \
\
	return result; \
}

#define opendir_decl(_name) \
\
extern DIR *_name(const char *); \
static DIR * (*true_ ## _name) (const char *) = NULL; \
\
DIR *_name(const char *name) \
{ \
	DIR *result = NULL; \
\
	if FUNCTION_SANDBOX_SAFE("opendir", name) { \
		check_dlsym(_name); \
		result = true_ ## _name(name); \
	} \
\
	return result; \
}

#define mknod_decl(_name) \
\
extern int _name(const char *, mode_t, dev_t); \
static int (*true_ ## _name) (const char *, mode_t, dev_t) = NULL; \
\
int _name(const char *pathname, mode_t mode, dev_t dev) \
{ \
	int result = -1; \
\
	if FUNCTION_SANDBOX_SAFE("mknod", pathname) { \
		check_dlsym(_name); \
		result = true_ ## _name(pathname, mode, dev); \
	} \
\
	return result; \
}

#define __xmknod_decl(_name) \
\
extern int _name(int, const char *, __mode_t, __dev_t *); \
static int (*true_ ## _name) (int, const char *, __mode_t, __dev_t *) = NULL; \
\
int _name(int ver, const char *pathname, __mode_t mode, __dev_t *dev) \
{ \
	int result = -1; \
\
	if FUNCTION_SANDBOX_SAFE("mknod", pathname) { \
		check_dlsym(_name); \
		result = true_ ## _name(ver, pathname, mode, dev); \
	} \
\
	return result; \
}

#define mkfifo_decl(_name) \
\
extern int _name(const char *, mode_t); \
static int (*true_ ## _name) (const char *, mode_t) = NULL; \
\
int _name(const char *pathname, mode_t mode) \
{ \
	int result = -1; \
\
	if FUNCTION_SANDBOX_SAFE("mkfifo", pathname) { \
		check_dlsym(_name); \
		result = true_ ## _name(pathname, mode); \
	} \
\
	return result; \
}

#define access_decl(_name) \
\
extern int _name(const char *, int); \
static int (*true_ ## _name) (const char *, int) = NULL; \
\
int _name(const char *pathname, int mode) \
{ \
	int result = -1; \
\
	if FUNCTION_SANDBOX_SAFE_ACCESS("access", pathname, mode) { \
		check_dlsym(_name); \
		result = true_ ## _name(pathname, mode); \
	} \
\
	return result; \
}

#define open_decl(_name) \
\
extern int _name(const char *, int, ...); \
static int (*true_ ## _name) (const char *, int, ...) = NULL; \
\
/* Eventually, there is a third parameter: it's mode_t mode */ \
int _name(const char *pathname, int flags, ...) \
{ \
	va_list ap; \
	mode_t mode = 0; \
	int result = -1; \
\
	if (flags & O_CREAT) { \
		va_start(ap, flags); \
		mode = va_arg(ap, mode_t); \
		va_end(ap); \
	} \
\
	if FUNCTION_SANDBOX_SAFE_OPEN_INT("open", pathname, flags) { \
		check_dlsym(_name); \
		if (flags & O_CREAT) \
			result = true_ ## _name(pathname, flags, mode); \
		else \
			result = true_ ## _name(pathname, flags); \
	} \
\
	return result; \
}

#define rename_decl(_name) \
\
extern int _name(const char *, const char *); \
static int (*true_ ## _name) (const char *, const char *) = NULL; \
\
int _name(const char *oldpath, const char *newpath) \
{ \
	int result = -1; \
\
	if (FUNCTION_SANDBOX_SAFE("rename", oldpath) && \
	    FUNCTION_SANDBOX_SAFE("rename", newpath)) { \
		check_dlsym(_name); \
		result = true_ ## _name(oldpath, newpath); \
	} \
\
	return result; \
}

#define rmdir_decl(_name) \
\
extern int _name(const char *); \
static int (*true_ ## _name) (const char *) = NULL; \
\
int _name(const char *pathname) \
{ \
	int result = -1; \
\
	if FUNCTION_SANDBOX_SAFE("rmdir", pathname) { \
		check_dlsym(_name); \
		result = true_ ## _name(pathname); \
	} \
\
	return result; \
}

#define symlink_decl(_name) \
\
extern int _name(const char *, const char *); \
static int (*true_ ## _name) (const char *, const char *) = NULL; \
\
int _name(const char *oldpath, const char *newpath) \
{ \
	int result = -1; \
\
	if FUNCTION_SANDBOX_SAFE("symlink", newpath) { \
		check_dlsym(_name); \
		result = true_ ## _name(oldpath, newpath); \
	} \
\
	return result; \
}

#define truncate_decl(_name) \
\
extern int _name(const char *, TRUNCATE_T); \
static int (*true_ ## _name) (const char *, TRUNCATE_T) = NULL; \
\
int _name(const char *path, TRUNCATE_T length) \
{ \
	int result = -1; \
\
	if FUNCTION_SANDBOX_SAFE("truncate", path) { \
		check_dlsym(_name); \
		result = true_ ## _name(path, length); \
	} \
\
	return result; \
}

#define unlink_decl(_name) \
\
extern int _name(const char *); \
static int (*true_ ## _name) (const char *) = NULL; \
\
int _name(const char *pathname) \
{ \
	int result = -1; \
	char canonic[SB_PATH_MAX]; \
\
	if (-1 == canonicalize(pathname, canonic)) \
		/* Path is too long to canonicalize, do not fail, but just let
		 * the real function handle it (see bug #94630 and #21766). */ \
		if (ENAMETOOLONG != errno) \
			return -1; \
\
	/* XXX: Hack to make sure sandboxed process cannot remove
	 * a device node, bug #79836. */ \
	if ((0 == strncmp(canonic, "/dev/null", 9)) || \
	    (0 == strncmp(canonic, "/dev/zero", 9))) { \
		errno = EACCES; \
		return result; \
	} \
\
	if FUNCTION_SANDBOX_SAFE("unlink", pathname) { \
		check_dlsym(_name); \
		result = true_ ## _name(pathname); \
	} \
\
	return result; \
}

#define creat64_decl(_name) \
\
extern int _name(const char *, __mode_t); \
/* static int (*true_ ## _name) (const char *, __mode_t) = NULL; */ \
\
int _name(const char *pathname, __mode_t mode) \
{ \
	int result = -1; \
\
	if FUNCTION_SANDBOX_SAFE("creat64", pathname) { \
		check_dlsym(open64_DEFAULT); \
		result = true_open64_DEFAULT(pathname, O_CREAT | O_WRONLY | O_TRUNC, mode); \
	} \
\
	return result; \
}

#define fopen64_decl(_name) \
\
extern FILE *_name(const char *, const char *); \
static FILE * (*true_ ## _name) (const char *, const char *) = NULL; \
\
FILE *_name(const char *pathname, const char *mode) \
{ \
	FILE *result = NULL; \
\
	if FUNCTION_SANDBOX_SAFE_OPEN_CHAR("fopen64", pathname, mode) { \
		check_dlsym(_name); \
		result = true_ ## _name(pathname, mode); \
	} \
\
	return result; \
}

#define open64_decl(_name) \
\
extern int _name(const char *, int, ...); \
static int (*true_ ## _name) (const char *, int, ...) = NULL; \
\
/* Eventually, there is a third parameter: it's mode_t mode */ \
int _name(const char *pathname, int flags, ...) \
{ \
	va_list ap; \
	mode_t mode = 0; \
	int result = -1; \
\
	if (flags & O_CREAT) { \
		va_start(ap, flags); \
		mode = va_arg(ap, mode_t); \
		va_end(ap); \
	} \
\
	if FUNCTION_SANDBOX_SAFE_OPEN_INT("open64", pathname, flags) { \
		check_dlsym(_name); \
		if (flags & O_CREAT) \
			result = true_ ## _name(pathname, flags, mode); \
		else \
			result = true_ ## _name(pathname, flags); \
	} \
\
	return result; \
}

#define truncate64_decl(_name) \
\
extern int _name(const char *, __off64_t); \
static int (*true_ ## _name) (const char *, __off64_t) = NULL; \
\
int _name(const char *path, __off64_t length) \
{ \
	int result = -1; \
\
	if FUNCTION_SANDBOX_SAFE("truncate64", path) { \
		check_dlsym(_name); \
		result = true_ ## _name(path, length); \
	} \
\
	return result; \
}

/*
 * Exec Wrappers
 */

#define execve_decl(_name) \
\
extern int _name(const char *, char *const[], char *const[]); \
static int (*true_ ## _name) (const char *, char *const[], char *const[]) = NULL; \
\
int _name(const char *filename, char *const argv[], char *const envp[]) \
{ \
	int old_errno = errno; \
	int result = -1; \
	int count = 0; \
	int env_len = 0; \
	char **my_env = NULL; \
	int kill_env = 1; \
	/* We limit the size LD_PRELOAD can be here, but it should be enough */ \
	char tmp_str[SB_BUF_LEN]; \
\
	if FUNCTION_SANDBOX_SAFE("execve", filename) { \
		while (envp[count] != NULL) { \
			/* Check if we do not have to do anything */ \
			if (strstr(envp[count], LD_PRELOAD_EQ) == envp[count]) { \
				if (NULL != strstr(envp[count], sandbox_lib)) { \
					my_env = (char **)envp; \
					kill_env = 0; \
					goto end_loop; \
				} \
			} \
\
			/* If LD_PRELOAD is set and sandbox_lib not in it */ \
			if (((strstr(envp[count], LD_PRELOAD_EQ) == envp[count]) && \
			     (NULL == strstr(envp[count], sandbox_lib))) || \
			    /* Or  LD_PRELOAD is not set, and this is the last loop */ \
			    ((strstr(envp[count], LD_PRELOAD_EQ) != envp[count]) && \
			     (NULL == envp[count + 1]))) { \
				int i = 0; \
				int add_ldpreload = 0; \
				const int max_envp_len = strlen(envp[count]) + strlen(sandbox_lib) + 1; \
\
				/* Fail safe ... */ \
				if (max_envp_len > SB_BUF_LEN) { \
					fprintf(stderr, "libsandbox:  max_envp_len too big!\n"); \
					errno = ENOMEM; \
					return result; \
				} \
\
				/* Calculate envp size */ \
				my_env = (char **)envp; \
				do \
					env_len++; \
				while (NULL != *my_env++); \
\
				/* Should we add LD_PRELOAD ? */ \
				if (strstr(envp[count], LD_PRELOAD_EQ) != envp[count]) \
					add_ldpreload = 1; \
\
				my_env = (char **)calloc(env_len + add_ldpreload, sizeof(char *)); \
				if (NULL == my_env) { \
					errno = ENOMEM; \
					return result; \
				} \
				/* Copy envp to my_env */ \
				do \
					/* Leave a space for LD_PRELOAD if needed */ \
					my_env[i + add_ldpreload] = envp[i]; \
				while (NULL != envp[i++]); \
\
				/* Add 'LD_PRELOAD=' to the beginning of our new string */ \
				snprintf(tmp_str, max_envp_len, "%s%s", LD_PRELOAD_EQ, sandbox_lib); \
\
				/* LD_PRELOAD already have variables other than sandbox_lib,
				 * thus we have to add sandbox_lib seperated via a whitespace. */ \
				if (0 == add_ldpreload) { \
					snprintf((char *)(tmp_str + strlen(tmp_str)), \
						 max_envp_len - strlen(tmp_str) + 1, " %s", \
						 (char *)(envp[count] + strlen(LD_PRELOAD_EQ))); \
				} \
\
				/* Valid string? */ \
				tmp_str[max_envp_len] = '\0'; \
\
				/* Ok, replace my_env[count] with our version that contains
				 * sandbox_lib ... */ \
				if (1 == add_ldpreload) \
					/* We reserved a space for LD_PRELOAD above */ \
					my_env[0] = tmp_str; \
				else \
					my_env[count] = tmp_str; \
\
				goto end_loop; \
			} \
			count++; \
		} \
\
end_loop: \
		errno = old_errno; \
		check_dlsym(_name); \
		result = true_ ## _name(filename, argv, my_env); \
		old_errno = errno; \
\
		if (my_env && kill_env) \
			free(my_env); \
	} \
\
	errno = old_errno; \
\
	return result; \
}

#include "symbols.h"

/*
 * Internal Functions
 */

static void init_context(sbcontext_t * context)
{
	context->show_access_violation = 1;
	context->deny_prefixes = NULL;
	context->num_deny_prefixes = 0;
	context->read_prefixes = NULL;
	context->num_read_prefixes = 0;
	context->write_prefixes = NULL;
	context->num_write_prefixes = 0;
	context->predict_prefixes = NULL;
	context->num_predict_prefixes = 0;
	context->write_denied_prefixes = NULL;
	context->num_write_denied_prefixes = 0;
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
	pfx_array = malloc(((num_delimiters * 2) + 2) * sizeof(char *));
	if (NULL == pfx_array)
		goto error;
	buffer = strndup(prefixes_env, prefixes_env_length);
	if (NULL == buffer)
		goto error;
	buffer_ptr = buffer;

#ifdef REENTRANT_STRTOK
	token = strtok_r(buffer_ptr, ":", &buffer_ptr);
#else
	token = strtok(buffer_ptr, ":");
#endif

	while ((NULL != token) && (strlen(token) > 0)) {
		pfx_item = resolve_path(token, 0);
		if (NULL != pfx_item) {
			pfx_num++;

			/* Now add the realpath if it exists and
			 * are not a duplicate */
			rpath = malloc(SB_PATH_MAX * sizeof(char));
			if (NULL != rpath) {
				pfx_item = realpath(*(&(pfx_item) - 1), rpath);
				if ((NULL != pfx_item) &&
				    (0 != strcmp(*(&(pfx_item) - 1), pfx_item))) {
					pfx_num++;
				} else {
					free(rpath);
					pfx_item = NULL;
				}
			} else {
				goto error;
			}
		}

#ifdef REENTRANT_STRTOK
		token = strtok_r(NULL, ":", &buffer_ptr);
#else
		token = strtok(NULL, ":");
#endif
	}

	free(buffer);

done:
	errno = old_errno;
	return;

error:
	perror("libsandbox:  Could not initialize environ\n");
	exit(EXIT_FAILURE);
}

static int check_prefixes(char **prefixes, int num_prefixes, const char *path)
{
	int i = 0;

	if (NULL == prefixes)
		return 0;
	
	for (i = 0; i < num_prefixes; i++) {
		if (NULL != prefixes[i]) {
			if (0 == strncmp(path, prefixes[i], strlen(prefixes[i])))
				return 1;
		}
	}

	return 0;
}

static int check_access(sbcontext_t * sbcontext, const char *func, const char *abs_path, const char *resolv_path)
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
	if (0 == strncmp(resolv_path, SANDBOX_LOG_LOCATION,
			 strlen(SANDBOX_LOG_LOCATION)))
		goto out;

	if ((NULL != sbcontext->read_prefixes) &&
	    ((0 == strncmp(func, "access_rd", 9)) ||
	     (0 == strncmp(func, "open_rd", 7)) ||
	     (0 == strncmp(func, "popen", 5)) ||
	     (0 == strncmp(func, "opendir", 7)) ||
	     (0 == strncmp(func, "system", 6)) ||
	     (0 == strncmp(func, "execl", 5)) ||
	     (0 == strncmp(func, "execlp", 6)) ||
	     (0 == strncmp(func, "execle", 6)) ||
	     (0 == strncmp(func, "execv", 5)) ||
	     (0 == strncmp(func, "execvp", 6)) ||
	     (0 == strncmp(func, "execve", 6)))) {
		retval = check_prefixes(sbcontext->read_prefixes,
					sbcontext->num_read_prefixes, resolv_path);
		if (1 == retval) {
			/* Fall in a readable path, Grant Access */
			result = 1;
			goto out;
		}

		/* If we are here, and still no joy, and its the access() call,
		 * do not log it, but just return -1 */
		if (0 == strncmp(func, "access_rd", 7)) {
			sbcontext->show_access_violation = 0;
			goto out;
		}
	}
		
	if ((0 == strncmp(func, "access_wr", 7)) ||
	    (0 == strncmp(func, "open_wr", 7)) ||
	    (0 == strncmp(func, "creat", 5)) ||
	    (0 == strncmp(func, "creat64", 7)) ||
	    (0 == strncmp(func, "mkdir", 5)) ||
	    (0 == strncmp(func, "mknod", 5)) ||
	    (0 == strncmp(func, "mkfifo", 6)) ||
	    (0 == strncmp(func, "link", 4)) ||
	    (0 == strncmp(func, "symlink", 7)) ||
	    (0 == strncmp(func, "rename", 6)) ||
	    (0 == strncmp(func, "utime", 5)) ||
	    (0 == strncmp(func, "utimes", 6)) ||
	    (0 == strncmp(func, "unlink", 6)) ||
	    (0 == strncmp(func, "rmdir", 5)) ||
	    (0 == strncmp(func, "chown", 5)) ||
	    (0 == strncmp(func, "lchown", 6)) ||
	    (0 == strncmp(func, "chmod", 5)) ||
	    (0 == strncmp(func, "truncate", 8)) ||
	    (0 == strncmp(func, "ftruncate", 9)) ||
	    (0 == strncmp(func, "truncate64", 10)) ||
	    (0 == strncmp(func, "ftruncate64", 11))) {
		struct stat st;
		char proc_self_fd[SB_PATH_MAX];

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
		if (((0 == strncmp(func, "unlink", 6)) ||
		     (0 == strncmp(func, "lchown", 6)) ||
		     (0 == strncmp(func, "rename", 6)) ||
		     (0 == strncmp(func, "symlink", 7))) &&
		    ((-1 != lstat(abs_path, &st)) && (S_ISLNK(st.st_mode)))) {
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
unlink_hack_end:

		/* XXX: Hack to allow writing to '/proc/self/fd' (bug #91516)
		 *      It needs to be here, as for each process '/proc/self'
		 *      will differ ... */
		if ((0 == strncmp(resolv_path, "/proc", strlen("/proc"))) &&
		    (NULL != realpath("/proc/self/fd", proc_self_fd))) {
			if (0 == strncmp(resolv_path, proc_self_fd,
					 strlen(proc_self_fd))) {
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
		if (0 == strncmp(func, "access_wr", 7)) {
			sbcontext->show_access_violation = 0;
			goto out;
		}
	}

out:
	errno = old_errno;

	return result;
}

static int check_syscall(sbcontext_t * sbcontext, const char *func, const char *file)
{
	struct stat log_stat;
	char buffer[512];
	char *absolute_path = NULL;
	char *resolved_path = NULL;
	char *log_path = NULL, *debug_log_path = NULL;
	int old_errno = errno;
	int result = 1;
	int log_file = 0, debug_log_file = 0;
	int access = 0, debug = 0, verbose = 1;
	int color = ((getenv("NOCOLOR") != NULL) ? 0 : 1);

	absolute_path = resolve_path(file, 0);
	if (NULL == absolute_path)
		goto fp_error;
	resolved_path = resolve_path(file, 1);
	if (NULL == resolved_path)
		goto fp_error;

	log_path = getenv(ENV_SANDBOX_LOG);
	if (NULL != getenv(ENV_SANDBOX_DEBUG)) {
		if ((0 == strncasecmp(getenv(ENV_SANDBOX_DEBUG), "1", 1)) ||
		    (0 == strncasecmp(getenv(ENV_SANDBOX_DEBUG), "yes", 3))) {
			debug_log_path = getenv(ENV_SANDBOX_DEBUG_LOG);
			debug = 1;
		}
	}

	if (NULL != getenv(ENV_SANDBOX_VERBOSE)) {
		if ((0 == strncasecmp(getenv(ENV_SANDBOX_VERBOSE), "0", 1)) ||
		    (0 == strncasecmp(getenv(ENV_SANDBOX_VERBOSE), "no", 2)))
			verbose = 0;
	}

	result = check_access(sbcontext, func, absolute_path, resolved_path);

	if (1 == verbose) {
		if ((0 == result) && (1 == sbcontext->show_access_violation)) {
			EERROR(color, "ACCESS DENIED", "  %s:%*s%s\n",
				func, (int)(10 - strlen(func)), "", absolute_path);
		} else if ((1 == debug) && (1 == sbcontext->show_access_violation)) {
			EINFO(color, "ACCESS ALLOWED", "  %s:%*s%s\n",
				func, (int)(10 - strlen(func)), "", absolute_path);
		} else if ((1 == debug) && (0 == sbcontext->show_access_violation)) {
			EWARN(color, "ACCESS PREDICTED", "  %s:%*s%s\n",
				func, (int)(10 - strlen(func)), "", absolute_path);
		}
	}

	if ((0 == result) && (1 == sbcontext->show_access_violation))
		access = 1;

	if (((NULL != log_path) && (1 == access)) ||
	    ((NULL != debug_log_path) && (1 == debug))) {
		if (0 != strncmp(absolute_path, resolved_path, strlen(absolute_path))) {
			sprintf(buffer, "%s:%*s%s (symlink to %s)\n", func,
					(int)(10 - strlen(func)), "",
					absolute_path, resolved_path);
		} else {
			sprintf(buffer, "%s:%*s%s\n", func,
					(int)(10 - strlen(func)), "",
					absolute_path);
		}
		if (1 == access) {
			if ((0 == lstat(log_path, &log_stat)) &&
			    (0 == S_ISREG(log_stat.st_mode))) {
				EERROR(color, "SECURITY BREACH", "  '%s' %s\n", log_path,
					"already exists and is not a regular file!");
			} else {
				check_dlsym(open_DEFAULT);
				log_file = true_open_DEFAULT(log_path, O_APPEND | O_WRONLY |
						O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP |
						S_IROTH);
				if (log_file >= 0) {
					write(log_file, buffer, strlen(buffer));
					close(log_file);
				}
			}
		} 
		if (1 == debug) {
			if ((0 == lstat(debug_log_path, &log_stat)) &&
			    (0 == S_ISREG(log_stat.st_mode))) {
				EERROR(color, "SECURITY BREACH", "  '%s' %s\n", debug_log_path,
					"already exists and is not a regular file!");
			} else {
				check_dlsym(open_DEFAULT);
				debug_log_file = true_open_DEFAULT(debug_log_path, O_APPEND | O_WRONLY |
						O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP |
						S_IROTH);
				if (debug_log_file >= 0) {
					write(debug_log_file, buffer, strlen(buffer));
					close(debug_log_file);
				}
			}
		}
	}

	if (NULL != absolute_path)
		free(absolute_path);
	if (NULL != resolved_path)
		free(resolved_path);

	errno = old_errno;

	return result;

fp_error:
	if (NULL != absolute_path)
		free(absolute_path);
	if (NULL != resolved_path)
		free(resolved_path);
	
	/* The path is too long to be canonicalized, so just warn and let the
	 * function handle it (see bug #94630 and #21766 for more info) */
	if (ENAMETOOLONG == errno) {
		if (0 == sb_path_size_warning) {
			EWARN(color, "PATH LENGTH", "  %s:%*s%s\n",
			      func, (int)(10 - strlen(func)), "", file);
			sb_path_size_warning = 1;
		}
			
		return 1;
	}

	return 0;
}

static int is_sandbox_on()
{
	int old_errno = errno;

	/* $SANDBOX_ACTIVE is an env variable that should ONLY
	 * be used internal by sandbox.c and libsanbox.c.  External
	 * sources should NEVER set it, else the sandbox is enabled
	 * in some cases when run in parallel with another sandbox,
	 * but not even in the sandbox shell.
	 *
	 * Azarah (3 Aug 2002)
	 */
	if ((NULL != getenv(ENV_SANDBOX_ON)) &&
	    ((0 == strncmp(getenv(ENV_SANDBOX_ON), "1", 1)) ||
	     (0 == strncmp(getenv(ENV_SANDBOX_ON), "yes", 3))) &&
	    (NULL != getenv(ENV_SANDBOX_ACTIVE)) &&
	    (0 == strncmp(getenv(ENV_SANDBOX_ACTIVE), SANDBOX_ACTIVE, 13))) {
		errno = old_errno;
		return 1;
	} else {
		errno = old_errno;
		return 0;
	}
}

static int before_syscall(const char *func, const char *file)
{
	int old_errno = errno;
	int result = 1;
//	static sbcontext_t sbcontext;
	char *deny = getenv(ENV_SANDBOX_DENY);
	char *read = getenv(ENV_SANDBOX_READ);
	char *write = getenv(ENV_SANDBOX_WRITE);
	char *predict = getenv(ENV_SANDBOX_PREDICT);

	if (NULL == file || 0 == strlen(file)) {
		/* The file/directory does not exist */
		errno = ENOENT;
		return 0;
	}

	if(0 == sb_init) {
		init_context(&sbcontext);
		cached_env_vars = malloc(sizeof(char *) * 4);
		cached_env_vars[0] = cached_env_vars[1] = cached_env_vars[2] = cached_env_vars[3] = NULL;
		sb_init = 1;
	}

	if((NULL == deny && cached_env_vars[0] != deny) || NULL == cached_env_vars[0] ||
		strcmp(cached_env_vars[0], deny) != 0) {

		clean_env_entries(&(sbcontext.deny_prefixes),
			&(sbcontext.num_deny_prefixes));

		if(NULL != cached_env_vars[0])
			free(cached_env_vars[0]);

		if(NULL != deny) {
			init_env_entries(&(sbcontext.deny_prefixes),
				&(sbcontext.num_deny_prefixes), ENV_SANDBOX_DENY, deny, 1);
			cached_env_vars[0] = strdup(deny);
		} else {
			cached_env_vars[0] = NULL;
		}
	}

	if((NULL == read && cached_env_vars[1] != read) || NULL == cached_env_vars[1] || 
		strcmp(cached_env_vars[1], read) != 0) {

		clean_env_entries(&(sbcontext.read_prefixes),
			&(sbcontext.num_read_prefixes));

		if(NULL != cached_env_vars[1])
			free(cached_env_vars[1]);

		if(NULL != read) {
			init_env_entries(&(sbcontext.read_prefixes),
				&(sbcontext.num_read_prefixes), ENV_SANDBOX_READ, read, 1);
			cached_env_vars[1] = strdup(read);
		} else {
			cached_env_vars[1] = NULL;
		}
	}

	if((NULL == write && cached_env_vars[2] != write) || NULL == cached_env_vars[2] ||
		strcmp(cached_env_vars[2], write) != 0) {

		clean_env_entries(&(sbcontext.write_prefixes),
			&(sbcontext.num_write_prefixes));

		if(NULL != cached_env_vars[2])
			free(cached_env_vars[2]);

		if(NULL != write) {
			init_env_entries(&(sbcontext.write_prefixes),
				&(sbcontext.num_write_prefixes), ENV_SANDBOX_WRITE, write, 1);
			cached_env_vars[2] = strdup(write);
		} else {
			cached_env_vars[2] = NULL;
		}
	}

	if((NULL == predict && cached_env_vars[3] != predict) || NULL == cached_env_vars[3] ||
		strcmp(cached_env_vars[3], predict) != 0) {

		clean_env_entries(&(sbcontext.predict_prefixes),
			&(sbcontext.num_predict_prefixes));

		if(NULL != cached_env_vars[3])
			free(cached_env_vars[3]);

		if(NULL != predict) {
			init_env_entries(&(sbcontext.predict_prefixes),
				&(sbcontext.num_predict_prefixes), ENV_SANDBOX_PREDICT, predict, 1);
			cached_env_vars[3] = strdup(predict);
		} else {
			cached_env_vars[3] = NULL;
		}

	}

	/* Might have been reset in check_access() */
	sbcontext.show_access_violation = 1;

	result = check_syscall(&sbcontext, func, file);

	errno = old_errno;

	if (0 == result) {
		errno = EACCES;
	}

	return result;
}

static int before_syscall_access(const char *func, const char *file, int flags)
{
	if (flags & W_OK) {
		return before_syscall("access_wr", file);
	} else {
		return before_syscall("access_rd", file);
	}
}

static int before_syscall_open_int(const char *func, const char *file, int flags)
{
	if ((flags & O_WRONLY) || (flags & O_RDWR)) {
		return before_syscall("open_wr", file);
	} else {
		return before_syscall("open_rd", file);
	}
}

static int before_syscall_open_char(const char *func, const char *file, const char *mode)
{
	if (*mode == 'r' && (0 == (strcmp(mode, "r")) ||
	    /* The strspn accept args are known non-writable modifiers */
	    (strlen(++mode) == strspn(mode, "xbtmc")))) {
		return before_syscall("open_rd", file);
	} else {
		return before_syscall("open_wr", file);
	}
}


// vim:noexpandtab noai:cindent ai
