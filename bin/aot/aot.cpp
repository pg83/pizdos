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

namespace std {
    template <typename T>
    struct initializer_list {
        const T* first;
        usize count;

        const T* begin() const {
            return first;
        }

        const T* end() const {
            return first + count;
        }

        usize size() const {
            return count;
        }
    };
}

namespace {
    using Int = i64;
    using Uint = u64;
    template <typename T>
    using InitialValues = std::initializer_list<T>;

    template <typename A, typename B>
    struct Tuple2 {
        A v0{};
        B v1{};
    };

    template <typename A, typename B, typename C>
    struct Tuple3 {
        A v0{};
        B v1{};
        C v2{};
    };

    template <typename A, typename B, typename C, typename D>
    struct Tuple4 {
        A v0{};
        B v1{};
        C v2{};
        D v3{};
    };

    template <typename A, typename B, typename C, typename D, typename E>
    struct Tuple5 {
        A v0{};
        B v1{};
        C v2{};
        D v3{};
        E v4{};
    };

    static ObjPool* activePool;

    struct PoolScope {
        ObjPool* previous;

        PoolScope(ObjPool& pool)
            : previous(activePool)
        {
            activePool = &pool;
        }

        ~PoolScope() {
            activePool = previous;
        }
    };

    template <typename T, usize N>
    struct Array {
        T elements[N == 0 ? 1 : N]{};

        T& operator[](Int i) {
            require(i >= 0 && Uint(i) < N, "array index");
            return elements[i];
        }

        const T& operator[](Int i) const {
            require(i >= 0 && Uint(i) < N, "array index");
            return elements[i];
        }

        bool operator==(const Array& other) const {
            for (usize i = 0; i < N; ++i) {
                if (!(elements[i] == other.elements[i])) {
                    return false;
                }
            }
            return true;
        }
    };

    template <typename T, usize N>
    static Int length(const Array<T, N>&) {
        return N;
    }

    template <typename T, usize N>
    static Int capacity(const Array<T, N>&) {
        return N;
    }

    template <typename T>
    struct ArrayData {
        ObjPool* owner;
        T* borrowed = nullptr;
        SmallVec<T> small;
        PageVec<T> pages;
        Int count = 0;
        bool paged = false;

        ArrayData(ObjPool* pool)
            : owner(pool)
        {
        }

        void initialize(Int size) {
            require(size >= 0 && Uint(size) <= 0x7fffffff, "array too large");
            count = size;
            paged = Uint(size) * sizeof(T) > pageBytes - sizeof(Buffer::Header);
            if (paged) {
                pages.resize(usize(size));
            } else {
                small.resize(usize(size));
            }
            for (Int i = 0; i < size; ++i) {
                (*this)[i] = T{};
            }
        }

        T& operator[](Int i) {
            if (borrowed) {
                return borrowed[i];
            }
            return paged ? pages[usize(i)] : small[usize(i)];
        }
    };

    // A view can be copied freely. Its storage belongs to exactly one ObjPool.
    template <typename T>
    struct Slice {
        ArrayData<T>* array = nullptr;
        u32 offset = 0;
        u32 count = 0;
        u32 space = 0;

        T& operator[](Int i) const {
            require(i >= 0 && Uint(i) < count, "slice index");
            return (*array)[offset + usize(i)];
        }

        template <typename F>
        void visit(F f) const {
            for (u32 i = 0; i < count; ++i) {
                f((*this)[i]);
            }
        }
    };

    template <typename T>
    static Int length(Slice<T> s) {
        return s.count;
    }

    template <typename T>
    static Int capacity(Slice<T> s) {
        return s.space;
    }

    template <typename T>
    static bool isNil(Slice<T> s) {
        return !s.array;
    }

    template <typename T>
    static bool isNil(T* p) {
        return p == nullptr;
    }

    template <typename T>
    static Slice<T> makeSlice(Int size, Int space) {
        require(size >= 0 && space >= size && Uint(space) <= 0x7fffffff, "invalid slice size");
        auto* data = activePool->make<ArrayData<T>>(activePool);
        data->initialize(space);
        return {data, 0, u32(size), u32(space)};
    }

    template <typename T>
    static Slice<T> makeSlice(Int size) {
        return makeSlice<T>(size, size);
    }

    template <typename T>
    static Slice<T> borrowSlice(T* pointer, Int size) {
        auto* data = activePool->make<ArrayData<T>>(activePool);
        data->borrowed = pointer;
        data->count = size;
        return {data, 0, u32(size), u32(size)};
    }

    template <typename T>
    static Slice<T> subslice(Slice<T> s, Int low, Int high, Int space) {
        require(low >= 0 && high >= low && space >= high && Uint(space) <= s.space, "slice bounds");
        return {s.array, s.offset + u32(low), u32(high - low), u32(space - low)};
    }

    template <typename T>
    static Slice<T> subslice(Slice<T> s, Int low, Int high) {
        return subslice(s, low, high, s.space);
    }

    template <typename T, usize N>
    static Slice<T> subslice(Array<T, N>& a, Int low, Int high) {
        return subslice(borrowSlice(a.elements, N), low, high);
    }

    template <typename T>
    static void growSlice(Slice<T>& s, Int extra) {
        require(extra >= 0 && Uint(extra) <= 0x7fffffff - s.count, "slice size overflow");
        const Int need = s.count + extra;
        if (need <= s.space) {
            return;
        }
        ObjPool* owner = s.array ? s.array->owner : activePool;
        PoolScope scope(*owner);
        Int space = s.space ? s.space * 2 : 4;
        if (space < need) {
            space = need;
        }
        Slice<T> result = makeSlice<T>(s.count, space);
        for (Int i = 0; i < s.count; ++i) {
            result[i] = s[i];
        }
        s = result;
    }

