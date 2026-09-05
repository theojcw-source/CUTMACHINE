#import "ChatPanelView.h"

#include "ChatLlmClient.h"
#include "ChatSession.h"
#include "Document.h"
#include "McpTools.h"
#include "UiTheme.h"

#import "UiComponents.h"
#import "UiThemeAppKit.h"

#import <Security/Security.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace {

NSString* const kAiModelDefaultsKey = @"AiEngine.Model.v1";
NSString* const kAiBaseUrlDefaultsKey = @"AiEngine.BaseUrl.v1";
NSString* const kAiProviderDefaultsKey = @"AiEngine.Provider.v1";
NSString* const kAiKeychainService = @"com.cutmachine.editor.ai";
NSString* const kAnthropicKeychainAccount = @"anthropic-messages-api-key";
NSString* const kOpenAiKeychainAccount = @"openai-compatible-api-key";

NSDictionary* AiKeychainIdentity(chat::ChatLlmProvider provider) {
    NSString* account = provider == chat::ChatLlmProvider::AnthropicMessages
                            ? kAnthropicKeychainAccount
                            : kOpenAiKeychainAccount;
    return @{
        (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService : kAiKeychainService,
        (__bridge id)kSecAttrAccount : account,
    };
}

std::string SecurityError(OSStatus status) {
    CFStringRef text = SecCopyErrorMessageString(status, nullptr);
    NSString* message = CFBridgingRelease(text);
    return message.UTF8String ?: "unknown Keychain error";
}

bool ReadAiApiKey(chat::ChatLlmProvider provider, std::string& key,
                  std::string& error) {
    key.clear();
    error.clear();
    NSMutableDictionary* query = [AiKeychainIdentity(provider) mutableCopy];
    query[(__bridge id)kSecReturnData] = @YES;
    query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
    CFTypeRef result = nullptr;
    const OSStatus status =
        SecItemCopyMatching((__bridge CFDictionaryRef)query, &result);
    if (status == errSecItemNotFound) return true;
    if (status != errSecSuccess) {
        error = "unable to read the API key from Keychain: " +
                SecurityError(status);
        return false;
    }
    NSData* data = CFBridgingRelease(result);
    NSString* value = [[NSString alloc] initWithData:data
                                            encoding:NSUTF8StringEncoding];
    if (!value) {
        error = "the API key stored in Keychain is not valid UTF-8";
        return false;
    }
    key = value.UTF8String ?: "";
    return true;
}

bool WriteAiApiKey(chat::ChatLlmProvider provider, const std::string& key,
                   std::string& error) {
    error.clear();
    NSDictionary* identity = AiKeychainIdentity(provider);
    if (key.empty()) {
        const OSStatus status =
            SecItemDelete((__bridge CFDictionaryRef)identity);
        if (status == errSecSuccess || status == errSecItemNotFound)
            return true;
        error = "unable to delete the API key from Keychain: " +
                SecurityError(status);
        return false;
    }
    NSData* data = [NSData dataWithBytes:key.data() length:key.size()];
    const NSDictionary* values = @{(__bridge id)kSecValueData : data};
    OSStatus status = SecItemUpdate((__bridge CFDictionaryRef)identity,
                                    (__bridge CFDictionaryRef)values);
    if (status == errSecItemNotFound) {
        NSMutableDictionary* item = [identity mutableCopy];
        item[(__bridge id)kSecValueData] = data;
        status = SecItemAdd((__bridge CFDictionaryRef)item, nullptr);
    }
    if (status == errSecSuccess) return true;
    error = "unable to save the API key in Keychain: " + SecurityError(status);
    return false;
}

chat::ChatLlmConfig LoadAiConfiguration(std::string& configurationError) {
    chat::ChatLlmConfig config =
        chat::ChatLlmConfig::FromEnvironment(&configurationError);
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    NSString* provider = [defaults stringForKey:kAiProviderDefaultsKey];
    if ([provider isEqualToString:@"openai-compatible"] ||
        [provider isEqualToString:@"ollama"])
        config.provider = chat::ChatLlmProvider::Ollama;
    NSString* model = [defaults stringForKey:kAiModelDefaultsKey];
    NSString* baseUrl = [defaults stringForKey:kAiBaseUrlDefaultsKey];
    if (config.provider == chat::ChatLlmProvider::Ollama) {
        config.api_key.clear();
        if (model.length == 0) config.model = "qwen2.5-coder:7b";
        if (baseUrl.length == 0) {
            config.base_url = "http://localhost:11434";
        } else if ([baseUrl hasSuffix:@"/v1"]) {
            baseUrl = [baseUrl substringToIndex:baseUrl.length - 3];
        }
    }
    if (model.length > 0) config.model = model.UTF8String ?: config.model;
    if (baseUrl.length > 0)
        config.base_url = baseUrl.UTF8String ?: config.base_url;

    std::string storedKey;
    std::string keychainError;
    if (!ReadAiApiKey(config.provider, storedKey, keychainError)) {
        configurationError = keychainError;
    } else if (!storedKey.empty()) {
        config.api_key = std::move(storedKey);
    }
    if (config.provider == chat::ChatLlmProvider::AnthropicMessages &&
        config.api_key.empty()) {
        if (keychainError.empty())
            configurationError = "Moteur IA non configuré.";
    } else if (keychainError.empty()) {
        configurationError.clear();
    }
    return config;
}

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

    bool ApplyProjectEdit(ProjectOperation operation, std::string& resultJson,
                          std::string& errorName,
                          std::string& message) override {
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok =
              live_.ApplyProjectEdit(operation, resultJson, errorName, message);
        });
        return ok;
    }

    bool ReadTimelineTranscript(std::string& json,
                                std::string& message) override {
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok = live_.ReadTimelineTranscript(json, message);
        });
        return ok;
    }

    // Forwarded like everything else. Leaving these to McpBackend's
    // "this backend has no project cache" defaults silently disabled every
    // transcript- and picture-driven tool from the chat panel while the same
    // tools worked over MCP -- the exact kind of surface-specific behaviour
    // PHILOSOPHY.md principle 3 rules out.
    bool ReadSourceTranscript(const Ulid& sourceId, Transcript& transcript,
                              std::string& message) override {
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok = live_.ReadSourceTranscript(sourceId, transcript, message);
        });
        return ok;
    }

    bool ReadSourceShotQuality(const Ulid& sourceId, ShotQualityReport& report,
                               std::string& message) override {
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok = live_.ReadSourceShotQuality(sourceId, report, message);
        });
        return ok;
    }

    bool AnalyzeSourceShotQuality(const Ulid& sourceId, std::string& resultJson,
                                  std::string& message) override {
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok = live_.AnalyzeSourceShotQuality(sourceId, resultJson, message);
        });
        return ok;
    }

    bool ReadSourceSpeechOnset(const Ulid& sourceId, SpeechOnsetReport& report,
                               std::string& message) override {
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok = live_.ReadSourceSpeechOnset(sourceId, report, message);
        });
        return ok;
    }

    bool AnalyzeSourceSpeechOnset(const Ulid& sourceId,
                                  const SpeechOnsetSettings& settings,
                                  std::string& resultJson,
                                  std::string& message) override {
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok = live_.AnalyzeSourceSpeechOnset(sourceId, settings, resultJson,
                                              message);
        });
        return ok;
    }

    bool CaptureSourceFrame(const Ulid& sourceId, const RationalTime& time,
                            std::string& jpegBytes,
                            std::string& message) override {
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok = live_.CaptureSourceFrame(sourceId, time, jpegBytes, message);
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
bool UrlSessionTransport(const chat::ChatHttpRequest& request,
                         chat::ChatHttpResponse& response, std::string& error) {
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
- (void)cmReloadConfiguration;
- (void)configurePressed:(id)sender;
- (void)presetChanged:(id)sender;
- (void)sendPressed:(id)sender;
@end

@implementation CMChatPanelView {
    NSScrollView* _scrollView;
    NSView* _transcriptContainer;  // flipped; rows stack top to bottom
    NSTextField* _inputField;
    NSButton* _sendButton;
    NSButton* _configureButton;
    NSPopUpButton* _presetPopup;
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

        _presetPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect];
        [_presetPopup addItemWithTitle:@"Prompt libre"];
        [_presetPopup addItemWithTitle:@"Short interview dynamique — 60 s"];
        _presetPopup.target = self;
        _presetPopup.action = @selector(presetChanged:);
        _presetPopup.identifier = @"chat.prompt_preset";
        [self addSubview:_presetPopup];

        _configureButton = CMMakeStyledButton(@"Configurer le moteur IA…", self,
                                              @selector(configurePressed:));
        _configureButton.identifier = @"chat.configure_engine";
        [self addSubview:_configureButton];

        [self cmLayoutForSize:frame.size];
    }
    return self;
}

