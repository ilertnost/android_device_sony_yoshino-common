#define LOG_TAG "libsensorfix"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include <binder/ProcessState.h>
#include <log/log.h>
#include <utils/RefBase.h>

#include <android/frameworks/sensorservice/1.0/IEventQueue.h>
#include <android/frameworks/sensorservice/1.0/IEventQueueCallback.h>
#include <android/frameworks/sensorservice/1.0/ISensorManager.h>
#include <android/hardware/sensors/1.0/types.h>

using android::RefBase;
using android::sp;
using android::wp;
using android::frameworks::sensorservice::V1_0::IEventQueue;
using android::frameworks::sensorservice::V1_0::IEventQueueCallback;
using android::frameworks::sensorservice::V1_0::ISensorManager;
using android::hardware::sensors::V1_0::Event;
using android::hardware::sensors::V1_0::EventPayload;
using android::hardware::sensors::V1_0::SensorInfo;
using android::hardware::sensors::V1_0::SensorType;
using android::frameworks::sensorservice::V1_0::Result;
using android::hardware::Return;
using android::hardware::Void;

struct ASensorManager;
struct ASensor { int32_t handle; int32_t type; };
struct ASensorEventQueue;
struct ALooper;

constexpr int kAlooperEventInput = 1;
using ALooperCallbackFunc = int (*)(int fd, int events, void* data);

struct ASensorEvent {
    int32_t version;
    int32_t sensor;
    int32_t type;
    int32_t reserved0;
    int64_t timestamp;
    union {
        float data[16];
        struct { float x, y, z; } accel;
    };
    uint32_t flags;
    int32_t reserved1[3];
};
static_assert(sizeof(ASensorEvent) == 104, "ASensorEvent layout mismatch");

namespace {
constexpr const char* kAidlSensorServiceName =
        "android.frameworks.sensorservice.ISensorManager/default";
bool gPinnedToBinder = false;

using AServiceManagerFn = void* (*)(const char*);

void* callRealAidlSm(const char* sym, const char* instance) {
    AServiceManagerFn fn = reinterpret_cast<AServiceManagerFn>(dlsym(RTLD_NEXT, sym));
    return fn ? fn(instance) : nullptr;
}

inline bool isAidlSensorService(const char* instance) {
    return instance != nullptr && strcmp(instance, kAidlSensorServiceName) == 0;
}

__attribute__((constructor(101))) void pinBinderDriver() {
    if (android::ProcessState::selfOrNull() != nullptr) {
        ALOGW("ProcessState already initialised; leaving driver alone "
              "(HIDL bridge will still provide sensors)");
        return;
    }
    new android::sp<android::ProcessState>(android::ProcessState::initWithDriver("/dev/binder"));
    gPinnedToBinder = true;
    ALOGI("pinned ProcessState to /dev/binder (vendor default is /dev/vndbinder) "
          "[diagnostic; EIS now rides HIDL]");
}

}

extern "C" {

#pragma GCC visibility push(default)

void* AServiceManager_waitForService(const char* instance) {
    if (isAidlSensorService(instance)) {
        ALOGI("AIDL waitForService(%s) -> NULL (served by HIDL bridge; "
              "AIDL path has BAD_TYPE in this process)", instance);
        return nullptr;
    }
    return callRealAidlSm(__func__, instance);
}
void* AServiceManager_getService(const char* instance) {
    if (isAidlSensorService(instance)) return nullptr;
    return callRealAidlSm(__func__, instance);
}
void* AServiceManager_checkService(const char* instance) {
    if (isAidlSensorService(instance)) return nullptr;
    return callRealAidlSm(__func__, instance);
}

#pragma GCC visibility pop

}

namespace {

struct ManagerState : public RefBase {
    sp<ISensorManager> mgr;
    std::mutex cache_mtx;
    std::map<int, ASensor*> cache;
};

ManagerState* g_manager = nullptr;
std::once_flag g_managerOnce;

struct QueueState;

std::mutex g_queues_mtx;
std::map<void*, sp<QueueState>> g_queues;

struct QueueState : public RefBase {
    sp<IEventQueue> queue;
    int read_fd = -1;
    int write_fd = -1;
    int last_handle = -1;
    ALooperCallbackFunc user_cb = nullptr;
    void* user_data = nullptr;
    std::atomic<bool> stopping{false};
    std::mutex fd_mtx;
    std::thread poller;
};

struct SensorCallback : public IEventQueueCallback {
    wp<QueueState> q_;
    explicit SensorCallback(const sp<QueueState>& q) : q_(q) {}

