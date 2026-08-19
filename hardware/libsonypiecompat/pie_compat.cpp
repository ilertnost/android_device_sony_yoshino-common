#include <stdint.h>

#include <new>

#include <binder/Parcel.h>
#include <gui/Surface.h>
#include <utils/Errors.h>
#include <utils/StrongPointer.h>

extern "C" __attribute__((visibility("default")))
void _ZN7android7SurfaceC1ERKNS_2spINS_22IGraphicBufferProducerEEEb(
        void* thiz, const android::sp<android::IGraphicBufferProducer>& bufferProducer,
        bool controlledByApp) {
    new (thiz) android::Surface(bufferProducer, controlledByApp,
                                android::sp<android::IBinder>());
}

extern "C" __attribute__((visibility("default")))
intptr_t _ZNK7android6Parcel10readIntPtrEv(const android::Parcel* thiz) {
    int64_t value = 0;
    if (thiz->readInt64(&value) != android::OK) {
        return 0;
    }
    return static_cast<intptr_t>(value);
}
