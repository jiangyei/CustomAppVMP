#include "stdafx.h"
#include "DexOpcodes.h"
#include "Exception.h"
#include "InterpC.h"
#include "DvmDex.h"
#include "Resolve.h"
#include "Thread.h"
#include "Sync.h"
#include "TypeCheck.h"
#include "Alloc.h"
#include "Class.h"
#include "Array.h"
#include "Common.h"
#include "WriteBarrier.h"
#include "Object.h"
#include "ObjectInlines.h"
#include <math.h>
#include "Stack.h"
//////////////////////////////////////////////////////////////////////////
#define GOTO_TARGET_DECL(_target, ...)
inline void dvmAbort(void) {
    exit(1);
}

static void copySwappedArrayData(void* dest, const u2* src, u4 size, u2 width)
{
#if __BYTE_ORDER == __LITTLE_ENDIAN
    memcpy(dest, src, size*width);
#else
    int i;

    switch (width) {
    case 1:
        /* un-swap pairs of bytes as we go */
        for (i = (size-1) & ~1; i >= 0; i -= 2) {
            ((u1*)dest)[i] = ((u1*)src)[i+1];
            ((u1*)dest)[i+1] = ((u1*)src)[i];
        }
        /*
         * "src" is padded to end on a two-byte boundary, but we don't want to
         * assume "dest" is, so we handle odd length specially.
         */
        if ((size & 1) != 0) {
            ((u1*)dest)[size-1] = ((u1*)src)[size];
        }
        break;
    case 2:
        /* already swapped correctly */
        memcpy(dest, src, size*width);
        break;
    case 4:
        /* swap word halves */
        for (i = 0; i < (int) size; i++) {
            ((u4*)dest)[i] = (src[(i << 1) + 1] << 16) | src[i << 1];
        }
        break;
    case 8:
        /* swap word halves and words */
        for (i = 0; i < (int) (size << 1); i += 2) {
            ((int*)dest)[i] = (src[(i << 1) + 3] << 16) | src[(i << 1) + 2];
            ((int*)dest)[i+1] = (src[(i << 1) + 1] << 16) | src[i << 1];
        }
        break;
    default:
        ALOGE("Unexpected width %d in copySwappedArrayData", width);
        dvmAbort();
        break;
    }
#endif
}
bool dvmInterpHandleFillArrayData(JNIEnv *env,ArrayObject* arrayObj, const u2* arrayData)
{
    u2 width;
    u4 size;

    if (arrayObj == NULL) {
        dvmThrowNullPointerException(env,NULL);
        return false;
    }
    assert (!IS_CLASS_FLAG_SET(((Object *)arrayObj)->clazz,
                               CLASS_ISOBJECTARRAY));

    /*
     * Array data table format:
     *  ushort ident = 0x0300   magic value
     *  ushort width            width of each element in the table
     *  uint   size             number of elements in the table
     *  ubyte  data[size*width] table of data values (may contain a single-byte
     *                          padding at the end)
     *
     * Total size is 4+(width * size + 1)/2 16-bit code units.
     */
    if (arrayData[0] != kArrayDataSignature) {
        dvmThrowInternalError("bad array data magic");
        return false;
    }

    width = arrayData[1];
    size = arrayData[2] | (((u4)arrayData[3]) << 16);

    if (size > arrayObj->length) {
        dvmThrowArrayIndexOutOfBoundsException(env,arrayObj->length, size);
        return false;
    }
    copySwappedArrayData(arrayObj->contents, &arrayData[4], size, width);
    return true;
}
#if __BYTE_ORDER == __LITTLE_ENDIAN
static inline s4 s4FromSwitchData(const void* switchData) {
    return *(s4*) switchData;
}
#else
static inline s4 s4FromSwitchData(const void* switchData) {
    u2* data = switchData;
    return data[0] | (((s4) data[1]) << 16);
}
#endif
s4 dvmInterpHandlePackedSwitch(const u2* switchData, s4 testVal)
{
    const int kInstrLen = 3;

    /*
     * Packed switch data format:
     *  ushort ident = 0x0100   magic value
     *  ushort size             number of entries in the table
     *  int first_key           first (and lowest) switch case value
     *  int targets[size]       branch targets, relative to switch opcode
     *
     * Total size is (4+size*2) 16-bit code units.
     */
    if (*switchData++ != kPackedSwitchSignature) {
        /* should have been caught by verifier */
        dvmThrowInternalError("bad packed switch magic");
        return kInstrLen;
    }

    u2 size = *switchData++;
    assert(size > 0);

    s4 firstKey = *switchData++;
    firstKey |= (*switchData++) << 16;

    int index = testVal - firstKey;
    if (index < 0 || index >= size) {
        MY_LOG_INFO("Value %d not found in switch (%d-%d)",
              testVal, firstKey, firstKey+size-1);
        return kInstrLen;
    }

    /* The entries are guaranteed to be aligned on a 32-bit boundary;
     * we can treat them as a native int array.
     */
    const s4* entries = (const s4*) switchData;
    assert(((u4)entries & 0x3) == 0);

    assert(index >= 0 && index < size);
    MY_LOG_INFO("Value %d found in slot %d (goto 0x%02x)",
          testVal, index,
          s4FromSwitchData(&entries[index]));
    return s4FromSwitchData(&entries[index]);
}
s4 dvmInterpHandleSparseSwitch(const u2* switchData, s4 testVal)
{
    const int kInstrLen = 3;
    u2 size;
    const s4* keys;
    const s4* entries;

    /*
     * Sparse switch data format:
     *  ushort ident = 0x0200   magic value
     *  ushort size             number of entries in the table; > 0
     *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
     *  int targets[size]       branch targets, relative to switch opcode
     *
     * Total size is (2+size*4) 16-bit code units.
     */

    if (*switchData++ != kSparseSwitchSignature) {
        /* should have been caught by verifier */
        dvmThrowInternalError("bad sparse switch magic");
        return kInstrLen;
    }

    size = *switchData++;
    assert(size > 0);

    /* The keys are guaranteed to be aligned on a 32-bit boundary;
     * we can treat them as a native int array.
     */
    keys = (const s4*) switchData;
    assert(((u4)keys & 0x3) == 0);

    /* The entries are guaranteed to be aligned on a 32-bit boundary;
     * we can treat them as a native int array.
     */
    entries = keys + size;
    assert(((u4)entries & 0x3) == 0);

    /*
     * Binary-search through the array of keys, which are guaranteed to
     * be sorted low-to-high.
     */
    int lo = 0;
    int hi = size - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;

        s4 foundVal = s4FromSwitchData(&keys[mid]);
        if (testVal < foundVal) {
            hi = mid - 1;
        } else if (testVal > foundVal) {
            lo = mid + 1;
        } else {
            MY_LOG_INFO("Value %d found in entry %d (goto 0x%02x)",
                  testVal, mid, s4FromSwitchData(&entries[mid]));
            return s4FromSwitchData(&entries[mid]);
        }
    }

    MY_LOG_INFO("Value %d not found in switch", testVal);
    return kInstrLen;
}

//////////////////////////////////////////////////////////////////////////

static const char kSpacing[] = "            ";

//////////////////////////////////////////////////////////////////////////

/**
 * 获得参数寄存器个数。
 * @param[in] separatorData Separator数据。
 * @return 返回参数寄存器个数。
 */
static size_t getParamRegCount(const SeparatorData* separatorData) {
    int count = 0;

    for (int i = 0; i < separatorData->paramShortDesc.size; i++) {
        switch (separatorData->paramShortDesc.str[i]) {
        case 'Z':
        case 'B':
        case 'S':
        case 'C':
        case 'I':
        case 'F':
        case 'L':
        case '[':
            count++;
            break;
        case 'J':
        case 'D':
            count += 2;
            break;
        default:
            MY_LOG_ERROR("无效的短类型！");
            break;
        }
    }
    return count;
}

/**
 * 是否是静态方法。
 * @param[in] separatorData Separator数据。
 * @return true：是静态方法。false：不是静态方法。
 */
static inline bool isStaticMethod(const SeparatorData* separatorData) {
    return separatorData->accessFlag & ACC_STATIC == 0 ? false : true;
}

/**
 * 解析可变参数，获得参数数组。
 * @param[in] 
 * @param[in] 
 * @return 返回参数数组。这个数组使用完后需要释放内存。
 */
static jvalue* getParams(const SeparatorData* separatorData, va_list args) {
    jvalue* params = new jvalue[separatorData->paramSize];
    for (int i = 0; i < separatorData->paramSize; i++) {
        switch (separatorData->paramShortDesc.str[i]) {
        case 'Z':
            params[i].z = va_arg(args, jboolean);
            break;

        case 'B':
            params[i].b = va_arg(args, jbyte);
            break;

        case 'S':
            params[i].s = va_arg(args, jshort);
            break;

        case 'C':
            params[i].c = va_arg(args, jchar);
            break;

        case 'I':
            params[i].i = va_arg(args, jint);
            break;

        case 'J':
            params[i].j = va_arg(args, jlong);
            break;

        case 'F':
            params[i].f = va_arg(args, jfloat);
            break;

        case 'D':
            params[i].d = va_arg(args, jdouble);
            break;

        case 'L':
            params[i].l = va_arg(args, jobject);
            break;

        case '[':
            params[i].l = va_arg(args, jarray);
            break;
        default:
            MY_LOG_WARNING("无效的短类型。");
            break;
        }
    }
    return params;
}

//////////////////////////////////////////////////////////////////////////

/* get a long from an array of u4 */
static inline s8 getLongFromArray(const u4* ptr, int idx)
{
#if defined(NO_UNALIGN_64__UNION)
    union { s8 ll; u4 parts[2]; } conv;

    ptr += idx;
    conv.parts[0] = ptr[0];
    conv.parts[1] = ptr[1];
    return conv.ll;
#else
    s8 val;
    memcpy(&val, &ptr[idx], 8);
    return val;
#endif
}

/* store a long into an array of u4 */
static inline void putLongToArray(u4* ptr, int idx, s8 val)
{
#if defined(NO_UNALIGN_64__UNION)
    union { s8 ll; u4 parts[2]; } conv;

    ptr += idx;
    conv.ll = val;
    ptr[0] = conv.parts[0];
    ptr[1] = conv.parts[1];
#else
    memcpy(&ptr[idx], &val, 8);
#endif
}

/* get a double from an array of u4 */
static inline double getDoubleFromArray(const u4* ptr, int idx)
{
#if defined(NO_UNALIGN_64__UNION)
    union { double d; u4 parts[2]; } conv;

    ptr += idx;
    conv.parts[0] = ptr[0];
    conv.parts[1] = ptr[1];
    return conv.d;
#else
    double dval;
    memcpy(&dval, &ptr[idx], 8);
    return dval;
#endif
}

/* store a double into an array of u4 */
static inline void putDoubleToArray(u4* ptr, int idx, double dval)
{
#if defined(NO_UNALIGN_64__UNION)
    union { double d; u4 parts[2]; } conv;

    ptr += idx;
    conv.d = dval;
    ptr[0] = conv.parts[0];
    ptr[1] = conv.parts[1];
#else
    memcpy(&ptr[idx], &dval, 8);
#endif
}

//////////////////////////////////////////////////////////////////////////

//#define LOG_INSTR                   /* verbose debugging */
/* set and adjust ANDROID_LOG_TAGS='*:i jdwp:i dalvikvm:i dalvikvmi:i' */

/*
 * Export another copy of the PC on every instruction; this is largely
 * redundant with EXPORT_PC and the debugger code.  This value can be
 * compared against what we have stored on the stack with EXPORT_PC to
 * help ensure that we aren't missing any export calls.
 */
#if WITH_EXTRA_GC_CHECKS > 1
# define EXPORT_EXTRA_PC() (self->currentPc2 = pc)
#else
# define EXPORT_EXTRA_PC()
#endif

/*
 * Adjust the program counter.  "_offset" is a signed int, in 16-bit units.
 *
 * Assumes the existence of "const u2* pc" and "const u2* curMethod->insns".
 *
 * We don't advance the program counter until we finish an instruction or
 * branch, because we do want to have to unroll the PC if there's an
 * exception.
 */
#ifdef CHECK_BRANCH_OFFSETS
# define ADJUST_PC(_offset) do {                                            \
        int myoff = _offset;        /* deref only once */                   \
        if (pc + myoff < curMethod->insns ||                                \
            pc + myoff >= curMethod->insns + dvmGetMethodInsnsSize(curMethod)) \
        {                                                                   \
            char* desc;                                                     \
            desc = dexProtoCopyMethodDescriptor(&curMethod->prototype);     \
            MY_LOG_ERROR("Invalid branch %d at 0x%04x in %s.%s %s",                 \
                myoff, (int) (pc - curMethod->insns),                       \
                curMethod->clazz->descriptor, curMethod->name, desc);       \
            free(desc);                                                     \
            dvmAbort();                                                     \
        }                                                                   \
        pc += myoff;                                                        \
        EXPORT_EXTRA_PC();                                                  \
    } while (false)
#else
# define ADJUST_PC(_offset) do {                                            \
        pc += _offset;                                                      \
        EXPORT_EXTRA_PC();                                                  \
    } while (false)
#endif

/*
 * If enabled, validate the register number on every access.  Otherwise,
 * just do an array access.
 *
 * Assumes the existence of "u4* fp".
 *
 * "_idx" may be referenced more than once.
 */
#ifdef CHECK_REGISTER_INDICES
# define GET_REGISTER(_idx) \
    ( (_idx) < curMethod->registersSize ? \
        (fp[(_idx)]) : (assert(!"bad reg"),1969) )
# define SET_REGISTER(_idx, _val) \
    ( (_idx) < curMethod->registersSize ? \
        (fp[(_idx)] = (u4)(_val)) : (assert(!"bad reg"),1969) )
# define GET_REGISTER_AS_OBJECT(_idx)       ((Object *)GET_REGISTER(_idx))
# define SET_REGISTER_AS_OBJECT(_idx, _val) SET_REGISTER(_idx, (s4)_val)
# define GET_REGISTER_INT(_idx) ((s4) GET_REGISTER(_idx))
# define SET_REGISTER_INT(_idx, _val) SET_REGISTER(_idx, (s4)_val)
# define GET_REGISTER_WIDE(_idx) \
    ( (_idx) < curMethod->registersSize-1 ? \
        getLongFromArray(fp, (_idx)) : (assert(!"bad reg"),1969) )
# define SET_REGISTER_WIDE(_idx, _val) \
    ( (_idx) < curMethod->registersSize-1 ? \
        (void)putLongToArray(fp, (_idx), (_val)) : assert(!"bad reg") )
# define GET_REGISTER_FLOAT(_idx) \
    ( (_idx) < curMethod->registersSize ? \
        (*((float*) &fp[(_idx)])) : (assert(!"bad reg"),1969.0f) )
# define SET_REGISTER_FLOAT(_idx, _val) \
    ( (_idx) < curMethod->registersSize ? \
        (*((float*) &fp[(_idx)]) = (_val)) : (assert(!"bad reg"),1969.0f) )
# define GET_REGISTER_DOUBLE(_idx) \
    ( (_idx) < curMethod->registersSize-1 ? \
        getDoubleFromArray(fp, (_idx)) : (assert(!"bad reg"),1969.0) )
# define SET_REGISTER_DOUBLE(_idx, _val) \
    ( (_idx) < curMethod->registersSize-1 ? \
        (void)putDoubleToArray(fp, (_idx), (_val)) : assert(!"bad reg") )
#else
# define GET_REGISTER(_idx)                 (fp[(_idx)])
# define SET_REGISTER(_idx, _val)           (fp[(_idx)] = (_val))
# define GET_REGISTER_AS_OBJECT(_idx)       ((Object*) fp[(_idx)])
# define SET_REGISTER_AS_OBJECT(_idx, _val) (fp[(_idx)] = (u4)(_val))
# define GET_REGISTER_INT(_idx)             ((s4)GET_REGISTER(_idx))
# define SET_REGISTER_INT(_idx, _val)       SET_REGISTER(_idx, (s4)_val)
# define GET_REGISTER_WIDE(_idx)            getLongFromArray(fp, (_idx))
# define SET_REGISTER_WIDE(_idx, _val)      putLongToArray(fp, (_idx), (_val))
# define GET_REGISTER_FLOAT(_idx)           (*((float*) &fp[(_idx)]))
# define SET_REGISTER_FLOAT(_idx, _val)     (*((float*) &fp[(_idx)]) = (_val))
# define GET_REGISTER_DOUBLE(_idx)          getDoubleFromArray(fp, (_idx))
# define SET_REGISTER_DOUBLE(_idx, _val)    putDoubleToArray(fp, (_idx), (_val))
#endif

/*
 * Get 16 bits from the specified offset of the program counter.  We always
 * want to load 16 bits at a time from the instruction stream -- it's more
 * efficient than 8 and won't have the alignment problems that 32 might.
 *
 * Assumes existence of "const u2* pc".
 */
#define FETCH(_offset)     (pc[(_offset)])

/*
 * Extract instruction byte from 16-bit fetch (_inst is a u2).
 */
#define INST_INST(_inst)    ((_inst) & 0xff)

/*
 * Replace the opcode (used when handling breakpoints).  _opcode is a u1.
 */
#define INST_REPLACE_OP(_inst, _opcode) (((_inst) & 0xff00) | _opcode)

/*
 * Extract the "vA, vB" 4-bit registers from the instruction word (_inst is u2).
 */
#define INST_A(_inst)       (((_inst) >> 8) & 0x0f)
#define INST_B(_inst)       ((_inst) >> 12)

/*
 * Get the 8-bit "vAA" 8-bit register index from the instruction word.
 * (_inst is u2)
 */
#define INST_AA(_inst)      ((_inst) >> 8)

/*
 * The current PC must be available to Throwable constructors, e.g.
 * those created by the various exception throw routines, so that the
 * exception stack trace can be generated correctly.  If we don't do this,
 * the offset within the current method won't be shown correctly.  See the
 * notes in Exception.c.
 *
 * This is also used to determine the address for precise GC.
 *
 * Assumes existence of "u4* fp" and "const u2* pc".
 */
