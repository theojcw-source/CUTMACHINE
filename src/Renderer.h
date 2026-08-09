#pragma once

#import <AppKit/AppKit.h>

struct AVFrame;

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool Initialize(NSView* view);
    void Resize(NSRect bounds);
    bool RenderFrames(const AVFrame* firstFrame, const AVFrame* secondFrame,
                      float secondOpacity = 0.5f);

private:
    struct Impl;
    Impl* impl_;
};
