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

#include <folly/coro/GtestHelpers.h>
#include <folly/coro/TaskWrapper.h>
#include <folly/coro/Timeout.h>
#include <folly/fibers/Semaphore.h>

using namespace std::literals::chrono_literals;

namespace folly::coro {

template <typename>
class TinyNowTask;
template <typename>
class TinyNowTaskWithExecutor;

namespace detail {
template <typename T>
struct TinyNowTaskWithExecutorCfg {
  using InnerTaskWithExecutorT = TaskWithExecutor<T>;
  using WrapperTaskT = TinyNowTask<T>;
};
template <typename T>
using TinyNowTaskWithExecutorBase =
    AddMustAwaitImmediately<TaskWithExecutorWrapperCrtp<
        TinyNowTaskWithExecutor<T>,
        detail::TinyNowTaskWithExecutorCfg<T>>>;
} // namespace detail

template <typename T>
class FOLLY_NODISCARD TinyNowTaskWithExecutor final
    : public detail::TinyNowTaskWithExecutorBase<T> {
 protected:
  using detail::TinyNowTaskWithExecutorBase<T>::TinyNowTaskWithExecutorBase;
};

namespace detail {
template <typename T>
class TinyNowTaskPromise final
    : public TaskPromiseWrapper<T, TinyNowTask<T>, TaskPromise<T>> {};
template <typename T>
struct TinyNowTaskCfg {
  using ValueT = T;
  using InnerTaskT = Task<T>;
  using TaskWithExecutorT = TinyNowTaskWithExecutor<T>;
  using PromiseT = TinyNowTaskPromise<T>;
};
template <typename T>
using TinyNowTaskBase = AddMustAwaitImmediately<
    TaskWrapperCrtp<TinyNowTask<T>, detail::TinyNowTaskCfg<T>>>;
} // namespace detail

template <typename T>
class FOLLY_CORO_TASK_ATTRS TinyNowTask final
    : public detail::TinyNowTaskBase<T> {
 protected:
  using detail::TinyNowTaskBase<T>::TinyNowTaskBase;
};

///////////////////

template <typename>
class TinyMovableTask;
template <typename>
class TinyMovableTaskWithExecutor;

namespace detail {
template <typename T>
struct TinyMovableTaskWithExecutorCfg {
  using InnerTaskWithExecutorT = TaskWithExecutor<T>;
  using WrapperTaskT = TinyMovableTask<T>;
};
template <typename T>
using TinyMovableTaskWithExecutorBase = TaskWithExecutorWrapperCrtp<
    TinyMovableTaskWithExecutor<T>,
    detail::TinyMovableTaskWithExecutorCfg<T>>;
} // namespace detail

template <typename T>
class FOLLY_NODISCARD TinyMovableTaskWithExecutor final
    : public detail::TinyMovableTaskWithExecutorBase<T> {
 protected:
  using detail::TinyMovableTaskWithExecutorBase<
      T>::TinyMovableTaskWithExecutorBase;
};

namespace detail {
template <typename T>
class TinyMovableTaskPromise final
    : public TaskPromiseWrapper<T, TinyMovableTask<T>, TaskPromise<T>> {};
template <typename T>
struct TinyMovableTaskCfg {
  using ValueT = T;
  using InnerTaskT = Task<T>;
  using TaskWithExecutorT = TinyMovableTaskWithExecutor<T>;
  using PromiseT = TinyMovableTaskPromise<T>;
};
template <typename T>
using TinyMovableTaskBase =
    TaskWrapperCrtp<TinyMovableTask<T>, detail::TinyMovableTaskCfg<T>>;
} // namespace detail

template <typename T>
class FOLLY_CORO_TASK_ATTRS TinyMovableTask final
    : public detail::TinyMovableTaskBase<T> {
 protected:
  using detail::TinyMovableTaskBase<T>::TinyMovableTaskBase;
};

///////////////////

template <
    template <typename>
    class TaskT,
    template <typename>
    class TaskWithExecutorT>
struct TaskWrapperTest : testing::Test {
  static TaskT<int> intFunc(int x) { co_return x; }
  static TaskT<int> intPtrFunc(auto x) { co_return *x; }
  static TaskT<void> voidFunc(auto x, int* ran) {
    EXPECT_EQ(17, *x);
    ++*ran;
    co_return;
  }

