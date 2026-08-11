#include "Renderer.h"

#include "ColorManagement.h"

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
    id<MTLRenderPipelineState> solidPipeline = nil;
    id<MTLSamplerState> sampler = nil;
    id<MTLTexture> workingTexture = nil;
    std::vector<std::array<id<MTLTexture>, 3>> planes;
    std::vector<std::array<int, 3>> textureWidths;
    std::vector<std::array<int, 3>> textureHeights;
    std::vector<int> textureFormats;
    CGColorSpaceRef sdrColorSpace = nullptr;
    CGColorSpaceRef hlgColorSpace = nullptr;

    ~Impl() {
        if (sdrColorSpace) CGColorSpaceRelease(sdrColorSpace);
        if (hlgColorSpace) CGColorSpaceRelease(hlgColorSpace);
    }
};

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

    MTLSamplerDescriptor* samplerDescriptor = [MTLSamplerDescriptor new];
    samplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
    samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
    samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    impl_->sampler =
        [impl_->device newSamplerStateWithDescriptor:samplerDescriptor];
    return impl_->sampler != nil;
}

void Renderer::Resize(NSRect bounds) {
    if (!impl_->layer) {
        return;
    }
    const CGFloat scale = impl_->layer.contentsScale;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    impl_->layer.frame = bounds;
    impl_->layer.drawableSize =
        CGSizeMake(bounds.size.width * scale, bounds.size.height * scale);
    [CATransaction commit];
}

bool Renderer::RenderFrames(const std::vector<AVFrame*>& frames,
                            const TimelineRenderData& timeline) {
    if (!impl_->workingPipeline || !impl_->outputPipeline ||
        !impl_->solidPipeline || !impl_->queue) {
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
            (pixel->flags &
             (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL |
              AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_HWACCEL)) ||
            pixel->comp[0].plane != 0 || pixel->comp[1].plane != 1 ||
            pixel->comp[2].plane != 2 ||
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
        std::array<int, 3> planeWidths = {
            frame->width, AV_CEIL_RSHIFT(frame->width, pixel->log2_chroma_w),
            AV_CEIL_RSHIFT(frame->width, pixel->log2_chroma_w)};
        std::array<int, 3> planeHeights = {
            frame->height, AV_CEIL_RSHIFT(frame->height, pixel->log2_chroma_h),
            AV_CEIL_RSHIFT(frame->height, pixel->log2_chroma_h)};
        if (impl_->textureWidths[frameIndex] != planeWidths ||
            impl_->textureHeights[frameIndex] != planeHeights ||
            impl_->textureFormats[frameIndex] != frame->format) {
            for (int plane = 0; plane < 3; ++plane) {
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

        for (int plane = 0; plane < 3; ++plane) {
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

    const bool hlgOutput = timeline.color_management.enabled &&
                           timeline.color_management.output_transfer == "hlg";
    impl_->layer.colorspace =
        hlgOutput ? impl_->hlgColorSpace : impl_->sdrColorSpace;
    impl_->layer.wantsExtendedDynamicRangeContent = hlgOutput;
    impl_->layer.EDRMetadata = hlgOutput ? CAEDRMetadata.HLGMetadata : nil;

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
            float redFromCr;
            float greenFromCb;
            float greenFromCr;
            float blueFromCb;
            float sampleScale;
            float yOffset;
            float yScale;
            float chromaOffset;
            float chromaScale;
            float padding[2];
        } parameters = {};
        const auto gamut = [](const std::string& value) -> int32_t {
            if (value == "sony_sgamut3_cine") return 1;
            if (value == "sony_sgamut3") return 2;
            if (value == "rec2020") return 3;
            return 0;
        };
        parameters.colorManagementEnabled =
            timeline.color_management.enabled ? 1 : 0;
        parameters.inputGamut = gamut(timeline.color_management.input_gamut);
        parameters.inputTransfer =
            timeline.color_management.input_transfer == "sony_slog3"
                ? 1
                : (timeline.color_management.input_transfer == "linear" ? 2
                                                                        : 0);
        parameters.useAcescct =
            timeline.color_management.working_gamut == "acescct" ? 1 : 0;
        for (size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex) {
            const AVFrame* frame = frames[frameIndex];
            if (!frame) continue;
            const AVPixFmtDescriptor* pixel =
                av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
            const int depth = pixel->comp[0].depth;
            bool fullRange = timeline.color_management.input_range == "full";
            if (timeline.color_management.input_range == "auto") {
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
                timeline.color_management.input_ycbcr_matrix == "auto"
                    ? frame->colorspace == AVCOL_SPC_BT2020_NCL
                    : timeline.color_management.input_ycbcr_matrix ==
                          "bt2020_ncl";
            const YuvMatrixParameters matrix =
                BuildYuvMatrixParameters(bt2020Matrix);
            parameters.redFromCr = matrix.red_from_cr;
            parameters.greenFromCb = matrix.green_from_cb;
            parameters.greenFromCr = matrix.green_from_cr;
            parameters.blueFromCb = matrix.blue_from_cb;
            for (NSUInteger plane = 0; plane < 3; ++plane)
                [workingEncoder
                    setFragmentTexture:impl_->planes[frameIndex][plane]
                               atIndex:plane];
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
    [workingEncoder setRenderPipelineState:impl_->solidPipeline];
    struct SolidParameters {
        float rect[4];
        float color[4];
        float drawableSize[2];
        float padding[2];
        int32_t colorManagementEnabled;
        int32_t outputTransfer;
        int32_t colorPadding[2];
    } solid = {};
    solid.colorManagementEnabled = timeline.color_management.enabled ? 1 : 0;
    solid.outputTransfer = hlgOutput ? 1 : 0;
    for (const MetalRect& item : timeline.rectangles) {
        double left = item.x;
        double top = item.y;
        double width = item.width;
        double height = item.height;
        if (width < 0.0) {
            left += width;
            width = -width;
        }
        if (height < 0.0) {
            top += height;
            height = -height;
        }
        if (width <= 0.0 || height <= 0.0) continue;
        solid.rect[0] = static_cast<float>(left * scale);
        solid.rect[1] = static_cast<float>(top * scale);
        solid.rect[2] = static_cast<float>(width * scale);
        solid.rect[3] = static_cast<float>(height * scale);
        solid.color[0] = item.red;
        solid.color[1] = item.green;
        solid.color[2] = item.blue;
        solid.color[3] = item.alpha;
        solid.drawableSize[0] = static_cast<float>(drawable.texture.width);
        solid.drawableSize[1] = static_cast<float>(drawable.texture.height);
        [workingEncoder setVertexBytes:&solid length:sizeof(solid) atIndex:0];
        [workingEncoder setFragmentBytes:&solid length:sizeof(solid) atIndex:0];
        [workingEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                           vertexStart:0
                           vertexCount:6];
    }
    [workingEncoder endEncoding];

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
    struct OutputParameters {
        int32_t colorManagementEnabled;
        int32_t outputGamut;
        int32_t outputTransfer;
        int32_t padding;
    } outputParameters = {
        timeline.color_management.enabled ? 1 : 0,
        timeline.color_management.output_gamut == "rec2020" ? 1 : 0,
        hlgOutput ? 1 : 0, 0};
    [encoder setFragmentTexture:impl_->workingTexture atIndex:0];
    [encoder setFragmentSamplerState:impl_->sampler atIndex:0];
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
