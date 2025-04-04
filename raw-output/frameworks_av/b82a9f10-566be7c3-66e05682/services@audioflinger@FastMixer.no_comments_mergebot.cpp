#define LOG_TAG "FastMixer"
#define ATRACE_TAG ATRACE_TAG_AUDIO
#include "Configuration.h"
#include <sys/atomics.h>
#include <time.h>
#include <utils/Log.h>
#include <utils/Trace.h>
#include <system/audio.h>
#ifdef FAST_MIXER_STATISTICS
#include <cpustats/CentralTendencyStatistics.h>
#ifdef CPU_FREQUENCY_STATISTICS
#include <cpustats/ThreadCpuUsage.h>
#endif
#endif
#include "AudioMixer.h"
#include "FastMixer.h"
#define FAST_HOT_IDLE_NS 1000000L
#define FAST_DEFAULT_NS 999999999L
#define MIN_WARMUP_CYCLES 2
#define MAX_WARMUP_CYCLES 10
#define FCC_2 2
namespace android {
bool FastMixer::threadLoop()
{
    static const FastMixerState initial;
    const FastMixerState *previous = &initial, *current = &initial;
    FastMixerState preIdle;
    struct timespec oldTs = {0, 0};
    bool oldTsValid = false;
    long slopNs = 0;
    long sleepNs = -1;
    int fastTrackNames[FastMixerState::kMaxFastTracks];
    int generations[FastMixerState::kMaxFastTracks];
    unsigned i;
    for (i = 0; i < FastMixerState::kMaxFastTracks; ++i) {
        fastTrackNames[i] = -1;
        generations[i] = 0;
    }
    NBAIO_Sink *outputSink = NULL;
    int outputSinkGen = 0;
    AudioMixer* mixer = NULL;
    short *mixBuffer = NULL;
    enum {UNDEFINED, MIXED, ZEROED} mixBufferState = UNDEFINED;
    NBAIO_Format format = Format_Invalid;
    unsigned sampleRate = 0;
    int fastTracksGen = 0;
    long periodNs = 0;
    long underrunNs = 0;
    long overrunNs = 0;
    long forceNs = 0;
    long warmupNs = 0;
    FastMixerDumpState dummyDumpState, *dumpState = &dummyDumpState;
    bool ignoreNextOverrun = true;
#ifdef FAST_MIXER_STATISTICS
    struct timespec oldLoad = {0, 0};
    bool oldLoadValid = false;
    uint32_t bounds = 0;
    bool full = false;
#ifdef CPU_FREQUENCY_STATISTICS
    ThreadCpuUsage tcu;
#endif
#endif
    unsigned coldGen = 0;
    bool isWarm = false;
    struct timespec measuredWarmupTs = {0, 0};
    uint32_t warmupCycles = 0;
    NBAIO_Sink* teeSink = NULL;
    NBLog::Writer dummyLogWriter, *logWriter = &dummyLogWriter;
    uint32_t totalNativeFramesWritten = 0;
    AudioTimestamp timestamp;
    uint32_t nativeFramesWrittenButNotPresented = 0;
    status_t timestampStatus = INVALID_OPERATION;
    for (;;) {
        if (sleepNs >= 0) {
            if (sleepNs > 0) {
                ALOG_ASSERT(sleepNs < 1000000000);
                const struct timespec req = {0, sleepNs};
                nanosleep(&req, NULL);
            } else {
                sched_yield();
            }
        }
        sleepNs = FAST_DEFAULT_NS;
        const FastMixerState *next = mSQ.poll();
        if (next == NULL) {
            ALOG_ASSERT(current == &initial && previous == &initial);
            next = current;
        }
        FastMixerState::Command command = next->mCommand;
        if (next != current) {
            dumpState = next->mDumpState != NULL ? next->mDumpState : &dummyDumpState;
            teeSink = next->mTeeSink;
            logWriter = next->mNBLogWriter != NULL ? next->mNBLogWriter : &dummyLogWriter;
            if (mixer != NULL) {
                mixer->setLog(logWriter);
            }
            if (!(current->mCommand & FastMixerState::IDLE)) {
                if (command & FastMixerState::IDLE) {
                    preIdle = *current;
                    current = &preIdle;
                    oldTsValid = false;
#ifdef FAST_MIXER_STATISTICS
                    oldLoadValid = false;
#endif
                    ignoreNextOverrun = true;
                }
                previous = current;
            }
            current = next;
        }
#if !LOG_NDEBUG
        next = NULL;
#endif
        dumpState->mCommand = command;
        switch (command) {
        case FastMixerState::INITIAL:
        case FastMixerState::HOT_IDLE:
            sleepNs = FAST_HOT_IDLE_NS;
            continue;
        case FastMixerState::COLD_IDLE:
            if (current->mColdGen != coldGen) {
                int32_t *coldFutexAddr = current->mColdFutexAddr;
                ALOG_ASSERT(coldFutexAddr != NULL);
                int32_t old = android_atomic_dec(coldFutexAddr);
                if (old <= 0) {
                    __futex_syscall4(coldFutexAddr, FUTEX_WAIT_PRIVATE, old - 1, NULL);
                }
                int policy = sched_getscheduler(0);
                if (!(policy == SCHED_FIFO || policy == SCHED_RR)) {
                    ALOGE("did not receive expected priority boost");
                }
                isWarm = false;
                measuredWarmupTs.tv_sec = 0;
                measuredWarmupTs.tv_nsec = 0;
                warmupCycles = 0;
                sleepNs = -1;
                coldGen = current->mColdGen;
#ifdef FAST_MIXER_STATISTICS
                bounds = 0;
                full = false;
#endif
                oldTsValid = !clock_gettime(CLOCK_MONOTONIC, &oldTs);
                timestampStatus = INVALID_OPERATION;
            } else {
                sleepNs = FAST_HOT_IDLE_NS;
            }
            continue;
        case FastMixerState::EXIT:
            delete mixer;
            delete[] mixBuffer;
            return false;
        case FastMixerState::MIX:
        case FastMixerState::WRITE:
        case FastMixerState::MIX_WRITE:
            break;
        default:
            LOG_FATAL("bad command %d", command);
        }
        size_t frameCount = current->mFrameCount;
        if (current != previous) {
            unsigned previousTrackMask;
            NBAIO_Format previousFormat = format;
            if (current->mOutputSinkGen != outputSinkGen) {
                outputSink = current->mOutputSink;
                outputSinkGen = current->mOutputSinkGen;
                if (outputSink == NULL) {
                    format = Format_Invalid;
                    sampleRate = 0;
                } else {
                    format = outputSink->format();
                    sampleRate = Format_sampleRate(format);
                    ALOG_ASSERT(Format_channelCount(format) == FCC_2);
                }
            }
            if ((!Format_isEqual(format, previousFormat)) || (frameCount != previous->mFrameCount)) {
                delete mixer;
                mixer = NULL;
                delete[] mixBuffer;
                mixBuffer = NULL;
                if (frameCount > 0 && sampleRate > 0) {
                    mixer = new AudioMixer(frameCount, sampleRate, FastMixerState::kMaxFastTracks);
                    mixBuffer = new short[frameCount * FCC_2];
                    periodNs = (frameCount * 1000000000LL) / sampleRate;
                    underrunNs = (frameCount * 1750000000LL) / sampleRate;
                    overrunNs = (frameCount * 500000000LL) / sampleRate;
                    forceNs = (frameCount * 950000000LL) / sampleRate;
                    warmupNs = (frameCount * 500000000LL) / sampleRate;
                } else {
                    periodNs = 0;
                    underrunNs = 0;
                    overrunNs = 0;
                    forceNs = 0;
                    warmupNs = 0;
                }
                mixBufferState = UNDEFINED;
#if !LOG_NDEBUG
                for (i = 0; i < FastMixerState::kMaxFastTracks; ++i) {
                    fastTrackNames[i] = -1;
                }
#endif
                previousTrackMask = 0;
                fastTracksGen = current->mFastTracksGen - 1;
                dumpState->mFrameCount = frameCount;
            } else {
                previousTrackMask = previous->mTrackMask;
            }
            unsigned currentTrackMask = current->mTrackMask;
            dumpState->mTrackMask = currentTrackMask;
            if (current->mFastTracksGen != fastTracksGen) {
                ALOG_ASSERT(mixBuffer != NULL);
                int name;
                unsigned removedTracks = previousTrackMask & ~currentTrackMask;
                while (removedTracks != 0) {
                    i = __builtin_ctz(removedTracks);
                    removedTracks &= ~(1 << i);
                    const FastTrack* fastTrack = &current->mFastTracks[i];
                    ALOG_ASSERT(fastTrack->mBufferProvider == NULL);
                    if (mixer != NULL) {
                        name = fastTrackNames[i];
                        ALOG_ASSERT(name >= 0);
                        mixer->deleteTrackName(name);
                    }
#if !LOG_NDEBUG
                    fastTrackNames[i] = -1;
#endif
                    generations[i] = fastTrack->mGeneration;
                }
                unsigned addedTracks = currentTrackMask & ~previousTrackMask;
                while (addedTracks != 0) {
                    i = __builtin_ctz(addedTracks);
                    addedTracks &= ~(1 << i);
                    const FastTrack* fastTrack = &current->mFastTracks[i];
                    AudioBufferProvider *bufferProvider = fastTrack->mBufferProvider;
                    ALOG_ASSERT(bufferProvider != NULL && fastTrackNames[i] == -1);
                    if (mixer != NULL) {
                        name = mixer->getTrackName(AUDIO_CHANNEL_OUT_STEREO, -555);
                        ALOG_ASSERT(name >= 0);
                        fastTrackNames[i] = name;
                        mixer->setBufferProvider(name, bufferProvider);
                        mixer->setParameter(name, AudioMixer::TRACK, AudioMixer::MAIN_BUFFER,
                                (void *) mixBuffer);
                        mixer->setParameter(name, AudioMixer::TRACK, AudioMixer::CHANNEL_MASK,
                                (void *)(uintptr_t)fastTrack->mChannelMask);
                        mixer->enable(name);
                    }
                    generations[i] = fastTrack->mGeneration;
                }
                unsigned modifiedTracks = currentTrackMask & previousTrackMask;
                while (modifiedTracks != 0) {
                    i = __builtin_ctz(modifiedTracks);
                    modifiedTracks &= ~(1 << i);
                    const FastTrack* fastTrack = &current->mFastTracks[i];
                    if (fastTrack->mGeneration != generations[i]) {
                        AudioBufferProvider *bufferProvider = fastTrack->mBufferProvider;
                        ALOG_ASSERT(bufferProvider != NULL);
                        if (mixer != NULL) {
                            name = fastTrackNames[i];
                            ALOG_ASSERT(name >= 0);
                            mixer->setBufferProvider(name, bufferProvider);
                            if (fastTrack->mVolumeProvider == NULL) {
                                mixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME0,
                                        (void *)0x1000);
                                mixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME1,
                                        (void *)0x1000);
                            }
                            mixer->setParameter(name, AudioMixer::RESAMPLE,
                                    AudioMixer::REMOVE, NULL);
                            mixer->setParameter(name, AudioMixer::TRACK, AudioMixer::CHANNEL_MASK,
                                    (void *)(uintptr_t) fastTrack->mChannelMask);
                        }
                        generations[i] = fastTrack->mGeneration;
                    }
                }
                fastTracksGen = current->mFastTracksGen;
                dumpState->mNumTracks = popcount(currentTrackMask);
            }
#if 1
            previous = current;
#endif
        }
        if ((command & FastMixerState::MIX) && (mixer != NULL) && isWarm) {
            ALOG_ASSERT(mixBuffer != NULL);
            unsigned currentTrackMask = current->mTrackMask;
            while (currentTrackMask != 0) {
                i = __builtin_ctz(currentTrackMask);
                currentTrackMask &= ~(1 << i);
                const FastTrack* fastTrack = &current->mFastTracks[i];
                if (timestampStatus == NO_ERROR) {
                    uint32_t trackFramesWrittenButNotPresented =
                        nativeFramesWrittenButNotPresented;
                    uint32_t trackFramesWritten = fastTrack->mBufferProvider->framesReleased();
                    if (trackFramesWritten >= trackFramesWrittenButNotPresented) {
                        AudioTimestamp perTrackTimestamp;
                        perTrackTimestamp.mPosition =
                                trackFramesWritten - trackFramesWrittenButNotPresented;
                        perTrackTimestamp.mTime = timestamp.mTime;
                        fastTrack->mBufferProvider->onTimestamp(perTrackTimestamp);
                    }
                }
                int name = fastTrackNames[i];
                ALOG_ASSERT(name >= 0);
                if (fastTrack->mVolumeProvider != NULL) {
                    uint32_t vlr = fastTrack->mVolumeProvider->getVolumeLR();
                    mixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME0,
                            (void *)(uintptr_t)(vlr & 0xFFFF));
                    mixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME1,
                            (void *)(uintptr_t)(vlr >> 16));
                }
                size_t framesReady = fastTrack->mBufferProvider->framesReady();
                if (ATRACE_ENABLED()) {
                    char traceName[16];
                    strcpy(traceName, "fRdy");
                    traceName[4] = i + (i < 10 ? '0' : 'A' - 10);
                    traceName[5] = '\0';
                    ATRACE_INT(traceName, framesReady);
                }
                FastTrackDump *ftDump = &dumpState->mTracks[i];
                FastTrackUnderruns underruns = ftDump->mUnderruns;
                if (framesReady < frameCount) {
                    if (framesReady == 0) {
                        underruns.mBitFields.mEmpty++;
                        underruns.mBitFields.mMostRecent = UNDERRUN_EMPTY;
                        mixer->disable(name);
                    } else {
                        underruns.mBitFields.mPartial++;
                        underruns.mBitFields.mMostRecent = UNDERRUN_PARTIAL;
                        mixer->enable(name);
                    }
                } else {
                    underruns.mBitFields.mFull++;
                    underruns.mBitFields.mMostRecent = UNDERRUN_FULL;
                    mixer->enable(name);
                }
                ftDump->mUnderruns = underruns;
                ftDump->mFramesReady = framesReady;
            }
            int64_t pts;
            if (outputSink == NULL || (OK != outputSink->getNextWriteTimestamp(&pts))) {
                pts = AudioBufferProvider::kInvalidPTS;
            }
            mixer->process(pts);
            mixBufferState = MIXED;
        } else if (mixBufferState == MIXED) {
            mixBufferState = UNDEFINED;
        }
        bool attemptedWrite = false;
        if ((command & FastMixerState::WRITE) && (outputSink != NULL) && (mixBuffer != NULL)) {
            if (mixBufferState == UNDEFINED) {
                memset(mixBuffer, 0, frameCount * FCC_2 * sizeof(short));
                mixBufferState = ZEROED;
            }
            if (teeSink != NULL) {
                (void) teeSink->write(mixBuffer, frameCount);
            }
            dumpState->mWriteSequence++;
            ATRACE_BEGIN("write");
            ssize_t framesWritten = outputSink->write(mixBuffer, frameCount);
            ATRACE_END();
            dumpState->mWriteSequence++;
            if (framesWritten >= 0) {
                ALOG_ASSERT((size_t) framesWritten <= frameCount);
                totalNativeFramesWritten += framesWritten;
                dumpState->mFramesWritten = totalNativeFramesWritten;
            } else {
                dumpState->mWriteErrors++;
            }
            attemptedWrite = true;
            timestampStatus = outputSink->getTimestamp(timestamp);
            if (timestampStatus == NO_ERROR) {
                uint32_t totalNativeFramesPresented = timestamp.mPosition;
                if (totalNativeFramesPresented <= totalNativeFramesWritten) {
                    nativeFramesWrittenButNotPresented =
                        totalNativeFramesWritten - totalNativeFramesPresented;
                } else {
                    timestampStatus = INVALID_OPERATION;
                }
            }
        }
        struct timespec newTs;
        int rc = clock_gettime(CLOCK_MONOTONIC, &newTs);
        if (rc == 0) {
            if (oldTsValid) {
                time_t sec = newTs.tv_sec - oldTs.tv_sec;
                long nsec = newTs.tv_nsec - oldTs.tv_nsec;
                ALOGE_IF(sec < 0 || (sec == 0 && nsec < 0),
                        "clock_gettime(CLOCK_MONOTONIC) failed: was %ld.%09ld but now %ld.%09ld",
                        oldTs.tv_sec, oldTs.tv_nsec, newTs.tv_sec, newTs.tv_nsec);
                if (nsec < 0) {
                    --sec;
                    nsec += 1000000000;
                }
                if (!isWarm && attemptedWrite) {
                    measuredWarmupTs.tv_sec += sec;
                    measuredWarmupTs.tv_nsec += nsec;
                    if (measuredWarmupTs.tv_nsec >= 1000000000) {
                        measuredWarmupTs.tv_sec++;
                        measuredWarmupTs.tv_nsec -= 1000000000;
                    }
                    ++warmupCycles;
                    if ((nsec > warmupNs && warmupCycles >= MIN_WARMUP_CYCLES) ||
                            (warmupCycles >= MAX_WARMUP_CYCLES)) {
                        isWarm = true;
                        dumpState->mMeasuredWarmupTs = measuredWarmupTs;
                        dumpState->mWarmupCycles = warmupCycles;
                    }
                }
                sleepNs = -1;
                if (isWarm) {
                    if (sec > 0 || nsec > underrunNs) {
                        ATRACE_NAME("underrun");
                        ALOGV("underrun: time since last cycle %d.%03ld sec",
                                (int) sec, nsec / 1000000L);
                        dumpState->mUnderruns++;
                        ignoreNextOverrun = true;
                    } else if (nsec < overrunNs) {
                        if (ignoreNextOverrun) {
                            ignoreNextOverrun = false;
                        } else {
                            ALOGV("overrun: time since last cycle %d.%03ld sec",
                                    (int) sec, nsec / 1000000L);
                            dumpState->mOverruns++;
                        }
                        sleepNs = forceNs - nsec;
                    } else {
                        ignoreNextOverrun = false;
                    }
                }
#ifdef FAST_MIXER_STATISTICS
                if (isWarm) {
                    size_t i = bounds & (dumpState->mSamplingN - 1);
                    bounds = (bounds & 0xFFFF0000) | ((bounds + 1) & 0xFFFF);
                    if (full) {
                        bounds += 0x10000;
                    } else if (!(bounds & (dumpState->mSamplingN - 1))) {
                        full = true;
                    }
                    uint32_t monotonicNs = nsec;
                    if (sec > 0 && sec < 4) {
                        monotonicNs += sec * 1000000000;
                    }
                    uint32_t loadNs = 0;
                    struct timespec newLoad;
                    rc = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &newLoad);
                    if (rc == 0) {
                        if (oldLoadValid) {
                            sec = newLoad.tv_sec - oldLoad.tv_sec;
                            nsec = newLoad.tv_nsec - oldLoad.tv_nsec;
                            if (nsec < 0) {
                                --sec;
                                nsec += 1000000000;
                            }
                            loadNs = nsec;
                            if (sec > 0 && sec < 4) {
                                loadNs += sec * 1000000000;
                            }
                        } else {
                            oldLoadValid = true;
                        }
                        oldLoad = newLoad;
                    }
#ifdef CPU_FREQUENCY_STATISTICS
                    int cpuNum = sched_getcpu();
                    uint32_t kHz = tcu.getCpukHz(cpuNum);
                    kHz = (kHz << 4) | (cpuNum & 0xF);
#endif
                    dumpState->mMonotonicNs[i] = monotonicNs;
                    dumpState->mLoadNs[i] = loadNs;
#ifdef CPU_FREQUENCY_STATISTICS
                    dumpState->mCpukHz[i] = kHz;
#endif
                    dumpState->mBounds = bounds;
                    ATRACE_INT("cycle_ms", monotonicNs / 1000000);
                    ATRACE_INT("load_us", loadNs / 1000);
                }
#endif
            } else {
                oldTsValid = true;
                sleepNs = periodNs;
                ignoreNextOverrun = true;
            }
            oldTs = newTs;
        } else {
            oldTsValid = false;
            sleepNs = periodNs;
        }
    }
}
#ifdef FAST_MIXER_STATISTICS
        uint32_t samplingN
