#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextra"
#include <chrono>
#include <cmath>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <ftl/enum.h>
#include <utils/Trace.h>
#include "../SurfaceFlingerProperties.h"
#include "RefreshRateConfigs.h"
#undef LOG_TAG
#define LOG_TAG "RefreshRateConfigs"
namespace android::scheduler {
namespace {
std::string formatLayerInfo(const RefreshRateConfigs::LayerRequirement& layer, float weight) {
    return base::StringPrintf("%s (type=%s, weight=%.2f seamlessness=%s) %s", layer.name.c_str(),
                              RefreshRateConfigs::layerVoteTypeString(layer.vote).c_str(), weight,
                              toString(layer.seamlessness).c_str(),
                              to_string(layer.desiredRefreshRate).c_str());
}
std::vector<Fps> constructKnownFrameRates(const DisplayModes& modes) {
    std::vector<Fps> knownFrameRates = {Fps(24.0f), Fps(30.0f), Fps(45.0f), Fps(60.0f), Fps(72.0f)};
    knownFrameRates.reserve(knownFrameRates.size() + modes.size());
    for (const auto& mode : modes) {
        const auto refreshRate = Fps::fromPeriodNsecs(mode->getVsyncPeriod());
        knownFrameRates.emplace_back(refreshRate);
    }
    std::sort(knownFrameRates.begin(), knownFrameRates.end(), Fps::comparesLess);
    knownFrameRates.erase(std::unique(knownFrameRates.begin(), knownFrameRates.end(),
                                      Fps::EqualsWithMargin()),
                          knownFrameRates.end());
    return knownFrameRates;
}
}
using AllRefreshRatesMapType = RefreshRateConfigs::AllRefreshRatesMapType;
using RefreshRate = RefreshRateConfigs::RefreshRate;
bool RefreshRate::inPolicy(Fps minRefreshRate, Fps maxRefreshRate) const {
    using fps_approx_ops::operator<=;
    return minRefreshRate <= getFps() && getFps() <= maxRefreshRate;
}
std::string RefreshRate::toString() const {
    return base::StringPrintf("{id=%d, hwcId=%d, fps=%.2f, width=%d, height=%d group=%d}",
                              getModeId().value(), mode->getHwcId(), getFps().getValue(),
                              mode->getWidth(), mode->getHeight(), getModeGroup());
}
std::string RefreshRateConfigs::Policy::toString() const {
    return base::StringPrintf("default mode ID: %d, allowGroupSwitching = %d"
                              ", primary range: %s, app request range: %s",
                              defaultMode.value(), allowGroupSwitching,
                              primaryRange.toString().c_str(), appRequestRange.toString().c_str());
}
std::pair<nsecs_t, nsecs_t> RefreshRateConfigs::getDisplayFrames(nsecs_t layerPeriod,
                                                                 nsecs_t displayPeriod) const {
    auto [quotient, remainder] = std::div(layerPeriod, displayPeriod);
    if (remainder <= MARGIN_FOR_PERIOD_CALCULATION ||
        std::abs(remainder - displayPeriod) <= MARGIN_FOR_PERIOD_CALCULATION) {
        quotient++;
        remainder = 0;
    }
    return {quotient, remainder};
}
bool RefreshRateConfigs::isVoteAllowed(const LayerRequirement& layer,
                                       const RefreshRate& refreshRate) const {
    using namespace fps_approx_ops;
    switch (layer.vote) {
        case LayerVoteType::ExplicitExactOrMultiple:
        case LayerVoteType::Heuristic:
            if (mConfig.frameRateMultipleThreshold != 0 &&
                refreshRate.getFps() >= Fps::fromValue(mConfig.frameRateMultipleThreshold) &&
                layer.desiredRefreshRate < Fps::fromValue(mConfig.frameRateMultipleThreshold / 2)) {
                return false;
            }
            break;
        case LayerVoteType::ExplicitDefault:
        case LayerVoteType::ExplicitExact:
        case LayerVoteType::Max:
        case LayerVoteType::Min:
        case LayerVoteType::NoVote:
            break;
    }
    return true;
}
float RefreshRateConfigs::calculateNonExactMatchingLayerScoreLocked(const LayerRequirement& layer, const RefreshRate& refreshRate) const {
    constexpr float kScoreForFractionalPairs = .8f;
    const auto displayPeriod = refreshRate.getVsyncPeriod();
    const auto layerPeriod = layer.desiredRefreshRate.getPeriodNsecs();
    if (layer.vote == LayerVoteType::ExplicitDefault) {
        auto actualLayerPeriod = displayPeriod;
        int multiplier = 1;
        while (layerPeriod > actualLayerPeriod + MARGIN_FOR_PERIOD_CALCULATION) {
            multiplier++;
            actualLayerPeriod = displayPeriod * multiplier;
        }
        return std::min(1.0f,
                        static_cast<float>(layerPeriod) / static_cast<float>(actualLayerPeriod));
    }
    if (layer.vote == LayerVoteType::ExplicitExactOrMultiple ||
        layer.vote == LayerVoteType::Heuristic) {
        if (isFractionalPairOrMultiple(refreshRate.getFps(), layer.desiredRefreshRate)) {
            return kScoreForFractionalPairs;
        }
        const auto [displayFramesQuotient, displayFramesRemainder] =
                getDisplayFrames(layerPeriod, displayPeriod);
        static constexpr size_t MAX_FRAMES_TO_FIT = 10;
        if (displayFramesRemainder == 0) {
            return 1.0f;
        }
        if (displayFramesQuotient == 0) {
            return (static_cast<float>(layerPeriod) / static_cast<float>(displayPeriod)) *
                    (1.0f / (MAX_FRAMES_TO_FIT + 1));
        }
        auto diff = std::abs(displayFramesRemainder - (displayPeriod - displayFramesRemainder));
        int iter = 2;
        while (diff > MARGIN_FOR_PERIOD_CALCULATION && iter < MAX_FRAMES_TO_FIT) {
            diff = diff - (displayPeriod - diff);
            iter++;
        }
        return (1.0f / iter);
    }
    return 0;
}
float RefreshRateConfigs::calculateLayerScoreLocked(const LayerRequirement& layer,
                                                    const RefreshRate& refreshRate,
                                                    bool isSeamlessSwitch) const {
    if (!isVoteAllowed(layer, refreshRate)) {
        return 0;
    }
    constexpr float kScoreForFractionalPairs = .8f;
    constexpr float kSeamedSwitchPenalty = 0.95f;
    const float seamlessness = isSeamlessSwitch ? 1.0f : kSeamedSwitchPenalty;
    if (layer.vote == LayerVoteType::Max) {
        const auto ratio = refreshRate.getFps().getValue() /
                mAppRequestRefreshRates.back()->getFps().getValue();
        return ratio * ratio;
    }
<<<<<<< HEAD
||||||| 5502d9f7a2
    const auto displayPeriod = refreshRate.getVsyncPeriod();
    const auto layerPeriod = layer.desiredRefreshRate.getPeriodNsecs();
    if (layer.vote == LayerVoteType::ExplicitDefault) {
        auto actualLayerPeriod = displayPeriod;
        int multiplier = 1;
        while (layerPeriod > actualLayerPeriod + MARGIN_FOR_PERIOD_CALCULATION) {
            multiplier++;
            actualLayerPeriod = displayPeriod * multiplier;
        }
        return std::min(1.0f,
                        static_cast<float>(layerPeriod) / static_cast<float>(actualLayerPeriod));
    }
    if (layer.vote == LayerVoteType::ExplicitExactOrMultiple ||
        layer.vote == LayerVoteType::Heuristic) {
        const auto [displayFramesQuotient, displayFramesRemainder] =
                getDisplayFrames(layerPeriod, displayPeriod);
        static constexpr size_t MAX_FRAMES_TO_FIT = 10;
        if (displayFramesRemainder == 0) {
            return 1.0f * seamlessness;
        }
        if (displayFramesQuotient == 0) {
            return (static_cast<float>(layerPeriod) / static_cast<float>(displayPeriod)) *
                    (1.0f / (MAX_FRAMES_TO_FIT + 1));
        }
        auto diff = std::abs(displayFramesRemainder - (displayPeriod - displayFramesRemainder));
        int iter = 2;
        while (diff > MARGIN_FOR_PERIOD_CALCULATION && iter < MAX_FRAMES_TO_FIT) {
            diff = diff - (displayPeriod - diff);
            iter++;
        }
        return (1.0f / iter) * seamlessness;
    }
=======
    const auto displayPeriod = refreshRate.getVsyncPeriod();
    const auto layerPeriod = layer.desiredRefreshRate.getPeriodNsecs();
    if (layer.vote == LayerVoteType::ExplicitDefault) {
        auto actualLayerPeriod = displayPeriod;
        int multiplier = 1;
        while (layerPeriod > actualLayerPeriod + MARGIN_FOR_PERIOD_CALCULATION) {
            multiplier++;
            actualLayerPeriod = displayPeriod * multiplier;
        }
        return std::min(1.0f,
                        static_cast<float>(layerPeriod) / static_cast<float>(actualLayerPeriod));
    }
    if (layer.vote == LayerVoteType::ExplicitExactOrMultiple ||
        layer.vote == LayerVoteType::Heuristic) {
        if (isFractionalPairOrMultiple(refreshRate.getFps(), layer.desiredRefreshRate)) {
            return kScoreForFractionalPairs * seamlessness;
        }
        const auto [displayFramesQuotient, displayFramesRemainder] =
                getDisplayFrames(layerPeriod, displayPeriod);
        static constexpr size_t MAX_FRAMES_TO_FIT = 10;
        if (displayFramesRemainder == 0) {
            return 1.0f * seamlessness;
        }
        if (displayFramesQuotient == 0) {
            return (static_cast<float>(layerPeriod) / static_cast<float>(displayPeriod)) *
                    (1.0f / (MAX_FRAMES_TO_FIT + 1));
        }
        auto diff = std::abs(displayFramesRemainder - (displayPeriod - displayFramesRemainder));
        int iter = 2;
        while (diff > MARGIN_FOR_PERIOD_CALCULATION && iter < MAX_FRAMES_TO_FIT) {
            diff = diff - (displayPeriod - diff);
            iter++;
        }
        return (1.0f / iter) * seamlessness;
    }
>>>>>>> 9c647ace
    if (layer.vote == LayerVoteType::ExplicitExact) {
        const int divider = getFrameRateDivider(refreshRate.getFps(), layer.desiredRefreshRate);
        if (mSupportsFrameRateOverrideByContent) {
            return divider > 0;
        }
        return divider == 1;
    }
    if (getFrameRateDivider(refreshRate.getFps(), layer.desiredRefreshRate) > 0) {
        return 1.0f * seamlessness;
    }
    constexpr float kNonExactMatchingPenalty = 0.95f;
    return calculateNonExactMatchingLayerScoreLocked(layer, refreshRate) * seamlessness *
            kNonExactMatchingPenalty;
}
struct RefreshRateScore {
    const RefreshRate* refreshRate;
    float score;
};
RefreshRate RefreshRateConfigs::getBestRefreshRate(const std::vector<LayerRequirement>& layers,
                                                   GlobalSignals globalSignals,
                                                   GlobalSignals* outSignalsConsidered) const {
    std::lock_guard lock(mLock);
    if (auto cached = getCachedBestRefreshRate(layers, globalSignals, outSignalsConsidered)) {
        return *cached;
    }
    GlobalSignals signalsConsidered;
    RefreshRate result = getBestRefreshRateLocked(layers, globalSignals, &signalsConsidered);
    lastBestRefreshRateInvocation.emplace(
            GetBestRefreshRateInvocation{.layerRequirements = layers,
                                         .globalSignals = globalSignals,
                                         .outSignalsConsidered = signalsConsidered,
                                         .resultingBestRefreshRate = result});
    if (outSignalsConsidered) {
        *outSignalsConsidered = signalsConsidered;
    }
    return result;
}
std::optional<RefreshRate> RefreshRateConfigs::getCachedBestRefreshRate(
        const std::vector<LayerRequirement>& layers, GlobalSignals globalSignals,
        GlobalSignals* outSignalsConsidered) const {
    const bool sameAsLastCall = lastBestRefreshRateInvocation &&
            lastBestRefreshRateInvocation->layerRequirements == layers &&
            lastBestRefreshRateInvocation->globalSignals == globalSignals;
    if (sameAsLastCall) {
        if (outSignalsConsidered) {
            *outSignalsConsidered = lastBestRefreshRateInvocation->outSignalsConsidered;
        }
        return lastBestRefreshRateInvocation->resultingBestRefreshRate;
    }
    return {};
}
RefreshRate RefreshRateConfigs::getBestRefreshRateLocked(
        const std::vector<LayerRequirement>& layers, GlobalSignals globalSignals,
        GlobalSignals* outSignalsConsidered) const {
    ATRACE_CALL();
    ALOGV("getBestRefreshRate %zu layers", layers.size());
    if (outSignalsConsidered) *outSignalsConsidered = {};
    const auto setTouchConsidered = [&] {
        if (outSignalsConsidered) {
            outSignalsConsidered->touch = true;
        }
    };
    const auto setIdleConsidered = [&] {
        if (outSignalsConsidered) {
            outSignalsConsidered->idle = true;
        }
    };
    int noVoteLayers = 0;
    int minVoteLayers = 0;
    int maxVoteLayers = 0;
    int explicitDefaultVoteLayers = 0;
    int explicitExactOrMultipleVoteLayers = 0;
    int explicitExact = 0;
    float maxExplicitWeight = 0;
    int seamedFocusedLayers = 0;
    for (const auto& layer : layers) {
        switch (layer.vote) {
            case LayerVoteType::NoVote:
                noVoteLayers++;
                break;
            case LayerVoteType::Min:
                minVoteLayers++;
                break;
            case LayerVoteType::Max:
                maxVoteLayers++;
                break;
            case LayerVoteType::ExplicitDefault:
                explicitDefaultVoteLayers++;
                maxExplicitWeight = std::max(maxExplicitWeight, layer.weight);
                break;
            case LayerVoteType::ExplicitExactOrMultiple:
                explicitExactOrMultipleVoteLayers++;
                maxExplicitWeight = std::max(maxExplicitWeight, layer.weight);
                break;
            case LayerVoteType::ExplicitExact:
                explicitExact++;
                maxExplicitWeight = std::max(maxExplicitWeight, layer.weight);
                break;
            case LayerVoteType::Heuristic:
                break;
        }
        if (layer.seamlessness == Seamlessness::SeamedAndSeamless && layer.focused) {
            seamedFocusedLayers++;
        }
    }
    const bool hasExplicitVoteLayers = explicitDefaultVoteLayers > 0 ||
            explicitExactOrMultipleVoteLayers > 0 || explicitExact > 0;
    const Policy* policy = getCurrentPolicyLocked();
    const auto& defaultMode = mRefreshRates.at(policy->defaultMode);
    const auto anchorGroup = seamedFocusedLayers > 0 ? mCurrentRefreshRate->getModeGroup()
                                                     : defaultMode->getModeGroup();
    if (globalSignals.touch && !hasExplicitVoteLayers) {
        ALOGV("TouchBoost - choose %s", getMaxRefreshRateByPolicyLocked().getName().c_str());
        setTouchConsidered();
        return getMaxRefreshRateByPolicyLocked(anchorGroup);
    }
    const bool primaryRangeIsSingleRate =
            isApproxEqual(policy->primaryRange.min, policy->primaryRange.max);
    if (!globalSignals.touch && globalSignals.idle &&
        !(primaryRangeIsSingleRate && hasExplicitVoteLayers)) {
        ALOGV("Idle - choose %s", getMinRefreshRateByPolicyLocked().getName().c_str());
        setIdleConsidered();
        return getMinRefreshRateByPolicyLocked();
    }
    if (layers.empty() || noVoteLayers == layers.size()) {
        const auto& refreshRate = getMaxRefreshRateByPolicyLocked(anchorGroup);
        ALOGV("no layers with votes - choose %s", refreshRate.getName().c_str());
        return refreshRate;
    }
    if (noVoteLayers + minVoteLayers == layers.size()) {
        ALOGV("all layers Min - choose %s", getMinRefreshRateByPolicyLocked().getName().c_str());
        return getMinRefreshRateByPolicyLocked();
    }
    std::vector<RefreshRateScore> scores;
    scores.reserve(mAppRequestRefreshRates.size());
    for (const auto refreshRate : mAppRequestRefreshRates) {
        scores.emplace_back(RefreshRateScore{refreshRate, 0.0f});
    }
    for (const auto& layer : layers) {
        ALOGV("Calculating score for %s (%s, weight %.2f, desired %.2f) ", layer.name.c_str(),
              ftl::enum_string(layer.vote).c_str(), layer.weight,
              layer.desiredRefreshRate.getValue());
        if (layer.vote == LayerVoteType::NoVote || layer.vote == LayerVoteType::Min) {
            continue;
        }
        auto weight = layer.weight;
        for (auto i = 0u; i < scores.size(); i++) {
            const bool isSeamlessSwitch =
                    scores[i].refreshRate->getModeGroup() == mCurrentRefreshRate->getModeGroup();
            if (layer.seamlessness == Seamlessness::OnlySeamless && !isSeamlessSwitch) {
                ALOGV("%s ignores %s to avoid non-seamless switch. Current mode = %s",
                      formatLayerInfo(layer, weight).c_str(),
                      scores[i].refreshRate->toString().c_str(),
                      mCurrentRefreshRate->toString().c_str());
                continue;
            }
            if (layer.seamlessness == Seamlessness::SeamedAndSeamless && !isSeamlessSwitch &&
                !layer.focused) {
                ALOGV("%s ignores %s because it's not focused and the switch is going to be seamed."
                      " Current mode = %s",
                      formatLayerInfo(layer, weight).c_str(),
                      scores[i].refreshRate->toString().c_str(),
                      mCurrentRefreshRate->toString().c_str());
                continue;
            }
            const bool isInPolicyForDefault = scores[i].refreshRate->getModeGroup() == anchorGroup;
            if (layer.seamlessness == Seamlessness::Default && !isInPolicyForDefault) {
                ALOGV("%s ignores %s. Current mode = %s", formatLayerInfo(layer, weight).c_str(),
                      scores[i].refreshRate->toString().c_str(),
                      mCurrentRefreshRate->toString().c_str());
                continue;
            }
            bool inPrimaryRange = scores[i].refreshRate->inPolicy(policy->primaryRange.min,
                                                                  policy->primaryRange.max);
            if ((primaryRangeIsSingleRate || !inPrimaryRange) &&
                !(layer.focused &&
                  (layer.vote == LayerVoteType::ExplicitDefault ||
                   layer.vote == LayerVoteType::ExplicitExact))) {
                continue;
            }
            const auto layerScore =
                    calculateLayerScoreLocked(layer, *scores[i].refreshRate, isSeamlessSwitch);
            ALOGV("%s gives %s score of %.4f", formatLayerInfo(layer, weight).c_str(),
                  scores[i].refreshRate->getName().c_str(), layerScore);
            scores[i].score += weight * layerScore;
        }
    }
    const RefreshRate* bestRefreshRate = maxVoteLayers > 0
            ? getBestRefreshRate(scores.rbegin(), scores.rend())
            : getBestRefreshRate(scores.begin(), scores.end());
    if (primaryRangeIsSingleRate) {
        if (std::all_of(scores.begin(), scores.end(),
                        [](RefreshRateScore score) { return score.score == 0; })) {
            const auto& refreshRate = getMaxRefreshRateByPolicyLocked(anchorGroup);
            ALOGV("layers not scored - choose %s", refreshRate.getName().c_str());
            return refreshRate;
        } else {
            return *bestRefreshRate;
        }
    }
    const RefreshRate& touchRefreshRate = getMaxRefreshRateByPolicyLocked(anchorGroup);
    const bool touchBoostForExplicitExact = [&] {
        if (mSupportsFrameRateOverrideByContent) {
            return explicitExact + noVoteLayers != layers.size();
        } else {
            return explicitExact == 0;
        }
    }();
    using fps_approx_ops::operator<;
    if (globalSignals.touch && explicitDefaultVoteLayers == 0 && touchBoostForExplicitExact &&
        bestRefreshRate->getFps() < touchRefreshRate.getFps()) {
        setTouchConsidered();
        ALOGV("TouchBoost - choose %s", touchRefreshRate.getName().c_str());
        return touchRefreshRate;
    }
    return *bestRefreshRate;
}
std::unordered_map<uid_t, std::vector<const RefreshRateConfigs::LayerRequirement*>>
groupLayersByUid(const std::vector<RefreshRateConfigs::LayerRequirement>& layers) {
    std::unordered_map<uid_t, std::vector<const RefreshRateConfigs::LayerRequirement*>> layersByUid;
    for (const auto& layer : layers) {
        auto iter = layersByUid.emplace(layer.ownerUid,
                                        std::vector<const RefreshRateConfigs::LayerRequirement*>());
        auto& layersWithSameUid = iter.first->second;
        layersWithSameUid.push_back(&layer);
    }
    for (auto iter = layersByUid.begin(); iter != layersByUid.end();) {
        const auto& layersWithSameUid = iter->second;
        bool skipUid = false;
        for (const auto& layer : layersWithSameUid) {
            if (layer->vote == RefreshRateConfigs::LayerVoteType::Max ||
                layer->vote == RefreshRateConfigs::LayerVoteType::Heuristic) {
                skipUid = true;
                break;
            }
        }
        if (skipUid) {
            iter = layersByUid.erase(iter);
        } else {
            ++iter;
        }
    }
    return layersByUid;
}
std::vector<RefreshRateScore> initializeScoresForAllRefreshRates(
        const AllRefreshRatesMapType& refreshRates) {
    std::vector<RefreshRateScore> scores;
    scores.reserve(refreshRates.size());
    for (const auto& [ignored, refreshRate] : refreshRates) {
        scores.emplace_back(RefreshRateScore{refreshRate.get(), 0.0f});
    }
    std::sort(scores.begin(), scores.end(),
              [](const auto& a, const auto& b) { return *a.refreshRate < *b.refreshRate; });
    return scores;
}
RefreshRateConfigs::UidToFrameRateOverride RefreshRateConfigs::getFrameRateOverrides(const std::vector<LayerRequirement>& layers, Fps displayFrameRate, GlobalSignals globalSignals) const {
    ATRACE_CALL();
    ALOGV("getFrameRateOverrides %zu layers", layers.size());
    std::lock_guard lock(mLock);
    std::vector<RefreshRateScore> scores = initializeScoresForAllRefreshRates(mRefreshRates);
    std::unordered_map<uid_t, std::vector<const LayerRequirement*>> layersByUid =
            groupLayersByUid(layers);
    UidToFrameRateOverride frameRateOverrides;
    for (const auto& [uid, layersWithSameUid] : layersByUid) {
        const bool hasExplicitExactOrMultiple =
                std::any_of(layersWithSameUid.cbegin(), layersWithSameUid.cend(),
                            [](const auto& layer) {
                                return layer->vote == LayerVoteType::ExplicitExactOrMultiple;
                            });
        if (globalSignals.touch && hasExplicitExactOrMultiple) {
            continue;
        }
        for (auto& score : scores) {
            score.score = 0;
        }
        for (const auto& layer : layersWithSameUid) {
            if (layer->vote == LayerVoteType::NoVote || layer->vote == LayerVoteType::Min) {
                continue;
            }
            LOG_ALWAYS_FATAL_IF(layer->vote != LayerVoteType::ExplicitDefault &&
                                layer->vote != LayerVoteType::ExplicitExactOrMultiple &&
                                layer->vote != LayerVoteType::ExplicitExact);
            for (RefreshRateScore& score : scores) {
                const auto layerScore = calculateLayerScoreLocked(*layer, *score.refreshRate,
                                                                                       true);
                score.score += layer->weight * layerScore;
            }
        }
        auto iter =
                std::remove_if(scores.begin(), scores.end(), [&](const RefreshRateScore& score) {
                    return getFrameRateDivider(displayFrameRate, score.refreshRate->getFps()) == 0;
                });
        scores.erase(iter, scores.end());
        if (std::all_of(scores.begin(), scores.end(),
                        [](const RefreshRateScore& score) { return score.score == 0; })) {
            continue;
        }
        const RefreshRate* bestRefreshRate = getBestRefreshRate(scores.begin(), scores.end());
        frameRateOverrides.emplace(uid, bestRefreshRate->getFps());
    }
    return frameRateOverrides;
}
template <typename Iter>
const RefreshRate* RefreshRateConfigs::getBestRefreshRate(Iter begin, Iter end) const {
    constexpr auto kEpsilon = 0.0001f;
    const RefreshRate* bestRefreshRate = begin->refreshRate;
    float max = begin->score;
    for (auto i = begin; i != end; ++i) {
        const auto [refreshRate, score] = *i;
        ALOGV("%s scores %.2f", refreshRate->getName().c_str(), score);
        ATRACE_INT(refreshRate->getName().c_str(), static_cast<int>(std::round(score * 100)));
        if (score > max * (1 + kEpsilon)) {
            max = score;
            bestRefreshRate = refreshRate;
        }
    }
    return bestRefreshRate;
}
std::optional<Fps> RefreshRateConfigs::onKernelTimerChanged(
        std::optional<DisplayModeId> desiredActiveConfigId, bool timerExpired) const {
    std::lock_guard lock(mLock);
    const auto& current = desiredActiveConfigId ? *mRefreshRates.at(*desiredActiveConfigId)
                                                : *mCurrentRefreshRate;
    const auto& min = *mMinSupportedRefreshRate;
    if (current != min) {
        const auto& refreshRate = timerExpired ? min : current;
        return refreshRate.getFps();
    }
    return {};
}
const RefreshRate& RefreshRateConfigs::getMinRefreshRateByPolicyLocked() const {
    for (auto refreshRate : mPrimaryRefreshRates) {
        if (mCurrentRefreshRate->getModeGroup() == refreshRate->getModeGroup()) {
            return *refreshRate;
        }
    }
    ALOGE("Can't find min refresh rate by policy with the same mode group"
          " as the current mode %s",
          mCurrentRefreshRate->toString().c_str());
    return *mPrimaryRefreshRates.front();
}
RefreshRate RefreshRateConfigs::getMaxRefreshRateByPolicy() const {
    std::lock_guard lock(mLock);
    return getMaxRefreshRateByPolicyLocked();
}
const RefreshRate& RefreshRateConfigs::getMaxRefreshRateByPolicyLocked(int anchorGroup) const {
    for (auto it = mPrimaryRefreshRates.rbegin(); it != mPrimaryRefreshRates.rend(); it++) {
        const auto& refreshRate = (**it);
        if (anchorGroup == refreshRate.getModeGroup()) {
            return refreshRate;
        }
    }
    ALOGE("Can't find max refresh rate by policy with the same mode group"
          " as the current mode %s",
          mCurrentRefreshRate->toString().c_str());
    return *mPrimaryRefreshRates.back();
}
RefreshRate RefreshRateConfigs::getCurrentRefreshRate() const {
    std::lock_guard lock(mLock);
    return *mCurrentRefreshRate;
}
RefreshRate RefreshRateConfigs::getCurrentRefreshRateByPolicy() const {
    std::lock_guard lock(mLock);
    return getCurrentRefreshRateByPolicyLocked();
}
const RefreshRate& RefreshRateConfigs::getCurrentRefreshRateByPolicyLocked() const {
    if (std::find(mAppRequestRefreshRates.begin(), mAppRequestRefreshRates.end(),
                  mCurrentRefreshRate) != mAppRequestRefreshRates.end()) {
        return *mCurrentRefreshRate;
    }
    return *mRefreshRates.at(getCurrentPolicyLocked()->defaultMode);
}
void RefreshRateConfigs::setCurrentModeId(DisplayModeId modeId) {
    std::lock_guard lock(mLock);
    lastBestRefreshRateInvocation.reset();
    mCurrentRefreshRate = mRefreshRates.at(modeId).get();
}
RefreshRateConfigs::RefreshRateConfigs(const DisplayModes& modes, DisplayModeId currentModeId,
                                       Config config)
      : mKnownFrameRates(constructKnownFrameRates(modes)), mConfig(config) {
    initializeIdleTimer();
    updateDisplayModes(modes, currentModeId);
}
void RefreshRateConfigs::initializeIdleTimer() {
    if (mConfig.idleTimerTimeoutMs > 0) {
        mIdleTimer.emplace(
                "IdleTimer", std::chrono::milliseconds(mConfig.idleTimerTimeoutMs),
                [this] {
                    std::scoped_lock lock(mIdleTimerCallbacksMutex);
                    if (const auto callbacks = getIdleTimerCallbacks()) {
                        callbacks->onReset();
                    }
                },
                [this] {
                    std::scoped_lock lock(mIdleTimerCallbacksMutex);
                    if (const auto callbacks = getIdleTimerCallbacks()) {
                        callbacks->onExpired();
                    }
                });
    }
}
void RefreshRateConfigs::updateDisplayModes(const DisplayModes& modes,
                                            DisplayModeId currentModeId) {
    std::lock_guard lock(mLock);
    LOG_ALWAYS_FATAL_IF(std::none_of(modes.begin(), modes.end(), [&](DisplayModePtr mode) {
        return mode->getId() == currentModeId;
    }));
    lastBestRefreshRateInvocation.reset();
    mRefreshRates.clear();
    for (const auto& mode : modes) {
        const auto modeId = mode->getId();
        mRefreshRates.emplace(modeId,
                              std::make_unique<RefreshRate>(mode, RefreshRate::ConstructorTag(0)));
        if (modeId == currentModeId) {
            mCurrentRefreshRate = mRefreshRates.at(modeId).get();
        }
    }
    std::vector<const RefreshRate*> sortedModes;
    getSortedRefreshRateListLocked([](const RefreshRate&) { return true; }, &sortedModes);
    mDisplayManagerPolicy = {};
    mDisplayManagerPolicy.defaultMode = currentModeId;
    mMinSupportedRefreshRate = sortedModes.front();
    mMaxSupportedRefreshRate = sortedModes.back();
    mSupportsFrameRateOverrideByContent = false;
    if (mConfig.enableFrameRateOverride) {
        for (const auto& mode1 : sortedModes) {
            for (const auto& mode2 : sortedModes) {
                if (getFrameRateDivider(mode1->getFps(), mode2->getFps()) >= 2) {
                    mSupportsFrameRateOverrideByContent = true;
                    break;
                }
            }
        }
    }
    constructAvailableRefreshRates();
}
bool RefreshRateConfigs::isPolicyValidLocked(const Policy& policy) const {
    auto iter = mRefreshRates.find(policy.defaultMode);
    if (iter == mRefreshRates.end()) {
        ALOGE("Default mode is not found.");
        return false;
    }
    const RefreshRate& refreshRate = *iter->second;
    if (!refreshRate.inPolicy(policy.primaryRange.min, policy.primaryRange.max)) {
        ALOGE("Default mode is not in the primary range.");
        return false;
    }
    using namespace fps_approx_ops;
    return policy.appRequestRange.min <= policy.primaryRange.min &&
            policy.appRequestRange.max >= policy.primaryRange.max;
}
status_t RefreshRateConfigs::setDisplayManagerPolicy(const Policy& policy) {
    std::lock_guard lock(mLock);
    if (!isPolicyValidLocked(policy)) {
        ALOGE("Invalid refresh rate policy: %s", policy.toString().c_str());
        return BAD_VALUE;
    }
    lastBestRefreshRateInvocation.reset();
    Policy previousPolicy = *getCurrentPolicyLocked();
    mDisplayManagerPolicy = policy;
    if (*getCurrentPolicyLocked() == previousPolicy) {
        return CURRENT_POLICY_UNCHANGED;
    }
    constructAvailableRefreshRates();
    return NO_ERROR;
}
status_t RefreshRateConfigs::setOverridePolicy(const std::optional<Policy>& policy) {
    std::lock_guard lock(mLock);
    if (policy && !isPolicyValidLocked(*policy)) {
        return BAD_VALUE;
    }
    lastBestRefreshRateInvocation.reset();
    Policy previousPolicy = *getCurrentPolicyLocked();
    mOverridePolicy = policy;
    if (*getCurrentPolicyLocked() == previousPolicy) {
        return CURRENT_POLICY_UNCHANGED;
    }
    constructAvailableRefreshRates();
    return NO_ERROR;
}
const RefreshRateConfigs::Policy* RefreshRateConfigs::getCurrentPolicyLocked() const {
    return mOverridePolicy ? &mOverridePolicy.value() : &mDisplayManagerPolicy;
}
RefreshRateConfigs::Policy RefreshRateConfigs::getCurrentPolicy() const {
    std::lock_guard lock(mLock);
    return *getCurrentPolicyLocked();
}
RefreshRateConfigs::Policy RefreshRateConfigs::getDisplayManagerPolicy() const {
    std::lock_guard lock(mLock);
    return mDisplayManagerPolicy;
}
bool RefreshRateConfigs::isModeAllowed(DisplayModeId modeId) const {
    std::lock_guard lock(mLock);
    for (const RefreshRate* refreshRate : mAppRequestRefreshRates) {
        if (refreshRate->getModeId() == modeId) {
            return true;
        }
    }
    return false;
}
void RefreshRateConfigs::getSortedRefreshRateListLocked(
        const std::function<bool(const RefreshRate&)>& shouldAddRefreshRate,
        std::vector<const RefreshRate*>* outRefreshRates) {
    outRefreshRates->clear();
    outRefreshRates->reserve(mRefreshRates.size());
    for (const auto& [type, refreshRate] : mRefreshRates) {
        if (shouldAddRefreshRate(*refreshRate)) {
            ALOGV("getSortedRefreshRateListLocked: mode %d added to list policy",
                  refreshRate->getModeId().value());
            outRefreshRates->push_back(refreshRate.get());
        }
    }
    std::sort(outRefreshRates->begin(), outRefreshRates->end(),
              [](const auto refreshRate1, const auto refreshRate2) {
                  if (refreshRate1->mode->getVsyncPeriod() !=
                      refreshRate2->mode->getVsyncPeriod()) {
                      return refreshRate1->mode->getVsyncPeriod() >
                              refreshRate2->mode->getVsyncPeriod();
                  } else {
                      return refreshRate1->mode->getGroup() > refreshRate2->mode->getGroup();
                  }
              });
}
void RefreshRateConfigs::constructAvailableRefreshRates() {
    const Policy* policy = getCurrentPolicyLocked();
    const auto& defaultMode = mRefreshRates.at(policy->defaultMode)->mode;
    ALOGV("constructAvailableRefreshRates: %s ", policy->toString().c_str());
    auto filterRefreshRates =
            [&](Fps min, Fps max, const char* listName,
                std::vector<const RefreshRate*>* outRefreshRates) REQUIRES(mLock) {
                getSortedRefreshRateListLocked(
                        [&](const RefreshRate& refreshRate) REQUIRES(mLock) {
                            const auto& mode = refreshRate.mode;
                            return mode->getHeight() == defaultMode->getHeight() &&
                                    mode->getWidth() == defaultMode->getWidth() &&
                                    mode->getDpiX() == defaultMode->getDpiX() &&
                                    mode->getDpiY() == defaultMode->getDpiY() &&
                                    (policy->allowGroupSwitching ||
                                     mode->getGroup() == defaultMode->getGroup()) &&
                                    refreshRate.inPolicy(min, max);
                        },
                        outRefreshRates);
                LOG_ALWAYS_FATAL_IF(outRefreshRates->empty(),
                                    "No matching modes for %s range: min=%s max=%s", listName,
                                    to_string(min).c_str(), to_string(max).c_str());
                auto stringifyRefreshRates = [&]() -> std::string {
                    std::string str;
                    for (auto refreshRate : *outRefreshRates) {
                        base::StringAppendF(&str, "%s ", refreshRate->getName().c_str());
                    }
                    return str;
                };
                ALOGV("%s refresh rates: %s", listName, stringifyRefreshRates().c_str());
            }}
Fps RefreshRateConfigs::findClosestKnownFrameRate(Fps frameRate) const {
    using namespace fps_approx_ops;
    if (frameRate <= mKnownFrameRates.front()) {
        return mKnownFrameRates.front();
    }
    if (frameRate >= mKnownFrameRates.back()) {
        return mKnownFrameRates.back();
    }
    auto lowerBound = std::lower_bound(mKnownFrameRates.begin(), mKnownFrameRates.end(), frameRate,
                                       isStrictlyLess);
    const auto distance1 = std::abs(frameRate.getValue() - lowerBound->getValue());
    const auto distance2 = std::abs(frameRate.getValue() - std::prev(lowerBound)->getValue());
    return distance1 < distance2 ? *lowerBound : *std::prev(lowerBound);
}
RefreshRateConfigs::KernelIdleTimerAction RefreshRateConfigs::getIdleTimerAction() const {
    std::lock_guard lock(mLock);
    const auto& deviceMin = *mMinSupportedRefreshRate;
    const auto& minByPolicy = getMinRefreshRateByPolicyLocked();
    const auto& maxByPolicy = getMaxRefreshRateByPolicyLocked();
    const auto& currentPolicy = getCurrentPolicyLocked();
    if (deviceMin < minByPolicy) {
        return RefreshRateConfigs::KernelIdleTimerAction::TurnOff;
    }
    if (minByPolicy == maxByPolicy) {
        if (isApproxLess(currentPolicy->primaryRange.min, deviceMin.getFps())) {
            return RefreshRateConfigs::KernelIdleTimerAction::TurnOn;
        }
        return RefreshRateConfigs::KernelIdleTimerAction::TurnOff;
    }
    return RefreshRateConfigs::KernelIdleTimerAction::TurnOn;
}
int RefreshRateConfigs::getFrameRateDivider(Fps displayFrameRate, Fps layerFrameRate) {
    constexpr float kThreshold = 0.0009f;
    const auto numPeriods = displayFrameRate.getValue() / layerFrameRate.getValue();
    const auto numPeriodsRounded = std::round(numPeriods);
    if (std::abs(numPeriods - numPeriodsRounded) > kThreshold) {
        return 0;
    }
    return static_cast<int>(numPeriodsRounded);
}
bool RefreshRateConfigs::isFractionalPairOrMultiple(Fps smaller, Fps bigger) {
    if (smaller.getValue() > bigger.getValue()) {
        return isFractionalPairOrMultiple(bigger, smaller);
    }
    const auto multiplier = std::round(bigger.getValue() / smaller.getValue());
    constexpr float kCoef = 1000.f / 1001.f;
    return bigger.equalsWithMargin(Fps(smaller.getValue() * multiplier / kCoef)) ||
            bigger.equalsWithMargin(Fps(smaller.getValue() * multiplier * kCoef));
}
void RefreshRateConfigs::dump(std::string& result) const {
    std::lock_guard lock(mLock);
    base::StringAppendF(&result, "DesiredDisplayModeSpecs (DisplayManager): %s\n\n",
                        mDisplayManagerPolicy.toString().c_str());
    scheduler::RefreshRateConfigs::Policy currentPolicy = *getCurrentPolicyLocked();
    if (mOverridePolicy && currentPolicy != mDisplayManagerPolicy) {
        base::StringAppendF(&result, "DesiredDisplayModeSpecs (Override): %s\n\n",
                            currentPolicy.toString().c_str());
    }
    auto mode = mCurrentRefreshRate->mode;
    base::StringAppendF(&result, "Current mode: %s\n", mCurrentRefreshRate->toString().c_str());
    result.append("Refresh rates:\n");
    for (const auto& [id, refreshRate] : mRefreshRates) {
        mode = refreshRate->mode;
        base::StringAppendF(&result, "\t%s\n", refreshRate->toString().c_str());
    }
    base::StringAppendF(&result, "Supports Frame Rate Override By Content: %s\n",
                        mSupportsFrameRateOverrideByContent ? "yes" : "no");
    base::StringAppendF(&result, "Idle timer: (%s) %s\n",
                        mConfig.supportKernelIdleTimer ? "kernel" : "platform",
                        mIdleTimer ? mIdleTimer->dump().c_str() : "off");
    result.append("\n");
}
