// tests/rational_time_property_tests.cc
//
// Property-based testing for RationalTime.
//
// Pas de framework externe -- même pattern Test/Check que le reste de la suite.
// Les propriétés sont vérifiées sur une grille exhaustive de rates et valeurs
// représentatives plutôt que sur de l'aléatoire pur, pour que les échecs
// soient reproductibles sans graine et lisibles dans l'output CI.
//
// Propriétés couvertes :
//   P1  add est commutatif
//   P2  add est associatif (même rate)
//   P3  sub est l'inverse de add
//   P4  rescale exact aller-retour
//   P5  compare est antisymétrique
//   P6  compare est transitif
//   P7  to_frames floor : f == t.to_frames(fps) => t >= {f*den, num}
//   P8  add/sub ne produisent pas de NaN ou d'infini (overflow => exception)
//   P9  valeur zéro est neutre pour add
//   P10 égalité entre rates différents (même valeur sémantique)

#include "RationalTime.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
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

// Rates représentatifs du monde réel + cas limites
const std::vector<int32_t> kRates = {
    1,
    24,
    25,
    30,
    48,
    50,
    60,
    // NTSC drop-frame stocké comme {value * 1001, 30000}
    // mais on teste les rates directement ici
    1001,
    30000,
    // audio
    44100,
    48000,
    // sous-frames (timecode SMPTE)
    2997,
};

// Valeurs représentatives pour value (incluant 0, négatif, grands)
const std::vector<int64_t> kValues = {
    -3600LL * 48000,  // -1h en samples 48kHz
    -1,
    0,
    1,
    24,
    25,
    30,
    48,
    100,
    1001,
    30000,
    static_cast<int64_t>(3600) * 48000,   // 1h en samples
    static_cast<int64_t>(24) * 3600 * 25  // 24h en frames 25fps
};

// Rescale exact uniquement si value * newRate divisible par rate
bool CanRescaleExact(const RationalTime& t, int32_t newRate) {
    if (t.rate <= 0 || newRate <= 0) return false;
    // __int128 pour éviter l'overflow dans le test lui-même
    const __int128 num = static_cast<__int128>(t.value) * newRate;
    return (num % t.rate) == 0;
}

}  // namespace

