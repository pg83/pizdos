using u8 = __UINT8_TYPE__;
using u16 = __UINT16_TYPE__;
using u32 = __UINT32_TYPE__;
using u64 = __UINT64_TYPE__;
using i8 = __INT8_TYPE__;
using i16 = __INT16_TYPE__;
using i32 = __INT32_TYPE__;
using i64 = __INT64_TYPE__;
using usize = __SIZE_TYPE__;
using uptr = __UINTPTR_TYPE__;

#if !defined(__wasm__) && !defined(wasm)
    #include <stdlib.h>
    #include <unistd.h>
    #include <errno.h>
#endif

void* operator new(usize, void* p) noexcept {
    return p;
}

namespace {
    static constexpr usize pageBytes = 65536;
    static constexpr usize objectAlignment = 16;

    struct IoVec {
        void* data;
        usize length;
    };

    struct ConstIoVec {
        const void* data;
        usize length;
    };
}

#if defined(__wasm__) || defined(wasm)
extern "C" __attribute__((import_module("wasi_snapshot_preview1"), import_name("fd_read"))) u16 wasiFdRead(u32, const IoVec*, usize, usize*);
extern "C" __attribute__((import_module("wasi_snapshot_preview1"), import_name("fd_write"))) u16 wasiFdWrite(u32, const ConstIoVec*, usize, usize*);
extern "C" __attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_exit"), noreturn)) void wasiProcExit(u32);
#else
extern "C" u16 wasiFdRead(u32 fd, const IoVec* vectors, usize count, usize* done) {
    *done = 0;
    for (usize i = 0; i < count; ++i) {
        ssize_t result;
        do {
            result = ::read(static_cast<int>(fd), vectors[i].data, vectors[i].length);
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            return 29;
        }
        *done += static_cast<usize>(result);
        if (static_cast<usize>(result) != vectors[i].length) {
            break;
        }
    }
    return 0;
}

extern "C" u16 wasiFdWrite(u32 fd, const ConstIoVec* vectors, usize count, usize* done) {
    *done = 0;
    for (usize i = 0; i < count; ++i) {
        ssize_t result;
        do {
            result = ::write(static_cast<int>(fd), vectors[i].data, vectors[i].length);
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            return 29;
        }
        *done += static_cast<usize>(result);
        if (static_cast<usize>(result) != vectors[i].length) {
            break;
        }
    }
    return 0;
}

extern "C" [[noreturn]] void wasiProcExit(u32 code) {
    if (code == 134) {
        ::abort();
    }
    ::_exit(static_cast<int>(code));
}
#endif

namespace {
    static void copyBytes(void* destination, const void* source, usize size) {
        auto* out = static_cast<u8*>(destination);
        const auto* in = static_cast<const u8*>(source);
        for (usize i = 0; i < size; ++i) {
            out[i] = in[i];
        }
    }

    static void zeroBytes(void* destination, usize size) {
        auto* out = static_cast<u8*>(destination);
        for (usize i = 0; i < size; ++i) {
            out[i] = 0;
        }
    }

    static void moveBytes(void* destination, const void* source, usize size) {
        auto* out = static_cast<u8*>(destination);
        const auto* in = static_cast<const u8*>(source);
        if (reinterpret_cast<uptr>(out) <= reinterpret_cast<uptr>(in)) {
            copyBytes(destination, source, size);
        } else {
            for (usize i = size; i != 0; --i) {
                out[i - 1] = in[i - 1];
            }
        }
    }

    static void writeAll(u32 fd, const void* data, usize length) {
        auto* at = static_cast<const u8*>(data);
        while (length != 0) {
            const ConstIoVec vector{at, length};
            usize done = 0;
            if (wasiFdWrite(fd, &vector, 1, &done) != 0 || done == 0) {
                wasiProcExit(1);
            }
            at += done;
            length -= done;
        }
    }

    [[noreturn]] static void fail(const char* text) {
        usize length = 0;
        while (text[length] != 0) {
            ++length;
        }
        writeAll(2, text, length);
        writeAll(2, "\n", 1);
        wasiProcExit(134);
    }

