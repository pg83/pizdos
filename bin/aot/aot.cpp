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
    #include <sys/mman.h>
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
        __builtin_memcpy(destination, source, size);
    }

    static void zeroBytes(void* destination, usize size) {
        __builtin_memset(destination, 0, size);
    }

    static void moveBytes(void* destination, const void* source, usize size) {
        __builtin_memmove(destination, source, size);
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

    static constexpr usize heapSegmentShift = 21;
    static constexpr usize heapSegmentBytes = usize(1) << heapSegmentShift;
    static constexpr usize heapPagesPerSegment = heapSegmentBytes / pageBytes;
    static constexpr usize heapClassCount = 48;
    static constexpr u16 heapFreePage = 0xffff;
    static constexpr u16 heapUnavailablePage = 0xfffe;
    static constexpr u32 heapNoSegment = 0;
#if defined(__wasm__) || defined(wasm)
    static constexpr usize heapSegmentLimit = usize(1) << 11;
#else
    static constexpr usize heapSegmentLimit = usize(1) << 15;
    static constexpr uptr heapNativeBase = uptr(0x2000) << 32;
#endif

    struct HeapSlot {
        HeapSlot* next;
    };

    struct HeapPage {
        HeapSlot* free;
        HeapPage* prev;
        HeapPage* next;
        u32 live;
        u16 cls;
    };

    struct HeapSegment {
        u32 freeMask;
        u32 prev;
        u32 next;
        bool linked;
        bool registered;
    };

    struct HeapClass {
        HeapSlot* free;
        uptr bump;
        uptr end;
        usize size;
        HeapPage* current;
        HeapPage* partial;
    };

    // Adapted from trustme/bin/rustc/malloc.cpp. All ordinary allocations
    // fit in one page, so runs and large-object headers are unnecessary.
    // Metadata is outside payload pages, including the 64 KiB size class.
    struct Heap {
        static usize classIndex(usize size);
        static usize classSize(usize index);
        HeapPage* pageOf(const void* object);
        uptr pageAddress(const HeapPage* page);
        void linkSegment(u32 index);
        void unlinkSegment(u32 index);
        void registerSegment(u32 index);
        void mapSegment();
        HeapPage* takePage();
        void releasePage(HeapPage* page);
        void* refill(usize index);
        void* allocate(usize size);
        void release(void* object);
        void releaseSlow(HeapPage* page, void* object);
        void adoptPages(void* data, usize bytes);

        HeapClass classes[heapClassCount]{};
        HeapSegment segments[heapSegmentLimit]{};
        HeapPage pages[heapSegmentLimit * heapPagesPerSegment]{};
        u32 freeSegments = heapNoSegment;
        u32 mappedSegments = 0;
    };

    static Heap heap;
}

usize Heap::classIndex(usize size) {
    require(size != 0 && size <= pageBytes, "allocation exceeds 64 KiB");
    if (size <= 256) {
        return (size + 15) / 16 - 1;
    }
    const unsigned exponent = 31 - __builtin_clz(u32(size - 1));
    return 16 + (exponent - 8) * 4 + (((size - 1) >> (exponent - 2)) & 3);
}

usize Heap::classSize(usize index) {
    if (index < 16) {
        return (index + 1) * objectAlignment;
    }
    const usize base = usize(1) << (8 + (index - 16) / 4);
    return base + (base / 4) * ((index - 16) % 4 + 1);
}

HeapPage* Heap::pageOf(const void* object) {
    uptr address = reinterpret_cast<uptr>(object);
#if !defined(__wasm__) && !defined(wasm)
    address -= heapNativeBase;
    require(address < heapSegmentLimit * heapSegmentBytes, "object outside allocator");
#endif
    return pages + (address >> 16);
}

uptr Heap::pageAddress(const HeapPage* page) {
    const uptr address = uptr(page - pages) << 16;
#if defined(__wasm__) || defined(wasm)
    return address;
#else
    return heapNativeBase + address;
#endif
}

void Heap::linkSegment(u32 index) {
    auto& segment = segments[index];
    segment.linked = true;
    segment.prev = heapNoSegment;
    segment.next = freeSegments;
    if (freeSegments != heapNoSegment) {
        segments[freeSegments].prev = index;
    }
    freeSegments = index;
}

void Heap::unlinkSegment(u32 index) {
    auto& segment = segments[index];
    if (segment.prev != heapNoSegment) {
        segments[segment.prev].next = segment.next;
    } else {
        freeSegments = segment.next;
    }
    if (segment.next != heapNoSegment) {
        segments[segment.next].prev = segment.prev;
    }
    segment.linked = false;
}

void Heap::registerSegment(u32 index) {
    require(index < heapSegmentLimit, "allocator region exhausted");
    auto& segment = segments[index];
    require(!segment.registered, "allocator segment already registered");
    segment.registered = true;
    segment.freeMask = 0;
    segment.linked = false;
    segment.prev = segment.next = heapNoSegment;
    for (usize i = 0; i < heapPagesPerSegment; ++i) {
        pages[usize(index) * heapPagesPerSegment + i].cls = heapUnavailablePage;
    }
}

void Heap::mapSegment() {
#if defined(__wasm__) || defined(wasm)
    const usize before = __builtin_wasm_memory_size(0);
    const usize first = alignUp(before, heapPagesPerSegment);
    const u32 index = u32(first / heapPagesPerSegment);
    require(index < heapSegmentLimit, "allocator region exhausted");
    if (__builtin_wasm_memory_grow(0, first - before + heapPagesPerSegment) == ~usize(0)) {
        fail("out of memory");
    }
    // I/O blobs and host-owned input may separate segments in linear memory.
    // The padding after them is ordinary reusable page storage.
    if (first != before) {
        adoptPages(reinterpret_cast<void*>(before * pageBytes), (first - before) * pageBytes);
    }
#else
    require(mappedSegments + 1 < heapSegmentLimit, "allocator region exhausted");
    const u32 index = mappedSegments + 1;
    void* wanted = reinterpret_cast<void*>(heapNativeBase + usize(index) * heapSegmentBytes);
    void* got = ::mmap(wanted, heapSegmentBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    require(got == wanted, "allocator mapping failed");
    #if defined(MADV_HUGEPAGE)
    (void)::madvise(got, heapSegmentBytes, MADV_HUGEPAGE);
    #endif
#endif
    registerSegment(index);
    auto& segment = segments[index];
    segment.freeMask = ~u32(0);
    for (usize i = 0; i < heapPagesPerSegment; ++i) {
        pages[usize(index) * heapPagesPerSegment + i].cls = heapFreePage;
    }
    linkSegment(index);
    ++mappedSegments;
}

HeapPage* Heap::takePage() {
    if (freeSegments == heapNoSegment) {
        mapSegment();
    }
    const u32 index = freeSegments;
    auto& segment = segments[index];
    const unsigned slot = __builtin_ctz(segment.freeMask);
    segment.freeMask &= ~(u32(1) << slot);
    if (segment.freeMask == 0) {
        unlinkSegment(index);
    }
    auto* page = pages + usize(index) * heapPagesPerSegment + slot;
    require(page->cls == heapFreePage, "allocator page is not free");
    page->free = nullptr;
    page->prev = page->next = nullptr;
    page->live = 0;
    return page;
}

void Heap::releasePage(HeapPage* page) {
    const usize pageIndex = usize(page - pages);
    const u32 segmentIndex = u32(pageIndex / heapPagesPerSegment);
    auto& segment = segments[segmentIndex];
    const u32 bit = u32(1) << (pageIndex % heapPagesPerSegment);
    require((segment.freeMask & bit) == 0, "allocator page already free");
    page->cls = heapFreePage;
    page->free = nullptr;
    page->prev = page->next = nullptr;
    segment.freeMask |= bit;
    if (!segment.linked) {
        linkSegment(segmentIndex);
    }
}

void* Heap::refill(usize index) {
    auto& state = classes[index];
    const usize size = classSize(index);
    // A current page counts frees in the class-local list. At this point
    // both that list and the bump range are exhausted: every slot is live.
    if (state.current) {
        state.current->live = u32(pageBytes / size);
    }
    if (auto* page = state.partial) {
        state.partial = page->next;
        if (state.partial) {
            state.partial->prev = nullptr;
        }
        page->prev = page->next = nullptr;
        state.current = page;
        auto* slot = page->free;
        page->free = nullptr;
        state.free = slot->next;
        state.bump = state.end = 0;
        return slot;
    }
    auto* page = takePage();
    page->cls = u16(index);
    state.current = page;
    state.free = nullptr;
    state.size = size;
    state.bump = pageAddress(page) + size;
    state.end = pageAddress(page) + (pageBytes / size) * size;
    return reinterpret_cast<void*>(pageAddress(page));
}

void* Heap::allocate(usize size) {
    auto& state = classes[classIndex(size)];
    if (auto* slot = state.free) {
        state.free = slot->next;
        return slot;
    }
    if (state.bump < state.end) {
        void* object = reinterpret_cast<void*>(state.bump);
        state.bump += state.size;
        return object;
    }
    return refill(classIndex(size));
}

void Heap::releaseSlow(HeapPage* page, void* object) {
    const usize index = page->cls;
    require(index < heapClassCount && page->live != 0, "free of unallocated object");
    auto& state = classes[index];
    if (--page->live == 0) {
        // Classes above 32 KiB have one slot per page. Such a full page
        // becomes empty on its first free and was never on the partial list.
        if (page->free) {
            if (page->prev) {
                page->prev->next = page->next;
            } else {
                state.partial = page->next;
            }
            if (page->next) {
                page->next->prev = page->prev;
            }
        }
        releasePage(page);
        return;
    }
    auto* slot = static_cast<HeapSlot*>(object);
    slot->next = page->free;
    page->free = slot;
    if (!slot->next) {
        page->prev = nullptr;
        page->next = state.partial;
        if (state.partial) {
            state.partial->prev = page;
        }
        state.partial = page;
    }
}

void Heap::release(void* object) {
    if (!object) {
        return;
    }
    auto* page = pageOf(object);
    const usize index = page->cls;
    require(index < heapClassCount, "free of unallocated object");
    auto& state = classes[index];
    if (page == state.current) {
        auto* slot = static_cast<HeapSlot*>(object);
        slot->next = state.free;
        state.free = slot;
        return;
    }
    releaseSlow(page, object);
}

void Heap::adoptPages(void* data, usize bytes) {
#if defined(__wasm__) || defined(wasm)
    const uptr first = reinterpret_cast<uptr>(data);
    require(first % pageBytes == 0 && bytes % pageBytes == 0, "invalid free memory range");
    for (usize offset = 0; offset < bytes; offset += pageBytes) {
        const usize pageIndex = (first + offset) >> 16;
        const u32 segmentIndex = u32(pageIndex / heapPagesPerSegment);
        require(segmentIndex < heapSegmentLimit, "allocator region exhausted");
        if (!segments[segmentIndex].registered) {
            registerSegment(segmentIndex);
        }
        auto* page = pages + pageIndex;
        require(page->cls == heapUnavailablePage, "I/O memory already owned by allocator");
        releasePage(page);
    }
#else
    (void)data;
    (void)bytes;
    fail("external pages are not in the native allocator region");
#endif
}

namespace {
    static void* allocPage() {
        return heap.allocate(pageBytes);
    }

    static void freePage(void* page) {
        require(page && reinterpret_cast<uptr>(page) % pageBytes == 0, "invalid page");
        heap.release(page);
    }

    static usize allocationBytes(usize size) {
        require(size != 0 && size <= pageBytes, "allocation exceeds 64 KiB");
        usize bytes = objectAlignment;
        while (bytes < size) {
            bytes *= 2;
        }
        return bytes;
    }

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
        heap.release(header());
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
    const usize bytes = allocationBytes(need);
    auto* before = header();
    const usize used = before->used;
    auto* next = static_cast<Header*>(heap.allocate(bytes));
    *next = {used, bytes};
    if (used) {
        copyBytes(next + 1, storage, used);
    }
    if (before != &emptyBuffer) {
        heap.release(before);
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
            return data()[index];
        }

        const T& operator[](usize index) const {
            return data()[index];
        }

        T& at(usize index) {
            require(index < size(), "SmallVec index");
            return (*this)[index];
        }

        const T& at(usize index) const {
            require(index < size(), "SmallVec index");
            return (*this)[index];
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

        void pushBack(T value) {
            const usize before = buffer.size();
            buffer.resize(checkedAdd(before, sizeof(T)));
            copyBytes(static_cast<u8*>(buffer.data()) + before, &value, sizeof(T));
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
            return pages[index / itemsPerPage][index % itemsPerPage];
        }

        const T& operator[](usize index) const {
            return pages[index / itemsPerPage][index % itemsPerPage];
        }

        T& at(usize index) {
            require(index < count, "PageVec index");
            return (*this)[index];
        }

        const T& at(usize index) const {
            require(index < count, "PageVec index");
            return (*this)[index];
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

        void append(const T* values, usize size) {
            reserve(checkedAdd(count, size));
            while (size) {
                const usize offset = count % itemsPerPage;
                const usize available = itemsPerPage - offset;
                const usize take = size < available ? size : available;
                copyBytes(pages.data()[count / itemsPerPage] + offset, values, take * sizeof(T));
                count += take;
                values += take;
                size -= take;
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
        auto* chunk = static_cast<Chunk*>(heap.allocate(bytes));
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
        heap.release(chunk);
    }
    current = nullptr;
    left = 0;
    nextChunk = 128;
}

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
    // All containers and inputs are bounded by INT32_MAX. Wasm scalar
    // values and ELF sizes use their explicit i64/u64 types.
    using Int = i32;
    using Uint = u32;
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

    template <typename Signature>
    struct Fn;

    template <typename R, typename... A>
    struct Fn<R(A...)> {
        void* context = nullptr;
        R (*invoke)(void*, A...) = nullptr;
        Fn() = default;

        template <typename F>
        Fn(F value) {
            context = activePool->make<F>(static_cast<F&&>(value));
            invoke = [](void* object, A... args) -> R {
                return (*static_cast<F*>(object))(args...);
            };
        }

        R operator()(A... args) const {
            require(invoke != nullptr, "nil callback");
            return invoke(context, args...);
        }
    };

    template <typename T>
    static bool isNil(Fn<T> fn) {
        return fn.invoke == nullptr;
    }

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
        enum struct Kind : u8 {
            Contiguous,
            Small,
            Paged,
        };

        ObjPool* owner;
        void* storage = nullptr;
        Int count = 0;
        Kind kind = Kind::Contiguous;

        ArrayData(ObjPool* pool)
            : owner(pool)
        {
        }

        ArrayData(const ArrayData&) = delete;
        ArrayData& operator=(const ArrayData&) = delete;

        ArrayData(ArrayData&& other) noexcept
            : owner(other.owner)
            , storage(other.storage)
            , count(other.count)
            , kind(other.kind)
        {
            other.storage = nullptr;
            other.count = 0;
            other.kind = Kind::Contiguous;
        }

        T* data() const {
            if (kind == Kind::Small) {
                return static_cast<SmallVec<T>*>(storage)->data();
            }
            return kind == Kind::Contiguous ? static_cast<T*>(storage) : nullptr;
        }

        PageVec<T>* pages() const {
            return kind == Kind::Paged ? static_cast<PageVec<T>*>(storage) : nullptr;
        }

        static void reset(T* values, usize count) {
            if (!count) {
                return;
            }
            if constexpr (__is_integral(T) || __is_pointer(T) || __is_floating_point(T) || __is_enum(T)) {
                zeroBytes(values, count * sizeof(T));
            } else {
                for (usize i = 0; i < count; ++i) {
                    values[i] = T{};
                }
            }
        }

        void initialize(Int size, Int clearFrom = 0) {
            require(size >= 0 && Uint(size) <= 0x7fffffff && clearFrom >= 0 && clearFrom <= size, "array too large");
            count = size;
            if (!size) {
                return;
            }
            if (u64(size) * sizeof(T) > pageBytes - sizeof(Buffer::Header)) {
                kind = Kind::Paged;
                auto* values = owner->make<PageVec<T>>();
                storage = values;
                values->resize(usize(size));
            } else {
                kind = Kind::Small;
                auto* values = owner->make<SmallVec<T>>();
                storage = values;
                values->resize(usize(size));
            }
            clearElements(clearFrom);
        }

        void clearElements(Int clearFrom) {
            if (auto* values = pages()) {
                usize at = usize(clearFrom);
                while (at < usize(count)) {
                    const usize offset = at % PageVec<T>::itemsPerPage;
                    const usize available = PageVec<T>::itemsPerPage - offset;
                    const usize remaining = usize(count) - at;
                    const usize take = remaining < available ? remaining : available;
                    reset(values->pages.data()[at / PageVec<T>::itemsPerPage] + offset, take);
                    at += take;
                }
            } else {
                reset(data() + clearFrom, usize(count - clearFrom));
            }
        }

        // Pool destruction owns the container; rehash can release its buffer
        // earlier without removing its disposal record from the pool.
        void releaseStorage() {
            if (kind == Kind::Small) {
                *static_cast<SmallVec<T>*>(storage) = SmallVec<T>{};
            } else if (kind == Kind::Paged) {
                *static_cast<PageVec<T>*>(storage) = PageVec<T>{};
            }
            storage = nullptr;
            count = 0;
            kind = Kind::Contiguous;
        }

        T& operator[](Int i) {
            if (kind == Kind::Contiguous) {
                return static_cast<T*>(storage)[i];
            }
            if (kind == Kind::Small) {
                return static_cast<SmallVec<T>*>(storage)->data()[i];
            }
            const usize index = usize(i);
            return static_cast<PageVec<T>*>(storage)->pages.data()[index / PageVec<T>::itemsPerPage][index % PageVec<T>::itemsPerPage];
        }
    };

    template <typename T, typename Storage>
    struct ArrayStorage {
        ArrayData<T> array;
        Storage values;

        ArrayStorage(ObjPool* pool, typename ArrayData<T>::Kind kind)
            : array(pool)
        {
            array.kind = kind;
            array.storage = &values;
        }
    };

    template <typename T>
    static __attribute__((noinline)) ArrayData<T>* makeArrayData(ObjPool& owner, Int size, Int clearFrom = 0) {
        require(size >= 0 && Uint(size) <= 0x7fffffff && clearFrom >= 0 && clearFrom <= size, "array too large");
        ArrayData<T>* array;
        if (!size) {
            return owner.make<ArrayData<T>>(&owner);
        } else if (u64(size) * sizeof(T) <= pageBytes - sizeof(Buffer::Header)) {
            auto* backing = owner.make<ArrayStorage<T, SmallVec<T>>>(&owner, ArrayData<T>::Kind::Small);
            backing->values.resize(usize(size));
            array = &backing->array;
        } else {
            auto* backing = owner.make<ArrayStorage<T, PageVec<T>>>(&owner, ArrayData<T>::Kind::Paged);
            backing->values.resize(usize(size));
            array = &backing->array;
        }
        array->count = size;
        array->clearElements(clearFrom);
        // Slice growth creates another backing array. This container never
        // reallocates, so its pool-owned buffer can be addressed directly.
        if (array->kind == ArrayData<T>::Kind::Small) {
            array->storage = array->data();
            array->kind = ArrayData<T>::Kind::Contiguous;
        }
        return array;
    }

    // A view can be copied freely. Its storage belongs to exactly one ObjPool.
    // Backing arrays are fixed after creation: Small buffers become direct
    // Contiguous views, and only Paged storage needs a container lookup.
    template <typename T>
    struct Slice {
        ArrayData<T>* array = nullptr;
        u32 offset = 0;
        u32 count = 0;
        u32 space = 0;

        T& unchecked(u32 i) const {
            const usize at = usize(offset) + i;
            if (array->kind == ArrayData<T>::Kind::Contiguous) {
                return static_cast<T*>(array->storage)[at];
            }
            return static_cast<PageVec<T>*>(array->storage)->pages.data()[at / PageVec<T>::itemsPerPage][at % PageVec<T>::itemsPerPage];
        }

        T& operator[](Int i) const {
            require(i >= 0 && Uint(i) < count, "slice index");
            return unchecked(u32(i));
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

    // A bounded, non-owning view. Its source is either contiguous storage or
    // an owning PageVec; appending and allocation belong to the owner.
    template <typename T>
    struct Unqualified {
        using Type = T;
    };

    template <typename T>
    struct Unqualified<const T> {
        using Type = T;
    };

    template <typename T>
    struct Span {
        using Value = typename Unqualified<T>::Type;
        T* contiguous = nullptr;
        PageVec<Value>* pages = nullptr;
        u32 offset = 0;
        u32 count = 0;

        Span() = default;

        Span(ArrayData<Value>& source)
            : contiguous(source.data())
            , pages(source.pages())
            , count(u32(source.count))
        {
        }

        Span(Slice<Value> source)
            : contiguous(source.array && source.array->kind == ArrayData<Value>::Kind::Contiguous ? static_cast<T*>(source.array->storage) : nullptr)
            , pages(source.array ? source.array->pages() : nullptr)
            , offset(source.offset)
            , count(source.count)
        {
        }

        Span(InitialValues<Value> source) requires(__is_const(T))
            : contiguous(source.begin())
            , count(u32(source.size()))
        {
        }

        Span(PageVec<Value>& source)
            : Span(source, 0, source.size())
        {
        }

        Span(PageVec<Value>& source, usize begin, usize end)
            : pages(&source)
            , offset(u32(begin))
            , count(u32(end - begin))
        {
            require(begin <= end && end <= source.size(), "page view bounds");
        }

        Span(T* data, usize size)
            : contiguous(data)
            , count(u32(size))
        {
        }

        template <typename U>
        Span(Span<U> source) requires(__is_same(T, const U))
            : contiguous(source.contiguous)
            , pages(source.pages)
            , offset(source.offset)
            , count(source.count)
        {
        }

        T& operator[](Int index) const {
            const usize at = offset + usize(index);
            if (contiguous) {
                return contiguous[at];
            }
            return pages->pages.data()[at / PageVec<Value>::itemsPerPage][at % PageVec<Value>::itemsPerPage];
        }

        T& at(Int index) const {
            require(Uint(index) < count, "view index");
            return (*this)[index];
        }
    };

    template <typename T>
    static Int length(Span<T> view) {
        return view.count;
    }

    template <typename T>
    static Int length(const PageVec<T>& values) {
        return Int(values.size());
    }

    template <typename T>
    static Span<T> subslice(Span<T> source, Int low, Int high) {
        require(low >= 0 && high >= low && Uint(high) <= source.count, "view bounds");
        source.offset += u32(low);
        source.count = u32(high - low);
        return source;
    }

    template <typename T>
    static Span<T> subslice(PageVec<T>& source, Int low, Int high) {
        return {source, usize(low), usize(high)};
    }

    // Own small storage until it no longer fits in one allocation, then
    // promote once to pages. Growth releases the previous small buffer.
    template <typename T>
    struct Vec {
        SmallVec<T> small;
        // The page count also holds the size while storage is still small.
        PageVec<T> pages;
        bool paged = false;

        Vec() = default;

        Vec(InitialValues<T> source) {
            assign(Span<const T>(source));
        }

        Vec(Slice<T> source) {
            assign(Span<const T>(source));
        }

        Vec(const Vec&) = delete;
        Vec& operator=(const Vec&) = delete;
        Vec(Vec&&) = default;
        Vec& operator=(Vec&&) = default;

        usize size() const {
            return pages.count;
        }

        void reserve(usize count) {
            if (!paged && count <= (pageBytes - sizeof(Buffer::Header)) / sizeof(T)) {
                small.reserve(count);
                return;
            }
            if (!paged) {
                const usize before = pages.count;
                pages.count = 0;
                pages.append(small.data(), before);
                small = SmallVec<T>{};
                paged = true;
            }
            pages.reserve(count);
        }

        void resize(usize count) {
            reserve(count);
            if (paged) {
                pages.resize(count);
            } else {
                small.resize(count);
                pages.count = count;
            }
        }

        void clear() {
            if (paged) {
                pages.clear();
            } else {
                small.clear();
                pages.count = 0;
            }
        }

        T& operator[](usize index) {
            return paged ? pages.pages.data()[index / PageVec<T>::itemsPerPage][index % PageVec<T>::itemsPerPage] : small.data()[index];
        }

        const T& operator[](usize index) const {
            return paged ? pages.pages.data()[index / PageVec<T>::itemsPerPage][index % PageVec<T>::itemsPerPage] : small.data()[index];
        }

        T& at(usize index) {
            require(index < size(), "Vec index");
            return (*this)[index];
        }

        const T& at(usize index) const {
            require(index < size(), "Vec index");
            return (*this)[index];
        }

        Span<T> view() {
            if (paged) {
                return {pages, 0, pages.size()};
            }
            return {small.data(), pages.count};
        }

        operator Span<T>() {
            return view();
        }

        void pushBack(T value) {
            if (paged) {
                pages.pushBack(value);
            } else if (pages.count < (pageBytes - sizeof(Buffer::Header)) / sizeof(T)) {
                small.pushBack(value);
                ++pages.count;
            } else {
                reserve(pages.count + 1);
                pages.pushBack(value);
            }
        }

        T popBack() {
            if (paged) {
                return pages.popBack();
            }
            T value = small.popBack();
            --pages.count;
            return value;
        }

        void assign(Span<const T> source) {
            reserve(source.count);
            resize(source.count);
            for (usize i = 0; i < source.count; ++i) {
                (*this)[i] = source[Int(i)];
            }
        }

        void fill(const T& value) {
            auto fillRange = [&](T* data, usize count) {
                for (usize i = 0; i < count; ++i) {
                    data[i] = value;
                }
            };
            if (!paged) {
                fillRange(small.data(), pages.count);
                return;
            }
            usize remaining = pages.size();
            for (usize p = 0; remaining; ++p) {
                const usize take = remaining < PageVec<T>::itemsPerPage ? remaining : PageVec<T>::itemsPerPage;
                fillRange(pages.pages.data()[p], take);
                remaining -= take;
            }
        }

        void append(Span<const T> source) {
            const usize before = size();
            const bool internal = source.contiguous && !paged && source.contiguous == small.data();
            reserve(before + source.count);
            if (internal) {
                const u32 begin = source.offset;
                const u32 count = source.count;
                source = view();
                source.offset = begin;
                source.count = count;
            }
            resize(before + source.count);
            for (usize i = 0; i < source.count; ++i) {
                (*this)[before + i] = source[Int(i)];
            }
        }

        template <typename F>
        void visit(F f) {
            for (usize i = 0; i < size(); ++i) {
                f((*this)[i]);
            }
        }
    };

    template <typename T>
    static Int length(const Vec<T>& values) {
        return Int(values.size());
    }

    // Short compiler work lists keep their first elements in the owner.
    // Contiguous buffers are released on growth/destruction. Page containers
    // belong to the current ObjPool, as does the surrounding interval.
    template <typename T, usize inlineCount>
    struct InlineVec {
        static_assert(__is_trivially_copyable(T));
        static_assert(__is_trivially_destructible(T));
        static_assert(inlineCount > 0 && inlineCount * sizeof(T) <= sizeof(u64));

        union Storage {
            T values[inlineCount]{};
            PageVec<T>* pages;
        };

        Storage storage{};
        T* contiguous = storage.values;
        u32 count = 0;
        u32 space = inlineCount;

        InlineVec() = default;
        InlineVec(const InlineVec&) = delete;
        InlineVec& operator=(const InlineVec&) = delete;

        InlineVec(InitialValues<T> source) {
            assign(Span<const T>(source));
        }

        ~InlineVec() {
            releaseBuffer();
        }

        InlineVec(InlineVec&& other) noexcept
            : storage(other.storage)
            , contiguous(other.contiguous == other.storage.values ? storage.values : other.contiguous)
            , count(other.count)
            , space(other.space)
        {
            other.storage = Storage{};
            other.contiguous = other.storage.values;
            other.count = 0;
            other.space = inlineCount;
        }

        InlineVec& operator=(InlineVec&& other) noexcept {
            if (this != &other) {
                releaseBuffer();
                if (!contiguous) {
                    *storage.pages = PageVec<T>{};
                }
                storage = other.storage;
                contiguous = other.contiguous == other.storage.values ? storage.values : other.contiguous;
                count = other.count;
                space = other.space;
                other.storage = Storage{};
                other.contiguous = other.storage.values;
                other.count = 0;
                other.space = inlineCount;
            }
            return *this;
        }

        void releaseBuffer() {
            if (contiguous && contiguous != storage.values) {
                heap.release(contiguous);
            }
        }

        usize size() const {
            return count;
        }

        usize capacity() const {
            return space;
        }

        T* data() {
            return contiguous;
        }

        const T* data() const {
            return contiguous;
        }

        T& operator[](usize index) {
            if (!contiguous) {
                return (*storage.pages)[index];
            }
            return contiguous[index];
        }

        const T& operator[](usize index) const {
            if (!contiguous) {
                return (*storage.pages)[index];
            }
            return contiguous[index];
        }

        void reserve(usize size) {
            if (size > space) {
                grow(size);
            }
        }

        __attribute__((noinline)) void grow(usize size) {
            require(size <= 0x7fffffff, "inline vector too large");
            if (!contiguous) {
                storage.pages->reserve(size);
                const usize capacity = storage.pages->capacity();
                space = u32(capacity < 0x7fffffff ? capacity : 0x7fffffff);
            } else if (size <= PageVec<T>::itemsPerPage) {
                const usize bytes = allocationBytes(checkedMultiply(size, sizeof(T)));
                auto* next = static_cast<T*>(heap.allocate(bytes));
                if (count) {
                    copyBytes(next, data(), count * sizeof(T));
                }
                releaseBuffer();
                contiguous = next;
                space = u32(bytes / sizeof(T));
            } else {
                auto* pages = activePool->make<PageVec<T>>();
                pages->reserve(size);
                pages->append(data(), count);
                releaseBuffer();
                storage.pages = pages;
                contiguous = nullptr;
                const usize capacity = pages->capacity();
                space = u32(capacity < 0x7fffffff ? capacity : 0x7fffffff);
            }
        }

        void resize(usize size) {
            reserve(size);
            count = u32(size);
        }

        void clear() {
            count = 0;
        }

        Span<T> view() {
            if (!contiguous) {
                // PageVec owns capacity; the list and its views own the length.
                Span<T> result;
                result.pages = storage.pages;
                result.count = count;
                return result;
            }
            return {data(), count};
        }

        void pushBack(T value) {
            const usize at = count;
            reserve(at + 1);
            (*this)[at] = value;
            count = u32(at + 1);
        }

        void assign(Span<const T> source) {
            resize(source.count);
            for (u32 i = 0; i < source.count; ++i) {
                (*this)[i] = source[Int(i)];
            }
        }

        void append(Span<const T> source) {
            if (!source.count) {
                return;
            }
            const uptr begin = reinterpret_cast<uptr>(source.contiguous);
            const uptr own = reinterpret_cast<uptr>(contiguous);
            const bool internal = own && begin >= own && begin - own < usize(count) * sizeof(T);
            const usize offset = internal ? (begin - own) / sizeof(T) + source.offset : 0;
            const usize before = count;
            const usize length = source.count;
            resize(before + length);
            if (internal) {
                source = subslice(view(), Int(offset), Int(offset + length));
            }
            for (usize i = 0; i < length; ++i) {
                (*this)[before + i] = source[Int(i)];
            }
        }
    };

    template <typename T, usize inlineCount>
    static Int length(const InlineVec<T, inlineCount>& values) {
        return Int(values.size());
    }

    // Nodes live in ObjPool; the vector owns only its compact pointer table.
    template <typename T>
    struct NodeVec {
        Vec<T*> nodes;
        ObjPool* owner = activePool;

        NodeVec() = default;
        NodeVec(const NodeVec&) = delete;
        NodeVec& operator=(const NodeVec&) = delete;
        NodeVec(NodeVec&&) = default;
        NodeVec& operator=(NodeVec&&) = default;

        usize size() const {
            return nodes.size();
        }

        T& operator[](usize index) {
            return *nodes[index];
        }

        const T& operator[](usize index) const {
            return *nodes[index];
        }

        void pushBack(T value) {
            nodes.pushBack(owner->make<T>(move(value)));
        }

        template <typename F>
        void visit(F f) {
            nodes.visit([&](T* node) {
                f(*node);
            });
        }
    };

    template <typename T>
    static Int length(const NodeVec<T>& values) {
        return Int(values.size());
    }

    template <typename T, typename U>
    static void appendSpan(PageVec<T>& destination, Span<U> source) {
        destination.reserve(destination.size() + source.count);
        for (Int i = 0; i < source.count; ++i) {
            destination.pushBack(source[i]);
        }
    }

    template <typename T, typename U>
    static Int copySlice(Span<T> destination, Span<U> source) {
        static_assert(__is_same(T, typename Unqualified<U>::Type));
        const usize count = destination.count < source.count ? destination.count : source.count;
        if (!count) {
            return 0;
        }
        if (destination.contiguous && source.contiguous) {
            moveBytes(destination.contiguous + destination.offset, source.contiguous + source.offset, count * sizeof(T));
            return Int(count);
        }
        const bool backward = destination.pages == source.pages && destination.offset > source.offset;
        constexpr usize perPage = PageVec<T>::itemsPerPage;
        usize remaining = count;
        while (remaining) {
            const usize at = backward ? remaining : count - remaining;
            const usize dst = destination.offset + at;
            const usize src = source.offset + at;
            usize take = remaining;
            if (!destination.contiguous) {
                const usize available = backward ? (dst - 1) % perPage + 1 : perPage - dst % perPage;
                if (take > available) {
                    take = available;
                }
            }
            if (!source.contiguous) {
                const usize available = backward ? (src - 1) % perPage + 1 : perPage - src % perPage;
                if (take > available) {
                    take = available;
                }
            }
            const usize dstBegin = dst - (backward ? take : 0);
            const usize srcBegin = src - (backward ? take : 0);
            T* out = destination.contiguous ? destination.contiguous + dstBegin : destination.pages->pages.data()[dstBegin / perPage] + dstBegin % perPage;
            const T* in = source.contiguous ? source.contiguous + srcBegin : source.pages->pages.data()[srcBegin / perPage] + srcBegin % perPage;
            moveBytes(out, in, take * sizeof(T));
            remaining -= take;
        }
        return Int(count);
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
        auto* data = makeArrayData<T>(*activePool, space);
        return {data, 0, u32(size), u32(space)};
    }

    template <typename T>
    static Slice<T> makeSlice(Int size) {
        return makeSlice<T>(size, size);
    }

    template <typename T>
    static Slice<T> borrowSlice(T* pointer, Int size) {
        auto* data = activePool->make<ArrayData<T>>(activePool);
        data->storage = pointer;
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
        auto* data = makeArrayData<T>(*owner, space, s.count);
        Slice<T> result{data, 0, s.count, u32(space)};
        copySlice(Span<T>(result), Span<const T>(s));
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
        ((s.unchecked(s.count++) = T(values)), ...);
        return s;
    }

    template <typename T>
    static Slice<T> appendSlice(Slice<T> destination, Slice<T> source) {
        growSlice(destination, source.count);
        for (Int i = 0; i < source.count; ++i) {
            destination.unchecked(destination.count++) = source[i];
        }
        return destination;
    }

    template <typename T, typename U>
    static Slice<T> appendSlice(Slice<T> destination, Span<U> source) {
        growSlice(destination, source.count);
        for (Int i = 0; i < source.count; ++i) {
            destination.unchecked(destination.count++) = source[i];
        }
        return destination;
    }

    template <typename T>
    static Int copySlice(Slice<T> destination, Slice<T> source) {
        return copySlice(Span<T>(destination), Span<const T>(source));
    }

    static void writeBytes(u32 fd, Span<const u8> bytes) {
        if (!bytes.count) {
            return;
        }
        if (bytes.contiguous) {
            writeAll(fd, bytes.contiguous + bytes.offset, bytes.count);
            return;
        }
        usize at = bytes.offset;
        usize remaining = bytes.count;
        while (remaining) {
            const usize offset = at % pageBytes;
            const usize available = pageBytes - offset;
            const usize take = remaining < available ? remaining : available;
            writeAll(fd, bytes.pages->pages.data()[at / pageBytes] + offset, take);
            at += take;
            remaining -= take;
        }
    }

    static void writeZeros(u32 fd, u64 count) {
        static constexpr u8 zeros[64]{};
        while (count) {
            const usize take = count < sizeof(zeros) ? usize(count) : sizeof(zeros);
            writeAll(fd, zeros, take);
            count -= take;
        }
    }

    template <typename T>
    static void storeLittle(u8* bytes, T value) {
        for (usize i = 0; i < sizeof(T); ++i) {
            bytes[i] = u8(value >> (i * 8));
        }
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
        if (s.array->kind == ArrayData<u8>::Kind::Contiguous) {
            return {static_cast<u8*>(s.array->storage) + s.offset, s.count};
        }
        const usize first = s.offset / pageBytes;
        if ((usize(s.offset) + s.count - 1) / pageBytes == first) {
            return {s.array->pages()->pages.data()[first] + s.offset % pageBytes, s.count};
        }
        require(s.count <= pageBytes - 32, "large internal string");
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
            out.unchecked(out.count++) = text.data[i];
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

    template <typename T>
    static T zeroValue() {
        return {};
    }

    template <typename T>
    static void clearMap(Slice<T> values) {
        for (Int i = 0; i < values.count; ++i) {
            values[i] = T{};
        }
    }

    template <typename T, typename U>
    static T convert(U value) {
        return static_cast<T>(value);
    }

    template <typename T, typename U>
    static T shiftLeft(T value, U amount) {
        if (u64(amount) >= sizeof(T) * 8) {
            return T(0);
        }
        using Unsigned = u64;
        return T(Unsigned(value) << amount);
    }

    template <typename T, typename U>
    static T shiftRight(T value, U amount) {
        if (u64(amount) >= sizeof(T) * 8) {
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

    template <typename T, typename C, typename... A>
    static auto minimum(T a, T b, C c, A... rest) {
        return minimum(minimum(a, b), c, rest...);
    }

    template <typename T, typename C, typename... A>
    static auto maximum(T a, T b, C c, A... rest) {
        return maximum(maximum(a, b), c, rest...);
    }

    static u64 combineHash(u64 a, u64 b) {
        return (a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2)));
    }

    template <typename T>
    static u64 hashValue(T value) {
        u64 x;
        if constexpr (__is_pointer(T)) {
            x = reinterpret_cast<uptr>(value);
        } else {
            x = u64(value);
        }
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
        Span<MapEntry<K, V>> buckets(m.data->buckets);
        for (Int probe = 0; probe <= mask; ++probe, at = (at + 1) & mask) {
            auto& entry = buckets[at];
            if (entry.state == 0) {
                return -1;
            }
            if (entry.state == 1 && entry.key == key) {
                return at;
            }
        }
        return -1;
    }

    // Find an existing key or the first reusable slot in one probe walk.
    // Continue past tombstones so an update cannot create a duplicate key.
    template <typename K, typename V>
    static Int findMapSlot(Map<K, V> m, const K& key) {
        const Int mask = m.data->buckets.count - 1;
        Int at = Int(hashValue(key) & mask);
        Int free = -1;
        Span<MapEntry<K, V>> buckets(m.data->buckets);
        for (Int probe = 0; probe <= mask; ++probe, at = (at + 1) & mask) {
            auto& entry = buckets[at];
            if (entry.state == 1) {
                if (entry.key == key) {
                    return at;
                }
            } else {
                if (free < 0) {
                    free = at;
                }
                if (entry.state == 0) {
                    return free;
                }
            }
        }
        return free;
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
        Span<MapEntry<K, V>> buckets(m.data->buckets);
        for (;; at = (at + 1) & mask) {
            auto& entry = buckets[at];
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
        Int at = m.data->buckets.count ? findMapSlot(m, key) : -1;
        if (at >= 0 && m.data->buckets[at].state == 1) {
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
            old.releaseStorage();
            at = findMapSlot(m, key);
        }
        require(at >= 0, "map has no insertion slot");
        auto& entry = m.data->buckets[at];
        ++m.data->count;
        if (entry.state == 0) {
            ++m.data->occupied;
        }
        entry = {key, value, 1};
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
    static u32 mathFloat32bits(float value) {
        return __builtin_bit_cast(u32, value);
    }

    static u64 mathFloat64bits(double value) {
        return __builtin_bit_cast(u64, value);
    }

    static float mathFloat32frombits(u32 value) {
        return __builtin_bit_cast(float, value);
    }

    static double mathFloat64frombits(u64 value) {
        return __builtin_bit_cast(double, value);
    }

    static bool mathIsNaN(double value) {
        return value != value;
    }

    static double mathInf(Int sign) {
        return mathFloat64frombits(sign < 0 ? 0xfff0000000000000ULL : 0x7ff0000000000000ULL);
    }

    static bool mathIsInf(double value, Int sign) {
        return sign >= 0 && value == mathInf(1) || sign <= 0 && value == mathInf(-1);
    }

    static double mathAbs(double value) {
        return mathFloat64frombits(mathFloat64bits(value) & 0x7fffffffffffffffULL);
    }

    static double mathTrunc(double value) {
        u64 bits = mathFloat64bits(value);
        Int exponent = Int((bits >> 52) & 2047) - 1023;
        if (exponent < 0) {
            return mathFloat64frombits(bits & (u64(1) << 63));
        }
        if (exponent < 52) {
            bits &= ~((u64(1) << (52 - exponent)) - 1);
        }
        return mathFloat64frombits(bits);
    }

    static double mathLdexp(double value, Int exponent) {
        require(exponent >= -1022 && exponent <= 1023, "float exponent");
        return value * mathFloat64frombits(u64(exponent + 1023) << 52);
    }

    static double mathNextafter(double value, double toward) {
        if (mathIsNaN(value) || mathIsNaN(toward)) {
            return value + toward;
        }
        if (value == toward) {
            return toward;
        }
        if (value == 0) {
            return mathFloat64frombits((mathFloat64bits(toward) & (u64(1) << 63)) | 1);
        }
        u64 bits = mathFloat64bits(value);
        if ((toward > value) == (value > 0)) {
            ++bits;
        } else {
            --bits;
        }
        return mathFloat64frombits(bits);
    }

    static float mathNextafter32(float value, float toward) {
        if (value != value || toward != toward) {
            return value + toward;
        }
        if (value == toward) {
            return toward;
        }
        if (value == 0) {
            return mathFloat32frombits((mathFloat32bits(toward) & (u32(1) << 31)) | 1);
        }
        u32 bits = mathFloat32bits(value);
        if ((toward > value) == (value > 0)) {
            ++bits;
        } else {
            --bits;
        }
        return mathFloat32frombits(bits);
    }

    static Int bitsLen(Uint value) {
        return value ? 64 - __builtin_clzll(value) : 0;
    }

    static Int bitsLeadingZeros32(u32 value) {
        return value ? __builtin_clz(value) : 32;
    }

    static Int bitsLeadingZeros64(u64 value) {
        return value ? __builtin_clzll(value) : 64;
    }

    static Int bitsTrailingZeros32(u32 value) {
        return value ? __builtin_ctz(value) : 32;
    }

    static Int bitsTrailingZeros64(u64 value) {
        return value ? __builtin_ctzll(value) : 64;
    }

    static Int bitsOnesCount64(u64 value) {
        return __builtin_popcountll(value);
    }

    static u32 bitsRotateLeft32(u32 value, Int count) {
        u32 shift = u32(count) & 31;
        return value << shift | value >> ((32 - shift) & 31);
    }

    static u64 bitsRotateLeft64(u64 value, Int count) {
        u32 shift = u32(count) & 63;
        return value << shift | value >> ((64 - shift) & 63);
    }

    template <typename T>
    static T littleRead(Slice<u8> bytes) {
        T result = 0;
        for (usize i = 0; i < sizeof(T); ++i) {
            result |= T(bytes[i]) << (8 * i);
        }
        return result;
    }

    template <typename T>
    static void littleWrite(Span<u8> bytes, T value) {
        for (usize i = 0; i < sizeof(T); ++i) {
            bytes[i] = u8(value >> (8 * i));
        }
    }

    static u16 littleUint16(Slice<u8> bytes) {
        return littleRead<u16>(bytes);
    }

    static u32 littleUint32(Slice<u8> bytes) {
        return littleRead<u32>(bytes);
    }

    static u64 littleUint64(Slice<u8> bytes) {
        return littleRead<u64>(bytes);
    }

    static void littlePutUint16(Span<u8> bytes, u16 value) {
        littleWrite(bytes, value);
    }

    static void littlePutUint32(Span<u8> bytes, u32 value) {
        littleWrite(bytes, value);
    }

    static void littlePutUint64(Span<u8> bytes, u64 value) {
        littleWrite(bytes, value);
    }

    template <typename T>
    static Slice<u8> littleAppend(Slice<u8> bytes, T value) {
        u32 at = bytes.count;
        growSlice(bytes, sizeof(T));
        bytes.count += sizeof(T);
        littleWrite(subslice(bytes, at, bytes.count), value);
        return bytes;
    }

    static Slice<u8> littleAppendUint16(Slice<u8> bytes, u16 value) {
        return littleAppend(bytes, value);
    }

    static Slice<u8> littleAppendUint32(Slice<u8> bytes, u32 value) {
        return littleAppend(bytes, value);
    }

    static Slice<u8> littleAppendUint64(Slice<u8> bytes, u64 value) {
        return littleAppend(bytes, value);
    }

    static Slice<u8> binaryAppendUvarint(Slice<u8> bytes, u64 value) {
        while (value >= 128) {
            bytes = appendValues(bytes, u8(value) | 128);
            value >>= 7;
        }
        return appendValues(bytes, u8(value));
    }

    static String cloneString(String value) {
        if (!value.count) {
            return {};
        }
        auto* bytes = static_cast<u8*>(activePool->allocate(value.count, 1));
        copyBytes(bytes, value.data, value.count);
        return {bytes, value.count};
    }

    static bool stringsHasPrefix(String text, String prefix) {
        return text.count >= prefix.count && subslice(text, 0, prefix.count) == prefix;
    }

    template <typename T, typename V>
    static bool slicesContains(Slice<T> values, V value) {
        for (Int i = 0; i < values.count; ++i) {
            if (values[i] == value) {
                return true;
            }
        }
        return false;
    }

    template <typename T>
    static Slice<T> slicesClone(Slice<T> values) {
        if (isNil(values)) {
            return {};
        }
        auto result = makeSlice<T>(values.count);
        copySlice(result, values);
        return result;
    }

    template <typename F>
    static Int sortSearch(Int size, F predicate) {
        Int low = 0, high = size;
        while (low < high) {
            Int middle = low + (high - low) / 2;
            if (predicate(middle)) {
                high = middle;
            } else {
                low = middle + 1;
            }
        }
        return low;
    }

    struct FormatValue {
        String text{};
        u64 number = 0;
        bool negative = false;

        FormatValue(String value)
            : text(value)
        {
        }

        template <typename T>
        FormatValue(T value)
            : number(u64(value))
            , negative(value < T(0))
        {
        }
    };

    template <typename... A>
    static String fmtSprintf(String format, A... values) {
        FormatValue args[] = {FormatValue(values)...};
        Int argument = 0;
        SmallVec<u8> out;
        for (usize i = 0; i < format.count; ++i) {
            u8 ch = format.data[i];
            if (ch != '%') {
                out.pushBack(ch);
                continue;
            }
            require(++i < format.count, "format end");
            if (format.data[i] == '%') {
                out.pushBack('%');
                continue;
            }
            u32 width = 0;
            bool padZero = format.data[i] == '0';
            while (format.data[i] >= '0' && format.data[i] <= '9') {
                width = width * 10 + format.data[i++] - '0';
                require(i < format.count, "format end");
            }
            require(argument < sizeof...(values), "format arguments");
            auto arg = args[argument++];
            u8 kind = format.data[i];
            if (kind == 's') {
                out.append(arg.text.data, arg.text.count);
                continue;
            }
            require(kind == 'd' || kind == 'x' || kind == 'X', "format kind");
            u32 base = kind == 'd' ? 10 : 16;
            u64 number = base == 10 && arg.negative ? -arg.number : arg.number;
            u8 buffer[32];
            usize at = sizeof(buffer);
            do {
                u32 digit = number % base;
                buffer[--at] = u8(digit < 10 ? '0' + digit : (kind == 'X' ? 'A' : 'a') + digit - 10);
                number /= base;
            } while (number);
            if (base == 10 && arg.negative) {
                buffer[--at] = '-';
            }
            while (width > sizeof(buffer) - at) {
                out.pushBack(padZero ? '0' : ' ');
                --width;
            }
            out.append(buffer + at, sizeof(buffer) - at);
        }
        auto* bytes = static_cast<u8*>(activePool->allocate(out.size() + 1, 1));
        copyBytes(bytes, out.data(), out.size());
        bytes[out.size()] = 0;
        return {bytes, out.size()};
    }
}

namespace {
    // Compiler data is pool owned; slice and map fields are non-owning views.

    struct AddressExpression;

    struct AddressExpressions;

    struct AddressTerm;

    struct Allocator;

    struct Amem;

    struct Anonymous10;

    struct Anonymous17;

    struct Anonymous24;

    struct Anonymous4;

    struct Anonymous49;

    struct Anonymous50;

    struct Anonymous68;

    struct Anonymous93;

    struct Asm;

    struct Block;

    struct Bypass;

    struct CallFixup;

    struct CfgInfo;

    struct CheckUndo;

    struct CheckedEdge;

    struct CheckedValue;

    struct CodeBlob;

    struct CompileOptions;

    struct ConstKey;

    struct Control;

    struct CopyAddress;

    struct CopyPairCoalesce41;

    struct CseKey;

    struct CseVisit;

    struct Data;

    struct Edge;

    struct Elem;

    struct Export;

    struct ExportMeta;

    struct FloatIntegerNarrow;

    struct FloatRange;

    struct FloatRanges;

    struct FrameCfg34;

    struct FrameRegalloc324;

    struct Frontend;

    struct Func;

    struct FuncEmitter;

    struct FuncType;

    struct Function;

    struct Global;

    struct GlobalMeta;

    struct GoFrame;

    struct GoLoc;

    struct Import;

    struct ImportMeta;

    struct Interval;

    struct JumpFixup;

    struct Layout;

    template <typename L, typename S>
    struct LessSwap;

    struct Limits;

    struct LoadRangeLoop_version741;

    struct LoopSpan;

    struct LowerVisit;

    struct Lowerer;

    struct Lrange;

    struct MBlock;

    struct MFunc;

    struct MInst;

    struct Mem;

    struct Memory;

    struct MemoryLoop;

    struct MemoryMeta;

    struct Meta;

    struct MetaWriter;

    struct Module;

    struct Move;

    struct ObjCode;

    struct Optimizer;

    struct OrigInst;

    struct OrigOperand;

    struct PendingPhi;

    struct PredRef;

    struct RangeKeyLoop_version570;

    struct Reader;

    struct Region;

    struct RipFixup;

    struct SpEntry;

    struct SpillDef;

    struct Table;

    struct TableFixup;

    struct TableMeta;

    struct TailRangeGroup;

    struct TailRangeRegion;

    struct Term;

    struct TextAddress;

    struct TextReloc;

    struct TrapFixup;

    struct UnsignedRange;

    struct UnsignedRanges;

    struct Value;

    struct VisitMir346;

    using MemKind = i32;

    using MOp = u8;

    using Op = u16;

    using Xorshift = u64;

    using SortedHint = Int;

    // Small sets use at most 32 words and a mask of occupied words. Larger
    // sets allocate 256-bit chunks under 16-Kibit directory nodes lazily.
    static constexpr u32 denseBitsetLimit = 2048;

    struct BitChunk {
        u64 words[4]{};
    };

    struct BitNode {
        u64 occupied = 0;
    };

    // The pointer table follows the header in the same pool allocation.
    // The last directory node only stores slots within the bitset's bounds.
    static BitChunk** bitNodeChunks(BitNode* node) {
        return reinterpret_cast<BitChunk**>(node + 1);
    }

    struct BitsetData {
        ObjPool* pool = nullptr;
        void* storage = nullptr;
        u32 bits = 0;
        u32 occupied = 0;
    };

    struct SparseBitsetStorage {
        BitsetData data;
        SmallVec<BitNode*> nodes;
    };

    struct Bitset {
        BitsetData* data = nullptr;
    };

    static u64 bitsetSparseWord(BitsetData* data, usize index) {
        BitNode* node = static_cast<BitNode**>(data->storage)[index / 256];
        if (!node) {
            return 0;
        }
        BitChunk* chunk = bitNodeChunks(node)[index / 4 % 64];
        return chunk ? chunk->words[index % 4] : 0;
    }

    static u64 bitsetWord(Bitset set, usize index) {
        if (!set.data || index >= (usize(set.data->bits) + 63) / 64) {
            return 0;
        }
        if (set.data->bits <= denseBitsetLimit) {
            return set.data->storage ? static_cast<u64*>(set.data->storage)[index] : 0;
        }
        return bitsetSparseWord(set.data, index);
    }

    static u64* bitsetDenseWords(BitsetData* data) {
        if (!data->storage) {
            const usize bytes = ((usize(data->bits) + 63) / 64) * sizeof(u64);
            data->storage = data->pool->allocate(bytes, alignof(u64));
            zeroBytes(data->storage, bytes);
        }
        return static_cast<u64*>(data->storage);
    }

    static bool bitsetAddSparseWord(BitsetData* data, usize index, u64 bits) {
        if (bits == 0) {
            return false;
        }
        BitNode*& node = static_cast<BitNode**>(data->storage)[index / 256];
        if (!node) {
            const usize available = (usize(data->bits) + 255) / 256 - (index / 256) * 64;
            const usize count = available < 64 ? available : 64;
            const usize bytes = sizeof(BitNode) + count * sizeof(BitChunk*);
            void* storage = data->pool->allocate(bytes, alignof(BitNode));
            zeroBytes(storage, bytes);
            node = new (storage) BitNode;
        }
        BitChunk*& chunk = bitNodeChunks(node)[index / 4 % 64];
        if (!chunk) {
            chunk = data->pool->make<BitChunk>();
            node->occupied |= u64(1) << (index / 4 % 64);
        }
        u64& word = chunk->words[index % 4];
        const u64 before = word;
        word |= bits;
        return before != word;
    }

    static bool bitsetAddWord(Bitset set, usize index, u64 bits) {
        if (bits == 0) {
            return false;
        }
        require(set.data && index < (set.data->bits + 63) / 64, "bitset index");
        if (set.data->bits <= denseBitsetLimit) {
            u64& word = bitsetDenseWords(set.data)[index];
            const u64 before = word;
            word |= bits;
            set.data->occupied |= u32(1) << index;
            return before != word;
        }
        return bitsetAddSparseWord(set.data, index, bits);
    }

    template <typename F>
    static void visitBitsetWords(Bitset set, F visitor) {
        if (!set.data) {
            return;
        }
        if (set.data->bits <= denseBitsetLimit) {
            auto* words = static_cast<u64*>(set.data->storage);
            for (u32 occupied = set.data->occupied; occupied; occupied &= occupied - 1) {
                const usize index = usize(bitsTrailingZeros64(occupied));
                visitor(index, words[index]);
            }
            return;
        }
        auto** nodes = static_cast<BitNode**>(set.data->storage);
        const usize count = (usize(set.data->bits) + 16383) / 16384;
        for (usize n = 0; n < count; ++n) {
            BitNode* node = nodes[n];
            if (!node) {
                continue;
            }
            for (u64 occupied = node->occupied; occupied; occupied &= occupied - 1) {
                usize c = usize(bitsTrailingZeros64(occupied));
                BitChunk* chunk = bitNodeChunks(node)[c];
                for (usize w = 0; w < 4; ++w) {
                    if (chunk->words[w]) {
                        visitor(n * 256 + c * 4 + w, chunk->words[w]);
                    }
                }
            }
        }
    }

    // Compute in |= gen | (out & ~kill), without materializing a dense
    // temporary or scanning words with no live virtual registers.
    static bool bitsetPropagate(Bitset in, Bitset gen, Bitset out, Bitset kill) {
        if (!gen.data && in.data && out.data && kill.data && in.data->bits <= denseBitsetLimit && in.data->bits == out.data->bits && in.data->bits == kill.data->bits) {
            auto* words = static_cast<u64*>(in.data->storage);
            const auto* source = static_cast<const u64*>(out.data->storage);
            const auto* mask = static_cast<const u64*>(kill.data->storage);
            bool changed = false;
            for (u32 occupied = out.data->occupied; occupied; occupied &= occupied - 1) {
                const usize index = usize(bitsTrailingZeros64(occupied));
                const u64 bits = source[index] & ~(mask ? mask[index] : u64(0));
                if (!bits) {
                    continue;
                }
                if (!words) {
                    words = bitsetDenseWords(in.data);
                }
                const u64 before = words[index];
                words[index] |= bits;
                in.data->occupied |= u32(1) << index;
                changed |= before != words[index];
            }
            return changed;
        }
        if (!gen.data && in.data && out.data && kill.data && in.data->bits > denseBitsetLimit && in.data->bits == out.data->bits && in.data->bits == kill.data->bits) {
            bool changed = false;
            // The source iterator bounds every word and all three sets have
            // the same representation, so per-word tag checks are redundant.
            visitBitsetWords(out, [&](usize index, u64 word) {
                if (bitsetAddSparseWord(in.data, index, word & ~bitsetSparseWord(kill.data, index))) {
                    changed = true;
                }
            });
            return changed;
        }
        bool changed = false;
        visitBitsetWords(gen, [&](usize index, u64 word) {
            if (bitsetAddWord(in, index, word)) {
                changed = true;
            }
        });
        visitBitsetWords(out, [&](usize index, u64 word) {
            if (bitsetAddWord(in, index, word & ~bitsetWord(kill, index))) {
                changed = true;
            }
        });
        return changed;
    }

    using ValType = u8;

    using TermKind = u8;

    using FAluOp = u8;

    using VReg = i32;

    using AluOp = u8;

    struct CheckUndo {
        i32 value{};
        i64 end{};
        bool set{};
    };

    static bool operator==(const CheckUndo& a, const CheckUndo& b) {
        return a.value == b.value && a.end == b.end && a.set == b.set;
    }

    static u64 hashValue(const CheckUndo& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.value));
        result = combineHash(result, hashValue(value.end));
        result = combineHash(result, hashValue(value.set));
        return result;
    }

    template <typename L, typename S>
    struct LessSwap {
        L Less;
        S Swap;
    };

    struct Region {
        String name{};
        u32 start{};
        u32 end{};
        Slice<SpEntry> sp{};
    };

    struct Anonymous49 {
        i32 base{};
        i32 base2{};
        i32 stride{};
        u64 mul{};
        u64 mul2{};
        u64 factor{};
    };

    static bool operator==(const Anonymous49& a, const Anonymous49& b) {
        return a.base == b.base && a.base2 == b.base2 && a.stride == b.stride && a.mul == b.mul && a.mul2 == b.mul2 && a.factor == b.factor;
    }

    static u64 hashValue(const Anonymous49& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.base));
        result = combineHash(result, hashValue(value.base2));
        result = combineHash(result, hashValue(value.stride));
        result = combineHash(result, hashValue(value.mul));
        result = combineHash(result, hashValue(value.mul2));
        result = combineHash(result, hashValue(value.factor));
        return result;
    }

    struct SpEntry {
        i32 pc{};
        i32 delta{};
    };

    static bool operator==(const SpEntry& a, const SpEntry& b) {
        return a.pc == b.pc && a.delta == b.delta;
    }

    static u64 hashValue(const SpEntry& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.pc));
        result = combineHash(result, hashValue(value.delta));
        return result;
    }

    struct AddressTerm {
        i32 value{};
        u32 mul{};
    };

    static bool operator==(const AddressTerm& a, const AddressTerm& b) {
        return a.value == b.value && a.mul == b.mul;
    }

    static u64 hashValue(const AddressTerm& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.value));
        result = combineHash(result, hashValue(value.mul));
        return result;
    }

    struct Data {
        u8 mode{};
        u32 memory{};
        Slice<u8> offset{};
        Slice<u8> bytes{};
    };

    struct OrigInst {
        Slice<OrigOperand> ops{};
    };

    struct Control {
        Int kind{};
        Slice<ValType> params{};
        Slice<ValType> results{};
        Int height{};
        i32 target{};
        i32 elseBlock{};
        Slice<i32> elseArgs{};
        bool dead{};
        bool outer{};
    };

    struct FrameRegalloc324 {
        i32 b{};
        Int i{};
    };

    static bool operator==(const FrameRegalloc324& a, const FrameRegalloc324& b) {
        return a.b == b.b && a.i == b.i;
    }

    static u64 hashValue(const FrameRegalloc324& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.b));
        result = combineHash(result, hashValue(value.i));
        return result;
    }

    struct FloatRanges {
        Function* f{};
        Map<i32, FloatRange> facts{};
        Map<i32, FloatRange> cache{};
    };

    struct Move {
        VReg dst{};
        VReg src{};
    };

    static bool operator==(const Move& a, const Move& b) {
        return a.dst == b.dst && a.src == b.src;
    }

    static u64 hashValue(const Move& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.dst));
        result = combineHash(result, hashValue(value.src));
        return result;
    }

    struct CodeBlob {
        Slice<u8> bytes{};
        Int align{};
    };

    struct Global {
        ValType typ{};
        bool mutableValue{};
        bool imported{};
        Slice<u8> init{};
    };

    struct Anonymous17 {
        i32 dst{};
        i32 src{};
        Int depth{};
    };

    static bool operator==(const Anonymous17& a, const Anonymous17& b) {
        return a.dst == b.dst && a.src == b.src && a.depth == b.depth;
    }

    static u64 hashValue(const Anonymous17& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.dst));
        result = combineHash(result, hashValue(value.src));
        result = combineHash(result, hashValue(value.depth));
        return result;
    }

    struct LoopSpan {
        i32 base{};
        u64 mul{};
        u64 factor{};
        u64 offset{};
        i32 base2{};
        u64 mul2{};
        u64 lo{};
        i32 stride{};
        bool valid{};
    };

    static bool operator==(const LoopSpan& a, const LoopSpan& b) {
        return a.base == b.base && a.mul == b.mul && a.factor == b.factor && a.offset == b.offset && a.base2 == b.base2 && a.mul2 == b.mul2 && a.lo == b.lo && a.stride == b.stride && a.valid == b.valid;
    }

    static u64 hashValue(const LoopSpan& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.base));
        result = combineHash(result, hashValue(value.mul));
        result = combineHash(result, hashValue(value.factor));
        result = combineHash(result, hashValue(value.offset));
        result = combineHash(result, hashValue(value.base2));
        result = combineHash(result, hashValue(value.mul2));
        result = combineHash(result, hashValue(value.lo));
        result = combineHash(result, hashValue(value.stride));
        result = combineHash(result, hashValue(value.valid));
        return result;
    }

    struct Edge {
        i32 block{};
        Slice<i32> args{};
    };

    struct GoFrame {
        Slice<GoLoc> params{};
        Slice<GoLoc> results{};
        i32 size{};
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
        bool batchData{};
    };

    struct MetaWriter {
        Slice<u8> out{};
    };

    struct CopyPairCoalesce41 {
        i32 dst{};
        i32 src{};
        Int depth{};
    };

    static bool operator==(const CopyPairCoalesce41& a, const CopyPairCoalesce41& b) {
        return a.dst == b.dst && a.src == b.src && a.depth == b.depth;
    }

    static u64 hashValue(const CopyPairCoalesce41& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.dst));
        result = combineHash(result, hashValue(value.src));
        result = combineHash(result, hashValue(value.depth));
        return result;
    }

    struct FuncEmitter {
        ObjCode* oc{};
        Asm* a{};
        MFunc* mf{};
        Allocator* al{};
        i32 frame{};
        Slice<VReg> pushes{};
        bool framed{};
        i32 body{};
        Slice<i32> blockAt{};
        Slice<JumpFixup> jumps{};
        Slice<TrapFixup> traps{};
        Slice<TableFixup> tables{};
        Slice<Fn<void()>> late{};
        i32 incoming{};
        i32 profile{};
        bool cold{};
    };

    struct Amem {
        Int base{};
        Int index{};
        u8 scale{};
        i32 disp{};
        i32 label{};
        u8 labelKind{};
    };

    static bool operator==(const Amem& a, const Amem& b) {
        return a.base == b.base && a.index == b.index && a.scale == b.scale && a.disp == b.disp && a.label == b.label && a.labelKind == b.labelKind;
    }

    static u64 hashValue(const Amem& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.base));
        result = combineHash(result, hashValue(value.index));
        result = combineHash(result, hashValue(value.scale));
        result = combineHash(result, hashValue(value.disp));
        result = combineHash(result, hashValue(value.label));
        result = combineHash(result, hashValue(value.labelKind));
        return result;
    }

    struct OrigOperand {
        VReg vreg{};
        VReg loc{};
        Int role{};
    };

    static bool operator==(const OrigOperand& a, const OrigOperand& b) {
        return a.vreg == b.vreg && a.loc == b.loc && a.role == b.role;
    }

    static u64 hashValue(const OrigOperand& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.vreg));
        result = combineHash(result, hashValue(value.loc));
        result = combineHash(result, hashValue(value.role));
        return result;
    }

    struct CheckedValue {
        i32 value{};
        i64 end{};
    };

    static bool operator==(const CheckedValue& a, const CheckedValue& b) {
        return a.value == b.value && a.end == b.end;
    }

    static u64 hashValue(const CheckedValue& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.value));
        result = combineHash(result, hashValue(value.end));
        return result;
    }

    struct TextReloc {
        i32 at{};
        i32 sym{};
        i64 addend{};
    };

    static bool operator==(const TextReloc& a, const TextReloc& b) {
        return a.at == b.at && a.sym == b.sym && a.addend == b.addend;
    }

    static u64 hashValue(const TextReloc& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.at));
        result = combineHash(result, hashValue(value.sym));
        result = combineHash(result, hashValue(value.addend));
        return result;
    }

    struct SpillDef {
        Int count{};
        i32 block{};
        i32 pos{};
    };

    static bool operator==(const SpillDef& a, const SpillDef& b) {
        return a.count == b.count && a.block == b.block && a.pos == b.pos;
    }

    static u64 hashValue(const SpillDef& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.count));
        result = combineHash(result, hashValue(value.block));
        result = combineHash(result, hashValue(value.pos));
        return result;
    }

    struct Term {
        TermKind kind{};
        i32 value{};
        Slice<Edge> edges{};
        Slice<i32> values{};
        i64 aux{};
    };

    struct Lrange {
        i32 from{};
        i32 to{};
    };

    // Coalescing needs ranges, not allocation state or use positions.
    struct CopyInterval {
        InlineVec<Lrange, 1> ranges;
    };

    static_assert(sizeof(CopyInterval) == (sizeof(void*) == 8 ? 24 : 20));

    struct IntervalChildren {
        InlineVec<Interval*, 1>* entries = nullptr;

        usize size() const {
            return entries ? entries->size() : 0;
        }

        Interval*& operator[](usize index) {
            return (*entries)[index];
        }

        Interval* operator[](usize index) const {
            return (*entries)[index];
        }

        void insert(usize index, Interval* child) {
            if (!entries) {
                entries = activePool->make<InlineVec<Interval*, 1>>();
            }
            const usize before = entries->size();
            entries->pushBack(child);
            const auto values = entries->view();
            copySlice(subslice(values, Int(index + 1), Int(before + 1)), subslice(values, Int(index), Int(before)));
            (*entries)[index] = child;
        }
    };

    static Int length(const IntervalChildren& children) {
        return Int(children.size());
    }

    struct Interval {
        VReg vreg{};
        u8 classValue{};
        InlineVec<Lrange, 1> ranges{};
        InlineVec<i32, 2> uses{};
        VReg reg{};
        Interval* parent{};
        IntervalChildren children{};
        VReg hint{};
        // Fixed intervals already have physical vreg IDs. Reuse their flag's
        // storage for the upper bound, valid after finish and maintained by split.
        i32 end{};
        Int cursor{};
        i32 locationFrom{};
        i32 locationTo{};
        VReg locationReg{};
    };

    static_assert(sizeof(Interval) == (sizeof(void*) == 8 ? 104 : 84));

    struct Anonymous50 {
        i32 begin{};
        i32 end{};
    };

    static bool operator==(const Anonymous50& a, const Anonymous50& b) {
        return a.begin == b.begin && a.end == b.end;
    }

    static u64 hashValue(const Anonymous50& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.begin));
        result = combineHash(result, hashValue(value.end));
        return result;
    }

    struct ParamIndex {
        i32 index = 0;
        u32 epoch = 0;
    };

    struct Optimizer {
        Function* f{};
        Vec<i32> alias{};
        Map<ConstKey, i32> consts{};
        Vec<i32> uses{};
        Vec<u32> incomingSeen;
        u32 incomingEpoch = 0;
        Vec<ParamIndex> repairIndex;
        u32 repairEpoch = 0;
    };

    struct LoopBodyUses;

    struct MemoryLoop {
        Function* f{};
        CfgInfo* ci{};
        i32 header{};
        i32 latch{};
        i32 preheader{};
        i32 exit{};
        Edge entry{};
        Slice<bool> members{};
        Slice<i32> order{};
        i32 count{};
        i32 countBias{};
        u64 countStep{};
        u64 countStop{};
        u64 countMask{};
        bool countSigned{};
        i32 start{};
        i32 limit{};
        bool signedLimit{};
        Map<i32, LoopSpan> induction{};
        Map<i32, LoopSpan> cache{};
        Map<i32, bool> noWrap{};
        UnsignedRanges* unsignedValue{};
        bool allowLoads{};
        Map<i32, bool> invariantCache{};
        Slice<i32> invariantNodes{};
        Slice<i32> copies{};
        LoopBodyUses* bodyUses = nullptr;
    };

    struct PredRef {
        i32 block{};
        i32 edge{};
    };

    static bool operator==(const PredRef& a, const PredRef& b) {
        return a.block == b.block && a.edge == b.edge;
    }

    static u64 hashValue(const PredRef& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.block));
        result = combineHash(result, hashValue(value.edge));
        return result;
    }

    struct Layout {
        i32 memoryEnd{};
        i32 tailProfile{};
        Slice<i32> globals{};
        Slice<i32> tables{};
        i32 descs{};
        i32 dropped{};
        i32 size{};
    };

    struct TableMeta {
        u32 offset{};
        u64 min{};
        u64 max{};
        bool imported{};
    };

    static bool operator==(const TableMeta& a, const TableMeta& b) {
        return a.offset == b.offset && a.min == b.min && a.max == b.max && a.imported == b.imported;
    }

    static u64 hashValue(const TableMeta& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.offset));
        result = combineHash(result, hashValue(value.min));
        result = combineHash(result, hashValue(value.max));
        result = combineHash(result, hashValue(value.imported));
        return result;
    }

    struct LoadRangeLoop_version741 {
        i32 begin{};
        i32 end{};
    };

    static bool operator==(const LoadRangeLoop_version741& a, const LoadRangeLoop_version741& b) {
        return a.begin == b.begin && a.end == b.end;
    }

    static u64 hashValue(const LoadRangeLoop_version741& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.begin));
        result = combineHash(result, hashValue(value.end));
        return result;
    }

    struct Elem {
        u8 mode{};
        u32 table{};
        Slice<u8> offset{};
        ValType typ{};
        Slice<Slice<u8>> exprs{};
    };

    struct RipFixup {
        i32 at{};
        i32 end{};
        i32 label{};
        u8 kind{};
    };

    static bool operator==(const RipFixup& a, const RipFixup& b) {
        return a.at == b.at && a.end == b.end && a.label == b.label && a.kind == b.kind;
    }

    static u64 hashValue(const RipFixup& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.at));
        result = combineHash(result, hashValue(value.end));
        result = combineHash(result, hashValue(value.label));
        result = combineHash(result, hashValue(value.kind));
        return result;
    }

    struct Asm {
        PageVec<u8> buf{};
        Slice<RipFixup> fixups{};
        i32 sp{};
        Slice<SpEntry> sps{};
    };

    struct Anonymous93 {
        i32 b{};
        Int i{};
    };

    static bool operator==(const Anonymous93& a, const Anonymous93& b) {
        return a.b == b.b && a.i == b.i;
    }

    static u64 hashValue(const Anonymous93& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.b));
        result = combineHash(result, hashValue(value.i));
        return result;
    }

    struct FuncType {
        Slice<ValType> params{};
        Slice<ValType> results{};
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

    struct TrapFixup {
        i32 at{};
        i64 code{};
        i32 sp{};
    };

    static bool operator==(const TrapFixup& a, const TrapFixup& b) {
        return a.at == b.at && a.code == b.code && a.sp == b.sp;
    }

    static u64 hashValue(const TrapFixup& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.at));
        result = combineHash(result, hashValue(value.code));
        result = combineHash(result, hashValue(value.sp));
        return result;
    }

    struct Anonymous68 {
        i32 block{};
        Int edge{};
    };

    static bool operator==(const Anonymous68& a, const Anonymous68& b) {
        return a.block == b.block && a.edge == b.edge;
    }

    static u64 hashValue(const Anonymous68& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.block));
        result = combineHash(result, hashValue(value.edge));
        return result;
    }

    struct Lowerer {
        Module* m{};
        Function* f{};
        Layout* lay{};
        CfgInfo* ci{};
        MFunc* mf{};
        Vec<VReg> vregs{};
        Vec<i32> uses{};
        Vec<bool> deferred{};
        Vec<i32> mblk{};
        i32 cur{};
        Map<Array<i32, 2>, i32> split{};
        Map<i32, i64> checked{};
        Slice<CheckUndo> checkUndo{};
        Slice<Slice<i32>> floatHomes{};
        Vec<bool> rawTrunc{};
        UnsignedRanges* addresses{};
    };

    struct AddressExpressions {
        Function* f{};
        Slice<AddressExpression> cache{};
        Slice<bool> seen{};
    };

    struct Mem {
        VReg base{};
        VReg index{};
        u8 scale{};
        i32 disp{};
        i32 label{};
    };

    static bool operator==(const Mem& a, const Mem& b) {
        return a.base == b.base && a.index == b.index && a.scale == b.scale && a.disp == b.disp && a.label == b.label;
    }

    static u64 hashValue(const Mem& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.base));
        result = combineHash(result, hashValue(value.index));
        result = combineHash(result, hashValue(value.scale));
        result = combineHash(result, hashValue(value.disp));
        result = combineHash(result, hashValue(value.label));
        return result;
    }

    struct VisitMir346 {
        i32 block{};
        Int edge{};
    };

    static bool operator==(const VisitMir346& a, const VisitMir346& b) {
        return a.block == b.block && a.edge == b.edge;
    }

    static u64 hashValue(const VisitMir346& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.block));
        result = combineHash(result, hashValue(value.edge));
        return result;
    }

    struct Reader {
        Slice<u8> data{};
        Int pos{};
    };

    struct Anonymous10 {
        i32 b{};
        Int i{};
    };

    static bool operator==(const Anonymous10& a, const Anonymous10& b) {
        return a.b == b.b && a.i == b.i;
    }

    static u64 hashValue(const Anonymous10& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.b));
        result = combineHash(result, hashValue(value.i));
        return result;
    }

    struct ConstKey {
        ValType typ{};
        i64 bits{};
    };

    static bool operator==(const ConstKey& a, const ConstKey& b) {
        return a.typ == b.typ && a.bits == b.bits;
    }

    static u64 hashValue(const ConstKey& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.typ));
        result = combineHash(result, hashValue(value.bits));
        return result;
    }

    struct Frontend {
        Module* m{};
        Function* f{};
        Reader r{};
        Slice<ValType> locals{};
        Slice<i32> stack{};
        Slice<Control> ctrls{};
        i32 cur{};
        Slice<Slice<i32>> defs{};
        Slice<bool> sealed{};
        Slice<Slice<PendingPhi>> incomplete{};
        Slice<Slice<PredRef>> preds{};
        Vec<i32> alias{};
        Map<ConstKey, i32> consts{};
        Map<i32, Slice<i32>> phiOps{};
        Slice<Int> explicitValue{};
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

    // A power-of-two stride makes paged value indexing a shift and mask.
    struct alignas(16) Value {
        Op op{};
        ValType typ{};
        u16 nargs{};
        i32 args{};
        i64 aux{};
        i32 aux2{};
        i32 block{};
    };

    static_assert(sizeof(Value) == 32);

    static bool operator==(const Value& a, const Value& b) {
        return a.op == b.op && a.typ == b.typ && a.nargs == b.nargs && a.args == b.args && a.aux == b.aux && a.aux2 == b.aux2 && a.block == b.block;
    }

    static u64 hashValue(const Value& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.op));
        result = combineHash(result, hashValue(value.typ));
        result = combineHash(result, hashValue(value.nargs));
        result = combineHash(result, hashValue(value.args));
        result = combineHash(result, hashValue(value.aux));
        result = combineHash(result, hashValue(value.aux2));
        result = combineHash(result, hashValue(value.block));
        return result;
    }

    struct JumpFixup {
        i32 at{};
        i32 block{};
    };

    static bool operator==(const JumpFixup& a, const JumpFixup& b) {
        return a.at == b.at && a.block == b.block;
    }

    static u64 hashValue(const JumpFixup& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.at));
        result = combineHash(result, hashValue(value.block));
        return result;
    }

    struct MemoryMeta {
        u64 min{};
        u64 max{};
        bool imported{};
        u64 tail{};
        u32 tailEnd{};
        u32 profile{};
        u32 profileFuncs{};
    };

    static bool operator==(const MemoryMeta& a, const MemoryMeta& b) {
        return a.min == b.min && a.max == b.max && a.imported == b.imported && a.tail == b.tail && a.tailEnd == b.tailEnd && a.profile == b.profile && a.profileFuncs == b.profileFuncs;
    }

    static u64 hashValue(const MemoryMeta& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.min));
        result = combineHash(result, hashValue(value.max));
        result = combineHash(result, hashValue(value.imported));
        result = combineHash(result, hashValue(value.tail));
        result = combineHash(result, hashValue(value.tailEnd));
        result = combineHash(result, hashValue(value.profile));
        result = combineHash(result, hashValue(value.profileFuncs));
        return result;
    }

    struct UnsignedRange {
        u64 lo{};
        u64 hi{};
    };

    static bool operator==(const UnsignedRange& a, const UnsignedRange& b) {
        return a.lo == b.lo && a.hi == b.hi;
    }

    static u64 hashValue(const UnsignedRange& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.lo));
        result = combineHash(result, hashValue(value.hi));
        return result;
    }

    struct AddressExpression {
        Array<AddressTerm, 4> terms{};
        u32 offset{};
    };

    static bool operator==(const AddressExpression& a, const AddressExpression& b) {
        return a.terms == b.terms && a.offset == b.offset;
    }

    static u64 hashValue(const AddressExpression& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.terms));
        result = combineHash(result, hashValue(value.offset));
        return result;
    }

    struct IntervalQueue {
        struct Entry {
            Interval* interval;
            i32 start;
            u32 order;
        };

        static bool before(const Entry& a, const Entry& b);
        void push(Interval* interval, i32 start);
        bool empty() const;
        Interval* pop();

        // Initial intervals keep their sorted order. Only splits need a
        // mutable heap; pop merges the two streams with stable ties.
        PageVec<Entry> initial;
        Vec<Entry> entries;
        usize nextInitial = 0;
        u32 nextOrder = 0;
    };

    bool IntervalQueue::before(const Entry& a, const Entry& b) {
        return a.start < b.start || (a.start == b.start && a.order < b.order);
    }

    void IntervalQueue::push(Interval* interval, i32 start) {
        require(nextOrder != 0xffffffffu, "too many pending intervals");
        Entry entry{interval, start, nextOrder++};
        usize at = entries.size();
        entries.pushBack(entry);
        while (at) {
            const usize parent = (at - 1) / 2;
            if (!before(entry, entries[parent])) {
                break;
            }
            entries[at] = entries[parent];
            at = parent;
        }
        entries[at] = entry;
    }

    Interval* IntervalQueue::pop() {
        require(!empty(), "empty interval queue");
        if (nextInitial < initial.size() && (entries.size() == 0 || before(initial[nextInitial], entries[0]))) {
            return initial[nextInitial++].interval;
        }
        Interval* result = entries[0].interval;
        const Entry last = entries.popBack();
        const usize count = entries.size();
        if (count) {
            usize at = 0;
            for (usize child = 1; child < count; child = at * 2 + 1) {
                if (child + 1 < count && before(entries[child + 1], entries[child])) {
                    ++child;
                }
                if (!before(entries[child], last)) {
                    break;
                }
                entries[at] = entries[child];
                at = child;
            }
            entries[at] = last;
        }
        return result;
    }

    bool IntervalQueue::empty() const {
        return nextInitial == initial.size() && entries.size() == 0;
    }

    struct PositionedMoves {
        struct Entry {
            i32 position;
            u32 order;
            Move move;
        };

        Vec<Entry> entries;
        u32 nextOrder = 0;
    };

    static void appendMove(PositionedMoves& moves, i32 position, Move move) {
        require(moves.nextOrder != 0xffffffffu, "too many interval moves");
        moves.entries.pushBack({position, moves.nextOrder++, move});
    }

    struct Allocator {
        MFunc* mf{};
        Vec<i32> order{};
        Vec<i32> blockFrom{};
        Vec<i32> blockTo{};
        Vec<Bitset> liveIn{};
        Vec<Bitset> liveOut{};
        Vec<i32> copyIndex{};
        Vec<VReg> copyRegs{};
        Vec<Interval*> intervals{};
        Vec<CopyInterval*> copyIntervals;
        Array<Interval*, 32> fixed{};
        IntervalQueue unhandled{};
        Vec<Interval*> active{};
        Vec<Interval*> inactive{};
        Vec<i32> slots{};
        i32 nslots{};
        Vec<i32> posBlock{};
        Array<bool, 32> used{};
    };

    struct Bypass {
        i32 pred{};
        Slice<i32> args{};
    };

    struct Meta {
        u32 instanceSize{};
        MemoryMeta* memory{};
        Slice<TableMeta> tables{};
        Slice<GlobalMeta> globals{};
        Slice<ImportMeta> imports{};
        Slice<ExportMeta> exports{};
        String init{};
        Slice<Region> regions{};
    };

    struct CfgInfo {
        Slice<i32> order{};
        Slice<i32> rpoIndex{};
        Slice<i32> idom{};
        Slice<i32> depth{};
        Slice<bool> header{};
    };

    struct DominatorChildren {
        Vec<i32> first;
        Vec<i32> next;
    };

    struct GlobalMeta {
        u32 offset{};
        ValType typ{};
        bool mutableValue{};
        bool imported{};
    };

    static bool operator==(const GlobalMeta& a, const GlobalMeta& b) {
        return a.offset == b.offset && a.typ == b.typ && a.mutableValue == b.mutableValue && a.imported == b.imported;
    }

    static u64 hashValue(const GlobalMeta& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.offset));
        result = combineHash(result, hashValue(value.typ));
        result = combineHash(result, hashValue(value.mutableValue));
        result = combineHash(result, hashValue(value.imported));
        return result;
    }

    struct UnsignedRanges {
        Function* f{};
        Slice<UnsignedRange> cache{};
        Slice<bool> seen{};
    };

    struct PendingPhi {
        i32 local{};
        i32 phi{};
    };

    static bool operator==(const PendingPhi& a, const PendingPhi& b) {
        return a.local == b.local && a.phi == b.phi;
    }

    static u64 hashValue(const PendingPhi& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.local));
        result = combineHash(result, hashValue(value.phi));
        return result;
    }

    struct CseVisit {
        i32 block{};
        bool leave{};
        usize base{};
    };

    struct FloatRange {
        double lo{};
        double hi{};
        bool nan{};
    };

    static bool operator==(const FloatRange& a, const FloatRange& b) {
        return a.lo == b.lo && a.hi == b.hi && a.nan == b.nan;
    }

    static u64 hashValue(const FloatRange& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.lo));
        result = combineHash(result, hashValue(value.hi));
        result = combineHash(result, hashValue(value.nan));
        return result;
    }

    struct FrameCfg34 {
        i32 b{};
        Int i{};
    };

    static bool operator==(const FrameCfg34& a, const FrameCfg34& b) {
        return a.b == b.b && a.i == b.i;
    }

    static u64 hashValue(const FrameCfg34& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.b));
        result = combineHash(result, hashValue(value.i));
        return result;
    }

    struct ImportMeta {
        u8 kind{};
        String module{};
        String name{};
        u32 index{};
        String sym{};
        Slice<ValType> params{};
        Slice<ValType> results{};
    };

    struct LowerVisit {
        i32 block{};
        bool leave{};
        Int undo{};
    };

    static bool operator==(const LowerVisit& a, const LowerVisit& b) {
        return a.block == b.block && a.leave == b.leave && a.undo == b.undo;
    }

    static u64 hashValue(const LowerVisit& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.block));
        result = combineHash(result, hashValue(value.leave));
        result = combineHash(result, hashValue(value.undo));
        return result;
    }

    struct CheckedEdge {
        i32 pred{};
        Edge* edge{};
    };

    static bool operator==(const CheckedEdge& a, const CheckedEdge& b) {
        return a.pred == b.pred && a.edge == b.edge;
    }

    static u64 hashValue(const CheckedEdge& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.pred));
        result = combineHash(result, hashValue(value.edge));
        return result;
    }

    struct TableFixup {
        i32 lea{};
        i32 end{};
        Span<i32> table{};
    };

    struct CseKey {
        Op op{};
        ValType typ{};
        u16 nargs{};
        i64 aux{};
        i32 aux2{};
        Array<i32, 3> args{};
    };

    static bool operator==(const CseKey& a, const CseKey& b) {
        return a.op == b.op && a.typ == b.typ && a.nargs == b.nargs && a.aux == b.aux && a.aux2 == b.aux2 && a.args == b.args;
    }

    static u64 hashValue(const CseKey& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.op));
        result = combineHash(result, hashValue(value.typ));
        result = combineHash(result, hashValue(value.nargs));
        result = combineHash(result, hashValue(value.aux));
        result = combineHash(result, hashValue(value.aux2));
        result = combineHash(result, hashValue(value.args));
        return result;
    }

    struct CompileOptions {
        bool memoryTail{};
        bool tailRanges{};
        bool tailProfile{};
    };

    static bool operator==(const CompileOptions& a, const CompileOptions& b) {
        return a.memoryTail == b.memoryTail && a.tailRanges == b.tailRanges && a.tailProfile == b.tailProfile;
    }

    static u64 hashValue(const CompileOptions& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.memoryTail));
        result = combineHash(result, hashValue(value.tailRanges));
        result = combineHash(result, hashValue(value.tailProfile));
        return result;
    }

    struct FloatIntegerNarrow {
        Optimizer* o{};
        Slice<u8> valid{};
        Map<i32, i32> values{};
    };

    struct TextAddress {
        i32 at{};
        i32 target{};
    };

    static bool operator==(const TextAddress& a, const TextAddress& b) {
        return a.at == b.at && a.target == b.target;
    }

    static u64 hashValue(const TextAddress& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.at));
        result = combineHash(result, hashValue(value.target));
        return result;
    }

    struct MFunc {
        NodeVec<MBlock> blocks{};
        Int nregs{};
        Slice<u8> classValue{};
        Int stackArgs{};
        Int incoming{};
        bool hasCalls{};
        bool returnsTwice{};
        u8 registerClasses{};
        Slice<u64> consts{};
    };

    static_assert(sizeof(MFunc) == (sizeof(void*) == 8 ? 112 : 68));

    struct TailRangeRegion {
        Slice<TailRangeGroup*> groups{};
        Int start{};
        Int end{};
    };

    struct RangeKeyLoop_version570 {
        i32 base{};
        i32 base2{};
        i32 stride{};
        u64 mul{};
        u64 mul2{};
        u64 factor{};
    };

    static bool operator==(const RangeKeyLoop_version570& a, const RangeKeyLoop_version570& b) {
        return a.base == b.base && a.base2 == b.base2 && a.stride == b.stride && a.mul == b.mul && a.mul2 == b.mul2 && a.factor == b.factor;
    }

    static u64 hashValue(const RangeKeyLoop_version570& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.base));
        result = combineHash(result, hashValue(value.base2));
        result = combineHash(result, hashValue(value.stride));
        result = combineHash(result, hashValue(value.mul));
        result = combineHash(result, hashValue(value.mul2));
        result = combineHash(result, hashValue(value.factor));
        return result;
    }

    struct Function {
        Module* module{};
        Int index{};
        FuncType* typ{};
        PageVec<Value> values{};
        PageVec<i32> pool{};
        ObjPool blockPool;
        NodeVec<Block> blocks;

        Function();
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

    struct ExportMeta {
        u8 kind{};
        String name{};
        u32 index{};
        String sym{};
        Slice<ValType> params{};
        Slice<ValType> results{};
    };

    static bool operator==(const Lrange& a, const Lrange& b) {
        return a.from == b.from && a.to == b.to;
    }

    static u64 hashValue(const Lrange& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.from));
        result = combineHash(result, hashValue(value.to));
        return result;
    }

    struct CallFixup {
        i32 at{};
        i32 fn{};
    };

    static bool operator==(const CallFixup& a, const CallFixup& b) {
        return a.at == b.at && a.fn == b.fn;
    }

    static u64 hashValue(const CallFixup& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.at));
        result = combineHash(result, hashValue(value.fn));
        return result;
    }

    struct Anonymous24 {
        Int index{};
        String name{};
    };

    static bool operator==(const Anonymous24& a, const Anonymous24& b) {
        return a.index == b.index && a.name == b.name;
    }

    static u64 hashValue(const Anonymous24& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.index));
        result = combineHash(result, hashValue(value.name));
        return result;
    }

    struct GoLoc {
        VReg reg{};
        i32 off{};
    };

    static bool operator==(const GoLoc& a, const GoLoc& b) {
        return a.reg == b.reg && a.off == b.off;
    }

    static u64 hashValue(const GoLoc& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.reg));
        result = combineHash(result, hashValue(value.off));
        return result;
    }

    struct Func {
        u32 typ{};
        bool imported{};
        bool returnsTwice{};
        String name{};
        Slice<u8> body{};
        Int offset{};
    };

    struct CopyAddress {
        Map<i32, u32> terms{};
        u32 offset{};
        u32 step{};
        bool valid{};
    };

    struct TailRangeGroup {
        Int start{};
        VReg src{};
        AddressExpression expr{};
        Slice<Int> checks{};
    };

    struct ObjCode {
        ObjPool* pool{};
        Module* m{};
        Layout* lay{};
        Asm a{};
        Slice<i32> funcAt{};
        Array<i32, 6> stubAt{};
        Slice<CallFixup> calls{};
        Slice<CallFixup> stubCalls{};
        Map<i32, i32> jumpAt{};
        Slice<CallFixup> jumpCalls{};
        Slice<CodeBlob> blobs{};
        Slice<i32> blobAt{};
        u64 rodataSize{};
        Map<Array<u64, 2>, i32> floatIx{};
        Slice<String> externs{};
        Map<String, i32> externIx{};
        Slice<TextReloc> relocs{};
        Slice<TextAddress> addresses{};
        Slice<Region> regions{};
        String regionName{};
        Map<String, bool> globals{};
        Int initFn{};
        i32 dataInitTable = -1;
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

    struct MInstExtra {
        Vec<VReg> uses;
        Vec<VReg> defs;
        Vec<VReg> temps;
        Vec<Move> moves;
        Vec<i32> table;
    };

    struct MInst {
        MOp op{};
        u8 size{};
        u8 dsize{};
        u8 cc{};
        u8 aux{};
        VReg dst{};
        VReg src{};
        VReg src2{};
        i64 imm{};
        Mem mem{};

        union {
            i32 target{};
            // mCheck retains the logical address even if registers are reused.
            i32 addressValue;
        };

        MInstExtra* extra = nullptr;
    };

    struct MBlock {
        Vec<MInst> insts{};
        Slice<i32> succs{};
        Slice<i32> preds{};
        Int depth{};
        i32 id{};
        bool cold{};
    };

    static MInstExtra* mInstExtra(MInst* in) {
        if (!in->extra) {
            in->extra = activePool->make<MInstExtra>();
        }
        return in->extra;
    }

    static Span<VReg> mInstUses(MInst* in) {
        return in->extra ? in->extra->uses.view() : Span<VReg>{};
    }

    static Span<VReg> mInstDefs(MInst* in) {
        return in->extra ? in->extra->defs.view() : Span<VReg>{};
    }

    static Span<VReg> mInstTemps(MInst* in) {
        return in->extra ? in->extra->temps.view() : Span<VReg>{};
    }

    static Span<Move> mInstMoves(MInst* in) {
        return in->extra ? in->extra->moves.view() : Span<Move>{};
    }

    static Span<i32> mInstTable(MInst* in) {
        return in->extra ? in->extra->table.view() : Span<i32>{};
    }

    struct Anonymous4 {
        AddressExpression expr{};
        u32 mul{};
    };

    static bool operator==(const Anonymous4& a, const Anonymous4& b) {
        return a.expr == b.expr && a.mul == b.mul;
    }

    static u64 hashValue(const Anonymous4& value) {
        u64 result = 0;
        result = combineHash(result, hashValue(value.expr));
        result = combineHash(result, hashValue(value.mul));
        return result;
    }

    struct Block {
        Slice<i32> params{};
        Vec<i32> instrs{};
        Term term{};
        Slice<i32> preds{};
        bool dead{};
    };

    // Loop versioning only appends instructions to bodies. Existing body
    // operands only change when escaping values are replaced by fresh exit
    // parameters. Their counts are marked unknown; old counts remain upper
    // bounds. Terminator operands are handled by the original escape walk.
    struct LoopBodyUses {
        Function* function;
        Vec<u32> counts;
        Vec<Lrange> valueRanges;
        Vec<u32> instructionsSeen;
        usize nextValue = 0;
        u32 clones = 0;
        bool ready = false;

        explicit LoopBodyUses(Function* f)
            : function(f)
        {
        }

        void ensureSize() {
            const usize before = counts.size();
            counts.resize(function->values.size());
            for (usize i = before; i < counts.size(); ++i) {
                counts[i] = 0;
            }
        }

        bool prepare() {
            if (++clones < 5) {
                return false;
            }
            ensureSize();
            const usize seen = instructionsSeen.size();
            instructionsSeen.resize(function->blocks.size());
            for (usize i = seen; i < instructionsSeen.size(); ++i) {
                instructionsSeen[i] = 0;
            }
            const usize before = valueRanges.size();
            valueRanges.resize(function->blocks.size());
            for (usize i = before; i < valueRanges.size(); ++i) {
                valueRanges[i] = {0x7fffffff, 0};
            }
            for (; nextValue < function->values.size(); ++nextValue) {
                const i32 block = function->values[nextValue].block;
                if (block >= 0 && usize(block) < valueRanges.size()) {
                    auto& range = valueRanges[usize(block)];
                    range.from = range.from < i32(nextValue) ? range.from : i32(nextValue);
                    range.to = i32(nextValue + 1);
                }
            }
            for (usize b = 0; b < function->blocks.size(); ++b) {
                const auto& block = function->blocks[b];
                if (block.dead) {
                    continue;
                }
                for (usize i = instructionsSeen[b]; i < block.instrs.size(); ++i) {
                    const Value& value = function->values[block.instrs[i]];
                    for (u32 j = 0; j < value.nargs; ++j) {
                        u32& count = counts[usize(function->pool[usize(value.args) + j])];
                        if (count != 0xffffffffu) {
                            ++count;
                        }
                    }
                }
                instructionsSeen[b] = u32(block.instrs.size());
            }
            ready = true;
            return true;
        }

        void unknown(i32 value) {
            if (ready) {
                ensureSize();
                counts[usize(value)] = 0xffffffffu;
            }
        }
    };

    Function::Function() {
        blocks.owner = &blockPool;
    }

    static constexpr Int offMem = Int(0LL);

    static constexpr Int offMemSize = Int(8LL);

    static constexpr Int offMemBound = Int(16LL);

    static constexpr Int offStackLimit = Int(24LL);

    static constexpr Int offHost = Int(32LL);

    static constexpr Int offTokens = Int(40LL);

    static constexpr Int offJump = Int(48LL);

    static constexpr Int offBase = Int(104LL);

    static constexpr Int offInterrupt = Int(112LL);

    static constexpr Int headerSize = Int(120LL);

    static constexpr Int jumpPC = Int(0LL);

    static constexpr Int jumpSP = Int(1LL);

    static constexpr Int jumpBP = Int(2LL);

    static constexpr Int jumpR10 = Int(3LL);

    static constexpr Int jumpR11 = Int(4LL);

    static constexpr Int jumpR12 = Int(5LL);

    static constexpr Int jumpToken = Int(6LL);

    static constexpr Int jumpWords = Int(7LL);

    static constexpr Int boundSlack = Int(65536LL);

    static constexpr Int tailCounterBytes = Int(32LL);

    static constexpr Int tableEntries = Int(0LL);

    static constexpr Int tableSize = Int(8LL);

    static constexpr Int tableMax = Int(12LL);

    static constexpr Int tableBytes = Int(16LL);

    static constexpr Int descBytes = Int(16LL);

    static constexpr Int labelLocal = Int(0LL);

    static constexpr Int labelModule = Int(1LL);

    static constexpr Int labelFunc = Int(2LL);

    static constexpr Int labelGot = Int(3LL);

    static constexpr Int ppNone = Int(0LL);

    static constexpr Int pp66 = Int(1LL);

    static constexpr Int ppF3 = Int(2LL);

    static constexpr Int ppF2 = Int(3LL);

    static constexpr Int shtProgbits = Int(1LL);

    static constexpr Int shtSymtab = Int(2LL);

    static constexpr Int shtStrtab = Int(3LL);

    static constexpr Int shtRela = Int(4LL);

    static constexpr Int shtNote = Int(7LL);

    static constexpr Int shtNobits = Int(8LL);

    static constexpr Int shfAlloc = Int(2LL);

    static constexpr Int shfExec = Int(4LL);

    static constexpr Int shfInfoLink = Int(64LL);

    static constexpr Int rX86_64_64 = Int(1LL);

    static constexpr Int rX86_64_PC32 = Int(2LL);

    static constexpr Int rX86_64_GOTPCREL = Int(9LL);

    static constexpr Int rX86_64_GOTPCRELX = Int(41LL);

    static constexpr Int rX86_64_REX_GOTPCRELX = Int(42LL);

    static const String noteOwner = String("wasm2o");

    static constexpr Int inExternal = Int(0LL);

    static constexpr Int inText = Int(1LL);

    static constexpr Int inRodata = Int(2LL);

    static constexpr Int ctrlBlock = Int(0LL);

    static constexpr Int ctrlLoop = Int(1LL);

    static constexpr Int ctrlIf = Int(2LL);

    static constexpr Int ctrlElse = Int(3LL);

    static constexpr Int ctrlFunc = Int(4LL);

    static constexpr Int stubMorestack = Int(0LL);

    static constexpr Int stubMemmove = Int(1LL);

    static constexpr Int stubMemset = Int(2LL);

    static constexpr Int stubMemoryGrow = Int(3LL);

    static constexpr Int stubTableGrow = Int(4LL);

    static constexpr Int stubInterrupt = Int(5LL);

    static constexpr Int stubCount = Int(6LL);

    static constexpr Int gStackHi = Int(8LL);

    static constexpr Int gStackguard0 = Int(16LL);

    static constexpr Int stackSmall = Int(128LL);

    static constexpr Int leafFrame = Int(512LL);

    static constexpr Int framesCtx = Int(0LL);

    static constexpr Int framesSP = Int(8LL);

    static constexpr Int framesBP = Int(16LL);

    static constexpr Int framesFuncs = Int(24LL);

    static constexpr Int framesEntry = Int(40LL);

    static constexpr Int framesResume = Int(56LL);

    static constexpr Int framesBuf = Int(72LL);

    static constexpr Int framesCap = Int(80LL);

    static constexpr Int framesSize = Int(88LL);

    static constexpr Int framesTop = Int(96LL);

    static constexpr Int resumeCtx = Int(0LL);

    static constexpr Int resumeBuf = Int(8LL);

    static constexpr Int resumeSize = Int(16LL);

    static constexpr Int resumeTop = Int(24LL);

    static constexpr Int resumeBP = Int(32LL);

    static constexpr Int resumeSP = Int(40LL);

    static constexpr Int resumePC = Int(48LL);

    static constexpr Int resumeR10 = Int(56LL);

    static constexpr Int resumeSlack = Int(512LL);

    static constexpr Op OpInvalid = Op(0LL);

    static constexpr Op OpConst = Op(1LL);

    static constexpr Op OpParam = Op(2LL);

    static constexpr Op OpUndef = Op(3LL);

    static constexpr Op OpAdd = Op(4LL);

    static constexpr Op OpSub = Op(5LL);

    static constexpr Op OpMul = Op(6LL);

    static constexpr Op OpDivS = Op(7LL);

    static constexpr Op OpDivU = Op(8LL);

    static constexpr Op OpRemS = Op(9LL);

    static constexpr Op OpRemU = Op(10LL);

    static constexpr Op OpAnd = Op(11LL);

    static constexpr Op OpOr = Op(12LL);

    static constexpr Op OpXor = Op(13LL);

    static constexpr Op OpShl = Op(14LL);

    static constexpr Op OpShrS = Op(15LL);

    static constexpr Op OpShrU = Op(16LL);

    static constexpr Op OpRotl = Op(17LL);

    static constexpr Op OpRotr = Op(18LL);

    static constexpr Op OpClz = Op(19LL);

    static constexpr Op OpCtz = Op(20LL);

    static constexpr Op OpPopcnt = Op(21LL);

    static constexpr Op OpExtend8S = Op(22LL);

    static constexpr Op OpExtend16S = Op(23LL);

    static constexpr Op OpExtend32S = Op(24LL);

    static constexpr Op OpEqz = Op(25LL);

    static constexpr Op OpEq = Op(26LL);

    static constexpr Op OpNe = Op(27LL);

    static constexpr Op OpLtS = Op(28LL);

    static constexpr Op OpLtU = Op(29LL);

    static constexpr Op OpGtS = Op(30LL);

    static constexpr Op OpGtU = Op(31LL);

    static constexpr Op OpLeS = Op(32LL);

    static constexpr Op OpLeU = Op(33LL);

    static constexpr Op OpGeS = Op(34LL);

    static constexpr Op OpGeU = Op(35LL);

    static constexpr Op OpFAdd = Op(36LL);

    static constexpr Op OpFSub = Op(37LL);

    static constexpr Op OpFMul = Op(38LL);

    static constexpr Op OpFDiv = Op(39LL);

    static constexpr Op OpFMin = Op(40LL);

    static constexpr Op OpFMax = Op(41LL);

    static constexpr Op OpFCopysign = Op(42LL);

    static constexpr Op OpFAbs = Op(43LL);

    static constexpr Op OpFNeg = Op(44LL);

    static constexpr Op OpFSqrt = Op(45LL);

    static constexpr Op OpFCeil = Op(46LL);

    static constexpr Op OpFFloor = Op(47LL);

    static constexpr Op OpFTrunc = Op(48LL);

    static constexpr Op OpFNearest = Op(49LL);

    static constexpr Op OpFEq = Op(50LL);

    static constexpr Op OpFNe = Op(51LL);

    static constexpr Op OpFLt = Op(52LL);

    static constexpr Op OpFGt = Op(53LL);

    static constexpr Op OpFLe = Op(54LL);

    static constexpr Op OpFGe = Op(55LL);

    static constexpr Op OpWrap = Op(56LL);

    static constexpr Op OpExtendS = Op(57LL);

    static constexpr Op OpExtendU = Op(58LL);

    static constexpr Op OpTruncS = Op(59LL);

    static constexpr Op OpTruncU = Op(60LL);

    static constexpr Op OpTruncSatS = Op(61LL);

    static constexpr Op OpTruncSatU = Op(62LL);

    static constexpr Op OpConvertS = Op(63LL);

    static constexpr Op OpConvertU = Op(64LL);

    static constexpr Op OpDemote = Op(65LL);

    static constexpr Op OpPromote = Op(66LL);

    static constexpr Op OpReinterpret = Op(67LL);

    static constexpr Op OpSelect = Op(68LL);

    static constexpr Op OpLoad = Op(69LL);

    static constexpr Op OpStore = Op(70LL);

    static constexpr Op OpMemSize = Op(71LL);

    static constexpr Op OpMemGrow = Op(72LL);

    static constexpr Op OpMemCopy = Op(73LL);

    static constexpr Op OpMemFill = Op(74LL);

    static constexpr Op OpMemInit = Op(75LL);

    static constexpr Op OpDataDrop = Op(76LL);

    static constexpr Op OpGlobalGet = Op(77LL);

    static constexpr Op OpGlobalSet = Op(78LL);

    static constexpr Op OpCall = Op(79LL);

    static constexpr Op OpCallIndirect = Op(80LL);

    static constexpr Op OpExtract = Op(81LL);

    static constexpr Op OpRefNull = Op(82LL);

    static constexpr Op OpRefIsNull = Op(83LL);

    static constexpr Op OpRefFunc = Op(84LL);

    static constexpr Op OpTableGet = Op(85LL);

    static constexpr Op OpTableSet = Op(86LL);

    static constexpr Op OpTableSize = Op(87LL);

    static constexpr Op OpTableGrow = Op(88LL);

    static constexpr Op OpTableFill = Op(89LL);

    static constexpr Op OpTableCopy = Op(90LL);

    static constexpr Op OpTableInit = Op(91LL);

    static constexpr Op OpElemDrop = Op(92LL);

    static constexpr Op OpCopyForward = Op(93LL);

    static constexpr Op opCount = Op(94LL);

    static constexpr ValType TNone = ValType(0LL);

    static constexpr ValType TTuple = ValType(1LL);

    static constexpr MemKind memSigned = MemKind(16LL);

    static constexpr MemKind memProven = MemKind(32LL);

    static constexpr TermKind TermNone = TermKind(0LL);

    static constexpr TermKind TermJump = TermKind(1LL);

    static constexpr TermKind TermIf = TermKind(2LL);

    static constexpr TermKind TermSwitch = TermKind(3LL);

    static constexpr TermKind TermReturn = TermKind(4LL);

    static constexpr TermKind TermTrap = TermKind(5LL);

    static constexpr Int trapUnreachable = Int(1LL);

    static constexpr Int trapMemoryOOB = Int(2LL);

    static constexpr Int trapIntOverflow = Int(3LL);

    static constexpr Int trapDivByZero = Int(4LL);

    static constexpr Int trapConversion = Int(5LL);

    static constexpr Int trapIndirectType = Int(6LL);

    static constexpr Int trapUndefinedElem = Int(7LL);

    static constexpr Int trapUninitElem = Int(8LL);

    static constexpr Int trapStack = Int(9LL);

    static constexpr Int trapTableOOB = Int(10LL);

    static constexpr Int trapNullRef = Int(11LL);

    static constexpr Int calleeFunc = Int(0LL);

    static constexpr Int calleeStub = Int(1LL);

    static constexpr Int calleeJump = Int(2LL);

    static constexpr Int constF32Sign = Int(0LL);

    static constexpr Int constF32Abs = Int(1LL);

    static constexpr Int constF64Sign = Int(2LL);

    static constexpr Int constF64Abs = Int(3LL);

    static constexpr Int constCount = Int(4LL);

    static constexpr VReg RAX = VReg(0LL);

    static constexpr VReg RCX = VReg(1LL);

    static constexpr VReg RDX = VReg(2LL);

    static constexpr VReg RBX = VReg(3LL);

    static constexpr VReg RSP = VReg(4LL);

    static constexpr VReg RBP = VReg(5LL);

    static constexpr VReg RSI = VReg(6LL);

    static constexpr VReg RDI = VReg(7LL);

    static constexpr VReg R8 = VReg(8LL);

    static constexpr VReg R9 = VReg(9LL);

    static constexpr VReg R10 = VReg(10LL);

    static constexpr VReg R11 = VReg(11LL);

    static constexpr VReg R12 = VReg(12LL);

    static constexpr VReg R13 = VReg(13LL);

    static constexpr VReg R14 = VReg(14LL);

    static constexpr VReg R15 = VReg(15LL);

    static constexpr VReg XMM0 = VReg(16LL);

    static constexpr VReg noReg = VReg(-1LL);

    static constexpr VReg firstVirtual = VReg(32LL);

    static constexpr VReg ctxReg = VReg(13LL);

    static constexpr VReg memReg = VReg(15LL);

    static constexpr Int classGPR = Int(0LL);

    static constexpr Int classXMM = Int(1LL);

    static constexpr MOp mNop = MOp(0LL);

    static constexpr MOp mMov = MOp(1LL);

    static constexpr MOp mMovImm = MOp(2LL);

    static constexpr MOp mLoad = MOp(3LL);

    static constexpr MOp mStore = MOp(4LL);

    static constexpr MOp mStoreImm = MOp(5LL);

    static constexpr MOp mLea = MOp(6LL);

    static constexpr MOp mAlu = MOp(7LL);

    static constexpr MOp mAluImm = MOp(8LL);

    static constexpr MOp mAluMem = MOp(9LL);

    static constexpr MOp mImul3 = MOp(10LL);

    static constexpr MOp mCmp = MOp(11LL);

    static constexpr MOp mCmpImm = MOp(12LL);

    static constexpr MOp mCmpMem = MOp(13LL);

    static constexpr MOp mCmpMemImm = MOp(14LL);

    static constexpr MOp mTest = MOp(15LL);

    static constexpr MOp mTestImm = MOp(16LL);

    static constexpr MOp mSetcc = MOp(17LL);

    static constexpr MOp mCmov = MOp(18LL);

    static constexpr MOp mShiftImm = MOp(19LL);

    static constexpr MOp mShiftX = MOp(20LL);

    static constexpr MOp mRotCL = MOp(21LL);

    static constexpr MOp mUnary = MOp(22LL);

    static constexpr MOp mBitCount = MOp(23LL);

    static constexpr MOp mExt = MOp(24LL);

    static constexpr MOp mDiv = MOp(25LL);

    static constexpr MOp mSignExtAx = MOp(26LL);

    static constexpr MOp mZeroDx = MOp(27LL);

    static constexpr MOp mFMov = MOp(28LL);

    static constexpr MOp mFLoad = MOp(29LL);

    static constexpr MOp mFStore = MOp(30LL);

    static constexpr MOp mFConst = MOp(31LL);

    static constexpr MOp mFAlu = MOp(32LL);

    static constexpr MOp mFUnary = MOp(33LL);

    static constexpr MOp mFCmp = MOp(34LL);

    static constexpr MOp mCvtIF = MOp(35LL);

    static constexpr MOp mCvtFI = MOp(36LL);

    static constexpr MOp mCvtFF = MOp(37LL);

    static constexpr MOp mMovGX = MOp(38LL);

    static constexpr MOp mMovXG = MOp(39LL);

    static constexpr MOp mJmp = MOp(40LL);

    static constexpr MOp mJcc = MOp(41LL);

    static constexpr MOp mSwitch = MOp(42LL);

    static constexpr MOp mCall = MOp(43LL);

    static constexpr MOp mCallInd = MOp(44LL);

    static constexpr MOp mRet = MOp(45LL);

    static constexpr MOp mTrap = MOp(46LL);

    static constexpr MOp mTrapIf = MOp(47LL);

    static constexpr MOp mCheck = MOp(48LL);

    static constexpr MOp mParMove = MOp(49LL);

    static constexpr MOp mStackArg = MOp(50LL);

    static constexpr MOp mLoadArg = MOp(51LL);

    static constexpr MOp mStoreRet = MOp(52LL);

    static constexpr MOp mGrowMem = MOp(53LL);

    static constexpr MOp mMemCopy = MOp(54LL);

    static constexpr MOp mMemFill = MOp(55LL);

    static constexpr MOp mSpillStore = MOp(56LL);

    static constexpr MOp mSpillLoad = MOp(57LL);

    static constexpr MOp mDivOvf = MOp(58LL);

    static constexpr MOp mTruncF = MOp(59LL);

    static constexpr MOp mFMinMax = MOp(60LL);

    static constexpr MOp mCvtU64F = MOp(61LL);

    static constexpr MOp mFAluMem = MOp(62LL);

    static constexpr MOp mFSelect = MOp(63LL);

    static constexpr MOp mMemOp = MOp(64LL);

    static constexpr MOp mCopyForward = MOp(65LL);

    static constexpr MOp mCounter = MOp(66LL);

    static constexpr Int truncUnsigned = Int(1LL);

    static constexpr Int truncSat = Int(2LL);

    static constexpr AluOp aluAdd = AluOp(0LL);

    static constexpr AluOp aluOr = AluOp(1LL);

    static constexpr AluOp aluAdc = AluOp(2LL);

    static constexpr AluOp aluSbb = AluOp(3LL);

    static constexpr AluOp aluAnd = AluOp(4LL);

    static constexpr AluOp aluSub = AluOp(5LL);

    static constexpr AluOp aluXor = AluOp(6LL);

    static constexpr AluOp aluCmp = AluOp(7LL);

    static constexpr AluOp aluImul = AluOp(8LL);

    static constexpr Int shiftRol = Int(0LL);

    static constexpr Int shiftRor = Int(1LL);

    static constexpr Int shiftShl = Int(4LL);

    static constexpr Int shiftShr = Int(5LL);

    static constexpr Int shiftSar = Int(7LL);

    static constexpr FAluOp fAdd = FAluOp(0LL);

    static constexpr FAluOp fSub = FAluOp(1LL);

    static constexpr FAluOp fMul = FAluOp(2LL);

    static constexpr FAluOp fDiv = FAluOp(3LL);

    static constexpr FAluOp fMin = FAluOp(4LL);

    static constexpr FAluOp fMax = FAluOp(5LL);

    static constexpr FAluOp fAnd = FAluOp(6LL);

    static constexpr FAluOp fAndn = FAluOp(7LL);

    static constexpr FAluOp fOr = FAluOp(8LL);

    static constexpr FAluOp fXor = FAluOp(9LL);

    static constexpr Int fSqrt = Int(0LL);

    static constexpr Int fRound = Int(1LL);

    static constexpr Int ccO = Int(0LL);

    static constexpr Int ccNO = Int(1LL);

    static constexpr Int ccB = Int(2LL);

    static constexpr Int ccAE = Int(3LL);

    static constexpr Int ccE = Int(4LL);

    static constexpr Int ccNE = Int(5LL);

    static constexpr Int ccBE = Int(6LL);

    static constexpr Int ccA = Int(7LL);

    static constexpr Int ccS = Int(8LL);

    static constexpr Int ccNS = Int(9LL);

    static constexpr Int ccP = Int(10LL);

    static constexpr Int ccNP = Int(11LL);

    static constexpr Int ccL = Int(12LL);

    static constexpr Int ccGE = Int(13LL);

    static constexpr Int ccLE = Int(14LL);

    static constexpr Int ccG = Int(15LL);

    static constexpr Int roleUse = Int(1LL);

    static constexpr Int roleDef = Int(2LL);

    static constexpr Int roleUseDef = Int(3LL);

    static constexpr Int roleTemp = Int(4LL);

    static constexpr ValType I32 = ValType(127LL);

    static constexpr ValType I64 = ValType(126LL);

    static constexpr ValType F32 = ValType(125LL);

    static constexpr ValType F64 = ValType(124LL);

    static constexpr ValType V128 = ValType(123LL);

    static constexpr ValType FuncRef = ValType(112LL);

    static constexpr ValType ExternRef = ValType(111LL);

    static constexpr Int externFunc = Int(0LL);

    static constexpr Int externTable = Int(1LL);

    static constexpr Int externMemory = Int(2LL);

    static constexpr Int externGlobal = Int(3LL);

    static constexpr Int segmentActive = Int(0LL);

    static constexpr Int segmentPassive = Int(1LL);

    static constexpr Int segmentDeclarative = Int(2LL);

    static constexpr i32 maxPos = i32(1073741824LL);

    static const String symInit = String("w2o.init");

    static const String symLand = String("w2o.land");

    static const String symFrames = String("w2o.frames");

    static const String symResume = String("w2o.resume");

    static const String symImport = String("w2o.import.");

    static const String symExport = String("w2o.export.");

    static const String symMorestack = String("runtime.morestack_noctxt");

    static const String symTrap = String("w2o.trap");

    static const String symMemoryGrow = String("w2o.memory_grow");

    static const String symTableGrow = String("w2o.table_grow");

    static const String symInterrupt = String("w2o.interrupt");

    static const String symInterruptCall = String("w2o.interrupt_call");

    static constexpr SortedHint unknownHint = SortedHint(0LL);

    static constexpr SortedHint increasingHint = SortedHint(1LL);

    static constexpr SortedHint decreasingHint = SortedHint(2LL);

    static AddressExpression addressExpressionsOf(AddressExpressions* r, i32 v, Int depth);

    static bool floatIntegerNarrowCan(FloatIntegerNarrow* n, i32 v, Int depth);

    static i32 floatIntegerNarrowInteger(FloatIntegerNarrow* n, i32 v);

    static void floatRangesAdd(FloatRanges* r, i32 v, FloatRange bound);

    static void floatRangesConstrain(FloatRanges* r, i32 cond, bool truth);

    static FloatRange floatRangesOf(FloatRanges* r, i32 v, Int depth);

    static CfgInfo* functionAnalyze(Function* f);

    static Span<i32> functionArgs(Function* f, i32 v);

    static void functionComputePreds(Function* f);

    static i32 functionNewBlock(Function* f);

    static i32 functionNewValue(Function* f, Op op, ValType typ, i32 block, i64 aux, Span<const i32> args);

    static Slice<i32> functionRpo(Function* f);

    static i32 layoutDataDropped(Layout* l, Int seg);

    static i32 layoutDesc(Layout* l, Int f);

    static i32 layoutElemDropped(Layout* l, Module* m, Int seg);

    static Slice<i32> mFuncHotOrder(MFunc* mf, Slice<i32> order);

    static VReg mFuncNewReg(MFunc* mf, u8 classValue);

    static u8 mFuncRegClass(MFunc* mf, VReg r);

    static Slice<VReg> mInstClobberSet(MInst* in, u8 registerClasses);

    template <typename F>
    static void mInstForEachOperand(MInst* in, F cb);

    static i32 memoryLoopClone(MemoryLoop* l, Map<i32, bool> proven, Map<i32, i32> invariants, Map<i32, Int> cases);

    static CopyAddress memoryLoopCopyAddress(MemoryLoop* l, i32 v, Slice<i64> steps, Map<i32, i32> invariants, Map<i32, CopyAddress> cache, Int depth);

    static i32 memoryLoopCopyLoop(MemoryLoop* l, i32 fast, i32 count, i32 size, Map<i32, bool> proven, Map<i32, i32> invariants);

    static bool memoryLoopHasLiveOut(MemoryLoop* l);

    static bool memoryLoopInvariant(MemoryLoop* l, i32 v, Int depth);

    static LoopSpan memoryLoopSpan(MemoryLoop* l, i32 v, Int depth);

    static i32 memoryLoopVersion(MemoryLoop* l);

    static i32 memoryLoopVersionOne(MemoryLoop* l);

    static Int moduleAddInitFunction(Module* m);

    static void moduleDataSection(Module* m, Reader* r);

    static void moduleElemSection(Module* m, Reader* r);

    static void moduleExportSection(Module* m, Reader* r);

    static FuncType* moduleFuncType(Module* m, u32 index);

    static void moduleImportSection(Module* m, Reader* r);

    static void moduleTypeSection(Module* m, Reader* r);

    static UnsignedRange unsignedRangesOf(UnsignedRanges* r, i32 v, Int depth);

    static void allocatorAddUnhandled(Allocator* a, Interval* it);

    static i32 allocatorAdvance(Allocator* a, i32 pos);

    static void allocatorAllocateBlocked(Allocator* a, Interval* cur);

    template <bool copiesOnly = false>
    static void allocatorBuildIntervals(Allocator* a);

    static Slice<VReg> allocatorCalleeSavedUsed(Allocator* a);

    static bool allocatorCanEvict(Allocator* a, Interval* it, i32 pos);

    static Slice<VReg> allocatorCandidates(Allocator* a, u8 classValue);

    static bool allocatorCoalesce(Allocator* a);

    static bool allocatorSelectCopies(Allocator* a);

    static void allocatorEvict(Allocator* a, Interval* it, i32 pos);

    static void allocatorHint(Allocator* a, VReg dst, VReg src);

    static VReg allocatorHintReg(Allocator* a, Interval* cur);

    static Interval* allocatorInterval(Allocator* a, VReg r);

    static void allocatorLinearize(Allocator* a);

    static void allocatorLiveness(Allocator* a, bool copiesOnly = false);

    static VReg allocatorLoc(Allocator* a, Interval* it);

    static VReg allocatorLocation(Allocator* a, VReg r, i32 p);

    static u8 allocatorRegClass(Allocator* a, VReg r);

    static void allocatorResolve(Allocator* a);

    static void allocatorSaveSpillsBeforeLoops(Allocator* a, PositionedMoves& inner, Map<i32, Slice<Move>> atEnd, Map<i32, Slice<Move>> atStart);

    static void allocatorScan(Allocator* a);

    static void allocatorSpill(Allocator* a, Interval* it);

    static i32 allocatorSplitPos(Allocator* a, i32 min, i32 max);

    static bool allocatorTryFree(Allocator* a, Interval* cur);

    static Int allocatorVindex(Allocator* a, VReg r);

    static void asmAluMI(Asm* a, AluOp op, u8 size, Amem m, i64 x);

    static void asmAluRI(Asm* a, AluOp op, u8 size, VReg dst, i64 x);

    static void asmAluRIRaw(Asm* a, AluOp op, u8 size, VReg dst, i64 x);

    static void asmAluRM(Asm* a, AluOp op, u8 size, VReg dst, Amem m);

    static void asmAluRR(Asm* a, AluOp op, u8 size, VReg dst, VReg src);

    static void asmB(Asm* a, Span<const u8> bytes);

    static void asmBitCount(Asm* a, u8 kind, u8 size, VReg dst, VReg src);

    static void asmCallMem(Asm* a, Amem m);

    static void asmCallReg(Asm* a, VReg r);

    static i32 asmCallRel(Asm* a);

    static void asmCmov(Asm* a, u8 cc, u8 size, VReg dst, VReg src);

    static void asmCvtFF(Asm* a, u8 from, VReg dst, VReg src);

    static void asmCvtFI(Asm* a, u8 isize, u8 fsize, VReg dst, VReg src);

    static void asmCvtIF(Asm* a, u8 isize, u8 fsize, VReg dst, VReg src);

    static void asmDiv(Asm* a, bool signedValue, u8 size, VReg src);

    static void asmExt(Asm* a, u8 width, u8 dsize, bool signedValue, VReg dst, VReg src);

    static void asmFalu(Asm* a, FAluOp op, u8 size, VReg dst, VReg x, VReg y);

    static void asmFaluM(Asm* a, FAluOp op, u8 size, VReg dst, VReg x, Amem m);

    static void asmFblend(Asm* a, u8 size, VReg dst, VReg x, VReg y, VReg mask);

    static void asmFcmp(Asm* a, u8 size, VReg x, VReg y);

    static void asmFcmpM(Asm* a, u8 size, VReg x, Amem m);

    static void asmFcmpMask(Asm* a, u8 size, VReg dst, VReg x, VReg y, u8 predicate);

    static void asmFload(Asm* a, u8 size, VReg dst, Amem m);

    static void asmFmov(Asm* a, VReg dst, VReg src);

    static void asmFround(Asm* a, u8 size, VReg dst, VReg src, i64 mode);

    static void asmFsqrt(Asm* a, u8 size, VReg dst, VReg src);

    static void asmFstore(Asm* a, u8 size, VReg src, Amem m);

    static void asmFxor(Asm* a, VReg dst);

    static void asmHere8(Asm* a, i32 at);

    static void asmImul3(Asm* a, u8 size, VReg dst, VReg src, i64 x);

    static i32 asmJcc8(Asm* a, u8 cc);

    static i32 asmJccRel(Asm* a, u8 cc);

    static i32 asmJmp8(Asm* a);

    static i32 asmJmpRel(Asm* a);

    static void asmLea(Asm* a, u8 size, VReg dst, Amem m);

    static void asmLoad(Asm* a, u8 width, u8 dsize, bool signedValue, VReg dst, Amem m);

    static void asmModrmMem(Asm* a, Int reg, Amem m, Int imm);

    static void asmModrmReg(Asm* a, Int reg, Int rm);

    static void asmMovGX(Asm* a, u8 size, VReg dst, VReg src);

    static void asmMovImm(Asm* a, u8 size, VReg dst, i64 x);

    static void asmMovRR(Asm* a, u8 size, VReg dst, VReg src);

    static void asmMovXG(Asm* a, u8 size, VReg dst, VReg src);

    static void asmMoveSP(Asm* a, i32 by);

    static void asmNops(Asm* a, i32 n);

    static void asmOpRM(Asm* a, bool w, Span<const u8> op, Int reg, Amem m, Int imm, bool byteReg);

    static void asmOpRR(Asm* a, bool w, Span<const u8> op, Int reg, Int rm, bool byteRegs);

    static void asmPatch32(Asm* a, i32 at, i32 target);

    static void asmPatch8(Asm* a, i32 at, i32 target);

    static void asmPop(Asm* a, VReg r);

    static void asmPopMem(Asm* a, Amem m);

    static void asmPopf(Asm* a);

    static i32 asmPos(Asm* a);

    static void asmPrefixed(Asm* a, u8 prefix, bool w, Span<const u8> op, Int reg, Int rm);

    static void asmPush(Asm* a, VReg r);

    static void asmPushMem(Asm* a, Amem m);

    static void asmPushf(Asm* a);

    static void asmRepMovsb(Asm* a);

    static void asmRepStosb(Asm* a);

    static void asmRet(Asm* a);

    static void asmRex(Asm* a, bool w, Int r, Int x, Int b, bool force);

    static void asmSetSP(Asm* a, i32 delta);

    static void asmSetcc(Asm* a, u8 cc, VReg dst);

    static void asmShiftCL(Asm* a, u8 kind, u8 size, VReg dst);

    static void asmShiftImm(Asm* a, u8 kind, u8 size, VReg dst, i64 n);

    static void asmShiftX(Asm* a, u8 kind, u8 size, VReg dst, VReg src, VReg count);

    static Slice<SpEntry> asmSpTable(Asm* a, i32 start, i32 end);

    static void asmStore(Asm* a, u8 width, VReg src, Amem m);

    static void asmStoreImm(Asm* a, u8 width, Amem m, i64 x);

    static void asmTest(Asm* a, u8 size, VReg x, VReg y);

    static void asmTestImm(Asm* a, u8 size, VReg x, i64 imm);

    static void asmU32(Asm* a, u32 x);

    static void asmU64(Asm* a, u64 x);

    static void asmUd2(Asm* a);

    static void asmUnary(Asm* a, u8 kind, u8 size, VReg dst);

    static void asmVex(Asm* a, Int r, Int x, Int b, u8 mmmmm, bool w, Int vvvv, bool l, u8 pp);

    static void asmVexRM(Asm* a, u8 pp, u8 mmmmm, bool w, u8 op, Int reg, Int vvvv, Amem m, Int imm);

    static void asmVexRR(Asm* a, u8 pp, u8 mmmmm, bool w, u8 op, Int reg, Int vvvv, Int rm);

    static void asmXchg(Asm* a, VReg x, VReg y);

    static DominatorChildren cfgInfoChildren(CfgInfo* ci);

    static bool cfgInfoDominates(CfgInfo* ci, i32 a, i32 b);

    static void cfgInfoDominators(CfgInfo* ci, Function* f);

    static i32 cfgInfoIntersect(CfgInfo* ci, i32 a, i32 b);

    static void cfgInfoLoops(CfgInfo* ci, Function* f);

    static void frontendAddEdge(Frontend* fe, i32 from, i32 to, Slice<i32> args);

    static i32 frontendAddExplicit(Frontend* fe, i32 b, ValType t);

    static i32 frontendAddOperands(Frontend* fe, i32 local, i32 phi);

    static i32 frontendAddParam(Frontend* fe, i32 b, ValType t);

    static ValType frontendAddrType(Frontend* fe);

    static void frontendBinary(Frontend* fe, Op op, ValType t);

    static Tuple2<Slice<ValType>, Slice<ValType>> frontendBlockType(Frontend* fe);

    static void frontendBody(Frontend* fe);

    static void frontendCall(Frontend* fe, Op op, FuncType* t, i64 aux, i32 aux2, Slice<i32> extra);

    static void frontendCheckData(Frontend* fe, u32 seg);

    static i32 frontendConstant(Frontend* fe, ValType t, i64 bits);

    static void frontendDeadAfter(Frontend* fe);

    static void frontendElseOp(Frontend* fe);

    static i32 frontendEmit(Frontend* fe, Op op, ValType t, i64 aux, Span<const i32> args);

    static void frontendEnd(Frontend* fe);

    static u32 frontendGlobalIndex(Frontend* fe);

    static void frontendJump(Frontend* fe, i32 to, Slice<i32> args);

    static Control* frontendLabel(Frontend* fe, u32 depth);

    static Slice<ValType> frontendLabelTypes(Frontend* fe, Control* c);

    static void frontendLoad(Frontend* fe, ValType t, Int width, bool signedValue);

    static i32 frontendLocalIndex(Frontend* fe);

    static void frontendMarkDead(Frontend* fe);

    static void frontendMemIndex(Frontend* fe);

    static Tuple2<i64, i32> frontendMemarg(Frontend* fe);

    static i32 frontendNewBlock(Frontend* fe);

    static i32 frontendParamSlot(Frontend* fe, i32 v);

    static i32 frontendPop(Frontend* fe);

    static Slice<i32> frontendPopN(Frontend* fe, Int n);

    static void frontendPrefixFC(Frontend* fe);

    static void frontendPush(Frontend* fe, i32 v);

    static i32 frontendRead(Frontend* fe, i32 local, i32 b);

    static i32 frontendResolve(Frontend* fe, i32 v);

    static void frontendResolveAliases(Frontend* fe);

    static void frontendRun(Frontend* fe);

    static void frontendSeal(Frontend* fe, i32 b);

    static void frontendSkip(Frontend* fe, u8 op);

    static void frontendStore(Frontend* fe, Int width);

    static u32 frontendTableIndex(Frontend* fe);

    static Slice<i32> frontendTop(Frontend* fe, Int n);

    static i32 frontendTryRemoveTrivial(Frontend* fe, i32 phi);

    static ValType frontendTypeOf(Frontend* fe, i32 v);

    static void frontendUnary(Frontend* fe, Op op, ValType t);

    static void frontendWrite(Frontend* fe, i32 local, i32 b, i32 v);

    static i32 frontendZero(Frontend* fe, ValType t);

    static void funcEmitterBlock(FuncEmitter* e, MBlock* blk, i32 next);

    static void funcEmitterCheck(FuncEmitter* e, MInst* in);

    static void funcEmitterCvtU64F(FuncEmitter* e, MInst* in);

    static void funcEmitterExactCheck(FuncEmitter* e, VReg src, i64 end);

    static void funcEmitterInst(FuncEmitter* e, MInst* in);

    static void funcEmitterJumpTo(FuncEmitter* e, Int cc, i32 block);

    static Amem funcEmitterMem(FuncEmitter* e, Mem m);

    static void funcEmitterMinMax(FuncEmitter* e, MInst* in);

    static void funcEmitterMove(FuncEmitter* e, VReg dst, VReg src);

    static void funcEmitterParMove(FuncEmitter* e, Span<Move> moves);

    static Amem funcEmitterSlotMem(FuncEmitter* e, i32 slot);

    static void funcEmitterSwap(FuncEmitter* e, VReg x, VReg y);

    static void funcEmitterSwitchTable(FuncEmitter* e, MInst* in);

    static void funcEmitterTrapJcc(FuncEmitter* e, u8 cc, i64 code);

    static void funcEmitterTrapJmp(FuncEmitter* e, i64 code);

    static void funcEmitterTruncF(FuncEmitter* e, MInst* in);

    template <typename T>
    static void intervalAddRange(T* it, i32 from, i32 to);

    static i32 intervalCovered(Interval* it, i32 p);

    static bool intervalCovers(Interval* it, i32 p);

    static bool intervalCoversAt(Interval* it, i32 p);

    static void intervalRecordUse(Interval* it, i32 p);

    template <typename T>
    static bool intervalCutAt(T* it, i32 p);

    static i32 intervalEnd(Interval* it);

    static bool intervalEndsBefore(Interval* it, i32 p);

    static void intervalFinish(Interval* it);

    static void intervalFinish(CopyInterval* it);

    static i32 intervalIntersection(Interval* it, Interval* o);

    static i32 intervalNextUse(Interval* it, i32 p);

    static i32 intervalPrevUse(Interval* it, i32 p);

    static Interval* intervalSplit(Interval* it, i32 p);

    static i32 intervalStart(Interval* it);

    static Mem lowererAddress(Lowerer* l, i32 addr, i64 offset, Int width, bool proven);

    static void lowererAlu(Lowerer* l, i32 v, Op op, i32 a, i32 b, u8 size);

    static void lowererCall(Lowerer* l, i32 v, Int index, Span<const i32> args);

    static void lowererCallIndirect(Lowerer* l, i32 v, Int typeIndex, Int table, Span<const i32> args);

    static void lowererCheckRange(Lowerer* l, VReg a, VReg n, Mem limit, u8 limitSize, i64 code);

    static u8 lowererClassOf(Lowerer* l, ValType t);

    static Tuple2<u8, Int> lowererCompare(Lowerer* l, i32 c);

    static i64 lowererConstBits(Lowerer* l, i32 v);

    static VReg lowererConstReg(Lowerer* l, i64 x, u8 size);

    static void lowererConvert(Lowerer* l, i32 v, Op op, i32 a, u8 size);

    static void lowererCopysign(Lowerer* l, i32 v, i32 a, i32 b, u8 size);

    static void lowererCountUses(Lowerer* l);

    static VReg lowererDef(Lowerer* l, i32 v);

    static void lowererDivide(Lowerer* l, i32 v, Op op, i32 a, i32 b, u8 size);

    static i32 lowererEdgeBlock(Lowerer* l, i32 b, Int i);

    static void lowererEdgeMoves(Lowerer* l, Edge* e);

    static MInst* lowererEmit(Lowerer* l, MInst in);

    static void lowererEntry(Lowerer* l);

    static Slice<i32> lowererExtracts(Lowerer* l, i32 call);

    static void lowererFindRawTruncations(Lowerer* l);

    static VReg lowererFloatBits(Lowerer* l, i32 v, u8 size);

    static Mem lowererGlobalAddr(Lowerer* l, Int index);

    static void lowererGlobalGet(Lowerer* l, i32 v, Int index);

    static void lowererGlobalSet(Lowerer* l, Int index, i32 value);

    static Tuple2<i64, bool> lowererImm(Lowerer* l, i32 v, u8 size);

    static bool lowererIsConst(Lowerer* l, i32 v);

    static bool lowererLeaAdd(Lowerer* l, i32 v, i32 a, i32 b, u8 size);

    static void lowererLinkBlocks(Lowerer* l);

    static void lowererLoad(Lowerer* l, i32 v, i32 addr);

    static VReg lowererLoad64(Lowerer* l, Mem mem);

    static void lowererLowerValue(Lowerer* l, i32 v);

    static void lowererMarkDeferred(Lowerer* l);

    static void lowererMemAddr(Lowerer* l, VReg dst, VReg a);

    static void lowererMemCopy(Lowerer* l, Span<const i32> args);

    static void lowererMemFill(Lowerer* l, Span<const i32> args);

    static void lowererMemInit(Lowerer* l, Int seg, Span<const i32> args);

    static void lowererMemStub(Lowerer* l, Int stub, VReg a, VReg b, VReg c);

    static void lowererMov(Lowerer* l, VReg dst, VReg src, u8 size);

    static void lowererOutgoing(Lowerer* l, Int stackArgs, Slice<VReg> regs);

    static Tuple2<Slice<VReg>, Int> lowererPlaceArgs(Lowerer* l, Span<const i32> args);

    static void lowererPlaceFloatConstants(Lowerer* l);

    static Tuple2<Slice<VReg>, Int> lowererPlaceRegs(Lowerer* l, Slice<VReg> regs, Slice<ValType> types);

    static void lowererRotate(Lowerer* l, i32 v, Op op, i32 a, i32 b, u8 size);

    static bool lowererScaledIndex(Lowerer* l, i32 v, i32 block);

    static void lowererSelectValue(Lowerer* l, i32 v, Span<const i32> args);

    static void lowererSetcc(Lowerer* l, i32 v);

    static void lowererShift(Lowerer* l, i32 v, Op op, i32 a, i32 b, u8 size);

    static void lowererStore(Lowerer* l, i32 v, i32 addr, i32 value);

    static void lowererStubCall(Lowerer* l, Int stub, Slice<VReg> regs, Slice<ValType> types, i32 result);

    static void lowererTableGet(Lowerer* l, i32 v, Int table, i32 index);

    static void lowererTableSet(Lowerer* l, Int table, i32 index, i32 value);

    static MInst lowererTailCounter(Lowerer* l, i32 offset, i64 amount);

    static void lowererTakeResults(Lowerer* l, i32 v, Slice<ValType> results, Slice<VReg> regs, Int stackArgs);

    static void lowererTerminator(Lowerer* l, i32 b);

    static void lowererTestBits(Lowerer* l, i32 a, i32 b);

    static void lowererTrapIf(Lowerer* l, u8 cc, i64 code);

    static VReg lowererUse(Lowerer* l, i32 v);

    static void lowererVersionTailRanges(Lowerer* l);

    static void metaWriterFlag(MetaWriter* w, bool b);

    static void metaWriterStr(MetaWriter* w, String s);

    static void metaWriterU(MetaWriter* w, u64 x);

    static i32 objCodeAddBlob(ObjCode* oc, Slice<u8> b, Int align);

    static void objCodeAddFixedBlobs(ObjCode* oc);

    static void objCodeAlign16(ObjCode* oc);

    static i32 objCodeBeginRegion(ObjCode* oc, String name);

    static void objCodeCallFunc(ObjCode* oc, Int fn);

    static void objCodeCallGo(ObjCode* oc, String sym);

    static void objCodeCallStub(ObjCode* oc, Int kind);

    static Meta* objCodeDescription(ObjCode* oc);

    static void objCodeEmitFunction(ObjCode* oc, Int index, MFunc* mf, Allocator* al);

    static void objCodeEndRegion(ObjCode* oc, i32 start);

    static void objCodeExportThunks(ObjCode* oc);

    static i32 objCodeExtern(ObjCode* oc, String sym);

    static Amem objCodeFloatConst(ObjCode* oc, u64 bits, u8 size);

    static void objCodeFramesStub(ObjCode* oc);

    static String objCodeFuncName(ObjCode* oc, Int i);

    static i32 objCodeGoCall(ObjCode* oc, String name, Slice<ValType> params, Slice<ValType> results, String sym, bool record);

    static Amem objCodeGotMem(ObjCode* oc, String sym);

    static void objCodeImportAdapters(ObjCode* oc);

    static void objCodeInitThunk(ObjCode* oc);

    static i32 objCodeInterruptStub(ObjCode* oc);

    static void objCodeLandStub(ObjCode* oc);

    static void objCodePlaceBlobs(ObjCode* oc);

    static void objCodeRecordFrame(ObjCode* oc);

    static void objCodeResolve(ObjCode* oc);

    static void objCodeResumeStub(ObjCode* oc);

    static void objCodeSlowPath(ObjCode* oc, i32 at, i32 entry);

    static i32 objCodeStackCheck(ObjCode* oc, i32 total, VReg temp);

    static void objCodeStubs(ObjCode* oc);

    static i32 objCodeThunk(ObjCode* oc, String name, Slice<ValType> params, Slice<ValType> results, Int index, Fn<void()> body);

    static void objCodeTrapCall(ObjCode* oc, i64 code);

    static void optimizerApply(Optimizer* o);

    static bool optimizerCleanup(Optimizer* o);

    static void optimizerCommonExpressions(Optimizer* o);

    static i64 optimizerConstOf(Optimizer* o, i32 v);

    static i32 optimizerConstant(Optimizer* o, ValType t, i64 bits);

    static void optimizerCountUses(Optimizer* o);

    static void optimizerDeadCode(Optimizer* o);

    static void optimizerDropParams(Optimizer* o, i32 b, Slice<bool> keep);

    static bool optimizerIdentity(Optimizer* o, i32 v, Op op, i32 a, i32 b, ValType t);

    static Vec<Edge*> optimizerIncoming(Optimizer* o, i32 b);

    static bool optimizerIsConst(Optimizer* o, i32 v);

    static bool optimizerIsZext(Optimizer* o, i32 v);

    static bool optimizerLocalParams(Optimizer* o, i32 b);

    static Tuple2<i32, bool> optimizerLow32(Optimizer* o, i32 v);

    static bool optimizerNarrowFloatComparisons(Optimizer* o);

    static Int optimizerParamIndex(Optimizer* o, i32 b, i32 v);

    static void optimizerRepair(Optimizer* o, i32 h, Map<i32, Bypass> bypasses);

    static void optimizerReplace(Optimizer* o, i32 v, i32 w);

    static i32 optimizerResolve(Optimizer* o, i32 v);

    static void optimizerSelectTriangles(Optimizer* o);

    static void optimizerSetArgs(Optimizer* o, i32 v, Span<const i32> args);

    static bool optimizerSimplify(Optimizer* o);

    static bool optimizerSimplifyTerm(Optimizer* o, i32 b);

    static bool optimizerSimplifyValue(Optimizer* o, i32 v);

    static Slice<i32> optimizerSubstitute(Optimizer* o, i32 b, Slice<i32> out, Slice<i32> in);

    static bool optimizerThread(Optimizer* o);

    static bool optimizerTrivialParams(Optimizer* o);

    static bool optimizerUnaryIdentity(Optimizer* o, i32 v, Op op, i32 a);

    static bool optimizerUnreachable(Optimizer* o);

    static u8 readerByte(Reader* r);

    static Slice<u8> readerBytes(Reader* r, Int n);

    static Slice<u8> readerConstExpr(Reader* r);

    static bool readerEof(Reader* r);

    static u32 readerF32(Reader* r);

    static u64 readerF64(Reader* r);

    static u32 readerLe32(Reader* r, Slice<u8> b);

    static u64 readerLeb(Reader* r, Uint bits);

    static Limits readerLimits(Reader* r, bool memory);

    static String readerName(Reader* r);

    static ValType readerRefType(Reader* r);

    static i32 readerS32(Reader* r);

    static i64 readerS64(Reader* r);

    static i64 readerSleb(Reader* r, Uint bits);

    static u32 readerU32(Reader* r);

    static ValType readerValType(Reader* r);

    static u64 xorshiftNext(Xorshift* r);

    static bool vRegIsPhys(VReg r);

    static bool vRegIsXMM(VReg r);

    template <typename F>
    static void bitsetEach(Bitset s, F cb);

    template <typename F>
    static void bitsetEachIntersection(Bitset s, Bitset filter, F cb);

    static bool bitsetHas(Bitset s, Int i);

    static void bitsetSet(Bitset s, Int i);

    static bool bitsetUnion(Bitset s, Bitset o);

    static bool memKindSigned(MemKind k);

    static Int memKindWidth(MemKind k);

    static Tuple2<AddressExpression, bool> addAddressExpression(AddressExpression a, AddressExpression b, u32 factor);

    static LoopSpan addLoopSpans(LoopSpan a, LoopSpan b);

    static AddressExpression addressAtom(i32 v);

    static i32 alignTo(i32 x, i32 n);

    static bool allocatable(VReg r);

    static Allocator* allocate(MFunc* mf);

    static Slice<u8> appendLeb(Slice<u8> b, u64 v);

    static Slice<u8> appendSleb(Slice<u8> b, i64 v);

    static Slice<u8> appendUleb(Slice<u8> out, u64 x);

    static Slice<i32> appendUnique(Slice<i32> list, i32 x);

    static Slice<VReg> assigned(Slice<VReg> regs);

    static Amem baseAt(VReg base, i32 disp);

    template <typename Data>
    static void breakPatterns_func(Data data, Int a, Int b);

    static Function* buildFunction(Module* m, Int index);

    static Slice<Slice<CheckedValue>> checkedValues(Function* f, CfgInfo* ci);

    template <typename Data>
    static Tuple2<Int, SortedHint> choosePivot_func(Data data, Int a, Int b);

    static Tuple2<i64, bool> constOffset(Slice<u8> expr);

    static MInst copyMachineInst(MInst in);

    static i32 dataLabel(Int seg);

    static Module* decodeModule(Slice<u8> data);

    static Slice<i32> dedup(Slice<i32> xs);

    static String displayName(String s);

    static Span<Move> dropSelfMoves(Span<Move> moves);

    static Tuple2<Slice<u8>, Slice<u32>> ehFrame(Slice<Region> regions);

    static Slice<u8> encodeNote(Meta* m);

    static String exportSymbol(String name, FuncType* t);

    static u8 fAluPP(FAluOp op, u8 size);

    static MemoryLoop* findMemoryLoop(Function* f, CfgInfo* ci, i32 h);

    static Int firstBranch(Span<MInst> insts);

    static bool flagCondition(Op op);

    static Tuple2<double, bool> floatConstant(Function* f, i32 v);

    static Tuple2<u64, bool> foldInt(Op op, ValType t, u64 a, u64 b);

    static Tuple2<u64, bool> foldUnary(Op op, ValType t, ValType from, u64 a);

    static bool globalMutability(Reader* r);

    static GoFrame goAssign(Slice<ValType> params, Slice<ValType> results);

    static i32 goSize(ValType t);

    static bool hasEffect(Op op);

    template <typename Data>
    static void heapSort_func(Data data, Int a, Int b);

    static u8 hi(Int r);

    static String importSymbol(String module, String name, FuncType* t);

    static Amem indexed(VReg base, VReg index);

    static i64 inductionStep(Function* f, i32 phi, i32 next);

    static Tuple2<Slice<i32>, bool> inductionTerms(Function* f, i32 v, i32 phi, Int depth);

    template <typename Data>
    static void insertionSort_func(Data data, Int a, Int b);

    static MInst inst(MOp op);

    static Tuple3<Slice<VReg>, Slice<Int>, Int> internalLocs(Slice<ValType> types, Slice<VReg> gprs, Slice<VReg> xmms, Int first);

    static bool isCompare(Op op);

    static bool isFloat(ValType t);

    static bool isIntOp(Op op);

    static bool isSlot(VReg r);

    static u8 lo3(Int r);

    static MFunc* lowerFunction(Module* m, Layout* lay, Function* f);

    static MemKind makeMemKind(Int width, bool signedValue);

    static u64 mask(ValType t);

    static i64 maxSigned(u8 size);

    template <typename Data>
    static Int medianAdjacent_func(Data data, Int a, Int* swaps);

    template <typename Data>
    static Int median_func(Data data, Int a, Int b, Int c, Int* swaps);

    static FloatRange meetFloatRange(FloatRange a, FloatRange b);

    static Mem memAt(VReg base, i32 disp);

    static Int memB(Amem m);

    static Mem memIndexed(VReg base, VReg index, i32 disp);

    static Amem memOf(Mem m);

    static Int memX(Amem m);

    static void mergeRanges(InlineVec<Lrange, 1>& x, InlineVec<Lrange, 1>& y, InlineVec<Lrange, 1>& scratch);

    static Bitset newBitset(Int n);

    static Layout* newLayout(Module* m);

    static Uint nextPowerOfTwo(Int lengthValue);

    static void optimize(Function* f);

    static Int order(u8 id);

    template <typename Data>
    static Tuple2<Int, Int> order2_func(Data data, Int a, Int b, Int* swaps);

    static bool overlap(Span<Lrange> x, Span<Lrange> y);

    static u8 packedPP(u8 size);

    static MInst parMove(Slice<Move> moves);

    template <typename Data>
    static bool partialInsertionSort_func(Data data, Int a, Int b);

    template <typename Data>
    static Int partitionEqual_func(Data data, Int a, Int b, Int pivot);

    template <typename Data>
    static Tuple2<Int, bool> partition_func(Data data, Int a, Int b, Int pivot);

    template <typename Data>
    static void pdqsort_func(Data data, Int a, Int b, Int limit);

    static bool readsState(Op op);

    static Slice<u8> refFuncExpr(u32 index);

    static Int regNum(VReg r);

    static Slice<VReg> resultRegs(Slice<ValType> results);

    static bool returnsTwice(String name);

    template <typename Data>
    static void reverseRange_func(Data data, Int a, Int b);

    template <typename Data>
    static void rotate_func(Data data, Int a, Int m, Int b);

    static Amem rspAt(i32 disp);

    static bool sameArgs(Optimizer* o, Slice<i32> x, Slice<i32> y);

    static u8 scalarPP(u8 size);

    static u8 scaleBits(u8 s);

    static i32 selfUses(Span<Edge*> in, Int k, i32 p);

    template <typename Data>
    static void siftDown_func(Data data, Int lo, Int hi, Int first);

    static VReg slotLoc(i32 slot);

    static i32 slotOf(VReg r);

    template <typename Data>
    static void stable_func(Data data, Int n);

    static u8 swapCC(u8 cc);

    template <typename Data>
    static void swapRange_func(Data data, Int a, Int b, Int n);

    template <typename Data>
    static void symMerge_func(Data data, Int a, Int m, Int b);

    static String symbolPart(String s);

    static Tuple3<u64, u64, bool> truncBounds(u8 isize, u8 fsize);

    static u64 typeID(FuncType* t);

    static u8 typeSize(ValType t);

    static String typesPart(Slice<ValType> ts);

    static Slice<i32> uniquePredecessors(Slice<i32> blocks);

    static FloatRange unknownFloatRange();

    static u64 valTypeCode(ValType t);

    static void versionMemoryLoops(Function* f);

    static void writeELF(ObjCode* oc, Slice<u8> note);

    static Slice<VReg> xmmRange(Int n);

    static void moduleCustom(Module*, Reader*);

    template <usize size>
    static void asmBytes(Asm* a, const u8 (&bytes)[size]) {
        const usize at = a->buf.size();
        const usize offset = at % pageBytes;
        if (__builtin_expect(size <= pageBytes && offset <= pageBytes - size, true)) {
            a->buf.reserve(checkedAdd(at, size));
            copyBytes(a->buf.pages.data()[at / pageBytes] + offset, bytes, size);
            a->buf.count = at + size;
        } else {
            a->buf.append(bytes, size);
        }
    }

    /*
The sorting routines below follow Go 1.25 sort (zsortfunc.go), under:

Copyright 2009 The Go Authors.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

   * Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
   * Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the following disclaimer
in the documentation and/or other materials provided with the
distribution.
   * Neither the name of Google LLC nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
    // Preserve Go sort's ordering of ties, which affects register allocation.
    template <typename T, typename F>
    static void sortValues(Slice<T> values, F less) {
        LessSwap data{less, [values](Int a, Int b) {
            T old = values[a];
            values[a] = values[b];
            values[b] = old;
        }};
        pdqsort_func(data, 0, length(values), 64 - bitsLeadingZeros64(length(values)));
    }

    template <typename T, typename F>
    static void sortValues(Span<T> values, F less) {
        LessSwap data{less, [values](Int a, Int b) {
            T old = values[a];
            values[a] = values[b];
            values[b] = old;
        }};
        pdqsort_func(data, 0, length(values), 64 - bitsLeadingZeros64(length(values)));
    }

    template <typename T, typename F>
    static void sortValuesStable(Slice<T> values, F less) {
        LessSwap data{less, [values](Int a, Int b) {
            T old = values[a];
            values[a] = values[b];
            values[b] = old;
        }};
        stable_func(data, length(values));
    }

    static Slice<VReg> allocGPRs{};

    static Slice<VReg> allocXMMs{};

    static AluOp aluOpcode(Op op) {
        switch (op) {
            case OpAdd:
                return aluAdd;
            case OpSub:
                return aluSub;
            case OpAnd:
                return aluAnd;
            case OpOr:
                return aluOr;
            case OpXor:
                return aluXor;
            case OpMul:
                return aluImul;
            default:
                return {};
        }
    }

    static Slice<VReg> argGPRs{};

    static Slice<VReg> argXMMs{};

    static Slice<VReg> calleeSaved{};

    static Slice<VReg> callerSaved{};

    static Slice<VReg> callerSavedGprs{};

    static Slice<VReg> callerSavedXmms{};

    static u8 floatOpcode(FAluOp op) {
        switch (op) {
            case fAdd:
                return 88;
            case fSub:
                return 92;
            case fMul:
                return 89;
            case fDiv:
                return 94;
            case fMin:
                return 93;
            case fMax:
                return 95;
            case fAnd:
                return 84;
            case fAndn:
                return 85;
            case fOr:
                return 86;
            case fXor:
                return 87;
            default:
                return {};
        }
    }

    static Array<Op, 7> floatUnary{};

    static Slice<VReg> goFloatRegs{};

    static Slice<VReg> goIntRegs{};

    static Array<u8, 10> intCC{};

    static Tuple2<Op, bool> invertComparison(Op op) {
        switch (op) {
            case OpEq:
                return {OpNe, true};
            case OpNe:
                return {OpEq, true};
            case OpLtS:
                return {OpGeS, true};
            case OpGeS:
                return {OpLtS, true};
            case OpLtU:
                return {OpGeU, true};
            case OpGeU:
                return {OpLtU, true};
            case OpGtS:
                return {OpLeS, true};
            case OpLeS:
                return {OpGtS, true};
            case OpGtU:
                return {OpLeU, true};
            case OpLeU:
                return {OpGtU, true};
            default:
                return {0, false};
        }
    }

    static Slice<VReg> memmoveClobbers{};

    static Slice<VReg> memsetClobbers{};

    static Array<Slice<u8>, 10> nopBytes{};

    static Slice<VReg> resultGPRs{};

    static Slice<VReg> resultXMMs{};

    static Tuple2<u8, bool> typeLetter(ValType op) {
        switch (op) {
            case I32:
                return {'i', true};
            case I64:
                return {'I', true};
            case F32:
                return {'f', true};
            case F64:
                return {'F', true};
            case V128:
                return {'v', true};
            case FuncRef:
                return {'r', true};
            case ExternRef:
                return {'e', true};
            default:
                return {0, false};
        }
    }

    static void initializeCompilerTables() {
        allocGPRs = sliceOf<VReg>({RAX, RCX, RDX, RSI, RDI, R8, R9, RBX, R10, R11, R12});

        allocXMMs = xmmRange(Int(15LL));

        argGPRs = sliceOf<VReg>({RBX, RCX, RDI, RSI, R8, R9});

        argXMMs = xmmRange(Int(15LL));

        calleeSaved = sliceOf<VReg>({R10, R11, R12});

        callerSavedGprs = sliceOf<VReg>({RAX, RBX, RCX, RDX, RSI, RDI, R8, R9});
        callerSavedXmms = xmmRange(Int(15LL));
        callerSaved = appendSlice(callerSavedGprs, callerSavedXmms);

        floatUnary = Array<Op, 7>{{OpFAbs, OpFNeg, OpFCeil, OpFFloor, OpFTrunc, OpFNearest, OpFSqrt}};

        goFloatRegs = xmmRange(Int(15LL));

        goIntRegs = sliceOf<VReg>({RAX, RBX, RCX, RDI, RSI, R8, R9, R10, R11});

        intCC = Array<u8, 10>{{ccE, ccNE, ccL, ccB, ccG, ccA, ccLE, ccBE, ccGE, ccAE}};

        memmoveClobbers = sliceOf<VReg>({RAX, RCX, RSI, RDI});

        memsetClobbers = sliceOf<VReg>({RCX, RDI});

        nopBytes = Array<Slice<u8>, 10>{{{}, sliceOf<u8>({u8(144LL)}), sliceOf<u8>({u8(102LL), u8(144LL)}), sliceOf<u8>({u8(15LL), u8(31LL), u8(0LL)}), sliceOf<u8>({u8(15LL), u8(31LL), u8(64LL), u8(0LL)}), sliceOf<u8>({u8(15LL), u8(31LL), u8(68LL), u8(0LL), u8(0LL)}), sliceOf<u8>({u8(102LL), u8(15LL), u8(31LL), u8(68LL), u8(0LL), u8(0LL)}), sliceOf<u8>({u8(15LL), u8(31LL), u8(128LL), u8(0LL), u8(0LL), u8(0LL), u8(0LL)}), sliceOf<u8>({u8(15LL), u8(31LL), u8(132LL), u8(0LL), u8(0LL), u8(0LL), u8(0LL), u8(0LL)}), sliceOf<u8>({u8(102LL), u8(15LL), u8(31LL), u8(132LL), u8(0LL), u8(0LL), u8(0LL), u8(0LL), u8(0LL)})}};

        resultGPRs = sliceOf<VReg>({RAX, RBX, RCX, RDI, RSI, R8, R9});

        resultXMMs = xmmRange(Int(15LL));
    }

    static AddressExpression addressExpressionsOf(AddressExpressions* r, i32 v, Int depth) {
        if ((r)->seen[v]) {
            return (r)->cache[v];
        }
        Function* f = (r)->f;
        Value val = (f)->values[v];
        AddressExpression out = addressAtom(v);
        auto temporary1 = true;
        auto temporary2 = out;
        (r)->seen[v] = temporary1;
        (r)->cache[v] = temporary2;
        if (((depth > Int(32LL)) || (((val).typ != I32) && ((val).typ != I64)))) {
            return out;
        }
        Span<i32> args = functionArgs(f, v);
        {
            auto switch3 = (val).op;
            switch (switch3) {
                case OpConst: {
                    out = AddressExpression{{}, convert<u32>((val).aux)};
                    break;
                }
                case OpWrap:
                case OpExtendU:
                case OpExtendS: {
                    out = addressExpressionsOf(r, args[Int(0LL)], (depth + Int(1LL)));
                    break;
                }
                case OpAdd:
                case OpSub: {
                    u32 factor = u32(1LL);
                    if (((val).op == OpSub)) {
                        factor = u32(4294967295LL);
                    }
                    {
                        auto temporary4 = addAddressExpression(addressExpressionsOf(r, args[Int(0LL)], (depth + Int(1LL))), addressExpressionsOf(r, args[Int(1LL)], (depth + Int(1LL))), factor);
                        AddressExpression sum = temporary4.v0;
                        bool ok = temporary4.v1;
                        if (ok) {
                            out = sum;
                        }
                    }
                    break;
                }
                case OpMul:
                case OpShl: {
                    auto temporary5 = args[Int(0LL)];
                    auto temporary6 = (f)->values[args[Int(1LL)]];
                    i32 a = temporary5;
                    Value k = temporary6;
                    if ((((val).op == OpMul) && ((k).op != OpConst))) {
                        auto temporary7 = args[Int(1LL)];
                        auto temporary8 = (f)->values[args[Int(0LL)]];
                        a = temporary7;
                        k = temporary8;
                    }
                    if (((k).op == OpConst)) {
                        u32 factor = convert<u32>((k).aux);
                        if (((val).op == OpShl)) {
                            u64 shift = (convert<u64>((k).aux) & convert<u64>(u8(u8(typeSize((val).typ) * u8(8LL)) - u8(1LL))));
                            factor = u32(0LL);
                            if ((shift < u64(32LL))) {
                                factor = shiftLeft(u32(1LL), shift);
                            }
                        }
                        {
                            auto temporary9 = addAddressExpression(AddressExpression{}, addressExpressionsOf(r, a, (depth + Int(1LL))), factor);
                            AddressExpression product = temporary9.v0;
                            bool ok = temporary9.v1;
                            if (ok) {
                                out = product;
                            }
                        }
                    }
                    break;
                }
            }
        }
        (r)->cache[v] = out;
        return out;
    }

    static bool floatIntegerNarrowCan(FloatIntegerNarrow* n, i32 v, Int depth) {
        Function* f = ((n)->o)->f;
        Value* val = (&((f)->values[v]));
        if ((((val)->typ != F64) || (depth > Int(24LL)))) {
            return false;
        }
        if (((n)->valid[v] != u8(0LL))) {
            return ((n)->valid[v] == u8(2LL));
        }
        (n)->valid[v] = u8(1LL);
        bool ok = false;
        {
            auto switch10 = (val)->op;
            switch (switch10) {
                case OpConst: {
                    double x = mathFloat64frombits(convert<u64>((val)->aux));
                    ok = (((x >= double(0.0)) && (x <= double(4294967295.0))) && (mathTrunc(x) == x));
                    break;
                }
                case OpConvertU: {
                    ok = (((f)->values[functionArgs(f, v)[Int(0LL)]]).typ == I32);
                    break;
                }
                case OpParam: {
                    Vec<Edge*> incoming = optimizerIncoming((n)->o, (val)->block);
                    ok = (((val)->block != i32(0LL)) && (length(incoming) > Int(0LL)));
                    {
                        auto range11 = incoming.view();
                        for (Int index12 = 0, rangeEnd13 = length(range11); index12 < rangeEnd13; ++index12) {
                            Edge* e = range11[index12];
                            ok = (ok && floatIntegerNarrowCan(n, (e)->args[(val)->aux], (depth + Int(1LL))));
                        }
                    }
                    break;
                }
            }
        }
        if (ok) {
            (n)->valid[v] = u8(2LL);
        }
        return ok;
    }

    static i32 floatIntegerNarrowInteger(FloatIntegerNarrow* n, i32 v) {
        {
            auto temporary14 = lookupMap((n)->values, v);
            i32 mapped = temporary14.v0;
            bool ok = temporary14.v1;
            if (ok) {
                return mapped;
            }
        }
        Function* f = ((n)->o)->f;
        Value val = (f)->values[v];
        {
            auto switch15 = (val).op;
            switch (switch15) {
                case OpConst: {
                    return optimizerConstant((n)->o, I32, convert<i64>(mathFloat64frombits(convert<u64>((val).aux))));
                    break;
                }
                case OpConvertU: {
                    return functionArgs(f, v)[Int(0LL)];
                    break;
                }
            }
        }
        i32 b = (val).block;
        Int index = length(((f)->blocks[b]).params);
        i32 p = functionNewValue(f, OpParam, I32, b, convert<i64>(index), Span<const i32>({}));
        ((f)->blocks[b]).params = appendValues(((f)->blocks[b]).params, p);
        setMap((n)->values, v, p);
        Vec<Edge*> incoming = optimizerIncoming((n)->o, b);
        {
            auto range16 = incoming.view();
            for (Int index17 = 0, rangeEnd18 = length(range16); index17 < rangeEnd18; ++index17) {
                Edge* e = range16[index17];
                (e)->args = appendValues((e)->args, i32(-1LL));
            }
        }
        {
            auto range19 = incoming.view();
            for (Int index20 = 0, rangeEnd21 = length(range19); index20 < rangeEnd21; ++index20) {
                Edge* e = range19[index20];
                i32 arg = (e)->args[(val).aux];
                (e)->args[index] = floatIntegerNarrowInteger(n, arg);
            }
        }
        return p;
    }

    static void floatRangesAdd(FloatRanges* r, i32 v, FloatRange bound) {
        {
            auto temporary22 = lookupMap((r)->facts, v);
            FloatRange prior = temporary22.v0;
            bool ok = temporary22.v1;
            if (ok) {
                bound = meetFloatRange(prior, bound);
            }
        }
        setMap((r)->facts, v, bound);
    }

    // constrain records an ordered comparison on the path where a value is
    // used. A false comparison admits NaN; another fact must exclude it before
    // a conversion may lose its saturation checks.
    static void floatRangesConstrain(FloatRanges* r, i32 cond, bool truth) {
        Function* f = (r)->f;
        Value* val = (&((f)->values[cond]));
        Op op = (val)->op;
        if (((op < OpFEq) || (op > OpFGe))) {
            return;
        }
        Span<i32> args = functionArgs(f, cond);
        auto temporary23 = args[Int(0LL)];
        auto temporary24 = args[Int(1LL)];
        i32 v = temporary23;
        i32 c = temporary24;
        auto temporary25 = floatConstant(f, c);
        double k = temporary25.v0;
        bool ok = temporary25.v1;
        if ((!(ok))) {
            auto temporary26 = floatConstant(f, v);
            k = temporary26.v0;
            ok = temporary26.v1;
            v = c;
            {
                auto switch27 = op;
                switch (switch27) {
                    case OpFLt: {
                        op = OpFGt;
                        break;
                    }
                    case OpFLe: {
                        op = OpFGe;
                        break;
                    }
                    case OpFGt: {
                        op = OpFLt;
                        break;
                    }
                    case OpFGe: {
                        op = OpFLe;
                        break;
                    }
                }
            }
        }
        if (((!(ok)) || mathIsNaN(k))) {
            return;
        }
        auto temporary28 = mathNextafter(k, mathInf(Int(-1LL)));
        auto temporary29 = mathNextafter(k, mathInf(Int(1LL)));
        double prev = temporary28;
        double next = temporary29;
        if ((((f)->values[v]).typ == F32)) {
            prev = convert<double>(mathNextafter32(convert<float>(k), convert<float>(mathInf(Int(-1LL)))));
            next = convert<double>(mathNextafter32(convert<float>(k), convert<float>(mathInf(Int(1LL)))));
        }
        FloatRange bound = unknownFloatRange();
        (bound).nan = (!(truth));
        {
            auto switch30 = op;
            switch (switch30) {
                case OpFEq:
                case OpFNe: {
                    if ((truth != ((op == OpFEq)))) {
                        return;
                    }
                    auto temporary31 = k;
                    auto temporary32 = k;
                    auto temporary33 = false;
                    (bound).lo = temporary31;
                    (bound).hi = temporary32;
                    (bound).nan = temporary33;
                    break;
                }
                case OpFLt: {
                    if (truth) {
                        (bound).hi = prev;
                    } else {
                        (bound).lo = k;
                    }
                    break;
                }
                case OpFLe: {
                    if (truth) {
                        (bound).hi = k;
                    } else {
                        (bound).lo = next;
                    }
                    break;
                }
                case OpFGt: {
                    if (truth) {
                        (bound).lo = next;
                    } else {
                        (bound).hi = k;
                    }
                    break;
                }
                case OpFGe: {
                    if (truth) {
                        (bound).lo = k;
                    } else {
                        (bound).hi = prev;
                    }
                    break;
                }
            }
        }
        floatRangesAdd(r, v, bound);
        if ((((((f)->values[v]).op == OpFAbs) && ((bound).hi >= double(0.0))) && (!(mathIsInf((bound).hi, Int(0LL)))))) {
            floatRangesAdd(r, functionArgs(f, v)[Int(0LL)], FloatRange{(-((bound).hi)), (bound).hi, (bound).nan});
        }
    }

    static FloatRange floatRangesOf(FloatRanges* r, i32 v, Int depth) {
        {
            auto temporary34 = lookupMap((r)->cache, v);
            FloatRange got = temporary34.v0;
            bool ok = temporary34.v1;
            if (ok) {
                return got;
            }
        }
        Value* val = (&(((r)->f)->values[v]));
        FloatRange out = unknownFloatRange();
        {
            auto temporary35 = floatConstant((r)->f, v);
            double k = temporary35.v0;
            bool ok = temporary35.v1;
            if (ok) {
                out = FloatRange{k, k, mathIsNaN(k)};
            } else if ((depth < Int(32LL))) {
                Span<i32> args = functionArgs((r)->f, v);
                {
                    auto switch36 = (val)->op;
                    switch (switch36) {
                        case OpFAdd:
                        case OpFSub:
                        case OpFMul:
                        case OpFDiv: {
                            auto temporary37 = floatRangesOf(r, args[Int(0LL)], (depth + Int(1LL)));
                            auto temporary38 = floatRangesOf(r, args[Int(1LL)], (depth + Int(1LL)));
                            FloatRange a = temporary37;
                            FloatRange b = temporary38;
                            bool finiteB = (((!((b).nan)) && (!(mathIsInf((b).lo, Int(0LL))))) && (!(mathIsInf((b).hi, Int(0LL)))));
                            if ((finiteB && (((((val)->op == OpFAdd) || ((val)->op == OpFSub)) || (((b).lo == (b).hi) && ((b).lo > double(0.0))))))) {
                                (out).nan = (a).nan;
                                {
                                    auto switch39 = (val)->op;
                                    switch (switch39) {
                                        case OpFAdd: {
                                            auto temporary40 = ((a).lo + (b).lo);
                                            auto temporary41 = ((a).hi + (b).hi);
                                            (out).lo = temporary40;
                                            (out).hi = temporary41;
                                            break;
                                        }
                                        case OpFSub: {
                                            auto temporary42 = ((a).lo - (b).hi);
                                            auto temporary43 = ((a).hi - (b).lo);
                                            (out).lo = temporary42;
                                            (out).hi = temporary43;
                                            break;
                                        }
                                        case OpFMul: {
                                            auto temporary44 = ((a).lo * (b).lo);
                                            auto temporary45 = ((a).hi * (b).hi);
                                            (out).lo = temporary44;
                                            (out).hi = temporary45;
                                            break;
                                        }
                                        case OpFDiv: {
                                            auto temporary46 = ((a).lo / (b).lo);
                                            auto temporary47 = ((a).hi / (b).hi);
                                            (out).lo = temporary46;
                                            (out).hi = temporary47;
                                            break;
                                        }
                                    }
                                }
                            }
                            break;
                        }
                        case OpFAbs: {
                            FloatRange a = floatRangesOf(r, args[Int(0LL)], (depth + Int(1LL)));
                            out = FloatRange{double(0.0), maximum(mathAbs((a).lo), mathAbs((a).hi)), (a).nan};
                            break;
                        }
                        case OpFNeg: {
                            FloatRange a = floatRangesOf(r, args[Int(0LL)], (depth + Int(1LL)));
                            out = FloatRange{(-((a).hi)), (-((a).lo)), (a).nan};
                            break;
                        }
                        case OpSelect: {
                            auto temporary48 = floatRangesOf(r, args[Int(1LL)], (depth + Int(1LL)));
                            auto temporary49 = floatRangesOf(r, args[Int(2LL)], (depth + Int(1LL)));
                            FloatRange a = temporary48;
                            FloatRange b = temporary49;
                            out = FloatRange{minimum((a).lo, (b).lo), maximum((a).hi, (b).hi), ((a).nan || (b).nan)};
                            break;
                        }
                    }
                }
            }
        }
        if (((val)->typ == F32)) {
            auto temporary50 = convert<double>(convert<float>((out).lo));
            auto temporary51 = convert<double>(convert<float>((out).hi));
            (out).lo = temporary50;
            (out).hi = temporary51;
        }
        {
            auto temporary52 = lookupMap((r)->facts, v);
            FloatRange bound = temporary52.v0;
            bool ok = temporary52.v1;
            if (ok) {
                out = meetFloatRange(out, bound);
            }
        }
        setMap((r)->cache, v, out);
        return out;
    }

    static CfgInfo* functionAnalyze(Function* f) {
        CfgInfo* ci = activePool->make<CfgInfo>(CfgInfo{functionRpo(f), {}, {}, {}, {}});
        Int n = length((f)->blocks);
        (ci)->rpoIndex = makeSlice<i32>(n);
        {
            auto range53 = (ci)->rpoIndex;
            for (Int index54 = 0, rangeEnd55 = length(range53); index54 < rangeEnd55; ++index54) {
                Int i = index54;
                (ci)->rpoIndex[i] = i32(-1LL);
            }
        }
        {
            auto range56 = (ci)->order;
            for (Int index57 = 0, rangeEnd58 = length(range56); index57 < rangeEnd58; ++index57) {
                Int i = index57;
                i32 b = range56[index57];
                (ci)->rpoIndex[b] = convert<i32>(i);
            }
        }
        functionComputePreds(f);
        cfgInfoDominators(ci, f);
        cfgInfoLoops(ci, f);
        return ci;
    }

    static Span<i32> functionArgs(Function* f, i32 v) {
        const Value& val = f->values[v];
        // Records refer to operands already stored in this append-only pool.
        // Growth preserves their offsets; shortening only reduces the count.
        Span<i32> args;
        args.pages = &f->pool;
        args.offset = u32(val.args);
        args.count = val.nargs;
        return args;
    }

    // computePreds fills each block's predecessor list from the terminators, one
    // entry per edge.
    static void functionComputePreds(Function* f) {
        // Predecessors belong to the graph even when the caller is building
        // a short-lived analysis in another pool.
        PoolScope graphScope(*f->blocks.owner);
        {
            auto& range59 = (f)->blocks;
            for (Int index60 = 0, rangeEnd61 = length(range59); index60 < rangeEnd61; ++index60) {
                Int i = index60;
                ((f)->blocks[i]).preds = subslice(((f)->blocks[i]).preds, Int(0), Int(0LL));
            }
        }
        {
            auto& range62 = (f)->blocks;
            for (Int index63 = 0, rangeEnd64 = length(range62); index63 < rangeEnd64; ++index63) {
                Int i = index63;
                if (((f)->blocks[i]).dead) {
                    continue;
                }
                {
                    auto range65 = (((f)->blocks[i]).term).edges;
                    for (Int index66 = 0, rangeEnd67 = length(range65); index66 < rangeEnd67; ++index66) {
                        Edge e = range65[index66];
                        ((f)->blocks[(e).block]).preds = appendValues(((f)->blocks[(e).block]).preds, convert<i32>(i));
                    }
                }
            }
        }
    }

    static i32 functionNewBlock(Function* f) {
        f->blocks.pushBack(Block{});
        return convert<i32>((length((f)->blocks) - Int(1LL)));
    }

    static i32 functionNewValue(Function* f, Op op, ValType typ, i32 block, i64 aux, Span<const i32> args) {
        i32 id = convert<i32>(length((f)->values));
        i32 start = convert<i32>(length((f)->pool));
        appendSpan((f)->pool, args);
        (f)->values.pushBack(Value{op, typ, convert<u16>(length(args)), start, aux, {}, block});
        return id;
    }

    static Slice<i32> functionRpo(Function* f) {
        Int n = length((f)->blocks);
        Slice<bool> visited = makeSlice<bool>(n);
        Slice<i32> post = makeSlice<i32>(Int(0LL), n);
        Slice<FrameCfg34> stack = sliceOf<FrameCfg34>({FrameCfg34{i32(0LL), Int(0LL)}});
        visited[Int(0LL)] = true;
        {
            for (; (length(stack) > Int(0LL));) {
                FrameCfg34* top = (&(stack[(length(stack) - Int(1LL))]));
                Slice<Edge> edges = (((f)->blocks[(top)->b]).term).edges;
                if (((top)->i < length(edges))) {
                    i32 s = (edges[((length(edges) - Int(1LL)) - (top)->i)]).block;
                    (top)->i++;
                    if ((!(visited[s]))) {
                        visited[s] = true;
                        stack = appendValues(stack, FrameCfg34{s, Int(0LL)});
                    }
                    continue;
                }
                post = appendValues(post, (top)->b);
                stack = subslice(stack, Int(0), (length(stack) - Int(1LL)));
            }
        }
        {
            auto temporary68 = Int(0LL);
            auto temporary69 = (length(post) - Int(1LL));
            Int i = temporary68;
            Int j = temporary69;
            for (; (i < j); [&]() {
                auto temporary70 = (i + Int(1LL));
                auto temporary71 = (j - Int(1LL));
                i = temporary70;
                j = temporary71;
            }()) {
                auto temporary72 = post[j];
                auto temporary73 = post[i];
                post[i] = temporary72;
                post[j] = temporary73;
            }
        }
        return post;
    }

    static i32 layoutDataDropped(Layout* l, Int seg) {
        return ((l)->dropped + convert<i32>(seg));
    }

    static i32 layoutDesc(Layout* l, Int f) {
        return ((l)->descs + (convert<i32>(f) * descBytes));
    }

    static i32 layoutElemDropped(Layout* l, Module* m, Int seg) {
        return ((l)->dropped + convert<i32>((length((m)->datas) + seg)));
    }

    // Build hot reverse postorder without visiting fallback edges: cold paths
    // must not affect the placement of hot successors. A strict function keeps
    // the allocator's exact order.
    static Span<i32> mFuncHotOrder(MFunc* mf, Span<i32> order) {
        Int first = Int(-1LL);
        {
            auto range74 = order;
            for (Int index75 = 0, rangeEnd76 = length(range74); index75 < rangeEnd76; ++index75) {
                Int j = index75;
                i32 b = range74[index75];
                if (((mf)->blocks[b]).cold) {
                    first = j;
                    break;
                }
            }
        }
        if ((first < Int(0LL))) {
            return order;
        }
        Slice<bool> seen = makeSlice<bool>(length((mf)->blocks));
        seen[Int(0LL)] = true;
        Slice<VisitMir346> stack = sliceOf<VisitMir346>({VisitMir346{i32(0LL), {}}});
        Slice<i32> post = {};
        {
            for (; (length(stack) > Int(0LL));) {
                VisitMir346* top = (&(stack[(length(stack) - Int(1LL))]));
                Slice<i32> succs = ((mf)->blocks[(top)->block]).succs;
                if (((top)->edge < length(succs))) {
                    i32 s = succs[((length(succs) - Int(1LL)) - (top)->edge)];
                    (top)->edge++;
                    if (((!(seen[s])) && (!(((mf)->blocks[s]).cold)))) {
                        seen[s] = true;
                        stack = appendValues(stack, VisitMir346{s, {}});
                    }
                    continue;
                }
                post = appendValues(post, (top)->block);
                stack = subslice(stack, Int(0), (length(stack) - Int(1LL)));
            }
        }
        Slice<i32> out = makeSlice<i32>(Int(0LL), length(order));
        {
            Int j = (length(post) - Int(1LL));
            for (; (j >= Int(0LL)); j--) {
                out = appendValues(out, post[j]);
            }
        }
        {
            auto range77 = order;
            for (Int index78 = 0, rangeEnd79 = length(range77); index78 < rangeEnd79; ++index78) {
                i32 b = range77[index78];
                if (((mf)->blocks[b]).cold) {
                    out = appendValues(out, b);
                }
            }
        }
        return out;
    }

    static VReg mFuncNewReg(MFunc* mf, u8 classValue) {
        VReg r = convert<VReg>((Int(32LL) + (mf)->nregs));
        (mf)->nregs++;
        mf->registerClasses |= u8(1u << classValue);
        (mf)->classValue = appendValues((mf)->classValue, classValue);
        return r;
    }

    static u8 mFuncRegClass(MFunc* mf, VReg r) {
        if (vRegIsPhys(r)) {
            if (vRegIsXMM(r)) {
                return classXMM;
            }
            return classGPR;
        }
        return (mf)->classValue[(r - firstVirtual)];
    }

    // Fixed intervals constrain virtual allocation only in the same register
    // class. Calls need no XMM clobbers in an integer-only function, or GPR
    // clobbers in a function with only virtual XMM registers.
    static Slice<VReg> mInstClobberSet(MInst* in, u8 registerClasses) {
        if (in->op == mCall && in->aux == calleeStub) {
            if (in->imm == stubMemmove) {
                return registerClasses & (1u << classGPR) ? memmoveClobbers : Slice<VReg>{};
            }
            if (in->imm == stubMemset) {
                return registerClasses & (1u << classGPR) ? memsetClobbers : Slice<VReg>{};
            }
        }
        if (in->op == mCall || in->op == mCallInd || in->op == mGrowMem) {
            if (registerClasses == (1u << classGPR)) {
                return callerSavedGprs;
            }
            if (registerClasses == (1u << classXMM)) {
                return callerSavedXmms;
            }
            return registerClasses ? callerSaved : Slice<VReg>{};
        }
        return {};
    }

    // forEachOperand calls cb for every register operand of an instruction with
    // its role, uses before defs.
    template <typename F>
    static void mInstForEachOperand(MInst* in, F cb) {
        auto mem = [&]() mutable -> void {
            if ((((in)->mem).base != noReg)) {
                cb((&(((in)->mem).base)), roleUse);
            }
            if ((((in)->mem).index != noReg)) {
                cb((&(((in)->mem).index)), roleUse);
            }
        };
        {
            auto switch82 = (in)->op;
            switch (switch82) {
                case mMov:
                case mFMov:
                case mExt:
                case mBitCount:
                case mCvtIF:
                case mCvtFI:
                case mCvtFF:
                case mMovGX:
                case mMovXG:
                case mFUnary: {
                    cb((&((in)->src)), roleUse);
                    cb((&((in)->dst)), roleDef);
                    break;
                }
                case mMovImm:
                case mSetcc:
                case mFConst: {
                    cb((&((in)->dst)), roleDef);
                    break;
                }
                case mLoad:
                case mFLoad:
                case mLea: {
                    mem();
                    cb((&((in)->dst)), roleDef);
                    break;
                }
                case mStore:
                case mFStore: {
                    mem();
                    cb((&((in)->src)), roleUse);
                    break;
                }
                case mStoreImm:
                case mCmpMemImm:
                case mCounter: {
                    mem();
                    break;
                }
                case mAlu: {
                    cb((&((in)->src)), roleUse);
                    cb((&((in)->dst)), roleUseDef);
                    break;
                }
                case mAluImm:
                case mShiftImm:
                case mUnary: {
                    cb((&((in)->dst)), roleUseDef);
                    break;
                }
                case mAluMem: {
                    mem();
                    cb((&((in)->dst)), roleUseDef);
                    break;
                }
                case mImul3: {
                    cb((&((in)->src)), roleUse);
                    cb((&((in)->dst)), roleDef);
                    break;
                }
                case mCmp:
                case mTest:
                case mFCmp: {
                    cb((&((in)->src)), roleUse);
                    cb((&((in)->src2)), roleUse);
                    break;
                }
                case mCmpImm:
                case mTestImm:
                case mCheck:
                case mSwitch:
                case mCallInd:
                case mStackArg:
                case mStoreRet: {
                    cb((&((in)->src)), roleUse);
                    break;
                }
                case mCmpMem: {
                    mem();
                    cb((&((in)->src)), roleUse);
                    break;
                }
                case mCmov: {
                    cb((&((in)->src)), roleUse);
                    cb((&((in)->dst)), roleUseDef);
                    break;
                }
                case mShiftX:
                case mFAlu: {
                    cb((&((in)->src)), roleUse);
                    cb((&((in)->src2)), roleUse);
                    cb((&((in)->dst)), roleDef);
                    break;
                }
                case mRotCL: {
                    cb((&((in)->src)), roleUse);
                    cb((&((in)->dst)), roleUseDef);
                    break;
                }
                case mParMove: {
                    {
                        auto range83 = mInstMoves(in);
                        for (Int index84 = 0, rangeEnd85 = length(range83); index84 < rangeEnd85; ++index84) {
                            Int i = index84;
                            cb((&((mInstMoves(in)[i]).src)), roleUse);
                        }
                    }
                    {
                        auto range86 = mInstMoves(in);
                        for (Int index87 = 0, rangeEnd88 = length(range86); index87 < rangeEnd88; ++index87) {
                            Int i = index87;
                            cb((&((mInstMoves(in)[i]).dst)), roleDef);
                        }
                    }
                    break;
                }
                case mLoadArg: {
                    cb((&((in)->dst)), roleDef);
                    break;
                }
                case mDivOvf: {
                    cb((&((in)->src)), roleUse);
                    cb((&((in)->src2)), roleUse);
                    break;
                }
                case mTruncF:
                case mCvtU64F: {
                    cb((&((in)->src)), roleUse);
                    cb((&((in)->dst)), roleDef);
                    break;
                }
                case mFMinMax: {
                    cb((&((in)->src)), roleUse);
                    cb((&((in)->src2)), roleUse);
                    cb((&((in)->dst)), roleDef);
                    break;
                }
                case mFSelect: {
                    cb((&((in)->src)), roleUse);
                    cb((&((in)->src2)), roleUse);
                    {
                        auto range89 = mInstUses(in);
                        for (Int index90 = 0, rangeEnd91 = length(range89); index90 < rangeEnd91; ++index90) {
                            Int i = index90;
                            cb((&(mInstUses(in)[i])), roleUse);
                        }
                    }
                    cb((&((in)->dst)), roleDef);
                    break;
                }
                case mFAluMem: {
                    mem();
                    cb((&((in)->src)), roleUse);
                    cb((&((in)->dst)), roleDef);
                    break;
                }
            }
        }
        {
            auto range92 = mInstTemps(in);
            for (Int index93 = 0, rangeEnd94 = length(range92); index93 < rangeEnd94; ++index93) {
                Int i = index93;
                cb((&(mInstTemps(in)[i])), roleTemp);
            }
        }
        if ((((((((((in)->op == mDiv) || ((in)->op == mCall)) || ((in)->op == mCallInd)) || ((in)->op == mRet)) || ((in)->op == mSignExtAx)) || ((in)->op == mZeroDx)) || ((in)->op == mGrowMem)) || ((in)->op == mCopyForward))) {
            if ((((in)->op == mDiv) || ((in)->op == mGrowMem))) {
                cb((&((in)->src)), roleUse);
            }
            {
                auto range95 = mInstUses(in);
                for (Int index96 = 0, rangeEnd97 = length(range95); index96 < rangeEnd97; ++index96) {
                    Int i = index96;
                    cb((&(mInstUses(in)[i])), roleUse);
                }
            }
            {
                auto range98 = mInstDefs(in);
                for (Int index99 = 0, rangeEnd100 = length(range98); index99 < rangeEnd100; ++index99) {
                    Int i = index99;
                    cb((&(mInstDefs(in)[i])), roleDef);
                }
            }
        }
    }

    static i32 memoryLoopClone(MemoryLoop* l, Map<i32, bool> proven, Map<i32, i32> invariants, Map<i32, Int> cases) {
        Function* f = l->f;
        const Int oldBlocks = length(f->blocks);
        const Int oldValues = length(f->values);
        const bool indexed = l->bodyUses && l->bodyUses->prepare();
        Vec<i32> blocks;
        Vec<i32> values;
        blocks.resize(usize(oldBlocks));
        values.resize(usize(oldValues));
        blocks.fill(-1);
        values.fill(-1);
        for (Int i = 0; i < mapSlots(invariants); ++i) {
            if (mapOccupied(invariants, i)) {
                values[usize(mapKey(invariants, i))] = mapValue(invariants, i);
            }
        }
        for (Int i = 0; i < length(l->order); ++i) {
            blocks[usize(l->order[i])] = functionNewBlock(f);
        }

        struct ClonedValue {
            i32 original;
            bool checked;
        };

        Vec<ClonedValue> cloned;
        // Value IDs retain their original order, including values no longer
        // present in instruction lists, so allocation and code layout agree.
        Vec<Lrange> ranges;
        if (indexed) {
            for (Int b = 0; b < length(l->members); ++b) {
                if (l->members[b]) {
                    const Lrange range = l->bodyUses->valueRanges[usize(b)];
                    if (range.from < range.to) {
                        ranges.pushBack(range);
                    }
                }
            }
            if (ranges.size() > 1) {
                sortValues(ranges.view(), [&](Int i, Int j) {
                    return ranges[usize(i)].from < ranges[usize(j)].from;
                });
            }
        } else {
            ranges.pushBack({0, oldValues});
        }
        usize nextRange = 0;
        while (nextRange < ranges.size()) {
            Lrange range = ranges[nextRange++];
            while (nextRange < ranges.size() && ranges[nextRange].from <= range.to) {
                const i32 end = ranges[nextRange++].to;
                range.to = range.to > end ? range.to : end;
            }
            for (i32 v = range.from; v < range.to; ++v) {
                const Value val = f->values[v];
                if (val.block < 0 || val.block >= length(l->members) || !l->members[val.block] || values[usize(v)] >= 0) {
                    continue;
                }
                const bool checked = proven[v];
                const i32 n = functionNewValue(f, val.op, val.typ, blocks[usize(val.block)], val.aux, {});
                f->values[n].aux2 = val.aux2 | (checked ? i32(32) : i32(0));
                values[usize(v)] = n;
                cloned.pushBack({v, checked});
            }
        }
        auto remap = [&](i32 v) {
            const i32 mapped = values[usize(v)];
            return mapped >= 0 ? mapped : v;
        };
        for (usize i = 0; i < cloned.size(); ++i) {
            const ClonedValue entry = cloned[i];
            const i32 n = values[usize(entry.original)];
            const auto args = functionArgs(f, entry.original);
            i32 address = 0;
            if (entry.checked) {
                address = args[0];
                while (l->noWrap[address]) {
                    const auto add = functionArgs(f, address);
                    i32 a = add[0];
                    Value k = f->values[add[1]];
                    if (k.op != OpConst) {
                        a = add[1];
                        k = f->values[add[0]];
                    }
                    if (k.op != OpConst || u64(u32(k.aux)) + u64(f->values[n].aux) > u64(2147483647)) {
                        break;
                    }
                    f->values[n].aux += i64(u32(k.aux));
                    address = a;
                }
            }
            f->values[n].args = i32(f->pool.size());
            f->values[n].nargs = u16(args.count);
            for (Int j = 0; j < length(args); ++j) {
                f->pool.pushBack(remap(entry.checked && j == 0 ? address : args[j]));
            }
        }
        auto remapArgs = [&](Slice<i32> args) {
            if (!length(args)) {
                return Slice<i32>{};
            }
            auto result = makeSlice<i32>(length(args));
            for (Int i = 0; i < length(args); ++i) {
                result[i] = remap(args[i]);
            }
            return result;
        };
        for (Int i = 0; i < length(l->order); ++i) {
            const i32 b = l->order[i];
            const Block& src = f->blocks[b];
            Block& dst = f->blocks[blocks[usize(b)]];
            dst.params = remapArgs(src.params);
            dst.instrs.reserve(src.instrs.size());
            for (Int j = 0; j < length(src.instrs); ++j) {
                const i32 v = src.instrs[j];
                // Hoisted values existed before cloning; new copies have IDs
                // at or above oldValues. An unmapped value stays unchanged.
                if (values[usize(v)] >= 0 && values[usize(v)] < oldValues) {
                    continue;
                }
                dst.instrs.pushBack(remap(v));
            }
            Term term = src.term;
            const auto selected = lookupMap(cases, b);
            if (selected.v1) {
                term = Term{TermJump, {}, subslice(term.edges, selected.v0, selected.v0 + 1), {}, {}};
            }
            dst.term = Term{term.kind, term.value, {}, remapArgs(term.values), term.aux};
            if (term.kind == TermIf || term.kind == TermSwitch) {
                dst.term.value = remap(term.value);
            }
            if (length(term.edges)) {
                dst.term.edges = makeSlice<Edge>(length(term.edges));
            }
            for (Int j = 0; j < length(term.edges); ++j) {
                const Edge edge = term.edges[j];
                const i32 mapped = blocks[usize(edge.block)];
                dst.term.edges[j] = Edge{mapped >= 0 ? mapped : edge.block, remapArgs(edge.args)};
            }
        }
        ObjPool temporary;
        Bitset escapes;
        {
            PoolScope scope(temporary);
            escapes = newBitset(oldValues);
        }
        Vec<i32> escapedValues;
        Vec<i32*> outsideUses;
        bool scanBodies = true;
        if (indexed) {
            auto eligible = [&](i32 v) {
                if (u32(v) >= u32(oldValues) || values[usize(v)] < 0) {
                    return false;
                }
                const i32 block = f->values[v].block;
                return block >= 0 && block < length(l->members) && l->members[block];
            };
            auto account = [&](bool restore) {
                for (Int i = 0; i < length(l->order); ++i) {
                    const auto& block = f->blocks[l->order[i]];
                    if (block.dead) {
                        continue;
                    }
                    for (Int j = 0; j < length(block.instrs); ++j) {
                        const auto args = functionArgs(f, block.instrs[j]);
                        for (Int k = 0; k < length(args); ++k) {
                            const i32 v = args[k];
                            if (eligible(v)) {
                                u32& count = l->bodyUses->counts[usize(v)];
                                if (count != 0xffffffffu) {
                                    count = restore ? count + 1 : count ? count - 1 : 0xffffffffu;
                                }
                            }
                        }
                    }
                }
            };
            account(false);
            scanBodies = false;
            for (usize i = 0; i < cloned.size(); ++i) {
                if (eligible(cloned[i].original) && l->bodyUses->counts[usize(cloned[i].original)] != 0) {
                    scanBodies = true;
                    break;
                }
            }
            for (Int i = 0; !scanBodies && i < mapSlots(invariants); ++i) {
                if (mapOccupied(invariants, i)) {
                    const i32 v = mapKey(invariants, i);
                    scanBodies = eligible(v) && l->bodyUses->counts[usize(v)] != 0;
                }
            }
            account(true);
        }
        auto mark = [&](i32* use) {
            const i32 v = *use;
            const i32 block = f->values[v].block;
            if (block < 0 || block >= length(l->members) || !l->members[block] || values[usize(v)] < 0) {
                return;
            }
            if (!bitsetHas(escapes, v)) {
                bitsetSet(escapes, v);
                escapedValues.pushBack(v);
            }
            // Operand pages and Block objects do not move. The exit-argument
            // appends below only grow edges inside the original/copied loop.
            outsideUses.pushBack(use);
        };
        for (Int b = 0; b < oldBlocks; ++b) {
            if (f->blocks[b].dead || (b < length(l->members) && l->members[b])) {
                continue;
            }
            Block& block = f->blocks[b];
            if (scanBodies) {
                for (Int i = 0; i < length(block.instrs); ++i) {
                    auto args = functionArgs(f, block.instrs[i]);
                    for (Int j = 0; j < length(args); ++j) {
                        mark(&args[j]);
                    }
                }
            }
            if (block.term.kind == TermIf || block.term.kind == TermSwitch) {
                mark(&block.term.value);
            }
            for (Int i = 0; i < length(block.term.values); ++i) {
                mark(&block.term.values[i]);
            }
            for (Int i = 0; i < length(block.term.edges); ++i) {
                auto args = block.term.edges[i].args;
                for (Int j = 0; j < length(args); ++j) {
                    mark(&args[j]);
                }
            }
        }
        if (escapedValues.size() > 1) {
            sortValues(escapedValues.view(), [&](Int i, Int j) {
                return escapedValues[usize(i)] < escapedValues[usize(j)];
            });
        }
        const i32 mergedBase = i32(f->values.size());
        for (usize i = 0; i < escapedValues.size(); ++i) {
            const i32 v = escapedValues[i];
            const i32 fast = values[usize(v)];
            const i32 p = functionNewValue(f, OpParam, f->values[v].typ, l->exit, i64(length(f->blocks[l->exit].params)), {});
            if (l->bodyUses) {
                l->bodyUses->unknown(p);
            }
            f->blocks[l->exit].params = appendValues(f->blocks[l->exit].params, p);
            for (Int j = 0; j < length(l->order); ++j) {
                const i32 b = l->order[j];
                auto original = f->blocks[b].term.edges;
                auto copy = f->blocks[blocks[usize(b)]].term.edges;
                for (Int k = 0; k < length(original); ++k) {
                    if (original[k].block == l->exit) {
                        original[k].args = appendValues(original[k].args, v);
                    }
                }
                for (Int k = 0; k < length(copy); ++k) {
                    if (copy[k].block == l->exit) {
                        copy[k].args = appendValues(copy[k].args, fast);
                    }
                }
            }
            // Fast-copy lookups are finished for v. Reuse its slot for the
            // exit parameter; all such IDs are at or above mergedBase.
            values[usize(v)] = p;
        }
        for (usize i = 0; i < outsideUses.size(); ++i) {
            i32* use = outsideUses[i];
            // A shared operand view may record the same address twice.
            if (u32(*use) < u32(oldValues)) {
                const i32 mapped = values[usize(*use)];
                if (mapped >= mergedBase) {
                    *use = mapped;
                }
            }
        }
        return blocks[usize(l->header)];
    }

    static CopyAddress memoryLoopCopyAddress(MemoryLoop* l, i32 v, Slice<i64> steps, Map<i32, i32> invariants, Map<i32, CopyAddress> cache, Int depth) {
        {
            auto temporary185 = lookupMap(cache, v);
            CopyAddress known = temporary185.v0;
            bool ok = temporary185.v1;
            if (ok) {
                return known;
            }
        }
        Function* f = (l)->f;
        Value val = (f)->values[v];
        CopyAddress bad = CopyAddress{};
        if (((depth > Int(24LL)) || ((val).typ != I32))) {
            return bad;
        }
        CopyAddress out = CopyAddress{mapOf<i32, u32>({}), {}, {}, true};
        auto temporary186 = lookupMap(invariants, v);
        i32 mapped = temporary186.v0;
        bool hoisted = temporary186.v1;
        if (((val).op == OpConst)) {
            (out).offset = convert<u32>((val).aux);
        } else if (((hoisted || ((val).block != (l)->header)) || ((val).op == OpParam))) {
            i32 key = v;
            if (hoisted) {
                key = mapped;
            } else if (((val).block == (l)->header)) {
                (out).step = convert<u32>(steps[(val).aux]);
            }
            setMap((out).terms, key, u32(1LL));
        } else {
            Span<i32> args = functionArgs(f, v);
            {
                auto switch187 = (val).op;
                switch (switch187) {
                    case OpAdd:
                    case OpSub: {
                        CopyAddress a = memoryLoopCopyAddress(l, args[Int(0LL)], steps, invariants, cache, (depth + Int(1LL)));
                        CopyAddress b = memoryLoopCopyAddress(l, args[Int(1LL)], steps, invariants, cache, (depth + Int(1LL)));
                        if (((!((a).valid)) || (!((b).valid)))) {
                            return bad;
                        }
                        u32 mul = u32(1LL);
                        if (((val).op == OpSub)) {
                            mul = u32(4294967295LL);
                        }
                        auto temporary188 = ((a).offset + (mul * (b).offset));
                        auto temporary189 = ((a).step + (mul * (b).step));
                        (out).offset = temporary188;
                        (out).step = temporary189;
                        {
                            auto range190 = (a).terms;
                            for (Int index191 = 0; index191 < mapSlots(range190); ++index191) {
                                if (!mapOccupied(range190, index191)) {
                                    continue;
                                }
                                i32 v = mapKey(range190, index191);
                                u32 k = mapValue(range190, index191);
                                setMap((out).terms, v, k);
                            }
                        }
                        {
                            auto range193 = (b).terms;
                            for (Int index194 = 0; index194 < mapSlots(range193); ++index194) {
                                if (!mapOccupied(range193, index194)) {
                                    continue;
                                }
                                i32 v = mapKey(range193, index194);
                                u32 k = mapValue(range193, index194);
                                setMap((out).terms, v, ((out).terms[v] + (mul * k)));
                            }
                        }
                        break;
                    }
                    case OpShl:
                    case OpMul: {
                        auto temporary196 = args[Int(0LL)];
                        auto temporary197 = (f)->values[args[Int(1LL)]];
                        i32 a = temporary196;
                        Value k = temporary197;
                        if ((((val).op == OpMul) && ((k).op != OpConst))) {
                            auto temporary198 = args[Int(1LL)];
                            auto temporary199 = (f)->values[args[Int(0LL)]];
                            a = temporary198;
                            k = temporary199;
                        }
                        if (((k).op != OpConst)) {
                            return bad;
                        }
                        CopyAddress base = memoryLoopCopyAddress(l, a, steps, invariants, cache, (depth + Int(1LL)));
                        if ((!((base).valid))) {
                            return bad;
                        }
                        u32 mul = convert<u32>((k).aux);
                        if (((val).op == OpShl)) {
                            mul = shiftLeft(u32(1LL), ((mul & u32(31LL))));
                        }
                        auto temporary200 = ((base).offset * mul);
                        auto temporary201 = ((base).step * mul);
                        (out).offset = temporary200;
                        (out).step = temporary201;
                        {
                            auto range202 = (base).terms;
                            for (Int index203 = 0; index203 < mapSlots(range202); ++index203) {
                                if (!mapOccupied(range202, index203)) {
                                    continue;
                                }
                                i32 v = mapKey(range202, index203);
                                u32 k = mapValue(range202, index203);
                                setMap((out).terms, v, (k * mul));
                            }
                        }
                        break;
                    }
                    default: {
                        return bad;
                        break;
                    }
                }
            }
        }
        {
            auto range205 = (out).terms;
            for (Int index206 = 0; index206 < mapSlots(range205); ++index206) {
                if (!mapOccupied(range205, index206)) {
                    continue;
                }
                i32 v = mapKey(range205, index206);
                u32 k = mapValue(range205, index206);
                if ((k == u32(0LL))) {
                    eraseMap((out).terms, v);
                }
            }
        }
        setMap(cache, v, out);
        return out;
    }

    // A checked, contiguous byte-copy loop can use a string instruction.
    // Forward order preserves dependencies between overlapping loads and stores;
    // memory.copy's memmove semantics would change those dependencies.
    static i32 memoryLoopCopyLoop(MemoryLoop* l, i32 fast, i32 count, i32 size, Map<i32, bool> proven, Map<i32, i32> invariants) {
        Function* f = (l)->f;
        if ((length((l)->order) != Int(1LL))) {
            return i32(-1LL);
        }
        Slice<i32> loads = {};
        Slice<i32> stores = {};
        i32 pending = i32(-1LL);
        {
            auto range208 = ((f)->blocks[(l)->header]).instrs.view();
            for (Int index209 = 0, rangeEnd210 = length(range208); index209 < rangeEnd210; ++index209) {
                i32 v = range208[index209];
                Value* val = (&((f)->values[v]));
                {
                    auto switch211 = (val)->op;
                    switch (switch211) {
                        case OpLoad: {
                            if ((((pending >= i32(0LL)) || (!(proven[v]))) || (memKindWidth(convert<MemKind>((val)->aux2)) != Int(1LL)))) {
                                return i32(-1LL);
                            }
                            loads = appendValues(loads, v);
                            pending = v;
                            break;
                        }
                        case OpStore: {
                            if (((((pending < i32(0LL)) || (!(proven[v]))) || (memKindWidth(convert<MemKind>((val)->aux2)) != Int(1LL))) || (functionArgs(f, v)[Int(1LL)] != pending))) {
                                return i32(-1LL);
                            }
                            stores = appendValues(stores, v);
                            pending = i32(-1LL);
                            break;
                        }
                        default: {
                            if (readsState((val)->op)) {
                                return i32(-1LL);
                            }
                            break;
                        }
                    }
                }
            }
        }
        if (((pending >= i32(0LL)) || (length(loads) == Int(0LL)))) {
            return i32(-1LL);
        }
        Edge back = {};
        Edge exit = {};
        {
            auto range212 = (((f)->blocks[(l)->header]).term).edges;
            for (Int index213 = 0, rangeEnd214 = length(range212); index213 < rangeEnd214; ++index213) {
                Edge e = range212[index213];
                if (((e).block == (l)->header)) {
                    back = e;
                } else {
                    exit = e;
                }
            }
        }
        Slice<i64> steps = makeSlice<i64>(length(((f)->blocks[(l)->header]).params));
        {
            auto range215 = ((f)->blocks[(l)->header]).params;
            for (Int index216 = 0, rangeEnd217 = length(range215); index216 < rangeEnd217; ++index216) {
                Int j = index216;
                i32 p = range215[index216];
                if ((((f)->values[p]).typ != I32)) {
                    return i32(-1LL);
                }
                steps[j] = inductionStep(f, p, (back).args[j]);
                if (((steps[j] == i64(0LL)) && ((back).args[j] != p))) {
                    return i32(-1LL);
                }
            }
        }
        u64 width = convert<u64>(length(loads));
        Map<i32, CopyAddress> addresses = mapOf<i32, CopyAddress>({});
        auto contiguous = [&](Slice<i32> values) mutable -> bool {
            CopyAddress first = {};
            {
                auto range218 = values;
                for (Int index219 = 0, rangeEnd220 = length(range218); index219 < rangeEnd220; ++index219) {
                    Int j = index219;
                    i32 v = range218[index219];
                    CopyAddress addr = memoryLoopCopyAddress(l, functionArgs(f, v)[Int(0LL)], steps, invariants, addresses, Int(0LL));
                    (addr).offset += convert<u32>(((f)->values[v]).aux);
                    if (((!((addr).valid)) || ((addr).step != convert<u32>(width)))) {
                        return false;
                    }
                    if ((j == Int(0LL))) {
                        first = addr;
                    } else if ((((addr).offset != ((first).offset + convert<u32>(j))) || (length((addr).terms) != length((first).terms)))) {
                        return false;
                    }
                    {
                        auto range221 = (first).terms;
                        for (Int index222 = 0; index222 < mapSlots(range221); ++index222) {
                            if (!mapOccupied(range221, index222)) {
                                continue;
                            }
                            i32 v = mapKey(range221, index222);
                            u32 mul = mapValue(range221, index222);
                            if (((addr).terms[v] != mul)) {
                                return false;
                            }
                        }
                    }
                }
            }
            return true;
        };
        if (((!(contiguous(loads))) || (!(contiguous(stores))))) {
            return i32(-1LL);
        }
        Map<i32, i32> reverse = mapOf<i32, i32>({});
        {
            auto range224 = ((f)->blocks[(l)->header]).params;
            for (Int index225 = 0, rangeEnd226 = length(range224); index225 < rangeEnd226; ++index225) {
                Int j = index225;
                i32 p = range224[index225];
                setMap(reverse, ((f)->blocks[fast]).params[j], p);
            }
        }
        Int index = Int(0LL);
        {
            auto range227 = ((f)->blocks[(l)->header]).instrs.view();
            for (Int index228 = 0, rangeEnd229 = length(range227); index228 < rangeEnd229; ++index228) {
                i32 v = range227[index228];
                {
                    auto temporary230 = lookupMap(invariants, v);
                    bool hoisted = temporary230.v1;
                    if ((!(hoisted))) {
                        setMap(reverse, ((f)->blocks[fast]).instrs[index], v);
                        index++;
                    }
                }
            }
        }
        Fn<bool(i32, Int)> can = {};
        can = [&](i32 v, Int depth) mutable -> bool {
            Value* val = (&((f)->values[v]));
            {
                auto temporary231 = lookupMap(invariants, v);
                bool hoisted = temporary231.v1;
                if (((hoisted || ((val)->block != (l)->header)) || ((val)->op == OpParam))) {
                    return true;
                }
            }
            if (((depth > Int(24LL)) || readsState((val)->op))) {
                return false;
            }
            {
                auto range232 = functionArgs(f, v);
                for (Int index233 = 0, rangeEnd234 = length(range232); index233 < rangeEnd234; ++index233) {
                    i32 a = range232[index233];
                    if ((!(can(a, (depth + Int(1LL)))))) {
                        return false;
                    }
                }
            }
            return true;
        };
        auto temporary235 = functionArgs(f, loads[Int(0LL)])[Int(0LL)];
        auto temporary236 = functionArgs(f, stores[Int(0LL)])[Int(0LL)];
        i32 source = temporary235;
        i32 dest = temporary236;
        if (((!(can(source, Int(0LL)))) || (!(can(dest, Int(0LL)))))) {
            return i32(-1LL);
        }
        Edge fastExit = (((f)->blocks[fast]).term).edges[Int(0LL)];
        if (((fastExit).block == fast)) {
            fastExit = (((f)->blocks[fast]).term).edges[Int(1LL)];
        }
        auto original = [&](i32 v) mutable -> i32 {
            {
                auto temporary237 = lookupMap(reverse, v);
                i32 old = temporary237.v0;
                bool ok = temporary237.v1;
                if (ok) {
                    return old;
                }
            }
            return v;
        };
        {
            auto range238 = (fastExit).args;
            for (Int index239 = 0, rangeEnd240 = length(range238); index239 < rangeEnd240; ++index239) {
                i32 v = range238[index239];
                if ((!(can(original(v), Int(0LL))))) {
                    return i32(-1LL);
                }
            }
        }
        i32 bulk = functionNewBlock(f);
        i32 current = bulk;
        {
            auto range241 = ((f)->blocks[fast]).params;
            for (Int index242 = 0, rangeEnd243 = length(range241); index242 < rangeEnd243; ++index242) {
                Int j = index242;
                i32 p = range241[index242];
                i32 v = functionNewValue(f, OpParam, ((f)->values[p]).typ, bulk, convert<i64>(j), Span<const i32>({}));
                ((f)->blocks[bulk]).params = appendValues(((f)->blocks[bulk]).params, v);
            }
        }
        auto emit = [&](Op op, ValType typ, i64 aux, Slice<i32> args) mutable -> i32 {
            i32 v = functionNewValue(f, op, typ, current, aux, args);
            ((f)->blocks[current]).instrs.pushBack(v);
            return v;
        };
        auto constant = [&](ValType typ, i64 x) mutable -> i32 {
            return functionNewValue(f, OpConst, typ, i32(-1LL), x, Span<const i32>({}));
        };
        i32 iterations = emit(OpExtendU, I64, i64(0LL), sliceOf<i32>({count}));
        if ((((l)->limit < i32(0LL)) && ((((l)->countStep != u64(1LL)) || ((l)->countStop != u64(0LL)))))) {
            iterations = emit(OpSub, I64, i64(0LL), sliceOf<i32>({iterations, constant(I64, convert<i64>(((l)->countStop + u64(1LL))))}));
            if ((((l)->countStep & (((l)->countStep - u64(1LL)))) == u64(0LL))) {
                iterations = emit(OpShrU, I64, i64(0LL), sliceOf<i32>({iterations, constant(I64, convert<i64>(bitsTrailingZeros64((l)->countStep)))}));
            } else {
                iterations = emit(OpDivU, I64, i64(0LL), sliceOf<i32>({iterations, constant(I64, convert<i64>((l)->countStep))}));
            }
            iterations = emit(OpAdd, I64, i64(0LL), sliceOf<i32>({iterations, constant(I64, i64(1LL))}));
        }
        i32 last = emit(OpWrap, I32, i64(0LL), sliceOf<i32>({emit(OpSub, I64, i64(0LL), sliceOf<i32>({iterations, constant(I64, i64(1LL))}))}));
        Fn<i32(i32, bool, Map<i32, i32>)> evaluate = {};
        evaluate = [&](i32 v, bool final, Map<i32, i32> cache) mutable -> i32 {
            {
                auto temporary244 = lookupMap(invariants, v);
                i32 mapped = temporary244.v0;
                bool ok = temporary244.v1;
                if (ok) {
                    return mapped;
                }
            }
            {
                auto temporary245 = lookupMap(cache, v);
                i32 mapped = temporary245.v0;
                bool ok = temporary245.v1;
                if (ok) {
                    return mapped;
                }
            }
            Value val = (f)->values[v];
            if (((val).block != (l)->header)) {
                return v;
            }
            i32 mapped = {};
            if (((val).op == OpParam)) {
                mapped = ((f)->blocks[bulk]).params[(val).aux];
                if ((final && (steps[(val).aux] != i64(0LL)))) {
                    i32 delta = emit(OpMul, I32, i64(0LL), sliceOf<i32>({last, constant(I32, steps[(val).aux])}));
                    mapped = emit(OpAdd, I32, i64(0LL), sliceOf<i32>({mapped, delta}));
                }
            } else {
                Slice<i32> args = appendSlice(Slice<i32>{}, functionArgs(f, v));
                {
                    auto range246 = args;
                    for (Int index247 = 0, rangeEnd248 = length(range246); index247 < rangeEnd248; ++index247) {
                        Int j = index247;
                        i32 a = range246[index247];
                        args[j] = evaluate(a, final, cache);
                    }
                }
                mapped = emit((val).op, (val).typ, (val).aux, args);
                ((f)->values[mapped]).aux2 = (val).aux2;
            }
            setMap(cache, v, mapped);
            return mapped;
        };
        Map<i32, i32> initial = mapOf<i32, i32>({});
        auto temporary249 = evaluate(source, false, initial);
        auto temporary250 = evaluate(dest, false, initial);
        i32 src = temporary249;
        i32 dst = temporary250;
        {
            i64 off = ((f)->values[loads[Int(0LL)]]).aux;
            if ((off != i64(0LL))) {
                src = emit(OpAdd, I32, i64(0LL), sliceOf<i32>({src, constant(I32, off)}));
            }
        }
        {
            i64 off = ((f)->values[stores[Int(0LL)]]).aux;
            if ((off != i64(0LL))) {
                dst = emit(OpAdd, I32, i64(0LL), sliceOf<i32>({dst, constant(I32, off)}));
            }
        }
        i32 lengthValue = emit(OpMul, I64, i64(0LL), sliceOf<i32>({iterations, constant(I64, convert<i64>(width))}));
        i32 condition = emit(OpGeU, I32, i64(0LL), sliceOf<i32>({lengthValue, constant(I64, i64(16LL))}));
        if ((l)->countSigned) {
            i32 positive = emit(OpLeU, I32, i64(0LL), sliceOf<i32>({count, constant(I32, i64(2147483647LL))}));
            condition = emit(OpAnd, I32, i64(0LL), sliceOf<i32>({condition, positive}));
        }
        {
            auto range251 = Array<i32, 2>{{src, dst}};
            for (Int index252 = 0, rangeEnd253 = length(range251); index252 < rangeEnd253; ++index252) {
                i32 addr = range251[index252];
                i32 end = emit(OpAdd, I64, i64(0LL), sliceOf<i32>({emit(OpExtendU, I64, i64(0LL), sliceOf<i32>({addr})), lengthValue}));
                condition = emit(OpAnd, I32, i64(0LL), sliceOf<i32>({condition, emit(OpLeU, I32, i64(0LL), sliceOf<i32>({end, size}))}));
            }
        }
        current = functionNewBlock(f);
        ((f)->blocks[bulk]).term = Term{TermIf, condition, sliceOf<Edge>({Edge{current, {}}, Edge{fast, appendSlice(Slice<i32>{}, ((f)->blocks[bulk]).params)}}), {}, {}};
        emit(OpCopyForward, TNone, i64(0LL), sliceOf<i32>({dst, src, lengthValue}));
        Map<i32, i32> final = mapOf<i32, i32>({});
        Slice<i32> args = makeSlice<i32>(length((fastExit).args));
        {
            auto range254 = (fastExit).args;
            for (Int index255 = 0, rangeEnd256 = length(range254); index255 < rangeEnd256; ++index255) {
                Int j = index255;
                i32 v = range254[index255];
                args[j] = evaluate(original(v), true, final);
            }
        }
        ((f)->blocks[current]).term = Term{TermJump, {}, sliceOf<Edge>({Edge{(exit).block, args}}), {}, {}};
        return bulk;
    }

    // With an entry that bypasses the loop, existing exit parameters already
    // merge the two paths. A new parameter for a collapsed live-out value
    // would need an undefined argument on that bypass, so leave that case alone.
    static bool memoryLoopHasLiveOut(MemoryLoop* l) {
        Function* f = (l)->f;
        auto inside = [&](i32 v) mutable -> bool {
            i32 b = ((f)->values[v]).block;
            return ((b >= i32(0LL)) && (l)->members[b]);
        };
        {
            auto& range257 = (f)->blocks;
            for (Int index258 = 0, rangeEnd259 = length(range257); index258 < rangeEnd259; ++index258) {
                Int b = index258;
                // The header dominates every definition in the loop and
                // therefore every direct use. Merged exit parameters have
                // their own definitions outside the loop.
                if (l->members[b] || f->blocks[b].dead || !cfgInfoDominates(l->ci, l->header, i32(b))) {
                    continue;
                }
                Block* blk = (&((f)->blocks[b]));
                {
                    auto range260 = (blk)->instrs.view();
                    for (Int index261 = 0, rangeEnd262 = length(range260); index261 < rangeEnd262; ++index261) {
                        i32 v = range260[index261];
                        {
                            auto range263 = functionArgs(f, v);
                            for (Int index264 = 0, rangeEnd265 = length(range263); index264 < rangeEnd265; ++index264) {
                                i32 a = range263[index264];
                                if (inside(a)) {
                                    return true;
                                }
                            }
                        }
                    }
                }
                if (((((((blk)->term).kind == TermIf) || (((blk)->term).kind == TermSwitch))) && inside(((blk)->term).value))) {
                    return true;
                }
                {
                    auto range266 = ((blk)->term).values;
                    for (Int index267 = 0, rangeEnd268 = length(range266); index267 < rangeEnd268; ++index267) {
                        i32 v = range266[index267];
                        if (inside(v)) {
                            return true;
                        }
                    }
                }
                {
                    auto range269 = ((blk)->term).edges;
                    for (Int index270 = 0, rangeEnd271 = length(range269); index270 < rangeEnd271; ++index270) {
                        Edge e = range269[index270];
                        {
                            auto range272 = (e).args;
                            for (Int index273 = 0, rangeEnd274 = length(range272); index273 < rangeEnd274; ++index273) {
                                i32 v = range272[index273];
                                if (inside(v)) {
                                    return true;
                                }
                            }
                        }
                    }
                }
            }
        }
        return false;
    }

    static bool memoryLoopInvariant(MemoryLoop* l, i32 v, Int depth) {
        Value* val = (&(((l)->f)->values[v]));
        if (((val)->block < i32(0LL))) {
            return true;
        }
        if ((!((l)->members[(val)->block]))) {
            return cfgInfoDominates((l)->ci, (val)->block, (l)->header);
        }
        {
            auto temporary275 = lookupMap((l)->invariantCache, v);
            bool known = temporary275.v0;
            bool ok = temporary275.v1;
            if (ok) {
                return known;
            }
        }
        setMap((l)->invariantCache, v, false);
        if (((depth > Int(32LL)) || (((((val)->typ != I32) && ((val)->typ != I64)) && ((val)->typ != F32)) && ((val)->typ != F64)))) {
            return false;
        }
        if (((val)->op == OpLoad)) {
            if ((((!((l)->allowLoads)) || (length((l)->invariantNodes) >= Int(128LL))) || (memKindWidth(convert<MemKind>((val)->aux2)) == Int(0LL)))) {
                return false;
            }
        } else if (readsState((val)->op)) {
            return false;
        }
        {
            auto range276 = functionArgs((l)->f, v);
            for (Int index277 = 0, rangeEnd278 = length(range276); index277 < rangeEnd278; ++index277) {
                i32 a = range276[index277];
                if ((!(memoryLoopInvariant(l, a, (depth + Int(1LL)))))) {
                    return false;
                }
            }
        }
        setMap((l)->invariantCache, v, true);
        (l)->invariantNodes = appendValues((l)->invariantNodes, v);
        return true;
    }

    static LoopSpan memoryLoopSpan(MemoryLoop* l, i32 v, Int depth) {
        {
            auto temporary279 = lookupMap((l)->induction, v);
            LoopSpan bound = temporary279.v0;
            bool ok = temporary279.v1;
            if (ok) {
                return bound;
            }
        }
        {
            auto temporary280 = lookupMap((l)->cache, v);
            LoopSpan bound = temporary280.v0;
            bool ok = temporary280.v1;
            if (ok) {
                return bound;
            }
        }
        LoopSpan bad = LoopSpan{};
        setMap((l)->cache, v, bad);
        Value* val = (&(((l)->f)->values[v]));
        if ((((val)->typ != I32) || (depth > Int(32LL)))) {
            return bad;
        }
        LoopSpan out = bad;
        if (((val)->op == OpConst)) {
            out = LoopSpan{i32(-1LL), {}, {}, convert<u64>(convert<u32>((val)->aux)), {}, {}, convert<u64>(convert<u32>((val)->aux)), {}, true};
        } else if (memoryLoopInvariant(l, v, Int(0LL))) {
            out = LoopSpan{v, u64(1LL), {}, {}, {}, {}, {}, {}, true};
        } else {
            UnsignedRange bound = unsignedRangesOf((l)->unsignedValue, v, Int(0LL));
            if (((bound).hi < u64(4294967295LL))) {
                out = LoopSpan{i32(-1LL), {}, {}, (bound).hi, {}, {}, (bound).lo, {}, true};
            } else {
                Span<i32> args = functionArgs((l)->f, v);
                {
                    auto switch281 = (val)->op;
                    switch (switch281) {
                        case OpAdd: {
                            auto temporary282 = memoryLoopSpan(l, args[Int(0LL)], (depth + Int(1LL)));
                            auto temporary283 = memoryLoopSpan(l, args[Int(1LL)], (depth + Int(1LL)));
                            LoopSpan a = temporary282;
                            LoopSpan b = temporary283;
                            out = addLoopSpans(a, b);
                            if ((out).valid) {
                                setMap((l)->noWrap, v, true);
                            }
                            break;
                        }
                        case OpShl: {
                            if (((((l)->f)->values[args[Int(1LL)]]).op == OpConst)) {
                                u32 n = (convert<u32>((((l)->f)->values[args[Int(1LL)]]).aux) & u32(31LL));
                                if ((n <= u32(16LL))) {
                                    out = memoryLoopSpan(l, args[Int(0LL)], (depth + Int(1LL)));
                                    (out).mul = shiftLeft((out).mul, n);
                                    (out).mul2 = shiftLeft((out).mul2, n);
                                    (out).factor = shiftLeft((out).factor, n);
                                    (out).offset = shiftLeft((out).offset, n);
                                    (out).lo = shiftLeft((out).lo, n);
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }
        if ((((((out).mul > u64(65536LL)) || ((out).mul2 > u64(65536LL))) || ((out).factor > u64(65536LL))) || ((out).offset > u64(1099511627776LL)))) {
            (out).valid = false;
        }
        setMap((l)->cache, v, out);
        return out;
    }

    static i32 memoryLoopVersion(MemoryLoop* l) {
        {
            Int attempt = Int(0LL);
            for (; (attempt < Int(2LL)); attempt++) {
                {
                    auto range284 = (l)->induction;
                    for (Int index285 = 0; index285 < mapSlots(range284); ++index285) {
                        if (!mapOccupied(range284, index285)) {
                            continue;
                        }
                        i32 p = mapKey(range284, index285);
                        LoopSpan bound = mapValue(range284, index285);
                        if (((bound).stride != i32(0LL))) {
                            eraseMap((l)->induction, p);
                        }
                    }
                }
                (l)->allowLoads = (((attempt == Int(0LL)) && ((((l)->f)->module)->importedFuncs == Int(0LL))) && (!(((((l)->f)->module)->mems[Int(0LL)]).imported)));
                {
                    auto range287 = (l)->order;
                    for (Int index288 = 0, rangeEnd289 = length(range287); index288 < rangeEnd289; ++index288) {
                        i32 b = range287[index288];
                        {
                            auto range290 = (((l)->f)->blocks[b]).instrs.view();
                            for (Int index291 = 0, rangeEnd292 = length(range290); index291 < rangeEnd292; ++index291) {
                                i32 v = range290[index291];
                                {
                                    auto switch293 = (((l)->f)->values[v]).op;
                                    switch (switch293) {
                                        case OpCall:
                                        case OpCallIndirect:
                                        case OpMemCopy:
                                        case OpMemFill:
                                        case OpMemInit:
                                        case OpCopyForward: {
                                            (l)->allowLoads = false;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                (l)->invariantCache = mapOf<i32, bool>({});
                (l)->invariantNodes = {};
                (l)->cache = mapOf<i32, LoopSpan>({});
                (l)->noWrap = mapOf<i32, bool>({});
                {
                    i32 fast = memoryLoopVersionOne(l);
                    if ((fast >= i32(0LL))) {
                        return fast;
                    }
                }
            }
        }
        return i32(-1LL);
    }

    static i32 memoryLoopVersionOne(MemoryLoop* l) {
        Function* f = (l)->f;
        if ((((l)->limit >= i32(0LL)) && (!(memoryLoopInvariant(l, (l)->limit, Int(0LL)))))) {
            return i32(-1LL);
        }
        Slice<i32> switches = {};
        {
            auto range294 = (l)->order;
            for (Int index295 = 0, rangeEnd296 = length(range294); index295 < rangeEnd296; ++index295) {
                i32 b = range294[index295];
                Term* t = (&(((f)->blocks[b]).term));
                if ((((((t)->kind == TermSwitch) && (length((t)->edges) >= Int(5LL))) && (((t)->edges[Int(0LL)]).block != ((t)->edges[(length((t)->edges) - Int(1LL))]).block)) && memoryLoopInvariant(l, (t)->value, Int(0LL)))) {
                    switches = appendValues(switches, b);
                }
            }
        }
        Slice<LoopSpan> writes = {};
        Map<RangeKeyLoop_version570, Int> writeIndex = mapOf<RangeKeyLoop_version570, Int>({});
        auto keyOf = [&](LoopSpan bound) mutable -> RangeKeyLoop_version570 {
            return RangeKeyLoop_version570{(bound).base, (bound).base2, (bound).stride, (bound).mul, (bound).mul2, (bound).factor};
        };
        {
            auto range297 = (((f)->blocks[(l)->latch]).term).edges;
            for (Int index298 = 0, rangeEnd299 = length(range297); index298 < rangeEnd299; ++index298) {
                Edge e = range297[index298];
                if (((e).block != (l)->header)) {
                    continue;
                }
                {
                    auto range300 = ((f)->blocks[(l)->header]).params;
                    for (Int index301 = 0, rangeEnd302 = length(range300); index301 < rangeEnd302; ++index301) {
                        Int j = index301;
                        i32 p = range300[index301];
                        {
                            auto temporary303 = lookupMap((l)->induction, p);
                            bool known = temporary303.v1;
                            if (known) {
                                continue;
                            }
                        }
                        auto temporary304 = inductionTerms(f, (e).args[j], p, Int(0LL));
                        Slice<i32> terms = temporary304.v0;
                        bool ok = temporary304.v1;
                        if ((!(ok))) {
                            continue;
                        }
                        LoopSpan bound = LoopSpan{((l)->entry).args[j], u64(1LL), {}, {}, {}, {}, {}, {}, true};
                        {
                            auto range305 = terms;
                            for (Int index306 = 0, rangeEnd307 = length(range305); index306 < rangeEnd307; ++index306) {
                                i32 v = range305[index306];
                                Value* val = (&((f)->values[v]));
                                if ((((((val)->op == OpConst) && ((bound).stride == i32(0LL))) && ((val)->aux >= i64(0LL))) && ((val)->aux <= i64(65536LL)))) {
                                    (bound).factor += convert<u64>((val)->aux);
                                } else if (((((val)->op != OpConst) && ((((bound).factor == u64(0LL)) || ((bound).stride == (v + i32(1LL)))))) && memoryLoopInvariant(l, v, Int(0LL)))) {
                                    (bound).stride = (v + i32(1LL));
                                    (bound).factor++;
                                } else {
                                    (bound).valid = false;
                                    break;
                                }
                            }
                        }
                        if (((bound).valid && ((bound).factor <= u64(65536LL)))) {
                            setMap((l)->induction, p, bound);
                        }
                    }
                }
            }
        }
        Map<RangeKeyLoop_version570, Int> indices = mapOf<RangeKeyLoop_version570, Int>({});
        Slice<LoopSpan> guards = {};
        Map<i32, bool> proven = mapOf<i32, bool>({});
        bool unboundedWrite = false;
        {
            auto range308 = (l)->order;
            for (Int index309 = 0, rangeEnd310 = length(range308); index309 < rangeEnd310; ++index309) {
                i32 b = range308[index309];
                {
                    auto range311 = ((f)->blocks[b]).instrs.view();
                    for (Int index312 = 0, rangeEnd313 = length(range311); index312 < rangeEnd313; ++index312) {
                        i32 v = range311[index312];
                        Value* val = (&((f)->values[v]));
                        if (((((val)->op != OpLoad) && ((val)->op != OpStore)) || (MemKind(convert<MemKind>((val)->aux2) & memProven) != MemKind(0LL)))) {
                            continue;
                        }
                        i32 addr = functionArgs(f, v)[Int(0LL)];
                        u64 end = (convert<u64>((val)->aux) + convert<u64>(memKindWidth(convert<MemKind>((val)->aux2))));
                        LoopSpan bound = memoryLoopSpan(l, addr, Int(0LL));
                        if (((val)->op == OpStore)) {
                            if ((bound).valid) {
                                LoopSpan write = bound;
                                (write).lo += convert<u64>((val)->aux);
                                (write).offset += end;
                                RangeKeyLoop_version570 key = keyOf(write);
                                {
                                    auto temporary314 = lookupMap(writeIndex, key);
                                    Int j = temporary314.v0;
                                    bool known = temporary314.v1;
                                    if (known) {
                                        (writes[j]).lo = minimum((writes[j]).lo, (write).lo);
                                        (writes[j]).offset = maximum((writes[j]).offset, (write).offset);
                                    } else {
                                        setMap(writeIndex, key, length(writes));
                                        writes = appendValues(writes, write);
                                    }
                                }
                            } else {
                                unboundedWrite = true;
                            }
                        }
                        if (((((unsignedRangesOf((l)->unsignedValue, addr, Int(0LL))).hi + end) <= (((((f)->module)->mems[Int(0LL)]).limits).min * u64(65536LL))) && (!((((f)->module)->mems[Int(0LL)]).imported)))) {
                            continue;
                        }
                        if ((!((bound).valid))) {
                            continue;
                        }
                        (bound).offset += end;
                        RangeKeyLoop_version570 key = keyOf(bound);
                        {
                            auto temporary315 = lookupMap(indices, key);
                            Int j = temporary315.v0;
                            bool ok = temporary315.v1;
                            if (ok) {
                                (guards[j]).offset = maximum((guards[j]).offset, (bound).offset);
                            } else {
                                setMap(indices, key, length(guards));
                                guards = appendValues(guards, bound);
                            }
                        }
                        setMap(proven, v, true);
                    }
                }
            }
        }
        bool hasLoad = false;
        {
            auto range316 = (l)->invariantNodes;
            for (Int index317 = 0, rangeEnd318 = length(range316); index317 < rangeEnd318; ++index317) {
                i32 v = range316[index317];
                hasLoad = (hasLoad || (((f)->values[v]).op == OpLoad));
            }
        }
        if ((((length(proven) < Int(2LL)) || (length(guards) > Int(24LL))) || (unboundedWrite && hasLoad))) {
            return i32(-1LL);
        }
        u64 minimum = u64(4LL);
        if ((length(proven) < Int(4LL))) {
            minimum = u64(8LL);
        }
        if (hasLoad) {
            minimum = u64(16LL);
        }
        u64 minimumCount = (((((minimum - u64(1LL))) * (l)->countStep) + (l)->countStop) + u64(1LL));
        if (((((l)->count >= i32(0LL)) && ((l)->countBias < i32(0LL))) && ((unsignedRangesOf((l)->unsignedValue, (l)->count, Int(0LL))).hi < minimumCount))) {
            return i32(-1LL);
        }
        i32 guard = functionNewBlock(f);
        i32 current = guard;
        auto emit = [&](Op op, ValType typ, Slice<i32> args) mutable -> i32 {
            i32 v = functionNewValue(f, op, typ, current, i64(0LL), args);
            ((f)->blocks[current]).instrs.pushBack(v);
            return v;
        };
        auto constant = [&](ValType typ, u64 x) mutable -> i32 {
            return functionNewValue(f, OpConst, typ, i32(-1LL), convert<i64>(x), Span<const i32>({}));
        };
        i32 pages = emit(OpMemSize, I32, sliceOf<i32>({}));
        i32 size = emit(OpShl, I64, sliceOf<i32>({emit(OpExtendU, I64, sliceOf<i32>({pages})), constant(I64, u64(16LL))}));
        i32 condition = constant(I32, u64(1LL));
        i32 countValue = (l)->count;
        if (((l)->count >= i32(0LL))) {
            if (((l)->countBias >= i32(0LL))) {
                countValue = emit(OpAdd, I32, sliceOf<i32>({countValue, (l)->countBias}));
            }
            condition = emit(OpGeU, I32, sliceOf<i32>({countValue, constant(I32, minimumCount)}));
            if (((l)->countMask != u64(0LL))) {
                i32 aligned = emit(OpEqz, I32, sliceOf<i32>({emit(OpAnd, I32, sliceOf<i32>({countValue, constant(I32, (l)->countMask)}))}));
                condition = emit(OpAnd, I32, sliceOf<i32>({condition, aligned}));
            }
        }
        Map<i32, i32> invariants = mapOf<i32, i32>({});
        auto mapValue = [&](i32 v) mutable -> i32 {
            {
                auto temporary319 = lookupMap(invariants, v);
                i32 mapped = temporary319.v0;
                bool ok = temporary319.v1;
                if (ok) {
                    return mapped;
                }
            }
            return v;
        };
        Slice<LoadRangeLoop_version741> reads = {};
        Map<CseKey, i32> available = mapOf<CseKey, i32>({});
        if ((length((l)->invariantNodes) > Int(0LL))) {
            i32 next = functionNewBlock(f);
            ((f)->blocks[current]).term = Term{TermIf, condition, sliceOf<Edge>({Edge{next, {}}, (l)->entry}), {}, {}};
            current = next;
        }
        {
            auto range320 = (l)->invariantNodes;
            for (Int index321 = 0, rangeEnd322 = length(range320); index321 < rangeEnd322; ++index321) {
                i32 v = range320[index321];
                Value val = (f)->values[v];
                Slice<i32> args = appendSlice(Slice<i32>{}, functionArgs(f, v));
                {
                    auto range323 = args;
                    for (Int index324 = 0, rangeEnd325 = length(range323); index324 < rangeEnd325; ++index324) {
                        Int j = index324;
                        args[j] = mapValue(args[j]);
                    }
                }
                CseKey key = CseKey{(val).op, (val).typ, (val).nargs, (val).aux, (val).aux2, {}};
                bool cacheable = (length(args) <= Int(3LL));
                if (cacheable) {
                    copySlice(subslice((key).args, Int(0), length((key).args)), args);
                    {
                        auto temporary326 = lookupMap(available, key);
                        i32 n = temporary326.v0;
                        bool known = temporary326.v1;
                        if (known) {
                            setMap(invariants, v, n);
                            continue;
                        }
                    }
                }
                if (((val).op == OpLoad)) {
                    i32 addr = emit(OpExtendU, I64, sliceOf<i32>({args[Int(0LL)]}));
                    i32 begin = emit(OpAdd, I64, sliceOf<i32>({addr, constant(I64, convert<u64>((val).aux))}));
                    i32 end = emit(OpAdd, I64, sliceOf<i32>({begin, constant(I64, convert<u64>(memKindWidth(convert<MemKind>((val).aux2))))}));
                    i32 ok = emit(OpLeU, I32, sliceOf<i32>({end, size}));
                    i32 next = functionNewBlock(f);
                    ((f)->blocks[current]).term = Term{TermIf, ok, sliceOf<Edge>({Edge{next, {}}, (l)->entry}), {}, {}};
                    current = next;
                    reads = appendValues(reads, LoadRangeLoop_version741{begin, end});
                    (val).aux2 |= i32(32LL);
                }
                i32 n = emit((val).op, (val).typ, args);
                auto temporary327 = (val).aux;
                auto temporary328 = (val).aux2;
                ((f)->values[n]).aux = temporary327;
                ((f)->values[n]).aux2 = temporary328;
                setMap(invariants, v, n);
                if (cacheable) {
                    setMap(available, key, n);
                }
            }
        }
        if (((l)->limit >= i32(0LL))) {
            i32 limit = mapValue((l)->limit);
            countValue = emit(OpSub, I32, sliceOf<i32>({limit, (l)->start}));
            condition = emit(OpLtU, I32, sliceOf<i32>({(l)->start, limit}));
            condition = emit(OpAnd, I32, sliceOf<i32>({condition, emit(OpGeU, I32, sliceOf<i32>({countValue, constant(I32, minimum)}))}));
            if ((l)->signedLimit) {
                condition = emit(OpAnd, I32, sliceOf<i32>({condition, emit(OpLeU, I32, sliceOf<i32>({limit, constant(I32, u64(2147483647LL))}))}));
            }
        }
        i32 count = emit(OpExtendU, I64, sliceOf<i32>({countValue}));
        i32 iterations = emit(OpSub, I64, sliceOf<i32>({count, constant(I64, u64(1LL))}));
        auto evaluate = [&](LoopSpan bound) mutable -> Tuple2<i32, i32> {
            i32 end = constant(I64, (bound).offset);
            i32 begin = constant(I64, (bound).lo);
            {
                auto range329 = Array<Array<i64, 2>, 2>{{Array<i64, 2>{{convert<i64>((bound).base), convert<i64>((bound).mul)}}, Array<i64, 2>{{convert<i64>((bound).base2), convert<i64>((bound).mul2)}}}};
                for (Int index330 = 0, rangeEnd331 = length(range329); index330 < rangeEnd331; ++index330) {
                    Array<i64, 2> term = range329[index330];
                    if ((term[Int(1LL)] == i64(0LL))) {
                        continue;
                    }
                    i32 base = emit(OpExtendU, I64, sliceOf<i32>({mapValue(convert<i32>(term[Int(0LL)]))}));
                    if ((term[Int(1LL)] != i64(1LL))) {
                        base = emit(OpMul, I64, sliceOf<i32>({base, constant(I64, convert<u64>(term[Int(1LL)]))}));
                    }
                    end = emit(OpAdd, I64, sliceOf<i32>({end, base}));
                    begin = emit(OpAdd, I64, sliceOf<i32>({begin, base}));
                }
            }
            if (((bound).factor != u64(0LL))) {
                i32 distance = emit(OpMul, I64, sliceOf<i32>({iterations, constant(I64, (bound).factor)}));
                if (((bound).stride != i32(0LL))) {
                    i32 stride = mapValue(((bound).stride - i32(1LL)));
                    condition = emit(OpAnd, I32, sliceOf<i32>({condition, emit(OpLeU, I32, sliceOf<i32>({stride, constant(I32, (u64(65536LL) / (bound).factor))}))}));
                    distance = emit(OpMul, I64, sliceOf<i32>({distance, emit(OpExtendU, I64, sliceOf<i32>({stride}))}));
                }
                end = emit(OpAdd, I64, sliceOf<i32>({end, distance}));
            }
            return {begin, end};
        };
        {
            auto range332 = guards;
            for (Int index333 = 0, rangeEnd334 = length(range332); index333 < rangeEnd334; ++index333) {
                LoopSpan bound = range332[index333];
                auto temporary335 = evaluate(bound);
                i32 end = temporary335.v1;
                condition = emit(OpAnd, I32, sliceOf<i32>({condition, emit(OpLeU, I32, sliceOf<i32>({end, size}))}));
            }
        }
        {
            auto range336 = writes;
            for (Int index337 = 0, rangeEnd338 = length(range336); index337 < rangeEnd338; ++index337) {
                LoopSpan write = range336[index337];
                auto temporary339 = evaluate(write);
                i32 begin = temporary339.v0;
                i32 end = temporary339.v1;
                condition = emit(OpAnd, I32, sliceOf<i32>({condition, emit(OpLeU, I32, sliceOf<i32>({end, size}))}));
                {
                    auto range340 = reads;
                    for (Int index341 = 0, rangeEnd342 = length(range340); index341 < rangeEnd342; ++index341) {
                        LoadRangeLoop_version741 read = range340[index341];
                        i32 disjoint = emit(OpOr, I32, sliceOf<i32>({emit(OpLeU, I32, sliceOf<i32>({end, (read).begin})), emit(OpLeU, I32, sliceOf<i32>({(read).end, begin}))}));
                        condition = emit(OpAnd, I32, sliceOf<i32>({condition, disjoint}));
                    }
                }
            }
        }
        i32 fast = memoryLoopClone(l, proven, invariants, {});
        (l)->copies = appendValues((l)->copies, fast);
        Edge entry = Edge{fast, appendSlice(Slice<i32>{}, ((l)->entry).args)};
        {
            i32 bulk = memoryLoopCopyLoop(l, fast, countValue, size, proven, invariants);
            if ((bulk >= i32(0LL))) {
                (entry).block = bulk;
            }
        }
        if ((length(switches) >= Int(2LL))) {
            i32 index = mapValue((((f)->blocks[switches[Int(0LL)]]).term).value);
            Map<i32, Int> cases = mapOf<i32, Int>({});
            {
                auto range343 = switches;
                for (Int index344 = 0, rangeEnd345 = length(range343); index344 < rangeEnd345; ++index344) {
                    i32 b = range343[index344];
                    if ((mapValue((((f)->blocks[b]).term).value) == index)) {
                        setMap(cases, b, Int(0LL));
                    }
                }
            }
            if ((length(cases) >= Int(2LL))) {
                i32 specialized = memoryLoopClone(l, proven, invariants, cases);
                (l)->copies = appendValues((l)->copies, specialized);
                i32 choice = functionNewBlock(f);
                i32 eq = functionNewValue(f, OpEqz, I32, choice, i64(0LL), Span<const i32>({index}));
                ((f)->blocks[choice]).instrs.pushBack(eq);
                ((f)->blocks[choice]).term = Term{TermIf, eq, sliceOf<Edge>({Edge{specialized, appendSlice(Slice<i32>{}, ((l)->entry).args)}, entry}), {}, {}};
                entry = Edge{choice, {}};
            }
        }
        ((f)->blocks[current]).term = Term{TermIf, condition, sliceOf<Edge>({entry, (l)->entry}), {}, {}};
        {
            auto range346 = (((f)->blocks[(l)->preheader]).term).edges;
            for (Int index347 = 0, rangeEnd348 = length(range346); index347 < rangeEnd348; ++index347) {
                Int j = index347;
                if ((((((f)->blocks[(l)->preheader]).term).edges[j]).block == (l)->header)) {
                    (((f)->blocks[(l)->preheader]).term).edges[j] = Edge{guard, {}};
                }
            }
        }
        return fast;
    }

    // Large collections of constant data segments do not need a compiler
    // pipeline per copy. Only omit their bounds checks when fresh, private
    // memory is guaranteed to contain every destination, including empty ones.
    static bool moduleCanBatchData(Module* m) {
        if (length(m->mems) != 1 || m->mems[0].imported) {
            return false;
        }
        const u64 minimum = m->mems[0].limits.min * u64(pageBytes);
        if (m->start >= 0) {
            if (u64(m->start) >= u64(length(m->funcs)) || m->funcs[Int(m->start)].returnsTwice) {
                return false;
            }
            const auto* type = moduleFuncType(m, u32(m->start));
            if (length(type->params) || length(type->results)) {
                return false;
            }
        }
        u32 count = 0;
        for (Int i = 0; i < length(m->datas); ++i) {
            const Data& data = m->datas[i];
            if (data.mode != segmentActive) {
                continue;
            }
            if (data.memory != 0 || !length(data.offset) || data.offset[0] != 0x41) {
                return false;
            }
            const auto offset = constOffset(data.offset);
            if (!offset.v1 || u64(offset.v0) > minimum || u64(length(data.bytes)) > minimum - u64(offset.v0)) {
                return false;
            }
            ++count;
        }
        // The existing initializer splits after 128 operations. Keep small
        // initializers on that path and bound the descriptor allocation.
        return count >= 128 && count <= 0x7fffffff / 16;
    }

    // addInitFunction appends a function that initializes the globals and
    // applies the active data segments, then calls the start function.
    static Int moduleAddInitFunction(Module* m) {
        m->batchData = moduleCanBatchData(m);
        Slice<u8> code = sliceOf<u8>({u8(0LL)});
        Slice<Slice<u8>> parts = {};
        Int steps = Int(0LL);
        auto next = [&]() mutable -> void {
            if ((steps == Int(128LL))) {
                parts = appendValues(parts, appendValues(code, u8(11LL)));
                code = sliceOf<u8>({u8(0LL)});
                steps = Int(0LL);
            }
            steps++;
        };
        {
            Int i = (m)->importedGlobals;
            for (; (i < length((m)->globals)); i++) {
                next();
                Global* g = (&((m)->globals[i]));
                code = appendSlice(code, subslice((g)->init, Int(0), (length((g)->init) - Int(1LL))));
                code = appendValues(code, u8(36LL));
                code = appendLeb(code, convert<u64>(i));
            }
        }
        {
            auto range349 = (m)->datas;
            for (Int index350 = 0, rangeEnd351 = length(range349); index350 < rangeEnd351; ++index350) {
                Int i = index350;
                Data d = range349[index350];
                if (m->batchData || d.mode != segmentActive) {
                    continue;
                }
                next();
                code = appendSlice(code, subslice((d).offset, Int(0), (length((d).offset) - Int(1LL))));
                code = appendValues(code, u8(65LL), u8(0LL), u8(65LL));
                code = appendSleb(code, convert<i64>(convert<i32>(convert<u32>(length((d).bytes)))));
                code = appendValues(code, u8(252LL), u8(8LL));
                code = appendLeb(code, convert<u64>(i));
                code = appendValues(code, u8(0LL), u8(252LL), u8(9LL));
                code = appendLeb(code, convert<u64>(i));
            }
        }
        Int typ = Int(-1LL);
        {
            auto range352 = (m)->types;
            for (Int index353 = 0, rangeEnd354 = length(range352); index353 < rangeEnd354; ++index353) {
                Int i = index353;
                FuncType t = range352[index353];
                if (((length((t).params) == Int(0LL)) && (length((t).results) == Int(0LL)))) {
                    typ = i;
                    break;
                }
            }
        }
        if ((typ < Int(0LL))) {
            (m)->types = appendValues((m)->types, FuncType{});
            typ = (length((m)->types) - Int(1LL));
        }
        if ((length(parts) > Int(0LL))) {
            parts = appendValues(parts, appendValues(code, u8(11LL)));
            code = sliceOf<u8>({u8(0LL)});
            {
                auto range355 = parts;
                for (Int index356 = 0, rangeEnd357 = length(range355); index356 < rangeEnd357; ++index356) {
                    Int k = index356;
                    Slice<u8> body = range355[index356];
                    Int fn = length((m)->funcs);
                    (m)->funcs = appendValues((m)->funcs, Func{convert<u32>(typ), {}, {}, fmtSprintf(String("init.part%d"), k), body, {}});
                    code = appendLeb(appendValues(code, u8(16LL)), convert<u64>(fn));
                }
            }
        }
        if (m->start >= 0 && !m->batchData) {
            code = appendLeb(appendValues(code, u8(16LL)), convert<u64>((m)->start));
        }
        code = appendValues(code, u8(11LL));
        if (((m)->dataCount < i64(0LL))) {
            (m)->dataCount = convert<i64>(length((m)->datas));
        }
        (m)->funcs = appendValues((m)->funcs, Func{convert<u32>(typ), {}, {}, String("init"), code, {}});
        return (length((m)->funcs) - Int(1LL));
    }

    static void moduleDataSection(Module* m, Reader* r) {
        u32 n = readerU32(r);
        {
            u32 i = u32(0LL);
            for (; (i < n); i++) {
                u32 flags = readerU32(r);
                Data d = Data{};
                {
                    auto switch358 = flags;
                    switch (switch358) {
                        case u32(0LL): {
                            (d).offset = readerConstExpr(r);
                            break;
                        }
                        case u32(1LL): {
                            (d).mode = segmentPassive;
                            break;
                        }
                        case u32(2LL): {
                            (d).memory = readerU32(r);
                            (d).offset = readerConstExpr(r);
                            break;
                        }
                        default: {
                            compilerError(String("malformed data segment kind %d"));
                            break;
                        }
                    }
                }
                (d).bytes = readerBytes(r, convert<i64>(readerU32(r)));
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
                Elem e = Elem{{}, {}, {}, FuncRef, {}};
                {
                    if (((flags & u32(1LL)) == u32(0LL))) {
                        (e).mode = segmentActive;
                    } else if (((flags & u32(2LL)) == u32(0LL))) {
                        (e).mode = segmentPassive;
                    } else {
                        (e).mode = segmentDeclarative;
                    }
                switchEnd360:;
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
                if (((e).kind > externGlobal)) {
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

    static FuncType* moduleFuncType(Module* m, u32 index) {
        return (&((m)->types[((m)->funcs[index]).typ]));
    }

    static void moduleImportSection(Module* m, Reader* r) {
        u32 n = readerU32(r);
        {
            u32 i = u32(0LL);
            for (; (i < n); i++) {
                Import imp = Import{readerName(r), readerName(r), readerByte(r), {}};
                {
                    auto switch361 = (imp).kind;
                    switch (switch361) {
                        case externFunc: {
                            (imp).index = convert<u32>(length((m)->funcs));
                            (m)->funcs = appendValues((m)->funcs, Func{readerU32(r), true, returnsTwice((imp).name), {}, {}, {}});
                            (m)->importedFuncs++;
                            break;
                        }
                        case externTable: {
                            (imp).index = convert<u32>(length((m)->tables));
                            ValType elem = readerRefType(r);
                            (m)->tables = appendValues((m)->tables, Table{elem, readerLimits(r, false), true});
                            (m)->importedTables++;
                            break;
                        }
                        case externMemory: {
                            (imp).index = convert<u32>(length((m)->mems));
                            (m)->mems = appendValues((m)->mems, Memory{readerLimits(r, true), true});
                            (m)->importedMems++;
                            break;
                        }
                        case externGlobal: {
                            (imp).index = convert<u32>(length((m)->globals));
                            Global g = Global{readerValType(r), {}, true, {}};
                            (g).mutableValue = globalMutability(r);
                            (m)->globals = appendValues((m)->globals, g);
                            (m)->importedGlobals++;
                            break;
                        }
                        default: {
                            compilerError(String("malformed import kind %d"));
                            break;
                        }
                    }
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

    // Ranges are unsigned and inclusive. Arithmetic that could wrap stays
    // unknown; a mask, a narrow load or a shift can bound it again. A cyclic
    // dependency starts at the full range, so loop parameters never assume an
    // unproved bound from their back edge.
    static UnsignedRange unsignedRangesOf(UnsignedRanges* r, i32 v, Int depth) {
        if ((r)->seen[v]) {
            return (r)->cache[v];
        }
        UnsignedRange unknown = UnsignedRange{u64(0LL), u64(4294967295LL)};
        (r)->seen[v] = true;
        (r)->cache[v] = unknown;
        Value* val = (&(((r)->f)->values[v]));
        if ((((val)->typ != I32) || (depth > Int(32LL)))) {
            return unknown;
        }
        UnsignedRange out = unknown;
        Span<i32> args = functionArgs((r)->f, v);
        {
            auto switch362 = (val)->op;
            switch (switch362) {
                case OpConst: {
                    u64 k = convert<u64>(convert<u32>((val)->aux));
                    out = UnsignedRange{k, k};
                    break;
                }
                case OpLoad: {
                    MemKind kind = convert<MemKind>((val)->aux2);
                    if (((!(memKindSigned(kind))) && (memKindWidth(kind) < Int(4LL)))) {
                        (out).hi = (shiftLeft(u64(1LL), convert<Uint>((memKindWidth(kind) * Int(8LL)))) - u64(1LL));
                    }
                    break;
                }
                case OpEqz:
                case OpEq:
                case OpNe:
                case OpLtS:
                case OpLtU:
                case OpGtS:
                case OpGtU:
                case OpLeS:
                case OpLeU:
                case OpGeS:
                case OpGeU:
                case OpFEq:
                case OpFNe:
                case OpFLt:
                case OpFGt:
                case OpFLe:
                case OpFGe:
                case OpRefIsNull: {
                    (out).hi = u64(1LL);
                    break;
                }
                case OpAnd:
                case OpAdd:
                case OpSub:
                case OpMul:
                case OpShrU:
                case OpShl: {
                    auto temporary363 = unsignedRangesOf(r, args[Int(0LL)], (depth + Int(1LL)));
                    auto temporary364 = unsignedRangesOf(r, args[Int(1LL)], (depth + Int(1LL)));
                    UnsignedRange a = temporary363;
                    UnsignedRange b = temporary364;
                    {
                        auto switch365 = (val)->op;
                        switch (switch365) {
                            case OpAnd: {
                                (out).hi = minimum((a).hi, (b).hi);
                                break;
                            }
                            case OpAdd: {
                                if ((((a).hi + (b).hi) <= u64(4294967295LL))) {
                                    out = UnsignedRange{((a).lo + (b).lo), ((a).hi + (b).hi)};
                                }
                                break;
                            }
                            case OpSub: {
                                if (((a).lo >= (b).hi)) {
                                    out = UnsignedRange{((a).lo - (b).hi), ((a).hi - (b).lo)};
                                }
                                break;
                            }
                            case OpMul: {
                                if ((((a).hi * (b).hi) <= u64(4294967295LL))) {
                                    out = UnsignedRange{((a).lo * (b).lo), ((a).hi * (b).hi)};
                                }
                                break;
                            }
                            case OpShrU: {
                                if (((b).lo == (b).hi)) {
                                    u64 shift = ((b).lo & u64(31LL));
                                    out = UnsignedRange{shiftRight((a).lo, shift), shiftRight((a).hi, shift)};
                                }
                                break;
                            }
                            case OpShl: {
                                if (((b).lo == (b).hi)) {
                                    u64 shift = ((b).lo & u64(31LL));
                                    if (((a).hi <= shiftRight(u64(4294967295LL), shift))) {
                                        out = UnsignedRange{shiftLeft((a).lo, shift), shiftLeft((a).hi, shift)};
                                    }
                                }
                                break;
                            }
                        }
                    }
                    break;
                }
                case OpSelect: {
                    auto temporary366 = unsignedRangesOf(r, args[Int(1LL)], (depth + Int(1LL)));
                    auto temporary367 = unsignedRangesOf(r, args[Int(2LL)], (depth + Int(1LL)));
                    UnsignedRange a = temporary366;
                    UnsignedRange b = temporary367;
                    out = UnsignedRange{minimum((a).lo, (b).lo), maximum((a).hi, (b).hi)};
                    break;
                }
                case OpParam: {
                    bool first = true;
                    {
                        auto range368 = (((r)->f)->blocks[(val)->block]).preds;
                        for (Int index369 = 0, rangeEnd370 = length(range368); index369 < rangeEnd370; ++index369) {
                            i32 p = range368[index369];
                            {
                                auto range371 = ((((r)->f)->blocks[p]).term).edges;
                                for (Int index372 = 0, rangeEnd373 = length(range371); index372 < rangeEnd373; ++index372) {
                                    Edge e = range371[index372];
                                    if (((e).block != (val)->block)) {
                                        continue;
                                    }
                                    UnsignedRange bound = unsignedRangesOf(r, (e).args[(val)->aux], (depth + Int(1LL)));
                                    if (first) {
                                        out = bound;
                                        first = false;
                                    } else {
                                        out = UnsignedRange{minimum((out).lo, (bound).lo), maximum((out).hi, (bound).hi)};
                                    }
                                }
                            }
                        }
                    }
                    break;
                }
                case OpWrap: {
                    if (((((r)->f)->values[args[Int(0LL)]]).op == OpExtendU)) {
                        out = unsignedRangesOf(r, functionArgs((r)->f, args[Int(0LL)])[Int(0LL)], (depth + Int(1LL)));
                    }
                    break;
                }
            }
        }
        (r)->cache[v] = out;
        return out;
    }

    static void allocatorAddUnhandled(Allocator* a, Interval* it) {
        a->unhandled.push(it, intervalStart(it));
    }

    static i32 allocatorAdvance(Allocator* a, i32 pos) {
        i32 nextChange = maxPos;
        Span<Interval*> active = subslice(a->active.view(), 0, 0);
        Interval* moved[32];
        usize movedCount = 0;
        {
            auto range374 = (a)->active.view();
            for (Int index375 = 0, rangeEnd376 = length(range374); index375 < rangeEnd376; ++index375) {
                Interval* it = range374[index375];
                {
                    if (intervalEndsBefore(it, pos)) {
                    } else if ((!(intervalCoversAt(it, pos)))) {
                        require(movedCount < 32, "too many active register intervals");
                        moved[movedCount++] = it;
                        nextChange = minimum(nextChange, it->ranges[it->cursor].from);
                    } else {
                        active[active.count++] = it;
                        nextChange = minimum(nextChange, it->ranges[it->cursor].to);
                    }
                switchEnd378:;
                }
            }
        }
        a->active.resize(active.count);
        Span<Interval*> inactive = subslice(a->inactive.view(), 0, 0);
        {
            auto range379 = (a)->inactive.view();
            for (Int index380 = 0, rangeEnd381 = length(range379); index380 < rangeEnd381; ++index380) {
                Interval* it = range379[index380];
                {
                    if (intervalEndsBefore(it, pos)) {
                    } else if (intervalCoversAt(it, pos)) {
                        a->active.pushBack(it);
                        nextChange = minimum(nextChange, it->ranges[it->cursor].to);
                    } else {
                        inactive[inactive.count++] = it;
                        nextChange = minimum(nextChange, it->ranges[it->cursor].from);
                    }
                switchEnd383:;
                }
            }
        }
        a->inactive.resize(inactive.count);
        a->inactive.append(Span<Interval* const>(moved, movedCount));
        return nextChange;
    }

    static void allocatorAllocateBlocked(Allocator* a, Interval* cur) {
        Array<i32, 32> nextUse = {};
        Array<i32, 32> blockPos = {};
        {
            auto range384 = allocatorCandidates(a, (cur)->classValue);
            for (Int index385 = 0, rangeEnd386 = length(range384); index385 < rangeEnd386; ++index385) {
                VReg r = range384[index385];
                nextUse[r] = maxPos;
                blockPos[r] = maxPos;
            }
        }
        i32 start = intervalStart(cur);
        {
            auto range387 = (a)->active.view();
            for (Int index388 = 0, rangeEnd389 = length(range387); index388 < rangeEnd389; ++index388) {
                Interval* it = range387[index388];
                if (vRegIsPhys(it->vreg)) {
                    nextUse[(it)->reg] = i32(0LL);
                    blockPos[(it)->reg] = i32(0LL);
                    continue;
                }
                i32 u = intervalNextUse(it, start);
                if ((u == start)) {
                    u = i32(0LL);
                }
                if ((u < nextUse[(it)->reg])) {
                    nextUse[(it)->reg] = u;
                }
            }
        }
        {
            auto range390 = (a)->inactive.view();
            for (Int index391 = 0, rangeEnd392 = length(range390); index391 < rangeEnd392; ++index391) {
                Interval* it = range390[index391];
                i32 x = intervalIntersection(it, cur);
                if ((x == maxPos)) {
                    continue;
                }
                if (vRegIsPhys(it->vreg)) {
                    if ((x < blockPos[(it)->reg])) {
                        blockPos[(it)->reg] = x;
                    }
                    if ((x < nextUse[(it)->reg])) {
                        nextUse[(it)->reg] = x;
                    }
                    continue;
                }
                {
                    i32 u = intervalNextUse(it, start);
                    if ((u < nextUse[(it)->reg])) {
                        nextUse[(it)->reg] = u;
                    }
                }
            }
        }
        VReg reg = noReg;
        {
            auto range393 = allocatorCandidates(a, (cur)->classValue);
            for (Int index394 = 0, rangeEnd395 = length(range393); index394 < rangeEnd395; ++index394) {
                VReg r = range393[index394];
                if (((reg == noReg) || (nextUse[r] > nextUse[reg]))) {
                    reg = r;
                }
            }
        }
        i32 firstUse = intervalNextUse(cur, start);
        if ((((firstUse > nextUse[reg]) || (blockPos[reg] <= (start + i32(1LL)))) || (nextUse[reg] == i32(0LL)))) {
            allocatorSpill(a, cur);
            if ((firstUse < maxPos)) {
                i32 at = allocatorSplitPos(a, (start + i32(1LL)), firstUse);
                if ((at < i32(0LL))) {
                    compilerError(String("internal error: no register for %s at %d"));
                }
                allocatorAddUnhandled(a, intervalSplit(cur, at));
            }
            return;
        }
        (cur)->reg = reg;
        (a)->used[reg] = true;
        if ((blockPos[reg] < intervalEnd(cur))) {
            i32 at = allocatorSplitPos(a, (start + i32(1LL)), blockPos[reg]);
            if ((at < i32(0LL))) {
                compilerError(String("internal error: no split position for %s before %d"));
            }
            allocatorAddUnhandled(a, intervalSplit(cur, at));
        }
        Span<Interval*> active = subslice(a->active.view(), 0, 0);
        {
            auto range396 = (a)->active.view();
            for (Int index397 = 0, rangeEnd398 = length(range396); index397 < rangeEnd398; ++index397) {
                Interval* it = range396[index397];
                if (it->reg != reg || vRegIsPhys(it->vreg)) {
                    active[active.count++] = it;
                    continue;
                }
                allocatorEvict(a, it, start);
            }
        }
        a->active.resize(active.count);
        Span<Interval*> inactive = subslice(a->inactive.view(), 0, 0);
        {
            auto range399 = (a)->inactive.view();
            for (Int index400 = 0, rangeEnd401 = length(range399); index400 < rangeEnd401; ++index400) {
                Interval* it = range399[index400];
                if (it->reg != reg || vRegIsPhys(it->vreg)) {
                    inactive[inactive.count++] = it;
                    continue;
                }
                i32 x = intervalIntersection(it, cur);
                if ((x == maxPos)) {
                    inactive[inactive.count++] = it;
                    continue;
                }
                i32 lo = (intervalPrevUse(it, x) + i32(1LL));
                if ((lo < start)) {
                    lo = start;
                }
                i32 at = allocatorSplitPos(a, lo, x);
                bool split = ((at >= i32(0LL)) && (at > intervalStart(it)));
                if (((split && allocatorCanEvict(a, it, intervalCovered(it, at))) || ((!(split)) && allocatorCanEvict(a, it, start)))) {
                    if (split) {
                        Interval* child = intervalSplit(it, at);
                        allocatorEvict(a, child, intervalStart(child));
                    } else {
                        allocatorEvict(a, it, start);
                        continue;
                    }
                    inactive[inactive.count++] = it;
                    continue;
                }
                i32 cut = allocatorSplitPos(a, (start + i32(1LL)), x);
                if ((cut < i32(0LL))) {
                    compilerError(String("internal error: no split position for %s before %d"));
                }
                allocatorAddUnhandled(a, intervalSplit(cur, cut));
                inactive[inactive.count++] = it;
            }
        }
        a->inactive.resize(inactive.count);
    }

    // The coalescing pass only needs ranges of virtual copy operands. Liveness
    // is independent for each register, so projecting its sets gives the same
    // ranges without building physical intervals, uses or allocation hints.
    template <bool copiesOnly>
    static void allocatorBuildIntervals(Allocator* a) {
        MFunc* mf = a->mf;
        auto& entries = [&]() -> auto& {
            if constexpr (copiesOnly) {
                return a->copyIntervals;
            } else {
                return a->intervals;
            }
        }();
        entries.resize(usize(mf->nregs));
        entries.fill(nullptr);
        auto interval = [&](VReg r) {
            if constexpr (copiesOnly) {
                auto& entry = a->copyIntervals[usize(allocatorVindex(a, r))];
                if (!entry) {
                    entry = activePool->make<CopyInterval>();
                }
                return entry;
            } else {
                return allocatorInterval(a, r);
            }
        };
        const auto copyIndex = a->copyIndex.view();
        const auto copyRegs = a->copyRegs.view();
        auto accepts = [&](VReg r) {
            if (r == noReg) {
                return false;
            }
            if constexpr (copiesOnly) {
                return !vRegIsPhys(r) && copyIndex[allocatorVindex(a, r)] >= 0;
            }
            return !vRegIsPhys(r) || allocatable(r);
        };
        for (Int i = length(a->order) - 1; i >= 0; --i) {
            const i32 b = a->order[i];
            const i32 from = a->blockFrom[b];
            const i32 to = a->blockTo[b];
            bitsetEach(a->liveOut[b], [&](Int x) {
                const VReg r = copiesOnly ? copyRegs[x] : firstVirtual + VReg(x);
                intervalAddRange(interval(r), from, to);
            });
            auto instructions = mf->blocks[b].insts.view();
            for (Int j = length(instructions) - 1; j >= 0; --j) {
                MInst* in = &instructions[j];
                const i32 p = from + 2 + 2 * j;
                if constexpr (!copiesOnly) {
                    auto clobbers = mInstClobberSet(in, mf->registerClasses);
                    for (Int k = 0; k < length(clobbers); ++k) {
                        intervalAddRange(interval(clobbers[k]), p + 1, p + 2);
                    }
                }
                const bool needReg = in->op != mParMove;
                mInstForEachOperand(in, [&](VReg* r, Int role) {
                    if ((!(role & roleDef) && role != roleTemp) || !accepts(*r)) {
                        return;
                    }
                    auto* it = interval(*r);
                    if (role == roleTemp) {
                        intervalAddRange(it, p, p + 2);
                        if constexpr (!copiesOnly) {
                            intervalRecordUse(it, p + 1);
                            intervalRecordUse(it, p);
                        }
                    } else if (role & roleDef) {
                        if (!intervalCutAt(it, p + 1)) {
                            intervalAddRange(it, p + 1, p + 2);
                        }
                        if constexpr (!copiesOnly) {
                            if (needReg || vRegIsPhys(*r)) {
                                intervalRecordUse(it, p + 1);
                            }
                        }
                    }
                });
                mInstForEachOperand(in, [&](VReg* r, Int role) {
                    if (!(role & roleUse) || role == roleTemp || !accepts(*r)) {
                        return;
                    }
                    auto* it = interval(*r);
                    intervalAddRange(it, from, p + 1);
                    if constexpr (!copiesOnly) {
                        if (needReg || vRegIsPhys(*r)) {
                            intervalRecordUse(it, p);
                        }
                    }
                });
                if constexpr (!copiesOnly) {
                    if (in->op == mMov || in->op == mFMov) {
                        allocatorHint(a, in->dst, in->src);
                    }
                    auto moves = mInstMoves(in);
                    for (Int k = 0; k < length(moves); ++k) {
                        allocatorHint(a, moves[k].dst, moves[k].src);
                    }
                }
            }
        }
        entries.visit([](auto* it) {
            if (it) {
                intervalFinish(it);
            }
        });
        if constexpr (!copiesOnly) {
            for (Int r = 0; r < 32; ++r) {
                if (a->fixed[r]) {
                    intervalFinish(a->fixed[r]);
                }
            }
        }
    }

    // calleeSavedUsed lists the callee-saved registers the function writes.
    static Slice<VReg> allocatorCalleeSavedUsed(Allocator* a) {
        Slice<VReg> out = {};
        {
            auto range418 = calleeSaved;
            for (Int index419 = 0, rangeEnd420 = length(range418); index419 < rangeEnd420; ++index419) {
                VReg r = range418[index419];
                if ((a)->used[r]) {
                    out = appendValues(out, r);
                }
            }
        }
        return out;
    }

    // canEvict tells whether evict can take the register from it at pos.
    static bool allocatorCanEvict(Allocator* a, Interval* it, i32 pos) {
        i32 start = intervalStart(it);
        if ((start < pos)) {
            i32 at = {};
            if ((((pos & i32(1LL)) != i32(0LL)) && (intervalPrevUse(it, pos) == (pos - i32(1LL))))) {
                at = pos;
            } else {
                i32 lo = (intervalPrevUse(it, pos) + i32(1LL));
                if ((lo <= start)) {
                    lo = (start + i32(1LL));
                }
                at = allocatorSplitPos(a, lo, pos);
            }
            {
                if (((at >= i32(0LL)) && (at > start))) {
                    start = intervalCovered(it, at);
                } else if ((intervalPrevUse(it, pos) >= start)) {
                    return false;
                }
            switchEnd422:;
            }
        }
        i32 u = intervalNextUse(it, pos);
        if ((u == maxPos)) {
            return true;
        }
        i32 lo = pos;
        if ((lo <= start)) {
            lo = (start + i32(1LL));
        }
        i32 at = allocatorSplitPos(a, lo, u);
        return ((at >= i32(0LL)) && (at > start));
    }

    static Slice<VReg> allocatorCandidates(Allocator* a, u8 classValue) {
        if ((classValue == classXMM)) {
            return allocXMMs;
        }
        return allocGPRs;
    }

    static bool allocatorSelectCopies(Allocator* a) {
        MFunc* mf = a->mf;
        auto select = [&](VReg dst, VReg src) {
            if (dst == noReg || src == noReg || dst == src || vRegIsPhys(dst) || vRegIsPhys(src)) {
                return;
            }
            const usize x = usize(allocatorVindex(a, dst));
            const usize y = usize(allocatorVindex(a, src));
            if (mf->classValue[Int(x)] != mf->classValue[Int(y)]) {
                return;
            }
            if (!a->copyIndex.size()) {
                a->copyIndex.resize(usize(mf->nregs));
                a->copyIndex.fill(-1);
            }
            a->copyIndex[x] = 0;
            a->copyIndex[y] = 0;
        };
        for (Int b = 0; b < length(mf->blocks); ++b) {
            auto instructions = mf->blocks[b].insts.view();
            for (Int i = 0; i < length(instructions); ++i) {
                MInst* in = &instructions[i];
                if (in->op == mParMove) {
                    auto moves = mInstMoves(in);
                    for (Int j = 0; j < length(moves); ++j) {
                        select(moves[j].dst, moves[j].src);
                    }
                } else if (in->op == mMov || in->op == mFMov) {
                    select(in->dst, in->src);
                }
            }
        }
        if (!a->copyIndex.size()) {
            return false;
        }
        for (usize x = 0; x < a->copyIndex.size(); ++x) {
            if (a->copyIndex[x] >= 0) {
                a->copyIndex[x] = i32(a->copyRegs.size());
                a->copyRegs.pushBack(firstVirtual + VReg(x));
            }
        }
        return true;
    }

    // coalesce renames the operands of the copies whose registers it joins
    // and reports whether it joined any: the allocator then starts again on
    // the renamed code.
    static bool allocatorCoalesce(Allocator* a) {
        MFunc* mf = a->mf;
        // The projection map has no remaining users after range construction.
        Vec<i32> parent(move(a->copyIndex));
        InlineVec<Lrange, 1> scratch;
        parent.resize(usize(mf->nregs));
        for (Int x = 0; x < mf->nregs; ++x) {
            parent[usize(x)] = i32(x);
        }
        auto find = [&](i32 x) {
            while (parent[usize(x)] != x) {
                parent[usize(x)] = parent[usize(parent[usize(x)])];
                x = parent[usize(x)];
            }
            return x;
        };
        bool joined = false;
        auto join = [&](VReg dst, VReg src) {
            if (dst == noReg || src == noReg || dst == src || vRegIsPhys(dst) || vRegIsPhys(src)) {
                return;
            }
            const Int dx = allocatorVindex(a, dst);
            const Int sx = allocatorVindex(a, src);
            if (mf->classValue[dx] != mf->classValue[sx]) {
                return;
            }
            const i32 x = find(i32(dx));
            const i32 y = find(i32(sx));
            if (x == y) {
                return;
            }
            CopyInterval* left = a->copyIntervals[usize(x)];
            CopyInterval* right = a->copyIntervals[usize(y)];
            if (!left || !right || !left->ranges.size() || !right->ranges.size() || overlap(left->ranges.view(), right->ranges.view())) {
                return;
            }
            parent[usize(y)] = x;
            // Only root ranges are read from here. The whole allocator is
            // discarded after a join, so merged buffers can be reused now.
            mergeRanges(left->ranges, right->ranges, scratch);
            joined = true;
        };
        // Copies were stably sorted by their block's loop depth. Bucketing
        // blocks by that same integer key preserves the exact visitation
        // order without collecting and sorting a second list of copies.
        Int deepest = 0;
        for (Int b = 0; b < length(mf->blocks); ++b) {
            require(mf->blocks[b].depth >= 0, "negative loop depth");
            deepest = maximum(deepest, mf->blocks[b].depth);
        }
        Vec<i32> heads;
        Vec<i32> tails;
        Vec<i32> next;
        heads.resize(usize(deepest + 1));
        tails.resize(usize(deepest + 1));
        next.resize(mf->blocks.size());
        heads.visit([](i32& head) {
            head = -1;
        });
        for (Int b = 0; b < length(mf->blocks); ++b) {
            const usize depth = usize(mf->blocks[b].depth);
            next[usize(b)] = -1;
            if (heads[depth] < 0) {
                heads[depth] = i32(b);
            } else {
                next[usize(tails[depth])] = i32(b);
            }
            tails[depth] = i32(b);
        }
        for (Int depth = deepest; depth >= 0; --depth) {
            for (i32 b = heads[usize(depth)]; b >= 0; b = next[usize(b)]) {
                auto& instructions = mf->blocks[b].insts;
                for (usize i = 0; i < instructions.size(); ++i) {
                    MInst* in = &instructions[i];
                    if (in->op == mParMove) {
                        auto moves = mInstMoves(in);
                        for (Int j = 0; j < length(moves); ++j) {
                            join(moves[j].dst, moves[j].src);
                        }
                    } else if (in->op == mMov || in->op == mFMov) {
                        join(in->dst, in->src);
                    }
                }
            }
        }
        if ((!(joined))) {
            return false;
        }
        // Close the gaps left by joined registers while preserving the
        // order of the canonical representatives. Liveness and allocation
        // then visit the same values with smaller index tables and sets.
        Vec<i32> canonical;
        canonical.resize(usize(mf->nregs));
        i32 count = 0;
        for (i32 x = 0; x < mf->nregs; ++x) {
            if (find(x) == x) {
                canonical[usize(x)] = count;
                mf->classValue[count++] = mf->classValue[x];
            }
        }
        {
            auto& range441 = (mf)->blocks;
            for (Int index442 = 0, rangeEnd443 = length(range441); index442 < rangeEnd443; ++index442) {
                Int b = index442;
                {
                    auto range444 = ((mf)->blocks[b]).insts.view();
                    for (Int index445 = 0, rangeEnd446 = length(range444); index445 < rangeEnd446; ++index445) {
                        Int i = index445;
                        MInst* in = (&(((mf)->blocks[b]).insts[i]));
                        mInstForEachOperand(in, [&](VReg* r, Int role) mutable -> void {
                            if ((((*(r)) != noReg) && (!(vRegIsPhys(*(r)))))) {
                                *r = firstVirtual + canonical[usize(find(i32(allocatorVindex(a, *r))))];
                            }
                        });
                        {
                            if (((in)->op == mParMove)) {
                                mInstExtra(in)->moves.assign(dropSelfMoves(mInstMoves(in)));
                            } else if (((((((in)->op == mMov) || ((in)->op == mFMov))) && ((in)->dst == (in)->src)) && ((in)->aux == u8(0LL)))) {
                                (*(in)) = inst(mNop);
                            }
                        switchEnd448:;
                        }
                    }
                }
            }
        }
        mf->nregs = count;
        mf->classValue = subslice(mf->classValue, 0, count);
        return true;
    }

    // evict takes the register from it at pos, which it may cover: the part
    // from there is spilled until its next use, which goes back to the
    // unhandled list.
    static void allocatorEvict(Allocator* a, Interval* it, i32 pos) {
        Interval* tail = it;
        if ((intervalStart(it) < pos)) {
            i32 at = {};
            if ((((pos & i32(1LL)) != i32(0LL)) && (intervalPrevUse(it, pos) == (pos - i32(1LL))))) {
                at = pos;
            } else {
                i32 lo = (intervalPrevUse(it, pos) + i32(1LL));
                if ((lo <= intervalStart(it))) {
                    lo = (intervalStart(it) + i32(1LL));
                }
                at = allocatorSplitPos(a, lo, pos);
            }
            {
                if (((at >= i32(0LL)) && (at > intervalStart(it)))) {
                    tail = intervalSplit(it, at);
                } else if ((intervalPrevUse(it, pos) < intervalStart(it))) {
                } else {
                    compilerError(String("internal error: cannot evict %s at %d"));
                }
            switchEnd450:;
            }
        }
        allocatorSpill(a, tail);
        i32 u = intervalNextUse(tail, pos);
        if ((u == maxPos)) {
            return;
        }
        i32 lo = pos;
        if ((lo <= intervalStart(tail))) {
            lo = (intervalStart(tail) + i32(1LL));
        }
        i32 at = allocatorSplitPos(a, lo, u);
        if (((at < i32(0LL)) || (at <= intervalStart(tail)))) {
            compilerError(String("internal error: cannot reload %s before %d"));
        }
        allocatorAddUnhandled(a, intervalSplit(tail, at));
    }

    static void allocatorHint(Allocator* a, VReg dst, VReg src) {
        if ((vRegIsPhys(dst) && vRegIsPhys(src))) {
            return;
        }
        if ((!(vRegIsPhys(dst)))) {
            Interval* it = allocatorInterval(a, dst);
            if (((it)->hint == noReg)) {
                (it)->hint = src;
            }
        }
        if ((!(vRegIsPhys(src)))) {
            Interval* it = allocatorInterval(a, src);
            if (((it)->hint == noReg)) {
                (it)->hint = dst;
            }
        }
    }

    // hintReg returns the register the hint holds at the start of cur.
    static VReg allocatorHintReg(Allocator* a, Interval* cur) {
        VReg h = (cur)->hint;
        if ((h == noReg)) {
            return noReg;
        }
        if (vRegIsPhys(h)) {
            return h;
        }
        Interval* it = (a)->intervals[allocatorVindex(a, h)];
        if (isNil(it)) {
            return noReg;
        }
        i32 p = intervalStart(cur);
        {
            for (Int i = -1; i < length(it->children); ++i) {
                Interval* c = i < 0 ? it : it->children[i];
                if ((((c)->reg != noReg) && ((((intervalCovers(c, (p - i32(1LL))) || intervalCovers(c, p)) || (intervalEnd(c) == p)) || (intervalEnd(c) == (p - i32(1LL))))))) {
                    return (c)->reg;
                }
            }
        }
        return noReg;
    }

    static __attribute__((noinline)) Interval* newInterval(VReg vreg, VReg reg, u8 classValue = 0, VReg hint = 0) {
        Interval* it = activePool->make<Interval>();
        it->vreg = vreg;
        it->classValue = classValue;
        it->reg = reg;
        it->hint = hint;
        return it;
    }

    static Interval* allocatorInterval(Allocator* a, VReg r) {
        if (vRegIsPhys(r)) {
            auto& entry = a->fixed[r];
            if (!entry) {
                entry = newInterval(r, r);
            }
            return entry;
        }
        const Int x = allocatorVindex(a, r);
        auto& entry = a->intervals[x];
        if (!entry) {
            entry = newInterval(r, noReg, a->mf->classValue[x], noReg);
        }
        return entry;
    }

    // linearize orders blocks in reverse postorder of the machine CFG and
    // numbers instruction positions.
    static void allocatorLinearize(Allocator* a) {
        MFunc* mf = a->mf;
        const usize n = mf->blocks.size();
        Vec<u8> visited;
        visited.resize(n);
        visited.fill(0);
        Vec<FrameRegalloc324> stack;
        stack.pushBack({0, 0});
        visited[0] = 1;
        a->order.reserve(n);
        while (stack.size()) {
            auto* top = &stack[stack.size() - 1];
            auto succs = mf->blocks[top->b].succs;
            if (top->i < length(succs)) {
                const i32 successor = succs[length(succs) - 1 - top->i++];
                if (!visited[usize(successor)]) {
                    visited[usize(successor)] = 1;
                    stack.pushBack({successor, 0});
                }
                continue;
            }
            a->order.pushBack(top->b);
            stack.popBack();
        }
        const usize count = a->order.size();
        for (usize i = 0; i < count / 2; ++i) {
            const i32 block = a->order[i];
            a->order[i] = a->order[count - 1 - i];
            a->order[count - 1 - i] = block;
        }
        a->blockFrom.resize(n);
        a->blockFrom.fill(0);
        a->blockTo.resize(n);
        a->blockTo.fill(0);
        i32 pos = 0;
        for (usize i = 0; i < count; ++i) {
            const i32 block = a->order[i];
            a->blockFrom[usize(block)] = pos;
            pos += 2 + 2 * i32(mf->blocks[block].insts.size());
            a->blockTo[usize(block)] = pos;
        }
        a->posBlock.resize(usize(pos / 2 + 1));
        a->posBlock[a->posBlock.size() - 1] = 0;
        for (usize i = 0; i < count; ++i) {
            const i32 block = a->order[i];
            for (i32 p = a->blockFrom[usize(block)]; p < a->blockTo[usize(block)]; p += 2) {
                a->posBlock[usize(p / 2)] = block;
            }
        }
    }

    static void allocatorLiveness(Allocator* a, bool copiesOnly) {
        MFunc* mf = a->mf;
        const Int n = length(mf->blocks);
        const Int nv = copiesOnly ? Int(a->copyRegs.size()) : mf->nregs;
        const auto copyIndex = a->copyIndex.view();
        // Gen/kill sets and operand callbacks have no users after dataflow.
        ObjPool temporary;
        Vec<Bitset> kill;
        kill.resize(usize(n));
        kill.fill(Bitset{});
        a->liveIn.resize(usize(n));
        a->liveIn.fill(Bitset{});
        a->liveOut.resize(usize(n));
        a->liveOut.fill(Bitset{});
        for (Int index = 0; index < length(a->order); ++index) {
            const i32 b = a->order[index];
            a->liveIn[b] = newBitset(nv);
            {
                PoolScope scope(temporary);
                Bitset g = a->liveIn[b];
                Bitset k = newBitset(nv);
                for (Int i = 0; i < length(mf->blocks[b].insts); ++i) {
                    mInstForEachOperand(&mf->blocks[b].insts[i], [&](VReg* r, Int role) {
                        if (vRegIsPhys(*r) || *r == noReg) {
                            return;
                        }
                        Int x = allocatorVindex(a, *r);
                        if (copiesOnly) {
                            x = copyIndex[x];
                            if (x < 0) {
                                return;
                            }
                        }
                        if ((role & roleUse) != 0 && role != roleTemp) {
                            // A local use cannot add gen, and a use-def's
                            // definition is already present in kill.
                            if (bitsetHas(k, x)) {
                                return;
                            }
                            bitsetSet(g, x);
                        }
                        if ((role & roleDef) != 0 || role == roleTemp) {
                            bitsetSet(k, x);
                        }
                    });
                }
                kill[b] = k;
            }
            a->liveOut[b] = newBitset(nv);
        }
        // A block only needs another visit after a successor's live-in set
        // changes. States 0/1/2 mean unreachable / idle / queued.
        Vec<i32> work;
        Vec<u8> state;
        work.resize(usize(n));
        state.resize(usize(n));
        state.fill(0);
        usize pending = usize(length(a->order));
        usize head = 0;
        usize tail = pending == usize(n) ? 0 : pending;
        for (usize i = 0; i < pending; ++i) {
            const i32 b = a->order[Int(pending - 1 - i)];
            work[i] = b;
            state[usize(b)] = 2;
        }
        while (pending) {
            const i32 b = work[head];
            if (++head == usize(n)) {
                head = 0;
            }
            --pending;
            state[usize(b)] = 1;
            Bitset out = a->liveOut[b];
            for (Int j = 0; j < length(mf->blocks[b].succs); ++j) {
                bitsetUnion(out, a->liveIn[mf->blocks[b].succs[j]]);
            }
            if (bitsetPropagate(a->liveIn[b], {}, out, kill[b])) {
                auto preds = mf->blocks[b].preds;
                for (Int j = 0; j < length(preds); ++j) {
                    const i32 pred = preds[j];
                    if (state[usize(pred)] == 1) {
                        work[tail] = pred;
                        if (++tail == usize(n)) {
                            tail = 0;
                        }
                        ++pending;
                        state[usize(pred)] = 2;
                    }
                }
            }
        }
    }

    static VReg allocatorLoc(Allocator* a, Interval* it) {
        if (((it)->reg != noReg)) {
            return (it)->reg;
        }
        return slotLoc((a)->slots[allocatorVindex(a, (it)->vreg)]);
    }

    // location of vreg r at position p: a register, or a stack slot encoded as
    // a negative value below noReg.
    static VReg allocatorLocation(Allocator* a, VReg r, i32 p) {
        Interval* root = a->intervals[allocatorVindex(a, r)];
        if (p >= root->locationFrom && p < root->locationTo) {
            return root->locationReg;
        }
        Interval* part = root;
        if (!intervalCovers(part, p)) {
            // Splitting cuts an interval's suffix. Children therefore have
            // disjoint extents and are sorted by start, including children
            // made by splitting a previous child.
            const Int end = sortSearch(length(root->children), [&](Int i) {
                return intervalStart(root->children[i]) > p;
            });
            if (!end || !intervalCovers(root->children[end - 1], p)) {
                compilerError(String("internal error: v%d not live at %d"));
            }
            part = root->children[end - 1];
        }
        const Int range = sortSearch(length(part->ranges), [&](Int i) {
            return part->ranges[i].to > p;
        });
        root->locationFrom = part->ranges[range].from;
        root->locationTo = part->ranges[range].to;
        root->locationReg = allocatorLoc(a, part);
        return root->locationReg;
    }

    static u8 allocatorRegClass(Allocator* a, VReg r) {
        if (vRegIsXMM(r)) {
            return classXMM;
        }
        return classGPR;
    }

    // resolve rewrites operands to registers and inserts the moves for split
    // intervals inside blocks and on edges.
    static void allocatorResolve(Allocator* a) {
        MFunc* mf = (a)->mf;
        PositionedMoves inner;
        Bitset split;
        {
            auto range478 = (a)->intervals.view();
            for (Int index479 = 0, rangeEnd480 = length(range478); index479 < rangeEnd480; ++index479) {
                Int x = index479;
                Interval* it = range478[index479];
                if (isNil(it)) {
                    continue;
                }
                if (length(it->children)) {
                    if (!split.data) {
                        split = newBitset(mf->nregs);
                    }
                    bitsetSet(split, x);
                }
                {
                    Int i = Int(0LL);
                    for (; (i < length(it->children)); i++) {
                        auto temporary481 = i ? it->children[i - 1] : it;
                        auto temporary482 = it->children[i];
                        Interval* prev = temporary481;
                        Interval* c = temporary482;
                        i32 p = intervalStart(c);
                        if ((intervalEnd(prev) != p)) {
                            continue;
                        }
                        {
                            i32 b = (a)->posBlock[(p / i32(2LL))];
                            if (((a)->blockFrom[b] == p)) {
                                continue;
                            }
                        }
                        auto temporary483 = allocatorLoc(a, prev);
                        auto temporary484 = allocatorLoc(a, c);
                        VReg src = temporary483;
                        VReg dst = temporary484;
                        i32 key = (i32(2LL) * p);
                        if (((p & i32(1LL)) != i32(0LL))) {
                            if ((!(isSlot(dst)))) {
                                compilerError(String("internal error: %s split at odd position %d into a register"));
                            }
                            key = ((i32(2LL) * ((p - i32(1LL)))) + i32(1LL));
                        }
                        if ((src != dst)) {
                            appendMove(inner, key, Move{dst, src});
                        }
                        (void)(x);
                    }
                }
            }
        }
        Map<i32, Slice<Move>> atEnd = mapOf<i32, Slice<Move>>({});
        Map<i32, Slice<Move>> atStart = mapOf<i32, Slice<Move>>({});
        // An unsplit interval has one location throughout the CFG. Only
        // split registers can need a move between predecessor and successor.
        if (split.data) {
            auto range485 = (a)->order.view();
            for (Int index486 = 0, rangeEnd487 = length(range485); index486 < rangeEnd487; ++index486) {
                i32 b = range485[index486];
                MBlock* blk = (&((mf)->blocks[b]));
                {
                    auto range488 = (blk)->succs;
                    for (Int index489 = 0, rangeEnd490 = length(range488); index489 < rangeEnd490; ++index489) {
                        i32 s = range488[index489];
                        Slice<Move> moves = {};
                        bitsetEachIntersection((a)->liveIn[s], split, [&](Int x) mutable -> void {
                            VReg r = (convert<VReg>(x) + firstVirtual);
                            VReg src = allocatorLocation(a, r, ((a)->blockTo[b] - i32(1LL)));
                            VReg dst = allocatorLocation(a, r, (a)->blockFrom[s]);
                            if ((src != dst)) {
                                moves = appendValues(moves, Move{dst, src});
                            }
                        });
                        if ((length(moves) == Int(0LL))) {
                            continue;
                        }
                        if ((length((blk)->succs) == Int(1LL))) {
                            setMap(atEnd, b, appendSlice(atEnd[b], moves));
                        } else if ((length(((mf)->blocks[s]).preds) == Int(1LL))) {
                            setMap(atStart, s, appendSlice(atStart[s], moves));
                        } else {
                            compilerError(String("internal error: critical edge B%d->B%d needs moves"));
                        }
                    }
                }
            }
        }
        allocatorSaveSpillsBeforeLoops(a, inner, atEnd, atStart);
        if (inner.entries.size() > 1) {
            sortValues(inner.entries.view(), [&](Int i, Int j) {
                const auto& left = inner.entries[usize(i)];
                const auto& right = inner.entries[usize(j)];
                return left.position < right.position || (left.position == right.position && left.order < right.order);
            });
        }
        usize nextMove = 0;
        for (usize index = 0; index < a->order.size(); ++index) {
            const i32 b = a->order[index];
            MBlock* blk = &mf->blocks[b];
            const usize count = blk->insts.size();
            const auto instructions = blk->insts.view();
            Vec<MInst> out;
            bool rebuilding = false;
            // Keep the original storage until an inserted instruction needs
            // space. Earlier instructions have already been rewritten.
            auto prepareOutput = [&](usize prefix) {
                if (!rebuilding) {
                    out.reserve(count);
                    out.append(subslice(instructions, 0, Int(prefix)));
                    rebuilding = true;
                }
            };
            auto emitMoves = [&](Slice<Move> moves, usize prefix) {
                if (length(moves)) {
                    prepareOutput(prefix);
                    out.pushBack(parMove(moves));
                }
            };
            const auto endMoves = atEnd[b];
            emitMoves(atStart[b], 0);
            const Int branches = length(endMoves) ? firstBranch(blk->insts) : Int(count);
            const i32 firstKey = 2 * (a->blockFrom[b] + 2);
            while (nextMove < inner.entries.size() && inner.entries[nextMove].position < firstKey) {
                ++nextMove;
            }
            const bool hasInner = count && nextMove < inner.entries.size() && inner.entries[nextMove].position < 2 * (a->blockTo[b] - 1);
            for (usize i = 0; i < count; ++i) {
                const i32 p = a->blockFrom[b] + 2 + 2 * i32(i);
                if (Int(i) == branches) {
                    emitMoves(endMoves, i);
                }
                if (hasInner) {
                    for (i32 half = 0; half < 2; ++half) {
                        const i32 key = 2 * p + half;
                        while (nextMove < inner.entries.size() && inner.entries[nextMove].position < key) {
                            ++nextMove;
                        }
                        if (nextMove < inner.entries.size() && inner.entries[nextMove].position == key) {
                            prepareOutput(i);
                            MInst moves = inst(mParMove);
                            auto& list = mInstExtra(&moves)->moves;
                            do {
                                list.pushBack(inner.entries[nextMove++].move);
                            } while (nextMove < inner.entries.size() && inner.entries[nextMove].position == key);
                            out.pushBack(moves);
                        }
                    }
                }
                MInst* in = &instructions[Int(i)];
                mInstForEachOperand(in, [&](VReg* r, Int role) {
                    if (*r == noReg || vRegIsPhys(*r)) {
                        return;
                    }
                    const i32 at = role == roleDef ? p + 1 : p;
                    *r = allocatorLocation(a, *r, at);
                    if (isSlot(*r) && in->op != mParMove) {
                        compilerError(String("internal error: operand of %s in a stack slot"));
                    }
                });
                if (in->op == mParMove) {
                    mInstExtra(in)->moves.resize(usize(length(dropSelfMoves(mInstMoves(in)))));
                }
                if (rebuilding) {
                    out.pushBack(*in);
                }
            }
            if (branches == Int(count)) {
                emitMoves(endMoves, count);
            }
            if (rebuilding) {
                blk->insts = move(out);
            }
        }
    }

    // A spill slot belongs to one virtual register. If that register has one
    // definition outside a loop, saving it there makes every later save of the
    // same value redundant, including saves repeated on each loop iteration.
    // Coalesced loop parameters have several definitions and are left alone.
    static void allocatorSaveSpillsBeforeLoops(Allocator* a, PositionedMoves& inner, Map<i32, Slice<Move>> atEnd, Map<i32, Slice<Move>> atStart) {
        if (!a->nslots || (!inner.entries.size() && !length(atEnd) && !length(atStart))) {
            return;
        }
        bool hasLoop = false;
        for (Int i = 0; i < length(a->mf->blocks); ++i) {
            if (a->mf->blocks[usize(i)].depth > 0) {
                hasLoop = true;
                break;
            }
        }
        if (!hasLoop) {
            return;
        }
        Slice<SpillDef> defs = makeSlice<SpillDef>(((a)->mf)->nregs);
        {
            auto range509 = (a)->order.view();
            for (Int index510 = 0, rangeEnd511 = length(range509); index510 < rangeEnd511; ++index510) {
                i32 b = range509[index510];
                {
                    auto range512 = (((a)->mf)->blocks[b]).insts.view();
                    for (Int index513 = 0, rangeEnd514 = length(range512); index513 < rangeEnd514; ++index513) {
                        Int i = index513;
                        MInst* in = (&((((a)->mf)->blocks[b]).insts[i]));
                        mInstForEachOperand(in, [&](VReg* r, Int role) mutable -> void {
                            if ((((((*(r)) == noReg) || vRegIsPhys(*(r))) || ((role & roleDef) == Int(0LL))) || (role == roleTemp))) {
                                return;
                            }
                            SpillDef* d = (&(defs[allocatorVindex(a, (*(r)))]));
                            (d)->count++;
                            (d)->block = b;
                            (d)->pos = a->blockFrom[b] + 2 + 2 * i;
                        });
                    }
                }
            }
        }
        Map<VReg, Int> owners = makeMap<VReg, Int>();
        {
            auto range515 = (a)->slots.view();
            for (Int index516 = 0, rangeEnd517 = length(range515); index516 < rangeEnd517; ++index516) {
                Int x = index516;
                i32 slot = range515[index516];
                if (((slot >= i32(0LL)) && ((defs[x]).count == Int(1LL)))) {
                    setMap(owners, slotLoc(slot), x);
                }
            }
        }
        if (!length(owners)) {
            return;
        }
        Slice<bool> save = makeSlice<bool>(length(defs));
        auto consider = [&](Slice<Move> moves, i32 b) mutable -> void {
            {
                auto range518 = moves;
                for (Int index519 = 0, rangeEnd520 = length(range518); index519 < rangeEnd520; ++index519) {
                    Move m = range518[index519];
                    {
                        auto temporary521 = lookupMap(owners, (m).dst);
                        Int x = temporary521.v0;
                        bool ok = temporary521.v1;
                        if ((ok && ((((a)->mf)->blocks[b]).depth > (((a)->mf)->blocks[(defs[x]).block]).depth))) {
                            save[x] = true;
                        }
                    }
                }
            }
        };
        for (usize i = 0; i < inner.entries.size(); ++i) {
            const auto& entry = inner.entries[i];
            auto owner = lookupMap(owners, entry.move.dst);
            if (owner.v1 && a->mf->blocks[a->posBlock[entry.position / 4]].depth > a->mf->blocks[defs[owner.v0].block].depth) {
                save[owner.v0] = true;
            }
        }
        {
            auto range525 = atEnd;
            for (Int index526 = 0; index526 < mapSlots(range525); ++index526) {
                if (!mapOccupied(range525, index526)) {
                    continue;
                }
                i32 b = mapKey(range525, index526);
                Slice<Move> moves = mapValue(range525, index526);
                consider(moves, b);
            }
        }
        {
            auto range528 = atStart;
            for (Int index529 = 0; index529 < mapSlots(range528); ++index529) {
                if (!mapOccupied(range528, index529)) {
                    continue;
                }
                i32 b = mapKey(range528, index529);
                Slice<Move> moves = mapValue(range528, index529);
                consider(moves, b);
            }
        }
        auto filter = [&](Slice<Move> moves) mutable -> Slice<Move> {
            Slice<Move> out = subslice(moves, Int(0), Int(0LL));
            {
                auto range531 = moves;
                for (Int index532 = 0, rangeEnd533 = length(range531); index532 < rangeEnd533; ++index532) {
                    Move m = range531[index532];
                    {
                        auto temporary534 = lookupMap(owners, (m).dst);
                        Int x = temporary534.v0;
                        bool ok = temporary534.v1;
                        if ((ok && save[x])) {
                            continue;
                        }
                    }
                    out = appendValues(out, m);
                }
            }
            return out;
        };
        usize kept = 0;
        for (usize i = 0; i < inner.entries.size(); ++i) {
            const auto entry = inner.entries[i];
            auto owner = lookupMap(owners, entry.move.dst);
            if (!owner.v1 || !save[owner.v0]) {
                inner.entries[kept++] = entry;
            }
        }
        inner.entries.resize(kept);
        {
            auto range538 = atEnd;
            for (Int index539 = 0; index539 < mapSlots(range538); ++index539) {
                if (!mapOccupied(range538, index539)) {
                    continue;
                }
                i32 b = mapKey(range538, index539);
                Slice<Move> moves = mapValue(range538, index539);
                setMap(atEnd, b, filter(moves));
            }
        }
        {
            auto range541 = atStart;
            for (Int index542 = 0; index542 < mapSlots(range541); ++index542) {
                if (!mapOccupied(range541, index542)) {
                    continue;
                }
                i32 b = mapKey(range541, index542);
                Slice<Move> moves = mapValue(range541, index542);
                setMap(atStart, b, filter(moves));
            }
        }
        {
            auto range544 = save;
            for (Int index545 = 0, rangeEnd546 = length(range544); index545 < rangeEnd546; ++index545) {
                Int x = index545;
                bool yes = range544[index545];
                if ((!(yes))) {
                    continue;
                }
                SpillDef d = defs[x];
                VReg r = (convert<VReg>(x) + firstVirtual);
                VReg src = allocatorLocation(a, r, ((d).pos + i32(1LL)));
                VReg dst = slotLoc((a)->slots[x]);
                if ((src != dst)) {
                    i32 key = (i32(2LL) * (((d).pos + i32(2LL))));
                    appendMove(inner, key, Move{dst, src});
                }
            }
        }
    }

    static void allocatorScan(Allocator* a) {
        {
            auto range547 = (a)->intervals.view();
            for (Int index548 = 0, rangeEnd549 = length(range547); index548 < rangeEnd549; ++index548) {
                Interval* it = range547[index548];
                if (((!isNil(it)) && (length((it)->ranges) > Int(0LL)))) {
                    a->unhandled.initial.pushBack({it, intervalStart(it), 0});
                }
            }
        }
        sortValues(Span<IntervalQueue::Entry>(a->unhandled.initial), [&](Int i, Int j) {
            return a->unhandled.initial[usize(i)].start < a->unhandled.initial[usize(j)].start;
        });
        // Preserve the previous sort's ties. Future insertions used an upper
        // bound, so they follow all existing entries with the same start.
        for (usize i = 0; i < a->unhandled.initial.size(); ++i) {
            a->unhandled.initial[i].order = u32(i);
        }
        a->unhandled.nextOrder = u32(a->unhandled.initial.size());
        {
            auto range550 = (a)->fixed;
            for (Int index551 = 0, rangeEnd552 = length(range550); index551 < rangeEnd552; ++index551) {
                Int r = index551;
                if (((!isNil((a)->fixed[r])) && (length(((a)->fixed[r])->ranges) > Int(0LL)))) {
                    a->inactive.pushBack(a->fixed[r]);
                }
            }
        }
        a->slots.resize(usize(a->mf->nregs));
        a->slots.fill(-1);
        // Only range boundaries change active/inactive membership. Eviction
        // removes active intervals; shortening an inactive prefix can only
        // leave an inert, ended entry until the next advance.
        i32 nextChange = -1;
        {
            for (; !a->unhandled.empty();) {
                Interval* cur = a->unhandled.pop();
                i32 pos = intervalStart(cur);
                if (pos >= nextChange) {
                    nextChange = allocatorAdvance(a, pos);
                }
                if ((!(allocatorTryFree(a, cur)))) {
                    allocatorAllocateBlocked(a, cur);
                }
                if (((cur)->reg != noReg)) {
                    a->active.pushBack(cur);
                    nextChange = minimum(nextChange, cur->ranges[0].to);
                }
            }
        }
    }

    static void allocatorSpill(Allocator* a, Interval* it) {
        (it)->reg = noReg;
        Int x = allocatorVindex(a, (it)->vreg);
        if (((a)->slots[x] < i32(0LL))) {
            (a)->slots[x] = (a)->nslots;
            (a)->nslots++;
        }
    }

    // splitPos chooses an instruction boundary between min and max, both
    // included: the start of the block with the lowest loop depth in that
    // range, else the latest boundary. It returns -1 when there is none.
    static i32 allocatorSplitPos(Allocator* a, i32 min, i32 max) {
        max &= ~(i32(1LL));
        if (((min & i32(1LL)) != i32(0LL))) {
            min++;
        }
        if ((min > max)) {
            return i32(-1LL);
        }
        i32 best = max;
        Int bestDepth = (((a)->mf)->blocks[(a)->posBlock[(max / i32(2LL))]]).depth;
        {
            i32 p = max;
            for (; (p > min);) {
                i32 b = (a)->posBlock[(((p - i32(1LL))) / i32(2LL))];
                i32 from = (a)->blockFrom[b];
                if ((from < min)) {
                    break;
                }
                {
                    Int d = (((a)->mf)->blocks[b]).depth;
                    if ((d < bestDepth)) {
                        best = from;
                        bestDepth = d;
                    }
                }
                p = from;
            }
        }
        return best;
    }

    static bool allocatorTryFree(Allocator* a, Interval* cur) {
        Array<i32, 32> freeUntil = {};
        {
            auto range556 = allocatorCandidates(a, (cur)->classValue);
            for (Int index557 = 0, rangeEnd558 = length(range556); index557 < rangeEnd558; ++index557) {
                VReg r = range556[index557];
                freeUntil[r] = maxPos;
            }
        }
        {
            auto range559 = (a)->active.view();
            for (Int index560 = 0, rangeEnd561 = length(range559); index560 < rangeEnd561; ++index560) {
                Interval* it = range559[index560];
                freeUntil[(it)->reg] = i32(0LL);
            }
        }
        {
            auto range562 = (a)->inactive.view();
            for (Int index563 = 0, rangeEnd564 = length(range562); index563 < rangeEnd564; ++index563) {
                Interval* it = range562[index563];
                if ((freeUntil[(it)->reg] == i32(0LL))) {
                    continue;
                }
                {
                    i32 x = intervalIntersection(it, cur);
                    if ((x < freeUntil[(it)->reg])) {
                        freeUntil[(it)->reg] = x;
                    }
                }
            }
        }
        auto temporary565 = intervalStart(cur);
        auto temporary566 = intervalEnd(cur);
        i32 start = temporary565;
        i32 end = temporary566;
        {
            auto range567 = allocatorCandidates(a, (cur)->classValue);
            for (Int index568 = 0, rangeEnd569 = length(range567); index568 < rangeEnd569; ++index568) {
                VReg r = range567[index568];
                if ((freeUntil[r] < end)) {
                    freeUntil[r] &= ~(i32(1LL));
                    if ((freeUntil[r] <= start)) {
                        freeUntil[r] = i32(0LL);
                    }
                }
            }
        }
        VReg reg = noReg;
        {
            VReg h = allocatorHintReg(a, cur);
            if (((((h != noReg) && allocatable(h)) && (allocatorRegClass(a, h) == (cur)->classValue)) && (freeUntil[h] >= end))) {
                reg = h;
            }
        }
        if ((reg == noReg)) {
            VReg best = noReg;
            {
                auto range570 = allocatorCandidates(a, (cur)->classValue);
                for (Int index571 = 0, rangeEnd572 = length(range570); index571 < rangeEnd572; ++index571) {
                    VReg r = range570[index571];
                    if ((freeUntil[r] >= end)) {
                        reg = r;
                        break;
                    }
                    if (((best == noReg) || (freeUntil[r] > freeUntil[best]))) {
                        best = r;
                    }
                }
            }
            if ((reg == noReg)) {
                reg = best;
            }
        }
        if ((freeUntil[reg] == i32(0LL))) {
            return false;
        }
        (cur)->reg = reg;
        (a)->used[reg] = true;
        if ((freeUntil[reg] < end)) {
            i32 at = allocatorSplitPos(a, (start + i32(1LL)), freeUntil[reg]);
            allocatorAddUnhandled(a, intervalSplit(cur, at));
        }
        return true;
    }

    static Int allocatorVindex(Allocator* a, VReg r) {
        return convert<i64>((r - firstVirtual));
    }

    static void asmAluMI(Asm* a, AluOp op, u8 size, Amem m, i64 x) {
        if ((x == convert<i64>(convert<i8>(x)))) {
            asmOpRM(a, (size == u8(8LL)), Span<const u8>({u8(131LL)}), convert<i64>(op), m, Int(1LL), false);
            asmBytes(a, {u8(convert<u8>(x))});
            return;
        }
        asmOpRM(a, (size == u8(8LL)), Span<const u8>({u8(129LL)}), convert<i64>(op), m, Int(4LL), false);
        asmU32(a, convert<u32>(x));
    }

    static void asmAluRI(Asm* a, AluOp op, u8 size, VReg dst, i64 x) {
        asmAluRIRaw(a, op, size, dst, x);
        if ((dst == RSP)) {
            {
                auto switch573 = op;
                switch (switch573) {
                    case aluSub: {
                        asmMoveSP(a, convert<i32>(x));
                        break;
                    }
                    case aluAdd: {
                        asmMoveSP(a, (-(convert<i32>(x))));
                        break;
                    }
                    case aluCmp: {
                        break;
                    }
                    default: {
                        compilerError(String("internal error: untracked change of rsp"));
                        break;
                    }
                }
            }
        }
    }

    static void asmAluRIRaw(Asm* a, AluOp op, u8 size, VReg dst, i64 x) {
        Int d = convert<i64>(dst);
        if ((x == convert<i64>(convert<i8>(x)))) {
            asmRex(a, (size == u8(8LL)), Int(-1LL), Int(-1LL), d, false);
            asmBytes(a, {u8(u8(131LL))});
            asmModrmReg(a, convert<i64>(op), d);
            asmBytes(a, {u8(convert<u8>(x))});
            return;
        }
        asmRex(a, (size == u8(8LL)), Int(-1LL), Int(-1LL), d, false);
        asmBytes(a, {u8(u8(129LL))});
        asmModrmReg(a, convert<i64>(op), d);
        asmU32(a, convert<u32>(x));
    }

    static void asmAluRM(Asm* a, AluOp op, u8 size, VReg dst, Amem m) {
        if ((op == aluImul)) {
            asmOpRM(a, (size == u8(8LL)), Span<const u8>({u8(15LL), u8(175LL)}), convert<i64>(dst), m, Int(0LL), false);
            return;
        }
        asmOpRM(a, (size == u8(8LL)), Span<const u8>({u8(shiftLeft(convert<u8>(op), Int(3LL)) | u8(3LL))}), convert<i64>(dst), m, Int(0LL), false);
    }

    // aluRR emits "op dst, src" for add, or, adc, sbb, and, sub, xor, cmp.
    static void asmAluRR(Asm* a, AluOp op, u8 size, VReg dst, VReg src) {
        if ((op == aluImul)) {
            asmOpRR(a, (size == u8(8LL)), Span<const u8>({u8(15LL), u8(175LL)}), convert<i64>(dst), convert<i64>(src), false);
            return;
        }
        asmOpRR(a, (size == u8(8LL)), Span<const u8>({u8(shiftLeft(convert<u8>(op), Int(3LL)) | u8(3LL))}), convert<i64>(dst), convert<i64>(src), false);
    }

    static void asmB(Asm* a, Span<const u8> bytes) {
        appendSpan((a)->buf, Span<const u8>(bytes));
    }

    static void asmBitCount(Asm* a, u8 kind, u8 size, VReg dst, VReg src) {
        static constexpr u8 opcodes[] = {189, 188, 184};
        require(kind < 3, "bit count opcode");
        asmPrefixed(a, 243, size == 8, Span<const u8>({15, opcodes[kind]}), dst, src);
    }

    // callMem calls the address stored at m.
    static void asmCallMem(Asm* a, Amem m) {
        asmOpRM(a, false, Span<const u8>({u8(255LL)}), Int(2LL), m, Int(0LL), false);
    }

    static void asmCallReg(Asm* a, VReg r) {
        asmRex(a, false, Int(-1LL), Int(-1LL), convert<i64>(r), false);
        asmBytes(a, {u8(u8(255LL))});
        asmModrmReg(a, Int(2LL), convert<i64>(r));
    }

    static i32 asmCallRel(Asm* a) {
        asmBytes(a, {u8(u8(232LL))});
        asmU32(a, u32(0LL));
        return (asmPos(a) - i32(4LL));
    }

    static void asmCmov(Asm* a, u8 cc, u8 size, VReg dst, VReg src) {
        asmOpRR(a, (size == u8(8LL)), Span<const u8>({u8(15LL), u8(u8(64LL) + cc)}), convert<i64>(dst), convert<i64>(src), false);
    }

    static void asmCvtFF(Asm* a, u8 from, VReg dst, VReg src) {
        Int d = regNum(dst);
        asmVexRR(a, scalarPP(from), u8(1LL), false, u8(90LL), d, Int(15LL), regNum(src));
    }

    // cvtFI truncates a float of fsize bytes to a signed integer of isize bytes.
    static void asmCvtFI(Asm* a, u8 isize, u8 fsize, VReg dst, VReg src) {
        asmVexRR(a, scalarPP(fsize), u8(1LL), (isize == u8(8LL)), u8(44LL), convert<i64>(dst), Int(-1LL), regNum(src));
    }

    // cvtIF converts a signed integer of isize bytes to a float of fsize bytes.
    static void asmCvtIF(Asm* a, u8 isize, u8 fsize, VReg dst, VReg src) {
        Int d = regNum(dst);
        asmVexRR(a, scalarPP(fsize), u8(1LL), (isize == u8(8LL)), u8(42LL), d, Int(15LL), convert<i64>(src));
    }

    static void asmDiv(Asm* a, bool signedValue, u8 size, VReg src) {
        Int s = convert<i64>(src);
        asmRex(a, (size == u8(8LL)), Int(-1LL), Int(-1LL), s, false);
        asmBytes(a, {u8(u8(247LL))});
        if (signedValue) {
            asmModrmReg(a, Int(7LL), s);
        } else {
            asmModrmReg(a, Int(6LL), s);
        }
    }

    static void asmExt(Asm* a, u8 width, u8 dsize, bool signedValue, VReg dst, VReg src) {
        auto temporary574 = convert<i64>(dst);
        auto temporary575 = convert<i64>(src);
        Int d = temporary574;
        Int s = temporary575;
        {
            if (((width == u8(4LL)) && signedValue)) {
                asmOpRR(a, true, Span<const u8>({u8(99LL)}), d, s, false);
            } else if ((width == u8(4LL))) {
                asmOpRR(a, false, Span<const u8>({u8(139LL)}), d, s, false);
            } else if ((width == u8(2LL))) {
                u8 op = u8(183LL);
                if (signedValue) {
                    op = u8(191LL);
                }
                asmOpRR(a, (signedValue && (dsize == u8(8LL))), Span<const u8>({u8(15LL), op}), d, s, false);
            } else {
                u8 op = u8(182LL);
                if (signedValue) {
                    op = u8(190LL);
                }
                asmOpRR(a, (signedValue && (dsize == u8(8LL))), Span<const u8>({u8(15LL), op}), d, s, true);
            }
        switchEnd577:;
        }
    }

    static void asmFalu(Asm* a, FAluOp op, u8 size, VReg dst, VReg x, VReg y) {
        asmVexRR(a, fAluPP(op, size), u8(1LL), false, floatOpcode(op), regNum(dst), regNum(x), regNum(y));
    }

    static void asmFaluM(Asm* a, FAluOp op, u8 size, VReg dst, VReg x, Amem m) {
        asmVexRM(a, fAluPP(op, size), u8(1LL), false, floatOpcode(op), regNum(dst), regNum(x), m, Int(0LL));
    }

    static void asmFblend(Asm* a, u8 size, VReg dst, VReg x, VReg y, VReg mask) {
        u8 op = u8(74LL);
        if ((size == u8(8LL))) {
            op = u8(75LL);
        }
        asmVexRR(a, pp66, u8(3LL), false, op, regNum(dst), regNum(x), regNum(y));
        asmBytes(a, {u8(convert<u8>(shiftLeft(regNum(mask), Int(4LL))))});
    }

    static void asmFcmp(Asm* a, u8 size, VReg x, VReg y) {
        asmVexRR(a, packedPP(size), u8(1LL), false, u8(46LL), regNum(x), Int(-1LL), regNum(y));
    }

    static void asmFcmpM(Asm* a, u8 size, VReg x, Amem m) {
        asmVexRM(a, packedPP(size), u8(1LL), false, u8(46LL), regNum(x), Int(-1LL), m, Int(0LL));
    }

    static void asmFcmpMask(Asm* a, u8 size, VReg dst, VReg x, VReg y, u8 predicate) {
        asmVexRR(a, scalarPP(size), u8(1LL), false, u8(194LL), regNum(dst), regNum(x), regNum(y));
        asmBytes(a, {u8(predicate)});
    }

    static void asmFload(Asm* a, u8 size, VReg dst, Amem m) {
        asmVexRM(a, scalarPP(size), u8(1LL), false, u8(16LL), regNum(dst), Int(-1LL), m, Int(0LL));
    }

    static void asmFmov(Asm* a, VReg dst, VReg src) {
        asmVexRR(a, ppNone, u8(1LL), false, u8(40LL), regNum(dst), Int(-1LL), regNum(src));
    }

    static void asmFround(Asm* a, u8 size, VReg dst, VReg src, i64 mode) {
        u8 op = u8(10LL);
        if ((size == u8(8LL))) {
            op = u8(11LL);
        }
        asmVexRR(a, pp66, u8(3LL), false, op, regNum(dst), regNum(src), regNum(src));
        asmBytes(a, {u8(u8(convert<u8>(mode) | u8(8LL)))});
    }

    static void asmFsqrt(Asm* a, u8 size, VReg dst, VReg src) {
        asmVexRR(a, scalarPP(size), u8(1LL), false, u8(81LL), regNum(dst), regNum(src), regNum(src));
    }

    static void asmFstore(Asm* a, u8 size, VReg src, Amem m) {
        asmVexRM(a, scalarPP(size), u8(1LL), false, u8(17LL), regNum(src), Int(-1LL), m, Int(0LL));
    }

    static void asmFxor(Asm* a, VReg dst) {
        Int r = regNum(dst);
        asmVexRR(a, ppNone, u8(1LL), false, u8(87LL), r, r, r);
    }

    static void asmHere8(Asm* a, i32 at) {
        i32 d = (asmPos(a) - ((at + i32(1LL))));
        if ((d > i32(127LL))) {
            compilerError(String("internal error: short jump too far"));
        }
        (a)->buf[at] = convert<u8>(d);
    }

    static void asmImul3(Asm* a, u8 size, VReg dst, VReg src, i64 x) {
        if ((x == convert<i64>(convert<i8>(x)))) {
            asmOpRR(a, (size == u8(8LL)), Span<const u8>({u8(107LL)}), convert<i64>(dst), convert<i64>(src), false);
            asmBytes(a, {u8(convert<u8>(x))});
            return;
        }
        asmOpRR(a, (size == u8(8LL)), Span<const u8>({u8(105LL)}), convert<i64>(dst), convert<i64>(src), false);
        asmU32(a, convert<u32>(x));
    }

    static i32 asmJcc8(Asm* a, u8 cc) {
        asmBytes(a, {u8(u8(u8(112LL) + cc)), u8(u8(0LL))});
        return (asmPos(a) - i32(1LL));
    }

    static i32 asmJccRel(Asm* a, u8 cc) {
        asmBytes(a, {u8(u8(15LL)), u8(u8(u8(128LL) + cc))});
        asmU32(a, u32(0LL));
        return (asmPos(a) - i32(4LL));
    }

    static i32 asmJmp8(Asm* a) {
        asmBytes(a, {u8(u8(235LL)), u8(u8(0LL))});
        return (asmPos(a) - i32(1LL));
    }

    static i32 asmJmpRel(Asm* a) {
        asmBytes(a, {u8(u8(233LL))});
        asmU32(a, u32(0LL));
        return (asmPos(a) - i32(4LL));
    }

    static void asmLea(Asm* a, u8 size, VReg dst, Amem m) {
        asmOpRM(a, (size == u8(8LL)), Span<const u8>({u8(141LL)}), convert<i64>(dst), m, Int(0LL), false);
        if ((dst == RSP)) {
            if (((((m).base != Int(4LL)) || ((m).index >= Int(0LL))) || ((m).label >= i32(0LL)))) {
                compilerError(String("internal error: untracked change of rsp"));
            }
            asmMoveSP(a, (-((m).disp)));
        }
    }

    // load emits a load of width bytes into a register of dsize bytes.
    static void asmLoad(Asm* a, u8 width, u8 dsize, bool signedValue, VReg dst, Amem m) {
        Int d = convert<i64>(dst);
        {
            if ((width == u8(8LL))) {
                asmOpRM(a, true, Span<const u8>({u8(139LL)}), d, m, Int(0LL), false);
            } else if ((((width == u8(4LL)) && signedValue) && (dsize == u8(8LL)))) {
                asmOpRM(a, true, Span<const u8>({u8(99LL)}), d, m, Int(0LL), false);
            } else if ((width == u8(4LL))) {
                asmOpRM(a, false, Span<const u8>({u8(139LL)}), d, m, Int(0LL), false);
            } else if ((width == u8(2LL))) {
                u8 op = u8(183LL);
                if (signedValue) {
                    op = u8(191LL);
                }
                asmOpRM(a, (signedValue && (dsize == u8(8LL))), Span<const u8>({u8(15LL), op}), d, m, Int(0LL), false);
            } else {
                u8 op = u8(182LL);
                if (signedValue) {
                    op = u8(190LL);
                }
                asmOpRM(a, (signedValue && (dsize == u8(8LL))), Span<const u8>({u8(15LL), op}), d, m, Int(0LL), false);
            }
        switchEnd579:;
        }
    }

    // modrmMem emits ModRM, SIB and displacement for a memory operand; imm is
    // the number of immediate bytes that follow, for RIP-relative fixups.
    static void asmModrmMem(Asm* a, Int reg, Amem m, Int imm) {
        u8 r = shiftLeft(lo3(reg), Int(3LL));
        if (((m).label >= i32(0LL))) {
            asmBytes(a, {u8(u8(r | u8(5LL)))});
            (a)->fixups = appendValues((a)->fixups, RipFixup{asmPos(a), ((asmPos(a) + i32(4LL)) + convert<i32>(imm)), (m).label, (m).labelKind});
            asmU32(a, u32(0LL));
            return;
        }
        if (((m).base < Int(0LL))) {
            asmBytes(a, {u8(u8(r | u8(4LL)))});
            asmBytes(a, {u8(u8(u8(shiftLeft(scaleBits((m).scale), Int(6LL)) | shiftLeft(lo3((m).index), Int(3LL))) | u8(5LL)))});
            asmU32(a, convert<u32>((m).disp));
            return;
        }
        u8 mod = {};
        {
            if ((((m).disp == i32(0LL)) && (lo3((m).base) != u8(5LL)))) {
                mod = u8(0LL);
            } else if (((m).disp == convert<i32>(convert<i8>((m).disp)))) {
                mod = u8(1LL);
            } else {
                mod = u8(2LL);
            }
        switchEnd581:;
        }
        if ((((m).index >= Int(0LL)) || (lo3((m).base) == u8(4LL)))) {
            u8 index = u8(4LL);
            if (((m).index >= Int(0LL))) {
                index = lo3((m).index);
            }
            asmBytes(a, {u8(u8(u8(shiftLeft(mod, Int(6LL)) | r) | u8(4LL)))});
            asmBytes(a, {u8(u8(u8(shiftLeft(scaleBits((m).scale), Int(6LL)) | shiftLeft(index, Int(3LL))) | lo3((m).base)))});
        } else {
            asmBytes(a, {u8(u8(u8(shiftLeft(mod, Int(6LL)) | r) | lo3((m).base)))});
        }
        {
            auto switch582 = mod;
            switch (switch582) {
                case u8(1LL): {
                    asmBytes(a, {u8(convert<u8>((m).disp))});
                    break;
                }
                case u8(2LL): {
                    asmU32(a, convert<u32>((m).disp));
                    break;
                }
            }
        }
    }

    static void asmModrmReg(Asm* a, Int reg, Int rm) {
        asmBytes(a, {u8(u8(u8(u8(192LL) | shiftLeft(lo3(reg), Int(3LL))) | lo3(rm)))});
    }

    static void asmMovGX(Asm* a, u8 size, VReg dst, VReg src) {
        asmVexRR(a, pp66, u8(1LL), (size == u8(8LL)), u8(110LL), regNum(dst), Int(-1LL), convert<i64>(src));
    }

    static void asmMovImm(Asm* a, u8 size, VReg dst, i64 x) {
        Int d = convert<i64>(dst);
        {
            if ((x == i64(0LL))) {
                asmOpRR(a, false, Span<const u8>({u8(51LL)}), d, d, false);
            } else if (((size == u8(4LL)) || (((x >= i64(0LL)) && (x <= i64(4294967295LL)))))) {
                asmRex(a, false, Int(-1LL), Int(-1LL), d, false);
                asmBytes(a, {u8(u8(u8(184LL) + lo3(d)))});
                asmU32(a, convert<u32>(x));
            } else if ((x == convert<i64>(convert<i32>(x)))) {
                asmRex(a, true, Int(-1LL), Int(-1LL), d, false);
                asmBytes(a, {u8(u8(199LL))});
                asmModrmReg(a, Int(0LL), d);
                asmU32(a, convert<u32>(x));
            } else {
                asmRex(a, true, Int(-1LL), Int(-1LL), d, false);
                asmBytes(a, {u8(u8(u8(184LL) + lo3(d)))});
                asmU64(a, convert<u64>(x));
            }
        switchEnd584:;
        }
    }

    static void asmMovRR(Asm* a, u8 size, VReg dst, VReg src) {
        asmOpRR(a, (size == u8(8LL)), Span<const u8>({u8(139LL)}), convert<i64>(dst), convert<i64>(src), false);
    }

    static void asmMovXG(Asm* a, u8 size, VReg dst, VReg src) {
        asmVexRR(a, pp66, u8(1LL), (size == u8(8LL)), u8(126LL), regNum(src), Int(-1LL), convert<i64>(dst));
    }

    static void asmMoveSP(Asm* a, i32 by) {
        asmSetSP(a, ((a)->sp + by));
    }

    static void asmNops(Asm* a, i32 n) {
        {
            for (; (n > i32(0LL));) {
                i32 width = minimum(n, i32(9LL));
                appendSpan((a)->buf, Span<const u8>(nopBytes[width]));
                n -= width;
            }
        }
    }

    static void asmOpRM(Asm* a, bool w, Span<const u8> op, Int reg, Amem m, Int imm, bool byteReg) {
        asmRex(a, w, reg, memX(m), memB(m), (byteReg && (reg >= Int(4LL))));
        asmB(a, op);
        asmModrmMem(a, reg, m, imm);
    }

    // opRR emits an instruction "op reg, rm" between registers.
    static void asmOpRR(Asm* a, bool w, Span<const u8> op, Int reg, Int rm, bool byteRegs) {
        asmRex(a, w, reg, Int(-1LL), rm, (byteRegs && (((reg >= Int(4LL)) || (rm >= Int(4LL))))));
        asmB(a, op);
        asmModrmReg(a, reg, rm);
    }

    // patch32 stores a displacement to target, relative to the end of the
    // field at at.
    static void asmPatch32(Asm* a, i32 at, i32 target) {
        littlePutUint32(subslice((a)->buf, at, length((a)->buf)), convert<u32>((target - ((at + i32(4LL))))));
    }

    // patch8 points the short jump whose displacement is at at to target.
    static void asmPatch8(Asm* a, i32 at, i32 target) {
        i32 d = (target - ((at + i32(1LL))));
        if ((d != convert<i32>(convert<i8>(d)))) {
            compilerError(String("internal error: short jump too far"));
        }
        (a)->buf[at] = convert<u8>(convert<i8>(d));
    }

    static void asmPop(Asm* a, VReg r) {
        asmRex(a, false, Int(-1LL), Int(-1LL), convert<i64>(r), false);
        asmBytes(a, {u8(u8(u8(88LL) + lo3(convert<i64>(r))))});
        asmMoveSP(a, i32(-8LL));
    }

    static void asmPopMem(Asm* a, Amem m) {
        asmOpRM(a, false, Span<const u8>({u8(143LL)}), Int(0LL), m, Int(0LL), false);
        asmMoveSP(a, i32(-8LL));
    }

    static void asmPopf(Asm* a) {
        asmBytes(a, {u8(u8(157LL))});
        asmMoveSP(a, i32(-8LL));
    }

    static i32 asmPos(Asm* a) {
        return convert<i32>(length((a)->buf));
    }

    // prefixed emits a mandatory prefix (66, F2, F3) before the REX prefix.
    static void asmPrefixed(Asm* a, u8 prefix, bool w, Span<const u8> op, Int reg, Int rm) {
        asmBytes(a, {u8(prefix)});
        asmOpRR(a, w, op, reg, rm, false);
    }

    static void asmPush(Asm* a, VReg r) {
        asmRex(a, false, Int(-1LL), Int(-1LL), convert<i64>(r), false);
        asmBytes(a, {u8(u8(u8(80LL) + lo3(convert<i64>(r))))});
        asmMoveSP(a, i32(8LL));
    }

    static void asmPushMem(Asm* a, Amem m) {
        asmOpRM(a, false, Span<const u8>({u8(255LL)}), Int(6LL), m, Int(0LL), false);
        asmMoveSP(a, i32(8LL));
    }

    static void asmPushf(Asm* a) {
        asmBytes(a, {u8(u8(156LL))});
        asmMoveSP(a, i32(8LL));
    }

    static void asmRepMovsb(Asm* a) {
        asmBytes(a, {u8(u8(243LL)), u8(u8(164LL))});
    }

    static void asmRepStosb(Asm* a) {
        asmBytes(a, {u8(u8(243LL)), u8(u8(170LL))});
    }

    static void asmRet(Asm* a) {
        asmBytes(a, {u8(u8(195LL))});
    }

    // rex emits a REX prefix when needed: w for 64-bit operands, r extends
    // ModRM.reg, x SIB.index, b ModRM.rm or SIB.base; force for byte registers
    // spl..dil.
    static void asmRex(Asm* a, bool w, Int r, Int x, Int b, bool force) {
        u8 v = u8(64LL);
        if (w) {
            v |= u8(8LL);
        }
        if ((r >= Int(0LL))) {
            v |= shiftLeft(hi(r), Int(2LL));
        }
        if ((x >= Int(0LL))) {
            v |= shiftLeft(hi(x), Int(1LL));
        }
        if ((b >= Int(0LL))) {
            v |= hi(b);
        }
        if (((v != u8(64LL)) || force)) {
            asmBytes(a, {u8(v)});
        }
    }

    // setSP declares the stack pointer's delta from here on: at the start of a
    // region and where control arrives from elsewhere.
    static void asmSetSP(Asm* a, i32 delta) {
        Int n = length((a)->sps);
        {
            if (((n > Int(0LL)) && (((a)->sps[(n - Int(1LL))]).pc == asmPos(a)))) {
                ((a)->sps[(n - Int(1LL))]).delta = delta;
            } else if (((n == Int(0LL)) || (((a)->sps[(n - Int(1LL))]).delta != delta))) {
                (a)->sps = appendValues((a)->sps, SpEntry{asmPos(a), delta});
            }
        switchEnd586:;
        }
        (a)->sp = delta;
    }

    static void asmSetcc(Asm* a, u8 cc, VReg dst) {
        Int d = convert<i64>(dst);
        asmRex(a, false, Int(-1LL), Int(-1LL), d, (d >= Int(4LL)));
        asmBytes(a, {u8(u8(15LL)), u8(u8(u8(144LL) + cc))});
        asmModrmReg(a, Int(0LL), d);
        asmRex(a, false, d, Int(-1LL), d, (d >= Int(4LL)));
        asmBytes(a, {u8(u8(15LL)), u8(u8(182LL))});
        asmModrmReg(a, d, d);
    }

    static void asmShiftCL(Asm* a, u8 kind, u8 size, VReg dst) {
        Int d = convert<i64>(dst);
        asmRex(a, (size == u8(8LL)), Int(-1LL), Int(-1LL), d, false);
        asmBytes(a, {u8(u8(211LL))});
        asmModrmReg(a, convert<i64>(kind), d);
    }

    static void asmShiftImm(Asm* a, u8 kind, u8 size, VReg dst, i64 n) {
        Int d = convert<i64>(dst);
        asmRex(a, (size == u8(8LL)), Int(-1LL), Int(-1LL), d, false);
        if ((n == i64(1LL))) {
            asmBytes(a, {u8(u8(209LL))});
            asmModrmReg(a, convert<i64>(kind), d);
            return;
        }
        asmBytes(a, {u8(u8(193LL))});
        asmModrmReg(a, convert<i64>(kind), d);
        asmBytes(a, {u8(convert<u8>(n))});
    }

    static void asmShiftX(Asm* a, u8 kind, u8 size, VReg dst, VReg src, VReg count) {
        u8 pp = u8(1LL);
        {
            auto switch587 = kind;
            switch (switch587) {
                case shiftShr: {
                    pp = ppF2;
                    break;
                }
                case shiftSar: {
                    pp = ppF3;
                    break;
                }
            }
        }
        asmVexRR(a, pp, u8(2LL), (size == u8(8LL)), u8(247LL), convert<i64>(dst), convert<i64>(count), convert<i64>(src));
    }

    // spTable returns the deltas of the region [start, end): the one at its
    // start, then every change, at offsets from start.
    static Slice<SpEntry> asmSpTable(Asm* a, i32 start, i32 end) {
        Int k = sortSearch(length((a)->sps), [&](Int i) mutable -> bool {
            return (((a)->sps[i]).pc > start);
        });
        Slice<SpEntry> out = sliceOf<SpEntry>({SpEntry{i32(0LL), i32(0LL)}});
        if ((k > Int(0LL))) {
            (out[Int(0LL)]).delta = ((a)->sps[(k - Int(1LL))]).delta;
        }
        {
            auto range588 = subslice((a)->sps, k, length((a)->sps));
            for (Int index589 = 0, rangeEnd590 = length(range588); index589 < rangeEnd590; ++index589) {
                SpEntry e = range588[index589];
                if (((e).pc >= end)) {
                    break;
                }
                if (((e).delta != (out[(length(out) - Int(1LL))]).delta)) {
                    out = appendValues(out, SpEntry{((e).pc - start), (e).delta});
                }
            }
        }
        return out;
    }

    static void asmStore(Asm* a, u8 width, VReg src, Amem m) {
        Int s = convert<i64>(src);
        {
            auto switch591 = width;
            switch (switch591) {
                case u8(8LL): {
                    asmOpRM(a, true, Span<const u8>({u8(137LL)}), s, m, Int(0LL), false);
                    break;
                }
                case u8(4LL): {
                    asmOpRM(a, false, Span<const u8>({u8(137LL)}), s, m, Int(0LL), false);
                    break;
                }
                case u8(2LL): {
                    asmBytes(a, {u8(u8(102LL))});
                    asmOpRM(a, false, Span<const u8>({u8(137LL)}), s, m, Int(0LL), false);
                    break;
                }
                default: {
                    asmOpRM(a, false, Span<const u8>({u8(136LL)}), s, m, Int(0LL), true);
                    break;
                }
            }
        }
    }

    static void asmStoreImm(Asm* a, u8 width, Amem m, i64 x) {
        {
            auto switch592 = width;
            switch (switch592) {
                case u8(8LL): {
                    asmOpRM(a, true, Span<const u8>({u8(199LL)}), Int(0LL), m, Int(4LL), false);
                    asmU32(a, convert<u32>(x));
                    break;
                }
                case u8(4LL): {
                    asmOpRM(a, false, Span<const u8>({u8(199LL)}), Int(0LL), m, Int(4LL), false);
                    asmU32(a, convert<u32>(x));
                    break;
                }
                case u8(2LL): {
                    asmBytes(a, {u8(u8(102LL))});
                    asmOpRM(a, false, Span<const u8>({u8(199LL)}), Int(0LL), m, Int(2LL), false);
                    asmBytes(a, {u8(convert<u8>(x)), u8(convert<u8>(shiftRight(x, Int(8LL))))});
                    break;
                }
                default: {
                    asmOpRM(a, false, Span<const u8>({u8(198LL)}), Int(0LL), m, Int(1LL), false);
                    asmBytes(a, {u8(convert<u8>(x))});
                    break;
                }
            }
        }
    }

    static void asmTest(Asm* a, u8 size, VReg x, VReg y) {
        asmOpRR(a, (size == u8(8LL)), Span<const u8>({u8(133LL)}), convert<i64>(y), convert<i64>(x), false);
    }

    static void asmTestImm(Asm* a, u8 size, VReg x, i64 imm) {
        asmRex(a, (size == u8(8LL)), Int(-1LL), Int(-1LL), convert<i64>(x), false);
        asmBytes(a, {u8(u8(247LL))});
        asmModrmReg(a, Int(0LL), convert<i64>(x));
        asmU32(a, convert<u32>(imm));
    }

    static void asmU32(Asm* a, u32 x) {
        u8 bytes[4];
        storeLittle(bytes, x);
        a->buf.append(bytes, sizeof(bytes));
    }

    static void asmU64(Asm* a, u64 x) {
        u8 bytes[8];
        storeLittle(bytes, x);
        a->buf.append(bytes, sizeof(bytes));
    }

    static void asmUd2(Asm* a) {
        asmBytes(a, {u8(u8(15LL)), u8(u8(11LL))});
    }

    static void asmUnary(Asm* a, u8 kind, u8 size, VReg dst) {
        Int d = convert<i64>(dst);
        asmRex(a, (size == u8(8LL)), Int(-1LL), Int(-1LL), d, false);
        asmBytes(a, {u8(u8(247LL))});
        asmModrmReg(a, (Int(3LL) - convert<i64>(kind)), d);
    }

    // vex emits a VEX prefix: pp selects 66/F3/F2, mmmmm the opcode map,
    // vvvv the extra source register.
    static void asmVex(Asm* a, Int r, Int x, Int b, u8 mmmmm, bool w, Int vvvv, bool l, u8 pp) {
        auto temporary593 = u8(1LL);
        auto temporary594 = u8(1LL);
        auto temporary595 = u8(1LL);
        u8 R = temporary593;
        u8 X = temporary594;
        u8 B = temporary595;
        if (((r >= Int(0LL)) && (hi(r) == u8(1LL)))) {
            R = u8(0LL);
        }
        if (((x >= Int(0LL)) && (hi(x) == u8(1LL)))) {
            X = u8(0LL);
        }
        if (((b >= Int(0LL)) && (hi(b) == u8(1LL)))) {
            B = u8(0LL);
        }
        u8 v = u8(15LL);
        if ((vvvv >= Int(0LL))) {
            v = u8(convert<u8>((~(vvvv))) & u8(15LL));
        }
        u8 L = u8(0LL);
        if (l) {
            L = u8(1LL);
        }
        if (((((X == u8(1LL)) && (B == u8(1LL))) && (mmmmm == u8(1LL))) && (!(w)))) {
            asmBytes(a, {u8(u8(197LL)), u8(u8(u8(u8(shiftLeft(R, Int(7LL)) | shiftLeft(v, Int(3LL))) | shiftLeft(L, Int(2LL))) | pp))});
            return;
        }
        u8 W = u8(0LL);
        if (w) {
            W = u8(1LL);
        }
        asmBytes(a, {u8(u8(196LL)), u8(u8(u8(u8(shiftLeft(R, Int(7LL)) | shiftLeft(X, Int(6LL))) | shiftLeft(B, Int(5LL))) | mmmmm)), u8(u8(u8(u8(shiftLeft(W, Int(7LL)) | shiftLeft(v, Int(3LL))) | shiftLeft(L, Int(2LL))) | pp))});
    }

    static void asmVexRM(Asm* a, u8 pp, u8 mmmmm, bool w, u8 op, Int reg, Int vvvv, Amem m, Int imm) {
        asmVex(a, reg, memX(m), memB(m), mmmmm, w, vvvv, false, pp);
        asmBytes(a, {u8(op)});
        asmModrmMem(a, reg, m, imm);
    }

    // vexRR emits a VEX instruction with register operands reg, vvvv and rm.
    static void asmVexRR(Asm* a, u8 pp, u8 mmmmm, bool w, u8 op, Int reg, Int vvvv, Int rm) {
        asmVex(a, reg, Int(-1LL), rm, mmmmm, w, vvvv, false, pp);
        asmBytes(a, {u8(op)});
        asmModrmReg(a, reg, rm);
    }

    static void asmXchg(Asm* a, VReg x, VReg y) {
        asmOpRR(a, true, Span<const u8>({u8(135LL)}), convert<i64>(x), convert<i64>(y), false);
    }

    // Link children in reverse RPO: pushing this list onto a stack visits
    // siblings in the same forward order as the dominator traversal.
    static DominatorChildren cfgInfoChildren(CfgInfo* ci) {
        DominatorChildren children;
        children.first.resize(usize(length(ci->idom)));
        children.first.fill(-1);
        children.next.resize(usize(length(ci->idom)));
        for (Int i = 1; i < length(ci->order); ++i) {
            const i32 b = ci->order[i];
            const i32 p = ci->idom[b];
            children.next[usize(b)] = children.first[usize(p)];
            children.first[usize(p)] = b;
        }
        return children;
    }

    static bool cfgInfoDominates(CfgInfo* ci, i32 a, i32 b) {
        {
            for (; true;) {
                if ((a == b)) {
                    return true;
                }
                if (((ci)->rpoIndex[b] <= (ci)->rpoIndex[a])) {
                    return false;
                }
                b = (ci)->idom[b];
            }
        }
    }

    // dominators is Cooper, Harvey and Kennedy's iterative algorithm.
    static void cfgInfoDominators(CfgInfo* ci, Function* f) {
        Int n = length((f)->blocks);
        (ci)->idom = makeSlice<i32>(n);
        {
            auto range599 = (ci)->idom;
            for (Int index600 = 0, rangeEnd601 = length(range599); index600 < rangeEnd601; ++index600) {
                Int i = index600;
                (ci)->idom[i] = i32(-1LL);
            }
        }
        i32 entry = (ci)->order[Int(0LL)];
        (ci)->idom[entry] = entry;
        {
            bool changed = true;
            for (; changed;) {
                changed = false;
                {
                    auto range602 = subslice((ci)->order, Int(1LL), length((ci)->order));
                    for (Int index603 = 0, rangeEnd604 = length(range602); index603 < rangeEnd604; ++index603) {
                        i32 b = range602[index603];
                        i32 newIdom = i32(-1LL);
                        {
                            auto range605 = ((f)->blocks[b]).preds;
                            for (Int index606 = 0, rangeEnd607 = length(range605); index606 < rangeEnd607; ++index606) {
                                i32 p = range605[index606];
                                if (((ci)->idom[p] < i32(0LL))) {
                                    continue;
                                }
                                if ((newIdom < i32(0LL))) {
                                    newIdom = p;
                                } else {
                                    newIdom = cfgInfoIntersect(ci, p, newIdom);
                                }
                            }
                        }
                        if ((newIdom != (ci)->idom[b])) {
                            (ci)->idom[b] = newIdom;
                            changed = true;
                        }
                    }
                }
            }
        }
    }

    static i32 cfgInfoIntersect(CfgInfo* ci, i32 a, i32 b) {
        {
            for (; (a != b);) {
                {
                    for (; ((ci)->rpoIndex[a] > (ci)->rpoIndex[b]);) {
                        a = (ci)->idom[a];
                    }
                }
                {
                    for (; ((ci)->rpoIndex[b] > (ci)->rpoIndex[a]);) {
                        b = (ci)->idom[b];
                    }
                }
            }
        }
        return a;
    }

    static void cfgInfoLoops(CfgInfo* ci, Function* f) {
        Int n = length((f)->blocks);
        (ci)->depth = makeSlice<i32>(n);
        (ci)->header = makeSlice<bool>(n);
        Slice<i32> work = {};
        Slice<i32> inLoop = makeSlice<i32>(n);
        {
            auto range608 = inLoop;
            for (Int index609 = 0, rangeEnd610 = length(range608); index609 < rangeEnd610; ++index609) {
                Int i = index609;
                inLoop[i] = i32(-1LL);
            }
        }
        {
            auto range611 = (ci)->order;
            for (Int index612 = 0, rangeEnd613 = length(range611); index612 < rangeEnd613; ++index612) {
                i32 h = range611[index612];
                Slice<i32> latches = {};
                {
                    auto range614 = ((f)->blocks[h]).preds;
                    for (Int index615 = 0, rangeEnd616 = length(range614); index615 < rangeEnd616; ++index615) {
                        i32 p = range614[index615];
                        if (((ci)->rpoIndex[p] >= (ci)->rpoIndex[h])) {
                            latches = appendValues(latches, p);
                        }
                    }
                }
                if ((length(latches) == Int(0LL))) {
                    continue;
                }
                (ci)->header[h] = true;
                inLoop[h] = h;
                (ci)->depth[h]++;
                work = appendSlice(subslice(work, Int(0), Int(0LL)), latches);
                {
                    for (; (length(work) > Int(0LL));) {
                        i32 b = work[(length(work) - Int(1LL))];
                        work = subslice(work, Int(0), (length(work) - Int(1LL)));
                        if (((inLoop[b] == h) || ((ci)->rpoIndex[b] < i32(0LL)))) {
                            continue;
                        }
                        inLoop[b] = h;
                        (ci)->depth[b]++;
                        work = appendSlice(work, ((f)->blocks[b]).preds);
                    }
                }
            }
        }
    }

    // edge ends the current block's terminator edge list with an edge to b and
    // records it as one of b's predecessors.
    static void frontendAddEdge(Frontend* fe, i32 from, i32 to, Slice<i32> args) {
        Term* t = (&((((fe)->f)->blocks[from]).term));
        (t)->edges = appendValues((t)->edges, Edge{to, args});
        (fe)->preds[to] = appendValues((fe)->preds[to], PredRef{from, convert<i32>((length((t)->edges) - Int(1LL)))});
        if ((fe)->sealed[to]) {
            compilerError(String("internal error: edge to sealed block b%d"));
        }
    }

    // addExplicit adds a parameter for an operand stack value; edges supply
    // its argument when they are created.
    static i32 frontendAddExplicit(Frontend* fe, i32 b, ValType t) {
        if ((length((((fe)->f)->blocks[b]).params) != (fe)->explicitValue[b])) {
            compilerError(String("internal error: explicit parameter after a phi"));
        }
        (fe)->explicitValue[b]++;
        return frontendAddParam(fe, b, t);
    }

    static i32 frontendAddOperands(Frontend* fe, i32 local, i32 phi) {
        i32 b = (((fe)->f)->values[phi]).block;
        Slice<i32> ops = makeSlice<i32>(Int(0LL), length((fe)->preds[b]));
        {
            auto range617 = (fe)->preds[b];
            for (Int index618 = 0, rangeEnd619 = length(range617); index618 < rangeEnd619; ++index618) {
                PredRef p = range617[index618];
                ops = appendValues(ops, frontendRead(fe, local, (p).block));
            }
        }
        setMap((fe)->phiOps, phi, ops);
        return frontendTryRemoveTrivial(fe, phi);
    }

    static i32 frontendAddParam(Frontend* fe, i32 b, ValType t) {
        Block* blk = (&(((fe)->f)->blocks[b]));
        i32 p = functionNewValue((fe)->f, OpParam, t, b, convert<i64>(length((blk)->params)), Span<const i32>({}));
        (blk)->params = appendValues((blk)->params, p);
        return p;
    }

    static ValType frontendAddrType(Frontend* fe) {
        if (((((fe)->m)->mems[Int(0LL)]).limits).is64) {
            return I64;
        }
        return I32;
    }

    static void frontendBinary(Frontend* fe, Op op, ValType t) {
        if (((fe)->cur < i32(0LL))) {
            return;
        }
        i32 b = frontendPop(fe);
        i32 a = frontendPop(fe);
        frontendPush(fe, frontendEmit(fe, op, t, i64(0LL), Span<const i32>({a, b})));
    }

    static Tuple2<Slice<ValType>, Slice<ValType>> frontendBlockType(Frontend* fe) {
        Reader* r = (&((fe)->r));
        if (((r)->pos < length((r)->data))) {
            {
                u8 b = (r)->data[(r)->pos];
                auto switch620 = b;
                switch (switch620) {
                    case u8(64LL): {
                        (r)->pos++;
                        return {{}, {}};
                        break;
                    }
                    case u8(127LL):
                    case u8(126LL):
                    case u8(125LL):
                    case u8(124LL):
                    case u8(123LL):
                    case u8(112LL):
                    case u8(111LL): {
                        (r)->pos++;
                        return {{}, sliceOf<ValType>({convert<ValType>(b)})};
                        break;
                    }
                }
            }
        }
        i64 index = readerSleb(r, Uint(33LL));
        if (((index < i64(0LL)) || (convert<i64>(index) >= length(((fe)->m)->types)))) {
            compilerError(String("unknown type %d"));
        }
        FuncType* t = (&(((fe)->m)->types[index]));
        return {(t)->params, (t)->results};
    }

    static void frontendBody(Frontend* fe) {
        Reader* r = (&((fe)->r));
        {
            for (; (length((fe)->ctrls) > Int(0LL));) {
                u8 op = readerByte(r);
                if (((fe)->cur < i32(0LL))) {
                    frontendSkip(fe, op);
                    continue;
                }
                {
                    auto switch621 = op;
                    switch (switch621) {
                        case u8(0LL): {
                            (((fe)->f)->blocks[(fe)->cur]).term = Term{TermTrap, {}, {}, {}, trapUnreachable};
                            frontendMarkDead(fe);
                            break;
                        }
                        case u8(1LL): {
                            break;
                        }
                        case u8(2LL):
                        case u8(3LL): {
                            auto temporary622 = frontendBlockType(fe);
                            Slice<ValType> params = temporary622.v0;
                            Slice<ValType> results = temporary622.v1;
                            Slice<i32> args = frontendPopN(fe, length(params));
                            Control c = Control{ctrlBlock, params, results, length((fe)->stack), {}, i32(-1LL), {}, {}, {}};
                            if ((op == u8(3LL))) {
                                (c).kind = ctrlLoop;
                                i32 header = frontendNewBlock(fe);
                                (c).target = header;
                                Slice<i32> vals = makeSlice<i32>(length(params));
                                {
                                    auto range623 = params;
                                    for (Int index624 = 0, rangeEnd625 = length(range623); index624 < rangeEnd625; ++index624) {
                                        Int i = index624;
                                        ValType t = range623[index624];
                                        vals[i] = frontendAddExplicit(fe, header, t);
                                    }
                                }
                                frontendJump(fe, header, args);
                                (fe)->cur = header;
                                args = vals;
                            } else {
                                (c).target = frontendNewBlock(fe);
                                {
                                    auto range626 = results;
                                    for (Int index627 = 0, rangeEnd628 = length(range626); index627 < rangeEnd628; ++index627) {
                                        ValType t = range626[index627];
                                        frontendAddExplicit(fe, (c).target, t);
                                    }
                                }
                            }
                            (fe)->ctrls = appendValues((fe)->ctrls, c);
                            (fe)->stack = appendSlice((fe)->stack, args);
                            break;
                        }
                        case u8(4LL): {
                            auto temporary629 = frontendBlockType(fe);
                            Slice<ValType> params = temporary629.v0;
                            Slice<ValType> results = temporary629.v1;
                            i32 cond = frontendPop(fe);
                            Slice<i32> args = frontendPopN(fe, length(params));
                            Control c = Control{ctrlIf, params, results, length((fe)->stack), {}, {}, args, {}, {}};
                            (c).target = frontendNewBlock(fe);
                            {
                                auto range630 = results;
                                for (Int index631 = 0, rangeEnd632 = length(range630); index631 < rangeEnd632; ++index631) {
                                    ValType t = range630[index631];
                                    frontendAddExplicit(fe, (c).target, t);
                                }
                            }
                            i32 then = frontendNewBlock(fe);
                            (c).elseBlock = frontendNewBlock(fe);
                            Block* blk = (&(((fe)->f)->blocks[(fe)->cur]));
                            ((blk)->term).kind = TermIf;
                            ((blk)->term).value = cond;
                            frontendAddEdge(fe, (fe)->cur, then, {});
                            frontendAddEdge(fe, (fe)->cur, (c).elseBlock, {});
                            frontendSeal(fe, then);
                            frontendSeal(fe, (c).elseBlock);
                            (fe)->cur = then;
                            (fe)->ctrls = appendValues((fe)->ctrls, c);
                            (fe)->stack = appendSlice((fe)->stack, args);
                            break;
                        }
                        case u8(5LL): {
                            frontendElseOp(fe);
                            break;
                        }
                        case u8(11LL): {
                            frontendEnd(fe);
                            break;
                        }
                        case u8(12LL): {
                            Control* c = frontendLabel(fe, readerU32(r));
                            frontendJump(fe, (c)->target, frontendTop(fe, length(frontendLabelTypes(fe, c))));
                            frontendMarkDead(fe);
                            break;
                        }
                        case u8(13LL): {
                            Control* c = frontendLabel(fe, readerU32(r));
                            i32 cond = frontendPop(fe);
                            Slice<i32> args = frontendTop(fe, length(frontendLabelTypes(fe, c)));
                            i32 next = frontendNewBlock(fe);
                            Block* blk = (&(((fe)->f)->blocks[(fe)->cur]));
                            ((blk)->term).kind = TermIf;
                            ((blk)->term).value = cond;
                            frontendAddEdge(fe, (fe)->cur, (c)->target, args);
                            frontendAddEdge(fe, (fe)->cur, next, {});
                            frontendSeal(fe, next);
                            (fe)->cur = next;
                            break;
                        }
                        case u8(14LL): {
                            u32 n = readerU32(r);
                            if ((n > u32(1048576LL))) {
                                compilerError(String("br_table too large"));
                            }
                            Slice<u32> labels = makeSlice<u32>((n + u32(1LL)));
                            {
                                auto range633 = labels;
                                for (Int index634 = 0, rangeEnd635 = length(range633); index634 < rangeEnd635; ++index634) {
                                    Int i = index634;
                                    labels[i] = readerU32(r);
                                }
                            }
                            i32 index = frontendPop(fe);
                            Int arity = length(frontendLabelTypes(fe, frontendLabel(fe, labels[n])));
                            Slice<i32> args = frontendTop(fe, arity);
                            Block* blk = (&(((fe)->f)->blocks[(fe)->cur]));
                            ((blk)->term).kind = TermSwitch;
                            ((blk)->term).value = index;
                            {
                                auto range636 = labels;
                                for (Int index637 = 0, rangeEnd638 = length(range636); index637 < rangeEnd638; ++index637) {
                                    u32 l = range636[index637];
                                    Control* c = frontendLabel(fe, l);
                                    if ((length(frontendLabelTypes(fe, c)) != arity)) {
                                        compilerError(String("type mismatch: br_table arity"));
                                    }
                                    frontendAddEdge(fe, (fe)->cur, (c)->target, appendSlice(Slice<i32>{}, args));
                                }
                            }
                            frontendMarkDead(fe);
                            break;
                        }
                        case u8(15LL): {
                            (((fe)->f)->blocks[(fe)->cur]).term = Term{TermReturn, {}, {}, frontendTop(fe, length((((fe)->f)->typ)->results)), {}};
                            frontendMarkDead(fe);
                            break;
                        }
                        case u8(16LL): {
                            u32 index = readerU32(r);
                            if ((convert<i64>(index) >= length(((fe)->m)->funcs))) {
                                compilerError(String("unknown function %d"));
                            }
                            frontendCall(fe, OpCall, moduleFuncType((fe)->m, index), convert<i64>(index), i32(0LL), {});
                            break;
                        }
                        case u8(17LL): {
                            u32 typeIndex = readerU32(r);
                            u32 table = readerU32(r);
                            if ((convert<i64>(typeIndex) >= length(((fe)->m)->types))) {
                                compilerError(String("unknown type %d"));
                            }
                            if ((convert<i64>(table) >= length(((fe)->m)->tables))) {
                                compilerError(String("unknown table %d"));
                            }
                            i32 elem = frontendPop(fe);
                            frontendCall(fe, OpCallIndirect, (&(((fe)->m)->types[typeIndex])), convert<i64>(typeIndex), convert<i32>(table), sliceOf<i32>({elem}));
                            break;
                        }
                        case u8(26LL): {
                            frontendPop(fe);
                            break;
                        }
                        case u8(27LL):
                        case u8(28LL): {
                            if ((op == u8(28LL))) {
                                u32 n = readerU32(r);
                                {
                                    u32 i = u32(0LL);
                                    for (; (i < n); i++) {
                                        readerValType(r);
                                    }
                                }
                            }
                            i32 cond = frontendPop(fe);
                            i32 b = frontendPop(fe);
                            i32 a = frontendPop(fe);
                            frontendPush(fe, frontendEmit(fe, OpSelect, frontendTypeOf(fe, a), i64(0LL), Span<const i32>({cond, a, b})));
                            break;
                        }
                        case u8(32LL): {
                            i32 local = frontendLocalIndex(fe);
                            frontendPush(fe, frontendRead(fe, local, (fe)->cur));
                            break;
                        }
                        case u8(33LL): {
                            i32 local = frontendLocalIndex(fe);
                            frontendWrite(fe, local, (fe)->cur, frontendPop(fe));
                            break;
                        }
                        case u8(34LL): {
                            i32 local = frontendLocalIndex(fe);
                            i32 v = frontendPop(fe);
                            frontendWrite(fe, local, (fe)->cur, v);
                            frontendPush(fe, v);
                            break;
                        }
                        case u8(35LL): {
                            u32 index = frontendGlobalIndex(fe);
                            frontendPush(fe, frontendEmit(fe, OpGlobalGet, (((fe)->m)->globals[index]).typ, convert<i64>(index), Span<const i32>({})));
                            break;
                        }
                        case u8(36LL): {
                            u32 index = frontendGlobalIndex(fe);
                            frontendEmit(fe, OpGlobalSet, TNone, convert<i64>(index), Span<const i32>({frontendPop(fe)}));
                            break;
                        }
                        case u8(37LL): {
                            u32 table = frontendTableIndex(fe);
                            i32 index = frontendPop(fe);
                            frontendPush(fe, frontendEmit(fe, OpTableGet, (((fe)->m)->tables[table]).elem, convert<i64>(table), Span<const i32>({index})));
                            break;
                        }
                        case u8(38LL): {
                            u32 table = frontendTableIndex(fe);
                            i32 val = frontendPop(fe);
                            i32 index = frontendPop(fe);
                            frontendEmit(fe, OpTableSet, TNone, convert<i64>(table), Span<const i32>({index, val}));
                            break;
                        }
                        case u8(40LL): {
                            frontendLoad(fe, I32, Int(4LL), false);
                            break;
                        }
                        case u8(41LL): {
                            frontendLoad(fe, I64, Int(8LL), false);
                            break;
                        }
                        case u8(42LL): {
                            frontendLoad(fe, F32, Int(4LL), false);
                            break;
                        }
                        case u8(43LL): {
                            frontendLoad(fe, F64, Int(8LL), false);
                            break;
                        }
                        case u8(44LL): {
                            frontendLoad(fe, I32, Int(1LL), true);
                            break;
                        }
                        case u8(45LL): {
                            frontendLoad(fe, I32, Int(1LL), false);
                            break;
                        }
                        case u8(46LL): {
                            frontendLoad(fe, I32, Int(2LL), true);
                            break;
                        }
                        case u8(47LL): {
                            frontendLoad(fe, I32, Int(2LL), false);
                            break;
                        }
                        case u8(48LL): {
                            frontendLoad(fe, I64, Int(1LL), true);
                            break;
                        }
                        case u8(49LL): {
                            frontendLoad(fe, I64, Int(1LL), false);
                            break;
                        }
                        case u8(50LL): {
                            frontendLoad(fe, I64, Int(2LL), true);
                            break;
                        }
                        case u8(51LL): {
                            frontendLoad(fe, I64, Int(2LL), false);
                            break;
                        }
                        case u8(52LL): {
                            frontendLoad(fe, I64, Int(4LL), true);
                            break;
                        }
                        case u8(53LL): {
                            frontendLoad(fe, I64, Int(4LL), false);
                            break;
                        }
                        case u8(54LL):
                        case u8(56LL): {
                            frontendStore(fe, Int(4LL));
                            break;
                        }
                        case u8(55LL):
                        case u8(57LL): {
                            frontendStore(fe, Int(8LL));
                            break;
                        }
                        case u8(58LL):
                        case u8(60LL): {
                            frontendStore(fe, Int(1LL));
                            break;
                        }
                        case u8(59LL):
                        case u8(61LL): {
                            frontendStore(fe, Int(2LL));
                            break;
                        }
                        case u8(62LL): {
                            frontendStore(fe, Int(4LL));
                            break;
                        }
                        case u8(63LL): {
                            frontendMemIndex(fe);
                            frontendPush(fe, frontendEmit(fe, OpMemSize, frontendAddrType(fe), i64(0LL), Span<const i32>({})));
                            break;
                        }
                        case u8(64LL): {
                            frontendMemIndex(fe);
                            i32 delta = frontendPop(fe);
                            frontendPush(fe, frontendEmit(fe, OpMemGrow, frontendAddrType(fe), i64(0LL), Span<const i32>({delta})));
                            break;
                        }
                        case u8(65LL): {
                            frontendPush(fe, frontendConstant(fe, I32, convert<i64>(readerS32(r))));
                            break;
                        }
                        case u8(66LL): {
                            frontendPush(fe, frontendConstant(fe, I64, readerS64(r)));
                            break;
                        }
                        case u8(67LL): {
                            frontendPush(fe, frontendConstant(fe, F32, convert<i64>(readerF32(r))));
                            break;
                        }
                        case u8(68LL): {
                            frontendPush(fe, frontendConstant(fe, F64, convert<i64>(readerF64(r))));
                            break;
                        }
                        case u8(69LL): {
                            frontendUnary(fe, OpEqz, I32);
                            break;
                        }
                        case u8(80LL): {
                            frontendUnary(fe, OpEqz, I32);
                            break;
                        }
                        case u8(70LL):
                        case u8(71LL):
                        case u8(72LL):
                        case u8(73LL):
                        case u8(74LL):
                        case u8(75LL):
                        case u8(76LL):
                        case u8(77LL):
                        case u8(78LL):
                        case u8(79LL): {
                            frontendBinary(fe, Op(OpEq + convert<Op>(u8(op - u8(70LL)))), I32);
                            break;
                        }
                        case u8(81LL):
                        case u8(82LL):
                        case u8(83LL):
                        case u8(84LL):
                        case u8(85LL):
                        case u8(86LL):
                        case u8(87LL):
                        case u8(88LL):
                        case u8(89LL):
                        case u8(90LL): {
                            frontendBinary(fe, Op(OpEq + convert<Op>(u8(op - u8(81LL)))), I32);
                            break;
                        }
                        case u8(91LL):
                        case u8(92LL):
                        case u8(93LL):
                        case u8(94LL):
                        case u8(95LL):
                        case u8(96LL): {
                            frontendBinary(fe, Op(OpFEq + convert<Op>(u8(op - u8(91LL)))), I32);
                            break;
                        }
                        case u8(97LL):
                        case u8(98LL):
                        case u8(99LL):
                        case u8(100LL):
                        case u8(101LL):
                        case u8(102LL): {
                            frontendBinary(fe, Op(OpFEq + convert<Op>(u8(op - u8(97LL)))), I32);
                            break;
                        }
                        case u8(103LL):
                        case u8(104LL):
                        case u8(105LL): {
                            frontendUnary(fe, Op(OpClz + convert<Op>(u8(op - u8(103LL)))), I32);
                            break;
                        }
                        case u8(121LL):
                        case u8(122LL):
                        case u8(123LL): {
                            frontendUnary(fe, Op(OpClz + convert<Op>(u8(op - u8(121LL)))), I64);
                            break;
                        }
                        case u8(106LL):
                        case u8(107LL):
                        case u8(108LL):
                        case u8(109LL):
                        case u8(110LL):
                        case u8(111LL):
                        case u8(112LL):
                        case u8(113LL):
                        case u8(114LL):
                        case u8(115LL):
                        case u8(116LL):
                        case u8(117LL):
                        case u8(118LL):
                        case u8(119LL):
                        case u8(120LL): {
                            frontendBinary(fe, Op(OpAdd + convert<Op>(u8(op - u8(106LL)))), I32);
                            break;
                        }
                        case u8(124LL):
                        case u8(125LL):
                        case u8(126LL):
                        case u8(127LL):
                        case u8(128LL):
                        case u8(129LL):
                        case u8(130LL):
                        case u8(131LL):
                        case u8(132LL):
                        case u8(133LL):
                        case u8(134LL):
                        case u8(135LL):
                        case u8(136LL):
                        case u8(137LL):
                        case u8(138LL): {
                            frontendBinary(fe, Op(OpAdd + convert<Op>(u8(op - u8(124LL)))), I64);
                            break;
                        }
                        case u8(139LL):
                        case u8(140LL):
                        case u8(141LL):
                        case u8(142LL):
                        case u8(143LL):
                        case u8(144LL):
                        case u8(145LL): {
                            frontendUnary(fe, floatUnary[u8(op - u8(139LL))], F32);
                            break;
                        }
                        case u8(153LL):
                        case u8(154LL):
                        case u8(155LL):
                        case u8(156LL):
                        case u8(157LL):
                        case u8(158LL):
                        case u8(159LL): {
                            frontendUnary(fe, floatUnary[u8(op - u8(153LL))], F64);
                            break;
                        }
                        case u8(146LL):
                        case u8(147LL):
                        case u8(148LL):
                        case u8(149LL):
                        case u8(150LL):
                        case u8(151LL):
                        case u8(152LL): {
                            frontendBinary(fe, Op(OpFAdd + convert<Op>(u8(op - u8(146LL)))), F32);
                            break;
                        }
                        case u8(160LL):
                        case u8(161LL):
                        case u8(162LL):
                        case u8(163LL):
                        case u8(164LL):
                        case u8(165LL):
                        case u8(166LL): {
                            frontendBinary(fe, Op(OpFAdd + convert<Op>(u8(op - u8(160LL)))), F64);
                            break;
                        }
                        case u8(167LL): {
                            frontendUnary(fe, OpWrap, I32);
                            break;
                        }
                        case u8(168LL):
                        case u8(170LL): {
                            frontendUnary(fe, OpTruncS, I32);
                            break;
                        }
                        case u8(169LL):
                        case u8(171LL): {
                            frontendUnary(fe, OpTruncU, I32);
                            break;
                        }
                        case u8(172LL): {
                            frontendUnary(fe, OpExtendS, I64);
                            break;
                        }
                        case u8(173LL): {
                            frontendUnary(fe, OpExtendU, I64);
                            break;
                        }
                        case u8(174LL):
                        case u8(176LL): {
                            frontendUnary(fe, OpTruncS, I64);
                            break;
                        }
                        case u8(175LL):
                        case u8(177LL): {
                            frontendUnary(fe, OpTruncU, I64);
                            break;
                        }
                        case u8(178LL):
                        case u8(180LL): {
                            frontendUnary(fe, OpConvertS, F32);
                            break;
                        }
                        case u8(179LL):
                        case u8(181LL): {
                            frontendUnary(fe, OpConvertU, F32);
                            break;
                        }
                        case u8(182LL): {
                            frontendUnary(fe, OpDemote, F32);
                            break;
                        }
                        case u8(183LL):
                        case u8(185LL): {
                            frontendUnary(fe, OpConvertS, F64);
                            break;
                        }
                        case u8(184LL):
                        case u8(186LL): {
                            frontendUnary(fe, OpConvertU, F64);
                            break;
                        }
                        case u8(187LL): {
                            frontendUnary(fe, OpPromote, F64);
                            break;
                        }
                        case u8(188LL): {
                            frontendUnary(fe, OpReinterpret, I32);
                            break;
                        }
                        case u8(189LL): {
                            frontendUnary(fe, OpReinterpret, I64);
                            break;
                        }
                        case u8(190LL): {
                            frontendUnary(fe, OpReinterpret, F32);
                            break;
                        }
                        case u8(191LL): {
                            frontendUnary(fe, OpReinterpret, F64);
                            break;
                        }
                        case u8(192LL): {
                            frontendUnary(fe, OpExtend8S, I32);
                            break;
                        }
                        case u8(193LL): {
                            frontendUnary(fe, OpExtend16S, I32);
                            break;
                        }
                        case u8(194LL): {
                            frontendUnary(fe, OpExtend8S, I64);
                            break;
                        }
                        case u8(195LL): {
                            frontendUnary(fe, OpExtend16S, I64);
                            break;
                        }
                        case u8(196LL): {
                            frontendUnary(fe, OpExtend32S, I64);
                            break;
                        }
                        case u8(208LL): {
                            ValType t = readerRefType(r);
                            frontendPush(fe, functionNewValue((fe)->f, OpRefNull, t, i32(-1LL), i64(0LL), Span<const i32>({})));
                            break;
                        }
                        case u8(209LL): {
                            frontendUnary(fe, OpRefIsNull, I32);
                            break;
                        }
                        case u8(210LL): {
                            u32 index = readerU32(r);
                            if ((convert<i64>(index) >= length(((fe)->m)->funcs))) {
                                compilerError(String("unknown function %d"));
                            }
                            frontendPush(fe, frontendEmit(fe, OpRefFunc, FuncRef, convert<i64>(index), Span<const i32>({})));
                            break;
                        }
                        case u8(252LL): {
                            frontendPrefixFC(fe);
                            break;
                        }
                        default: {
                            compilerError(String("illegal opcode 0x%02x at offset %d"));
                            break;
                        }
                    }
                }
            }
        }
        if ((!(readerEof(r)))) {
            compilerError(String("operators remaining after end of function"));
        }
    }

    static void frontendCall(Frontend* fe, Op op, FuncType* t, i64 aux, i32 aux2, Slice<i32> extra) {
        Slice<i32> args = frontendPopN(fe, length((t)->params));
        args = appendSlice(args, extra);
        ValType typ = TNone;
        {
            auto switch639 = length((t)->results);
            switch (switch639) {
                case Int(0LL): {
                    break;
                }
                case Int(1LL): {
                    typ = (t)->results[Int(0LL)];
                    break;
                }
                default: {
                    typ = TTuple;
                    break;
                }
            }
        }
        i32 v = frontendEmit(fe, op, typ, aux, args);
        (((fe)->f)->values[v]).aux2 = aux2;
        if ((length((t)->results) == Int(1LL))) {
            frontendPush(fe, v);
        } else {
            {
                auto range640 = (t)->results;
                for (Int index641 = 0, rangeEnd642 = length(range640); index641 < rangeEnd642; ++index641) {
                    Int i = index641;
                    ValType rt = range640[index641];
                    frontendPush(fe, frontendEmit(fe, OpExtract, rt, convert<i64>(i), Span<const i32>({v})));
                }
            }
        }
    }

    static void frontendCheckData(Frontend* fe, u32 seg) {
        if ((((fe)->m)->dataCount < i64(0LL))) {
            compilerError(String("data count section required"));
        }
        if ((convert<i64>(seg) >= ((fe)->m)->dataCount)) {
            compilerError(String("unknown data segment %d"));
        }
    }

    static i32 frontendConstant(Frontend* fe, ValType t, i64 bits) {
        ConstKey key = ConstKey{t, bits};
        {
            auto temporary643 = lookupMap((fe)->consts, key);
            i32 v = temporary643.v0;
            bool ok = temporary643.v1;
            if (ok) {
                return v;
            }
        }
        i32 v = functionNewValue((fe)->f, OpConst, t, i32(-1LL), bits, Span<const i32>({}));
        setMap((fe)->consts, key, v);
        return v;
    }

    // deadAfter makes the code following a construct unreachable: nothing
    // falls out of it.
    static void frontendDeadAfter(Frontend* fe) {
        (fe)->cur = i32(-1LL);
        if ((length((fe)->ctrls) > Int(0LL))) {
            Control* c = (&((fe)->ctrls[(length((fe)->ctrls) - Int(1LL))]));
            (c)->dead = true;
            (fe)->stack = subslice((fe)->stack, Int(0), (c)->height);
        }
    }

    static void frontendElseOp(Frontend* fe) {
        Control* c = (&((fe)->ctrls[(length((fe)->ctrls) - Int(1LL))]));
        if (((c)->kind != ctrlIf)) {
            compilerError(String("else without if"));
        }
        if ((c)->outer) {
            (c)->kind = ctrlElse;
            return;
        }
        if (((fe)->cur >= i32(0LL))) {
            frontendJump(fe, (c)->target, frontendPopN(fe, length((c)->results)));
        }
        (fe)->stack = appendSlice(subslice((fe)->stack, Int(0), (c)->height), (c)->elseArgs);
        (fe)->cur = (c)->elseBlock;
        (c)->elseBlock = i32(-1LL);
        (c)->kind = ctrlElse;
        (c)->dead = false;
    }

    // emit appends an instruction to the current block.
    static i32 frontendEmit(Frontend* fe, Op op, ValType t, i64 aux, Span<const i32> args) {
        i32 v = functionNewValue((fe)->f, op, t, (fe)->cur, aux, args);
        Block* blk = (&(((fe)->f)->blocks[(fe)->cur]));
        (blk)->instrs.pushBack(v);
        return v;
    }

    static void frontendEnd(Frontend* fe) {
        Control* c = (&((fe)->ctrls[(length((fe)->ctrls) - Int(1LL))]));
        if ((c)->outer) {
            (fe)->ctrls = subslice((fe)->ctrls, Int(0), (length((fe)->ctrls) - Int(1LL)));
            return;
        }
        bool live = ((fe)->cur >= i32(0LL));
        Slice<i32> results = {};
        if (live) {
            results = frontendPopN(fe, length((c)->results));
            if ((length((fe)->stack) != (c)->height)) {
                compilerError(String("type mismatch: values remaining on stack at end of block"));
            }
        }
        Control frame = (*(c));
        (fe)->ctrls = subslice((fe)->ctrls, Int(0), (length((fe)->ctrls) - Int(1LL)));
        (fe)->stack = subslice((fe)->stack, Int(0), (frame).height);
        if (((frame).kind == ctrlLoop)) {
            frontendSeal(fe, (frame).target);
            if (live) {
                (fe)->stack = appendSlice((fe)->stack, results);
            } else {
                frontendDeadAfter(fe);
            }
            return;
        }
        if (((frame).kind == ctrlIf)) {
            i32 cur = (fe)->cur;
            (fe)->cur = (frame).elseBlock;
            frontendJump(fe, (frame).target, (frame).elseArgs);
            (fe)->cur = cur;
        }
        if ((live && (length((fe)->preds[(frame).target]) == Int(0LL)))) {
            (((fe)->f)->blocks[(frame).target]).dead = true;
            frontendSeal(fe, (frame).target);
            if (((frame).kind == ctrlFunc)) {
                (((fe)->f)->blocks[(fe)->cur]).term = Term{TermReturn, {}, {}, results, {}};
                (fe)->cur = i32(-1LL);
                return;
            }
            (fe)->stack = appendSlice((fe)->stack, results);
            return;
        }
        if (live) {
            frontendJump(fe, (frame).target, results);
        }
        frontendSeal(fe, (frame).target);
        if ((length((fe)->preds[(frame).target]) == Int(0LL))) {
            (((fe)->f)->blocks[(frame).target]).dead = true;
            frontendDeadAfter(fe);
            return;
        }
        (fe)->cur = (frame).target;
        Slice<i32> params = appendSlice(Slice<i32>{}, subslice((((fe)->f)->blocks[(frame).target]).params, Int(0), length((frame).results)));
        if (((frame).kind == ctrlFunc)) {
            (((fe)->f)->blocks[(fe)->cur]).term = Term{TermReturn, {}, {}, params, {}};
            (fe)->cur = i32(-1LL);
            return;
        }
        (fe)->stack = appendSlice((fe)->stack, params);
    }

    static u32 frontendGlobalIndex(Frontend* fe) {
        u32 index = readerU32(&((fe)->r));
        if ((convert<i64>(index) >= length(((fe)->m)->globals))) {
            compilerError(String("unknown global %d"));
        }
        return index;
    }

    static void frontendJump(Frontend* fe, i32 to, Slice<i32> args) {
        ((((fe)->f)->blocks[(fe)->cur]).term).kind = TermJump;
        frontendAddEdge(fe, (fe)->cur, to, args);
    }

    static Control* frontendLabel(Frontend* fe, u32 depth) {
        if ((convert<i64>(depth) >= length((fe)->ctrls))) {
            compilerError(String("unknown label %d"));
        }
        return (&((fe)->ctrls[((length((fe)->ctrls) - Int(1LL)) - convert<i64>(depth))]));
    }

    // branchArgs are the values a branch to frame c passes: a loop's parameters
    // or a block's results.
    static Slice<ValType> frontendLabelTypes(Frontend* fe, Control* c) {
        if (((c)->kind == ctrlLoop)) {
            return (c)->params;
        }
        return (c)->results;
    }

    static void frontendLoad(Frontend* fe, ValType t, Int width, bool signedValue) {
        auto temporary644 = frontendMemarg(fe);
        i64 offset = temporary644.v0;
        if (((fe)->cur < i32(0LL))) {
            return;
        }
        i32 addr = frontendPop(fe);
        i32 v = frontendEmit(fe, OpLoad, t, offset, Span<const i32>({addr}));
        (((fe)->f)->values[v]).aux2 = convert<i32>(makeMemKind(width, signedValue));
        frontendPush(fe, v);
    }

    static i32 frontendLocalIndex(Frontend* fe) {
        u32 index = readerU32(&((fe)->r));
        if ((convert<i64>(index) >= length((fe)->locals))) {
            compilerError(String("unknown local %d"));
        }
        return convert<i32>(index);
    }

    // markDead makes the rest of the current frame unreachable.
    static void frontendMarkDead(Frontend* fe) {
        Control* c = (&((fe)->ctrls[(length((fe)->ctrls) - Int(1LL))]));
        (c)->dead = true;
        (fe)->stack = subslice((fe)->stack, Int(0), (c)->height);
        (fe)->cur = i32(-1LL);
    }

    static void frontendMemIndex(Frontend* fe) {
        {
            u32 mem = readerU32(&((fe)->r));
            if (((mem != u32(0LL)) || (length(((fe)->m)->mems) == Int(0LL)))) {
                compilerError(String("unknown memory %d"));
            }
        }
    }

    static Tuple2<i64, i32> frontendMemarg(Frontend* fe) {
        Reader* r = (&((fe)->r));
        u32 align = readerU32(r);
        u32 mem = u32(0LL);
        if (((align & u32(64LL)) != u32(0LL))) {
            align &= ~(u32(64LL));
            mem = readerU32(r);
        }
        if (((mem != u32(0LL)) || (length(((fe)->m)->mems) == Int(0LL)))) {
            compilerError(String("unknown memory %d"));
        }
        if ((align >= u32(32LL))) {
            compilerError(String("malformed memop flags"));
        }
        if (((((fe)->m)->mems[Int(0LL)]).limits).is64) {
            return {convert<i64>(readerLeb(r, Uint(64LL))), convert<i32>(align)};
        }
        return {convert<i64>(readerU32(r)), convert<i32>(align)};
    }

    static i32 frontendNewBlock(Frontend* fe) {
        i32 b = functionNewBlock((fe)->f);
        (fe)->defs = appendValues((fe)->defs, zeroValue<Slice<i32>>());
        (fe)->sealed = appendValues((fe)->sealed, false);
        (fe)->incomplete = appendValues((fe)->incomplete, zeroValue<Slice<PendingPhi>>());
        (fe)->preds = appendValues((fe)->preds, zeroValue<Slice<PredRef>>());
        (fe)->explicitValue = appendValues((fe)->explicitValue, Int(0LL));
        return b;
    }

    // paramSlot maps a parameter value to its alias slot. Aliases are kept per
    // value; values created after the parameter grow the table on demand.
    static i32 frontendParamSlot(Frontend* fe, i32 v) {
        {
            for (; (convert<i64>(v) >= length((fe)->alias));) {
                fe->alias.pushBack(-1);
            }
        }
        return v;
    }

    static i32 frontendPop(Frontend* fe) {
        if ((length((fe)->stack) <= ((fe)->ctrls[(length((fe)->ctrls) - Int(1LL))]).height)) {
            compilerError(String("type mismatch: operand stack underflow"));
        }
        i32 v = (fe)->stack[(length((fe)->stack) - Int(1LL))];
        (fe)->stack = subslice((fe)->stack, Int(0), (length((fe)->stack) - Int(1LL)));
        return v;
    }

    // popN pops n values and returns them in stack order.
    static Slice<i32> frontendPopN(Frontend* fe, Int n) {
        if (((length((fe)->stack) - n) < ((fe)->ctrls[(length((fe)->ctrls) - Int(1LL))]).height)) {
            compilerError(String("type mismatch: operand stack underflow"));
        }
        Slice<i32> vals = appendSlice(Slice<i32>{}, subslice((fe)->stack, (length((fe)->stack) - n), length((fe)->stack)));
        (fe)->stack = subslice((fe)->stack, Int(0), (length((fe)->stack) - n));
        return vals;
    }

    static void frontendPrefixFC(Frontend* fe) {
        Reader* r = (&((fe)->r));
        u32 sub = readerU32(r);
        {
            auto switch645 = sub;
            switch (switch645) {
                case u32(0LL):
                case u32(1LL):
                case u32(2LL):
                case u32(3LL): {
                    Op op = OpTruncSatS;
                    if (((sub & u32(1LL)) != u32(0LL))) {
                        op = OpTruncSatU;
                    }
                    frontendUnary(fe, op, I32);
                    break;
                }
                case u32(4LL):
                case u32(5LL):
                case u32(6LL):
                case u32(7LL): {
                    Op op = OpTruncSatS;
                    if (((sub & u32(1LL)) != u32(0LL))) {
                        op = OpTruncSatU;
                    }
                    frontendUnary(fe, op, I64);
                    break;
                }
                case u32(8LL): {
                    u32 seg = readerU32(r);
                    frontendMemIndex(fe);
                    frontendCheckData(fe, seg);
                    i32 n = frontendPop(fe);
                    i32 s = frontendPop(fe);
                    i32 d = frontendPop(fe);
                    frontendEmit(fe, OpMemInit, TNone, convert<i64>(seg), Span<const i32>({d, s, n}));
                    break;
                }
                case u32(9LL): {
                    u32 seg = readerU32(r);
                    frontendCheckData(fe, seg);
                    frontendEmit(fe, OpDataDrop, TNone, convert<i64>(seg), Span<const i32>({}));
                    break;
                }
                case u32(10LL): {
                    frontendMemIndex(fe);
                    frontendMemIndex(fe);
                    i32 n = frontendPop(fe);
                    i32 s = frontendPop(fe);
                    i32 d = frontendPop(fe);
                    frontendEmit(fe, OpMemCopy, TNone, i64(0LL), Span<const i32>({d, s, n}));
                    break;
                }
                case u32(11LL): {
                    frontendMemIndex(fe);
                    i32 n = frontendPop(fe);
                    i32 val = frontendPop(fe);
                    i32 d = frontendPop(fe);
                    frontendEmit(fe, OpMemFill, TNone, i64(0LL), Span<const i32>({d, val, n}));
                    break;
                }
                case u32(12LL): {
                    u32 seg = readerU32(r);
                    u32 table = frontendTableIndex(fe);
                    if ((convert<i64>(seg) >= length(((fe)->m)->elems))) {
                        compilerError(String("unknown elem segment %d"));
                    }
                    i32 n = frontendPop(fe);
                    i32 s = frontendPop(fe);
                    i32 d = frontendPop(fe);
                    i32 v = frontendEmit(fe, OpTableInit, TNone, convert<i64>(seg), Span<const i32>({d, s, n}));
                    (((fe)->f)->values[v]).aux2 = convert<i32>(table);
                    break;
                }
                case u32(13LL): {
                    u32 seg = readerU32(r);
                    if ((convert<i64>(seg) >= length(((fe)->m)->elems))) {
                        compilerError(String("unknown elem segment %d"));
                    }
                    frontendEmit(fe, OpElemDrop, TNone, convert<i64>(seg), Span<const i32>({}));
                    break;
                }
                case u32(14LL): {
                    u32 dst = frontendTableIndex(fe);
                    u32 src = frontendTableIndex(fe);
                    i32 n = frontendPop(fe);
                    i32 s = frontendPop(fe);
                    i32 d = frontendPop(fe);
                    i32 v = frontendEmit(fe, OpTableCopy, TNone, convert<i64>(dst), Span<const i32>({d, s, n}));
                    (((fe)->f)->values[v]).aux2 = convert<i32>(src);
                    break;
                }
                case u32(15LL): {
                    u32 table = frontendTableIndex(fe);
                    i32 n = frontendPop(fe);
                    i32 val = frontendPop(fe);
                    frontendPush(fe, frontendEmit(fe, OpTableGrow, I32, convert<i64>(table), Span<const i32>({val, n})));
                    break;
                }
                case u32(16LL): {
                    u32 table = frontendTableIndex(fe);
                    frontendPush(fe, frontendEmit(fe, OpTableSize, I32, convert<i64>(table), Span<const i32>({})));
                    break;
                }
                case u32(17LL): {
                    u32 table = frontendTableIndex(fe);
                    i32 n = frontendPop(fe);
                    i32 val = frontendPop(fe);
                    i32 d = frontendPop(fe);
                    frontendEmit(fe, OpTableFill, TNone, convert<i64>(table), Span<const i32>({d, val, n}));
                    break;
                }
                default: {
                    compilerError(String("illegal opcode 0xfc %d"));
                    break;
                }
            }
        }
    }

    static void frontendPush(Frontend* fe, i32 v) {
        (fe)->stack = appendValues((fe)->stack, v);
    }

    static i32 frontendRead(Frontend* fe, i32 local, i32 b) {
        {
            Slice<i32> d = (fe)->defs[b];
            if (((!isNil(d)) && (d[local] >= i32(0LL)))) {
                return frontendResolve(fe, d[local]);
            }
        }
        i32 v = {};
        ValType t = (fe)->locals[local];
        {
            if ((!((fe)->sealed[b]))) {
                v = frontendAddParam(fe, b, t);
                (fe)->incomplete[b] = appendValues((fe)->incomplete[b], PendingPhi{local, v});
            } else if ((length((fe)->preds[b]) == Int(1LL))) {
                v = frontendRead(fe, local, ((fe)->preds[b][Int(0LL)]).block);
            } else if ((length((fe)->preds[b]) == Int(0LL))) {
                v = functionNewValue((fe)->f, OpUndef, t, i32(-1LL), i64(0LL), Span<const i32>({}));
            } else {
                v = frontendAddParam(fe, b, t);
                frontendWrite(fe, local, b, v);
                v = frontendAddOperands(fe, local, v);
            }
        switchEnd647:;
        }
        frontendWrite(fe, local, b, v);
        return v;
    }

    static i32 frontendResolve(Frontend* fe, i32 v) {
        {
            for (; ((convert<i64>(v) < length((fe)->alias)) && ((fe)->alias[v] >= i32(0LL)));) {
                v = (fe)->alias[v];
            }
        }
        return v;
    }

    // Alias state is consumed here: only the finished function escapes the frontend.
    static __attribute__((noinline)) void frontendCompactValues(Frontend* fe) {
        Function* f = fe->f;
        // Reuse the frontend's alias table as an old-to-new map. Retained
        // IDs are complemented so alias chains still end at a negative
        // entry while the original IDs are being resolved.
        while (length(fe->alias) < length(f->values)) {
            fe->alias.pushBack(-1);
        }
        const Span<i32> aliases = fe->alias.view();
        i32 count = 0;
        for (u32 v = 0; v < aliases.count; ++v) {
            i32 source = aliases[v];
            if (source < 0) {
                aliases[v] = ~count++;
            } else {
                while (aliases[source] >= 0) {
                    source = aliases[source];
                }
                aliases[v] = source;
            }
        }
        for (u32 v = 0; v < aliases.count; ++v) {
            if (aliases[v] >= 0) {
                aliases[v] = ~aliases[aliases[v]];
            }
        }
        for (u32 v = 0; v < aliases.count; ++v) {
            const i32 entry = aliases[v];
            if (entry < 0) {
                aliases[v] = ~entry;
                f->values[usize(~entry)] = f->values[v];
            }
        }
        // Edge argument slices may share storage. Tag new IDs until every
        // old reference has been visited, then decode in a second walk.
        auto rewriteIds = [&]<bool decode>() {
            auto rewrite = [&](i32& v) {
                if constexpr (!decode) {
                    if (v >= 0) {
                        v = i32(u32(aliases[v]) | 0x80000000u);
                    }
                } else if (u32(v) - 0x80000000u < u32(count)) {
                    v = i32(u32(v) & 0x7fffffffu);
                }
            };
            auto rewriteSpan = [&](Span<i32> ids) {
                for (u32 i = 0; i < ids.count; ++i) {
                    rewrite(ids[i]);
                }
            };
            usize remaining = f->pool.size();
            for (usize page = 0; remaining; ++page) {
                const usize take = remaining < PageVec<i32>::itemsPerPage ? remaining : PageVec<i32>::itemsPerPage;
                i32* operands = f->pool.pages.data()[page];
                for (usize i = 0; i < take; ++i) {
                    rewrite(operands[i]);
                }
                remaining -= take;
            }
            for (usize b = 0; b < f->blocks.size(); ++b) {
                auto& block = f->blocks[b];
                rewriteSpan(block.params);
                rewriteSpan(block.instrs.view());
                auto& term = block.term;
                if (term.kind == TermIf || term.kind == TermSwitch) {
                    rewrite(term.value);
                }
                rewriteSpan(term.values);
                for (Int i = 0; i < length(term.edges); ++i) {
                    rewriteSpan(term.edges[i].args);
                }
            }
        };
        rewriteIds.template operator()<false>();
        rewriteIds.template operator()<true>();
        f->values.resize(usize(count));
        const usize pages = (usize(count) + PageVec<Value>::itemsPerPage - 1) / PageVec<Value>::itemsPerPage;
        while (f->values.pages.size() > pages) {
            freePage(f->values.pages.popBack());
        }
    }

    // resolveAliases rewrites every use of a removed phi to its replacement,
    // keeps the remaining phis after the explicit parameters and appends their
    // operands to the incoming edges.
    static void frontendResolveAliases(Frontend* fe) {
        Function* f = (fe)->f;
        {
            auto& range648 = (f)->blocks;
            for (Int index649 = 0, rangeEnd650 = length(range648); index649 < rangeEnd650; ++index649) {
                Int b = index649;
                Block* blk = (&((f)->blocks[b]));
                if ((blk)->dead) {
                    continue;
                }
                Slice<i32> params = subslice((blk)->params, Int(0), (fe)->explicitValue[b]);
                {
                    auto range651 = subslice((blk)->params, (fe)->explicitValue[b], length((blk)->params));
                    for (Int index652 = 0, rangeEnd653 = length(range651); index652 < rangeEnd653; ++index652) {
                        i32 p = range651[index652];
                        if ((frontendResolve(fe, p) != p)) {
                            continue;
                        }
                        ((f)->values[p]).aux = convert<i64>(length(params));
                        params = appendValues(params, p);
                        {
                            auto range654 = (fe)->preds[b];
                            for (Int index655 = 0, rangeEnd656 = length(range654); index655 < rangeEnd656; ++index655) {
                                Int i = index655;
                                PredRef pr = range654[index655];
                                Edge* e = (&((((f)->blocks[(pr).block]).term).edges[(pr).edge]));
                                (e)->args = appendValues((e)->args, (fe)->phiOps[p][i]);
                            }
                        }
                    }
                }
                (blk)->params = params;
            }
        }
        if (fe->alias.size()) {
            frontendCompactValues(fe);
            fe->alias = Vec<i32>{};
        }
        functionComputePreds(f);
    }

    static void frontendRun(Frontend* fe) {
        Function* f = (fe)->f;
        Reader* r = (&((fe)->r));
        (fe)->locals = appendSlice((fe)->locals, ((f)->typ)->params);
        u32 groups = readerU32(r);
        u64 total = convert<u64>(length((fe)->locals));
        {
            u32 i = u32(0LL);
            for (; (i < groups); i++) {
                u32 n = readerU32(r);
                ValType t = readerValType(r);
                total += convert<u64>(n);
                if ((total > u64(50000LL))) {
                    compilerError(String("too many locals"));
                }
                {
                    u32 j = u32(0LL);
                    for (; (j < n); j++) {
                        (fe)->locals = appendValues((fe)->locals, t);
                    }
                }
            }
        }
        i32 entry = frontendNewBlock(fe);
        frontendSeal(fe, entry);
        (fe)->cur = entry;
        {
            auto range672 = ((f)->typ)->params;
            for (Int index673 = 0, rangeEnd674 = length(range672); index673 < rangeEnd674; ++index673) {
                Int i = index673;
                ValType t = range672[index673];
                frontendWrite(fe, convert<i32>(i), entry, frontendAddExplicit(fe, entry, t));
            }
        }
        {
            Int i = length(((f)->typ)->params);
            for (; (i < length((fe)->locals)); i++) {
                frontendWrite(fe, convert<i32>(i), entry, frontendZero(fe, (fe)->locals[i]));
            }
        }
        i32 ret = frontendNewBlock(fe);
        (fe)->ctrls = appendValues((fe)->ctrls, Control{ctrlFunc, {}, ((f)->typ)->results, {}, ret, i32(-1LL), {}, {}, {}});
        {
            auto range675 = ((f)->typ)->results;
            for (Int index676 = 0, rangeEnd677 = length(range675); index676 < rangeEnd677; ++index676) {
                ValType t = range675[index676];
                frontendAddExplicit(fe, ret, t);
            }
        }
        frontendBody(fe);
        frontendResolveAliases(fe);
    }

    static void frontendSeal(Frontend* fe, i32 b) {
        if ((fe)->sealed[b]) {
            return;
        }
        (fe)->sealed[b] = true;
        Slice<PendingPhi> pending = (fe)->incomplete[b];
        (fe)->incomplete[b] = {};
        {
            auto range678 = pending;
            for (Int index679 = 0, rangeEnd680 = length(range678); index679 < rangeEnd680; ++index679) {
                PendingPhi p = range678[index679];
                frontendAddOperands(fe, (p).local, (p).phi);
            }
        }
    }

    // skip consumes an instruction in unreachable code, tracking only the
    // structure needed to find where reachable code resumes.
    static void frontendSkip(Frontend* fe, u8 op) {
        Reader* r = (&((fe)->r));
        {
            auto switch681 = op;
            switch (switch681) {
                case u8(2LL):
                case u8(3LL):
                case u8(4LL): {
                    frontendBlockType(fe);
                    (fe)->ctrls = appendValues((fe)->ctrls, Control{ctrlBlock, {}, {}, length((fe)->stack), {}, i32(-1LL), {}, {}, true});
                    if ((op == u8(4LL))) {
                        ((fe)->ctrls[(length((fe)->ctrls) - Int(1LL))]).kind = ctrlIf;
                    }
                    break;
                }
                case u8(5LL): {
                    frontendElseOp(fe);
                    break;
                }
                case u8(11LL): {
                    frontendEnd(fe);
                    break;
                }
                case u8(12LL):
                case u8(13LL): {
                    readerU32(r);
                    break;
                }
                case u8(14LL): {
                    u32 n = readerU32(r);
                    {
                        u32 i = u32(0LL);
                        for (; (i <= n); i++) {
                            readerU32(r);
                        }
                    }
                    break;
                }
                case u8(16LL):
                case u8(18LL):
                case u8(32LL):
                case u8(33LL):
                case u8(34LL):
                case u8(35LL):
                case u8(36LL):
                case u8(37LL):
                case u8(38LL):
                case u8(210LL): {
                    readerU32(r);
                    break;
                }
                case u8(17LL): {
                    readerU32(r);
                    readerU32(r);
                    break;
                }
                case u8(28LL): {
                    u32 n = readerU32(r);
                    {
                        u32 i = u32(0LL);
                        for (; (i < n); i++) {
                            readerValType(r);
                        }
                    }
                    break;
                }
                case u8(63LL):
                case u8(64LL): {
                    readerU32(r);
                    break;
                }
                case u8(65LL): {
                    readerS32(r);
                    break;
                }
                case u8(66LL): {
                    readerS64(r);
                    break;
                }
                case u8(67LL): {
                    readerBytes(r, Int(4LL));
                    break;
                }
                case u8(68LL): {
                    readerBytes(r, Int(8LL));
                    break;
                }
                case u8(208LL): {
                    readerRefType(r);
                    break;
                }
                case u8(252LL): {
                    {
                        u32 sub = readerU32(r);
                        auto switch682 = sub;
                        switch (switch682) {
                            case u32(8LL):
                            case u32(12LL):
                            case u32(14LL): {
                                readerU32(r);
                                readerU32(r);
                                break;
                            }
                            case u32(10LL): {
                                readerU32(r);
                                readerU32(r);
                                break;
                            }
                            case u32(9LL):
                            case u32(11LL):
                            case u32(13LL):
                            case u32(15LL):
                            case u32(16LL):
                            case u32(17LL): {
                                readerU32(r);
                                break;
                            }
                            default: {
                                if ((sub > u32(7LL))) {
                                    compilerError(String("illegal opcode 0xfc %d"));
                                }
                                break;
                            }
                        }
                    }
                    break;
                }
                default: {
                    if (((op >= u8(40LL)) && (op <= u8(62LL)))) {
                        frontendMemarg(fe);
                    } else if (((op > u8(196LL)) && (op != u8(209LL)))) {
                        compilerError(String("illegal opcode 0x%02x"));
                    }
                    break;
                }
            }
        }
    }

    static void frontendStore(Frontend* fe, Int width) {
        auto temporary683 = frontendMemarg(fe);
        i64 offset = temporary683.v0;
        if (((fe)->cur < i32(0LL))) {
            return;
        }
        i32 val = frontendPop(fe);
        i32 addr = frontendPop(fe);
        i32 v = frontendEmit(fe, OpStore, TNone, offset, Span<const i32>({addr, val}));
        (((fe)->f)->values[v]).aux2 = convert<i32>(makeMemKind(width, false));
    }

    static u32 frontendTableIndex(Frontend* fe) {
        u32 index = readerU32(&((fe)->r));
        if ((convert<i64>(index) >= length(((fe)->m)->tables))) {
            compilerError(String("unknown table %d"));
        }
        return index;
    }

    // top returns the n values on top of the stack without popping them.
    static Slice<i32> frontendTop(Frontend* fe, Int n) {
        if (((length((fe)->stack) - n) < ((fe)->ctrls[(length((fe)->ctrls) - Int(1LL))]).height)) {
            compilerError(String("type mismatch: operand stack underflow"));
        }
        return appendSlice(Slice<i32>{}, subslice((fe)->stack, (length((fe)->stack) - n), length((fe)->stack)));
    }

    // tryRemoveTrivial replaces a parameter that receives a single value (or
    // itself) on every edge by that value.
    static i32 frontendTryRemoveTrivial(Frontend* fe, i32 phi) {
        i32 same = i32(-1LL);
        {
            auto range684 = (fe)->phiOps[phi];
            for (Int index685 = 0, rangeEnd686 = length(range684); index685 < rangeEnd686; ++index685) {
                i32 op = range684[index685];
                i32 arg = frontendResolve(fe, op);
                if (((arg == same) || (arg == phi))) {
                    continue;
                }
                if ((same >= i32(0LL))) {
                    return phi;
                }
                same = arg;
            }
        }
        if ((same < i32(0LL))) {
            same = functionNewValue((fe)->f, OpUndef, (((fe)->f)->values[phi]).typ, i32(-1LL), i64(0LL), Span<const i32>({}));
        }
        (fe)->alias[frontendParamSlot(fe, phi)] = same;
        return same;
    }

    static ValType frontendTypeOf(Frontend* fe, i32 v) {
        return (((fe)->f)->values[v]).typ;
    }

    static void frontendUnary(Frontend* fe, Op op, ValType t) {
        if (((fe)->cur < i32(0LL))) {
            return;
        }
        i32 a = frontendPop(fe);
        frontendPush(fe, frontendEmit(fe, op, t, i64(0LL), Span<const i32>({a})));
    }

    static void frontendWrite(Frontend* fe, i32 local, i32 b, i32 v) {
        if (isNil((fe)->defs[b])) {
            (fe)->defs[b] = makeSlice<i32>(length((fe)->locals));
            {
                auto range687 = (fe)->defs[b];
                for (Int index688 = 0, rangeEnd689 = length(range687); index688 < rangeEnd689; ++index688) {
                    Int i = index688;
                    (fe)->defs[b][i] = i32(-1LL);
                }
            }
        }
        (fe)->defs[b][local] = v;
    }

    static i32 frontendZero(Frontend* fe, ValType t) {
        if (((t == FuncRef) || (t == ExternRef))) {
            return functionNewValue((fe)->f, OpRefNull, t, i32(-1LL), i64(0LL), Span<const i32>({}));
        }
        return frontendConstant(fe, t, i64(0LL));
    }

    static void funcEmitterBlock(FuncEmitter* e, MBlock* blk, i32 next) {
        (e)->cold = (blk)->cold;
        Span<MInst> insts = (blk)->insts.view();
        Int n = length(insts);
        {
            Int i = Int(0LL);
            for (; (i < n); i++) {
                MInst* in = (&(insts[i]));
                {
                    auto switch690 = (in)->op;
                    switch (switch690) {
                        case mJmp: {
                            if (((in)->target != next)) {
                                funcEmitterJumpTo(e, Int(-1LL), (in)->target);
                            }
                            break;
                        }
                        case mJcc: {
                            if (((((i + Int(1LL)) == (n - Int(1LL))) && ((insts[(i + Int(1LL))]).op == mJmp)) && ((in)->target == next))) {
                                funcEmitterJumpTo(e, convert<i64>(u8((in)->cc ^ u8(1LL))), (insts[(i + Int(1LL))]).target);
                                i++;
                                continue;
                            }
                            funcEmitterJumpTo(e, convert<i64>((in)->cc), (in)->target);
                            break;
                        }
                        default: {
                            funcEmitterInst(e, in);
                            break;
                        }
                    }
                }
            }
        }
    }

    // check emits the bounds check of an access ending at most imm bytes past
    // the address: the common case compares with size - boundSlack, the slow
    // path the exact limit.
    static void funcEmitterCheck(FuncEmitter* e, MInst* in) {
        Asm* a = (e)->a;
        if (((e)->profile != i32(0LL))) {
            asmAluMI(a, aluAdd, u8(8LL), baseAt(ctxReg, ((e)->profile + i32(24LL))), i64(1LL));
        }
        auto temporary691 = (in)->src;
        auto temporary692 = (in)->imm;
        VReg src = temporary691;
        i64 end = temporary692;
        if ((e)->cold) {
            funcEmitterExactCheck(e, src, end);
            return;
        }
        asmAluRM(a, aluCmp, u8(8LL), src, baseAt(ctxReg, offMemBound));
        i32 at = asmJccRel(a, ccG);
        i32 resume = asmPos(a);
        (e)->late = appendValues((e)->late, [=]() mutable -> void {
            asmPatch32(a, at, asmPos(a));
            funcEmitterExactCheck(e, src, end);
            asmPatch32(a, asmJmpRel(a), resume);
        });
    }

    static void funcEmitterCvtU64F(FuncEmitter* e, MInst* in) {
        Asm* a = (e)->a;
        auto temporary693 = (in)->dst;
        auto temporary694 = (in)->src;
        VReg d = temporary693;
        VReg s = temporary694;
        auto temporary695 = mInstTemps(in)[Int(0LL)];
        auto temporary696 = mInstTemps(in)[Int(1LL)];
        VReg t1 = temporary695;
        VReg t2 = temporary696;
        asmTest(a, u8(8LL), s, s);
        i32 big = asmJcc8(a, ccS);
        asmCvtIF(a, u8(8LL), (in)->dsize, d, s);
        i32 done = asmJmp8(a);
        asmHere8(a, big);
        asmMovRR(a, u8(8LL), t1, s);
        asmShiftImm(a, shiftShr, u8(8LL), t1, i64(1LL));
        asmMovRR(a, u8(4LL), t2, s);
        asmAluRI(a, aluAnd, u8(4LL), t2, i64(1LL));
        asmAluRR(a, aluOr, u8(8LL), t1, t2);
        asmCvtIF(a, u8(8LL), (in)->dsize, d, t1);
        asmFalu(a, fAdd, (in)->dsize, d, d, d);
        asmHere8(a, done);
    }

    // A fallback is already cold: use the exact limit directly, without
    // another fast comparison and an out-of-line slow path returning here.
    static void funcEmitterExactCheck(FuncEmitter* e, VReg src, i64 end) {
        Asm* a = (e)->a;
        VReg s = RAX;
        if ((src == RAX)) {
            s = RCX;
        }
        asmPush(a, s);
        asmLoad(a, u8(8LL), u8(8LL), false, s, baseAt(ctxReg, (((e)->oc)->lay)->memoryEnd));
        asmAluRI(a, aluSub, u8(8LL), s, end);
        asmAluRR(a, aluCmp, u8(8LL), src, s);
        asmPop(a, s);
        funcEmitterTrapJcc(e, ccG, trapMemoryOOB);
    }

    static void funcEmitterInst(FuncEmitter* e, MInst* in) {
        Asm* a = (e)->a;
        {
            auto switch697 = (in)->op;
            switch (switch697) {
                case mNop: {
                    break;
                }
                case mCounter: {
                    asmAluMI(a, aluAdd, u8(8LL), funcEmitterMem(e, (in)->mem), (in)->imm);
                    break;
                }
                case mCopyForward: {
                    asmLea(a, u8(8LL), RAX, baseAt(RSI, i32(1LL)));
                    asmAluRR(a, aluCmp, u8(8LL), RDI, RAX);
                    i32 copy = asmJcc8(a, ccNE);
                    asmLoad(a, u8(1LL), u8(4LL), false, RAX, baseAt(RSI, i32(0LL)));
                    asmBytes(a, {u8(u8(243LL)), u8(u8(170LL))});
                    i32 done = asmJmp8(a);
                    asmHere8(a, copy);
                    asmBytes(a, {u8(u8(243LL)), u8(u8(164LL))});
                    asmHere8(a, done);
                    break;
                }
                case mMov: {
                    if ((((in)->dst == (in)->src) && ((in)->aux == u8(0LL)))) {
                        return;
                    }
                    u8 size = (in)->size;
                    if (((in)->aux == u8(1LL))) {
                        size = u8(4LL);
                    }
                    asmMovRR(a, size, (in)->dst, (in)->src);
                    break;
                }
                case mFMov: {
                    if (((in)->dst != (in)->src)) {
                        asmFmov(a, (in)->dst, (in)->src);
                    }
                    break;
                }
                case mMovImm: {
                    asmMovImm(a, (in)->size, (in)->dst, (in)->imm);
                    break;
                }
                case mLoad: {
                    asmLoad(a, (in)->size, (in)->dsize, ((in)->aux == u8(1LL)), (in)->dst, funcEmitterMem(e, (in)->mem));
                    break;
                }
                case mStore: {
                    asmStore(a, (in)->size, (in)->src, funcEmitterMem(e, (in)->mem));
                    break;
                }
                case mStoreImm: {
                    asmStoreImm(a, (in)->size, funcEmitterMem(e, (in)->mem), (in)->imm);
                    break;
                }
                case mLea: {
                    asmLea(a, (in)->size, (in)->dst, funcEmitterMem(e, (in)->mem));
                    break;
                }
                case mAlu: {
                    asmAluRR(a, convert<AluOp>((in)->aux), (in)->size, (in)->dst, (in)->src);
                    break;
                }
                case mAluImm: {
                    asmAluRI(a, convert<AluOp>((in)->aux), (in)->size, (in)->dst, (in)->imm);
                    break;
                }
                case mAluMem: {
                    asmAluRM(a, convert<AluOp>((in)->aux), (in)->size, (in)->dst, funcEmitterMem(e, (in)->mem));
                    break;
                }
                case mImul3: {
                    asmImul3(a, (in)->size, (in)->dst, (in)->src, (in)->imm);
                    break;
                }
                case mCmp: {
                    asmAluRR(a, aluCmp, (in)->size, (in)->src, (in)->src2);
                    break;
                }
                case mCmpImm: {
                    asmAluRI(a, aluCmp, (in)->size, (in)->src, (in)->imm);
                    break;
                }
                case mCmpMem: {
                    asmAluRM(a, aluCmp, (in)->size, (in)->src, funcEmitterMem(e, (in)->mem));
                    break;
                }
                case mCmpMemImm: {
                    asmAluMI(a, aluCmp, (in)->size, funcEmitterMem(e, (in)->mem), (in)->imm);
                    break;
                }
                case mTest: {
                    asmTest(a, (in)->size, (in)->src, (in)->src2);
                    break;
                }
                case mTestImm: {
                    if (((((in)->aux == u8(1LL)) && ((in)->imm >= i64(0LL))) && ((in)->imm <= i64(255LL)))) {
                        asmOpRR(a, false, Span<const u8>({u8(246LL)}), Int(0LL), convert<i64>((in)->src), true);
                        asmBytes(a, {u8(convert<u8>((in)->imm))});
                    } else {
                        asmTestImm(a, (in)->size, (in)->src, (in)->imm);
                    }
                    break;
                }
                case mSetcc: {
                    asmSetcc(a, (in)->cc, (in)->dst);
                    break;
                }
                case mCmov: {
                    asmCmov(a, (in)->cc, (in)->size, (in)->dst, (in)->src);
                    break;
                }
                case mShiftImm: {
                    asmShiftImm(a, (in)->aux, (in)->size, (in)->dst, (in)->imm);
                    break;
                }
                case mShiftX: {
                    asmShiftX(a, (in)->aux, (in)->size, (in)->dst, (in)->src, (in)->src2);
                    break;
                }
                case mRotCL: {
                    asmShiftCL(a, (in)->aux, (in)->size, (in)->dst);
                    break;
                }
                case mUnary: {
                    asmUnary(a, (in)->aux, (in)->size, (in)->dst);
                    break;
                }
                case mBitCount: {
                    if (((in)->dst != (in)->src)) {
                        asmAluRR(a, aluXor, u8(4LL), (in)->dst, (in)->dst);
                    }
                    asmBitCount(a, (in)->aux, (in)->size, (in)->dst, (in)->src);
                    break;
                }
                case mExt: {
                    asmExt(a, (in)->size, (in)->dsize, ((in)->aux == u8(1LL)), (in)->dst, (in)->src);
                    break;
                }
                case mDiv: {
                    asmDiv(a, ((in)->aux == u8(1LL)), (in)->size, (in)->src);
                    break;
                }
                case mSignExtAx: {
                    if (((in)->size == u8(8LL))) {
                        asmBytes(a, {u8(u8(72LL))});
                    }
                    asmBytes(a, {u8(u8(153LL))});
                    break;
                }
                case mZeroDx: {
                    asmAluRR(a, aluXor, u8(4LL), RDX, RDX);
                    break;
                }
                case mFLoad: {
                    asmFload(a, (in)->size, (in)->dst, funcEmitterMem(e, (in)->mem));
                    break;
                }
                case mFStore: {
                    asmFstore(a, (in)->size, (in)->src, funcEmitterMem(e, (in)->mem));
                    break;
                }
                case mFConst: {
                    if (((in)->imm == i64(0LL))) {
                        asmFxor(a, (in)->dst);
                    } else {
                        asmFload(a, (in)->size, (in)->dst, objCodeFloatConst((e)->oc, convert<u64>((in)->imm), (in)->size));
                    }
                    break;
                }
                case mFAlu: {
                    asmFalu(a, convert<FAluOp>((in)->aux), (in)->size, (in)->dst, (in)->src, (in)->src2);
                    break;
                }
                case mFSelect: {
                    VReg mask = mInstTemps(in)[Int(0LL)];
                    Array<u8, 6> predicates = Array<u8, 6>{{u8(0LL), u8(4LL), u8(17LL), u8(30LL), u8(18LL), u8(29LL)}};
                    asmFcmpMask(a, (in)->dsize, mask, mInstUses(in)[Int(0LL)], mInstUses(in)[Int(1LL)], predicates[(in)->aux]);
                    if ((((in)->dsize == u8(4LL)) && ((in)->size == u8(8LL)))) {
                        asmVexRR(a, pp66, u8(1LL), false, u8(112LL), regNum(mask), Int(-1LL), regNum(mask));
                        asmBytes(a, {u8(u8(0LL))});
                    }
                    asmFblend(a, (in)->size, (in)->dst, (in)->src2, (in)->src, mask);
                    break;
                }
                case mFAluMem: {
                    asmFaluM(a, convert<FAluOp>((in)->aux), (in)->size, (in)->dst, (in)->src, funcEmitterMem(e, (in)->mem));
                    break;
                }
                case mFUnary: {
                    if (((in)->aux == fSqrt)) {
                        asmFsqrt(a, (in)->size, (in)->dst, (in)->src);
                    } else {
                        asmFround(a, (in)->size, (in)->dst, (in)->src, (in)->imm);
                    }
                    break;
                }
                case mFCmp: {
                    asmFcmp(a, (in)->size, (in)->src, (in)->src2);
                    break;
                }
                case mCvtIF: {
                    asmCvtIF(a, (in)->size, (in)->dsize, (in)->dst, (in)->src);
                    break;
                }
                case mCvtFI: {
                    asmCvtFI(a, (in)->size, (in)->dsize, (in)->dst, (in)->src);
                    break;
                }
                case mCvtFF: {
                    asmCvtFF(a, (in)->size, (in)->dst, (in)->src);
                    break;
                }
                case mMovGX: {
                    asmMovGX(a, (in)->size, (in)->dst, (in)->src);
                    break;
                }
                case mMovXG: {
                    asmMovXG(a, (in)->size, (in)->dst, (in)->src);
                    break;
                }
                case mSwitch: {
                    funcEmitterSwitchTable(e, in);
                    break;
                }
                case mCall: {
                    {
                        auto switch698 = (in)->aux;
                        switch (switch698) {
                            case calleeStub: {
                                objCodeCallStub((e)->oc, convert<i64>((in)->imm));
                                break;
                            }
                            case calleeJump: {
                                ((e)->oc)->jumpCalls = appendValues(((e)->oc)->jumpCalls, CallFixup{asmCallRel(a), convert<i32>((in)->imm)});
                                break;
                            }
                            default: {
                                objCodeCallFunc((e)->oc, convert<i64>((in)->imm));
                                break;
                            }
                        }
                    }
                    break;
                }
                case mCallInd: {
                    asmCallReg(a, (in)->src);
                    break;
                }
                case mRet: {
                    if (((e)->frame > i32(0LL))) {
                        asmAluRI(a, aluAdd, u8(8LL), RSP, convert<i64>((e)->frame));
                    }
                    {
                        Int i = (length((e)->pushes) - Int(1LL));
                        for (; (i >= Int(0LL)); i--) {
                            asmPop(a, (e)->pushes[i]);
                        }
                    }
                    if (((e)->mf)->returnsTwice) {
                        asmLea(a, u8(8LL), RSP, rspAt(i32(8LL)));
                    }
                    if ((e)->framed) {
                        asmPop(a, RBP);
                    }
                    asmRet(a);
                    asmSetSP(a, (e)->body);
                    break;
                }
                case mTrap: {
                    funcEmitterTrapJmp(e, (in)->imm);
                    break;
                }
                case mTrapIf: {
                    funcEmitterTrapJcc(e, (in)->cc, (in)->imm);
                    break;
                }
                case mCheck: {
                    funcEmitterCheck(e, in);
                    break;
                }
                case mParMove: {
                    funcEmitterParMove(e, mInstMoves(in));
                    break;
                }
                case mStackArg: {
                    if (vRegIsXMM((in)->src)) {
                        asmFstore(a, u8(8LL), (in)->src, rspAt(convert<i32>((i64(8LL) * (in)->imm))));
                    } else {
                        asmStore(a, u8(8LL), (in)->src, rspAt(convert<i32>((i64(8LL) * (in)->imm))));
                    }
                    break;
                }
                case mLoadArg: {
                    Amem m = rspAt(((e)->incoming + convert<i32>((i64(8LL) * (in)->imm))));
                    if (vRegIsXMM((in)->dst)) {
                        asmFload(a, u8(8LL), (in)->dst, m);
                    } else {
                        asmLoad(a, u8(8LL), u8(8LL), false, (in)->dst, m);
                    }
                    break;
                }
                case mStoreRet: {
                    Amem m = rspAt(((e)->incoming + convert<i32>((i64(8LL) * ((convert<i64>(((e)->mf)->incoming) + (in)->imm))))));
                    if (vRegIsXMM((in)->src)) {
                        asmFstore(a, u8(8LL), (in)->src, m);
                    } else {
                        asmStore(a, u8(8LL), (in)->src, m);
                    }
                    break;
                }
                case mDivOvf: {
                    asmAluRI(a, aluCmp, (in)->size, (in)->src2, i64(-1LL));
                    i32 j = asmJcc8(a, ccNE);
                    asmAluRI(a, aluCmp, (in)->size, (in)->src, i64(1LL));
                    funcEmitterTrapJcc(e, ccO, trapIntOverflow);
                    asmHere8(a, j);
                    break;
                }
                case mTruncF: {
                    funcEmitterTruncF(e, in);
                    break;
                }
                case mFMinMax: {
                    funcEmitterMinMax(e, in);
                    break;
                }
                case mCvtU64F: {
                    funcEmitterCvtU64F(e, in);
                    break;
                }
                default: {
                    compilerError(String("internal error: cannot emit %s"));
                    break;
                }
            }
        }
    }

    static void funcEmitterJumpTo(FuncEmitter* e, Int cc, i32 block) {
        i32 at = {};
        if ((cc < Int(0LL))) {
            at = asmJmpRel((e)->a);
        } else {
            at = asmJccRel((e)->a, convert<u8>(cc));
        }
        (e)->jumps = appendValues((e)->jumps, JumpFixup{at, block});
    }

    static Amem funcEmitterMem(FuncEmitter* e, Mem m) {
        Amem am = memOf(m);
        if (((m).label >= i32(0LL))) {
            (am).label = (m).label;
            (am).labelKind = labelModule;
        }
        return am;
    }

    static void funcEmitterMinMax(FuncEmitter* e, MInst* in) {
        Asm* a = (e)->a;
        auto temporary699 = (in)->size;
        auto temporary700 = (in)->dst;
        auto temporary701 = (in)->src;
        auto temporary702 = (in)->src2;
        u8 size = temporary699;
        VReg d = temporary700;
        VReg x = temporary701;
        VReg y = temporary702;
        asmFcmp(a, size, x, y);
        i32 nan = asmJcc8(a, ccP);
        i32 normal = asmJcc8(a, ccNE);
        if (((in)->aux == u8(1LL))) {
            asmFalu(a, fAnd, size, d, x, y);
        } else {
            asmFalu(a, fOr, size, d, x, y);
        }
        i32 done1 = asmJmp8(a);
        asmHere8(a, normal);
        if (((in)->aux == u8(1LL))) {
            asmFalu(a, fMax, size, d, x, y);
        } else {
            asmFalu(a, fMin, size, d, x, y);
        }
        i32 done2 = asmJmp8(a);
        asmHere8(a, nan);
        asmFalu(a, fAdd, size, d, x, y);
        asmHere8(a, done1);
        asmHere8(a, done2);
    }

    static void funcEmitterMove(FuncEmitter* e, VReg dst, VReg src) {
        Asm* a = (e)->a;
        {
            if ((isSlot(dst) && isSlot(src))) {
                asmPushMem(a, funcEmitterSlotMem(e, slotOf(src)));
                asmPopMem(a, funcEmitterSlotMem(e, slotOf(dst)));
            } else if (isSlot(dst)) {
                if (vRegIsXMM(src)) {
                    asmFstore(a, u8(8LL), src, funcEmitterSlotMem(e, slotOf(dst)));
                } else {
                    asmStore(a, u8(8LL), src, funcEmitterSlotMem(e, slotOf(dst)));
                }
            } else if (isSlot(src)) {
                if (vRegIsXMM(dst)) {
                    asmFload(a, u8(8LL), dst, funcEmitterSlotMem(e, slotOf(src)));
                } else {
                    asmLoad(a, u8(8LL), u8(8LL), false, dst, funcEmitterSlotMem(e, slotOf(src)));
                }
            } else if (vRegIsXMM(dst)) {
                asmFmov(a, dst, src);
            } else {
                asmMovRR(a, u8(8LL), dst, src);
            }
        switchEnd704:;
        }
    }

    // parMove performs moves between registers and stack slots as if all at
    // once.
    static void funcEmitterParMove(FuncEmitter* e, Span<Move> moves) {
        Slice<Move> pending = makeSlice<Move>(Int(0LL), length(moves));
        {
            auto range705 = moves;
            for (Int index706 = 0, rangeEnd707 = length(range705); index706 < rangeEnd707; ++index706) {
                Move m = range705[index706];
                if (((m).dst != (m).src)) {
                    pending = appendValues(pending, m);
                }
            }
        }
        {
            for (; (length(pending) > Int(0LL));) {
                bool progress = false;
                {
                    Int i = Int(0LL);
                    for (; (i < length(pending)); i++) {
                        Move m = pending[i];
                        bool blocked = false;
                        {
                            auto range708 = pending;
                            for (Int index709 = 0, rangeEnd710 = length(range708); index709 < rangeEnd710; ++index709) {
                                Int j = index709;
                                Move o = range708[index709];
                                if (((j != i) && ((o).src == (m).dst))) {
                                    blocked = true;
                                    break;
                                }
                            }
                        }
                        if (blocked) {
                            continue;
                        }
                        funcEmitterMove(e, (m).dst, (m).src);
                        pending = appendSlice(subslice(pending, Int(0), i), subslice(pending, (i + Int(1LL)), length(pending)));
                        i--;
                        progress = true;
                    }
                }
                if (progress) {
                    continue;
                }
                Move m = pending[Int(0LL)];
                funcEmitterSwap(e, (m).dst, (m).src);
                pending = subslice(pending, Int(1LL), length(pending));
                {
                    auto range711 = pending;
                    for (Int index712 = 0, rangeEnd713 = length(range711); index712 < rangeEnd713; ++index712) {
                        Int k = index712;
                        {
                            auto switch714 = (pending[k]).src;
                            if ((switch714 == (m).dst)) {
                                (pending[k]).src = (m).src;
                            } else if ((switch714 == (m).src)) {
                                (pending[k]).src = (m).dst;
                            }
                        switchEnd715:;
                        }
                    }
                }
            }
        }
    }

    static Amem funcEmitterSlotMem(FuncEmitter* e, i32 slot) {
        return rspAt((convert<i32>(((e)->mf)->stackArgs) + (i32(8LL) * slot)));
    }

    static void funcEmitterSwap(FuncEmitter* e, VReg x, VReg y) {
        Asm* a = (e)->a;
        if (isSlot(x)) {
            auto temporary716 = y;
            auto temporary717 = x;
            x = temporary716;
            y = temporary717;
        }
        {
            if (isSlot(x)) {
                auto temporary720 = funcEmitterSlotMem(e, slotOf(x));
                auto temporary721 = funcEmitterSlotMem(e, slotOf(y));
                Amem mx = temporary720;
                Amem my = temporary721;
                asmPushMem(a, mx);
                (my).disp += i32(8LL);
                asmPushMem(a, my);
                (mx).disp += i32(8LL);
                asmPopMem(a, mx);
                (my).disp -= i32(8LL);
                asmPopMem(a, my);
            } else if (isSlot(y)) {
                Amem m = funcEmitterSlotMem(e, slotOf(y));
                asmPushMem(a, m);
                Amem shifted = m;
                (shifted).disp += i32(8LL);
                if (vRegIsXMM(x)) {
                    asmFstore(a, u8(8LL), x, shifted);
                    asmFload(a, u8(8LL), x, rspAt(i32(0LL)));
                    asmLea(a, u8(8LL), RSP, rspAt(i32(8LL)));
                } else {
                    asmStore(a, u8(8LL), x, shifted);
                    asmPop(a, x);
                }
            } else if (vRegIsXMM(x)) {
                asmFalu(a, fXor, u8(8LL), x, x, y);
                asmFalu(a, fXor, u8(8LL), y, y, x);
                asmFalu(a, fXor, u8(8LL), x, x, y);
            } else {
                asmXchg(a, x, y);
            }
        switchEnd719:;
        }
    }

    static void funcEmitterSwitchTable(FuncEmitter* e, MInst* in) {
        Asm* a = (e)->a;
        Int n = (length(mInstTable(in)) - Int(1LL));
        i32 def = mInstTable(in)[n];
        if ((n == Int(0LL))) {
            funcEmitterJumpTo(e, Int(-1LL), def);
            return;
        }
        if (((((in)->aux == u8(0LL)) && (n >= Int(4LL))) && (mInstTable(in)[Int(0LL)] != def))) {
            asmTest(a, u8(4LL), (in)->src, (in)->src);
            funcEmitterJumpTo(e, ccE, mInstTable(in)[Int(0LL)]);
        }
        VReg t0 = mInstTemps(in)[Int(0LL)];
        asmAluRI(a, aluCmp, u8(4LL), (in)->src, convert<i64>(n));
        funcEmitterJumpTo(e, ccAE, def);
        asmRex(a, true, convert<i64>(t0), Int(-1LL), Int(-1LL), false);
        asmBytes(a, {u8(u8(141LL)), u8(u8(shiftLeft(lo3(convert<i64>(t0)), Int(3LL)) | u8(5LL)))});
        i32 leaAt = asmPos(a);
        asmU32(a, u32(0LL));
        asmOpRM(a, false, Span<const u8>({u8(255LL)}), Int(4LL), Amem{t0, (in)->src, u8(8LL), {}, i32(-1LL), {}}, Int(0LL), false);
        (e)->tables = appendValues((e)->tables, TableFixup{leaAt, (leaAt + i32(4LL)), mInstTable(in)});
    }

    static void funcEmitterTrapJcc(FuncEmitter* e, u8 cc, i64 code) {
        (e)->traps = appendValues((e)->traps, TrapFixup{asmJccRel((e)->a, cc), code, ((e)->a)->sp});
    }

    static void funcEmitterTrapJmp(FuncEmitter* e, i64 code) {
        (e)->traps = appendValues((e)->traps, TrapFixup{asmJmpRel((e)->a), code, ((e)->a)->sp});
    }

    static void funcEmitterTruncF(FuncEmitter* e, MInst* in) {
        Asm* a = (e)->a;
        auto temporary722 = (in)->size;
        auto temporary723 = (in)->dsize;
        u8 isize = temporary722;
        u8 fsize = temporary723;
        auto temporary724 = (in)->dst;
        auto temporary725 = (in)->src;
        VReg dst = temporary724;
        VReg src = temporary725;
        bool unsignedValue = (u8((in)->aux & truncUnsigned) != u8(0LL));
        bool sat = (u8((in)->aux & truncSat) != u8(0LL));
        auto temporary726 = truncBounds(isize, fsize);
        u64 high = temporary726.v1;
        if ((!(unsignedValue))) {
            asmCvtFI(a, isize, fsize, dst, src);
            asmAluRI(a, aluCmp, isize, dst, i64(1LL));
            i32 at = asmJccRel(a, ccO);
            i32 resume = asmPos(a);
            (e)->late = appendValues((e)->late, [=]() mutable -> void {
                asmPatch32(a, at, asmPos(a));
                asmFcmp(a, fsize, src, src);
                if (sat) {
                    i32 nan = asmJcc8(a, ccP);
                    asmFcmpM(a, fsize, src, objCodeFloatConst((e)->oc, u64(0LL), fsize));
                    asmPatch32(a, asmJccRel(a, ccBE), resume);
                    asmMovImm(a, isize, dst, maxSigned(isize));
                    asmPatch32(a, asmJmpRel(a), resume);
                    asmHere8(a, nan);
                    asmMovImm(a, u8(4LL), dst, i64(0LL));
                    asmPatch32(a, asmJmpRel(a), resume);
                    return;
                }
                funcEmitterTrapJcc(e, ccP, trapConversion);
                auto temporary727 = truncBounds(isize, fsize);
                u64 low = temporary727.v0;
                bool strict = temporary727.v2;
                asmFcmpM(a, fsize, src, objCodeFloatConst((e)->oc, low, fsize));
                if (strict) {
                    funcEmitterTrapJcc(e, ccBE, trapIntOverflow);
                } else {
                    funcEmitterTrapJcc(e, ccB, trapIntOverflow);
                }
                asmFcmpM(a, fsize, src, objCodeFloatConst((e)->oc, high, fsize));
                funcEmitterTrapJcc(e, ccAE, trapIntOverflow);
                asmPatch32(a, asmJmpRel(a), resume);
            });
            return;
        }
        if ((isize == u8(4LL))) {
            VReg tg = mInstTemps(in)[Int(0LL)];
            asmCvtFI(a, u8(8LL), fsize, dst, src);
            asmMovRR(a, u8(8LL), tg, dst);
            asmShiftImm(a, shiftShr, u8(8LL), tg, i64(32LL));
            i32 at = asmJccRel(a, ccNE);
            i32 resume = asmPos(a);
            (e)->late = appendValues((e)->late, [=]() mutable -> void {
                asmPatch32(a, at, asmPos(a));
                asmFcmp(a, fsize, src, src);
                if (sat) {
                    i32 zero = asmJcc8(a, ccP);
                    asmFcmpM(a, fsize, src, objCodeFloatConst((e)->oc, u64(0LL), fsize));
                    i32 neg = asmJcc8(a, ccB);
                    asmMovImm(a, u8(4LL), dst, i64(4294967295LL));
                    asmPatch32(a, asmJmpRel(a), resume);
                    asmHere8(a, zero);
                    asmHere8(a, neg);
                    asmMovImm(a, u8(4LL), dst, i64(0LL));
                    asmPatch32(a, asmJmpRel(a), resume);
                    return;
                }
                funcEmitterTrapJcc(e, ccP, trapConversion);
                funcEmitterTrapJmp(e, trapIntOverflow);
            });
            return;
        }
        auto temporary728 = mInstTemps(in)[Int(0LL)];
        auto temporary729 = mInstTemps(in)[Int(1LL)];
        VReg tx = temporary728;
        VReg tg = temporary729;
        asmFcmpM(a, fsize, src, objCodeFloatConst((e)->oc, high, fsize));
        i32 big = asmJcc8(a, ccAE);
        asmCvtFI(a, u8(8LL), fsize, dst, src);
        asmTest(a, u8(8LL), dst, dst);
        i32 slowLow = asmJccRel(a, ccS);
        i32 done = asmJmp8(a);
        asmHere8(a, big);
        asmFaluM(a, fSub, fsize, tx, src, objCodeFloatConst((e)->oc, high, fsize));
        asmCvtFI(a, u8(8LL), fsize, dst, tx);
        asmTest(a, u8(8LL), dst, dst);
        i32 slowHigh = asmJccRel(a, ccS);
        asmMovImm(a, u8(8LL), tg, i64((-9223372036854775807LL - 1)));
        asmAluRR(a, aluXor, u8(8LL), dst, tg);
        asmHere8(a, done);
        i32 resume = asmPos(a);
        (e)->late = appendValues((e)->late, [=]() mutable -> void {
            asmPatch32(a, slowLow, asmPos(a));
            if (sat) {
                asmMovImm(a, u8(4LL), dst, i64(0LL));
            } else {
                asmFcmp(a, fsize, src, src);
                funcEmitterTrapJcc(e, ccP, trapConversion);
                funcEmitterTrapJmp(e, trapIntOverflow);
            }
            asmPatch32(a, asmJmpRel(a), resume);
            asmPatch32(a, slowHigh, asmPos(a));
            if (sat) {
                asmMovImm(a, u8(8LL), dst, i64(-1LL));
                asmPatch32(a, asmJmpRel(a), resume);
            } else {
                funcEmitterTrapJmp(e, trapIntOverflow);
            }
        });
    }

    // addRange adds [from, to) during the backward walk, which keeps the ranges
    // in decreasing order: they arrive in decreasing order and merge with the
    // last one when they touch. finish puts them in order.
    template <typename T>
    static void intervalAddRange(T* it, i32 from, i32 to) {
        {
            Int n = length((it)->ranges);
            if ((n > Int(0LL))) {
                Lrange* last = (&((it)->ranges[(n - Int(1LL))]));
                if ((to >= (last)->from)) {
                    if ((from < (last)->from)) {
                        (last)->from = from;
                    }
                    if ((to > (last)->to)) {
                        (last)->to = to;
                    }
                    return;
                }
            }
        }
        (it)->ranges.pushBack(Lrange{from, to});
    }

    // covered is the first position from p on that it covers.
    static i32 intervalCovered(Interval* it, i32 p) {
        Int k = sortSearch(length((it)->ranges), [&](Int i) mutable -> bool {
            return (((it)->ranges[i]).to > p);
        });
        if (((k == length((it)->ranges)) || (((it)->ranges[k]).from <= p))) {
            return p;
        }
        return ((it)->ranges[k]).from;
    }

    static bool intervalCovers(Interval* it, i32 p) {
        if (!length(it->ranges)) {
            return false;
        }
        if (p < it->ranges[0].from || p >= it->end) {
            return false;
        }
        if (length(it->ranges) == 1) {
            return true;
        }
        Int k = sortSearch(length((it)->ranges), [&](Int i) mutable -> bool {
            return (((it)->ranges[i]).to > p);
        });
        return ((k < length((it)->ranges)) && (((it)->ranges[k]).from <= p));
    }

    // coversAt is covers for the scan position: p never decreases between
    // calls.
    static bool intervalCoversAt(Interval* it, i32 p) {
        {
            for (; (((it)->cursor < length((it)->ranges)) && (((it)->ranges[(it)->cursor]).to <= p));) {
                (it)->cursor++;
            }
        }
        return (((it)->cursor < length((it)->ranges)) && (((it)->ranges[(it)->cursor]).from <= p));
    }

    // cutAt shortens the earliest range, which covers a definition at p, to
    // start there; it reports false when no range covers p.
    template <typename T>
    static bool intervalCutAt(T* it, i32 p) {
        Int n = length((it)->ranges);
        if ((((n > Int(0LL)) && (((it)->ranges[(n - Int(1LL))]).from <= p)) && (((it)->ranges[(n - Int(1LL))]).to > p))) {
            ((it)->ranges[(n - Int(1LL))]).from = p;
            return true;
        }
        return false;
    }

    static i32 intervalEnd(Interval* it) {
        return it->end;
    }

    static bool intervalEndsBefore(Interval* it, i32 p) {
        return (intervalEnd(it) <= p);
    }

    // Instructions arrive in descending position order. Within one
    // instruction, definitions and uses may revisit its two positions.
    static void intervalRecordUse(Interval* it, i32 p) {
        auto& uses = it->uses;
        const usize count = uses.size();
        if (!count || p < uses[count - 1]) {
            uses.pushBack(p);
        } else if (p != uses[count - 1]) {
            const i32 last = uses[count - 1];
            require((p & 1) && last == p - 1, "out of order interval use");
            if (count == 1 || uses[count - 2] != p) {
                uses[count - 1] = p;
                uses.pushBack(last);
            }
        }
    }

    static void intervalFinishRanges(InlineVec<Lrange, 1>& ranges) {
        Span<Lrange> r = ranges.view();
        {
            auto temporary730 = Int(0LL);
            auto temporary731 = (length(r) - Int(1LL));
            Int i = temporary730;
            Int j = temporary731;
            for (; (i < j); [&]() {
                auto temporary732 = (i + Int(1LL));
                auto temporary733 = (j - Int(1LL));
                i = temporary732;
                j = temporary733;
            }()) {
                auto temporary734 = r[j];
                auto temporary735 = r[i];
                r[i] = temporary734;
                r[j] = temporary735;
            }
        }
    }

    static void intervalFinish(CopyInterval* it) {
        intervalFinishRanges(it->ranges);
    }

    static void intervalFinish(Interval* it) {
        intervalFinishRanges(it->ranges);
        const auto r = it->ranges.view();
        it->end = length(r) ? r[length(r) - 1].to : 0;
        const usize count = it->uses.size();
        for (usize i = 0; i < count / 2; ++i) {
            const i32 use = it->uses[i];
            it->uses[i] = it->uses[count - 1 - i];
            it->uses[count - 1 - i] = use;
        }
    }

    // intersection returns the first position both intervals cover, or maxPos;
    // it starts from both cursors.
    static i32 intervalIntersection(Interval* it, Interval* o) {
        if (((it)->cursor > length((it)->ranges))) {
            (it)->cursor = length((it)->ranges);
        }
        auto temporary736 = (it)->cursor;
        auto temporary737 = (o)->cursor;
        Int i = temporary736;
        Int j = temporary737;
        {
            for (; ((i < length((it)->ranges)) && (j < length((o)->ranges)));) {
                auto temporary738 = (it)->ranges[i];
                auto temporary739 = (o)->ranges[j];
                Lrange a = temporary738;
                Lrange b = temporary739;
                if (((a).to <= (b).from)) {
                    i++;
                    continue;
                }
                if (((b).to <= (a).from)) {
                    j++;
                    continue;
                }
                if (((a).from > (b).from)) {
                    return (a).from;
                }
                return (b).from;
            }
        }
        return maxPos;
    }

    static i32 intervalNextUse(Interval* it, i32 p) {
        Int k = sortSearch(length((it)->uses), [&](Int i) mutable -> bool {
            return ((it)->uses[i] >= p);
        });
        if ((k < length((it)->uses))) {
            return (it)->uses[k];
        }
        return maxPos;
    }

    // prevUse returns the last use before p, or -1.
    static i32 intervalPrevUse(Interval* it, i32 p) {
        Int k = sortSearch(length((it)->uses), [&](Int i) mutable -> bool {
            return ((it)->uses[i] >= p);
        });
        if ((k > Int(0LL))) {
            return (it)->uses[(k - Int(1LL))];
        }
        return i32(-1LL);
    }

    // split cuts the interval at p: the returned child covers p and later.
    static Interval* intervalSplit(Interval* it, i32 p) {
        Interval* child = newInterval(it->vreg, noReg, it->classValue, it->hint);
        Int k = Int(0LL);
        {
            for (; ((k < length((it)->ranges)) && (((it)->ranges[k]).to <= p));) {
                k++;
            }
        }
        if (((k < length((it)->ranges)) && (((it)->ranges[k]).from < p))) {
            child->ranges.pushBack(Lrange{p, it->ranges[usize(k)].to});
            child->ranges.append(subslice(it->ranges.view(), k + 1, length(it->ranges)));
            it->ranges.resize(usize(k + 1));
            it->ranges[usize(k)].to = p;
        } else {
            child->ranges.assign(subslice(it->ranges.view(), k, length(it->ranges)));
            it->ranges.resize(usize(k));
        }
        child->end = child->ranges.size() ? child->ranges[child->ranges.size() - 1].to : 0;
        it->end = it->ranges.size() ? it->ranges[it->ranges.size() - 1].to : 0;
        if (((it)->cursor > length((it)->ranges))) {
            (it)->cursor = length((it)->ranges);
        }
        Int u = sortSearch(length((it)->uses), [&](Int i) mutable -> bool {
            return ((it)->uses[i] >= p);
        });
        child->uses.assign(subslice(it->uses.view(), u, length(it->uses)));
        it->uses.resize(usize(u));
        Interval* root = it;
        if ((!isNil((it)->parent))) {
            root = (it)->parent;
        }
        (child)->parent = root;
        Int idx = sortSearch(length((root)->children), [&](Int i) mutable -> bool {
            return (intervalStart((root)->children[i]) > p);
        });
        root->children.insert(usize(idx), child);
        return child;
    }

    static i32 intervalStart(Interval* it) {
        return ((it)->ranges[Int(0LL)]).from;
    }

    // address returns the memory operand of an access of width bytes at addr
    // plus the static offset, emitting its bounds check.
    static Mem lowererAddress(Lowerer* l, i32 addr, i64 offset, Int width, bool proven) {
        VReg ra = lowererUse(l, addr);
        i64 end = (offset + convert<i64>(width));
        bool checked = proven;
        if (((length(((l)->m)->mems) > Int(0LL)) && (!((((l)->m)->mems[Int(0LL)]).imported)))) {
            if (isNil((l)->addresses)) {
                (l)->addresses = activePool->make<UnsignedRanges>(UnsignedRanges{(l)->f, makeSlice<UnsignedRange>(length(((l)->f)->values)), makeSlice<bool>(length(((l)->f)->values))});
            }
            checked = (checked || (((unsignedRangesOf((l)->addresses, addr, Int(0LL))).hi + convert<u64>(end)) <= ((((((l)->m)->mems[Int(0LL)]).limits).min * u64(65536LL)) + ((l)->m)->memoryTail)));
        }
        auto temporary740 = lookupMap((l)->checked, addr);
        i64 prior = temporary740.v0;
        bool had = temporary740.v1;
        checked = (checked || (had && (prior >= end)));
        if ((checked && (offset < i64(2147483647LL)))) {
            return memIndexed(memReg, ra, convert<i32>(offset));
        }
        if (((!(had)) || (end > prior))) {
            (l)->checkUndo = appendValues((l)->checkUndo, CheckUndo{addr, prior, had});
            setMap((l)->checked, addr, end);
        }
        if ((end <= boundSlack)) {
            MInst in = inst(mCheck);
            (in).src = ra;
            (in).imm = end;
            in.addressValue = addr;
            lowererEmit(l, in);
            return memIndexed(memReg, ra, convert<i32>(offset));
        }
        VReg t = mFuncNewReg((l)->mf, classGPR);
        MInst mi = inst(mMovImm);
        (mi).dst = t;
        (mi).imm = end;
        (mi).size = u8(8LL);
        lowererEmit(l, mi);
        MInst add = inst(mAlu);
        (add).dst = t;
        (add).src = ra;
        (add).size = u8(8LL);
        (add).aux = u8(0LL);
        lowererEmit(l, add);
        MInst c = inst(mCmpMem);
        (c).src = t;
        (c).mem = memAt(ctxReg, ((l)->lay)->memoryEnd);
        (c).size = u8(8LL);
        lowererEmit(l, c);
        lowererTrapIf(l, ccA, trapMemoryOOB);
        if ((offset < i64(2147483647LL))) {
            return memIndexed(memReg, ra, convert<i32>(offset));
        }
        VReg p = mFuncNewReg((l)->mf, classGPR);
        mi = inst(mMovImm);
        (mi).dst = p;
        (mi).imm = offset;
        (mi).size = u8(8LL);
        lowererEmit(l, mi);
        add = inst(mAlu);
        (add).dst = p;
        (add).src = ra;
        (add).size = u8(8LL);
        (add).aux = u8(0LL);
        lowererEmit(l, add);
        return memIndexed(memReg, p, i32(0LL));
    }

    static void lowererAlu(Lowerer* l, i32 v, Op op, i32 a, i32 b, u8 size) {
        if (((op == OpAdd) && lowererLeaAdd(l, v, a, b, size))) {
            return;
        }
        if ((((op != OpSub) && lowererIsConst(l, a)) && (!(lowererIsConst(l, b))))) {
            auto temporary741 = b;
            auto temporary742 = a;
            a = temporary741;
            b = temporary742;
        }
        VReg d = lowererDef(l, v);
        if (((((op == OpAnd) && (size == u8(8LL))) && lowererIsConst(l, b)) && (convert<u64>(lowererConstBits(l, b)) == u64(4294967295LL)))) {
            MInst in = inst(mMov);
            (in).dst = d;
            (in).src = lowererUse(l, a);
            (in).size = u8(4LL);
            (in).aux = u8(1LL);
            lowererEmit(l, in);
            return;
        }
        {
            auto temporary743 = lowererImm(l, b, size);
            i64 x = temporary743.v0;
            bool ok = temporary743.v1;
            if (ok) {
                {
                    auto switch744 = op;
                    switch (switch744) {
                        case OpAdd:
                        case OpSub: {
                            if ((op == OpSub)) {
                                x = (-(x));
                            }
                            if ((x == convert<i64>(convert<i32>(x)))) {
                                MInst in = inst(mLea);
                                (in).dst = d;
                                (in).mem = memAt(lowererUse(l, a), convert<i32>(x));
                                (in).size = size;
                                lowererEmit(l, in);
                                return;
                            }
                            break;
                        }
                        case OpMul: {
                            MInst in = inst(mImul3);
                            (in).dst = d;
                            (in).src = lowererUse(l, a);
                            (in).imm = x;
                            (in).size = size;
                            lowererEmit(l, in);
                            return;
                            break;
                        }
                    }
                }
                if (((op != OpAdd) && (op != OpSub))) {
                    lowererMov(l, d, lowererUse(l, a), size);
                    MInst in = inst(mAluImm);
                    (in).dst = d;
                    (in).imm = x;
                    (in).size = size;
                    (in).aux = convert<u8>(aluOpcode(op));
                    lowererEmit(l, in);
                    return;
                }
            }
        }
        VReg ra = lowererUse(l, a);
        VReg rb = lowererUse(l, b);
        lowererMov(l, d, ra, size);
        MInst in = inst(mAlu);
        (in).dst = d;
        (in).src = rb;
        (in).size = size;
        (in).aux = convert<u8>(aluOpcode(op));
        lowererEmit(l, in);
    }

    // call calls a function of the module; an imported one through its
    // adapter, which keeps the internal convention, the memory register
    // included.
    static void lowererCall(Lowerer* l, i32 v, Int index, Span<const i32> args) {
        FuncType* t = moduleFuncType((l)->m, convert<u32>(index));
        auto temporary745 = lowererPlaceArgs(l, args);
        Slice<VReg> uses = temporary745.v0;
        Int stack = temporary745.v1;
        Slice<VReg> regs = resultRegs((t)->results);
        lowererOutgoing(l, stack, regs);
        MInst in = inst(mCall);
        (in).aux = calleeFunc;
        (in).imm = convert<i64>(index);
        mInstExtra(&in)->uses.assign(uses);
        mInstExtra(&in)->defs.assign(assigned(regs));
        ((l)->mf)->hasCalls = true;
        if ((((l)->m)->funcs[index]).returnsTwice) {
            (in).aux = calleeJump;
            ((l)->mf)->returnsTwice = true;
        }
        lowererEmit(l, in);
        lowererTakeResults(l, v, (t)->results, regs, stack);
    }

    static void lowererCallIndirect(Lowerer* l, i32 v, Int typeIndex, Int table, Span<const i32> args) {
        FuncType* t = (&(((l)->m)->types[typeIndex]));
        i32 elem = args[(length(args) - Int(1LL))];
        args = subslice(args, Int(0), (length(args) - Int(1LL)));
        i32 tb = ((l)->lay)->tables[table];
        VReg re = lowererUse(l, elem);
        MInst c = inst(mCmpMem);
        (c).src = re;
        (c).mem = memAt(ctxReg, (tb + tableSize));
        (c).size = u8(4LL);
        lowererEmit(l, c);
        lowererTrapIf(l, ccAE, trapUndefinedElem);
        VReg entries = lowererLoad64(l, memAt(ctxReg, (tb + tableEntries)));
        VReg desc = lowererLoad64(l, Mem{entries, re, u8(8LL), {}, i32(-1LL)});
        MInst tst = inst(mTest);
        (tst).src = desc;
        (tst).src2 = desc;
        (tst).size = u8(8LL);
        lowererEmit(l, tst);
        lowererTrapIf(l, ccE, trapUninitElem);
        i64 id = convert<i64>(typeID(t));
        if ((id == convert<i64>(convert<i32>(id)))) {
            MInst ci = inst(mCmpMemImm);
            (ci).mem = memAt(desc, i32(8LL));
            (ci).imm = id;
            (ci).size = u8(8LL);
            lowererEmit(l, ci);
        } else {
            MInst cm = inst(mCmpMem);
            (cm).src = lowererConstReg(l, id, u8(8LL));
            (cm).mem = memAt(desc, i32(8LL));
            (cm).size = u8(8LL);
            lowererEmit(l, cm);
        }
        lowererTrapIf(l, ccNE, trapIndirectType);
        VReg code = lowererLoad64(l, memAt(desc, i32(0LL)));
        auto temporary746 = lowererPlaceArgs(l, args);
        Slice<VReg> uses = temporary746.v0;
        Int stack = temporary746.v1;
        Slice<VReg> regs = resultRegs((t)->results);
        lowererOutgoing(l, stack, regs);
        MInst in = inst(mCallInd);
        (in).src = code;
        mInstExtra(&in)->uses.assign(uses);
        mInstExtra(&in)->defs.assign(assigned(regs));
        ((l)->mf)->hasCalls = true;
        lowererEmit(l, in);
        lowererTakeResults(l, v, (t)->results, regs, stack);
    }

    // checkRange traps unless a + n <= limit, all unsigned 32-bit values
    // summed in 64 bits; limit is a memory operand.
    static void lowererCheckRange(Lowerer* l, VReg a, VReg n, Mem limit, u8 limitSize, i64 code) {
        VReg t = mFuncNewReg((l)->mf, classGPR);
        lowererMov(l, t, a, u8(8LL));
        MInst add = inst(mAlu);
        (add).dst = t;
        (add).src = n;
        (add).size = u8(8LL);
        (add).aux = u8(0LL);
        lowererEmit(l, add);
        if ((limitSize == u8(8LL))) {
            MInst c = inst(mCmpMem);
            (c).src = t;
            (c).mem = limit;
            (c).size = u8(8LL);
            lowererEmit(l, c);
        } else {
            VReg lim = mFuncNewReg((l)->mf, classGPR);
            MInst ld = inst(mLoad);
            (ld).dst = lim;
            (ld).mem = limit;
            (ld).size = limitSize;
            (ld).dsize = u8(8LL);
            lowererEmit(l, ld);
            MInst c = inst(mCmp);
            (c).src = t;
            (c).src2 = lim;
            (c).size = u8(8LL);
            lowererEmit(l, c);
        }
        lowererTrapIf(l, ccA, code);
    }

    static u8 lowererClassOf(Lowerer* l, ValType t) {
        if ((isFloat(t) || (t == V128))) {
            return classXMM;
        }
        return classGPR;
    }

    // compare emits the flags for comparison c and returns the condition that
    // holds when it is true. A float equality needs the parity flag as well:
    // parity is then reported through the second result, 1 when an unordered
    // result must make the comparison false (eq) and 2 when it must make it
    // true (ne).
    static Tuple2<u8, Int> lowererCompare(Lowerer* l, i32 c) {
        Function* f = (l)->f;
        Value* val = (&((f)->values[c]));
        Span<i32> args = functionArgs(f, c);
        if (((val)->op == OpAnd)) {
            lowererTestBits(l, args[Int(0LL)], args[Int(1LL)]);
            return {ccNE, Int(0LL)};
        }
        if ((((val)->op == OpXor) || ((val)->op == OpSub))) {
            MInst in = inst(mCmp);
            (in).src = lowererUse(l, args[Int(0LL)]);
            (in).src2 = lowererUse(l, args[Int(1LL)]);
            (in).size = typeSize(((f)->values[args[Int(0LL)]]).typ);
            lowererEmit(l, in);
            return {ccNE, Int(0LL)};
        }
        if (((val)->op == OpOr)) {
            lowererLowerValue(l, c);
            return {ccNE, Int(0LL)};
        }
        if (((val)->op == OpEqz)) {
            i32 v = args[Int(0LL)];
            if (((l)->deferred[v] && (((f)->values[v]).op == OpAnd))) {
                Span<i32> bits = functionArgs(f, v);
                lowererTestBits(l, bits[Int(0LL)], bits[Int(1LL)]);
                return {ccE, Int(0LL)};
            }
            VReg a = lowererUse(l, args[Int(0LL)]);
            MInst in = inst(mTest);
            (in).src = a;
            (in).src2 = a;
            (in).size = typeSize(((f)->values[args[Int(0LL)]]).typ);
            lowererEmit(l, in);
            return {ccE, Int(0LL)};
        }
        auto temporary747 = args[Int(0LL)];
        auto temporary748 = args[Int(1LL)];
        i32 a = temporary747;
        i32 b = temporary748;
        u8 size = typeSize(((f)->values[a]).typ);
        if (((val)->op >= OpFEq)) {
            Op op = (val)->op;
            if (((op == OpFLt) || (op == OpFLe))) {
                auto temporary749 = b;
                auto temporary750 = a;
                a = temporary749;
                b = temporary750;
            }
            MInst in = inst(mFCmp);
            (in).src = lowererUse(l, a);
            (in).src2 = lowererUse(l, b);
            (in).size = size;
            lowererEmit(l, in);
            {
                auto switch751 = op;
                switch (switch751) {
                    case OpFEq: {
                        return {ccE, Int(1LL)};
                        break;
                    }
                    case OpFNe: {
                        return {ccNE, Int(2LL)};
                        break;
                    }
                    case OpFLt:
                    case OpFGt: {
                        return {ccA, Int(0LL)};
                        break;
                    }
                }
            }
            return {ccAE, Int(0LL)};
        }
        u8 cc = intCC[Op((val)->op - OpEq)];
        if ((lowererIsConst(l, a) && (!(lowererIsConst(l, b))))) {
            auto temporary752 = b;
            auto temporary753 = a;
            a = temporary752;
            b = temporary753;
            cc = swapCC(cc);
        }
        {
            auto temporary754 = lowererImm(l, b, size);
            i64 x = temporary754.v0;
            bool ok = temporary754.v1;
            if (ok) {
                MInst in = inst(mCmpImm);
                (in).src = lowererUse(l, a);
                (in).imm = x;
                (in).size = size;
                if (((x == i64(0LL)) && (((cc == ccE) || (cc == ccNE))))) {
                    (in).op = mTest;
                    (in).src2 = (in).src;
                }
                lowererEmit(l, in);
                return {cc, Int(0LL)};
            }
        }
        MInst in = inst(mCmp);
        (in).src = lowererUse(l, a);
        (in).src2 = lowererUse(l, b);
        (in).size = size;
        lowererEmit(l, in);
        return {cc, Int(0LL)};
    }

    static i64 lowererConstBits(Lowerer* l, i32 v) {
        if (((((l)->f)->values[v]).op == OpConst)) {
            return (((l)->f)->values[v]).aux;
        }
        return i64(0LL);
    }

    static VReg lowererConstReg(Lowerer* l, i64 x, u8 size) {
        VReg r = mFuncNewReg((l)->mf, classGPR);
        MInst in = inst(mMovImm);
        (in).dst = r;
        (in).imm = x;
        (in).size = size;
        lowererEmit(l, in);
        return r;
    }

    static void lowererConvert(Lowerer* l, i32 v, Op op, i32 a, u8 size) {
        ValType srcType = (((l)->f)->values[a]).typ;
        VReg src = lowererUse(l, a);
        VReg d = lowererDef(l, v);
        if (((op == OpConvertU) && (srcType == I64))) {
            MInst in = inst(mCvtU64F);
            (in).dst = d;
            (in).src = src;
            (in).dsize = size;
            (in).size = u8(8LL);
            mInstExtra(&in)->temps.assign(Span<const VReg>({mFuncNewReg((l)->mf, classGPR), mFuncNewReg((l)->mf, classGPR)}));
            lowererEmit(l, in);
            return;
        }
        MInst in = inst(mCvtIF);
        (in).dst = d;
        (in).src = src;
        (in).dsize = size;
        (in).size = typeSize(srcType);
        if ((op == OpConvertU)) {
            (in).size = u8(8LL);
            if (isNil((l)->addresses)) {
                (l)->addresses = activePool->make<UnsignedRanges>(UnsignedRanges{(l)->f, makeSlice<UnsignedRange>(length(((l)->f)->values)), makeSlice<bool>(length(((l)->f)->values))});
            }
            if (((unsignedRangesOf((l)->addresses, a, Int(0LL))).hi <= u64(2147483647LL))) {
                (in).size = u8(4LL);
            }
        }
        lowererEmit(l, in);
    }

    static void lowererCopysign(Lowerer* l, i32 v, i32 a, i32 b, u8 size) {
        auto temporary755 = i32(0LL);
        auto temporary756 = i32(1LL);
        i32 sign = temporary755;
        i32 abs = temporary756;
        if ((size == u8(8LL))) {
            auto temporary757 = constF64Sign;
            auto temporary758 = constF64Abs;
            sign = temporary757;
            abs = temporary758;
        }
        VReg t1 = mFuncNewReg((l)->mf, classXMM);
        MInst in = inst(mFAluMem);
        (in).dst = t1;
        (in).src = lowererUse(l, b);
        (in).mem = Mem{noReg, noReg, {}, {}, sign};
        (in).aux = u8(6LL);
        (in).size = size;
        lowererEmit(l, in);
        VReg t2 = mFuncNewReg((l)->mf, classXMM);
        in = inst(mFAluMem);
        (in).dst = t2;
        (in).src = lowererUse(l, a);
        (in).mem = Mem{noReg, noReg, {}, {}, abs};
        (in).aux = u8(6LL);
        (in).size = size;
        lowererEmit(l, in);
        MInst orValue = inst(mFAlu);
        (orValue).dst = lowererDef(l, v);
        (orValue).src = t1;
        (orValue).src2 = t2;
        (orValue).aux = u8(8LL);
        (orValue).size = size;
        lowererEmit(l, orValue);
    }

    static void lowererCountUses(Lowerer* l) {
        Function* f = (l)->f;
        l->uses.resize(f->values.size());
        l->uses.fill(0);
        {
            auto range759 = ((l)->ci)->order;
            for (Int index760 = 0, rangeEnd761 = length(range759); index760 < rangeEnd761; ++index760) {
                i32 b = range759[index760];
                Block* blk = (&((f)->blocks[b]));
                {
                    auto range762 = (blk)->instrs.view();
                    for (Int index763 = 0, rangeEnd764 = length(range762); index763 < rangeEnd764; ++index763) {
                        i32 v = range762[index763];
                        {
                            auto range765 = functionArgs(f, v);
                            for (Int index766 = 0, rangeEnd767 = length(range765); index766 < rangeEnd767; ++index766) {
                                i32 a = range765[index766];
                                (l)->uses[a]++;
                            }
                        }
                    }
                }
                Term* t = (&((blk)->term));
                if ((((t)->kind == TermIf) || ((t)->kind == TermSwitch))) {
                    (l)->uses[(t)->value]++;
                }
                {
                    auto range768 = (t)->values;
                    for (Int index769 = 0, rangeEnd770 = length(range768); index769 < rangeEnd770; ++index769) {
                        i32 v = range768[index769];
                        (l)->uses[v]++;
                    }
                }
                {
                    auto range771 = (t)->edges;
                    for (Int index772 = 0, rangeEnd773 = length(range771); index772 < rangeEnd773; ++index772) {
                        Edge e = range771[index772];
                        {
                            auto range774 = (e).args;
                            for (Int index775 = 0, rangeEnd776 = length(range774); index775 < rangeEnd776; ++index775) {
                                i32 a = range774[index775];
                                (l)->uses[a]++;
                            }
                        }
                    }
                }
            }
        }
    }

    static VReg lowererDef(Lowerer* l, i32 v) {
        if (((l)->vregs[v] == noReg)) {
            (l)->vregs[v] = mFuncNewReg((l)->mf, lowererClassOf(l, (((l)->f)->values[v]).typ));
        }
        return (l)->vregs[v];
    }

    static void lowererDivide(Lowerer* l, i32 v, Op op, i32 a, i32 b, u8 size) {
        VReg ra = lowererUse(l, a);
        VReg rb = lowererUse(l, b);
        MInst t = inst(mTest);
        (t).src = rb;
        (t).src2 = rb;
        (t).size = size;
        lowererEmit(l, t);
        lowererTrapIf(l, ccE, trapDivByZero);
        bool signedValue = ((op == OpDivS) || (op == OpRemS));
        if ((op == OpDivS)) {
            MInst in = inst(mDivOvf);
            (in).src = ra;
            (in).src2 = rb;
            (in).size = size;
            lowererEmit(l, in);
        }
        if ((op == OpRemS)) {
            VReg one = mFuncNewReg((l)->mf, classGPR);
            MInst mi = inst(mMovImm);
            (mi).dst = one;
            (mi).imm = i64(1LL);
            (mi).size = u8(4LL);
            lowererEmit(l, mi);
            VReg divisor = mFuncNewReg((l)->mf, classGPR);
            lowererMov(l, divisor, rb, size);
            MInst c = inst(mCmpImm);
            (c).src = rb;
            (c).imm = i64(-1LL);
            (c).size = size;
            lowererEmit(l, c);
            MInst cm = inst(mCmov);
            (cm).dst = divisor;
            (cm).src = one;
            (cm).cc = ccE;
            (cm).size = size;
            lowererEmit(l, cm);
            rb = divisor;
        }
        lowererMov(l, RAX, ra, size);
        if (signedValue) {
            MInst in = inst(mSignExtAx);
            (in).size = size;
            mInstExtra(&in)->uses.assign(Span<const VReg>({RAX}));
            mInstExtra(&in)->defs.assign(Span<const VReg>({RDX}));
            lowererEmit(l, in);
        } else {
            MInst in = inst(mZeroDx);
            mInstExtra(&in)->defs.assign(Span<const VReg>({RDX}));
            lowererEmit(l, in);
        }
        MInst in = inst(mDiv);
        (in).src = rb;
        (in).size = size;
        mInstExtra(&in)->uses.assign(Span<const VReg>({RAX, RDX}));
        mInstExtra(&in)->defs.assign(Span<const VReg>({RAX, RDX}));
        if (signedValue) {
            (in).aux = u8(1LL);
        }
        lowererEmit(l, in);
        if (((op == OpDivS) || (op == OpDivU))) {
            lowererMov(l, lowererDef(l, v), RAX, size);
        } else {
            lowererMov(l, lowererDef(l, v), RDX, size);
        }
    }

    // edgeBlock returns the MIR block a branch along edge i of SSA block b
    // targets, placing the parameter moves on a block of their own when b has
    // other successors.
    static i32 lowererEdgeBlock(Lowerer* l, i32 b, Int i) {
        Edge* e = (&(((((l)->f)->blocks[b]).term).edges[i]));
        i32 target = (l)->mblk[(e)->block];
        if (((length((e)->args) == Int(0LL)) && (length((((l)->f)->blocks[(e)->block]).preds) <= Int(1LL)))) {
            return target;
        }
        Array<i32, 2> key = Array<i32, 2>{{b, convert<i32>(i)}};
        {
            auto temporary777 = lookupMap((l)->split, key);
            i32 s = temporary777.v0;
            bool ok = temporary777.v1;
            if (ok) {
                return s;
            }
        }
        i32 s = convert<i32>(length(((l)->mf)->blocks));
        ((l)->mf)->blocks.pushBack(MBlock{{}, {}, {}, (((l)->mf)->blocks[(l)->cur]).depth, i32(-1LL), {}});
        setMap((l)->split, key, s);
        i32 saved = (l)->cur;
        (l)->cur = s;
        lowererEdgeMoves(l, e);
        MInst j = inst(mJmp);
        (j).target = target;
        lowererEmit(l, j);
        (l)->cur = saved;
        return s;
    }

    static void lowererEdgeMoves(Lowerer* l, Edge* e) {
        if ((length((e)->args) == Int(0LL))) {
            return;
        }
        Slice<i32> params = (((l)->f)->blocks[(e)->block]).params;
        MInst in = inst(mParMove);
        {
            auto range778 = (e)->args;
            for (Int index779 = 0, rangeEnd780 = length(range778); index779 < rangeEnd780; ++index779) {
                Int i = index779;
                i32 a = range778[index779];
                VReg src = lowererUse(l, a);
                VReg dst = (l)->vregs[params[i]];
                if ((src != dst)) {
                    mInstExtra(&in)->moves.pushBack(Move{dst, src});
                }
            }
        }
        if ((length(mInstMoves(&in)) > Int(0LL))) {
            lowererEmit(l, in);
        }
    }

    static MInst* lowererEmit(Lowerer* l, MInst in) {
        MBlock* blk = (&(((l)->mf)->blocks[(l)->cur]));
        (blk)->insts.pushBack(in);
        return (&((blk)->insts[(length((blk)->insts) - Int(1LL))]));
    }

    static void lowererEntry(Lowerer* l) {
        Function* f = (l)->f;
        Slice<i32> params = ((f)->blocks[((l)->ci)->order[Int(0LL)]]).params;
        auto temporary781 = Int(0LL);
        auto temporary782 = Int(0LL);
        Int gi = temporary781;
        Int xi = temporary782;
        Int stack = Int(0LL);
        MInst pm = inst(mParMove);
        {
            auto range783 = params;
            for (Int index784 = 0, rangeEnd785 = length(range783); index784 < rangeEnd785; ++index784) {
                i32 p = range783[index784];
                ValType t = ((f)->values[p]).typ;
                VReg r = (l)->vregs[p];
                if (isFloat(t)) {
                    if ((xi < length(argXMMs))) {
                        mInstExtra(&pm)->moves.pushBack(Move{r, argXMMs[xi]});
                        xi++;
                        continue;
                    }
                } else if ((gi < length(argGPRs))) {
                    mInstExtra(&pm)->moves.pushBack(Move{r, argGPRs[gi]});
                    gi++;
                    continue;
                }
                MInst in = inst(mLoadArg);
                (in).dst = r;
                (in).imm = convert<i64>(stack);
                (in).size = typeSize(t);
                lowererEmit(l, in);
                stack++;
            }
        }
        if ((length(mInstMoves(&pm)) > Int(0LL))) {
            lowererEmit(l, pm);
        }
        ((l)->mf)->incoming = stack;
    }

    // extracts lists the OpExtract values of a multi-value call; the frontend
    // emits them right after it.
    static Slice<i32> lowererExtracts(Lowerer* l, i32 call) {
        Function* f = (l)->f;
        Block* blk = (&((f)->blocks[((f)->values[call]).block]));
        Slice<i32> out = {};
        {
            auto range786 = (blk)->instrs.view();
            for (Int index787 = 0, rangeEnd788 = length(range786); index787 < rangeEnd788; ++index787) {
                Int i = index787;
                i32 v = range786[index787];
                if ((v != call)) {
                    continue;
                }
                {
                    auto range789 = subslice((blk)->instrs.view(), (i + Int(1LL)), length((blk)->instrs));
                    for (Int index790 = 0, rangeEnd791 = length(range789); index790 < rangeEnd791; ++index790) {
                        i32 w = range789[index790];
                        if (((((f)->values[w]).op != OpExtract) || (functionArgs(f, w)[Int(0LL)] != call))) {
                            break;
                        }
                        out = appendValues(out, w);
                    }
                }
            }
        }
        return out;
    }

    // Saturation is unnecessary when the input is in the signed destination's
    // range on every path that observes the result. A select may discard an
    // out-of-range conversion; raw x86 conversion is non-trapping too, so its
    // unobserved result does not matter. Trapping wasm conversions stay intact.
    static void lowererFindRawTruncations(Lowerer* l) {
        Function* f = (l)->f;
        l->rawTrunc.resize(f->values.size());
        l->rawTrunc.fill(false);
        Slice<i32> user = makeSlice<i32>(length((f)->values));
        {
            auto range792 = user;
            for (Int index793 = 0, rangeEnd794 = length(range792); index793 < rangeEnd794; ++index793) {
                Int v = index793;
                user[v] = i32(-1LL);
            }
        }
        {
            auto range795 = ((l)->ci)->order;
            for (Int index796 = 0, rangeEnd797 = length(range795); index796 < rangeEnd797; ++index796) {
                i32 b = range795[index796];
                {
                    auto range798 = ((f)->blocks[b]).instrs.view();
                    for (Int index799 = 0, rangeEnd800 = length(range798); index799 < rangeEnd800; ++index799) {
                        i32 v = range798[index799];
                        {
                            auto range801 = functionArgs(f, v);
                            for (Int index802 = 0, rangeEnd803 = length(range801); index802 < rangeEnd803; ++index802) {
                                i32 a = range801[index802];
                                user[a] = v;
                            }
                        }
                    }
                }
            }
        }
        {
            auto& range804 = (f)->values;
            for (Int index805 = 0, rangeEnd806 = length(range804); index805 < rangeEnd806; ++index805) {
                Int v = index805;
                Value* val = (&((f)->values[v]));
                if (((((((val)->op != OpTruncSatS) && ((val)->op != OpTruncSatU)) || ((val)->block < i32(0LL))) || (((l)->ci)->rpoIndex[(val)->block] < i32(0LL))) || ((l)->uses[v] == i32(0LL)))) {
                    continue;
                }
                FloatRanges* r = activePool->make<FloatRanges>(FloatRanges{f, mapOf<i32, FloatRange>({}), mapOf<i32, FloatRange>({})});
                i32 b = (val)->block;
                {
                    i32 p = ((l)->ci)->idom[b];
                    for (; (p != b);) {
                        Term* t = (&(((f)->blocks[p]).term));
                        if ((((t)->kind == TermIf) && (length((t)->edges) == Int(2LL)))) {
                            bool a = cfgInfoDominates((l)->ci, ((t)->edges[Int(0LL)]).block, b);
                            bool z = cfgInfoDominates((l)->ci, ((t)->edges[Int(1LL)]).block, b);
                            if ((a != z)) {
                                floatRangesConstrain(r, (t)->value, a);
                            }
                        }
                        if ((((l)->ci)->idom[p] == p)) {
                            break;
                        }
                        p = ((l)->ci)->idom[p];
                    }
                }
                if ((((l)->uses[v] == i32(1LL)) && (user[v] >= i32(0LL)))) {
                    i32 u = user[v];
                    if ((((f)->values[u]).op == OpSelect)) {
                        Span<i32> args = functionArgs(f, u);
                        if ((args[Int(1LL)] != args[Int(2LL)])) {
                            floatRangesConstrain(r, args[Int(0LL)], (args[Int(1LL)] == convert<i32>(v)));
                        }
                    }
                }
                FloatRange bound = floatRangesOf(r, functionArgs(f, convert<i32>(v))[Int(0LL)], Int(0LL));
                double hi = mathLdexp(double(1.0), ((convert<i64>(typeSize((val)->typ)) * Int(8LL)) - Int(1LL)));
                double lo = (-(hi));
                if (((val)->op == OpTruncSatU)) {
                    lo = double(0.0);
                }
                (l)->rawTrunc[v] = ((((!((bound).nan)) && ((bound).lo <= (bound).hi)) && ((bound).lo >= lo)) && ((bound).hi < hi));
            }
        }
    }

    // floatBits materializes a select's alternatives in the class cmov uses.
    // Constants need no trip through an xmm register, and their NaN payloads
    // and signs of zero stay intact.
    static VReg lowererFloatBits(Lowerer* l, i32 v, u8 size) {
        VReg r = mFuncNewReg((l)->mf, classGPR);
        MInst in = inst(mMovXG);
        (in).dst = r;
        (in).size = size;
        if (lowererIsConst(l, v)) {
            (in).op = mMovImm;
            (in).imm = lowererConstBits(l, v);
        } else {
            (in).src = lowererUse(l, v);
        }
        lowererEmit(l, in);
        return r;
    }

    static Mem lowererGlobalAddr(Lowerer* l, Int index) {
        if ((((l)->m)->globals[index]).imported) {
            VReg p = mFuncNewReg((l)->mf, classGPR);
            MInst in = inst(mLoad);
            (in).dst = p;
            (in).mem = memAt(ctxReg, ((l)->lay)->globals[index]);
            (in).size = u8(8LL);
            (in).dsize = u8(8LL);
            lowererEmit(l, in);
            return memAt(p, i32(0LL));
        }
        return memAt(ctxReg, ((l)->lay)->globals[index]);
    }

    static void lowererGlobalGet(Lowerer* l, i32 v, Int index) {
        ValType t = (((l)->m)->globals[index]).typ;
        Mem mem = lowererGlobalAddr(l, index);
        MInst in = inst(mLoad);
        if (isFloat(t)) {
            (in).op = mFLoad;
        }
        (in).dst = lowererDef(l, v);
        (in).mem = mem;
        (in).size = typeSize(t);
        (in).dsize = (in).size;
        lowererEmit(l, in);
    }

    static void lowererGlobalSet(Lowerer* l, Int index, i32 value) {
        ValType t = (((l)->m)->globals[index]).typ;
        Mem mem = lowererGlobalAddr(l, index);
        {
            auto temporary807 = lowererImm(l, value, typeSize(t));
            i64 x = temporary807.v0;
            bool ok = temporary807.v1;
            if (ok) {
                MInst in = inst(mStoreImm);
                (in).mem = mem;
                (in).imm = x;
                (in).size = typeSize(t);
                lowererEmit(l, in);
                return;
            }
        }
        MInst in = inst(mStore);
        if (isFloat(t)) {
            (in).op = mFStore;
        }
        (in).mem = mem;
        (in).src = lowererUse(l, value);
        (in).size = typeSize(t);
        lowererEmit(l, in);
    }

    // imm reports whether v is a constant usable as an immediate of an
    // instruction of the given width.
    static Tuple2<i64, bool> lowererImm(Lowerer* l, i32 v, u8 size) {
        if (((!(lowererIsConst(l, v))) || isFloat((((l)->f)->values[v]).typ))) {
            return {i64(0LL), false};
        }
        i64 x = lowererConstBits(l, v);
        if ((size == u8(4LL))) {
            return {convert<i64>(convert<i32>(x)), true};
        }
        if ((x == convert<i64>(convert<i32>(x)))) {
            return {x, true};
        }
        return {i64(0LL), false};
    }

    static bool lowererIsConst(Lowerer* l, i32 v) {
        Op op = (((l)->f)->values[v]).op;
        return (((op == OpConst) || (op == OpUndef)) || (op == OpRefNull));
    }

    // LEA computes a sum in one instruction with an independent destination.
    // Its 32-bit form preserves wasm's wrapping arithmetic even when the full
    // effective address overflows, and can absorb a single-use scaled index.
    static bool lowererLeaAdd(Lowerer* l, i32 v, i32 a, i32 b, u8 size) {
        Function* f = (l)->f;
        u8 scale = u8(1LL);
        bool folded = false;
        if (((l)->deferred[a] && (((f)->values[a]).op == OpShl))) {
            auto temporary808 = b;
            auto temporary809 = a;
            a = temporary808;
            b = temporary809;
        }
        if (((l)->deferred[b] && (((f)->values[b]).op == OpShl))) {
            Span<i32> args = functionArgs(f, b);
            scale = shiftLeft(u8(1LL), convert<Uint>((lowererConstBits(l, args[Int(1LL)]) & convert<i64>(u8(u8(size * u8(8LL)) - u8(1LL))))));
            b = args[Int(0LL)];
            folded = true;
        }
        Mem mem = Mem{noReg, noReg, scale, {}, i32(-1LL)};
        {
            auto temporary810 = lowererImm(l, a, size);
            i64 x = temporary810.v0;
            bool ok = temporary810.v1;
            if ((((ok && (x >= i64(-2147483648LL))) && (x <= i64(2147483647LL))) && folded)) {
                (mem).disp = convert<i32>(x);
                (mem).index = lowererUse(l, b);
            } else if (((!(lowererIsConst(l, a))) && (!(lowererIsConst(l, b))))) {
                (mem).base = lowererUse(l, a);
                (mem).index = lowererUse(l, b);
            } else {
                return false;
            }
        }
        MInst in = inst(mLea);
        (in).dst = lowererDef(l, v);
        (in).size = size;
        (in).mem = mem;
        lowererEmit(l, in);
        return true;
    }

    // linkBlocks fills successor and predecessor lists from the branches.
    static void lowererLinkBlocks(Lowerer* l) {
        MFunc* mf = (l)->mf;
        {
            auto& range811 = (mf)->blocks;
            for (Int index812 = 0, rangeEnd813 = length(range811); index812 < rangeEnd813; ++index812) {
                Int b = index812;
                MBlock* blk = (&((mf)->blocks[b]));
                {
                    auto range814 = (blk)->insts.view();
                    for (Int index815 = 0, rangeEnd816 = length(range814); index815 < rangeEnd816; ++index815) {
                        Int i = index815;
                        MInst* in = (&((blk)->insts[i]));
                        {
                            auto switch817 = (in)->op;
                            switch (switch817) {
                                case mJmp:
                                case mJcc: {
                                    (blk)->succs = appendUnique((blk)->succs, (in)->target);
                                    break;
                                }
                                case mSwitch: {
                                    {
                                        auto range818 = mInstTable(in);
                                        for (Int index819 = 0, rangeEnd820 = length(range818); index819 < rangeEnd820; ++index819) {
                                            i32 t = range818[index819];
                                            (blk)->succs = appendUnique((blk)->succs, t);
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
        {
            auto& range821 = (mf)->blocks;
            for (Int index822 = 0, rangeEnd823 = length(range821); index822 < rangeEnd823; ++index822) {
                Int b = index822;
                {
                    auto range824 = ((mf)->blocks[b]).succs;
                    for (Int index825 = 0, rangeEnd826 = length(range824); index825 < rangeEnd826; ++index825) {
                        i32 s = range824[index825];
                        ((mf)->blocks[s]).preds = appendValues(((mf)->blocks[s]).preds, convert<i32>(b));
                    }
                }
            }
        }
    }

    static void lowererLoad(Lowerer* l, i32 v, i32 addr) {
        Value* val = (&(((l)->f)->values[v]));
        MemKind kind = convert<MemKind>((val)->aux2);
        Mem mem = lowererAddress(l, addr, (val)->aux, memKindWidth(kind), (MemKind(kind & memProven) != MemKind(0LL)));
        MInst in = inst(mLoad);
        if (isFloat((val)->typ)) {
            (in).op = mFLoad;
        }
        (in).dst = lowererDef(l, v);
        (in).mem = mem;
        (in).size = convert<u8>(memKindWidth(kind));
        (in).dsize = typeSize((val)->typ);
        if (memKindSigned(kind)) {
            (in).aux = u8(1LL);
        }
        lowererEmit(l, in);
    }

    static VReg lowererLoad64(Lowerer* l, Mem mem) {
        VReg r = mFuncNewReg((l)->mf, classGPR);
        MInst in = inst(mLoad);
        (in).dst = r;
        (in).mem = mem;
        (in).size = u8(8LL);
        (in).dsize = u8(8LL);
        lowererEmit(l, in);
        return r;
    }

    static void lowererLowerValue(Lowerer* l, i32 v) {
        Function* f = (l)->f;
        Value* val = (&((f)->values[v]));
        Span<i32> args = functionArgs(f, v);
        u8 size = typeSize((val)->typ);
        {
            auto switch827 = (val)->op;
            switch (switch827) {
                case OpAdd:
                case OpSub:
                case OpAnd:
                case OpOr:
                case OpXor:
                case OpMul: {
                    lowererAlu(l, v, (val)->op, args[Int(0LL)], args[Int(1LL)], size);
                    break;
                }
                case OpShl:
                case OpShrS:
                case OpShrU: {
                    lowererShift(l, v, (val)->op, args[Int(0LL)], args[Int(1LL)], size);
                    break;
                }
                case OpRotl:
                case OpRotr: {
                    lowererRotate(l, v, (val)->op, args[Int(0LL)], args[Int(1LL)], size);
                    break;
                }
                case OpDivS:
                case OpDivU:
                case OpRemS:
                case OpRemU: {
                    lowererDivide(l, v, (val)->op, args[Int(0LL)], args[Int(1LL)], size);
                    break;
                }
                case OpClz:
                case OpCtz:
                case OpPopcnt: {
                    MInst in = inst(mBitCount);
                    (in).dst = lowererDef(l, v);
                    (in).src = lowererUse(l, args[Int(0LL)]);
                    (in).size = size;
                    (in).aux = convert<u8>(Op((val)->op - OpClz));
                    lowererEmit(l, in);
                    break;
                }
                case OpExtend8S:
                case OpExtend16S:
                case OpExtend32S: {
                    MInst in = inst(mExt);
                    (in).dst = lowererDef(l, v);
                    (in).src = lowererUse(l, args[Int(0LL)]);
                    (in).size = Array<u8, 3>{{u8(1LL), u8(2LL), u8(4LL)}}[Op((val)->op - OpExtend8S)];
                    (in).dsize = size;
                    (in).aux = u8(1LL);
                    lowererEmit(l, in);
                    break;
                }
                case OpEqz:
                case OpEq:
                case OpNe:
                case OpLtS:
                case OpLtU:
                case OpGtS:
                case OpGtU:
                case OpLeS:
                case OpLeU:
                case OpGeS:
                case OpGeU:
                case OpFEq:
                case OpFNe:
                case OpFLt:
                case OpFGt:
                case OpFLe:
                case OpFGe: {
                    lowererSetcc(l, v);
                    break;
                }
                case OpFAdd:
                case OpFSub:
                case OpFMul:
                case OpFDiv: {
                    MInst in = inst(mFAlu);
                    (in).dst = lowererDef(l, v);
                    (in).src = lowererUse(l, args[Int(0LL)]);
                    (in).src2 = lowererUse(l, args[Int(1LL)]);
                    (in).size = size;
                    (in).aux = convert<u8>(Op((val)->op - OpFAdd));
                    lowererEmit(l, in);
                    break;
                }
                case OpFMin:
                case OpFMax: {
                    MInst in = inst(mFMinMax);
                    (in).dst = lowererDef(l, v);
                    (in).src = lowererUse(l, args[Int(0LL)]);
                    (in).src2 = lowererUse(l, args[Int(1LL)]);
                    (in).size = size;
                    if (((val)->op == OpFMax)) {
                        (in).aux = u8(1LL);
                    }
                    lowererEmit(l, in);
                    break;
                }
                case OpFCopysign: {
                    lowererCopysign(l, v, args[Int(0LL)], args[Int(1LL)], size);
                    break;
                }
                case OpFAbs:
                case OpFNeg: {
                    MInst in = inst(mFAluMem);
                    (in).dst = lowererDef(l, v);
                    (in).src = lowererUse(l, args[Int(0LL)]);
                    (in).size = size;
                    (in).aux = u8(6LL);
                    (in).mem = Mem{noReg, noReg, {}, {}, constF32Abs};
                    if (((val)->op == OpFNeg)) {
                        (in).aux = u8(9LL);
                        ((in).mem).label = constF32Sign;
                    }
                    if ((size == u8(8LL))) {
                        ((in).mem).label += i32(2LL);
                    }
                    lowererEmit(l, in);
                    break;
                }
                case OpFSqrt:
                case OpFCeil:
                case OpFFloor:
                case OpFTrunc:
                case OpFNearest: {
                    MInst in = inst(mFUnary);
                    (in).dst = lowererDef(l, v);
                    (in).src = lowererUse(l, args[Int(0LL)]);
                    (in).size = size;
                    if (((val)->op != OpFSqrt)) {
                        (in).aux = fRound;
                        switch (val->op) {
                            case OpFFloor: {
                                in.imm = 1;
                                break;
                            }
                            case OpFCeil: {
                                in.imm = 2;
                                break;
                            }
                            case OpFTrunc: {
                                in.imm = 3;
                                break;
                            }
                            default: {
                                break;
                            }
                        }
                    }
                    lowererEmit(l, in);
                    break;
                }
                case OpWrap: {
                    MInst in = inst(mMov);
                    (in).dst = lowererDef(l, v);
                    (in).src = lowererUse(l, args[Int(0LL)]);
                    (in).size = u8(4LL);
                    (in).aux = u8(1LL);
                    lowererEmit(l, in);
                    break;
                }
                case OpExtendU: {
                    lowererMov(l, lowererDef(l, v), lowererUse(l, args[Int(0LL)]), u8(8LL));
                    break;
                }
                case OpExtendS: {
                    MInst in = inst(mExt);
                    (in).dst = lowererDef(l, v);
                    (in).src = lowererUse(l, args[Int(0LL)]);
                    (in).size = u8(4LL);
                    (in).dsize = u8(8LL);
                    (in).aux = u8(1LL);
                    lowererEmit(l, in);
                    break;
                }
                case OpTruncS:
                case OpTruncU:
                case OpTruncSatS:
                case OpTruncSatU: {
                    MInst in = inst(mTruncF);
                    (in).dst = lowererDef(l, v);
                    (in).src = lowererUse(l, args[Int(0LL)]);
                    (in).size = size;
                    (in).dsize = typeSize(((f)->values[args[Int(0LL)]]).typ);
                    if ((l)->rawTrunc[v]) {
                        (in).op = mCvtFI;
                        lowererEmit(l, in);
                        break;
                    }
                    if ((((val)->op == OpTruncU) || ((val)->op == OpTruncSatU))) {
                        (in).aux |= truncUnsigned;
                        if ((size == u8(4LL))) {
                            mInstExtra(&in)->temps.assign(Span<const VReg>({mFuncNewReg((l)->mf, classGPR)}));
                        } else {
                            mInstExtra(&in)->temps.assign(Span<const VReg>({mFuncNewReg((l)->mf, classXMM), mFuncNewReg((l)->mf, classGPR)}));
                        }
                    }
                    if ((((val)->op == OpTruncSatS) || ((val)->op == OpTruncSatU))) {
                        (in).aux |= truncSat;
                    }
                    lowererEmit(l, in);
                    break;
                }
                case OpConvertS:
                case OpConvertU: {
                    lowererConvert(l, v, (val)->op, args[Int(0LL)], size);
                    break;
                }
                case OpDemote:
                case OpPromote: {
                    MInst in = inst(mCvtFF);
                    (in).dst = lowererDef(l, v);
                    (in).src = lowererUse(l, args[Int(0LL)]);
                    (in).size = typeSize(((f)->values[args[Int(0LL)]]).typ);
                    (in).dsize = size;
                    lowererEmit(l, in);
                    break;
                }
                case OpReinterpret: {
                    MInst in = inst(mMovGX);
                    if ((!(isFloat((val)->typ)))) {
                        (in).op = mMovXG;
                    }
                    (in).dst = lowererDef(l, v);
                    (in).src = lowererUse(l, args[Int(0LL)]);
                    (in).size = size;
                    lowererEmit(l, in);
                    break;
                }
                case OpSelect: {
                    lowererSelectValue(l, v, args);
                    break;
                }
                case OpLoad: {
                    lowererLoad(l, v, args[Int(0LL)]);
                    break;
                }
                case OpStore: {
                    lowererStore(l, v, args[Int(0LL)], args[Int(1LL)]);
                    break;
                }
                case OpMemSize: {
                    MInst in = inst(mLoad);
                    (in).dst = lowererDef(l, v);
                    (in).mem = memAt(ctxReg, offMemSize);
                    (in).size = u8(8LL);
                    (in).dsize = u8(8LL);
                    lowererEmit(l, in);
                    MInst sh = inst(mShiftImm);
                    (sh).dst = (in).dst;
                    (sh).imm = i64(16LL);
                    (sh).size = u8(8LL);
                    (sh).aux = shiftShr;
                    lowererEmit(l, sh);
                    break;
                }
                case OpMemGrow: {
                    lowererStubCall(l, stubMemoryGrow, sliceOf<VReg>({lowererUse(l, args[Int(0LL)])}), sliceOf<ValType>({I32}), v);
                    break;
                }
                case OpMemCopy: {
                    lowererMemCopy(l, args);
                    break;
                }
                case OpCopyForward: {
                    auto temporary828 = mFuncNewReg((l)->mf, classGPR);
                    auto temporary829 = mFuncNewReg((l)->mf, classGPR);
                    VReg dst = temporary828;
                    VReg src = temporary829;
                    lowererMemAddr(l, dst, lowererUse(l, args[Int(0LL)]));
                    lowererMemAddr(l, src, lowererUse(l, args[Int(1LL)]));
                    MInst pm = inst(mParMove);
                    mInstExtra(&pm)->moves.assign(Span<const Move>({Move{RDI, dst}, Move{RSI, src}, Move{RCX, lowererUse(l, args[Int(2LL)])}}));
                    lowererEmit(l, pm);
                    MInst in = inst(mCopyForward);
                    mInstExtra(&in)->uses.assign(Span<const VReg>({RDI, RSI, RCX}));
                    mInstExtra(&in)->defs.assign(Span<const VReg>({RAX, RDI, RSI, RCX}));
                    lowererEmit(l, in);
                    break;
                }
                case OpMemFill: {
                    lowererMemFill(l, args);
                    break;
                }
                case OpMemInit: {
                    lowererMemInit(l, convert<i64>((val)->aux), args);
                    break;
                }
                case OpDataDrop: {
                    MInst in = inst(mStoreImm);
                    (in).mem = memAt(ctxReg, layoutDataDropped((l)->lay, convert<i64>((val)->aux)));
                    (in).size = u8(1LL);
                    (in).imm = i64(1LL);
                    lowererEmit(l, in);
                    break;
                }
                case OpGlobalGet: {
                    lowererGlobalGet(l, v, convert<i64>((val)->aux));
                    break;
                }
                case OpGlobalSet: {
                    lowererGlobalSet(l, convert<i64>((val)->aux), args[Int(0LL)]);
                    break;
                }
                case OpCall: {
                    lowererCall(l, v, convert<i64>((val)->aux), args);
                    break;
                }
                case OpCallIndirect: {
                    lowererCallIndirect(l, v, convert<i64>((val)->aux), convert<i64>((val)->aux2), args);
                    break;
                }
                case OpExtract: {
                    break;
                }
                case OpRefFunc: {
                    MInst in = inst(mLea);
                    (in).dst = lowererDef(l, v);
                    (in).mem = memAt(ctxReg, layoutDesc((l)->lay, convert<i64>((val)->aux)));
                    (in).size = u8(8LL);
                    lowererEmit(l, in);
                    break;
                }
                case OpRefIsNull: {
                    MInst in = inst(mTest);
                    VReg a = lowererUse(l, args[Int(0LL)]);
                    (in).src = a;
                    (in).src2 = a;
                    (in).size = u8(8LL);
                    lowererEmit(l, in);
                    MInst set = inst(mSetcc);
                    (set).dst = lowererDef(l, v);
                    (set).cc = ccE;
                    lowererEmit(l, set);
                    break;
                }
                case OpTableGet: {
                    lowererTableGet(l, v, convert<i64>((val)->aux), args[Int(0LL)]);
                    break;
                }
                case OpTableSet: {
                    lowererTableSet(l, convert<i64>((val)->aux), args[Int(0LL)], args[Int(1LL)]);
                    break;
                }
                case OpTableSize: {
                    MInst in = inst(mLoad);
                    (in).dst = lowererDef(l, v);
                    (in).mem = memAt(ctxReg, (((l)->lay)->tables[(val)->aux] + tableSize));
                    (in).size = u8(4LL);
                    (in).dsize = u8(4LL);
                    lowererEmit(l, in);
                    break;
                }
                case OpTableGrow: {
                    VReg table = lowererConstReg(l, (val)->aux, u8(4LL));
                    lowererStubCall(l, stubTableGrow, sliceOf<VReg>({table, lowererUse(l, args[Int(1LL)]), lowererUse(l, args[Int(0LL)])}), sliceOf<ValType>({I32, I32, FuncRef}), v);
                    break;
                }
                default: {
                    compilerError(String("unsupported operator %s in function %d"));
                    break;
                }
            }
        }
    }

    // markDeferred finds comparisons whose only use is the branch or a select of
    // their own block.
    static void lowererMarkDeferred(Lowerer* l) {
        Function* f = (l)->f;
        l->deferred.resize(f->values.size());
        l->deferred.fill(false);
        {
            auto range830 = ((l)->ci)->order;
            for (Int index831 = 0, rangeEnd832 = length(range830); index831 < rangeEnd832; ++index831) {
                i32 b = range830[index831];
                Block* blk = (&((f)->blocks[b]));
                if ((((blk)->term).kind == TermIf)) {
                    i32 c = ((blk)->term).value;
                    if (((((l)->uses[c] == i32(1LL)) && (((f)->values[c]).block == b)) && flagCondition(((f)->values[c]).op))) {
                        (l)->deferred[c] = true;
                    }
                }
                {
                    auto range833 = (blk)->instrs.view();
                    for (Int index834 = 0, rangeEnd835 = length(range833); index834 < rangeEnd835; ++index834) {
                        i32 v = range833[index834];
                        Value* val = (&((f)->values[v]));
                        if (((val)->op == OpAdd)) {
                            Span<i32> args = functionArgs(f, v);
                            {
                                Int i = (length(args) - Int(1LL));
                                for (; (i >= Int(0LL)); i--) {
                                    i32 a = args[i];
                                    if (lowererScaledIndex(l, a, b)) {
                                        (l)->deferred[a] = true;
                                        break;
                                    }
                                }
                            }
                        }
                        if (((val)->op == OpEqz)) {
                            i32 a = functionArgs(f, v)[Int(0LL)];
                            if (((((l)->uses[a] == i32(1LL)) && (((f)->values[a]).block == b)) && (((f)->values[a]).op == OpAnd))) {
                                (l)->deferred[a] = true;
                            }
                        }
                        if ((((f)->values[v]).op != OpSelect)) {
                            continue;
                        }
                        i32 c = functionArgs(f, v)[Int(0LL)];
                        Op op = ((f)->values[c]).op;
                        if (((((l)->uses[c] == i32(1LL)) && (((f)->values[c]).block == b)) && flagCondition(op))) {
                            (l)->deferred[c] = true;
                        }
                    }
                }
            }
        }
    }

    static void lowererMemAddr(Lowerer* l, VReg dst, VReg a) {
        MInst in = inst(mLea);
        (in).dst = dst;
        (in).mem = memIndexed(memReg, a, i32(0LL));
        (in).size = u8(8LL);
        lowererEmit(l, in);
    }

    static void lowererMemCopy(Lowerer* l, Span<const i32> args) {
        auto temporary836 = lowererUse(l, args[Int(0LL)]);
        auto temporary837 = lowererUse(l, args[Int(1LL)]);
        auto temporary838 = lowererUse(l, args[Int(2LL)]);
        VReg d = temporary836;
        VReg s = temporary837;
        VReg n = temporary838;
        lowererCheckRange(l, d, n, memAt(ctxReg, ((l)->lay)->memoryEnd), u8(8LL), trapMemoryOOB);
        lowererCheckRange(l, s, n, memAt(ctxReg, ((l)->lay)->memoryEnd), u8(8LL), trapMemoryOOB);
        VReg dst = mFuncNewReg((l)->mf, classGPR);
        lowererMemAddr(l, dst, d);
        VReg src = mFuncNewReg((l)->mf, classGPR);
        lowererMemAddr(l, src, s);
        lowererMemStub(l, stubMemmove, dst, src, n);
    }

    static void lowererMemFill(Lowerer* l, Span<const i32> args) {
        auto temporary839 = lowererUse(l, args[Int(0LL)]);
        auto temporary840 = lowererUse(l, args[Int(1LL)]);
        auto temporary841 = lowererUse(l, args[Int(2LL)]);
        VReg d = temporary839;
        VReg val = temporary840;
        VReg n = temporary841;
        lowererCheckRange(l, d, n, memAt(ctxReg, ((l)->lay)->memoryEnd), u8(8LL), trapMemoryOOB);
        VReg dst = mFuncNewReg((l)->mf, classGPR);
        lowererMemAddr(l, dst, d);
        lowererMemStub(l, stubMemset, dst, val, n);
    }

    static void lowererMemInit(Lowerer* l, Int seg, Span<const i32> args) {
        auto temporary842 = lowererUse(l, args[Int(0LL)]);
        auto temporary843 = lowererUse(l, args[Int(1LL)]);
        auto temporary844 = lowererUse(l, args[Int(2LL)]);
        VReg d = temporary842;
        VReg s = temporary843;
        VReg n = temporary844;
        VReg lengthValue = lowererConstReg(l, convert<i64>(length((((l)->m)->datas[seg]).bytes)), u8(8LL));
        VReg zero = lowererConstReg(l, i64(0LL), u8(4LL));
        VReg flag = mFuncNewReg((l)->mf, classGPR);
        MInst ld = inst(mLoad);
        (ld).dst = flag;
        (ld).mem = memAt(ctxReg, layoutDataDropped((l)->lay, seg));
        (ld).size = u8(1LL);
        (ld).dsize = u8(4LL);
        lowererEmit(l, ld);
        MInst tst = inst(mTest);
        (tst).src = flag;
        (tst).src2 = flag;
        (tst).size = u8(4LL);
        lowererEmit(l, tst);
        MInst cm = inst(mCmov);
        (cm).dst = lengthValue;
        (cm).src = zero;
        (cm).cc = ccNE;
        (cm).size = u8(8LL);
        lowererEmit(l, cm);
        VReg t = mFuncNewReg((l)->mf, classGPR);
        lowererMov(l, t, s, u8(8LL));
        MInst add = inst(mAlu);
        (add).dst = t;
        (add).src = n;
        (add).size = u8(8LL);
        (add).aux = u8(0LL);
        lowererEmit(l, add);
        MInst c = inst(mCmp);
        (c).src = t;
        (c).src2 = lengthValue;
        (c).size = u8(8LL);
        lowererEmit(l, c);
        lowererTrapIf(l, ccA, trapMemoryOOB);
        lowererCheckRange(l, d, n, memAt(ctxReg, ((l)->lay)->memoryEnd), u8(8LL), trapMemoryOOB);
        VReg dst = mFuncNewReg((l)->mf, classGPR);
        lowererMemAddr(l, dst, d);
        VReg src = mFuncNewReg((l)->mf, classGPR);
        MInst lea = inst(mLea);
        (lea).dst = src;
        (lea).mem = Mem{noReg, noReg, {}, {}, dataLabel(seg)};
        (lea).size = u8(8LL);
        lowererEmit(l, lea);
        MInst addS = inst(mAlu);
        (addS).dst = src;
        (addS).src = s;
        (addS).size = u8(8LL);
        (addS).aux = u8(0LL);
        lowererEmit(l, addS);
        lowererMemStub(l, stubMemmove, dst, src, n);
    }

    // memStub calls the memmove stub with the destination, source and length,
    // or the memset stub with the destination, byte and length, in the
    // registers of their string instructions.
    static void lowererMemStub(Lowerer* l, Int stub, VReg a, VReg b, VReg c) {
        VReg second = RSI;
        if ((stub == stubMemset)) {
            second = RAX;
        }
        MInst pm = inst(mParMove);
        mInstExtra(&pm)->moves.assign(Span<const Move>({Move{RDI, a}, Move{second, b}, Move{RCX, c}}));
        lowererEmit(l, pm);
        MInst in = inst(mCall);
        (in).aux = calleeStub;
        (in).imm = convert<i64>(stub);
        mInstExtra(&in)->uses.assign(Span<const VReg>({RDI, second, RCX}));
        ((l)->mf)->hasCalls = true;
        lowererEmit(l, in);
    }

    static void lowererMov(Lowerer* l, VReg dst, VReg src, u8 size) {
        MInst in = inst(mMov);
        if ((vRegIsXMM(dst) || (((!(vRegIsPhys(dst))) && (mFuncRegClass((l)->mf, dst) == classXMM))))) {
            (in).op = mFMov;
        }
        (in).dst = dst;
        (in).src = src;
        (in).size = size;
        lowererEmit(l, in);
    }

    // outgoing makes room for a call's stack arguments and results.
    static void lowererOutgoing(Lowerer* l, Int stackArgs, Slice<VReg> regs) {
        Int n = stackArgs;
        {
            auto range845 = regs;
            for (Int index846 = 0, rangeEnd847 = length(range845); index846 < rangeEnd847; ++index846) {
                VReg r = range845[index846];
                if ((r == noReg)) {
                    n++;
                }
            }
        }
        if (((Int(8LL) * n) > ((l)->mf)->stackArgs)) {
            ((l)->mf)->stackArgs = (Int(8LL) * n);
        }
    }

    // placeArgs moves call arguments to their registers and stack slots and
    // returns the registers the call reads and the number of stack slots.
    static Tuple2<Slice<VReg>, Int> lowererPlaceArgs(Lowerer* l, Span<const i32> args) {
        Slice<VReg> regs = makeSlice<VReg>(length(args));
        Slice<ValType> types = makeSlice<ValType>(length(args));
        {
            auto range848 = args;
            for (Int index849 = 0, rangeEnd850 = length(range848); index849 < rangeEnd850; ++index849) {
                Int i = index849;
                i32 a = range848[index849];
                regs[i] = lowererUse(l, a);
                types[i] = (((l)->f)->values[a]).typ;
            }
        }
        return lowererPlaceRegs(l, regs, types);
    }

    // Nonzero float constants share a register at their common dominator,
    // outside a loop when needed, including uses in returns and edge copies.
    // Integer constants and zero remain cheap to rematerialize at each use.
    static void lowererPlaceFloatConstants(Lowerer* l) {
        Function* f = (l)->f;
        Slice<i32> homes = makeSlice<i32>(length((f)->values));
        {
            auto range851 = homes;
            for (Int index852 = 0, rangeEnd853 = length(range851); index852 < rangeEnd853; ++index852) {
                Int v = index852;
                homes[v] = i32(-1LL);
            }
        }
        auto use = [&](i32 b, i32 v) mutable -> void {
            Value* val = (&((f)->values[v]));
            if ((((((val)->op != OpConst) || (!(isFloat((val)->typ)))) || ((val)->aux == i64(0LL))) || ((l)->uses[v] < i32(2LL)))) {
                return;
            }
            if ((homes[v] < i32(0LL))) {
                homes[v] = b;
            } else {
                homes[v] = cfgInfoIntersect((l)->ci, homes[v], b);
            }
        };
        {
            auto range854 = ((l)->ci)->order;
            for (Int index855 = 0, rangeEnd856 = length(range854); index855 < rangeEnd856; ++index855) {
                i32 b = range854[index855];
                Block* blk = (&((f)->blocks[b]));
                {
                    auto range857 = (blk)->instrs.view();
                    for (Int index858 = 0, rangeEnd859 = length(range857); index858 < rangeEnd859; ++index858) {
                        i32 v = range857[index858];
                        {
                            auto range860 = functionArgs(f, v);
                            for (Int index861 = 0, rangeEnd862 = length(range860); index861 < rangeEnd862; ++index861) {
                                i32 a = range860[index861];
                                use(b, a);
                            }
                        }
                    }
                }
                {
                    auto range863 = ((blk)->term).values;
                    for (Int index864 = 0, rangeEnd865 = length(range863); index864 < rangeEnd865; ++index864) {
                        i32 v = range863[index864];
                        use(b, v);
                    }
                }
                {
                    auto range866 = ((blk)->term).edges;
                    for (Int index867 = 0, rangeEnd868 = length(range866); index867 < rangeEnd868; ++index867) {
                        Edge e = range866[index867];
                        {
                            auto range869 = (e).args;
                            for (Int index870 = 0, rangeEnd871 = length(range869); index870 < rangeEnd871; ++index870) {
                                i32 v = range869[index870];
                                use(b, v);
                            }
                        }
                    }
                }
            }
        }
        (l)->floatHomes = makeSlice<Slice<i32>>(length((f)->blocks));
        {
            auto range872 = homes;
            for (Int index873 = 0, rangeEnd874 = length(range872); index873 < rangeEnd874; ++index873) {
                Int v = index873;
                i32 b = range872[index873];
                if ((b < i32(0LL))) {
                    continue;
                }
                {
                    for (; ((((l)->ci)->depth[b] > i32(0LL)) && (((l)->ci)->idom[b] != b));) {
                        b = ((l)->ci)->idom[b];
                    }
                }
                (l)->floatHomes[b] = appendValues((l)->floatHomes[b], convert<i32>(v));
            }
        }
    }

    // placeRegs moves argument registers to their argument registers and stack
    // slots and returns the fixed registers the call reads and the number of
    // stack slots.
    static Tuple2<Slice<VReg>, Int> lowererPlaceRegs(Lowerer* l, Slice<VReg> regs, Slice<ValType> types) {
        Slice<VReg> uses = {};
        auto temporary875 = Int(0LL);
        auto temporary876 = Int(0LL);
        auto temporary877 = Int(0LL);
        Int gi = temporary875;
        Int xi = temporary876;
        Int stack = temporary877;
        MInst pm = inst(mParMove);
        {
            auto range878 = regs;
            for (Int index879 = 0, rangeEnd880 = length(range878); index879 < rangeEnd880; ++index879) {
                Int i = index879;
                VReg r = range878[index879];
                ValType t = types[i];
                if (isFloat(t)) {
                    if ((xi < length(argXMMs))) {
                        mInstExtra(&pm)->moves.pushBack(Move{argXMMs[xi], r});
                        uses = appendValues(uses, argXMMs[xi]);
                        xi++;
                        continue;
                    }
                } else if ((gi < length(argGPRs))) {
                    mInstExtra(&pm)->moves.pushBack(Move{argGPRs[gi], r});
                    uses = appendValues(uses, argGPRs[gi]);
                    gi++;
                    continue;
                }
                MInst in = inst(mStackArg);
                (in).src = r;
                (in).imm = convert<i64>(stack);
                (in).size = typeSize(t);
                lowererEmit(l, in);
                stack++;
            }
        }
        if ((length(mInstMoves(&pm)) > Int(0LL))) {
            lowererEmit(l, pm);
        }
        return {uses, stack};
    }

    static void lowererRotate(Lowerer* l, i32 v, Op op, i32 a, i32 b, u8 size) {
        VReg d = lowererDef(l, v);
        u8 kind = u8(0LL);
        if ((op == OpRotr)) {
            kind = shiftRor;
        }
        if (lowererIsConst(l, b)) {
            i64 n = (lowererConstBits(l, b) & convert<i64>(u8(u8(size * u8(8LL)) - u8(1LL))));
            lowererMov(l, d, lowererUse(l, a), size);
            if ((n != i64(0LL))) {
                MInst in = inst(mShiftImm);
                (in).dst = d;
                (in).imm = n;
                (in).size = size;
                (in).aux = kind;
                lowererEmit(l, in);
            }
            return;
        }
        VReg ra = lowererUse(l, a);
        VReg rb = lowererUse(l, b);
        lowererMov(l, d, ra, size);
        lowererMov(l, RCX, rb, size);
        MInst in = inst(mRotCL);
        (in).dst = d;
        (in).src = RCX;
        (in).size = size;
        (in).aux = kind;
        lowererEmit(l, in);
    }

    static bool lowererScaledIndex(Lowerer* l, i32 v, i32 block) {
        Function* f = (l)->f;
        Value* val = (&((f)->values[v]));
        if (((((val)->op != OpShl) || ((val)->block != block)) || ((l)->uses[v] != i32(1LL)))) {
            return false;
        }
        i32 count = functionArgs(f, v)[Int(1LL)];
        return (lowererIsConst(l, count) && ((lowererConstBits(l, count) & convert<i64>(u8(u8(typeSize((val)->typ) * u8(8LL)) - u8(1LL)))) <= i64(3LL)));
    }

    static void lowererSelectValue(Lowerer* l, i32 v, Span<const i32> args) {
        Function* f = (l)->f;
        auto temporary881 = args[Int(0LL)];
        auto temporary882 = args[Int(1LL)];
        auto temporary883 = args[Int(2LL)];
        i32 cond = temporary881;
        i32 x = temporary882;
        i32 y = temporary883;
        ValType t = ((f)->values[v]).typ;
        u8 size = typeSize(t);
        if (isFloat(t)) {
            if ((((l)->deferred[cond] && (((f)->values[cond]).op >= OpFEq)) && (((f)->values[cond]).op <= OpFGe))) {
                Span<i32> cmp = functionArgs(f, cond);
                MInst in = inst(mFSelect);
                (in).dst = lowererDef(l, v);
                (in).src = lowererUse(l, x);
                (in).src2 = lowererUse(l, y);
                mInstExtra(&in)->uses.assign(Span<const VReg>({lowererUse(l, cmp[Int(0LL)]), lowererUse(l, cmp[Int(1LL)])}));
                mInstExtra(&in)->temps.assign(Span<const VReg>({mFuncNewReg((l)->mf, classXMM)}));
                (in).aux = convert<u8>(Op(((f)->values[cond]).op - OpFEq));
                (in).size = size;
                (in).dsize = typeSize(((f)->values[cmp[Int(0LL)]]).typ);
                lowererEmit(l, in);
                return;
            }
            VReg gx = lowererFloatBits(l, x, size);
            VReg gy = lowererFloatBits(l, y, size);
            u8 cc = u8(5LL);
            if ((l)->deferred[cond]) {
                Int parity = {};
                auto temporary884 = lowererCompare(l, cond);
                cc = temporary884.v0;
                parity = temporary884.v1;
                if ((parity != Int(0LL))) {
                    compilerError(String("internal error: float comparison deferred to select"));
                }
            } else {
                VReg c = lowererUse(l, cond);
                MInst tst = inst(mTest);
                (tst).src = c;
                (tst).src2 = c;
                (tst).size = u8(4LL);
                lowererEmit(l, tst);
            }
            MInst cm = inst(mCmov);
            (cm).dst = gy;
            (cm).src = gx;
            (cm).cc = cc;
            (cm).size = size;
            lowererEmit(l, cm);
            MInst back = inst(mMovGX);
            (back).dst = lowererDef(l, v);
            (back).src = gy;
            (back).size = size;
            lowererEmit(l, back);
            return;
        }
        VReg rx = lowererUse(l, x);
        VReg ry = lowererUse(l, y);
        VReg d = lowererDef(l, v);
        lowererMov(l, d, ry, size);
        u8 cc = u8(5LL);
        Int parity = Int(0LL);
        if ((l)->deferred[cond]) {
            auto temporary885 = lowererCompare(l, cond);
            cc = temporary885.v0;
            parity = temporary885.v1;
        } else {
            VReg c = lowererUse(l, cond);
            MInst tst = inst(mTest);
            (tst).src = c;
            (tst).src2 = c;
            (tst).size = u8(4LL);
            lowererEmit(l, tst);
        }
        MInst cm = inst(mCmov);
        (cm).dst = d;
        (cm).src = rx;
        (cm).cc = cc;
        (cm).size = size;
        if ((size < u8(4LL))) {
            (cm).size = u8(4LL);
        }
        lowererEmit(l, cm);
        if ((parity != Int(0LL))) {
            (cm).cc = ccP;
            if ((parity == Int(1LL))) {
                (cm).src = ry;
            }
            lowererEmit(l, cm);
        }
    }

    static void lowererSetcc(Lowerer* l, i32 v) {
        auto temporary886 = lowererCompare(l, v);
        u8 cc = temporary886.v0;
        Int parity = temporary886.v1;
        VReg d = lowererDef(l, v);
        MInst in = inst(mSetcc);
        (in).dst = d;
        (in).cc = cc;
        lowererEmit(l, in);
        if ((parity == Int(0LL))) {
            return;
        }
        VReg p = mFuncNewReg((l)->mf, classGPR);
        MInst pi = inst(mSetcc);
        (pi).dst = p;
        (pi).cc = ccNP;
        AluOp op = aluAnd;
        if ((parity == Int(2LL))) {
            (pi).cc = ccP;
            op = aluOr;
        }
        lowererEmit(l, pi);
        MInst a = inst(mAlu);
        (a).dst = d;
        (a).src = p;
        (a).size = u8(4LL);
        (a).aux = convert<u8>(op);
        lowererEmit(l, a);
    }

    static void lowererShift(Lowerer* l, i32 v, Op op, i32 a, i32 b, u8 size) {
        VReg d = lowererDef(l, v);
        u8 kind = 0;
        switch (op) {
            case OpShl: {
                kind = shiftShl;
                break;
            }
            case OpShrS: {
                kind = shiftSar;
                break;
            }
            case OpShrU: {
                kind = shiftShr;
                break;
            }
            default: {
                break;
            }
        }
        if (lowererIsConst(l, b)) {
            i64 n = (lowererConstBits(l, b) & convert<i64>(u8(u8(size * u8(8LL)) - u8(1LL))));
            lowererMov(l, d, lowererUse(l, a), size);
            if ((n != i64(0LL))) {
                MInst in = inst(mShiftImm);
                (in).dst = d;
                (in).imm = n;
                (in).size = size;
                (in).aux = kind;
                lowererEmit(l, in);
            }
            return;
        }
        MInst in = inst(mShiftX);
        (in).dst = d;
        (in).src = lowererUse(l, a);
        (in).src2 = lowererUse(l, b);
        (in).size = size;
        (in).aux = kind;
        lowererEmit(l, in);
    }

    static void lowererStore(Lowerer* l, i32 v, i32 addr, i32 value) {
        Value* val = (&(((l)->f)->values[v]));
        MemKind kind = convert<MemKind>((val)->aux2);
        Mem mem = lowererAddress(l, addr, (val)->aux, memKindWidth(kind), (MemKind(kind & memProven) != MemKind(0LL)));
        u8 width = convert<u8>(memKindWidth(kind));
        ValType t = (((l)->f)->values[value]).typ;
        if (((isFloat(t) && lowererIsConst(l, value)) && ((l)->vregs[value] == noReg))) {
            i64 bits = lowererConstBits(l, value);
            if (((width == u8(4LL)) || (bits == convert<i64>(convert<i32>(bits))))) {
                MInst in = inst(mStoreImm);
                (in).mem = mem;
                (in).imm = bits;
                (in).size = width;
                lowererEmit(l, in);
                return;
            }
        }
        if ((!(isFloat(t)))) {
            {
                auto temporary887 = lowererImm(l, value, width);
                i64 x = temporary887.v0;
                bool ok = temporary887.v1;
                if ((ok && (width <= u8(8LL)))) {
                    MInst in = inst(mStoreImm);
                    (in).mem = mem;
                    (in).imm = x;
                    (in).size = width;
                    lowererEmit(l, in);
                    return;
                }
            }
        }
        MInst in = inst(mStore);
        if (isFloat(t)) {
            (in).op = mFStore;
        }
        (in).mem = mem;
        (in).src = lowererUse(l, value);
        (in).size = width;
        lowererEmit(l, in);
    }

    // stubCall calls a stub that goes into Go, in the internal convention;
    // result, when not negative, receives the i32 result.
    static void lowererStubCall(Lowerer* l, Int stub, Slice<VReg> regs, Slice<ValType> types, i32 result) {
        auto temporary888 = lowererPlaceRegs(l, regs, types);
        Slice<VReg> uses = temporary888.v0;
        Int stack = temporary888.v1;
        if ((stack != Int(0LL))) {
            compilerError(String("internal error: stub %d takes stack arguments"));
        }
        MInst in = inst(mCall);
        (in).aux = calleeStub;
        (in).imm = convert<i64>(stub);
        mInstExtra(&in)->uses.assign(uses);
        if ((result >= i32(0LL))) {
            mInstExtra(&in)->defs.assign(Span<const VReg>({RAX}));
        }
        ((l)->mf)->hasCalls = true;
        lowererEmit(l, in);
        if ((result >= i32(0LL))) {
            MInst mv = inst(mMov);
            (mv).dst = lowererDef(l, result);
            (mv).src = RAX;
            (mv).size = u8(4LL);
            (mv).aux = u8(1LL);
            lowererEmit(l, mv);
        }
    }

    static void lowererTableGet(Lowerer* l, i32 v, Int table, i32 index) {
        i32 tb = ((l)->lay)->tables[table];
        VReg ri = lowererUse(l, index);
        MInst c = inst(mCmpMem);
        (c).src = ri;
        (c).mem = memAt(ctxReg, (tb + tableSize));
        (c).size = u8(4LL);
        lowererEmit(l, c);
        lowererTrapIf(l, ccAE, trapTableOOB);
        VReg entries = mFuncNewReg((l)->mf, classGPR);
        MInst ld = inst(mLoad);
        (ld).dst = entries;
        (ld).mem = memAt(ctxReg, (tb + tableEntries));
        (ld).size = u8(8LL);
        (ld).dsize = u8(8LL);
        lowererEmit(l, ld);
        ld = inst(mLoad);
        (ld).dst = lowererDef(l, v);
        (ld).mem = Mem{entries, ri, u8(8LL), {}, i32(-1LL)};
        (ld).size = u8(8LL);
        (ld).dsize = u8(8LL);
        lowererEmit(l, ld);
    }

    static void lowererTableSet(Lowerer* l, Int table, i32 index, i32 value) {
        i32 tb = ((l)->lay)->tables[table];
        VReg ri = lowererUse(l, index);
        VReg rv = lowererUse(l, value);
        MInst c = inst(mCmpMem);
        (c).src = ri;
        (c).mem = memAt(ctxReg, (tb + tableSize));
        (c).size = u8(4LL);
        lowererEmit(l, c);
        lowererTrapIf(l, ccAE, trapTableOOB);
        VReg entries = mFuncNewReg((l)->mf, classGPR);
        MInst ld = inst(mLoad);
        (ld).dst = entries;
        (ld).mem = memAt(ctxReg, (tb + tableEntries));
        (ld).size = u8(8LL);
        (ld).dsize = u8(8LL);
        lowererEmit(l, ld);
        MInst st = inst(mStore);
        (st).src = rv;
        (st).mem = Mem{entries, ri, u8(8LL), {}, i32(-1LL)};
        (st).size = u8(8LL);
        lowererEmit(l, st);
    }

    static MInst lowererTailCounter(Lowerer* l, i32 offset, i64 amount) {
        MInst in = inst(mCounter);
        (in).mem = memAt(ctxReg, ((((l)->lay)->tailProfile + (convert<i32>(((l)->f)->index) * tailCounterBytes)) + offset));
        (in).imm = amount;
        return in;
    }

    // takeResults defines a call's results: from their registers, or from the
    // stack result slots after stackArgs argument slots.
    static void lowererTakeResults(Lowerer* l, i32 v, Slice<ValType> results, Slice<VReg> regs, Int stackArgs) {
        if ((length(results) == Int(0LL))) {
            return;
        }
        Slice<VReg> dsts = {};
        if ((length(results) == Int(1LL))) {
            dsts = sliceOf<VReg>({lowererDef(l, v)});
        } else {
            dsts = makeSlice<VReg>(length(results));
            {
                auto range889 = dsts;
                for (Int index890 = 0, rangeEnd891 = length(range889); index890 < rangeEnd891; ++index890) {
                    Int i = index890;
                    dsts[i] = noReg;
                }
            }
            {
                auto range892 = lowererExtracts(l, v);
                for (Int index893 = 0, rangeEnd894 = length(range892); index893 < rangeEnd894; ++index893) {
                    i32 user = range892[index893];
                    dsts[(((l)->f)->values[user]).aux] = lowererDef(l, user);
                }
            }
        }
        MInst pm = inst(mParMove);
        Int slot = stackArgs;
        {
            auto range895 = dsts;
            for (Int index896 = 0, rangeEnd897 = length(range895); index896 < rangeEnd897; ++index896) {
                Int i = index896;
                VReg d = range895[index896];
                if ((regs[i] == noReg)) {
                    if ((d != noReg)) {
                        MInst in = inst(mLoad);
                        if (isFloat(results[i])) {
                            (in).op = mFLoad;
                        }
                        (in).dst = d;
                        (in).mem = memAt(RSP, convert<i32>((Int(8LL) * slot)));
                        (in).size = typeSize(results[i]);
                        (in).dsize = (in).size;
                        lowererEmit(l, in);
                    }
                    slot++;
                    continue;
                }
                if ((d != noReg)) {
                    mInstExtra(&pm)->moves.pushBack(Move{d, regs[i]});
                }
            }
        }
        if ((length(mInstMoves(&pm)) > Int(0LL))) {
            lowererEmit(l, pm);
        }
    }

    static void lowererTerminator(Lowerer* l, i32 b) {
        Term* t = (&((((l)->f)->blocks[b]).term));
        {
            auto switch898 = (t)->kind;
            switch (switch898) {
                case TermJump: {
                    lowererEdgeMoves(l, (&((t)->edges[Int(0LL)])));
                    MInst j = inst(mJmp);
                    (j).target = (l)->mblk[((t)->edges[Int(0LL)]).block];
                    lowererEmit(l, j);
                    break;
                }
                case TermIf: {
                    i32 then = lowererEdgeBlock(l, b, Int(0LL));
                    i32 els = lowererEdgeBlock(l, b, Int(1LL));
                    u8 cc = {};
                    Int parity = Int(0LL);
                    if ((l)->deferred[(t)->value]) {
                        auto temporary899 = lowererCompare(l, (t)->value);
                        cc = temporary899.v0;
                        parity = temporary899.v1;
                    } else {
                        VReg c = lowererUse(l, (t)->value);
                        MInst tst = inst(mTest);
                        (tst).src = c;
                        (tst).src2 = c;
                        (tst).size = u8(4LL);
                        lowererEmit(l, tst);
                        cc = ccNE;
                    }
                    {
                        auto switch900 = parity;
                        switch (switch900) {
                            case Int(1LL): {
                                MInst j = inst(mJcc);
                                (j).cc = ccNE;
                                (j).target = els;
                                lowererEmit(l, j);
                                j = inst(mJcc);
                                (j).cc = ccP;
                                (j).target = els;
                                lowererEmit(l, j);
                                j = inst(mJmp);
                                (j).target = then;
                                lowererEmit(l, j);
                                break;
                                break;
                            }
                            case Int(2LL): {
                                MInst j = inst(mJcc);
                                (j).cc = ccNE;
                                (j).target = then;
                                lowererEmit(l, j);
                                j = inst(mJcc);
                                (j).cc = ccP;
                                (j).target = then;
                                lowererEmit(l, j);
                                j = inst(mJmp);
                                (j).target = els;
                                lowererEmit(l, j);
                                break;
                                break;
                            }
                        }
                    }
                    if ((parity != Int(0LL))) {
                        break;
                    }
                    MInst j = inst(mJcc);
                    (j).cc = cc;
                    (j).target = then;
                    lowererEmit(l, j);
                    j = inst(mJmp);
                    (j).target = els;
                    lowererEmit(l, j);
                    break;
                }
                case TermSwitch: {
                    MInst in = inst(mSwitch);
                    (in).src = lowererUse(l, (t)->value);
                    mInstExtra(&in)->temps.assign(Span<const VReg>({mFuncNewReg((l)->mf, classGPR)}));
                    {
                        auto range901 = (t)->edges;
                        for (Int index902 = 0, rangeEnd903 = length(range901); index902 < rangeEnd903; ++index902) {
                            Int i = index902;
                            mInstExtra(&in)->table.pushBack(lowererEdgeBlock(l, b, i));
                        }
                    }
                    if (((length(mInstTable(&in)) >= Int(5LL)) && (mInstTable(&in)[Int(0LL)] != mInstTable(&in)[(length(mInstTable(&in)) - Int(1LL))]))) {
                        i32 dispatch = convert<i32>(length(((l)->mf)->blocks));
                        i32 first = mInstTable(&in)[Int(0LL)];
                        mInstTable(&in)[Int(0LL)] = mInstTable(&in)[(length(mInstTable(&in)) - Int(1LL))];
                        (in).aux = u8(1LL);
                        ((l)->mf)->blocks.pushBack(MBlock{Vec<MInst>({in}), {}, {}, (((l)->mf)->blocks[(l)->cur]).depth, i32(-1LL), {}});
                        MInst test = inst(mTest);
                        auto temporary904 = (in).src;
                        auto temporary905 = (in).src;
                        auto temporary906 = u8(4LL);
                        (test).src = temporary904;
                        (test).src2 = temporary905;
                        (test).size = temporary906;
                        lowererEmit(l, test);
                        MInst j = inst(mJcc);
                        auto temporary907 = ccE;
                        auto temporary908 = first;
                        (j).cc = temporary907;
                        (j).target = temporary908;
                        lowererEmit(l, j);
                        j = inst(mJmp);
                        (j).target = dispatch;
                        lowererEmit(l, j);
                        break;
                    }
                    lowererEmit(l, in);
                    break;
                }
                case TermReturn: {
                    Slice<ValType> results = (((l)->f)->typ)->results;
                    Slice<VReg> regs = resultRegs(results);
                    MInst pm = inst(mParMove);
                    Int slot = Int(0LL);
                    {
                        auto range909 = (t)->values;
                        for (Int index910 = 0, rangeEnd911 = length(range909); index910 < rangeEnd911; ++index910) {
                            Int i = index910;
                            i32 v = range909[index910];
                            if ((regs[i] != noReg)) {
                                mInstExtra(&pm)->moves.pushBack(Move{regs[i], lowererUse(l, v)});
                                continue;
                            }
                            MInst in = inst(mStoreRet);
                            (in).src = lowererUse(l, v);
                            (in).imm = convert<i64>(slot);
                            (in).size = typeSize(results[i]);
                            lowererEmit(l, in);
                            slot++;
                        }
                    }
                    if ((length(mInstMoves(&pm)) > Int(0LL))) {
                        lowererEmit(l, pm);
                    }
                    MInst in = inst(mRet);
                    mInstExtra(&in)->uses.assign(assigned(regs));
                    lowererEmit(l, in);
                    break;
                }
                case TermTrap: {
                    MInst in = inst(mTrap);
                    (in).imm = (t)->aux;
                    lowererEmit(l, in);
                    break;
                }
                default: {
                    compilerError(String("internal error: block %d of function %d has no terminator"));
                    break;
                }
            }
        }
    }

    static void lowererTestBits(Lowerer* l, i32 a, i32 b) {
        if (lowererIsConst(l, a)) {
            auto temporary912 = b;
            auto temporary913 = a;
            a = temporary912;
            b = temporary913;
        }
        MInst in = inst(mTest);
        (in).size = typeSize((((l)->f)->values[a]).typ);
        (in).src = lowererUse(l, a);
        {
            auto temporary914 = lowererImm(l, b, (in).size);
            i64 x = temporary914.v0;
            bool ok = temporary914.v1;
            if (ok) {
                (in).op = mTestImm;
                (in).imm = x;
                (in).aux = u8(1LL);
            } else {
                (in).src2 = lowererUse(l, b);
            }
        }
        lowererEmit(l, in);
    }

    static void lowererTrapIf(Lowerer* l, u8 cc, i64 code) {
        MInst in = inst(mTrapIf);
        (in).cc = cc;
        (in).imm = code;
        lowererEmit(l, in);
    }

    // use returns a register holding v, materializing a constant.
    static VReg lowererUse(Lowerer* l, i32 v) {
        Value* val = (&(((l)->f)->values[v]));
        if (lowererIsConst(l, v)) {
            i64 bits = lowererConstBits(l, v);
            if (isFloat((val)->typ)) {
                if ((bits == i64(0LL))) {
                    return VReg(31LL);
                }
                if (((l)->vregs[v] != noReg)) {
                    return (l)->vregs[v];
                }
                VReg r = mFuncNewReg((l)->mf, classXMM);
                MInst in = inst(mFConst);
                (in).dst = r;
                (in).imm = bits;
                (in).size = typeSize((val)->typ);
                lowererEmit(l, in);
                return r;
            }
            VReg r = mFuncNewReg((l)->mf, classGPR);
            MInst in = inst(mMovImm);
            (in).dst = r;
            (in).imm = bits;
            (in).size = typeSize((val)->typ);
            if (((val)->typ == I32)) {
                (in).imm = convert<i64>(convert<u32>(bits));
            }
            lowererEmit(l, in);
            return r;
        }
        if ((l)->deferred[v]) {
            (l)->deferred[v] = false;
            lowererLowerValue(l, v);
        }
        if (((l)->vregs[v] == noReg)) {
            compilerError(String("internal error: v%d used before definition in function %d"));
        }
        return (l)->vregs[v];
    }

    // In padded mode p <= memory.size guarantees the next 64 KiB physically.
    // Clone only the overlapping spans from a group's first to last check.
    // The checked copy rejoins the shared continuation immediately after the
    // last check, before the final memory access. Calls and terminators outside
    // those spans are therefore never copied into a fallback.
    // The fast copy retains i32 address arithmetic and drops covered checks;
    // entry into the tail itself therefore remains supported by the cold copy.
    static void lowererVersionTailRanges(Lowerer* l) {
        Function* f = (l)->f;
        MFunc* mf = (l)->mf;
        AddressExpressions* ranges = activePool->make<AddressExpressions>(AddressExpressions{f, makeSlice<AddressExpression>(length((f)->values)), makeSlice<bool>(length((f)->values))});
        Int n = length((mf)->blocks);
        bool changed = false;
        {
            Int b = Int(0LL);
            for (; (b < n); b++) {
                struct BlockView {
                    Span<MInst> insts;
                    Slice<i32> succs;
                    Int depth;
                };

                BlockView original{mf->blocks[b].insts.view(), mf->blocks[b].succs, mf->blocks[b].depth};
                Map<Array<AddressTerm, 4>, Slice<TailRangeGroup*>> active = mapOf<Array<AddressTerm, 4>, Slice<TailRangeGroup*>>({});
                Slice<TailRangeGroup*> groups = {};
                {
                    auto range921 = (original).insts;
                    for (Int index922 = 0, rangeEnd923 = length(range921); index922 < rangeEnd923; ++index922) {
                        Int j = index922;
                        MInst in = range921[index922];
                        if (((in).op != mCheck)) {
                            {
                                auto switch924 = (in).op;
                                switch (switch924) {
                                    case mCall:
                                    case mCallInd:
                                    case mParMove:
                                    case mGrowMem:
                                    case mMemCopy:
                                    case mMemFill:
                                    case mMemOp:
                                    case mCopyForward: {
                                        clearMap(active);
                                        break;
                                    }
                                }
                            }
                            continue;
                        }
                        AddressExpression expr = addressExpressionsOf(ranges, in.addressValue, Int(0LL));
                        TailRangeGroup* group = {};
                        {
                            auto range925 = active[(expr).terms];
                            for (Int index926 = 0, rangeEnd927 = length(range925); index926 < rangeEnd927; ++index926) {
                                TailRangeGroup* prior = range925[index926];
                                u64 delta = convert<u64>(((expr).offset - ((prior)->expr).offset));
                                if (((delta + convert<u64>((in).imm)) <= ((l)->m)->memoryTail)) {
                                    group = prior;
                                    break;
                                }
                            }
                        }
                        if (isNil(group)) {
                            group = activePool->make<TailRangeGroup>(TailRangeGroup{j, (in).src, expr, {}});
                            setMap(active, (expr).terms, appendValues(active[(expr).terms], group));
                            groups = appendValues(groups, group);
                        }
                        (group)->checks = appendValues((group)->checks, j);
                    }
                }
                Slice<TailRangeGroup*> selected = subslice(groups, Int(0), Int(0LL));
                Slice<bool> skip = makeSlice<bool>(length((original).insts));
                {
                    auto range928 = groups;
                    for (Int index929 = 0, rangeEnd930 = length(range928); index929 < rangeEnd930; ++index929) {
                        TailRangeGroup* group = range928[index929];
                        if ((length((group)->checks) < Int(2LL))) {
                            continue;
                        }
                        selected = appendValues(selected, group);
                        {
                            auto range931 = (group)->checks;
                            for (Int index932 = 0, rangeEnd933 = length(range931); index932 < rangeEnd933; ++index932) {
                                Int j = range931[index932];
                                skip[j] = true;
                            }
                        }
                    }
                }
                if ((length(selected) == Int(0LL))) {
                    continue;
                }
                Vec<MInst> originalInsts = move(mf->blocks[b].insts);
                original.insts = originalInsts.view();
                changed = true;
                Slice<TailRangeRegion> regions = {};
                {
                    auto range934 = selected;
                    for (Int index935 = 0, rangeEnd936 = length(range934); index935 < rangeEnd936; ++index935) {
                        TailRangeGroup* group = range934[index935];
                        Int end = ((group)->checks[(length((group)->checks) - Int(1LL))] + Int(1LL));
                        if (((length(regions) == Int(0LL)) || ((group)->start > (regions[(length(regions) - Int(1LL))]).end))) {
                            regions = appendValues(regions, TailRangeRegion{{}, (group)->start, end});
                        }
                        TailRangeRegion* region = (&(regions[(length(regions) - Int(1LL))]));
                        (region)->end = maximum((region)->end, end);
                        (region)->groups = appendValues((region)->groups, group);
                    }
                }
                auto jump = [&](i32 target) mutable -> MInst {
                    MInst in = inst(mJmp);
                    (in).target = target;
                    return in;
                };
                i32 continuation = convert<i32>(b);
                Int start = Int(0LL);
                {
                    auto range937 = regions;
                    for (Int index938 = 0, rangeEnd939 = length(range937); index938 < rangeEnd939; ++index938) {
                        Int r = index938;
                        TailRangeRegion region = range937[index938];
                        Slice<i32> guards = makeSlice<i32>(length((region).groups));
                        Slice<i32> fast = makeSlice<i32>(length((region).groups));
                        Slice<i32> cold = makeSlice<i32>(length((region).groups));
                        {
                            auto range940 = (region).groups;
                            for (Int index941 = 0, rangeEnd942 = length(range940); index941 < rangeEnd942; ++index941) {
                                Int j = index941;
                                guards[j] = convert<i32>(length((mf)->blocks));
                                (mf)->blocks.pushBack(MBlock{{}, {}, {}, (original).depth, i32(-1LL), {}});
                                fast[j] = convert<i32>(length((mf)->blocks));
                                (mf)->blocks.pushBack(MBlock{{}, {}, {}, (original).depth, i32(-1LL), {}});
                                cold[j] = convert<i32>(length((mf)->blocks));
                                (mf)->blocks.pushBack(MBlock{{}, {}, {}, (original).depth, i32(-1LL), true});
                            }
                        }
                        Vec<MInst> prefix;
                        prefix.assign(subslice((original).insts, start, (region).start));
                        prefix.pushBack(jump(guards[Int(0LL)]));
                        ((mf)->blocks[continuation]).insts = move(prefix);
                        ((mf)->blocks[continuation]).succs = sliceOf<i32>({guards[Int(0LL)]});
                        continuation = convert<i32>(length((mf)->blocks));
                        (mf)->blocks.pushBack(MBlock{{}, {}, {}, (original).depth, i32(-1LL), {}});
                        {
                            auto range943 = (region).groups;
                            for (Int index944 = 0, rangeEnd945 = length(range943); index944 < rangeEnd945; ++index944) {
                                Int j = index944;
                                TailRangeGroup* group = range943[index944];
                                Int end = (region).end;
                                auto temporary946 = continuation;
                                auto temporary947 = continuation;
                                i32 nextFast = temporary946;
                                i32 nextCold = temporary947;
                                if (((j + Int(1LL)) < length((region).groups))) {
                                    end = ((region).groups[(j + Int(1LL))])->start;
                                    auto temporary948 = guards[(j + Int(1LL))];
                                    auto temporary949 = cold[(j + Int(1LL))];
                                    nextFast = temporary948;
                                    nextCold = temporary949;
                                }
                                MInst cmp = inst(mCmpMem);
                                auto temporary950 = (group)->src;
                                auto temporary951 = memAt(ctxReg, offMemSize);
                                auto temporary952 = u8(8LL);
                                (cmp).src = temporary950;
                                (cmp).mem = temporary951;
                                (cmp).size = temporary952;
                                MInst branch = inst(mJcc);
                                auto temporary953 = ccG;
                                auto temporary954 = cold[j];
                                (branch).cc = temporary953;
                                (branch).target = temporary954;
                                ((mf)->blocks[guards[j]]).insts.assign(Span<const MInst>({cmp, branch, jump(fast[j])}));
                                ((mf)->blocks[guards[j]]).succs = sliceOf<i32>({fast[j], cold[j]});
                                if (((l)->m)->tailProfile) {
                                    MInst counter = lowererTailCounter(l, i32(0LL), i64(1LL));
                                    Vec<MInst> counted({counter});
                                    counted.append(((mf)->blocks[guards[j]]).insts.view());
                                    ((mf)->blocks[guards[j]]).insts = move(counted);
                                    i32 miss = convert<i32>(length((mf)->blocks));
                                    (mf)->blocks.pushBack(MBlock{Vec<MInst>({lowererTailCounter(l, i32(8LL), i64(1LL)), jump(cold[j])}), sliceOf<i32>({cold[j]}), {}, (original).depth, i32(-1LL), true});
                                    (((mf)->blocks[guards[j]]).insts[Int(2LL)]).target = miss;
                                    ((mf)->blocks[guards[j]]).succs[Int(1LL)] = miss;
                                }
                                Vec<MInst> fastBody;
                                Vec<MInst> coldBody;
                                i64 omitted = i64(0LL);
                                {
                                    Int k = (group)->start;
                                    for (; (k < end); k++) {
                                        MInst in = (original).insts[k];
                                        coldBody.pushBack(copyMachineInst(in));
                                        if (skip[k]) {
                                            omitted++;
                                        } else {
                                            fastBody.pushBack(copyMachineInst(in));
                                        }
                                    }
                                }
                                if (((l)->m)->tailProfile) {
                                    Vec<MInst> counted({lowererTailCounter(l, i32(16LL), omitted)});
                                    counted.append(fastBody.view());
                                    fastBody = move(counted);
                                }
                                fastBody.pushBack(jump(nextFast));
                                ((mf)->blocks[fast[j]]).insts = move(fastBody);
                                coldBody.pushBack(jump(nextCold));
                                ((mf)->blocks[cold[j]]).insts = move(coldBody);
                                ((mf)->blocks[fast[j]]).succs = sliceOf<i32>({nextFast});
                                ((mf)->blocks[cold[j]]).succs = sliceOf<i32>({nextCold});
                            }
                        }
                        start = (region).end;
                        if (((r + Int(1LL)) == length(regions))) {
                            ((mf)->blocks[continuation]).insts.assign(subslice((original).insts, start, length((original).insts)));
                            ((mf)->blocks[continuation]).succs = appendSlice(Slice<i32>{}, (original).succs);
                        }
                    }
                }
            }
        }
        if (changed) {
            {
                auto& range955 = (mf)->blocks;
                for (Int index956 = 0, rangeEnd957 = length(range955); index956 < rangeEnd957; ++index956) {
                    Int b = index956;
                    ((mf)->blocks[b]).preds = {};
                }
            }
            {
                auto& range958 = (mf)->blocks;
                for (Int index959 = 0, rangeEnd960 = length(range958); index959 < rangeEnd960; ++index959) {
                    Int b = index959;
                    {
                        auto range961 = ((mf)->blocks[b]).succs;
                        for (Int index962 = 0, rangeEnd963 = length(range961); index962 < rangeEnd963; ++index962) {
                            i32 succ = range961[index962];
                            ((mf)->blocks[succ]).preds = appendValues(((mf)->blocks[succ]).preds, convert<i32>(b));
                        }
                    }
                }
            }
            Int n = length((mf)->blocks);
            {
                Int b = Int(0LL);
                for (; (b < n); b++) {
                    if ((length(((mf)->blocks[b]).succs) < Int(2LL))) {
                        continue;
                    }
                    {
                        auto range964 = ((mf)->blocks[b]).succs;
                        for (Int index965 = 0, rangeEnd966 = length(range964); index965 < rangeEnd966; ++index965) {
                            Int j = index965;
                            i32 succ = range964[index965];
                            if ((length(((mf)->blocks[succ]).preds) < Int(2LL))) {
                                continue;
                            }
                            i32 edge = convert<i32>(length((mf)->blocks));
                            MInst jump = inst(mJmp);
                            (jump).target = succ;
                            (mf)->blocks.pushBack(MBlock{Vec<MInst>({jump}), sliceOf<i32>({succ}), sliceOf<i32>({convert<i32>(b)}), ((mf)->blocks[b]).depth, i32(-1LL), (((mf)->blocks[b]).cold || ((mf)->blocks[succ]).cold)});
                            ((mf)->blocks[b]).succs[j] = edge;
                            {
                                auto range967 = ((mf)->blocks[b]).insts.view();
                                for (Int index968 = 0, rangeEnd969 = length(range967); index968 < rangeEnd969; ++index968) {
                                    Int k = index968;
                                    MInst* in = (&(((mf)->blocks[b]).insts[k]));
                                    {
                                        auto switch970 = (in)->op;
                                        switch (switch970) {
                                            case mJmp:
                                            case mJcc: {
                                                if (((in)->target == succ)) {
                                                    (in)->target = edge;
                                                }
                                                break;
                                            }
                                            case mSwitch: {
                                                {
                                                    auto range971 = mInstTable(in);
                                                    for (Int index972 = 0, rangeEnd973 = length(range971); index972 < rangeEnd973; ++index972) {
                                                        Int t = index972;
                                                        if ((mInstTable(in)[t] == succ)) {
                                                            mInstTable(in)[t] = edge;
                                                        }
                                                    }
                                                }
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                            {
                                auto range974 = ((mf)->blocks[succ]).preds;
                                for (Int index975 = 0, rangeEnd976 = length(range974); index975 < rangeEnd976; ++index975) {
                                    Int k = index975;
                                    i32 pred = range974[index975];
                                    if ((pred == convert<i32>(b))) {
                                        ((mf)->blocks[succ]).preds[k] = edge;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    static void metaWriterFlag(MetaWriter* w, bool b) {
        if (b) {
            metaWriterU(w, u64(1LL));
        } else {
            metaWriterU(w, u64(0LL));
        }
    }

    static void metaWriterStr(MetaWriter* w, String s) {
        metaWriterU(w, convert<u64>(length(s)));
        (w)->out = appendSlice((w)->out, s);
    }

    static void metaWriterU(MetaWriter* w, u64 x) {
        (w)->out = binaryAppendUvarint((w)->out, x);
    }

    static i32 objCodeAddBlob(ObjCode* oc, Slice<u8> b, Int align) {
        if (b.array && b.array->owner != oc->pool) {
            PoolScope persistent(*oc->pool);
            b = slicesClone(b);
        }
        (oc)->blobs = appendValues((oc)->blobs, CodeBlob{b, align});
        return convert<i32>((length((oc)->blobs) - Int(1LL)));
    }

    static void objCodeAddFixedBlobs(ObjCode* oc) {
        auto mask = [&](u64 lo, u64 hi) mutable -> Slice<u8> {
            Slice<u8> b = makeSlice<u8>(Int(16LL));
            littlePutUint64(b, lo);
            littlePutUint64(subslice(b, Int(8LL), length(b)), hi);
            return b;
        };
        objCodeAddBlob(oc, mask(u64(2147483648LL), u64(0LL)), Int(16LL));
        objCodeAddBlob(oc, mask(u64(2147483647LL), u64(0LL)), Int(16LL));
        objCodeAddBlob(oc, mask(u64(9223372036854775808ULL), u64(0LL)), Int(16LL));
        objCodeAddBlob(oc, mask(u64(9223372036854775807LL), u64(0LL)), Int(16LL));
        {
            auto range977 = ((oc)->m)->datas;
            for (Int index978 = 0, rangeEnd979 = length(range977); index978 < rangeEnd979; ++index978) {
                Data d = range977[index978];
                objCodeAddBlob(oc, (d).bytes, Int(1LL));
            }
        }
    }

    static void objCodeAlign16(ObjCode* oc) {
        {
            for (; ((length(((oc)->a).buf) % Int(16LL)) != Int(0LL));) {
                asmBytes(&((oc)->a), {u8(u8(204LL))});
            }
        }
    }

    // beginRegion starts a code region at a 16-byte boundary; endRegion records
    // it with its stack pointer table.
    static i32 objCodeBeginRegion(ObjCode* oc, String name) {
        PoolScope persistent(*oc->pool);
        name = cloneString(name);
        objCodeAlign16(oc);
        asmSetSP(&((oc)->a), i32(0LL));
        (oc)->regionName = name;
        return asmPos(&((oc)->a));
    }

    static void objCodeCallFunc(ObjCode* oc, Int fn) {
        (oc)->calls = appendValues((oc)->calls, CallFixup{asmCallRel(&((oc)->a)), convert<i32>(fn)});
    }

    // callGo calls the Go function value sym names: the closure context goes in
    // RDX, the code is its first word.
    static void objCodeCallGo(ObjCode* oc, String sym) {
        asmLoad(&((oc)->a), u8(8LL), u8(8LL), false, RDX, objCodeGotMem(oc, sym));
        asmCallMem(&((oc)->a), baseAt(RDX, i32(0LL)));
    }

    static void objCodeCallStub(ObjCode* oc, Int kind) {
        (oc)->stubCalls = appendValues((oc)->stubCalls, CallFixup{asmCallRel(&((oc)->a)), convert<i32>(kind)});
    }

    // description is what the symbols do not tell a loader: the instance, its
    // memory, tables and globals, the imports and exports of those.
    static Meta* objCodeDescription(ObjCode* oc) {
        Module* m = (oc)->m;
        Layout* lay = (oc)->lay;
        Meta* meta = activePool->make<Meta>(Meta{convert<u32>((lay)->size), {}, {}, {}, {}, {}, {}, {}});
        if ((length((m)->mems) > Int(0LL))) {
            Limits l = ((m)->mems[Int(0LL)]).limits;
            u64 max = u64(65536LL);
            if ((l).hasMax) {
                max = (l).max;
            }
            (meta)->memory = activePool->make<MemoryMeta>(MemoryMeta{(l).min, max, ((m)->mems[Int(0LL)]).imported, (m)->memoryTail, {}, {}, {}});
            if (((m)->memoryTail != u64(0LL))) {
                ((meta)->memory)->tailEnd = convert<u32>((lay)->memoryEnd);
            }
            if ((m)->tailProfile) {
                ((meta)->memory)->profile = convert<u32>((lay)->tailProfile);
                ((meta)->memory)->profileFuncs = convert<u32>(length((m)->funcs));
            }
        }
        {
            auto range980 = (m)->tables;
            for (Int index981 = 0, rangeEnd982 = length(range980); index981 < rangeEnd982; ++index981) {
                Int i = index981;
                Table t = range980[index981];
                u64 max = u64(4294967295LL);
                if (((t).limits).hasMax) {
                    max = ((t).limits).max;
                }
                (meta)->tables = appendValues((meta)->tables, TableMeta{convert<u32>((lay)->tables[i]), ((t).limits).min, max, (t).imported});
            }
        }
        {
            auto range983 = (m)->globals;
            for (Int index984 = 0, rangeEnd985 = length(range983); index984 < rangeEnd985; ++index984) {
                Int i = index984;
                Global g = range983[index984];
                (meta)->globals = appendValues((meta)->globals, GlobalMeta{convert<u32>((lay)->globals[i]), (g).typ, (g).mutableValue, (g).imported});
            }
        }
        {
            auto range986 = (m)->imports;
            for (Int index987 = 0, rangeEnd988 = length(range986); index987 < rangeEnd988; ++index987) {
                Import imp = range986[index987];
                if (((imp).kind != externFunc)) {
                    (meta)->imports = appendValues((meta)->imports, ImportMeta{(imp).kind, (imp).module, (imp).name, (imp).index, {}, {}, {}});
                }
            }
        }
        {
            auto range989 = (m)->exports;
            for (Int index990 = 0, rangeEnd991 = length(range989); index990 < rangeEnd991; ++index990) {
                Export e = range989[index990];
                if (((e).kind != externFunc)) {
                    (meta)->exports = appendValues((meta)->exports, ExportMeta{(e).kind, (e).name, (e).index, {}, {}, {}});
                }
            }
        }
        return meta;
    }

    static void objCodeEmitFunction(ObjCode* oc, Int index, MFunc* mf, Allocator* al) {
        Asm* a = (&((oc)->a));
        i32 start = objCodeBeginRegion(oc, objCodeFuncName(oc, index));
        (oc)->funcAt[index] = start;
        FuncEmitter* e = activePool->make<FuncEmitter>(FuncEmitter{oc, a, mf, al, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}});
        if (((oc)->m)->tailProfile) {
            (e)->profile = (((oc)->lay)->tailProfile + (convert<i32>(index) * tailCounterBytes));
        }
        (e)->pushes = allocatorCalleeSavedUsed(al);
        (e)->frame = (convert<i32>((mf)->stackArgs) + (i32(8LL) * (al)->nslots));
        (e)->framed = (((mf)->hasCalls || ((e)->frame > i32(0LL))) || (length((e)->pushes) > Int(0LL)));
        (e)->body = ((i32(8LL) * convert<i32>(length((e)->pushes))) + (e)->frame);
        if ((e)->framed) {
            (e)->body += i32(8LL);
        }
        if ((mf)->returnsTwice) {
            (e)->body += i32(8LL);
        }
        (e)->incoming = ((e)->body + i32(8LL));
        i32 slow = i32(-1LL);
        if (((mf)->hasCalls || ((e)->body > leafFrame))) {
            slow = objCodeStackCheck(oc, (e)->body, RAX);
        }
        if ((e)->framed) {
            asmPush(a, RBP);
            asmMovRR(a, u8(8LL), RBP, RSP);
        }
        if ((mf)->returnsTwice) {
            asmAluRR(a, aluXor, u8(4LL), RAX, RAX);
            asmPush(a, RAX);
        }
        {
            auto range992 = (e)->pushes;
            for (Int index993 = 0, rangeEnd994 = length(range992); index993 < rangeEnd994; ++index993) {
                VReg r = range992[index993];
                asmPush(a, r);
            }
        }
        if (((e)->frame > i32(0LL))) {
            asmAluRI(a, aluSub, u8(8LL), RSP, convert<i64>((e)->frame));
        }
        (e)->blockAt = makeSlice<i32>(length((mf)->blocks));
        {
            auto range995 = (e)->blockAt;
            for (Int index996 = 0, rangeEnd997 = length(range995); index996 < rangeEnd997; ++index996) {
                Int i = index996;
                (e)->blockAt[i] = i32(-1LL);
            }
        }
        Span<i32> order = mFuncHotOrder(mf, al->order.view());
        Slice<Int> positions = makeSlice<Int>(length((mf)->blocks));
        {
            auto range998 = order;
            for (Int index999 = 0, rangeEnd1000 = length(range998); index999 < rangeEnd1000; ++index999) {
                Int j = index999;
                i32 b = range998[index999];
                positions[b] = j;
            }
        }
        {
            auto range1001 = order;
            for (Int index1002 = 0, rangeEnd1003 = length(range1001); index1002 < rangeEnd1003; ++index1002) {
                Int i = index1002;
                i32 b = range1001[index1002];
                i32 next = i32(-1LL);
                if (((i + Int(1LL)) < length(order))) {
                    next = order[(i + Int(1LL))];
                }
                asmSetSP(a, (e)->body);
                {
                    auto range1004 = ((mf)->blocks[b]).preds;
                    for (Int index1005 = 0, rangeEnd1006 = length(range1004); index1005 < rangeEnd1006; ++index1005) {
                        i32 p = range1004[index1005];
                        if ((((!(((mf)->blocks[b]).cold)) && (!(((mf)->blocks[p]).cold))) && (positions[p] >= positions[b]))) {
                            asmNops(a, ((-(asmPos(a))) & i32(31LL)));
                            break;
                        }
                    }
                }
                (e)->blockAt[b] = asmPos(a);
                funcEmitterBlock(e, (&((mf)->blocks[b])), next);
            }
        }
        {
            Int i = Int(0LL);
            for (; (i < length((e)->late)); i++) {
                asmSetSP(a, (e)->body);
                (e)->late[i]();
            }
        }
        if ((slow >= i32(0LL))) {
            asmSetSP(a, i32(0LL));
            objCodeSlowPath(oc, slow, start);
        }
        Map<Array<i64, 2>, i32> stubs = mapOf<Array<i64, 2>, i32>({});
        {
            auto range1007 = (e)->traps;
            for (Int index1008 = 0, rangeEnd1009 = length(range1007); index1008 < rangeEnd1009; ++index1008) {
                TrapFixup t = range1007[index1008];
                Array<i64, 2> key = Array<i64, 2>{{(t).code, convert<i64>((t).sp)}};
                auto temporary1010 = lookupMap(stubs, key);
                i32 at = temporary1010.v0;
                bool ok = temporary1010.v1;
                if ((!(ok))) {
                    at = asmPos(a);
                    setMap(stubs, key, at);
                    asmSetSP(a, (t).sp);
                    objCodeTrapCall(oc, (t).code);
                }
                asmPatch32(a, (t).at, at);
            }
        }
        {
            auto range1011 = (e)->jumps;
            for (Int index1012 = 0, rangeEnd1013 = length(range1011); index1012 < rangeEnd1013; ++index1012) {
                JumpFixup j = range1011[index1012];
                asmPatch32(a, (j).at, (e)->blockAt[(j).block]);
            }
        }
        {
            auto range1014 = (e)->tables;
            for (Int index1015 = 0, rangeEnd1016 = length(range1014); index1015 < rangeEnd1016; ++index1015) {
                TableFixup t = range1014[index1015];
                {
                    for (; ((length((a)->buf) % Int(8LL)) != Int(0LL));) {
                        asmBytes(a, {u8(u8(204LL))});
                    }
                }
                i32 base = asmPos(a);
                asmPatch32(a, (t).lea, base);
                {
                    auto range1017 = (t).table;
                    for (Int index1018 = 0, rangeEnd1019 = length(range1017); index1018 < rangeEnd1019; ++index1018) {
                        i32 b = range1017[index1018];
                        (oc)->addresses = appendValues((oc)->addresses, TextAddress{asmPos(a), (e)->blockAt[b]});
                        asmU64(a, u64(0LL));
                    }
                }
            }
        }
        objCodeEndRegion(oc, start);
    }

    static void objCodeEndRegion(ObjCode* oc, i32 start) {
        PoolScope persistent(*oc->pool);
        i32 end = asmPos(&((oc)->a));
        (oc)->regions = appendValues((oc)->regions, Region{(oc)->regionName, convert<u32>(start), convert<u32>(end), asmSpTable(&((oc)->a), start, end)});
    }

    // exportThunks gives every exported function a thunk Go calls, global.
    static void objCodeExportThunks(ObjCode* oc) {
        Module* m = (oc)->m;
        {
            auto range1020 = (m)->exports;
            for (Int index1021 = 0, rangeEnd1022 = length(range1020); index1021 < rangeEnd1022; ++index1021) {
                Export e = range1020[index1021];
                if (((e).kind == externFunc)) {
                    FuncType* t = moduleFuncType(m, (e).index);
                    String sym = exportSymbol((e).name, t);
                    setMap((oc)->globals, sym, true);
                    objCodeThunk(oc, sym, (t)->params, (t)->results, convert<i64>((e).index), {});
                }
            }
        }
    }

    // extern returns the index of an external symbol.
    static i32 objCodeExtern(ObjCode* oc, String sym) {
        PoolScope persistent(*oc->pool);
        sym = cloneString(sym);
        {
            auto temporary1023 = lookupMap((oc)->externIx, sym);
            i32 i = temporary1023.v0;
            bool ok = temporary1023.v1;
            if (ok) {
                return i;
            }
        }
        i32 i = convert<i32>(length((oc)->externs));
        (oc)->externs = appendValues((oc)->externs, sym);
        setMap((oc)->externIx, sym, i);
        return i;
    }

    // floatConst returns a RIP-relative operand for a float constant.
    static Amem objCodeFloatConst(ObjCode* oc, u64 bits, u8 size) {
        Array<u64, 2> key = Array<u64, 2>{{bits, convert<u64>(size)}};
        auto temporary1024 = lookupMap((oc)->floatIx, key);
        i32 label = temporary1024.v0;
        bool ok = temporary1024.v1;
        if ((!(ok))) {
            Slice<u8> b = makeSlice<u8>(size);
            if ((size == u8(4LL))) {
                littlePutUint32(b, convert<u32>(bits));
            } else {
                littlePutUint64(b, bits);
            }
            label = objCodeAddBlob(oc, b, convert<i64>(size));
            setMap((oc)->floatIx, key, label);
        }
        return Amem{Int(-1LL), Int(-1LL), {}, {}, label, labelModule};
    }

    // framesStub emits the stub fork calls first, func(request uintptr), on a
    // framesRequest: it walks the frame pointers from the jump frame's caller up
    // while each returns into a function of the module, to the frame that
    // returns into the entry thunk or the resume stub, and copies those
    // frames, from the caller's
    // stack pointer to that return address, when they fit the buffer. In the
    // copy each saved RBP is an offset from its start, the top one 0. A leaf
    // that only reads the stack, it may be preempted anywhere.
    static void objCodeFramesStub(ObjCode* oc) {
        Asm* a = (&((oc)->a));
        auto word = [&](i32 k) mutable -> Amem {
            return baseAt(RAX, k);
        };
        i32 start = objCodeBeginRegion(oc, symFrames);
        setMap((oc)->globals, symFrames, true);
        asmLoad(a, u8(8LL), u8(8LL), false, RDX, baseAt(R14, gStackHi));
        asmLoad(a, u8(8LL), u8(8LL), false, R10, word(framesCtx));
        asmMovRR(a, u8(8LL), R9, RDX);
        asmAluRM(a, aluSub, u8(8LL), R9, baseAt(R10, offBase));
        asmMovRR(a, u8(8LL), RSI, R9);
        asmAluRM(a, aluSub, u8(8LL), RSI, word(framesSP));
        asmMovRR(a, u8(8LL), RDI, R9);
        asmAluRM(a, aluSub, u8(8LL), RDI, word(framesBP));
        i32 walk = asmPos(a);
        asmLoad(a, u8(8LL), u8(8LL), false, R8, baseAt(RDI, i32(8LL)));
        Slice<i32> top = {};
        Slice<i32> fail = {};
        {
            auto range1025 = Array<i32, 2>{{framesEntry, framesResume}};
            for (Int index1026 = 0, rangeEnd1027 = length(range1025); index1026 < rangeEnd1027; ++index1026) {
                i32 entry = range1025[index1026];
                asmAluRM(a, aluCmp, u8(8LL), R8, word(entry));
                i32 below = asmJcc8(a, ccB);
                asmAluRM(a, aluCmp, u8(8LL), R8, word((entry + i32(8LL))));
                top = appendValues(top, asmJccRel(a, ccB));
                asmHere8(a, below);
            }
        }
        asmAluRM(a, aluCmp, u8(8LL), R8, word(framesFuncs));
        fail = appendValues(fail, asmJccRel(a, ccB));
        asmAluRM(a, aluCmp, u8(8LL), R8, word(i32(32LL)));
        fail = appendValues(fail, asmJccRel(a, ccAE));
        asmLoad(a, u8(8LL), u8(8LL), false, R8, baseAt(RDI, i32(0LL)));
        asmAluRR(a, aluCmp, u8(8LL), R8, RDI);
        fail = appendValues(fail, asmJccRel(a, ccBE));
        asmAluRR(a, aluCmp, u8(8LL), R8, RDX);
        fail = appendValues(fail, asmJccRel(a, ccAE));
        asmMovRR(a, u8(8LL), RDI, R8);
        asmPatch32(a, asmJmpRel(a), walk);
        {
            auto range1028 = top;
            for (Int index1029 = 0, rangeEnd1030 = length(range1028); index1029 < rangeEnd1030; ++index1029) {
                i32 at = range1028[index1029];
                asmPatch32(a, at, asmPos(a));
            }
        }
        asmLea(a, u8(8LL), RCX, baseAt(RDI, i32(16LL)));
        asmAluRR(a, aluSub, u8(8LL), RCX, RSI);
        asmStore(a, u8(8LL), RCX, word(framesSize));
        asmMovRR(a, u8(8LL), R11, RDI);
        asmAluRR(a, aluSub, u8(8LL), R11, RSI);
        asmStore(a, u8(8LL), R11, word(framesTop));
        asmAluRM(a, aluCmp, u8(8LL), RCX, word(framesCap));
        i32 small = asmJccRel(a, ccA);
        asmMovRR(a, u8(8LL), RBX, RSI);
        asmLoad(a, u8(8LL), u8(8LL), false, R10, word(framesBuf));
        asmMovRR(a, u8(8LL), RDI, R10);
        asmRepMovsb(a);
        asmLoad(a, u8(8LL), u8(8LL), false, R8, word(framesSP));
        asmAluRM(a, aluSub, u8(8LL), R8, word(framesBP));
        i32 relink = asmPos(a);
        asmAluRR(a, aluCmp, u8(8LL), R8, R11);
        i32 last = asmJcc8(a, ccE);
        asmLoad(a, u8(8LL), u8(8LL), false, RCX, indexed(R10, R8));
        asmAluRR(a, aluSub, u8(8LL), RCX, RBX);
        asmStore(a, u8(8LL), RCX, indexed(R10, R8));
        asmMovRR(a, u8(8LL), R8, RCX);
        asmPatch8(a, asmJmp8(a), relink);
        asmHere8(a, last);
        asmStoreImm(a, u8(8LL), indexed(R10, R11), i64(0LL));
        asmPatch32(a, small, asmPos(a));
        asmRet(a);
        {
            auto range1031 = fail;
            for (Int index1032 = 0, rangeEnd1033 = length(range1031); index1032 < rangeEnd1033; ++index1032) {
                i32 at = range1031[index1032];
                asmPatch32(a, at, asmPos(a));
            }
        }
        asmStoreImm(a, u8(8LL), word(framesSize), i64(0LL));
        asmRet(a);
        objCodeEndRegion(oc, start);
    }

    // funcName names a function's region, which Go shows in tracebacks.
    static String objCodeFuncName(ObjCode* oc, Int i) {
        {
            String n = (((oc)->m)->funcs[i]).name;
            if ((n != String(""))) {
                return fmtSprintf(String("f%d:%s"), i, displayName(n));
            }
        }
        return fmtSprintf(String("f%d"), i);
    }

    // goCall emits a function of the internal convention that calls the Go
    // function value of sym with the instance and its parameters, and returns
    // its results; with record it writes the jump frame first. When the
    // instance asks, by its interrupt word, it calls the interrupt stub on the
    // way out, as Go's runtime would inject a call of it there.
    static i32 objCodeGoCall(ObjCode* oc, String name, Slice<ValType> params, Slice<ValType> results, String sym, bool record) {
        Asm* a = (&((oc)->a));
        GoFrame g = goAssign(params, results);
        auto temporary1034 = internalLocs(params, argGPRs, argXMMs, Int(0LL));
        Slice<VReg> pregs = temporary1034.v0;
        Slice<Int> pslots = temporary1034.v1;
        Int nslots = temporary1034.v2;
        auto temporary1035 = internalLocs(results, resultGPRs, resultXMMs, nslots);
        Slice<VReg> rregs = temporary1035.v0;
        Slice<Int> rslots = temporary1035.v1;
        Slice<VReg> saved = sliceOf<VReg>({R10, R11, R12, ctxReg});
        i32 total = ((i32(8LL) + (i32(8LL) * convert<i32>(length(saved)))) + (g).size);
        i32 start = objCodeBeginRegion(oc, name);
        i32 slow = objCodeStackCheck(oc, total, RAX);
        asmPush(a, RBP);
        asmMovRR(a, u8(8LL), RBP, RSP);
        {
            auto range1036 = saved;
            for (Int index1037 = 0, rangeEnd1038 = length(range1036); index1037 < rangeEnd1038; ++index1037) {
                VReg r = range1036[index1037];
                asmPush(a, r);
            }
        }
        if (((g).size > i32(0LL))) {
            asmAluRI(a, aluSub, u8(8LL), RSP, convert<i64>((g).size));
        }
        i32 incoming = (total + i32(8LL));
        if (record) {
            objCodeRecordFrame(oc);
        }
        {
            auto range1039 = params;
            for (Int index1040 = 0, rangeEnd1041 = length(range1039); index1040 < rangeEnd1041; ++index1040) {
                Int i = index1040;
                ValType t = range1039[index1040];
                GoLoc dst = (g).params[i];
                {
                    if (((pregs[i] != noReg) && ((dst).reg == pregs[i]))) {
                    } else if (((pregs[i] == noReg) && ((dst).reg != noReg))) {
                        asmLoad(a, u8(8LL), u8(8LL), false, (dst).reg, rspAt((incoming + convert<i32>((Int(8LL) * pslots[i])))));
                    } else if ((pregs[i] == noReg)) {
                        asmLoad(a, u8(8LL), u8(8LL), false, RDX, rspAt((incoming + convert<i32>((Int(8LL) * pslots[i])))));
                        asmStore(a, convert<u8>(goSize(t)), RDX, rspAt((dst).off));
                    } else {
                        compilerError(String("internal error: parameter %d of %s moves between registers"));
                    }
                switchEnd1043:;
                }
            }
        }
        asmMovRR(a, u8(8LL), RAX, ctxReg);
        objCodeCallGo(oc, sym);
        {
            auto range1044 = results;
            for (Int index1045 = 0, rangeEnd1046 = length(range1044); index1045 < rangeEnd1046; ++index1045) {
                Int i = index1045;
                ValType t = range1044[index1045];
                GoLoc src = (g).results[i];
                {
                    if (((rregs[i] != noReg) && ((src).reg == rregs[i]))) {
                        if ((t == I32)) {
                            asmMovRR(a, u8(4LL), (src).reg, (src).reg);
                        }
                    } else if (((rregs[i] == noReg) && ((src).reg != noReg))) {
                        asmStore(a, u8(8LL), (src).reg, rspAt((incoming + convert<i32>((Int(8LL) * rslots[i])))));
                    } else if ((rregs[i] == noReg)) {
                        asmLoad(a, convert<u8>(goSize(t)), u8(8LL), false, RDX, rspAt((src).off));
                        asmStore(a, u8(8LL), RDX, rspAt((incoming + convert<i32>((Int(8LL) * rslots[i])))));
                    } else {
                        compilerError(String("internal error: result %d of %s moves between registers"));
                    }
                switchEnd1048:;
                }
            }
        }
        if (((g).size > i32(0LL))) {
            asmAluRI(a, aluAdd, u8(8LL), RSP, convert<i64>((g).size));
        }
        {
            Int i = (length(saved) - Int(1LL));
            for (; (i >= Int(0LL)); i--) {
                asmPop(a, saved[i]);
            }
        }
        asmLoad(a, u8(8LL), u8(8LL), false, memReg, baseAt(ctxReg, offMem));
        asmAluMI(a, aluCmp, u8(4LL), baseAt(ctxReg, offInterrupt), i64(0LL));
        i32 interrupt = asmJcc8(a, ccNE);
        i32 framed = (a)->sp;
        asmPop(a, RBP);
        asmRet(a);
        asmSetSP(a, framed);
        asmHere8(a, interrupt);
        objCodeCallStub(oc, stubInterrupt);
        asmPop(a, RBP);
        asmRet(a);
        asmSetSP(a, i32(0LL));
        objCodeSlowPath(oc, slow, start);
        objCodeEndRegion(oc, start);
        return start;
    }

    // gotMem is the GOT entry of a symbol: it holds the symbol's address.
    static Amem objCodeGotMem(ObjCode* oc, String sym) {
        return Amem{Int(-1LL), Int(-1LL), {}, {}, objCodeExtern(oc, sym), labelGot};
    }

    // importAdapters give imported functions an entry in the internal
    // convention: an adapter calls the Go function value of the import's
    // symbol, which names it with its type. One that returns twice has a second
    // adapter, for its direct calls, which writes the jump frame; the module
    // then has the land stub (goabi.go). Tables and exports take the first,
    // as their callers keep no token.
    static void objCodeImportAdapters(ObjCode* oc) {
        Module* m = (oc)->m;
        {
            auto range1049 = (m)->imports;
            for (Int index1050 = 0, rangeEnd1051 = length(range1049); index1050 < rangeEnd1051; ++index1050) {
                Import imp = range1049[index1050];
                if (((imp).kind != externFunc)) {
                    continue;
                }
                FuncType* t = moduleFuncType(m, (imp).index);
                String sym = importSymbol((imp).module, (imp).name, t);
                String name = (((String("w2o.adapter.") + symbolPart((imp).module)) + String(".")) + symbolPart((imp).name));
                (oc)->funcAt[(imp).index] = objCodeGoCall(oc, name, (t)->params, (t)->results, sym, false);
                if (((m)->funcs[(imp).index]).returnsTwice) {
                    setMap((oc)->jumpAt, convert<i32>((imp).index), objCodeGoCall(oc, (name + String(".jump")), (t)->params, (t)->results, sym, true));
                }
            }
        }
        if ((length((oc)->jumpAt) > Int(0LL))) {
            objCodeLandStub(oc);
            objCodeFramesStub(oc);
            objCodeResumeStub(oc);
        }
    }

    // Every destination was checked against the private memory's minimum.
    // Sources remain the original data blobs; the table preserves segment
    // order, overlapping writes, and each segment's dropped state.
    static void objCodeInitData(ObjCode* oc) {
        Module* m = oc->m;
        Asm* a = &oc->a;
        Int count = 0;
        for (Int i = 0; i < length(m->datas); ++i) {
            count += m->datas[i].mode == segmentActive;
        }
        Slice<u8> table = makeSlice<u8>(count * 16);
        Int row = 0;
        for (Int i = 0; i < length(m->datas); ++i) {
            const Data& data = m->datas[i];
            if (data.mode != segmentActive) {
                continue;
            }
            auto entry = subslice(table, row++ * 16, length(table));
            littlePutUint32(entry, u32(constOffset(data.offset).v0));
            littlePutUint32(subslice(entry, 4, length(entry)), u32(length(data.bytes)));
            littlePutUint32(subslice(entry, 12, length(entry)), u32(layoutDataDropped(oc->lay, i)));
        }
        oc->dataInitTable = objCodeAddBlob(oc, table, 4);
        asmLea(a, 8, R8, Amem{-1, -1, {}, {}, oc->dataInitTable, labelModule});
        asmMovImm(a, 4, R9, count);
        const i32 loop = asmPos(a);
        asmLoad(a, 4, 4, false, RDI, baseAt(R8, 0));
        asmAluRR(a, aluAdd, 8, RDI, memReg);
        asmLoad(a, 4, 4, false, RCX, baseAt(R8, 4));
        asmLoad(a, 4, 8, true, RSI, baseAt(R8, 8));
        asmAluRR(a, aluAdd, 8, RSI, R8);
        asmBytes(a, {0xf3, 0xa4});
        asmLoad(a, 4, 4, false, RAX, baseAt(R8, 12));
        asmStoreImm(a, 1, indexed(ctxReg, RAX), 1);
        asmAluRI(a, aluAdd, 8, R8, 16);
        asmAluRI(a, aluSub, 4, R9, 1);
        asmPatch32(a, asmJccRel(a, ccNE), loop);
    }

    // initThunk fills function descriptors and active elements, then sets
    // globals, initializes data and calls start. Large, statically safe data
    // initializers run here; the other cases stay in the generated function.
    static void objCodeInitThunk(ObjCode* oc) {
        Module* m = (oc)->m;
        Layout* lay = (oc)->lay;
        Asm* a = (&((oc)->a));
        setMap((oc)->globals, symInit, true);
        objCodeThunk(oc, symInit, {}, {}, Int(-1LL), [&]() mutable -> void {
            {
                auto range1052 = (m)->funcs;
                for (Int index1053 = 0, rangeEnd1054 = length(range1052); index1053 < rangeEnd1054; ++index1053) {
                    Int i = index1053;
                    i32 d = layoutDesc(lay, i);
                    asmBytes(a, {u8(u8(72LL)), u8(u8(141LL)), u8(u8(5LL))});
                    (a)->fixups = appendValues((a)->fixups, RipFixup{asmPos(a), (asmPos(a) + i32(4LL)), convert<i32>(i), labelFunc});
                    asmU32(a, u32(0LL));
                    asmStore(a, u8(8LL), RAX, baseAt(ctxReg, d));
                    asmMovImm(a, u8(8LL), RAX, convert<i64>(typeID(moduleFuncType(m, convert<u32>(i)))));
                    asmStore(a, u8(8LL), RAX, baseAt(ctxReg, (d + i32(8LL))));
                }
            }
            {
                auto range1055 = (m)->elems;
                for (Int index1056 = 0, rangeEnd1057 = length(range1055); index1056 < rangeEnd1057; ++index1056) {
                    Int i = index1056;
                    Elem e = range1055[index1056];
                    if (((e).mode == segmentActive)) {
                        auto temporary1058 = constOffset((e).offset);
                        i64 off = temporary1058.v0;
                        bool ok = temporary1058.v1;
                        if ((!(ok))) {
                            compilerError(String("unsupported: element segment %d with a computed offset"));
                        }
                        i32 tb = (lay)->tables[(e).table];
                        i64 n = convert<i64>(length((e).exprs));
                        Slice<u8> arr = makeSlice<u8>(Int(0LL), (i64(4LL) * n));
                        {
                            auto range1059 = (e).exprs;
                            for (Int index1060 = 0, rangeEnd1061 = length(range1059); index1060 < rangeEnd1061; ++index1060) {
                                Slice<u8> x = range1059[index1060];
                                i32 d = {};
                                if ((x[Int(0LL)] == u8(210LL))) {
                                    Reader* r = activePool->make<Reader>(Reader{subslice(x, Int(1LL), length(x)), {}});
                                    d = layoutDesc(lay, convert<i64>(readerU32(r)));
                                }
                                arr = littleAppendUint32(arr, convert<u32>(d));
                            }
                        }
                        asmLoad(a, u8(4LL), u8(4LL), false, RAX, baseAt(ctxReg, (tb + tableSize)));
                        asmMovImm(a, u8(8LL), RCX, (off + n));
                        asmAluRR(a, aluCmp, u8(8LL), RAX, RCX);
                        i32 fits = asmJcc8(a, ccAE);
                        objCodeTrapCall(oc, trapTableOOB);
                        asmHere8(a, fits);
                        if ((n > i64(0LL))) {
                            i32 label = objCodeAddBlob(oc, arr, Int(4LL));
                            asmLea(a, u8(8LL), RSI, Amem{Int(-1LL), Int(-1LL), {}, {}, label, labelModule});
                            asmLoad(a, u8(8LL), u8(8LL), false, RDI, baseAt(ctxReg, (tb + tableEntries)));
                            asmMovImm(a, u8(8LL), RAX, (i64(8LL) * off));
                            asmAluRR(a, aluAdd, u8(8LL), RDI, RAX);
                            asmMovImm(a, u8(4LL), RCX, n);
                            i32 loop = asmPos(a);
                            asmOpRM(a, true, Span<const u8>({u8(99LL)}), Int(0LL), baseAt(RSI, i32(0LL)), Int(0LL), false);
                            asmTest(a, u8(8LL), RAX, RAX);
                            i32 null = asmJcc8(a, ccE);
                            asmAluRR(a, aluAdd, u8(8LL), RAX, ctxReg);
                            asmHere8(a, null);
                            asmStore(a, u8(8LL), RAX, baseAt(RDI, i32(0LL)));
                            asmAluRI(a, aluAdd, u8(8LL), RSI, i64(4LL));
                            asmAluRI(a, aluAdd, u8(8LL), RDI, i64(8LL));
                            asmAluRI(a, aluSub, u8(4LL), RCX, i64(1LL));
                            asmPatch32(a, asmJccRel(a, ccNE), loop);
                        }
                    }
                    if (((e).mode != segmentPassive)) {
                        asmStoreImm(a, u8(1LL), baseAt(ctxReg, layoutElemDropped(lay, m, i)), i64(1LL));
                    }
                }
            }
            objCodeCallFunc(oc, (oc)->initFn);
            if (m->batchData) {
                objCodeInitData(oc);
                if (m->start >= 0) {
                    objCodeCallFunc(oc, Int(m->start));
                }
            }
        });
    }

    // interruptStub emits what Go's runtime injects a call of, from a
    // preemption signal, at an instruction of a function of the module when
    // Go was asked to interrupt the code (lib/goruntime): it calls the Go
    // function value of w2o.interrupt with the instance and returns to that
    // instruction with every register and flag as it was, but the memory
    // register, read again. The frame below its saved RBP is a frame of
    // registers, which hold no stack address and no Go pointer of their own.
    static i32 objCodeInterruptStub(ObjCode* oc) {
        Asm* a = (&((oc)->a));
        Slice<VReg> saved = sliceOf<VReg>({RAX, RBX, RCX, RDX, RSI, RDI, R8, R9, R10, R11, R12, R13});
        i32 xmms = i32(120LL);
        i32 spill = (goAssign({}, {})).size;
        i32 start = objCodeBeginRegion(oc, symInterruptCall);
        setMap((oc)->globals, symInterruptCall, true);
        asmPush(a, RBP);
        asmMovRR(a, u8(8LL), RBP, RSP);
        asmPushf(a);
        {
            auto range1062 = saved;
            for (Int index1063 = 0, rangeEnd1064 = length(range1062); index1063 < rangeEnd1064; ++index1063) {
                VReg r = range1062[index1063];
                asmPush(a, r);
            }
        }
        asmAluRI(a, aluSub, u8(8LL), RSP, convert<i64>((xmms + spill)));
        {
            auto range1065 = Int(15LL);
            for (Int index1066 = 0, rangeEnd1067 = range1065; index1066 < rangeEnd1067; ++index1066) {
                Int i = index1066;
                asmFstore(a, u8(8LL), (XMM0 + convert<VReg>(i)), rspAt((spill + convert<i32>((Int(8LL) * i)))));
            }
        }
        asmMovRR(a, u8(8LL), RAX, ctxReg);
        objCodeCallGo(oc, symInterrupt);
        {
            auto range1068 = Int(15LL);
            for (Int index1069 = 0, rangeEnd1070 = range1068; index1069 < rangeEnd1070; ++index1069) {
                Int i = index1069;
                asmFload(a, u8(8LL), (XMM0 + convert<VReg>(i)), rspAt((spill + convert<i32>((Int(8LL) * i)))));
            }
        }
        asmAluRI(a, aluAdd, u8(8LL), RSP, convert<i64>((xmms + spill)));
        {
            Int i = (length(saved) - Int(1LL));
            for (; (i >= Int(0LL)); i--) {
                asmPop(a, saved[i]);
            }
        }
        asmLoad(a, u8(8LL), u8(8LL), false, memReg, baseAt(ctxReg, offMem));
        asmPopf(a);
        asmPop(a, RBP);
        asmRet(a);
        objCodeEndRegion(oc, start);
        return start;
    }

    // landStub emits the stub longjmp calls, func(jump uintptr, val uint32,
    // ctx uintptr) uint32, jump a Jump (jump.go): its jump frame, then the code
    // of the caller's function, [start, end). It returns from the call that
    // wrote the frame again, with val, when the caller's RBP is on the chain of
    // frame pointers from its own caller, the frame below returns into that
    // code and the token is the frame's: the invocation is the one that made
    // the call. Else it returns 0.
    //
    // The switch is a ret, so the stack pointer table says 0 throughout:
    // before it the stub is a leaf of its Go caller, after it a leaf called
    // from the landing site, whose return address it stores at the new top.
    static void objCodeLandStub(ObjCode* oc) {
        Asm* a = (&((oc)->a));
        auto word = [&](i32 k) mutable -> Amem {
            return baseAt(RAX, (i32(8LL) * k));
        };
        i32 start = objCodeBeginRegion(oc, symLand);
        setMap((oc)->globals, symLand, true);
        asmLoad(a, u8(8LL), u8(8LL), false, RDX, baseAt(R14, gStackHi));
        asmMovRR(a, u8(8LL), R9, RDX);
        asmAluRM(a, aluSub, u8(8LL), R9, baseAt(RCX, offBase));
        asmMovRR(a, u8(8LL), RSI, R9);
        asmAluRM(a, aluSub, u8(8LL), RSI, word(jumpBP));
        asmMovRR(a, u8(8LL), RDI, RBP);
        i32 walk = asmPos(a);
        asmLoad(a, u8(8LL), u8(8LL), false, R8, baseAt(RDI, i32(0LL)));
        asmAluRR(a, aluCmp, u8(8LL), R8, RSI);
        i32 found = asmJcc8(a, ccE);
        asmAluRR(a, aluCmp, u8(8LL), R8, RDI);
        i32 down = asmJccRel(a, ccBE);
        asmAluRR(a, aluCmp, u8(8LL), R8, RDX);
        i32 above = asmJccRel(a, ccAE);
        asmMovRR(a, u8(8LL), RDI, R8);
        asmPatch8(a, asmJmp8(a), walk);
        asmHere8(a, found);
        asmLoad(a, u8(8LL), u8(8LL), false, R8, baseAt(RDI, i32(8LL)));
        asmAluRM(a, aluCmp, u8(8LL), R8, word(jumpWords));
        i32 before = asmJccRel(a, ccB);
        asmAluRM(a, aluCmp, u8(8LL), R8, word(i32(8LL)));
        i32 after = asmJccRel(a, ccAE);
        asmLoad(a, u8(8LL), u8(8LL), false, R8, baseAt(RSI, i32(-8LL)));
        asmAluRM(a, aluCmp, u8(8LL), R8, word(jumpToken));
        i32 other = asmJccRel(a, ccNE);
        asmMovRR(a, u8(8LL), ctxReg, RCX);
        asmLoad(a, u8(8LL), u8(8LL), false, memReg, baseAt(ctxReg, offMem));
        asmLoad(a, u8(8LL), u8(8LL), false, R10, word(jumpR10));
        asmLoad(a, u8(8LL), u8(8LL), false, R11, word(jumpR11));
        asmLoad(a, u8(8LL), u8(8LL), false, R12, word(jumpR12));
        asmMovRR(a, u8(8LL), R8, R9);
        asmAluRM(a, aluSub, u8(8LL), R8, word(jumpSP));
        asmAluRI(a, aluSub, u8(8LL), R8, i64(8LL));
        asmLoad(a, u8(8LL), u8(8LL), false, R9, word(jumpPC));
        asmStore(a, u8(8LL), R9, baseAt(R8, i32(0LL)));
        asmMovRR(a, u8(8LL), RBP, RSI);
        asmMovRR(a, u8(4LL), RAX, RBX);
        asmMovRR(a, u8(8LL), RSP, R8);
        asmRet(a);
        {
            auto range1071 = Array<i32, 5>{{down, above, before, after, other}};
            for (Int index1072 = 0, rangeEnd1073 = length(range1071); index1072 < rangeEnd1073; ++index1072) {
                i32 at = range1071[index1072];
                asmPatch32(a, at, asmPos(a));
            }
        }
        asmAluRR(a, aluXor, u8(4LL), RAX, RAX);
        asmRet(a);
        objCodeEndRegion(oc, start);
    }

    // placeBlobs lays the constants and data segments out in .rodata.
    static void objCodePlaceBlobs(ObjCode* oc) {
        oc->blobAt = makeSlice<i32>(length(oc->blobs));
        u64 offset = 0;
        for (Int i = 0; i < length(oc->blobs); ++i) {
            const CodeBlob& blob = oc->blobs[i];
            offset = alignUp(usize(offset), usize(blob.align));
            require(offset <= 0x7fffffff, "rodata offset overflow");
            oc->blobAt[i] = i32(offset);
            offset += length(blob.bytes);
        }
        oc->rodataSize = offset;
        if (oc->dataInitTable >= 0) {
            auto table = oc->blobs[oc->dataInitTable].bytes;
            const i32 base = oc->blobAt[oc->dataInitTable];
            Int row = 0;
            for (Int i = 0; i < length(oc->m->datas); ++i) {
                if (oc->m->datas[i].mode != segmentActive) {
                    continue;
                }
                const i32 at = row++ * 16;
                const i64 source = i64(oc->blobAt[dataLabel(i)]) - (i64(base) + at);
                require(source >= -2147483648LL && source <= 2147483647LL, "data initializer displacement overflow");
                littlePutUint32(subslice(table, at + 8, length(table)), u32(source));
            }
        }
    }

    // recordFrame writes the jump frame in an adapter, whose RBP is set while
    // the caller's registers are still in theirs; RAX and RDX carry nothing.
    // The caller's token is handed out from the instance's count when the slot
    // is still zero.
    static void objCodeRecordFrame(ObjCode* oc) {
        Asm* a = (&((oc)->a));
        auto word = [&](i32 k) mutable -> Amem {
            return baseAt(ctxReg, (offJump + (i32(8LL) * k)));
        };
        asmLoad(a, u8(8LL), u8(8LL), false, RAX, baseAt(RBP, i32(8LL)));
        asmStore(a, u8(8LL), RAX, word(jumpPC));
        asmLoad(a, u8(8LL), u8(8LL), false, RAX, baseAt(R14, gStackHi));
        asmAluRM(a, aluSub, u8(8LL), RAX, baseAt(ctxReg, offBase));
        asmAluRR(a, aluSub, u8(8LL), RAX, RBP);
        asmAluRI(a, aluSub, u8(8LL), RAX, i64(16LL));
        asmStore(a, u8(8LL), RAX, word(jumpSP));
        asmLoad(a, u8(8LL), u8(8LL), false, RDX, baseAt(RBP, i32(0LL)));
        asmLoad(a, u8(8LL), u8(8LL), false, RAX, baseAt(R14, gStackHi));
        asmAluRM(a, aluSub, u8(8LL), RAX, baseAt(ctxReg, offBase));
        asmAluRR(a, aluSub, u8(8LL), RAX, RDX);
        asmStore(a, u8(8LL), RAX, word(jumpBP));
        asmStore(a, u8(8LL), R10, word(jumpR10));
        asmStore(a, u8(8LL), R11, word(jumpR11));
        asmStore(a, u8(8LL), R12, word(jumpR12));
        asmLoad(a, u8(8LL), u8(8LL), false, RAX, baseAt(RDX, i32(-8LL)));
        asmTest(a, u8(8LL), RAX, RAX);
        i32 have = asmJcc8(a, ccNE);
        asmLoad(a, u8(8LL), u8(8LL), false, RAX, baseAt(ctxReg, offTokens));
        asmAluRI(a, aluAdd, u8(8LL), RAX, i64(1LL));
        asmStore(a, u8(8LL), RAX, baseAt(ctxReg, offTokens));
        asmStore(a, u8(8LL), RAX, baseAt(RDX, i32(-8LL)));
        asmHere8(a, have);
        asmStore(a, u8(8LL), RAX, word(jumpToken));
    }

    // resolve fills the displacements within the text and turns the others
    // into relocations: a displacement is relative to the end of its
    // instruction, so the addend takes that distance off.
    static void objCodeResolve(ObjCode* oc) {
        Asm* a = (&((oc)->a));
        {
            auto range1077 = (a)->fixups;
            for (Int index1078 = 0, rangeEnd1079 = length(range1077); index1078 < rangeEnd1079; ++index1078) {
                RipFixup f = range1077[index1078];
                {
                    auto switch1080 = (f).kind;
                    switch (switch1080) {
                        case labelModule: {
                            (oc)->relocs = appendValues((oc)->relocs, TextReloc{(f).at, i32(-1LL), (convert<i64>((oc)->blobAt[(f).label]) - convert<i64>(((f).end - (f).at)))});
                            break;
                        }
                        case labelGot: {
                            (oc)->relocs = appendValues((oc)->relocs, TextReloc{(f).at, (f).label, (-(convert<i64>(((f).end - (f).at))))});
                            break;
                        }
                        case labelFunc: {
                            littlePutUint32(subslice((a)->buf, (f).at, length((a)->buf)), convert<u32>(((oc)->funcAt[(f).label] - (f).end)));
                            break;
                        }
                        default: {
                            compilerError(String("internal error: unresolved local label"));
                            break;
                        }
                    }
                }
            }
        }
        {
            auto range1081 = (oc)->calls;
            for (Int index1082 = 0, rangeEnd1083 = length(range1081); index1082 < rangeEnd1083; ++index1082) {
                CallFixup c = range1081[index1082];
                asmPatch32(a, (c).at, (oc)->funcAt[(c).fn]);
            }
        }
        {
            auto range1084 = (oc)->stubCalls;
            for (Int index1085 = 0, rangeEnd1086 = length(range1084); index1085 < rangeEnd1086; ++index1085) {
                CallFixup c = range1084[index1085];
                asmPatch32(a, (c).at, (oc)->stubAt[(c).fn]);
            }
        }
        {
            auto range1087 = (oc)->jumpCalls;
            for (Int index1088 = 0, rangeEnd1089 = length(range1087); index1088 < rangeEnd1089; ++index1088) {
                CallFixup c = range1087[index1088];
                asmPatch32(a, (c).at, (oc)->jumpAt[(c).fn]);
            }
        }
    }

    // resumeStub emits the stub a child of fork enters by, func(request
    // uintptr), on a resumeRequest: with a frame of its own, it places the
    // frames below its stack pointer, links them to its frame and returns from
    // the call of the jump frame with 0, the instance's base set so that the
    // parent's offsets hold. The top frame returns into the stub, which
    // returns to Go. Writing below its stack pointer, it must not be preempted
    // (gort.go); first it grows the stack until the frames fit.
    static void objCodeResumeStub(ObjCode* oc) {
        Asm* a = (&((oc)->a));
        auto word = [&](i32 k) mutable -> Amem {
            return baseAt(RAX, k);
        };
        i32 start = objCodeBeginRegion(oc, symResume);
        setMap((oc)->globals, symResume, true);
        asmLoad(a, u8(8LL), u8(8LL), false, ctxReg, word(resumeCtx));
        asmLea(a, u8(8LL), RDX, rspAt(i32(-512LL)));
        asmAluRM(a, aluSub, u8(8LL), RDX, word(resumeSize));
        asmAluRM(a, aluCmp, u8(8LL), RDX, baseAt(R14, gStackguard0));
        i32 grow = asmJccRel(a, ccBE);
        asmPush(a, RBP);
        asmMovRR(a, u8(8LL), RBP, RSP);
        asmMovRR(a, u8(8LL), RDI, RSP);
        asmAluRM(a, aluSub, u8(8LL), RDI, word(resumeSize));
        asmMovRR(a, u8(8LL), RBX, RDI);
        asmLoad(a, u8(8LL), u8(8LL), false, RSI, word(resumeBuf));
        asmLoad(a, u8(8LL), u8(8LL), false, RCX, word(resumeSize));
        asmRepMovsb(a);
        asmLoad(a, u8(8LL), u8(8LL), false, R8, word(resumeBP));
        asmLoad(a, u8(8LL), u8(8LL), false, R11, word(resumeTop));
        i32 relink = asmPos(a);
        asmAluRR(a, aluCmp, u8(8LL), R8, R11);
        i32 last = asmJcc8(a, ccE);
        asmLoad(a, u8(8LL), u8(8LL), false, RCX, indexed(RBX, R8));
        asmLea(a, u8(8LL), R9, indexed(RBX, RCX));
        asmStore(a, u8(8LL), R9, indexed(RBX, R8));
        asmMovRR(a, u8(8LL), R8, RCX);
        asmPatch8(a, asmJmp8(a), relink);
        asmHere8(a, last);
        asmStore(a, u8(8LL), RBP, indexed(RBX, R11));
        asmRex(a, true, Int(9LL), Int(-1LL), Int(-1LL), false);
        asmBytes(a, {u8(u8(141LL)), u8(u8(shiftLeft(lo3(Int(9LL)), Int(3LL)) | u8(5LL)))});
        i32 contAt = asmPos(a);
        asmU32(a, u32(0LL));
        asmLea(a, u8(8LL), RCX, indexed(RBX, R11));
        asmStore(a, u8(8LL), R9, baseAt(RCX, i32(8LL)));
        asmLoad(a, u8(8LL), u8(8LL), false, R9, baseAt(R14, gStackHi));
        asmAluRR(a, aluSub, u8(8LL), R9, RBX);
        asmAluRM(a, aluSub, u8(8LL), R9, word(resumeSP));
        asmStore(a, u8(8LL), R9, baseAt(ctxReg, offBase));
        asmLoad(a, u8(8LL), u8(8LL), false, memReg, baseAt(ctxReg, offMem));
        {
            auto range1090 = Array<VReg, 3>{{R10, R11, R12}};
            for (Int index1091 = 0, rangeEnd1092 = length(range1090); index1091 < rangeEnd1092; ++index1091) {
                Int k = index1091;
                VReg r = range1090[index1091];
                asmLoad(a, u8(8LL), u8(8LL), false, r, word((resumeR10 + (i32(8LL) * convert<i32>(k)))));
            }
        }
        asmLoad(a, u8(8LL), u8(8LL), false, R9, word(resumePC));
        asmMovRR(a, u8(8LL), RBP, RBX);
        asmAluRM(a, aluAdd, u8(8LL), RBP, word(resumeBP));
        asmLea(a, u8(8LL), R8, baseAt(RBX, i32(-8LL)));
        asmStore(a, u8(8LL), R9, baseAt(R8, i32(0LL)));
        asmAluRR(a, aluXor, u8(4LL), RAX, RAX);
        asmMovRR(a, u8(8LL), RSP, R8);
        asmRet(a);
        i32 cont = asmPos(a);
        littlePutUint32(subslice((a)->buf, contAt, length((a)->buf)), convert<u32>((cont - ((contAt + i32(4LL))))));
        asmPop(a, RBP);
        asmRet(a);
        asmSetSP(a, i32(0LL));
        asmPatch32(a, grow, asmPos(a));
        objCodeCallStub(oc, stubMorestack);
        asmPatch32(a, asmJmpRel(a), start);
        objCodeEndRegion(oc, start);
    }

    // slowPath grows the stack and starts the region again at entry.
    static void objCodeSlowPath(ObjCode* oc, i32 at, i32 entry) {
        Asm* a = (&((oc)->a));
        asmPatch32(a, at, asmPos(a));
        objCodeCallStub(oc, stubMorestack);
        asmPatch32(a, asmJmpRel(a), entry);
    }

    // stackCheck compares the stack pointer, less total-stackSmall for a large
    // frame computed in temp, with g.stackguard0 and calls the morestack stub
    // from out of line when it is not above. The caller emits the slow path at
    // the delta it had here, with slowPath.
    static i32 objCodeStackCheck(ObjCode* oc, i32 total, VReg temp) {
        Asm* a = (&((oc)->a));
        if ((total <= stackSmall)) {
            asmAluRM(a, aluCmp, u8(8LL), RSP, baseAt(R14, gStackguard0));
        } else {
            asmLea(a, u8(8LL), temp, rspAt((-(((total - stackSmall))))));
            asmAluRM(a, aluCmp, u8(8LL), temp, baseAt(R14, gStackguard0));
        }
        return asmJccRel(a, ccBE);
    }

    // stubs emits the morestack and bulk memory stubs and the Go calls of
    // memory.grow and table.grow.
    static void objCodeStubs(ObjCode* oc) {
        Asm* a = (&((oc)->a));
        i32 start = objCodeBeginRegion(oc, String("w2o.morestack"));
        (oc)->stubAt[stubMorestack] = start;
        Slice<VReg> saved = sliceOf<VReg>({RAX, RBX, RCX, RDX, RSI, RDI, R8, R9, R10, R11, R12, R13, R15});
        asmPush(a, RBP);
        asmMovRR(a, u8(8LL), RBP, RSP);
        {
            auto range1093 = saved;
            for (Int index1094 = 0, rangeEnd1095 = length(range1093); index1094 < rangeEnd1095; ++index1094) {
                VReg r = range1093[index1094];
                asmPush(a, r);
            }
        }
        i32 xmms = convert<i32>((Int(8LL) * length(goFloatRegs)));
        asmAluRI(a, aluSub, u8(8LL), RSP, convert<i64>(xmms));
        {
            auto range1096 = goFloatRegs;
            for (Int index1097 = 0, rangeEnd1098 = length(range1096); index1097 < rangeEnd1098; ++index1097) {
                Int i = index1097;
                VReg x = range1096[index1097];
                asmFstore(a, u8(8LL), x, rspAt(convert<i32>((Int(8LL) * i))));
            }
        }
        asmLoad(a, u8(8LL), u8(8LL), false, RAX, baseAt(R14, gStackHi));
        asmAluRR(a, aluSub, u8(8LL), RAX, RSP);
        asmAluRM(a, aluCmp, u8(8LL), RAX, baseAt(ctxReg, offStackLimit));
        i32 overflow = asmJccRel(a, ccA);
        asmCallMem(a, objCodeGotMem(oc, symMorestack));
        {
            auto range1099 = goFloatRegs;
            for (Int index1100 = 0, rangeEnd1101 = length(range1099); index1100 < rangeEnd1101; ++index1100) {
                Int i = index1100;
                VReg x = range1099[index1100];
                asmFload(a, u8(8LL), x, rspAt(convert<i32>((Int(8LL) * i))));
            }
        }
        asmAluRI(a, aluAdd, u8(8LL), RSP, convert<i64>(xmms));
        {
            Int i = (length(saved) - Int(1LL));
            for (; (i >= Int(0LL)); i--) {
                asmPop(a, saved[i]);
            }
        }
        asmPop(a, RBP);
        asmRet(a);
        asmSetSP(a, ((i32(8LL) + (i32(8LL) * convert<i32>(length(saved)))) + xmms));
        asmPatch32(a, overflow, asmPos(a));
        objCodeTrapCall(oc, trapStack);
        objCodeEndRegion(oc, start);
        start = objCodeBeginRegion(oc, String("w2o.memmove"));
        (oc)->stubAt[stubMemmove] = start;
        asmMovRR(a, u8(8LL), RAX, RDI);
        asmAluRR(a, aluSub, u8(8LL), RAX, RSI);
        asmAluRR(a, aluCmp, u8(8LL), RAX, RCX);
        i32 forward = asmJcc8(a, ccAE);
        asmTest(a, u8(8LL), RAX, RAX);
        i32 same = asmJcc8(a, ccE);
        asmAluRR(a, aluAdd, u8(8LL), RSI, RCX);
        asmAluRR(a, aluAdd, u8(8LL), RDI, RCX);
        i32 words = asmPos(a);
        asmAluRI(a, aluCmp, u8(8LL), RCX, i64(8LL));
        i32 bytes = asmJcc8(a, ccB);
        asmAluRI(a, aluSub, u8(8LL), RSI, i64(8LL));
        asmAluRI(a, aluSub, u8(8LL), RDI, i64(8LL));
        asmLoad(a, u8(8LL), u8(8LL), false, RAX, baseAt(RSI, i32(0LL)));
        asmStore(a, u8(8LL), RAX, baseAt(RDI, i32(0LL)));
        asmAluRI(a, aluSub, u8(8LL), RCX, i64(8LL));
        asmPatch8(a, asmJmp8(a), words);
        asmHere8(a, bytes);
        i32 tail = asmPos(a);
        asmTest(a, u8(8LL), RCX, RCX);
        i32 done = asmJcc8(a, ccE);
        asmAluRI(a, aluSub, u8(8LL), RSI, i64(1LL));
        asmAluRI(a, aluSub, u8(8LL), RDI, i64(1LL));
        asmLoad(a, u8(1LL), u8(4LL), false, RAX, baseAt(RSI, i32(0LL)));
        asmStore(a, u8(1LL), RAX, baseAt(RDI, i32(0LL)));
        asmAluRI(a, aluSub, u8(8LL), RCX, i64(1LL));
        asmPatch8(a, asmJmp8(a), tail);
        asmHere8(a, forward);
        asmRepMovsb(a);
        asmHere8(a, same);
        asmHere8(a, done);
        asmRet(a);
        objCodeEndRegion(oc, start);
        start = objCodeBeginRegion(oc, String("w2o.memset"));
        (oc)->stubAt[stubMemset] = start;
        asmRepStosb(a);
        asmRet(a);
        objCodeEndRegion(oc, start);
        (oc)->stubAt[stubMemoryGrow] = objCodeGoCall(oc, String("w2o.memory_grow_call"), sliceOf<ValType>({I32}), sliceOf<ValType>({I32}), symMemoryGrow, false);
        (oc)->stubAt[stubTableGrow] = objCodeGoCall(oc, String("w2o.table_grow_call"), sliceOf<ValType>({I32, I32, FuncRef}), sliceOf<ValType>({I32}), symTableGrow, false);
        (oc)->stubAt[stubInterrupt] = objCodeInterruptStub(oc);
    }

    // thunk emits a function Go calls with ABIInternal, func(ctx uintptr,
    // params...) (results...), that calls function index in the internal
    // convention; with index negative it calls body instead, which emits the
    // work itself.
    static i32 objCodeThunk(ObjCode* oc, String name, Slice<ValType> params, Slice<ValType> results, Int index, Fn<void()> body) {
        Asm* a = (&((oc)->a));
        GoFrame g = goAssign(params, results);
        auto temporary1102 = internalLocs(params, argGPRs, argXMMs, Int(0LL));
        Slice<VReg> pregs = temporary1102.v0;
        Slice<Int> pslots = temporary1102.v1;
        Int nslots = temporary1102.v2;
        auto temporary1103 = internalLocs(results, resultGPRs, resultXMMs, nslots);
        Slice<VReg> rregs = temporary1103.v0;
        Slice<Int> rslots = temporary1103.v1;
        Int nall = temporary1103.v2;
        i32 frame = convert<i32>((Int(8LL) * nall));
        i32 total = (i32(8LL) + frame);
        i32 start = objCodeBeginRegion(oc, name);
        asmMovRR(a, u8(8LL), ctxReg, RAX);
        i32 slow = objCodeStackCheck(oc, total, R12);
        asmPush(a, RBP);
        asmMovRR(a, u8(8LL), RBP, RSP);
        if ((frame > i32(0LL))) {
            asmAluRI(a, aluSub, u8(8LL), RSP, convert<i64>(frame));
        }
        asmLoad(a, u8(8LL), u8(8LL), false, memReg, baseAt(ctxReg, offMem));
        i32 goArgs = (total + i32(8LL));
        {
            auto range1104 = params;
            for (Int index1105 = 0, rangeEnd1106 = length(range1104); index1105 < rangeEnd1106; ++index1105) {
                Int i = index1105;
                ValType t = range1104[index1105];
                GoLoc src = (g).params[i];
                {
                    if (((pregs[i] != noReg) && ((src).reg == pregs[i]))) {
                        if ((t == I32)) {
                            asmMovRR(a, u8(4LL), (src).reg, (src).reg);
                        }
                    } else if (((pregs[i] == noReg) && ((src).reg != noReg))) {
                        if ((t == I32)) {
                            asmMovRR(a, u8(4LL), (src).reg, (src).reg);
                        }
                        asmStore(a, u8(8LL), (src).reg, rspAt(convert<i32>((Int(8LL) * pslots[i]))));
                    } else if ((pregs[i] == noReg)) {
                        asmLoad(a, convert<u8>(goSize(t)), u8(8LL), false, RDX, rspAt((goArgs + (src).off)));
                        asmStore(a, u8(8LL), RDX, rspAt(convert<i32>((Int(8LL) * pslots[i]))));
                    } else {
                        compilerError(String("internal error: parameter %d of %s moves between registers"));
                    }
                switchEnd1108:;
                }
            }
        }
        if ((index >= Int(0LL))) {
            objCodeCallFunc(oc, index);
        } else {
            body();
        }
        {
            auto range1109 = results;
            for (Int index1110 = 0, rangeEnd1111 = length(range1109); index1110 < rangeEnd1111; ++index1110) {
                Int i = index1110;
                ValType t = range1109[index1110];
                GoLoc dst = (g).results[i];
                {
                    if (((rregs[i] != noReg) && ((dst).reg == rregs[i]))) {
                    } else if (((rregs[i] == noReg) && ((dst).reg != noReg))) {
                        asmLoad(a, u8(8LL), u8(8LL), false, (dst).reg, rspAt(convert<i32>((Int(8LL) * rslots[i]))));
                    } else if ((rregs[i] == noReg)) {
                        asmLoad(a, u8(8LL), u8(8LL), false, RDX, rspAt(convert<i32>((Int(8LL) * rslots[i]))));
                        asmStore(a, convert<u8>(goSize(t)), RDX, rspAt((goArgs + (dst).off)));
                    } else {
                        compilerError(String("internal error: result %d of %s moves between registers"));
                    }
                switchEnd1113:;
                }
            }
        }
        if ((frame > i32(0LL))) {
            asmAluRI(a, aluAdd, u8(8LL), RSP, convert<i64>(frame));
        }
        asmPop(a, RBP);
        asmRet(a);
        asmSetSP(a, i32(0LL));
        objCodeSlowPath(oc, slow, start);
        objCodeEndRegion(oc, start);
        return start;
    }

    // trapCall calls Go's trap with the instance and code; it does not return.
    // Like any Go call it leaves the callee room to spill its arguments. Code
    // after it is reached by jumps, with the stack pointer of before.
    static void objCodeTrapCall(ObjCode* oc, i64 code) {
        Asm* a = (&((oc)->a));
        i32 sp = (a)->sp;
        asmAluRI(a, aluSub, u8(8LL), RSP, convert<i64>((goAssign(sliceOf<ValType>({I32}), {})).size));
        asmMovRR(a, u8(8LL), RAX, ctxReg);
        asmMovImm(a, u8(4LL), RBX, code);
        objCodeCallGo(oc, symTrap);
        asmUd2(a);
        asmSetSP(a, sp);
    }

    // apply rewrites every operand through the aliases.
    static void optimizerApply(Optimizer* o) {
        Function* f = (o)->f;
        if ((length((o)->alias) == Int(0LL))) {
            return;
        }
        const Span<const i32> aliases = o->alias.view();
        auto resolve = [&](i32 v) {
            while (u32(v) < aliases.count) {
                const i32 next = aliases[v];
                if (next < 0) {
                    break;
                }
                v = next;
            }
            return v;
        };
        // This pool contains only operand IDs. Old tails left by shortened
        // argument lists have no users, so resolving them is harmless.
        // Walk each page directly instead of fetching every Value header.
        usize remaining = f->pool.size();
        for (usize p = 0; remaining; ++p) {
            const usize take = remaining < PageVec<i32>::itemsPerPage ? remaining : PageVec<i32>::itemsPerPage;
            i32* operands = f->pool.pages.data()[p];
            for (usize i = 0; i < take; ++i) {
                operands[i] = resolve(operands[i]);
            }
            remaining -= take;
        }
        {
            auto& range1117 = (f)->blocks;
            for (Int index1118 = 0, rangeEnd1119 = length(range1117); index1118 < rangeEnd1119; ++index1118) {
                Int b = index1118;
                Term* t = (&(((f)->blocks[b]).term));
                if ((((t)->kind == TermIf) || ((t)->kind == TermSwitch))) {
                    (t)->value = resolve((t)->value);
                }
                {
                    auto range1120 = (t)->values;
                    for (Int index1121 = 0, rangeEnd1122 = length(range1120); index1121 < rangeEnd1122; ++index1121) {
                        Int i = index1121;
                        (t)->values[i] = resolve((t)->values[i]);
                    }
                }
                {
                    auto range1123 = (t)->edges;
                    for (Int index1124 = 0, rangeEnd1125 = length(range1123); index1124 < rangeEnd1125; ++index1124) {
                        Int i = index1124;
                        {
                            auto range1126 = ((t)->edges[i]).args;
                            for (Int index1127 = 0, rangeEnd1128 = length(range1126); index1127 < rangeEnd1128; ++index1127) {
                                Int j = index1127;
                                ((t)->edges[i]).args[j] = resolve(((t)->edges[i]).args[j]);
                            }
                        }
                    }
                }
            }
        }
    }

    // cleanup removes unreachable blocks, sends edges past blocks that only
    // jump, and merges blocks into their only predecessor.
    static bool optimizerCleanup(Optimizer* o) {
        Function* f = (o)->f;
        bool changed = false;
        optimizerCountUses(o);
        {
            auto& range1129 = (f)->blocks;
            for (Int index1130 = 0, rangeEnd1131 = length(range1129); index1130 < rangeEnd1131; ++index1130) {
                Int b = index1130;
                Block* blk = (&((f)->blocks[b]));
                if ((((((blk)->dead || (b == Int(0LL))) || (length((blk)->instrs) != Int(0LL))) || (((blk)->term).kind != TermJump)) || ((((blk)->term).edges[Int(0LL)]).block == convert<i32>(b)))) {
                    continue;
                }
                if ((!(optimizerLocalParams(o, convert<i32>(b))))) {
                    continue;
                }
                Edge next = ((blk)->term).edges[Int(0LL)];
                {
                    auto range1132 = (blk)->preds;
                    for (Int index1133 = 0, rangeEnd1134 = length(range1132); index1133 < rangeEnd1134; ++index1133) {
                        i32 p = range1132[index1133];
                        Term* pt = (&(((f)->blocks[p]).term));
                        {
                            auto range1135 = (pt)->edges;
                            for (Int index1136 = 0, rangeEnd1137 = length(range1135); index1136 < rangeEnd1137; ++index1136) {
                                Int i = index1136;
                                Edge* e = (&((pt)->edges[i]));
                                if (((e)->block == convert<i32>(b))) {
                                    Slice<i32> args = optimizerSubstitute(o, convert<i32>(b), (next).args, (e)->args);
                                    (*(e)) = Edge{(next).block, args};
                                    changed = true;
                                }
                            }
                        }
                    }
                }
            }
        }
        if (changed) {
            functionComputePreds(f);
        }
        changed = (optimizerUnreachable(o) || changed);
        bool merged = false;
        {
            auto& range1138 = (f)->blocks;
            for (Int index1139 = 0, rangeEnd1140 = length(range1138); index1139 < rangeEnd1140; ++index1139) {
                Int b = index1139;
                Block* blk = (&((f)->blocks[b]));
                if ((((blk)->dead || (b == Int(0LL))) || (length((blk)->preds) != Int(1LL)))) {
                    continue;
                }
                i32 p = (blk)->preds[Int(0LL)];
                Block* pb = (&((f)->blocks[p]));
                if (((p == convert<i32>(b)) || (((pb)->term).kind != TermJump))) {
                    continue;
                }
                Edge e = ((pb)->term).edges[Int(0LL)];
                {
                    auto range1141 = (blk)->params;
                    for (Int index1142 = 0, rangeEnd1143 = length(range1141); index1142 < rangeEnd1143; ++index1142) {
                        Int i = index1142;
                        i32 param = range1141[index1142];
                        optimizerReplace(o, param, optimizerResolve(o, (e).args[i]));
                    }
                }
                {
                    auto range1144 = (blk)->instrs.view();
                    for (Int index1145 = 0, rangeEnd1146 = length(range1144); index1145 < rangeEnd1146; ++index1145) {
                        i32 v = range1144[index1145];
                        ((f)->values[v]).block = p;
                    }
                }
                (pb)->instrs.append((blk)->instrs.view());
                (pb)->term = (blk)->term;
                (*(blk)) = Block{{}, {}, {}, {}, true};
                {
                    auto range1147 = ((pb)->term).edges;
                    for (Int index1148 = 0, rangeEnd1149 = length(range1147); index1148 < rangeEnd1149; ++index1148) {
                        Edge s = range1147[index1148];
                        Block* sb = (&((f)->blocks[(s).block]));
                        {
                            auto range1150 = (sb)->preds;
                            for (Int index1151 = 0, rangeEnd1152 = length(range1150); index1151 < rangeEnd1152; ++index1151) {
                                Int i = index1151;
                                i32 q = range1150[index1151];
                                if ((q == convert<i32>(b))) {
                                    (sb)->preds[i] = p;
                                }
                            }
                        }
                    }
                }
                merged = true;
            }
        }
        if (merged) {
            optimizerApply(o);
            functionComputePreds(f);
        }
        return (changed || merged);
    }

    static void optimizerCommonExpressions(Optimizer* o) {
        Function* f = o->f;
        optimizerApply(o);
        CfgInfo* ci = functionAnalyze(f);
        auto children = cfgInfoChildren(ci);

        struct Entry {
            u64 hash;
            CseKey key;
            i32 value;
            i32 next;
        };

        Vec<i32> heads;
        Vec<Entry> entries;
        Vec<CseVisit> stack;
        stack.pushBack({ci->order[0], false, 0});
        while (stack.size()) {
            const CseVisit visit = stack.popBack();
            if (visit.leave) {
                // Insertions are nested by dominance. Rehashing preserves
                // insertion order, so undoing the suffix restores each head.
                while (entries.size() > visit.base) {
                    const auto& entry = entries[entries.size() - 1];
                    heads[usize(entry.hash) & (heads.size() - 1)] = entry.next;
                    entries.resize(entries.size() - 1);
                }
                continue;
            }
            const usize base = entries.size();
            auto instructions = f->blocks[visit.block].instrs.view();
            for (Int i = 0; i < length(instructions); ++i) {
                const i32 v = instructions[i];
                const Value& val = f->values[v];
                if (readsState(val.op) || val.nargs > 3 || (val.typ != I32 && val.typ != I64 && val.typ != F32 && val.typ != F64)) {
                    continue;
                }
                CseKey key{val.op, val.typ, val.nargs, val.aux, val.aux2, {}};
                auto args = functionArgs(f, v);
                for (Int j = 0; j < length(args); ++j) {
                    key.args[j] = optimizerResolve(o, args[j]);
                }
                if (!isFloat(val.typ)) {
                    switch (val.op) {
                        case OpAdd:
                        case OpMul:
                        case OpAnd:
                        case OpOr:
                        case OpXor:
                        case OpEq:
                        case OpNe: {
                            if (key.args[0] > key.args[1]) {
                                const i32 a = key.args[0];
                                key.args[0] = key.args[1];
                                key.args[1] = a;
                            }
                            break;
                        }
                    }
                }
                const u64 hash = hashValue(key);
                i32 prior = -1;
                if (heads.size()) {
                    i32 at = heads[usize(hash) & (heads.size() - 1)];
                    while (at >= 0) {
                        const auto& entry = entries[usize(at)];
                        if (entry.hash == hash && entry.key == key) {
                            prior = entry.value;
                            break;
                        }
                        at = entry.next;
                    }
                }
                if (prior >= 0) {
                    optimizerReplace(o, v, prior);
                    continue;
                }
                if (entries.size() == heads.size()) {
                    heads.resize(heads.size() ? heads.size() * 2 : 16);
                    heads.fill(-1);
                    for (usize j = 0; j < entries.size(); ++j) {
                        auto& entry = entries[j];
                        const usize bucket = usize(entry.hash) & (heads.size() - 1);
                        entry.next = heads[bucket];
                        heads[bucket] = i32(j);
                    }
                }
                const usize bucket = usize(hash) & (heads.size() - 1);
                const i32 at = i32(entries.size());
                entries.pushBack({hash, key, v, heads[bucket]});
                heads[bucket] = at;
            }
            stack.pushBack({visit.block, true, base});
            for (i32 child = children.first[usize(visit.block)]; child >= 0; child = children.next[usize(child)]) {
                stack.pushBack({child, false, 0});
            }
        }
        optimizerApply(o);
    }

    static i64 optimizerConstOf(Optimizer* o, i32 v) {
        return (((o)->f)->values[v]).aux;
    }

    static i32 optimizerConstant(Optimizer* o, ValType t, i64 bits) {
        if ((t == I32)) {
            bits = convert<i64>(convert<i32>(bits));
        }
        ConstKey key = ConstKey{t, bits};
        {
            auto temporary1166 = lookupMap((o)->consts, key);
            i32 v = temporary1166.v0;
            bool ok = temporary1166.v1;
            if (ok) {
                return v;
            }
        }
        i32 v = functionNewValue((o)->f, OpConst, t, i32(-1LL), bits, Span<const i32>({}));
        setMap((o)->consts, key, v);
        return v;
    }

    static void optimizerCountUses(Optimizer* o) {
        Function* f = (o)->f;
        o->uses.resize(usize(length(f->values)));
        o->uses.fill(0);
        {
            auto& range1167 = (f)->blocks;
            for (Int index1168 = 0, rangeEnd1169 = length(range1167); index1168 < rangeEnd1169; ++index1168) {
                Int b = index1168;
                Block* blk = (&((f)->blocks[b]));
                if ((blk)->dead) {
                    continue;
                }
                {
                    auto range1170 = (blk)->instrs.view();
                    for (Int index1171 = 0, rangeEnd1172 = length(range1170); index1171 < rangeEnd1172; ++index1171) {
                        i32 v = range1170[index1171];
                        {
                            auto range1173 = functionArgs(f, v);
                            for (Int index1174 = 0, rangeEnd1175 = length(range1173); index1174 < rangeEnd1175; ++index1174) {
                                i32 a = range1173[index1174];
                                (o)->uses[a]++;
                            }
                        }
                    }
                }
                Term* t = (&((blk)->term));
                if ((((t)->kind == TermIf) || ((t)->kind == TermSwitch))) {
                    (o)->uses[(t)->value]++;
                }
                {
                    auto range1176 = (t)->values;
                    for (Int index1177 = 0, rangeEnd1178 = length(range1176); index1177 < rangeEnd1178; ++index1177) {
                        i32 v = range1176[index1177];
                        (o)->uses[v]++;
                    }
                }
                {
                    auto range1179 = (t)->edges;
                    for (Int index1180 = 0, rangeEnd1181 = length(range1179); index1180 < rangeEnd1181; ++index1180) {
                        Edge e = range1179[index1180];
                        {
                            auto range1182 = (e).args;
                            for (Int index1183 = 0, rangeEnd1184 = length(range1182); index1183 < rangeEnd1184; ++index1183) {
                                i32 a = range1182[index1183];
                                (o)->uses[a]++;
                            }
                        }
                    }
                }
            }
        }
    }

    // deadCode removes pure values nothing uses and parameters only their own
    // edges use.
    static void optimizerDeadCode(Optimizer* o) {
        Function* f = (o)->f;
        {
            for (; true;) {
                optimizerCountUses(o);
                bool removed = false;
                {
                    auto& range1185 = (f)->blocks;
                    for (Int index1186 = 0, rangeEnd1187 = length(range1185); index1186 < rangeEnd1187; ++index1186) {
                        Int b = index1186;
                        Block* blk = (&((f)->blocks[b]));
                        if ((blk)->dead) {
                            continue;
                        }
                        const usize count = blk->instrs.size();
                        usize first = count;
                        {
                            Int i = (length((blk)->instrs) - Int(1LL));
                            for (; (i >= Int(0LL)); i--) {
                                i32 v = (blk)->instrs[i];
                                if ((((o)->uses[v] == i32(0LL)) && (!(hasEffect(((f)->values[v]).op))))) {
                                    {
                                        auto range1188 = functionArgs(f, v);
                                        for (Int index1189 = 0, rangeEnd1190 = length(range1188); index1189 < rangeEnd1190; ++index1189) {
                                            i32 a = range1188[index1189];
                                            (o)->uses[a]--;
                                        }
                                    }
                                    removed = true;
                                    continue;
                                }
                                blk->instrs[--first] = v;
                            }
                        }
                        if (first && first != count) {
                            copySlice(blk->instrs.view(), subslice(blk->instrs.view(), Int(first), Int(count)));
                        }
                        blk->instrs.resize(count - first);
                    }
                }
                {
                    auto& range1197 = (f)->blocks;
                    for (Int index1198 = 0, rangeEnd1199 = length(range1197); index1198 < rangeEnd1199; ++index1198) {
                        Int b = index1198;
                        Block* blk = (&((f)->blocks[b]));
                        if ((((blk)->dead || (b == Int(0LL))) || (length((blk)->params) == Int(0LL)))) {
                            continue;
                        }
                        Slice<bool> keep = makeSlice<bool>(length((blk)->params));
                        bool drop = false;
                        Vec<Edge*> in = optimizerIncoming(o, convert<i32>(b));
                        {
                            auto range1200 = (blk)->params;
                            for (Int index1201 = 0, rangeEnd1202 = length(range1200); index1201 < rangeEnd1202; ++index1201) {
                                Int k = index1201;
                                i32 p = range1200[index1201];
                                keep[k] = ((o)->uses[p] > selfUses(in, k, p));
                                if ((!(keep[k]))) {
                                    drop = true;
                                }
                            }
                        }
                        if (drop) {
                            optimizerDropParams(o, convert<i32>(b), keep);
                            removed = true;
                        }
                    }
                }
                if ((!(removed))) {
                    return;
                }
            }
        }
    }

    // dropParams removes the parameters of block b not kept, and their
    // arguments on every incoming edge.
    static void optimizerDropParams(Optimizer* o, i32 b, Slice<bool> keep) {
        Function* f = (o)->f;
        Block* blk = (&((f)->blocks[b]));
        Slice<i32> params = {};
        {
            auto range1203 = (blk)->params;
            for (Int index1204 = 0, rangeEnd1205 = length(range1203); index1204 < rangeEnd1205; ++index1204) {
                Int i = index1204;
                i32 p = range1203[index1204];
                if (keep[i]) {
                    ((f)->values[p]).aux = convert<i64>(length(params));
                    params = appendValues(params, p);
                }
            }
        }
        Map<i32, bool> seen = mapOf<i32, bool>({});
        {
            auto range1206 = (blk)->preds;
            for (Int index1207 = 0, rangeEnd1208 = length(range1206); index1207 < rangeEnd1208; ++index1207) {
                i32 p = range1206[index1207];
                if (seen[p]) {
                    continue;
                }
                setMap(seen, p, true);
                Term* pt = (&(((f)->blocks[p]).term));
                {
                    auto range1209 = (pt)->edges;
                    for (Int index1210 = 0, rangeEnd1211 = length(range1209); index1210 < rangeEnd1211; ++index1210) {
                        Int i = index1210;
                        Edge* e = (&((pt)->edges[i]));
                        if (((e)->block != b)) {
                            continue;
                        }
                        Slice<i32> args = {};
                        {
                            auto range1212 = (e)->args;
                            for (Int index1213 = 0, rangeEnd1214 = length(range1212); index1213 < rangeEnd1214; ++index1213) {
                                Int j = index1213;
                                i32 a = range1212[index1213];
                                if (keep[j]) {
                                    args = appendValues(args, a);
                                }
                            }
                        }
                        (e)->args = args;
                    }
                }
            }
        }
        (blk)->params = params;
    }

    static bool optimizerIdentity(Optimizer* o, i32 v, Op op, i32 a, i32 b, ValType t) {
        Function* f = (o)->f;
        if (((optimizerIsConst(o, a) && (!(optimizerIsConst(o, b)))) && ((((((((op == OpAdd) || (op == OpMul)) || (op == OpAnd)) || (op == OpOr)) || (op == OpXor)) || (op == OpEq)) || (op == OpNe))))) {
            auto temporary1215 = b;
            auto temporary1216 = a;
            a = temporary1215;
            b = temporary1216;
            optimizerSetArgs(o, v, Span<const i32>({a, b}));
        }
        if (optimizerIsConst(o, b)) {
            u64 c = (convert<u64>(optimizerConstOf(o, b)) & mask(t));
            {
                if (((c == u64(0LL)) && ((((((((((op == OpAdd) || (op == OpSub)) || (op == OpOr)) || (op == OpXor)) || (op == OpShl)) || (op == OpShrS)) || (op == OpShrU)) || (op == OpRotl)) || (op == OpRotr))))) {
                    optimizerReplace(o, v, a);
                    return true;
                } else if (((c == u64(0LL)) && (((op == OpMul) || (op == OpAnd))))) {
                    optimizerReplace(o, v, b);
                    return true;
                } else if (((c == u64(1LL)) && ((((op == OpMul) || (op == OpDivS)) || (op == OpDivU))))) {
                    optimizerReplace(o, v, a);
                    return true;
                } else if (((c == mask(t)) && (op == OpAnd))) {
                    optimizerReplace(o, v, a);
                    return true;
                } else if ((((c == u64(0LL)) && (op == OpEq)) && (t == I32))) {
                    ((f)->values[v]).op = OpEqz;
                    optimizerSetArgs(o, v, Span<const i32>({a}));
                    return true;
                } else if (((c == u64(0LL)) && (op == OpGeU))) {
                    optimizerReplace(o, v, optimizerConstant(o, I32, i64(1LL)));
                    return true;
                } else if (((c == u64(0LL)) && (op == OpLtU))) {
                    optimizerReplace(o, v, optimizerConstant(o, I32, i64(0LL)));
                    return true;
                }
            switchEnd1218:;
            }
            if (((op == OpMul) && ((c & ((c - u64(1LL)))) == u64(0LL)))) {
                ((f)->values[v]).op = OpShl;
                optimizerSetArgs(o, v, Span<const i32>({a, optimizerConstant(o, t, convert<i64>(bitsTrailingZeros64(c)))}));
                return true;
            }
            if ((op == OpAnd)) {
                Value* src = (&((f)->values[a]));
                if ((((src)->op == OpExtendU) && (c == u64(4294967295LL)))) {
                    optimizerReplace(o, v, a);
                    return true;
                }
            }
        }
        if ((a == b)) {
            {
                auto switch1219 = op;
                switch (switch1219) {
                    case OpSub:
                    case OpXor: {
                        optimizerReplace(o, v, optimizerConstant(o, t, i64(0LL)));
                        return true;
                        break;
                    }
                    case OpAnd:
                    case OpOr: {
                        optimizerReplace(o, v, a);
                        return true;
                        break;
                    }
                    case OpEq:
                    case OpLeS:
                    case OpLeU:
                    case OpGeS:
                    case OpGeU: {
                        optimizerReplace(o, v, optimizerConstant(o, I32, i64(1LL)));
                        return true;
                        break;
                    }
                    case OpNe:
                    case OpLtS:
                    case OpLtU:
                    case OpGtS:
                    case OpGtU: {
                        optimizerReplace(o, v, optimizerConstant(o, I32, i64(0LL)));
                        return true;
                        break;
                    }
                }
            }
        }
        return false;
    }

    // incoming lists the edges into block b, each once.
    static Vec<Edge*> optimizerIncoming(Optimizer* o, i32 b) {
        Function* f = o->f;
        Vec<Edge*> out;
        Slice<i32> preds = f->blocks[b].preds;
        const bool deduplicate = length(preds) > 1;
        u32 epoch = 0;
        if (deduplicate) {
            const usize before = o->incomingSeen.size();
            const usize count = usize(length(f->blocks));
            if (before < count) {
                o->incomingSeen.resize(count);
                for (usize i = before; i < count; ++i) {
                    o->incomingSeen[i] = 0;
                }
            }
            epoch = ++o->incomingEpoch;
            if (!epoch) {
                o->incomingSeen.visit([](u32& mark) {
                    mark = 0;
                });
                epoch = ++o->incomingEpoch;
            }
        }
        for (Int i = 0; i < length(preds); ++i) {
            const i32 q = preds[i];
            if (deduplicate) {
                if (o->incomingSeen[usize(q)] == epoch) {
                    continue;
                }
                o->incomingSeen[usize(q)] = epoch;
            }
            auto edges = f->blocks[q].term.edges;
            for (Int e = 0; e < length(edges); ++e) {
                if (edges[e].block == b) {
                    out.pushBack(&edges[e]);
                }
            }
        }
        return out;
    }

    static bool optimizerIsConst(Optimizer* o, i32 v) {
        return ((((o)->f)->values[v]).op == OpConst);
    }

    // isZext reports an i64 value known to have its upper half zero.
    static bool optimizerIsZext(Optimizer* o, i32 v) {
        Value* val = (&(((o)->f)->values[v]));
        return ((((val)->op == OpExtendU) || ((((val)->op == OpConst) && (convert<u64>((val)->aux) <= u64(4294967295LL))))) || (((((val)->op == OpLoad) && (memKindWidth(convert<MemKind>((val)->aux2)) < Int(8LL))) && (!(memKindSigned(convert<MemKind>((val)->aux2)))))));
    }

    // localParams reports whether the parameters of block b are used only by
    // its own terminator: edges can bypass b without breaking the dominance of
    // their definitions.
    static bool optimizerLocalParams(Optimizer* o, i32 b) {
        Function* f = (o)->f;
        Block* blk = (&((f)->blocks[b]));
        if ((length((blk)->params) == Int(0LL))) {
            return true;
        }
        Map<i32, i32> own = mapOf<i32, i32>({});
        Term* t = (&((blk)->term));
        if ((((t)->kind == TermIf) || ((t)->kind == TermSwitch))) {
            setMap(own, optimizerResolve(o, (t)->value), own[optimizerResolve(o, (t)->value)] + 1);
        }
        {
            auto range1226 = (t)->values;
            for (Int index1227 = 0, rangeEnd1228 = length(range1226); index1227 < rangeEnd1228; ++index1227) {
                i32 v = range1226[index1227];
                setMap(own, optimizerResolve(o, v), own[optimizerResolve(o, v)] + 1);
            }
        }
        {
            auto range1229 = (t)->edges;
            for (Int index1230 = 0, rangeEnd1231 = length(range1229); index1230 < rangeEnd1231; ++index1230) {
                Edge e = range1229[index1230];
                {
                    auto range1232 = (e).args;
                    for (Int index1233 = 0, rangeEnd1234 = length(range1232); index1233 < rangeEnd1234; ++index1233) {
                        i32 a = range1232[index1233];
                        setMap(own, optimizerResolve(o, a), own[optimizerResolve(o, a)] + 1);
                    }
                }
            }
        }
        {
            auto range1235 = (blk)->params;
            for (Int index1236 = 0, rangeEnd1237 = length(range1235); index1236 < rangeEnd1237; ++index1236) {
                i32 p = range1235[index1236];
                if (((o)->uses[p] != own[p])) {
                    return false;
                }
            }
        }
        return true;
    }

    // low32 returns an i32 value equal to the low half of i64 value v, without
    // new instructions.
    static Tuple2<i32, bool> optimizerLow32(Optimizer* o, i32 v) {
        Function* f = (o)->f;
        Value* val = (&((f)->values[v]));
        {
            auto switch1238 = (val)->op;
            switch (switch1238) {
                case OpConst: {
                    return {optimizerConstant(o, I32, convert<i64>(convert<u32>((val)->aux))), true};
                    break;
                }
                case OpExtendU:
                case OpExtendS: {
                    return {functionArgs(f, v)[Int(0LL)], true};
                    break;
                }
            }
        }
        return {i32(0LL), false};
    }

    // Every u32 has an exact f64 representation. Comparisons after a merge of
    // such conversions and integral constants can stay entirely in integers;
    // the original float values remain available to any other users.
    static bool optimizerNarrowFloatComparisons(Optimizer* o) {
        Function* f = (o)->f;
        FloatIntegerNarrow* n = activePool->make<FloatIntegerNarrow>(FloatIntegerNarrow{o, makeSlice<u8>(length((f)->values)), mapOf<i32, i32>({})});
        bool changed = false;
        {
            auto& range1239 = (f)->values;
            for (Int index1240 = 0, rangeEnd1241 = length(range1239); index1240 < rangeEnd1241; ++index1240) {
                Int v = index1240;
                Value val = (f)->values[v];
                if ((((((val).op < OpFEq) || ((val).op > OpFGe)) || ((val).block < i32(0LL))) || ((f)->blocks[(val).block]).dead)) {
                    continue;
                }
                Span<i32> args = functionArgs(f, convert<i32>(v));
                if (((!(floatIntegerNarrowCan(n, args[Int(0LL)], Int(0LL)))) || (!(floatIntegerNarrowCan(n, args[Int(1LL)], Int(0LL)))))) {
                    continue;
                }
                auto temporary1242 = floatIntegerNarrowInteger(n, args[Int(0LL)]);
                auto temporary1243 = floatIntegerNarrowInteger(n, args[Int(1LL)]);
                i32 a = temporary1242;
                i32 b = temporary1243;
                Array<Op, 6> ops = Array<Op, 6>{{OpEq, OpNe, OpLtU, OpGtU, OpLeU, OpGeU}};
                ((f)->values[v]).op = ops[Op((val).op - OpFEq)];
                optimizerSetArgs(o, convert<i32>(v), Span<const i32>({a, b}));
                changed = true;
            }
        }
        return changed;
    }

    // paramIndex returns the position of value v among block b's parameters.
    static Int optimizerParamIndex(Optimizer* o, i32 b, i32 v) {
        {
            auto range1244 = (((o)->f)->blocks[b]).params;
            for (Int index1245 = 0, rangeEnd1246 = length(range1244); index1245 < rangeEnd1246; ++index1245) {
                Int i = index1245;
                i32 p = range1244[index1245];
                if ((p == v)) {
                    return i;
                }
            }
        }
        return Int(-1LL);
    }

    // repair rewrites the uses of h's parameters outside h after thread: the
    // value of a parameter in a block is h's on the paths through h and the
    // argument of the edge on the paths through a bypass, and a block where
    // both meet gets a parameter for it.
    static void optimizerRepair(Optimizer* o, i32 h, Map<i32, Bypass> bypasses) {
        Function* f = (o)->f;
        Slice<i32> params = appendSlice(Slice<i32>{}, ((f)->blocks[h]).params);
        u32 epoch = ++o->repairEpoch;
        if (!epoch) {
            o->repairIndex.visit([](ParamIndex& entry) {
                entry.epoch = 0;
            });
            epoch = ++o->repairEpoch;
        }
        if (length(params)) {
            const usize before = o->repairIndex.size();
            const usize count = usize(length(f->values));
            if (before < count) {
                o->repairIndex.resize(count);
                for (usize i = before; i < count; ++i) {
                    o->repairIndex[i] = {};
                }
            }
            for (Int j = 0; j < length(params); ++j) {
                o->repairIndex[usize(params[j])] = {i32(j), epoch};
            }
        }
        auto paramIndex = [&](i32 v) -> i32 {
            if (Uint(v) >= o->repairIndex.size()) {
                return -1;
            }
            const ParamIndex entry = o->repairIndex[usize(v)];
            return entry.epoch == epoch ? entry.index : -1;
        };
        Map<Array<i32, 2>, i32> memo = mapOf<Array<i32, 2>, i32>({});
        Map<i32, Slice<i32>> preds = mapOf<i32, Slice<i32>>({});
        auto seen = [&](auto& recur, i32 b, i32 v) -> i32 {
            v = optimizerResolve(o, v);
            {
                i32 j = paramIndex(v);
                bool ok = j >= 0;
                if (ok) {
                    return recur(recur, b, j);
                }
            }
            return v;
        };
        auto value = [&](auto& recur, i32 b, Int j) -> i32 {
            if ((b == h)) {
                return params[j];
            }
            Array<i32, 2> key = Array<i32, 2>{{b, convert<i32>(j)}};
            {
                auto found = lookupMap(memo, key);
                if (found.v1) {
                    return found.v0;
                }
            }
            {
                auto found = lookupMap(bypasses, b);
                if (found.v1) {
                    const Bypass d = found.v0;
                    const i32 v = seen(recur, d.pred, d.args[j]);
                    setMap(memo, key, v);
                    return v;
                }
            }
            auto temporary1253 = lookupMap(preds, b);
            Slice<i32> from = temporary1253.v0;
            bool ok = temporary1253.v1;
            if ((!(ok))) {
                from = uniquePredecessors(((f)->blocks[b]).preds);
                setMap(preds, b, from);
            }
            if ((length(from) == Int(0LL))) {
                i32 v = functionNewValue(f, OpUndef, ((f)->values[params[j]]).typ, i32(-1LL), i64(0LL), Span<const i32>({}));
                setMap(memo, key, v);
                return v;
            }
            if ((length(from) == Int(1LL))) {
                i32 v = recur(recur, from[Int(0LL)], j);
                setMap(memo, key, v);
                return v;
            }
            Int at = length(((f)->blocks[b]).params);
            i32 x = functionNewValue(f, OpParam, ((f)->values[params[j]]).typ, b, convert<i64>(at), Span<const i32>({}));
            ((f)->blocks[b]).params = appendValues(((f)->blocks[b]).params, x);
            setMap(memo, key, x);
            {
                auto range1254 = from;
                for (Int index1255 = 0, rangeEnd1256 = length(range1254); index1255 < rangeEnd1256; ++index1255) {
                    i32 q = range1254[index1255];
                    {
                        auto range1257 = (((f)->blocks[q]).term).edges;
                        for (Int index1258 = 0, rangeEnd1259 = length(range1257); index1258 < rangeEnd1259; ++index1258) {
                            Int i = index1258;
                            {
                                Edge* e = (&((((f)->blocks[q]).term).edges[i]));
                                if (((e)->block == b)) {
                                    (e)->args = appendValues((e)->args, x);
                                }
                            }
                        }
                    }
                }
            }
            {
                auto range1260 = from;
                for (Int index1261 = 0, rangeEnd1262 = length(range1260); index1261 < rangeEnd1262; ++index1261) {
                    i32 q = range1260[index1261];
                    {
                        auto range1263 = (((f)->blocks[q]).term).edges;
                        for (Int index1264 = 0, rangeEnd1265 = length(range1263); index1264 < rangeEnd1265; ++index1264) {
                            Int i = index1264;
                            if ((((((f)->blocks[q]).term).edges[i]).block == b)) {
                                i32 arg = recur(recur, q, j);
                                ((((f)->blocks[q]).term).edges[i]).args[at] = arg;
                            }
                        }
                    }
                }
            }
            return x;
        };
        {
            i32 b = i32(0LL);
            for (; (b < convert<i32>(length((f)->blocks))); b++) {
                if (((b == h) || ((f)->blocks[b]).dead)) {
                    continue;
                }
                auto temporary1266 = lookupMap(bypasses, b);
                bool isBypass = temporary1266.v1;
                auto at = [&](i32 v) mutable -> i32 {
                    if (isBypass) {
                        return seen(value, (bypasses[b]).pred, v);
                    }
                    return seen(value, b, v);
                };
                {
                    auto range1267 = ((f)->blocks[b]).instrs.view();
                    for (Int index1268 = 0, rangeEnd1269 = length(range1267); index1268 < rangeEnd1269; ++index1268) {
                        i32 v = range1267[index1268];
                        Span<i32> args = functionArgs(f, v);
                        {
                            auto range1270 = args;
                            for (Int index1271 = 0, rangeEnd1272 = length(range1270); index1271 < rangeEnd1272; ++index1271) {
                                Int i = index1271;
                                i32 a = range1270[index1271];
                                {
                                    bool ok = paramIndex(optimizerResolve(o, a)) >= 0;
                                    if (ok) {
                                        args[i] = at(a);
                                    }
                                }
                            }
                        }
                    }
                }
                Term* t = (&(((f)->blocks[b]).term));
                if ((((t)->kind == TermIf) || ((t)->kind == TermSwitch))) {
                    (t)->value = at((t)->value);
                }
                {
                    auto range1274 = (t)->values;
                    for (Int index1275 = 0, rangeEnd1276 = length(range1274); index1275 < rangeEnd1276; ++index1275) {
                        Int i = index1275;
                        (t)->values[i] = at((t)->values[i]);
                    }
                }
                {
                    auto range1277 = (t)->edges;
                    for (Int index1278 = 0, rangeEnd1279 = length(range1277); index1278 < rangeEnd1279; ++index1278) {
                        Int i = index1278;
                        {
                            auto range1280 = ((t)->edges[i]).args;
                            for (Int index1281 = 0, rangeEnd1282 = length(range1280); index1281 < rangeEnd1282; ++index1281) {
                                Int k = index1281;
                                i32 a = range1280[index1281];
                                ((t)->edges[i]).args[k] = at(a);
                            }
                        }
                    }
                }
            }
        }
    }

    static void optimizerReplace(Optimizer* o, i32 v, i32 w) {
        {
            for (; (length((o)->alias) < length(((o)->f)->values));) {
                o->alias.pushBack(-1);
            }
        }
        if ((v != w)) {
            (o)->alias[v] = w;
        }
    }

    static i32 optimizerResolve(Optimizer* o, i32 v) {
        {
            for (; ((convert<i64>(v) < length((o)->alias)) && ((o)->alias[v] >= i32(0LL)));) {
                v = (o)->alias[v];
            }
        }
        return v;
    }

    // Replace short, non-trapping conditional computations by selects. Bit
    // tests often have unpredictable outcomes; float equality is also used by
    // scalar rounding code to apply a small correction on ties.
    static void optimizerSelectTriangles(Optimizer* o) {
        Function* f = (o)->f;
        {
            Int round = Int(0LL);
            for (; (round < Int(8LL)); round++) {
                bool changed = false;
                {
                    auto range1283 = functionRpo(f);
                    for (Int index1284 = 0, rangeEnd1285 = length(range1283); index1284 < rangeEnd1285; ++index1284) {
                        i32 b = range1283[index1284];
                        Block* blk = (&((f)->blocks[b]));
                        Term t = (blk)->term;
                        if (((blk)->dead || ((t).kind != TermIf))) {
                            continue;
                        }
                        Int budget = Int(0LL);
                        {
                            auto switch1286 = ((f)->values[(t).value]).op;
                            switch (switch1286) {
                                case OpAnd: {
                                    budget = Int(8LL);
                                    break;
                                }
                                case OpFEq:
                                case OpFNe: {
                                    budget = Int(4LL);
                                    break;
                                }
                            }
                        }
                        if ((budget == Int(0LL))) {
                            continue;
                        }
                        {
                            Int side = Int(0LL);
                            for (; (side < Int(2LL)); side++) {
                                auto temporary1287 = (t).edges[side];
                                auto temporary1288 = (t).edges[(Int(1LL) - side)];
                                Edge arm = temporary1287;
                                Edge merge = temporary1288;
                                Block* body = (&((f)->blocks[(arm).block]));
                                if (((((((((arm).block == b) || (body)->dead) || (length((body)->params) != Int(0LL))) || (length((body)->preds) != Int(1LL))) || ((body)->preds[Int(0LL)] != b)) || (((body)->term).kind != TermJump)) || ((((body)->term).edges[Int(0LL)]).block != (merge).block))) {
                                    continue;
                                }
                                if ((length((body)->instrs) > budget)) {
                                    continue;
                                }
                                bool pure = true;
                                {
                                    auto range1289 = (body)->instrs.view();
                                    for (Int index1290 = 0, rangeEnd1291 = length(range1289); index1290 < rangeEnd1291; ++index1290) {
                                        i32 v = range1289[index1290];
                                        Op op = ((f)->values[v]).op;
                                        pure = (pure && (!(readsState(op))));
                                        {
                                            auto switch1292 = op;
                                            switch (switch1292) {
                                                case OpFDiv:
                                                case OpFSqrt:
                                                case OpFMin:
                                                case OpFMax:
                                                case OpTruncSatS:
                                                case OpTruncSatU:
                                                case OpConvertU: {
                                                    pure = false;
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                }
                                {
                                    auto range1293 = ((f)->blocks[(merge).block]).params;
                                    for (Int index1294 = 0, rangeEnd1295 = length(range1293); index1294 < rangeEnd1295; ++index1294) {
                                        i32 p = range1293[index1294];
                                        ValType typ = ((f)->values[p]).typ;
                                        pure = (pure && (((((typ == I32) || (typ == I64)) || (typ == F32)) || (typ == F64))));
                                    }
                                }
                                if ((!(pure))) {
                                    continue;
                                }
                                auto temporary1296 = (((body)->term).edges[Int(0LL)]).args;
                                auto temporary1297 = (merge).args;
                                Slice<i32> yes = temporary1296;
                                Slice<i32> no = temporary1297;
                                if ((side == Int(1LL))) {
                                    auto temporary1298 = no;
                                    auto temporary1299 = yes;
                                    yes = temporary1298;
                                    no = temporary1299;
                                }
                                {
                                    auto range1300 = (body)->instrs.view();
                                    for (Int index1301 = 0, rangeEnd1302 = length(range1300); index1301 < rangeEnd1302; ++index1301) {
                                        i32 v = range1300[index1301];
                                        ((f)->values[v]).block = b;
                                        (blk)->instrs.pushBack(v);
                                    }
                                }
                                Slice<i32> args = makeSlice<i32>(length(yes));
                                {
                                    auto range1303 = yes;
                                    for (Int index1304 = 0, rangeEnd1305 = length(range1303); index1304 < rangeEnd1305; ++index1304) {
                                        Int j = index1304;
                                        i32 x = range1303[index1304];
                                        args[j] = x;
                                        if ((x != no[j])) {
                                            i32 v = functionNewValue(f, OpSelect, ((f)->values[x]).typ, b, i64(0LL), Span<const i32>({(t).value, x, no[j]}));
                                            (blk)->instrs.pushBack(v);
                                            args[j] = v;
                                        }
                                    }
                                }
                                (blk)->term = Term{TermJump, {}, sliceOf<Edge>({Edge{(merge).block, args}}), {}, {}};
                                (*(body)) = Block{{}, {}, {}, {}, true};
                                functionComputePreds(f);
                                changed = true;
                                break;
                            }
                        }
                    }
                }
                if ((!(changed))) {
                    break;
                }
            }
        }
    }

    // setArgs replaces the operands of v.
    static void optimizerSetArgs(Optimizer* o, i32 v, Span<const i32> args) {
        Function* f = (o)->f;
        Value* val = (&((f)->values[v]));
        if ((convert<i64>((val)->nargs) >= length(args))) {
            copySlice(subslice((f)->pool, (val)->args, length((f)->pool)), args);
            (val)->nargs = convert<u16>(length(args));
            return;
        }
        (val)->args = convert<i32>(length((f)->pool));
        (val)->nargs = convert<u16>(length(args));
        appendSpan((f)->pool, args);
    }

    // simplify folds constants and rewrites operations into simpler ones.
    static bool optimizerSimplify(Optimizer* o) {
        Function* f = (o)->f;
        bool changed = false;
        optimizerCountUses(o);
        {
            auto& range1306 = (f)->blocks;
            for (Int index1307 = 0, rangeEnd1308 = length(range1306); index1307 < rangeEnd1308; ++index1307) {
                Int b = index1307;
                Block* blk = (&((f)->blocks[b]));
                if ((blk)->dead) {
                    continue;
                }
                {
                    auto range1309 = (blk)->instrs.view();
                    for (Int index1310 = 0, rangeEnd1311 = length(range1309); index1310 < rangeEnd1311; ++index1310) {
                        i32 v = range1309[index1310];
                        if (optimizerSimplifyValue(o, v)) {
                            changed = true;
                        }
                    }
                }
                if (optimizerSimplifyTerm(o, convert<i32>(b))) {
                    changed = true;
                }
            }
        }
        optimizerApply(o);
        return changed;
    }

    static bool optimizerSimplifyTerm(Optimizer* o, i32 b) {
        Function* f = (o)->f;
        Term* t = (&(((f)->blocks[b]).term));
        {
            auto switch1312 = (t)->kind;
            switch (switch1312) {
                case TermIf: {
                    i32 c = optimizerResolve(o, (t)->value);
                    (t)->value = c;
                    Value* val = (&((f)->values[c]));
                    if (((val)->op == OpConst)) {
                        Edge e = (t)->edges[Int(1LL)];
                        if (((val)->aux != i64(0LL))) {
                            e = (t)->edges[Int(0LL)];
                        }
                        (*(t)) = Term{TermJump, {}, sliceOf<Edge>({e}), {}, {}};
                        return true;
                    }
                    if ((((val)->op == OpEqz) && (((f)->values[functionArgs(f, c)[Int(0LL)]]).typ == I32))) {
                        (t)->value = functionArgs(f, c)[Int(0LL)];
                        auto temporary1313 = (t)->edges[Int(1LL)];
                        auto temporary1314 = (t)->edges[Int(0LL)];
                        (t)->edges[Int(0LL)] = temporary1313;
                        (t)->edges[Int(1LL)] = temporary1314;
                        return true;
                    }
                    if ((((((val)->op == OpNe) && optimizerIsConst(o, functionArgs(f, c)[Int(1LL)])) && (optimizerConstOf(o, functionArgs(f, c)[Int(1LL)]) == i64(0LL))) && (((f)->values[functionArgs(f, c)[Int(0LL)]]).typ == I32))) {
                        (t)->value = functionArgs(f, c)[Int(0LL)];
                        return true;
                    }
                    if (((((t)->edges[Int(0LL)]).block == ((t)->edges[Int(1LL)]).block) && sameArgs(o, ((t)->edges[Int(0LL)]).args, ((t)->edges[Int(1LL)]).args))) {
                        (*(t)) = Term{TermJump, {}, sliceOf<Edge>({(t)->edges[Int(0LL)]}), {}, {}};
                        return true;
                    }
                    break;
                }
                case TermSwitch: {
                    i32 c = optimizerResolve(o, (t)->value);
                    (t)->value = c;
                    if ((((f)->values[c]).op == OpConst)) {
                        u64 i = convert<u64>(convert<u32>(((f)->values[c]).aux));
                        if ((i >= convert<u64>((length((t)->edges) - Int(1LL))))) {
                            i = convert<u64>((length((t)->edges) - Int(1LL)));
                        }
                        (*(t)) = Term{TermJump, {}, sliceOf<Edge>({(t)->edges[i]}), {}, {}};
                        return true;
                    }
                    break;
                }
            }
        }
        return false;
    }

    static bool optimizerSimplifyValue(Optimizer* o, i32 v) {
        Function* f = (o)->f;
        Value* val = (&((f)->values[v]));
        if (((convert<i64>(v) < length((o)->alias)) && ((o)->alias[v] >= i32(0LL)))) {
            return false;
        }
        Span<i32> args = functionArgs(f, v);
        {
            auto range1315 = args;
            for (Int index1316 = 0, rangeEnd1317 = length(range1315); index1316 < rangeEnd1317; ++index1316) {
                Int i = index1316;
                i32 a = range1315[index1316];
                args[i] = optimizerResolve(o, a);
            }
        }
        ValType t = (val)->typ;
        {
            if ((((((val)->nargs == u16(2LL)) && isIntOp((val)->op)) && ((val)->op != OpEqz)) && (!(isFloat(((f)->values[args[Int(0LL)]]).typ))))) {
                auto temporary1320 = args[Int(0LL)];
                auto temporary1321 = args[Int(1LL)];
                i32 a = temporary1320;
                i32 b = temporary1321;
                ValType at = ((f)->values[a]).typ;
                if ((optimizerIsConst(o, a) && optimizerIsConst(o, b))) {
                    {
                        auto temporary1322 = foldInt((val)->op, at, convert<u64>(optimizerConstOf(o, a)), convert<u64>(optimizerConstOf(o, b)));
                        u64 r = temporary1322.v0;
                        bool ok = temporary1322.v1;
                        if (ok) {
                            optimizerReplace(o, v, optimizerConstant(o, t, convert<i64>(r)));
                            return true;
                        }
                    }
                    return false;
                }
                return optimizerIdentity(o, v, (val)->op, a, b, at);
            } else if ((((val)->nargs == u16(1LL)) && (((((((val)->op >= OpClz) && ((val)->op <= OpEqz)) || ((val)->op == OpWrap)) || ((val)->op == OpExtendU)) || ((val)->op == OpExtendS))))) {
                i32 a = args[Int(0LL)];
                ValType from = ((f)->values[a]).typ;
                if (isFloat(from)) {
                    return false;
                }
                if (optimizerIsConst(o, a)) {
                    {
                        auto temporary1323 = foldUnary((val)->op, t, from, convert<u64>(optimizerConstOf(o, a)));
                        u64 r = temporary1323.v0;
                        bool ok = temporary1323.v1;
                        if (ok) {
                            optimizerReplace(o, v, optimizerConstant(o, t, convert<i64>(r)));
                            return true;
                        }
                    }
                }
                return optimizerUnaryIdentity(o, v, (val)->op, a);
            } else if (((val)->op == OpSelect)) {
                auto temporary1324 = args[Int(0LL)];
                auto temporary1325 = args[Int(1LL)];
                auto temporary1326 = args[Int(2LL)];
                i32 c = temporary1324;
                i32 x = temporary1325;
                i32 y = temporary1326;
                if ((x == y)) {
                    optimizerReplace(o, v, x);
                    return true;
                }
                if (optimizerIsConst(o, c)) {
                    if ((optimizerConstOf(o, c) != i64(0LL))) {
                        optimizerReplace(o, v, x);
                    } else {
                        optimizerReplace(o, v, y);
                    }
                    return true;
                }
            }
        switchEnd1319:;
        }
        return false;
    }

    // substitute maps arguments of an edge leaving block b to what they are on
    // an incoming edge with args: b's parameters become those.
    static Slice<i32> optimizerSubstitute(Optimizer* o, i32 b, Slice<i32> out, Slice<i32> in) {
        Slice<i32> res = makeSlice<i32>(length(out));
        {
            auto range1327 = out;
            for (Int index1328 = 0, rangeEnd1329 = length(range1327); index1328 < rangeEnd1329; ++index1328) {
                Int i = index1328;
                i32 a = range1327[index1328];
                a = optimizerResolve(o, a);
                {
                    Int k = optimizerParamIndex(o, b, a);
                    if ((k >= Int(0LL))) {
                        res[i] = in[k];
                    } else {
                        res[i] = a;
                    }
                }
            }
        }
        return res;
    }

    // thread sends edges that pass a constant to a block that only branches on
    // it straight to the branch's target. The block's parameters may be used
    // past it, as the loop of a Go function that dispatches on its resume
    // point reads its locals in every case: an edge bypassing the block goes
    // through a block of its own where they are the values the edge passed,
    // and repair makes the uses read whichever definition reaches them.
    static bool optimizerThread(Optimizer* o) {
        Function* f = (o)->f;
        bool changed = false;
        optimizerApply(o);
        optimizerCountUses(o);
        {
            i32 h = i32(0LL);
            for (; (h < convert<i32>(length((f)->blocks))); h++) {
                Block* blk = (&((f)->blocks[h]));
                if (((((blk)->dead || (h == i32(0LL))) || (length((blk)->instrs) != Int(0LL))) || (((((blk)->term).kind != TermSwitch) && (((blk)->term).kind != TermIf))))) {
                    continue;
                }
                Int k = optimizerParamIndex(o, h, optimizerResolve(o, ((blk)->term).value));
                if ((k < Int(0LL))) {
                    continue;
                }
                bool local = optimizerLocalParams(o, h);
                Map<i32, Bypass> bypasses = mapOf<i32, Bypass>({});
                TermKind kind = (((f)->blocks[h]).term).kind;
                Slice<Edge> targets = slicesClone((((f)->blocks[h]).term).edges);
                {
                    auto range1330 = ((f)->blocks[h]).preds;
                    for (Int index1331 = 0, rangeEnd1332 = length(range1330); index1331 < rangeEnd1332; ++index1331) {
                        i32 p = range1330[index1331];
                        {
                            auto range1333 = (((f)->blocks[p]).term).edges;
                            for (Int index1334 = 0, rangeEnd1335 = length(range1333); index1334 < rangeEnd1335; ++index1334) {
                                Int i = index1334;
                                Edge e = (((f)->blocks[p]).term).edges[i];
                                if ((((e).block != h) || (length((e).args) <= k))) {
                                    continue;
                                }
                                i32 c = optimizerResolve(o, (e).args[k]);
                                if ((!(optimizerIsConst(o, c)))) {
                                    continue;
                                }
                                u64 x = convert<u64>(convert<u32>(optimizerConstOf(o, c)));
                                Edge target = {};
                                if ((kind == TermIf)) {
                                    target = targets[Int(1LL)];
                                    if ((x != u64(0LL))) {
                                        target = targets[Int(0LL)];
                                    }
                                } else {
                                    if ((x >= convert<u64>((length(targets) - Int(1LL))))) {
                                        x = convert<u64>((length(targets) - Int(1LL)));
                                    }
                                    target = targets[x];
                                }
                                Slice<i32> args = optimizerSubstitute(o, h, (target).args, (e).args);
                                changed = true;
                                if (local) {
                                    (((f)->blocks[p]).term).edges[i] = Edge{(target).block, args};
                                    continue;
                                }
                                i32 b = functionNewBlock(f);
                                ((f)->blocks[b]).term = Term{TermJump, {}, sliceOf<Edge>({Edge{(target).block, args}}), {}, {}};
                                setMap(bypasses, b, Bypass{p, appendSlice(Slice<i32>{}, (e).args)});
                                (((f)->blocks[p]).term).edges[i] = Edge{b, {}};
                            }
                        }
                    }
                }
                if ((length(bypasses) > Int(0LL))) {
                    functionComputePreds(f);
                    optimizerRepair(o, h, bypasses);
                    optimizerCountUses(o);
                }
            }
        }
        if (changed) {
            functionComputePreds(f);
        }
        return changed;
    }

    // trivialParams replaces a parameter that receives one value on every edge,
    // apart from itself, by that value.
    static bool optimizerTrivialParams(Optimizer* o) {
        Function* f = (o)->f;
        bool changed = false;
        {
            auto& range1336 = (f)->blocks;
            for (Int index1337 = 0, rangeEnd1338 = length(range1336); index1337 < rangeEnd1338; ++index1337) {
                Int b = index1337;
                Block* blk = (&((f)->blocks[b]));
                if ((((blk)->dead || (b == Int(0LL))) || (length((blk)->params) == Int(0LL)))) {
                    continue;
                }
                Slice<bool> keep = {};
                bool removed = false;
                Vec<Edge*> in = optimizerIncoming(o, convert<i32>(b));
                {
                    auto range1339 = (blk)->params;
                    for (Int index1340 = 0, rangeEnd1341 = length(range1339); index1340 < rangeEnd1341; ++index1340) {
                        Int k = index1340;
                        i32 param = range1339[index1340];
                        i32 same = i32(-1LL);
                        bool trivial = true;
                        {
                            auto range1342 = in.view();
                            for (Int index1343 = 0, rangeEnd1344 = length(range1342); index1343 < rangeEnd1344; ++index1343) {
                                Edge* e = range1342[index1343];
                                i32 a = optimizerResolve(o, (e)->args[k]);
                                if (((a == param) || (a == same))) {
                                    continue;
                                }
                                if ((same >= i32(0LL))) {
                                    trivial = false;
                                    break;
                                }
                                same = a;
                            }
                        }
                        if (isNil(keep)) {
                            keep = makeSlice<bool>(length((blk)->params));
                        }
                        keep[k] = true;
                        if (trivial) {
                            if ((same < i32(0LL))) {
                                same = functionNewValue(f, OpUndef, ((f)->values[param]).typ, i32(-1LL), i64(0LL), Span<const i32>({}));
                            }
                            optimizerReplace(o, param, same);
                            keep[k] = false;
                            removed = true;
                        }
                    }
                }
                if ((!(removed))) {
                    continue;
                }
                optimizerDropParams(o, convert<i32>(b), keep);
                changed = true;
            }
        }
        if (changed) {
            optimizerApply(o);
        }
        return changed;
    }

    static bool optimizerUnaryIdentity(Optimizer* o, i32 v, Op op, i32 a) {
        Function* f = (o)->f;
        Value* src = (&((f)->values[a]));
        {
            auto switch1345 = op;
            switch (switch1345) {
                case OpEqz: {
                    {
                        auto temporary1346 = invertComparison((src)->op);
                        Op invert = temporary1346.v0;
                        bool ok = temporary1346.v1;
                        if (((ok && ((o)->uses[a] == i32(1LL))) && ((src)->block == ((f)->values[v]).block))) {
                            Span<i32> sa = functionArgs(f, a);
                            auto temporary1347 = sa[Int(0LL)];
                            auto temporary1348 = sa[Int(1LL)];
                            i32 x = temporary1347;
                            i32 y = temporary1348;
                            ((f)->values[v]).op = invert;
                            optimizerSetArgs(o, v, Span<const i32>({x, y}));
                            return true;
                        }
                    }
                    if ((((src)->op == OpEqz) && (((f)->values[functionArgs(f, a)[Int(0LL)]]).typ == I32))) {
                        i32 x = functionArgs(f, a)[Int(0LL)];
                        ((f)->values[v]).op = OpNe;
                        optimizerSetArgs(o, v, Span<const i32>({x, optimizerConstant(o, I32, i64(0LL))}));
                        return true;
                    }
                    break;
                }
                case OpWrap: {
                    {
                        auto switch1349 = (src)->op;
                        switch (switch1349) {
                            case OpExtendU:
                            case OpExtendS: {
                                optimizerReplace(o, v, functionArgs(f, a)[Int(0LL)]);
                                return true;
                                break;
                            }
                            case OpAdd:
                            case OpSub:
                            case OpMul:
                            case OpAnd:
                            case OpOr:
                            case OpXor:
                            case OpShl: {
                                Span<i32> sa = functionArgs(f, a);
                                auto temporary1350 = optimizerLow32(o, sa[Int(0LL)]);
                                i32 x = temporary1350.v0;
                                bool ok1 = temporary1350.v1;
                                auto temporary1351 = optimizerLow32(o, sa[Int(1LL)]);
                                i32 y = temporary1351.v0;
                                bool ok2 = temporary1351.v1;
                                if (((ok1 && ok2) && ((((src)->op != OpShl) || optimizerIsConst(o, sa[Int(1LL)]))))) {
                                    if ((((src)->op == OpShl) && ((convert<u64>(optimizerConstOf(o, sa[Int(1LL)])) & u64(63LL)) >= u64(32LL)))) {
                                        return false;
                                    }
                                    ((f)->values[v]).op = (src)->op;
                                    optimizerSetArgs(o, v, Span<const i32>({x, y}));
                                    return true;
                                }
                                break;
                            }
                        }
                    }
                    break;
                }
                case OpExtendU: {
                    if ((((src)->op == OpWrap) && optimizerIsZext(o, functionArgs(f, a)[Int(0LL)]))) {
                        optimizerReplace(o, v, functionArgs(f, a)[Int(0LL)]);
                        return true;
                    }
                    break;
                }
            }
        }
        return false;
    }

    static bool optimizerUnreachable(Optimizer* o) {
        Function* f = (o)->f;
        Slice<bool> reached = makeSlice<bool>(length((f)->blocks));
        Slice<i32> work = sliceOf<i32>({i32(0LL)});
        reached[Int(0LL)] = true;
        {
            for (; (length(work) > Int(0LL));) {
                i32 b = work[(length(work) - Int(1LL))];
                work = subslice(work, Int(0), (length(work) - Int(1LL)));
                {
                    auto range1352 = (((f)->blocks[b]).term).edges;
                    for (Int index1353 = 0, rangeEnd1354 = length(range1352); index1353 < rangeEnd1354; ++index1353) {
                        Edge e = range1352[index1353];
                        if ((!(reached[(e).block]))) {
                            reached[(e).block] = true;
                            work = appendValues(work, (e).block);
                        }
                    }
                }
            }
        }
        bool changed = false;
        {
            auto& range1355 = (f)->blocks;
            for (Int index1356 = 0, rangeEnd1357 = length(range1355); index1356 < rangeEnd1357; ++index1356) {
                Int b = index1356;
                if (((!(reached[b])) && (!(((f)->blocks[b]).dead)))) {
                    (f)->blocks[b] = Block{{}, {}, {}, {}, true};
                    changed = true;
                }
            }
        }
        if (changed) {
            functionComputePreds(f);
        }
        return changed;
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

    // constExpr returns the bytes of a constant expression up to and including
    // its end opcode.
    static Slice<u8> readerConstExpr(Reader* r) {
        Int start = (r)->pos;
        {
            for (; true;) {
                u8 op = readerByte(r);
                {
                    auto switch1358 = op;
                    switch (switch1358) {
                        case u8(11LL): {
                            return subslice((r)->data, start, (r)->pos);
                            break;
                        }
                        case u8(65LL): {
                            readerS32(r);
                            break;
                        }
                        case u8(66LL): {
                            readerS64(r);
                            break;
                        }
                        case u8(67LL): {
                            readerBytes(r, Int(4LL));
                            break;
                        }
                        case u8(68LL): {
                            readerBytes(r, Int(8LL));
                            break;
                        }
                        case u8(35LL):
                        case u8(210LL): {
                            readerU32(r);
                            break;
                        }
                        case u8(208LL): {
                            readerRefType(r);
                            break;
                        }
                        case u8(106LL):
                        case u8(107LL):
                        case u8(108LL):
                        case u8(124LL):
                        case u8(125LL):
                        case u8(126LL): {
                            break;
                        }
                        case u8(253LL): {
                            if ((readerU32(r) != u32(12LL))) {
                                compilerError(String("constant expression required at offset %d"));
                            }
                            readerBytes(r, Int(16LL));
                            break;
                        }
                        default: {
                            compilerError(String("constant expression required: opcode 0x%02x at offset %d"));
                            break;
                        }
                    }
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

    static u64 readerF64(Reader* r) {
        Slice<u8> b = readerBytes(r, Int(8LL));
        return (convert<u64>(readerLe32(r, subslice(b, Int(0LL), Int(4LL)))) | shiftLeft(convert<u64>(readerLe32(r, subslice(b, Int(4LL), Int(8LL)))), Int(32LL)));
    }

    static u32 readerLe32(Reader* r, Slice<u8> b) {
        return (((convert<u32>(b[Int(0LL)]) | shiftLeft(convert<u32>(b[Int(1LL)]), Int(8LL))) | shiftLeft(convert<u32>(b[Int(2LL)]), Int(16LL))) | shiftLeft(convert<u32>(b[Int(3LL)]), Int(24LL)));
    }

    // leb reads an unsigned LEB128 number of at most bits bits.
    static u64 readerLeb(Reader* r, Uint bits) {
        u64 result = {};
        Uint shift = {};
        {
            for (; true;) {
                u8 b = readerByte(r);
                if ((((shift + Uint(7LL)) >= bits) && (shiftRight(u8(b & u8(127LL)), ((bits - shift))) != u8(0LL)))) {
                    compilerError(String("integer too large at offset %d"));
                }
                result |= shiftLeft(convert<u64>(u8(b & u8(127LL))), shift);
                if ((u8(b & u8(128LL)) == u8(0LL))) {
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
        (l).hasMax = (u8(flags & u8(1LL)) != u8(0LL));
        (l).shared = (u8(flags & u8(2LL)) != u8(0LL));
        (l).is64 = (u8(flags & u8(4LL)) != u8(0LL));
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
        Slice<u8> b = readerBytes(r, convert<i64>(n));
        if ((!(utf8Valid(b)))) {
            compilerError(String("malformed UTF-8 encoding at offset %d"));
        }
        return asString(b);
    }

    static ValType readerRefType(Reader* r) {
        ValType t = convert<ValType>(readerByte(r));
        if (((t != FuncRef) && (t != ExternRef))) {
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

    // sleb reads a signed LEB128 number of at most bits bits.
    static i64 readerSleb(Reader* r, Uint bits) {
        i64 result = {};
        Uint shift = {};
        {
            for (; true;) {
                u8 b = readerByte(r);
                if (((shift + Uint(7LL)) >= bits)) {
                    i8 rest = shiftRight(convert<i8>(shiftLeft(b, Int(1LL))), ((bits - shift)));
                    if (((u8(b & u8(128LL)) != u8(0LL)) || (((rest != i8(0LL)) && (rest != i8(-1LL)))))) {
                        if ((u8(b & u8(128LL)) != u8(0LL))) {
                            compilerError(String("integer representation too long at offset %d"));
                        }
                        compilerError(String("integer too large at offset %d"));
                    }
                }
                result |= shiftLeft(convert<i64>(u8(b & u8(127LL))), shift);
                shift += Uint(7LL);
                if ((u8(b & u8(128LL)) == u8(0LL))) {
                    if (((shift < Uint(64LL)) && (u8(b & u8(64LL)) != u8(0LL)))) {
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
            auto switch1359 = t;
            switch (switch1359) {
                case I32:
                case I64:
                case F32:
                case F64:
                case V128:
                case FuncRef:
                case ExternRef: {
                    return t;
                    break;
                }
            }
        }
        compilerError(String("malformed value type 0x%02x at offset %d"));
        return ValType(0LL);
    }

    static u64 xorshiftNext(Xorshift* r) {
        (*(r)) ^= shiftLeft((*(r)), Int(13LL));
        (*(r)) ^= shiftRight((*(r)), Int(7LL));
        (*(r)) ^= shiftLeft((*(r)), Int(17LL));
        return convert<u64>((*(r)));
    }

    static bool vRegIsPhys(VReg r) {
        return ((r >= VReg(0LL)) && (r < firstVirtual));
    }

    static bool vRegIsXMM(VReg r) {
        return ((r >= XMM0) && (r < firstVirtual));
    }

    template <typename F>
    static void bitsetEach(Bitset s, F cb) {
        // Preserve ascending register order for identical interval allocation.
        visitBitsetWords(s, [&](usize index, u64 word) {
            while (word) {
                cb(Int(index * 64 + bitsTrailingZeros64(word)));
                word &= word - 1;
            }
        });
    }

    template <typename F>
    static void bitsetEachIntersection(Bitset s, Bitset filter, F cb) {
        visitBitsetWords(s, [&](usize index, u64 word) {
            word &= bitsetWord(filter, index);
            while (word) {
                cb(Int(index * 64 + bitsTrailingZeros64(word)));
                word &= word - 1;
            }
        });
    }

    static bool bitsetHas(Bitset s, Int i) {
        require(i >= 0 && s.data && usize(i) < s.data->bits, "bitset index");
        return (bitsetWord(s, usize(i) / 64) & (u64(1) << (usize(i) % 64))) != 0;
    }

    static void bitsetSet(Bitset s, Int i) {
        require(i >= 0 && s.data && usize(i) < s.data->bits, "bitset index");
        bitsetAddWord(s, usize(i) / 64, u64(1) << (usize(i) % 64));
    }

    // union adds o to s and reports whether s changed.
    static bool bitsetUnion(Bitset s, Bitset o) {
        if (s.data && o.data && s.data->bits <= denseBitsetLimit && s.data->bits == o.data->bits) {
            if (!o.data->occupied) {
                return false;
            }
            if (!s.data->occupied) {
                auto* words = bitsetDenseWords(s.data);
                copyBytes(words, o.data->storage, ((usize(s.data->bits) + 63) / 64) * sizeof(u64));
                s.data->occupied = o.data->occupied;
                return true;
            }
            auto* words = static_cast<u64*>(s.data->storage);
            const auto* source = static_cast<const u64*>(o.data->storage);
            bool changed = false;
            for (u32 occupied = o.data->occupied; occupied; occupied &= occupied - 1) {
                const usize index = usize(bitsTrailingZeros64(occupied));
                const u64 before = words[index];
                words[index] |= source[index];
                changed |= before != words[index];
            }
            s.data->occupied |= o.data->occupied;
            return changed;
        }
        if (s.data && o.data && s.data->bits > denseBitsetLimit && s.data->bits == o.data->bits) {
            bool changed = false;
            visitBitsetWords(o, [&](usize index, u64 word) {
                if (bitsetAddSparseWord(s.data, index, word)) {
                    changed = true;
                }
            });
            return changed;
        }
        bool changed = false;
        visitBitsetWords(o, [&](usize index, u64 word) {
            if (bitsetAddWord(s, index, word)) {
                changed = true;
            }
        });
        return changed;
    }

    static bool memKindSigned(MemKind k) {
        return (MemKind(k & memSigned) != MemKind(0LL));
    }

    static Int memKindWidth(MemKind k) {
        return convert<i64>(MemKind(k & MemKind(15LL)));
    }

    static Tuple2<AddressExpression, bool> addAddressExpression(AddressExpression a, AddressExpression b, u32 factor) {
        AddressExpression out = AddressExpression{{}, ((a).offset + (factor * (b).offset))};
        {
            auto range1366 = Array<Anonymous4, 2>{{Anonymous4{a, u32(1LL)}, Anonymous4{b, factor}}};
            for (Int index1367 = 0, rangeEnd1368 = length(range1366); index1367 < rangeEnd1368; ++index1367) {
                Anonymous4 source = range1366[index1367];
                {
                    auto range1369 = ((source).expr).terms;
                    for (Int index1370 = 0, rangeEnd1371 = length(range1369); index1370 < rangeEnd1371; ++index1370) {
                        AddressTerm term = range1369[index1370];
                        (term).mul *= (source).mul;
                        if (((term).mul == u32(0LL))) {
                            continue;
                        }
                        Int at = Int(-1LL);
                        {
                            auto range1372 = (out).terms;
                            for (Int index1373 = 0, rangeEnd1374 = length(range1372); index1373 < rangeEnd1374; ++index1373) {
                                Int j = index1373;
                                AddressTerm prior = range1372[index1373];
                                if ((((prior).mul != u32(0LL)) && ((prior).value == (term).value))) {
                                    at = j;
                                    break;
                                }
                            }
                        }
                        if ((at < Int(0LL))) {
                            {
                                auto range1375 = (out).terms;
                                for (Int index1376 = 0, rangeEnd1377 = length(range1375); index1376 < rangeEnd1377; ++index1376) {
                                    Int j = index1376;
                                    AddressTerm prior = range1375[index1376];
                                    if (((prior).mul == u32(0LL))) {
                                        at = j;
                                        ((out).terms[j]).value = (term).value;
                                        break;
                                    }
                                }
                            }
                        }
                        if ((at < Int(0LL))) {
                            return {AddressExpression{}, false};
                        }
                        ((out).terms[at]).mul += (term).mul;
                    }
                }
            }
        }
        Int n = Int(0LL);
        {
            auto range1378 = (out).terms;
            for (Int index1379 = 0, rangeEnd1380 = length(range1378); index1379 < rangeEnd1380; ++index1379) {
                Int j = index1379;
                if ((((out).terms[j]).mul != u32(0LL))) {
                    (out).terms[n] = (out).terms[j];
                    n++;
                }
            }
        }
        {
            Int j = n;
            for (; (j < Int(4LL)); j++) {
                (out).terms[j] = AddressTerm{};
            }
        }
        {
            Int j = Int(1LL);
            for (; (j < n); j++) {
                {
                    Int k = j;
                    for (; ((k > Int(0LL)) && (((out).terms[k]).value < ((out).terms[(k - Int(1LL))]).value)); k--) {
                        auto temporary1381 = (out).terms[(k - Int(1LL))];
                        auto temporary1382 = (out).terms[k];
                        (out).terms[k] = temporary1381;
                        (out).terms[(k - Int(1LL))] = temporary1382;
                    }
                }
            }
        }
        return {out, true};
    }

    static LoopSpan addLoopSpans(LoopSpan a, LoopSpan b) {
        if ((((!((a).valid)) || (!((b).valid))) || ((((a).factor != u64(0LL)) && ((b).factor != u64(0LL))) && ((a).stride != (b).stride)))) {
            return LoopSpan{};
        }
        LoopSpan out = LoopSpan{i32(-1LL), {}, ((a).factor + (b).factor), ((a).offset + (b).offset), {}, {}, ((a).lo + (b).lo), (a).stride, true};
        if (((a).factor == u64(0LL))) {
            (out).stride = (b).stride;
        }
        {
            auto range1383 = Array<Array<i64, 2>, 4>{{Array<i64, 2>{{convert<i64>((a).base), convert<i64>((a).mul)}}, Array<i64, 2>{{convert<i64>((a).base2), convert<i64>((a).mul2)}}, Array<i64, 2>{{convert<i64>((b).base), convert<i64>((b).mul)}}, Array<i64, 2>{{convert<i64>((b).base2), convert<i64>((b).mul2)}}}};
            for (Int index1384 = 0, rangeEnd1385 = length(range1383); index1384 < rangeEnd1385; ++index1384) {
                Array<i64, 2> term = range1383[index1384];
                auto temporary1386 = convert<i32>(term[Int(0LL)]);
                auto temporary1387 = convert<u64>(term[Int(1LL)]);
                i32 v = temporary1386;
                u64 mul = temporary1387;
                if ((mul == u64(0LL))) {
                    continue;
                }
                {
                    if ((((out).mul == u64(0LL)) || ((out).base == v))) {
                        (out).base = v;
                        (out).mul += mul;
                    } else if ((((out).mul2 == u64(0LL)) || ((out).base2 == v))) {
                        (out).base2 = v;
                        (out).mul2 += mul;
                    } else {
                        return LoopSpan{};
                    }
                switchEnd1389:;
                }
            }
        }
        if ((((out).mul2 != u64(0LL)) && ((out).base2 < (out).base))) {
            auto temporary1390 = (out).base2;
            auto temporary1391 = (out).base;
            (out).base = temporary1390;
            (out).base2 = temporary1391;
            auto temporary1392 = (out).mul2;
            auto temporary1393 = (out).mul;
            (out).mul = temporary1392;
            (out).mul2 = temporary1393;
        }
        return out;
    }

    static AddressExpression addressAtom(i32 v) {
        return AddressExpression{Array<AddressTerm, 4>{{AddressTerm{v, u32(1LL)}}}, {}};
    }

    static i32 alignTo(i32 x, i32 n) {
        return ((((x + n) - i32(1LL))) & ~(((n - i32(1LL)))));
    }

    // allocatable reports registers the allocator may hand out.
    static bool allocatable(VReg r) {
        return ((((((r != RSP) && (r != RBP)) && (r != R14)) && (r != ctxReg)) && (r != memReg)) && (r != VReg(31LL)));
    }

    static Allocator* allocate(MFunc* mf) {
        // The first allocation is discarded after copy coalescing. Its
        // intervals and liveness facts must not overlap the second pass.
        ObjPool* arena = activePool->make<ObjPool>();
        PoolScope scope(*arena);
        Allocator* a = arena->make<Allocator>();
        a->mf = mf;
        allocatorLinearize(a);
        if (allocatorSelectCopies(a)) {
            allocatorLiveness(a, true);
            allocatorBuildIntervals<true>(a);
            allocatorCoalesce(a);
            // Coalescing only rewrites operands and replaces copies with
            // no-ops; block order and instruction positions stay valid.
            Vec<i32> order(move(a->order));
            Vec<i32> blockFrom(move(a->blockFrom));
            Vec<i32> blockTo(move(a->blockTo));
            Vec<i32> posBlock(move(a->posBlock));
            // Coalescing stores register numbers and views of the original
            // move arrays in mf; none of this arena's objects escape to it.
            arena->clear();
            a = arena->make<Allocator>();
            a->mf = mf;
            a->order = move(order);
            a->blockFrom = move(blockFrom);
            a->blockTo = move(blockTo);
            a->posBlock = move(posBlock);
        }
        allocatorLiveness(a);
        allocatorBuildIntervals(a);
        allocatorScan(a);
        allocatorResolve(a);
        return a;
    }

    static Slice<u8> appendLeb(Slice<u8> b, u64 v) {
        {
            for (; true;) {
                u8 c = convert<u8>((v & u64(127LL)));
                v = shiftRight(v, Int(7LL));
                if ((v == u64(0LL))) {
                    return appendValues(b, c);
                }
                b = appendValues(b, u8(c | u8(128LL)));
            }
        }
    }

    static Slice<u8> appendSleb(Slice<u8> b, i64 v) {
        {
            for (; true;) {
                u8 c = convert<u8>((v & i64(127LL)));
                v = shiftRight(v, Int(7LL));
                if (((((v == i64(0LL)) && (u8(c & u8(64LL)) == u8(0LL)))) || (((v == i64(-1LL)) && (u8(c & u8(64LL)) != u8(0LL)))))) {
                    return appendValues(b, c);
                }
                b = appendValues(b, u8(c | u8(128LL)));
            }
        }
    }

    static Slice<u8> appendUleb(Slice<u8> out, u64 x) {
        return appendLeb(out, x);
    }

    static Slice<i32> appendUnique(Slice<i32> list, i32 x) {
        {
            auto range1394 = list;
            for (Int index1395 = 0, rangeEnd1396 = length(range1394); index1395 < rangeEnd1396; ++index1395) {
                i32 y = range1394[index1395];
                if ((y == x)) {
                    return list;
                }
            }
        }
        return appendValues(list, x);
    }

    static Slice<VReg> assigned(Slice<VReg> regs) {
        Slice<VReg> out = {};
        {
            auto range1397 = regs;
            for (Int index1398 = 0, rangeEnd1399 = length(range1397); index1398 < rangeEnd1399; ++index1398) {
                VReg r = range1397[index1398];
                if ((r != noReg)) {
                    out = appendValues(out, r);
                }
            }
        }
        return out;
    }

    static Amem baseAt(VReg base, i32 disp) {
        return Amem{base, Int(-1LL), u8(1LL), disp, i32(-1LL), {}};
    }

    // breakPatterns_func scatters some elements around in an attempt to break some patterns
    // that might cause imbalanced partitions in quicksort.
    template <typename Data>
    static void breakPatterns_func(Data data, Int a, Int b) {
        Int lengthValue = (b - a);
        if ((lengthValue >= Int(8LL))) {
            Xorshift random = convert<Xorshift>(lengthValue);
            Uint modulus = nextPowerOfTwo(lengthValue);
            {
                Int idx = ((a + (((lengthValue / Int(4LL))) * Int(2LL))) - Int(1LL));
                for (; (idx <= ((a + (((lengthValue / Int(4LL))) * Int(2LL))) + Int(1LL))); idx++) {
                    Int other = convert<i64>((convert<Uint>(xorshiftNext(&(random))) & ((modulus - Uint(1LL)))));
                    if ((other >= lengthValue)) {
                        other -= lengthValue;
                    }
                    (data).Swap(idx, (a + other));
                }
            }
        }
    }

    static Function* buildFunction(Module* m, Int index) {
        Func* fn = (&((m)->funcs[index]));
        Function* f = activePool->make<Function>();
        f->module = m;
        f->index = index;
        f->typ = &m->types[fn->typ];
        Frontend* fe = activePool->make<Frontend>(Frontend{m, f, Reader{(fn)->body, {}}, {}, {}, {}, {}, {}, {}, {}, {}, {}, mapOf<ConstKey, i32>({}), mapOf<i32, Slice<i32>>({}), {}});
        frontendRun(fe);
        return f;
    }

    // Carry successful memory checks through SSA edges and parameters. A
    // parameter is checked only as far as every incoming argument has already
    // been checked. Starting with no facts keeps cyclic proofs conservative.
    static Slice<Slice<CheckedValue>> checkedValues(Function* f, CfgInfo* ci) {
        Int limit = minimum(Int(1024LL), (Int(1048576LL) / length((f)->blocks)));
        if ((limit == Int(0LL))) {
            return {};
        }
        Slice<Slice<CheckedEdge>> incoming = makeSlice<Slice<CheckedEdge>>(length((f)->blocks));
        {
            auto range1400 = (ci)->order;
            for (Int index1401 = 0, rangeEnd1402 = length(range1400); index1401 < rangeEnd1402; ++index1401) {
                i32 b = range1400[index1401];
                {
                    auto range1403 = (((f)->blocks[b]).term).edges;
                    for (Int index1404 = 0, rangeEnd1405 = length(range1403); index1404 < rangeEnd1405; ++index1404) {
                        Int j = index1404;
                        Edge* e = (&((((f)->blocks[b]).term).edges[j]));
                        incoming[(e)->block] = appendValues(incoming[(e)->block], CheckedEdge{b, e});
                    }
                }
            }
        }
        Slice<i32> tracked = makeSlice<i32>(length((f)->values));
        {
            auto range1406 = tracked;
            for (Int index1407 = 0, rangeEnd1408 = length(range1406); index1407 < rangeEnd1408; ++index1407) {
                Int j = index1407;
                tracked[j] = i32(-1LL);
            }
        }
        Slice<i32> values = {};
        Fn<void(i32)> add = {};
        add = [&](i32 v) mutable -> void {
            if (((tracked[v] >= i32(0LL)) || (length(values) > limit))) {
                return;
            }
            tracked[v] = convert<i32>(length(values));
            values = appendValues(values, v);
            Value* val = (&((f)->values[v]));
            if (((val)->op == OpParam)) {
                {
                    auto range1409 = incoming[(val)->block];
                    for (Int index1410 = 0, rangeEnd1411 = length(range1409); index1410 < rangeEnd1411; ++index1410) {
                        CheckedEdge e = range1409[index1410];
                        add(((e).edge)->args[(val)->aux]);
                    }
                }
            }
        };
        {
            auto range1412 = (ci)->order;
            for (Int index1413 = 0, rangeEnd1414 = length(range1412); index1413 < rangeEnd1414; ++index1413) {
                i32 b = range1412[index1413];
                {
                    auto range1415 = ((f)->blocks[b]).instrs.view();
                    for (Int index1416 = 0, rangeEnd1417 = length(range1415); index1416 < rangeEnd1417; ++index1416) {
                        i32 v = range1415[index1416];
                        Value* val = (&((f)->values[v]));
                        if ((((val)->op == OpLoad) || ((val)->op == OpStore))) {
                            i32 addr = functionArgs(f, v)[Int(0LL)];
                            if ((((f)->values[addr]).op == OpParam)) {
                                add(addr);
                            }
                        }
                    }
                }
            }
        }
        Int n = length(values);
        if (((n == Int(0LL)) || (n > limit))) {
            return {};
        }
        Slice<Slice<Int>> defs = makeSlice<Slice<Int>>(length((f)->blocks));
        {
            auto range1418 = values;
            for (Int index1419 = 0, rangeEnd1420 = length(range1418); index1419 < rangeEnd1420; ++index1419) {
                Int j = index1419;
                i32 v = range1418[index1419];
                {
                    i32 b = ((f)->values[v]).block;
                    if ((b >= i32(0LL))) {
                        defs[b] = appendValues(defs[b], j);
                    }
                }
            }
        }
        Slice<i64> out = makeSlice<i64>((n * length((f)->blocks)));
        Slice<i64> cur = makeSlice<i64>(n);
        auto entry = [&](i32 b) mutable -> void {
            bool first = true;
            if ((b != i32(0LL))) {
                {
                    auto range1421 = ((f)->blocks[b]).preds;
                    for (Int index1422 = 0, rangeEnd1423 = length(range1421); index1422 < rangeEnd1423; ++index1422) {
                        i32 p = range1421[index1422];
                        if (((ci)->rpoIndex[p] < i32(0LL))) {
                            continue;
                        }
                        Slice<i64> row = subslice(out, (convert<i64>(p) * n), (((convert<i64>(p) + Int(1LL))) * n));
                        {
                            auto range1424 = cur;
                            for (Int index1425 = 0, rangeEnd1426 = length(range1424); index1425 < rangeEnd1426; ++index1425) {
                                Int j = index1425;
                                if (first) {
                                    cur[j] = row[j];
                                } else {
                                    cur[j] = minimum(cur[j], row[j]);
                                }
                            }
                        }
                        first = false;
                    }
                }
            }
            if (first) {
                clearMap(cur);
            }
            {
                auto range1427 = defs[b];
                for (Int index1428 = 0, rangeEnd1429 = length(range1427); index1428 < rangeEnd1429; ++index1428) {
                    Int j = range1427[index1428];
                    cur[j] = i64(0LL);
                }
            }
            {
                auto range1430 = ((f)->blocks[b]).params;
                for (Int index1431 = 0, rangeEnd1432 = length(range1430); index1431 < rangeEnd1432; ++index1431) {
                    i32 p = range1430[index1431];
                    i32 j = tracked[p];
                    if ((((j < i32(0LL)) || (b == i32(0LL))) || (length(incoming[b]) == Int(0LL)))) {
                        continue;
                    }
                    i64 end = i64(4611686018427387904LL);
                    {
                        auto range1433 = incoming[b];
                        for (Int index1434 = 0, rangeEnd1435 = length(range1433); index1434 < rangeEnd1435; ++index1434) {
                            CheckedEdge e = range1433[index1434];
                            i32 arg = tracked[((e).edge)->args[((f)->values[p]).aux]];
                            end = minimum(end, out[((convert<i64>((e).pred) * n) + convert<i64>(arg))]);
                        }
                    }
                    cur[j] = end;
                }
            }
        };
        {
            Int round = Int(0LL);
            for (; (round < Int(16LL)); round++) {
                bool changed = false;
                {
                    auto range1436 = (ci)->order;
                    for (Int index1437 = 0, rangeEnd1438 = length(range1436); index1437 < rangeEnd1438; ++index1437) {
                        i32 b = range1436[index1437];
                        entry(b);
                        {
                            auto range1439 = ((f)->blocks[b]).instrs.view();
                            for (Int index1440 = 0, rangeEnd1441 = length(range1439); index1440 < rangeEnd1441; ++index1440) {
                                i32 v = range1439[index1440];
                                Value* val = (&((f)->values[v]));
                                if ((((val)->op == OpLoad) || ((val)->op == OpStore))) {
                                    {
                                        i32 j = tracked[functionArgs(f, v)[Int(0LL)]];
                                        if ((j >= i32(0LL))) {
                                            cur[j] = maximum(cur[j], ((val)->aux + convert<i64>(memKindWidth(convert<MemKind>((val)->aux2)))));
                                        }
                                    }
                                }
                            }
                        }
                        Slice<i64> row = subslice(out, (convert<i64>(b) * n), (((convert<i64>(b) + Int(1LL))) * n));
                        {
                            auto range1442 = cur;
                            for (Int index1443 = 0, rangeEnd1444 = length(range1442); index1443 < rangeEnd1444; ++index1443) {
                                Int j = index1443;
                                i64 end = range1442[index1443];
                                changed = (changed || (row[j] != end));
                                row[j] = end;
                            }
                        }
                    }
                }
                if ((!(changed))) {
                    break;
                }
            }
        }
        Slice<Slice<CheckedValue>> proofs = makeSlice<Slice<CheckedValue>>(length((f)->blocks));
        {
            auto range1445 = (ci)->order;
            for (Int index1446 = 0, rangeEnd1447 = length(range1445); index1446 < rangeEnd1447; ++index1446) {
                i32 b = range1445[index1446];
                entry(b);
                {
                    auto range1448 = cur;
                    for (Int index1449 = 0, rangeEnd1450 = length(range1448); index1449 < rangeEnd1450; ++index1449) {
                        Int j = index1449;
                        i64 end = range1448[index1449];
                        i32 v = values[j];
                        i32 owner = ((f)->values[v]).block;
                        if (((end > i64(0LL)) && (((owner < i32(0LL)) || cfgInfoDominates(ci, owner, b))))) {
                            proofs[b] = appendValues(proofs[b], CheckedValue{v, end});
                        }
                    }
                }
            }
        }
        return proofs;
    }

    // choosePivot_func chooses a pivot in data[a:b].
    //
    // [0,8): chooses a static pivot.
    // [8,shortestNinther): uses the simple median-of-three method.
    // [shortestNinther,∞): uses the Tukey ninther method.
    template <typename Data>
    static Tuple2<Int, SortedHint> choosePivot_func(Data data, Int a, Int b) {
        Int pivot{};
        SortedHint hint{};
        constexpr Int shortestNinther = Int(50LL);
        constexpr Int maxSwaps = Int(12LL);
        Int l = (b - a);
        Int swaps = {};
        Int i = (a + ((l / Int(4LL)) * Int(1LL)));
        Int j = (a + ((l / Int(4LL)) * Int(2LL)));
        Int k = (a + ((l / Int(4LL)) * Int(3LL)));
        if ((l >= Int(8LL))) {
            if ((l >= Int(50LL))) {
                i = medianAdjacent_func(data, i, (&(swaps)));
                j = medianAdjacent_func(data, j, (&(swaps)));
                k = medianAdjacent_func(data, k, (&(swaps)));
            }
            j = median_func(data, i, j, k, (&(swaps)));
        }
        {
            auto switch1451 = swaps;
            switch (switch1451) {
                case Int(0LL): {
                    return {j, increasingHint};
                    break;
                }
                case Int(12LL): {
                    return {j, decreasingHint};
                    break;
                }
                default: {
                    return {j, unknownHint};
                    break;
                }
            }
        }
    }

    // constOffset evaluates a constant offset expression: i32.const or
    // i64.const.
    static Tuple2<i64, bool> constOffset(Slice<u8> expr) {
        Reader reader{expr, {}};
        Reader* r = &reader;
        u8 op = readerByte(r);
        i64 v = {};
        {
            auto switch1452 = op;
            switch (switch1452) {
                case u8(65LL): {
                    v = convert<i64>(convert<u32>(readerS32(r)));
                    break;
                }
                case u8(66LL): {
                    v = readerS64(r);
                    break;
                }
                default: {
                    return {i64(0LL), false};
                    break;
                }
            }
        }
        return {v, ((readerByte(r) == u8(11LL)) && readerEof(r))};
    }

    static MInst copyMachineInst(MInst in) {
        if (auto* old = in.extra) {
            in.extra = nullptr;
            auto* copy = mInstExtra(&in);
            copy->uses.assign(old->uses.view());
            copy->defs.assign(old->defs.view());
            copy->temps.assign(old->temps.view());
            copy->moves.assign(old->moves.view());
            copy->table.assign(old->table.view());
        }
        return in;
    }

    static i32 dataLabel(Int seg) {
        return (constCount + convert<i32>(seg));
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
                Reader* body = activePool->make<Reader>(Reader{readerBytes(r, convert<i64>(size)), {}});
                Int base = ((r)->pos - convert<i64>(size));
                if ((id != u8(0LL))) {
                    if ((order(id) <= order(last))) {
                        compilerError(String("unexpected content after last section"));
                    }
                    last = id;
                }
                {
                    auto switch1453 = id;
                    switch (switch1453) {
                        case u8(0LL): {
                            moduleCustom(m, body);
                            break;
                        }
                        case u8(1LL): {
                            moduleTypeSection(m, body);
                            break;
                        }
                        case u8(2LL): {
                            moduleImportSection(m, body);
                            break;
                        }
                        case u8(3LL): {
                            u32 n = readerU32(body);
                            {
                                u32 i = u32(0LL);
                                for (; (i < n); i++) {
                                    funcTypes = appendValues(funcTypes, readerU32(body));
                                }
                            }
                            break;
                        }
                        case u8(4LL): {
                            u32 n = readerU32(body);
                            {
                                u32 i = u32(0LL);
                                for (; (i < n); i++) {
                                    ValType elem = readerRefType(body);
                                    (m)->tables = appendValues((m)->tables, Table{elem, readerLimits(body, false), {}});
                                }
                            }
                            break;
                        }
                        case u8(5LL): {
                            u32 n = readerU32(body);
                            {
                                u32 i = u32(0LL);
                                for (; (i < n); i++) {
                                    (m)->mems = appendValues((m)->mems, Memory{readerLimits(body, true), {}});
                                }
                            }
                            break;
                        }
                        case u8(6LL): {
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
                            break;
                        }
                        case u8(7LL): {
                            moduleExportSection(m, body);
                            break;
                        }
                        case u8(8LL): {
                            (m)->start = convert<i64>(readerU32(body));
                            break;
                        }
                        case u8(9LL): {
                            moduleElemSection(m, body);
                            break;
                        }
                        case u8(10LL): {
                            Int n = convert<i64>(readerU32(body));
                            if ((n != length(funcTypes))) {
                                compilerError(String("function and code section have inconsistent lengths"));
                            }
                            {
                                Int i = Int(0LL);
                                for (; (i < n); i++) {
                                    Int size = convert<i64>(readerU32(body));
                                    Int offset = (base + (body)->pos);
                                    (m)->funcs = appendValues((m)->funcs, Func{funcTypes[i], {}, {}, {}, readerBytes(body, size), offset});
                                }
                            }
                            codes = n;
                            break;
                        }
                        case u8(11LL): {
                            moduleDataSection(m, body);
                            break;
                        }
                        case u8(12LL): {
                            (m)->dataCount = convert<i64>(readerU32(body));
                            break;
                        }
                        default: {
                            compilerError(String("malformed section id %d"));
                            break;
                        }
                    }
                }
                if ((!(readerEof(body)))) {
                    compilerError(String("section size mismatch in section %d"));
                }
            }
        }
        if ((codes != length(funcTypes))) {
            compilerError(String("function and code section have inconsistent lengths"));
        }
        if ((((m)->dataCount >= i64(0LL)) && (convert<i64>((m)->dataCount) != length((m)->datas)))) {
            compilerError(String("data count and data section have inconsistent lengths"));
        }
        {
            auto range1454 = (m)->funcs;
            for (Int index1455 = 0, rangeEnd1456 = length(range1454); index1455 < rangeEnd1456; ++index1455) {
                Int i = index1455;
                if ((convert<i64>(((m)->funcs[i]).typ) >= length((m)->types))) {
                    compilerError(String("unknown type %d"));
                }
            }
        }
        if ((length((m)->mems) > Int(1LL))) {
            compilerError(String("unsupported: %d memories"));
        }
        {
            auto range1457 = (m)->datas;
            for (Int index1458 = 0, rangeEnd1459 = length(range1457); index1458 < rangeEnd1459; ++index1458) {
                Int i = index1458;
                Data d = range1457[index1458];
                if (((d).memory != u32(0LL))) {
                    compilerError(String("unsupported: data segment %d of memory %d"));
                }
            }
        }
        return m;
    }

    static Slice<i32> dedup(Slice<i32> xs) {
        if ((length(xs) < Int(2LL))) {
            return xs;
        }
        Slice<i32> out = subslice(xs, Int(0), Int(1LL));
        {
            auto range1460 = subslice(xs, Int(1LL), length(xs));
            for (Int index1461 = 0, rangeEnd1462 = length(range1460); index1461 < rangeEnd1462; ++index1461) {
                i32 x = range1460[index1461];
                if ((x != out[(length(out) - Int(1LL))])) {
                    out = appendValues(out, x);
                }
            }
        }
        return out;
    }

    // displayName keeps a name readable in tracebacks: only control bytes,
    // which a symbol table cannot hold or show, become _XX.
    static String displayName(String s) {
        Slice<u8> b = makeSlice<u8>(Int(0LL), length(s));
        {
            Int i = Int(0LL);
            for (; (i < length(s)); i++) {
                {
                    u8 c = s[i];
                    if (((c < u8(32LL)) || (c == u8(127LL)))) {
                        b = appendSlice(b, fmtSprintf(String("_%02X"), c));
                    } else {
                        b = appendValues(b, c);
                    }
                }
            }
        }
        return asString(b);
    }

    static Span<Move> dropSelfMoves(Span<Move> moves) {
        Int count = 0;
        {
            auto range1463 = moves;
            for (Int index1464 = 0, rangeEnd1465 = length(range1463); index1464 < rangeEnd1465; ++index1464) {
                Move m = range1463[index1464];
                if (((m).dst != (m).src)) {
                    moves[count++] = m;
                }
            }
        }
        return subslice(moves, 0, count);
    }

    // ehFrame writes a CIE and an FDE per region, and the offsets of the FDEs'
    // starting addresses, which a relocation to the text fills.
    static Tuple2<Slice<u8>, Slice<u32>> ehFrame(Slice<Region> regions) {
        Slice<u8> out = {};
        Slice<u32> starts = {};
        auto entry = [&](Slice<u8> body) mutable -> void {
            {
                for (; ((((Int(4LL) + length(body))) % Int(8LL)) != Int(0LL));) {
                    body = appendValues(body, u8(0LL));
                }
            }
            out = littleAppendUint32(out, convert<u32>(length(body)));
            out = appendSlice(out, body);
        };
        Slice<u8> cie = sliceOf<u8>({u8(0LL), u8(0LL), u8(0LL), u8(0LL), u8(1LL), u8(122LL), u8(82LL), u8(0LL), u8(1LL), u8(120LL), u8(16LL), u8(1LL), u8(27LL), u8(12LL), u8(7LL), u8(8LL), u8(144LL), u8(1LL)});
        entry(cie);
        {
            auto range1466 = regions;
            for (Int index1467 = 0, rangeEnd1468 = length(range1466); index1467 < rangeEnd1468; ++index1467) {
                Region r = range1466[index1467];
                Int at = length(out);
                Slice<u8> body = littleAppendUint32({}, convert<u32>((at + Int(4LL))));
                starts = appendValues(starts, convert<u32>((at + Int(8LL))));
                body = littleAppendUint32(body, u32(0LL));
                body = littleAppendUint32(body, ((r).end - (r).start));
                body = appendValues(body, u8(0LL));
                i32 loc = i32(0LL);
                {
                    auto range1469 = (r).sp;
                    for (Int index1470 = 0, rangeEnd1471 = length(range1469); index1470 < rangeEnd1471; ++index1470) {
                        Int k = index1470;
                        SpEntry e = range1469[index1470];
                        if (((k == Int(0LL)) && ((e).delta == i32(0LL)))) {
                            continue;
                        }
                        {
                            i32 d = ((e).pc - loc);
                            if ((d == i32(0LL))) {
                            } else if ((d < i32(64LL))) {
                                body = appendValues(body, u8(u8(64LL) | convert<u8>(d)));
                            } else if ((d < i32(256LL))) {
                                body = appendValues(body, u8(2LL), convert<u8>(d));
                            } else if ((d < i32(65536LL))) {
                                body = littleAppendUint16(appendValues(body, u8(3LL)), convert<u16>(d));
                            } else {
                                body = littleAppendUint32(appendValues(body, u8(4LL)), convert<u32>(d));
                            }
                        switchEnd1473:;
                        }
                        loc = (e).pc;
                        body = appendUleb(appendValues(body, u8(14LL)), convert<u64>(((e).delta + i32(8LL))));
                    }
                }
                entry(body);
            }
        }
        return {out, starts};
    }

    // encodeNote writes the descriptor: imports and exports of functions, the
    // init symbol and the regions are not in it.
    static Slice<u8> encodeNote(Meta* m) {
        MetaWriter* w = activePool->make<MetaWriter>(MetaWriter{});
        metaWriterU(w, convert<u64>((m)->instanceSize));
        metaWriterFlag(w, (!isNil((m)->memory)));
        if ((!isNil((m)->memory))) {
            metaWriterU(w, ((m)->memory)->min);
            metaWriterU(w, ((m)->memory)->max);
            metaWriterFlag(w, ((m)->memory)->imported);
        }
        metaWriterU(w, convert<u64>(length((m)->tables)));
        {
            auto range1474 = (m)->tables;
            for (Int index1475 = 0, rangeEnd1476 = length(range1474); index1475 < rangeEnd1476; ++index1475) {
                TableMeta t = range1474[index1475];
                metaWriterU(w, convert<u64>((t).offset));
                metaWriterU(w, (t).min);
                metaWriterU(w, (t).max);
                metaWriterFlag(w, (t).imported);
            }
        }
        metaWriterU(w, convert<u64>(length((m)->globals)));
        {
            auto range1477 = (m)->globals;
            for (Int index1478 = 0, rangeEnd1479 = length(range1477); index1478 < rangeEnd1479; ++index1478) {
                GlobalMeta g = range1477[index1478];
                metaWriterU(w, convert<u64>((g).offset));
                metaWriterU(w, convert<u64>((g).typ));
                metaWriterFlag(w, (g).mutableValue);
                metaWriterFlag(w, (g).imported);
            }
        }
        metaWriterU(w, convert<u64>(length((m)->imports)));
        {
            auto range1480 = (m)->imports;
            for (Int index1481 = 0, rangeEnd1482 = length(range1480); index1481 < rangeEnd1482; ++index1481) {
                ImportMeta i = range1480[index1481];
                metaWriterU(w, convert<u64>((i).kind));
                metaWriterStr(w, (i).module);
                metaWriterStr(w, (i).name);
                metaWriterU(w, convert<u64>((i).index));
            }
        }
        metaWriterU(w, convert<u64>(length((m)->exports)));
        {
            auto range1483 = (m)->exports;
            for (Int index1484 = 0, rangeEnd1485 = length(range1483); index1484 < rangeEnd1485; ++index1484) {
                ExportMeta e = range1483[index1484];
                metaWriterU(w, convert<u64>((e).kind));
                metaWriterStr(w, (e).name);
                metaWriterU(w, convert<u64>((e).index));
            }
        }
        if (((!isNil((m)->memory)) && (((m)->memory)->tail != u64(0LL)))) {
            metaWriterU(w, ((m)->memory)->tail);
            metaWriterU(w, convert<u64>(((m)->memory)->tailEnd));
            if ((((m)->memory)->profile != u32(0LL))) {
                metaWriterU(w, convert<u64>(((m)->memory)->profile));
                metaWriterU(w, convert<u64>(((m)->memory)->profileFuncs));
            }
        }
        return (w)->out;
    }

    static String exportSymbol(String name, FuncType* t) {
        return (((((symExport + symbolPart(name)) + String(".")) + typesPart((t)->params)) + String(".")) + typesPart((t)->results));
    }

    static u8 fAluPP(FAluOp op, u8 size) {
        if ((op >= fAnd)) {
            return packedPP(size);
        }
        return scalarPP(size);
    }

    static MemoryLoop* findMemoryLoop(Function* f, CfgInfo* ci, i32 h) {
        MemoryLoop* l = activePool->make<MemoryLoop>(MemoryLoop{f, ci, h, i32(-1LL), i32(-1LL), i32(-1LL), {}, makeSlice<bool>(length((f)->blocks)), {}, {}, i32(-1LL), u64(1LL), {}, {}, {}, i32(-1LL), i32(-1LL), {}, mapOf<i32, LoopSpan>({}), mapOf<i32, LoopSpan>({}), {}, {}, {}, {}, {}, {}});
        {
            auto range1486 = ((f)->blocks[h]).preds;
            for (Int index1487 = 0, rangeEnd1488 = length(range1486); index1487 < rangeEnd1488; ++index1487) {
                i32 p = range1486[index1487];
                if (cfgInfoDominates(ci, h, p)) {
                    if (((l)->latch >= i32(0LL))) {
                        return {};
                    }
                    (l)->latch = p;
                } else {
                    if (((l)->preheader >= i32(0LL))) {
                        return {};
                    }
                    (l)->preheader = p;
                }
            }
        }
        if ((((l)->latch < i32(0LL)) || ((l)->preheader < i32(0LL)))) {
            return {};
        }
        {
            auto range1489 = (((f)->blocks[(l)->preheader]).term).edges;
            for (Int index1490 = 0, rangeEnd1491 = length(range1489); index1490 < rangeEnd1491; ++index1490) {
                Edge e = range1489[index1490];
                if (((e).block == h)) {
                    (l)->entry = e;
                }
            }
        }
        (l)->members[h] = true;
        Slice<i32> work = sliceOf<i32>({(l)->latch});
        {
            for (; (length(work) > Int(0LL));) {
                i32 b = work[(length(work) - Int(1LL))];
                work = subslice(work, Int(0), (length(work) - Int(1LL)));
                if ((l)->members[b]) {
                    continue;
                }
                if (((ci)->header[b] || (!(cfgInfoDominates(ci, h, b))))) {
                    return {};
                }
                (l)->members[b] = true;
                work = appendSlice(work, ((f)->blocks[b]).preds);
            }
        }
        Int insts = Int(0LL);
        bool multipleExits = false;
        {
            auto range1492 = (ci)->order;
            for (Int index1493 = 0, rangeEnd1494 = length(range1492); index1493 < rangeEnd1494; ++index1493) {
                i32 b = range1492[index1493];
                if ((!((l)->members[b]))) {
                    continue;
                }
                (l)->order = appendValues((l)->order, b);
                insts += length(((f)->blocks[b]).instrs);
                {
                    auto range1495 = (((f)->blocks[b]).term).edges;
                    for (Int index1496 = 0, rangeEnd1497 = length(range1495); index1496 < rangeEnd1497; ++index1496) {
                        Edge e = range1495[index1496];
                        if ((!((l)->members[(e).block]))) {
                            if ((((l)->exit >= i32(0LL)) && ((l)->exit != (e).block))) {
                                multipleExits = true;
                            } else {
                                (l)->exit = (e).block;
                            }
                        }
                    }
                }
            }
        }
        if (((((l)->exit < i32(0LL)) || (length((l)->order) > Int(48LL))) || (insts > Int(400LL)))) {
            return {};
        }
        bool outsidePred = false;
        {
            auto range1498 = ((f)->blocks[(l)->exit]).preds;
            for (Int index1499 = 0, rangeEnd1500 = length(range1498); index1499 < rangeEnd1500; ++index1499) {
                i32 p = range1498[index1499];
                outsidePred = (outsidePred || (!((l)->members[p])));
            }
        }
        Term* t = (&(((f)->blocks[(l)->latch]).term));
        if (((t)->kind != TermIf)) {
            return {};
        }
        Value* condition = (&((f)->values[(t)->value]));
        Span<i32> args = functionArgs(f, (t)->value);
        i32 next = {};
        i64 unsignedStop = i64(-1LL);
        bool zeroTest = false;
        if ((((condition)->op == OpEqz) && (((t)->edges[Int(1LL)]).block == h))) {
            next = args[Int(0LL)];
            zeroTest = true;
        } else if ((((((condition)->op == OpNe) && (((t)->edges[Int(0LL)]).block == h)) && (((f)->values[args[Int(1LL)]]).op == OpConst)) && (((f)->values[args[Int(1LL)]]).aux == i64(0LL)))) {
            next = args[Int(0LL)];
            zeroTest = true;
        } else if ((((((condition)->op == OpGtU) && (((t)->edges[Int(0LL)]).block == h)) && (((f)->values[args[Int(1LL)]]).op == OpConst)) && (convert<u32>(((f)->values[args[Int(1LL)]]).aux) < u32(65536LL)))) {
            next = args[Int(0LL)];
            unsignedStop = convert<i64>(convert<u32>(((f)->values[args[Int(1LL)]]).aux));
        } else if ((((((condition)->op == OpGtS) && (((t)->edges[Int(0LL)]).block == h)) && (((f)->values[args[Int(1LL)]]).op == OpConst)) && (((f)->values[args[Int(1LL)]]).aux == i64(0LL)))) {
            next = args[Int(0LL)];
            (l)->countSigned = true;
        } else if (((((t)->edges[Int(0LL)]).block == h) && ((((condition)->op == OpAdd) || ((condition)->op == OpSub))))) {
            next = (t)->value;
            zeroTest = true;
        } else if (((((t)->edges[Int(0LL)]).block == h) && ((((condition)->op == OpLtS) || ((condition)->op == OpLtU))))) {
            next = args[Int(0LL)];
            (l)->limit = args[Int(1LL)];
            (l)->signedLimit = ((condition)->op == OpLtS);
        } else {
            return {};
        }
        Edge back = {};
        {
            auto range1501 = (t)->edges;
            for (Int index1502 = 0, rangeEnd1503 = length(range1501); index1502 < rangeEnd1503; ++index1502) {
                Edge e = range1501[index1502];
                if (((e).block == h)) {
                    back = e;
                }
            }
        }
        (l)->count = i32(-1LL);
        {
            auto range1504 = ((f)->blocks[h]).params;
            for (Int index1505 = 0, rangeEnd1506 = length(range1504); index1505 < rangeEnd1506; ++index1505) {
                Int j = index1505;
                i32 p = range1504[index1505];
                auto temporary1507 = next;
                auto temporary1508 = i32(-1LL);
                i32 candidate = temporary1507;
                i32 bias = temporary1508;
                if ((((unsignedStop >= i64(0LL)) && ((back).args[j] != next)) && (((f)->values[next]).op == OpAdd))) {
                    Span<i32> a = functionArgs(f, next);
                    {
                        Int k = Int(0LL);
                        for (; (k < Int(2LL)); k++) {
                            i32 other = ((f)->values[a[(Int(1LL) - k)]]).block;
                            if (((a[k] == (back).args[j]) && (((other < i32(0LL)) || ((!((l)->members[other])) && cfgInfoDominates(ci, other, h)))))) {
                                auto temporary1509 = a[k];
                                auto temporary1510 = a[(Int(1LL) - k)];
                                candidate = temporary1509;
                                bias = temporary1510;
                            }
                        }
                    }
                }
                if (((back).args[j] != candidate)) {
                    continue;
                }
                i64 step = inductionStep(f, p, candidate);
                if ((((l)->limit >= i32(0LL)) && (step == i64(1LL)))) {
                    (l)->start = ((l)->entry).args[j];
                } else if (((((l)->limit < i32(0LL)) && (step < i64(0LL))) && (step >= i64(-65536LL)))) {
                    u64 n = convert<u64>((-(step)));
                    auto temporary1511 = u64(0LL);
                    auto temporary1512 = u64(0LL);
                    u64 stop = temporary1511;
                    u64 mask = temporary1512;
                    if (((unsignedStop >= i64(0LL)) && ((convert<u64>(unsignedStop) + u64(1LL)) >= n))) {
                        stop = convert<u64>(unsignedStop);
                    } else if ((zeroTest && ((n & ((n - u64(1LL)))) == u64(0LL)))) {
                        auto temporary1513 = (n - u64(1LL));
                        auto temporary1514 = (n - u64(1LL));
                        stop = temporary1513;
                        mask = temporary1514;
                    } else if (((step != i64(-1LL)) || (unsignedStop >= i64(0LL)))) {
                        continue;
                    }
                    (l)->count = ((l)->entry).args[j];
                    auto temporary1515 = bias;
                    auto temporary1516 = n;
                    auto temporary1517 = stop;
                    auto temporary1518 = mask;
                    (l)->countBias = temporary1515;
                    (l)->countStep = temporary1516;
                    (l)->countStop = temporary1517;
                    (l)->countMask = temporary1518;
                    if (((step == i64(-1LL)) && (bias < i32(0LL)))) {
                        setMap((l)->induction, p, LoopSpan{i32(-1LL), {}, u64(1LL), u64(1LL), {}, {}, {}, {}, true});
                    }
                }
            }
        }
        if ((((l)->count < i32(0LL)) && ((l)->start < i32(0LL)))) {
            return {};
        }
        {
            auto range1519 = ((f)->blocks[h]).params;
            for (Int index1520 = 0, rangeEnd1521 = length(range1519); index1520 < rangeEnd1521; ++index1520) {
                Int j = index1520;
                i32 p = range1519[index1520];
                i64 step = inductionStep(f, p, (back).args[j]);
                if (((step > i64(0LL)) && (step <= i64(65536LL)))) {
                    setMap((l)->induction, p, LoopSpan{((l)->entry).args[j], u64(1LL), convert<u64>(step), {}, {}, {}, {}, {}, true});
                }
            }
        }
        // Only countable loops need the whole-function live-out scan.
        if ((multipleExits || outsidePred) && memoryLoopHasLiveOut(l)) {
            return {};
        }
        (l)->unsignedValue = activePool->make<UnsignedRanges>(UnsignedRanges{f, makeSlice<UnsignedRange>(length((f)->values)), makeSlice<bool>(length((f)->values))});
        return l;
    }

    // firstBranch returns the index of the branch sequence ending a block.
    static Int firstBranch(Span<MInst> insts) {
        Int i = length(insts);
        {
            for (; (i > Int(0LL));) {
                {
                    auto switch1522 = (insts[(i - Int(1LL))]).op;
                    switch (switch1522) {
                        case mJmp:
                        case mJcc:
                        case mSwitch:
                        case mRet:
                        case mTrap: {
                            i--;
                            continue;
                            break;
                        }
                    }
                }
                break;
            }
        }
        return i;
    }

    static bool flagCondition(Op op) {
        return ((((isCompare(op) || (op == OpAnd)) || (op == OpOr)) || (op == OpXor)) || (op == OpSub));
    }

    static Tuple2<double, bool> floatConstant(Function* f, i32 v) {
        Value* val = (&((f)->values[v]));
        if ((((val)->op != OpConst) || (!(isFloat((val)->typ))))) {
            return {double(0.0), false};
        }
        if (((val)->typ == F32)) {
            return {convert<double>(mathFloat32frombits(convert<u32>((val)->aux))), true};
        }
        return {mathFloat64frombits(convert<u64>((val)->aux)), true};
    }

    // foldInt evaluates a pure integer operation on constants.
    static Tuple2<u64, bool> foldInt(Op op, ValType t, u64 a, u64 b) {
        Uint w = Uint(64LL);
        if ((t == I32)) {
            w = Uint(32LL);
        }
        u64 m = mask(t);
        a &= m;
        b &= m;
        i64 sa = shiftRight(convert<i64>(shiftLeft(a, ((Uint(64LL) - w)))), ((Uint(64LL) - w)));
        i64 sb = shiftRight(convert<i64>(shiftLeft(b, ((Uint(64LL) - w)))), ((Uint(64LL) - w)));
        u64 sh = (b & convert<u64>((w - Uint(1LL))));
        auto boolean = [&](bool c) mutable -> Tuple2<u64, bool> {
            if (c) {
                return {u64(1LL), true};
            }
            return {u64(0LL), true};
        };
        {
            auto switch1523 = op;
            switch (switch1523) {
                case OpAdd: {
                    return {(((a + b)) & m), true};
                    break;
                }
                case OpSub: {
                    return {(((a - b)) & m), true};
                    break;
                }
                case OpMul: {
                    return {(((a * b)) & m), true};
                    break;
                }
                case OpAnd: {
                    return {(a & b), true};
                    break;
                }
                case OpOr: {
                    return {(a | b), true};
                    break;
                }
                case OpXor: {
                    return {(a ^ b), true};
                    break;
                }
                case OpShl: {
                    return {((shiftLeft(a, sh)) & m), true};
                    break;
                }
                case OpShrU: {
                    return {shiftRight(a, sh), true};
                    break;
                }
                case OpShrS: {
                    return {(convert<u64>(shiftRight(sa, sh)) & m), true};
                    break;
                }
                case OpRotl: {
                    if ((w == Uint(32LL))) {
                        return {convert<u64>(bitsRotateLeft32(convert<u32>(a), convert<i64>(sh))), true};
                    }
                    return {bitsRotateLeft64(a, convert<i64>(sh)), true};
                    break;
                }
                case OpRotr: {
                    if ((w == Uint(32LL))) {
                        return {convert<u64>(bitsRotateLeft32(convert<u32>(a), (-(convert<i64>(sh))))), true};
                    }
                    return {bitsRotateLeft64(a, (-(convert<i64>(sh)))), true};
                    break;
                }
                case OpDivU: {
                    if ((b == u64(0LL))) {
                        return {u64(0LL), false};
                    }
                    return {(a / b), true};
                    break;
                }
                case OpRemU: {
                    if ((b == u64(0LL))) {
                        return {u64(0LL), false};
                    }
                    return {(a % b), true};
                    break;
                }
                case OpDivS: {
                    if (((b == u64(0LL)) || (((sb == i64(-1LL)) && (a == shiftLeft(u64(1LL), ((w - Uint(1LL))))))))) {
                        return {u64(0LL), false};
                    }
                    return {(convert<u64>((sa / sb)) & m), true};
                    break;
                }
                case OpRemS: {
                    if ((b == u64(0LL))) {
                        return {u64(0LL), false};
                    }
                    if ((sb == i64(-1LL))) {
                        return {u64(0LL), true};
                    }
                    return {(convert<u64>((sa % sb)) & m), true};
                    break;
                }
                case OpEq: {
                    return boolean((a == b));
                    break;
                }
                case OpNe: {
                    return boolean((a != b));
                    break;
                }
                case OpLtS: {
                    return boolean((sa < sb));
                    break;
                }
                case OpLtU: {
                    return boolean((a < b));
                    break;
                }
                case OpGtS: {
                    return boolean((sa > sb));
                    break;
                }
                case OpGtU: {
                    return boolean((a > b));
                    break;
                }
                case OpLeS: {
                    return boolean((sa <= sb));
                    break;
                }
                case OpLeU: {
                    return boolean((a <= b));
                    break;
                }
                case OpGeS: {
                    return boolean((sa >= sb));
                    break;
                }
                case OpGeU: {
                    return boolean((a >= b));
                    break;
                }
            }
        }
        return {u64(0LL), false};
    }

    static Tuple2<u64, bool> foldUnary(Op op, ValType t, ValType from, u64 a) {
        u64 m = mask(t);
        {
            auto switch1524 = op;
            switch (switch1524) {
                case OpEqz: {
                    if (((a & mask(from)) == u64(0LL))) {
                        return {u64(1LL), true};
                    }
                    return {u64(0LL), true};
                    break;
                }
                case OpClz: {
                    if ((t == I32)) {
                        return {convert<u64>(bitsLeadingZeros32(convert<u32>(a))), true};
                    }
                    return {convert<u64>(bitsLeadingZeros64(a)), true};
                    break;
                }
                case OpCtz: {
                    if ((t == I32)) {
                        return {convert<u64>(bitsTrailingZeros32(convert<u32>(a))), true};
                    }
                    return {convert<u64>(bitsTrailingZeros64(a)), true};
                    break;
                }
                case OpPopcnt: {
                    return {convert<u64>(bitsOnesCount64((a & m))), true};
                    break;
                }
                case OpExtend8S: {
                    return {(convert<u64>(convert<i64>(convert<i8>(a))) & m), true};
                    break;
                }
                case OpExtend16S: {
                    return {(convert<u64>(convert<i64>(convert<i16>(a))) & m), true};
                    break;
                }
                case OpExtend32S: {
                    return {(convert<u64>(convert<i64>(convert<i32>(a))) & m), true};
                    break;
                }
                case OpWrap: {
                    return {(a & u64(4294967295LL)), true};
                    break;
                }
                case OpExtendU: {
                    return {(a & u64(4294967295LL)), true};
                    break;
                }
                case OpExtendS: {
                    return {convert<u64>(convert<i64>(convert<i32>(a))), true};
                    break;
                }
            }
        }
        return {u64(0LL), false};
    }

    static bool globalMutability(Reader* r) {
        u8 b = readerByte(r);
        if ((b > u8(1LL))) {
            compilerError(String("malformed mutability 0x%02x"));
        }
        return (b == u8(1LL));
    }

    static GoFrame goAssign(Slice<ValType> params, Slice<ValType> results) {
        GoFrame f = {};
        i32 off = i32(0LL);
        auto assign = [&](Slice<ValType> types, Int ints) mutable -> Slice<GoLoc> {
            Int floats = Int(0LL);
            Slice<GoLoc> locs = makeSlice<GoLoc>(length(types));
            {
                auto range1525 = types;
                for (Int index1526 = 0, rangeEnd1527 = length(range1525); index1526 < rangeEnd1527; ++index1526) {
                    Int i = index1526;
                    ValType t = range1525[index1526];
                    {
                        if ((isFloat(t) && (floats < length(goFloatRegs)))) {
                            locs[i] = GoLoc{goFloatRegs[floats], {}};
                            floats++;
                        } else if (((!(isFloat(t))) && (ints < length(goIntRegs)))) {
                            locs[i] = GoLoc{goIntRegs[ints], {}};
                            ints++;
                        } else {
                            off = alignTo(off, goSize(t));
                            locs[i] = GoLoc{noReg, off};
                            off += goSize(t);
                        }
                    switchEnd1529:;
                    }
                }
            }
            off = alignTo(off, i32(8LL));
            return locs;
        };
        (f).params = assign(params, Int(1LL));
        (f).results = assign(results, Int(0LL));
        off += i32(8LL);
        {
            auto range1530 = (f).params;
            for (Int index1531 = 0, rangeEnd1532 = length(range1530); index1531 < rangeEnd1532; ++index1531) {
                Int i = index1531;
                GoLoc l = range1530[index1531];
                if (((l).reg != noReg)) {
                    off = (alignTo(off, goSize(params[i])) + goSize(params[i]));
                }
            }
        }
        (f).size = alignTo(off, i32(8LL));
        return f;
    }

    static i32 goSize(ValType t) {
        if (((t == I32) || (t == F32))) {
            return i32(4LL);
        }
        return i32(8LL);
    }

    // hasEffect reports whether a value must stay even when nothing uses it:
    // it writes state, or it may trap.
    static bool hasEffect(Op op) {
        {
            auto switch1533 = op;
            switch (switch1533) {
                case OpDivS:
                case OpDivU:
                case OpRemS:
                case OpRemU:
                case OpTruncS:
                case OpTruncU:
                case OpLoad:
                case OpStore:
                case OpMemGrow:
                case OpMemCopy:
                case OpMemFill:
                case OpMemInit:
                case OpDataDrop:
                case OpCopyForward:
                case OpGlobalSet:
                case OpCall:
                case OpCallIndirect:
                case OpTableGet:
                case OpTableSet:
                case OpTableGrow:
                case OpTableFill:
                case OpTableCopy:
                case OpTableInit:
                case OpElemDrop: {
                    return true;
                    break;
                }
            }
        }
        return false;
    }

    template <typename Data>
    static void heapSort_func(Data data, Int a, Int b) {
        Int first = a;
        Int lo = Int(0LL);
        Int hi = (b - a);
        {
            Int i = (((hi - Int(1LL))) / Int(2LL));
            for (; (i >= Int(0LL)); i--) {
                siftDown_func(data, i, hi, first);
            }
        }
        {
            Int i = (hi - Int(1LL));
            for (; (i >= Int(0LL)); i--) {
                (data).Swap(first, (first + i));
                siftDown_func(data, lo, i, first);
            }
        }
    }

    static u8 hi(Int r) {
        return u8(convert<u8>(shiftRight(r, Int(3LL))) & u8(1LL));
    }

    static String importSymbol(String module, String name, FuncType* t) {
        return (((((((symImport + symbolPart(module)) + String(".")) + symbolPart(name)) + String(".")) + typesPart((t)->params)) + String(".")) + typesPart((t)->results));
    }

    // indexed is [base+index].
    static Amem indexed(VReg base, VReg index) {
        return Amem{base, index, u8(1LL), {}, i32(-1LL), {}};
    }

    static i64 inductionStep(Function* f, i32 phi, i32 next) {
        Value* val = (&((f)->values[next]));
        Span<i32> args = functionArgs(f, next);
        if ((((val)->typ != I32) || (((val)->op != OpAdd) && ((val)->op != OpSub)))) {
            return i64(0LL);
        }
        auto temporary1534 = args[Int(0LL)];
        auto temporary1535 = args[Int(1LL)];
        i32 a = temporary1534;
        i32 b = temporary1535;
        if ((((val)->op == OpAdd) && (b == phi))) {
            auto temporary1536 = b;
            auto temporary1537 = a;
            a = temporary1536;
            b = temporary1537;
        }
        if (((a != phi) || (((f)->values[b]).op != OpConst))) {
            return i64(0LL);
        }
        i64 step = convert<i64>(convert<i32>(((f)->values[b]).aux));
        if (((val)->op == OpSub)) {
            step = (-(step));
        }
        return step;
    }

    // Unrolled loops may advance a pointer by adding the same stride several
    // times. Keep those increments as a factored stride, without changing the
    // wrapping i32 arithmetic of the original loop.
    static Tuple2<Slice<i32>, bool> inductionTerms(Function* f, i32 v, i32 phi, Int depth) {
        if ((v == phi)) {
            return {{}, true};
        }
        if ((((depth > Int(16LL)) || (((f)->values[v]).op != OpAdd)) || (((f)->values[v]).typ != I32))) {
            return {{}, false};
        }
        Span<i32> args = functionArgs(f, v);
        {
            Int j = Int(0LL);
            for (; (j < Int(2LL)); j++) {
                {
                    auto temporary1538 = inductionTerms(f, args[j], phi, (depth + Int(1LL)));
                    Slice<i32> terms = temporary1538.v0;
                    bool ok = temporary1538.v1;
                    if (ok) {
                        return {appendValues(terms, args[(Int(1LL) - j)]), true};
                    }
                }
            }
        }
        return {{}, false};
    }

    // insertionSort_func sorts data[a:b] using insertion sort.
    template <typename Data>
    static void insertionSort_func(Data data, Int a, Int b) {
        {
            Int i = (a + Int(1LL));
            for (; (i < b); i++) {
                {
                    Int j = i;
                    for (; ((j > a) && (data).Less(j, (j - Int(1LL)))); j--) {
                        (data).Swap(j, (j - Int(1LL)));
                    }
                }
            }
        }
    }

    static MInst inst(MOp op) {
        return MInst{op, {}, {}, {}, {}, noReg, noReg, noReg, {}, Mem{noReg, noReg, {}, {}, i32(-1LL)}, i32(-1LL), nullptr};
    }

    // internalLocs gives the internal convention's places of values: a
    // register, or noReg and the index of a stack slot, counted from first.
    static Tuple3<Slice<VReg>, Slice<Int>, Int> internalLocs(Slice<ValType> types, Slice<VReg> gprs, Slice<VReg> xmms, Int first) {
        auto temporary1539 = Int(0LL);
        auto temporary1540 = Int(0LL);
        auto temporary1541 = first;
        Int gi = temporary1539;
        Int xi = temporary1540;
        Int slot = temporary1541;
        Slice<VReg> regs = makeSlice<VReg>(length(types));
        Slice<Int> slots = makeSlice<Int>(length(types));
        {
            auto range1542 = types;
            for (Int index1543 = 0, rangeEnd1544 = length(range1542); index1543 < rangeEnd1544; ++index1543) {
                Int i = index1543;
                ValType t = range1542[index1543];
                regs[i] = noReg;
                {
                    if ((isFloat(t) && (xi < length(xmms)))) {
                        regs[i] = xmms[xi];
                        xi++;
                    } else if (((!(isFloat(t))) && (gi < length(gprs)))) {
                        regs[i] = gprs[gi];
                        gi++;
                    } else {
                        slots[i] = slot;
                        slot++;
                    }
                switchEnd1546:;
                }
            }
        }
        return {regs, slots, slot};
    }

    static bool isCompare(Op op) {
        return ((((op >= OpEqz) && (op <= OpGeU))) || (((op >= OpFEq) && (op <= OpFGe))));
    }

    static bool isFloat(ValType t) {
        return ((t == F32) || (t == F64));
    }

    static bool isIntOp(Op op) {
        return ((op >= OpAdd) && (op <= OpGeU));
    }

    static bool isSlot(VReg r) {
        return (r < noReg);
    }

    static u8 lo3(Int r) {
        return convert<u8>((r & Int(7LL)));
    }

    static MFunc* lowerFunction(Module* m, Layout* lay, Function* f) {
        Lowerer* l = activePool->make<Lowerer>(Lowerer{m, f, lay, {}, activePool->make<MFunc>(MFunc{}), {}, {}, {}, {}, {}, mapOf<Array<i32, 2>, i32>({}), mapOf<i32, i64>({}), {}, {}, {}, {}});
        (l)->ci = functionAnalyze(f);
        l->vregs.resize(f->values.size());
        l->vregs.fill(noReg);
        lowererCountUses(l);
        lowererMarkDeferred(l);
        lowererPlaceFloatConstants(l);
        lowererFindRawTruncations(l);
        Slice<Slice<CheckedValue>> proofs = checkedValues(f, (l)->ci);
        l->mblk.resize(f->blocks.size());
        l->mblk.fill(-1);
        {
            auto range1553 = ((l)->ci)->order;
            for (Int index1554 = 0, rangeEnd1555 = length(range1553); index1554 < rangeEnd1555; ++index1554) {
                i32 b = range1553[index1554];
                (l)->mblk[b] = convert<i32>(length(((l)->mf)->blocks));
                ((l)->mf)->blocks.pushBack(MBlock{{}, {}, {}, ((l)->ci)->depth[b], b, {}});
            }
        }
        {
            auto range1556 = ((l)->ci)->order;
            for (Int index1557 = 0, rangeEnd1558 = length(range1556); index1557 < rangeEnd1558; ++index1557) {
                i32 b = range1556[index1557];
                {
                    auto range1559 = ((f)->blocks[b]).params;
                    for (Int index1560 = 0, rangeEnd1561 = length(range1559); index1560 < rangeEnd1561; ++index1560) {
                        i32 p = range1559[index1560];
                        lowererDef(l, p);
                    }
                }
            }
        }
        auto children = cfgInfoChildren(l->ci);
        Vec<LowerVisit> visits;
        visits.pushBack({l->ci->order[0], false, 0});
        {
            for (; (length(visits) > Int(0LL));) {
                const LowerVisit visit = visits.popBack();
                if ((visit).leave) {
                    {
                        for (; (length((l)->checkUndo) > (visit).undo);) {
                            CheckUndo u = (l)->checkUndo[(length((l)->checkUndo) - Int(1LL))];
                            (l)->checkUndo = subslice((l)->checkUndo, Int(0), (length((l)->checkUndo) - Int(1LL)));
                            if ((u).set) {
                                setMap((l)->checked, (u).value, (u).end);
                            } else {
                                eraseMap((l)->checked, (u).value);
                            }
                        }
                    }
                    continue;
                }
                i32 b = (visit).block;
                Int undo = length((l)->checkUndo);
                (l)->cur = (l)->mblk[b];
                if ((!isNil(proofs))) {
                    {
                        auto range1562 = proofs[b];
                        for (Int index1563 = 0, rangeEnd1564 = length(range1562); index1563 < rangeEnd1564; ++index1563) {
                            CheckedValue proof = range1562[index1563];
                            auto temporary1565 = lookupMap((l)->checked, (proof).value);
                            i64 prior = temporary1565.v0;
                            bool had = temporary1565.v1;
                            if (((!(had)) || (prior < (proof).end))) {
                                (l)->checkUndo = appendValues((l)->checkUndo, CheckUndo{(proof).value, prior, had});
                                setMap((l)->checked, (proof).value, (proof).end);
                            }
                        }
                    }
                }
                if ((b == ((l)->ci)->order[Int(0LL)])) {
                    lowererEntry(l);
                }
                {
                    auto range1566 = (l)->floatHomes[b];
                    for (Int index1567 = 0, rangeEnd1568 = length(range1566); index1567 < rangeEnd1568; ++index1567) {
                        i32 v = range1566[index1567];
                        Value* val = (&((f)->values[v]));
                        MInst in = inst(mFConst);
                        (in).dst = lowererDef(l, v);
                        (in).imm = (val)->aux;
                        (in).size = typeSize((val)->typ);
                        lowererEmit(l, in);
                    }
                }
                {
                    auto range1569 = ((f)->blocks[b]).instrs.view();
                    for (Int index1570 = 0, rangeEnd1571 = length(range1569); index1570 < rangeEnd1571; ++index1570) {
                        i32 v = range1569[index1570];
                        if ((!((l)->deferred[v]))) {
                            lowererLowerValue(l, v);
                        }
                    }
                }
                lowererTerminator(l, b);
                visits.pushBack({b, true, undo});
                {
                    for (i32 child = children.first[usize(b)]; child >= 0; child = children.next[usize(child)]) {
                        visits.pushBack({child, false, 0});
                    }
                }
            }
        }
        lowererLinkBlocks(l);
        if ((m)->tailRanges) {
            lowererVersionTailRanges(l);
        }
        return (l)->mf;
    }

    static MemKind makeMemKind(Int width, bool signedValue) {
        MemKind k = convert<MemKind>(width);
        if (signedValue) {
            k |= memSigned;
        }
        return k;
    }

    static u64 mask(ValType t) {
        if ((t == I32)) {
            return u64(4294967295LL);
        }
        return u64(18446744073709551615ULL);
    }

    static i64 maxSigned(u8 size) {
        if ((size == u8(8LL))) {
            return i64(9223372036854775807LL);
        }
        return i64(2147483647LL);
    }

    // medianAdjacent_func finds the median of data[a - 1], data[a], data[a + 1] and stores the index into a.
    template <typename Data>
    static Int medianAdjacent_func(Data data, Int a, Int* swaps) {
        return median_func(data, (a - Int(1LL)), a, (a + Int(1LL)), swaps);
    }

    // median_func returns x where data[x] is the median of data[a],data[b],data[c], where x is a, b, or c.
    template <typename Data>
    static Int median_func(Data data, Int a, Int b, Int c, Int* swaps) {
        auto temporary1572 = order2_func(data, a, b, swaps);
        a = temporary1572.v0;
        b = temporary1572.v1;
        auto temporary1573 = order2_func(data, b, c, swaps);
        b = temporary1573.v0;
        c = temporary1573.v1;
        auto temporary1574 = order2_func(data, a, b, swaps);
        a = temporary1574.v0;
        b = temporary1574.v1;
        return b;
    }

    static FloatRange meetFloatRange(FloatRange a, FloatRange b) {
        return FloatRange{maximum((a).lo, (b).lo), minimum((a).hi, (b).hi), ((a).nan && (b).nan)};
    }

    static Mem memAt(VReg base, i32 disp) {
        return Mem{base, noReg, u8(1LL), disp, i32(-1LL)};
    }

    static Int memB(Amem m) {
        if (((m).label >= i32(0LL))) {
            return Int(-1LL);
        }
        return (m).base;
    }

    static Mem memIndexed(VReg base, VReg index, i32 disp) {
        return Mem{base, index, u8(1LL), disp, i32(-1LL)};
    }

    static Amem memOf(Mem m) {
        Amem am = Amem{Int(-1LL), Int(-1LL), (m).scale, (m).disp, i32(-1LL), {}};
        if (((m).label >= i32(0LL))) {
            (am).label = (m).label;
            (am).labelKind = labelModule;
            return am;
        }
        if (((m).base != noReg)) {
            (am).base = regNum((m).base);
        }
        if (((m).index != noReg)) {
            (am).index = regNum((m).index);
        }
        if (((am).scale == u8(0LL))) {
            (am).scale = u8(1LL);
        }
        return am;
    }

    static Int memX(Amem m) {
        if (((m).label >= i32(0LL))) {
            return Int(-1LL);
        }
        return (m).index;
    }

    // Merge two disjoint normalized sets. Their buffers are temporary coalescing
    // state: y is consumed, while x keeps the same canonical register ID.
    static void mergeRanges(InlineVec<Lrange, 1>& x, InlineVec<Lrange, 1>& y, InlineVec<Lrange, 1>& scratch) {
        if (!y.size()) {
            return;
        }
        if (!x.size()) {
            x = move(y);
            return;
        }
        const usize nx = x.size();
        const usize ny = y.size();
        if (x[nx - 1].to <= y[0].from) {
            usize first = 0;
            if (x[nx - 1].to == y[0].from) {
                x[nx - 1].to = y[0].to;
                first = 1;
            }
            x.append(subslice(y.view(), Int(first), Int(ny)));
            y = InlineVec<Lrange, 1>{};
            return;
        }
        if (y[ny - 1].to <= x[0].from) {
            usize first = 0;
            if (y[ny - 1].to == x[0].from) {
                y[ny - 1].to = x[0].to;
                first = 1;
            }
            y.append(subslice(x.view(), Int(first), Int(nx)));
            x = move(y);
            return;
        }
        scratch.clear();
        scratch.reserve(nx + ny);
        const auto left = x.view();
        const auto right = y.view();
        usize i = 0;
        usize j = 0;
        while (i < nx || j < ny) {
            const Lrange range = j == ny || (i < nx && left[Int(i)].from < right[Int(j)].from) ? left[Int(i++)] : right[Int(j++)];
            const usize count = scratch.size();
            if (count && scratch[count - 1].to == range.from) {
                scratch[count - 1].to = range.to;
            } else {
                scratch.pushBack(range);
            }
        }
        InlineVec<Lrange, 1> previous(move(x));
        x = move(scratch);
        scratch = move(previous);
        y = InlineVec<Lrange, 1>{};
    }

    static Bitset newBitset(Int n) {
        require(n >= 0, "bitset size");
        if (u32(n) <= denseBitsetLimit) {
            auto* data = activePool->make<BitsetData>();
            data->pool = activePool;
            data->bits = u32(n);
            return {data};
        }
        auto* backing = activePool->make<SparseBitsetStorage>();
        backing->data.pool = activePool;
        backing->data.bits = u32(n);
        backing->nodes.resize((usize(n) + 16383) / 16384);
        backing->nodes.visit([](BitNode*& node) {
            node = nullptr;
        });
        backing->data.storage = backing->nodes.data();
        return {&backing->data};
    }

    static Layout* newLayout(Module* m) {
        Layout* l = activePool->make<Layout>(Layout{offMemSize, {}, {}, {}, {}, {}, {}});
        i32 off = i32(120LL);
        if (((m)->memoryTail != u64(0LL))) {
            (l)->memoryEnd = off;
            off += i32(8LL);
        }
        {
            auto range1577 = (m)->globals;
            for (Int index1578 = 0, rangeEnd1579 = length(range1577); index1578 < rangeEnd1579; ++index1578) {
                Global g = range1577[index1578];
                if (((g).typ == V128)) {
                    off = (((off + i32(15LL))) & ~(i32(15LL)));
                }
                (l)->globals = appendValues((l)->globals, off);
                off += i32(8LL);
                if ((((g).typ == V128) && (!((g).imported)))) {
                    off += i32(8LL);
                }
            }
        }
        off = (((off + i32(15LL))) & ~(i32(15LL)));
        {
            auto range1580 = (m)->tables;
            for (Int index1581 = 0, rangeEnd1582 = length(range1580); index1581 < rangeEnd1582; ++index1581) {
                (l)->tables = appendValues((l)->tables, off);
                off += tableBytes;
            }
        }
        (l)->descs = off;
        off += (convert<i32>(length((m)->funcs)) * descBytes);
        (l)->dropped = off;
        off += convert<i32>((length((m)->datas) + length((m)->elems)));
        if ((m)->tailProfile) {
            off = (((off + i32(7LL))) & ~(i32(7LL)));
            (l)->tailProfile = off;
            off += (convert<i32>(length((m)->funcs)) * tailCounterBytes);
        }
        (l)->size = (((off + i32(15LL))) & ~(i32(15LL)));
        return l;
    }

    static Uint nextPowerOfTwo(Int lengthValue) {
        Uint shift = convert<Uint>(bitsLen(convert<Uint>(lengthValue)));
        return convert<Uint>(shiftLeft(Uint(1LL), shift));
    }

    static void optimize(Function* f) {
        Optimizer* o = activePool->make<Optimizer>(Optimizer{f, {}, mapOf<ConstKey, i32>({}), {}});
        {
            auto& range1583 = (f)->values;
            for (Int index1584 = 0, rangeEnd1585 = length(range1583); index1584 < rangeEnd1585; ++index1584) {
                Int v = index1584;
                Value* val = (&((f)->values[v]));
                if ((((val)->op == OpConst) && ((val)->block < i32(0LL)))) {
                    ConstKey key = ConstKey{(val)->typ, (val)->aux};
                    {
                        auto temporary1586 = lookupMap((o)->consts, key);
                        bool ok = temporary1586.v1;
                        if ((!(ok))) {
                            setMap((o)->consts, key, convert<i32>(v));
                        }
                    }
                }
            }
        }
        functionComputePreds(f);
        {
            Int round = Int(0LL);
            for (; (round < Int(8LL)); round++) {
                bool changed = optimizerSimplify(o);
                changed = (optimizerThread(o) || changed);
                changed = (optimizerCleanup(o) || changed);
                changed = (optimizerTrivialParams(o) || changed);
                if ((!(changed))) {
                    break;
                }
            }
        }
        optimizerCommonExpressions(o);
        if (optimizerNarrowFloatComparisons(o)) {
            optimizerDeadCode(o);
        }
        optimizerSelectTriangles(o);
        versionMemoryLoops(f);
        {
            Int round = Int(0LL);
            for (; (round < Int(4LL)); round++) {
                bool changed = optimizerCleanup(o);
                changed = (optimizerTrivialParams(o) || changed);
                if ((!(changed))) {
                    break;
                }
            }
        }
        optimizerDeadCode(o);
    }

    // order ranks section ids in their required order; datacount goes before
    // code.
    static Int order(u8 id) {
        {
            auto switch1587 = id;
            switch (switch1587) {
                case u8(12LL): {
                    return Int(10LL);
                    break;
                }
                case u8(10LL):
                case u8(11LL): {
                    return (convert<i64>(id) + Int(1LL));
                    break;
                }
            }
        }
        return convert<i64>(id);
    }

    // order2_func returns x,y where data[x] <= data[y], where x,y=a,b or x,y=b,a.
    template <typename Data>
    static Tuple2<Int, Int> order2_func(Data data, Int a, Int b, Int* swaps) {
        if ((data).Less(b, a)) {
            (*(swaps))++;
            return {b, a};
        }
        return {a, b};
    }

    // overlap tells whether two sorted lists of ranges share a position.
    static bool overlap(Span<Lrange> x, Span<Lrange> y) {
        auto temporary1588 = Int(0LL);
        auto temporary1589 = Int(0LL);
        Int i = temporary1588;
        Int j = temporary1589;
        {
            for (; ((i < length(x)) && (j < length(y)));) {
                {
                    if (((x[i]).to <= (y[j]).from)) {
                        i++;
                    } else if (((y[j]).to <= (x[i]).from)) {
                        j++;
                    } else {
                        return true;
                    }
                switchEnd1591:;
                }
            }
        }
        return false;
    }

    static u8 packedPP(u8 size) {
        if ((size == u8(8LL))) {
            return pp66;
        }
        return ppNone;
    }

    static MInst parMove(Slice<Move> moves) {
        MInst in = inst(mParMove);
        mInstExtra(&in)->moves.assign(moves);
        return in;
    }

    // partialInsertionSort_func partially sorts a slice, returns true if the slice is sorted at the end.
    template <typename Data>
    static bool partialInsertionSort_func(Data data, Int a, Int b) {
        constexpr Int maxSteps = Int(5LL);
        constexpr Int shortestShifting = Int(50LL);
        Int i = (a + Int(1LL));
        {
            Int j = Int(0LL);
            for (; (j < Int(5LL)); j++) {
                {
                    for (; ((i < b) && (!((data).Less(i, (i - Int(1LL))))));) {
                        i++;
                    }
                }
                if ((i == b)) {
                    return true;
                }
                if (((b - a) < Int(50LL))) {
                    return false;
                }
                (data).Swap(i, (i - Int(1LL)));
                if (((i - a) >= Int(2LL))) {
                    {
                        Int j = (i - Int(1LL));
                        for (; (j >= Int(1LL)); j--) {
                            if ((!((data).Less(j, (j - Int(1LL)))))) {
                                break;
                            }
                            (data).Swap(j, (j - Int(1LL)));
                        }
                    }
                }
                if (((b - i) >= Int(2LL))) {
                    {
                        Int j = (i + Int(1LL));
                        for (; (j < b); j++) {
                            if ((!((data).Less(j, (j - Int(1LL)))))) {
                                break;
                            }
                            (data).Swap(j, (j - Int(1LL)));
                        }
                    }
                }
            }
        }
        return false;
    }

    // partitionEqual_func partitions data[a:b] into elements equal to data[pivot] followed by elements greater than data[pivot].
    // It assumed that data[a:b] does not contain elements smaller than the data[pivot].
    template <typename Data>
    static Int partitionEqual_func(Data data, Int a, Int b, Int pivot) {
        Int newpivot{};
        (data).Swap(a, pivot);
        auto temporary1592 = (a + Int(1LL));
        auto temporary1593 = (b - Int(1LL));
        Int i = temporary1592;
        Int j = temporary1593;
        {
            for (; true;) {
                {
                    for (; ((i <= j) && (!((data).Less(a, i))));) {
                        i++;
                    }
                }
                {
                    for (; ((i <= j) && (data).Less(a, j));) {
                        j--;
                    }
                }
                if ((i > j)) {
                    break;
                }
                (data).Swap(i, j);
                i++;
                j--;
            }
        }
        return i;
    }

    // partition_func does one quicksort partition.
    // Let p = data[pivot]
    // Moves elements in data[a:b] around, so that data[i]<p and data[j]>=p for i<newpivot and j>newpivot.
    // On return, data[newpivot] = p
    template <typename Data>
    static Tuple2<Int, bool> partition_func(Data data, Int a, Int b, Int pivot) {
        Int newpivot{};
        bool alreadyPartitioned{};
        (data).Swap(a, pivot);
        auto temporary1594 = (a + Int(1LL));
        auto temporary1595 = (b - Int(1LL));
        Int i = temporary1594;
        Int j = temporary1595;
        {
            for (; ((i <= j) && (data).Less(i, a));) {
                i++;
            }
        }
        {
            for (; ((i <= j) && (!((data).Less(j, a))));) {
                j--;
            }
        }
        if ((i > j)) {
            (data).Swap(j, a);
            return {j, true};
        }
        (data).Swap(i, j);
        i++;
        j--;
        {
            for (; true;) {
                {
                    for (; ((i <= j) && (data).Less(i, a));) {
                        i++;
                    }
                }
                {
                    for (; ((i <= j) && (!((data).Less(j, a))));) {
                        j--;
                    }
                }
                if ((i > j)) {
                    break;
                }
                (data).Swap(i, j);
                i++;
                j--;
            }
        }
        (data).Swap(j, a);
        return {j, false};
    }

    // pdqsort_func sorts data[a:b].
    // The algorithm based on pattern-defeating quicksort(pdqsort), but without the optimizations from BlockQuicksort.
    // pdqsort paper: https://arxiv.org/pdf/2106.05123.pdf
    // C++ implementation: https://github.com/orlp/pdqsort
    // Rust implementation: https://docs.rs/pdqsort/latest/pdqsort/
    // limit is the number of allowed bad (very unbalanced) pivots before falling back to heapsort.
    template <typename Data>
    static void pdqsort_func(Data data, Int a, Int b, Int limit) {
        constexpr Int maxInsertion = Int(12LL);
        bool wasBalanced = true;
        bool wasPartitioned = true;
        {
            for (; true;) {
                Int lengthValue = (b - a);
                if ((lengthValue <= Int(12LL))) {
                    insertionSort_func(data, a, b);
                    return;
                }
                if ((limit == Int(0LL))) {
                    heapSort_func(data, a, b);
                    return;
                }
                if ((!(wasBalanced))) {
                    breakPatterns_func(data, a, b);
                    limit--;
                }
                auto temporary1596 = choosePivot_func(data, a, b);
                Int pivot = temporary1596.v0;
                SortedHint hint = temporary1596.v1;
                if ((hint == decreasingHint)) {
                    reverseRange_func(data, a, b);
                    pivot = (((b - Int(1LL))) - ((pivot - a)));
                    hint = increasingHint;
                }
                if (((wasBalanced && wasPartitioned) && (hint == increasingHint))) {
                    if (partialInsertionSort_func(data, a, b)) {
                        return;
                    }
                }
                if (((a > Int(0LL)) && (!((data).Less((a - Int(1LL)), pivot))))) {
                    Int mid = partitionEqual_func(data, a, b, pivot);
                    a = mid;
                    continue;
                }
                auto temporary1597 = partition_func(data, a, b, pivot);
                Int mid = temporary1597.v0;
                bool alreadyPartitioned = temporary1597.v1;
                wasPartitioned = alreadyPartitioned;
                auto temporary1598 = (mid - a);
                auto temporary1599 = (b - mid);
                Int leftLen = temporary1598;
                Int rightLen = temporary1599;
                Int balanceThreshold = (lengthValue / Int(8LL));
                if ((leftLen < rightLen)) {
                    wasBalanced = (leftLen >= balanceThreshold);
                    pdqsort_func(data, a, mid, limit);
                    a = (mid + Int(1LL));
                } else {
                    wasBalanced = (rightLen >= balanceThreshold);
                    pdqsort_func(data, (mid + Int(1LL)), b, limit);
                    b = mid;
                }
            }
        }
    }

    // readsState reports whether a value depends on mutable state, so two equal
    // instructions may produce different results.
    static bool readsState(Op op) {
        {
            auto switch1600 = op;
            switch (switch1600) {
                case OpLoad:
                case OpMemSize:
                case OpGlobalGet:
                case OpTableGet:
                case OpTableSize:
                case OpExtract:
                case OpParam:
                case OpUndef: {
                    return true;
                    break;
                }
            }
        }
        return hasEffect(op);
    }

    // refFuncExpr is the constant expression ref.func index.
    static Slice<u8> refFuncExpr(u32 index) {
        Slice<u8> expr = sliceOf<u8>({u8(210LL)});
        expr = appendLeb(expr, convert<u64>(index));
        return appendValues(expr, u8(11LL));
    }

    static Int regNum(VReg r) {
        if (vRegIsXMM(r)) {
            return convert<i64>((r - XMM0));
        }
        return convert<i64>(r);
    }

    // resultRegs assigns results to the result registers in order; one that
    // does not fit gets noReg and the next stack result slot, which follows the
    // stack arguments in the caller's outgoing area.
    static Slice<VReg> resultRegs(Slice<ValType> results) {
        auto temporary1601 = Int(0LL);
        auto temporary1602 = Int(0LL);
        Int gi = temporary1601;
        Int xi = temporary1602;
        Slice<VReg> regs = makeSlice<VReg>(length(results));
        {
            auto range1603 = results;
            for (Int index1604 = 0, rangeEnd1605 = length(range1603); index1604 < rangeEnd1605; ++index1604) {
                Int i = index1604;
                ValType t = range1603[index1604];
                regs[i] = noReg;
                {
                    if ((isFloat(t) && (xi < length(resultXMMs)))) {
                        regs[i] = resultXMMs[xi];
                        xi++;
                    } else if (((!(isFloat(t))) && (gi < length(resultGPRs)))) {
                        regs[i] = resultGPRs[gi];
                        gi++;
                    }
                switchEnd1607:;
                }
            }
        }
        return regs;
    }

    // returnsTwice tells the imports that return twice by their names, as C
    // compilers do.
    static bool returnsTwice(String name) {
        {
            auto switch1608 = name;
            if ((switch1608 == String("setjmp")) || (switch1608 == String("_setjmp")) || (switch1608 == String("sigsetjmp")) || (switch1608 == String("vfork")) || (switch1608 == String("fork"))) {
                return true;
            }
        switchEnd1609:;
        }
        return false;
    }

    template <typename Data>
    static void reverseRange_func(Data data, Int a, Int b) {
        Int i = a;
        Int j = (b - Int(1LL));
        {
            for (; (i < j);) {
                (data).Swap(i, j);
                i++;
                j--;
            }
        }
    }

    // rotate_func rotates two consecutive blocks u = data[a:m] and v = data[m:b] in data:
    // Data of the form 'x u v y' is changed to 'x v u y'.
    // rotate performs at most b-a many calls to data.Swap,
    // and it assumes non-degenerate arguments: a < m && m < b.
    template <typename Data>
    static void rotate_func(Data data, Int a, Int m, Int b) {
        Int i = (m - a);
        Int j = (b - m);
        {
            for (; (i != j);) {
                if ((i > j)) {
                    swapRange_func(data, (m - i), m, j);
                    i -= j;
                } else {
                    swapRange_func(data, (m - i), ((m + j) - i), i);
                    j -= i;
                }
            }
        }
        swapRange_func(data, (m - i), m, i);
    }

    static Amem rspAt(i32 disp) {
        return Amem{Int(4LL), Int(-1LL), u8(1LL), disp, i32(-1LL), {}};
    }

    static bool sameArgs(Optimizer* o, Slice<i32> x, Slice<i32> y) {
        if ((length(x) != length(y))) {
            return false;
        }
        {
            auto range1610 = x;
            for (Int index1611 = 0, rangeEnd1612 = length(range1610); index1611 < rangeEnd1612; ++index1611) {
                Int i = index1611;
                if ((optimizerResolve(o, x[i]) != optimizerResolve(o, y[i]))) {
                    return false;
                }
            }
        }
        return true;
    }

    static u8 scalarPP(u8 size) {
        if ((size == u8(8LL))) {
            return ppF2;
        }
        return ppF3;
    }

    static u8 scaleBits(u8 s) {
        {
            auto switch1613 = s;
            switch (switch1613) {
                case u8(2LL): {
                    return u8(1LL);
                    break;
                }
                case u8(4LL): {
                    return u8(2LL);
                    break;
                }
                case u8(8LL): {
                    return u8(3LL);
                    break;
                }
            }
        }
        return u8(0LL);
    }

    // selfUses counts the uses of parameter p of block b that are its own
    // argument on the edges in, into b.
    static i32 selfUses(Span<Edge*> in, Int k, i32 p) {
        i32 n = i32(0LL);
        {
            auto range1614 = in;
            for (Int index1615 = 0, rangeEnd1616 = length(range1614); index1615 < rangeEnd1616; ++index1615) {
                Edge* e = range1614[index1615];
                if (((k < length((e)->args)) && ((e)->args[k] == p))) {
                    n++;
                }
            }
        }
        return n;
    }

    // siftDown_func implements the heap property on data[lo:hi].
    // first is an offset into the array where the root of the heap lies.
    template <typename Data>
    static void siftDown_func(Data data, Int lo, Int hi, Int first) {
        Int root = lo;
        {
            for (; true;) {
                Int child = ((Int(2LL) * root) + Int(1LL));
                if ((child >= hi)) {
                    break;
                }
                if ((((child + Int(1LL)) < hi) && (data).Less((first + child), ((first + child) + Int(1LL))))) {
                    child++;
                }
                if ((!((data).Less((first + root), (first + child))))) {
                    return;
                }
                (data).Swap((first + root), (first + child));
                root = child;
            }
        }
    }

    static VReg slotLoc(i32 slot) {
        return convert<VReg>((i32(-2LL) - slot));
    }

    static i32 slotOf(VReg r) {
        return convert<i32>((VReg(-2LL) - r));
    }

    template <typename Data>
    static void stable_func(Data data, Int n) {
        Int blockSize = Int(20LL);
        auto temporary1617 = Int(0LL);
        auto temporary1618 = blockSize;
        Int a = temporary1617;
        Int b = temporary1618;
        {
            for (; (b <= n);) {
                insertionSort_func(data, a, b);
                a = b;
                b += blockSize;
            }
        }
        insertionSort_func(data, a, n);
        {
            for (; (blockSize < n);) {
                auto temporary1619 = Int(0LL);
                auto temporary1620 = (Int(2LL) * blockSize);
                a = temporary1619;
                b = temporary1620;
                {
                    for (; (b <= n);) {
                        symMerge_func(data, a, (a + blockSize), b);
                        a = b;
                        b += (Int(2LL) * blockSize);
                    }
                }
                {
                    Int m = (a + blockSize);
                    if ((m < n)) {
                        symMerge_func(data, a, m, n);
                    }
                }
                blockSize *= Int(2LL);
            }
        }
    }

    static u8 swapCC(u8 cc) {
        {
            auto switch1621 = cc;
            switch (switch1621) {
                case ccL: {
                    return ccG;
                    break;
                }
                case ccG: {
                    return ccL;
                    break;
                }
                case ccLE: {
                    return ccGE;
                    break;
                }
                case ccGE: {
                    return ccLE;
                    break;
                }
                case ccB: {
                    return ccA;
                    break;
                }
                case ccA: {
                    return ccB;
                    break;
                }
                case ccBE: {
                    return ccAE;
                    break;
                }
                case ccAE: {
                    return ccBE;
                    break;
                }
            }
        }
        return cc;
    }

    template <typename Data>
    static void swapRange_func(Data data, Int a, Int b, Int n) {
        {
            Int i = Int(0LL);
            for (; (i < n); i++) {
                (data).Swap((a + i), (b + i));
            }
        }
    }

    // symMerge_func merges the two sorted subsequences data[a:m] and data[m:b] using
    // the SymMerge algorithm from Pok-Son Kim and Arne Kutzner, "Stable Minimum
    // Storage Merging by Symmetric Comparisons", in Susanne Albers and Tomasz
    // Radzik, editors, Algorithms - ESA 2004, volume 3221 of Lecture Notes in
    // Computer Science, pages 714-723. Springer, 2004.
    //
    // Let M = m-a and N = b-n. Wolog M < N.
    // The recursion depth is bound by ceil(log(N+M)).
    // The algorithm needs O(M*log(N/M + 1)) calls to data.Less.
    // The algorithm needs O((M+N)*log(M)) calls to data.Swap.
    //
    // The paper gives O((M+N)*log(M)) as the number of assignments assuming a
    // rotation algorithm which uses O(M+N+gcd(M+N)) assignments. The argumentation
    // in the paper carries through for Swap operations, especially as the block
    // swapping rotate uses only O(M+N) Swaps.
    //
    // symMerge assumes non-degenerate arguments: a < m && m < b.
    // Having the caller check this condition eliminates many leaf recursion calls,
    // which improves performance.
    template <typename Data>
    static void symMerge_func(Data data, Int a, Int m, Int b) {
        if (((m - a) == Int(1LL))) {
            Int i = m;
            Int j = b;
            {
                for (; (i < j);) {
                    Int h = convert<i64>(shiftRight(convert<Uint>((i + j)), Int(1LL)));
                    if ((data).Less(h, a)) {
                        i = (h + Int(1LL));
                    } else {
                        j = h;
                    }
                }
            }
            {
                Int k = a;
                for (; (k < (i - Int(1LL))); k++) {
                    (data).Swap(k, (k + Int(1LL)));
                }
            }
            return;
        }
        if (((b - m) == Int(1LL))) {
            Int i = a;
            Int j = m;
            {
                for (; (i < j);) {
                    Int h = convert<i64>(shiftRight(convert<Uint>((i + j)), Int(1LL)));
                    if ((!((data).Less(m, h)))) {
                        i = (h + Int(1LL));
                    } else {
                        j = h;
                    }
                }
            }
            {
                Int k = m;
                for (; (k > i); k--) {
                    (data).Swap(k, (k - Int(1LL)));
                }
            }
            return;
        }
        Int mid = convert<i64>(shiftRight(convert<Uint>((a + b)), Int(1LL)));
        Int n = (mid + m);
        Int start = {};
        Int r = {};
        if ((m > mid)) {
            start = (n - b);
            r = mid;
        } else {
            start = a;
            r = m;
        }
        Int p = (n - Int(1LL));
        {
            for (; (start < r);) {
                Int c = convert<i64>(shiftRight(convert<Uint>((start + r)), Int(1LL)));
                if ((!((data).Less((p - c), c)))) {
                    start = (c + Int(1LL));
                } else {
                    r = c;
                }
            }
        }
        Int end = (n - start);
        if (((start < m) && (m < end))) {
            rotate_func(data, start, m, end);
        }
        if (((a < start) && (start < mid))) {
            symMerge_func(data, a, start, mid);
        }
        if (((mid < end) && (end < b))) {
            symMerge_func(data, mid, end, b);
        }
    }

    // symbolPart spells a wasm name in a symbol: letters and digits as they
    // are, every other byte, underscores too, as _XX. The spelling is one to
    // one and leaves "." to join parts.
    static String symbolPart(String s) {
        Slice<u8> b = makeSlice<u8>(Int(0LL), length(s));
        {
            Int i = Int(0LL);
            for (; (i < length(s)); i++) {
                u8 c = s[i];
                {
                    if (((c >= u8(97LL)) && (c <= u8(122LL))) || ((c >= u8(65LL)) && (c <= u8(90LL))) || ((c >= u8(48LL)) && (c <= u8(57LL)))) {
                        b = appendValues(b, c);
                    } else {
                        b = appendSlice(b, fmtSprintf(String("_%02X"), c));
                    }
                switchEnd1623:;
                }
            }
        }
        return asString(b);
    }

    // truncBounds returns the float bit patterns bounding a truncation that
    // produced the minimum integer: the input is in range when it is above low
    // (at or above, unless strict) and below high.
    static Tuple3<u64, u64, bool> truncBounds(u8 isize, u8 fsize) {
        u64 low{};
        u64 high{};
        bool strict{};
        if ((fsize == u8(8LL))) {
            if ((isize == u8(4LL))) {
                return {mathFloat64bits(double(-2147483649.0)), mathFloat64bits(double(2147483648.0)), true};
            }
            return {mathFloat64bits(double(-9.223372036854776e+18)), mathFloat64bits(double(9.223372036854776e+18)), false};
        }
        if ((isize == u8(4LL))) {
            return {convert<u64>(mathFloat32bits(double(-2147483648.0))), convert<u64>(mathFloat32bits(double(2147483648.0))), false};
        }
        return {convert<u64>(mathFloat32bits(double(-9.223372036854776e+18))), convert<u64>(mathFloat32bits(double(9.223372036854776e+18))), false};
    }

    // typeID is a structural identity of a function type, equal for equal
    // signatures across modules: call_indirect compares it at run time.
    static u64 typeID(FuncType* t) {
        u64 id = u64(1LL);
        Int bits = Int(1LL);
        auto push = [&](u64 code) mutable -> void {
            id = (shiftLeft(id, Int(3LL)) | code);
            bits += Int(3LL);
        };
        {
            auto range1624 = (t)->params;
            for (Int index1625 = 0, rangeEnd1626 = length(range1624); index1625 < rangeEnd1626; ++index1625) {
                ValType p = range1624[index1625];
                push(valTypeCode(p));
            }
        }
        push(u64(0LL));
        {
            auto range1627 = (t)->results;
            for (Int index1628 = 0, rangeEnd1629 = length(range1627); index1628 < rangeEnd1629; ++index1628) {
                ValType r = range1627[index1628];
                push(valTypeCode(r));
            }
        }
        if ((bits <= Int(63LL))) {
            return id;
        }
        u64 h = u64(14695981039346656037ULL);
        {
            auto range1630 = (t)->params;
            for (Int index1631 = 0, rangeEnd1632 = length(range1630); index1631 < rangeEnd1632; ++index1631) {
                ValType p = range1630[index1631];
                h = (((h ^ valTypeCode(p))) * u64(1099511628211LL));
            }
        }
        h = (((h ^ u64(7LL))) * u64(1099511628211LL));
        {
            auto range1633 = (t)->results;
            for (Int index1634 = 0, rangeEnd1635 = length(range1633); index1634 < rangeEnd1635; ++index1634) {
                ValType r = range1633[index1634];
                h = (((h ^ valTypeCode(r))) * u64(1099511628211LL));
            }
        }
        return (h | u64(9223372036854775808ULL));
    }

    static u8 typeSize(ValType t) {
        {
            auto switch1636 = t;
            switch (switch1636) {
                case I32:
                case F32: {
                    return u8(4LL);
                    break;
                }
                case V128: {
                    return u8(16LL);
                    break;
                }
            }
        }
        return u8(8LL);
    }

    static String typesPart(Slice<ValType> ts) {
        Slice<u8> b = makeSlice<u8>(length(ts));
        {
            auto range1637 = ts;
            for (Int index1638 = 0, rangeEnd1639 = length(range1637); index1638 < rangeEnd1639; ++index1638) {
                Int i = index1638;
                ValType t = range1637[index1638];
                auto temporary1640 = typeLetter(t);
                u8 c = temporary1640.v0;
                bool ok = temporary1640.v1;
                if ((!(ok))) {
                    compilerError(String("unsupported: a %s at the boundary"));
                }
                b[i] = c;
            }
        }
        return asString(b);
    }

    // unique is a list of blocks without repeats, in order.
    // functionComputePreds emits edges grouped by source block. Repair only
    // changes operands, so adjacent deduplication preserves predecessor order.
    static Slice<i32> uniquePredecessors(Slice<i32> blocks) {
        Slice<i32> out = {};
        for (Int i = 0; i < length(blocks); ++i) {
            const i32 block = blocks[i];
            if (!length(out) || out[length(out) - 1] != block) {
                out = appendValues(out, block);
            }
        }
        return out;
    }

    static FloatRange unknownFloatRange() {
        return FloatRange{mathInf(Int(-1LL)), mathInf(Int(1LL)), true};
    }

    static u64 valTypeCode(ValType t) {
        {
            auto switch1644 = t;
            switch (switch1644) {
                case I32: {
                    return u64(1LL);
                    break;
                }
                case I64: {
                    return u64(2LL);
                    break;
                }
                case F32: {
                    return u64(3LL);
                    break;
                }
                case F64: {
                    return u64(4LL);
                    break;
                }
                case V128: {
                    return u64(5LL);
                    break;
                }
                case FuncRef: {
                    return u64(6LL);
                    break;
                }
            }
        }
        return u64(7LL);
    }

    static void versionMemoryLoops(Function* f) {
        if ((length(((f)->module)->mems) == Int(0LL))) {
            return;
        }
        LoopBodyUses bodyUses(f);
        Map<i32, bool> done = mapOf<i32, bool>({});
        {
            Int round = Int(0LL);
            for (; (round < Int(64LL)); round++) {
                ObjPool cfgPool;
                CfgInfo* ci;
                {
                    PoolScope analysisScope(cfgPool);
                    ci = functionAnalyze(f);
                }
                bool changed = false;
                {
                    auto range1645 = (ci)->order;
                    for (Int index1646 = 0, rangeEnd1647 = length(range1645); index1646 < rangeEnd1647; ++index1646) {
                        i32 h = range1645[index1646];
                        if (((!((ci)->header[h])) || done[h])) {
                            continue;
                        }
                        setMap(done, h, true);
                        ObjPool loopPool;
                        MemoryLoop* loop;
                        {
                            PoolScope analysisScope(loopPool);
                            loop = findMemoryLoop(f, ci, h);
                            if (loop) {
                                loop->bodyUses = &bodyUses;
                                loop->invariantNodes = makeSlice<i32>(0);
                                loop->copies = makeSlice<i32>(0);
                            }
                        }
                        if (isNil(loop)) {
                            continue;
                        }
                        {
                            i32 fast = memoryLoopVersion(loop);
                            if ((fast >= i32(0LL))) {
                                {
                                    auto range1648 = (loop)->copies;
                                    for (Int index1649 = 0, rangeEnd1650 = length(range1648); index1649 < rangeEnd1650; ++index1649) {
                                        i32 h = range1648[index1649];
                                        setMap(done, h, true);
                                    }
                                }
                                changed = true;
                                break;
                            }
                        }
                    }
                }
                if ((!(changed))) {
                    break;
                }
            }
        }
    }

    static void writeELF(ObjCode* oc, Slice<u8> note) {
        constexpr Int secText = Int(11LL);
        constexpr Int secRelaText = {};
        constexpr Int secRodata = {};
        constexpr Int secEhFrame = {};
        constexpr Int secRelaEhFrame = {};
        constexpr Int secNoteModule = {};
        constexpr Int secSymtab = {};
        constexpr Int secStrtab = {};
        constexpr Int secShstrtab = {};
        constexpr Int secNoteStack = {};
        constexpr Int secCount = {};
        Slice<u8> strtab = {};
        auto str = [&](String s) mutable -> u32 {
            u32 off = convert<u32>(length(strtab));
            strtab = appendSlice(strtab, s);
            strtab = appendValues(strtab, u8(0LL));
            return off;
        };
        str(String(""));
        Slice<u8> symtab = {};
        auto sym = [&](u32 name, u8 info, u16 shndx, u64 value, u64 size) mutable -> void {
            symtab = littleAppendUint32(symtab, name);
            symtab = appendValues(symtab, info, u8(0LL));
            symtab = littleAppendUint16(symtab, shndx);
            symtab = littleAppendUint64(symtab, value);
            symtab = littleAppendUint64(symtab, size);
        };
        constexpr Int stbLocal = Int(0LL);
        constexpr Int stbGlobal = Int(1LL);
        constexpr Int sttNotype = Int(0LL);
        constexpr Int sttFunc = Int(2LL);
        constexpr Int sttSect = Int(3LL);
        constexpr Int symText = Int(1LL);
        constexpr Int symRodata = Int(2LL);
        sym(u32(0LL), u8(0LL), u16(0LL), u64(0LL), u64(0LL));
        sym(u32(0LL), u8(3LL), u16(1LL), u64(0LL), u64(0LL));
        sym(u32(0LL), u8(3LL), u16(3LL), u64(0LL), u64(0LL));
        Int count = Int(3LL);
        {
            auto range1651 = (oc)->regions;
            for (Int index1652 = 0, rangeEnd1653 = length(range1651); index1652 < rangeEnd1653; ++index1652) {
                Region r = range1651[index1652];
                if ((!((oc)->globals[(r).name]))) {
                    sym(str((r).name), u8(2LL), u16(1LL), convert<u64>((r).start), convert<u64>(((r).end - (r).start)));
                    count++;
                }
            }
        }
        Int firstGlobal = count;
        {
            auto range1654 = (oc)->regions;
            for (Int index1655 = 0, rangeEnd1656 = length(range1654); index1655 < rangeEnd1656; ++index1655) {
                Region r = range1654[index1655];
                if ((oc)->globals[(r).name]) {
                    sym(str((r).name), u8(18LL), u16(1LL), convert<u64>((r).start), convert<u64>(((r).end - (r).start)));
                    count++;
                }
            }
        }
        Int externBase = count;
        {
            auto range1657 = (oc)->externs;
            for (Int index1658 = 0, rangeEnd1659 = length(range1657); index1658 < rangeEnd1659; ++index1658) {
                String name = range1657[index1658];
                sym(str(name), u8(16LL), u16(0LL), u64(0LL), u64(0LL));
            }
        }
        auto rela = [&](Slice<u8> out, u64 at, Int symbol, u32 typ, i64 addend) mutable -> Slice<u8> {
            out = littleAppendUint64(out, at);
            out = littleAppendUint64(out, (shiftLeft(convert<u64>(symbol), Int(32LL)) | convert<u64>(typ)));
            return littleAppendUint64(out, convert<u64>(addend));
        };
        Slice<u8> relaText = {};
        {
            auto range1660 = (oc)->relocs;
            for (Int index1661 = 0, rangeEnd1662 = length(range1660); index1661 < rangeEnd1662; ++index1661) {
                TextReloc r = range1660[index1661];
                if (((r).sym < i32(0LL))) {
                    relaText = rela(relaText, convert<u64>((r).at), Int(2LL), rX86_64_PC32, (r).addend);
                } else {
                    relaText = rela(relaText, convert<u64>((r).at), (externBase + convert<i64>((r).sym)), rX86_64_GOTPCREL, (r).addend);
                }
            }
        }
        {
            auto range1663 = (oc)->addresses;
            for (Int index1664 = 0, rangeEnd1665 = length(range1663); index1664 < rangeEnd1665; ++index1664) {
                TextAddress r = range1663[index1664];
                relaText = rela(relaText, convert<u64>((r).at), Int(1LL), rX86_64_64, convert<i64>((r).target));
            }
        }
        auto temporary1666 = ehFrame((oc)->regions);
        Slice<u8> eh = temporary1666.v0;
        Slice<u32> starts = temporary1666.v1;
        Slice<u8> relaEh = {};
        {
            auto range1667 = starts;
            for (Int index1668 = 0, rangeEnd1669 = length(range1667); index1668 < rangeEnd1669; ++index1668) {
                Int k = index1668;
                u32 at = range1667[index1668];
                relaEh = rela(relaEh, convert<u64>(at), Int(1LL), rX86_64_PC32, convert<i64>(((oc)->regions[k]).start));
            }
        }
        Slice<u8> noteBody = littleAppendUint32({}, u32(7LL));
        noteBody = littleAppendUint32(noteBody, convert<u32>(length(note)));
        noteBody = littleAppendUint32(noteBody, u32(1LL));
        noteBody = appendValues(appendSlice(noteBody, noteOwner), u8(0LL));
        {
            for (; ((length(noteBody) % Int(4LL)) != Int(0LL));) {
                noteBody = appendValues(noteBody, u8(0LL));
            }
        }
        noteBody = appendSlice(noteBody, note);
        {
            for (; ((length(noteBody) % Int(4LL)) != Int(0LL));) {
                noteBody = appendValues(noteBody, u8(0LL));
            }
        }
        Slice<u8> shstrtab = {};
        auto sh = [&](String s) mutable -> u32 {
            u32 off = convert<u32>(length(shstrtab));
            shstrtab = appendSlice(shstrtab, s);
            shstrtab = appendValues(shstrtab, u8(0LL));
            return off;
        };
        sh(String(""));
        Array<u32, 11> names = Array<u32, 11>{{}};
        {
            auto range1670 = Array<Anonymous24, 10>{{Anonymous24{Int(1LL), String(".text")}, Anonymous24{Int(2LL), String(".rela.text")}, Anonymous24{Int(3LL), String(".rodata")}, Anonymous24{Int(4LL), String(".eh_frame")}, Anonymous24{Int(5LL), String(".rela.eh_frame")}, Anonymous24{Int(6LL), String(".note.wasm2o")}, Anonymous24{Int(7LL), String(".symtab")}, Anonymous24{Int(8LL), String(".strtab")}, Anonymous24{Int(9LL), String(".shstrtab")}, Anonymous24{Int(10LL), String(".note.GNU-stack")}}};
            for (Int index1671 = 0, rangeEnd1672 = length(range1670); index1671 < rangeEnd1672; ++index1671) {
                Anonymous24 sec = range1670[index1671];
                names[(sec).index] = sh((sec).name);
            }
        }
        Array<u64, 11> offsets{};
        Array<u64, 11> sizes{};
        Array<Span<const u8>, 11> contents{};
        u64 position = 64;
        auto reserveSection = [&](Int section, u64 bytes, usize alignment) {
            position = alignUp(usize(position), alignment);
            offsets[section] = position;
            sizes[section] = bytes;
            position += bytes;
        };
        auto place = [&](Int section, Span<const u8> data, usize alignment) {
            reserveSection(section, length(data), alignment);
            contents[section] = data;
        };
        place(1, oc->a.buf, 16);
        place(2, relaText, 8);
        reserveSection(3, oc->rodataSize, 16);
        place(4, eh, 8);
        place(5, relaEh, 8);
        place(6, noteBody, 4);
        place(7, symtab, 8);
        place(8, strtab, 1);
        place(9, shstrtab, 1);
        place(10, {}, 1);
        const u64 shoff = alignUp(usize(position), 8);
        u8 fileHeader[64]{};
        copyBytes(fileHeader, "\177ELF\002\001\001", 8);
        storeLittle(fileHeader + 16, u16(1));
        storeLittle(fileHeader + 18, u16(62));
        storeLittle(fileHeader + 20, u32(1));
        storeLittle(fileHeader + 40, shoff);
        storeLittle(fileHeader + 52, u16(64));
        storeLittle(fileHeader + 58, u16(64));
        storeLittle(fileHeader + 60, u16(11));
        storeLittle(fileHeader + 62, u16(9));
        u8 sectionHeaders[11 * 64]{};
        auto header = [&](Int section, u32 type, u64 flags, u32 link, u32 info, u64 alignment, u64 entrySize) {
            u8* bytes = sectionHeaders + usize(section) * 64;
            storeLittle(bytes, names[section]);
            storeLittle(bytes + 4, type);
            storeLittle(bytes + 8, flags);
            storeLittle(bytes + 24, offsets[section]);
            storeLittle(bytes + 32, sizes[section]);
            storeLittle(bytes + 40, link);
            storeLittle(bytes + 44, info);
            storeLittle(bytes + 48, alignment);
            storeLittle(bytes + 56, entrySize);
        };
        header(1, shtProgbits, shfAlloc | shfExec, 0, 0, 16, 0);
        header(2, shtRela, shfInfoLink, 7, 1, 8, 24);
        header(3, shtProgbits, shfAlloc, 0, 0, 16, 0);
        header(4, shtProgbits, shfAlloc, 0, 0, 8, 0);
        header(5, shtRela, shfInfoLink, 7, 4, 8, 24);
        header(6, shtNote, 0, 0, 0, 4, 0);
        header(7, shtSymtab, 0, 8, u32(firstGlobal), 8, 24);
        header(8, shtStrtab, 0, 0, 0, 1, 0);
        header(9, shtStrtab, 0, 0, 0, 1, 0);
        header(10, shtProgbits, 0, 0, 0, 1, 0);
        writeAll(1, fileHeader, sizeof(fileHeader));
        position = sizeof(fileHeader);
        for (Int section = 1; section < 11; ++section) {
            writeZeros(1, offsets[section] - position);
            if (section == 3) {
                u64 written = 0;
                for (Int i = 0; i < length(oc->blobs); ++i) {
                    writeZeros(1, u64(oc->blobAt[i]) - written);
                    writeBytes(1, oc->blobs[i].bytes);
                    written = u64(oc->blobAt[i]) + length(oc->blobs[i].bytes);
                }
                require(written == oc->rodataSize, "rodata layout mismatch");
            } else {
                writeBytes(1, contents[section]);
            }
            position = offsets[section] + sizes[section];
        }
        writeZeros(1, shoff - position);
        writeAll(1, sectionHeaders, sizeof(sectionHeaders));
    }

    static Slice<VReg> xmmRange(Int n) {
        Slice<VReg> out = makeSlice<VReg>(n);
        {
            auto range1673 = out;
            for (Int index1674 = 0, rangeEnd1675 = length(range1673); index1674 < rangeEnd1675; ++index1674) {
                Int i = index1674;
                out[i] = (XMM0 + convert<VReg>(i));
            }
        }
        return out;
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

    static Slice<u8> readInput(ObjPool& owner) {
        auto* input = &owner.make<ArrayStorage<u8, PageVec<u8>>>(&owner, ArrayData<u8>::Kind::Paged)->array;
        auto* pages = input->pages();
        for (;;) {
            const usize before = pages->size();
            pages->reserve(before + 1);
            const usize offset = before % pageBytes;
            const usize available = pageBytes - offset;
            IoVec vector{pages->pages.data()[before / pageBytes] + offset, available};
            usize count = 0;
            require(wasiFdRead(0, &vector, 1, &count) == 0, "stdin read failed");
            require(count <= available, "stdin read overflow");
            if (!count) {
                break;
            }
            pages->count += count;
        }
        if (pages->size() % pageBytes == 0) {
            freePage(pages->pages.popBack());
        }
        require(pages->size() <= 0x7fffffff, "input exceeds address space");
        input->count = Int(pages->size());
        return {input, 0, u32(input->count), u32(input->count)};
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
        ObjPool pool;
        PoolScope scope(pool);
        Module* module = decodeModule(readInput(pool));
        writeNumber(length(module->types));
        writeAll(1, " ", 1);
        writeNumber(length(module->funcs));
        writeAll(1, " ", 1);
        writeNumber(length(module->datas));
        writeAll(1, "\n", 1);
    }
}

namespace {
    static void compileWithOptions(CompileOptions options) {
        ObjPool modulePool;
        ObjPool ssaPool;
        ObjPool machinePool;
        PoolScope scope(modulePool);
        initializeCompilerTables();
        Module* module = decodeModule(readInput(modulePool));
        if (options.memoryTail && length(module->mems) > 0) {
            module->memoryTail = 65536;
            module->tailRanges = options.tailRanges;
            module->tailProfile = options.tailProfile && options.tailRanges;
        }
        ObjCode* code = modulePool.make<ObjCode>();
        code->pool = &modulePool;
        code->m = module;
        code->a.fixups = makeSlice<RipFixup>(0);
        code->a.sps = makeSlice<SpEntry>(0);
        code->funcAt = makeSlice<i32>(0);
        code->calls = makeSlice<CallFixup>(0);
        code->stubCalls = makeSlice<CallFixup>(0);
        code->jumpAt = makeMap<i32, i32>();
        code->jumpCalls = makeSlice<CallFixup>(0);
        code->blobs = makeSlice<CodeBlob>(0);
        code->blobAt = makeSlice<i32>(0);
        code->floatIx = makeMap<Array<u64, 2>, i32>();
        code->externs = makeSlice<String>(0);
        code->externIx = makeMap<String, i32>();
        code->relocs = makeSlice<TextReloc>(0);
        code->addresses = makeSlice<TextAddress>(0);
        code->regions = makeSlice<Region>(0);
        code->globals = makeMap<String, bool>();
        code->initFn = moduleAddInitFunction(module);
        code->lay = newLayout(module);
        code->funcAt = makeSlice<i32>(length(module->funcs));
        objCodeAddFixedBlobs(code);
        objCodeStubs(code);
        objCodeImportAdapters(code);
        for (Int i = module->importedFuncs; i < length(module->funcs); ++i) {
            Function* function;
            {
                PoolScope ssaScope(ssaPool);
                function = buildFunction(module, i);
                optimize(function);
            }
            PoolScope machineScope(machinePool);
            MFunc* machine = lowerFunction(module, code->lay, function);
            ssaPool.clear();
            Allocator* allocation = allocate(machine);
            objCodeEmitFunction(code, i, machine, allocation);
            machinePool.clear();
        }
        objCodeExportThunks(code);
        objCodeInitThunk(code);
        objCodePlaceBlobs(code);
        objCodeResolve(code);
        writeELF(code, encodeNote(objCodeDescription(code)));
    }
}

extern "C"
#if defined(__wasm__) || defined(wasm)
    __attribute__((export_name("compile")))
#endif
    void compile() {
    compileWithOptions(CompileOptions{true, true, false});
}

extern "C"
#if defined(__wasm__) || defined(wasm)
    __attribute__((export_name("compileOptions")))
#endif
    void compileOptions(u32 flags) {
    require((flags & ~u32(7)) == 0, "invalid compile options");
    compileWithOptions(CompileOptions{bool(flags & 1), bool(flags & 2), bool(flags & 4)});
}

namespace {
    static void compilerCommand(bool strict = false) {
#ifdef AOT_STRICT
        strict = true;
#endif
        if (strict) {
            compileWithOptions(CompileOptions{});
        } else {
            compile();
        }
    }
}

#if !defined(AOT_SELF_TEST)
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
    #elif !defined(__wasm__) && !defined(wasm)
int main(int count, char** arguments) {
    bool strict = false;
    if (count == 2) {
        const char* a = arguments[1];
        const char* b = "--strict";
        usize i = 0;
        while (a[i] && a[i] == b[i]) {
            ++i;
        }
        require(a[i] == 0 && b[i] == 0, "usage: aot [--strict] < input.wasm > output.o");
        strict = true;
    } else {
        require(count == 1, "usage: aot [--strict] < input.wasm > output.o");
    }
    compilerCommand(strict);
    return 0;
}
    #elif defined(AOT_COMMAND)
extern "C" __attribute__((export_name("_start"))) void start() {
    compilerCommand();
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

    static void allocatorSelfTest() {
        for (usize cls = 0; cls < heapClassCount; ++cls) {
            const usize bytes = Heap::classSize(cls);
            const usize low = cls == 0 ? 1 : Heap::classSize(cls - 1) + 1;
            require(Heap::classIndex(low) == cls && Heap::classIndex(bytes) == cls, "allocator size class");
            PageVec<void*> objects;
            const usize count = 3 * (pageBytes / bytes) + 7;
            for (usize i = 0; i < count; ++i) {
                auto* object = static_cast<u8*>(heap.allocate(low));
                require(reinterpret_cast<uptr>(object) % objectAlignment == 0, "allocator alignment");
                *reinterpret_cast<u64*>(object) = i + 1;
                *reinterpret_cast<u64*>(object + bytes - sizeof(u64)) = ~(u64(i) + 1);
                objects.pushBack(object);
            }
            for (usize i = 0; i < count; ++i) {
                auto* object = static_cast<u8*>(objects[i]);
                require(*reinterpret_cast<u64*>(object) == i + 1, "allocator overlap");
                require(*reinterpret_cast<u64*>(object + bytes - sizeof(u64)) == ~(u64(i) + 1), "allocator extent");
            }
            for (usize i = 1; i < count; i += 2) {
                heap.release(objects[i]);
            }
            for (usize i = 1; i < count; i += 2) {
                objects[i] = heap.allocate(bytes);
                *static_cast<u64*>(objects[i]) = i + 1;
            }
            for (usize i = 0; i < count; ++i) {
                require(*static_cast<u64*>(objects[i]) == i + 1, "partial page refill");
            }
            for (usize order = 0; order < 3; ++order) {
                for (usize i = order; i < count; i += 3) {
                    heap.release(objects[i]);
                }
            }
        }
        {
            PageVec<void*> large;
            for (usize i = 0; i < 96; ++i) {
                large.pushBack(heap.allocate(pageBytes));
            }
            const u32 mapped = heap.mappedSegments;
            large.visit([](void* object) {
                heap.release(object);
            });
            void* smaller[256];
            for (usize i = 0; i < 256; ++i) {
                smaller[i] = heap.allocate(8192);
            }
            require(heap.mappedSegments == mapped, "empty pages not shared between classes");
            for (usize i = 0; i < 256; ++i) {
                heap.release(smaller[i]);
            }
        }
        {
            void* first = heap.allocate(pageBytes);
            void* current = heap.allocate(pageBytes);
            heap.release(first);
            require(heap.pageOf(first)->cls == heapFreePage, "single-slot page release");
            heap.release(current);
        }
    #if defined(__wasm__) || defined(wasm)
        {
            const usize oldPages = __builtin_wasm_memory_grow(0, 1);
            require(oldPages != ~usize(0), "test memory growth");
            auto* first = reinterpret_cast<u8*>(oldPages * pageBytes);
            heap.adoptPages(first, pageBytes);
            HeapPage* recycled[1];
            for (usize i = 0; i < 1; ++i) {
                recycled[i] = heap.takePage();
                const uptr address = heap.pageAddress(recycled[i]);
                require(address >= reinterpret_cast<uptr>(first) && address < reinterpret_cast<uptr>(first) + pageBytes, "I/O page recycling");
            }
            for (usize i = 0; i < 1; ++i) {
                heap.releasePage(recycled[i]);
            }
        }
    #endif
    }

    static void selfTest() {
        for (usize prefix : {usize(0), pageBytes - 1, pageBytes - 2, pageBytes - 3, pageBytes - 7}) {
            Asm assembler;
            assembler.buf.resize(prefix);
            if (prefix) {
                assembler.buf[prefix - 1] = 0xa5;
            }
            auto emit = [&]<usize size>(const u8(&bytes)[size]) {
                assembler.buf.count = prefix;
                asmBytes(&assembler, bytes);
                require(assembler.buf.size() == prefix + size, "fixed assembler insertion size");
                for (usize i = 0; i < size; ++i) {
                    require(assembler.buf[prefix + i] == bytes[i], "fixed assembler bytes across page boundaries");
                }
                require(!prefix || assembler.buf[prefix - 1] == 0xa5, "assembler insertion preserves previous bytes");
            };
            emit({0x11});
            emit({0x22, 0x33});
            emit({0x44, 0x55, 0x66});
            emit({0x11, 0x22, 0x33, 0x44});
            emit({0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88});
        }
        static_assert(sizeof(Int) == 4 && sizeof(Uint) == 4);
        require(shiftLeft(u64(1), u64(1) << 32) == 0, "wide left shift count");
        require(shiftRight(u64(1), u64(1) << 32) == 0, "wide unsigned shift count");
        require(shiftRight(i64(-1), u64(1) << 32) == -1, "wide signed shift count");
        require(hashValue(Int(-1)) == hashValue(i64(-1)), "signed index hash");
        allocatorSelfTest();
        void* page = allocPage();
        freePage(page);
        require(allocPage() == page, "page freelist");
        freePage(page);
        for (usize bytes = 1; bytes <= pageBytes; bytes *= 2) {
            void* object = heap.allocate(bytes);
            require(reinterpret_cast<uptr>(object) % objectAlignment == 0, "object alignment");
            heap.release(object);
            require(heap.allocate(bytes) == object, "object freelist");
            heap.release(object);
        }
        SmallVec<u32> small;
        require(sizeof(small) == sizeof(void*) && small.empty(), "empty vector representation");
        for (u32 i = 0; i < 1000; ++i) {
            small.pushBack(i);
        }
        for (u32 i = 0; i < 1000; ++i) {
            require(small[i] == i, "small vector contents");
        }
        while (small.size() < small.capacity()) {
            small.pushBack(17);
        }
        const u32 first = small[0];
        small.pushBack(small[0]);
        require(small.back() == first && small[0] == first, "small vector aliased growth");
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
        Vec<u32> promoted;
        for (u32 i = 0; i < 40000; ++i) {
            promoted.pushBack(i * 3);
        }
        require(promoted.paged && promoted.small.empty(), "vector promotion releases small storage");
        promoted.assign(subslice(promoted.view(), 123, 33000));
        require(promoted.size() == 33000 - 123, "vector view assignment size");
        for (usize i = 0; i < promoted.size(); ++i) {
            require(promoted[i] == (i + 123) * 3, "vector view assignment across pages");
        }
        promoted.fill(42);
        require(promoted.size() == 33000 - 123, "paged fill preserves size");
        for (usize i = 0; i < promoted.size(); ++i) {
            require(promoted[i] == 42, "paged fill contents");
        }
        Vec<i32> filled;
        filled.fill(-1);
        filled.resize(37);
        filled.fill(-1);
        require(filled.size() == 37 && filled[0] == -1 && filled[36] == -1, "small fill contents");
        for (usize count : {usize(37), usize(17000)}) {
            Vec<i32> values;
            values.reserve(count);
            require(values.size() == 0, "vector reserve preserves size");
            values.resize(count);
            values.fill(-7);
            Vec<i32> movedValues(move(values));
            require(values.size() == 0 && movedValues.size() == count, "vector move preserves size");
            require(movedValues.popBack() == -7 && movedValues.size() == count - 1, "vector pop preserves contents");
            values.pushBack(11);
            values = move(movedValues);
            require(movedValues.size() == 0 && values.size() == count - 1 && values[0] == -7, "vector move assignment replaces contents");
            values.clear();
            require(values.size() == 0, "vector clear resets size");
            values.pushBack(13);
            require(values.size() == 1 && values[0] == 13, "vector reuse after clear");
        }
        {
            Vec<u32> values;
            const usize count = (pageBytes - sizeof(Buffer::Header)) / sizeof(u32);
            values.resize(count);
            values.fill(19);
            values.append(subslice(values.view(), Int(count - 3), Int(count)));
            require(values.size() == count + 3 && values[count - 1] == 19 && values[count + 2] == 19, "self append promotes vector storage");
        }
        static_assert(sizeof(MInst) <= 72);
        for (i32 delta : {32, 131072}) {
            ObjPool pool;
            PoolScope scope(pool);
            Module module;
            module.memoryTail = pageBytes;
            auto* f = pool.make<Function>();
            f->module = &module;
            functionNewBlock(f);
            const i32 a = functionNewValue(f, OpParam, I32, 0, 0, {});
            const i32 offset = functionNewValue(f, OpConst, I32, -1, delta, {});
            const i32 b = functionNewValue(f, OpAdd, I32, 0, 0, Span<const i32>({a, offset}));
            MFunc machine;
            machine.blocks.pushBack(MBlock{});
            const VReg shared = mFuncNewReg(&machine, classGPR);
            auto check = [&](i32 value) {
                MInst in = inst(mCheck);
                in.src = shared;
                in.imm = 4;
                in.addressValue = value;
                machine.blocks[0].insts.pushBack(in);
            };
            check(a);
            MInst add = inst(mLea);
            add.dst = shared;
            add.mem = memAt(shared, delta);
            add.size = 4;
            machine.blocks[0].insts.pushBack(add);
            check(b);
            Lowerer lowerer;
            lowerer.m = &module;
            lowerer.f = f;
            lowerer.mf = &machine;
            lowerer.vregs.resize(f->values.size());
            lowerer.vregs.fill(noReg);
            lowerer.vregs[a] = shared;
            lowerer.vregs[b] = shared;
            lowererVersionTailRanges(&lowerer);
            require((machine.blocks.size() > 1) == (delta == 32), "range guards distinguish SSA addresses in one reused register");
            for (usize block = 0; block < machine.blocks.size(); ++block) {
                auto instructions = machine.blocks[block].insts.view();
                for (Int i = 0; i < length(instructions); ++i) {
                    if (instructions[i].op == mCheck) {
                        require(instructions[i].addressValue == a || instructions[i].addressValue == b, "fallback checks retain their SSA addresses");
                    }
                }
            }
        }
        {
            ObjPool pool;
            PoolScope scope(pool);
            InlineVec<u32, 2> values({11, 22});
            require(values.data() == values.storage.values && values.size() == 2, "inline vector starts in its owner");
            values.pushBack(33);
            require(values.data() != values.storage.values && values.size() == 3 && values[0] == 11 && values[2] == 33, "inline vector grows to a buffer");
            values.append(values.view());
            require(values.size() == 6 && values[0] == 11 && values[3] == 11 && values[5] == 33, "inline vector appends an internal view during growth");
            values.assign(subslice(values.view(), 2, 5));
            require(values.size() == 3 && values[0] == 33 && values[1] == 11 && values[2] == 22, "inline vector assigns an internal view");
            InlineVec<u32, 2> moved(move(values));
            require(!values.size() && values.data() == values.storage.values && moved.size() == 3 && moved[2] == 22, "inline vector moves buffer ownership");
            moved = InlineVec<u32, 2>({41});
            require(moved.data() == moved.storage.values && moved.size() == 1 && moved[0] == 41, "inline vector releases a replaced buffer");
            values = move(moved);
            require(!moved.size() && values.size() == 1 && values[0] == 41, "inline vector moves inline elements");
            constexpr usize boundary = PageVec<u32>::itemsPerPage;
            InlineVec<u32, 2> large;
            for (u32 i = 0; i < boundary; ++i) {
                large.pushBack(i);
            }
            large.append(subslice(large.view(), boundary - 4, boundary));
            require(!large.data() && large.size() == boundary + 4, "inline vector grows from a full buffer to pages");
            for (usize i = 0; i < large.size(); ++i) {
                require(large[i] == (i < boundary ? i : i - 4), "inline vector preserves internal source on promotion");
            }
            values = move(large);
            require(!large.size() && values.size() == boundary + 4 && values[boundary] == boundary - 4, "inline vector moves page ownership");
            values = InlineVec<u32, 2>{};
            require(!values.size() && values.data() == values.storage.values, "inline vector releases replaced pages");
        }
        {
            ObjPool pool;
            PoolScope scope(pool);
            InlineVec<Lrange, 1> x({{10, 20}});
            InlineVec<Lrange, 1> y({{0, 5}});
            InlineVec<Lrange, 1> scratch;
            mergeRanges(x, y, scratch);
            require(!y.size() && x.size() == 2 && x[0].from == 0 && x[1].to == 20, "range union preserves a gap");
            y = InlineVec<Lrange, 1>({{5, 10}});
            mergeRanges(x, y, scratch);
            require(!y.size() && x.size() == 1 && x[0].from == 0 && x[0].to == 20, "range union closes a gap");
            y = InlineVec<Lrange, 1>({{20, 25}, {40, 45}});
            mergeRanges(x, y, scratch);
            require(x.size() == 2 && x[0].to == 25 && x[1].from == 40 && x[1].to == 45, "range union appends after a touching range");
            y = InlineVec<Lrange, 1>({{30, 35}});
            mergeRanges(x, y, scratch);
            require(x.size() == 3 && x[0].to == 25 && x[1].from == 30 && x[1].to == 35 && x[2].from == 40, "range union reuses merge storage");
        }
        {
            ObjPool pool;
            PoolScope scope(pool);
            InlineVec<Lrange, 1> x;
            InlineVec<Lrange, 1> y;
            InlineVec<Lrange, 1> scratch;
            for (i32 i = 0; i < 12000; ++i) {
                (i & 1 ? y : x).pushBack({4 * i, 4 * i + 2});
            }
            mergeRanges(x, y, scratch);
            require(!y.size() && x.size() == 12000, "paged range union size");
            usize at = 0;
            for (i32 p = 0; p < 48000; ++p) {
                while (at < x.size() && x[at].to <= p) {
                    ++at;
                }
                const bool covered = at < x.size() && x[at].from <= p;
                require(covered == (p % 4 < 2), "paged range union coverage");
            }
            for (i32 i = 0; i < 12000; ++i) {
                y.pushBack({4 * i + 2, 4 * i + 4});
            }
            mergeRanges(x, y, scratch);
            require(!y.size() && x.size() == 1 && x[0].from == 0 && x[0].to == 48000, "range union coalesces page-spanning gaps");
        }
        {
            ObjPool pool;
            PoolScope scope(pool);
            const auto oldAll = callerSaved;
            const auto oldGprs = callerSavedGprs;
            const auto oldXmms = callerSavedXmms;
            callerSavedGprs = sliceOf<VReg>({RAX});
            callerSavedXmms = sliceOf<VReg>({XMM0});
            callerSaved = sliceOf<VReg>({RAX, XMM0});
            for (u8 mask = 0; mask < 4; ++mask) {
                MFunc function;
                function.blocks.pushBack(MBlock{});
                if (mask & (1u << classGPR)) {
                    mFuncNewReg(&function, classGPR);
                }
                if (mask & (1u << classXMM)) {
                    mFuncNewReg(&function, classXMM);
                }
                MInst call = inst(mCall);
                call.aux = calleeFunc;
                function.blocks[0].insts.pushBack(call);
                Allocator allocation;
                allocation.mf = &function;
                allocatorLinearize(&allocation);
                allocatorLiveness(&allocation);
                allocatorBuildIntervals(&allocation);
                require(function.registerClasses == mask, "machine register class summary");
                require(bool(allocation.fixed[RAX]) == bool(mask & (1u << classGPR)), "call clobbers constrain virtual GPRs");
                require(bool(allocation.fixed[XMM0]) == bool(mask & (1u << classXMM)), "call clobbers constrain virtual XMMs");
            }
            callerSaved = oldAll;
            callerSavedGprs = oldGprs;
            callerSavedXmms = oldXmms;
        }
        {
            ObjPool pool;
            PoolScope scope(pool);
            MFunc function;
            function.blocks.pushBack(MBlock{});
            const VReg integer = mFuncNewReg(&function, classGPR);
            const VReg floating = mFuncNewReg(&function, classXMM);
            const VReg integerCopy = mFuncNewReg(&function, classGPR);
            const VReg floatingCopy = mFuncNewReg(&function, classXMM);
            auto emit = [&](MOp op, VReg dst, VReg src) {
                MInst in = inst(op);
                in.dst = dst;
                in.src = src;
                in.size = 8;
                function.blocks[0].insts.pushBack(in);
            };
            emit(mMovImm, integer, noReg);
            emit(mMov, integerCopy, integer);
            emit(mFConst, floating, noReg);
            emit(mFMov, floatingCopy, floating);
            Allocator allocation;
            allocation.mf = &function;
            allocatorLinearize(&allocation);
            require(allocatorSelectCopies(&allocation), "selecting virtual register copies");
            allocatorLiveness(&allocation, true);
            allocatorBuildIntervals<true>(&allocation);
            require(allocatorCoalesce(&allocation), "coalescing virtual register copies");
            require(function.nregs == 2 && length(function.classValue) == 2, "coalescing compacts virtual register IDs");
            require(function.classValue[0] == classGPR && function.classValue[1] == classXMM, "coalescing preserves register classes");
            require(function.blocks[0].insts[0].dst == firstVirtual && function.blocks[0].insts[2].dst == firstVirtual + 1, "coalescing preserves canonical register order");
            require(function.blocks[0].insts[1].op == mNop && function.blocks[0].insts[3].op == mNop, "coalescing removes renamed self copies");
        }
        for (Int registers : {2, 2050}) {
            ObjPool pool;
            PoolScope scope(pool);
            MFunc function;
            function.blocks.pushBack(MBlock{});
            function.blocks.pushBack(MBlock{});
            for (Int i = 0; i < registers; ++i) {
                mFuncNewReg(&function, classGPR);
            }
            const VReg local = firstVirtual;
            const VReg input = firstVirtual + 1;
            auto emit = [&](Int block, MOp op, VReg dst, VReg src) {
                MInst in = inst(op);
                in.dst = dst;
                in.src = src;
                in.size = 8;
                function.blocks[block].insts.pushBack(in);
            };
            emit(0, mMovImm, input, noReg);
            emit(1, mMov, local, input);
            for (Int i = 0; i < 3; ++i) {
                emit(1, mAluImm, local, noReg);
                emit(1, mAluImm, input, noReg);
            }
            function.blocks[0].succs = sliceOf<i32>({1});
            function.blocks[1].preds = sliceOf<i32>({0});
            for (bool copiesOnly : {false, true}) {
                Allocator allocation;
                allocation.mf = &function;
                allocatorLinearize(&allocation);
                if (copiesOnly) {
                    require(allocatorSelectCopies(&allocation), "liveness copy operand selection");
                }
                allocatorLiveness(&allocation, copiesOnly);
                const Int x = copiesOnly ? allocation.copyIndex[0] : 0;
                const Int y = copiesOnly ? allocation.copyIndex[1] : 1;
                require(!bitsetHas(allocation.liveIn[1], x) && bitsetHas(allocation.liveIn[1], y), "local and incoming use-def liveness");
                require(!bitsetHas(allocation.liveIn[0], y) && bitsetHas(allocation.liveOut[0], y), "use-def liveness across a predecessor");
                bitsetEach(allocation.liveOut[1], [&](Int) {
                    fail("unexpected outgoing liveness after local use-defs");
                });
            }
        }
        {
            ObjPool pool;
            PoolScope scope(pool);
            MFunc function;
            for (Int b = 0; b < 5; ++b) {
                function.blocks.pushBack(MBlock{});
            }
            const VReg a = mFuncNewReg(&function, classGPR);
            const VReg untracked = mFuncNewReg(&function, classGPR);
            const VReg b = mFuncNewReg(&function, classGPR);
            const VReg temporary = mFuncNewReg(&function, classGPR);
            const VReg c = mFuncNewReg(&function, classGPR);
            const VReg d = mFuncNewReg(&function, classGPR);
            auto emit = [&](Int block, MOp op, VReg dst, VReg src) {
                MInst in = inst(op);
                in.dst = dst;
                in.src = src;
                in.size = 8;
                function.blocks[block].insts.pushBack(in);
            };
            emit(0, mMovImm, a, noReg);
            emit(0, mMovImm, untracked, noReg);
            emit(1, mMov, b, a);
            emit(1, mMovImm, temporary, noReg);
            emit(2, mAlu, b, untracked);
            MInst compare = inst(mCmp);
            compare.src = a;
            compare.src2 = b;
            function.blocks[2].insts.pushBack(compare);
            emit(3, mMov, RAX, b);
            function.blocks[3].insts.pushBack(inst(mCall));
            emit(4, mMov, c, d);
            function.blocks[0].succs = sliceOf<i32>({1});
            function.blocks[1].succs = sliceOf<i32>({2});
            function.blocks[2].succs = sliceOf<i32>({1, 3});
            function.blocks[1].preds = sliceOf<i32>({0, 2});
            function.blocks[2].preds = sliceOf<i32>({1});
            function.blocks[3].preds = sliceOf<i32>({2});
            Allocator full;
            full.mf = &function;
            allocatorLinearize(&full);
            allocatorLiveness(&full);
            allocatorBuildIntervals(&full);
            Allocator projected;
            projected.mf = &function;
            allocatorLinearize(&projected);
            require(allocatorSelectCopies(&projected) && projected.copyRegs.size() == 4, "selecting copies with gaps in register IDs");
            allocatorLiveness(&projected, true);
            allocatorBuildIntervals<true>(&projected);
            for (Int x = 0; x < function.nregs; ++x) {
                if (projected.copyIndex[usize(x)] < 0) {
                    require(!projected.copyIntervals[usize(x)], "unselected registers have no projected intervals");
                    continue;
                }
                const Int selected = projected.copyIndex[usize(x)];
                for (usize i = 0; i < full.order.size(); ++i) {
                    const i32 block = full.order[i];
                    require(bitsetHas(full.liveIn[block], x) == bitsetHas(projected.liveIn[block], selected), "projected live-in equals full analysis");
                    require(bitsetHas(full.liveOut[block], x) == bitsetHas(projected.liveOut[block], selected), "projected live-out equals full analysis");
                }
                Interval* expected = full.intervals[usize(x)];
                CopyInterval* actual = projected.copyIntervals[usize(x)];
                require(bool(expected) == bool(actual), "projected reachable intervals");
                if (!expected) {
                    continue;
                }
                require(expected->ranges.size() == actual->ranges.size(), "projected interval bounds");
                for (usize i = 0; i < expected->ranges.size(); ++i) {
                    require(expected->ranges[i].from == actual->ranges[i].from && expected->ranges[i].to == actual->ranges[i].to, "projected interval equals full analysis");
                }
            }
            for (Int r = 0; r < 32; ++r) {
                require(!projected.fixed[r], "coalescing does not build physical intervals");
            }
            require(!allocatorCoalesce(&projected) && function.nregs == 6, "coalescing preserves overlapping copies and unreachable registers");
            MFunc noCopies;
            noCopies.blocks.pushBack(MBlock{});
            const VReg value = mFuncNewReg(&noCopies, classGPR);
            MInst physical = inst(mMov);
            physical.dst = RAX;
            physical.src = value;
            noCopies.blocks[0].insts.pushBack(physical);
            Allocator skipped;
            skipped.mf = &noCopies;
            require(!allocatorSelectCopies(&skipped) && !skipped.copyIndex.size(), "physical copies need no coalescing analysis");
        }
        {
            ObjPool pool;
            PoolScope scope(pool);
            Allocator allocation;
            Interval first{.vreg = firstVirtual, .reg = RBX};
            Interval second{.vreg = firstVirtual + 1, .reg = RCX};
            Interval third{.vreg = firstVirtual + 2, .reg = RDX};
            Interval fixed{.vreg = RAX, .reg = RAX};
            intervalAddRange(&first, 6, 10);
            intervalAddRange(&first, 0, 3);
            intervalAddRange(&second, 0, 7);
            intervalAddRange(&third, 4, 6);
            intervalAddRange(&fixed, 8, 12);
            intervalAddRange(&fixed, 2, 4);
            for (Interval* it : {&first, &second, &third, &fixed}) {
                intervalFinish(it);
            }
            allocation.active = {&first, &second};
            allocation.inactive = {&third, &fixed};
            const i32 positions[] = {0, 2, 3, 4, 6, 7, 8, 10, 12};
            const i32 boundaries[] = {2, 3, 4, 6, 7, 8, 10, 12, maxPos};
            for (usize i = 0; i < 9; ++i) {
                const i32 pos = positions[i];
                require(allocatorAdvance(&allocation, pos) == boundaries[i], "scan next membership boundary");
                for (usize j = 0; j < allocation.active.size(); ++j) {
                    require(intervalCovers(allocation.active[j], pos), "scan active membership at boundary");
                }
                for (usize j = 0; j < allocation.inactive.size(); ++j) {
                    require(!intervalCovers(allocation.inactive[j], pos) && intervalEnd(allocation.inactive[j]) > pos, "scan inactive membership at boundary");
                }
                if (pos == 3) {
                    require(allocation.active.size() == 2 && allocation.active[0] == &second && allocation.active[1] == &fixed && allocation.inactive.size() == 2 && allocation.inactive[0] == &third && allocation.inactive[1] == &first, "scan preserves membership order across holes");
                }
            }
            require(!allocation.active.size() && !allocation.inactive.size(), "scan releases expired memberships");
            Interval prefix{.vreg = firstVirtual, .reg = RBX, .hint = noReg};
            Interval probe{.vreg = firstVirtual + 1, .reg = RCX, .hint = noReg};
            intervalAddRange(&prefix, 8, 12);
            intervalAddRange(&prefix, 0, 2);
            intervalAddRange(&probe, 5, 7);
            intervalFinish(&prefix);
            intervalFinish(&probe);
            allocation.inactive.pushBack(&prefix);
            const i32 boundary = allocatorAdvance(&allocation, 4);
            require(boundary == 8, "scan future inactive range boundary");
            intervalSplit(&prefix, 6);
            require(prefix.reg == RBX && intervalEnd(&prefix) == 2 && prefix.cursor == length(prefix.ranges) && intervalIntersection(&prefix, &probe) == maxPos, "shortened inactive prefix stays inert until advance");
            require(allocatorAdvance(&allocation, boundary) == maxPos && !allocation.inactive.size(), "scan cleans shortened inactive prefix at cached boundary");
        }
        {
            ObjPool pool;
            PoolScope scope(pool);
            Interval root{.vreg = firstVirtual, .reg = noReg, .hint = noReg};
            intervalAddRange(&root, 30, 40);
            intervalAddRange(&root, 10, 20);
            intervalFinish(&root);
            require(intervalEnd(&root) == 40 && !intervalEndsBefore(&root, 39), "interval upper bound after finishing");
            Interval* middle = intervalSplit(&root, 15);
            Interval* tail = intervalSplit(middle, 25);
            require(intervalEnd(&root) == 15 && intervalEndsBefore(&root, 15), "interval upper bound after splitting a range");
            require(intervalEnd(middle) == 20 && intervalEndsBefore(middle, 20), "interval upper bound after splitting in a gap");
            require(intervalEnd(tail) == 40 && !intervalEndsBefore(tail, 39), "nested interval upper bound");
            require(intervalCovers(&root, 14) && !intervalCovers(&root, 15), "split interval coverage");
            require(!intervalCovers(middle, 25) && !intervalCovers(tail, 25) && intervalCovers(tail, 30), "split gap coverage");
        }
        {
            Interval interval;
            const i32 visits[] = {11, 10, 11, 7, 6, 7, 2, 3, 2, 0, 1, 1, 0};
            for (i32 p : visits) {
                intervalRecordUse(&interval, p);
            }
            intervalFinish(&interval);
            const i32 expected[] = {0, 1, 2, 3, 6, 7, 10, 11};
            require(interval.uses.size() == 8, "interval use deduplication");
            for (usize i = 0; i < 8; ++i) {
                require(interval.uses[i] == expected[i], "interval use order");
            }
            require(intervalNextUse(&interval, 4) == 6 && intervalPrevUse(&interval, 6) == 3, "interval use lookup");
        }
        {
            Interval intervals[5]{};
            IntervalQueue queue;
            queue.push(&intervals[0], 5);
            queue.push(&intervals[1], 3);
            queue.push(&intervals[2], 5);
            require(queue.pop() == &intervals[1], "interval queue priority");
            queue.push(&intervals[3], 5);
            queue.push(&intervals[4], 1);
            require(queue.pop() == &intervals[4], "interval queue insertion");
            require(queue.pop() == &intervals[0], "interval queue first tie");
            require(queue.pop() == &intervals[2], "interval queue second tie");
            require(queue.pop() == &intervals[3], "interval queue later tie");
            require(queue.empty(), "interval queue drain");
        }
        {
            Interval intervals[6]{};
            IntervalQueue queue;
            queue.initial.pushBack({&intervals[0], 3, 0});
            queue.initial.pushBack({&intervals[1], 5, 1});
            queue.initial.pushBack({&intervals[2], 5, 2});
            queue.nextOrder = 3;
            queue.push(&intervals[3], 5);
            queue.push(&intervals[4], 1);
            require(queue.pop() == &intervals[4], "split before initial interval");
            require(queue.pop() == &intervals[0], "initial interval priority");
            queue.push(&intervals[5], 5);
            require(queue.pop() == &intervals[1] && queue.pop() == &intervals[2], "initial interval ties");
            require(queue.pop() == &intervals[3] && queue.pop() == &intervals[5], "split interval ties");
            require(queue.empty(), "merged interval queue drain");
        }
        u64 total = 0;
        moved.visit([&](u64 value) {
            total += value;
        });
        require(total == u64(49999) * 50000 / 2 * 7, "paged visitor");
        static_assert(sizeof(ArrayData<u8>) == 2 * sizeof(void*) + 8);
        static_assert(__is_trivially_destructible(ArrayData<u8>));
        {
            ObjPool pool;
            PoolScope scope(pool);
            u32 borrowed[] = {11, 22, 33};
            auto values = borrowSlice(borrowed, 3);
            auto alias = subslice(values, 1, 3);
            values[1] = 44;
            require(borrowed[1] == 44 && alias[0] == 44, "borrowed slice aliases");
            values = appendValues(values, u32(55));
            values[1] = 66;
            require(borrowed[1] == 44 && alias[0] == 44 && values[3] == 55, "borrowed slice growth");
            auto empty = makeSlice<u32>(0);
            empty = appendValues(empty, u32(77));
            require(length(empty) == 1 && empty[0] == 77, "empty slice growth");
            const Int smallCount = (pageBytes - sizeof(Buffer::Header)) / sizeof(u32);
            auto growing = makeSlice<u32>(smallCount);
            growing[smallCount - 1] = 88;
            auto tail = subslice(growing, smallCount - 1, smallCount);
            growing = appendValues(growing, u32(99));
            require(growing[0] == 0 && growing[smallCount - 1] == 88 && growing[smallCount] == 99, "slice growth to pages");
            growing[smallCount - 1] = 111;
            require(tail[0] == 88, "old slice survives paged growth");
            const Int boundary = PageVec<u32>::itemsPerPage;
            auto paged = makeSlice<u32>(boundary + 2);
            require(paged[boundary - 1] == 0 && paged[boundary] == 0, "paged slice zero initialization");
            paged[boundary - 1] = 123;
            paged[boundary] = 456;
            copySlice(subslice(Span<u32>(paged), boundary, boundary + 2), subslice(Span<const u32>(paged), boundary - 1, boundary + 1));
            require(paged[boundary] == 123 && paged[boundary + 1] == 456, "overlapping copy across pages");
            auto bytes = makeSlice<u8>(pageBytes + 2);
            bytes[pageBytes - 1] = 'a';
            bytes[pageBytes] = 'b';
            bytes[pageBytes + 1] = 'c';
            require(asString(subslice(bytes, pageBytes - 1, pageBytes + 2)) == String("abc"), "string across pages");
            require(asString(subslice(bytes, pageBytes, pageBytes + 2)) == String("bc"), "string within page");
        }
        {
            ObjPool pool;
            PoolScope scope(pool);
            const Int sizes[] = {0, 1, 63, 64, 65, 511, 2048, 2049, 16385};
            for (Int size : sizes) {
                Bitset live = newBitset(size);
                Bitset out = newBitset(size);
                Bitset kill = newBitset(size);
                Bitset gen = newBitset(size);
                bool expectedChange = false;
                for (Int i = 0; i < size; ++i) {
                    if (i % 7 == 0) {
                        bitsetSet(live, i);
                    }
                    if (i % 3 == 0) {
                        bitsetSet(out, i);
                        expectedChange |= i % 5 != 0 && i % 7 != 0;
                    }
                    if (i % 5 == 0) {
                        bitsetSet(kill, i);
                    }
                    if (i % 11 == 0) {
                        bitsetSet(gen, i);
                    }
                }
                require(bitsetPropagate(live, {}, out, kill) == expectedChange, "bitset propagation change flag");
                require(!bitsetPropagate(live, {}, out, kill), "bitset propagation fixed point");
                bitsetPropagate(live, gen, {}, kill);
                bitsetUnion(live, kill);
                require(!bitsetUnion(live, live), "bitset self union");
                Int next = 0;
                auto expected = [](Int i) {
                    return i % 7 == 0 || i % 3 == 0 || i % 5 == 0 || i % 11 == 0;
                };
                for (Int i = 0; i < size; ++i) {
                    require(bitsetHas(live, i) == expected(i), "bitset dataflow result");
                }
                bitsetEach(live, [&](Int i) {
                    while (next < size && !expected(next)) {
                        ++next;
                    }
                    require(i == next++, "bitset dataflow iteration order");
                });
                while (next < size && !expected(next)) {
                    ++next;
                }
                require(next == size, "bitset dataflow iteration count");
            }
            Bitset small = newBitset(2048);
            Bitset large = newBitset(2049);
            bitsetSet(small, 7);
            bitsetSet(large, 2047);
            require(bitsetUnion(small, large) && bitsetHas(small, 2047), "sparse to dense union");
            require(bitsetUnion(large, small) && bitsetHas(large, 7), "dense to sparse union");
            require(!bitsetUnion(small, large), "mixed union fixed point");
        }
        {
            ObjPool pool;
            PoolScope scope(pool);
            Bitset live = newBitset(70001);
            Bitset gen = newBitset(70001);
            Bitset out = newBitset(70001);
            Bitset kill = newBitset(70001);
            bitsetSet(gen, 0);
            bitsetSet(gen, 16383);
            bitsetSet(gen, 65536);
            bitsetSet(out, 64);
            bitsetSet(out, 16384);
            bitsetSet(out, 70000);
            bitsetSet(kill, 16384);
            require(bitsetPropagate(live, gen, out, kill), "liveness changes");
            require(!bitsetPropagate(live, gen, out, kill), "liveness convergence");
            require(!bitsetHas(live, 16384), "liveness kill");
            const Int expected[] = {0, 64, 16383, 65536, 70000};
            usize seen = 0;
            bitsetEach(live, [&](Int bit) {
                require(seen < 5 && expected[seen++] == bit, "sparse bitset order");
            });
            require(seen == 5, "sparse bitset contents");
            require(bitsetUnion(live, out) && bitsetHas(live, 16384), "sparse union");
            require(!bitsetUnion(live, out), "sparse union convergence");
            Bitset filter = newBitset(70001);
            for (Int bit : {64, 42, 16384, 70000}) {
                bitsetSet(filter, bit);
            }
            const Int intersection[] = {64, 16384, 70000};
            seen = 0;
            bitsetEachIntersection(live, filter, [&](Int bit) {
                require(seen < 3 && bit == intersection[seen++], "sparse intersection order and contents");
            });
            require(seen == 3, "sparse intersection size");
            Bitset dense = newBitset(2048);
            bitsetSet(dense, 64);
            bitsetSet(dense, 100);
            seen = 0;
            bitsetEachIntersection(filter, dense, [&](Int bit) {
                require(bit == 64 && !seen++, "sparse intersection with dense filter");
            });
            bitsetEachIntersection(dense, filter, [&](Int bit) {
                require(bit == 64 && seen++ == 1, "dense intersection with sparse filter");
            });
            bitsetEachIntersection(live, {}, [&](Int) {
                fail("intersection with an empty filter");
            });
            require(seen == 2, "mixed intersection sizes");
        }
        for (Int size : {2049, 2304, 2305, 4096, 16383, 16384, 16385, 16641, 32769, 70001}) {
            ObjPool pool;
            PoolScope scope(pool);
            Bitset bits = newBitset(size);
            for (Int end = 256; end < size; end += 256) {
                bitsetSet(bits, end - 1);
            }
            bitsetSet(bits, size - 1);
            Bitset copy = newBitset(size);
            require(bitsetUnion(copy, bits) && !bitsetUnion(copy, bits), "partial sparse directory union");
            Int expected = 255;
            Int seen = 0;
            bitsetEach(copy, [&](Int bit) {
                require(bit == expected, "partial sparse directory iteration");
                expected = minimum(expected + 256, size - 1);
                ++seen;
            });
            require(seen == (size + 255) / 256 && bitsetHas(copy, size - 1) && bitsetWord(copy, usize(size + 63) / 64) == 0, "partial sparse directory final bit");
        }
        int count = 0;
        int order[3]{};
        {
            ObjPool pool;
            PoolScope scope(pool);
            auto map = makeMap<i32, i32>();
            for (i32 i = 0; i < 4096; ++i) {
                setMap(map, i, i * 3);
            }
            for (i32 i = 0; i < 4096; i += 2) {
                eraseMap(map, i);
            }
            require(length(map) == 2048, "map erase count");
            for (i32 i = 1; i < 4096; i += 2) {
                setMap(map, i, -i);
            }
            require(length(map) == 2048, "map update count");
            for (i32 i = 0; i < 8192; i += 2) {
                setMap(map, i, i * 7);
            }
            require(length(map) == 6144, "map reinsert count");
            for (i32 i = 0; i < 8192; ++i) {
                auto found = lookupMap(map, i);
                const bool present = i < 4096 || i % 2 == 0;
                require(found.v1 == present, "map membership after reuse");
                if (present) {
                    require(found.v0 == (i % 2 == 0 ? i * 7 : -i), "map value after reuse");
                }
            }
        }
        {
            ObjPool pool;
            PoolScope scope(pool);
            auto* f = pool.make<Function>();
            Optimizer optimizer;
            optimizer.f = f;
            const usize boundary = PageVec<i32>::itemsPerPage;
            f->pool.resize(boundary - 1);
            const i32 value = functionNewValue(f, OpSelect, I32, 0, 0, Span<const i32>({11, 22, 33}));
            auto before = functionArgs(f, value);
            require(before.count == 3 && before[0] == 11 && before[1] == 22 && before[2] == 33, "operand view across page boundary");
            before[1] = 44;
            require(f->pool[boundary] == 44, "operand view updates stored operands");
            f->pool.resize(2 * boundary + 1);
            require(before[0] == 11 && before[1] == 44 && before[2] == 33, "operand view survives pool growth");
            optimizerSetArgs(&optimizer, value, Span<const i32>({55}));
            require(functionArgs(f, value).count == 1 && functionArgs(f, value)[0] == 55 && before[1] == 44, "shortened operand view across pages");
            optimizerSetArgs(&optimizer, value, Span<const i32>({66, 77, 88, 99}));
            const auto after = functionArgs(f, value);
            require(after.count == 4 && after[0] == 66 && after[3] == 99 && before[0] == 55, "grown operand record preserves old view");
            const i32 empty = functionNewValue(f, OpUndef, I32, -1, 0, {});
            require(functionArgs(f, empty).count == 0, "empty operand view at pool end");
        }
        {
            ObjPool pool;
            PoolScope scope(pool);
            auto* f = pool.make<Function>();
            auto* o = pool.make<Optimizer>();
            o->f = f;
            for (i32 b = 0; b < 3; ++b) {
                functionNewBlock(f);
            }
            const i32 input = functionNewValue(f, OpParam, I32, 0, 0, {});
            const i32 zero = functionNewValue(f, OpConst, I32, -1, 0, {});
            const i32 first = functionNewValue(f, OpAdd, I32, 0, 0, Span<const i32>({input, zero}));
            const i32 second = functionNewValue(f, OpAdd, I32, 0, 0, Span<const i32>({input, zero}));
            const i32 user = functionNewValue(f, OpAdd, I32, 0, 0, Span<const i32>({second, first}));
            f->blocks[0].params = sliceOf<i32>({input});
            f->blocks[0].instrs = {first, second, user};
            i32 last = -1;
            for (i32 i = 0; i < 9000; ++i) {
                // Removed instructions retain their value and operand records.
                last = functionNewValue(f, OpAdd, I32, 0, 0, Span<const i32>({second, zero}));
            }
            const i32 left = functionNewValue(f, OpParam, I32, 1, 0, {});
            const i32 right = functionNewValue(f, OpParam, I32, 2, 0, {});
            f->blocks[1].params = sliceOf<i32>({left});
            f->blocks[2].params = sliceOf<i32>({right});
            f->blocks[0].term = Term{TermIf, second, sliceOf<Edge>({Edge{1, sliceOf<i32>({second})}, Edge{2, sliceOf<i32>({user})}}), {}, {}};
            f->blocks[1].term = Term{TermReturn, {}, {}, sliceOf<i32>({left}), {}};
            f->blocks[2].term = Term{TermReturn, {}, {}, sliceOf<i32>({second}), {}};
            optimizerReplace(o, second, first);
            optimizerReplace(o, first, input);
            optimizerSetArgs(o, user, Span<const i32>({second}));
            f->values[user].op = OpEqz;
            optimizerApply(o);
            require(functionArgs(f, user)[0] == input && f->values[user].nargs == 1, "aliases in shortened operand lists");
            require(functionArgs(f, last)[0] == input, "aliases across operand pages");
            require(f->blocks[0].term.value == input && f->blocks[0].term.edges[0].args[0] == input && f->blocks[2].term.values[0] == input, "aliases in terminators and edges");
            optimizerSetArgs(o, user, Span<const i32>({second, zero}));
            f->values[user].op = OpAdd;
            optimizerApply(o);
            require(functionArgs(f, user)[0] == input && functionArgs(f, user)[1] == zero, "aliases after growing a shortened operand list");
        }
        for (i32 removed : {0, 1, 2047, 2048, 2049, 16385}) {
            for (bool crossPage : {false, true}) {
                ObjPool pool;
                PoolScope scope(pool);
                auto* f = pool.make<Function>();
                auto* fe = pool.make<Frontend>();
                fe->f = f;
                for (i32 b = 0; b < 2; ++b) {
                    functionNewBlock(f);
                }
                fe->explicitValue = sliceOf<Int>({1, 2});
                fe->preds = makeSlice<Slice<PredRef>>(2);
                const i32 input = functionNewValue(f, OpParam, I32, 0, 0, {});
                functionNewValue(f, OpConst, I32, -1, 0, {});
                const i32 first = functionNewValue(f, OpParam, I32, 1, 0, {});
                const i32 second = functionNewValue(f, OpParam, I32, 1, 1, {});
                f->blocks[0].params = sliceOf<i32>({input});
                f->blocks[1].params = sliceOf<i32>({first, second});
                for (i32 i = 0; i < removed; ++i) {
                    const i32 phi = functionNewValue(f, OpParam, I32, 1, i + 2, {});
                    f->blocks[1].params = appendValues(f->blocks[1].params, phi);
                }
                const i32 constant = functionNewValue(f, OpConst, I32, -1, 17, {});
                if (removed) {
                    fe->alias.resize(4 + removed);
                    for (Int i = 0; i < length(fe->alias); ++i) {
                        fe->alias[i] = i < 4 ? -1 : constant;
                    }
                    if (removed > 1) {
                        fe->alias[4] = 5;
                    }
                    if (removed > 2) {
                        fe->alias[3 + removed] = 4;
                    }
                }
                if (crossPage) {
                    f->pool.resize(PageVec<i32>::itemsPerPage - 1);
                    for (usize i = 0; i < f->pool.size(); ++i) {
                        f->pool[i] = input;
                    }
                }
                const i32 source = removed ? 4 : constant;
                const i32 user = functionNewValue(f, OpAdd, I32, 0, 0, Span<const i32>({source, input}));
                const i32 consumer = functionNewValue(f, OpAdd, I32, 0, 0, Span<const i32>({user, source}));
                f->blocks[0].instrs = {user, consumer};
                // Two edges and a return share the same cells; in the first
                // case those cells are also the instruction's pool operands.
                Slice<i32> shared = crossPage ? sliceOf<i32>({user, source}) : borrowSlice(&f->pool[usize(f->values[consumer].args)], 2);
                f->blocks[0].term = Term{TermIf, user, sliceOf<Edge>({Edge{1, shared}, Edge{1, shared}}), {}, {}};
                f->blocks[1].term = Term{TermReturn, {}, {}, shared, {}};
                frontendResolveAliases(fe);
                require(fe->alias.size() == 0, "frontend alias scratch released before optimization");
                require(f->values.size() == 7 && f->values.pages.size() == 1, "frontend phi records release excess pages");
                require(f->values[1].op == OpConst && f->values[1].aux == 0 && f->values[4].aux == 17, "frontend phi compaction keeps non-phi record order");
                require(f->blocks[0].instrs[0] == 5 && f->blocks[0].instrs[1] == 6 && f->blocks[1].params.count == 2 && f->blocks[1].params[0] == 2 && f->blocks[1].params[1] == 3, "frontend phi compaction rewrites definitions");
                const auto args = functionArgs(f, 5);
                require(args.count == 2 && args[0] == 4 && args[1] == 0, "frontend phi aliases map operands across pages");
                const auto sharedOperands = functionArgs(f, 6);
                require(sharedOperands.count == 2 && sharedOperands[0] == 5 && sharedOperands[1] == 4, "frontend phi compaction keeps shared pool references");
                require(f->blocks[0].term.value == 5 && f->blocks[0].term.edges[0].args[0] == 5 && f->blocks[0].term.edges[1].args[0] == 5 && f->blocks[1].term.values[0] == 5, "frontend phi compaction rewrites shared operand storage once");
                require(f->blocks[1].preds.count == 2 && f->blocks[1].preds[0] == 0 && f->blocks[1].preds[1] == 0, "frontend phi compaction retains parallel predecessors");
            }
        }
        {
            ObjPool pool;
            PoolScope scope(pool);
            auto* f = pool.make<Function>();
            auto* o = pool.make<Optimizer>();
            o->f = f;
            for (int i = 0; i < 4; ++i) {
                functionNewBlock(f);
            }
            const i32 input = functionNewValue(f, OpParam, I32, 0, 0, {});
            f->blocks[0].params = sliceOf<i32>({input});
            Array<i32, 32> constants;
            Array<i32, 32> roots;
            auto emit = [&](i32 block, Op op, i32 a, i32 b) {
                const i32 v = functionNewValue(f, op, I32, block, 0, Span<const i32>({a, b}));
                f->blocks[block].instrs.pushBack(v);
                return v;
            };
            for (i32 i = 0; i < 32; ++i) {
                constants[i] = functionNewValue(f, OpConst, I32, -1, i + 1, {});
                roots[i] = emit(0, OpAdd, input, constants[i]);
            }
            i32 first = -1;
            for (i32 i = 0; i < 32; ++i) {
                first = emit(1, OpMul, input, constants[i]);
            }
            const i32 rootCopy = emit(1, OpAdd, input, constants[17]);
            const i32 second = emit(2, OpMul, input, constants[31]);
            const i32 third = emit(3, OpMul, input, constants[31]);
            const i32 thirdCopy = emit(3, OpMul, input, constants[31]);
            const i32 merged = functionNewValue(f, OpParam, I32, 3, 0, {});
            const i32 shared = functionNewValue(f, OpParam, I32, 3, 1, {});
            f->blocks[3].params = sliceOf<i32>({merged, shared});
            f->blocks[0].term = Term{TermIf, input, sliceOf<Edge>({Edge{1, {}}, Edge{2, {}}}), {}, {}};
            f->blocks[1].term = Term{TermJump, {}, sliceOf<Edge>({Edge{3, sliceOf<i32>({first, rootCopy})}}), {}, {}};
            f->blocks[2].term = Term{TermJump, {}, sliceOf<Edge>({Edge{3, sliceOf<i32>({second, roots[17]})}}), {}, {}};
            f->blocks[3].term = Term{TermReturn, {}, {}, sliceOf<i32>({third, thirdCopy, merged, shared}), {}};
            functionComputePreds(f);
            optimizerCommonExpressions(o);
            require(optimizerResolve(o, rootCopy) == roots[17] && f->blocks[1].term.edges[0].args[1] == roots[17], "CSE retains dominating expressions after growth");
            require(optimizerResolve(o, second) == second, "CSE restores sibling scope after growth");
            require(optimizerResolve(o, third) == third, "CSE does not merge sibling definitions");
            require(f->blocks[3].term.values[0] == third && f->blocks[3].term.values[1] == third, "CSE merges expressions within a scope");
        }
        {
            ObjPool pool;
            PoolScope scope(pool);
            auto* f = pool.make<Function>();
            for (i32 b = 0; b < 4; ++b) {
                functionNewBlock(f);
            }
            const i32 input = functionNewValue(f, OpParam, I32, 0, 0, {});
            const i32 one = functionNewValue(f, OpConst, I32, -1, 1, {});
            const i32 value = functionNewValue(f, OpAdd, I32, 1, 0, Span<const i32>({input, one}));
            f->blocks[0].params = sliceOf<i32>({input});
            f->blocks[0].term = Term{TermIf, input, sliceOf<Edge>({Edge{1, {}}, Edge{3, {}}}), {}, {}};
            f->blocks[1].instrs.pushBack(value);
            f->blocks[1].term = Term{TermIf, input, sliceOf<Edge>({Edge{1, {}}, Edge{2, {}}}), {}, {}};
            f->blocks[2].term = Term{TermReturn, {}, {}, sliceOf<i32>({one}), {}};
            f->blocks[3].term = Term{TermReturn, {}, {}, sliceOf<i32>({one}), {}};
            MemoryLoop loop;
            loop.f = f;
            loop.header = 1;
            loop.members = makeSlice<bool>(4);
            loop.members[1] = true;
            loop.ci = functionAnalyze(f);
            require(!memoryLoopHasLiveOut(&loop), "loop without direct live-out values");
            f->blocks[2].term.values[0] = value;
            require(memoryLoopHasLiveOut(&loop), "loop value used by a dominated exit");
            const i32 merged = functionNewValue(f, OpParam, I32, 2, 0, {});
            f->blocks[2].params = sliceOf<i32>({merged});
            f->blocks[2].term.values[0] = merged;
            f->blocks[1].term.edges[1].args = sliceOf<i32>({value});
            f->blocks[3].term = Term{TermJump, {}, sliceOf<Edge>({Edge{2, sliceOf<i32>({one})}}), {}, {}};
            loop.ci = functionAnalyze(f);
            require(!cfgInfoDominates(loop.ci, 1, 2) && !memoryLoopHasLiveOut(&loop), "loop value merged with an entry bypass");
        }
        {
            ObjPool pool;
            PoolScope scope(pool);
            auto* f = pool.make<Function>();
            auto* o = pool.make<Optimizer>();
            o->f = f;
            for (int i = 0; i < 4; ++i) {
                f->blocks.pushBack(Block{});
            }
            f->blocks[0].term.edges = sliceOf<Edge>({Edge{3, {}}, Edge{1, {}}, Edge{3, {}}});
            f->blocks[1].term.edges = sliceOf<Edge>({Edge{3, {}}});
            f->blocks[2].term.edges = sliceOf<Edge>({Edge{3, {}}});
            f->blocks[3].preds = sliceOf<i32>({2, 0, 2, 1, 0});
            for (int pass = 0; pass < 3; ++pass) {
                if (pass == 1) {
                    o->incomingEpoch = 0xffffffffu;
                } else if (pass == 2) {
                    f->blocks.pushBack(Block{});
                }
                auto incoming = optimizerIncoming(o, 3);
                require(incoming.size() == 4, "incoming parallel edges");
                require(incoming[0] == &f->blocks[2].term.edges[0], "incoming first predecessor");
                require(incoming[1] == &f->blocks[0].term.edges[0] && incoming[2] == &f->blocks[0].term.edges[2], "incoming edge order");
                require(incoming[3] == &f->blocks[1].term.edges[0], "incoming last predecessor");
            }
        }
        for (bool appendAfter : {false, true}) {
            for (bool outsideBody : {false, true}) {
                ObjPool pool;
                PoolScope scope(pool);
                auto build = [&](bool indexed) {
                    auto* f = pool.make<Function>();
                    for (i32 i = 0; i < 4; ++i) {
                        functionNewBlock(f);
                    }
                    const i32 one = functionNewValue(f, OpConst, I32, -1, 1, {});
                    const i32 value = functionNewValue(f, OpParam, I32, 1, 0, {});
                    functionNewValue(f, OpAdd, I32, 1, 0, Span<const i32>({value, one}));
                    const i32 increment = functionNewValue(f, OpAdd, I32, 1, 0, Span<const i32>({value, one}));
                    f->blocks[0].term = Term{TermJump, {}, sliceOf<Edge>({{1, sliceOf<i32>({one})}}), {}, {}};
                    f->blocks[1].params = sliceOf<i32>({value});
                    f->blocks[1].instrs.pushBack(increment);
                    f->blocks[1].term = Term{TermIf, value, sliceOf<Edge>({{1, sliceOf<i32>({increment})}, {2, {}}}), {}, {}};
                    if (outsideBody) {
                        const i32 user = functionNewValue(f, OpAdd, I32, 2, 0, Span<const i32>({increment, one}));
                        f->blocks[2].instrs.pushBack(user);
                        f->blocks[2].term = Term{TermReturn, {}, {}, borrowSlice(&f->pool[usize(f->values[user].args)], 1), {}};
                    } else {
                        f->blocks[2].term = Term{TermReturn, {}, {}, sliceOf<i32>({increment}), {}};
                    }
                    functionNewValue(f, OpConst, I32, 3, 7, {});
                    functionNewValue(f, OpAdd, I32, 1, 0, Span<const i32>({value, one}));
                    LoopBodyUses uses(f);
                    uses.clones = 4;
                    MemoryLoop loop;
                    loop.f = f;
                    loop.header = 1;
                    loop.exit = 2;
                    loop.order = sliceOf<i32>({1});
                    loop.members = makeSlice<bool>(4);
                    loop.members[1] = true;
                    loop.bodyUses = indexed ? &uses : nullptr;
                    i32 appended = -1;
                    for (i32 copy = 0; copy < 2; ++copy) {
                        if (copy && appendAfter) {
                            appended = functionNewValue(f, OpAdd, I32, 3, 0, Span<const i32>({increment, one}));
                            f->blocks[3].instrs.pushBack(appended);
                        }
                        memoryLoopClone(&loop, {}, {}, {});
                    }
                    require(f->blocks[2].params.count == (appendAfter ? 2u : 1u), "loop exit parameter preserved across copies");
                    require(f->blocks[2].term.values[0] == f->blocks[2].params[0], "loop exit operand rewritten");
                    if (appendAfter) {
                        require(functionArgs(f, appended)[0] == f->blocks[2].params[1], "uses appended to a previously indexed block");
                    }
                    return f;
                };
                Function* original = build(false);
                Function* indexed = build(true);
                auto same = [](const auto& left, const auto& right) {
                    require(length(left) == length(right), "indexed loop collection size");
                    for (Int i = 0; i < length(left); ++i) {
                        require(left[i] == right[i], "indexed loop collection values");
                    }
                };
                same(original->values, indexed->values);
                same(original->pool, indexed->pool);
                require(original->blocks.size() == indexed->blocks.size(), "indexed loop block count");
                for (usize b = 0; b < original->blocks.size(); ++b) {
                    const auto& left = original->blocks[b];
                    const auto& right = indexed->blocks[b];
                    same(left.params, right.params);
                    same(left.instrs, right.instrs);
                    require(left.dead == right.dead && left.term.kind == right.term.kind && left.term.value == right.term.value && left.term.aux == right.term.aux, "indexed loop block metadata");
                    same(left.term.values, right.term.values);
                    require(left.term.edges.count == right.term.edges.count, "indexed loop edge count");
                    for (Int i = 0; i < length(left.term.edges); ++i) {
                        require(left.term.edges[i].block == right.term.edges[i].block, "indexed loop edge target");
                        same(left.term.edges[i].args, right.term.edges[i].args);
                    }
                }
            }
        }
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
