/*
 * Copyright (C) 2020 The Android Open Source Project
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

#define LOG_TAG "libusbconfigfs"

#include "include/UsbGadgetCommon.h"
#include <android-base/strings.h>

namespace android {
namespace hardware {
namespace usb {
namespace gadget {

static volatile bool gadgetPullup;

MonitorFfs::MonitorFfs(const char* const gadget)
    : mPullupPolicyPath(GetProperty("ro.vendor.usb.gadget.pullup_policy", "")),
      mWatchFd(),
      mEndpointList(),
      mLock(),
      mCv(),
      mLockFd(),
      mCurrentUsbFunctionsApplied(false),
      mMonitor(),
      mCallback(NULL),
      mPayload(NULL),
      mGadgetName(gadget),
      mMonitorRunning(false) {
    unique_fd eventFd(eventfd(0, 0));
    if (eventFd == -1) {
        ALOGE("mEventFd failed to create %d", errno);
        abort();
    }

    unique_fd epollFd(epoll_create(2));
    if (epollFd == -1) {
        ALOGE("mEpollFd failed to create %d", errno);
        abort();
    }

    unique_fd inotifyFd(inotify_init());
    if (inotifyFd < 0) {
        ALOGE("inotify init failed");
        abort();
    }

    if (addEpollFd(epollFd, inotifyFd) == -1) abort();

    if (addEpollFd(epollFd, eventFd) == -1) abort();

    mEpollFd = std::move(epollFd);
    mInotifyFd = std::move(inotifyFd);
    mEventFd = std::move(eventFd);
    gadgetPullup = false;

    initializePullupPolicy();
}

bool MonitorFfs::initializePullupPolicy() {
    if (mPullupPolicyPath.empty() || mPullupPolicyFd.get() >= 0) return true;
    mPullupPolicyFd.reset(TEMP_FAILURE_RETRY(
            open(mPullupPolicyPath.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK)));
    if (mPullupPolicyFd.get() < 0) {
        ALOGE("Unable to open USB pullup policy: %d; new connections blocked", errno);
        return false;
    }
    // A sysfs read acknowledges the initial notification counter. A disabled
    // policy is a valid input; an I/O failure invalidates the descriptor.
    pullupAllowed();
    if (mPullupPolicyFd.get() < 0) return false;
    struct epoll_event policyEvent = {};
    policyEvent.events = EPOLLPRI | EPOLLERR;
    policyEvent.data.fd = mPullupPolicyFd.get();
    if (epoll_ctl(mEpollFd, EPOLL_CTL_ADD, mPullupPolicyFd, &policyEvent) != 0) {
        ALOGE("Unable to monitor USB pullup policy: %d; new connections blocked", errno);
        mPullupPolicyFd.reset();
        return false;
    }
    return true;
}

bool MonitorFfs::pullupAllowed() {
    if (mPullupPolicyPath.empty()) return true;
    if (mPullupPolicyFd.get() < 0) return false;
    string value;
    if (lseek(mPullupPolicyFd, 0, SEEK_SET) < 0 ||
        !::android::base::ReadFdToString(mPullupPolicyFd, &value)) {
        ALOGE("Unable to read USB pullup policy: %d; new connections blocked", errno);
        // Closing removes the descriptor from epoll. Remain alive and fail
        // closed, without repeatedly waking on a persistent error. A later
        // function reconfiguration can reopen the policy in startMonitor().
        mPullupPolicyFd.reset();
        return false;
    }
    value = ::android::base::Trim(value);
    // KEEP may rebind the existing session; the kernel refuses a new session.
    // Unknown values and read failures continue to block pullup.
    return value == "1" || value == "-1";
}

bool MonitorFfs::tryPullup() {
    if (!pullupAllowed()) return false;
    for (const auto& endpoint : mEndpointList) {
        if (access(endpoint.c_str(), R_OK) != 0) return false;
    }
    string current;
    // A controller can already have rebound the configured gadget on enable.
    if (!::android::base::ReadFileToString(PULLUP_PATH, &current) ||
        ::android::base::Trim(current) != mGadgetName) {
        if (!WriteStringToFile(mGadgetName, PULLUP_PATH)) return false;
    }
    lock_guard<mutex> lock(mLock);
    mCurrentUsbFunctionsApplied = true;
    if (mCallback) mCallback(true, mPayload);
    gadgetPullup = true;
    mCv.notify_all();
    return true;
}

static void displayInotifyEvent(struct inotify_event* i) {
    ALOGE("    wd =%2d; ", i->wd);
    if (i->cookie > 0) ALOGE("cookie =%4d; ", i->cookie);

    ALOGE("mask = ");
    if (i->mask & IN_ACCESS) ALOGE("IN_ACCESS ");
    if (i->mask & IN_ATTRIB) ALOGE("IN_ATTRIB ");
    if (i->mask & IN_CLOSE_NOWRITE) ALOGE("IN_CLOSE_NOWRITE ");
    if (i->mask & IN_CLOSE_WRITE) ALOGE("IN_CLOSE_WRITE ");
    if (i->mask & IN_CREATE) ALOGE("IN_CREATE ");
    if (i->mask & IN_DELETE) ALOGE("IN_DELETE ");
    if (i->mask & IN_DELETE_SELF) ALOGE("IN_DELETE_SELF ");
    if (i->mask & IN_IGNORED) ALOGE("IN_IGNORED ");
    if (i->mask & IN_ISDIR) ALOGE("IN_ISDIR ");
    if (i->mask & IN_MODIFY) ALOGE("IN_MODIFY ");
    if (i->mask & IN_MOVE_SELF) ALOGE("IN_MOVE_SELF ");
    if (i->mask & IN_MOVED_FROM) ALOGE("IN_MOVED_FROM ");
    if (i->mask & IN_MOVED_TO) ALOGE("IN_MOVED_TO ");
    if (i->mask & IN_OPEN) ALOGE("IN_OPEN ");
    if (i->mask & IN_Q_OVERFLOW) ALOGE("IN_Q_OVERFLOW ");
    if (i->mask & IN_UNMOUNT) ALOGE("IN_UNMOUNT ");
    ALOGE("\n");

    if (i->len > 0) ALOGE("        name = %s\n", i->name);
}

void* MonitorFfs::startMonitorFd(void* param) {
    auto* monitorFfs = static_cast<MonitorFfs*>(param);
    alignas(inotify_event) char buf[kBufferSize];
    bool writeUdc = true;
    struct epoll_event events[kEpollEvents];
    steady_clock::time_point disconnect = steady_clock::time_point::min();
    bool descriptorsWerePresent = false;

    bool descriptorsPresentAtStart = true;
    for (const auto& endpoint : monitorFfs->mEndpointList) {
        if (access(endpoint.c_str(), R_OK) != 0) {
            descriptorsPresentAtStart = false;
            break;
        }
    }
    // Preserve the existing settling interval only for descriptors already
    // present at startup. Descriptors created later need no artificial delay.
    if (descriptorsPresentAtStart && monitorFfs->pullupAllowed()) {
        usleep(kPullUpDelay);
        if (monitorFfs->tryPullup()) {
            writeUdc = false;
            descriptorsWerePresent = true;
        }
    }

    for (;;) {
        int nrEvents = TEMP_FAILURE_RETRY(
                epoll_wait(monitorFfs->mEpollFd, events, kEpollEvents, -1));
        LOG_ALWAYS_FATAL_IF(nrEvents < 0, "epoll wait failed: %d", errno);
        bool reevaluate = false;
        for (int i = 0; i < nrEvents; i++) {
            if (events[i].data.fd == monitorFfs->mEventFd) {
                uint64_t flag;
                if (TEMP_FAILURE_RETRY(read(monitorFfs->mEventFd, &flag, sizeof(flag))) ==
                            static_cast<ssize_t>(sizeof(flag)) &&
                    flag == kShutdownMonitor) return nullptr;
            } else if (events[i].data.fd == monitorFfs->mPullupPolicyFd.get()) {
                // Read before waiting again to acknowledge EPOLLPRI/POLLERR.
                if (!monitorFfs->pullupAllowed()) writeUdc = true;
                reevaluate = true;
            } else if (events[i].data.fd == monitorFfs->mInotifyFd) {
                int numRead = TEMP_FAILURE_RETRY(read(monitorFfs->mInotifyFd, buf, sizeof(buf)));
                if (numRead <= 0) continue;
                for (size_t offset = 0; offset + sizeof(inotify_event) <=
                                      static_cast<size_t>(numRead);) {
                    const auto* event = reinterpret_cast<const inotify_event*>(buf + offset);
                    size_t length = sizeof(inotify_event) + event->len;
                    if (length > static_cast<size_t>(numRead) - offset ||
                        length % alignof(inotify_event) != 0) break;
                    if (kDebug) displayInotifyEvent(const_cast<inotify_event*>(event));
                    if (event->mask & (IN_DELETE | IN_DELETE_SELF | IN_MOVED_FROM | IN_UNMOUNT)) {
                        // A real descriptor teardown starts one settling interval.
                        // Initial descriptor creation and unrelated notifications do not.
                        if (descriptorsWerePresent) disconnect = steady_clock::now();
                        descriptorsWerePresent = false;
                        writeUdc = true;
                    }
                    offset += length;
                }
                reevaluate = true;
            }
        }
        if (!reevaluate) continue;
        bool descriptorPresent = true;
        for (const auto& endpoint : monitorFfs->mEndpointList) {
            if (access(endpoint.c_str(), R_OK) != 0) {
                descriptorPresent = false;
                break;
            }
        }
        if (!descriptorPresent) {
            if (descriptorsWerePresent) disconnect = steady_clock::now();
            descriptorsWerePresent = false;
            writeUdc = true;
        } else if (writeUdc && monitorFfs->pullupAllowed()) {
            descriptorsWerePresent = true;
            if (disconnect != steady_clock::time_point::min()) {
                auto elapsed = std::chrono::duration_cast<microseconds>(
                        steady_clock::now() - disconnect).count();
                if (elapsed < kPullUpDelay) usleep(kPullUpDelay - elapsed);
            }
            // Recheck the policy after the settling delay. A concurrent disable
            // remains enforced by the kernel even if it races this last check.
            if (monitorFfs->tryPullup()) writeUdc = false;
        }
    }
}

void MonitorFfs::reset() {
    lock_guard<mutex> lock(mLockFd);
    uint64_t flag = 100;
    unsigned long ret;

    if (mMonitorRunning) {
        // Stop the monitor thread by writing into signal fd.
        ret = TEMP_FAILURE_RETRY(write(mEventFd, &flag, sizeof(flag)));
        if (ret < 0) ALOGE("Error writing eventfd errno=%d", errno);

        ALOGI("mMonitor signalled to exit");
        mMonitor->join();
        ALOGI("mMonitor destroyed");
        mMonitorRunning = false;
    }

    for (std::vector<int>::size_type i = 0; i != mWatchFd.size(); i++)
        inotify_rm_watch(mInotifyFd, mWatchFd[i]);

    mEndpointList.clear();
    gadgetPullup = false;
    mCallback = NULL;
    mPayload = NULL;
}

bool MonitorFfs::startMonitor() {
    initializePullupPolicy();
    mMonitor = unique_ptr<thread>(new thread(this->startMonitorFd, this));
    mMonitorRunning = true;
    return true;
}

bool MonitorFfs::isMonitorRunning() {
    return mMonitorRunning;
}

bool MonitorFfs::waitForPullUp(int timeout_ms) {
    std::unique_lock<std::mutex> lk(mLock);

    if (gadgetPullup) return true;

    if (mCv.wait_for(lk, timeout_ms * 1ms, [] { return gadgetPullup; })) {
        ALOGI("monitorFfs signalled true");
        return true;
    } else {
        ALOGI("monitorFfs signalled error");
        // continue monitoring as the descriptors might be written at a later
        // point.
        return false;
    }
}

bool MonitorFfs::addInotifyFd(string fd) {
    lock_guard<mutex> lock(mLockFd);
    int wfd;

    wfd = inotify_add_watch(mInotifyFd, fd.c_str(), IN_ALL_EVENTS);
    if (wfd == -1)
        return false;
    else
        mWatchFd.push_back(wfd);

    return true;
}

void MonitorFfs::addEndPoint(string ep) {
    lock_guard<mutex> lock(mLockFd);

    mEndpointList.push_back(ep);
}

void MonitorFfs::registerFunctionsAppliedCallback(void (*callback)(bool functionsApplied,
                                                                   void* payload),
                                                  void* payload) {
    mCallback = callback;
    mPayload = payload;
}

}  // namespace gadget
}  // namespace usb
}  // namespace hardware
}  // namespace android
