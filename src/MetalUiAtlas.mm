#include "MetalUiAtlas.h"

#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

namespace {

constexpr size_t kAtlasWidth = 2048;
constexpr size_t kAtlasHeight = 2048;
constexpr CGFloat kAtlasScale = 2.0;
constexpr size_t kPadding = 2;

struct AtlasEntry {
    size_t x = 0;
    size_t y = 0;
    size_t width = 0;
    size_t height = 0;
    double advance = 0.0;
};

struct GlyphKey {
    MetalFontFace face = MetalFontFace::Mono;
    bool bold = false;
    int point_size = 11;
    UniChar character = 0;

    bool operator<(const GlyphKey& other) const {
        return std::tie(face, bold, point_size, character) <
               std::tie(other.face, other.bold, other.point_size,
                        other.character);
    }
};

struct AtlasPacker {
    size_t x = 1;
    size_t y = 1;
    size_t row_height = 0;

    bool Allocate(size_t width, size_t height, AtlasEntry& entry) {
        if (x + width + 1 > kAtlasWidth) {
            x = 1;
            y += row_height + 1;
            row_height = 0;
        }
        if (y + height + 1 > kAtlasHeight) return false;
        entry.x = x;
        entry.y = y;
        entry.width = width;
        entry.height = height;
        x += width + 1;
        row_height = std::max(row_height, height);
        return true;
    }
};

NSFont* AtlasFont(MetalFontFace face, bool bold, CGFloat pointSize) {
    const NSFontWeight weight = bold ? NSFontWeightBold : NSFontWeightMedium;
    if (face == MetalFontFace::Mono)
        return [NSFont monospacedSystemFontOfSize:pointSize weight:weight];
    return [NSFont systemFontOfSize:pointSize weight:weight];
}

NSString* IconDirectory() {
    NSString* bundled = [NSBundle.mainBundle resourcePath];
    if (bundled.length > 0) {
        NSString* candidate = [bundled stringByAppendingPathComponent:@"icons"];
        if ([NSFileManager.defaultManager fileExistsAtPath:candidate])
            return candidate;
    }
    return [NSString stringWithUTF8String:CUTMACHINE_ICON_DIR];
}

bool LoadIconAlpha(NSString* path, std::vector<uint8_t>& alpha, size_t& width,
                   size_t& height) {
    NSImage* image = [[NSImage alloc] initWithContentsOfFile:path];
    if (!image) return false;
    NSRect proposed = NSMakeRect(0.0, 0.0, image.size.width, image.size.height);
    CGImageRef cgImage = [image CGImageForProposedRect:&proposed
                                               context:nil
                                                 hints:nil];
    if (!cgImage) return false;
    width = CGImageGetWidth(cgImage);
    height = CGImageGetHeight(cgImage);
    if (width == 0 || height == 0) return false;

    std::vector<uint8_t> rgba(width * height * 4, 0);
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(
        rgba.data(), width, height, 8, width * 4, space,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(space);
    if (!context) return false;
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), cgImage);
    CGContextRelease(context);

    alpha.resize(width * height);
    // CGBitmapContext's storage order already matches Metal's normalized
    // texture coordinates. Reversing these rows turns every icon upside down.
    for (size_t index = 0; index < width * height; ++index)
        alpha[index] = rgba[index * 4 + 3];
    return true;
}

}  // namespace

struct MetalUiAtlas::Impl {
    id<MTLTexture> texture = nil;
    std::map<GlyphKey, AtlasEntry> glyphs;
    std::map<std::string, AtlasEntry> icons;
};

MetalUiAtlas::MetalUiAtlas() : impl_(new Impl()) {}
MetalUiAtlas::~MetalUiAtlas() { delete impl_; }

