#include "Document.h"
#include "ProjectStorage.h"
#include "ResolveImport.h"
#include "Ulid.h"

#include <cstdlib>
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

std::string Quote(const std::filesystem::path& path) {
    std::string result = "'";
    for (char character : path.string()) {
        if (character == '\'')
            result += "'\\''";
        else
            result += character;
    }
    return result + "'";
}

void CheckRejected(const std::string& json, const std::string& label) {
    ResolveManifest manifest;
    std::string error;
    Check(!ParseResolveManifest(json, manifest, error),
          "le manifeste doit être refusé : " + label);
    Check(!error.empty(), "un refus nomme son motif : " + label);
}

const DocumentBin* FindBin(const Document& document, const std::string& name) {
    for (const DocumentBin& bin : document.bins)
        if (bin.name == name) return &bin;
    return nullptr;
}

}  // namespace

int main() {
    // --- Parsing -----------------------------------------------------------
    const std::string valid = R"({
        "schema": "cutmachine.resolve-manifest.v1",
        "project": "LISAAMOD273",
        "resolve_version": "20.3.1.6",
        "bins": [
            {"key": "b2", "name": "Rosie", "parent_key": "b1"},
            {"key": "b1", "name": "1_RUSHES", "parent_key": ""}
        ],
        "clips": [
            {"path": "/rushes/C8015.MP4", "name": "C8015.MP4",
             "bin_key": "b2", "resolve_uid": "5d45"},
            {"path": "/rushes/C8035.MP4", "name": "C8035.MP4", "bin_key": ""}
        ]
    })";
    ResolveManifest manifest;
    std::string error;
    Check(ParseResolveManifest(valid, manifest, error),
          "un manifeste valide est accepté : " + error);
    Check(manifest.project == "LISAAMOD273" && manifest.bins.size() == 2 &&
              manifest.clips.size() == 2,
          "le manifeste conserve projet, chutiers et rushes");
    Check(manifest.clips[0].resolve_uid == "5d45",
          "l'identifiant Resolve est conservé pour le diagnostic");

    CheckRejected(R"({"schema":"autre.v1"})", "schéma inconnu");
    CheckRejected(R"({"project":"x"})", "schéma absent");
    CheckRejected(R"({"schema":"cutmachine.resolve-manifest.v1","bins":[
        {"key":"b1","name":"A"},{"key":"b1","name":"B"}]})",
                  "clé dupliquée");
    CheckRejected(R"({"schema":"cutmachine.resolve-manifest.v1","bins":[
        {"key":"b1","name":"A","parent_key":"absent"}]})",
                  "parent inconnu");
    CheckRejected(R"({"schema":"cutmachine.resolve-manifest.v1","bins":[
        {"key":"b1","name":"A","parent_key":"b2"},
        {"key":"b2","name":"B","parent_key":"b1"}]})",
                  "cycle de chutiers");
    CheckRejected(R"({"schema":"cutmachine.resolve-manifest.v1","clips":[
        {"path":"/a.mp4","bin_key":"absent"}]})",
                  "rush dans un chutier absent");
    CheckRejected(R"({"schema":"cutmachine.resolve-manifest.v1","clips":[
        {"path":""}]})",
                  "rush sans chemin");
    CheckRejected(R"({"schema":"cutmachine.resolve-manifest.v1","bins":[
        {"key":"","name":"A"}]})",
                  "clé vide");

    // --- Planning ----------------------------------------------------------
    Document document;
    ResolveImportPlan plan;
    Check(PlanResolveImport(document, manifest, plan, error),
          "un document vierge se planifie : " + error);
    Check(plan.new_bins.size() == 2 && plan.reused_bins == 0,
          "les deux chutiers sont à créer");
    Check(plan.new_bins[0].name == "1_RUSHES" &&
              plan.new_bins[0].parent_id.empty(),
          "le parent est créé avant l'enfant, à la racine");
    Check(plan.new_bins[1].name == "Rosie" &&
              plan.new_bins[1].parent_id == plan.new_bins[0].bin_id,
          "l'enfant pointe vers le ULID de son parent");
    Check(plan.bin_ids.count("") == 1 && plan.bin_ids.at("").empty(),
          "la racine du manifeste reste le chutier vide");

    // Applying the plan then re-planning must create nothing: a second import
    // of a Media Pool that grew adds only what is new.
    for (const AddBinOperation& operation : plan.new_bins)
        document.bins.push_back(
            {operation.bin_id, operation.name, operation.parent_id});
    ResolveImportPlan replan;
    Check(PlanResolveImport(document, manifest, replan, error),
          "un second passage se planifie : " + error);
    Check(replan.new_bins.empty() && replan.reused_bins == 2,
          "un second import ne duplique aucun chutier");
    Check(replan.bin_ids.at("b2") == plan.new_bins[1].bin_id,
          "le chutier réutilisé garde son identité");

    // A homonym under a different parent is a different bin.
    Document twoParents;
    ResolveManifest homonyms;
    Check(ParseResolveManifest(
              R"({"schema":"cutmachine.resolve-manifest.v1","bins":[
                  {"key":"a","name":"Jour 01"},
                  {"key":"b","name":"B"},
                  {"key":"c","name":"Jour 01","parent_key":"b"}]})",
              homonyms, error),
          "manifeste homonyme accepté : " + error);
    ResolveImportPlan homonymPlan;
    Check(PlanResolveImport(twoParents, homonyms, homonymPlan, error),
          "manifeste homonyme planifié : " + error);
    Check(homonymPlan.new_bins.size() == 3,
          "deux chutiers homonymes sous des parents différents coexistent");

    // --- End to end --------------------------------------------------------
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / (GenerateUlid() + "-resolve");
    const std::filesystem::path media = root / "rushes";
    std::filesystem::create_directories(media);
    const std::filesystem::path first = media / "C8015.MP4";
    const std::filesystem::path second = media / "C8035.MP4";
    for (const std::filesystem::path& path : {first, second}) {
        const std::string generate =
            Quote(FFMPEG_EXECUTABLE) +
            " -hide_banner -loglevel error -f lavfi "
            "-i 'color=c=black:s=64x32:r=25:d=0.4' -c:v mpeg4 " +
            Quote(path);
        Check(std::system(generate.c_str()) == 0,
              "FFmpeg génère le rush de test " + path.filename().string());
    }

    std::string projectPath;
    Check(CreatePortableProject((root / "Resolve.cutmachine-project").string(),
                                Project("Resolve"), projectPath, error),
          "le paquet projet se crée : " + error);

    const std::filesystem::path manifestPath = root / "manifest.json";
    {
        std::ofstream out(manifestPath);
        out << R"({"schema":"cutmachine.resolve-manifest.v1",)"
            << R"("project":"Resolve","bins":[)"
            << R"({"key":"b1","name":"1_RUSHES","parent_key":""},)"
            << R"({"key":"b2","name":"Rosie","parent_key":"b1"}],)"
            << R"("clips":[{"path":")" << first.string()
            << R"(","name":"C8015.MP4","bin_key":"b2"},)"
            << R"({"path":")" << second.string()
            << R"(","name":"C8035.MP4","bin_key":"b1"}]})";
    }

    std::string output;
    Check(ImportResolveCommand(projectPath, manifestPath.string(), output) == 0,
          "l'import Resolve réussit : " + output);
    Check(output.find("\"added\":2") != std::string::npos &&
              output.find("\"bins_created\":2") != std::string::npos,
          "l'import rapporte deux rushes et deux chutiers : " + output);

    Project reloaded;
    Check(LoadStoredProject(projectPath, reloaded, error),
          "le projet se recharge : " + error);
    Document stored = reloaded.MakeActiveDocument();
    Check(stored.library.size() == 2,
          "les deux rushes sont dans la médiathèque");
    const DocumentBin* rushes = FindBin(stored, "1_RUSHES");
    const DocumentBin* rosie = FindBin(stored, "Rosie");
    Check(rushes != nullptr && rosie != nullptr, "les deux chutiers existent");
    if (rushes != nullptr && rosie != nullptr) {
        Check(rushes->parent_id.empty() && rosie->parent_id == rushes->id,
              "la hiérarchie Resolve est reproduite");
        for (const LibraryMedia& item : stored.library) {
            if (item.filename == "C8015.MP4")
                Check(item.bin_id == rosie->id,
                      "le rush du sous-chutier est classé dans Rosie");
            if (item.filename == "C8035.MP4")
                Check(item.bin_id == rushes->id,
                      "le rush du chutier parent est classé dans 1_RUSHES");
        }
        Check(stored.library[0].rate.den != 0 &&
                  stored.library[0].duration.rate != 0,
              "la cadence vient de la sonde FFmpeg, pas du manifeste");
    }

    // Re-importing the same manifest must be a no-op, not a duplication.
    std::string secondOutput;
    Check(ImportResolveCommand(projectPath, manifestPath.string(),
                               secondOutput) == 0,
          "le second import réussit : " + secondOutput);
    Check(secondOutput.find("\"added\":0") != std::string::npos &&
              secondOutput.find("\"bins_created\":0") != std::string::npos &&
              secondOutput.find("\"bins_reused\":2") != std::string::npos,
          "le second import ne crée rien : " + secondOutput);
    Project again;
    Check(LoadStoredProject(projectPath, again, error),
          "le projet se recharge après le second import : " + error);
    Check(again.MakeActiveDocument().library.size() == 2 &&
              again.MakeActiveDocument().bins.size() == 2,
          "aucun doublon après un second import");

    // A rush whose file is gone must be named, not just counted: on a Media
    // Pool of hundreds, "skipped: 3" is not a report.
    const std::filesystem::path offlinePath = root / "offline.json";
    {
        std::ofstream out(offlinePath);
        out << R"({"schema":"cutmachine.resolve-manifest.v1","clips":[)"
            << R"({"path":")" << (media / "C9999.MP4").string()
            << R"(","name":"C9999.MP4"}]})";
    }
    std::string offlineOutput;
    Check(ImportResolveCommand(projectPath, offlinePath.string(),
                               offlineOutput) == 0,
          "un rush hors ligne n'arrête pas le lot : " + offlineOutput);
    Check(offlineOutput.find("\"skipped\":1") != std::string::npos &&
              offlineOutput.find("C9999.MP4") != std::string::npos,
          "le rush hors ligne est nommé dans errors : " + offlineOutput);
    Check(secondOutput.find("\"errors\":[]") != std::string::npos,
          "un rush déjà présent n'est pas rapporté comme erreur : " +
              secondOutput);

    std::string missing;
    Check(ImportResolveCommand(projectPath, (root / "absent.json").string(),
                               missing) != 0,
          "un manifeste absent échoue");

    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
    if (failures == 0) std::cout << "resolve import tests passed\n";
    return failures == 0 ? 0 : 1;
}
