#include "InterviewShort.h"
#include "Json.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
}  // namespace

int main() {
    Document document;
    const Ulid sourceId = "01K50000000000000000000001";
    document.sources = {{sourceId, "interview.mov", {25, 1}, {250, 25}}};
    document.sequence.tracks = {
        {"01K50000000000000000000002",
         "audio",
         0,
         {{"01K50000000000000000000003",
           sourceId,
           {50, 25},
           {100, 25},
           {25, 25}}}},
    };
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("cutmachine-interview-short-" + GenerateUlid());
    std::filesystem::create_directories(directory);
    {
        std::ofstream cache(directory / (sourceId + ".json"));
        cache << "{\"version\":1,\"media_id\":\"" << sourceId
              << "\",\"whisper_model\":\"test.bin\","
                 "\"source_rate\":{\"num\":25,\"den\":1},"
                 "\"words\":[{\"text\":\"Une\",\"start\":{\"value\":"
                 "50,\"rate\":25},\"end\":{\"value\":60,\"rate\":25}},{"
                 "\"text\":\"accroche.\",\"start\":{\"value\":60,"
                 "\"rate\":25},\"end\":{\"value\":75,\"rate\":25}}]}";
    }
    std::string json;
    std::string error;
    Check(DescribeTimelineTranscriptForAgent(document, directory, json, error),
          "describe transcript: " + error);
    mcp_json::Value root;
    Check(mcp_json::Value::Parse(json, root, error),
          "agent transcript JSON parses: " + error);
    const mcp_json::Value* spans = root.Find("spans");
    Check(spans && spans->IsArray() && spans->AsArray().size() == 1,
          "words are grouped into one semantic span");
    const mcp_json::Value& span = spans->AsArray().front();
    int64_t sourceIn = -1;
    Check(span.Find("source_in") && span.Find("source_in")->Find("value") &&
              span.Find("source_in")->Find("value")->AsInt64(sourceIn) &&
              sourceIn == 50 && span.Find("text") &&
              span.Find("text")->AsString() == "Une accroche.",
          "span exposes source-exact boundaries and readable text");
    std::filesystem::remove_all(directory);
    std::cout << "interview short tests passed\n";
    return 0;
}
