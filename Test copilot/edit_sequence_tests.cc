// tests/edit_sequence_tests.cc
//
// Tests de séquences d'opérations générées (stateful testing).
//
// Principe : générer des séquences aléatoires mais reproductibles d'opérations
// valides sur un Document, et vérifier des invariants à chaque étape :
//
//   I1  document.Validate() == true après chaque Apply réussi
//   I2  tous les IDs sont uniques dans le document
//   I3  aucune durée de clip n'est nulle ou négative
//   I4  Undo ramène exactement à l'état précédent (byte-identical)
//   I5  Redo ramène exactement à l'état post-Apply (byte-identical)
//   I6  la sérialisation est idempotente : Serialize(Deserialize(s)) == s
//   I7  N Apply puis N Undo ramène à l'état initial
//   I8  la pile d'opérations est cohérente avec AppliedCount/UndoneCount
//
// Chaque sous-test utilise une graine fixe (LCG) pour être reproductible,
// mais différente entre sous-tests pour couvrir des territoires variés.

#include "Document.h"
#include "EditLog.h"
#include "Operations.h"
#include "Project.h"
#include "RationalTime.h"
#include "Ulid.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// -----------------------------------------------------------------------
// Infrastructure de test (même style que le reste de la suite)
// -----------------------------------------------------------------------

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
// Mini LCG déterministe (pas de dépendance <random>)
// -----------------------------------------------------------------------
struct Lcg {
    explicit Lcg(uint64_t seed) : state_(seed) {}
    uint64_t Next() {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return state_;
    }
    int64_t NextInt64(int64_t lo, int64_t hi) {
        if (lo >= hi) return lo;
        return lo + static_cast<int64_t>(Next() % static_cast<uint64_t>(hi - lo));
    }
    bool NextBool() { return Next() % 2 == 0; }
    size_t NextIndex(size_t size) {
        if (size == 0) return 0;
        return static_cast<size_t>(Next() % size);
    }
};

// -----------------------------------------------------------------------
// Invariants vérifiables sur Document
// -----------------------------------------------------------------------

bool AllIdsUnique(const Document& doc, std::string& reason) {
    std::vector<std::string> ids;
    for (const auto& track : doc.sequence.tracks) {
        ids.push_back(track.id);
        for (const auto& clip : track.clips) {
            ids.push_back(clip.id);
        }
    }
    for (const auto& marker : doc.sequence.markers) {
        ids.push_back(marker.id);
    }
    std::sort(ids.begin(), ids.end());
    const auto dup = std::adjacent_find(ids.begin(), ids.end());
    if (dup != ids.end()) {
        reason = "ID dupliqué : " + *dup;
        return false;
    }
    return true;
}

bool NoDegenerateClip(const Document& doc, std::string& reason) {
    for (const auto& track : doc.sequence.tracks) {
        for (const auto& clip : track.clips) {
            if (clip.duration <= RationalTime{0, clip.duration.rate}) {
                reason = "clip " + clip.id + " durée <= 0";
                return false;
            }
        }
    }
    return true;
}

void CheckInvariants(const Document& doc, const std::string& context) {
    std::string error;
    Check(doc.Validate(error), context + " : Validate() : " + error);
    Check(AllIdsUnique(doc, error), context + " : IDs uniques : " + error);
    Check(NoDegenerateClip(doc, error), context + " : clips valides : " + error);
}

// -----------------------------------------------------------------------
// Générateur d'opérations valides
// Retourne {ok, operation} où ok == false si le document est trop vide
// pour que l'opération soit applicable.
// -----------------------------------------------------------------------

struct GeneratedOp {
    bool applicable = false;
    Operation op;
};

// Rates réalistes
const int32_t kRate = 25;  // frames @ 25fps pour toute la suite

Document MakeBaseDocument() {
    Document doc;
    doc.sequence.name = "test";
    doc.sequence.frame_rate = {25, 1};
    doc.sequence.width = 1920;
    doc.sequence.height = 1080;
    return doc;
}

// Ajouter une piste vidéo (toujours applicable)
GeneratedOp GenAddTrack(Lcg& rng) {
    AddTrackOperation op;
    op.kind = rng.NextBool() ? "video" : "audio";
    op.index = 0;
    op.id = GenerateUlid();
    return {true, op};
}

