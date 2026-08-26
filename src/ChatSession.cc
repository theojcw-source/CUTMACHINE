#include "ChatSession.h"

#include <set>
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

void CopyField(const mcp_json::Value& source, const std::string& key,
               mcp_json::Value& destination) {
    const mcp_json::Value* value = source.Find(key);
    if (value) destination.Set(key, *value);
}

std::string CompactDescribeForOllama(const std::string& json) {
    mcp_json::Value root;
    std::string parseError;
    if (!mcp_json::Value::Parse(json, root, parseError) || !root.IsObject())
        return json;
    const mcp_json::Value* timeline = root.Find("timeline");
    if (!timeline || !timeline->IsObject()) return json;

    std::set<std::string> usedSourceIds;
    const mcp_json::Value* tracks = timeline->Find("tracks");
    if (tracks && tracks->IsArray()) {
        for (const mcp_json::Value& track : tracks->AsArray()) {
            const mcp_json::Value* items = track.Find("items");
            if (!items || !items->IsArray()) continue;
            for (const mcp_json::Value& item : items->AsArray()) {
                const mcp_json::Value* sourceId = item.Find("source_id");
                if (sourceId && sourceId->IsString())
                    usedSourceIds.insert(sourceId->AsString());
            }
        }
    }

    mcp_json::Value compactTimeline = mcp_json::Value::MakeObject();
    CopyField(*timeline, "duration", compactTimeline);
    CopyField(*timeline, "tracks", compactTimeline);
    mcp_json::Value usedSources = mcp_json::Value::MakeArray();
    const mcp_json::Value* sources = timeline->Find("sources");
    if (sources && sources->IsArray()) {
        for (const mcp_json::Value& source : sources->AsArray()) {
            const mcp_json::Value* id = source.Find("id");
            if (id && id->IsString() && usedSourceIds.count(id->AsString()))
                usedSources.Push(source);
        }
    }
    compactTimeline.Set("sources", std::move(usedSources));

    mcp_json::Value compact = mcp_json::Value::MakeObject();
    CopyField(root, "sequence", compact);
    compact.Set("timeline", std::move(compactTimeline));
    const mcp_json::Value* library = root.Find("library");
    if (library && library->IsArray()) {
        mcp_json::Value compactLibrary = mcp_json::Value::MakeArray();
        for (const mcp_json::Value& item : library->AsArray()) {
            mcp_json::Value summary = mcp_json::Value::MakeObject();
            CopyField(item, "alias", summary);
            CopyField(item, "id", summary);
            CopyField(item, "filename", summary);
            CopyField(item, "bin_id", summary);
            CopyField(item, "in_use", summary);
            compactLibrary.Push(std::move(summary));
        }
        compact.Set("library", std::move(compactLibrary));
    }
    CopyField(root, "bins", compact);
    CopyField(root, "markers", compact);
    const std::string result = compact.Dump();
    return result.size() < json.size() ? result : json;
}

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
    "ambigu sont acceptés par les outils qui prennent un ID ; les aliases "
    "visibles comme A1, M1 et K1 le sont aussi. Préfère les outils "
    "d'intention déterministes comme `shorten_linked_clip` aux opérations "
    "brutes lorsqu'ils correspondent exactement à la demande.\n"
    "Pour monter une interview depuis sa transcription, appelle "
    "`get_timeline_transcript`, sélectionne seulement les spans retournés "
    "et recopie leurs temps exacts dans `create_interview_short`. Cet outil "
    "crée une nouvelle timeline et préserve l'originale.\n"
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
            const std::string modelResultText =
                outcome.ok && use.tool_name == "describe" &&
                        llm_.Config().provider == ChatLlmProvider::Ollama
                    ? CompactDescribeForOllama(resultText)
                    : resultText;

            ChatTranscriptEntry resultEntry;
            resultEntry.kind = ChatEntryKind::ToolResult;
            resultEntry.tool_name = use.tool_name;
            resultEntry.tool_ok = outcome.ok;
            resultEntry.text = resultText;
            Emit(std::move(resultEntry));

            toolResultsMessage.content.push_back(
                ChatContentBlock::MakeToolResult(use.tool_use_id,
                                                 modelResultText, !outcome.ok,
                                                 use.tool_name));
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