    static void require(bool condition, const char* text) {
        if (!condition) {
            fail(text);
        }
    }

    static usize checkedAdd(usize a, usize b) {
        require(b <= ~usize(0) - a, "size overflow");
        return a + b;
    }

    static usize checkedMultiply(usize a, usize b) {
        require(a == 0 || b <= ~usize(0) / a, "size overflow");
        return a * b;
    }

    static usize alignUp(usize size, usize alignment) {
        return checkedAdd(size, alignment - 1) & ~(alignment - 1);
    }

    struct FreePage {
        FreePage* next;
    };

    static FreePage* freePages;

    static void* growPages(usize count) {
        require(count != 0, "zero page allocation");
#if defined(__wasm__) || defined(wasm)
        const usize first = __builtin_wasm_memory_grow(0, count);
        if (first == ~usize(0)) {
            fail("out of memory");
        }
        return reinterpret_cast<void*>(first * pageBytes);
#else
        void* data = ::aligned_alloc(pageBytes, checkedMultiply(count, pageBytes));
        if (data == nullptr) {
            fail("out of memory");
        }
        return data;
#endif
    }

    static void* allocPage() {
        if (freePages != nullptr) {
            auto* result = freePages;
            freePages = result->next;
            return result;
        }
        return growPages(1);
    }

    static void freePage(void* page) {
        require(page != nullptr && reinterpret_cast<uptr>(page) % pageBytes == 0, "invalid page");
        auto* link = static_cast<FreePage*>(page);
        link->next = freePages;
        freePages = link;
    }

    struct FreeObject {
        FreeObject* next;
    };

    struct ObjectClass {
        u8* current;
        usize used;
        FreeObject* free;
    };

    struct SmallObjAllocator {
        void* allocate(usize size);
        void deallocate(void* object, usize size);

        ObjectClass classes[13]{};
    };

    static SmallObjAllocator smallAllocator;

    static usize allocationClass(usize size) {
        require(size != 0 && size <= pageBytes, "small allocation exceeds 64 KiB");
        usize index = 0;
        usize bytes = objectAlignment;
        while (bytes < size) {
            bytes *= 2;
            ++index;
        }
        return index;
    }
}

void* SmallObjAllocator::allocate(usize size) {
    const usize index = allocationClass(size);
    auto& state = classes[index];
    if (state.free != nullptr) {
        auto* result = state.free;
        state.free = result->next;
        return result;
    }
    const usize bytes = objectAlignment << index;
    if (state.current == nullptr || bytes > pageBytes - state.used) {
        state.current = static_cast<u8*>(allocPage());
        state.used = 0;
    }
    auto* result = state.current + state.used;
    state.used += bytes;
    return result;
}

void SmallObjAllocator::deallocate(void* object, usize size) {
    if (object == nullptr) {
        return;
    }
    auto& state = classes[allocationClass(size)];
    auto* link = static_cast<FreeObject*>(object);
    link->next = state.free;
    state.free = link;
}

namespace {
    template <typename T>
    static T&& move(T& value) {
        return static_cast<T&&>(value);
    }

    struct Buffer {
        struct alignas(objectAlignment) Header {
            usize used;
            usize allocation;
        };

        Buffer();
        ~Buffer();
        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;
        Buffer(Buffer&& other) noexcept;
        Buffer& operator=(Buffer&& other) noexcept;
        void* data();
        const void* data() const;
        usize size() const;
        usize capacity() const;
        void reserve(usize size);
        void resize(usize size);
        void clear();
        void append(const void* data, usize size);
        void swap(Buffer& other);

        Header* header() const;
        void* storage;
    };

    alignas(objectAlignment) static const Buffer::Header emptyBuffer{};
}

Buffer::Buffer()
    : storage(const_cast<Header*>(&emptyBuffer) + 1)
{
}

Buffer::~Buffer() {
    if (header() != &emptyBuffer) {
        smallAllocator.deallocate(header(), header()->allocation);
    }
}

Buffer::Buffer(Buffer&& other) noexcept
    : Buffer() {
    swap(other);
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    Buffer temporary;
    temporary.swap(other);
    swap(temporary);
    return *this;
}

