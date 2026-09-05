#include "Document.h"
#include "ResolveExport.h"

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

LibraryMedia Media(const std::string& id, const std::string& name,
                   int32_t rateNum) {
    LibraryMedia media;
    media.id = id;
    media.filename = name;
    media.path = "/rushes/" + name;
    media.codec = "h264";
    media.width = 3840;
    media.height = 2160;
    media.orientation = "landscape";
    media.rate = {rateNum, 1};
    media.duration = {1000, rateNum};
    return media;
}

DocumentClip Clip(const std::string& id, const std::string& sourceId,
                  RationalTime timelineIn, RationalTime sourceIn,
                  RationalTime duration) {
    DocumentClip clip;
    clip.id = id;
    clip.source_id = sourceId;
    clip.timeline_in = timelineIn;
    clip.source_in = sourceIn;
    clip.duration = duration;
    return clip;
}

}  // namespace

int main() {
    // --- Conversion exacte ------------------------------------------------
    int64_t frames = -1;
    Check(ExactFrameCount({100, 25}, {25, 1}, frames) && frames == 100,
          "une position au débit de la source donne son numéro d'image");
    Check(ExactFrameCount({100, 25}, {50, 1}, frames) && frames == 200,
          "la même seconde vaut deux fois plus d'images à 50 i/s");
    // La moitié d'une image à 25 i/s : Resolve ne sait pas l'exprimer, et
    // arrondir déplacerait la coupe.
    Check(!ExactFrameCount({1, 50}, {25, 1}, frames),
          "une position entre deux images est refusée, jamais arrondie");
    Check(!ExactFrameCount({1, 1001}, {30000, 1001}, frames) || frames == 30000,
          "un débit non entier reste exact ou se refuse");
    Check(!ExactFrameCount({10, 0}, {25, 1}, frames),
          "un débit nul est refusé");

    // --- Timeline ---------------------------------------------------------
    Document document;
    document.sequence.name = "MONTAGE";
    document.sequence.frame_rate = {25, 1};
    document.sequence.width = 2160;
    document.sequence.height = 3840;
    document.library = {Media("01K30000000000000000000001", "A.MP4", 25),
                        Media("01K30000000000000000000002", "B.MP4", 25)};
    DocumentTrack video;
    video.id = "01K30000000000000000000010";
    video.kind = "video";
    video.index = 0;
    // Volontairement dans le désordre : c'est la position sur la timeline qui
    // fait foi, pas l'ordre de stockage.
    video.clips = {
        Clip("01K30000000000000000000021", "01K30000000000000000000002",
             {100, 25}, {50, 25}, {25, 25}),
        Clip("01K30000000000000000000020", "01K30000000000000000000001",
             {0, 25}, {0, 25}, {100, 25}),
    };
    DocumentTrack audio;
    audio.id = "01K30000000000000000000011";
    audio.kind = "audio";
    audio.index = 1;
    audio.clips = {Clip("01K30000000000000000000022",
                        "01K30000000000000000000001", {0, 25}, {0, 25},
                        {100, 25})};
    document.sequence.tracks = {video, audio};

    ResolveTimelineExport exported;
    ResolveTimelineExport rejectedPosition;
    std::string error;
    Check(BuildResolveTimelineExport(document, exported, error),
          "une timeline simple s'exporte : " + error);
    Check(exported.name == "MONTAGE" && exported.width == 2160 &&
              exported.height == 3840,
          "le format de séquence est transmis");
    Check(exported.clips.size() == 2,
          "les clips audio ne sont pas renvoyés : Resolve rapporte le son "
          "avec l'image, les envoyer doublerait chaque plan");
    Check(exported.clips[0].filename == "A.MP4" &&
              exported.clips[1].filename == "B.MP4",
          "les plans sortent dans l'ordre de la timeline, pas du stockage");
    // 100 images à partir de 0 : la borne de fin est exclusive, parce que
    // Resolve construit end-start images. Vérifié sur un aller-retour réel :
    // une borne inclusive perdait une image sur chaque plan.
    Check(exported.clips[0].start_frame == 0 &&
              exported.clips[0].end_frame == 100,
          "la borne de fin est exclusive");
    Check(exported.clips[1].start_frame == 50 &&
              exported.clips[1].end_frame == 75,
          "le second plan porte sa propre plage source");
    Check(exported.clips[0].video_layer == 0 &&
              exported.clips[1].video_layer == 0,
          "une timeline à une seule piste reste sur la couche de base");
    Check(exported.clips[0].record_frame == 0 &&
              exported.clips[1].record_frame == 100,
          "chaque plan porte sa position, en images de séquence");
    Check(exported.clips[0].with_audio && !exported.clips[1].with_audio,
          "seul le plan que la timeline joue aussi sur une piste son "
          "emporte son audio");

    // --- Recouvrement -----------------------------------------------------
    // Le cas qui a motivé le schéma v2 : un plan de coupe posé PAR-DESSUS un
    // plan parlant. Aplati en une seule liste, il repartait après la phrase
    // qu'il devait couvrir, et le montage s'allongeait d'autant.
    Document layered = document;
    DocumentTrack overlay;
    overlay.id = "01K30000000000000000000012";
    overlay.kind = "video";
    overlay.index = 2;
    overlay.clips = {Clip("01K30000000000000000000023",
                          "01K30000000000000000000002", {25, 25}, {200, 25},
                          {50, 25})};
    layered.sequence.tracks.push_back(overlay);
    ResolveTimelineExport covered;
    Check(BuildResolveTimelineExport(layered, covered, error),
          "un montage à deux pistes vidéo s'exporte : " + error);
    Check(covered.clips.size() == 3, "les trois plans sont envoyés");
    Check(
        covered.clips[0].video_layer == 0 && covered.clips[1].video_layer == 0,
        "la couche de base sort en premier, entière");
    Check(covered.clips[2].video_layer == 1 &&
              covered.clips[2].record_frame == 25,
          "le plan de coupe nomme sa couche et sa position au lieu de "
          "s'ajouter à la suite");
    Check(!covered.clips[2].with_audio,
          "un plan de coupe part sans son : Resolve poserait sinon son "
          "ambiance sous l'interview qu'il recouvre");

    // Une position de timeline qui ne tombe pas sur une image de séquence.
    Document offGrid = document;
    offGrid.sequence.tracks[0].clips[1].timeline_in = {1, 50};
    Check(!BuildResolveTimelineExport(offGrid, rejectedPosition, error) &&
              error.find("whole sequence frame") != std::string::npos,
          "une position entre deux images de séquence est refusée : " + error);

    // --- Refus ------------------------------------------------------------
    Document unknown = document;
    unknown.library.clear();
    ResolveTimelineExport rejected;
    Check(!BuildResolveTimelineExport(unknown, rejected, error) &&
              error.find("not in the library") != std::string::npos,
          "un média absent de la médiathèque est refusé : " + error);

    // Une source à 50 i/s coupée sur une demi-image de son propre domaine.
    Document halfFrame;
    halfFrame.sequence.frame_rate = {25, 1};
    halfFrame.library = {Media("01K30000000000000000000001", "C.MP4", 3)};
    DocumentTrack odd;
    odd.id = "01K30000000000000000000010";
    odd.kind = "video";
    odd.index = 0;
    odd.clips = {Clip("01K30000000000000000000020",
                      "01K30000000000000000000001", {0, 25}, {1, 25},
                      {25, 25})};
    halfFrame.sequence.tracks = {odd};
    Check(
        !BuildResolveTimelineExport(halfFrame, rejected, error) &&
            error.find("whole source frames") != std::string::npos,
        "une coupe qui tombe entre deux images source est refusée : " + error);

    Document silent;
    silent.sequence.frame_rate = {25, 1};
    Check(!BuildResolveTimelineExport(silent, rejected, error),
          "une timeline sans plan vidéo n'a rien à envoyer");

    // --- Rapport ----------------------------------------------------------
    const std::string json = SerializeResolveTimelineExport(exported);
    Check(json.find("\"schema\":\"cutmachine.resolve-timeline.v2\"") !=
              std::string::npos,
          "le rapport annonce son schéma");
    Check(json.find("\"start_frame\":50") != std::string::npos &&
              json.find("\"end_frame\":75") != std::string::npos,
          "les plages sortent en images entières : " + json);
    Check(
        json.find("\"video_layer\":0") != std::string::npos &&
            json.find("\"record_frame\":100") != std::string::npos &&
            json.find("\"with_audio\":true") != std::string::npos &&
            json.find("\"with_audio\":false") != std::string::npos,
        "le rapport porte la couche, la position et le sort du son : " + json);
    Check(json.find("cutmachine.resolve-timeline.v2") != std::string::npos,
          "le schéma passe en v2 : un pont v1 aplatirait les couches");

    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "All resolve export tests passed\n";
    return 0;
}
