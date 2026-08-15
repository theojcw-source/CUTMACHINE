#pragma once

// AppKit-only. This sandbox has no Xcode/AppKit toolchain, so this file and
// ChatPanelView.mm are unverified beyond a manual read -- see the F2.1
// report (UiComponents.h) for exactly what that means, and this ticket's
// own report for what *is* verified (ChatLlmClient.h/ChatSession.h/
// McpLiveBackend.h, all plain C++).
//
// The chat panel (ROADMAP.md F2.4): a message transcript plus a text input
// and send button, installed into PanelLayout.h's PanelSlot::Chat via
// CMPanelHostView -setContentView:forSlot:, styled from UiTheme.h/
// UiComponents.h like every other F2.x panel.
//
// This view owns no document-mutation logic of its own. Every edit an
// instruction causes is dispatched by ChatSession through
// McpToolRegistry::Call -- the exact function McpServer.cc's `tools/call`
// JSON-RPC handler already uses -- against whatever McpBackend
// -configureWithBackend: was given. This view's only responsibilities are
// rendering the transcript, reading the input field, and getting each turn
// off the main thread (network I/O) while still running the resulting
// document mutation back on it (see ChatPanelView.mm's MainThreadBackend).

#import <AppKit/AppKit.h>

#include "McpBackend.h"

@interface CMChatPanelView : NSView

- (instancetype)initWithFrame:(NSRect)frame;

// Wires the panel to a live document. `backend` is not owned and must
// outlive this view -- main.mm constructs an McpLiveBackend
// (McpLiveBackend.h) over `self.state->document`/`self.state->editLog` and
// passes it here once, right where the chat view is installed into the
// right dock. `self.state` is allocated once before the window is built and
// never reallocated afterward (only its fields are mutated by ordinary
// edits and project loads), so that reference stays valid for the window's
// whole lifetime. Call once, before the view is shown.
- (void)configureWithBackend:(McpBackend&)backend;

@end
