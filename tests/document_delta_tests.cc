#include "Document.h"
#include "DocumentDelta.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

DocumentClip Clip(const std::string& id, int64_t timelineIn, int64_t duration) {
    DocumentClip clip;
    clip.id = id;
    clip.source_id = "01K30000000000000000000001";
    clip.source_in = RationalTime{0, 25};
    clip.duration = RationalTime{duration, 25};
    clip.timeline_in = RationalTime{timelineIn, 25};
    clip.include_audio = false;
    return clip;
}

// Trois plans bout à bout sur une piste vidéo : la forme d'un montage.
Document Base() {
    Document document;
    document.sequence.id = "01K300000000000000000000SQ";
    document.sequence.name = "MONTAGE";
    document.sequence.frame_rate = {25, 1};
    document.sequence.width = 1080;
    document.sequence.height = 1920;
    DocumentTrack video;
    video.id = "01K30000000000000000000010";
    video.kind = "video";
    video.index = 0;
    video.clips = {Clip("01K3000000000000000000000A", 0, 100),
                   Clip("01K3000000000000000000000B", 100, 100),
                   Clip("01K3000000000000000000000C", 200, 100)};
    document.sequence.tracks = {video};
    return document;
}

DocumentTrack& Video(Document& document) { return document.sequence.tracks[0]; }

}  // namespace