// TODO 这里这里不支持。
#define EXPORT_PC()         /*(SAVEAREA_FROM_FP(fp)->xtra.currentPc = pc)*/

/*
 * Check to see if "obj" is NULL.  If so, throw an exception.  Assumes the
 * pc has already been exported to the stack.
 *
 * Perform additional checks on debug builds.
 *
 * Use this to check for NULL when the instruction handler calls into
 * something that could throw an exception (so we have already called
 * EXPORT_PC at the top).
 */
static inline bool checkForNull(JNIEnv* env, Object* obj)
{
    if (obj == NULL) {
        dvmThrowNullPointerException(env, NULL);
        return false;
    }
#ifdef WITH_EXTRA_OBJECT_VALIDATION
    if (!dvmIsHeapAddress(obj)) {
        MY_LOG_ERROR("Invalid object %p", obj);
        dvmAbort();
    }
#endif
#ifndef NDEBUG
    if (obj->clazz == NULL || ((u4) obj->clazz) <= 65536) {
        /* probable heap corruption */
        MY_LOG_ERROR("Invalid object class %p (in %p)", obj->clazz, obj);
        dvmAbort();
    }
#endif
    return true;
}

/*
 * Check to see if "obj" is NULL.  If so, export the PC into the stack
 * frame and throw an exception.
 *
 * Perform additional checks on debug builds.
 *
 * Use this to check for NULL when the instruction handler doesn't do
 * anything else that can throw an exception.
 */
static inline bool checkForNullExportPC(JNIEnv* env, Object* obj, u4* fp, const u2* pc)
{
    if (obj == NULL) {
        EXPORT_PC();
        dvmThrowNullPointerException(env, NULL);
        return false;
    }
#ifdef WITH_EXTRA_OBJECT_VALIDATION
    if (!dvmIsHeapAddress(obj)) {
        MY_LOG_ERROR("Invalid object %p", obj);
        dvmAbort();
    }
#endif
#ifndef NDEBUG
    if (obj->clazz == NULL || ((u4) obj->clazz) <= 65536) {
        /* probable heap corruption */
        MY_LOG_ERROR("Invalid object class %p (in %p)", obj->clazz, obj);
        dvmAbort();
    }
#endif
    return true;
}

/* File: portable/stubdefs.cpp */
/*
 * In the C mterp stubs, "goto" is a function call followed immediately
 * by a return.
 */

#define GOTO_TARGET_DECL(_target, ...)

#define GOTO_TARGET(_target, ...) _target:

#define GOTO_TARGET_END

/* ugh */
#define STUB_HACK(x)
#define JIT_STUB_HACK(x)

/*
 * InterpSave's pc and fp must be valid when breaking out to a
 * "Reportxxx" routine.  Because the portable interpreter uses local
 * variables for these, we must flush prior.  Stubs, however, use
 * the interpSave vars directly, so this is a nop for stubs.
 */
#define PC_FP_TO_SELF()                                                    \
    self->interpSave.pc = pc;                                              \
    self->interpSave.curFrame = fp;
#define PC_TO_SELF() self->interpSave.pc = pc;

/*
 * Instruction framing.  For a switch-oriented implementation this is
 * case/break, for a threaded implementation it's a goto label and an
 * instruction fetch/computed goto.
 *
 * Assumes the existence of "const u2* pc" and (for threaded operation)
 * "u2 inst".
 */
# define H(_op)             &&op_##_op
# define HANDLE_OPCODE(_op) op_##_op:
# define FINISH(_offset) {                                                  \
        ADJUST_PC(_offset);                                                 \
        inst = FETCH(0);                                                    \
        /*if (self->interpBreak.ctl.subMode) {*/                                \
            /*dvmCheckBefore(pc, fp, self);*/                                   \
        /*}*/                                                                   \
        goto *handlerTable[INST_INST(inst)];                                \
    }
# define FINISH_BKPT(_opcode) {                                             \
        goto *handlerTable[_opcode];                                        \
    }

#define OP_END

/*
 * The "goto" targets just turn into goto statements.  The "arguments" are
 * passed through local variables.
 */

#define GOTO_exceptionThrown() goto exceptionThrown;

//#define GOTO_returnFromMethod() goto returnFromMethod;

#define GOTO_invoke(_target, _methodCallRange)                              \
    do {                                                                    \
        methodCallRange = _methodCallRange;                                 \
        goto _target;                                                       \
    } while(false)

/* for this, the "args" are already in the locals */
#define GOTO_invokeMethod(_methodCallRange, _methodToCall, _vsrc1, _vdst) goto invokeMethod;
#define GOTO_bail() goto bail;

/*
 * Periodically check for thread suspension.
 *
 * While we're at it, see if a debugger has attached or the profiler has
 * started.  If so, switch to a different "goto" table.
 */
#define PERIODIC_CHECKS(_pcadj) {                              \
        if (dvmCheckSuspendQuick(dvmThreadSelfHook())) {                                   \
            EXPORT_PC();  /* need for precise GC */                         \
            dvmCheckSuspendPendingHook(dvmThreadSelfHook());                                   \
        }                                                                   \
    }

/* File: c/opcommon.cpp */
/* forward declarations of goto targets */
 GOTO_TARGET_DECL(filledNewArray, bool methodCallRange);
 GOTO_TARGET_DECL(invokeVirtual, bool methodCallRange);
 GOTO_TARGET_DECL(invokeSuper, bool methodCallRange);
 GOTO_TARGET_DECL(invokeInterface, bool methodCallRange);
 GOTO_TARGET_DECL(invokeDirect, bool methodCallRange);
 GOTO_TARGET_DECL(invokeStatic, bool methodCallRange);
 GOTO_TARGET_DECL(invokeVirtualQuick, bool methodCallRange);
 GOTO_TARGET_DECL(invokeSuperQuick, bool methodCallRange);
 GOTO_TARGET_DECL(invokeMethod, bool methodCallRange, const Method* methodToCall,
     u2 count, u2 regs);
 GOTO_TARGET_DECL(returnFromMethod);
 GOTO_TARGET_DECL(exceptionThrown);





#define HANDLE_NUMCONV(_opcode, _opname, _fromtype, _totype)                \
    HANDLE_OPCODE(_opcode /*vA, vB*/)                                       \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);                                               \
        MY_LOG_VERBOSE("|%s v%d,v%d", (_opname), vdst, vsrc1);                       \
        SET_REGISTER##_totype(vdst,                                         \
            GET_REGISTER##_fromtype(vsrc1));                                \
        FINISH(1);

#define HANDLE_FLOAT_TO_INT(_opcode, _opname, _fromvtype, _fromrtype,       \
        _tovtype, _tortype)                                                 \
    HANDLE_OPCODE(_opcode /*vA, vB*/)                                       \
    {                                                                       \
        /* spec defines specific handling for +/- inf and NaN values */     \
        _fromvtype val;                                                     \
        _tovtype intMin, intMax, result;                                    \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);                                               \
        MY_LOG_VERBOSE("|%s v%d,v%d", (_opname), vdst, vsrc1);                       \
        val = GET_REGISTER##_fromrtype(vsrc1);                              \
        intMin = (_tovtype) 1 << (sizeof(_tovtype) * 8 -1);                 \
        intMax = ~intMin;                                                   \
        result = (_tovtype) val;                                            \
        if (val >= intMax)          /* +inf */                              \
            result = intMax;                                                \
        else if (val <= intMin)     /* -inf */                              \
            result = intMin;                                                \
        else if (val != val)        /* NaN */                               \
            result = 0;                                                     \
        else                                                                \
            result = (_tovtype) val;                                        \
        SET_REGISTER##_tortype(vdst, result);                               \
    }                                                                       \
    FINISH(1);

#define HANDLE_INT_TO_SMALL(_opcode, _opname, _type)                        \
    HANDLE_OPCODE(_opcode /*vA, vB*/)                                       \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);                                               \
        MY_LOG_VERBOSE("|int-to-%s v%d,v%d", (_opname), vdst, vsrc1);                \
        SET_REGISTER(vdst, (_type) GET_REGISTER(vsrc1));                    \
        FINISH(1);

/* NOTE: the comparison result is always a signed 4-byte integer */
#define HANDLE_OP_CMPX(_opcode, _opname, _varType, _type, _nanVal)          \
    HANDLE_OPCODE(_opcode /*vAA, vBB, vCC*/)                                \
    {                                                                       \
        int result;                                                         \
        u2 regs;                                                            \
        _varType val1, val2;                                                \
        vdst = INST_AA(inst);                                               \
        regs = FETCH(1);                                                    \
        vsrc1 = regs & 0xff;                                                \
        vsrc2 = regs >> 8;                                                  \
        MY_LOG_VERBOSE("|cmp%s v%d,v%d,v%d", (_opname), vdst, vsrc1, vsrc2);         \
        val1 = GET_REGISTER##_type(vsrc1);                                  \
        val2 = GET_REGISTER##_type(vsrc2);                                  \
        if (val1 == val2)                                                   \
            result = 0;                                                     \
        else if (val1 < val2)                                               \
            result = -1;                                                    \
        else if (val1 > val2)                                               \
            result = 1;                                                     \
        else                                                                \
            result = (_nanVal);                                             \
        MY_LOG_VERBOSE("+ result=%d", result);                                       \
        SET_REGISTER(vdst, result);                                         \
    }                                                                       \
    FINISH(2);

#define HANDLE_OP_IF_XX(_opcode, _opname, _cmp)                             \
    HANDLE_OPCODE(_opcode /*vA, vB, +CCCC*/)                                \
        vsrc1 = INST_A(inst);                                               \
        vsrc2 = INST_B(inst);                                               \
        if ((s4) GET_REGISTER(vsrc1) _cmp (s4) GET_REGISTER(vsrc2)) {       \
            int branchOffset = (s2)FETCH(1);    /* sign-extended */         \
            MY_LOG_VERBOSE("|if-%s v%d,v%d,+0x%04x", (_opname), vsrc1, vsrc2,        \
                branchOffset);                                              \
            MY_LOG_VERBOSE("> branch taken");                                        \
            if (branchOffset < 0)                                           \
                PERIODIC_CHECKS(branchOffset);                              \
            FINISH(branchOffset);                                           \
        } else {                                                            \
            MY_LOG_VERBOSE("|if-%s v%d,v%d,-", (_opname), vsrc1, vsrc2);             \
            FINISH(2);                                                      \
        }

#define HANDLE_OP_IF_XXZ(_opcode, _opname, _cmp)                            \
    HANDLE_OPCODE(_opcode /*vAA, +BBBB*/)                                   \
        vsrc1 = INST_AA(inst);                                              \
        if ((s4) GET_REGISTER(vsrc1) _cmp 0) {                              \
            int branchOffset = (s2)FETCH(1);    /* sign-extended */         \
            MY_LOG_VERBOSE("|if-%s v%d,+0x%04x", (_opname), vsrc1, branchOffset);    \
            MY_LOG_VERBOSE("> branch taken");                                        \
            if (branchOffset < 0)                                           \
                PERIODIC_CHECKS(branchOffset);                              \
            FINISH(branchOffset);                                           \
        } else {                                                            \
            MY_LOG_VERBOSE("|if-%s v%d,-", (_opname), vsrc1);                        \
            FINISH(2);                                                      \
        }

#define HANDLE_UNOP(_opcode, _opname, _pfx, _sfx, _type)                    \
    HANDLE_OPCODE(_opcode /*vA, vB*/)                                       \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);                                               \
        MY_LOG_VERBOSE("|%s v%d,v%d", (_opname), vdst, vsrc1);                       \
        SET_REGISTER##_type(vdst, _pfx GET_REGISTER##_type(vsrc1) _sfx);    \
        FINISH(1);

#define HANDLE_OP_X_INT(_opcode, _opname, _op, _chkdiv)                     \
    HANDLE_OPCODE(_opcode /*vAA, vBB, vCC*/)                                \
    {                                                                       \
        u2 srcRegs;                                                         \
        vdst = INST_AA(inst);                                               \
        srcRegs = FETCH(1);                                                 \
        vsrc1 = srcRegs & 0xff;                                             \
        vsrc2 = srcRegs >> 8;                                               \
        MY_LOG_VERBOSE("|%s-int v%d,v%d", (_opname), vdst, vsrc1);                   \
        if (_chkdiv != 0) {                                                 \
            s4 firstVal, secondVal, result;                                 \
            firstVal = GET_REGISTER(vsrc1);                                 \
            secondVal = GET_REGISTER(vsrc2);                                \
            if (secondVal == 0) {                                           \
                EXPORT_PC();                                                \
                dvmThrowArithmeticException(env, "divide by zero");              \
                GOTO_exceptionThrown();                                     \
            }                                                               \
            if ((u4)firstVal == 0x80000000 && secondVal == -1) {            \
                if (_chkdiv == 1)                                           \
                    result = firstVal;  /* division */                      \
                else                                                        \
                    result = 0;         /* remainder */                     \
            } else {                                                        \
                result = firstVal _op secondVal;                            \
            }                                                               \
            SET_REGISTER(vdst, result);                                     \
        } else {                                                            \
            /* non-div/rem case */                                          \
            SET_REGISTER(vdst,                                              \
                (s4) GET_REGISTER(vsrc1) _op (s4) GET_REGISTER(vsrc2));     \
        }                                                                   \
    }                                                                       \
    FINISH(2);

#define HANDLE_OP_SHX_INT(_opcode, _opname, _cast, _op)                     \
    HANDLE_OPCODE(_opcode /*vAA, vBB, vCC*/)                                \
    {                                                                       \
        u2 srcRegs;                                                         \
        vdst = INST_AA(inst);                                               \
        srcRegs = FETCH(1);                                                 \
        vsrc1 = srcRegs & 0xff;                                             \
        vsrc2 = srcRegs >> 8;                                               \
        MY_LOG_VERBOSE("|%s-int v%d,v%d", (_opname), vdst, vsrc1);                   \
        SET_REGISTER(vdst,                                                  \
            _cast GET_REGISTER(vsrc1) _op (GET_REGISTER(vsrc2) & 0x1f));    \
    }                                                                       \
    FINISH(2);

#define HANDLE_OP_X_INT_LIT16(_opcode, _opname, _op, _chkdiv)               \
    HANDLE_OPCODE(_opcode /*vA, vB, #+CCCC*/)                               \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);                                               \
        vsrc2 = FETCH(1);                                                   \
        MY_LOG_VERBOSE("|%s-int/lit16 v%d,v%d,#+0x%04x",                             \
            (_opname), vdst, vsrc1, vsrc2);                                 \
        if (_chkdiv != 0) {                                                 \
            s4 firstVal, result;                                            \
            firstVal = GET_REGISTER(vsrc1);                                 \
            if ((s2) vsrc2 == 0) {                                          \
                EXPORT_PC();                                                \
                dvmThrowArithmeticException(env, "divide by zero");              \
                GOTO_exceptionThrown();                                     \
            }                                                               \
            if ((u4)firstVal == 0x80000000 && ((s2) vsrc2) == -1) {         \
                /* won't generate /lit16 instr for this; check anyway */    \
                if (_chkdiv == 1)                                           \
                    result = firstVal;  /* division */                      \
                else                                                        \
                    result = 0;         /* remainder */                     \
            } else {                                                        \
                result = firstVal _op (s2) vsrc2;                           \
            }                                                               \
            SET_REGISTER(vdst, result);                                     \
        } else {                                                            \
            /* non-div/rem case */                                          \
            SET_REGISTER(vdst, GET_REGISTER(vsrc1) _op (s2) vsrc2);         \
        }                                                                   \
        FINISH(2);

#define HANDLE_OP_X_INT_LIT8(_opcode, _opname, _op, _chkdiv)                \
    HANDLE_OPCODE(_opcode /*vAA, vBB, #+CC*/)                               \
    {                                                                       \
        u2 litInfo;                                                         \
        vdst = INST_AA(inst);                                               \
        litInfo = FETCH(1);                                                 \
        vsrc1 = litInfo & 0xff;                                             \
        vsrc2 = litInfo >> 8;       /* constant */                          \
        MY_LOG_VERBOSE("|%s-int/lit8 v%d,v%d,#+0x%02x",                              \
            (_opname), vdst, vsrc1, vsrc2);                                 \
        if (_chkdiv != 0) {                                                 \
            s4 firstVal, result;                                            \
            firstVal = GET_REGISTER(vsrc1);                                 \
            if ((s1) vsrc2 == 0) {                                          \
                EXPORT_PC();                                                \
                dvmThrowArithmeticException(env, "divide by zero");              \
                GOTO_exceptionThrown();                                     \
            }                                                               \
            if ((u4)firstVal == 0x80000000 && ((s1) vsrc2) == -1) {         \
                if (_chkdiv == 1)                                           \
                    result = firstVal;  /* division */                      \
                else                                                        \
                    result = 0;         /* remainder */                     \
            } else {                                                        \
                result = firstVal _op ((s1) vsrc2);                         \
            }                                                               \
            SET_REGISTER(vdst, result);                                     \
        } else {                                                            \
            SET_REGISTER(vdst,                                              \
                (s4) GET_REGISTER(vsrc1) _op (s1) vsrc2);                   \
        }                                                                   \
    }                                                                       \
    FINISH(2);

#define HANDLE_OP_SHX_INT_LIT8(_opcode, _opname, _cast, _op)                \
    HANDLE_OPCODE(_opcode /*vAA, vBB, #+CC*/)                               \
    {                                                                       \
        u2 litInfo;                                                         \
        vdst = INST_AA(inst);                                               \
        litInfo = FETCH(1);                                                 \
        vsrc1 = litInfo & 0xff;                                             \
        vsrc2 = litInfo >> 8;       /* constant */                          \
        MY_LOG_VERBOSE("|%s-int/lit8 v%d,v%d,#+0x%02x",                              \
            (_opname), vdst, vsrc1, vsrc2);                                 \
        SET_REGISTER(vdst,                                                  \
            _cast GET_REGISTER(vsrc1) _op (vsrc2 & 0x1f));                  \
    }                                                                       \
    FINISH(2);

