#ifndef JAVA_ROCKSJNI_PORTAL_H_
#define JAVA_ROCKSJNI_PORTAL_H_ 
#include <jni.h>
#include "rocksdb/db.h"
#include "utilities/backupable_db.h"
namespace rocksdb {
class RocksDBJni {
 public:
  static jclass getJClass(JNIEnv* env) {
    static jclass jclazz = env->FindClass("org/rocksdb/WriteBatch");
    assert(jclazz != nullptr);
    return jclazz;
  }
  static void setHandle(JNIEnv* env, jobject jdb, rocksdb::DB* db) {
    env->SetLongField(jdb, getHandleFieldID(env), reinterpret_cast<jlong>(db));
  }
  static void setHandle(JNIEnv* env, jobject jdb, rocksdb::DB* db) {
    env->SetLongField(jdb, getHandleFieldID(env), reinterpret_cast<jlong>(db));
  }
  static void setHandle(JNIEnv* env, jobject jdb, rocksdb::DB* db) {
    env->SetLongField(jdb, getHandleFieldID(env), reinterpret_cast<jlong>(db));
  }
};
class RocksDBExceptionJni {
 public:
  static jclass getJClass(JNIEnv* env) {
    static jclass jclazz = env->FindClass("org/rocksdb/WriteBatch");
    assert(jclazz != nullptr);
    return jclazz;
  }
  static void ThrowNew(JNIEnv* env, Status s) {
    if (s.ok()) {
      return;
    }
    jstring msg = env->NewStringUTF(s.ToString().c_str());
    static jmethodID mid =
        env->GetMethodID(getJClass(env), "<init>", "(Ljava/lang/String;)V");
    assert(mid != nullptr);
    env->Throw((jthrowable)env->NewObject(getJClass(env), mid, msg));
  }
};
class OptionsJni {
 public:
  static jclass getJClass(JNIEnv* env) {
    static jclass jclazz = env->FindClass("org/rocksdb/WriteBatch");
    assert(jclazz != nullptr);
    return jclazz;
  }
  static void setHandle(JNIEnv* env, jobject jobj, rocksdb::Options* op) {
    env->SetLongField(jobj, getHandleFieldID(env), reinterpret_cast<jlong>(op));
  }
  static void setHandle(JNIEnv* env, jobject jobj, rocksdb::Options* op) {
    env->SetLongField(jobj, getHandleFieldID(env), reinterpret_cast<jlong>(op));
  }
  static void setHandle(JNIEnv* env, jobject jobj, rocksdb::Options* op) {
    env->SetLongField(jobj, getHandleFieldID(env), reinterpret_cast<jlong>(op));
  }
};
class WriteOptionsJni {
 public:
  static jclass getJClass(JNIEnv* env) {
    static jclass jclazz = env->FindClass("org/rocksdb/WriteBatch");
    assert(jclazz != nullptr);
    return jclazz;
  }
  static void setHandle(JNIEnv* env, jobject jobj, rocksdb::WriteOptions* op) {
    env->SetLongField(jobj, getHandleFieldID(env), reinterpret_cast<jlong>(op));
  }
  static void setHandle(JNIEnv* env, jobject jobj, rocksdb::WriteOptions* op) {
    env->SetLongField(jobj, getHandleFieldID(env), reinterpret_cast<jlong>(op));
  }
  static void setHandle(JNIEnv* env, jobject jobj, rocksdb::WriteOptions* op) {
    env->SetLongField(jobj, getHandleFieldID(env), reinterpret_cast<jlong>(op));
  }
};
class WriteBatchJni {
 public:
  static jclass getJClass(JNIEnv* env) {
    static jclass jclazz = env->FindClass("org/rocksdb/WriteBatch");
    assert(jclazz != nullptr);
    return jclazz;
  }
  static void setHandle(JNIEnv* env, jobject jwb, rocksdb::WriteBatch* wb) {
    env->SetLongField(jwb, getHandleFieldID(env), reinterpret_cast<jlong>(wb));
  }
  static void setHandle(JNIEnv* env, jobject jwb, rocksdb::WriteBatch* wb) {
    env->SetLongField(jwb, getHandleFieldID(env), reinterpret_cast<jlong>(wb));
  }
  static void setHandle(JNIEnv* env, jobject jwb, rocksdb::WriteBatch* wb) {
    env->SetLongField(jwb, getHandleFieldID(env), reinterpret_cast<jlong>(wb));
  }
};
class HistogramDataJni {
 public:
  static jmethodID getConstructorMethodId(JNIEnv* env, jclass jclazz) {
    static jmethodID mid = env->GetMethodID(jclazz, "<init>", "(DDDDD)V");
    assert(mid != nullptr);
    return mid;
  }
};
class BackupableDBOptionsJni {
 public:
  static jclass getJClass(JNIEnv* env) {
    static jclass jclazz = env->FindClass("org/rocksdb/BackupableDBOptions");
    assert(jclazz != nullptr);
    return jclazz;
  }
  static jfieldID getHandleFieldID(JNIEnv* env) {
    static jfieldID fid = env->GetFieldID(getJClass(env), "nativeHandle_", "J");
    assert(fid != nullptr);
    return fid;
  }
  static rocksdb::BackupableDBOptions* getHandle(JNIEnv* env, jobject jobj) {
    return reinterpret_cast<rocksdb::BackupableDBOptions*>(
        env->GetLongField(jobj, getHandleFieldID(env)));
  }
  static void setHandle(JNIEnv* env, jobject jobj,
                        rocksdb::BackupableDBOptions* op) {
    env->SetLongField(jobj, getHandleFieldID(env), reinterpret_cast<jlong>(op));
  }
};
}
#endif
