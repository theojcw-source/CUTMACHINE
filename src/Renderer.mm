#include "Renderer.h"

#include "ColorManagement.h"
#include "MetalUiAtlas.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <array>
#include <cstdio>

struct Renderer::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    CAMetalLayer* layer = nil;
    id<MTLRenderPipelineState> workingPipeline = nil;
    id<MTLRenderPipelineState> outputPipeline = nil;
    id<MTLComputePipelineState> scopePipeline = nil;
    id<MTLRenderPipelineState> solidPipeline = nil;
    id<MTLRenderPipelineState> textPipeline = nil;
    id<MTLSamplerState> sampler = nil;
    MetalUiAtlas uiAtlas;
    id<MTLTexture> workingTexture = nil;
    id<MTLTexture> ocioInputLut = nil;
    id<MTLTexture> ocioOutputLut = nil;
    std::string ocioLutKey;
    id<MTLBuffer> scopeHistogram = nil;
    size_t scopeHistogramBins = 0;
    std::vector<std::array<id<MTLTexture>, 4>> planes;
    std::vector<std::array<int, 4>> textureWidths;
    std::vector<std::array<int, 4>> textureHeights;
    std::vector<int> textureFormats;
    CGColorSpaceRef sdrColorSpace = nullptr;
    CGColorSpaceRef hlgColorSpace = nullptr;

    ~Impl() {
        if (sdrColorSpace) CGColorSpaceRelease(sdrColorSpace);
        if (hlgColorSpace) CGColorSpaceRelease(hlgColorSpace);
    }
};

namespace {

id<MTLTexture> UploadOpenColorIoLut(id<MTLDevice> device,
                                    const OpenColorIoLut3D& lut) {
    if (!device || lut.edge <= 1 ||
        lut.rgba.size() !=
            static_cast<size_t>(lut.edge) * lut.edge * lut.edge * 4)
        return nil;
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor new];
    descriptor.textureType = MTLTextureType3D;
    descriptor.pixelFormat = MTLPixelFormatRGBA32Float;
    descriptor.width = static_cast<NSUInteger>(lut.edge);
    descriptor.height = static_cast<NSUInteger>(lut.edge);
    descriptor.depth = static_cast<NSUInteger>(lut.edge);
    descriptor.mipmapLevelCount = 1;
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    if (!texture) return nil;
    const NSUInteger bytesPerRow =
        static_cast<NSUInteger>(lut.edge) * 4 * sizeof(float);
    [texture
        replaceRegion:MTLRegionMake3D(0, 0, 0, lut.edge, lut.edge, lut.edge)
          mipmapLevel:0
                slice:0
            withBytes:lut.rgba.data()
          bytesPerRow:bytesPerRow
        bytesPerImage:bytesPerRow * static_cast<NSUInteger>(lut.edge)];
    return texture;
}

}  // namespace

Renderer::Renderer() : impl_(new Impl()) {}
Renderer::~Renderer() { delete impl_; }