    template <typename T>
    static Slice<T> sliceOf(InitialValues<T> values) {
        auto s = makeSlice<T>(values.size());
        usize i = 0;
        for (const T& value : values) {
            s[i++] = value;
        }
        return s;
    }

    template <typename T, typename... A>
    static Slice<T> appendValues(Slice<T> s, A... values) {
        growSlice(s, sizeof...(A));
        (((*s.array)[s.offset + s.count++] = T(values)), ...);
        return s;
    }

    template <typename T>
    static Slice<T> appendSlice(Slice<T> destination, Slice<T> source) {
        growSlice(destination, source.count);
        for (Int i = 0; i < source.count; ++i) {
            (*destination.array)[destination.offset + destination.count++] = source[i];
        }
        return destination;
    }

    template <typename T>
    static Int copySlice(Slice<T> destination, Slice<T> source) {
        Int n = destination.count < source.count ? destination.count : source.count;
        if (destination.array == source.array && destination.offset > source.offset) {
            for (Int i = n; i > 0; --i) {
                destination[i - 1] = source[i - 1];
            }
        } else {
            for (Int i = 0; i < n; ++i) {
                destination[i] = source[i];
            }
        }
        return n;
    }

    struct String {
        const u8* data = nullptr;
        usize count = 0;
        String() = default;

        template <usize N>
        constexpr String(const char (&text)[N])
            : data(reinterpret_cast<const u8*>(text))
            , count(N - 1)
        {
        }

        String(const u8* bytes, usize size)
            : data(bytes)
            , count(size)
        {
        }

        u8 operator[](Int i) const {
            require(i >= 0 && Uint(i) < count, "string index");
            return data[i];
        }

        bool operator==(String other) const {
            if (count != other.count) {
                return false;
            }
            for (usize i = 0; i < count; ++i) {
                if (data[i] != other.data[i]) {
                    return false;
                }
            }
            return true;
        }

        bool operator<(String other) const {
            usize n = count < other.count ? count : other.count;
            for (usize i = 0; i < n; ++i) {
                if (data[i] != other.data[i]) {
                    return data[i] < other.data[i];
                }
            }
            return count < other.count;
        }
    };

    static Int length(String s) {
        return s.count;
    }

    static String subslice(String s, Int low, Int high) {
        require(low >= 0 && high >= low && Uint(high) <= s.count, "string bounds");
        return {s.data + low, usize(high - low)};
    }

    static String asString(Slice<u8> s) {
        if (!s.count) {
            return {};
        }
        if (s.array->borrowed) {
            return {s.array->borrowed + s.offset, s.count};
        }
        if (!s.array->paged) {
            return {s.array->small.data() + s.offset, s.count};
        }
        require(s.count <= pageBytes, "large internal string");
        auto* p = static_cast<u8*>(activePool->allocate(s.count, 1));
        for (u32 i = 0; i < s.count; ++i) {
            p[i] = s[i];
        }
        return {p, s.count};
    }

    static String asString(String s) {
        return s;
    }

    static Slice<u8> asBytes(String s) {
        auto result = makeSlice<u8>(s.count);
        for (usize i = 0; i < s.count; ++i) {
            result[i] = s.data[i];
        }
        return result;
    }

    static Slice<u8> appendSlice(Slice<u8> out, String text) {
        growSlice(out, text.count);
        for (usize i = 0; i < text.count; ++i) {
            (*out.array)[out.offset + out.count++] = text.data[i];
        }
        return out;
    }

    static Int copySlice(Slice<u8> out, String text) {
        Int n = out.count < text.count ? out.count : text.count;
        for (Int i = 0; i < n; ++i) {
            out[i] = text.data[i];
        }
        return n;
    }

    static String operator+(String a, String b) {
        require(a.count + b.count <= pageBytes - 32, "large internal string");
        auto* p = static_cast<u8*>(activePool->allocate(a.count + b.count + 1, 1));
        copyBytes(p, a.data, a.count);
        copyBytes(p + a.count, b.data, b.count);
        p[a.count + b.count] = 0;
        return {p, a.count + b.count};
    }

    [[noreturn]] static void compilerError(String message) {
        writeAll(2, message.data, message.count);
        writeAll(2, "\n", 1);
        wasiProcExit(134);
    }

    template <typename T, typename U>
    static T convert(U value) {
        return static_cast<T>(value);
    }

    template <typename T, typename U>
    static T shiftLeft(T value, U amount) {
        if (Uint(amount) >= sizeof(T) * 8) {
            return T(0);
        }
        using Unsigned = u64;
        return T(Unsigned(value) << amount);
    }

    template <typename T, typename U>
    static T shiftRight(T value, U amount) {
        if (Uint(amount) >= sizeof(T) * 8) {
            return value < T(0) ? T(-1) : T(0);
        }
        return T(value >> amount);
    }

    template <typename T, typename U>
    static auto minimum(T a, U b) {
        return a < b ? a : b;
    }

    template <typename T, typename U>
    static auto maximum(T a, U b) {
        return a > b ? a : b;
    }

    template <typename T, typename... A>
    static auto minimum(T a, T b, A... rest) {
        return minimum(minimum(a, b), rest...);
    }

    template <typename T, typename... A>
    static auto maximum(T a, T b, A... rest) {
        return maximum(maximum(a, b), rest...);
    }

