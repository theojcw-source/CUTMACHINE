#include "ChatSession.h"

#include <utility>

namespace chat {

namespace {

// A runaway agentic loop (model keeps requesting tools and never settles)
// is a bug in the model or the prompt, not a normal outcome -- stop and
// surface it rather than let the chat panel hang or burn API budget
// indefinitely. Sized generously above what any single CUTMACHINE
// instruction should need (a `describe` call plus a small handful of
// edits), the same spirit as the sidecar's own two-attempt retry cap in
// sidecar/repl.py, just wider because this loop's tool catalog is the full
// F1.2 surface rather than one structured plan.
constexpr int kMaxToolIterations = 8;

}  // namespace

const char kDefaultSystemPrompt[] =
    "Tu es l'agent de montage intégré à CUTMACHINE, un éditeur vidéo "
    "déterministe. Tu ne modifies jamais le montage directement : chaque "
    "changement passe par un appel d'outil, qui applique une opération "
    "réversible et journalisée. N'invente jamais un identifiant ; commence "
    "par l'outil `describe` pour observer l'état actuel de la timeline "
    "(pistes, clips, marqueurs, médiathèque) si tu ne l'as pas déjà fait "
    "dans cette conversation ou si la demande porte sur un état que tu "
    "n'as pas encore vu. Les identifiants complets ou tout préfixe non "
    "ambigu sont acceptés par les outils qui prennent un ID.\n"
    "Si une demande est ambiguë ou impossible avec les outils disponibles, "
    "explique pourquoi en texte plutôt que d'appeler un outil au hasard. "
    "Si un outil échoue, le message d'erreur t'est renvoyé : corrige ton "
    "appel ou explique le blocage à l'utilisateur, ne réessaie pas "
    "indéfiniment le même appel. Réponds en français, de façon concise, et "
    "résume ce que tu as fait après chaque outil appliqué avec succès.";

ChatSession::ChatSession(
    McpBackend& backend, const McpToolRegistry& registry,
    const ChatLlmClient& llm, std::string systemPrompt,
    std::function<void(const ChatTranscriptEntry&)> onEntry)
    : backend_(backend),
      registry_(registry),
      llm_(llm),
      system_prompt_(std::move(systemPrompt)),
      on_entry_(std::move(onEntry)) {}

void ChatSession::Emit(ChatTranscriptEntry entry) {
    if (on_entry_) on_entry_(entry);
    transcript_.push_back(std::move(entry));
}

bool ChatSession::SubmitUserMessage(const std::string& text,
                                    std::string& error) {
    ChatTranscriptEntry userEntry;
    userEntry.kind = ChatEntryKind::UserMessage;
    userEntry.text = text;
    Emit(std::move(userEntry));

    ChatMessage userMessage;
    userMessage.role = "user";
    userMessage.content.push_back(ChatContentBlock::MakeText(text));
    messages_.push_back(std::move(userMessage));

    for (int iteration = 0; iteration < kMaxToolIterations; ++iteration) {
        const ChatSendResult result =
            llm_.SendMessages(messages_, system_prompt_, registry_.Tools());
        if (!result.ok) {
            error = result.error;
            ChatTranscriptEntry errorEntry;
            errorEntry.kind = ChatEntryKind::Error;
            errorEntry.text = error;
            Emit(std::move(errorEntry));
            return false;
        }

        ChatMessage assistantMessage;
        assistantMessage.role = "assistant";
        assistantMessage.content = result.content;
        messages_.push_back(std::move(assistantMessage));

        std::vector<ChatContentBlock> toolUses;
        for (const ChatContentBlock& block : result.content) {
            if (block.type == ChatBlockType::Text) {
                if (block.text.empty()) continue;
                ChatTranscriptEntry textEntry;
                textEntry.kind = ChatEntryKind::AssistantText;
                textEntry.text = block.text;
                Emit(std::move(textEntry));
            } else if (block.type == ChatBlockType::ToolUse) {
                toolUses.push_back(block);
            }
        }

        if (toolUses.empty()) return true;  // end_turn: no more tools requested

        ChatMessage toolResultsMessage;
        toolResultsMessage.role = "user";
        for (const ChatContentBlock& use : toolUses) {
            ChatTranscriptEntry callEntry;
            callEntry.kind = ChatEntryKind::ToolCall;
            callEntry.tool_name = use.tool_name;
            callEntry.tool_args_json = use.tool_input.Dump();
            Emit(std::move(callEntry));

            const McpToolCallOutcome outcome =
                registry_.Call(backend_, use.tool_name, use.tool_input);
            const std::string resultText =
                outcome.ok ? outcome.result_json
                           : (outcome.error_name + ": " + outcome.message);

            ChatTranscriptEntry resultEntry;
            resultEntry.kind = ChatEntryKind::ToolResult;
            resultEntry.tool_name = use.tool_name;
            resultEntry.tool_ok = outcome.ok;
            resultEntry.text = resultText;
            Emit(std::move(resultEntry));

            toolResultsMessage.content.push_back(
                ChatContentBlock::MakeToolResult(use.tool_use_id, resultText,
                                                 !outcome.ok));
        }
        messages_.push_back(std::move(toolResultsMessage));
    }

    error = "l'assistant a demandé des outils sans s'arrêter après " +
            std::to_string(kMaxToolIterations) +
            " allers-retours ; arrêt de sécurité.";
    ChatTranscriptEntry limitEntry;
    limitEntry.kind = ChatEntryKind::Error;
    limitEntry.text = error;
    Emit(std::move(limitEntry));
    return false;
}

}  // namespace chat