- (void)configureWithBackend:(McpBackend&)backend {
    _liveBackend = &backend;
    _mainThreadBackend = std::make_unique<MainThreadBackend>(backend);
    _registry = std::make_unique<McpToolRegistry>();

    [self cmReloadConfiguration];
}

- (void)cmReloadConfiguration {
    if (!_mainThreadBackend || !_registry) return;
    _missingKeyError.clear();
    chat::ChatLlmConfig config = LoadAiConfiguration(_missingKeyError);
    _llm = std::make_unique<chat::ChatLlmClient>(std::move(config),
                                                 UrlSessionTransport);

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

    if (_missingKeyError.empty()) {
        _statusLabel.hidden = YES;
        _statusLabel.stringValue = @"";
    } else {
        _statusLabel.stringValue =
            [NSString stringWithUTF8String:_missingKeyError.c_str()];
        _statusLabel.hidden = NO;
    }
    [self cmLayoutForSize:self.bounds.size];
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self cmLayoutForSize:newSize];
}

- (void)cmLayoutForSize:(NSSize)size {
    const CGFloat inset = CGF(ui::theme::kSpaceS);
    const CGFloat inputHeight = CGF(ui::theme::kControlRowHeight);
    const CGFloat sendWidth = 76.0;
    const CGFloat configureHeight = 28.0;
    const CGFloat presetHeight = 28.0;
    const CGFloat statusHeight = _statusLabel.hidden ? 0.0 : 28.0;

    _statusLabel.frame = NSMakeRect(
        inset, inputHeight + presetHeight + configureHeight + 3 * inset,
        size.width - 2 * inset, std::max<CGFloat>(0.0, statusHeight - inset));
    _inputField.frame =
        NSMakeRect(inset, inset,
                   std::max<CGFloat>(0.0, size.width - sendWidth - 3 * inset),
                   inputHeight);
    _sendButton.frame = NSMakeRect(size.width - sendWidth - inset, inset,
                                   sendWidth, inputHeight);
    _presetPopup.frame = NSMakeRect(inset, inputHeight + 2 * inset,
                                    size.width - 2 * inset, presetHeight);
    _configureButton.frame =
        NSMakeRect(inset, inputHeight + presetHeight + 3 * inset,
                   size.width - 2 * inset, configureHeight);

    const CGFloat scrollTop =
        inputHeight + presetHeight + configureHeight + statusHeight + 5 * inset;
    _scrollView.frame =
        NSMakeRect(0, scrollTop, size.width,
                   std::max<CGFloat>(0.0, size.height - scrollTop));
    [self cmRelayoutTranscript];
}

