/* $Id: ferror.c 366 2009-09-13 15:14:02Z solar $ */

/* ferror( FILE * )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

#include <stdio.h>

#ifndef REGTEST

int ferror( struct _PDCLIB_file_t * stream )
{
    return stream->status & _PDCLIB_ERRORFLAG;
}

#endif

#ifdef TEST
#include <_PDCLIB_test.h>

int main( void )
{
    /* Testing covered by clearerr(). */
    return TEST_RESULTS;
}

#endif

