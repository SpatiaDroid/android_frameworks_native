/*
 * Copyright 2019 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#undef LOG_TAG
#define LOG_TAG "RegionSamplingThread"

#include "RegionSamplingThread.h"

#include <gui/IRegionSamplingListener.h>
#include <utils/Trace.h>

#include "DisplayDevice.h"
#include "Layer.h"
#include "SurfaceFlinger.h"

namespace android {

template <typename T>
struct SpHash {
    size_t operator()(const sp<T>& p) const { return std::hash<T*>()(p.get()); }
};

RegionSamplingThread::RegionSamplingThread(SurfaceFlinger& flinger) : mFlinger(flinger) {
    std::lock_guard threadLock(mThreadMutex);
    mThread = std::thread([this]() { threadMain(); });
    pthread_setname_np(mThread.native_handle(), "RegionSamplingThread");
}

RegionSamplingThread::~RegionSamplingThread() {
    {
        std::lock_guard lock(mMutex);
        mRunning = false;
        mCondition.notify_one();
    }

    std::lock_guard threadLock(mThreadMutex);
    if (mThread.joinable()) {
        mThread.join();
    }
}

void RegionSamplingThread::addListener(const Rect& samplingArea, const sp<IBinder>& stopLayerHandle,
                                       const sp<IRegionSamplingListener>& listener) {
    wp<Layer> stopLayer = stopLayerHandle != nullptr
            ? static_cast<Layer::Handle*>(stopLayerHandle.get())->owner
            : nullptr;

    sp<IBinder> asBinder = IInterface::asBinder(listener);
    asBinder->linkToDeath(this);
    std::lock_guard lock(mMutex);
    mDescriptors.emplace(wp<IBinder>(asBinder), Descriptor{samplingArea, stopLayer, listener});
}

void RegionSamplingThread::removeListener(const sp<IRegionSamplingListener>& listener) {
    std::lock_guard lock(mMutex);
    mDescriptors.erase(wp<IBinder>(IInterface::asBinder(listener)));
}

void RegionSamplingThread::sampleNow() {
    std::lock_guard lock(mMutex);
    mSampleRequested = true;
    mCondition.notify_one();
}

void RegionSamplingThread::binderDied(const wp<IBinder>& who) {
    std::lock_guard lock(mMutex);
    mDescriptors.erase(who);
}

namespace {
// Using Rec. 709 primaries
float getLuma(float r, float g, float b) {
    constexpr auto rec709_red_primary = 0.2126f;
    constexpr auto rec709_green_primary = 0.7152f;
    constexpr auto rec709_blue_primary = 0.0722f;
    return rec709_red_primary * r + rec709_green_primary * g + rec709_blue_primary * b;
}

float sampleArea(const uint32_t* data, int32_t stride, const Rect& area) {
    std::array<int32_t, 256> brightnessBuckets = {};
    const int32_t majoritySampleNum = area.getWidth() * area.getHeight() / 2;

    for (int32_t row = area.top; row < area.bottom; ++row) {
        const uint32_t* rowBase = data + row * stride;
        for (int32_t column = area.left; column < area.right; ++column) {
            uint32_t pixel = rowBase[column];
            const float r = (pixel & 0xFF) / 255.0f;
            const float g = ((pixel >> 8) & 0xFF) / 255.0f;
            const float b = ((pixel >> 16) & 0xFF) / 255.0f;
            const uint8_t luma = std::round(getLuma(r, g, b) * 255.0f);
            ++brightnessBuckets[luma];
            if (brightnessBuckets[luma] > majoritySampleNum) return luma / 255.0f;
        }
    }

    int32_t accumulated = 0;
    size_t bucket = 0;
    while (bucket++ < brightnessBuckets.size()) {
        accumulated += brightnessBuckets[bucket];
        if (accumulated > majoritySampleNum) break;
    }

    return bucket / 255.0f;
}
} // anonymous namespace

std::vector<float> sampleBuffer(const sp<GraphicBuffer>& buffer, const Point& leftTop,
                                const std::vector<RegionSamplingThread::Descriptor>& descriptors) {
    void* data_raw = nullptr;
    buffer->lock(GRALLOC_USAGE_SW_READ_OFTEN, &data_raw);
    std::shared_ptr<uint32_t> data(reinterpret_cast<uint32_t*>(data_raw),
                                   [&buffer](auto) { buffer->unlock(); });
    if (!data) return {};

    const int32_t stride = buffer->getStride();
    std::vector<float> lumas(descriptors.size());
    std::transform(descriptors.begin(), descriptors.end(), lumas.begin(),
                   [&](auto const& descriptor) {
                       return sampleArea(data.get(), stride, descriptor.area - leftTop);
                   });
    return lumas;
}

void RegionSamplingThread::captureSample() {
    ATRACE_CALL();

    if (mDescriptors.empty()) {
        return;
    }

    std::vector<RegionSamplingThread::Descriptor> descriptors;
    Region sampleRegion;
    for (const auto& [listener, descriptor] : mDescriptors) {
        sampleRegion.orSelf(descriptor.area);
        descriptors.emplace_back(descriptor);
    }

    Rect sampledArea = sampleRegion.bounds();

    sp<const DisplayDevice> device = mFlinger.getDefaultDisplayDevice();
    DisplayRenderArea renderArea(device, sampledArea, sampledArea.getWidth(),
                                 sampledArea.getHeight(), ui::Dataspace::V0_SRGB,
                                 ui::Transform::ROT_0);

    std::unordered_set<sp<IRegionSamplingListener>, SpHash<IRegionSamplingListener>> listeners;

    auto traverseLayers = [&](const LayerVector::Visitor& visitor) {
        bool stopLayerFound = false;
        auto filterVisitor = [&](Layer* layer) {
            // We don't want to capture any layers beyond the stop layer
            if (stopLayerFound) return;

            // Likewise if we just found a stop layer, set the flag and abort
            for (const auto& [area, stopLayer, listener] : descriptors) {
                if (layer == stopLayer.promote().get()) {
                    stopLayerFound = true;
                    return;
                }
            }

            // Compute the layer's position on the screen
            Rect bounds = Rect(layer->getBounds());
            ui::Transform transform = layer->getTransform();
            constexpr bool roundOutwards = true;
            Rect transformed = transform.transform(bounds, roundOutwards);

            // If this layer doesn't intersect with the larger sampledArea, skip capturing it
            Rect ignore;
            if (!transformed.intersect(sampledArea, &ignore)) return;

            // If the layer doesn't intersect a sampling area, skip capturing it
            bool intersectsAnyArea = false;
            for (const auto& [area, stopLayer, listener] : descriptors) {
                if (transformed.intersect(area, &ignore)) {
                    intersectsAnyArea = true;
                    listeners.insert(listener);
                }
            }
            if (!intersectsAnyArea) return;

            ALOGV("Traversing [%s] [%d, %d, %d, %d]", layer->getName().string(), bounds.left,
                  bounds.top, bounds.right, bounds.bottom);
            visitor(layer);
        };
        mFlinger.traverseLayersInDisplay(device, filterVisitor);
    };

    const uint32_t usage = GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_HW_RENDER;
    sp<GraphicBuffer> buffer =
            new GraphicBuffer(sampledArea.getWidth(), sampledArea.getHeight(),
                              PIXEL_FORMAT_RGBA_8888, 1, usage, "RegionSamplingThread");

    // When calling into SF, we post a message into the SF message queue (so the
    // screen capture runs on the main thread). This message blocks until the
    // screenshot is actually captured, but before the capture occurs, the main
    // thread may perform a normal refresh cycle. At the end of this cycle, it
    // can request another sample (because layers changed), which triggers a
    // call into sampleNow. When sampleNow attempts to grab the mutex, we can
    // deadlock.
    //
    // To avoid this, we drop the mutex while we call into SF.
    mMutex.unlock();
    mFlinger.captureScreenCore(renderArea, traverseLayers, buffer, false);
    mMutex.lock();

    std::vector<Descriptor> activeDescriptors;
    for (const auto& descriptor : descriptors) {
        if (listeners.count(descriptor.listener) != 0) {
            activeDescriptors.emplace_back(descriptor);
        }
    }

    ALOGV("Sampling %zu descriptors", activeDescriptors.size());
    std::vector<float> lumas = sampleBuffer(buffer, sampledArea.leftTop(), activeDescriptors);

    if (lumas.size() != activeDescriptors.size()) {
        return;
    }

    for (size_t d = 0; d < activeDescriptors.size(); ++d) {
        activeDescriptors[d].listener->onSampleCollected(lumas[d]);
    }
}

void RegionSamplingThread::threadMain() {
    std::lock_guard lock(mMutex);
    while (mRunning) {
        if (mSampleRequested) {
            mSampleRequested = false;
            captureSample();
        }
        mCondition.wait(mMutex,
                        [this]() REQUIRES(mMutex) { return mSampleRequested || !mRunning; });
    }
}

} // namespace android