// Included into every C++ file of the Android build (tools/build_android.ps1, -include): what
// Aurora uses of C++20's library that NDK r27's libc++ (LLVM 18) lacks. std::jthread and
// std::stop_token are there behind -fexperimental-library; std::atomic_ref (libc++ 19) is
// provided here, as far as Aurora uses it (load, store, fetch_add/sub on integers), through the
// compiler's atomic builtins. A newer NDK (r28+) needs none of this.
#pragma once
#ifdef __cplusplus
#include <atomic>
#include <version>

#if defined(_LIBCPP_VERSION) && !defined(__cpp_lib_atomic_ref)
namespace std {
template <class T>
struct atomic_ref {
    static_assert(is_trivially_copyable_v<T>);
    using value_type = T;
    explicit atomic_ref(T& r) noexcept : p_(&r) {}
    atomic_ref(const atomic_ref&) noexcept = default;
    T load(memory_order o = memory_order_seq_cst) const noexcept { return __atomic_load_n(p_, int(o)); }
    void store(T v, memory_order o = memory_order_seq_cst) const noexcept { __atomic_store_n(p_, v, int(o)); }
    T exchange(T v, memory_order o = memory_order_seq_cst) const noexcept { return __atomic_exchange_n(p_, v, int(o)); }
    T fetch_add(T v, memory_order o = memory_order_seq_cst) const noexcept { return __atomic_fetch_add(p_, v, int(o)); }
    T fetch_sub(T v, memory_order o = memory_order_seq_cst) const noexcept { return __atomic_fetch_sub(p_, v, int(o)); }
    operator T() const noexcept { return load(); }
    T operator++() const noexcept { return fetch_add(1) + 1; }
    T operator++(int) const noexcept { return fetch_add(1); }
    T operator--() const noexcept { return fetch_sub(1) - 1; }
    T operator--(int) const noexcept { return fetch_sub(1); }
    T operator+=(T v) const noexcept { return fetch_add(v) + v; }
    T operator-=(T v) const noexcept { return fetch_sub(v) - v; }

private:
    T* p_;
};
template <class T>
atomic_ref(T&) -> atomic_ref<T>;
}  // namespace std
#endif
#endif
