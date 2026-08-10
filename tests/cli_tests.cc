#include "Cli.h"
#include "Document.h"
#include "EditLog.h"
#include "Operations.h"
#include "Ulid.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

Document Fixture() {
    Document document;
    document.sources = {
        {"01K30000000000000000000001", "folder/A.MP4", {25, 1}, {1000, 25}},
    };
    document.tracks = {
        {"01K30000000000000000000002",
         "video",
         0,
         {{"01K30000000000000000000003",
           "01K30000000000000000000001",
           {100, 25},
           {10, 25},
           {5, 25}},
          {"01K30000000000000000000004",
           "01K30000000000000000000001",
           {200, 25},
           {10, 25},
           {20, 25}}}},
    };
    return document;
}

}  // namespace

int main() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        (GenerateUlid() + "-cli-tests");
    std::filesystem::create_directory(directory);
    const std::filesystem::path path = directory / "document.json";
    std::string error;
    Check(Fixture().Save(path.string(), error), "fixture saves: " + error);

    std::string firstDescription;
    std::string secondDescription;
    Check(DescribeCommand(path.string(), firstDescription) == 0,
          "describe succeeds");
    Check(DescribeCommand(path.string(), secondDescription) == 0,
          "second describe succeeds");
    Check(firstDescription == secondDescription, "describe is byte-stable");
    Check(firstDescription.find("\"alias\":\"A1\"") != std::string::npos &&
              firstDescription.find("\"alias\":\"A2\"") != std::string::npos,
          "describe emits stable per-track aliases");
    Check(firstDescription.find("\"type\":\"gap\"") != std::string::npos,
          "describe emits holes");
    Check(firstDescription.find("\"frames\":") != std::string::npos &&
              firstDescription.find("\"seconds\":") != std::string::npos,
          "describe emits frames and decimal seconds");

    const std::string before = Read(path);
    const Operation trim = TrimClipOperation{
        "01K30000000000000000000003", TrimEdge::Tail, {-1, 25}, std::nullopt};
    std::string result;
    Check(ApplyOperationCommand(path.string(), SerializeOperation(trim),
                                result) == 0,
          "valid apply-op succeeds: " + result);
    Check(result.find("{\"ok\":true,\"doc_hash\":\"") == 0,
          "valid apply-op returns a document hash");
    const std::string after = Read(path);
    Check(after != before, "valid apply-op changes the document bytes");

    EditLog log;
    EditError editError = EditError::None;
    std::string detail;
    Check(EditLog::Load(EditLogPathForDocument(path.string()), log, editError,
                        detail),
          "sidecar edit log loads: " + detail);
    Check(log.AppliedCount() == 1, "valid apply-op increments edit log");

    const Operation addBin =
        AddBinOperation{"01K30000000000000000000009", "Rushes CLI"};
    Check(ApplyOperationCommand(path.string(), SerializeOperation(addBin),
                                result) == 0,
          "CLI creates a persistent bin through the same operation path");
    std::string withBinDescription;
    Check(DescribeCommand(path.string(), withBinDescription) == 0 &&
              withBinDescription.find("\"name\":\"Rushes CLI\"") !=
                  std::string::npos,
          "describe exposes bins created by apply-op");
    const std::string afterBin = Read(path);

    const std::string logBeforeRejection =
        Read(EditLogPathForDocument(path.string()));
    const Operation rejected =
        RemoveClipOperation{"01K39999999999999999999999", {}};
    Check(ApplyOperationCommand(path.string(), SerializeOperation(rejected),
                                result) == 1,
          "refused apply-op returns status 1");
    Check(result.find("\"error\":\"UnknownClip\"") != std::string::npos,
          "refused apply-op returns the exact operation error name");
    Check(Read(path) == afterBin,
          "refused apply-op leaves document byte-identical");
    Check(Read(EditLogPathForDocument(path.string())) == logBeforeRejection,
          "refused apply-op leaves edit log byte-identical");

    Check(ApplyOperationCommand(path.string(), "{not json", result) == 1,
          "malformed operation returns status 1");
    Check(result.find("\"error\":\"ParseError\"") != std::string::npos,
          "malformed operation returns ParseError");
    Check(Read(path) == afterBin,
          "malformed operation leaves document byte-identical");

    std::filesystem::remove_all(directory);
    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All CLI tests passed\n";
    return 0;
}