  Task<void> checkBasics() {
    // Non-void ephemeral lambda
    EXPECT_EQ(
        1337, co_await [](int x) -> TaskT<int> { co_return 1300 + x; }(37));
    // Non-void fn & named lambda
    {
      auto x = std::make_unique<int>(17);
      auto lambdaTmpl = [](auto x) -> TaskT<int> { co_return x; };
      EXPECT_EQ(20, co_await intPtrFunc(std::move(x)) + co_await lambdaTmpl(3));
    }
    // void lambda
    {
      int ran = 0;
      auto lambdaTmpl = [&](auto x) -> TaskT<void> {
        EXPECT_EQ(3, x);
        ++ran;
        co_return;
      };
      co_await lambdaTmpl(3);
      EXPECT_EQ(1, ran);
    }
    // void fn
    {
      int ran = 0;
      auto x = std::make_unique<int>(17);
      co_await voidFunc(std::move(x), &ran);
      EXPECT_EQ(1, ran);
    }
    // can await a `Task`
    EXPECT_EQ(
        1337, co_await []() -> TaskT<int> {
          co_return 1300 + co_await ([]() -> Task<int> { co_return 37; }());
        }());
    // `co_return` works with an implicit constructor
    {
      auto t = []() -> TaskT<std::pair<int, int>> { co_return {3, 4}; };
      EXPECT_EQ(std::pair(3, 4), co_await t());
    }
  }

  Task<void> checkCancellation() {
    bool ran = false;
    EXPECT_THROW(
        co_await timeout(
            [&]() -> TaskT<void> {
              ran = true;
              folly::fibers::Semaphore stuck{0}; // a cancellable baton
              co_await stuck.co_wait();
            }(),
            200ms),
        folly::FutureTimeout);
    EXPECT_TRUE(ran);
  }

  Task<void> checkWithExecutor() {
    auto ex = co_await co_current_executor;
    static_assert(
        std::is_same_v<
            decltype(co_withExecutor(ex, intFunc(5))),
            TaskWithExecutorT<int>>);
    EXPECT_EQ(5, co_await co_withExecutor(ex, intFunc(5)));
  }

  struct MyError : std::exception {};