    Return<void> onEvent(const Event& e) override {
        sp<QueueState> qs = q_.promote();
        if (qs == nullptr) return Void();
        if (qs->stopping.load(std::memory_order_acquire)) return Void();

        static std::atomic<int64_t> g_seen_type1{0};
        static std::atomic<int64_t> g_seen_type4{0};
        int32_t t = static_cast<int32_t>(e.sensorType);
        int64_t evt_n;
        if (t == 1)      evt_n = ++g_seen_type1;
        else if (t == 4) evt_n = ++g_seen_type4;
        else             evt_n = 0;
        if (evt_n == 1 || (evt_n > 0 && (evt_n % 200) == 0)) {
            ALOGI("onEvent type=%d handle=0x%x ts=%lld (n=%lld)",
                  t, e.sensorHandle,
                  static_cast<long long>(e.timestamp),
                  static_cast<long long>(evt_n));
        }

        ASensorEvent ae;
        memset(&ae, 0, sizeof(ae));
        ae.sensor = e.sensorHandle;
        ae.type = static_cast<int32_t>(e.sensorType);
        ae.timestamp = e.timestamp;
        ae.data[0] = e.u.vec3.x;
        ae.data[1] = e.u.vec3.y;
        ae.data[2] = e.u.vec3.z;
        ae.data[3] = static_cast<float>(e.u.vec3.status);

        std::lock_guard<std::mutex> l(qs->fd_mtx);
        if (qs->write_fd < 0) return Void();
        ssize_t n;
        do {
            n = ::send(qs->write_fd, &ae, sizeof(ae), MSG_NOSIGNAL | MSG_DONTWAIT);
        } while (n < 0 && errno == EINTR);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            ALOGW("sensor event socket write failed: %s", strerror(errno));
        }
        return Void();
    }
};

void pollLoop(QueueState* qs) {
    while (!qs->stopping.load(std::memory_order_acquire)) {
        struct pollfd pfd;
        pfd.fd = qs->read_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int rc = ::poll(&pfd, 1, 500);
        if (rc < 0) {
            if (errno == EINTR) continue;
            ALOGW("sensor poller poll() failed: %s", strerror(errno));
            break;
        }
        if (rc == 0) continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (pfd.revents & POLLIN) {
            if (qs->user_cb != nullptr) {
                qs->user_cb(qs->read_fd, kAlooperEventInput, qs->user_data);
            } else {
                char buf[sizeof(ASensorEvent) * 4];
                while (::read(qs->read_fd, buf, sizeof(buf)) > 0) { }
            }
        }
    }
}

}

extern "C" {

#pragma GCC visibility push(default)

ASensorManager* ASensorManager_getInstanceForPackage(const char* package) {
    std::call_once(g_managerOnce, [&]() {
        sp<ISensorManager> mgr = ISensorManager::tryGetService("default", false);
        if (mgr == nullptr) {
            ALOGE("HIDL ISensorManager::tryGetService(\"default\") returned null -- "
                  "EIS unavailable (is sensorservice running?)");
            return;
        }
        g_manager = new ManagerState;
        g_manager->mgr = mgr;
        ALOGI("HIDL ISensorManager acquired (package='%s'); ASensor bridge ACTIVE",
              package ? package : "(null)");
    });
    return reinterpret_cast<ASensorManager*>(g_manager);
}

ASensor const* ASensorManager_getDefaultSensor(ASensorManager* m, int type) {
    ManagerState* s = reinterpret_cast<ManagerState*>(m);
    if (s == nullptr || s->mgr == nullptr) return nullptr;

    {
        std::lock_guard<std::mutex> l(s->cache_mtx);
        auto it = s->cache.find(type);
        if (it != s->cache.end()) return it->second;
    }

    SensorInfo info{};
    Result result = Result::INVALID_OPERATION;
    auto cb = [&](const SensorInfo& si, Result r) {
        info = si;
        result = r;
    };
    s->mgr->getDefaultSensor(static_cast<SensorType>(type), cb);
    if (result != Result::OK) {
        ALOGE("getDefaultSensor(type=%d) failed: result=%d", type, static_cast<int>(result));
        return nullptr;
    }

    ASensor* a = new ASensor{info.sensorHandle, type};
    {
        std::lock_guard<std::mutex> l(s->cache_mtx);
        s->cache[type] = a;
    }
    ALOGI("getDefaultSensor(type=%d) -> handle=0x%x", type, info.sensorHandle);
    return a;
}

ASensorEventQueue* ASensorManager_createEventQueue(
        ASensorManager* m, ALooper* , int ,
        ALooperCallbackFunc cb, void* data) {
    ManagerState* s = reinterpret_cast<ManagerState*>(m);
    if (s == nullptr || s->mgr == nullptr) {
        ALOGE("createEventQueue: no manager");
        return nullptr;
    }

    sp<QueueState> qs = new QueueState;
    qs->user_cb = cb;
    qs->user_data = data;

    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) != 0) {
        ALOGE("socketpair: %s", strerror(errno));
        return nullptr;
    }
    qs->read_fd = fds[0];
    qs->write_fd = fds[1];

    sp<SensorCallback> cb_obj = new SensorCallback(qs);
    Result result = Result::INVALID_OPERATION;
    auto hicb = [&](const sp<IEventQueue>& q, Result r) {
        qs->queue = q;
        result = r;
    };
    s->mgr->createEventQueue(cb_obj, hicb);
    if (result != Result::OK || qs->queue == nullptr) {
        ALOGE("createEventQueue failed: result=%d", static_cast<int>(result));
        ::close(qs->read_fd);
        ::close(qs->write_fd);
        return nullptr;
    }

    QueueState* qs_raw = qs.get();
    qs->poller = std::thread([qs_raw]() { pollLoop(qs_raw); });

    {
        std::lock_guard<std::mutex> l(g_queues_mtx);
        g_queues[qs_raw] = qs;
    }

    ALOGI("createEventQueue: HIDL IEventQueue acquired, poller started");
    return reinterpret_cast<ASensorEventQueue*>(qs_raw);
}

