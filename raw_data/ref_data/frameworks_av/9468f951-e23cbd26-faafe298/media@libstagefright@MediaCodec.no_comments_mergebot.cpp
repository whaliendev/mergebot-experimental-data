#define LOG_TAG "MediaCodec"
#include <utils/Log.h>
#include <inttypes.h>
#include <stdlib.h>
#include "include/SecureBuffer.h"
#include "StagefrightPluginLoader.h"
#include "include/SharedMemoryBuffer.h"
#include "include/SoftwareRenderer.h"
#include <android/hardware/cas/native/1.0/IDescrambler.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <android/hardware/media/omx/1.0/IGraphicBufferSource.h>
#include <aidl/android/media/BnResourceManagerClient.h>
#include <aidl/android/media/IResourceManagerService.h>
#include <media/IResourceManagerService.h>
#include <media/MediaAnalyticsItem.h>
#include <android/binder_ibinder.h>
#include <android/binder_manager.h>
#include <binder/IMemory.h>
#include <binder/MemoryDealer.h>
#include <cutils/properties.h>
#include <gui/BufferQueue.h>
#include <gui/Surface.h>
#include <mediadrm/ICrypto.h>
#include <media/IOMX.h>
#include <media/MediaCodecBuffer.h>
#include <media/MediaMetricsItem.h>
#include <media/MediaResource.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/AString.h>
#include <media/stagefright/foundation/AUtils.h>
#include <media/stagefright/foundation/avc_utils.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/ACodec.h>
#include <media/stagefright/BatteryChecker.h>
#include <media/stagefright/BufferProducerWrapper.h>
#include <media/stagefright/CCodec.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaFilter.h>
#include <media/stagefright/OMXClient.h>
#include <media/stagefright/PersistentSurface.h>
#include <media/stagefright/SurfaceUtils.h>
#include <private/android_filesystem_config.h>
#include <utils/Singleton.h>
namespace android {
static const char *kCodecKeyName = "codec";
static const char *kCodecCodec =
    "android.media.mediacodec.codec";
static const char *kCodecMime =
    "android.media.mediacodec.mime";
static const char *kCodecMode =
    "android.media.mediacodec.mode";
static const char *kCodecModeVideo =
    "video";
static const char *kCodecModeAudio = "audio";
static const char *kCodecEncoder = "android.media.mediacodec.encoder";
static const char *kCodecSecure = "android.media.mediacodec.secure";
static const char *kCodecWidth = "android.media.mediacodec.width";
static const char *kCodecHeight = "android.media.mediacodec.height";
static const char *kCodecRotation =
    "android.media.mediacodec.rotation-degrees";
static const char *kCodecCrypto = "android.media.mediacodec.crypto";
static const char *kCodecProfile =
    "android.media.mediacodec.profile";
static const char *kCodecLevel = "android.media.mediacodec.level";
static const char *kCodecMaxWidth =
    "android.media.mediacodec.maxwidth";
static const char *kCodecMaxHeight =
    "android.media.mediacodec.maxheight";
static const char *kCodecError = "android.media.mediacodec.errcode";
static const char *kCodecErrorState = "android.media.mediacodec.errstate";
static const char *kCodecLatencyMax =
    "android.media.mediacodec.latency.max";
static const char *kCodecLatencyMin =
    "android.media.mediacodec.latency.min";
static const char *kCodecLatencyAvg =
    "android.media.mediacodec.latency.avg";
static const char *kCodecLatencyCount = "android.media.mediacodec.latency.n";
static const char *kCodecLatencyHist =
    "android.media.mediacodec.latency.hist";
static const char *kCodecLatencyUnknown =
    "android.media.mediacodec.latency.unknown";
static const char *kCodecNumLowLatencyModeOn =
    "android.media.mediacodec.low-latency.on";
static const char *kCodecNumLowLatencyModeOff =
    "android.media.mediacodec.low-latency.off";
static const char *kCodecFirstFrameIndexLowLatencyModeOn =
    "android.media.mediacodec.low-latency.first-frame";
static const char *kCodecRecentLatencyMax =
    "android.media.mediacodec.recent.max";
static const char *kCodecRecentLatencyMin =
    "android.media.mediacodec.recent.min";
static const char *kCodecRecentLatencyAvg =
    "android.media.mediacodec.recent.avg";
static const char *kCodecRecentLatencyCount =
    "android.media.mediacodec.recent.n";
static const char *kCodecRecentLatencyHist =
    "android.media.mediacodec.recent.hist";
static bool kEmitHistogram = false;
static int64_t getId(const std::shared_ptr<IResourceManagerClient> &client) {
  return (int64_t)client.get();
}
static bool isResourceError(status_t err) { return (err == NO_MEMORY); }
static const int kMaxRetry = 2;
static const int kMaxReclaimWaitTimeInUs = 500000;
static const int kNumBuffersAlign = 16;
struct ResourceManagerClient : public BnResourceManagerClient {
  explicit ResourceManagerClient(MediaCodec *codec) : mMediaCodec(codec) {}
  Status reclaimResource(bool *_aidl_return) override {
    sp<MediaCodec> codec = mMediaCodec.promote();
    if (codec == NULL) {
      *_aidl_return = true;
      return Status::ok();
    }
    status_t err = codec->reclaim();
    if (err == WOULD_BLOCK) {
      ALOGD("Wait for the client to release codec.");
      usleep(kMaxReclaimWaitTimeInUs);
      ALOGD("Try to reclaim again.");
      err = codec->reclaim(true );
    }
    if (err != OK) {
      ALOGW("ResourceManagerClient failed to release codec with err %d", err);
    }
    *_aidl_return = (err == OK);
    return Status::ok();
  }
  Status getName(::std::string *_aidl_return) override {
    _aidl_return->clear();
    sp<MediaCodec> codec = mMediaCodec.promote();
    if (codec == NULL) {
      return Status::ok();
    }
    AString name;
    if (codec->getName(&name) == OK) {
      *_aidl_return = name.c_str();
    }
    return Status::ok();
  }
 protected:
  virtual ~ResourceManagerClient() {}
 private:
  wp<MediaCodec> mMediaCodec;
  DISALLOW_EVIL_CONSTRUCTORS(ResourceManagerClient);
};
struct MediaCodec::ResourceManagerServiceProxy : public RefBase {
  ResourceManagerServiceProxy(
      pid_t pid, uid_t uid,
      const std::shared_ptr<IResourceManagerClient> &client);
  virtual ~ResourceManagerServiceProxy();
  void init();
  static void BinderDiedCallback(void *cookie);
  void binderDied();
  void addResource(const MediaResourceParcel &resource);
  void removeResource(const MediaResourceParcel &resource);
  void removeClient();
  bool reclaimResource(const std::vector<MediaResourceParcel> &resources);
 private:
  Mutex mLock;
  pid_t mPid;
  uid_t mUid;
  std::shared_ptr<IResourceManagerService> mService;
  std::shared_ptr<IResourceManagerClient> mClient;
  ::ndk::ScopedAIBinder_DeathRecipient mDeathRecipient;
};
MediaCodec::ResourceManagerServiceProxy::ResourceManagerServiceProxy(
    pid_t pid, uid_t uid, const std::shared_ptr<IResourceManagerClient> &client)
    : mPid(pid),
      mUid(uid),
      mClient(client),
      mDeathRecipient(AIBinder_DeathRecipient_new(BinderDiedCallback)) {
  if (mPid == MediaCodec::kNoPid) {
    mPid = AIBinder_getCallingPid();
  }
}
~ResourceManagerServiceProxy() {
  if (mService != nullptr) {
    AIBinder_unlinkToDeath(mService->asBinder().get(), mDeathRecipient.get(),
                           this);
  }
}
void MediaCodec::ResourceManagerServiceProxy::init() {
  ::ndk::SpAIBinder binder(
      AServiceManager_getService("media.resource_manager"));
  mService = IResourceManagerService::fromBinder(binder);
  if (mService == nullptr) {
    ALOGE("Failed to get ResourceManagerService");
    return;
  }
  AIBinder_linkToDeath(mService->asBinder().get(), mDeathRecipient.get(), this);
}
void MediaCodec::ResourceManagerServiceProxy::BinderDiedCallback(void *cookie) {
  auto thiz = static_cast<ResourceManagerServiceProxy *>(cookie);
  thiz->binderDied();
}
void MediaCodec::ResourceManagerServiceProxy::binderDied() {
  ALOGW("ResourceManagerService died.");
  Mutex::Autolock _l(mLock);
  mService = nullptr;
}
void MediaCodec::ResourceManagerServiceProxy::addResource(
    const MediaResourceParcel &resource) {
  std::vector<MediaResourceParcel> resources;
  resources.push_back(resource);
  Mutex::Autolock _l(mLock);
  if (mService == nullptr) {
    return;
  }
  mService->addResource(mPid, mUid, getId(mClient), mClient, resources);
}
void MediaCodec::ResourceManagerServiceProxy::removeResource(
    const MediaResourceParcel &resource) {
  std::vector<MediaResourceParcel> resources;
  resources.push_back(resource);
  Mutex::Autolock _l(mLock);
  if (mService == nullptr) {
    return;
  }
  mService->removeResource(mPid, getId(mClient), resources);
}
void MediaCodec::ResourceManagerServiceProxy::removeClient() {
  Mutex::Autolock _l(mLock);
  if (mService == nullptr) {
    return;
  }
  mService->removeClient(mPid, getId(mClient));
}
bool MediaCodec::ResourceManagerServiceProxy::reclaimResource(
    const std::vector<MediaResourceParcel> &resources) {
  Mutex::Autolock _l(mLock);
  if (mService == NULL) {
    return false;
  }
  bool success;
  Status status = mService->reclaimResource(mPid, resources, &success);
  return status.isOk() && success;
}
std::string MediaCodec::Histogram::emit() {
  std::string value;
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "%" PRId64 ",%" PRId64 ",%" PRId64 "{",
           mFloor, mWidth, mBelow);
  value = buffer;
  for (int i = 0; i < mBucketCount; i++) {
    if (i != 0) {
      value = value + ",";
    }
    snprintf(buffer, sizeof(buffer), "%" PRId64, mBuckets[i]);
    value = value + buffer;
  }
  snprintf(buffer, sizeof(buffer), "}%" PRId64, mAbove);
  value = value + buffer;
  return value;
}
std::string MediaCodec::Histogram::emit() {
  std::string value;
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "%" PRId64 ",%" PRId64 ",%" PRId64 "{",
           mFloor, mWidth, mBelow);
  value = buffer;
  for (int i = 0; i < mBucketCount; i++) {
    if (i != 0) {
      value = value + ",";
    }
    snprintf(buffer, sizeof(buffer), "%" PRId64, mBuckets[i]);
    value = value + buffer;
  }
  snprintf(buffer, sizeof(buffer), "}%" PRId64, mAbove);
  value = value + buffer;
  return value;
}
std::string MediaCodec::Histogram::emit() {
  std::string value;
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "%" PRId64 ",%" PRId64 ",%" PRId64 "{",
           mFloor, mWidth, mBelow);
  value = buffer;
  for (int i = 0; i < mBucketCount; i++) {
    if (i != 0) {
      value = value + ",";
    }
    snprintf(buffer, sizeof(buffer), "%" PRId64, mBuckets[i]);
    value = value + buffer;
  }
  snprintf(buffer, sizeof(buffer), "}%" PRId64, mAbove);
  value = value + buffer;
  return value;
}
std::string MediaCodec::Histogram::emit() {
  std::string value;
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "%" PRId64 ",%" PRId64 ",%" PRId64 "{",
           mFloor, mWidth, mBelow);
  value = buffer;
  for (int i = 0; i < mBucketCount; i++) {
    if (i != 0) {
      value = value + ",";
    }
    snprintf(buffer, sizeof(buffer), "%" PRId64, mBuckets[i]);
    value = value + buffer;
  }
  snprintf(buffer, sizeof(buffer), "}%" PRId64, mAbove);
  value = value + buffer;
  return value;
}
std::string MediaCodec::Histogram::emit() {
  std::string value;
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "%" PRId64 ",%" PRId64 ",%" PRId64 "{",
           mFloor, mWidth, mBelow);
  value = buffer;
  for (int i = 0; i < mBucketCount; i++) {
    if (i != 0) {
      value = value + ",";
    }
    snprintf(buffer, sizeof(buffer), "%" PRId64, mBuckets[i]);
    value = value + buffer;
  }
  snprintf(buffer, sizeof(buffer), "}%" PRId64, mAbove);
  value = value + buffer;
  return value;
}
std::string MediaCodec::Histogram::emit() {
  std::string value;
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "%" PRId64 ",%" PRId64 ",%" PRId64 "{",
           mFloor, mWidth, mBelow);
  value = buffer;
  for (int i = 0; i < mBucketCount; i++) {
    if (i != 0) {
      value = value + ",";
    }
    snprintf(buffer, sizeof(buffer), "%" PRId64, mBuckets[i]);
    value = value + buffer;
  }
  snprintf(buffer, sizeof(buffer), "}%" PRId64, mAbove);
  value = value + buffer;
  return value;
}
MediaCodec::BufferInfo::BufferInfo() : mOwnedByClient(false) {}
namespace {
enum {
  kWhatFillThisBuffer = 'fill',
  kWhatDrainThisBuffer = 'drai',
  kWhatEOS = 'eos ',
  kWhatStartCompleted = 'Scom',
  kWhatStopCompleted = 'scom',
  kWhatReleaseCompleted = 'rcom',
  kWhatFlushCompleted = 'fcom',
  kWhatError = 'erro',
  kWhatComponentAllocated = 'cAll',
  kWhatComponentConfigured = 'cCon',
  kWhatInputSurfaceCreated = 'isfc',
  kWhatInputSurfaceAccepted = 'isfa',
  kWhatSignaledInputEOS = 'seos',
  kWhatOutputFramesRendered = 'outR',
  kWhatOutputBuffersChanged = 'outC',
};
class BufferCallback : public CodecBase::BufferCallback {
 public:
  explicit BufferCallback(const sp<AMessage> &notify);
  virtual ~BufferCallback()
      virtual void onInputBufferAvailable(
          size_t index, const sp<MediaCodecBuffer> &buffer) override;
  virtual void onOutputBufferAvailable(
      size_t index, const sp<MediaCodecBuffer> &buffer) override;
 private:
  const sp<AMessage> mNotify;
};
BufferCallback::BufferCallback(const sp<AMessage> &notify) : mNotify(notify) {}
void BufferCallback::onInputBufferAvailable(
    size_t index, const sp<MediaCodecBuffer> &buffer) {
  sp<AMessage> notify(mNotify->dup());
  notify->setInt32("what", kWhatFillThisBuffer);
  notify->setSize("index", index);
  notify->setObject("buffer", buffer);
  notify->post();
}
void BufferCallback::onOutputBufferAvailable(
    size_t index, const sp<MediaCodecBuffer> &buffer) {
  sp<AMessage> notify(mNotify->dup());
  notify->setInt32("what", kWhatDrainThisBuffer);
  notify->setSize("index", index);
  notify->setObject("buffer", buffer);
  notify->post();
}
class CodecCallback : public CodecBase::CodecCallback {
 public:
  explicit CodecCallback(const sp<AMessage> &notify);
  virtual ~CodecCallback()
      virtual void onEos(status_t err) override;
  virtual void onStartCompleted() override;
  virtual void onStopCompleted() override;
  virtual void onReleaseCompleted() override;
  virtual void onFlushCompleted() override;
  virtual void onError(status_t err, enum ActionCode actionCode) override;
  virtual void onComponentAllocated(const char *componentName) override;
  virtual void onComponentConfigured(const sp<AMessage> &inputFormat,
                                     const sp<AMessage> &outputFormat) override;
  virtual void onInputSurfaceCreated(
      const sp<AMessage> &inputFormat, const sp<AMessage> &outputFormat,
      const sp<BufferProducerWrapper> &inputSurface) override;
  virtual void onInputSurfaceCreationFailed(status_t err) override;
  virtual void onInputSurfaceAccepted(
      const sp<AMessage> &inputFormat,
      const sp<AMessage> &outputFormat) override;
  virtual void onInputSurfaceDeclined(status_t err) override;
  virtual void onSignaledInputEOS(status_t err) override;
  virtual void onOutputFramesRendered(
      const std::list<FrameRenderTracker::Info> &done) override;
  virtual void onOutputBuffersChanged() override;
 private:
  const sp<AMessage> mNotify;
};
CodecCallback::CodecCallback(const sp<AMessage> &notify) : mNotify(notify) {}
void CodecCallback::onEos(status_t err) {
  sp<AMessage> notify(mNotify->dup());
  notify->setInt32("what", kWhatEOS);
  notify->setInt32("err", err);
  notify->post();
}
void CodecCallback::onStartCompleted() {
  sp<AMessage> notify(mNotify->dup());
  notify->setInt32("what", kWhatStartCompleted);
  notify->post();
}
void CodecCallback::onStopCompleted() {
  sp<AMessage> notify(mNotify->dup());
  notify->setInt32("what", kWhatStopCompleted);
  notify->post();
}
void CodecCallback::onReleaseCompleted() {
  sp<AMessage> notify(mNotify->dup());
  notify->setInt32("what", kWhatReleaseCompleted);
  notify->post();
}
void CodecCallback::onFlushCompleted() {
  sp<AMessage> notify(mNotify->dup());
  notify->setInt32("what", kWhatFlushCompleted);
  notify->post();
}
void CodecCallback::onError(status_t err, enum ActionCode actionCode) {
  sp<AMessage> notify(mNotify->dup());
  notify->setInt32("what", kWhatError);
  notify->setInt32("err", err);
  notify->setInt32("actionCode", actionCode);
  notify->post();
}
void CodecCallback::onComponentAllocated(const char *componentName) {
  sp<AMessage> notify(mNotify->dup());
  notify->setInt32("what", kWhatComponentAllocated);
  notify->setString("componentName", componentName);
  notify->post();
}
void CodecCallback::onComponentConfigured(const sp<AMessage> &inputFormat,
                                          const sp<AMessage> &outputFormat) {
  sp<AMessage> notify(mNotify->dup());
  notify->setInt32("what", kWhatComponentConfigured);
  notify->setMessage("input-format", inputFormat);
  notify->setMessage("output-format", outputFormat);
  notify->post();
}
void CodecCallback::onInputSurfaceCreated(
    const sp<AMessage> &inputFormat, const sp<AMessage> &outputFormat,
    const sp<BufferProducerWrapper> &inputSurface) {
  sp<AMessage> notify(mNotify->dup());
  notify->setInt32("what", kWhatInputSurfaceCreated);
  notify->setMessage("input-format", inputFormat);
  notify->setMessage("output-format", outputFormat);
  notify->setObject("input-surface", inputSurface);
  notify->post();
}
void CodecCallback::onInputSurfaceCreationFailed(status_t err) {
  sp<AMessage> notify(mNotify->dup());
  notify->setInt32("what", kWhatInputSurfaceCreated);
  notify->setInt32("err", err);
  notify->post();
}
void CodecCallback::onInputSurfaceAccepted(const sp<AMessage> &inputFormat,
                                           const sp<AMessage> &outputFormat) {
  sp<AMessage> notify(mNotify->dup());
  notify->setInt32("what", kWhatInputSurfaceAccepted);
  notify->setMessage("input-format", inputFormat);
  notify->setMessage("output-format", outputFormat);
  notify->post();
}
void CodecCallback::onInputSurfaceDeclined(status_t err) {
  sp<AMessage> notify(mNotify->dup());
  notify->setInt32("what", kWhatInputSurfaceAccepted);
  notify->setInt32("err", err);
  notify->post();
}
void CodecCallback::onSignaledInputEOS(status_t err) {
  sp<AMessage> notify(mNotify->dup());
  notify->setInt32("what", kWhatSignaledInputEOS);
  if (err != OK) {
    notify->setInt32("err", err);
  }
  notify->post();
}
void CodecCallback::onOutputFramesRendered(
    const std::list<FrameRenderTracker::Info> &done) {
  sp<AMessage> notify(mNotify->dup());
  notify->setInt32("what", kWhatOutputFramesRendered);
  if (MediaCodec::CreateFramesRenderedMessage(done, notify)) {
    notify->post();
  }
}
void CodecCallback::onOutputBuffersChanged() {
  sp<AMessage> notify(mNotify->dup());
  notify->setInt32("what", kWhatOutputBuffersChanged);
  notify->post();
}
}
sp<MediaCodec> MediaCodec::CreateByType(const sp<ALooper> &looper,
                                        const AString &mime, bool encoder,
                                        status_t *err, pid_t pid, uid_t uid) {
  Vector<AString> matchingCodecs;
  MediaCodecList::findMatchingCodecs(mime.c_str(), encoder, 0, &matchingCodecs);
  if (err != NULL) {
    *err = NAME_NOT_FOUND;
  }
  for (size_t i = 0; i < matchingCodecs.size(); ++i) {
    sp<MediaCodec> codec = new MediaCodec(looper, pid, uid);
    AString componentName = matchingCodecs[i];
    status_t ret = codec->init(componentName);
    if (err != NULL) {
      *err = ret;
    }
    if (ret == OK) {
      return codec;
    }
    ALOGD("Allocating component '%s' failed (%d), try next one.",
          componentName.c_str(), ret);
  }
  return NULL;
}
sp<MediaCodec> MediaCodec::CreateByComponentName(const sp<ALooper> &looper,
                                                 const AString &name,
                                                 status_t *err, pid_t pid,
                                                 uid_t uid) {
  sp<MediaCodec> codec = new MediaCodec(looper, pid, uid);
  const status_t ret = codec->init(name);
  if (err != NULL) {
    *err = ret;
  }
  return ret == OK ? codec : NULL;
}
sp<PersistentSurface> MediaCodec::CreatePersistentInputSurface() {
  sp<PersistentSurface> pluginSurface = CCodec::CreateInputSurface();
  if (pluginSurface != nullptr) {
    return pluginSurface;
  }
  OMXClient client;
  if (client.connect() != OK) {
    ALOGE("Failed to connect to OMX to create persistent input surface.");
    return NULL;
  }
  sp<IOMX> omx = client.interface();
  sp<IGraphicBufferProducer> bufferProducer;
  sp<hardware::media::omx::V1_0::IGraphicBufferSource> bufferSource;
  status_t err = omx->createInputSurface(&bufferProducer, &bufferSource);
  if (err != OK) {
    ALOGE("Failed to create persistent input surface.");
    return NULL;
  }
  return new PersistentSurface(bufferProducer, bufferSource);
}
MediaCodec::MediaCodec(const sp<ALooper> &looper, pid_t pid, uid_t uid)
    : mState(UNINITIALIZED),
      mReleasedByResourceManager(false),
      mLooper(looper),
      mCodec(NULL),
      mReplyID(0),
      mFlags(0),
      mStickyError(OK),
      mSoftRenderer(NULL),
      mAnalyticsItem(NULL),
      mMetricsHandle(0),
      mIsVideo(false),
      mVideoWidth(0),
      mVideoHeight(0),
      mRotationDegrees(0),
      mDequeueInputTimeoutGeneration(0),
      mDequeueInputReplyID(0),
      mDequeueOutputTimeoutGeneration(0),
      mDequeueOutputReplyID(0),
      mHaveInputSurface(false),
      mHavePendingInputBuffers(false),
      mCpuBoostRequested(false),
      mLatencyUnknown(0),
      mNumLowLatencyEnables(0),
      mNumLowLatencyDisables(0),
      mIsLowLatencyModeOn(false),
      mIndexOfFirstFrameWhenLowLatencyOn(-1),
      mInputBufferCounter(0) {
  if (uid == kNoUid) {
    mUid = AIBinder_getCallingUid();
  } else {
    mUid = uid;
  }
  mResourceManagerProxy = new ResourceManagerServiceProxy(
      pid, mUid, ::ndk::SharedRefBase::make<ResourceManagerClient>(this));
  initMediametrics();
}
MediaCodec::~MediaCodec() {
  CHECK_EQ(mState, UNINITIALIZED);
  mResourceManagerProxy->removeClient();
  flushMediametrics();
}
void MediaCodec::initMediametrics() {
  if (mMetricsHandle == 0) {
    mMetricsHandle = mediametrics_create(kCodecKeyName);
  }
  mLatencyHist.setup(kLatencyHistBuckets, kLatencyHistWidth, kLatencyHistFloor);
  {
    Mutex::Autolock al(mRecentLock);
    for (int i = 0; i < kRecentLatencyFrames; i++) {
      mRecentSamples[i] = kRecentSampleInvalid;
    }
    mRecentHead = 0;
  }
}
void MediaCodec::updateMediametrics() {
  ALOGV("MediaCodec::updateMediametrics");
  if (mMetricsHandle == 0) {
    return;
  }
  if (mLatencyHist.getCount() != 0) {
    mediametrics_setInt64(mMetricsHandle, kCodecLatencyMax,
                          mLatencyHist.getMax());
    mediametrics_setInt64(mMetricsHandle, kCodecLatencyMin,
                          mLatencyHist.getMin());
    mediametrics_setInt64(mMetricsHandle, kCodecLatencyAvg,
                          mLatencyHist.getAvg());
    mediametrics_setInt64(mMetricsHandle, kCodecLatencyCount,
                          mLatencyHist.getCount());
    if (kEmitHistogram) {
      std::string hist = mLatencyHist.emit();
      mediametrics_setCString(mMetricsHandle, kCodecLatencyHist, hist.c_str());
    }
  }
  if (mLatencyUnknown > 0) {
    mediametrics_setInt64(mMetricsHandle, kCodecLatencyUnknown,
                          mLatencyUnknown);
  }
#if 0
    updateEphemeralMediametrics(mMetricsHandle);
#endif
}
void MediaCodec::updateEphemeralMediametrics(mediametrics_handle_t item) {
  ALOGD("MediaCodec::updateEphemeralMediametrics()");
  if (item == 0) {
    return;
  }
  Histogram recentHist;
  recentHist.setup(kLatencyHistBuckets, kLatencyHistWidth, kLatencyHistFloor);
  {
    Mutex::Autolock al(mRecentLock);
    for (int i = 0; i < kRecentLatencyFrames; i++) {
      if (mRecentSamples[i] != kRecentSampleInvalid) {
        recentHist.insert(mRecentSamples[i]);
      }
    }
  }
  if (recentHist.getCount() != 0) {
    mediametrics_setInt64(item, kCodecRecentLatencyMax, recentHist.getMax());
    mediametrics_setInt64(item, kCodecRecentLatencyMin, recentHist.getMin());
    mediametrics_setInt64(item, kCodecRecentLatencyAvg, recentHist.getAvg());
    mediametrics_setInt64(item, kCodecRecentLatencyCount,
                          recentHist.getCount());
    if (kEmitHistogram) {
      std::string hist = recentHist.emit();
      mediametrics_setCString(item, kCodecRecentLatencyHist, hist.c_str());
    }
  }
}
void MediaCodec::flushMediametrics() {
  updateMediametrics();
  if (mMetricsHandle != 0) {
    if (mediametrics_count(mMetricsHandle) > 0) {
      mediametrics_selfRecord(mMetricsHandle);
    }
    mediametrics_delete(mMetricsHandle);
    mMetricsHandle = 0;
  }
}
void MediaCodec::updateLowLatency(const sp<AMessage> &msg) {
  int32_t lowLatency = 0;
  if (msg->findInt32("low-latency", &lowLatency)) {
    Mutex::Autolock al(mLatencyLock);
    if (lowLatency > 0) {
      ++mNumLowLatencyEnables;
      mIsLowLatencyModeOn = true;
    } else if (lowLatency == 0) {
      ++mNumLowLatencyDisables;
      mIsLowLatencyModeOn = false;
    }
  }
}
std::string MediaCodec::Histogram::emit() {
  std::string value;
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "%" PRId64 ",%" PRId64 ",%" PRId64 "{",
           mFloor, mWidth, mBelow);
  value = buffer;
  for (int i = 0; i < mBucketCount; i++) {
    if (i != 0) {
      value = value + ",";
    }
    snprintf(buffer, sizeof(buffer), "%" PRId64, mBuckets[i]);
    value = value + buffer;
  }
  snprintf(buffer, sizeof(buffer), "}%" PRId64, mAbove);
  value = value + buffer;
  return value;
}
std::string MediaCodec::Histogram::emit() {
  std::string value;
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "%" PRId64 ",%" PRId64 ",%" PRId64 "{",
           mFloor, mWidth, mBelow);
  value = buffer;
  for (int i = 0; i < mBucketCount; i++) {
    if (i != 0) {
      value = value + ",";
    }
    snprintf(buffer, sizeof(buffer), "%" PRId64, mBuckets[i]);
    value = value + buffer;
  }
  snprintf(buffer, sizeof(buffer), "}%" PRId64, mAbove);
  value = value + buffer;
  return value;
}
std::string MediaCodec::Histogram::emit() {
  std::string value;
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "%" PRId64 ",%" PRId64 ",%" PRId64 "{",
           mFloor, mWidth, mBelow);
  value = buffer;
  for (int i = 0; i < mBucketCount; i++) {
    if (i != 0) {
      value = value + ",";
    }
    snprintf(buffer, sizeof(buffer), "%" PRId64, mBuckets[i]);
    value = value + buffer;
  }
  snprintf(buffer, sizeof(buffer), "}%" PRId64, mAbove);
  value = value + buffer;
  return value;
}
void MediaCodec::statsBufferSent(int64_t presentationUs) {
  if (presentationUs <= 0) {
    ALOGV("presentation time: %" PRId64, presentationUs);
    return;
  }
  if (mBatteryChecker != nullptr) {
    mBatteryChecker->onCodecActivity([this]() {
      mResourceManagerProxy->addResource(MediaResource::VideoBatteryResource());
    });
  }
  const int64_t nowNs = systemTime(SYSTEM_TIME_MONOTONIC);
  BufferFlightTiming_t startdata = {presentationUs, nowNs};
  {
    Mutex::Autolock al(mLatencyLock);
    mBuffersInFlight.push_back(startdata);
    if (mIsLowLatencyModeOn && mIndexOfFirstFrameWhenLowLatencyOn < 0) {
      mIndexOfFirstFrameWhenLowLatencyOn = mInputBufferCounter;
    }
    ++mInputBufferCounter;
  }
}
void MediaCodec::statsBufferReceived(int64_t presentationUs) {
  CHECK_NE(mState, UNINITIALIZED);
  Mutex::Autolock al(mLatencyLock);
  if (presentationUs <= 0) {
    ALOGV("-- returned buffer timestamp %" PRId64 " <= 0, ignore it",
          presentationUs);
    mLatencyUnknown++;
    return;
  }
  if (mBatteryChecker != nullptr) {
    mBatteryChecker->onCodecActivity([this]() {
      mResourceManagerProxy->addResource(MediaResource::VideoBatteryResource());
    });
  }
  BufferFlightTiming_t startdata;
  bool valid = false;
  while (mBuffersInFlight.size() > 0) {
    startdata = *mBuffersInFlight.begin();
    ALOGV("-- Looking at startdata. presentation %" PRId64 ", start %" PRId64,
          startdata.presentationUs, startdata.startedNs);
    if (startdata.presentationUs == presentationUs) {
      ALOGV("-- match entry for %" PRId64 ", hits our frame of %" PRId64,
            startdata.presentationUs, presentationUs);
      mBuffersInFlight.pop_front();
      valid = true;
      break;
    } else if (startdata.presentationUs < presentationUs) {
      ALOGV("--  drop entry for %" PRId64 ", before our frame of %" PRId64,
            startdata.presentationUs, presentationUs);
      mBuffersInFlight.pop_front();
      continue;
    } else {
      ALOGV("--  found entry for %" PRId64 ", AFTER our frame of %" PRId64
            " we have nothing to pair with",
            startdata.presentationUs, presentationUs);
      mLatencyUnknown++;
      return;
    }
  }
  if (!valid) {
    ALOGV("-- empty queue, so ignore that.");
    mLatencyUnknown++;
    return;
  }
  const int64_t nowNs = systemTime(SYSTEM_TIME_MONOTONIC);
  int64_t latencyUs = (nowNs - startdata.startedNs + 500) / 1000;
  mLatencyHist.insert(latencyUs);
  {
    Mutex::Autolock al(mRecentLock);
    if (mRecentHead >= kRecentLatencyFrames) {
      mRecentHead = 0;
    }
    mRecentSamples[mRecentHead++] = latencyUs;
  }
}
status_t MediaCodec::PostAndAwaitResponse(const sp<AMessage> &msg,
                                          sp<AMessage> *response) {
  status_t err = msg->postAndAwaitResponse(response);
  if (err != OK) {
    return err;
  }
  if (!(*response)->findInt32("err", &err)) {
    err = OK;
  }
  return err;
}
void MediaCodec::PostReplyWithError(const sp<AReplyToken> &replyID,
                                    int32_t err) {
  int32_t finalErr = err;
  if (mReleasedByResourceManager) {
    finalErr = DEAD_OBJECT;
  }
  sp<AMessage> response = new AMessage;
  response->setInt32("err", finalErr);
  response->postReply(replyID);
}
static CodecBase *CreateCCodec() { return new CCodec; }
sp<CodecBase> MediaCodec::GetCodecBase(const AString &name, const char *owner) {
  if (owner) {
    if (strcmp(owner, "default") == 0) {
      return new ACodec;
    } else if (strncmp(owner, "codec2", 6) == 0) {
      return CreateCCodec();
    }
  }
  if (name.startsWithIgnoreCase("c2.")) {
    return CreateCCodec();
  } else if (name.startsWithIgnoreCase("omx.")) {
    return new ACodec;
  } else if (name.startsWithIgnoreCase("android.filter.")) {
    return new MediaFilter;
  } else {
    return NULL;
  }
}
status_t MediaCodec::init(const AString &name) {
  mResourceManagerProxy->init();
  mInitName = name;
  mCodecInfo.clear();
  bool secureCodec = false;
  const char *owner = "";
  if (!name.startsWith("android.filter.")) {
    AString tmp = name;
    if (tmp.endsWith(".secure")) {
      secureCodec = true;
      tmp.erase(tmp.size() - 7, 7);
    }
    const sp<IMediaCodecList> mcl = MediaCodecList::getInstance();
    if (mcl == NULL) {
      mCodec = NULL;
      return NO_INIT;
    }
    for (const AString &codecName : {name, tmp}) {
      ssize_t codecIdx = mcl->findCodecByName(codecName.c_str());
      if (codecIdx < 0) {
        continue;
      }
      mCodecInfo = mcl->getCodecInfo(codecIdx);
      Vector<AString> mediaTypes;
      mCodecInfo->getSupportedMediaTypes(&mediaTypes);
      for (size_t i = 0; i < mediaTypes.size(); i++) {
        if (mediaTypes[i].startsWith("video/")) {
          mIsVideo = true;
          break;
        }
      }
      break;
    }
    if (mCodecInfo == nullptr) {
      return NAME_NOT_FOUND;
    }
    owner = mCodecInfo->getOwnerName();
  }
  mCodec = GetCodecBase(name, owner);
  if (mCodec == NULL) {
    return NAME_NOT_FOUND;
  }
  if (mIsVideo) {
    if (mCodecLooper == NULL) {
      mCodecLooper = new ALooper;
      mCodecLooper->setName("CodecLooper");
      mCodecLooper->start(false, false, ANDROID_PRIORITY_AUDIO);
    }
    mCodecLooper->registerHandler(mCodec);
  } else {
    mLooper->registerHandler(mCodec);
  }
  mLooper->registerHandler(this);
  mCodec->setCallback(std::unique_ptr<CodecBase::CodecCallback>(
      new CodecCallback(new AMessage(kWhatCodecNotify, this))));
  mBufferChannel = mCodec->getBufferChannel();
  mBufferChannel->setCallback(std::unique_ptr<CodecBase::BufferCallback>(
      new BufferCallback(new AMessage(kWhatCodecNotify, this))));
  sp<AMessage> msg = new AMessage(kWhatInit, this);
  if (mCodecInfo) {
    msg->setObject("codecInfo", mCodecInfo);
  }
  msg->setString("name", name);
  if (mMetricsHandle != 0) {
    mediametrics_setCString(mMetricsHandle, kCodecCodec, name.c_str());
    mediametrics_setCString(mMetricsHandle, kCodecMode,
                            mIsVideo ? kCodecModeVideo : kCodecModeAudio);
  }
  if (mIsVideo) {
    mBatteryChecker =
        new BatteryChecker(new AMessage(kWhatCheckBatteryStats, this));
  }
  status_t err;
  std::vector<MediaResourceParcel> resources;
  resources.push_back(MediaResource::CodecResource(secureCodec, mIsVideo));
  for (int i = 0; i <= kMaxRetry; ++i) {
    if (i > 0) {
      if (!mResourceManagerProxy->reclaimResource(resources)) {
        break;
      }
    }
    sp<AMessage> response;
    err = PostAndAwaitResponse(msg, &response);
    if (!isResourceError(err)) {
      break;
    }
  }
  return err;
}
status_t MediaCodec::setCallback(const sp<AMessage> &callback) {
  sp<AMessage> msg = new AMessage(kWhatSetCallback, this);
  msg->setMessage("callback", callback);
  sp<AMessage> response;
  return PostAndAwaitResponse(msg, &response);
}
status_t MediaCodec::setOnFrameRenderedNotification(
    const sp<AMessage> &notify) {
  sp<AMessage> msg = new AMessage(kWhatSetNotification, this);
  msg->setMessage("on-frame-rendered", notify);
  return msg->post();
}
status_t MediaCodec::configure(const sp<AMessage> &format,
                               const sp<Surface> &nativeWindow,
                               const sp<ICrypto> &crypto, uint32_t flags) {
  return configure(format, nativeWindow, crypto, NULL, flags);
}
status_t MediaCodec::configure(const sp<AMessage> &format,
                               const sp<Surface> &surface,
                               const sp<ICrypto> &crypto,
                               const sp<IDescrambler> &descrambler,
                               uint32_t flags) {
  sp<AMessage> msg = new AMessage(kWhatConfigure, this);
  if (mMetricsHandle != 0) {
    int32_t profile = 0;
    if (format->findInt32("profile", &profile)) {
      mediametrics_setInt32(mMetricsHandle, kCodecProfile, profile);
    }
    int32_t level = 0;
    if (format->findInt32("level", &level)) {
      mediametrics_setInt32(mMetricsHandle, kCodecLevel, level);
    }
    mediametrics_setInt32(mMetricsHandle, kCodecEncoder,
                          (flags & CONFIGURE_FLAG_ENCODE) ? 1 : 0);
  }
  if (mIsVideo) {
    format->findInt32("width", &mVideoWidth);
    format->findInt32("height", &mVideoHeight);
    if (!format->findInt32("rotation-degrees", &mRotationDegrees)) {
      mRotationDegrees = 0;
    }
    if (mMetricsHandle != 0) {
      mediametrics_setInt32(mMetricsHandle, kCodecWidth, mVideoWidth);
      mediametrics_setInt32(mMetricsHandle, kCodecHeight, mVideoHeight);
      mediametrics_setInt32(mMetricsHandle, kCodecRotation, mRotationDegrees);
      int32_t maxWidth = 0;
      if (format->findInt32("max-width", &maxWidth)) {
        mediametrics_setInt32(mMetricsHandle, kCodecMaxWidth, maxWidth);
      }
      int32_t maxHeight = 0;
      if (format->findInt32("max-height", &maxHeight)) {
        mediametrics_setInt32(mMetricsHandle, kCodecMaxHeight, maxHeight);
      }
    }
    if (mVideoWidth < 0 || mVideoHeight < 0 ||
        (uint64_t)mVideoWidth * mVideoHeight > (uint64_t)INT32_MAX / 4) {
      ALOGE("Invalid size(s), width=%d, height=%d", mVideoWidth, mVideoHeight);
      return BAD_VALUE;
    }
  }
  updateLowLatency(format);
  msg->setMessage("format", format);
  msg->setInt32("flags", flags);
  msg->setObject("surface", surface);
  if (crypto != NULL || descrambler != NULL) {
    if (crypto != NULL) {
      msg->setPointer("crypto", crypto.get());
    } else {
      msg->setPointer("descrambler", descrambler.get());
    }
    if (mMetricsHandle != 0) {
      mediametrics_setInt32(mMetricsHandle, kCodecCrypto, 1);
    }
  } else if (mFlags & kFlagIsSecure) {
    ALOGW("Crypto or descrambler should be given for secure codec");
  }
  mConfigureMsg = msg;
  status_t err;
  std::vector<MediaResourceParcel> resources;
  resources.push_back(
      MediaResource::CodecResource(mFlags & kFlagIsSecure, mIsVideo));
  resources.push_back(MediaResource::GraphicMemoryResource(1));
  for (int i = 0; i <= kMaxRetry; ++i) {
    if (i > 0) {
      if (!mResourceManagerProxy->reclaimResource(resources)) {
        break;
      }
    }
    sp<AMessage> response;
    err = PostAndAwaitResponse(msg, &response);
    if (err != OK && err != INVALID_OPERATION) {
      ALOGE("configure failed with err 0x%08x, resetting...", err);
      reset();
    }
    if (!isResourceError(err)) {
      break;
    }
  }
  return err;
}
status_t MediaCodec::releaseCrypto() {
  ALOGV("releaseCrypto");
  sp<AMessage> msg = new AMessage(kWhatDrmReleaseCrypto, this);
  sp<AMessage> response;
  status_t status = msg->postAndAwaitResponse(&response);
  if (status == OK && response != NULL) {
    CHECK(response->findInt32("status", &status));
    ALOGV("releaseCrypto ret: %d ", status);
  } else {
    ALOGE("releaseCrypto err: %d", status);
  }
  return status;
}
void MediaCodec::onReleaseCrypto(const sp<AMessage> &msg) {
  status_t status = INVALID_OPERATION;
  if (mCrypto != NULL) {
    ALOGV("onReleaseCrypto: mCrypto: %p (%d)", mCrypto.get(),
          mCrypto->getStrongCount());
    mBufferChannel->setCrypto(NULL);
    ALOGD("onReleaseCrypto: [before clear]  mCrypto: %p (%d)", mCrypto.get(),
          mCrypto->getStrongCount());
    mCrypto.clear();
    status = OK;
  } else {
    ALOGW("onReleaseCrypto: No mCrypto. err: %d", status);
  }
  sp<AMessage> response = new AMessage;
  response->setInt32("status", status);
  sp<AReplyToken> replyID;
  CHECK(msg->senderAwaitsResponse(&replyID));
  response->postReply(replyID);
}
status_t MediaCodec::setInputSurface(const sp<PersistentSurface> &surface) {
  sp<AMessage> msg = new AMessage(kWhatSetInputSurface, this);
  msg->setObject("input-surface", surface.get());
  sp<AMessage> response;
  return PostAndAwaitResponse(msg, &response);
}
status_t MediaCodec::setSurface(const sp<Surface> &surface) {
  sp<AMessage> msg = new AMessage(kWhatSetSurface, this);
  msg->setObject("surface", surface);
  sp<AMessage> response;
  return PostAndAwaitResponse(msg, &response);
}
status_t MediaCodec::createInputSurface(
    sp<IGraphicBufferProducer> *bufferProducer) {
  sp<AMessage> msg = new AMessage(kWhatCreateInputSurface, this);
  sp<AMessage> response;
  status_t err = PostAndAwaitResponse(msg, &response);
  if (err == NO_ERROR) {
    sp<RefBase> obj;
    bool found = response->findObject("input-surface", &obj);
    CHECK(found);
    sp<BufferProducerWrapper> wrapper(
        static_cast<BufferProducerWrapper *>(obj.get()));
    *bufferProducer = wrapper->getBufferProducer();
  } else {
    ALOGW("createInputSurface failed, err=%d", err);
  }
  return err;
}
uint64_t MediaCodec::getGraphicBufferSize() {
  if (!mIsVideo) {
    return 0;
  }
  uint64_t size = 0;
  size_t portNum = sizeof(mPortBuffers) / sizeof((mPortBuffers)[0]);
  for (size_t i = 0; i < portNum; ++i) {
    size += mPortBuffers[i].size() * mVideoWidth * mVideoHeight * 3 / 2;
  }
  return size;
}
status_t MediaCodec::start() {
  sp<AMessage> msg = new AMessage(kWhatStart, this);
  status_t err;
  std::vector<MediaResourceParcel> resources;
  resources.push_back(
      MediaResource::CodecResource(mFlags & kFlagIsSecure, mIsVideo));
  resources.push_back(MediaResource::GraphicMemoryResource(1));
  for (int i = 0; i <= kMaxRetry; ++i) {
    if (i > 0) {
      if (!mResourceManagerProxy->reclaimResource(resources)) {
        break;
      }
      err = reset();
      if (err != OK) {
        ALOGE("retrying start: failed to reset codec");
        break;
      }
      sp<AMessage> response;
      err = PostAndAwaitResponse(mConfigureMsg, &response);
      if (err != OK) {
        ALOGE("retrying start: failed to configure codec");
        break;
      }
    }
    sp<AMessage> response;
    err = PostAndAwaitResponse(msg, &response);
    if (!isResourceError(err)) {
      break;
    }
  }
  return err;
}
status_t MediaCodec::stop() {
  sp<AMessage> msg = new AMessage(kWhatStop, this);
  sp<AMessage> response;
  return PostAndAwaitResponse(msg, &response);
}
bool MediaCodec::hasPendingBuffer(int portIndex) {
  return std::any_of(
      mPortBuffers[portIndex].begin(), mPortBuffers[portIndex].end(),
      [](const BufferInfo &info) { return info.mOwnedByClient; });
}
bool MediaCodec::hasPendingBuffer() {
  return hasPendingBuffer(kPortIndexInput) ||
         hasPendingBuffer(kPortIndexOutput);
}
status_t MediaCodec::reclaim(bool force) {
  ALOGD("MediaCodec::reclaim(%p) %s", this, mInitName.c_str());
  sp<AMessage> msg = new AMessage(kWhatRelease, this);
  msg->setInt32("reclaimed", 1);
  msg->setInt32("force", force ? 1 : 0);
  sp<AMessage> response;
  status_t ret = PostAndAwaitResponse(msg, &response);
  if (ret == -ENOENT) {
    ALOGD("MediaCodec looper is gone, skip reclaim");
    ret = OK;
  }
  return ret;
}
status_t MediaCodec::release() {
  sp<AMessage> msg = new AMessage(kWhatRelease, this);
  sp<AMessage> response;
  return PostAndAwaitResponse(msg, &response);
}
status_t MediaCodec::reset() {
  status_t err = release();
  if (mCodec != NULL) {
    if (mCodecLooper != NULL) {
      mCodecLooper->unregisterHandler(mCodec->id());
    } else {
      mLooper->unregisterHandler(mCodec->id());
    }
    mCodec = NULL;
  }
  mLooper->unregisterHandler(id());
  mFlags = 0;
  mStickyError = OK;
  mReplyID = 0;
  mDequeueInputReplyID = 0;
  mDequeueOutputReplyID = 0;
  mDequeueInputTimeoutGeneration = 0;
  mDequeueOutputTimeoutGeneration = 0;
  mHaveInputSurface = false;
  if (err == OK) {
    err = init(mInitName);
  }
  return err;
}
status_t MediaCodec::queueInputBuffer(size_t index, size_t offset, size_t size,
                                      int64_t presentationTimeUs,
                                      uint32_t flags, AString *errorDetailMsg) {
  if (errorDetailMsg != NULL) {
    errorDetailMsg->clear();
  }
  sp<AMessage> msg = new AMessage(kWhatQueueInputBuffer, this);
  msg->setSize("index", index);
  msg->setSize("offset", offset);
  msg->setSize("size", size);
  msg->setInt64("timeUs", presentationTimeUs);
  msg->setInt32("flags", flags);
  msg->setPointer("errorDetailMsg", errorDetailMsg);
  sp<AMessage> response;
  return PostAndAwaitResponse(msg, &response);
}
status_t MediaCodec::queueSecureInputBuffer(
    size_t index, size_t offset, const CryptoPlugin::SubSample *subSamples,
    size_t numSubSamples, const uint8_t key[16], const uint8_t iv[16],
    CryptoPlugin::Mode mode, const CryptoPlugin::Pattern &pattern,
    int64_t presentationTimeUs, uint32_t flags, AString *errorDetailMsg) {
  if (errorDetailMsg != NULL) {
    errorDetailMsg->clear();
  }
  sp<AMessage> msg = new AMessage(kWhatQueueInputBuffer, this);
  msg->setSize("index", index);
  msg->setSize("offset", offset);
  msg->setPointer("subSamples", (void *)subSamples);
  msg->setSize("numSubSamples", numSubSamples);
  msg->setPointer("key", (void *)key);
  msg->setPointer("iv", (void *)iv);
  msg->setInt32("mode", mode);
  msg->setInt32("encryptBlocks", pattern.mEncryptBlocks);
  msg->setInt32("skipBlocks", pattern.mSkipBlocks);
  msg->setInt64("timeUs", presentationTimeUs);
  msg->setInt32("flags", flags);
  msg->setPointer("errorDetailMsg", errorDetailMsg);
  sp<AMessage> response;
  status_t err = PostAndAwaitResponse(msg, &response);
  return err;
}
status_t MediaCodec::dequeueInputBuffer(size_t *index, int64_t timeoutUs) {
  sp<AMessage> msg = new AMessage(kWhatDequeueInputBuffer, this);
  msg->setInt64("timeoutUs", timeoutUs);
  sp<AMessage> response;
  status_t err;
  if ((err = PostAndAwaitResponse(msg, &response)) != OK) {
    return err;
  }
  CHECK(response->findSize("index", index));
  return OK;
}
status_t MediaCodec::dequeueOutputBuffer(size_t *index, size_t *offset,
                                         size_t *size,
                                         int64_t *presentationTimeUs,
                                         uint32_t *flags, int64_t timeoutUs) {
  sp<AMessage> msg = new AMessage(kWhatDequeueOutputBuffer, this);
  msg->setInt64("timeoutUs", timeoutUs);
  sp<AMessage> response;
  status_t err;
  if ((err = PostAndAwaitResponse(msg, &response)) != OK) {
    return err;
  }
  CHECK(response->findSize("index", index));
  CHECK(response->findSize("offset", offset));
  CHECK(response->findSize("size", size));
  CHECK(response->findInt64("timeUs", presentationTimeUs));
  CHECK(response->findInt32("flags", (int32_t *)flags));
  return OK;
}
status_t MediaCodec::renderOutputBufferAndRelease(size_t index) {
  sp<AMessage> msg = new AMessage(kWhatReleaseOutputBuffer, this);
  msg->setSize("index", index);
  msg->setInt32("render", true);
  sp<AMessage> response;
  return PostAndAwaitResponse(msg, &response);
}
status_t MediaCodec::renderOutputBufferAndRelease(size_t index,
                                                  int64_t timestampNs) {
  sp<AMessage> msg = new AMessage(kWhatReleaseOutputBuffer, this);
  msg->setSize("index", index);
  msg->setInt32("render", true);
  msg->setInt64("timestampNs", timestampNs);
  sp<AMessage> response;
  return PostAndAwaitResponse(msg, &response);
}
status_t MediaCodec::releaseOutputBuffer(size_t index) {
  sp<AMessage> msg = new AMessage(kWhatReleaseOutputBuffer, this);
  msg->setSize("index", index);
  sp<AMessage> response;
  return PostAndAwaitResponse(msg, &response);
}
status_t MediaCodec::signalEndOfInputStream() {
  sp<AMessage> msg = new AMessage(kWhatSignalEndOfInputStream, this);
  sp<AMessage> response;
  return PostAndAwaitResponse(msg, &response);
}
status_t MediaCodec::getOutputFormat(sp<AMessage> *format) const {
  sp<AMessage> msg = new AMessage(kWhatGetOutputFormat, this);
  sp<AMessage> response;
  status_t err;
  if ((err = PostAndAwaitResponse(msg, &response)) != OK) {
    return err;
  }
  CHECK(response->findMessage("format", format));
  return OK;
}
status_t MediaCodec::getInputFormat(sp<AMessage> *format) const {
  sp<AMessage> msg = new AMessage(kWhatGetInputFormat, this);
  sp<AMessage> response;
  status_t err;
  if ((err = PostAndAwaitResponse(msg, &response)) != OK) {
    return err;
  }
  CHECK(response->findMessage("format", format));
  return OK;
}
status_t MediaCodec::getName(AString *name) const {
  sp<AMessage> msg = new AMessage(kWhatGetName, this);
  sp<AMessage> response;
  status_t err;
  if ((err = PostAndAwaitResponse(msg, &response)) != OK) {
    return err;
  }
  CHECK(response->findString("name", name));
  return OK;
}
status_t MediaCodec::getCodecInfo(sp<MediaCodecInfo> *codecInfo) const {
  sp<AMessage> msg = new AMessage(kWhatGetCodecInfo, this);
  sp<AMessage> response;
  status_t err;
  if ((err = PostAndAwaitResponse(msg, &response)) != OK) {
    return err;
  }
  sp<RefBase> obj;
  CHECK(response->findObject("codecInfo", &obj));
  *codecInfo = static_cast<MediaCodecInfo *>(obj.get());
  return OK;
}
status_t MediaCodec::getMetrics(mediametrics_handle_t &reply) {
  reply = 0;
  if (mMetricsHandle == 0) {
    return UNKNOWN_ERROR;
  }
  updateMediametrics();
  reply = mediametrics_dup(mMetricsHandle);
  updateEphemeralMediametrics(reply);
  return OK;
}
status_t MediaCodec::getInputBuffers(
    Vector<sp<MediaCodecBuffer> > *buffers) const {
  sp<AMessage> msg = new AMessage(kWhatGetBuffers, this);
  msg->setInt32("portIndex", kPortIndexInput);
  msg->setPointer("buffers", buffers);
  sp<AMessage> response;
  return PostAndAwaitResponse(msg, &response);
}
status_t MediaCodec::getOutputBuffers(
    Vector<sp<MediaCodecBuffer> > *buffers) const {
  sp<AMessage> msg = new AMessage(kWhatGetBuffers, this);
  msg->setInt32("portIndex", kPortIndexOutput);
  msg->setPointer("buffers", buffers);
  sp<AMessage> response;
  return PostAndAwaitResponse(msg, &response);
}
status_t MediaCodec::getOutputBuffer(size_t index,
                                     sp<MediaCodecBuffer> *buffer) {
  sp<AMessage> format;
  return getBufferAndFormat(kPortIndexOutput, index, buffer, &format);
}
status_t MediaCodec::getOutputFormat(size_t index, sp<AMessage> *format) {
  sp<MediaCodecBuffer> buffer;
  return getBufferAndFormat(kPortIndexOutput, index, &buffer, format);
}
status_t MediaCodec::getInputBuffer(size_t index,
                                    sp<MediaCodecBuffer> *buffer) {
  sp<AMessage> format;
  return getBufferAndFormat(kPortIndexInput, index, buffer, &format);
}
bool MediaCodec::isExecuting() const {
  return mState == STARTED || mState == FLUSHED;
}
status_t MediaCodec::getBufferAndFormat(size_t portIndex, size_t index,
                                        sp<MediaCodecBuffer> *buffer,
                                        sp<AMessage> *format) {
  if (mReleasedByResourceManager) {
    ALOGE("getBufferAndFormat - resource already released");
    return DEAD_OBJECT;
  }
  if (buffer == NULL) {
    ALOGE("getBufferAndFormat - null MediaCodecBuffer");
    return INVALID_OPERATION;
  }
  if (format == NULL) {
    ALOGE("getBufferAndFormat - null AMessage");
    return INVALID_OPERATION;
  }
  buffer->clear();
  format->clear();
  if (!isExecuting()) {
    ALOGE("getBufferAndFormat - not executing");
    return INVALID_OPERATION;
  }
  Mutex::Autolock al(mBufferLock);
  std::vector<BufferInfo> &buffers = mPortBuffers[portIndex];
  if (index >= buffers.size()) {
    ALOGE(
        "getBufferAndFormat - trying to get buffer with "
        "bad index (index=%zu buffer_size=%zu)",
        index, buffers.size());
    return INVALID_OPERATION;
  }
  const BufferInfo &info = buffers[index];
  if (!info.mOwnedByClient) {
    ALOGE(
        "getBufferAndFormat - invalid operation "
        "(the index %zu is not owned by client)",
        index);
    return INVALID_OPERATION;
  }
  *buffer = info.mData;
  *format = info.mData->format();
  return OK;
}
status_t MediaCodec::flush() {
  sp<AMessage> msg = new AMessage(kWhatFlush, this);
  sp<AMessage> response;
  return PostAndAwaitResponse(msg, &response);
}
status_t MediaCodec::requestIDRFrame() {
  (new AMessage(kWhatRequestIDRFrame, this))->post();
  return OK;
}
void MediaCodec::requestActivityNotification(const sp<AMessage> &notify) {
  sp<AMessage> msg = new AMessage(kWhatRequestActivityNotification, this);
  msg->setMessage("notify", notify);
  msg->post();
}
void MediaCodec::requestCpuBoostIfNeeded() {
  if (mCpuBoostRequested) {
    return;
  }
  int32_t colorFormat;
  if (mOutputFormat->contains("hdr-static-info") &&
      mOutputFormat->findInt32("color-format", &colorFormat)
      && ((mSoftRenderer != NULL &&
           colorFormat == OMX_COLOR_FormatYUV420Planar16) ||
          mOwnerName.equalsIgnoreCase("codec2::software"))) {
    int32_t left, top, right, bottom, width, height;
    int64_t totalPixel = 0;
    if (mOutputFormat->findRect("crop", &left, &top, &right, &bottom)) {
      totalPixel = (right - left + 1) * (bottom - top + 1);
    } else if (mOutputFormat->findInt32("width", &width) &&
               mOutputFormat->findInt32("height", &height)) {
      totalPixel = width * height;
    }
    if (totalPixel >= 1920 * 1080) {
      mResourceManagerProxy->addResource(MediaResource::CpuBoostResource());
      mCpuBoostRequested = true;
    }
  }
}
BatteryChecker::BatteryChecker(const sp<AMessage> &msg, int64_t timeoutUs)
    : mTimeoutUs(timeoutUs),
      mLastActivityTimeUs(-1ll),
      mBatteryStatNotified(false),
      mBatteryCheckerGeneration(0),
      mIsExecuting(false),
      mBatteryCheckerMsg(msg) {}
