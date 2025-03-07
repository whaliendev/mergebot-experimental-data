       
#include <android/gui/DropInputMode.h>
#include <android/gui/ISurfaceComposerClient.h>
#include <gui/BufferQueue.h>
#include <gui/ISurfaceComposerClient.h>
#include <gui/LayerState.h>
#include <gui/WindowInfo.h>
#include <layerproto/LayerProtoHeader.h>
#include <math/vec4.h>
#include <renderengine/Mesh.h>
#include <renderengine/Texture.h>
#include <sys/types.h>
#include <ui/BlurRegion.h>
#include <ui/FloatRect.h>
#include <ui/FrameStats.h>
#include <ui/GraphicBuffer.h>
#include <ui/PixelFormat.h>
#include <ui/Region.h>
#include <ui/StretchEffect.h>
#include <ui/Transform.h>
#include <utils/RefBase.h>
#include <utils/Timers.h>
#include <compositionengine/LayerFE.h>
#include <compositionengine/LayerFECompositionState.h>
#include <scheduler/Fps.h>
#include <scheduler/Seamlessness.h>
#include <chrono>
#include <cstdint>
#include <list>
#include <optional>
#include "ClientCache.h"
#include "DisplayHardware/ComposerHal.h"
#include <vector>
#include "MonitoredProducer.h"
#include "RenderArea.h"
#include "Client.h"
#include "DisplayHardware/HWComposer.h"
#include "FrameTracker.h"
#include "HwcSlotGenerator.h"
#include "LayerFE.h"
#include "LayerVector.h"
#include "Scheduler/LayerInfo.h"
#include "SurfaceFlinger.h"
#include "Tracing/LayerTracing.h"
#include "TransactionCallbackInvoker.h"
using namespace android::surfaceflinger;
namespace android {
class Client;
class Colorizer;
class DisplayDevice;
class GraphicBuffer;
class SurfaceFlinger;
class LayerDebugInfo;
namespace compositionengine {
class OutputLayer;
struct LayerFECompositionState;
}
namespace gui {}
namespace frametimeline {
class SurfaceFrame;
}
class Layer : public virtual RefBase {
    static constexpr int32_t PRIORITY_UNSET = -1;
    static constexpr int32_t PRIORITY_FOCUSED_WITH_MODE = 0;
    static constexpr int32_t PRIORITY_FOCUSED_WITHOUT_MODE = 1;
    static constexpr int32_t PRIORITY_NOT_FOCUSED_WITH_MODE = 2;
private:
    friend class SlotGenerationTest;
public:
    enum {
        eDontUpdateGeometryState = 0x00000001,
        eVisibleRegion = 0x00000002,
        eInputInfoChanged = 0x00000004
    };
    struct Geometry {
        uint32_t w;
        uint32_t h;
        ui::Transform transform;
        inline bool operator==(const Geometry& rhs) const {
            return (w == rhs.w && h == rhs.h) && (transform.tx() == rhs.transform.tx()) &&
                    (transform.ty() == rhs.transform.ty());
        }
        inline bool operator!=(const Geometry& rhs) const { return !operator==(rhs); }
    };
    using FrameRate = scheduler::LayerInfo::FrameRate;
    using FrameRateCompatibility = scheduler::LayerInfo::FrameRateCompatibility;
    struct State {
        int32_t z;
        ui::LayerStack layerStack;
        uint32_t flags;
        int32_t sequence;
        bool modified;
        Rect crop;
        LayerMetadata metadata;
        wp<Layer> zOrderRelativeOf;
        bool isRelativeOf{false};
        SortedVector<wp<Layer>> zOrderRelatives;
        half4 color;
        float cornerRadius;
        int backgroundBlurRadius;
        gui::WindowInfo inputInfo;
        wp<Layer> touchableRegionCrop;
        ui::Dataspace dataspace;
        bool dataspaceRequested;
        uint64_t frameNumber;
        ui::Transform transform;
        uint32_t bufferTransform;
        bool transformToDisplayInverse;
        Region transparentRegionHint;
        std::shared_ptr<renderengine::ExternalTexture> buffer;
        client_cache_t clientCacheId;
        sp<Fence> acquireFence;
        std::shared_ptr<FenceTime> acquireFenceTime;
        HdrMetadata hdrMetadata;
        Region surfaceDamageRegion;
        int32_t api;
        sp<NativeHandle> sidebandStream;
        mat4 colorTransform;
        bool hasColorTransform;
        sp<Layer> bgColorLayer;
        std::deque<sp<CallbackHandle>> callbackHandles;
        bool colorSpaceAgnostic;
        nsecs_t desiredPresentTime = 0;
        bool isAutoTimestamp = true;
        float shadowRadius;
        std::vector<BlurRegion> blurRegions;
        int32_t frameRateSelectionPriority;
        FrameRateCompatibility defaultFrameRateCompatibility;
        FrameRate frameRate;
        FrameRate frameRateForLayerTree;
        ui::Transform::RotationFlags fixedTransformHint;
        FrameTimelineInfo frameTimelineInfo;
        nsecs_t postTime;
        sp<ITransactionCompletedListener> releaseBufferListener;
        std::shared_ptr<frametimeline::SurfaceFrame> bufferSurfaceFrameTX;
        std::unordered_map<int64_t, std::shared_ptr<frametimeline::SurfaceFrame>>
                bufferlessSurfaceFramesTX;
        static constexpr uint32_t kStateSurfaceFramesThreshold = 25;
        StretchEffect stretchEffect;
        bool isTrustedOverlay;
        Rect bufferCrop;
        Rect destinationFrame;
        sp<IBinder> releaseBufferEndpoint;
        gui::DropInputMode dropInputMode;
        bool autoRefresh = false;
        bool dimmingEnabled = true;
    };
    explicit Layer(const LayerCreationArgs& args);
    virtual ~Layer();
    static bool isLayerFocusedBasedOnPriority(int32_t priority);
    static void miniDumpHeader(std::string& result);
    virtual const char* getType() const { return "Layer"; }
    virtual bool isVisible() const;
    virtual sp<Layer> createClone();
    bool setMatrix(const layer_state_t::matrix22_t& matrix);
    bool setPosition(float x, float y);
    bool setCrop(const Rect& crop);
    virtual bool setLayer(int32_t z);
    virtual bool setRelativeLayer(const sp<IBinder>& relativeToHandle, int32_t relativeZ);
    virtual bool setAlpha(float alpha);
    bool setColor(const half3& );
    ;
    virtual bool setCornerRadius(float cornerRadius);
    virtual bool setBackgroundBlurRadius(int backgroundBlurRadius);
    virtual bool setBlurRegions(const std::vector<BlurRegion>& effectRegions);
    bool setTransparentRegionHint(const Region& transparent);
    virtual bool setTrustedOverlay(bool);
    virtual bool setFlags(uint32_t flags, uint32_t mask);
    virtual bool setLayerStack(ui::LayerStack);
    virtual ui::LayerStack getLayerStack(
            LayerVector::StateSet state = LayerVector::StateSet::Drawing) const;
    virtual bool setMetadata(const LayerMetadata& data);
    virtual void setChildrenDrawingParent(const sp<Layer>&);
    virtual bool reparent(const sp<IBinder>& newParentHandle);
    virtual bool setColorTransform(const mat4& matrix);
    virtual mat4 getColorTransform() const;
    virtual bool hasColorTransform() const;
    virtual bool isColorSpaceAgnostic() const { return mDrawingState.colorSpaceAgnostic; }
    virtual bool isDimmingEnabled() const { return getDrawingState().dimmingEnabled; }
    bool setTransform(uint32_t );
    bool setTransformToDisplayInverse(bool );
    bool setBuffer(std::shared_ptr<renderengine::ExternalTexture>& ,
                   const BufferData& , nsecs_t ,
                   nsecs_t , bool ,
                   std::optional<nsecs_t> , const FrameTimelineInfo& );
    bool setDataspace(ui::Dataspace );
    bool setHdrMetadata(const HdrMetadata& );
    bool setSurfaceDamageRegion(const Region& );
    bool setApi(int32_t );
    bool setSidebandStream(const sp<NativeHandle>& );
    ;
    bool setTransactionCompletedListeners(const std::vector<sp<CallbackHandle>>& );
    virtual bool setBackgroundColor(const half3& color, float alpha, ui::Dataspace dataspace);
    virtual bool setColorSpaceAgnostic(const bool agnostic);
    virtual bool setDimmingEnabled(const bool dimmingEnabled);
    virtual bool setDefaultFrameRateCompatibility(FrameRateCompatibility compatibility);
    virtual bool setFrameRateSelectionPriority(int32_t priority);
    virtual bool setFixedTransformHint(ui::Transform::RotationFlags fixedTransformHint);
    void setAutoRefresh(bool );
    bool setDropInputMode(gui::DropInputMode);
    virtual int32_t getFrameRateSelectionPriority() const;
    virtual FrameRateCompatibility getDefaultFrameRateCompatibility() const;
    ui::Dataspace getDataSpace() const;
    ui::Dataspace getRequestedDataSpace() const;
    virtual sp<LayerFE> getCompositionEngineLayerFE() const;
    const LayerSnapshot* getLayerSnapshot() const;
    LayerSnapshot* editLayerSnapshot();
    void useSurfaceDamage();
    void useEmptyDamage();
    Region getVisibleRegion(const DisplayDevice*) const;
    bool isOpaque(const Layer::State&) const;
    bool canReceiveInput() const;
    bool isProtected() const;
    virtual bool isFixedSize() const { return true; }
    bool usesSourceCrop() const { return hasBufferOrSidebandStream(); }
    virtual bool isCreatedFromMainThread() const { return false; }
    ui::Transform getActiveTransform(const Layer::State& s) const { return s.transform; }
    Region getActiveTransparentRegion(const Layer::State& s) const {
        return s.transparentRegionHint;
    }
    Rect getCrop(const Layer::State& s) const { return s.crop; }
    bool needsFiltering(const DisplayDevice*) const;
    bool needsFilteringForScreenshots(const DisplayDevice*, const ui::Transform&) const;
    ui::Dataspace translateDataspace(ui::Dataspace dataspace);
    void updateCloneBufferInfo();
    uint64_t mPreviousFrameNumber = 0;
    bool isHdrY410() const;
    void onPostComposition(const DisplayDevice*, const std::shared_ptr<FenceTime>& ,
                           const std::shared_ptr<FenceTime>& ,
                           const CompositorTiming&);
    void releasePendingBuffer(nsecs_t );
    bool latchBuffer(bool& , nsecs_t );
    void latchAndReleaseBuffer();
    Rect getBufferCrop() const;
    uint32_t getBufferTransform() const;
    sp<GraphicBuffer> getBuffer() const;
    const std::shared_ptr<renderengine::ExternalTexture>& getExternalTexture() const;
    ui::Transform::RotationFlags getTransformHint() const { return mTransformHint; }
    bool hasReadyFrame() const;
    virtual int32_t getQueuedFrameCount() const { return 0; }
    Rect getBufferSize(const Layer::State&) const;
    FloatRect computeSourceBounds(const FloatRect& parentBounds) const;
    virtual FrameRate getFrameRateForLayerTree() const;
    bool getTransformToDisplayInverse() const;
    virtual RoundedCornerState getRoundedCornerState() const;
    bool hasRoundedCorners() const {
        return getRoundedCornerState().hasRoundedCorners() {
            return getRoundedCornerState().hasRoundedCorners();
        }
        PixelFormat getPixelFormat() const;
        bool hasRoundedCorners() const override {
            return getRoundedCornerState().hasRoundedCorners() {
                return getRoundedCornerState().hasRoundedCorners();
            }
            bool needsInputInfo() const {
                return (hasInputInfo() || hasBufferOrSidebandStream()) && !mPotentialCursor;
            }
            void onFirstRef() override;
            struct BufferInfo {
                nsecs_t mDesiredPresentTime;
                std::shared_ptr<FenceTime> mFenceTime;
                sp<Fence> mFence;
                uint32_t mTransform{0};
                ui::Dataspace mDataspace{ui::Dataspace::UNKNOWN};
                Rect mCrop;
                uint32_t mScaleMode{NATIVE_WINDOW_SCALING_MODE_FREEZE};
                Region mSurfaceDamage;
                HdrMetadata mHdrMetadata;
                int mApi;
                PixelFormat mPixelFormat{PIXEL_FORMAT_NONE};
                bool mTransformToDisplayInverse{false};
                std::shared_ptr<renderengine::ExternalTexture> mBuffer;
                uint64_t mFrameNumber;
                int mBufferSlot{BufferQueue::INVALID_BUFFER_SLOT};
                bool mFrameLatencyNeeded{false};
            };
            BufferInfo mBufferInfo;
            const compositionengine::LayerFECompositionState* getCompositionState() const;
            bool fenceHasSignaled() const;
            bool onPreComposition(nsecs_t refreshStartTime);
            void onLayerDisplayed(ftl::SharedFuture<FenceResult>);
            void setWasClientComposed(const sp<Fence>& fence) {
                mLastClientCompositionFence = fence;
                mClearClientCompositionFenceOnLayerDisplayed = false;
            }
            const char* getDebugName() const;
            bool setShadowRadius(float shadowRadius);
            bool isLegacyDataSpace() const;
            uint32_t getTransactionFlags() const {
                return mTransactionFlags;
            }
            void setTransactionFlags(uint32_t mask);
            uint32_t clearTransactionFlags(uint32_t mask);
            FloatRect getBounds(const Region& activeTransparentRegion) const;
            FloatRect getBounds() const;
            void computeBounds(FloatRect parentBounds, ui::Transform parentTransform,
                               float shadowRadius);
            int32_t getSequence() const {
                return sequence;
            }
            uint64_t getCurrentBufferId() const {
                return getBuffer() ? getBuffer()->getId() : 0;
            }
            bool isSecure() const;
            bool isHiddenByPolicy() const;
            bool isInternalDisplayOverlay() const;
            ui::LayerFilter getOutputFilter() const {
                return {getLayerStack(), isInternalDisplayOverlay()};
            }
            bool isRemovedFromCurrentState() const;
            LayerProto* writeToProto(LayersProto & layersProto, uint32_t traceFlags);
            void writeToProtoDrawingState(LayerProto * layerInfo);
            void writeToProtoCommonState(LayerProto * layerInfo, LayerVector::StateSet,
                                         uint32_t traceFlags = LayerTracing::TRACE_ALL);
            gui::WindowInfo::Type getWindowType() const {
                return mWindowType;
            }
            void updateMirrorInfo();
            virtual uint32_t doTransaction(uint32_t transactionFlags);
            void removeRelativeZ(const std::vector<Layer*>& layersInTree);
            void removeFromCurrentState();
            void onRemovedFromCurrentState();
            void addToCurrentState();
            void updateTransformHint(ui::Transform::RotationFlags);
            void skipReportingTransformHint();
            inline const State& getDrawingState() const {
                return mDrawingState;
            }
            inline State& getDrawingState() {
                return mDrawingState;
            }
            gui::LayerDebugInfo getLayerDebugInfo(const DisplayDevice*) const;
            void miniDump(std::string & result, const DisplayDevice&) const;
            void dumpFrameStats(std::string & result) const;
            void dumpCallingUidPid(std::string & result) const;
            void clearFrameStats();
            void logFrameStats();
            void getFrameStats(FrameStats * outStats) const;
            void onDisconnect();
            ui::Transform getTransform() const;
            bool isTransformValid() const;
            half getAlpha() const;
            half4 getColor() const;
            int32_t getBackgroundBlurRadius() const;
            bool drawShadows() const {
                return mEffectiveShadowRadius > 0.f;
            };
            ui::Transform::RotationFlags getFixedTransformHint() const;
            void traverse(LayerVector::StateSet, const LayerVector::Visitor&);
            void traverseInReverseZOrder(LayerVector::StateSet, const LayerVector::Visitor&);
            void traverseInZOrder(LayerVector::StateSet, const LayerVector::Visitor&);
            void traverseChildrenInZOrder(LayerVector::StateSet, const LayerVector::Visitor&);
            size_t getChildrenCount() const;
            LayerVector& getCurrentChildren() {
                return mCurrentChildren;
            }
            void addChild(const sp<Layer>&);
            ssize_t removeChild(const sp<Layer>& layer);
            sp<Layer> getParent() const {
                return mCurrentParent.promote();
            }
            bool isAtRoot() const {
                return mIsAtRoot;
            }
            void setIsAtRoot(bool isAtRoot) {
                mIsAtRoot = isAtRoot;
            }
            bool hasParent() const {
                return getParent() != nullptr;
            }
            Rect getScreenBounds(bool reduceTransparentRegion = true) const;
            bool setChildLayer(const sp<Layer>& childLayer, int32_t z);
            bool setChildRelativeLayer(const sp<Layer>& childLayer,
                                       const sp<IBinder>& relativeToHandle, int32_t relativeZ);
            void commitChildList();
            int32_t getZ(LayerVector::StateSet) const;
            Rect getCroppedBufferSize(const Layer::State& s) const;
            bool setFrameRate(FrameRate);
            virtual void setFrameTimelineInfoForBuffer(const FrameTimelineInfo& ) {}
            void setFrameTimelineVsyncForBufferTransaction(const FrameTimelineInfo& info,
                                                           nsecs_t postTime);
            void setFrameTimelineVsyncForBufferlessTransaction(const FrameTimelineInfo& info,
                                                               nsecs_t postTime);
            void addSurfaceFrameDroppedForBuffer(std::shared_ptr<frametimeline::SurfaceFrame> &
                                                 surfaceFrame);
            void addSurfaceFramePresentedForBuffer(std::shared_ptr<frametimeline::SurfaceFrame> &
                                                           surfaceFrame,
                                                   nsecs_t acquireFenceTime,
                                                   nsecs_t currentLatchTime);
            std::shared_ptr<frametimeline::SurfaceFrame>
            createSurfaceFrameForTransaction(const FrameTimelineInfo& info, nsecs_t postTime);
            std::shared_ptr<frametimeline::SurfaceFrame>
            createSurfaceFrameForBuffer(const FrameTimelineInfo& info, nsecs_t queueTime,
                                        std::string debugName);
            sp<IBinder> getHandle();
            const std::string& getName() const {
                return mName;
            }
            bool getPremultipledAlpha() const;
            void setInputInfo(const gui::WindowInfo& info);
            struct InputDisplayArgs {
                const ui::Transform* transform = nullptr;
                bool isSecure = false;
            };
            gui::WindowInfo fillInputInfo(const InputDisplayArgs& displayArgs);
            bool hasInputInfo() const;
            void setGameModeForTree(GameMode);
            void setGameMode(GameMode gameMode) {
                mGameMode = gameMode;
            }
            GameMode getGameMode() const {
                return mGameMode;
            }
            virtual uid_t getOwnerUid() const {
                return mOwnerUid;
            }
            pid_t getOwnerPid() {
                return mOwnerPid;
            }
            sp<Layer> mClonedChild;
            bool mHadClonedChild = false;
            void setClonedChild(const sp<Layer>& mClonedChild);
            mutable bool contentDirty{false};
            Region surfaceDamageRegion;
            const int32_t sequence;
            bool mPendingHWCDestroy{false};
            bool backpressureEnabled() const {
                return mDrawingState.flags & layer_state_t::eEnableBackpressure;
            }
            bool setStretchEffect(const StretchEffect& effect);
            StretchEffect getStretchEffect() const;
            bool enableBorder(bool shouldEnable, float width, const half4& color);
            bool isBorderEnabled();
            float getBorderWidth();
            const half4& getBorderColor();
            bool setBufferCrop(const Rect& );
            bool setDestinationFrame(const Rect& );
            void decrementPendingBufferCount();
            std::atomic<int32_t>* getPendingBufferCounter() {
                return &mPendingBufferTransactions;
            }
            std::string getPendingBufferCounterName() {
                return mBlastTransactionName;
            }
            bool updateGeometry();
            bool simpleBufferUpdate(const layer_state_t&) const;
            static bool isOpaqueFormat(PixelFormat format);
            void updateSnapshot(bool updateGeometry);
            void updateMetadataSnapshot(const LayerMetadata& parentMetadata);
            void updateRelativeMetadataSnapshot(const LayerMetadata& relativeLayerMetadata,
                                                std::unordered_set<Layer*>& visited);
        protected:
            friend class TestableSurfaceFlinger;
            friend class FpsReporterTest;
            friend class RefreshRateSelectionTest;
            friend class SetFrameRateTest;
            friend class TransactionFrameTracerTest;
            friend class TransactionSurfaceFrameTest;
        private:
            bool getAutoRefresh() const {
                return mDrawingState.autoRefresh;
            }
            bool getSidebandStreamChanged() const {
                return mSidebandStreamChanged;
            }
            std::atomic<bool> mSidebandStreamChanged{false};
        protected:
            virtual void setInitialValuesForClone(const sp<Layer>& clonedFrom);
            void preparePerFrameCompositionState();
            void preparePerFrameBufferCompositionState();
            void preparePerFrameEffectsCompositionState();
            virtual void commitTransaction(State & stateToCommit);
            void gatherBufferInfo();
            void onSurfaceFrameCreated(const std::shared_ptr<frametimeline::SurfaceFrame>&);
            sp<Layer> getClonedFrom() {
                return mClonedFrom != nullptr ? mClonedFrom.promote() : nullptr;
            }
            bool isClone() {
                return mClonedFrom != nullptr;
            }
            bool isClonedFromAlive() {
                return getClonedFrom() != nullptr;
            }
            void cloneDrawingState(const Layer* from);
            void updateClonedDrawingState(std::map<sp<Layer>, sp<Layer>> & clonedLayersMap);
            void updateClonedChildren(const sp<Layer>& mirrorRoot,
                                      std::map<sp<Layer>, sp<Layer>>& clonedLayersMap);
            void updateClonedRelatives(const std::map<sp<Layer>, sp<Layer>>& clonedLayersMap);
            void addChildToDrawing(const sp<Layer>&);
            void updateClonedInputInfo(const std::map<sp<Layer>, sp<Layer>>& clonedLayersMap);
            void prepareBasicGeometryCompositionState();
            void prepareGeometryCompositionState();
            void prepareCursorCompositionState();
            uint32_t getEffectiveUsage(uint32_t usage) const;
            void setupRoundedCornersCropCoordinates(Rect win, const FloatRect& roundedCornersCrop)
                    const;
            void setParent(const sp<Layer>&);
            LayerVector makeTraversalList(LayerVector::StateSet, bool* outSkipRelativeZUsers);
            void addZOrderRelative(const wp<Layer>& relative);
            void removeZOrderRelative(const wp<Layer>& relative);
            compositionengine::OutputLayer* findOutputLayerForDisplay(const DisplayDevice*) const;
            bool usingRelativeZ(LayerVector::StateSet) const;
            virtual ui::Transform getInputTransform() const;
            Rect getInputBounds() const;
            sp<SurfaceFlinger> mFlinger;
            bool mPremultipliedAlpha{true};
            const std::string mName;
            const std::string mTransactionName{"TX - " + mName};
            State mDrawingState;
            uint32_t mTransactionFlags{0};
            int32_t mLastCommittedTxSequence = -1;
            FrameTracker mFrameTracker;
            sp<NativeHandle> mSidebandStream;
            bool mIsActiveBufferUpdatedForGpu = true;
            std::atomic<uint64_t> mCurrentFrameNumber{0};
            bool mNeedsFiltering{false};
            std::atomic<bool> mRemovedFromDrawingState{false};
            bool mProtectedByApp{false};
            mutable Mutex mLock;
            const wp<Client> mClientRef;
            bool mPotentialCursor{false};
            LayerVector mCurrentChildren{LayerVector::StateSet::Current};
            LayerVector mDrawingChildren{LayerVector::StateSet::Drawing};
            wp<Layer> mCurrentParent;
            wp<Layer> mDrawingParent;
            const gui::WindowInfo::Type mWindowType;
            uid_t mOwnerUid;
            pid_t mOwnerPid;
            nsecs_t mLastLatchTime = 0;
            mutable bool mDrawingStateModified = false;
            sp<Fence> mLastClientCompositionFence;
            bool mClearClientCompositionFenceOnLayerDisplayed = false;
        private:
            virtual bool canDrawShadows() const {
                return true;
            }
            aidl::android::hardware::graphics::composer3::Composition getCompositionType(
                    const DisplayDevice&) const;
            std::vector<Layer*> getLayersInTree(LayerVector::StateSet);
            void traverseChildrenInZOrderInner(const std::vector<Layer*>& layersInTree,
                                               LayerVector::StateSet, const LayerVector::Visitor&);
            LayerVector makeChildrenTraversalList(LayerVector::StateSet,
                                                  const std::vector<Layer*>& layersInTree);
            void updateTreeHasFrameRateVote();
            bool propagateFrameRateForLayerTree(FrameRate parentFrameRate, bool* transactionNeeded);
            bool setFrameRateForLayerTree(FrameRate);
            void setZOrderRelativeOf(const wp<Layer>& relativeOf);
            bool isTrustedOverlay() const;
            gui::DropInputMode getDropInputMode() const;
            void handleDropInputMode(gui::WindowInfo & info) const;
            sp<Layer> getClonedRoot();
            sp<Layer> getRootLayer();
            void fillTouchOcclusionMode(gui::WindowInfo & info);
            void fillInputFrameInfo(gui::WindowInfo&, const ui::Transform& screenToDisplay);
            inline void tracePendingBufferCount(int32_t pendingBuffers);
            bool latchSidebandStream(bool& recomputeVisibleRegions);
            bool hasFrameUpdate() const;
            void updateTexImage(nsecs_t latchTime);
            Rect computeBufferCrop(const State& s);
            bool willPresentCurrentTransaction() const;
            bool bufferNeedsFiltering() const;
            void callReleaseBufferCallback(const sp<ITransactionCompletedListener>& listener,
                                           const sp<GraphicBuffer>& buffer, uint64_t framenumber,
                                           const sp<Fence>& releaseFence,
                                           uint32_t currentMaxAcquiredBufferCount);
            bool fillsColor() const;
            bool hasBlur() const;
            bool hasEffect() const {
                return fillsColor() || drawShadows() || hasBlur();
            }
            bool hasBufferOrSidebandStream() const {
                return ((mSidebandStream != nullptr) || (mBufferInfo.mBuffer != nullptr));
            }
            bool hasBufferOrSidebandStreamInDrawing() const {
                return ((mDrawingState.sidebandStream != nullptr) ||
                        (mDrawingState.buffer != nullptr));
            }
            bool hasSomethingToDraw() const {
                return hasEffect() || hasBufferOrSidebandStream();
            }
            void updateChildrenSnapshots(bool updateGeometry);
            ui::Transform mEffectiveTransform;
            FloatRect mSourceBounds;
            FloatRect mBounds;
            FloatRect mScreenBounds;
            bool mGetHandleCalled = false;
            wp<Layer> mClonedFrom;
            float mEffectiveShadowRadius = 0.f;
            GameMode mGameMode = GameMode::Unsupported;
            const std::vector<BlurRegion> getBlurRegions() const;
            bool mIsAtRoot = false;
            uint32_t mLayerCreationFlags;
            bool findInHierarchy(const sp<Layer>&);
            bool mBorderEnabled = false;
            float mBorderWidth;
            half4 mBorderColor;
            void setTransformHint(ui::Transform::RotationFlags);
            const uint32_t mTextureName;
            ui::Transform::RotationFlags mTransformHint = ui::Transform::ROT_0;
            bool mSkipReportingTransformHint = true;
            ReleaseCallbackId mPreviousReleaseCallbackId = ReleaseCallbackId::INVALID_ID;
            uint64_t mPreviousReleasedFrameNumber = 0;
            uint64_t mPreviousBarrierFrameNumber = 0;
            bool mReleasePreviousBuffer = false;
            std::variant<nsecs_t, sp<Fence>> mCallbackHandleAcquireTimeOrFence = -1;
            std::deque<std::shared_ptr<android::frametimeline::SurfaceFrame>>
                    mPendingJankClassifications;
            static constexpr int kPendingClassificationMaxSurfaceFrames = 50;
            const std::string mBlastTransactionName{"BufferTX - " + mName};
            std::atomic<int32_t> mPendingBufferTransactions{0};
            ui::Transform mRequestedTransform;
            sp<HwcSlotGenerator> mHwcSlotGenerator;
            sp<LayerFE> mLayerFE;
            std::unique_ptr<LayerSnapshot> mSnapshot = std::make_unique<LayerSnapshot>();
            friend class LayerSnapshotGuard;
        };
        class LayerSnapshotGuard {
        public:
            LayerSnapshotGuard(Layer* layer) REQUIRES(kMainThreadContext);
            ~LayerSnapshotGuard() REQUIRES(kMainThreadContext);
            LayerSnapshotGuard(const LayerSnapshotGuard&) = delete;
            LayerSnapshotGuard& operator=(const LayerSnapshotGuard&) = delete
                    LayerSnapshotGuard(LayerSnapshotGuard && other) REQUIRES(kMainThreadContext);
            LayerSnapshotGuard& operator=(LayerSnapshotGuard&& other) REQUIRES(kMainThreadContext);
        private:
            Layer* mLayer;
        };
        std::ostream& operator<<(std::ostream& stream, const Layer::FrameRate& rate);
    }