int main() {
    // --- ClipsEqual : une garde par champ ---------------------------------
    // Si DocumentClip gagne un champ et que ClipsEqual ne le compare pas, le
    // delta sous-déclare en silence. Chaque champ a son cas ici.
    {
        const DocumentClip base = Clip("01K3000000000000000000000A", 0, 100);
        Check(ClipsEqual(base, base), "un clip est égal à lui-même");
        struct Case {
            const char* name;
            DocumentClip mutated;
        };
        std::vector<Case> cases;
        auto with = [&](const char* name, void (*apply)(DocumentClip&)) {
            DocumentClip copy = base;
            apply(copy);
            cases.push_back({name, copy});
        };
        with("id",
             [](DocumentClip& c) { c.id = "01K3000000000000000000000Z"; });
        with("source_id", [](DocumentClip& c) {
            c.source_id = "01K30000000000000000000009";
        });
        with("source_in",
             [](DocumentClip& c) { c.source_in = RationalTime{5, 25}; });
        with("duration",
             [](DocumentClip& c) { c.duration = RationalTime{50, 25}; });
        with("timeline_in",
             [](DocumentClip& c) { c.timeline_in = RationalTime{7, 25}; });
        with("include_audio",
             [](DocumentClip& c) { c.include_audio = !c.include_audio; });
        with("link_group_id", [](DocumentClip& c) {
            c.link_group_id = "01K3000000000000000000000L";
        });
        with("sync_anchor_clip_id", [](DocumentClip& c) {
            c.sync_anchor_clip_id = "01K3000000000000000000000S";
        });
        with("sync_reference_delta", [](DocumentClip& c) {
            c.sync_reference_delta = RationalTime{3, 25};
        });
        with("effects", [](DocumentClip& c) {
            ClipEffect effect;
            effect.id = "01K3000000000000000000000E";
            effect.type = "color.exposure";
            effect.params["value"] = EffectParamValue{1, 2};
            c.effects.push_back(effect);
        });
        with("caption_group_id", [](DocumentClip& c) {
            c.caption_group_id = "01K3000000000000000000000G";
        });
        with("caption_text",
             [](DocumentClip& c) { c.caption_text = "bonjour"; });
        with("opacity",
             [](DocumentClip& c) { c.opacity = EffectParamValue{1, 2}; });
        for (const Case& item : cases)
            Check(!ClipsEqual(base, item.mutated),
                  std::string("un changement de ") + item.name +
                      " rend les clips différents");
        Check(cases.size() == 13,
              "les treize champs de DocumentClip sont couverts (" +
                  std::to_string(cases.size()) + ")");
        // Un effet identique en valeur mais rangé sous une autre clé compte.
        DocumentClip renamed = base;
        ClipEffect effect;
        effect.id = "01K3000000000000000000000E";
        effect.type = "color.exposure";
        effect.params["autre"] = EffectParamValue{1, 2};
        renamed.effects.push_back(effect);
        Check(!ClipsEqual(base, cases.back().mutated) &&
                  !ClipsEqual(renamed, cases[9].mutated),
              "un paramètre d'effet renommé n'est pas la même chose");
    }

    // --- Rien n'a bougé ----------------------------------------------------
    {
        const Document document = Base();
        DocumentDelta delta;
        Check(!ComputeDocumentDelta(document, document, delta),
              "un document identique ne déclare aucun changement");
        Check(delta.clips.empty() && delta.shifted.empty() &&
                  delta.removed_clip_ids.empty(),
              "et le delta est vide");
        Check(delta.duration == RationalTime{300, 25},
              "mais la durée est publiée quand même");
    }

    // --- Un ripple devient une règle, pas une liste -------------------------
    {
        const Document before = Base();
        Document after = before;
        // La tête du premier plan est rognée de 20 images : les deux suivants
        // remontent d'autant. C'est le geste le plus fréquent du montage.
        Video(after).clips[0].source_in = RationalTime{20, 25};
        Video(after).clips[0].duration = RationalTime{80, 25};
        Video(after).clips[1].timeline_in = RationalTime{80, 25};
        Video(after).clips[2].timeline_in = RationalTime{180, 25};
        DocumentDelta delta;
        Check(ComputeDocumentDelta(before, after, delta), "le ripple change");
        Check(delta.clips.size() == 1 &&
                  delta.clips[0].clip_id == "01K3000000000000000000000A" &&
                  delta.clips[0].duration == RationalTime{80, 25},
              "le clip rogné sort en état résultant");
        Check(delta.shifted.size() == 1 && delta.shifted[0].count == 2 &&
                  delta.shifted[0].by == RationalTime{-20, 25} &&
                  delta.shifted[0].from == RationalTime{100, 25},
              "les deux plans déplacés tiennent en une règle signée");
        Check(delta.duration == RationalTime{280, 25},
              "la nouvelle durée est publiée");
    }

    // --- Une règle ne s'invente pas ----------------------------------------
    {
        // Un seul plan bouge, alors qu'un autre le suit sans bouger : aucune
        // règle ne peut décrire ça sans mentir sur le plan resté en place.
        const Document before = Base();
        Document after = before;
        Video(after).clips[1].timeline_in = RationalTime{120, 25};
        DocumentDelta delta;
        Check(ComputeDocumentDelta(before, after, delta),
              "le déplacement change");
        Check(delta.shifted.empty(),
              "un déplacement partiel ne devient pas une règle");
        Check(delta.clips.size() == 1 &&
                  delta.clips[0].timeline_in == RationalTime{120, 25},
              "il sort en clip individuel");
    }
    {
        // Deux plans bougent, de valeurs différentes : pas une règle non plus.
        const Document before = Base();
        Document after = before;
        Video(after).clips[1].timeline_in = RationalTime{110, 25};
        Video(after).clips[2].timeline_in = RationalTime{230, 25};
        DocumentDelta delta;
        ComputeDocumentDelta(before, after, delta);
        Check(delta.shifted.empty() && delta.clips.size() == 2,
              "deux décalages inégaux restent deux clips");
    }

    // --- Création, suppression, pistes, séquence ---------------------------
    {
        const Document before = Base();
        Document after = before;
        Video(after).clips.push_back(
            Clip("01K3000000000000000000000D", 300, 50));
        DocumentDelta delta;
        ComputeDocumentDelta(before, after, delta);
        Check(delta.clips.size() == 1 && delta.clips[0].created,
              "un clip ajouté est marqué créé");
    }
    {
        const Document before = Base();
        Document after = before;
        Video(after).clips.erase(Video(after).clips.begin() + 1);
        DocumentDelta delta;
        ComputeDocumentDelta(before, after, delta);
        Check(delta.removed_clip_ids.size() == 1 &&
                  delta.removed_clip_ids[0] == "01K3000000000000000000000B",
              "un clip retiré sort par son identifiant");
    }
    {
        const Document before = Base();
        Document after = before;
        DocumentTrack overlay;
        overlay.id = "01K30000000000000000000011";
        overlay.kind = "video";
        overlay.index = 2;
        after.sequence.tracks.push_back(overlay);
        DocumentDelta delta;
        ComputeDocumentDelta(before, after, delta);
        Check(delta.created_tracks.size() == 1 &&
                  delta.created_tracks[0].index == 2 &&
                  delta.created_tracks[0].kind == "video",
              "une piste créée sort avec son genre et son index");
        DocumentDelta back;
        ComputeDocumentDelta(after, before, back);
        Check(back.removed_track_ids.size() == 1,
              "et sa suppression sort par identifiant");
    }
    {
        const Document before = Base();
        Document after = before;
        after.sequence.width = 2160;
        after.sequence.height = 3840;
        DocumentDelta delta;
        ComputeDocumentDelta(before, after, delta);
        Check(delta.sequence_changed && delta.clips.empty(),
              "un changement de format se signale sans lister les plans");
    }

    // --- Sérialisation -----------------------------------------------------
    {
        const Document before = Base();
        Document after = before;
        Video(after).clips[0].duration = RationalTime{80, 25};
        Video(after).clips[1].timeline_in = RationalTime{80, 25};
        Video(after).clips[2].timeline_in = RationalTime{180, 25};
        DocumentDelta delta;
        ComputeDocumentDelta(before, after, delta);
        const std::string json = SerializeDocumentDelta(delta);
        Check(json.find("\"shifted\"") != std::string::npos &&
                  json.find("\"duration\"") != std::string::npos,
              "le rapport porte la règle et la durée : " + json);
        Check(json.find("removed_clip_ids") == std::string::npos &&
                  json.find("created_tracks") == std::string::npos,
              "et tait les sections vides : " + json);
    }
    {
        const Document document = Base();
        DocumentDelta delta;
        ComputeDocumentDelta(document, document, delta);
        const std::string json = SerializeDocumentDelta(delta);
        Check(json.find("clips") == std::string::npos,
              "un delta vide ne fabrique pas un tableau de clips : " + json);
    }

    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "All document delta tests passed\n";
    return 0;
}
