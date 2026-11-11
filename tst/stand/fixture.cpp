using u8 = __UINT8_TYPE__;
using u32 = __UINT32_TYPE__;

#define EXPORT(name) __attribute__((export_name(name)))

extern "C" u8 __heap_base;
static u32 next;
static u32 mode;
static u32 calls;

struct Image {
    u32 width;
    u32 height;
    u32 pixels[2];
};

static Image image;

extern "C" EXPORT("malloc") u32 allocate(u32 size) {
    if (size == 9999) {
        return 0;
    }
    if (!next) {
        next = reinterpret_cast<u32>(&__heap_base);
    }
    u32 result = next;
    next += (size + 15) & ~15u;
    u32 pages = (next + 65535) / 65536;
    u32 current = __builtin_wasm_memory_size(0);
    if (pages > current && __builtin_wasm_memory_grow(0, pages - current) == ~u32(0)) {
        return 0;
    }
    return result;
}

extern "C" EXPORT("free") void release(u32) {
}

extern "C" EXPORT("decode") u32 decode(const u8* input, u32 size, const char*, u32) {
    if (!size || input[0] == 'f') {
        return 0;
    }
    if (input[0] == 't') {
        __builtin_trap();
    }
    image = Image{2, 1, {0x04030201, 0x08070605}};
    return reinterpret_cast<u32>(&image);
}

extern "C" EXPORT("magic_mime") const char* mime(const u8* input, u32 size) {
    if (!size || input[0] == 'f') {
        return nullptr;
    }
    if (input[0] == 't') {
        __builtin_trap();
    }
    ++calls;
    return input[0] == 's' && calls % 2 == 0 ? "text/plain" : "application/x-fixture";
}

static u32 openDocument(const u8* input, u32 size) {
    if (!size || input[0] == 'f') {
        return 0;
    }
    if (input[0] == 't') {
        __builtin_trap();
    }
    mode = input[0];
    return 1;
}

static u32 pages(u32) {
    return mode == 'l' ? 18 : 4;
}

static float width(u32, u32 page) {
    return page == 0 ? 800 : page == 1 ? 0 : page == 2 ? 400 : 1;
}

static float height(u32, u32 page) {
    return page == 0 ? 400 : page == 2 ? 800 : 1;
}

static u32 render(u32, u32 page, u32 width, u32 height) {
    if (page == 2) {
        return 0;
    }
    image = Image{1, 1, {(width & 255) | ((height & 255) << 8) | (page << 16) | 0xff000000, 0}};
    return reinterpret_cast<u32>(&image);
}

extern "C" EXPORT("pdf_open") u32 pdfOpen(const u8* input, u32 size) {
    return openDocument(input, size);
}

extern "C" EXPORT("pdf_pages") u32 pdfPages(u32 document) {
    return pages(document);
}

extern "C" EXPORT("pdf_width") float pdfWidth(u32 document, u32 page) {
    return width(document, page);
}

extern "C" EXPORT("pdf_height") float pdfHeight(u32 document, u32 page) {
    return height(document, page);
}

extern "C" EXPORT("pdf_render") u32 pdfRender(u32 document, u32 page, u32 w, u32 h) {
    return render(document, page, w, h);
}

extern "C" EXPORT("pdf_close") void pdfClose(u32) {
}

extern "C" EXPORT("djvu_open") u32 djvuOpen(const u8* input, u32 size) {
    return openDocument(input, size);
}

extern "C" EXPORT("djvu_pages") u32 djvuPages(u32 document) {
    return pages(document);
}

extern "C" EXPORT("djvu_width") float djvuWidth(u32 document, u32 page) {
    return width(document, page);
}

extern "C" EXPORT("djvu_height") float djvuHeight(u32 document, u32 page) {
    return height(document, page);
}

extern "C" EXPORT("djvu_render") u32 djvuRender(u32 document, u32 page, u32 w, u32 h) {
    return render(document, page, w, h);
}

extern "C" EXPORT("djvu_close") void djvuClose(u32) {
}

extern "C" EXPORT("svg_open") u32 svgOpen(const u8* input, u32 size) {
    return openDocument(input, size);
}

extern "C" EXPORT("svg_pages") u32 svgPages(u32 document) {
    return pages(document);
}

extern "C" EXPORT("svg_width") float svgWidth(u32 document, u32 page) {
    return width(document, page);
}

extern "C" EXPORT("svg_height") float svgHeight(u32 document, u32 page) {
    return height(document, page);
}

extern "C" EXPORT("svg_render") u32 svgRender(u32 document, u32 page, u32 w, u32 h) {
    return render(document, page, w, h);
}

extern "C" EXPORT("svg_close") void svgClose(u32) {
}
