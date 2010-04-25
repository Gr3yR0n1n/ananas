/* $Id: clearerr.c 366 2009-09-13 15:14:02Z solar $ */

/* clearerr( FILE * )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

#include <stdio.h>

#ifndef REGTEST

void clearerr( struct _PDCLIB_file_t * stream )
{
    stream->status &= ~( _PDCLIB_ERRORFLAG | _PDCLIB_EOFFLAG );
}

#endif

#ifdef TEST
#include <_PDCLIB_test.h>

int main( void )
{
    FILE * fh;
    remove( "testfile" );
    TESTCASE( ( fh = fopen( "testfile", "w+" ) ) != NULL );
    /* Flags should be clear */
    TESTCASE( ! ferror( fh ) );
    TESTCASE( ! feof( fh ) );
    /* Reading from input stream - should provoke error */
    TESTCASE( fgetc( fh ) == EOF );
    TESTCASE( ferror( fh ) );
    TESTCASE( ! feof( fh ) );
    /* clearerr() should clear flags */
    clearerr( fh );
    TESTCASE( ! ferror( fh ) );
    TESTCASE( ! feof( fh ) );
    /* Reading from empty stream - should provoke EOF */
    rewind( fh );
    TESTCASE( fgetc( fh ) == EOF );
    TESTCASE( ! ferror( fh ) );
    TESTCASE( feof( fh ) );
    /* clearerr() should clear flags */
    clearerr( fh );
    TESTCASE( ! ferror( fh ) );
    TESTCASE( ! feof( fh ) );
    TESTCASE( fclose( fh ) == 0 );
    remove( "testfile" );
    return TEST_RESULTS;
}

#endif