#endif
        ) :
    mCommand(FastMixerState::INITIAL), mWriteSequence(0), mFramesWritten(0),
    mNumTracks(0), mWriteErrors(0), mUnderruns(0), mOverruns(0),
    mSampleRate(0), mFrameCount(0), mWarmupCycles(0),
    mTrackMask(0)
#ifdef FAST_MIXER_STATISTICS
    , mSamplingN(0), mBounds(0)
#endif
{
    mMeasuredWarmupTs.tv_sec = 0;
    mMeasuredWarmupTs.tv_nsec = 0;
#ifdef FAST_MIXER_STATISTICS
    increaseSamplingN(samplingN);
#endif
}
#ifdef FAST_MIXER_STATISTICS
void FastMixerDumpState::increaseSamplingN(uint32_t samplingN)
{
    if (samplingN <= mSamplingN || samplingN > kSamplingN || roundup(samplingN) != samplingN) {
        return;
    }
    uint32_t additional = samplingN - mSamplingN;
    memset(&mMonotonicNs[mSamplingN], 0, sizeof(mMonotonicNs[0]) * additional);
    memset(&mLoadNs[mSamplingN], 0, sizeof(mLoadNs[0]) * additional);
#ifdef CPU_FREQUENCY_STATISTICS
    memset(&mCpukHz[mSamplingN], 0, sizeof(mCpukHz[0]) * additional);
#endif
    mSamplingN = samplingN;
}
#endif
FastMixerDumpState::~FastMixerDumpState()
{
}
static int compare_uint32_t(const void *pa, const void *pb)
{
    uint32_t a = *(const uint32_t *)pa;
    uint32_t b = *(const uint32_t *)pb;
    if (a < b) {
        return -1;
    } else if (a > b) {
        return 1;
    } else {
        return 0;
    }
}
void FastMixerDumpState::dump(int fd) const
{
    if (mCommand == FastMixerState::INITIAL) {
        fdprintf(fd, "  FastMixer not initialized\n");
        return;
    }
#define COMMAND_MAX 32
    char string[COMMAND_MAX];
    switch (mCommand) {
    case FastMixerState::INITIAL:
        strcpy(string, "INITIAL");
        break;
    case FastMixerState::HOT_IDLE:
        strcpy(string, "HOT_IDLE");
        break;
    case FastMixerState::COLD_IDLE:
        strcpy(string, "COLD_IDLE");
        break;
    case FastMixerState::EXIT:
        strcpy(string, "EXIT");
        break;
    case FastMixerState::MIX:
        strcpy(string, "MIX");
        break;
    case FastMixerState::WRITE:
        strcpy(string, "WRITE");
        break;
    case FastMixerState::MIX_WRITE:
        strcpy(string, "MIX_WRITE");
        break;
    default:
        snprintf(string, COMMAND_MAX, "%d", mCommand);
        break;
    }
    double measuredWarmupMs = (mMeasuredWarmupTs.tv_sec * 1000.0) +
            (mMeasuredWarmupTs.tv_nsec / 1000000.0);
    double mixPeriodSec = (double) mFrameCount / (double) mSampleRate;
    fdprintf(fd, "  FastMixer command=%s writeSequence=%u framesWritten=%u\n"
                 "            numTracks=%u writeErrors=%u underruns=%u overruns=%u\n"
                 "            sampleRate=%u frameCount=%u measuredWarmup=%.3g ms, warmupCycles=%u\n"
                 "            mixPeriod=%.2f ms\n",
                 string, mWriteSequence, mFramesWritten,
                 mNumTracks, mWriteErrors, mUnderruns, mOverruns,
                 mSampleRate, mFrameCount, measuredWarmupMs, mWarmupCycles,
                 mixPeriodSec * 1e3);
#ifdef FAST_MIXER_STATISTICS
    uint32_t bounds = mBounds;
    uint32_t newestOpen = bounds & 0xFFFF;
    uint32_t oldestClosed = bounds >> 16;
    uint32_t n = (newestOpen - oldestClosed) & 0xFFFF;
    if (n > mSamplingN) {
        ALOGE("too many samples %u", n);
        n = mSamplingN;
    }
    CentralTendencyStatistics wall, loadNs;
#ifdef CPU_FREQUENCY_STATISTICS
    CentralTendencyStatistics kHz, loadMHz;
    uint32_t previousCpukHz = 0;
#endif
    static const uint32_t kTailDenominator = 1000;
    uint32_t *tail = n >= kTailDenominator ? new uint32_t[n] : NULL;
    for (uint32_t j = 0; j < n; ++j) {
        size_t i = oldestClosed++ & (mSamplingN - 1);
        uint32_t wallNs = mMonotonicNs[i];
        if (tail != NULL) {
            tail[j] = wallNs;
        }
        wall.sample(wallNs);
        uint32_t sampleLoadNs = mLoadNs[i];
        loadNs.sample(sampleLoadNs);
#ifdef CPU_FREQUENCY_STATISTICS
        uint32_t sampleCpukHz = mCpukHz[i];
        if ((sampleCpukHz & ~0xF) != 0) {
            kHz.sample(sampleCpukHz >> 4);
            if (sampleCpukHz == previousCpukHz) {
                double megacycles = (double) sampleLoadNs * (double) (sampleCpukHz >> 4) * 1e-12;
                double adjMHz = megacycles / mixPeriodSec;
                loadMHz.sample(adjMHz);
            }
        }
        previousCpukHz = sampleCpukHz;
#endif
    }
    if (n) {
        fdprintf(fd, "  Simple moving statistics over last %.1f seconds:\n",
                     wall.n() * mixPeriodSec);
        fdprintf(fd, "    wall clock time in ms per mix cycle:\n"
                     "      mean=%.2f min=%.2f max=%.2f stddev=%.2f\n",
                     wall.mean()*1e-6, wall.minimum()*1e-6, wall.maximum()*1e-6,
                     wall.stddev()*1e-6);
        fdprintf(fd, "    raw CPU load in us per mix cycle:\n"
                     "      mean=%.0f min=%.0f max=%.0f stddev=%.0f\n",
                     loadNs.mean()*1e-3, loadNs.minimum()*1e-3, loadNs.maximum()*1e-3,
                     loadNs.stddev()*1e-3);
    } else {
        fdprintf(fd, "  No FastMixer statistics available currently\n");
    }
#ifdef CPU_FREQUENCY_STATISTICS
    fdprintf(fd, "  CPU clock frequency in MHz:\n"
                 "    mean=%.0f min=%.0f max=%.0f stddev=%.0f\n",
                 kHz.mean()*1e-3, kHz.minimum()*1e-3, kHz.maximum()*1e-3, kHz.stddev()*1e-3);
    fdprintf(fd, "  adjusted CPU load in MHz (i.e. normalized for CPU clock frequency):\n"
                 "    mean=%.1f min=%.1f max=%.1f stddev=%.1f\n",
                 loadMHz.mean(), loadMHz.minimum(), loadMHz.maximum(), loadMHz.stddev());
#endif
    if (tail != NULL) {
        qsort(tail, n, sizeof(uint32_t), compare_uint32_t);
        uint32_t count = n / kTailDenominator;
        CentralTendencyStatistics left, right;
        for (uint32_t i = 0; i < count; ++i) {
            left.sample(tail[i]);
            right.sample(tail[n - (i + 1)]);
        }
        fdprintf(fd, "  Distribution of mix cycle times in ms for the tails (> ~3 stddev outliers):\n"
                     "    left tail: mean=%.2f min=%.2f max=%.2f stddev=%.2f\n"
                     "    right tail: mean=%.2f min=%.2f max=%.2f stddev=%.2f\n",
                     left.mean()*1e-6, left.minimum()*1e-6, left.maximum()*1e-6, left.stddev()*1e-6,
                     right.mean()*1e-6, right.minimum()*1e-6, right.maximum()*1e-6,
                     right.stddev()*1e-6);
        delete[] tail;
    }
#endif
    uint32_t trackMask = mTrackMask;
    fdprintf(fd, "  Fast tracks: kMaxFastTracks=%u activeMask=%#x\n",
            FastMixerState::kMaxFastTracks, trackMask);
    fdprintf(fd, "  Index Active Full Partial Empty  Recent Ready\n");
    for (uint32_t i = 0; i < FastMixerState::kMaxFastTracks; ++i, trackMask >>= 1) {
        bool isActive = trackMask & 1;
        const FastTrackDump *ftDump = &mTracks[i];
        const FastTrackUnderruns& underruns = ftDump->mUnderruns;
        const char *mostRecent;
        switch (underruns.mBitFields.mMostRecent) {
        case UNDERRUN_FULL:
            mostRecent = "full";
            break;
        case UNDERRUN_PARTIAL:
            mostRecent = "partial";
            break;
        case UNDERRUN_EMPTY:
            mostRecent = "empty";
            break;
        default:
            mostRecent = "?";
            break;
        }
        fdprintf(fd, "  %5u %6s %4u %7u %5u %7s %5u\n", i, isActive ? "yes" : "no",
                (underruns.mBitFields.mFull) & UNDERRUN_MASK,
                (underruns.mBitFields.mPartial) & UNDERRUN_MASK,
                (underruns.mBitFields.mEmpty) & UNDERRUN_MASK,
                mostRecent, ftDump->mFramesReady);
    }
}
#ifdef FAST_MIXER_STATISTICS
        uint32_t samplingN
