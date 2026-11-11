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

    enum class ChaosFault: u32 { Read, Write, Grow, Count };
#ifdef AOT_CHAOS
    static u32 chaosSkips[u32(ChaosFault::Count)]{};
    static bool chaosArmed[u32(ChaosFault::Count)]{};

    static bool chaosFail(ChaosFault point) {
        const u32 index = u32(point);
        if (!chaosArmed[index]) {
            return false;
        }
        if (chaosSkips[index] != 0) {
            --chaosSkips[index];
            return false;
        }
        chaosArmed[index] = false;
        return true;
    }
#else
    static constexpr bool chaosFail(ChaosFault) {
        return false;
    }
#endif
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
            if (chaosFail(ChaosFault::Write) || wasiFdWrite(fd, &vector, 1, &done) != 0 || done == 0) {
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
#if defined(__wasm__) || defined(wasm)
        void adoptPages(void* data, usize bytes);
#endif

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
    if (chaosFail(ChaosFault::Grow) || __builtin_wasm_memory_grow(0, first - before + heapPagesPerSegment) == ~usize(0)) {
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
    void* got = chaosFail(ChaosFault::Grow) ? MAP_FAILED : ::mmap(wanted, heapSegmentBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
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

#if defined(__wasm__) || defined(wasm)
void Heap::adoptPages(void* data, usize bytes) {
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
}
#endif

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
        void grow(usize size);
        void resize(usize size);
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

// The capacity test stays small enough to be inlined where buffers grow
// element by element; growth itself is out of line.
inline void Buffer::reserve(usize size) {
    if (size > capacity()) {
        grow(size);
    }
}

__attribute__((noinline)) void Buffer::grow(usize size) {
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

inline void Buffer::resize(usize size) {
    reserve(size);
    if (size != 0 || header() != &emptyBuffer) {
        header()->used = size;
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

        constexpr initializer_list()
            : first(nullptr)
            , count(0)
        {
        }

        const T* begin() const {
            return first;
        }

        const T* end() const {
            return first + count;
        }

        usize size() const {
            return count;
        }

    private:
        constexpr initializer_list(const T* start, usize length)
            : first(start)
            , count(length)
        {
        }
    };
}

namespace {
    // All containers and inputs are bounded by INT32_MAX. Wasm scalar
    // values and ELF sizes use their explicit i64/u64 types.
    using Int = i32;
    using Uint = u32;

    // Named traits also work in the constraints of GCC's coverage build.
    template <typename A, typename B>
    static constexpr bool sameType = __is_same(A, B);

    template <typename T>
    static constexpr bool constType = false;

    template <typename T>
    static constexpr bool constType<const T> = true;

    template <typename T>
    static constexpr bool scalarType = __is_pointer(T) || __is_enum(T) || sameType<T, bool> || sameType<T, char> || sameType<T, signed char> || sameType<T, unsigned char> || sameType<T, short> || sameType<T, unsigned short> || sameType<T, int> || sameType<T, unsigned> || sameType<T, long> || sameType<T, unsigned long> || sameType<T, long long> || sameType<T, unsigned long long> || sameType<T, wchar_t> || sameType<T, char8_t> || sameType<T, char16_t> || sameType<T, char32_t> || sameType<T, float> || sameType<T, double> || sameType<T, long double>;
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
            return kind == Kind::Contiguous ? static_cast<T*>(storage) : nullptr;
        }

        PageVec<T>* pages() const {
            return kind == Kind::Paged ? static_cast<PageVec<T>*>(storage) : nullptr;
        }

        static void reset(T* values, usize count) {
            if (!count) {
                return;
            }
            if constexpr (scalarType<T>) {
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
            if (u64(size) * sizeof(T) > pageBytes - sizeof(ObjPool::Chunk)) {
                kind = Kind::Paged;
                auto* values = owner->make<PageVec<T>>();
                storage = values;
                values->resize(usize(size));
            } else {
                storage = owner->allocate(usize(size) * sizeof(T), alignof(T));
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

        // Contiguous storage is pool memory. Pool destruction owns a paged
        // container; rehash can release its pages earlier without removing
        // its disposal record from the pool.
        void releaseStorage() {
            if (kind == Kind::Paged) {
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
        } else if (u64(size) * sizeof(T) <= pageBytes - sizeof(ObjPool::Chunk)) {
            // Slice growth creates another backing array, so one that fits a
            // pool chunk is pool memory and needs no disposal.
            array = owner.make<ArrayData<T>>(&owner);
            array->storage = owner.allocate(usize(size) * sizeof(T), alignof(T));
        } else {
            auto* backing = owner.make<ArrayStorage<T, PageVec<T>>>(&owner, ArrayData<T>::Kind::Paged);
            backing->values.resize(usize(size));
            array = &backing->array;
        }
        array->count = size;
        array->clearElements(clearFrom);
        return array;
    }

    // A view can be copied freely. Its storage belongs to exactly one ObjPool.
    // Backing arrays are fixed after creation: those that fit a pool chunk
    // are Contiguous pool memory, and only Paged storage needs a container
    // lookup. A contiguous view holds the address of its first element, so
    // reading one is a single load; a paged view holds its first index
    // instead, and the low bit of its array pointer tells them apart.
    template <typename T>
    struct Slice {
        uptr tagged = 0;

        union {
            T* data = nullptr;
            u32 offset;
        };

        u32 count = 0;
        u32 space = 0;

        static Slice of(ArrayData<T>* array, u32 first, u32 count, u32 space) {
            Slice s;
            if (array && array->kind == ArrayData<T>::Kind::Paged) {
                s.tagged = reinterpret_cast<uptr>(array) | 1;
                s.offset = first;
            } else {
                s.tagged = reinterpret_cast<uptr>(array);
                s.data = array ? static_cast<T*>(array->storage) + first : nullptr;
            }
            s.count = count;
            s.space = space;
            return s;
        }

        ArrayData<T>* array() const {
            return reinterpret_cast<ArrayData<T>*>(tagged & ~uptr(1));
        }

        bool paged() const {
            return tagged & 1;
        }

        T& unchecked(u32 i) const {
            if (!paged()) {
                return data[i];
            }
            const usize at = usize(offset) + i;
            return static_cast<PageVec<T>*>(array()->storage)->pages.data()[at / PageVec<T>::itemsPerPage][at % PageVec<T>::itemsPerPage];
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
            : contiguous(source.paged() ? nullptr : source.data)
            , pages(source.paged() ? source.array()->pages() : nullptr)
            , offset(source.paged() ? source.offset : 0)
            , count(source.count)
        {
        }

        Span(InitialValues<Value> source) requires(constType<T>)
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
        Span(Span<U> source) requires(sameType<T, const U>)
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
        return !s.tagged;
    }

    template <typename T>
    static bool isNil(T* p) {
        return p == nullptr;
    }

    template <typename T>
    static Slice<T> makeSlice(Int size, Int space) {
        require(size >= 0 && space >= size && Uint(space) <= 0x7fffffff, "invalid slice size");
        auto* data = makeArrayData<T>(*activePool, space);
        return Slice<T>::of(data, 0, u32(size), u32(space));
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
        return Slice<T>::of(data, 0, u32(size), u32(size));
    }

    template <typename T>
    static Slice<T> subslice(Slice<T> s, Int low, Int high, Int space) {
        require(low >= 0 && high >= low && space >= high && Uint(space) <= s.space, "slice bounds");
        Slice<T> result = s;
        if (s.paged()) {
            result.offset = s.offset + u32(low);
        } else if (low) {
            result.data = s.data + low;
        }
        result.count = u32(high - low);
        result.space = u32(space - low);
        return result;
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
        ObjPool* owner = s.tagged ? s.array()->owner : activePool;
        PoolScope scope(*owner);
        Int space = s.space ? s.space * 2 : 4;
        if (space < need) {
            space = need;
        }
        auto* data = makeArrayData<T>(*owner, space, s.count);
        Slice<T> result = Slice<T>::of(data, 0, s.count, u32(space));
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

    };

    static Int length(String s) {
        return s.count;
    }

    static String asString(Slice<u8> s) {
        if (!s.count) {
            return {};
        }
        if (!s.paged()) {
            return {s.data, s.count};
        }
        const usize first = s.offset / pageBytes;
        if ((usize(s.offset) + s.count - 1) / pageBytes == first) {
            return {s.array()->pages()->pages.data()[first] + s.offset % pageBytes, s.count};
        }
        require(s.count <= pageBytes - 32, "large internal string");
        auto* p = static_cast<u8*>(activePool->allocate(s.count, 1));
        for (u32 i = 0; i < s.count; ++i) {
            p[i] = s[i];
        }
        return {p, s.count};
    }

    static Slice<u8> appendSlice(Slice<u8> out, String text) {
        growSlice(out, text.count);
        for (usize i = 0; i < text.count; ++i) {
            out.unchecked(out.count++) = text.data[i];
        }
        return out;
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
        return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
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

    struct Table;

    struct TableFixup;

    struct TableMeta;

    struct TailRangeGroup;

    struct TailRangeRegion;

    struct Term;

    struct TextAddress;

    struct TextReloc;

    struct TrapFixup;

    struct ValueRange;

    struct ValueRanges;

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

    // allocatorLiveness creates all three sets with the same nv. The local
    // uses are already in in; add out & ~kill without a dense temporary.
    static bool bitsetPropagate(Bitset in, Bitset out, Bitset kill) {
        if (in.data->bits <= denseBitsetLimit) {
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
        bool changed = false;
        visitBitsetWords(out, [&](usize index, u64 word) {
            if (bitsetAddSparseWord(in.data, index, word & ~bitsetSparseWord(kill.data, index))) {
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

    template <typename L, typename S>
    struct LessSwap {
        L Less;
        S Swap;
    };

    // LineMark says that from pc on the code is of the wasm instruction at
    // pos, an address of the module's line table (moduleReadLines); LineRow
    // is a row of that table, the source line of the code from address on,
    // or its end when end is set; LineEntry is what the object records: a
    // file and line from a pc of the text on.
    struct LineMark {
        i32 pc{};
        u32 pos{};
    };

    struct LineRow {
        u32 address{};
        u32 file{};
        u32 line{};
        bool end{};
    };

    struct LineEntry {
        u32 pc{};
        u32 file{};
        u32 line{};
    };

    struct Region {
        String name{};
        u32 start{};
        u32 end{};
        Slice<SpEntry> sp{};
        Slice<LineMark> lines{};
    };

    struct Anonymous49 {
        i32 base{};
        i32 base2{};
        i32 stride{};
        u64 mul{};
        u64 mul2{};
        u64 factor{};
    };

    struct SpEntry {
        i32 pc{};
        i32 delta{};
    };

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

    struct FloatRanges {
        Function* f{};
        Map<i32, FloatRange> facts{};
        Map<i32, FloatRange> cache{};
    };

    struct Move {
        VReg dst{};
        VReg src{};
    };

    // A RegMove of size 4 zero-extends a 32-bit value.
    struct RegMove {
        VReg dst{};
        VReg src{};
        u8 size{};
    };

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
        // A term that may be negative: its guard sums the terms sign
        // extended and requires the span to begin at or above 0, and the
        // fast copy keeps the wrapping additions of the address.
        bool signedTerms{};
    };

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
        // asan compiles every access with a check of the shadow map
        // (lowererAsanAddress) instead of a bounds check.
        bool asan{};
        // codeOffset is where the code section's contents begin in the
        // module, which DWARF's addresses count from; debugLine, debugLineStr
        // and debugStr are its DWARF sections, when it has them, and
        // sourceFiles and lineRows what moduleReadLines made of them.
        Int codeOffset{};
        Slice<u8> debugLine{};
        Slice<u8> debugLineStr{};
        Slice<u8> debugStr{};
        Slice<String> sourceFiles{};
        Slice<LineRow> lineRows{};
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
        // Emission positions of blocks, the last position control falls into
        // from the end of each position, and the final target of each block
        // that only forwards (-1 for others): see objCodeEmitFunction.
        Slice<Int> positions{};
        Vec<Int> reach;
        Vec<i32> forward;
        // shorts and trapShorts tell, in the order of the jumps to blocks
        // and to traps, which take the short form: the first emission of a
        // function finds them, a second uses them. aligned are the starts of
        // the aligned loop headers and padded their padding; trapTo is where
        // each trap jump goes.
        Slice<u8> shorts{};
        Slice<u8> trapShorts{};
        Vec<i32> aligned;
        Vec<i32> padded;
        Vec<i32> trapTo;
        // spans are the sizes of the aligned loops of the first emission,
        // from the header to the end of the last block that returns to it,
        // in the order of their headers; plan is those the second uses.
        Vec<i32> spans;
        Slice<i32> plan{};
    };

    // alignPad is the padding before a loop header at pos: to 32 bytes, and
    // to 64 when the loop, of span bytes, fits in 64 but would cross a
    // 64-byte boundary: the front end fetches 64-byte blocks.
    static i32 alignPad(i32 pos, i32 span) {
        i32 pad = -pos & 31;
        if (span > 0 && span <= 64 && ((pos + pad) & 63) + span > 64) {
            pad += 32;
        }
        return pad;
    }

    struct Amem {
        Int base{};
        Int index{};
        u8 scale{};
        i32 disp{};
        i32 label{};
        u8 labelKind{};
    };

    struct OrigOperand {
        VReg vreg{};
        VReg loc{};
        Int role{};
    };

    struct CheckedValue {
        i32 value{};
        i64 end{};
    };

    struct TextReloc {
        i32 at{};
        i32 sym{};
        i64 addend{};
    };

    struct Term {
        TermKind kind{};
        i32 value{};
        Slice<Edge> edges{};
        Slice<i32> values{};
        i64 aux{};
        u32 pos{};
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
        // memUses are the uses an instruction can read from memory: they
        // need no register, but count as uses for weighing the value.
        InlineVec<i32, 1> memUses{};
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

    static_assert(sizeof(Interval) == (sizeof(void*) == 8 ? 128 : 100));

    struct Anonymous50 {
        i32 begin{};
        i32 end{};
    };

    struct ParamIndex {
        i32 index = 0;
        u32 epoch = 0;
    };

    struct Optimizer {
        Function* f{};
        Vec<i32> alias{};
        Map<ConstKey, i32> consts{};
        Vec<i32> uses{};
        // counted reports that uses matches the function; changes to operands,
        // live blocks or instruction lists clear it.
        bool counted = false;
        // Replacements made and replacements applied to the operands.
        u32 replaced = 0;
        u32 applied = 0;
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
        // The variable compared with the limit advances by startStep, times
        // the invariant startStride when that names a value; inclusive loops
        // continue at the limit, exact ones until the variable equals it.
        i64 startStep{};
        i32 startStride = -1;
        bool inclusive{};
        bool exact{};
        Map<i32, LoopSpan> induction{};
        Map<i32, LoopSpan> cache{};
        Map<i32, bool> noWrap{};
        ValueRanges* valueRanges{};
        bool allowLoads{};
        Map<i32, bool> invariantCache{};
        Slice<i32> invariantNodes{};
        LoopBodyUses* bodyUses = nullptr;
    };

    struct PredRef {
        i32 block{};
        i32 edge{};
    };

    struct Layout {
        i32 memoryEnd{};
        i32 tailProfile{};
        // shadow is the slot holding the shadow map's base, in an asan object.
        i32 shadow{};
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

    struct LoadRangeLoop_version741 {
        i32 begin{};
        i32 end{};
    };

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

    struct Asm {
        PageVec<u8> buf{};
        Slice<RipFixup> fixups{};
        i32 sp{};
        Slice<SpEntry> sps{};
        // lines marks where the code of another wasm instruction begins,
        // lastPos the instruction the code is of (asmMarkPos).
        Slice<LineMark> lines{};
        u32 lastPos{};
    };

    struct Anonymous93 {
        i32 b{};
        Int i{};
    };

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

    struct Memory {
        Limits limits{};
        bool imported{};
    };

    struct TrapFixup {
        i32 at{};
        i64 code{};
        i32 sp{};
        bool small{};
        // An asan trap reports the access: its address is reg + disp, or
        // disp alone without a register, of width bytes, kind a load, a
        // store or a range (asanLoad and kin).
        VReg reg = -1;
        i32 disp{};
        u8 width{};
        u8 kind{};
        // pos is the wasm instruction of the check, which its pad is of
        // too, placed after the function's code as it is.
        u32 pos{};
    };

    struct Anonymous68 {
        i32 block{};
        Int edge{};
    };

    struct Lowerer {
        Module* m{};
        Function* f{};
        Layout* lay{};
        CfgInfo* ci{};
        MFunc* mf{};
        Vec<VReg> vregs{};
        Vec<i32> uses{};
        Vec<bool> deferred{};
        // foldable marks a deferred load its only user may read as a memory
        // operand (lowererFoldedLoad).
        Vec<bool> foldable{};
        // rmw holds, for a store that updates what a load read from its
        // address, the value it stores, or -1 (lowererMarkUpdates).
        Vec<i32> rmw{};
        // rmwFlags marks a block's branch condition that tests a value an
        // update in place stored: the update's flags decide the branch.
        Vec<bool> rmwFlags{};
        // bounded is, for the address of an access proven in bounds in the
        // block being lowered, that block: the guard that proved it covers
        // the block's other accesses too, so there a sum of the address and
        // a constant within boundSlack does not wrap.
        Vec<i32> bounded{};
        i32 block = -1;
        Vec<i32> mblk{};
        i32 cur{};
        Map<Array<i32, 2>, i32> split{};
        Map<i32, i64> checked{};
        Slice<CheckUndo> checkUndo{};
        Slice<Slice<i32>> floatHomes{};
        Vec<bool> rawTrunc{};
        ValueRanges* addresses{};
        ObjCode* oc{};
        // pos is the position of the value being lowered (Function
        // positions), stamped on the instructions it emits.
        u32 pos{};
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

    struct VisitMir346 {
        i32 block{};
        Int edge{};
    };

    struct Reader {
        Slice<u8> data{};
        Int pos{};
    };

    struct Anonymous10 {
        i32 b{};
        Int i{};
    };

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
        // pos is the address of the instruction being decoded in the
        // module's line table: its offset in the code section's contents.
        u32 pos{};
    };

    struct Export {
        String name{};
        u8 kind{};
        u32 index{};
    };

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

    struct JumpFixup {
        i32 at{};
        i32 block{};
        // small jumps have an 8-bit displacement at at, others 32 bits.
        bool small{};
    };

    struct MemoryMeta {
        u64 min{};
        u64 max{};
        bool imported{};
        u64 tail{};
        u32 tailEnd{};
        u32 profile{};
        u32 profileFuncs{};
        // shadow is the instance slot of the shadow map's base: an asan
        // object, which has no tail.
        u32 shadow{};
    };

    struct ValueRange {
        i64 lo{};
        i64 hi{};
    };

    struct AddressExpression {
        Array<AddressTerm, 4> terms{};
        u32 offset{};
    };

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
        // invariantDepth is, for a virtual register defined once, the loop
        // depth of its definition's block, and for any other a depth no
        // block exceeds.
        Vec<i32> invariantDepth{};
        // A virtual register whose one definition loads a float constant is
        // constant, with the constant's bits. Its spill slot is a constant
        // slot, numbered from constantSlot, the constant of constantSlots:
        // it takes no room in the frame, nothing is saved to it, and the
        // emitter reads the constant where it would read the slot.
        Vec<bool> constant{};
        Vec<u64> constantBits{};
        Vec<u64> constantSlots{};
        // definitions counts each virtual register's definitions.
        Vec<i32> definitions{};
        // home is, for a value split at a loop header that many blocks
        // return to, its part from there: a later part of the value takes
        // that part's register while it is free, and evicts its holder when
        // it can, so that the value returns to the header where it left it.
        Vec<Interval*> home{};
    };

    static constexpr i32 constantSlot = 1 << 24;

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
        // files and lines are the source lines of the text, from the
        // module's DWARF line table: each entry a file and a line from a pc
        // on (objCodeLines).
        Slice<String> files{};
        Slice<LineEntry> lines{};
    };

    struct CfgInfo {
        Slice<i32> order{};
        Slice<i32> rpoIndex{};
        Slice<i32> idom{};
        Slice<i32> depth{};
        Slice<bool> header{};
        // For loop versioning, the dominator tree in preorder: a block
        // dominates the blocks from its own place there up to the end of its
        // subtree. Unreachable blocks that are not dead follow the tree.
        Slice<i32> preorder{};
        Slice<i32> enter{};
        Slice<i32> leave{};
        // Loop versioning adds blocks after the analysis; each is entered
        // only from the preheader recorded here.
        Slice<i32> added{};
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

    struct ValueRanges {
        Function* f{};
        Slice<ValueRange> cache{};
        Slice<bool> seen{};
    };

    struct PendingPhi {
        i32 local{};
        i32 phi{};
    };

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

    struct FrameCfg34 {
        i32 b{};
        Int i{};
    };

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

    struct CheckedEdge {
        i32 pred{};
        Edge* edge{};
    };

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
        // asan checks every access against the shadow map the loader keeps
        // beside the memory, a byte for each byte of it (lowererAsanAddress);
        // it excludes the tail.
        bool asan{};
    };

    struct FloatIntegerNarrow {
        Optimizer* o{};
        Slice<u8> valid{};
        Map<i32, i32> values{};
    };

    struct TextAddress {
        i32 at{};
        i32 target{};
    };

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
        // positions holds, for each value, the address of the wasm
        // instruction it came from in the module's line table, or 0 for a
        // value a pass made (frontendEmit).
        PageVec<u32> positions{};
        PageVec<i32> pool{};
        ObjPool blockPool;
        NodeVec<Block> blocks;
        // Only Wasm loops make cycles; the frontend notes whether it saw one.
        bool hasLoops = false;
        // An analysis of the current graph, with up-to-date predecessors, or
        // null once a pass may have changed edges since it was made.
        CfgInfo* cfg = nullptr;

        Function();
    };

    struct Import {
        String module{};
        String name{};
        u8 kind{};
        u32 index{};
    };

    struct ExportMeta {
        u8 kind{};
        String name{};
        u32 index{};
        String sym{};
        Slice<ValType> params{};
        Slice<ValType> results{};
    };

    struct CallFixup {
        i32 at{};
        i32 fn{};
    };

    struct Anonymous24 {
        Int index{};
        String name{};
    };

    struct GoLoc {
        VReg reg{};
        i32 off{};
    };

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
        Slice<CallFixup> calls{};
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
        // importSyms are the imported functions' symbols by function index,
        // importIntrinsics the memory functions among them, importCode
        // those with code, an adapter, that a pointer can reach.
        Slice<String> importSyms{};
        Slice<Int> importIntrinsics{};
        Slice<bool> importCode{};
    };

    struct Table {
        ValType elem{};
        Limits limits{};
        bool imported{};
    };

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
        // pos is the wasm instruction the instruction is of, in the module's
        // line table, or 0 for one of the lowering's own (lowererEmit).
        u32 pos{};
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

    static constexpr Int offMem = 0;

    static constexpr Int offMemSize = 8;

    static constexpr Int offMemBound = 16;

    static constexpr Int offStackLimit = 24;

    static constexpr Int offHost = 32;

    static constexpr Int offTokens = 40;

    static constexpr Int offJump = 48;

    static constexpr Int jumpPC = 0;

    static constexpr Int jumpSP = 1;

    static constexpr Int jumpBP = 2;

    // jumpRegs holds the callee-saved registers, calleeSaved's in order.
    static constexpr Int jumpRegs = 3;

    static constexpr Int jumpToken = 8;

    static constexpr Int jumpWords = 9;

    static constexpr Int offBase = offJump + 8 * jumpWords;

    static constexpr Int offInterrupt = offBase + 8;

    static constexpr Int headerSize = offInterrupt + 8;

    static constexpr Int boundSlack = 1024;

    static constexpr Int tailCounterBytes = 32;

    static constexpr Int tableEntries = 0;

    static constexpr Int tableSize = 8;

    static constexpr Int tableMax = 12;

    static constexpr Int tableBytes = 16;

    static constexpr Int descBytes = 16;

    static constexpr Int labelLocal = 0;

    static constexpr Int labelModule = 1;

    static constexpr Int labelFunc = 2;

    static constexpr Int labelGot = 3;

    static constexpr Int ppNone = 0;

    static constexpr Int pp66 = 1;

    static constexpr Int ppF3 = 2;

    static constexpr Int ppF2 = 3;

    static constexpr Int shtProgbits = 1;

    static constexpr Int shtSymtab = 2;

    static constexpr Int shtStrtab = 3;

    static constexpr Int shtRela = 4;

    static constexpr Int shtNote = 7;

    static constexpr Int shtNobits = 8;

    static constexpr Int shfAlloc = 2;

    static constexpr Int shfExec = 4;

    static constexpr Int shfInfoLink = 64;

    static constexpr Int rX86_64_64 = 1;

    static constexpr Int rX86_64_PC32 = 2;

    static constexpr Int rX86_64_GOTPCREL = 9;

    static constexpr Int rX86_64_GOTPCRELX = 41;

    static constexpr Int rX86_64_REX_GOTPCRELX = 42;

    static const String noteOwner = String("wasm2o");

    static constexpr Int inExternal = 0;

    static constexpr Int inText = 1;

    static constexpr Int inRodata = 2;

    static constexpr Int ctrlBlock = 0;

    static constexpr Int ctrlLoop = 1;

    static constexpr Int ctrlIf = 2;

    static constexpr Int ctrlElse = 3;

    static constexpr Int ctrlFunc = 4;

    static constexpr Int gStackHi = 8;

    static constexpr Int gStackguard0 = 16;

    // gInstance is the field of g our runtime adds (lib/goruntime): the
    // instance the goroutine runs, which the code reads after a call of Go.
    static constexpr Int gInstance = 472;

    static constexpr Int stackSmall = 128;

    static constexpr Int leafFrame = 512;

    static constexpr Int resumeSlack = 512;

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

    static constexpr Int trapUnreachable = 1;

    static constexpr Int trapMemoryOOB = 2;

    static constexpr Int trapIntOverflow = 3;

    static constexpr Int trapDivByZero = 4;

    static constexpr Int trapConversion = 5;

    static constexpr Int trapIndirectType = 6;

    static constexpr Int trapUndefinedElem = 7;

    static constexpr Int trapUninitElem = 8;

    static constexpr Int trapStack = 9;

    static constexpr Int trapTableOOB = 10;

    static constexpr Int trapNullRef = 11;

    // trapAsan is an access of memory the shadow map marks inaccessible; the
    // trap reports the access's address, width and kind: a load, a store, or
    // a range a bulk operation or a string function would read or write.
    static constexpr Int trapAsan = 12;

    static constexpr u8 asanLoad = 0;

    static constexpr u8 asanStore = 1;

    static constexpr u8 asanRange = 2;

    // shadowOK is the shadow map's byte of an accessible byte of memory:
    // every other value is inaccessible, so a check compares the shadow of
    // the access's bytes with all ones (funcEmitterShadow).
    static constexpr i64 shadowOK = -1;

    static constexpr Int calleeFunc = 0;

    static constexpr Int calleeJump = 2;

    // calleeGo calls a Go function of the VM, imm the index of its extern
    // symbol: an import's. calleeStr calls a function of lib/str, imm its
    // symbol's index, as the module's functions call each other, with host
    // addresses.
    static constexpr Int calleeGo = 3;

    static constexpr Int calleeStr = 4;

    // The imports lib/c's memory and string functions call, __kernel_memcpy
    // and the like, which the code compiles itself: it checks their ranges
    // and calls lib/str's.
    static constexpr Int intrinsicNone = 0;

    static constexpr Int intrinsicMemcpy = 1;

    static constexpr Int intrinsicMemmove = 2;

    static constexpr Int intrinsicMemset = 3;

    static constexpr Int intrinsicMemcmp = 4;

    static constexpr Int intrinsicMemchr = 5;

    static constexpr Int intrinsicStrlen = 6;

    static constexpr Int intrinsicStrcpy = 7;

    static constexpr Int intrinsicStrcat = 8;

    static constexpr Int intrinsicStrcmp = 9;

    static constexpr Int intrinsicStrspn = 10;

    static constexpr Int intrinsicStrcspn = 11;

    static constexpr Int constF32Sign = 0;

    static constexpr Int constF32Abs = 1;

    static constexpr Int constF64Sign = 2;

    static constexpr Int constF64Abs = 3;

    static constexpr Int constCount = 4;

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

    static constexpr Int classGPR = 0;

    static constexpr Int classXMM = 1;

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

    // shadow checks the shadow map of an access of size bytes at src + imm,
    // or at imm without a register, aux its kind; trapAt traps on cc with
    // the asan report of the access at src + mem.disp (funcEmitterTrapJccAt).
    static constexpr MOp mShadow = MOp(69LL);

    static constexpr MOp mTrapAt = MOp(70LL);

    // aluMemImm and aluMemReg update a memory operand with an immediate or a
    // register: mem op= imm, mem op= src.
    static constexpr MOp mAluMemImm = MOp(67LL);

    static constexpr MOp mAluMemReg = MOp(68LL);

    static constexpr Int truncUnsigned = 1;

    static constexpr Int truncSat = 2;

    static constexpr AluOp aluAdd = AluOp(0LL);

    static constexpr AluOp aluOr = AluOp(1LL);

    static constexpr AluOp aluAdc = AluOp(2LL);

    static constexpr AluOp aluSbb = AluOp(3LL);

    static constexpr AluOp aluAnd = AluOp(4LL);

    static constexpr AluOp aluSub = AluOp(5LL);

    static constexpr AluOp aluXor = AluOp(6LL);

    static constexpr AluOp aluCmp = AluOp(7LL);

    static constexpr AluOp aluImul = AluOp(8LL);

    static constexpr Int shiftRol = 0;

    static constexpr Int shiftRor = 1;

    static constexpr Int shiftShl = 4;

    static constexpr Int shiftShr = 5;

    static constexpr Int shiftSar = 7;

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

    static constexpr Int fSqrt = 0;

    static constexpr Int fRound = 1;

    static constexpr Int ccO = 0;

    static constexpr Int ccNO = 1;

    static constexpr Int ccB = 2;

    static constexpr Int ccAE = 3;

    static constexpr Int ccE = 4;

    static constexpr Int ccNE = 5;

    static constexpr Int ccBE = 6;

    static constexpr Int ccA = 7;

    static constexpr Int ccS = 8;

    static constexpr Int ccNS = 9;

    static constexpr Int ccP = 10;

    static constexpr Int ccNP = 11;

    static constexpr Int ccL = 12;

    static constexpr Int ccGE = 13;

    static constexpr Int ccLE = 14;

    static constexpr Int ccG = 15;

    static constexpr Int roleUse = 1;

    static constexpr Int roleDef = 2;

    static constexpr Int roleUseDef = 3;

    static constexpr Int roleTemp = 4;

    static constexpr ValType I32 = ValType(127LL);

    static constexpr ValType I64 = ValType(126LL);

    static constexpr ValType F32 = ValType(125LL);

    static constexpr ValType F64 = ValType(124LL);

    static constexpr ValType V128 = ValType(123LL);

    static constexpr ValType FuncRef = ValType(112LL);

    static constexpr ValType ExternRef = ValType(111LL);

    static constexpr Int externFunc = 0;

    static constexpr Int externTable = 1;

    static constexpr Int externMemory = 2;

    static constexpr Int externGlobal = 3;

    static constexpr Int segmentActive = 0;

    static constexpr Int segmentPassive = 1;

    static constexpr Int segmentDeclarative = 2;

    static constexpr i32 maxPos = 1073741824;

    static const String symInit = String("w2o.init");

    static const String symMorestackStub = String("w2o.morestack");

    static const String symImport = String("w2o.import.");

    static const String symExport = String("w2o.export.");

    static const String symTrap = String("w2o.trap");

    // trap_at is the trap of an asan check, with the access's address and
    // its width and kind; asan_range is the loader's stub that counts the
    // accessible bytes of a range of the shadow map (stubs_amd64.s), called
    // as a function of lib/str is.
    static const String symTrapAt = String("w2o.trap_at");

    static const String symAsanRange = String("w2o.asan_range");

    static const String symMemoryGrow = String("w2o.memory_grow");

    static const String symTableGrow = String("w2o.table_grow");

    static const String symInterruptCall = String("w2o.interrupt_call");

    // lib/str's functions the loader binds, those of the host's
    // configuration: memcpy and memmove take the destination, the source and
    // the length, memset the destination, the byte and the length, as the
    // module's functions take arguments, and keep what they keep.
    static const String symMemcpy = String("str.memcpy");

    static const String symMemmove = String("str.memmove");

    static const String symMemset = String("str.memset");

    // memcmp takes the two addresses and the length, memchr the address, the
    // byte and the length, strlen the address; memcmp's result is an int,
    // memchr's an address or null, strlen's a length.
    static const String symMemcmp = String("str.memcmp");

    static const String symMemchr = String("str.memchr");

    static const String symStrlen = String("str.strlen");

    // strcmp takes the two strings, strspn and strcspn the string and the
    // set; strcmp's result is an int, theirs a length.
    static const String symStrcmp = String("str.strcmp");

    static const String symStrspn = String("str.strspn");

    static const String symStrcspn = String("str.strcspn");

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

    static Span<i32> mFuncHotOrder(MFunc* mf, Span<i32> order);

    static i32 mFuncSuccAt(MFunc* mf, Slice<i32> succs, Int i);

    static VReg mFuncNewReg(MFunc* mf, u8 classValue);

    static u8 mFuncRegClass(MFunc* mf, VReg r);

    static Slice<VReg> mInstClobberSet(MInst* in, u8 registerClasses);

    static bool mInstMemoryOperand(MInst* in, VReg* r);

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

    static ValueRange valueRangesOf(ValueRanges* r, i32 v, Int depth);

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

    static VReg allocatorSiblingReg(Allocator* a, Interval* cur);

    static Interval* allocatorInterval(Allocator* a, VReg r);

    static void allocatorLinearize(Allocator* a);

    static void allocatorLiveness(Allocator* a, bool copiesOnly = false);

    static VReg allocatorLoc(Allocator* a, Interval* it);

    static VReg allocatorLocation(Allocator* a, VReg r, i32 p);

    static VReg allocatorLocate(Allocator* a, Interval* root, i32 p);

    static u8 allocatorRegClass(Allocator* a, VReg r);

    static void allocatorResolve(Allocator* a);

    static void allocatorSaveSpillsAtDefinitions(Allocator* a, PositionedMoves& inner, Map<i32, Slice<Move>> atEnd, Map<i32, Slice<Move>> atStart);

    static void allocatorSpillAtJoins(Allocator* a);

    static void allocatorSplitAtJoins(Allocator* a);

    static void allocatorDefinitions(Allocator* a, bool depths);

    static void allocatorConstantDefinitions(Allocator* a);

    static void allocatorOrderSlots(Allocator* a);

    static void allocatorDeadMoves(Allocator* a);

    static void allocatorDeadSpills(Allocator* a);

    static void allocatorCopies(Allocator* a);

    static void allocatorDuplicateDispatch(Allocator* a);

    static void allocatorNarrowMasks(Allocator* a);

    static Interval* allocatorPartAt(Interval* root, i32 p);

    static void allocatorScan(Allocator* a);

    static void allocatorSpill(Allocator* a, Interval* it);

    static i32 allocatorSplitPos(Allocator* a, i32 min, i32 max);

    static bool allocatorTryFree(Allocator* a, Interval* cur);

    static Int allocatorVindex(Allocator* a, VReg r);

    static void asmAluMI(Asm* a, AluOp op, u8 size, Amem m, i64 x);

    static void asmAluRI(Asm* a, AluOp op, u8 size, VReg dst, i64 x);

    static void asmAluRIRaw(Asm* a, AluOp op, u8 size, VReg dst, i64 x);

    static void asmAluRM(Asm* a, AluOp op, u8 size, VReg dst, Amem m);

    static void asmAluMR(Asm* a, AluOp op, u8 size, Amem m, VReg src);

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

    static void asmMoveRegs(Asm* a, Slice<RegMove> moves, VReg scratch);

    static void asmMovXG(Asm* a, u8 size, VReg dst, VReg src);

    static void asmMoveSP(Asm* a, i32 by);

    static void asmNops(Asm* a, i32 n);

    static void asmOpRM(Asm* a, bool w, Span<const u8> op, Int reg, Amem m, Int imm, bool byteReg);

    static void asmOpRR(Asm* a, bool w, Span<const u8> op, Int reg, Int rm, bool byteRegs);

    static void asmPatch32(Asm* a, i32 at, i32 target);

    static void asmPatch8(Asm* a, i32 at, i32 target);

    static void asmPop(Asm* a, VReg r);

    static void asmPopMem(Asm* a, Amem m);

    static i32 asmPos(Asm* a);

    static void asmPrefixed(Asm* a, u8 prefix, bool w, Span<const u8> op, Int reg, Int rm);

    static void asmPush(Asm* a, VReg r);

    static void asmPushZero(Asm* a);

    static void asmPushMem(Asm* a, Amem m);

    static void asmRet(Asm* a);

    static void asmRex(Asm* a, bool w, Int r, Int x, Int b, bool force);

    static void asmSetSP(Asm* a, i32 delta);

    static void asmSetcc(Asm* a, u8 cc, VReg dst);

    static void asmShiftCL(Asm* a, u8 kind, u8 size, VReg dst);

    static void asmShiftImm(Asm* a, u8 kind, u8 size, VReg dst, i64 n);

    static void asmShiftX(Asm* a, u8 kind, u8 size, VReg dst, VReg src, VReg count);

    static Slice<SpEntry> asmSpTable(Asm* a, i32 start, i32 end);

    static void asmMarkPos(Asm* a, u32 pos);

    static Slice<LineMark> asmLineTable(Asm* a, i32 start, i32 end);

    static void moduleReadLines(Module* m);

    static void objCodeLines(ObjCode* oc, Meta* meta);

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

    static bool loopDominates(CfgInfo* ci, i32 a, i32 b);

    // loopEachDominated calls f with every block an analyzed block h
    // dominates: its subtree of the dominator tree, then the blocks added
    // after the analysis under it. It stops when f returns true.
    template <typename F>
    static bool loopEachDominated(CfgInfo* ci, i32 h, F f);

    static void cfgInfoPreorder(CfgInfo* ci, Function* f);

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

    static i32 frontendTryRemoveTrivial(Frontend* fe, i32 phi, Span<const i32> ops);

    static ValType frontendTypeOf(Frontend* fe, i32 v);

    static void frontendUnary(Frontend* fe, Op op, ValType t);

    static void frontendWrite(Frontend* fe, i32 local, i32 b, i32 v);

    static i32 frontendZero(Frontend* fe, ValType t);

    static void funcEmitterBlock(FuncEmitter* e, MBlock* blk, Int at);

    static void funcEmitterCheck(FuncEmitter* e, MInst* in);

    static void funcEmitterShadow(FuncEmitter* e, MInst* in);

    static void funcEmitterTrapJccAt(FuncEmitter* e, u8 cc, i64 code, VReg reg, i32 disp, u8 width, u8 kind);

    static void funcEmitterCvtU64F(FuncEmitter* e, MInst* in);

    static void funcEmitterExactCheck(FuncEmitter* e, VReg src, i64 end);

    static void funcEmitterInst(FuncEmitter* e, MInst* in);

    static void funcEmitterJumpTo(FuncEmitter* e, Int cc, i32 block);

    static Amem funcEmitterMem(FuncEmitter* e, Mem m);

    static void funcEmitterMinMax(FuncEmitter* e, MInst* in);

    static void funcEmitterMove(FuncEmitter* e, VReg dst, VReg src);

    static void funcEmitterParMove(FuncEmitter* e, Span<Move> moves);

    static Amem funcEmitterSlotMem(FuncEmitter* e, i32 slot);

    static void funcEmitterGoCall(FuncEmitter* e, i32 index);

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

    static Mem lowererAddress(Lowerer* l, i32 addr, i64 offset, Int width, bool proven, bool store);

    static Mem lowererAsanAddress(Lowerer* l, i32 addr, i64 offset, Int width, bool store);

    static void lowererAsanRange(Lowerer* l, VReg a, VReg n);

    static VReg lowererShadowAddress(Lowerer* l, VReg a);

    static void lowererAlu(Lowerer* l, i32 v, Op op, i32 a, i32 b, u8 size);

    static void lowererCall(Lowerer* l, i32 v, Int index, Span<const i32> args);

    static void lowererGoCall(Lowerer* l, i32 v, Slice<VReg> regs, Slice<ValType> params, Slice<ValType> results, String sym);

    static void lowererIntrinsic(Lowerer* l, i32 v, Int intrinsic, Span<const i32> args);

    static void lowererStrCall(Lowerer* l, String sym, Span<const VReg> args);

    static VReg lowererStrResult(Lowerer* l);

    static VReg lowererHostAddress(Lowerer* l, VReg a);

    static VReg lowererStrLength(Lowerer* l, VReg s);

    static VReg lowererSum(Lowerer* l, VReg a, VReg b);

    static void lowererCheckEnd(Lowerer* l, VReg a, VReg n, u8 cc);

    static void lowererMemCompare(Lowerer* l, i32 v, Span<const i32> args);

    static void lowererMemFind(Lowerer* l, i32 v, Span<const i32> args);

    static void lowererStrCopy(Lowerer* l, i32 v, Span<const i32> args, bool append);

    static void lowererSign(Lowerer* l, i32 v);

    static void lowererStrPair(Lowerer* l, i32 v, Span<const i32> args, String sym);

    static void lowererCallIndirect(Lowerer* l, i32 v, Int typeIndex, Int table, Span<const i32> args);

    static void lowererCheckRange(Lowerer* l, VReg a, VReg n, Mem limit, u8 limitSize, i64 code);

    static u8 lowererClassOf(Lowerer* l, ValType t);

    static Tuple2<u8, Int> lowererCompare(Lowerer* l, i32 c);

    static bool lowererBranchPair(Lowerer* l, i32 c);

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

    static bool lowererNarrowShift(Lowerer* l, i32 v, i32 a, i32 k);

    static VReg lowererLoad64(Lowerer* l, Mem mem);

    static void lowererLowerValue(Lowerer* l, i32 v);

    static void lowererMarkDeferred(Lowerer* l);

    static void lowererMarkFoldable(Lowerer* l);

    static void lowererMarkUpdates(Lowerer* l);

    static Tuple2<Mem, bool> lowererFoldedLoad(Lowerer* l, i32 v);

    static bool lowererTransparent(Op op);

    static void lowererMemAddr(Lowerer* l, VReg dst, VReg a);

    static void lowererMemCopy(Lowerer* l, Span<const i32> args, String sym);

    static void lowererMemFill(Lowerer* l, Span<const i32> args);

    static void lowererMemInit(Lowerer* l, Int seg, Span<const i32> args);

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

    static Meta* objCodeDescription(ObjCode* oc);

    static void objCodeEmitFunction(ObjCode* oc, Int index, MFunc* mf, Allocator* al);

    static FuncEmitter* objCodeEmitBody(ObjCode* oc, Int index, MFunc* mf, Allocator* al, Slice<u8> shorts, Slice<u8> trapShorts, Slice<i32> plan);

    static void objCodeEndRegion(ObjCode* oc, i32 start);

    static void objCodeExportThunks(ObjCode* oc);

    static i32 objCodeExtern(ObjCode* oc, String sym);

    static Amem objCodeFloatConst(ObjCode* oc, u64 bits, u8 size);

    static String objCodeFuncName(ObjCode* oc, Int i);

    static i32 objCodeGoCall(ObjCode* oc, String name, Slice<ValType> params, Slice<ValType> results, String sym, bool record);

    static Amem objCodeGotMem(ObjCode* oc, String sym);

    static void objCodeImportAdapters(ObjCode* oc);

    static Slice<bool> objCodeNamedFunctions(ObjCode* oc);

    static void objCodeInitThunk(ObjCode* oc);

    static void objCodePlaceBlobs(ObjCode* oc);

    static void objCodeRecordFrame(ObjCode* oc);

    static void objCodeResolve(ObjCode* oc);

    static void objCodeSlowPath(ObjCode* oc, i32 at, i32 entry, i32 total, VReg temp);

    static void objCodeDropWord(ObjCode* oc);

    static i32 objCodeStackCheck(ObjCode* oc, i32 total, VReg temp);

    static Slice<i32> objCodeStackCheckBlocks(MFunc* mf);

    static i32 objCodeThunk(ObjCode* oc, String name, Slice<ValType> params, Slice<ValType> results, Int index, Fn<void()> body);

    static void objCodeTrapCall(ObjCode* oc, i64 code);

    static void objCodeTrapAtCall(ObjCode* oc, const TrapFixup& t);

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

    static bool optimizerSimplify(Optimizer* o, Int from = 0);

    static bool optimizerSimplifyTerm(Optimizer* o, i32 b);

    static bool optimizerSimplifyValue(Optimizer* o, i32 v);

    static Slice<i32> optimizerSubstitute(Optimizer* o, i32 b, Slice<i32> out, Slice<i32> in);

    static bool optimizerThread(Optimizer* o);

    static bool optimizerTrivialParams(Optimizer* o);

    static bool optimizerUnaryIdentity(Optimizer* o, i32 v, Op op, i32 a);

    static bool optimizerUnreachable(Optimizer* o);

    static bool optimizerUnrollLoops(Optimizer* o);

    static bool optimizerDisjointOrs(Optimizer* o);

    static bool optimizerOuterConstants(Optimizer* o);

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

    static bool moduleLowFree(Module* m);

    static MInst copyMachineInst(MInst in);

    static i32 dataLabel(Int seg);

    static Module* decodeModule(Slice<u8> data);


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

    static MFunc* lowerFunction(ObjCode* oc, Function* f);

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

    static Int intrinsicOf(Import imp, FuncType* t);

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

    static bool versionMemoryLoops(Function* f);

    static void versionedPreds(Function* f, i32 preheader, i32 header, Int first);

    static bool windowBase(Function* f, i32 addr, i64 end, i32* base, i64* c);

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

    static Slice<VReg> goClobbers{};

    static Slice<VReg> goClobberGprs{};

    static Slice<VReg> goClobberXmms{};

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
        // The internal convention: arguments and results in the six
        // caller-saved registers, five callee-saved; R13 is the instance,
        // R15 the memory base, R14 g and RBP the frame pointer.
        allocGPRs = sliceOf<VReg>({RAX, RCX, RDX, RSI, RDI, R8, RBX, R9, R10, R11, R12});

        allocXMMs = xmmRange(15);

        argGPRs = sliceOf<VReg>({RAX, RCX, RDX, RSI, RDI, R8});

        argXMMs = xmmRange(15);

        calleeSaved = sliceOf<VReg>({RBX, R9, R10, R11, R12});

        callerSavedGprs = sliceOf<VReg>({RAX, RCX, RDX, RSI, RDI, R8});
        callerSavedXmms = xmmRange(15);
        callerSaved = appendSlice(callerSavedGprs, callerSavedXmms);

        // A call of Go writes every register the code allocates; R13 and R15
        // come back from g and the instance.
        goClobberGprs = sliceOf<VReg>({RAX, RBX, RCX, RDX, RSI, RDI, R8, R9, R10, R11, R12});
        goClobberXmms = xmmRange(15);
        goClobbers = appendSlice(goClobberGprs, goClobberXmms);

        floatUnary = Array<Op, 7>{{OpFAbs, OpFNeg, OpFCeil, OpFFloor, OpFTrunc, OpFNearest, OpFSqrt}};

        goFloatRegs = xmmRange(15);

        goIntRegs = sliceOf<VReg>({RAX, RBX, RCX, RDI, RSI, R8, R9, R10, R11});

        intCC = Array<u8, 10>{{ccE, ccNE, ccL, ccB, ccG, ccA, ccLE, ccBE, ccGE, ccAE}};

        nopBytes = Array<Slice<u8>, 10>{{{}, sliceOf<u8>({u8(144LL)}), sliceOf<u8>({u8(102LL), u8(144LL)}), sliceOf<u8>({u8(15LL), u8(31LL), u8(0LL)}), sliceOf<u8>({u8(15LL), u8(31LL), u8(64LL), u8(0LL)}), sliceOf<u8>({u8(15LL), u8(31LL), u8(68LL), u8(0LL), u8(0LL)}), sliceOf<u8>({u8(102LL), u8(15LL), u8(31LL), u8(68LL), u8(0LL), u8(0LL)}), sliceOf<u8>({u8(15LL), u8(31LL), u8(128LL), u8(0LL), u8(0LL), u8(0LL), u8(0LL)}), sliceOf<u8>({u8(15LL), u8(31LL), u8(132LL), u8(0LL), u8(0LL), u8(0LL), u8(0LL), u8(0LL)}), sliceOf<u8>({u8(102LL), u8(15LL), u8(31LL), u8(132LL), u8(0LL), u8(0LL), u8(0LL), u8(0LL), u8(0LL)})}};

        resultGPRs = sliceOf<VReg>({RAX, RCX, RDX, RSI, RDI, R8});

        resultXMMs = xmmRange(15);
    }

    static AddressExpression addressExpressionsOf(AddressExpressions* r, i32 v, Int depth) {
        if (r->seen[v]) {
            return r->cache[v];
        }
        Function* f = r->f;
        Value val = f->values[v];
        AddressExpression out = addressAtom(v);
        auto temporary1 = true;
        auto temporary2 = out;
        r->seen[v] = temporary1;
        r->cache[v] = temporary2;
        if (depth > 32 || (val.typ != I32 && val.typ != I64)) {
            return out;
        }
        Span<i32> args = functionArgs(f, v);
        switch (val.op) {
            case OpConst: {
                out = AddressExpression{{}, convert<u32>(val.aux)};
                break;
            }
            case OpWrap:
            case OpExtendU:
            case OpExtendS: {
                out = addressExpressionsOf(r, args[0], depth + 1);
                break;
            }
            case OpAdd:
            case OpSub: {
                u32 factor = 1;
                if (val.op == OpSub) {
                    factor = u32(4294967295LL);
                }
                auto temporary4 = addAddressExpression(addressExpressionsOf(r, args[0], depth + 1), addressExpressionsOf(r, args[1], depth + 1), factor);
                AddressExpression sum = temporary4.v0;
                bool ok = temporary4.v1;
                if (ok) {
                    out = sum;
                }
                break;
            }
            case OpMul:
            case OpShl: {
                i32 a = args[0];
                Value k = f->values[args[1]];
                if (val.op == OpMul && k.op != OpConst) {
                    auto temporary7 = args[1];
                    auto temporary8 = f->values[args[0]];
                    a = temporary7;
                    k = temporary8;
                }
                if (k.op == OpConst) {
                    u32 factor = convert<u32>(k.aux);
                    if (val.op == OpShl) {
                        u64 shift = convert<u64>(k.aux) & convert<u64>(u8(u8(typeSize(val.typ) * 8) - 1));
                        factor = 0;
                        if (shift < 32) {
                            factor = shiftLeft(u32(1LL), shift);
                        }
                    }
                    auto temporary9 = addAddressExpression(AddressExpression{}, addressExpressionsOf(r, a, depth + 1), factor);
                    AddressExpression product = temporary9.v0;
                    bool ok = temporary9.v1;
                    if (ok) {
                        out = product;
                    }
                }
                break;
            }
        }
        r->cache[v] = out;
        return out;
    }

    static bool floatIntegerNarrowCan(FloatIntegerNarrow* n, i32 v, Int depth) {
        Function* f = n->o->f;
        Value* val = &f->values[v];
        if (val->typ != F64 || depth > 24) {
            return false;
        }
        if (n->valid[v] != 0) {
            return n->valid[v] == 2;
        }
        n->valid[v] = 1;
        bool ok = false;
        switch (val->op) {
            case OpConst: {
                double x = mathFloat64frombits(convert<u64>(val->aux));
                ok = x >= double(0.0) && x <= double(4294967295.0) && mathTrunc(x) == x;
                break;
            }
            case OpConvertU: {
                ok = f->values[functionArgs(f, v)[0]].typ == I32;
                break;
            }
            case OpParam: {
                Vec<Edge*> incoming = optimizerIncoming(n->o, val->block);
                ok = val->block != 0 && length(incoming) > 0;
                auto range11 = incoming.view();
                for (Int index12 = 0, rangeEnd13 = length(range11); index12 < rangeEnd13; ++index12) {
                    Edge* e = range11[index12];
                    ok = ok && floatIntegerNarrowCan(n, e->args[val->aux], depth + 1);
                }
                break;
            }
        }
        if (ok) {
            n->valid[v] = 2;
        }
        return ok;
    }

    static i32 floatIntegerNarrowInteger(FloatIntegerNarrow* n, i32 v) {
        auto temporary14 = lookupMap(n->values, v);
        i32 mapped = temporary14.v0;
        bool ok = temporary14.v1;
        if (ok) {
            return mapped;
        }
        Function* f = n->o->f;
        Value val = f->values[v];
        switch (val.op) {
            case OpConst: {
                return optimizerConstant(n->o, I32, convert<i64>(mathFloat64frombits(convert<u64>(val.aux))));
            }
            case OpConvertU: {
                return functionArgs(f, v)[0];
            }
        }
        i32 b = val.block;
        Int index = length(f->blocks[b].params);
        i32 p = functionNewValue(f, OpParam, I32, b, convert<i64>(index), Span<const i32>({}));
        f->blocks[b].params = appendValues(f->blocks[b].params, p);
        setMap(n->values, v, p);
        Vec<Edge*> incoming = optimizerIncoming(n->o, b);
        auto range16 = incoming.view();
        for (Int index17 = 0, rangeEnd18 = length(range16); index17 < rangeEnd18; ++index17) {
            Edge* e = range16[index17];
            e->args = appendValues(e->args, -1);
        }
        auto range19 = incoming.view();
        for (Int index20 = 0, rangeEnd21 = length(range19); index20 < rangeEnd21; ++index20) {
            Edge* e = range19[index20];
            i32 arg = e->args[val.aux];
            e->args[index] = floatIntegerNarrowInteger(n, arg);
        }
        return p;
    }

    static void floatRangesAdd(FloatRanges* r, i32 v, FloatRange bound) {
        auto temporary22 = lookupMap(r->facts, v);
        FloatRange prior = temporary22.v0;
        bool ok = temporary22.v1;
        if (ok) {
            bound = meetFloatRange(prior, bound);
        }
        setMap(r->facts, v, bound);
    }

    // constrain records an ordered comparison on the path where a value is
    // used. A false comparison admits NaN; another fact must exclude it before
    // a conversion may lose its saturation checks.
    static void floatRangesConstrain(FloatRanges* r, i32 cond, bool truth) {
        Function* f = r->f;
        Value* val = &f->values[cond];
        Op op = val->op;
        if (op < OpFEq || op > OpFGe) {
            return;
        }
        Span<i32> args = functionArgs(f, cond);
        i32 v = args[0];
        i32 c = args[1];
        auto [k, ok] = floatConstant(f, c);
        if (!ok) {
            auto temporary26 = floatConstant(f, v);
            k = temporary26.v0;
            ok = temporary26.v1;
            v = c;
            switch (op) {
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
        if (!ok || mathIsNaN(k)) {
            return;
        }
        double prev = mathNextafter(k, mathInf(-1));
        double next = mathNextafter(k, mathInf(1));
        if (f->values[v].typ == F32) {
            prev = convert<double>(mathNextafter32(convert<float>(k), convert<float>(mathInf(-1))));
            next = convert<double>(mathNextafter32(convert<float>(k), convert<float>(mathInf(1))));
        }
        FloatRange bound = unknownFloatRange();
        bound.nan = !truth;
        switch (op) {
            case OpFEq:
            case OpFNe: {
                if (truth != (op == OpFEq)) {
                    return;
                }
                auto temporary31 = k;
                auto temporary32 = k;
                auto temporary33 = false;
                bound.lo = temporary31;
                bound.hi = temporary32;
                bound.nan = temporary33;
                break;
            }
            case OpFLt: {
                if (truth) {
                    bound.hi = prev;
                } else {
                    bound.lo = k;
                }
                break;
            }
            case OpFLe: {
                if (truth) {
                    bound.hi = k;
                } else {
                    bound.lo = next;
                }
                break;
            }
            case OpFGt: {
                if (truth) {
                    bound.lo = next;
                } else {
                    bound.hi = k;
                }
                break;
            }
            case OpFGe: {
                if (truth) {
                    bound.lo = k;
                } else {
                    bound.hi = prev;
                }
                break;
            }
        }
        floatRangesAdd(r, v, bound);
        if (f->values[v].op == OpFAbs && bound.hi >= double(0.0) && !mathIsInf(bound.hi, 0)) {
            floatRangesAdd(r, functionArgs(f, v)[0], FloatRange{-bound.hi, bound.hi, bound.nan});
        }
    }

    static FloatRange floatRangesOf(FloatRanges* r, i32 v, Int depth) {
        {
            auto temporary34 = lookupMap(r->cache, v);
            FloatRange got = temporary34.v0;
            bool ok = temporary34.v1;
            if (ok) {
                return got;
            }
        }
        Value* val = &r->f->values[v];
        FloatRange out = unknownFloatRange();
        {
            auto [k, ok] = floatConstant(r->f, v);
            if (ok) {
                out = FloatRange{k, k, mathIsNaN(k)};
            } else if (depth < 32) {
                Span<i32> args = functionArgs(r->f, v);
                switch (val->op) {
                    case OpFAdd:
                    case OpFSub:
                    case OpFMul:
                    case OpFDiv: {
                        FloatRange a = floatRangesOf(r, args[0], depth + 1);
                        FloatRange b = floatRangesOf(r, args[1], depth + 1);
                        bool finiteB = !b.nan && !mathIsInf(b.lo, 0) && !mathIsInf(b.hi, 0);
                        if (finiteB && (val->op == OpFAdd || val->op == OpFSub || (b.lo == b.hi && b.lo > double(0.0)))) {
                            out.nan = a.nan;
                            switch (val->op) {
                                case OpFAdd: {
                                    auto temporary40 = a.lo + b.lo;
                                    auto temporary41 = a.hi + b.hi;
                                    out.lo = temporary40;
                                    out.hi = temporary41;
                                    break;
                                }
                                case OpFSub: {
                                    auto temporary42 = a.lo - b.hi;
                                    auto temporary43 = a.hi - b.lo;
                                    out.lo = temporary42;
                                    out.hi = temporary43;
                                    break;
                                }
                                case OpFMul: {
                                    auto temporary44 = a.lo * b.lo;
                                    auto temporary45 = a.hi * b.hi;
                                    out.lo = temporary44;
                                    out.hi = temporary45;
                                    break;
                                }
                                case OpFDiv: {
                                    auto temporary46 = a.lo / b.lo;
                                    auto temporary47 = a.hi / b.hi;
                                    out.lo = temporary46;
                                    out.hi = temporary47;
                                    break;
                                }
                            }
                        }
                        break;
                    }
                    case OpFAbs: {
                        FloatRange a = floatRangesOf(r, args[0], depth + 1);
                        out = FloatRange{double(0.0), maximum(mathAbs(a.lo), mathAbs(a.hi)), a.nan};
                        break;
                    }
                    case OpFNeg: {
                        FloatRange a = floatRangesOf(r, args[0], depth + 1);
                        out = FloatRange{-a.hi, -a.lo, a.nan};
                        break;
                    }
                    case OpSelect: {
                        FloatRange a = floatRangesOf(r, args[1], depth + 1);
                        FloatRange b = floatRangesOf(r, args[2], depth + 1);
                        out = FloatRange{minimum(a.lo, b.lo), maximum(a.hi, b.hi), a.nan || b.nan};
                        break;
                    }
                }
            }
        }
        if (val->typ == F32) {
            auto temporary50 = convert<double>(convert<float>(out.lo));
            auto temporary51 = convert<double>(convert<float>(out.hi));
            out.lo = temporary50;
            out.hi = temporary51;
        }
        auto temporary52 = lookupMap(r->facts, v);
        FloatRange bound = temporary52.v0;
        bool ok = temporary52.v1;
        if (ok) {
            out = meetFloatRange(out, bound);
        }
        setMap(r->cache, v, out);
        return out;
    }

    static CfgInfo* functionAnalyze(Function* f) {
        CfgInfo* ci = activePool->make<CfgInfo>(CfgInfo{functionRpo(f), {}, {}, {}, {}, {}, {}, {}, {}});
        Int n = length(f->blocks);
        ci->rpoIndex = makeSlice<i32>(n);
        for (Int i = 0, end = length(ci->rpoIndex); i < end; ++i) {
            ci->rpoIndex[i] = -1;
        }
        auto order = ci->order;
        for (Int i = 0; i < length(order); ++i) {
            i32 b = order[i];
            ci->rpoIndex[b] = convert<i32>(i);
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
        for (Int i = 0, end = length(f->blocks); i < end; ++i) {
            f->blocks[i].preds = subslice(f->blocks[i].preds, 0, 0);
        }
        for (Int i = 0, end = length(f->blocks); i < end; ++i) {
            if (f->blocks[i].dead) {
                continue;
            }
            auto edges = f->blocks[i].term.edges;
            for (Int j = 0; j < length(edges); ++j) {
                Edge e = edges[j];
                f->blocks[e.block].preds = appendValues(f->blocks[e.block].preds, convert<i32>(i));
            }
        }
    }

    static i32 functionNewBlock(Function* f) {
        f->blocks.pushBack(Block{});
        return convert<i32>(length(f->blocks) - 1);
    }

    static i32 functionNewValue(Function* f, Op op, ValType typ, i32 block, i64 aux, Span<const i32> args) {
        i32 id = convert<i32>(length(f->values));
        i32 start = convert<i32>(length(f->pool));
        appendSpan(f->pool, args);
        f->values.pushBack(Value{op, typ, convert<u16>(length(args)), start, aux, {}, block});
        f->positions.pushBack(0);
        return id;
    }

    static Slice<i32> functionRpo(Function* f) {
        Int n = length(f->blocks);
        Slice<bool> visited = makeSlice<bool>(n);
        Slice<i32> post = makeSlice<i32>(0, n);
        Slice<FrameCfg34> stack = sliceOf<FrameCfg34>({FrameCfg34{0, 0}});
        visited[0] = true;
        while (length(stack) > Int(0LL)) {
            FrameCfg34* top = &stack[length(stack) - 1];
            Slice<Edge> edges = f->blocks[top->b].term.edges;
            if (top->i < length(edges)) {
                i32 s = edges[length(edges) - 1 - top->i].block;
                top->i++;
                if (!visited[s]) {
                    visited[s] = true;
                    stack = appendValues(stack, FrameCfg34{s, 0});
                }
                continue;
            }
            post = appendValues(post, top->b);
            stack = subslice(stack, 0, length(stack) - 1);
        }
        Int i = 0;
        Int j = length(post) - 1;
        for (; i < j; [&]() {
            auto temporary70 = i + 1;
            auto temporary71 = j - 1;
            i = temporary70;
            j = temporary71;
        }()) {
            auto temporary72 = post[j];
            auto temporary73 = post[i];
            post[i] = temporary72;
            post[j] = temporary73;
        }
        return post;
    }

    static i32 layoutDataDropped(Layout* l, Int seg) {
        return l->dropped + convert<i32>(seg);
    }

    static i32 layoutDesc(Layout* l, Int f) {
        return l->descs + convert<i32>(f) * descBytes;
    }

    static i32 layoutElemDropped(Layout* l, Module* m, Int seg) {
        return l->dropped + convert<i32>(length(m->datas) + seg);
    }

    // succAt is the i-th successor a depth-first walk takes. Reverse
    // postorder places the last one right after its block, so of two
    // successors the one in the deeper loop is taken last: a loop's body
    // follows its header, and exits leave the loop's code.
    static i32 mFuncSuccAt(MFunc* mf, Slice<i32> succs, Int i) {
        const Int n = length(succs);
        if (n == 2 && mf->blocks[succs[0]].depth < mf->blocks[succs[1]].depth) {
            return succs[i];
        }
        return succs[n - 1 - i];
    }

    // Build hot reverse postorder without visiting fallback edges: cold paths
    // must not affect the placement of hot successors. A strict function keeps
    // the allocator's exact order.
    static Span<i32> mFuncHotOrder(MFunc* mf, Span<i32> order) {
        Int first = -1;
        for (Int j = 0; j < length(order); ++j) {
            i32 b = order[j];
            if (mf->blocks[b].cold) {
                first = j;
                break;
            }
        }
        if (first < 0) {
            return order;
        }
        Slice<bool> seen = makeSlice<bool>(length(mf->blocks));
        seen[0] = true;
        Slice<VisitMir346> stack = sliceOf<VisitMir346>({VisitMir346{0, {}}});
        Slice<i32> post = {};
        while (length(stack) > Int(0LL)) {
            VisitMir346* top = &stack[length(stack) - 1];
            Slice<i32> succs = mf->blocks[top->block].succs;
            if (top->edge < length(succs)) {
                i32 s = mFuncSuccAt(mf, succs, top->edge);
                top->edge++;
                if (!seen[s] && !mf->blocks[s].cold) {
                    seen[s] = true;
                    stack = appendValues(stack, VisitMir346{s, {}});
                }
                continue;
            }
            post = appendValues(post, top->block);
            stack = subslice(stack, 0, length(stack) - 1);
        }
        Slice<i32> out = makeSlice<i32>(0, length(order));
        Int j = length(post) - 1;
        for (; j >= 0; j--) {
            out = appendValues(out, post[j]);
        }
        for (Int i = 0; i < length(order); ++i) {
            i32 b = order[i];
            if (mf->blocks[b].cold) {
                out = appendValues(out, b);
            }
        }
        return out;
    }

    static VReg mFuncNewReg(MFunc* mf, u8 classValue) {
        VReg r = convert<VReg>(32 + mf->nregs);
        mf->nregs++;
        mf->registerClasses |= u8(1u << classValue);
        mf->classValue = appendValues(mf->classValue, classValue);
        return r;
    }

    static u8 mFuncRegClass(MFunc* mf, VReg r) {
        if (vRegIsPhys(r)) {
            if (vRegIsXMM(r)) {
                return classXMM;
            }
            return classGPR;
        }
        return mf->classValue[r - firstVirtual];
    }

    // Fixed intervals constrain virtual allocation only in the same register
    // class. Calls need no XMM clobbers in an integer-only function, or GPR
    // clobbers in a function with only virtual XMM registers.
    static Slice<VReg> mInstClobberSet(MInst* in, u8 registerClasses) {
        if (in->op == mCall && in->aux == calleeGo) {
            if (registerClasses == (1u << classGPR)) {
                return goClobberGprs;
            }
            if (registerClasses == (1u << classXMM)) {
                return goClobberXmms;
            }
            return registerClasses ? goClobbers : Slice<VReg>{};
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

    // memoryOperand tells whether r is an operand of in that x86 reads from
    // memory as well: the second of an integer, compare or float operation.
    // Its use needs no register; spilled there, the instruction reads it
    // from its slot.
    static bool mInstMemoryOperand(MInst* in, VReg* r) {
        switch (in->op) {
            case mAlu: {
                return r == &in->src && in->src != in->dst;
            }
            case mCmp:
            case mFAlu: {
                return r == &in->src2 && in->src2 != in->src;
            }
        }
        return false;
    }

    // forEachOperand calls cb for every register operand of an instruction with
    // its role, uses before defs.
    template <typename F>
    static void mInstForEachOperand(MInst* in, F cb) {
        auto mem = [&]() mutable -> void {
            if (in->mem.base != noReg) {
                cb(&in->mem.base, roleUse);
            }
            if (in->mem.index != noReg) {
                cb(&in->mem.index, roleUse);
            }
        };
        switch (in->op) {
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
                cb(&in->src, roleUse);
                cb(&in->dst, roleDef);
                break;
            }
            case mMovImm:
            case mSetcc:
            case mFConst: {
                cb(&in->dst, roleDef);
                break;
            }
            case mLoad:
            case mFLoad:
            case mLea: {
                mem();
                cb(&in->dst, roleDef);
                break;
            }
            case mStore:
            case mFStore: {
                mem();
                cb(&in->src, roleUse);
                break;
            }
            case mStoreImm:
            case mCmpMemImm:
            case mCounter:
            case mAluMemImm: {
                mem();
                break;
            }
            case mAluMemReg: {
                mem();
                cb(&in->src, roleUse);
                break;
            }
            case mAlu: {
                cb(&in->src, roleUse);
                cb(&in->dst, roleUseDef);
                break;
            }
            case mAluImm:
            case mShiftImm:
            case mUnary: {
                cb(&in->dst, roleUseDef);
                break;
            }
            case mAluMem: {
                mem();
                cb(&in->dst, roleUseDef);
                break;
            }
            case mImul3: {
                cb(&in->src, roleUse);
                cb(&in->dst, roleDef);
                break;
            }
            case mCmp:
            case mTest:
            case mFCmp: {
                cb(&in->src, roleUse);
                cb(&in->src2, roleUse);
                break;
            }
            case mCmpImm:
            case mTestImm:
            case mCheck:
            case mTrapAt:
            case mSwitch:
            case mCallInd:
            case mStackArg:
            case mStoreRet: {
                cb(&in->src, roleUse);
                break;
            }
            case mShadow: {
                if (in->src != noReg) {
                    cb(&in->src, roleUse);
                }
                break;
            }
            case mCmpMem: {
                mem();
                cb(&in->src, roleUse);
                break;
            }
            case mCmov: {
                cb(&in->src, roleUse);
                cb(&in->dst, roleUseDef);
                break;
            }
            case mShiftX:
            case mFAlu: {
                cb(&in->src, roleUse);
                cb(&in->src2, roleUse);
                cb(&in->dst, roleDef);
                break;
            }
            case mRotCL: {
                cb(&in->src, roleUse);
                cb(&in->dst, roleUseDef);
                break;
            }
            case mParMove: {
                for (Int i = 0, end = length(mInstMoves(in)); i < end; ++i) {
                    cb(&mInstMoves(in)[i].src, roleUse);
                }
                for (Int i = 0, end = length(mInstMoves(in)); i < end; ++i) {
                    cb(&mInstMoves(in)[i].dst, roleDef);
                }
                break;
            }
            case mLoadArg: {
                cb(&in->dst, roleDef);
                break;
            }
            case mDivOvf: {
                cb(&in->src, roleUse);
                cb(&in->src2, roleUse);
                break;
            }
            case mTruncF:
            case mCvtU64F: {
                cb(&in->src, roleUse);
                cb(&in->dst, roleDef);
                break;
            }
            case mFMinMax: {
                cb(&in->src, roleUse);
                cb(&in->src2, roleUse);
                cb(&in->dst, roleDef);
                break;
            }
            case mFSelect: {
                cb(&in->src, roleUse);
                cb(&in->src2, roleUse);
                for (Int i = 0, end = length(mInstUses(in)); i < end; ++i) {
                    cb(&mInstUses(in)[i], roleUse);
                }
                cb(&in->dst, roleDef);
                break;
            }
            case mFAluMem: {
                mem();
                cb(&in->src, roleUse);
                cb(&in->dst, roleDef);
                break;
            }
        }
        for (Int i = 0, end = length(mInstTemps(in)); i < end; ++i) {
            cb(&mInstTemps(in)[i], roleTemp);
        }
        if (in->op == mDiv || in->op == mCall || in->op == mCallInd || in->op == mRet || in->op == mSignExtAx || in->op == mZeroDx || in->op == mGrowMem || in->op == mCopyForward) {
            if (in->op == mDiv || in->op == mGrowMem) {
                cb(&in->src, roleUse);
            }
            for (Int i = 0, end = length(mInstUses(in)); i < end; ++i) {
                cb(&mInstUses(in)[i], roleUse);
            }
            for (Int i = 0, end = length(mInstDefs(in)); i < end; ++i) {
                cb(&mInstDefs(in)[i], roleDef);
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
                f->values[n].aux2 = val.aux2 | (checked ? 32 : 0);
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
                    if (k.op != OpConst || u64(u32(k.aux)) + u64(f->values[n].aux) > 2147483647) {
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
        // Only blocks the header dominates and unreachable ones use values
        // of the loop; the order of the uses found does not matter. Blocks
        // added by this versioning use none.
        auto scan = [&](i32 b) -> bool {
            if (b >= oldBlocks || f->blocks[b].dead || (b < length(l->members) && l->members[b])) {
                return false;
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
            return false;
        };
        loopEachDominated(l->ci, l->header, scan);
        for (Int i = length(l->ci->order); i < length(l->ci->preorder); ++i) {
            scan(l->ci->preorder[i]);
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
        auto temporary185 = lookupMap(cache, v);
        CopyAddress known = temporary185.v0;
        bool ok = temporary185.v1;
        if (ok) {
            return known;
        }
        Function* f = l->f;
        Value val = f->values[v];
        CopyAddress bad = CopyAddress{};
        if (depth > 24 || val.typ != I32) {
            return bad;
        }
        CopyAddress out = CopyAddress{mapOf<i32, u32>({}), {}, {}, true};
        auto temporary186 = lookupMap(invariants, v);
        i32 mapped = temporary186.v0;
        bool hoisted = temporary186.v1;
        if (val.op == OpConst) {
            out.offset = convert<u32>(val.aux);
        } else if (hoisted || val.block != l->header || val.op == OpParam) {
            i32 key = v;
            if (hoisted) {
                key = mapped;
            } else if (val.block == l->header) {
                out.step = convert<u32>(steps[val.aux]);
            }
            setMap(out.terms, key, u32(1LL));
        } else {
            Span<i32> args = functionArgs(f, v);
            switch (val.op) {
                case OpAdd:
                case OpSub: {
                    CopyAddress a = memoryLoopCopyAddress(l, args[0], steps, invariants, cache, depth + 1);
                    CopyAddress b = memoryLoopCopyAddress(l, args[1], steps, invariants, cache, depth + 1);
                    if (!a.valid || !b.valid) {
                        return bad;
                    }
                    u32 mul = 1;
                    if (val.op == OpSub) {
                        mul = u32(4294967295LL);
                    }
                    auto temporary188 = a.offset + mul * b.offset;
                    auto temporary189 = a.step + mul * b.step;
                    out.offset = temporary188;
                    out.step = temporary189;
                    auto range190 = a.terms;
                    for (Int index191 = 0; index191 < mapSlots(range190); ++index191) {
                        if (!mapOccupied(range190, index191)) {
                            continue;
                        }
                        i32 v = mapKey(range190, index191);
                        u32 k = mapValue(range190, index191);
                        setMap(out.terms, v, k);
                    }
                    auto range193 = b.terms;
                    for (Int index194 = 0; index194 < mapSlots(range193); ++index194) {
                        if (!mapOccupied(range193, index194)) {
                            continue;
                        }
                        i32 v = mapKey(range193, index194);
                        u32 k = mapValue(range193, index194);
                        setMap(out.terms, v, out.terms[v] + mul * k);
                    }
                    break;
                }
                case OpShl:
                case OpMul: {
                    i32 a = args[0];
                    Value k = f->values[args[1]];
                    if (val.op == OpMul && k.op != OpConst) {
                        auto temporary198 = args[1];
                        auto temporary199 = f->values[args[0]];
                        a = temporary198;
                        k = temporary199;
                    }
                    if (k.op != OpConst) {
                        return bad;
                    }
                    CopyAddress base = memoryLoopCopyAddress(l, a, steps, invariants, cache, depth + 1);
                    if (!base.valid) {
                        return bad;
                    }
                    u32 mul = convert<u32>(k.aux);
                    if (val.op == OpShl) {
                        mul = shiftLeft(u32(1LL), mul & 31);
                    }
                    auto temporary200 = base.offset * mul;
                    auto temporary201 = base.step * mul;
                    out.offset = temporary200;
                    out.step = temporary201;
                    auto range202 = base.terms;
                    for (Int index203 = 0; index203 < mapSlots(range202); ++index203) {
                        if (!mapOccupied(range202, index203)) {
                            continue;
                        }
                        i32 v = mapKey(range202, index203);
                        u32 k = mapValue(range202, index203);
                        setMap(out.terms, v, k * mul);
                    }
                    break;
                }
                default: {
                    return bad;
                }
            }
        }
        auto range205 = out.terms;
        for (Int index206 = 0; index206 < mapSlots(range205); ++index206) {
            if (!mapOccupied(range205, index206)) {
                continue;
            }
            i32 v = mapKey(range205, index206);
            u32 k = mapValue(range205, index206);
            if (k == 0) {
                eraseMap(out.terms, v);
            }
        }
        setMap(cache, v, out);
        return out;
    }

    // A checked, contiguous byte-copy loop can use a string instruction.
    // Forward order preserves dependencies between overlapping loads and stores;
    // memory.copy's memmove semantics would change those dependencies.
    static i32 memoryLoopCopyLoop(MemoryLoop* l, i32 fast, i32 count, i32 size, Map<i32, bool> proven, Map<i32, i32> invariants) {
        Function* f = l->f;
        if (length(l->order) != 1) {
            return -1;
        }
        Slice<i32> loads = {};
        Slice<i32> stores = {};
        i32 pending = -1;
        auto instrs = f->blocks[l->header].instrs.view();
        for (Int i = 0; i < length(instrs); ++i) {
            i32 v = instrs[i];
            Value* val = &f->values[v];
            switch (val->op) {
                case OpLoad: {
                    if (pending >= 0 || !proven[v] || memKindWidth(convert<MemKind>(val->aux2)) != 1) {
                        return -1;
                    }
                    loads = appendValues(loads, v);
                    pending = v;
                    break;
                }
                case OpStore: {
                    if (pending < 0 || !proven[v] || memKindWidth(convert<MemKind>(val->aux2)) != 1 || functionArgs(f, v)[1] != pending) {
                        return -1;
                    }
                    stores = appendValues(stores, v);
                    pending = -1;
                    break;
                }
                default: {
                    if (readsState(val->op)) {
                        return -1;
                    }
                    break;
                }
            }
        }
        if (pending >= 0 || length(loads) == 0) {
            return -1;
        }
        Edge back = {};
        Edge exit = {};
        auto edges = f->blocks[l->header].term.edges;
        for (Int i = 0; i < length(edges); ++i) {
            Edge e = edges[i];
            if (e.block == l->header) {
                back = e;
            } else {
                exit = e;
            }
        }
        Slice<i64> steps = makeSlice<i64>(length(f->blocks[l->header].params));
        auto params = f->blocks[l->header].params;
        for (Int j = 0; j < length(params); ++j) {
            i32 p = params[j];
            if (f->values[p].typ != I32) {
                return -1;
            }
            steps[j] = inductionStep(f, p, back.args[j]);
            if (steps[j] == 0 && back.args[j] != p) {
                return -1;
            }
        }
        u64 width = convert<u64>(length(loads));
        Map<i32, CopyAddress> addresses = mapOf<i32, CopyAddress>({});
        auto contiguous = [&](Slice<i32> values) mutable -> bool {
            CopyAddress first = {};
            auto range218 = values;
            for (Int index219 = 0, rangeEnd220 = length(range218); index219 < rangeEnd220; ++index219) {
                Int j = index219;
                i32 v = range218[index219];
                CopyAddress addr = memoryLoopCopyAddress(l, functionArgs(f, v)[0], steps, invariants, addresses, 0);
                addr.offset += convert<u32>(f->values[v].aux);
                if (!addr.valid || addr.step != convert<u32>(width)) {
                    return false;
                }
                if (j == 0) {
                    first = addr;
                } else if (addr.offset != first.offset + convert<u32>(j) || length(addr.terms) != length(first.terms)) {
                    return false;
                }
                auto range221 = first.terms;
                for (Int index222 = 0; index222 < mapSlots(range221); ++index222) {
                    if (!mapOccupied(range221, index222)) {
                        continue;
                    }
                    i32 v = mapKey(range221, index222);
                    u32 mul = mapValue(range221, index222);
                    if (addr.terms[v] != mul) {
                        return false;
                    }
                }
            }
            return true;
        };
        if (!contiguous(loads) || !contiguous(stores)) {
            return -1;
        }
        Map<i32, i32> reverse = mapOf<i32, i32>({});
        auto range224 = f->blocks[l->header].params;
        for (Int index225 = 0, rangeEnd226 = length(range224); index225 < rangeEnd226; ++index225) {
            Int j = index225;
            i32 p = range224[index225];
            setMap(reverse, f->blocks[fast].params[j], p);
        }
        Int index = 0;
        auto range227 = f->blocks[l->header].instrs.view();
        for (Int index228 = 0, rangeEnd229 = length(range227); index228 < rangeEnd229; ++index228) {
            i32 v = range227[index228];
            auto temporary230 = lookupMap(invariants, v);
            bool hoisted = temporary230.v1;
            if (!hoisted) {
                setMap(reverse, f->blocks[fast].instrs[index], v);
                index++;
            }
        }
        Fn<bool(i32, Int)> can = {};
        can = [&](i32 v, Int depth) mutable -> bool {
            Value* val = &f->values[v];
            auto temporary231 = lookupMap(invariants, v);
            bool hoisted = temporary231.v1;
            if (hoisted || val->block != l->header || val->op == OpParam) {
                return true;
            }
            if (depth > 24 || readsState(val->op)) {
                return false;
            }
            auto range232 = functionArgs(f, v);
            for (Int index233 = 0, rangeEnd234 = length(range232); index233 < rangeEnd234; ++index233) {
                i32 a = range232[index233];
                if (!can(a, depth + 1)) {
                    return false;
                }
            }
            return true;
        };
        i32 source = functionArgs(f, loads[0])[0];
        i32 dest = functionArgs(f, stores[0])[0];
        if (!can(source, 0) || !can(dest, 0)) {
            return -1;
        }
        Edge fastExit = f->blocks[fast].term.edges[0];
        if (fastExit.block == fast) {
            fastExit = f->blocks[fast].term.edges[1];
        }
        auto original = [&](i32 v) mutable -> i32 {
            auto temporary237 = lookupMap(reverse, v);
            i32 old = temporary237.v0;
            bool ok = temporary237.v1;
            if (ok) {
                return old;
            }
            return v;
        };
        auto range238 = fastExit.args;
        for (Int index239 = 0, rangeEnd240 = length(range238); index239 < rangeEnd240; ++index239) {
            i32 v = range238[index239];
            if (!can(original(v), 0)) {
                return -1;
            }
        }
        i32 bulk = functionNewBlock(f);
        i32 current = bulk;
        auto range241 = f->blocks[fast].params;
        for (Int index242 = 0, rangeEnd243 = length(range241); index242 < rangeEnd243; ++index242) {
            Int j = index242;
            i32 p = range241[index242];
            i32 v = functionNewValue(f, OpParam, f->values[p].typ, bulk, convert<i64>(j), Span<const i32>({}));
            f->blocks[bulk].params = appendValues(f->blocks[bulk].params, v);
        }
        auto emit = [&](Op op, ValType typ, i64 aux, Slice<i32> args) mutable -> i32 {
            i32 v = functionNewValue(f, op, typ, current, aux, args);
            f->blocks[current].instrs.pushBack(v);
            return v;
        };
        auto constant = [&](ValType typ, i64 x) mutable -> i32 {
            return functionNewValue(f, OpConst, typ, -1, x, Span<const i32>({}));
        };
        i32 iterations = emit(OpExtendU, I64, i64(0LL), sliceOf<i32>({count}));
        if (l->limit < 0 && (l->countStep != 1 || l->countStop != 0)) {
            iterations = emit(OpSub, I64, i64(0LL), sliceOf<i32>({iterations, constant(I64, convert<i64>(l->countStop + 1))}));
            if ((l->countStep & (l->countStep - 1)) == 0) {
                iterations = emit(OpShrU, I64, i64(0LL), sliceOf<i32>({iterations, constant(I64, convert<i64>(bitsTrailingZeros64(l->countStep)))}));
            } else {
                iterations = emit(OpDivU, I64, i64(0LL), sliceOf<i32>({iterations, constant(I64, convert<i64>(l->countStep))}));
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
            Value val = f->values[v];
            if (val.block != l->header) {
                return v;
            }
            i32 mapped = {};
            if (val.op == OpParam) {
                mapped = f->blocks[bulk].params[val.aux];
                if (final && steps[val.aux] != 0) {
                    i32 delta = emit(OpMul, I32, i64(0LL), sliceOf<i32>({last, constant(I32, steps[val.aux])}));
                    mapped = emit(OpAdd, I32, i64(0LL), sliceOf<i32>({mapped, delta}));
                }
            } else {
                Slice<i32> args = appendSlice(Slice<i32>{}, functionArgs(f, v));
                auto range246 = args;
                for (Int index247 = 0, rangeEnd248 = length(range246); index247 < rangeEnd248; ++index247) {
                    Int j = index247;
                    i32 a = range246[index247];
                    args[j] = evaluate(a, final, cache);
                }
                mapped = emit(val.op, val.typ, val.aux, args);
                f->values[mapped].aux2 = val.aux2;
            }
            setMap(cache, v, mapped);
            return mapped;
        };
        Map<i32, i32> initial = mapOf<i32, i32>({});
        i32 src = evaluate(source, false, initial);
        i32 dst = evaluate(dest, false, initial);
        {
            i64 off = f->values[loads[0]].aux;
            if (off != 0) {
                src = emit(OpAdd, I32, i64(0LL), sliceOf<i32>({src, constant(I32, off)}));
            }
        }
        i64 off = f->values[stores[0]].aux;
        if (off != 0) {
            dst = emit(OpAdd, I32, i64(0LL), sliceOf<i32>({dst, constant(I32, off)}));
        }
        i32 lengthValue = emit(OpMul, I64, i64(0LL), sliceOf<i32>({iterations, constant(I64, convert<i64>(width))}));
        i32 condition = emit(OpGeU, I32, i64(0LL), sliceOf<i32>({lengthValue, constant(I64, i64(16LL))}));
        if (l->countSigned) {
            i32 positive = emit(OpLeU, I32, i64(0LL), sliceOf<i32>({count, constant(I32, i64(2147483647LL))}));
            condition = emit(OpAnd, I32, i64(0LL), sliceOf<i32>({condition, positive}));
        }
        auto range251 = Array<i32, 2>{{src, dst}};
        for (Int index252 = 0, rangeEnd253 = length(range251); index252 < rangeEnd253; ++index252) {
            i32 addr = range251[index252];
            i32 end = emit(OpAdd, I64, i64(0LL), sliceOf<i32>({emit(OpExtendU, I64, i64(0LL), sliceOf<i32>({addr})), lengthValue}));
            condition = emit(OpAnd, I32, i64(0LL), sliceOf<i32>({condition, emit(OpLeU, I32, i64(0LL), sliceOf<i32>({end, size}))}));
        }
        current = functionNewBlock(f);
        f->blocks[bulk].term = Term{TermIf, condition, sliceOf<Edge>({Edge{current, {}}, Edge{fast, appendSlice(Slice<i32>{}, f->blocks[bulk].params)}}), {}, {}};
        emit(OpCopyForward, TNone, i64(0LL), sliceOf<i32>({dst, src, lengthValue}));
        Map<i32, i32> final = mapOf<i32, i32>({});
        Slice<i32> args = makeSlice<i32>(length(fastExit.args));
        auto range254 = fastExit.args;
        for (Int index255 = 0, rangeEnd256 = length(range254); index255 < rangeEnd256; ++index255) {
            Int j = index255;
            i32 v = range254[index255];
            args[j] = evaluate(original(v), true, final);
        }
        f->blocks[current].term = Term{TermJump, {}, sliceOf<Edge>({Edge{exit.block, args}}), {}, {}};
        return bulk;
    }

    // With an entry that bypasses the loop, existing exit parameters already
    // merge the two paths. A new parameter for a collapsed live-out value
    // would need an undefined argument on that bypass, so leave that case alone.
    static bool memoryLoopHasLiveOut(MemoryLoop* l) {
        Function* f = l->f;
        auto inside = [&](i32 v) mutable -> bool {
            i32 b = f->values[v].block;
            return b >= 0 && l->members[b];
        };
        // The header dominates every definition in the loop and therefore
        // every direct use. Merged exit parameters have their own
        // definitions outside the loop.
        return loopEachDominated(l->ci, l->header, [&](i32 b) -> bool {
            if (l->members[b] || f->blocks[b].dead) {
                return false;
            }
            Block* blk = &f->blocks[b];
            auto instrs = blk->instrs.view();
            for (Int i = 0; i < length(instrs); ++i) {
                i32 v = instrs[i];
                auto args = functionArgs(f, v);
                for (Int i = 0; i < length(args); ++i) {
                    i32 a = args[i];
                    if (inside(a)) {
                        return true;
                    }
                }
            }
            if ((blk->term.kind == TermIf || blk->term.kind == TermSwitch) && inside(blk->term.value)) {
                return true;
            }
            auto values = blk->term.values;
            for (Int i = 0; i < length(values); ++i) {
                i32 v = values[i];
                if (inside(v)) {
                    return true;
                }
            }
            auto edges = blk->term.edges;
            for (Int i = 0; i < length(edges); ++i) {
                Edge e = edges[i];
                auto args = e.args;
                for (Int i = 0; i < length(args); ++i) {
                    i32 v = args[i];
                    if (inside(v)) {
                        return true;
                    }
                }
            }
            return false;
        });
    }

    static bool memoryLoopInvariant(MemoryLoop* l, i32 v, Int depth) {
        Value* val = &l->f->values[v];
        if (val->block < 0) {
            return true;
        }
        if (!l->members[val->block]) {
            return loopDominates(l->ci, val->block, l->header);
        }
        auto [known, ok] = lookupMap(l->invariantCache, v);
        if (ok) {
            return known;
        }
        setMap(l->invariantCache, v, false);
        if (depth > 32 || (val->typ != I32 && val->typ != I64 && val->typ != F32 && val->typ != F64)) {
            return false;
        }
        if (val->op == OpLoad) {
            if (!l->allowLoads || length(l->invariantNodes) >= 128 || memKindWidth(convert<MemKind>(val->aux2)) == 0) {
                return false;
            }
        } else if (readsState(val->op)) {
            return false;
        }
        auto args = functionArgs(l->f, v);
        for (Int i = 0; i < length(args); ++i) {
            i32 a = args[i];
            if (!memoryLoopInvariant(l, a, depth + 1)) {
                return false;
            }
        }
        setMap(l->invariantCache, v, true);
        l->invariantNodes = appendValues(l->invariantNodes, v);
        return true;
    }

    static LoopSpan memoryLoopSpan(MemoryLoop* l, i32 v, Int depth) {
        {
            auto temporary279 = lookupMap(l->induction, v);
            LoopSpan bound = temporary279.v0;
            bool ok = temporary279.v1;
            if (ok) {
                return bound;
            }
        }
        {
            auto temporary280 = lookupMap(l->cache, v);
            LoopSpan bound = temporary280.v0;
            bool ok = temporary280.v1;
            if (ok) {
                return bound;
            }
        }
        LoopSpan bad = LoopSpan{};
        setMap(l->cache, v, bad);
        Value* val = &l->f->values[v];
        if (val->typ != I32 || depth > 32) {
            return bad;
        }
        LoopSpan out = bad;
        if (val->op == OpConst) {
            out = LoopSpan{-1, {}, {}, convert<u64>(convert<u32>(val->aux)), {}, {}, convert<u64>(convert<u32>(val->aux)), {}, true};
        } else if (memoryLoopInvariant(l, v, 0)) {
            out = LoopSpan{v, u64(1LL), {}, {}, {}, {}, {}, {}, true};
            const ValueRange range = valueRangesOf(l->valueRanges, v, 0);
            out.signedTerms = range.lo < 0 || range.hi >= i64(2147483648LL);
        } else {
            ValueRange bound = valueRangesOf(l->valueRanges, v, 0);
            // A bounded value spans its range; one reaching far past the
            // memory the module starts with would hardly ever pass a guard.
            const i64 reach = maximum(i64(l->f->module->mems[0].limits.min * u64(65536)), i64(67108864LL));
            if (bound.lo >= 0 && bound.hi < i64(4294967295LL)) {
                if (bound.hi <= reach) {
                    out = LoopSpan{-1, {}, {}, u64(bound.hi), {}, {}, u64(bound.lo), {}, true};
                }
            } else {
                Span<i32> args = functionArgs(l->f, v);
                switch (val->op) {
                    case OpAdd: {
                        LoopSpan a = memoryLoopSpan(l, args[0], depth + 1);
                        LoopSpan b = memoryLoopSpan(l, args[1], depth + 1);
                        out = addLoopSpans(a, b);
                        if (out.valid && !out.signedTerms) {
                            setMap(l->noWrap, v, true);
                        }
                        break;
                    }
                    case OpShl: {
                        if (l->f->values[args[1]].op == OpConst) {
                            u32 n = convert<u32>(l->f->values[args[1]].aux) & 31;
                            if (n <= 16) {
                                out = memoryLoopSpan(l, args[0], depth + 1);
                                out.mul = shiftLeft(out.mul, n);
                                out.mul2 = shiftLeft(out.mul2, n);
                                out.factor = shiftLeft(out.factor, n);
                                out.offset = shiftLeft(out.offset, n);
                                out.lo = shiftLeft(out.lo, n);
                            }
                        }
                        break;
                    }
                }
            }
        }
        if (out.mul > 65536 || out.mul2 > 65536 || out.factor > 65536 || out.offset > u64(1099511627776LL)) {
            out.valid = false;
        }
        setMap(l->cache, v, out);
        return out;
    }

    static i32 memoryLoopVersion(MemoryLoop* l) {
        Int attempt = 0;
        for (; attempt < 2; attempt++) {
            auto range284 = l->induction;
            for (Int index285 = 0; index285 < mapSlots(range284); ++index285) {
                if (!mapOccupied(range284, index285)) {
                    continue;
                }
                i32 p = mapKey(range284, index285);
                LoopSpan bound = mapValue(range284, index285);
                if (bound.stride != 0) {
                    eraseMap(l->induction, p);
                }
            }
            l->allowLoads = attempt == 0 && l->f->module->importedFuncs == 0 && !l->f->module->mems[0].imported;
            auto order = l->order;
            for (Int i = 0; i < length(order); ++i) {
                i32 b = order[i];
                auto instrs = l->f->blocks[b].instrs.view();
                for (Int i = 0; i < length(instrs); ++i) {
                    i32 v = instrs[i];
                    switch (l->f->values[v].op) {
                        case OpCall:
                        case OpCallIndirect:
                        case OpMemCopy:
                        case OpMemFill:
                        case OpMemInit:
                        case OpCopyForward: {
                            l->allowLoads = false;
                            break;
                        }
                    }
                }
            }
            l->invariantCache = mapOf<i32, bool>({});
            l->invariantNodes = {};
            l->cache = mapOf<i32, LoopSpan>({});
            l->noWrap = mapOf<i32, bool>({});
            i32 fast = memoryLoopVersionOne(l);
            if (fast >= 0) {
                return fast;
            }
        }
        return -1;
    }

    static i32 memoryLoopVersionOne(MemoryLoop* l) {
        Function* f = l->f;
        if (l->limit >= 0 && !memoryLoopInvariant(l, l->limit, 0)) {
            return -1;
        }
        Slice<i32> switches = {};
        auto order = l->order;
        for (Int i = 0; i < length(order); ++i) {
            i32 b = order[i];
            Term* t = &f->blocks[b].term;
            if (t->kind == TermSwitch && length(t->edges) >= 5 && t->edges[0].block != t->edges[length(t->edges) - 1].block && memoryLoopInvariant(l, t->value, 0)) {
                switches = appendValues(switches, b);
            }
        }
        Slice<LoopSpan> writes = {};
        Map<RangeKeyLoop_version570, Int> writeIndex = mapOf<RangeKeyLoop_version570, Int>({});
        auto keyOf = [&](LoopSpan bound) mutable -> RangeKeyLoop_version570 {
            return RangeKeyLoop_version570{bound.base, bound.base2, bound.stride, bound.mul, bound.mul2, bound.factor};
        };
        auto edges = f->blocks[l->latch].term.edges;
        for (Int i = 0; i < length(edges); ++i) {
            Edge e = edges[i];
            if (e.block != l->header) {
                continue;
            }
            auto params = f->blocks[l->header].params;
            for (Int j = 0; j < length(params); ++j) {
                i32 p = params[j];
                auto temporary303 = lookupMap(l->induction, p);
                bool known = temporary303.v1;
                if (known) {
                    continue;
                }
                auto temporary304 = inductionTerms(f, e.args[j], p, 0);
                Slice<i32> terms = temporary304.v0;
                bool ok = temporary304.v1;
                if (!ok) {
                    continue;
                }
                LoopSpan bound = LoopSpan{l->entry.args[j], u64(1LL), {}, {}, {}, {}, {}, {}, true};
                for (Int i = 0; i < length(terms); ++i) {
                    i32 v = terms[i];
                    Value* val = &f->values[v];
                    if (val->op == OpConst && bound.stride == 0 && val->aux >= 0 && val->aux <= 65536) {
                        bound.factor += convert<u64>(val->aux);
                    } else if (val->op != OpConst && (bound.factor == 0 || bound.stride == v + 1) && memoryLoopInvariant(l, v, 0)) {
                        bound.stride = v + 1;
                        bound.factor++;
                    } else {
                        bound.valid = false;
                        break;
                    }
                }
                if (bound.valid && bound.factor <= 65536) {
                    setMap(l->induction, p, bound);
                }
            }
        }
        Map<RangeKeyLoop_version570, Int> indices = mapOf<RangeKeyLoop_version570, Int>({});
        Slice<LoopSpan> guards = {};
        Map<i32, bool> proven = mapOf<i32, bool>({});
        bool unboundedWrite = false;
        // An access at an invariant address is usually checked once before
        // the loop already; the version must free one that moves.
        bool moving = false;
        auto range308 = l->order;
        for (Int index309 = 0, rangeEnd310 = length(range308); index309 < rangeEnd310; ++index309) {
            i32 b = range308[index309];
            auto instrs = f->blocks[b].instrs.view();
            for (Int i = 0; i < length(instrs); ++i) {
                i32 v = instrs[i];
                Value* val = &f->values[v];
                if ((val->op != OpLoad && val->op != OpStore) || MemKind(convert<MemKind>(val->aux2) & memProven) != MemKind(0LL)) {
                    continue;
                }
                i32 addr = functionArgs(f, v)[0];
                u64 end = convert<u64>(val->aux) + convert<u64>(memKindWidth(convert<MemKind>(val->aux2)));
                LoopSpan bound = memoryLoopSpan(l, addr, 0);
                if (val->op == OpStore) {
                    if (bound.valid) {
                        LoopSpan write = bound;
                        write.lo += convert<u64>(val->aux);
                        write.offset += end;
                        RangeKeyLoop_version570 key = keyOf(write);
                        auto temporary314 = lookupMap(writeIndex, key);
                        Int j = temporary314.v0;
                        bool known = temporary314.v1;
                        if (known) {
                            writes[j].lo = minimum(writes[j].lo, write.lo);
                            writes[j].offset = maximum(writes[j].offset, write.offset);
                        } else {
                            setMap(writeIndex, key, length(writes));
                            writes = appendValues(writes, write);
                        }
                    } else {
                        unboundedWrite = true;
                    }
                }
                const ValueRange range = valueRangesOf(l->valueRanges, addr, 0);
                if (range.lo >= 0 && range.hi + i64(end) <= i64(f->module->mems[0].limits.min * 65536) && !f->module->mems[0].imported) {
                    continue;
                }
                if (!bound.valid) {
                    continue;
                }
                bound.offset += end;
                RangeKeyLoop_version570 key = keyOf(bound);
                auto temporary315 = lookupMap(indices, key);
                Int j = temporary315.v0;
                bool ok = temporary315.v1;
                if (ok) {
                    guards[j].offset = maximum(guards[j].offset, bound.offset);
                } else {
                    setMap(indices, key, length(guards));
                    guards = appendValues(guards, bound);
                }
                setMap(proven, v, true);
                moving = moving || bound.factor != 0;
            }
        }
        bool hasLoad = false;
        auto invariantNodes = l->invariantNodes;
        for (Int i = 0; i < length(invariantNodes); ++i) {
            i32 v = invariantNodes[i];
            hasLoad = hasLoad || f->values[v].op == OpLoad;
        }
        if (length(proven) < 2 || !moving || length(guards) > 24 || (unboundedWrite && hasLoad)) {
            return -1;
        }
        u64 minimum = 4;
        if (length(proven) < 4) {
            minimum = 8;
        }
        if (hasLoad) {
            minimum = 16;
        }
        u64 minimumCount = (minimum - 1) * l->countStep + l->countStop + 1;
        if (l->count >= 0 && l->countBias < 0) {
            const ValueRange range = valueRangesOf(l->valueRanges, l->count, 0);
            if (range.lo >= 0 && range.hi < i64(minimumCount)) {
                return -1;
            }
        }
        if (l->limit >= 0) {
            // What the ranges of the start and the limit say about the
            // iterations: a loop known to be short is not worth a guard.
            const ValueRange startRange = valueRangesOf(l->valueRanges, l->start, 0);
            const ValueRange limitRange = valueRangesOf(l->valueRanges, l->limit, 0);
            const i64 lo = startRange.lo < 0 ? 0 : startRange.lo;
            const i64 hi = limitRange.hi > i64(4294967295LL) ? i64(4294967295LL) : limitRange.hi;
            if (hi < lo) {
                return -1;
            }
            const i64 distance = hi - lo + (l->inclusive ? 1 : 0);
            const i64 step = l->startStep;
            if ((distance + step - 1) / step < i64(minimum)) {
                return -1;
            }
        }
        i32 guard = functionNewBlock(f);
        i32 current = guard;
        auto emit = [&](Op op, ValType typ, Slice<i32> args) mutable -> i32 {
            i32 v = functionNewValue(f, op, typ, current, 0, args);
            f->blocks[current].instrs.pushBack(v);
            return v;
        };
        auto constant = [&](ValType typ, u64 x) mutable -> i32 {
            return functionNewValue(f, OpConst, typ, -1, convert<i64>(x), Span<const i32>({}));
        };
        i32 pages = emit(OpMemSize, I32, sliceOf<i32>({}));
        i32 size = emit(OpShl, I64, sliceOf<i32>({emit(OpExtendU, I64, sliceOf<i32>({pages})), constant(I64, u64(16LL))}));
        i32 condition = constant(I32, u64(1LL));
        i32 countValue = l->count;
        if (l->count >= 0) {
            if (l->countBias >= 0) {
                countValue = emit(OpAdd, I32, sliceOf<i32>({countValue, l->countBias}));
            }
            condition = emit(OpGeU, I32, sliceOf<i32>({countValue, constant(I32, minimumCount)}));
            if (l->countMask != 0) {
                i32 aligned = emit(OpEqz, I32, sliceOf<i32>({emit(OpAnd, I32, sliceOf<i32>({countValue, constant(I32, l->countMask)}))}));
                condition = emit(OpAnd, I32, sliceOf<i32>({condition, aligned}));
            }
        }
        Map<i32, i32> invariants = mapOf<i32, i32>({});
        auto mapValue = [&](i32 v) mutable -> i32 {
            auto temporary319 = lookupMap(invariants, v);
            i32 mapped = temporary319.v0;
            bool ok = temporary319.v1;
            if (ok) {
                return mapped;
            }
            return v;
        };
        Slice<LoadRangeLoop_version741> reads = {};
        Map<CseKey, i32> available = mapOf<CseKey, i32>({});
        if (length(l->invariantNodes) > 0) {
            i32 next = functionNewBlock(f);
            f->blocks[current].term = Term{TermIf, condition, sliceOf<Edge>({Edge{next, {}}, l->entry}), {}, {}};
            current = next;
        }
        auto range320 = l->invariantNodes;
        for (Int index321 = 0, rangeEnd322 = length(range320); index321 < rangeEnd322; ++index321) {
            i32 v = range320[index321];
            Value val = f->values[v];
            Slice<i32> args = appendSlice(Slice<i32>{}, functionArgs(f, v));
            for (Int j = 0; j < length(args); ++j) {
                args[j] = mapValue(args[j]);
            }
            CseKey key = CseKey{val.op, val.typ, val.nargs, val.aux, val.aux2, {}};
            bool cacheable = length(args) <= 3;
            if (cacheable) {
                copySlice(subslice(key.args, 0, length(key.args)), args);
                auto temporary326 = lookupMap(available, key);
                i32 n = temporary326.v0;
                bool known = temporary326.v1;
                if (known) {
                    setMap(invariants, v, n);
                    continue;
                }
            }
            if (val.op == OpLoad) {
                i32 addr = emit(OpExtendU, I64, sliceOf<i32>({args[0]}));
                i32 begin = emit(OpAdd, I64, sliceOf<i32>({addr, constant(I64, convert<u64>(val.aux))}));
                i32 end = emit(OpAdd, I64, sliceOf<i32>({begin, constant(I64, convert<u64>(memKindWidth(convert<MemKind>(val.aux2))))}));
                i32 ok = emit(OpLeU, I32, sliceOf<i32>({end, size}));
                i32 next = functionNewBlock(f);
                f->blocks[current].term = Term{TermIf, ok, sliceOf<Edge>({Edge{next, {}}, l->entry}), {}, {}};
                current = next;
                reads = appendValues(reads, LoadRangeLoop_version741{begin, end});
                val.aux2 |= 32;
            }
            i32 n = emit(val.op, val.typ, args);
            auto temporary327 = val.aux;
            auto temporary328 = val.aux2;
            f->values[n].aux = temporary327;
            f->values[n].aux2 = temporary328;
            setMap(invariants, v, n);
            if (cacheable) {
                setMap(available, key, n);
            }
        }
        if (l->limit >= 0) {
            i32 limit = mapValue(l->limit);
            auto both = [&](i32 a, i32 b) mutable -> i32 {
                return emit(OpAnd, I32, sliceOf<i32>({a, b}));
            };
            // The variable runs from start to the limit, which an inclusive
            // loop reaches: the iterations are the distance in steps.
            condition = emit(l->inclusive ? OpLeU : OpLtU, I32, sliceOf<i32>({l->start, limit}));
            if (l->signedLimit) {
                condition = both(condition, emit(OpLeU, I32, sliceOf<i32>({limit, constant(I32, u64(2147483647LL))})));
            }
            i32 distance = emit(OpSub, I64, sliceOf<i32>({emit(OpExtendU, I64, sliceOf<i32>({limit})), emit(OpExtendU, I64, sliceOf<i32>({l->start}))}));
            if (l->inclusive) {
                distance = emit(OpAdd, I64, sliceOf<i32>({distance, constant(I64, u64(1LL))}));
                condition = both(condition, emit(OpLeU, I32, sliceOf<i32>({distance, constant(I64, u64(4294967295LL))})));
            }
            if (l->startStride < 0 && l->startStep == 1) {
                countValue = emit(OpWrap, I32, sliceOf<i32>({distance}));
            } else {
                i32 step = constant(I64, convert<u64>(l->startStep));
                if (l->startStride >= 0) {
                    // The guard's arithmetic runs whatever the stride is: a
                    // stride outside 1..65536 fails it and divides as 1.
                    i32 stride = mapValue(l->startStride);
                    i32 small = emit(OpLtU, I32, sliceOf<i32>({emit(OpSub, I32, sliceOf<i32>({stride, constant(I32, u64(1LL))})), constant(I32, u64(65536LL))}));
                    condition = both(condition, small);
                    i32 strides = emit(OpSelect, I64, sliceOf<i32>({small, emit(OpExtendU, I64, sliceOf<i32>({stride})), constant(I64, u64(1LL))}));
                    step = emit(OpMul, I64, sliceOf<i32>({strides, step}));
                }
                if (l->exact) {
                    condition = both(condition, emit(OpEqz, I32, sliceOf<i32>({emit(OpRemU, I64, sliceOf<i32>({distance, step}))})));
                    countValue = emit(OpWrap, I32, sliceOf<i32>({emit(OpDivU, I64, sliceOf<i32>({distance, step}))}));
                } else {
                    // Advancing the variable while it passes the test must
                    // not wrap, nor change sign under a signed test, so that
                    // it never falls below its start.
                    i32 beyond = emit(OpAdd, I64, sliceOf<i32>({emit(OpExtendU, I64, sliceOf<i32>({limit})), step}));
                    const u64 most = (l->signedLimit ? u64(2147483648LL) : u64(4294967296LL)) - (l->inclusive ? 1 : 0);
                    condition = both(condition, emit(OpLeU, I32, sliceOf<i32>({beyond, constant(I64, most)})));
                    i32 rounded = emit(OpAdd, I64, sliceOf<i32>({distance, emit(OpSub, I64, sliceOf<i32>({step, constant(I64, u64(1LL))}))}));
                    countValue = emit(OpWrap, I32, sliceOf<i32>({emit(OpDivU, I64, sliceOf<i32>({rounded, step}))}));
                }
            }
            condition = both(condition, emit(OpGeU, I32, sliceOf<i32>({countValue, constant(I32, minimum)})));
        }
        // The conditions so far are cheap and decide most entries that take
        // the original loop; the guards of the accesses follow in a block of
        // their own.
        if (f->values[condition].op != OpConst) {
            i32 next = functionNewBlock(f);
            f->blocks[current].term = Term{TermIf, condition, sliceOf<Edge>({Edge{next, {}}, l->entry}), {}, {}};
            current = next;
            condition = constant(I32, u64(1LL));
        }
        i32 count = emit(OpExtendU, I64, sliceOf<i32>({countValue}));
        i32 iterations = emit(OpSub, I64, sliceOf<i32>({count, constant(I64, u64(1LL))}));
        auto evaluate = [&](LoopSpan bound) mutable -> Tuple2<i32, i32> {
            i32 end = constant(I64, bound.offset);
            i32 begin = constant(I64, bound.lo);
            auto range329 = Array<Array<i64, 2>, 2>{{Array<i64, 2>{{convert<i64>(bound.base), convert<i64>(bound.mul)}}, Array<i64, 2>{{convert<i64>(bound.base2), convert<i64>(bound.mul2)}}}};
            for (Int index330 = 0, rangeEnd331 = length(range329); index330 < rangeEnd331; ++index330) {
                Array<i64, 2> term = range329[index330];
                if (term[1] == 0) {
                    continue;
                }
                i32 base = emit(bound.signedTerms ? OpExtendS : OpExtendU, I64, sliceOf<i32>({mapValue(convert<i32>(term[0]))}));
                if (term[1] != 1) {
                    base = emit(OpMul, I64, sliceOf<i32>({base, constant(I64, convert<u64>(term[1]))}));
                }
                end = emit(OpAdd, I64, sliceOf<i32>({end, base}));
                begin = emit(OpAdd, I64, sliceOf<i32>({begin, base}));
            }
            if (bound.factor != 0) {
                i32 distance = emit(OpMul, I64, sliceOf<i32>({iterations, constant(I64, bound.factor)}));
                if (bound.stride != 0) {
                    i32 stride = mapValue(bound.stride - 1);
                    condition = emit(OpAnd, I32, sliceOf<i32>({condition, emit(OpLeU, I32, sliceOf<i32>({stride, constant(I32, 65536 / bound.factor)}))}));
                    distance = emit(OpMul, I64, sliceOf<i32>({distance, emit(OpExtendU, I64, sliceOf<i32>({stride}))}));
                }
                end = emit(OpAdd, I64, sliceOf<i32>({end, distance}));
            }
            return {begin, end};
        };
        auto range332 = guards;
        for (Int index333 = 0, rangeEnd334 = length(range332); index333 < rangeEnd334; ++index333) {
            LoopSpan bound = range332[index333];
            auto temporary335 = evaluate(bound);
            i32 begin = temporary335.v0;
            i32 end = temporary335.v1;
            condition = emit(OpAnd, I32, sliceOf<i32>({condition, emit(OpLeU, I32, sliceOf<i32>({end, size}))}));
            if (bound.signedTerms) {
                condition = emit(OpAnd, I32, sliceOf<i32>({condition, emit(OpGeS, I32, sliceOf<i32>({begin, constant(I64, u64(0LL))}))}));
            }
        }
        auto range336 = writes;
        for (Int index337 = 0, rangeEnd338 = length(range336); index337 < rangeEnd338; ++index337) {
            LoopSpan write = range336[index337];
            auto temporary339 = evaluate(write);
            i32 begin = temporary339.v0;
            i32 end = temporary339.v1;
            condition = emit(OpAnd, I32, sliceOf<i32>({condition, emit(OpLeU, I32, sliceOf<i32>({end, size}))}));
            if (write.signedTerms) {
                condition = emit(OpAnd, I32, sliceOf<i32>({condition, emit(OpGeS, I32, sliceOf<i32>({begin, constant(I64, u64(0LL))}))}));
            }
            auto range340 = reads;
            for (Int index341 = 0, rangeEnd342 = length(range340); index341 < rangeEnd342; ++index341) {
                LoadRangeLoop_version741 read = range340[index341];
                i32 disjoint = emit(OpOr, I32, sliceOf<i32>({emit(OpLeU, I32, sliceOf<i32>({end, read.begin})), emit(OpLeU, I32, sliceOf<i32>({read.end, begin}))}));
                condition = emit(OpAnd, I32, sliceOf<i32>({condition, disjoint}));
            }
        }
        i32 fast = memoryLoopClone(l, proven, invariants, {});
        Edge entry = Edge{fast, appendSlice(Slice<i32>{}, l->entry.args)};
        i32 bulk = memoryLoopCopyLoop(l, fast, countValue, size, proven, invariants);
        if (bulk >= 0) {
            entry.block = bulk;
        }
        if (length(switches) >= 2) {
            i32 index = mapValue(f->blocks[switches[0]].term.value);
            Map<i32, Int> cases = mapOf<i32, Int>({});
            auto range343 = switches;
            for (Int index344 = 0, rangeEnd345 = length(range343); index344 < rangeEnd345; ++index344) {
                i32 b = range343[index344];
                if (mapValue(f->blocks[b].term.value) == index) {
                    setMap(cases, b, 0);
                }
            }
            if (length(cases) >= 2) {
                i32 specialized = memoryLoopClone(l, proven, invariants, cases);
                i32 choice = functionNewBlock(f);
                i32 eq = functionNewValue(f, OpEqz, I32, choice, 0, Span<const i32>({index}));
                f->blocks[choice].instrs.pushBack(eq);
                f->blocks[choice].term = Term{TermIf, eq, sliceOf<Edge>({Edge{specialized, appendSlice(Slice<i32>{}, l->entry.args)}, entry}), {}, {}};
                entry = Edge{choice, {}};
            }
        }
        f->blocks[current].term = Term{TermIf, condition, sliceOf<Edge>({entry, l->entry}), {}, {}};
        for (Int j = 0, end = length(f->blocks[l->preheader].term.edges); j < end; ++j) {
            if (f->blocks[l->preheader].term.edges[j].block == l->header) {
                f->blocks[l->preheader].term.edges[j] = Edge{guard, {}};
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
        Int steps = 0;
        auto next = [&]() mutable -> void {
            if (steps == 128) {
                parts = appendValues(parts, appendValues(code, u8(11LL)));
                code = sliceOf<u8>({u8(0LL)});
                steps = 0;
            }
            steps++;
        };
        {
            Int i = m->importedGlobals;
            for (; i < length(m->globals); i++) {
                next();
                Global* g = &m->globals[i];
                code = appendSlice(code, subslice(g->init, 0, length(g->init) - 1));
                code = appendValues(code, u8(36LL));
                code = appendLeb(code, convert<u64>(i));
            }
        }
        auto datas = m->datas;
        for (Int i = 0; i < length(datas); ++i) {
            Data d = datas[i];
            if (m->batchData || d.mode != segmentActive) {
                continue;
            }
            next();
            code = appendSlice(code, subslice(d.offset, 0, length(d.offset) - 1));
            code = appendValues(code, u8(65LL), u8(0LL), u8(65LL));
            code = appendSleb(code, convert<i64>(convert<i32>(convert<u32>(length(d.bytes)))));
            code = appendValues(code, u8(252LL), u8(8LL));
            code = appendLeb(code, convert<u64>(i));
            code = appendValues(code, u8(0LL), u8(252LL), u8(9LL));
            code = appendLeb(code, convert<u64>(i));
        }
        Int typ = -1;
        auto types = m->types;
        for (Int i = 0; i < length(types); ++i) {
            FuncType t = types[i];
            if (length(t.params) == 0 && length(t.results) == 0) {
                typ = i;
                break;
            }
        }
        if (typ < 0) {
            m->types = appendValues(m->types, FuncType{});
            typ = length(m->types) - 1;
        }
        if (length(parts) > 0) {
            parts = appendValues(parts, appendValues(code, u8(11LL)));
            code = sliceOf<u8>({u8(0LL)});
            auto range355 = parts;
            for (Int index356 = 0, rangeEnd357 = length(range355); index356 < rangeEnd357; ++index356) {
                Int k = index356;
                Slice<u8> body = range355[index356];
                Int fn = length(m->funcs);
                m->funcs = appendValues(m->funcs, Func{convert<u32>(typ), {}, {}, fmtSprintf(String("init.part%d"), k), body, {}});
                code = appendLeb(appendValues(code, u8(16LL)), convert<u64>(fn));
            }
        }
        if (m->start >= 0 && !m->batchData) {
            code = appendLeb(appendValues(code, u8(16LL)), convert<u64>(m->start));
        }
        code = appendValues(code, u8(11LL));
        if (m->dataCount < 0) {
            m->dataCount = convert<i64>(length(m->datas));
        }
        m->funcs = appendValues(m->funcs, Func{convert<u32>(typ), {}, {}, String("init"), code, {}});
        return length(m->funcs) - 1;
    }

    static void moduleDataSection(Module* m, Reader* r) {
        u32 n = readerU32(r);
        u32 i = 0;
        for (; i < n; i++) {
            u32 flags = readerU32(r);
            Data d = Data{};
            switch (flags) {
                case 0: {
                    d.offset = readerConstExpr(r);
                    break;
                }
                case 1: {
                    d.mode = segmentPassive;
                    break;
                }
                case 2: {
                    d.memory = readerU32(r);
                    d.offset = readerConstExpr(r);
                    break;
                }
                default: {
                    compilerError(String("malformed data segment kind %d"));
                    break;
                }
            }
            d.bytes = readerBytes(r, convert<i64>(readerU32(r)));
            m->datas = appendValues(m->datas, d);
        }
    }

    static void moduleElemSection(Module* m, Reader* r) {
        u32 n = readerU32(r);
        u32 i = 0;
        for (; i < n; i++) {
            u32 flags = readerU32(r);
            if (flags > 7) {
                compilerError(String("malformed elements segment kind %d"));
            }
            Elem e = Elem{{}, {}, {}, FuncRef, {}};
            if ((flags & 1) == 0) {
                e.mode = segmentActive;
            } else if ((flags & 2) == 0) {
                e.mode = segmentPassive;
            } else {
                e.mode = segmentDeclarative;
            }
            if ((flags & 1) == 0) {
                if ((flags & 2) != 0) {
                    e.table = readerU32(r);
                }
                e.offset = readerConstExpr(r);
            }
            bool exprs = (flags & 4) != 0;
            if ((flags & 3) != 0) {
                if (exprs) {
                    e.typ = readerRefType(r);
                } else {
                    u8 kind = readerByte(r);
                    if (kind != 0) {
                        compilerError(String("malformed element kind %d"));
                    }
                }
            }
            u32 count = readerU32(r);
            u32 j = 0;
            for (; j < count; j++) {
                if (exprs) {
                    e.exprs = appendValues(e.exprs, readerConstExpr(r));
                } else {
                    e.exprs = appendValues(e.exprs, refFuncExpr(readerU32(r)));
                }
            }
            m->elems = appendValues(m->elems, e);
        }
    }

    static void moduleExportSection(Module* m, Reader* r) {
        u32 n = readerU32(r);
        Map<String, bool> seen = mapOf<String, bool>({});
        u32 i = 0;
        for (; i < n; i++) {
            Export e = Export{readerName(r), readerByte(r), readerU32(r)};
            if (e.kind > externGlobal) {
                compilerError(String("malformed export kind %d"));
            }
            if (seen[e.name]) {
                compilerError(String("duplicate export name %q"));
            }
            setMap(seen, e.name, true);
            m->exports = appendValues(m->exports, e);
        }
    }

    static FuncType* moduleFuncType(Module* m, u32 index) {
        return &m->types[m->funcs[index].typ];
    }

    static void moduleImportSection(Module* m, Reader* r) {
        u32 n = readerU32(r);
        u32 i = 0;
        for (; i < n; i++) {
            Import imp = Import{readerName(r), readerName(r), readerByte(r), {}};
            switch (imp.kind) {
                case externFunc: {
                    imp.index = convert<u32>(length(m->funcs));
                    m->funcs = appendValues(m->funcs, Func{readerU32(r), true, returnsTwice(imp.name), {}, {}, {}});
                    m->importedFuncs++;
                    break;
                }
                case externTable: {
                    imp.index = convert<u32>(length(m->tables));
                    ValType elem = readerRefType(r);
                    m->tables = appendValues(m->tables, Table{elem, readerLimits(r, false), true});
                    m->importedTables++;
                    break;
                }
                case externMemory: {
                    imp.index = convert<u32>(length(m->mems));
                    m->mems = appendValues(m->mems, Memory{readerLimits(r, true), true});
                    m->importedMems++;
                    break;
                }
                case externGlobal: {
                    imp.index = convert<u32>(length(m->globals));
                    Global g = Global{readerValType(r), {}, true, {}};
                    g.mutableValue = globalMutability(r);
                    m->globals = appendValues(m->globals, g);
                    m->importedGlobals++;
                    break;
                }
                default: {
                    compilerError(String("malformed import kind %d"));
                    break;
                }
            }
            m->imports = appendValues(m->imports, imp);
        }
    }

    static void moduleTypeSection(Module* m, Reader* r) {
        u32 n = readerU32(r);
        u32 i = 0;
        for (; i < n; i++) {
            u8 form = readerByte(r);
            if (form != 96) {
                compilerError(String("malformed function type form 0x%02x"));
            }
            FuncType t = {};
            u32 np = readerU32(r);
            {
                u32 j = 0;
                for (; j < np; j++) {
                    t.params = appendValues(t.params, readerValType(r));
                }
            }
            u32 nr = readerU32(r);
            u32 j = 0;
            for (; j < nr; j++) {
                t.results = appendValues(t.results, readerValType(r));
            }
            m->types = appendValues(m->types, t);
        }
    }

    // Ranges are unsigned and inclusive. Arithmetic that could wrap stays
    // unknown; a mask, a narrow load or a shift can bound it again. A cyclic
    // dependency starts at the full range, so loop parameters never assume an
    // unproved bound from their back edge.
    // valueRangesOf bounds an i32 value by an interval of exact integers:
    // the value is one of them modulo 2^32, and when the interval lies in
    // [0, 2^32) the value itself. A subtraction may go negative, and an
    // address adding such a difference to a base is still in bounds when
    // the whole interval is.
    static ValueRange valueRangesOf(ValueRanges* r, i32 v, Int depth) {
        if (r->seen[v]) {
            return r->cache[v];
        }
        const ValueRange unknown = ValueRange{0, i64(4294967295LL)};
        r->seen[v] = true;
        r->cache[v] = unknown;
        Value* val = &r->f->values[v];
        if (val->typ != I32 || depth > 32) {
            return unknown;
        }
        constexpr i64 reach = i64(1LL) << 40;
        auto within = [&](i64 lo, i64 hi) {
            return lo > -reach && hi < reach && lo <= hi;
        };
        ValueRange out = unknown;
        Span<i32> args = functionArgs(r->f, v);
        switch (val->op) {
            case OpConst: {
                const i64 k = i64(convert<u32>(val->aux));
                out = ValueRange{k, k};
                break;
            }
            case OpLoad: {
                MemKind kind = convert<MemKind>(val->aux2);
                if (!memKindSigned(kind) && memKindWidth(kind) < 4) {
                    out.hi = (i64(1LL) << (memKindWidth(kind) * 8)) - 1;
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
                out.hi = 1;
                break;
            }
            case OpAnd:
            case OpAdd:
            case OpSub:
            case OpMul:
            case OpShrU:
            case OpShl: {
                ValueRange a = valueRangesOf(r, args[0], depth + 1);
                ValueRange b = valueRangesOf(r, args[1], depth + 1);
                switch (val->op) {
                    case OpAnd: {
                        // A mask bounds any bit pattern.
                        if (a.lo >= 0 && b.lo >= 0) {
                            out = ValueRange{0, minimum(a.hi, b.hi)};
                        } else if (b.lo == b.hi && b.lo >= 0) {
                            out = ValueRange{0, b.hi};
                        } else if (a.lo == a.hi && a.lo >= 0) {
                            out = ValueRange{0, a.hi};
                        }
                        break;
                    }
                    case OpAdd: {
                        if (within(a.lo + b.lo, a.hi + b.hi)) {
                            out = ValueRange{a.lo + b.lo, a.hi + b.hi};
                        }
                        break;
                    }
                    case OpSub: {
                        if (within(a.lo - b.hi, a.hi - b.lo)) {
                            out = ValueRange{a.lo - b.hi, a.hi - b.lo};
                        }
                        break;
                    }
                    case OpMul: {
                        // Bounds of an unknown value are 2^32 - 1: their
                        // product would wrap in 64 bits.
                        if (a.lo >= 0 && b.lo >= 0 && (b.hi == 0 || a.hi <= i64(4294967295LL) / b.hi)) {
                            out = ValueRange{a.lo * b.lo, a.hi * b.hi};
                        }
                        break;
                    }
                    case OpShrU: {
                        if (b.lo == b.hi && a.lo >= 0 && a.hi <= i64(4294967295LL)) {
                            const i64 shift = b.lo & 31;
                            out = ValueRange{a.lo >> shift, a.hi >> shift};
                        }
                        break;
                    }
                    case OpShl: {
                        if (b.lo == b.hi) {
                            const i64 factor = i64(1LL) << (b.lo & 31);
                            if (a.lo > -reach / factor && a.hi < reach / factor && within(a.lo * factor, a.hi * factor)) {
                                out = ValueRange{a.lo * factor, a.hi * factor};
                            }
                        }
                        break;
                    }
                }
                break;
            }
            case OpSelect: {
                ValueRange a = valueRangesOf(r, args[1], depth + 1);
                ValueRange b = valueRangesOf(r, args[2], depth + 1);
                out = ValueRange{minimum(a.lo, b.lo), maximum(a.hi, b.hi)};
                break;
            }
            case OpParam: {
                bool first = true;
                auto preds = r->f->blocks[val->block].preds;
                for (Int i = 0; i < length(preds); ++i) {
                    i32 p = preds[i];
                    auto edges = r->f->blocks[p].term.edges;
                    for (Int i = 0; i < length(edges); ++i) {
                        Edge e = edges[i];
                        if (e.block != val->block) {
                            continue;
                        }
                        ValueRange bound = valueRangesOf(r, e.args[val->aux], depth + 1);
                        if (first) {
                            out = bound;
                            first = false;
                        } else {
                            out = ValueRange{minimum(out.lo, bound.lo), maximum(out.hi, bound.hi)};
                        }
                    }
                }
                break;
            }
            case OpWrap: {
                if (r->f->values[args[0]].op == OpExtendU) {
                    out = valueRangesOf(r, functionArgs(r->f, args[0])[0], depth + 1);
                }
                break;
            }
        }
        if (out.lo > out.hi) {
            out = unknown;
        }
        r->cache[v] = out;
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
        auto range374 = a->active.view();
        for (Int index375 = 0, rangeEnd376 = length(range374); index375 < rangeEnd376; ++index375) {
            Interval* it = range374[index375];
            if (intervalEndsBefore(it, pos)) {
            } else if (!intervalCoversAt(it, pos)) {
                require(movedCount < 32, "too many active register intervals");
                moved[movedCount++] = it;
                nextChange = minimum(nextChange, it->ranges[it->cursor].from);
            } else {
                active[active.count++] = it;
                nextChange = minimum(nextChange, it->ranges[it->cursor].to);
            }
        }
        a->active.resize(active.count);
        Span<Interval*> inactive = subslice(a->inactive.view(), 0, 0);
        auto range379 = a->inactive.view();
        for (Int index380 = 0, rangeEnd381 = length(range379); index380 < rangeEnd381; ++index380) {
            Interval* it = range379[index380];
            if (intervalEndsBefore(it, pos)) {
            } else if (intervalCoversAt(it, pos)) {
                a->active.pushBack(it);
                nextChange = minimum(nextChange, it->ranges[it->cursor].to);
            } else {
                inactive[inactive.count++] = it;
                nextChange = minimum(nextChange, it->ranges[it->cursor].from);
            }
        }
        a->inactive.resize(inactive.count);
        a->inactive.append(Span<Interval* const>(moved, movedCount));
        return nextChange;
    }

    static void allocatorAllocateBlocked(Allocator* a, Interval* cur) {
        Array<i32, 32> nextUse = {};
        Array<i32, 32> blockPos = {};
        auto candidates = allocatorCandidates(a, cur->classValue);
        for (Int i = 0; i < length(candidates); ++i) {
            VReg r = candidates[i];
            nextUse[r] = maxPos;
            blockPos[r] = maxPos;
        }
        i32 start = intervalStart(cur);
        auto range387 = a->active.view();
        for (Int index388 = 0, rangeEnd389 = length(range387); index388 < rangeEnd389; ++index388) {
            Interval* it = range387[index388];
            if (vRegIsPhys(it->vreg)) {
                nextUse[it->reg] = 0;
                blockPos[it->reg] = 0;
                continue;
            }
            i32 u = intervalNextUse(it, start);
            if (u == start) {
                u = 0;
            }
            if (u < nextUse[it->reg]) {
                nextUse[it->reg] = u;
            }
        }
        auto range390 = a->inactive.view();
        for (Int index391 = 0, rangeEnd392 = length(range390); index391 < rangeEnd392; ++index391) {
            Interval* it = range390[index391];
            i32 x = intervalIntersection(it, cur);
            if (x == maxPos) {
                continue;
            }
            if (vRegIsPhys(it->vreg)) {
                if (x < blockPos[it->reg]) {
                    blockPos[it->reg] = x;
                }
                if (x < nextUse[it->reg]) {
                    nextUse[it->reg] = x;
                }
                continue;
            }
            i32 u = intervalNextUse(it, start);
            if (u < nextUse[it->reg]) {
                nextUse[it->reg] = u;
            }
        }
        VReg reg = noReg;
        auto range393 = allocatorCandidates(a, cur->classValue);
        for (Int index394 = 0, rangeEnd395 = length(range393); index394 < rangeEnd395; ++index394) {
            VReg r = range393[index394];
            if (reg == noReg || nextUse[r] > nextUse[reg]) {
                reg = r;
            }
        }
        i32 firstUse = intervalNextUse(cur, start);
        // A part of a value split at a dispatch header takes back the
        // register it has there whenever that register's next use comes
        // after the part's first.
        if (a->home.size()) {
            Interval* h = a->home[usize(allocatorVindex(a, cur->vreg))];
            if (h && h != cur && h->reg != noReg && intervalStart(h) < start && nextUse[h->reg] > firstUse && blockPos[h->reg] > start + 1) {
                reg = h->reg;
            }
        }
        if (firstUse > nextUse[reg] || blockPos[reg] <= start + 1 || nextUse[reg] == 0) {
            allocatorSpill(a, cur);
            if (firstUse < maxPos) {
                i32 at = allocatorSplitPos(a, start + 1, firstUse);
                if (at < 0) {
                    compilerError(String("internal error: no register for %s at %d"));
                }
                allocatorAddUnhandled(a, intervalSplit(cur, at));
            }
            return;
        }
        cur->reg = reg;
        a->used[reg] = true;
        if (blockPos[reg] < intervalEnd(cur)) {
            i32 at = allocatorSplitPos(a, start + 1, blockPos[reg]);
            if (at < 0) {
                compilerError(String("internal error: no split position for %s before %d"));
            }
            allocatorAddUnhandled(a, intervalSplit(cur, at));
        }
        Span<Interval*> active = subslice(a->active.view(), 0, 0);
        auto range396 = a->active.view();
        for (Int index397 = 0, rangeEnd398 = length(range396); index397 < rangeEnd398; ++index397) {
            Interval* it = range396[index397];
            if (it->reg != reg || vRegIsPhys(it->vreg)) {
                active[active.count++] = it;
                continue;
            }
            allocatorEvict(a, it, start);
        }
        a->active.resize(active.count);
        Span<Interval*> inactive = subslice(a->inactive.view(), 0, 0);
        auto range399 = a->inactive.view();
        for (Int index400 = 0, rangeEnd401 = length(range399); index400 < rangeEnd401; ++index400) {
            Interval* it = range399[index400];
            if (it->reg != reg || vRegIsPhys(it->vreg)) {
                inactive[inactive.count++] = it;
                continue;
            }
            i32 x = intervalIntersection(it, cur);
            if (x == maxPos) {
                inactive[inactive.count++] = it;
                continue;
            }
            i32 lo = intervalPrevUse(it, x) + 1;
            if (lo < start) {
                lo = start;
            }
            i32 at = allocatorSplitPos(a, lo, x);
            bool split = at >= 0 && at > intervalStart(it);
            if ((split && allocatorCanEvict(a, it, intervalCovered(it, at))) || (!split && allocatorCanEvict(a, it, start))) {
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
            i32 cut = allocatorSplitPos(a, start + 1, x);
            if (cut < 0) {
                compilerError(String("internal error: no split position for %s before %d"));
            }
            allocatorAddUnhandled(a, intervalSplit(cur, cut));
            inactive[inactive.count++] = it;
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
                    if (in->op == mCall && in->aux == calleeGo) {
                        // The function's callers keep the callee-saved
                        // registers across their call of it: it saves them,
                        // as Go writes them.
                        for (Int k = 0; k < length(calleeSaved); ++k) {
                            a->used[calleeSaved[k]] = true;
                        }
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
                        // A value invariant in the loop of its use is read from
                        // memory there rather than reloaded, when that can be.
                        if (vRegIsPhys(*r) || (needReg && !(mInstMemoryOperand(in, r) && a->invariantDepth[usize(allocatorVindex(a, *r))] < i32(mf->blocks[b].depth)))) {
                            intervalRecordUse(it, p);
                        } else if (needReg) {
                            it->memUses.pushBack(p);
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
        for (Int i = 0; i < length(calleeSaved); ++i) {
            VReg r = calleeSaved[i];
            if (a->used[r]) {
                out = appendValues(out, r);
            }
        }
        return out;
    }

    // canEvict tells whether evict can take the register from it at pos.
    static bool allocatorCanEvict(Allocator* a, Interval* it, i32 pos) {
        i32 start = intervalStart(it);
        if (start < pos) {
            i32 at = {};
            if ((pos & 1) != 0 && intervalPrevUse(it, pos) == pos - 1) {
                at = pos;
            } else {
                i32 lo = intervalPrevUse(it, pos) + 1;
                if (lo <= start) {
                    lo = start + 1;
                }
                at = allocatorSplitPos(a, lo, pos);
            }
            if (at >= 0 && at > start) {
                start = intervalCovered(it, at);
            } else if (intervalPrevUse(it, pos) >= start) {
                return false;
            }
        }
        i32 u = intervalNextUse(it, pos);
        if (u == maxPos) {
            return true;
        }
        i32 lo = pos;
        if (lo <= start) {
            lo = start + 1;
        }
        i32 at = allocatorSplitPos(a, lo, u);
        return at >= 0 && at > start;
    }

    static Slice<VReg> allocatorCandidates(Allocator* a, u8 classValue) {
        if (classValue == classXMM) {
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
        // A constant joins only a register its copy alone defines, so that
        // what they become is still a constant: joined into a register
        // defined otherwise too it would be saved and reloaded as a value.
        allocatorDefinitions(a, false);
        Vec<bool> constant(move(a->constant));
        Vec<bool> lone;
        lone.resize(usize(mf->nregs));
        lone.fill(true);
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
            if ((constant[usize(x)] || constant[usize(y)]) && (constant[usize(x)] || !lone[usize(x)] || a->definitions[usize(dx)] != 1)) {
                return;
            }
            CopyInterval* left = a->copyIntervals[usize(x)];
            CopyInterval* right = a->copyIntervals[usize(y)];
            if (!left || !right || !left->ranges.size() || !right->ranges.size() || overlap(left->ranges.view(), right->ranges.view())) {
                return;
            }
            parent[usize(y)] = x;
            constant[usize(x)] = constant[usize(x)] || constant[usize(y)];
            lone[usize(x)] = false;
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
        if (!joined) {
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
        for (Int b = 0, end = length(mf->blocks); b < end; ++b) {
            for (Int i = 0, end = length(mf->blocks[b].insts.view()); i < end; ++i) {
                MInst* in = &mf->blocks[b].insts[i];
                mInstForEachOperand(in, [&](VReg* r, Int role) mutable -> void {
                    if (*r != noReg && !vRegIsPhys(*r)) {
                        *r = firstVirtual + canonical[usize(find(i32(allocatorVindex(a, *r))))];
                    }
                });
                if (in->op == mParMove) {
                    mInstExtra(in)->moves.assign(dropSelfMoves(mInstMoves(in)));
                } else if ((in->op == mMov || in->op == mFMov) && in->dst == in->src && in->aux == 0) {
                    *in = inst(mNop);
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
        if (intervalStart(it) < pos) {
            i32 at = {};
            if ((pos & 1) != 0 && intervalPrevUse(it, pos) == pos - 1) {
                at = pos;
            } else {
                i32 lo = intervalPrevUse(it, pos) + 1;
                if (lo <= intervalStart(it)) {
                    lo = intervalStart(it) + 1;
                }
                at = allocatorSplitPos(a, lo, pos);
            }
            if (at >= 0 && at > intervalStart(it)) {
                tail = intervalSplit(it, at);
            } else if (intervalPrevUse(it, pos) < intervalStart(it)) {
            } else {
                compilerError(String("internal error: cannot evict %s at %d"));
            }
        }
        allocatorSpill(a, tail);
        i32 u = intervalNextUse(tail, pos);
        if (u == maxPos) {
            return;
        }
        i32 lo = pos;
        if (lo <= intervalStart(tail)) {
            lo = intervalStart(tail) + 1;
        }
        i32 at = allocatorSplitPos(a, lo, u);
        if (at < 0 || at <= intervalStart(tail)) {
            compilerError(String("internal error: cannot reload %s before %d"));
        }
        allocatorAddUnhandled(a, intervalSplit(tail, at));
    }

    static void allocatorHint(Allocator* a, VReg dst, VReg src) {
        if (vRegIsPhys(dst) && vRegIsPhys(src)) {
            return;
        }
        if (!vRegIsPhys(dst)) {
            Interval* it = allocatorInterval(a, dst);
            if (it->hint == noReg) {
                it->hint = src;
            }
        }
        if (!vRegIsPhys(src)) {
            Interval* it = allocatorInterval(a, src);
            if (it->hint == noReg) {
                it->hint = dst;
            }
        }
    }

    // hintReg returns the register the hint holds at the start of cur.
    static VReg allocatorHintReg(Allocator* a, Interval* cur) {
        VReg h = cur->hint;
        if (h == noReg) {
            return noReg;
        }
        if (vRegIsPhys(h)) {
            return h;
        }
        Interval* it = a->intervals[allocatorVindex(a, h)];
        if (isNil(it)) {
            return noReg;
        }
        i32 p = intervalStart(cur);
        for (Int i = -1; i < length(it->children); ++i) {
            Interval* c = i < 0 ? it : it->children[i];
            if (c->reg != noReg && (intervalCovers(c, p - 1) || intervalCovers(c, p) || intervalEnd(c) == p || intervalEnd(c) == p - 1)) {
                return c->reg;
            }
        }
        return noReg;
    }

    // siblingReg returns the register another part of cur's value has: the
    // part adjacent to cur, else the earliest allocated. Parts in one
    // register need no move between them where blocks meet.
    static VReg allocatorSiblingReg(Allocator* a, Interval* cur) {
        Interval* root = isNil(cur->parent) ? cur : cur->parent;
        if (root == cur && !length(root->children)) {
            return noReg;
        }
        const i32 start = intervalStart(cur);
        const i32 end = intervalEnd(cur);
        VReg earliest = noReg;
        for (Int i = -1; i < length(root->children); ++i) {
            Interval* c = i < 0 ? root : root->children[i];
            if (c == cur || c->reg == noReg) {
                continue;
            }
            if (intervalEnd(c) == start || intervalStart(c) == end) {
                return c->reg;
            }
            if (earliest == noReg) {
                earliest = c->reg;
            }
        }
        return earliest;
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
                const i32 successor = mFuncSuccAt(mf, succs, top->i++);
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
            if (bitsetPropagate(a->liveIn[b], out, kill[b])) {
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
        if (it->reg != noReg) {
            return it->reg;
        }
        return slotLoc(a->slots[allocatorVindex(a, it->vreg)]);
    }

    // location of vreg r at position p, where r is live: a register, or a
    // stack slot encoded as a negative value below noReg. A part of a split
    // interval has one location over its whole extent, and parts do not
    // overlap; the root caches the extent it last found.
    static VReg allocatorLocation(Allocator* a, VReg r, i32 p) {
        Interval* root = a->intervals[allocatorVindex(a, r)];
        if (p >= root->locationFrom && p < root->locationTo) {
            return root->locationReg;
        }
        return allocatorLocate(a, root, p);
    }

    static __attribute__((noinline)) VReg allocatorLocate(Allocator* a, Interval* root, i32 p) {
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
        root->locationFrom = intervalStart(part);
        root->locationTo = intervalEnd(part);
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
        MFunc* mf = a->mf;
        PositionedMoves inner;
        Bitset split;
        auto intervals = a->intervals.view();
        for (Int j = 0; j < length(intervals); ++j) {
            Int x = j;
            Interval* it = intervals[j];
            if (isNil(it)) {
                continue;
            }
            if (length(it->children)) {
                if (!split.data) {
                    split = newBitset(mf->nregs);
                }
                bitsetSet(split, x);
            }
            Int i = 0;
            for (; i < length(it->children); i++) {
                Interval* prev = i ? it->children[i - 1] : it;
                Interval* c = it->children[i];
                i32 p = intervalStart(c);
                if (intervalEnd(prev) != p) {
                    continue;
                }
                i32 b = a->posBlock[p / 2];
                if (a->blockFrom[b] == p) {
                    continue;
                }
                VReg src = allocatorLoc(a, prev);
                VReg dst = allocatorLoc(a, c);
                i32 key = 2 * p;
                if ((p & 1) != 0) {
                    if (!isSlot(dst)) {
                        compilerError(String("internal error: %s split at odd position %d into a register"));
                    }
                    key = 2 * (p - 1) + 1;
                }
                if (src != dst && !(isSlot(dst) && slotOf(dst) >= constantSlot)) {
                    appendMove(inner, key, Move{dst, src});
                }
                (void)x;
            }
        }
        Map<i32, Slice<Move>> atEnd = mapOf<i32, Slice<Move>>({});
        Map<i32, Slice<Move>> atStart = mapOf<i32, Slice<Move>>({});
        // An unsplit interval has one location throughout the CFG. Only
        // split registers can need a move between predecessor and successor.
        if (split.data) {
            auto order = a->order.view();
            for (Int i = 0; i < length(order); ++i) {
                i32 b = order[i];
                MBlock* blk = &mf->blocks[b];
                auto succs = blk->succs;
                for (Int i = 0; i < length(succs); ++i) {
                    i32 s = succs[i];
                    Slice<Move> moves = {};
                    bitsetEachIntersection(a->liveIn[s], split, [&](Int x) mutable -> void {
                        VReg r = convert<VReg>(x) + firstVirtual;
                        VReg src = allocatorLocation(a, r, a->blockTo[b] - 1);
                        VReg dst = allocatorLocation(a, r, a->blockFrom[s]);
                        if (src != dst && !(isSlot(dst) && slotOf(dst) >= constantSlot)) {
                            moves = appendValues(moves, Move{dst, src});
                        }
                    });
                    if (length(moves) == 0) {
                        continue;
                    }
                    if (length(blk->succs) == 1) {
                        setMap(atEnd, b, appendSlice(atEnd[b], moves));
                    } else if (length(mf->blocks[s].preds) == 1) {
                        setMap(atStart, s, appendSlice(atStart[s], moves));
                    } else {
                        compilerError(String("internal error: critical edge B%d->B%d needs moves"));
                    }
                }
            }
        }
        allocatorSaveSpillsAtDefinitions(a, inner, atEnd, atStart);
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
                // The moves of the edge read locations at the end of the
                // block: after the moves inside it at the branch, a split
                // there or the save of a value defined just before it.
                if (Int(i) == branches) {
                    emitMoves(endMoves, i);
                }
                MInst* in = &instructions[Int(i)];
                if (in->op == mFConst && !vRegIsPhys(in->dst) && isSlot(allocatorLocation(a, in->dst, p + 1))) {
                    *in = inst(mNop);
                }
                mInstForEachOperand(in, [&](VReg* r, Int role) {
                    if (*r == noReg || vRegIsPhys(*r)) {
                        return;
                    }
                    const i32 at = role == roleDef ? p + 1 : p;
                    *r = allocatorLocation(a, *r, at);
                    if (isSlot(*r) && in->op != mParMove && !mInstMemoryOperand(in, r)) {
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

    // A spill slot belongs to one virtual register. Saving the register in
    // its slot after each of its definitions keeps the slot up to date
    // wherever the value lives, so every save where a part of it moves from
    // a register to the slot can go. That is cheaper when the definitions
    // run less often than those saves: a value defined outside a loop and
    // spilled in it, or a loop parameter defined in a few of the loop's
    // blocks and spilled in many. A block weighs eight times the loop around
    // it; a cold one weighs one.
    static void allocatorSaveSpillsAtDefinitions(Allocator* a, PositionedMoves& inner, Map<i32, Slice<Move>> atEnd, Map<i32, Slice<Move>> atStart) {
        if (!a->nslots || (!inner.entries.size() && !length(atEnd) && !length(atStart))) {
            return;
        }
        auto weight = [&](i32 b) -> u64 {
            const MBlock& blk = a->mf->blocks[b];
            if (blk.cold) {
                return 1;
            }
            u64 w = 1;
            for (Int d = 0; d < blk.depth && d < 6; ++d) {
                w *= 8;
            }
            return w;
        };
        Map<VReg, Int> owners = makeMap<VReg, Int>();
        auto slots = a->slots.view();
        for (Int x = 0; x < length(slots); ++x) {
            if (slots[x] >= 0 && slots[x] < constantSlot) {
                setMap(owners, slotLoc(slots[x]), x);
            }
        }
        if (!length(owners)) {
            return;
        }
        Slice<u64> saves = makeSlice<u64>(a->mf->nregs);
        auto consider = [&](Slice<Move> moves, i32 b) mutable -> void {
            for (Int i = 0; i < length(moves); ++i) {
                auto owner = lookupMap(owners, moves[i].dst);
                if (owner.v1) {
                    saves[owner.v0] += weight(b);
                }
            }
        };
        for (usize i = 0; i < inner.entries.size(); ++i) {
            const auto& entry = inner.entries[i];
            auto owner = lookupMap(owners, entry.move.dst);
            if (owner.v1) {
                saves[owner.v0] += weight(a->posBlock[entry.position / 4]);
            }
        }
        for (Int k = 0; k < mapSlots(atEnd); ++k) {
            if (mapOccupied(atEnd, k)) {
                consider(mapValue(atEnd, k), mapKey(atEnd, k));
            }
        }
        for (Int k = 0; k < mapSlots(atStart); ++k) {
            if (mapOccupied(atStart, k)) {
                consider(mapValue(atStart, k), mapKey(atStart, k));
            }
        }
        // The saves after the definitions, and the definitions that end
        // their block, after which the resolver has no place for a save.
        Slice<u64> defined = makeSlice<u64>(a->mf->nregs);
        Slice<bool> last = makeSlice<bool>(a->mf->nregs);
        auto eachDefinition = [&](auto&& f) {
            auto order = a->order.view();
            for (Int j = 0; j < length(order); ++j) {
                const i32 b = order[j];
                auto instructions = a->mf->blocks[b].insts.view();
                for (Int i = 0; i < length(instructions); ++i) {
                    const i32 p = a->blockFrom[b] + 2 + 2 * i32(i);
                    mInstForEachOperand(&instructions[i], [&](VReg* r, Int role) {
                        if (*r == noReg || vRegIsPhys(*r) || (role & roleDef) == 0 || role == roleTemp) {
                            return;
                        }
                        const Int x = allocatorVindex(a, *r);
                        if (saves[x] != 0) {
                            f(x, *r, b, i, p);
                        }
                    });
                }
            }
        };
        eachDefinition([&](Int x, VReg r, i32 b, Int i, i32 p) {
            if (i + 1 == length(a->mf->blocks[b].insts)) {
                last[x] = true;
            } else if (!isSlot(allocatorLocation(a, r, p + 1))) {
                defined[x] += weight(b);
            }
        });
        Slice<bool> save = makeSlice<bool>(a->mf->nregs);
        bool any = false;
        for (Int x = 0; x < a->mf->nregs; ++x) {
            if (saves[x] != 0 && !last[x] && defined[x] < saves[x]) {
                save[x] = true;
                any = true;
            }
        }
        if (!any) {
            return;
        }
        auto filter = [&](Slice<Move> moves) mutable -> Slice<Move> {
            Slice<Move> out = subslice(moves, 0, 0);
            for (Int i = 0; i < length(moves); ++i) {
                auto owner = lookupMap(owners, moves[i].dst);
                if (!owner.v1 || !save[owner.v0]) {
                    out = appendValues(out, moves[i]);
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
        for (Int k = 0; k < mapSlots(atEnd); ++k) {
            if (mapOccupied(atEnd, k)) {
                setMap(atEnd, mapKey(atEnd, k), filter(mapValue(atEnd, k)));
            }
        }
        for (Int k = 0; k < mapSlots(atStart); ++k) {
            if (mapOccupied(atStart, k)) {
                setMap(atStart, mapKey(atStart, k), filter(mapValue(atStart, k)));
            }
        }
        eachDefinition([&](Int x, VReg r, i32, Int, i32 p) {
            if (!save[x]) {
                return;
            }
            VReg src = allocatorLocation(a, r, p + 1);
            VReg dst = slotLoc(a->slots[x]);
            if (src != dst) {
                appendMove(inner, 2 * (p + 2), Move{dst, src});
            }
        });
    }

    // partAt returns the part of root's value covering p, or null.
    static Interval* allocatorPartAt(Interval* root, i32 p) {
        if (intervalCovers(root, p)) {
            return root;
        }
        const Int k = sortSearch(length(root->children), [&](Int i) {
            return intervalStart(root->children[i]) > p;
        });
        if (k && intervalCovers(root->children[k - 1], p)) {
            return root->children[k - 1];
        }
        return nullptr;
    }

    // definitions counts the definitions of each virtual register and finds
    // the constants, those defined only by a load of a float constant; with
    // depths it fills invariantDepth as well.
    static void allocatorDefinitions(Allocator* a, bool depths) {
        MFunc* mf = a->mf;
        Vec<i32>& defs = a->definitions;
        defs.resize(usize(mf->nregs));
        defs.fill(0);
        a->constant.resize(usize(mf->nregs));
        a->constant.fill(false);
        a->constantBits.resize(usize(mf->nregs));
        a->constantBits.fill(0);
        if (depths) {
            a->invariantDepth.resize(usize(mf->nregs));
            a->invariantDepth.fill(-1);
        }
        auto order = a->order.view();
        for (Int j = 0; j < length(order); ++j) {
            const i32 b = order[j];
            const i32 depth = i32(mf->blocks[b].depth);
            auto instructions = mf->blocks[b].insts.view();
            for (Int i = 0; i < length(instructions); ++i) {
                MInst* in = &instructions[i];
                mInstForEachOperand(in, [&](VReg* r, Int role) {
                    if (*r == noReg || vRegIsPhys(*r) || (role & roleDef) == 0 || role == roleTemp) {
                        return;
                    }
                    const usize x = usize(allocatorVindex(a, *r));
                    defs[x]++;
                    if (in->op == mFConst) {
                        a->constant[x] = true;
                        a->constantBits[x] = in->size == 4 ? u64(u32(in->imm)) : u64(in->imm);
                    }
                    if (depths) {
                        i32& d = a->invariantDepth[x];
                        d = d == -1 ? depth : 1 << 30;
                    }
                });
            }
        }
        for (usize x = 0; x < defs.size(); ++x) {
            if (defs[x] != 1) {
                a->constant[x] = false;
            }
        }
    }

    // deadMoves drops the moves into registers that nothing reads before
    // they are written again: a reload at the start of a block that does
    // not use the value and leaves it in its slot, say. So go the leas and
    // extensions, which set no flags, that nothing reads: a sum each of
    // whose accesses is addressed from its base. The liveness of the
    // physical registers runs backward over the blocks to a fixed point;
    // every instruction names the registers it reads and writes, or its
    // clobbers do, as the allocation itself relies on.
    static void allocatorDeadMoves(Allocator* a) {
        MFunc* mf = a->mf;
        const usize n = mf->blocks.size();
        auto bit = [](VReg r) -> u32 {
            return vRegIsPhys(r) ? u32(1) << u32(r) : 0;
        };
        // masks are the registers an instruction reads and those it writes.
        auto masks = [&](MInst* in) -> Tuple2<u32, u32> {
            u32 defs = 0;
            u32 uses = 0;
            auto clobbers = mInstClobberSet(in, mf->registerClasses);
            for (Int i = 0; i < length(clobbers); ++i) {
                defs |= bit(clobbers[i]);
            }
            mInstForEachOperand(in, [&](VReg* r, Int role) {
                if (role == roleTemp || (role & roleDef) != 0) {
                    defs |= bit(*r);
                }
                if ((role & roleUse) != 0) {
                    uses |= bit(*r);
                }
            });
            return {uses, defs};
        };
        auto transfer = [&](MInst* in, u32 live) -> u32 {
            const auto m = masks(in);
            return (live & ~m.v1) | m.v0;
        };
        Vec<u32> liveIn;
        liveIn.resize(n);
        // gen are the registers a block reads before it writes them, kill
        // those it writes.
        Vec<u32> gen;
        gen.resize(n);
        Vec<u32> kill;
        kill.resize(n);
        auto liveOut = [&](i32 b) {
            u32 out = 0;
            const auto succs = mf->blocks[b].succs;
            for (Int k = 0; k < length(succs); ++k) {
                out |= liveIn[usize(succs[k])];
            }
            return out;
        };
        auto order = a->order.view();
        // A dropped move can leave the one that fed it dead in turn.
        for (Int round = 0, dropped = 1; round < 4 && dropped; ++round) {
        dropped = 0;
        for (Int j = 0; j < length(order); ++j) {
            const i32 b = order[j];
            u32 g = 0;
            u32 k = 0;
            auto instructions = mf->blocks[b].insts.view();
            for (Int i = length(instructions) - 1; i >= 0; --i) {
                // Through an instruction a register written and not read
                // is killed, one read is generated.
                const auto m = masks(&instructions[i]);
                g = (g & ~m.v1) | m.v0;
                k = (k | m.v1) & ~m.v0;
            }
            gen[usize(b)] = g;
            kill[usize(b)] = k;
            liveIn[usize(b)] = g;
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (Int j = length(order) - 1; j >= 0; --j) {
                const i32 b = order[j];
                const u32 live = gen[usize(b)] | (liveOut(b) & ~kill[usize(b)]);
                if (live != liveIn[usize(b)]) {
                    liveIn[usize(b)] = live;
                    changed = true;
                }
            }
        }
        for (Int j = 0; j < length(order); ++j) {
            const i32 b = order[j];
            u32 live = liveOut(b);
            auto instructions = mf->blocks[b].insts.view();
            for (Int i = length(instructions) - 1; i >= 0; --i) {
                MInst* in = &instructions[i];
                if (in->op == mParMove) {
                    auto& moves = mInstExtra(in)->moves;
                    usize kept = 0;
                    for (usize k = 0; k < moves.size(); ++k) {
                        if (!vRegIsPhys(moves[k].dst) || (live & bit(moves[k].dst)) != 0) {
                            moves[kept++] = moves[k];
                        }
                    }
                    dropped += Int(moves.size() - kept);
                    moves.resize(kept);
                } else if ((in->op == mMov || in->op == mFMov || in->op == mMovImm || in->op == mFConst || in->op == mLea || in->op == mExt) && vRegIsPhys(in->dst) && (live & bit(in->dst)) == 0) {
                    *in = inst(mNop);
                    dropped++;
                }
                live = transfer(in, live);
            }
        }
        }
    }

    // deadSpills drops the stores into spill slots that nothing reads before
    // they are written again: the spill at a definition of a value whose
    // slot the next definition fills too, say. The liveness of the slots
    // runs backward over the blocks to a fixed point, as deadMoves's of the
    // registers does. A function that calls an import returning twice keeps
    // every store: the second return reads the slots wherever a call left
    // them.
    static void allocatorDeadSpills(Allocator* a) {
        MFunc* mf = a->mf;
        if (mf->returnsTwice || a->nslots <= 0) {
            return;
        }
        const usize words = (usize(a->nslots) + 63) / 64;
        const usize n = mf->blocks.size();
        auto slot = [](VReg r) -> i32 {
            return isSlot(r) && slotOf(r) < constantSlot ? slotOf(r) : -1;
        };
        // The sets are rows of words, one per block: a Vec of many is paged,
        // so a row is reached by index, never by a pointer into it.
        Vec<u64> liveIn;
        liveIn.resize(n * words);
        liveIn.fill(0);
        Vec<u64> gen;
        gen.resize(n * words);
        gen.fill(0);
        Vec<u64> kill;
        kill.resize(n * words);
        kill.fill(0);
        Vec<u64> out;
        out.resize(words);
        auto bit = [](i32 k) {
            return u64(1) << (u64(k) % 64);
        };
        auto order = a->order.view();
        // Through an instruction the slots it writes die, those it reads live.
        auto transfer = [&](MInst* in) {
            mInstForEachOperand(in, [&](VReg* r, Int role) {
                const i32 k = slot(*r);
                if (k >= 0 && role == roleDef) {
                    out[usize(k) / 64] &= ~bit(k);
                }
            });
            mInstForEachOperand(in, [&](VReg* r, Int role) {
                const i32 k = slot(*r);
                if (k >= 0 && role != roleDef) {
                    out[usize(k) / 64] |= bit(k);
                }
            });
        };
        for (Int j = 0; j < length(order); ++j) {
            const usize b = usize(order[j]);
            const usize row = b * words;
            auto instructions = mf->blocks[b].insts.view();
            for (Int i = length(instructions) - 1; i >= 0; --i) {
                mInstForEachOperand(&instructions[i], [&](VReg* r, Int role) {
                    const i32 x = slot(*r);
                    if (x >= 0 && role == roleDef) {
                        gen[row + usize(x) / 64] &= ~bit(x);
                        kill[row + usize(x) / 64] |= bit(x);
                    }
                });
                mInstForEachOperand(&instructions[i], [&](VReg* r, Int role) {
                    const i32 x = slot(*r);
                    if (x >= 0 && role != roleDef) {
                        gen[row + usize(x) / 64] |= bit(x);
                        kill[row + usize(x) / 64] &= ~bit(x);
                    }
                });
            }
            for (usize w = 0; w < words; ++w) {
                liveIn[row + w] = gen[row + w];
            }
        }
        auto liveOut = [&](usize b) {
            out.fill(0);
            const auto succs = mf->blocks[b].succs;
            for (Int k = 0; k < length(succs); ++k) {
                const usize row = usize(succs[k]) * words;
                for (usize w = 0; w < words; ++w) {
                    out[w] |= liveIn[row + w];
                }
            }
        };
        for (bool changed = true; changed;) {
            changed = false;
            for (Int j = length(order) - 1; j >= 0; --j) {
                const usize b = usize(order[j]);
                const usize row = b * words;
                liveOut(b);
                for (usize w = 0; w < words; ++w) {
                    const u64 x = gen[row + w] | (out[w] & ~kill[row + w]);
                    if (x != liveIn[row + w]) {
                        liveIn[row + w] = x;
                        changed = true;
                    }
                }
            }
        }
        auto live = [&](i32 k) {
            return ((out[usize(k) / 64] >> (u64(k) % 64)) & 1) != 0;
        };
        for (Int j = 0; j < length(order); ++j) {
            const usize b = usize(order[j]);
            liveOut(b);
            auto instructions = mf->blocks[b].insts.view();
            for (Int i = length(instructions) - 1; i >= 0; --i) {
                MInst* in = &instructions[i];
                const i32 k = slot(in->dst);
                if (in->op == mParMove) {
                    auto& moves = mInstExtra(in)->moves;
                    usize kept = 0;
                    for (usize m = 0; m < moves.size(); ++m) {
                        const i32 x = slot(moves[m].dst);
                        if (x < 0 || live(x)) {
                            moves[kept++] = moves[m];
                        }
                    }
                    moves.resize(kept);
                } else if ((in->op == mMov || in->op == mFMov) && k >= 0 && !live(k)) {
                    *in = inst(mNop);
                }
                transfer(in);
            }
        }
    }

    // duplicateDispatch copies a dispatch, a block of a few instructions
    // ending in a switch that many blocks jump to, over the jump at the end
    // of each of them: each block takes its own indirect jump, through the
    // one table, instead of a jump to the shared one. The blocks' moves
    // have put every value where the dispatch reads it. The predecessor
    // lists stay as they were.
    static void allocatorDuplicateDispatch(Allocator* a) {
        MFunc* mf = a->mf;
        auto order = a->order.view();
        for (Int j = 0; j < length(order); ++j) {
            const i32 h = order[j];
            auto& insts = mf->blocks[h].insts;
            const usize n = insts.size();
            if (n == 0 || n > 3 || insts[n - 1].op != mSwitch) {
                continue;
            }
            bool plain = true;
            for (usize i = 0; i + 1 < n; ++i) {
                plain = plain && (insts[i].op == mAluImm || insts[i].op == mMov || insts[i].op == mLea || insts[i].op == mNop);
            }
            const auto preds = mf->blocks[h].preds;
            Int jumping = 0;
            for (Int k = 0; k < length(preds); ++k) {
                const auto& pi = mf->blocks[preds[k]].insts;
                jumping += pi.size() && pi[pi.size() - 1].op == mJmp && pi[pi.size() - 1].target == h;
            }
            if (!plain || jumping < 16) {
                continue;
            }
            for (Int k = 0; k < length(preds); ++k) {
                MBlock* p = &mf->blocks[preds[k]];
                if (!p->insts.size() || p->insts[p->insts.size() - 1].op != mJmp || p->insts[p->insts.size() - 1].target != h) {
                    continue;
                }
                p->insts.popBack();
                for (usize i = 0; i < n; ++i) {
                    p->insts.pushBack(mf->blocks[h].insts[i]);
                }
                p->succs = mf->blocks[h].succs;
            }
        }
    }

    // copies makes a move of a register that copies another, earlier in the
    // block and unchanged since, read that other one: a chain of moves at a
    // block's end reads the values where they are, and the moves left with
    // no reader go with the dead ones.
    static void allocatorCopies(Allocator* a) {
        MFunc* mf = a->mf;
        auto order = a->order.view();
        for (Int j = 0; j < length(order); ++j) {
            // copy[r] is the register r holds a copy of, or noReg.
            Array<VReg, 32> copy{};
            for (Int r = 0; r < 32; ++r) {
                copy[r] = noReg;
            }
            auto forget = [&](VReg d) {
                if (!vRegIsPhys(d)) {
                    return;
                }
                copy[d] = noReg;
                for (Int r = 0; r < 32; ++r) {
                    if (copy[r] == d) {
                        copy[r] = noReg;
                    }
                }
            };
            auto insts = mf->blocks[order[j]].insts.view();
            for (Int i = 0; i < length(insts); ++i) {
                MInst* in = &insts[i];
                if (in->op == mParMove) {
                    auto& moves = mInstExtra(in)->moves;
                    for (usize k = 0; k < moves.size(); ++k) {
                        const VReg src = moves[k].src;
                        if (vRegIsPhys(src) && copy[src] != noReg && vRegIsXMM(copy[src]) == vRegIsXMM(moves[k].dst)) {
                            moves[k].src = copy[src];
                        }
                    }
                    for (usize k = 0; k < moves.size(); ++k) {
                        forget(moves[k].dst);
                    }
                    for (usize k = 0; k < moves.size(); ++k) {
                        const VReg src = moves[k].src;
                        const VReg dst = moves[k].dst;
                        bool written = false;
                        for (usize m = 0; m < moves.size(); ++m) {
                            written = written || moves[m].dst == src;
                        }
                        if (vRegIsPhys(src) && vRegIsPhys(dst) && src != dst && !written) {
                            copy[dst] = src;
                        }
                    }
                    continue;
                }
                mInstForEachOperand(in, [&](VReg* r, Int role) {
                    if (role == roleTemp || (role & roleDef) != 0) {
                        forget(*r);
                    }
                });
                auto clobbers = mInstClobberSet(in, mf->registerClasses);
                for (Int c = 0; c < length(clobbers); ++c) {
                    forget(clobbers[c]);
                }
            }
        }
    }

    // narrowMasks loads only the low bytes of an unsigned load that a mask
    // of them is the only reader of in its register: a byte or a half
    // instead of a word and an and. The load's check, of its own width,
    // stays where it was. A mask of a load no wider than it goes alone.
    static void allocatorNarrowMasks(Allocator* a) {
        MFunc* mf = a->mf;
        auto order = a->order.view();
        for (Int j = 0; j < length(order); ++j) {
            auto insts = mf->blocks[order[j]].insts.view();
            for (Int i = 0; i < length(insts); ++i) {
                MInst* in = &insts[i];
                if (in->op != mAluImm || in->aux != u8(aluAnd) || in->size != 4 || !vRegIsPhys(in->dst) || (in->imm != 255 && in->imm != 65535)) {
                    continue;
                }
                const u8 width = in->imm == 255 ? 1 : 2;
                Int next = i + 1;
                while (next < length(insts) && insts[next].op == mNop) {
                    ++next;
                }
                if (next < length(insts) && (insts[next].op == mJcc || insts[next].op == mSetcc || insts[next].op == mCmov || insts[next].op == mFSelect)) {
                    continue;
                }
                const VReg r = in->dst;
                // Back through the block, and a block it alone enters from.
                i32 b = order[j];
                Span<MInst> scan = insts;
                Int k = i - 1;
                for (Int hops = 0; hops < 4; ++hops) {
                    bool done = false;
                    for (; k >= 0 && !done; --k) {
                        MInst* prior = &scan[k];
                        if ((prior->op == mMov || prior->op == mFMov) && prior->dst == prior->src && prior->aux == 0) {
                            continue;
                        }
                        if (prior->op == mLoad && prior->dst == r) {
                            if (prior->aux == 0 && prior->dsize == 4) {
                                if (prior->size > width) {
                                    prior->size = width;
                                }
                                *in = inst(mNop);
                            }
                            done = true;
                            break;
                        }
                        bool touches = false;
                        mInstForEachOperand(prior, [&](VReg* o, Int) {
                            touches = touches || *o == r;
                        });
                        auto clobbers = mInstClobberSet(prior, mf->registerClasses);
                        for (Int c = 0; c < length(clobbers); ++c) {
                            touches = touches || clobbers[c] == r;
                        }
                        done = touches;
                    }
                    const auto preds = mf->blocks[b].preds;
                    if (done || length(preds) != 1 || length(mf->blocks[preds[0]].succs) != 1) {
                        break;
                    }
                    b = preds[0];
                    scan = mf->blocks[b].insts.view();
                    k = length(scan) - 1;
                }
            }
        }
    }

    // orderSlots numbers the spill slots by how often the code reaches them,
    // a block weighing eight times the loop around it and a cold one one:
    // the slots first in the frame take an 8-bit displacement.
    static void allocatorOrderSlots(Allocator* a) {
        if (a->nslots < 2) {
            return;
        }
        MFunc* mf = a->mf;
        auto each = [&](auto&& f) {
            for (Int j = 0; j < length(a->order); ++j) {
                MBlock* blk = &mf->blocks[a->order[j]];
                u64 w = 1;
                for (Int d = 0; !blk->cold && d < blk->depth && d < 6; ++d) {
                    w *= 8;
                }
                auto instructions = blk->insts.view();
                for (Int i = 0; i < length(instructions); ++i) {
                    mInstForEachOperand(&instructions[i], [&](VReg* r, Int) {
                        if (isSlot(*r) && slotOf(*r) < constantSlot) {
                            f(r, w);
                        }
                    });
                }
            }
        };
        Slice<u64> weight = makeSlice<u64>(a->nslots);
        each([&](VReg* r, u64 w) {
            weight[slotOf(*r)] += w;
        });
        Slice<i32> byWeight = makeSlice<i32>(a->nslots);
        for (i32 k = 0; k < a->nslots; ++k) {
            byWeight[k] = k;
        }
        sortValuesStable(byWeight, [&](Int x, Int y) {
            return weight[byWeight[x]] > weight[byWeight[y]];
        });
        Slice<i32> renumber = makeSlice<i32>(a->nslots);
        bool changed = false;
        for (i32 k = 0; k < a->nslots; ++k) {
            renumber[byWeight[k]] = k;
            changed = changed || byWeight[k] != k;
        }
        if (!changed) {
            return;
        }
        each([&](VReg* r, u64) {
            *r = slotLoc(renumber[slotOf(*r)]);
        });
        for (usize x = 0; x < a->slots.size(); ++x) {
            if (a->slots[x] >= 0 && a->slots[x] < constantSlot) {
                a->slots[x] = renumber[a->slots[x]];
            }
        }
    }

    // constantDefinitions moves to its constant slot a constant whose first
    // part is used by nothing after its definition: the definition goes, and
    // whatever reads the part reads the constant.
    static void allocatorConstantDefinitions(Allocator* a) {
        for (usize x = 0; x < a->constant.size(); ++x) {
            Interval* root = a->intervals[x];
            if (a->constant[x] && root && root->reg != noReg && length(root->uses) == 1 && length(root->memUses) == 0) {
                allocatorSpill(a, root);
            }
        }
    }

    // splitAtJoins starts afresh, at a loop header that many of the loop's
    // blocks return to, every value live there: the scan chooses their
    // registers at the header, those with the most uses after it first,
    // instead of keeping what each got where it began. A dispatch loop's
    // hottest values then take the registers its handlers' calls keep, for
    // the whole loop.
    static void allocatorSplitAtJoins(Allocator* a) {
        MFunc* mf = a->mf;
        struct Part {
            Interval* interval;
            Int uses;
        };
        auto order = a->order.view();
        for (Int j = 0; j < length(order); ++j) {
            const i32 b = order[j];
            const i32 from = a->blockFrom[b];
            const auto preds = mf->blocks[b].preds;
            Int back = 0;
            for (Int i = 0; i < length(preds); ++i) {
                if (a->blockFrom[preds[i]] >= from) {
                    back++;
                }
            }
            if (back < 16) {
                continue;
            }
            Slice<Part> parts = {};
            bitsetEach(a->liveIn[b], [&](Int x) {
                Interval* root = a->intervals[x];
                if (isNil(root) || !length(root->ranges)) {
                    return;
                }
                Interval* part = root;
                if (length(root->children)) {
                    part = root->children[length(root->children) - 1];
                }
                if (intervalStart(part) >= from || !intervalCovers(part, from)) {
                    return;
                }
                Interval* child = intervalSplit(part, from);
                parts = appendValues(parts, Part{child, length(child->uses) + length(child->memUses)});
                if (!a->home.size()) {
                    a->home.resize(usize(mf->nregs));
                    a->home.fill(nullptr);
                }
                a->home[usize(x)] = child;
            });
            sortValuesStable(parts, [&](Int x, Int y) {
                return parts[x].uses > parts[y].uses;
            });
            for (Int i = 0; i < length(parts); ++i) {
                a->unhandled.push(parts[i].interval, from);
            }
        }
    }

    // spillAtJoins keeps a value in its slot from the start of a block that
    // many blocks enter, when most of them would restore its register
    // there: a dispatch loop's handlers spill such a value around their
    // calls and reload it at their ends, though few of them use it. The
    // value's part at the block's start goes to the slot from there when it
    // has no use left; an entering block that has the value in a register
    // then saves it, unless the value is saved at its definitions.
    static void allocatorSpillAtJoins(Allocator* a) {
        MFunc* mf = a->mf;
        auto order = a->order.view();
        // A loop's header keeps in its slot a value every block returning
        // to it has there: the value is reloaded at its first use in the
        // loop instead of on each back edge, which then needs no move and
        // takes the loop's conditional branch itself.
        for (Int j = 0; j < length(order); ++j) {
            const i32 b = order[j];
            const auto preds = mf->blocks[b].preds;
            const i32 from = a->blockFrom[b];
            bool loop = false;
            for (Int i = 0; i < length(preds); ++i) {
                loop = loop || a->blockFrom[preds[i]] >= from;
            }
            if (!loop) {
                continue;
            }
            bitsetEach(a->liveIn[b], [&](Int x) {
                Interval* root = a->intervals[x];
                if (isNil(root) || a->slots[x] < 0) {
                    return;
                }
                Interval* part = allocatorPartAt(root, from);
                if (isNil(part) || part->reg == noReg) {
                    return;
                }
                const VReg r = firstVirtual + VReg(x);
                for (Int i = 0; i < length(preds); ++i) {
                    const i32 p = preds[i];
                    if (a->blockFrom[p] >= from && !isSlot(allocatorLocation(a, r, a->blockTo[p] - 1))) {
                        return;
                    }
                }
                const i32 use = intervalNextUse(part, from);
                const i32 at = use < maxPos ? allocatorSplitPos(a, from + 1, use) : -1;
                if (use < maxPos && (at <= from || at >= intervalEnd(part))) {
                    return;
                }
                const VReg reg = part->reg;
                Interval* head = intervalStart(part) < from ? intervalSplit(part, from) : part;
                if (at > intervalStart(head)) {
                    intervalSplit(head, at)->reg = reg;
                }
                allocatorSpill(a, head);
                root->locationFrom = 0;
                root->locationTo = 0;
            });
        }
        for (Int j = 0; j < length(order); ++j) {
            const i32 b = order[j];
            const auto preds = mf->blocks[b].preds;
            if (length(preds) < 8) {
                continue;
            }
            const i32 from = a->blockFrom[b];
            bitsetEach(a->liveIn[b], [&](Int x) {
                Interval* root = a->intervals[x];
                if (isNil(root)) {
                    return;
                }
                Interval* part = allocatorPartAt(root, from);
                if (isNil(part) || part->reg == noReg || intervalNextUse(part, from) < intervalEnd(part) || (length(part->memUses) && part->memUses[length(part->memUses) - 1] >= from)) {
                    return;
                }
                const VReg r = firstVirtual + VReg(x);
                Int restores = 0;
                Int saves = 0;
                for (Int i = 0; i < length(preds); ++i) {
                    const VReg at = allocatorLocation(a, r, a->blockTo[preds[i]] - 1);
                    if (at != part->reg) {
                        restores++;
                    }
                    if (!isSlot(at)) {
                        saves++;
                    }
                }
                if (restores <= saves + 1) {
                    return;
                }
                allocatorSpill(a, intervalStart(part) < from ? intervalSplit(part, from) : part);
                root->locationFrom = 0;
                root->locationTo = 0;
            });
        }
    }

    static void allocatorScan(Allocator* a) {
        auto range547 = a->intervals.view();
        for (Int index548 = 0, rangeEnd549 = length(range547); index548 < rangeEnd549; ++index548) {
            Interval* it = range547[index548];
            if (!isNil(it) && length(it->ranges) > 0) {
                a->unhandled.initial.pushBack({it, intervalStart(it), 0});
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
        allocatorSplitAtJoins(a);
        for (Int r = 0, end = length(a->fixed); r < end; ++r) {
            if (!isNil(a->fixed[r]) && length(a->fixed[r]->ranges) > 0) {
                a->inactive.pushBack(a->fixed[r]);
            }
        }
        a->slots.resize(usize(a->mf->nregs));
        a->slots.fill(-1);
        // Only range boundaries change active/inactive membership. Eviction
        // removes active intervals; shortening an inactive prefix can only
        // leave an inert, ended entry until the next advance.
        i32 nextChange = -1;
        while (!a->unhandled.empty()) {
            Interval* cur = a->unhandled.pop();
            i32 pos = intervalStart(cur);
            if (pos >= nextChange) {
                nextChange = allocatorAdvance(a, pos);
            }
            if (!allocatorTryFree(a, cur)) {
                allocatorAllocateBlocked(a, cur);
            }
            if (cur->reg != noReg) {
                a->active.pushBack(cur);
                nextChange = minimum(nextChange, cur->ranges[0].to);
            }
        }
    }

    static void allocatorSpill(Allocator* a, Interval* it) {
        it->reg = noReg;
        Int x = allocatorVindex(a, it->vreg);
        if (a->slots[x] < 0 && a->constant[usize(x)]) {
            a->slots[x] = constantSlot + i32(a->constantSlots.size());
            a->constantSlots.pushBack(a->constantBits[usize(x)]);
        } else if (a->slots[x] < 0) {
            a->slots[x] = a->nslots;
            a->nslots++;
        }
    }

    // splitPos chooses an instruction boundary between min and max, both
    // included: the start of the block with the lowest loop depth in that
    // range, else the latest boundary. It returns -1 when there is none.
    static i32 allocatorSplitPos(Allocator* a, i32 min, i32 max) {
        max &= ~1;
        if ((min & 1) != 0) {
            min++;
        }
        if (min > max) {
            return -1;
        }
        i32 best = max;
        Int bestDepth = a->mf->blocks[a->posBlock[max / 2]].depth;
        i32 p = max;
        while (p > min) {
            i32 b = a->posBlock[(p - 1) / 2];
            i32 from = a->blockFrom[b];
            if (from < min) {
                break;
            }
            Int d = a->mf->blocks[b].depth;
            if (d < bestDepth) {
                best = from;
                bestDepth = d;
            }
            p = from;
        }
        return best;
    }

    static bool allocatorTryFree(Allocator* a, Interval* cur) {
        Array<i32, 32> freeUntil = {};
        auto candidates = allocatorCandidates(a, cur->classValue);
        for (Int i = 0; i < length(candidates); ++i) {
            VReg r = candidates[i];
            freeUntil[r] = maxPos;
        }
        auto active = a->active.view();
        for (Int i = 0; i < length(active); ++i) {
            Interval* it = active[i];
            freeUntil[it->reg] = 0;
        }
        auto inactive = a->inactive.view();
        for (Int i = 0; i < length(inactive); ++i) {
            Interval* it = inactive[i];
            if (freeUntil[it->reg] == 0) {
                continue;
            }
            i32 x = intervalIntersection(it, cur);
            if (x < freeUntil[it->reg]) {
                freeUntil[it->reg] = x;
            }
        }
        i32 start = intervalStart(cur);
        i32 end = intervalEnd(cur);
        auto range567 = allocatorCandidates(a, cur->classValue);
        for (Int index568 = 0, rangeEnd569 = length(range567); index568 < rangeEnd569; ++index568) {
            VReg r = range567[index568];
            if (freeUntil[r] < end) {
                freeUntil[r] &= ~1;
                if (freeUntil[r] <= start) {
                    freeUntil[r] = 0;
                }
            }
        }
        VReg reg = noReg;
        VReg h = allocatorHintReg(a, cur);
        if (h != noReg && allocatable(h) && allocatorRegClass(a, h) == cur->classValue && freeUntil[h] >= end) {
            reg = h;
        }
        if (reg == noReg && a->home.size()) {
            Interval* h = a->home[usize(allocatorVindex(a, cur->vreg))];
            if (h && h != cur && h->reg != noReg && intervalStart(h) < start && freeUntil[h->reg] > start + 1) {
                reg = h->reg;
            }
        }
        if (reg == noReg) {
            VReg s = allocatorSiblingReg(a, cur);
            if (s != noReg && allocatable(s) && allocatorRegClass(a, s) == cur->classValue && freeUntil[s] >= end) {
                reg = s;
            }
        }
        if (reg == noReg) {
            VReg best = noReg;
            auto candidates = allocatorCandidates(a, cur->classValue);
            for (Int i = 0; i < length(candidates); ++i) {
                VReg r = candidates[i];
                if (freeUntil[r] >= end) {
                    reg = r;
                    break;
                }
                if (best == noReg || freeUntil[r] > freeUntil[best]) {
                    best = r;
                }
            }
            if (reg == noReg) {
                reg = best;
            }
        }
        if (freeUntil[reg] == 0) {
            return false;
        }
        cur->reg = reg;
        a->used[reg] = true;
        if (freeUntil[reg] < end) {
            i32 at = allocatorSplitPos(a, start + 1, freeUntil[reg]);
            allocatorAddUnhandled(a, intervalSplit(cur, at));
        }
        return true;
    }

    static Int allocatorVindex(Allocator* a, VReg r) {
        return convert<i64>(r - firstVirtual);
    }

    static void asmAluMI(Asm* a, AluOp op, u8 size, Amem m, i64 x) {
        if (size == 1) {
            asmOpRM(a, false, Span<const u8>({u8(128LL)}), convert<i64>(op), m, 1, false);
            asmBytes(a, {u8(convert<u8>(x))});
            return;
        }
        if (size == 2) {
            asmBytes(a, {u8(u8(102LL))});
        }
        if (x == convert<i64>(convert<i8>(x))) {
            asmOpRM(a, size == 8, Span<const u8>({u8(131LL)}), convert<i64>(op), m, 1, false);
            asmBytes(a, {u8(convert<u8>(x))});
            return;
        }
        if (size == 2) {
            asmOpRM(a, false, Span<const u8>({u8(129LL)}), convert<i64>(op), m, 2, false);
            asmBytes(a, {u8(convert<u8>(x)), u8(convert<u8>(shiftRight(x, 8)))});
            return;
        }
        asmOpRM(a, size == 8, Span<const u8>({u8(129LL)}), convert<i64>(op), m, 4, false);
        asmU32(a, convert<u32>(x));
    }

    static void asmAluRI(Asm* a, AluOp op, u8 size, VReg dst, i64 x) {
        asmAluRIRaw(a, op, size, dst, x);
        if (dst == RSP) {
            switch (op) {
                case aluSub: {
                    asmMoveSP(a, convert<i32>(x));
                    break;
                }
                case aluAdd: {
                    asmMoveSP(a, -convert<i32>(x));
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

    static void asmAluRIRaw(Asm* a, AluOp op, u8 size, VReg dst, i64 x) {
        Int d = convert<i64>(dst);
        if (x == convert<i64>(convert<i8>(x))) {
            asmRex(a, size == 8, -1, -1, d, false);
            asmBytes(a, {u8(u8(131LL))});
            asmModrmReg(a, convert<i64>(op), d);
            asmBytes(a, {u8(convert<u8>(x))});
            return;
        }
        asmRex(a, size == 8, -1, -1, d, false);
        asmBytes(a, {u8(u8(129LL))});
        asmModrmReg(a, convert<i64>(op), d);
        asmU32(a, convert<u32>(x));
    }

    // aluMR emits "op m, src": the memory operand is updated.
    static void asmAluMR(Asm* a, AluOp op, u8 size, Amem m, VReg src) {
        asmOpRM(a, size == 8, Span<const u8>({u8(shiftLeft(convert<u8>(op), 3) | 1)}), convert<i64>(src), m, 0, false);
    }

    static void asmAluRM(Asm* a, AluOp op, u8 size, VReg dst, Amem m) {
        if (op == aluImul) {
            asmOpRM(a, size == 8, Span<const u8>({u8(15LL), u8(175LL)}), convert<i64>(dst), m, 0, false);
            return;
        }
        asmOpRM(a, size == 8, Span<const u8>({u8(shiftLeft(convert<u8>(op), 3) | 3)}), convert<i64>(dst), m, 0, false);
    }

    // aluRR emits "op dst, src" for add, or, adc, sbb, and, sub, xor, cmp.
    static void asmAluRR(Asm* a, AluOp op, u8 size, VReg dst, VReg src) {
        if (op == aluImul) {
            asmOpRR(a, size == 8, Span<const u8>({u8(15LL), u8(175LL)}), convert<i64>(dst), convert<i64>(src), false);
            return;
        }
        asmOpRR(a, size == 8, Span<const u8>({u8(shiftLeft(convert<u8>(op), 3) | 3)}), convert<i64>(dst), convert<i64>(src), false);
    }

    static void asmB(Asm* a, Span<const u8> bytes) {
        appendSpan(a->buf, Span<const u8>(bytes));
    }

    static void asmBitCount(Asm* a, u8 kind, u8 size, VReg dst, VReg src) {
        static constexpr u8 opcodes[] = {189, 188, 184};
        require(kind < 3, "bit count opcode");
        asmPrefixed(a, 243, size == 8, Span<const u8>({15, opcodes[kind]}), dst, src);
    }

    // callMem calls the address stored at m.
    static void asmCallMem(Asm* a, Amem m) {
        asmOpRM(a, false, Span<const u8>({u8(255LL)}), 2, m, 0, false);
    }

    static void asmCallReg(Asm* a, VReg r) {
        asmRex(a, false, -1, -1, convert<i64>(r), false);
        asmBytes(a, {u8(u8(255LL))});
        asmModrmReg(a, 2, convert<i64>(r));
    }

    static i32 asmCallRel(Asm* a) {
        asmBytes(a, {u8(u8(232LL))});
        asmU32(a, 0);
        return asmPos(a) - 4;
    }

    static void asmCmov(Asm* a, u8 cc, u8 size, VReg dst, VReg src) {
        asmOpRR(a, size == 8, Span<const u8>({u8(15LL), u8(64 + cc)}), convert<i64>(dst), convert<i64>(src), false);
    }

    static void asmCvtFF(Asm* a, u8 from, VReg dst, VReg src) {
        Int d = regNum(dst);
        asmVexRR(a, scalarPP(from), 1, false, 90, d, 15, regNum(src));
    }

    // cvtFI truncates a float of fsize bytes to a signed integer of isize bytes.
    static void asmCvtFI(Asm* a, u8 isize, u8 fsize, VReg dst, VReg src) {
        asmVexRR(a, scalarPP(fsize), 1, isize == 8, 44, convert<i64>(dst), -1, regNum(src));
    }

    // cvtIF converts a signed integer of isize bytes to a float of fsize bytes.
    static void asmCvtIF(Asm* a, u8 isize, u8 fsize, VReg dst, VReg src) {
        Int d = regNum(dst);
        asmVexRR(a, scalarPP(fsize), 1, isize == 8, 42, d, 15, convert<i64>(src));
    }

    static void asmDiv(Asm* a, bool signedValue, u8 size, VReg src) {
        Int s = convert<i64>(src);
        asmRex(a, size == 8, -1, -1, s, false);
        asmBytes(a, {u8(u8(247LL))});
        if (signedValue) {
            asmModrmReg(a, 7, s);
        } else {
            asmModrmReg(a, 6, s);
        }
    }

    static void asmExt(Asm* a, u8 width, u8 dsize, bool signedValue, VReg dst, VReg src) {
        Int d = convert<i64>(dst);
        Int s = convert<i64>(src);
        if (width == 4 && signedValue) {
            asmOpRR(a, true, Span<const u8>({u8(99LL)}), d, s, false);
        } else if (width == 4) {
            asmOpRR(a, false, Span<const u8>({u8(139LL)}), d, s, false);
        } else if (width == 2) {
            u8 op = 183;
            if (signedValue) {
                op = 191;
            }
            asmOpRR(a, signedValue && dsize == 8, Span<const u8>({u8(15LL), op}), d, s, false);
        } else {
            u8 op = 182;
            if (signedValue) {
                op = 190;
            }
            asmOpRR(a, signedValue && dsize == 8, Span<const u8>({u8(15LL), op}), d, s, true);
        }
    }

    static void asmFalu(Asm* a, FAluOp op, u8 size, VReg dst, VReg x, VReg y) {
        asmVexRR(a, fAluPP(op, size), 1, false, floatOpcode(op), regNum(dst), regNum(x), regNum(y));
    }

    static void asmFaluM(Asm* a, FAluOp op, u8 size, VReg dst, VReg x, Amem m) {
        asmVexRM(a, fAluPP(op, size), 1, false, floatOpcode(op), regNum(dst), regNum(x), m, 0);
    }

    static void asmFblend(Asm* a, u8 size, VReg dst, VReg x, VReg y, VReg mask) {
        u8 op = 74;
        if (size == 8) {
            op = 75;
        }
        asmVexRR(a, pp66, 3, false, op, regNum(dst), regNum(x), regNum(y));
        asmBytes(a, {u8(convert<u8>(shiftLeft(regNum(mask), 4)))});
    }

    static void asmFcmp(Asm* a, u8 size, VReg x, VReg y) {
        asmVexRR(a, packedPP(size), 1, false, 46, regNum(x), -1, regNum(y));
    }

    static void asmFcmpM(Asm* a, u8 size, VReg x, Amem m) {
        asmVexRM(a, packedPP(size), 1, false, 46, regNum(x), -1, m, 0);
    }

    static void asmFcmpMask(Asm* a, u8 size, VReg dst, VReg x, VReg y, u8 predicate) {
        asmVexRR(a, scalarPP(size), 1, false, 194, regNum(dst), regNum(x), regNum(y));
        asmBytes(a, {u8(predicate)});
    }

    static void asmFload(Asm* a, u8 size, VReg dst, Amem m) {
        asmVexRM(a, scalarPP(size), 1, false, 16, regNum(dst), -1, m, 0);
    }

    static void asmFmov(Asm* a, VReg dst, VReg src) {
        asmVexRR(a, ppNone, 1, false, 40, regNum(dst), -1, regNum(src));
    }

    static void asmFround(Asm* a, u8 size, VReg dst, VReg src, i64 mode) {
        u8 op = 10;
        if (size == 8) {
            op = 11;
        }
        asmVexRR(a, pp66, 3, false, op, regNum(dst), regNum(src), regNum(src));
        asmBytes(a, {u8(u8(convert<u8>(mode) | 8))});
    }

    static void asmFsqrt(Asm* a, u8 size, VReg dst, VReg src) {
        asmVexRR(a, scalarPP(size), 1, false, 81, regNum(dst), regNum(src), regNum(src));
    }

    static void asmFstore(Asm* a, u8 size, VReg src, Amem m) {
        asmVexRM(a, scalarPP(size), 1, false, 17, regNum(src), -1, m, 0);
    }

    static void asmFxor(Asm* a, VReg dst) {
        Int r = regNum(dst);
        asmVexRR(a, ppNone, 1, false, 87, r, r, r);
    }

    static void asmHere8(Asm* a, i32 at) {
        i32 d = asmPos(a) - (at + 1);
        if (d > 127) {
            compilerError(String("internal error: short jump too far"));
        }
        a->buf[at] = convert<u8>(d);
    }

    static void asmImul3(Asm* a, u8 size, VReg dst, VReg src, i64 x) {
        if (x == convert<i64>(convert<i8>(x))) {
            asmOpRR(a, size == 8, Span<const u8>({u8(107LL)}), convert<i64>(dst), convert<i64>(src), false);
            asmBytes(a, {u8(convert<u8>(x))});
            return;
        }
        asmOpRR(a, size == 8, Span<const u8>({u8(105LL)}), convert<i64>(dst), convert<i64>(src), false);
        asmU32(a, convert<u32>(x));
    }

    static i32 asmJcc8(Asm* a, u8 cc) {
        asmBytes(a, {u8(u8(112 + cc)), u8(u8(0LL))});
        return asmPos(a) - 1;
    }

    static i32 asmJccRel(Asm* a, u8 cc) {
        asmBytes(a, {u8(u8(15LL)), u8(u8(128 + cc))});
        asmU32(a, 0);
        return asmPos(a) - 4;
    }

    static i32 asmJmp8(Asm* a) {
        asmBytes(a, {u8(u8(235LL)), u8(u8(0LL))});
        return asmPos(a) - 1;
    }

    static i32 asmJmpRel(Asm* a) {
        asmBytes(a, {u8(u8(233LL))});
        asmU32(a, 0);
        return asmPos(a) - 4;
    }

    static void asmLea(Asm* a, u8 size, VReg dst, Amem m) {
        asmOpRM(a, size == 8, Span<const u8>({u8(141LL)}), convert<i64>(dst), m, 0, false);
        if (dst == RSP) {
            if (m.base != 4 || m.index >= 0 || m.label >= 0) {
                compilerError(String("internal error: untracked change of rsp"));
            }
            asmMoveSP(a, -m.disp);
        }
    }

    // load emits a load of width bytes into a register of dsize bytes.
    static void asmLoad(Asm* a, u8 width, u8 dsize, bool signedValue, VReg dst, Amem m) {
        Int d = convert<i64>(dst);
        if (width == 8) {
            asmOpRM(a, true, Span<const u8>({u8(139LL)}), d, m, 0, false);
        } else if (width == 4 && signedValue && dsize == 8) {
            asmOpRM(a, true, Span<const u8>({u8(99LL)}), d, m, 0, false);
        } else if (width == 4) {
            asmOpRM(a, false, Span<const u8>({u8(139LL)}), d, m, 0, false);
        } else if (width == 2) {
            u8 op = 183;
            if (signedValue) {
                op = 191;
            }
            asmOpRM(a, signedValue && dsize == 8, Span<const u8>({u8(15LL), op}), d, m, 0, false);
        } else {
            u8 op = 182;
            if (signedValue) {
                op = 190;
            }
            asmOpRM(a, signedValue && dsize == 8, Span<const u8>({u8(15LL), op}), d, m, 0, false);
        }
    }

    // modrmMem emits ModRM, SIB and displacement for a memory operand; imm is
    // the number of immediate bytes that follow, for RIP-relative fixups.
    static void asmModrmMem(Asm* a, Int reg, Amem m, Int imm) {
        u8 r = shiftLeft(lo3(reg), 3);
        if (m.label >= 0) {
            asmBytes(a, {u8(u8(r | 5))});
            a->fixups = appendValues(a->fixups, RipFixup{asmPos(a), asmPos(a) + 4 + convert<i32>(imm), m.label, m.labelKind});
            asmU32(a, 0);
            return;
        }
        if (m.base < 0) {
            asmBytes(a, {u8(u8(r | 4))});
            asmBytes(a, {u8(u8(u8(shiftLeft(scaleBits(m.scale), 6) | shiftLeft(lo3(m.index), 3)) | 5))});
            asmU32(a, convert<u32>(m.disp));
            return;
        }
        u8 mod = {};
        if (m.disp == 0 && lo3(m.base) != 5) {
            mod = 0;
        } else if (m.disp == convert<i32>(convert<i8>(m.disp))) {
            mod = 1;
        } else {
            mod = 2;
        }
        if (m.index >= 0 || lo3(m.base) == 4) {
            u8 index = 4;
            if (m.index >= 0) {
                index = lo3(m.index);
            }
            asmBytes(a, {u8(u8(u8(shiftLeft(mod, 6) | r) | 4))});
            asmBytes(a, {u8(u8(u8(shiftLeft(scaleBits(m.scale), 6) | shiftLeft(index, 3)) | lo3(m.base)))});
        } else {
            asmBytes(a, {u8(u8(u8(shiftLeft(mod, 6) | r) | lo3(m.base)))});
        }
        switch (mod) {
            case 1: {
                asmBytes(a, {u8(convert<u8>(m.disp))});
                break;
            }
            case 2: {
                asmU32(a, convert<u32>(m.disp));
                break;
            }
        }
    }

    static void asmModrmReg(Asm* a, Int reg, Int rm) {
        asmBytes(a, {u8(u8(u8(192 | shiftLeft(lo3(reg), 3)) | lo3(rm)))});
    }

    static void asmMovGX(Asm* a, u8 size, VReg dst, VReg src) {
        asmVexRR(a, pp66, 1, size == 8, 110, regNum(dst), -1, convert<i64>(src));
    }

    static void asmMovImm(Asm* a, u8 size, VReg dst, i64 x) {
        Int d = convert<i64>(dst);
        if (x == 0) {
            asmOpRR(a, false, Span<const u8>({u8(51LL)}), d, d, false);
        } else if (size == 4 || (x >= 0 && x <= i64(4294967295LL))) {
            asmRex(a, false, -1, -1, d, false);
            asmBytes(a, {u8(u8(184 + lo3(d)))});
            asmU32(a, convert<u32>(x));
        } else if (x == convert<i64>(convert<i32>(x))) {
            asmRex(a, true, -1, -1, d, false);
            asmBytes(a, {u8(u8(199LL))});
            asmModrmReg(a, 0, d);
            asmU32(a, convert<u32>(x));
        } else {
            asmRex(a, true, -1, -1, d, false);
            asmBytes(a, {u8(u8(184 + lo3(d)))});
            asmU64(a, convert<u64>(x));
        }
    }

    // moveRegs moves registers in parallel: a move whose destination no other
    // move still reads goes first; when every destination is read, the first
    // source goes to scratch, which no move names, and its readers read that.
    static void asmMoveRegs(Asm* a, Slice<RegMove> moves, VReg scratch) {
        Slice<RegMove> pending = {};
        for (Int i = 0; i < length(moves); ++i) {
            if (moves[i].dst != moves[i].src) {
                pending = appendValues(pending, moves[i]);
            } else if (moves[i].size == 4) {
                asmMovRR(a, 4, moves[i].dst, moves[i].src);
            }
        }
        while (length(pending) > 0) {
            Int done = -1;
            for (Int i = 0; i < length(pending) && done < 0; ++i) {
                bool blocked = false;
                for (Int j = 0; j < length(pending); ++j) {
                    if (j != i && pending[j].src == pending[i].dst) {
                        blocked = true;
                        break;
                    }
                }
                if (!blocked) {
                    asmMovRR(a, pending[i].size, pending[i].dst, pending[i].src);
                    done = i;
                }
            }
            if (done >= 0) {
                pending = appendSlice(subslice(pending, 0, done), subslice(pending, done + 1, length(pending)));
                continue;
            }
            VReg s = pending[0].src;
            asmMovRR(a, 8, scratch, s);
            for (Int j = 0; j < length(pending); ++j) {
                if (pending[j].src == s) {
                    pending[j].src = scratch;
                }
            }
        }
    }

    static void asmMovRR(Asm* a, u8 size, VReg dst, VReg src) {
        asmOpRR(a, size == 8, Span<const u8>({u8(139LL)}), convert<i64>(dst), convert<i64>(src), false);
    }

    static void asmMovXG(Asm* a, u8 size, VReg dst, VReg src) {
        asmVexRR(a, pp66, 1, size == 8, 126, regNum(src), -1, convert<i64>(dst));
    }

    static void asmMoveSP(Asm* a, i32 by) {
        asmSetSP(a, a->sp + by);
    }

    static void asmNops(Asm* a, i32 n) {
        while (n > i32(0LL)) {
            i32 width = minimum(n, 9);
            appendSpan(a->buf, Span<const u8>(nopBytes[width]));
            n -= width;
        }
    }

    static void asmOpRM(Asm* a, bool w, Span<const u8> op, Int reg, Amem m, Int imm, bool byteReg) {
        asmRex(a, w, reg, memX(m), memB(m), byteReg && reg >= 4);
        asmB(a, op);
        asmModrmMem(a, reg, m, imm);
    }

    // opRR emits an instruction "op reg, rm" between registers.
    static void asmOpRR(Asm* a, bool w, Span<const u8> op, Int reg, Int rm, bool byteRegs) {
        asmRex(a, w, reg, -1, rm, byteRegs && (reg >= 4 || rm >= 4));
        asmB(a, op);
        asmModrmReg(a, reg, rm);
    }

    // patch32 stores a displacement to target, relative to the end of the
    // field at at.
    static void asmPatch32(Asm* a, i32 at, i32 target) {
        littlePutUint32(subslice(a->buf, at, length(a->buf)), convert<u32>(target - (at + 4)));
    }

    // patch8 points the short jump whose displacement is at at to target.
    static void asmPatch8(Asm* a, i32 at, i32 target) {
        i32 d = target - (at + 1);
        if (d != convert<i32>(convert<i8>(d))) {
            compilerError(String("internal error: short jump too far"));
        }
        a->buf[at] = convert<u8>(convert<i8>(d));
    }

    static void asmPop(Asm* a, VReg r) {
        asmRex(a, false, -1, -1, convert<i64>(r), false);
        asmBytes(a, {u8(u8(88 + lo3(convert<i64>(r))))});
        asmMoveSP(a, -8);
    }

    static void asmPopMem(Asm* a, Amem m) {
        asmOpRM(a, false, Span<const u8>({u8(143LL)}), 0, m, 0, false);
        asmMoveSP(a, -8);
    }

    static i32 asmPos(Asm* a) {
        return convert<i32>(length(a->buf));
    }

    // prefixed emits a mandatory prefix (66, F2, F3) before the REX prefix.
    static void asmPrefixed(Asm* a, u8 prefix, bool w, Span<const u8> op, Int reg, Int rm) {
        asmBytes(a, {u8(prefix)});
        asmOpRR(a, w, op, reg, rm, false);
    }

    static void asmPush(Asm* a, VReg r) {
        asmRex(a, false, -1, -1, convert<i64>(r), false);
        asmBytes(a, {u8(u8(80 + lo3(convert<i64>(r))))});
        asmMoveSP(a, 8);
    }

    static void asmPushZero(Asm* a) {
        asmBytes(a, {u8(u8(106LL)), u8(u8(0LL))});
        asmMoveSP(a, 8);
    }

    static void asmPushMem(Asm* a, Amem m) {
        asmOpRM(a, false, Span<const u8>({u8(255LL)}), 6, m, 0, false);
        asmMoveSP(a, 8);
    }

    static void asmRet(Asm* a) {
        asmBytes(a, {u8(u8(195LL))});
    }

    // rex emits a REX prefix when needed: w for 64-bit operands, r extends
    // ModRM.reg, x SIB.index, b ModRM.rm or SIB.base; force for byte registers
    // spl..dil.
    static void asmRex(Asm* a, bool w, Int r, Int x, Int b, bool force) {
        u8 v = 64;
        if (w) {
            v |= 8;
        }
        if (r >= 0) {
            v |= shiftLeft(hi(r), 2);
        }
        if (x >= 0) {
            v |= shiftLeft(hi(x), 1);
        }
        if (b >= 0) {
            v |= hi(b);
        }
        if (v != 64 || force) {
            asmBytes(a, {u8(v)});
        }
    }

    // setSP declares the stack pointer's delta from here on: at the start of a
    // region and where control arrives from elsewhere.
    static void asmSetSP(Asm* a, i32 delta) {
        Int n = length(a->sps);
        if (n > 0 && a->sps[n - 1].pc == asmPos(a)) {
            a->sps[n - 1].delta = delta;
        } else if (n == 0 || a->sps[n - 1].delta != delta) {
            a->sps = appendValues(a->sps, SpEntry{asmPos(a), delta});
        }
        a->sp = delta;
    }

    static void asmSetcc(Asm* a, u8 cc, VReg dst) {
        Int d = convert<i64>(dst);
        asmRex(a, false, -1, -1, d, d >= 4);
        asmBytes(a, {u8(u8(15LL)), u8(u8(144 + cc))});
        asmModrmReg(a, 0, d);
        asmRex(a, false, d, -1, d, d >= 4);
        asmBytes(a, {u8(u8(15LL)), u8(u8(182LL))});
        asmModrmReg(a, d, d);
    }

    static void asmShiftCL(Asm* a, u8 kind, u8 size, VReg dst) {
        Int d = convert<i64>(dst);
        asmRex(a, size == 8, -1, -1, d, false);
        asmBytes(a, {u8(u8(211LL))});
        asmModrmReg(a, convert<i64>(kind), d);
    }

    static void asmShiftImm(Asm* a, u8 kind, u8 size, VReg dst, i64 n) {
        Int d = convert<i64>(dst);
        asmRex(a, size == 8, -1, -1, d, false);
        if (n == 1) {
            asmBytes(a, {u8(u8(209LL))});
            asmModrmReg(a, convert<i64>(kind), d);
            return;
        }
        asmBytes(a, {u8(u8(193LL))});
        asmModrmReg(a, convert<i64>(kind), d);
        asmBytes(a, {u8(convert<u8>(n))});
    }

    static void asmShiftX(Asm* a, u8 kind, u8 size, VReg dst, VReg src, VReg count) {
        u8 pp = 1;
        switch (kind) {
            case shiftShr: {
                pp = ppF2;
                break;
            }
            case shiftSar: {
                pp = ppF3;
                break;
            }
        }
        asmVexRR(a, pp, 2, size == 8, 247, convert<i64>(dst), convert<i64>(count), convert<i64>(src));
    }

    // spTable returns the deltas of the region [start, end): the one at its
    // start, then every change, at offsets from start.
    // markPos notes that the code from here on is of the wasm instruction at
    // pos: nothing for 0, the lowering's own instructions, which stay with
    // the one before them.
    static void asmMarkPos(Asm* a, u32 pos) {
        if (pos == 0 || pos == a->lastPos) {
            return;
        }
        a->lastPos = pos;
        const i32 pc = asmPos(a);
        if (length(a->lines) && a->lines[length(a->lines) - 1].pc == pc) {
            a->lines[length(a->lines) - 1].pos = pos;
            return;
        }
        a->lines = appendValues(a->lines, LineMark{pc, pos});
    }

    // lineTable is the marks of a region, from its start: the region's
    // prologue counts as its first instruction's.
    static Slice<LineMark> asmLineTable(Asm* a, i32 start, i32 end) {
        Int k = sortSearch(length(a->lines), [&](Int i) mutable -> bool {
            return a->lines[i].pc >= start;
        });
        Slice<LineMark> out = {};
        for (Int i = k; i < length(a->lines) && a->lines[i].pc < end; ++i) {
            const LineMark m = a->lines[i];
            if (length(out) == 0) {
                out = appendValues(out, LineMark{0, m.pos});
            } else {
                out = appendValues(out, LineMark{m.pc - start, m.pos});
            }
        }
        return out;
    }

    static Slice<SpEntry> asmSpTable(Asm* a, i32 start, i32 end) {
        Int k = sortSearch(length(a->sps), [&](Int i) mutable -> bool {
            return a->sps[i].pc > start;
        });
        Slice<SpEntry> out = sliceOf<SpEntry>({SpEntry{0, 0}});
        if (k > 0) {
            out[0].delta = a->sps[k - 1].delta;
        }
        auto sps = subslice(a->sps, k, length(a->sps));
        for (Int i = 0; i < length(sps); ++i) {
            SpEntry e = sps[i];
            if (e.pc >= end) {
                break;
            }
            if (e.delta != out[length(out) - 1].delta) {
                out = appendValues(out, SpEntry{e.pc - start, e.delta});
            }
        }
        return out;
    }

    static void asmStore(Asm* a, u8 width, VReg src, Amem m) {
        Int s = convert<i64>(src);
        switch (width) {
            case 8: {
                asmOpRM(a, true, Span<const u8>({u8(137LL)}), s, m, 0, false);
                break;
            }
            case 4: {
                asmOpRM(a, false, Span<const u8>({u8(137LL)}), s, m, 0, false);
                break;
            }
            case 2: {
                asmBytes(a, {u8(u8(102LL))});
                asmOpRM(a, false, Span<const u8>({u8(137LL)}), s, m, 0, false);
                break;
            }
            default: {
                asmOpRM(a, false, Span<const u8>({u8(136LL)}), s, m, 0, true);
                break;
            }
        }
    }

    static void asmStoreImm(Asm* a, u8 width, Amem m, i64 x) {
        switch (width) {
            case 8: {
                asmOpRM(a, true, Span<const u8>({u8(199LL)}), 0, m, 4, false);
                asmU32(a, convert<u32>(x));
                break;
            }
            case 4: {
                asmOpRM(a, false, Span<const u8>({u8(199LL)}), 0, m, 4, false);
                asmU32(a, convert<u32>(x));
                break;
            }
            case 2: {
                asmBytes(a, {u8(u8(102LL))});
                asmOpRM(a, false, Span<const u8>({u8(199LL)}), 0, m, 2, false);
                asmBytes(a, {u8(convert<u8>(x)), u8(convert<u8>(shiftRight(x, 8)))});
                break;
            }
            default: {
                asmOpRM(a, false, Span<const u8>({u8(198LL)}), 0, m, 1, false);
                asmBytes(a, {u8(convert<u8>(x))});
                break;
            }
        }
    }

    static void asmTest(Asm* a, u8 size, VReg x, VReg y) {
        asmOpRR(a, size == 8, Span<const u8>({u8(133LL)}), convert<i64>(y), convert<i64>(x), false);
    }

    static void asmTestImm(Asm* a, u8 size, VReg x, i64 imm) {
        asmRex(a, size == 8, -1, -1, convert<i64>(x), false);
        asmBytes(a, {u8(u8(247LL))});
        asmModrmReg(a, 0, convert<i64>(x));
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
        asmRex(a, size == 8, -1, -1, d, false);
        asmBytes(a, {u8(u8(247LL))});
        asmModrmReg(a, 3 - convert<i64>(kind), d);
    }

    // vex emits a VEX prefix: pp selects 66/F3/F2, mmmmm the opcode map,
    // vvvv the extra source register.
    static void asmVex(Asm* a, Int r, Int x, Int b, u8 mmmmm, bool w, Int vvvv, bool l, u8 pp) {
        u8 R = 1;
        u8 X = 1;
        u8 B = 1;
        if (r >= 0 && hi(r) == 1) {
            R = 0;
        }
        if (x >= 0 && hi(x) == 1) {
            X = 0;
        }
        if (b >= 0 && hi(b) == 1) {
            B = 0;
        }
        u8 v = 15;
        if (vvvv >= 0) {
            v = u8(convert<u8>(~vvvv) & 15);
        }
        u8 L = 0;
        if (l) {
            L = 1;
        }
        if (X == 1 && B == 1 && mmmmm == 1 && !w) {
            asmBytes(a, {u8(u8(197LL)), u8(u8(u8(u8(shiftLeft(R, 7) | shiftLeft(v, 3)) | shiftLeft(L, 2)) | pp))});
            return;
        }
        u8 W = 0;
        if (w) {
            W = 1;
        }
        asmBytes(a, {u8(u8(196LL)), u8(u8(u8(u8(shiftLeft(R, 7) | shiftLeft(X, 6)) | shiftLeft(B, 5)) | mmmmm)), u8(u8(u8(u8(shiftLeft(W, 7) | shiftLeft(v, 3)) | shiftLeft(L, 2)) | pp))});
    }

    static void asmVexRM(Asm* a, u8 pp, u8 mmmmm, bool w, u8 op, Int reg, Int vvvv, Amem m, Int imm) {
        asmVex(a, reg, memX(m), memB(m), mmmmm, w, vvvv, false, pp);
        asmBytes(a, {u8(op)});
        asmModrmMem(a, reg, m, imm);
    }

    // vexRR emits a VEX instruction with register operands reg, vvvv and rm.
    static void asmVexRR(Asm* a, u8 pp, u8 mmmmm, bool w, u8 op, Int reg, Int vvvv, Int rm) {
        asmVex(a, reg, -1, rm, mmmmm, w, vvvv, false, pp);
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
        while (true) {
            if (a == b) {
                return true;
            }
            if (ci->rpoIndex[b] <= ci->rpoIndex[a]) {
                return false;
            }
            b = ci->idom[b];
        }
    }

    // preorder numbers the dominator tree depth first; unreachable blocks
    // have no place in it.
    static void cfgInfoPreorder(CfgInfo* ci, Function* f) {
        const Int n = length(ci->idom);
        DominatorChildren children = cfgInfoChildren(ci);
        ci->preorder = makeSlice<i32>(length(ci->order));
        ci->enter = makeSlice<i32>(n);
        ci->leave = makeSlice<i32>(n);
        for (Int b = 0; b < n; ++b) {
            ci->enter[b] = -1;
            ci->leave[b] = -1;
        }
        Vec<i32> stack;
        stack.pushBack(ci->order[0]);
        i32 count = 0;
        while (stack.size()) {
            const i32 b = stack[stack.size() - 1];
            if (ci->enter[b] >= 0) {
                stack.popBack();
                ci->leave[b] = count;
                continue;
            }
            ci->enter[b] = count;
            ci->preorder[count++] = b;
            for (i32 child = children.first[usize(b)]; child >= 0; child = children.next[usize(child)]) {
                stack.pushBack(child);
            }
        }
        for (Int b = 0; b < n; ++b) {
            if (ci->rpoIndex[b] < 0 && !f->blocks[b].dead) {
                ci->preorder = appendValues(ci->preorder, i32(b));
            }
        }
    }

    template <typename F>
    static bool loopEachDominated(CfgInfo* ci, i32 h, F f) {
        for (Int i = ci->enter[h]; i < ci->leave[h]; ++i) {
            if (f(ci->preorder[i])) {
                return true;
            }
        }
        const Int analyzed = length(ci->rpoIndex);
        for (Int k = 0; k < length(ci->added); ++k) {
            const i32 b = i32(analyzed + k);
            if (loopDominates(ci, h, b) && f(b)) {
                return true;
            }
        }
        return false;
    }

    // loopDominates also answers for blocks added after the analysis: every
    // path to one passes its preheader, so an analyzed block dominates it
    // exactly when it dominates that preheader. Loops read no value defined
    // in an added block, which is taken to dominate no analyzed one.
    static bool loopDominates(CfgInfo* ci, i32 a, i32 b) {
        const Int analyzed = length(ci->rpoIndex);
        if (a == b) {
            return true;
        }
        if (a >= analyzed) {
            return false;
        }
        while (b >= analyzed) {
            b = ci->added[b - analyzed];
        }
        return cfgInfoDominates(ci, a, b);
    }

    // dominators is Cooper, Harvey and Kennedy's iterative algorithm.
    static void cfgInfoDominators(CfgInfo* ci, Function* f) {
        Int n = length(f->blocks);
        ci->idom = makeSlice<i32>(n);
        for (Int i = 0, end = length(ci->idom); i < end; ++i) {
            ci->idom[i] = -1;
        }
        i32 entry = ci->order[0];
        ci->idom[entry] = entry;
        bool changed = true;
        while (changed) {
            changed = false;
            auto order = subslice(ci->order, 1, length(ci->order));
            for (Int i = 0; i < length(order); ++i) {
                i32 b = order[i];
                i32 newIdom = -1;
                auto preds = f->blocks[b].preds;
                for (Int i = 0; i < length(preds); ++i) {
                    i32 p = preds[i];
                    if (ci->idom[p] < 0) {
                        continue;
                    }
                    if (newIdom < 0) {
                        newIdom = p;
                    } else {
                        newIdom = cfgInfoIntersect(ci, p, newIdom);
                    }
                }
                if (newIdom != ci->idom[b]) {
                    ci->idom[b] = newIdom;
                    changed = true;
                }
            }
        }
    }

    static i32 cfgInfoIntersect(CfgInfo* ci, i32 a, i32 b) {
        while (a != b) {
            while (ci->rpoIndex[a] > ci->rpoIndex[b]) {
                a = ci->idom[a];
            }
            while (ci->rpoIndex[b] > ci->rpoIndex[a]) {
                b = ci->idom[b];
            }
        }
        return a;
    }

    static void cfgInfoLoops(CfgInfo* ci, Function* f) {
        Int n = length(f->blocks);
        ci->depth = makeSlice<i32>(n);
        ci->header = makeSlice<bool>(n);
        Slice<i32> work = {};
        Slice<i32> inLoop = makeSlice<i32>(n);
        for (Int i = 0; i < length(inLoop); ++i) {
            inLoop[i] = -1;
        }
        auto order = ci->order;
        for (Int i = 0; i < length(order); ++i) {
            i32 h = order[i];
            Slice<i32> latches = {};
            auto preds = f->blocks[h].preds;
            for (Int i = 0; i < length(preds); ++i) {
                i32 p = preds[i];
                if (ci->rpoIndex[p] >= ci->rpoIndex[h]) {
                    latches = appendValues(latches, p);
                }
            }
            if (length(latches) == 0) {
                continue;
            }
            ci->header[h] = true;
            inLoop[h] = h;
            ci->depth[h]++;
            work = appendSlice(subslice(work, 0, 0), latches);
            while (length(work) > Int(0LL)) {
                i32 b = work[length(work) - 1];
                work = subslice(work, 0, length(work) - 1);
                if (inLoop[b] == h || ci->rpoIndex[b] < 0) {
                    continue;
                }
                inLoop[b] = h;
                ci->depth[b]++;
                work = appendSlice(work, f->blocks[b].preds);
            }
        }
    }

    // edge ends the current block's terminator edge list with an edge to b and
    // records it as one of b's predecessors.
    static void frontendAddEdge(Frontend* fe, i32 from, i32 to, Slice<i32> args) {
        Term* t = &fe->f->blocks[from].term;
        t->edges = appendValues(t->edges, Edge{to, args});
        fe->preds[to] = appendValues(fe->preds[to], PredRef{from, convert<i32>(length(t->edges) - 1)});
        if (fe->sealed[to]) {
            compilerError(String("internal error: edge to sealed block b%d"));
        }
    }

    // addExplicit adds a parameter for an operand stack value; edges supply
    // its argument when they are created.
    static i32 frontendAddExplicit(Frontend* fe, i32 b, ValType t) {
        if (length(fe->f->blocks[b].params) != fe->explicitValue[b]) {
            compilerError(String("internal error: explicit parameter after a phi"));
        }
        fe->explicitValue[b]++;
        return frontendAddParam(fe, b, t);
    }

    static __attribute__((noinline)) void frontendRetainOperands(Frontend* fe, i32 phi, Span<const i32> ops) {
        setMap(fe->phiOps, phi, appendSlice(Slice<i32>{}, ops));
    }

    static i32 frontendAddOperands(Frontend* fe, i32 local, i32 phi) {
        i32 b = fe->f->values[phi].block;
        InlineVec<i32, 2> ops;
        ops.reserve(usize(length(fe->preds[b])));
        auto range617 = fe->preds[b];
        for (Int index618 = 0, rangeEnd619 = length(range617); index618 < rangeEnd619; ++index618) {
            PredRef p = range617[index618];
            ops.pushBack(frontendRead(fe, local, p.block));
        }
        const i32 result = frontendTryRemoveTrivial(fe, phi, ops.view());
        if (result == phi) {
            // Removed phis have no later users of their incoming list.
            frontendRetainOperands(fe, phi, ops.view());
        }
        return result;
    }

    static i32 frontendAddParam(Frontend* fe, i32 b, ValType t) {
        Block* blk = &fe->f->blocks[b];
        i32 p = functionNewValue(fe->f, OpParam, t, b, convert<i64>(length(blk->params)), Span<const i32>({}));
        blk->params = appendValues(blk->params, p);
        return p;
    }

    static ValType frontendAddrType(Frontend* fe) {
        if (fe->m->mems[0].limits.is64) {
            return I64;
        }
        return I32;
    }

    static void frontendBinary(Frontend* fe, Op op, ValType t) {
        if (fe->cur < 0) {
            return;
        }
        i32 b = frontendPop(fe);
        i32 a = frontendPop(fe);
        frontendPush(fe, frontendEmit(fe, op, t, 0, Span<const i32>({a, b})));
    }

    static Tuple2<Slice<ValType>, Slice<ValType>> frontendBlockType(Frontend* fe) {
        Reader* r = &fe->r;
        if (r->pos < length(r->data)) {
            u8 b = r->data[r->pos];
            switch (b) {
                case 64: {
                    r->pos++;
                    return {{}, {}};
                }
                case 127:
                case 126:
                case 125:
                case 124:
                case 123:
                case 112:
                case 111: {
                    r->pos++;
                    return {{}, sliceOf<ValType>({convert<ValType>(b)})};
                }
            }
        }
        i64 index = readerSleb(r, 33);
        if (index < 0 || convert<i64>(index) >= length(fe->m->types)) {
            compilerError(String("unknown type %d"));
        }
        FuncType* t = &fe->m->types[index];
        return {t->params, t->results};
    }

    static void frontendBody(Frontend* fe) {
        Reader* r = &fe->r;
        // The function's body in the code section's contents: the module's
        // line table counts addresses from them.
        const Int body = fe->m->funcs[fe->f->index].offset - fe->m->codeOffset;
        while (length(fe->ctrls) > Int(0LL)) {
            fe->pos = convert<u32>(body + r->pos);
            u8 op = readerByte(r);
            if (fe->cur < 0) {
                frontendSkip(fe, op);
                continue;
            }
            switch (op) {
                case 0: {
                    fe->f->blocks[fe->cur].term = Term{TermTrap, {}, {}, {}, trapUnreachable, fe->pos};
                    frontendMarkDead(fe);
                    break;
                }
                case 1: {
                    break;
                }
                case 2:
                case 3: {
                    auto temporary622 = frontendBlockType(fe);
                    Slice<ValType> params = temporary622.v0;
                    Slice<ValType> results = temporary622.v1;
                    Slice<i32> args = frontendPopN(fe, length(params));
                    Control c = Control{ctrlBlock, params, results, length(fe->stack), {}, -1, {}, {}, {}};
                    if (op == 3) {
                        c.kind = ctrlLoop;
                        fe->f->hasLoops = true;
                        i32 header = frontendNewBlock(fe);
                        c.target = header;
                        Slice<i32> vals = makeSlice<i32>(length(params));
                        for (Int i = 0; i < length(params); ++i) {
                            ValType t = params[i];
                            vals[i] = frontendAddExplicit(fe, header, t);
                        }
                        frontendJump(fe, header, args);
                        fe->cur = header;
                        args = vals;
                    } else {
                        c.target = frontendNewBlock(fe);
                        for (Int i = 0; i < length(results); ++i) {
                            ValType t = results[i];
                            frontendAddExplicit(fe, c.target, t);
                        }
                    }
                    fe->ctrls = appendValues(fe->ctrls, c);
                    fe->stack = appendSlice(fe->stack, args);
                    break;
                }
                case 4: {
                    auto temporary629 = frontendBlockType(fe);
                    Slice<ValType> params = temporary629.v0;
                    Slice<ValType> results = temporary629.v1;
                    i32 cond = frontendPop(fe);
                    Slice<i32> args = frontendPopN(fe, length(params));
                    Control c = Control{ctrlIf, params, results, length(fe->stack), {}, {}, args, {}, {}};
                    c.target = frontendNewBlock(fe);
                    for (Int i = 0; i < length(results); ++i) {
                        ValType t = results[i];
                        frontendAddExplicit(fe, c.target, t);
                    }
                    i32 then = frontendNewBlock(fe);
                    c.elseBlock = frontendNewBlock(fe);
                    Block* blk = &fe->f->blocks[fe->cur];
                    blk->term.kind = TermIf;
                    blk->term.value = cond;
                    frontendAddEdge(fe, fe->cur, then, {});
                    frontendAddEdge(fe, fe->cur, c.elseBlock, {});
                    frontendSeal(fe, then);
                    frontendSeal(fe, c.elseBlock);
                    fe->cur = then;
                    fe->ctrls = appendValues(fe->ctrls, c);
                    fe->stack = appendSlice(fe->stack, args);
                    break;
                }
                case 5: {
                    frontendElseOp(fe);
                    break;
                }
                case 11: {
                    frontendEnd(fe);
                    break;
                }
                case 12: {
                    Control* c = frontendLabel(fe, readerU32(r));
                    frontendJump(fe, c->target, frontendTop(fe, length(frontendLabelTypes(fe, c))));
                    frontendMarkDead(fe);
                    break;
                }
                case 13: {
                    Control* c = frontendLabel(fe, readerU32(r));
                    i32 cond = frontendPop(fe);
                    Slice<i32> args = frontendTop(fe, length(frontendLabelTypes(fe, c)));
                    i32 next = frontendNewBlock(fe);
                    Block* blk = &fe->f->blocks[fe->cur];
                    blk->term.kind = TermIf;
                    blk->term.value = cond;
                    frontendAddEdge(fe, fe->cur, c->target, args);
                    frontendAddEdge(fe, fe->cur, next, {});
                    frontendSeal(fe, next);
                    fe->cur = next;
                    break;
                }
                case 14: {
                    u32 n = readerU32(r);
                    if (n > 1048576) {
                        compilerError(String("br_table too large"));
                    }
                    Slice<u32> labels = makeSlice<u32>(n + 1);
                    for (Int i = 0; i < length(labels); ++i) {
                        labels[i] = readerU32(r);
                    }
                    i32 index = frontendPop(fe);
                    Int arity = length(frontendLabelTypes(fe, frontendLabel(fe, labels[n])));
                    Slice<i32> args = frontendTop(fe, arity);
                    Block* blk = &fe->f->blocks[fe->cur];
                    blk->term.kind = TermSwitch;
                    blk->term.value = index;
                    for (Int i = 0; i < length(labels); ++i) {
                        u32 l = labels[i];
                        Control* c = frontendLabel(fe, l);
                        if (length(frontendLabelTypes(fe, c)) != arity) {
                            compilerError(String("type mismatch: br_table arity"));
                        }
                        frontendAddEdge(fe, fe->cur, c->target, appendSlice(Slice<i32>{}, args));
                    }
                    frontendMarkDead(fe);
                    break;
                }
                case 15: {
                    fe->f->blocks[fe->cur].term = Term{TermReturn, {}, {}, frontendTop(fe, length(fe->f->typ->results)), {}};
                    frontendMarkDead(fe);
                    break;
                }
                case 16: {
                    u32 index = readerU32(r);
                    if (convert<i64>(index) >= length(fe->m->funcs)) {
                        compilerError(String("unknown function %d"));
                    }
                    frontendCall(fe, OpCall, moduleFuncType(fe->m, index), convert<i64>(index), 0, {});
                    break;
                }
                case 17: {
                    u32 typeIndex = readerU32(r);
                    u32 table = readerU32(r);
                    if (convert<i64>(typeIndex) >= length(fe->m->types)) {
                        compilerError(String("unknown type %d"));
                    }
                    if (convert<i64>(table) >= length(fe->m->tables)) {
                        compilerError(String("unknown table %d"));
                    }
                    i32 elem = frontendPop(fe);
                    frontendCall(fe, OpCallIndirect, &fe->m->types[typeIndex], convert<i64>(typeIndex), convert<i32>(table), sliceOf<i32>({elem}));
                    break;
                }
                case 26: {
                    frontendPop(fe);
                    break;
                }
                case 27:
                case 28: {
                    if (op == 28) {
                        u32 n = readerU32(r);
                        u32 i = 0;
                        for (; i < n; i++) {
                            readerValType(r);
                        }
                    }
                    i32 cond = frontendPop(fe);
                    i32 b = frontendPop(fe);
                    i32 a = frontendPop(fe);
                    frontendPush(fe, frontendEmit(fe, OpSelect, frontendTypeOf(fe, a), 0, Span<const i32>({cond, a, b})));
                    break;
                }
                case 32: {
                    i32 local = frontendLocalIndex(fe);
                    frontendPush(fe, frontendRead(fe, local, fe->cur));
                    break;
                }
                case 33: {
                    i32 local = frontendLocalIndex(fe);
                    frontendWrite(fe, local, fe->cur, frontendPop(fe));
                    break;
                }
                case 34: {
                    i32 local = frontendLocalIndex(fe);
                    i32 v = frontendPop(fe);
                    frontendWrite(fe, local, fe->cur, v);
                    frontendPush(fe, v);
                    break;
                }
                case 35: {
                    u32 index = frontendGlobalIndex(fe);
                    frontendPush(fe, frontendEmit(fe, OpGlobalGet, fe->m->globals[index].typ, convert<i64>(index), Span<const i32>({})));
                    break;
                }
                case 36: {
                    u32 index = frontendGlobalIndex(fe);
                    frontendEmit(fe, OpGlobalSet, TNone, convert<i64>(index), Span<const i32>({frontendPop(fe)}));
                    break;
                }
                case 37: {
                    u32 table = frontendTableIndex(fe);
                    i32 index = frontendPop(fe);
                    frontendPush(fe, frontendEmit(fe, OpTableGet, fe->m->tables[table].elem, convert<i64>(table), Span<const i32>({index})));
                    break;
                }
                case 38: {
                    u32 table = frontendTableIndex(fe);
                    i32 val = frontendPop(fe);
                    i32 index = frontendPop(fe);
                    frontendEmit(fe, OpTableSet, TNone, convert<i64>(table), Span<const i32>({index, val}));
                    break;
                }
                case 40: {
                    frontendLoad(fe, I32, 4, false);
                    break;
                }
                case 41: {
                    frontendLoad(fe, I64, 8, false);
                    break;
                }
                case 42: {
                    frontendLoad(fe, F32, 4, false);
                    break;
                }
                case 43: {
                    frontendLoad(fe, F64, 8, false);
                    break;
                }
                case 44: {
                    frontendLoad(fe, I32, 1, true);
                    break;
                }
                case 45: {
                    frontendLoad(fe, I32, 1, false);
                    break;
                }
                case 46: {
                    frontendLoad(fe, I32, 2, true);
                    break;
                }
                case 47: {
                    frontendLoad(fe, I32, 2, false);
                    break;
                }
                case 48: {
                    frontendLoad(fe, I64, 1, true);
                    break;
                }
                case 49: {
                    frontendLoad(fe, I64, 1, false);
                    break;
                }
                case 50: {
                    frontendLoad(fe, I64, 2, true);
                    break;
                }
                case 51: {
                    frontendLoad(fe, I64, 2, false);
                    break;
                }
                case 52: {
                    frontendLoad(fe, I64, 4, true);
                    break;
                }
                case 53: {
                    frontendLoad(fe, I64, 4, false);
                    break;
                }
                case 54:
                case 56: {
                    frontendStore(fe, 4);
                    break;
                }
                case 55:
                case 57: {
                    frontendStore(fe, 8);
                    break;
                }
                case 58:
                case 60: {
                    frontendStore(fe, 1);
                    break;
                }
                case 59:
                case 61: {
                    frontendStore(fe, 2);
                    break;
                }
                case 62: {
                    frontendStore(fe, 4);
                    break;
                }
                case 63: {
                    frontendMemIndex(fe);
                    frontendPush(fe, frontendEmit(fe, OpMemSize, frontendAddrType(fe), 0, Span<const i32>({})));
                    break;
                }
                case 64: {
                    frontendMemIndex(fe);
                    i32 delta = frontendPop(fe);
                    frontendPush(fe, frontendEmit(fe, OpMemGrow, frontendAddrType(fe), 0, Span<const i32>({delta})));
                    break;
                }
                case 65: {
                    frontendPush(fe, frontendConstant(fe, I32, convert<i64>(readerS32(r))));
                    break;
                }
                case 66: {
                    frontendPush(fe, frontendConstant(fe, I64, readerS64(r)));
                    break;
                }
                case 67: {
                    frontendPush(fe, frontendConstant(fe, F32, convert<i64>(readerF32(r))));
                    break;
                }
                case 68: {
                    frontendPush(fe, frontendConstant(fe, F64, convert<i64>(readerF64(r))));
                    break;
                }
                case 69: {
                    frontendUnary(fe, OpEqz, I32);
                    break;
                }
                case 80: {
                    frontendUnary(fe, OpEqz, I32);
                    break;
                }
                case 70:
                case 71:
                case 72:
                case 73:
                case 74:
                case 75:
                case 76:
                case 77:
                case 78:
                case 79: {
                    frontendBinary(fe, Op(OpEq + convert<Op>(u8(op - 70))), I32);
                    break;
                }
                case 81:
                case 82:
                case 83:
                case 84:
                case 85:
                case 86:
                case 87:
                case 88:
                case 89:
                case 90: {
                    frontendBinary(fe, Op(OpEq + convert<Op>(u8(op - 81))), I32);
                    break;
                }
                case 91:
                case 92:
                case 93:
                case 94:
                case 95:
                case 96: {
                    frontendBinary(fe, Op(OpFEq + convert<Op>(u8(op - 91))), I32);
                    break;
                }
                case 97:
                case 98:
                case 99:
                case 100:
                case 101:
                case 102: {
                    frontendBinary(fe, Op(OpFEq + convert<Op>(u8(op - 97))), I32);
                    break;
                }
                case 103:
                case 104:
                case 105: {
                    frontendUnary(fe, Op(OpClz + convert<Op>(u8(op - 103))), I32);
                    break;
                }
                case 121:
                case 122:
                case 123: {
                    frontendUnary(fe, Op(OpClz + convert<Op>(u8(op - 121))), I64);
                    break;
                }
                case 106:
                case 107:
                case 108:
                case 109:
                case 110:
                case 111:
                case 112:
                case 113:
                case 114:
                case 115:
                case 116:
                case 117:
                case 118:
                case 119:
                case 120: {
                    frontendBinary(fe, Op(OpAdd + convert<Op>(u8(op - 106))), I32);
                    break;
                }
                case 124:
                case 125:
                case 126:
                case 127:
                case 128:
                case 129:
                case 130:
                case 131:
                case 132:
                case 133:
                case 134:
                case 135:
                case 136:
                case 137:
                case 138: {
                    frontendBinary(fe, Op(OpAdd + convert<Op>(u8(op - 124))), I64);
                    break;
                }
                case 139:
                case 140:
                case 141:
                case 142:
                case 143:
                case 144:
                case 145: {
                    frontendUnary(fe, floatUnary[u8(op - 139)], F32);
                    break;
                }
                case 153:
                case 154:
                case 155:
                case 156:
                case 157:
                case 158:
                case 159: {
                    frontendUnary(fe, floatUnary[u8(op - 153)], F64);
                    break;
                }
                case 146:
                case 147:
                case 148:
                case 149:
                case 150:
                case 151:
                case 152: {
                    frontendBinary(fe, Op(OpFAdd + convert<Op>(u8(op - 146))), F32);
                    break;
                }
                case 160:
                case 161:
                case 162:
                case 163:
                case 164:
                case 165:
                case 166: {
                    frontendBinary(fe, Op(OpFAdd + convert<Op>(u8(op - 160))), F64);
                    break;
                }
                case 167: {
                    frontendUnary(fe, OpWrap, I32);
                    break;
                }
                case 168:
                case 170: {
                    frontendUnary(fe, OpTruncS, I32);
                    break;
                }
                case 169:
                case 171: {
                    frontendUnary(fe, OpTruncU, I32);
                    break;
                }
                case 172: {
                    frontendUnary(fe, OpExtendS, I64);
                    break;
                }
                case 173: {
                    frontendUnary(fe, OpExtendU, I64);
                    break;
                }
                case 174:
                case 176: {
                    frontendUnary(fe, OpTruncS, I64);
                    break;
                }
                case 175:
                case 177: {
                    frontendUnary(fe, OpTruncU, I64);
                    break;
                }
                case 178:
                case 180: {
                    frontendUnary(fe, OpConvertS, F32);
                    break;
                }
                case 179:
                case 181: {
                    frontendUnary(fe, OpConvertU, F32);
                    break;
                }
                case 182: {
                    frontendUnary(fe, OpDemote, F32);
                    break;
                }
                case 183:
                case 185: {
                    frontendUnary(fe, OpConvertS, F64);
                    break;
                }
                case 184:
                case 186: {
                    frontendUnary(fe, OpConvertU, F64);
                    break;
                }
                case 187: {
                    frontendUnary(fe, OpPromote, F64);
                    break;
                }
                case 188: {
                    frontendUnary(fe, OpReinterpret, I32);
                    break;
                }
                case 189: {
                    frontendUnary(fe, OpReinterpret, I64);
                    break;
                }
                case 190: {
                    frontendUnary(fe, OpReinterpret, F32);
                    break;
                }
                case 191: {
                    frontendUnary(fe, OpReinterpret, F64);
                    break;
                }
                case 192: {
                    frontendUnary(fe, OpExtend8S, I32);
                    break;
                }
                case 193: {
                    frontendUnary(fe, OpExtend16S, I32);
                    break;
                }
                case 194: {
                    frontendUnary(fe, OpExtend8S, I64);
                    break;
                }
                case 195: {
                    frontendUnary(fe, OpExtend16S, I64);
                    break;
                }
                case 196: {
                    frontendUnary(fe, OpExtend32S, I64);
                    break;
                }
                case 208: {
                    ValType t = readerRefType(r);
                    frontendPush(fe, functionNewValue(fe->f, OpRefNull, t, -1, 0, Span<const i32>({})));
                    break;
                }
                case 209: {
                    frontendUnary(fe, OpRefIsNull, I32);
                    break;
                }
                case 210: {
                    u32 index = readerU32(r);
                    if (convert<i64>(index) >= length(fe->m->funcs)) {
                        compilerError(String("unknown function %d"));
                    }
                    frontendPush(fe, frontendEmit(fe, OpRefFunc, FuncRef, convert<i64>(index), Span<const i32>({})));
                    break;
                }
                case 252: {
                    frontendPrefixFC(fe);
                    break;
                }
                default: {
                    compilerError(String("illegal opcode 0x%02x at offset %d"));
                    break;
                }
            }
        }
        if (!readerEof(r)) {
            compilerError(String("operators remaining after end of function"));
        }
    }

    static void frontendCall(Frontend* fe, Op op, FuncType* t, i64 aux, i32 aux2, Slice<i32> extra) {
        Slice<i32> args = frontendPopN(fe, length(t->params));
        args = appendSlice(args, extra);
        ValType typ = TNone;
        switch (length(t->results)) {
            case 0: {
                break;
            }
            case 1: {
                typ = t->results[0];
                break;
            }
            default: {
                typ = TTuple;
                break;
            }
        }
        i32 v = frontendEmit(fe, op, typ, aux, args);
        fe->f->values[v].aux2 = aux2;
        if (length(t->results) == 1) {
            frontendPush(fe, v);
        } else {
            auto results = t->results;
            for (Int i = 0; i < length(results); ++i) {
                ValType rt = results[i];
                frontendPush(fe, frontendEmit(fe, OpExtract, rt, convert<i64>(i), Span<const i32>({v})));
            }
        }
    }

    static void frontendCheckData(Frontend* fe, u32 seg) {
        if (fe->m->dataCount < 0) {
            compilerError(String("data count section required"));
        }
        if (convert<i64>(seg) >= fe->m->dataCount) {
            compilerError(String("unknown data segment %d"));
        }
    }

    static i32 frontendConstant(Frontend* fe, ValType t, i64 bits) {
        ConstKey key = ConstKey{t, bits};
        {
            auto temporary643 = lookupMap(fe->consts, key);
            i32 v = temporary643.v0;
            bool ok = temporary643.v1;
            if (ok) {
                return v;
            }
        }
        i32 v = functionNewValue(fe->f, OpConst, t, -1, bits, Span<const i32>({}));
        setMap(fe->consts, key, v);
        return v;
    }

    // deadAfter makes the code following a construct unreachable: nothing
    // falls out of it.
    static void frontendDeadAfter(Frontend* fe) {
        fe->cur = -1;
        if (length(fe->ctrls) > 0) {
            Control* c = &fe->ctrls[length(fe->ctrls) - 1];
            c->dead = true;
            fe->stack = subslice(fe->stack, 0, c->height);
        }
    }

    static void frontendElseOp(Frontend* fe) {
        Control* c = &fe->ctrls[length(fe->ctrls) - 1];
        if (c->kind != ctrlIf) {
            compilerError(String("else without if"));
        }
        if (c->outer) {
            c->kind = ctrlElse;
            return;
        }
        if (fe->cur >= 0) {
            frontendJump(fe, c->target, frontendPopN(fe, length(c->results)));
        }
        fe->stack = appendSlice(subslice(fe->stack, 0, c->height), c->elseArgs);
        fe->cur = c->elseBlock;
        c->elseBlock = -1;
        c->kind = ctrlElse;
        c->dead = false;
    }

    // emit appends an instruction to the current block.
    static i32 frontendEmit(Frontend* fe, Op op, ValType t, i64 aux, Span<const i32> args) {
        i32 v = functionNewValue(fe->f, op, t, fe->cur, aux, args);
        fe->f->positions[usize(v)] = fe->pos;
        Block* blk = &fe->f->blocks[fe->cur];
        blk->instrs.pushBack(v);
        return v;
    }

    static void frontendEnd(Frontend* fe) {
        Control* c = &fe->ctrls[length(fe->ctrls) - 1];
        if (c->outer) {
            fe->ctrls = subslice(fe->ctrls, 0, length(fe->ctrls) - 1);
            return;
        }
        bool live = fe->cur >= 0;
        Slice<i32> results = {};
        if (live) {
            results = frontendPopN(fe, length(c->results));
            if (length(fe->stack) != c->height) {
                compilerError(String("type mismatch: values remaining on stack at end of block"));
            }
        }
        Control frame = *c;
        fe->ctrls = subslice(fe->ctrls, 0, length(fe->ctrls) - 1);
        fe->stack = subslice(fe->stack, 0, frame.height);
        if (frame.kind == ctrlLoop) {
            frontendSeal(fe, frame.target);
            if (live) {
                fe->stack = appendSlice(fe->stack, results);
            } else {
                frontendDeadAfter(fe);
            }
            return;
        }
        if (frame.kind == ctrlIf) {
            i32 cur = fe->cur;
            fe->cur = frame.elseBlock;
            frontendJump(fe, frame.target, frame.elseArgs);
            fe->cur = cur;
        }
        if (live && length(fe->preds[frame.target]) == 0) {
            fe->f->blocks[frame.target].dead = true;
            frontendSeal(fe, frame.target);
            if (frame.kind == ctrlFunc) {
                fe->f->blocks[fe->cur].term = Term{TermReturn, {}, {}, results, {}};
                fe->cur = -1;
                return;
            }
            fe->stack = appendSlice(fe->stack, results);
            return;
        }
        if (live) {
            frontendJump(fe, frame.target, results);
        }
        frontendSeal(fe, frame.target);
        if (length(fe->preds[frame.target]) == 0) {
            fe->f->blocks[frame.target].dead = true;
            frontendDeadAfter(fe);
            return;
        }
        fe->cur = frame.target;
        Slice<i32> params = appendSlice(Slice<i32>{}, subslice(fe->f->blocks[frame.target].params, 0, length(frame.results)));
        if (frame.kind == ctrlFunc) {
            fe->f->blocks[fe->cur].term = Term{TermReturn, {}, {}, params, {}};
            fe->cur = -1;
            return;
        }
        fe->stack = appendSlice(fe->stack, params);
    }

    static u32 frontendGlobalIndex(Frontend* fe) {
        u32 index = readerU32(&fe->r);
        if (convert<i64>(index) >= length(fe->m->globals)) {
            compilerError(String("unknown global %d"));
        }
        return index;
    }

    static void frontendJump(Frontend* fe, i32 to, Slice<i32> args) {
        fe->f->blocks[fe->cur].term.kind = TermJump;
        frontendAddEdge(fe, fe->cur, to, args);
    }

    static Control* frontendLabel(Frontend* fe, u32 depth) {
        if (convert<i64>(depth) >= length(fe->ctrls)) {
            compilerError(String("unknown label %d"));
        }
        return &fe->ctrls[length(fe->ctrls) - 1 - convert<i64>(depth)];
    }

    // branchArgs are the values a branch to frame c passes: a loop's parameters
    // or a block's results.
    static Slice<ValType> frontendLabelTypes(Frontend* fe, Control* c) {
        if (c->kind == ctrlLoop) {
            return c->params;
        }
        return c->results;
    }

    static void frontendLoad(Frontend* fe, ValType t, Int width, bool signedValue) {
        auto temporary644 = frontendMemarg(fe);
        i64 offset = temporary644.v0;
        if (fe->cur < 0) {
            return;
        }
        i32 addr = frontendPop(fe);
        i32 v = frontendEmit(fe, OpLoad, t, offset, Span<const i32>({addr}));
        fe->f->values[v].aux2 = convert<i32>(makeMemKind(width, signedValue));
        frontendPush(fe, v);
    }

    static i32 frontendLocalIndex(Frontend* fe) {
        u32 index = readerU32(&fe->r);
        if (convert<i64>(index) >= length(fe->locals)) {
            compilerError(String("unknown local %d"));
        }
        return convert<i32>(index);
    }

    // markDead makes the rest of the current frame unreachable.
    static void frontendMarkDead(Frontend* fe) {
        Control* c = &fe->ctrls[length(fe->ctrls) - 1];
        c->dead = true;
        fe->stack = subslice(fe->stack, 0, c->height);
        fe->cur = -1;
    }

    static void frontendMemIndex(Frontend* fe) {
        u32 mem = readerU32(&fe->r);
        if (mem != 0 || length(fe->m->mems) == 0) {
            compilerError(String("unknown memory %d"));
        }
    }

    static Tuple2<i64, i32> frontendMemarg(Frontend* fe) {
        Reader* r = &fe->r;
        u32 align = readerU32(r);
        u32 mem = 0;
        if ((align & 64) != 0) {
            align &= ~u32(64LL);
            mem = readerU32(r);
        }
        if (mem != 0 || length(fe->m->mems) == 0) {
            compilerError(String("unknown memory %d"));
        }
        if (align >= 32) {
            compilerError(String("malformed memop flags"));
        }
        if (fe->m->mems[0].limits.is64) {
            return {convert<i64>(readerLeb(r, 64)), convert<i32>(align)};
        }
        return {convert<i64>(readerU32(r)), convert<i32>(align)};
    }

    static i32 frontendNewBlock(Frontend* fe) {
        i32 b = functionNewBlock(fe->f);
        fe->defs = appendValues(fe->defs, zeroValue<Slice<i32>>());
        fe->sealed = appendValues(fe->sealed, false);
        fe->incomplete = appendValues(fe->incomplete, zeroValue<Slice<PendingPhi>>());
        fe->preds = appendValues(fe->preds, zeroValue<Slice<PredRef>>());
        fe->explicitValue = appendValues(fe->explicitValue, 0);
        return b;
    }

    // paramSlot maps a parameter value to its alias slot. Aliases are kept per
    // value; values created after the parameter grow the table on demand.
    static i32 frontendParamSlot(Frontend* fe, i32 v) {
        while (convert<i64>(v) >= length(fe->alias)) {
            fe->alias.pushBack(-1);
        }
        return v;
    }

    static i32 frontendPop(Frontend* fe) {
        if (length(fe->stack) <= fe->ctrls[length(fe->ctrls) - 1].height) {
            compilerError(String("type mismatch: operand stack underflow"));
        }
        i32 v = fe->stack[length(fe->stack) - 1];
        fe->stack = subslice(fe->stack, 0, length(fe->stack) - 1);
        return v;
    }

    // popN pops n values and returns them in stack order.
    static Slice<i32> frontendPopN(Frontend* fe, Int n) {
        if (length(fe->stack) - n < fe->ctrls[length(fe->ctrls) - 1].height) {
            compilerError(String("type mismatch: operand stack underflow"));
        }
        Slice<i32> vals = appendSlice(Slice<i32>{}, subslice(fe->stack, length(fe->stack) - n, length(fe->stack)));
        fe->stack = subslice(fe->stack, 0, length(fe->stack) - n);
        return vals;
    }

    static void frontendPrefixFC(Frontend* fe) {
        Reader* r = &fe->r;
        u32 sub = readerU32(r);
        switch (sub) {
            case 0:
            case 1:
            case 2:
            case 3: {
                Op op = OpTruncSatS;
                if ((sub & 1) != 0) {
                    op = OpTruncSatU;
                }
                frontendUnary(fe, op, I32);
                break;
            }
            case 4:
            case 5:
            case 6:
            case 7: {
                Op op = OpTruncSatS;
                if ((sub & 1) != 0) {
                    op = OpTruncSatU;
                }
                frontendUnary(fe, op, I64);
                break;
            }
            case 8: {
                u32 seg = readerU32(r);
                frontendMemIndex(fe);
                frontendCheckData(fe, seg);
                i32 n = frontendPop(fe);
                i32 s = frontendPop(fe);
                i32 d = frontendPop(fe);
                frontendEmit(fe, OpMemInit, TNone, convert<i64>(seg), Span<const i32>({d, s, n}));
                break;
            }
            case 9: {
                u32 seg = readerU32(r);
                frontendCheckData(fe, seg);
                frontendEmit(fe, OpDataDrop, TNone, convert<i64>(seg), Span<const i32>({}));
                break;
            }
            case 10: {
                frontendMemIndex(fe);
                frontendMemIndex(fe);
                i32 n = frontendPop(fe);
                i32 s = frontendPop(fe);
                i32 d = frontendPop(fe);
                frontendEmit(fe, OpMemCopy, TNone, 0, Span<const i32>({d, s, n}));
                break;
            }
            case 11: {
                frontendMemIndex(fe);
                i32 n = frontendPop(fe);
                i32 val = frontendPop(fe);
                i32 d = frontendPop(fe);
                frontendEmit(fe, OpMemFill, TNone, 0, Span<const i32>({d, val, n}));
                break;
            }
            case 12: {
                u32 seg = readerU32(r);
                u32 table = frontendTableIndex(fe);
                if (convert<i64>(seg) >= length(fe->m->elems)) {
                    compilerError(String("unknown elem segment %d"));
                }
                i32 n = frontendPop(fe);
                i32 s = frontendPop(fe);
                i32 d = frontendPop(fe);
                i32 v = frontendEmit(fe, OpTableInit, TNone, convert<i64>(seg), Span<const i32>({d, s, n}));
                fe->f->values[v].aux2 = convert<i32>(table);
                break;
            }
            case 13: {
                u32 seg = readerU32(r);
                if (convert<i64>(seg) >= length(fe->m->elems)) {
                    compilerError(String("unknown elem segment %d"));
                }
                frontendEmit(fe, OpElemDrop, TNone, convert<i64>(seg), Span<const i32>({}));
                break;
            }
            case 14: {
                u32 dst = frontendTableIndex(fe);
                u32 src = frontendTableIndex(fe);
                i32 n = frontendPop(fe);
                i32 s = frontendPop(fe);
                i32 d = frontendPop(fe);
                i32 v = frontendEmit(fe, OpTableCopy, TNone, convert<i64>(dst), Span<const i32>({d, s, n}));
                fe->f->values[v].aux2 = convert<i32>(src);
                break;
            }
            case 15: {
                u32 table = frontendTableIndex(fe);
                i32 n = frontendPop(fe);
                i32 val = frontendPop(fe);
                frontendPush(fe, frontendEmit(fe, OpTableGrow, I32, convert<i64>(table), Span<const i32>({val, n})));
                break;
            }
            case 16: {
                u32 table = frontendTableIndex(fe);
                frontendPush(fe, frontendEmit(fe, OpTableSize, I32, convert<i64>(table), Span<const i32>({})));
                break;
            }
            case 17: {
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

    static void frontendPush(Frontend* fe, i32 v) {
        fe->stack = appendValues(fe->stack, v);
    }

    static i32 frontendRead(Frontend* fe, i32 local, i32 b) {
        Slice<i32> d = fe->defs[b];
        if (!isNil(d) && d[local] >= 0) {
            return frontendResolve(fe, d[local]);
        }
        i32 v = {};
        ValType t = fe->locals[local];
        if (!fe->sealed[b]) {
            v = frontendAddParam(fe, b, t);
            fe->incomplete[b] = appendValues(fe->incomplete[b], PendingPhi{local, v});
        } else if (length(fe->preds[b]) == 1) {
            v = frontendRead(fe, local, fe->preds[b][0].block);
        } else if (length(fe->preds[b]) == 0) {
            v = functionNewValue(fe->f, OpUndef, t, -1, 0, Span<const i32>({}));
        } else {
            v = frontendAddParam(fe, b, t);
            frontendWrite(fe, local, b, v);
            v = frontendAddOperands(fe, local, v);
        }
        frontendWrite(fe, local, b, v);
        return v;
    }

    static i32 frontendResolve(Frontend* fe, i32 v) {
        while (convert<i64>(v) < length(fe->alias) && fe->alias[v] >= i32(0LL)) {
            v = fe->alias[v];
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
        Function* f = fe->f;
        for (Int b = 0, end = length(f->blocks); b < end; ++b) {
            Block* blk = &f->blocks[b];
            if (blk->dead) {
                continue;
            }
            Slice<i32> params = subslice(blk->params, 0, fe->explicitValue[b]);
            auto range651 = subslice(blk->params, fe->explicitValue[b], length(blk->params));
            for (Int index652 = 0, rangeEnd653 = length(range651); index652 < rangeEnd653; ++index652) {
                i32 p = range651[index652];
                if (frontendResolve(fe, p) != p) {
                    continue;
                }
                f->values[p].aux = convert<i64>(length(params));
                params = appendValues(params, p);
                auto range654 = fe->preds[b];
                for (Int index655 = 0, rangeEnd656 = length(range654); index655 < rangeEnd656; ++index655) {
                    Int i = index655;
                    PredRef pr = range654[index655];
                    Edge* e = &f->blocks[pr.block].term.edges[pr.edge];
                    e->args = appendValues(e->args, fe->phiOps[p][i]);
                }
            }
            blk->params = params;
        }
        if (fe->alias.size()) {
            frontendCompactValues(fe);
            fe->alias = Vec<i32>{};
        }
        functionComputePreds(f);
    }

    static void frontendRun(Frontend* fe) {
        Function* f = fe->f;
        Reader* r = &fe->r;
        fe->locals = appendSlice(fe->locals, f->typ->params);
        u32 groups = readerU32(r);
        u64 total = convert<u64>(length(fe->locals));
        {
            u32 i = 0;
            for (; i < groups; i++) {
                u32 n = readerU32(r);
                ValType t = readerValType(r);
                total += convert<u64>(n);
                if (total > 50000) {
                    compilerError(String("too many locals"));
                }
                u32 j = 0;
                for (; j < n; j++) {
                    fe->locals = appendValues(fe->locals, t);
                }
            }
        }
        i32 entry = frontendNewBlock(fe);
        frontendSeal(fe, entry);
        fe->cur = entry;
        auto params = f->typ->params;
        for (Int i = 0; i < length(params); ++i) {
            ValType t = params[i];
            frontendWrite(fe, convert<i32>(i), entry, frontendAddExplicit(fe, entry, t));
        }
        Int i = length(f->typ->params);
        for (; i < length(fe->locals); i++) {
            frontendWrite(fe, convert<i32>(i), entry, frontendZero(fe, fe->locals[i]));
        }
        i32 ret = frontendNewBlock(fe);
        fe->ctrls = appendValues(fe->ctrls, Control{ctrlFunc, {}, f->typ->results, {}, ret, -1, {}, {}, {}});
        auto results = f->typ->results;
        for (Int i = 0; i < length(results); ++i) {
            ValType t = results[i];
            frontendAddExplicit(fe, ret, t);
        }
        frontendBody(fe);
        frontendResolveAliases(fe);
    }

    static void frontendSeal(Frontend* fe, i32 b) {
        if (fe->sealed[b]) {
            return;
        }
        fe->sealed[b] = true;
        Slice<PendingPhi> pending = fe->incomplete[b];
        fe->incomplete[b] = {};
        for (Int i = 0; i < length(pending); ++i) {
            PendingPhi p = pending[i];
            frontendAddOperands(fe, p.local, p.phi);
        }
    }

    // skip consumes an instruction in unreachable code, tracking only the
    // structure needed to find where reachable code resumes.
    static void frontendSkip(Frontend* fe, u8 op) {
        Reader* r = &fe->r;
        switch (op) {
            case 2:
            case 3:
            case 4: {
                frontendBlockType(fe);
                fe->ctrls = appendValues(fe->ctrls, Control{ctrlBlock, {}, {}, length(fe->stack), {}, -1, {}, {}, true});
                if (op == 4) {
                    fe->ctrls[length(fe->ctrls) - 1].kind = ctrlIf;
                }
                break;
            }
            case 5: {
                frontendElseOp(fe);
                break;
            }
            case 11: {
                frontendEnd(fe);
                break;
            }
            case 12:
            case 13: {
                readerU32(r);
                break;
            }
            case 14: {
                u32 n = readerU32(r);
                u32 i = 0;
                for (; i <= n; i++) {
                    readerU32(r);
                }
                break;
            }
            case 16:
            case 18:
            case 32:
            case 33:
            case 34:
            case 35:
            case 36:
            case 37:
            case 38:
            case 210: {
                readerU32(r);
                break;
            }
            case 17: {
                readerU32(r);
                readerU32(r);
                break;
            }
            case 28: {
                u32 n = readerU32(r);
                u32 i = 0;
                for (; i < n; i++) {
                    readerValType(r);
                }
                break;
            }
            case 63:
            case 64: {
                readerU32(r);
                break;
            }
            case 65: {
                readerS32(r);
                break;
            }
            case 66: {
                readerS64(r);
                break;
            }
            case 67: {
                readerBytes(r, 4);
                break;
            }
            case 68: {
                readerBytes(r, 8);
                break;
            }
            case 208: {
                readerRefType(r);
                break;
            }
            case 252: {
                u32 sub = readerU32(r);
                switch (sub) {
                    case 8:
                    case 12:
                    case 14: {
                        readerU32(r);
                        readerU32(r);
                        break;
                    }
                    case 10: {
                        readerU32(r);
                        readerU32(r);
                        break;
                    }
                    case 9:
                    case 11:
                    case 13:
                    case 15:
                    case 16:
                    case 17: {
                        readerU32(r);
                        break;
                    }
                    default: {
                        if (sub > 7) {
                            compilerError(String("illegal opcode 0xfc %d"));
                        }
                        break;
                    }
                }
                break;
            }
            default: {
                if (op >= 40 && op <= 62) {
                    frontendMemarg(fe);
                } else if (op > 196 && op != 209) {
                    compilerError(String("illegal opcode 0x%02x"));
                }
                break;
            }
        }
    }

    static void frontendStore(Frontend* fe, Int width) {
        auto temporary683 = frontendMemarg(fe);
        i64 offset = temporary683.v0;
        if (fe->cur < 0) {
            return;
        }
        i32 val = frontendPop(fe);
        i32 addr = frontendPop(fe);
        i32 v = frontendEmit(fe, OpStore, TNone, offset, Span<const i32>({addr, val}));
        fe->f->values[v].aux2 = convert<i32>(makeMemKind(width, false));
    }

    static u32 frontendTableIndex(Frontend* fe) {
        u32 index = readerU32(&fe->r);
        if (convert<i64>(index) >= length(fe->m->tables)) {
            compilerError(String("unknown table %d"));
        }
        return index;
    }

    // top returns the n values on top of the stack without popping them.
    static Slice<i32> frontendTop(Frontend* fe, Int n) {
        if (length(fe->stack) - n < fe->ctrls[length(fe->ctrls) - 1].height) {
            compilerError(String("type mismatch: operand stack underflow"));
        }
        return appendSlice(Slice<i32>{}, subslice(fe->stack, length(fe->stack) - n, length(fe->stack)));
    }

    // tryRemoveTrivial replaces a parameter that receives a single value (or
    // itself) on every edge by that value.
    static i32 frontendTryRemoveTrivial(Frontend* fe, i32 phi, Span<const i32> ops) {
        i32 same = -1;
        for (Int i = 0; i < length(ops); ++i) {
            i32 op = ops[i];
            i32 arg = frontendResolve(fe, op);
            if (arg == same || arg == phi) {
                continue;
            }
            if (same >= 0) {
                return phi;
            }
            same = arg;
        }
        if (same < 0) {
            same = functionNewValue(fe->f, OpUndef, fe->f->values[phi].typ, -1, 0, Span<const i32>({}));
        }
        fe->alias[frontendParamSlot(fe, phi)] = same;
        return same;
    }

    static ValType frontendTypeOf(Frontend* fe, i32 v) {
        return fe->f->values[v].typ;
    }

    static void frontendUnary(Frontend* fe, Op op, ValType t) {
        if (fe->cur < 0) {
            return;
        }
        i32 a = frontendPop(fe);
        frontendPush(fe, frontendEmit(fe, op, t, 0, Span<const i32>({a})));
    }

    static void frontendWrite(Frontend* fe, i32 local, i32 b, i32 v) {
        if (isNil(fe->defs[b])) {
            fe->defs[b] = makeSlice<i32>(length(fe->locals));
            for (Int i = 0, end = length(fe->defs[b]); i < end; ++i) {
                fe->defs[b][i] = -1;
            }
        }
        fe->defs[b][local] = v;
    }

    static i32 frontendZero(Frontend* fe, ValType t) {
        if (t == FuncRef || t == ExternRef) {
            return functionNewValue(fe->f, OpRefNull, t, -1, 0, Span<const i32>({}));
        }
        return frontendConstant(fe, t, 0);
    }

    // An instruction the emitter drops: a no-op or a copy onto itself.
    static bool mInstSilent(MInst* in) {
        switch (in->op) {
            case mNop: {
                return true;
            }
            case mMov: {
                return in->dst == in->src && in->aux == 0;
            }
            case mFMov: {
                return in->dst == in->src;
            }
            case mParMove: {
                const auto moves = mInstMoves(in);
                for (Int i = 0; i < length(moves); ++i) {
                    if (moves[i].dst != moves[i].src) {
                        return false;
                    }
                }
                return true;
            }
            default: {
                return false;
            }
        }
    }

    // target is where a jump to block b goes: blocks that only forward to
    // another one are not emitted.
    static i32 funcEmitterTarget(FuncEmitter* e, i32 b) {
        return e->forward[usize(b)] >= 0 ? e->forward[usize(b)] : b;
    }

    // lands reports whether the end of the block at position at reaches block
    // b without a jump: b follows it, perhaps past blocks that emit nothing.
    static bool funcEmitterLands(FuncEmitter* e, Int at, i32 b) {
        const Int position = e->positions[funcEmitterTarget(e, b)];
        return position > at && position <= e->reach[usize(at)];
    }

    static void funcEmitterBlock(FuncEmitter* e, MBlock* blk, Int at) {
        e->cold = blk->cold;
        Span<MInst> insts = blk->insts.view();
        Int n = length(insts);
        Int i = 0;
        for (; i < n; i++) {
            MInst* in = &insts[i];
            asmMarkPos(e->a, in->pos);
            switch (in->op) {
                case mJmp: {
                    if (!funcEmitterLands(e, at, in->target)) {
                        funcEmitterJumpTo(e, -1, in->target);
                    }
                    break;
                }
                case mJcc: {
                    // Resolve can leave emptied moves before the final jump.
                    Int last = i + 1;
                    while (last < n && mInstSilent(&insts[last])) {
                        ++last;
                    }
                    if (last == n - 1 && insts[last].op == mJmp && funcEmitterLands(e, at, in->target)) {
                        const i32 other = insts[last].target;
                        if (!funcEmitterLands(e, at, other)) {
                            funcEmitterJumpTo(e, convert<i64>(u8(in->cc ^ 1)), other);
                        }
                        i = last;
                        continue;
                    }
                    funcEmitterJumpTo(e, convert<i64>(in->cc), in->target);
                    break;
                }
                default: {
                    funcEmitterInst(e, in);
                    break;
                }
            }
        }
    }

    // check emits the bounds check of an access ending at most imm bytes past
    // the address: the common case compares with size - boundSlack, the slow
    // path the exact limit.
    static void funcEmitterCheck(FuncEmitter* e, MInst* in) {
        Asm* a = e->a;
        if (e->profile != 0) {
            asmAluMI(a, aluAdd, 8, baseAt(ctxReg, e->profile + 24), 1);
        }
        VReg src = in->src;
        i64 end = in->imm;
        if (e->oc->m->memoryTail) {
            // The accessible tail reaches boundSlack past the logical size, so
            // a base within the logical size covers the access; a base past it
            // traps, even where its access would still end inside the tail.
            asmAluRM(a, aluCmp, 8, src, baseAt(ctxReg, offMemBound));
            funcEmitterTrapJcc(e, ccG, trapMemoryOOB);
            return;
        }
        if (e->cold) {
            funcEmitterExactCheck(e, src, end);
            return;
        }
        asmAluRM(a, aluCmp, 8, src, baseAt(ctxReg, offMemBound));
        i32 at = asmJccRel(a, ccG);
        i32 resume = asmPos(a);
        e->late = appendValues(e->late, [=]() mutable -> void {
            asmPatch32(a, at, asmPos(a));
            funcEmitterExactCheck(e, src, end);
            asmPatch32(a, asmJmpRel(a), resume);
        });
    }

    // shadow emits the check of an access against the shadow map: the map's
    // base from the instance, then the map's bytes of the access compared
    // with shadowOK in one compare of the access's width, two of eight bytes
    // for a vector. A compare that fails traps with the access's address.
    static void funcEmitterShadow(FuncEmitter* e, MInst* in) {
        Asm* a = e->a;
        VReg t = mInstTemps(in)[0];
        asmLoad(a, 8, 8, false, t, baseAt(ctxReg, e->oc->lay->shadow));
        const i32 disp = convert<i32>(in->imm);
        Mem m = in->src == noReg ? memAt(t, disp) : memIndexed(t, in->src, disp);
        if (in->size == 16) {
            asmAluMI(a, aluCmp, 8, funcEmitterMem(e, m), shadowOK);
            funcEmitterTrapJccAt(e, ccNE, trapAsan, in->src, disp, 16, in->aux);
            m.disp += 8;
            asmAluMI(a, aluCmp, 8, funcEmitterMem(e, m), shadowOK);
            funcEmitterTrapJccAt(e, ccNE, trapAsan, in->src, disp, 16, in->aux);
            return;
        }
        asmAluMI(a, aluCmp, in->size, funcEmitterMem(e, m), shadowOK);
        funcEmitterTrapJccAt(e, ccNE, trapAsan, in->src, disp, in->size, in->aux);
    }

    static void funcEmitterCvtU64F(FuncEmitter* e, MInst* in) {
        Asm* a = e->a;
        VReg d = in->dst;
        VReg s = in->src;
        VReg t1 = mInstTemps(in)[0];
        VReg t2 = mInstTemps(in)[1];
        asmTest(a, 8, s, s);
        i32 big = asmJcc8(a, ccS);
        asmCvtIF(a, 8, in->dsize, d, s);
        i32 done = asmJmp8(a);
        asmHere8(a, big);
        asmMovRR(a, 8, t1, s);
        asmShiftImm(a, shiftShr, 8, t1, 1);
        asmMovRR(a, 4, t2, s);
        asmAluRI(a, aluAnd, 4, t2, 1);
        asmAluRR(a, aluOr, 8, t1, t2);
        asmCvtIF(a, 8, in->dsize, d, t1);
        asmFalu(a, fAdd, in->dsize, d, d, d);
        asmHere8(a, done);
    }

    // A fallback is already cold: use the exact limit directly, without
    // another fast comparison and an out-of-line slow path returning here.
    static void funcEmitterExactCheck(FuncEmitter* e, VReg src, i64 end) {
        Asm* a = e->a;
        VReg s = RAX;
        if (src == RAX) {
            s = RCX;
        }
        asmPush(a, s);
        asmLoad(a, 8, 8, false, s, baseAt(ctxReg, e->oc->lay->memoryEnd));
        asmAluRI(a, aluSub, 8, s, end);
        asmAluRR(a, aluCmp, 8, src, s);
        asmPop(a, s);
        funcEmitterTrapJcc(e, ccG, trapMemoryOOB);
    }

    static void funcEmitterInst(FuncEmitter* e, MInst* in) {
        Asm* a = e->a;
        switch (in->op) {
            case mNop: {
                break;
            }
            case mCounter: {
                asmAluMI(a, aluAdd, 8, funcEmitterMem(e, in->mem), in->imm);
                break;
            }
            case mCopyForward: {
                asmLea(a, 8, RAX, baseAt(RSI, 1));
                asmAluRR(a, aluCmp, 8, RDI, RAX);
                i32 copy = asmJcc8(a, ccNE);
                asmLoad(a, 1, 4, false, RAX, baseAt(RSI, 0));
                asmBytes(a, {u8(u8(243LL)), u8(u8(170LL))});
                i32 done = asmJmp8(a);
                asmHere8(a, copy);
                asmBytes(a, {u8(u8(243LL)), u8(u8(164LL))});
                asmHere8(a, done);
                break;
            }
            case mMov: {
                if (in->dst == in->src && in->aux == 0) {
                    return;
                }
                u8 size = in->size;
                if (in->aux == 1) {
                    size = 4;
                }
                asmMovRR(a, size, in->dst, in->src);
                break;
            }
            case mFMov: {
                if (in->dst != in->src) {
                    asmFmov(a, in->dst, in->src);
                }
                break;
            }
            case mMovImm: {
                asmMovImm(a, in->size, in->dst, in->imm);
                break;
            }
            case mLoad: {
                asmLoad(a, in->size, in->dsize, in->aux == 1, in->dst, funcEmitterMem(e, in->mem));
                break;
            }
            case mStore: {
                asmStore(a, in->size, in->src, funcEmitterMem(e, in->mem));
                break;
            }
            case mStoreImm: {
                asmStoreImm(a, in->size, funcEmitterMem(e, in->mem), in->imm);
                break;
            }
            case mLea: {
                asmLea(a, in->size, in->dst, funcEmitterMem(e, in->mem));
                break;
            }
            case mAlu: {
                if (isSlot(in->src)) {
                    asmAluRM(a, convert<AluOp>(in->aux), in->size, in->dst, funcEmitterSlotMem(e, slotOf(in->src)));
                } else {
                    asmAluRR(a, convert<AluOp>(in->aux), in->size, in->dst, in->src);
                }
                break;
            }
            case mAluImm: {
                asmAluRI(a, convert<AluOp>(in->aux), in->size, in->dst, in->imm);
                break;
            }
            case mAluMem: {
                asmAluRM(a, convert<AluOp>(in->aux), in->size, in->dst, funcEmitterMem(e, in->mem));
                break;
            }
            case mImul3: {
                asmImul3(a, in->size, in->dst, in->src, in->imm);
                break;
            }
            case mCmp: {
                if (isSlot(in->src2)) {
                    asmAluRM(a, aluCmp, in->size, in->src, funcEmitterSlotMem(e, slotOf(in->src2)));
                } else {
                    asmAluRR(a, aluCmp, in->size, in->src, in->src2);
                }
                break;
            }
            case mCmpImm: {
                asmAluRI(a, aluCmp, in->size, in->src, in->imm);
                break;
            }
            case mCmpMem: {
                asmAluRM(a, aluCmp, in->size, in->src, funcEmitterMem(e, in->mem));
                break;
            }
            case mCmpMemImm: {
                asmAluMI(a, aluCmp, in->size, funcEmitterMem(e, in->mem), in->imm);
                break;
            }
            case mAluMemImm: {
                asmAluMI(a, convert<AluOp>(in->aux), in->size, funcEmitterMem(e, in->mem), in->imm);
                break;
            }
            case mAluMemReg: {
                asmAluMR(a, convert<AluOp>(in->aux), in->size, funcEmitterMem(e, in->mem), in->src);
                break;
            }
            case mTest: {
                asmTest(a, in->size, in->src, in->src2);
                break;
            }
            case mTestImm: {
                if (in->aux == 1 && in->imm >= 0 && in->imm <= 255) {
                    asmOpRR(a, false, Span<const u8>({u8(246LL)}), 0, convert<i64>(in->src), true);
                    asmBytes(a, {u8(convert<u8>(in->imm))});
                } else {
                    asmTestImm(a, in->size, in->src, in->imm);
                }
                break;
            }
            case mSetcc: {
                asmSetcc(a, in->cc, in->dst);
                break;
            }
            case mCmov: {
                asmCmov(a, in->cc, in->size, in->dst, in->src);
                break;
            }
            case mShiftImm: {
                asmShiftImm(a, in->aux, in->size, in->dst, in->imm);
                break;
            }
            case mShiftX: {
                asmShiftX(a, in->aux, in->size, in->dst, in->src, in->src2);
                break;
            }
            case mRotCL: {
                asmShiftCL(a, in->aux, in->size, in->dst);
                break;
            }
            case mUnary: {
                asmUnary(a, in->aux, in->size, in->dst);
                break;
            }
            case mBitCount: {
                if (in->dst != in->src) {
                    asmAluRR(a, aluXor, 4, in->dst, in->dst);
                }
                asmBitCount(a, in->aux, in->size, in->dst, in->src);
                break;
            }
            case mExt: {
                asmExt(a, in->size, in->dsize, in->aux == 1, in->dst, in->src);
                break;
            }
            case mDiv: {
                asmDiv(a, in->aux == 1, in->size, in->src);
                break;
            }
            case mSignExtAx: {
                if (in->size == 8) {
                    asmBytes(a, {u8(u8(72LL))});
                }
                asmBytes(a, {u8(u8(153LL))});
                break;
            }
            case mZeroDx: {
                asmAluRR(a, aluXor, 4, RDX, RDX);
                break;
            }
            case mFLoad: {
                asmFload(a, in->size, in->dst, funcEmitterMem(e, in->mem));
                break;
            }
            case mFStore: {
                asmFstore(a, in->size, in->src, funcEmitterMem(e, in->mem));
                break;
            }
            case mFConst: {
                if (in->imm == 0) {
                    asmFxor(a, in->dst);
                } else {
                    asmFload(a, in->size, in->dst, objCodeFloatConst(e->oc, convert<u64>(in->imm), in->size));
                }
                break;
            }
            case mFAlu: {
                if (isSlot(in->src2)) {
                    asmFaluM(a, convert<FAluOp>(in->aux), in->size, in->dst, in->src, funcEmitterSlotMem(e, slotOf(in->src2)));
                } else {
                    asmFalu(a, convert<FAluOp>(in->aux), in->size, in->dst, in->src, in->src2);
                }
                break;
            }
            case mFSelect: {
                VReg mask = mInstTemps(in)[0];
                Array<u8, 6> predicates = Array<u8, 6>{{u8(0LL), u8(4LL), u8(17LL), u8(30LL), u8(18LL), u8(29LL)}};
                asmFcmpMask(a, in->dsize, mask, mInstUses(in)[0], mInstUses(in)[1], predicates[in->aux]);
                if (in->dsize == 4 && in->size == 8) {
                    asmVexRR(a, pp66, 1, false, 112, regNum(mask), -1, regNum(mask));
                    asmBytes(a, {u8(u8(0LL))});
                }
                asmFblend(a, in->size, in->dst, in->src2, in->src, mask);
                break;
            }
            case mFAluMem: {
                asmFaluM(a, convert<FAluOp>(in->aux), in->size, in->dst, in->src, funcEmitterMem(e, in->mem));
                break;
            }
            case mFUnary: {
                if (in->aux == fSqrt) {
                    asmFsqrt(a, in->size, in->dst, in->src);
                } else {
                    asmFround(a, in->size, in->dst, in->src, in->imm);
                }
                break;
            }
            case mFCmp: {
                asmFcmp(a, in->size, in->src, in->src2);
                break;
            }
            case mCvtIF: {
                asmCvtIF(a, in->size, in->dsize, in->dst, in->src);
                break;
            }
            case mCvtFI: {
                asmCvtFI(a, in->size, in->dsize, in->dst, in->src);
                break;
            }
            case mCvtFF: {
                asmCvtFF(a, in->size, in->dst, in->src);
                break;
            }
            case mMovGX: {
                asmMovGX(a, in->size, in->dst, in->src);
                break;
            }
            case mMovXG: {
                asmMovXG(a, in->size, in->dst, in->src);
                break;
            }
            case mSwitch: {
                funcEmitterSwitchTable(e, in);
                break;
            }
            case mCall: {
                switch (in->aux) {
                    case calleeJump: {
                        e->oc->jumpCalls = appendValues(e->oc->jumpCalls, CallFixup{asmCallRel(a), convert<i32>(in->imm)});
                        break;
                    }
                    case calleeGo: {
                        funcEmitterGoCall(e, convert<i32>(in->imm));
                        break;
                    }
                    case calleeStr: {
                        asmCallMem(a, Amem{-1, -1, {}, {}, convert<i32>(in->imm), labelGot});
                        break;
                    }
                    default: {
                        objCodeCallFunc(e->oc, convert<i64>(in->imm));
                        break;
                    }
                }
                break;
            }
            case mCallInd: {
                asmCallReg(a, in->src);
                break;
            }
            case mRet: {
                if (e->frame > 0) {
                    asmAluRI(a, aluAdd, 8, RSP, convert<i64>(e->frame));
                }
                Int i = length(e->pushes) - 1;
                for (; i >= 0; i--) {
                    asmPop(a, e->pushes[i]);
                }
                if (e->mf->returnsTwice) {
                    asmLea(a, 8, RSP, rspAt(8));
                }
                if (e->framed) {
                    asmPop(a, RBP);
                }
                asmRet(a);
                asmSetSP(a, e->body);
                break;
            }
            case mTrap: {
                funcEmitterTrapJmp(e, in->imm);
                break;
            }
            case mTrapIf: {
                funcEmitterTrapJcc(e, in->cc, in->imm);
                break;
            }
            case mCheck: {
                funcEmitterCheck(e, in);
                break;
            }
            case mShadow: {
                funcEmitterShadow(e, in);
                break;
            }
            case mTrapAt: {
                funcEmitterTrapJccAt(e, in->cc, in->imm, in->src, in->mem.disp, in->size, in->aux);
                break;
            }
            case mParMove: {
                funcEmitterParMove(e, mInstMoves(in));
                break;
            }
            case mStackArg: {
                // imm is a slot, or with aux the byte offset of a Go argument
                // of size bytes.
                i32 at = convert<i32>(in->aux ? in->imm : 8 * in->imm);
                u8 size = in->aux ? in->size : 8;
                if (vRegIsXMM(in->src)) {
                    asmFstore(a, size, in->src, rspAt(at));
                } else {
                    asmStore(a, size, in->src, rspAt(at));
                }
                break;
            }
            case mLoadArg: {
                Amem m = rspAt(e->incoming + convert<i32>(8 * in->imm));
                if (vRegIsXMM(in->dst)) {
                    asmFload(a, 8, in->dst, m);
                } else {
                    asmLoad(a, 8, 8, false, in->dst, m);
                }
                break;
            }
            case mStoreRet: {
                Amem m = rspAt(e->incoming + convert<i32>(8 * (convert<i64>(e->mf->incoming) + in->imm)));
                if (vRegIsXMM(in->src)) {
                    asmFstore(a, 8, in->src, m);
                } else {
                    asmStore(a, 8, in->src, m);
                }
                break;
            }
            case mDivOvf: {
                asmAluRI(a, aluCmp, in->size, in->src2, -1);
                i32 j = asmJcc8(a, ccNE);
                asmAluRI(a, aluCmp, in->size, in->src, 1);
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

    static void funcEmitterJumpTo(FuncEmitter* e, Int cc, i32 block) {
        const Int k = length(e->jumps);
        const bool small = k < length(e->shorts) && e->shorts[k] != 0;
        i32 at = {};
        if (cc < 0) {
            at = small ? asmJmp8(e->a) : asmJmpRel(e->a);
        } else {
            at = small ? asmJcc8(e->a, convert<u8>(cc)) : asmJccRel(e->a, convert<u8>(cc));
        }
        e->jumps = appendValues(e->jumps, JumpFixup{at, funcEmitterTarget(e, block), small});
    }

    static Amem funcEmitterMem(FuncEmitter* e, Mem m) {
        Amem am = memOf(m);
        if (m.label >= 0) {
            am.label = m.label;
            am.labelKind = labelModule;
        }
        return am;
    }

    static void funcEmitterMinMax(FuncEmitter* e, MInst* in) {
        Asm* a = e->a;
        u8 size = in->size;
        VReg d = in->dst;
        VReg x = in->src;
        VReg y = in->src2;
        asmFcmp(a, size, x, y);
        i32 nan = asmJcc8(a, ccP);
        i32 normal = asmJcc8(a, ccNE);
        if (in->aux == 1) {
            asmFalu(a, fAnd, size, d, x, y);
        } else {
            asmFalu(a, fOr, size, d, x, y);
        }
        i32 done1 = asmJmp8(a);
        asmHere8(a, normal);
        if (in->aux == 1) {
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
        Asm* a = e->a;
        if (isSlot(dst) && slotOf(dst) >= constantSlot) {
            compilerError(String("internal error: a move into a constant slot"));
        }
        if (isSlot(dst) && isSlot(src)) {
            asmPushMem(a, funcEmitterSlotMem(e, slotOf(src)));
            asmPopMem(a, funcEmitterSlotMem(e, slotOf(dst)));
        } else if (isSlot(dst)) {
            if (vRegIsXMM(src)) {
                asmFstore(a, 8, src, funcEmitterSlotMem(e, slotOf(dst)));
            } else {
                asmStore(a, 8, src, funcEmitterSlotMem(e, slotOf(dst)));
            }
        } else if (isSlot(src)) {
            if (vRegIsXMM(dst)) {
                asmFload(a, 8, dst, funcEmitterSlotMem(e, slotOf(src)));
            } else {
                asmLoad(a, 8, 8, false, dst, funcEmitterSlotMem(e, slotOf(src)));
            }
        } else if (vRegIsXMM(dst)) {
            asmFmov(a, dst, src);
        } else {
            asmMovRR(a, 8, dst, src);
        }
    }

    // parMove performs moves between registers and stack slots as if all at
    // once.
    static void funcEmitterParMove(FuncEmitter* e, Span<Move> moves) {
        Slice<Move> pending = makeSlice<Move>(0, length(moves));
        for (Int i = 0; i < length(moves); ++i) {
            Move m = moves[i];
            if (m.dst != m.src) {
                pending = appendValues(pending, m);
            }
        }
        while (length(pending) > Int(0LL)) {
            bool progress = false;
            Int i = 0;
            for (; i < length(pending); i++) {
                Move m = pending[i];
                bool blocked = false;
                auto range708 = pending;
                for (Int index709 = 0, rangeEnd710 = length(range708); index709 < rangeEnd710; ++index709) {
                    Int j = index709;
                    Move o = range708[index709];
                    if (j != i && o.src == m.dst) {
                        blocked = true;
                        break;
                    }
                }
                if (blocked) {
                    continue;
                }
                funcEmitterMove(e, m.dst, m.src);
                pending = appendSlice(subslice(pending, 0, i), subslice(pending, i + 1, length(pending)));
                i--;
                progress = true;
            }
            if (progress) {
                continue;
            }
            // A swap performs one move of a cycle and renames the sources of
            // the others; the move completing a cycle of two is done with it.
            Move m = pending[0];
            funcEmitterSwap(e, m.dst, m.src);
            Slice<Move> rest = makeSlice<Move>(0, length(pending) - 1);
            for (Int k = 1, end = length(pending); k < end; ++k) {
                Move o = pending[k];
                if (o.src == m.dst) {
                    o.src = m.src;
                } else if (o.src == m.src) {
                    o.src = m.dst;
                }
                if (o.src != o.dst) {
                    rest = appendValues(rest, o);
                }
            }
            pending = rest;
        }
    }

    static Amem funcEmitterSlotMem(FuncEmitter* e, i32 slot) {
        if (slot >= constantSlot) {
            return objCodeFloatConst(e->oc, e->al->constantSlots[usize(slot - constantSlot)], 8);
        }
        return rspAt(convert<i32>(e->mf->stackArgs) + 8 * slot);
    }

    // goCall calls the function of extern symbol index as Go calls one, with
    // the instance in RAX. Go keeps none of the registers the code uses: the
    // instance comes back from g, where the entry thunk put it, the memory
    // base from the instance. While the instance's interrupt word is set,
    // the interrupt stub runs on the way out, as Go's runtime would inject a
    // call of it there.
    static void funcEmitterGoCall(FuncEmitter* e, i32 index) {
        Asm* a = e->a;
        asmMovRR(a, 8, RAX, ctxReg);
        asmCallMem(a, Amem{-1, -1, {}, {}, index, labelGot});
        asmLoad(a, 8, 8, false, ctxReg, baseAt(R14, gInstance));
        asmLoad(a, 8, 8, false, memReg, baseAt(ctxReg, offMem));
        asmAluMI(a, aluCmp, 4, baseAt(ctxReg, offInterrupt), 0);
        i32 at = asmJccRel(a, ccNE);
        i32 resume = asmPos(a);
        e->late = appendValues(e->late, [=]() mutable -> void {
            asmPatch32(a, at, asmPos(a));
            asmCallMem(a, objCodeGotMem(e->oc, symInterruptCall));
            asmPatch32(a, asmJmpRel(a), resume);
        });
    }

    static void funcEmitterSwap(FuncEmitter* e, VReg x, VReg y) {
        Asm* a = e->a;
        if ((isSlot(x) && slotOf(x) >= constantSlot) || (isSlot(y) && slotOf(y) >= constantSlot)) {
            compilerError(String("internal error: a swap with a constant slot"));
        }
        if (isSlot(x)) {
            auto temporary716 = y;
            auto temporary717 = x;
            x = temporary716;
            y = temporary717;
        }
        if (isSlot(x)) {
            Amem mx = funcEmitterSlotMem(e, slotOf(x));
            Amem my = funcEmitterSlotMem(e, slotOf(y));
            asmPushMem(a, mx);
            my.disp += 8;
            asmPushMem(a, my);
            mx.disp += 8;
            asmPopMem(a, mx);
            my.disp -= 8;
            asmPopMem(a, my);
        } else if (isSlot(y)) {
            Amem m = funcEmitterSlotMem(e, slotOf(y));
            asmPushMem(a, m);
            Amem shifted = m;
            shifted.disp += 8;
            if (vRegIsXMM(x)) {
                asmFstore(a, 8, x, shifted);
                asmFload(a, 8, x, rspAt(0));
                asmLea(a, 8, RSP, rspAt(8));
            } else {
                asmStore(a, 8, x, shifted);
                asmPop(a, x);
            }
        } else if (vRegIsXMM(x)) {
            asmFalu(a, fXor, 8, x, x, y);
            asmFalu(a, fXor, 8, y, y, x);
            asmFalu(a, fXor, 8, x, x, y);
        } else {
            asmXchg(a, x, y);
        }
    }

    static void funcEmitterSwitchTable(FuncEmitter* e, MInst* in) {
        Asm* a = e->a;
        Int n = length(mInstTable(in)) - 1;
        i32 def = mInstTable(in)[n];
        if (n == 0) {
            funcEmitterJumpTo(e, -1, def);
            return;
        }
        if (in->aux == 0 && n >= 4 && mInstTable(in)[0] != def) {
            asmTest(a, 4, in->src, in->src);
            funcEmitterJumpTo(e, ccE, mInstTable(in)[0]);
        }
        VReg t0 = mInstTemps(in)[0];
        if (in->aux != 2) {
            asmAluRI(a, aluCmp, 4, in->src, convert<i64>(n));
            funcEmitterJumpTo(e, ccAE, def);
        }
        asmRex(a, true, convert<i64>(t0), -1, -1, false);
        asmBytes(a, {u8(u8(141LL)), u8(u8(shiftLeft(lo3(convert<i64>(t0)), 3) | 5))});
        i32 leaAt = asmPos(a);
        asmU32(a, 0);
        asmOpRM(a, false, Span<const u8>({u8(255LL)}), 4, Amem{t0, in->src, u8(8LL), {}, -1, {}}, 0, false);
        e->tables = appendValues(e->tables, TableFixup{leaAt, leaAt + 4, mInstTable(in)});
    }

    static void funcEmitterTrapJcc(FuncEmitter* e, u8 cc, i64 code) {
        const Int k = length(e->traps);
        const bool small = k < length(e->trapShorts) && e->trapShorts[k] != 0;
        TrapFixup t{small ? asmJcc8(e->a, cc) : asmJccRel(e->a, cc), code, e->a->sp, small};
        t.pos = e->a->lastPos;
        e->traps = appendValues(e->traps, t);
    }

    static void funcEmitterTrapJmp(FuncEmitter* e, i64 code) {
        const Int k = length(e->traps);
        const bool small = k < length(e->trapShorts) && e->trapShorts[k] != 0;
        TrapFixup t{small ? asmJmp8(e->a) : asmJmpRel(e->a), code, e->a->sp, small};
        t.pos = e->a->lastPos;
        e->traps = appendValues(e->traps, t);
    }

    // trapJccAt is trapJcc of an asan trap, which reports the access at
    // reg + disp, or disp alone when reg is noReg, of width bytes and kind.
    static void funcEmitterTrapJccAt(FuncEmitter* e, u8 cc, i64 code, VReg reg, i32 disp, u8 width, u8 kind) {
        const Int k = length(e->traps);
        const bool small = k < length(e->trapShorts) && e->trapShorts[k] != 0;
        TrapFixup t{small ? asmJcc8(e->a, cc) : asmJccRel(e->a, cc), code, e->a->sp, small};
        t.reg = reg;
        t.disp = disp;
        t.width = width;
        t.kind = kind;
        t.pos = e->a->lastPos;
        e->traps = appendValues(e->traps, t);
    }

    static void funcEmitterTruncF(FuncEmitter* e, MInst* in) {
        Asm* a = e->a;
        u8 isize = in->size;
        u8 fsize = in->dsize;
        VReg dst = in->dst;
        VReg src = in->src;
        bool valueRanges = u8(in->aux & truncUnsigned) != 0;
        bool sat = u8(in->aux & truncSat) != 0;
        auto temporary726 = truncBounds(isize, fsize);
        u64 high = temporary726.v1;
        if (!valueRanges) {
            asmCvtFI(a, isize, fsize, dst, src);
            asmAluRI(a, aluCmp, isize, dst, 1);
            i32 at = asmJccRel(a, ccO);
            i32 resume = asmPos(a);
            e->late = appendValues(e->late, [=]() mutable -> void {
                asmPatch32(a, at, asmPos(a));
                asmFcmp(a, fsize, src, src);
                if (sat) {
                    i32 nan = asmJcc8(a, ccP);
                    asmFcmpM(a, fsize, src, objCodeFloatConst(e->oc, 0, fsize));
                    asmPatch32(a, asmJccRel(a, ccBE), resume);
                    asmMovImm(a, isize, dst, maxSigned(isize));
                    asmPatch32(a, asmJmpRel(a), resume);
                    asmHere8(a, nan);
                    asmMovImm(a, 4, dst, 0);
                    asmPatch32(a, asmJmpRel(a), resume);
                    return;
                }
                funcEmitterTrapJcc(e, ccP, trapConversion);
                auto temporary727 = truncBounds(isize, fsize);
                u64 low = temporary727.v0;
                bool strict = temporary727.v2;
                asmFcmpM(a, fsize, src, objCodeFloatConst(e->oc, low, fsize));
                if (strict) {
                    funcEmitterTrapJcc(e, ccBE, trapIntOverflow);
                } else {
                    funcEmitterTrapJcc(e, ccB, trapIntOverflow);
                }
                asmFcmpM(a, fsize, src, objCodeFloatConst(e->oc, high, fsize));
                funcEmitterTrapJcc(e, ccAE, trapIntOverflow);
                asmPatch32(a, asmJmpRel(a), resume);
            });
            return;
        }
        if (isize == 4) {
            VReg tg = mInstTemps(in)[0];
            asmCvtFI(a, 8, fsize, dst, src);
            asmMovRR(a, 8, tg, dst);
            asmShiftImm(a, shiftShr, 8, tg, 32);
            i32 at = asmJccRel(a, ccNE);
            i32 resume = asmPos(a);
            e->late = appendValues(e->late, [=]() mutable -> void {
                asmPatch32(a, at, asmPos(a));
                asmFcmp(a, fsize, src, src);
                if (sat) {
                    i32 zero = asmJcc8(a, ccP);
                    asmFcmpM(a, fsize, src, objCodeFloatConst(e->oc, 0, fsize));
                    i32 neg = asmJcc8(a, ccB);
                    asmMovImm(a, 4, dst, i64(4294967295LL));
                    asmPatch32(a, asmJmpRel(a), resume);
                    asmHere8(a, zero);
                    asmHere8(a, neg);
                    asmMovImm(a, 4, dst, 0);
                    asmPatch32(a, asmJmpRel(a), resume);
                    return;
                }
                funcEmitterTrapJcc(e, ccP, trapConversion);
                funcEmitterTrapJmp(e, trapIntOverflow);
            });
            return;
        }
        VReg tx = mInstTemps(in)[0];
        VReg tg = mInstTemps(in)[1];
        asmFcmpM(a, fsize, src, objCodeFloatConst(e->oc, high, fsize));
        i32 big = asmJcc8(a, ccAE);
        asmCvtFI(a, 8, fsize, dst, src);
        asmTest(a, 8, dst, dst);
        i32 slowLow = asmJccRel(a, ccS);
        i32 done = asmJmp8(a);
        asmHere8(a, big);
        asmFaluM(a, fSub, fsize, tx, src, objCodeFloatConst(e->oc, high, fsize));
        asmCvtFI(a, 8, fsize, dst, tx);
        asmTest(a, 8, dst, dst);
        i32 slowHigh = asmJccRel(a, ccS);
        asmMovImm(a, 8, tg, i64(-9223372036854775807LL - 1));
        asmAluRR(a, aluXor, 8, dst, tg);
        asmHere8(a, done);
        i32 resume = asmPos(a);
        e->late = appendValues(e->late, [=]() mutable -> void {
            asmPatch32(a, slowLow, asmPos(a));
            if (sat) {
                asmMovImm(a, 4, dst, 0);
            } else {
                asmFcmp(a, fsize, src, src);
                funcEmitterTrapJcc(e, ccP, trapConversion);
                funcEmitterTrapJmp(e, trapIntOverflow);
            }
            asmPatch32(a, asmJmpRel(a), resume);
            asmPatch32(a, slowHigh, asmPos(a));
            if (sat) {
                asmMovImm(a, 8, dst, -1);
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
        Int n = length(it->ranges);
        if (n > 0) {
            Lrange* last = &it->ranges[n - 1];
            if (to >= last->from) {
                if (from < last->from) {
                    last->from = from;
                }
                if (to > last->to) {
                    last->to = to;
                }
                return;
            }
        }
        it->ranges.pushBack(Lrange{from, to});
    }

    // covered is the first position from p on that it covers.
    static i32 intervalCovered(Interval* it, i32 p) {
        Int k = sortSearch(length(it->ranges), [&](Int i) mutable -> bool {
            return it->ranges[i].to > p;
        });
        if (k == length(it->ranges) || it->ranges[k].from <= p) {
            return p;
        }
        return it->ranges[k].from;
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
        Int k = sortSearch(length(it->ranges), [&](Int i) mutable -> bool {
            return it->ranges[i].to > p;
        });
        return k < length(it->ranges) && it->ranges[k].from <= p;
    }

    // coversAt is covers for the scan position: p never decreases between
    // calls.
    static bool intervalCoversAt(Interval* it, i32 p) {
        while (it->cursor < length(it->ranges) && it->ranges[it->cursor].to <= p) {
            it->cursor++;
        }
        return it->cursor < length(it->ranges) && it->ranges[it->cursor].from <= p;
    }

    // cutAt shortens the earliest range, which covers a definition at p, to
    // start there; it reports false when no range covers p.
    template <typename T>
    static bool intervalCutAt(T* it, i32 p) {
        Int n = length(it->ranges);
        if (n > 0 && it->ranges[n - 1].from <= p && it->ranges[n - 1].to > p) {
            it->ranges[n - 1].from = p;
            return true;
        }
        return false;
    }

    static i32 intervalEnd(Interval* it) {
        return it->end;
    }

    static bool intervalEndsBefore(Interval* it, i32 p) {
        return intervalEnd(it) <= p;
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
        Int i = 0;
        Int j = length(r) - 1;
        for (; i < j; [&]() {
            auto temporary732 = i + 1;
            auto temporary733 = j - 1;
            i = temporary732;
            j = temporary733;
        }()) {
            auto temporary734 = r[j];
            auto temporary735 = r[i];
            r[i] = temporary734;
            r[j] = temporary735;
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
        const usize memCount = it->memUses.size();
        for (usize i = 0; i < memCount / 2; ++i) {
            const i32 use = it->memUses[i];
            it->memUses[i] = it->memUses[memCount - 1 - i];
            it->memUses[memCount - 1 - i] = use;
        }
    }

    // intersection returns the first position both intervals cover, or maxPos;
    // it starts from both cursors.
    static i32 intervalIntersection(Interval* it, Interval* o) {
        if (it->cursor > length(it->ranges)) {
            it->cursor = length(it->ranges);
        }
        Int i = it->cursor;
        Int j = o->cursor;
        while (i < length(it->ranges) && j < length(o->ranges)) {
            Lrange a = it->ranges[i];
            Lrange b = o->ranges[j];
            if (a.to <= b.from) {
                i++;
                continue;
            }
            if (b.to <= a.from) {
                j++;
                continue;
            }
            if (a.from > b.from) {
                return a.from;
            }
            return b.from;
        }
        return maxPos;
    }

    static i32 intervalNextUse(Interval* it, i32 p) {
        Int k = sortSearch(length(it->uses), [&](Int i) mutable -> bool {
            return it->uses[i] >= p;
        });
        if (k < length(it->uses)) {
            return it->uses[k];
        }
        return maxPos;
    }

    // prevUse returns the last use before p, or -1.
    static i32 intervalPrevUse(Interval* it, i32 p) {
        Int k = sortSearch(length(it->uses), [&](Int i) mutable -> bool {
            return it->uses[i] >= p;
        });
        if (k > 0) {
            return it->uses[k - 1];
        }
        return -1;
    }

    // split cuts the interval at p: the returned child covers p and later.
    static Interval* intervalSplit(Interval* it, i32 p) {
        Interval* child = newInterval(it->vreg, noReg, it->classValue, it->hint);
        Int k = 0;
        while (k < length(it->ranges) && it->ranges[k].to <= p) {
            k++;
        }
        if (k < length(it->ranges) && it->ranges[k].from < p) {
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
        if (it->cursor > length(it->ranges)) {
            it->cursor = length(it->ranges);
        }
        Int u = sortSearch(length(it->uses), [&](Int i) mutable -> bool {
            return it->uses[i] >= p;
        });
        child->uses.assign(subslice(it->uses.view(), u, length(it->uses)));
        it->uses.resize(usize(u));
        Int mu = sortSearch(length(it->memUses), [&](Int i) mutable -> bool {
            return it->memUses[i] >= p;
        });
        child->memUses.assign(subslice(it->memUses.view(), mu, length(it->memUses)));
        it->memUses.resize(usize(mu));
        Interval* root = it;
        if (!isNil(it->parent)) {
            root = it->parent;
        }
        child->parent = root;
        Int idx = sortSearch(length(root->children), [&](Int i) mutable -> bool {
            return intervalStart(root->children[i]) > p;
        });
        root->children.insert(usize(idx), child);
        return child;
    }

    static i32 intervalStart(Interval* it) {
        return it->ranges[0].from;
    }

    // address returns the memory operand of an access of width bytes at addr
    // plus the static offset, emitting its bounds check.
    static Mem lowererAddress(Lowerer* l, i32 addr, i64 offset, Int width, bool proven, bool store) {
        if (l->m->asan) {
            return lowererAsanAddress(l, addr, offset, width, store);
        }
        i64 end = offset + convert<i64>(width);
        bool checked = proven;
        if (length(l->m->mems) > 0 && !l->m->mems[0].imported) {
            if (isNil(l->addresses)) {
                l->addresses = activePool->make<ValueRanges>(ValueRanges{l->f, makeSlice<ValueRange>(length(l->f->values)), makeSlice<bool>(length(l->f->values))});
            }
            const ValueRange range = valueRangesOf(l->addresses, addr, 0);
            checked = checked || (range.lo >= 0 && range.hi + end <= i64(l->m->mems[0].limits.min * 65536 + l->m->memoryTail));
        }
        const bool provenAccess = checked;
        auto temporary740 = lookupMap(l->checked, addr);
        i64 prior = temporary740.v0;
        bool had = temporary740.v1;
        checked = checked || (had && prior >= end);
        if (l->m->memoryTail && end <= boundSlack) {
            // An address base + c, with c within boundSlack either way, is
            // covered by the check of the base: the memory has boundSlack
            // accessible bytes before it and past its size, so with the
            // base within the size the access begins at most boundSlack
            // before the base and ends at most boundSlack past it. The base
            // is checked instead, once for every address derived from it in
            // its scope, and the access is addressed from the base in 64
            // bits, so a sum used for addresses alone is never computed
            // (markDeferred), and a sum that wrapped around 2^32 is
            // addressed as if it had not: it lands in the free memory on
            // either side (moduleLowFree), where the base's trap, or the
            // slack, is right. A sum with c >= 0 is itself within the base's
            // check; one with c < 0 may have wrapped, so its own register
            // is never taken as checked. Unless proven, the access is
            // addressed so even where the sum is known: checkedValues takes
            // it as reaching c + end past the base.
            Value* av = &l->f->values[addr];
            if (av->op == OpAdd) {
                Span<i32> args = functionArgs(l->f, addr);
                i32 base = args[0];
                i32 k = args[1];
                if (lowererIsConst(l, base) && !lowererIsConst(l, k)) {
                    base = args[1];
                    k = args[0];
                }
                if (lowererIsConst(l, k) && !lowererIsConst(l, base)) {
                    i64 c = convert<i64>(convert<i32>(convert<u32>(lowererConstBits(l, k))));
                    i64 top = c + end;
                    auto temporary741 = lookupMap(l->checked, base);
                    const bool baseChecked = temporary741.v1 && temporary741.v0 >= top;
                    // A proven access at a sum of a bounded base and a
                    // constant is the base's: the sum does not wrap.
                    const bool boundedBase = proven && l->bounded[usize(base)] == l->block && l->block >= 0;
                    if (c >= -boundSlack && top <= boundSlack && (baseChecked || !provenAccess || boundedBase)) {
                        VReg rb = lowererUse(l, base);
                        // The sum reaches as far past itself as the base
                        // does past the base, less c: the whole slack
                        // after a check of the base, only as far as a fact
                        // of the base reaches without one.
                        i64 reach = baseChecked ? temporary741.v0 : 0;
                        if (!baseChecked && !boundedBase) {
                            l->checkUndo = appendValues(l->checkUndo, CheckUndo{base, temporary741.v0, temporary741.v1});
                            setMap(l->checked, base, i64(boundSlack));
                            MInst in = inst(mCheck);
                            in.src = rb;
                            in.imm = maximum(top, convert<i64>(width));
                            in.addressValue = base;
                            lowererEmit(l, in);
                            reach = boundSlack;
                        }
                        if (c >= 0 && reach - c > 0 && (!had || prior < reach - c)) {
                            l->checkUndo = appendValues(l->checkUndo, CheckUndo{addr, prior, had});
                            setMap(l->checked, addr, reach - c);
                        }
                        return memIndexed(memReg, rb, convert<i32>(offset + c));
                    }
                }
                // So is a sum of a base and an index whose range keeps it
                // within boundSlack, a byte-indexed table's entry, say: the
                // sum, which does not wrap from a base within the size, is
                // addressed itself.
                if (!checked && !lowererIsConst(l, args[0]) && !lowererIsConst(l, args[1])) {
                    if (isNil(l->addresses)) {
                        l->addresses = activePool->make<ValueRanges>(ValueRanges{l->f, makeSlice<ValueRange>(length(l->f->values)), makeSlice<bool>(length(l->f->values))});
                    }
                    for (Int i = 0; i < 2; ++i) {
                        const i32 index = args[i];
                        const i32 base2 = args[1 - i];
                        const ValueRange range = valueRangesOf(l->addresses, index, 0);
                        const i64 top = range.hi + end;
                        // A load the sum folds has no register of its own.
                        if (range.lo < 0 || top > boundSlack || l->foldable[usize(base2)]) {
                            continue;
                        }
                        auto known = lookupMap(l->checked, base2);
                        i64 reach = known.v1 ? known.v0 : 0;
                        if (!known.v1 || known.v0 < top) {
                            l->checkUndo = appendValues(l->checkUndo, CheckUndo{base2, known.v0, known.v1});
                            setMap(l->checked, base2, i64(boundSlack));
                            MInst in = inst(mCheck);
                            in.src = lowererUse(l, base2);
                            in.imm = maximum(top, convert<i64>(width));
                            in.addressValue = base2;
                            lowererEmit(l, in);
                            reach = boundSlack;
                        }
                        if (!had || prior < reach - range.hi) {
                            l->checkUndo = appendValues(l->checkUndo, CheckUndo{addr, prior, had});
                            setMap(l->checked, addr, reach - range.hi);
                        }
                        return memIndexed(memReg, lowererUse(l, addr), convert<i32>(offset));
                    }
                }
            }
        }
        if (checked && lowererIsConst(l, addr)) {
            // A constant address within bounds, a global's, is the
            // displacement.
            const i64 at = convert<i64>(convert<u32>(lowererConstBits(l, addr))) + offset;
            if (at < 2147483647) {
                return memAt(memReg, convert<i32>(at));
            }
        }
        if (checked && offset < 2147483647) {
            // A sum of a base and a constant within bounds, used only for
            // addresses, is addressed from the base, the constant in the
            // displacement, when the base's range shows the sum does not
            // wrap: a table at a constant address indexed by a byte, say.
            Value* av = &l->f->values[addr];
            if (av->op == OpAdd && l->deferred[addr] && !isNil(l->addresses)) {
                Span<i32> args = functionArgs(l->f, addr);
                const i32 k = lowererIsConst(l, args[1]) ? 1 : lowererIsConst(l, args[0]) ? 0 : -1;
                if (k >= 0 && !lowererIsConst(l, args[1 - k])) {
                    const i32 base = args[1 - k];
                    const i64 c = convert<i64>(convert<u32>(lowererConstBits(l, args[k])));
                    const ValueRange range = valueRangesOf(l->addresses, base, 0);
                    if (range.lo >= 0 && range.hi + c < 2147483647 && offset + c < 2147483647) {
                        return memIndexed(memReg, lowererUse(l, base), convert<i32>(offset + c));
                    }
                }
            }
            return memIndexed(memReg, lowererUse(l, addr), convert<i32>(offset));
        }
        VReg ra = lowererUse(l, addr);
        // With the accessible tail, the check of a base within the logical size
        // admits every access ending at most boundSlack past that base.
        const i64 admitted = l->m->memoryTail && end <= boundSlack ? i64(boundSlack) : end;
        if (!had || admitted > prior) {
            l->checkUndo = appendValues(l->checkUndo, CheckUndo{addr, prior, had});
            setMap(l->checked, addr, admitted);
        }
        if (end <= boundSlack) {
            MInst in = inst(mCheck);
            in.src = ra;
            in.imm = end;
            in.addressValue = addr;
            lowererEmit(l, in);
            return memIndexed(memReg, ra, convert<i32>(offset));
        }
        VReg t = mFuncNewReg(l->mf, classGPR);
        MInst mi = inst(mMovImm);
        mi.dst = t;
        mi.imm = end;
        mi.size = 8;
        lowererEmit(l, mi);
        MInst add = inst(mAlu);
        add.dst = t;
        add.src = ra;
        add.size = 8;
        add.aux = 0;
        lowererEmit(l, add);
        MInst c = inst(mCmpMem);
        c.src = t;
        c.mem = memAt(ctxReg, l->lay->memoryEnd);
        c.size = 8;
        lowererEmit(l, c);
        lowererTrapIf(l, ccA, trapMemoryOOB);
        if (offset < 2147483647) {
            return memIndexed(memReg, ra, convert<i32>(offset));
        }
        VReg p = mFuncNewReg(l->mf, classGPR);
        mi = inst(mMovImm);
        mi.dst = p;
        mi.imm = offset;
        mi.size = 8;
        lowererEmit(l, mi);
        add = inst(mAlu);
        add.dst = p;
        add.src = ra;
        add.size = 8;
        add.aux = 0;
        lowererEmit(l, add);
        return memIndexed(memReg, p, 0);
    }

    // asanAddress is address in an asan object: every access checks the
    // shadow map of its own bytes (mShadow), none is covered by another's
    // check, and the memory is addressed as the checked bytes are. The map
    // has a byte for every 32-bit address and boundSlack past them, so an
    // address with a static offset within boundSlack is checked and
    // addressed as it is; a larger offset is summed first and the sum
    // checked against the memory's size, so neither the map nor the memory
    // is indexed past what the loader mapped.
    static Mem lowererAsanAddress(Lowerer* l, i32 addr, i64 offset, Int width, bool store) {
        const u8 kind = store ? asanStore : asanLoad;
        if (lowererIsConst(l, addr)) {
            const i64 at = convert<i64>(convert<u32>(lowererConstBits(l, addr))) + offset;
            if (at + convert<i64>(width) <= i64(4294967296LL) && at < 2147483647) {
                MInst in = inst(mShadow);
                in.imm = at;
                in.size = convert<u8>(width);
                in.aux = kind;
                mInstExtra(&in)->temps.assign(Span<const VReg>({mFuncNewReg(l->mf, classGPR)}));
                lowererEmit(l, in);
                return memAt(memReg, convert<i32>(at));
            }
        }
        VReg ra = lowererUse(l, addr);
        if (offset + convert<i64>(width) > boundSlack) {
            VReg t = lowererSum(l, ra, lowererConstReg(l, offset, 8));
            lowererCheckEnd(l, t, lowererConstReg(l, convert<i64>(width), 8), ccA);
            ra = t;
            offset = 0;
        }
        MInst in = inst(mShadow);
        in.src = ra;
        in.imm = offset;
        in.size = convert<u8>(width);
        in.aux = kind;
        mInstExtra(&in)->temps.assign(Span<const VReg>({mFuncNewReg(l->mf, classGPR)}));
        lowererEmit(l, in);
        return memIndexed(memReg, ra, convert<i32>(offset));
    }

    // shadowAddress is the host address of the shadow map's byte of a: the
    // map's base, which the instance keeps, plus the 32-bit address.
    static VReg lowererShadowAddress(Lowerer* l, VReg a) {
        VReg h = mFuncNewReg(l->mf, classGPR);
        MInst ld = inst(mLoad);
        ld.dst = h;
        ld.mem = memAt(ctxReg, l->lay->shadow);
        ld.size = 8;
        ld.dsize = 8;
        lowererEmit(l, ld);
        MInst add = inst(mAlu);
        add.dst = h;
        add.src = a;
        add.size = 8;
        add.aux = convert<u8>(aluAdd);
        lowererEmit(l, add);
        return h;
    }

    // asanRange traps unless the n bytes at a are accessible, a + n within
    // the memory already: the loader's stub counts the accessible bytes at
    // the start of the range's shadow, and the first inaccessible byte is
    // the trap's address.
    static void lowererAsanRange(Lowerer* l, VReg a, VReg n) {
        lowererStrCall(l, symAsanRange, Span<const VReg>({lowererShadowAddress(l, a), n}));
        VReg ok = lowererStrResult(l);
        VReg bad = lowererSum(l, a, ok);
        MInst c = inst(mCmp);
        c.src = ok;
        c.src2 = n;
        c.size = 8;
        lowererEmit(l, c);
        MInst in = inst(mTrapAt);
        in.cc = ccNE;
        in.imm = trapAsan;
        in.src = bad;
        in.size = 1;
        in.aux = asanRange;
        lowererEmit(l, in);
    }

    static void lowererAlu(Lowerer* l, i32 v, Op op, i32 a, i32 b, u8 size) {
        if (op != OpSub && op != OpMul && l->foldable[a] && l->deferred[a] && !(l->foldable[b] && l->deferred[b])) {
            auto swapped = a;
            a = b;
            b = swapped;
        }
        if (op != OpMul && l->foldable[b] && l->deferred[b]) {
            VReg ra = lowererUse(l, a);
            Mem m = lowererFoldedLoad(l, b).v0;
            VReg d = lowererDef(l, v);
            lowererMov(l, d, ra, size);
            MInst in = inst(mAluMem);
            in.dst = d;
            in.mem = m;
            in.size = size;
            in.aux = convert<u8>(aluOpcode(op));
            lowererEmit(l, in);
            return;
        }
        if (op == OpAdd && lowererLeaAdd(l, v, a, b, size)) {
            return;
        }
        if (op != OpSub && lowererIsConst(l, a) && !lowererIsConst(l, b)) {
            auto temporary741 = b;
            auto temporary742 = a;
            a = temporary741;
            b = temporary742;
        }
        VReg d = lowererDef(l, v);
        if (op == OpAnd && size == 8 && lowererIsConst(l, b) && convert<u64>(lowererConstBits(l, b)) == u64(4294967295LL)) {
            MInst in = inst(mMov);
            in.dst = d;
            in.src = lowererUse(l, a);
            in.size = 4;
            in.aux = 1;
            lowererEmit(l, in);
            return;
        }
        auto temporary743 = lowererImm(l, b, size);
        i64 x = temporary743.v0;
        bool ok = temporary743.v1;
        if (ok) {
            switch (op) {
                case OpAdd:
                case OpSub: {
                    if (op == OpSub) {
                        x = -x;
                    }
                    if (x == convert<i64>(convert<i32>(x))) {
                        MInst in = inst(mLea);
                        in.dst = d;
                        in.mem = memAt(lowererUse(l, a), convert<i32>(x));
                        in.size = size;
                        lowererEmit(l, in);
                        return;
                    }
                    break;
                }
                case OpMul: {
                    MInst in = inst(mImul3);
                    in.dst = d;
                    in.src = lowererUse(l, a);
                    in.imm = x;
                    in.size = size;
                    lowererEmit(l, in);
                    return;
                }
            }
            if (op != OpAdd && op != OpSub) {
                lowererMov(l, d, lowererUse(l, a), size);
                MInst in = inst(mAluImm);
                in.dst = d;
                in.imm = x;
                in.size = size;
                in.aux = convert<u8>(aluOpcode(op));
                lowererEmit(l, in);
                return;
            }
        }
        // The destination starts as a copy of the first operand: of a
        // commutative operation, the one used only here, in this block,
        // which then shares its register.
        if ((op == OpMul || op == OpAnd || op == OpOr || op == OpXor) && l->uses[a] > 1 && l->uses[b] == 1 && l->f->values[b].block == l->block && l->f->values[b].op != OpParam) {
            auto swapped = a;
            a = b;
            b = swapped;
        }
        VReg ra = lowererUse(l, a);
        VReg rb = lowererUse(l, b);
        lowererMov(l, d, ra, size);
        MInst in = inst(mAlu);
        in.dst = d;
        in.src = rb;
        in.size = size;
        in.aux = convert<u8>(aluOpcode(op));
        lowererEmit(l, in);
    }

    // call calls a function of the module, an imported one as Go calls it;
    // one that returns twice through its adapter, which writes the jump
    // frame.
    static void lowererCall(Lowerer* l, i32 v, Int index, Span<const i32> args) {
        FuncType* t = moduleFuncType(l->m, convert<u32>(index));
        if (index < l->m->importedFuncs && l->oc->importIntrinsics[index] != intrinsicNone) {
            lowererIntrinsic(l, v, l->oc->importIntrinsics[index], args);
            return;
        }
        if (index < l->m->importedFuncs && !l->m->funcs[index].returnsTwice) {
            Slice<VReg> regs = makeSlice<VReg>(length(args));
            for (Int i = 0; i < length(args); ++i) {
                regs[i] = lowererUse(l, args[i]);
            }
            lowererGoCall(l, v, regs, t->params, t->results, l->oc->importSyms[index]);
            return;
        }
        auto temporary745 = lowererPlaceArgs(l, args);
        Slice<VReg> uses = temporary745.v0;
        Int stack = temporary745.v1;
        Slice<VReg> regs = resultRegs(t->results);
        lowererOutgoing(l, stack, regs);
        MInst in = inst(mCall);
        in.aux = calleeFunc;
        in.imm = convert<i64>(index);
        mInstExtra(&in)->uses.assign(uses);
        mInstExtra(&in)->defs.assign(assigned(regs));
        l->mf->hasCalls = true;
        if (l->m->funcs[index].returnsTwice) {
            in.aux = calleeJump;
            l->mf->returnsTwice = true;
        }
        lowererEmit(l, in);
        lowererTakeResults(l, v, t->results, regs, stack);
    }

    // goCall calls the function sym names as Go calls a function: the
    // instance goes first, in RAX, which the emitter sets; the arguments
    // follow in Go's registers and stack slots, and so do the results. Go
    // leaves the upper half of a 32-bit result alone: a move of 4 bytes
    // clears it.
    static void lowererGoCall(Lowerer* l, i32 v, Slice<VReg> regs, Slice<ValType> params, Slice<ValType> results, String sym) {
        GoFrame g = goAssign(params, results);
        Slice<VReg> uses = {};
        MInst pm = inst(mParMove);
        for (Int i = 0; i < length(params); ++i) {
            GoLoc loc = g.params[i];
            if (loc.reg != noReg) {
                mInstExtra(&pm)->moves.pushBack(Move{loc.reg, regs[i]});
                uses = appendValues(uses, loc.reg);
                continue;
            }
            MInst in = inst(mStackArg);
            in.src = regs[i];
            in.imm = convert<i64>(loc.off);
            in.size = convert<u8>(goSize(params[i]));
            in.aux = 1;
            lowererEmit(l, in);
        }
        if (length(mInstMoves(&pm)) > 0) {
            lowererEmit(l, pm);
        }
        if (Int(g.size) > l->mf->stackArgs) {
            l->mf->stackArgs = g.size;
        }
        Slice<VReg> defs = {};
        for (Int i = 0; i < length(results); ++i) {
            if (g.results[i].reg != noReg) {
                defs = appendValues(defs, g.results[i].reg);
            }
        }
        MInst in = inst(mCall);
        in.aux = calleeGo;
        in.imm = convert<i64>(objCodeExtern(l->oc, sym));
        mInstExtra(&in)->uses.assign(uses);
        mInstExtra(&in)->defs.assign(defs);
        l->mf->hasCalls = true;
        lowererEmit(l, in);
        if (length(results) == 0 || v < 0) {
            return;
        }
        Slice<VReg> dsts = {};
        if (length(results) == 1) {
            dsts = sliceOf<VReg>({lowererDef(l, v)});
        } else {
            dsts = makeSlice<VReg>(length(results));
            for (Int i = 0, end = length(dsts); i < end; ++i) {
                dsts[i] = noReg;
            }
            auto extracts = lowererExtracts(l, v);
            for (Int i = 0; i < length(extracts); ++i) {
                i32 user = extracts[i];
                dsts[l->f->values[user].aux] = lowererDef(l, user);
            }
        }
        for (Int i = 0; i < length(results); ++i) {
            VReg d = dsts[i];
            GoLoc loc = g.results[i];
            if (d == noReg) {
                continue;
            }
            if (loc.reg == noReg) {
                MInst ld = inst(isFloat(results[i]) ? mFLoad : mLoad);
                ld.dst = d;
                ld.mem = memAt(RSP, loc.off);
                ld.size = convert<u8>(goSize(results[i]));
                ld.dsize = ld.size;
                lowererEmit(l, ld);
            } else if (results[i] == I32) {
                MInst mv = inst(mMov);
                mv.dst = d;
                mv.src = loc.reg;
                mv.size = 4;
                mv.aux = 1;
                lowererEmit(l, mv);
            } else {
                lowererMov(l, d, loc.reg, typeSize(results[i]));
            }
        }
    }

    static void lowererCallIndirect(Lowerer* l, i32 v, Int typeIndex, Int table, Span<const i32> args) {
        FuncType* t = &l->m->types[typeIndex];
        i32 elem = args[length(args) - 1];
        args = subslice(args, 0, length(args) - 1);
        i32 tb = l->lay->tables[table];
        VReg re = lowererUse(l, elem);
        MInst c = inst(mCmpMem);
        c.src = re;
        c.mem = memAt(ctxReg, tb + tableSize);
        c.size = 4;
        lowererEmit(l, c);
        lowererTrapIf(l, ccAE, trapUndefinedElem);
        VReg entries = lowererLoad64(l, memAt(ctxReg, tb + tableEntries));
        VReg desc = lowererLoad64(l, Mem{entries, re, u8(8LL), {}, -1});
        MInst tst = inst(mTest);
        tst.src = desc;
        tst.src2 = desc;
        tst.size = 8;
        lowererEmit(l, tst);
        lowererTrapIf(l, ccE, trapUninitElem);
        i64 id = convert<i64>(typeID(t));
        if (id == convert<i64>(convert<i32>(id))) {
            MInst ci = inst(mCmpMemImm);
            ci.mem = memAt(desc, 8);
            ci.imm = id;
            ci.size = 8;
            lowererEmit(l, ci);
        } else {
            MInst cm = inst(mCmpMem);
            cm.src = lowererConstReg(l, id, 8);
            cm.mem = memAt(desc, 8);
            cm.size = 8;
            lowererEmit(l, cm);
        }
        lowererTrapIf(l, ccNE, trapIndirectType);
        VReg code = lowererLoad64(l, memAt(desc, 0));
        auto temporary746 = lowererPlaceArgs(l, args);
        Slice<VReg> uses = temporary746.v0;
        Int stack = temporary746.v1;
        Slice<VReg> regs = resultRegs(t->results);
        lowererOutgoing(l, stack, regs);
        MInst in = inst(mCallInd);
        in.src = code;
        mInstExtra(&in)->uses.assign(uses);
        mInstExtra(&in)->defs.assign(assigned(regs));
        l->mf->hasCalls = true;
        lowererEmit(l, in);
        lowererTakeResults(l, v, t->results, regs, stack);
    }

    // checkRange traps unless a + n <= limit, all unsigned 32-bit values
    // summed in 64 bits; limit is a memory operand.
    static void lowererCheckRange(Lowerer* l, VReg a, VReg n, Mem limit, u8 limitSize, i64 code) {
        VReg t = mFuncNewReg(l->mf, classGPR);
        lowererMov(l, t, a, 8);
        MInst add = inst(mAlu);
        add.dst = t;
        add.src = n;
        add.size = 8;
        add.aux = 0;
        lowererEmit(l, add);
        if (limitSize == 8) {
            MInst c = inst(mCmpMem);
            c.src = t;
            c.mem = limit;
            c.size = 8;
            lowererEmit(l, c);
        } else {
            VReg lim = mFuncNewReg(l->mf, classGPR);
            MInst ld = inst(mLoad);
            ld.dst = lim;
            ld.mem = limit;
            ld.size = limitSize;
            ld.dsize = 8;
            lowererEmit(l, ld);
            MInst c = inst(mCmp);
            c.src = t;
            c.src2 = lim;
            c.size = 8;
            lowererEmit(l, c);
        }
        lowererTrapIf(l, ccA, code);
    }

    static u8 lowererClassOf(Lowerer* l, ValType t) {
        if (isFloat(t) || t == V128) {
            return classXMM;
        }
        return classGPR;
    }

    // compare emits the flags for comparison c and returns the condition that
    // holds when it is true. A float equality needs the parity flag as well:
    // parity is then reported through the second result, 1 when an unordered
    // result must make the comparison false (eq) and 2 when it must make it
    // true (ne).
    // branchPair reports whether c, which a block branches on, is an i32 and
    // or or of two integer comparisons of that block that nothing else
    // reads: the branch can test each in turn.
    static bool lowererBranchPair(Lowerer* l, i32 c) {
        Function* f = l->f;
        const Value* val = &f->values[c];
        if ((val->op != OpAnd && val->op != OpOr) || val->typ != I32) {
            return false;
        }
        Span<i32> args = functionArgs(f, c);
        auto comparison = [&](i32 x) {
            const Value* xv = &f->values[x];
            return xv->op >= OpEqz && xv->op <= OpGeU && l->uses[x] == 1 && xv->block == val->block;
        };
        return args[0] != args[1] && comparison(args[0]) && comparison(args[1]);
    }

    static Tuple2<u8, Int> lowererCompare(Lowerer* l, i32 c) {
        Function* f = l->f;
        Value* val = &f->values[c];
        Span<i32> args = functionArgs(f, c);
        if (val->op == OpAnd) {
            lowererTestBits(l, args[0], args[1]);
            return {ccNE, 0};
        }
        if (val->op == OpXor || val->op == OpSub) {
            MInst in = inst(mCmp);
            in.src = lowererUse(l, args[0]);
            in.src2 = lowererUse(l, args[1]);
            in.size = typeSize(f->values[args[0]].typ);
            lowererEmit(l, in);
            return {ccNE, 0};
        }
        if (val->op == OpOr) {
            lowererLowerValue(l, c);
            return {ccNE, 0};
        }
        if (val->op == OpEqz) {
            i32 v = args[0];
            if (l->deferred[v] && f->values[v].op == OpAnd) {
                Span<i32> bits = functionArgs(f, v);
                lowererTestBits(l, bits[0], bits[1]);
                return {ccE, 0};
            }
            auto folded = lowererFoldedLoad(l, v);
            if (folded.v1) {
                MInst in = inst(mCmpMemImm);
                in.mem = folded.v0;
                in.imm = 0;
                in.size = convert<u8>(memKindWidth(convert<MemKind>(f->values[v].aux2)));
                lowererEmit(l, in);
                return {ccE, 0};
            }
            VReg a = lowererUse(l, args[0]);
            MInst in = inst(mTest);
            in.src = a;
            in.src2 = a;
            in.size = typeSize(f->values[args[0]].typ);
            lowererEmit(l, in);
            return {ccE, 0};
        }
        i32 a = args[0];
        i32 b = args[1];
        u8 size = typeSize(f->values[a].typ);
        if (val->op >= OpFEq) {
            Op op = val->op;
            if (op == OpFLt || op == OpFLe) {
                auto temporary749 = b;
                auto temporary750 = a;
                a = temporary749;
                b = temporary750;
            }
            MInst in = inst(mFCmp);
            in.src = lowererUse(l, a);
            in.src2 = lowererUse(l, b);
            in.size = size;
            lowererEmit(l, in);
            switch (op) {
                case OpFEq: {
                    return {ccE, 1};
                }
                case OpFNe: {
                    return {ccNE, 2};
                }
                case OpFLt:
                case OpFGt: {
                    return {ccA, 0};
                }
            }
            return {ccAE, 0};
        }
        u8 cc = intCC[Op(val->op - OpEq)];
        if (lowererIsConst(l, a) && !lowererIsConst(l, b)) {
            auto temporary752 = b;
            auto temporary753 = a;
            a = temporary752;
            b = temporary753;
            cc = swapCC(cc);
        }
        auto temporary754 = lowererImm(l, b, size);
        i64 x = temporary754.v0;
        bool ok = temporary754.v1;
        if (ok) {
            auto folded = lowererFoldedLoad(l, a);
            if (folded.v1) {
                MInst in = inst(mCmpMemImm);
                in.mem = folded.v0;
                in.imm = x;
                in.size = convert<u8>(memKindWidth(convert<MemKind>(f->values[a].aux2)));
                lowererEmit(l, in);
                return {cc, 0};
            }
            MInst in = inst(mCmpImm);
            in.src = lowererUse(l, a);
            in.imm = x;
            in.size = size;
            if (x == 0 && (cc == ccE || cc == ccNE)) {
                in.op = mTest;
                in.src2 = in.src;
            }
            lowererEmit(l, in);
            return {cc, 0};
        }
        if (l->foldable[a] && l->deferred[a] && !(l->foldable[b] && l->deferred[b])) {
            auto swapped = a;
            a = b;
            b = swapped;
            cc = swapCC(cc);
        }
        if (l->foldable[b] && l->deferred[b]) {
            MInst in = inst(mCmpMem);
            in.src = lowererUse(l, a);
            in.mem = lowererFoldedLoad(l, b).v0;
            in.size = size;
            lowererEmit(l, in);
            return {cc, 0};
        }
        MInst in = inst(mCmp);
        in.src = lowererUse(l, a);
        in.src2 = lowererUse(l, b);
        in.size = size;
        lowererEmit(l, in);
        return {cc, 0};
    }

    static i64 lowererConstBits(Lowerer* l, i32 v) {
        if (l->f->values[v].op == OpConst) {
            return l->f->values[v].aux;
        }
        return i64(0LL);
    }

    static VReg lowererConstReg(Lowerer* l, i64 x, u8 size) {
        VReg r = mFuncNewReg(l->mf, classGPR);
        MInst in = inst(mMovImm);
        in.dst = r;
        in.imm = x;
        in.size = size;
        lowererEmit(l, in);
        return r;
    }

    static void lowererConvert(Lowerer* l, i32 v, Op op, i32 a, u8 size) {
        ValType srcType = l->f->values[a].typ;
        VReg src = lowererUse(l, a);
        VReg d = lowererDef(l, v);
        if (op == OpConvertU && srcType == I64) {
            MInst in = inst(mCvtU64F);
            in.dst = d;
            in.src = src;
            in.dsize = size;
            in.size = 8;
            mInstExtra(&in)->temps.assign(Span<const VReg>({mFuncNewReg(l->mf, classGPR), mFuncNewReg(l->mf, classGPR)}));
            lowererEmit(l, in);
            return;
        }
        MInst in = inst(mCvtIF);
        in.dst = d;
        in.src = src;
        in.dsize = size;
        in.size = typeSize(srcType);
        if (op == OpConvertU) {
            in.size = 8;
            if (isNil(l->addresses)) {
                l->addresses = activePool->make<ValueRanges>(ValueRanges{l->f, makeSlice<ValueRange>(length(l->f->values)), makeSlice<bool>(length(l->f->values))});
            }
            const ValueRange range = valueRangesOf(l->addresses, a, 0);
            if (range.lo >= 0 && range.hi <= 2147483647) {
                in.size = 4;
            }
        }
        lowererEmit(l, in);
    }

    static void lowererCopysign(Lowerer* l, i32 v, i32 a, i32 b, u8 size) {
        i32 sign = 0;
        i32 abs = 1;
        if (size == 8) {
            sign = constF64Sign;
            abs = constF64Abs;
        }
        VReg t1 = mFuncNewReg(l->mf, classXMM);
        MInst in = inst(mFAluMem);
        in.dst = t1;
        in.src = lowererUse(l, b);
        in.mem = Mem{noReg, noReg, {}, {}, sign};
        in.aux = 6;
        in.size = size;
        lowererEmit(l, in);
        VReg t2 = mFuncNewReg(l->mf, classXMM);
        in = inst(mFAluMem);
        in.dst = t2;
        in.src = lowererUse(l, a);
        in.mem = Mem{noReg, noReg, {}, {}, abs};
        in.aux = 6;
        in.size = size;
        lowererEmit(l, in);
        MInst orValue = inst(mFAlu);
        orValue.dst = lowererDef(l, v);
        orValue.src = t1;
        orValue.src2 = t2;
        orValue.aux = 8;
        orValue.size = size;
        lowererEmit(l, orValue);
    }

    static void lowererCountUses(Lowerer* l) {
        Function* f = l->f;
        l->uses.resize(f->values.size());
        l->uses.fill(0);
        auto order = l->ci->order;
        for (Int i = 0; i < length(order); ++i) {
            i32 b = order[i];
            Block* blk = &f->blocks[b];
            auto instrs = blk->instrs.view();
            for (Int i = 0; i < length(instrs); ++i) {
                i32 v = instrs[i];
                auto args = functionArgs(f, v);
                for (Int i = 0; i < length(args); ++i) {
                    i32 a = args[i];
                    l->uses[a]++;
                }
            }
            Term* t = &blk->term;
            if (t->kind == TermIf || t->kind == TermSwitch) {
                l->uses[t->value]++;
            }
            auto values = t->values;
            for (Int i = 0; i < length(values); ++i) {
                i32 v = values[i];
                l->uses[v]++;
            }
            auto edges = t->edges;
            for (Int i = 0; i < length(edges); ++i) {
                Edge e = edges[i];
                auto args = e.args;
                for (Int i = 0; i < length(args); ++i) {
                    i32 a = args[i];
                    l->uses[a]++;
                }
            }
        }
    }

    static VReg lowererDef(Lowerer* l, i32 v) {
        if (l->vregs[v] == noReg) {
            l->vregs[v] = mFuncNewReg(l->mf, lowererClassOf(l, l->f->values[v].typ));
        }
        return l->vregs[v];
    }

    // signedMagic gives the multiplier and shift dividing a 32-bit signed value
    // by d, 2 <= |d| < 2^31, after Hacker's Delight, figure 10-1.
    static Tuple2<i32, i32> signedMagic(i32 d) {
        const u32 two31 = 0x80000000u;
        const u32 ad = d < 0 ? 0 - u32(d) : u32(d);
        const u32 t = two31 + (u32(d) >> 31);
        const u32 anc = t - 1 - t % ad;
        i32 p = 31;
        u32 q1 = two31 / anc;
        u32 r1 = two31 - q1 * anc;
        u32 q2 = two31 / ad;
        u32 r2 = two31 - q2 * ad;
        u32 delta = 0;
        do {
            ++p;
            q1 *= 2;
            r1 *= 2;
            if (r1 >= anc) {
                ++q1;
                r1 -= anc;
            }
            q2 *= 2;
            r2 *= 2;
            if (r2 >= ad) {
                ++q2;
                r2 -= ad;
            }
            delta = ad - r2;
        } while (q1 < delta || (q1 == delta && r1 == 0));
        const i32 m = i32(q2 + 1);
        return {d < 0 ? -m : m, p - 32};
    }

    static void lowererShift(Lowerer* l, VReg r, Int kind, i64 n, u8 size) {
        if (n == 0) {
            return;
        }
        MInst in = inst(mShiftImm);
        in.dst = r;
        in.imm = n;
        in.size = size;
        in.aux = u8(kind);
        lowererEmit(l, in);
    }

    static void lowererAluTo(Lowerer* l, VReg dst, AluOp op, VReg src, u8 size) {
        MInst in = inst(mAlu);
        in.dst = dst;
        in.src = src;
        in.size = size;
        in.aux = u8(op);
        lowererEmit(l, in);
    }

    static VReg lowererCopy(Lowerer* l, VReg src, u8 size) {
        VReg r = mFuncNewReg(l->mf, classGPR);
        lowererMov(l, r, src, size);
        return r;
    }

    static void lowererZero(Lowerer* l, VReg d, u8 size) {
        MInst zero = inst(mMovImm);
        zero.dst = d;
        zero.imm = 0;
        zero.size = size;
        lowererEmit(l, zero);
    }

    // divideByConstant divides by a constant without a division instruction
    // where it can: shifts for powers of two, otherwise a multiplication by
    // the divisor's reciprocal for 32-bit divisors. A remainder subtracts the
    // product of the quotient and the divisor. It reports false for zero, a
    // signed quotient by -1 and the divisors it leaves to a division.
    static bool lowererDivideByConstant(Lowerer* l, i32 v, Op op, i32 a, i32 b, u8 size) {
        if (l->f->values[b].op != OpConst) {
            return false;
        }
        const bool signedValue = op == OpDivS || op == OpRemS;
        const bool remainder = op == OpRemS || op == OpRemU;
        const i64 width = size == 8 ? 64 : 32;
        const u64 mask = width == 64 ? ~u64(0) : u64(0xffffffffu);
        const u64 bits = u64(lowererConstBits(l, b)) & mask;
        const i64 divisor = width == 64 ? i64(bits) : i64(i32(u32(bits)));
        // A signed minimum keeps its bits as the magnitude.
        const u64 magnitude = signedValue && divisor < 0 ? (u64(0) - bits) & mask : bits;
        const bool power = (magnitude & (magnitude - 1)) == 0;
        if (bits == 0 || (signedValue && divisor == -1 && !remainder)) {
            return false;
        }
        if (!power && (width != 32 || magnitude >= (u64(1) << 31))) {
            return false;
        }
        VReg x = lowererUse(l, a);
        VReg d = lowererDef(l, v);
        if (magnitude == 1) {
            if (remainder) {
                lowererZero(l, d, size);
            } else {
                lowererMov(l, d, x, size);
            }
            return true;
        }
        if (power) {
            const i64 k = bitsTrailingZeros64(magnitude);
            if (!signedValue) {
                lowererMov(l, d, x, size);
                if (remainder) {
                    lowererShift(l, d, shiftShl, width - k, size);
                    lowererShift(l, d, shiftShr, width - k, size);
                } else {
                    lowererShift(l, d, shiftShr, k, size);
                }
                return true;
            }
            // Rounding toward zero adds 2^k - 1 to a negative dividend.
            VReg t = lowererCopy(l, x, size);
            lowererShift(l, t, shiftSar, width - 1, size);
            lowererShift(l, t, shiftShr, width - k, size);
            lowererAluTo(l, t, aluAdd, x, size);
            if (remainder) {
                lowererShift(l, t, shiftSar, k, size);
                lowererShift(l, t, shiftShl, k, size);
                lowererMov(l, d, x, size);
                lowererAluTo(l, d, aluSub, t, size);
                return true;
            }
            lowererShift(l, t, shiftSar, k, size);
            lowererMov(l, d, t, size);
            if (divisor < 0) {
                MInst neg = inst(mUnary);
                neg.dst = d;
                neg.size = size;
                neg.aux = 0;
                lowererEmit(l, neg);
            }
            return true;
        }
        VReg q = mFuncNewReg(l->mf, classGPR);
        if (signedValue) {
            const auto magic = signedMagic(i32(divisor));
            VReg wide = mFuncNewReg(l->mf, classGPR);
            MInst ext = inst(mExt);
            ext.dst = wide;
            ext.src = x;
            ext.size = 4;
            ext.dsize = 8;
            ext.aux = 1;
            lowererEmit(l, ext);
            MInst mul = inst(mImul3);
            mul.dst = q;
            mul.src = wide;
            mul.imm = magic.v0;
            mul.size = 8;
            lowererEmit(l, mul);
            lowererShift(l, q, shiftSar, 32, u8(8LL));
            if (divisor > 0 && magic.v0 < 0) {
                lowererAluTo(l, q, aluAdd, x, 4);
            } else if (divisor < 0 && magic.v0 > 0) {
                lowererAluTo(l, q, aluSub, x, 4);
            }
            lowererShift(l, q, shiftSar, magic.v1, u8(4LL));
            VReg sign = lowererCopy(l, q, 4);
            lowererShift(l, sign, shiftShr, 31, u8(4LL));
            lowererAluTo(l, q, aluAdd, sign, 4);
        } else {
            // q = x * m >> (32 + s) with m = floor(2^(32+s) / d) + 1 of 33 bits:
            // x * (m - 2^32) fits 64 bits, and adding x stands for the top bit.
            const i64 s = 64 - bitsLeadingZeros64(magnitude - 1);
            const u64 low = (u64(1) << (32 + s)) / magnitude + 1 - (u64(1) << 32);
            if (low < (u64(1) << 31)) {
                MInst mul = inst(mImul3);
                mul.dst = q;
                mul.src = x;
                mul.imm = i64(low);
                mul.size = 8;
                lowererEmit(l, mul);
            } else {
                MInst c = inst(mMovImm);
                c.dst = q;
                c.imm = i64(low);
                c.size = 8;
                lowererEmit(l, c);
                lowererAluTo(l, q, aluImul, x, 8);
            }
            lowererShift(l, q, shiftShr, 32, u8(8LL));
            lowererAluTo(l, q, aluAdd, x, 8);
            lowererShift(l, q, shiftShr, s, u8(8LL));
        }
        if (!remainder) {
            lowererMov(l, d, q, 4);
            return true;
        }
        MInst product = inst(mImul3);
        product.dst = q;
        product.src = q;
        product.imm = divisor;
        product.size = 4;
        lowererEmit(l, product);
        lowererMov(l, d, x, 4);
        lowererAluTo(l, d, aluSub, q, 4);
        return true;
    }

    static void lowererDivide(Lowerer* l, i32 v, Op op, i32 a, i32 b, u8 size) {
        if (lowererDivideByConstant(l, v, op, a, b, size)) {
            return;
        }
        // A constant divisor needs neither the zero test nor, unless it is
        // -1, the guard of a signed minimum's quotient and remainder.
        const bool constant = l->f->values[b].op == OpConst;
        const u64 bits = u64(lowererConstBits(l, b)) & (size == 8 ? ~u64(0) : u64(0xffffffffu));
        const bool nonzero = constant && bits != 0;
        const bool minusOne = !constant || bits == (size == 8 ? ~u64(0) : u64(0xffffffffu));
        VReg ra = lowererUse(l, a);
        VReg rb = lowererUse(l, b);
        if (!nonzero) {
            MInst t = inst(mTest);
            t.src = rb;
            t.src2 = rb;
            t.size = size;
            lowererEmit(l, t);
            lowererTrapIf(l, ccE, trapDivByZero);
        }
        bool signedValue = op == OpDivS || op == OpRemS;
        if (op == OpDivS && minusOne) {
            MInst in = inst(mDivOvf);
            in.src = ra;
            in.src2 = rb;
            in.size = size;
            lowererEmit(l, in);
        }
        if (op == OpRemS && minusOne) {
            VReg one = mFuncNewReg(l->mf, classGPR);
            MInst mi = inst(mMovImm);
            mi.dst = one;
            mi.imm = 1;
            mi.size = 4;
            lowererEmit(l, mi);
            VReg divisor = mFuncNewReg(l->mf, classGPR);
            lowererMov(l, divisor, rb, size);
            MInst c = inst(mCmpImm);
            c.src = rb;
            c.imm = -1;
            c.size = size;
            lowererEmit(l, c);
            MInst cm = inst(mCmov);
            cm.dst = divisor;
            cm.src = one;
            cm.cc = ccE;
            cm.size = size;
            lowererEmit(l, cm);
            rb = divisor;
        }
        lowererMov(l, RAX, ra, size);
        if (signedValue) {
            MInst in = inst(mSignExtAx);
            in.size = size;
            mInstExtra(&in)->uses.assign(Span<const VReg>({RAX}));
            mInstExtra(&in)->defs.assign(Span<const VReg>({RDX}));
            lowererEmit(l, in);
        } else {
            MInst in = inst(mZeroDx);
            mInstExtra(&in)->defs.assign(Span<const VReg>({RDX}));
            lowererEmit(l, in);
        }
        MInst in = inst(mDiv);
        in.src = rb;
        in.size = size;
        mInstExtra(&in)->uses.assign(Span<const VReg>({RAX, RDX}));
        mInstExtra(&in)->defs.assign(Span<const VReg>({RAX, RDX}));
        if (signedValue) {
            in.aux = 1;
        }
        lowererEmit(l, in);
        if (op == OpDivS || op == OpDivU) {
            lowererMov(l, lowererDef(l, v), RAX, size);
        } else {
            lowererMov(l, lowererDef(l, v), RDX, size);
        }
    }

    // edgeBlock returns the MIR block a branch along edge i of SSA block b
    // targets, placing the parameter moves on a block of their own when b has
    // other successors.
    static i32 lowererEdgeBlock(Lowerer* l, i32 b, Int i) {
        Edge* e = &l->f->blocks[b].term.edges[i];
        i32 target = l->mblk[e->block];
        if (length(e->args) == 0 && length(l->f->blocks[e->block].preds) <= 1) {
            return target;
        }
        Array<i32, 2> key = Array<i32, 2>{{b, convert<i32>(i)}};
        {
            auto temporary777 = lookupMap(l->split, key);
            i32 s = temporary777.v0;
            bool ok = temporary777.v1;
            if (ok) {
                return s;
            }
        }
        i32 s = convert<i32>(length(l->mf->blocks));
        l->mf->blocks.pushBack(MBlock{{}, {}, {}, l->mf->blocks[l->cur].depth, -1, {}});
        setMap(l->split, key, s);
        i32 saved = l->cur;
        l->cur = s;
        lowererEdgeMoves(l, e);
        MInst j = inst(mJmp);
        j.target = target;
        lowererEmit(l, j);
        l->cur = saved;
        return s;
    }

    static void lowererEdgeMoves(Lowerer* l, Edge* e) {
        if (length(e->args) == 0) {
            return;
        }
        Slice<i32> params = l->f->blocks[e->block].params;
        MInst in = inst(mParMove);
        auto args = e->args;
        for (Int i = 0; i < length(args); ++i) {
            i32 a = args[i];
            VReg src = lowererUse(l, a);
            VReg dst = l->vregs[params[i]];
            if (src != dst) {
                mInstExtra(&in)->moves.pushBack(Move{dst, src});
            }
        }
        if (length(mInstMoves(&in)) > 0) {
            lowererEmit(l, in);
        }
    }

    static MInst* lowererEmit(Lowerer* l, MInst in) {
        MBlock* blk = &l->mf->blocks[l->cur];
        in.pos = l->pos;
        blk->insts.pushBack(in);
        return &blk->insts[length(blk->insts) - 1];
    }

    static void lowererEntry(Lowerer* l) {
        Function* f = l->f;
        Slice<i32> params = f->blocks[l->ci->order[0]].params;
        Int gi = 0;
        Int xi = 0;
        Slice<i32> stacked;
        MInst pm = inst(mParMove);
        for (Int i = 0; i < length(params); ++i) {
            i32 p = params[i];
            ValType t = f->values[p].typ;
            VReg r = l->vregs[p];
            if (isFloat(t)) {
                if (xi < length(argXMMs)) {
                    mInstExtra(&pm)->moves.pushBack(Move{r, argXMMs[xi]});
                    xi++;
                    continue;
                }
            } else if (gi < length(argGPRs)) {
                mInstExtra(&pm)->moves.pushBack(Move{r, argGPRs[gi]});
                gi++;
                continue;
            }
            stacked = appendValues(stacked, p);
        }
        // Release fixed incoming registers before loading stack arguments.
        // All allocatable XMM registers can be occupied by those inputs.
        if (length(mInstMoves(&pm)) > 0) {
            lowererEmit(l, pm);
        }
        for (Int stack = 0; stack < length(stacked); ++stack) {
            const i32 p = stacked[stack];
            MInst in = inst(mLoadArg);
            in.dst = l->vregs[p];
            in.imm = convert<i64>(stack);
            in.size = typeSize(f->values[p].typ);
            lowererEmit(l, in);
        }
        l->mf->incoming = length(stacked);
    }

    // extracts lists the OpExtract values of a multi-value call; the frontend
    // emits them right after it.
    static Slice<i32> lowererExtracts(Lowerer* l, i32 call) {
        Function* f = l->f;
        Block* blk = &f->blocks[f->values[call].block];
        Slice<i32> out = {};
        auto instrs = blk->instrs.view();
        for (Int i = 0; i < length(instrs); ++i) {
            i32 v = instrs[i];
            if (v != call) {
                continue;
            }
            auto range789 = subslice(blk->instrs.view(), i + 1, length(blk->instrs));
            for (Int index790 = 0, rangeEnd791 = length(range789); index790 < rangeEnd791; ++index790) {
                i32 w = range789[index790];
                if (f->values[w].op != OpExtract || functionArgs(f, w)[0] != call) {
                    break;
                }
                out = appendValues(out, w);
            }
        }
        return out;
    }

    // Saturation is unnecessary when the input is in the signed destination's
    // range on every path that observes the result. A select may discard an
    // out-of-range conversion; raw x86 conversion is non-trapping too, so its
    // unobserved result does not matter. Trapping wasm conversions stay intact.
    static void lowererFindRawTruncations(Lowerer* l) {
        Function* f = l->f;
        l->rawTrunc.resize(f->values.size());
        l->rawTrunc.fill(false);
        Slice<i32> user = makeSlice<i32>(length(f->values));
        for (Int v = 0; v < length(user); ++v) {
            user[v] = -1;
        }
        auto order = l->ci->order;
        for (Int i = 0; i < length(order); ++i) {
            i32 b = order[i];
            auto instrs = f->blocks[b].instrs.view();
            for (Int i = 0; i < length(instrs); ++i) {
                i32 v = instrs[i];
                auto range801 = functionArgs(f, v);
                for (Int index802 = 0, rangeEnd803 = length(range801); index802 < rangeEnd803; ++index802) {
                    i32 a = range801[index802];
                    user[a] = v;
                }
            }
        }
        for (Int v = 0, end = length(f->values); v < end; ++v) {
            Value* val = &f->values[v];
            if ((val->op != OpTruncSatS && val->op != OpTruncSatU) || val->block < 0 || l->ci->rpoIndex[val->block] < 0 || l->uses[v] == 0) {
                continue;
            }
            FloatRanges* r = activePool->make<FloatRanges>(FloatRanges{f, mapOf<i32, FloatRange>({}), mapOf<i32, FloatRange>({})});
            i32 b = val->block;
            i32 p = l->ci->idom[b];
            while (p != b) {
                Term* t = &f->blocks[p].term;
                if (t->kind == TermIf && length(t->edges) == 2) {
                    bool a = cfgInfoDominates(l->ci, t->edges[0].block, b);
                    bool z = cfgInfoDominates(l->ci, t->edges[1].block, b);
                    if (a != z) {
                        floatRangesConstrain(r, t->value, a);
                    }
                }
                if (l->ci->idom[p] == p) {
                    break;
                }
                p = l->ci->idom[p];
            }
            if (l->uses[v] == 1 && user[v] >= 0) {
                i32 u = user[v];
                if (f->values[u].op == OpSelect) {
                    Span<i32> args = functionArgs(f, u);
                    if (args[1] != args[2]) {
                        floatRangesConstrain(r, args[0], args[1] == convert<i32>(v));
                    }
                }
            }
            FloatRange bound = floatRangesOf(r, functionArgs(f, convert<i32>(v))[0], 0);
            double hi = mathLdexp(double(1.0), convert<i64>(typeSize(val->typ)) * 8 - 1);
            double lo = -hi;
            if (val->op == OpTruncSatU) {
                lo = double(0.0);
            }
            l->rawTrunc[v] = !bound.nan && bound.lo <= bound.hi && bound.lo >= lo && bound.hi < hi;
        }
    }

    // floatBits materializes a select's alternatives in the class cmov uses.
    // Constants need no trip through an xmm register, and their NaN payloads
    // and signs of zero stay intact.
    static VReg lowererFloatBits(Lowerer* l, i32 v, u8 size) {
        VReg r = mFuncNewReg(l->mf, classGPR);
        MInst in = inst(mMovXG);
        in.dst = r;
        in.size = size;
        if (lowererIsConst(l, v)) {
            in.op = mMovImm;
            in.imm = lowererConstBits(l, v);
        } else {
            in.src = lowererUse(l, v);
        }
        lowererEmit(l, in);
        return r;
    }

    static Mem lowererGlobalAddr(Lowerer* l, Int index) {
        if (l->m->globals[index].imported) {
            VReg p = mFuncNewReg(l->mf, classGPR);
            MInst in = inst(mLoad);
            in.dst = p;
            in.mem = memAt(ctxReg, l->lay->globals[index]);
            in.size = 8;
            in.dsize = 8;
            lowererEmit(l, in);
            return memAt(p, 0);
        }
        return memAt(ctxReg, l->lay->globals[index]);
    }

    static void lowererGlobalGet(Lowerer* l, i32 v, Int index) {
        ValType t = l->m->globals[index].typ;
        Mem mem = lowererGlobalAddr(l, index);
        MInst in = inst(mLoad);
        if (isFloat(t)) {
            in.op = mFLoad;
        }
        in.dst = lowererDef(l, v);
        in.mem = mem;
        in.size = typeSize(t);
        in.dsize = in.size;
        lowererEmit(l, in);
    }

    static void lowererGlobalSet(Lowerer* l, Int index, i32 value) {
        ValType t = l->m->globals[index].typ;
        Mem mem = lowererGlobalAddr(l, index);
        auto temporary807 = lowererImm(l, value, typeSize(t));
        i64 x = temporary807.v0;
        bool ok = temporary807.v1;
        if (ok) {
            MInst in = inst(mStoreImm);
            in.mem = mem;
            in.imm = x;
            in.size = typeSize(t);
            lowererEmit(l, in);
            return;
        }
        MInst in = inst(mStore);
        if (isFloat(t)) {
            in.op = mFStore;
        }
        in.mem = mem;
        in.src = lowererUse(l, value);
        in.size = typeSize(t);
        lowererEmit(l, in);
    }

    // imm reports whether v is a constant usable as an immediate of an
    // instruction of the given width.
    static Tuple2<i64, bool> lowererImm(Lowerer* l, i32 v, u8 size) {
        if (!lowererIsConst(l, v) || isFloat(l->f->values[v].typ)) {
            return {i64(0LL), false};
        }
        i64 x = lowererConstBits(l, v);
        if (size == 4) {
            return {convert<i64>(convert<i32>(x)), true};
        }
        if (x == convert<i64>(convert<i32>(x))) {
            return {x, true};
        }
        return {i64(0LL), false};
    }

    static bool lowererIsConst(Lowerer* l, i32 v) {
        Op op = l->f->values[v].op;
        return op == OpConst || op == OpUndef || op == OpRefNull;
    }

    // LEA computes a sum in one instruction with an independent destination.
    // Its 32-bit form preserves wasm's wrapping arithmetic even when the full
    // effective address overflows, and can absorb a single-use scaled index.
    static bool lowererLeaAdd(Lowerer* l, i32 v, i32 a, i32 b, u8 size) {
        Function* f = l->f;
        u8 scale = 1;
        bool folded = false;
        if (l->deferred[a] && f->values[a].op == OpShl) {
            auto temporary808 = b;
            auto temporary809 = a;
            a = temporary808;
            b = temporary809;
        }
        if (l->deferred[b] && f->values[b].op == OpShl) {
            Span<i32> args = functionArgs(f, b);
            scale = shiftLeft(u8(1LL), convert<Uint>(lowererConstBits(l, args[1]) & convert<i64>(u8(u8(size * 8) - 1))));
            b = args[0];
            folded = true;
        }
        Mem mem = Mem{noReg, noReg, scale, {}, -1};
        auto temporary810 = lowererImm(l, a, size);
        i64 x = temporary810.v0;
        bool ok = temporary810.v1;
        if (ok && x >= -2147483648 && x <= 2147483647 && folded) {
            mem.disp = convert<i32>(x);
            mem.index = lowererUse(l, b);
        } else if (!lowererIsConst(l, a) && !lowererIsConst(l, b)) {
            mem.base = lowererUse(l, a);
            mem.index = lowererUse(l, b);
        } else {
            return false;
        }
        MInst in = inst(mLea);
        in.dst = lowererDef(l, v);
        in.size = size;
        in.mem = mem;
        lowererEmit(l, in);
        return true;
    }

    // linkBlocks fills successor and predecessor lists from the branches.
    static void lowererLinkBlocks(Lowerer* l) {
        MFunc* mf = l->mf;
        for (Int b = 0, end = length(mf->blocks); b < end; ++b) {
            MBlock* blk = &mf->blocks[b];
            for (Int i = 0, end = length(blk->insts.view()); i < end; ++i) {
                MInst* in = &blk->insts[i];
                switch (in->op) {
                    case mJmp:
                    case mJcc: {
                        blk->succs = appendUnique(blk->succs, in->target);
                        break;
                    }
                    case mSwitch: {
                        auto table = mInstTable(in);
                        for (Int i = 0; i < length(table); ++i) {
                            i32 t = table[i];
                            blk->succs = appendUnique(blk->succs, t);
                        }
                        break;
                    }
                }
            }
        }
        for (Int b = 0, end = length(mf->blocks); b < end; ++b) {
            auto succs = mf->blocks[b].succs;
            for (Int i = 0; i < length(succs); ++i) {
                i32 s = succs[i];
                mf->blocks[s].preds = appendValues(mf->blocks[s].preds, convert<i32>(b));
            }
        }
    }

    static void lowererLoad(Lowerer* l, i32 v, i32 addr) {
        Value* val = &l->f->values[v];
        MemKind kind = convert<MemKind>(val->aux2);
        Mem mem = lowererAddress(l, addr, val->aux, memKindWidth(kind), MemKind(kind & memProven) != MemKind(0LL), false);
        MInst in = inst(mLoad);
        if (isFloat(val->typ)) {
            in.op = mFLoad;
        }
        in.dst = lowererDef(l, v);
        in.mem = mem;
        in.size = convert<u8>(memKindWidth(kind));
        in.dsize = typeSize(val->typ);
        if (memKindSigned(kind)) {
            in.aux = 1;
        }
        lowererEmit(l, in);
    }

    // narrowShift loads the high bytes of a load shifted right by whole
    // bytes, little-endian at a higher offset, where nothing writes memory
    // between the load and the shift: the opcode word's argument, say,
    // instead of a copy and a shift.
    static bool lowererNarrowShift(Lowerer* l, i32 v, i32 a, i32 k) {
        Function* f = l->f;
        const Value* lv = &f->values[a];
        if (!lowererIsConst(l, k) || lv->op != OpLoad || lv->typ != I32 || lv->block != f->values[v].block || f->values[v].typ != I32) {
            return false;
        }
        const MemKind kind = convert<MemKind>(lv->aux2);
        const Int width = memKindWidth(kind);
        const i64 shift = lowererConstBits(l, k) & 31;
        Int narrow = 0;
        if (width == 2 && !memKindSigned(kind) && shift == 8) {
            narrow = 1;
        } else if (width == 4 && (shift == 16 || shift == 24)) {
            narrow = 4 - shift / 8;
        }
        if (narrow == 0) {
            return false;
        }
        auto instrs = f->blocks[lv->block].instrs.view();
        Int i = 0;
        while (i < length(instrs) && instrs[i] != a) {
            ++i;
        }
        for (++i; i < length(instrs) && instrs[i] != v; ++i) {
            if (!lowererTransparent(f->values[instrs[i]].op)) {
                return false;
            }
        }
        if (i == length(instrs)) {
            return false;
        }
        const i32 addr = functionArgs(f, a)[0];
        MInst in = inst(mLoad);
        in.mem = lowererAddress(l, addr, lv->aux + shift / 8, narrow, MemKind(kind & memProven) != MemKind(0LL), false);
        in.dst = lowererDef(l, v);
        in.size = convert<u8>(narrow);
        in.dsize = 4;
        lowererEmit(l, in);
        return true;
    }

    static VReg lowererLoad64(Lowerer* l, Mem mem) {
        VReg r = mFuncNewReg(l->mf, classGPR);
        MInst in = inst(mLoad);
        in.dst = r;
        in.mem = mem;
        in.size = 8;
        in.dsize = 8;
        lowererEmit(l, in);
        return r;
    }

    static void lowererLowerValue(Lowerer* l, i32 v) {
        Function* f = l->f;
        Value* val = &f->values[v];
        Span<i32> args = functionArgs(f, v);
        u8 size = typeSize(val->typ);
        switch (val->op) {
            case OpAdd:
            case OpSub:
            case OpAnd:
            case OpOr:
            case OpXor:
            case OpMul: {
                lowererAlu(l, v, val->op, args[0], args[1], size);
                break;
            }
            case OpShl:
            case OpShrS:
            case OpShrU: {
                if (val->op == OpShrU && lowererNarrowShift(l, v, args[0], args[1])) {
                    break;
                }
                lowererShift(l, v, val->op, args[0], args[1], size);
                break;
            }
            case OpRotl:
            case OpRotr: {
                lowererRotate(l, v, val->op, args[0], args[1], size);
                break;
            }
            case OpDivS:
            case OpDivU:
            case OpRemS:
            case OpRemU: {
                lowererDivide(l, v, val->op, args[0], args[1], size);
                break;
            }
            case OpClz:
            case OpCtz:
            case OpPopcnt: {
                MInst in = inst(mBitCount);
                in.dst = lowererDef(l, v);
                in.src = lowererUse(l, args[0]);
                in.size = size;
                in.aux = convert<u8>(Op(val->op - OpClz));
                lowererEmit(l, in);
                break;
            }
            case OpExtend8S:
            case OpExtend16S:
            case OpExtend32S: {
                MInst in = inst(mExt);
                in.dst = lowererDef(l, v);
                in.src = lowererUse(l, args[0]);
                in.size = Array<u8, 3>{{u8(1LL), u8(2LL), u8(4LL)}}[Op(val->op - OpExtend8S)];
                in.dsize = size;
                in.aux = 1;
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
                i32 x = args[0];
                i32 y = args[1];
                if ((val->op == OpFAdd || val->op == OpFMul) && l->foldable[x] && l->deferred[x] && !(l->foldable[y] && l->deferred[y])) {
                    x = args[1];
                    y = args[0];
                }
                if (l->foldable[y] && l->deferred[y]) {
                    MInst in = inst(mFAluMem);
                    in.src = lowererUse(l, x);
                    in.mem = lowererFoldedLoad(l, y).v0;
                    in.dst = lowererDef(l, v);
                    in.size = size;
                    in.aux = convert<u8>(Op(val->op - OpFAdd));
                    lowererEmit(l, in);
                    break;
                }
                MInst in = inst(mFAlu);
                in.dst = lowererDef(l, v);
                in.src = lowererUse(l, x);
                in.src2 = lowererUse(l, y);
                in.size = size;
                in.aux = convert<u8>(Op(val->op - OpFAdd));
                lowererEmit(l, in);
                break;
            }
            case OpFMin:
            case OpFMax: {
                MInst in = inst(mFMinMax);
                in.dst = lowererDef(l, v);
                in.src = lowererUse(l, args[0]);
                in.src2 = lowererUse(l, args[1]);
                in.size = size;
                if (val->op == OpFMax) {
                    in.aux = 1;
                }
                lowererEmit(l, in);
                break;
            }
            case OpFCopysign: {
                lowererCopysign(l, v, args[0], args[1], size);
                break;
            }
            case OpFAbs:
            case OpFNeg: {
                MInst in = inst(mFAluMem);
                in.dst = lowererDef(l, v);
                in.src = lowererUse(l, args[0]);
                in.size = size;
                in.aux = 6;
                in.mem = Mem{noReg, noReg, {}, {}, constF32Abs};
                if (val->op == OpFNeg) {
                    in.aux = 9;
                    in.mem.label = constF32Sign;
                }
                if (size == 8) {
                    in.mem.label += 2;
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
                in.dst = lowererDef(l, v);
                in.src = lowererUse(l, args[0]);
                in.size = size;
                if (val->op != OpFSqrt) {
                    in.aux = fRound;
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
                in.dst = lowererDef(l, v);
                in.src = lowererUse(l, args[0]);
                in.size = 4;
                in.aux = 1;
                lowererEmit(l, in);
                break;
            }
            case OpExtendU: {
                lowererMov(l, lowererDef(l, v), lowererUse(l, args[0]), 8);
                break;
            }
            case OpExtendS: {
                MInst in = inst(mExt);
                in.dst = lowererDef(l, v);
                in.src = lowererUse(l, args[0]);
                in.size = 4;
                in.dsize = 8;
                in.aux = 1;
                lowererEmit(l, in);
                break;
            }
            case OpTruncS:
            case OpTruncU:
            case OpTruncSatS:
            case OpTruncSatU: {
                MInst in = inst(mTruncF);
                in.dst = lowererDef(l, v);
                in.src = lowererUse(l, args[0]);
                in.size = size;
                in.dsize = typeSize(f->values[args[0]].typ);
                if (l->rawTrunc[v]) {
                    in.op = mCvtFI;
                    lowererEmit(l, in);
                    break;
                }
                if (val->op == OpTruncU || val->op == OpTruncSatU) {
                    in.aux |= truncUnsigned;
                    if (size == 4) {
                        mInstExtra(&in)->temps.assign(Span<const VReg>({mFuncNewReg(l->mf, classGPR)}));
                    } else {
                        mInstExtra(&in)->temps.assign(Span<const VReg>({mFuncNewReg(l->mf, classXMM), mFuncNewReg(l->mf, classGPR)}));
                    }
                }
                if (val->op == OpTruncSatS || val->op == OpTruncSatU) {
                    in.aux |= truncSat;
                }
                lowererEmit(l, in);
                break;
            }
            case OpConvertS:
            case OpConvertU: {
                lowererConvert(l, v, val->op, args[0], size);
                break;
            }
            case OpDemote:
            case OpPromote: {
                MInst in = inst(mCvtFF);
                in.dst = lowererDef(l, v);
                in.src = lowererUse(l, args[0]);
                in.size = typeSize(f->values[args[0]].typ);
                in.dsize = size;
                lowererEmit(l, in);
                break;
            }
            case OpReinterpret: {
                MInst in = inst(mMovGX);
                if (!isFloat(val->typ)) {
                    in.op = mMovXG;
                }
                in.dst = lowererDef(l, v);
                in.src = lowererUse(l, args[0]);
                in.size = size;
                lowererEmit(l, in);
                break;
            }
            case OpSelect: {
                lowererSelectValue(l, v, args);
                break;
            }
            case OpLoad: {
                lowererLoad(l, v, args[0]);
                break;
            }
            case OpStore: {
                lowererStore(l, v, args[0], args[1]);
                break;
            }
            case OpMemSize: {
                MInst in = inst(mLoad);
                in.dst = lowererDef(l, v);
                in.mem = memAt(ctxReg, offMemSize);
                in.size = 8;
                in.dsize = 8;
                lowererEmit(l, in);
                MInst sh = inst(mShiftImm);
                sh.dst = in.dst;
                sh.imm = 16;
                sh.size = 8;
                sh.aux = shiftShr;
                lowererEmit(l, sh);
                break;
            }
            case OpMemGrow: {
                lowererGoCall(l, v, sliceOf<VReg>({lowererUse(l, args[0])}), sliceOf<ValType>({I32}), sliceOf<ValType>({I32}), symMemoryGrow);
                break;
            }
            case OpMemCopy: {
                lowererMemCopy(l, args, symMemmove);
                break;
            }
            case OpCopyForward: {
                if (l->m->asan) {
                    // The ranges are within the memory by the branch that
                    // chose the copy; the shadow map decides the rest.
                    lowererAsanRange(l, lowererUse(l, args[0]), lowererUse(l, args[2]));
                    lowererAsanRange(l, lowererUse(l, args[1]), lowererUse(l, args[2]));
                }
                VReg dst = mFuncNewReg(l->mf, classGPR);
                VReg src = mFuncNewReg(l->mf, classGPR);
                lowererMemAddr(l, dst, lowererUse(l, args[0]));
                lowererMemAddr(l, src, lowererUse(l, args[1]));
                MInst pm = inst(mParMove);
                mInstExtra(&pm)->moves.assign(Span<const Move>({Move{RDI, dst}, Move{RSI, src}, Move{RCX, lowererUse(l, args[2])}}));
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
                lowererMemInit(l, convert<i64>(val->aux), args);
                break;
            }
            case OpDataDrop: {
                MInst in = inst(mStoreImm);
                in.mem = memAt(ctxReg, layoutDataDropped(l->lay, convert<i64>(val->aux)));
                in.size = 1;
                in.imm = 1;
                lowererEmit(l, in);
                break;
            }
            case OpGlobalGet: {
                lowererGlobalGet(l, v, convert<i64>(val->aux));
                break;
            }
            case OpGlobalSet: {
                lowererGlobalSet(l, convert<i64>(val->aux), args[0]);
                break;
            }
            case OpCall: {
                lowererCall(l, v, convert<i64>(val->aux), args);
                break;
            }
            case OpCallIndirect: {
                lowererCallIndirect(l, v, convert<i64>(val->aux), convert<i64>(val->aux2), args);
                break;
            }
            case OpExtract: {
                break;
            }
            case OpRefFunc: {
                Int fn = convert<i64>(val->aux);
                if (fn < l->m->importedFuncs && !l->oc->importCode[fn]) {
                    if (l->oc->importIntrinsics[fn] != intrinsicNone) {
                        compilerError(String("unsupported: the address of an import compiled at its calls"));
                    }
                    compilerError(String("ref.func of a function no element, export or global names"));
                }
                MInst in = inst(mLea);
                in.dst = lowererDef(l, v);
                in.mem = memAt(ctxReg, layoutDesc(l->lay, convert<i64>(val->aux)));
                in.size = 8;
                lowererEmit(l, in);
                break;
            }
            case OpRefIsNull: {
                MInst in = inst(mTest);
                VReg a = lowererUse(l, args[0]);
                in.src = a;
                in.src2 = a;
                in.size = 8;
                lowererEmit(l, in);
                MInst set = inst(mSetcc);
                set.dst = lowererDef(l, v);
                set.cc = ccE;
                lowererEmit(l, set);
                break;
            }
            case OpTableGet: {
                lowererTableGet(l, v, convert<i64>(val->aux), args[0]);
                break;
            }
            case OpTableSet: {
                lowererTableSet(l, convert<i64>(val->aux), args[0], args[1]);
                break;
            }
            case OpTableSize: {
                MInst in = inst(mLoad);
                in.dst = lowererDef(l, v);
                in.mem = memAt(ctxReg, l->lay->tables[val->aux] + tableSize);
                in.size = 4;
                in.dsize = 4;
                lowererEmit(l, in);
                break;
            }
            case OpTableGrow: {
                VReg table = lowererConstReg(l, val->aux, 4);
                lowererGoCall(l, v, sliceOf<VReg>({table, lowererUse(l, args[1]), lowererUse(l, args[0])}), sliceOf<ValType>({I32, I32, FuncRef}), sliceOf<ValType>({I32}), symTableGrow);
                break;
            }
            default: {
                compilerError(String("unsupported operator %s in function %d"));
                break;
            }
        }
    }

    // markDeferred finds comparisons whose only use is the branch or a select of
    // their own block.
    static void lowererMarkDeferred(Lowerer* l) {
        Function* f = l->f;
        l->deferred.resize(f->values.size());
        l->deferred.fill(false);
        l->foldable.resize(f->values.size());
        l->foldable.fill(false);
        l->rmw.resize(f->values.size());
        l->rmw.fill(-1);
        l->rmwFlags.resize(f->values.size());
        l->rmwFlags.fill(false);
        l->bounded.resize(f->values.size());
        l->bounded.fill(-1);
        auto order = l->ci->order;
        // An i32 sum of a base and a constant that only loads and stores of
        // its block address is lowered where one of them uses it, if any
        // does: lowererAddress addresses from the base when the base's check
        // covers the sum.
        if (l->m->memoryTail) {
            Slice<i32> addressUses = makeSlice<i32>(length(f->values));
            for (Int j = 0; j < length(order); ++j) {
                i32 b = order[j];
                auto instrs = f->blocks[b].instrs.view();
                for (Int i = 0; i < length(instrs); ++i) {
                    i32 v = instrs[i];
                    if (f->values[v].op != OpLoad && f->values[v].op != OpStore) {
                        continue;
                    }
                    i32 a = functionArgs(f, v)[0];
                    Value* av = &f->values[a];
                    if (av->op != OpAdd || av->typ != I32 || av->block != b) {
                        continue;
                    }
                    Span<i32> sum = functionArgs(f, a);
                    if (lowererIsConst(l, sum[0]) != lowererIsConst(l, sum[1])) {
                        addressUses[a]++;
                    }
                }
            }
            for (Int v = 0, end = length(f->values); v < end; ++v) {
                if (addressUses[v] > 0 && addressUses[v] == l->uses[v]) {
                    l->deferred[v] = true;
                }
            }
        }
        for (Int j = 0; j < length(order); ++j) {
            i32 b = order[j];
            Block* blk = &f->blocks[b];
            if (blk->term.kind == TermIf) {
                i32 c = blk->term.value;
                if (l->uses[c] == 1 && f->values[c].block == b && flagCondition(f->values[c].op)) {
                    l->deferred[c] = true;
                    // Both integer comparisons of an and or an or the block
                    // branches on wait for the branch: each branches itself.
                    if (lowererBranchPair(l, c)) {
                        Span<i32> args = functionArgs(f, c);
                        l->deferred[args[0]] = true;
                        l->deferred[args[1]] = true;
                    }
                }
            }
            auto instrs = blk->instrs.view();
            for (Int j = 0; j < length(instrs); ++j) {
                i32 v = instrs[j];
                Value* val = &f->values[v];
                if (val->op == OpAdd) {
                    Span<i32> args = functionArgs(f, v);
                    Int i = length(args) - 1;
                    for (; i >= 0; i--) {
                        i32 a = args[i];
                        if (lowererScaledIndex(l, a, b)) {
                            l->deferred[a] = true;
                            break;
                        }
                    }
                }
                if (val->op == OpEqz) {
                    i32 a = functionArgs(f, v)[0];
                    if (l->uses[a] == 1 && f->values[a].block == b && f->values[a].op == OpAnd) {
                        l->deferred[a] = true;
                    }
                }
                if (f->values[v].op != OpSelect) {
                    continue;
                }
                i32 c = functionArgs(f, v)[0];
                Op op = f->values[c].op;
                if (l->uses[c] == 1 && f->values[c].block == b && flagCondition(op)) {
                    l->deferred[c] = true;
                }
            }
        }
        lowererMarkUpdates(l);
        lowererMarkFoldable(l);
    }

    // markUpdates finds stores of op(load, x) to the address the load read,
    // at the load's width, op one x86 can apply to memory: the store updates
    // memory in place. The load is in the store's block, or ends the block
    // the store's block has as its only predecessor; between the two no
    // value writes memory or traps otherwise than a load. A load of the
    // store's block used only there goes: the store's check takes its place.
    static void lowererMarkUpdates(Lowerer* l) {
        Function* f = l->f;
        auto order = l->ci->order;
        for (Int o = 0; o < length(order); ++o) {
            const i32 b = order[o];
            auto instrs = f->blocks[b].instrs.view();
            for (Int s = 0; s < length(instrs); ++s) {
                const i32 st = instrs[s];
                const Value* sv = &f->values[st];
                if (sv->op != OpStore) {
                    continue;
                }
                Span<i32> sargs = functionArgs(f, st);
                const i32 addr = sargs[0];
                const i32 y = sargs[1];
                const Value* yv = &f->values[y];
                const Int width = memKindWidth(convert<MemKind>(sv->aux2));
                // A value stored and otherwise only tested for zero by the
                // block's branch, the store last before it, branches on the
                // update's own flags.
                i32 flags = -1;
                if (l->uses[y] == 2 && f->blocks[b].term.kind == TermIf) {
                    const i32 c = f->blocks[b].term.value;
                    bool zero = c == y;
                    bool last = true;
                    if (c != y) {
                        const Value* cv = &f->values[c];
                        Span<i32> cargs = functionArgs(f, c);
                        zero = cv->op == OpEqz ? cargs[0] == y : (cv->op == OpEq || cv->op == OpNe) && ((cargs[0] == y && lowererIsConst(l, cargs[1]) && lowererConstBits(l, cargs[1]) == 0) || (cargs[1] == y && lowererIsConst(l, cargs[0]) && lowererConstBits(l, cargs[0]) == 0));
                        last = cv->block == b && l->uses[c] == 1 && l->deferred[c];
                    }
                    for (Int i = s + 1; last && i < length(instrs); ++i) {
                        last = instrs[i] == c;
                    }
                    if (zero && last) {
                        flags = c;
                    }
                }
                if (yv->block != b || (l->uses[y] != 1 && flags < 0) || isFloat(yv->typ) || width != Int(typeSize(yv->typ)) || (width != 4 && width != 8)) {
                    continue;
                }
                if (yv->op != OpAdd && yv->op != OpSub && yv->op != OpAnd && yv->op != OpOr && yv->op != OpXor) {
                    continue;
                }
                Span<i32> yargs = functionArgs(f, y);
                i32 load = -1;
                for (Int i = 0; i < 2 && load < 0; ++i) {
                    if (i == 1 && yv->op == OpSub) {
                        break;
                    }
                    const Value* lv = &f->values[yargs[i]];
                    if (lv->op == OpLoad && functionArgs(f, yargs[i])[0] == addr && lv->aux == sv->aux && memKindWidth(convert<MemKind>(lv->aux2)) == width && yargs[1 - i] != yargs[i]) {
                        load = yargs[i];
                    }
                }
                if (load < 0) {
                    continue;
                }
                const i32 lb = f->values[load].block;
                bool ok = true;
                Int from = 0;
                if (lb == b) {
                    from = -1;
                    for (Int i = 0; i < s; ++i) {
                        if (instrs[i] == load) {
                            from = i + 1;
                        }
                    }
                    ok = from >= 0;
                } else {
                    const auto preds = f->blocks[b].preds;
                    ok = length(preds) == 1 && preds[0] == lb;
                    if (ok) {
                        auto before = f->blocks[lb].instrs.view();
                        Int at = -1;
                        for (Int i = 0; i < length(before); ++i) {
                            if (before[i] == load) {
                                at = i;
                            }
                        }
                        ok = at >= 0;
                        for (Int i = at + 1; ok && i < length(before); ++i) {
                            ok = lowererTransparent(f->values[before[i]].op);
                        }
                    }
                }
                for (Int i = from; ok && i < s; ++i) {
                    ok = instrs[i] == y || lowererTransparent(f->values[instrs[i]].op);
                }
                if (!ok) {
                    continue;
                }
                l->rmw[st] = y;
                l->deferred[y] = true;
                if (flags >= 0) {
                    l->rmwFlags[flags] = true;
                }
                if (lb == b && l->uses[load] == 1) {
                    l->deferred[load] = true;
                }
            }
        }
    }

    // transparent tells whether a load may move past a value of op: it
    // writes no memory and traps, if at all, as a load does.
    static bool lowererTransparent(Op op) {
        switch (op) {
            case OpDivS:
            case OpDivU:
            case OpRemS:
            case OpRemU:
            case OpTruncS:
            case OpTruncU:
            case OpStore:
            case OpMemGrow:
            case OpMemCopy:
            case OpMemFill:
            case OpMemInit:
            case OpDataDrop:
            case OpGlobalSet:
            case OpCall:
            case OpCallIndirect:
            case OpTableGet:
            case OpTableSet:
            case OpTableGrow:
            case OpTableFill:
            case OpTableCopy:
            case OpTableInit:
            case OpElemDrop:
            case OpCopyForward:
            case OpInvalid: {
                return false;
            }
        }
        return true;
    }

    // markFoldable defers a load whose only use reads it once as an operand
    // an x86 instruction can take from memory: the second of a two-operand
    // integer or float operation, either of a commutative one, a compared
    // value, at the width it is used with, or a value compared with zero at
    // any width. Between the load and where its user is lowered, which for
    // a deferred user is the block's end, no value may write memory or trap
    // otherwise than a load: the load and its check move to the user.
    static void lowererMarkFoldable(Lowerer* l) {
        Function* f = l->f;
        auto order = l->ci->order;
        for (Int o = 0; o < length(order); ++o) {
            const i32 b = order[o];
            auto instrs = f->blocks[b].instrs.view();
            // A loaded value the block branches on is compared with zero
            // in memory, at any width.
            const Term* t = &f->blocks[b].term;
            if (t->kind == TermIf && f->values[t->value].op == OpLoad && f->values[t->value].block == b && l->uses[t->value] == 1 && !l->deferred[t->value]) {
                Int at = 0;
                while (at < length(instrs) && instrs[at] != t->value) {
                    ++at;
                }
                bool ok = at < length(instrs);
                for (Int i = at + 1; ok && i < length(instrs); ++i) {
                    ok = lowererTransparent(f->values[instrs[i]].op);
                }
                if (ok) {
                    l->deferred[t->value] = true;
                    l->foldable[t->value] = true;
                }
            }
            for (Int j = 0; j < length(instrs); ++j) {
                const i32 v = instrs[j];
                const Value* val = &f->values[v];
                if (val->op != OpLoad || l->uses[v] != 1 || l->deferred[v]) {
                    continue;
                }
                const Int width = memKindWidth(convert<MemKind>(val->aux2));
                if (width != 4 && width != 8) {
                    continue;
                }
                Int k = j + 1;
                bool clear = true;
                for (; k < length(instrs); ++k) {
                    const i32 u = instrs[k];
                    Span<i32> args = functionArgs(f, u);
                    bool uses = false;
                    for (Int i = 0; i < length(args); ++i) {
                        uses = uses || args[i] == v;
                    }
                    if (uses) {
                        break;
                    }
                    if (!lowererTransparent(f->values[u].op)) {
                        clear = false;
                        break;
                    }
                }
                if (!clear || k == length(instrs)) {
                    continue;
                }
                const i32 u = instrs[k];
                const Value* user = &f->values[u];
                Span<i32> args = functionArgs(f, u);
                if (length(args) < 1 || length(args) > 2) {
                    continue;
                }
                const bool second = length(args) == 2 && args[1] == v && args[0] != v;
                const bool first = args[0] == v && (length(args) == 1 || args[1] != v);
                const i32 other = length(args) == 2 ? args[second ? 0 : 1] : -1;
                const bool full = width == Int(typeSize(val->typ));
                const bool zero = other >= 0 && lowererIsConst(l, other) && lowererConstBits(l, other) == 0;
                bool ok = false;
                switch (user->op) {
                    case OpAdd:
                    case OpAnd:
                    case OpOr:
                    case OpXor: {
                        ok = full && !isFloat(val->typ) && (first || second) && !lowererIsConst(l, other);
                        break;
                    }
                    case OpSub: {
                        ok = full && !isFloat(val->typ) && second && !lowererIsConst(l, other);
                        break;
                    }
                    case OpEqz: {
                        ok = first;
                        break;
                    }
                    case OpEq:
                    case OpNe: {
                        ok = (first || second) && (full || zero);
                        break;
                    }
                    case OpLtS:
                    case OpLtU:
                    case OpGtS:
                    case OpGtU:
                    case OpLeS:
                    case OpLeU:
                    case OpGeS:
                    case OpGeU: {
                        ok = (first || second) && full;
                        break;
                    }
                    case OpFAdd:
                    case OpFMul: {
                        ok = full && (first || second) && !lowererIsConst(l, other);
                        break;
                    }
                    case OpFSub:
                    case OpFDiv: {
                        ok = full && second && !lowererIsConst(l, other);
                        break;
                    }
                }
                if (!ok) {
                    continue;
                }
                if (l->deferred[u]) {
                    for (Int i = k + 1; ok && i < length(instrs); ++i) {
                        ok = lowererTransparent(f->values[instrs[i]].op);
                    }
                }
                if (ok) {
                    l->deferred[v] = true;
                    l->foldable[v] = true;
                }
            }
        }
    }

    // foldedLoad returns the memory operand of a foldable load not yet
    // lowered, checking its address, and whether there is one.
    static Tuple2<Mem, bool> lowererFoldedLoad(Lowerer* l, i32 v) {
        if (!l->foldable[v] || !l->deferred[v]) {
            return {Mem{}, false};
        }
        l->deferred[v] = false;
        Value* val = &l->f->values[v];
        MemKind kind = convert<MemKind>(val->aux2);
        return {lowererAddress(l, functionArgs(l->f, v)[0], val->aux, memKindWidth(kind), MemKind(kind & memProven) != MemKind(0LL), false), true};
    }

    static void lowererMemAddr(Lowerer* l, VReg dst, VReg a) {
        MInst in = inst(mLea);
        in.dst = dst;
        in.mem = memIndexed(memReg, a, 0);
        in.size = 8;
        lowererEmit(l, in);
    }

    // The bulk memory instructions and the imports of lib/c's memcpy,
    // memmove and memset check their ranges and call lib/str's functions
    // with host addresses: memory.init's source is the segment's bytes in
    // the object. A constant length of inlineCopyMax bytes or less is loads
    // and stores instead, as a native compiler makes them.
    static constexpr i64 inlineCopyMax = 32;

    struct MemChunk {
        i64 at{};
        u8 width{};
    };

    // memChunks covers n bytes, 1 to inlineCopyMax, with at most four
    // accesses of up to eight bytes: whole eights, then the rest as one
    // access of its width, two and one for three, or an access of eight
    // (four below eight bytes) reaching the end over the previous.
    static Int memChunks(i64 n, Array<MemChunk, 5>* chunks) {
        Int count = 0;
        i64 at = 0;
        while (n - at >= 8) {
            (*chunks)[count++] = MemChunk{at, 8};
            at += 8;
        }
        i64 rest = n - at;
        if (rest == 1 || rest == 2 || rest == 4) {
            (*chunks)[count++] = MemChunk{at, convert<u8>(rest)};
        } else if (rest == 3) {
            (*chunks)[count++] = MemChunk{at, 2};
            (*chunks)[count++] = MemChunk{at + 2, 1};
        } else if (rest >= 5 && n >= 8) {
            (*chunks)[count++] = MemChunk{n - 8, 8};
        } else if (rest >= 5) {
            (*chunks)[count++] = MemChunk{0, 4};
            (*chunks)[count++] = MemChunk{n - 4, 4};
        }
        return count;
    }

    // constLength is a constant length argument of 1 to inlineCopyMax, or 0.
    static i64 lowererConstLength(Lowerer* l, i32 v) {
        if (!lowererIsConst(l, v)) {
            return 0;
        }
        i64 n = convert<i64>(convert<u32>(lowererConstBits(l, v)));
        return n >= 1 && n <= inlineCopyMax ? n : 0;
    }

    // inlineCopy copies a constant length as loads, all of them first, then
    // stores: memory.copy may overlap. The addresses are checked as any
    // access's; the last chunk reaches farthest, so its check, made first,
    // covers the others, and a trap precedes every store.
    static bool lowererInlineCopy(Lowerer* l, Span<const i32> args) {
        i64 n = lowererConstLength(l, args[2]);
        if (n == 0) {
            return false;
        }
        Array<MemChunk, 5> chunks{};
        Int count = memChunks(n, &chunks);
        Array<VReg, 5> loaded{};
        for (Int i = count - 1; i >= 0; --i) {
            MInst in = inst(mLoad);
            in.dst = mFuncNewReg(l->mf, classGPR);
            in.mem = lowererAddress(l, args[1], chunks[i].at, chunks[i].width, false, false);
            in.size = chunks[i].width;
            in.dsize = 8;
            lowererEmit(l, in);
            loaded[i] = in.dst;
        }
        for (Int i = count - 1; i >= 0; --i) {
            MInst in = inst(mStore);
            in.mem = lowererAddress(l, args[0], chunks[i].at, chunks[i].width, false, true);
            in.src = loaded[i];
            in.size = chunks[i].width;
            lowererEmit(l, in);
        }
        return true;
    }

    // inlineFill stores a constant byte over a constant length, the
    // farthest chunk first as inlineCopy does.
    static bool lowererInlineFill(Lowerer* l, Span<const i32> args) {
        i64 n = lowererConstLength(l, args[2]);
        if (n == 0 || !lowererIsConst(l, args[1])) {
            return false;
        }
        i64 byte = lowererConstBits(l, args[1]) & 255;
        i64 pattern = convert<i64>(convert<u64>(byte) * 0x0101010101010101ULL);
        VReg value = byte == 0 ? noReg : lowererConstReg(l, pattern, 8);
        Array<MemChunk, 5> chunks{};
        Int count = memChunks(n, &chunks);
        for (Int i = count - 1; i >= 0; --i) {
            Mem mem = lowererAddress(l, args[0], chunks[i].at, chunks[i].width, false, true);
            if (value == noReg) {
                MInst in = inst(mStoreImm);
                in.mem = mem;
                in.imm = 0;
                in.size = chunks[i].width;
                lowererEmit(l, in);
            } else {
                MInst in = inst(mStore);
                in.mem = mem;
                in.src = value;
                in.size = chunks[i].width;
                lowererEmit(l, in);
            }
        }
        return true;
    }

    static void lowererMemCopy(Lowerer* l, Span<const i32> args, String sym) {
        if (lowererInlineCopy(l, args)) {
            return;
        }
        VReg d = lowererUse(l, args[0]);
        VReg s = lowererUse(l, args[1]);
        VReg n = lowererUse(l, args[2]);
        lowererCheckRange(l, d, n, memAt(ctxReg, l->lay->memoryEnd), 8, trapMemoryOOB);
        lowererCheckRange(l, s, n, memAt(ctxReg, l->lay->memoryEnd), 8, trapMemoryOOB);
        if (l->m->asan) {
            lowererAsanRange(l, d, n);
            lowererAsanRange(l, s, n);
        }
        VReg dst = mFuncNewReg(l->mf, classGPR);
        lowererMemAddr(l, dst, d);
        VReg src = mFuncNewReg(l->mf, classGPR);
        lowererMemAddr(l, src, s);
        lowererStrCall(l, sym, Span<const VReg>({dst, src, n}));
    }

    static void lowererMemFill(Lowerer* l, Span<const i32> args) {
        if (lowererInlineFill(l, args)) {
            return;
        }
        VReg d = lowererUse(l, args[0]);
        VReg val = lowererUse(l, args[1]);
        VReg n = lowererUse(l, args[2]);
        lowererCheckRange(l, d, n, memAt(ctxReg, l->lay->memoryEnd), 8, trapMemoryOOB);
        if (l->m->asan) {
            lowererAsanRange(l, d, n);
        }
        VReg dst = mFuncNewReg(l->mf, classGPR);
        lowererMemAddr(l, dst, d);
        lowererStrCall(l, symMemset, Span<const VReg>({dst, val, n}));
    }

    // intrinsic compiles a call of one of lib/c's functions lib/str has:
    // memcpy's, memmove's and memset's result is the destination.
    static void lowererIntrinsic(Lowerer* l, i32 v, Int intrinsic, Span<const i32> args) {
        switch (intrinsic) {
            case intrinsicMemcmp: {
                lowererMemCompare(l, v, args);
                return;
            }
            case intrinsicMemchr: {
                lowererMemFind(l, v, args);
                return;
            }
            case intrinsicStrlen: {
                VReg n = lowererStrLength(l, lowererUse(l, args[0]));
                if (v >= 0 && l->uses[v] > 0) {
                    lowererMov(l, lowererDef(l, v), n, 4);
                }
                return;
            }
            case intrinsicStrcpy:
            case intrinsicStrcat: {
                lowererStrCopy(l, v, args, intrinsic == intrinsicStrcat);
                return;
            }
            case intrinsicStrcmp: {
                lowererStrPair(l, v, args, symStrcmp);
                return;
            }
            case intrinsicStrspn: {
                lowererStrPair(l, v, args, symStrspn);
                return;
            }
            case intrinsicStrcspn: {
                lowererStrPair(l, v, args, symStrcspn);
                return;
            }
        }
        if (intrinsic == intrinsicMemset) {
            lowererMemFill(l, args);
        } else {
            lowererMemCopy(l, args, intrinsic == intrinsicMemcpy ? symMemcpy : symMemmove);
        }
        if (v >= 0 && l->uses[v] > 0) {
            lowererMov(l, lowererDef(l, v), lowererUse(l, args[0]), 4);
        }
    }

    // strCall calls the function of lib/str sym names with up to three
    // arguments in AX, CX and DX, as the module's functions call each other:
    // it changes what such a call may, and the instance and the memory base
    // stay. What it returns is in AX after it.
    static void lowererStrCall(Lowerer* l, String sym, Span<const VReg> args) {
        const VReg regs[] = {RAX, RCX, RDX};
        Slice<Move> moves = {};
        Slice<VReg> uses = {};
        for (Int i = 0; i < length(args); ++i) {
            moves = appendValues(moves, Move{regs[i], args[i]});
            uses = appendValues(uses, regs[i]);
        }
        MInst pm = inst(mParMove);
        mInstExtra(&pm)->moves.assign(Span<const Move>(moves));
        lowererEmit(l, pm);
        MInst in = inst(mCall);
        in.aux = calleeStr;
        in.imm = convert<i64>(objCodeExtern(l->oc, sym));
        mInstExtra(&in)->uses.assign(Span<const VReg>(uses));
        l->mf->hasCalls = true;
        lowererEmit(l, in);
    }

    // strResult copies all of AX, what the function of lib/str just called
    // returned, into a register of its own.
    static VReg lowererStrResult(Lowerer* l) {
        VReg r = mFuncNewReg(l->mf, classGPR);
        lowererMov(l, r, RAX, 8);
        return r;
    }

    // hostAddress is the host address of the memory at a.
    static VReg lowererHostAddress(Lowerer* l, VReg a) {
        VReg h = mFuncNewReg(l->mf, classGPR);
        lowererMemAddr(l, h, a);
        return h;
    }

    // sum is a + b in 64 bits, in a register of its own.
    static VReg lowererSum(Lowerer* l, VReg a, VReg b) {
        VReg t = mFuncNewReg(l->mf, classGPR);
        lowererMov(l, t, a, 8);
        MInst add = inst(mAlu);
        add.dst = t;
        add.src = b;
        add.size = 8;
        add.aux = convert<u8>(aluAdd);
        lowererEmit(l, add);
        return t;
    }

    // checkEnd traps unless a + n, n a register or noReg for none, is below
    // the accessible end of the memory, the address of a byte of it, for cc
    // ccAE, or at most that end, the end of a range of it, for ccA.
    static void lowererCheckEnd(Lowerer* l, VReg a, VReg n, u8 cc) {
        VReg t = a;
        if (n != noReg) {
            t = lowererSum(l, a, n);
        }
        MInst c = inst(mCmpMem);
        c.src = t;
        c.mem = memAt(ctxReg, l->lay->memoryEnd);
        c.size = 8;
        lowererEmit(l, c);
        lowererTrapIf(l, cc, trapMemoryOOB);
    }

    // strLength is the length of the string at s, its NUL within the memory
    // as the kernel's strlen has it: s is within the memory before the call
    // and s + n after it. Past the memory's end the loader keeps zeros, so
    // lib/str's strlen stops there when the string does not.
    static VReg lowererStrLength(Lowerer* l, VReg s) {
        lowererCheckEnd(l, s, noReg, ccAE);
        lowererStrCall(l, symStrlen, Span<const VReg>({lowererHostAddress(l, s)}));
        VReg n = lowererStrResult(l);
        lowererCheckEnd(l, s, n, ccAE);
        if (l->m->asan) {
            // The string and its NUL.
            VReg size = mFuncNewReg(l->mf, classGPR);
            lowererMov(l, size, n, 8);
            MInst nul = inst(mAluImm);
            nul.dst = size;
            nul.imm = 1;
            nul.size = 8;
            nul.aux = convert<u8>(aluAdd);
            lowererEmit(l, nul);
            lowererAsanRange(l, s, size);
        }
        return n;
    }

    // memCompare compiles memcmp: both ranges within the memory, the result
    // -1, 0 or 1 as the kernel's, whatever lib/str's configuration returns of
    // the same sign.
    static void lowererMemCompare(Lowerer* l, i32 v, Span<const i32> args) {
        VReg a = lowererUse(l, args[0]);
        VReg b = lowererUse(l, args[1]);
        VReg n = lowererUse(l, args[2]);
        const Mem end = memAt(ctxReg, l->lay->memoryEnd);
        lowererCheckRange(l, a, n, end, 8, trapMemoryOOB);
        lowererCheckRange(l, b, n, end, 8, trapMemoryOOB);
        if (l->m->asan) {
            lowererAsanRange(l, a, n);
            lowererAsanRange(l, b, n);
        }
        lowererStrCall(l, symMemcmp, Span<const VReg>({lowererHostAddress(l, a), lowererHostAddress(l, b), n}));
        lowererSign(l, v);
    }

    // sign makes v, if used, -1, 0 or 1 by the sign of the int lib/str's
    // function just returned.
    static void lowererSign(Lowerer* l, i32 v) {
        if (v < 0 || l->uses[v] == 0) {
            return;
        }
        VReg x = lowererStrResult(l);
        MInst t = inst(mTest);
        t.src = x;
        t.src2 = x;
        t.size = 4;
        lowererEmit(l, t);
        VReg d = lowererDef(l, v);
        MInst above = inst(mSetcc);
        above.dst = d;
        above.cc = ccG;
        lowererEmit(l, above);
        VReg below = mFuncNewReg(l->mf, classGPR);
        MInst lt = inst(mSetcc);
        lt.dst = below;
        lt.cc = ccL;
        lowererEmit(l, lt);
        MInst sub = inst(mAlu);
        sub.dst = d;
        sub.src = below;
        sub.size = 4;
        sub.aux = convert<u8>(aluSub);
        lowererEmit(l, sub);
    }

    // memFind compiles memchr as the kernel's: it searches up to n bytes or
    // to the memory's end, whichever is nearer, and a search that found
    // nothing short of n bytes traps. The result is the address found, or 0.
    static void lowererMemFind(Lowerer* l, i32 v, Span<const i32> args) {
        VReg s = lowererUse(l, args[0]);
        VReg c = lowererUse(l, args[1]);
        VReg n = lowererUse(l, args[2]);
        lowererCheckEnd(l, s, noReg, ccA);
        VReg room = mFuncNewReg(l->mf, classGPR);
        MInst ld = inst(mLoad);
        ld.dst = room;
        ld.mem = memAt(ctxReg, l->lay->memoryEnd);
        ld.size = 8;
        ld.dsize = 8;
        lowererEmit(l, ld);
        MInst sub = inst(mAlu);
        sub.dst = room;
        sub.src = s;
        sub.size = 8;
        sub.aux = convert<u8>(aluSub);
        lowererEmit(l, sub);
        VReg m = mFuncNewReg(l->mf, classGPR);
        lowererMov(l, m, n, 8);
        MInst cmp = inst(mCmp);
        cmp.src = m;
        cmp.src2 = room;
        cmp.size = 8;
        lowererEmit(l, cmp);
        MInst least = inst(mCmov);
        least.dst = m;
        least.src = room;
        least.cc = ccA;
        least.size = 8;
        lowererEmit(l, least);
        lowererStrCall(l, symMemchr, Span<const VReg>({lowererHostAddress(l, s), c, m}));
        VReg p = lowererStrResult(l);
        if (l->m->asan) {
            // The bytes it read: up to the one found, or all m of them.
            VReg read = mFuncNewReg(l->mf, classGPR);
            lowererMov(l, read, p, 8);
            MInst host = inst(mAlu);
            host.dst = read;
            host.src = memReg;
            host.size = 8;
            host.aux = convert<u8>(aluSub);
            lowererEmit(l, host);
            MInst start = inst(mAlu);
            start.dst = read;
            start.src = s;
            start.size = 8;
            start.aux = convert<u8>(aluSub);
            lowererEmit(l, start);
            MInst last = inst(mAluImm);
            last.dst = read;
            last.imm = 1;
            last.size = 8;
            last.aux = convert<u8>(aluAdd);
            lowererEmit(l, last);
            MInst any = inst(mTest);
            any.src = p;
            any.src2 = p;
            any.size = 8;
            lowererEmit(l, any);
            MInst all = inst(mCmov);
            all.dst = read;
            all.src = m;
            all.cc = ccE;
            all.size = 8;
            lowererEmit(l, all);
            lowererAsanRange(l, s, read);
        }
        // x is n when the byte was found and m when not: below n it trapped
        VReg x = mFuncNewReg(l->mf, classGPR);
        lowererMov(l, x, n, 8);
        MInst found = inst(mTest);
        found.src = p;
        found.src2 = p;
        found.size = 8;
        lowererEmit(l, found);
        MInst shortened = inst(mCmov);
        shortened.dst = x;
        shortened.src = m;
        shortened.cc = ccE;
        shortened.size = 8;
        lowererEmit(l, shortened);
        MInst cut = inst(mCmp);
        cut.src = x;
        cut.src2 = n;
        cut.size = 8;
        lowererEmit(l, cut);
        lowererTrapIf(l, ccB, trapMemoryOOB);
        if (v < 0 || l->uses[v] == 0) {
            return;
        }
        VReg r = mFuncNewReg(l->mf, classGPR);
        lowererMov(l, r, p, 8);
        MInst off = inst(mAlu);
        off.dst = r;
        off.src = memReg;
        off.size = 8;
        off.aux = convert<u8>(aluSub);
        lowererEmit(l, off);
        MInst none = inst(mTest);
        none.src = p;
        none.src2 = p;
        none.size = 8;
        lowererEmit(l, none);
        MInst null = inst(mCmov);
        null.dst = r;
        null.src = p;
        null.cc = ccE;
        null.size = 8;
        lowererEmit(l, null);
        lowererMov(l, lowererDef(l, v), r, 4);
    }

    // strPair compiles strcmp, strspn and strcspn as the kernel's: both
    // strings within the memory, by strlen, before lib/str's function reads
    // them. strcmp's result is -1, 0 or 1, the others' a length.
    static void lowererStrPair(Lowerer* l, i32 v, Span<const i32> args, String sym) {
        VReg a = lowererUse(l, args[0]);
        VReg b = lowererUse(l, args[1]);
        lowererStrLength(l, a);
        lowererStrLength(l, b);
        lowererStrCall(l, sym, Span<const VReg>({lowererHostAddress(l, a), lowererHostAddress(l, b)}));
        if (sym == symStrcmp) {
            lowererSign(l, v);
        } else if (v >= 0 && l->uses[v] > 0) {
            lowererMov(l, lowererDef(l, v), RAX, 4);
        }
    }

    // strCopy compiles strcpy and strcat as the kernel's: the string at s,
    // its length from strlen, copied with its NUL to d, or for strcat to the
    // NUL of the string at d, the destination's range checked before.
    static void lowererStrCopy(Lowerer* l, i32 v, Span<const i32> args, bool append) {
        VReg d = lowererUse(l, args[0]);
        VReg s = lowererUse(l, args[1]);
        VReg at = d;
        if (append) {
            at = lowererSum(l, d, lowererStrLength(l, d));
        }
        VReg n = lowererStrLength(l, s);
        lowererCheckEnd(l, at, n, ccAE);
        VReg size = mFuncNewReg(l->mf, classGPR);
        lowererMov(l, size, n, 8);
        MInst nul = inst(mAluImm);
        nul.dst = size;
        nul.imm = 1;
        nul.size = 8;
        nul.aux = convert<u8>(aluAdd);
        lowererEmit(l, nul);
        if (l->m->asan) {
            lowererAsanRange(l, at, size);
        }
        lowererStrCall(l, symMemmove, Span<const VReg>({lowererHostAddress(l, at), lowererHostAddress(l, s), size}));
        if (v >= 0 && l->uses[v] > 0) {
            lowererMov(l, lowererDef(l, v), d, 4);
        }
    }

    static void lowererMemInit(Lowerer* l, Int seg, Span<const i32> args) {
        VReg d = lowererUse(l, args[0]);
        VReg s = lowererUse(l, args[1]);
        VReg n = lowererUse(l, args[2]);
        VReg lengthValue = lowererConstReg(l, convert<i64>(length(l->m->datas[seg].bytes)), 8);
        VReg zero = lowererConstReg(l, 0, 4);
        VReg flag = mFuncNewReg(l->mf, classGPR);
        MInst ld = inst(mLoad);
        ld.dst = flag;
        ld.mem = memAt(ctxReg, layoutDataDropped(l->lay, seg));
        ld.size = 1;
        ld.dsize = 4;
        lowererEmit(l, ld);
        MInst tst = inst(mTest);
        tst.src = flag;
        tst.src2 = flag;
        tst.size = 4;
        lowererEmit(l, tst);
        MInst cm = inst(mCmov);
        cm.dst = lengthValue;
        cm.src = zero;
        cm.cc = ccNE;
        cm.size = 8;
        lowererEmit(l, cm);
        VReg t = mFuncNewReg(l->mf, classGPR);
        lowererMov(l, t, s, 8);
        MInst add = inst(mAlu);
        add.dst = t;
        add.src = n;
        add.size = 8;
        add.aux = 0;
        lowererEmit(l, add);
        MInst c = inst(mCmp);
        c.src = t;
        c.src2 = lengthValue;
        c.size = 8;
        lowererEmit(l, c);
        lowererTrapIf(l, ccA, trapMemoryOOB);
        lowererCheckRange(l, d, n, memAt(ctxReg, l->lay->memoryEnd), 8, trapMemoryOOB);
        if (l->m->asan) {
            lowererAsanRange(l, d, n);
        }
        VReg dst = mFuncNewReg(l->mf, classGPR);
        lowererMemAddr(l, dst, d);
        VReg src = mFuncNewReg(l->mf, classGPR);
        MInst lea = inst(mLea);
        lea.dst = src;
        lea.mem = Mem{noReg, noReg, {}, {}, dataLabel(seg)};
        lea.size = 8;
        lowererEmit(l, lea);
        MInst addS = inst(mAlu);
        addS.dst = src;
        addS.src = s;
        addS.size = 8;
        addS.aux = 0;
        lowererEmit(l, addS);
        lowererStrCall(l, symMemmove, Span<const VReg>({dst, src, n}));
    }

    static void lowererMov(Lowerer* l, VReg dst, VReg src, u8 size) {
        MInst in = inst(mMov);
        if (vRegIsXMM(dst) || (!vRegIsPhys(dst) && mFuncRegClass(l->mf, dst) == classXMM)) {
            in.op = mFMov;
        }
        in.dst = dst;
        in.src = src;
        in.size = size;
        lowererEmit(l, in);
    }

    // outgoing makes room for a call's stack arguments and results.
    static void lowererOutgoing(Lowerer* l, Int stackArgs, Slice<VReg> regs) {
        Int n = stackArgs;
        for (Int i = 0; i < length(regs); ++i) {
            VReg r = regs[i];
            if (r == noReg) {
                n++;
            }
        }
        if (8 * n > l->mf->stackArgs) {
            l->mf->stackArgs = 8 * n;
        }
    }

    // placeArgs moves call arguments to their registers and stack slots and
    // returns the registers the call reads and the number of stack slots.
    static Tuple2<Slice<VReg>, Int> lowererPlaceArgs(Lowerer* l, Span<const i32> args) {
        Slice<VReg> regs = makeSlice<VReg>(length(args));
        Slice<ValType> types = makeSlice<ValType>(length(args));
        for (Int i = 0; i < length(args); ++i) {
            i32 a = args[i];
            regs[i] = lowererUse(l, a);
            types[i] = l->f->values[a].typ;
        }
        return lowererPlaceRegs(l, regs, types);
    }

    // Nonzero float constants share a register at their common dominator,
    // outside a loop when needed, including uses in returns and edge copies.
    // Integer constants and zero remain cheap to rematerialize at each use.
    static void lowererPlaceFloatConstants(Lowerer* l) {
        Function* f = l->f;
        Slice<i32> homes = makeSlice<i32>(length(f->values));
        for (Int v = 0, end = length(homes); v < end; ++v) {
            homes[v] = -1;
        }
        auto use = [&](i32 b, i32 v) mutable -> void {
            Value* val = &f->values[v];
            if (val->op != OpConst || !isFloat(val->typ) || val->aux == 0 || l->uses[v] < 2) {
                return;
            }
            if (homes[v] < 0) {
                homes[v] = b;
            } else {
                homes[v] = cfgInfoIntersect(l->ci, homes[v], b);
            }
        };
        auto order = l->ci->order;
        for (Int i = 0; i < length(order); ++i) {
            i32 b = order[i];
            Block* blk = &f->blocks[b];
            auto instrs = blk->instrs.view();
            for (Int i = 0; i < length(instrs); ++i) {
                i32 v = instrs[i];
                auto args = functionArgs(f, v);
                for (Int i = 0; i < length(args); ++i) {
                    i32 a = args[i];
                    use(b, a);
                }
            }
            auto values = blk->term.values;
            for (Int i = 0; i < length(values); ++i) {
                i32 v = values[i];
                use(b, v);
            }
            auto edges = blk->term.edges;
            for (Int i = 0; i < length(edges); ++i) {
                Edge e = edges[i];
                auto args = e.args;
                for (Int i = 0; i < length(args); ++i) {
                    i32 v = args[i];
                    use(b, v);
                }
            }
        }
        l->floatHomes = makeSlice<Slice<i32>>(length(f->blocks));
        auto range872 = homes;
        for (Int index873 = 0, rangeEnd874 = length(range872); index873 < rangeEnd874; ++index873) {
            Int v = index873;
            i32 b = range872[index873];
            if (b < 0) {
                continue;
            }
            while (l->ci->depth[b] > i32(0LL) && l->ci->idom[b] != b) {
                b = l->ci->idom[b];
            }
            l->floatHomes[b] = appendValues(l->floatHomes[b], convert<i32>(v));
        }
    }

    // placeRegs moves argument registers to their argument registers and stack
    // slots and returns the fixed registers the call reads and the number of
    // stack slots.
    static Tuple2<Slice<VReg>, Int> lowererPlaceRegs(Lowerer* l, Slice<VReg> regs, Slice<ValType> types) {
        Slice<VReg> uses = {};
        Int gi = 0;
        Int xi = 0;
        Int stack = 0;
        MInst pm = inst(mParMove);
        for (Int i = 0; i < length(regs); ++i) {
            VReg r = regs[i];
            ValType t = types[i];
            if (isFloat(t)) {
                if (xi < length(argXMMs)) {
                    mInstExtra(&pm)->moves.pushBack(Move{argXMMs[xi], r});
                    uses = appendValues(uses, argXMMs[xi]);
                    xi++;
                    continue;
                }
            } else if (gi < length(argGPRs)) {
                mInstExtra(&pm)->moves.pushBack(Move{argGPRs[gi], r});
                uses = appendValues(uses, argGPRs[gi]);
                gi++;
                continue;
            }
            MInst in = inst(mStackArg);
            in.src = r;
            in.imm = convert<i64>(stack);
            in.size = typeSize(t);
            lowererEmit(l, in);
            stack++;
        }
        if (length(mInstMoves(&pm)) > 0) {
            lowererEmit(l, pm);
        }
        return {uses, stack};
    }

    static void lowererRotate(Lowerer* l, i32 v, Op op, i32 a, i32 b, u8 size) {
        VReg d = lowererDef(l, v);
        u8 kind = 0;
        if (op == OpRotr) {
            kind = shiftRor;
        }
        if (lowererIsConst(l, b)) {
            i64 n = lowererConstBits(l, b) & convert<i64>(u8(u8(size * 8) - 1));
            lowererMov(l, d, lowererUse(l, a), size);
            if (n != 0) {
                MInst in = inst(mShiftImm);
                in.dst = d;
                in.imm = n;
                in.size = size;
                in.aux = kind;
                lowererEmit(l, in);
            }
            return;
        }
        VReg ra = lowererUse(l, a);
        VReg rb = lowererUse(l, b);
        lowererMov(l, d, ra, size);
        lowererMov(l, RCX, rb, size);
        MInst in = inst(mRotCL);
        in.dst = d;
        in.src = RCX;
        in.size = size;
        in.aux = kind;
        lowererEmit(l, in);
    }

    static bool lowererScaledIndex(Lowerer* l, i32 v, i32 block) {
        Function* f = l->f;
        Value* val = &f->values[v];
        if (val->op != OpShl || val->block != block || l->uses[v] != 1) {
            return false;
        }
        i32 count = functionArgs(f, v)[1];
        return lowererIsConst(l, count) && (lowererConstBits(l, count) & convert<i64>(u8(u8(typeSize(val->typ) * 8) - 1))) <= 3;
    }

    static void lowererSelectValue(Lowerer* l, i32 v, Span<const i32> args) {
        Function* f = l->f;
        i32 cond = args[0];
        i32 x = args[1];
        i32 y = args[2];
        ValType t = f->values[v].typ;
        u8 size = typeSize(t);
        if (isFloat(t)) {
            if (l->deferred[cond] && f->values[cond].op >= OpFEq && f->values[cond].op <= OpFGe) {
                Span<i32> cmp = functionArgs(f, cond);
                const Op cop = f->values[cond].op;
                const bool direct = cmp[0] == x && cmp[1] == y;
                if ((cop == OpFLt || cop == OpFGt) && (direct || (cmp[0] == y && cmp[1] == x)) && typeSize(f->values[cmp[0]].typ) == size) {
                    // x < y ? x : y is minss x, y, and x > y ? x : y maxss
                    // x, y, exactly: a false comparison, with a NaN or
                    // equal operands, gives y in both.
                    MInst in = inst(mFAlu);
                    in.dst = lowererDef(l, v);
                    in.src = lowererUse(l, x);
                    in.src2 = lowererUse(l, y);
                    in.size = size;
                    in.aux = convert<u8>((cop == OpFLt) == direct ? fMin : fMax);
                    lowererEmit(l, in);
                    return;
                }
                MInst in = inst(mFSelect);
                in.dst = lowererDef(l, v);
                in.src = lowererUse(l, x);
                in.src2 = lowererUse(l, y);
                mInstExtra(&in)->uses.assign(Span<const VReg>({lowererUse(l, cmp[0]), lowererUse(l, cmp[1])}));
                mInstExtra(&in)->temps.assign(Span<const VReg>({mFuncNewReg(l->mf, classXMM)}));
                in.aux = convert<u8>(Op(f->values[cond].op - OpFEq));
                in.size = size;
                in.dsize = typeSize(f->values[cmp[0]].typ);
                lowererEmit(l, in);
                return;
            }
            VReg gx = lowererFloatBits(l, x, size);
            VReg gy = lowererFloatBits(l, y, size);
            u8 cc = 5;
            if (l->deferred[cond]) {
                Int parity = {};
                auto temporary884 = lowererCompare(l, cond);
                cc = temporary884.v0;
                parity = temporary884.v1;
                if (parity != 0) {
                    compilerError(String("internal error: float comparison deferred to select"));
                }
            } else {
                VReg c = lowererUse(l, cond);
                MInst tst = inst(mTest);
                tst.src = c;
                tst.src2 = c;
                tst.size = 4;
                lowererEmit(l, tst);
            }
            MInst cm = inst(mCmov);
            cm.dst = gy;
            cm.src = gx;
            cm.cc = cc;
            cm.size = size;
            lowererEmit(l, cm);
            MInst back = inst(mMovGX);
            back.dst = lowererDef(l, v);
            back.src = gy;
            back.size = size;
            lowererEmit(l, back);
            return;
        }
        VReg rx = lowererUse(l, x);
        VReg ry = lowererUse(l, y);
        VReg d = lowererDef(l, v);
        u8 cc = 5;
        Int parity = 0;
        if (l->deferred[cond]) {
            auto temporary885 = lowererCompare(l, cond);
            cc = temporary885.v0;
            parity = temporary885.v1;
        } else {
            VReg c = lowererUse(l, cond);
            MInst tst = inst(mTest);
            tst.src = c;
            tst.src2 = c;
            tst.size = 4;
            lowererEmit(l, tst);
        }
        // The conditional move overwrites a copy of one value with the
        // other: of the value used only here, if the other is used again,
        // so that the copy is the value's own register.
        if (parity == 0 && l->uses[x] == 1 && l->uses[y] > 1) {
            auto swapped = rx;
            rx = ry;
            ry = swapped;
            cc = convert<u8>(cc ^ 1);
        }
        lowererMov(l, d, ry, size);
        MInst cm = inst(mCmov);
        cm.dst = d;
        cm.src = rx;
        cm.cc = cc;
        cm.size = size;
        if (size < 4) {
            cm.size = 4;
        }
        lowererEmit(l, cm);
        if (parity != 0) {
            cm.cc = ccP;
            if (parity == 1) {
                cm.src = ry;
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
        in.dst = d;
        in.cc = cc;
        lowererEmit(l, in);
        if (parity == 0) {
            return;
        }
        VReg p = mFuncNewReg(l->mf, classGPR);
        MInst pi = inst(mSetcc);
        pi.dst = p;
        pi.cc = ccNP;
        AluOp op = aluAnd;
        if (parity == 2) {
            pi.cc = ccP;
            op = aluOr;
        }
        lowererEmit(l, pi);
        MInst a = inst(mAlu);
        a.dst = d;
        a.src = p;
        a.size = 4;
        a.aux = convert<u8>(op);
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
            i64 n = lowererConstBits(l, b) & convert<i64>(u8(u8(size * 8) - 1));
            lowererMov(l, d, lowererUse(l, a), size);
            if (n != 0) {
                MInst in = inst(mShiftImm);
                in.dst = d;
                in.imm = n;
                in.size = size;
                in.aux = kind;
                lowererEmit(l, in);
            }
            return;
        }
        MInst in = inst(mShiftX);
        in.dst = d;
        in.src = lowererUse(l, a);
        in.src2 = lowererUse(l, b);
        in.size = size;
        in.aux = kind;
        lowererEmit(l, in);
    }

    static void lowererStore(Lowerer* l, i32 v, i32 addr, i32 value) {
        Value* val = &l->f->values[v];
        MemKind kind = convert<MemKind>(val->aux2);
        Mem mem = lowererAddress(l, addr, val->aux, memKindWidth(kind), MemKind(kind & memProven) != MemKind(0LL), true);
        u8 width = convert<u8>(memKindWidth(kind));
        if (l->rmw[v] >= 0) {
            const i32 y = l->rmw[v];
            Value* yv = &l->f->values[y];
            Span<i32> yargs = functionArgs(l->f, y);
            const Value* first = &l->f->values[yargs[0]];
            const bool loadFirst = first->op == OpLoad && functionArgs(l->f, yargs[0])[0] == addr && first->aux == val->aux;
            const i32 x = loadFirst ? yargs[1] : yargs[0];
            auto imm = lowererImm(l, x, width);
            MInst in = inst(imm.v1 ? mAluMemImm : mAluMemReg);
            in.mem = mem;
            if (imm.v1) {
                in.imm = imm.v0;
            } else {
                in.src = lowererUse(l, x);
            }
            in.size = width;
            in.aux = convert<u8>(aluOpcode(yv->op));
            lowererEmit(l, in);
            return;
        }
        ValType t = l->f->values[value].typ;
        if (isFloat(t) && lowererIsConst(l, value) && l->vregs[value] == noReg) {
            i64 bits = lowererConstBits(l, value);
            if (width == 4 || bits == convert<i64>(convert<i32>(bits))) {
                MInst in = inst(mStoreImm);
                in.mem = mem;
                in.imm = bits;
                in.size = width;
                lowererEmit(l, in);
                return;
            }
        }
        if (!isFloat(t)) {
            auto temporary887 = lowererImm(l, value, width);
            i64 x = temporary887.v0;
            bool ok = temporary887.v1;
            if (ok && width <= 8) {
                MInst in = inst(mStoreImm);
                in.mem = mem;
                in.imm = x;
                in.size = width;
                lowererEmit(l, in);
                return;
            }
        }
        MInst in = inst(mStore);
        if (isFloat(t)) {
            in.op = mFStore;
        }
        in.mem = mem;
        in.src = lowererUse(l, value);
        in.size = width;
        lowererEmit(l, in);
    }

    static void lowererTableGet(Lowerer* l, i32 v, Int table, i32 index) {
        i32 tb = l->lay->tables[table];
        VReg ri = lowererUse(l, index);
        MInst c = inst(mCmpMem);
        c.src = ri;
        c.mem = memAt(ctxReg, tb + tableSize);
        c.size = 4;
        lowererEmit(l, c);
        lowererTrapIf(l, ccAE, trapTableOOB);
        VReg entries = mFuncNewReg(l->mf, classGPR);
        MInst ld = inst(mLoad);
        ld.dst = entries;
        ld.mem = memAt(ctxReg, tb + tableEntries);
        ld.size = 8;
        ld.dsize = 8;
        lowererEmit(l, ld);
        ld = inst(mLoad);
        ld.dst = lowererDef(l, v);
        ld.mem = Mem{entries, ri, u8(8LL), {}, -1};
        ld.size = 8;
        ld.dsize = 8;
        lowererEmit(l, ld);
    }

    static void lowererTableSet(Lowerer* l, Int table, i32 index, i32 value) {
        i32 tb = l->lay->tables[table];
        VReg ri = lowererUse(l, index);
        VReg rv = lowererUse(l, value);
        MInst c = inst(mCmpMem);
        c.src = ri;
        c.mem = memAt(ctxReg, tb + tableSize);
        c.size = 4;
        lowererEmit(l, c);
        lowererTrapIf(l, ccAE, trapTableOOB);
        VReg entries = mFuncNewReg(l->mf, classGPR);
        MInst ld = inst(mLoad);
        ld.dst = entries;
        ld.mem = memAt(ctxReg, tb + tableEntries);
        ld.size = 8;
        ld.dsize = 8;
        lowererEmit(l, ld);
        MInst st = inst(mStore);
        st.src = rv;
        st.mem = Mem{entries, ri, u8(8LL), {}, -1};
        st.size = 8;
        lowererEmit(l, st);
    }

    static MInst lowererTailCounter(Lowerer* l, i32 offset, i64 amount) {
        MInst in = inst(mCounter);
        in.mem = memAt(ctxReg, l->lay->tailProfile + convert<i32>(l->f->index) * tailCounterBytes + offset);
        in.imm = amount;
        return in;
    }

    // takeResults defines a call's results: from their registers, or from the
    // stack result slots after stackArgs argument slots.
    static void lowererTakeResults(Lowerer* l, i32 v, Slice<ValType> results, Slice<VReg> regs, Int stackArgs) {
        if (length(results) == 0) {
            return;
        }
        Slice<VReg> dsts = {};
        if (length(results) == 1) {
            dsts = sliceOf<VReg>({lowererDef(l, v)});
        } else {
            dsts = makeSlice<VReg>(length(results));
            for (Int i = 0, end = length(dsts); i < end; ++i) {
                dsts[i] = noReg;
            }
            auto extracts = lowererExtracts(l, v);
            for (Int i = 0; i < length(extracts); ++i) {
                i32 user = extracts[i];
                dsts[l->f->values[user].aux] = lowererDef(l, user);
            }
        }
        MInst pm = inst(mParMove);
        Int slot = stackArgs;
        auto range895 = dsts;
        for (Int index896 = 0, rangeEnd897 = length(range895); index896 < rangeEnd897; ++index896) {
            Int i = index896;
            VReg d = range895[index896];
            if (regs[i] == noReg) {
                if (d != noReg) {
                    MInst in = inst(mLoad);
                    if (isFloat(results[i])) {
                        in.op = mFLoad;
                    }
                    in.dst = d;
                    in.mem = memAt(RSP, convert<i32>(8 * slot));
                    in.size = typeSize(results[i]);
                    in.dsize = in.size;
                    lowererEmit(l, in);
                }
                slot++;
                continue;
            }
            if (d != noReg) {
                mInstExtra(&pm)->moves.pushBack(Move{d, regs[i]});
            }
        }
        if (length(mInstMoves(&pm)) > 0) {
            lowererEmit(l, pm);
        }
    }

    static void lowererTerminator(Lowerer* l, i32 b) {
        Term* t = &l->f->blocks[b].term;
        switch (t->kind) {
            case TermJump: {
                lowererEdgeMoves(l, &t->edges[0]);
                MInst j = inst(mJmp);
                j.target = l->mblk[t->edges[0].block];
                lowererEmit(l, j);
                break;
            }
            case TermIf: {
                i32 then = lowererEdgeBlock(l, b, 0);
                i32 els = lowererEdgeBlock(l, b, 1);
                if (l->deferred[t->value] && lowererBranchPair(l, t->value)) {
                    // a && b leaves for the else when a fails, a || b for the
                    // then when a holds; b decides the rest.
                    const bool conjunction = l->f->values[t->value].op == OpAnd;
                    Span<i32> args = functionArgs(l->f, t->value);
                    MInst j = inst(mJcc);
                    const u8 first = lowererCompare(l, args[0]).v0;
                    j.cc = conjunction ? convert<u8>(first ^ 1) : first;
                    j.target = conjunction ? els : then;
                    lowererEmit(l, j);
                    j = inst(mJcc);
                    j.cc = lowererCompare(l, args[1]).v0;
                    j.target = then;
                    lowererEmit(l, j);
                    j = inst(mJmp);
                    j.target = els;
                    lowererEmit(l, j);
                    break;
                }
                u8 cc = {};
                Int parity = 0;
                if (l->rmwFlags[t->value]) {
                    const Op op = l->f->values[t->value].op;
                    cc = op == OpEqz || op == OpEq ? ccE : ccNE;
                } else if (l->foldable[t->value] && l->deferred[t->value] && l->f->values[t->value].op == OpLoad) {
                    MInst in = inst(mCmpMemImm);
                    in.size = convert<u8>(memKindWidth(convert<MemKind>(l->f->values[t->value].aux2)));
                    in.mem = lowererFoldedLoad(l, t->value).v0;
                    in.imm = 0;
                    lowererEmit(l, in);
                    cc = ccNE;
                } else if (l->deferred[t->value]) {
                    auto temporary899 = lowererCompare(l, t->value);
                    cc = temporary899.v0;
                    parity = temporary899.v1;
                } else {
                    VReg c = lowererUse(l, t->value);
                    MInst tst = inst(mTest);
                    tst.src = c;
                    tst.src2 = c;
                    tst.size = 4;
                    lowererEmit(l, tst);
                    cc = ccNE;
                }
                switch (parity) {
                    case 1: {
                        MInst j = inst(mJcc);
                        j.cc = ccNE;
                        j.target = els;
                        lowererEmit(l, j);
                        j = inst(mJcc);
                        j.cc = ccP;
                        j.target = els;
                        lowererEmit(l, j);
                        j = inst(mJmp);
                        j.target = then;
                        lowererEmit(l, j);
                        break;
                    }
                    case 2: {
                        MInst j = inst(mJcc);
                        j.cc = ccNE;
                        j.target = then;
                        lowererEmit(l, j);
                        j = inst(mJcc);
                        j.cc = ccP;
                        j.target = then;
                        lowererEmit(l, j);
                        j = inst(mJmp);
                        j.target = els;
                        lowererEmit(l, j);
                        break;
                    }
                }
                if (parity != 0) {
                    break;
                }
                MInst j = inst(mJcc);
                j.cc = cc;
                j.target = then;
                lowererEmit(l, j);
                j = inst(mJmp);
                j.target = els;
                lowererEmit(l, j);
                break;
            }
            case TermSwitch: {
                MInst in = inst(mSwitch);
                in.src = lowererUse(l, t->value);
                mInstExtra(&in)->temps.assign(Span<const VReg>({mFuncNewReg(l->mf, classGPR)}));
                for (Int i = 0, end = length(t->edges); i < end; ++i) {
                    mInstExtra(&in)->table.pushBack(lowererEdgeBlock(l, b, i));
                }
                // An index the table covers needs no comparison with its size.
                if (isNil(l->addresses)) {
                    l->addresses = activePool->make<ValueRanges>(ValueRanges{l->f, makeSlice<ValueRange>(length(l->f->values)), makeSlice<bool>(length(l->f->values))});
                }
                const ValueRange range = valueRangesOf(l->addresses, t->value, 0);
                const bool covered = range.lo >= 0 && range.hi < length(t->edges) - 1;
                if (covered) {
                    in.aux = 2;
                }
                if (!covered && length(mInstTable(&in)) >= 5 && mInstTable(&in)[0] != mInstTable(&in)[length(mInstTable(&in)) - 1]) {
                    i32 dispatch = convert<i32>(length(l->mf->blocks));
                    i32 first = mInstTable(&in)[0];
                    mInstTable(&in)[0] = mInstTable(&in)[length(mInstTable(&in)) - 1];
                    in.aux = 1;
                    l->mf->blocks.pushBack(MBlock{Vec<MInst>({in}), {}, {}, l->mf->blocks[l->cur].depth, -1, {}});
                    MInst test = inst(mTest);
                    auto temporary904 = in.src;
                    auto temporary905 = in.src;
                    auto temporary906 = 4;
                    test.src = temporary904;
                    test.src2 = temporary905;
                    test.size = temporary906;
                    lowererEmit(l, test);
                    MInst j = inst(mJcc);
                    auto temporary907 = ccE;
                    auto temporary908 = first;
                    j.cc = temporary907;
                    j.target = temporary908;
                    lowererEmit(l, j);
                    j = inst(mJmp);
                    j.target = dispatch;
                    lowererEmit(l, j);
                    break;
                }
                lowererEmit(l, in);
                break;
            }
            case TermReturn: {
                Slice<ValType> results = l->f->typ->results;
                Slice<VReg> regs = resultRegs(results);
                MInst pm = inst(mParMove);
                Int slot = 0;
                auto values = t->values;
                for (Int i = 0; i < length(values); ++i) {
                    i32 v = values[i];
                    if (regs[i] != noReg) {
                        mInstExtra(&pm)->moves.pushBack(Move{regs[i], lowererUse(l, v)});
                        continue;
                    }
                    MInst in = inst(mStoreRet);
                    in.src = lowererUse(l, v);
                    in.imm = convert<i64>(slot);
                    in.size = typeSize(results[i]);
                    lowererEmit(l, in);
                    slot++;
                }
                if (length(mInstMoves(&pm)) > 0) {
                    lowererEmit(l, pm);
                }
                MInst in = inst(mRet);
                mInstExtra(&in)->uses.assign(assigned(regs));
                lowererEmit(l, in);
                break;
            }
            case TermTrap: {
                MInst in = inst(mTrap);
                in.imm = t->aux;
                lowererEmit(l, in);
                break;
            }
            default: {
                compilerError(String("internal error: block %d of function %d has no terminator"));
                break;
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
        in.size = typeSize(l->f->values[a].typ);
        in.src = lowererUse(l, a);
        auto temporary914 = lowererImm(l, b, in.size);
        i64 x = temporary914.v0;
        bool ok = temporary914.v1;
        if (ok) {
            in.op = mTestImm;
            in.imm = x;
            in.aux = 1;
        } else {
            in.src2 = lowererUse(l, b);
        }
        lowererEmit(l, in);
    }

    static void lowererTrapIf(Lowerer* l, u8 cc, i64 code) {
        MInst in = inst(mTrapIf);
        in.cc = cc;
        in.imm = code;
        lowererEmit(l, in);
    }

    // use returns a register holding v, materializing a constant.
    static VReg lowererUse(Lowerer* l, i32 v) {
        Value* val = &l->f->values[v];
        if (lowererIsConst(l, v)) {
            i64 bits = lowererConstBits(l, v);
            if (isFloat(val->typ)) {
                if (bits == 0) {
                    return VReg(31LL);
                }
                if (l->vregs[v] != noReg) {
                    return l->vregs[v];
                }
                VReg r = mFuncNewReg(l->mf, classXMM);
                MInst in = inst(mFConst);
                in.dst = r;
                in.imm = bits;
                in.size = typeSize(val->typ);
                lowererEmit(l, in);
                return r;
            }
            VReg r = mFuncNewReg(l->mf, classGPR);
            MInst in = inst(mMovImm);
            in.dst = r;
            in.imm = bits;
            in.size = typeSize(val->typ);
            if (val->typ == I32) {
                in.imm = convert<i64>(convert<u32>(bits));
            }
            lowererEmit(l, in);
            return r;
        }
        if (l->deferred[v]) {
            l->deferred[v] = false;
            lowererLowerValue(l, v);
        }
        if (l->vregs[v] == noReg) {
            compilerError(String("internal error: v%d used before definition in function %d"));
        }
        return l->vregs[v];
    }

    // In padded mode p <= memory.size guarantees the next boundSlack bytes
    // physically.
    // Clone only the overlapping spans from a group's first to last check.
    // The checked copy rejoins the shared continuation immediately after the
    // last check, before the final memory access. Calls and terminators outside
    // those spans are therefore never copied into a fallback.
    // The fast copy retains i32 address arithmetic and drops covered checks;
    // entry into the tail itself therefore remains supported by the cold copy.
    static void lowererVersionTailRanges(Lowerer* l) {
        Function* f = l->f;
        MFunc* mf = l->mf;
        AddressExpressions* ranges = activePool->make<AddressExpressions>(AddressExpressions{f, makeSlice<AddressExpression>(length(f->values)), makeSlice<bool>(length(f->values))});
        Int n = length(mf->blocks);
        bool changed = false;
        {
            Int b = 0;
            for (; b < n; b++) {
                struct BlockView {
                    Span<MInst> insts;
                    Slice<i32> succs;
                    Int depth;
                };

                BlockView original{mf->blocks[b].insts.view(), mf->blocks[b].succs, mf->blocks[b].depth};
                Map<Array<AddressTerm, 4>, Slice<TailRangeGroup*>> active = mapOf<Array<AddressTerm, 4>, Slice<TailRangeGroup*>>({});
                Slice<TailRangeGroup*> groups = {};
                auto range921 = original.insts;
                for (Int index922 = 0, rangeEnd923 = length(range921); index922 < rangeEnd923; ++index922) {
                    Int j = index922;
                    MInst in = range921[index922];
                    if (in.op != mCheck) {
                        switch (in.op) {
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
                        continue;
                    }
                    AddressExpression expr = addressExpressionsOf(ranges, in.addressValue, 0);
                    TailRangeGroup* group = {};
                    auto terms = active[expr.terms];
                    for (Int i = 0; i < length(terms); ++i) {
                        TailRangeGroup* prior = terms[i];
                        u64 delta = convert<u64>(expr.offset - prior->expr.offset);
                        if (delta + convert<u64>(in.imm) <= l->m->memoryTail) {
                            group = prior;
                            break;
                        }
                    }
                    if (isNil(group)) {
                        group = activePool->make<TailRangeGroup>(TailRangeGroup{j, in.src, expr, {}});
                        setMap(active, expr.terms, appendValues(active[expr.terms], group));
                        groups = appendValues(groups, group);
                    }
                    group->checks = appendValues(group->checks, j);
                }
                Slice<TailRangeGroup*> selected = subslice(groups, 0, 0);
                Slice<bool> skip = makeSlice<bool>(length(original.insts));
                auto range928 = groups;
                for (Int index929 = 0, rangeEnd930 = length(range928); index929 < rangeEnd930; ++index929) {
                    TailRangeGroup* group = range928[index929];
                    if (length(group->checks) < 2) {
                        continue;
                    }
                    selected = appendValues(selected, group);
                    auto checks = group->checks;
                    for (Int i = 0; i < length(checks); ++i) {
                        Int j = checks[i];
                        skip[j] = true;
                    }
                }
                if (length(selected) == 0) {
                    continue;
                }
                Vec<MInst> originalInsts = move(mf->blocks[b].insts);
                original.insts = originalInsts.view();
                changed = true;
                Slice<TailRangeRegion> regions = {};
                auto range934 = selected;
                for (Int index935 = 0, rangeEnd936 = length(range934); index935 < rangeEnd936; ++index935) {
                    TailRangeGroup* group = range934[index935];
                    Int end = group->checks[length(group->checks) - 1] + 1;
                    if (length(regions) == 0 || group->start > regions[length(regions) - 1].end) {
                        regions = appendValues(regions, TailRangeRegion{{}, group->start, end});
                    }
                    TailRangeRegion* region = &regions[length(regions) - 1];
                    region->end = maximum(region->end, end);
                    region->groups = appendValues(region->groups, group);
                }
                auto jump = [&](i32 target) mutable -> MInst {
                    MInst in = inst(mJmp);
                    in.target = target;
                    return in;
                };
                i32 continuation = convert<i32>(b);
                Int start = 0;
                auto range937 = regions;
                for (Int index938 = 0, rangeEnd939 = length(range937); index938 < rangeEnd939; ++index938) {
                    Int r = index938;
                    TailRangeRegion region = range937[index938];
                    Slice<i32> guards = makeSlice<i32>(length(region.groups));
                    Slice<i32> fast = makeSlice<i32>(length(region.groups));
                    Slice<i32> cold = makeSlice<i32>(length(region.groups));
                    for (Int j = 0, end = length(region.groups); j < end; ++j) {
                        guards[j] = convert<i32>(length(mf->blocks));
                        mf->blocks.pushBack(MBlock{{}, {}, {}, original.depth, -1, {}});
                        fast[j] = convert<i32>(length(mf->blocks));
                        mf->blocks.pushBack(MBlock{{}, {}, {}, original.depth, -1, {}});
                        cold[j] = convert<i32>(length(mf->blocks));
                        mf->blocks.pushBack(MBlock{{}, {}, {}, original.depth, -1, true});
                    }
                    Vec<MInst> prefix;
                    prefix.assign(subslice(original.insts, start, region.start));
                    prefix.pushBack(jump(guards[0]));
                    mf->blocks[continuation].insts = move(prefix);
                    mf->blocks[continuation].succs = sliceOf<i32>({guards[0]});
                    continuation = convert<i32>(length(mf->blocks));
                    mf->blocks.pushBack(MBlock{{}, {}, {}, original.depth, -1, {}});
                    auto range943 = region.groups;
                    for (Int index944 = 0, rangeEnd945 = length(range943); index944 < rangeEnd945; ++index944) {
                        Int j = index944;
                        TailRangeGroup* group = range943[index944];
                        Int end = region.end;
                        i32 nextFast = continuation;
                        i32 nextCold = continuation;
                        if (j + 1 < length(region.groups)) {
                            end = region.groups[j + 1]->start;
                            auto temporary948 = guards[j + 1];
                            auto temporary949 = cold[j + 1];
                            nextFast = temporary948;
                            nextCold = temporary949;
                        }
                        MInst cmp = inst(mCmpMem);
                        auto temporary950 = group->src;
                        auto temporary951 = memAt(ctxReg, offMemSize);
                        auto temporary952 = 8;
                        cmp.src = temporary950;
                        cmp.mem = temporary951;
                        cmp.size = temporary952;
                        MInst branch = inst(mJcc);
                        auto temporary953 = ccG;
                        auto temporary954 = cold[j];
                        branch.cc = temporary953;
                        branch.target = temporary954;
                        mf->blocks[guards[j]].insts.assign(Span<const MInst>({cmp, branch, jump(fast[j])}));
                        mf->blocks[guards[j]].succs = sliceOf<i32>({fast[j], cold[j]});
                        if (l->m->tailProfile) {
                            MInst counter = lowererTailCounter(l, 0, 1);
                            Vec<MInst> counted({counter});
                            counted.append(mf->blocks[guards[j]].insts.view());
                            mf->blocks[guards[j]].insts = move(counted);
                            i32 miss = convert<i32>(length(mf->blocks));
                            mf->blocks.pushBack(MBlock{Vec<MInst>({lowererTailCounter(l, 8, 1), jump(cold[j])}), sliceOf<i32>({cold[j]}), {}, original.depth, -1, true});
                            mf->blocks[guards[j]].insts[2].target = miss;
                            mf->blocks[guards[j]].succs[1] = miss;
                        }
                        Vec<MInst> fastBody;
                        Vec<MInst> coldBody;
                        i64 omitted = 0;
                        Int k = group->start;
                        for (; k < end; k++) {
                            MInst in = original.insts[k];
                            coldBody.pushBack(copyMachineInst(in));
                            if (skip[k]) {
                                omitted++;
                            } else {
                                fastBody.pushBack(copyMachineInst(in));
                            }
                        }
                        if (l->m->tailProfile) {
                            Vec<MInst> counted({lowererTailCounter(l, 16, omitted)});
                            counted.append(fastBody.view());
                            fastBody = move(counted);
                        }
                        fastBody.pushBack(jump(nextFast));
                        mf->blocks[fast[j]].insts = move(fastBody);
                        coldBody.pushBack(jump(nextCold));
                        mf->blocks[cold[j]].insts = move(coldBody);
                        mf->blocks[fast[j]].succs = sliceOf<i32>({nextFast});
                        mf->blocks[cold[j]].succs = sliceOf<i32>({nextCold});
                    }
                    start = region.end;
                    if (r + 1 == length(regions)) {
                        mf->blocks[continuation].insts.assign(subslice(original.insts, start, length(original.insts)));
                        mf->blocks[continuation].succs = appendSlice(Slice<i32>{}, original.succs);
                    }
                }
            }
        }
        if (changed) {
            for (Int b = 0, end = length(mf->blocks); b < end; ++b) {
                mf->blocks[b].preds = {};
            }
            for (Int b = 0, end = length(mf->blocks); b < end; ++b) {
                auto range961 = mf->blocks[b].succs;
                for (Int index962 = 0, rangeEnd963 = length(range961); index962 < rangeEnd963; ++index962) {
                    i32 succ = range961[index962];
                    mf->blocks[succ].preds = appendValues(mf->blocks[succ].preds, convert<i32>(b));
                }
            }
            Int n = length(mf->blocks);
            Int b = 0;
            for (; b < n; b++) {
                if (length(mf->blocks[b].succs) < 2) {
                    continue;
                }
                auto range964 = mf->blocks[b].succs;
                for (Int index965 = 0, rangeEnd966 = length(range964); index965 < rangeEnd966; ++index965) {
                    Int j = index965;
                    i32 succ = range964[index965];
                    if (length(mf->blocks[succ].preds) < 2) {
                        continue;
                    }
                    i32 edge = convert<i32>(length(mf->blocks));
                    MInst jump = inst(mJmp);
                    jump.target = succ;
                    mf->blocks.pushBack(MBlock{Vec<MInst>({jump}), sliceOf<i32>({succ}), sliceOf<i32>({convert<i32>(b)}), mf->blocks[b].depth, -1, mf->blocks[b].cold || mf->blocks[succ].cold});
                    mf->blocks[b].succs[j] = edge;
                    for (Int k = 0, end = length(mf->blocks[b].insts.view()); k < end; ++k) {
                        MInst* in = &mf->blocks[b].insts[k];
                        switch (in->op) {
                            case mJmp:
                            case mJcc: {
                                if (in->target == succ) {
                                    in->target = edge;
                                }
                                break;
                            }
                            case mSwitch: {
                                for (Int t = 0, end = length(mInstTable(in)); t < end; ++t) {
                                    if (mInstTable(in)[t] == succ) {
                                        mInstTable(in)[t] = edge;
                                    }
                                }
                                break;
                            }
                        }
                    }
                    auto preds = mf->blocks[succ].preds;
                    for (Int k = 0; k < length(preds); ++k) {
                        i32 pred = preds[k];
                        if (pred == convert<i32>(b)) {
                            mf->blocks[succ].preds[k] = edge;
                        }
                    }
                }
            }
        }
    }

    static void metaWriterFlag(MetaWriter* w, bool b) {
        if (b) {
            metaWriterU(w, 1);
        } else {
            metaWriterU(w, 0);
        }
    }

    static void metaWriterStr(MetaWriter* w, String s) {
        metaWriterU(w, convert<u64>(length(s)));
        w->out = appendSlice(w->out, s);
    }

    static void metaWriterU(MetaWriter* w, u64 x) {
        w->out = binaryAppendUvarint(w->out, x);
    }

    static i32 objCodeAddBlob(ObjCode* oc, Slice<u8> b, Int align) {
        if (b.tagged && b.array()->owner != oc->pool) {
            PoolScope persistent(*oc->pool);
            b = slicesClone(b);
        }
        oc->blobs = appendValues(oc->blobs, CodeBlob{b, align});
        return convert<i32>(length(oc->blobs) - 1);
    }

    static void objCodeAddFixedBlobs(ObjCode* oc) {
        auto mask = [&](u64 lo, u64 hi) mutable -> Slice<u8> {
            Slice<u8> b = makeSlice<u8>(16);
            littlePutUint64(b, lo);
            littlePutUint64(subslice(b, 8, length(b)), hi);
            return b;
        };
        objCodeAddBlob(oc, mask(u64(2147483648LL), u64(0LL)), 16);
        objCodeAddBlob(oc, mask(u64(2147483647LL), u64(0LL)), 16);
        objCodeAddBlob(oc, mask(u64(9223372036854775808ULL), u64(0LL)), 16);
        objCodeAddBlob(oc, mask(u64(9223372036854775807LL), u64(0LL)), 16);
        auto datas = oc->m->datas;
        for (Int i = 0; i < length(datas); ++i) {
            Data d = datas[i];
            objCodeAddBlob(oc, d.bytes, 1);
        }
    }

    static void objCodeAlign16(ObjCode* oc) {
        while (length(oc->a.buf) % Int(16LL) != Int(0LL)) {
            asmBytes(&oc->a, {u8(u8(204LL))});
        }
    }

    // beginRegion starts a code region at a 16-byte boundary; endRegion records
    // it with its stack pointer table.
    static i32 objCodeBeginRegion(ObjCode* oc, String name) {
        PoolScope persistent(*oc->pool);
        name = cloneString(name);
        objCodeAlign16(oc);
        asmSetSP(&oc->a, 0);
        oc->regionName = name;
        return asmPos(&oc->a);
    }

    static void objCodeCallFunc(ObjCode* oc, Int fn) {
        oc->calls = appendValues(oc->calls, CallFixup{asmCallRel(&oc->a), convert<i32>(fn)});
    }

    // callGo calls the Go function sym names, whose code address the loader
    // puts in the GOT.
    static void objCodeCallGo(ObjCode* oc, String sym) {
        asmCallMem(&oc->a, objCodeGotMem(oc, sym));
    }

    // description is what the symbols do not tell a loader: the instance, its
    // memory, tables and globals, the imports and exports of those.
    static Meta* objCodeDescription(ObjCode* oc) {
        Module* m = oc->m;
        Layout* lay = oc->lay;
        Meta* meta = activePool->make<Meta>(Meta{convert<u32>(lay->size), {}, {}, {}, {}, {}, {}, {}});
        if (length(m->mems) > 0) {
            Limits l = m->mems[0].limits;
            u64 max = 65536;
            if (l.hasMax) {
                max = l.max;
            }
            meta->memory = activePool->make<MemoryMeta>(MemoryMeta{l.min, max, m->mems[0].imported, m->memoryTail, {}, {}, {}});
            if (m->memoryTail != 0) {
                meta->memory->tailEnd = convert<u32>(lay->memoryEnd);
            }
            if (m->tailProfile) {
                meta->memory->profile = convert<u32>(lay->tailProfile);
                meta->memory->profileFuncs = convert<u32>(length(m->funcs));
            }
            if (m->asan) {
                meta->memory->shadow = convert<u32>(lay->shadow);
            }
        }
        auto tables = m->tables;
        for (Int i = 0; i < length(tables); ++i) {
            Table t = tables[i];
            u64 max = u64(4294967295LL);
            if (t.limits.hasMax) {
                max = t.limits.max;
            }
            meta->tables = appendValues(meta->tables, TableMeta{convert<u32>(lay->tables[i]), t.limits.min, max, t.imported});
        }
        auto globals = m->globals;
        for (Int i = 0; i < length(globals); ++i) {
            Global g = globals[i];
            meta->globals = appendValues(meta->globals, GlobalMeta{convert<u32>(lay->globals[i]), g.typ, g.mutableValue, g.imported});
        }
        auto imports = m->imports;
        for (Int i = 0; i < length(imports); ++i) {
            Import imp = imports[i];
            if (imp.kind != externFunc) {
                meta->imports = appendValues(meta->imports, ImportMeta{imp.kind, imp.module, imp.name, imp.index, {}, {}, {}});
            }
        }
        auto exports = m->exports;
        for (Int i = 0; i < length(exports); ++i) {
            Export e = exports[i];
            if (e.kind != externFunc) {
                meta->exports = appendValues(meta->exports, ExportMeta{e.kind, e.name, e.index, {}, {}, {}});
            }
        }
        objCodeLines(oc, meta);
        return meta;
    }

    // shortJumps chooses the jumps of an emission with every jump long that
    // can be short. Starting from all of them short, it drops those that
    // do not fit where the others' sizes and the padding of the aligned
    // loop headers put them, until all fit: the layout follows exactly
    // from the jumps' sizes and the headers' alignment.
    static bool objCodeShortJumps(FuncEmitter* e, Slice<u8> shorts, Slice<u8> trapShorts) {
        Asm* a = e->a;
        struct Site {
            i32 from;
            i32 size;
            i32 to;
            u8* plan;
        };
        Slice<Site> sites = makeSlice<Site>(0, length(e->jumps) + length(e->traps));
        for (Int k = 0; k < length(e->jumps); ++k) {
            const JumpFixup j = e->jumps[k];
            const bool conditional = a->buf[usize(j.at - 1)] != 233;
            sites = appendValues(sites, Site{j.at - (conditional ? 2 : 1), conditional ? 6 : 5, e->blockAt[j.block], &shorts[k]});
        }
        for (Int k = 0; k < length(e->traps); ++k) {
            const TrapFixup t = e->traps[k];
            const bool conditional = a->buf[usize(t.at - 1)] != 233;
            sites = appendValues(sites, Site{t.at - (conditional ? 2 : 1), conditional ? 6 : 5, e->trapTo[usize(k)], &trapShorts[k]});
        }
        if (!length(sites)) {
            return false;
        }
        sortValues(sites, [&](Int x, Int y) {
            return sites[x].from < sites[y].from;
        });
        // A jump can be short only when it fits with every jump between
        // its ends short and no padding there.
        for (Int k = 0; k < length(sites); ++k) {
            const i32 to = sites[k].to;
            const i32 lo = minimum(sites[k].from, to);
            const i32 hi = maximum(sites[k].from, to);
            const Int first = sortSearch(length(sites), [&](Int i) {
                return sites[i].from >= lo;
            });
            const Int last = sortSearch(length(sites), [&](Int i) {
                return sites[i].from >= hi;
            });
            i64 pads = 0;
            for (usize h = 0; h < e->aligned.size(); ++h) {
                if (e->aligned[h] > lo && e->aligned[h] <= hi) {
                    pads += e->padded[h];
                }
            }
            const i64 d = i64(to) - i64(sites[k].from + 2);
            const i64 room = 4 * (last - first) + pads;
            *sites[k].plan = d >= -128 - room && d <= 127 + room;
        }
        // The positions where the shift of later code changes, and the
        // shift from each on.
        Slice<i32> at = makeSlice<i32>(0, length(sites) + i64(e->aligned.size()));
        Slice<i32> shift = makeSlice<i32>(0, length(sites) + i64(e->aligned.size()));
        Slice<i32> from = makeSlice<i32>(length(sites));
        for (bool changed = true; changed;) {
            at = subslice(at, 0, 0);
            shift = subslice(shift, 0, 0);
            i32 delta = 0;
            usize h = 0;
            for (Int k = 0; k <= length(sites); ++k) {
                const i32 next = k < length(sites) ? sites[k].from : i32(2147483647);
                for (; h < e->aligned.size() && e->aligned[h] - e->padded[h] <= next; ++h) {
                    const i32 before = e->aligned[h] - e->padded[h] + delta;
                    delta += alignPad(before, e->spans[h]) - e->padded[h];
                    at = appendValues(at, e->aligned[h]);
                    shift = appendValues(shift, delta);
                }
                if (k == length(sites)) {
                    break;
                }
                from[k] = sites[k].from + delta;
                if (*sites[k].plan) {
                    delta -= sites[k].size - 2;
                    at = appendValues(at, sites[k].from + sites[k].size);
                    shift = appendValues(shift, delta);
                }
            }
            changed = false;
            for (Int k = 0; k < length(sites); ++k) {
                if (!*sites[k].plan) {
                    continue;
                }
                const i32 t = sites[k].to;
                const Int i = sortSearch(length(at), [&](Int i) {
                    return at[i] > t;
                });
                const i64 d = i64(t) + (i ? shift[i - 1] : 0) - i64(from[k] + 2);
                if (d < -128 || d > 127) {
                    *sites[k].plan = 0;
                    changed = true;
                }
            }
        }
        for (Int k = 0; k < length(sites); ++k) {
            if (*sites[k].plan) {
                return true;
            }
        }
        return false;
    }

    // emitFunction emits a function with every jump long, then, when some
    // can be short, again with those short.
    static void objCodeEmitFunction(ObjCode* oc, Int index, MFunc* mf, Allocator* al) {
        Asm* a = &oc->a;
        i32 start = objCodeBeginRegion(oc, objCodeFuncName(oc, index));
        oc->funcAt[index] = start;
        const Int fixups = length(a->fixups);
        const Int sps = length(a->sps);
        const SpEntry lastSp = sps ? a->sps[sps - 1] : SpEntry{};
        const i32 sp = a->sp;
        const Int lines = length(a->lines);
        a->lastPos = 0;
        const Int calls = length(oc->calls);
        const Int jumpCalls = length(oc->jumpCalls);
        const Int addresses = length(oc->addresses);
        FuncEmitter* e = objCodeEmitBody(oc, index, mf, al, {}, {}, {});
        Slice<u8> shorts = makeSlice<u8>(length(e->jumps));
        Slice<u8> trapShorts = makeSlice<u8>(length(e->traps));
        Slice<i32> plan = makeSlice<i32>(i64(e->spans.size()));
        bool realign = false;
        for (usize k = 0; k < e->spans.size(); ++k) {
            plan[i64(k)] = e->spans[k];
            realign = realign || alignPad(e->aligned[k] - e->padded[k], e->spans[k]) != e->padded[k];
        }
        if (objCodeShortJumps(e, shorts, trapShorts) || realign) {
            a->buf.resize(usize(start));
            a->fixups = subslice(a->fixups, 0, fixups);
            a->sps = subslice(a->sps, 0, sps);
            if (sps) {
                a->sps[sps - 1] = lastSp;
            }
            a->sp = sp;
            a->lines = subslice(a->lines, 0, lines);
            a->lastPos = 0;
            oc->calls = subslice(oc->calls, 0, calls);
            oc->jumpCalls = subslice(oc->jumpCalls, 0, jumpCalls);
            oc->addresses = subslice(oc->addresses, 0, addresses);
            objCodeEmitBody(oc, index, mf, al, shorts, trapShorts, plan);
        }
        objCodeEndRegion(oc, start);
    }

    static FuncEmitter* objCodeEmitBody(ObjCode* oc, Int index, MFunc* mf, Allocator* al, Slice<u8> shorts, Slice<u8> trapShorts, Slice<i32> plan) {
        Asm* a = &oc->a;
        FuncEmitter* e = activePool->make<FuncEmitter>(FuncEmitter{oc, a, mf, al, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}});
        e->shorts = shorts;
        e->trapShorts = trapShorts;
        e->plan = plan;
        if (oc->m->tailProfile) {
            e->profile = oc->lay->tailProfile + convert<i32>(index) * tailCounterBytes;
        }
        e->pushes = allocatorCalleeSavedUsed(al);
        e->frame = convert<i32>(mf->stackArgs) + 8 * al->nslots;
        // The stack check follows the pushes: the arguments fill the
        // caller-saved registers, so a large frame's check computes in a
        // callee-saved register the function saves, R12 when it saves none.
        // A function that a leaf's frame fits may check before its calls
        // instead, against the stack pointer the body has.
        Slice<i32> checkBlocks = {};
        if (mf->hasCalls && e->frame <= leafFrame && !mf->returnsTwice) {
            checkBlocks = objCodeStackCheckBlocks(mf);
        }
        Slice<bool> checksAt = makeSlice<bool>(length(mf->blocks));
        for (Int i = 0; i < length(checkBlocks); ++i) {
            checksAt[checkBlocks[i]] = true;
        }
        const bool checks = (mf->hasCalls && length(checkBlocks) == 0) || e->frame > leafFrame;
        if (checks && e->frame > stackSmall && length(e->pushes) == 0) {
            e->pushes = sliceOf<VReg>({R12});
        }
        const VReg checkTemp = length(e->pushes) ? e->pushes[0] : noReg;
        e->framed = mf->hasCalls || e->frame > 0 || length(e->pushes) > 0;
        e->body = 8 * convert<i32>(length(e->pushes)) + e->frame;
        if (e->framed) {
            e->body += 8;
        }
        if (mf->returnsTwice) {
            e->body += 8;
        }
        e->incoming = e->body + 8;
        if (e->framed) {
            asmPush(a, RBP);
            asmMovRR(a, 8, RBP, RSP);
        }
        if (mf->returnsTwice) {
            asmPushZero(a);
        }
        auto pushes = e->pushes;
        for (Int i = 0; i < length(pushes); ++i) {
            VReg r = pushes[i];
            asmPush(a, r);
        }
        const i32 retry = asmPos(a);
        i32 slow = -1;
        if (checks) {
            slow = objCodeStackCheck(oc, e->frame, checkTemp);
        }
        if (e->frame > 0) {
            asmAluRI(a, aluSub, 8, RSP, convert<i64>(e->frame));
        }
        e->blockAt = makeSlice<i32>(length(mf->blocks));
        for (Int i = 0, end = length(e->blockAt); i < end; ++i) {
            e->blockAt[i] = -1;
        }
        Span<i32> order = mFuncHotOrder(mf, al->order.view());
        Slice<Int> positions = makeSlice<Int>(length(mf->blocks));
        for (Int j = 0; j < length(order); ++j) {
            i32 b = order[j];
            positions[b] = j;
        }
        // A block of silent instructions and a jump is not emitted: jumps to
        // it go to its final target, unless such blocks form a cycle. Every
        // transfer in machine IR is explicit, so no block relies on falling
        // into it. Walking backwards past those and blocks with no code at
        // all gives each position the last one its end falls into.
        e->positions = positions;
        e->reach.resize(usize(length(order)));
        e->forward.resize(mf->blocks.size());
        e->forward.fill(-1);
        Vec<u8> empty;
        empty.resize(mf->blocks.size());
        empty.fill(0);
        Vec<i32> jumps;
        jumps.resize(mf->blocks.size());
        jumps.fill(-1);
        for (Int j = 0; j < length(order); ++j) {
            const auto insts = mf->blocks[order[j]].insts.view();
            Int k = 0;
            while (k < length(insts) && mInstSilent(&insts[k])) {
                ++k;
            }
            empty[usize(order[j])] = k == length(insts) && !checksAt[order[j]];
            if (k + 1 == length(insts) && insts[k].op == mJmp && !checksAt[order[j]]) {
                jumps[usize(order[j])] = insts[k].target;
            }
        }
        for (Int j = 0; j < length(order); ++j) {
            const i32 b = order[j];
            i32 x = b;
            for (Int hops = 0; jumps[usize(x)] >= 0 && hops < 16; ++hops) {
                x = jumps[usize(x)];
                if (x == b) {
                    break;
                }
            }
            if (jumps[usize(b)] >= 0 && jumps[usize(x)] < 0) {
                e->forward[usize(b)] = x;
            }
        }
        for (Int j = length(order) - 1; j >= 0; --j) {
            const i32 next = j + 1 < length(order) ? order[j + 1] : -1;
            const bool passes = next >= 0 && (empty[usize(next)] || e->forward[usize(next)] >= 0);
            e->reach[usize(j)] = next < 0 ? j : passes ? e->reach[usize(j + 1)] : j + 1;
        }
        // Loop headers are aligned; a back edge through a forwarding block
        // reaches its final target.
        Vec<u8> header;
        header.resize(mf->blocks.size());
        header.fill(0);
        Vec<i32> latch;
        latch.resize(mf->blocks.size());
        latch.fill(-1);
        for (Int j = 0; j < length(order); ++j) {
            const i32 b = order[j];
            const auto preds = mf->blocks[b].preds;
            for (Int k = 0; k < length(preds); ++k) {
                const i32 p = preds[k];
                const i32 to = funcEmitterTarget(e, b);
                if (!mf->blocks[to].cold && !mf->blocks[p].cold && positions[p] >= positions[to]) {
                    header[usize(to)] = 1;
                    if (latch[usize(to)] < 0 || positions[p] > positions[latch[usize(to)]]) {
                        latch[usize(to)] = p;
                    }
                }
            }
        }
        Vec<i32> blockEnd;
        blockEnd.resize(mf->blocks.size());
        blockEnd.fill(-1);
        Vec<i32> headers;
        Vec<Array<i32, 2>> bodyChecks;
        for (Int i = 0; i < length(order); ++i) {
            i32 b = order[i];
            asmSetSP(a, e->body);
            if (e->forward[usize(b)] >= 0) {
                continue;
            }
            if (header[usize(b)]) {
                const usize k = e->aligned.size();
                const i32 pad = alignPad(asmPos(a), k < usize(length(e->plan)) ? e->plan[k] : 0);
                asmNops(a, pad);
                e->aligned.pushBack(asmPos(a));
                e->padded.pushBack(pad);
                headers.pushBack(b);
            }
            e->blockAt[b] = asmPos(a);
            if (checksAt[b]) {
                const i32 at = asmPos(a);
                bodyChecks.pushBack(Array<i32, 2>{{objCodeStackCheck(oc, 0, noReg), at}});
            }
            funcEmitterBlock(e, &mf->blocks[b], i);
            blockEnd[usize(b)] = asmPos(a);
        }
        for (usize k = 0; k < headers.size(); ++k) {
            const i32 l = latch[usize(headers[k])];
            const i32 end = l >= 0 ? blockEnd[usize(l)] : -1;
            e->spans.pushBack(end > e->aligned[k] ? end - e->aligned[k] : 0);
        }
        Int i = 0;
        for (; i < length(e->late); i++) {
            asmSetSP(a, e->body);
            e->late[i]();
        }
        if (slow >= 0) {
            asmSetSP(a, e->body - e->frame);
            objCodeSlowPath(oc, slow, retry, e->frame, checkTemp);
        }
        for (usize k = 0; k < bodyChecks.size(); ++k) {
            asmSetSP(a, e->body);
            objCodeSlowPath(oc, bodyChecks[k][0], bodyChecks[k][1], 0, noReg);
        }
        Map<Array<i64, 2>, i32> stubs = mapOf<Array<i64, 2>, i32>({});
        auto traps = e->traps;
        for (Int i = 0; i < length(traps); ++i) {
            TrapFixup t = traps[i];
            Array<i64, 2> key = Array<i64, 2>{{t.code, convert<i64>(t.sp)}};
            auto temporary1010 = lookupMap(stubs, key);
            i32 at = temporary1010.v0;
            bool ok = temporary1010.v1;
            if (t.code == trapAsan) {
                // An asan trap reports its own access: no pad is shared.
                at = asmPos(a);
                asmMarkPos(a, t.pos);
                asmSetSP(a, t.sp);
                objCodeTrapAtCall(oc, t);
            } else if (!ok) {
                // A pad shared by traps is of the first one's instruction.
                at = asmPos(a);
                asmMarkPos(a, t.pos);
                setMap(stubs, key, at);
                asmSetSP(a, t.sp);
                objCodeTrapCall(oc, t.code);
            }
            e->trapTo.pushBack(at);
            if (t.small) {
                asmPatch8(a, t.at, at);
            } else {
                asmPatch32(a, t.at, at);
            }
        }
        auto range1011 = e->jumps;
        for (Int index1012 = 0, rangeEnd1013 = length(range1011); index1012 < rangeEnd1013; ++index1012) {
            JumpFixup j = range1011[index1012];
            if (j.small) {
                asmPatch8(a, j.at, e->blockAt[j.block]);
            } else {
                asmPatch32(a, j.at, e->blockAt[j.block]);
            }
        }
        auto tables = e->tables;
        // Copies of one dispatch share its table.
        Map<uptr, i32> bases = makeMap<uptr, i32>();
        for (Int i = 0; i < length(tables); ++i) {
            TableFixup t = tables[i];
            const uptr key = t.table.contiguous ? reinterpret_cast<uptr>(t.table.contiguous) : reinterpret_cast<uptr>(t.table.pages) + t.table.offset;
            auto known = lookupMap(bases, key);
            if (known.v1) {
                asmPatch32(a, t.lea, known.v0);
                continue;
            }
            while (length(a->buf) % Int(8LL) != Int(0LL)) {
                asmBytes(a, {u8(u8(204LL))});
            }
            i32 base = asmPos(a);
            setMap(bases, key, base);
            asmPatch32(a, t.lea, base);
            auto table = t.table;
            for (Int i = 0; i < length(table); ++i) {
                i32 b = table[i];
                oc->addresses = appendValues(oc->addresses, TextAddress{asmPos(a), e->blockAt[funcEmitterTarget(e, b)]});
                asmU64(a, 0);
            }
        }
        return e;
    }

    static void objCodeEndRegion(ObjCode* oc, i32 start) {
        PoolScope persistent(*oc->pool);
        i32 end = asmPos(&oc->a);
        oc->regions = appendValues(oc->regions, Region{oc->regionName, convert<u32>(start), convert<u32>(end), asmSpTable(&oc->a, start, end), asmLineTable(&oc->a, start, end)});
    }

    // exportThunks gives every exported function a thunk Go calls, global.
    static void objCodeExportThunks(ObjCode* oc) {
        Module* m = oc->m;
        auto exports = m->exports;
        for (Int i = 0; i < length(exports); ++i) {
            Export e = exports[i];
            if (e.kind == externFunc) {
                FuncType* t = moduleFuncType(m, e.index);
                String sym = exportSymbol(e.name, t);
                setMap(oc->globals, sym, true);
                objCodeThunk(oc, sym, t->params, t->results, convert<i64>(e.index), {});
            }
        }
    }

    // extern returns the index of an external symbol.
    static i32 objCodeExtern(ObjCode* oc, String sym) {
        PoolScope persistent(*oc->pool);
        sym = cloneString(sym);
        {
            auto temporary1023 = lookupMap(oc->externIx, sym);
            i32 i = temporary1023.v0;
            bool ok = temporary1023.v1;
            if (ok) {
                return i;
            }
        }
        i32 i = convert<i32>(length(oc->externs));
        oc->externs = appendValues(oc->externs, sym);
        setMap(oc->externIx, sym, i);
        return i;
    }

    // floatConst returns a RIP-relative operand for a float constant.
    static Amem objCodeFloatConst(ObjCode* oc, u64 bits, u8 size) {
        Array<u64, 2> key = Array<u64, 2>{{bits, convert<u64>(size)}};
        auto temporary1024 = lookupMap(oc->floatIx, key);
        i32 label = temporary1024.v0;
        bool ok = temporary1024.v1;
        if (!ok) {
            Slice<u8> b = makeSlice<u8>(size);
            if (size == 4) {
                littlePutUint32(b, convert<u32>(bits));
            } else {
                littlePutUint64(b, bits);
            }
            label = objCodeAddBlob(oc, b, convert<i64>(size));
            setMap(oc->floatIx, key, label);
        }
        return Amem{-1, -1, {}, {}, label, labelModule};
    }

    // funcName names a function's region, which Go shows in tracebacks.
    static String objCodeFuncName(ObjCode* oc, Int i) {
        String n = oc->m->funcs[i].name;
        if ((n != String(""))) {
            return fmtSprintf(String("f%d:%s"), i, displayName(n));
        }
        return fmtSprintf(String("f%d"), i);
    }

    // goCall emits a function of the internal convention that calls the Go
    // function value of sym with the instance and its parameters, and returns
    // its results; with record it writes the jump frame first. When the
    // instance asks, by its interrupt word, it calls the interrupt stub on the
    // way out, as Go's runtime would inject a call of it there.
    static i32 objCodeGoCall(ObjCode* oc, String name, Slice<ValType> params, Slice<ValType> results, String sym, bool record) {
        Asm* a = &oc->a;
        GoFrame g = goAssign(params, results);
        auto temporary1034 = internalLocs(params, argGPRs, argXMMs, 0);
        Slice<VReg> pregs = temporary1034.v0;
        Slice<Int> pslots = temporary1034.v1;
        Int nslots = temporary1034.v2;
        auto temporary1035 = internalLocs(results, resultGPRs, resultXMMs, nslots);
        Slice<VReg> rregs = temporary1035.v0;
        Slice<Int> rslots = temporary1035.v1;
        Slice<VReg> saved = appendValues(appendSlice(Slice<VReg>{}, calleeSaved), ctxReg);
        i32 total = 8 + 8 * convert<i32>(length(saved)) + g.size;
        i32 start = objCodeBeginRegion(oc, name);
        asmPush(a, RBP);
        asmMovRR(a, 8, RBP, RSP);
        for (Int i = 0; i < length(saved); ++i) {
            VReg r = saved[i];
            asmPush(a, r);
        }
        // The arguments fill the caller-saved registers: the stack check
        // follows the pushes and computes in R12, saved.
        const i32 retry = asmPos(a);
        i32 slow = objCodeStackCheck(oc, g.size, R12);
        if (g.size > 0) {
            asmAluRI(a, aluSub, 8, RSP, convert<i64>(g.size));
        }
        i32 incoming = total + 8;
        if (record) {
            // The caller's callee-saved registers, still in theirs, go into
            // the frame before recordFrame uses two of them.
            for (Int k = 0; k < length(calleeSaved); ++k) {
                asmStore(a, 8, calleeSaved[k], baseAt(ctxReg, offJump + 8 * (jumpRegs + convert<i32>(k))));
            }
            objCodeRecordFrame(oc);
        }
        // The arguments move from the internal convention to Go's: registers
        // to Go's stack first, then the registers among themselves, then the
        // stack to Go's registers, which no register move reads. R12, saved,
        // is the scratch.
        Slice<RegMove> moves = {};
        for (Int i = 0; i < length(params); ++i) {
            ValType t = params[i];
            GoLoc dst = g.params[i];
            if (pregs[i] != noReg && dst.reg == noReg) {
                asmStore(a, convert<u8>(goSize(t)), pregs[i], rspAt(dst.off));
            } else if (pregs[i] == noReg && dst.reg == noReg) {
                asmLoad(a, 8, 8, false, R12, rspAt(incoming + convert<i32>(8 * pslots[i])));
                asmStore(a, convert<u8>(goSize(t)), R12, rspAt(dst.off));
            } else if (pregs[i] != noReg) {
                moves = appendValues(moves, RegMove{dst.reg, pregs[i], 8});
            }
        }
        asmMoveRegs(a, moves, R12);
        for (Int i = 0; i < length(params); ++i) {
            GoLoc dst = g.params[i];
            if (pregs[i] == noReg && dst.reg != noReg) {
                asmLoad(a, 8, 8, false, dst.reg, rspAt(incoming + convert<i32>(8 * pslots[i])));
            }
        }
        asmMovRR(a, 8, RAX, ctxReg);
        objCodeCallGo(oc, sym);
        // The results move back the same way; Go leaves the upper half of a
        // 32-bit result alone.
        moves = {};
        for (Int i = 0; i < length(results); ++i) {
            ValType t = results[i];
            GoLoc src = g.results[i];
            if (rregs[i] == noReg && src.reg != noReg) {
                if (t == I32) {
                    asmMovRR(a, 4, src.reg, src.reg);
                }
                asmStore(a, 8, src.reg, rspAt(incoming + convert<i32>(8 * rslots[i])));
            } else if (rregs[i] == noReg && src.reg == noReg) {
                asmLoad(a, convert<u8>(goSize(t)), 8, false, R12, rspAt(src.off));
                asmStore(a, 8, R12, rspAt(incoming + convert<i32>(8 * rslots[i])));
            } else if (rregs[i] != noReg && src.reg != noReg) {
                moves = appendValues(moves, RegMove{rregs[i], src.reg, convert<u8>(t == I32 ? 4 : 8)});
            }
        }
        asmMoveRegs(a, moves, R12);
        for (Int i = 0; i < length(results); ++i) {
            ValType t = results[i];
            GoLoc src = g.results[i];
            if (rregs[i] != noReg && src.reg == noReg) {
                asmLoad(a, convert<u8>(goSize(t)), 8, false, rregs[i], rspAt(src.off));
            }
        }
        if (g.size > 0) {
            asmAluRI(a, aluAdd, 8, RSP, convert<i64>(g.size));
        }
        Int i = length(saved) - 1;
        for (; i >= 0; i--) {
            asmPop(a, saved[i]);
        }
        asmLoad(a, 8, 8, false, memReg, baseAt(ctxReg, offMem));
        asmAluMI(a, aluCmp, 4, baseAt(ctxReg, offInterrupt), 0);
        i32 interrupt = asmJcc8(a, ccNE);
        i32 framed = a->sp;
        asmPop(a, RBP);
        asmRet(a);
        asmSetSP(a, framed);
        asmHere8(a, interrupt);
        asmCallMem(a, objCodeGotMem(oc, symInterruptCall));
        asmPop(a, RBP);
        asmRet(a);
        asmSetSP(a, 8 + 8 * convert<i32>(length(saved)));
        objCodeSlowPath(oc, slow, retry, g.size, R12);
        objCodeEndRegion(oc, start);
        return start;
    }

    // gotMem is the GOT entry of a symbol: it holds the symbol's address.
    static Amem objCodeGotMem(ObjCode* oc, String sym) {
        return Amem{-1, -1, {}, {}, objCodeExtern(oc, sym), labelGot};
    }

    // importAdapters give imported functions an entry in the internal
    // convention: an adapter calls the Go function value of the import's
    // symbol, which names it with its type. One that returns twice has a second
    // adapter, for its direct calls, which writes the jump frame; the module
    // then has the land stub (goabi.go). Tables and exports take the first,
    // as their callers keep no token.
    // importAdapters makes the code of the imports a pointer can reach, the
    // functions the module names: an adapter that calls the Go function.
    // The others are only called, so they have none; an import the code
    // compiles at its calls must not be named.
    static void objCodeImportAdapters(ObjCode* oc) {
        Module* m = oc->m;
        Slice<bool> named = objCodeNamedFunctions(oc);
        auto imports = m->imports;
        for (Int i = 0; i < length(imports); ++i) {
            Import imp = imports[i];
            if (imp.kind != externFunc) {
                continue;
            }
            FuncType* t = moduleFuncType(m, imp.index);
            String sym = importSymbol(imp.module, imp.name, t);
            oc->importSyms[imp.index] = sym;
            oc->importIntrinsics[imp.index] = intrinsicOf(imp, t);
            if (oc->importIntrinsics[imp.index] != intrinsicNone) {
                if (named[imp.index]) {
                    compilerError(String("unsupported: the address of an import compiled at its calls"));
                }
                continue;
            }
            String name = String("w2o.adapter.") + symbolPart(imp.module) + String(".") + symbolPart(imp.name);
            if (named[imp.index]) {
                oc->importCode[imp.index] = true;
                oc->funcAt[imp.index] = objCodeGoCall(oc, name, t->params, t->results, sym, false);
            }
            if (m->funcs[imp.index].returnsTwice) {
                setMap(oc->jumpAt, convert<i32>(imp.index), objCodeGoCall(oc, name + String(".jump"), t->params, t->results, sym, true));
            }
        }
    }

    // namedFunctions marks the functions the module's elements, exports and
    // globals name: the only ones whose address it can take, by ref.func
    // too, which a valid module keeps to them.
    static Slice<bool> objCodeNamedFunctions(ObjCode* oc) {
        Module* m = oc->m;
        Slice<bool> named = makeSlice<bool>(length(m->funcs));
        auto refFunc = [&](Slice<u8> x) mutable -> void {
            if (length(x) > 0 && x[0] == 210) {
                Reader r = Reader{subslice(x, 1, length(x)), {}};
                u32 index = readerU32(&r);
                if (convert<i64>(index) < length(named)) {
                    named[index] = true;
                }
            }
        };
        auto elems = m->elems;
        for (Int i = 0; i < length(elems); ++i) {
            auto exprs = elems[i].exprs;
            for (Int k = 0; k < length(exprs); ++k) {
                refFunc(exprs[k]);
            }
        }
        auto exports = m->exports;
        for (Int i = 0; i < length(exports); ++i) {
            if (exports[i].kind == externFunc) {
                named[exports[i].index] = true;
            }
        }
        auto globals = m->globals;
        for (Int i = 0; i < length(globals); ++i) {
            refFunc(globals[i].init);
        }
        return named;
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
        Module* m = oc->m;
        Layout* lay = oc->lay;
        Asm* a = &oc->a;
        setMap(oc->globals, symInit, true);
        objCodeThunk(oc, symInit, {}, {}, -1, [&]() mutable -> void {
            auto range1052 = m->funcs;
            for (Int index1053 = 0, rangeEnd1054 = length(range1052); index1053 < rangeEnd1054; ++index1053) {
                Int i = index1053;
                if (i < m->importedFuncs && !oc->importCode[i]) {
                    continue;
                }
                i32 d = layoutDesc(lay, i);
                asmBytes(a, {u8(u8(72LL)), u8(u8(141LL)), u8(u8(5LL))});
                a->fixups = appendValues(a->fixups, RipFixup{asmPos(a), asmPos(a) + 4, convert<i32>(i), labelFunc});
                asmU32(a, 0);
                asmStore(a, 8, RAX, baseAt(ctxReg, d));
                asmMovImm(a, 8, RAX, convert<i64>(typeID(moduleFuncType(m, convert<u32>(i)))));
                asmStore(a, 8, RAX, baseAt(ctxReg, d + 8));
            }
            auto range1055 = m->elems;
            for (Int index1056 = 0, rangeEnd1057 = length(range1055); index1056 < rangeEnd1057; ++index1056) {
                Int i = index1056;
                Elem e = range1055[index1056];
                if (e.mode == segmentActive) {
                    auto temporary1058 = constOffset(e.offset);
                    i64 off = temporary1058.v0;
                    bool ok = temporary1058.v1;
                    if (!ok) {
                        compilerError(String("unsupported: element segment %d with a computed offset"));
                    }
                    i32 tb = lay->tables[e.table];
                    i64 n = convert<i64>(length(e.exprs));
                    Slice<u8> arr = makeSlice<u8>(0, 4 * n);
                    auto range1059 = e.exprs;
                    for (Int index1060 = 0, rangeEnd1061 = length(range1059); index1060 < rangeEnd1061; ++index1060) {
                        Slice<u8> x = range1059[index1060];
                        i32 d = {};
                        if (x[0] == 210) {
                            Reader* r = activePool->make<Reader>(Reader{subslice(x, 1, length(x)), {}});
                            d = layoutDesc(lay, convert<i64>(readerU32(r)));
                        }
                        arr = littleAppendUint32(arr, convert<u32>(d));
                    }
                    asmLoad(a, 4, 4, false, RAX, baseAt(ctxReg, tb + tableSize));
                    asmMovImm(a, 8, RCX, off + n);
                    asmAluRR(a, aluCmp, 8, RAX, RCX);
                    i32 fits = asmJcc8(a, ccAE);
                    objCodeTrapCall(oc, trapTableOOB);
                    asmHere8(a, fits);
                    if (n > 0) {
                        i32 label = objCodeAddBlob(oc, arr, 4);
                        asmLea(a, 8, RSI, Amem{-1, -1, {}, {}, label, labelModule});
                        asmLoad(a, 8, 8, false, RDI, baseAt(ctxReg, tb + tableEntries));
                        asmMovImm(a, 8, RAX, 8 * off);
                        asmAluRR(a, aluAdd, 8, RDI, RAX);
                        asmMovImm(a, 4, RCX, n);
                        i32 loop = asmPos(a);
                        asmOpRM(a, true, Span<const u8>({u8(99LL)}), 0, baseAt(RSI, 0), 0, false);
                        asmTest(a, 8, RAX, RAX);
                        i32 null = asmJcc8(a, ccE);
                        asmAluRR(a, aluAdd, 8, RAX, ctxReg);
                        asmHere8(a, null);
                        asmStore(a, 8, RAX, baseAt(RDI, 0));
                        asmAluRI(a, aluAdd, 8, RSI, 4);
                        asmAluRI(a, aluAdd, 8, RDI, 8);
                        asmAluRI(a, aluSub, 4, RCX, 1);
                        asmPatch32(a, asmJccRel(a, ccNE), loop);
                    }
                }
                if (e.mode != segmentPassive) {
                    asmStoreImm(a, 1, baseAt(ctxReg, layoutElemDropped(lay, m, i)), 1);
                }
            }
            objCodeCallFunc(oc, oc->initFn);
            if (m->batchData) {
                objCodeInitData(oc);
                if (m->start >= 0) {
                    objCodeCallFunc(oc, Int(m->start));
                }
            }
        });
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
    // the caller's registers are still in theirs, the callee-saved ones
    // saved: R11 and R12 are its scratch. The caller's token is handed out
    // from the instance's count when the slot is still zero.
    static void objCodeRecordFrame(ObjCode* oc) {
        Asm* a = &oc->a;
        auto word = [&](i32 k) mutable -> Amem {
            return baseAt(ctxReg, offJump + 8 * k);
        };
        asmLoad(a, 8, 8, false, R12, baseAt(RBP, 8));
        asmStore(a, 8, R12, word(jumpPC));
        asmLoad(a, 8, 8, false, R12, baseAt(R14, gStackHi));
        asmAluRM(a, aluSub, 8, R12, baseAt(ctxReg, offBase));
        asmAluRR(a, aluSub, 8, R12, RBP);
        asmAluRI(a, aluSub, 8, R12, 16);
        asmStore(a, 8, R12, word(jumpSP));
        asmLoad(a, 8, 8, false, R11, baseAt(RBP, 0));
        asmLoad(a, 8, 8, false, R12, baseAt(R14, gStackHi));
        asmAluRM(a, aluSub, 8, R12, baseAt(ctxReg, offBase));
        asmAluRR(a, aluSub, 8, R12, R11);
        asmStore(a, 8, R12, word(jumpBP));
        asmLoad(a, 8, 8, false, R12, baseAt(R11, -8));
        asmTest(a, 8, R12, R12);
        i32 have = asmJcc8(a, ccNE);
        asmLoad(a, 8, 8, false, R12, baseAt(ctxReg, offTokens));
        asmAluRI(a, aluAdd, 8, R12, 1);
        asmStore(a, 8, R12, baseAt(ctxReg, offTokens));
        asmStore(a, 8, R12, baseAt(R11, -8));
        asmHere8(a, have);
        asmStore(a, 8, R12, word(jumpToken));
    }

    // resolve fills the displacements within the text and turns the others
    // into relocations: a displacement is relative to the end of its
    // instruction, so the addend takes that distance off.
    static void objCodeResolve(ObjCode* oc) {
        Asm* a = &oc->a;
        auto fixups = a->fixups;
        for (Int i = 0; i < length(fixups); ++i) {
            RipFixup f = fixups[i];
            switch (f.kind) {
                case labelModule: {
                    oc->relocs = appendValues(oc->relocs, TextReloc{f.at, -1, convert<i64>(oc->blobAt[f.label]) - convert<i64>(f.end - f.at)});
                    break;
                }
                case labelGot: {
                    oc->relocs = appendValues(oc->relocs, TextReloc{f.at, f.label, -convert<i64>(f.end - f.at)});
                    break;
                }
                case labelFunc: {
                    littlePutUint32(subslice(a->buf, f.at, length(a->buf)), convert<u32>(oc->funcAt[f.label] - f.end));
                    break;
                }
                default: {
                    compilerError(String("internal error: unresolved local label"));
                    break;
                }
            }
        }
        auto calls = oc->calls;
        for (Int i = 0; i < length(calls); ++i) {
            CallFixup c = calls[i];
            asmPatch32(a, c.at, oc->funcAt[c.fn]);
        }
        auto jumpCalls = oc->jumpCalls;
        for (Int i = 0; i < length(jumpCalls); ++i) {
            CallFixup c = jumpCalls[i];
            asmPatch32(a, c.at, oc->jumpAt[c.fn]);
        }
    }

    // slowPath grows the stack and starts the region again at entry. It
    // passes the morestack stub what stackCheck compared, on the stack.
    static void objCodeSlowPath(ObjCode* oc, i32 at, i32 entry, i32 total, VReg temp) {
        Asm* a = &oc->a;
        asmPatch32(a, at, asmPos(a));
        asmPush(a, total <= stackSmall ? RSP : temp);
        asmCallMem(a, objCodeGotMem(oc, symMorestackStub));
        objCodeDropWord(oc);
        asmPatch32(a, asmJmpRel(a), entry);
    }

    // dropWord takes a word off the stack without touching a register or
    // the flags. The SP delta at a call's return address must still count
    // what the caller pushed for the callee, so the callee cannot pop it.
    static void objCodeDropWord(ObjCode* oc) {
        Asm* a = &oc->a;
        asmLea(a, 8, RSP, rspAt(8));
        asmMoveSP(a, -8);
    }

    // stackCheck compares the stack pointer, less total-stackSmall for a large
    // frame computed in temp, with g.stackguard0 and calls the morestack stub
    // from out of line when it is not above. The caller emits the slow path at
    // the delta it had here, with slowPath.
    static i32 objCodeStackCheck(ObjCode* oc, i32 total, VReg temp) {
        Asm* a = &oc->a;
        if (total <= stackSmall) {
            asmAluRM(a, aluCmp, 8, RSP, baseAt(R14, gStackguard0));
        } else {
            asmLea(a, 8, temp, rspAt(-(total - stackSmall)));
            asmAluRM(a, aluCmp, 8, temp, baseAt(R14, gStackguard0));
        }
        return asmJccRel(a, ccBE);
    }

    // stackCheckBlocks gives the block that checks the stack where it
    // begins, in place of the prologue, of a function with calls whose frame
    // a leaf could have: until its first call it runs as a leaf does. The
    // block is the nearest common dominator of the blocks that call, outside
    // every loop, and there is none when that is the entry. The check takes
    // the room of the prologue's.
    static Slice<i32> objCodeStackCheckBlocks(MFunc* mf) {
        const Int n = length(mf->blocks);
        // The blocks in postorder from the entry, block 0.
        Slice<i32> post = makeSlice<i32>(0);
        Slice<bool> seen = makeSlice<bool>(n);
        Vec<Array<i32, 2>> stack;
        stack.pushBack(Array<i32, 2>{{0, 0}});
        seen[0] = true;
        while (stack.size()) {
            const Array<i32, 2> top = stack[stack.size() - 1];
            const auto succs = mf->blocks[top[0]].succs;
            if (top[1] < length(succs)) {
                stack[stack.size() - 1][1] = top[1] + 1;
                const i32 next = succs[top[1]];
                if (!seen[next]) {
                    seen[next] = true;
                    stack.pushBack(Array<i32, 2>{{next, 0}});
                }
                continue;
            }
            post = appendValues(post, top[0]);
            stack.resize(stack.size() - 1);
        }
        Slice<i32> rank = makeSlice<i32>(n);
        for (Int i = 0; i < length(post); ++i) {
            rank[post[i]] = convert<i32>(i);
        }
        // The predecessors from the successors, the blocks' own lists being
        // stale where the allocator copied a dispatch into the blocks that
        // jumped to it.
        Slice<Slice<i32>> preds = makeSlice<Slice<i32>>(n);
        for (Int i = 0; i < length(post); ++i) {
            const auto succs = mf->blocks[post[i]].succs;
            for (Int k = 0; k < length(succs); ++k) {
                preds[succs[k]] = appendValues(preds[succs[k]], post[i]);
            }
        }
        Slice<i32> idom = makeSlice<i32>(n);
        for (Int b = 0; b < n; ++b) {
            idom[b] = -1;
        }
        idom[0] = 0;
        for (bool changed = true; changed;) {
            changed = false;
            for (Int i = length(post) - 2; i >= 0; --i) {
                const i32 b = post[i];
                i32 d = -1;
                for (Int k = 0; k < length(preds[b]); ++k) {
                    i32 x = preds[b][k];
                    if (idom[x] < 0) {
                        continue;
                    }
                    if (d >= 0) {
                        i32 y = d;
                        while (x != y) {
                            while (rank[x] < rank[y]) {
                                x = idom[x];
                            }
                            while (rank[y] < rank[x]) {
                                y = idom[y];
                            }
                        }
                    }
                    d = x;
                }
                if (d >= 0 && idom[b] != d) {
                    idom[b] = d;
                    changed = true;
                }
            }
        }
        Slice<bool> checks = makeSlice<bool>(n);
        for (Int i = 0; i < length(post); ++i) {
            const i32 b = post[i];
            auto insts = mf->blocks[b].insts.view();
            bool calls = false;
            for (Int k = 0; k < length(insts) && !calls; ++k) {
                calls = insts[k].op == mCall || insts[k].op == mCallInd;
            }
            if (!calls) {
                continue;
            }
            i32 at = b;
            while (at != 0 && mf->blocks[at].depth > 0) {
                at = idom[at];
            }
            if (at == 0) {
                return {};
            }
            checks[at] = true;
        }
        i32 common = -1;
        for (Int i = 0; i < length(post); ++i) {
            i32 x = post[i];
            if (!checks[x]) {
                continue;
            }
            if (common >= 0) {
                i32 y = common;
                while (x != y) {
                    while (rank[x] < rank[y]) {
                        x = idom[x];
                    }
                    while (rank[y] < rank[x]) {
                        y = idom[y];
                    }
                }
            }
            common = x;
        }
        while (common > 0 && mf->blocks[common].depth > 0) {
            common = idom[common];
        }
        if (common <= 0) {
            return {};
        }
        return sliceOf<i32>({common});
    }

    // thunk emits a function Go calls with ABIInternal, func(ctx uintptr,
    // params...) (results...), that calls function index in the internal
    // convention; with index negative it calls body instead, which emits the
    // work itself.
    static i32 objCodeThunk(ObjCode* oc, String name, Slice<ValType> params, Slice<ValType> results, Int index, Fn<void()> body) {
        Asm* a = &oc->a;
        GoFrame g = goAssign(params, results);
        auto temporary1102 = internalLocs(params, argGPRs, argXMMs, 0);
        Slice<VReg> pregs = temporary1102.v0;
        Slice<Int> pslots = temporary1102.v1;
        Int nslots = temporary1102.v2;
        auto temporary1103 = internalLocs(results, resultGPRs, resultXMMs, nslots);
        Slice<VReg> rregs = temporary1103.v0;
        Slice<Int> rslots = temporary1103.v1;
        Int nall = temporary1103.v2;
        // The frame's last word keeps the instance g had: the thunk's own
        // takes its place while the code runs.
        i32 saved = convert<i32>(8 * nall);
        i32 frame = saved + 8;
        i32 total = 8 + frame;
        i32 start = objCodeBeginRegion(oc, name);
        asmMovRR(a, 8, ctxReg, RAX);
        i32 slow = objCodeStackCheck(oc, total, R12);
        asmPush(a, RBP);
        asmMovRR(a, 8, RBP, RSP);
        asmAluRI(a, aluSub, 8, RSP, convert<i64>(frame));
        asmLoad(a, 8, 8, false, RDX, baseAt(R14, gInstance));
        asmStore(a, 8, RDX, rspAt(saved));
        asmStore(a, 8, ctxReg, baseAt(R14, gInstance));
        asmLoad(a, 8, 8, false, memReg, baseAt(ctxReg, offMem));
        i32 goArgs = total + 8;
        // The parameters move from Go's convention to the internal one, as
        // the adapter moves the other way (goCall), R12 the scratch; a
        // 32-bit value Go passed is zero-extended.
        Slice<RegMove> moves = {};
        for (Int i = 0; i < length(params); ++i) {
            ValType t = params[i];
            GoLoc src = g.params[i];
            if (pregs[i] == noReg && src.reg != noReg) {
                if (t == I32) {
                    asmMovRR(a, 4, src.reg, src.reg);
                }
                asmStore(a, 8, src.reg, rspAt(convert<i32>(8 * pslots[i])));
            } else if (pregs[i] == noReg && src.reg == noReg) {
                asmLoad(a, convert<u8>(goSize(t)), 8, false, R12, rspAt(goArgs + src.off));
                asmStore(a, 8, R12, rspAt(convert<i32>(8 * pslots[i])));
            } else if (pregs[i] != noReg && src.reg != noReg) {
                moves = appendValues(moves, RegMove{pregs[i], src.reg, convert<u8>(t == I32 ? 4 : 8)});
            }
        }
        asmMoveRegs(a, moves, R12);
        for (Int i = 0; i < length(params); ++i) {
            ValType t = params[i];
            GoLoc src = g.params[i];
            if (pregs[i] != noReg && src.reg == noReg) {
                asmLoad(a, convert<u8>(goSize(t)), 8, false, pregs[i], rspAt(goArgs + src.off));
            }
        }
        if (index >= 0) {
            objCodeCallFunc(oc, index);
        } else {
            body();
        }
        moves = {};
        for (Int i = 0; i < length(results); ++i) {
            ValType t = results[i];
            GoLoc dst = g.results[i];
            if (rregs[i] != noReg && dst.reg == noReg) {
                asmStore(a, convert<u8>(goSize(t)), rregs[i], rspAt(goArgs + dst.off));
            } else if (rregs[i] == noReg && dst.reg == noReg) {
                asmLoad(a, 8, 8, false, R12, rspAt(convert<i32>(8 * rslots[i])));
                asmStore(a, convert<u8>(goSize(t)), R12, rspAt(goArgs + dst.off));
            } else if (rregs[i] != noReg && dst.reg != noReg) {
                moves = appendValues(moves, RegMove{dst.reg, rregs[i], 8});
            }
        }
        asmMoveRegs(a, moves, R12);
        for (Int i = 0; i < length(results); ++i) {
            GoLoc dst = g.results[i];
            if (rregs[i] == noReg && dst.reg != noReg) {
                asmLoad(a, 8, 8, false, dst.reg, rspAt(convert<i32>(8 * rslots[i])));
            }
        }
        asmLoad(a, 8, 8, false, RDX, rspAt(saved));
        asmStore(a, 8, RDX, baseAt(R14, gInstance));
        asmAluRI(a, aluAdd, 8, RSP, convert<i64>(frame));
        asmPop(a, RBP);
        asmRet(a);
        asmSetSP(a, 0);
        objCodeSlowPath(oc, slow, start, total, R12);
        objCodeEndRegion(oc, start);
        return start;
    }

    // trapCall calls Go's trap with the instance and code; it does not return.
    // Like any Go call it leaves the callee room to spill its arguments. Code
    // after it is reached by jumps, with the stack pointer of before.
    static void objCodeTrapCall(ObjCode* oc, i64 code) {
        Asm* a = &oc->a;
        i32 sp = a->sp;
        asmAluRI(a, aluSub, 8, RSP, convert<i64>(goAssign(sliceOf<ValType>({I32}), {}).size));
        asmMovRR(a, 8, RAX, ctxReg);
        asmMovImm(a, 4, RBX, code);
        objCodeCallGo(oc, symTrap);
        asmUd2(a);
        asmSetSP(a, sp);
    }

    // trapAtCall calls Go's trap_at with the instance, the code, the
    // address of the access the fixup describes and its width and kind, in
    // Go's registers; it does not return. The address is taken first: the
    // register holding it may be one of those.
    static void objCodeTrapAtCall(ObjCode* oc, const TrapFixup& t) {
        Asm* a = &oc->a;
        i32 sp = a->sp;
        asmAluRI(a, aluSub, 8, RSP, convert<i64>(goAssign(sliceOf<ValType>({I32, I64, I32}), {}).size));
        if (t.reg == noReg) {
            asmMovImm(a, 8, RCX, convert<i64>(convert<u32>(t.disp)));
        } else {
            asmLea(a, 8, RCX, baseAt(t.reg, t.disp));
        }
        asmMovRR(a, 8, RAX, ctxReg);
        asmMovImm(a, 4, RBX, t.code);
        asmMovImm(a, 4, RDI, convert<i64>(t.width) | convert<i64>(t.kind) << 8);
        objCodeCallGo(oc, symTrapAt);
        asmUd2(a);
        asmSetSP(a, sp);
    }

    // apply rewrites every operand through the aliases.
    static void optimizerApply(Optimizer* o) {
        Function* f = o->f;
        // Every pass applies its replacements before it returns, and operands
        // written since copy resolved ones.
        if (o->applied == o->replaced) {
            return;
        }
        o->applied = o->replaced;
        o->counted = false;
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
        for (Int b = 0, end = length(f->blocks); b < end; ++b) {
            Term* t = &f->blocks[b].term;
            if (t->kind == TermIf || t->kind == TermSwitch) {
                t->value = resolve(t->value);
            }
            for (Int i = 0, end = length(t->values); i < end; ++i) {
                t->values[i] = resolve(t->values[i]);
            }
            for (Int i = 0, end = length(t->edges); i < end; ++i) {
                for (Int j = 0, end = length(t->edges[i].args); j < end; ++j) {
                    t->edges[i].args[j] = resolve(t->edges[i].args[j]);
                }
            }
        }
    }

    // cleanup removes unreachable blocks, sends edges past blocks that only
    // jump, and merges blocks into their only predecessor.
    static bool optimizerCleanup(Optimizer* o) {
        Function* f = o->f;
        bool changed = false;
        optimizerCountUses(o);
        for (Int b = 0, end = length(f->blocks); b < end; ++b) {
            Block* blk = &f->blocks[b];
            if (blk->dead || b == 0 || length(blk->instrs) != 0 || blk->term.kind != TermJump || blk->term.edges[0].block == convert<i32>(b)) {
                continue;
            }
            if (!optimizerLocalParams(o, convert<i32>(b))) {
                continue;
            }
            Edge next = blk->term.edges[0];
            auto preds = blk->preds;
            for (Int j = 0; j < length(preds); ++j) {
                i32 p = preds[j];
                Term* pt = &f->blocks[p].term;
                for (Int i = 0, end = length(pt->edges); i < end; ++i) {
                    Edge* e = &pt->edges[i];
                    if (e->block == convert<i32>(b)) {
                        Slice<i32> args = optimizerSubstitute(o, convert<i32>(b), next.args, e->args);
                        *e = Edge{next.block, args};
                        changed = true;
                    }
                }
            }
        }
        if (changed) {
            functionComputePreds(f);
        }
        changed = optimizerUnreachable(o) || changed;
        bool merged = false;
        for (Int b = 0, end = length(f->blocks); b < end; ++b) {
            Block* blk = &f->blocks[b];
            if (blk->dead || b == 0 || length(blk->preds) != 1) {
                continue;
            }
            i32 p = blk->preds[0];
            Block* pb = &f->blocks[p];
            if (p == convert<i32>(b) || pb->term.kind != TermJump) {
                continue;
            }
            Edge e = pb->term.edges[0];
            auto params = blk->params;
            for (Int i = 0; i < length(params); ++i) {
                i32 param = params[i];
                optimizerReplace(o, param, optimizerResolve(o, e.args[i]));
            }
            auto instrs = blk->instrs.view();
            for (Int i = 0; i < length(instrs); ++i) {
                i32 v = instrs[i];
                f->values[v].block = p;
            }
            pb->instrs.append(blk->instrs.view());
            pb->term = blk->term;
            *blk = Block{{}, {}, {}, {}, true};
            auto edges = pb->term.edges;
            for (Int j = 0; j < length(edges); ++j) {
                Edge s = edges[j];
                Block* sb = &f->blocks[s.block];
                auto range1150 = sb->preds;
                for (Int index1151 = 0, rangeEnd1152 = length(range1150); index1151 < rangeEnd1152; ++index1151) {
                    Int i = index1151;
                    i32 q = range1150[index1151];
                    if (q == convert<i32>(b)) {
                        sb->preds[i] = p;
                    }
                }
            }
            merged = true;
        }
        if (merged) {
            optimizerApply(o);
            functionComputePreds(f);
        }
        if (changed || merged) {
            o->counted = false;
            f->cfg = nullptr;
        }
        return changed || merged;
    }

    static void optimizerCommonExpressions(Optimizer* o) {
        Function* f = o->f;
        optimizerApply(o);
        CfgInfo* ci = f->cfg ? f->cfg : functionAnalyze(f);
        f->cfg = ci;
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
        return o->f->values[v].aux;
    }

    static i32 optimizerConstant(Optimizer* o, ValType t, i64 bits) {
        if (t == I32) {
            bits = convert<i64>(convert<i32>(bits));
        }
        ConstKey key = ConstKey{t, bits};
        {
            auto temporary1166 = lookupMap(o->consts, key);
            i32 v = temporary1166.v0;
            bool ok = temporary1166.v1;
            if (ok) {
                return v;
            }
        }
        i32 v = functionNewValue(o->f, OpConst, t, -1, bits, Span<const i32>({}));
        setMap(o->consts, key, v);
        return v;
    }

    static void optimizerCountUses(Optimizer* o) {
        Function* f = o->f;
        if (o->counted && o->uses.size() == f->values.size()) {
            return;
        }
        o->counted = true;
        o->uses.resize(usize(length(f->values)));
        o->uses.fill(0);
        const Span<i32> uses = o->uses.view();
        for (Int b = 0, end = length(f->blocks); b < end; ++b) {
            Block* blk = &f->blocks[b];
            if (blk->dead) {
                continue;
            }
            auto instrs = blk->instrs.view();
            for (Int i = 0; i < length(instrs); ++i) {
                i32 v = instrs[i];
                auto args = functionArgs(f, v);
                for (Int i = 0; i < length(args); ++i) {
                    i32 a = args[i];
                    uses[a]++;
                }
            }
            Term* t = &blk->term;
            if (t->kind == TermIf || t->kind == TermSwitch) {
                uses[t->value]++;
            }
            auto values = t->values;
            for (Int i = 0; i < length(values); ++i) {
                i32 v = values[i];
                uses[v]++;
            }
            auto edges = t->edges;
            for (Int i = 0; i < length(edges); ++i) {
                Edge e = edges[i];
                auto args = e.args;
                for (Int i = 0; i < length(args); ++i) {
                    i32 a = args[i];
                    uses[a]++;
                }
            }
        }
    }

    // deadCode removes pure values nothing uses and parameters only their own
    // edges use.
    static void optimizerDeadCode(Optimizer* o) {
        Function* f = o->f;
        while (true) {
            optimizerCountUses(o);
            bool removed = false;
            for (Int b = 0, end = length(f->blocks); b < end; ++b) {
                Block* blk = &f->blocks[b];
                if (blk->dead) {
                    continue;
                }
                const usize count = blk->instrs.size();
                usize first = count;
                Int i = length(blk->instrs) - 1;
                for (; i >= 0; i--) {
                    i32 v = blk->instrs[i];
                    if (o->uses[v] == 0 && !hasEffect(f->values[v].op)) {
                        auto args = functionArgs(f, v);
                        for (Int i = 0; i < length(args); ++i) {
                            i32 a = args[i];
                            o->uses[a]--;
                        }
                        removed = true;
                        continue;
                    }
                    blk->instrs[--first] = v;
                }
                if (first && first != count) {
                    copySlice(blk->instrs.view(), subslice(blk->instrs.view(), Int(first), Int(count)));
                }
                blk->instrs.resize(count - first);
            }
            for (Int b = 0, end = length(f->blocks); b < end; ++b) {
                Block* blk = &f->blocks[b];
                if (blk->dead || b == 0 || length(blk->params) == 0) {
                    continue;
                }
                Slice<bool> keep = makeSlice<bool>(length(blk->params));
                bool drop = false;
                Vec<Edge*> in = optimizerIncoming(o, convert<i32>(b));
                auto params = blk->params;
                for (Int k = 0; k < length(params); ++k) {
                    i32 p = params[k];
                    keep[k] = o->uses[p] > selfUses(in, k, p);
                    if (!keep[k]) {
                        drop = true;
                    }
                }
                if (drop) {
                    optimizerDropParams(o, convert<i32>(b), keep);
                    removed = true;
                }
            }
            if (!removed) {
                return;
            }
        }
    }

    // dropParams removes the parameters of block b not kept, and their
    // arguments on every incoming edge.
    static void optimizerDropParams(Optimizer* o, i32 b, Slice<bool> keep) {
        Function* f = o->f;
        o->counted = false;
        Block* blk = &f->blocks[b];
        Slice<i32> params = {};
        auto range1203 = blk->params;
        for (Int index1204 = 0, rangeEnd1205 = length(range1203); index1204 < rangeEnd1205; ++index1204) {
            Int i = index1204;
            i32 p = range1203[index1204];
            if (keep[i]) {
                f->values[p].aux = convert<i64>(length(params));
                params = appendValues(params, p);
            }
        }
        Map<i32, bool> seen = mapOf<i32, bool>({});
        auto preds = blk->preds;
        for (Int k = 0; k < length(preds); ++k) {
            i32 p = preds[k];
            if (seen[p]) {
                continue;
            }
            setMap(seen, p, true);
            Term* pt = &f->blocks[p].term;
            for (Int i = 0, end = length(pt->edges); i < end; ++i) {
                Edge* e = &pt->edges[i];
                if (e->block != b) {
                    continue;
                }
                Slice<i32> args = {};
                auto range1212 = e->args;
                for (Int index1213 = 0, rangeEnd1214 = length(range1212); index1213 < rangeEnd1214; ++index1213) {
                    Int j = index1213;
                    i32 a = range1212[index1213];
                    if (keep[j]) {
                        args = appendValues(args, a);
                    }
                }
                e->args = args;
            }
        }
        blk->params = params;
    }

    static bool optimizerIdentity(Optimizer* o, i32 v, Op op, i32 a, i32 b, ValType t) {
        Function* f = o->f;
        if (optimizerIsConst(o, a) && !optimizerIsConst(o, b) && (op == OpAdd || op == OpMul || op == OpAnd || op == OpOr || op == OpXor || op == OpEq || op == OpNe)) {
            auto temporary1215 = b;
            auto temporary1216 = a;
            a = temporary1215;
            b = temporary1216;
            optimizerSetArgs(o, v, Span<const i32>({a, b}));
        }
        if (optimizerIsConst(o, b)) {
            u64 c = convert<u64>(optimizerConstOf(o, b)) & mask(t);
            if (c == 0 && (op == OpAdd || op == OpSub || op == OpOr || op == OpXor || op == OpShl || op == OpShrS || op == OpShrU || op == OpRotl || op == OpRotr)) {
                optimizerReplace(o, v, a);
                return true;
            } else if (c == 0 && (op == OpMul || op == OpAnd)) {
                optimizerReplace(o, v, b);
                return true;
            } else if (c == 1 && (op == OpMul || op == OpDivS || op == OpDivU)) {
                optimizerReplace(o, v, a);
                return true;
            } else if (c == mask(t) && op == OpAnd) {
                optimizerReplace(o, v, a);
                return true;
            } else if (c == 0 && op == OpEq && t == I32) {
                f->values[v].op = OpEqz;
                optimizerSetArgs(o, v, Span<const i32>({a}));
                return true;
            } else if (c == 0 && op == OpGeU) {
                optimizerReplace(o, v, optimizerConstant(o, I32, 1));
                return true;
            } else if (c == 0 && op == OpLtU) {
                optimizerReplace(o, v, optimizerConstant(o, I32, 0));
                return true;
            }
            if (op == OpMul && (c & (c - 1)) == 0) {
                f->values[v].op = OpShl;
                optimizerSetArgs(o, v, Span<const i32>({a, optimizerConstant(o, t, convert<i64>(bitsTrailingZeros64(c)))}));
                return true;
            }
            if (op == OpAnd) {
                Value* src = &f->values[a];
                if (src->op == OpExtendU && c == u64(4294967295LL)) {
                    optimizerReplace(o, v, a);
                    return true;
                }
            }
        }
        if (a == b) {
            switch (op) {
                case OpSub:
                case OpXor: {
                    optimizerReplace(o, v, optimizerConstant(o, t, 0));
                    return true;
                }
                case OpAnd:
                case OpOr: {
                    optimizerReplace(o, v, a);
                    return true;
                }
                case OpEq:
                case OpLeS:
                case OpLeU:
                case OpGeS:
                case OpGeU: {
                    optimizerReplace(o, v, optimizerConstant(o, I32, 1));
                    return true;
                }
                case OpNe:
                case OpLtS:
                case OpLtU:
                case OpGtS:
                case OpGtU: {
                    optimizerReplace(o, v, optimizerConstant(o, I32, 0));
                    return true;
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
        return o->f->values[v].op == OpConst;
    }

    // isZext reports an i64 value known to have its upper half zero.
    static bool optimizerIsZext(Optimizer* o, i32 v) {
        Value* val = &o->f->values[v];
        return val->op == OpExtendU || (val->op == OpConst && convert<u64>(val->aux) <= u64(4294967295LL)) || (val->op == OpLoad && memKindWidth(convert<MemKind>(val->aux2)) < 8 && !memKindSigned(convert<MemKind>(val->aux2)));
    }

    // localParams reports whether the parameters of block b are used only by
    // its own terminator: edges can bypass b without breaking the dominance of
    // their definitions.
    static bool optimizerLocalParams(Optimizer* o, i32 b) {
        Function* f = o->f;
        Block* blk = &f->blocks[b];
        if (length(blk->params) == 0) {
            return true;
        }
        Map<i32, i32> own = mapOf<i32, i32>({});
        Term* t = &blk->term;
        if (t->kind == TermIf || t->kind == TermSwitch) {
            setMap(own, optimizerResolve(o, t->value), own[optimizerResolve(o, t->value)] + 1);
        }
        auto values = t->values;
        for (Int i = 0; i < length(values); ++i) {
            i32 v = values[i];
            setMap(own, optimizerResolve(o, v), own[optimizerResolve(o, v)] + 1);
        }
        auto edges = t->edges;
        for (Int i = 0; i < length(edges); ++i) {
            Edge e = edges[i];
            auto args = e.args;
            for (Int i = 0; i < length(args); ++i) {
                i32 a = args[i];
                setMap(own, optimizerResolve(o, a), own[optimizerResolve(o, a)] + 1);
            }
        }
        auto params = blk->params;
        for (Int i = 0; i < length(params); ++i) {
            i32 p = params[i];
            if (o->uses[p] != own[p]) {
                return false;
            }
        }
        return true;
    }

    // low32 returns an i32 value equal to the low half of i64 value v, without
    // new instructions.
    static Tuple2<i32, bool> optimizerLow32(Optimizer* o, i32 v) {
        Function* f = o->f;
        Value* val = &f->values[v];
        switch (val->op) {
            case OpConst: {
                return {optimizerConstant(o, I32, convert<i64>(convert<u32>(val->aux))), true};
            }
            case OpExtendU:
            case OpExtendS: {
                return {functionArgs(f, v)[0], true};
            }
        }
        return {0, false};
    }

    // Every u32 has an exact f64 representation. Comparisons after a merge of
    // such conversions and integral constants can stay entirely in integers;
    // the original float values remain available to any other users.
    static bool optimizerNarrowFloatComparisons(Optimizer* o) {
        Function* f = o->f;
        FloatIntegerNarrow* n = activePool->make<FloatIntegerNarrow>(FloatIntegerNarrow{o, makeSlice<u8>(length(f->values)), mapOf<i32, i32>({})});
        bool changed = false;
        for (Int v = 0, end = length(f->values); v < end; ++v) {
            Value val = f->values[v];
            if (val.op < OpFEq || val.op > OpFGe || val.block < 0 || f->blocks[val.block].dead) {
                continue;
            }
            Span<i32> args = functionArgs(f, convert<i32>(v));
            if (!floatIntegerNarrowCan(n, args[0], 0) || !floatIntegerNarrowCan(n, args[1], 0)) {
                continue;
            }
            i32 a = floatIntegerNarrowInteger(n, args[0]);
            i32 b = floatIntegerNarrowInteger(n, args[1]);
            Array<Op, 6> ops = Array<Op, 6>{{OpEq, OpNe, OpLtU, OpGtU, OpLeU, OpGeU}};
            f->values[v].op = ops[Op(val.op - OpFEq)];
            optimizerSetArgs(o, convert<i32>(v), Span<const i32>({a, b}));
            changed = true;
        }
        return changed;
    }

    // paramIndex returns the position of value v among block b's parameters.
    static Int optimizerParamIndex(Optimizer* o, i32 b, i32 v) {
        auto params = o->f->blocks[b].params;
        for (Int i = 0; i < length(params); ++i) {
            i32 p = params[i];
            if (p == v) {
                return i;
            }
        }
        return -1;
    }

    // repair rewrites the uses of h's parameters outside h after thread: the
    // value of a parameter in a block is h's on the paths through h and the
    // argument of the edge on the paths through a bypass, and a block where
    // both meet gets a parameter for it.
    static void optimizerRepair(Optimizer* o, i32 h, Map<i32, Bypass> bypasses) {
        Function* f = o->f;
        Slice<i32> params = appendSlice(Slice<i32>{}, f->blocks[h].params);
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
            i32 j = paramIndex(v);
            bool ok = j >= 0;
            if (ok) {
                return recur(recur, b, j);
            }
            return v;
        };
        auto value = [&](auto& recur, i32 b, Int j) -> i32 {
            if (b == h) {
                return params[j];
            }
            Array<i32, 2> key = Array<i32, 2>{{b, convert<i32>(j)}};
            {
                auto found = lookupMap(memo, key);
                if (found.v1) {
                    return found.v0;
                }
            }
            auto found = lookupMap(bypasses, b);
            if (found.v1) {
                const Bypass d = found.v0;
                const i32 v = seen(recur, d.pred, d.args[j]);
                setMap(memo, key, v);
                return v;
            }
            auto temporary1253 = lookupMap(preds, b);
            Slice<i32> from = temporary1253.v0;
            bool ok = temporary1253.v1;
            if (!ok) {
                from = uniquePredecessors(f->blocks[b].preds);
                setMap(preds, b, from);
            }
            if (length(from) == 0) {
                i32 v = functionNewValue(f, OpUndef, f->values[params[j]].typ, -1, 0, Span<const i32>({}));
                setMap(memo, key, v);
                return v;
            }
            if (length(from) == 1) {
                i32 v = recur(recur, from[0], j);
                setMap(memo, key, v);
                return v;
            }
            Int at = length(f->blocks[b].params);
            i32 x = functionNewValue(f, OpParam, f->values[params[j]].typ, b, convert<i64>(at), Span<const i32>({}));
            f->blocks[b].params = appendValues(f->blocks[b].params, x);
            setMap(memo, key, x);
            auto range1254 = from;
            for (Int index1255 = 0, rangeEnd1256 = length(range1254); index1255 < rangeEnd1256; ++index1255) {
                i32 q = range1254[index1255];
                auto range1257 = f->blocks[q].term.edges;
                for (Int index1258 = 0, rangeEnd1259 = length(range1257); index1258 < rangeEnd1259; ++index1258) {
                    Int i = index1258;
                    Edge* e = &f->blocks[q].term.edges[i];
                    if (e->block == b) {
                        e->args = appendValues(e->args, x);
                    }
                }
            }
            auto range1260 = from;
            for (Int index1261 = 0, rangeEnd1262 = length(range1260); index1261 < rangeEnd1262; ++index1261) {
                i32 q = range1260[index1261];
                auto range1263 = f->blocks[q].term.edges;
                for (Int index1264 = 0, rangeEnd1265 = length(range1263); index1264 < rangeEnd1265; ++index1264) {
                    Int i = index1264;
                    if (f->blocks[q].term.edges[i].block == b) {
                        i32 arg = recur(recur, q, j);
                        f->blocks[q].term.edges[i].args[at] = arg;
                    }
                }
            }
            return x;
        };
        i32 b = 0;
        for (; b < convert<i32>(length(f->blocks)); b++) {
            if (b == h || f->blocks[b].dead) {
                continue;
            }
            auto temporary1266 = lookupMap(bypasses, b);
            bool isBypass = temporary1266.v1;
            auto at = [&](i32 v) mutable -> i32 {
                if (isBypass) {
                    return seen(value, bypasses[b].pred, v);
                }
                return seen(value, b, v);
            };
            auto instrs = f->blocks[b].instrs.view();
            for (Int j = 0; j < length(instrs); ++j) {
                i32 v = instrs[j];
                Span<i32> args = functionArgs(f, v);
                for (Int i = 0; i < length(args); ++i) {
                    i32 a = args[i];
                    bool ok = paramIndex(optimizerResolve(o, a)) >= 0;
                    if (ok) {
                        args[i] = at(a);
                    }
                }
            }
            Term* t = &f->blocks[b].term;
            if (t->kind == TermIf || t->kind == TermSwitch) {
                t->value = at(t->value);
            }
            for (Int i = 0, end = length(t->values); i < end; ++i) {
                t->values[i] = at(t->values[i]);
            }
            for (Int i = 0, end = length(t->edges); i < end; ++i) {
                auto range1280 = t->edges[i].args;
                for (Int index1281 = 0, rangeEnd1282 = length(range1280); index1281 < rangeEnd1282; ++index1281) {
                    Int k = index1281;
                    i32 a = range1280[index1281];
                    t->edges[i].args[k] = at(a);
                }
            }
        }
    }

    static void optimizerReplace(Optimizer* o, i32 v, i32 w) {
        while (length(o->alias) < length(o->f->values)) {
            o->alias.pushBack(-1);
        }
        if (v != w) {
            o->alias[v] = w;
            o->replaced++;
        }
    }

    static i32 optimizerResolve(Optimizer* o, i32 v) {
        while (convert<i64>(v) < length(o->alias) && o->alias[v] >= i32(0LL)) {
            v = o->alias[v];
        }
        return v;
    }

    // Replace short, non-trapping conditional computations by selects. Bit
    // tests often have unpredictable outcomes; float equality is also used by
    // scalar rounding code to apply a small correction on ties.
    static void optimizerSelectTriangles(Optimizer* o) {
        Function* f = o->f;
        Int round = 0;
        for (; round < 8; round++) {
            bool changed = false;
            auto rpo = functionRpo(f);
            for (Int i = 0; i < length(rpo); ++i) {
                i32 b = rpo[i];
                Block* blk = &f->blocks[b];
                Term t = blk->term;
                if (blk->dead || t.kind != TermIf) {
                    continue;
                }
                Int budget = 0;
                switch (f->values[t.value].op) {
                    case OpAnd: {
                        budget = 8;
                        break;
                    }
                    case OpFEq:
                    case OpFNe: {
                        budget = 4;
                        break;
                    }
                }
                if (budget == 0) {
                    continue;
                }
                Int side = 0;
                for (; side < 2; side++) {
                    Edge arm = t.edges[side];
                    Edge merge = t.edges[1 - side];
                    Block* body = &f->blocks[arm.block];
                    if (arm.block == b || body->dead || length(body->params) != 0 || length(body->preds) != 1 || body->preds[0] != b || body->term.kind != TermJump || body->term.edges[0].block != merge.block) {
                        continue;
                    }
                    if (length(body->instrs) > budget) {
                        continue;
                    }
                    bool pure = true;
                    auto instrs = body->instrs.view();
                    for (Int i = 0; i < length(instrs); ++i) {
                        i32 v = instrs[i];
                        Op op = f->values[v].op;
                        pure = pure && !readsState(op);
                        switch (op) {
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
                    auto params = f->blocks[merge.block].params;
                    for (Int i = 0; i < length(params); ++i) {
                        i32 p = params[i];
                        ValType typ = f->values[p].typ;
                        pure = pure && (typ == I32 || typ == I64 || typ == F32 || typ == F64);
                    }
                    if (!pure) {
                        continue;
                    }
                    Slice<i32> yes = body->term.edges[0].args;
                    Slice<i32> no = merge.args;
                    if (side == 1) {
                        auto temporary1298 = no;
                        auto temporary1299 = yes;
                        yes = temporary1298;
                        no = temporary1299;
                    }
                    auto range1300 = body->instrs.view();
                    for (Int index1301 = 0, rangeEnd1302 = length(range1300); index1301 < rangeEnd1302; ++index1301) {
                        i32 v = range1300[index1301];
                        f->values[v].block = b;
                        blk->instrs.pushBack(v);
                    }
                    Slice<i32> args = makeSlice<i32>(length(yes));
                    auto range1303 = yes;
                    for (Int index1304 = 0, rangeEnd1305 = length(range1303); index1304 < rangeEnd1305; ++index1304) {
                        Int j = index1304;
                        i32 x = range1303[index1304];
                        args[j] = x;
                        if (x != no[j]) {
                            i32 v = functionNewValue(f, OpSelect, f->values[x].typ, b, 0, Span<const i32>({t.value, x, no[j]}));
                            blk->instrs.pushBack(v);
                            args[j] = v;
                        }
                    }
                    blk->term = Term{TermJump, {}, sliceOf<Edge>({Edge{merge.block, args}}), {}, {}};
                    *body = Block{{}, {}, {}, {}, true};
                    functionComputePreds(f);
                    o->counted = false;
                    f->cfg = nullptr;
                    changed = true;
                    break;
                }
            }
            if (!changed) {
                break;
            }
        }
    }

    // setArgs replaces the operands of v.
    static void optimizerSetArgs(Optimizer* o, i32 v, Span<const i32> args) {
        Function* f = o->f;
        o->counted = false;
        Value* val = &f->values[v];
        if (convert<i64>(val->nargs) >= length(args)) {
            copySlice(subslice(f->pool, val->args, length(f->pool)), args);
            val->nargs = convert<u16>(length(args));
            return;
        }
        val->args = convert<i32>(length(f->pool));
        val->nargs = convert<u16>(length(args));
        appendSpan(f->pool, args);
    }

    // simplify folds constants and rewrites operations into simpler ones.
    static bool optimizerSimplify(Optimizer* o, Int from) {
        Function* f = o->f;
        bool changed = false;
        optimizerCountUses(o);
        for (Int b = from, end = length(f->blocks); b < end; ++b) {
            Block* blk = &f->blocks[b];
            if (blk->dead) {
                continue;
            }
            auto instrs = blk->instrs.view();
            for (Int i = 0; i < length(instrs); ++i) {
                i32 v = instrs[i];
                if (optimizerSimplifyValue(o, v)) {
                    changed = true;
                }
            }
            if (optimizerSimplifyTerm(o, convert<i32>(b))) {
                changed = true;
            }
        }
        optimizerApply(o);
        if (changed) {
            o->counted = false;
        }
        return changed;
    }

    static bool optimizerSimplifyTerm(Optimizer* o, i32 b) {
        Function* f = o->f;
        Term* t = &f->blocks[b].term;
        switch (t->kind) {
            case TermIf: {
                i32 c = optimizerResolve(o, t->value);
                t->value = c;
                Value* val = &f->values[c];
                if (val->op == OpConst) {
                    Edge e = t->edges[1];
                    if (val->aux != 0) {
                        e = t->edges[0];
                    }
                    *t = Term{TermJump, {}, sliceOf<Edge>({e}), {}, {}};
                    return true;
                }
                if (val->op == OpEqz && f->values[functionArgs(f, c)[0]].typ == I32) {
                    t->value = functionArgs(f, c)[0];
                    auto temporary1313 = t->edges[1];
                    auto temporary1314 = t->edges[0];
                    t->edges[0] = temporary1313;
                    t->edges[1] = temporary1314;
                    return true;
                }
                if (val->op == OpNe && optimizerIsConst(o, functionArgs(f, c)[1]) && optimizerConstOf(o, functionArgs(f, c)[1]) == 0 && f->values[functionArgs(f, c)[0]].typ == I32) {
                    t->value = functionArgs(f, c)[0];
                    return true;
                }
                if (t->edges[0].block == t->edges[1].block && sameArgs(o, t->edges[0].args, t->edges[1].args)) {
                    *t = Term{TermJump, {}, sliceOf<Edge>({t->edges[0]}), {}, {}};
                    return true;
                }
                break;
            }
            case TermSwitch: {
                i32 c = optimizerResolve(o, t->value);
                t->value = c;
                if (f->values[c].op == OpConst) {
                    u64 i = convert<u64>(convert<u32>(f->values[c].aux));
                    if (i >= convert<u64>(length(t->edges) - 1)) {
                        i = convert<u64>(length(t->edges) - 1);
                    }
                    *t = Term{TermJump, {}, sliceOf<Edge>({t->edges[i]}), {}, {}};
                    return true;
                }
                break;
            }
        }
        return false;
    }

    static bool optimizerSimplifyValue(Optimizer* o, i32 v) {
        Function* f = o->f;
        Value* val = &f->values[v];
        if (convert<i64>(v) < length(o->alias) && o->alias[v] >= 0) {
            return false;
        }
        Span<i32> args = functionArgs(f, v);
        for (Int i = 0; i < length(args); ++i) {
            i32 a = args[i];
            args[i] = optimizerResolve(o, a);
        }
        ValType t = val->typ;
        if (val->nargs == 2 && isIntOp(val->op) && val->op != OpEqz && !isFloat(f->values[args[0]].typ)) {
            i32 a = args[0];
            i32 b = args[1];
            ValType at = f->values[a].typ;
            if (optimizerIsConst(o, a) && optimizerIsConst(o, b)) {
                auto temporary1322 = foldInt(val->op, at, convert<u64>(optimizerConstOf(o, a)), convert<u64>(optimizerConstOf(o, b)));
                u64 r = temporary1322.v0;
                bool ok = temporary1322.v1;
                if (ok) {
                    optimizerReplace(o, v, optimizerConstant(o, t, convert<i64>(r)));
                    return true;
                }
                return false;
            }
            return optimizerIdentity(o, v, val->op, a, b, at);
        } else if (val->nargs == 1 && ((val->op >= OpClz && val->op <= OpEqz) || val->op == OpWrap || val->op == OpExtendU || val->op == OpExtendS)) {
            i32 a = args[0];
            ValType from = f->values[a].typ;
            if (isFloat(from)) {
                return false;
            }
            if (optimizerIsConst(o, a)) {
                auto temporary1323 = foldUnary(val->op, t, from, convert<u64>(optimizerConstOf(o, a)));
                u64 r = temporary1323.v0;
                bool ok = temporary1323.v1;
                if (ok) {
                    optimizerReplace(o, v, optimizerConstant(o, t, convert<i64>(r)));
                    return true;
                }
            }
            return optimizerUnaryIdentity(o, v, val->op, a);
        } else if (val->op == OpSelect) {
            i32 c = args[0];
            i32 x = args[1];
            i32 y = args[2];
            if (x == y) {
                optimizerReplace(o, v, x);
                return true;
            }
            if (optimizerIsConst(o, c)) {
                if (optimizerConstOf(o, c) != 0) {
                    optimizerReplace(o, v, x);
                } else {
                    optimizerReplace(o, v, y);
                }
                return true;
            }
        }
        return false;
    }

    // substitute maps arguments of an edge leaving block b to what they are on
    // an incoming edge with args: b's parameters become those.
    static Slice<i32> optimizerSubstitute(Optimizer* o, i32 b, Slice<i32> out, Slice<i32> in) {
        Slice<i32> res = makeSlice<i32>(length(out));
        for (Int i = 0; i < length(out); ++i) {
            i32 a = out[i];
            a = optimizerResolve(o, a);
            Int k = optimizerParamIndex(o, b, a);
            if (k >= 0) {
                res[i] = in[k];
            } else {
                res[i] = a;
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
        Function* f = o->f;
        bool changed = false;
        optimizerApply(o);
        optimizerCountUses(o);
        i32 h = 0;
        for (; h < convert<i32>(length(f->blocks)); h++) {
            Block* blk = &f->blocks[h];
            if (blk->dead || h == 0 || length(blk->instrs) != 0 || (blk->term.kind != TermSwitch && blk->term.kind != TermIf)) {
                continue;
            }
            Int k = optimizerParamIndex(o, h, optimizerResolve(o, blk->term.value));
            if (k < 0) {
                continue;
            }
            bool local = optimizerLocalParams(o, h);
            Map<i32, Bypass> bypasses = mapOf<i32, Bypass>({});
            TermKind kind = f->blocks[h].term.kind;
            Slice<Edge> targets = slicesClone(f->blocks[h].term.edges);
            auto preds = f->blocks[h].preds;
            for (Int j = 0; j < length(preds); ++j) {
                i32 p = preds[j];
                for (Int i = 0, end = length(f->blocks[p].term.edges); i < end; ++i) {
                    Edge e = f->blocks[p].term.edges[i];
                    if (e.block != h || length(e.args) <= k) {
                        continue;
                    }
                    i32 c = optimizerResolve(o, e.args[k]);
                    if (!optimizerIsConst(o, c)) {
                        continue;
                    }
                    u64 x = convert<u64>(convert<u32>(optimizerConstOf(o, c)));
                    Edge target = {};
                    if (kind == TermIf) {
                        target = targets[1];
                        if (x != 0) {
                            target = targets[0];
                        }
                    } else {
                        if (x >= convert<u64>(length(targets) - 1)) {
                            x = convert<u64>(length(targets) - 1);
                        }
                        target = targets[x];
                    }
                    Slice<i32> args = optimizerSubstitute(o, h, target.args, e.args);
                    changed = true;
                    if (local) {
                        f->blocks[p].term.edges[i] = Edge{target.block, args};
                        continue;
                    }
                    i32 b = functionNewBlock(f);
                    f->blocks[b].term = Term{TermJump, {}, sliceOf<Edge>({Edge{target.block, args}}), {}, {}};
                    setMap(bypasses, b, Bypass{p, appendSlice(Slice<i32>{}, e.args)});
                    f->blocks[p].term.edges[i] = Edge{b, {}};
                }
            }
            if (length(bypasses) > 0) {
                functionComputePreds(f);
                optimizerRepair(o, h, bypasses);
                o->counted = false;
                optimizerCountUses(o);
            }
        }
        if (changed) {
            functionComputePreds(f);
            o->counted = false;
            f->cfg = nullptr;
        }
        return changed;
    }

    // trivialParams replaces a parameter that receives one value on every edge,
    // apart from itself, by that value.
    static bool optimizerTrivialParams(Optimizer* o) {
        Function* f = o->f;
        bool changed = false;
        for (Int b = 0, end = length(f->blocks); b < end; ++b) {
            Block* blk = &f->blocks[b];
            if (blk->dead || b == 0 || length(blk->params) == 0) {
                continue;
            }
            Slice<bool> keep = {};
            bool removed = false;
            Vec<Edge*> in = optimizerIncoming(o, convert<i32>(b));
            auto params = blk->params;
            for (Int k = 0; k < length(params); ++k) {
                i32 param = params[k];
                i32 same = -1;
                bool trivial = true;
                auto range1342 = in.view();
                for (Int index1343 = 0, rangeEnd1344 = length(range1342); index1343 < rangeEnd1344; ++index1343) {
                    Edge* e = range1342[index1343];
                    i32 a = optimizerResolve(o, e->args[k]);
                    if (a == param || a == same) {
                        continue;
                    }
                    if (same >= 0) {
                        trivial = false;
                        break;
                    }
                    same = a;
                }
                if (isNil(keep)) {
                    keep = makeSlice<bool>(length(blk->params));
                }
                keep[k] = true;
                if (trivial) {
                    if (same < 0) {
                        same = functionNewValue(f, OpUndef, f->values[param].typ, -1, 0, Span<const i32>({}));
                    }
                    optimizerReplace(o, param, same);
                    keep[k] = false;
                    removed = true;
                }
            }
            if (!removed) {
                continue;
            }
            optimizerDropParams(o, convert<i32>(b), keep);
            changed = true;
        }
        if (changed) {
            optimizerApply(o);
        }
        return changed;
    }

    static bool optimizerUnaryIdentity(Optimizer* o, i32 v, Op op, i32 a) {
        Function* f = o->f;
        Value* src = &f->values[a];
        switch (op) {
            case OpEqz: {
                auto temporary1346 = invertComparison(src->op);
                Op invert = temporary1346.v0;
                bool ok = temporary1346.v1;
                if (ok && o->uses[a] == 1 && src->block == f->values[v].block) {
                    Span<i32> sa = functionArgs(f, a);
                    i32 x = sa[0];
                    i32 y = sa[1];
                    f->values[v].op = invert;
                    optimizerSetArgs(o, v, Span<const i32>({x, y}));
                    return true;
                }
                if (src->op == OpEqz && f->values[functionArgs(f, a)[0]].typ == I32) {
                    i32 x = functionArgs(f, a)[0];
                    f->values[v].op = OpNe;
                    optimizerSetArgs(o, v, Span<const i32>({x, optimizerConstant(o, I32, 0)}));
                    return true;
                }
                break;
            }
            case OpWrap: {
                switch (src->op) {
                    case OpExtendU:
                    case OpExtendS: {
                        optimizerReplace(o, v, functionArgs(f, a)[0]);
                        return true;
                    }
                    case OpAdd:
                    case OpSub:
                    case OpMul:
                    case OpAnd:
                    case OpOr:
                    case OpXor:
                    case OpShl: {
                        Span<i32> sa = functionArgs(f, a);
                        auto temporary1350 = optimizerLow32(o, sa[0]);
                        i32 x = temporary1350.v0;
                        bool ok1 = temporary1350.v1;
                        auto temporary1351 = optimizerLow32(o, sa[1]);
                        i32 y = temporary1351.v0;
                        bool ok2 = temporary1351.v1;
                        if (ok1 && ok2 && (src->op != OpShl || optimizerIsConst(o, sa[1]))) {
                            if (src->op == OpShl && (convert<u64>(optimizerConstOf(o, sa[1])) & 63) >= 32) {
                                return false;
                            }
                            f->values[v].op = src->op;
                            optimizerSetArgs(o, v, Span<const i32>({x, y}));
                            return true;
                        }
                        break;
                    }
                }
                break;
            }
            case OpExtendU: {
                if (src->op == OpWrap && optimizerIsZext(o, functionArgs(f, a)[0])) {
                    optimizerReplace(o, v, functionArgs(f, a)[0]);
                    return true;
                }
                break;
            }
        }
        return false;
    }

    static bool optimizerUnreachable(Optimizer* o) {
        Function* f = o->f;
        Slice<bool> reached = makeSlice<bool>(length(f->blocks));
        Slice<i32> work = sliceOf<i32>({0});
        reached[0] = true;
        while (length(work) > Int(0LL)) {
            i32 b = work[length(work) - 1];
            work = subslice(work, 0, length(work) - 1);
            auto edges = f->blocks[b].term.edges;
            for (Int i = 0; i < length(edges); ++i) {
                Edge e = edges[i];
                if (!reached[e.block]) {
                    reached[e.block] = true;
                    work = appendValues(work, e.block);
                }
            }
        }
        bool changed = false;
        for (Int b = 0, end = length(f->blocks); b < end; ++b) {
            if (!reached[b] && !f->blocks[b].dead) {
                f->blocks[b] = Block{{}, {}, {}, {}, true};
                changed = true;
            }
        }
        if (changed) {
            functionComputePreds(f);
        }
        return changed;
    }

    // unrollLoops replaces an innermost loop by its body as many times in a
    // row as it runs, when constants decide that: the loop leaves only from
    // its latch, what the latch tests follows from the constants the
    // preheader passes its header's parameters, and it runs at most
    // unrollTrips times, its body short. The copies see their variables as
    // constants, so the lanes of a loop over a pixel's channels become a
    // base and a constant each, which one check of the base covers.
    static bool optimizerUnrollLoops(Optimizer* o) {
        constexpr Int unrollTrips = 8;
        constexpr Int unrollInstructions = 640;
        Function* f = o->f;
        if (!f->hasLoops) {
            return false;
        }
        optimizerApply(o);
        CfgInfo* ci = functionAnalyze(f);
        const Int blocks = length(f->blocks);
        bool changed = false;
        auto order = ci->order;
        for (Int oi = 0; oi < length(order); ++oi) {
            const i32 h = order[oi];
            if (!ci->header[h] || h >= blocks) {
                continue;
            }
            i32 latch = -1;
            i32 preheader = -1;
            bool simple = true;
            auto preds = f->blocks[h].preds;
            for (Int i = 0; i < length(preds); ++i) {
                if (cfgInfoDominates(ci, h, preds[i])) {
                    simple = simple && latch < 0;
                    latch = preds[i];
                } else {
                    simple = simple && preheader < 0;
                    preheader = preds[i];
                }
            }
            if (!simple || latch < 0 || preheader < 0 || f->blocks[latch].term.kind != TermIf) {
                continue;
            }
            // The body: what reaches the latch back to the header, with no
            // other loop inside.
            Slice<bool> member = makeSlice<bool>(blocks);
            member[h] = true;
            Slice<i32> work = sliceOf<i32>({latch});
            Int insts = 0;
            while (simple && length(work) > 0) {
                const i32 b = work[length(work) - 1];
                work = subslice(work, 0, length(work) - 1);
                if (member[b]) {
                    continue;
                }
                if (ci->header[b] || ci->rpoIndex[b] < 0 || !cfgInfoDominates(ci, h, b)) {
                    simple = false;
                    break;
                }
                member[b] = true;
                work = appendSlice(work, f->blocks[b].preds);
            }
            if (!simple) {
                continue;
            }
            // The body accesses memory and calls nothing.
            Slice<i32> body = {};
            bool accesses = false;
            for (Int i = 0; i < length(order); ++i) {
                if (member[order[i]]) {
                    body = appendValues(body, order[i]);
                    auto instrs = f->blocks[order[i]].instrs.view();
                    insts += length(instrs);
                    for (Int j = 0; j < length(instrs); ++j) {
                        const Op op = f->values[instrs[j]].op;
                        accesses = accesses || op == OpLoad || op == OpStore;
                        simple = simple && op != OpCall && op != OpCallIndirect;
                    }
                }
            }
            if (!simple || !accesses) {
                continue;
            }
            // Only the latch leaves, to one block, and only it returns.
            Term* t = &f->blocks[latch].term;
            const Int back = t->edges[0].block == h ? 0 : 1;
            if (t->edges[back].block != h || member[t->edges[1 - back].block]) {
                continue;
            }
            const i32 exit = t->edges[1 - back].block;
            for (Int i = 0; i < length(body) && simple; ++i) {
                const Term& bt = f->blocks[body[i]].term;
                if (bt.kind == TermReturn || bt.kind == TermTrap) {
                    simple = false;
                }
                for (Int k = 0; k < length(bt.edges) && simple; ++k) {
                    const i32 to = bt.edges[k].block;
                    simple = (member[to] && (to != h || body[i] == latch)) || (body[i] == latch && to == exit);
                }
            }
            if (!simple) {
                continue;
            }
            // Run the latch's test on constants: the parameters the
            // preheader passes constants, and what the latch passes back.
            Edge entry = {};
            auto pedges = f->blocks[preheader].term.edges;
            for (Int k = 0; k < length(pedges); ++k) {
                if (pedges[k].block == h) {
                    entry = pedges[k];
                }
            }
            auto params = f->blocks[h].params;
            Slice<i64> known = makeSlice<i64>(length(params));
            Slice<bool> isKnown = makeSlice<bool>(length(params));
            for (Int j = 0; j < length(params); ++j) {
                const Value& a = f->values[entry.args[j]];
                isKnown[j] = a.op == OpConst;
                known[j] = a.aux;
            }
            Fn<Tuple2<u64, bool>(i32, Int)> eval = {};
            eval = [&](i32 v, Int depth) -> Tuple2<u64, bool> {
                const Value& val = f->values[v];
                if (val.op == OpConst) {
                    return {u64(val.aux) & mask(val.typ), true};
                }
                if (depth > 12 || val.block < 0 || !member[val.block] || (val.typ != I32 && val.typ != I64)) {
                    return {0, false};
                }
                if (val.op == OpParam) {
                    if (val.block != h || !isKnown[val.aux]) {
                        return {0, false};
                    }
                    return {u64(known[val.aux]) & mask(val.typ), true};
                }
                auto args = functionArgs(f, v);
                if (val.nargs == 2 && isIntOp(val.op) && val.op != OpEqz) {
                    auto a = eval(args[0], depth + 1);
                    auto b = eval(args[1], depth + 1);
                    if (!a.v1 || !b.v1) {
                        return {0, false};
                    }
                    return foldInt(val.op, f->values[args[0]].typ, a.v0, b.v0);
                }
                if (val.nargs == 1 && ((val.op >= OpClz && val.op <= OpEqz) || val.op == OpWrap || val.op == OpExtendU || val.op == OpExtendS)) {
                    auto a = eval(args[0], depth + 1);
                    if (!a.v1) {
                        return {0, false};
                    }
                    return foldUnary(val.op, val.typ, f->values[args[0]].typ, a.v0);
                }
                return {0, false};
            };
            Int trips = 1;
            for (;; ++trips) {
                auto c = eval(t->value, 0);
                if (!c.v1) {
                    trips = 0;
                    break;
                }
                if ((c.v0 != 0) != (back == 0)) {
                    break;
                }
                if (trips == unrollTrips) {
                    trips = 0;
                    break;
                }
                Slice<i64> next = makeSlice<i64>(length(params));
                Slice<bool> nextKnown = makeSlice<bool>(length(params));
                for (Int j = 0; j < length(params); ++j) {
                    auto x = eval(t->edges[back].args[j], 0);
                    nextKnown[j] = x.v1;
                    next[j] = i64(x.v0);
                }
                known = next;
                isKnown = nextKnown;
            }
            if (trips < 2 || trips * insts > unrollInstructions) {
                continue;
            }
            // Copy k reads, for the header's parameters, what copy k - 1
            // passes back; the last copy's latch goes to the exit.
            const Int values = length(f->values);
            // The copies take the body's terminators as they were: the
            // latch's changes as the copies are made.
            Slice<Term> terms = makeSlice<Term>(length(body));
            for (Int i = 0; i < length(body); ++i) {
                terms[i] = f->blocks[body[i]].term;
            }
            Slice<i32> map = makeSlice<i32>(values);
            for (Int v = 0; v < values; ++v) {
                map[v] = i32(v);
            }
                i32 previousLatch = latch;
            for (Int k = 1; k < trips; ++k) {
                Slice<i32> copy = makeSlice<i32>(blocks);
                for (Int i = 0; i < length(body); ++i) {
                    copy[body[i]] = functionNewBlock(f);
                }
                Slice<i32> backArgs = appendSlice(Slice<i32>{}, f->blocks[previousLatch].term.edges[back].args);
                for (Int j = 0; j < length(params); ++j) {
                    map[params[j]] = backArgs[j];
                }
                auto remap = [&](i32 v) {
                    return v < values ? map[v] : v;
                };
                for (Int i = 0; i < length(body); ++i) {
                    const i32 b = body[i];
                    const i32 nb = copy[b];
                    if (b != h) {
                        Slice<i32> ps = {};
                        auto bps = f->blocks[b].params;
                        for (Int j = 0; j < length(bps); ++j) {
                            const i32 n = functionNewValue(f, OpParam, f->values[bps[j]].typ, nb, f->values[bps[j]].aux, {});
                            map[bps[j]] = n;
                            ps = appendValues(ps, n);
                        }
                        f->blocks[nb].params = ps;
                    }
                    auto instrs = f->blocks[b].instrs.view();
                    for (Int j = 0; j < length(instrs); ++j) {
                        const i32 v = instrs[j];
                        const Value val = f->values[v];
                        Slice<i32> args = appendSlice(Slice<i32>{}, functionArgs(f, v));
                        for (Int a = 0; a < length(args); ++a) {
                            args[a] = remap(args[a]);
                        }
                        const i32 n = functionNewValue(f, val.op, val.typ, nb, val.aux, args);
                        f->values[n].aux2 = val.aux2;
                        map[v] = n;
                        f->blocks[nb].instrs.pushBack(n);
                    }
                }
                for (Int i = 0; i < length(body); ++i) {
                    const i32 b = body[i];
                    const Term src = terms[i];
                    Term dst = Term{src.kind, src.value, {}, {}, src.aux};
                    if (src.kind == TermIf || src.kind == TermSwitch) {
                        dst.value = remap(src.value);
                    }
                    for (Int j = 0; j < length(src.values); ++j) {
                        dst.values = appendValues(dst.values, remap(src.values[j]));
                    }
                    for (Int j = 0; j < length(src.edges); ++j) {
                        Slice<i32> args = {};
                        for (Int a = 0; a < length(src.edges[j].args); ++a) {
                            args = appendValues(args, remap(src.edges[j].args[a]));
                        }
                        const i32 to = src.edges[j].block;
                        dst.edges = appendValues(dst.edges, Edge{member[to] ? copy[to] : to, args});
                    }
                    f->blocks[copy[b]].term = dst;
                }
                // The latch before this copy enters it.
                f->blocks[previousLatch].term = Term{TermJump, {}, sliceOf<Edge>({Edge{copy[h], {}}}), {}, {}};
                previousLatch = copy[latch];
            }
            // The last copy leaves; every use past the loop reads its values.
            Edge out = f->blocks[previousLatch].term.edges[1 - back];
            f->blocks[previousLatch].term = Term{TermJump, {}, sliceOf<Edge>({out}), {}, {}};
            auto outside = [&](i32 v) {
                if (v >= values) {
                    return v;
                }
                const i32 b = f->values[v].block;
                return b >= 0 && b < blocks && member[b] ? map[v] : v;
            };
            for (Int b = 0; b < blocks; ++b) {
                if (member[b] || f->blocks[b].dead) {
                    continue;
                }
                auto instrs = f->blocks[b].instrs.view();
                for (Int j = 0; j < length(instrs); ++j) {
                    auto args = functionArgs(f, instrs[j]);
                    for (Int a = 0; a < length(args); ++a) {
                        args[a] = outside(args[a]);
                    }
                }
                Term* bt = &f->blocks[b].term;
                if (bt->kind == TermIf || bt->kind == TermSwitch) {
                    bt->value = outside(bt->value);
                }
                for (Int j = 0; j < length(bt->values); ++j) {
                    bt->values[j] = outside(bt->values[j]);
                }
                for (Int j = 0; j < length(bt->edges); ++j) {
                    for (Int a = 0; a < length(bt->edges[j].args); ++a) {
                        bt->edges[j].args[a] = outside(bt->edges[j].args[a]);
                    }
                }
            }
            changed = true;
            // The analysis no longer describes the function.
            break;
        }
        if (changed) {
            functionComputePreds(f);
            f->cfg = nullptr;
            o->counted = false;
        } else {
            f->cfg = ci;
        }
        return changed;
    }

    // disjointOrs makes an add of each or whose operands have no set bit in
    // common, as LLVM's disjoint or is before it becomes Wasm: the lanes of
    // a pixel, (p << 2 | i) << 1 past a row, are then a base and a constant.
    // A value's known trailing zeros come from constants, shifts, products
    // and sums, a block parameter's from what every edge passes it, from the
    // most each could have down to a fixed point; the other operand's bits
    // that may be set from constants, masks, narrow loads and shifts right.
    static bool optimizerDisjointOrs(Optimizer* o) {
        Function* f = o->f;
        optimizerApply(o);
        const Int n = length(f->values);
        auto width = [&](i32 v) -> Int {
            return f->values[v].typ == I64 ? 64 : 32;
        };
        // The bits a value may have set, from the lowest up.
        auto bits = [&](i32 v) -> Int {
            const Value& val = f->values[v];
            const Int w = width(v);
            switch (val.op) {
                case OpConst: {
                    const u64 c = u64(val.aux) & mask(val.typ);
                    return c == 0 ? 0 : 64 - Int(bitsLeadingZeros64(c));
                }
                case OpAnd: {
                    auto args = functionArgs(f, v);
                    for (Int k = 0; k < 2; ++k) {
                        if (f->values[args[k]].op == OpConst) {
                            const u64 c = u64(f->values[args[k]].aux) & mask(val.typ);
                            return c == 0 ? 0 : 64 - Int(bitsLeadingZeros64(c));
                        }
                    }
                    return w;
                }
                case OpLoad: {
                    const MemKind kind = convert<MemKind>(val.aux2);
                    const Int bytes = memKindWidth(kind);
                    return !memKindSigned(kind) && bytes * 8 < w ? bytes * 8 : w;
                }
                case OpShrU: {
                    auto args = functionArgs(f, v);
                    if (f->values[args[1]].op == OpConst) {
                        return w - Int(u64(f->values[args[1]].aux) & u64(w - 1));
                    }
                    return w;
                }
            }
            return isCompare(val.op) ? 1 : w;
        };
        // Only an or of a value with few bits that may be set can be one.
        bool any = false;
        for (Int v = 0; v < n && !any; ++v) {
            if (f->values[v].op == OpOr && f->values[v].block >= 0) {
                auto args = functionArgs(f, i32(v));
                any = bits(args[0]) < width(args[0]) || bits(args[1]) < width(args[1]);
            }
        }
        if (!any) {
            return false;
        }
        Slice<u8> zeros = makeSlice<u8>(n);
        for (Int v = 0; v < n; ++v) {
            zeros[v] = 64;
        }
        Slice<i32> order = functionRpo(f);
        Slice<Slice<Edge*>> incoming = makeSlice<Slice<Edge*>>(length(f->blocks));
        for (Int i = 0; i < length(order); ++i) {
            const i32 b = order[i];
            for (Int k = 0; k < length(f->blocks[b].term.edges); ++k) {
                Edge* e = &f->blocks[b].term.edges[k];
                incoming[e->block] = appendValues(incoming[e->block], e);
            }
        }
        auto known = [&](i32 v) -> Int {
            const Value& val = f->values[v];
            if (val.op == OpConst) {
                const u64 c = u64(val.aux) & mask(val.typ);
                return c == 0 ? width(v) : Int(bitsTrailingZeros64(c));
            }
            Int z = zeros[v];
            return z < width(v) ? z : width(v);
        };
        for (bool changed = true; changed;) {
            changed = false;
            for (Int i = 0; i < length(order); ++i) {
                const i32 b = order[i];
                auto params = f->blocks[b].params;
                for (Int j = 0; j < length(params); ++j) {
                    const i32 p = params[j];
                    Int z = 64;
                    auto edges = incoming[b];
                    for (Int k = 0; k < length(edges); ++k) {
                        const Int x = known(edges[k]->args[f->values[p].aux]);
                        z = x < z ? x : z;
                    }
                    if (i == 0 || length(edges) == 0) {
                        z = 0;
                    }
                    if (z < zeros[p]) {
                        zeros[p] = u8(z);
                        changed = true;
                    }
                }
                auto instrs = f->blocks[b].instrs.view();
                for (Int j = 0; j < length(instrs); ++j) {
                    const i32 v = instrs[j];
                    const Value& val = f->values[v];
                    if (val.typ != I32 && val.typ != I64) {
                        continue;
                    }
                    auto args = functionArgs(f, v);
                    Int z = 0;
                    switch (val.op) {
                        case OpAdd:
                        case OpSub:
                        case OpOr:
                        case OpXor: {
                            const Int x = known(args[0]);
                            const Int y = known(args[1]);
                            z = x < y ? x : y;
                            break;
                        }
                        case OpAnd: {
                            const Int x = known(args[0]);
                            const Int y = known(args[1]);
                            z = x > y ? x : y;
                            break;
                        }
                        case OpMul: {
                            z = known(args[0]) + known(args[1]);
                            break;
                        }
                        case OpShl: {
                            if (f->values[args[1]].op == OpConst) {
                                z = known(args[0]) + Int(u64(f->values[args[1]].aux) & u64(width(v) - 1));
                            }
                            break;
                        }
                        case OpSelect: {
                            const Int x = known(args[1]);
                            const Int y = known(args[2]);
                            z = x < y ? x : y;
                            break;
                        }
                        case OpExtendU:
                        case OpExtendS:
                        case OpWrap: {
                            z = known(args[0]);
                            break;
                        }
                    }
                    // Every value starts at the top and only falls.
                    z = z < width(v) ? z : width(v);
                    if (z < zeros[v]) {
                        zeros[v] = u8(z);
                        changed = true;
                    }
                }
            }
        }
        bool rewritten = false;
        for (Int v = 0; v < n; ++v) {
            Value& val = f->values[v];
            if (val.op != OpOr || val.block < 0 || (val.typ != I32 && val.typ != I64)) {
                continue;
            }
            auto args = functionArgs(f, i32(v));
            if (bits(args[1]) <= known(args[0]) || bits(args[0]) <= known(args[1])) {
                f->values[v].op = OpAdd;
                rewritten = true;
            }
        }
        if (rewritten) {
            o->counted = false;
        }
        return rewritten;
    }

    // outerConstants moves a constant at least zero out of an i32 sum to its
    // outside, (p + (q + c)) to ((p + q) + c) and (x + c) << s to
    // (x << s) + (c << s), when nothing else reads the inner value: an
    // address is then a base and a constant, the lowering's form, and the
    // addresses that differ in the constant alone share the base and its
    // check. A constant below zero stays, since the base would be past the
    // address.
    static bool optimizerOuterConstants(Optimizer* o) {
        Function* f = o->f;
        optimizerApply(o);
        optimizerCountUses(o);
        bool changed = false;
        auto constant = [&](i32 v, i64* c) {
            const Value& val = f->values[v];
            if (val.op != OpConst || val.typ != I32) {
                return false;
            }
            *c = i64(i32(u32(val.aux)));
            return true;
        };
        // inner gives x and c >= 0 of a sum x + c only v reads.
        auto inner = [&](i32 v, i32* x, i64* c) {
            const Value& val = f->values[v];
            if (val.op != OpAdd || val.typ != I32 || o->uses[v] != 1) {
                return false;
            }
            auto args = functionArgs(f, v);
            for (Int k = 0; k < 2; ++k) {
                if (constant(args[1 - k], c) && !constant(args[k], c) && *c >= 0) {
                    constant(args[1 - k], c);
                    *x = args[k];
                    return true;
                }
            }
            return false;
        };
        for (Int b = 0, end = length(f->blocks); b < end; ++b) {
            if (f->blocks[b].dead) {
                continue;
            }
            Vec<i32> out;
            auto instrs = f->blocks[b].instrs.view();
            bool rebuilt = false;
            for (Int i = 0; i < length(instrs); ++i) {
                const i32 v = instrs[i];
                const Value val = f->values[v];
                auto args = functionArgs(f, v);
                i32 x = -1;
                i64 c = 0;
                if (val.op == OpShl && val.typ == I32 && f->values[args[1]].op == OpConst && inner(args[0], &x, &c)) {
                    const u32 sh = u32(f->values[args[1]].aux) & 31;
                    const i64 moved = i64(i32(u32(c) << sh));
                    if (moved >= 0 && (u64(c) << sh) < u64(2147483648LL)) {
                        const i32 t = functionNewValue(f, OpShl, I32, i32(b), 0, Span<const i32>({x, args[1]}));
                        out.pushBack(t);
                        f->values[v].op = OpAdd;
                        optimizerSetArgs(o, v, Span<const i32>({t, optimizerConstant(o, I32, moved)}));
                        rebuilt = changed = true;
                    }
                } else if (val.op == OpAdd && val.typ == I32) {
                    for (Int k = 0; k < 2; ++k) {
                        i64 other = 0;
                        if (!constant(args[1 - k], &other) && inner(args[k], &x, &c)) {
                            const i32 t = functionNewValue(f, OpAdd, I32, i32(b), 0, Span<const i32>({args[1 - k], x}));
                            out.pushBack(t);
                            optimizerSetArgs(o, v, Span<const i32>({t, optimizerConstant(o, I32, c)}));
                            rebuilt = changed = true;
                            break;
                        }
                    }
                }
                out.pushBack(v);
            }
            if (rebuilt) {
                f->blocks[b].instrs = move(out);
            }
        }
        if (changed) {
            o->counted = false;
        }
        return changed;
    }

    static u8 readerByte(Reader* r) {
        if (r->pos >= length(r->data)) {
            compilerError(String("unexpected end at offset %d"));
        }
        u8 b = r->data[r->pos];
        r->pos++;
        return b;
    }

    static Slice<u8> readerBytes(Reader* r, Int n) {
        if (n < 0 || n > length(r->data) - r->pos) {
            compilerError(String("unexpected end at offset %d: need %d bytes"));
        }
        Slice<u8> b = subslice(r->data, r->pos, r->pos + n);
        r->pos += n;
        return b;
    }

    // constExpr returns the bytes of a constant expression up to and including
    // its end opcode.
    static Slice<u8> readerConstExpr(Reader* r) {
        Int start = r->pos;
        while (true) {
            u8 op = readerByte(r);
            switch (op) {
                case 11: {
                    return subslice(r->data, start, r->pos);
                }
                case 65: {
                    readerS32(r);
                    break;
                }
                case 66: {
                    readerS64(r);
                    break;
                }
                case 67: {
                    readerBytes(r, 4);
                    break;
                }
                case 68: {
                    readerBytes(r, 8);
                    break;
                }
                case 35:
                case 210: {
                    readerU32(r);
                    break;
                }
                case 208: {
                    readerRefType(r);
                    break;
                }
                case 106:
                case 107:
                case 108:
                case 124:
                case 125:
                case 126: {
                    break;
                }
                case 253: {
                    if (readerU32(r) != 12) {
                        compilerError(String("constant expression required at offset %d"));
                    }
                    readerBytes(r, 16);
                    break;
                }
                default: {
                    compilerError(String("constant expression required: opcode 0x%02x at offset %d"));
                    break;
                }
            }
        }
    }

    static bool readerEof(Reader* r) {
        return r->pos >= length(r->data);
    }

    static u32 readerF32(Reader* r) {
        Slice<u8> b = readerBytes(r, 4);
        return ((convert<u32>(b[0]) | shiftLeft(convert<u32>(b[1]), 8)) | shiftLeft(convert<u32>(b[2]), 16)) | shiftLeft(convert<u32>(b[3]), 24);
    }

    static u64 readerF64(Reader* r) {
        Slice<u8> b = readerBytes(r, 8);
        return convert<u64>(readerLe32(r, subslice(b, 0, 4))) | shiftLeft(convert<u64>(readerLe32(r, subslice(b, 4, 8))), 32);
    }

    static u32 readerLe32(Reader* r, Slice<u8> b) {
        return ((convert<u32>(b[0]) | shiftLeft(convert<u32>(b[1]), 8)) | shiftLeft(convert<u32>(b[2]), 16)) | shiftLeft(convert<u32>(b[3]), 24);
    }

    // leb reads an unsigned LEB128 number of at most bits bits.
    static u64 readerLeb(Reader* r, Uint bits) {
        u64 result = {};
        Uint shift = {};
        while (true) {
            u8 b = readerByte(r);
            if (shift + 7 >= bits && shiftRight(u8(b & 127), bits - shift) != 0) {
                compilerError(String("integer too large at offset %d"));
            }
            result |= shiftLeft(convert<u64>(u8(b & 127)), shift);
            if (u8(b & 128) == 0) {
                return result;
            }
            shift += 7;
            if (shift >= bits) {
                compilerError(String("integer representation too long at offset %d"));
            }
        }
    }

    static Limits readerLimits(Reader* r, bool memory) {
        u8 flags = readerByte(r);
        Limits l = {};
        if (flags > 7 || (!memory && flags > 1)) {
            compilerError(String("integer too large: limits flags 0x%02x"));
        }
        l.hasMax = u8(flags & 1) != 0;
        l.shared = u8(flags & 2) != 0;
        l.is64 = u8(flags & 4) != 0;
        if (l.is64) {
            compilerError(String("unsupported: memory64"));
        }
        Uint bits = 32;
        if (l.is64) {
            bits = 64;
        }
        l.min = readerLeb(r, bits);
        if (l.hasMax) {
            l.max = readerLeb(r, bits);
        }
        return l;
    }

    static String readerName(Reader* r) {
        u32 n = readerU32(r);
        Slice<u8> b = readerBytes(r, convert<i64>(n));
        if (!utf8Valid(b)) {
            compilerError(String("malformed UTF-8 encoding at offset %d"));
        }
        return asString(b);
    }

    static ValType readerRefType(Reader* r) {
        ValType t = convert<ValType>(readerByte(r));
        if (t != FuncRef && t != ExternRef) {
            compilerError(String("malformed reference type 0x%02x at offset %d"));
        }
        return t;
    }

    static i32 readerS32(Reader* r) {
        return convert<i32>(readerSleb(r, 32));
    }

    static i64 readerS64(Reader* r) {
        return readerSleb(r, 64);
    }

    // sleb reads a signed LEB128 number of at most bits bits.
    static i64 readerSleb(Reader* r, Uint bits) {
        i64 result = {};
        Uint shift = {};
        while (true) {
            u8 b = readerByte(r);
            if (shift + 7 >= bits) {
                i8 rest = shiftRight(convert<i8>(shiftLeft(b, 1)), bits - shift);
                if (u8(b & 128) != 0 || (rest != 0 && rest != -1)) {
                    if (u8(b & 128) != 0) {
                        compilerError(String("integer representation too long at offset %d"));
                    }
                    compilerError(String("integer too large at offset %d"));
                }
            }
            result |= shiftLeft(convert<i64>(u8(b & 127)), shift);
            shift += 7;
            if (u8(b & 128) == 0) {
                if (shift < 64 && u8(b & 64) != 0) {
                    result |= shiftLeft(i64(-1LL), shift);
                }
                return result;
            }
        }
    }

    static u32 readerU32(Reader* r) {
        if (r->pos < length(r->data) && r->data[r->pos] < 128) {
            u8 b = r->data[r->pos];
            r->pos++;
            return convert<u32>(b);
        }
        return convert<u32>(readerLeb(r, 32));
    }

    static ValType readerValType(Reader* r) {
        ValType t = convert<ValType>(readerByte(r));
        switch (t) {
            case I32:
            case I64:
            case F32:
            case F64:
            case V128:
            case FuncRef:
            case ExternRef: {
                return t;
            }
        }
        compilerError(String("malformed value type 0x%02x at offset %d"));
        return ValType(0LL);
    }

    static u64 xorshiftNext(Xorshift* r) {
        *r ^= shiftLeft(*r, 13);
        *r ^= shiftRight(*r, 7);
        *r ^= shiftLeft(*r, 17);
        return convert<u64>(*r);
    }

    static bool vRegIsPhys(VReg r) {
        return r >= VReg(0LL) && r < firstVirtual;
    }

    static bool vRegIsXMM(VReg r) {
        return r >= XMM0 && r < firstVirtual;
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

    // allocatorLiveness unions initialized sets of its one fixed nv.
    static bool bitsetUnion(Bitset s, Bitset o) {
        if (s.data->bits <= denseBitsetLimit) {
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
        bool changed = false;
        visitBitsetWords(o, [&](usize index, u64 word) {
            if (bitsetAddSparseWord(s.data, index, word)) {
                changed = true;
            }
        });
        return changed;
    }

    static bool memKindSigned(MemKind k) {
        return MemKind(k & memSigned) != MemKind(0LL);
    }

    static Int memKindWidth(MemKind k) {
        return convert<i64>(MemKind(k & MemKind(15LL)));
    }

    static Tuple2<AddressExpression, bool> addAddressExpression(AddressExpression a, AddressExpression b, u32 factor) {
        AddressExpression out = AddressExpression{{}, a.offset + factor * b.offset};
        auto range1366 = Array<Anonymous4, 2>{{Anonymous4{a, u32(1LL)}, Anonymous4{b, factor}}};
        for (Int index1367 = 0, rangeEnd1368 = length(range1366); index1367 < rangeEnd1368; ++index1367) {
            Anonymous4 source = range1366[index1367];
            auto terms = source.expr.terms;
            for (Int i = 0; i < length(terms); ++i) {
                AddressTerm term = terms[i];
                term.mul *= source.mul;
                if (term.mul == 0) {
                    continue;
                }
                Int at = -1;
                auto range1372 = out.terms;
                for (Int index1373 = 0, rangeEnd1374 = length(range1372); index1373 < rangeEnd1374; ++index1373) {
                    Int j = index1373;
                    AddressTerm prior = range1372[index1373];
                    if (prior.mul != 0 && prior.value == term.value) {
                        at = j;
                        break;
                    }
                }
                if (at < 0) {
                    auto range1375 = out.terms;
                    for (Int index1376 = 0, rangeEnd1377 = length(range1375); index1376 < rangeEnd1377; ++index1376) {
                        Int j = index1376;
                        AddressTerm prior = range1375[index1376];
                        if (prior.mul == 0) {
                            at = j;
                            out.terms[j].value = term.value;
                            break;
                        }
                    }
                }
                if (at < 0) {
                    return {AddressExpression{}, false};
                }
                out.terms[at].mul += term.mul;
            }
        }
        Int n = 0;
        for (Int j = 0, end = length(out.terms); j < end; ++j) {
            if (out.terms[j].mul != 0) {
                out.terms[n] = out.terms[j];
                n++;
            }
        }
        {
            Int j = n;
            for (; j < 4; j++) {
                out.terms[j] = AddressTerm{};
            }
        }
        Int j = 1;
        for (; j < n; j++) {
            Int k = j;
            for (; k > 0 && out.terms[k].value < out.terms[k - 1].value; k--) {
                auto temporary1381 = out.terms[k - 1];
                auto temporary1382 = out.terms[k];
                out.terms[k] = temporary1381;
                out.terms[k - 1] = temporary1382;
            }
        }
        return {out, true};
    }

    static LoopSpan addLoopSpans(LoopSpan a, LoopSpan b) {
        if (!a.valid || !b.valid || (a.factor != 0 && b.factor != 0 && a.stride != b.stride)) {
            return LoopSpan{};
        }
        LoopSpan out = LoopSpan{-1, {}, a.factor + b.factor, a.offset + b.offset, {}, {}, a.lo + b.lo, a.stride, true, a.signedTerms || b.signedTerms};
        if (a.factor == 0) {
            out.stride = b.stride;
        }
        auto mul2 = Array<Array<i64, 2>, 4>{{Array<i64, 2>{{convert<i64>(a.base), convert<i64>(a.mul)}}, Array<i64, 2>{{convert<i64>(a.base2), convert<i64>(a.mul2)}}, Array<i64, 2>{{convert<i64>(b.base), convert<i64>(b.mul)}}, Array<i64, 2>{{convert<i64>(b.base2), convert<i64>(b.mul2)}}}};
        for (Int i = 0; i < length(mul2); ++i) {
            Array<i64, 2> term = mul2[i];
            i32 v = convert<i32>(term[0]);
            u64 mul = convert<u64>(term[1]);
            if (mul == 0) {
                continue;
            }
            if (out.mul == 0 || out.base == v) {
                out.base = v;
                out.mul += mul;
            } else if (out.mul2 == 0 || out.base2 == v) {
                out.base2 = v;
                out.mul2 += mul;
            } else {
                return LoopSpan{};
            }
        }
        if (out.mul2 != 0 && out.base2 < out.base) {
            auto temporary1390 = out.base2;
            auto temporary1391 = out.base;
            out.base = temporary1390;
            out.base2 = temporary1391;
            auto temporary1392 = out.mul2;
            auto temporary1393 = out.mul;
            out.mul = temporary1392;
            out.mul2 = temporary1393;
        }
        return out;
    }

    static AddressExpression addressAtom(i32 v) {
        return AddressExpression{Array<AddressTerm, 4>{{AddressTerm{v, u32(1LL)}}}, {}};
    }

    static i32 alignTo(i32 x, i32 n) {
        return (x + n - 1) & ~(n - 1);
    }

    // allocatable reports registers the allocator may hand out.
    static bool allocatable(VReg r) {
        return r != RSP && r != RBP && r != R14 && r != ctxReg && r != memReg && r != VReg(31LL);
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
        allocatorDefinitions(a, true);
        allocatorBuildIntervals(a);
        allocatorScan(a);
        allocatorSpillAtJoins(a);
        allocatorConstantDefinitions(a);
        allocatorResolve(a);
        allocatorOrderSlots(a);
        allocatorCopies(a);
        allocatorDeadMoves(a);
        allocatorDeadSpills(a);
        allocatorDuplicateDispatch(a);
        allocatorNarrowMasks(a);
        return a;
    }

    static Slice<u8> appendLeb(Slice<u8> b, u64 v) {
        while (true) {
            u8 c = convert<u8>(v & 127);
            v = shiftRight(v, 7);
            if (v == 0) {
                return appendValues(b, c);
            }
            b = appendValues(b, u8(c | 128));
        }
    }

    static Slice<u8> appendSleb(Slice<u8> b, i64 v) {
        while (true) {
            u8 c = convert<u8>(v & 127);
            v = shiftRight(v, 7);
            if ((v == 0 && u8(c & 64) == 0) || (v == -1 && u8(c & 64) != 0)) {
                return appendValues(b, c);
            }
            b = appendValues(b, u8(c | 128));
        }
    }

    static Slice<u8> appendUleb(Slice<u8> out, u64 x) {
        return appendLeb(out, x);
    }

    static Slice<i32> appendUnique(Slice<i32> list, i32 x) {
        for (Int i = 0; i < length(list); ++i) {
            i32 y = list[i];
            if (y == x) {
                return list;
            }
        }
        return appendValues(list, x);
    }

    static Slice<VReg> assigned(Slice<VReg> regs) {
        Slice<VReg> out = {};
        for (Int i = 0; i < length(regs); ++i) {
            VReg r = regs[i];
            if (r != noReg) {
                out = appendValues(out, r);
            }
        }
        return out;
    }

    static Amem baseAt(VReg base, i32 disp) {
        return Amem{base, -1, u8(1LL), disp, -1, {}};
    }

    // breakPatterns_func scatters some elements around in an attempt to break some patterns
    // that might cause imbalanced partitions in quicksort.
    template <typename Data>
    static void breakPatterns_func(Data data, Int a, Int b) {
        Int lengthValue = b - a;
        if (lengthValue >= 8) {
            Xorshift random = convert<Xorshift>(lengthValue);
            Uint modulus = nextPowerOfTwo(lengthValue);
            Int idx = a + (lengthValue / 4) * 2 - 1;
            for (; idx <= a + (lengthValue / 4) * 2 + 1; idx++) {
                Int other = convert<i64>(convert<Uint>(xorshiftNext(&(random))) & (modulus - 1));
                if (other >= lengthValue) {
                    other -= lengthValue;
                }
                data.Swap(idx, a + other);
            }
        }
    }

    static Function* buildFunction(Module* m, Int index) {
        Func* fn = &m->funcs[index];
        Function* f = activePool->make<Function>();
        f->module = m;
        f->index = index;
        f->typ = &m->types[fn->typ];
        Frontend* fe = activePool->make<Frontend>(Frontend{m, f, Reader{fn->body, {}}, {}, {}, {}, {}, {}, {}, {}, {}, {}, mapOf<ConstKey, i32>({}), mapOf<i32, Slice<i32>>({}), {}});
        frontendRun(fe);
        return f;
    }

    // Carry successful memory checks through SSA edges and parameters. A
    // value is checked at a block's entry as far as it is at the end of
    // every predecessor, a parameter as far as every incoming argument is,
    // and an argument that is the sum of a value and a constant c >= 0 as
    // far as that value is, less c. A predecessor not yet reached admits
    // everything: the facts only shrink from there to the greatest fixed
    // point, which every reachable block's entry bounds; a parameter that
    // shrinks again after the first full round is dropped, so the rounds
    // end. A value's facts start at the block that defines it, and every
    // fact comes from an access done there or later.
    static Slice<Slice<CheckedValue>> checkedValues(Function* f, CfgInfo* ci) {
        Int limit = minimum(16384, 16777216 / length(f->blocks));
        if (limit == 0) {
            return {};
        }
        // The analysis keeps its tables in a pool of its own; only the proofs
        // go to the caller's.
        ObjPool* caller = activePool;
        ObjPool temporary;
        PoolScope scope(temporary);
        Slice<Slice<CheckedEdge>> incoming = makeSlice<Slice<CheckedEdge>>(length(f->blocks));
        auto order = ci->order;
        for (Int i = 0; i < length(order); ++i) {
            i32 b = order[i];
            for (Int j = 0, end = length(f->blocks[b].term.edges); j < end; ++j) {
                Edge* e = &f->blocks[b].term.edges[j];
                incoming[e->block] = appendValues(incoming[e->block], CheckedEdge{b, e});
            }
        }
        // sum gives the value and the constant c >= 0 a sum adds to it.
        auto sum = [&](i32 v, i32* base, i64* c) {
            if (f->values[v].op != OpAdd) {
                return false;
            }
            Span<i32> args = functionArgs(f, v);
            for (Int i = 0; i < 2; ++i) {
                const Value* k = &f->values[args[1 - i]];
                if (k->op == OpConst && f->values[args[i]].op != OpConst) {
                    *base = args[i];
                    *c = convert<i64>(convert<i32>(convert<u32>(k->aux)));
                    return *c >= 0;
                }
            }
            return false;
        };
        Slice<i32> tracked = makeSlice<i32>(length(f->values));
        for (Int j = 0, end = length(tracked); j < end; ++j) {
            tracked[j] = -1;
        }
        Slice<i32> values = {};
        Fn<void(i32)> add = {};
        add = [&](i32 v) mutable -> void {
            if (tracked[v] >= 0 || length(values) > limit) {
                return;
            }
            tracked[v] = convert<i32>(length(values));
            values = appendValues(values, v);
            Value* val = &f->values[v];
            if (val->op == OpParam) {
                auto range1409 = incoming[val->block];
                for (Int index1410 = 0, rangeEnd1411 = length(range1409); index1410 < rangeEnd1411; ++index1410) {
                    CheckedEdge e = range1409[index1410];
                    add(e.edge->args[val->aux]);
                }
            }
            i32 base = -1;
            i64 c = 0;
            if (sum(v, &base, &c)) {
                add(base);
            }
        };
        // An access the lowering proves from the ranges of its address
        // checks nothing, its base least of all: a base whose range dips
        // below zero may have wrapped around while the sum did not.
        const bool own = length(f->module->mems) > 0 && !f->module->mems[0].imported;
        ValueRanges* ranges = nullptr;
        auto inRange = [&](i32 addr, i64 end) {
            if (!own) {
                return false;
            }
            if (!ranges) {
                ranges = activePool->make<ValueRanges>(ValueRanges{f, makeSlice<ValueRange>(length(f->values)), makeSlice<bool>(length(f->values))});
            }
            const ValueRange range = valueRangesOf(ranges, addr, 0);
            return range.lo >= 0 && range.hi + end <= i64(f->module->mems[0].limits.min * 65536 + f->module->memoryTail);
        };
        // The accesses of the blocks in order, each with its address and the
        // end it checks; from blocks[i] on are those of the block order[i].
        Vec<CheckedValue> accesses;
        Vec<i32> blocks;
        auto range1412 = ci->order;
        for (Int index1413 = 0, rangeEnd1414 = length(range1412); index1413 < rangeEnd1414; ++index1413) {
            i32 b = range1412[index1413];
            blocks.pushBack(i32(accesses.size()));
            auto instrs = f->blocks[b].instrs.view();
            for (Int i = 0; i < length(instrs); ++i) {
                i32 v = instrs[i];
                Value* val = &f->values[v];
                if (val->op == OpLoad || val->op == OpStore) {
                    i32 addr = functionArgs(f, v)[0];
                    const MemKind kind = convert<MemKind>(val->aux2);
                    const i64 end = val->aux + convert<i64>(memKindWidth(kind));
                    i32 base = -1;
                    i64 c = 0;
                    if (windowBase(f, addr, end, &base, &c)) {
                        if (MemKind(kind & memProven) == MemKind(0LL) && c + end > 0 && !inRange(addr, end)) {
                            accesses.pushBack(CheckedValue{base, c + end});
                            if (f->values[base].op == OpParam) {
                                add(base);
                            }
                        }
                        if (c >= 0) {
                            accesses.pushBack(CheckedValue{addr, end});
                        }
                    } else {
                        accesses.pushBack(CheckedValue{addr, end});
                    }
                    if (f->values[addr].op == OpParam) {
                        add(addr);
                    }
                }
            }
        }
        blocks.pushBack(i32(accesses.size()));
        const Int n = length(values);
        if (n == 0 || n > limit) {
            return {};
        }
        // Only accesses through tracked values check anything.
        usize kept = 0;
        for (usize i = 0, at = 0; i + 1 < blocks.size(); ++i) {
            const usize end = usize(blocks[i + 1]);
            blocks[i] = i32(kept);
            for (; at < end; ++at) {
                const i32 j = tracked[accesses[at].value];
                if (j >= 0) {
                    accesses[kept++] = CheckedValue{j, accesses[at].end};
                }
            }
        }
        blocks[blocks.size() - 1] = i32(kept);
        Slice<Slice<Int>> defs = makeSlice<Slice<Int>>(length(f->blocks));
        auto range1418 = values;
        for (Int index1419 = 0, rangeEnd1420 = length(range1418); index1419 < rangeEnd1420; ++index1419) {
            Int j = index1419;
            i32 v = range1418[index1419];
            i32 b = f->values[v].block;
            if (b >= 0) {
                defs[b] = appendValues(defs[b], j);
            }
        }
        // Each tracked argument that is a sum: the tracked index of its value
        // and the constant, or -1.
        Slice<i32> sumBase = makeSlice<i32>(n);
        Slice<i64> sumConst = makeSlice<i64>(n);
        for (Int j = 0; j < n; ++j) {
            i32 base = -1;
            i64 c = 0;
            sumBase[j] = sum(values[j], &base, &c) ? tracked[base] : -1;
            sumConst[j] = c;
        }
        // A block's row lists the tracked values checked at its end, by index
        // in increasing order, with how far each is checked. Few values are
        // checked in any one block, so rows hold only those. The current
        // block's ends are kept by index, with the indices it has set.
        Slice<Slice<CheckedValue>> rows = makeSlice<Slice<CheckedValue>>(length(f->blocks));
        Slice<bool> reached = makeSlice<bool>(length(f->blocks));
        Slice<i64> ends = makeSlice<i64>(n);
        Slice<u32> marks = makeSlice<u32>(n);
        u32 epoch = 0;
        Slice<i32> touched = makeSlice<i32>(n);
        Int touchedCount = 0;
        Slice<CheckedValue> row = makeSlice<CheckedValue>(n);
        Int rowCount = 0;
        // How far each parameter was checked the round before, or -1.
        Slice<i64> before = makeSlice<i64>(n);
        for (Int j = 0; j < n; ++j) {
            before[j] = -1;
        }
        Int round = 0;
        auto set = [&](i32 j, i64 end) {
            ends[j] = end;
            if (marks[j] != epoch) {
                marks[j] = epoch;
                touched[touchedCount++] = j;
            }
        };
        auto lookup = [&](Slice<CheckedValue> checked, i32 j) -> i64 {
            Int low = 0;
            Int high = length(checked);
            while (low < high) {
                const Int middle = (low + high) / 2;
                if (checked[middle].value < j) {
                    low = middle + 1;
                } else {
                    high = middle;
                }
            }
            return low < length(checked) && checked[low].value == j ? checked[low].end : 0;
        };
        // reach is how far argument j is checked at the end of block p.
        auto reach = [&](i32 p, i32 j) -> i64 {
            if (j < 0) {
                return 0;
            }
            i64 end = lookup(rows[p], j);
            if (sumBase[j] >= 0) {
                end = maximum(end, lookup(rows[p], sumBase[j]) - sumConst[j]);
            }
            return end;
        };
        auto entry = [&](i32 b) {
            for (Int t = 0; t < touchedCount; ++t) {
                ends[touched[t]] = 0;
            }
            touchedCount = 0;
            ++epoch;
            bool first = true;
            if (b != 0) {
                const auto preds = f->blocks[b].preds;
                for (Int k = 0; k < length(preds); ++k) {
                    const i32 p = preds[k];
                    if (ci->rpoIndex[p] < 0 || !reached[p]) {
                        continue;
                    }
                    const Slice<CheckedValue> checked = rows[p];
                    if (first) {
                        for (Int i = 0; i < length(checked); ++i) {
                            set(checked[i].value, checked[i].end);
                        }
                    } else {
                        // The indices set so far are in increasing order; an
                        // index the row lacks is unchecked.
                        Int i = 0;
                        for (Int t = 0; t < touchedCount; ++t) {
                            const i32 j = touched[t];
                            while (i < length(checked) && checked[i].value < j) {
                                ++i;
                            }
                            ends[j] = i < length(checked) && checked[i].value == j ? minimum(ends[j], checked[i].end) : 0;
                        }
                    }
                    first = false;
                }
            }
            auto range1427 = defs[b];
            for (Int index1428 = 0, rangeEnd1429 = length(range1427); index1428 < rangeEnd1429; ++index1428) {
                Int j = range1427[index1428];
                ends[j] = 0;
            }
            auto range1430 = f->blocks[b].params;
            for (Int index1431 = 0, rangeEnd1432 = length(range1430); index1431 < rangeEnd1432; ++index1431) {
                i32 p = range1430[index1431];
                i32 j = tracked[p];
                if (j < 0 || b == 0 || length(incoming[b]) == 0) {
                    continue;
                }
                i64 end = i64(4611686018427387904LL);
                auto range1433 = incoming[b];
                for (Int index1434 = 0, rangeEnd1435 = length(range1433); index1434 < rangeEnd1435; ++index1434) {
                    CheckedEdge e = range1433[index1434];
                    if (ci->rpoIndex[e.pred] < 0 || !reached[e.pred]) {
                        continue;
                    }
                    end = minimum(end, reach(e.pred, tracked[e.edge->args[f->values[p].aux]]));
                }
                if (end == i64(4611686018427387904LL)) {
                    end = 0;
                }
                if (round >= 2 && before[j] >= 0 && end < before[j]) {
                    end = 0;
                }
                before[j] = end;
                set(j, end);
            }
        };
        // collect fills row with the current block's checked values in
        // increasing index order.
        auto collect = [&]() {
            rowCount = 0;
            for (Int t = 0; t < touchedCount; ++t) {
                const i32 j = touched[t];
                if (ends[j] > 0) {
                    row[rowCount++] = CheckedValue{j, ends[j]};
                }
            }
            for (Int i = 1; i < rowCount; ++i) {
                const CheckedValue item = row[i];
                Int at = i;
                while (at > 0 && row[at - 1].value > item.value) {
                    row[at] = row[at - 1];
                    --at;
                }
                row[at] = item;
            }
        };
        // A round visits, in order, the blocks whose predecessors changed
        // since they were visited, the first round all; a block keeps the
        // facts of its entry from its last visit.
        Slice<bool> dirty = makeSlice<bool>(length(f->blocks));
        for (Int i = 0; i < length(order); ++i) {
            dirty[order[i]] = true;
        }
        Slice<Slice<CheckedValue>> entries = makeSlice<Slice<CheckedValue>>(length(f->blocks));
        bool stable = false;
        for (; round < 64 && !stable; round++) {
            stable = true;
            for (Int i = 0; i < length(order); ++i) {
                i32 b = order[i];
                if (!dirty[b]) {
                    continue;
                }
                dirty[b] = false;
                entry(b);
                Slice<CheckedValue> at = subslice(entries[b], 0, 0);
                for (Int t = 0; t < touchedCount; ++t) {
                    if (ends[touched[t]] > 0) {
                        at = appendValues(at, CheckedValue{touched[t], ends[touched[t]]});
                    }
                }
                entries[b] = at;
                for (usize k = usize(blocks[usize(i)]), end = usize(blocks[usize(i + 1)]); k < end; ++k) {
                    const CheckedValue access = accesses[k];
                    set(access.value, maximum(ends[access.value], access.end));
                }
                collect();
                const Slice<CheckedValue> old = rows[b];
                bool same = reached[b] && length(old) == rowCount;
                for (Int k = 0; same && k < rowCount; ++k) {
                    same = old[k].value == row[k].value && old[k].end == row[k].end;
                }
                if (!same) {
                    stable = false;
                    rows[b] = appendSlice(Slice<CheckedValue>{}, subslice(row, 0, rowCount));
                    auto edges = f->blocks[b].term.edges;
                    for (Int k = 0; k < length(edges); ++k) {
                        dirty[edges[k].block] = true;
                    }
                }
                reached[b] = true;
            }
        }
        // Facts that still shrink may not hold.
        if (!stable) {
            return {};
        }
        // A block dominates those numbered from its own place in the
        // dominator tree's preorder through its subtree.
        DominatorChildren children = cfgInfoChildren(ci);
        Slice<i32> enter = makeSlice<i32>(length(f->blocks));
        Slice<i32> through = makeSlice<i32>(length(f->blocks));
        Vec<i32> preorder;
        Vec<i32> stack;
        stack.pushBack(order[0]);
        while (stack.size() > 0) {
            const i32 b = stack.popBack();
            enter[b] = convert<i32>(preorder.size());
            preorder.pushBack(b);
            for (i32 c = children.first[usize(b)]; c >= 0; c = children.next[usize(c)]) {
                stack.pushBack(c);
            }
        }
        for (usize i = 0; i < preorder.size(); ++i) {
            through[preorder[i]] = enter[preorder[i]];
        }
        for (usize i = preorder.size(); i-- > 1;) {
            const i32 b = preorder[i];
            through[ci->idom[b]] = maximum(through[ci->idom[b]], through[b]);
        }
        PoolScope results(*caller);
        Slice<Slice<CheckedValue>> proofs = makeSlice<Slice<CheckedValue>>(length(f->blocks));
        auto range1445 = ci->order;
        for (Int index1446 = 0, rangeEnd1447 = length(range1445); index1446 < rangeEnd1447; ++index1446) {
            i32 b = range1445[index1446];
            auto facts = entries[b];
            for (Int k = 0; k < length(facts); ++k) {
                const i64 end = facts[k].end;
                i32 v = values[facts[k].value];
                i32 owner = f->values[v].block;
                if (owner < 0 || (enter[owner] <= enter[b] && enter[b] <= through[owner])) {
                    proofs[b] = appendValues(proofs[b], CheckedValue{v, end});
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
        constexpr Int shortestNinther = 50;
        constexpr Int maxSwaps = 12;
        Int l = b - a;
        Int swaps = {};
        Int i = a + (l / 4) * 1;
        Int j = a + (l / 4) * 2;
        Int k = a + (l / 4) * 3;
        if (l >= 8) {
            if (l >= 50) {
                i = medianAdjacent_func(data, i, &(swaps));
                j = medianAdjacent_func(data, j, &(swaps));
                k = medianAdjacent_func(data, k, &(swaps));
            }
            j = median_func(data, i, j, k, &(swaps));
        }
        switch (swaps) {
            case 0: {
                return {j, increasingHint};
            }
            case 12: {
                return {j, decreasingHint};
            }
            default: {
                return {j, unknownHint};
            }
        }
    }

    // moduleLowFree tells whether the module keeps nothing in the first
    // boundSlack bytes of its own memory, which compilation with the tail
    // requires: every active data segment at a constant address of
    // boundSlack or more (a passive one is placed by memory.init, as a store
    // is), and its stack, when the first global is a mutable i32 with a
    // constant start, as wasm-ld's __stack_pointer is, starting at boundSlack
    // or more. wasm-ld places data at 1024 by default; our toolchain links
    // the stack first, its lowest boundSlack bytes a red zone past the
    // program's budget. An address that wrapped around 2^32 is then out of
    // bounds, and a sum's base may be checked for it (lowererAddress).
    static bool moduleLowFree(Module* m) {
        if (length(m->mems) == 0 || m->mems[0].imported) {
            return false;
        }
        for (Int i = 0; i < length(m->datas); ++i) {
            Data d = m->datas[i];
            if (d.mode != segmentActive) {
                continue;
            }
            auto offset = constOffset(d.offset);
            if (!offset.v1 || offset.v0 < boundSlack) {
                return false;
            }
        }
        if (length(m->globals) == 0) {
            return true;
        }
        Global g = m->globals[0];
        if (g.imported || g.typ != I32 || !g.mutableValue) {
            return true;
        }
        auto stack = constOffset(g.init);
        return !stack.v1 || stack.v0 >= boundSlack;
    }

    // constOffset evaluates a constant offset expression: i32.const or
    // i64.const.
    static Tuple2<i64, bool> constOffset(Slice<u8> expr) {
        Reader reader{expr, {}};
        Reader* r = &reader;
        u8 op = readerByte(r);
        i64 v = {};
        switch (op) {
            case 65: {
                v = convert<i64>(convert<u32>(readerS32(r)));
                break;
            }
            case 66: {
                v = readerS64(r);
                break;
            }
            default: {
                return {i64(0LL), false};
            }
        }
        return {v, readerByte(r) == 11 && readerEof(r)};
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
        return constCount + convert<i32>(seg);
    }

    static Module* decodeModule(Slice<u8> data) {
        Reader* r = activePool->make<Reader>(Reader{data, {}});
        Module* m = activePool->make<Module>(Module{{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, i64(-1LL), {}, {}, i64(-1LL), {}, {}, {}, {}});
        if ((asString(readerBytes(r, 4)) != String("\000asm"))) {
            compilerError(String("magic header not detected"));
        }
        if (readerF32(r) != 1) {
            compilerError(String("unknown binary version"));
        }
        Slice<u32> funcTypes = {};
        Int codes = {};
        u8 last = 0;
        while (!readerEof(r)) {
            u8 id = readerByte(r);
            u32 size = readerU32(r);
            Reader* body = activePool->make<Reader>(Reader{readerBytes(r, convert<i64>(size)), {}});
            Int base = r->pos - convert<i64>(size);
            if (id != 0) {
                if (order(id) <= order(last)) {
                    compilerError(String("unexpected content after last section"));
                }
                last = id;
            }
            switch (id) {
                case 0: {
                    moduleCustom(m, body);
                    break;
                }
                case 1: {
                    moduleTypeSection(m, body);
                    break;
                }
                case 2: {
                    moduleImportSection(m, body);
                    break;
                }
                case 3: {
                    u32 n = readerU32(body);
                    u32 i = 0;
                    for (; i < n; i++) {
                        funcTypes = appendValues(funcTypes, readerU32(body));
                    }
                    break;
                }
                case 4: {
                    u32 n = readerU32(body);
                    u32 i = 0;
                    for (; i < n; i++) {
                        ValType elem = readerRefType(body);
                        m->tables = appendValues(m->tables, Table{elem, readerLimits(body, false), {}});
                    }
                    break;
                }
                case 5: {
                    u32 n = readerU32(body);
                    u32 i = 0;
                    for (; i < n; i++) {
                        m->mems = appendValues(m->mems, Memory{readerLimits(body, true), {}});
                    }
                    break;
                }
                case 6: {
                    u32 n = readerU32(body);
                    u32 i = 0;
                    for (; i < n; i++) {
                        Global g = Global{readerValType(body), {}, {}, {}};
                        g.mutableValue = globalMutability(body);
                        g.init = readerConstExpr(body);
                        m->globals = appendValues(m->globals, g);
                    }
                    break;
                }
                case 7: {
                    moduleExportSection(m, body);
                    break;
                }
                case 8: {
                    m->start = convert<i64>(readerU32(body));
                    break;
                }
                case 9: {
                    moduleElemSection(m, body);
                    break;
                }
                case 10: {
                    Int n = convert<i64>(readerU32(body));
                    if (n != length(funcTypes)) {
                        compilerError(String("function and code section have inconsistent lengths"));
                    }
                    Int i = 0;
                    m->codeOffset = base;
                    for (; i < n; i++) {
                        Int size = convert<i64>(readerU32(body));
                        Int offset = base + body->pos;
                        m->funcs = appendValues(m->funcs, Func{funcTypes[i], {}, {}, {}, readerBytes(body, size), offset});
                    }
                    codes = n;
                    break;
                }
                case 11: {
                    moduleDataSection(m, body);
                    break;
                }
                case 12: {
                    m->dataCount = convert<i64>(readerU32(body));
                    break;
                }
                default: {
                    compilerError(String("malformed section id %d"));
                    break;
                }
            }
            if (!readerEof(body)) {
                compilerError(String("section size mismatch in section %d"));
            }
        }
        if (codes != length(funcTypes)) {
            compilerError(String("function and code section have inconsistent lengths"));
        }
        if (m->dataCount >= 0 && convert<i64>(m->dataCount) != length(m->datas)) {
            compilerError(String("data count and data section have inconsistent lengths"));
        }
        for (Int i = 0, end = length(m->funcs); i < end; ++i) {
            if (convert<i64>(m->funcs[i].typ) >= length(m->types)) {
                compilerError(String("unknown type %d"));
            }
        }
        if (length(m->mems) > 1) {
            compilerError(String("unsupported: %d memories"));
        }
        auto datas = m->datas;
        for (Int i = 0; i < length(datas); ++i) {
            Data d = datas[i];
            if (d.memory != 0) {
                compilerError(String("unsupported: data segment %d of memory %d"));
            }
        }
        return m;
    }

    // displayName keeps a name readable in tracebacks: only control bytes,
    // which a symbol table cannot hold or show, become _XX.
    static String displayName(String s) {
        Slice<u8> b = makeSlice<u8>(0, length(s));
        Int i = 0;
        for (; i < length(s); i++) {
            u8 c = s[i];
            if (c < 32 || c == 127) {
                b = appendSlice(b, fmtSprintf(String("_%02X"), c));
            } else {
                b = appendValues(b, c);
            }
        }
        return asString(b);
    }

    static Span<Move> dropSelfMoves(Span<Move> moves) {
        Int count = 0;
        for (Int i = 0; i < length(moves); ++i) {
            Move m = moves[i];
            if (m.dst != m.src) {
                moves[count++] = m;
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
            while ((Int(4LL) + length(body)) % Int(8LL) != Int(0LL)) {
                body = appendValues(body, u8(0LL));
            }
            out = littleAppendUint32(out, convert<u32>(length(body)));
            out = appendSlice(out, body);
        };
        Slice<u8> cie = sliceOf<u8>({u8(0LL), u8(0LL), u8(0LL), u8(0LL), u8(1LL), u8(122LL), u8(82LL), u8(0LL), u8(1LL), u8(120LL), u8(16LL), u8(1LL), u8(27LL), u8(12LL), u8(7LL), u8(8LL), u8(144LL), u8(1LL)});
        entry(cie);
        for (Int i = 0; i < length(regions); ++i) {
            Region r = regions[i];
            Int at = length(out);
            Slice<u8> body = littleAppendUint32({}, convert<u32>(at + 4));
            starts = appendValues(starts, convert<u32>(at + 8));
            body = littleAppendUint32(body, 0);
            body = littleAppendUint32(body, r.end - r.start);
            body = appendValues(body, u8(0LL));
            i32 loc = 0;
            auto sp = r.sp;
            for (Int k = 0; k < length(sp); ++k) {
                SpEntry e = sp[k];
                if (k == 0 && e.delta == 0) {
                    continue;
                }
                i32 d = e.pc - loc;
                if (d == 0) {
                } else if (d < 64) {
                    body = appendValues(body, u8(64 | convert<u8>(d)));
                } else if (d < 256) {
                    body = appendValues(body, u8(2LL), convert<u8>(d));
                } else if (d < 65536) {
                    body = littleAppendUint16(appendValues(body, u8(3LL)), convert<u16>(d));
                } else {
                    body = littleAppendUint32(appendValues(body, u8(4LL)), convert<u32>(d));
                }
                loc = e.pc;
                body = appendUleb(appendValues(body, u8(14LL)), convert<u64>(e.delta + 8));
            }
            entry(body);
        }
        return {out, starts};
    }

    // encodeNote writes the descriptor: imports and exports of functions, the
    // init symbol and the regions are not in it.
    static Slice<u8> encodeNote(Meta* m) {
        MetaWriter* w = activePool->make<MetaWriter>(MetaWriter{});
        metaWriterU(w, convert<u64>(m->instanceSize));
        metaWriterFlag(w, !isNil(m->memory));
        if (!isNil(m->memory)) {
            metaWriterU(w, m->memory->min);
            metaWriterU(w, m->memory->max);
            metaWriterFlag(w, m->memory->imported);
        }
        metaWriterU(w, convert<u64>(length(m->tables)));
        auto tables = m->tables;
        for (Int i = 0; i < length(tables); ++i) {
            TableMeta t = tables[i];
            metaWriterU(w, convert<u64>(t.offset));
            metaWriterU(w, t.min);
            metaWriterU(w, t.max);
            metaWriterFlag(w, t.imported);
        }
        metaWriterU(w, convert<u64>(length(m->globals)));
        auto globals = m->globals;
        for (Int i = 0; i < length(globals); ++i) {
            GlobalMeta g = globals[i];
            metaWriterU(w, convert<u64>(g.offset));
            metaWriterU(w, convert<u64>(g.typ));
            metaWriterFlag(w, g.mutableValue);
            metaWriterFlag(w, g.imported);
        }
        metaWriterU(w, convert<u64>(length(m->imports)));
        auto imports = m->imports;
        for (Int j = 0; j < length(imports); ++j) {
            ImportMeta i = imports[j];
            metaWriterU(w, convert<u64>(i.kind));
            metaWriterStr(w, i.module);
            metaWriterStr(w, i.name);
            metaWriterU(w, convert<u64>(i.index));
        }
        metaWriterU(w, convert<u64>(length(m->exports)));
        auto exports = m->exports;
        for (Int i = 0; i < length(exports); ++i) {
            ExportMeta e = exports[i];
            metaWriterU(w, convert<u64>(e.kind));
            metaWriterStr(w, e.name);
            metaWriterU(w, convert<u64>(e.index));
        }
        // What follows is optional, by its flags: the accessible tail and its
        // slot, the tail's profile counters, the shadow map's slot, the
        // source lines.
        const bool tail = !isNil(m->memory) && m->memory->tail != 0;
        const bool profile = tail && m->memory->profile != 0;
        const bool asan = !isNil(m->memory) && m->memory->shadow != 0;
        const bool lines = length(m->lines) != 0;
        metaWriterU(w, (tail ? 1 : 0) | (profile ? 2 : 0) | (asan ? 4 : 0) | (lines ? 8 : 0));
        if (tail) {
            metaWriterU(w, m->memory->tail);
            metaWriterU(w, convert<u64>(m->memory->tailEnd));
        }
        if (profile) {
            metaWriterU(w, convert<u64>(m->memory->profile));
            metaWriterU(w, convert<u64>(m->memory->profileFuncs));
        }
        if (asan) {
            metaWriterU(w, convert<u64>(m->memory->shadow));
        }
        if (lines) {
            metaWriterU(w, convert<u64>(length(m->files)));
            for (Int i = 0; i < length(m->files); ++i) {
                metaWriterStr(w, m->files[i]);
            }
            metaWriterU(w, convert<u64>(length(m->lines)));
            u32 pc = 0;
            for (Int i = 0; i < length(m->lines); ++i) {
                const LineEntry e = m->lines[i];
                metaWriterU(w, e.pc - pc);
                metaWriterU(w, e.file);
                metaWriterU(w, e.line);
                pc = e.pc;
            }
        }
        return w->out;
    }

    static String exportSymbol(String name, FuncType* t) {
        return symExport + symbolPart(name) + String(".") + typesPart(t->params) + String(".") + typesPart(t->results);
    }

    static u8 fAluPP(FAluOp op, u8 size) {
        if (op >= fAnd) {
            return packedPP(size);
        }
        return scalarPP(size);
    }

    static MemoryLoop* findMemoryLoop(Function* f, CfgInfo* ci, i32 h) {
        MemoryLoop* l = activePool->make<MemoryLoop>(MemoryLoop{f, ci, h, -1, -1, -1, {}, makeSlice<bool>(length(f->blocks)), {}, {}, -1, u64(1LL), {}, {}, {}, -1, -1, {}, 0, -1, false, false, mapOf<i32, LoopSpan>({}), mapOf<i32, LoopSpan>({}), {}, {}, {}, {}, {}});
        auto preds = f->blocks[h].preds;
        for (Int i = 0; i < length(preds); ++i) {
            i32 p = preds[i];
            if (loopDominates(ci, h, p)) {
                if (l->latch >= 0) {
                    return {};
                }
                l->latch = p;
            } else {
                if (l->preheader >= 0) {
                    return {};
                }
                l->preheader = p;
            }
        }
        if (l->latch < 0 || l->preheader < 0) {
            return {};
        }
        auto edges = f->blocks[l->preheader].term.edges;
        for (Int i = 0; i < length(edges); ++i) {
            Edge e = edges[i];
            if (e.block == h) {
                l->entry = e;
            }
        }
        l->members[h] = true;
        Slice<i32> work = sliceOf<i32>({l->latch});
        while (length(work) > Int(0LL)) {
            i32 b = work[length(work) - 1];
            work = subslice(work, 0, length(work) - 1);
            if (l->members[b]) {
                continue;
            }
            // An added block belongs to a versioned loop, whose header the
            // walk would reach as well.
            if (b >= length(ci->header) || ci->header[b] || !loopDominates(ci, h, b)) {
                return {};
            }
            l->members[b] = true;
            work = appendSlice(work, f->blocks[b].preds);
        }
        Int insts = 0;
        bool multipleExits = false;
        auto order = ci->order;
        for (Int i = 0; i < length(order); ++i) {
            i32 b = order[i];
            if (!l->members[b]) {
                continue;
            }
            l->order = appendValues(l->order, b);
            insts += length(f->blocks[b].instrs);
            auto range1495 = f->blocks[b].term.edges;
            for (Int index1496 = 0, rangeEnd1497 = length(range1495); index1496 < rangeEnd1497; ++index1496) {
                Edge e = range1495[index1496];
                if (!l->members[e.block]) {
                    if (l->exit >= 0 && l->exit != e.block) {
                        multipleExits = true;
                    } else {
                        l->exit = e.block;
                    }
                }
            }
        }
        if (l->exit < 0 || length(l->order) > 48 || insts > 400) {
            return {};
        }
        bool outsidePred = false;
        auto range1498 = f->blocks[l->exit].preds;
        for (Int index1499 = 0, rangeEnd1500 = length(range1498); index1499 < rangeEnd1500; ++index1499) {
            i32 p = range1498[index1499];
            outsidePred = outsidePred || !l->members[p];
        }
        Term* t = &f->blocks[l->latch].term;
        if (t->kind != TermIf) {
            return {};
        }
        Value* condition = &f->values[t->value];
        Span<i32> args = functionArgs(f, t->value);
        i32 next = {};
        i64 unsignedStop = -1;
        bool zeroTest = false;
        if (condition->op == OpEqz && t->edges[1].block == h) {
            next = args[0];
            zeroTest = true;
        } else if (condition->op == OpNe && t->edges[0].block == h && f->values[args[1]].op == OpConst && f->values[args[1]].aux == 0) {
            next = args[0];
            zeroTest = true;
        } else if (condition->op == OpGtU && t->edges[0].block == h && f->values[args[1]].op == OpConst && convert<u32>(f->values[args[1]].aux) < 65536) {
            next = args[0];
            unsignedStop = convert<i64>(convert<u32>(f->values[args[1]].aux));
        } else if (condition->op == OpGtS && t->edges[0].block == h && f->values[args[1]].op == OpConst && f->values[args[1]].aux == 0) {
            next = args[0];
            l->countSigned = true;
        } else if (t->edges[0].block == h && (condition->op == OpAdd || condition->op == OpSub)) {
            next = t->value;
            zeroTest = true;
        } else if (t->edges[0].block == h && (condition->op == OpLtS || condition->op == OpLtU)) {
            next = args[0];
            l->limit = args[1];
            l->signedLimit = condition->op == OpLtS;
        } else if (t->edges[0].block == h && (condition->op == OpLeS || condition->op == OpLeU)) {
            next = args[0];
            l->limit = args[1];
            l->signedLimit = condition->op == OpLeS;
            l->inclusive = true;
        } else if (t->edges[0].block == h && condition->op == OpNe) {
            next = args[0];
            l->limit = args[1];
            l->exact = true;
        } else {
            return {};
        }
        Edge back = {};
        auto range1501 = t->edges;
        for (Int index1502 = 0, rangeEnd1503 = length(range1501); index1502 < rangeEnd1503; ++index1502) {
            Edge e = range1501[index1502];
            if (e.block == h) {
                back = e;
            }
        }
        l->count = -1;
        auto params = f->blocks[h].params;
        for (Int j = 0; j < length(params); ++j) {
            i32 p = params[j];
            i32 candidate = next;
            i32 bias = -1;
            if (unsignedStop >= 0 && back.args[j] != next && f->values[next].op == OpAdd) {
                Span<i32> a = functionArgs(f, next);
                Int k = 0;
                for (; k < 2; k++) {
                    i32 other = f->values[a[1 - k]].block;
                    if (a[k] == back.args[j] && (other < 0 || (!l->members[other] && loopDominates(ci, other, h)))) {
                        auto temporary1509 = a[k];
                        auto temporary1510 = a[1 - k];
                        candidate = temporary1509;
                        bias = temporary1510;
                    }
                }
            }
            if (back.args[j] != candidate) {
                continue;
            }
            i64 step = inductionStep(f, p, candidate);
            if (l->limit >= 0 && step >= 1 && step <= 65536) {
                l->start = l->entry.args[j];
                l->startStep = step;
                l->startStride = -1;
            } else if (l->limit >= 0 && step == 0 && l->start < 0) {
                // The variable advances by one invariant stride, added once
                // or, unrolled, several times per iteration.
                auto [terms, ok] = inductionTerms(f, candidate, p, 0);
                i32 stride = ok && length(terms) > 0 && length(terms) <= 65536 ? terms[0] : -1;
                for (Int k = 1; stride >= 0 && k < length(terms); ++k) {
                    if (terms[k] != stride) {
                        stride = -1;
                    }
                }
                if (stride >= 0) {
                    const Value* sv = &f->values[stride];
                    if (sv->op != OpConst && (sv->block < 0 || (!l->members[sv->block] && loopDominates(ci, sv->block, h)))) {
                        l->start = l->entry.args[j];
                        l->startStep = length(terms);
                        l->startStride = stride;
                    }
                }
            } else if (l->limit < 0 && step < 0 && step >= -65536) {
                u64 n = convert<u64>(-step);
                u64 stop = 0;
                u64 mask = 0;
                if (unsignedStop >= 0 && convert<u64>(unsignedStop) + 1 >= n) {
                    stop = convert<u64>(unsignedStop);
                } else if (zeroTest && (n & (n - 1)) == 0) {
                    auto temporary1513 = n - 1;
                    auto temporary1514 = n - 1;
                    stop = temporary1513;
                    mask = temporary1514;
                } else if (step != -1 || unsignedStop >= 0) {
                    continue;
                }
                l->count = l->entry.args[j];
                auto temporary1515 = bias;
                auto temporary1516 = n;
                auto temporary1517 = stop;
                auto temporary1518 = mask;
                l->countBias = temporary1515;
                l->countStep = temporary1516;
                l->countStop = temporary1517;
                l->countMask = temporary1518;
                if (step == -1 && bias < 0) {
                    setMap(l->induction, p, LoopSpan{-1, {}, u64(1LL), u64(1LL), {}, {}, {}, {}, true});
                }
            }
        }
        if (l->count < 0 && l->start < 0) {
            return {};
        }
        auto range1519 = f->blocks[h].params;
        for (Int index1520 = 0, rangeEnd1521 = length(range1519); index1520 < rangeEnd1521; ++index1520) {
            Int j = index1520;
            i32 p = range1519[index1520];
            i64 step = inductionStep(f, p, back.args[j]);
            if (step > 0 && step <= 65536) {
                setMap(l->induction, p, LoopSpan{l->entry.args[j], u64(1LL), convert<u64>(step), {}, {}, {}, {}, {}, true});
            }
        }
        // Only countable loops need the whole-function live-out scan.
        if ((multipleExits || outsidePred) && memoryLoopHasLiveOut(l)) {
            return {};
        }
        l->valueRanges = activePool->make<ValueRanges>(ValueRanges{f, makeSlice<ValueRange>(length(f->values)), makeSlice<bool>(length(f->values))});
        return l;
    }

    // firstBranch returns the index of the branch sequence ending a block.
    static Int firstBranch(Span<MInst> insts) {
        Int i = length(insts);
        while (i > Int(0LL)) {
            switch (insts[i - Int(1LL)].op) {
                case mJmp:
                case mJcc:
                case mSwitch:
                case mRet:
                case mTrap: {
                    i--;
                    continue;
                }
            }
            break;
        }
        return i;
    }

    static bool flagCondition(Op op) {
        return isCompare(op) || op == OpAnd || op == OpOr || op == OpXor || op == OpSub;
    }

    static Tuple2<double, bool> floatConstant(Function* f, i32 v) {
        Value* val = &f->values[v];
        if (val->op != OpConst || !isFloat(val->typ)) {
            return {double(0.0), false};
        }
        if (val->typ == F32) {
            return {convert<double>(mathFloat32frombits(convert<u32>(val->aux))), true};
        }
        return {mathFloat64frombits(convert<u64>(val->aux)), true};
    }

    // foldInt evaluates a pure integer operation on constants.
    static Tuple2<u64, bool> foldInt(Op op, ValType t, u64 a, u64 b) {
        Uint w = 64;
        if (t == I32) {
            w = 32;
        }
        u64 m = mask(t);
        a &= m;
        b &= m;
        i64 sa = shiftRight(convert<i64>(shiftLeft(a, 64 - w)), 64 - w);
        i64 sb = shiftRight(convert<i64>(shiftLeft(b, 64 - w)), 64 - w);
        u64 sh = b & convert<u64>(w - 1);
        auto boolean = [&](bool c) mutable -> Tuple2<u64, bool> {
            if (c) {
                return {u64(1LL), true};
            }
            return {u64(0LL), true};
        };
        switch (op) {
            case OpAdd: {
                return {(a + b) & m, true};
            }
            case OpSub: {
                return {(a - b) & m, true};
            }
            case OpMul: {
                return {(a * b) & m, true};
            }
            case OpAnd: {
                return {a & b, true};
            }
            case OpOr: {
                return {a | b, true};
            }
            case OpXor: {
                return {a ^ b, true};
            }
            case OpShl: {
                return {shiftLeft(a, sh) & m, true};
            }
            case OpShrU: {
                return {shiftRight(a, sh), true};
            }
            case OpShrS: {
                return {convert<u64>(shiftRight(sa, sh)) & m, true};
            }
            case OpRotl: {
                if (w == 32) {
                    return {convert<u64>(bitsRotateLeft32(convert<u32>(a), convert<i64>(sh))), true};
                }
                return {bitsRotateLeft64(a, convert<i64>(sh)), true};
            }
            case OpRotr: {
                if (w == 32) {
                    return {convert<u64>(bitsRotateLeft32(convert<u32>(a), -convert<i64>(sh))), true};
                }
                return {bitsRotateLeft64(a, -convert<i64>(sh)), true};
            }
            case OpDivU: {
                if (b == 0) {
                    return {u64(0LL), false};
                }
                return {a / b, true};
            }
            case OpRemU: {
                if (b == 0) {
                    return {u64(0LL), false};
                }
                return {a % b, true};
            }
            case OpDivS: {
                if (b == 0 || (sb == -1 && a == shiftLeft(u64(1LL), w - 1))) {
                    return {u64(0LL), false};
                }
                return {convert<u64>(sa / sb) & m, true};
            }
            case OpRemS: {
                if (b == 0) {
                    return {u64(0LL), false};
                }
                if (sb == -1) {
                    return {u64(0LL), true};
                }
                return {convert<u64>(sa % sb) & m, true};
            }
            case OpEq: {
                return boolean(a == b);
            }
            case OpNe: {
                return boolean(a != b);
            }
            case OpLtS: {
                return boolean(sa < sb);
            }
            case OpLtU: {
                return boolean(a < b);
            }
            case OpGtS: {
                return boolean(sa > sb);
            }
            case OpGtU: {
                return boolean(a > b);
            }
            case OpLeS: {
                return boolean(sa <= sb);
            }
            case OpLeU: {
                return boolean(a <= b);
            }
            case OpGeS: {
                return boolean(sa >= sb);
            }
            case OpGeU: {
                return boolean(a >= b);
            }
        }
        return {u64(0LL), false};
    }

    static Tuple2<u64, bool> foldUnary(Op op, ValType t, ValType from, u64 a) {
        u64 m = mask(t);
        switch (op) {
            case OpEqz: {
                if ((a & mask(from)) == 0) {
                    return {u64(1LL), true};
                }
                return {u64(0LL), true};
            }
            case OpClz: {
                if (t == I32) {
                    return {convert<u64>(bitsLeadingZeros32(convert<u32>(a))), true};
                }
                return {convert<u64>(bitsLeadingZeros64(a)), true};
            }
            case OpCtz: {
                if (t == I32) {
                    return {convert<u64>(bitsTrailingZeros32(convert<u32>(a))), true};
                }
                return {convert<u64>(bitsTrailingZeros64(a)), true};
            }
            case OpPopcnt: {
                return {convert<u64>(bitsOnesCount64(a & m)), true};
            }
            case OpExtend8S: {
                return {convert<u64>(convert<i64>(convert<i8>(a))) & m, true};
            }
            case OpExtend16S: {
                return {convert<u64>(convert<i64>(convert<i16>(a))) & m, true};
            }
            case OpExtend32S: {
                return {convert<u64>(convert<i64>(convert<i32>(a))) & m, true};
            }
            case OpWrap: {
                return {a & u64(4294967295LL), true};
            }
            case OpExtendU: {
                return {a & u64(4294967295LL), true};
            }
            case OpExtendS: {
                return {convert<u64>(convert<i64>(convert<i32>(a))), true};
            }
        }
        return {u64(0LL), false};
    }

    static bool globalMutability(Reader* r) {
        u8 b = readerByte(r);
        if (b > 1) {
            compilerError(String("malformed mutability 0x%02x"));
        }
        return b == 1;
    }

    static GoFrame goAssign(Slice<ValType> params, Slice<ValType> results) {
        GoFrame f = {};
        i32 off = 0;
        auto assign = [&](Slice<ValType> types, Int ints) mutable -> Slice<GoLoc> {
            Int floats = 0;
            Slice<GoLoc> locs = makeSlice<GoLoc>(length(types));
            auto range1525 = types;
            for (Int index1526 = 0, rangeEnd1527 = length(range1525); index1526 < rangeEnd1527; ++index1526) {
                Int i = index1526;
                ValType t = range1525[index1526];
                if (isFloat(t) && floats < length(goFloatRegs)) {
                    locs[i] = GoLoc{goFloatRegs[floats], {}};
                    floats++;
                } else if (!isFloat(t) && ints < length(goIntRegs)) {
                    locs[i] = GoLoc{goIntRegs[ints], {}};
                    ints++;
                } else {
                    off = alignTo(off, goSize(t));
                    locs[i] = GoLoc{noReg, off};
                    off += goSize(t);
                }
            }
            off = alignTo(off, 8);
            return locs;
        };
        f.params = assign(params, 1);
        f.results = assign(results, 0);
        off += 8;
        auto range1530 = f.params;
        for (Int index1531 = 0, rangeEnd1532 = length(range1530); index1531 < rangeEnd1532; ++index1531) {
            Int i = index1531;
            GoLoc l = range1530[index1531];
            if (l.reg != noReg) {
                off = alignTo(off, goSize(params[i])) + goSize(params[i]);
            }
        }
        f.size = alignTo(off, 8);
        return f;
    }

    static i32 goSize(ValType t) {
        if (t == I32 || t == F32) {
            return 4;
        }
        return 8;
    }

    // hasEffect reports whether a value must stay even when nothing uses it:
    // it writes state, or it may trap.
    static bool hasEffect(Op op) {
        switch (op) {
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
            }
        }
        return false;
    }

    template <typename Data>
    static void heapSort_func(Data data, Int a, Int b) {
        Int first = a;
        Int lo = 0;
        Int hi = b - a;
        {
            Int i = (hi - 1) / 2;
            for (; i >= 0; i--) {
                siftDown_func(data, i, hi, first);
            }
        }
        Int i = hi - 1;
        for (; i >= 0; i--) {
            data.Swap(first, first + i);
            siftDown_func(data, lo, i, first);
        }
    }

    static u8 hi(Int r) {
        return u8(convert<u8>(shiftRight(r, 3)) & 1);
    }

    static String importSymbol(String module, String name, FuncType* t) {
        return symImport + symbolPart(module) + String(".") + symbolPart(name) + String(".") + typesPart(t->params) + String(".") + typesPart(t->results);
    }

    // indexed is [base+index].
    static Amem indexed(VReg base, VReg index) {
        return Amem{base, index, u8(1LL), {}, -1, {}};
    }

    static i64 inductionStep(Function* f, i32 phi, i32 next) {
        Value* val = &f->values[next];
        Span<i32> args = functionArgs(f, next);
        if (val->typ != I32 || (val->op != OpAdd && val->op != OpSub)) {
            return i64(0LL);
        }
        i32 a = args[0];
        i32 b = args[1];
        if (val->op == OpAdd && b == phi) {
            auto temporary1536 = b;
            auto temporary1537 = a;
            a = temporary1536;
            b = temporary1537;
        }
        if (a != phi || f->values[b].op != OpConst) {
            return i64(0LL);
        }
        i64 step = convert<i64>(convert<i32>(f->values[b].aux));
        if (val->op == OpSub) {
            step = -step;
        }
        return step;
    }

    // Unrolled loops may advance a pointer by adding the same stride several
    // times. Keep those increments as a factored stride, without changing the
    // wrapping i32 arithmetic of the original loop.
    static Tuple2<Slice<i32>, bool> inductionTerms(Function* f, i32 v, i32 phi, Int depth) {
        if (v == phi) {
            return {{}, true};
        }
        if (depth > 16 || f->values[v].op != OpAdd || f->values[v].typ != I32) {
            return {{}, false};
        }
        Span<i32> args = functionArgs(f, v);
        Int j = 0;
        for (; j < 2; j++) {
            auto temporary1538 = inductionTerms(f, args[j], phi, depth + 1);
            Slice<i32> terms = temporary1538.v0;
            bool ok = temporary1538.v1;
            if (ok) {
                return {appendValues(terms, args[1 - j]), true};
            }
        }
        return {{}, false};
    }

    // insertionSort_func sorts data[a:b] using insertion sort.
    template <typename Data>
    static void insertionSort_func(Data data, Int a, Int b) {
        Int i = a + 1;
        for (; i < b; i++) {
            Int j = i;
            for (; j > a && data.Less(j, j - 1); j--) {
                data.Swap(j, j - 1);
            }
        }
    }

    static MInst inst(MOp op) {
        return MInst{op, {}, {}, {}, {}, noReg, noReg, noReg, {}, Mem{noReg, noReg, {}, {}, -1}, -1, nullptr};
    }

    // internalLocs gives the internal convention's places of values: a
    // register, or noReg and the index of a stack slot, counted from first.
    static Tuple3<Slice<VReg>, Slice<Int>, Int> internalLocs(Slice<ValType> types, Slice<VReg> gprs, Slice<VReg> xmms, Int first) {
        Int gi = 0;
        Int xi = 0;
        Int slot = first;
        Slice<VReg> regs = makeSlice<VReg>(length(types));
        Slice<Int> slots = makeSlice<Int>(length(types));
        for (Int i = 0; i < length(types); ++i) {
            ValType t = types[i];
            regs[i] = noReg;
            if (isFloat(t) && xi < length(xmms)) {
                regs[i] = xmms[xi];
                xi++;
            } else if (!isFloat(t) && gi < length(gprs)) {
                regs[i] = gprs[gi];
                gi++;
            } else {
                slots[i] = slot;
                slot++;
            }
        }
        return {regs, slots, slot};
    }

    static bool isCompare(Op op) {
        return (op >= OpEqz && op <= OpGeU) || (op >= OpFEq && op <= OpFGe);
    }

    static bool isFloat(ValType t) {
        return t == F32 || t == F64;
    }

    static bool isIntOp(Op op) {
        return op >= OpAdd && op <= OpGeU;
    }

    static bool isSlot(VReg r) {
        return r < noReg;
    }

    static u8 lo3(Int r) {
        return convert<u8>(r & 7);
    }

    static MFunc* lowerFunction(ObjCode* oc, Function* f) {
        Lowerer* l = activePool->make<Lowerer>(Lowerer{oc->m, f, oc->lay, {}, activePool->make<MFunc>(MFunc{}), {}, {}, {}, {}, {}, {}, {}, -1, {}, {}, mapOf<Array<i32, 2>, i32>({}), mapOf<i32, i64>({}), {}, {}, {}, {}});
        l->oc = oc;
        l->ci = f->cfg ? f->cfg : functionAnalyze(f);
        l->vregs.resize(f->values.size());
        l->vregs.fill(noReg);
        lowererCountUses(l);
        lowererMarkDeferred(l);
        lowererPlaceFloatConstants(l);
        lowererFindRawTruncations(l);
        Slice<Slice<CheckedValue>> proofs = checkedValues(f, l->ci);
        l->mblk.resize(f->blocks.size());
        l->mblk.fill(-1);
        auto order = l->ci->order;
        for (Int i = 0; i < length(order); ++i) {
            i32 b = order[i];
            l->mblk[b] = convert<i32>(length(l->mf->blocks));
            l->mf->blocks.pushBack(MBlock{{}, {}, {}, l->ci->depth[b], b, {}});
        }
        auto range1556 = l->ci->order;
        for (Int index1557 = 0, rangeEnd1558 = length(range1556); index1557 < rangeEnd1558; ++index1557) {
            i32 b = range1556[index1557];
            auto params = f->blocks[b].params;
            for (Int i = 0; i < length(params); ++i) {
                i32 p = params[i];
                lowererDef(l, p);
            }
        }
        auto children = cfgInfoChildren(l->ci);
        Vec<LowerVisit> visits;
        visits.pushBack({l->ci->order[0], false, 0});
        while (length(visits) > Int(0LL)) {
            const LowerVisit visit = visits.popBack();
            if (visit.leave) {
                while (length(l->checkUndo) > visit.undo) {
                    CheckUndo u = l->checkUndo[length(l->checkUndo) - 1];
                    l->checkUndo = subslice(l->checkUndo, 0, length(l->checkUndo) - 1);
                    if (u.set) {
                        setMap(l->checked, u.value, u.end);
                    } else {
                        eraseMap(l->checked, u.value);
                    }
                }
                continue;
            }
            i32 b = visit.block;
            Int undo = length(l->checkUndo);
            l->cur = l->mblk[b];
            if (!isNil(proofs)) {
                auto range1562 = proofs[b];
                for (Int index1563 = 0, rangeEnd1564 = length(range1562); index1563 < rangeEnd1564; ++index1563) {
                    CheckedValue proof = range1562[index1563];
                    auto temporary1565 = lookupMap(l->checked, proof.value);
                    i64 prior = temporary1565.v0;
                    bool had = temporary1565.v1;
                    if (!had || prior < proof.end) {
                        l->checkUndo = appendValues(l->checkUndo, CheckUndo{proof.value, prior, had});
                        setMap(l->checked, proof.value, proof.end);
                    }
                }
            }
            if (b == l->ci->order[0]) {
                lowererEntry(l);
            }
            auto range1566 = l->floatHomes[b];
            for (Int index1567 = 0, rangeEnd1568 = length(range1566); index1567 < rangeEnd1568; ++index1567) {
                i32 v = range1566[index1567];
                Value* val = &f->values[v];
                MInst in = inst(mFConst);
                in.dst = lowererDef(l, v);
                in.imm = val->aux;
                in.size = typeSize(val->typ);
                lowererEmit(l, in);
            }
            auto instrs = f->blocks[b].instrs.view();
            l->block = b;
            for (Int i = 0; i < length(instrs); ++i) {
                const Value* val = &f->values[instrs[i]];
                if ((val->op == OpLoad || val->op == OpStore) && MemKind(convert<MemKind>(val->aux2) & memProven) != MemKind(0LL)) {
                    l->bounded[usize(functionArgs(f, instrs[i])[0])] = b;
                }
            }
            for (Int i = 0; i < length(instrs); ++i) {
                i32 v = instrs[i];
                if (!l->deferred[v]) {
                    l->pos = f->positions[usize(v)];
                    lowererLowerValue(l, v);
                }
            }
            l->pos = f->blocks[b].term.pos;
            lowererTerminator(l, b);
            visits.pushBack({b, true, undo});
            for (i32 child = children.first[usize(b)]; child >= 0; child = children.next[usize(child)]) {
                visits.pushBack({child, false, 0});
            }
        }
        lowererLinkBlocks(l);
        if (l->m->tailRanges) {
            lowererVersionTailRanges(l);
        }
        return l->mf;
    }

    static MemKind makeMemKind(Int width, bool signedValue) {
        MemKind k = convert<MemKind>(width);
        if (signedValue) {
            k |= memSigned;
        }
        return k;
    }

    static u64 mask(ValType t) {
        if (t == I32) {
            return u64(4294967295LL);
        }
        return u64(18446744073709551615ULL);
    }

    static i64 maxSigned(u8 size) {
        if (size == 8) {
            return i64(9223372036854775807LL);
        }
        return i64(2147483647LL);
    }

    // medianAdjacent_func finds the median of data[a - 1], data[a], data[a + 1] and stores the index into a.
    template <typename Data>
    static Int medianAdjacent_func(Data data, Int a, Int* swaps) {
        return median_func(data, a - 1, a, a + 1, swaps);
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
        return FloatRange{maximum(a.lo, b.lo), minimum(a.hi, b.hi), a.nan && b.nan};
    }

    static Mem memAt(VReg base, i32 disp) {
        return Mem{base, noReg, u8(1LL), disp, -1};
    }

    static Int memB(Amem m) {
        if (m.label >= 0) {
            return -1;
        }
        return m.base;
    }

    static Mem memIndexed(VReg base, VReg index, i32 disp) {
        return Mem{base, index, u8(1LL), disp, -1};
    }

    static Amem memOf(Mem m) {
        Amem am = Amem{-1, -1, m.scale, m.disp, -1, {}};
        if (m.label >= 0) {
            am.label = m.label;
            am.labelKind = labelModule;
            return am;
        }
        if (m.base != noReg) {
            am.base = regNum(m.base);
        }
        if (m.index != noReg) {
            am.index = regNum(m.index);
        }
        if (am.scale == 0) {
            am.scale = 1;
        }
        return am;
    }

    static Int memX(Amem m) {
        if (m.label >= 0) {
            return -1;
        }
        return m.index;
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
        i32 off = headerSize;
        if (m->memoryTail != 0) {
            l->memoryEnd = off;
            off += 8;
        }
        if (m->asan) {
            l->shadow = off;
            off += 8;
        }
        auto globals = m->globals;
        for (Int i = 0; i < length(globals); ++i) {
            Global g = globals[i];
            if (g.typ == V128) {
                off = (off + 15) & ~15;
            }
            l->globals = appendValues(l->globals, off);
            off += 8;
            if (g.typ == V128 && !g.imported) {
                off += 8;
            }
        }
        off = (off + 15) & ~15;
        for (Int i = 0, end = length(m->tables); i < end; ++i) {
            l->tables = appendValues(l->tables, off);
            off += tableBytes;
        }
        l->descs = off;
        off += convert<i32>(length(m->funcs)) * descBytes;
        l->dropped = off;
        off += convert<i32>(length(m->datas) + length(m->elems));
        if (m->tailProfile) {
            off = (off + 7) & ~7;
            l->tailProfile = off;
            off += convert<i32>(length(m->funcs)) * tailCounterBytes;
        }
        l->size = (off + 15) & ~15;
        return l;
    }

    static Uint nextPowerOfTwo(Int lengthValue) {
        Uint shift = convert<Uint>(bitsLen(convert<Uint>(lengthValue)));
        return convert<Uint>(shiftLeft(Uint(1LL), shift));
    }

    static void optimize(Function* f) {
        Optimizer* o = activePool->make<Optimizer>(Optimizer{f, {}, mapOf<ConstKey, i32>({}), {}});
        for (Int v = 0, end = length(f->values); v < end; ++v) {
            Value* val = &f->values[v];
            if (val->op == OpConst && val->block < 0) {
                ConstKey key = ConstKey{val->typ, val->aux};
                auto temporary1586 = lookupMap(o->consts, key);
                bool ok = temporary1586.v1;
                if (!ok) {
                    setMap(o->consts, key, convert<i32>(v));
                }
            }
        }
        functionComputePreds(f);
        {
            Int round = 0;
            for (; round < 8; round++) {
                bool changed = optimizerSimplify(o);
                changed = optimizerThread(o) || changed;
                changed = optimizerCleanup(o) || changed;
                changed = optimizerTrivialParams(o) || changed;
                if (!changed) {
                    break;
                }
            }
        }
        {
            Int unrolled = 0;
            for (; unrolled < 16 && optimizerUnrollLoops(o); unrolled++) {
            }
            for (Int round = 0; unrolled > 0 && round < 8; round++) {
                bool changed = optimizerSimplify(o);
                changed = optimizerThread(o) || changed;
                changed = optimizerCleanup(o) || changed;
                changed = optimizerTrivialParams(o) || changed;
                if (!changed) {
                    break;
                }
            }
            // The analysis of a pass that changed nothing serves the
            // common expressions; the rounds may change edges.
            bool moved = optimizerDisjointOrs(o);
            for (Int round = 0; round < 4 && optimizerOuterConstants(o); round++) {
                moved = true;
            }
            for (Int round = 0; moved && round < 4; round++) {
                f->cfg = nullptr;
                bool changed = optimizerSimplify(o);
                changed = optimizerCleanup(o) || changed;
                if (!changed) {
                    break;
                }
            }
            if (unrolled > 0) {
                f->cfg = nullptr;
            }
        }
        optimizerCommonExpressions(o);
        if (optimizerNarrowFloatComparisons(o)) {
            optimizerDeadCode(o);
        }
        optimizerSelectTriangles(o);
        const Int unversioned = length(f->blocks);
        const bool versioned = versionMemoryLoops(f);
        if (versioned) {
            o->counted = false;
        }
        Int round = 0;
        for (; round < 4; round++) {
            // The guards of versioned loops are built of constants and
            // loop-entry values the simplifier folds.
            bool changed = versioned && optimizerSimplify(o, unversioned);
            changed = optimizerCleanup(o) || changed;
            changed = optimizerTrivialParams(o) || changed;
            if (!changed) {
                break;
            }
        }
        optimizerDeadCode(o);
    }

    // order ranks section ids in their required order; datacount goes before
    // code.
    static Int order(u8 id) {
        switch (id) {
            case 12: {
                return 10;
            }
            case 10:
            case 11: {
                return convert<i64>(id) + 1;
            }
        }
        return convert<i64>(id);
    }

    // order2_func returns x,y where data[x] <= data[y], where x,y=a,b or x,y=b,a.
    template <typename Data>
    static Tuple2<Int, Int> order2_func(Data data, Int a, Int b, Int* swaps) {
        if (data.Less(b, a)) {
            (*swaps)++;
            return {b, a};
        }
        return {a, b};
    }

    // overlap tells whether two sorted lists of ranges share a position.
    static bool overlap(Span<Lrange> x, Span<Lrange> y) {
        Int i = 0;
        Int j = 0;
        while (i < length(x) && j < length(y)) {
            if (x[i].to <= y[j].from) {
                i++;
            } else if (y[j].to <= x[i].from) {
                j++;
            } else {
                return true;
            }
        }
        return false;
    }

    static u8 packedPP(u8 size) {
        if (size == 8) {
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
        constexpr Int maxSteps = 5;
        constexpr Int shortestShifting = 50;
        Int i = a + 1;
        Int j = 0;
        for (; j < 5; j++) {
            while (i < b && !data.Less(i, i - Int(1LL))) {
                i++;
            }
            if (i == b) {
                return true;
            }
            if (b - a < 50) {
                return false;
            }
            data.Swap(i, i - 1);
            if (i - a >= 2) {
                Int j = i - 1;
                for (; j >= 1; j--) {
                    if (!data.Less(j, j - 1)) {
                        break;
                    }
                    data.Swap(j, j - 1);
                }
            }
            if (b - i >= 2) {
                Int j = i + 1;
                for (; j < b; j++) {
                    if (!data.Less(j, j - 1)) {
                        break;
                    }
                    data.Swap(j, j - 1);
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
        data.Swap(a, pivot);
        Int i = a + 1;
        Int j = b - 1;
        while (true) {
            while (i <= j && !data.Less(a, i)) {
                i++;
            }
            while (i <= j && data.Less(a, j)) {
                j--;
            }
            if (i > j) {
                break;
            }
            data.Swap(i, j);
            i++;
            j--;
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
        data.Swap(a, pivot);
        Int i = a + 1;
        Int j = b - 1;
        while (i <= j && data.Less(i, a)) {
            i++;
        }
        while (i <= j && !data.Less(j, a)) {
            j--;
        }
        if (i > j) {
            data.Swap(j, a);
            return {j, true};
        }
        data.Swap(i, j);
        i++;
        j--;
        while (true) {
            while (i <= j && data.Less(i, a)) {
                i++;
            }
            while (i <= j && !data.Less(j, a)) {
                j--;
            }
            if (i > j) {
                break;
            }
            data.Swap(i, j);
            i++;
            j--;
        }
        data.Swap(j, a);
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
        constexpr Int maxInsertion = 12;
        bool wasBalanced = true;
        bool wasPartitioned = true;
        while (true) {
            Int lengthValue = b - a;
            if (lengthValue <= 12) {
                insertionSort_func(data, a, b);
                return;
            }
            if (limit == 0) {
                heapSort_func(data, a, b);
                return;
            }
            if (!wasBalanced) {
                breakPatterns_func(data, a, b);
                limit--;
            }
            auto temporary1596 = choosePivot_func(data, a, b);
            Int pivot = temporary1596.v0;
            SortedHint hint = temporary1596.v1;
            if (hint == decreasingHint) {
                reverseRange_func(data, a, b);
                pivot = b - 1 - (pivot - a);
                hint = increasingHint;
            }
            if (wasBalanced && wasPartitioned && hint == increasingHint) {
                if (partialInsertionSort_func(data, a, b)) {
                    return;
                }
            }
            if (a > 0 && !data.Less(a - 1, pivot)) {
                Int mid = partitionEqual_func(data, a, b, pivot);
                a = mid;
                continue;
            }
            auto temporary1597 = partition_func(data, a, b, pivot);
            Int mid = temporary1597.v0;
            bool alreadyPartitioned = temporary1597.v1;
            wasPartitioned = alreadyPartitioned;
            Int leftLen = mid - a;
            Int rightLen = b - mid;
            Int balanceThreshold = lengthValue / 8;
            if (leftLen < rightLen) {
                wasBalanced = leftLen >= balanceThreshold;
                pdqsort_func(data, a, mid, limit);
                a = mid + 1;
            } else {
                wasBalanced = rightLen >= balanceThreshold;
                pdqsort_func(data, mid + 1, b, limit);
                b = mid;
            }
        }
    }

    // readsState reports whether a value depends on mutable state, so two equal
    // instructions may produce different results.
    static bool readsState(Op op) {
        switch (op) {
            case OpLoad:
            case OpMemSize:
            case OpGlobalGet:
            case OpTableGet:
            case OpTableSize:
            case OpExtract:
            case OpParam:
            case OpUndef: {
                return true;
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
            return convert<i64>(r - XMM0);
        }
        return convert<i64>(r);
    }

    // resultRegs assigns results to the result registers in order; one that
    // does not fit gets noReg and the next stack result slot, which follows the
    // stack arguments in the caller's outgoing area.
    static Slice<VReg> resultRegs(Slice<ValType> results) {
        Int gi = 0;
        Int xi = 0;
        Slice<VReg> regs = makeSlice<VReg>(length(results));
        for (Int i = 0; i < length(results); ++i) {
            ValType t = results[i];
            regs[i] = noReg;
            if (isFloat(t) && xi < length(resultXMMs)) {
                regs[i] = resultXMMs[xi];
                xi++;
            } else if (!isFloat(t) && gi < length(resultGPRs)) {
                regs[i] = resultGPRs[gi];
                gi++;
            }
        }
        return regs;
    }

    // intrinsicOf tells the imports lib/c's memory and string functions call,
    // __kernel_memcpy and the like (string.h), by module, name and type.
    static Int intrinsicOf(Import imp, FuncType* t) {
        if (imp.module != String("kernel") || length(t->results) != 1 || t->results[0] != I32) {
            return intrinsicNone;
        }
        for (Int i = 0; i < length(t->params); ++i) {
            if (t->params[i] != I32) {
                return intrinsicNone;
            }
        }
        struct Known {
            String name;
            Int params;
            Int intrinsic;
        };
        const Known known[] = {{String("__kernel_memcpy"), 3, intrinsicMemcpy}, {String("__kernel_memmove"), 3, intrinsicMemmove},
                               {String("__kernel_memset"), 3, intrinsicMemset}, {String("__kernel_memcmp"), 3, intrinsicMemcmp},
                               {String("__kernel_memchr"), 3, intrinsicMemchr}, {String("__kernel_strlen"), 1, intrinsicStrlen},
                               {String("__kernel_strcpy"), 2, intrinsicStrcpy}, {String("__kernel_strcat"), 2, intrinsicStrcat},
                               {String("__kernel_strcmp"), 2, intrinsicStrcmp}, {String("__kernel_strspn"), 2, intrinsicStrspn},
                               {String("__kernel_strcspn"), 2, intrinsicStrcspn}};
        for (const Known& k : known) {
            if (imp.name == k.name && length(t->params) == k.params) {
                return k.intrinsic;
            }
        }
        return intrinsicNone;
    }

    // returnsTwice tells the imports that return twice by their names, as C
    // compilers do.
    static bool returnsTwice(String name) {
        auto switch1608 = name;
        if (switch1608 == String("setjmp") || switch1608 == String("_setjmp") || switch1608 == String("sigsetjmp") || switch1608 == String("vfork") || switch1608 == String("fork") || switch1608 == String("forkpty")) {
            return true;
        }
        return false;
    }

    template <typename Data>
    static void reverseRange_func(Data data, Int a, Int b) {
        Int i = a;
        Int j = b - 1;
        while (i < j) {
            data.Swap(i, j);
            i++;
            j--;
        }
    }

    // rotate_func rotates two consecutive blocks u = data[a:m] and v = data[m:b] in data:
    // Data of the form 'x u v y' is changed to 'x v u y'.
    // rotate performs at most b-a many calls to data.Swap,
    // and it assumes non-degenerate arguments: a < m && m < b.
    template <typename Data>
    static void rotate_func(Data data, Int a, Int m, Int b) {
        Int i = m - a;
        Int j = b - m;
        while (i != j) {
            if (i > j) {
                swapRange_func(data, m - i, m, j);
                i -= j;
            } else {
                swapRange_func(data, m - i, m + j - i, i);
                j -= i;
            }
        }
        swapRange_func(data, m - i, m, i);
    }

    static Amem rspAt(i32 disp) {
        return Amem{4, -1, u8(1LL), disp, -1, {}};
    }

    static bool sameArgs(Optimizer* o, Slice<i32> x, Slice<i32> y) {
        if (length(x) != length(y)) {
            return false;
        }
        for (Int i = 0; i < length(x); ++i) {
            if (optimizerResolve(o, x[i]) != optimizerResolve(o, y[i])) {
                return false;
            }
        }
        return true;
    }

    static u8 scalarPP(u8 size) {
        if (size == 8) {
            return ppF2;
        }
        return ppF3;
    }

    static u8 scaleBits(u8 s) {
        switch (s) {
            case 2: {
                return u8(1LL);
            }
            case 4: {
                return u8(2LL);
            }
            case 8: {
                return u8(3LL);
            }
        }
        return u8(0LL);
    }

    // selfUses counts the uses of parameter p of block b that are its own
    // argument on the edges in, into b.
    static i32 selfUses(Span<Edge*> in, Int k, i32 p) {
        i32 n = 0;
        for (Int i = 0; i < length(in); ++i) {
            Edge* e = in[i];
            if (k < length(e->args) && e->args[k] == p) {
                n++;
            }
        }
        return n;
    }

    // siftDown_func implements the heap property on data[lo:hi].
    // first is an offset into the array where the root of the heap lies.
    template <typename Data>
    static void siftDown_func(Data data, Int lo, Int hi, Int first) {
        Int root = lo;
        while (true) {
            Int child = 2 * root + 1;
            if (child >= hi) {
                break;
            }
            if (child + 1 < hi && data.Less(first + child, first + child + 1)) {
                child++;
            }
            if (!data.Less(first + root, first + child)) {
                return;
            }
            data.Swap(first + root, first + child);
            root = child;
        }
    }

    static VReg slotLoc(i32 slot) {
        return convert<VReg>(-2 - slot);
    }

    static i32 slotOf(VReg r) {
        return convert<i32>(VReg(-2LL) - r);
    }

    template <typename Data>
    static void stable_func(Data data, Int n) {
        Int blockSize = 20;
        Int a = 0;
        Int b = blockSize;
        while (b <= n) {
            insertionSort_func(data, a, b);
            a = b;
            b += blockSize;
        }
        insertionSort_func(data, a, n);
        while (blockSize < n) {
            auto temporary1619 = 0;
            auto temporary1620 = 2 * blockSize;
            a = temporary1619;
            b = temporary1620;
            while (b <= n) {
                symMerge_func(data, a, a + blockSize, b);
                a = b;
                b += 2 * blockSize;
            }
            Int m = a + blockSize;
            if (m < n) {
                symMerge_func(data, a, m, n);
            }
            blockSize *= 2;
        }
    }

    static u8 swapCC(u8 cc) {
        switch (cc) {
            case ccL: {
                return ccG;
            }
            case ccG: {
                return ccL;
            }
            case ccLE: {
                return ccGE;
            }
            case ccGE: {
                return ccLE;
            }
            case ccB: {
                return ccA;
            }
            case ccA: {
                return ccB;
            }
            case ccBE: {
                return ccAE;
            }
            case ccAE: {
                return ccBE;
            }
        }
        return cc;
    }

    template <typename Data>
    static void swapRange_func(Data data, Int a, Int b, Int n) {
        Int i = 0;
        for (; i < n; i++) {
            data.Swap(a + i, b + i);
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
        if (m - a == 1) {
            Int i = m;
            Int j = b;
            while (i < j) {
                Int h = convert<i64>(shiftRight(convert<Uint>(i + j), 1));
                if (data.Less(h, a)) {
                    i = h + 1;
                } else {
                    j = h;
                }
            }
            Int k = a;
            for (; k < i - 1; k++) {
                data.Swap(k, k + 1);
            }
            return;
        }
        if (b - m == 1) {
            Int i = a;
            Int j = m;
            while (i < j) {
                Int h = convert<i64>(shiftRight(convert<Uint>(i + j), 1));
                if (!data.Less(m, h)) {
                    i = h + 1;
                } else {
                    j = h;
                }
            }
            Int k = m;
            for (; k > i; k--) {
                data.Swap(k, k - 1);
            }
            return;
        }
        Int mid = convert<i64>(shiftRight(convert<Uint>(a + b), 1));
        Int n = mid + m;
        Int start = {};
        Int r = {};
        if (m > mid) {
            start = n - b;
            r = mid;
        } else {
            start = a;
            r = m;
        }
        Int p = n - 1;
        while (start < r) {
            Int c = convert<i64>(shiftRight(convert<Uint>(start + r), 1));
            if (!data.Less(p - c, c)) {
                start = c + 1;
            } else {
                r = c;
            }
        }
        Int end = n - start;
        if (start < m && m < end) {
            rotate_func(data, start, m, end);
        }
        if (a < start && start < mid) {
            symMerge_func(data, a, start, mid);
        }
        if (mid < end && end < b) {
            symMerge_func(data, mid, end, b);
        }
    }

    // symbolPart spells a wasm name in a symbol: letters and digits as they
    // are, every other byte, underscores too, as _XX. The spelling is one to
    // one and leaves "." to join parts.
    static String symbolPart(String s) {
        Slice<u8> b = makeSlice<u8>(0, length(s));
        Int i = 0;
        for (; i < length(s); i++) {
            u8 c = s[i];
            if ((c >= 97 && c <= 122) || (c >= 65 && c <= 90) || (c >= 48 && c <= 57)) {
                b = appendValues(b, c);
            } else {
                b = appendSlice(b, fmtSprintf(String("_%02X"), c));
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
        if (fsize == 8) {
            if (isize == 4) {
                return {mathFloat64bits(double(-2147483649.0)), mathFloat64bits(double(2147483648.0)), true};
            }
            return {mathFloat64bits(double(-9.223372036854776e+18)), mathFloat64bits(double(9.223372036854776e+18)), false};
        }
        if (isize == 4) {
            return {convert<u64>(mathFloat32bits(double(-2147483648.0))), convert<u64>(mathFloat32bits(double(2147483648.0))), false};
        }
        return {convert<u64>(mathFloat32bits(double(-9.223372036854776e+18))), convert<u64>(mathFloat32bits(double(9.223372036854776e+18))), false};
    }

    // typeID is a structural identity of a function type, equal for equal
    // signatures across modules: call_indirect compares it at run time.
    static u64 typeID(FuncType* t) {
        u64 id = 1;
        Int bits = 1;
        auto push = [&](u64 code) mutable -> void {
            id = shiftLeft(id, 3) | code;
            bits += 3;
        };
        auto params = t->params;
        for (Int i = 0; i < length(params); ++i) {
            ValType p = params[i];
            push(valTypeCode(p));
        }
        push(u64(0LL));
        auto results = t->results;
        for (Int i = 0; i < length(results); ++i) {
            ValType r = results[i];
            push(valTypeCode(r));
        }
        if (bits <= 63) {
            return id;
        }
        u64 h = u64(14695981039346656037ULL);
        auto range1630 = t->params;
        for (Int index1631 = 0, rangeEnd1632 = length(range1630); index1631 < rangeEnd1632; ++index1631) {
            ValType p = range1630[index1631];
            h = (h ^ valTypeCode(p)) * u64(1099511628211LL);
        }
        h = (h ^ 7) * u64(1099511628211LL);
        auto range1633 = t->results;
        for (Int index1634 = 0, rangeEnd1635 = length(range1633); index1634 < rangeEnd1635; ++index1634) {
            ValType r = range1633[index1634];
            h = (h ^ valTypeCode(r)) * u64(1099511628211LL);
        }
        return h | u64(9223372036854775808ULL);
    }

    static u8 typeSize(ValType t) {
        switch (t) {
            case I32:
            case F32: {
                return u8(4LL);
            }
            case V128: {
                return u8(16LL);
            }
        }
        return u8(8LL);
    }

    static String typesPart(Slice<ValType> ts) {
        Slice<u8> b = makeSlice<u8>(length(ts));
        for (Int i = 0; i < length(ts); ++i) {
            ValType t = ts[i];
            auto temporary1640 = typeLetter(t);
            u8 c = temporary1640.v0;
            bool ok = temporary1640.v1;
            if (!ok) {
                compilerError(String("unsupported: a %s at the boundary"));
            }
            b[i] = c;
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
        return FloatRange{mathInf(-1), mathInf(1), true};
    }

    static u64 valTypeCode(ValType t) {
        switch (t) {
            case I32: {
                return u64(1LL);
            }
            case I64: {
                return u64(2LL);
            }
            case F32: {
                return u64(3LL);
            }
            case F64: {
                return u64(4LL);
            }
            case V128: {
                return u64(5LL);
            }
            case FuncRef: {
                return u64(6LL);
            }
        }
        return u64(7LL);
    }

    // versionedPreds brings predecessors up to date after versioning a loop:
    // the preheader's edge to the header now leads to a block numbered from
    // first, and every other new edge leaves such a block. Predecessors stay
    // in the order of their blocks, as computePreds lists them.
    static void versionedPreds(Function* f, i32 preheader, i32 header, Int first) {
        PoolScope graphScope(*f->blocks.owner);
        Slice<i32> preds = f->blocks[header].preds;
        Int at = 0;
        while (preds[at] != preheader) {
            ++at;
        }
        for (Int i = at + 1; i < length(preds); ++i) {
            preds[i - 1] = preds[i];
        }
        f->blocks[header].preds = subslice(preds, 0, length(preds) - 1);
        auto add = [&](i32 from) {
            auto edges = f->blocks[from].term.edges;
            for (Int j = 0; j < length(edges); ++j) {
                const i32 to = edges[j].block;
                if (from >= first || to >= first) {
                    f->blocks[to].preds = appendValues(f->blocks[to].preds, from);
                }
            }
        };
        add(preheader);
        for (Int b = first; b < length(f->blocks); ++b) {
            add(i32(b));
        }
    }

    // versionMemoryLoops reports whether it may have changed the function.
    static bool versionMemoryLoops(Function* f) {
        if (length(f->module->mems) == 0) {
            return false;
        }
        // No pass before this one adds a cycle. Without loops there is nothing
        // to version, and the analysis would only have refreshed preds.
        if (!f->hasLoops) {
            if (!f->cfg) {
                functionComputePreds(f);
            }
            return false;
        }
        LoopBodyUses bodyUses(f);
        // Only what loops read outlives the analysis, so the function can
        // grow into the memory of its working sets.
        ObjPool cfgPool;
        CfgInfo* ci;
        {
            ObjPool analysisPool;
            CfgInfo* full = f->cfg;
            if (!full) {
                PoolScope analysisScope(analysisPool);
                full = functionAnalyze(f);
            }
            PoolScope cfgScope(cfgPool);
            ci = activePool->make<CfgInfo>(CfgInfo{appendSlice(Slice<i32>{}, full->order), appendSlice(Slice<i32>{}, full->rpoIndex), appendSlice(Slice<i32>{}, full->idom), {}, appendSlice(Slice<bool>{}, full->header), {}, {}, {}, {}});
            cfgInfoPreorder(ci, f);
        }
        // Versioning sends the preheader's edge to blocks of its own, which
        // enter the original loop first, and adds no other edge between
        // analyzed blocks. Their order, dominators and headers stay as
        // analyzed, so one analysis serves every loop of the function.
        bool tried = false;
        Int versioned = 0;
        auto order = ci->order;
        for (Int i = 0; i < length(order) && versioned < 64; ++i) {
            i32 h = order[i];
            if (!ci->header[h]) {
                continue;
            }
            ObjPool loopPool;
            MemoryLoop* loop;
            {
                PoolScope analysisScope(loopPool);
                loop = findMemoryLoop(f, ci, h);
                if (loop) {
                    loop->bodyUses = &bodyUses;
                    loop->invariantNodes = makeSlice<i32>(0);
                }
            }
            if (isNil(loop)) {
                continue;
            }
            tried = true;
            const Int before = length(f->blocks);
            const i32 fast = memoryLoopVersion(loop);
            {
                PoolScope analysisScope(cfgPool);
                for (Int b = before; b < length(f->blocks); ++b) {
                    ci->added = appendValues(ci->added, loop->preheader);
                }
            }
            if (fast >= 0) {
                versionedPreds(f, loop->preheader, loop->header, before);
                f->cfg = nullptr;
                versioned++;
            }
        }
        return tried;
    }

    // windowBase reports whether the lowering addresses an access ending end
    // bytes past addr from a base, addr being the sum of the base and a
    // constant c within the slack, and gives the base and c. Unless proven,
    // such an access checks the base as far as c + end past it; it checks
    // the sum only for c >= 0, as one below its base may have wrapped around
    // to far past the size.
    static bool windowBase(Function* f, i32 addr, i64 end, i32* base, i64* c) {
        if (!f->module->memoryTail || end > boundSlack || f->values[addr].op != OpAdd) {
            return false;
        }
        auto constant = [&](i32 v) {
            const Op op = f->values[v].op;
            return op == OpConst || op == OpUndef || op == OpRefNull;
        };
        Span<i32> args = functionArgs(f, addr);
        i32 b = args[0];
        i32 k = args[1];
        if (constant(b)) {
            b = args[1];
            k = args[0];
        }
        if (!constant(k) || constant(b)) {
            return false;
        }
        *base = b;
        *c = f->values[k].op == OpConst ? convert<i64>(convert<i32>(convert<u32>(f->values[k].aux))) : 0;
        return *c >= -boundSlack && *c + end <= boundSlack;
    }

    static void writeELF(ObjCode* oc, Slice<u8> note) {
        constexpr Int secText = 11;
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
        constexpr Int stbLocal = 0;
        constexpr Int stbGlobal = 1;
        constexpr Int sttNotype = 0;
        constexpr Int sttFunc = 2;
        constexpr Int sttSect = 3;
        constexpr Int symText = 1;
        constexpr Int symRodata = 2;
        sym(u32(0LL), u8(0LL), u16(0LL), u64(0LL), u64(0LL));
        sym(u32(0LL), u8(3LL), u16(1LL), u64(0LL), u64(0LL));
        sym(u32(0LL), u8(3LL), u16(3LL), u64(0LL), u64(0LL));
        Int count = 3;
        auto regions = oc->regions;
        for (Int i = 0; i < length(regions); ++i) {
            Region r = regions[i];
            if (!oc->globals[r.name]) {
                sym(str(r.name), u8(2LL), u16(1LL), convert<u64>(r.start), convert<u64>(r.end - r.start));
                count++;
            }
        }
        Int firstGlobal = count;
        auto range1654 = oc->regions;
        for (Int index1655 = 0, rangeEnd1656 = length(range1654); index1655 < rangeEnd1656; ++index1655) {
            Region r = range1654[index1655];
            if (oc->globals[r.name]) {
                sym(str(r.name), u8(18LL), u16(1LL), convert<u64>(r.start), convert<u64>(r.end - r.start));
                count++;
            }
        }
        Int externBase = count;
        auto externs = oc->externs;
        for (Int i = 0; i < length(externs); ++i) {
            String name = externs[i];
            sym(str(name), u8(16LL), u16(0LL), u64(0LL), u64(0LL));
        }
        auto rela = [&](Slice<u8> out, u64 at, Int symbol, u32 typ, i64 addend) mutable -> Slice<u8> {
            out = littleAppendUint64(out, at);
            out = littleAppendUint64(out, shiftLeft(convert<u64>(symbol), 32) | convert<u64>(typ));
            return littleAppendUint64(out, convert<u64>(addend));
        };
        Slice<u8> relaText = {};
        auto relocs = oc->relocs;
        for (Int i = 0; i < length(relocs); ++i) {
            TextReloc r = relocs[i];
            if (r.sym < 0) {
                relaText = rela(relaText, convert<u64>(r.at), 2, rX86_64_PC32, r.addend);
            } else {
                relaText = rela(relaText, convert<u64>(r.at), externBase + convert<i64>(r.sym), rX86_64_GOTPCREL, r.addend);
            }
        }
        auto addresses = oc->addresses;
        for (Int i = 0; i < length(addresses); ++i) {
            TextAddress r = addresses[i];
            relaText = rela(relaText, convert<u64>(r.at), 1, rX86_64_64, convert<i64>(r.target));
        }
        auto temporary1666 = ehFrame(oc->regions);
        Slice<u8> eh = temporary1666.v0;
        Slice<u32> starts = temporary1666.v1;
        Slice<u8> relaEh = {};
        for (Int k = 0; k < length(starts); ++k) {
            u32 at = starts[k];
            relaEh = rela(relaEh, convert<u64>(at), 1, rX86_64_PC32, convert<i64>(oc->regions[k].start));
        }
        Slice<u8> noteBody = littleAppendUint32({}, 7);
        noteBody = littleAppendUint32(noteBody, convert<u32>(length(note)));
        noteBody = littleAppendUint32(noteBody, 1);
        noteBody = appendValues(appendSlice(noteBody, noteOwner), u8(0LL));
        while (length(noteBody) % Int(4LL) != Int(0LL)) {
            noteBody = appendValues(noteBody, u8(0LL));
        }
        noteBody = appendSlice(noteBody, note);
        while (length(noteBody) % Int(4LL) != Int(0LL)) {
            noteBody = appendValues(noteBody, u8(0LL));
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
        auto range1670 = Array<Anonymous24, 10>{{Anonymous24{1, String(".text")}, Anonymous24{2, String(".rela.text")}, Anonymous24{3, String(".rodata")}, Anonymous24{4, String(".eh_frame")}, Anonymous24{5, String(".rela.eh_frame")}, Anonymous24{6, String(".note.wasm2o")}, Anonymous24{7, String(".symtab")}, Anonymous24{8, String(".strtab")}, Anonymous24{9, String(".shstrtab")}, Anonymous24{10, String(".note.GNU-stack")}}};
        for (Int index1671 = 0, rangeEnd1672 = length(range1670); index1671 < rangeEnd1672; ++index1671) {
            Anonymous24 sec = range1670[index1671];
            names[sec.index] = sh(sec.name);
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
        for (Int i = 0; i < length(out); ++i) {
            out[i] = XMM0 + convert<VReg>(i);
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

    // A reader of DWARF's encodings over a section; ok goes false at its end.
    struct DwarfReader {
        Slice<u8> data{};
        Int pos{};
        bool ok = true;

        u8 byte() {
            if (pos < 0 || pos >= length(data)) {
                ok = false;
                return 0;
            }
            return data[pos++];
        }

        void skip(u64 count) {
            if (pos < 0 || pos > length(data) || count > u64(length(data) - pos)) {
                ok = false;
                return;
            }
            pos += convert<Int>(count);
        }

        u64 fixed(Int n) {
            if (n < 0 || n > 8) {
                ok = false;
                return 0;
            }
            u64 v = 0;
            for (Int i = 0; i < n; ++i) {
                v |= u64(byte()) << (8 * i);
            }
            return v;
        }

        u64 uleb() {
            u64 v = 0;
            Int shift = 0;
            for (;;) {
                const u8 b = byte();
                if (shift < 64) {
                    v |= u64(b & 127) << shift;
                }
                shift += 7;
                if (!(b & 128) || !ok) {
                    return v;
                }
            }
        }

        i64 sleb() {
            i64 v = 0;
            Int shift = 0;
            u8 b = 0;
            do {
                b = byte();
                if (shift < 64) {
                    v |= i64(u64(b & 127) << shift);
                }
                shift += 7;
            } while ((b & 128) && ok);
            if (shift < 64 && (b & 64)) {
                v |= -(i64(1) << shift);
            }
            return v;
        }

        String cstr() {
            const Int start = pos;
            while (pos < length(data) && data[pos] != 0) {
                pos++;
            }
            String s = asString(subslice(data, start, pos));
            if (pos < length(data)) {
                pos++;
            } else {
                ok = false;
            }
            return s;
        }
    };

    // dwarfString is the string at off of a string section, or empty.
    static String dwarfString(Slice<u8> section, u64 off) {
        if (off >= u64(length(section))) {
            return String("");
        }
        DwarfReader r{section, convert<Int>(off), true};
        return r.cstr();
    }

    // dwarfPath joins a directory of the table and a file name.
    static String dwarfPath(Slice<String> dirs, u64 dir, String name) {
        if (length(name) && name[0] == '/') {
            return name;
        }
        if (dir < u64(length(dirs)) && length(dirs[convert<Int>(dir)])) {
            return dirs[convert<Int>(dir)] + String("/") + name;
        }
        return name;
    }

    // dwarfForm reads a value of a DWARF 5 entry format: a string, from the
    // entry or a string section, or a number; ok goes false for a form
    // it does not know.
    static Tuple2<String, u64> dwarfForm(DwarfReader* r, u64 form, Int offsetSize, Module* m) {
        switch (form) {
            case 0x08: {
                return {r->cstr(), 0};
            }
            case 0x1f: {
                return {dwarfString(m->debugLineStr, r->fixed(offsetSize)), 0};
            }
            case 0x0e: {
                return {dwarfString(m->debugStr, r->fixed(offsetSize)), 0};
            }
            case 0x0f: {
                return {String(""), r->uleb()};
            }
            case 0x0b: {
                return {String(""), r->fixed(1)};
            }
            case 0x05: {
                return {String(""), r->fixed(2)};
            }
            case 0x06: {
                return {String(""), r->fixed(4)};
            }
            case 0x07: {
                return {String(""), r->fixed(8)};
            }
            case 0x1e: {
                r->skip(16);
                return {String(""), 0};
            }
            case 0x09: {
                r->skip(r->uleb());
                return {String(""), 0};
            }
        }
        r->ok = false;
        return {String(""), 0};
    }

    // readLines runs the module's DWARF line programs (.debug_line, versions
    // 2 to 5) into one table of rows sorted by address, the offset of a wasm
    // instruction in the code section's contents, and the files they name:
    // a unit's files follow the units' before, so a row's file is an index
    // of the module's list. A program the reader does not understand ends
    // the table where it is.
    static void moduleReadLines(Module* m) {
        if (length(m->debugLine) == 0) {
            return;
        }
        DwarfReader r{m->debugLine, 0, true};
        Slice<LineRow> rows = {};
        Slice<String> files = {};
        while (r.ok && r.pos < length(r.data)) {
            u64 unitLength = r.fixed(4);
            Int offsetSize = 4;
            if (unitLength == 0xffffffffu) {
                unitLength = r.fixed(8);
                offsetSize = 8;
            }
            if (!r.ok || unitLength > u64(length(r.data) - r.pos)) {
                break;
            }
            const Int unitEnd = r.pos + convert<Int>(unitLength);
            const u64 version = r.fixed(2);
            if (version < 2 || version > 5) {
                break;
            }
            if (version >= 5) {
                r.fixed(1);
                r.fixed(1);
            }
            const u64 headerLength = r.fixed(offsetSize);
            if (!r.ok || r.pos > unitEnd || headerLength > u64(unitEnd - r.pos)) {
                break;
            }
            const Int programStart = r.pos + convert<Int>(headerLength);
            const u64 minInst = maximum(r.fixed(1), u64(1));
            if (version >= 4) {
                r.fixed(1);
            }
            r.fixed(1);
            const i64 lineBase = i64(i8(r.fixed(1)));
            const u64 lineRange = maximum(r.fixed(1), u64(1));
            const u64 opcodeBase = maximum(r.fixed(1), u64(1));
            Slice<u8> stdLengths = makeSlice<u8>(convert<Int>(opcodeBase - 1));
            for (Int i = 0; i < length(stdLengths); ++i) {
                stdLengths[i] = r.byte();
            }
            const Int fileBase = length(files);
            Slice<String> dirs = {};
            if (version <= 4) {
                // Directory 0 is the compilation directory, which only
                // .debug_info knows; file numbers count from 1.
                dirs = appendValues(dirs, String(""));
                for (;;) {
                    String d = r.cstr();
                    if (length(d) == 0 || !r.ok) {
                        break;
                    }
                    dirs = appendValues(dirs, d);
                }
                files = appendValues(files, String(""));
                for (;;) {
                    String name = r.cstr();
                    if (length(name) == 0 || !r.ok) {
                        break;
                    }
                    const u64 dir = r.uleb();
                    r.uleb();
                    r.uleb();
                    files = appendValues(files, dwarfPath(dirs, dir, name));
                }
            } else {
                Slice<Array<u64, 2>> formats = {};
                const u64 dirFormats = r.fixed(1);
                for (u64 i = 0; i < dirFormats; ++i) {
                    const u64 type = r.uleb();
                    formats = appendValues(formats, Array<u64, 2>{{type, r.uleb()}});
                }
                const u64 dirCount = r.uleb();
                for (u64 i = 0; i < dirCount && r.ok; ++i) {
                    String path = String("");
                    for (Int k = 0; k < length(formats); ++k) {
                        auto value = dwarfForm(&r, formats[k][1], offsetSize, m);
                        if (formats[k][0] == 1) {
                            path = value.v0;
                        }
                    }
                    dirs = appendValues(dirs, path);
                }
                formats = {};
                const u64 fileFormats = r.fixed(1);
                for (u64 i = 0; i < fileFormats; ++i) {
                    const u64 type = r.uleb();
                    formats = appendValues(formats, Array<u64, 2>{{type, r.uleb()}});
                }
                const u64 fileCount = r.uleb();
                for (u64 i = 0; i < fileCount && r.ok; ++i) {
                    String name = String("");
                    u64 dir = 0;
                    for (Int k = 0; k < length(formats); ++k) {
                        auto value = dwarfForm(&r, formats[k][1], offsetSize, m);
                        if (formats[k][0] == 1) {
                            name = value.v0;
                        } else if (formats[k][0] == 2) {
                            dir = value.v1;
                        }
                    }
                    files = appendValues(files, dwarfPath(dirs, dir, name));
                }
            }
            if (!r.ok || r.pos > programStart) {
                break;
            }
            r.pos = programStart;
            u64 address = 0;
            u64 file = 1;
            i64 line = 1;
            auto row = [&](bool end) mutable -> void {
                rows = appendValues(rows, LineRow{convert<u32>(address), convert<u32>(fileBase + convert<Int>(file)), convert<u32>(line), end});
            };
            while (r.ok && r.pos < unitEnd) {
                const u8 op = r.byte();
                if (op >= opcodeBase) {
                    const u64 adjusted = op - opcodeBase;
                    address += (adjusted / lineRange) * minInst;
                    line += lineBase + i64(adjusted % lineRange);
                    row(false);
                } else if (op == 0) {
                    const u64 len = r.uleb();
                    if (len == 0) {
                        continue;
                    }
                    if (!r.ok || len > u64(unitEnd - r.pos)) {
                        r.ok = false;
                        break;
                    }
                    const Int next = r.pos + convert<Int>(len);
                    const u8 sub = r.byte();
                    switch (sub) {
                        case 1: {
                            row(true);
                            address = 0;
                            file = 1;
                            line = 1;
                            break;
                        }
                        case 2: {
                            address = r.fixed(convert<Int>(len) - 1);
                            break;
                        }
                        case 3: {
                            String name = r.cstr();
                            const u64 dir = r.uleb();
                            r.uleb();
                            r.uleb();
                            files = appendValues(files, dwarfPath(dirs, dir, name));
                            break;
                        }
                        default: {
                            break;
                        }
                    }
                    r.pos = next;
                } else {
                    switch (op) {
                        case 1: {
                            row(false);
                            break;
                        }
                        case 2: {
                            address += r.uleb() * minInst;
                            break;
                        }
                        case 3: {
                            line += r.sleb();
                            break;
                        }
                        case 4: {
                            file = r.uleb();
                            break;
                        }
                        case 5:
                        case 12: {
                            r.uleb();
                            break;
                        }
                        case 8: {
                            address += ((255 - opcodeBase) / lineRange) * minInst;
                            break;
                        }
                        case 9: {
                            address += r.fixed(2);
                            break;
                        }
                        case 6:
                        case 7:
                        case 10:
                        case 11: {
                            break;
                        }
                        default: {
                            for (u64 i = 0; i < stdLengths[op - 1]; ++i) {
                                r.uleb();
                            }
                            break;
                        }
                    }
                }
            }
            r.pos = unitEnd;
        }
        // The rows in order of address; of rows at one address the last
        // written holds, so the order is kept among them.
        Slice<Int> order = makeSlice<Int>(length(rows));
        for (Int i = 0; i < length(order); ++i) {
            order[i] = i;
        }
        sortValues(order, [&](Int x, Int y) mutable -> bool {
            return rows[order[x]].address < rows[order[y]].address || (rows[order[x]].address == rows[order[y]].address && order[x] < order[y]);
        });
        Slice<LineRow> sorted = makeSlice<LineRow>(length(rows));
        for (Int i = 0; i < length(order); ++i) {
            sorted[i] = rows[order[i]];
        }
        m->sourceFiles = files;
        m->lineRows = sorted;
    }

    // lines gives the description the source lines of the text: for every
    // mark of every region, the row of the module's line table at its
    // instruction, an entry where the file or line changes.
    static void objCodeLines(ObjCode* oc, Meta* meta) {
        Module* m = oc->m;
        moduleReadLines(m);
        auto rows = m->lineRows;
        if (length(rows) == 0) {
            return;
        }
        meta->files = m->sourceFiles;
        Slice<LineEntry> out = {};
        auto regions = oc->regions;
        for (Int i = 0; i < length(regions); ++i) {
            const Region r = regions[i];
            for (Int j = 0; j < length(r.lines); ++j) {
                const LineMark mark = r.lines[j];
                const Int k = sortSearch(length(rows), [&](Int x) mutable -> bool {
                    return rows[x].address > mark.pos;
                });
                if (k == 0 || rows[k - 1].end) {
                    continue;
                }
                const LineRow row = rows[k - 1];
                const u32 pc = r.start + convert<u32>(mark.pc);
                const Int n = length(out);
                if (n && out[n - 1].pc == pc) {
                    out[n - 1] = LineEntry{pc, row.file, row.line};
                    continue;
                }
                if (n && out[n - 1].file == row.file && out[n - 1].line == row.line) {
                    continue;
                }
                out = appendValues(out, LineEntry{pc, row.file, row.line});
            }
        }
        meta->lines = out;
    }

    static void moduleCustom(Module* module, Reader* reader) {
        String name = readerName(reader);
        if (name == String("name")) {
            readOptionalNames(module, *reader);
        }
        Slice<u8> rest = subslice(reader->data, reader->pos, length(reader->data));
        if (name == String(".debug_line")) {
            module->debugLine = rest;
        } else if (name == String(".debug_line_str")) {
            module->debugLineStr = rest;
        } else if (name == String(".debug_str")) {
            module->debugStr = rest;
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
            require(!chaosFail(ChaosFault::Read) && wasiFdRead(0, &vector, 1, &count) == 0, "stdin read failed");
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
        return Slice<u8>::of(input, 0, u32(input->count), u32(input->count));
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
        if (options.asan) {
            // The shadow map's checks take the place of the tail's.
            options.memoryTail = false;
            options.tailRanges = false;
            options.tailProfile = false;
            module->asan = length(module->mems) > 0;
        }
        if (options.memoryTail && length(module->mems) > 0) {
            module->memoryTail = boundSlack;
            module->tailRanges = options.tailRanges;
            module->tailProfile = options.tailProfile && options.tailRanges;
            if (!moduleLowFree(module)) {
                compilerError(String("unsupported: data or the stack in the first KiB of memory"));
            }
        }
        ObjCode* code = modulePool.make<ObjCode>();
        code->pool = &modulePool;
        code->m = module;
        code->a.fixups = makeSlice<RipFixup>(0);
        code->a.sps = makeSlice<SpEntry>(0);
        code->a.lines = makeSlice<LineMark>(0);
        code->funcAt = makeSlice<i32>(0);
        code->calls = makeSlice<CallFixup>(0);
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
        code->importSyms = makeSlice<String>(module->importedFuncs);
        code->importIntrinsics = makeSlice<Int>(module->importedFuncs);
        code->importCode = makeSlice<bool>(module->importedFuncs);
        objCodeAddFixedBlobs(code);
        objCodeImportAdapters(code);
        for (Int i = module->importedFuncs; i < length(module->funcs); ++i) {
            Function* function;
            {
                PoolScope ssaScope(ssaPool);
                function = buildFunction(module, i);
                optimize(function);
            }
            PoolScope machineScope(machinePool);
            MFunc* machine = lowerFunction(code, function);
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
    require((flags & ~u32(15)) == 0, "invalid compile options");
    compileWithOptions(CompileOptions{bool(flags & 1), bool(flags & 2), bool(flags & 4), bool(flags & 8)});
}

#ifdef AOT_CHAOS
extern "C"
    #if defined(__wasm__) || defined(wasm)
    __attribute__((export_name("chaosConfigure")))
    #endif
void chaosConfigure(u32 point, u32 skip) {
    require(point < u32(ChaosFault::Count), "unknown chaos point");
    chaosSkips[point] = skip;
    chaosArmed[point] = true;
}

    #if !defined(__wasm__) && !defined(wasm)
namespace {
    static void configureNativeChaos() {
        const char* script = ::getenv("PIZDOS_AOT_CHAOS");
        const char* names[] = {"read", "write", "grow"};
        while (script && *script) {
            u32 point = 0;
            usize length = 0;
            for (; point < u32(ChaosFault::Count); ++point) {
                length = 0;
                while (names[point][length] && names[point][length] == script[length]) {
                    ++length;
                }
                if (!names[point][length] && script[length] == '@') {
                    break;
                }
            }
            require(point < u32(ChaosFault::Count) && !chaosArmed[point], "invalid PIZDOS_AOT_CHAOS");
            script += length + 1;
            require(*script >= '0' && *script <= '9', "invalid PIZDOS_AOT_CHAOS");
            u64 skip = 0;
            while (*script >= '0' && *script <= '9') {
                skip = skip * 10 + u32(*script++ - '0');
                require(skip <= 0xffffffffULL, "invalid PIZDOS_AOT_CHAOS");
            }
            require(*script == 0 || *script == ',', "invalid PIZDOS_AOT_CHAOS");
            chaosConfigure(point, u32(skip));
            if (*script == ',') {
                ++script;
                require(*script != 0, "invalid PIZDOS_AOT_CHAOS");
            }
        }
    }
}
    #endif
#endif

namespace {
    static void compilerCommand(bool strict = false) {
        if (strict) {
            compileWithOptions(CompileOptions{});
        } else {
            compile();
        }
    }
}

#if !defined(__wasm__) && !defined(wasm)
static bool sameString(const char* a, const char* b) {
    usize i = 0;
    while (a[i] && a[i] == b[i]) {
        ++i;
    }
    return a[i] == 0 && b[i] == 0;
}

int main(int count, char** arguments) {
#ifdef AOT_CHAOS
    configureNativeChaos();
#endif
    const char* usage = "usage: aot [--plain|--strict|--asan|--tail-profile] < input.wasm > output.o";
    if (count == 2 && sameString(arguments[1], "--plain")) {
        compileOptions(1);
        return 0;
    }
    if (count == 2 && sameString(arguments[1], "--tail-profile")) {
        compileOptions(7);
        return 0;
    }
    if (count == 2 && sameString(arguments[1], "--asan")) {
        compileWithOptions(CompileOptions{false, false, false, true});
        return 0;
    }
    bool strict = false;
    if (count == 2) {
        require(sameString(arguments[1], "--strict"), usage);
        strict = true;
    } else {
        require(count == 1, usage);
    }
    compilerCommand(strict);
    return 0;
}
#elif defined(AOT_COMMAND)
extern "C" __attribute__((export_name("_start"))) void start() {
    compilerCommand();
}
#endif