// Ajouter un clip sur une piste existante
GeneratedOp GenAddClip(const Document& doc, Lcg& rng) {
    if (doc.sequence.tracks.empty()) return {false, {}};
    const size_t ti = rng.NextIndex(doc.sequence.tracks.size());
    const auto& track = doc.sequence.tracks[ti];

    // Trouver un timeline_in qui ne chevauche pas les clips existants
    // Stratégie simple : mettre après le dernier clip de la piste
    int64_t latestEnd = 0;
    for (const auto& clip : track.clips) {
        const int64_t end = clip.timeline_in.add(clip.duration).value;
        if (end > latestEnd) latestEnd = end;
    }
    // Durée : 24-200 frames
    const int64_t dur = rng.NextInt64(24, 201);
    const int64_t gap = rng.NextInt64(0, 25);  // 0-24 frames de gap

    AddClipOperation op;
    op.track_id = track.id;
    op.id = GenerateUlid();
    op.source_id = GenerateUlid();
    op.source_in = {0, kRate};
    op.timeline_in = {latestEnd + gap, kRate};
    op.duration = {dur, kRate};
    return {true, op};
}

// Ajouter un marqueur
GeneratedOp GenAddMarker(const Document& doc, Lcg& rng) {
    DocumentMarker marker;
    marker.id = GenerateUlid();
    marker.name = "M" + std::to_string(rng.Next() % 1000);
    marker.time = {rng.NextInt64(0, 3600), kRate};
    AddMarkerOperation op;
    op.marker = marker;
    return {true, op};
}

// Supprimer un clip
GeneratedOp GenRemoveClip(const Document& doc, Lcg& rng) {
    // Collecter tous les clips
    std::vector<std::string> clipIds;
    for (const auto& track : doc.sequence.tracks)
        for (const auto& clip : track.clips)
            clipIds.push_back(clip.id);
    if (clipIds.empty()) return {false, {}};
    const std::string id = clipIds[rng.NextIndex(clipIds.size())];
    RemoveClipOperation op;
    op.clip_id = id;
    return {true, op};
}

// Supprimer un marqueur
GeneratedOp GenRemoveMarker(const Document& doc, Lcg& rng) {
    if (doc.sequence.markers.empty()) return {false, {}};
    const size_t i = rng.NextIndex(doc.sequence.markers.size());
    RemoveMarkerOperation op;
    op.marker_id = doc.sequence.markers[i].id;
    return {true, op};
}

// Choisir une opération aléatoire parmi les applicables
GeneratedOp PickOperation(const Document& doc, Lcg& rng) {
    const int choice = static_cast<int>(rng.Next() % 5);
    switch (choice) {
        case 0: return GenAddTrack(rng);
        case 1: return GenAddClip(doc, rng);
        case 2: return GenAddMarker(doc, rng);
        case 3: return GenRemoveClip(doc, rng);
        case 4: return GenRemoveMarker(doc, rng);
        default: return {false, {}};
    }
}

// -----------------------------------------------------------------------
// Tests principaux
// -----------------------------------------------------------------------

}  // namespace

