#ifndef MB86H_WINDOWS_BINARY_FILE_H
#define MB86H_WINDOWS_BINARY_FILE_H
#include <stdio.h>
#include <wchar.h>
/* Atomically create a binary file; never truncate an existing path. */
FILE *xz_create_binary_file(const wchar_t *path);
#endif
