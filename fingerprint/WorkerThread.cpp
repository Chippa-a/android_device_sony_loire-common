/*
 * Copyright (C) 2019 Marijn Suijten
 * Copyright (C) 2019-2020 The LineageOS Project
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

#define LOG_TAG "FingerprintWorkerThread"
// #define LOG_NDEBUG 0

#include <errno.h>
#include <log/log.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/poll.h>
#include <unistd.h>

#include "WorkerThread.h"

namespace SynchronizedWorkerThread {

static const char* AsyncStateToChar(const AsyncState state) {
#define ENUM_STR(enum)     \
    case AsyncState::enum: \
        return #enum;

    switch (state) {
        ENUM_STR(Invalid)
        ENUM_STR(Idle)
        ENUM_STR(Pause)
        ENUM_STR(Authenticate)
        ENUM_STR(Enroll)
        ENUM_STR(Stop)
    }
#undef ENUM_STR

    ALOGE("%s: Unknown enum state %d", __func__, state);
    return "UNKNOWN (SEE PREVIOUS ERROR)";
}

void WorkHandler::IdleAsync() {
    // The default implementation blocks indefinitely
    getWorker().isEventAvailable(-1);
}

WorkerThread::WorkerThread(WorkHandler* handler) : mHandler(handler) {
    LOG_ALWAYS_FATAL_IF(!mHandler, "WorkHandler is null!");

    event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    LOG_ALWAYS_FATAL_IF(event_fd < 0, "Failed to create eventfd: %s", strerror(errno));
}

WorkerThread::~WorkerThread() {
    Stop();

    close(event_fd);
}

int WorkerThread::getEventFd() const {
    return event_fd;
}

void* WorkerThread::ThreadStart(void* arg) {
    auto& self = *static_cast<WorkerThread*>(arg);
    self.RunThread();
    return nullptr;
}

void WorkerThread::RunThread() {
    ALOGD("Async thread up");
    for (;;) {
        auto nextState = consumeState();
        ALOGD("%s: Switched to state %s", __func__, AsyncStateToChar(nextState));
        switch (nextState) {
            case AsyncState::Idle:
                mHandler->IdleAsync();
                break;
            case AsyncState::Pause:
                isEventAvailable(-1);
                // Poll always returns if the data in the eventfd is non-zero.
                break;
            case AsyncState::Authenticate:
                mHandler->AuthenticateAsync();
                break;
            case AsyncState::Enroll:
                mHandler->EnrollAsync();
                break;
            case AsyncState::Stop:
                ALOGI("Stopping WorkerThread");
                return;
            default:
                ALOGW("Unexpected AsyncState %s", AsyncStateToChar(nextState));
                break;
        }
        currentState = AsyncState::Idle;
    }
}

void WorkerThread::Start() {
    thread = std::thread(ThreadStart, this);
}

void WorkerThread::Stop() {
    std::unique_lock<std::mutex> writerLock(mEventWriterMutex);
    std::unique_lock<std::mutex> threadLock(mThreadMutex);

    if (thread.joinable()) {
        ALOGW("Requesting thread to stop");
        auto success = waitForState(AsyncState::Stop, writerLock, threadLock);
        LOG_ALWAYS_FATAL_IF(!success, "Failed to stop thread!");
        thread.join();
    }
}

bool WorkerThread::Pause() {
    ALOGV("Waiting for thread to pause");
    return waitForState(AsyncState::Pause);
}

bool WorkerThread::Resume() {
    ALOGV("Requesting thread to resume");
    return moveToState(AsyncState::Idle);
}

AsyncState WorkerThread::consumeState() {
    std::unique_lock<std::mutex> lock(mThreadMutex);
    eventfd_t stateAvailable;
    AsyncState state = AsyncState::Idle;

    int rc = eventfd_read(event_fd, &stateAvailable);
    // When data is read, stateAvailable will be non-zero.
    if (!rc) {
        LOG_ALWAYS_FATAL_IF(desiredState == AsyncState::Invalid, "Requested state is invalid!");
        state = desiredState;
    }

    ALOGV("%s: Consumed state %s", __func__, AsyncStateToChar(state));

    currentState = state;
    desiredState = AsyncState::Invalid;

    lock.unlock();
    mThreadStateChanged.notify_all();

    return state;
}

bool WorkerThread::isEventAvailable(int timeout) const {
    struct pollfd pfd = {
            .fd = event_fd,
            .events = POLLIN,
    };

    int cnt = poll(&pfd, 1, timeout);

    if (cnt < 0) {
        ALOGE("%s: Failed polling eventfd: %d", __func__, cnt);
        return false;
    }

    bool available = cnt > 0;
    ALOGV("%s: available=%d", __func__, available);

    return available;
}

bool WorkerThread::moveToState(AsyncState state) {
    std::unique_lock<std::mutex> writerLock(mEventWriterMutex);
    std::unique_lock<std::mutex> threadLock(mThreadMutex);
    return moveToState(state, writerLock, threadLock);
}

bool WorkerThread::waitForState(AsyncState state) {
    std::unique_lock<std::mutex> writerLock(mEventWriterMutex);
    std::unique_lock<std::mutex> threadLock(mThreadMutex);
    return waitForState(state, writerLock, threadLock);
}

bool WorkerThread::moveToState(AsyncState state, std::unique_lock<std::mutex>& writerLock,
                               std::unique_lock<std::mutex>& threadLock) {
    LOG_ALWAYS_FATAL_IF(writerLock.mutex() != &mEventWriterMutex || !writerLock.owns_lock(),
                        "Caller didn't lock mEventWriterMutex!");
    LOG_ALWAYS_FATAL_IF(threadLock.mutex() != &mThreadMutex || !threadLock.owns_lock(),
                        "Caller didn't lock mThreadMutex!");

    ALOGD("%s: Setting state to %s", __func__, AsyncStateToChar(state));

    if (desiredState != AsyncState::Invalid) {
        ALOGW("Previous state %s was not consumed. Overriding to %s!",
              AsyncStateToChar(desiredState), AsyncStateToChar(state));
    }

    desiredState = state;

    int rc = eventfd_write(event_fd, 1);
    if (rc) {
        ALOGE("%s: Failed to write event-available to eventfd: %d", __func__, rc);
    }
    return !rc;
}

bool WorkerThread::waitForState(AsyncState state, std::unique_lock<std::mutex>& writerLock,
                                std::unique_lock<std::mutex>& threadLock) {
    constexpr auto wait_timeout = std::chrono::seconds(3);

    // WARNING: moveToState validates the locks. If critical code is
    // inserted, be sure to apply the same validation here as well!

    if (!moveToState(state, writerLock, threadLock)) {
        ALOGE("Failed to transition from %s to %s", AsyncStateToChar(currentState),
              AsyncStateToChar(state));
        return false;
    }

    // Wait for the thread to enter the new state:
    bool success = mThreadStateChanged.wait_for(threadLock, wait_timeout,
                                                [state, this]() { return currentState == state; });

    // Always crash, instead of blocking forever:
    LOG_ALWAYS_FATAL_IF(!success,
                        "Timed out waiting for %s for %llds. Are you writing race conditions??",
                        AsyncStateToChar(state), wait_timeout.count());

    ALOGD("%s: Successfully switched to %s", __func__, AsyncStateToChar(state));

    return true;
}

}  // namespace SynchronizedWorkerThread
