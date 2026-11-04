#include "bon_abi.h"
#include <stddef.h>
#include <string.h>
/* x64 image-relative MSVC RTTI, including the IBonDriver base hierarchy.
 * pSelf makes the enclosing block the relative base; no absolute pointers
 * are truncated into RVAs. Type names match the public C++ declarations. */
struct col { uint32_t signature,offset,cd_offset,type,hierarchy,self; };
struct hierarchy { uint32_t signature,attributes,count,bases; };
struct base { uint32_t type,count; int32_t member,parent,virtual_offset;
              uint32_t attributes,hierarchy; };
struct type { uintptr_t vtable,spare; char name[24]; };
static struct {
    const struct col *locator;
    struct bon_vtable table;
    struct col col;
    struct hierarchy derived,base_hierarchy;
    uint32_t bases[2],base_bases[1];
    struct base derived_base,base;
    struct type derived_type,base_type;
} rtti;
#define RVA(field) ((uint32_t)offsetof(__typeof__(rtti),field))
static INIT_ONCE once=INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK initialize(PINIT_ONCE init,void *parameter,void **context)
{
    (void)init; (void)context;
    rtti.table=*(const struct bon_vtable *)parameter;
    rtti.locator=&rtti.col;
    rtti.col=(struct col){1,0,0,RVA(derived_type),RVA(derived),RVA(col)};
    rtti.derived=(struct hierarchy){0,0,2,RVA(bases)};
    rtti.base_hierarchy=(struct hierarchy){0,0,1,RVA(base_bases)};
    rtti.bases[0]=RVA(derived_base); rtti.bases[1]=RVA(base); rtti.base_bases[0]=RVA(base);
    rtti.derived_base=(struct base){RVA(derived_type),1,0,-1,0,0x40,RVA(derived)};
    rtti.base=(struct base){RVA(base_type),0,0,-1,0,0x40,RVA(base_hierarchy)};
    strcpy(rtti.derived_type.name,".?AVIBonDriver2@@");
    strcpy(rtti.base_type.name,".?AVIBonDriver@@");
    return TRUE;
}
const struct bon_vtable *bon_msvc_table(const struct bon_vtable *table)
{
    _Static_assert(sizeof(struct bon_vtable)==17*sizeof(void *),"17 BonDriver slots");
    _Static_assert(offsetof(__typeof__(rtti),table)==sizeof(void *),"RTTI at vtable[-1]");
    if(!InitOnceExecuteOnce(&once,initialize,(void *)table,NULL)) return NULL;
    return &rtti.table;
}
