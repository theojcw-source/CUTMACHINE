#include "Renderer.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <array>
#include <cstdio>

struct Renderer::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    CAMetalLayer* layer = nil;
    id<MTLRenderPipelineState> pipeline = nil;
    id<MTLRenderPipelineState> solidPipeline = nil;
    id<MTLSamplerState> sampler = nil;
    std::vector<std::array<id<MTLTexture>, 3>> planes;
    std::vector<int> textureWidths;
    std::vector<int> textureHeights;
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
    impl_->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    impl_->layer.framebufferOnly = YES;
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
    id<MTLFunction> fragment = [library newFunctionWithName:@"fragment_main"];
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = vertex;
    descriptor.fragmentFunction = fragment;
    descriptor.colorAttachments[0].pixelFormat = impl_->layer.pixelFormat;
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
    impl_->pipeline =
        [impl_->device newRenderPipelineStateWithDescriptor:descriptor
                                                      error:&error];
    if (!impl_->pipeline) {
        std::fprintf(stderr, "Unable to create Metal pipeline: %s\n",
                     error.localizedDescription.UTF8String);
        return false;
    }

    descriptor.vertexFunction = [library newFunctionWithName:@"vertex_solid"];
    descriptor.fragmentFunction =
        [library newFunctionWithName:@"fragment_solid"];
    descriptor.colorAttachments[0].blendingEnabled = YES;
    descriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
    descriptor.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
    descriptor.colorAttachments[0].sourceRGBBlendFactor =
        MTLBlendFactorSourceAlpha;
    descriptor.colorAttachments[0].sourceAlphaBlendFactor =
        MTLBlendFactorSourceAlpha;
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
    impl_->layer.frame = bounds;
    impl_->layer.drawableSize =
        CGSizeMake(bounds.size.width * scale, bounds.size.height * scale);
}

bool Renderer::RenderFrames(const std::vector<AVFrame*>& frames,
                            const TimelineRenderData& timeline) {
    if (!impl_->pipeline || !impl_->solidPipeline || !impl_->queue) {
        return false;
    }

    if (impl_->planes.size() < frames.size()) {
        impl_->planes.resize(frames.size());
        impl_->textureWidths.resize(frames.size(), 0);
        impl_->textureHeights.resize(frames.size(), 0);
    }
    for (size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex) {
        const AVFrame* frame = frames[frameIndex];
        if (!frame) continue;
        if (frame->width <= 0 || frame->height <= 0 || !frame->data[0] ||
            !frame->data[1] || !frame->data[2]) {
            std::fprintf(stderr, "Invalid planar AVFrame passed to renderer\n");
            return false;
        }
        if (impl_->textureWidths[frameIndex] != frame->width ||
            impl_->textureHeights[frameIndex] != frame->height) {
            for (int plane = 0; plane < 3; ++plane) {
                const int planeWidth =
                    plane == 0 ? frame->width : frame->width / 2;
                MTLTextureDescriptor* textureDescriptor = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Unorm
                                                 width:planeWidth
                                                height:frame->height
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
            impl_->textureWidths[frameIndex] = frame->width;
            impl_->textureHeights[frameIndex] = frame->height;
        }

        for (int plane = 0; plane < 3; ++plane) {
            const int planeWidth = plane == 0 ? frame->width : frame->width / 2;
            if (frame->linesize[plane] <= 0) {
                std::fprintf(stderr,
                             "Unsupported negative/zero linesize for layer %zu "
                             "plane %d: %d\n",
                             frameIndex, plane, frame->linesize[plane]);
                return false;
            }
            const MTLRegion region =
                MTLRegionMake2D(0, 0, planeWidth, frame->height);
            [impl_->planes[frameIndex][plane]
                replaceRegion:region
                  mipmapLevel:0
                    withBytes:frame->data[plane]
                  bytesPerRow:static_cast<NSUInteger>(frame->linesize[plane])];
        }
    }

    id<CAMetalDrawable> drawable = [impl_->layer nextDrawable];
    if (!drawable) {
        std::fprintf(stderr, "CAMetalLayer returned no drawable\n");
        return false;
    }

    id<MTLCommandBuffer> commandBuffer = [impl_->queue commandBuffer];
    MTLRenderPassDescriptor* pass =
        [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    const double scale = impl_->layer.contentsScale;
    const double videoHeight =
        std::clamp(timeline.video_height * scale, 0.0,
                   static_cast<double>(drawable.texture.height));
    if (videoHeight > 0.0) {
        [encoder
            setViewport:MTLViewport{0.0, 0.0,
                                    static_cast<double>(drawable.texture.width),
                                    videoHeight, 0.0, 1.0}];
        [encoder setRenderPipelineState:impl_->pipeline];
        struct PresentationParameters {
            float left;
            float top;
            float width;
            float height;
            int32_t quarterTurns;
            float opacity;
        } parameters = {};
        for (size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex) {
            const AVFrame* frame = frames[frameIndex];
            if (!frame) continue;
            for (NSUInteger plane = 0; plane < 3; ++plane)
                [encoder setFragmentTexture:impl_->planes[frameIndex][plane]
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
            const double contentAspect = displayedWidth / displayedHeight;
            const double viewportAspect =
                static_cast<double>(drawable.texture.width) / videoHeight;
            parameters.left = parameters.top = 0.0f;
            parameters.width = parameters.height = 1.0f;
            if (contentAspect > viewportAspect) {
                parameters.height =
                    static_cast<float>(viewportAspect / contentAspect);
                parameters.top = (1.0f - parameters.height) * 0.5f;
            } else {
                parameters.width =
                    static_cast<float>(contentAspect / viewportAspect);
                parameters.left = (1.0f - parameters.width) * 0.5f;
            }
            parameters.opacity = 1.0f;
            [encoder setFragmentBytes:&parameters
                               length:sizeof(parameters)
                              atIndex:0];
            [encoder setFragmentSamplerState:impl_->sampler atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:0
                        vertexCount:6];
        }
    }

    [encoder
        setViewport:MTLViewport{0.0, 0.0,
                                static_cast<double>(drawable.texture.width),
                                static_cast<double>(drawable.texture.height),
                                0.0, 1.0}];
    [encoder setRenderPipelineState:impl_->solidPipeline];
    struct SolidParameters {
        float rect[4];
        float color[4];
        float drawableSize[2];
        float padding[2];
    } solid;
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
        solid.padding[0] = solid.padding[1] = 0.0f;
        [encoder setVertexBytes:&solid length:sizeof(solid) atIndex:0];
        [encoder setFragmentBytes:&solid length:sizeof(solid) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:0
                    vertexCount:6];
    }
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