void BatteryChecker::onCodecActivity(std::function<void()> batteryOnCb) {
  if (!isExecuting()) {
    return;
  }
  if (!mBatteryStatNotified) {
    batteryOnCb();
    mBatteryStatNotified = true;
    sp<AMessage> msg = mBatteryCheckerMsg->dup();
    msg->setInt32("generation", mBatteryCheckerGeneration);
    msg->post(mTimeoutUs);
    mLastActivityTimeUs = -1ll;
  } else {
    mLastActivityTimeUs = ALooper::GetNowUs();
  }
}
void BatteryChecker::onCheckBatteryTimer(const sp<AMessage> &msg,
                                         std::function<void()> batteryOffCb) {
  int32_t generation;
  if (!msg->findInt32("generation", &generation) ||
      generation != mBatteryCheckerGeneration) {
    return;
  }
  if (mLastActivityTimeUs < 0ll) {
    batteryOffCb();
    mBatteryStatNotified = false;
  } else {
    msg->post(mTimeoutUs + mLastActivityTimeUs - ALooper::GetNowUs());
    mLastActivityTimeUs = -1ll;
  }
}
void BatteryChecker::onClientRemoved() {
  mBatteryStatNotified = false;
  mBatteryCheckerGeneration++;
}
void MediaCodec::cancelPendingDequeueOperations() {
  if (mFlags & kFlagDequeueInputPending) {
    PostReplyWithError(mDequeueInputReplyID, INVALID_OPERATION);
    ++mDequeueInputTimeoutGeneration;
    mDequeueInputReplyID = 0;
    mFlags &= ~kFlagDequeueInputPending;
  }
  if (mFlags & kFlagDequeueOutputPending) {
    PostReplyWithError(mDequeueOutputReplyID, INVALID_OPERATION);
    ++mDequeueOutputTimeoutGeneration;
    mDequeueOutputReplyID = 0;
    mFlags &= ~kFlagDequeueOutputPending;
  }
}
bool MediaCodec::handleDequeueInputBuffer(const sp<AReplyToken> &replyID,
                                          bool newRequest) {
  if (!isExecuting() || (mFlags & kFlagIsAsync) ||
      (newRequest && (mFlags & kFlagDequeueInputPending))) {
    PostReplyWithError(replyID, INVALID_OPERATION);
    return true;
  } else if (mFlags & kFlagStickyError) {
    PostReplyWithError(replyID, getStickyError());
    return true;
  }
  ssize_t index = dequeuePortBuffer(kPortIndexInput);
  if (index < 0) {
    CHECK_EQ(index, -EAGAIN);
    return false;
  }
  sp<AMessage> response = new AMessage;
  response->setSize("index", index);
  response->postReply(replyID);
  return true;
}
bool MediaCodec::handleDequeueOutputBuffer(const sp<AReplyToken> &replyID,
                                           bool newRequest) {
  if (!isExecuting() || (mFlags & kFlagIsAsync) ||
      (newRequest && (mFlags & kFlagDequeueOutputPending))) {
    PostReplyWithError(replyID, INVALID_OPERATION);
  } else if (mFlags & kFlagStickyError) {
    PostReplyWithError(replyID, getStickyError());
  } else if (mFlags & kFlagOutputBuffersChanged) {
    PostReplyWithError(replyID, INFO_OUTPUT_BUFFERS_CHANGED);
    mFlags &= ~kFlagOutputBuffersChanged;
  } else if (mFlags & kFlagOutputFormatChanged) {
    PostReplyWithError(replyID, INFO_FORMAT_CHANGED);
    mFlags &= ~kFlagOutputFormatChanged;
  } else {
    sp<AMessage> response = new AMessage;
    ssize_t index = dequeuePortBuffer(kPortIndexOutput);
    if (index < 0) {
      CHECK_EQ(index, -EAGAIN);
      return false;
    }
    const sp<MediaCodecBuffer> &buffer =
        mPortBuffers[kPortIndexOutput][index].mData;
    response->setSize("index", index);
    response->setSize("offset", buffer->offset());
    response->setSize("size", buffer->size());
    int64_t timeUs;
    CHECK(buffer->meta()->findInt64("timeUs", &timeUs));
    statsBufferReceived(timeUs);
    response->setInt64("timeUs", timeUs);
    int32_t flags;
    CHECK(buffer->meta()->findInt32("flags", &flags));
    response->setInt32("flags", flags);
    response->postReply(replyID);
  }
  return true;
}
void MediaCodec::onMessageReceived(const sp<AMessage> &msg) {
  switch (msg->what()) {
    case kWhatCodecNotify: {
      int32_t what;
      CHECK(msg->findInt32("what", &what));
      switch (what) {
        case kWhatError: {
          int32_t err, actionCode;
          CHECK(msg->findInt32("err", &err));
          CHECK(msg->findInt32("actionCode", &actionCode));
          ALOGE("Codec reported err %#x, actionCode %d, while in state %d", err,
                actionCode, mState);
          if (err == DEAD_OBJECT) {
            mFlags |= kFlagSawMediaServerDie;
            mFlags &= ~kFlagIsComponentAllocated;
          }
          bool sendErrorResponse = true;
          switch (mState) {
            case INITIALIZING: {
              setState(UNINITIALIZED);
              break;
            }
            case CONFIGURING: {
              if (actionCode == ACTION_CODE_FATAL) {
                mediametrics_setInt32(mMetricsHandle, kCodecError, err);
                mediametrics_setCString(mMetricsHandle, kCodecErrorState,
                                        stateString(mState).c_str());
                flushMediametrics();
                initMediametrics();
              }
              setState(actionCode == ACTION_CODE_FATAL ? UNINITIALIZED
                                                       : INITIALIZED);
              break;
            }
            case STARTING: {
              if (actionCode == ACTION_CODE_FATAL) {
                mediametrics_setInt32(mMetricsHandle, kCodecError, err);
                mediametrics_setCString(mMetricsHandle, kCodecErrorState,
                                        stateString(mState).c_str());
                flushMediametrics();
                initMediametrics();
              }
              setState(actionCode == ACTION_CODE_FATAL ? UNINITIALIZED
                                                       : CONFIGURED);
              break;
            }
            case RELEASING: {
              sendErrorResponse = false;
              FALLTHROUGH_INTENDED;
            }
            case STOPPING: {
              if (mFlags & kFlagSawMediaServerDie) {
                setState(UNINITIALIZED);
                if (mState == RELEASING) {
                  mComponentName.clear();
                }
                (new AMessage)->postReply(mReplyID);
                sendErrorResponse = false;
              }
              break;
            }
            case FLUSHING: {
              if (actionCode == ACTION_CODE_FATAL) {
                mediametrics_setInt32(mMetricsHandle, kCodecError, err);
                mediametrics_setCString(mMetricsHandle, kCodecErrorState,
                                        stateString(mState).c_str());
                flushMediametrics();
                initMediametrics();
                setState(UNINITIALIZED);
              } else {
                setState((mFlags & kFlagIsAsync) ? FLUSHED : STARTED);
              }
              break;
            }
            case FLUSHED:
            case STARTED: {
              sendErrorResponse = false;
              setStickyError(err);
              postActivityNotificationIfPossible();
              cancelPendingDequeueOperations();
              if (mFlags & kFlagIsAsync) {
                onError(err, actionCode);
              }
              switch (actionCode) {
                case ACTION_CODE_TRANSIENT:
                  break;
                case ACTION_CODE_RECOVERABLE:
                  setState(INITIALIZED);
                  break;
                default:
                  mediametrics_setInt32(mMetricsHandle, kCodecError, err);
                  mediametrics_setCString(mMetricsHandle, kCodecErrorState,
                                          stateString(mState).c_str());
                  flushMediametrics();
                  initMediametrics();
                  setState(UNINITIALIZED);
                  break;
              }
              break;
            }
            default: {
              sendErrorResponse = false;
              setStickyError(err);
              postActivityNotificationIfPossible();
              if (mState == UNINITIALIZED) {
                actionCode = ACTION_CODE_FATAL;
              }
              if (mFlags & kFlagIsAsync) {
                onError(err, actionCode);
              }
              switch (actionCode) {
                case ACTION_CODE_TRANSIENT:
                  break;
                case ACTION_CODE_RECOVERABLE:
                  setState(INITIALIZED);
                  break;
                default:
                  setState(UNINITIALIZED);
                  break;
              }
              break;
            }
          }
          if (sendErrorResponse) {
            PostReplyWithError(mReplyID, err);
          }
          break;
        }
        case kWhatComponentAllocated: {
          if (mState == RELEASING || mState == UNINITIALIZED) {
            ALOGW("allocate interrupted by error or release, current state %d",
                  mState);
            break;
          }
          CHECK_EQ(mState, INITIALIZING);
          setState(INITIALIZED);
          mFlags |= kFlagIsComponentAllocated;
          CHECK(msg->findString("componentName", &mComponentName));
          if (mComponentName.c_str()) {
            mediametrics_setCString(mMetricsHandle, kCodecCodec,
                                    mComponentName.c_str());
          }
          const char *owner = mCodecInfo ? mCodecInfo->getOwnerName() : "";
          if (mComponentName.startsWith("OMX.google.") &&
              strncmp(owner, "default", 8) == 0) {
            mFlags |= kFlagUsesSoftwareRenderer;
          } else {
            mFlags &= ~kFlagUsesSoftwareRenderer;
          }
          mOwnerName = owner;
          if (mComponentName.endsWith(".secure")) {
            mFlags |= kFlagIsSecure;
<<<<<<< HEAD
            mediametrics_setInt32(mMetricsHandle, kCodecSecure, 1);
||||||| faafe2989a
            resourceType = MediaResource::kSecureCodec;
            mAnalyticsItem->setInt32(kCodecSecure, 1);
=======
            resourceType = MediaResource::kSecureCodec;
            mediametrics_setInt32(mMetricsHandle, kCodecSecure, 1);
>>>>>>> e23cbd26568fdd243d8dc6e052e664b885101688
          } else {
            mFlags &= ~kFlagIsSecure;
<<<<<<< HEAD
            mediametrics_setInt32(mMetricsHandle, kCodecSecure, 0);
||||||| faafe2989a
            resourceType = MediaResource::kNonSecureCodec;
            mAnalyticsItem->setInt32(kCodecSecure, 0);
=======
            resourceType = MediaResource::kNonSecureCodec;
            mediametrics_setInt32(mMetricsHandle, kCodecSecure, 0);
>>>>>>> e23cbd26568fdd243d8dc6e052e664b885101688
          }
          if (mIsVideo) {
            mResourceManagerProxy->addResource(
                MediaResource::CodecResource(mFlags & kFlagIsSecure, mIsVideo));
          }
          (new AMessage)->postReply(mReplyID);
          break;
        }
        case kWhatComponentConfigured: {
          if (mState == RELEASING || mState == UNINITIALIZED ||
              mState == INITIALIZED) {
            ALOGW("configure interrupted by error or release, current state %d",
                  mState);
            break;
          }
          CHECK_EQ(mState, CONFIGURING);
          mHaveInputSurface = false;
          CHECK(msg->findMessage("input-format", &mInputFormat));
          CHECK(msg->findMessage("output-format", &mOutputFormat));
          if (mSurface != nullptr && !mAllowFrameDroppingBySurface) {
            mInputFormat->setInt32("allow-frame-drop",
                                   mAllowFrameDroppingBySurface);
          }
          ALOGV("[%s] configured as input format: %s, output format: %s",
                mComponentName.c_str(), mInputFormat->debugString(4).c_str(),
                mOutputFormat->debugString(4).c_str());
          int32_t usingSwRenderer;
          if (mOutputFormat->findInt32("using-sw-renderer", &usingSwRenderer) &&
              usingSwRenderer) {
            mFlags |= kFlagUsesSoftwareRenderer;
          }
          setState(CONFIGURED);
          (new AMessage)->postReply(mReplyID);
          if (mMetricsHandle != 0) {
            sp<AMessage> format;
            if (mConfigureMsg != NULL &&
                mConfigureMsg->findMessage("format", &format)) {
              AString mime;
              if (format->findString("mime", &mime)) {
                mediametrics_setCString(mMetricsHandle, kCodecMime,
                                        mime.c_str());
              }
            }
          }
          break;
        }
        case kWhatInputSurfaceCreated: {
          status_t err = NO_ERROR;
          sp<AMessage> response = new AMessage;
          if (!msg->findInt32("err", &err)) {
            sp<RefBase> obj;
            msg->findObject("input-surface", &obj);
            CHECK(msg->findMessage("input-format", &mInputFormat));
            CHECK(msg->findMessage("output-format", &mOutputFormat));
            ALOGV(
                "[%s] input surface created as input format: %s, output "
                "format: %s",
                mComponentName.c_str(), mInputFormat->debugString(4).c_str(),
                mOutputFormat->debugString(4).c_str());
            CHECK(obj != NULL);
            response->setObject("input-surface", obj);
            mHaveInputSurface = true;
          } else {
            response->setInt32("err", err);
          }
          response->postReply(mReplyID);
          break;
        }
        case kWhatInputSurfaceAccepted: {
          status_t err = NO_ERROR;
          sp<AMessage> response = new AMessage();
          if (!msg->findInt32("err", &err)) {
            CHECK(msg->findMessage("input-format", &mInputFormat));
            CHECK(msg->findMessage("output-format", &mOutputFormat));
            mHaveInputSurface = true;
          } else {
            response->setInt32("err", err);
          }
          response->postReply(mReplyID);
          break;
        }
        case kWhatSignaledInputEOS: {
          sp<AMessage> response = new AMessage;
          status_t err;
          if (msg->findInt32("err", &err)) {
            response->setInt32("err", err);
          }
          response->postReply(mReplyID);
          break;
        }
        case kWhatStartCompleted: {
          if (mState == RELEASING || mState == UNINITIALIZED) {
            ALOGW("start interrupted by release, current state %d", mState);
            break;
          }
          CHECK_EQ(mState, STARTING);
          if (mIsVideo) {
            mResourceManagerProxy->addResource(
                MediaResource::GraphicMemoryResource(getGraphicBufferSize()));
          }
          setState(STARTED);
          (new AMessage)->postReply(mReplyID);
          break;
        }
        case kWhatOutputBuffersChanged: {
          mFlags |= kFlagOutputBuffersChanged;
          postActivityNotificationIfPossible();
          break;
        }
        case kWhatOutputFramesRendered: {
          if (mState == STARTED && mOnFrameRenderedNotification != NULL) {
            sp<AMessage> notify = mOnFrameRenderedNotification->dup();
            notify->setMessage("data", msg);
            notify->post();
          }
          break;
        }
        case kWhatFillThisBuffer: {
                               updateBuffers(kPortIndexInput, msg);
          if (mState == FLUSHING || mState == STOPPING || mState == RELEASING) {
            returnBuffersToCodecOnPort(kPortIndexInput);
            break;
          }
          if (!mCSD.empty()) {
            ssize_t index = dequeuePortBuffer(kPortIndexInput);
            CHECK_GE(index, 0);
            status_t err = queueCSDInputBuffer(index);
            if (err != OK) {
              ALOGE("queueCSDInputBuffer failed w/ error %d", err);
              setStickyError(err);
              postActivityNotificationIfPossible();
              cancelPendingDequeueOperations();
            }
            break;
          }
          if (mFlags & kFlagIsAsync) {
            if (!mHaveInputSurface) {
              if (mState == FLUSHED) {
                mHavePendingInputBuffers = true;
              } else {
                onInputBufferAvailable();
              }
            }
          } else if (mFlags & kFlagDequeueInputPending) {
            CHECK(handleDequeueInputBuffer(mDequeueInputReplyID));
            ++mDequeueInputTimeoutGeneration;
            mFlags &= ~kFlagDequeueInputPending;
            mDequeueInputReplyID = 0;
          } else {
            postActivityNotificationIfPossible();
          }
          break;
        }
        case kWhatDrainThisBuffer: {
                               updateBuffers(kPortIndexOutput, msg);
          if (mState == FLUSHING || mState == STOPPING || mState == RELEASING) {
            returnBuffersToCodecOnPort(kPortIndexOutput);
            break;
          }
          sp<RefBase> obj;
          CHECK(msg->findObject("buffer", &obj));
          sp<MediaCodecBuffer> buffer =
              static_cast<MediaCodecBuffer *>(obj.get());
          if (mOutputFormat != buffer->format()) {
            mOutputFormat = buffer->format();
            ALOGV("[%s] output format changed to: %s", mComponentName.c_str(),
                  mOutputFormat->debugString(4).c_str());
            if (mSoftRenderer == NULL && mSurface != NULL &&
                (mFlags & kFlagUsesSoftwareRenderer)) {
              AString mime;
              CHECK(mOutputFormat->findString("mime", &mime));
              int32_t dataSpace;
              if (mOutputFormat->findInt32("android._dataspace", &dataSpace)) {
                ALOGD("[%s] setting dataspace on output surface to #%x",
                      mComponentName.c_str(), dataSpace);
                int err = native_window_set_buffers_data_space(
                    mSurface.get(), (android_dataspace)dataSpace);
                ALOGW_IF(err != 0, "failed to set dataspace on surface (%d)",
                         err);
              }
              if (mOutputFormat->contains("hdr-static-info")) {
                HDRStaticInfo info;
                if (ColorUtils::getHDRStaticInfoFromFormat(mOutputFormat,
                                                           &info)) {
                  setNativeWindowHdrMetadata(mSurface.get(), &info);
                }
              }
              sp<ABuffer> hdr10PlusInfo;
              if (mOutputFormat->findBuffer("hdr10-plus-info",
                                            &hdr10PlusInfo) &&
                  hdr10PlusInfo != nullptr && hdr10PlusInfo->size() > 0) {
                native_window_set_buffers_hdr10_plus_metadata(
                    mSurface.get(), hdr10PlusInfo->size(),
                    hdr10PlusInfo->data());
              }
              if (mime.startsWithIgnoreCase("video/")) {
                mSurface->setDequeueTimeout(-1);
                mSoftRenderer =
                    new SoftwareRenderer(mSurface, mRotationDegrees);
              }
            }
            requestCpuBoostIfNeeded();
            if (mFlags & kFlagIsEncoder) {
              int32_t flags = 0;
              (void)buffer->meta()->findInt32("flags", &flags);
              if (flags & BUFFER_FLAG_CODECCONFIG) {
                status_t err = amendOutputFormatWithCodecSpecificData(buffer);
                if (err != OK) {
                  ALOGE(
                      "Codec spit out malformed codec "
                      "specific data!");
                }
              }
            }
            if (mFlags & kFlagIsAsync) {
              onOutputFormatChanged();
            } else {
              mFlags |= kFlagOutputFormatChanged;
              postActivityNotificationIfPossible();
            }
            if (mCrypto != NULL) {
              int32_t left, top, right, bottom, width, height;
              if (mOutputFormat->findRect("crop", &left, &top, &right,
                                          &bottom)) {
                mCrypto->notifyResolution(right - left + 1, bottom - top + 1);
              } else if (mOutputFormat->findInt32("width", &width) &&
                         mOutputFormat->findInt32("height", &height)) {
                mCrypto->notifyResolution(width, height);
              }
            }
          }
          if (mFlags & kFlagIsAsync) {
            onOutputBufferAvailable();
          } else if (mFlags & kFlagDequeueOutputPending) {
            CHECK(handleDequeueOutputBuffer(mDequeueOutputReplyID));
            ++mDequeueOutputTimeoutGeneration;
            mFlags &= ~kFlagDequeueOutputPending;
            mDequeueOutputReplyID = 0;
          } else {
            postActivityNotificationIfPossible();
          }
          break;
        }
        case kWhatEOS: {
          break;
        }
        case kWhatStopCompleted: {
          if (mState != STOPPING) {
            ALOGW("Received kWhatStopCompleted in state %d", mState);
            break;
          }
          setState(INITIALIZED);
          (new AMessage)->postReply(mReplyID);
          break;
        }
        case kWhatReleaseCompleted: {
          if (mState != RELEASING) {
            ALOGW("Received kWhatReleaseCompleted in state %d", mState);
            break;
          }
          setState(UNINITIALIZED);
          mComponentName.clear();
          mFlags &= ~kFlagIsComponentAllocated;
          if (mBatteryChecker != nullptr) {
            mBatteryChecker->onClientRemoved();
          }
          mResourceManagerProxy->removeClient();
          (new AMessage)->postReply(mReplyID);
          break;
        }
        case kWhatFlushCompleted: {
          if (mState != FLUSHING) {
            ALOGW("received FlushCompleted message in state %d", mState);
            break;
          }
          if (mFlags & kFlagIsAsync) {
            setState(FLUSHED);
          } else {
            setState(STARTED);
            mCodec->signalResume();
          }
          (new AMessage)->postReply(mReplyID);
          break;
        }
        default:
          TRESPASS();
      }
      break;
    }
    case kWhatInit: {
      sp<AReplyToken> replyID;
      CHECK(msg->senderAwaitsResponse(&replyID));
      if (mState != UNINITIALIZED) {
        PostReplyWithError(replyID, INVALID_OPERATION);
        break;
      }
      mReplyID = replyID;
      setState(INITIALIZING);
      sp<RefBase> codecInfo;
      (void)msg->findObject("codecInfo", &codecInfo);
      AString name;
      CHECK(msg->findString("name", &name));
      sp<AMessage> format = new AMessage;
      if (codecInfo) {
        format->setObject("codecInfo", codecInfo);
      }
      format->setString("componentName", name);
      mCodec->initiateAllocateComponent(format);
      break;
    }
    case kWhatSetNotification: {
      sp<AMessage> notify;
      if (msg->findMessage("on-frame-rendered", &notify)) {
        mOnFrameRenderedNotification = notify;
      }
      break;
    }
    case kWhatSetCallback: {
      sp<AReplyToken> replyID;
      CHECK(msg->senderAwaitsResponse(&replyID));
      if (mState == UNINITIALIZED || mState == INITIALIZING || isExecuting()) {
        PostReplyWithError(replyID, INVALID_OPERATION);
        break;
      }
      sp<AMessage> callback;
      CHECK(msg->findMessage("callback", &callback));
      mCallback = callback;
      if (mCallback != NULL) {
        ALOGI("MediaCodec will operate in async mode");
        mFlags |= kFlagIsAsync;
      } else {
        mFlags &= ~kFlagIsAsync;
      }
      sp<AMessage> response = new AMessage;
      response->postReply(replyID);
      break;
    }
    case kWhatConfigure: {
      sp<AReplyToken> replyID;
      CHECK(msg->senderAwaitsResponse(&replyID));
      if (mState != INITIALIZED) {
        PostReplyWithError(replyID, INVALID_OPERATION);
        break;
      }
      sp<RefBase> obj;
      CHECK(msg->findObject("surface", &obj));
      sp<AMessage> format;
      CHECK(msg->findMessage("format", &format));
      int32_t push;
      if (msg->findInt32("push-blank-buffers-on-shutdown", &push) &&
          push != 0) {
        mFlags |= kFlagPushBlankBuffersOnShutdown;
      }
      if (obj != NULL) {
        if (!format->findInt32("allow-frame-drop",
                               &mAllowFrameDroppingBySurface)) {
          mAllowFrameDroppingBySurface = true;
        }
        format->setObject("native-window", obj);
        status_t err = handleSetSurface(static_cast<Surface *>(obj.get()));
        if (err != OK) {
          PostReplyWithError(replyID, err);
          break;
        }
      } else {
        mAllowFrameDroppingBySurface = false;
        handleSetSurface(NULL);
      }
      mReplyID = replyID;
      setState(CONFIGURING);
      void *crypto;
      if (!msg->findPointer("crypto", &crypto)) {
        crypto = NULL;
      }
      ALOGV("kWhatConfigure: Old mCrypto: %p (%d)", mCrypto.get(),
            (mCrypto != NULL ? mCrypto->getStrongCount() : 0));
      mCrypto = static_cast<ICrypto *>(crypto);
      mBufferChannel->setCrypto(mCrypto);
      ALOGV("kWhatConfigure: New mCrypto: %p (%d)", mCrypto.get(),
            (mCrypto != NULL ? mCrypto->getStrongCount() : 0));
      void *descrambler;
      if (!msg->findPointer("descrambler", &descrambler)) {
        descrambler = NULL;
      }
      mDescrambler = static_cast<IDescrambler *>(descrambler);
      mBufferChannel->setDescrambler(mDescrambler);
      uint32_t flags;
      CHECK(msg->findInt32("flags", (int32_t *)&flags));
      if (flags & CONFIGURE_FLAG_ENCODE) {
        format->setInt32("encoder", true);
        mFlags |= kFlagIsEncoder;
      }
      extractCSD(format);
      mCodec->initiateConfigureComponent(format);
      break;
    }
    case kWhatSetSurface: {
      sp<AReplyToken> replyID;
      CHECK(msg->senderAwaitsResponse(&replyID));
      status_t err = OK;
      switch (mState) {
        case CONFIGURED:
        case STARTED:
        case FLUSHED: {
          sp<RefBase> obj;
          (void)msg->findObject("surface", &obj);
          sp<Surface> surface = static_cast<Surface *>(obj.get());
          if (mSurface == NULL) {
            err = INVALID_OPERATION;
          } else if (obj == NULL) {
            err = BAD_VALUE;
          } else {
            err = connectToSurface(surface);
            if (err == ALREADY_EXISTS) {
              err = OK;
            } else {
              if (err == OK) {
                if (mFlags & kFlagUsesSoftwareRenderer) {
                  if (mSoftRenderer != NULL &&
                      (mFlags & kFlagPushBlankBuffersOnShutdown)) {
                    pushBlankBuffersToNativeWindow(mSurface.get());
                  }
                  surface->setDequeueTimeout(-1);
                  mSoftRenderer = new SoftwareRenderer(surface);
                } else {
                  err = mCodec->setSurface(surface);
                }
              }
              if (err == OK) {
                (void)disconnectFromSurface();
                mSurface = surface;
              }
            }
          }
          break;
        }
        default:
          err = INVALID_OPERATION;
          break;
      }
      PostReplyWithError(replyID, err);
      break;
    }
    case kWhatCreateInputSurface:
    case kWhatSetInputSurface: {
      sp<AReplyToken> replyID;
      CHECK(msg->senderAwaitsResponse(&replyID));
      if (mState != CONFIGURED) {
        PostReplyWithError(replyID, INVALID_OPERATION);
        break;
      }
      mReplyID = replyID;
      if (msg->what() == kWhatCreateInputSurface) {
        mCodec->initiateCreateInputSurface();
      } else {
        sp<RefBase> obj;
        CHECK(msg->findObject("input-surface", &obj));
        mCodec->initiateSetInputSurface(
            static_cast<PersistentSurface *>(obj.get()));
      }
      break;
    }
    case kWhatStart: {
      sp<AReplyToken> replyID;
      CHECK(msg->senderAwaitsResponse(&replyID));
      if (mState == FLUSHED) {
        setState(STARTED);
        if (mHavePendingInputBuffers) {
          onInputBufferAvailable();
          mHavePendingInputBuffers = false;
        }
        mCodec->signalResume();
        PostReplyWithError(replyID, OK);
        break;
      } else if (mState != CONFIGURED) {
        PostReplyWithError(replyID, INVALID_OPERATION);
        break;
      }
      mReplyID = replyID;
      setState(STARTING);
      mCodec->initiateStart();
      break;
    }
    case kWhatStop:
    case kWhatRelease: {
      State targetState =
          (msg->what() == kWhatStop) ? INITIALIZED : UNINITIALIZED;
      sp<AReplyToken> replyID;
      CHECK(msg->senderAwaitsResponse(&replyID));
      if (mState == UNINITIALIZED && mReleasedByResourceManager) {
        sp<AMessage> response = new AMessage;
        response->setInt32("err", OK);
        response->postReply(replyID);
        break;
      }
      int32_t reclaimed = 0;
      msg->findInt32("reclaimed", &reclaimed);
      if (reclaimed) {
        mReleasedByResourceManager = true;
        int32_t force = 0;
        msg->findInt32("force", &force);
        if (!force && hasPendingBuffer()) {
          ALOGW("Can't reclaim codec right now due to pending buffers.");
          sp<AMessage> response = new AMessage;
          response->setInt32("err", WOULD_BLOCK);
          response->postReply(replyID);
          if (mFlags & kFlagIsAsync) {
            onError(DEAD_OBJECT, ACTION_CODE_FATAL);
          }
          break;
        }
      }
      bool isReleasingAllocatedComponent =
          (mFlags & kFlagIsComponentAllocated) && targetState == UNINITIALIZED;
      if (!isReleasingAllocatedComponent
          && mState != INITIALIZED && mState != CONFIGURED && !isExecuting()) {
        sp<AMessage> response = new AMessage;
        status_t err = mState == targetState ? OK : INVALID_OPERATION;
        response->setInt32("err", err);
        if (err == OK && targetState == UNINITIALIZED) {
          mComponentName.clear();
        }
        response->postReply(replyID);
        break;
      }
      if (mState == FLUSHING || mState == STOPPING || mState == CONFIGURING ||
          mState == STARTING) {
        (new AMessage)->postReply(mReplyID);
      }
      if (mFlags & kFlagSawMediaServerDie) {
        setState(UNINITIALIZED);
        if (targetState == UNINITIALIZED) {
          mComponentName.clear();
        }
        (new AMessage)->postReply(replyID);
        break;
      }
      if (msg->what() == kWhatStop && (mFlags & kFlagStickyError)) {
        PostReplyWithError(replyID, getStickyError());
        break;
      }
      mReplyID = replyID;
      setState(msg->what() == kWhatStop ? STOPPING : RELEASING);
      mCodec->initiateShutdown(msg->what() ==
                               kWhatStop );
      returnBuffersToCodec(reclaimed);
      if (mSoftRenderer != NULL && (mFlags & kFlagPushBlankBuffersOnShutdown)) {
        pushBlankBuffersToNativeWindow(mSurface.get());
      }
      break;
    }
    case kWhatDequeueInputBuffer: {
      sp<AReplyToken> replyID;
      CHECK(msg->senderAwaitsResponse(&replyID));
      if (mFlags & kFlagIsAsync) {
        ALOGE("dequeueInputBuffer can't be used in async mode");
        PostReplyWithError(replyID, INVALID_OPERATION);
        break;
      }
      if (mHaveInputSurface) {
        ALOGE("dequeueInputBuffer can't be used with input surface");
        PostReplyWithError(replyID, INVALID_OPERATION);
        break;
      }
      if (handleDequeueInputBuffer(replyID, true )) {
        break;
      }
      int64_t timeoutUs;
      CHECK(msg->findInt64("timeoutUs", &timeoutUs));
      if (timeoutUs == 0LL) {
        PostReplyWithError(replyID, -EAGAIN);
        break;
      }
      mFlags |= kFlagDequeueInputPending;
      mDequeueInputReplyID = replyID;
      if (timeoutUs > 0LL) {
        sp<AMessage> timeoutMsg = new AMessage(kWhatDequeueInputTimedOut, this);
        timeoutMsg->setInt32("generation", ++mDequeueInputTimeoutGeneration);
        timeoutMsg->post(timeoutUs);
      }
      break;
    }
    case kWhatDequeueInputTimedOut: {
      int32_t generation;
      CHECK(msg->findInt32("generation", &generation));
      if (generation != mDequeueInputTimeoutGeneration) {
        break;
      }
      CHECK(mFlags & kFlagDequeueInputPending);
      PostReplyWithError(mDequeueInputReplyID, -EAGAIN);
      mFlags &= ~kFlagDequeueInputPending;
      mDequeueInputReplyID = 0;
      break;
    }
    case kWhatQueueInputBuffer: {
      sp<AReplyToken> replyID;
      CHECK(msg->senderAwaitsResponse(&replyID));
      if (!isExecuting()) {
        PostReplyWithError(replyID, INVALID_OPERATION);
        break;
      } else if (mFlags & kFlagStickyError) {
        PostReplyWithError(replyID, getStickyError());
        break;
      }
      status_t err = onQueueInputBuffer(msg);
      PostReplyWithError(replyID, err);
      break;
    }
    case kWhatDequeueOutputBuffer: {
      sp<AReplyToken> replyID;
      CHECK(msg->senderAwaitsResponse(&replyID));
      if (mFlags & kFlagIsAsync) {
        ALOGE("dequeueOutputBuffer can't be used in async mode");
        PostReplyWithError(replyID, INVALID_OPERATION);
        break;
      }
      if (handleDequeueOutputBuffer(replyID, true )) {
        break;
      }
      int64_t timeoutUs;
      CHECK(msg->findInt64("timeoutUs", &timeoutUs));
      if (timeoutUs == 0LL) {
        PostReplyWithError(replyID, -EAGAIN);
        break;
      }
      mFlags |= kFlagDequeueOutputPending;
      mDequeueOutputReplyID = replyID;
      if (timeoutUs > 0LL) {
        sp<AMessage> timeoutMsg =
            new AMessage(kWhatDequeueOutputTimedOut, this);
        timeoutMsg->setInt32("generation", ++mDequeueOutputTimeoutGeneration);
        timeoutMsg->post(timeoutUs);
      }
      break;
    }
    case kWhatDequeueOutputTimedOut: {
      int32_t generation;
      CHECK(msg->findInt32("generation", &generation));
      if (generation != mDequeueOutputTimeoutGeneration) {
        break;
      }
      CHECK(mFlags & kFlagDequeueOutputPending);
      PostReplyWithError(mDequeueOutputReplyID, -EAGAIN);
      mFlags &= ~kFlagDequeueOutputPending;
      mDequeueOutputReplyID = 0;
      break;
    }
    case kWhatReleaseOutputBuffer: {
      sp<AReplyToken> replyID;
      CHECK(msg->senderAwaitsResponse(&replyID));
      if (!isExecuting()) {
        PostReplyWithError(replyID, INVALID_OPERATION);
        break;
      } else if (mFlags & kFlagStickyError) {
        PostReplyWithError(replyID, getStickyError());
        break;
      }
      status_t err = onReleaseOutputBuffer(msg);
      PostReplyWithError(replyID, err);
      break;
    }
    case kWhatSignalEndOfInputStream: {
      sp<AReplyToken> replyID;
      CHECK(msg->senderAwaitsResponse(&replyID));
      if (!isExecuting() || !mHaveInputSurface) {
        PostReplyWithError(replyID, INVALID_OPERATION);
        break;
      } else if (mFlags & kFlagStickyError) {
        PostReplyWithError(replyID, getStickyError());
        break;
      }
      mReplyID = replyID;
      mCodec->signalEndOfInputStream();
      break;
    }
    case kWhatGetBuffers: {
      sp<AReplyToken> replyID;
      CHECK(msg->senderAwaitsResponse(&replyID));
      if (!isExecuting() || (mFlags & kFlagIsAsync)) {
        PostReplyWithError(replyID, INVALID_OPERATION);
        break;
      } else if (mFlags & kFlagStickyError) {
        PostReplyWithError(replyID, getStickyError());
        break;
      }
      int32_t portIndex;
      CHECK(msg->findInt32("portIndex", &portIndex));
      Vector<sp<MediaCodecBuffer> > *dstBuffers;
      CHECK(msg->findPointer("buffers", (void **)&dstBuffers));
      dstBuffers->clear();
      if (portIndex != kPortIndexInput || !mHaveInputSurface) {
        if (portIndex == kPortIndexInput) {
          mBufferChannel->getInputBufferArray(dstBuffers);
        } else {
          mBufferChannel->getOutputBufferArray(dstBuffers);
        }
      }
      (new AMessage)->postReply(replyID);
      break;
    }
    case kWhatFlush: {
      sp<AReplyToken> replyID;
      CHECK(msg->senderAwaitsResponse(&replyID));
      if (!isExecuting()) {
        PostReplyWithError(replyID, INVALID_OPERATION);
        break;
      } else if (mFlags & kFlagStickyError) {
        PostReplyWithError(replyID, getStickyError());
        break;
      }
      mReplyID = replyID;
      setState(FLUSHING);
      mCodec->signalFlush();
      returnBuffersToCodec();
      break;
    }
    case kWhatGetInputFormat:
    case kWhatGetOutputFormat: {
      sp<AMessage> format =
          (msg->what() == kWhatGetOutputFormat ? mOutputFormat : mInputFormat);
      sp<AReplyToken> replyID;
      CHECK(msg->senderAwaitsResponse(&replyID));
      if ((mState != CONFIGURED && mState != STARTING && mState != STARTED &&
           mState != FLUSHING && mState != FLUSHED) ||
          format == NULL) {
        PostReplyWithError(replyID, INVALID_OPERATION);
        break;
      } else if (mFlags & kFlagStickyError) {
        PostReplyWithError(replyID, getStickyError());
        break;
      }
      sp<AMessage> response = new AMessage;
      response->setMessage("format", format);
      response->postReply(replyID);
      break;
    }
    case kWhatRequestIDRFrame: {
      mCodec->signalRequestIDRFrame();
      break;
    }
    case kWhatRequestActivityNotification: {
      CHECK(mActivityNotify == NULL);
      CHECK(msg->findMessage("notify", &mActivityNotify));
      postActivityNotificationIfPossible();
      break;
    }
    case kWhatGetName: {
      sp<AReplyToken> replyID;
      CHECK(msg->senderAwaitsResponse(&replyID));
      if (mComponentName.empty()) {
        PostReplyWithError(replyID, INVALID_OPERATION);
        break;
      }
      sp<AMessage> response = new AMessage;
      response->setString("name", mComponentName.c_str());
      response->postReply(replyID);
      break;
    }
    case kWhatGetCodecInfo: {
      sp<AReplyToken> replyID;
      CHECK(msg->senderAwaitsResponse(&replyID));
      sp<AMessage> response = new AMessage;
      response->setObject("codecInfo", mCodecInfo);
      response->postReply(replyID);
      break;
    }
    case kWhatSetParameters: {
      sp<AReplyToken> replyID;
      CHECK(msg->senderAwaitsResponse(&replyID));
      sp<AMessage> params;
      CHECK(msg->findMessage("params", &params));
      status_t err = onSetParameters(params);
      PostReplyWithError(replyID, err);
      break;
    }
    case kWhatDrmReleaseCrypto: {
      onReleaseCrypto(msg);
      break;
    }
    case kWhatCheckBatteryStats: {
      if (mBatteryChecker != nullptr) {
        mBatteryChecker->onCheckBatteryTimer(msg, [this]() {
          mResourceManagerProxy->removeResource(
              MediaResource::VideoBatteryResource());
        });
      }
      break;
    }
    default:
      TRESPASS();
  }
}
void MediaCodec::extractCSD(const sp<AMessage> &format) {
  mCSD.clear();
  size_t i = 0;
  for (;;) {
    sp<ABuffer> csd;
    if (!format->findBuffer(AStringPrintf("csd-%u", i).c_str(), &csd)) {
      break;
    }
    if (csd->size() == 0) {
      ALOGW("csd-%zu size is 0", i);
    }
    mCSD.push_back(csd);
    ++i;
  }
  ALOGV("Found %zu pieces of codec specific data.", mCSD.size());
}
status_t MediaCodec::queueCSDInputBuffer(size_t bufferIndex) {
  CHECK(!mCSD.empty());
  const BufferInfo &info = mPortBuffers[kPortIndexInput][bufferIndex];
  sp<ABuffer> csd = *mCSD.begin();
  mCSD.erase(mCSD.begin());
  const sp<MediaCodecBuffer> &codecInputData = info.mData;
  if (csd->size() > codecInputData->capacity()) {
    return -EINVAL;
  }
  if (codecInputData->data() == NULL) {
    ALOGV("Input buffer %zu is not properly allocated", bufferIndex);
    return -EINVAL;
  }
  memcpy(codecInputData->data(), csd->data(), csd->size());
  AString errorDetailMsg;
  sp<AMessage> msg = new AMessage(kWhatQueueInputBuffer, this);
  msg->setSize("index", bufferIndex);
  msg->setSize("offset", 0);
  msg->setSize("size", csd->size());
  msg->setInt64("timeUs", 0LL);
  msg->setInt32("flags", BUFFER_FLAG_CODECCONFIG);
  msg->setPointer("errorDetailMsg", &errorDetailMsg);
  return onQueueInputBuffer(msg);
}
void MediaCodec::setState(State newState) {
  if (newState == INITIALIZED || newState == UNINITIALIZED) {
    delete mSoftRenderer;
    mSoftRenderer = NULL;
    if (mCrypto != NULL) {
      ALOGV("setState: ~mCrypto: %p (%d)", mCrypto.get(),
            (mCrypto != NULL ? mCrypto->getStrongCount() : 0));
    }
    mCrypto.clear();
    mDescrambler.clear();
    handleSetSurface(NULL);
    mInputFormat.clear();
    mOutputFormat.clear();
    mFlags &= ~kFlagOutputFormatChanged;
    mFlags &= ~kFlagOutputBuffersChanged;
    mFlags &= ~kFlagStickyError;
    mFlags &= ~kFlagIsEncoder;
    mFlags &= ~kFlagIsAsync;
    mStickyError = OK;
    mActivityNotify.clear();
    mCallback.clear();
  }
  if (newState == UNINITIALIZED) {
    returnBuffersToCodec();
    mFlags &= ~kFlagSawMediaServerDie;
  }
  mState = newState;
  if (mBatteryChecker != nullptr) {
    mBatteryChecker->setExecuting(isExecuting());
  }
  cancelPendingDequeueOperations();
}
void MediaCodec::returnBuffersToCodec(bool isReclaim) {
  returnBuffersToCodecOnPort(kPortIndexInput, isReclaim);
  returnBuffersToCodecOnPort(kPortIndexOutput, isReclaim);
}
void MediaCodec::returnBuffersToCodecOnPort(int32_t portIndex, bool isReclaim) {
  CHECK(portIndex == kPortIndexInput || portIndex == kPortIndexOutput);
  Mutex::Autolock al(mBufferLock);
  for (size_t i = 0; i < mPortBuffers[portIndex].size(); ++i) {
    BufferInfo *info = &mPortBuffers[portIndex][i];
    if (info->mData != nullptr) {
      sp<MediaCodecBuffer> buffer = info->mData;
      if (isReclaim && info->mOwnedByClient) {
        ALOGD(
            "port %d buffer %zu still owned by client when codec is reclaimed",
            portIndex, i);
      } else {
        info->mOwnedByClient = false;
        info->mData.clear();
      }
      mBufferChannel->discardBuffer(buffer);
    }
  }
  mAvailPortBuffers[portIndex].clear();
}
size_t MediaCodec::updateBuffers(int32_t portIndex, const sp<AMessage> &msg) {
  CHECK(portIndex == kPortIndexInput || portIndex == kPortIndexOutput);
  size_t index;
  CHECK(msg->findSize("index", &index));
  sp<RefBase> obj;
  CHECK(msg->findObject("buffer", &obj));
  sp<MediaCodecBuffer> buffer = static_cast<MediaCodecBuffer *>(obj.get());
  {
    Mutex::Autolock al(mBufferLock);
    if (mPortBuffers[portIndex].size() <= index) {
      mPortBuffers[portIndex].resize(align(index + 1, kNumBuffersAlign));
    }
    mPortBuffers[portIndex][index].mData = buffer;
  }
  mAvailPortBuffers[portIndex].push_back(index);
  return index;
}
status_t MediaCodec::onQueueInputBuffer(const sp<AMessage> &msg) {
  size_t index;
  size_t offset;
  size_t size;
  int64_t timeUs;
  uint32_t flags;
  CHECK(msg->findSize("index", &index));
  CHECK(msg->findSize("offset", &offset));
  CHECK(msg->findInt64("timeUs", &timeUs));
  CHECK(msg->findInt32("flags", (int32_t *)&flags));
  const CryptoPlugin::SubSample *subSamples;
  size_t numSubSamples;
  const uint8_t *key;
  const uint8_t *iv;
  CryptoPlugin::Mode mode = CryptoPlugin::kMode_Unencrypted;
  CryptoPlugin::SubSample ss;
  CryptoPlugin::Pattern pattern;
  if (msg->findSize("size", &size)) {
    if (hasCryptoOrDescrambler()) {
      ss.mNumBytesOfClearData = size;
      ss.mNumBytesOfEncryptedData = 0;
      subSamples = &ss;
      numSubSamples = 1;
      key = NULL;
      iv = NULL;
      pattern.mEncryptBlocks = 0;
      pattern.mSkipBlocks = 0;
    }
  } else {
    if (!hasCryptoOrDescrambler()) {
      ALOGE("[%s] queuing secure buffer without mCrypto or mDescrambler!",
            mComponentName.c_str());
      return -EINVAL;
    }
    CHECK(msg->findPointer("subSamples", (void **)&subSamples));
    CHECK(msg->findSize("numSubSamples", &numSubSamples));
    CHECK(msg->findPointer("key", (void **)&key));
    CHECK(msg->findPointer("iv", (void **)&iv));
    CHECK(msg->findInt32("encryptBlocks", (int32_t *)&pattern.mEncryptBlocks));
    CHECK(msg->findInt32("skipBlocks", (int32_t *)&pattern.mSkipBlocks));
    int32_t tmp;
    CHECK(msg->findInt32("mode", &tmp));
    mode = (CryptoPlugin::Mode)tmp;
    size = 0;
    for (size_t i = 0; i < numSubSamples; ++i) {
      size += subSamples[i].mNumBytesOfClearData;
      size += subSamples[i].mNumBytesOfEncryptedData;
    }
  }
  if (index >= mPortBuffers[kPortIndexInput].size()) {
    return -ERANGE;
  }
  BufferInfo *info = &mPortBuffers[kPortIndexInput][index];
  if (info->mData == nullptr || !info->mOwnedByClient) {
    return -EACCES;
  }
  if (offset + size > info->mData->capacity()) {
    return -EINVAL;
  }
  info->mData->setRange(offset, size);
  info->mData->meta()->setInt64("timeUs", timeUs);
  if (flags & BUFFER_FLAG_EOS) {
    info->mData->meta()->setInt32("eos", true);
  }
  if (flags & BUFFER_FLAG_CODECCONFIG) {
    info->mData->meta()->setInt32("csd", true);
  }
  sp<MediaCodecBuffer> buffer = info->mData;
  status_t err = OK;
  if (hasCryptoOrDescrambler()) {
    AString *errorDetailMsg;
    CHECK(msg->findPointer("errorDetailMsg", (void **)&errorDetailMsg));
    err = mBufferChannel->queueSecureInputBuffer(
        buffer, (mFlags & kFlagIsSecure), key, iv, mode, pattern, subSamples,
        numSubSamples, errorDetailMsg);
  } else {
    err = mBufferChannel->queueInputBuffer(buffer);
  }
  if (err == OK) {
    Mutex::Autolock al(mBufferLock);
    info->mOwnedByClient = false;
    info->mData.clear();
    statsBufferSent(timeUs);
  }
  return err;
}
size_t MediaCodec::CreateFramesRenderedMessage(
    const std::list<FrameRenderTracker::Info> &done, sp<AMessage> &msg) {
  size_t index = 0;
  for (std::list<FrameRenderTracker::Info>::const_iterator it = done.cbegin();
       it != done.cend(); ++it) {
    if (it->getRenderTimeNs() < 0) {
      continue;
    }
    msg->setInt64(AStringPrintf("%zu-media-time-us", index).c_str(),
                  it->getMediaTimeUs());
    msg->setInt64(AStringPrintf("%zu-system-nano", index).c_str(),
                  it->getRenderTimeNs());
    ++index;
  }
  return index;
}
status_t MediaCodec::onReleaseOutputBuffer(const sp<AMessage> &msg) {
  size_t index;
  CHECK(msg->findSize("index", &index));
  int32_t render;
  if (!msg->findInt32("render", &render)) {
    render = 0;
  }
  if (!isExecuting()) {
    return -EINVAL;
  }
  if (index >= mPortBuffers[kPortIndexOutput].size()) {
    return -ERANGE;
  }
  BufferInfo *info = &mPortBuffers[kPortIndexOutput][index];
  if (info->mData == nullptr || !info->mOwnedByClient) {
    return -EACCES;
  }
  sp<MediaCodecBuffer> buffer;
  {
    Mutex::Autolock al(mBufferLock);
    info->mOwnedByClient = false;
    buffer = info->mData;
    info->mData.clear();
  }
  if (render && buffer->size() != 0) {
    int64_t mediaTimeUs = -1;
    buffer->meta()->findInt64("timeUs", &mediaTimeUs);
    int64_t renderTimeNs = 0;
    if (!msg->findInt64("timestampNs", &renderTimeNs)) {
      ALOGV("using buffer PTS of %lld", (long long)mediaTimeUs);
      renderTimeNs = mediaTimeUs * 1000;
    }
    if (mSoftRenderer != NULL) {
      std::list<FrameRenderTracker::Info> doneFrames = mSoftRenderer->render(
          buffer->data(), buffer->size(), mediaTimeUs, renderTimeNs,
          mPortBuffers[kPortIndexOutput].size(), buffer->format());
      if (!doneFrames.empty() && mState == STARTED &&
          mOnFrameRenderedNotification != NULL) {
        sp<AMessage> notify = mOnFrameRenderedNotification->dup();
        sp<AMessage> data = new AMessage;
        if (CreateFramesRenderedMessage(doneFrames, data)) {
          notify->setMessage("data", data);
          notify->post();
        }
      }
    }
    mBufferChannel->renderOutputBuffer(buffer, renderTimeNs);
  } else {
    mBufferChannel->discardBuffer(buffer);
  }
  return OK;
}
ssize_t MediaCodec::dequeuePortBuffer(int32_t portIndex) {
  CHECK(portIndex == kPortIndexInput || portIndex == kPortIndexOutput);
  List<size_t> *availBuffers = &mAvailPortBuffers[portIndex];
  if (availBuffers->empty()) {
    return -EAGAIN;
  }
  size_t index = *availBuffers->begin();
  availBuffers->erase(availBuffers->begin());
  BufferInfo *info = &mPortBuffers[portIndex][index];
  CHECK(!info->mOwnedByClient);
  {
    Mutex::Autolock al(mBufferLock);
    info->mOwnedByClient = true;
    if (info->mData->format() != NULL) {
      sp<ABuffer> imageData;
      if (info->mData->format()->findBuffer("image-data", &imageData)) {
        info->mData->meta()->setBuffer("image-data", imageData);
      }
      int32_t left, top, right, bottom;
      if (info->mData->format()->findRect("crop", &left, &top, &right,
                                          &bottom)) {
        info->mData->meta()->setRect("crop-rect", left, top, right, bottom);
      }
    }
  }
  return index;
}
status_t MediaCodec::connectToSurface(const sp<Surface> &surface) {
  status_t err = OK;
  if (surface != NULL) {
    uint64_t oldId, newId;
    if (mSurface != NULL && surface->getUniqueId(&newId) == NO_ERROR &&
        mSurface->getUniqueId(&oldId) == NO_ERROR && newId == oldId) {
      ALOGI("[%s] connecting to the same surface. Nothing to do.",
            mComponentName.c_str());
      return ALREADY_EXISTS;
    }
    err = nativeWindowConnect(surface.get(), "connectToSurface");
    if (err == OK) {
      static uint32_t mSurfaceGeneration = 0;
      uint32_t generation =
          (getpid() << 10) | (++mSurfaceGeneration & ((1 << 10) - 1));
      surface->setGenerationNumber(generation);
      ALOGI("[%s] setting surface generation to %u", mComponentName.c_str(),
            generation);
      nativeWindowDisconnect(surface.get(), "connectToSurface(reconnect)");
      err = nativeWindowConnect(surface.get(), "connectToSurface(reconnect)");
    }
    if (err != OK) {
      ALOGE("nativeWindowConnect returned an error: %s (%d)", strerror(-err),
            err);
    } else {
      if (!mAllowFrameDroppingBySurface) {
        disableLegacyBufferDropPostQ(surface);
      }
    }
  }
  return err == ALREADY_EXISTS ? BAD_VALUE : err;
}
status_t MediaCodec::disconnectFromSurface() {
  status_t err = OK;
  if (mSurface != NULL) {
    mSurface->setGenerationNumber(0);
    err = nativeWindowDisconnect(mSurface.get(), "disconnectFromSurface");
    if (err != OK) {
      ALOGW("nativeWindowDisconnect returned an error: %s (%d)", strerror(-err),
            err);
    }
    mSurface.clear();
  }
  return err;
}
status_t MediaCodec::handleSetSurface(const sp<Surface> &surface) {
  status_t err = OK;
  if (mSurface != NULL) {
    (void)disconnectFromSurface();
  }
  if (surface != NULL) {
    err = connectToSurface(surface);
    if (err == OK) {
      mSurface = surface;
    }
  }
  return err;
}
void MediaCodec::onInputBufferAvailable() {
  int32_t index;
  while ((index = dequeuePortBuffer(kPortIndexInput)) >= 0) {
    sp<AMessage> msg = mCallback->dup();
    msg->setInt32("callbackID", CB_INPUT_AVAILABLE);
    msg->setInt32("index", index);
    msg->post();
  }
}
void MediaCodec::onOutputBufferAvailable() {
  int32_t index;
  while ((index = dequeuePortBuffer(kPortIndexOutput)) >= 0) {
    const sp<MediaCodecBuffer> &buffer =
        mPortBuffers[kPortIndexOutput][index].mData;
    sp<AMessage> msg = mCallback->dup();
    msg->setInt32("callbackID", CB_OUTPUT_AVAILABLE);
    msg->setInt32("index", index);
    msg->setSize("offset", buffer->offset());
    msg->setSize("size", buffer->size());
    int64_t timeUs;
    CHECK(buffer->meta()->findInt64("timeUs", &timeUs));
    msg->setInt64("timeUs", timeUs);
    statsBufferReceived(timeUs);
    int32_t flags;
    CHECK(buffer->meta()->findInt32("flags", &flags));
    msg->setInt32("flags", flags);
    msg->post();
  }
}
void MediaCodec::onError(status_t err, int32_t actionCode, const char *detail) {
  if (mCallback != NULL) {
    sp<AMessage> msg = mCallback->dup();
    msg->setInt32("callbackID", CB_ERROR);
    msg->setInt32("err", err);
    msg->setInt32("actionCode", actionCode);
    if (detail != NULL) {
      msg->setString("detail", detail);
    }
    msg->post();
  }
}
void MediaCodec::onOutputFormatChanged() {
  if (mCallback != NULL) {
    sp<AMessage> msg = mCallback->dup();
    msg->setInt32("callbackID", CB_OUTPUT_FORMAT_CHANGED);
    msg->setMessage("format", mOutputFormat);
    msg->post();
  }
}
void MediaCodec::postActivityNotificationIfPossible() {
  if (mActivityNotify == NULL) {
    return;
  }
  bool isErrorOrOutputChanged =
      (mFlags & (kFlagStickyError | kFlagOutputBuffersChanged |
                 kFlagOutputFormatChanged));
  if (isErrorOrOutputChanged || !mAvailPortBuffers[kPortIndexInput].empty() ||
      !mAvailPortBuffers[kPortIndexOutput].empty()) {
    mActivityNotify->setInt32("input-buffers",
                              mAvailPortBuffers[kPortIndexInput].size());
    if (isErrorOrOutputChanged) {
      mActivityNotify->setInt32("output-buffers", INT32_MAX);
    } else {
      mActivityNotify->setInt32("output-buffers",
                                mAvailPortBuffers[kPortIndexOutput].size());
    }
    mActivityNotify->post();
    mActivityNotify.clear();
  }
}
status_t MediaCodec::setParameters(const sp<AMessage> &params) {
  sp<AMessage> msg = new AMessage(kWhatSetParameters, this);
  msg->setMessage("params", params);
  sp<AMessage> response;
  return PostAndAwaitResponse(msg, &response);
}
status_t MediaCodec::onSetParameters(const sp<AMessage> &params) {
  updateLowLatency(params);
  mCodec->signalSetParameters(params);
  return OK;
}
status_t MediaCodec::amendOutputFormatWithCodecSpecificData(
    const sp<MediaCodecBuffer> &buffer) {
  AString mime;
  CHECK(mOutputFormat->findString("mime", &mime));
  if (!strcasecmp(mime.c_str(), MEDIA_MIMETYPE_VIDEO_AVC)) {
    unsigned csdIndex = 0;
    const uint8_t *data = buffer->data();
    size_t size = buffer->size();
    const uint8_t *nalStart;
    size_t nalSize;
    while (getNextNALUnit(&data, &size, &nalStart, &nalSize, true) == OK) {
      sp<ABuffer> csd = new ABuffer(nalSize + 4);
      memcpy(csd->data(), "\x00\x00\x00\x01", 4);
      memcpy(csd->data() + 4, nalStart, nalSize);
      mOutputFormat->setBuffer(AStringPrintf("csd-%u", csdIndex).c_str(), csd);
      ++csdIndex;
    }
    if (csdIndex != 2) {
      return ERROR_MALFORMED;
    }
  } else {
    sp<ABuffer> csd = new ABuffer(buffer->size());
    memcpy(csd->data(), buffer->data(), buffer->size());
    csd->setRange(0, buffer->size());
    mOutputFormat->setBuffer("csd-0", csd);
  }
  return OK;
}
std::string MediaCodec::stateString(State state) {
  const char *rval = NULL;
  char rawbuffer[16];
  switch (state) {
    case UNINITIALIZED:
      rval = "UNINITIALIZED";
      break;
    case INITIALIZING:
      rval = "INITIALIZING";
      break;
    case INITIALIZED:
      rval = "INITIALIZED";
      break;
    case CONFIGURING:
      rval = "CONFIGURING";
      break;
    case CONFIGURED:
      rval = "CONFIGURED";
      break;
    case STARTING:
      rval = "STARTING";
      break;
    case STARTED:
      rval = "STARTED";
      break;
    case FLUSHING:
      rval = "FLUSHING";
      break;
    case FLUSHED:
      rval = "FLUSHED";
      break;
    case STOPPING:
      rval = "STOPPING";
      break;
    case RELEASING:
      rval = "RELEASING";
      break;
    default:
      snprintf(rawbuffer, sizeof(rawbuffer), "%d", state);
      rval = rawbuffer;
      break;
  }
  return rval;
}
using Status = ::ndk::ScopedAStatus;
using aidl::android::media::BnResourceManagerClient;
using aidl::android::media::IResourceManagerClient;
using aidl::android::media::IResourceManagerService;
}