    static u64 combineHash(u64 a, u64 b) {
        return (a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2)));
    }

    template <typename T>
    static u64 hashValue(T value) {
        u64 x = u64(value);
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }

    static u64 hashValue(String s) {
        u64 result = 14695981039346656037ULL;
        for (usize i = 0; i < s.count; ++i) {
            result = (result ^ s.data[i]) * 1099511628211ULL;
        }
        return result;
    }

    template <typename T, usize N>
    static u64 hashValue(const Array<T, N>& a) {
        u64 result = 0;
        for (usize i = 0; i < N; ++i) {
            result = combineHash(result, hashValue(a[i]));
        }
        return result;
    }

    template <typename K, typename V>
    struct MapEntry {
        K key{};
        V value{};
        u8 state = 0;
    };

    template <typename K, typename V>
    struct MapData {
        ArrayData<MapEntry<K, V>> buckets;
        Int count = 0;
        Int occupied = 0;

        MapData(ObjPool* pool)
            : buckets(pool)
        {
        }
    };

    template <typename K, typename V>
    struct Map {
        MapData<K, V>* data = nullptr;
        V operator[](K key) const;
    };

    template <typename K, typename V>
    static Map<K, V> makeMap(Int = 0) {
        return {activePool->make<MapData<K, V>>(activePool)};
    }

    template <typename K, typename V>
    static bool isNil(Map<K, V> m) {
        return !m.data;
    }

    template <typename K, typename V>
    static Int length(Map<K, V> m) {
        return m.data ? m.data->count : 0;
    }

    template <typename K, typename V>
    static Int mapSlots(Map<K, V> m) {
        return m.data ? m.data->buckets.count : 0;
    }

    template <typename K, typename V>
    static bool mapOccupied(Map<K, V> m, Int at) {
        return m.data->buckets[at].state == 1;
    }

    template <typename K, typename V>
    static K mapKey(Map<K, V> m, Int at) {
        return m.data->buckets[at].key;
    }

    template <typename K, typename V>
    static V mapValue(Map<K, V> m, Int at) {
        return m.data->buckets[at].value;
    }

    template <typename K, typename V>
    static Int findMap(Map<K, V> m, const K& key) {
        if (!m.data || !m.data->buckets.count) {
            return -1;
        }
        Int mask = m.data->buckets.count - 1;
        Int at = Int(hashValue(key) & mask);
        for (Int probe = 0; probe <= mask; ++probe, at = (at + 1) & mask) {
            auto& entry = m.data->buckets[at];
            if (entry.state == 0) {
                return -1;
            }
            if (entry.state == 1 && entry.key == key) {
                return at;
            }
        }
        return -1;
    }

    template <typename K, typename V>
    V Map<K, V>::operator[](K key) const {
        Int at = findMap(*this, key);
        return at < 0 ? V{} : data->buckets[at].value;
    }

    template <typename K, typename V>
    static Tuple2<V, bool> lookupMap(Map<K, V> m, K key) {
        Int at = findMap(m, key);
        return {at < 0 ? V{} : m.data->buckets[at].value, at >= 0};
    }

    template <typename K, typename V>
    static void insertMap(Map<K, V> m, K key, V value) {
        Int mask = m.data->buckets.count - 1;
        Int at = Int(hashValue(key) & mask);
        for (;; at = (at + 1) & mask) {
            auto& entry = m.data->buckets[at];
            if (entry.state != 1 || entry.key == key) {
                if (entry.state != 1) {
                    ++m.data->count;
                    if (entry.state == 0) {
                        ++m.data->occupied;
                    }
                }
                entry = {key, value, 1};
                return;
            }
        }
    }

    template <typename K, typename V, typename A, typename B>
    static void setMap(Map<K, V> m, A keyValue, B valueValue) {
        require(m.data != nullptr, "nil map assignment");
        K key = K(keyValue);
        V value = V(valueValue);
        Int at = findMap(m, key);
        if (at >= 0) {
            m.data->buckets[at].value = value;
            return;
        }
        if (!m.data->buckets.count || m.data->occupied * 4 >= m.data->buckets.count * 3) {
            Int size = m.data->buckets.count ? m.data->buckets.count * 2 : 8;
            auto old = move(m.data->buckets);
            new (&m.data->buckets) ArrayData<MapEntry<K, V>>(old.owner);
            m.data->buckets.initialize(size);
            m.data->count = 0;
            m.data->occupied = 0;
            for (Int i = 0; i < old.count; ++i) {
                auto e = old[i];
                if (e.state == 1) {
                    insertMap(m, e.key, e.value);
                }
            }
        }
        insertMap(m, key, value);
    }

    template <typename K, typename V>
    static Map<K, V> mapOf(InitialValues<Tuple2<K, V>> entries) {
        auto map = makeMap<K, V>();
        for (auto e : entries) {
            setMap(map, e.v0, e.v1);
        }
        return map;
    }

    template <typename K, typename V>
    static void eraseMap(Map<K, V> m, K key) {
        Int at = findMap(m, key);
        if (at >= 0) {
            m.data->buckets[at].state = 2;
            --m.data->count;
        }
    }

    template <typename K, typename V>
    static void clearMap(Map<K, V> m) {
        if (!m.data) {
            return;
        }
        for (Int i = 0; i < m.data->buckets.count; ++i) {
            m.data->buckets[i] = {};
        }
        m.data->count = 0;
        m.data->occupied = 0;
    }

    static bool utf8Valid(Slice<u8> text) {
        for (Int i = 0; i < text.count;) {
            u32 value = text[i++];
            if (value < 0x80) {
                continue;
            }
            Int remaining;
            u32 minimum;
            if (value >= 0xc2 && value <= 0xdf) {
                remaining = 1;
                minimum = 0x80;
                value &= 0x1f;
            } else if (value >= 0xe0 && value <= 0xef) {
                remaining = 2;
                minimum = 0x800;
                value &= 0x0f;
            } else if (value >= 0xf0 && value <= 0xf4) {
                remaining = 3;
                minimum = 0x10000;
                value &= 7;
            } else {
                return false;
            }
            if (remaining > text.count - i) {
                return false;
            }
            for (; remaining != 0; --remaining) {
                u8 byte = text[i++];
                if ((byte & 0xc0) != 0x80) {
                    return false;
                }
                value = (value << 6) | (byte & 0x3f);
            }
            if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
                return false;
            }
        }
        return true;
    }
}