void ASensorManager_destroyEventQueue(ASensorManager* , ASensorEventQueue* q) {
    if (q == nullptr) return;
    sp<QueueState> qs;
    {
        std::lock_guard<std::mutex> l(g_queues_mtx);
        auto it = g_queues.find(q);
        if (it == g_queues.end()) return;
        qs = it->second;
        g_queues.erase(it);
    }
    if (qs == nullptr) return;

    qs->stopping.store(true, std::memory_order_release);

    if (qs->queue != nullptr && qs->last_handle >= 0) {
        qs->queue->disableSensor(qs->last_handle);
    }

    {
        std::lock_guard<std::mutex> l(qs->fd_mtx);
        if (qs->write_fd >= 0) { ::close(qs->write_fd); qs->write_fd = -1; }
    }
    if (qs->poller.joinable()) qs->poller.join();
    if (qs->read_fd >= 0) { ::close(qs->read_fd); qs->read_fd = -1; }

    qs->queue.clear();
    ALOGI("destroyEventQueue: queue torn down");
}

int ASensorEventQueue_setEventRate(ASensorEventQueue* q, ASensor const* sensor, int32_t usec) {
    QueueState* qs = reinterpret_cast<QueueState*>(q);
    if (qs == nullptr || qs->queue == nullptr || sensor == nullptr) return -EINVAL;
    qs->last_handle = sensor->handle;
    Return<Result> r = qs->queue->enableSensor(sensor->handle, usec, 0LL);
    if (!r.isOk()) {
        ALOGE("enableSensor(h=%d,rate=%dus) transport error: %s",
              sensor->handle, usec, r.description().c_str());
        return -EIO;
    }
    Result rr = static_cast<Result>(r);
    if (rr != Result::OK) {
        ALOGE("enableSensor(h=%d,rate=%dus) result=%d",
              sensor->handle, usec, static_cast<int>(rr));
        return -EINVAL;
    }
    ALOGI("enableSensor(h=0x%x type=%d rate=%dus) OK", sensor->handle, sensor->type, usec);
    return 0;
}

ssize_t ASensorEventQueue_getEvents(ASensorEventQueue* q, ASensorEvent* events, size_t count) {
    QueueState* qs = reinterpret_cast<QueueState*>(q);
    if (qs == nullptr || qs->read_fd < 0 || events == nullptr || count == 0) return -EINVAL;
    ssize_t got = 0;
    for (size_t i = 0; i < count; i++) {
        ssize_t r;
        do {
            r = ::read(qs->read_fd, &events[i], sizeof(ASensorEvent));
        } while (r < 0 && errno == EINTR);
        if (r == static_cast<ssize_t>(sizeof(ASensorEvent))) {
            got++;
        } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else if (r == 0) {
            break;
        } else {
            ALOGW("getEvents short/err read: rc=%zd errno=%d (%s)",
                  r, errno, strerror(errno));
            break;
        }
    }
    return got;
}

int ASensorEventQueue_disableSensor(ASensorEventQueue* q, ASensor const* sensor) {
    QueueState* qs = reinterpret_cast<QueueState*>(q);
    if (qs == nullptr || qs->queue == nullptr || sensor == nullptr) return -EINVAL;
    Return<Result> r = qs->queue->disableSensor(sensor->handle);
    if (!r.isOk()) {
        ALOGE("disableSensor(h=%d) transport error: %s",
              sensor->handle, r.description().c_str());
        return -EIO;
    }
    Result rr = static_cast<Result>(r);
    if (rr != Result::OK) {
        ALOGE("disableSensor(h=%d) result=%d", sensor->handle, static_cast<int>(rr));
        return -EINVAL;
    }
    return 0;
}

#pragma GCC visibility pop

}