  Task<void> checkException() {
    EXPECT_THROW(
        co_await []() -> TaskT<void> { co_yield co_error(MyError{}); }(),
        MyError);
    auto res = co_await co_awaitTry([]() -> TaskT<void> {
      co_yield co_error(MyError{});
    }());
    EXPECT_TRUE(res.template hasException<MyError>());
  }
};

using NowTaskWrapperTest =
    TaskWrapperTest<TinyNowTask, TinyNowTaskWithExecutor>;
using MovableTaskWrapperTest =
    TaskWrapperTest<TinyMovableTask, TinyMovableTaskWithExecutor>;

CO_TEST_F(NowTaskWrapperTest, basics) {
  co_await checkBasics();
}
CO_TEST_F(MovableTaskWrapperTest, basics) {
  co_await checkBasics();
}

CO_TEST_F(NowTaskWrapperTest, withExecutor) {
  co_await checkWithExecutor();
}
CO_TEST_F(MovableTaskWrapperTest, withExecutor) {
  co_await checkWithExecutor();
}

CO_TEST_F(NowTaskWrapperTest, cancellation) {
  co_await checkCancellation();
}
CO_TEST_F(MovableTaskWrapperTest, cancellation) {
  co_await checkCancellation();
}

CO_TEST_F(NowTaskWrapperTest, exceptions) {
  co_await checkException();
}
CO_TEST_F(MovableTaskWrapperTest, exceptions) {
  co_await checkException();
}

template <typename>
struct RecursiveWrapTask;

namespace detail {

template <typename... BaseArgs>
class RecursiveTaskPromiseWrapper final
    : public TaskPromiseWrapper<BaseArgs...> {};

template <typename T, typename InnerTask, typename InnerPromise>
struct RecursiveTaskWrapperConfig {
  using ValueT = T;
  using InnerTaskT = InnerTask;
  // IMPORTANT: In a real implementation, this should, of course, wrap an
  // `InnerTaskWithExecutor`.
  using TaskWithExecutorT = TaskWithExecutor<T>;
  using PromiseT = RecursiveTaskPromiseWrapper<
      T,
      RecursiveWrapTask<RecursiveTaskWrapperConfig>,
      InnerPromise>;
};

template <typename Cfg>
using RecursiveWrapTaskBase = TaskWrapperCrtp<RecursiveWrapTask<Cfg>, Cfg>;

template <typename T>
using TwoWrapTaskConfig = RecursiveTaskWrapperConfig<
    T,
    TinyMovableTask<T>,
    TinyMovableTaskPromise<T>>;

} // namespace detail

template <typename Cfg>
struct FOLLY_CORO_TASK_ATTRS RecursiveWrapTask final
    : public detail::RecursiveWrapTaskBase<Cfg> {
  using detail::RecursiveWrapTaskBase<Cfg>::unwrapTask;

 protected:
  using detail::RecursiveWrapTaskBase<Cfg>::RecursiveWrapTaskBase;
};

template <typename T>
using TwoWrapTask = RecursiveWrapTask<detail::TwoWrapTaskConfig<T>>;

namespace detail {
template <typename T>
using ThreeWrapTaskConfig = RecursiveTaskWrapperConfig<
    T,
    TwoWrapTask<T>,
    typename TwoWrapTaskConfig<T>::PromiseT>;
} // namespace detail

template <typename T>
using ThreeWrapTask = RecursiveWrapTask<detail::ThreeWrapTaskConfig<T>>;

CO_TEST(TaskWrapper, recursiveUnwrap) {
  auto t = []() -> ThreeWrapTask<int> { co_return 3; };
  EXPECT_EQ(3, co_await t());
  static_assert(std::is_same_v<decltype(t().unwrapTask()), TwoWrapTask<int>>);
  EXPECT_EQ(3, co_await t().unwrapTask());
  static_assert(
      std::is_same_v<
          decltype(t().unwrapTask().unwrapTask()),
          TinyMovableTask<int>>);
  EXPECT_EQ(3, co_await t().unwrapTask().unwrapTask());
}

template <typename>
struct OpaqueTask;

namespace detail {
template <typename T>
class OpaqueTaskPromise final
    : public TaskPromiseWrapper<T, OpaqueTask<T>, TaskPromise<T>> {};
template <typename T>
struct OpaqueTaskWrapperConfig {
  using ValueT = T;
  using InnerTaskT = Task<T>;
  using TaskWithExecutorT = TaskWithExecutor<T>;
  using PromiseT = OpaqueTaskPromise<T>;
};
template <typename T>
using OpaqueTaskBase =
    OpaqueTaskWrapperCrtp<OpaqueTask<T>, detail::OpaqueTaskWrapperConfig<T>>;
} // namespace detail

template <typename T>
struct FOLLY_CORO_TASK_ATTRS OpaqueTask final
    : public detail::OpaqueTaskBase<T> {
  using detail::OpaqueTaskBase<T>::unwrapTask;

 protected:
  using detail::OpaqueTaskBase<T>::OpaqueTaskBase;
};

static_assert(is_semi_awaitable_v<TinyMovableTask<int>>);
static_assert(is_semi_awaitable_v<TinyNowTask<int>>);
static_assert(!is_semi_awaitable_v<OpaqueTask<int>>);

CO_TEST(TaskWrapper, opaque) {
  auto ot = [](int x) -> OpaqueTask<int> { co_return 1300 + x; }(37);
  EXPECT_EQ(1337, co_await std::move(ot).unwrapTask());
}

} // namespace folly::coro