#endif
        ) :
    mCommand(FastMixerState::INITIAL), mWriteSequence(0), mFramesWritten(0),
    mNumTracks(0), mWriteErrors(0), mUnderruns(0), mOverruns(0),
    mSampleRate(0), mFrameCount(0), mWarmupCycles(0),
    mTrackMask(0)
#ifdef FAST_MIXER_STATISTICS
    , mSamplingN(0), mBounds(0)
#endif
{
    mMeasuredWarmupTs.tv_sec = 0;
    mMeasuredWarmupTs.tv_nsec = 0;
#ifdef FAST_MIXER_STATISTICS
    increaseSamplingN(samplingN);
#endif
}
#ifdef FAST_MIXER_STATISTICS
void FastMixerDumpState::increaseSamplingN(uint32_t samplingN)
{
    if (samplingN <= mSamplingN || samplingN > kSamplingN || roundup(samplingN) != samplingN) {
        return;
    }
    uint32_t additional = samplingN - mSamplingN;
    memset(&mMonotonicNs[mSamplingN], 0, sizeof(mMonotonicNs[0]) * additional);
    memset(&mLoadNs[mSamplingN], 0, sizeof(mLoadNs[0]) * additional);
#ifdef CPU_FREQUENCY_STATISTICS
    memset(&mCpukHz[mSamplingN], 0, sizeof(mCpukHz[0]) * additional);
#endif
    mSamplingN = samplingN;
}
#endif
FastMixerDumpState::~FastMixerDumpState()
{
}
static int compare_uint32_t(const void *pa, const void *pb)
{
    uint32_t a = *(const uint32_t *)pa;
    uint32_t b = *(const uint32_t *)pb;
    if (a < b) {
        return -1;
    } else if (a > b) {
        return 1;
    } else {
        return 0;
    }
}
void FastMixerDumpState::dump(int fd) const
{
    if (mCommand == FastMixerState::INITIAL) {
        fdprintf(fd, "FastMixer not initialized\n");
        return;
    }
#define COMMAND_MAX 32
    char string[COMMAND_MAX];
    switch (mCommand) {
    case FastMixerState::INITIAL:
        strcpy(string, "INITIAL");
        break;
    case FastMixerState::HOT_IDLE:
        strcpy(string, "HOT_IDLE");
        break;
    case FastMixerState::COLD_IDLE:
        strcpy(string, "COLD_IDLE");
        break;
    case FastMixerState::EXIT:
        strcpy(string, "EXIT");
        break;
    case FastMixerState::MIX:
        strcpy(string, "MIX");
        break;
    case FastMixerState::WRITE:
        strcpy(string, "WRITE");
        break;
    case FastMixerState::MIX_WRITE:
        strcpy(string, "MIX_WRITE");
        break;
    default:
        snprintf(string, COMMAND_MAX, "%d", mCommand);
        break;
    }
    double measuredWarmupMs = (mMeasuredWarmupTs.tv_sec * 1000.0) +
            (mMeasuredWarmupTs.tv_nsec / 1000000.0);
    double mixPeriodSec = (double) mFrameCount / (double) mSampleRate;
    fdprintf(fd, "FastMixer command=%s writeSequence=%u framesWritten=%u\n"
                 "          numTracks=%u writeErrors=%u underruns=%u overruns=%u\n"
                 "          sampleRate=%u frameCount=%zu measuredWarmup=%.3g ms, warmupCycles=%u\n"
                 "          mixPeriod=%.2f ms\n",
                 string, mWriteSequence, mFramesWritten,
                 mNumTracks, mWriteErrors, mUnderruns, mOverruns,
                 mSampleRate, mFrameCount, measuredWarmupMs, mWarmupCycles,
                 mixPeriodSec * 1e3);
#ifdef FAST_MIXER_STATISTICS
    uint32_t bounds = mBounds;
    uint32_t newestOpen = bounds & 0xFFFF;
    uint32_t oldestClosed = bounds >> 16;
    uint32_t n = (newestOpen - oldestClosed) & 0xFFFF;
    if (n > mSamplingN) {
        ALOGE("too many samples %u", n);
        n = mSamplingN;
    }
    CentralTendencyStatistics wall, loadNs;
#ifdef CPU_FREQUENCY_STATISTICS
    CentralTendencyStatistics kHz, loadMHz;
    uint32_t previousCpukHz = 0;
#endif
    static const uint32_t kTailDenominator = 1000;
    uint32_t *tail = n >= kTailDenominator ? new uint32_t[n] : NULL;
    for (uint32_t j = 0; j < n; ++j) {
        size_t i = oldestClosed++ & (mSamplingN - 1);
        uint32_t wallNs = mMonotonicNs[i];
        if (tail != NULL) {
            tail[j] = wallNs;
        }
        wall.sample(wallNs);
        uint32_t sampleLoadNs = mLoadNs[i];
        loadNs.sample(sampleLoadNs);
#ifdef CPU_FREQUENCY_STATISTICS
        uint32_t sampleCpukHz = mCpukHz[i];
        if ((sampleCpukHz & ~0xF) != 0) {
            kHz.sample(sampleCpukHz >> 4);
            if (sampleCpukHz == previousCpukHz) {
                double megacycles = (double) sampleLoadNs * (double) (sampleCpukHz >> 4) * 1e-12;
                double adjMHz = megacycles / mixPeriodSec;
                loadMHz.sample(adjMHz);
            }
        }
        previousCpukHz = sampleCpukHz;
#endif
    }
    fdprintf(fd, "Simple moving statistics over last %.1f seconds:\n", wall.n() * mixPeriodSec);
    fdprintf(fd, "  wall clock time in ms per mix cycle:\n"
                 "    mean=%.2f min=%.2f max=%.2f stddev=%.2f\n",
                 wall.mean()*1e-6, wall.minimum()*1e-6, wall.maximum()*1e-6, wall.stddev()*1e-6);
    fdprintf(fd, "  raw CPU load in us per mix cycle:\n"
                 "    mean=%.0f min=%.0f max=%.0f stddev=%.0f\n",
                 loadNs.mean()*1e-3, loadNs.minimum()*1e-3, loadNs.maximum()*1e-3,
                 loadNs.stddev()*1e-3);
#ifdef CPU_FREQUENCY_STATISTICS
    fdprintf(fd, "  CPU clock frequency in MHz:\n"
                 "    mean=%.0f min=%.0f max=%.0f stddev=%.0f\n",
                 kHz.mean()*1e-3, kHz.minimum()*1e-3, kHz.maximum()*1e-3, kHz.stddev()*1e-3);
    fdprintf(fd, "  adjusted CPU load in MHz (i.e. normalized for CPU clock frequency):\n"
                 "    mean=%.1f min=%.1f max=%.1f stddev=%.1f\n",
                 loadMHz.mean(), loadMHz.minimum(), loadMHz.maximum(), loadMHz.stddev());
#endif
    if (tail != NULL) {
        qsort(tail, n, sizeof(uint32_t), compare_uint32_t);
        uint32_t count = n / kTailDenominator;
        CentralTendencyStatistics left, right;
        for (uint32_t i = 0; i < count; ++i) {
            left.sample(tail[i]);
            right.sample(tail[n - (i + 1)]);
        }
        fdprintf(fd, "Distribution of mix cycle times in ms for the tails (> ~3 stddev outliers):\n"
                     "  left tail: mean=%.2f min=%.2f max=%.2f stddev=%.2f\n"
                     "  right tail: mean=%.2f min=%.2f max=%.2f stddev=%.2f\n",
                     left.mean()*1e-6, left.minimum()*1e-6, left.maximum()*1e-6, left.stddev()*1e-6,
                     right.mean()*1e-6, right.minimum()*1e-6, right.maximum()*1e-6,
                     right.stddev()*1e-6);
        delete[] tail;
    }
#endif
    uint32_t trackMask = mTrackMask;
    fdprintf(fd, "Fast tracks: kMaxFastTracks=%u activeMask=%#x\n",
            FastMixerState::kMaxFastTracks, trackMask);
    fdprintf(fd, "Index Active Full Partial Empty  Recent Ready\n");
    for (uint32_t i = 0; i < FastMixerState::kMaxFastTracks; ++i, trackMask >>= 1) {
        bool isActive = trackMask & 1;
        const FastTrackDump *ftDump = &mTracks[i];
        const FastTrackUnderruns& underruns = ftDump->mUnderruns;
        const char *mostRecent;
        switch (underruns.mBitFields.mMostRecent) {
        case UNDERRUN_FULL:
            mostRecent = "full";
            break;
        case UNDERRUN_PARTIAL:
            mostRecent = "partial";
            break;
        case UNDERRUN_EMPTY:
            mostRecent = "empty";
            break;
        default:
            mostRecent = "?";
            break;
        }
        fdprintf(fd, "%5u %6s %4u %7u %5u %7s %5zu\n", i, isActive ? "yes" : "no",
                (underruns.mBitFields.mFull) & UNDERRUN_MASK,
                (underruns.mBitFields.mPartial) & UNDERRUN_MASK,
                (underruns.mBitFields.mEmpty) & UNDERRUN_MASK,
                mostRecent, ftDump->mFramesReady);
    }
}
}
