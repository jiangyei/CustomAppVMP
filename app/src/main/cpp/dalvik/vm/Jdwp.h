//
// Created by liu meng on 2018/8/20.
//

#ifndef CUSTOMAPPVMP_JDWP_H
#define CUSTOMAPPVMP_JDWP_H
typedef u8 ObjectId;
enum JdwpTransportType {
    kJdwpTransportUnknown = 0,
    kJdwpTransportSocket,       /* transport=dt_socket */
    kJdwpTransportAndroidAdb,   /* transport=dt_android_adb */
};
enum JdwpError {
    ERR_NONE                                        = 0,
    ERR_INVALID_THREAD                              = 10,
    ERR_INVALID_THREAD_GROUP                        = 11,
    ERR_INVALID_PRIORITY                            = 12,
    ERR_THREAD_NOT_SUSPENDED                        = 13,
    ERR_THREAD_SUSPENDED                            = 14,
    ERR_INVALID_OBJECT                              = 20,
    ERR_INVALID_CLASS                               = 21,
    ERR_CLASS_NOT_PREPARED                          = 22,
    ERR_INVALID_METHODID                            = 23,
    ERR_INVALID_LOCATION                            = 24,
    ERR_INVALID_FIELDID                             = 25,
    ERR_INVALID_FRAMEID                             = 30,
    ERR_NO_MORE_FRAMES                              = 31,
    ERR_OPAQUE_FRAME                                = 32,
    ERR_NOT_CURRENT_FRAME                           = 33,
    ERR_TYPE_MISMATCH                               = 34,
    ERR_INVALID_SLOT                                = 35,
    ERR_DUPLICATE                                   = 40,
    ERR_NOT_FOUND                                   = 41,
    ERR_INVALID_MONITOR                             = 50,
    ERR_NOT_MONITOR_OWNER                           = 51,
    ERR_INTERRUPT                                   = 52,
    ERR_INVALID_CLASS_FORMAT                        = 60,
    ERR_CIRCULAR_CLASS_DEFINITION                   = 61,
    ERR_FAILS_VERIFICATION                          = 62,
    ERR_ADD_METHOD_NOT_IMPLEMENTED                  = 63,
    ERR_SCHEMA_CHANGE_NOT_IMPLEMENTED               = 64,
    ERR_INVALID_TYPESTATE                           = 65,
    ERR_HIERARCHY_CHANGE_NOT_IMPLEMENTED            = 66,
    ERR_DELETE_METHOD_NOT_IMPLEMENTED               = 67,
    ERR_UNSUPPORTED_VERSION                         = 68,
    ERR_NAMES_DONT_MATCH                            = 69,
    ERR_CLASS_MODIFIERS_CHANGE_NOT_IMPLEMENTED      = 70,
    ERR_METHOD_MODIFIERS_CHANGE_NOT_IMPLEMENTED     = 71,
    ERR_NOT_IMPLEMENTED                             = 99,
    ERR_NULL_POINTER                                = 100,
    ERR_ABSENT_INFORMATION                          = 101,
    ERR_INVALID_EVENT_TYPE                          = 102,
    ERR_ILLEGAL_ARGUMENT                            = 103,
    ERR_OUT_OF_MEMORY                               = 110,
    ERR_ACCESS_DENIED                               = 111,
    ERR_VM_DEAD                                     = 112,
    ERR_INTERNAL                                    = 113,
    ERR_UNATTACHED_THREAD                           = 115,
    ERR_INVALID_TAG                                 = 500,
    ERR_ALREADY_INVOKING                            = 502,
    ERR_INVALID_INDEX                               = 503,
    ERR_INVALID_LENGTH                              = 504,
    ERR_INVALID_STRING                              = 506,
    ERR_INVALID_CLASS_LOADER                        = 507,
    ERR_INVALID_ARRAY                               = 508,
    ERR_TRANSPORT_LOAD                              = 509,
    ERR_TRANSPORT_INIT                              = 510,
    ERR_NATIVE_METHOD                               = 511,
    ERR_INVALID_COUNT                               = 512,
};
#endif //CUSTOMAPPVMP_JDWP_H
