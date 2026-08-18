#import "ChatPanelView.h"

#include "ChatLlmClient.h"
#include "ChatSession.h"
#include "Document.h"
#include "McpTools.h"
#include "UiTheme.h"

#import "UiComponents.h"
#import "UiThemeAppKit.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------
// MainThreadBackend: wraps the live McpLiveBackend so every actual document
// read/mutation runs on the main thread, serialized with every mouse-driven
// edit and with rendering -- while the chat turn that triggers it (network
// I/O to the LLM, potentially several round trips) runs entirely on a
// background queue (see -sendPressed: below). ChatSession itself is
// thread-agnostic; this is the one seam that makes "run the slow part off
// the main thread, the document-touching part on it" true without
// ChatSession knowing anything about threads.
// ---------------------------------------------------------------------
class MainThreadBackend : public McpBackend {
public:
    explicit MainThreadBackend(McpBackend& live) : live_(live) {}

    bool SnapshotDocument(Document& document, std::string& message) override {
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok = live_.SnapshotDocument(document, message);
        });
        return ok;
    }

    bool ApplyOperation(Operation operation, std::string& resultJson,
                        std::string& errorName, std::string& message) override {
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok = live_.ApplyOperation(operation, resultJson, errorName, message);
        });
        return ok;
    }

    bool Undo(std::string& resultJson, std::string& errorName,
              std::string& message) override {
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok = live_.Undo(resultJson, errorName, message);
        });
        return ok;
    }

    bool Redo(std::string& resultJson, std::string& errorName,
              std::string& message) override {
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok = live_.Redo(resultJson, errorName, message);
        });
        return ok;
    }

    bool Describe(std::string& json, std::string& message) override {
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok = live_.Describe(json, message);
        });
        return ok;
    }

private:
    McpBackend& live_;
};

// The one real (non-test) ChatHttpTransport: a synchronous NSURLSession
// POST. Always called from the background queue -sendPressed: dispatches
// to, never the main thread -- blocking on the semaphore below is exactly
// the point, it is what makes ChatLlmClient::SendMessages a plain blocking
// call from ChatSession's point of view, matching sidecar/planner.py's own
// synchronous urllib-based _post_json.
bool AnthropicUrlSessionTransport(const chat::ChatHttpRequest& request,
                                  chat::ChatHttpResponse& response,
                                  std::string& error) {
    NSURL* url = [NSURL
        URLWithString:[NSString stringWithUTF8String:request.url.c_str()]];
    if (!url) {
        error = "invalid URL: " + request.url;
        return false;
    }
    NSMutableURLRequest* urlRequest = [NSMutableURLRequest requestWithURL:url];
    urlRequest.HTTPMethod = @"POST";
    for (const auto& header : request.headers) {
        [urlRequest setValue:[NSString
                                 stringWithUTF8String:header.second.c_str()]
            forHTTPHeaderField:[NSString
                                   stringWithUTF8String:header.first.c_str()]];
    }
    urlRequest.HTTPBody = [NSData dataWithBytes:request.body.data()
                                         length:request.body.size()];
    urlRequest.timeoutInterval = 120.0;

    __block bool receivedResponse = false;
    __block int statusCode = 0;
    __block std::string bodyText;
    __block std::string errorText;
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    NSURLSessionDataTask* task = [[NSURLSession sharedSession]
        dataTaskWithRequest:urlRequest
          completionHandler:^(NSData* data, NSURLResponse* urlResponse,
                              NSError* nsError) {
            if (nsError != nil) {
                errorText = nsError.localizedDescription.UTF8String
                                ?: "unknown network error";
            } else {
                receivedResponse = true;
                statusCode = static_cast<int>(
                    ((NSHTTPURLResponse*)urlResponse).statusCode);
                if (data.length > 0)
                    bodyText.assign(static_cast<const char*>(data.bytes),
                                    data.length);
            }
            dispatch_semaphore_signal(semaphore);
          }];
    [task resume];
    dispatch_semaphore_wait(semaphore, DISPATCH_TIME_FOREVER);

    if (!receivedResponse) {
        error = errorText;
        return false;
    }
    response.status_code = statusCode;
    response.body = bodyText;
    return true;
}

CGFloat CGF(double value) { return static_cast<CGFloat>(value); }

}  // namespace

// A trivial flipped NSView, so the transcript stacks top-down (message 1 at
// the top) the way a scrollable log naturally reads, matching NSScrollView's
// own top-down scroller convention. Declared up front since
// -[CMChatPanelView initWithFrame:] below instantiates one.
@interface CMFlippedView : NSView
@end

