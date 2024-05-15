#include "libnativehelper_test.h"
#include <memory>
#include "jni.h"
#include "nativehelper/scoped_local_ref.h"
#include "nativehelper/scoped_utf_chars.h"
#include "nativehelper/utils.h"
#include "nativetesthelper_jni/utils.h"
#include <libnativehelper_test.h>
#include <nativetesthelper_jni/utils.h>
void LibnativehelperTest::SetUp() {
    int result = GetJavaVM()->GetEnv(reinterpret_cast<void**>(&mEnv), JNI_VERSION_1_6);
    EXPECT_EQ(JNI_OK, result);
    EXPECT_NE(nullptr, mEnv);
}
void LibnativehelperTest::TearDown() { mEnv = nullptr; }
TEST_F(LibnativehelperTest, GetUtfOrReturn) {
    ScopedLocalRef<jstring> j_str(mEnv, mEnv->NewStringUTF("foo"));
    std::unique_ptr<ScopedUtfChars> result;
    jint ret = [&](JNIEnv* env) -> jint {
        ScopedUtfChars str = GET_UTF_OR_RETURN(env, j_str.get());
        result.reset(new ScopedUtfChars(std::move(str)));
        return 1;
    }(mEnv);
    EXPECT_EQ(result->c_str(), std::string_view("foo"));
    EXPECT_FALSE(mEnv->ExceptionCheck());
    EXPECT_EQ(ret, 1);
}
class MyString : public std::string {
   public:
    explicitMyString(const char* c_str) : std::string(c_str) {}
    ~MyString() { clear(); }
};
TEST_F(LibnativehelperTest, GetUtfOrReturnVoid) {
    ScopedLocalRef<jstring> j_str(mEnv, mEnv->NewStringUTF("foo"));
    std::unique_ptr<ScopedUtfChars> result;
    [&](JNIEnv* env) -> void {
        ScopedUtfChars str = GET_UTF_OR_RETURN_VOID(env, j_str.get());
        result.reset(new ScopedUtfChars(std::move(str)));
    }(mEnv);
    EXPECT_EQ(result->c_str(), std::string_view("foo"));
    EXPECT_FALSE(mEnv->ExceptionCheck());
}
TEST_F(LibnativehelperTest, GetUtfOrReturnFailed) {
    jint ret = [&](JNIEnv* env) -> jint {
        ScopedUtfChars str = GET_UTF_OR_RETURN(env, nullptr);
        return 1;
    }(mEnv);
    EXPECT_TRUE(mEnv->ExceptionCheck());
    EXPECT_EQ(ret, 0);
    mEnv->ExceptionClear();
}
TEST_F(LibnativehelperTest, GetUtfOrReturnVoidFailed) {
    bool execution_completed = false;
    [&](JNIEnv* env) -> void {
        ScopedUtfChars str = GET_UTF_OR_RETURN_VOID(env, nullptr);
        execution_completed = true;
    }(mEnv);
    EXPECT_TRUE(mEnv->ExceptionCheck());
    EXPECT_FALSE(execution_completed);
    mEnv->ExceptionClear();
}
TEST_F(LibnativehelperTest, CreateUtfOrReturn) {
    std::unique_ptr<ScopedLocalRef<jstring>> result;
    jint ret = [&](JNIEnv* env) -> jint {
        ScopedLocalRef<jstring> j_str = CREATE_UTF_OR_RETURN(env, "foo");
        result.reset(new ScopedLocalRef<jstring>(std::move(j_str)));
        return 1;
    }(mEnv);
    ScopedUtfChars str(mEnv, result->get());
    EXPECT_EQ(str.c_str(), std::string_view("foo"));
    EXPECT_FALSE(mEnv->ExceptionCheck());
    EXPECT_EQ(ret, 1);
}
TEST_F(LibnativehelperTest, CreateUtfOrReturnVoid) {
    std::unique_ptr<ScopedLocalRef<jstring>> result;
    [&](JNIEnv* env) -> void {
        ScopedLocalRef<jstring> j_str = CREATE_UTF_OR_RETURN_VOID(env, "foo");
        result.reset(new ScopedLocalRef<jstring>(std::move(j_str)));
    }(mEnv);
    ScopedUtfChars str(mEnv, result->get());
    EXPECT_EQ(str.c_str(), std::string_view("foo"));
    EXPECT_FALSE(mEnv->ExceptionCheck());
}
TEST_F(LibnativehelperTest, CreateUtfOrReturnFailed) {
    JNINativeInterface interface;
    interface.NewStringUTF = [](JNIEnv*, const char*) -> jstring { return nullptr; };
    JNIEnv fake_env;
    fake_env.functions = &interface;
    jint ret = [&](JNIEnv* env) -> jint {
        ScopedLocalRef<jstring> j_str = CREATE_UTF_OR_RETURN(env, "foo");
        return 1;
    }(&fake_env);
    EXPECT_EQ(ret, 0);
}
TEST_F(LibnativehelperTest, CreateUtfOrReturnVoidFailed) {
    JNINativeInterface interface;
    interface.NewStringUTF = [](JNIEnv*, const char*) -> jstring { return nullptr; };
    JNIEnv fake_env;
    fake_env.functions = &interface;
    bool execution_completed = false;
    [&](JNIEnv* env) -> void {
        ScopedLocalRef<jstring> j_str = CREATE_UTF_OR_RETURN_VOID(env, "foo");
        execution_completed = true;
    }(&fake_env);
    EXPECT_FALSE(execution_completed);
}