- (void)presetChanged:(id)sender {
    (void)sender;
    _inputField.placeholderString = _presetPopup.indexOfSelectedItem == 1
                                        ? @"Angle ou consigne facultative…"
                                        : @"Décrire une modification…";
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

- (void)configurePressed:(id)sender {
    (void)sender;
    [self showConfigurationWindow];
}

- (void)showConfigurationWindow {
    if (_turnInFlight) {
        NSBeep();
        return;
    }
    const chat::ChatLlmConfig current =
        _llm ? _llm->Config() : chat::ChatLlmConfig::FromEnvironment();
    NSString* currentModel =
        [NSString stringWithUTF8String:current.model.c_str()] ?: @"";
    NSString* currentBaseUrl =
        [NSString stringWithUTF8String:current.base_url.c_str()] ?: @"";

    while (true) {
        NSAlert* alert = [NSAlert new];
        alert.messageText = @"Moteur IA";
        alert.informativeText =
            @"Choisissez Anthropic ou un serveur local Ollama compatible "
             "OpenAI. Une éventuelle clé reste dans le Trousseau de ce Mac "
             "et ne fait jamais partie du projet.";
        [alert addButtonWithTitle:@"Enregistrer"];
        [alert addButtonWithTitle:@"Annuler"];

        NSView* form =
            [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 480, 190)];
        const auto addLabel = [&](NSString* title, CGFloat y) {
            NSTextField* label = [NSTextField labelWithString:title];
            label.frame = NSMakeRect(0, y + 3, 128, 20);
            label.font = CMFont(ui::theme::kFontSizeBody, NSFontWeightRegular);
            [form addSubview:label];
        };

        addLabel(@"Protocole", 156);
        NSPopUpButton* protocol =
            [[NSPopUpButton alloc] initWithFrame:NSMakeRect(136, 152, 344, 28)];
        [protocol addItemWithTitle:@"Anthropic Messages API"];
        [protocol addItemWithTitle:@"Ollama local"];
        [protocol
            selectItemAtIndex:current.provider == chat::ChatLlmProvider::Ollama
                                  ? 1
                                  : 0];
        protocol.identifier = @"ai_engine.provider";
        [form addSubview:protocol];

        addLabel(@"Modèle", 116);
        NSTextField* model =
            [[NSTextField alloc] initWithFrame:NSMakeRect(136, 112, 344, 26)];
        model.stringValue = currentModel;
        model.placeholderString = @"qwen2.5-coder:7b";
        model.identifier = @"ai_engine.model";
        [form addSubview:model];

        addLabel(@"URL de base", 76);
        NSTextField* baseUrl =
            [[NSTextField alloc] initWithFrame:NSMakeRect(136, 72, 344, 26)];
        baseUrl.stringValue = currentBaseUrl;
        baseUrl.placeholderString = @"http://localhost:11434";
        baseUrl.identifier = @"ai_engine.base_url";
        [form addSubview:baseUrl];

        addLabel(@"Clé API", 36);
        NSSecureTextField* apiKey = [[NSSecureTextField alloc]
            initWithFrame:NSMakeRect(136, 32, 344, 26)];
        apiKey.placeholderString = current.api_key.empty()
                                       ? @"Facultative avec Ollama local"
                                       : @"Laisser vide pour conserver la clé";
        apiKey.identifier = @"ai_engine.api_key";
        [form addSubview:apiKey];

        NSButton* removeKey =
            [NSButton checkboxWithTitle:@"Supprimer la clé enregistrée"
                                 target:nil
                                 action:nil];
        removeKey.frame = NSMakeRect(136, 0, 344, 22);
        removeKey.identifier = @"ai_engine.remove_key";
        [form addSubview:removeKey];
        alert.accessoryView = form;

        if ([alert runModal] != NSAlertFirstButtonReturn) return;
        NSString* selectedModel = [model.stringValue
            stringByTrimmingCharactersInSet:
                NSCharacterSet.whitespaceAndNewlineCharacterSet];
        NSString* selectedBaseUrl = [baseUrl.stringValue
            stringByTrimmingCharactersInSet:
                NSCharacterSet.whitespaceAndNewlineCharacterSet];
        const chat::ChatLlmProvider selectedProvider =
            protocol.indexOfSelectedItem == 1
                ? chat::ChatLlmProvider::Ollama
                : chat::ChatLlmProvider::AnthropicMessages;
        if (selectedProvider != current.provider) {
            if ([selectedModel isEqualToString:currentModel])
                selectedModel =
                    selectedProvider == chat::ChatLlmProvider::Ollama
                        ? @"qwen2.5-coder:7b"
                        : @"claude-sonnet-4-5";
            if ([selectedBaseUrl isEqualToString:currentBaseUrl])
                selectedBaseUrl =
                    selectedProvider == chat::ChatLlmProvider::Ollama
                        ? @"http://localhost:11434"
                        : @"https://api.anthropic.com";
        }
        while ([selectedBaseUrl hasSuffix:@"/"])
            selectedBaseUrl =
                [selectedBaseUrl substringToIndex:selectedBaseUrl.length - 1];
        NSURLComponents* components =
            [NSURLComponents componentsWithString:selectedBaseUrl];
        const BOOL validScheme =
            [components.scheme.lowercaseString isEqualToString:@"https"] ||
            [components.scheme.lowercaseString isEqualToString:@"http"];
        if (selectedModel.length == 0 || selectedBaseUrl.length == 0 ||
            !validScheme || components.host.length == 0) {
            NSAlert* invalid = [NSAlert new];
            invalid.alertStyle = NSAlertStyleWarning;
            invalid.messageText = @"Configuration IA invalide";
            invalid.informativeText =
                @"Renseignez un modèle et une URL HTTP(S) complète, sans le "
                 "chemin final /messages ou /chat/completions.";
            [invalid runModal];
            continue;
        }

        std::string keychainError;
        if (removeKey.state == NSControlStateValueOn) {
            if (!WriteAiApiKey(selectedProvider, {}, keychainError)) {
                NSAlert* failure = [NSAlert new];
                failure.alertStyle = NSAlertStyleCritical;
                failure.messageText = @"Trousseau inaccessible";
                failure.informativeText =
                    [NSString stringWithUTF8String:keychainError.c_str()];
                [failure runModal];
                continue;
            }
        } else if (apiKey.stringValue.length > 0) {
            const std::string key = apiKey.stringValue.UTF8String ?: "";
            if (!WriteAiApiKey(selectedProvider, key, keychainError)) {
                NSAlert* failure = [NSAlert new];
                failure.alertStyle = NSAlertStyleCritical;
                failure.messageText = @"Trousseau inaccessible";
                failure.informativeText =
                    [NSString stringWithUTF8String:keychainError.c_str()];
                [failure runModal];
                continue;
            }
        }

        NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
        [defaults setObject:selectedProvider == chat::ChatLlmProvider::Ollama
                                ? @"ollama"
                                : @"anthropic-messages"
                     forKey:kAiProviderDefaultsKey];
        [defaults setObject:selectedModel forKey:kAiModelDefaultsKey];
        [defaults setObject:selectedBaseUrl forKey:kAiBaseUrlDefaultsKey];
        for (NSView* row in [_transcriptContainer.subviews copy])
            [row removeFromSuperview];
        [self cmReloadConfiguration];
        [self cmRelayoutTranscript];
        [self.window makeFirstResponder:_inputField];
        return;
    }
}

