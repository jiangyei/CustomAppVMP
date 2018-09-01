/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Exception handling.
 */
#ifndef DALVIK_EXCEPTION_H_
#define DALVIK_EXCEPTION_H_

#include "Thread.h"

typedef int (*dvmFindCatchBlock_func)(Thread* self, int relPc, Object* exception,
                                       bool scanOnly, void** newFrame);
dvmFindCatchBlock_func dvmFindCatchBlockHook;
bool initExceptionFuction(void *dvm_hand,int apilevel);


void dvmThrowNullPointerException(JNIEnv* env, const char* msg);

void dvmThrowArrayIndexOutOfBoundsException(JNIEnv* env, int length, int index);

void dvmThrowArithmeticException(JNIEnv* env, const char* msg);
INLINE bool dvmCheckException(Thread* self) {
    return (self->exception != NULL);
}
void dvmThrowClassCastException(ClassObject* actual, ClassObject* desired)
{

}
void dvmThrowNegativeArraySizeException(s4 size) {
//    dvmThrowExceptionFmt(gDvm.exNegativeArraySizeException, "%d", size);
}
void dvmThrowRuntimeException(const char* msg);
void dvmThrowInternalError(const char* msg);
INLINE void dvmSetException(Thread* self, Object* exception)
{
    assert(exception != NULL);
    self->exception = exception;
}
INLINE Object* dvmGetException(Thread* self) {
    return self->exception;
}
INLINE void dvmClearException(Thread* self) {
    self->exception = NULL;
}
void dvmThrowNoSuchMethodError(const char* msg) {
//    dvmThrowException(gDvm.exNoSuchMethodError, msg);
}

void dvmThrowArrayStoreExceptionIncompatibleElement(ClassObject* objectType,
                                                    ClassObject* arrayType)
{
//    throwTypeError(gDvm.exArrayStoreException,
//                   "%s cannot be stored in an array of type %s",
//                   objectType, arrayType);
}

#endif  // DALVIK_EXCEPTION_H_
