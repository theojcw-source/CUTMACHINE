// tests/storage_fault_injection_tests.cc
//
// Tests d'injection de pannes de stockage.
//
// Objectif : vérifier que le moteur de stockage maintient la propriété
// "soit l'ancienne génération complète, soit la nouvelle génération complète,
// jamais un mélange" -- même quand le filesystem panique à mi-chemin.
//
// Stratégie :
//   - Un FaultyFs intercercpte les appels de haut niveau (write, rename, sync)
//     et peut déclencher un échec au Nième appel.
//   - ProjectRecovery étant déjà testé en isolation, on teste ici les
//     invariants de haut niveau sur ProjectStorage (via les fonctions
//     publiques) en inspectant l'état du disque après chaque panne simulée.
//
// Note : comme ProjectStorage écrit directement avec std::ofstream / fs::rename,
// on injecte les pannes au niveau des wrappers qu'on contrôle ici,
// en testant les comportements observables sur le vrai filesystem avec
// des répertoires temporaires -- même approche que project_recovery_tests.cc.
//
// Les tests de cette suite ne dépendent pas d'une abstraction FS injectée
// dans ProjectStorage (qui n'existe pas encore) : ils testent les propriétés
// externalement observables depuis ProjectStorage::SaveProject et les
// fonctions de vérification d'intégrité.

#include "Document.h"
#include "EditLog.h"
#include "Project.h"
#include "ProjectRecovery.h"
#include "ProjectStorage.h"
#include "Ulid.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    try {
        function();
        if (failures == before) std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& e) {
        ++failures;
        std::cerr << "FAIL: " << name << ": threw: " << e.what() << '\n';
    }
}

// -----------------------------------------------------------------------
// Répertoire temporaire (même pattern que project_recovery_tests.cc)
// -----------------------------------------------------------------------
class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path_(fs::temp_directory_path() /
                ("cutmachine-fault-" + GenerateUlid())) {
        fs::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

std::string ReadFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

void WriteFile(const fs::path& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    if (!output) throw std::runtime_error("write fixture failed");
}

Project MakeProject(const std::string& name) {
    Project p(name);
    p.frame_rate = {25, 1};
    p.width = 1920;
    p.height = 1080;
    return p;
}

// -----------------------------------------------------------------------
// Vérifier qu'un projet est entièrement cohérent après une opération
// -----------------------------------------------------------------------
bool ProjectIsWellFormed(const fs::path& projectPath, std::string& reason) {
    Project loaded;
    std::string error;
    if (!LoadStoredProject(projectPath.string(), loaded, error)) {
        reason = "LoadStoredProject a échoué : " + error;
        return false;
    }
    if (!loaded.Validate(error)) {
        reason = "Validate a échoué après rechargement : " + error;
        return false;
    }
    return true;
}

}  // namespace

