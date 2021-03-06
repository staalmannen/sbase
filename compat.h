/* See LICENSE file for copyright and license details. */
#include <limits.h>
#include <sys/limits.h>

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX _POSIX_HOST_NAME_MAX
#endif

/* Plan9 hacks */

#ifndef LLONG_MAX
#define LLONG_MAX LONG_MAX
#endif

#ifndef LLONG_MIN
#define LLONG_MIN LONG_MIN
#endif

#define _POSIX_HOST_NAME_MAX _POSIX_NAME_MAX

#define lchown chown

#ifndef PRIu32
#define PRIu32 "lu"
#endif

#define intmax_t int32_t
#define uintmax_t uint32_t

#define st_rdev st_dev

/* nothing */
#define S_ISVTX 0001000
#define S_ISSOCK(m)	(((m)&S__MASK) == 0140000)

#define strtold strtod
void sync(void);
#define llabs labs
#define utimes utime
#define SIGURG /*nothing*/

int utimensat(int, const char *, const struct timespec *, int);

/* struct stat does not have atim and mtim */
#include <time.h>
#include <sys/stat.h>
struct timespec _atim;
struct timespec _mtim;
#define st.st_atim _atim
#define st.st_mtim _mtim
