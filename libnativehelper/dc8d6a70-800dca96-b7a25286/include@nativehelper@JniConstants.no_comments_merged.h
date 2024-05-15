#ifndef JNI_CONSTANTS_H_included
#define JNI_CONSTANTS_H_included 
#include "JNIHelp.h"
struct JniConstants {
    static void init(JNIEnv* env);
    static jclass bigDecimalClass;
    static jclass booleanClass;
    static jclass byteArrayClass;
    static jclass byteClass;
    static jclass calendarClass;
    static jclass characterClass;
    static jclass charsetICUClass;
    static jclass constructorClass;
    static jclass deflaterClass;
    static jclass doubleClass;
    static jclass errnoExceptionClass;
    static jclass fieldClass;
    static jclass fileDescriptorClass;
    static jclass floatClass;
    static jclass gaiExceptionClass;
    static jclass inet6AddressClass;
    static jclass inetAddressClass;
    static jclass inetAddressHolderClass;
    static jclass inetSocketAddressClass;
    static jclass inetSocketAddressHolderClass;
    static jclass inflaterClass;
    static jclass inputStreamClass;
    static jclass integerClass;
    static jclass localeDataClass;
    static jclass longClass;
    static jclass methodClass;
    static jclass mutableIntClass;
    static jclass mutableLongClass;
    static jclass netlinkSocketAddressClass;
    static jclass objectClass;
    static jclass objectArrayClass;
    static jclass outputStreamClass;
    static jclass packetSocketAddressClass;
    static jclass parsePositionClass;
    static jclass patternSyntaxExceptionClass;
    static jclass referenceClass;
    static jclass shortClass;
    static jclass socketClass;
    static jclass socketImplClass;
    static jclass stringClass;
    static jclass structAddrinfoClass;
    static jclass structFlockClass;
    static jclass structGroupReqClass;
    static jclass structGroupSourceReqClass;
    static jclass structLingerClass;
    static jclass structPasswdClass;
    static jclass structPollfdClass;
    static jclass structStatClass;
    static jclass structStatVfsClass;
    static jclass structTimevalClass;
    static jclass structUcredClass;
    static jclass structUtsnameClass;
    static jclass unixSocketAddressClass;
    static jclass zipEntryClass;
};
#define NATIVE_METHOD(className,functionName,signature) \
    { #functionName, signature, reinterpret_cast<void*>(className ## _ ## functionName) }
#endif
