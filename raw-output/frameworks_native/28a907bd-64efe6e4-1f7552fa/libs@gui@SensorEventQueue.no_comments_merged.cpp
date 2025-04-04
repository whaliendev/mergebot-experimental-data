#define LOG_TAG "Sensors"
#include <stdint.h>
#include <sys/types.h>
#include <utils/Errors.h>
#include <utils/RefBase.h>
#include <utils/Looper.h>
#include <gui/Sensor.h>
#include <gui/BitTube.h>
#include <gui/SensorEventQueue.h>
#include <gui/ISensorEventConnection.h>
#include <android/sensor.h>
namespace android {
SensorEventQueue::SensorEventQueue(const sp<ISensorEventConnection>& connection)
    : mSensorEventConnection(connection)
{
}
SensorEventQueue::~SensorEventQueue()
{
}
void SensorEventQueue::onFirstRef()
{
    mSensorChannel = mSensorEventConnection->getSensorChannel();
}
int SensorEventQueue::getFd() const
{
    return mSensorChannel->getFd();
}
ssize_t SensorEventQueue::write(ASensorEvent const* events, size_t numEvents)
{
    ssize_t size = mSensorChannel->write(events, numEvents * sizeof(events[0]));
    if (size >= 0) {
        if (size % sizeof(events[0])) {
            return -EINVAL;
        }
        size /= sizeof(events[0]);
    }
    return size;
}
ssize_t SensorEventQueue::read(ASensorEvent* events, size_t numEvents)
{
    ssize_t size = mSensorChannel->read(events, numEvents*sizeof(events[0]));
    ALOGE_IF(size<0 && size!=-EAGAIN,
            "SensorChannel::read error (%s)", strerror(-size));
    if (size >= 0) {
        if (size % sizeof(events[0])) {
            ALOGE("SensorEventQueue partial read (event-size=%u, read=%d)",
                    sizeof(events[0]), int(size));
            return -EINVAL;
        }
        size /= sizeof(events[0]);
    }
    return size;
}
sp<Looper> SensorEventQueue::getLooper() const
{
    Mutex::Autolock _l(mLock);
    if (mLooper == 0) {
        mLooper = new Looper(true);
        mLooper->addFd(getFd(), getFd(), ALOOPER_EVENT_INPUT, NULL, NULL);
    }
    return mLooper;
}
status_t SensorEventQueue::waitForEvent() const
{
    const int fd = getFd();
    sp<Looper> looper(getLooper());
    int32_t result;
    do {
        result = looper->pollOnce(-1);
        if (result == ALOOPER_EVENT_ERROR) {
<<<<<<< HEAD
            ALOGE("SensorEventQueue::waitForEvent error (errno=%d)", errno);
||||||| 1f7552fadf
            LOGE("SensorChannel::waitForEvent error (errno=%d)", errno);
=======
            ALOGE("SensorChannel::waitForEvent error (errno=%d)", errno);
>>>>>>> 64efe6e4
            result = -EPIPE;
            break;
        }
    } while (result != fd);
    return (result == fd) ? status_t(NO_ERROR) : result;
}
status_t SensorEventQueue::wake() const
{
    sp<Looper> looper(getLooper());
    looper->wake();
    return NO_ERROR;
}
status_t SensorEventQueue::enableSensor(Sensor const* sensor) const {
    return mSensorEventConnection->enableDisable(sensor->getHandle(), true);
}
status_t SensorEventQueue::disableSensor(Sensor const* sensor) const {
    return mSensorEventConnection->enableDisable(sensor->getHandle(), false);
}
status_t SensorEventQueue::enableSensor(int32_t handle, int32_t us) const {
    status_t err = mSensorEventConnection->enableDisable(handle, true);
    if (err == NO_ERROR) {
        mSensorEventConnection->setEventRate(handle, us2ns(us));
    }
    return err;
}
status_t SensorEventQueue::disableSensor(int32_t handle) const {
    return mSensorEventConnection->enableDisable(handle, false);
}
status_t SensorEventQueue::setEventRate(Sensor const* sensor, nsecs_t ns) const {
    return mSensorEventConnection->setEventRate(sensor->getHandle(), ns);
}
};
