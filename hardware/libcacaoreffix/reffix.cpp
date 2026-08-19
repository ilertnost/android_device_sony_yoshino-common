#define LOG_TAG "cacaoreffix"

#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <log/log.h>

namespace {

using RefFn = void (*)(const void*, const void*);

intptr_t adjustOffset(const void* p) {
    if (p == nullptr || (reinterpret_cast<uintptr_t>(p) & 0x3) != 0) {
        return 0;
    }
    const intptr_t* const* obj = reinterpret_cast<const intptr_t* const*>(p);
    const intptr_t* vptr = obj[0];
    if (vptr == nullptr || (reinterpret_cast<uintptr_t>(vptr) & 0x3) != 0) {
        return 0;
    }
    const intptr_t n = vptr[-3];
    if (n <= 0 || n > 512 || (n & 0x3) != 0) {
        return 0;
    }
    const uint8_t* candidate = reinterpret_cast<const uint8_t*>(p) + n;
    const intptr_t* vptr2 = *reinterpret_cast<const intptr_t* const*>(candidate);
    if (vptr2 == nullptr || (reinterpret_cast<uintptr_t>(vptr2) & 0x3) != 0) {
        return 0;
    }
    return (vptr2[-3] == -n) ? n : 0;
}

const void* adjust(const void* p, const char* who) {
    const intptr_t n = adjustOffset(p);
    if (n == 0) {
        return p;
    }
    const void* fixed = reinterpret_cast<const uint8_t*>(p) + n;
    ALOGI("%s: corrected this %p -> %p (+%ld, complete-object -> RefBase subobject)", who, p, fixed,
          (long)n);
    return fixed;
}

}

