#include <ananas/types.h>
#include <ananas/error.h>
#include <_posix/error.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

int access(const char* path, int amode)
{
	struct stat sb;
	if (stat(path, &sb) < 0)
		return -1; /* already sets errno */

	/* XXX This is wrong - we should let the kernel do it */
	if ((amode & X_OK) && (sb.st_mode & 0111) == 0)
		goto no_access;
	if ((amode & R_OK) && (sb.st_mode & 0222) == 0)
		goto no_access;
	if ((amode & W_OK) && (sb.st_mode & 0444) == 0)
		goto no_access;

	return 0;

no_access:
	errno = EACCES;
	return -1;
}
