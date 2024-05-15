#define LOG_TAG "JNIHelp"
#define LIBCORE_CPP_JNI_HELPERS 
#include "JniConstants.h"
#include "JNIHelp.h"
#include "cutils/log.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <string>
template<typename T>
class scoped_local_ref {
public:
    scoped_local_ref(C_JNIEnv* env, T localRef = NULL)
    : mEnv(env), mLocalRef(localRef)
    {
    }
    ~scoped_local_ref() {
        reset();
    }
    void reset(T localRef = NULL) {
        if (mLocalRef != NULL) {
            (*mEnv)->DeleteLocalRef(reinterpret_cast<JNIEnv*>(mEnv), mLocalRef);
            mLocalRef = localRef;
        }
    }
    T get() const {
        return mLocalRef;
    }
private:
    C_JNIEnv* mEnv;
    T mLocalRef;
    scoped_local_ref(const scoped_local_ref&);
    void operator=(const scoped_local_ref&);
};
static jclass findClass(C_JNIEnv* env, const char* className) {
    JNIEnv* e = reinterpret_cast<JNIEnv*>(env);
    return (*env)->FindClass(e, className);
}
extern "C" int jniRegisterNativeMethods(C_JNIEnv* env, const char* className,
    const JNINativeMethod* gMethods, int numMethods)
{
    JNIEnv* e = reinterpret_cast<JNIEnv*>(env);
    ALOGV("Registering %s's %d native methods...", className, numMethods);
    scoped_local_ref<jclass> c(env, findClass(env, className));
    if (c.get() == NULL) {
        char* msg;
        asprintf(&msg, "Native registration unable to find class '%s', aborting...", className);
        e->FatalError(msg);
    }
    if ((*env)->RegisterNatives(e, c.get(), gMethods, numMethods) < 0) {
        char* msg;
        asprintf(&msg, "RegisterNatives failed for '%s', aborting...", className);
        e->FatalError(msg);
    }
    return 0;
}
static bool getExceptionSummary(C_JNIEnv* env, jthrowable exception, std::string& result) {
    JNIEnv* e = reinterpret_cast<JNIEnv*>(env);
    scoped_local_ref<jclass> exceptionClass(env, (*env)->GetObjectClass(e, exception));
    scoped_local_ref<jclass> classClass(env,
            (*env)->GetObjectClass(e, exceptionClass.get()));
    jmethodID classGetNameMethod =
            (*env)->GetMethodID(e, classClass.get(), "getName", "()Ljava/lang/String;");
    scoped_local_ref<jstring> classNameStr(env,
            (jstring) (*env)->CallObjectMethod(e, exceptionClass.get(), classGetNameMethod));
    if (classNameStr.get() == NULL) {
        (*env)->ExceptionClear(e);
        result = "<error getting class name>";
        return false;
    }
    const char* classNameChars = (*env)->GetStringUTFChars(e, classNameStr.get(), NULL);
    if (classNameChars == NULL) {
        (*env)->ExceptionClear(e);
        result = "<error getting class name UTF-8>";
        return false;
    }
    result += classNameChars;
    (*env)->ReleaseStringUTFChars(e, classNameStr.get(), classNameChars);
    jmethodID getMessage =
            (*env)->GetMethodID(e, exceptionClass.get(), "getMessage", "()Ljava/lang/String;");
    scoped_local_ref<jstring> messageStr(env,
            (jstring) (*env)->CallObjectMethod(e, exception, getMessage));
    if (messageStr.get() == NULL) {
        return true;
    }
    result += ": ";
    const char* messageChars = (*env)->GetStringUTFChars(e, messageStr.get(), NULL);
    if (messageChars != NULL) {
        result += messageChars;
        (*env)->ReleaseStringUTFChars(e, messageStr.get(), messageChars);
    } else {
        result += "<error getting message>";
        (*env)->ExceptionClear(e);
    }
    return true;
}
static bool getStackTrace(C_JNIEnv* env, jthrowable exception, std::string& result) {
    JNIEnv* e = reinterpret_cast<JNIEnv*>(env);
    scoped_local_ref<jclass> stringWriterClass(env, findClass(env, "java/io/StringWriter"));
    if (stringWriterClass.get() == NULL) {
        return false;
    }
    jmethodID stringWriterCtor = (*env)->GetMethodID(e, stringWriterClass.get(), "<init>", "()V");
    jmethodID stringWriterToStringMethod =
            (*env)->GetMethodID(e, stringWriterClass.get(), "toString", "()Ljava/lang/String;");
    scoped_local_ref<jclass> printWriterClass(env, findClass(env, "java/io/PrintWriter"));
    if (printWriterClass.get() == NULL) {
        return false;
    }
    jmethodID printWriterCtor =
            (*env)->GetMethodID(e, printWriterClass.get(), "<init>", "(Ljava/io/Writer;)V");
    scoped_local_ref<jobject> stringWriter(env,
            (*env)->NewObject(e, stringWriterClass.get(), stringWriterCtor));
    if (stringWriter.get() == NULL) {
        return false;
    }
    jobject printWriter =
            (*env)->NewObject(e, printWriterClass.get(), printWriterCtor, stringWriter.get());
    if (printWriter == NULL) {
        return false;
    }
    scoped_local_ref<jclass> exceptionClass(env, (*env)->GetObjectClass(e, exception));
    jmethodID printStackTraceMethod =
            (*env)->GetMethodID(e, exceptionClass.get(), "printStackTrace", "(Ljava/io/PrintWriter;)V");
    (*env)->CallVoidMethod(e, exception, printStackTraceMethod, printWriter);
    if ((*env)->ExceptionCheck(e)) {
        return false;
    }
    scoped_local_ref<jstring> messageStr(env,
            (jstring) (*env)->CallObjectMethod(e, stringWriter.get(), stringWriterToStringMethod));
    if (messageStr.get() == NULL) {
        return false;
    }
    const char* utfChars = (*env)->GetStringUTFChars(e, messageStr.get(), NULL);
    if (utfChars == NULL) {
        return false;
    }
    result = utfChars;
    (*env)->ReleaseStringUTFChars(e, messageStr.get(), utfChars);
    return true;
}
extern "C" int jniThrowException(C_JNIEnv* env, const char* className, const char* msg) {
    JNIEnv* e = reinterpret_cast<JNIEnv*>(env);
    if ((*env)->ExceptionCheck(e)) {
        scoped_local_ref<jthrowable> exception(env, (*env)->ExceptionOccurred(e));
        (*env)->ExceptionClear(e);
        if (exception.get() != NULL) {
            std::string text;
            getExceptionSummary(env, exception.get(), text);
            ALOGW("Discarding pending exception (%s) to throw %s", text.c_str(), className);
        }
    }
    scoped_local_ref<jclass> exceptionClass(env, findClass(env, className));
    if (exceptionClass.get() == NULL) {
        ALOGE("Unable to find exception class %s", className);
        return -1;
    }
    if ((*env)->ThrowNew(e, exceptionClass.get(), msg) != JNI_OK) {
        ALOGE("Failed throwing '%s' '%s'", className, msg);
        return -1;
    }
    return 0;
}
int jniThrowExceptionFmt(C_JNIEnv* env, const char* className, const char* fmt, va_list args) {
    char msgBuf[512];
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
    return jniThrowException(env, className, msgBuf);
}
int jniThrowNullPointerException(C_JNIEnv* env, const char* msg) {
    return jniThrowException(env, "java/lang/NullPointerException", msg);
}
int jniThrowRuntimeException(C_JNIEnv* env, const char* msg) {
    return jniThrowException(env, "java/lang/RuntimeException", msg);
}
int jniThrowIOException(C_JNIEnv* env, int errnum) {
    char buffer[80];
    const char* message = jniStrError(errnum, buffer, sizeof(buffer));
    return jniThrowException(env, "java/io/IOException", message);
}
void jniLogException(C_JNIEnv* env, int priority, const char* tag, jthrowable exception) {
    std::string trace(jniGetStackTrace(env, exception));
    __android_log_write(priority, tag, trace.c_str());
}
extern "C" std::string jniGetStackTrace(C_JNIEnv* env, jthrowable exception) {
    JNIEnv* e = reinterpret_cast<JNIEnv*>(env);
    scoped_local_ref<jthrowable> currentException(env, (*env)->ExceptionOccurred(e));
    if (exception == NULL) {
        exception = currentException.get();
        if (exception == NULL) {
          return "<no pending exception>";
        }
    }
    if (currentException.get() != NULL) {
        (*env)->ExceptionClear(e);
    }
    std::string trace;
    if (!getStackTrace(env, exception, trace)) {
        (*env)->ExceptionClear(e);
        getExceptionSummary(env, exception, trace);
    }
    if (currentException.get() != NULL) {
        (*env)->Throw(e, currentException.get());
    }
    return trace;
}
const char* jniStrError(int errnum, char* buf, size_t buflen) {
    char* ret = (char*) strerror_r(errnum, buf, buflen);
    if (((int)ret) == 0) {
        return buf;
    } else if (((int)ret) == -1) {
        snprintf(buf, buflen, "errno %d", errnum);
        return buf;
    } else {
        return ret;
    }
}
jobject jniCreateFileDescriptor(C_JNIEnv* env, int fd) {
    JNIEnv* e = reinterpret_cast<JNIEnv*>(env);
    static jmethodID ctor = e->GetMethodID(JniConstants::fileDescriptorClass, "<init>", "()V");
    jobject fileDescriptor = (*env)->NewObject(e, JniConstants::fileDescriptorClass, ctor);
    jniSetFileDescriptorOfFD(env, fileDescriptor, fd);
    return fileDescriptor;
}
int jniGetFDFromFileDescriptor(C_JNIEnv* env, jobject fileDescriptor) {
    JNIEnv* e = reinterpret_cast<JNIEnv*>(env);
    static jfieldID fid = e->GetFieldID(JniConstants::fileDescriptorClass, "descriptor", "I");
    return (*env)->GetIntField(e, fileDescriptor, fid);
}
void jniSetFileDescriptorOfFD(C_JNIEnv* env, jobject fileDescriptor, int value) {
    JNIEnv* e = reinterpret_cast<JNIEnv*>(env);
    static jfieldID fid = e->GetFieldID(JniConstants::fileDescriptorClass, "descriptor", "I");
    (*env)->SetIntField(e, fileDescriptor, fid, value);
}
#define kNoCopyMagic 0xd5aab57f
extern "C" jbyte* jniGetNonMovableArrayElements(C_JNIEnv* env, jarray arrayObj) {
    JNIEnv* e = reinterpret_cast<JNIEnv*>(env);
    jbyteArray byteArray = reinterpret_cast<jbyteArray>(arrayObj);
    uint32_t noCopy = kNoCopyMagic;
    jbyte* result = (*env)->GetByteArrayElements(e, byteArray, reinterpret_cast<jboolean*>(&noCopy));
    (*env)->ReleaseByteArrayElements(e, byteArray, reinterpret_cast<jbyte*>(kNoCopyMagic), 0);
    return result;
}