namespace {
    // Compiler data is pool owned; slice and map fields are non-owning views.

    struct Data;

    struct Elem;

    struct Export;

    struct Func;

    struct FuncType;

    struct Global;

    struct Import;

    struct Limits;

    struct Memory;

    struct Module;

    struct Reader;

    struct Table;

    using ValType = u8;

    struct Func {
        u32 typ{};
        bool imported{};
        bool returnsTwice{};
        String name{};
        Slice<u8> body{};
        Int offset{};
    };

    struct Limits {
        u64 min{};
        u64 max{};
        bool hasMax{};
        bool is64{};
        bool shared{};
    };

    static bool operator==(const Limits& a, const Limits& b) {
        return a.min == b.min && a.max == b.max && a.hasMax == b.hasMax && a.is64 == b.is64 && a.shared == b.shared;
    }

    static u64 hashValue(const Limits& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.min));
        result = combineHash(result, hashValue(value.max));
        result = combineHash(result, hashValue(value.hasMax));
        result = combineHash(result, hashValue(value.is64));
        result = combineHash(result, hashValue(value.shared));
        return result;
    }

    struct Elem {
        u8 mode{};
        u32 table{};
        Slice<u8> offset{};
        ValType typ{};
        Slice<Slice<u8>> exprs{};
    };

    struct Reader {
        Slice<u8> data{};
        Int pos{};
    };

    struct Table {
        ValType elem{};
        Limits limits{};
        bool imported{};
    };

    static bool operator==(const Table& a, const Table& b) {
        return a.elem == b.elem && a.limits == b.limits && a.imported == b.imported;
    }

    static u64 hashValue(const Table& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.elem));
        result = combineHash(result, hashValue(value.limits));
        result = combineHash(result, hashValue(value.imported));
        return result;
    }

    struct Global {
        ValType typ{};
        bool mutableValue{};
        bool imported{};
        Slice<u8> init{};
    };

    struct Module {
        u64 memoryTail{};
        bool tailRanges{};
        bool tailProfile{};
        Slice<FuncType> types{};
        Slice<Import> imports{};
        Slice<Func> funcs{};
        Slice<Table> tables{};
        Slice<Memory> mems{};
        Slice<Global> globals{};
        Slice<Export> exports{};
        i64 start{};
        Slice<Elem> elems{};
        Slice<Data> datas{};
        i64 dataCount{};
        Int importedFuncs{};
        Int importedTables{};
        Int importedMems{};
        Int importedGlobals{};
    };

    struct Import {
        String module{};
        String name{};
        u8 kind{};
        u32 index{};
    };

    static bool operator==(const Import& a, const Import& b) {
        return a.module == b.module && a.name == b.name && a.kind == b.kind && a.index == b.index;
    }

    static u64 hashValue(const Import& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.module));
        result = combineHash(result, hashValue(value.name));
        result = combineHash(result, hashValue(value.kind));
        result = combineHash(result, hashValue(value.index));
        return result;
    }

    struct FuncType {
        Slice<ValType> params{};
        Slice<ValType> results{};
    };

    struct Export {
        String name{};
        u8 kind{};
        u32 index{};
    };

    static bool operator==(const Export& a, const Export& b) {
        return a.name == b.name && a.kind == b.kind && a.index == b.index;
    }

    static u64 hashValue(const Export& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.name));
        result = combineHash(result, hashValue(value.kind));
        result = combineHash(result, hashValue(value.index));
        return result;
    }

    struct Data {
        u8 mode{};
        u32 memory{};
        Slice<u8> offset{};
        Slice<u8> bytes{};
    };

    struct Memory {
        Limits limits{};
        bool imported{};
    };

    static bool operator==(const Memory& a, const Memory& b) {
        return a.limits == b.limits && a.imported == b.imported;
    }

    static u64 hashValue(const Memory& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.limits));
        result = combineHash(result, hashValue(value.imported));
        return result;
    }

    static void moduleDataSection(Module* m, Reader* r);

    static void moduleElemSection(Module* m, Reader* r);

    static void moduleExportSection(Module* m, Reader* r);

    static void moduleImportSection(Module* m, Reader* r);

    static void moduleTypeSection(Module* m, Reader* r);

    static u8 readerByte(Reader* r);

    static Slice<u8> readerBytes(Reader* r, Int n);

    static Slice<u8> readerConstExpr(Reader* r);

    static bool readerEof(Reader* r);

    static u32 readerF32(Reader* r);

    static u64 readerLeb(Reader* r, Uint bits);

    static Limits readerLimits(Reader* r, bool memory);

    static String readerName(Reader* r);

    static ValType readerRefType(Reader* r);

    static i32 readerS32(Reader* r);

    static i64 readerS64(Reader* r);

    static i64 readerSleb(Reader* r, Uint bits);

    static u32 readerU32(Reader* r);

    static ValType readerValType(Reader* r);

    static Slice<u8> appendLeb(Slice<u8> b, u64 v);

    static Module* decodeModule(Slice<u8> data);

    static bool globalMutability(Reader* r);

    static Int order(u8 id);

    static Slice<u8> refFuncExpr(u32 index);

    static bool returnsTwice(String name);

    static void moduleCustom(Module*, Reader*);

    static void moduleDataSection(Module* m, Reader* r) {
        u32 n = readerU32(r);
        {
            u32 i = u32(0LL);
            for (; (i < n); i++) {
                u32 flags = readerU32(r);
                Data d = Data{};
                {
                    auto switch1 = flags;
                    if ((switch1 == u32(0LL))) {
                        (d).offset = readerConstExpr(r);
                    } else if ((switch1 == u32(1LL))) {
                        (d).mode = u8(1LL);
                    } else if ((switch1 == u32(2LL))) {
                        (d).memory = readerU32(r);
                        (d).offset = readerConstExpr(r);
                    } else {
                        compilerError(String("malformed data segment kind %d"));
                    }
                switchEnd2:;
                }
                (d).bytes = readerBytes(r, convert<Int>(readerU32(r)));
                (m)->datas = appendValues((m)->datas, d);
            }
        }
    }

    static void moduleElemSection(Module* m, Reader* r) {
        u32 n = readerU32(r);
        {
            u32 i = u32(0LL);
            for (; (i < n); i++) {
                u32 flags = readerU32(r);
                if ((flags > u32(7LL))) {
                    compilerError(String("malformed elements segment kind %d"));
                }
                Elem e = Elem{{}, {}, {}, ValType(112LL), {}};
                {
                    if (((flags & u32(1LL)) == u32(0LL))) {
                        (e).mode = u8(0LL);
                    } else if (((flags & u32(2LL)) == u32(0LL))) {
                        (e).mode = u8(1LL);
                    } else {
                        (e).mode = u8(2LL);
                    }
                switchEnd4:;
                }
                if (((flags & u32(1LL)) == u32(0LL))) {
                    if (((flags & u32(2LL)) != u32(0LL))) {
                        (e).table = readerU32(r);
                    }
                    (e).offset = readerConstExpr(r);
                }
                bool exprs = ((flags & u32(4LL)) != u32(0LL));
                if (((flags & u32(3LL)) != u32(0LL))) {
                    if (exprs) {
                        (e).typ = readerRefType(r);
                    } else {
                        u8 kind = readerByte(r);
                        if ((kind != u8(0LL))) {
                            compilerError(String("malformed element kind %d"));
                        }
                    }
                }
                u32 count = readerU32(r);
                {
                    u32 j = u32(0LL);
                    for (; (j < count); j++) {
                        if (exprs) {
                            (e).exprs = appendValues((e).exprs, readerConstExpr(r));
                        } else {
                            (e).exprs = appendValues((e).exprs, refFuncExpr(readerU32(r)));
                        }
                    }
                }
                (m)->elems = appendValues((m)->elems, e);
            }
        }
    }

    static void moduleExportSection(Module* m, Reader* r) {
        u32 n = readerU32(r);
        Map<String, bool> seen = mapOf<String, bool>({});
        {
            u32 i = u32(0LL);
            for (; (i < n); i++) {
                Export e = Export{readerName(r), readerByte(r), readerU32(r)};
                if (((e).kind > u8(3LL))) {
                    compilerError(String("malformed export kind %d"));
                }
                if (seen[(e).name]) {
                    compilerError(String("duplicate export name %q"));
                }
                setMap(seen, (e).name, true);
                (m)->exports = appendValues((m)->exports, e);
            }
        }
    }

    static void moduleImportSection(Module* m, Reader* r) {
        u32 n = readerU32(r);
        {
            u32 i = u32(0LL);
            for (; (i < n); i++) {
                Import imp = Import{readerName(r), readerName(r), readerByte(r), {}};
                {
                    auto switch5 = (imp).kind;
                    if ((switch5 == u8(0LL))) {
                        (imp).index = convert<u32>(length((m)->funcs));
                        (m)->funcs = appendValues((m)->funcs, Func{readerU32(r), true, returnsTwice((imp).name), {}, {}, {}});
                        (m)->importedFuncs++;
                    } else if ((switch5 == u8(1LL))) {
                        (imp).index = convert<u32>(length((m)->tables));
                        ValType elem = readerRefType(r);
                        (m)->tables = appendValues((m)->tables, Table{elem, readerLimits(r, false), true});
                        (m)->importedTables++;
                    } else if ((switch5 == u8(2LL))) {
                        (imp).index = convert<u32>(length((m)->mems));
                        (m)->mems = appendValues((m)->mems, Memory{readerLimits(r, true), true});
                        (m)->importedMems++;
                    } else if ((switch5 == u8(3LL))) {
                        (imp).index = convert<u32>(length((m)->globals));
                        Global g = Global{readerValType(r), {}, true, {}};
                        (g).mutableValue = globalMutability(r);
                        (m)->globals = appendValues((m)->globals, g);
                        (m)->importedGlobals++;
                    } else {
                        compilerError(String("malformed import kind %d"));
                    }
                switchEnd6:;
                }
                (m)->imports = appendValues((m)->imports, imp);
            }
        }
    }

    static void moduleTypeSection(Module* m, Reader* r) {
        u32 n = readerU32(r);
        {
            u32 i = u32(0LL);
            for (; (i < n); i++) {
                {
                    u8 form = readerByte(r);
                    if ((form != u8(96LL))) {
                        compilerError(String("malformed function type form 0x%02x"));
                    }
                }
                FuncType t = {};
                u32 np = readerU32(r);
                {
                    u32 j = u32(0LL);
                    for (; (j < np); j++) {
                        (t).params = appendValues((t).params, readerValType(r));
                    }
                }
                u32 nr = readerU32(r);
                {
                    u32 j = u32(0LL);
                    for (; (j < nr); j++) {
                        (t).results = appendValues((t).results, readerValType(r));
                    }
                }
                (m)->types = appendValues((m)->types, t);
            }
        }
    }

    static u8 readerByte(Reader* r) {
        if (((r)->pos >= length((r)->data))) {
            compilerError(String("unexpected end at offset %d"));
        }
        u8 b = (r)->data[(r)->pos];
        (r)->pos++;
        return b;
    }

    static Slice<u8> readerBytes(Reader* r, Int n) {
        if (((n < Int(0LL)) || (n > (length((r)->data) - (r)->pos)))) {
            compilerError(String("unexpected end at offset %d: need %d bytes"));
        }
        Slice<u8> b = subslice((r)->data, (r)->pos, ((r)->pos + n));
        (r)->pos += n;
        return b;
    }

    static Slice<u8> readerConstExpr(Reader* r) {
        Int start = (r)->pos;
        {
            for (; true;) {
                u8 op = readerByte(r);
                {
                    auto switch7 = op;
                    if ((switch7 == u8(11LL))) {
                        return subslice((r)->data, start, (r)->pos);
                    } else if ((switch7 == u8(65LL))) {
                        readerS32(r);
                    } else if ((switch7 == u8(66LL))) {
                        readerS64(r);
                    } else if ((switch7 == u8(67LL))) {
                        readerBytes(r, Int(4LL));
                    } else if ((switch7 == u8(68LL))) {
                        readerBytes(r, Int(8LL));
                    } else if ((switch7 == u8(35LL)) || (switch7 == u8(210LL))) {
                        readerU32(r);
                    } else if ((switch7 == u8(208LL))) {
                        readerRefType(r);
                    } else if ((switch7 == u8(106LL)) || (switch7 == u8(107LL)) || (switch7 == u8(108LL)) || (switch7 == u8(124LL)) || (switch7 == u8(125LL)) || (switch7 == u8(126LL))) {
                    } else if ((switch7 == u8(253LL))) {
                        if ((readerU32(r) != u32(12LL))) {
                            compilerError(String("constant expression required at offset %d"));
                        }
                        readerBytes(r, Int(16LL));
                    } else {
                        compilerError(String("constant expression required: opcode 0x%02x at offset %d"));
                    }
                switchEnd8:;
                }
            }
        }
    }

    static bool readerEof(Reader* r) {
        return ((r)->pos >= length((r)->data));
    }

    static u32 readerF32(Reader* r) {
        Slice<u8> b = readerBytes(r, Int(4LL));
        return (((convert<u32>(b[Int(0LL)]) | shiftLeft(convert<u32>(b[Int(1LL)]), Int(8LL))) | shiftLeft(convert<u32>(b[Int(2LL)]), Int(16LL))) | shiftLeft(convert<u32>(b[Int(3LL)]), Int(24LL)));
    }

    static u64 readerLeb(Reader* r, Uint bits) {
        u64 result = {};
        Uint shift = {};
        {
            for (; true;) {
                u8 b = readerByte(r);
                if ((((shift + Uint(7LL)) >= bits) && (shiftRight((b & u8(127LL)), ((bits - shift))) != u8(0LL)))) {
                    compilerError(String("integer too large at offset %d"));
                }
                result |= shiftLeft(convert<u64>((b & u8(127LL))), shift);
                if (((b & u8(128LL)) == u8(0LL))) {
                    return result;
                }
                shift += Uint(7LL);
                if ((shift >= bits)) {
                    compilerError(String("integer representation too long at offset %d"));
                }
            }
        }
    }

    static Limits readerLimits(Reader* r, bool memory) {
        u8 flags = readerByte(r);
        Limits l = {};
        if (((flags > u8(7LL)) || (((!(memory)) && (flags > u8(1LL)))))) {
            compilerError(String("integer too large: limits flags 0x%02x"));
        }
        (l).hasMax = ((flags & u8(1LL)) != u8(0LL));
        (l).shared = ((flags & u8(2LL)) != u8(0LL));
        (l).is64 = ((flags & u8(4LL)) != u8(0LL));
        if ((l).is64) {
            compilerError(String("unsupported: memory64"));
        }
        Uint bits = Uint(32LL);
        if ((l).is64) {
            bits = Uint(64LL);
        }
        (l).min = readerLeb(r, bits);
        if ((l).hasMax) {
            (l).max = readerLeb(r, bits);
        }
        return l;
    }

    static String readerName(Reader* r) {
        u32 n = readerU32(r);
        Slice<u8> b = readerBytes(r, convert<Int>(n));
        if ((!(utf8Valid(b)))) {
            compilerError(String("malformed UTF-8 encoding at offset %d"));
        }
        return asString(b);
    }

    static ValType readerRefType(Reader* r) {
        ValType t = convert<ValType>(readerByte(r));
        if (((t != ValType(112LL)) && (t != ValType(111LL)))) {
            compilerError(String("malformed reference type 0x%02x at offset %d"));
        }
        return t;
    }

    static i32 readerS32(Reader* r) {
        return convert<i32>(readerSleb(r, Uint(32LL)));
    }

    static i64 readerS64(Reader* r) {
        return readerSleb(r, Uint(64LL));
    }

    static i64 readerSleb(Reader* r, Uint bits) {
        i64 result = {};
        Uint shift = {};
        {
            for (; true;) {
                u8 b = readerByte(r);
                if (((shift + Uint(7LL)) >= bits)) {
                    i8 rest = shiftRight(convert<i8>(shiftLeft(b, Int(1LL))), ((bits - shift)));
                    if ((((b & u8(128LL)) != u8(0LL)) || (((rest != i8(0LL)) && (rest != i8(-1LL)))))) {
                        if (((b & u8(128LL)) != u8(0LL))) {
                            compilerError(String("integer representation too long at offset %d"));
                        }
                        compilerError(String("integer too large at offset %d"));
                    }
                }
                result |= shiftLeft(convert<i64>((b & u8(127LL))), shift);
                shift += Uint(7LL);
                if (((b & u8(128LL)) == u8(0LL))) {
                    if (((shift < Uint(64LL)) && ((b & u8(64LL)) != u8(0LL)))) {
                        result |= shiftLeft(i64(-1LL), shift);
                    }
                    return result;
                }
            }
        }
    }

    static u32 readerU32(Reader* r) {
        if ((((r)->pos < length((r)->data)) && ((r)->data[(r)->pos] < u8(128LL)))) {
            u8 b = (r)->data[(r)->pos];
            (r)->pos++;
            return convert<u32>(b);
        }
        return convert<u32>(readerLeb(r, Uint(32LL)));
    }

    static ValType readerValType(Reader* r) {
        ValType t = convert<ValType>(readerByte(r));
        {
            auto switch9 = t;
            if ((switch9 == ValType(127LL)) || (switch9 == ValType(126LL)) || (switch9 == ValType(125LL)) || (switch9 == ValType(124LL)) || (switch9 == ValType(123LL)) || (switch9 == ValType(112LL)) || (switch9 == ValType(111LL))) {
                return t;
            }
        switchEnd10:;
        }
        compilerError(String("malformed value type 0x%02x at offset %d"));
        return ValType(0LL);
    }

    static Slice<u8> appendLeb(Slice<u8> b, u64 v) {
        {
            for (; true;) {
                u8 c = convert<u8>((v & u64(127LL)));
                v = shiftRight(v, Int(7LL));
                if ((v == u64(0LL))) {
                    return appendValues(b, c);
                }
                b = appendValues(b, (c | u8(128LL)));
            }
        }
    }

    static Module* decodeModule(Slice<u8> data) {
        Reader* r = activePool->make<Reader>(Reader{data, {}});
        Module* m = activePool->make<Module>(Module{{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, i64(-1LL), {}, {}, i64(-1LL), {}, {}, {}, {}});
        if ((asString(readerBytes(r, Int(4LL))) != String("\000asm"))) {
            compilerError(String("magic header not detected"));
        }
        if ((readerF32(r) != u32(1LL))) {
            compilerError(String("unknown binary version"));
        }
        Slice<u32> funcTypes = {};
        Int codes = {};
        u8 last = u8(0LL);
        {
            for (; (!(readerEof(r)));) {
                u8 id = readerByte(r);
                u32 size = readerU32(r);
                Reader* body = activePool->make<Reader>(Reader{readerBytes(r, convert<Int>(size)), {}});
                Int base = ((r)->pos - convert<Int>(size));
                if ((id != u8(0LL))) {
                    if ((order(id) <= order(last))) {
                        compilerError(String("unexpected content after last section"));
                    }
                    last = id;
                }
                {
                    auto switch11 = id;
                    if ((switch11 == u8(0LL))) {
                        moduleCustom(m, body);
                    } else if ((switch11 == u8(1LL))) {
                        moduleTypeSection(m, body);
                    } else if ((switch11 == u8(2LL))) {
                        moduleImportSection(m, body);
                    } else if ((switch11 == u8(3LL))) {
                        u32 n = readerU32(body);
                        {
                            u32 i = u32(0LL);
                            for (; (i < n); i++) {
                                funcTypes = appendValues(funcTypes, readerU32(body));
                            }
                        }
                    } else if ((switch11 == u8(4LL))) {
                        u32 n = readerU32(body);
                        {
                            u32 i = u32(0LL);
                            for (; (i < n); i++) {
                                ValType elem = readerRefType(body);
                                (m)->tables = appendValues((m)->tables, Table{elem, readerLimits(body, false), {}});
                            }
                        }
                    } else if ((switch11 == u8(5LL))) {
                        u32 n = readerU32(body);
                        {
                            u32 i = u32(0LL);
                            for (; (i < n); i++) {
                                (m)->mems = appendValues((m)->mems, Memory{readerLimits(body, true), {}});
                            }
                        }
                    } else if ((switch11 == u8(6LL))) {
                        u32 n = readerU32(body);
                        {
                            u32 i = u32(0LL);
                            for (; (i < n); i++) {
                                Global g = Global{readerValType(body), {}, {}, {}};
                                (g).mutableValue = globalMutability(body);
                                (g).init = readerConstExpr(body);
                                (m)->globals = appendValues((m)->globals, g);
                            }
                        }
                    } else if ((switch11 == u8(7LL))) {
                        moduleExportSection(m, body);
                    } else if ((switch11 == u8(8LL))) {
                        (m)->start = convert<i64>(readerU32(body));
                    } else if ((switch11 == u8(9LL))) {
                        moduleElemSection(m, body);
                    } else if ((switch11 == u8(10LL))) {
                        Int n = convert<Int>(readerU32(body));
                        if ((n != length(funcTypes))) {
                            compilerError(String("function and code section have inconsistent lengths"));
                        }
                        {
                            Int i = Int(0LL);
                            for (; (i < n); i++) {
                                Int size = convert<Int>(readerU32(body));
                                Int offset = (base + (body)->pos);
                                (m)->funcs = appendValues((m)->funcs, Func{funcTypes[i], {}, {}, {}, readerBytes(body, size), offset});
                            }
                        }
                        codes = n;
                    } else if ((switch11 == u8(11LL))) {
                        moduleDataSection(m, body);
                    } else if ((switch11 == u8(12LL))) {
                        (m)->dataCount = convert<i64>(readerU32(body));
                    } else {
                        compilerError(String("malformed section id %d"));
                    }
                switchEnd12:;
                }
                if ((!(readerEof(body)))) {
                    compilerError(String("section size mismatch in section %d"));
                }
            }
        }
        if ((codes != length(funcTypes))) {
            compilerError(String("function and code section have inconsistent lengths"));
        }
        if ((((m)->dataCount >= i64(0LL)) && (convert<Int>((m)->dataCount) != length((m)->datas)))) {
            compilerError(String("data count and data section have inconsistent lengths"));
        }
        {
            auto range13 = (m)->funcs;
            for (Int index14 = 0, end = length(range13); index14 < end; ++index14) {
                Int i = index14;
                if ((convert<Int>(((m)->funcs[i]).typ) >= length((m)->types))) {
                    compilerError(String("unknown type %d"));
                }
            }
        }
        if ((length((m)->mems) > Int(1LL))) {
            compilerError(String("unsupported: %d memories"));
        }
        {
            auto range15 = (m)->datas;
            for (Int index16 = 0, end = length(range15); index16 < end; ++index16) {
                Int i = index16;
                Data d = range15[index16];
                if (((d).memory != u32(0LL))) {
                    compilerError(String("unsupported: data segment %d of memory %d"));
                }
            }
        }
        return m;
    }

    static bool globalMutability(Reader* r) {
        u8 b = readerByte(r);
        if ((b > u8(1LL))) {
            compilerError(String("malformed mutability 0x%02x"));
        }
        return (b == u8(1LL));
    }

    static Int order(u8 id) {
        {
            auto switch17 = id;
            if ((switch17 == u8(12LL))) {
                return Int(10LL);
            } else if ((switch17 == u8(10LL)) || (switch17 == u8(11LL))) {
                return (convert<Int>(id) + Int(1LL));
            }
        switchEnd18:;
        }
        return convert<Int>(id);
    }

    static Slice<u8> refFuncExpr(u32 index) {
        Slice<u8> expr = sliceOf<u8>({u8(210LL)});
        expr = appendLeb(expr, convert<u64>(index));
        return appendValues(expr, u8(11LL));
    }

    static bool returnsTwice(String name) {
        {
            auto switch19 = name;
            if ((switch19 == String("setjmp")) || (switch19 == String("_setjmp")) || (switch19 == String("sigsetjmp")) || (switch19 == String("vfork")) || (switch19 == String("fork"))) {
                return true;
            }
        switchEnd20:;
        }
        return false;
    }

}

