#include <errno.h>
#include <wchar.h>

size_t
mbsnrtowcs(wchar_t* dst, const char** src, size_t nmc, size_t len, mbstate_t* ps)
{
	errno = EILSEQ;
	return (size_t)-1;
}
