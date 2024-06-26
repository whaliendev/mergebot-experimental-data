#define LOG_TAG "JniConstants"
#include "ALog-priv.h"
#include "JniConstants.h"
#include "ScopedLocalRef.h"
#include <stdlib.h>
jclass JniConstants::bigDecimalClass;
jclass JniConstants::booleanClass;
jclass JniConstants::byteArrayClass;
jclass JniConstants::byteClass;
jclass JniConstants::calendarClass;
jclass JniConstants::characterClass;
jclass JniConstants::charsetICUClass;
jclass JniConstants::constructorClass;
jclass JniConstants::deflaterClass;
jclass JniConstants::doubleClass;
jclass JniConstants::errnoExceptionClass;
jclass JniConstants::fieldClass;
jclass JniConstants::fileDescriptorClass;
jclass JniConstants::floatClass;
jclass JniConstants::gaiExceptionClass;
jclass JniConstants::inet6AddressClass;
jclass JniConstants::inetAddressClass;
jclass JniConstants::inetAddressHolderClass;
jclass JniConstants::inetSocketAddressClass;
jclass JniConstants::inetSocketAddressHolderClass;
jclass JniConstants::inflaterClass;
jclass JniConstants::inputStreamClass;
jclass JniConstants::integerClass;
jclass JniConstants::localeDataClass;
jclass JniConstants::longClass;
jclass JniConstants::methodClass;
jclass JniConstants::mutableIntClass;
jclass JniConstants::mutableLongClass;
jclass JniConstants::netlinkSocketAddressClass;
jclass JniConstants::objectClass;
jclass JniConstants::objectArrayClass;
jclass JniConstants::outputStreamClass;
jclass JniConstants::packetSocketAddressClass;
jclass JniConstants::parsePositionClass;
jclass JniConstants::patternSyntaxExceptionClass;
jclass JniConstants::referenceClass;
jclass JniConstants::shortClass;
jclass JniConstants::socketClass;
jclass JniConstants::socketImplClass;
jclass JniConstants::stringClass;
jclass JniConstants::structAddrinfoClass;
jclass JniConstants::structFlockClass;
jclass JniConstants::structGroupReqClass;
jclass JniConstants::structGroupSourceReqClass;
jclass JniConstants::structLingerClass;
jclass JniConstants::structPasswdClass;
jclass JniConstants::structPollfdClass;
jclass JniConstants::structStatClass;
jclass JniConstants::structStatVfsClass;
jclass JniConstants::structTimevalClass;
jclass JniConstants::structUcredClass;
jclass JniConstants::structUtsnameClass;
jclass JniConstants::unixSocketAddressClass;
jclass JniConstants::zipEntryClass;
static jclass findClass(JNIEnv* env, const char* name) {
  ScopedLocalRef<jclass> localClass(env, env->FindClass(name));
  jclass result = reinterpret_cast<jclass>(env->NewGlobalRef(localClass.get()));
  if (result == NULL) {
    ALOGE("failed to find class '%s'", name);
    abort();
  }
  return result;
}
void JniConstants::init(JNIEnv* env) {
  bigDecimalClass = findClass(env, "java/math/BigDecimal");
  booleanClass = findClass(env, "java/lang/Boolean");
  byteClass = findClass(env, "java/lang/Byte");
  byteArrayClass = findClass(env, "[B");
  calendarClass = findClass(env, "java/util/Calendar");
  characterClass = findClass(env, "java/lang/Character");
  charsetICUClass = findClass(env, "java/nio/charset/CharsetICU");
  constructorClass = findClass(env, "java/lang/reflect/Constructor");
  floatClass = findClass(env, "java/lang/Float");
  deflaterClass = findClass(env, "java/util/zip/Deflater");
  doubleClass = findClass(env, "java/lang/Double");
  errnoExceptionClass = findClass(env, "android/system/ErrnoException");
  fieldClass = findClass(env, "java/lang/reflect/Field");
  fileDescriptorClass = findClass(env, "java/io/FileDescriptor");
  gaiExceptionClass = findClass(env, "android/system/GaiException");
  inet6AddressClass = findClass(env, "java/net/Inet6Address");
  inetAddressClass = findClass(env, "java/net/InetAddress");
  inetAddressHolderClass =
      findClass(env, "java/net/InetAddress$InetAddressHolder");
  inetSocketAddressClass = findClass(env, "java/net/InetSocketAddress");
<<<<<<< /tmp/.mergebot/47060b8d762c346617290efed906fb91073cf64c/dc8d6a-800dca/ours/JniConstants.cpp
||||||| /tmp/.mergebot/47060b8d762c346617290efed906fb91073cf64c/dc8d6a-800dca/base/JniConstants.cpp
  inetUnixAddressClass = findClass(env, "java/net/InetUnixAddress");
=======
  inetSocketAddressHolderClass =
      findClass(env, "java/net/InetSocketAddress$InetSocketAddressHolder");
  inetUnixAddressClass = findClass(env, "java/net/InetUnixAddress");
>>>>>>> /tmp/.mergebot/47060b8d762c346617290efed906fb91073cf64c/dc8d6a-800dca/theirs/JniConstants.cpp
  inflaterClass = findClass(env, "java/util/zip/Inflater");
  inputStreamClass = findClass(env, "java/io/InputStream");
  integerClass = findClass(env, "java/lang/Integer");
  localeDataClass = findClass(env, "libcore/icu/LocaleData");
  longClass = findClass(env, "java/lang/Long");
  methodClass = findClass(env, "java/lang/reflect/Method");
  mutableIntClass = findClass(env, "android/util/MutableInt");
  mutableLongClass = findClass(env, "android/util/MutableLong");
  netlinkSocketAddressClass =
      findClass(env, "android/system/NetlinkSocketAddress");
  objectClass = findClass(env, "java/lang/Object");
  objectArrayClass = findClass(env, "[Ljava/lang/Object;");
  outputStreamClass = findClass(env, "java/io/OutputStream");
  packetSocketAddressClass =
      findClass(env, "android/system/PacketSocketAddress");
  parsePositionClass = findClass(env, "java/text/ParsePosition");
  patternSyntaxExceptionClass =
      findClass(env, "java/util/regex/PatternSyntaxException");
  referenceClass = findClass(env, "java/lang/ref/Reference");
  shortClass = findClass(env, "java/lang/Short");
  socketClass = findClass(env, "java/net/Socket");
  socketImplClass = findClass(env, "java/net/SocketImpl");
  stringClass = findClass(env, "java/lang/String");
  structAddrinfoClass = findClass(env, "android/system/StructAddrinfo");
  structFlockClass = findClass(env, "android/system/StructFlock");
  structGroupReqClass = findClass(env, "android/system/StructGroupReq");
  structGroupSourceReqClass =
      findClass(env, "android/system/StructGroupSourceReq");
  structLingerClass = findClass(env, "android/system/StructLinger");
  structPasswdClass = findClass(env, "android/system/StructPasswd");
  structPollfdClass = findClass(env, "android/system/StructPollfd");
  structStatClass = findClass(env, "android/system/StructStat");
  structStatVfsClass = findClass(env, "android/system/StructStatVfs");
  structTimevalClass = findClass(env, "android/system/StructTimeval");
  structUcredClass = findClass(env, "android/system/StructUcred");
  structUtsnameClass = findClass(env, "android/system/StructUtsname");
  unixSocketAddressClass = findClass(env, "android/system/UnixSocketAddress");
  zipEntryClass = findClass(env, "java/util/zip/ZipEntry");
}
