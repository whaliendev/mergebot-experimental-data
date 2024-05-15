#ifndef INCLUDING_FROM_AUDIOFLINGER_H
#error This header file should only be included from AudioFlinger.h
#endif
class ThreadBase : public Thread {
 public:
  enum type_t {
    MIXER,
    DIRECT,
    DUPLICATING,
    RECORD,
    OFFLOAD,
    MMAP
  };
  static const char* threadTypeToString(type_t type);
  ThreadBase(const sp<AudioFlinger>& audioFlinger, audio_io_handle_t id,
             type_t type, bool systemReady);
  virtual ~ThreadBase();
  virtual status_t readyToRun();
  void clearPowerManager();
  enum {
    CFG_EVENT_IO,
    CFG_EVENT_PRIO,
    CFG_EVENT_SET_PARAMETER,
    CFG_EVENT_CREATE_AUDIO_PATCH,
    CFG_EVENT_RELEASE_AUDIO_PATCH,
    CFG_EVENT_UPDATE_OUT_DEVICE,
  };
  class ConfigEventData : public RefBase {
   public:
    virtual ~ConfigEventData() {}
    virtual void dump(char* buffer, size_t size) = 0;
   protected:
    ConfigEventData() {}
  };
  class ConfigEvent : public RefBase {
   public:
    virtual ~ConfigEvent() {}
    void dump(char* buffer, size_t size) { mData->dump(buffer, size); }
    const int mType;
    Mutex mLock;
    Condition mCond;
    status_t mStatus;
    bool mWaitStatus;
    bool
        mRequiresSystemReady;
    sp<ConfigEventData> mData;
   protected:
    explicit ConfigEvent(int type, bool requiresSystemReady = false)
        : mType(type),
          mStatus(NO_ERROR),
          mWaitStatus(false),
          mRequiresSystemReady(requiresSystemReady),
          mData(NULL) {}
  };
  class IoConfigEventData : public ConfigEventData {
   public:
    IoConfigEventData(audio_io_config_event event, pid_t pid,
                      audio_port_handle_t portId)
        : mEvent(event), mPid(pid), mPortId(portId) {}
    virtual void dump(char* buffer, size_t size) {
      snprintf(buffer, size, "IO event: event %d\n", mEvent);
    }
    const audio_io_config_event mEvent;
    const pid_t mPid;
    const audio_port_handle_t mPortId;
  };
  class IoConfigEvent : public ConfigEvent {
   public:
    IoConfigEvent(audio_io_config_event event, pid_t pid,
                  audio_port_handle_t portId)
        : ConfigEvent(CFG_EVENT_IO) {
      mData = new IoConfigEventData(event, pid, portId);
    }
    virtual ~IoConfigEvent() {}
  };
  class PrioConfigEventData : public ConfigEventData {
   public:
    PrioConfigEventData(pid_t pid, pid_t tid, int32_t prio, bool forApp)
        : mPid(pid), mTid(tid), mPrio(prio), mForApp(forApp) {}
    virtual void dump(char* buffer, size_t size) {
      snprintf(buffer, size,
               "Prio event: pid %d, tid %d, prio %d, for app? %d\n", mPid, mTid,
               mPrio, mForApp);
    }
    const pid_t mPid;
    const pid_t mTid;
    const int32_t mPrio;
    const bool mForApp;
  };
  class PrioConfigEvent : public ConfigEvent {
   public:
    PrioConfigEvent(pid_t pid, pid_t tid, int32_t prio, bool forApp)
        : ConfigEvent(CFG_EVENT_PRIO, true) {
      mData = new PrioConfigEventData(pid, tid, prio, forApp);
    }
    virtual ~PrioConfigEvent() {}
  };
  class SetParameterConfigEventData : public ConfigEventData {
   public:
    explicit SetParameterConfigEventData(String8 keyValuePairs)
        : mKeyValuePairs(keyValuePairs) {}
    virtual void dump(char* buffer, size_t size) {
      snprintf(buffer, size, "KeyValue: %s\n", mKeyValuePairs.string());
    }
    const String8 mKeyValuePairs;
  };
  class SetParameterConfigEvent : public ConfigEvent {
   public:
    explicit SetParameterConfigEvent(String8 keyValuePairs)
        : ConfigEvent(CFG_EVENT_SET_PARAMETER) {
      mData = new SetParameterConfigEventData(keyValuePairs);
      mWaitStatus = true;
    }
    virtual ~SetParameterConfigEvent() {}
  };
  class CreateAudioPatchConfigEventData : public ConfigEventData {
   public:
    CreateAudioPatchConfigEventData(const struct audio_patch patch,
                                    audio_patch_handle_t handle)
        : mPatch(patch), mHandle(handle) {}
    virtual void dump(char* buffer, size_t size) {
      snprintf(buffer, size, "Patch handle: %u\n", mHandle);
    }
    const struct audio_patch mPatch;
    audio_patch_handle_t mHandle;
  };
  class CreateAudioPatchConfigEvent : public ConfigEvent {
   public:
    CreateAudioPatchConfigEvent(const struct audio_patch patch,
                                audio_patch_handle_t handle)
        : ConfigEvent(CFG_EVENT_CREATE_AUDIO_PATCH) {
      mData = new CreateAudioPatchConfigEventData(patch, handle);
      mWaitStatus = true;
    }
    virtual ~CreateAudioPatchConfigEvent() {}
  };
  class ReleaseAudioPatchConfigEventData : public ConfigEventData {
   public:
    explicit ReleaseAudioPatchConfigEventData(const audio_patch_handle_t handle)
        : mHandle(handle) {}
    virtual void dump(char* buffer, size_t size) {
      snprintf(buffer, size, "Patch handle: %u\n", mHandle);
    }
    audio_patch_handle_t mHandle;
  };
  class ReleaseAudioPatchConfigEvent : public ConfigEvent {
   public:
    explicit ReleaseAudioPatchConfigEvent(const audio_patch_handle_t handle)
        : ConfigEvent(CFG_EVENT_RELEASE_AUDIO_PATCH) {
      mData = new ReleaseAudioPatchConfigEventData(handle);
      mWaitStatus = true;
    }
    virtual ~ReleaseAudioPatchConfigEvent() {}
  };
  class UpdateOutDevicesConfigEventData : public ConfigEventData {
   public:
    explicit UpdateOutDevicesConfigEventData(
        const DeviceDescriptorBaseVector& outDevices)
        : mOutDevices(outDevices) {}
    virtual void dump(char* buffer, size_t size) {
      snprintf(buffer, size, "Devices: %s",
               android::toString(mOutDevices).c_str());
    }
    DeviceDescriptorBaseVector mOutDevices;
  };
  class UpdateOutDevicesConfigEvent : public ConfigEvent {
   public:
    explicit UpdateOutDevicesConfigEvent(
        const DeviceDescriptorBaseVector& outDevices)
        : ConfigEvent(CFG_EVENT_UPDATE_OUT_DEVICE) {
      mData = new UpdateOutDevicesConfigEventData(outDevices);
    }
    virtual ~UpdateOutDevicesConfigEvent();
  };
  class PMDeathRecipient : public IBinder::DeathRecipient {
   public:
    explicit PMDeathRecipient(const wp<ThreadBase>& thread) : mThread(thread) {}
    virtual ~PMDeathRecipient() {}
    virtual void binderDied(const wp<IBinder>& who);
   private:
    DISALLOW_COPY_AND_ASSIGN(PMDeathRecipient);
    wp<ThreadBase> mThread;
  };
  virtual status_t initCheck() const = 0;
  type_t type() const { return mType; }
  bool isDuplicating() const { return (mType == DUPLICATING); }
  audio_io_handle_t id() const { return mId; }
  uint32_t sampleRate() const { return mSampleRate; }
  audio_channel_mask_t channelMask() const { return mChannelMask; }
  audio_format_t format() const { return mHALFormat; }
  uint32_t channelCount() const { return mChannelCount; }
  virtual size_t frameCount() const = 0;
  size_t frameCountHAL() const { return mFrameCount; }
  size_t frameSize() const { return mFrameSize; }
  void exit();
  virtual bool checkForNewParameter_l(const String8& keyValuePair,
                                      status_t& status) = 0;
  virtual status_t setParameters(const String8& keyValuePairs);
  virtual String8 getParameters(const String8& keys) = 0;
  virtual void ioConfigChanged(
      audio_io_config_event event, pid_t pid = 0,
      audio_port_handle_t portId = AUDIO_PORT_HANDLE_NONE) = 0;
  status_t sendConfigEvent_l(sp<ConfigEvent>& event);
  void sendIoConfigEvent(audio_io_config_event event, pid_t pid = 0,
                         audio_port_handle_t portId = AUDIO_PORT_HANDLE_NONE);
  void sendIoConfigEvent_l(audio_io_config_event event, pid_t pid = 0,
                           audio_port_handle_t portId = AUDIO_PORT_HANDLE_NONE);
  void sendPrioConfigEvent(pid_t pid, pid_t tid, int32_t prio, bool forApp);
  void sendPrioConfigEvent_l(pid_t pid, pid_t tid, int32_t prio, bool forApp);
  status_t sendSetParameterConfigEvent_l(const String8& keyValuePair);
  status_t sendCreateAudioPatchConfigEvent(const struct audio_patch* patch,
                                           audio_patch_handle_t* handle);
  status_t sendReleaseAudioPatchConfigEvent(audio_patch_handle_t handle);
  status_t sendUpdateOutDeviceConfigEvent(
      const DeviceDescriptorBaseVector& outDevices);
  void processConfigEvents_l();
  virtual void cacheParameters_l() = 0;
  virtual status_t createAudioPatch_l(const struct audio_patch* patch,
                                      audio_patch_handle_t* handle) = 0;
  virtual status_t releaseAudioPatch_l(const audio_patch_handle_t handle) = 0;
  virtual void updateOutDevices(const DeviceDescriptorBaseVector& outDevices);
  virtual void toAudioPortConfig(struct audio_port_config* config) = 0;
  bool standby() const { return mStandby; }
  const DeviceTypeSet outDeviceTypes() const {
    return getAudioDeviceTypes(mOutDeviceTypeAddrs);
  }
  audio_devices_t inDevice() const { return mInDeviceTypeAddr.mType; }
  DeviceTypeSet getDeviceTypes() const {
    return isOutput() ? outDeviceTypes() : DeviceTypeSet({inDeviceType()});
  }
  const AudioDeviceTypeAddrVector& outDeviceTypeAddrs() const {
    return mOutDeviceTypeAddrs;
  }
  const AudioDeviceTypeAddr& inDeviceTypeAddr() const {
    return mInDeviceTypeAddr;
  }
  virtual bool isOutput() const = 0;
  virtual sp<StreamHalInterface> stream() const = 0;
  sp<EffectHandle> createEffect_l(const sp<AudioFlinger::Client>& client,
                                  const sp<IEffectClient>& effectClient,
                                  int32_t priority, audio_session_t sessionId,
                                  effect_descriptor_t* desc, int* enabled,
                                  status_t* status , bool pinned);
  enum effect_state {
    EFFECT_SESSION = 0x1,
    TRACK_SESSION = 0x2,
    FAST_SESSION = 0x4
  };
  sp<EffectChain> getEffectChain(audio_session_t sessionId);
  sp<EffectChain> getEffectChain_l(audio_session_t sessionId) const;
  std::vector<int> getEffectIds_l(audio_session_t sessionId);
  virtual status_t addEffectChain_l(const sp<EffectChain>& chain) = 0;
  virtual size_t removeEffectChain_l(const sp<EffectChain>& chain) = 0;
  void lockEffectChains_l(Vector<sp<EffectChain>>& effectChains);
  void unlockEffectChains(const Vector<sp<EffectChain>>& effectChains);
  Vector<sp<EffectChain>> getEffectChains_l() const { return mEffectChains; };
  void setMode(audio_mode_t mode);
  sp<AudioFlinger::EffectModule> getEffect(audio_session_t sessionId,
                                           int effectId);
  sp<AudioFlinger::EffectModule> getEffect_l(audio_session_t sessionId,
                                             int effectId);
  status_t addEffect_l(const sp<EffectModule>& effect);
  void removeEffect_l(const sp<EffectModule>& effect, bool release = false);
  void disconnectEffectHandle(EffectHandle* handle, bool unpinIfLast);
  virtual void detachAuxEffect_l(int effectId __unused) {}
  virtual uint32_t hasAudioSession_l(audio_session_t sessionId) const = 0;
  uint32_t hasAudioSession(audio_session_t sessionId) const {
    Mutex::Autolock _l(mLock);
    return hasAudioSession_l(sessionId);
  }
  template <typename T>
  uint32_t hasAudioSession_l(audio_session_t sessionId, const T& tracks) const {
    uint32_t result = 0;
    if (getEffectChain_l(sessionId) != 0) {
      result = EFFECT_SESSION;
    }
    for (size_t i = 0; i < tracks.size(); ++i) {
      const sp<TrackBase>& track = tracks[i];
      if (sessionId == track->sessionId() &&
          !track->isInvalid()
          && !track->isTerminated()) {
        result |= TRACK_SESSION;
        if (track->isFastTrack()) {
          result |= FAST_SESSION;
        }
        break;
      }
    }
    return result;
  }
  virtual uint32_t getStrategyForSession_l(audio_session_t sessionId __unused) {
    return 0;
  }
  void checkSuspendOnEffectEnabled(
      const sp<EffectModule>& effect, bool enabled,
      audio_session_t sessionId = AUDIO_SESSION_OUTPUT_MIX);
  void checkSuspendOnEffectEnabled_l(
      const sp<EffectModule>& effect, bool enabled,
      audio_session_t sessionId = AUDIO_SESSION_OUTPUT_MIX);
  virtual status_t setSyncEvent(const sp<SyncEvent>& event) = 0;
  virtual bool isValidSyncEvent(const sp<SyncEvent>& event) const = 0;
  virtual sp<MemoryDealer> readOnlyHeap() const { return 0; }
  virtual sp<IMemory> pipeMemory() const { return 0; }
  void systemReady();
  virtual status_t checkEffectCompatibility_l(const effect_descriptor_t* desc,
                                              audio_session_t sessionId) = 0;
  void broadcast_l();
  virtual bool isTimestampCorrectionEnabled() const { return false; }
  bool isMsdDevice() const { return mIsMsdDevice; }
  void dump(int fd, const Vector<String16>& args);
  void sendStatistics(bool force);
  mutable Mutex mLock;
 protected:
  class SuspendedSessionDesc : public RefBase {
   public:
    SuspendedSessionDesc() : mRefCount(0) {}
    int mRefCount;
    effect_uuid_t mType;
  };
  void acquireWakeLock();
  virtual void acquireWakeLock_l();
  void releaseWakeLock();
  void releaseWakeLock_l();
  void updateWakeLockUids_l(const SortedVector<uid_t>& uids);
  void getPowerManager_l();
  void setEffectSuspended_l(const effect_uuid_t* type, bool suspend,
                            audio_session_t sessionId);
  void updateSuspendedSessions_l(const effect_uuid_t* type, bool suspend,
                                 audio_session_t sessionId);
  void checkSuspendOnAddEffectChain_l(const sp<EffectChain>& chain);
  virtual void updateMetadata_l() = 0;
  String16 getWakeLockTag();
  virtual void preExit() {}
  virtual void setMasterMono_l(bool mono __unused) {}
  virtual bool requireMonoBlend() { return false; }
  virtual status_t threadloop_getHalTimestamp_l(
      ExtendedTimestamp* timestamp __unused) const {
    return INVALID_OPERATION;
  }
  virtual void dumpInternals_l(int fd __unused,
                               const Vector<String16>& args __unused) {}
  virtual void dumpTracks_l(int fd __unused,
                            const Vector<String16>& args __unused) {}
 private:
  friend class AudioFlinger;
 protected:
  const type_t mType;
  Condition mWaitWorkCV;
  const sp<AudioFlinger> mAudioFlinger;
  uint32_t mSampleRate;
  size_t mFrameCount;
  audio_channel_mask_t mChannelMask;
  uint32_t mChannelCount;
  size_t mFrameSize;
  audio_format_t mFormat;
  audio_format_t mHALFormat;
  size_t mBufferSize;
  AudioDeviceTypeAddrVector
      mOutDeviceTypeAddrs;
  AudioDeviceTypeAddr mInDeviceTypeAddr;
  Vector<sp<ConfigEvent>> mConfigEvents;
  Vector<sp<ConfigEvent>> mPendingConfigEvents;
  bool mStandby;
  struct audio_patch mPatch;
  audio_source_t mAudioSource;
  const audio_io_handle_t mId;
  Vector<sp<EffectChain>> mEffectChains;
  static const int kThreadNameLength = 16;
  char mThreadName[kThreadNameLength];
  sp<IPowerManager> mPowerManager;
  sp<IBinder> mWakeLockToken;
  const sp<PMDeathRecipient> mDeathRecipient;
  KeyedVector<audio_session_t, KeyedVector<int, sp<SuspendedSessionDesc>>>
      mSuspendedSessions;
  static const size_t kLogSize = 4 * 1024;
  sp<NBLog::Writer> mNBLogWriter;
  bool mSystemReady;
  ExtendedTimestamp mTimestamp;
  TimestampVerifier<
      int64_t , int64_t >
      mTimestampVerifier;
  audio_devices_t mTimestampCorrectedDevice = AUDIO_DEVICE_NONE;
  int64_t mLastIoBeginNs = -1;
  int64_t mLastIoEndNs = -1;
  audio_utils::Statistics<double> mIoJitterMs{0.995 };
  audio_utils::Statistics<double> mProcessTimeMs{0.995 };
  audio_utils::Statistics<double> mLatencyMs{0.995 };
  int64_t mLastRecordedTimestampVerifierN = 0;
  int64_t mLastRecordedTimeNs = 0;
  bool mIsMsdDevice = false;
  bool mSignalPending;
#ifdef TEE_SINK
  NBAIO_Tee mTee;
#endif
  template <typename T>
  class ActiveTracks {
   public:
    explicit ActiveTracks(SimpleLog* localLog = nullptr)
        : mActiveTracksGeneration(0),
          mLastActiveTracksGeneration(0),
          mLocalLog(localLog) {}
    ~ActiveTracks() {
      ALOGW_IF(!mActiveTracks.isEmpty(),
               "ActiveTracks should be empty in destructor");
    }
    sp<T> getLatest() { return mLatestActiveTrack.promote(); }
    ssize_t add(const sp<T>& track);
    ssize_t remove(const sp<T>& track);
    size_t size() const { return mActiveTracks.size(); }
    bool isEmpty() const { return mActiveTracks.isEmpty(); }
    ssize_t indexOf(const sp<T>& item) { return mActiveTracks.indexOf(item); }
    sp<T> operator[](size_t index) const { return mActiveTracks[index]; }
    typename SortedVector<sp<T>>::iterator begin() {
      return mActiveTracks.begin();
    }
    typename SortedVector<sp<T>>::iterator end() { return mActiveTracks.end(); }
    void clear();
    void updatePowerState(sp<ThreadBase> thread, bool force = false);
    bool readAndClearHasChanged();
   private:
    void logTrack(const char* funcName, const sp<T>& track) const;
    SortedVector<uid_t> getWakeLockUids() {
      SortedVector<uid_t> wakeLockUids;
      for (const sp<T>& track : mActiveTracks) {
        wakeLockUids.add(track->uid());
      }
      return wakeLockUids;
    }
    std::map<uid_t, std::pair<ssize_t , ssize_t >>
        mBatteryCounter;
    SortedVector<sp<T>> mActiveTracks;
    int mActiveTracksGeneration;
    int mLastActiveTracksGeneration;
    wp<T> mLatestActiveTrack;
    SimpleLog* const mLocalLog;
    bool mHasChanged = false;
  };
  SimpleLog mLocalLog;
 private:
  void dumpBase_l(int fd, const Vector<String16>& args);
  void dumpEffectChains_l(int fd, const Vector<String16>& args);
};
class VolumeInterface {
 public:
  virtual ~VolumeInterface() {}
  virtual void setMasterVolume(float value) = 0;
  virtual void setMasterMute(bool muted) = 0;
  virtual void setStreamVolume(audio_stream_type_t stream, float value) = 0;
  virtual void setStreamMute(audio_stream_type_t stream, bool muted) = 0;
  virtual float streamVolume(audio_stream_type_t stream) const = 0;
};
class PlaybackThread : public ThreadBase,
                       public StreamOutHalInterfaceCallback,
                       public VolumeInterface {
 public:
  enum mixer_state {
    MIXER_IDLE,
    MIXER_TRACKS_ENABLED,
    MIXER_TRACKS_READY,
    MIXER_DRAIN_TRACK,
    MIXER_DRAIN_ALL,
  };
  static const int8_t kMaxTrackRetriesOffload = 20;
  static const int8_t kMaxTrackStartupRetriesOffload = 100;
  static const int8_t kMaxTrackStopRetriesOffload = 2;
  static constexpr uint32_t kMaxTracksPerUid = 40;
  static constexpr size_t kMaxTracks = 256;
  static const nsecs_t kMaxNextBufferDelayNs = 100000000;
  PlaybackThread(const sp<AudioFlinger>& audioFlinger, AudioStreamOut* output,
                 audio_io_handle_t id, type_t type, bool systemReady);
  virtual ~PlaybackThread();
  virtual bool threadLoop();
  virtual void onFirstRef();
  virtual status_t checkEffectCompatibility_l(const effect_descriptor_t* desc,
                                              audio_session_t sessionId);
 protected:
  virtual void threadLoop_mix() = 0;
  virtual void threadLoop_sleepTime() = 0;
  virtual ssize_t threadLoop_write();
  virtual void threadLoop_drain();
  virtual void threadLoop_standby();
  virtual void threadLoop_exit();
  virtual void threadLoop_removeTracks(const Vector<sp<Track>>& tracksToRemove);
  virtual mixer_state prepareTracks_l(Vector<sp<Track>>* tracksToRemove) = 0;
  void removeTracks_l(const Vector<sp<Track>>& tracksToRemove);
  status_t handleVoipVolume_l(float* volume);
  virtual void onWriteReady();
  virtual void onDrainReady();
  virtual void onError();
  void resetWriteBlocked(uint32_t sequence);
  void resetDraining(uint32_t sequence);
  virtual bool waitingAsyncCallback();
  virtual bool waitingAsyncCallback_l();
  virtual bool shouldStandby_l();
  virtual void onAddNewTrack_l();
  void onAsyncError();
  virtual void preExit();
  virtual bool keepWakeLock() const { return true; }
  virtual void acquireWakeLock_l() {
    ThreadBase::acquireWakeLock_l();
    mActiveTracks.updatePowerState(this, true );
  }
  void dumpInternals_l(int fd, const Vector<String16>& args) override;
  void dumpTracks_l(int fd, const Vector<String16>& args) override;
 public:
  virtual status_t initCheck() const {
    return (mOutput == NULL) ? NO_INIT : NO_ERROR;
  }
  uint32_t latency() const;
  uint32_t latency_l() const;
  virtual void setMasterVolume(float value);
  virtual void setMasterBalance(float balance);
  virtual void setMasterMute(bool muted);
  virtual void setStreamVolume(audio_stream_type_t stream, float value);
  virtual void setStreamMute(audio_stream_type_t stream, bool muted);
  virtual float streamVolume(audio_stream_type_t stream) const;
  void setVolumeForOutput_l(float left, float right) const;
  sp<Track> createTrack_l(
      const sp<AudioFlinger::Client>& client, audio_stream_type_t streamType,
      const audio_attributes_t& attr, uint32_t* sampleRate,
      audio_format_t format, audio_channel_mask_t channelMask,
      size_t* pFrameCount, size_t* pNotificationFrameCount,
      uint32_t notificationsPerBuffer, float speed,
      const sp<IMemory>& sharedBuffer, audio_session_t sessionId,
      audio_output_flags_t* flags, pid_t creatorPid, pid_t tid, uid_t uid,
      status_t* status , audio_port_handle_t portId);
  AudioStreamOut* getOutput() const;
  AudioStreamOut* clearOutput();
  virtual sp<StreamHalInterface> stream() const;
  void suspend() { (void)android_atomic_inc(&mSuspended); }
  void restore() {
    if (android_atomic_dec(&mSuspended) <= 0) {
      android_atomic_release_store(0, &mSuspended);
    }
  }
  bool isSuspended() const {
    return android_atomic_acquire_load(&mSuspended) > 0;
  }
  virtual String8 getParameters(const String8& keys);
  virtual void ioConfigChanged(
      audio_io_config_event event, pid_t pid = 0,
      audio_port_handle_t portId = AUDIO_PORT_HANDLE_NONE);
  status_t getRenderPosition(uint32_t* halFrames, uint32_t* dspFrames);
  effect_buffer_t* sinkBuffer() const {
    return reinterpret_cast<effect_buffer_t*>(mSinkBuffer);
  }
  virtual void detachAuxEffect_l(int effectId);
  status_t attachAuxEffect(const sp<AudioFlinger::PlaybackThread::Track>& track,
                           int EffectId);
  status_t attachAuxEffect_l(
      const sp<AudioFlinger::PlaybackThread::Track>& track, int EffectId);
  virtual status_t addEffectChain_l(const sp<EffectChain>& chain);
  virtual size_t removeEffectChain_l(const sp<EffectChain>& chain);
  uint32_t hasAudioSession_l(audio_session_t sessionId) const override {
    return ThreadBase::hasAudioSession_l(sessionId, mTracks);
  }
  virtual uint32_t getStrategyForSession_l(audio_session_t sessionId);
  virtual status_t setSyncEvent(const sp<SyncEvent>& event);
  virtual bool isValidSyncEvent(const sp<SyncEvent>& event) const;
  bool invalidateTracks_l(audio_stream_type_t streamType);
  virtual void invalidateTracks(audio_stream_type_t streamType);
  virtual size_t frameCount() const { return mNormalFrameCount; }
  status_t getTimestamp_l(AudioTimestamp& timestamp);
  void addPatchTrack(const sp<PatchTrack>& track);
  void deletePatchTrack(const sp<PatchTrack>& track);
  virtual void toAudioPortConfig(struct audio_port_config* config);
  virtual int64_t computeWaitTimeNs_l() const { return INT64_MAX; }
  virtual bool isOutput() const override { return true; }
  virtual bool isTrackAllowed_l(audio_channel_mask_t channelMask __unused,
                                audio_format_t format __unused,
                                audio_session_t sessionId __unused,
                                uid_t uid) const {
    return trackCountForUid_l(uid) < PlaybackThread::kMaxTracksPerUid &&
           mTracks.size() < PlaybackThread::kMaxTracks;
  }
  bool isTimestampCorrectionEnabled() const override {
    return audio_is_output_devices(mTimestampCorrectedDevice) &&
           outDeviceTypes().count(mTimestampCorrectedDevice) != 0;
  }
 protected:
  size_t mNormalFrameCount;
  bool mThreadThrottle;
  uint32_t mThreadThrottleTimeMs;
  uint32_t mThreadThrottleEndMs;
  uint32_t mHalfBufferMs;
  void* mSinkBuffer;
  bool mMixerBufferEnabled;
  void* mMixerBuffer;
  size_t mMixerBufferSize;
  audio_format_t mMixerBufferFormat;
  bool mMixerBufferValid;
  bool mEffectBufferEnabled;
  void* mEffectBuffer;
  size_t mEffectBufferSize;
  audio_format_t mEffectBufferFormat;
  bool mEffectBufferValid;
  volatile int32_t mSuspended;
  int64_t mBytesWritten;
  int64_t mFramesWritten;
  int64_t mSuspendedFrames;
  audio_channel_mask_t mHapticChannelMask = AUDIO_CHANNEL_NONE;
  uint32_t mHapticChannelCount = 0;
 private:
  bool mMasterMute;
  void setMasterMute_l(bool muted) { mMasterMute = muted; }
 protected:
  ActiveTracks<Track> mActiveTracks;
  virtual uint32_t activeSleepTimeUs()
      const;
  virtual uint32_t idleSleepTimeUs() const = 0;
  virtual uint32_t suspendSleepTimeUs()
      const = 0;
  void checkSilentMode_l();
  virtual void saveOutputTracks() {}
  virtual void clearOutputTracks() {}
  virtual void cacheParameters_l();
  virtual uint32_t correctLatency_l(uint32_t latency) const;
  virtual status_t createAudioPatch_l(const struct audio_patch* patch,
                                      audio_patch_handle_t* handle);
  virtual status_t releaseAudioPatch_l(const audio_patch_handle_t handle);
  bool usesHwAvSync() const {
    return (mType == DIRECT) && (mOutput != NULL) && mHwSupportsPause &&
           (mOutput->flags & AUDIO_OUTPUT_FLAG_HW_AV_SYNC);
  }
  uint32_t trackCountForUid_l(uid_t uid) const;
 private:
  friend class AudioFlinger;
  DISALLOW_COPY_AND_ASSIGN(PlaybackThread);
  status_t addTrack_l(const sp<Track>& track);
  bool destroyTrack_l(const sp<Track>& track);
  void removeTrack_l(const sp<Track>& track);
  void readOutputParameters_l();
  void updateMetadata_l() final;
  virtual void sendMetadataToBackend_l(
      const StreamOutHalInterface::SourceMetadata& metadata);
  template <typename T>
  class Tracks {
   public:
    Tracks(bool saveDeletedTrackIds)
        : mSaveDeletedTrackIds(saveDeletedTrackIds) {}
    ssize_t add(const sp<T>& track) {
      const ssize_t index = mTracks.add(track);
      LOG_ALWAYS_FATAL_IF(index < 0, "cannot add track");
      return index;
    }
    ssize_t remove(const sp<T>& track);
    size_t size() const { return mTracks.size(); }
    bool isEmpty() const { return mTracks.isEmpty(); }
    ssize_t indexOf(const sp<T>& item) { return mTracks.indexOf(item); }
    sp<T> operator[](size_t index) const { return mTracks[index]; }
    typename SortedVector<sp<T>>::iterator begin() { return mTracks.begin(); }
    typename SortedVector<sp<T>>::iterator end() { return mTracks.end(); }
    size_t processDeletedTrackIds(std::function<void(int)> f) {
      for (const int trackId : mDeletedTrackIds) {
        f(trackId);
      }
      return mDeletedTrackIds.size();
    }
    void clearDeletedTrackIds() { mDeletedTrackIds.clear(); }
   private:
    const bool mSaveDeletedTrackIds;
    std::set<int> mDeletedTrackIds;
    SortedVector<sp<T>> mTracks;
  };
  Tracks<Track> mTracks;
  stream_type_t mStreamTypes[AUDIO_STREAM_CNT];
  AudioStreamOut* mOutput;
  float mMasterVolume;
  std::atomic<float> mMasterBalance{};
  audio_utils::Balance mBalance;
  int mNumWrites;
  int mNumDelayedWrites;
  bool mInWrite;
  nsecs_t mStandbyTimeNs;
  size_t mSinkBufferSize;
  uint32_t mActiveSleepTimeUs;
  uint32_t mIdleSleepTimeUs;
  uint32_t mSleepTimeUs;
  mixer_state mMixerStatus;
  mixer_state mMixerStatusIgnoringFastTracks;
  uint32_t sleepTimeShift;
  nsecs_t mStandbyDelayNs;
  nsecs_t maxPeriod;
  uint32_t writeFrames;
  size_t mBytesRemaining;
  size_t mCurrentWriteLength;
  bool mUseAsyncWrite;
  uint32_t mWriteAckSequence;
  uint32_t mDrainSequence;
  sp<AsyncCallbackThread> mCallbackThread;
  sp<NBAIO_Sink> mOutputSink;
  sp<NBAIO_Sink> mPipeSink;
  sp<NBAIO_Sink> mNormalSink;
  uint32_t mScreenState;
  static const size_t kFastMixerLogSize = 8 * 1024;
  sp<NBLog::Writer> mFastMixerNBLogWriter;
  audio_utils::Statistics<double> mDownstreamLatencyStatMs{0.999};
 public:
  virtual bool hasFastMixer() const = 0;
  virtual FastTrackUnderruns getFastTrackUnderruns(
      size_t fastIndex __unused) const {
    FastTrackUnderruns dummy;
    return dummy;
  }
 protected:
  unsigned mFastTrackAvailMask;
  bool mHwSupportsPause;
  bool mHwPaused;
  bool mFlushPending;
  float mLeftVolFloat;
  float mRightVolFloat;
};
class MixerThread : public PlaybackThread {
 public:
  MixerThread(const sp<AudioFlinger>& audioFlinger, AudioStreamOut* output,
              audio_io_handle_t id, bool systemReady, type_t type = MIXER);
  virtual ~MixerThread();
  virtual bool checkForNewParameter_l(const String8& keyValuePair,
                                      status_t& status);
  virtual bool isTrackAllowed_l(audio_channel_mask_t channelMask,
                                audio_format_t format,
                                audio_session_t sessionId,
                                uid_t uid) const override;
 protected:
  virtual mixer_state prepareTracks_l(Vector<sp<Track>>* tracksToRemove);
  virtual uint32_t idleSleepTimeUs() const;
  virtual uint32_t suspendSleepTimeUs() const;
  virtual void cacheParameters_l();
  virtual void acquireWakeLock_l() {
    PlaybackThread::acquireWakeLock_l();
    if (hasFastMixer()) {
      mFastMixer->setBoottimeOffset(
          mTimestamp.mTimebaseOffset[ExtendedTimestamp::TIMEBASE_BOOTTIME]);
    }
  }
  void dumpInternals_l(int fd, const Vector<String16>& args) override;
  virtual ssize_t threadLoop_write();
  virtual void threadLoop_standby();
  virtual void threadLoop_mix();
  virtual void threadLoop_sleepTime();
  virtual uint32_t correctLatency_l(uint32_t latency) const;
  virtual status_t createAudioPatch_l(const struct audio_patch* patch,
                                      audio_patch_handle_t* handle);
  virtual status_t releaseAudioPatch_l(const audio_patch_handle_t handle);
  AudioMixer* mAudioMixer;
 private:
  sp<FastMixer> mFastMixer;
  sp<AudioWatchdog>
      mAudioWatchdog;
  FastMixerDumpState mFastMixerDumpState;
#ifdef STATE_QUEUE_DUMP
  StateQueueObserverDump mStateQueueObserverDump;
  StateQueueMutatorDump mStateQueueMutatorDump;
#endif
  AudioWatchdogDump mAudioWatchdogDump;
  int32_t mFastMixerFutex;
  std::atomic_bool mMasterMono;
 public:
  virtual bool hasFastMixer() const { return mFastMixer != 0; }
  virtual FastTrackUnderruns getFastTrackUnderruns(size_t fastIndex) const {
    ALOG_ASSERT(fastIndex < FastMixerState::sMaxFastTracks);
    return mFastMixerDumpState.mTracks[fastIndex].mUnderruns;
  }
  status_t threadloop_getHalTimestamp_l(
      ExtendedTimestamp* timestamp) const override {
    if (mNormalSink.get() != nullptr) {
      return mNormalSink->getTimestamp(*timestamp);
    }
    return INVALID_OPERATION;
  }
 protected:
  virtual void setMasterMono_l(bool mono) {
    mMasterMono.store(mono);
    if (mFastMixer != nullptr) {
      mFastMixer->setMasterMono(mMasterMono);
    }
  }
  virtual bool requireMonoBlend() {
    return mMasterMono.load() && !hasFastMixer();
  }
  void setMasterBalance(float balance) override {
    mMasterBalance.store(balance);
    if (hasFastMixer()) {
      mFastMixer->setMasterBalance(balance);
    }
  }
};
class DirectOutputThread : public PlaybackThread {
 public:
  DirectOutputThread(const sp<AudioFlinger>& audioFlinger,
                     AudioStreamOut* output, audio_io_handle_t id,
                     audio_devices_t device, bool systemReady)
      : DirectOutputThread(audioFlinger, output, id, device, DIRECT,
                           systemReady) {}
  virtual ~DirectOutputThread();
  status_t selectPresentation(int presentationId, int programId);
  virtual bool checkForNewParameter_l(const String8& keyValuePair,
                                      status_t& status);
  virtual void flushHw_l();
  void setMasterBalance(float balance) override;
 protected:
  virtual uint32_t activeSleepTimeUs() const;
  virtual uint32_t idleSleepTimeUs() const;
  virtual uint32_t suspendSleepTimeUs() const;
  virtual void cacheParameters_l();
  void dumpInternals_l(int fd, const Vector<String16>& args) override;
  virtual mixer_state prepareTracks_l(Vector<sp<Track>>* tracksToRemove);
  virtual void threadLoop_mix();
  virtual void threadLoop_sleepTime();
  virtual void threadLoop_exit();
  virtual bool shouldStandby_l();
  virtual void onAddNewTrack_l();
  bool mVolumeShaperActive = false;
  DirectOutputThread(const sp<AudioFlinger>& audioFlinger,
                     AudioStreamOut* output, audio_io_handle_t id,
                     ThreadBase::type_t type, bool systemReady);
  void processVolume_l(Track* track, bool lastTrack);
  sp<Track> mActiveTrack;
  wp<Track> mPreviousTrack;
  float mMasterBalanceLeft = 1.f;
  float mMasterBalanceRight = 1.f;
 public:
  virtual bool hasFastMixer() const { return false; }
  virtual int64_t computeWaitTimeNs_l() const override;
  status_t threadloop_getHalTimestamp_l(
      ExtendedTimestamp* timestamp) const override {
    if (mOutput != nullptr) {
      uint64_t uposition64;
      struct timespec time;
      if (mOutput->getPresentationPosition(&uposition64, &time) == OK) {
        timestamp->mPosition[ExtendedTimestamp::LOCATION_KERNEL] =
            (int64_t)uposition64;
        timestamp->mTimeNs[ExtendedTimestamp::LOCATION_KERNEL] =
            audio_utils_ns_from_timespec(&time);
        return NO_ERROR;
      }
    }
    return INVALID_OPERATION;
  }
};
class OffloadThread : public DirectOutputThread {
 public:
  OffloadThread(const sp<AudioFlinger>& audioFlinger, AudioStreamOut* output,
                audio_io_handle_t id, bool systemReady);
  virtual ~OffloadThread() {}
  virtual void flushHw_l();
 protected:
  virtual mixer_state prepareTracks_l(Vector<sp<Track>>* tracksToRemove);
  virtual void threadLoop_exit();
  virtual bool waitingAsyncCallback();
  virtual bool waitingAsyncCallback_l();
  virtual void invalidateTracks(audio_stream_type_t streamType);
  virtual bool keepWakeLock() const {
    return (mKeepWakeLock || (mDrainSequence & 1));
  }
 private:
  size_t mPausedWriteLength;
  size_t
      mPausedBytesRemaining;
  bool mKeepWakeLock;
  uint64_t
      mOffloadUnderrunPosition;
};
class AsyncCallbackThread : public Thread {
 public:
  explicit AsyncCallbackThread(const wp<PlaybackThread>& playbackThread);
  virtual ~AsyncCallbackThread();
  virtual bool threadLoop();
  virtual void onFirstRef();
  void exit();
  void setWriteBlocked(uint32_t sequence);
  void resetWriteBlocked();
  void setDraining(uint32_t sequence);
  void resetDraining();
  void setAsyncError();
 private:
  const wp<PlaybackThread> mPlaybackThread;
  uint32_t mWriteAckSequence;
  uint32_t mDrainSequence;
  Condition mWaitWorkCV;
  Mutex mLock;
  bool mAsyncError;
};
class DuplicatingThread : public MixerThread {
 public:
  DuplicatingThread(const sp<AudioFlinger>& audioFlinger,
                    MixerThread* mainThread, audio_io_handle_t id,
                    bool systemReady);
  virtual ~DuplicatingThread();
  void addOutputTrack(MixerThread* thread);
  void removeOutputTrack(MixerThread* thread);
  uint32_t waitTimeMs() const { return mWaitTimeMs; }
  void sendMetadataToBackend_l(
      const StreamOutHalInterface::SourceMetadata& metadata) override;
 protected:
  virtual uint32_t activeSleepTimeUs() const;
  void dumpInternals_l(int fd, const Vector<String16>& args) override;
 private:
  bool outputsReady(const SortedVector<sp<OutputTrack>>& outputTracks);
 protected:
  virtual void threadLoop_mix();
  virtual void threadLoop_sleepTime();
  virtual ssize_t threadLoop_write();
  virtual void threadLoop_standby();
  virtual void cacheParameters_l();
 private:
  virtual void updateWaitTime_l();
 protected:
  virtual void saveOutputTracks();
  virtual void clearOutputTracks();
 private:
  uint32_t mWaitTimeMs;
  SortedVector<sp<OutputTrack>> outputTracks;
  SortedVector<sp<OutputTrack>> mOutputTracks;
 public:
  virtual bool hasFastMixer() const { return false; }
  status_t threadloop_getHalTimestamp_l(
      ExtendedTimestamp* timestamp) const override {
    if (mOutputTracks.size() > 0) {
      const ExtendedTimestamp trackTimestamp =
          mOutputTracks[0]->getClientProxyTimestamp();
      if (trackTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL] > 0) {
        timestamp->mTimeNs[ExtendedTimestamp::LOCATION_KERNEL] =
            trackTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL];
        timestamp->mPosition[ExtendedTimestamp::LOCATION_KERNEL] =
            trackTimestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL];
        return OK;
      }
    }
    return INVALID_OPERATION;
  }
};
class RecordThread : public ThreadBase {
 public:
  class RecordTrack;
  class ResamplerBufferProvider : public AudioBufferProvider {
   public:
    explicit ResamplerBufferProvider(RecordTrack* recordTrack)
        : mRecordTrack(recordTrack), mRsmpInUnrel(0), mRsmpInFront(0) {}
    virtual ~ResamplerBufferProvider() {}
    virtual void reset();
    virtual void sync(size_t* framesAvailable = NULL, bool* hasOverrun = NULL);
    virtual status_t getNextBuffer(AudioBufferProvider::Buffer* buffer);
    virtual void releaseBuffer(AudioBufferProvider::Buffer* buffer);
   private:
    RecordTrack* const mRecordTrack;
    size_t mRsmpInUnrel;
    int32_t mRsmpInFront;
  };
  RecordThread(const sp<AudioFlinger>& audioFlinger, AudioStreamIn* input,
               audio_io_handle_t id, bool systemReady);
  virtual ~RecordThread();
  void destroyTrack_l(const sp<RecordTrack>& track);
  void removeTrack_l(const sp<RecordTrack>& track);
  virtual bool threadLoop();
  virtual void preExit();
  virtual void onFirstRef();
  virtual status_t initCheck() const {
    return (mInput == NULL) ? NO_INIT : NO_ERROR;
  }
  virtual sp<MemoryDealer> readOnlyHeap() const { return mReadOnlyHeap; }
  virtual sp<IMemory> pipeMemory() const { return mPipeMemory; }
  sp<AudioFlinger::RecordThread::RecordTrack> createRecordTrack_l(
      const sp<AudioFlinger::Client>& client, const audio_attributes_t& attr,
      uint32_t* pSampleRate, audio_format_t format,
      audio_channel_mask_t channelMask, size_t* pFrameCount,
      audio_session_t sessionId, size_t* pNotificationFrameCount,
      pid_t creatorPid, uid_t uid, audio_input_flags_t* flags, pid_t tid,
      status_t* status , audio_port_handle_t portId,
      const String16& opPackageName);
  status_t start(RecordTrack* recordTrack, AudioSystem::sync_event_t event,
                 audio_session_t triggerSession);
  bool stop(RecordTrack* recordTrack);
  AudioStreamIn* clearInput();
  virtual sp<StreamHalInterface> stream() const;
  virtual bool checkForNewParameter_l(const String8& keyValuePair,
                                      status_t& status);
  virtual void cacheParameters_l() {}
  virtual String8 getParameters(const String8& keys);
  virtual void ioConfigChanged(
      audio_io_config_event event, pid_t pid = 0,
      audio_port_handle_t portId = AUDIO_PORT_HANDLE_NONE);
  virtual status_t createAudioPatch_l(const struct audio_patch* patch,
                                      audio_patch_handle_t* handle);
  virtual status_t releaseAudioPatch_l(const audio_patch_handle_t handle);
  void updateOutDevices(const DeviceDescriptorBaseVector& outDevices) override;
  void addPatchTrack(const sp<PatchRecord>& record);
  void deletePatchTrack(const sp<PatchRecord>& record);
  void readInputParameters_l();
  virtual uint32_t getInputFramesLost();
  virtual status_t addEffectChain_l(const sp<EffectChain>& chain);
  virtual size_t removeEffectChain_l(const sp<EffectChain>& chain);
  uint32_t hasAudioSession_l(audio_session_t sessionId) const override {
    return ThreadBase::hasAudioSession_l(sessionId, mTracks);
  }
  KeyedVector<audio_session_t, bool> sessionIds() const;
  virtual status_t setSyncEvent(const sp<SyncEvent>& event);
  virtual bool isValidSyncEvent(const sp<SyncEvent>& event) const;
  static void syncStartEventCallback(const wp<SyncEvent>& event);
  virtual size_t frameCount() const { return mFrameCount; }
  bool hasFastCapture() const { return mFastCapture != 0; }
  virtual void toAudioPortConfig(struct audio_port_config* config);
  virtual status_t checkEffectCompatibility_l(const effect_descriptor_t* desc,
                                              audio_session_t sessionId);
  virtual void acquireWakeLock_l() {
    ThreadBase::acquireWakeLock_l();
    mActiveTracks.updatePowerState(this, true );
  }
  virtual bool isOutput() const override { return false; }
  void checkBtNrec();
  void setRecordSilenced(audio_port_handle_t portId, bool silenced);
  status_t getActiveMicrophones(
      std::vector<media::MicrophoneInfo>* activeMicrophones);
  status_t setPreferredMicrophoneDirection(
      audio_microphone_direction_t direction);
  status_t setPreferredMicrophoneFieldDimension(float zoom);
  void updateMetadata_l() override;
  bool fastTrackAvailable() const { return mFastTrackAvail; }
  bool isTimestampCorrectionEnabled() const override {
    return audio_is_input_device(mTimestampCorrectedDevice) &&
           inDeviceType() == mTimestampCorrectedDevice;
  }
 protected:
  void dumpInternals_l(int fd, const Vector<String16>& args) override;
  void dumpTracks_l(int fd, const Vector<String16>& args) override;
 private:
  void standbyIfNotAlreadyInStandby();
  void inputStandBy();
  void checkBtNrec_l();
  AudioStreamIn* mInput;
  Source* mSource;
  SortedVector<sp<RecordTrack>> mTracks;
  ActiveTracks<RecordTrack> mActiveTracks;
  Condition mStartStopCond;
  void* mRsmpInBuffer;
  size_t mRsmpInFrames;
  size_t mRsmpInFramesP2;
  size_t mRsmpInFramesOA;
  int32_t mRsmpInRear;
  const sp<MemoryDealer> mReadOnlyHeap;
  sp<FastCapture> mFastCapture;
  FastCaptureDumpState mFastCaptureDumpState;
#ifdef STATE_QUEUE_DUMP
#endif
  int32_t mFastCaptureFutex;
  sp<NBAIO_Source> mInputSource;
  sp<NBAIO_Source> mNormalSource;
  sp<NBAIO_Sink> mPipeSink;
  sp<NBAIO_Source> mPipeSource;
  size_t mPipeFramesP2;
  sp<IMemory> mPipeMemory;
  static const size_t kFastCaptureLogSize = 4 * 1024;
  sp<NBLog::Writer> mFastCaptureNBLogWriter;
  bool mFastTrackAvail;
  std::atomic_bool mBtNrecSuspended;
  int64_t mFramesRead = 0;
  DeviceDescriptorBaseVector mOutDevices;
};
class MmapThread : public ThreadBase {
 public:
  MmapThread(const sp<AudioFlinger>& audioFlinger, audio_io_handle_t id,
             AudioHwDevice* hwDev, sp<StreamHalInterface> stream,
             bool systemReady);
  virtual ~MmapThread();
  virtual void configure(const audio_attributes_t* attr,
                         audio_stream_type_t streamType,
                         audio_session_t sessionId,
                         const sp<MmapStreamCallback>& callback,
                         audio_port_handle_t deviceId,
                         audio_port_handle_t portId);
  void disconnect();
  status_t createMmapBuffer(int32_t minSizeFrames,
                            struct audio_mmap_buffer_info* info);
  status_t getMmapPosition(struct audio_mmap_position* position);
  status_t start(const AudioClient& client, audio_port_handle_t* handle);
  status_t stop(audio_port_handle_t handle);
  status_t standby();
  virtual void onFirstRef();
  virtual bool threadLoop();
  virtual void threadLoop_exit();
  virtual void threadLoop_standby();
  virtual bool shouldStandby_l() { return false; }
  virtual status_t exitStandby();
  virtual status_t initCheck() const {
    return (mHalStream == 0) ? NO_INIT : NO_ERROR;
  }
  virtual size_t frameCount() const { return mFrameCount; }
  virtual bool checkForNewParameter_l(const String8& keyValuePair,
                                      status_t& status);
  virtual String8 getParameters(const String8& keys);
  virtual void ioConfigChanged(
      audio_io_config_event event, pid_t pid = 0,
      audio_port_handle_t portId = AUDIO_PORT_HANDLE_NONE);
  void readHalParameters_l();
  virtual void cacheParameters_l() {}
  virtual status_t createAudioPatch_l(const struct audio_patch* patch,
                                      audio_patch_handle_t* handle);
  virtual status_t releaseAudioPatch_l(const audio_patch_handle_t handle);
  virtual void toAudioPortConfig(struct audio_port_config* config);
  virtual sp<StreamHalInterface> stream() const { return mHalStream; }
  virtual status_t addEffectChain_l(const sp<EffectChain>& chain);
  virtual size_t removeEffectChain_l(const sp<EffectChain>& chain);
  virtual status_t checkEffectCompatibility_l(const effect_descriptor_t* desc,
                                              audio_session_t sessionId);
  uint32_t hasAudioSession_l(audio_session_t sessionId) const override {
    return ThreadBase::hasAudioSession_l(sessionId, mActiveTracks);
  }
  virtual status_t setSyncEvent(const sp<SyncEvent>& event);
  virtual bool isValidSyncEvent(const sp<SyncEvent>& event) const;
  virtual void checkSilentMode_l() {}
  virtual void processVolume_l() {}
  void checkInvalidTracks_l();
  virtual audio_stream_type_t streamType() { return AUDIO_STREAM_DEFAULT; }
  virtual void invalidateTracks(audio_stream_type_t streamType __unused) {}
  virtual void setRecordSilenced(audio_port_handle_t portId __unused,
                                 bool silenced __unused) {}
 protected:
  void dumpInternals_l(int fd, const Vector<String16>& args) override;
  void dumpTracks_l(int fd, const Vector<String16>& args) override;
  audio_port_handle_t mDeviceId = AUDIO_PORT_HANDLE_NONE;
  audio_attributes_t mAttr;
  audio_session_t mSessionId;
  audio_port_handle_t mPortId;
  wp<MmapStreamCallback> mCallback;
  sp<StreamHalInterface> mHalStream;
  sp<DeviceHalInterface> mHalDevice;
  AudioHwDevice* const mAudioHwDev;
  ActiveTracks<MmapTrack> mActiveTracks;
  float mHalVolFloat;
  int32_t mNoCallbackWarningCount;
  static constexpr int32_t kMaxNoCallbackWarnings = 5;
};
class MmapPlaybackThread : public MmapThread, public VolumeInterface {
 public:
  MmapPlaybackThread(const sp<AudioFlinger>& audioFlinger, audio_io_handle_t id,
                     AudioHwDevice* hwDev, AudioStreamOut* output,
                     bool systemReady);
  virtual ~MmapPlaybackThread() {}
  virtual void configure(const audio_attributes_t* attr,
                         audio_stream_type_t streamType,
                         audio_session_t sessionId,
                         const sp<MmapStreamCallback>& callback,
                         audio_port_handle_t deviceId,
                         audio_port_handle_t portId);
  AudioStreamOut* clearOutput();
  virtual void setMasterVolume(float value);
  virtual void setMasterMute(bool muted);
  virtual void setStreamVolume(audio_stream_type_t stream, float value);
  virtual void setStreamMute(audio_stream_type_t stream, bool muted);
  virtual float streamVolume(audio_stream_type_t stream) const;
  void setMasterMute_l(bool muted) { mMasterMute = muted; }
  virtual void invalidateTracks(audio_stream_type_t streamType);
  virtual audio_stream_type_t streamType() { return mStreamType; }
  virtual void checkSilentMode_l();
  void processVolume_l() override;
  virtual bool isOutput() const override { return true; }
  void updateMetadata_l() override;
  virtual void toAudioPortConfig(struct audio_port_config* config);
 protected:
  void dumpInternals_l(int fd, const Vector<String16>& args) override;
  audio_stream_type_t mStreamType;
  float mMasterVolume;
  float mStreamVolume;
  bool mMasterMute;
  bool mStreamMute;
  AudioStreamOut* mOutput;
};
class MmapCaptureThread : public MmapThread {
 public:
  MmapCaptureThread(const sp<AudioFlinger>& audioFlinger, audio_io_handle_t id,
                    AudioHwDevice* hwDev, AudioStreamIn* input,
                    bool systemReady);
  virtual ~MmapCaptureThread() {}
  AudioStreamIn* clearInput();
  status_t exitStandby() override;
  virtual bool isOutput() const override { return false; }
  void updateMetadata_l() override;
  void processVolume_l() override;
  void setRecordSilenced(audio_port_handle_t portId, bool silenced) override;
  virtual void toAudioPortConfig(struct audio_port_config* config);
 protected:
  AudioStreamIn* mInput;
};
