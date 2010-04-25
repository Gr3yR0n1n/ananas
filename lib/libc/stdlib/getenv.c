/* $Id: getenv.c 264 2007-09-14 03:48:22Z solar $ */

/* getenv( const char * )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

/* This is an example implementation of getenv() fit for use with POSIX kernels.
*/

#include <string.h>
#include <stdlib.h>

#ifndef REGTEST

extern char * * environ;

char * getenv( const char * name )
{
#if 0
    size_t len = strlen( name );
    size_t index = 0;
    while ( environ[ index ] != NULL )
    {
        if ( strncmp( environ[ index ], name, len ) == 0 )
        {
            return environ[ index ] + len + 1;
        }
        index++;
    }
#endif
    return NULL;
}

#endif

#ifdef TEST
#include <_PDCLIB_test.h>

int main( void )
{
    TESTCASE( strcmp( getenv( "SHELL" ), "/bin/bash" ) == 0 );
    /* TESTCASE( strcmp( getenv( "SHELL" ), "/bin/sh" ) == 0 ); */
    return TEST_RESULTS;
}

#endif