@implementation CMFlippedView
- (BOOL)isFlipped {
    return YES;
}
@end

// ---------------------------------------------------------------------
// One transcript line. Private to this file -- nothing outside
// ChatPanelView.mm needs to know how a row is drawn.
// ---------------------------------------------------------------------

typedef NS_ENUM(NSInteger, CMChatRowStyle) {
    CMChatRowStyleUser,
    CMChatRowStyleAssistant,
    CMChatRowStyleToolCall,
    CMChatRowStyleToolResultOk,
    CMChatRowStyleToolResultError,
    CMChatRowStyleError,
};

@interface CMChatRowView : NSView
- (instancetype)initWithText:(NSString*)text style:(CMChatRowStyle)style;
- (CGFloat)layoutForWidth:(CGFloat)width;  // returns the height it now needs
@end

@implementation CMChatRowView {
    NSView* _accentStrip;
    NSTextField* _textField;
    CMChatRowStyle _style;
}

- (instancetype)initWithText:(NSString*)text style:(CMChatRowStyle)style {
    if ((self = [super initWithFrame:NSZeroRect])) {
        _style = style;
        self.wantsLayer = YES;

        _accentStrip = [[NSView alloc] initWithFrame:NSZeroRect];
        _accentStrip.wantsLayer = YES;
        _accentStrip.layer.backgroundColor = [self accentColor].CGColor;
        [self addSubview:_accentStrip];

        _textField = [NSTextField wrappingLabelWithString:text ?: @""];
        _textField.font = CMFont(ui::theme::kFontSizeBody, NSFontWeightRegular);
        _textField.textColor = (style == CMChatRowStyleError ||
                                style == CMChatRowStyleToolResultError)
                                   ? CMThemeColor(ui::theme::kError)
                                   : CMTextPrimaryColor();
        _textField.backgroundColor = NSColor.clearColor;
        [self addSubview:_textField];
    }
    return self;
}

- (NSColor*)accentColor {
    switch (_style) {
        case CMChatRowStyleUser:
            return CMThemeColor(ui::theme::kAccent);
        case CMChatRowStyleAssistant:
            return CMTextSecondaryColor();
        case CMChatRowStyleToolCall:
            return CMThemeColor(ui::theme::kAccentHi);
        case CMChatRowStyleToolResultOk:
            return CMThemeColor(ui::theme::kRenderCached);
        case CMChatRowStyleToolResultError:
        case CMChatRowStyleError:
            return CMThemeColor(ui::theme::kError);
    }
    return CMTextSecondaryColor();
}

- (CGFloat)layoutForWidth:(CGFloat)width {
    const CGFloat inset = CGF(ui::theme::kSpaceS);
    const CGFloat stripWidth = 3.0;
    const CGFloat textWidth =
        std::max<CGFloat>(0.0, width - stripWidth - 2 * inset);
    _textField.preferredMaxLayoutWidth = textWidth;
    // -sizeThatFits: is UIKit, not AppKit; measure with the string's own
    // bounding rect instead, the standard AppKit technique for "how tall
    // does this wrapped label need to be at this width".
    NSDictionary* attributes = @{NSFontAttributeName : _textField.font};
    NSRect bounds =
        [(_textField.stringValue.length ? _textField.stringValue : @" ")
            boundingRectWithSize:NSMakeSize(textWidth, CGFLOAT_MAX)
                         options:NSStringDrawingUsesLineFragmentOrigin
                      attributes:attributes];
    const CGFloat textHeight = std::ceil(bounds.size.height) + 2.0;
    const CGFloat rowHeight = textHeight + 2 * inset;

    _accentStrip.frame = NSMakeRect(0, 0, stripWidth, rowHeight);
    _textField.frame =
        NSMakeRect(stripWidth + inset, inset, textWidth, textHeight);
    self.frame = NSMakeRect(0, 0, width, rowHeight);
    return rowHeight;
}

@end

// ---------------------------------------------------------------------
// CMChatPanelView
// ---------------------------------------------------------------------

@interface CMChatPanelView () <NSTextFieldDelegate>
- (void)cmLayoutForSize:(NSSize)size;
- (void)cmRelayoutTranscript;
- (CMChatRowStyle)cmStyleForEntry:(const chat::ChatTranscriptEntry&)entry;
- (NSString*)cmDisplayTextForEntry:(const chat::ChatTranscriptEntry&)entry;
- (void)appendTranscriptEntry:(const chat::ChatTranscriptEntry&)entry;
- (void)cmScrollToBottom;
- (void)sendPressed:(id)sender;
@end

