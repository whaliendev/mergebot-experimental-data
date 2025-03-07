#include "media/Twist.h"
#include <android-base/stringprintf.h>
#include "media/QuaternionUtil.h"
#include "QuaternionUtil.h"
namespace android {
namespace media {
Pose3f integrate(const Twist3f& twist, float dt) {
    Eigen::Vector3f translation = twist.translationalVelocity() * dt;
    Eigen::Vector3f rotationVector = twist.rotationalVelocity() * dt;
    return Pose3f(translation, rotationVectorToQuaternion(rotationVector));
}
Twist3f differentiate(const Pose3f& pose, float dt) {
    Eigen::Vector3f translationalVelocity = pose.translation() / dt;
    Eigen::Vector3f rotationalVelocity = quaternionToRotationVector(pose.rotation()) / dt;
    return Twist3f(translationalVelocity, rotationalVelocity);
}
std::ostream& operator<<(std::ostream& os, const Twist3f& twist) {
    os << "translation: " << twist.translationalVelocity().transpose()
       << " rotation vector: " << twist.rotationalVelocity().transpose();
    return os;
}
std::string Twist3f::toString() const {
    return base::StringPrintf("[%0.2f, %0.2f, %0.2f, %0.2f, %0.2f, %0.2f]",
                              mTranslationalVelocity[0], mTranslationalVelocity[1],
                              mTranslationalVelocity[2], mRotationalVelocity[0],
                              mRotationalVelocity[1], mRotationalVelocity[2]);
}
}
}