#define HANDLE_OP_X_INT_2ADDR(_opcode, _opname, _op, _chkdiv)               \
    HANDLE_OPCODE(_opcode /*vA, vB*/)                                       \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);                                               \
        MY_LOG_VERBOSE("|%s-int-2addr v%d,v%d", (_opname), vdst, vsrc1);             \
        if (_chkdiv != 0) {                                                 \
            s4 firstVal, secondVal, result;                                 \
            firstVal = GET_REGISTER(vdst);                                  \
            secondVal = GET_REGISTER(vsrc1);                                \
            if (secondVal == 0) {                                           \
                EXPORT_PC();                                                \
                dvmThrowArithmeticException(env, "divide by zero");              \
                GOTO_exceptionThrown();                                     \
            }                                                               \
            if ((u4)firstVal == 0x80000000 && secondVal == -1) {            \
                if (_chkdiv == 1)                                           \
                    result = firstVal;  /* division */                      \
                else                                                        \
                    result = 0;         /* remainder */                     \
            } else {                                                        \
                result = firstVal _op secondVal;                            \
            }                                                               \
            SET_REGISTER(vdst, result);                                     \
        } else {                                                            \
            SET_REGISTER(vdst,                                              \
                (s4) GET_REGISTER(vdst) _op (s4) GET_REGISTER(vsrc1));      \
        }                                                                   \
        FINISH(1);

#define HANDLE_OP_SHX_INT_2ADDR(_opcode, _opname, _cast, _op)               \
    HANDLE_OPCODE(_opcode /*vA, vB*/)                                       \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);                                               \
        MY_LOG_VERBOSE("|%s-int-2addr v%d,v%d", (_opname), vdst, vsrc1);             \
        SET_REGISTER(vdst,                                                  \
            _cast GET_REGISTER(vdst) _op (GET_REGISTER(vsrc1) & 0x1f));     \
        FINISH(1);

#define HANDLE_OP_X_LONG(_opcode, _opname, _op, _chkdiv)                    \
    HANDLE_OPCODE(_opcode /*vAA, vBB, vCC*/)                                \
    {                                                                       \
        u2 srcRegs;                                                         \
        vdst = INST_AA(inst);                                               \
        srcRegs = FETCH(1);                                                 \
        vsrc1 = srcRegs & 0xff;                                             \
        vsrc2 = srcRegs >> 8;                                               \
        MY_LOG_VERBOSE("|%s-long v%d,v%d,v%d", (_opname), vdst, vsrc1, vsrc2);       \
        if (_chkdiv != 0) {                                                 \
            s8 firstVal, secondVal, result;                                 \
            firstVal = GET_REGISTER_WIDE(vsrc1);                            \
            secondVal = GET_REGISTER_WIDE(vsrc2);                           \
            if (secondVal == 0LL) {                                         \
                EXPORT_PC();                                                \
                dvmThrowArithmeticException(env, "divide by zero");              \
                GOTO_exceptionThrown();                                     \
            }                                                               \
            if ((u8)firstVal == 0x8000000000000000ULL &&                    \
                secondVal == -1LL)                                          \
            {                                                               \
                if (_chkdiv == 1)                                           \
                    result = firstVal;  /* division */                      \
                else                                                        \
                    result = 0;         /* remainder */                     \
            } else {                                                        \
                result = firstVal _op secondVal;                            \
            }                                                               \
            SET_REGISTER_WIDE(vdst, result);                                \
        } else {                                                            \
            SET_REGISTER_WIDE(vdst,                                         \
                (s8) GET_REGISTER_WIDE(vsrc1) _op (s8) GET_REGISTER_WIDE(vsrc2)); \
        }                                                                   \
    }                                                                       \
    FINISH(2);

#define HANDLE_OP_SHX_LONG(_opcode, _opname, _cast, _op)                    \
    HANDLE_OPCODE(_opcode /*vAA, vBB, vCC*/)                                \
    {                                                                       \
        u2 srcRegs;                                                         \
        vdst = INST_AA(inst);                                               \
        srcRegs = FETCH(1);                                                 \
        vsrc1 = srcRegs & 0xff;                                             \
        vsrc2 = srcRegs >> 8;                                               \
        MY_LOG_VERBOSE("|%s-long v%d,v%d,v%d", (_opname), vdst, vsrc1, vsrc2);       \
        SET_REGISTER_WIDE(vdst,                                             \
            _cast GET_REGISTER_WIDE(vsrc1) _op (GET_REGISTER(vsrc2) & 0x3f)); \
    }                                                                       \
    FINISH(2);

#define HANDLE_OP_X_LONG_2ADDR(_opcode, _opname, _op, _chkdiv)              \
    HANDLE_OPCODE(_opcode /*vA, vB*/)                                       \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);                                               \
        MY_LOG_VERBOSE("|%s-long-2addr v%d,v%d", (_opname), vdst, vsrc1);            \
        if (_chkdiv != 0) {                                                 \
            s8 firstVal, secondVal, result;                                 \
            firstVal = GET_REGISTER_WIDE(vdst);                             \
            secondVal = GET_REGISTER_WIDE(vsrc1);                           \
            if (secondVal == 0LL) {                                         \
                EXPORT_PC();                                                \
                dvmThrowArithmeticException(env, "divide by zero");              \
                GOTO_exceptionThrown();                                     \
            }                                                               \
            if ((u8)firstVal == 0x8000000000000000ULL &&                    \
                secondVal == -1LL)                                          \
            {                                                               \
                if (_chkdiv == 1)                                           \
                    result = firstVal;  /* division */                      \
                else                                                        \
                    result = 0;         /* remainder */                     \
            } else {                                                        \
                result = firstVal _op secondVal;                            \
            }                                                               \
            SET_REGISTER_WIDE(vdst, result);                                \
        } else {                                                            \
            SET_REGISTER_WIDE(vdst,                                         \
                (s8) GET_REGISTER_WIDE(vdst) _op (s8)GET_REGISTER_WIDE(vsrc1));\
        }                                                                   \
        FINISH(1);

#define HANDLE_OP_SHX_LONG_2ADDR(_opcode, _opname, _cast, _op)              \
    HANDLE_OPCODE(_opcode /*vA, vB*/)                                       \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);                                               \
        MY_LOG_VERBOSE("|%s-long-2addr v%d,v%d", (_opname), vdst, vsrc1);            \
        SET_REGISTER_WIDE(vdst,                                             \
            _cast GET_REGISTER_WIDE(vdst) _op (GET_REGISTER(vsrc1) & 0x3f)); \
        FINISH(1);

#define HANDLE_OP_X_FLOAT(_opcode, _opname, _op)                            \
    HANDLE_OPCODE(_opcode /*vAA, vBB, vCC*/)                                \
    {                                                                       \
        u2 srcRegs;                                                         \
        vdst = INST_AA(inst);                                               \
        srcRegs = FETCH(1);                                                 \
        vsrc1 = srcRegs & 0xff;                                             \
        vsrc2 = srcRegs >> 8;                                               \
        MY_LOG_VERBOSE("|%s-float v%d,v%d,v%d", (_opname), vdst, vsrc1, vsrc2);      \
        SET_REGISTER_FLOAT(vdst,                                            \
            GET_REGISTER_FLOAT(vsrc1) _op GET_REGISTER_FLOAT(vsrc2));       \
    }                                                                       \
    FINISH(2);

#define HANDLE_OP_X_DOUBLE(_opcode, _opname, _op)                           \
    HANDLE_OPCODE(_opcode /*vAA, vBB, vCC*/)                                \
    {                                                                       \
        u2 srcRegs;                                                         \
        vdst = INST_AA(inst);                                               \
        srcRegs = FETCH(1);                                                 \
        vsrc1 = srcRegs & 0xff;                                             \
        vsrc2 = srcRegs >> 8;                                               \
        MY_LOG_VERBOSE("|%s-double v%d,v%d,v%d", (_opname), vdst, vsrc1, vsrc2);     \
        SET_REGISTER_DOUBLE(vdst,                                           \
            GET_REGISTER_DOUBLE(vsrc1) _op GET_REGISTER_DOUBLE(vsrc2));     \
    }                                                                       \
    FINISH(2);

#define HANDLE_OP_X_FLOAT_2ADDR(_opcode, _opname, _op)                      \
    HANDLE_OPCODE(_opcode /*vA, vB*/)                                       \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);                                               \
        MY_LOG_VERBOSE("|%s-float-2addr v%d,v%d", (_opname), vdst, vsrc1);           \
        SET_REGISTER_FLOAT(vdst,                                            \
            GET_REGISTER_FLOAT(vdst) _op GET_REGISTER_FLOAT(vsrc1));        \
        FINISH(1);

#define HANDLE_OP_X_DOUBLE_2ADDR(_opcode, _opname, _op)                     \
    HANDLE_OPCODE(_opcode /*vA, vB*/)                                       \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);                                               \
        MY_LOG_VERBOSE("|%s-double-2addr v%d,v%d", (_opname), vdst, vsrc1);          \
        SET_REGISTER_DOUBLE(vdst,                                           \
            GET_REGISTER_DOUBLE(vdst) _op GET_REGISTER_DOUBLE(vsrc1));      \
        FINISH(1);

#define HANDLE_OP_AGET(_opcode, _opname, _type, _regsize)                   \
    HANDLE_OPCODE(_opcode /*vAA, vBB, vCC*/)                                \
    {                                                                       \
        ArrayObject* arrayObj;                                              \
        u2 arrayInfo;                                                       \
        EXPORT_PC();                                                        \
        vdst = INST_AA(inst);                                               \
        arrayInfo = FETCH(1);                                               \
        vsrc1 = arrayInfo & 0xff;    /* array ptr */                        \
        vsrc2 = arrayInfo >> 8;      /* index */                            \
        MY_LOG_VERBOSE("|aget%s v%d,v%d,v%d", (_opname), vdst, vsrc1, vsrc2);        \
        arrayObj = (ArrayObject*) GET_REGISTER(vsrc1);                      \
        if (!checkForNull(env, (Object*) arrayObj))                              \
            GOTO_exceptionThrown();                                         \
        if (GET_REGISTER(vsrc2) >= arrayObj->length) {                      \
            dvmThrowArrayIndexOutOfBoundsException(                         \
                arrayObj->length, GET_REGISTER(vsrc2));                     \
            GOTO_exceptionThrown();                                         \
        }                                                                   \
        SET_REGISTER##_regsize(vdst,                                        \
            ((_type*)(void*)arrayObj->contents)[GET_REGISTER(vsrc2)]);      \
        MY_LOG_VERBOSE("+ AGET[%d]=%#x", GET_REGISTER(vsrc2), GET_REGISTER(vdst));   \
    }                                                                       \
    FINISH(2);

#define HANDLE_OP_APUT(_opcode, _opname, _type, _regsize)                   \
    HANDLE_OPCODE(_opcode /*vAA, vBB, vCC*/)                                \
    {                                                                       \
        ArrayObject* arrayObj;                                              \
        u2 arrayInfo;                                                       \
        EXPORT_PC();                                                        \
        vdst = INST_AA(inst);       /* AA: source value */                  \
        arrayInfo = FETCH(1);                                               \
        vsrc1 = arrayInfo & 0xff;   /* BB: array ptr */                     \
        vsrc2 = arrayInfo >> 8;     /* CC: index */                         \
        MY_LOG_VERBOSE("|aput%s v%d,v%d,v%d", (_opname), vdst, vsrc1, vsrc2);        \
        arrayObj = (ArrayObject*) GET_REGISTER(vsrc1);                      \
        if (!checkForNull(env, (Object*) arrayObj))                              \
            GOTO_exceptionThrown();                                         \
        if (GET_REGISTER(vsrc2) >= arrayObj->length) {                      \
            dvmThrowArrayIndexOutOfBoundsException(                         \
                arrayObj->length, GET_REGISTER(vsrc2));                     \
            GOTO_exceptionThrown();                                         \
        }                                                                   \
        MY_LOG_VERBOSE("+ APUT[%d]=0x%08x", GET_REGISTER(vsrc2), GET_REGISTER(vdst));\
        ((_type*)(void*)arrayObj->contents)[GET_REGISTER(vsrc2)] =          \
            GET_REGISTER##_regsize(vdst);                                   \
    }                                                                       \
    FINISH(2);
#define HANDLE_IGET_X(_opcode, _opname, _ftype, _regsize)                   \
    HANDLE_OPCODE(_opcode /*vA, vB, field@CCCC*/)                           \
    {                                                                       \
        InstField* ifield;                                                  \
        Object* obj;                                                        \
        EXPORT_PC();                                                        \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);   /* object ptr */                            \
        ref = FETCH(1);         /* field ref */                             \
        ILOGV("|iget%s v%d,v%d,field@0x%04x", (_opname), vdst, vsrc1, ref); \
        obj = (Object*) GET_REGISTER(vsrc1);                                \
        if (!checkForNull(obj))                                             \
            GOTO_exceptionThrown();                                         \
        ifield = (InstField*) dvmDexGetResolvedField(methodClassDex, ref);  \
        if (ifield == NULL) {                                               \
            ifield = dvmResolveInstField(curMethod->clazz, ref);            \
            if (ifield == NULL)                                             \
                GOTO_exceptionThrown();                                     \
        }                                                                   \
        SET_REGISTER##_regsize(vdst,                                        \
            dvmGetField##_ftype(obj, ifield->byteOffset));                  \
        ILOGV("+ IGET '%s'=0x%08llx", ifield->name,                         \
            (u8) GET_REGISTER##_regsize(vdst));                             \
    }                                                                       \
    FINISH(2);