@implementation CMChatPanelView {
    NSScrollView* _scrollView;
    NSView* _transcriptContainer;  // flipped; rows stack top to bottom
    NSTextField* _inputField;
    NSButton* _sendButton;
    NSTextField* _statusLabel;

    McpBackend* _liveBackend;  // not owned; see -configureWithBackend:
    std::unique_ptr<MainThreadBackend> _mainThreadBackend;
    std::unique_ptr<McpToolRegistry> _registry;
    std::unique_ptr<chat::ChatLlmClient> _llm;
    std::unique_ptr<chat::ChatSession> _session;
    std::string _missingKeyError;
    BOOL _turnInFlight;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        self.layer.backgroundColor = CMSurfacePanelColor().CGColor;

        _transcriptContainer = [[CMFlippedView alloc] initWithFrame:NSZeroRect];
        _scrollView = [[NSScrollView alloc] initWithFrame:NSZeroRect];
        _scrollView.documentView = _transcriptContainer;
        _scrollView.hasVerticalScroller = YES;
        _scrollView.drawsBackground = NO;
        _scrollView.borderType = NSNoBorder;
        [self addSubview:_scrollView];

        _statusLabel = [NSTextField wrappingLabelWithString:@""];
        _statusLabel.font =
            CMFont(ui::theme::kFontSizeCaption, NSFontWeightRegular);
        _statusLabel.textColor = CMThemeColor(ui::theme::kError);
        _statusLabel.backgroundColor = NSColor.clearColor;
        _statusLabel.hidden = YES;
        [self addSubview:_statusLabel];

        _inputField = [[NSTextField alloc] initWithFrame:NSZeroRect];
        _inputField.placeholderString = @"Décrire une modification…";
        _inputField.font =
            CMFont(ui::theme::kFontSizeBody, NSFontWeightRegular);
        _inputField.delegate = self;
        _inputField.target = self;
        _inputField.action = @selector(sendPressed:);
        [self addSubview:_inputField];

        _sendButton =
            CMMakeStyledButton(@"Envoyer", self, @selector(sendPressed:));
        [self addSubview:_sendButton];

        [self cmLayoutForSize:frame.size];
    }
    return self;
}

- (void)configureWithBackend:(McpBackend&)backend {
    _liveBackend = &backend;
    _mainThreadBackend = std::make_unique<MainThreadBackend>(backend);
    _registry = std::make_unique<McpToolRegistry>();

    chat::ChatLlmConfig config =
        chat::ChatLlmConfig::FromEnvironment(&_missingKeyError);
    _llm = std::make_unique<chat::ChatLlmClient>(std::move(config),
                                                 AnthropicUrlSessionTransport);

    __weak CMChatPanelView* weakSelf = self;
    _session = std::make_unique<chat::ChatSession>(
        *_mainThreadBackend, *_registry, *_llm, chat::kDefaultSystemPrompt,
        [weakSelf](const chat::ChatTranscriptEntry& entry) {
            // ChatSession::SubmitUserMessage runs on the background queue
            // -sendPressed: spawns; hop back to the main thread for every
            // UI update, one transcript entry at a time, so the panel fills
            // in live instead of jumping once at the end of the turn.
            const chat::ChatTranscriptEntry entryCopy = entry;
            dispatch_async(dispatch_get_main_queue(), ^{
              CMChatPanelView* strongSelf = weakSelf;
              if (strongSelf) [strongSelf appendTranscriptEntry:entryCopy];
            });
        });

    if (!_missingKeyError.empty()) {
        _statusLabel.stringValue =
            [NSString stringWithUTF8String:_missingKeyError.c_str()];
        _statusLabel.hidden = NO;
        [self cmLayoutForSize:self.bounds.size];
    }
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self cmLayoutForSize:newSize];
}

- (void)cmLayoutForSize:(NSSize)size {
    const CGFloat inset = CGF(ui::theme::kSpaceS);
    const CGFloat inputHeight = CGF(ui::theme::kControlRowHeight);
    const CGFloat sendWidth = 76.0;
    const CGFloat statusHeight = _statusLabel.hidden ? 0.0 : 28.0;

    _statusLabel.frame =
        NSMakeRect(inset, inputHeight + inset, size.width - 2 * inset,
                   std::max<CGFloat>(0.0, statusHeight - inset));
    _inputField.frame =
        NSMakeRect(inset, inset,
                   std::max<CGFloat>(0.0, size.width - sendWidth - 3 * inset),
                   inputHeight);
    _sendButton.frame = NSMakeRect(size.width - sendWidth - inset, inset,
                                   sendWidth, inputHeight);

    const CGFloat scrollTop = inputHeight + statusHeight + 2 * inset;
    _scrollView.frame =
        NSMakeRect(0, scrollTop, size.width,
                   std::max<CGFloat>(0.0, size.height - scrollTop));
    [self cmRelayoutTranscript];
}