bool MetalUiAtlas::Initialize(id<MTLDevice> device) {
    if (!device) return false;
    std::vector<uint8_t> atlas(kAtlasWidth * kAtlasHeight, 0);
    AtlasPacker packer;

    struct FontSpec {
        MetalFontFace face;
        bool bold;
        int point_size;
    };
    const std::array<FontSpec, 8> fonts{{
        {MetalFontFace::Mono, false, 10},
        {MetalFontFace::Mono, true, 10},
        {MetalFontFace::Mono, false, 11},
        {MetalFontFace::Mono, true, 11},
        {MetalFontFace::Mono, true, 32},
        {MetalFontFace::Sans, false, 11},
        {MetalFontFace::Sans, true, 12},
        {MetalFontFace::Sans, true, 13},
    }};

    for (const FontSpec& spec : fonts) {
        NSFont* nsFont =
            AtlasFont(spec.face, spec.bold, spec.point_size * kAtlasScale);
        CTFontRef font = (__bridge CTFontRef)nsFont;
        const size_t cellHeight =
            static_cast<size_t>(std::ceil(CTFontGetAscent(font) +
                                          CTFontGetDescent(font) +
                                          CTFontGetLeading(font))) +
            kPadding * 2;
        for (UniChar character = 0x20; character <= 0xff; ++character) {
            CGGlyph glyph = 0;
            if (!CTFontGetGlyphsForCharacters(font, &character, &glyph, 1) ||
                glyph == 0)
                continue;
            CGSize advance = {};
            CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal,
                                       &glyph, &advance, 1);
            const size_t cellWidth =
                static_cast<size_t>(std::ceil(std::max(1.0, advance.width))) +
                kPadding * 2;
            AtlasEntry entry;
            if (!packer.Allocate(cellWidth, cellHeight, entry)) {
                std::fprintf(stderr, "Metal UI atlas exhausted by glyphs\n");
                return false;
            }
            entry.advance = advance.width / kAtlasScale;
            impl_->glyphs[{spec.face, spec.bold, spec.point_size, character}] =
                entry;

            std::vector<uint8_t> cell(cellWidth * cellHeight, 0);
            CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
            CGContextRef context =
                CGBitmapContextCreate(cell.data(), cellWidth, cellHeight, 8,
                                      cellWidth, gray, kCGImageAlphaNone);
            CGColorSpaceRelease(gray);
            if (!context) return false;
            CGContextSetShouldAntialias(context, true);
            CGContextSetGrayFillColor(context, 1.0, 1.0);
            CGContextSetTextDrawingMode(context, kCGTextFill);
            CGContextSetTextMatrix(context, CGAffineTransformIdentity);
            const CGPoint position{
                static_cast<CGFloat>(kPadding),
                static_cast<CGFloat>(kPadding) + CTFontGetDescent(font)};
            CTFontDrawGlyphs(font, &glyph, &position, 1, context);
            CGContextRelease(context);

            // Keep CGBitmapContext's storage order. The shader maps v0 to the
            // quad's top edge, and Metal samples this byte order accordingly.
            for (size_t row = 0; row < cellHeight; ++row)
                std::copy_n(
                    cell.data() + row * cellWidth, cellWidth,
                    atlas.data() + (entry.y + row) * kAtlasWidth + entry.x);
        }
    }

    NSString* iconDirectory = IconDirectory();
    const std::array<const char*, 12> iconNames{{
        "chevrons-right-left",
        "eye",
        "hand",
        "headphones",
        "link",
        "lock",
        "magnet",
        "mouse-pointer-2",
        "scissors",
        "search",
        "unfold-horizontal",
        "volume-x",
    }};
    for (const char* name : iconNames) {
        NSString* filename = [[NSString stringWithUTF8String:name]
            stringByAppendingString:@".png"];
        NSString* path =
            [iconDirectory stringByAppendingPathComponent:filename];
        std::vector<uint8_t> alpha;
        size_t width = 0;
        size_t height = 0;
        if (!LoadIconAlpha(path, alpha, width, height)) {
            std::fprintf(stderr, "Unable to load rasterized UI icon: %s\n",
                         path.UTF8String);
            return false;
        }
        AtlasEntry entry;
        if (!packer.Allocate(width, height, entry)) {
            std::fprintf(stderr, "Metal UI atlas exhausted by icons\n");
            return false;
        }
        for (size_t row = 0; row < height; ++row)
            std::copy_n(alpha.data() + row * width, width,
                        atlas.data() + (entry.y + row) * kAtlasWidth + entry.x);
        entry.advance = static_cast<double>(width) / kAtlasScale;
        impl_->icons[name] = entry;
    }

    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                     width:kAtlasWidth
                                    height:kAtlasHeight
                                 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    impl_->texture = [device newTextureWithDescriptor:descriptor];
    if (!impl_->texture) return false;
    [impl_->texture
        replaceRegion:MTLRegionMake2D(0, 0, kAtlasWidth, kAtlasHeight)
          mipmapLevel:0
            withBytes:atlas.data()
          bytesPerRow:kAtlasWidth];
    return true;
}

id<MTLTexture> MetalUiAtlas::texture() const { return impl_->texture; }

void MetalUiAtlas::AppendTextQuads(const MetalText& text,
                                   std::vector<MetalAtlasQuad>& quads) const {
    NSString* string = [NSString stringWithUTF8String:text.text.c_str()];
    if (!string) return;
    const int pointSize = static_cast<int>(std::lround(text.point_size));
    double cursor = text.x;
    const double right = text.max_width > 0.0
                             ? text.x + text.max_width
                             : std::numeric_limits<double>::infinity();
    for (NSUInteger index = 0; index < string.length; ++index) {
        UniChar character = [string characterAtIndex:index];
        GlyphKey key{text.face, text.bold, pointSize, character};
        auto found = impl_->glyphs.find(key);
        if (found == impl_->glyphs.end()) {
            key.character = '?';
            found = impl_->glyphs.find(key);
        }
        if (found == impl_->glyphs.end()) continue;
        const AtlasEntry& entry = found->second;
        if (cursor + entry.advance > right) break;
        quads.push_back({
            cursor,
            text.y,
            static_cast<double>(entry.width) / kAtlasScale,
            static_cast<double>(entry.height) / kAtlasScale,
            static_cast<float>(entry.x) / kAtlasWidth,
            static_cast<float>(entry.y) / kAtlasHeight,
            static_cast<float>(entry.x + entry.width) / kAtlasWidth,
            static_cast<float>(entry.y + entry.height) / kAtlasHeight,
            text.color,
        });
        cursor += entry.advance;
    }
}

bool MetalUiAtlas::IconQuad(const MetalIcon& icon, MetalAtlasQuad& quad) const {
    const auto found = impl_->icons.find(icon.name);
    if (found == impl_->icons.end() || icon.width <= 0.0 || icon.height <= 0.0)
        return false;
    const AtlasEntry& entry = found->second;
    quad = {
        icon.x,
        icon.y,
        icon.width,
        icon.height,
        static_cast<float>(entry.x) / kAtlasWidth,
        static_cast<float>(entry.y) / kAtlasHeight,
        static_cast<float>(entry.x + entry.width) / kAtlasWidth,
        static_cast<float>(entry.y + entry.height) / kAtlasHeight,
        icon.color,
    };
    return true;
}
