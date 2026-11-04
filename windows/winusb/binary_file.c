#include "binary_file.h"
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

FILE *xz_create_binary_file(const wchar_t *path)
{
    /* msvcrt.dll does not support the C11 fopen "x" mode. */
    int fd=_wopen(path,_O_WRONLY|_O_CREAT|_O_EXCL|_O_BINARY|_O_NOINHERIT,
                  _S_IREAD|_S_IWRITE);
    if(fd<0) return NULL;
    FILE *file=_wfdopen(fd,L"wb");
    if(!file) {
        int error=errno;
        _close(fd);
        errno=error;
    }
    return file;
}