namespace {
    static bool optionalU32(Reader& r, u32& result) {
        result = 0;
        for (u32 shift = 0; shift < 35 && r.pos < length(r.data); shift += 7) {
            u8 b = r.data[r.pos++];
            if (shift == 28 && b > 15) {
                return false;
            }
            result |= u32(b & 127) << shift;
            if (!(b & 128)) {
                return true;
            }
        }
        return false;
    }

    static void readOptionalNames(Module* module, Reader r) {
        while (r.pos < length(r.data)) {
            u8 id = r.data[r.pos++];
            u32 size;
            if (!optionalU32(r, size) || size > length(r.data) - r.pos) {
                return;
            }
            Reader part{subslice(r.data, r.pos, r.pos + size), 0};
            r.pos += size;
            if (id != 1) {
                continue;
            }
            u32 count;
            if (!optionalU32(part, count)) {
                return;
            }
            for (u32 i = 0; i < count; ++i) {
                u32 index, bytes;
                if (!optionalU32(part, index) || !optionalU32(part, bytes) || bytes > length(part.data) - part.pos) {
                    return;
                }
                auto name = subslice(part.data, part.pos, part.pos + bytes);
                part.pos += bytes;
                if (!utf8Valid(name)) {
                    return;
                }
                if (index < length(module->funcs)) {
                    module->funcs[index].name = asString(name);
                }
            }
        }
    }

