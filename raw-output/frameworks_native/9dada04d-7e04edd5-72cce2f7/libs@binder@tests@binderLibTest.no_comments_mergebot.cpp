#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <gtest/gtest.h>
#include <binder/Binder.h>
#include <binder/IBinder.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#define ARRAY_SIZE(array) (sizeof array / sizeof array[0])
using namespace android;
static ::testing::AssertionResult IsPageAligned(void *buf) {
    if (((unsigned long)buf & ((unsigned long)PAGE_SIZE - 1)) == 0)
        return ::testing::AssertionSuccess();
    else
        return ::testing::AssertionFailure() << buf << " is not page aligned";
}
static testing::Environment* binder_env;
static char *binderservername;
static char *binderserversuffix;
static char binderserverarg[] = "--binderserver";
static String16 binderLibTestServiceName = String16("test.binderLib");
enum BinderLibTestTranscationCode {
BINDER_LIB_TEST_NOP_TRANSACTION = IBinder::FIRST_CALL_TRANSACTION,BINDER_LIB_TEST_REGISTER_SERVER,BINDER_LIB_TEST_ADD_SERVER,BINDER_LIB_TEST_CALL_BACK,BINDER_LIB_TEST_CALL_BACK_VERIFY_BUF,BINDER_LIB_TEST_NOP_CALL_BACK,BINDER_LIB_TEST_GET_SELF_TRANSACTION,BINDER_LIB_TEST_GET_ID_TRANSACTION,BINDER_LIB_TEST_INDIRECT_TRANSACTION,BINDER_LIB_TEST_SET_ERROR_TRANSACTION,BINDER_LIB_TEST_GET_STATUS_TRANSACTION,BINDER_LIB_TEST_ADD_STRONG_REF_TRANSACTION,BINDER_LIB_TEST_LINK_DEATH_TRANSACTION,BINDER_LIB_TEST_WRITE_FILE_TRANSACTION,BINDER_LIB_TEST_PROMOTE_WEAK_REF_TRANSACTION,BINDER_LIB_TEST_EXIT_TRANSACTION,BINDER_LIB_TEST_DELAYED_EXIT_TRANSACTION,BINDER_LIB_TEST_GET_PTR_SIZE_TRANSACTION,BINDER_LIB_TEST_CREATE_BINDER_TRANSACTION,};
pid_t start_server_process(int arg2)
{
    int ret;
    pid_t pid;
    status_t status;
    int pipefd[2];
    char stri[16];
    char strpipefd1[16];
    char *childargv[] = {
        binderservername,
        binderserverarg,
        stri,
        strpipefd1,
        binderserversuffix,
        NULL
    };
    ret = pipe(pipefd);
    if (ret < 0)
        return ret;
    snprintf(stri, sizeof(stri), "%d", arg2);
    snprintf(strpipefd1, sizeof(strpipefd1), "%d", pipefd[1]);
    pid = fork();
    if (pid == -1)
        return pid;
    if (pid == 0) {
        close(pipefd[0]);
        execv(binderservername, childargv);
        status = -errno;
        write(pipefd[1], &status, sizeof(status));
        fprintf(stderr, "execv failed, %s\n", strerror(errno));
        _exit(EXIT_FAILURE);
    }
    close(pipefd[1]);
    ret = read(pipefd[0], &status, sizeof(status));
    close(pipefd[0]);
    if (ret == sizeof(status)) {
        ret = status;
    } else {
        kill(pid, SIGKILL);
        if (ret >= 0) {
            ret = NO_INIT;
        }
    }
    if (ret < 0) {
        wait(NULL);
        return ret;
    }
    return pid;
}
class BinderLibTestEnv : public ::testing::Environment {
public:
        BinderLibTestEnv() {}
        sp<IBinder> getServer(void) {
            return m_server;
        }
private:
        virtual void SetUp() {
            m_serverpid = start_server_process(0);
            ASSERT_GT(m_serverpid, 0);
            sp<IServiceManager> sm = defaultServiceManager();
            m_server = sm->getService(binderLibTestServiceName);
            ASSERT_TRUE(m_server != NULL);
        }
        virtual void TearDown() {
            status_t ret;
            Parcel data, reply;
            int exitStatus;
            pid_t pid;
            if (m_server != NULL) {
                ret = m_server->transact(BINDER_LIB_TEST_GET_STATUS_TRANSACTION, data, &reply);
                EXPECT_EQ(0, ret);
                ret = m_server->transact(BINDER_LIB_TEST_EXIT_TRANSACTION, data, &reply, TF_ONE_WAY);
                EXPECT_EQ(0, ret);
            }
            if (m_serverpid > 0) {
                pid = wait(&exitStatus);
                EXPECT_EQ(m_serverpid, pid);
                EXPECT_TRUE(WIFEXITED(exitStatus));
                EXPECT_EQ(0, WEXITSTATUS(exitStatus));
            }
        }
        pid_t m_serverpid;
        sp<IBinder> m_server;
};
class BinderLibTest : public ::testing::Test {
public:
        virtual void SetUp() {
            m_server = static_cast<BinderLibTestEnv *>(binder_env)->getServer();
        }
        virtual void TearDown() {
        }
protected:
        sp<IBinder> addServer(int32_t *idPtr = NULL)
        {
            int ret;
            int32_t id;
            Parcel data, reply;
            sp<IBinder> binder;
            ret = m_server->transact(BINDER_LIB_TEST_ADD_SERVER, data, &reply);
            EXPECT_EQ(NO_ERROR, ret);
            EXPECT_FALSE(binder != NULL);
            binder = reply.readStrongBinder();
            EXPECT_TRUE(binder != NULL);
            ret = reply.readInt32(&id);
            EXPECT_EQ(NO_ERROR, ret);
            if (idPtr)
                *idPtr = id;
            return binder;
        }
        void waitForReadData(int fd, int timeout_ms) {
            int ret;
            pollfd pfd = pollfd();
            pfd.fd = fd;
            pfd.events = POLLIN;
            ret = poll(&pfd, 1, timeout_ms);
            EXPECT_EQ(1, ret);
        }
        sp<IBinder> m_server;
};
class BinderLibTestBundle : public Parcel
{
public:
        BinderLibTestBundle(void) {}
        BinderLibTestBundle(const Parcel *source) : m_isValid(false) {
            int32_t mark;
            int32_t bundleLen;
            size_t pos;
            if (source->readInt32(&mark))
                return;
            if (mark != MARK_START)
                return;
            if (source->readInt32(&bundleLen))
                return;
            pos = source->dataPosition();
            if (Parcel::appendFrom(source, pos, bundleLen))
                return;
            source->setDataPosition(pos + bundleLen);
            if (source->readInt32(&mark))
                return;
            if (mark != MARK_END)
                return;
            m_isValid = true;
            setDataPosition(0);
        }
void appendTo(Parcel *dest) {
            dest->writeInt32(MARK_START);
            dest->writeInt32(dataSize());
            dest->appendFrom(this, 0, dataSize());
            dest->writeInt32(MARK_END);
        } bool isValid(void) {
            return m_isValid;
        }
private:
        enum {
            MARK_START = B_PACK_CHARS('B','T','B','S'),
            MARK_END = B_PACK_CHARS('B','T','B','E'),
        };
        bool m_isValid;
};
class BinderLibTestEvent
{
public:
        BinderLibTestEvent(void)
            : m_eventTriggered(false)
        {
            pthread_mutex_init(&m_waitMutex, NULL);
            pthread_cond_init(&m_waitCond, NULL);
        }
        int waitEvent(int timeout_s)
        {
            int ret;
            pthread_mutex_lock(&m_waitMutex);
            if (!m_eventTriggered) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += timeout_s;
                pthread_cond_timedwait(&m_waitCond, &m_waitMutex, &ts);
            }
            ret = m_eventTriggered ? NO_ERROR : TIMED_OUT;
            pthread_mutex_unlock(&m_waitMutex);
            return ret;
        }
        pthread_t getTriggeringThread()
        {
            return m_triggeringThread;
        }