int main() {
    // ------------------------------------------------------------------
    // P1 : add commutatif : a + b == b + a
    // ------------------------------------------------------------------
    Test("add est commutatif sur les rates courants", [] {
        // Paires de rates communs (LCM fini et dans int32_t)
        const std::vector<std::pair<int32_t, int32_t>> pairs = {
            {25, 50}, {24, 48}, {30, 60}, {25, 1}, {48000, 1}};
        for (auto [rA, rB] : pairs) {
            for (int64_t vA : {-7LL, 0LL, 1LL, 100LL}) {
                for (int64_t vB : {-3LL, 0LL, 1LL, 50LL}) {
                    const RationalTime a{vA, rA};
                    const RationalTime b{vB, rB};
                    try {
                        const RationalTime ab = a.add(b);
                        const RationalTime ba = b.add(a);
                        Check(ab == ba, "add non commutatif pour {" +
                                            std::to_string(vA) + "/" +
                                            std::to_string(rA) + "} + {" +
                                            std::to_string(vB) + "/" +
                                            std::to_string(rB) + "}");
                    } catch (const std::overflow_error&) {
                        // Overflow légal : LCM > int32_t ou value > int64_t
                    }
                }
            }
        }
    });

    // ------------------------------------------------------------------
    // P2 : add associatif sur même rate
    // ------------------------------------------------------------------
    Test("add est associatif quand même rate", [] {
        for (int32_t r : {1, 25, 48000}) {
            for (int64_t a : {-5LL, 0LL, 3LL, 100LL}) {
                for (int64_t b : {-2LL, 0LL, 7LL}) {
                    for (int64_t c : {-1LL, 0LL, 11LL}) {
                        const RationalTime ta{a, r};
                        const RationalTime tb{b, r};
                        const RationalTime tc{c, r};
                        try {
                            const RationalTime lhs = ta.add(tb).add(tc);
                            const RationalTime rhs = ta.add(tb.add(tc));
                            Check(lhs == rhs, "(a+b)+c != a+(b+c) pour rate=" +
                                                  std::to_string(r));
                        } catch (const std::overflow_error&) {
                        }
                    }
                }
            }
        }
    });

    // ------------------------------------------------------------------
    // P3 : sub est l'inverse de add : (a + b) - b == a
    // ------------------------------------------------------------------
    Test("sub inverse de add : (a+b)-b == a", [] {
        const std::vector<std::pair<int32_t, int32_t>> pairs = {
            {25, 25}, {25, 50}, {48000, 48000}, {30, 60}};
        for (auto [rA, rB] : pairs) {
            for (int64_t vA : {-10LL, 0LL, 1LL, 99LL}) {
                for (int64_t vB : {-5LL, 0LL, 3LL, 47LL}) {
                    const RationalTime a{vA, rA};
                    const RationalTime b{vB, rB};
                    try {
                        const RationalTime result = a.add(b).sub(b);
                        Check(result == a, "(a+b)-b != a pour a={" +
                                               std::to_string(vA) + "/" +
                                               std::to_string(rA) + "} b={" +
                                               std::to_string(vB) + "/" +
                                               std::to_string(rB) + "}");
                    } catch (const std::overflow_error&) {
                    }
                }
            }
        }
    });

    // ------------------------------------------------------------------
    // P4 : rescale aller-retour exact (quand possible)
    // ------------------------------------------------------------------
    Test("rescale aller-retour : rescale(rescale(t, rB), rA) == t", [] {
        const std::vector<std::pair<int32_t, int32_t>> pairs = {
            {25, 50}, {24, 48}, {1, 25}, {1, 48000}, {25, 48000}};
        for (auto [rA, rB] : pairs) {
            for (int64_t v : {0LL, 1LL, -1LL, 100LL, -100LL}) {
                const RationalTime t{v, rA};
                if (!CanRescaleExact(t, rB)) continue;
                try {
                    const RationalTime middle = t.rescale(rB);
                    if (!CanRescaleExact(middle, rA)) continue;
                    const RationalTime back = middle.rescale(rA);
                    Check(back == t, "rescale aller-retour échoue pour {" +
                                         std::to_string(v) + "/" +
                                         std::to_string(rA) + "} -> " +
                                         std::to_string(rB) + " -> " +
                                         std::to_string(rA));
                } catch (const std::invalid_argument&) {
                    // Rescale non exact : attendu, ignoré ici
                } catch (const std::overflow_error&) {
                }
            }
        }
    });

    // ------------------------------------------------------------------
    // P5 : rescale non-exact lève une exception (pas de troncature silencieuse)
    // ------------------------------------------------------------------
    Test("rescale non exact lève std::invalid_argument", [] {
        // 1 frame à 24fps ne peut pas se rescaler exactement en 25fps
        const RationalTime t{1, 24};
        bool threw = false;
        try {
            t.rescale(25);
        } catch (const std::invalid_argument&) {
            threw = true;
        } catch (const std::overflow_error&) {
            threw = true;  // acceptable
        }
        Check(threw,
              "rescale non exact doit lancer une exception, pas tronquer");
    });

    // ------------------------------------------------------------------
    // P6 : compare antisymétrique : sign(a.compare(b)) == -sign(b.compare(a))
    // ------------------------------------------------------------------
    Test("compare est antisymétrique", [] {
        const std::vector<RationalTime> samples = {
            {0, 1},   {1, 25},        {2, 25},     {1, 50},
            {-1, 25}, {30000, 30000}, {1001, 1001}};
        for (const auto& a : samples) {
            for (const auto& b : samples) {
                const int ab = a.compare(b);
                const int ba = b.compare(a);
                Check(ab == -ba || (ab == 0 && ba == 0),
                      "compare non antisymétrique");
            }
        }
    });

    // ------------------------------------------------------------------
    // P7 : compare transitif
    // ------------------------------------------------------------------
    Test("compare est transitif : a<=b et b<=c => a<=c", [] {
        const std::vector<RationalTime> samples = {
            {0, 25}, {1, 25}, {2, 25}, {1, 50}, {100, 48000}, {200, 48000}};
        for (const auto& a : samples) {
            for (const auto& b : samples) {
                for (const auto& c : samples) {
                    if (a.compare(b) <= 0 && b.compare(c) <= 0) {
                        Check(a.compare(c) <= 0, "transitivité violée");
                    }
                }
            }
        }
    });

    // ------------------------------------------------------------------
    // P8 : to_frames floor : frame*den/num <= t < (frame+1)*den/num
    // ------------------------------------------------------------------
    Test("to_frames est un floor correct", [] {
        struct Rate {
            int32_t num;
            int32_t den;
        };
        const std::vector<Rate> frameRates = {
            {25, 1}, {24, 1}, {30, 1}, {30000, 1001}, {60, 1}};
        for (auto [num, den] : frameRates) {
            for (int64_t v : {0LL, 1LL, 7LL, 100LL, -1LL, -25LL}) {
                for (int32_t r : {1, 25, 48000}) {
                    const RationalTime t{v, r};
                    const int64_t f = t.to_frames(num, den);
                    // floor : t >= {f * den, num}
                    // Si f valide dans int64, construire le RationalTime
                    // et vérifier sans overflow
                    try {
                        const RationalTime floor{f * static_cast<int64_t>(den),
                                                 num};
                        const RationalTime ceil{
                            (f + 1) * static_cast<int64_t>(den), num};
                        Check(t >= floor,
                              "to_frames : t < floor(t) -- floor division "
                              "incorrecte");
                        Check(t < ceil,
                              "to_frames : t >= ceil(t) -- pas vraiment un "
                              "floor");
                    } catch (const std::overflow_error&) {
                    }
                }
            }
        }
    });

    // ------------------------------------------------------------------
    // P9 : zéro est neutre pour add
    // ------------------------------------------------------------------
    Test("zéro est neutre pour add", [] {
        const RationalTime zero{0, 1};
        for (int32_t r : kRates) {
            for (int64_t v : {-100LL, 0LL, 1LL, 100LL}) {
                const RationalTime t{v, r};
                try {
                    Check(t.add(zero) == t, "t + 0 != t");
                    Check(zero.add(t) == t, "0 + t != t");
                } catch (const std::overflow_error&) {
                }
            }
        }
    });

    // ------------------------------------------------------------------
    // P10 : égalité sémantique entre rates différents
    // ------------------------------------------------------------------
    Test("même valeur sémantique == true entre rates différents", [] {
        // 1 seconde exprimée de plusieurs façons
        const std::vector<RationalTime> one_second = {
            {1, 1},         {25, 25},       {30, 30},
            {48000, 48000}, {30000, 30000}, {24, 24}};
        for (const auto& a : one_second) {
            for (const auto& b : one_second) {
                Check(a == b,
                      "1 seconde exprimée en rates différents devrait être "
                      "égale");
            }
        }
        // Demi-seconde : 12/24 == 1/2 == 25/50
        const std::vector<RationalTime> half_second = {
            {1, 2}, {12, 24}, {25, 50}, {24000, 48000}};
        for (const auto& a : half_second) {
            for (const auto& b : half_second) {
                Check(a == b,
                      "0.5s exprimée en rates différents devrait être égale");
            }
        }
    });

    // ------------------------------------------------------------------
    // P11 : rate invalide (<= 0) lève une exception
    // ------------------------------------------------------------------
    Test("rate invalide lève std::invalid_argument", [] {
        bool threw = false;
        try {
            RationalTime bad{1, 0};
            RationalTime good{1, 25};
            good.compare(bad);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        Check(threw, "rate=0 dans compare doit lever invalid_argument");

        threw = false;
        try {
            RationalTime{1, 25}.rescale(0);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        Check(threw, "rescale vers rate=0 doit lever invalid_argument");
    });

    // ------------------------------------------------------------------
    // P12 : overflow détecté et non silencieux
    // ------------------------------------------------------------------
    Test("overflow est détecté et lève std::overflow_error", [] {
        // Deux heures de samples à 48kHz au rate le plus élevé
        const RationalTime huge{std::numeric_limits<int64_t>::max() / 2, 48000};
        bool threw = false;
        try {
            huge.add(huge);
        } catch (const std::overflow_error&) {
            threw = true;
        }
        Check(threw,
              "add qui déborderait int64_t doit lancer std::overflow_error");
    });

    if (failures != 0) {
        std::cerr << failures << " propriété(s) RationalTime violée(s)\n";
        return 1;
    }
    std::cout << "Toutes les propriétés RationalTime vérifiées\n";
    return 0;
}
