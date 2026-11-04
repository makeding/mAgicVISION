#include "mb86H/loader.h"
#include "mb86H/resident_image.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv)
{
    if(argc!=2) return 2;
    FILE *f=fopen(argv[1],"rb");
    if(!f) return 2;
    if(fseek(f,0,SEEK_END)) { fclose(f); return 2; }
    long length=ftell(f); rewind(f);
    if(length<64 || length>1024*1024) { fclose(f); return 2; }
    unsigned char *bundle=malloc((size_t)length);
    if(!bundle) { fclose(f); return 2; }
    if(fread(bundle,1,(size_t)length,f)!=(size_t)length) { free(bundle); fclose(f); return 2; }
    fclose(f);
    struct mb86H_loader loader={0};
    int rc=mb86H_loader_parse(&loader,bundle,(size_t)length);
    if(!rc) rc=mb86H_resident_image_validate(loader.resident);
    printf("firmware bundle=%s bytes=%ld shared parse/resident checksum rc=%d\n",argv[1],length,rc);
    free(bundle); return rc ? 1 : 0;
}
