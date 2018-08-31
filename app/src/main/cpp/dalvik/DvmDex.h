//
// Created by liu meng on 2018/8/31.
//

#ifndef CUSTOMAPPVMP_DVMDEX_H
#define CUSTOMAPPVMP_DVMDEX_H

#include "stdafx.h"
#include "DexFile.h"
#include "SysUtil.h"
struct DvmDex {
    /* pointer to the DexFile we're associated with */
    DexFile*            pDexFile;

    /* clone of pDexFile->pHeader (it's used frequently enough) */
    const DexHeader*    pHeader;

    /* interned strings; parallel to "stringIds" */
    struct StringObject** pResStrings;

    /* resolved classes; parallel to "typeIds" */
    struct ClassObject** pResClasses;

    /* resolved methods; parallel to "methodIds" */
    struct Method**     pResMethods;

    /* resolved instance fields; parallel to "fieldIds" */
    /* (this holds both InstField and StaticField) */
    struct Field**      pResFields;

    /* interface method lookup cache */
    struct AtomicCache* pInterfaceCache;

    /* shared memory region with file contents */
    bool                isMappedReadOnly;
    MemMapping          memMap;

    jobject dex_object;

    /* lock ensuring mutual exclusion during updates */
    pthread_mutex_t     modLock;
};
 struct StringObject* dvmDexGetResolvedString(const DvmDex* pDvmDex,
                                                    u4 stringIdx)
{
    return pDvmDex->pResStrings[stringIdx];
}
#endif //CUSTOMAPPVMP_DVMDEX_H