int main() {
    // ------------------------------------------------------------------
    // T1 : invariants après chaque Apply
    // ------------------------------------------------------------------
    Test("I1-I3 : invariants préservés après N Apply valides", [] {
        Lcg rng(0xDEADBEEF42ULL);
        Document doc = MakeBaseDocument();
        EditLog log;

        // D'abord on force quelques pistes pour avoir quelque chose à éditer
        for (int i = 0; i < 3; ++i) {
            GeneratedOp g = GenAddTrack(rng);
            EditError err = EditError::None;
            std::string msg;
            log.Apply(doc, g.op, err, msg);
        }

        int applied = 0;
        for (int step = 0; step < 200; ++step) {
            GeneratedOp g = PickOperation(doc, rng);
            if (!g.applicable) continue;

            const std::string docBefore = doc.SaveToString();
            EditError err = EditError::None;
            std::string msg;
            const bool ok = log.Apply(doc, g.op, err, msg);

            if (ok) {
                ++applied;
                CheckInvariants(doc, "step " + std::to_string(step));
            } else {
                // Opération rejetée : le document doit être inchangé
                Check(doc.SaveToString() == docBefore,
                      "Apply raté a muté le document (step " +
                          std::to_string(step) + ") : " + msg);
            }
        }
        // On doit avoir au moins appliqué quelques opérations
        Check(applied >= 5, "trop peu d'opérations applicables : " +
                                 std::to_string(applied));
    });

    // ------------------------------------------------------------------
    // T2 : Undo restaure exactement l'état précédent (byte-identical)
    // ------------------------------------------------------------------
    Test("I4 : Undo restaure l'état précédent byte-identical", [] {
        Lcg rng(0xCAFEBABE99ULL);
        Document doc = MakeBaseDocument();
        EditLog log;

        // Construire un document de base
        for (int i = 0; i < 2; ++i) {
            auto g = GenAddTrack(rng);
            EditError e = EditError::None; std::string m;
            log.Apply(doc, g.op, e, m);
        }

        for (int round = 0; round < 50; ++round) {
            const std::string stateBefore = doc.SaveToString();
            const size_t countBefore = log.AppliedCount();

            auto g = PickOperation(doc, rng);
            if (!g.applicable) continue;

            EditError err = EditError::None;
            std::string msg;
            if (!log.Apply(doc, g.op, err, msg)) continue;

            Check(log.AppliedCount() == countBefore + 1,
                  "AppliedCount n'a pas augmenté après Apply");

            // Undo
            EditError undoErr = EditError::None;
            std::string undoMsg;
            const bool undoOk = log.Undo(doc, undoErr, undoMsg);
            Check(undoOk, "Undo a échoué : " + undoMsg);
            if (undoOk) {
                Check(doc.SaveToString() == stateBefore,
                      "Undo round " + std::to_string(round) +
                          " n'a pas restauré l'état byte-identical");
                Check(log.AppliedCount() == countBefore,
                      "AppliedCount incorrect après Undo");
            }
        }
    });

    // ------------------------------------------------------------------
    // T3 : Redo restaure l'état post-Apply (byte-identical)
    // ------------------------------------------------------------------
    Test("I5 : Redo restaure l'état post-Apply byte-identical", [] {
        Lcg rng(0x1234567890ABULL);
        Document doc = MakeBaseDocument();
        EditLog log;

        for (int i = 0; i < 2; ++i) {
            auto g = GenAddTrack(rng);
            EditError e = EditError::None; std::string m;
            log.Apply(doc, g.op, e, m);
        }

        for (int round = 0; round < 50; ++round) {
            auto g = PickOperation(doc, rng);
            if (!g.applicable) continue;

            EditError err = EditError::None;
            std::string msg;
            if (!log.Apply(doc, g.op, err, msg)) continue;

            const std::string stateAfterApply = doc.SaveToString();

            // Undo
            EditError undoErr = EditError::None; std::string undoMsg;
            if (!log.Undo(doc, undoErr, undoMsg)) continue;

            // Redo
            EditError redoErr = EditError::None; std::string redoMsg;
            const bool redoOk = log.Redo(doc, redoErr, redoMsg);
            Check(redoOk, "Redo a échoué : " + redoMsg);
            if (redoOk) {
                Check(doc.SaveToString() == stateAfterApply,
                      "Redo round " + std::to_string(round) +
                          " n'a pas restauré l'état post-Apply");
            }
        }
    });

    // ------------------------------------------------------------------
    // T4 : sérialisation idempotente après N opérations
    // ------------------------------------------------------------------
    Test("I6 : Serialize(Deserialize(s)) == s après N opérations", [] {
        Lcg rng(0xFEEDFACE11ULL);
        Document doc = MakeBaseDocument();
        EditLog log;

        for (int i = 0; i < 2; ++i) {
            auto g = GenAddTrack(rng);
            EditError e = EditError::None; std::string m;
            log.Apply(doc, g.op, e, m);
        }

        for (int step = 0; step < 80; ++step) {
            auto g = PickOperation(doc, rng);
            if (!g.applicable) continue;
            EditError err = EditError::None; std::string msg;
            if (!log.Apply(doc, g.op, err, msg)) continue;

            // Sérialisation du log
            const std::string s1 = log.Serialize();
            EditLog reloaded;
            EditError loadErr = EditError::None;
            std::string loadMsg;
            const bool loaded = EditLog::Deserialize(s1, reloaded, loadErr, loadMsg);
            Check(loaded, "Deserialize a échoué step " + std::to_string(step) +
                              " : " + loadMsg);
            if (loaded) {
                const std::string s2 = reloaded.Serialize();
                Check(s1 == s2,
                      "Serialize non idempotent step " + std::to_string(step));
            }
        }
    });

    // ------------------------------------------------------------------
    // T5 : N Apply puis N Undo => état initial (byte-identical)
    // ------------------------------------------------------------------
    Test("I7 : N Apply + N Undo => retour à l'état initial", [] {
        Lcg rng(0xABCDEF0123ULL);
        Document doc = MakeBaseDocument();
        EditLog log;

        // Construire un document non trivial
        for (int i = 0; i < 3; ++i) {
            auto g = GenAddTrack(rng);
            EditError e = EditError::None; std::string m;
            log.Apply(doc, g.op, e, m);
        }
        for (int i = 0; i < 5; ++i) {
            auto g = GenAddClip(doc, rng);
            if (!g.applicable) continue;
            EditError e = EditError::None; std::string m;
            log.Apply(doc, g.op, e, m);
        }

        const std::string initialState = doc.SaveToString();
        const size_t initialApplied = log.AppliedCount();

        // Appliquer jusqu'à 30 opérations supplémentaires
        std::vector<std::string> states;
        states.push_back(initialState);
        int extraApplied = 0;

        for (int step = 0; step < 60 && extraApplied < 30; ++step) {
            auto g = PickOperation(doc, rng);
            if (!g.applicable) continue;
            EditError err = EditError::None; std::string msg;
            if (!log.Apply(doc, g.op, err, msg)) continue;
            ++extraApplied;
            states.push_back(doc.SaveToString());
        }

        // Undo de tout ce qui a été appliqué en plus
        for (int i = extraApplied - 1; i >= 0; --i) {
            EditError err = EditError::None; std::string msg;
            const bool ok = log.Undo(doc, err, msg);
            Check(ok, "Undo #" + std::to_string(i) + " a échoué : " + msg);
            if (ok) {
                Check(doc.SaveToString() == states[static_cast<size_t>(i)],
                      "Undo #" + std::to_string(i) +
                          " : état différent de l'état attendu");
            }
        }

        Check(log.AppliedCount() == initialApplied,
              "AppliedCount incorrect après N Undo");
        Check(doc.SaveToString() == initialState,
              "N Apply + N Undo ne restaure pas l'état initial");
    });

    // ------------------------------------------------------------------
    // T6 : AppliedCount et UndoneCount sont cohérents
    // ------------------------------------------------------------------
    Test("I8 : AppliedCount et UndoneCount cohérents avec Apply/Undo/Redo", [] {
        Lcg rng(0x9999DEADULL);
        Document doc = MakeBaseDocument();
        EditLog log;

        {
            auto g = GenAddTrack(rng);
            EditError e = EditError::None; std::string m;
            log.Apply(doc, g.op, e, m);
        }

        size_t applied = log.AppliedCount();
        size_t undone  = log.UndoneCount();

        Check(applied + undone == log.AppliedCount() + log.UndoneCount(),
              "invariant compteurs initial");

        for (int step = 0; step < 100; ++step) {
            const bool doUndo = rng.NextBool() && applied > 0;
            const bool doRedo = !doUndo && rng.NextBool() && undone > 0;

            if (doUndo) {
                EditError e = EditError::None; std::string m;
                if (log.Undo(doc, e, m)) {
                    Check(log.AppliedCount() == applied - 1,
                          "AppliedCount incorrect après Undo");
                    Check(log.UndoneCount() == undone + 1,
                          "UndoneCount incorrect après Undo");
                    applied = log.AppliedCount();
                    undone  = log.UndoneCount();
                }
            } else if (doRedo) {
                EditError e = EditError::None; std::string m;
                if (log.Redo(doc, e, m)) {
                    Check(log.AppliedCount() == applied + 1,
                          "AppliedCount incorrect après Redo");
                    Check(log.UndoneCount() == undone - 1,
                          "UndoneCount incorrect après Redo");
                    applied = log.AppliedCount();
                    undone  = log.UndoneCount();
                }
            } else {
                auto g = PickOperation(doc, rng);
                if (!g.applicable) continue;
                EditError e = EditError::None; std::string m;
                if (log.Apply(doc, g.op, e, m)) {
                    Check(log.AppliedCount() == applied + 1,
                          "AppliedCount incorrect après Apply");
                    // Apply vide la pile Undo
                    Check(log.UndoneCount() == 0,
                          "UndoneCount doit être 0 après Apply");
                    applied = log.AppliedCount();
                    undone  = log.UndoneCount();
                }
            }
        }
    });

    // ------------------------------------------------------------------
    // T7 : Save/Load round-trip après une séquence d'opérations
    // ------------------------------------------------------------------
    Test("Document Save+Load round-trip après N opérations", [] {
        Lcg rng(0xBAD1DEAULL);
        Document doc = MakeBaseDocument();
        EditLog log;

        for (int i = 0; i < 2; ++i) {
            auto g = GenAddTrack(rng);
            EditError e = EditError::None; std::string m;
            log.Apply(doc, g.op, e, m);
        }

        for (int step = 0; step < 40; ++step) {
            auto g = PickOperation(doc, rng);
            if (!g.applicable) continue;
            EditError err = EditError::None; std::string msg;
            log.Apply(doc, g.op, err, msg);
        }

        // Sauvegarder via SaveToString
        const std::string saved = doc.SaveToString();

        // Recharger
        Document loaded;
        std::string loadError;
        const bool ok = Document::LoadFromString(saved, loaded, loadError);
        Check(ok, "LoadFromString a échoué : " + loadError);
        if (ok) {
            const std::string resaved = loaded.SaveToString();
            Check(resaved == saved,
                  "Document Save/Load non idempotent : "
                  "octets différents après rechargement");
        }
    });

    if (failures != 0) {
        std::cerr << failures << " test(s) de séquences d'opérations échoué(s)\n";
        return 1;
    }
    std::cout << "Tous les tests de séquences d'opérations passent\n";
    return 0;
}