extern "C" {

void _ZNK7android7RefBase9incStrongEPKv(const void* thiz, const void* id) {
    static RefFn real = reinterpret_cast<RefFn>(dlsym(RTLD_NEXT, "_ZNK7android7RefBase9incStrongEPKv"));
    if (real == nullptr) {
        ALOGE("incStrong: real symbol not found");
        return;
    }
    real(adjust(thiz, "incStrong"), id);
}

using HeapCtor = void* (*)(void*, unsigned int, unsigned int, const char*);
using BaseCtor = void* (*)(void*, const void*, int, unsigned int);

void* _ZN7android14MemoryHeapBaseC1EjjPKc(void* thiz, unsigned int size, unsigned int flags,
                                          const char* name) {
    static HeapCtor real =
            reinterpret_cast<HeapCtor>(dlsym(RTLD_NEXT, "_ZN7android14MemoryHeapBaseC1EjjPKc"));
    if (real == nullptr) {
        ALOGE("MemoryHeapBase ctor: real symbol not found");
        return thiz;
    }
    void* r = real(thiz, size, flags, name);
    const uintptr_t* w = reinterpret_cast<const uintptr_t*>(thiz);
    ALOGI("MemoryHeapBase(%p, size=%u, flags=0x%x, name=%s)", thiz, size, flags,
          name ? name : "(null)");
    ALOGI("  heap words: %p %p %p %p %p %p %p %p %p %p %p %p %p", (void*)w[0], (void*)w[1],
          (void*)w[2], (void*)w[3], (void*)w[4], (void*)w[5], (void*)w[6], (void*)w[7],
          (void*)w[8], (void*)w[9], (void*)w[10], (void*)w[11], (void*)w[12]);

    if (access("/data/local/tmp/cacao_pin_heap", F_OK) == 0) {
        const void* refbase = adjust(thiz, "MemoryHeapBase-pin");
        static RefFn realInc =
                reinterpret_cast<RefFn>(dlsym(RTLD_NEXT, "_ZNK7android7RefBase9incStrongEPKv"));
        if (realInc != nullptr && refbase != thiz) {
            realInc(refbase, thiz);
            ALOGI("MemoryHeapBase: PINNED %p via RefBase %p", thiz, refbase);
        } else {
            ALOGW("MemoryHeapBase: pin skipped, no validated vbase offset for %p", thiz);
        }
    }
    return r;
}

void* _ZN7android10MemoryBaseC1ERKNS_2spINS_11IMemoryHeapEEEij(void* thiz, const void* heapSp,
                                                               int offset, unsigned int size) {
    static BaseCtor real = reinterpret_cast<BaseCtor>(
            dlsym(RTLD_NEXT, "_ZN7android10MemoryBaseC1ERKNS_2spINS_11IMemoryHeapEEEij"));
    if (real == nullptr) {
        ALOGE("MemoryBase ctor: real symbol not found");
        return thiz;
    }
    const void* heap = heapSp ? *reinterpret_cast<const void* const*>(heapSp) : nullptr;

    void* fixedSlot[1] = {nullptr};
    const void* useSp = heapSp;
    if (heap != nullptr && access("/data/local/tmp/cacao_no_heapfix", F_OK) != 0) {
        const uint8_t* candidate = reinterpret_cast<const uint8_t*>(heap) - 44;
        const intptr_t* cv = *reinterpret_cast<const intptr_t* const*>(candidate);
        if (cv != nullptr && cv[-3] == 44) {
            fixedSlot[0] = const_cast<void*>(reinterpret_cast<const void*>(candidate));
            useSp = fixedSlot;
            ALOGI("MemoryBase: heap ptr corrected %p -> %p (RefBase subobject -> complete object)",
                  heap, fixedSlot[0]);
        } else {
            ALOGW("MemoryBase: heap %p did not validate as complete+44, left alone", heap);
        }
    }
    void* r = real(thiz, useSp, offset, size);
    const uintptr_t* w = reinterpret_cast<const uintptr_t*>(thiz);
    const uintptr_t heapVptr = heap ? *reinterpret_cast<const uintptr_t*>(heap) : 0;

    if (access("/data/local/tmp/cacao_pin_base", F_OK) == 0) {
        const void* refbase = adjust(thiz, "MemoryBase-pin");
        static RefFn realInc =
                reinterpret_cast<RefFn>(dlsym(RTLD_NEXT, "_ZNK7android7RefBase9incStrongEPKv"));
        if (realInc != nullptr && refbase != thiz) {
            realInc(refbase, thiz);
            ALOGI("MemoryBase: PINNED %p via RefBase %p", thiz, refbase);
        } else {
            ALOGW("MemoryBase: pin skipped, no validated vbase offset for %p", thiz);
        }
    }
    ALOGI("MemoryBase(%p, heap=%p heapVptr=%p, off=%d, size=%u)  words: %p %p %p %p %p %p %p %p %p",
          thiz, heap, (void*)heapVptr, offset, size, (void*)w[0], (void*)w[1], (void*)w[2],
          (void*)w[3], (void*)w[4], (void*)w[5], (void*)w[6], (void*)w[7], (void*)w[8]);
    return r;
}

using GetMemFn = void* (*)(void*, const void*, int*, unsigned int*);

void* _ZNK7android10MemoryBase9getMemoryEPiPj(void* sret, const void* thiz, int* offset,
                                              unsigned int* size) {
    static GetMemFn real = reinterpret_cast<GetMemFn>(
            dlsym(RTLD_NEXT, "_ZNK7android10MemoryBase9getMemoryEPiPj"));
    const uintptr_t* w = reinterpret_cast<const uintptr_t*>(thiz);
    const uintptr_t heap = w ? w[6] : 0;
    const uintptr_t heapVptr =
            heap ? *reinterpret_cast<const uintptr_t*>(heap) : 0;
    Dl_info di0 = {};
    void* ra0 = __builtin_return_address(0);
    const char* who0 = (dladdr(ra0, &di0) && di0.dli_fname) ? di0.dli_fname : "?";
    ALOGI("getMemory: caller=%p (%s %s)", ra0, who0, di0.dli_sname ? di0.dli_sname : "");
    ALOGI("getMemory(this=%p) mHeap=%p heapVptr=%p  words: %p %p %p %p %p %p %p %p %p", thiz,
          (void*)heap, (void*)heapVptr, (void*)w[0], (void*)w[1], (void*)w[2], (void*)w[3],
          (void*)w[4], (void*)w[5], (void*)w[6], (void*)w[7], (void*)w[8]);
    if (real == nullptr) {
        ALOGE("getMemory: real symbol not found");
        return sret;
    }
    void* r = real(sret, thiz, offset, size);
    ALOGI("getMemory(this=%p) returned heap=%p", thiz,
          sret ? *reinterpret_cast<void**>(sret) : nullptr);
    return r;
}

using PtrFn = void* (*)(const void*);

void* _ZNK7android7IMemory15unsecurePointerEv(const void* thiz) {
    static PtrFn real = reinterpret_cast<PtrFn>(
            dlsym(RTLD_NEXT, "_ZNK7android7IMemory15unsecurePointerEv"));
    if (real == nullptr) {
        ALOGE("unsecurePointer: real symbol not found");
        return nullptr;
    }
    void* p = real(thiz);
    const uintptr_t* w = reinterpret_cast<const uintptr_t*>(thiz);
    ALOGI("unsecurePointer(this=%p) -> %p   (mHeap=%p)", thiz, p, w ? (void*)w[6] : nullptr);
    return p;
}

void* _ZNK7android14MemoryHeapBase7getBaseEv(const void* thiz) {
    static PtrFn real = reinterpret_cast<PtrFn>(
            dlsym(RTLD_NEXT, "_ZNK7android14MemoryHeapBase7getBaseEv"));
    if (real == nullptr) {
        ALOGE("getBase: real symbol not found");
        return nullptr;
    }
    void* p = real(thiz);
    ALOGI("MemoryHeapBase::getBase(this=%p) -> %p", thiz, p);
    return p;
}

void _ZNK7android7RefBase9decStrongEPKv(const void* thiz, const void* id) {
    static RefFn real = reinterpret_cast<RefFn>(dlsym(RTLD_NEXT, "_ZNK7android7RefBase9decStrongEPKv"));
    if (real == nullptr) {
        ALOGE("decStrong: real symbol not found");
        return;
    }
    real(adjust(thiz, "decStrong"), id);
}

}
