#include "SequenceFormat.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

LibraryMedia Media(int32_t width, int32_t height, int32_t rateNum,
                   int32_t rotation = 0) {
    LibraryMedia media;
    media.width = width;
    media.height = height;
    media.rotation_degrees = rotation;
    media.rate = {rateNum, 1};
    return media;
}

}  // namespace

int main() {
    // --- Rotation ---------------------------------------------------------
    int32_t width = 0;
    int32_t height = 0;
    Check(DisplayDimensions(Media(3840, 2160, 25, 0), width, height) &&
              width == 3840 && height == 2160,
          "un rush non tourné garde ses dimensions");
    Check(DisplayDimensions(Media(3840, 2160, 25, 90), width, height) &&
              width == 2160 && height == 3840,
          "un quart de tour échange largeur et hauteur");
    Check(DisplayDimensions(Media(3840, 2160, 25, 270), width, height) &&
              width == 2160 && height == 3840,
          "trois quarts de tour échangent aussi");
    // The case a real project produced: FFmpeg reports a counterclockwise
    // angle, and it can be negative.
    Check(DisplayDimensions(Media(3840, 2160, 50, -90), width, height) &&
              width == 2160 && height == 3840,
          "une rotation négative est normalisée avant l'échange");
    Check(DisplayDimensions(Media(3840, 2160, 25, 180), width, height) &&
              width == 3840 && height == 2160,
          "un demi-tour n'échange rien");
    Check(DisplayDimensions(Media(3840, 2160, 25, 450), width, height) &&
              width == 2160 && height == 3840,
          "un angle au-delà du tour complet est ramené dans [0,360)");
    Check(!DisplayDimensions(Media(3840, 2160, 25, 45), width, height),
          "un angle qui n'est pas droit n'a pas de format entier exact");
    Check(!DisplayDimensions(Media(0, 0, 25), width, height),
          "un média sans image n'a pas de dimensions");

    // --- Choix du format --------------------------------------------------
    std::string error;
    SequenceFormatProposal proposal;
    std::vector<LibraryMedia> library;
    for (int index = 0; index < 3; ++index)
        library.push_back(Media(3840, 2160, 25, 90));
    library.push_back(Media(3840, 2160, 50, 90));
    Check(ResolveSequenceFormat(library, proposal, error),
          "une médiathèque mixte se résout : " + error);
    Check(proposal.chosen.width == 2160 && proposal.chosen.height == 3840,
          "le format retenu est celui affiché, pas celui stocké");
    Check(proposal.chosen.frame_rate.num == 25 &&
              proposal.chosen.frame_rate.den == 1,
          "la cadence majoritaire l'emporte");
    Check(proposal.chosen.media_count == 3 && proposal.media_considered == 4,
          "les comptes rendent la majorité vérifiable");
    Check(!proposal.unanimous && proposal.candidates.size() == 2 &&
              proposal.candidates[1].frame_rate.num == 50,
          "le format minoritaire est rapporté, pas escamoté");

    // L'ordre de la médiathèque ne doit rien changer.
    std::vector<LibraryMedia> reversed(library.rbegin(), library.rend());
    SequenceFormatProposal reversedProposal;
    Check(ResolveSequenceFormat(reversed, reversedProposal, error),
          "la médiathèque inversée se résout : " + error);
    Check(reversedProposal.chosen.width == proposal.chosen.width &&
              reversedProposal.chosen.height == proposal.chosen.height &&
              reversedProposal.chosen.frame_rate.num ==
                  proposal.chosen.frame_rate.num,
          "le résultat ne dépend pas de l'ordre de la médiathèque");

    // Égalité parfaite : la plus grande image tranche, puis la cadence.
    SequenceFormatProposal tie;
    Check(ResolveSequenceFormat({Media(1920, 1080, 25), Media(3840, 2160, 25)},
                                tie, error),
          "une égalité se résout : " + error);
    Check(tie.chosen.width == 3840,
          "à égalité de comptes, la plus grande image l'emporte");
    SequenceFormatProposal rateTie;
    Check(ResolveSequenceFormat({Media(1920, 1080, 25), Media(1920, 1080, 50)},
                                rateTie, error),
          "une égalité de taille se résout : " + error);
    Check(rateTie.chosen.frame_rate.num == 50,
          "à taille égale, la cadence la plus haute l'emporte");

    // --- Ce qui ne compte pas --------------------------------------------
    LibraryMedia audioOnly;
    audioOnly.has_audio = true;
    audioOnly.rate = {0, 1};
    SequenceFormatProposal mixed;
    Check(ResolveSequenceFormat(
              {Media(1920, 1080, 25), audioOnly, Media(0, 0, 25)}, mixed,
              error),
          "une médiathèque partiellement inutilisable se résout : " + error);
    Check(mixed.media_considered == 1 && mixed.media_ignored == 2 &&
              mixed.unanimous,
          "les médias sans format sont comptés à part, jamais rattachés");

    SequenceFormatProposal impossible;
    Check(!ResolveSequenceFormat({audioOnly}, impossible, error),
          "sans aucune image, il n'y a pas de format à déduire");
    Check(error.find("audio-only") != std::string::npos,
          "le refus dit pourquoi : " + error);
    Check(!ResolveSequenceFormat({}, impossible, error),
          "une médiathèque vide ne produit pas de format");

    // --- Rapport ----------------------------------------------------------
    const std::string json = SequenceFormatProposalJson(proposal);
    Check(json.find("\"width\":2160") != std::string::npos &&
              json.find("\"media_count\":3") != std::string::npos &&
              json.back() == '}',
          "le rapport porte le format et ses comptes : " + json);
    const std::string spliced =
        SequenceFormatProposalJson(proposal, ",\"applied\":true");
    Check(spliced.find(",\"applied\":true}") != std::string::npos,
          "les champs propres à l'appelant se greffent avant l'accolade : " +
              spliced);

    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "All sequence format tests passed\n";
    return 0;
}
