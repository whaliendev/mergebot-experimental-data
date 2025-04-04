#undef LOG_TAG
#define LOG_TAG "ColorLayer"
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <renderengine/RenderEngine.h>
#include <ui/GraphicBuffer.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include "ColorLayer.h"
#include "DisplayDevice.h"
#include "RenderEngine/RenderEngine.h"
#include "SurfaceFlinger.h"
namespace android {
ColorLayer::ColorLayer(SurfaceFlinger* flinger, const sp<Client>& client, const String8& name,
                       uint32_t w, uint32_t h, uint32_t flags)
      : Layer(flinger, client, name, w, h, flags) {
    mDrawingState = mCurrentState;
}
void ColorLayer::onDraw(const RenderArea& renderArea, const Region& ,
                        bool useIdentityTransform) {
    half4 color = getColor();
    if (color.a > 0) {
        renderengine::Mesh mesh(renderengine::Mesh::TRIANGLE_FAN, 4, 2);
        computeGeometry(renderArea, mesh, useIdentityTransform);
        auto& engine(mFlinger->getRenderEngine());
        engine.setupLayerBlending(getPremultipledAlpha(), false ,
                                  true , color);
        engine.setSourceDataSpace(mCurrentDataSpace);
        engine.drawMesh(mesh);
        engine.disableBlending();
    }
}
bool ColorLayer::isVisible() const {
    return !isHiddenByPolicy() && getAlpha() > 0.0f;
}
void ColorLayer::setPerFrameData(const sp<const DisplayDevice>& display) {
    const ui::Transform& tr = display->getTransform();
    const auto& viewport = display->getViewport();
    Region visible = tr.transform(visibleRegion.intersect(viewport));
<<<<<<< HEAD
    const auto displayId = display->getId();
    getBE().compositionInfo.hwc.visibleRegion = visible;
    getBE().compositionInfo.hwc.dataspace = mCurrentDataSpace;
||||||| d1030f6a7d
    auto hwcId = displayDevice->getHwcDisplayId();
    auto& hwcInfo = getBE().mHwcLayers[hwcId];
    auto& hwcLayer = hwcInfo.layer;
    auto error = hwcLayer->setVisibleRegion(visible);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set visible region: %s (%d)", mName.string(),
              to_string(error).c_str(), static_cast<int32_t>(error));
        visible.dump(LOG_TAG);
    }
=======
    auto hwcId = displayDevice->getHwcDisplayId();
    if (!hasHwcLayer(hwcId)) {
        return;
    }
    auto& hwcInfo = getBE().mHwcLayers[hwcId];
    auto& hwcLayer = hwcInfo.layer;
    auto error = hwcLayer->setVisibleRegion(visible);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set visible region: %s (%d)", mName.string(),
              to_string(error).c_str(), static_cast<int32_t>(error));
        visible.dump(LOG_TAG);
    }
>>>>>>> 8acaef00
    setCompositionType(displayId, HWC2::Composition::SolidColor);
    getBE().compositionInfo.hwc.dataspace = mCurrentDataSpace;
    half4 color = getColor();
    getBE().compositionInfo.hwc.color = { static_cast<uint8_t>(std::round(255.0f * color.r)),
                                      static_cast<uint8_t>(std::round(255.0f * color.g)),
                                      static_cast<uint8_t>(std::round(255.0f * color.b)), 255 };
    getBE().compositionInfo.hwc.transform = HWC2::Transform::None;
}
}