Buffer::Header* Buffer::header() const {
    return static_cast<Header*>(storage) - 1;
}

void* Buffer::data() {
    return storage;
}

const void* Buffer::data() const {
    return storage;
}

usize Buffer::size() const {
    return header()->used;
}

usize Buffer::capacity() const {
    return header()->allocation == 0 ? 0 : header()->allocation - sizeof(Header);
}

void Buffer::reserve(usize size) {
    if (size <= capacity()) {
        return;
    }
    const usize need = checkedAdd(size, sizeof(Header));
    const usize bytes = objectAlignment << allocationClass(need);
    auto* next = static_cast<Header*>(smallAllocator.allocate(bytes));
    *next = {header()->used, bytes};
    copyBytes(next + 1, storage, header()->used);
    if (header() != &emptyBuffer) {
        smallAllocator.deallocate(header(), header()->allocation);
    }
    storage = next + 1;
}

void Buffer::resize(usize size) {
    reserve(size);
    if (size != 0 || header() != &emptyBuffer) {
        header()->used = size;
    }
}

void Buffer::clear() {
    if (header() != &emptyBuffer) {
        header()->used = 0;
    }
}

void Buffer::append(const void* data, usize size) {
    if (size == 0) {
        return;
    }
    const usize before = this->size();
    const auto pointer = reinterpret_cast<uptr>(data);
    const auto base = reinterpret_cast<uptr>(storage);
    const bool internal = pointer >= base && pointer - base <= before && size <= before - (pointer - base);
    const usize offset = internal ? pointer - base : 0;
    reserve(checkedAdd(before, size));
    if (internal) {
        data = static_cast<const u8*>(storage) + offset;
    }
    moveBytes(static_cast<u8*>(storage) + before, data, size);
    header()->used = before + size;
}

void Buffer::swap(Buffer& other) {
    auto* temporary = storage;
    storage = other.storage;
    other.storage = temporary;
}

namespace {
    template <typename T>
    struct SmallVec {
        static_assert(__is_trivially_copyable(T));
        static_assert(__is_trivially_destructible(T));
        static_assert(alignof(T) <= objectAlignment);

        SmallVec() = default;
        SmallVec(const SmallVec&) = delete;
        SmallVec& operator=(const SmallVec&) = delete;
        SmallVec(SmallVec&&) = default;
        SmallVec& operator=(SmallVec&&) = default;

        T* data() {
            return static_cast<T*>(buffer.data());
        }

        const T* data() const {
            return static_cast<const T*>(buffer.data());
        }

        usize size() const {
            return buffer.size() / sizeof(T);
        }

        usize capacity() const {
            return buffer.capacity() / sizeof(T);
        }

        bool empty() const {
            return size() == 0;
        }

        T& operator[](usize index) {
            require(index < size(), "SmallVec index");
            return data()[index];
        }

        const T& operator[](usize index) const {
            require(index < size(), "SmallVec index");
            return data()[index];
        }

        T& back() {
            return (*this)[size() - 1];
        }

        const T& back() const {
            return (*this)[size() - 1];
        }

        void reserve(usize count) {
            buffer.reserve(checkedMultiply(count, sizeof(T)));
        }

        void resize(usize count) {
            buffer.resize(checkedMultiply(count, sizeof(T)));
        }

        void clear() {
            buffer.clear();
        }

        void pushBack(const T& value) {
            buffer.append(&value, sizeof(T));
        }

        T popBack() {
            T value = back();
            resize(size() - 1);
            return value;
        }

        void append(const T* values, usize count) {
            buffer.append(values, checkedMultiply(count, sizeof(T)));
        }

        template <typename F>
        void visit(F f) {
            for (usize i = 0; i < size(); ++i) {
                f((*this)[i]);
            }
        }

        template <typename F>
        void visit(F f) const {
            for (usize i = 0; i < size(); ++i) {
                f((*this)[i]);
            }
        }

        Buffer buffer;
    };

