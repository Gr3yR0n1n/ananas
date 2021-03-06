#include "lib.h"
#include <ananas/error.h>
#include <ananas/procinfo.h>
#include <ananas/syscall-vmops.h>
#include <sys/mman.h> // for PROT_...

namespace {
struct PROCINFO* s_ProcInfo;
}

extern "C" {
long sys_open(const char*, int, int, handleindex_t*);
long sys_close(handleindex_t);
long sys_read(int, void*, size_t*);
long sys_write(int, const void*, size_t*);
long sys_seek(int, off_t*, int);
void sys_exit(int);
}

extern "C" int
open(const char* path, int flags, ...)
{
	handleindex_t index;
	return sys_open(path, flags, 0, &index) == ANANAS_ERROR_NONE ? index : -1;
}

extern "C" int
close(int fd)
{
	return sys_close(fd) == ANANAS_ERROR_NONE ? 0 : -1;
}

ssize_t
read(int fd, void* buf, size_t count)
{
	size_t len = count;
	if (sys_read(fd, buf, &len) != ANANAS_ERROR_NONE)
		return -1;
	return len;
}

ssize_t
write(int fd, const void* buf, size_t count)
{
	size_t len = count;
	if (sys_write(fd, buf, &len) != ANANAS_ERROR_NONE)
		return -1;
	return len;
}

off_t
lseek(int fd, off_t offset, int whence)
{
	off_t new_offset = offset;
	return sys_seek(fd, &new_offset, whence) == ANANAS_ERROR_NONE ? new_offset : -1;
}

void
exit(int status)
{
	sys_exit(status);
}

void*
mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	struct VMOP_OPTIONS vo;

	memset(&vo, 0, sizeof(vo));
	vo.vo_size = sizeof(vo);
	vo.vo_op = OP_MAP;
	vo.vo_addr = addr;
	vo.vo_len = length;
	vo.vo_flags = 0;
	if (prot & PROT_READ)
		vo.vo_flags |= VMOP_FLAG_READ;
	if (prot & PROT_WRITE)
		vo.vo_flags |= VMOP_FLAG_WRITE;
	if (prot & PROT_EXEC)
		vo.vo_flags |= VMOP_FLAG_EXECUTE;

	if (flags & MAP_PRIVATE)
		vo.vo_flags |= VMOP_FLAG_PRIVATE;
	else
		vo.vo_flags |= VMOP_FLAG_SHARED;
	if (flags & MAP_FIXED)
		vo.vo_flags |= VMOP_FLAG_FIXED;

	if (fd != -1) {
		vo.vo_flags |= VMOP_FLAG_HANDLE;
		vo.vo_handle = fd;
	}
	vo.vo_offset = offset;

	return sys_vmop(&vo) == ANANAS_ERROR_NONE ? static_cast<void*>(vo.vo_addr) : reinterpret_cast<void*>(-1);
}

int
munmap(void* addr, size_t length)
{
	struct VMOP_OPTIONS vo;

	memset(&vo, 0, sizeof(vo));
	vo.vo_size = sizeof(vo);
	vo.vo_op = OP_UNMAP;
	vo.vo_addr = addr;
	vo.vo_len = length;

	return sys_vmop(&vo) == ANANAS_ERROR_NONE ? 0 : -1;
}

size_t
strlen(const char* s)
{
	size_t n = 0;
	while(*s++ != '\0')
		n++;
	return n;
}

char*
strcpy(char* d, const char* s)
{
	memcpy(d, s, strlen(s) + 1 /* terminating \0 */);
	return d;
}

char*
strncpy(char* d, const char* s, size_t n)
{
	size_t i = 0;
	for(/* nothing */; s[i] != '\0' && i < n; i++)
		d[i] = s[i];

	for(/* nothing */; i < n; i++)
		d[i] = '\0';
	return d;
}

char*
strchr(const char* s, int c)
{
	for(/* nothing */; *s != '\0'; s++)
		if (*s == c)
			return const_cast<char*>(s);
	return nullptr;
}

char*
strcat(char* dest, const char* src)
{
	char* d = dest;
	d += strlen(d);
	while ((*d++ = *src++) != '\0')
		/* nothing */;
	return dest;
}

int
strcmp(const char* a, const char* b)
{
	while(*a != '\0' && *a == *b)
		a++, b++;
	return *a - *b;
}


int
strncmp(const char* s1, const char* s2, size_t n)
{
	while (n > 0 && *s1 != '\0' && *s2 != '\0' && *s1 == *s2) {
		n--; s1++; s2++;
	}
	return (n == 0) ? n : *s1 - *s2;
}


char*
strdup(const char* s)
{
	size_t len = strlen(s) + 1;
	auto p = static_cast<char*>(malloc(len));
	if (p != nullptr)
		memcpy(p, s, len);
	return p;
}

void*
memset(void* s, int c, size_t n)
{
	char* p = static_cast<char*>(s);
	while(n-- > 0)
		*p++ = c;
	return s;
}

void
memcpy(void* dst, const void* src, size_t len)
{
	/* Copies sz bytes of variable type T-sized data */
#define DO_COPY(T, sz) \
		do { \
			size_t x = (size_t)sz; \
			while (x >= sizeof(T)) { \
				*(T*)d = *(T*)s; \
				x -= sizeof(T); \
				s += sizeof(T); \
				d += sizeof(T); \
				len -= sizeof(T);  \
			} \
		} while(0)

	auto d = static_cast<char*>(dst);
	auto s = static_cast<const char*>(src);

	/* First of all, attempt to align to 32-bit boundary */
	if (len >= 4 && ((addr_t)dst & 3))
		DO_COPY(uint8_t, len & 3);

	/* Attempt to copy everything using 4 bytes at a time */
	DO_COPY(uint32_t, len);

	/* Cover the leftovers */
	DO_COPY(uint8_t, len);

#undef DO_COPY
}

void panic(const char* msg)
{
	write(STDERR_FILENO, msg, strlen(msg));
	exit(-1);
}

void*
operator new(size_t len) throw()
{
	return malloc(len);
}

void
operator delete(void* p) throw()
{
	free(p);
}

void
InitializeProcessInfo(void* procinfo)
{
	s_ProcInfo = static_cast<struct PROCINFO*>(procinfo);
	if (s_ProcInfo->pi_size < sizeof(struct PROCINFO))
		die("invalid procinfo length");
}

const char*
GetProgName()
{
	return s_ProcInfo->pi_args;
}

char*
getenv(const char* name)
{
	char* ptr = s_ProcInfo->pi_env;
	while (*ptr != '\0') {
		char* p = strchr(ptr, '=');
		if (p == nullptr)
			return nullptr; // corrupt environment
		if (strncmp(ptr, name, p - ptr) == 0)
			return p + 1;

		ptr += strlen(ptr) + 1;
	}

	return nullptr;
}
