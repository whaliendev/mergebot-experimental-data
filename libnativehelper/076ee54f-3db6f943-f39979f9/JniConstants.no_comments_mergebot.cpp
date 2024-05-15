#define LOG_TAG "JniConstants"
#include "ALog-priv.h"
#include "JniConstants.h"
#include "ScopedLocalRef.h"
#include <stdlib.h>
#include <atomic>
#include <mutex>
jclass JniConstants::booleanClass;
jclass JniConstants::byteArrayClass;
jclass JniConstants::calendarClass;
jclass JniConstants::charsetICUClass;
jclass JniConstants::doubleClass;
jclass JniConstants::errnoExceptionClass;
jclass JniConstants::fileDescriptorClass;
jclass JniConstants::gaiExceptionClass;
jclass JniConstants::inet6AddressClass;
jclass JniConstants::inetAddressClass;
jclass JniConstants::inetAddressHolderClass;
jclass JniConstants::inetSocketAddressClass;
jclass JniConstants::inetSocketAddressHolderClass;
jclass JniConstants::integerClass;
jclass JniConstants::localeDataClass;
jclass JniConstants::longClass;
jclass JniConstants::mutableIntClass;
jclass JniConstants::mutableLongClass;
jclass JniConstants::netlinkSocketAddressClass;
jclass JniConstants::packetSocketAddressClass;
jclass JniConstants::patternSyntaxExceptionClass;
jclass JniConstants::referenceClass;
jclass JniConstants::socketTaggerClass;
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
<<<<<<< /tmp/.mergebot/47060b8d762c346617290efed906fb91073cf64c/076ee5-3db6f9/ours/JniConstants.cpp
  if (g_constants_initialized) {
    return;
  }
  std::lock_guard<std::mutex> guard(g_constants_mutex);
  if (g_constants_initialized) {
    return;
  }
  bigDecimalClass = findClass(env, "java/math/BigDecimal");
||||||| /tmp/.mergebot/47060b8d762c346617290efed906fb91073cf64c/076ee5-3db6f9/base/JniConstants.cpp
  bigDecimalClass = findClass(env, "java/math/BigDecimal");
=======
>>>>>>> /tmp/.mergebot/47060b8d762c346617290efed906fb91073cf64c/076ee5-3db6f9/theirs/JniConstants.cpp
  booleanClass = findClass(env, "java/lang/Boolean");
  byteArrayClass = findClass(env, "[B");
  calendarClass = findClass(env, "java/util/Calendar");
  charsetICUClass = findClass(env, "java/nio/charset/CharsetICU");
  doubleClass = findClass(env, "java/lang/Double");
  errnoExceptionClass = findClass(env, "android/system/ErrnoException");
  fileDescriptorClass = findClass(env, "java/io/FileDescriptor");
  gaiExceptionClass = findClass(env, "android/system/GaiException");
  inet6AddressClass = findClass(env, "java/net/Inet6Address");
  inetAddressClass = findClass(env, "java/net/InetAddress");
  inetAddressHolderClass =
      findClass(env, "java/net/InetAddress$InetAddressHolder");
  inetSocketAddressClass = findClass(env, "java/net/InetSocketAddress");
  inetSocketAddressHolderClass =
      findClass(env, "java/net/InetSocketAddress$InetSocketAddressHolder");
  integerClass = findClass(env, "java/lang/Integer");
  localeDataClass = findClass(env, "libcore/icu/LocaleData");
  longClass = findClass(env, "java/lang/Long");
  mutableIntClass = findClass(env, "android/util/MutableInt");
  mutableLongClass = findClass(env, "android/util/MutableLong");
  netlinkSocketAddressClass =
      findClass(env, "android/system/NetlinkSocketAddress");
  packetSocketAddressClass =
      findClass(env, "android/system/PacketSocketAddress");
  patternSyntaxExceptionClass =
      findClass(env, "java/util/regex/PatternSyntaxException");
  referenceClass = findClass(env, "java/lang/ref/Reference");
  socketTaggerClass = findClass(env, "dalvik/system/SocketTagger");
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
  g_constants_initialized = true;
}
static std::atomic<bool> g_constants_initialized(false);
static std::mutex g_constants_mutex;