    template <typename T>
    struct PageVec {
        static_assert(__is_trivially_copyable(T));
        static_assert(__is_trivially_destructible(T));
        static_assert(sizeof(T) <= pageBytes);
        static_assert(alignof(T) <= objectAlignment);
        static constexpr usize itemsPerPage = pageBytes / sizeof(T);

        PageVec() = default;
        PageVec(const PageVec&) = delete;
        PageVec& operator=(const PageVec&) = delete;

        ~PageVec() {
            pages.visit([](T* page) {
                freePage(page);
            });
        }

        PageVec(PageVec&& other) noexcept
            : pages(move(other.pages))
            , count(other.count) {
            other.count = 0;
        }

        PageVec& operator=(PageVec&& other) noexcept {
            if (this != &other) {
                this->~PageVec();
                new (this) PageVec(move(other));
            }
            return *this;
        }

        usize size() const {
            return count;
        }

        usize capacity() const {
            return checkedMultiply(pages.size(), itemsPerPage);
        }

        bool empty() const {
            return count == 0;
        }

        T& operator[](usize index) {
            require(index < count, "PageVec index");
            return pages[index / itemsPerPage][index % itemsPerPage];
        }

        const T& operator[](usize index) const {
            require(index < count, "PageVec index");
            return pages[index / itemsPerPage][index % itemsPerPage];
        }

        T& back() {
            return (*this)[count - 1];
        }

        const T& back() const {
            return (*this)[count - 1];
        }

        void reserve(usize count) {
            while (capacity() < count) {
                auto* page = static_cast<T*>(allocPage());
                pages.pushBack(page);
            }
        }

        void resize(usize size) {
            reserve(size);
            count = size;
        }

        void clear() {
            count = 0;
        }

        void pushBack(const T& value) {
            const usize at = count;
            reserve(checkedAdd(at, 1));
            ++count;
            (*this)[at] = value;
        }

        T popBack() {
            T value = back();
            --count;
            return value;
        }

        void append(const T* values, usize count) {
            for (usize i = 0; i < count; ++i) {
                pushBack(values[i]);
            }
        }

        template <typename F>
        void visit(F f) {
            for (usize i = 0; i < count; ++i) {
                f((*this)[i]);
            }
        }

        template <typename F>
        void visit(F f) const {
            for (usize i = 0; i < count; ++i) {
                f((*this)[i]);
            }
        }

        SmallVec<T*> pages;
        usize count = 0;
    };

    struct ObjPool {
        struct alignas(objectAlignment) Chunk {
            Chunk* next;
            usize size;
        };

        struct Disposal {
            Disposal* next;
            void (*destroy)(Disposal*);
        };

        template <typename T>
        struct Holder {
            Disposal disposal;
            alignas(T) u8 object[sizeof(T)];
        };

        ObjPool() = default;
        ~ObjPool();
        ObjPool(const ObjPool&) = delete;
        ObjPool& operator=(const ObjPool&) = delete;
        void* allocate(usize size, usize alignment = objectAlignment);
        void clear();

        template <typename T, typename... A>
        T* make(A&&... args) {
            static_assert(alignof(T) <= objectAlignment);
            if constexpr (__is_trivially_destructible(T)) {
                return new (allocate(sizeof(T), alignof(T))) T(static_cast<A&&>(args)...);
            } else {
                auto* holder = static_cast<Holder<T>*>(allocate(sizeof(Holder<T>), alignof(Holder<T>)));
                auto* object = new (holder->object) T(static_cast<A&&>(args)...);
                holder->disposal = {disposals, [](Disposal* node) {
                    auto* entry = reinterpret_cast<Holder<T>*>(node);
                    reinterpret_cast<T*>(entry->object)->~T();
                }};
                disposals = &holder->disposal;
                return object;
            }
        }

        Chunk* chunks = nullptr;
        Disposal* disposals = nullptr;
        u8* current = nullptr;
        usize left = 0;
        usize nextChunk = 128;
    };
}

ObjPool::~ObjPool() {
    clear();
}