#define HANDLE_SGET_X(_opcode, _opname, _ftype, _regsize)                   \
    HANDLE_OPCODE(_opcode /*vAA, field@BBBB*/)                              \
    {                                                                       \
        StaticField* sfield;                                                \
        vdst = INST_AA(inst);                                               \
        ref = FETCH(1);         /* field ref */                             \
        ILOGV("|sget%s v%d,sfield@0x%04x", (_opname), vdst, ref);           \
        sfield = (StaticField*)dvmDexGetResolvedField(methodClassDex, ref); \
        if (sfield == NULL) {                                               \
            EXPORT_PC();                                                    \
            sfield = dvmResolveStaticField(curMethod->clazz, ref);          \
            if (sfield == NULL)                                             \
                GOTO_exceptionThrown();                                     \
            if (dvmDexGetResolvedField(methodClassDex, ref) == NULL) {      \
                JIT_STUB_HACK(dvmJitEndTraceSelect(self,pc));               \
            }                                                               \
        }                                                                   \
        SET_REGISTER##_regsize(vdst, dvmGetStaticField##_ftype(sfield));    \
        ILOGV("+ SGET '%s'=0x%08llx",                                       \
            sfield->name, (u8)GET_REGISTER##_regsize(vdst));                \
    }                                                                       \
    FINISH(2);
#define HANDLE_IPUT_X(_opcode, _opname, _ftype, _regsize)                   \
    HANDLE_OPCODE(_opcode /*vA, vB, field@CCCC*/)                           \
    {                                                                       \
        InstField* ifield;                                                  \
        Object* obj;                                                        \
        EXPORT_PC();                                                        \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);   /* object ptr */                            \
        ref = FETCH(1);         /* field ref */                             \
        ILOGV("|iput%s v%d,v%d,field@0x%04x", (_opname), vdst, vsrc1, ref); \
        obj = (Object*) GET_REGISTER(vsrc1);                                \
        if (!checkForNull(obj))                                             \
            GOTO_exceptionThrown();                                         \
        ifield = (InstField*) dvmDexGetResolvedField(methodClassDex, ref);  \
        if (ifield == NULL) {                                               \
            ifield = dvmResolveInstField(curMethod->clazz, ref);            \
            if (ifield == NULL)                                             \
                GOTO_exceptionThrown();                                     \
        }                                                                   \
        dvmSetField##_ftype(obj, ifield->byteOffset,                        \
            GET_REGISTER##_regsize(vdst));                                  \
        ILOGV("+ IPUT '%s'=0x%08llx", ifield->name,                         \
            (u8) GET_REGISTER##_regsize(vdst));                             \
    }                                                                       \
    FINISH(2);
#define HANDLE_SPUT_X(_opcode, _opname, _ftype, _regsize)                   \
    HANDLE_OPCODE(_opcode /*vAA, field@BBBB*/)                              \
    {                                                                       \
        StaticField* sfield;                                                \
        vdst = INST_AA(inst);                                               \
        ref = FETCH(1);         /* field ref */                             \
        ILOGV("|sput%s v%d,sfield@0x%04x", (_opname), vdst, ref);           \
        sfield = (StaticField*)dvmDexGetResolvedField(methodClassDex, ref); \
        if (sfield == NULL) {                                               \
            EXPORT_PC();                                                    \
            sfield = dvmResolveStaticField(curMethod->clazz, ref);          \
            if (sfield == NULL)                                             \
                GOTO_exceptionThrown();                                     \
            if (dvmDexGetResolvedField(methodClassDex, ref) == NULL) {      \
                JIT_STUB_HACK(dvmJitEndTraceSelect(self,pc));               \
            }                                                               \
        }                                                                   \
        dvmSetStaticField##_ftype(sfield, GET_REGISTER##_regsize(vdst));    \
        ILOGV("+ SPUT '%s'=0x%08llx",                                       \
            sfield->name, (u8)GET_REGISTER##_regsize(vdst));                \
    }                                                                       \
    FINISH(2);
#define HANDLE_IGET_X_QUICK(_opcode, _opname, _ftype, _regsize)             \
    HANDLE_OPCODE(_opcode /*vA, vB, field@CCCC*/)                           \
    {                                                                       \
        Object* obj;                                                        \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);   /* object ptr */                            \
        ref = FETCH(1);         /* field offset */                          \
        ILOGV("|iget%s-quick v%d,v%d,field@+%u",                            \
            (_opname), vdst, vsrc1, ref);                                   \
        obj = (Object*) GET_REGISTER(vsrc1);                                \
        if (!checkForNullExportPC(obj, fp, pc))                             \
            GOTO_exceptionThrown();                                         \
        SET_REGISTER##_regsize(vdst, dvmGetField##_ftype(obj, ref));        \
        ILOGV("+ IGETQ %d=0x%08llx", ref,                                   \
            (u8) GET_REGISTER##_regsize(vdst));                             \
    }                                                                       \
    FINISH(2);
#define HANDLE_IPUT_X_QUICK(_opcode, _opname, _ftype, _regsize)             \
    HANDLE_OPCODE(_opcode /*vA, vB, field@CCCC*/)                           \
    {                                                                       \
        Object* obj;                                                        \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);   /* object ptr */                            \
        ref = FETCH(1);         /* field offset */                          \
        ILOGV("|iput%s-quick v%d,v%d,field@0x%04x",                         \
            (_opname), vdst, vsrc1, ref);                                   \
        obj = (Object*) GET_REGISTER(vsrc1);                                \
        if (!checkForNullExportPC(obj, fp, pc))                             \
            GOTO_exceptionThrown();                                         \
        dvmSetField##_ftype(obj, ref, GET_REGISTER##_regsize(vdst));        \
        ILOGV("+ IPUTQ %d=0x%08llx", ref,                                   \
            (u8) GET_REGISTER##_regsize(vdst));                             \
    }                                                                       \
    FINISH(2);

//////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////

#define GOTO_bail() goto bail;

//////////////////////////////////////////////////////////////////////////

jvalue BWdvmInterpretPortable(const SeparatorData* separatorData, JNIEnv* env, jobject thiz, ...) {
    jvalue* params = NULL; // 参数数组。
    JValue retval;  // 返回值。
    DvmDex* methodClassDex;
    const Method* curMethod;
    const Method* methodToCall;
    const u2* pc;   // 程序计数器。
    u4 fp[65535];   // 寄存器数组。
    u4 ref;
    u2 inst;        // 当前指令。
    u2 vsrc1, vsrc2, vdst;      // usually used for register indexes
    bool methodCallRange;
    unsigned int startIndex;

    // 处理参数。
    va_list args;
    va_start(args, thiz); 
    params = getParams(separatorData, args);
    va_end(args);

    // 获得参数寄存器个数。    
    size_t paramRegCount = getParamRegCount(separatorData);

    // 设置参数寄存器的值。
    if (isStaticMethod(separatorData)) {
        startIndex = separatorData->registerSize - separatorData->paramSize;
    } else {
        startIndex = separatorData->registerSize - separatorData->paramSize;
        fp[startIndex++] = (u4)thiz;
    }
    for (int i = startIndex, j = 0; j < separatorData->paramSize; j++ ) {
        if ('D' == separatorData->paramShortDesc.str[i] || 'J' == separatorData->paramShortDesc.str[i]) {
            fp[i++] = params[j].j & 0xFFFFFFFF;
            fp[i++] = (params[j].j >> 32) & 0xFFFFFFFF;
        } else {
            fp[i++] = params[j].i;
        }
    }

    pc = separatorData->insts;

    /* static computed goto table */
    DEFINE_GOTO_TABLE(handlerTable);

    // 抓取第一条指令。
    FINISH(0);

/*--- start of opcodes ---*/

/* File: c/OP_NOP.cpp */
HANDLE_OPCODE(OP_NOP)
    FINISH(1);
OP_END

/* File: c/OP_MOVE.cpp */
HANDLE_OPCODE(OP_MOVE /*vA, vB*/)
    vdst = INST_A(inst);
    vsrc1 = INST_B(inst);
    MY_LOG_VERBOSE("|move%s v%d,v%d %s(v%d=0x%08x)",
        (INST_INST(inst) == OP_MOVE) ? "" : "-object", vdst, vsrc1,
        kSpacing, vdst, GET_REGISTER(vsrc1));
    SET_REGISTER(vdst, GET_REGISTER(vsrc1));
    FINISH(1);
OP_END

/* File: c/OP_MOVE_FROM16.cpp */
HANDLE_OPCODE(OP_MOVE_FROM16 /*vAA, vBBBB*/)
    vdst = INST_AA(inst);
    vsrc1 = FETCH(1);
    MY_LOG_VERBOSE("|move%s/from16 v%d,v%d %s(v%d=0x%08x)",
        (INST_INST(inst) == OP_MOVE_FROM16) ? "" : "-object", vdst, vsrc1,
        kSpacing, vdst, GET_REGISTER(vsrc1));
    SET_REGISTER(vdst, GET_REGISTER(vsrc1));
    FINISH(2);
OP_END

/* File: c/OP_MOVE_16.cpp */
HANDLE_OPCODE(OP_MOVE_16 /*vAAAA, vBBBB*/)
    vdst = FETCH(1);
    vsrc1 = FETCH(2);
    MY_LOG_VERBOSE("|move%s/16 v%d,v%d %s(v%d=0x%08x)",
        (INST_INST(inst) == OP_MOVE_16) ? "" : "-object", vdst, vsrc1,
        kSpacing, vdst, GET_REGISTER(vsrc1));
    SET_REGISTER(vdst, GET_REGISTER(vsrc1));
    FINISH(3);
OP_END

/* File: c/OP_MOVE_WIDE.cpp */
HANDLE_OPCODE(OP_MOVE_WIDE /*vA, vB*/)
    /* IMPORTANT: must correctly handle overlapping registers, e.g. both
     * "move-wide v6, v7" and "move-wide v7, v6" */
    vdst = INST_A(inst);
    vsrc1 = INST_B(inst);
    MY_LOG_VERBOSE("|move-wide v%d,v%d %s(v%d=0x%08llx)", vdst, vsrc1,
        kSpacing+5, vdst, GET_REGISTER_WIDE(vsrc1));
    SET_REGISTER_WIDE(vdst, GET_REGISTER_WIDE(vsrc1));
    FINISH(1);
OP_END

/* File: c/OP_MOVE_WIDE_FROM16.cpp */
HANDLE_OPCODE(OP_MOVE_WIDE_FROM16 /*vAA, vBBBB*/)
    vdst = INST_AA(inst);
    vsrc1 = FETCH(1);
    MY_LOG_VERBOSE("|move-wide/from16 v%d,v%d  (v%d=0x%08llx)", vdst, vsrc1,
        vdst, GET_REGISTER_WIDE(vsrc1));
    SET_REGISTER_WIDE(vdst, GET_REGISTER_WIDE(vsrc1));
    FINISH(2);
OP_END

/* File: c/OP_MOVE_WIDE_16.cpp */
HANDLE_OPCODE(OP_MOVE_WIDE_16 /*vAAAA, vBBBB*/)
    vdst = FETCH(1);
    vsrc1 = FETCH(2);
    MY_LOG_VERBOSE("|move-wide/16 v%d,v%d %s(v%d=0x%08llx)", vdst, vsrc1,
        kSpacing+8, vdst, GET_REGISTER_WIDE(vsrc1));
    SET_REGISTER_WIDE(vdst, GET_REGISTER_WIDE(vsrc1));
    FINISH(3);
OP_END

/* File: c/OP_MOVE_OBJECT.cpp */
/* File: c/OP_MOVE.cpp */
HANDLE_OPCODE(OP_MOVE_OBJECT /*vA, vB*/)
    vdst = INST_A(inst);
    vsrc1 = INST_B(inst);
    MY_LOG_VERBOSE("|move%s v%d,v%d %s(v%d=0x%08x)",
        (INST_INST(inst) == OP_MOVE) ? "" : "-object", vdst, vsrc1,
        kSpacing, vdst, GET_REGISTER(vsrc1));
    SET_REGISTER(vdst, GET_REGISTER(vsrc1));
    FINISH(1);
OP_END


/* File: c/OP_MOVE_OBJECT_FROM16.cpp */
/* File: c/OP_MOVE_FROM16.cpp */
HANDLE_OPCODE(OP_MOVE_OBJECT_FROM16 /*vAA, vBBBB*/)
    vdst = INST_AA(inst);
    vsrc1 = FETCH(1);
    MY_LOG_VERBOSE("|move%s/from16 v%d,v%d %s(v%d=0x%08x)",
        (INST_INST(inst) == OP_MOVE_FROM16) ? "" : "-object", vdst, vsrc1,
        kSpacing, vdst, GET_REGISTER(vsrc1));
    SET_REGISTER(vdst, GET_REGISTER(vsrc1));
    FINISH(2);
OP_END


/* File: c/OP_MOVE_OBJECT_16.cpp */
/* File: c/OP_MOVE_16.cpp */
HANDLE_OPCODE(OP_MOVE_OBJECT_16 /*vAAAA, vBBBB*/)
    vdst = FETCH(1);
    vsrc1 = FETCH(2);
    MY_LOG_VERBOSE("|move%s/16 v%d,v%d %s(v%d=0x%08x)",
        (INST_INST(inst) == OP_MOVE_16) ? "" : "-object", vdst, vsrc1,
        kSpacing, vdst, GET_REGISTER(vsrc1));
    SET_REGISTER(vdst, GET_REGISTER(vsrc1));
    FINISH(3);
OP_END


/* File: c/OP_MOVE_RESULT.cpp */
HANDLE_OPCODE(OP_MOVE_RESULT /*vAA*/)
    vdst = INST_AA(inst);
    MY_LOG_VERBOSE("|move-result%s v%d %s(v%d=0x%08x)",
         (INST_INST(inst) == OP_MOVE_RESULT) ? "" : "-object",
         vdst, kSpacing+4, vdst,retval.i);
    SET_REGISTER(vdst, retval.i);
    FINISH(1);
OP_END

/* File: c/OP_MOVE_RESULT_WIDE.cpp */
HANDLE_OPCODE(OP_MOVE_RESULT_WIDE /*vAA*/)
    vdst = INST_AA(inst);
    MY_LOG_VERBOSE("|move-result-wide v%d %s(0x%08llx)", vdst, kSpacing, retval.j);
    SET_REGISTER_WIDE(vdst, retval.j);
    FINISH(1);
OP_END

/* File: c/OP_MOVE_RESULT_OBJECT.cpp */
/* File: c/OP_MOVE_RESULT.cpp */
HANDLE_OPCODE(OP_MOVE_RESULT_OBJECT /*vAA*/)
    vdst = INST_AA(inst);
    MY_LOG_VERBOSE("|move-result%s v%d %s(v%d=0x%08x)",
         (INST_INST(inst) == OP_MOVE_RESULT) ? "" : "-object",
         vdst, kSpacing+4, vdst,retval.i);
    SET_REGISTER(vdst, retval.i);
    FINISH(1);
OP_END


// TODO 异常还不支持。
/* File: c/OP_MOVE_EXCEPTION.cpp */
HANDLE_OPCODE(OP_MOVE_EXCEPTION /*vAA*/)
    vdst = INST_AA(inst);
    MY_LOG_VERBOSE("|move-exception v%d", vdst);
    /*assert(self->exception != NULL);*/
    /*SET_REGISTER(vdst, (u4)self->exception);*/
    /*dvmClearException(self);*/
    FINISH(1);
OP_END

/* File: c/OP_RETURN_VOID.cpp */
HANDLE_OPCODE(OP_RETURN_VOID /**/)
    MY_LOG_VERBOSE("|return-void");
#ifndef NDEBUG
    retval.j = 0xababababULL;    // placate valgrind
#endif
    /*GOTO_returnFromMethod();*/
    GOTO_bail();
OP_END

/* File: c/OP_RETURN.cpp */
HANDLE_OPCODE(OP_RETURN /*vAA*/)
    vsrc1 = INST_AA(inst);
    MY_LOG_VERBOSE("|return%s v%d",
        (INST_INST(inst) == OP_RETURN) ? "" : "-object", vsrc1);
    retval.i = GET_REGISTER(vsrc1);
    /*GOTO_returnFromMethod();*/
    GOTO_bail();
OP_END

/* File: c/OP_RETURN_WIDE.cpp */
HANDLE_OPCODE(OP_RETURN_WIDE /*vAA*/)
    vsrc1 = INST_AA(inst);
    MY_LOG_VERBOSE("|return-wide v%d", vsrc1);
    retval.j = GET_REGISTER_WIDE(vsrc1);
    /*GOTO_returnFromMethod();*/
    GOTO_bail();
OP_END

/* File: c/OP_RETURN_OBJECT.cpp */
/* File: c/OP_RETURN.cpp */
HANDLE_OPCODE(OP_RETURN_OBJECT /*vAA*/)
    vsrc1 = INST_AA(inst);
    MY_LOG_VERBOSE("|return%s v%d",
        (INST_INST(inst) == OP_RETURN) ? "" : "-object", vsrc1);
    retval.i = GET_REGISTER(vsrc1);
    /*GOTO_returnFromMethod();*/
    GOTO_bail();
OP_END


/* File: c/OP_CONST_4.cpp */
HANDLE_OPCODE(OP_CONST_4 /*vA, #+B*/)
    {
        s4 tmp;

        vdst = INST_A(inst);
        tmp = (s4) (INST_B(inst) << 28) >> 28;  // sign extend 4-bit value
        MY_LOG_VERBOSE("|const/4 v%d,#0x%02x", vdst, (s4)tmp);
        SET_REGISTER(vdst, tmp);
    }
    FINISH(1);
OP_END

/* File: c/OP_CONST_16.cpp */
HANDLE_OPCODE(OP_CONST_16 /*vAA, #+BBBB*/)
    vdst = INST_AA(inst);
    vsrc1 = FETCH(1);
    MY_LOG_VERBOSE("|const/16 v%d,#0x%04x", vdst, (s2)vsrc1);
    SET_REGISTER(vdst, (s2) vsrc1);
    FINISH(2);
OP_END

/* File: c/OP_CONST.cpp */
HANDLE_OPCODE(OP_CONST /*vAA, #+BBBBBBBB*/)
    {
        u4 tmp;

        vdst = INST_AA(inst);
        tmp = FETCH(1);
        tmp |= (u4)FETCH(2) << 16;
        MY_LOG_VERBOSE("|const v%d,#0x%08x", vdst, tmp);
        SET_REGISTER(vdst, tmp);
    }
    FINISH(3);
OP_END

/* File: c/OP_CONST_HIGH16.cpp */
HANDLE_OPCODE(OP_CONST_HIGH16 /*vAA, #+BBBB0000*/)
    vdst = INST_AA(inst);
    vsrc1 = FETCH(1);
    MY_LOG_VERBOSE("|const/high16 v%d,#0x%04x0000", vdst, vsrc1);
    SET_REGISTER(vdst, vsrc1 << 16);
    FINISH(2);
OP_END

/* File: c/OP_CONST_WIDE_16.cpp */
HANDLE_OPCODE(OP_CONST_WIDE_16 /*vAA, #+BBBB*/)
    vdst = INST_AA(inst);
    vsrc1 = FETCH(1);
    MY_LOG_VERBOSE("|const-wide/16 v%d,#0x%04x", vdst, (s2)vsrc1);
    SET_REGISTER_WIDE(vdst, (s2)vsrc1);
    FINISH(2);
OP_END

/* File: c/OP_CONST_WIDE_32.cpp */
HANDLE_OPCODE(OP_CONST_WIDE_32 /*vAA, #+BBBBBBBB*/)
    {
        u4 tmp;

        vdst = INST_AA(inst);
        tmp = FETCH(1);
        tmp |= (u4)FETCH(2) << 16;
        MY_LOG_VERBOSE("|const-wide/32 v%d,#0x%08x", vdst, tmp);
        SET_REGISTER_WIDE(vdst, (s4) tmp);
    }
    FINISH(3);
OP_END

/* File: c/OP_CONST_WIDE.cpp */
HANDLE_OPCODE(OP_CONST_WIDE /*vAA, #+BBBBBBBBBBBBBBBB*/)
    {
        u8 tmp;

        vdst = INST_AA(inst);
        tmp = FETCH(1);
        tmp |= (u8)FETCH(2) << 16;
        tmp |= (u8)FETCH(3) << 32;
        tmp |= (u8)FETCH(4) << 48;
        MY_LOG_VERBOSE("|const-wide v%d,#0x%08llx", vdst, tmp);
        SET_REGISTER_WIDE(vdst, tmp);
    }
    FINISH(5);
OP_END

/* File: c/OP_CONST_WIDE_HIGH16.cpp */
HANDLE_OPCODE(OP_CONST_WIDE_HIGH16 /*vAA, #+BBBB000000000000*/)
    vdst = INST_AA(inst);
    vsrc1 = FETCH(1);
    MY_LOG_VERBOSE("|const-wide/high16 v%d,#0x%04x000000000000", vdst, vsrc1);
    SET_REGISTER_WIDE(vdst, ((u8) vsrc1) << 48);
    FINISH(2);
OP_END
HANDLE_OPCODE(OP_CONST_STRING /*vAA, string@BBBB*/)
{
    StringObject* strObj;

    vdst = INST_AA(inst);
    ref = FETCH(1);
    MY_LOG_INFO("|const-string v%d string@0x%04x", vdst, ref);
    strObj = dvmDexGetResolvedString(methodClassDex, ref);
    if (strObj == NULL) {
        EXPORT_PC();
        strObj = dvmResolveStringhook(curMethod->clazz, ref);
        if (strObj == NULL)
            GOTO_exceptionThrown();
    }
    SET_REGISTER(vdst, (u4) strObj);
}
FINISH(2);
OP_END
HANDLE_OPCODE(OP_CONST_STRING_JUMBO /*vAA, string@BBBBBBBB*/)
{
    StringObject* strObj;
    u4 tmp;

    vdst = INST_AA(inst);
    tmp = FETCH(1);
    tmp |= (u4)FETCH(2) << 16;
    MY_LOG_INFO("|const-string/jumbo v%d string@0x%08x", vdst, tmp);
    strObj = dvmDexGetResolvedString(methodClassDex, tmp);
    if (strObj == NULL) {
        EXPORT_PC();
        strObj = dvmResolveStringhook(curMethod->clazz, tmp);
        if (strObj == NULL)
            GOTO_exceptionThrown();
    }
    SET_REGISTER(vdst, (u4) strObj);
}
FINISH(3);
OP_END
HANDLE_OPCODE(OP_CONST_CLASS /*vAA, class@BBBB*/)
{
    ClassObject* clazz;

    vdst = INST_AA(inst);
    ref = FETCH(1);
    MY_LOG_INFO("|const-class v%d class@0x%04x", vdst, ref);
    clazz = dvmDexGetResolvedClass(methodClassDex, ref);
    if (clazz == NULL) {
        EXPORT_PC();
        clazz = dvmResolveClasshook(curMethod->clazz, ref, true);
        if (clazz == NULL)
            GOTO_exceptionThrown();
    }
    SET_REGISTER(vdst, (u4) clazz);
}
FINISH(2);
OP_END

HANDLE_OPCODE(OP_MONITOR_ENTER /*vAA*/)
{
    Object* obj;

    vsrc1 = INST_AA(inst);
    MY_LOG_INFO("|monitor-enter v%d %s(0x%08x)",
          vsrc1, kSpacing+6, GET_REGISTER(vsrc1));
    obj = (Object*)GET_REGISTER(vsrc1);
    if (!checkForNullExportPC(env,obj, fp, pc))
        GOTO_exceptionThrown();
    MY_LOG_INFO("+ locking %p %s", obj, obj->clazz->descriptor);
    EXPORT_PC();    /* need for precise GC */
    dvmLockObjectHook(dvmThreadSelfHook(), obj);
}
FINISH(1);
OP_END
HANDLE_OPCODE(OP_MONITOR_EXIT /*vAA*/)
{
    Object* obj;

    EXPORT_PC();

    vsrc1 = INST_AA(inst);
    MY_LOG_INFO("|monitor-exit v%d %s(0x%08x)",
          vsrc1, kSpacing+5, GET_REGISTER(vsrc1));
    obj = (Object*)GET_REGISTER(vsrc1);
    if (!checkForNull(env,obj)) {
        /*
         * The exception needs to be processed at the *following*
         * instruction, not the current instruction (see the Dalvik
         * spec).  Because we're jumping to an exception handler,
         * we're not actually at risk of skipping an instruction
         * by doing so.
         */
        ADJUST_PC(1);           /* monitor-exit width is 1 */
        GOTO_exceptionThrown();
    }
    MY_LOG_INFO("+ unlocking %p %s", obj, obj->clazz->descriptor);
    if (!dvmUnlockObjectHook(dvmThreadSelfHook(), obj)) {
        assert(dvmCheckException(dvmThreadSelfHook()));
        ADJUST_PC(1);
        GOTO_exceptionThrown();
    }
}
FINISH(1);
OP_END

/* File: c/OP_CHECK_CAST.cpp */
HANDLE_OPCODE(OP_CHECK_CAST /*vAA, class@BBBB*/)
{
    ClassObject* clazz;
    Object* obj;

    EXPORT_PC();

    vsrc1 = INST_AA(inst);
    ref = FETCH(1);         /* class to check against */
    MY_LOG_INFO("|check-cast v%d,class@0x%04x", vsrc1, ref);

    obj = (Object*)GET_REGISTER(vsrc1);
    if (obj != NULL) {
#if defined(WITH_EXTRA_OBJECT_VALIDATION)
        if (!checkForNull(obj))
            GOTO_exceptionThrown();
#endif
        clazz = dvmDexGetResolvedClass(methodClassDex, ref);
        if (clazz == NULL) {
            clazz = dvmResolveClasshook(curMethod->clazz, ref, false);
            if (clazz == NULL)
                GOTO_exceptionThrown();
        }
        if (!dvmInstanceof(obj->clazz, clazz)) {
            dvmThrowClassCastException(obj->clazz, clazz);
            GOTO_exceptionThrown();
        }
    }
}
FINISH(2);
OP_END
HANDLE_OPCODE(OP_INSTANCE_OF /*vA, vB, class@CCCC*/)
{
    ClassObject* clazz;
    Object* obj;

    vdst = INST_A(inst);
    vsrc1 = INST_B(inst);   /* object to check */
    ref = FETCH(1);         /* class to check against */
    MY_LOG_INFO("|instance-of v%d,v%d,class@0x%04x", vdst, vsrc1, ref);

    obj = (Object*)GET_REGISTER(vsrc1);
    if (obj == NULL) {
        SET_REGISTER(vdst, 0);
    } else {
#if defined(WITH_EXTRA_OBJECT_VALIDATION)
        if (!checkForNullExportPC(obj, fp, pc))
            GOTO_exceptionThrown();
#endif
        clazz = dvmDexGetResolvedClass(methodClassDex, ref);
        if (clazz == NULL) {
            EXPORT_PC();
            clazz = dvmResolveClasshook(curMethod->clazz, ref, true);
            if (clazz == NULL)
                GOTO_exceptionThrown();
        }
        SET_REGISTER(vdst, dvmInstanceof(obj->clazz, clazz));
    }
}
FINISH(2);
OP_END
HANDLE_OPCODE(OP_ARRAY_LENGTH /*vA, vB*/)
{
    ArrayObject* arrayObj;

    vdst = INST_A(inst);
    vsrc1 = INST_B(inst);
    arrayObj = (ArrayObject*) GET_REGISTER(vsrc1);
    MY_LOG_INFO("|array-length v%d,v%d  (%p)", vdst, vsrc1, arrayObj);
    if (!checkForNullExportPC(env,(Object*) arrayObj, fp, pc))
        GOTO_exceptionThrown();
    /* verifier guarantees this is an array reference */
    SET_REGISTER(vdst, arrayObj->length);
}
FINISH(1);
OP_END

HANDLE_OPCODE(OP_NEW_INSTANCE /*vAA, class@BBBB*/)
{
    ClassObject* clazz;
    Object* newObj;

    EXPORT_PC();

    vdst = INST_AA(inst);
    ref = FETCH(1);
    MY_LOG_INFO("|new-instance v%d,class@0x%04x", vdst, ref);
    clazz = dvmDexGetResolvedClass(methodClassDex, ref);
    if (clazz == NULL) {
        clazz = dvmResolveClasshook(curMethod->clazz, ref, false);
        if (clazz == NULL)
            GOTO_exceptionThrown();
    }

    if (!dvmIsClassInitialized(clazz) && !dvmInitClassHook(clazz))
        GOTO_exceptionThrown();

#if defined(WITH_JIT)
    /*
     * The JIT needs dvmDexGetResolvedClass() to return non-null.
     * Since we use the portable interpreter to build the trace, this extra
     * check is not needed for mterp.
     */
    if ((self->interpBreak.ctl.subMode & kSubModeJitTraceBuild) &&
        (!dvmDexGetResolvedClass(methodClassDex, ref))) {
        /* Class initialization is still ongoing - end the trace */
        dvmJitEndTraceSelect(self,pc);
    }
#endif

    /*
     * Verifier now tests for interface/abstract class.
     */
    //if (dvmIsInterfaceClass(clazz) || dvmIsAbstractClass(clazz)) {
    //    dvmThrowExceptionWithClassMessage(gDvm.exInstantiationError,
    //        clazz->descriptor);
    //    GOTO_exceptionThrown();
    //}
    newObj = dvmAllocObjectHook(clazz, ALLOC_DONT_TRACK);
    if (newObj == NULL)
        GOTO_exceptionThrown();
    SET_REGISTER(vdst, (u4) newObj);
}
FINISH(2);
OP_END
HANDLE_OPCODE(OP_NEW_ARRAY /*vA, vB, class@CCCC*/)
{
    ClassObject* arrayClass;
    ArrayObject* newArray;
    s4 length;

    EXPORT_PC();

    vdst = INST_A(inst);
    vsrc1 = INST_B(inst);       /* length reg */
    ref = FETCH(1);
    MY_LOG_INFO("|new-array v%d,v%d,class@0x%04x  (%d elements)",
          vdst, vsrc1, ref, (s4) GET_REGISTER(vsrc1));
    length = (s4) GET_REGISTER(vsrc1);
    if (length < 0) {
        dvmThrowNegativeArraySizeException(length);
        GOTO_exceptionThrown();
    }
    arrayClass = dvmDexGetResolvedClass(methodClassDex, ref);
    if (arrayClass == NULL) {
        arrayClass = dvmResolveClasshook(curMethod->clazz, ref, false);
        if (arrayClass == NULL)
            GOTO_exceptionThrown();
    }
    /* verifier guarantees this is an array class */
    assert(dvmIsArrayClass(arrayClass));
    assert(dvmIsClassInitialized(arrayClass));

    newArray = dvmAllocArrayByClassHook(arrayClass, length, ALLOC_DONT_TRACK);
    if (newArray == NULL)
        GOTO_exceptionThrown();
    SET_REGISTER(vdst, (u4) newArray);
}
FINISH(2);
OP_END

HANDLE_OPCODE(OP_FILLED_NEW_ARRAY /*vB, {vD, vE, vF, vG, vA}, class@CCCC*/)
GOTO_invoke(filledNewArray,false);
OP_END
HANDLE_OPCODE(OP_FILLED_NEW_ARRAY_RANGE /*{vCCCC..v(CCCC+AA-1)}, class@BBBB*/)
GOTO_invoke(filledNewArray, true);
OP_END
HANDLE_OPCODE(OP_FILL_ARRAY_DATA)   /*vAA, +BBBBBBBB*/
{
    const u2* arrayData;
    s4 offset;
    ArrayObject* arrayObj;

    EXPORT_PC();
    vsrc1 = INST_AA(inst);
    offset = FETCH(1) | (((s4) FETCH(2)) << 16);
    MY_LOG_INFO("|fill-array-data v%d +0x%04x", vsrc1, offset);
    arrayData = pc + offset;       // offset in 16-bit units
#ifndef NDEBUG
    if (arrayData < curMethod->insns ||
        arrayData >= curMethod->insns + dvmGetMethodInsnsSize(curMethod))
    {
        /* should have been caught in verifier */
        dvmThrowInternalError("bad fill array data");
        GOTO_exceptionThrown();
    }
#endif
    arrayObj = (ArrayObject*) GET_REGISTER(vsrc1);
    if (!dvmInterpHandleFillArrayData(env,arrayObj, arrayData)) {
        GOTO_exceptionThrown();
    }
    FINISH(3);
}
OP_END
HANDLE_OPCODE(OP_THROW /*vAA*/)
{
    Object* obj;

    /*
     * We don't create an exception here, but the process of searching
     * for a catch block can do class lookups and throw exceptions.
     * We need to update the saved PC.
     */
    EXPORT_PC();

    vsrc1 = INST_AA(inst);
    MY_LOG_INFO("|throw v%d  (%p)", vsrc1, (void*)GET_REGISTER(vsrc1));
    obj = (Object*) GET_REGISTER(vsrc1);
    if (!checkForNull(env,obj)) {
        /* will throw a null pointer exception */
        MY_LOG_INFO("Bad exception");
    } else {
        /* use the requested exception */
        dvmSetException(dvmThreadSelfHook(), obj);
    }
    GOTO_exceptionThrown();
}
OP_END
HANDLE_OPCODE(OP_GOTO /*+AA*/)
vdst = INST_AA(inst);
if ((s1)vdst < 0)
    MY_LOG_INFO("|goto -0x%02x", -((s1)vdst));
else
    MY_LOG_INFO("|goto +0x%02x", ((s1)vdst));
    MY_LOG_INFO("> branch taken");
if ((s1)vdst < 0)
PERIODIC_CHECKS((s1)vdst);
FINISH((s1)vdst);
OP_END
HANDLE_OPCODE(OP_GOTO_16 /*+AAAA*/)
{
    s4 offset = (s2) FETCH(1);          /* sign-extend next code unit */

    if (offset < 0)
        MY_LOG_INFO("|goto/16 -0x%04x", -offset);
    else
        MY_LOG_INFO("|goto/16 +0x%04x", offset);
    MY_LOG_INFO("> branch taken");
    if (offset < 0)
    PERIODIC_CHECKS(offset);
    FINISH(offset);
}
OP_END

/* File: c/OP_GOTO_32.cpp */
HANDLE_OPCODE(OP_GOTO_32 /*+AAAAAAAA*/)
{
    s4 offset = FETCH(1);               /* low-order 16 bits */
    offset |= ((s4) FETCH(2)) << 16;    /* high-order 16 bits */

    if (offset < 0)
        MY_LOG_INFO("|goto/32 -0x%08x", -offset);
    else
        MY_LOG_INFO("|goto/32 +0x%08x", offset);
    MY_LOG_INFO("> branch taken");
    if (offset <= 0)    /* allowed to branch to self */
    PERIODIC_CHECKS(offset);
    FINISH(offset);
}
OP_END
HANDLE_OPCODE(OP_PACKED_SWITCH /*vAA, +BBBB*/)
{
    const u2* switchData;
    u4 testVal;
    s4 offset;

    vsrc1 = INST_AA(inst);
    offset = FETCH(1) | (((s4) FETCH(2)) << 16);
    MY_LOG_INFO("|packed-switch v%d +0x%04x", vsrc1, offset);
    switchData = pc + offset;       // offset in 16-bit units
#ifndef NDEBUG
    if (switchData < curMethod->insns ||
        switchData >= curMethod->insns + dvmGetMethodInsnsSize(curMethod))
    {
        /* should have been caught in verifier */
        EXPORT_PC();
        dvmThrowInternalError("bad packed switch");
        GOTO_exceptionThrown();
    }
#endif
    testVal = GET_REGISTER(vsrc1);

    offset = dvmInterpHandlePackedSwitch(switchData, testVal);
        MY_LOG_INFO("> branch taken (0x%04x)", offset);
    if (offset <= 0)  /* uncommon */
    PERIODIC_CHECKS(offset);
    FINISH(offset);
}
OP_END

HANDLE_OPCODE(OP_SPARSE_SWITCH /*vAA, +BBBB*/)
{
    const u2* switchData;
    u4 testVal;
    s4 offset;

    vsrc1 = INST_AA(inst);
    offset = FETCH(1) | (((s4) FETCH(2)) << 16);
    MY_LOG_INFO("|sparse-switch v%d +0x%04x", vsrc1, offset);
    switchData = pc + offset;       // offset in 16-bit units
#ifndef NDEBUG
    if (switchData < curMethod->insns ||
        switchData >= curMethod->insns + dvmGetMethodInsnsSize(curMethod))
    {
        /* should have been caught in verifier */
        EXPORT_PC();
        dvmThrowInternalError("bad sparse switch");
        GOTO_exceptionThrown();
    }
#endif
    testVal = GET_REGISTER(vsrc1);

    offset = dvmInterpHandleSparseSwitch(switchData, testVal);
    MY_LOG_INFO("> branch taken (0x%04x)", offset);
    if (offset <= 0)  /* uncommon */
    PERIODIC_CHECKS(offset);
    FINISH(offset);
}
OP_END
HANDLE_OP_CMPX(OP_CMPL_FLOAT, "l-float", float, _FLOAT, -1)
OP_END
/* File: c/OP_CMPG_FLOAT.cpp */
HANDLE_OP_CMPX(OP_CMPG_FLOAT, "g-float", float, _FLOAT, 1)
OP_END

/* File: c/OP_CMPL_DOUBLE.cpp */
HANDLE_OP_CMPX(OP_CMPL_DOUBLE, "l-double", double, _DOUBLE, -1)
OP_END

/* File: c/OP_CMPG_DOUBLE.cpp */
HANDLE_OP_CMPX(OP_CMPG_DOUBLE, "g-double", double, _DOUBLE, 1)
OP_END

/* File: c/OP_CMP_LONG.cpp */
HANDLE_OP_CMPX(OP_CMP_LONG, "-long", s8, _WIDE, 0)
OP_END

/* File: c/OP_IF_EQ.cpp */
HANDLE_OP_IF_XX(OP_IF_EQ, "eq", ==)
OP_END

/* File: c/OP_IF_NE.cpp */
HANDLE_OP_IF_XX(OP_IF_NE, "ne", !=)
OP_END

/* File: c/OP_IF_LT.cpp */
HANDLE_OP_IF_XX(OP_IF_LT, "lt", <)
OP_END

/* File: c/OP_IF_GE.cpp */
HANDLE_OP_IF_XX(OP_IF_GE, "ge", >=)
OP_END

/* File: c/OP_IF_GT.cpp */
HANDLE_OP_IF_XX(OP_IF_GT, "gt", >)
OP_END

/* File: c/OP_IF_LE.cpp */
HANDLE_OP_IF_XX(OP_IF_LE, "le", <=)
OP_END
HANDLE_OP_IF_XXZ(OP_IF_EQZ, "eqz", ==)
OP_END

/* File: c/OP_IF_NEZ.cpp */
HANDLE_OP_IF_XXZ(OP_IF_NEZ, "nez", !=)
OP_END

/* File: c/OP_IF_LTZ.cpp */
HANDLE_OP_IF_XXZ(OP_IF_LTZ, "ltz", <)
OP_END

/* File: c/OP_IF_GEZ.cpp */
HANDLE_OP_IF_XXZ(OP_IF_GEZ, "gez", >=)
OP_END

/* File: c/OP_IF_GTZ.cpp */
HANDLE_OP_IF_XXZ(OP_IF_GTZ, "gtz", >)
OP_END

/* File: c/OP_IF_LEZ.cpp */
HANDLE_OP_IF_XXZ(OP_IF_LEZ, "lez", <=)
OP_END

HANDLE_OPCODE(OP_UNUSED_3E)
OP_END

/* File: c/OP_UNUSED_3F.cpp */
HANDLE_OPCODE(OP_UNUSED_3F)
OP_END

/* File: c/OP_UNUSED_40.cpp */
HANDLE_OPCODE(OP_UNUSED_40)
OP_END

/* File: c/OP_UNUSED_41.cpp */
HANDLE_OPCODE(OP_UNUSED_41)
OP_END

/* File: c/OP_UNUSED_42.cpp */
HANDLE_OPCODE(OP_UNUSED_42)
OP_END

/* File: c/OP_UNUSED_43.cpp */
HANDLE_OPCODE(OP_UNUSED_43)
OP_END


/* File: c/OP_AGET.cpp */
HANDLE_OP_AGET(OP_AGET, "", u4, )
OP_END

/* File: c/OP_AGET_WIDE.cpp */
HANDLE_OP_AGET(OP_AGET_WIDE, "-wide", s8, _WIDE)
OP_END

/* File: c/OP_AGET_OBJECT.cpp */
HANDLE_OP_AGET(OP_AGET_OBJECT, "-object", u4, )
OP_END

/* File: c/OP_AGET_BOOLEAN.cpp */
HANDLE_OP_AGET(OP_AGET_BOOLEAN, "-boolean", u1, )
OP_END

/* File: c/OP_AGET_BYTE.cpp */
HANDLE_OP_AGET(OP_AGET_BYTE, "-byte", s1, )
OP_END

/* File: c/OP_AGET_CHAR.cpp */
HANDLE_OP_AGET(OP_AGET_CHAR, "-char", u2, )
OP_END

/* File: c/OP_AGET_SHORT.cpp */
HANDLE_OP_AGET(OP_AGET_SHORT, "-short", s2, )
OP_END

HANDLE_OP_APUT(OP_APUT, "", u4, )
OP_END

/* File: c/OP_APUT_WIDE.cpp */
HANDLE_OP_APUT(OP_APUT_WIDE, "-wide", s8, _WIDE)
OP_END
HANDLE_OPCODE(OP_APUT_OBJECT /*vAA, vBB, vCC*/)
{
    ArrayObject* arrayObj;
    Object* obj;
    u2 arrayInfo;
    EXPORT_PC();
    vdst = INST_AA(inst);       /* AA: source value */
    arrayInfo = FETCH(1);
    vsrc1 = arrayInfo & 0xff;   /* BB: array ptr */
    vsrc2 = arrayInfo >> 8;     /* CC: index */
    MY_LOG_INFO("|aput%s v%d,v%d,v%d", "-object", vdst, vsrc1, vsrc2);
    arrayObj = (ArrayObject*) GET_REGISTER(vsrc1);
    if (!checkForNull(env,(Object*) arrayObj))
        GOTO_exceptionThrown();
    if (GET_REGISTER(vsrc2) >= arrayObj->length) {
        dvmThrowArrayIndexOutOfBoundsException(env,
                arrayObj->length, GET_REGISTER(vsrc2));
        GOTO_exceptionThrown();
    }
    obj = (Object*) GET_REGISTER(vdst);
    if (obj != NULL) {
        if (!checkForNull(env,obj))
            GOTO_exceptionThrown();
        if (!dvmCanPutArrayElementHook(obj->clazz, arrayObj->clazz)) {
            MY_LOG_INFO("Can't put a '%s'(%p) into array type='%s'(%p)",
                  obj->clazz->descriptor, obj,
                  arrayObj->clazz->descriptor, arrayObj);
            dvmThrowArrayStoreExceptionIncompatibleElement(obj->clazz, arrayObj->clazz);
            GOTO_exceptionThrown();
        }
    }
    MY_LOG_INFO("+ APUT[%d]=0x%08x", GET_REGISTER(vsrc2), GET_REGISTER(vdst));
    dvmSetObjectArrayElement(arrayObj,
                             GET_REGISTER(vsrc2),
                             (Object *)GET_REGISTER(vdst));
}
FINISH(2);
OP_END
HANDLE_OP_APUT(OP_APUT_BOOLEAN, "-boolean", u1, )
OP_END

/* File: c/OP_APUT_BYTE.cpp */
HANDLE_OP_APUT(OP_APUT_BYTE, "-byte", s1, )
OP_END

/* File: c/OP_APUT_CHAR.cpp */
HANDLE_OP_APUT(OP_APUT_CHAR, "-char", u2, )
OP_END

/* File: c/OP_APUT_SHORT.cpp */
HANDLE_OP_APUT(OP_APUT_SHORT, "-short", s2, )
OP_END
/* File: c/OP_IGET.cpp */
HANDLE_IGET_X(OP_IGET,"", Int, )
OP_END

/* File: c/OP_IGET_WIDE.cpp */
HANDLE_IGET_X(OP_IGET_WIDE,             "-wide", Long, _WIDE)
OP_END

/* File: c/OP_IGET_OBJECT.cpp */
HANDLE_IGET_X(OP_IGET_OBJECT,           "-object", Object, _AS_OBJECT)
OP_END

/* File: c/OP_IGET_BOOLEAN.cpp */
HANDLE_IGET_X(OP_IGET_BOOLEAN,          "", Int, )
OP_END

/* File: c/OP_IGET_BYTE.cpp */
HANDLE_IGET_X(OP_IGET_BYTE,             "", Int, )
OP_END

/* File: c/OP_IGET_CHAR.cpp */
HANDLE_IGET_X(OP_IGET_CHAR,             "", Int, )
OP_END

/* File: c/OP_IGET_SHORT.cpp */
HANDLE_IGET_X(OP_IGET_SHORT,            "", Int, )
OP_END

/* File: c/OP_IPUT.cpp */
HANDLE_IPUT_X(OP_IPUT,                  "", Int, )
OP_END

/* File: c/OP_IPUT_WIDE.cpp */
HANDLE_IPUT_X(OP_IPUT_WIDE,             "-wide", Long, _WIDE)
OP_END

/* File: c/OP_IPUT_OBJECT.cpp */
/*
* The VM spec says we should verify that the reference being stored into
* the field is assignment compatible.  In practice, many popular VMs don't
* do this because it slows down a very common operation.  It's not so bad
* for us, since "dexopt" quickens it whenever possible, but it's still an
* issue.
*
* To make this spec-complaint, we'd need to add a ClassObject pointer to
* the Field struct, resolve the field's type descriptor at link or class
* init time, and then verify the type here.
*/
HANDLE_IPUT_X(OP_IPUT_OBJECT,           "-object", Object, _AS_OBJECT)
OP_END

/* File: c/OP_IPUT_BOOLEAN.cpp */
HANDLE_IPUT_X(OP_IPUT_BOOLEAN,          "", Int, )
OP_END

/* File: c/OP_IPUT_BYTE.cpp */
HANDLE_IPUT_X(OP_IPUT_BYTE,             "", Int, )
OP_END

/* File: c/OP_IPUT_CHAR.cpp */
HANDLE_IPUT_X(OP_IPUT_CHAR,             "", Int, )
OP_END

/* File: c/OP_IPUT_SHORT.cpp */
HANDLE_IPUT_X(OP_IPUT_SHORT,            "", Int, )
OP_END
/* File: c/OP_SGET.cpp */
HANDLE_SGET_X(OP_SGET,                  "", Int, )
OP_END

/* File: c/OP_SGET_WIDE.cpp */
HANDLE_SGET_X(OP_SGET_WIDE,             "-wide", Long, _WIDE)
OP_END

/* File: c/OP_SGET_OBJECT.cpp */
HANDLE_SGET_X(OP_SGET_OBJECT,           "-object", Object, _AS_OBJECT)
OP_END

/* File: c/OP_SGET_BOOLEAN.cpp */
HANDLE_SGET_X(OP_SGET_BOOLEAN,          "", Int, )
OP_END

/* File: c/OP_SGET_BYTE.cpp */
HANDLE_SGET_X(OP_SGET_BYTE,             "", Int, )
OP_END

/* File: c/OP_SGET_CHAR.cpp */
HANDLE_SGET_X(OP_SGET_CHAR,             "", Int, )
OP_END

/* File: c/OP_SGET_SHORT.cpp */
HANDLE_SGET_X(OP_SGET_SHORT,            "", Int, )
OP_END
/* File: c/OP_SPUT.cpp */
HANDLE_SPUT_X(OP_SPUT,                  "", Int, )
OP_END

/* File: c/OP_SPUT_WIDE.cpp */
HANDLE_SPUT_X(OP_SPUT_WIDE,             "-wide", Long, _WIDE)
OP_END

/* File: c/OP_SPUT_OBJECT.cpp */
HANDLE_SPUT_X(OP_SPUT_OBJECT,           "-object", Object, _AS_OBJECT)
OP_END

/* File: c/OP_SPUT_BOOLEAN.cpp */
HANDLE_SPUT_X(OP_SPUT_BOOLEAN,          "", Int, )
OP_END

/* File: c/OP_SPUT_BYTE.cpp */
HANDLE_SPUT_X(OP_SPUT_BYTE,             "", Int, )
OP_END

/* File: c/OP_SPUT_CHAR.cpp */
HANDLE_SPUT_X(OP_SPUT_CHAR,             "", Int, )
OP_END

/* File: c/OP_SPUT_SHORT.cpp */
HANDLE_SPUT_X(OP_SPUT_SHORT,            "", Int, )
OP_END

HANDLE_OPCODE(OP_INVOKE_VIRTUAL)
HANDLE_OPCODE(OP_INVOKE_SUPER)
HANDLE_OPCODE(OP_INVOKE_DIRECT)
HANDLE_OPCODE(OP_INVOKE_STATIC)
HANDLE_OPCODE(OP_INVOKE_INTERFACE)
/* File: c/OP_UNUSED_73.cpp */
HANDLE_OPCODE(OP_UNUSED_73)
OP_END

/* File: c/OP_INVOKE_VIRTUAL_RANGE.cpp */
HANDLE_OPCODE(OP_INVOKE_VIRTUAL_RANGE /*{vCCCC..v(CCCC+AA-1)}, meth@BBBB*/)
GOTO_invoke(invokeVirtual, true);
OP_END

/* File: c/OP_INVOKE_SUPER_RANGE.cpp */
HANDLE_OPCODE(OP_INVOKE_SUPER_RANGE /*{vCCCC..v(CCCC+AA-1)}, meth@BBBB*/)
GOTO_invoke(invokeSuper, true);
OP_END

/* File: c/OP_INVOKE_DIRECT_RANGE.cpp */
HANDLE_OPCODE(OP_INVOKE_DIRECT_RANGE /*{vCCCC..v(CCCC+AA-1)}, meth@BBBB*/)
GOTO_invoke(invokeDirect, true);
OP_END

/* File: c/OP_INVOKE_STATIC_RANGE.cpp */
HANDLE_OPCODE(OP_INVOKE_STATIC_RANGE /*{vCCCC..v(CCCC+AA-1)}, meth@BBBB*/)
GOTO_invoke(invokeStatic, true);
OP_END

/* File: c/OP_INVOKE_INTERFACE_RANGE.cpp */
HANDLE_OPCODE(OP_INVOKE_INTERFACE_RANGE /*{vCCCC..v(CCCC+AA-1)}, meth@BBBB*/)
GOTO_invoke(invokeInterface, true);
OP_END

/* File: c/OP_UNUSED_79.cpp */
HANDLE_OPCODE(OP_UNUSED_79)
OP_END

/* File: c/OP_UNUSED_7A.cpp */
HANDLE_OPCODE(OP_UNUSED_7A)
OP_END

/* File: c/OP_NEG_INT.cpp */
HANDLE_UNOP(OP_NEG_INT, "neg-int", -, , )
OP_END

/* File: c/OP_NOT_INT.cpp */
HANDLE_UNOP(OP_NOT_INT, "not-int", , ^ 0xffffffff, )
OP_END

/* File: c/OP_NEG_LONG.cpp */
HANDLE_UNOP(OP_NEG_LONG, "neg-long", -, , _WIDE)
OP_END

/* File: c/OP_NOT_LONG.cpp */
HANDLE_UNOP(OP_NOT_LONG, "not-long", , ^ 0xffffffffffffffffULL, _WIDE)
OP_END

/* File: c/OP_NEG_FLOAT.cpp */
HANDLE_UNOP(OP_NEG_FLOAT, "neg-float", -, , _FLOAT)
OP_END

/* File: c/OP_NEG_DOUBLE.cpp */
HANDLE_UNOP(OP_NEG_DOUBLE, "neg-double", -, , _DOUBLE)
OP_END

/* File: c/OP_INT_TO_LONG.cpp */
HANDLE_NUMCONV(OP_INT_TO_LONG,          "int-to-long", _INT, _WIDE)
OP_END

/* File: c/OP_INT_TO_FLOAT.cpp */
HANDLE_NUMCONV(OP_INT_TO_FLOAT,         "int-to-float", _INT, _FLOAT)
OP_END

/* File: c/OP_INT_TO_DOUBLE.cpp */
HANDLE_NUMCONV(OP_INT_TO_DOUBLE,        "int-to-double", _INT, _DOUBLE)
OP_END

/* File: c/OP_LONG_TO_INT.cpp */
HANDLE_NUMCONV(OP_LONG_TO_INT,          "long-to-int", _WIDE, _INT)
OP_END

/* File: c/OP_LONG_TO_FLOAT.cpp */
HANDLE_NUMCONV(OP_LONG_TO_FLOAT,        "long-to-float", _WIDE, _FLOAT)
OP_END

/* File: c/OP_LONG_TO_DOUBLE.cpp */
HANDLE_NUMCONV(OP_LONG_TO_DOUBLE,       "long-to-double", _WIDE, _DOUBLE)
OP_END

/* File: c/OP_FLOAT_TO_INT.cpp */
HANDLE_FLOAT_TO_INT(OP_FLOAT_TO_INT,    "float-to-int",
                float, _FLOAT, s4, _INT)
OP_END

/* File: c/OP_FLOAT_TO_LONG.cpp */
HANDLE_FLOAT_TO_INT(OP_FLOAT_TO_LONG,   "float-to-long",
                float, _FLOAT, s8, _WIDE)
OP_END

/* File: c/OP_FLOAT_TO_DOUBLE.cpp */
HANDLE_NUMCONV(OP_FLOAT_TO_DOUBLE,      "float-to-double", _FLOAT, _DOUBLE)
OP_END

/* File: c/OP_DOUBLE_TO_INT.cpp */
HANDLE_FLOAT_TO_INT(OP_DOUBLE_TO_INT,   "double-to-int",
                double, _DOUBLE, s4, _INT)
OP_END

/* File: c/OP_DOUBLE_TO_LONG.cpp */
HANDLE_FLOAT_TO_INT(OP_DOUBLE_TO_LONG,  "double-to-long",
                double, _DOUBLE, s8, _WIDE)
OP_END

/* File: c/OP_DOUBLE_TO_FLOAT.cpp */
HANDLE_NUMCONV(OP_DOUBLE_TO_FLOAT,      "double-to-float", _DOUBLE, _FLOAT)
OP_END

/* File: c/OP_INT_TO_BYTE.cpp */
HANDLE_INT_TO_SMALL(OP_INT_TO_BYTE,     "byte", s1)
OP_END

/* File: c/OP_INT_TO_CHAR.cpp */
HANDLE_INT_TO_SMALL(OP_INT_TO_CHAR,     "char", u2)
OP_END

/* File: c/OP_INT_TO_SHORT.cpp */
HANDLE_INT_TO_SMALL(OP_INT_TO_SHORT,    "short", s2)    /* want sign bit */
OP_END

/* File: c/OP_ADD_INT.cpp */
HANDLE_OP_X_INT(OP_ADD_INT, "add", +, 0)
OP_END

/* File: c/OP_SUB_INT.cpp */
HANDLE_OP_X_INT(OP_SUB_INT, "sub", -, 0)
OP_END

/* File: c/OP_MUL_INT.cpp */
HANDLE_OP_X_INT(OP_MUL_INT, "mul", *, 0)
OP_END

/* File: c/OP_DIV_INT.cpp */
HANDLE_OP_X_INT(OP_DIV_INT, "div", /, 1)
OP_END

/* File: c/OP_REM_INT.cpp */
HANDLE_OP_X_INT(OP_REM_INT, "rem", %, 2)
OP_END

/* File: c/OP_AND_INT.cpp */
HANDLE_OP_X_INT(OP_AND_INT, "and", &, 0)
OP_END

/* File: c/OP_OR_INT.cpp */
HANDLE_OP_X_INT(OP_OR_INT,  "or",  |, 0)
OP_END

/* File: c/OP_XOR_INT.cpp */
HANDLE_OP_X_INT(OP_XOR_INT, "xor", ^, 0)
OP_END

/* File: c/OP_SHL_INT.cpp */
HANDLE_OP_SHX_INT(OP_SHL_INT, "shl", (s4), <<)
OP_END

/* File: c/OP_SHR_INT.cpp */
HANDLE_OP_SHX_INT(OP_SHR_INT, "shr", (s4), >>)
OP_END

/* File: c/OP_USHR_INT.cpp */
HANDLE_OP_SHX_INT(OP_USHR_INT, "ushr", (u4), >>)
OP_END

/* File: c/OP_ADD_LONG.cpp */
HANDLE_OP_X_LONG(OP_ADD_LONG, "add", +, 0)
OP_END

/* File: c/OP_SUB_LONG.cpp */
HANDLE_OP_X_LONG(OP_SUB_LONG, "sub", -, 0)
OP_END

/* File: c/OP_MUL_LONG.cpp */
HANDLE_OP_X_LONG(OP_MUL_LONG, "mul", *, 0)
OP_END

/* File: c/OP_DIV_LONG.cpp */
HANDLE_OP_X_LONG(OP_DIV_LONG, "div", /, 1)
OP_END

/* File: c/OP_REM_LONG.cpp */
HANDLE_OP_X_LONG(OP_REM_LONG, "rem", %, 2)
OP_END

/* File: c/OP_AND_LONG.cpp */
HANDLE_OP_X_LONG(OP_AND_LONG, "and", &, 0)
OP_END

/* File: c/OP_OR_LONG.cpp */
HANDLE_OP_X_LONG(OP_OR_LONG,  "or", |, 0)
OP_END

/* File: c/OP_XOR_LONG.cpp */
HANDLE_OP_X_LONG(OP_XOR_LONG, "xor", ^, 0)
OP_END

/* File: c/OP_SHL_LONG.cpp */
HANDLE_OP_SHX_LONG(OP_SHL_LONG, "shl", (s8), <<)
OP_END

/* File: c/OP_SHR_LONG.cpp */
HANDLE_OP_SHX_LONG(OP_SHR_LONG, "shr", (s8), >>)
OP_END

/* File: c/OP_USHR_LONG.cpp */
HANDLE_OP_SHX_LONG(OP_USHR_LONG, "ushr", (u8), >>)
OP_END

/* File: c/OP_ADD_FLOAT.cpp */
HANDLE_OP_X_FLOAT(OP_ADD_FLOAT, "add", +)
OP_END

/* File: c/OP_SUB_FLOAT.cpp */
HANDLE_OP_X_FLOAT(OP_SUB_FLOAT, "sub", -)
OP_END

/* File: c/OP_MUL_FLOAT.cpp */
HANDLE_OP_X_FLOAT(OP_MUL_FLOAT, "mul", *)
OP_END

/* File: c/OP_DIV_FLOAT.cpp */
HANDLE_OP_X_FLOAT(OP_DIV_FLOAT, "div", /)
OP_END

/* File: c/OP_REM_FLOAT.cpp */
HANDLE_OPCODE(OP_REM_FLOAT /*vAA, vBB, vCC*/)
{
    u2 srcRegs;
    vdst = INST_AA(inst);
    srcRegs = FETCH(1);
    vsrc1 = srcRegs & 0xff;
    vsrc2 = srcRegs >> 8;
    MY_LOG_INFO("|%s-float v%d,v%d,v%d", "mod", vdst, vsrc1, vsrc2);
    SET_REGISTER_FLOAT(vdst,
                       fmodf(GET_REGISTER_FLOAT(vsrc1), GET_REGISTER_FLOAT(vsrc2)));
}
FINISH(2);
OP_END

/* File: c/OP_ADD_DOUBLE.cpp */
HANDLE_OP_X_DOUBLE(OP_ADD_DOUBLE, "add", +)
OP_END

/* File: c/OP_SUB_DOUBLE.cpp */
HANDLE_OP_X_DOUBLE(OP_SUB_DOUBLE, "sub", -)
OP_END

/* File: c/OP_MUL_DOUBLE.cpp */
HANDLE_OP_X_DOUBLE(OP_MUL_DOUBLE, "mul", *)
OP_END

/* File: c/OP_DIV_DOUBLE.cpp */
HANDLE_OP_X_DOUBLE(OP_DIV_DOUBLE, "div", /)
OP_END

/* File: c/OP_REM_DOUBLE.cpp */
HANDLE_OPCODE(OP_REM_DOUBLE /*vAA, vBB, vCC*/)
{
    u2 srcRegs;
    vdst = INST_AA(inst);
    srcRegs = FETCH(1);
    vsrc1 = srcRegs & 0xff;
    vsrc2 = srcRegs >> 8;
    MY_LOG_INFO("|%s-double v%d,v%d,v%d", "mod", vdst, vsrc1, vsrc2);
    SET_REGISTER_DOUBLE(vdst,
                        fmod(GET_REGISTER_DOUBLE(vsrc1), GET_REGISTER_DOUBLE(vsrc2)));
}
FINISH(2);
OP_END

/* File: c/OP_ADD_INT_2ADDR.cpp */
HANDLE_OP_X_INT_2ADDR(OP_ADD_INT_2ADDR, "add", +, 0)
OP_END

/* File: c/OP_SUB_INT_2ADDR.cpp */
HANDLE_OP_X_INT_2ADDR(OP_SUB_INT_2ADDR, "sub", -, 0)
OP_END

/* File: c/OP_MUL_INT_2ADDR.cpp */
HANDLE_OP_X_INT_2ADDR(OP_MUL_INT_2ADDR, "mul", *, 0)
OP_END

/* File: c/OP_DIV_INT_2ADDR.cpp */
HANDLE_OP_X_INT_2ADDR(OP_DIV_INT_2ADDR, "div", /, 1)
OP_END

/* File: c/OP_REM_INT_2ADDR.cpp */
HANDLE_OP_X_INT_2ADDR(OP_REM_INT_2ADDR, "rem", %, 2)
OP_END

/* File: c/OP_AND_INT_2ADDR.cpp */
HANDLE_OP_X_INT_2ADDR(OP_AND_INT_2ADDR, "and", &, 0)
OP_END

/* File: c/OP_OR_INT_2ADDR.cpp */
HANDLE_OP_X_INT_2ADDR(OP_OR_INT_2ADDR,  "or", |, 0)
OP_END

/* File: c/OP_XOR_INT_2ADDR.cpp */
HANDLE_OP_X_INT_2ADDR(OP_XOR_INT_2ADDR, "xor", ^, 0)
OP_END

/* File: c/OP_SHL_INT_2ADDR.cpp */
HANDLE_OP_SHX_INT_2ADDR(OP_SHL_INT_2ADDR, "shl", (s4), <<)
OP_END

/* File: c/OP_SHR_INT_2ADDR.cpp */
HANDLE_OP_SHX_INT_2ADDR(OP_SHR_INT_2ADDR, "shr", (s4), >>)
OP_END

/* File: c/OP_USHR_INT_2ADDR.cpp */
HANDLE_OP_SHX_INT_2ADDR(OP_USHR_INT_2ADDR, "ushr", (u4), >>)
OP_END

/* File: c/OP_ADD_LONG_2ADDR.cpp */
HANDLE_OP_X_LONG_2ADDR(OP_ADD_LONG_2ADDR, "add", +, 0)
OP_END

/* File: c/OP_SUB_LONG_2ADDR.cpp */
HANDLE_OP_X_LONG_2ADDR(OP_SUB_LONG_2ADDR, "sub", -, 0)
OP_END

/* File: c/OP_MUL_LONG_2ADDR.cpp */
HANDLE_OP_X_LONG_2ADDR(OP_MUL_LONG_2ADDR, "mul", *, 0)
OP_END

/* File: c/OP_DIV_LONG_2ADDR.cpp */
HANDLE_OP_X_LONG_2ADDR(OP_DIV_LONG_2ADDR, "div", /, 1)
OP_END

/* File: c/OP_REM_LONG_2ADDR.cpp */
HANDLE_OP_X_LONG_2ADDR(OP_REM_LONG_2ADDR, "rem", %, 2)
OP_END

/* File: c/OP_AND_LONG_2ADDR.cpp */
HANDLE_OP_X_LONG_2ADDR(OP_AND_LONG_2ADDR, "and", &, 0)
OP_END

/* File: c/OP_OR_LONG_2ADDR.cpp */
HANDLE_OP_X_LONG_2ADDR(OP_OR_LONG_2ADDR,  "or", |, 0)
OP_END

/* File: c/OP_XOR_LONG_2ADDR.cpp */
HANDLE_OP_X_LONG_2ADDR(OP_XOR_LONG_2ADDR, "xor", ^, 0)
OP_END

/* File: c/OP_SHL_LONG_2ADDR.cpp */
HANDLE_OP_SHX_LONG_2ADDR(OP_SHL_LONG_2ADDR, "shl", (s8), <<)
OP_END

/* File: c/OP_SHR_LONG_2ADDR.cpp */
HANDLE_OP_SHX_LONG_2ADDR(OP_SHR_LONG_2ADDR, "shr", (s8), >>)
OP_END

/* File: c/OP_USHR_LONG_2ADDR.cpp */
HANDLE_OP_SHX_LONG_2ADDR(OP_USHR_LONG_2ADDR, "ushr", (u8), >>)
OP_END

/* File: c/OP_ADD_FLOAT_2ADDR.cpp */
HANDLE_OP_X_FLOAT_2ADDR(OP_ADD_FLOAT_2ADDR, "add", +)
OP_END

/* File: c/OP_SUB_FLOAT_2ADDR.cpp */
HANDLE_OP_X_FLOAT_2ADDR(OP_SUB_FLOAT_2ADDR, "sub", -)
OP_END

/* File: c/OP_MUL_FLOAT_2ADDR.cpp */
HANDLE_OP_X_FLOAT_2ADDR(OP_MUL_FLOAT_2ADDR, "mul", *)
OP_END

/* File: c/OP_DIV_FLOAT_2ADDR.cpp */
HANDLE_OP_X_FLOAT_2ADDR(OP_DIV_FLOAT_2ADDR, "div", /)
OP_END

/* File: c/OP_REM_FLOAT_2ADDR.cpp */
HANDLE_OPCODE(OP_REM_FLOAT_2ADDR /*vA, vB*/)
vdst = INST_A(inst);
vsrc1 = INST_B(inst);
MY_LOG_INFO("|%s-float-2addr v%d,v%d", "mod", vdst, vsrc1);
SET_REGISTER_FLOAT(vdst,
                   fmodf(GET_REGISTER_FLOAT(vdst), GET_REGISTER_FLOAT(vsrc1)));
FINISH(1);
OP_END

/* File: c/OP_ADD_DOUBLE_2ADDR.cpp */
HANDLE_OP_X_DOUBLE_2ADDR(OP_ADD_DOUBLE_2ADDR, "add", +)
OP_END

/* File: c/OP_SUB_DOUBLE_2ADDR.cpp */
HANDLE_OP_X_DOUBLE_2ADDR(OP_SUB_DOUBLE_2ADDR, "sub", -)
OP_END

/* File: c/OP_MUL_DOUBLE_2ADDR.cpp */
HANDLE_OP_X_DOUBLE_2ADDR(OP_MUL_DOUBLE_2ADDR, "mul", *)
OP_END

/* File: c/OP_DIV_DOUBLE_2ADDR.cpp */
HANDLE_OP_X_DOUBLE_2ADDR(OP_DIV_DOUBLE_2ADDR, "div", /)
OP_END

/* File: c/OP_REM_DOUBLE_2ADDR.cpp */
HANDLE_OPCODE(OP_REM_DOUBLE_2ADDR /*vA, vB*/)
vdst = INST_A(inst);
vsrc1 = INST_B(inst);
    MY_LOG_INFO("|%s-double-2addr v%d,v%d", "mod", vdst, vsrc1);
SET_REGISTER_DOUBLE(vdst,
                    fmod(GET_REGISTER_DOUBLE(vdst), GET_REGISTER_DOUBLE(vsrc1)));
FINISH(1);
OP_END

/* File: c/OP_ADD_INT_LIT16.cpp */
HANDLE_OP_X_INT_LIT16(OP_ADD_INT_LIT16, "add", +, 0)
OP_END

/* File: c/OP_RSUB_INT.cpp */
HANDLE_OPCODE(OP_RSUB_INT /*vA, vB, #+CCCC*/)
{
    vdst = INST_A(inst);
    vsrc1 = INST_B(inst);
    vsrc2 = FETCH(1);
    MY_LOG_INFO("|rsub-int v%d,v%d,#+0x%04x", vdst, vsrc1, vsrc2);
    SET_REGISTER(vdst, (s2) vsrc2 - (s4) GET_REGISTER(vsrc1));
}
FINISH(2);
OP_END

/* File: c/OP_MUL_INT_LIT16.cpp */
HANDLE_OP_X_INT_LIT16(OP_MUL_INT_LIT16, "mul", *, 0)
OP_END

/* File: c/OP_DIV_INT_LIT16.cpp */
HANDLE_OP_X_INT_LIT16(OP_DIV_INT_LIT16, "div", /, 1)
OP_END

/* File: c/OP_REM_INT_LIT16.cpp */
HANDLE_OP_X_INT_LIT16(OP_REM_INT_LIT16, "rem", %, 2)
OP_END

/* File: c/OP_AND_INT_LIT16.cpp */
HANDLE_OP_X_INT_LIT16(OP_AND_INT_LIT16, "and", &, 0)
OP_END

/* File: c/OP_OR_INT_LIT16.cpp */
HANDLE_OP_X_INT_LIT16(OP_OR_INT_LIT16,  "or",  |, 0)
OP_END

/* File: c/OP_XOR_INT_LIT16.cpp */
HANDLE_OP_X_INT_LIT16(OP_XOR_INT_LIT16, "xor", ^, 0)
OP_END

/* File: c/OP_ADD_INT_LIT8.cpp */
HANDLE_OP_X_INT_LIT8(OP_ADD_INT_LIT8,   "add", +, 0)
OP_END

/* File: c/OP_RSUB_INT_LIT8.cpp */
HANDLE_OPCODE(OP_RSUB_INT_LIT8 /*vAA, vBB, #+CC*/)
{
    u2 litInfo;
    vdst = INST_AA(inst);
    litInfo = FETCH(1);
    vsrc1 = litInfo & 0xff;
    vsrc2 = litInfo >> 8;
    MY_LOG_VERBOSE("|%s-int/lit8 v%d,v%d,#+0x%02x", "rsub", vdst, vsrc1, vsrc2);
    SET_REGISTER(vdst, (s1) vsrc2 - (s4) GET_REGISTER(vsrc1));
}
FINISH(2);
OP_END

/* File: c/OP_MUL_INT_LIT8.cpp */
HANDLE_OP_X_INT_LIT8(OP_MUL_INT_LIT8,   "mul", *, 0)
OP_END

/* File: c/OP_DIV_INT_LIT8.cpp */
HANDLE_OP_X_INT_LIT8(OP_DIV_INT_LIT8,   "div", /, 1)
OP_END

/* File: c/OP_REM_INT_LIT8.cpp */
HANDLE_OP_X_INT_LIT8(OP_REM_INT_LIT8,   "rem", %, 2)
OP_END

/* File: c/OP_AND_INT_LIT8.cpp */
HANDLE_OP_X_INT_LIT8(OP_AND_INT_LIT8,   "and", &, 0)
OP_END

/* File: c/OP_OR_INT_LIT8.cpp */
HANDLE_OP_X_INT_LIT8(OP_OR_INT_LIT8,    "or",  |, 0)
OP_END

/* File: c/OP_XOR_INT_LIT8.cpp */
HANDLE_OP_X_INT_LIT8(OP_XOR_INT_LIT8,   "xor", ^, 0)
OP_END

/* File: c/OP_SHL_INT_LIT8.cpp */
HANDLE_OP_SHX_INT_LIT8(OP_SHL_INT_LIT8,   "shl", (s4), <<)
OP_END

/* File: c/OP_SHR_INT_LIT8.cpp */
HANDLE_OP_SHX_INT_LIT8(OP_SHR_INT_LIT8,   "shr", (s4), >>)
OP_END

/* File: c/OP_USHR_INT_LIT8.cpp */
HANDLE_OP_SHX_INT_LIT8(OP_USHR_INT_LIT8,  "ushr", (u4), >>)

HANDLE_IGET_X(OP_IGET_VOLATILE,         "-volatile", IntVolatile, )
OP_END

/* File: c/OP_IPUT_VOLATILE.cpp */
HANDLE_IPUT_X(OP_IPUT_VOLATILE,         "-volatile", IntVolatile, )
OP_END

/* File: c/OP_SGET_VOLATILE.cpp */
HANDLE_SGET_X(OP_SGET_VOLATILE,         "-volatile", IntVolatile, )
OP_END

/* File: c/OP_SPUT_VOLATILE.cpp */
HANDLE_SPUT_X(OP_SPUT_VOLATILE,         "-volatile", IntVolatile, )
OP_END

/* File: c/OP_IGET_OBJECT_VOLATILE.cpp */
HANDLE_IGET_X(OP_IGET_OBJECT_VOLATILE,  "-object-volatile", ObjectVolatile, _AS_OBJECT)
OP_END

/* File: c/OP_IGET_WIDE_VOLATILE.cpp */
HANDLE_IGET_X(OP_IGET_WIDE_VOLATILE,    "-wide-volatile", LongVolatile, _WIDE)
OP_END

/* File: c/OP_IPUT_WIDE_VOLATILE.cpp */
HANDLE_IPUT_X(OP_IPUT_WIDE_VOLATILE,    "-wide-volatile", LongVolatile, _WIDE)
OP_END

/* File: c/OP_SGET_WIDE_VOLATILE.cpp */
HANDLE_SGET_X(OP_SGET_WIDE_VOLATILE,    "-wide-volatile", LongVolatile, _WIDE)
OP_END

/* File: c/OP_SPUT_WIDE_VOLATILE.cpp */
HANDLE_SPUT_X(OP_SPUT_WIDE_VOLATILE,    "-wide-volatile", LongVolatile, _WIDE)
OP_END
HANDLE_OPCODE(OP_BREAKPOINT)
{
    /*
     * Restart this instruction with the original opcode.  We do
     * this by simply jumping to the handler.
     *
     * It's probably not necessary to update "inst", but we do it
     * for the sake of anything that needs to do disambiguation in a
     * common handler with INST_INST.
     *
     * The breakpoint itself is handled over in updateDebugger(),
     * because we need to detect other events (method entry, single
     * step) and report them in the same event packet, and we're not
     * yet handling those through breakpoint instructions.  By the
     * time we get here, the breakpoint has already been handled and
     * the thread resumed.
     */
    u1 originalOpcode = dvmGetOriginalOpcode(pc);
    MY_LOG_INFO("+++ break 0x%02x (0x%04x -> 0x%04x)", originalOpcode, inst,
          INST_REPLACE_OP(inst, originalOpcode));
    inst = INST_REPLACE_OP(inst, originalOpcode);
    FINISH_BKPT(originalOpcode);
}
OP_END
HANDLE_OPCODE(OP_THROW_VERIFICATION_ERROR)
EXPORT_PC();
vsrc1 = INST_AA(inst);
ref = FETCH(1);             /* class/field/method ref */
dvmThrowVerificationError(curMethod, vsrc1, ref);
GOTO_exceptionThrown();
OP_END
HANDLE_OPCODE(OP_EXECUTE_INLINE /*vB, {vD, vE, vF, vG}, inline@CCCC*/)
{
    /*
     * This has the same form as other method calls, but we ignore
     * the 5th argument (vA).  This is chiefly because the first four
     * arguments to a function on ARM are in registers.
     *
     * We only set the arguments that are actually used, leaving
     * the rest uninitialized.  We're assuming that, if the method
     * needs them, they'll be specified in the call.
     *
     * However, this annoys gcc when optimizations are enabled,
     * causing a "may be used uninitialized" warning.  Quieting
     * the warnings incurs a slight penalty (5%: 373ns vs. 393ns
     * on empty method).  Note that valgrind is perfectly happy
     * either way as the uninitialiezd values are never actually
     * used.
     */
    u4 arg0, arg1, arg2, arg3;
    arg0 = arg1 = arg2 = arg3 = 0;

    EXPORT_PC();

    vsrc1 = INST_B(inst);       /* #of args */
    ref = FETCH(1);             /* inline call "ref" */
    vdst = FETCH(2);            /* 0-4 register indices */
    MY_LOG_INFO("|execute-inline args=%d @%d {regs=0x%04x}",
          vsrc1, ref, vdst);

    assert((vdst >> 16) == 0);  // 16-bit type -or- high 16 bits clear
    assert(vsrc1 <= 4);

    switch (vsrc1) {
        case 4:
            arg3 = GET_REGISTER(vdst >> 12);
            /* fall through */
        case 3:
            arg2 = GET_REGISTER((vdst & 0x0f00) >> 8);
            /* fall through */
        case 2:
            arg1 = GET_REGISTER((vdst & 0x00f0) >> 4);
            /* fall through */
        case 1:
            arg0 = GET_REGISTER(vdst & 0x0f);
            /* fall through */
        default:        // case 0
            ;
    }

    if (dvmThreadSelfHook()->interpBreak.ctl.subMode & kSubModeDebugProfile) {
        if (!dvmPerformInlineOp4Dbg(arg0, arg1, arg2, arg3, &retval, ref))
            GOTO_exceptionThrown();
    } else {
        if (!dvmPerformInlineOp4Std(arg0, arg1, arg2, arg3, &retval, ref))
            GOTO_exceptionThrown();
    }
}
FINISH(3);
OP_END
HANDLE_OPCODE(OP_EXECUTE_INLINE_RANGE)
HANDLE_OPCODE(OP_INVOKE_OBJECT_INIT_RANGE)
HANDLE_OPCODE(OP_RETURN_VOID_BARRIER /**/)
MY_LOG_INFO("|return-void");
#ifndef NDEBUG
retval.j = 0xababababULL;   /* placate valgrind */
#endif
ANDROID_MEMBAR_STORE();
GOTO_returnFromMethod();
OP_END
/* File: c/OP_IGET_QUICK.cpp */
HANDLE_IGET_X_QUICK(OP_IGET_QUICK,          "", Int, )
OP_END

/* File: c/OP_IGET_WIDE_QUICK.cpp */
HANDLE_IGET_X_QUICK(OP_IGET_WIDE_QUICK,     "-wide", Long, _WIDE)
OP_END

/* File: c/OP_IGET_OBJECT_QUICK.cpp */
HANDLE_IGET_X_QUICK(OP_IGET_OBJECT_QUICK,   "-object", Object, _AS_OBJECT)
OP_END

/* File: c/OP_IPUT_QUICK.cpp */
HANDLE_IPUT_X_QUICK(OP_IPUT_QUICK,          "", Int, )
OP_END

/* File: c/OP_IPUT_WIDE_QUICK.cpp */
HANDLE_IPUT_X_QUICK(OP_IPUT_WIDE_QUICK,     "-wide", Long, _WIDE)
OP_END

/* File: c/OP_IPUT_OBJECT_QUICK.cpp */
HANDLE_IPUT_X_QUICK(OP_IPUT_OBJECT_QUICK,   "-object", Object, _AS_OBJECT)
OP_END

HANDLE_OPCODE(OP_INVOKE_VIRTUAL_QUICK)
HANDLE_OPCODE(OP_INVOKE_VIRTUAL_QUICK_RANGE)
HANDLE_OPCODE(OP_INVOKE_SUPER_QUICK)
HANDLE_OPCODE(OP_INVOKE_SUPER_QUICK_RANGE)
/* File: c/OP_IPUT_OBJECT_VOLATILE.cpp */
HANDLE_IPUT_X(OP_IPUT_OBJECT_VOLATILE,  "-object-volatile", ObjectVolatile, _AS_OBJECT)
OP_END

/* File: c/OP_SGET_OBJECT_VOLATILE.cpp */
HANDLE_SGET_X(OP_SGET_OBJECT_VOLATILE,  "-object-volatile", ObjectVolatile, _AS_OBJECT)
OP_END

/* File: c/OP_SPUT_OBJECT_VOLATILE.cpp */
HANDLE_SPUT_X(OP_SPUT_OBJECT_VOLATILE,  "-object-volatile", ObjectVolatile, _AS_OBJECT)
OP_END

/* File: c/OP_UNUSED_FF.cpp */
HANDLE_OPCODE(OP_UNUSED_FF)
/*
 * In portable interp, most unused opcodes will fall through to here.
 */
MY_LOG_INFO("unknown opcode 0x%02x\n", INST_INST(inst));
dvmAbort();
FINISH(1);
OP_END
GOTO_TARGET(filledNewArray, bool methodCallRange, bool)
{
    ClassObject* arrayClass;
    ArrayObject* newArray;
    u4* contents;
    char typeCh;
    int i;
    u4 arg5;

    EXPORT_PC();

    ref = FETCH(1);             /* class ref */
    vdst = FETCH(2);            /* first 4 regs -or- range base */

    if (methodCallRange) {
        vsrc1 = INST_AA(inst);  /* #of elements */
        arg5 = -1;              /* silence compiler warning */
        MY_LOG_INFO("|filled-new-array-range args=%d @0x%04x {regs=v%d-v%d}",
              vsrc1, ref, vdst, vdst+vsrc1-1);
    } else {
        arg5 = INST_A(inst);
        vsrc1 = INST_B(inst);   /* #of elements */
        MY_LOG_INFO("|filled-new-array args=%d @0x%04x {regs=0x%04x %x}",
              vsrc1, ref, vdst, arg5);
    }

    /*
     * Resolve the array class.
     */
    arrayClass = dvmDexGetResolvedClass(methodClassDex, ref);
    if (arrayClass == NULL) {
        arrayClass = dvmResolveClasshook(curMethod->clazz, ref, false);
        if (arrayClass == NULL)
            GOTO_exceptionThrown();
    }
    /*
    if (!dvmIsArrayClass(arrayClass)) {
        dvmThrowRuntimeException(
            "filled-new-array needs array class");
        GOTO_exceptionThrown();
    }
    */
    /* verifier guarantees this is an array class */
    assert(dvmIsArrayClass(arrayClass));
    assert(dvmIsClassInitialized(arrayClass));

    /*
     * Create an array of the specified type.
     */
    MY_LOG_INFO("+++ filled-new-array type is '%s'", arrayClass->descriptor);
    typeCh = arrayClass->descriptor[1];
    if (typeCh == 'D' || typeCh == 'J') {
        /* category 2 primitives not allowed */
        dvmThrowRuntimeException("bad filled array req");
        GOTO_exceptionThrown();
    } else if (typeCh != 'L' && typeCh != '[' && typeCh != 'I') {
        /* TODO: requires multiple "fill in" loops with different widths */
        MY_LOG_INFO("non-int primitives not implemented");
        dvmThrowInternalError(
                "filled-new-array not implemented for anything but 'int'");
        GOTO_exceptionThrown();
    }

    newArray = dvmAllocArrayByClassHook(arrayClass, vsrc1, ALLOC_DONT_TRACK);
    if (newArray == NULL)
        GOTO_exceptionThrown();

    /*
     * Fill in the elements.  It's legal for vsrc1 to be zero.
     */
    contents = (u4*)(void*)newArray->contents;
    if (methodCallRange) {
        for (i = 0; i < vsrc1; i++)
            contents[i] = GET_REGISTER(vdst+i);
    } else {
        assert(vsrc1 <= 5);
        if (vsrc1 == 5) {
            contents[4] = GET_REGISTER(arg5);
            vsrc1--;
        }
        for (i = 0; i < vsrc1; i++) {
            contents[i] = GET_REGISTER(vdst & 0x0f);
            vdst >>= 4;
        }
    }
    if (typeCh == 'L' || typeCh == '[') {
        dvmWriteBarrierArray(newArray, 0, newArray->length);
    }

    retval.l = (Object*)newArray;
}
FINISH(3);
GOTO_TARGET_END
GOTO_TARGET(invokeVirtual, bool methodCallRange, bool)
{
    Method* baseMethod;
    Object* thisPtr;

    EXPORT_PC();

    vsrc1 = INST_AA(inst);      /* AA (count) or BA (count + arg 5) */
    ref = FETCH(1);             /* method ref */
    vdst = FETCH(2);            /* 4 regs -or- first reg */

    /*
     * The object against which we are executing a method is always
     * in the first argument.
     */
    if (methodCallRange) {
        assert(vsrc1 > 0);
        MY_LOG_INFO("|invoke-virtual-range args=%d @0x%04x {regs=v%d-v%d}",
              vsrc1, ref, vdst, vdst+vsrc1-1);
        thisPtr = (Object*) GET_REGISTER(vdst);
    } else {
        assert((vsrc1>>4) > 0);
        MY_LOG_INFO("|invoke-virtual args=%d @0x%04x {regs=0x%04x %x}",
              vsrc1 >> 4, ref, vdst, vsrc1 & 0x0f);
        thisPtr = (Object*) GET_REGISTER(vdst & 0x0f);
    }

    if (!checkForNull(env,thisPtr))
        GOTO_exceptionThrown();

    /*
     * Resolve the method.  This is the correct method for the static
     * type of the object.  We also verify access permissions here.
     */
    baseMethod = dvmDexGetResolvedMethod(methodClassDex, ref);
    if (baseMethod == NULL) {
        baseMethod = dvmResolveMethodhook(curMethod->clazz, ref,METHOD_VIRTUAL);
        if (baseMethod == NULL) {
            MY_LOG_INFO("+ unknown method or access denied");
            GOTO_exceptionThrown();
        }
    }

    /*
     * Combine the object we found with the vtable offset in the
     * method.
     */
    assert(baseMethod->methodIndex < thisPtr->clazz->vtableCount);
    methodToCall = thisPtr->clazz->vtable[baseMethod->methodIndex];

#if defined(WITH_JIT) && defined(MTERP_STUB)
    self->methodToCall = methodToCall;
    self->callsiteClass = thisPtr->clazz;
#endif


    assert(!dvmIsAbstractMethod(methodToCall) ||
           methodToCall->nativeFunc != NULL);

        MY_LOG_INFO("+++ base=%s.%s virtual[%d]=%s.%s",
          baseMethod->clazz->descriptor, baseMethod->name,
          (u4) baseMethod->methodIndex,
          methodToCall->clazz->descriptor, methodToCall->name);
    assert(methodToCall != NULL);

    GOTO_invokeMethod(methodCallRange, methodToCall, vsrc1, vdst);
}
GOTO_TARGET_END
GOTO_TARGET(invokeMethod, bool methodCallRange, const Method* _methodToCall,
            u2 count, u2 regs)
{
    STUB_HACK(vsrc1 = count; vdst = regs; methodToCall = _methodToCall;);

    //printf("range=%d call=%p count=%d regs=0x%04x\n",
    //    methodCallRange, methodToCall, count, regs);
    //printf(" --> %s.%s %s\n", methodToCall->clazz->descriptor,
    //    methodToCall->name, methodToCall->shorty);

    u4* outs;
    int i;

    /*
     * Copy args.  This may corrupt vsrc1/vdst.
     */
    if (methodCallRange) {
        // could use memcpy or a "Duff's device"; most functions have
        // so few args it won't matter much
        assert(vsrc1 <= curMethod->outsSize);
        assert(vsrc1 == methodToCall->insSize);
        outs = OUTS_FROM_FP(fp, vsrc1);
        for (i = 0; i < vsrc1; i++)
            outs[i] = GET_REGISTER(vdst+i);
    } else {
        u4 count = vsrc1 >> 4;

        assert(count <= curMethod->outsSize);
        assert(count == methodToCall->insSize);
        assert(count <= 5);

        outs = OUTS_FROM_FP(fp, count);
#if 0
        if (count == 5) {
            outs[4] = GET_REGISTER(vsrc1 & 0x0f);
            count--;
        }
        for (i = 0; i < (int) count; i++) {
            outs[i] = GET_REGISTER(vdst & 0x0f);
            vdst >>= 4;
        }
#else
        // This version executes fewer instructions but is larger
        // overall.  Seems to be a teensy bit faster.
        assert((vdst >> 16) == 0);  // 16 bits -or- high 16 bits clear
        switch (count) {
            case 5:
                outs[4] = GET_REGISTER(vsrc1 & 0x0f);
            case 4:
                outs[3] = GET_REGISTER(vdst >> 12);
            case 3:
                outs[2] = GET_REGISTER((vdst & 0x0f00) >> 8);
            case 2:
                outs[1] = GET_REGISTER((vdst & 0x00f0) >> 4);
            case 1:
                outs[0] = GET_REGISTER(vdst & 0x0f);
            default:
                ;
        }
#endif
    }
}

/*
 * (This was originally a "goto" target; I've kept it separate from the
 * stuff above in case we want to refactor things again.)
 *
 * At this point, we have the arguments stored in the "outs" area of
 * the current method's stack frame, and the method to call in
 * "methodToCall".  Push a new stack frame.
 */
{
    StackSaveArea* newSaveArea;
    u4* newFp;

    MY_LOG_INFO("> %s%s.%s %s",
          dvmIsNativeMethod(methodToCall) ? "(NATIVE) " : "",
          methodToCall->clazz->descriptor, methodToCall->name,
          methodToCall->shorty);

    newFp = (u4*) SAVEAREA_FROM_FP(fp) - methodToCall->registersSize;
    newSaveArea = SAVEAREA_FROM_FP(newFp);

    /* verify that we have enough space */
    if (true) {
        u1* bottom;
        bottom = (u1*) newSaveArea - methodToCall->outsSize * sizeof(u4);
        if (bottom < self->interpStackEnd) {
            /* stack overflow */
            MY_LOG_INFO("Stack overflow on method call (start=%p end=%p newBot=%p(%d) size=%d '%s')",
                  self->interpStackStart, self->interpStackEnd, bottom,
                  (u1*) fp - bottom, self->interpStackSize,
                  methodToCall->name);
            dvmHandleStackOverflow(self, methodToCall);
            assert(dvmCheckException(self));
            GOTO_exceptionThrown();
        }
        //ALOGD("+++ fp=%p newFp=%p newSave=%p bottom=%p",
        //    fp, newFp, newSaveArea, bottom);
    }

#ifdef LOG_INSTR
    if (methodToCall->registersSize > methodToCall->insSize) {
        /*
         * This makes valgrind quiet when we print registers that
         * haven't been initialized.  Turn it off when the debug
         * messages are disabled -- we want valgrind to report any
         * used-before-initialized issues.
         */
        memset(newFp, 0xcc,
            (methodToCall->registersSize - methodToCall->insSize) * 4);
    }
#endif

#ifdef EASY_GDB
    newSaveArea->prevSave = SAVEAREA_FROM_FP(fp);
#endif
    newSaveArea->prevFrame = fp;
    newSaveArea->savedPc = pc;
#if defined(WITH_JIT) && defined(MTERP_STUB)
    newSaveArea->returnAddr = 0;
#endif
    newSaveArea->method = methodToCall;

    if (self->interpBreak.ctl.subMode != 0) {
        /*
         * We mark ENTER here for both native and non-native
         * calls.  For native calls, we'll mark EXIT on return.
         * For non-native calls, EXIT is marked in the RETURN op.
         */
        PC_TO_SELF();
        dvmReportInvoke(self, methodToCall);
    }

    if (!dvmIsNativeMethod(methodToCall)) {
        /*
         * "Call" interpreted code.  Reposition the PC, update the
         * frame pointer and other local state, and continue.
         */
        curMethod = methodToCall;
        self->interpSave.method = curMethod;
        methodClassDex = curMethod->clazz->pDvmDex;
        pc = methodToCall->insns;
        fp = newFp;
        self->interpSave.curFrame = fp;
#ifdef EASY_GDB
        debugSaveArea = SAVEAREA_FROM_FP(newFp);
#endif
        self->debugIsMethodEntry = true;        // profiling, debugging
        MY_LOG_INFO("> pc <-- %s.%s %s", curMethod->clazz->descriptor,
              curMethod->name, curMethod->shorty);
//        DUMP_REGS(curMethod, fp, true);         // show input args
        FINISH(0);                              // jump to method start
    } else {
        /* set this up for JNI locals, even if not a JNI native */
        newSaveArea->xtra.localRefCookie = self->jniLocalRefTable.segmentState.all;

        self->interpSave.curFrame = newFp;

//        DUMP_REGS(methodToCall, newFp, true);   // show input args

        if (self->interpBreak.ctl.subMode != 0) {
            dvmReportPreNativeInvoke(methodToCall, self, newSaveArea->prevFrame);
        }

        MY_LOG_INFO("> native <-- %s.%s %s", methodToCall->clazz->descriptor,
              methodToCall->name, methodToCall->shorty);

        /*
         * Jump through native call bridge.  Because we leave no
         * space for locals on native calls, "newFp" points directly
         * to the method arguments.
         */
        (*methodToCall->nativeFunc)(newFp, &retval, methodToCall, self);

        if (dvmThreadSelfHook()->interpBreak.ctl.subMode != 0) {
            dvmReportPostNativeInvoke(methodToCall, self, newSaveArea->prevFrame);
        }

        /* pop frame off */
        dvmPopJniLocals(dvmThreadSelfHook(), newSaveArea);
        dvmThreadSelfHook()->interpSave.curFrame = newSaveArea->prevFrame;
        fp = newSaveArea->prevFrame;

        /*
         * If the native code threw an exception, or interpreted code
         * invoked by the native call threw one and nobody has cleared
         * it, jump to our local exception handling.
         */
        if (dvmCheckException(dvmThreadSelfHook())) {
            MY_LOG_INFO("Exception thrown by/below native code");
            GOTO_exceptionThrown();
        }

        MY_LOG_INFO("> retval=0x%llx (leaving native)", retval.j);
        MY_LOG_INFO("> (return from native %s.%s to %s.%s %s)",
              methodToCall->clazz->descriptor, methodToCall->name,
              curMethod->clazz->descriptor, curMethod->name,
              curMethod->shorty);

        //u2 invokeInstr = INST_INST(FETCH(0));
        if (true /*invokeInstr >= OP_INVOKE_VIRTUAL &&
            invokeInstr <= OP_INVOKE_INTERFACE*/)
        {
            FINISH(3);
        } else {
            //ALOGE("Unknown invoke instr %02x at %d",
            //    invokeInstr, (int) (pc - curMethod->insns));
            assert(false);
        }
    }
}
assert(false);      // should not get here
GOTO_TARGET_END

// TODO 异常现在不支持。
    /*
     * Jump here when the code throws an exception.
     *
     * By the time we get here, the Throwable has been created and the stack
     * trace has been saved off.
     */
GOTO_TARGET(exceptionThrown)
GOTO_TARGET_END

bail:
    if (NULL != params) {
        delete[] params;
    }
    MY_LOG_INFO("|-- Leaving interpreter loop");
    return retval;
}