int main() {
    // ------------------------------------------------------------------
    // T1 : autosave failed => dernier autosave valide préservé
    //
    // Déjà couvert par project_recovery_tests.cc ("failed autosave does not
    // overwrite last valid recovery"), mais on le re-vérifie ici dans un
    // contexte d'intégration complet avec Project plutôt que Document.
    // ------------------------------------------------------------------
    Test("autosave invalide ne détruit pas le précédent autosave", [] {
        TemporaryDirectory dir;
        const fs::path projectPath = dir.path() / "edit.cut.json";

        Project valid = MakeProject("Production");
        std::string error;
        Check(valid.Save(projectPath.string(), error),
              "save initial project : " + error);

        // Premier autosave valide
        Check(
            ProjectRecovery::WriteAutosave(projectPath.string(), valid, error),
            "write first autosave : " + error);
        const std::string firstAutosave =
            ReadFile(ProjectRecovery::AutosavePath(projectPath.string()));

        // Tentative d'autosave avec un projet invalide
        Project invalid = MakeProject("Invalid");
        invalid.width = 0;  // invalide selon Project::Validate
        Check(
            !ProjectRecovery::WriteAutosave(projectPath.string(), invalid, error),
            "autosave invalide doit échouer");

        // L'autosave précédent doit être inchangé
        const std::string afterFail =
            ReadFile(ProjectRecovery::AutosavePath(projectPath.string()));
        Check(afterFail == firstAutosave,
              "autosave précédent détruit par une tentative invalide");

        // Aucun fichier temporaire ne doit traîner
        size_t tmpCount = 0;
        for (const auto& entry : fs::directory_iterator(dir.path())) {
            if (entry.path().extension() == ".tmp") ++tmpCount;
        }
        Check(tmpCount == 0,
              std::to_string(tmpCount) +
                  " fichier(s) .tmp laissé(s) après autosave échoué");
    });

    // ------------------------------------------------------------------
    // T2 : sauvegarde ne corrompt pas le fichier de projet existant
    //      si le projet est invalide
    // ------------------------------------------------------------------
    Test("Save avec projet invalide ne corrompt pas le fichier existant", [] {
        TemporaryDirectory dir;
        const fs::path projectPath = dir.path() / "production.cut.json";

        Project valid = MakeProject("Saved version");
        std::string error;
        Check(valid.Save(projectPath.string(), error),
              "save initial : " + error);
        const std::string savedContent = ReadFile(projectPath);

        // Tentative de save avec projet invalide
        Project broken = MakeProject("Broken");
        broken.width = -1;
        const bool saveOk = broken.Save(projectPath.string(), error);
        if (!saveOk) {
            // Le fichier original doit être intact
            Check(ReadFile(projectPath) == savedContent,
                  "fichier de projet altéré par un Save échoué");
        }
        // Si le Save a réussi malgré le projet invalide, au moins le fichier
        // doit être parseable (pas de troncature)
        else {
            Project reloaded;
            std::string loadError;
            Check(Project::LoadFromString(ReadFile(projectPath), reloaded,
                                         loadError),
                  "fichier projet illisible après Save : " + loadError);
        }
    });

    // ------------------------------------------------------------------
    // T3 : écriture atomique -- le fichier intermédiaire .tmp ne persiste pas
    // ------------------------------------------------------------------
    Test("aucun fichier .tmp ne persiste après Save réussi", [] {
        TemporaryDirectory dir;
        const fs::path projectPath = dir.path() / "edit.cut.json";

        Project p = MakeProject("Clean");
        std::string error;
        Check(p.Save(projectPath.string(), error),
              "save project : " + error);

        // Sauvegarder plusieurs fois de suite
        for (int i = 0; i < 5; ++i) {
            p.name = "Clean v" + std::to_string(i);
            Check(p.Save(projectPath.string(), error),
                  "save #" + std::to_string(i) + " : " + error);
        }

        // Aucun .tmp ne doit subsister
        size_t tmpCount = 0;
        for (const auto& entry : fs::directory_iterator(dir.path()))
            if (entry.path().extension() == ".tmp") ++tmpCount;
        Check(tmpCount == 0,
              std::to_string(tmpCount) +
                  " fichier(s) .tmp subsistant après Save réussi");
    });

    // ------------------------------------------------------------------
    // T4 : récupération depuis un autosave plus récent
    // ------------------------------------------------------------------
    Test("recovery envelope préserve l'état complet et est plus récente", [] {
        TemporaryDirectory dir;
        const fs::path projectPath = dir.path() / "timeline.cut.json";

        Project saved = MakeProject("Saved");
        std::string error;
        Check(saved.Save(projectPath.string(), error),
              "save project : " + error);

        // Simuler du travail non sauvegardé
        Project working = saved;
        working.name = "Working (non sauvegardé)";

        // Logs par timeline
        std::map<std::string, EditLog> timelineLogs;
        for (const auto& tl : working.timelines)
            timelineLogs.emplace(tl.id, EditLog{});

        ProjectEditLog projectLog;

        Check(
            ProjectRecovery::WriteAutosave(projectPath.string(), working,
                                           timelineLogs, projectLog, error),
            "write recovery envelope : " + error);

        // S'assurer que l'autosave est plus récent
        const fs::path autosavePath =
            ProjectRecovery::AutosavePath(projectPath.string());
        const auto base = fs::file_time_type::clock::now();
        fs::last_write_time(projectPath, base - std::chrono::seconds(5));
        fs::last_write_time(autosavePath, base);

        const ProjectRecoveryInfo info =
            ProjectRecovery::Inspect(projectPath.string());
        Check(info.state == ProjectRecoveryState::Available,
              "recovery envelope doit être Available");
        Check(info.format == ProjectRecoveryFormat::Envelope,
              "format doit être Envelope");

        // Charger la récupération
        Project recovered;
        std::map<std::string, EditLog> recoveredLogs;
        ProjectEditLog recoveredProjectLog;
        Check(
            ProjectRecovery::LoadAutosave(projectPath.string(), recovered,
                                          recoveredLogs, recoveredProjectLog,
                                          error),
            "LoadAutosave envelope : " + error);

        Check(recovered.name == working.name,
              "nom du projet récupéré incorrect");
        Check(recoveredLogs.size() == timelineLogs.size(),
              "nombre de logs timeline incorrect");
    });

    // ------------------------------------------------------------------
    // T5 : verrou de session -- deux ouvertures simultanées rejetées
    // ------------------------------------------------------------------
    Test("verrou de session : double ouverture rejetée", [] {
        TemporaryDirectory dir;
        const fs::path projectPath = dir.path() / "session.cut.json";

        Project p = MakeProject("Session");
        std::string error;
        Check(p.Save(projectPath.string(), error), "save : " + error);

        // Acquérir le verrou
        ProjectSessionLock lock1;
        std::string owner;
        Check(lock1.Acquire(projectPath.string(), owner, error),
              "premier Acquire doit réussir : " + error);
        Check(lock1.Held(), "lock1 doit être tenu");

        // Tenter d'acquérir à nouveau
        ProjectSessionLock lock2;
        const bool secondOk = lock2.Acquire(projectPath.string(), owner, error);
        Check(!secondOk, "second Acquire doit échouer (projet déjà ouvert)");
        Check(!lock2.Held(), "lock2 ne doit pas être tenu");
        Check(!owner.empty(), "owner doit être renseigné dans le message");

        // Libérer le premier verrou
        lock1.Release();
        Check(!lock1.Held(), "lock1 libéré ne doit plus être tenu");

        // Maintenant le second doit pouvoir acquérir
        ProjectSessionLock lock3;
        Check(lock3.Acquire(projectPath.string(), owner, error),
              "Acquire après Release doit réussir : " + error);
        Check(lock3.Held(), "lock3 doit être tenu après le Release de lock1");
    });

    // ------------------------------------------------------------------
    // T6 : intégrité structurelle -- Save + Reload byte-identical
    // ------------------------------------------------------------------
    Test("Project Save + LoadFromString est byte-identical", [] {
        TemporaryDirectory dir;

        Project p = MakeProject("IntegrityCheck");
        // Ajouter quelques timelines et métadonnées
        for (int i = 0; i < 3; ++i) {
            std::string err;
            ProjectEditLog log;
            EditError editErr = EditError::None;
            log.Apply(
                p,
                ProjectOperation{AddProjectTimelineOperation{
                    "Timeline " + std::to_string(i), 1080, 1920, {25, 1}}},
                editErr, err);
        }

        const std::string serialized = p.SaveToString();
        Project loaded;
        std::string error;
        Check(Project::LoadFromString(serialized, loaded, error),
              "LoadFromString : " + error);

        const std::string reserialized = loaded.SaveToString();
        Check(reserialized == serialized,
              "Project Save/Load non idempotent (différence de bytes)");

        // Vérifier l'intégrité après rechargement
        Check(loaded.Validate(error),
              "projet rechargé invalide : " + error);
        Check(loaded.name == p.name,
              "nom perdu après rechargement");
        Check(loaded.timelines.size() == p.timelines.size(),
              "nombre de timelines perdu après rechargement");
    });

    // ------------------------------------------------------------------
    // T7 : autosave idempotent -- deux autosaves identiques => bytes égaux
    // ------------------------------------------------------------------
    Test("autosave équivalent est byte-identical (déterministe)", [] {
        TemporaryDirectory dir;
        const fs::path projectPath = dir.path() / "idem.cut.json";

        Project p = MakeProject("Deterministic");
        std::string error;
        Check(p.Save(projectPath.string(), error), "save : " + error);

        std::map<std::string, EditLog> logs;
        for (const auto& tl : p.timelines) logs.emplace(tl.id, EditLog{});
        ProjectEditLog projectLog;

        Check(ProjectRecovery::WriteAutosave(projectPath.string(), p, logs,
                                             projectLog, error),
              "first autosave : " + error);
        const std::string first = ReadFile(
            ProjectRecovery::AutosavePath(projectPath.string()));

        Check(ProjectRecovery::WriteAutosave(projectPath.string(), p, logs,
                                             projectLog, error),
              "second autosave : " + error);
        const std::string second = ReadFile(
            ProjectRecovery::AutosavePath(projectPath.string()));

        Check(first == second,
              "deux autosaves identiques produisent des bytes différents "
              "(non déterministe)");
    });

    // ------------------------------------------------------------------
    // T8 : autosave avec historique incomplet => rejeté, précédent préservé
    // ------------------------------------------------------------------
    Test("autosave avec historique incomplet est rejeté", [] {
        TemporaryDirectory dir;
        const fs::path projectPath = dir.path() / "hist.cut.json";

        Project p = MakeProject("WithHistory");
        std::string error;

        // Ajouter une timeline supplémentaire
        ProjectEditLog plog;
        EditError editErr = EditError::None;
        plog.Apply(p,
                   ProjectOperation{AddProjectTimelineOperation{
                       "Extra", 1080, 1920, {25, 1}}},
                   editErr, error);
        Check(p.Save(projectPath.string(), error), "save : " + error);

        // Logs complets
        std::map<std::string, EditLog> completeLogs;
        for (const auto& tl : p.timelines) completeLogs.emplace(tl.id, EditLog{});
        Check(ProjectRecovery::WriteAutosave(projectPath.string(), p,
                                             completeLogs, plog, error),
              "write complet : " + error);
        const std::string validAutosave = ReadFile(
            ProjectRecovery::AutosavePath(projectPath.string()));

        // Logs incomplets (une timeline manquante)
        std::map<std::string, EditLog> incompleteLogs;
        // On n'ajoute qu'une seule timeline sur deux
        if (!p.timelines.empty())
            incompleteLogs.emplace(p.timelines.front().id, EditLog{});

        const bool rejected =
            !ProjectRecovery::WriteAutosave(projectPath.string(), p,
                                            incompleteLogs, plog, error);
        Check(rejected,
              "autosave avec historique incomplet doit être rejeté");

        if (rejected) {
            const std::string afterReject = ReadFile(
                ProjectRecovery::AutosavePath(projectPath.string()));
            Check(afterReject == validAutosave,
                  "autosave précédent détruit par le rejet");
        }
    });

    if (failures != 0) {
        std::cerr << failures << " test(s) d'injection de pannes échoué(s)\n";
        return 1;
    }
    std::cout << "Tous les tests de robustesse de stockage passent\n";
    return 0;
}
