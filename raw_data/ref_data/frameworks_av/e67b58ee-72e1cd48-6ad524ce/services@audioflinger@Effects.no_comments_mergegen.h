       
#include "DeviceEffectManager.h"
#include "IAfEffect.h"
#include <android-base/macros.h>
#include <mediautils/Synchronization.h>
#include <private/media/AudioEffectShared.h>
#include <map>
#include <optional>
#include <vector>
namespace android {
class EffectBase : public virtual IAfEffectBase {
public:
    EffectBase(const sp<EffectCallbackInterface>& callback,
               effect_descriptor_t *desc,
               int id,
               audio_session_t sessionId,
               bool pinned);
    int id() const final { return mId; }
    effect_state state() const final {
        return mState;
    }
    audio_session_t sessionId() const final {
        return mSessionId;
    }
    const effect_descriptor_t& desc() const final { return mDescriptor; }
    bool isOffloadable() const final
                        { return (mDescriptor.flags & EFFECT_FLAG_OFFLOAD_SUPPORTED) != 0; }
    bool isImplementationSoftware() const final
                        { return (mDescriptor.flags & EFFECT_FLAG_HW_ACC_MASK) == 0; }
    bool isProcessImplemented() const final
                        { return (mDescriptor.flags & EFFECT_FLAG_NO_PROCESS) == 0; }
    bool isVolumeControl() const
                        { return (mDescriptor.flags & EFFECT_FLAG_VOLUME_MASK)
                            == EFFECT_FLAG_VOLUME_CTRL; }
    bool isVolumeMonitor() const final
                        { return (mDescriptor.flags & EFFECT_FLAG_VOLUME_MASK)
                            == EFFECT_FLAG_VOLUME_MONITOR; }
    status_t setEnabled(bool enabled, bool fromHandle) override EXCLUDES_EffectBase_Mutex;
    status_t setEnabled_l(bool enabled) final REQUIRES(audio_utils::EffectBase_Mutex);
    bool isEnabled() const final;
    void setSuspended(bool suspended) final EXCLUDES_EffectBase_Mutex;
    bool suspended() const final EXCLUDES_EffectBase_Mutex;
    status_t command(int32_t __unused,
                             const std::vector<uint8_t>& __unused,
                             int32_t __unused,
                             std::vector<uint8_t>* __unused) override {
        return NO_ERROR;
    }
    void setCallback(const sp<EffectCallbackInterface>& callback) final {
        mCallback = callback;
    }
    sp<EffectCallbackInterface> getCallback() const final {
        return mCallback.load();
    }
    status_t addHandle(IAfEffectHandle* handle) final EXCLUDES_EffectBase_Mutex;
    ssize_t disconnectHandle(IAfEffectHandle* handle,
                             bool unpinIfLast) final EXCLUDES_EffectBase_Mutex;
    ssize_t removeHandle(IAfEffectHandle* handle) final EXCLUDES_EffectBase_Mutex;
    ssize_t removeHandle_l(IAfEffectHandle* handle) final REQUIRES(audio_utils::EffectBase_Mutex);
    IAfEffectHandle* controlHandle_l() final REQUIRES(audio_utils::EffectBase_Mutex);
    bool purgeHandles() final EXCLUDES_EffectBase_Mutex;
    void checkSuspendOnEffectEnabled(bool enabled, bool threadLocked) final;
    bool isPinned() const final { return mPinned; }
    void unPin() final { mPinned = false; }
    audio_utils::mutex& mutex() const final
            RETURN_CAPABILITY(android::audio_utils::EffectBase_Mutex) {
        return mMutex;
    }
    status_t updatePolicyState() final EXCLUDES_EffectBase_Mutex;
    sp<IAfEffectModule> asEffectModule() override { return nullptr; }
    sp<IAfDeviceEffectProxy> asDeviceEffectProxy() override { return nullptr; }
    void dump(int fd, const Vector<String16>& args) const override;
protected:
    bool isInternal_l() const REQUIRES(audio_utils::EffectBase_Mutex) {
        for (auto handle : mHandles) {
            if (handle->client() != nullptr) {
                return false;
            }
        }
        return true;
    }
    bool mPinned = false;
    DISALLOW_COPY_AND_ASSIGN(EffectBase);
    mutable audio_utils::mutex mMutex{audio_utils::MutexOrder::kEffectBase_Mutex};
    mediautils::atomic_sp<EffectCallbackInterface> mCallback;
    const int mId;
    const audio_session_t mSessionId;
    const effect_descriptor_t mDescriptor;
    effect_state mState = IDLE;
    bool mSuspended = false;
    Vector<IAfEffectHandle *> mHandles;
    audio_utils::mutex& policyMutex() const
            RETURN_CAPABILITY(android::audio_utils::EffectBase_PolicyMutex) {
        return mPolicyMutex;
    }
    mutable audio_utils::mutex mPolicyMutex{audio_utils::MutexOrder::kEffectBase_PolicyMutex};
    bool mPolicyRegistered = false;
    bool mPolicyEnabled = false;
};
class EffectModule : public IAfEffectModule, public EffectBase {
public:
    EffectModule(const sp<EffectCallbackInterface>& callabck,
                    effect_descriptor_t *desc,
                    int id,
                    audio_session_t sessionId,
                    bool pinned,
                    audio_port_handle_t deviceId) REQUIRES(audio_utils::EffectChain_Mutex);
    ~EffectModule() override REQUIRES(audio_utils::EffectChain_Mutex);
    void process() final EXCLUDES_EffectBase_Mutex;
    bool updateState_l() final REQUIRES(audio_utils::EffectChain_Mutex) EXCLUDES_EffectBase_Mutex;
    status_t command(int32_t cmdCode, const std::vector<uint8_t>& cmdData, int32_t maxReplySize,
                     std::vector<uint8_t>* reply) final EXCLUDES_EffectBase_Mutex;
    void reset_l() final REQUIRES(audio_utils::EffectBase_Mutex);
    status_t configure_l() final REQUIRES(audio_utils::EffectChain_Mutex);
    status_t init_l() final REQUIRES(audio_utils::EffectChain_Mutex) EXCLUDES_EffectBase_Mutex;
    uint32_t status() const final {
        return mStatus;
    }
    bool isProcessEnabled() const final;
    bool isOffloadedOrDirect_l() const final REQUIRES(audio_utils::EffectChain_Mutex);
    bool isVolumeControlEnabled_l() const final REQUIRES(audio_utils::EffectChain_Mutex);
    void setInBuffer(const sp<EffectBufferHalInterface>& buffer) final;
    int16_t *inBuffer() const final {
        return mInBuffer != 0 ? reinterpret_cast<int16_t*>(mInBuffer->ptr()) : NULL;
    }
    void setOutBuffer(const sp<EffectBufferHalInterface>& buffer) final;
    int16_t *outBuffer() const final {
        return mOutBuffer != 0 ? reinterpret_cast<int16_t*>(mOutBuffer->ptr()) : NULL;
    }
    void updateAccessMode_l() final REQUIRES(audio_utils::EffectChain_Mutex) {
        if (requiredEffectBufferAccessMode() != mConfig.outputCfg.accessMode) {
            configure_l();
        }
    }
    status_t setDevices(const AudioDeviceTypeAddrVector& devices) final EXCLUDES_EffectBase_Mutex;
    status_t setInputDevice(const AudioDeviceTypeAddr& device) final EXCLUDES_EffectBase_Mutex;
    status_t setVolume_l(uint32_t* left, uint32_t* right, bool controller, bool force) final
            REQUIRES(audio_utils::EffectChain_Mutex);
    status_t setMode(audio_mode_t mode) final EXCLUDES_EffectBase_Mutex;
    status_t setAudioSource(audio_source_t source) final EXCLUDES_EffectBase_Mutex;
    status_t start_l() final REQUIRES(audio_utils::EffectChain_Mutex) EXCLUDES_EffectBase_Mutex;
    status_t stop_l() final REQUIRES(audio_utils::EffectChain_Mutex) EXCLUDES_EffectBase_Mutex;
    status_t setOffloaded_l(bool offloaded, audio_io_handle_t io) final
            REQUIRES(audio_utils::EffectChain_Mutex) EXCLUDES_EffectBase_Mutex;
    bool isOffloaded_l() const final
            REQUIRES(audio_utils::EffectChain_Mutex) EXCLUDES_EffectBase_Mutex;
    void addEffectToHal_l() final REQUIRES(audio_utils::EffectChain_Mutex);
    void release_l(const std::string& from = "") final REQUIRES(audio_utils::EffectChain_Mutex);
    sp<IAfEffectModule> asEffectModule() final { return this; }
    bool isHapticGenerator() const final;
    bool isSpatializer() const final;
    status_t setHapticScale_l(int id, os::HapticScale hapticScale) final
            REQUIRES(audio_utils::EffectChain_Mutex) EXCLUDES_EffectBase_Mutex;
    status_t setVibratorInfo_l(const media::AudioVibratorInfo& vibratorInfo) final
            REQUIRES(audio_utils::EffectChain_Mutex) EXCLUDES_EffectBase_Mutex;
    status_t sendMetadata_ll(const std::vector<playback_track_metadata_v7_t>& metadata) final
            REQUIRES(audio_utils::ThreadBase_Mutex,
                     audio_utils::EffectChain_Mutex) EXCLUDES_EffectBase_Mutex;
    status_t getConfigs_l(audio_config_base_t* inputCfg, audio_config_base_t* outputCfg,
                          bool* isOutput) const final
            REQUIRES(audio_utils::EffectHandle_Mutex) EXCLUDES_EffectBase_Mutex;
    void dump(int fd, const Vector<String16>& args) const final;
private:
    static const uint32_t MAX_DISABLE_TIME_MS = 10000;
    DISALLOW_COPY_AND_ASSIGN(EffectModule);
    status_t start_ll() REQUIRES(audio_utils::EffectChain_Mutex, audio_utils::EffectBase_Mutex);
    status_t stop_ll() REQUIRES(audio_utils::EffectChain_Mutex, audio_utils::EffectBase_Mutex);
    status_t removeEffectFromHal_l() REQUIRES(audio_utils::EffectChain_Mutex);
    status_t sendSetAudioDevicesCommand(const AudioDeviceTypeAddrVector &devices, uint32_t cmdCode);
    effect_buffer_access_e requiredEffectBufferAccessMode() const {
        return mConfig.inputCfg.buffer.raw == mConfig.outputCfg.buffer.raw
                ? EFFECT_BUFFER_ACCESS_WRITE : EFFECT_BUFFER_ACCESS_ACCUMULATE;
    }
    status_t setVolumeInternal(uint32_t* left, uint32_t* right,
                               bool controller );
    effect_config_t mConfig;
    sp<EffectHalInterface> mEffectInterface;
    sp<EffectBufferHalInterface> mInBuffer;
    sp<EffectBufferHalInterface> mOutBuffer;
    status_t mStatus;
    uint32_t mMaxDisableWaitCnt;
    uint32_t mDisableWaitCnt;
    bool mOffloaded;
    audio_io_handle_t mCurrentHalStream = AUDIO_IO_HANDLE_NONE;
    bool mIsOutput;
    bool mSupportsFloat;
    sp<EffectBufferHalInterface> mInConversionBuffer;
    sp<EffectBufferHalInterface> mOutConversionBuffer;
    uint32_t mInChannelCountRequested;
    uint32_t mOutChannelCountRequested;
    template <typename MUTEX>
    class AutoLockReentrant {
    public:
        AutoLockReentrant(MUTEX& mutex, pid_t allowedTid)
            : mMutex(gettid() == allowedTid ? nullptr : &mutex)
        {
            if (mMutex != nullptr) mMutex->lock();
        }
        ~AutoLockReentrant() {
            if (mMutex != nullptr) mMutex->unlock();
        }
    private:
        MUTEX * const mMutex;
    };
    static constexpr pid_t INVALID_PID = (pid_t)-1;
    pid_t mSetVolumeReentrantTid = INVALID_PID;
};
class EffectHandle: public IAfEffectHandle, public android::media::BnEffect {
public:
    EffectHandle(const sp<IAfEffectBase>& effect,
            const sp<Client>& client,
            const sp<media::IEffectClient>& effectClient,
            int32_t priority, bool notifyFramesProcessed);
    ~EffectHandle() override;
    status_t onTransact(
            uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) final;
    status_t initCheck() const final;
    android::binder::Status enable(int32_t* _aidl_return) final;
    android::binder::Status disable(int32_t* _aidl_return) final;
    android::binder::Status command(int32_t cmdCode,
                                    const std::vector<uint8_t>& cmdData,
                                    int32_t maxResponseSize,
                                    std::vector<uint8_t>* response,
                                    int32_t* _aidl_return) final;
    android::binder::Status disconnect() final;
    android::binder::Status getCblk(media::SharedFileRegion* _aidl_return) final;
    android::binder::Status getConfig(media::EffectConfig* _config,
                                      int32_t* _aidl_return) final;
    const sp<Client>& client() const final { return mClient; }
    sp<android::media::IEffect> asIEffect() final {
        return sp<android::media::IEffect>::fromExisting(this);
    }
private:
    void disconnect(bool unpinIfLast);
    void setControl(bool hasControl, bool signal, bool enabled) final;
    void commandExecuted(uint32_t cmdCode,
                         const std::vector<uint8_t>& cmdData,
                         const std::vector<uint8_t>& replyData) final;
    bool enabled() const final { return mEnabled; }
    void setEnabled(bool enabled) final;
    void framesProcessed(int32_t frames) const final;
public:
    wp<IAfEffectBase> effect() const final { return mEffect; }
    int id() const final {
        sp<IAfEffectBase> effect = mEffect.promote();
        if (effect == 0) {
            return 0;
        }
        return effect->id();
    }
private:
    int priority() const final { return mPriority; }
    bool hasControl() const final { return mHasControl; }
    bool disconnected() const final { return mDisconnected; }
    void dumpToBuffer(char* buffer, size_t size) const final;
private:
    DISALLOW_COPY_AND_ASSIGN(EffectHandle);
    audio_utils::mutex& mutex() const RETURN_CAPABILITY(android::audio_utils::EffectHandle_Mutex) {
        return mMutex;
    }
    mutable audio_utils::mutex mMutex{audio_utils::MutexOrder::kEffectHandle_Mutex};
    const wp<IAfEffectBase> mEffect;
    const sp<media::IEffectClient> mEffectClient;
              sp<Client> mClient;
    sp<IMemory> mCblkMemory;
    effect_param_cblk_t* mCblk;
    uint8_t* mBuffer;
    int mPriority;
    bool mHasControl;
    bool mEnabled;
    bool mDisconnected;
    const bool mNotifyFramesProcessed;
};
class EffectChain : public IAfEffectChain {
public:
    EffectChain(const sp<IAfThreadBase>& thread,
                audio_session_t sessionId,
                const sp<IAfThreadCallback>& afThreadCallback);
    void process_l() final REQUIRES(audio_utils::EffectChain_Mutex);
    audio_utils::mutex& mutex() const final RETURN_CAPABILITY(audio_utils::EffectChain_Mutex) {
        return mMutex;
    }
    status_t createEffect(sp<IAfEffectModule>& effect, effect_descriptor_t* desc, int id,
                            audio_session_t sessionId, bool pinned) final
            EXCLUDES_EffectChain_Mutex;
    status_t addEffect(const sp<IAfEffectModule>& handle) final
            EXCLUDES_EffectChain_Mutex;
    status_t addEffect_l(const sp<IAfEffectModule>& handle) final
            REQUIRES(audio_utils::EffectChain_Mutex);
    size_t removeEffect(const sp<IAfEffectModule>& handle, bool release = false) final
            EXCLUDES_EffectChain_Mutex;
    audio_session_t sessionId() const final { return mSessionId; }
    void setSessionId(audio_session_t sessionId) final { mSessionId = sessionId; }
    sp<IAfEffectModule> getEffectFromDesc(effect_descriptor_t* descriptor) const final
            EXCLUDES_EffectChain_Mutex;
    sp<IAfEffectModule> getEffectFromId_l(int id) const final
            REQUIRES(audio_utils::ThreadBase_Mutex) EXCLUDES_EffectChain_Mutex;
    sp<IAfEffectModule> getEffectFromType_l(const effect_uuid_t* type) const final
            REQUIRES(audio_utils::ThreadBase_Mutex) EXCLUDES_EffectChain_Mutex;
    std::vector<int> getEffectIds_l() const final REQUIRES(audio_utils::ThreadBase_Mutex);
    bool setVolume(uint32_t* left, uint32_t* right,
                   bool force = false) final EXCLUDES_EffectChain_Mutex;
    void resetVolume_l() final REQUIRES(audio_utils::EffectChain_Mutex);
    void setDevices_l(const AudioDeviceTypeAddrVector& devices) final
            REQUIRES(audio_utils::ThreadBase_Mutex) EXCLUDES_EffectChain_Mutex;
    void setInputDevice_l(const AudioDeviceTypeAddr& device) final
            REQUIRES(audio_utils::ThreadBase_Mutex) EXCLUDES_EffectChain_Mutex;
    void setMode_l(audio_mode_t mode) final
            REQUIRES(audio_utils::ThreadBase_Mutex) EXCLUDES_EffectChain_Mutex;
    void setAudioSource_l(audio_source_t source) final
            REQUIRES(audio_utils::ThreadBase_Mutex) EXCLUDES_EffectChain_Mutex;
    void setInBuffer(const sp<EffectBufferHalInterface>& buffer) final {
        mInBuffer = buffer;
    }
    float *inBuffer() const final {
        return mInBuffer != 0 ? reinterpret_cast<float*>(mInBuffer->ptr()) : NULL;
    }
    void setOutBuffer(const sp<EffectBufferHalInterface>& buffer) final {
        mOutBuffer = buffer;
    }
    float *outBuffer() const final {
        return mOutBuffer != 0 ? reinterpret_cast<float*>(mOutBuffer->ptr()) : NULL;
    }
    void incTrackCnt() final { android_atomic_inc(&mTrackCnt); }
    void decTrackCnt() final { android_atomic_dec(&mTrackCnt); }
    int32_t trackCnt() const final { return android_atomic_acquire_load(&mTrackCnt); }
    void incActiveTrackCnt() final { android_atomic_inc(&mActiveTrackCnt);
                               mTailBufferCount = mMaxTailBuffers; }
    void decActiveTrackCnt() final { android_atomic_dec(&mActiveTrackCnt); }
    int32_t activeTrackCnt() const final {
        return android_atomic_acquire_load(&mActiveTrackCnt);
    }
    product_strategy_t strategy() const final { return mStrategy; }
    void setStrategy(product_strategy_t strategy) final
            { mStrategy = strategy; }
    void setEffectSuspended_l(const effect_uuid_t* type, bool suspend) final
            REQUIRES(audio_utils::ThreadBase_Mutex);
    void setEffectSuspendedAll_l(bool suspend) final REQUIRES(audio_utils::ThreadBase_Mutex);
    void checkSuspendOnEffectEnabled_l(const sp<IAfEffectModule>& effect, bool enabled) final
            REQUIRES(audio_utils::ThreadBase_Mutex);
    void clearInputBuffer() final EXCLUDES_EffectChain_Mutex;
    bool isNonOffloadableEnabled() const final EXCLUDES_EffectChain_Mutex;
    bool isNonOffloadableEnabled_l() const final REQUIRES(audio_utils::EffectChain_Mutex);
    void syncHalEffectsState_l()
            REQUIRES(audio_utils::ThreadBase_Mutex) EXCLUDES_EffectChain_Mutex final;
    void checkOutputFlagCompatibility(audio_output_flags_t *flags) const final;
    void checkInputFlagCompatibility(audio_input_flags_t *flags) const final;
    bool isRawCompatible() const final;
    bool isFastCompatible() const final;
    bool isBitPerfectCompatible() const final;
    bool isCompatibleWithThread_l(const sp<IAfThreadBase>& thread) const final
            REQUIRES(audio_utils::ThreadBase_Mutex) EXCLUDES_EffectChain_Mutex;
    bool containsHapticGeneratingEffect() final
            EXCLUDES_EffectChain_Mutex;
    bool containsHapticGeneratingEffect_l() final
            REQUIRES(audio_utils::EffectChain_Mutex);
    void setHapticScale_l(int id, os::HapticScale hapticScale) final
            REQUIRES(audio_utils::ThreadBase_Mutex) EXCLUDES_EffectChain_Mutex;
    sp<EffectCallbackInterface> effectCallback() const final { return mEffectCallback; }
    wp<IAfThreadBase> thread() const final { return mEffectCallback->thread(); }
    bool isFirstEffect_l(int id) const final REQUIRES(audio_utils::EffectChain_Mutex) {
        return !mEffects.isEmpty() && id == mEffects[0]->id();
    }
    void dump(int fd, const Vector<String16>& args) const final;
    size_t numberOfEffects() const final {
      audio_utils::lock_guard _l(mutex());
      return mEffects.size();
    }
    sp<IAfEffectModule> getEffectModule(size_t index) const final {
        audio_utils::lock_guard _l(mutex());
        return mEffects[index];
    }
    void sendMetadata_l(const std::vector<playback_track_metadata_v7_t>& allMetadata,
        const std::optional<const std::vector<playback_track_metadata_v7_t>> spatializedMetadata)
            final REQUIRES(audio_utils::ThreadBase_Mutex);
    void setThread(const sp<IAfThreadBase>& thread) final EXCLUDES_EffectChain_Mutex;
  private:
    bool setVolume_l(uint32_t* left, uint32_t* right, bool force = false)
            REQUIRES(audio_utils::EffectChain_Mutex);
    class EffectCallback : public EffectCallbackInterface {
    public:
        EffectCallback(const wp<EffectChain>& owner,
                const sp<IAfThreadBase>& thread,
                const sp<IAfThreadCallback>& afThreadCallback)
            : mChain(owner)
            , mThread(thread), mAfThreadCallback(afThreadCallback) {
            if (thread != nullptr) {
                mThreadType = thread->type();
            }
        }
        status_t createEffectHal(const effect_uuid_t *pEffectUuid,
               int32_t sessionId, int32_t deviceId, sp<EffectHalInterface> *effect) override;
        status_t allocateHalBuffer(size_t size, sp<EffectBufferHalInterface>* buffer) override;
        bool updateOrphanEffectChains(const sp<IAfEffectBase>& effect) override;
        audio_io_handle_t io() const override;
        bool isOutput() const override;
        bool isOffload() const override;
        bool isOffloadOrDirect() const override;
        bool isOffloadOrMmap() const override;
        bool isSpatializer() const override;
        uint32_t sampleRate() const override;
        audio_channel_mask_t inChannelMask(int id) const override;
        uint32_t inChannelCount(int id) const override;
        audio_channel_mask_t outChannelMask() const override;
        uint32_t outChannelCount() const override;
        audio_channel_mask_t hapticChannelMask() const override;
        size_t frameCount() const override;
        uint32_t latency() const override;
        status_t addEffectToHal(const sp<EffectHalInterface>& effect) override;
        status_t removeEffectFromHal(const sp<EffectHalInterface>& effect) override;
        bool disconnectEffectHandle(IAfEffectHandle *handle, bool unpinIfLast) override;
        void setVolumeForOutput(float left, float right) const override;
        void checkSuspendOnEffectEnabled(const sp<IAfEffectBase>& effect, bool enabled,
                                         bool threadLocked) override;
        void resetVolume_l() override
                REQUIRES(audio_utils::ThreadBase_Mutex, audio_utils::EffectChain_Mutex);
        product_strategy_t strategy() const override;
        int32_t activeTrackCnt() const override;
        void onEffectEnable(const sp<IAfEffectBase>& effect) override;
        void onEffectDisable(const sp<IAfEffectBase>& effect) override;
        wp<IAfEffectChain> chain() const final { return mChain; }
        bool isAudioPolicyReady() const final {
            if (mAfThreadCallback == nullptr) {
                return false;
            }
            return mAfThreadCallback->isAudioPolicyReady();
        }
        wp<IAfThreadBase> thread() const { return mThread.load(); }
        void setThread(const sp<IAfThreadBase>& thread) {
            mThread = thread;
            if (thread != nullptr) {
                mThreadType = thread->type();
                mAfThreadCallback = thread->afThreadCallback();
            }
        }
        bool hasThreadAttached() const {
            return thread().promote() != nullptr;
        }
    private:
        const wp<IAfEffectChain> mChain;
        mediautils::atomic_wp<IAfThreadBase> mThread;
        sp<IAfThreadCallback> mAfThreadCallback;
        IAfThreadBase::type_t mThreadType = IAfThreadBase::MIXER;
    };
    DISALLOW_COPY_AND_ASSIGN(EffectChain);
    class SuspendedEffectDesc : public RefBase {
    public:
        SuspendedEffectDesc() : mRefCount(0) {}
        int mRefCount;
        effect_uuid_t mType;
        wp<IAfEffectModule> mEffect;
    };
    void getSuspendEligibleEffects(Vector<sp<IAfEffectModule>>& effects)
            EXCLUDES_EffectChain_Mutex;
    sp<IAfEffectModule> getEffectIfEnabled_l(const effect_uuid_t* type)
            REQUIRES(audio_utils::ThreadBase_Mutex);
    bool isEffectEligibleForSuspend(const effect_descriptor_t& desc);
    static bool isEffectEligibleForBtNrecSuspend_l(const effect_uuid_t* type)
            REQUIRES(audio_utils::ThreadBase_Mutex);
    void clearInputBuffer_l() REQUIRES(audio_utils::EffectChain_Mutex);
    bool hasVolumeControlEnabled_l() const REQUIRES(audio_utils::EffectChain_Mutex);
    void setVolumeForOutput_l(uint32_t left, uint32_t right)
            REQUIRES(audio_utils::EffectChain_Mutex);
    ssize_t getInsertIndex_l(const effect_descriptor_t& desc)
            REQUIRES(audio_utils::EffectChain_Mutex);
    std::optional<size_t> findVolumeControl_l(size_t from, size_t to) const
            REQUIRES(audio_utils::EffectChain_Mutex);
    mutable audio_utils::mutex mMutex{audio_utils::MutexOrder::kEffectChain_Mutex};
             Vector<sp<IAfEffectModule>> mEffects GUARDED_BY(mutex());
             audio_session_t mSessionId;
             sp<EffectBufferHalInterface> mInBuffer;
             sp<EffectBufferHalInterface> mOutBuffer;
    volatile int32_t mActiveTrackCnt;
    volatile int32_t mTrackCnt;
             int32_t mTailBufferCount;
             int32_t mMaxTailBuffers;
             uint32_t mLeftVolume;
             uint32_t mRightVolume;
             uint32_t mNewLeftVolume;
             uint32_t mNewRightVolume;
             product_strategy_t mStrategy = PRODUCT_STRATEGY_NONE;
             KeyedVector< int, sp<SuspendedEffectDesc> > mSuspendedEffects;
             const sp<EffectCallback> mEffectCallback;
             wp<IAfEffectModule> mVolumeControlEffect;
};
class DeviceEffectProxy : public IAfDeviceEffectProxy, public EffectBase {
public:
    DeviceEffectProxy(const AudioDeviceTypeAddr& device,
            const sp<DeviceEffectManagerCallback>& callback,
                effect_descriptor_t *desc, int id, bool notifyFramesProcessed)
            : EffectBase(callback, desc, id, AUDIO_SESSION_DEVICE, false),
                mDevice(device), mManagerCallback(callback),
                mMyCallback(new ProxyCallback(wp<DeviceEffectProxy>(this), callback)),
                mNotifyFramesProcessed(notifyFramesProcessed) {}
    status_t setEnabled(bool enabled, bool fromHandle) final;
    sp<IAfDeviceEffectProxy> asDeviceEffectProxy() final { return this; }
    status_t init_l(const std::map<audio_patch_handle_t, IAfPatchPanel::Patch>& patches) final
            REQUIRES(audio_utils::DeviceEffectManager_Mutex) EXCLUDES_EffectBase_Mutex;
    status_t onCreatePatch(audio_patch_handle_t patchHandle,
                           const IAfPatchPanel::Patch& patch) final;
    status_t onUpdatePatch(audio_patch_handle_t oldPatchHandle, audio_patch_handle_t newPatchHandle,
           const IAfPatchPanel::Patch& patch) final;
    void onReleasePatch(audio_patch_handle_t patchHandle) final;
    size_t removeEffect(const sp<IAfEffectModule>& effect) final;
    status_t addEffectToHal(const sp<EffectHalInterface>& effect) final;
    status_t removeEffectFromHal(const sp<EffectHalInterface>& effect) final;
    const AudioDeviceTypeAddr& device() const final { return mDevice; };
    bool isOutput() const final;
    uint32_t sampleRate() const final;
    audio_channel_mask_t channelMask() const final;
    uint32_t channelCount() const final;
    status_t command(int32_t cmdCode, const std::vector<uint8_t>& cmdData, int32_t maxReplySize,
                     std::vector<uint8_t>* reply) final EXCLUDES_DeviceEffectProxy_ProxyMutex;
    void dump2(int fd, int spaces) const final;
private:
    class ProxyCallback : public EffectCallbackInterface {
    public:
        ProxyCallback(const wp<DeviceEffectProxy>& owner,
                const sp<DeviceEffectManagerCallback>& callback)
            : mProxy(owner), mManagerCallback(callback) {}
        status_t createEffectHal(const effect_uuid_t *pEffectUuid,
               int32_t sessionId, int32_t deviceId, sp<EffectHalInterface> *effect) override;
        status_t allocateHalBuffer(size_t size __unused,
                sp<EffectBufferHalInterface>* buffer __unused) override { return NO_ERROR; }
        bool updateOrphanEffectChains(const sp<IAfEffectBase>& effect __unused) override {
                    return false;
        }
        audio_io_handle_t io() const override { return AUDIO_IO_HANDLE_NONE; }
        bool isOutput() const override;
        bool isOffload() const override { return false; }
        bool isOffloadOrDirect() const override { return false; }
        bool isOffloadOrMmap() const override { return false; }
        bool isSpatializer() const override { return false; }
        uint32_t sampleRate() const override;
        audio_channel_mask_t inChannelMask(int id) const override;
        uint32_t inChannelCount(int id) const override;
        audio_channel_mask_t outChannelMask() const override;
        uint32_t outChannelCount() const override;
        audio_channel_mask_t hapticChannelMask() const override { return AUDIO_CHANNEL_NONE; }
        size_t frameCount() const override { return 0; }
        uint32_t latency() const override { return 0; }
        status_t addEffectToHal(const sp<EffectHalInterface>& effect) override;
        status_t removeEffectFromHal(const sp<EffectHalInterface>& effect) override;
        bool disconnectEffectHandle(IAfEffectHandle *handle, bool unpinIfLast) override;
        void setVolumeForOutput(float left __unused, float right __unused) const override {}
        void checkSuspendOnEffectEnabled(const sp<IAfEffectBase>& effect __unused,
                              bool enabled __unused, bool threadLocked __unused) override {}
        void resetVolume_l() override REQUIRES(audio_utils::EffectChain_Mutex) {}
        product_strategy_t strategy() const override { return PRODUCT_STRATEGY_NONE; }
        int32_t activeTrackCnt() const override { return 0; }
        void onEffectEnable(const sp<IAfEffectBase>& effect __unused) override;
        void onEffectDisable(const sp<IAfEffectBase>& effect __unused) override;
        wp<IAfEffectChain> chain() const override { return nullptr; }
        bool isAudioPolicyReady() const override {
            return mManagerCallback->isAudioPolicyReady();
        }
        int newEffectId();
    private:
        const wp<DeviceEffectProxy> mProxy;
        const sp<DeviceEffectManagerCallback> mManagerCallback;
    };
    status_t checkPort(const IAfPatchPanel::Patch& patch,
            const struct audio_port_config* port, sp<IAfEffectHandle>* handle);
    const AudioDeviceTypeAddr mDevice;
    const sp<DeviceEffectManagerCallback> mManagerCallback;
    const sp<ProxyCallback> mMyCallback;
    audio_utils::mutex& proxyMutex() const
            RETURN_CAPABILITY(android::audio_utils::DeviceEffectProxy_ProxyMutex) {
        return mProxyMutex;
    }
    mutable audio_utils::mutex mProxyMutex{
            audio_utils::MutexOrder::kDeviceEffectProxy_ProxyMutex};
    std::map<audio_patch_handle_t, sp<IAfEffectHandle>> mEffectHandles;
    sp<IAfEffectModule> mHalEffect;
    struct audio_port_config mDevicePort = { .id = AUDIO_PORT_HANDLE_NONE };
    const bool mNotifyFramesProcessed;
};
}