    static void moduleCustom(Module* module, Reader* reader) {
        String name = readerName(reader);
        if (name == String("name")) {
            readOptionalNames(module, *reader);
        }
        reader->pos = length(reader->data);
    }

    static Blob readInput() {
        PageVec<u8> bytes;
        u8 buffer[pageBytes];
        for (;;) {
            IoVec vector{buffer, sizeof(buffer)};
            usize count = 0;
            require(wasiFdRead(0, &vector, 1, &count) == 0, "stdin read failed");
            if (count == 0) {
                break;
            }
            bytes.append(buffer, count);
        }
        auto* input = static_cast<u8*>(growPages((bytes.size() + pageBytes - 1) / pageBytes + (bytes.empty() ? 1 : 0)));
        for (usize i = 0; i < bytes.size(); ++i) {
            input[i] = bytes[i];
        }
        return {input, bytes.size()};
    }

    static void releaseBlob(Blob blob) {
#if defined(__wasm__) || defined(wasm)
        for (usize i = 0; i < blob.size; i += pageBytes) {
            freePage(const_cast<u8*>(blob.data + i));
        }
#else
        ::free(const_cast<u8*>(blob.data));
#endif
    }

    static void writeNumber(u64 value) {
        char buffer[24];
        usize at = sizeof(buffer);
        do {
            buffer[--at] = char('0' + value % 10);
            value /= 10;
        } while (value);
        writeAll(1, buffer + at, sizeof(buffer) - at);
    }

    static void decodeCommand() {
        Blob input = readInput();
        ObjPool pool;
        PoolScope scope(pool);
        Module* module = decodeModule(borrowSlice(const_cast<u8*>(input.data), input.size));
        writeNumber(length(module->types));
        writeAll(1, " ", 1);
        writeNumber(length(module->funcs));
        writeAll(1, " ", 1);
        writeNumber(length(module->datas));
        writeAll(1, "\n", 1);
        releaseBlob(input);
    }
}

#if defined(AOT_DECODE_TEST)
    #if defined(__wasm__) || defined(wasm)
extern "C" __attribute__((export_name("_start"))) void start() {
    decodeCommand();
}
    #else
int main() {
    decodeCommand();
    return 0;
}
    #endif
#endif
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