- (void)sendPressed:(id)sender {
    (void)sender;
    if (_turnInFlight || !_session) return;
    NSString* text = [_inputField.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet
                                            .whitespaceAndNewlineCharacterSet];
    const BOOL shortPreset = _presetPopup.indexOfSelectedItem == 1;
    if (text.length == 0 && !shortPreset) return;
    if (!_missingKeyError.empty()) {
        _statusLabel.hidden = NO;
        [self cmLayoutForSize:self.bounds.size];
        return;
    }

    _inputField.stringValue = @"";
    _inputField.enabled = NO;
    _sendButton.enabled = NO;
    _turnInFlight = YES;

    std::string instruction = text.UTF8String ? text.UTF8String : "";
    if (shortPreset) {
        instruction =
            "Exécute le preset « Short interview dynamique — 60 s ». "
            "Appelle d'abord get_timeline_transcript. Choisis uniquement des "
            "spans présents, pour une durée totale cible de 50 à 65 secondes. "
            "Réordonne-les pour ouvrir sur l'accroche la plus forte, garder "
            "un développement concis et finir sur une chute claire. Élimine "
            "les répétitions et les digressions. Copie source_id, source_in "
            "et duration exactement, sans calcul ni modification, puis "
            "appelle create_interview_short une seule fois. Ne modifie pas "
            "la timeline d'origine.";
        if (text.length > 0)
            instruction += " Consigne éditoriale supplémentaire : " +
                           std::string(text.UTF8String ?: "");
    }
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
