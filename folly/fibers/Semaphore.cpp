/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/fibers/Semaphore.h>

namespace folly {
namespace fibers {

bool Semaphore::signalSlow() {
  Waiter* waiter = nullptr;
  {
    // If we signalled a release, notify the waitlist
    auto waitListLock = waitList_.wlock();
    auto& waitList = *waitListLock;

    auto testVal = tokens_.load(std::memory_order_acquire);
    if (testVal != 0) {
      return false;
    }

    if (waitList.empty()) {
      // If the waitlist is now empty, ensure the token count increments
      // No need for CAS here as we will always be under the mutex
      CHECK(tokens_.compare_exchange_strong(
          testVal, testVal + 1, std::memory_order_release));
      return true;
    }
    waiter = &waitList.front();
    waitList.pop_front();
  }
  // Trigger waiter if there is one
  // Do it after releasing the waitList mutex, in case the waiter
  // eagerly calls signal
  waiter->baton.post();
  return true;
}

void Semaphore::signal() {
  auto oldVal = tokens_.load(std::memory_order_acquire);
  do {
    while (oldVal == 0) {
      if (signalSlow()) {
        return;
      }
      oldVal = tokens_.load(std::memory_order_acquire);
    }
  } while (!tokens_.compare_exchange_weak(
      oldVal,
      oldVal + 1,
      std::memory_order_release,
      std::memory_order_acquire));
}

bool Semaphore::waitSlow(Waiter& waiter) {
  // Slow path, create a baton and acquire a mutex to update the wait list
  {
    auto waitListLock = waitList_.wlock();
    auto& waitList = *waitListLock;

    auto testVal = tokens_.load(std::memory_order_acquire);
    if (testVal != 0) {
      return false;
    }
    // prepare baton and add to queue
    waitList.push_back(waiter);
    assert(!waitList.empty());
  }
  // Signal to caller that we managed to push a waiter
  return true;
}

void Semaphore::wait() {
  auto oldVal = tokens_.load(std::memory_order_acquire);
  do {
    while (oldVal == 0) {
      Waiter waiter;
      // If waitSlow fails it is because the token is non-zero by the time
      // the lock is taken, so we can just continue round the loop
      if (waitSlow(waiter)) {
        waiter.baton.wait();
        return;
      }
      oldVal = tokens_.load(std::memory_order_acquire);
    }
  } while (!tokens_.compare_exchange_weak(
      oldVal,
      oldVal - 1,
      std::memory_order_release,
      std::memory_order_acquire));
}

bool Semaphore::try_wait(Waiter& waiter) {
  auto oldVal = tokens_.load(std::memory_order_acquire);
  do {
    while (oldVal == 0) {
      if (waitSlow(waiter)) {
        return false;
      }
      oldVal = tokens_.load(std::memory_order_acquire);
    }
  } while (!tokens_.compare_exchange_weak(
      oldVal,
      oldVal - 1,
      std::memory_order_release,
      std::memory_order_acquire));
  return true;
}

bool Semaphore::try_wait() {
  auto oldVal = tokens_.load(std::memory_order_acquire);
  do {
    if (oldVal == 0) {
      return false;
    }
  } while (!tokens_.compare_exchange_weak(
      oldVal,
      oldVal - 1,
      std::memory_order_release,
      std::memory_order_acquire));
  return true;
}

#if FOLLY_HAS_COROUTINES

coro::Task<void> Semaphore::co_wait() {
  auto oldVal = tokens_.load(std::memory_order_acquire);
  do {
    while (oldVal == 0) {
      Waiter waiter;
      // If waitSlow fails it is because the token is non-zero by the time
      // the lock is taken, so we can just continue round the loop
      if (waitSlow(waiter)) {
        bool cancelled = false;
        {
          const auto& ct = co_await folly::coro::co_current_cancellation_token;
          folly::CancellationCallback cb{
              ct, [&] {
                {
                  auto waitListLock = waitList_.wlock();
                  auto& waitList = *waitListLock;

                  if (!waiter.hook_.is_linked()) {
                    // Already dequeued by signalSlow()
                    return;
                  }

                  cancelled = true;
                  waitList.erase(waitList.iterator_to(waiter));
                }

                waiter.baton.post();
              }};

          co_await waiter.baton;
        }

        // Check 'cancelled' flag only after deregistering the callback so we're
        // sure that we aren't reading it concurrently with a potential write
        // from a thread requesting cancellation.
        // TODO: This is not unreachable code, but the compiler wrongly thinks
        // it is. Once the compiler is fixed we can remove this.
        if (cancelled) {
          co_yield folly::coro::co_cancelled;
        }

        co_return;
      }
      oldVal = tokens_.load(std::memory_order_acquire);
    }
  } while (!tokens_.compare_exchange_weak(
      oldVal,
      oldVal - 1,
      std::memory_order_release,
      std::memory_order_acquire));
}

#endif

namespace {

class FutureWaiter final : public fibers::Baton::Waiter {
 public:
  FutureWaiter() { semaphoreWaiter.baton.setWaiter(*this); }

  void post() override {
    std::unique_ptr<FutureWaiter> destroyOnReturn{this};
    promise.setValue();
  }

  Semaphore::Waiter semaphoreWaiter;
  folly::Promise<Unit> promise;
};

} // namespace

SemiFuture<Unit> Semaphore::future_wait() {
  auto oldVal = tokens_.load(std::memory_order_acquire);
  do {
    while (oldVal == 0) {
      auto batonWaiterPtr = std::make_unique<FutureWaiter>();
      // If waitSlow fails it is because the token is non-zero by the time
      // the lock is taken, so we can just continue round the loop
      auto future = batonWaiterPtr->promise.getSemiFuture();
      if (waitSlow(batonWaiterPtr->semaphoreWaiter)) {
        (void)batonWaiterPtr.release();
        return future;
      }
      oldVal = tokens_.load(std::memory_order_acquire);
    }
  } while (!tokens_.compare_exchange_weak(
      oldVal,
      oldVal - 1,
      std::memory_order_release,
      std::memory_order_acquire));
  return makeSemiFuture();
}

size_t Semaphore::getCapacity() const {
  return capacity_;
}

size_t Semaphore::getAvailableTokens() const {
  return tokens_.load(std::memory_order_relaxed);
}

} // namespace fibers
} // namespace folly
