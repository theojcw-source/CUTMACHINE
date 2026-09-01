#include "SourceAddress.h"

#include "EditLog.h"
#include "Json.h"
#include "Operations.h"

#include <iostream>
#include <string>
#include <vector>

// QC-2026-09 A4. Pur : un Document en mémoire, aucune E/S. Ce qui est épinglé
// ici est la conversion que la session mesurée ratait à la main — image
// source vers position timeline — plus les deux refus qui l'empêchent de
// rendre un chiffre plausible mais faux.

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

const char kRush[] = "01K30000000000000000000001";
const char kNtsc[] = "01K30000000000000000000002";

// Une image et son son détaché, tous deux pris à l'image 100 du rush et posés
// à 50 images de timeline. Plus un second rush en 24000/1001, parce qu'un
// tournage ne tourne pas toujours à la cadence de la séquence.
Document Fixture() {
    Document document;
    document.sources = {
        {kRush, "rush.MP4", {25, 1}, {5000, 25}},
        {kNtsc, "ntsc.MP4", {24000, 1001}, {2000 * 1001, 24000}},
    };
    DocumentClip picture;
    picture.id = "01K30000000000000000000010";
    picture.source_id = kRush;
    picture.source_in = {100, 25};
    picture.duration = {75, 25};
    picture.timeline_in = {50, 25};
    picture.link_group_id = "01K30000000000000000000012";
    DocumentClip sound = picture;
    sound.id = "01K30000000000000000000011";
    DocumentClip ntsc;
    ntsc.id = "01K30000000000000000000013";
    ntsc.source_id = kNtsc;
    ntsc.source_in = {480 * 1001, 24000};
    ntsc.duration = {240 * 1001, 24000};
    ntsc.timeline_in = {200, 25};
    document.sequence.tracks = {
        {"01K30000000000000000000020", "video", 0, {picture, ntsc}},
        {"01K30000000000000000000021", "audio", 1, {sound}},
    };
    return document;
}

}  // namespace

