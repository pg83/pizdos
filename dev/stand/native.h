#pragma once

// The native forms of decode's exports. Pointers replace wasm offsets;
// the image block still begins with two little-endian uint32_t values.
struct image;
struct SvgDocument;

struct image* decode(const uint8_t*, size_t, const char*, size_t);
const char* magic_mime(const void*, uint32_t);

#define STAND_PAGE(prefix, Doc) \
    Doc* prefix##_open(const uint8_t*, size_t); \
    uint32_t prefix##_pages(Doc*); \
    float prefix##_width(Doc*, uint32_t); \
    float prefix##_height(Doc*, uint32_t); \
    struct image* prefix##_render(Doc*, uint32_t, uint32_t, uint32_t); \
    void prefix##_close(Doc*)

STAND_PAGE(pdf, void);
STAND_PAGE(djvu, void);
STAND_PAGE(svg, struct SvgDocument);

#undef STAND_PAGE
