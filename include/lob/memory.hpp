#pragma once

// Apple libc++ shipped without <memory_resource> through some C++20 toolchains.
// Use standard PMR where available, and the same resource/allocator model below
// otherwise. The fallback pools small nodes in aligned 128-node chunks.
#if __has_include(<memory_resource>)
#include <list>
#include <map>
#include <memory_resource>
#include <unordered_map>
namespace lob { namespace memory = std::pmr; }
#else
#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <list>
#include <map>
#include <new>
#include <unordered_map>
#include <vector>

namespace lob::memory {
class memory_resource {
public:
    virtual ~memory_resource() = default;
    void* allocate(std::size_t n, std::size_t a = alignof(std::max_align_t)) { return do_allocate(n, a); }
    void deallocate(void* p, std::size_t n, std::size_t a = alignof(std::max_align_t)) { do_deallocate(p, n, a); }
    bool is_equal(const memory_resource& other) const noexcept { return this == &other || do_is_equal(other); }
private:
    virtual void* do_allocate(std::size_t, std::size_t) = 0;
    virtual void do_deallocate(void*, std::size_t, std::size_t) = 0;
    virtual bool do_is_equal(const memory_resource&) const noexcept = 0;
};
class NewDeleteResource final : public memory_resource {
    void* do_allocate(std::size_t n, std::size_t a) override {
        if (a > __STDCPP_DEFAULT_NEW_ALIGNMENT__) return ::operator new(n, std::align_val_t(a));
        return ::operator new(n);
    }
    void do_deallocate(void* p, std::size_t, std::size_t a) override {
        if (a > __STDCPP_DEFAULT_NEW_ALIGNMENT__) ::operator delete(p, std::align_val_t(a));
        else ::operator delete(p);
    }
    bool do_is_equal(const memory_resource& other) const noexcept override { return this == &other; }
};
inline memory_resource* new_delete_resource() {
    static NewDeleteResource resource;
    return &resource;
}
template <class T>
class polymorphic_allocator {
public:
    using value_type = T;
    polymorphic_allocator(memory_resource* r = new_delete_resource()) noexcept : resource_(r) {}
    template <class U>
    polymorphic_allocator(const polymorphic_allocator<U>& other) noexcept : resource_(other.resource()) {}
    T* allocate(std::size_t n) {
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) throw std::bad_array_new_length();
        return static_cast<T*>(resource_->allocate(n * sizeof(T), alignof(T)));
    }
    void deallocate(T* p, std::size_t n) { resource_->deallocate(p, n * sizeof(T), alignof(T)); }
    memory_resource* resource() const noexcept { return resource_; }
    template <class U>
    bool operator==(const polymorphic_allocator<U>& other) const noexcept { return resource_->is_equal(*other.resource()); }
    template <class U>
    bool operator!=(const polymorphic_allocator<U>& other) const noexcept { return !(*this == other); }
private:
    memory_resource* resource_;
};
class unsynchronized_pool_resource final : public memory_resource {
    struct Free { Free* next; };
    struct Chunk { void* data; std::size_t bytes; };
    static constexpr std::size_t Granularity = std::max<std::size_t>(32, alignof(std::max_align_t));
    static constexpr std::size_t Classes = 16;
    static constexpr std::size_t Nodes = 128;
public:
    explicit unsynchronized_pool_resource(memory_resource* upstream) : upstream_(upstream) {}
    ~unsynchronized_pool_resource() override {
        for (auto c : chunks_) upstream_->deallocate(c.data, c.bytes, alignof(std::max_align_t));
    }
    unsynchronized_pool_resource(const unsynchronized_pool_resource&) = delete;
    unsynchronized_pool_resource& operator=(const unsynchronized_pool_resource&) = delete;
private:
    void* do_allocate(std::size_t n, std::size_t a) override {
        if (n > Granularity * Classes || a > alignof(std::max_align_t)) return upstream_->allocate(n, a);
        const auto bucket = (std::max(n, sizeof(Free)) - 1) / Granularity;
        if (!free_[bucket]) {
            auto stride = (bucket + 1) * Granularity;
            const auto bytes = stride * Nodes;
            auto* chunk = static_cast<std::byte*>(upstream_->allocate(bytes, alignof(std::max_align_t)));
            try { chunks_.push_back({chunk, bytes}); }
            catch (...) { upstream_->deallocate(chunk, bytes, alignof(std::max_align_t)); throw; }
            for (std::size_t i = 0; i < Nodes; ++i) {
                auto* node = ::new (static_cast<void*>(chunk + i * stride)) Free{free_[bucket]};
                free_[bucket] = node;
            }
        }
        auto* node = free_[bucket];
        free_[bucket] = node->next;
        return node;
    }
    void do_deallocate(void* p, std::size_t n, std::size_t a) override {
        if (n > Granularity * Classes || a > alignof(std::max_align_t)) { upstream_->deallocate(p, n, a); return; }
        const auto bucket = (std::max(n, sizeof(Free)) - 1) / Granularity;
        free_[bucket] = ::new (p) Free{free_[bucket]};
    }
    bool do_is_equal(const memory_resource& other) const noexcept override { return this == &other; }
    memory_resource* upstream_;
    std::array<Free*, Classes> free_{};
    std::vector<Chunk> chunks_;
};
template <class T> using list = std::list<T, polymorphic_allocator<T>>;
template <class K, class V, class Compare = std::less<K>>
using map = std::map<K, V, Compare, polymorphic_allocator<std::pair<const K, V>>>;
template <class K, class V>
using unordered_map = std::unordered_map<K, V, std::hash<K>, std::equal_to<K>, polymorphic_allocator<std::pair<const K, V>>>;
} // namespace lob::memory
#endif