int main() {
    const Document document = Fixture();
    std::string error;

    // --- Une image source, tous ses emplacements ---------------------------
    {
        std::vector<SourceFrameMatch> matches;
        Check(ResolveSourceFrame(document, kRush, 130, matches, error),
              "l'image 130 se résout : " + error);
        Check(matches.size() == 2,
              "une paire A/V joue la même image à deux endroits, et les deux "
              "sont rendus plutôt qu'un choisi");
        if (matches.size() == 2) {
            // 130 - 100 = 30 images dans le plan ; le plan entre à 50.
            Check(matches[0].timeline_position == RationalTime{80, 25},
                  "la position timeline est l'entrée du plan plus le décalage");
            Check(matches[0].offset_in_clip == RationalTime{30, 25},
                  "le décalage dans le plan est publié tel quel");
            Check(matches[0].track_kind == "video" &&
                      matches[1].track_kind == "audio",
                  "les correspondances sont ordonnées par piste");
            Check(matches[0].link_group_id == "01K30000000000000000000012",
                  "le groupe de liaison suit, pour que l'appelant sache que "
                  "les deux ne font qu'un geste");
        }
    }

    // --- Les bornes sont mi-ouvertes, comme partout ailleurs ----------------
    {
        std::vector<SourceFrameMatch> matches;
        Check(ResolveSourceFrame(document, kRush, 100, matches, error) &&
                  matches.size() == 2,
              "la première image du plan est dedans : " + error);
        Check(ResolveSourceFrame(document, kRush, 174, matches, error) &&
                  matches.size() == 2,
              "la dernière aussi : " + error);
        Check(ResolveSourceFrame(document, kRush, 175, matches, error) &&
                  matches.empty(),
              "celle d'après ne l'est pas : un plan joue [in, in+durée)");
        Check(ResolveSourceFrame(document, kRush, 4000, matches, error) &&
                  matches.empty(),
              "une image du rush absente du montage rend une liste vide, pas "
              "une erreur : c'est un fait exploitable");
    }

    // --- Une cadence qui ne tombe pas sur celle de la séquence --------------
    {
        std::vector<SourceFrameMatch> matches;
        Check(ResolveSourceFrame(document, kNtsc, 500, matches, error) &&
                  matches.size() == 1,
              "une image d'un rush en 24000/1001 se résout : " + error);
        if (matches.size() == 1) {
            // 20 images de 1001/24000 s après une entrée à 200/25 s. Le
            // résultat est exact, au ppcm des deux bases, sans arrondi.
            const RationalTime expected =
                RationalTime{200, 25}.add(RationalTime{20 * 1001, 24000});
            Check(matches[0].timeline_position == expected,
                  "la position reste exacte entre deux bases de temps");
        }
    }

    // --- Refus plutôt qu'invention -----------------------------------------
    {
        std::vector<SourceFrameMatch> matches;
        Check(!ResolveSourceFrame(document, "01K39999999999999999999999", 10,
                                  matches, error),
              "un média non monté est refusé");
        Check(!ResolveSourceFrame(document, kRush, -1, matches, error),
              "une image négative est refusée");
    }

    // --- La position d'une coupe -------------------------------------------
    {
        RationalTime position;
        Check(
            ResolveClipSourceFramePosition(
                document, "01K30000000000000000000010", 130, position, error) &&
                position == RationalTime{80, 25},
            "la coupe se pose là où le plan joue l'image demandée : " + error);
        Check(!ResolveClipSourceFramePosition(
                  document, "01K30000000000000000000010", 500, position, error),
              "une image que le plan ne joue pas est refusée, pas convertie "
              "en une position hors du plan");
        Check(error.find("does not play source frame") != std::string::npos,
              "et le refus dit lequel : " + error);
    }

    // --- Les deux rognages --------------------------------------------------
    {
        RationalTime delta;
        Check(ResolveClipSourceFrameTrim(document, "01K30000000000000000000010",
                                         110, TrimEdge::Head, delta, error) &&
                  delta == RationalTime{10, 25},
              "entrer sur l'image 110 rogne de 10 images en tête : " + error);
        // La queue est inclusive : garder jusqu'à 160 laisse 61 images, donc
        // 14 de moins que les 75 de départ. C'est ce +1 que l'appelant n'a
        // pas à faire.
        Check(ResolveClipSourceFrameTrim(document, "01K30000000000000000000010",
                                         160, TrimEdge::Tail, delta, error) &&
                  delta == RationalTime{-14, 25},
              "sortir sur l'image 160 la garde, elle comprise : " + error);
        // 175 est la première image après le plan : y entrer ne laisserait
        // rien. 174, la dernière qu'il joue, en laisse une et reste licite.
        Check(ResolveClipSourceFrameTrim(document, "01K30000000000000000000010",
                                         174, TrimEdge::Head, delta, error),
              "entrer sur la dernière image jouée laisse une image : " + error);
        Check(
            !ResolveClipSourceFrameTrim(document, "01K30000000000000000000010",
                                        175, TrimEdge::Head, delta, error) &&
                error.find("empty") != std::string::npos,
            "un rognage de tête qui laisserait le plan vide est refusé : " +
                error);

        // Et le résultat s'applique vraiment : le contrat du ticket est que
        // ces valeurs soient consommables telles quelles par les opérations
        // existantes, sans retouche.
        Document applied = Fixture();
        Check(ResolveClipSourceFrameTrim(applied, "01K30000000000000000000010",
                                         110, TrimEdge::Head, delta, error),
              "le rognage de tête se résout sur le document à modifier : " +
                  error);
        TrimClipOperation trim;
        trim.clip_id = "01K30000000000000000000010";
        trim.edge = TrimEdge::Head;
        trim.delta = delta;
        EditLog log;
        EditError editError = EditError::None;
        std::string message;
        Check(log.Apply(applied, Operation{trim}, editError, message),
              "et s'applique : " + message);
        const DocumentClip* trimmed =
            applied.FindClip("01K30000000000000000000010");
        Check(trimmed != nullptr && trimmed->source_in == RationalTime{110, 25},
              "le plan entre bien sur l'image demandée");
    }

    // --- Sérialisation ------------------------------------------------------
    {
        std::vector<SourceFrameMatch> matches;
        ResolveSourceFrame(document, kRush, 130, matches, error);
        mcp_json::Value parsed;
        std::string parseError;
        Check(mcp_json::Value::Parse(
                  DescribeSourceFrameMatches(kRush, 130, matches), parsed,
                  parseError) &&
                  parsed.IsObject(),
              "la vue JSON est valide : " + parseError);
        const mcp_json::Value* count = parsed.Find("match_count");
        Check(count != nullptr && count->IsNumber(),
              "elle publie le nombre de correspondances, pour qu'une liste "
              "vide se distingue d'un échec");
    }

    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "Tous les tests d'adressage par image source passent\n";
    return 0;
}