bool Renderer::Initialize(NSView* view) {
    impl_->device = MTLCreateSystemDefaultDevice();
    impl_->queue = [impl_->device newCommandQueue];
    if (!impl_->device || !impl_->queue) {
        std::fprintf(stderr, "Unable to initialize Metal device/queue\n");
        return false;
    }

    impl_->layer = [CAMetalLayer layer];
    impl_->layer.device = impl_->device;
    // XR keeps ten useful bits for HLG while remaining usable for SDR projects.
    impl_->layer.pixelFormat = MTLPixelFormatBGRA10_XR;
    impl_->layer.framebufferOnly = YES;
    // While a splitter is being dragged, keep the last drawable's aspect
    // instead of letting Core Animation stretch it to intermediate bounds.
    impl_->layer.contentsGravity = kCAGravityResizeAspect;
    impl_->sdrColorSpace = CGColorSpaceCreateWithName(kCGColorSpaceITUR_709);
    impl_->hlgColorSpace =
        CGColorSpaceCreateWithName(kCGColorSpaceITUR_2100_HLG);
    impl_->layer.colorspace = impl_->sdrColorSpace;
    impl_->layer.contentsScale = view.window.backingScaleFactor
                                     ?: NSScreen.mainScreen.backingScaleFactor;
    [view setWantsLayer:YES];
    view.layer = impl_->layer;
    Resize(view.bounds);

    NSError* error = nil;
    NSString* shaderPath =
        [NSString stringWithUTF8String:CUTMACHINE_SHADER_PATH];
    if (![[NSFileManager defaultManager] fileExistsAtPath:shaderPath]) {
        NSString* bundled = [[NSBundle mainBundle] pathForResource:@"shader"
                                                            ofType:@"metal"];
        if (bundled.length > 0) shaderPath = bundled;
    }
    NSString* shaderSource =
        [NSString stringWithContentsOfFile:shaderPath
                                  encoding:NSUTF8StringEncoding
                                     error:&error];
    if (!shaderSource) {
        std::fprintf(stderr, "Unable to read %s: %s\n", shaderPath.UTF8String,
                     error.localizedDescription.UTF8String);
        return false;
    }
    id<MTLLibrary> library = [impl_->device newLibraryWithSource:shaderSource
                                                         options:nil
                                                           error:&error];
    if (!library) {
        std::fprintf(stderr, "Unable to compile %s: %s\n",
                     shaderPath.UTF8String,
                     error.localizedDescription.UTF8String);
        return false;
    }

    id<MTLFunction> vertex = [library newFunctionWithName:@"vertex_main"];
    id<MTLFunction> fragment =
        [library newFunctionWithName:@"fragment_working"];
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = vertex;
    descriptor.fragmentFunction = fragment;
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    descriptor.colorAttachments[0].blendingEnabled = YES;
    descriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
    descriptor.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
    descriptor.colorAttachments[0].sourceRGBBlendFactor =
        MTLBlendFactorSourceAlpha;
    descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    descriptor.colorAttachments[0].destinationRGBBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
    descriptor.colorAttachments[0].destinationAlphaBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
    impl_->workingPipeline =
        [impl_->device newRenderPipelineStateWithDescriptor:descriptor
                                                      error:&error];
    if (!impl_->workingPipeline) {
        std::fprintf(stderr, "Unable to create Metal working pipeline: %s\n",
                     error.localizedDescription.UTF8String);
        return false;
    }

    descriptor.vertexFunction = vertex;
    descriptor.fragmentFunction =
        [library newFunctionWithName:@"fragment_output"];
    descriptor.colorAttachments[0].pixelFormat = impl_->layer.pixelFormat;
    descriptor.colorAttachments[0].blendingEnabled = NO;
    impl_->outputPipeline =
        [impl_->device newRenderPipelineStateWithDescriptor:descriptor
                                                      error:&error];
    if (!impl_->outputPipeline) {
        std::fprintf(stderr, "Unable to create Metal output pipeline: %s\n",
                     error.localizedDescription.UTF8String);
        return false;
    }

    id<MTLFunction> scopeFunction =
        [library newFunctionWithName:@"compute_video_scope"];
    impl_->scopePipeline =
        [impl_->device newComputePipelineStateWithFunction:scopeFunction
                                                     error:&error];
    if (!impl_->scopePipeline) {
        std::fprintf(stderr, "Unable to create video scope pipeline: %s\n",
                     error.localizedDescription.UTF8String);
        return false;
    }

    descriptor.vertexFunction = [library newFunctionWithName:@"vertex_solid"];
    descriptor.fragmentFunction =
        [library newFunctionWithName:@"fragment_solid"];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    descriptor.colorAttachments[0].blendingEnabled = YES;
    descriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
    descriptor.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
    descriptor.colorAttachments[0].sourceRGBBlendFactor =
        MTLBlendFactorSourceAlpha;
    descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    descriptor.colorAttachments[0].destinationRGBBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
    descriptor.colorAttachments[0].destinationAlphaBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
    impl_->solidPipeline =
        [impl_->device newRenderPipelineStateWithDescriptor:descriptor
                                                      error:&error];
    if (!impl_->solidPipeline) {
        std::fprintf(stderr, "Unable to create solid pipeline: %s\n",
                     error.localizedDescription.UTF8String);
        return false;
    }

    descriptor.vertexFunction = [library newFunctionWithName:@"vertex_text"];
    descriptor.fragmentFunction =
        [library newFunctionWithName:@"fragment_text"];
    impl_->textPipeline =
        [impl_->device newRenderPipelineStateWithDescriptor:descriptor
                                                      error:&error];
    if (!impl_->textPipeline) {
        std::fprintf(stderr, "Unable to create text pipeline: %s\n",
                     error.localizedDescription.UTF8String);
        return false;
    }

    MTLSamplerDescriptor* samplerDescriptor = [MTLSamplerDescriptor new];
    samplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
    samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
    samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    impl_->sampler =
        [impl_->device newSamplerStateWithDescriptor:samplerDescriptor];
    return impl_->sampler != nil && impl_->uiAtlas.Initialize(impl_->device);
}