protected:
void triggerEvent(void) {
            pthread_mutex_lock(&m_waitMutex);
            pthread_cond_signal(&m_waitCond);
            m_eventTriggered = true;
            m_triggeringThread = pthread_self();
            pthread_mutex_unlock(&m_waitMutex);
        }private:
        pthread_mutex_t m_waitMutex;
        pthread_cond_t m_waitCond;
        bool m_eventTriggered;
        pthread_t m_triggeringThread;
};
class BinderLibTestCallBack : public BBinder, public BinderLibTestEvent
{
public:
        BinderLibTestCallBack()
            : m_result(NOT_ENOUGH_DATA)
            , m_prev_end(NULL)
        {
        }
        status_t getResult(void)
        {
            return m_result;
        }
private:
        virtual status_t onTransact(uint32_t code,
                                    const Parcel& data, Parcel* reply,
                                    uint32_t flags = 0)
        {
            (void)reply;
            (void)flags;
            switch(code) {
            case BINDER_LIB_TEST_CALL_BACK:
                m_result = data.readInt32();
                triggerEvent();
                return NO_ERROR;
            case BINDER_LIB_TEST_CALL_BACK_VERIFY_BUF: {
                sp<IBinder> server;
                int ret;
                const uint8_t *buf = data.data();
                size_t size = data.dataSize();
                if (m_prev_end) {
                    EXPECT_LE((size_t)(buf - m_prev_end), (size_t)8);
                } else {
                    EXPECT_TRUE(IsPageAligned((void *)buf));
                }
                m_prev_end = buf + size + data.objectsCount() * sizeof(binder_size_t);
                if (size > 0) {
                    server = static_cast<BinderLibTestEnv *>(binder_env)->getServer();
                    ret = server->transact(BINDER_LIB_TEST_INDIRECT_TRANSACTION,
                                           data, reply);
                    EXPECT_EQ(NO_ERROR, ret);
                }
                return NO_ERROR;
            }
            default:
                return UNKNOWN_TRANSACTION;
            }
        }
        status_t m_result;
        const uint8_t *m_prev_end;
};
class TestDeathRecipient : public IBinder::DeathRecipient, public BinderLibTestEvent
{
private:
virtual void binderDied(const wp<IBinder>& who) {
            (void)who;
            triggerEvent();
        }};
