#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

namespace rail {

void forgetCoroutine(std::coroutine_handle<> H) noexcept;

namespace detail {

template <typename T> struct PromiseValue {
  std::optional<T> Value;
  void return_value(T V) { Value.emplace(std::move(V)); }
  T take() { return std::move(*Value); }
};

template <> struct PromiseValue<void> {
  void return_void() {}
  void take() {}
};

struct FinalAwaiter {
  bool await_ready() const noexcept { return false; }

  template <typename P> std::coroutine_handle<> await_suspend(std::coroutine_handle<P> H) const noexcept {
    if (auto Cont = H.promise().Continuation) return Cont;
    return std::noop_coroutine();
  }

  void await_resume() const noexcept {}
};

} // namespace detail

template <typename T = void> class [[nodiscard]] Coro {
public:
  struct promise_type : detail::PromiseValue<T> {
    std::coroutine_handle<> Continuation;
    std::exception_ptr Error;

    Coro get_return_object() { return Coro{std::coroutine_handle<promise_type>::from_promise(*this)}; }
    std::suspend_always initial_suspend() const noexcept { return {}; }
    detail::FinalAwaiter final_suspend() const noexcept { return {}; }
    void unhandled_exception() { Error = std::current_exception(); }
  };

  using HandleType = std::coroutine_handle<promise_type>;

  Coro() = default;
  explicit Coro(HandleType H) : H(H) {}
  Coro(const Coro &) = delete;
  Coro &operator=(const Coro &) = delete;
  Coro(Coro &&Other) noexcept : H(std::exchange(Other.H, {})) {}

  Coro &operator=(Coro &&Other) noexcept {
    if (this != &Other) {
      destroy();
      H = std::exchange(Other.H, {});
    }
    return *this;
  }

  ~Coro() { destroy(); }

  bool valid() const noexcept { return H != nullptr; }
  bool done() const noexcept { return !H || H.done(); }
  HandleType handle() const noexcept { return H; }

  // Detaches the handle so the caller owns its lifetime. Used by run().
  HandleType release() noexcept { return std::exchange(H, {}); }

  T result() {
    if (H.promise().Error) std::rethrow_exception(H.promise().Error);
    return H.promise().take();
  }

  // Begins execution without waiting for the result, so several operations can
  // be in flight at once. Pair with join() to collect the result later.
  void start() {
    if (H && !H.done()) H.resume();
  }

  // Waits for a coroutine already begun with start(). Distinct from co_await on
  // the coroutine itself, which resumes it: a started coroutine may be parked
  // on an event, so this only registers a continuation.
  auto join() noexcept {
    struct Awaiter {
      HandleType H;

      bool await_ready() const noexcept { return !H || H.done(); }
      void await_suspend(std::coroutine_handle<> Caller) const noexcept { H.promise().Continuation = Caller; }

      T await_resume() const {
        if (H.promise().Error) std::rethrow_exception(H.promise().Error);
        return H.promise().take();
      }
    };
    return Awaiter{H};
  }

  auto operator co_await() && noexcept {
    struct Awaiter {
      HandleType H;

      bool await_ready() const noexcept { return !H || H.done(); }

      std::coroutine_handle<> await_suspend(std::coroutine_handle<> Caller) const noexcept {
        H.promise().Continuation = Caller;
        return H;
      }

      T await_resume() const {
        if (H.promise().Error) std::rethrow_exception(H.promise().Error);
        return H.promise().take();
      }
    };
    return Awaiter{H};
  }

private:
  void destroy() noexcept {
    if (auto Old = std::exchange(H, {})) {
      if (!Old.done()) forgetCoroutine(Old);
      Old.destroy();
    }
  }

  HandleType H;
};

} // namespace rail