void* ObjPool::allocate(usize size, usize alignment) {
    require(size <= pageBytes - sizeof(Chunk) && alignment <= objectAlignment, "object exceeds pool chunk");
    size = alignUp(size == 0 ? 1 : size, objectAlignment);
    if (size > left) {
        usize bytes = nextChunk;
        while (bytes < checkedAdd(size, sizeof(Chunk))) {
            bytes *= 2;
        }
        require(bytes <= pageBytes, "object exceeds pool chunk");
        auto* chunk = static_cast<Chunk*>(smallAllocator.allocate(bytes));
        *chunk = {chunks, bytes};
        chunks = chunk;
        current = reinterpret_cast<u8*>(chunk + 1);
        left = bytes - sizeof(Chunk);
        nextChunk = bytes < pageBytes ? bytes * 2 : pageBytes;
    }
    auto* result = current;
    current += size;
    left -= size;
    return result;
}

void ObjPool::clear() {
    while (disposals != nullptr) {
        auto* node = disposals;
        disposals = node->next;
        node->destroy(node);
    }
    while (chunks != nullptr) {
        auto* chunk = chunks;
        chunks = chunk->next;
        smallAllocator.deallocate(chunk, chunk->size);
    }
    current = nullptr;
    left = 0;
    nextChunk = 128;
}

struct Blob {
    const u8* data;
    usize size;
};

#if defined(AOT_SELF_TEST)
namespace {
    struct LifetimeProbe {
        int* count;
        int id;
        int* order;

        LifetimeProbe(int* c, int i, int* o)
            : count(c)
            , id(i)
            , order(o)
        {
            ++*count;
        }

        ~LifetimeProbe() {
            order[--*count] = id;
        }
    };

    static void selfTest() {
        void* page = allocPage();
        freePage(page);
        require(allocPage() == page, "page freelist");
        freePage(page);
        for (usize bytes = 1; bytes <= pageBytes; bytes *= 2) {
            void* object = smallAllocator.allocate(bytes);
            require(reinterpret_cast<uptr>(object) % objectAlignment == 0, "object alignment");
            smallAllocator.deallocate(object, bytes);
            require(smallAllocator.allocate(bytes) == object, "object freelist");
            smallAllocator.deallocate(object, bytes);
        }
        SmallVec<u32> small;
        require(sizeof(small) == sizeof(void*) && small.empty(), "empty vector representation");
        for (u32 i = 0; i < 1000; ++i) {
            small.pushBack(i);
        }
        for (u32 i = 0; i < 1000; ++i) {
            require(small[i] == i, "small vector contents");
        }
        PageVec<u64> large;
        for (u64 i = 0; i < 50000; ++i) {
            large.pushBack(i * 7);
        }
        for (usize i = 0; i < large.size(); ++i) {
            require(large[i] == i * 7, "paged vector contents");
        }
        large.resize(60000);
        require(large.size() == 60000, "paged resize growth");
        for (usize i = 50000; i < large.size(); ++i) {
            large[i] = i * 7;
        }
        large.resize(50000);
        require(large.size() == 50000, "paged resize shrink");
        PageVec<u64> moved(move(large));
        require(large.empty() && moved.size() == 50000, "paged vector move");
        u64 total = 0;
        moved.visit([&](u64 value) {
            total += value;
        });
        require(total == u64(49999) * 50000 / 2 * 7, "paged visitor");
        int count = 0;
        int order[3]{};
        {
            ObjPool pool;
            for (int i = 0; i < 3; ++i) {
                pool.make<LifetimeProbe>(&count, i + 1, order);
            }
            for (int i = 0; i < 100000; ++i) {
                auto* p = pool.make<u64>(u64(i));
                require(*p == u64(i), "pool contents");
            }
            require(count == 3, "pool ownership");
        }
        require(count == 0 && order[0] == 1 && order[1] == 2 && order[2] == 3, "pool reverse disposal");
        writeAll(1, "aot primitives: ok\n", 19);
    }
}
    #if defined(__wasm__) || defined(wasm)
extern "C" __attribute__((export_name("_start"))) void testStart() {
    selfTest();
}
    #else
int main() {
    selfTest();
}
    #endif
#endif