void Renderer::Resize(NSRect bounds) {
    if (!impl_->layer) {
        return;
    }
    // Only the drawable is ours. This CAMetalLayer is the view's *backing*
    // layer (Initialize sets view.layer), so AppKit owns its frame and
    // expresses it in the superview's coordinates -- while every caller here
    // passes view.bounds, whose origin is always (0,0). Assigning that as the
    // layer frame teleported the surface to the top-left of its enclosing
    // view: harmless for the monitors, which do sit at their panel's origin,
    // but it lifted the timeline out of the bottom pane and drew it behind
    // the monitors, visible through the gap between them.
    const CGFloat scale = impl_->layer.contentsScale;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    impl_->layer.drawableSize =
        CGSizeMake(bounds.size.width * scale, bounds.size.height * scale);
    [CATransaction commit];
}

bool Renderer::RenderFrames(const std::vector<AVFrame*>& frames,
                            const TimelineRenderData& timeline) {
    if (!impl_->workingPipeline || !impl_->outputPipeline ||
        !impl_->scopePipeline || !impl_->solidPipeline ||
        !impl_->textPipeline || !impl_->queue) {
        return false;
    }

    if (impl_->planes.size() < frames.size()) {
        impl_->planes.resize(frames.size());
        impl_->textureWidths.resize(frames.size(), {});
        impl_->textureHeights.resize(frames.size(), {});
        impl_->textureFormats.resize(frames.size(), AV_PIX_FMT_NONE);
    }
    for (size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex) {
        const AVFrame* frame = frames[frameIndex];
        if (!frame) continue;
        const AVPixelFormat pixelFormat =
            static_cast<AVPixelFormat>(frame->format);
        const AVPixFmtDescriptor* pixel = av_pix_fmt_desc_get(pixelFormat);
        if (frame->width <= 0 || frame->height <= 0 || !pixel ||
            pixel->nb_components < 3 ||
            (pixel->flags & (AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM |
                             AV_PIX_FMT_FLAG_HWACCEL)) ||
            pixel->comp[0].plane == pixel->comp[1].plane ||
            pixel->comp[0].plane == pixel->comp[2].plane ||
            pixel->comp[1].plane == pixel->comp[2].plane ||
            pixel->comp[0].depth != pixel->comp[1].depth ||
            pixel->comp[0].depth != pixel->comp[2].depth ||
            pixel->comp[0].depth < 8 || pixel->comp[0].depth > 16 ||
            !frame->data[0] || !frame->data[1] || !frame->data[2]) {
            const char* name = av_get_pix_fmt_name(pixelFormat);
            std::fprintf(stderr,
                         "Unsupported planar AVFrame passed to renderer: %s\n",
                         name ? name : "unknown");
            return false;
        }
        const bool hasAlpha =
            pixel->nb_components >= 4 && (pixel->flags & AV_PIX_FMT_FLAG_ALPHA);
        if (hasAlpha && (pixel->comp[3].plane < 0 || pixel->comp[3].plane > 3 ||
                         pixel->comp[3].depth != pixel->comp[0].depth ||
                         !frame->data[pixel->comp[3].plane])) {
            std::fprintf(stderr,
                         "Unsupported alpha plane passed to renderer\n");
            return false;
        }
        std::array<int, 4> planeWidths = {
            frame->width, AV_CEIL_RSHIFT(frame->width, pixel->log2_chroma_w),
            AV_CEIL_RSHIFT(frame->width, pixel->log2_chroma_w), frame->width};
        std::array<int, 4> planeHeights = {
            frame->height, AV_CEIL_RSHIFT(frame->height, pixel->log2_chroma_h),
            AV_CEIL_RSHIFT(frame->height, pixel->log2_chroma_h), frame->height};
        if (impl_->textureWidths[frameIndex] != planeWidths ||
            impl_->textureHeights[frameIndex] != planeHeights ||
            impl_->textureFormats[frameIndex] != frame->format) {
            const int planeCount = hasAlpha ? 4 : 3;
            for (int plane = 0; plane < planeCount; ++plane) {
                MTLTextureDescriptor* textureDescriptor = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:
                        pixel->comp[0].depth > 8 ? MTLPixelFormatR16Unorm
                                                 : MTLPixelFormatR8Unorm
                                                 width:planeWidths[plane]
                                                height:planeHeights[plane]
                                             mipmapped:NO];
                textureDescriptor.storageMode = MTLStorageModeShared;
                textureDescriptor.usage = MTLTextureUsageShaderRead;
                impl_->planes[frameIndex][plane] =
                    [impl_->device newTextureWithDescriptor:textureDescriptor];
                if (!impl_->planes[frameIndex][plane]) {
                    std::fprintf(
                        stderr, "Unable to allocate Metal layer %zu plane %d\n",
                        frameIndex, plane);
                    return false;
                }
            }
            impl_->textureWidths[frameIndex] = planeWidths;
            impl_->textureHeights[frameIndex] = planeHeights;
            impl_->textureFormats[frameIndex] = frame->format;
        }

        const int planeCount = hasAlpha ? 4 : 3;
        for (int plane = 0; plane < planeCount; ++plane) {
            if (frame->linesize[plane] <= 0) {
                std::fprintf(stderr,
                             "Unsupported negative/zero linesize for layer %zu "
                             "plane %d: %d\n",
                             frameIndex, plane, frame->linesize[plane]);
                return false;
            }
            const MTLRegion region =
                MTLRegionMake2D(0, 0, planeWidths[plane], planeHeights[plane]);
            [impl_->planes[frameIndex][plane]
                replaceRegion:region
                  mipmapLevel:0
                    withBytes:frame->data[plane]
                  bytesPerRow:static_cast<NSUInteger>(frame->linesize[plane])];
        }
    }

    const ColorManagementSettings presentationColor =
        timeline.display_sdr_preview
            ? ColorManagementForSdrPreview(timeline.color_management)
            : timeline.color_management;
    if (presentationColor.enabled) {
        const std::string lutKey = OpenColorIoCacheKey(presentationColor);
        if (lutKey != impl_->ocioLutKey) {
            OpenColorIoLutPair luts;
            std::string error;
            if (!BuildOpenColorIoLuts(presentationColor, luts, error)) {
                std::fprintf(stderr,
                             "Unable to prepare OpenColorIO Metal LUTs: %s\n",
                             error.c_str());
                return false;
            }
            id<MTLTexture> inputLut =
                UploadOpenColorIoLut(impl_->device, luts.input_to_working);
            id<MTLTexture> outputLut =
                UploadOpenColorIoLut(impl_->device, luts.working_to_display);
            if (!inputLut || !outputLut) {
                std::fprintf(stderr,
                             "Unable to allocate OpenColorIO Metal LUTs\n");
                return false;
            }
            impl_->ocioInputLut = inputLut;
            impl_->ocioOutputLut = outputLut;
            impl_->ocioLutKey = lutKey;
        }
    }
    const bool hlgOutput =
        presentationColor.enabled && presentationColor.output_transfer == "hlg";
    impl_->layer.colorspace =
        hlgOutput ? impl_->hlgColorSpace : impl_->sdrColorSpace;
    impl_->layer.wantsExtendedDynamicRangeContent = hlgOutput;
    // The output pass already writes HLG-encoded values and the layer's HLG
    // color space carries that transfer function. CAEDRMetadata belongs to
    // the separate linear-EDR presentation path; attaching it here makes the
    // system tone-map an encoded signal again and burns the monitor image.
    impl_->layer.EDRMetadata = nil;

    id<CAMetalDrawable> drawable = [impl_->layer nextDrawable];
    if (!drawable) {
        std::fprintf(stderr, "CAMetalLayer returned no drawable\n");
        return false;
    }

    id<MTLCommandBuffer> commandBuffer = [impl_->queue commandBuffer];
    if (!impl_->workingTexture ||
        impl_->workingTexture.width != drawable.texture.width ||
        impl_->workingTexture.height != drawable.texture.height) {
        MTLTextureDescriptor* workingDescriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                         width:drawable.texture.width
                                        height:drawable.texture.height
                                     mipmapped:NO];
        workingDescriptor.storageMode = MTLStorageModePrivate;
        workingDescriptor.usage =
            MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        impl_->workingTexture =
            [impl_->device newTextureWithDescriptor:workingDescriptor];
        if (!impl_->workingTexture) {
            std::fprintf(stderr, "Unable to allocate ACES working texture\n");
            return false;
        }
    }

    MTLRenderPassDescriptor* workingPass =
        [MTLRenderPassDescriptor renderPassDescriptor];
    workingPass.colorAttachments[0].texture = impl_->workingTexture;
    workingPass.colorAttachments[0].loadAction = MTLLoadActionClear;
    workingPass.colorAttachments[0].storeAction = MTLStoreActionStore;
    workingPass.colorAttachments[0].clearColor =
        MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    id<MTLRenderCommandEncoder> workingEncoder =
        [commandBuffer renderCommandEncoderWithDescriptor:workingPass];

    const double scale = impl_->layer.contentsScale;
    const double videoHeight =
        std::clamp(timeline.video_height * scale, 0.0,
                   static_cast<double>(drawable.texture.height));
    if (videoHeight > 0.0) {
        [workingEncoder
            setViewport:MTLViewport{0.0, 0.0,
                                    static_cast<double>(drawable.texture.width),
                                    videoHeight, 0.0, 1.0}];
        [workingEncoder setRenderPipelineState:impl_->workingPipeline];
        struct PresentationParameters {
            float left;
            float top;
            float width;
            float height;
            int32_t quarterTurns;
            float opacity;
            int32_t colorManagementEnabled;
            int32_t inputGamut;
            int32_t inputTransfer;
            int32_t useAcescct;
            int32_t inputIsRgb;
            int32_t hasSourceAlpha;
            float redFromCr;
            float greenFromCb;
            float greenFromCr;
            float blueFromCb;
            float sampleScale;
            float yOffset;
            float yScale;
            float chromaOffset;
            float chromaScale;
        } parameters = {};
        const auto gamut = [](const std::string& value) -> int32_t {
            if (value == "sony_sgamut3_cine") return 1;
            if (value == "sony_sgamut3") return 2;
            if (value == "rec2020") return 3;
            return 0;
        };
        parameters.colorManagementEnabled = presentationColor.enabled ? 1 : 0;
        parameters.inputGamut = gamut(presentationColor.input_gamut);
        parameters.inputTransfer =
            presentationColor.input_transfer == "sony_slog3"
                ? 1
                : (presentationColor.input_transfer == "linear" ? 2 : 0);
        parameters.useAcescct =
            presentationColor.working_gamut == "acescct" ? 1 : 0;
        // Mirrors shader.metal's ClipGradeParameters/GradeEntry by hand (see
        // that file's comment on why this project keeps such structs
        // hand-synced rather than shared). Renderer.mm never interprets a
        // grade entry's `kind`; it only repacks whatever ColorEffects.h's
        // ResolveColorGrade already resolved per clip.
        struct GradeEntry {
            int32_t kind;
            float amount;
        };
        struct ClipGradeParameters {
            int32_t count;
            int32_t padding[3];
            GradeEntry entries[kMaxColorEffectsPerClip];
        };
        for (size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex) {
            const AVFrame* frame = frames[frameIndex];
            if (!frame) continue;
            const AVPixFmtDescriptor* pixel =
                av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
            const int depth = pixel->comp[0].depth;
            parameters.inputIsRgb =
                (pixel->flags & AV_PIX_FMT_FLAG_RGB) != 0 ? 1 : 0;
            parameters.hasSourceAlpha =
                pixel->nb_components >= 4 &&
                        (pixel->flags & AV_PIX_FMT_FLAG_ALPHA)
                    ? 1
                    : 0;
            bool fullRange = presentationColor.input_range == "full";
            if (presentationColor.input_range == "auto") {
                fullRange = frame->color_range == AVCOL_RANGE_JPEG ||
                            (frame->color_range == AVCOL_RANGE_UNSPECIFIED &&
                             parameters.inputTransfer == 1);
            }
            const YuvCodeParameters code =
                BuildYuvCodeParameters(depth, fullRange);
            parameters.sampleScale = code.sample_scale;
            parameters.yOffset = code.y_offset;
            parameters.yScale = code.y_scale;
            parameters.chromaOffset = code.chroma_offset;
            parameters.chromaScale = code.chroma_scale;
            const bool bt2020Matrix =
                presentationColor.input_ycbcr_matrix == "auto"
                    ? frame->colorspace == AVCOL_SPC_BT2020_NCL
                    : presentationColor.input_ycbcr_matrix == "bt2020_ncl";
            const YuvMatrixParameters matrix =
                BuildYuvMatrixParameters(bt2020Matrix);
            parameters.redFromCr = matrix.red_from_cr;
            parameters.greenFromCb = matrix.green_from_cb;
            parameters.greenFromCr = matrix.green_from_cr;
            parameters.blueFromCb = matrix.blue_from_cb;
            for (NSUInteger component = 0; component < 3; ++component)
                [workingEncoder
                    setFragmentTexture:
                        impl_->planes[frameIndex][pixel->comp[component].plane]
                               atIndex:component];
            id<MTLTexture> alphaTexture =
                parameters.hasSourceAlpha
                    ? impl_->planes[frameIndex][pixel->comp[3].plane]
                    : nil;
            [workingEncoder setFragmentTexture:alphaTexture atIndex:3];
            if (presentationColor.enabled)
                [workingEncoder setFragmentTexture:impl_->ocioInputLut
                                           atIndex:4];
            const int32_t degrees =
                frameIndex < timeline.video_rotation_degrees.size()
                    ? timeline.video_rotation_degrees[frameIndex]
                    : 0;
            const int32_t turns =
                static_cast<int32_t>(std::lround(degrees / 90.0));
            parameters.quarterTurns = ((turns % 4) + 4) % 4;
            const double displayedWidth =
                parameters.quarterTurns % 2 ? frame->height : frame->width;
            const double displayedHeight =
                parameters.quarterTurns % 2 ? frame->width : frame->height;
            const double viewportAspect =
                static_cast<double>(drawable.texture.width) / videoHeight;
            const double sequenceAspect =
                timeline.sequence_width > 0 && timeline.sequence_height > 0
                    ? static_cast<double>(timeline.sequence_width) /
                          timeline.sequence_height
                    : viewportAspect;
            double canvasWidth = drawable.texture.width;
            double canvasHeight = videoHeight;
            if (sequenceAspect > viewportAspect) {
                canvasHeight = canvasWidth / sequenceAspect;
            } else {
                canvasWidth = canvasHeight * sequenceAspect;
            }
            double contentWidth = 0.0;
            double contentHeight = 0.0;
            if (timeline.video_zoom > 0.0) {
                contentWidth = displayedWidth * timeline.video_zoom;
                contentHeight = displayedHeight * timeline.video_zoom;
            } else {
                // Kdenlive-style free resize: derive both dimensions from one
                // scale so changing only the viewer width can never stretch
                // or mirror the video content.
                const double fitScale =
                    std::min(canvasWidth / displayedWidth,
                             canvasHeight / displayedHeight);
                contentWidth = displayedWidth * fitScale;
                contentHeight = displayedHeight * fitScale;
            }
            parameters.width = static_cast<float>(
                contentWidth / static_cast<double>(drawable.texture.width));
            parameters.height = static_cast<float>(contentHeight / videoHeight);
            parameters.left = (1.0f - parameters.width) * 0.5f;
            parameters.top = (1.0f - parameters.height) * 0.5f;
            parameters.opacity =
                frameIndex < timeline.video_opacities.size()
                    ? std::clamp(timeline.video_opacities[frameIndex], 0.0f,
                                 1.0f)
                    : 1.0f;
            [workingEncoder setFragmentBytes:&parameters
                                      length:sizeof(parameters)
                                     atIndex:0];
            ClipGradeParameters gradeParameters = {};
            if (frameIndex < timeline.video_color_grades.size()) {
                const ResolvedColorGrade& grade =
                    timeline.video_color_grades[frameIndex];
                gradeParameters.count = grade.count;
                for (int32_t entryIndex = 0; entryIndex < grade.count;
                     ++entryIndex) {
                    gradeParameters.entries[entryIndex].kind =
                        grade.entries[entryIndex].kind;
                    gradeParameters.entries[entryIndex].amount =
                        grade.entries[entryIndex].amount;
                }
            }
            [workingEncoder setFragmentBytes:&gradeParameters
                                      length:sizeof(gradeParameters)
                                     atIndex:1];
            [workingEncoder setFragmentSamplerState:impl_->sampler atIndex:0];
            [workingEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                               vertexStart:0
                               vertexCount:6];
        }
    }
    [workingEncoder
        setViewport:MTLViewport{0.0, 0.0,
                                static_cast<double>(drawable.texture.width),
                                static_cast<double>(drawable.texture.height),
                                0.0, 1.0}];
    struct SolidParameters {
        float rect[4];
        float topColor[4];
        float bottomColor[4];
        float drawableSize[2];
        float padding[2];
        int32_t colorManagementEnabled;
        int32_t outputTransfer;
        int32_t colorPadding[2];
    } solid = {};
    solid.colorManagementEnabled = presentationColor.enabled ? 1 : 0;
    solid.outputTransfer = hlgOutput ? 1 : 0;
    solid.drawableSize[0] = static_cast<float>(drawable.texture.width);
    solid.drawableSize[1] = static_cast<float>(drawable.texture.height);

    struct TextParameters {
        float rect[4];
        float uv[4];
        float color[4];
        float drawableSize[2];
        float padding[2];
        int32_t colorManagementEnabled;
        int32_t outputTransfer;
        int32_t colorPadding[2];
    } text = {};
    text.drawableSize[0] = static_cast<float>(drawable.texture.width);
    text.drawableSize[1] = static_cast<float>(drawable.texture.height);
    text.colorManagementEnabled = presentationColor.enabled ? 1 : 0;
    text.outputTransfer = hlgOutput ? 1 : 0;

    const auto drawRect = [&](const MetalRect& item) {
        double left = item.x;
        double itemTop = item.y;
        double width = item.width;
        double height = item.height;
        if (width < 0.0) {
            left += width;
            width = -width;
        }
        if (height < 0.0) {
            itemTop += height;
            height = -height;
        }
        if (width <= 0.0 || height <= 0.0) return;
        solid.rect[0] = static_cast<float>(left * scale);
        solid.rect[1] = static_cast<float>(itemTop * scale);
        solid.rect[2] = static_cast<float>(width * scale);
        solid.rect[3] = static_cast<float>(height * scale);
        solid.topColor[0] = item.red;
        solid.topColor[1] = item.green;
        solid.topColor[2] = item.blue;
        solid.topColor[3] = item.alpha;
        solid.bottomColor[0] = item.bottom_red;
        solid.bottomColor[1] = item.bottom_green;
        solid.bottomColor[2] = item.bottom_blue;
        solid.bottomColor[3] = item.bottom_alpha;
        [workingEncoder setRenderPipelineState:impl_->solidPipeline];
        [workingEncoder setVertexBytes:&solid length:sizeof(solid) atIndex:0];
        [workingEncoder setFragmentBytes:&solid length:sizeof(solid) atIndex:0];
        [workingEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                           vertexStart:0
                           vertexCount:6];
    };
    const auto drawAtlasQuad = [&](const MetalAtlasQuad& quad) {
        text.rect[0] = static_cast<float>(quad.x * scale);
        text.rect[1] = static_cast<float>(quad.y * scale);
        text.rect[2] = static_cast<float>(quad.width * scale);
        text.rect[3] = static_cast<float>(quad.height * scale);
        text.uv[0] = quad.u0;
        text.uv[1] = quad.v0;
        text.uv[2] = quad.u1;
        text.uv[3] = quad.v1;
        text.color[0] = quad.color.r;
        text.color[1] = quad.color.g;
        text.color[2] = quad.color.b;
        text.color[3] = quad.color.a;
        [workingEncoder setRenderPipelineState:impl_->textPipeline];
        [workingEncoder setVertexBytes:&text length:sizeof(text) atIndex:0];
        [workingEncoder setFragmentBytes:&text length:sizeof(text) atIndex:0];
        [workingEncoder setFragmentTexture:impl_->uiAtlas.texture() atIndex:0];
        [workingEncoder setFragmentSamplerState:impl_->sampler atIndex:0];
        [workingEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                           vertexStart:0
                           vertexCount:6];
    };

    std::vector<MetalAtlasQuad> textQuads;
    for (const MetalDrawCommand& command : timeline.overlays) {
        if (const MetalRect* rectangle = std::get_if<MetalRect>(&command)) {
            drawRect(*rectangle);
        } else if (const MetalText* textItem =
                       std::get_if<MetalText>(&command)) {
            textQuads.clear();
            impl_->uiAtlas.AppendTextQuads(*textItem, textQuads);
            for (const MetalAtlasQuad& quad : textQuads) drawAtlasQuad(quad);
        } else if (const MetalIcon* icon = std::get_if<MetalIcon>(&command)) {
            MetalAtlasQuad quad;
            if (impl_->uiAtlas.IconQuad(*icon, quad)) drawAtlasQuad(quad);
        }
    }
    [workingEncoder endEncoding];

    const size_t scopeBins = VideoScopeHistogramBinCount(timeline.video_scope);
    if (!impl_->scopeHistogram || impl_->scopeHistogramBins < scopeBins) {
        impl_->scopeHistogram =
            [impl_->device newBufferWithLength:scopeBins * sizeof(uint32_t)
                                       options:MTLResourceStorageModePrivate];
        impl_->scopeHistogramBins = scopeBins;
    }
    if (!impl_->scopeHistogram) {
        std::fprintf(stderr, "Unable to allocate video scope histogram\n");
        return false;
    }

    struct OutputParameters {
        int32_t colorManagementEnabled;
        int32_t outputGamut;
        int32_t outputTransfer;
        int32_t scopeMode;
        uint32_t drawableWidth;
        uint32_t drawableHeight;
        uint32_t scopeSampleStep;
        uint32_t padding;
    } outputParameters = {
        presentationColor.enabled ? 1 : 0,
        presentationColor.output_gamut == "rec2020" ? 1 : 0,
        hlgOutput ? 1 : 0,
        static_cast<int32_t>(timeline.video_scope),
        static_cast<uint32_t>(drawable.texture.width),
        static_cast<uint32_t>(drawable.texture.height),
        2,
        presentationColor.working_gamut == "acescct" ? 1u : 0u,
    };

    id<MTLBlitCommandEncoder> scopeClear = [commandBuffer blitCommandEncoder];
    [scopeClear fillBuffer:impl_->scopeHistogram
                     range:NSMakeRange(0, scopeBins * sizeof(uint32_t))
                     value:0];
    [scopeClear endEncoding];
    if (timeline.video_scope != VideoScopeMode::Off) {
        id<MTLComputeCommandEncoder> scopeEncoder =
            [commandBuffer computeCommandEncoder];
        [scopeEncoder setComputePipelineState:impl_->scopePipeline];
        [scopeEncoder setTexture:impl_->workingTexture atIndex:0];
        if (presentationColor.enabled)
            [scopeEncoder setTexture:impl_->ocioOutputLut atIndex:1];
        [scopeEncoder setSamplerState:impl_->sampler atIndex:0];
        [scopeEncoder setBuffer:impl_->scopeHistogram offset:0 atIndex:0];
        [scopeEncoder setBytes:&outputParameters
                        length:sizeof(outputParameters)
                       atIndex:1];
        const NSUInteger threadWidth =
            impl_->scopePipeline.threadExecutionWidth;
        const NSUInteger threadHeight = std::max<NSUInteger>(
            1,
            impl_->scopePipeline.maxTotalThreadsPerThreadgroup / threadWidth);
        const MTLSize threads = MTLSizeMake(threadWidth, threadHeight, 1);
        const MTLSize grid = MTLSizeMake(
            (drawable.texture.width + outputParameters.scopeSampleStep - 1) /
                outputParameters.scopeSampleStep,
            (drawable.texture.height + outputParameters.scopeSampleStep - 1) /
                outputParameters.scopeSampleStep,
            1);
        [scopeEncoder dispatchThreads:grid threadsPerThreadgroup:threads];
        [scopeEncoder endEncoding];
    }

    MTLRenderPassDescriptor* pass =
        [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder
        setViewport:MTLViewport{0.0, 0.0,
                                static_cast<double>(drawable.texture.width),
                                static_cast<double>(drawable.texture.height),
                                0.0, 1.0}];
    [encoder setRenderPipelineState:impl_->outputPipeline];
    [encoder setFragmentTexture:impl_->workingTexture atIndex:0];
    if (presentationColor.enabled)
        [encoder setFragmentTexture:impl_->ocioOutputLut atIndex:1];
    [encoder setFragmentSamplerState:impl_->sampler atIndex:0];
    [encoder setFragmentBuffer:impl_->scopeHistogram offset:0 atIndex:1];
    [encoder setFragmentBytes:&outputParameters
                       length:sizeof(outputParameters)
                      atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:6];

    [encoder endEncoding];
    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];

    // Keep the synchronous path while the cache/renderer boundary is measured.
    [commandBuffer waitUntilCompleted];
    if (commandBuffer.status == MTLCommandBufferStatusError) {
        std::fprintf(stderr, "Metal command failed: %s\n",
                     commandBuffer.error.localizedDescription.UTF8String);
        return false;
    }
    return true;
}
