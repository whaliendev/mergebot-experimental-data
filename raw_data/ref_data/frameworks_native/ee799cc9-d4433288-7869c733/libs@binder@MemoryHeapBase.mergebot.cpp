/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "MemoryHeapBase"
#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <binder/MemoryHeapBase.h>
#include <cutils/ashmem.h>
#include <cutils/atomic.h>
#include <log/log.h>

namespace android {

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
#ifdef __BIONIC__
static int memfd_create_region(const char* name, size_t size) {
    int fd = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd == -1) {
        ALOGE("%s: memfd_create(%s, %zd) failed: %s\n", __func__, name, size, strerror(errno));
        return -1;
    }

    if (ftruncate(fd, size) == -1) {
        ALOGE("%s, ftruncate(%s, %zd) failed for memfd creation: %s\n", __func__, name, size,
              strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}
#endif

MemoryHeapBase::MemoryHeapBase()
      : mFD(-1), mSize(0), mBase(MAP_FAILED), mDevice(nullptr), mNeedUnmap(false), mOffset(0) {}

MemoryHeapBase::MemoryHeapBase(size_t size, uint32_t flags, char const* name)
      : mFD(-1),
        mSize(0),
        mBase(MAP_FAILED),
        mFlags(flags),
        mDevice(nullptr),
        mNeedUnmap(false),
        mOffset(0) {
    const size_t pagesize = getpagesize();
    size = ((size + pagesize - 1) & ~(pagesize - 1));
    int fd = -1;
    if (mFlags & FORCE_MEMFD) {
#ifdef __BIONIC__
        ALOGV("MemoryHeapBase: Attempting to force MemFD");
        fd = memfd_create_region(name ? name : "MemoryHeapBase", size);
        if (fd < 0 || (mapfd(fd, true, size) != NO_ERROR)) return;
<<<<<<< HEAD
        const int SEAL_FLAGS = ((mFlags & READ_ONLY) ? F_SEAL_FUTURE_WRITE : 0) | F_SEAL_GROW |
                F_SEAL_SHRINK | ((mFlags & MEMFD_ALLOW_SEALING_FLAG) ? 0 : F_SEAL_SEAL);
|||||||
        const int SEAL_FLAGS = ((mFlags & READ_ONLY) ? F_SEAL_FUTURE_WRITE : 0) |
                ((mFlags & MEMFD_ALLOW_SEALING) ? 0 : F_SEAL_SEAL);
=======
        const int SEAL_FLAGS = ((mFlags & READ_ONLY) ? F_SEAL_FUTURE_WRITE : 0) | F_SEAL_GROW |
                F_SEAL_SHRINK | ((mFlags & MEMFD_ALLOW_SEALING) ? 0 : F_SEAL_SEAL);
>>>>>>> 99785d0473407e38e18084e2bbf040d12e57beee
        if (SEAL_FLAGS && (fcntl(fd, F_ADD_SEALS, SEAL_FLAGS) == -1)) {
            ALOGE("MemoryHeapBase: MemFD %s sealing with flags %x failed with error  %s", name,
                  SEAL_FLAGS, strerror(errno));
            munmap(mBase, mSize);
            mBase = nullptr;
            mSize = 0;
            close(fd);
        }
        return;
#else
        mFlags &= ~(FORCE_MEMFD | MEMFD_ALLOW_SEALING_FLAG);
#endif
    }
    fd = ashmem_create_region(name ? name : "MemoryHeapBase", size);
    ALOGE_IF(fd < 0, "MemoryHeapBase: error creating ashmem region: %s", strerror(errno));
    if (fd < 0 || (mapfd(fd, true, size) != NO_ERROR)) return;
    if (mFlags & READ_ONLY) {
        ashmem_set_prot_region(fd, PROT_READ);
    }
}

MemoryHeapBase::MemoryHeapBase(const char* device, size_t size, uint32_t flags)
      : mFD(-1),
        mSize(0),
        mBase(MAP_FAILED),
        mFlags(flags),
        mDevice(nullptr),
        mNeedUnmap(false),
        mOffset(0) {
    if (flags & (FORCE_MEMFD | MEMFD_ALLOW_SEALING_FLAG)) {
        LOG_ALWAYS_FATAL("FORCE_MEMFD, MEMFD_ALLOW_SEALING only valid with creating constructor");
    }
    int open_flags = O_RDWR;
    if (flags & NO_CACHING) open_flags |= O_SYNC;

    int fd = open(device, open_flags);
    ALOGE_IF(fd < 0, "error opening %s: %s", device, strerror(errno));
    if (fd >= 0) {
        const size_t pagesize = getpagesize();
        size = ((size + pagesize - 1) & ~(pagesize - 1));
        if (mapfd(fd, false, size) == NO_ERROR) {
            mDevice = device;
        }
    }
}

MemoryHeapBase::MemoryHeapBase(int fd, size_t size, uint32_t flags, off_t offset)
      : mFD(-1),
        mSize(0),
        mBase(MAP_FAILED),
        mFlags(flags),
        mDevice(nullptr),
        mNeedUnmap(false),
        mOffset(0) {
    if (flags & (FORCE_MEMFD | MEMFD_ALLOW_SEALING_FLAG)) {
        LOG_ALWAYS_FATAL("FORCE_MEMFD, MEMFD_ALLOW_SEALING only valid with creating constructor");
    }
    const size_t pagesize = getpagesize();
    size = ((size + pagesize - 1) & ~(pagesize - 1));
    mapfd(fcntl(fd, F_DUPFD_CLOEXEC, 0), false, size, offset);
}

status_t MemoryHeapBase::init(int fd, void* base, size_t size, int flags, const char* device) {
    if (mFD != -1) {
        return INVALID_OPERATION;
    }
    mFD = fd;
    mBase = base;
    mSize = size;
    mFlags = flags;
    mDevice = device;
    return NO_ERROR;
}

status_t MemoryHeapBase::mapfd(int fd, bool writeableByCaller, size_t size, off_t offset) {
    if (size == 0) {
        // try to figure out the size automatically
        struct stat sb;
        if (fstat(fd, &sb) == 0) {
            size = (size_t)sb.st_size;
            // sb.st_size is off_t which on ILP32 may be 64 bits while size_t is 32 bits.
            if ((off_t)size != sb.st_size) {
                ALOGE("%s: size of file %lld cannot fit in memory", __func__,
                      (long long)sb.st_size);
                return INVALID_OPERATION;
            }
        }
        // if it didn't work, let mmap() fail.
    }

    if ((mFlags & DONT_MAP_LOCALLY) == 0) {
        int prot = PROT_READ;
        if (writeableByCaller || (mFlags & READ_ONLY) == 0) {
            prot |= PROT_WRITE;
        }
        void* base = (uint8_t*)mmap(nullptr, size, prot, MAP_SHARED, fd, offset);
        if (base == MAP_FAILED) {
            ALOGE("mmap(fd=%d, size=%zu) failed (%s)", fd, size, strerror(errno));
            close(fd);
            return -errno;
        }
        // ALOGD("mmap(fd=%d, base=%p, size=%zu)", fd, base, size);
        mBase = base;
        mNeedUnmap = true;
    } else {
        mBase = nullptr; // not MAP_FAILED
        mNeedUnmap = false;
    }
    mFD = fd;
    mSize = size;
    mOffset = offset;
    return NO_ERROR;
}

MemoryHeapBase::~MemoryHeapBase() {
    dispose();
}

void MemoryHeapBase::dispose() {
    int fd = android_atomic_or(-1, &mFD);
    if (fd >= 0) {
        if (mNeedUnmap) {
            // ALOGD("munmap(fd=%d, base=%p, size=%zu)", fd, mBase, mSize);
            munmap(mBase, mSize);
        }
        mBase = nullptr;
        mSize = 0;
        close(fd);
    }
}

int MemoryHeapBase::getHeapID() const {
    return mFD;
}

void* MemoryHeapBase::getBase() const {
    return mBase;
}

size_t MemoryHeapBase::getSize() const {
    return mSize;
}

uint32_t MemoryHeapBase::getFlags() const {
    return mFlags;
}

const char* MemoryHeapBase::getDevice() const {
    return mDevice;
}

off_t MemoryHeapBase::getOffset() const {
    return mOffset;
}

} // namespace android