TEST_F(BinderLibTest, CheckNoHeaderMappedInUser) {
    status_t ret;
    Parcel data, reply;
    sp<BinderLibTestCallBack> callBack = new BinderLibTestCallBack();
    for (int i = 0; i < 2; i++) {
        BinderLibTestBundle datai;
        datai.appendFrom(&data, 0, data.dataSize());
        data.freeData();
        data.writeInt32(1);
        data.writeStrongBinder(callBack);
        data.writeInt32(BINDER_LIB_TEST_CALL_BACK_VERIFY_BUF);
        datai.appendTo(&data);
    }
    ret = m_server->transact(BINDER_LIB_TEST_INDIRECT_TRANSACTION, data, &reply);
    EXPECT_EQ(NO_ERROR, ret);
}
TEST_F(BinderLibTest, CheckNoHeaderMappedInUser) {
    status_t ret;
    Parcel data, reply;
    sp<BinderLibTestCallBack> callBack = new BinderLibTestCallBack();
    for (int i = 0; i < 2; i++) {
        BinderLibTestBundle datai;
        datai.appendFrom(&data, 0, data.dataSize());
        data.freeData();
        data.writeInt32(1);
        data.writeStrongBinder(callBack);
        data.writeInt32(BINDER_LIB_TEST_CALL_BACK_VERIFY_BUF);
        datai.appendTo(&data);
    }
    ret = m_server->transact(BINDER_LIB_TEST_INDIRECT_TRANSACTION, data, &reply);
    EXPECT_EQ(NO_ERROR, ret);
}
TEST_F(BinderLibTest, CheckNoHeaderMappedInUser) {
    status_t ret;
    Parcel data, reply;
    sp<BinderLibTestCallBack> callBack = new BinderLibTestCallBack();
    for (int i = 0; i < 2; i++) {
        BinderLibTestBundle datai;
        datai.appendFrom(&data, 0, data.dataSize());
        data.freeData();
        data.writeInt32(1);
        data.writeStrongBinder(callBack);
        data.writeInt32(BINDER_LIB_TEST_CALL_BACK_VERIFY_BUF);
        datai.appendTo(&data);
    }
    ret = m_server->transact(BINDER_LIB_TEST_INDIRECT_TRANSACTION, data, &reply);
    EXPECT_EQ(NO_ERROR, ret);
}
TEST_F(BinderLibTest, CheckNoHeaderMappedInUser) {
    status_t ret;
    Parcel data, reply;
    sp<BinderLibTestCallBack> callBack = new BinderLibTestCallBack();
    for (int i = 0; i < 2; i++) {
        BinderLibTestBundle datai;
        datai.appendFrom(&data, 0, data.dataSize());
        data.freeData();
        data.writeInt32(1);
        data.writeStrongBinder(callBack);
        data.writeInt32(BINDER_LIB_TEST_CALL_BACK_VERIFY_BUF);
        datai.appendTo(&data);
    }
    ret = m_server->transact(BINDER_LIB_TEST_INDIRECT_TRANSACTION, data, &reply);
    EXPECT_EQ(NO_ERROR, ret);
}
TEST_F(BinderLibTest, CheckNoHeaderMappedInUser) {
    status_t ret;
    Parcel data, reply;
    sp<BinderLibTestCallBack> callBack = new BinderLibTestCallBack();
    for (int i = 0; i < 2; i++) {
        BinderLibTestBundle datai;
        datai.appendFrom(&data, 0, data.dataSize());
        data.freeData();
        data.writeInt32(1);
        data.writeStrongBinder(callBack);
        data.writeInt32(BINDER_LIB_TEST_CALL_BACK_VERIFY_BUF);
        datai.appendTo(&data);
    }
    ret = m_server->transact(BINDER_LIB_TEST_INDIRECT_TRANSACTION, data, &reply);
    EXPECT_EQ(NO_ERROR, ret);
}
TEST_F(BinderLibTest, CheckNoHeaderMappedInUser) {
    status_t ret;
    Parcel data, reply;
    sp<BinderLibTestCallBack> callBack = new BinderLibTestCallBack();
    for (int i = 0; i < 2; i++) {
        BinderLibTestBundle datai;
        datai.appendFrom(&data, 0, data.dataSize());
        data.freeData();
        data.writeInt32(1);
        data.writeStrongBinder(callBack);
        data.writeInt32(BINDER_LIB_TEST_CALL_BACK_VERIFY_BUF);
        datai.appendTo(&data);
    }
    ret = m_server->transact(BINDER_LIB_TEST_INDIRECT_TRANSACTION, data, &reply);
    EXPECT_EQ(NO_ERROR, ret);
}
TEST_F(BinderLibTest, CheckNoHeaderMappedInUser) {
    status_t ret;
    Parcel data, reply;
    sp<BinderLibTestCallBack> callBack = new BinderLibTestCallBack();
    for (int i = 0; i < 2; i++) {
        BinderLibTestBundle datai;
        datai.appendFrom(&data, 0, data.dataSize());
        data.freeData();
        data.writeInt32(1);
        data.writeStrongBinder(callBack);
        data.writeInt32(BINDER_LIB_TEST_CALL_BACK_VERIFY_BUF);
        datai.appendTo(&data);
    }
    ret = m_server->transact(BINDER_LIB_TEST_INDIRECT_TRANSACTION, data, &reply);
    EXPECT_EQ(NO_ERROR, ret);
}
TEST_F(BinderLibTest, CheckNoHeaderMappedInUser) {
    status_t ret;
    Parcel data, reply;
    sp<BinderLibTestCallBack> callBack = new BinderLibTestCallBack();
    for (int i = 0; i < 2; i++) {
        BinderLibTestBundle datai;
        datai.appendFrom(&data, 0, data.dataSize());
        data.freeData();
        data.writeInt32(1);
        data.writeStrongBinder(callBack);
        data.writeInt32(BINDER_LIB_TEST_CALL_BACK_VERIFY_BUF);
        datai.appendTo(&data);
    }
    ret = m_server->transact(BINDER_LIB_TEST_INDIRECT_TRANSACTION, data, &reply);
    EXPECT_EQ(NO_ERROR, ret);
}
TEST_F(BinderLibTest, CheckNoHeaderMappedInUser) {
    status_t ret;
    Parcel data, reply;
    sp<BinderLibTestCallBack> callBack = new BinderLibTestCallBack();
    for (int i = 0; i < 2; i++) {
        BinderLibTestBundle datai;
        datai.appendFrom(&data, 0, data.dataSize());
        data.freeData();
        data.writeInt32(1);
        data.writeStrongBinder(callBack);
        data.writeInt32(BINDER_LIB_TEST_CALL_BACK_VERIFY_BUF);
        datai.appendTo(&data);
    }
    ret = m_server->transact(BINDER_LIB_TEST_INDIRECT_TRANSACTION, data, &reply);
    EXPECT_EQ(NO_ERROR, ret);
}
TEST_F(BinderLibTest, CheckNoHeaderMappedInUser) {
    status_t ret;
    Parcel data, reply;
    sp<BinderLibTestCallBack> callBack = new BinderLibTestCallBack();
    for (int i = 0; i < 2; i++) {
        BinderLibTestBundle datai;
        datai.appendFrom(&data, 0, data.dataSize());
        data.freeData();
        data.writeInt32(1);
        data.writeStrongBinder(callBack);
        data.writeInt32(BINDER_LIB_TEST_CALL_BACK_VERIFY_BUF);
        datai.appendTo(&data);
    }
    ret = m_server->transact(BINDER_LIB_TEST_INDIRECT_TRANSACTION, data, &reply);
    EXPECT_EQ(NO_ERROR, ret);
}
TEST_F(BinderLibTest, CheckNoHeaderMappedInUser) {
    status_t ret;
    Parcel data, reply;
    sp<BinderLibTestCallBack> callBack = new BinderLibTestCallBack();
    for (int i = 0; i < 2; i++) {
        BinderLibTestBundle datai;
        datai.appendFrom(&data, 0, data.dataSize());
        data.freeData();
        data.writeInt32(1);
        data.writeStrongBinder(callBack);
        data.writeInt32(BINDER_LIB_TEST_CALL_BACK_VERIFY_BUF);
        datai.appendTo(&data);
    }
    ret = m_server->transact(BINDER_LIB_TEST_INDIRECT_TRANSACTION, data, &reply);
    EXPECT_EQ(NO_ERROR, ret);
}
TEST_F(BinderLibTest, CheckNoHeaderMappedInUser) {
    status_t ret;
    Parcel data, reply;
    sp<BinderLibTestCallBack> callBack = new BinderLibTestCallBack();
    for (int i = 0; i < 2; i++) {
        BinderLibTestBundle datai;
        datai.appendFrom(&data, 0, data.dataSize());
        data.freeData();
        data.writeInt32(1);
        data.writeStrongBinder(callBack);
        data.writeInt32(BINDER_LIB_TEST_CALL_BACK_VERIFY_BUF);
        datai.appendTo(&data);
    }
    ret = m_server->transact(BINDER_LIB_TEST_INDIRECT_TRANSACTION, data, &reply);
    EXPECT_EQ(NO_ERROR, ret);
}
TEST_F(BinderLibTest, CheckNoHeaderMappedInUser) {
    status_t ret;
    Parcel data, reply;
    sp<BinderLibTestCallBack> callBack = new BinderLibTestCallBack();
    for (int i = 0; i < 2; i++) {
        BinderLibTestBundle datai;
        datai.appendFrom(&data, 0, data.dataSize());
        data.freeData();
        data.writeInt32(1);
        data.writeStrongBinder(callBack);
        data.writeInt32(BINDER_LIB_TEST_CALL_BACK_VERIFY_BUF);
        datai.appendTo(&data);
    }
    ret = m_server->transact(BINDER_LIB_TEST_INDIRECT_TRANSACTION, data, &reply);
    EXPECT_EQ(NO_ERROR, ret);
}
TEST_F(BinderLibTest, CheckNoHeaderMappedInUser) {
    status_t ret;
    Parcel data, reply;
    sp<BinderLibTestCallBack> callBack = new BinderLibTestCallBack();
    for (int i = 0; i < 2; i++) {
        BinderLibTestBundle datai;
        datai.appendFrom(&data, 0, data.dataSize());
        data.freeData();
        data.writeInt32(1);
        data.writeStrongBinder(callBack);
        data.writeInt32(BINDER_LIB_TEST_CALL_BACK_VERIFY_BUF);
        datai.appendTo(&data);
    }
    ret = m_server->transact(BINDER_LIB_TEST_INDIRECT_TRANSACTION, data, &reply);
    EXPECT_EQ(NO_ERROR, ret);
}
TEST_F(BinderLibTest, CheckNoHeaderMappedInUser) {
    status_t ret;
    Parcel data, reply;
    sp<BinderLibTestCallBack> callBack = new BinderLibTestCallBack();
    for (int i = 0; i < 2; i++) {
        BinderLibTestBundle datai;
        datai.appendFrom(&data, 0, data.dataSize());
        data.freeData();
        data.writeInt32(1);
        data.writeStrongBinder(callBack);
        data.writeInt32(BINDER_LIB_TEST_CALL_BACK_VERIFY_BUF);
        datai.appendTo(&data);
    }
    ret = m_server->transact(BINDER_LIB_TEST_INDIRECT_TRANSACTION, data, &reply);
    EXPECT_EQ(NO_ERROR, ret);
}
TEST_F(BinderLibTest, CheckNoHeaderMappedInUser) {
    status_t ret;
    Parcel data, reply;
    sp<BinderLibTestCallBack> callBack = new BinderLibTestCallBack();
    for (int i = 0; i < 2; i++) {
        BinderLibTestBundle datai;
        datai.appendFrom(&data, 0, data.dataSize());
        data.freeData();
        data.writeInt32(1);
        data.writeStrongBinder(callBack);
        data.writeInt32(BINDER_LIB_TEST_CALL_BACK_VERIFY_BUF);
        datai.appendTo(&data);
    }
    ret = m_server->transact(BINDER_LIB_TEST_INDIRECT_TRANSACTION, data, &reply);
    EXPECT_EQ(NO_ERROR, ret);
}
TEST_F(BinderLibTest, CheckNoHeaderMappedInUser) {
    status_t ret;
    Parcel data, reply;
    sp<BinderLibTestCallBack> callBack = new BinderLibTestCallBack();
    for (int i = 0; i < 2; i++) {
        BinderLibTestBundle datai;
        datai.appendFrom(&data, 0, data.dataSize());
        data.freeData();
        data.writeInt32(1);
        data.writeStrongBinder(callBack);
        data.writeInt32(BINDER_LIB_TEST_CALL_BACK_VERIFY_BUF);
        datai.appendTo(&data);
    }
    ret = m_server->transact(BINDER_LIB_TEST_INDIRECT_TRANSACTION, data, &reply);
    EXPECT_EQ(NO_ERROR, ret);
}
class BinderLibTestService : public BBinder
{
public:
        BinderLibTestService(int32_t id)
            : m_id(id)
            , m_nextServerId(id + 1)
            , m_serverStartRequested(false)
        {
            pthread_mutex_init(&m_serverWaitMutex, NULL);
            pthread_cond_init(&m_serverWaitCond, NULL);
        }
        ~BinderLibTestService()
        {
            exit(EXIT_SUCCESS);
        }
        virtual status_t onTransact(uint32_t code,
                                    const Parcel& data, Parcel* reply,
                                    uint32_t flags = 0) {
            (void)flags;
            if (getuid() != (uid_t)IPCThreadState::self()->getCallingUid()) {
                return PERMISSION_DENIED;
            }
            switch (code) {
            case BINDER_LIB_TEST_REGISTER_SERVER: {
                int32_t id;
                sp<IBinder> binder;
                id = data.readInt32();
                binder = data.readStrongBinder();
                if (binder == NULL) {
                    return BAD_VALUE;
                }
                if (m_id != 0)
                    return INVALID_OPERATION;
                pthread_mutex_lock(&m_serverWaitMutex);
                if (m_serverStartRequested) {
                    m_serverStartRequested = false;
                    m_serverStarted = binder;
                    pthread_cond_signal(&m_serverWaitCond);
                }
                pthread_mutex_unlock(&m_serverWaitMutex);
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_ADD_SERVER: {
                int ret;
                uint8_t buf[1] = { 0 };
                int serverid;
                if (m_id != 0) {
                    return INVALID_OPERATION;
                }
                pthread_mutex_lock(&m_serverWaitMutex);
                if (m_serverStartRequested) {
                    ret = -EBUSY;
                } else {
                    serverid = m_nextServerId++;
                    m_serverStartRequested = true;
                    pthread_mutex_unlock(&m_serverWaitMutex);
                    ret = start_server_process(serverid);
                    pthread_mutex_lock(&m_serverWaitMutex);
                }
                if (ret > 0) {
                    if (m_serverStartRequested) {
                        struct timespec ts;
                        clock_gettime(CLOCK_REALTIME, &ts);
                        ts.tv_sec += 5;
                        ret = pthread_cond_timedwait(&m_serverWaitCond, &m_serverWaitMutex, &ts);
                    }
                    if (m_serverStartRequested) {
                        m_serverStartRequested = false;
                        ret = -ETIMEDOUT;
                    } else {
                        reply->writeStrongBinder(m_serverStarted);
                        reply->writeInt32(serverid);
                        m_serverStarted = NULL;
                        ret = NO_ERROR;
                    }
                } else if (ret >= 0) {
                    m_serverStartRequested = false;
                    ret = UNKNOWN_ERROR;
                }
                pthread_mutex_unlock(&m_serverWaitMutex);
                return ret;
            }
            case BINDER_LIB_TEST_NOP_TRANSACTION:
                return NO_ERROR;
            case BINDER_LIB_TEST_NOP_CALL_BACK: {
                Parcel data2, reply2;
                sp<IBinder> binder;
                binder = data.readStrongBinder();
                if (binder == NULL) {
                    return BAD_VALUE;
                }
                reply2.writeInt32(NO_ERROR);
                binder->transact(BINDER_LIB_TEST_CALL_BACK, data2, &reply2);
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_GET_SELF_TRANSACTION:
                reply->writeStrongBinder(this);
                return NO_ERROR;
            case BINDER_LIB_TEST_GET_ID_TRANSACTION:
                reply->writeInt32(m_id);
                return NO_ERROR;
            case BINDER_LIB_TEST_INDIRECT_TRANSACTION: {
                int32_t count;
                uint32_t indirect_code;
                sp<IBinder> binder;
                count = data.readInt32();
                reply->writeInt32(m_id);
                reply->writeInt32(count);
                for (int i = 0; i < count; i++) {
                    binder = data.readStrongBinder();
                    if (binder == NULL) {
                        return BAD_VALUE;
                    }
                    indirect_code = data.readInt32();
                    BinderLibTestBundle data2(&data);
                    if (!data2.isValid()) {
                        return BAD_VALUE;
                    }
                    BinderLibTestBundle reply2;
                    binder->transact(indirect_code, data2, &reply2);
                    reply2.appendTo(reply);
                }
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_SET_ERROR_TRANSACTION:
                reply->setError(data.readInt32());
                return NO_ERROR;
            case BINDER_LIB_TEST_GET_PTR_SIZE_TRANSACTION:
                reply->writeInt32(sizeof(void *));
                return NO_ERROR;
            case BINDER_LIB_TEST_GET_STATUS_TRANSACTION:
                return NO_ERROR;
            case BINDER_LIB_TEST_ADD_STRONG_REF_TRANSACTION:
                m_strongRef = data.readStrongBinder();
                return NO_ERROR;
            case BINDER_LIB_TEST_LINK_DEATH_TRANSACTION: {
                int ret;
                Parcel data2, reply2;
                sp<TestDeathRecipient> testDeathRecipient = new TestDeathRecipient();
                sp<IBinder> target;
                sp<IBinder> callback;
                target = data.readStrongBinder();
                if (target == NULL) {
                    return BAD_VALUE;
                }
                callback = data.readStrongBinder();
                if (callback == NULL) {
                    return BAD_VALUE;
                }
                ret = target->linkToDeath(testDeathRecipient);
                if (ret == NO_ERROR)
                    ret = testDeathRecipient->waitEvent(5);
                data2.writeInt32(ret);
                callback->transact(BINDER_LIB_TEST_CALL_BACK, data2, &reply2);
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_WRITE_FILE_TRANSACTION: {
                int ret;
                int32_t size;
                const void *buf;
                int fd;
                fd = data.readFileDescriptor();
                if (fd < 0) {
                    return BAD_VALUE;
                }
                ret = data.readInt32(&size);
                if (ret != NO_ERROR) {
                    return ret;
                }
                buf = data.readInplace(size);
                if (buf == NULL) {
                    return BAD_VALUE;
                }
                ret = write(fd, buf, size);
                if (ret != size)
                    return UNKNOWN_ERROR;
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_PROMOTE_WEAK_REF_TRANSACTION: {
                int ret;
                wp<IBinder> weak;
                sp<IBinder> strong;
                Parcel data2, reply2;
                sp<IServiceManager> sm = defaultServiceManager();
                sp<IBinder> server = sm->getService(binderLibTestServiceName);
                weak = data.readWeakBinder();
                if (weak == NULL) {
                    return BAD_VALUE;
                }
                strong = weak.promote();
                ret = server->transact(BINDER_LIB_TEST_NOP_TRANSACTION, data2, &reply2);
                if (ret != NO_ERROR)
                    exit(EXIT_FAILURE);
                if (strong == NULL) {
                    reply->setError(1);
                }
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_DELAYED_EXIT_TRANSACTION:
                alarm(10);
                return NO_ERROR;
            case BINDER_LIB_TEST_EXIT_TRANSACTION:
                while (wait(NULL) != -1 || errno != ECHILD)
                    ;
                exit(EXIT_SUCCESS);
            case BINDER_LIB_TEST_CREATE_BINDER_TRANSACTION: {
                bool strongRef = data.readBool();
                sp<IBinder> binder = new BBinder();
                if (strongRef) {
                    reply->writeStrongBinder(binder);
                } else {
                    reply->writeWeakBinder(binder);
                }
                return NO_ERROR;
            }
            default:
                return UNKNOWN_TRANSACTION;
            };
        }
private:
        int32_t m_id;
        int32_t m_nextServerId;
        pthread_mutex_t m_serverWaitMutex;
        pthread_cond_t m_serverWaitCond;
        bool m_serverStartRequested;
        sp<IBinder> m_serverStarted;
        sp<IBinder> m_strongRef;
};
int run_server(int index, int readypipefd)
{
    binderLibTestServiceName += String16(binderserversuffix);
    status_t ret;
    sp<IServiceManager> sm = defaultServiceManager();
    {
        sp<BinderLibTestService> testService = new BinderLibTestService(index);
        if (index == 0) {
            ret = sm->addService(binderLibTestServiceName, testService);
        } else {
            sp<IBinder> server = sm->getService(binderLibTestServiceName);
            Parcel data, reply;
            data.writeInt32(index);
            data.writeStrongBinder(testService);
            ret = server->transact(BINDER_LIB_TEST_REGISTER_SERVER, data, &reply);
        }
    }
    write(readypipefd, &ret, sizeof(ret));
    close(readypipefd);
    if (ret)
        return 1;
    ProcessState::self()->startThreadPool();
    IPCThreadState::self()->joinThreadPool();
    return 1;
}
int main(int argc, char **argv) {
    int ret;
    if (argc == 4 && !strcmp(argv[1], "--servername")) {
        binderservername = argv[2];
    } else {
        binderservername = argv[0];
    }
    if (argc == 5 && !strcmp(argv[1], binderserverarg)) {
        binderserversuffix = argv[4];
        return run_server(atoi(argv[2]), atoi(argv[3]));
    }
    binderserversuffix = new char[16];
    snprintf(binderserversuffix, 16, "%d", getpid());
    binderLibTestServiceName += String16(binderserversuffix);
    ::testing::InitGoogleTest(&argc, argv);
    binder_env = AddGlobalTestEnvironment(new BinderLibTestEnv());
    ProcessState::self()->startThreadPool();
    return RUN_ALL_TESTS();
}
