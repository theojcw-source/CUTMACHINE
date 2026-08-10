#include "Renderer.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

extern "C" {
#include <libavutil/frame.h>
}

#include <array>
#include <cstdio>

struct Renderer::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    CAMetalLayer* layer = nil;
    id<MTLRenderPipelineState> pipeline = nil;
    id<MTLSamplerState> sampler = nil;
    std::array<std::array<id<MTLTexture>, 3>, 2> planes = {};
    std::array<int, 2> textureWidths = {0, 0};
    std::array<int, 2> textureHeights = {0, 0};
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
    impl_->pipeline =
        [impl_->device newRenderPipelineStateWithDescriptor:descriptor
                                                      error:&error];
    if (!impl_->pipeline) {
        std::fprintf(stderr, "Unable to create Metal pipeline: %s\n",
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

bool Renderer::RenderFrames(const AVFrame* firstFrame,
                            const AVFrame* secondFrame, float secondOpacity) {
    if (!impl_->pipeline || !impl_->queue) {
        return false;
    }

    // A hole on the first video track must not prevent another populated
    // track from being shown. If both tracks are holes, the clear pass below
    // produces black and does not bind the sampling pipeline.
    if (!firstFrame && secondFrame) {
        firstFrame = secondFrame;
        secondFrame = nullptr;
    }

    const AVFrame* frames[2] = {firstFrame, secondFrame};
    const int frameCount = secondFrame ? 2 : 1;
    for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        const AVFrame* frame = frames[frameIndex];
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
                    std::fprintf(stderr,
                                 "Unable to allocate Metal frame %d plane %d\n",
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
                             "Unsupported negative/zero linesize for frame %d "
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
    if (firstFrame) {
        [encoder setRenderPipelineState:impl_->pipeline];
        for (NSUInteger plane = 0; plane < 3; ++plane) {
            [encoder setFragmentTexture:impl_->planes[0][plane] atIndex:plane];
            id<MTLTexture> secondTexture =
                secondFrame ? impl_->planes[1][plane] : impl_->planes[0][plane];
            [encoder setFragmentTexture:secondTexture atIndex:(plane + 3)];
        }
        struct CompositeParameters {
            float secondOpacity;
            uint32_t hasSecondFrame;
        } parameters = {secondOpacity, secondFrame ? 1u : 0u};
        [encoder setFragmentBytes:&parameters
                           length:sizeof(parameters)
                          atIndex:0];
        [encoder setFragmentSamplerState:impl_->sampler atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:0
                    vertexCount:6];
    }
    [encoder endEncoding];
    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];

    // The spike deliberately has one shared texture set and a synchronous path.
    [commandBuffer waitUntilCompleted];
    if (commandBuffer.status == MTLCommandBufferStatusError) {
        std::fprintf(stderr, "Metal command failed: %s\n",
                     commandBuffer.error.localizedDescription.UTF8String);
        return false;
    }
    return true;
}