// Re-stacks every row top to bottom at the scroll view's current content
// width. Simple rather than incremental -- chat transcripts in one session
// are short enough (a handful of turns, each a handful of tool calls) that
// re-measuring every row on each append or resize is not a real cost, and
// it keeps this file free of the bookkeeping an incremental layout would
// need to stay correct across resizes.
- (void)cmRelayoutTranscript {
    const CGFloat width = _scrollView.contentSize.width;
    CGFloat y = 0.0;
    for (NSView* row in _transcriptContainer.subviews) {
        if (![row isKindOfClass:[CMChatRowView class]]) continue;
        CMChatRowView* chatRow = (CMChatRowView*)row;
        const CGFloat height = [chatRow layoutForWidth:width];
        chatRow.frame = NSMakeRect(0, y, width, height);
        y += height;
    }
    _transcriptContainer.frame = NSMakeRect(0, 0, width, y);
}

- (CMChatRowStyle)cmStyleForEntry:(const chat::ChatTranscriptEntry&)entry {
    switch (entry.kind) {
        case chat::ChatEntryKind::UserMessage:
            return CMChatRowStyleUser;
        case chat::ChatEntryKind::AssistantText:
            return CMChatRowStyleAssistant;
        case chat::ChatEntryKind::ToolCall:
            return CMChatRowStyleToolCall;
        case chat::ChatEntryKind::ToolResult:
            return entry.tool_ok ? CMChatRowStyleToolResultOk
                                 : CMChatRowStyleToolResultError;
        case chat::ChatEntryKind::Error:
            return CMChatRowStyleError;
    }
    return CMChatRowStyleAssistant;
}

- (NSString*)cmDisplayTextForEntry:(const chat::ChatTranscriptEntry&)entry {
    std::string text;
    switch (entry.kind) {
        case chat::ChatEntryKind::UserMessage:
            text = "Vous : " + entry.text;
            break;
        case chat::ChatEntryKind::AssistantText:
            text = "Agent : " + entry.text;
            break;
        case chat::ChatEntryKind::ToolCall:
            text = "→ " + entry.tool_name + "(" + entry.tool_args_json + ")";
            break;
        case chat::ChatEntryKind::ToolResult:
            text = (entry.tool_ok ? std::string("✓ ") : std::string("✗ ")) +
                   entry.tool_name + " : " + entry.text;
            break;
        case chat::ChatEntryKind::Error:
            text = "Erreur : " + entry.text;
            break;
    }
    return [NSString stringWithUTF8String:text.c_str()];
}

- (void)appendTranscriptEntry:(const chat::ChatTranscriptEntry&)entry {
    CMChatRowView* row =
        [[CMChatRowView alloc] initWithText:[self cmDisplayTextForEntry:entry]
                                      style:[self cmStyleForEntry:entry]];
    [_transcriptContainer addSubview:row];
    [self cmRelayoutTranscript];
    [self cmScrollToBottom];
}

- (void)cmScrollToBottom {
    NSPoint bottom = NSMakePoint(
        0, std::max<CGFloat>(0.0, _transcriptContainer.frame.size.height -
                                      _scrollView.contentSize.height));
    [_transcriptContainer scrollPoint:bottom];
}

- (void)sendPressed:(id)sender {
    (void)sender;
    if (_turnInFlight || !_session) return;
    NSString* text = [_inputField.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet
                                            .whitespaceAndNewlineCharacterSet];
    if (text.length == 0) return;
    if (!_missingKeyError.empty()) {
        _statusLabel.hidden = NO;
        [self cmLayoutForSize:self.bounds.size];
        return;
    }

    _inputField.stringValue = @"";
    _inputField.enabled = NO;
    _sendButton.enabled = NO;
    _turnInFlight = YES;

    const std::string instruction = text.UTF8String ? text.UTF8String : "";
    chat::ChatSession* session = _session.get();
    __weak CMChatPanelView* weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      std::string error;
      const bool ok = session->SubmitUserMessage(instruction, error);
      (void)ok;  // failures are already in the transcript as an Error entry
      dispatch_async(dispatch_get_main_queue(), ^{
        CMChatPanelView* strongSelf = weakSelf;
        if (!strongSelf) return;
        strongSelf->_inputField.enabled = YES;
        strongSelf->_sendButton.enabled = YES;
        strongSelf->_turnInFlight = NO;
        [strongSelf.window makeFirstResponder:strongSelf->_inputField];
      });
    });
}

@end
