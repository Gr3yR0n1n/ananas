#ifndef SYS_CDEFS_H
#define SYS_CDEFS_H

/* Used in header files to play nice with C++ applications */
#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif


#endif /* SYS_CDEFS_H */
