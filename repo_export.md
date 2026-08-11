# Repo export for AI review

- Root: `/Volumes/code/CUTMACHINE`
- Generated: 2026-08-10 17:54 UTC
- File list source: git ls-files (honours .gitignore)
- Included: 57 files, ~874 KB
- Skipped: 11 files (see manifest at the end)

## File tree

```
AUDIT.md
CMakeLists.txt
LICENSE
README.md
RESULTS.md
docs/BINS_SPEC.md
docs/NLE_TIMELINE_SPEC.md
example-timeline.json
example-timeline.json.editlog.json
scripts/export_for_ai.py
sidecar/__init__.py
sidecar/binary.py
sidecar/eval-document.json
sidecar/eval.py
sidecar/planner.py
sidecar/repl.py
sidecar/schema.py
sidecar/test_sidecar.py
src/AudioPlayback.h
src/AudioPlayback.mm
src/Cli.cc
src/Cli.h
src/ColorManagement.cc
src/ColorManagement.h
src/DecodeWorker.cc
src/DecodeWorker.h
src/Document.cc
src/Document.h
src/EditLog.cc
src/EditLog.h
src/FrameCache.cc
src/FrameCache.h
src/Ingest.cc
src/Ingest.h
src/MediaSource.h
src/MediaSource.mm
src/Operations.cc
src/Operations.h
src/PerformanceMetrics.cc
src/PerformanceMetrics.h
src/RationalTime.h
src/Renderer.h
src/Renderer.mm
src/Timeline.cc
src/Timeline.h
src/TimelineView.cc
src/TimelineView.h
src/Ulid.cc
src/Ulid.h
src/main.mm
src/shader.metal
tests/audio_playback_tests.mm
tests/cli_tests.cc
tests/edit_tests.cc
tests/ingest_tests.cc
tests/model_tests.cc
tests/timeline_view_tests.cc
```

## Files

### AUDIT.md

```markdown
# Audit du planner CUTMACHINE

Date de mesure : 10 août 2026. Cet audit n'a modifié ni le prompt système, ni
le schéma, ni la vue `--describe`, ni le code du planner ou du binaire. La seule
modification de code est l'instrumentation de `sidecar/eval.py`.

## Addendum — expérience de simplification du planner

Le verdict initial ci-dessous s'arrêtait trop tôt. Le 10 août 2026, deux
changements ont été appliqués et mesurés séquentiellement avec le même modèle
local `qwen2.5-coder:7b`, à température zéro et sur les 15 cas inchangés.

| État | Score corpus | Cas de référence |
|---|---:|---:|
| Baseline auditée | 8/15 | PASS |
| Conversions temporelles déterministes | 11/15 | PASS |
| Puis résolution déterministe des entités | 15/15 | PASS |

Le 11/15 de l'étape temporelle est un replay contrôlé des réponses brutes du
run immédiatement précédent après ajout du normaliseur de position absolue ;
le prompt, le schéma et les sorties du modèle sont inchangés. Le 15/15 final
est un run HTTP complet après ajout du résolveur d'entités. La variance finale
n'a pas été remesurée : il s'agit d'un run, à comparer aux trois runs identiques
de la baseline.

La première étape remplace le `delta` signé demandé au modèle par une intention
`Shorten|Extend`, un bord et une quantité positive `Frames|Seconds`. Le code
calcule ensuite signe, timebase et position absolue. La seconde étape résout les
pistes ordinales, clips ordinaux ou uniques, alias et noms de source depuis la
vue avant de valider l'opération.

Le résultat le plus probant n'est pas seulement 15/15 : sur les cas 2, 11, 13
et 15, les ULID bruts produits par Qwen restent exactement les mauvais ULID de
la baseline, tandis que l'opération normalisée est correcte. Le gain vient donc
bien du déplacement d'un travail déterministe hors du modèle, et non d'une
amélioration fortuite de sa réponse. La conclusion initiale « capacité du
modèle, pas conception » est réfutée pour ce corpus : la chaîne technique était
correcte, mais son contrat faisait porter au modèle des conventions internes et
des résolutions que le code pouvait garantir.

## Verdict

**Le facteur limitant mesuré est la capacité du modèle local, pas la conception
commune aux deux backends.** Ollama avec `qwen2.5-coder:7b` obtient 8/15
(53,3 %) lors de chacun des trois runs. Anthropic avec le modèle effectivement
retourné `claude-sonnet-4-5-20250929` obtient 15/15 (100 %). L'écart est de
46,7 points, Anthropic ne reproduit aucun des sept échecs Ollama, et tous les
oracles sont valides directement dans le binaire.

Le cas critique « Raccourcis le premier plan de 2 secondes » passe sur les deux
backends. La fonction centrale est donc viable sur ce cas, même si le modèle
local reste insuffisant sur le corpus complet.

## Taux et variance

| Backend | Run 1 | Run 2 | Run 3 | Moyenne corpus | Variance observée |
|---|---:|---:|---:|---:|---:|
| Ollama `qwen2.5-coder:7b` | 8/15 (53,3 %) | 8/15 (53,3 %) | 8/15 (53,3 %) | 53,3 % | étendue 0 point ; écart-type 0 point |
| Anthropic `claude-sonnet-4-5-20250929` | 15/15 (100 %) | — | — | 100 % | non mesurée (un run demandé) |

Les sorties brutes Ollama sont identiques cas par cas sur les trois runs, pas
seulement les scores. L'ajout du cas de référence donne 9/16 à Ollama sur chaque
run et 16/16 à Anthropic ; ces scores étendus ne remplacent pas les taux du
corpus historique.

## Résultats des 15 cas

« PASS » signifie que l'opération normalisée est sémantiquement égale à
l'oracle. Les résultats Ollama ci-dessous sont ceux des trois runs, puisqu'ils
sont identiques. La famille indiquée est la première cause rencontrée ; une
cause secondaire est explicitement signalée.

| # | Instruction | Résultat Ollama (3/3 runs) | Résultat Anthropic | Famille |
|---:|---|---|---|---|
| 1 | Supprime le clip A1. | PASS | PASS | — |
| 2 | Enlève le deuxième clip de la piste vidéo 1. | FAIL : choisit B1 (`…008`) au lieu d'A2 (`…005`) | PASS | E — mauvais clip sélectionné ; l'ULID produit existe dans la vue |
| 3 | Supprime A3, le plan issu d'illustrations.mov. | PASS | PASS | — |
| 4 | Retire l'unique clip de la deuxième piste. | PASS | PASS | — |
| 5 | Raccourcis la fin de A1 de 10 images. | PASS | PASS | — |
| 6 | Retire 5 images à la fin de A2. | PASS | PASS | — |
| 7 | Coupe les 10 premières images de A3. | FAIL : `delta=-10/25`, attendu `+10/25` | PASS | C — signe temporel faux |
| 8 | Récupère 5 images avant le début actuel de A2. | PASS | PASS | — |
| 9 | Prolonge la fin de A3 de 10 images. | PASS | PASS | — |
| 10 | Enlève 5 images au début de B1. | FAIL : `delta=-5/25`, attendu `+5/25` | PASS | C — signe temporel faux |
| 11 | Sur la piste vidéo 1, insère à l'image 50 dix images de interview.mov à partir de sa source 200. | FAIL : piste 2 (`…007`) au lieu de piste 1 (`…003`) | PASS | E — mauvaise piste sélectionnée ; l'ULID produit existe dans la vue |
| 12 | Au début de la piste vidéo 2, insère les 15 premières images de illustrations.mov. | PASS | PASS | — |
| 13 | Ajoute à la fin de la piste vidéo 1 vingt images de interview.mov depuis l'image source 300. | FAIL : piste 2 au lieu de piste 1 ; `timeline_in=125/25` au lieu de `150/25` | PASS | E — mauvaise piste en première cause ; C secondaire |
| 14 | Dans le trou après A1, à l'image 50, place 10 images de illustrations.mov depuis l'image source 50. | FAIL : `timeline_in=75/25`, attendu `50/25` | PASS | C — position temporelle fausse |
| 15 | Insère au tout début de la piste vidéo 2 cinq images de interview.mov à partir de l'image source 400. | FAIL : piste 1 au lieu de piste 2 | PASS | E — mauvaise piste sélectionnée ; l'ULID produit existe dans la vue |

## Répartition des échecs

| Backend / périmètre | A | B | C | D | E | Total |
|---|---:|---:|---:|---:|---:|---:|
| Ollama, par run de 15 cas | 0 | 0 | 3 | 0 | 4 | 7 |
| Ollama, agrégé sur 3 runs (45 réponses) | 0 | 0 | 9 | 0 | 12 | 21 |
| Anthropic, 15 réponses | 0 | 0 | 0 | 0 | 0 | 0 |

Le cas 13 possède aussi une cause C secondaire, non ajoutée aux totaux afin de
respecter la classification exclusive par première cause. Aucun modèle n'a
inventé ou déformé d'ULID pendant ces runs : les erreurs d'identité Ollama sont
des sélections d'objets existants, donc E et non B. Il n'y a ni mauvais type
d'opération (A), ni sortie hors schéma/refus injustifié (D).

## Cas de référence isolé

Le cas ne figurait pas dans les 15 instructions. Il a été ajouté comme cas
supplémentaire, sans modifier le corpus historique ni ses dénominateurs.

Instruction : **« Raccourcis le premier plan de 2 secondes. »**

Oracle : `TrimClip`, clip A1 (`01K40000000000000000000004`), bord `Tail`,
`delta={"value":-50,"rate":25}`.

| Backend | Résultat | Opération produite |
|---|---|---|
| Ollama | PASS sur 3/3 runs | exactement l'oracle sur les champs sémantiques |
| Anthropic | PASS | exactement l'oracle sur les champs sémantiques |

## Vérifications de contrôle

1. **Contrainte Ollama réellement émise : oui.** Les 48 requêtes HTTP Ollama
   capturées (45 corpus + 3 référence) contiennent le champ `format`. Sa valeur
   est égale au schéma partagé. Ce constat vient du corps sérialisé de chaque
   `urllib.request.Request`, pas d'une lecture statique de l'intention du code.

2. **Schémas identiques : oui.** Le `format` effectivement envoyé à Ollama et
   l'`input_schema` effectivement envoyé dans l'unique outil Anthropic sont
   strictement identiques comme objets JSON, et tous deux égaux à
   `PLANNER_RESPONSE_SCHEMA`.

3. **Validité des 15 oracles : 15/15.** Chaque opération attendue a été soumise
   à `--apply-op` sur une copie fraîche de `sidecar/eval-document.json`. Les 15
   ont été acceptées. Aucun échec du planner n'est imputable à un faux cas de
   test.

4. **Taille de la vue : 1 638 caractères/octets UTF-8.** Le JSON compact exact
   issu de `--describe` mesure **753 tokens avec le tokenizer réel de
   `qwen2.5-coder:7b`** (`/api/generate`, mode `raw`) et **606 tokens avec
   `claude-sonnet-4-5`** (API `count_tokens`). Cette vue seule est modeste et ne
   constitue pas un signal de surcharge contextuelle. À titre distinct, une
   requête Anthropic complète observée consomme 2 340 tokens d'entrée, prompt
   système et enveloppe inclus.

5. **Validation ULID avant le binaire.** La chaîne est : réponse HTTP → décodage
   structuré → `_parse_plan` → `_normalize_operation` → validation contre les
   IDs de la vue. Un ULID absent déclenche donc un `PlannerError` local dans
   `planner.plan`, avant tout appel à `CutmachineBinary.apply_operation`. Le
   chemin d'évaluation n'appelle d'ailleurs jamais `--apply-op` pour une sortie
   modèle. Ce rejet peut masquer le refus nommé qu'aurait donné le C++, mais il
   ne masque pas le score : le harnais le comptabilise comme échec, et
   l'instrumentation conserve la sortie brute avec le point de rupture
   `validation_locale_ulid_avant_binaire`. Aucun rejet B n'a eu lieu dans cette
   campagne.

## Lecture diagnostique

La contrainte structurée fonctionne : 64/64 réponses ont traversé le transport,
le décodage et la validation locale sans erreur de schéma ou refus. Les sept
échecs récurrents de Qwen sont sémantiques : quatre résolutions d'objet/piste et
trois calculs temporels. Anthropic résout ces mêmes formulations sans changer
aucune autre composante de la chaîne. Cela exclut, sur ce corpus, le schéma, les
oracles, la longueur de vue et l'absence de contrainte de décodage comme causes
principales.

La conclusion reste bornée à ce corpus de 15 cas, un seul document, un seul run
Anthropic et les modèles mesurés. Elle ne prouve pas qu'aucun défaut de
conception n'existe hors corpus ; elle montre que les échecs observés ici ne
sont pas communs aux deux backends.

## Corrections envisageables, non appliquées

Classement par gain attendu sur ce corpus :

1. **Employer Anthropic pour le chemin exigeant ou router vers un modèle plus
   capable.** Gain observé : +7 cas sur 15, soit +46,7 points. C'est le seul gain
   directement mesuré, mais il introduit coût, latence réseau et dépendance à un
   service externe.

2. **Sortir la résolution des entités ordinales et des pistes du raisonnement
   libre.** Un résolveur déterministe pourrait transformer « deuxième clip de
   la piste 1 » et « piste vidéo 2 » en candidats ULID avant la production de
   l'opération. Gain maximal direct : 4 cas sur 15 (+26,7 points), avec moins de
   flexibilité linguistique à gérer explicitement.

3. **Rendre déterministes les conversions temporelles après extraction de
   l'intention.** Extraire action, bord et quantité, puis calculer signe,
   timebase et position dans une couche typée cible directement les trois
   échecs C (+20 points) et la cause secondaire du cas 13. Cela déplace une part
   du planner vers du code vérifiable.

4. **Évaluer un modèle local plus capable avec exactement ce protocole.** Le
   potentiel est de récupérer jusqu'aux sept cas perdus sans changer
   l'architecture, mais le gain n'est pas encore mesuré et le coût mémoire ou la
   latence peuvent augmenter.

5. **Ne modifier prompt, schéma ou vue qu'après une expérience isolée.** Leur
   gain attendu est faible dans cette campagne : zéro échec A/B/D, schémas
   identiques et respectés, vue de 753 tokens, et les règles de signe sont déjà
   présentes dans le prompt. Une telle piste demanderait un nouveau protocole
   A/B ; aucune modification ni aucun few-shot n'a été appliqué ici.

## Artefacts

`eval-trace.json` contient 64 enregistrements : instruction, oracle, sortie
brute avant validation, opération finale ou refus/erreur, point de rupture,
classification primaire et causes secondaires, réponse HTTP brute et preuve du
schéma présent dans la requête émise. Aucune clé API ni aucun en-tête secret n'y
est enregistré.
```

### CMakeLists.txt

```
cmake_minimum_required(VERSION 3.24)
project(CUTMACHINE LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 17)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswresample
)

set(SHADER_RUNTIME_PATH "${CMAKE_CURRENT_BINARY_DIR}/shader.metal")
configure_file(src/shader.metal "${SHADER_RUNTIME_PATH}" COPYONLY)

add_library(cutmachine_model
    src/Cli.cc
    src/ColorManagement.cc
    src/Document.cc
    src/Ingest.cc
    src/EditLog.cc
    src/Operations.cc
    src/Timeline.cc
    src/TimelineView.cc
    src/Ulid.cc
)
target_include_directories(cutmachine_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cutmachine_model PUBLIC PkgConfig::FFMPEG)

add_executable(cutmachine
    src/main.mm
    src/AudioPlayback.mm
    src/DecodeWorker.cc
    src/FrameCache.cc
    src/PerformanceMetrics.cc
    src/MediaSource.mm
    src/Renderer.mm
)
target_include_directories(cutmachine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cutmachine PRIVATE cutmachine_model)
target_compile_options(cutmachine PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
)
target_compile_definitions(cutmachine PRIVATE
    CUTMACHINE_SHADER_PATH="${SHADER_RUNTIME_PATH}"
)
target_link_libraries(cutmachine PRIVATE
    PkgConfig::FFMPEG
    "-framework AppKit"
    "-framework AVFAudio"
    "-framework Foundation"
    "-framework Metal"
    "-framework QuartzCore"
)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    add_executable(cutmachine_tests tests/model_tests.cc)
    target_link_libraries(cutmachine_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_model_tests COMMAND cutmachine_tests)

    add_executable(cutmachine_edit_tests tests/edit_tests.cc)
    target_link_libraries(cutmachine_edit_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_edit_tests COMMAND cutmachine_edit_tests)

    add_executable(cutmachine_timeline_view_tests
                   tests/timeline_view_tests.cc)
    target_link_libraries(cutmachine_timeline_view_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_timeline_view_tests
             COMMAND cutmachine_timeline_view_tests)

    add_executable(cutmachine_cli_tests tests/cli_tests.cc)
    target_link_libraries(cutmachine_cli_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_cli_tests COMMAND cutmachine_cli_tests)

    get_target_property(_ffmpeg_includes PkgConfig::FFMPEG
                        INTERFACE_INCLUDE_DIRECTORIES)
    list(GET _ffmpeg_includes 0 _ffmpeg_include)
    get_filename_component(_ffmpeg_prefix "${_ffmpeg_include}" DIRECTORY)
    find_program(FFMPEG_EXECUTABLE NAMES ffmpeg
                 HINTS "${_ffmpeg_prefix}/bin" REQUIRED)
    add_executable(cutmachine_ingest_tests tests/ingest_tests.cc)
    target_link_libraries(cutmachine_ingest_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_ingest_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_ingest_tests COMMAND cutmachine_ingest_tests)

    add_executable(cutmachine_audio_playback_tests
        tests/audio_playback_tests.mm
        src/AudioPlayback.mm
    )
    target_compile_options(cutmachine_audio_playback_tests PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_compile_definitions(cutmachine_audio_playback_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}"
    )
    target_link_libraries(cutmachine_audio_playback_tests PRIVATE
        cutmachine_model
        PkgConfig::FFMPEG
        "-framework AVFAudio"
        "-framework Foundation"
    )
    add_test(NAME cutmachine_audio_playback_tests
             COMMAND cutmachine_audio_playback_tests)

    add_test(
        NAME cutmachine_sidecar_tests
        COMMAND "${Python3_EXECUTABLE}" -m unittest sidecar.test_sidecar
    )
    set_tests_properties(cutmachine_sidecar_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()
```

### LICENSE

```

                                 Apache License
                           Version 2.0, January 2004
                        http://www.apache.org/licenses/

   TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION

   1. Definitions.

      "License" shall mean the terms and conditions for use, reproduction,
      and distribution as defined by Sections 1 through 9 of this document.

      "Licensor" shall mean the copyright owner or entity authorized by
      the copyright owner that is granting the License.

      "Legal Entity" shall mean the union of the acting entity and all
      other entities that control, are controlled by, or are under common
      control with that entity. For the purposes of this definition,
      "control" means (i) the power, direct or indirect, to cause the
      direction or management of such entity, whether by contract or
      otherwise, or (ii) ownership of fifty percent (50%) or more of the
      outstanding shares, or (iii) beneficial ownership of such entity.

      "You" (or "Your") shall mean an individual or Legal Entity
      exercising permissions granted by this License.

      "Source" form shall mean the preferred form for making modifications,
      including but not limited to software source code, documentation
      source, and configuration files.

      "Object" form shall mean any form resulting from mechanical
      transformation or translation of a Source form, including but
      not limited to compiled object code, generated documentation,
      and conversions to other media types.

      "Work" shall mean the work of authorship, whether in Source or
      Object form, made available under the License, as indicated by a
      copyright notice that is included in or attached to the work
      (an example is provided in the Appendix below).

      "Derivative Works" shall mean any work, whether in Source or Object
      form, that is based on (or derived from) the Work and for which the
      editorial revisions, annotations, elaborations, or other modifications
      represent, as a whole, an original work of authorship. For the purposes
      of this License, Derivative Works shall not include works that remain
      separable from, or merely link (or bind by name) to the interfaces of,
      the Work and Derivative Works thereof.

      "Contribution" shall mean any work of authorship, including
      the original version of the Work and any modifications or additions
      to that Work or Derivative Works thereof, that is intentionally
      submitted to Licensor for inclusion in the Work by the copyright owner
      or by an individual or Legal Entity authorized to submit on behalf of
      the copyright owner. For the purposes of this definition, "submitted"
      means any form of electronic, verbal, or written communication sent
      to the Licensor or its representatives, including but not limited to
      communication on electronic mailing lists, source code control systems,
      and issue tracking systems that are managed by, or on behalf of, the
      Licensor for the purpose of discussing and improving the Work, but
      excluding communication that is conspicuously marked or otherwise
      designated in writing by the copyright owner as "Not a Contribution."

      "Contributor" shall mean Licensor and any individual or Legal Entity
      on behalf of whom a Contribution has been received by Licensor and
      subsequently incorporated within the Work.

   2. Grant of Copyright License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      copyright license to reproduce, prepare Derivative Works of,
      publicly display, publicly perform, sublicense, and distribute the
      Work and such Derivative Works in Source or Object form.

   3. Grant of Patent License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      (except as stated in this section) patent license to make, have made,
      use, offer to sell, sell, import, and otherwise transfer the Work,
      where such license applies only to those patent claims licensable
      by such Contributor that are necessarily infringed by their
      Contribution(s) alone or by combination of their Contribution(s)
      with the Work to which such Contribution(s) was submitted. If You
      institute patent litigation against any entity (including a
      cross-claim or counterclaim in a lawsuit) alleging that the Work
      or a Contribution incorporated within the Work constitutes direct
      or contributory patent infringement, then any patent licenses
      granted to You under this License for that Work shall terminate
      as of the date such litigation is filed.

   4. Redistribution. You may reproduce and distribute copies of the
      Work or Derivative Works thereof in any medium, with or without
      modifications, and in Source or Object form, provided that You
      meet the following conditions:

      (a) You must give any other recipients of the Work or
          Derivative Works a copy of this License; and

      (b) You must cause any modified files to carry prominent notices
          stating that You changed the files; and

      (c) You must retain, in the Source form of any Derivative Works
          that You distribute, all copyright, patent, trademark, and
          attribution notices from the Source form of the Work,
          excluding those notices that do not pertain to any part of
          the Derivative Works; and

      (d) If the Work includes a "NOTICE" text file as part of its
          distribution, then any Derivative Works that You distribute must
          include a readable copy of the attribution notices contained
          within such NOTICE file, excluding those notices that do not
          pertain to any part of the Derivative Works, in at least one
          of the following places: within a NOTICE text file distributed
          as part of the Derivative Works; within the Source form or
          documentation, if provided along with the Derivative Works; or,
          within a display generated by the Derivative Works, if and
          wherever such third-party notices normally appear. The contents
          of the NOTICE file are for informational purposes only and
          do not modify the License. You may add Your own attribution
          notices within Derivative Works that You distribute, alongside
          or as an addendum to the NOTICE text from the Work, provided
          that such additional attribution notices cannot be construed
          as modifying the License.

      You may add Your own copyright statement to Your modifications and
      may provide additional or different license terms and conditions
      for use, reproduction, or distribution of Your modifications, or
      for any such Derivative Works as a whole, provided Your use,
      reproduction, and distribution of the Work otherwise complies with
      the conditions stated in this License.

   5. Submission of Contributions. Unless You explicitly state otherwise,
      any Contribution intentionally submitted for inclusion in the Work
      by You to the Licensor shall be under the terms and conditions of
      this License, without any additional terms or conditions.
      Notwithstanding the above, nothing herein shall supersede or modify
      the terms of any separate license agreement you may have executed
      with Licensor regarding such Contributions.

   6. Trademarks. This License does not grant permission to use the trade
      names, trademarks, service marks, or product names of the Licensor,
      except as required for reasonable and customary use in describing the
      origin of the Work and reproducing the content of the NOTICE file.

   7. Disclaimer of Warranty. Unless required by applicable law or
      agreed to in writing, Licensor provides the Work (and each
      Contributor provides its Contributions) on an "AS IS" BASIS,
      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
      implied, including, without limitation, any warranties or conditions
      of TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
      PARTICULAR PURPOSE. You are solely responsible for determining the
      appropriateness of using or redistributing the Work and assume any
      risks associated with Your exercise of permissions under this License.

   8. Limitation of Liability. In no event and under no legal theory,
      whether in tort (including negligence), contract, or otherwise,
      unless required by applicable law (such as deliberate and grossly
      negligent acts) or agreed to in writing, shall any Contributor be
      liable to You for damages, including any direct, indirect, special,
      incidental, or consequential damages of any character arising as a
      result of this License or out of the use or inability to use the
      Work (including but not limited to damages for loss of goodwill,
      work stoppage, computer failure or malfunction, or any and all
      other commercial damages or losses), even if such Contributor
      has been advised of the possibility of such damages.

   9. Accepting Warranty or Additional Liability. While redistributing
      the Work or Derivative Works thereof, You may choose to offer,
      and charge a fee for, acceptance of support, warranty, indemnity,
      or other liability obligations and/or rights consistent with this
      License. However, in accepting such obligations, You may act only
      on Your own behalf and on Your sole responsibility, not on behalf
      of any other Contributor, and only if You agree to indemnify,
      defend, and hold each Contributor harmless for any liability
      incurred by, or claims asserted against, such Contributor by reason
      of your accepting any such warranty or additional liability.

   END OF TERMS AND CONDITIONS

   APPENDIX: How to apply the Apache License to your work.

      To apply the Apache License to your work, attach the following
      boilerplate notice, with the fields enclosed by brackets "[]"
      replaced with your own identifying information. (Don't include
      the brackets!)  The text should be enclosed in the appropriate
      comment syntax for the file format. We also recommend that a
      file or class name and description of purpose be included on the
      same "printed page" as the copyright notice for easier
      identification within third-party archives.

   Copyright [yyyy] [name of copyright owner]

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
```

### README.md

```markdown
# CUTMACHINE

CUTMACHINE charge un document JSON de timeline, ouvre chaque source média par
son ULID, puis résout le scrub en clés de cache `(source_id, source_frame)`.
Les intervalles des clips sont semi-ouverts : `[timeline_in, timeline_in +
duration)`. Un trou ne déclenche aucun décodage et est rendu en noir.

## Build et tests

Prérequis : macOS, CMake 3.24+, pkg-config et FFmpeg (`libavformat`,
`libavcodec`, `libavutil`, `libswresample`).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Lancement

Placez `C8022.MP4` à côté de `example-timeline.json`, ou adaptez son champ
`path`, puis lancez :

```sh
./build/cutmachine ./example-timeline.json
```

Les chemins relatifs des sources sont résolus relativement au fichier JSON,
pas au répertoire courant du processus.

## Décisions de temps

- `RationalTime::rescale` refuse les conversions non exactes afin de ne jamais
  déplacer silencieusement un raccord.
- `to_frames` arrondit vers le bas vers la frame qui contient la position et
  accepte un taux rationnel, par exemple `30000/1001`.
- `TimelineViewport` est l'unique frontière temps/pixels. `TimeToX` conserve
  le calcul intermédiaire en `long double`; `XToTime(x, rate)` arrondit au tick
  le plus proche du timebase demandé, avec les demi-ticks à l'opposé de zéro.
  Dans le timebase NTSC `30000`, les frontières de frames `30000/1001` sont
  les multiples de `1001` ticks : leur aller-retour est donc exact. Ce choix
  symétrique rend aussi déterministes les vues dont `view_start` est négatif.
- `sources[].rate` est interprété comme une cadence rationnelle, tandis que le
  `rate` d'un `RationalTime` est un timebase entier. Ainsi, une frame à
  `30000/1001` dure `1001` ticks dans un timebase `30000`; cette convention est
  nécessaire car `RationalTime::rate` ne peut lui-même contenir un quotient.
- Le playhead possède deux grilles, basculées par `M`. En mode Image, sa
  position est arrondie à l'image la plus proche selon la cadence rationnelle
  de référence (la première source montée), y compris les multiples de `1001`
  à `30000/1001`. En mode
  Échantillon, elle est arrondie à l'échantillon 48 kHz le plus proche. Les
  demi-pas sont arrondis à l'opposé de zéro par calcul entier 128 bits ; aucun
  temps ne transite en `double` pour cette quantification.
- Les pistes vidéo sont classées par leur champ `index` et compositées du bas
  vers le haut. Un trou sur une piste supérieure révèle les pistes inférieures.

## Timeline graphique

La timeline est dessinée sous la vidéo dans le même `CAMetalLayer`. Les pistes,
clips visibles, trous, bordure de sélection, aperçu de trim, graduation et
playhead sont des primitives Metal ; seul le bandeau d'information sous la
surface est un label AppKit. Le hit-test travaille sur les rectangles calculés
par `TimelineViewport`, avec une zone de bord fixe de 6 points.

Un drag de bord ne modifie pas le document pendant le geste. Il construit un
aperçu, valide un `TrimClipOperation` sur une copie, puis émet au plus une
opération via `EditLog::Apply` au relâchement. Un aperçu invalide est rouge et
n'émet rien. `Cmd+Z` et `Cmd+Shift+Z` utilisent le même journal que le CLI.
Le corps d'un clip est déplaçable horizontalement ou vers une autre piste de
même type. `MoveClipOperation` conserve l'ULID, les temps source et la durée ;
une destination incompatible refuse le drop. Un chevauchement effectue un
overwrite non-ripple : les clips entièrement couverts disparaissent, les
intersections de bord sont retaillées et un clip traversé est séparé en deux
survivants. Les états exacts des pistes affectées sont journalisés pour que
l'undo/redo restaure les ULID et les représentations rationnelles à l'octet.
Les trims sont bornés avant affichage par le début de timeline, les limites de
la source, une durée minimale d'un tick et les clips voisins : la poignée ne
peut donc jamais traverser une limite valide. Les raccords proches s'aimantent
dans une zone fixe de 8 points et affichent un guide cyan ; `N` active ou coupe
ce magnétisme. Le clip sélectionné expose deux poignées jaunes de largeur fixe,
indépendantes du zoom.

Avec l'outil Sélection, un drag démarré dans une zone vide des pistes trace un
lasso cyan dans la surface Metal. Après un seuil de 4 points, tous les clips
dont le rectangle intersecte le lasso sont sélectionnés, y compris sur
plusieurs pistes ; leurs bordures restent marquées au relâchement. En dessous
du seuil, le geste reste un simple clic de déplacement du playhead. La
multi-sélection reste visuelle pour les clips sans relation. Pour un groupe
A/V lié, move, trim et suppression utilisent chacun une opération multi-clip
atomique afin qu'un geste ne puisse pas être partiellement appliqué.

Un clic dans un trou borné sélectionne sa plage exacte et l'affiche en cyan.
`Delete` ou `Backspace` raccorde alors la piste en décalant tous ses clips
suivants vers la gauche, sans déplacer les autres pistes. Cette fermeture de
trou est une unique `DeleteGapOperation` persistée dans l'event log ; elle est
donc annulable et rejouable à l'octet près.

La palette en haut à gauche expose les outils Sélection (`V`), Main (`H`),
Zoom (`Z`, avec `Option` pour dézoomer) et Lame (`C` ou `B`). La lame affiche
la future coupe en rouge et un clic dans un clip crée deux segments contigus,
avec un nouvel ULID stable pour celui de droite. La sélection permet aussi de
scrubber en continu dans les trous et la règle. `Espace` lance ou arrête la lecture ;
maintenu pendant un drag, il devient temporairement l'outil Main. `J`, `K` et
`L` contrôlent la lecture arrière, l'arrêt et la lecture avant. `F` cadre toute
la timeline, `+`/`-` zooment, Home/End rejoignent les extrémités et les flèches
gauche/droite avancent d'une frame (`Shift` : dix frames).
La ligne `+` sous les en-têtes est divisée en deux : bleu pour ajouter une
piste vidéo, vert pour ajouter une piste audio. `Cmd+Shift+T` ajoute une piste
vidéo et `Cmd+Option+Shift+T` une piste audio. La création est atomique et
annulable ; un clip peut ensuite être glissé vers une piste compatible avec
l'outil Sélection. Le player résout et compose toutes les pistes vidéo du
document, sans limite fonctionnelle fixée à deux couches.

## Audio

Les pistes audio embarquées dans les sources sont décodées par FFmpeg, puis
converties en PCM float stéréo 48 kHz par `libswresample`. Un `AVAudioSourceNode`
mixte en temps réel tous les clips actifs, qu'ils soient placés sur une piste
vidéo ou audio. Le callback audio lit un plan immuable construit depuis la
timeline et ne touche jamais directement au document éditable. `Espace` et
`J/K/L` pilotent simultanément image et son ; un seek, un trim, un move, une
fermeture de trou ou un undo reconstruit le plan de mixage au raccord exact.
Les échantillons additionnés sont limités dans `[-1, 1]` pour éviter un
dépassement numérique lors du mixage multipiste.

À l'ouverture d'un projet, tout clip vidéo dont la source contient du son est
séparé par défaut. L'application crée au besoin des pistes audio et émet une
`DetachAudioOperation` par clip dans l'event log : le rectangle audio reçoit
son propre ULID mais conserve exactement les mêmes `source_in`, durée et
`timeline_in`. Le clip vidéo passe à `include_audio:false`, donc le son n'est
jamais joué deux fois. Audio et image peuvent ensuite être déplacés, trimés,
coupés ou supprimés indépendamment. Cette normalisation est persistée et reste
annulable avec `Cmd+Z`. Le bouton **Séparer audio** (`U`) reste disponible pour
un éventuel clip lié ajouté ultérieurement.

Les rectangles vidéo utilisent une palette bleue, tandis que les rectangles
audio utilisent une palette verte, même lorsqu'ils proviennent du même média.
Chaque paire issue d'une séparation partage un `link_group_id` stable et une
référence de phase rationnelle exacte. Avec **Sélection liée : ON**, cliquer,
englober au lasso ou éditer un membre agit sur la paire ; le drag affiche les
deux aperçus et émet une seule opération atomique :
`MoveLinkedClipsOperation`, `TrimLinkedClipsOperation` ou
`RemoveLinkedClipsOperation`. Avec le
toggle désactivé, chaque rectangle reste indépendant. `Cmd+Shift+L` change le
mode et `Option` l'inverse pour un geste. Un déplacement audio indépendant
affiche sur le rectangle un badge signé en images (`+3f`) ou, pour un décalage
sub-frame, en échantillons (`-240smp`). Le retour à zéro est magnétique. Les
anciens projets déjà séparés sont migrés par une `SetClipLinkOperation`
atomique lorsque les temps source et timeline correspondent exactement.

Le déplacement manuel du playhead produit un scrub audio : chaque clic, drag
dans la règle ou dans un trou, et chaque pas au clavier déclenche un grain de
60 ms à la position demandée. Une enveloppe de 5 ms à l'entrée et à la sortie
évite les clics ; un nouveau mouvement remplace immédiatement le grain en cours
sans démarrer la lecture continue.
En mode Image, les événements souris restent dédupliqués après quantification :
tant que le curseur demeure dans la même frame, le grain n'est joué qu'une
seule fois. Il n'est réarmé qu'en entrant dans une autre frame, ce qui évite les
redémarrages très bruyants causés par les micro-mouvements sub-frame.
En mode Image, les flèches déplacent le playhead d'une image (`Shift` : dix) ;
en mode Échantillon, elles le déplacent d'un échantillon (`Shift` : dix). Le
bandeau inférieur rappelle en permanence la grille active.

## Chutiers

Le panneau **Médiathèque / Chutiers** occupe la gauche de la fenêtre, hors de
la surface de timeline Metal. Une arborescence affiche les chutiers imbriqués,
la racine et une vue de tous les médias. Un sélecteur propose une liste de
métadonnées ou une grille d'icônes ; le champ de recherche filtre les deux.
**+ Chutier** crée un enfant du chutier courant et **Supprimer** refuse un
chutier contenant encore des médias ou des enfants. **Déplacer le média dans
ce chutier** utilise `SetMediaBinOperation`. Toutes les mutations passent par
l'event log et suivent donc `Cmd+Z`/`Cmd+Shift+Z`. Les champs `bins`,
`parent_id` et `bin_id` sont persistés et exposés par `--describe`. Voir la
[`spécification des chutiers`](docs/BINS_SPEC.md).

Un double-clic sur un média, ou le bouton **Source**, l'ouvre dans le moniteur
Metal. Un drag depuis la liste ou la grille vers une piste vidéo crée une
`InsertClipOperation` journalisée avec l'ULID de la source, son `source_in`
zéro et sa durée rationnelle complète. Cliquer ensuite dans la timeline rend
le moniteur au programme. `--ingest` crée désormais le `DocumentSource`
montable ayant le même ULID stable que chaque nouveau média de bibliothèque.

L'application installe une barre de menus macOS native : **Édition**, **Clip**,
**Timeline** et **Lecture**. Le clic droit est contextuel : chutiers et médias
dans la médiathèque, clips, gaps et pistes dans la timeline. Les commandes de
montage utilisent les mêmes opérations que le CLI ; le renommage d'un chutier
est notamment une `RenameBinOperation` réversible. Un clip peut aussi être
retrouvé dans la médiathèque depuis son menu contextuel, comme le `Find in
Media Pool` des NLE classiques.

## API d'édition

`Operations.h` expose `InsertClipOperation`, `RemoveClipOperation`,
`TrimClipOperation`, `MoveClipOperation`, `DeleteGapOperation` et
`SplitClipOperation`, ainsi que les variantes liées de move, trim et remove et
`AddTrackOperation` pour le multipiste. La feuille de route comportementale est
décrite dans [`docs/NLE_TIMELINE_SPEC.md`](docs/NLE_TIMELINE_SPEC.md).
`JoinClipOperation` est l'inverse exact persisté d'une coupe. `EditLog::Apply`,
`Undo` et `Redo` renvoient un `EditError` nommé et garantissent l'atomicité du
document.

Le log conserve, dans les inverses Insert/Remove, les représentations exactes
des `timeline_in` affectés par le ripple. Cette métadonnée est nécessaire pour
restaurer les octets canoniques d'origine lorsque des timebases différentes
représentent le même instant. Elle ne constitue pas une opération supplémentaire.

## Commandes headless

Ces commandes s'exécutent avant toute initialisation d'AppKit, de Metal ou du
décodage média :

```sh
./build/cutmachine --describe ./example-timeline.json
./build/cutmachine --apply-op ./example-timeline.json \
  '{"type":"TrimClip","clip_id":"01K00000000000000000000003","edge":"Tail","delta":{"value":-1,"rate":25},"exact_clip":null}'
./build/cutmachine --ingest ./example-timeline.json ./rushes --recursive
```

`--describe` écrit uniquement la vue JSON condensée sur stdout, avec les blocs
distincts `timeline` et `library`. Les médias de bibliothèque ont des alias
`M1`, `M2`, etc. et restent disponibles lorsqu'ils sont montés (`in_use:true`).
`--ingest` ne lit que les en-têtes FFmpeg, conserve les cadences rationnelles
exactes, conserve la rotation de la display matrix et en déduit l'orientation
affichée. Le player reprobe aussi les sources montées au lancement : le bandeau
d'information expose codec, dimensions, orientation, rotation, cadence et
présence audio. Les vidéos portrait sont tournées puis ajustées au moniteur
avec leur ratio conservé et des bandes neutres, sans étirement. Ce cache de
présentation ne mute pas le document depuis l'interface. Les fichiers non vidéo
ou corrompus sont rapportés dans `errors` sans faire échouer le lot ; l'identité
idempotente est le chemin absolu résolu.

Le schéma courant du document est la version 2 et ajoute `library` à côté de
`sources`. Une version 1 reste lisible : ses sources sont promues en entrées de
bibliothèque avec les seules métadonnées historiques connues, puis enrichies
si leurs fichiers sont ingérés. Bibliothèque et source partagent l'ULID du
média ; l'ingest seul ne monte jamais de clip.

### Gestion colorimétrique

Le menu **Couleur** propose un preset direct **Sony S-Log3 → Rec.2020 HLG**
et un panneau avancé. La chaîne est persistée dans `color_management` au
niveau du projet : gamut et courbe d'entrée, matrice YCbCr, plage Full/Legal,
espace de grading, puis gamut et courbe de sortie. Le preset utilise la plage
Full imposée par la spécification Sony, S-Gamut3.Cine/S-Log3, ACEScct/AP1 comme
espace wide gamut de grading et une sortie Rec.2020/HLG. La plage et la matrice
peuvent aussi suivre automatiquement les métadonnées FFmpeg de chaque frame.

Chaque frame YUV planaire 8 à 16 bits est d'abord normalisée selon sa profondeur
et sa plage, puis l'IDT l'amène en ACES AP1. Les opérations de grading passent
par ACEScct ; la composition des pistes se fait ensuite en AP1 linéaire dans
une texture flottante 16 bits. Une seconde passe produit le signal HLG dans une
cible XR 10 bits. La couche Core Animation est annoncée en BT.2100 HLG avec ses
métadonnées EDR. Le blanc de l'interface est limité au blanc HDR de référence
(signal HLG 0,75), afin que la timeline reste à un niveau SDR confortable.

`--apply-op`
réutilise le format canonique de `SerializeOperation`, remplace le document de
façon transactionnelle et conserve le journal dans le fichier compagnon
`<document>.editlog.json`. En cas de refus, ni le document ni ce journal ne sont
modifiés.

## Sidecar conversationnel

Le pilote Python utilise uniquement la bibliothèque standard. Le backend est
sélectionné sans modifier le REPL :

```sh
# Ollama local
export CUTMACHINE_BACKEND=ollama
export CUTMACHINE_MODEL=qwen3:8b
python3 -m sidecar.repl ./example-timeline.json

# API Anthropic
export CUTMACHINE_BACKEND=anthropic
export CUTMACHINE_MODEL=claude-sonnet-4-5
export ANTHROPIC_API_KEY=...
python3 -m sidecar.repl ./example-timeline.json
```

Le sidecar charge aussi automatiquement le fichier `.env` à la racine du
projet, sans remplacer une variable déjà exportée par le shell. Ce fichier est
ignoré par Git.

Le planner demande au modèle une intention de trim (`Shorten` ou `Extend`) et
une quantité positive en frames ou secondes ; le signe et le timebase du
`TrimClip` sont calculés localement. Les références explicites de piste, de
clip, d'alias et de nom de source sont également résolues depuis la vue avant
validation. Un ULID proposé par le modèle n'est utilisé qu'en fallback lorsque
la formulation ne fournit pas une résolution déterministe unique.

Variables optionnelles : `CUTMACHINE_BINARY`, `CUTMACHINE_OLLAMA_URL`,
`CUTMACHINE_ANTHROPIC_URL`, `CUTMACHINE_OLLAMA_MODEL` et
`CUTMACHINE_ANTHROPIC_MODEL`. Les variables de modèle spécifiques prennent le
pas sur `CUTMACHINE_MODEL`. Chaque instruction est indépendante : le sidecar ne
conserve aucune conversation, ne propose qu'une opération et ne réessaie qu'une
fois après une erreur nommée du moteur.

Le corpus fixe de 15 instructions françaises permet de mesurer les changements
de prompt ou de modèle :

```sh
python3 -m sidecar.eval --backend ollama
python3 -m sidecar.eval --backend anthropic
python3 -m sidecar.eval --backend all
```

Le rapport affiche chaque comparaison et le taux de réussite séparément pour
chaque backend. Une évaluation parfaite retourne 0 ; toute divergence retourne 1.
```

### RESULTS.md

```markdown
# CUTMACHINE — résultats du spike XAVC S-I 4K

## Verdict

Le chemin technique est validé sur la machine de test Apple Silicon : deux
sources XAVC S-I 4K 25p sont décodées en logiciel par FFmpeg, transférées sous
forme de trois plans 10 bits chacune, converties en RGB et compositées par
Metal. L'application AppKit affiche le résultat et permet de changer de frame
avec un slider.

Le débit séquentiel rend deux pistes possibles. Le seek aléatoire n'est pas un
régime de fonctionnement viable à chaque frame et doit rester limité au
chargement, aux sauts lointains et aux vrais cache misses. Le cache et le
prefetch séquentiel sont donc des éléments nécessaires du produit, pas de
simples optimisations.

Le spike ne valide pas encore un scrub arrière fluide avec deux pistes. Cette
limite est localisée dans la reconstruction du cache en sens inverse sous
contention, et non dans le décodage séquentiel, l'upload ou le compositing GPU.

## Configuration testée

- macOS sur Apple Silicon, mémoire unifiée de 16 Go
- FFmpeg, décodage logiciel uniquement
- média Sony FX30 : 3840 × 2160, 25 fps, 168 frames
- H.264 High 4:2:2 Intra 10 bits, `yuv422p10le`, All-I
- `FF_THREAD_FRAME | FF_THREAD_SLICE` demandé
- FFmpeg a activé `FF_THREAD_FRAME` (`active_thread_type = 0x1`)
- 11 threads pour les mesures isolées initiales
- 5 threads par contexte pour le test à deux pistes
- cache global limité à 2,0 Go

Il n'y a ni VideoToolbox, ni autre hwaccel, ni `sws_scale`, ni conversion de
format sur le CPU.

## Résultats déterminants

| Mesure | Résultat | Interprétation |
|---|---:|---|
| Seek et décode à froid, accès aléatoire | 25,84 ms/frame | Latence d'un démarrage sans pipeline ; acceptable pour un saut ponctuel, pas pour chaque frame |
| Décode séquentiel, 168 frames, sans flush | 3,91 ms/frame | Le frame threading fonctionne ; facteur 6,6 face au régime à froid |
| Upload des trois plans avec `replaceRegion` | p95 2,71 ms/frame | L'upload synchrone n'est pas le goulot du spike |
| Estimation de deux pistes | 7,82 ms de décode + 5,42 ms d'upload | 13,24 ms avant compositing, dans un budget d'affichage de 16,67 ms mais avec peu de marge |

La mesure séquentielle jette les 20 premières frames afin de ne pas compter
l'amorçage du pipeline de frame threading. Aucun `avcodec_flush_buffers` n'est
effectué entre les frames. À l'inverse, le benchmark aléatoire effectue un
seek et un flush avant chaque échantillon : il mesure volontairement le coût à
froid.

La première lecture des 25,84 ms a failli conduire à déclarer la voie logicielle
inutilisable. Cette conclusion était incorrecte : le protocole détruisait le
pipeline de frame threading entre chaque mesure. Le chiffre décisif pour le
prefetch est le débit séquentiel de 3,91 ms/frame.

## Trace de scrub

Le protocole parcourt les 168 frames en avant, revient jusqu'à la première,
puis repart en avant. Les requêtes sont émises à 60 Hz, soit 502 échéances
d'affichage. Une frame est comptée comme drop si au moins une source ne possède
pas exactement l'index demandé à l'échéance ; le renderer affiche alors la
frame disponible la plus proche.

| Sources | Drops | Localisation |
|---|---:|---|
| Une piste | 5 | Tous pendant l'amorçage initial |
| Deux pistes | 75 | 2 au premier aller, 65 au retour, 8 au second aller |

Dans le dernier test dual, la fenêtre dérivée du budget est de 21 frames par
source : 16 dans la direction du prefetch, 4 de l'autre côté et la frame
courante. Le cache résident atteint environ 1,974 Go. Les 65 drops concentrés
sur le retour montrent que le prochain travail doit cibler le scrub inverse ;
une optimisation générale du renderer ne répondrait pas au signal observé.

ThreadSanitizer ne signale aucune race sur la trace complète à deux workers.
Les `AVFrame` conservées par le cache utilisent `av_frame_ref`/`av_frame_unref`,
et tous les appels Metal restent sur le thread principal avec une seule command
queue.

## Bugs silencieux révélés par l'instrumentation

### 1. Le prefetch s'auto-évinçait

La première fenêtre demandait 81 frames alors que le budget réel n'en gardait
qu'environ 74. Le worker décodait donc des frames que le LRU supprimait presque
immédiatement. Une variante du même défaut existait dans le prefetch inverse :
un nouveau bloc spéculatif était produit à chaque mouvement même si le bloc
précédent couvrait déjà la prochaine frontière.

La fenêtre est maintenant dérivée du budget : 70 % des frames finançables,
réparties entre les sources actives. Les 30 % restants couvrent la frame
affichée, les frames en vol, les références du renderer et la spéculation. Le
prefetch inverse ne produit un nouveau bloc que lorsque sa prochaine frontière
est réellement absente.

### 2. Une inversion redécodait des frames déjà en cache

Après un changement de direction, le cache pouvait contenir une longue plage
continue tandis que le curseur interne du décodeur pointait encore près de
l'ancien bloc. Au premier trou, le worker tentait de rejoindre ce trou en
redécodant séquentiellement toute la plage déjà en cache. Le hit rate restait
élevé, mais le prefetch perdait silencieusement la course avec le slider.

Le worker resynchronise désormais le décodeur directement sur le premier trou
si le rattrapage dépasse quatre frames. Cette correction a supprimé les drops
d'inversion sur la trace mono.

## Corrections apportées aux métriques

Les premiers percentiles de « latence de livraison » affichaient 0,000 ms. Ils
chronométraient en réalité un lookup dans la map sur des cache hits, pas une
livraison. Les métriques séparent maintenant :

- les hits, qui mesurent seulement l'accès au cache et restent proches de zéro ;
- les misses, mesurés entre la requête d'un index absent et son insertion dans
  le cache sous une forme affichable.

Le HUD expose aussi le hit rate glissant, les octets résidents, les drops
cumulés et les frames en vol. Sans cette séparation, un hit rate proche de
100 % masquait les rares misses responsables des drops visibles.

## Décisions conservées

- trois textures `MTLPixelFormatR16Unorm` par source, jamais deux ;
- `frame->linesize[i]` utilisé comme `bytesPerRow` ;
- remise à l'échelle `65535.0 / 1023.0` dans le shader ;
- conversion BT.709 video range et compositing exclusivement sur le GPU ;
- deux `AVCodecContext` indépendants à 5 threads pour deux pistes ;
- cache global de 2,0 Go, jamais un budget par piste ;
- affichage non bloquant : la frame exacte si disponible, sinon la plus proche.

## Suite éventuelle, hors du spike

Le spike s'arrête ici. Si le scrub arrière dual devient une exigence produit,
les expériences doivent être faites dans cet ordre :

1. Sur un miss arrière à l'index `N`, seek à `N - 24`, puis décoder les 24
   frames vers l'avant afin d'amortir un seek froid sur un bloc séquentiel.
2. Inverser dynamiquement la fenêtre avec la direction : environ 20 frames en
   arrière et 6 en avant pendant un retour, au lieu d'une répartition figée.
3. Mesurer seulement ensuite un contexte de seek dédié avec
   `FF_THREAD_SLICE`, si le média Sony contient effectivement plusieurs slices
   par frame.

La mesure slice threading n'est pas requise pour le verdict actuel et n'a pas
été réalisée. Elle ne doit donc pas être présentée comme un gain acquis.
```

### docs/BINS_SPEC.md

```markdown
# Spécification Médiathèque et chutiers

Le panneau reprend les conventions communes du Project panel de Premiere et du
Media Pool de Resolve, adaptées au modèle journalisé de CUTMACHINE.

## Comportement livré

- La colonne de gauche est une arborescence de chutiers imbriqués.
- `Tous les médias` donne une vue globale ; `Sans chutier` montre la racine.
- Créer un chutier lorsqu'un chutier est sélectionné crée un enfant.
- Un chutier ne peut être supprimé que s'il ne contient ni média ni enfant.
- Un sélecteur bascule entre une vue liste (nom, codec/résolution, durée) et
  une grille d'icônes, comme les modes de présentation du Finder.
- La recherche filtre immédiatement sur le nom, chemin, codec, orientation et
  dimensions, sans modifier le document.
- Un média sélectionné peut être déplacé dans le chutier courant. Le
  déplacement émet `SetMediaBinOperation` ; création et suppression utilisent
  `AddBinOperation` et `RemoveBinOperation`.
- `Cmd+Z`/`Cmd+Shift+Z` annulent et rejouent toutes ces mutations.
- Le clic droit sur un chutier permet de créer un enfant, le renommer ou le
  supprimer. `RenameBinOperation` rend le renommage sérialisable et annulable.
- Le clic droit sur un média permet de l'ouvrir dans Source, le déplacer vers
  le chutier sélectionné ou révéler son fichier dans le Finder.
- Un double-clic, ou le bouton `Source`, charge le média dans le moniteur Metal
  sans déplacer le playhead de programme.
- Un drag depuis la liste ou la grille vers une piste vidéo émet une unique
  `InsertClipOperation` avec `source_in = 0` et la durée source exacte.

Le choix hiérarchique suit les bins imbriqués du
[Media Pool Resolve](https://documents.blackmagicdesign.com/UserManuals/DaVinci_Resolve_14_Reference_Manual.pdf)
et le fait que les chutiers Premiere peuvent contenir médias et autres
chutiers selon la
[documentation Adobe](https://helpx.adobe.com/sg/premiere/desktop/organize-media/file-organization/bins-overview.html).
La vue liste et sa recherche s'appuient sur les comportements documentés du
[Project panel](https://helpx.adobe.com/premiere/desktop/get-started/customize-the-project-panel/customize-list-view-in-project-panel.html)
et de la
[recherche Premiere](https://helpx.adobe.com/premiere/desktop/organize-media/file-organization/search-options-in-premiere.html).

## Modèle JSON

Un `DocumentBin` possède un ULID, un nom et un `parent_id` facultatif. Un
`parent_id` vide désigne le premier niveau. Le validateur refuse les parents
inconnus, les auto-références et tout cycle. L'identité d'un média ne change
jamais lorsqu'il est reclassé.

```json
{"id":"01K…","name":"Jour 01","parent_id":"01J…"}
```

`parent_id` est omis lors de la sauvegarde pour les chutiers racine afin de
rester compatible avec les documents existants.

## Suite prévue

1. Glisser-déposer de médias et de chutiers dans l'arborescence, via une
   opération `SetBinParent` atomique.
2. Couleurs de labels et renommage direct dans l'arborescence.
3. Remplacer l'icône vidéo générique par une poster frame et un hover scrub.
4. Smart bins fondés sur des règles de métadonnées, sans dupliquer les médias.
5. Colonnes triables : cadence, orientation, audio, usage en timeline et
   statut online/offline.

Les Smart Bins resteront des vues calculées : leur contenu ne sera pas écrit
comme une seconde appartenance physique dans le document.
```

### docs/NLE_TIMELINE_SPEC.md

```markdown
# Spécification de montage timeline

Cette spécification traduit les conventions utiles de Premiere et Resolve dans
le modèle exact et journalisé de CUTMACHINE. Elle décrit des comportements,
pas une copie d'interface.

## Invariants

- Un geste de montage produit zéro ou une opération dans l'event log.
- Une opération multi-clip est atomique : si un membre échoue, le document et
  le log restent identiques octet pour octet.
- Tous les temps de montage sont des `RationalTime`. Seul
  `TimelineViewport` convertit temps et pixels.
- `Option` au début d'un geste inverse temporairement la sélection liée.
- L'aperçu peut être invalide et rouge, mais ne mute jamais le document.
- Overwrite, clear et ripple delete sont trois commandes distinctes.

## Menus et contexte

La barre macOS regroupe les commandes stables dans `Édition`, `Clip`,
`Timeline` et `Lecture`. Les menus contextuels sont construits au clic : leur
contenu dépend du panneau actif et de l'objet sous le pointeur, conformément à
la distinction entre menu de panneau et menu contextuel décrite par
[Premiere](https://helpx.adobe.com/premiere/desktop/get-started/tour-the-workspace/display-panel-options-and-menu.html).

- Média : ouvrir dans Source, déplacer dans le chutier courant, révéler dans
  le Finder.
- Chutier : créer un enfant, renommer, supprimer s'il est vide.
- Clip : ouvrir dans Source, retrouver dans la médiathèque, couper au point du
  clic, séparer l'audio, supprimer.
- Gap : fermer le gap.
- Piste ou fond : ajouter une piste et cadrer la timeline.

Les actions de montage passent par l'event log. Les commandes de présentation
comme `Révéler dans le Finder` ne mutent pas le document. Le raccourci affiché
dans le menu et le raccourci clavier direct appellent la même méthode.

## Sémantique retenue

### Sélection et liens A/V

Avec la sélection liée active, cliquer un membre sélectionne son groupe. Move,
trim, blade et delete doivent agir sur tous les membres concernés en une seule
transaction. Un montage indépendant conserve le décalage A/V exact et affiche
son badge. Cette règle reprend la sélection liée et son override `Option`
décrits par [Premiere](https://helpx.adobe.com/premiere/desktop/edit-projects/change-clip-sequence/select-clips.html),
ainsi que le blade appliqué aux parties audio et vidéo lorsque la sélection
liée est active dans le
[guide Resolve](https://documents.blackmagicdesign.com/UserManuals/DaVinci-Resolve-18-Editors-Guide.pdf).

État : move, trim et delete liés sont implémentés. Le blade lié est le prochain
lot ; il devra créer un nouveau groupe stable pour les segments droits afin
que les deux côtés du raccord ne deviennent pas un unique groupe de quatre
clips.

### Suppression et ripple

- `Clear` retire la sélection et laisse son intervalle libre.
- `RippleDelete` retire la sélection puis décale les éléments suivants sur les
  pistes participant au ripple.
- Supprimer un gap sélectionné ferme uniquement ce gap dans la version
  actuelle.

Premiere distingue explicitement delete et ripple delete, ce dernier décalant
les clips suivants pour fermer le trou :
[documentation Adobe](https://helpx.adobe.com/au/premiere/desktop/edit-projects/change-clip-sequence/remove-clips-from-a-sequence.html).

État : la fermeture de gap et la suppression liée sont atomiques. La
séparation `Clear`/`RippleDelete`, aujourd'hui confondue par `RemoveClip`, est
prioritaire avant les outils de trim avancés.

### Verrouillages de piste

- `track_lock` interdit toute mutation de la piste, mais pas sa lecture.
- `sync_lock` décide si la piste suit une insertion, un ripple trim ou un
  ripple delete effectué ailleurs.
- Une piste directement éditée participe toujours au ripple ; `sync_lock`
  concerne les autres pistes.

Cette distinction est conforme au
[Track Lock Adobe](https://helpx.adobe.com/ie/premiere/desktop/edit-projects/change-clip-sequence/track-lock-to-prevent-changes.html),
au [Sync Lock Adobe](https://helpx.adobe.com/premiere/desktop/edit-projects/change-clip-sequence/sync-lock-to-prevent-changes.html)
et au contrôle indépendant ajouté dans
[Resolve 20.2](https://documents.blackmagicdesign.com/SupportNotes/DaVinci_Resolve_20.2_New_Features_Guide.pdf).

État : spécifié, non implémenté. Le JSON devra porter les deux booléens et les
toggles devront émettre `SetTrackLock`/`SetTrackSyncLock`, jamais muter le
document directement.

### Outils de trim

1. Trim normal : change un bord et laisse un gap ou refuse un overlap.
2. Ripple trim : change un bord et translate les clips aval des pistes en
   sync lock.
3. Roll : déplace un raccord partagé sans changer la durée totale.
4. Slip : conserve le rectangle timeline et translate `source_in`.
5. Slide : déplace le clip en compensant les deux raccords voisins.

Le ripple trim doit décaler l'aval et mettre à jour la durée de séquence en
temps réel, comme le décrit
[Adobe](https://helpx.adobe.com/ca/premiere/desktop/edit-projects/trim-clips/perform-ripple-edits.html).
Chaque outil aura une opération dédiée avec snapshots exacts des pistes
affectées ; aucun ne sera simulé par plusieurs appels UI.

État : trim normal simple et lié implémenté. Ordre prévu : ripple, roll, slip,
slide.

## Priorités d'implémentation

### P0 — cohérence de montage

- Blade lié atomique avec nouveaux groupes gauche/droite.
- `ClearClips` et `RippleDeleteClips` multi-sélection.
- `track_lock` et `sync_lock`, avec icônes dans les en-têtes Metal.
- Navigation raccord précédent/suivant et nudge de sélection d'une image ou
  d'un échantillon.

### P1 — trim professionnel

- Ripple trim, roll edit, slip et slide.
- Sélection explicite d'un raccord et feedback de curseur par type de trim.
- Track targeting pour déterminer les pistes concernées par navigation,
  insert et match frame.

### P2 — assemblage et monitoring

- Source monitor avec In/Out, insert et overwrite vers pistes ciblées.
- Lift/extract sur une plage In/Out.
- Mute/solo/arm audio, niveaux et fondus de clip.
- Marqueurs de timeline et de clip.

Hors du prochain cycle : transitions, effets, titrage, multicam, proxies,
keyframes, scopes couleur et export final. Chacun nécessite un modèle métier
avant son interface.

## Arrondi temps/pixels

`XToTime(x, rate)` calcule en `long double`, multiplie par le timebase demandé,
puis arrondit au tick le plus proche avec les demi-ticks à l'opposé de zéro.
Le résultat est donc exactement sur la grille demandée. Pour `30000/1001`, on
demande le timebase `30000` et une frontière d'image est un multiple de `1001`
ticks. Le choix est symétrique pour un `view_start` négatif et rend
`XToTime(TimeToX(t), t.rate)` inverse à un tick près, sans laisser un temps de
document transiter en `double` hors du viewport.
```

### example-timeline.json

```json
{
  "version": 2,
  "color_management":{"enabled":true,"input_gamut":"sony_sgamut3_cine","input_transfer":"sony_slog3","input_ycbcr_matrix":"bt709","input_range":"full","working_gamut":"acescct","output_gamut":"rec2020","output_transfer":"hlg"},
  "library": [
    {"id":"01K00000000000000000000001","path":"C8022.MP4","filename":"C8022.MP4","bin_id":"01KZP5HMZ6ATPZ0RGVV6YMB34J","rate":{"num":25,"den":1},"duration":{"value":168,"rate":25}}
  ],
  "bins": [
    {"id":"01KZP5HMZ6ATPZ0RGVV6YMB34J","name":"1_rushes"}
  ],
  "sources": [
    {"id":"01K00000000000000000000001","path":"C8022.MP4","rate":{"num":25,"den":1},"duration":{"value":168,"rate":25}}
  ],
  "tracks": [
    {"id":"01K00000000000000000000002","kind":"video","index":0,"clips":[
      {"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":1}}
    ]},
    {"id":"01KZP24NHGCDW0PFTX01W9AJS2","kind":"video","index":1,"clips":[
      {"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":1}},
      {"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":20,"rate":25},"duration":{"value":15,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":1}},
      {"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":1}},
      {"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":41,"rate":25},"timeline_in":{"value":49,"rate":25},"include_audio":false,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":1}},
      {"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":90,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":1}}
    ]},
    {"id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","kind":"audio","index":2,"clips":[
      {"id":"01KZP5HDB6NVAS8DFZZYYA5XRA","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":25}},
      {"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":20,"rate":25},"duration":{"value":15,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},
      {"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},
      {"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":41,"rate":25},"timeline_in":{"value":49,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},
      {"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":90,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},
      {"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}
    ]},
    {"id":"01KZP4W7XXYCK7KAG6AJ8HX705","kind":"audio","index":3,"clips":[    ]}
  ]
}
```

### example-timeline.json.editlog.json

```json
{"version":1,"applied":[{"op":{"type":"TrimClip","clip_id":"01K00000000000000000000004","edge":"Tail","delta":{"value":-8,"rate":25},"exact_clip":{"source_in":{"value":120,"rate":25},"duration":{"value":4,"rate":25},"timeline_in":{"value":12,"rate":25}}},"inverse":{"type":"TrimClip","clip_id":"01K00000000000000000000004","edge":"Tail","delta":{"value":8,"rate":25},"exact_clip":{"source_in":{"value":120,"rate":25},"duration":{"value":12,"rate":25},"timeline_in":{"value":12,"rate":25}}}},{"op":{"type":"TrimClip","clip_id":"01K00000000000000000000004","edge":"Head","delta":{"value":1,"rate":25},"exact_clip":{"source_in":{"value":121,"rate":25},"duration":{"value":3,"rate":25},"timeline_in":{"value":13,"rate":25}}},"inverse":{"type":"TrimClip","clip_id":"01K00000000000000000000004","edge":"Head","delta":{"value":-1,"rate":25},"exact_clip":{"source_in":{"value":120,"rate":25},"duration":{"value":4,"rate":25},"timeline_in":{"value":12,"rate":25}}}},{"op":{"type":"TrimClip","clip_id":"01K00000000000000000000004","edge":"Tail","delta":{"value":33,"rate":25},"exact_clip":{"source_in":{"value":121,"rate":25},"duration":{"value":36,"rate":25},"timeline_in":{"value":13,"rate":25}}},"inverse":{"type":"TrimClip","clip_id":"01K00000000000000000000004","edge":"Tail","delta":{"value":-33,"rate":25},"exact_clip":{"source_in":{"value":121,"rate":25},"duration":{"value":3,"rate":25},"timeline_in":{"value":13,"rate":25}}}},{"op":{"type":"TrimClip","clip_id":"01K00000000000000000000004","edge":"Head","delta":{"value":13,"rate":25},"exact_clip":{"source_in":{"value":134,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":26,"rate":25}}},"inverse":{"type":"TrimClip","clip_id":"01K00000000000000000000004","edge":"Head","delta":{"value":-13,"rate":25},"exact_clip":{"source_in":{"value":121,"rate":25},"duration":{"value":36,"rate":25},"timeline_in":{"value":13,"rate":25}}}},{"op":{"type":"TrimClip","clip_id":"01K00000000000000000000004","edge":"Head","delta":{"value":-14,"rate":25},"exact_clip":{"source_in":{"value":120,"rate":25},"duration":{"value":37,"rate":25},"timeline_in":{"value":12,"rate":25}}},"inverse":{"type":"TrimClip","clip_id":"01K00000000000000000000004","edge":"Head","delta":{"value":14,"rate":25},"exact_clip":{"source_in":{"value":134,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":26,"rate":25}}}},{"op":{"type":"TrimClip","clip_id":"01K00000000000000000000004","edge":"Head","delta":{"value":4,"rate":25},"exact_clip":{"source_in":{"value":124,"rate":25},"duration":{"value":33,"rate":25},"timeline_in":{"value":16,"rate":25}}},"inverse":{"type":"TrimClip","clip_id":"01K00000000000000000000004","edge":"Head","delta":{"value":-4,"rate":25},"exact_clip":{"source_in":{"value":120,"rate":25},"duration":{"value":37,"rate":25},"timeline_in":{"value":12,"rate":25}}}},{"op":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":66,"rate":25},"exact_tracks":[]},"inverse":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":16,"rate":25},"exact_tracks":[]}},{"op":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":90,"rate":25},"exact_tracks":[]},"inverse":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":66,"rate":25},"exact_tracks":[]}},{"op":{"type":"TrimClip","clip_id":"01K00000000000000000000004","edge":"Head","delta":{"value":-70,"rate":25},"exact_clip":{"source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":20,"rate":25}}},"inverse":{"type":"TrimClip","clip_id":"01K00000000000000000000004","edge":"Head","delta":{"value":70,"rate":25},"exact_clip":{"source_in":{"value":124,"rate":25},"duration":{"value":33,"rate":25},"timeline_in":{"value":90,"rate":25}}}},{"op":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":40,"rate":25},"exact_tracks":[]},"inverse":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":20,"rate":25},"exact_tracks":[]}},{"op":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":207,"rate":25},"exact_tracks":[]},"inverse":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":40,"rate":25},"exact_tracks":[]}},{"op":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":97,"rate":25},"exact_tracks":[]},"inverse":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":207,"rate":25},"exact_tracks":[]}},{"op":{"type":"MoveClip","clip_id":"01K00000000000000000000003","track_id":"01K00000000000000000000002","timeline_in":{"value":62,"rate":25},"exact_tracks":[]},"inverse":{"type":"MoveClip","clip_id":"01K00000000000000000000003","track_id":"01K00000000000000000000002","timeline_in":{"value":0,"rate":25},"exact_tracks":[]}},{"op":{"type":"TrimClip","clip_id":"01K00000000000000000000003","edge":"Tail","delta":{"value":17,"rate":25},"exact_clip":{"source_in":{"value":0,"rate":25},"duration":{"value":29,"rate":25},"timeline_in":{"value":62,"rate":25}}},"inverse":{"type":"TrimClip","clip_id":"01K00000000000000000000003","edge":"Tail","delta":{"value":-17,"rate":25},"exact_clip":{"source_in":{"value":0,"rate":25},"duration":{"value":12,"rate":25},"timeline_in":{"value":62,"rate":25}}}},{"op":{"type":"MoveClip","clip_id":"01K00000000000000000000003","track_id":"01K00000000000000000000002","timeline_in":{"value":15,"rate":25},"exact_tracks":[]},"inverse":{"type":"MoveClip","clip_id":"01K00000000000000000000003","track_id":"01K00000000000000000000002","timeline_in":{"value":62,"rate":25},"exact_tracks":[]}},{"op":{"type":"TrimClip","clip_id":"01K00000000000000000000003","edge":"Tail","delta":{"value":53,"rate":25},"exact_clip":{"source_in":{"value":0,"rate":25},"duration":{"value":82,"rate":25},"timeline_in":{"value":15,"rate":25}}},"inverse":{"type":"TrimClip","clip_id":"01K00000000000000000000003","edge":"Tail","delta":{"value":-53,"rate":25},"exact_clip":{"source_in":{"value":0,"rate":25},"duration":{"value":29,"rate":25},"timeline_in":{"value":15,"rate":25}}}},{"op":{"type":"MoveClip","clip_id":"01K00000000000000000000003","track_id":"01K00000000000000000000002","timeline_in":{"value":0,"rate":25},"exact_tracks":[]},"inverse":{"type":"MoveClip","clip_id":"01K00000000000000000000003","track_id":"01K00000000000000000000002","timeline_in":{"value":15,"rate":25},"exact_tracks":[]}},{"op":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":82,"rate":25},"exact_tracks":[]},"inverse":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":97,"rate":25},"exact_tracks":[]}},{"op":{"type":"SplitClip","clip_id":"01K00000000000000000000003","timeline_position":{"value":50,"rate":25},"right_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6"},"inverse":{"type":"JoinClip","left_clip_id":"01K00000000000000000000003","right_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","joined_times":{"source_in":{"value":0,"rate":25},"duration":{"value":82,"rate":25},"timeline_in":{"value":0,"rate":25}}}},{"op":{"type":"MoveClip","clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","track_id":"01K00000000000000000000002","timeline_in":{"value":219,"rate":25},"exact_tracks":[]},"inverse":{"type":"MoveClip","clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","track_id":"01K00000000000000000000002","timeline_in":{"value":50,"rate":25},"exact_tracks":[]}},{"op":{"type":"MoveClip","clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","track_id":"01K00000000000000000000002","timeline_in":{"value":185,"rate":25},"exact_tracks":[]},"inverse":{"type":"MoveClip","clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","track_id":"01K00000000000000000000002","timeline_in":{"value":219,"rate":25},"exact_tracks":[]}},{"op":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":50,"rate":25},"exact_tracks":[]},"inverse":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":82,"rate":25},"exact_tracks":[]}},{"op":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":96,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":50,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":96,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":64,"rate":25},"duration":{"value":18,"rate":25},"timeline_in":{"value":199,"rate":25},"include_audio":true}]}]},"inverse":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":50,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":50,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":50,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":50,"rate":25},"duration":{"value":32,"rate":25},"timeline_in":{"value":185,"rate":25},"include_audio":true}]}]}},{"op":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":60,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":50,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":64,"rate":25},"duration":{"value":18,"rate":25},"timeline_in":{"value":199,"rate":25},"include_audio":true}]}]},"inverse":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":96,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":50,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":96,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":64,"rate":25},"duration":{"value":18,"rate":25},"timeline_in":{"value":199,"rate":25},"include_audio":true}]}]}},{"op":{"type":"MoveClip","clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","track_id":"01K00000000000000000000002","timeline_in":{"value":163,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":50,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":64,"rate":25},"duration":{"value":18,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]}]},"inverse":{"type":"MoveClip","clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","track_id":"01K00000000000000000000002","timeline_in":{"value":199,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":50,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":64,"rate":25},"duration":{"value":18,"rate":25},"timeline_in":{"value":199,"rate":25},"include_audio":true}]}]}},{"op":{"type":"TrimClip","clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","edge":"Tail","delta":{"value":18,"rate":25},"exact_clip":{"source_in":{"value":64,"rate":25},"duration":{"value":36,"rate":25},"timeline_in":{"value":163,"rate":25}}},"inverse":{"type":"TrimClip","clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","edge":"Tail","delta":{"value":-18,"rate":25},"exact_clip":{"source_in":{"value":64,"rate":25},"duration":{"value":18,"rate":25},"timeline_in":{"value":163,"rate":25}}}},{"op":{"type":"SplitClip","clip_id":"01K00000000000000000000003","timeline_position":{"value":27,"rate":25},"right_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV"},"inverse":{"type":"JoinClip","left_clip_id":"01K00000000000000000000003","right_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","joined_times":{"source_in":{"value":0,"rate":25},"duration":{"value":50,"rate":25},"timeline_in":{"value":0,"rate":25}}}},{"op":{"type":"DeleteGap","track_id":"01K00000000000000000000002","gap_start":{"value":50,"rate":25},"gap_duration":{"value":10,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":50,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":64,"rate":25},"duration":{"value":36,"rate":25},"timeline_in":{"value":153,"rate":25},"include_audio":true}]}]},"inverse":{"type":"DeleteGap","track_id":"01K00000000000000000000002","gap_start":{"value":50,"rate":25},"gap_duration":{"value":10,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":64,"rate":25},"duration":{"value":36,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]}]}},{"op":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":70,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":70,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":84,"rate":25},"duration":{"value":16,"rate":25},"timeline_in":{"value":173,"rate":25},"include_audio":true}]}]},"inverse":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":50,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":50,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":64,"rate":25},"duration":{"value":36,"rate":25},"timeline_in":{"value":153,"rate":25},"include_audio":true}]}]}},{"op":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":50,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":50,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":84,"rate":25},"duration":{"value":16,"rate":25},"timeline_in":{"value":173,"rate":25},"include_audio":true}]}]},"inverse":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":70,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":70,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":84,"rate":25},"duration":{"value":16,"rate":25},"timeline_in":{"value":173,"rate":25},"include_audio":true}]}]}},{"op":{"type":"MoveClip","clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","track_id":"01K00000000000000000000002","timeline_in":{"value":153,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":50,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":84,"rate":25},"duration":{"value":16,"rate":25},"timeline_in":{"value":153,"rate":25},"include_audio":true}]}]},"inverse":{"type":"MoveClip","clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","track_id":"01K00000000000000000000002","timeline_in":{"value":173,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":50,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":84,"rate":25},"duration":{"value":16,"rate":25},"timeline_in":{"value":173,"rate":25},"include_audio":true}]}]}},{"op":{"type":"MoveClip","clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","track_id":"01K00000000000000000000002","timeline_in":{"value":158,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":50,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":84,"rate":25},"duration":{"value":16,"rate":25},"timeline_in":{"value":158,"rate":25},"include_audio":true}]}]},"inverse":{"type":"MoveClip","clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","track_id":"01K00000000000000000000002","timeline_in":{"value":153,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":50,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":84,"rate":25},"duration":{"value":16,"rate":25},"timeline_in":{"value":153,"rate":25},"include_audio":true}]}]}},{"op":{"type":"TrimClip","clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","edge":"Head","delta":{"value":-5,"rate":25},"exact_clip":{"source_in":{"value":79,"rate":25},"duration":{"value":21,"rate":25},"timeline_in":{"value":153,"rate":25}}},"inverse":{"type":"TrimClip","clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","edge":"Head","delta":{"value":5,"rate":25},"exact_clip":{"source_in":{"value":84,"rate":25},"duration":{"value":16,"rate":25},"timeline_in":{"value":158,"rate":25}}}},{"op":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":60,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]}]},"inverse":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":50,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":50,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":79,"rate":25},"duration":{"value":21,"rate":25},"timeline_in":{"value":153,"rate":25},"include_audio":true}]}]}},{"op":{"type":"MoveClip","clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","track_id":"01K00000000000000000000002","timeline_in":{"value":37,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":37,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]}]},"inverse":{"type":"MoveClip","clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","track_id":"01K00000000000000000000002","timeline_in":{"value":27,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]}]}},{"op":{"type":"AddTrack","track_id":"01KZP24NHGCDW0PFTX01W9AJS2","kind":"video","index":1},"inverse":{"type":"RemoveTrack","track_id":"01KZP24NHGCDW0PFTX01W9AJS2"}},{"op":{"type":"MoveClip","clip_id":"01K00000000000000000000003","track_id":"01KZP24NHGCDW0PFTX01W9AJS2","timeline_in":{"value":1,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":37,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]},{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":1,"rate":25},"include_audio":true}]}]},"inverse":{"type":"MoveClip","clip_id":"01K00000000000000000000003","track_id":"01K00000000000000000000002","timeline_in":{"value":0,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":37,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]},{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[]}]}},{"op":{"type":"MoveClip","clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","track_id":"01K00000000000000000000002","timeline_in":{"value":23,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":23,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]}]},"inverse":{"type":"MoveClip","clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","track_id":"01K00000000000000000000002","timeline_in":{"value":37,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":37,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]}]}},{"op":{"type":"MoveClip","clip_id":"01K00000000000000000000003","track_id":"01KZP24NHGCDW0PFTX01W9AJS2","timeline_in":{"value":0,"rate":25},"exact_tracks":[{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true}]}]},"inverse":{"type":"MoveClip","clip_id":"01K00000000000000000000003","track_id":"01KZP24NHGCDW0PFTX01W9AJS2","timeline_in":{"value":1,"rate":25},"exact_tracks":[{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":1,"rate":25},"include_audio":true}]}]}},{"op":{"type":"MoveClip","clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","track_id":"01K00000000000000000000002","timeline_in":{"value":19,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":19,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]}]},"inverse":{"type":"MoveClip","clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","track_id":"01K00000000000000000000002","timeline_in":{"value":23,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":23,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]}]}},{"op":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01KZP24NHGCDW0PFTX01W9AJS2","timeline_in":{"value":39,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":19,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]},{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":39,"rate":25},"include_audio":true}]}]},"inverse":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":60,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":19,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]},{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true}]}]}},{"op":{"type":"AddTrack","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","kind":"audio","index":2},"inverse":{"type":"RemoveTrack","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0"}},{"op":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":48,"rate":25},"exact_tracks":[{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true}]},{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":19,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":48,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]}]},"inverse":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01KZP24NHGCDW0PFTX01W9AJS2","timeline_in":{"value":39,"rate":25},"exact_tracks":[{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":39,"rate":25},"include_audio":true}]},{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":19,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]}]}},{"op":{"type":"MoveClip","clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","track_id":"01K00000000000000000000002","timeline_in":{"value":22,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":48,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]}]},"inverse":{"type":"MoveClip","clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","track_id":"01K00000000000000000000002","timeline_in":{"value":19,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":19,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":48,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]}]}},{"op":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":45,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":45,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]}]},"inverse":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":48,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":48,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]}]}},{"op":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":58,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":58,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]}]},"inverse":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":45,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":45,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]}]}},{"op":{"type":"TrimClip","clip_id":"01K00000000000000000000004","edge":"Head","delta":{"value":60,"rate":25},"exact_clip":{"source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":118,"rate":25}}},"inverse":{"type":"TrimClip","clip_id":"01K00000000000000000000004","edge":"Head","delta":{"value":-60,"rate":25},"exact_clip":{"source_in":{"value":54,"rate":25},"duration":{"value":103,"rate":25},"timeline_in":{"value":58,"rate":25}}}},{"op":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":51,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]}]},"inverse":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":118,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":118,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]}]}},{"op":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01KZP24NHGCDW0PFTX01W9AJS2","timeline_in":{"value":51,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]},{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true}]}]},"inverse":{"type":"MoveClip","clip_id":"01K00000000000000000000004","track_id":"01K00000000000000000000002","timeline_in":{"value":51,"rate":25},"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]},{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true}]}]}},{"op":{"type":"AddTrack","track_id":"01KZP4W7XXYCK7KAG6AJ8HX705","kind":"audio","index":3},"inverse":{"type":"RemoveTrack","track_id":"01KZP4W7XXYCK7KAG6AJ8HX705"}},{"op":{"type":"SplitClip","clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","timeline_position":{"value":30,"rate":25},"right_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ"},"inverse":{"type":"JoinClip","left_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","right_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","joined_times":{"source_in":{"value":27,"rate":25},"duration":{"value":23,"rate":25},"timeline_in":{"value":22,"rate":25}}}},{"op":{"type":"SplitClip","clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","timeline_position":{"value":37,"rate":25},"right_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8"},"inverse":{"type":"JoinClip","left_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","right_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","joined_times":{"source_in":{"value":35,"rate":25},"duration":{"value":15,"rate":25},"timeline_in":{"value":30,"rate":25}}}},{"op":{"type":"DetachAudio","video_clip_id":"01K00000000000000000000004","audio_track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","audio_clip_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","exact_tracks":[{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":false}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true}]}]},"inverse":{"type":"DetachAudio","video_clip_id":"01K00000000000000000000004","audio_track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","audio_clip_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","exact_tracks":[{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[]}]}},{"op":{"type":"MoveClip","clip_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":60,"rate":25},"exact_tracks":[{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true}]}]},"inverse":{"type":"MoveClip","clip_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":51,"rate":25},"exact_tracks":[{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true}]}]}},{"op":{"type":"DetachAudio","video_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","audio_track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","audio_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":false},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":37,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true}]}]},"inverse":{"type":"DetachAudio","video_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","audio_track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","audio_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":37,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true}]}]}},{"op":{"type":"DetachAudio","video_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","audio_track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","audio_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":false},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":false},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":37,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true}]}]},"inverse":{"type":"DetachAudio","video_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","audio_track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","audio_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":false},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":37,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true}]}]}},{"op":{"type":"DetachAudio","video_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","audio_track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","audio_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":false},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":false},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":37,"rate":25},"include_audio":false},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":37,"rate":25},"include_audio":true},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true}]}]},"inverse":{"type":"DetachAudio","video_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","audio_track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","audio_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":false},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":false},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":37,"rate":25},"include_audio":true},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true}]}]}},{"op":{"type":"DetachAudio","video_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","audio_track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","audio_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":false},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":false},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":37,"rate":25},"include_audio":false},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":false}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":37,"rate":25},"include_audio":true},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]}]},"inverse":{"type":"DetachAudio","video_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","audio_track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","audio_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":false},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":false},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":37,"rate":25},"include_audio":false},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":37,"rate":25},"include_audio":true},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true}]}]}},{"op":{"type":"DetachAudio","video_clip_id":"01K00000000000000000000003","audio_track_id":"01KZP4W7XXYCK7KAG6AJ8HX705","audio_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","exact_tracks":[{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":false},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":false}]},{"track_id":"01KZP4W7XXYCK7KAG6AJ8HX705","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XRA","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true}]}]},"inverse":{"type":"DetachAudio","video_clip_id":"01K00000000000000000000003","audio_track_id":"01KZP4W7XXYCK7KAG6AJ8HX705","audio_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","exact_tracks":[{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":false}]},{"track_id":"01KZP4W7XXYCK7KAG6AJ8HX705","clips":[]}]}},{"op":{"type":"AddBin","bin_id":"01KZP5HMZ6ATPZ0RGVV6YMB34J","name":"1_rushes","parent_id":""},"inverse":{"type":"RemoveBin","bin_id":"01KZP5HMZ6ATPZ0RGVV6YMB34J","name":"1_rushes","parent_id":""}},{"op":{"type":"SetMediaBin","media_id":"01K00000000000000000000001","bin_id":"01KZP5HMZ6ATPZ0RGVV6YMB34J"},"inverse":{"type":"SetMediaBin","media_id":"01K00000000000000000000001","bin_id":""}},{"op":{"type":"MoveClip","clip_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":51,"rate":25},"exact_tracks":[{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":37,"rate":25},"include_audio":true},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]}]},"inverse":{"type":"MoveClip","clip_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":60,"rate":25},"exact_tracks":[{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":37,"rate":25},"include_audio":true},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":60,"rate":25},"include_audio":true},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true}]}]}},{"op":{"type":"SetClipLink","first_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","second_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","first_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","second_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","exact_first":null,"exact_second":null},"inverse":{"type":"SetClipLink","first_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","second_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","first_group_id":"","second_group_id":"","exact_first":null,"exact_second":null}},{"op":{"type":"SetClipLink","first_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","second_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","first_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","second_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","exact_first":null,"exact_second":null},"inverse":{"type":"SetClipLink","first_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","second_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","first_group_id":"","second_group_id":"","exact_first":null,"exact_second":null}},{"op":{"type":"SetClipLink","first_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","second_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","first_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","second_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","exact_first":null,"exact_second":null},"inverse":{"type":"SetClipLink","first_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","second_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","first_group_id":"","second_group_id":"","exact_first":null,"exact_second":null}},{"op":{"type":"SetClipLink","first_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","second_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","first_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","second_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","exact_first":null,"exact_second":null},"inverse":{"type":"SetClipLink","first_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","second_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","first_group_id":"","second_group_id":"","exact_first":null,"exact_second":null}},{"op":{"type":"SetClipLink","first_clip_id":"01K00000000000000000000003","second_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","first_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","second_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","exact_first":null,"exact_second":null},"inverse":{"type":"SetClipLink","first_clip_id":"01K00000000000000000000003","second_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","first_group_id":"","second_group_id":"","exact_first":null,"exact_second":null}},{"op":{"type":"SetClipLink","first_clip_id":"01K00000000000000000000004","second_clip_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","first_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","second_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","exact_first":null,"exact_second":null},"inverse":{"type":"SetClipLink","first_clip_id":"01K00000000000000000000004","second_clip_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","first_group_id":"","second_group_id":"","exact_first":null,"exact_second":null}},{"op":{"type":"SetClipLink","first_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","second_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","first_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","second_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","exact_first":{"group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","reference_delta":{"value":0,"rate":1}},"exact_second":{"group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","reference_delta":{"value":0,"rate":25}}},"inverse":{"type":"SetClipLink","first_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","second_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","first_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","second_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","exact_first":{"group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","anchor_clip_id":"","reference_delta":{"value":0,"rate":1}},"exact_second":{"group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","anchor_clip_id":"","reference_delta":{"value":0,"rate":1}}}},{"op":{"type":"SetClipLink","first_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","second_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","first_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","second_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","exact_first":{"group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","reference_delta":{"value":0,"rate":1}},"exact_second":{"group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","reference_delta":{"value":0,"rate":25}}},"inverse":{"type":"SetClipLink","first_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","second_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","first_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","second_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","exact_first":{"group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","anchor_clip_id":"","reference_delta":{"value":0,"rate":1}},"exact_second":{"group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","anchor_clip_id":"","reference_delta":{"value":0,"rate":1}}}},{"op":{"type":"SetClipLink","first_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","second_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","first_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","second_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","exact_first":{"group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","reference_delta":{"value":0,"rate":1}},"exact_second":{"group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","reference_delta":{"value":0,"rate":25}}},"inverse":{"type":"SetClipLink","first_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","second_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","first_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","second_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","exact_first":{"group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","anchor_clip_id":"","reference_delta":{"value":0,"rate":1}},"exact_second":{"group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","anchor_clip_id":"","reference_delta":{"value":0,"rate":1}}}},{"op":{"type":"SetClipLink","first_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","second_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","first_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","second_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","exact_first":{"group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","reference_delta":{"value":0,"rate":1}},"exact_second":{"group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","reference_delta":{"value":0,"rate":25}}},"inverse":{"type":"SetClipLink","first_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","second_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","first_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","second_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","exact_first":{"group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","anchor_clip_id":"","reference_delta":{"value":0,"rate":1}},"exact_second":{"group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","anchor_clip_id":"","reference_delta":{"value":0,"rate":1}}}},{"op":{"type":"SetClipLink","first_clip_id":"01K00000000000000000000003","second_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","first_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","second_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","exact_first":{"group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","anchor_clip_id":"01K00000000000000000000003","reference_delta":{"value":0,"rate":1}},"exact_second":{"group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","anchor_clip_id":"01K00000000000000000000003","reference_delta":{"value":0,"rate":25}}},"inverse":{"type":"SetClipLink","first_clip_id":"01K00000000000000000000003","second_clip_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","first_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","second_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","exact_first":{"group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","anchor_clip_id":"","reference_delta":{"value":0,"rate":1}},"exact_second":{"group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","anchor_clip_id":"","reference_delta":{"value":0,"rate":1}}}},{"op":{"type":"SetClipLink","first_clip_id":"01K00000000000000000000004","second_clip_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","first_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","second_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","exact_first":{"group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","anchor_clip_id":"01K00000000000000000000004","reference_delta":{"value":0,"rate":1}},"exact_second":{"group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","anchor_clip_id":"01K00000000000000000000004","reference_delta":{"value":0,"rate":25}}},"inverse":{"type":"SetClipLink","first_clip_id":"01K00000000000000000000004","second_clip_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","first_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","second_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","exact_first":{"group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","anchor_clip_id":"","reference_delta":{"value":0,"rate":1}},"exact_second":{"group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","anchor_clip_id":"","reference_delta":{"value":0,"rate":1}}}},{"op":{"type":"MoveLinkedClips","link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","moves":[{"clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","track_id":"01K00000000000000000000002","timeline_in":{"value":97,"rate":25}},{"clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":97,"rate":25}}],"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":97,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":97,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]},"inverse":{"type":"MoveLinkedClips","link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","moves":[{"clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","track_id":"01K00000000000000000000002","timeline_in":{"value":37,"rate":25}},{"clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":37,"rate":25}}],"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":37,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":37,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]}},{"op":{"type":"MoveLinkedClips","link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","moves":[{"clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","track_id":"01KZP24NHGCDW0PFTX01W9AJS2","timeline_in":{"value":102,"rate":25}},{"clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":102,"rate":25}}],"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":1}},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":false,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":102,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":102,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]},"inverse":{"type":"MoveLinkedClips","link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","moves":[{"clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","track_id":"01K00000000000000000000002","timeline_in":{"value":97,"rate":25}},{"clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":97,"rate":25}}],"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":97,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":1}},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":false,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":97,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]}},{"op":{"type":"MoveLinkedClips","link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","moves":[{"clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","track_id":"01KZP24NHGCDW0PFTX01W9AJS2","timeline_in":{"value":99,"rate":25}},{"clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":99,"rate":25}}],"exact_tracks":[{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":1}},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":false,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":99,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":99,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]},"inverse":{"type":"MoveLinkedClips","link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","moves":[{"clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","track_id":"01KZP24NHGCDW0PFTX01W9AJS2","timeline_in":{"value":102,"rate":25}},{"clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":102,"rate":25}}],"exact_tracks":[{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":1}},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":false,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":102,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":102,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]}},{"op":{"type":"MoveClip","clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":102,"rate":25},"exact_tracks":[{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":102,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]},"inverse":{"type":"MoveClip","clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":99,"rate":25},"exact_tracks":[{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":99,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]}},{"op":{"type":"MoveClip","clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":104,"rate":25},"exact_tracks":[{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":104,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]},"inverse":{"type":"MoveClip","clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":102,"rate":25},"exact_tracks":[{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":102,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]}},{"op":{"type":"MoveClip","clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":113,"rate":25},"exact_tracks":[{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":113,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]},"inverse":{"type":"MoveClip","clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":104,"rate":25},"exact_tracks":[{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":104,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]}},{"op":{"type":"MoveClip","clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":99,"rate":25},"exact_tracks":[{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":99,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]},"inverse":{"type":"MoveClip","clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":113,"rate":25},"exact_tracks":[{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":113,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]}},{"op":{"type":"MoveLinkedClips","link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","moves":[{"clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","track_id":"01KZP24NHGCDW0PFTX01W9AJS2","timeline_in":{"value":94,"rate":25}},{"clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":94,"rate":25}}],"exact_tracks":[{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":1}},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":false,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]},"inverse":{"type":"MoveLinkedClips","link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","moves":[{"clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","track_id":"01KZP24NHGCDW0PFTX01W9AJS2","timeline_in":{"value":99,"rate":25}},{"clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":99,"rate":25}}],"exact_tracks":[{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":1}},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":false,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":99,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":99,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]}},{"op":{"type":"MoveLinkedClips","link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","moves":[{"clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","track_id":"01KZP24NHGCDW0PFTX01W9AJS2","timeline_in":{"value":42,"rate":25}},{"clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":42,"rate":25}}],"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":1}},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":false,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]},"inverse":{"type":"MoveLinkedClips","link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","moves":[{"clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","track_id":"01K00000000000000000000002","timeline_in":{"value":30,"rate":25}},{"clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":30,"rate":25}}],"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":1}},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":false,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":30,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]}},{"op":{"type":"MoveLinkedClips","link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","moves":[{"clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","track_id":"01KZP24NHGCDW0PFTX01W9AJS2","timeline_in":{"value":34,"rate":25}},{"clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":34,"rate":25}}],"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":34,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":1}},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":false,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":34,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]},"inverse":{"type":"MoveLinkedClips","link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","moves":[{"clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","track_id":"01K00000000000000000000002","timeline_in":{"value":22,"rate":25}},{"clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":22,"rate":25}}],"exact_tracks":[{"track_id":"01K00000000000000000000002","clips":[{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZNZQCJSBKN94PZ67HQD54A6","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":1}},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":false,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":22,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]}},{"op":{"type":"MoveLinkedClips","link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","moves":[{"clip_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":0,"rate":25}},{"clip_id":"01K00000000000000000000003","track_id":"01KZP24NHGCDW0PFTX01W9AJS2","timeline_in":{"value":0,"rate":25}}],"exact_tracks":[{"track_id":"01KZP4W7XXYCK7KAG6AJ8HX705","clips":[]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XRA","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":34,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]},{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":34,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":1}},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":false,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":1}}]}]},"inverse":{"type":"MoveLinkedClips","link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","moves":[{"clip_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","track_id":"01KZP4W7XXYCK7KAG6AJ8HX705","timeline_in":{"value":0,"rate":25}},{"clip_id":"01K00000000000000000000003","track_id":"01KZP24NHGCDW0PFTX01W9AJS2","timeline_in":{"value":0,"rate":25}}],"exact_tracks":[{"track_id":"01KZP4W7XXYCK7KAG6AJ8HX705","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XRA","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":25}}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":34,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]},{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":34,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":1}},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":false,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":1}}]}]}},{"op":{"type":"TrimLinkedClips","link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","trims":[{"clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","edge":"Head","delta":{"value":-7,"rate":25}},{"clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","edge":"Head","delta":{"value":-7,"rate":25}}],"exact_tracks":[{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":20,"rate":25},"duration":{"value":15,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":1}},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":false,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XRA","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":20,"rate":25},"duration":{"value":15,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]},"inverse":{"type":"TrimLinkedClips","link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","trims":[{"clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","edge":"Head","delta":{"value":-7,"rate":25}},{"clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","edge":"Head","delta":{"value":-7,"rate":25}}],"exact_tracks":[{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":34,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":1}},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":false,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XRA","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":27,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":34,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]}},{"op":{"type":"MoveLinkedClips","link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","moves":[{"clip_id":"01K00000000000000000000004","track_id":"01KZP24NHGCDW0PFTX01W9AJS2","timeline_in":{"value":49,"rate":25}},{"clip_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":49,"rate":25}}],"exact_tracks":[{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":20,"rate":25},"duration":{"value":15,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":1}},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":49,"rate":25},"include_audio":false,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XRA","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":20,"rate":25},"duration":{"value":15,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":49,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]},"inverse":{"type":"MoveLinkedClips","link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","moves":[{"clip_id":"01K00000000000000000000004","track_id":"01KZP24NHGCDW0PFTX01W9AJS2","timeline_in":{"value":51,"rate":25}},{"clip_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":51,"rate":25}}],"exact_tracks":[{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":20,"rate":25},"duration":{"value":15,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":1}},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":false,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XRA","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":20,"rate":25},"duration":{"value":15,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":51,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]}},{"op":{"type":"MoveLinkedClips","link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","moves":[{"clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","track_id":"01KZP24NHGCDW0PFTX01W9AJS2","timeline_in":{"value":90,"rate":25}},{"clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":90,"rate":25}}],"exact_tracks":[{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":20,"rate":25},"duration":{"value":15,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":1}},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":41,"rate":25},"timeline_in":{"value":49,"rate":25},"include_audio":false,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":90,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XRA","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":20,"rate":25},"duration":{"value":15,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":41,"rate":25},"timeline_in":{"value":49,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":90,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]},"inverse":{"type":"MoveLinkedClips","link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","moves":[{"clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","track_id":"01KZP24NHGCDW0PFTX01W9AJS2","timeline_in":{"value":94,"rate":25}},{"clip_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","timeline_in":{"value":94,"rate":25}}],"exact_tracks":[{"track_id":"01KZP24NHGCDW0PFTX01W9AJS2","clips":[{"id":"01K00000000000000000000003","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP0CCXEGFEMVRSBS5RNB0NV","source_id":"01K00000000000000000000001","source_in":{"value":20,"rate":25},"duration":{"value":15,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WE5C3RBV23VP6QPXCJXJ","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":1}},{"id":"01K00000000000000000000004","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":49,"rate":25},"include_audio":false,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":1}},{"id":"01KZP4WEYCZKTMQZWSTJT7JKW8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":false,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":1}}]},{"track_id":"01KZP2TMVHPETKQ51ZQ8FMWXC0","clips":[{"id":"01KZP5HDB6NVAS8DFZZYYA5XRA","source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XRA","sync_anchor_clip_id":"01K00000000000000000000003","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR6","source_id":"01K00000000000000000000001","source_in":{"value":20,"rate":25},"duration":{"value":15,"rate":25},"timeline_in":{"value":27,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR6","sync_anchor_clip_id":"01KZP0CCXEGFEMVRSBS5RNB0NV","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR7","source_id":"01K00000000000000000000001","source_in":{"value":35,"rate":25},"duration":{"value":7,"rate":25},"timeline_in":{"value":42,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR7","sync_anchor_clip_id":"01KZP4WE5C3RBV23VP6QPXCJXJ","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","source_id":"01K00000000000000000000001","source_in":{"value":114,"rate":25},"duration":{"value":43,"rate":25},"timeline_in":{"value":49,"rate":25},"include_audio":true,"link_group_id":"01KZP4WMMRTSB2AZ7ASQZV8AX7","sync_anchor_clip_id":"01K00000000000000000000004","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR8","source_id":"01K00000000000000000000001","source_in":{"value":42,"rate":25},"duration":{"value":8,"rate":25},"timeline_in":{"value":94,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR8","sync_anchor_clip_id":"01KZP4WEYCZKTMQZWSTJT7JKW8","sync_reference_delta":{"value":0,"rate":25}},{"id":"01KZP5HDB6NVAS8DFZZYYA5XR9","source_id":"01K00000000000000000000001","source_in":{"value":89,"rate":25},"duration":{"value":11,"rate":25},"timeline_in":{"value":163,"rate":25},"include_audio":true,"link_group_id":"01KZP5HDB6NVAS8DFZZYYA5XR9","sync_anchor_clip_id":"01KZNZQCJSBKN94PZ67HQD54A6","sync_reference_delta":{"value":0,"rate":25}}]}]}}],"undone":[{"op":{"type":"SplitClip","clip_id":"01K00000000000000000000003","timeline_position":{"value":23,"rate":25},"right_clip_id":"01KZPAFHG6JJFN5HGD4ZHE9F1A"},"inverse":{"type":"JoinClip","left_clip_id":"01K00000000000000000000003","right_clip_id":"01KZPAFHG6JJFN5HGD4ZHE9F1A","joined_times":{"source_in":{"value":0,"rate":25},"duration":{"value":27,"rate":25},"timeline_in":{"value":0,"rate":25}}}}]}
```

### scripts/export_for_ai.py

```python
#!/usr/bin/env python3
"""
export_for_ai.py — Bundle this repo's source into one text file for an LLM.

Goal: produce a single, readable file an AI can be fed as context, while
leaving out anything that isn't source code (build output, CI/lint tooling,
media, lockfiles, IDE cruft) and anything that could be a secret (.env,
private keys, credentials, tokens found inside otherwise-normal files).

Usage:
    python3 scripts/export_for_ai.py
    python3 scripts/export_for_ai.py -o context.md --max-file-kb 500
    python3 scripts/export_for_ai.py --include-tooling   # keep CI/lint configs too
    python3 scripts/export_for_ai.py --no-git            # don't rely on .gitignore

By default it lists candidate files with `git ls-files -co --exclude-standard`
(tracked + untracked-but-not-ignored, i.e. it already honours .gitignore),
then layers its own filters on top — because things like .clang-format,
.github/workflows/*, or a committed sample .env would otherwise sail through.

No third-party dependencies; stdlib only.
"""

from __future__ import annotations

import argparse
import fnmatch
import re
import subprocess
import sys
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

# --------------------------------------------------------------------------
# Filters
# --------------------------------------------------------------------------

# Directories that are never source, regardless of what git tracks.
ALWAYS_EXCLUDE_DIRS = {
    ".git", ".tools", ".firecrawl", "build", "cmake-build-debug",
    "cmake-build-release", "node_modules", ".venv", "venv", "__pycache__",
    ".idea", ".vscode", "dist", "out", "Testing", "CMakeFiles", ".dSYM",
}

# Dev/CI/lint tooling — excluded by default because it's config, not the
# program logic. Re-include with --include-tooling.
TOOLING_PATH_GLOBS = [
    ".github/*", ".github/**/*",
    ".clang-format", ".clang-tidy",
    "Brewfile",
    ".gitignore", ".gitattributes",
    "CMakeUserPresets.json", "compile_commands.json",
    "Makefile", "DartConfiguration.tcl",
]

# File-name patterns that are always treated as secrets and always skipped,
# even with --include-tooling. No flag re-includes these.
SECRET_NAME_GLOBS = [
    ".env", ".env.*", "*.env",
    "*.pem", "*.key", "*.p12", "*.pfx", "*.jks", "*.keystore",
    "id_rsa*", "id_dsa*", "id_ecdsa*", "id_ed25519*", "*_rsa",
    ".netrc", ".npmrc", ".pypirc",
    "credentials*.json", "secrets*.*", "*.asc",
    "*apikey*", "*api_key*",
]

# Binary / media / build-artifact extensions — never useful as text context.
BINARY_EXTENSIONS = {
    ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico", ".icns", ".webp", ".tiff",
    ".mp4", ".mov", ".mxf", ".avi", ".mkv", ".m4v",
    ".wav", ".mp3", ".aac", ".m4a", ".flac",
    ".zip", ".tar", ".gz", ".tgz", ".7z", ".rar",
    ".ttf", ".otf", ".woff", ".woff2",
    ".o", ".a", ".dylib", ".so", ".dll", ".exe", ".bin",
    ".pdf", ".psd", ".sketch", ".fig",
    ".db", ".sqlite", ".sqlite3",
    ".ds_store",
}

# Regexes that flag likely secrets embedded in otherwise-normal source files.
# Matches get redacted in place; the file itself is still included.
SECRET_CONTENT_PATTERNS = [
    ("AWS Access Key ID", re.compile(r"AKIA[0-9A-Z]{16}")),
    ("AWS Secret Key", re.compile(r"(?i)aws_secret_access_key\s*[:=]\s*['\"]?[A-Za-z0-9/+=]{40}['\"]?")),
    ("Private key block", re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----[\s\S]*?-----END [A-Z ]*PRIVATE KEY-----")),
    ("Slack token", re.compile(r"xox[baprs]-[0-9A-Za-z-]{10,}")),
    ("GitHub token", re.compile(r"gh[pousr]_[A-Za-z0-9]{36,}")),
    ("Google API key", re.compile(r"AIza[0-9A-Za-z_\-]{35}")),
    ("Generic bearer/JWT", re.compile(r"eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}")),
    ("Assigned secret-looking value", re.compile(
        r"(?i)\b(api[_-]?key|secret|token|password|passwd)\b\s*[:=]\s*['\"][^'\"\s]{12,}['\"]"
    )),
]

MAX_FILE_KB_DEFAULT = 300
MAX_TOTAL_MB_DEFAULT = 20

LANG_BY_EXT = {
    ".cc": "cpp", ".cpp": "cpp", ".h": "cpp", ".hpp": "cpp",
    ".mm": "objectivec", ".m": "objectivec",
    ".metal": "metal",
    ".py": "python", ".json": "json", ".md": "markdown",
    ".yml": "yaml", ".yaml": "yaml", ".sh": "bash", ".cmake": "cmake",
}


@dataclass
class Skipped:
    path: str
    reason: str


@dataclass
class Included:
    path: str
    size: int
    redactions: int = 0


@dataclass
class Report:
    included: list[Included] = field(default_factory=list)
    skipped: list[Skipped] = field(default_factory=list)


def list_candidate_files(root: Path, use_git: bool) -> tuple[list[Path], bool]:
    """Return (paths relative to root, whether git was actually used)."""
    if use_git:
        try:
            out = subprocess.run(
                ["git", "-C", str(root), "ls-files", "-co", "--exclude-standard"],
                capture_output=True, text=True, check=True,
            ).stdout
            files = [Path(line) for line in out.splitlines() if line.strip()]
            if files:
                return files, True
        except (subprocess.CalledProcessError, FileNotFoundError):
            pass
    # Fallback: manual walk, best-effort (no .gitignore awareness).
    files = []
    for p in root.rglob("*"):
        if p.is_dir():
            continue
        rel = p.relative_to(root)
        if any(part in ALWAYS_EXCLUDE_DIRS for part in rel.parts):
            continue
        files.append(rel)
    return files, False


def matches_any_glob(rel_posix: str, name: str, globs: list[str]) -> bool:
    return any(fnmatch.fnmatch(rel_posix, g) or fnmatch.fnmatch(name, g) for g in globs)


def is_binary_bytes(data: bytes) -> bool:
    if b"\x00" in data:
        return True
    # Heuristic: too many non-printable bytes -> treat as binary.
    text_chars = bytes(range(32, 127)) + b"\n\r\t\f\b"
    nontext = sum(b not in text_chars for b in data)
    return len(data) > 0 and nontext / len(data) > 0.30


def redact_secrets(text: str) -> tuple[str, int]:
    count = 0
    for label, pattern in SECRET_CONTENT_PATTERNS:
        def _sub(m: re.Match, label=label) -> str:
            nonlocal count
            count += 1
            return f"«REDACTED:{label}»"
        text = pattern.sub(_sub, text)
    return text, count


def build_report(root: Path, args: argparse.Namespace) -> tuple[Report, list[str]]:
    files, used_git = list_candidate_files(root, use_git=not args.no_git)
    output_path_resolved = (root / args.output).resolve()
    max_file_bytes = args.max_file_kb * 1024
    max_total_bytes = args.max_total_mb * 1024 * 1024

    report = Report()
    total_bytes = 0
    chunks: list[str] = []

    for rel in sorted(files, key=lambda p: p.as_posix()):
        rel_posix = rel.as_posix()
        abs_path = root / rel
        name = rel.name

        if abs_path.resolve() == output_path_resolved:
            continue
        if any(part in ALWAYS_EXCLUDE_DIRS for part in rel.parts):
            continue
        if not abs_path.is_file():
            continue

        if matches_any_glob(rel_posix, name, SECRET_NAME_GLOBS):
            report.skipped.append(Skipped(rel_posix, "secret filename pattern"))
            continue

        if not args.include_tooling and matches_any_glob(rel_posix, name, TOOLING_PATH_GLOBS):
            report.skipped.append(Skipped(rel_posix, "tooling/CI config"))
            continue

        if rel.suffix.lower() in BINARY_EXTENSIONS or name == ".DS_Store":
            report.skipped.append(Skipped(rel_posix, "binary/media extension"))
            continue

        try:
            raw = abs_path.read_bytes()
        except OSError as e:
            report.skipped.append(Skipped(rel_posix, f"unreadable ({e})"))
            continue

        if is_binary_bytes(raw[:8192]):
            report.skipped.append(Skipped(rel_posix, "binary content"))
            continue

        if len(raw) > max_file_bytes:
            report.skipped.append(
                Skipped(rel_posix, f"too large ({len(raw)//1024} KB > {args.max_file_kb} KB)")
            )
            continue

        if total_bytes + len(raw) > max_total_bytes:
            report.skipped.append(Skipped(rel_posix, "total size budget exceeded"))
            continue

        text = raw.decode("utf-8", errors="replace")
        if not args.no_secret_scan:
            text, n_redacted = redact_secrets(text)
        else:
            n_redacted = 0

        total_bytes += len(raw)
        report.included.append(Included(rel_posix, len(raw), n_redacted))

        lang = LANG_BY_EXT.get(rel.suffix.lower(), "")
        chunks.append(f"### {rel_posix}\n\n```{lang}\n{text.rstrip()}\n```\n")

    return report, chunks


def render_output(root: Path, report: Report, chunks: list[str], used_git: bool) -> str:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    total_kb = sum(f.size for f in report.included) // 1024
    redacted_files = [f for f in report.included if f.redactions]

    lines = [
        f"# Repo export for AI review",
        "",
        f"- Root: `{root}`",
        f"- Generated: {now}",
        f"- File list source: {'git ls-files (honours .gitignore)' if used_git else 'manual walk (no .gitignore support)'}",
        f"- Included: {len(report.included)} files, ~{total_kb} KB",
        f"- Skipped: {len(report.skipped)} files (see manifest at the end)",
    ]
    if redacted_files:
        lines.append(f"- ⚠️ Redacted probable secrets in {len(redacted_files)} file(s) — double-check these by hand.")
    lines += ["", "## File tree", "", "```"]
    lines += [f.path for f in report.included]
    lines += ["```", "", "## Files", ""]
    lines += chunks
    lines += ["## Skipped files (manifest)", "", "| Path | Reason |", "|---|---|"]
    lines += [f"| {s.path} | {s.reason} |" for s in report.skipped]
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("root", nargs="?", default=".", help="Repo root (default: current directory)")
    parser.add_argument("-o", "--output", default="repo_export.md", help="Output file (default: repo_export.md)")
    parser.add_argument("--max-file-kb", type=int, default=MAX_FILE_KB_DEFAULT, help="Skip individual files larger than this")
    parser.add_argument("--max-total-mb", type=int, default=MAX_TOTAL_MB_DEFAULT, help="Stop once total included size exceeds this")
    parser.add_argument("--include-tooling", action="store_true", help="Also include CI/lint/build config files")
    parser.add_argument("--no-secret-scan", action="store_true", help="Skip in-content secret redaction (not recommended)")
    parser.add_argument("--no-git", action="store_true", help="Don't use git ls-files; walk the filesystem instead")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    if not root.is_dir():
        print(f"error: {root} is not a directory", file=sys.stderr)
        return 1

    files, used_git = list_candidate_files(root, use_git=not args.no_git)
    report, chunks = build_report(root, args)
    output_text = render_output(root, report, chunks, used_git)

    out_path = root / args.output
    out_path.write_text(output_text, encoding="utf-8")

    print(f"Wrote {out_path} — {len(report.included)} files included, {len(report.skipped)} skipped.")
    redacted = [f for f in report.included if f.redactions]
    if redacted:
        print(f"Redacted likely secrets in {len(redacted)} file(s):")
        for f in redacted:
            print(f"  - {f.path} ({f.redactions} match(es))")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

### sidecar/__init__.py

```python
"""Python sidecar for chat-driven CUTMACHINE edits."""
```

### sidecar/binary.py

```python
"""Safe subprocess boundary for the CUTMACHINE executable."""

from __future__ import annotations

import json
import os
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any


class BinaryError(RuntimeError):
    pass


@dataclass(frozen=True)
class ApplyResult:
    ok: bool
    doc_hash: str | None = None
    error: str | None = None
    detail: str | None = None


def _canonical_time(value: dict[str, Any]) -> dict[str, Any]:
    return {"value": value["value"], "rate": value["rate"]}


def _canonical_operation(operation: dict[str, Any]) -> dict[str, Any]:
    """Order the existing operation format as required by its C++ reader."""
    try:
        operation_type = operation["type"]
        if operation_type == "InsertClip":
            return {
                "type": operation_type,
                "track_id": operation["track_id"],
                "source_id": operation["source_id"],
                "source_in": _canonical_time(operation["source_in"]),
                "duration": _canonical_time(operation["duration"]),
                "timeline_in": _canonical_time(operation["timeline_in"]),
                "clip_id": operation["clip_id"],
                "exact_timeline": operation["exact_timeline"],
            }
        if operation_type == "RemoveClip":
            return {
                "type": operation_type,
                "clip_id": operation["clip_id"],
                "exact_timeline": operation["exact_timeline"],
            }
        if operation_type == "TrimClip":
            return {
                "type": operation_type,
                "clip_id": operation["clip_id"],
                "edge": operation["edge"],
                "delta": _canonical_time(operation["delta"]),
                "exact_clip": operation["exact_clip"],
            }
    except KeyError as exc:
        raise BinaryError(f"operation is missing {exc.args[0]!r}") from exc
    raise BinaryError(f"unknown operation type {operation_type!r}")


class CutmachineBinary:
    def __init__(self, executable: str | os.PathLike[str] | None = None) -> None:
        default = Path(__file__).resolve().parents[1] / "build" / "cutmachine"
        self.executable = str(
            executable or os.environ.get("CUTMACHINE_BINARY", default))

    def _run(self, arguments: list[str]) -> tuple[int, dict[str, Any]]:
        try:
            completed = subprocess.run(
                [self.executable, *arguments],
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        except OSError as exc:
            raise BinaryError(f"unable to execute {self.executable}: {exc}") from exc
        try:
            payload = json.loads(completed.stdout)
        except json.JSONDecodeError as exc:
            detail = completed.stderr.strip() or completed.stdout.strip()
            raise BinaryError(
                f"cutmachine returned non-JSON output (status {completed.returncode}): "
                f"{detail}") from exc
        if not isinstance(payload, dict):
            raise BinaryError("cutmachine stdout must be a JSON object")
        if completed.returncode not in (0, 1):
            detail = completed.stderr.strip() or payload
            raise BinaryError(
                f"cutmachine exited with status {completed.returncode}: {detail}")
        return completed.returncode, payload

    def describe(self, document: str | os.PathLike[str]) -> dict[str, Any]:
        status, payload = self._run(["--describe", str(document)])
        if status != 0:
            raise BinaryError(
                f"{payload.get('error', 'UnknownError')}: "
                f"{payload.get('detail', '')}")
        # The CLI now separates the available-media library from the mounted
        # timeline. Editing planners deliberately keep receiving only the
        # timeline because this milestone does not add library edit operations.
        timeline = payload.get("timeline")
        return timeline if isinstance(timeline, dict) else payload

    def apply_operation(
        self, document: str | os.PathLike[str], operation: dict[str, Any]
    ) -> ApplyResult:
        status, payload = self._run([
            "--apply-op", str(document),
            json.dumps(_canonical_operation(operation), ensure_ascii=False,
                       separators=(",", ":")),
        ])
        if status == 0 and payload.get("ok") is True:
            return ApplyResult(ok=True, doc_hash=payload.get("doc_hash"))
        if status == 1 and payload.get("ok") is False:
            return ApplyResult(
                ok=False,
                error=str(payload.get("error", "InvalidOperation")),
                detail=str(payload.get("detail", "")),
            )
        raise BinaryError("cutmachine status and JSON payload disagree")
```

### sidecar/eval-document.json

```json
{
  "version": 1,
  "sources": [
    {"id":"01K40000000000000000000001","path":"interview.mov","rate":{"num":25,"den":1},"duration":{"value":500,"rate":25}},
    {"id":"01K40000000000000000000002","path":"illustrations.mov","rate":{"num":25,"den":1},"duration":{"value":500,"rate":25}}
  ],
  "tracks": [
    {"id":"01K40000000000000000000003","kind":"video","index":0,"clips":[
      {"id":"01K40000000000000000000004","source_id":"01K40000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":50,"rate":25},"timeline_in":{"value":0,"rate":25}},
      {"id":"01K40000000000000000000005","source_id":"01K40000000000000000000001","source_in":{"value":100,"rate":25},"duration":{"value":25,"rate":25},"timeline_in":{"value":75,"rate":25}},
      {"id":"01K40000000000000000000006","source_id":"01K40000000000000000000002","source_in":{"value":0,"rate":25},"duration":{"value":50,"rate":25},"timeline_in":{"value":100,"rate":25}}
    ]},
    {"id":"01K40000000000000000000007","kind":"video","index":1,"clips":[
      {"id":"01K40000000000000000000008","source_id":"01K40000000000000000000002","source_in":{"value":200,"rate":25},"duration":{"value":25,"rate":25},"timeline_in":{"value":25,"rate":25}}
    ]}
  ]
}
```

### sidecar/eval.py

```python
"""Reproducible 15-case prompt/model evaluation harness."""

from __future__ import annotations

import argparse
import io
import json
import os
import shutil
import tempfile
import urllib.error
import urllib.request
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path
from typing import Any

from .binary import BinaryError, CutmachineBinary
from .planner import (
    AnthropicPlanner,
    OllamaPlanner,
    Plan,
    Planner,
    PlannerError,
)
from .schema import PLANNER_RESPONSE_SCHEMA


S1 = "01K40000000000000000000001"
S2 = "01K40000000000000000000002"
T1 = "01K40000000000000000000003"
T2 = "01K40000000000000000000007"
A1 = "01K40000000000000000000004"
A2 = "01K40000000000000000000005"
A3 = "01K40000000000000000000006"
B1 = "01K40000000000000000000008"


def _remove(clip_id: str) -> dict[str, Any]:
    return {"type": "RemoveClip", "clip_id": clip_id, "exact_timeline": []}


def _trim(clip_id: str, edge: str, frames: int) -> dict[str, Any]:
    return {
        "type": "TrimClip", "clip_id": clip_id, "edge": edge,
        "delta": {"value": frames, "rate": 25}, "exact_clip": None,
    }


def _insert(track_id: str, source_id: str, source_in: int,
            duration: int, timeline_in: int) -> dict[str, Any]:
    return {
        "type": "InsertClip", "track_id": track_id, "source_id": source_id,
        "source_in": {"value": source_in, "rate": 25},
        "duration": {"value": duration, "rate": 25},
        "timeline_in": {"value": timeline_in, "rate": 25},
        "clip_id": "", "exact_timeline": [],
    }


@dataclass(frozen=True)
class EvalCase:
    instruction: str
    expected: dict[str, Any]


CASES: tuple[EvalCase, ...] = (
    EvalCase("Supprime le clip A1.", _remove(A1)),
    EvalCase("Enlève le deuxième clip de la piste vidéo 1.", _remove(A2)),
    EvalCase("Supprime A3, le plan issu d'illustrations.mov.", _remove(A3)),
    EvalCase("Retire l'unique clip de la deuxième piste.", _remove(B1)),
    EvalCase("Raccourcis la fin de A1 de 10 images.", _trim(A1, "Tail", -10)),
    EvalCase("Retire 5 images à la fin de A2.", _trim(A2, "Tail", -5)),
    EvalCase("Coupe les 10 premières images de A3.", _trim(A3, "Head", 10)),
    EvalCase("Récupère 5 images avant le début actuel de A2.", _trim(A2, "Head", -5)),
    EvalCase("Prolonge la fin de A3 de 10 images.", _trim(A3, "Tail", 10)),
    EvalCase("Enlève 5 images au début de B1.", _trim(B1, "Head", 5)),
    EvalCase(
        "Sur la piste vidéo 1, insère à l'image 50 dix images de "
        "interview.mov à partir de sa source 200.",
        _insert(T1, S1, 200, 10, 50),
    ),
    EvalCase(
        "Au début de la piste vidéo 2, insère les 15 premières images de "
        "illustrations.mov.",
        _insert(T2, S2, 0, 15, 0),
    ),
    EvalCase(
        "Ajoute à la fin de la piste vidéo 1 vingt images de interview.mov "
        "depuis l'image source 300.",
        _insert(T1, S1, 300, 20, 150),
    ),
    EvalCase(
        "Dans le trou après A1, à l'image 50, place 10 images de "
        "illustrations.mov depuis l'image source 50.",
        _insert(T1, S2, 50, 10, 50),
    ),
    EvalCase(
        "Insère au tout début de la piste vidéo 2 cinq images de "
        "interview.mov à partir de l'image source 400.",
        _insert(T2, S1, 400, 5, 0),
    ),
)

# Kept outside CASES so the fixed historical corpus remains exactly 15 cases.
REFERENCE_CASE = EvalCase(
    "Raccourcis le premier plan de 2 secondes.", _trim(A1, "Tail", -50))


class _ReplayResponse:
    def __init__(self, body: bytes) -> None:
        self._body = body

    def __enter__(self) -> "_ReplayResponse":
        return self

    def __exit__(self, *args: Any) -> None:
        return None

    def read(self) -> bytes:
        return self._body


class TraceOpener:
    """Capture the actual HTTP request and response without changing parsing."""

    def __init__(self) -> None:
        self.request_url: str | None = None
        self.request_payload: dict[str, Any] | None = None
        self.response_text: str | None = None

    def reset(self) -> None:
        self.request_url = None
        self.request_payload = None
        self.response_text = None

    def __call__(self, request: Any, timeout: float) -> _ReplayResponse:
        self.request_url = request.full_url
        try:
            self.request_payload = json.loads(request.data)
        except (TypeError, json.JSONDecodeError):
            self.request_payload = None
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                body = response.read()
        except urllib.error.HTTPError as exc:
            body = exc.read()
            self.response_text = body.decode("utf-8", errors="replace")
            raise urllib.error.HTTPError(
                exc.url, exc.code, exc.msg, exc.headers, io.BytesIO(body)) from exc
        self.response_text = body.decode("utf-8", errors="replace")
        return _ReplayResponse(body)


def _raw_model_output(backend: str, response_text: str | None) -> Any:
    if response_text is None:
        return None
    try:
        response = json.loads(response_text)
    except json.JSONDecodeError:
        return response_text
    if backend == "ollama":
        try:
            return response["message"]["content"]
        except (KeyError, TypeError):
            return response
    blocks = response.get("content", []) if isinstance(response, dict) else []
    for block in blocks if isinstance(blocks, list) else []:
        if (isinstance(block, dict) and block.get("type") == "tool_use" and
                block.get("name") == AnthropicPlanner.TOOL_NAME):
            return block.get("input")
    return response


def _raw_operation(raw_output: Any) -> tuple[dict[str, Any] | None, bool]:
    value = raw_output
    if isinstance(value, str):
        try:
            value = json.loads(value)
        except json.JSONDecodeError:
            return None, False
    if not isinstance(value, dict):
        return None, False
    operation = value.get("operation")
    return (operation if isinstance(operation, dict) else None,
            operation is None and value.get("refusal") is not None)


def _classify_failure(
    raw_output: Any, actual: Any, expected: dict[str, Any], error: str | None,
    timeline: dict[str, Any],
) -> tuple[str, str, list[str]]:
    raw_operation, raw_refusal = _raw_operation(raw_output)
    if raw_refusal or (isinstance(actual, dict) and "refusal" in actual):
        return "D", "refus injustifié", []
    operation = (
        actual if isinstance(actual, dict) and "type" in actual
        else raw_operation
    )
    if operation is None:
        detail = error or "sortie structurée inexploitable"
        return "D", detail, []
    if operation.get("type") != expected.get("type"):
        return "A", (
            f"type {operation.get('type')!r} au lieu de {expected.get('type')!r}"
        ), []

    secondary: list[str] = []
    operation_type = expected["type"]
    id_fields = {
        "RemoveClip": ("clip_id",),
        "TrimClip": ("clip_id",),
        "InsertClip": ("track_id", "source_id"),
    }[operation_type]
    bad_ids = [field for field in id_fields
               if operation.get(field) != expected.get(field)]
    temporal_fields = {
        "RemoveClip": (),
        "TrimClip": ("delta",),
        "InsertClip": ("source_in", "duration", "timeline_in"),
    }[operation_type]
    bad_times = [field for field in temporal_fields
                 if not _time_equal(operation.get(field), expected.get(field))]
    if bad_ids:
        if bad_times:
            secondary.append("C: valeur temporelle également incorrecte (" +
                             ", ".join(bad_times) + ")")
        known_ids = {
            item.get("id")
            for collection in (timeline.get("sources", []),
                               timeline.get("tracks", []))
            for item in collection if isinstance(item, dict)
        }
        known_ids.update(
            item.get("id")
            for track in timeline.get("tracks", []) if isinstance(track, dict)
            for item in track.get("items", []) if isinstance(item, dict)
        )
        absent = [field for field in bad_ids
                  if operation.get(field) not in known_ids]
        if absent:
            return "B", "ULID absent de la vue (" + ", ".join(absent) + ")", secondary
        return "E", "mauvais objet sélectionné (" + ", ".join(bad_ids) + ")", secondary
    if operation_type == "TrimClip" and operation.get("edge") != expected.get("edge"):
        if bad_times:
            secondary.append("C: valeur temporelle également incorrecte (delta)")
        return "E", "bord de trim incorrect", secondary
    if bad_times:
        return "C", "valeur temporelle incorrecte (" + ", ".join(bad_times) + ")", []
    if error:
        if "ULID absent" in error:
            return "B", error, []
        return "D", error, []
    return "E", "divergence sémantique non couverte", []


def _breakpoint(error: str | None, passed: bool, actual: Any) -> str:
    if error:
        if error.startswith("HTTP ") or error.startswith("unable to reach"):
            return "transport_http"
        if "malformed JSON" in error or "returned no valid structured" in error:
            return "decodage_reponse_backend"
        if "ULID absent from the view" in error:
            return "validation_locale_ulid_avant_binaire"
        return "validation_locale_schema_avant_binaire"
    if isinstance(actual, dict) and "refusal" in actual:
        return "refus_modele_accepte_localement"
    return "termine" if passed else "comparaison_semantique_attendu_obtenu"


def _time_equal(left: Any, right: Any) -> bool:
    try:
        return Fraction(left["value"], left["rate"]) == Fraction(
            right["value"], right["rate"])
    except (KeyError, TypeError, ValueError, ZeroDivisionError):
        return False


def operations_equal(actual: dict[str, Any], expected: dict[str, Any]) -> bool:
    """Compare edit semantics while tolerating equivalent rational timebases."""
    if actual.get("type") != expected.get("type"):
        return False
    operation_type = expected["type"]
    if operation_type == "RemoveClip":
        return actual.get("clip_id") == expected["clip_id"]
    if operation_type == "TrimClip":
        return (
            actual.get("clip_id") == expected["clip_id"]
            and actual.get("edge") == expected["edge"]
            and _time_equal(actual.get("delta"), expected["delta"])
        )
    if operation_type == "InsertClip":
        return (
            actual.get("track_id") == expected["track_id"]
            and actual.get("source_id") == expected["source_id"]
            and _time_equal(actual.get("source_in"), expected["source_in"])
            and _time_equal(actual.get("duration"), expected["duration"])
            and _time_equal(actual.get("timeline_in"), expected["timeline_in"])
        )
    return False


def evaluate(
    planner: Planner,
    timeline: dict[str, Any],
    cases: tuple[EvalCase, ...],
    run: int,
    opener: TraceOpener,
    records: list[dict[str, Any]],
    trace_path: Path,
    trace_document: dict[str, Any],
) -> tuple[int, int]:
    successes = 0
    print(f"\nBackend: {planner.backend_name}, run {run}")
    for index, case in enumerate(cases, start=1):
        opener.reset()
        error: str | None = None
        try:
            plan: Plan = planner.plan(timeline, case.instruction)
            passed = plan.operation is not None and operations_equal(
                plan.operation, case.expected)
            actual: Any = plan.operation if plan.operation is not None else {
                "refusal": plan.refusal}
        except PlannerError as exc:
            passed = False
            error = str(exc)
            actual = {"planner_error": str(exc)}
        raw_output = _raw_model_output(
            planner.backend_name, opener.response_text)
        family: str | None = None
        classification: str | None = None
        secondary: list[str] = []
        if not passed:
            family, classification, secondary = _classify_failure(
                raw_output, actual, case.expected, error, timeline)
        request = opener.request_payload or {}
        records.append({
            "backend": planner.backend_name,
            "run": run,
            "case": index if index <= len(CASES) else "reference",
            "instruction": case.instruction,
            "expected_operation": case.expected,
            "raw_model_output": raw_output,
            "final_operation_or_refusal": actual,
            "passed": passed,
            "breakpoint": _breakpoint(error, passed, actual),
            "error": error,
            "failure_family": family,
            "failure_detail": classification,
            "secondary_causes": secondary,
            "http_evidence": {
                "url": opener.request_url,
                "format_present": "format" in request,
                "format": request.get("format"),
                "input_schema": (
                    request.get("tools", [{}])[0].get("input_schema")
                    if isinstance(request.get("tools"), list) and
                    request.get("tools") else None
                ),
            },
            "raw_http_response": opener.response_text,
        })
        trace_path.write_text(
            json.dumps(trace_document, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        successes += int(passed)
        print(f"{index:02d} {'PASS' if passed else 'FAIL'} — {case.instruction}")
        if not passed:
            print("   attendu:", json.dumps(case.expected, ensure_ascii=False))
            print("   obtenu  :", json.dumps(actual, ensure_ascii=False))
    rate = 100.0 * successes / len(cases)
    print(f"Résultat {planner.backend_name} run {run}: "
          f"{successes}/{len(cases)} ({rate:.1f} %)")
    return successes, len(cases)


def _direct_apply_control(
    binary: CutmachineBinary, document: Path
) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []
    with tempfile.TemporaryDirectory() as directory:
        for index, case in enumerate(CASES, start=1):
            copy = Path(directory) / f"case-{index}.json"
            shutil.copyfile(document, copy)
            result = binary.apply_operation(copy, case.expected)
            results.append({
                "case": index,
                "accepted": result.ok,
                "error": result.error,
                "detail": result.detail,
            })
    return results


def _request_controls(records: list[dict[str, Any]]) -> dict[str, Any]:
    ollama = next((record for record in records
                   if record["backend"] == "ollama"), None)
    anthropic = next((record for record in records
                      if record["backend"] == "anthropic"), None)
    ollama_format = ollama["http_evidence"]["format"] if ollama else None
    anthropic_schema = (
        anthropic["http_evidence"]["input_schema"] if anthropic else None)
    return {
        "ollama_format_present_in_emitted_http_request": bool(
            ollama and ollama["http_evidence"]["format_present"]),
        "ollama_format_equals_shared_schema": ollama_format == PLANNER_RESPONSE_SCHEMA,
        "anthropic_input_schema_equals_shared_schema": (
            anthropic_schema == PLANNER_RESPONSE_SCHEMA),
        "emitted_backend_schemas_strictly_identical": (
            ollama_format is not None and ollama_format == anthropic_schema),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Évalue les planners CUTMACHINE")
    parser.add_argument(
        "--backend", choices=("ollama", "anthropic", "all"),
        default=os.environ.get("CUTMACHINE_BACKEND", "ollama").lower())
    parser.add_argument("--model", help="remplace CUTMACHINE_MODEL")
    parser.add_argument(
        "--document", type=Path,
        default=Path(__file__).with_name("eval-document.json"))
    parser.add_argument("--binary", help="chemin du binaire cutmachine")
    parser.add_argument("--ollama-runs", type=int, default=1)
    parser.add_argument(
        "--trace", type=Path,
        default=Path(__file__).resolve().parents[1] / "eval-trace.json")
    args = parser.parse_args()
    trace: dict[str, Any] = {
        "document": str(args.document.resolve()),
        "cases": len(CASES),
        "reference_case_added_separately": True,
        "records": [],
        "controls": {},
    }
    try:
        binary = CutmachineBinary(args.binary)
        timeline = binary.describe(args.document)
        compact_view = json.dumps(
            timeline, ensure_ascii=False, separators=(",", ":"))
        trace["controls"]["describe_view"] = {
            "utf8_bytes": len(compact_view.encode("utf-8")),
            "characters": len(compact_view),
        }
        trace["controls"]["direct_apply_expected_operations"] = (
            _direct_apply_control(binary, args.document))
        cases = CASES + (REFERENCE_CASE,)
        totals: list[tuple[int, int]] = []
        if args.backend in {"ollama", "all"}:
            for run in range(1, args.ollama_runs + 1):
                opener = TraceOpener()
                totals.append(evaluate(
                    OllamaPlanner(model=args.model, opener=opener), timeline,
                    cases, run, opener, trace["records"], args.trace, trace))
        if args.backend in {"anthropic", "all"}:
            opener = TraceOpener()
            totals.append(evaluate(
                AnthropicPlanner(model=args.model, opener=opener), timeline,
                cases, 1, opener, trace["records"], args.trace, trace))
        trace["controls"].update(_request_controls(trace["records"]))
        trace["controls"]["ulid_validation_order"] = (
            "planner local validation before any binary call; eval model path "
            "does not invoke --apply-op")
        args.trace.write_text(
            json.dumps(trace, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
    except (BinaryError, PlannerError) as exc:
        print(f"Erreur d'évaluation : {exc}")
        return 1
    return 0 if all(success == total for success, total in totals) else 1


if __name__ == "__main__":
    raise SystemExit(main())
```

### sidecar/planner.py

```python
"""Backend-independent planning interface and HTTP implementations."""

from __future__ import annotations

import json
import os
import re
import urllib.error
import urllib.request
from abc import ABC, abstractmethod
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

from .schema import PLANNER_RESPONSE_SCHEMA


SYSTEM_PROMPT = """Tu pilotes CUTMACHINE, un éditeur vidéo déterministe.
Tu dois soumettre exactement une réponse structurée : une seule opération, ou un
refus motivé si la demande est impossible avec InsertClip, RemoveClip et TrimClip.
La réponse contient toujours les champs operation et refusal : celui qui n'est
pas utilisé vaut null.
Le schéma commun aux deux backends exige dans operation l'union des champs des
trois variantes. Choisis d'abord type et renseigne correctement ses champs ; les
champs sans rapport avec ce type sont des placeholders et seront ignorés.

Règles impératives :
- Les aliases (A1, A2...) servent uniquement à comprendre la demande.
- Dans l'opération, adresse toujours clips, pistes et sources par leur ULID complet.
- N'invente jamais un ULID absent de la vue de timeline.
- Produis une seule opération par tour, jamais une suite d'opérations.
- N'approxime pas une demande qui exige une autre capacité : refuse-la.
- Les pistes sont affichées par index à partir de zéro : « piste 1 » désigne
  index 0 et les aliases A*, « piste 2 » désigne index 1 et les aliases B*.
- Les secondes de la vue sont uniquement indicatives et ne servent jamais au
  calcul. Un temps {value, rate} vaut exactement value/rate seconde. Pour une
  cadence N/D, une frame vaut D ticks au rate N (exemple 30000/1001 : une frame
  est {value: 1001, rate: 30000}). À 25/1, 15 images s'écrivent donc
  {value: 15, rate: 25}, sans multiplication supplémentaire.
- Pour TrimClip, exprime l'intention sans signe interne : trim_action vaut
  Shorten pour raccourcir/enlever et Extend pour prolonger/récupérer ;
  trim_amount est une quantité strictement positive avec l'unité Frames ou
  Seconds. CUTMACHINE calculera le signe, le timebase et le delta canonique.
- Pour InsertClip, laisse clip_id vide et exact_timeline vide.
- Pour RemoveClip, laisse exact_timeline vide.
- Pour TrimClip, laisse exact_clip à null.
"""


def _load_project_env() -> None:
    """Load the repository .env without replacing exported environment values."""
    path = Path(__file__).resolve().parents[1] / ".env"
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except FileNotFoundError:
        return
    for raw_line in lines:
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        if line.startswith("export "):
            line = line[7:].lstrip()
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
            value = value[1:-1]
        if key:
            os.environ.setdefault(key, value)


class PlannerError(RuntimeError):
    """A backend, transport, or structured-response failure."""


@dataclass(frozen=True)
class Plan:
    operation: dict[str, Any] | None = None
    refusal: str | None = None

    def __post_init__(self) -> None:
        if (self.operation is None) == (self.refusal is None):
            raise ValueError("a plan must contain exactly one operation or refusal")


class Planner(ABC):
    @property
    @abstractmethod
    def backend_name(self) -> str:
        """Stable name used by the REPL and evaluation report."""

    @abstractmethod
    def plan(
        self,
        timeline: dict[str, Any],
        instruction: str,
        previous_error: dict[str, Any] | None = None,
    ) -> Plan:
        """Return one operation or a motivated refusal."""


def _prompt(
    timeline: dict[str, Any],
    instruction: str,
    previous_error: dict[str, Any] | None,
) -> str:
    resolved = _resolve_entities(timeline, instruction)
    parts = [
        "Vue de timeline JSON :",
        json.dumps(timeline, ensure_ascii=False, separators=(",", ":")),
        "Références déterministes résolues par CUTMACHINE :",
        json.dumps(resolved, ensure_ascii=False, separators=(",", ":")),
        "Instruction utilisateur :",
        instruction,
    ]
    if previous_error is not None:
        parts.extend(
            [
                "La première opération a été refusée par CUTMACHINE. Corrige-la "
                "une seule fois en tenant compte de cette erreur nommée :",
                json.dumps(previous_error, ensure_ascii=False, separators=(",", ":")),
            ]
        )
    return "\n".join(parts)


def _post_json(
    url: str,
    payload: dict[str, Any],
    headers: dict[str, str],
    timeout: float,
    opener: Callable[..., Any],
) -> dict[str, Any]:
    request = urllib.request.Request(
        url,
        data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
        headers={"Content-Type": "application/json", **headers},
        method="POST",
    )
    try:
        with opener(request, timeout=timeout) as response:
            body = response.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise PlannerError(f"HTTP {exc.code} from {url}: {detail}") from exc
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        raise PlannerError(f"unable to reach {url}: {exc}") from exc
    try:
        parsed = json.loads(body)
    except json.JSONDecodeError as exc:
        raise PlannerError(f"backend returned malformed JSON: {exc}") from exc
    if not isinstance(parsed, dict):
        raise PlannerError("backend response must be a JSON object")
    return parsed


def _parse_plan(
    value: Any, timeline: dict[str, Any], instruction: str
) -> Plan:
    if not isinstance(value, dict):
        raise PlannerError("structured planner response must be an object")
    # Some older Ollama grammar implementations omit a required nullable field.
    # Accept that equivalent representation after still passing the full schema
    # in `format`; local validation remains authoritative.
    if set(value) == {"operation"}:
        value = {"operation": value["operation"], "refusal": None}
    elif set(value) == {"refusal"}:
        value = {"operation": None, "refusal": value["refusal"]}
    elif set(value) != {"operation", "refusal"}:
        raise PlannerError("response must contain operation and refusal")
    if value["operation"] is None:
        refusal = value["refusal"]
        if not isinstance(refusal, dict) or set(refusal) != {"reason"}:
            raise PlannerError("invalid structured refusal")
        reason = refusal["reason"]
        if not isinstance(reason, str) or not reason.strip():
            raise PlannerError("refusal reason must be non-empty")
        return Plan(refusal=reason.strip())
    if not isinstance(value["operation"], dict):
        raise PlannerError("operation must be an object or null")
    operation = value["operation"]
    return Plan(operation=_normalize_operation(operation, timeline, instruction))


def _validate_time(value: Any, field: str, *, positive: bool = False,
                   nonnegative: bool = False) -> None:
    if not isinstance(value, dict) or set(value) != {"value", "rate"}:
        raise PlannerError(f"{field} must contain integer value and rate")
    if type(value["value"]) is not int or type(value["rate"]) is not int:
        raise PlannerError(f"{field} value and rate must be integers")
    if value["rate"] <= 0:
        raise PlannerError(f"{field}.rate must be positive")
    if positive and value["value"] <= 0:
        raise PlannerError(f"{field}.value must be positive")
    if nonnegative and value["value"] < 0:
        raise PlannerError(f"{field}.value must be non-negative")


def _known_ids(timeline: dict[str, Any]) -> tuple[set[str], set[str], set[str]]:
    source_ids = {
        source["id"] for source in timeline.get("sources", [])
        if isinstance(source, dict) and isinstance(source.get("id"), str)
    }
    track_ids: set[str] = set()
    clip_ids: set[str] = set()
    for track in timeline.get("tracks", []):
        if not isinstance(track, dict):
            continue
        if isinstance(track.get("id"), str):
            track_ids.add(track["id"])
        for item in track.get("items", []):
            if (isinstance(item, dict) and item.get("type") == "clip" and
                    isinstance(item.get("id"), str)):
                clip_ids.add(item["id"])
    return source_ids, track_ids, clip_ids


_ORDINALS = {
    "premier": 1, "première": 1,
    "deuxième": 2, "second": 2, "seconde": 2,
    "troisième": 3, "quatrième": 4, "cinquième": 5,
    "sixième": 6, "septième": 7, "huitième": 8,
    "neuvième": 9, "dixième": 10,
}


def _track_ordinal(text: str) -> int | None:
    numeric = re.search(r"\bpiste(?:\s+vid[eé]o)?\s+(\d+)\b", text)
    if numeric:
        return int(numeric.group(1))
    words = "|".join(_ORDINALS)
    named = re.search(
        rf"\b({words})\s+piste(?:\s+vid[eé]o)?\b", text)
    return _ORDINALS[named.group(1)] if named else None


def _clip_ordinal(text: str) -> int | None:
    words = "|".join(_ORDINALS)
    named = re.search(rf"\b({words})\s+(?:clip|plan)\b", text)
    if named:
        return _ORDINALS[named.group(1)]
    numeric = re.search(r"\b(?:clip|plan)\s+(\d+)\b", text)
    return int(numeric.group(1)) if numeric else None


def _resolve_entities(
    timeline: dict[str, Any], instruction: str
) -> dict[str, str]:
    text = instruction.casefold()
    tracks = [
        track for track in timeline.get("tracks", [])
        if isinstance(track, dict) and isinstance(track.get("id"), str)
    ]
    if re.search(r"\bpiste\s+vid[eé]o\b", text):
        tracks = [track for track in tracks if track.get("kind") == "video"]
    tracks.sort(key=lambda track: track.get("index", 0))
    resolution: dict[str, str] = {}

    track_number = _track_ordinal(text)
    selected_track = None
    if track_number is not None and 1 <= track_number <= len(tracks):
        selected_track = tracks[track_number - 1]
        resolution["track_id"] = selected_track["id"]

    clip_alias = _explicit_clip_alias(timeline, instruction)
    if clip_alias:
        resolution["clip_id"] = clip_alias
    else:
        ordinal = _clip_ordinal(text)
        candidates = selected_track.get("items", []) if selected_track else [
            item for track in tracks for item in track.get("items", [])
        ]
        clips = [
            item for item in candidates
            if isinstance(item, dict) and item.get("type") == "clip" and
            isinstance(item.get("id"), str)
        ]
        if ordinal is not None and 1 <= ordinal <= len(clips):
            resolution["clip_id"] = clips[ordinal - 1]["id"]
        elif "unique clip" in text and len(clips) == 1:
            resolution["clip_id"] = clips[0]["id"]

    matching_sources = [
        source["id"] for source in timeline.get("sources", [])
        if isinstance(source, dict) and isinstance(source.get("id"), str) and
        isinstance(source.get("file"), str) and
        source["file"].casefold() in text
    ]
    if len(matching_sources) == 1:
        resolution["source_id"] = matching_sources[0]
    return resolution


def _explicit_trim_intent(instruction: str) -> dict[str, Any] | None:
    text = instruction.casefold()
    amount = re.search(
        r"\b(\d+)\s+(?:premi[eè]res?\s+)?"
        r"(images?|frames?|secondes?)\b", text)
    if amount is None:
        return None
    if not re.search(
        r"\b(raccourc\w*|retir\w*|enl[eè]v\w*|coup\w*|"
        r"r[eé]cup[eè]r\w*|prolong\w*)\b", text
    ):
        return None
    extend = bool(re.search(r"\b(r[eé]cup[eè]r\w*|prolong\w*)\b", text))
    head = bool(re.search(
        r"\b(d[eé]but|premi[eè]res?|avant le d[eé]but)\b", text))
    return {
        "edge": "Head" if head else "Tail",
        "action": "Extend" if extend else "Shorten",
        "value": int(amount.group(1)),
        "unit": "Seconds" if amount.group(2).startswith("seconde") else "Frames",
    }


def _clip_rate(
    timeline: dict[str, Any], clip_id: str
) -> tuple[int, int] | None:
    source_id = None
    for track in timeline.get("tracks", []):
        for item in track.get("items", []) if isinstance(track, dict) else []:
            if isinstance(item, dict) and item.get("id") == clip_id:
                source_id = item.get("source_id")
                break
    for source in timeline.get("sources", []):
        if not isinstance(source, dict) or source.get("id") != source_id:
            continue
        match = re.fullmatch(r"(\d+)/(\d+)", str(source.get("frame_rate", "")))
        if match and int(match.group(1)) > 0 and int(match.group(2)) > 0:
            return int(match.group(1)), int(match.group(2))
    return None


def _timeline_rate(timeline: dict[str, Any]) -> tuple[int, int] | None:
    for source in timeline.get("sources", []):
        if not isinstance(source, dict):
            continue
        match = re.fullmatch(r"(\d+)/(\d+)", str(source.get("frame_rate", "")))
        if match and int(match.group(1)) > 0 and int(match.group(2)) > 0:
            return int(match.group(1)), int(match.group(2))
    return None


def _explicit_timeline_position(
    timeline: dict[str, Any], instruction: str,
    track: dict[str, Any] | None = None,
) -> dict[str, int] | None:
    text = instruction.casefold()
    frame = re.search(r"\b[àa] l['’]image\s+(\d+)\b", text)
    at_start = bool(re.search(
        r"\bau (?:tout )?d[eé]but (?:de )?(?:la )?piste\b", text))
    at_end = bool(re.search(r"\b[àa] la fin de la piste\b", text))
    if frame is None and not at_start and not at_end:
        return None
    rate = _timeline_rate(timeline)
    if rate is None:
        raise PlannerError("unable to determine the timeline presentation rate")
    numerator, denominator = rate
    frames = int(frame.group(1)) if frame else 0
    if at_end:
        if track is None:
            return None
        frames = max((
            int(item.get("timeline_in", {}).get("frames", 0)) +
            int(item.get("duration", {}).get("frames", 0))
            for item in track.get("items", [])
            if isinstance(item, dict) and item.get("type") == "clip"
        ), default=0)
    return {"value": frames * denominator, "rate": numerator}


def _explicit_clip_alias(
    timeline: dict[str, Any], instruction: str
) -> str | None:
    aliases = {
        item.get("alias", "").casefold(): item.get("id")
        for track in timeline.get("tracks", []) if isinstance(track, dict)
        for item in track.get("items", []) if isinstance(item, dict)
        if item.get("type") == "clip"
    }
    for token in re.findall(r"\b[a-z]+\d+\b", instruction.casefold()):
        if token in aliases and isinstance(aliases[token], str):
            return aliases[token]
    return None


def _trim_delta(
    amount: dict[str, Any], edge: str, action: str,
    timeline: dict[str, Any], clip_id: str,
) -> dict[str, int]:
    if (not isinstance(amount, dict) or
            set(amount) != {"value", "unit"} or
            type(amount["value"]) is not int or amount["value"] <= 0 or
            amount["unit"] not in {"Frames", "Seconds"}):
        raise PlannerError(
            "trim_amount must be a positive Frames or Seconds quantity")
    rate = _clip_rate(timeline, clip_id)
    if rate is None:
        raise PlannerError("unable to determine the trimmed clip media rate")
    numerator, denominator = rate
    ticks = amount["value"] * (
        denominator if amount["unit"] == "Frames" else numerator)
    shorten = action == "Shorten"
    positive = shorten if edge == "Head" else not shorten
    return {"value": ticks if positive else -ticks, "rate": numerator}


def _normalize_operation(
    operation: dict[str, Any], timeline: dict[str, Any], instruction: str = ""
) -> dict[str, Any]:
    source_ids, track_ids, clip_ids = _known_ids(timeline)
    resolved = _resolve_entities(timeline, instruction)
    operation_type = operation.get("type")
    explicit_trim = _explicit_trim_intent(instruction)
    if explicit_trim is not None:
        operation_type = "TrimClip"
    if operation_type == "InsertClip":
        required = {"track_id", "source_id", "source_in", "duration",
                    "timeline_in"}
        if not required.issubset(operation):
            raise PlannerError("InsertClip is missing required fields")
        track_id = resolved.get("track_id", operation["track_id"])
        source_id = resolved.get("source_id", operation["source_id"])
        if track_id not in track_ids:
            raise PlannerError("InsertClip references a track ULID absent from the view")
        if source_id not in source_ids:
            raise PlannerError("InsertClip references a source ULID absent from the view")
        _validate_time(operation["source_in"], "source_in", nonnegative=True)
        _validate_time(operation["duration"], "duration", positive=True)
        selected_track = next(
            (track for track in timeline.get("tracks", [])
             if isinstance(track, dict) and track.get("id") == track_id), None)
        timeline_in = (
            _explicit_timeline_position(timeline, instruction, selected_track)
            or operation["timeline_in"]
        )
        _validate_time(timeline_in, "timeline_in", nonnegative=True)
        return {
            "type": "InsertClip",
            "track_id": track_id,
            "source_id": source_id,
            "source_in": operation["source_in"],
            "duration": operation["duration"],
            "timeline_in": timeline_in,
            "clip_id": "",
            "exact_timeline": [],
        }
    elif operation_type == "RemoveClip":
        if "clip_id" not in operation:
            raise PlannerError("RemoveClip is missing clip_id")
        clip_id = resolved.get("clip_id", operation["clip_id"])
        if clip_id not in clip_ids:
            raise PlannerError("RemoveClip references a clip ULID absent from the view")
        return {
            "type": "RemoveClip",
            "clip_id": clip_id,
            "exact_timeline": [],
        }
    elif operation_type == "TrimClip":
        required = {"clip_id", "edge", "trim_action", "trim_amount"}
        if not required.issubset(operation):
            raise PlannerError("TrimClip is missing required fields")
        clip_id = resolved.get("clip_id", operation["clip_id"])
        if clip_id not in clip_ids:
            raise PlannerError("TrimClip references a clip ULID absent from the view")
        edge = explicit_trim["edge"] if explicit_trim else operation["edge"]
        action = (
            explicit_trim["action"] if explicit_trim
            else operation["trim_action"]
        )
        amount = (
            {"value": explicit_trim["value"], "unit": explicit_trim["unit"]}
            if explicit_trim else operation["trim_amount"]
        )
        if edge not in {"Head", "Tail"}:
            raise PlannerError("TrimClip edge must be Head or Tail")
        if action not in {"Shorten", "Extend"}:
            raise PlannerError("TrimClip trim_action must be Shorten or Extend")
        return {
            "type": "TrimClip",
            "clip_id": clip_id,
            "edge": edge,
            "delta": _trim_delta(
                amount, edge, action, timeline, clip_id),
            "exact_clip": None,
        }
    else:
        raise PlannerError("unknown CUTMACHINE operation type")


class OllamaPlanner(Planner):
    def __init__(
        self,
        model: str | None = None,
        base_url: str | None = None,
        timeout: float = 120.0,
        opener: Callable[..., Any] = urllib.request.urlopen,
    ) -> None:
        _load_project_env()
        self.model = (model or os.environ.get("CUTMACHINE_OLLAMA_MODEL") or
                      os.environ.get("CUTMACHINE_MODEL", "qwen3:8b"))
        self.base_url = (base_url or os.environ.get(
            "CUTMACHINE_OLLAMA_URL", "http://localhost:11434")).rstrip("/")
        self.timeout = timeout
        self._opener = opener

    @property
    def backend_name(self) -> str:
        return "ollama"

    def plan(self, timeline: dict[str, Any], instruction: str,
             previous_error: dict[str, Any] | None = None) -> Plan:
        payload = {
            "model": self.model,
            "stream": False,
            "format": PLANNER_RESPONSE_SCHEMA,
            "options": {"temperature": 0},
            "messages": [
                {"role": "system", "content": SYSTEM_PROMPT},
                {"role": "user", "content": _prompt(
                    timeline, instruction, previous_error)},
            ],
        }
        response = _post_json(f"{self.base_url}/api/chat", payload, {},
                              self.timeout, self._opener)
        try:
            content = response["message"]["content"]
            value = json.loads(content)
        except (KeyError, TypeError, json.JSONDecodeError) as exc:
            raise PlannerError("Ollama returned no valid structured content") from exc
        return _parse_plan(value, timeline, instruction)


class AnthropicPlanner(Planner):
    TOOL_NAME = "submit_cutmachine_plan"

    def __init__(
        self,
        model: str | None = None,
        api_key: str | None = None,
        base_url: str | None = None,
        timeout: float = 120.0,
        opener: Callable[..., Any] = urllib.request.urlopen,
    ) -> None:
        _load_project_env()
        self.model = (model or os.environ.get("CUTMACHINE_ANTHROPIC_MODEL") or
                      os.environ.get("CUTMACHINE_MODEL", "claude-sonnet-4-5"))
        self.api_key = api_key or os.environ.get("ANTHROPIC_API_KEY", "")
        if not self.api_key:
            raise PlannerError("ANTHROPIC_API_KEY is required")
        self.base_url = (base_url or os.environ.get(
            "CUTMACHINE_ANTHROPIC_URL", "https://api.anthropic.com")).rstrip("/")
        self.timeout = timeout
        self._opener = opener

    @property
    def backend_name(self) -> str:
        return "anthropic"

    def plan(self, timeline: dict[str, Any], instruction: str,
             previous_error: dict[str, Any] | None = None) -> Plan:
        payload = {
            "model": self.model,
            "max_tokens": 1024,
            "system": SYSTEM_PROMPT,
            "messages": [{"role": "user", "content": _prompt(
                timeline, instruction, previous_error)}],
            "tools": [{
                "name": self.TOOL_NAME,
                "description": (
                    "Soumet exactement une opération CUTMACHINE exécutable ou "
                    "un refus motivé lorsque la demande est impossible."),
                "input_schema": PLANNER_RESPONSE_SCHEMA,
                "strict": True,
            }],
            "tool_choice": {"type": "tool", "name": self.TOOL_NAME},
        }
        response = _post_json(
            f"{self.base_url}/v1/messages", payload,
            {"x-api-key": self.api_key, "anthropic-version": "2023-06-01"},
            self.timeout, self._opener,
        )
        blocks = response.get("content", [])
        for block in blocks if isinstance(blocks, list) else []:
            if (isinstance(block, dict) and block.get("type") == "tool_use" and
                    block.get("name") == self.TOOL_NAME):
                return _parse_plan(block.get("input"), timeline, instruction)
        raise PlannerError("Anthropic returned no submit_cutmachine_plan tool use")


def planner_from_environment() -> Planner:
    backend = os.environ.get("CUTMACHINE_BACKEND", "ollama").strip().lower()
    if backend == "ollama":
        return OllamaPlanner()
    if backend == "anthropic":
        return AnthropicPlanner()
    raise PlannerError(
        "CUTMACHINE_BACKEND must be 'ollama' or 'anthropic', got " + repr(backend))
```

### sidecar/repl.py

```python
"""Interactive, stateless edit loop."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Callable

from .binary import BinaryError, CutmachineBinary
from .planner import Plan, Planner, PlannerError, planner_from_environment


def _confirm(input_fn: Callable[[str], str]) -> bool:
    return input_fn("Appliquer cette opération ? (o/n) ").strip().lower() in {
        "o", "oui", "y", "yes",
    }


def run_turn(
    planner: Planner,
    binary: CutmachineBinary,
    document: Path,
    instruction: str,
    input_fn: Callable[[str], str] = input,
    print_fn: Callable[[str], None] = print,
) -> None:
    timeline = binary.describe(document)
    previous_error = None
    for attempt in range(2):
        plan: Plan = planner.plan(timeline, instruction, previous_error)
        if plan.refusal is not None:
            print_fn(f"Refus du modèle : {plan.refusal}")
            return
        assert plan.operation is not None
        print_fn("Opération proposée :")
        print_fn(json.dumps(plan.operation, ensure_ascii=False, indent=2))
        if not _confirm(input_fn):
            print_fn("Opération annulée.")
            return
        result = binary.apply_operation(document, plan.operation)
        if result.ok:
            print_fn(json.dumps(
                {"ok": True, "doc_hash": result.doc_hash}, ensure_ascii=False))
            return
        error_payload = {
            "ok": False,
            "error": result.error,
            "detail": result.detail,
        }
        print_fn(json.dumps(error_payload, ensure_ascii=False))
        if attempt == 0:
            print_fn("L'erreur est renvoyée au modèle pour un unique second essai.")
            previous_error = error_payload
    print_fn("Abandon après deux opérations refusées.")


def main() -> int:
    parser = argparse.ArgumentParser(description="Pilote chat de CUTMACHINE")
    parser.add_argument("document", type=Path)
    args = parser.parse_args()
    try:
        planner = planner_from_environment()
        binary = CutmachineBinary()
        print(f"Backend : {planner.backend_name} — document : {args.document}")
        while True:
            try:
                instruction = input("cutmachine> ").strip()
            except EOFError:
                print()
                return 0
            if instruction.lower() in {"quit", "exit", "q"}:
                return 0
            if not instruction:
                continue
            run_turn(planner, binary, args.document, instruction)
    except (BinaryError, PlannerError, KeyboardInterrupt) as exc:
        print(f"Erreur : {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
```

### sidecar/schema.py

```python
"""Single source of truth for planner structured output."""

from __future__ import annotations

from typing import Any


def _time(*, minimum: int | None = None) -> dict[str, Any]:
    # `minimum` is documented here for the exact operation variants below but
    # is enforced locally: Anthropic strict tools reject numeric bounds.
    del minimum
    value: dict[str, Any] = {"type": "integer"}
    return {
        "type": "object",
        "additionalProperties": False,
        "properties": {
            "value": value,
            "rate": {"type": "integer"},
        },
        "required": ["value", "rate"],
    }


RATIONAL_TIME_SCHEMA = _time()
NONNEGATIVE_TIME_SCHEMA = _time(minimum=0)
POSITIVE_TIME_SCHEMA = _time(minimum=1)

POSITIVE_QUANTITY_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "properties": {
        "value": {"type": "integer"},
        "unit": {"enum": ["Frames", "Seconds"]},
    },
    "required": ["value", "unit"],
}

EXACT_TIMELINE_ITEM_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "properties": {
        "clip_id": {"type": "string"},
        "timeline_in": RATIONAL_TIME_SCHEMA,
    },
    "required": ["clip_id", "timeline_in"],
}

INSERT_CLIP_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "properties": {
        "type": {"const": "InsertClip"},
        "track_id": {"type": "string"},
        "source_id": {"type": "string"},
        "source_in": NONNEGATIVE_TIME_SCHEMA,
        "duration": POSITIVE_TIME_SCHEMA,
        "timeline_in": NONNEGATIVE_TIME_SCHEMA,
        # CUTMACHINE generates the clip ULID and enriches the exact ripple state.
        "clip_id": {"const": ""},
        "exact_timeline": {"type": "array"},
    },
    "required": [
        "type", "track_id", "source_id", "source_in", "duration",
        "timeline_in", "clip_id", "exact_timeline",
    ],
}

REMOVE_CLIP_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "properties": {
        "type": {"const": "RemoveClip"},
        "clip_id": {"type": "string"},
        "exact_timeline": {"type": "array"},
    },
    "required": ["type", "clip_id", "exact_timeline"],
}

TRIM_CLIP_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "properties": {
        "type": {"const": "TrimClip"},
        "clip_id": {"type": "string"},
        "edge": {"enum": ["Head", "Tail"]},
        "trim_action": {"enum": ["Shorten", "Extend"]},
        "trim_amount": POSITIVE_QUANTITY_SCHEMA,
        "exact_clip": {"type": "null"},
    },
    "required": [
        "type", "clip_id", "edge", "trim_action", "trim_amount",
        "exact_clip",
    ],
}

# Common subset accepted by Ollama structured decoding and Anthropic strict
# tools. Both reject different JSON Schema composition keywords, so the
# discriminator and union of possible fields are expressed here; the exact
# per-operation required fields above are enforced again by planner.py.
OPERATION_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "properties": {
        "type": {"enum": ["InsertClip", "RemoveClip", "TrimClip"]},
        "track_id": {"type": "string"},
        "source_id": {"type": "string"},
        "source_in": NONNEGATIVE_TIME_SCHEMA,
        "duration": POSITIVE_TIME_SCHEMA,
        "timeline_in": NONNEGATIVE_TIME_SCHEMA,
        "clip_id": {"type": "string"},
        "exact_timeline": {"type": "array"},
        "edge": {"enum": ["Head", "Tail"]},
        "trim_action": {"enum": ["Shorten", "Extend"]},
        "trim_amount": POSITIVE_QUANTITY_SCHEMA,
        "exact_clip": {"type": "null"},
    },
    # Requiring the union is deliberate: conditional `required` needs schema
    # composition, unsupported by Anthropic strict tools. The adapter discards
    # fields irrelevant to the selected discriminator.
    "required": [
        "type", "track_id", "source_id", "source_in", "duration",
        "timeline_in", "clip_id", "exact_timeline", "edge", "trim_action",
        "trim_amount", "exact_clip",
    ],
}

REFUSAL_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "properties": {
        "reason": {"type": "string"},
    },
    "required": ["reason"],
}

# Both backends receive this exact object: Ollama in `format`, Anthropic in the
# sole tool's `input_schema`.
PLANNER_RESPONSE_SCHEMA: dict[str, Any] = {
    "title": "CUTMACHINE planner response",
    "type": "object",
    "additionalProperties": False,
    "properties": {
        "operation": {
            "type": ["object", "null"],
            "additionalProperties": False,
            "properties": OPERATION_SCHEMA["properties"],
            "required": OPERATION_SCHEMA["required"],
        },
        "refusal": {
            "type": ["object", "null"],
            "additionalProperties": False,
            "properties": REFUSAL_SCHEMA["properties"],
            "required": ["reason"],
        },
    },
    "required": ["operation", "refusal"],
}
```

### sidecar/test_sidecar.py

```python
from __future__ import annotations

import io
import json
import shutil
import tempfile
import unittest
from pathlib import Path
from typing import Any

from sidecar.binary import ApplyResult, CutmachineBinary
from sidecar.eval import A1, CASES, operations_equal
from sidecar.planner import (
    AnthropicPlanner,
    OllamaPlanner,
    Plan,
    Planner,
    PlannerError,
)
from sidecar.repl import run_turn
from sidecar.schema import PLANNER_RESPONSE_SCHEMA


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = Path(__file__).with_name("eval-document.json")
BINARY = ROOT / "build" / "cutmachine"


class FakeResponse:
    def __init__(self, payload: dict[str, Any]) -> None:
        self.payload = payload

    def __enter__(self) -> "FakeResponse":
        return self

    def __exit__(self, *args: Any) -> None:
        return None

    def read(self) -> bytes:
        return json.dumps(self.payload).encode()


class RecordingOpener:
    def __init__(self, response: dict[str, Any]) -> None:
        self.response = response
        self.request: Any = None
        self.timeout: float | None = None

    def __call__(self, request: Any, timeout: float) -> FakeResponse:
        self.request = request
        self.timeout = timeout
        return FakeResponse(self.response)

    @property
    def body(self) -> dict[str, Any]:
        return json.loads(self.request.data)


def remove_plan() -> dict[str, Any]:
    return {
        "operation": {
            "type": "RemoveClip", "clip_id": A1, "exact_timeline": [],
        },
        "refusal": None,
    }


def trim_plan(
    edge: str, action: str, amount: int, unit: str = "Frames"
) -> dict[str, Any]:
    return {
        "operation": {
            "type": "TrimClip",
            "clip_id": A1,
            "edge": edge,
            "trim_action": action,
            "trim_amount": {"value": amount, "unit": unit},
            "exact_clip": None,
        },
        "refusal": None,
    }


class PlannerProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not BINARY.exists():
            raise unittest.SkipTest("build/cutmachine is not available")
        cls.timeline = CutmachineBinary(BINARY).describe(FIXTURE)

    def test_ollama_receives_shared_schema_in_format(self) -> None:
        opener = RecordingOpener({
            "message": {"content": json.dumps(remove_plan())},
        })
        planner = OllamaPlanner(model="test-model", opener=opener)
        plan = planner.plan(self.timeline, "Supprime A1")
        self.assertEqual(plan.operation, remove_plan()["operation"])
        self.assertEqual(opener.body["format"], PLANNER_RESPONSE_SCHEMA)
        self.assertFalse(opener.body["stream"])
        self.assertEqual(opener.body["options"]["temperature"], 0)
        self.assertTrue(opener.request.full_url.endswith("/api/chat"))

    def test_shared_schema_uses_no_unsupported_composition_keywords(self) -> None:
        encoded = json.dumps(PLANNER_RESPONSE_SCHEMA)
        self.assertNotIn('"oneOf"', encoded)
        self.assertNotIn('"anyOf"', encoded)
        self.assertNotIn('"allOf"', encoded)

    def test_anthropic_receives_same_schema_as_tool_input(self) -> None:
        opener = RecordingOpener({
            "content": [{
                "type": "tool_use",
                "name": AnthropicPlanner.TOOL_NAME,
                "input": remove_plan(),
            }],
        })
        planner = AnthropicPlanner(
            model="test-model", api_key="test-key", opener=opener)
        plan = planner.plan(self.timeline, "Supprime A1")
        self.assertEqual(plan.operation, remove_plan()["operation"])
        tool = opener.body["tools"][0]
        self.assertEqual(tool["input_schema"], PLANNER_RESPONSE_SCHEMA)
        self.assertTrue(tool["strict"])
        self.assertEqual(
            opener.body["tool_choice"],
            {"type": "tool", "name": AnthropicPlanner.TOOL_NAME})
        self.assertTrue(opener.request.full_url.endswith("/v1/messages"))

    def test_refusal_is_supported_by_both_response_shapes(self) -> None:
        refusal = {
            "operation": None,
            "refusal": {"reason": "Deux opérations seraient nécessaires."},
        }
        ollama = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps(refusal)}}))
        anthropic = AnthropicPlanner(api_key="x", opener=RecordingOpener({
            "content": [{"type": "tool_use", "name": AnthropicPlanner.TOOL_NAME,
                         "input": refusal}]}))
        self.assertEqual(
            ollama.plan(self.timeline, "demande composée").refusal,
            refusal["refusal"]["reason"])
        self.assertEqual(
            anthropic.plan(self.timeline, "demande composée").refusal,
            refusal["refusal"]["reason"])

    def test_unknown_model_ulid_is_rejected_before_binary(self) -> None:
        invalid = remove_plan()
        invalid["operation"]["clip_id"] = "01K49999999999999999999999"
        planner = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps(invalid)}}))
        with self.assertRaisesRegex(PlannerError, "absent from the view"):
            planner.plan(self.timeline, "Supprime un clip inexistant")

    def test_older_ollama_missing_nullable_field_is_tolerated(self) -> None:
        response = remove_plan()["operation"]
        planner = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps({"operation": response})}}))
        self.assertEqual(
            planner.plan(self.timeline, "Supprime A1").operation, response)

    def test_backend_union_fields_are_normalized_to_canonical_operation(self) -> None:
        operation = {
            "type": "RemoveClip",
            "clip_id": A1,
            "edge": "Head",
            "trim_action": "Extend",
            "trim_amount": {"value": 99, "unit": "Frames"},
            "track_id": "irrelevant",
        }
        planner = AnthropicPlanner(api_key="x", opener=RecordingOpener({
            "content": [{"type": "tool_use", "name": AnthropicPlanner.TOOL_NAME,
                         "input": {"operation": operation,
                                   "refusal": {"reason": "irrelevant"}}}]}))
        self.assertEqual(
            planner.plan(self.timeline, "Supprime A1").operation,
            remove_plan()["operation"],
        )

    def test_trim_intent_is_converted_to_internal_delta_sign(self) -> None:
        expectations = (
            ("Head", "Shorten", 10),
            ("Head", "Extend", -10),
            ("Tail", "Shorten", -10),
            ("Tail", "Extend", 10),
        )
        for edge, action, expected_delta in expectations:
            with self.subTest(edge=edge, action=action):
                planner = OllamaPlanner(opener=RecordingOpener({
                    "message": {"content": json.dumps(
                        trim_plan(edge, action, 10))},
                }))
                operation = planner.plan(self.timeline, "trim").operation
                self.assertIsNotNone(operation)
                self.assertEqual(
                    operation["delta"],
                    {"value": expected_delta, "rate": 25},
                )

    def test_explicit_trim_language_controls_sign_timebase_and_type(self) -> None:
        response = trim_plan("Head", "Shorten", 99)
        response["operation"]["type"] = "InsertClip"
        planner = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps(response)},
        }))
        operation = planner.plan(
            self.timeline, "Récupère 2 secondes avant le début actuel de A1."
        ).operation
        self.assertEqual(operation, {
            "type": "TrimClip",
            "clip_id": A1,
            "edge": "Head",
            "delta": {"value": -50, "rate": 25},
            "exact_clip": None,
        })

        shorten = planner.plan(
            self.timeline, "Coupe les 10 premières images de A1."
        ).operation
        self.assertEqual(shorten["edge"], "Head")
        self.assertEqual(shorten["delta"], {"value": 10, "rate": 25})

    def test_explicit_timeline_frame_is_absolute(self) -> None:
        operation = {
            "type": "InsertClip",
            "track_id": self.timeline["tracks"][0]["id"],
            "source_id": self.timeline["sources"][1]["id"],
            "source_in": {"value": 50, "rate": 25},
            "duration": {"value": 10, "rate": 25},
            "timeline_in": {"value": 75, "rate": 25},
        }
        planner = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps({
                "operation": operation, "refusal": None})},
        }))
        normalized = planner.plan(
            self.timeline,
            "Dans le trou après A1, à l'image 50, place 10 images.",
        ).operation
        self.assertEqual(normalized["timeline_in"], {"value": 50, "rate": 25})

    def test_ordinal_entities_override_model_ulids(self) -> None:
        wrong_clip = remove_plan()
        wrong_clip["operation"]["clip_id"] = next(
            item["id"] for item in self.timeline["tracks"][1]["items"]
            if item["type"] == "clip")
        remove = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps(wrong_clip)},
        })).plan(
            self.timeline, "Enlève le deuxième clip de la piste vidéo 1."
        ).operation
        primary_clips = [
            item for item in self.timeline["tracks"][0]["items"]
            if item["type"] == "clip"]
        self.assertEqual(remove["clip_id"], primary_clips[1]["id"])

        insert = {
            "type": "InsertClip",
            "track_id": self.timeline["tracks"][1]["id"],
            "source_id": self.timeline["sources"][1]["id"],
            "source_in": {"value": 300, "rate": 25},
            "duration": {"value": 20, "rate": 25},
            "timeline_in": {"value": 1, "rate": 25},
        }
        normalized = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps({
                "operation": insert, "refusal": None})},
        })).plan(
            self.timeline,
            "Ajoute à la fin de la piste vidéo 1 vingt images de "
            "interview.mov depuis l'image source 300.",
        ).operation
        self.assertEqual(normalized["track_id"], self.timeline["tracks"][0]["id"])
        self.assertEqual(normalized["source_id"], self.timeline["sources"][0]["id"])
        self.assertEqual(normalized["timeline_in"], {"value": 150, "rate": 25})


class BinaryIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        if not BINARY.exists():
            self.skipTest("build/cutmachine is not available")
        self.directory = tempfile.TemporaryDirectory()
        self.document = Path(self.directory.name) / "document.json"
        shutil.copyfile(FIXTURE, self.document)
        self.binary = CutmachineBinary(BINARY)

    def tearDown(self) -> None:
        self.directory.cleanup()

    def test_describe_and_named_refusal(self) -> None:
        timeline = self.binary.describe(self.document)
        self.assertEqual(timeline["tracks"][0]["items"][0]["alias"], "A1")
        before = self.document.read_bytes()
        result = self.binary.apply_operation(self.document, {
            "type": "RemoveClip",
            "clip_id": "01K49999999999999999999999",
            "exact_timeline": [],
        })
        self.assertFalse(result.ok)
        self.assertEqual(result.error, "UnknownClip")
        self.assertEqual(self.document.read_bytes(), before)

    def test_reordered_operation_is_serialized_canonically(self) -> None:
        # JSON object order has no semantics, while the existing C++ operation
        # reader consumes its canonical serializer order.
        operation = {
            "exact_clip": None,
            "delta": {"rate": 25, "value": -1},
            "edge": "Tail",
            "clip_id": A1,
            "type": "TrimClip",
        }
        result = self.binary.apply_operation(self.document, operation)
        self.assertTrue(result.ok, f"{result.error}: {result.detail}")


class FakePlanner(Planner):
    def __init__(self) -> None:
        self.errors: list[dict[str, Any] | None] = []

    @property
    def backend_name(self) -> str:
        return "fake"

    def plan(self, timeline: dict[str, Any], instruction: str,
             previous_error: dict[str, Any] | None = None) -> Plan:
        self.errors.append(previous_error)
        return Plan(operation=remove_plan()["operation"])


class FakeBinary:
    def __init__(self) -> None:
        self.apply_count = 0

    def describe(self, document: Path) -> dict[str, Any]:
        return {"sources": [], "tracks": [], "duration": {"frames": 0,
                                                               "seconds": 0.0}}

    def apply_operation(self, document: Path,
                        operation: dict[str, Any]) -> ApplyResult:
        self.apply_count += 1
        if self.apply_count == 1:
            return ApplyResult(False, error="Overlap", detail="test overlap")
        return ApplyResult(True, doc_hash="abc123")


class ReplTests(unittest.TestCase):
    def test_one_retry_includes_named_binary_error(self) -> None:
        planner = FakePlanner()
        binary = FakeBinary()
        answers = iter(("o", "o"))
        output: list[str] = []
        run_turn(planner, binary, Path("unused.json"), "supprime A1",
                 input_fn=lambda _: next(answers), print_fn=output.append)
        self.assertEqual(binary.apply_count, 2)
        self.assertIsNone(planner.errors[0])
        self.assertEqual(planner.errors[1]["error"], "Overlap")
        self.assertIn("abc123", output[-1])


class EvaluationTests(unittest.TestCase):
    def test_corpus_has_exactly_fifteen_cases(self) -> None:
        self.assertEqual(len(CASES), 15)

    def test_semantic_comparison_accepts_equivalent_timebase(self) -> None:
        expected = CASES[4].expected
        actual = json.loads(json.dumps(expected))
        actual["delta"] = {"value": -20, "rate": 50}
        self.assertTrue(operations_equal(actual, expected))

    def test_all_expected_operations_are_accepted_on_fresh_documents(self) -> None:
        if not BINARY.exists():
            self.skipTest("build/cutmachine is not available")
        binary = CutmachineBinary(BINARY)
        with tempfile.TemporaryDirectory() as directory:
            for index, case in enumerate(CASES):
                document = Path(directory) / f"case-{index}.json"
                shutil.copyfile(FIXTURE, document)
                result = binary.apply_operation(document, case.expected)
                self.assertTrue(
                    result.ok,
                    f"invalid oracle for case {index + 1}: "
                    f"{result.error}: {result.detail}",
                )


if __name__ == "__main__":
    unittest.main()
```

### src/AudioPlayback.h

```cpp
#pragma once

#include "Document.h"

#include <cstddef>
#include <cstdint>
#include <string>

// Realtime timeline audio for macOS. Media is decoded once to a common float
// format; the output callback only reads immutable mix plans and never touches
// the editable Document.
class AudioPlayback {
public:
    AudioPlayback();
    ~AudioPlayback();

    bool Open(const Document& document, const std::string& baseDirectory,
              std::string& error);
    void RebuildTimeline(const Document& document);
    bool PlayFrom(RationalTime position, int direction, std::string& error);
    bool ScrubAt(RationalTime position, std::string& error);
    void Stop();
    size_t DecodedSourceCount() const;
    size_t PlannedClipCount() const;
    uint64_t ScrubTriggerCount() const;

private:
    struct Impl;
    Impl* impl_;
};
```

### src/AudioPlayback.mm

```objectivec
#include "AudioPlayback.h"

#import <AVFAudio/AVFAudio.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace {

constexpr int32_t kMixRate = 48000;
constexpr int32_t kScrubSamples = 2880;     // 60 ms
constexpr int32_t kScrubFadeSamples = 240;  // 5 ms

std::string AvError(int value) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(value, text, sizeof(text));
    return text;
}

struct PCMSource {
    std::vector<float> left;
    std::vector<float> right;
};

bool DecodeSource(const std::string& path, PCMSource& output,
                  std::string& error) {
    AVFormatContext* format = nullptr;
    int result = avformat_open_input(&format, path.c_str(), nullptr, nullptr);
    if (result < 0) {
        error = "audio open failed: " + AvError(result);
        return false;
    }
    const auto closeFormat = [](AVFormatContext* value) {
        avformat_close_input(&value);
    };
    std::unique_ptr<AVFormatContext, decltype(closeFormat)> formatOwner(
        format, closeFormat);
    result = avformat_find_stream_info(format, nullptr);
    if (result < 0) {
        error = "audio stream headers failed: " + AvError(result);
        return false;
    }
    const int streamIndex =
        av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        error = "no audio stream";
        return false;
    }
    AVStream* stream = format->streams[streamIndex];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        error = "no audio decoder";
        return false;
    }
    AVCodecContext* rawDecoder = avcodec_alloc_context3(codec);
    const auto freeDecoder = [](AVCodecContext* value) {
        avcodec_free_context(&value);
    };
    std::unique_ptr<AVCodecContext, decltype(freeDecoder)> decoder(rawDecoder,
                                                                   freeDecoder);
    if (!decoder ||
        avcodec_parameters_to_context(decoder.get(), stream->codecpar) < 0 ||
        avcodec_open2(decoder.get(), codec, nullptr) < 0) {
        error = "unable to initialize audio decoder";
        return false;
    }

    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    SwrContext* rawResampler = nullptr;
    result =
        swr_alloc_set_opts2(&rawResampler, &stereo, AV_SAMPLE_FMT_FLTP,
                            kMixRate, &decoder->ch_layout, decoder->sample_fmt,
                            decoder->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&stereo);
    const auto freeResampler = [](SwrContext* value) { swr_free(&value); };
    std::unique_ptr<SwrContext, decltype(freeResampler)> resampler(
        rawResampler, freeResampler);
    if (result < 0 || !resampler || swr_init(resampler.get()) < 0) {
        error = "unable to initialize audio resampler";
        return false;
    }

    AVPacket* rawPacket = av_packet_alloc();
    AVFrame* rawFrame = av_frame_alloc();
    const auto freePacket = [](AVPacket* value) { av_packet_free(&value); };
    const auto freeFrame = [](AVFrame* value) { av_frame_free(&value); };
    std::unique_ptr<AVPacket, decltype(freePacket)> packet(rawPacket,
                                                           freePacket);
    std::unique_ptr<AVFrame, decltype(freeFrame)> frame(rawFrame, freeFrame);
    if (!packet || !frame) {
        error = "unable to allocate audio packet/frame";
        return false;
    }

    const auto convert = [&](const AVFrame* input) {
        const int inputSamples = input ? input->nb_samples : 0;
        const int capacity =
            std::max(1, swr_get_out_samples(resampler.get(), inputSamples));
        std::vector<float> left(static_cast<size_t>(capacity));
        std::vector<float> right(static_cast<size_t>(capacity));
        uint8_t* planes[2] = {
            reinterpret_cast<uint8_t*>(left.data()),
            reinterpret_cast<uint8_t*>(right.data()),
        };
        const uint8_t* const* inputData =
            input ? const_cast<const uint8_t* const*>(input->extended_data)
                  : nullptr;
        const int count =
            swr_convert(resampler.get(), planes, capacity,
                        const_cast<const uint8_t**>(inputData), inputSamples);
        if (count < 0) return false;
        output.left.insert(output.left.end(), left.begin(),
                           left.begin() + count);
        output.right.insert(output.right.end(), right.begin(),
                            right.begin() + count);
        return true;
    };
    const auto receive = [&]() {
        while (true) {
            const int status =
                avcodec_receive_frame(decoder.get(), frame.get());
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return true;
            if (status < 0 || !convert(frame.get())) return false;
            av_frame_unref(frame.get());
        }
    };

    while (av_read_frame(format, packet.get()) >= 0) {
        if (packet->stream_index == streamIndex) {
            result = avcodec_send_packet(decoder.get(), packet.get());
            if (result < 0 && result != AVERROR(EAGAIN)) {
                error = "audio packet decode failed: " + AvError(result);
                return false;
            }
            if (!receive()) {
                error = "audio frame decode/resample failed";
                return false;
            }
        }
        av_packet_unref(packet.get());
    }
    avcodec_send_packet(decoder.get(), nullptr);
    if (!receive()) {
        error = "audio decoder drain failed";
        return false;
    }
    while (swr_get_delay(resampler.get(), kMixRate) > 0) {
        const size_t before = output.left.size();
        if (!convert(nullptr) || output.left.size() == before) break;
    }
    return !output.left.empty();
}

struct MixClip {
    std::shared_ptr<const PCMSource> source;
    int64_t timelineStart = 0;
    int64_t sourceStart = 0;
    int64_t length = 0;
};

struct MixPlan {
    std::vector<MixClip> clips;
};

}  // namespace

struct AudioPlayback::Impl {
    AVAudioEngine* engine = nil;
    AVAudioSourceNode* sourceNode = nil;
    std::map<Ulid, std::shared_ptr<const PCMSource>> sources;
    std::shared_ptr<const MixPlan> plan = std::make_shared<MixPlan>();
    std::atomic<int64_t> cursor{0};
    std::atomic<int> direction{0};
    std::atomic<int32_t> scrubRemaining{0};
    std::atomic<uint64_t> generation{0};
    std::atomic<int64_t> lastScrubSample{std::numeric_limits<int64_t>::min()};
    std::atomic<uint64_t> scrubTriggerCount{0};
};

AudioPlayback::AudioPlayback() : impl_(new Impl()) {}

AudioPlayback::~AudioPlayback() {
    Stop();
    if (impl_->engine && impl_->sourceNode)
        [impl_->engine detachNode:impl_->sourceNode];
    delete impl_;
}

bool AudioPlayback::Open(const Document& document,
                         const std::string& baseDirectory, std::string& error) {
    impl_->sources.clear();
    for (const DocumentSource& source : document.sources) {
        std::filesystem::path path(source.path);
        if (path.is_relative())
            path = std::filesystem::path(baseDirectory) / path;
        auto decoded = std::make_shared<PCMSource>();
        std::string decodeError;
        if (DecodeSource(path.lexically_normal().string(), *decoded,
                         decodeError)) {
            impl_->sources[source.id] = std::move(decoded);
        } else if (decodeError != "no audio stream") {
            std::fprintf(stderr, "Audio disabled for %s: %s\n",
                         path.string().c_str(), decodeError.c_str());
        }
    }
    RebuildTimeline(document);

    impl_->engine = [[AVAudioEngine alloc] init];
    AVAudioFormat* format =
        [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
                                         sampleRate:kMixRate
                                           channels:2
                                        interleaved:NO];
    Impl* state = impl_;
    impl_->sourceNode = [[AVAudioSourceNode alloc]
        initWithFormat:format
           renderBlock:^OSStatus(
               BOOL* isSilence, const AudioTimeStamp* timestamp,
               AVAudioFrameCount frameCount, AudioBufferList* outputData) {
             (void)timestamp;
             const auto plan = std::atomic_load(&state->plan);
             const uint64_t generation = state->generation.load();
             const int direction = state->direction.load();
             const int64_t cursor = state->cursor.load();
             const int32_t scrubRemaining = state->scrubRemaining.load();
             const bool scrubbing = scrubRemaining > 0;
             float* left = static_cast<float*>(outputData->mBuffers[0].mData);
             float* right = static_cast<float*>(outputData->mBuffers[1].mData);
             std::fill(left, left + frameCount, 0.0f);
             std::fill(right, right + frameCount, 0.0f);
             bool audible = false;
             if (direction != 0) {
                 for (AVAudioFrameCount index = 0; index < frameCount;
                      ++index) {
                     if (scrubbing && index >= static_cast<AVAudioFrameCount>(
                                                   scrubRemaining))
                         break;
                     const int64_t timelineSample =
                         cursor + static_cast<int64_t>(index) * direction;
                     for (const MixClip& clip : plan->clips) {
                         const int64_t offset =
                             timelineSample - clip.timelineStart;
                         if (offset < 0 || offset >= clip.length) continue;
                         const int64_t sourceSample = clip.sourceStart + offset;
                         if (sourceSample < 0 ||
                             sourceSample >=
                                 static_cast<int64_t>(clip.source->left.size()))
                             continue;
                         left[index] += clip.source->left[sourceSample];
                         right[index] += clip.source->right[sourceSample];
                         audible = true;
                     }
                     left[index] = std::clamp(left[index], -1.0f, 1.0f);
                     right[index] = std::clamp(right[index], -1.0f, 1.0f);
                     if (scrubbing) {
                         const int32_t elapsed =
                             kScrubSamples - scrubRemaining + index;
                         const int32_t remaining = scrubRemaining - index;
                         const float attack = std::clamp(
                             static_cast<float>(elapsed) / kScrubFadeSamples,
                             0.0f, 1.0f);
                         const float release = std::clamp(
                             static_cast<float>(remaining) / kScrubFadeSamples,
                             0.0f, 1.0f);
                         const float gain = std::min(attack, release);
                         left[index] *= gain;
                         right[index] *= gain;
                     }
                 }
                 if (state->generation.load() == generation) {
                     const int32_t consumed =
                         scrubbing
                             ? std::min<int32_t>(scrubRemaining, frameCount)
                             : static_cast<int32_t>(frameCount);
                     state->cursor.store(
                         cursor + static_cast<int64_t>(consumed) * direction);
                     if (scrubbing) {
                         const int32_t next = scrubRemaining - consumed;
                         state->scrubRemaining.store(next);
                         if (next == 0) state->direction.store(0);
                     }
                 }
             }
             *isSilence = !audible;
             return noErr;
           }];
    [impl_->engine attachNode:impl_->sourceNode];
    [impl_->engine connect:impl_->sourceNode
                        to:impl_->engine.mainMixerNode
                    format:format];
    error.clear();
    return true;
}

void AudioPlayback::RebuildTimeline(const Document& document) {
    auto plan = std::make_shared<MixPlan>();
    for (const DocumentTrack& track : document.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (track.kind == "video" && !clip.include_audio) continue;
            const auto source = impl_->sources.find(clip.source_id);
            if (source == impl_->sources.end()) continue;
            MixClip mixed;
            mixed.source = source->second;
            mixed.timelineStart = clip.timeline_in.to_frames(kMixRate);
            mixed.sourceStart = clip.source_in.to_frames(kMixRate);
            mixed.length = clip.duration.to_frames(kMixRate);
            if (mixed.length > 0) plan->clips.push_back(std::move(mixed));
        }
    }
    std::atomic_store(&impl_->plan,
                      std::static_pointer_cast<const MixPlan>(plan));
}

bool AudioPlayback::PlayFrom(RationalTime position, int direction,
                             std::string& error) {
    if (!impl_->engine || !impl_->sourceNode || direction == 0) {
        error = "audio engine is not initialized";
        return false;
    }
    impl_->generation.fetch_add(1);
    impl_->cursor.store(position.to_frames(kMixRate));
    impl_->scrubRemaining.store(0);
    impl_->lastScrubSample.store(std::numeric_limits<int64_t>::min());
    impl_->direction.store(direction < 0 ? -1 : 1);
    NSError* startError = nil;
    if (!impl_->engine.isRunning &&
        ![impl_->engine startAndReturnError:&startError]) {
        error =
            startError.localizedDescription.UTF8String ?: "audio start failed";
        impl_->direction.store(0);
        return false;
    }
    error.clear();
    return true;
}

bool AudioPlayback::ScrubAt(RationalTime position, std::string& error) {
    if (!impl_->engine || !impl_->sourceNode) {
        error = "audio engine is not initialized";
        return false;
    }
    const int64_t sample = position.to_frames(kMixRate);
    if (impl_->lastScrubSample.exchange(sample) == sample) {
        error.clear();
        return true;
    }
    impl_->scrubTriggerCount.fetch_add(1);
    impl_->generation.fetch_add(1);
    impl_->cursor.store(sample);
    impl_->scrubRemaining.store(kScrubSamples);
    impl_->direction.store(1);
    NSError* startError = nil;
    if (!impl_->engine.isRunning &&
        ![impl_->engine startAndReturnError:&startError]) {
        error =
            startError.localizedDescription.UTF8String ?: "audio scrub failed";
        impl_->direction.store(0);
        impl_->scrubRemaining.store(0);
        impl_->lastScrubSample.store(std::numeric_limits<int64_t>::min());
        return false;
    }
    error.clear();
    return true;
}

void AudioPlayback::Stop() {
    if (!impl_) return;
    impl_->generation.fetch_add(1);
    impl_->direction.store(0);
    impl_->scrubRemaining.store(0);
    impl_->lastScrubSample.store(std::numeric_limits<int64_t>::min());
    if (impl_->engine && impl_->engine.isRunning) [impl_->engine pause];
}

size_t AudioPlayback::DecodedSourceCount() const {
    return impl_->sources.size();
}

size_t AudioPlayback::PlannedClipCount() const {
    return std::atomic_load(&impl_->plan)->clips.size();
}

uint64_t AudioPlayback::ScrubTriggerCount() const {
    return impl_->scrubTriggerCount.load();
}
```

### src/Cli.cc

```cpp
#include "Cli.h"

#include "Document.h"
#include "EditLog.h"
#include "Operations.h"
#include "Timeline.h"
#include "Ulid.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

std::string EscapeJson(const std::string& input) {
    std::ostringstream output;
    for (const unsigned char character : input) {
        switch (character) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if (character < 0x20) {
                    const char digits[] = "0123456789abcdef";
                    output << "\\u00" << digits[character >> 4]
                           << digits[character & 0xf];
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    return output.str();
}

std::string ErrorJson(EditError error, const std::string& detail) {
    return "{\"ok\":false,\"error\":\"" + std::string(EditErrorName(error)) +
           "\",\"detail\":\"" + EscapeJson(detail) + "\"}\n";
}

bool ReadFile(const std::string& path, std::string& contents,
              std::string& message) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        message = "unable to open '" + path + "'";
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        message = "unable to read '" + path + "'";
        return false;
    }
    contents = buffer.str();
    return true;
}

bool WriteFile(const std::filesystem::path& path, const std::string& contents,
               std::string& message) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        message = "unable to create '" + path.string() + "'";
        return false;
    }
    output << contents;
    output.close();
    if (!output) {
        message = "unable to write '" + path.string() + "'";
        return false;
    }
    return true;
}

void RemoveIfPresent(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

bool Rename(const std::filesystem::path& from, const std::filesystem::path& to,
            std::string& message) {
    std::error_code error;
    std::filesystem::rename(from, to, error);
    if (!error) return true;
    message = "unable to rename '" + from.string() + "' to '" + to.string() +
              "': " + error.message();
    return false;
}

// Commits document and edit log as one recoverable pair. All validation and
// serialization happen before this function creates any file.
bool CommitPair(const std::string& documentPath,
                const std::string& documentJson, const std::string& logJson,
                std::string& message) {
    const std::filesystem::path document(documentPath);
    const std::filesystem::path log(EditLogPathForDocument(documentPath));
    const std::string nonce = ".cutmachine-" + GenerateUlid();
    const std::filesystem::path documentTemp =
        document.string() + nonce + ".tmp";
    const std::filesystem::path logTemp = log.string() + nonce + ".tmp";
    const std::filesystem::path documentBackup =
        document.string() + nonce + ".bak";
    const std::filesystem::path logBackup = log.string() + nonce + ".bak";

    if (!WriteFile(documentTemp, documentJson, message)) return false;
    if (!WriteFile(logTemp, logJson, message)) {
        RemoveIfPresent(documentTemp);
        return false;
    }

    std::error_code existsError;
    const bool hadLog = std::filesystem::exists(log, existsError);
    if (existsError) {
        message = "unable to inspect edit log '" + log.string() +
                  "': " + existsError.message();
        RemoveIfPresent(documentTemp);
        RemoveIfPresent(logTemp);
        return false;
    }

    if (!Rename(document, documentBackup, message)) {
        RemoveIfPresent(documentTemp);
        RemoveIfPresent(logTemp);
        return false;
    }
    if (hadLog && !Rename(log, logBackup, message)) {
        std::string ignored;
        Rename(documentBackup, document, ignored);
        RemoveIfPresent(documentTemp);
        RemoveIfPresent(logTemp);
        return false;
    }
    if (!Rename(documentTemp, document, message)) {
        std::string ignored;
        if (hadLog) Rename(logBackup, log, ignored);
        Rename(documentBackup, document, ignored);
        RemoveIfPresent(logTemp);
        return false;
    }
    if (!Rename(logTemp, log, message)) {
        std::string ignored;
        RemoveIfPresent(document);
        Rename(documentBackup, document, ignored);
        if (hadLog) Rename(logBackup, log, ignored);
        return false;
    }

    RemoveIfPresent(documentBackup);
    if (hadLog) RemoveIfPresent(logBackup);
    return true;
}

std::string DecimalSeconds(const RationalTime& time) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(9)
           << (static_cast<long double>(time.value) /
               static_cast<long double>(time.rate));
    std::string text = output.str();
    while (text.size() > 2 && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.push_back('0');
    return text;
}

void WriteTime(std::ostringstream& output, const RationalTime& time,
               const MediaRate& frameRate) {
    output << "{\"frames\":" << time.to_frames(frameRate.num, frameRate.den)
           << ",\"seconds\":" << DecimalSeconds(time) << '}';
}

std::string AliasPrefix(size_t ordinal) {
    std::string prefix;
    do {
        prefix.insert(prefix.begin(),
                      static_cast<char>('A' + static_cast<int>(ordinal % 26)));
        ordinal = ordinal / 26;
        if (ordinal == 0) break;
        --ordinal;
    } while (true);
    return prefix;
}

MediaRate PresentationRate(const Document& document) {
    if (!document.sources.empty()) return document.sources.front().rate;
    return {1, 1};
}

std::string Describe(const Document& document) {
    const MediaRate timelineRate = PresentationRate(document);
    Timeline timeline(document);
    const RationalTime duration = timeline.Duration();
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "{\"timeline\":{\"sources\":[";
    for (size_t index = 0; index < document.sources.size(); ++index) {
        if (index) output << ',';
        const DocumentSource& source = document.sources[index];
        output << "{\"id\":\"" << EscapeJson(source.id) << "\",\"file\":\""
               << EscapeJson(
                      std::filesystem::path(source.path).filename().string())
               << "\",\"frame_rate\":\"" << source.rate.num << '/'
               << source.rate.den << "\",\"duration\":";
        WriteTime(output, source.duration, source.rate);
        output << '}';
    }
    output << "],\"tracks\":[";

    std::vector<const DocumentTrack*> tracks;
    for (const DocumentTrack& track : document.tracks) tracks.push_back(&track);
    std::stable_sort(tracks.begin(), tracks.end(),
                     [](const DocumentTrack* left, const DocumentTrack* right) {
                         return left->index < right->index;
                     });
    for (size_t trackOrdinal = 0; trackOrdinal < tracks.size();
         ++trackOrdinal) {
        if (trackOrdinal) output << ',';
        const DocumentTrack& track = *tracks[trackOrdinal];
        output << "{\"id\":\"" << EscapeJson(track.id) << "\",\"kind\":\""
               << EscapeJson(track.kind) << "\",\"index\":" << track.index
               << ",\"items\":[";
        RationalTime cursor{0, 1};
        bool firstItem = true;
        for (size_t clipIndex = 0; clipIndex < track.clips.size();
             ++clipIndex) {
            const DocumentClip& clip = track.clips[clipIndex];
            const DocumentSource* source = document.FindSource(clip.source_id);
            if (cursor < clip.timeline_in) {
                if (!firstItem) output << ',';
                output << "{\"type\":\"gap\",\"timeline_in\":";
                WriteTime(output, cursor, timelineRate);
                output << ",\"duration\":";
                WriteTime(output, clip.timeline_in.sub(cursor), timelineRate);
                output << '}';
                firstItem = false;
            }
            if (!firstItem) output << ',';
            output << "{\"type\":\"clip\",\"alias\":\""
                   << AliasPrefix(trackOrdinal) << (clipIndex + 1)
                   << "\",\"id\":\"" << EscapeJson(clip.id)
                   << "\",\"source_id\":\"" << EscapeJson(clip.source_id)
                   << "\",\"source_in\":";
            WriteTime(output, clip.source_in, source->rate);
            output << ",\"timeline_in\":";
            WriteTime(output, clip.timeline_in, timelineRate);
            output << ",\"duration\":";
            WriteTime(output, clip.duration, timelineRate);
            output << ",\"include_audio\":"
                   << (clip.include_audio ? "true" : "false");
            if (!clip.link_group_id.empty())
                output << ",\"link_group_id\":\""
                       << EscapeJson(clip.link_group_id) << "\"";
            if (!clip.sync_anchor_clip_id.empty()) {
                output << ",\"sync_anchor_clip_id\":\""
                       << EscapeJson(clip.sync_anchor_clip_id)
                       << "\",\"sync_reference_delta\":";
                WriteTime(output, clip.sync_reference_delta, timelineRate);
            }
            output << '}';
            firstItem = false;
            cursor = clip.timeline_in.add(clip.duration);
        }
        if (cursor < duration) {
            if (!firstItem) output << ',';
            output << "{\"type\":\"gap\",\"timeline_in\":";
            WriteTime(output, cursor, timelineRate);
            output << ",\"duration\":";
            WriteTime(output, duration.sub(cursor), timelineRate);
            output << '}';
        }
        output << "]}";
    }
    output << "],\"duration\":";
    WriteTime(output, duration, timelineRate);
    output << "},\"library\":[";
    std::set<Ulid> usedMedia;
    for (const DocumentTrack& track : document.tracks) {
        for (const DocumentClip& clip : track.clips) {
            usedMedia.insert(clip.source_id);
        }
    }
    for (size_t index = 0; index < document.library.size(); ++index) {
        if (index) output << ',';
        const LibraryMedia& media = document.library[index];
        output << "{\"alias\":\"M" << (index + 1) << "\",\"id\":\""
               << EscapeJson(media.id) << "\",\"path\":\""
               << EscapeJson(media.path) << "\",\"filename\":\""
               << EscapeJson(media.filename) << "\"";
        if (media.metadata_complete) {
            output << ",\"codec\":\"" << EscapeJson(media.codec)
                   << "\",\"width\":" << media.width
                   << ",\"height\":" << media.height
                   << ",\"rotation_degrees\":" << media.rotation_degrees
                   << ",\"pixel_format\":\""
                   << EscapeJson(media.pixel_format)
                   << "\",\"color_range\":\""
                   << EscapeJson(media.color_range)
                   << "\",\"color_space\":\""
                   << EscapeJson(media.color_space)
                   << "\",\"color_transfer\":\""
                   << EscapeJson(media.color_transfer)
                   << "\",\"color_primaries\":\""
                   << EscapeJson(media.color_primaries) << "\"";
        }
        output << ",\"rate\":{\"num\":" << media.rate.num
               << ",\"den\":" << media.rate.den
               << "},\"duration\":{\"value\":" << media.duration.value
               << ",\"rate\":" << media.duration.rate << "}";
        if (media.metadata_complete) {
            output << ",\"orientation\":\"" << EscapeJson(media.orientation)
                   << "\",\"has_audio\":"
                   << (media.has_audio ? "true" : "false");
            if (media.has_audio) {
                output << ",\"audio_rate\":" << media.audio_rate
                       << ",\"audio_channels\":" << media.audio_channels;
            }
        }
        if (!media.bin_id.empty())
            output << ",\"bin_id\":\"" << EscapeJson(media.bin_id) << "\"";
        output << ",\"in_use\":"
               << (usedMedia.count(media.id) ? "true" : "false") << '}';
    }
    output << "],\"bins\":[";
    for (size_t index = 0; index < document.bins.size(); ++index) {
        if (index) output << ',';
        output << "{\"id\":\"" << EscapeJson(document.bins[index].id)
               << "\",\"name\":\"" << EscapeJson(document.bins[index].name)
               << "\"";
        if (!document.bins[index].parent_id.empty())
            output << ",\"parent_id\":\""
                   << EscapeJson(document.bins[index].parent_id) << "\"";
        output << "}";
    }
    output << "]}\n";
    return output.str();
}

std::string CanonicalHash(const std::string& json) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char byte : json) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

}  // namespace

std::string EditLogPathForDocument(const std::string& documentPath) {
    return documentPath + ".editlog.json";
}

bool CommitDocumentAndEditLog(const std::string& documentPath,
                              const Document& document, const EditLog& log,
                              std::string& message) {
    return CommitPair(documentPath, document.SaveToString(), log.Serialize(),
                      message);
}

int DescribeCommand(const std::string& documentPath, std::string& output) {
    std::string json;
    std::string message;
    if (!ReadFile(documentPath, json, message)) {
        output = ErrorJson(EditError::IoError, message);
        return 1;
    }
    Document document;
    if (!Document::LoadFromString(json, document, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return 1;
    }
    try {
        output = Describe(document);
        return 0;
    } catch (const std::exception& exception) {
        output = ErrorJson(EditError::ArithmeticError, exception.what());
        return 1;
    }
}

int ApplyOperationCommand(const std::string& documentPath,
                          const std::string& operationJson,
                          std::string& output) {
    Operation operation = RemoveClipOperation{};
    EditError error = EditError::None;
    std::string message;
    if (!DeserializeOperation(operationJson, operation, error, message)) {
        output = ErrorJson(error, message);
        return 1;
    }

    std::string documentJson;
    if (!ReadFile(documentPath, documentJson, message)) {
        output = ErrorJson(EditError::IoError, message);
        return 1;
    }
    Document document;
    if (!Document::LoadFromString(documentJson, document, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return 1;
    }

    EditLog log;
    const std::string logPath = EditLogPathForDocument(documentPath);
    std::error_code existsError;
    const bool logExists = std::filesystem::exists(logPath, existsError);
    if (existsError) {
        output = ErrorJson(EditError::IoError,
                           "unable to inspect edit log '" + logPath +
                               "': " + existsError.message());
        return 1;
    }
    if (logExists) {
        std::string logJson;
        if (!ReadFile(logPath, logJson, message)) {
            output = ErrorJson(EditError::IoError, message);
            return 1;
        }
        if (!EditLog::Deserialize(logJson, log, error, message)) {
            output = ErrorJson(error, message);
            return 1;
        }
    }

    if (!log.Apply(document, std::move(operation), error, message)) {
        output = ErrorJson(error, message);
        return 1;
    }

    const std::string updatedDocument = document.SaveToString();
    if (!CommitDocumentAndEditLog(documentPath, document, log, message)) {
        output = ErrorJson(EditError::IoError, message);
        return 1;
    }
    output = "{\"ok\":true,\"doc_hash\":\"" + CanonicalHash(updatedDocument) +
             "\"}\n";
    return 0;
}
```

### src/Cli.h

```cpp
#pragma once

#include <string>

class Document;
class EditLog;

// Headless command entry points. These functions depend only on the model
// library and never initialize AppKit, Metal, media decoding, or rendering.
int DescribeCommand(const std::string& documentPath, std::string& output);
int ApplyOperationCommand(const std::string& documentPath,
                          const std::string& operationJson,
                          std::string& output);

std::string EditLogPathForDocument(const std::string& documentPath);

// Shared transactional persistence used by both the headless command and the
// graphical editor after an EditLog operation succeeds.
bool CommitDocumentAndEditLog(const std::string& documentPath,
                              const Document& document, const EditLog& log,
                              std::string& message);
```

### src/ColorManagement.cc

```cpp
#include "ColorManagement.h"

#include <cmath>
#include <stdexcept>

YuvCodeParameters BuildYuvCodeParameters(int bitDepth, bool fullRange) {
    if (bitDepth < 8 || bitDepth > 16)
        throw std::invalid_argument("YUV bit depth must be between 8 and 16");
    const uint32_t maximumCode =
        bitDepth == 16 ? 65535u : ((1u << bitDepth) - 1u);
    YuvCodeParameters result;
    result.sample_scale =
        bitDepth > 8 ? 65535.0f / maximumCode : 1.0f;
    if (fullRange) {
        result.chroma_offset =
            static_cast<float>(1u << (bitDepth - 1)) / maximumCode;
        return result;
    }
    const uint32_t shift = static_cast<uint32_t>(bitDepth - 8);
    result.y_offset = static_cast<float>(16u << shift) / maximumCode;
    result.y_scale = static_cast<float>(maximumCode) / (219u << shift);
    result.chroma_offset =
        static_cast<float>(128u << shift) / maximumCode;
    result.chroma_scale =
        static_cast<float>(maximumCode) / (224u << shift);
    return result;
}

YuvMatrixParameters BuildYuvMatrixParameters(bool bt2020NonConstant) {
    if (bt2020NonConstant)
        return {1.4746f, -0.164553f, -0.571353f, 1.8814f};
    return {};
}

double DecodeSonySLog3(double signal) {
    constexpr double breakpoint = 171.2102946929 / 1023.0;
    if (signal >= breakpoint)
        return std::pow(10.0, (signal * 1023.0 - 420.0) / 261.5) *
                   0.19 -
               0.01;
    return (signal * 1023.0 - 95.0) * 0.01125 /
           (171.2102946929 - 95.0);
}

double EncodeAcesCct(double linearAp1) {
    if (linearAp1 <= 0.0078125)
        return linearAp1 * 10.5402377416545 + 0.0729055341958355;
    return (std::log2(linearAp1) + 9.72) / 17.52;
}

double DecodeAcesCct(double acesCct) {
    constexpr double breakpoint = 0.155251141552511;
    if (acesCct <= breakpoint)
        return (acesCct - 0.0729055341958355) / 10.5402377416545;
    return std::exp2(acesCct * 17.52 - 9.72);
}

double EncodeHlg(double sceneLinear) {
    constexpr double a = 0.17883277;
    constexpr double b = 0.28466892;
    constexpr double c = 0.55991073;
    if (sceneLinear <= 0.0) return 0.0;
    if (sceneLinear <= 1.0 / 12.0) return std::sqrt(3.0 * sceneLinear);
    return a * std::log(12.0 * sceneLinear - b) + c;
}

double DecodeHlg(double signal) {
    constexpr double a = 0.17883277;
    constexpr double b = 0.28466892;
    constexpr double c = 0.55991073;
    if (signal <= 0.5) return signal * signal / 3.0;
    return (std::exp((signal - c) / a) + b) / 12.0;
}

double HlgSceneReflectionScale() { return DecodeHlg(0.75) / 0.9; }
```

### src/ColorManagement.h

```cpp
#pragma once

struct YuvCodeParameters {
    float sample_scale = 1.0f;
    float y_offset = 0.0f;
    float y_scale = 1.0f;
    float chroma_offset = 0.5f;
    float chroma_scale = 1.0f;
};

struct YuvMatrixParameters {
    float red_from_cr = 1.5748f;
    float green_from_cb = -0.187324f;
    float green_from_cr = -0.468124f;
    float blue_from_cb = 1.8556f;
};

YuvCodeParameters BuildYuvCodeParameters(int bitDepth, bool fullRange);
YuvMatrixParameters BuildYuvMatrixParameters(bool bt2020NonConstant);

double DecodeSonySLog3(double signal);
double EncodeAcesCct(double linearAp1);
double DecodeAcesCct(double acesCct);
double EncodeHlg(double sceneLinear);
double DecodeHlg(double signal);

// Scene-reflection scale that maps Sony's 90% white to BT.2408 HLG
// Reference White at signal 0.75.
double HlgSceneReflectionScale();
```

### src/DecodeWorker.cc

```cpp
#include "DecodeWorker.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {

class InFlightFrame {
public:
    explicit InFlightFrame(PerformanceMetrics& metrics) : metrics_(metrics) {
        metrics_.FrameStarted();
    }
    ~InFlightFrame() { metrics_.FrameFinished(); }

private:
    PerformanceMetrics& metrics_;
};

}  // namespace

DecodeWorker::DecodeWorker(const FrameCache::SourceId& sourceId,
                           FrameCache& cache, PerformanceMetrics& metrics)
    : sourceId_(sourceId), cache_(cache), metrics_(metrics) {}

DecodeWorker::~DecodeWorker() {
    Stop();
    if (registeredSource_) {
        cache_.UnregisterSource(sourceId_);
    }
}

bool DecodeWorker::Open(const std::string& path, int threadCount) {
    if (!source_.Open(path, threadCount)) {
        return false;
    }
    cache_.RegisterSource(sourceId_);
    registeredSource_ = true;
    return true;
}

void DecodeWorker::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        return;
    }
    started_ = true;
    stopping_ = false;
    thread_ = std::thread(&DecodeWorker::Run, this);
}

void DecodeWorker::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            return;
        }
        stopping_ = true;
    }
    wakeup_.notify_one();
    if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
}

void DecodeWorker::RequestFrame(int64_t frameIndex) {
    const auto requestStart = std::chrono::steady_clock::now();
    const int64_t clamped = std::clamp<int64_t>(
        frameIndex, 0, std::max<int64_t>(0, FrameCount() - 1));
    const bool cacheHit = cache_.Contains(sourceId_, clamped);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (requestGeneration_ > 0 && clamped == requestedFrame_) {
            return;
        }
        if (previousRequestedFrame_ >= 0 &&
            clamped != previousRequestedFrame_) {
            const int direction = clamped > previousRequestedFrame_ ? 1 : -1;
            const int64_t elapsedMicroseconds =
                hasPreviousRequestTime_
                    ? std::chrono::duration_cast<std::chrono::microseconds>(
                          requestStart - previousRequestAt_)
                          .count()
                    : 0;
            const int64_t frameDelta =
                std::llabs(clamped - previousRequestedFrame_);
            const bool fasterThanPlayback =
                elapsedMicroseconds > 0 &&
                static_cast<__int128>(frameDelta) * 1000000 *
                        source_.FrameRateDenominator() >
                    static_cast<__int128>(elapsedMicroseconds) *
                        source_.FrameRateNumerator();
            const int requiredSamples = fasterThanPlayback ? 1 : 2;
            if (direction == candidateDirection_) {
                ++candidateDirectionSamples_;
            } else {
                candidateDirection_ = direction;
                candidateDirectionSamples_ = 1;
            }
            if (candidateDirectionSamples_ >= requiredSamples) {
                stableDirection_ = candidateDirection_;
            }
        }
        previousRequestedFrame_ = clamped;
        previousRequestAt_ = requestStart;
        hasPreviousRequestTime_ = true;
        requestedFrame_ = clamped;
        requestWasHit_ = cacheHit;
        requestStartedAt_ = requestStart;
        ++requestGeneration_;
    }
    metrics_.RecordRequest(cacheHit);
    if (cacheHit) {
        const int64_t microseconds =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - requestStart)
                .count();
        metrics_.RecordDelivery(microseconds, true);
    }
    wakeup_.notify_one();
}

void DecodeWorker::Run() {
    uint64_t handledGeneration = 0;
    while (true) {
        int64_t targetFrame = 0;
        int direction = 1;
        bool requestWasHit = false;
        std::chrono::steady_clock::time_point requestStart;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wakeup_.wait(lock, [&] {
                return stopping_ || requestGeneration_ != handledGeneration;
            });
            if (stopping_) {
                return;
            }
            targetFrame = requestedFrame_;
            direction = stableDirection_;
            requestWasHit = requestWasHit_;
            requestStart = requestStartedAt_;
            handledGeneration = requestGeneration_;
        }

        if (!cache_.Contains(sourceId_, targetFrame) &&
            !FillAscending(targetFrame, targetFrame, handledGeneration)) {
            continue;
        }

        if (RequestChanged(handledGeneration)) {
            continue;
        }
        if (!requestWasHit) {
            const int64_t microseconds =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - requestStart)
                    .count();
            metrics_.RecordDelivery(microseconds, false);
        }

        const FrameCache::PrefetchWindow window =
            cache_.WindowForSource(sourceId_);
        const int64_t aheadCount = static_cast<int64_t>(window.ahead);
        const int64_t behindCount = static_cast<int64_t>(window.behind);
        const int64_t lastFrame = FrameCount() - 1;
        const int64_t behindFirst =
            direction > 0 ? std::max<int64_t>(0, targetFrame - behindCount)
                          : targetFrame + 1;
        const int64_t behindLast =
            direction > 0
                ? targetFrame - 1
                : std::min<int64_t>(lastFrame, targetFrame + behindCount);
        if (!FillAscending(behindFirst, behindLast, handledGeneration)) {
            continue;
        }

        const int64_t aheadFirst =
            direction > 0 ? targetFrame + 1
                          : std::max<int64_t>(0, targetFrame - aheadCount);
        const int64_t aheadLast =
            direction > 0
                ? std::min<int64_t>(lastFrame, targetFrame + aheadCount)
                : targetFrame - 1;
        if (direction > 0) {
            FillAscending(aheadFirst, aheadLast, handledGeneration);
        } else {
            FillReverseAhead(aheadFirst, aheadLast, handledGeneration);
        }
    }
}

bool DecodeWorker::DecodeAt(int64_t frameIndex, uint64_t generation) {
    if (RequestChanged(generation)) {
        return false;
    }
    const AVFrame* decoded = nullptr;
    int64_t pts = 0;
    InFlightFrame inFlight(metrics_);
    if (!source_.DecodeFrame(frameIndex, decoded, pts)) {
        nextSequentialFrame_ = -1;
        return false;
    }
    int64_t actualFrame = source_.FrameIndexForPts(pts);
    if (actualFrame < 0) {
        actualFrame = frameIndex;
    }
    cache_.Put(sourceId_, actualFrame, decoded);
    nextSequentialFrame_ = actualFrame + 1;
    return !RequestChanged(generation);
}

bool DecodeWorker::FillAscending(int64_t firstFrame, int64_t lastFrame,
                                 uint64_t generation) {
    if (firstFrame > lastFrame) {
        return !RequestChanged(generation);
    }

    int64_t cursor = firstFrame;
    while (cursor <= lastFrame) {
        if (RequestChanged(generation)) {
            return false;
        }
        if (cache_.TouchFrame(sourceId_, cursor)) {
            ++cursor;
            continue;
        }

        // After a direction change, the cache can contain a long contiguous
        // run while the codec cursor still points near the old reverse chunk.
        // Re-decoding that whole cached run loses the prefetch race; seek to
        // the first actual hole once the catch-up is no longer cheap.
        constexpr int64_t kMaximumSequentialCatchUp = 4;
        if (nextSequentialFrame_ < 0 || nextSequentialFrame_ > cursor ||
            cursor - nextSequentialFrame_ > kMaximumSequentialCatchUp) {
            if (!DecodeAt(cursor, generation)) {
                return false;
            }
        } else {
            while (nextSequentialFrame_ <= cursor) {
                const AVFrame* decoded = nullptr;
                int64_t pts = 0;
                InFlightFrame inFlight(metrics_);
                if (!source_.DecodeNextFrame(decoded, pts)) {
                    nextSequentialFrame_ = -1;
                    return false;
                }
                int64_t actualFrame = source_.FrameIndexForPts(pts);
                if (actualFrame < 0) {
                    actualFrame = nextSequentialFrame_;
                }
                cache_.Put(sourceId_, actualFrame, decoded);
                nextSequentialFrame_ = actualFrame + 1;
                if (RequestChanged(generation)) {
                    return false;
                }
            }
        }
        ++cursor;
    }
    return true;
}

bool DecodeWorker::FillReverseAhead(int64_t firstFrame, int64_t lastFrame,
                                    uint64_t generation) {
    // Reverse playback necessarily seeks to the start of a forward-decodable
    // chunk. A larger chunk amortizes that cold seek and lets frame threading
    // reach its sequential regime before the next slider request arrives.
    constexpr int64_t kChunkSize = 6;
    while (firstFrame <= lastFrame) {
        if (IsStopping()) {
            return false;
        }

        int64_t chunkEnd = lastFrame;
        while (chunkEnd >= firstFrame &&
               cache_.TouchFrame(sourceId_, chunkEnd)) {
            --chunkEnd;
        }
        bool speculative = false;
        if (chunkEnd < firstFrame) {
            chunkEnd = firstFrame - 1;
            if (chunkEnd < 0) {
                return !RequestChanged(generation);
            }
            // A previous speculative chunk may already cover the next frame
            // outside the logical window. Do not produce another chunk until
            // the scrub position has consumed that reserve.
            if (cache_.TouchFrame(sourceId_, chunkEnd)) {
                return !RequestChanged(generation);
            }
            speculative = true;
        }
        const int64_t chunkStart =
            std::max<int64_t>(0, chunkEnd - kChunkSize + 1);

        const AVFrame* decoded = nullptr;
        int64_t pts = 0;
        InFlightFrame inFlight(metrics_);
        if (!source_.DecodeFrame(chunkStart, decoded, pts)) {
            nextSequentialFrame_ = -1;
            return false;
        }
        int64_t actualFrame = source_.FrameIndexForPts(pts);
        if (actualFrame < 0) {
            actualFrame = chunkStart;
        }
        cache_.Put(sourceId_, actualFrame, decoded);
        nextSequentialFrame_ = actualFrame + 1;

        while (actualFrame < chunkEnd) {
            if (IsStopping() || !source_.DecodeNextFrame(decoded, pts)) {
                nextSequentialFrame_ = -1;
                return false;
            }
            actualFrame = source_.FrameIndexForPts(pts);
            if (actualFrame < 0) {
                actualFrame = nextSequentialFrame_;
            }
            cache_.Put(sourceId_, actualFrame, decoded);
            nextSequentialFrame_ = actualFrame + 1;
        }

        if (RequestChanged(generation)) {
            return false;
        }
        if (speculative) {
            return true;
        }
    }
    return true;
}

bool DecodeWorker::RequestChanged(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopping_ || requestGeneration_ != generation;
}

bool DecodeWorker::IsStopping() {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopping_;
}

int DecodeWorker::Width() const { return source_.Width(); }
int DecodeWorker::Height() const { return source_.Height(); }
int64_t DecodeWorker::FrameCount() const { return source_.FrameCount(); }
int32_t DecodeWorker::FrameRateNumerator() const {
    return source_.FrameRateNumerator();
}
int32_t DecodeWorker::FrameRateDenominator() const {
    return source_.FrameRateDenominator();
}
```

### src/DecodeWorker.h

```cpp
#pragma once

#include "FrameCache.h"
#include "MediaSource.h"
#include "PerformanceMetrics.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

class DecodeWorker {
public:
    DecodeWorker(const FrameCache::SourceId& sourceId, FrameCache& cache,
                 PerformanceMetrics& metrics);
    ~DecodeWorker();

    DecodeWorker(const DecodeWorker&) = delete;
    DecodeWorker& operator=(const DecodeWorker&) = delete;

    bool Open(const std::string& path, int threadCount);
    void Start();
    void Stop();
    void RequestFrame(int64_t frameIndex);

    int Width() const;
    int Height() const;
    int64_t FrameCount() const;
    int32_t FrameRateNumerator() const;
    int32_t FrameRateDenominator() const;

private:
    void Run();
    bool DecodeAt(int64_t frameIndex, uint64_t generation);
    bool FillAscending(int64_t firstFrame, int64_t lastFrame,
                       uint64_t generation);
    bool FillReverseAhead(int64_t firstFrame, int64_t lastFrame,
                          uint64_t generation);
    bool RequestChanged(uint64_t generation);
    bool IsStopping();

    const FrameCache::SourceId sourceId_;
    FrameCache& cache_;
    PerformanceMetrics& metrics_;
    MediaSource source_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wakeup_;
    bool started_ = false;
    bool stopping_ = false;
    uint64_t requestGeneration_ = 0;
    int64_t requestedFrame_ = 0;
    int64_t previousRequestedFrame_ = -1;
    int candidateDirection_ = 0;
    int candidateDirectionSamples_ = 0;
    int stableDirection_ = 1;
    int64_t nextSequentialFrame_ = -1;
    bool registeredSource_ = false;
    bool requestWasHit_ = false;
    std::chrono::steady_clock::time_point requestStartedAt_;
    std::chrono::steady_clock::time_point previousRequestAt_;
    bool hasPreviousRequestTime_ = false;
};
```

### src/Document.cc

```cpp
#include "Document.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

struct JsonValue {
    enum class Type { Null, Boolean, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    int64_t number = 0;
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;
};

class JsonParser {
public:
    explicit JsonParser(const std::string& input) : input_(input) {}

    JsonValue Parse() {
        SkipWhitespace();
        JsonValue value = ParseValue();
        SkipWhitespace();
        if (position_ != input_.size()) {
            Fail("unexpected trailing content");
        }
        return value;
    }

private:
    [[noreturn]] void Fail(const std::string& message) const {
        throw std::runtime_error("JSON byte " + std::to_string(position_) +
                                 ": " + message);
    }

    void SkipWhitespace() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }

    char Take() {
        if (position_ >= input_.size()) {
            Fail("unexpected end of input");
        }
        return input_[position_++];
    }

    bool Consume(char expected) {
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    JsonValue ParseValue() {
        if (position_ >= input_.size()) {
            Fail("expected a value");
        }
        switch (input_[position_]) {
            case '{':
                return ParseObject();
            case '[':
                return ParseArray();
            case '"': {
                JsonValue value;
                value.type = JsonValue::Type::String;
                value.string = ParseString();
                return value;
            }
            case 't':
                return ParseLiteral("true", true);
            case 'f':
                return ParseLiteral("false", false);
            case 'n':
                return ParseNull();
            default:
                if (input_[position_] == '-' ||
                    std::isdigit(
                        static_cast<unsigned char>(input_[position_]))) {
                    return ParseNumber();
                }
                Fail("expected an object, array, string, integer or literal");
        }
    }

    JsonValue ParseObject() {
        Take();
        JsonValue value;
        value.type = JsonValue::Type::Object;
        SkipWhitespace();
        if (Consume('}')) {
            return value;
        }
        while (true) {
            if (position_ >= input_.size() || input_[position_] != '"') {
                Fail("expected an object key");
            }
            std::string key = ParseString();
            SkipWhitespace();
            if (!Consume(':')) {
                Fail("expected ':' after object key");
            }
            SkipWhitespace();
            if (!value.object.emplace(key, ParseValue()).second) {
                Fail("duplicate object key '" + key + "'");
            }
            SkipWhitespace();
            if (Consume('}')) {
                return value;
            }
            if (!Consume(',')) {
                Fail("expected ',' or '}'");
            }
            SkipWhitespace();
        }
    }

    JsonValue ParseArray() {
        Take();
        JsonValue value;
        value.type = JsonValue::Type::Array;
        SkipWhitespace();
        if (Consume(']')) {
            return value;
        }
        while (true) {
            value.array.push_back(ParseValue());
            SkipWhitespace();
            if (Consume(']')) {
                return value;
            }
            if (!Consume(',')) {
                Fail("expected ',' or ']'");
            }
            SkipWhitespace();
        }
    }

    static void AppendUtf8(uint32_t codePoint, std::string& output) {
        if (codePoint <= 0x7f) {
            output.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
            output.push_back(
                static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        }
    }

    std::string ParseString() {
        Take();
        std::string result;
        while (position_ < input_.size()) {
            const unsigned char character = static_cast<unsigned char>(Take());
            if (character == '"') {
                return result;
            }
            if (character < 0x20) {
                Fail("unescaped control character in string");
            }
            if (character != '\\') {
                result.push_back(static_cast<char>(character));
                continue;
            }
            const char escape = Take();
            switch (escape) {
                case '"':
                    result.push_back('"');
                    break;
                case '\\':
                    result.push_back('\\');
                    break;
                case '/':
                    result.push_back('/');
                    break;
                case 'b':
                    result.push_back('\b');
                    break;
                case 'f':
                    result.push_back('\f');
                    break;
                case 'n':
                    result.push_back('\n');
                    break;
                case 'r':
                    result.push_back('\r');
                    break;
                case 't':
                    result.push_back('\t');
                    break;
                case 'u': {
                    uint32_t codePoint = 0;
                    for (int index = 0; index < 4; ++index) {
                        const char hex = Take();
                        codePoint <<= 4;
                        if (hex >= '0' && hex <= '9')
                            codePoint |= hex - '0';
                        else if (hex >= 'a' && hex <= 'f')
                            codePoint |= hex - 'a' + 10;
                        else if (hex >= 'A' && hex <= 'F')
                            codePoint |= hex - 'A' + 10;
                        else
                            Fail("invalid Unicode escape");
                    }
                    if (codePoint >= 0xd800 && codePoint <= 0xdfff) {
                        Fail("UTF-16 surrogate escapes are not supported");
                    }
                    AppendUtf8(codePoint, result);
                    break;
                }
                default:
                    Fail("invalid string escape");
            }
        }
        Fail("unterminated string");
    }

    JsonValue ParseNumber() {
        const size_t start = position_;
        Consume('-');
        if (position_ >= input_.size()) {
            Fail("incomplete integer");
        }
        if (Consume('0')) {
            if (position_ < input_.size() &&
                std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                Fail("leading zero in integer");
            }
        } else {
            if (!std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                Fail("invalid integer");
            }
            while (
                position_ < input_.size() &&
                std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
        }
        if (position_ < input_.size() &&
            (input_[position_] == '.' || input_[position_] == 'e' ||
             input_[position_] == 'E')) {
            Fail("floating-point JSON numbers are forbidden in this document");
        }
        const std::string text = input_.substr(start, position_ - start);
        errno = 0;
        char* end = nullptr;
        const long long parsed = std::strtoll(text.c_str(), &end, 10);
        if (errno == ERANGE || !end || *end != '\0') {
            Fail("integer is outside int64_t range");
        }
        JsonValue value;
        value.type = JsonValue::Type::Number;
        value.number = static_cast<int64_t>(parsed);
        return value;
    }

    JsonValue ParseLiteral(const char* literal, bool boolean) {
        for (size_t index = 0; literal[index]; ++index) {
            if (Take() != literal[index]) {
                Fail("invalid literal");
            }
        }
        JsonValue value;
        value.type = JsonValue::Type::Boolean;
        value.boolean = boolean;
        return value;
    }

    JsonValue ParseNull() {
        const char literal[] = "null";
        for (size_t index = 0; literal[index]; ++index) {
            if (Take() != literal[index]) {
                Fail("invalid literal");
            }
        }
        return {};
    }

    const std::string& input_;
    size_t position_ = 0;
};

const JsonValue& Require(const JsonValue& object, const std::string& key,
                         JsonValue::Type type, const std::string& context) {
    if (object.type != JsonValue::Type::Object) {
        throw std::runtime_error(context + " must be an object");
    }
    const auto found = object.object.find(key);
    if (found == object.object.end()) {
        throw std::runtime_error(context + " is missing '" + key + "'");
    }
    if (found->second.type != type) {
        throw std::runtime_error(context + "." + key +
                                 " has the wrong JSON type");
    }
    return found->second;
}

const JsonValue* Optional(const JsonValue& object, const std::string& key,
                          JsonValue::Type type, const std::string& context) {
    if (object.type != JsonValue::Type::Object) {
        throw std::runtime_error(context + " must be an object");
    }
    const auto found = object.object.find(key);
    if (found == object.object.end()) return nullptr;
    if (found->second.type != type) {
        throw std::runtime_error(context + "." + key +
                                 " has the wrong JSON type");
    }
    return &found->second;
}

int32_t Int32(const JsonValue& object, const std::string& key,
              const std::string& context) {
    const int64_t value =
        Require(object, key, JsonValue::Type::Number, context).number;
    if (value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) {
        throw std::runtime_error(context + "." + key +
                                 " is outside int32_t range");
    }
    return static_cast<int32_t>(value);
}

RationalTime ParseTime(const JsonValue& value, const std::string& context) {
    return {Require(value, "value", JsonValue::Type::Number, context).number,
            Int32(value, "rate", context)};
}

std::string Escape(const std::string& input) {
    std::ostringstream output;
    for (const unsigned char character : input) {
        switch (character) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if (character < 0x20) {
                    const char digits[] = "0123456789abcdef";
                    output << "\\u00" << digits[character >> 4]
                           << digits[character & 0xf];
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    return output.str();
}

void WriteTime(std::ostringstream& output, const RationalTime& time) {
    output << "{\"value\":" << time.value << ",\"rate\":" << time.rate << "}";
}

bool RegisterId(const Ulid& id, const std::string& context, std::set<Ulid>& ids,
                std::string& error) {
    if (!IsValidUlid(id)) {
        error = context + " has invalid ULID '" + id + "'";
        return false;
    }
    if (!ids.insert(id).second) {
        error = "duplicate ID '" + id + "' at " + context;
        return false;
    }
    return true;
}

}  // namespace

bool Document::Load(const std::string& path, Document& output,
                    std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open document '" + path + "'";
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "unable to read document '" + path + "'";
        return false;
    }
    return LoadFromString(contents.str(), output, error);
}

bool Document::LoadFromString(const std::string& json, Document& output,
                              std::string& error) {
    try {
        const JsonValue root = JsonParser(json).Parse();
        Document parsed;
        parsed.version = Int32(root, "version", "document");
        if (parsed.version != 1 && parsed.version != 2) {
            throw std::runtime_error("unsupported document version " +
                                     std::to_string(parsed.version));
        }

        if (const JsonValue* color = Optional(
                root, "color_management", JsonValue::Type::Object,
                "document")) {
            const std::string context = "document.color_management";
            parsed.color_management.enabled =
                Require(*color, "enabled", JsonValue::Type::Boolean, context)
                    .boolean;
            parsed.color_management.input_gamut =
                Require(*color, "input_gamut", JsonValue::Type::String,
                        context)
                    .string;
            parsed.color_management.input_transfer =
                Require(*color, "input_transfer", JsonValue::Type::String,
                        context)
                    .string;
            parsed.color_management.input_ycbcr_matrix =
                Require(*color, "input_ycbcr_matrix", JsonValue::Type::String,
                        context)
                    .string;
            if (const JsonValue* range = Optional(
                    *color, "input_range", JsonValue::Type::String, context))
                parsed.color_management.input_range = range->string;
            parsed.color_management.working_gamut =
                Require(*color, "working_gamut", JsonValue::Type::String,
                        context)
                    .string;
            parsed.color_management.output_gamut =
                Require(*color, "output_gamut", JsonValue::Type::String,
                        context)
                    .string;
            parsed.color_management.output_transfer =
                Require(*color, "output_transfer", JsonValue::Type::String,
                        context)
                    .string;
        }

        if (parsed.version == 2) {
            const JsonValue& library =
                Require(root, "library", JsonValue::Type::Array, "document");
            for (size_t index = 0; index < library.array.size(); ++index) {
                const JsonValue& item = library.array[index];
                const std::string context =
                    "library[" + std::to_string(index) + "]";
                LibraryMedia media;
                media.id = Require(item, "id", JsonValue::Type::String, context)
                               .string;
                media.path =
                    Require(item, "path", JsonValue::Type::String, context)
                        .string;
                media.filename =
                    Require(item, "filename", JsonValue::Type::String, context)
                        .string;
                if (const JsonValue* bin = Optional(
                        item, "bin_id", JsonValue::Type::String, context))
                    media.bin_id = bin->string;
                const JsonValue* codec =
                    Optional(item, "codec", JsonValue::Type::String, context);
                if (!codec) {
                    media.metadata_complete = false;
                } else {
                    media.codec = codec->string;
                    media.width = Int32(item, "width", context);
                    media.height = Int32(item, "height", context);
                    if (const JsonValue* value = Optional(
                            item, "pixel_format", JsonValue::Type::String,
                            context))
                        media.pixel_format = value->string;
                    if (const JsonValue* value = Optional(
                            item, "color_range", JsonValue::Type::String,
                            context))
                        media.color_range = value->string;
                    if (const JsonValue* value = Optional(
                            item, "color_space", JsonValue::Type::String,
                            context))
                        media.color_space = value->string;
                    if (const JsonValue* value = Optional(
                            item, "color_transfer", JsonValue::Type::String,
                            context))
                        media.color_transfer = value->string;
                    if (const JsonValue* value = Optional(
                            item, "color_primaries", JsonValue::Type::String,
                            context))
                        media.color_primaries = value->string;
                    if (Optional(item, "rotation_degrees",
                                 JsonValue::Type::Number, context))
                        media.rotation_degrees =
                            Int32(item, "rotation_degrees", context);
                    media.orientation =
                        Require(item, "orientation", JsonValue::Type::String,
                                context)
                            .string;
                    media.has_audio = Require(item, "has_audio",
                                              JsonValue::Type::Boolean, context)
                                          .boolean;
                    if (media.has_audio) {
                        media.audio_rate = Int32(item, "audio_rate", context);
                        media.audio_channels =
                            Int32(item, "audio_channels", context);
                    }
                }
                const JsonValue& rate =
                    Require(item, "rate", JsonValue::Type::Object, context);
                media.rate = {Int32(rate, "num", context + ".rate"),
                              Int32(rate, "den", context + ".rate")};
                media.duration = ParseTime(
                    Require(item, "duration", JsonValue::Type::Object, context),
                    context + ".duration");
                parsed.library.push_back(std::move(media));
            }
            if (const JsonValue* bins = Optional(
                    root, "bins", JsonValue::Type::Array, "document")) {
                for (size_t index = 0; index < bins->array.size(); ++index) {
                    const JsonValue& item = bins->array[index];
                    const std::string context =
                        "bins[" + std::to_string(index) + "]";
                    DocumentBin bin;
                    bin.id =
                        Require(item, "id", JsonValue::Type::String, context)
                            .string;
                    bin.name =
                        Require(item, "name", JsonValue::Type::String, context)
                            .string;
                    if (const JsonValue* parent =
                            Optional(item, "parent_id", JsonValue::Type::String,
                                     context))
                        bin.parent_id = parent->string;
                    parsed.bins.push_back(std::move(bin));
                }
            }
        }

        const JsonValue& sources =
            Require(root, "sources", JsonValue::Type::Array, "document");
        for (size_t index = 0; index < sources.array.size(); ++index) {
            const JsonValue& item = sources.array[index];
            const std::string context =
                "sources[" + std::to_string(index) + "]";
            DocumentSource source;
            source.id =
                Require(item, "id", JsonValue::Type::String, context).string;
            source.path =
                Require(item, "path", JsonValue::Type::String, context).string;
            const JsonValue& rate =
                Require(item, "rate", JsonValue::Type::Object, context);
            source.rate = {Int32(rate, "num", context + ".rate"),
                           Int32(rate, "den", context + ".rate")};
            source.duration = ParseTime(
                Require(item, "duration", JsonValue::Type::Object, context),
                context + ".duration");
            parsed.sources.push_back(std::move(source));
        }

        if (parsed.version == 1) {
            parsed.version = 2;
            for (const DocumentSource& source : parsed.sources) {
                LibraryMedia media;
                media.id = source.id;
                media.path = source.path;
                media.filename =
                    std::filesystem::path(source.path).filename().string();
                media.rate = source.rate;
                media.duration = source.duration;
                media.metadata_complete = false;
                parsed.library.push_back(std::move(media));
            }
        }

        const JsonValue& tracks =
            Require(root, "tracks", JsonValue::Type::Array, "document");
        for (size_t trackIndex = 0; trackIndex < tracks.array.size();
             ++trackIndex) {
            const JsonValue& item = tracks.array[trackIndex];
            const std::string context =
                "tracks[" + std::to_string(trackIndex) + "]";
            DocumentTrack track;
            track.id =
                Require(item, "id", JsonValue::Type::String, context).string;
            track.kind =
                Require(item, "kind", JsonValue::Type::String, context).string;
            track.index = Int32(item, "index", context);
            const JsonValue& clips =
                Require(item, "clips", JsonValue::Type::Array, context);
            for (size_t clipIndex = 0; clipIndex < clips.array.size();
                 ++clipIndex) {
                const JsonValue& clipValue = clips.array[clipIndex];
                const std::string clipContext =
                    context + ".clips[" + std::to_string(clipIndex) + "]";
                DocumentClip clip;
                clip.id = Require(clipValue, "id", JsonValue::Type::String,
                                  clipContext)
                              .string;
                clip.source_id = Require(clipValue, "source_id",
                                         JsonValue::Type::String, clipContext)
                                     .string;
                clip.source_in =
                    ParseTime(Require(clipValue, "source_in",
                                      JsonValue::Type::Object, clipContext),
                              clipContext + ".source_in");
                clip.duration =
                    ParseTime(Require(clipValue, "duration",
                                      JsonValue::Type::Object, clipContext),
                              clipContext + ".duration");
                clip.timeline_in =
                    ParseTime(Require(clipValue, "timeline_in",
                                      JsonValue::Type::Object, clipContext),
                              clipContext + ".timeline_in");
                if (const JsonValue* includeAudio =
                        Optional(clipValue, "include_audio",
                                 JsonValue::Type::Boolean, clipContext))
                    clip.include_audio = includeAudio->boolean;
                if (const JsonValue* linkGroup =
                        Optional(clipValue, "link_group_id",
                                 JsonValue::Type::String, clipContext))
                    clip.link_group_id = linkGroup->string;
                if (const JsonValue* anchor =
                        Optional(clipValue, "sync_anchor_clip_id",
                                 JsonValue::Type::String, clipContext)) {
                    clip.sync_anchor_clip_id = anchor->string;
                    clip.sync_reference_delta =
                        ParseTime(Require(clipValue, "sync_reference_delta",
                                          JsonValue::Type::Object, clipContext),
                                  clipContext + ".sync_reference_delta");
                }
                track.clips.push_back(std::move(clip));
            }
            parsed.tracks.push_back(std::move(track));
        }

        if (!parsed.Validate(error)) {
            return false;
        }
        output = std::move(parsed);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool Document::Save(const std::string& path, std::string& error) const {
    if (!Validate(error)) {
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "unable to create document '" + path + "'";
        return false;
    }
    output << SaveToString();
    if (!output) {
        error = "unable to write document '" + path + "'";
        return false;
    }
    error.clear();
    return true;
}

std::string Document::SaveToString() const {
    std::ostringstream output;
    output << "{\n  \"version\": " << version
           << ",\n  \"color_management\":{"
           << "\"enabled\":" << (color_management.enabled ? "true" : "false")
           << ",\"input_gamut\":\"" << Escape(color_management.input_gamut)
           << "\",\"input_transfer\":\""
           << Escape(color_management.input_transfer)
           << "\",\"input_ycbcr_matrix\":\""
           << Escape(color_management.input_ycbcr_matrix)
           << "\",\"input_range\":\""
           << Escape(color_management.input_range)
           << "\",\"working_gamut\":\""
           << Escape(color_management.working_gamut)
           << "\",\"output_gamut\":\""
           << Escape(color_management.output_gamut)
           << "\",\"output_transfer\":\""
           << Escape(color_management.output_transfer)
           << "\"},\n  \"library\": [";
    for (size_t index = 0; index < library.size(); ++index) {
        const LibraryMedia& media = library[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"id\":\""
               << Escape(media.id) << "\",\"path\":\"" << Escape(media.path)
               << "\",\"filename\":\"" << Escape(media.filename) << "\"";
        if (!media.bin_id.empty())
            output << ",\"bin_id\":\"" << Escape(media.bin_id) << "\"";
        if (media.metadata_complete) {
            output << ",\"codec\":\"" << Escape(media.codec)
                   << "\",\"width\":" << media.width
                   << ",\"height\":" << media.height
                   << ",\"rotation_degrees\":" << media.rotation_degrees
                   << ",\"pixel_format\":\"" << Escape(media.pixel_format)
                   << "\",\"color_range\":\"" << Escape(media.color_range)
                   << "\",\"color_space\":\"" << Escape(media.color_space)
                   << "\",\"color_transfer\":\""
                   << Escape(media.color_transfer)
                   << "\",\"color_primaries\":\""
                   << Escape(media.color_primaries) << "\"";
        }
        output << ",\"rate\":{\"num\":" << media.rate.num
               << ",\"den\":" << media.rate.den << "},\"duration\":";
        WriteTime(output, media.duration);
        if (media.metadata_complete) {
            output << ",\"orientation\":\"" << Escape(media.orientation)
                   << "\",\"has_audio\":"
                   << (media.has_audio ? "true" : "false");
            if (media.has_audio) {
                output << ",\"audio_rate\":" << media.audio_rate
                       << ",\"audio_channels\":" << media.audio_channels;
            }
        }
        output << "}";
    }
    if (!library.empty()) output << '\n';
    output << "  ],\n  \"bins\": [";
    for (size_t index = 0; index < bins.size(); ++index) {
        const DocumentBin& bin = bins[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"id\":\""
               << Escape(bin.id) << "\",\"name\":\"" << Escape(bin.name)
               << "\"";
        if (!bin.parent_id.empty())
            output << ",\"parent_id\":\"" << Escape(bin.parent_id) << "\"";
        output << "}";
    }
    if (!bins.empty()) output << '\n';
    output << "  ],\n  \"sources\": [";
    for (size_t index = 0; index < sources.size(); ++index) {
        const DocumentSource& source = sources[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"id\":\""
               << Escape(source.id) << "\",\"path\":\"" << Escape(source.path)
               << "\",\"rate\":{\"num\":" << source.rate.num
               << ",\"den\":" << source.rate.den << "},\"duration\":";
        WriteTime(output, source.duration);
        output << "}";
    }
    if (!sources.empty()) output << '\n';
    output << "  ],\n  \"tracks\": [";
    for (size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        const DocumentTrack& track = tracks[trackIndex];
        output << (trackIndex == 0 ? "\n" : ",\n") << "    {\"id\":\""
               << Escape(track.id) << "\",\"kind\":\"" << Escape(track.kind)
               << "\",\"index\":" << track.index << ",\"clips\":[";
        for (size_t clipIndex = 0; clipIndex < track.clips.size();
             ++clipIndex) {
            const DocumentClip& clip = track.clips[clipIndex];
            output << (clipIndex == 0 ? "\n" : ",\n") << "      {\"id\":\""
                   << Escape(clip.id) << "\",\"source_id\":\""
                   << Escape(clip.source_id) << "\",\"source_in\":";
            WriteTime(output, clip.source_in);
            output << ",\"duration\":";
            WriteTime(output, clip.duration);
            output << ",\"timeline_in\":";
            WriteTime(output, clip.timeline_in);
            output << ",\"include_audio\":"
                   << (clip.include_audio ? "true" : "false");
            if (!clip.link_group_id.empty())
                output << ",\"link_group_id\":\"" << Escape(clip.link_group_id)
                       << "\"";
            if (!clip.sync_anchor_clip_id.empty()) {
                output << ",\"sync_anchor_clip_id\":\""
                       << Escape(clip.sync_anchor_clip_id)
                       << "\",\"sync_reference_delta\":";
                WriteTime(output, clip.sync_reference_delta);
            }
            output << "}";
        }
        if (!track.clips.empty()) output << '\n';
        output << "    ]}";
    }
    if (!tracks.empty()) output << '\n';
    output << "  ]\n}\n";
    return output.str();
}

bool Document::Validate(std::string& error) const {
    if (version != 2) {
        error = "unsupported document version " + std::to_string(version);
        return false;
    }

    const auto oneOf = [](const std::string& value,
                          std::initializer_list<const char*> allowed) {
        return std::any_of(allowed.begin(), allowed.end(),
                           [&](const char* item) { return value == item; });
    };
    if (!oneOf(color_management.input_gamut,
               {"rec709", "sony_sgamut3_cine", "sony_sgamut3", "rec2020"}) ||
        !oneOf(color_management.input_transfer,
               {"rec709", "sony_slog3", "linear"}) ||
        !oneOf(color_management.input_ycbcr_matrix,
               {"auto", "bt709", "bt2020_ncl"}) ||
        !oneOf(color_management.input_range,
               {"auto", "full", "limited"}) ||
        !oneOf(color_management.working_gamut,
               {"acescct", "rec2020", "rec709"}) ||
        !oneOf(color_management.output_gamut, {"rec709", "rec2020"}) ||
        !oneOf(color_management.output_transfer, {"rec709", "hlg"})) {
        error = "color_management contains an unsupported color space or transfer";
        return false;
    }
    if (color_management.output_transfer == "hlg" &&
        color_management.output_gamut != "rec2020") {
        error = "HLG output requires the rec2020 output gamut";
        return false;
    }

    std::set<Ulid> ids;
    std::set<Ulid> binIds;
    for (size_t index = 0; index < bins.size(); ++index) {
        const DocumentBin& bin = bins[index];
        const std::string context = "bin " + std::to_string(index);
        if (!RegisterId(bin.id, context, ids, error)) return false;
        if (bin.name.empty()) {
            error = context + " has an empty name";
            return false;
        }
        binIds.insert(bin.id);
    }
    for (size_t index = 0; index < bins.size(); ++index) {
        const DocumentBin& bin = bins[index];
        if (!bin.parent_id.empty() &&
            binIds.find(bin.parent_id) == binIds.end()) {
            error = "bin " + std::to_string(index) +
                    " references unknown parent_id '" + bin.parent_id + "'";
            return false;
        }
        std::set<Ulid> ancestors;
        const DocumentBin* cursor = &bin;
        while (!cursor->parent_id.empty()) {
            if (!ancestors.insert(cursor->id).second) {
                error = "bin hierarchy contains a cycle at '" + bin.id + "'";
                return false;
            }
            cursor = FindBin(cursor->parent_id);
            if (!cursor) break;
        }
    }
    std::set<Ulid> libraryIds;
    for (size_t index = 0; index < library.size(); ++index) {
        const LibraryMedia& media = library[index];
        const std::string context = "library media " + std::to_string(index);
        if (!IsValidUlid(media.id)) {
            error = context + " has invalid ULID '" + media.id + "'";
            return false;
        }
        if (!libraryIds.insert(media.id).second) {
            error = "duplicate library ID '" + media.id + "'";
            return false;
        }
        if (media.path.empty() || media.filename.empty()) {
            error = context + " has an empty path or filename";
            return false;
        }
        if (!media.bin_id.empty() &&
            binIds.find(media.bin_id) == binIds.end()) {
            error =
                context + " references unknown bin_id '" + media.bin_id + "'";
            return false;
        }
        if (media.rate.num <= 0 || media.rate.den <= 0 ||
            media.duration.rate <= 0 || media.duration.value <= 0) {
            error = context + " has an invalid rate or duration";
            return false;
        }
        if (media.metadata_complete) {
            if (media.codec.empty() || media.width <= 0 || media.height <= 0 ||
                media.rotation_degrees < -180 || media.rotation_degrees > 180 ||
                (media.orientation != "landscape" &&
                 media.orientation != "portrait" &&
                 media.orientation != "square")) {
                error = context + " has invalid video metadata";
                return false;
            }
            if (media.has_audio &&
                (media.audio_rate <= 0 || media.audio_channels <= 0)) {
                error = context + " has invalid audio metadata";
                return false;
            }
        }
    }
    std::set<Ulid> sourceIds;
    for (size_t index = 0; index < sources.size(); ++index) {
        const DocumentSource& source = sources[index];
        const std::string context = "source " + std::to_string(index);
        if (!RegisterId(source.id, context, ids, error)) return false;
        sourceIds.insert(source.id);
        if (source.path.empty()) {
            error = context + " ('" + source.id + "') has an empty path";
            return false;
        }
        if (source.rate.num <= 0 || source.rate.den <= 0) {
            error = context + " ('" + source.id +
                    "') has a zero or negative media rate";
            return false;
        }
        if (source.duration.rate <= 0) {
            error = context + " ('" + source.id +
                    "') has a zero or negative duration rate";
            return false;
        }
        if (source.duration.value <= 0) {
            error = context + " ('" + source.id +
                    "') has a zero or negative duration";
            return false;
        }
    }
    // A mounted source deliberately shares its media ULID. Library-only IDs,
    // however, must remain globally distinct from tracks and clips.
    for (const Ulid& libraryId : libraryIds) {
        if (sourceIds.find(libraryId) == sourceIds.end() &&
            !ids.insert(libraryId).second) {
            error = "library ID '" + libraryId +
                    "' collides with another document object";
            return false;
        }
    }

    std::set<int32_t> trackIndices;
    for (size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        const DocumentTrack& track = tracks[trackIndex];
        const std::string trackContext = "track " + std::to_string(trackIndex);
        if (!RegisterId(track.id, trackContext, ids, error)) return false;
        if (!trackIndices.insert(track.index).second) {
            error = "duplicate track index " + std::to_string(track.index);
            return false;
        }
        const DocumentClip* previous = nullptr;
        for (size_t clipIndex = 0; clipIndex < track.clips.size();
             ++clipIndex) {
            const DocumentClip& clip = track.clips[clipIndex];
            const std::string context =
                trackContext + " clip " + std::to_string(clipIndex);
            if (!RegisterId(clip.id, context, ids, error)) return false;
            if (!clip.link_group_id.empty() &&
                !IsValidUlid(clip.link_group_id)) {
                error = context + " ('" + clip.id +
                        "') has invalid link_group_id '" + clip.link_group_id +
                        "'";
                return false;
            }
            if (!clip.sync_anchor_clip_id.empty() &&
                (!IsValidUlid(clip.sync_anchor_clip_id) ||
                 clip.sync_reference_delta.rate <= 0)) {
                error = context + " ('" + clip.id +
                        "') has invalid synchronization reference";
                return false;
            }
            if (clip.link_group_id.empty() &&
                !clip.sync_anchor_clip_id.empty()) {
                error = context + " ('" + clip.id +
                        "') has an incomplete link synchronization state";
                return false;
            }
            if (sourceIds.find(clip.source_id) == sourceIds.end()) {
                error = context + " ('" + clip.id +
                        "') references unknown source_id '" + clip.source_id +
                        "'";
                return false;
            }
            if (clip.source_in.rate <= 0 || clip.duration.rate <= 0 ||
                clip.timeline_in.rate <= 0) {
                error = context + " ('" + clip.id +
                        "') has a zero or negative time rate";
                return false;
            }
            if (clip.duration.value <= 0) {
                error = context + " ('" + clip.id +
                        "') has a zero or negative duration";
                return false;
            }
            if (clip.source_in.value < 0) {
                error = context + " ('" + clip.id +
                        "') has source_in before source start";
                return false;
            }
            if (clip.timeline_in.value < 0) {
                error = context + " ('" + clip.id +
                        "') has timeline_in before zero";
                return false;
            }
            const DocumentSource* source = FindSource(clip.source_id);
            try {
                if (clip.source_in.add(clip.duration) > source->duration) {
                    error = context + " ('" + clip.id +
                            "') has source_in + duration outside source bounds";
                    return false;
                }
                if (previous) {
                    if (clip.timeline_in < previous->timeline_in) {
                        error = trackContext +
                                " clips are not sorted by timeline_in at '" +
                                clip.id + "'";
                        return false;
                    }
                    if (clip.timeline_in <
                        previous->timeline_in.add(previous->duration)) {
                        error = trackContext + " clips overlap at '" + clip.id +
                                "'";
                        return false;
                    }
                }
            } catch (const std::exception& exception) {
                error = context + " has invalid rational time arithmetic: " +
                        exception.what();
                return false;
            }
            previous = &clip;
        }
    }
    error.clear();
    return true;
}

const DocumentSource* Document::FindSource(const Ulid& id) const {
    for (const DocumentSource& source : sources) {
        if (source.id == id) return &source;
    }
    return nullptr;
}

DocumentSource* Document::FindSource(const Ulid& id) {
    for (DocumentSource& source : sources) {
        if (source.id == id) return &source;
    }
    return nullptr;
}

const LibraryMedia* Document::FindLibraryMedia(const Ulid& id) const {
    for (const LibraryMedia& media : library) {
        if (media.id == id) return &media;
    }
    return nullptr;
}

LibraryMedia* Document::FindLibraryMedia(const Ulid& id) {
    for (LibraryMedia& media : library) {
        if (media.id == id) return &media;
    }
    return nullptr;
}

const DocumentBin* Document::FindBin(const Ulid& id) const {
    for (const DocumentBin& bin : bins) {
        if (bin.id == id) return &bin;
    }
    return nullptr;
}

DocumentBin* Document::FindBin(const Ulid& id) {
    for (DocumentBin& bin : bins) {
        if (bin.id == id) return &bin;
    }
    return nullptr;
}

const DocumentTrack* Document::FindTrack(const Ulid& id) const {
    for (const DocumentTrack& track : tracks) {
        if (track.id == id) return &track;
    }
    return nullptr;
}

DocumentTrack* Document::FindTrack(const Ulid& id) {
    for (DocumentTrack& track : tracks) {
        if (track.id == id) return &track;
    }
    return nullptr;
}

const DocumentClip* Document::FindClip(const Ulid& id) const {
    for (const DocumentTrack& track : tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (clip.id == id) return &clip;
        }
    }
    return nullptr;
}

DocumentClip* Document::FindClip(const Ulid& id) {
    for (DocumentTrack& track : tracks) {
        for (DocumentClip& clip : track.clips) {
            if (clip.id == id) return &clip;
        }
    }
    return nullptr;
}

const DocumentTrack* Document::FindTrackForClip(const Ulid& clipId) const {
    for (const DocumentTrack& track : tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (clip.id == clipId) return &track;
        }
    }
    return nullptr;
}

DocumentTrack* Document::FindTrackForClip(const Ulid& clipId) {
    for (DocumentTrack& track : tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (clip.id == clipId) return &track;
        }
    }
    return nullptr;
}
```

### src/Document.h

```cpp
#pragma once

#include "RationalTime.h"
#include "Ulid.h"

#include <cstdint>
#include <string>
#include <vector>

struct MediaRate {
    int32_t num = 0;
    int32_t den = 1;
};

// Project-wide display pipeline. Values are deliberately named (rather than
// opaque numeric IDs) so project files remain readable and extensible.
struct ColorManagementSettings {
    bool enabled = false;
    std::string input_gamut = "rec709";
    std::string input_transfer = "rec709";
    std::string input_ycbcr_matrix = "auto";
    std::string input_range = "auto";
    std::string working_gamut = "acescct";
    std::string output_gamut = "rec709";
    std::string output_transfer = "rec709";
};

struct DocumentSource {
    Ulid id = GenerateUlid();
    std::string path;
    MediaRate rate;
    RationalTime duration;
};

struct LibraryMedia {
    Ulid id = GenerateUlid();
    std::string path;
    std::string filename;
    std::string codec;
    int32_t width = 0;
    int32_t height = 0;
    std::string pixel_format;
    std::string color_range;
    std::string color_space;
    std::string color_transfer;
    std::string color_primaries;
    // Counterclockwise display rotation reported by FFmpeg's display matrix.
    int32_t rotation_degrees = 0;
    MediaRate rate;
    RationalTime duration;
    std::string orientation;
    bool has_audio = false;
    int32_t audio_rate = 0;
    int32_t audio_channels = 0;
    // Empty means the project root. Bins organize library media only and do
    // not change source or clip identity.
    Ulid bin_id;

    // Version-1 sources do not contain technical metadata. They are promoted
    // to the library on load without fabricating values; a later ingest of the
    // same path replaces the incomplete entry with probed metadata.
    bool metadata_complete = true;
};

struct DocumentBin {
    Ulid id = GenerateUlid();
    std::string name;
    // Empty means a top-level bin. Bins form a project-local hierarchy.
    Ulid parent_id;
};

struct DocumentClip {
    Ulid id = GenerateUlid();
    Ulid source_id;
    RationalTime source_in;
    RationalTime duration;
    RationalTime timeline_in;
    // Video clips contribute embedded audio until DetachAudio creates an
    // independent audio-track clip and clears this flag.
    bool include_audio = true;
    // Clips produced by one A/V separation share this stable group. It is a
    // selection relationship only; their edit times remain independent.
    Ulid link_group_id;
    // Exact phase relationship captured when the link is created. Drift is
    // (timeline_in-source_in) relative to the anchor, minus this reference.
    Ulid sync_anchor_clip_id;
    RationalTime sync_reference_delta{0, 1};
};

struct DocumentTrack {
    Ulid id = GenerateUlid();
    std::string kind;
    int32_t index = 0;
    std::vector<DocumentClip> clips;
};

class Document {
public:
    int32_t version = 2;
    ColorManagementSettings color_management;
    std::vector<LibraryMedia> library;
    std::vector<DocumentBin> bins;
    std::vector<DocumentSource> sources;
    std::vector<DocumentTrack> tracks;

    static bool Load(const std::string& path, Document& output,
                     std::string& error);
    static bool LoadFromString(const std::string& json, Document& output,
                               std::string& error);
    bool Save(const std::string& path, std::string& error) const;
    std::string SaveToString() const;
    bool Validate(std::string& error) const;

    const DocumentSource* FindSource(const Ulid& id) const;
    DocumentSource* FindSource(const Ulid& id);
    const LibraryMedia* FindLibraryMedia(const Ulid& id) const;
    LibraryMedia* FindLibraryMedia(const Ulid& id);
    const DocumentBin* FindBin(const Ulid& id) const;
    DocumentBin* FindBin(const Ulid& id);
    const DocumentTrack* FindTrack(const Ulid& id) const;
    DocumentTrack* FindTrack(const Ulid& id);
    const DocumentClip* FindClip(const Ulid& id) const;
    DocumentClip* FindClip(const Ulid& id);
    const DocumentTrack* FindTrackForClip(const Ulid& clipId) const;
    DocumentTrack* FindTrackForClip(const Ulid& clipId);
};
```

### src/EditLog.cc

```cpp
#include "EditLog.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

class LogReader {
public:
    explicit LogReader(const std::string& input) : input_(input) {}

    void Expect(const std::string& text) {
        Skip();
        if (input_.compare(position_, text.size(), text) != 0) {
            throw std::runtime_error("expected '" + text + "' at byte " +
                                     std::to_string(position_));
        }
        position_ += text.size();
    }

    bool Consume(const std::string& text) {
        Skip();
        if (input_.compare(position_, text.size(), text) != 0) return false;
        position_ += text.size();
        return true;
    }

    std::string Object() {
        Skip();
        if (position_ >= input_.size() || input_[position_] != '{') {
            throw std::runtime_error("expected operation object at byte " +
                                     std::to_string(position_));
        }
        const size_t start = position_;
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        while (position_ < input_.size()) {
            const char character = input_[position_++];
            if (inString) {
                if (escaped)
                    escaped = false;
                else if (character == '\\')
                    escaped = true;
                else if (character == '"')
                    inString = false;
                continue;
            }
            if (character == '"')
                inString = true;
            else if (character == '{')
                ++depth;
            else if (character == '}' && --depth == 0)
                return input_.substr(start, position_ - start);
        }
        throw std::runtime_error("unterminated operation object");
    }

    void Finish() {
        Skip();
        if (position_ != input_.size())
            throw std::runtime_error("unexpected trailing edit log JSON");
    }

private:
    void Skip() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }
    const std::string& input_;
    size_t position_ = 0;
};

void WriteEntries(std::ostringstream& output,
                  const std::vector<Entry>& entries) {
    output << '[';
    for (size_t index = 0; index < entries.size(); ++index) {
        if (index) output << ',';
        output << "{\"op\":" << SerializeOperation(entries[index].op)
               << ",\"inverse\":" << SerializeOperation(entries[index].inverse)
               << '}';
    }
    output << ']';
}

std::vector<Entry> ReadEntries(LogReader& reader) {
    std::vector<Entry> entries;
    reader.Expect("[");
    if (reader.Consume("]")) return entries;
    while (true) {
        reader.Expect("{\"op\":");
        const std::string operationJson = reader.Object();
        reader.Expect(",\"inverse\":");
        const std::string inverseJson = reader.Object();
        reader.Expect("}");
        Entry entry;
        EditError error = EditError::None;
        std::string message;
        if (!DeserializeOperation(operationJson, entry.op, error, message))
            throw std::runtime_error("invalid logged operation: " + message);
        if (!DeserializeOperation(inverseJson, entry.inverse, error, message))
            throw std::runtime_error("invalid logged inverse: " + message);
        entries.push_back(std::move(entry));
        if (reader.Consume("]")) return entries;
        reader.Expect(",");
    }
}

}  // namespace

bool EditLog::Apply(Document& document, Operation operation, EditError& error,
                    std::string& message) {
    Operation inverse = RemoveClipOperation{};
    if (!ApplyOperation(document, operation, inverse, error, message))
        return false;
    applied_.push_back({std::move(operation), std::move(inverse)});
    undone_.clear();
    return true;
}

bool EditLog::Undo(Document& document, EditError& error, std::string& message) {
    if (applied_.empty()) {
        error = EditError::EmptyUndo;
        message = "undo stack is empty";
        return false;
    }
    Operation inverse = applied_.back().inverse;
    Operation ignored = RemoveClipOperation{};
    if (!ApplyOperation(document, inverse, ignored, error, message))
        return false;
    undone_.push_back(std::move(applied_.back()));
    applied_.pop_back();
    return true;
}

bool EditLog::Redo(Document& document, EditError& error, std::string& message) {
    if (undone_.empty()) {
        error = EditError::EmptyRedo;
        message = "redo stack is empty";
        return false;
    }
    Operation operation = undone_.back().op;
    Operation ignored = RemoveClipOperation{};
    if (!ApplyOperation(document, operation, ignored, error, message))
        return false;
    applied_.push_back(std::move(undone_.back()));
    undone_.pop_back();
    return true;
}

size_t EditLog::AppliedCount() const { return applied_.size(); }
size_t EditLog::UndoneCount() const { return undone_.size(); }
const std::vector<Entry>& EditLog::AppliedEntries() const { return applied_; }
const std::vector<Entry>& EditLog::UndoneEntries() const { return undone_; }

bool EditLog::Save(const std::string& path, EditError& error,
                   std::string& message) const {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = EditError::IoError;
        message = "unable to create edit log '" + path + "'";
        return false;
    }
    output << Serialize();
    if (!output) {
        error = EditError::IoError;
        message = "unable to write edit log '" + path + "'";
        return false;
    }
    error = EditError::None;
    message.clear();
    return true;
}

bool EditLog::Load(const std::string& path, EditLog& output, EditError& error,
                   std::string& message) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = EditError::IoError;
        message = "unable to open edit log '" + path + "'";
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = EditError::IoError;
        message = "unable to read edit log '" + path + "'";
        return false;
    }
    return Deserialize(contents.str(), output, error, message);
}

std::string EditLog::Serialize() const {
    std::ostringstream output;
    output << "{\"version\":1,\"applied\":";
    WriteEntries(output, applied_);
    output << ",\"undone\":";
    WriteEntries(output, undone_);
    output << "}\n";
    return output.str();
}

bool EditLog::Deserialize(const std::string& json, EditLog& output,
                          EditError& error, std::string& message) {
    try {
        LogReader reader(json);
        reader.Expect("{\"version\":1,\"applied\":");
        EditLog parsed;
        parsed.applied_ = ReadEntries(reader);
        reader.Expect(",\"undone\":");
        parsed.undone_ = ReadEntries(reader);
        reader.Expect("}");
        reader.Finish();
        output = std::move(parsed);
        error = EditError::None;
        message.clear();
        return true;
    } catch (const std::exception& exception) {
        error = EditError::ParseError;
        message = exception.what();
        return false;
    }
}
```

### src/EditLog.h

```cpp
#pragma once

#include "Operations.h"

#include <cstddef>
#include <string>
#include <vector>

struct Entry {
    Operation op;
    Operation inverse;
};

class EditLog {
public:
    bool Apply(Document& document, Operation operation, EditError& error,
               std::string& message);
    bool Undo(Document& document, EditError& error, std::string& message);
    bool Redo(Document& document, EditError& error, std::string& message);

    size_t AppliedCount() const;
    size_t UndoneCount() const;
    const std::vector<Entry>& AppliedEntries() const;
    const std::vector<Entry>& UndoneEntries() const;

    bool Save(const std::string& path, EditError& error,
              std::string& message) const;
    static bool Load(const std::string& path, EditLog& output, EditError& error,
                     std::string& message);
    std::string Serialize() const;
    static bool Deserialize(const std::string& json, EditLog& output,
                            EditError& error, std::string& message);

private:
    std::vector<Entry> applied_;
    std::vector<Entry> undone_;
};
```

### src/FrameCache.cc

```cpp
#include "FrameCache.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cstdlib>

namespace {

AVFrame* RetainFrame(const AVFrame* source) {
    AVFrame* retained = av_frame_alloc();
    if (!retained || av_frame_ref(retained, source) < 0) {
        av_frame_free(&retained);
        return nullptr;
    }
    return retained;
}

}  // namespace

FrameCache::FrameCache(size_t byteBudget) : byteBudget_(byteBudget) {}

FrameCache::~FrameCache() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& item : entries_) {
        av_frame_free(&item.second.frame);
    }
}

size_t FrameCache::FrameBytes(const AVFrame* frame) {
    size_t bytes = 0;
    for (const AVBufferRef* buffer : frame->buf) {
        if (buffer) {
            bytes += buffer->size;
        }
    }
    for (int index = 0; index < frame->nb_extended_buf; ++index) {
        if (frame->extended_buf[index]) {
            bytes += frame->extended_buf[index]->size;
        }
    }
    return bytes;
}

void FrameCache::Touch(EntryMap::iterator entry) {
    lru_.splice(lru_.begin(), lru_, entry->second.lruPosition);
    entry->second.lruPosition = lru_.begin();
}

void FrameCache::EvictToBudget() {
    while (totalBytes_ > byteBudget_ && !lru_.empty()) {
        const Key key = lru_.back();
        auto entry = entries_.find(key);
        if (entry != entries_.end()) {
            totalBytes_ -= entry->second.bytes;
            av_frame_free(&entry->second.frame);
            entries_.erase(entry);
        }
        lru_.pop_back();
    }
}

bool FrameCache::Put(const SourceId& sourceId, int64_t frameIndex,
                     const AVFrame* frame) {
    if (!frame) {
        return false;
    }
    AVFrame* retained = RetainFrame(frame);
    if (!retained) {
        return false;
    }
    const size_t bytes = FrameBytes(retained);

    std::lock_guard<std::mutex> lock(mutex_);
    auto& knownFrameBytes = sourceFrameBytes_[sourceId];
    knownFrameBytes = std::max(knownFrameBytes, bytes);
    const Key key = {sourceId, frameIndex};
    auto existing = entries_.find(key);
    if (existing != entries_.end()) {
        Touch(existing);
        av_frame_free(&retained);
        return true;
    }

    lru_.push_front(key);
    entries_.emplace(key, Entry{retained, bytes, lru_.begin()});
    totalBytes_ += bytes;
    EvictToBudget();
    return entries_.find(key) != entries_.end();
}

AVFrame* FrameCache::GetExact(const SourceId& sourceId, int64_t frameIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto entry = entries_.find(Key{sourceId, frameIndex});
    if (entry == entries_.end()) {
        return nullptr;
    }
    Touch(entry);
    return RetainFrame(entry->second.frame);
}

AVFrame* FrameCache::GetNearest(const SourceId& sourceId, int64_t frameIndex,
                                int64_t& outFrameIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto next = entries_.lower_bound(Key{sourceId, frameIndex});
    auto best = entries_.end();

    if (next != entries_.end() && next->first.sourceId == sourceId) {
        best = next;
    }
    if (next != entries_.begin()) {
        auto previous = std::prev(next);
        if (previous->first.sourceId == sourceId) {
            if (best == entries_.end() ||
                std::llabs(previous->first.frameIndex - frameIndex) <=
                    std::llabs(best->first.frameIndex - frameIndex)) {
                best = previous;
            }
        }
    }
    if (best == entries_.end()) {
        return nullptr;
    }

    outFrameIndex = best->first.frameIndex;
    Touch(best);
    return RetainFrame(best->second.frame);
}

bool FrameCache::Contains(const SourceId& sourceId, int64_t frameIndex) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.find(Key{sourceId, frameIndex}) != entries_.end();
}

bool FrameCache::TouchFrame(const SourceId& sourceId, int64_t frameIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto entry = entries_.find(Key{sourceId, frameIndex});
    if (entry == entries_.end()) {
        return false;
    }
    Touch(entry);
    return true;
}

void FrameCache::RegisterSource(const SourceId& sourceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    activeSources_.insert(sourceId);
}

void FrameCache::UnregisterSource(const SourceId& sourceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    activeSources_.erase(sourceId);
}

FrameCache::PrefetchWindow FrameCache::WindowForSource(
    const SourceId& sourceId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto bytes = sourceFrameBytes_.find(sourceId);
    if (bytes == sourceFrameBytes_.end() || bytes->second == 0) {
        return {};
    }

    const size_t sourceCount = std::max<size_t>(1, activeSources_.size());
    const __int128 numerator = static_cast<__int128>(byteBudget_) * 70;
    const __int128 denominator =
        static_cast<__int128>(bytes->second) * 100 * sourceCount;
    const size_t total = std::max<size_t>(
        1, static_cast<size_t>((numerator + denominator - 1) / denominator));
    if (total == 1) {
        return {};
    }
    const size_t ahead = std::min<size_t>(total - 1, (total * 3 + 2) / 4);
    return PrefetchWindow{ahead, total - 1 - ahead, total};
}

size_t FrameCache::ByteBudget() const { return byteBudget_; }

size_t FrameCache::TotalBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return totalBytes_;
}

size_t FrameCache::EntryCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}
```

### src/FrameCache.h

```cpp
#pragma once

#include "Ulid.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <mutex>
#include <set>

struct AVFrame;

class FrameCache {
public:
    using SourceId = Ulid;

    struct PrefetchWindow {
        size_t ahead = 0;
        size_t behind = 0;
        size_t totalIncludingCurrent = 1;
    };

    explicit FrameCache(size_t byteBudget);
    ~FrameCache();

    FrameCache(const FrameCache&) = delete;
    FrameCache& operator=(const FrameCache&) = delete;

    bool Put(const SourceId& sourceId, int64_t frameIndex,
             const AVFrame* frame);
    AVFrame* GetExact(const SourceId& sourceId, int64_t frameIndex);
    AVFrame* GetNearest(const SourceId& sourceId, int64_t frameIndex,
                        int64_t& outFrameIndex);
    bool Contains(const SourceId& sourceId, int64_t frameIndex) const;
    bool TouchFrame(const SourceId& sourceId, int64_t frameIndex);
    void RegisterSource(const SourceId& sourceId);
    void UnregisterSource(const SourceId& sourceId);
    PrefetchWindow WindowForSource(const SourceId& sourceId) const;

    size_t ByteBudget() const;
    size_t TotalBytes() const;
    size_t EntryCount() const;

private:
    struct Key {
        SourceId sourceId;
        int64_t frameIndex;

        bool operator<(const Key& other) const {
            return sourceId < other.sourceId || (sourceId == other.sourceId &&
                                                 frameIndex < other.frameIndex);
        }
    };

    struct Entry {
        AVFrame* frame = nullptr;
        size_t bytes = 0;
        std::list<Key>::iterator lruPosition;
    };

    using EntryMap = std::map<Key, Entry>;

    static size_t FrameBytes(const AVFrame* frame);
    void Touch(EntryMap::iterator entry);
    void EvictToBudget();

    const size_t byteBudget_;
    size_t totalBytes_ = 0;
    mutable std::mutex mutex_;
    EntryMap entries_;
    std::list<Key> lru_;
    std::set<SourceId> activeSources_;
    std::map<SourceId, size_t> sourceFrameBytes_;
};
```

### src/Ingest.cc

```cpp
#include "Ingest.h"

#include "Document.h"
#include "Ulid.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/version_major.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

struct IngestError {
    std::string file;
    std::string reason;
};

std::string EscapeJson(const std::string& input) {
    std::ostringstream output;
    for (const unsigned char character : input) {
        switch (character) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if (character < 0x20) {
                    const char digits[] = "0123456789abcdef";
                    output << "\\u00" << digits[character >> 4]
                           << digits[character & 0xf];
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    return output.str();
}

std::string AvError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

std::filesystem::path Resolved(const std::filesystem::path& path,
                               std::error_code& error) {
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) return {};
    return std::filesystem::weakly_canonical(absolute, error);
}

bool ProbeImpl(const std::filesystem::path& absolutePath, LibraryMedia& media,
               std::string& reason) {
    AVFormatContext* rawContext = nullptr;
    int result = avformat_open_input(&rawContext, absolutePath.c_str(), nullptr,
                                     nullptr);
    if (result < 0) {
        reason = "unable to open media: " + AvError(result);
        return false;
    }
    struct ContextCloser {
        void operator()(AVFormatContext* context) const {
            avformat_close_input(&context);
        }
    };
    std::unique_ptr<AVFormatContext, ContextCloser> context(rawContext);
    result = avformat_find_stream_info(context.get(), nullptr);
    if (result < 0) {
        reason = "unable to read stream headers: " + AvError(result);
        return false;
    }

    const int videoIndex = av_find_best_stream(
        context.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIndex < 0) {
        reason = "no video stream";
        return false;
    }
    AVStream* video = context->streams[videoIndex];
    const AVCodecParameters* parameters = video->codecpar;
    if (parameters->width <= 0 || parameters->height <= 0) {
        reason = "video stream has invalid dimensions";
        return false;
    }
    const AVRational frameRate = video->avg_frame_rate;
    if (frameRate.num <= 0 || frameRate.den <= 0) {
        reason = "video stream has no valid avg_frame_rate";
        return false;
    }

    int64_t duration = 0;
    if (video->duration != AV_NOPTS_VALUE && video->duration > 0) {
        duration = av_rescale_q_rnd(
            video->duration, video->time_base, AVRational{1, frameRate.num},
            static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
    } else if (context->duration != AV_NOPTS_VALUE && context->duration > 0) {
        duration = av_rescale_q_rnd(
            context->duration, AVRational{1, AV_TIME_BASE},
            AVRational{1, frameRate.num},
            static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
    }
    if (duration <= 0) {
        reason = "video stream has no positive duration";
        return false;
    }

    double rotation = 0.0;
    const uint8_t* displayMatrix = nullptr;
    size_t displayMatrixSize = 0;
#if LIBAVFORMAT_VERSION_MAJOR >= 62
    const AVPacketSideData* matrixSideData = av_packet_side_data_get(
        parameters->coded_side_data, parameters->nb_coded_side_data,
        AV_PKT_DATA_DISPLAYMATRIX);
    if (matrixSideData) {
        displayMatrix = matrixSideData->data;
        displayMatrixSize = matrixSideData->size;
    }
#else
    displayMatrix = av_stream_get_side_data(video, AV_PKT_DATA_DISPLAYMATRIX,
                                            &displayMatrixSize);
#endif
    if (displayMatrix && displayMatrixSize >= 9 * sizeof(int32_t)) {
        const double value = av_display_rotation_get(
            reinterpret_cast<const int32_t*>(displayMatrix));
        if (!std::isnan(value)) rotation = value;
    }
    media.rotation_degrees = static_cast<int32_t>(std::lround(rotation));
    const double radians =
        media.rotation_degrees * 3.14159265358979323846 / 180.0;
    const double displayedWidth =
        std::abs(parameters->width * std::cos(radians)) +
        std::abs(parameters->height * std::sin(radians));
    const double displayedHeight =
        std::abs(parameters->width * std::sin(radians)) +
        std::abs(parameters->height * std::cos(radians));

    media.codec = avcodec_get_name(parameters->codec_id);
    media.width = parameters->width;
    media.height = parameters->height;
    const char* pixelFormat =
        av_get_pix_fmt_name(static_cast<AVPixelFormat>(parameters->format));
    const char* colorRange = av_color_range_name(parameters->color_range);
    const char* colorSpace = av_color_space_name(parameters->color_space);
    const char* colorTransfer = av_color_transfer_name(parameters->color_trc);
    const char* colorPrimaries =
        av_color_primaries_name(parameters->color_primaries);
    media.pixel_format = pixelFormat ? pixelFormat : "unknown";
    media.color_range = colorRange ? colorRange : "unknown";
    media.color_space = colorSpace ? colorSpace : "unknown";
    media.color_transfer = colorTransfer ? colorTransfer : "unknown";
    media.color_primaries = colorPrimaries ? colorPrimaries : "unknown";
    media.rate = {frameRate.num, frameRate.den};
    media.duration = {duration, frameRate.num};
    const double scale = std::max(displayedWidth, displayedHeight);
    if (std::abs(displayedWidth - displayedHeight) <= scale * 1e-9) {
        media.orientation = "square";
    } else {
        media.orientation =
            displayedWidth > displayedHeight ? "landscape" : "portrait";
    }

    const int audioIndex = av_find_best_stream(
        context.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioIndex >= 0) {
        const AVCodecParameters* audio = context->streams[audioIndex]->codecpar;
        if (audio->sample_rate > 0 && audio->ch_layout.nb_channels > 0) {
            media.has_audio = true;
            media.audio_rate = audio->sample_rate;
            media.audio_channels = audio->ch_layout.nb_channels;
        }
    }
    return true;
}

bool CollectFiles(const std::filesystem::path& directory, bool recursive,
                  std::vector<std::filesystem::path>& files,
                  std::string& reason) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) {
        reason = error ? error.message() : "path is not a directory";
        return false;
    }
    const auto options = std::filesystem::directory_options::none;
    if (recursive) {
        std::filesystem::recursive_directory_iterator iterator(directory,
                                                               options, error),
            end;
        if (error) {
            reason = error.message();
            return false;
        }
        for (; iterator != end; iterator.increment(error)) {
            if (error) {
                reason = error.message();
                return false;
            }
            if (iterator->is_regular_file(error))
                files.push_back(iterator->path());
            if (error) {
                reason = error.message();
                return false;
            }
        }
    } else {
        std::filesystem::directory_iterator iterator(directory, options, error),
            end;
        if (error) {
            reason = error.message();
            return false;
        }
        for (; iterator != end; iterator.increment(error)) {
            if (error) {
                reason = error.message();
                return false;
            }
            if (iterator->is_regular_file(error))
                files.push_back(iterator->path());
            if (error) {
                reason = error.message();
                return false;
            }
        }
    }
    std::sort(files.begin(), files.end());
    return true;
}

std::string ResultJson(bool ok, size_t added, size_t skipped,
                       const std::vector<IngestError>& errors) {
    std::ostringstream output;
    output << "{\"ok\":" << (ok ? "true" : "false") << ",\"added\":" << added
           << ",\"skipped\":" << skipped << ",\"errors\":[";
    for (size_t index = 0; index < errors.size(); ++index) {
        if (index) output << ',';
        output << "{\"file\":\"" << EscapeJson(errors[index].file)
               << "\",\"reason\":\"" << EscapeJson(errors[index].reason)
               << "\"}";
    }
    output << "]}\n";
    return output.str();
}

bool AtomicSave(const Document& document, const std::filesystem::path& path,
                std::string& reason) {
    const std::filesystem::path temporary =
        path.string() + ".cutmachine-" + GenerateUlid() + ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
        reason = "unable to create temporary document";
        return false;
    }
    stream << document.SaveToString();
    stream.close();
    if (!stream) {
        reason = "unable to write temporary document";
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        reason = "unable to replace document: " + error.message();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

}  // namespace

bool ProbeMediaMetadata(const std::string& path, LibraryMedia& media,
                        std::string& reason) {
    av_log_set_level(AV_LOG_ERROR);
    return ProbeImpl(std::filesystem::path(path), media, reason);
}

int IngestCommand(const std::string& documentPath,
                  const std::string& directoryPath, bool recursive,
                  std::string& output) {
    av_log_set_level(AV_LOG_ERROR);
    Document document;
    std::string reason;
    if (!Document::Load(documentPath, document, reason)) {
        output = ResultJson(false, 0, 0, {{documentPath, reason}});
        return 1;
    }

    std::error_code pathError;
    const std::filesystem::path resolvedDocument =
        Resolved(documentPath, pathError);
    if (pathError) {
        output = ResultJson(false, 0, 0, {{documentPath, pathError.message()}});
        return 1;
    }
    const std::filesystem::path resolvedDirectory =
        Resolved(directoryPath, pathError);
    if (pathError) {
        output =
            ResultJson(false, 0, 0, {{directoryPath, pathError.message()}});
        return 1;
    }
    std::vector<std::filesystem::path> files;
    if (!CollectFiles(resolvedDirectory, recursive, files, reason)) {
        output = ResultJson(false, 0, 0, {{directoryPath, reason}});
        return 1;
    }

    std::map<std::string, size_t> knownPaths;
    for (size_t index = 0; index < document.library.size(); ++index) {
        const LibraryMedia& media = document.library[index];
        std::filesystem::path stored(media.path);
        if (stored.is_relative())
            stored = resolvedDocument.parent_path() / stored;
        std::error_code error;
        const std::filesystem::path resolved = Resolved(stored, error);
        if (!error) knownPaths.emplace(resolved.string(), index);
    }

    size_t added = 0;
    size_t skipped = 0;
    bool changed = false;
    std::vector<IngestError> errors;
    for (const std::filesystem::path& candidate : files) {
        std::error_code error;
        const std::filesystem::path absolute = Resolved(candidate, error);
        if (error) {
            ++skipped;
            errors.push_back({candidate.filename().string(), error.message()});
            continue;
        }
        const auto known = knownPaths.find(absolute.string());
        if (known != knownPaths.end()) {
            ++skipped;
            LibraryMedia& existing = document.library[known->second];
            if (!existing.metadata_complete) {
                LibraryMedia enriched;
                enriched.id = existing.id;
                enriched.path = existing.path;
                enriched.filename = absolute.filename().string();
                if (ProbeMediaMetadata(absolute.string(), enriched, reason)) {
                    existing = std::move(enriched);
                    changed = true;
                } else {
                    errors.push_back({absolute.filename().string(), reason});
                }
            }
            if (existing.metadata_complete &&
                !document.FindSource(existing.id)) {
                document.sources.push_back({existing.id, existing.path,
                                            existing.rate, existing.duration});
                changed = true;
            }
            continue;
        }
        LibraryMedia media;
        media.id = GenerateUlid();
        media.filename = absolute.filename().string();
        media.path = std::filesystem::relative(
                         absolute, resolvedDocument.parent_path(), error)
                         .lexically_normal()
                         .string();
        if (error) media.path = absolute.string();
        if (!ProbeMediaMetadata(absolute.string(), media, reason)) {
            ++skipped;
            errors.push_back({media.filename, reason});
            continue;
        }
        document.library.push_back(std::move(media));
        const LibraryMedia& addedMedia = document.library.back();
        if (!document.FindSource(addedMedia.id)) {
            document.sources.push_back({addedMedia.id, addedMedia.path,
                                        addedMedia.rate, addedMedia.duration});
        }
        knownPaths.emplace(absolute.string(), document.library.size() - 1);
        ++added;
        changed = true;
    }

    if (!document.Validate(reason)) {
        output = ResultJson(false, added, skipped, {{documentPath, reason}});
        return 1;
    }
    if (changed && !AtomicSave(document, resolvedDocument, reason)) {
        output = ResultJson(false, added, skipped, {{documentPath, reason}});
        return 1;
    }
    output = ResultJson(true, added, skipped, errors);
    return 0;
}
```

### src/Ingest.h

```cpp
#pragma once

#include "Document.h"

#include <string>

// Reads container/stream headers without decoding frames. The caller owns
// identity/path fields; technical metadata is filled on success.
bool ProbeMediaMetadata(const std::string& path, LibraryMedia& media,
                        std::string& reason);

// Scans media headers only. No decoder, renderer, AppKit, or Metal object is
// created by this command.
int IngestCommand(const std::string& documentPath,
                  const std::string& directoryPath, bool recursive,
                  std::string& output);
```

### src/MediaSource.h

```cpp
#pragma once

#include <cstdint>
#include <string>

struct AVFrame;

class MediaSource {
public:
    MediaSource();
    ~MediaSource();

    bool Open(const std::string& path, int threadCount = 0,
              int threadType = -1);
    bool DecodeFrame(int64_t frameIndex, const AVFrame*& outFrame,
                     int64_t& outPts);
    bool DecodeNextFrame(const AVFrame*& outFrame, int64_t& outPts);

    int Width() const;
    int Height() const;
    int64_t FrameCount() const;
    int32_t FrameRateNumerator() const;
    int32_t FrameRateDenominator() const;
    int64_t FrameIndexForPts(int64_t pts) const;

private:
    struct Impl;
    Impl* impl_;
};
```

### src/MediaSource.mm

```objectivec
#include "MediaSource.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cstdio>

namespace {

void LogAvError(const char* operation, int error) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, text, sizeof(text));
    std::fprintf(stderr, "%s failed: %s\n", operation, text);
}

}  // namespace

struct MediaSource::Impl {
    AVFormatContext* format = nullptr;
    AVCodecContext* decoder = nullptr;
    AVStream* stream = nullptr;
    int streamIndex = -1;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    AVRational frameRate = {0, 1};
    int64_t frameCount = 0;
    bool draining = false;
};

MediaSource::MediaSource() : impl_(new Impl()) {}

MediaSource::~MediaSource() {
    av_frame_free(&impl_->frame);
    av_packet_free(&impl_->packet);
    avcodec_free_context(&impl_->decoder);
    avformat_close_input(&impl_->format);
    delete impl_;
}

bool MediaSource::Open(const std::string& path, int threadCount,
                       int threadType) {
    int result =
        avformat_open_input(&impl_->format, path.c_str(), nullptr, nullptr);
    if (result < 0) {
        LogAvError("avformat_open_input", result);
        return false;
    }

    result = avformat_find_stream_info(impl_->format, nullptr);
    if (result < 0) {
        LogAvError("avformat_find_stream_info", result);
        return false;
    }

    result = av_find_best_stream(impl_->format, AVMEDIA_TYPE_VIDEO, -1, -1,
                                 nullptr, 0);
    if (result < 0) {
        LogAvError("av_find_best_stream(video)", result);
        return false;
    }
    impl_->streamIndex = result;
    impl_->stream = impl_->format->streams[impl_->streamIndex];

    const AVCodec* codec =
        avcodec_find_decoder(impl_->stream->codecpar->codec_id);
    if (!codec) {
        std::fprintf(stderr, "No FFmpeg decoder for codec id %d\n",
                     impl_->stream->codecpar->codec_id);
        return false;
    }

    impl_->decoder = avcodec_alloc_context3(codec);
    if (!impl_->decoder) {
        std::fprintf(stderr, "avcodec_alloc_context3 failed\n");
        return false;
    }
    result =
        avcodec_parameters_to_context(impl_->decoder, impl_->stream->codecpar);
    if (result < 0) {
        LogAvError("avcodec_parameters_to_context", result);
        return false;
    }

    // Required for software decoding performance. These must be set before
    // open2.
    impl_->decoder->thread_count = threadCount;
    impl_->decoder->thread_type =
        threadType < 0 ? FF_THREAD_SLICE | FF_THREAD_FRAME : threadType;

    result = avcodec_open2(impl_->decoder, codec, nullptr);
    if (result < 0) {
        LogAvError("avcodec_open2", result);
        return false;
    }
    std::fprintf(stderr,
                 "Decoder threading: count=%d requested=0x%x active=0x%x\n",
                 impl_->decoder->thread_count, impl_->decoder->thread_type,
                 impl_->decoder->active_thread_type);

    impl_->packet = av_packet_alloc();
    impl_->frame = av_frame_alloc();
    if (!impl_->packet || !impl_->frame) {
        std::fprintf(stderr, "Unable to allocate FFmpeg packet/frame\n");
        return false;
    }

    impl_->frameRate =
        av_guess_frame_rate(impl_->format, impl_->stream, nullptr);
    if (impl_->frameRate.num <= 0 || impl_->frameRate.den <= 0) {
        std::fprintf(stderr, "Unable to determine video frame rate\n");
        return false;
    }

    if (impl_->stream->nb_frames > 0) {
        impl_->frameCount = impl_->stream->nb_frames;
    } else {
        int64_t duration = 0;
        AVRational durationTimeBase = {0, 1};
        if (impl_->stream->duration != AV_NOPTS_VALUE) {
            duration = impl_->stream->duration;
            durationTimeBase = impl_->stream->time_base;
        } else if (impl_->format->duration != AV_NOPTS_VALUE) {
            duration = impl_->format->duration;
            durationTimeBase = AVRational{1, AV_TIME_BASE};
        }
        impl_->frameCount =
            duration > 0 && durationTimeBase.num > 0
                ? std::max<int64_t>(1,
                                    av_rescale_q_rnd(duration, durationTimeBase,
                                                     av_inv_q(impl_->frameRate),
                                                     AV_ROUND_NEAR_INF))
                : 1;
    }

    std::fprintf(stderr,
                 "Opened %dx%d, %d/%d fps, %lld frames (software decode)\n",
                 Width(), Height(), impl_->frameRate.num, impl_->frameRate.den,
                 static_cast<long long>(impl_->frameCount));
    return true;
}

bool MediaSource::DecodeFrame(int64_t frameIndex, const AVFrame*& outFrame,
                              int64_t& outPts) {
    outFrame = nullptr;
    outPts = AV_NOPTS_VALUE;
    if (!impl_->format || !impl_->decoder || !impl_->stream || !impl_->packet ||
        !impl_->frame) {
        return false;
    }

    frameIndex = std::clamp<int64_t>(
        frameIndex, 0, std::max<int64_t>(0, impl_->frameCount - 1));
    int64_t targetPts = av_rescale_q(frameIndex, av_inv_q(impl_->frameRate),
                                     impl_->stream->time_base);
    if (impl_->stream->start_time != AV_NOPTS_VALUE) {
        targetPts += impl_->stream->start_time;
    }

    int result = av_seek_frame(impl_->format, impl_->streamIndex, targetPts,
                               AVSEEK_FLAG_BACKWARD);
    if (result < 0) {
        LogAvError("av_seek_frame", result);
        return false;
    }
    avcodec_flush_buffers(impl_->decoder);
    av_frame_unref(impl_->frame);
    impl_->draining = false;

    while (DecodeNextFrame(outFrame, outPts)) {
        if (outPts == AV_NOPTS_VALUE || outPts >= targetPts) {
            return true;
        }
    }
    return false;
}

bool MediaSource::DecodeNextFrame(const AVFrame*& outFrame, int64_t& outPts) {
    outFrame = nullptr;
    outPts = AV_NOPTS_VALUE;
    if (!impl_->format || !impl_->decoder || !impl_->packet || !impl_->frame) {
        return false;
    }

    av_frame_unref(impl_->frame);
    while (true) {
        int result = avcodec_receive_frame(impl_->decoder, impl_->frame);
        if (result >= 0) {
            // Some containers signal colour only at stream level. Preserve it
            // on every cached frame so the renderer never has to rediscover or
            // guess metadata that FFmpeg already parsed.
            const AVCodecParameters* parameters = impl_->stream->codecpar;
            if (impl_->frame->color_range == AVCOL_RANGE_UNSPECIFIED)
                impl_->frame->color_range = parameters->color_range;
            if (impl_->frame->colorspace == AVCOL_SPC_UNSPECIFIED)
                impl_->frame->colorspace = parameters->color_space;
            if (impl_->frame->color_trc == AVCOL_TRC_UNSPECIFIED)
                impl_->frame->color_trc = parameters->color_trc;
            if (impl_->frame->color_primaries == AVCOL_PRI_UNSPECIFIED)
                impl_->frame->color_primaries = parameters->color_primaries;
            outPts = impl_->frame->best_effort_timestamp;
            if (outPts == AV_NOPTS_VALUE) {
                outPts = impl_->frame->pts;
            }
            outFrame = impl_->frame;
            return true;
        }
        if (result == AVERROR_EOF) {
            return false;
        }
        if (result != AVERROR(EAGAIN)) {
            LogAvError("avcodec_receive_frame", result);
            return false;
        }

        if (impl_->draining) {
            return false;
        }

        while (true) {
            result = av_read_frame(impl_->format, impl_->packet);
            if (result < 0) {
                impl_->draining = true;
                result = avcodec_send_packet(impl_->decoder, nullptr);
                if (result < 0 && result != AVERROR_EOF) {
                    LogAvError("avcodec_send_packet(drain)", result);
                    return false;
                }
                break;
            }
            if (impl_->packet->stream_index != impl_->streamIndex) {
                av_packet_unref(impl_->packet);
                continue;
            }
            result = avcodec_send_packet(impl_->decoder, impl_->packet);
            av_packet_unref(impl_->packet);
            if (result < 0) {
                LogAvError("avcodec_send_packet", result);
                return false;
            }
            break;
        }
    }
}

int MediaSource::Width() const {
    return impl_->decoder ? impl_->decoder->width : 0;
}
int MediaSource::Height() const {
    return impl_->decoder ? impl_->decoder->height : 0;
}
int64_t MediaSource::FrameCount() const { return impl_->frameCount; }
int32_t MediaSource::FrameRateNumerator() const { return impl_->frameRate.num; }
int32_t MediaSource::FrameRateDenominator() const {
    return impl_->frameRate.den;
}
int64_t MediaSource::FrameIndexForPts(int64_t pts) const {
    if (!impl_->stream || pts == AV_NOPTS_VALUE) {
        return -1;
    }
    if (impl_->stream->start_time != AV_NOPTS_VALUE) {
        pts -= impl_->stream->start_time;
    }
    return av_rescale_q_rnd(
        pts, impl_->stream->time_base, av_inv_q(impl_->frameRate),
        static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
}
```

### src/Operations.cc

```cpp
#include "Operations.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

void Fail(EditError code, const std::string& text, EditError& error,
          std::string& message) {
    error = code;
    message = text;
}

ExactClipTimes TimesOf(const DocumentClip& clip) {
    return {clip.source_in, clip.duration, clip.timeline_in};
}

RationalTime PhaseOf(const DocumentClip& clip) {
    return clip.timeline_in.sub(clip.source_in);
}

std::vector<ExactTimelinePosition> PositionsAfter(const DocumentTrack& track,
                                                  size_t first) {
    std::vector<ExactTimelinePosition> positions;
    for (size_t index = first; index < track.clips.size(); ++index) {
        positions.push_back(
            {track.clips[index].id, track.clips[index].timeline_in});
    }
    return positions;
}

bool ApplyExactPositions(Document& document,
                         const std::vector<ExactTimelinePosition>& positions,
                         EditError& error, std::string& message) {
    for (const ExactTimelinePosition& position : positions) {
        DocumentClip* clip = document.FindClip(position.clip_id);
        if (!clip) {
            Fail(EditError::UnknownClip,
                 "exact ripple state references unknown clip_id '" +
                     position.clip_id + "'",
                 error, message);
            return false;
        }
        if (position.timeline_in.rate <= 0 || position.timeline_in.value < 0) {
            Fail(EditError::InvalidTimelineIn,
                 "exact ripple state has invalid timeline_in", error, message);
            return false;
        }
        clip->timeline_in = position.timeline_in;
    }
    return true;
}

bool ValidateResult(const Document& candidate, EditError& error,
                    std::string& message) {
    std::string validation;
    if (candidate.Validate(validation)) return true;
    const EditError code = validation.find("overlap") != std::string::npos
                               ? EditError::Overlap
                               : EditError::ValidationFailed;
    Fail(code, validation, error, message);
    return false;
}

bool ValidateSourceRange(const DocumentSource& source,
                         const RationalTime& sourceIn,
                         const RationalTime& duration, EditError& error,
                         std::string& message) {
    if (sourceIn.rate <= 0 || duration.rate <= 0) {
        Fail(EditError::ArithmeticError, "time rate must be positive", error,
             message);
        return false;
    }
    if (duration.value <= 0) {
        Fail(EditError::InvalidDuration, "duration must be positive", error,
             message);
        return false;
    }
    if (sourceIn.value < 0 || sourceIn.add(duration) > source.duration) {
        Fail(EditError::SourceOutOfBounds,
             "source range is outside source_id '" + source.id + "'", error,
             message);
        return false;
    }
    return true;
}

bool ApplyInsert(Document& candidate, InsertClipOperation& operation,
                 Operation& inverse, EditError& error, std::string& message) {
    DocumentTrack* track = candidate.FindTrack(operation.track_id);
    if (!track) {
        Fail(EditError::UnknownTrack,
             "unknown track_id '" + operation.track_id + "'", error, message);
        return false;
    }
    const DocumentSource* source = candidate.FindSource(operation.source_id);
    if (!source) {
        Fail(EditError::UnknownSource,
             "unknown source_id '" + operation.source_id + "'", error, message);
        return false;
    }
    if (operation.timeline_in.rate <= 0 || operation.timeline_in.value < 0) {
        Fail(EditError::InvalidTimelineIn,
             "timeline_in must be non-negative with a positive rate", error,
             message);
        return false;
    }
    if (!ValidateSourceRange(*source, operation.source_in, operation.duration,
                             error, message)) {
        return false;
    }

    if (operation.clip_id.empty()) operation.clip_id = GenerateUlid();
    if (!IsValidUlid(operation.clip_id) ||
        candidate.FindClip(operation.clip_id) ||
        candidate.FindSource(operation.clip_id) ||
        candidate.FindTrack(operation.clip_id)) {
        Fail(EditError::DuplicateId,
             "insert clip_id is invalid or already exists: '" +
                 operation.clip_id + "'",
             error, message);
        return false;
    }

    auto insertion = std::lower_bound(
        track->clips.begin(), track->clips.end(), operation.timeline_in,
        [](const DocumentClip& clip, const RationalTime& position) {
            return clip.timeline_in < position;
        });
    const size_t insertionIndex =
        static_cast<size_t>(std::distance(track->clips.begin(), insertion));
    if (insertion != track->clips.begin()) {
        const DocumentClip& previous = *std::prev(insertion);
        if (operation.timeline_in <
            previous.timeline_in.add(previous.duration)) {
            Fail(EditError::Overlap,
                 "insertion timeline_in overlaps clip_id '" + previous.id + "'",
                 error, message);
            return false;
        }
    }

    const std::vector<ExactTimelinePosition> before =
        PositionsAfter(*track, insertionIndex);
    for (size_t index = insertionIndex; index < track->clips.size(); ++index) {
        track->clips[index].timeline_in =
            track->clips[index].timeline_in.add(operation.duration);
    }
    track->clips.insert(
        track->clips.begin() + static_cast<std::ptrdiff_t>(insertionIndex),
        DocumentClip{operation.clip_id, operation.source_id,
                     operation.source_in, operation.duration,
                     operation.timeline_in});
    if (!operation.exact_timeline_result.empty() &&
        !ApplyExactPositions(candidate, operation.exact_timeline_result, error,
                             message)) {
        return false;
    }
    if (!ValidateResult(candidate, error, message)) return false;

    if (operation.exact_timeline_result.empty()) {
        DocumentTrack* updated = candidate.FindTrack(operation.track_id);
        operation.exact_timeline_result =
            PositionsAfter(*updated, insertionIndex + 1);
    }
    inverse = RemoveClipOperation{operation.clip_id, before};
    return true;
}

bool ApplyRemove(Document& candidate, RemoveClipOperation& operation,
                 Operation& inverse, EditError& error, std::string& message) {
    DocumentTrack* track = candidate.FindTrackForClip(operation.clip_id);
    if (!track) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    const auto found = std::find_if(
        track->clips.begin(), track->clips.end(),
        [&](const DocumentClip& clip) { return clip.id == operation.clip_id; });
    const size_t index =
        static_cast<size_t>(std::distance(track->clips.begin(), found));
    const DocumentClip removed = *found;
    const Ulid trackId = track->id;
    const std::vector<ExactTimelinePosition> before =
        PositionsAfter(*track, index + 1);
    for (size_t next = index + 1; next < track->clips.size(); ++next) {
        track->clips[next].timeline_in =
            track->clips[next].timeline_in.sub(removed.duration);
    }
    track->clips.erase(track->clips.begin() +
                       static_cast<std::ptrdiff_t>(index));
    if (!operation.exact_timeline_result.empty() &&
        !ApplyExactPositions(candidate, operation.exact_timeline_result, error,
                             message)) {
        return false;
    }
    if (!ValidateResult(candidate, error, message)) return false;
    if (operation.exact_timeline_result.empty()) {
        DocumentTrack* updated = candidate.FindTrack(trackId);
        operation.exact_timeline_result = PositionsAfter(*updated, index);
    }
    inverse = InsertClipOperation{trackId,
                                  removed.source_id,
                                  removed.source_in,
                                  removed.duration,
                                  removed.timeline_in,
                                  removed.id,
                                  before};
    return true;
}

bool Negate(const RationalTime& value, RationalTime& output) {
    if (value.value == std::numeric_limits<int64_t>::min()) return false;
    output = {-value.value, value.rate};
    return true;
}

bool ApplyTrim(Document& candidate, TrimClipOperation& operation,
               Operation& inverse, EditError& error, std::string& message) {
    DocumentClip* clip = candidate.FindClip(operation.clip_id);
    if (!clip) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    if (operation.delta.rate <= 0) {
        Fail(EditError::ArithmeticError, "trim delta rate must be positive",
             error, message);
        return false;
    }
    const DocumentSource* source = candidate.FindSource(clip->source_id);
    if (!source) {
        Fail(EditError::UnknownSource,
             "clip references unknown source_id '" + clip->source_id + "'",
             error, message);
        return false;
    }
    const ExactClipTimes before = TimesOf(*clip);
    if (operation.edge == TrimEdge::Head) {
        clip->source_in = clip->source_in.add(operation.delta);
        clip->duration = clip->duration.sub(operation.delta);
        clip->timeline_in = clip->timeline_in.add(operation.delta);
    } else {
        clip->duration = clip->duration.add(operation.delta);
    }
    if (clip->duration.value <= 0) {
        Fail(EditError::InvalidDuration,
             "trim would make duration zero or negative", error, message);
        return false;
    }
    if (clip->timeline_in.value < 0) {
        Fail(EditError::InvalidTimelineIn,
             "trim would make timeline_in negative", error, message);
        return false;
    }
    if (!ValidateSourceRange(*source, clip->source_in, clip->duration, error,
                             message)) {
        return false;
    }
    if (operation.exact_clip_result) {
        clip->source_in = operation.exact_clip_result->source_in;
        clip->duration = operation.exact_clip_result->duration;
        clip->timeline_in = operation.exact_clip_result->timeline_in;
    }
    if (!ValidateResult(candidate, error, message)) return false;
    if (!operation.exact_clip_result)
        operation.exact_clip_result = TimesOf(*clip);

    RationalTime inverseDelta;
    if (!Negate(operation.delta, inverseDelta)) {
        Fail(EditError::ArithmeticError, "trim delta cannot be negated", error,
             message);
        return false;
    }
    inverse = TrimClipOperation{operation.clip_id, operation.edge, inverseDelta,
                                before};
    return true;
}

bool ApplyMove(Document& candidate, MoveClipOperation& operation,
               Operation& inverse, EditError& error, std::string& message) {
    DocumentTrack* sourceTrack = candidate.FindTrackForClip(operation.clip_id);
    if (!sourceTrack) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    DocumentTrack* targetTrack = candidate.FindTrack(operation.track_id);
    if (!targetTrack) {
        Fail(EditError::UnknownTrack,
             "unknown track_id '" + operation.track_id + "'", error, message);
        return false;
    }
    if (sourceTrack->kind != targetTrack->kind) {
        Fail(EditError::InvalidOperation,
             "cannot move a clip between tracks of different kinds", error,
             message);
        return false;
    }
    if (operation.timeline_in.rate <= 0 || operation.timeline_in.value < 0) {
        Fail(EditError::InvalidTimelineIn,
             "move timeline_in must be non-negative with a positive rate",
             error, message);
        return false;
    }

    const auto snapshotTracks = [&](const std::vector<Ulid>& trackIds) {
        std::vector<ExactTrackState> snapshots;
        for (const Ulid& trackId : trackIds) {
            if (std::any_of(snapshots.begin(), snapshots.end(),
                            [&](const ExactTrackState& state) {
                                return state.track_id == trackId;
                            }))
                continue;
            const DocumentTrack* track = candidate.FindTrack(trackId);
            if (track) snapshots.push_back({track->id, track->clips});
        }
        return snapshots;
    };

    if (!operation.exact_track_result.empty()) {
        const Ulid currentTrackId = sourceTrack->id;
        const RationalTime currentTimelineIn =
            candidate.FindClip(operation.clip_id)->timeline_in;
        std::vector<Ulid> affected;
        for (const ExactTrackState& state : operation.exact_track_result)
            affected.push_back(state.track_id);
        const std::vector<ExactTrackState> before = snapshotTracks(affected);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact move state references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result) {
            candidate.FindTrack(state.track_id)->clips = state.clips;
        }
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = MoveClipOperation{operation.clip_id, currentTrackId,
                                    currentTimelineIn, before};
        return true;
    }

    const Ulid sourceTrackId = sourceTrack->id;
    const std::vector<ExactTrackState> before =
        snapshotTracks({sourceTrackId, operation.track_id});
    const auto found = std::find_if(
        sourceTrack->clips.begin(), sourceTrack->clips.end(),
        [&](const DocumentClip& clip) { return clip.id == operation.clip_id; });
    const DocumentClip original = *found;
    sourceTrack->clips.erase(found);

    // Removing a clip cannot invalidate the tracks vector, but reacquiring the
    // destination avoids retaining a clip-vector-dependent pointer across the
    // erase and keeps same-track moves straightforward.
    targetTrack = candidate.FindTrack(operation.track_id);
    DocumentClip moved = original;
    moved.timeline_in = operation.timeline_in;
    const RationalTime movedEnd = moved.timeline_in.add(moved.duration);
    std::vector<DocumentClip> overwritten;
    overwritten.reserve(targetTrack->clips.size() + 2);
    for (const DocumentClip& existing : targetTrack->clips) {
        const RationalTime existingEnd =
            existing.timeline_in.add(existing.duration);
        if (existingEnd <= moved.timeline_in ||
            existing.timeline_in >= movedEnd) {
            overwritten.push_back(existing);
            continue;
        }

        const bool keepLeft = existing.timeline_in < moved.timeline_in;
        const bool keepRight = existingEnd > movedEnd;
        if (keepLeft) {
            DocumentClip left = existing;
            left.duration = moved.timeline_in.sub(existing.timeline_in);
            overwritten.push_back(std::move(left));
        }
        if (keepRight) {
            DocumentClip right = existing;
            if (keepLeft) {
                do {
                    right.id = GenerateUlid();
                } while (candidate.FindClip(right.id));
            }
            const RationalTime sourceOffset =
                movedEnd.sub(existing.timeline_in);
            right.source_in = existing.source_in.add(sourceOffset);
            right.duration = existingEnd.sub(movedEnd);
            right.timeline_in = movedEnd;
            overwritten.push_back(std::move(right));
        }
    }
    overwritten.push_back(std::move(moved));
    std::stable_sort(overwritten.begin(), overwritten.end(),
                     [](const DocumentClip& left, const DocumentClip& right) {
                         return left.timeline_in < right.timeline_in;
                     });
    targetTrack->clips = std::move(overwritten);
    if (!ValidateResult(candidate, error, message)) return false;

    operation.exact_track_result =
        snapshotTracks({sourceTrackId, operation.track_id});
    inverse = MoveClipOperation{operation.clip_id, sourceTrackId,
                                original.timeline_in, before};
    return true;
}

bool ApplyMoveLinked(Document& candidate, MoveLinkedClipsOperation& operation,
                     Operation& inverse, EditError& error,
                     std::string& message) {
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> result;
        for (const Ulid& id : ids) {
            if (std::any_of(result.begin(), result.end(),
                            [&](const ExactTrackState& state) {
                                return state.track_id == id;
                            }))
                continue;
            const DocumentTrack* track = candidate.FindTrack(id);
            if (track) result.push_back({id, track->clips});
        }
        return result;
    };
    if (!operation.exact_track_result.empty()) {
        std::vector<Ulid> ids;
        for (const ExactTrackState& state : operation.exact_track_result)
            ids.push_back(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(ids);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact linked move references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = MoveLinkedClipsOperation{operation.link_group_id,
                                           operation.moves, before};
        return true;
    }
    if (operation.moves.size() < 2 || operation.link_group_id.empty()) {
        Fail(EditError::InvalidOperation,
             "MoveLinkedClips requires at least two linked members", error,
             message);
        return false;
    }
    std::vector<Ulid> affectedTracks;
    std::vector<LinkedClipMove> inverseMoves;
    std::vector<Ulid> seenClips;
    for (const LinkedClipMove& move : operation.moves) {
        const DocumentClip* clip = candidate.FindClip(move.clip_id);
        const DocumentTrack* source = candidate.FindTrackForClip(move.clip_id);
        const DocumentTrack* target = candidate.FindTrack(move.track_id);
        if (!clip || !source || !target) {
            Fail(!target ? EditError::UnknownTrack : EditError::UnknownClip,
                 "linked move references an unknown clip or track", error,
                 message);
            return false;
        }
        if (clip->link_group_id != operation.link_group_id ||
            std::find(seenClips.begin(), seenClips.end(), clip->id) !=
                seenClips.end()) {
            Fail(EditError::InvalidOperation,
                 "linked move members must be unique and share link_group_id",
                 error, message);
            return false;
        }
        seenClips.push_back(clip->id);
        affectedTracks.push_back(source->id);
        affectedTracks.push_back(target->id);
        inverseMoves.push_back({clip->id, source->id, clip->timeline_in});
    }
    const std::vector<ExactTrackState> before = snapshots(affectedTracks);
    for (const LinkedClipMove& move : operation.moves) {
        MoveClipOperation single{
            move.clip_id, move.track_id, move.timeline_in, {}};
        Operation ignored = RemoveClipOperation{};
        if (!ApplyMove(candidate, single, ignored, error, message))
            return false;
    }
    operation.exact_track_result = snapshots(affectedTracks);
    inverse = MoveLinkedClipsOperation{operation.link_group_id,
                                       std::move(inverseMoves), before};
    return true;
}

bool ApplyTrimLinked(Document& candidate, TrimLinkedClipsOperation& operation,
                     Operation& inverse, EditError& error,
                     std::string& message) {
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> result;
        for (const Ulid& id : ids) {
            if (std::any_of(result.begin(), result.end(),
                            [&](const ExactTrackState& state) {
                                return state.track_id == id;
                            }))
                continue;
            const DocumentTrack* track = candidate.FindTrack(id);
            if (track) result.push_back({id, track->clips});
        }
        return result;
    };
    if (!operation.exact_track_result.empty()) {
        std::vector<Ulid> ids;
        for (const ExactTrackState& state : operation.exact_track_result)
            ids.push_back(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(ids);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact linked trim references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = TrimLinkedClipsOperation{operation.link_group_id,
                                           operation.trims, before};
        return true;
    }
    if (operation.trims.size() < 2 || operation.link_group_id.empty()) {
        Fail(EditError::InvalidOperation,
             "TrimLinkedClips requires at least two linked members", error,
             message);
        return false;
    }
    std::vector<Ulid> trackIds;
    std::vector<Ulid> seen;
    for (const LinkedClipTrim& trim : operation.trims) {
        const DocumentClip* clip = candidate.FindClip(trim.clip_id);
        const DocumentTrack* track = candidate.FindTrackForClip(trim.clip_id);
        if (!clip || !track || clip->link_group_id != operation.link_group_id ||
            std::find(seen.begin(), seen.end(), trim.clip_id) != seen.end()) {
            Fail(EditError::InvalidOperation,
                 "linked trim members must be unique and share link_group_id",
                 error, message);
            return false;
        }
        seen.push_back(trim.clip_id);
        trackIds.push_back(track->id);
    }
    const std::vector<ExactTrackState> before = snapshots(trackIds);
    for (const LinkedClipTrim& trim : operation.trims) {
        TrimClipOperation single{trim.clip_id, trim.edge, trim.delta,
                                 std::nullopt};
        Operation ignored = RemoveClipOperation{};
        if (!ApplyTrim(candidate, single, ignored, error, message))
            return false;
    }
    operation.exact_track_result = snapshots(trackIds);
    inverse = TrimLinkedClipsOperation{operation.link_group_id, operation.trims,
                                       before};
    return true;
}

bool ApplyRemoveLinked(Document& candidate,
                       RemoveLinkedClipsOperation& operation,
                       Operation& inverse, EditError& error,
                       std::string& message) {
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> result;
        for (const Ulid& id : ids) {
            if (std::any_of(result.begin(), result.end(),
                            [&](const ExactTrackState& state) {
                                return state.track_id == id;
                            }))
                continue;
            const DocumentTrack* track = candidate.FindTrack(id);
            if (track) result.push_back({id, track->clips});
        }
        return result;
    };
    if (!operation.exact_track_result.empty()) {
        std::vector<Ulid> ids;
        for (const ExactTrackState& state : operation.exact_track_result)
            ids.push_back(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(ids);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact linked remove references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = RemoveLinkedClipsOperation{operation.link_group_id,
                                             operation.clip_ids, before};
        return true;
    }
    if (operation.clip_ids.size() < 2 || operation.link_group_id.empty()) {
        Fail(EditError::InvalidOperation,
             "RemoveLinkedClips requires at least two linked members", error,
             message);
        return false;
    }
    std::vector<Ulid> trackIds;
    std::vector<Ulid> seen;
    for (const Ulid& id : operation.clip_ids) {
        const DocumentClip* clip = candidate.FindClip(id);
        const DocumentTrack* track = candidate.FindTrackForClip(id);
        if (!clip || !track || clip->link_group_id != operation.link_group_id ||
            std::find(seen.begin(), seen.end(), id) != seen.end()) {
            Fail(EditError::InvalidOperation,
                 "linked remove members must be unique and share link_group_id",
                 error, message);
            return false;
        }
        seen.push_back(id);
        trackIds.push_back(track->id);
    }
    const std::vector<ExactTrackState> before = snapshots(trackIds);
    for (const Ulid& id : operation.clip_ids) {
        RemoveClipOperation single{id, {}};
        Operation ignored = RemoveClipOperation{};
        if (!ApplyRemove(candidate, single, ignored, error, message))
            return false;
    }
    operation.exact_track_result = snapshots(trackIds);
    inverse = RemoveLinkedClipsOperation{operation.link_group_id,
                                         operation.clip_ids, before};
    return true;
}

bool ApplyDeleteGap(Document& candidate, DeleteGapOperation& operation,
                    Operation& inverse, EditError& error,
                    std::string& message) {
    DocumentTrack* track = candidate.FindTrack(operation.track_id);
    if (!track) {
        Fail(EditError::UnknownTrack,
             "unknown track_id '" + operation.track_id + "'", error, message);
        return false;
    }
    const ExactTrackState before{track->id, track->clips};
    if (!operation.exact_track_result.empty()) {
        if (operation.exact_track_result.size() != 1 ||
            operation.exact_track_result[0].track_id != track->id) {
            Fail(EditError::InvalidOperation,
                 "exact gap state must contain its destination track", error,
                 message);
            return false;
        }
        track->clips = operation.exact_track_result[0].clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = DeleteGapOperation{operation.track_id,
                                     operation.gap_start,
                                     operation.gap_duration,
                                     {before}};
        return true;
    }
    if (operation.gap_start.rate <= 0 || operation.gap_start.value < 0 ||
        operation.gap_duration.rate <= 0 || operation.gap_duration.value <= 0) {
        Fail(EditError::InvalidOperation,
             "gap start and duration must describe a positive range", error,
             message);
        return false;
    }
    const RationalTime gapEnd = operation.gap_start.add(operation.gap_duration);
    bool hasFollowingClip = false;
    for (const DocumentClip& clip : track->clips) {
        const RationalTime clipEnd = clip.timeline_in.add(clip.duration);
        if (clip.timeline_in < gapEnd && clipEnd > operation.gap_start) {
            Fail(EditError::InvalidOperation,
                 "DeleteGap range contains clip_id '" + clip.id + "'", error,
                 message);
            return false;
        }
        if (clip.timeline_in >= gapEnd) hasFollowingClip = true;
    }
    if (!hasFollowingClip) {
        Fail(EditError::InvalidOperation,
             "DeleteGap requires a following clip to close the gap", error,
             message);
        return false;
    }
    for (DocumentClip& clip : track->clips) {
        if (clip.timeline_in >= gapEnd)
            clip.timeline_in = clip.timeline_in.sub(operation.gap_duration);
    }
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = {{track->id, track->clips}};
    inverse = DeleteGapOperation{operation.track_id,
                                 operation.gap_start,
                                 operation.gap_duration,
                                 {before}};
    return true;
}

bool ApplyDetachAudio(Document& candidate, DetachAudioOperation& operation,
                      Operation& inverse, EditError& error,
                      std::string& message) {
    DocumentTrack* videoTrack =
        candidate.FindTrackForClip(operation.video_clip_id);
    DocumentTrack* audioTrack = candidate.FindTrack(operation.audio_track_id);
    if (!videoTrack) {
        Fail(EditError::UnknownClip,
             "unknown video_clip_id '" + operation.video_clip_id + "'", error,
             message);
        return false;
    }
    if (!audioTrack) {
        Fail(EditError::UnknownTrack,
             "unknown audio_track_id '" + operation.audio_track_id + "'", error,
             message);
        return false;
    }
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> result;
        for (const Ulid& id : ids) {
            if (std::any_of(result.begin(), result.end(),
                            [&](const ExactTrackState& state) {
                                return state.track_id == id;
                            }))
                continue;
            const DocumentTrack* track = candidate.FindTrack(id);
            if (track) result.push_back({id, track->clips});
        }
        return result;
    };
    if (!operation.exact_track_result.empty()) {
        std::vector<Ulid> ids;
        for (const ExactTrackState& state : operation.exact_track_result)
            ids.push_back(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(ids);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact detach state references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = DetachAudioOperation{operation.video_clip_id,
                                       operation.audio_track_id,
                                       operation.audio_clip_id, before};
        return true;
    }
    if (videoTrack->kind != "video" || audioTrack->kind != "audio") {
        Fail(EditError::InvalidOperation,
             "DetachAudio requires a video clip and an audio track", error,
             message);
        return false;
    }
    DocumentClip* videoClip = candidate.FindClip(operation.video_clip_id);
    if (!videoClip->include_audio) {
        Fail(EditError::InvalidOperation,
             "video clip audio is already detached", error, message);
        return false;
    }
    const LibraryMedia* media =
        candidate.FindLibraryMedia(videoClip->source_id);
    if (media && media->metadata_complete && !media->has_audio) {
        Fail(EditError::InvalidOperation, "source media has no audio stream",
             error, message);
        return false;
    }
    if (operation.audio_clip_id.empty())
        operation.audio_clip_id = GenerateUlid();
    if (!IsValidUlid(operation.audio_clip_id) ||
        candidate.FindClip(operation.audio_clip_id) ||
        candidate.FindTrack(operation.audio_clip_id) ||
        candidate.FindSource(operation.audio_clip_id) ||
        candidate.FindLibraryMedia(operation.audio_clip_id)) {
        Fail(EditError::DuplicateId,
             "audio_clip_id is invalid or already exists", error, message);
        return false;
    }
    const std::vector<ExactTrackState> before =
        snapshots({videoTrack->id, audioTrack->id});
    const RationalTime detachedEnd =
        videoClip->timeline_in.add(videoClip->duration);
    for (const DocumentClip& existing : audioTrack->clips) {
        if (existing.timeline_in < detachedEnd &&
            existing.timeline_in.add(existing.duration) >
                videoClip->timeline_in) {
            Fail(EditError::Overlap,
                 "detached audio would overlap clip_id '" + existing.id + "'",
                 error, message);
            return false;
        }
    }
    DocumentClip audioClip = *videoClip;
    audioClip.id = operation.audio_clip_id;
    videoClip->include_audio = false;
    videoClip->link_group_id = operation.audio_clip_id;
    videoClip->sync_anchor_clip_id = videoClip->id;
    videoClip->sync_reference_delta = {0, 1};
    audioClip.link_group_id = operation.audio_clip_id;
    audioClip.sync_anchor_clip_id = videoClip->id;
    audioClip.sync_reference_delta = {0, 1};
    audioTrack->clips.push_back(std::move(audioClip));
    std::stable_sort(audioTrack->clips.begin(), audioTrack->clips.end(),
                     [](const DocumentClip& left, const DocumentClip& right) {
                         return left.timeline_in < right.timeline_in;
                     });
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = snapshots({videoTrack->id, audioTrack->id});
    inverse =
        DetachAudioOperation{operation.video_clip_id, operation.audio_track_id,
                             operation.audio_clip_id, before};
    return true;
}

bool ApplyAddTrack(Document& candidate, AddTrackOperation& operation,
                   Operation& inverse, EditError& error, std::string& message) {
    if (operation.track_id.empty()) operation.track_id = GenerateUlid();
    if (!IsValidUlid(operation.track_id) ||
        candidate.FindTrack(operation.track_id) ||
        candidate.FindClip(operation.track_id) ||
        candidate.FindSource(operation.track_id) ||
        candidate.FindLibraryMedia(operation.track_id)) {
        Fail(EditError::DuplicateId,
             "track_id is invalid or already exists: '" + operation.track_id +
                 "'",
             error, message);
        return false;
    }
    if (operation.kind != "video" && operation.kind != "audio") {
        Fail(EditError::InvalidOperation,
             "track kind must be 'video' or 'audio'", error, message);
        return false;
    }
    if (operation.index < 0 ||
        std::any_of(candidate.tracks.begin(), candidate.tracks.end(),
                    [&](const DocumentTrack& track) {
                        return track.index == operation.index;
                    })) {
        Fail(EditError::InvalidOperation,
             "track index must be non-negative and unique", error, message);
        return false;
    }
    candidate.tracks.push_back(
        {operation.track_id, operation.kind, operation.index, {}});
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = RemoveTrackOperation{operation.track_id};
    return true;
}

bool ApplyRemoveTrack(Document& candidate, RemoveTrackOperation& operation,
                      Operation& inverse, EditError& error,
                      std::string& message) {
    const auto found =
        std::find_if(candidate.tracks.begin(), candidate.tracks.end(),
                     [&](const DocumentTrack& track) {
                         return track.id == operation.track_id;
                     });
    if (found == candidate.tracks.end()) {
        Fail(EditError::UnknownTrack,
             "unknown track_id '" + operation.track_id + "'", error, message);
        return false;
    }
    if (!found->clips.empty()) {
        Fail(EditError::InvalidOperation, "cannot remove a non-empty track",
             error, message);
        return false;
    }
    inverse = AddTrackOperation{found->id, found->kind, found->index};
    candidate.tracks.erase(found);
    return ValidateResult(candidate, error, message);
}

bool ApplyAddBin(Document& candidate, AddBinOperation& operation,
                 Operation& inverse, EditError& error, std::string& message) {
    if (operation.bin_id.empty()) operation.bin_id = GenerateUlid();
    if (!IsValidUlid(operation.bin_id) || candidate.FindBin(operation.bin_id) ||
        candidate.FindTrack(operation.bin_id) ||
        candidate.FindClip(operation.bin_id) ||
        candidate.FindSource(operation.bin_id) ||
        candidate.FindLibraryMedia(operation.bin_id)) {
        Fail(EditError::DuplicateId,
             "bin_id is invalid or already exists: '" + operation.bin_id + "'",
             error, message);
        return false;
    }
    if (operation.name.empty() || operation.name.size() > 128 ||
        std::any_of(operation.name.begin(), operation.name.end(),
                    [](unsigned char character) { return character < 0x20; })) {
        Fail(EditError::InvalidOperation,
             "bin name must contain between 1 and 128 bytes", error, message);
        return false;
    }
    if (!operation.parent_id.empty() &&
        !candidate.FindBin(operation.parent_id)) {
        Fail(EditError::UnknownBin,
             "unknown parent bin_id '" + operation.parent_id + "'", error,
             message);
        return false;
    }
    candidate.bins.push_back(
        {operation.bin_id, operation.name, operation.parent_id});
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = RemoveBinOperation{operation.bin_id, operation.name,
                                 operation.parent_id};
    return true;
}

bool ApplyRemoveBin(Document& candidate, RemoveBinOperation& operation,
                    Operation& inverse, EditError& error,
                    std::string& message) {
    const auto found = std::find_if(
        candidate.bins.begin(), candidate.bins.end(),
        [&](const DocumentBin& bin) { return bin.id == operation.bin_id; });
    if (found == candidate.bins.end()) {
        Fail(EditError::UnknownBin, "unknown bin_id '" + operation.bin_id + "'",
             error, message);
        return false;
    }
    if (std::any_of(candidate.library.begin(), candidate.library.end(),
                    [&](const LibraryMedia& media) {
                        return media.bin_id == operation.bin_id;
                    })) {
        Fail(EditError::InvalidOperation,
             "cannot remove a bin that still contains media", error, message);
        return false;
    }
    if (std::any_of(candidate.bins.begin(), candidate.bins.end(),
                    [&](const DocumentBin& bin) {
                        return bin.parent_id == operation.bin_id;
                    })) {
        Fail(EditError::InvalidOperation,
             "cannot remove a bin that still contains child bins", error,
             message);
        return false;
    }
    operation.name = found->name;
    operation.parent_id = found->parent_id;
    inverse = AddBinOperation{found->id, found->name, found->parent_id};
    candidate.bins.erase(found);
    return ValidateResult(candidate, error, message);
}

bool ApplyRenameBin(Document& candidate, RenameBinOperation& operation,
                    Operation& inverse, EditError& error,
                    std::string& message) {
    DocumentBin* bin = candidate.FindBin(operation.bin_id);
    if (!bin) {
        Fail(EditError::UnknownBin, "unknown bin_id '" + operation.bin_id + "'",
             error, message);
        return false;
    }
    if (operation.name.empty() || operation.name.size() > 128 ||
        std::any_of(operation.name.begin(), operation.name.end(),
                    [](unsigned char character) { return character < 0x20; })) {
        Fail(EditError::InvalidOperation,
             "bin name must contain between 1 and 128 bytes", error, message);
        return false;
    }
    inverse = RenameBinOperation{bin->id, bin->name};
    bin->name = operation.name;
    return ValidateResult(candidate, error, message);
}

bool ApplySetMediaBin(Document& candidate, SetMediaBinOperation& operation,
                      Operation& inverse, EditError& error,
                      std::string& message) {
    LibraryMedia* media = candidate.FindLibraryMedia(operation.media_id);
    if (!media) {
        Fail(EditError::UnknownMedia,
             "unknown media_id '" + operation.media_id + "'", error, message);
        return false;
    }
    if (!operation.bin_id.empty() && !candidate.FindBin(operation.bin_id)) {
        Fail(EditError::UnknownBin, "unknown bin_id '" + operation.bin_id + "'",
             error, message);
        return false;
    }
    inverse = SetMediaBinOperation{operation.media_id, media->bin_id};
    media->bin_id = operation.bin_id;
    return ValidateResult(candidate, error, message);
}

bool ApplySetClipLink(Document& candidate, SetClipLinkOperation& operation,
                      Operation& inverse, EditError& error,
                      std::string& message) {
    DocumentClip* first = candidate.FindClip(operation.first_clip_id);
    DocumentClip* second = candidate.FindClip(operation.second_clip_id);
    if (!first || !second || first == second) {
        Fail(EditError::UnknownClip,
             "SetClipLink requires two distinct existing clips", error,
             message);
        return false;
    }
    if ((!operation.first_group_id.empty() &&
         !IsValidUlid(operation.first_group_id)) ||
        (!operation.second_group_id.empty() &&
         !IsValidUlid(operation.second_group_id)) ||
        (operation.exact_first_result.has_value() !=
         operation.exact_second_result.has_value())) {
        Fail(EditError::InvalidOperation,
             "link group IDs must be empty or valid ULIDs", error, message);
        return false;
    }
    const SetClipLinkOperation::ExactState oldFirst{
        first->link_group_id, first->sync_anchor_clip_id,
        first->sync_reference_delta};
    const SetClipLinkOperation::ExactState oldSecond{
        second->link_group_id, second->sync_anchor_clip_id,
        second->sync_reference_delta};
    inverse =
        SetClipLinkOperation{first->id,          second->id, oldFirst.group_id,
                             oldSecond.group_id, oldFirst,   oldSecond};
    if (operation.exact_first_result) {
        const auto apply = [](DocumentClip& clip,
                              const SetClipLinkOperation::ExactState& state) {
            clip.link_group_id = state.group_id;
            clip.sync_anchor_clip_id = state.anchor_clip_id;
            clip.sync_reference_delta = state.reference_delta;
        };
        apply(*first, *operation.exact_first_result);
        apply(*second, *operation.exact_second_result);
    } else if (operation.first_group_id.empty() &&
               operation.second_group_id.empty()) {
        first->link_group_id.clear();
        first->sync_anchor_clip_id.clear();
        first->sync_reference_delta = {0, 1};
        second->link_group_id.clear();
        second->sync_anchor_clip_id.clear();
        second->sync_reference_delta = {0, 1};
    } else {
        if (operation.first_group_id != operation.second_group_id) {
            Fail(EditError::InvalidOperation,
                 "newly linked clips must share one group ID", error, message);
            return false;
        }
        const DocumentTrack* firstTrack = candidate.FindTrackForClip(first->id);
        DocumentClip* anchor =
            firstTrack && firstTrack->kind == "video" ? first : second;
        DocumentClip* member = anchor == first ? second : first;
        anchor->link_group_id = operation.first_group_id;
        anchor->sync_anchor_clip_id = anchor->id;
        anchor->sync_reference_delta = {0, 1};
        member->link_group_id = operation.first_group_id;
        member->sync_anchor_clip_id = anchor->id;
        member->sync_reference_delta = PhaseOf(*member).sub(PhaseOf(*anchor));
    }
    operation.exact_first_result = SetClipLinkOperation::ExactState{
        first->link_group_id, first->sync_anchor_clip_id,
        first->sync_reference_delta};
    operation.exact_second_result = SetClipLinkOperation::ExactState{
        second->link_group_id, second->sync_anchor_clip_id,
        second->sync_reference_delta};
    return ValidateResult(candidate, error, message);
}

bool ApplySplit(Document& candidate, SplitClipOperation& operation,
                Operation& inverse, EditError& error, std::string& message) {
    DocumentTrack* track = candidate.FindTrackForClip(operation.clip_id);
    if (!track) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    const auto found = std::find_if(
        track->clips.begin(), track->clips.end(),
        [&](const DocumentClip& clip) { return clip.id == operation.clip_id; });
    const size_t index =
        static_cast<size_t>(std::distance(track->clips.begin(), found));
    const DocumentClip original = *found;
    const RationalTime end = original.timeline_in.add(original.duration);
    if (operation.timeline_position.rate <= 0 ||
        operation.timeline_position <= original.timeline_in ||
        operation.timeline_position >= end) {
        Fail(EditError::InvalidTimelineIn,
             "split position must be strictly inside the clip", error, message);
        return false;
    }
    if (operation.right_clip_id.empty())
        operation.right_clip_id = GenerateUlid();
    if (!IsValidUlid(operation.right_clip_id) ||
        candidate.FindClip(operation.right_clip_id) ||
        candidate.FindSource(operation.right_clip_id) ||
        candidate.FindTrack(operation.right_clip_id)) {
        Fail(EditError::DuplicateId,
             "split right_clip_id is invalid or already exists: '" +
                 operation.right_clip_id + "'",
             error, message);
        return false;
    }

    const RationalTime leftDuration =
        operation.timeline_position.sub(original.timeline_in);
    DocumentClip left = original;
    left.duration = leftDuration;
    DocumentClip right{operation.right_clip_id, original.source_id,
                       original.source_in.add(leftDuration),
                       original.duration.sub(leftDuration),
                       operation.timeline_position};
    right.include_audio = original.include_audio;
    right.link_group_id = original.link_group_id;
    right.sync_anchor_clip_id = original.sync_anchor_clip_id;
    right.sync_reference_delta = original.sync_reference_delta;
    track->clips[index] = std::move(left);
    track->clips.insert(
        track->clips.begin() + static_cast<std::ptrdiff_t>(index + 1),
        std::move(right));
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = JoinClipOperation{original.id, operation.right_clip_id,
                                TimesOf(original)};
    return true;
}

bool ApplyJoin(Document& candidate, JoinClipOperation& operation,
               Operation& inverse, EditError& error, std::string& message) {
    DocumentTrack* track = candidate.FindTrackForClip(operation.left_clip_id);
    if (!track) {
        Fail(EditError::UnknownClip,
             "unknown left_clip_id '" + operation.left_clip_id + "'", error,
             message);
        return false;
    }
    const auto left = std::find_if(track->clips.begin(), track->clips.end(),
                                   [&](const DocumentClip& clip) {
                                       return clip.id == operation.left_clip_id;
                                   });
    if (left == track->clips.end() || std::next(left) == track->clips.end() ||
        std::next(left)->id != operation.right_clip_id) {
        Fail(EditError::InvalidOperation,
             "join clips must be adjacent on the same track", error, message);
        return false;
    }
    const DocumentClip right = *std::next(left);
    if (left->source_id != right.source_id ||
        left->timeline_in.add(left->duration) != right.timeline_in ||
        left->source_in.add(left->duration) != right.source_in) {
        Fail(EditError::InvalidOperation,
             "join clips are not contiguous pieces of one source", error,
             message);
        return false;
    }
    const RationalTime splitPosition = right.timeline_in;
    left->source_in = operation.joined_times.source_in;
    left->duration = operation.joined_times.duration;
    left->timeline_in = operation.joined_times.timeline_in;
    track->clips.erase(std::next(left));
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = SplitClipOperation{operation.left_clip_id, splitPosition,
                                 operation.right_clip_id};
    return true;
}

void WriteTime(std::ostringstream& output, const RationalTime& time) {
    output << "{\"value\":" << time.value << ",\"rate\":" << time.rate << "}";
}

void WriteString(std::ostringstream& output, const std::string& value) {
    output << '"';
    for (const char character : value) {
        if (character == '"' || character == '\\') output << '\\';
        output << character;
    }
    output << '"';
}

void WriteExactPositions(std::ostringstream& output,
                         const std::vector<ExactTimelinePosition>& positions) {
    output << '[';
    for (size_t index = 0; index < positions.size(); ++index) {
        if (index) output << ',';
        output << "{\"clip_id\":\"" << positions[index].clip_id
               << "\",\"timeline_in\":";
        WriteTime(output, positions[index].timeline_in);
        output << '}';
    }
    output << ']';
}

void WriteExactTracks(std::ostringstream& output,
                      const std::vector<ExactTrackState>& tracks) {
    output << '[';
    for (size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        if (trackIndex) output << ',';
        output << "{\"track_id\":\"" << tracks[trackIndex].track_id
               << "\",\"clips\":[";
        for (size_t clipIndex = 0; clipIndex < tracks[trackIndex].clips.size();
             ++clipIndex) {
            if (clipIndex) output << ',';
            const DocumentClip& clip = tracks[trackIndex].clips[clipIndex];
            output << "{\"id\":\"" << clip.id << "\",\"source_id\":\""
                   << clip.source_id << "\",\"source_in\":";
            WriteTime(output, clip.source_in);
            output << ",\"duration\":";
            WriteTime(output, clip.duration);
            output << ",\"timeline_in\":";
            WriteTime(output, clip.timeline_in);
            output << ",\"include_audio\":"
                   << (clip.include_audio ? "true" : "false");
            if (!clip.link_group_id.empty())
                output << ",\"link_group_id\":\"" << clip.link_group_id << "\"";
            if (!clip.sync_anchor_clip_id.empty()) {
                output << ",\"sync_anchor_clip_id\":\""
                       << clip.sync_anchor_clip_id
                       << "\",\"sync_reference_delta\":";
                WriteTime(output, clip.sync_reference_delta);
            }
            output << '}';
        }
        output << "]}";
    }
    output << ']';
}

class Reader {
public:
    explicit Reader(const std::string& input) : input_(input) {}

    void Expect(const std::string& text) {
        Skip();
        if (input_.compare(position_, text.size(), text) != 0) {
            throw std::runtime_error("expected '" + text + "' at byte " +
                                     std::to_string(position_));
        }
        position_ += text.size();
    }

    bool Consume(const std::string& text) {
        Skip();
        if (input_.compare(position_, text.size(), text) != 0) return false;
        position_ += text.size();
        return true;
    }

    std::string String() {
        Skip();
        if (position_ >= input_.size() || input_[position_++] != '"') {
            throw std::runtime_error("expected string at byte " +
                                     std::to_string(position_));
        }
        std::string output;
        while (position_ < input_.size()) {
            const char character = input_[position_++];
            if (character == '"') return output;
            if (character == '\\') {
                if (position_ >= input_.size())
                    throw std::runtime_error("unterminated string escape");
                const char escaped = input_[position_++];
                if (escaped != '"' && escaped != '\\')
                    throw std::runtime_error("unsupported string escape");
                output.push_back(escaped);
            } else {
                output.push_back(character);
            }
        }
        throw std::runtime_error("unterminated string");
    }

    int64_t Integer() {
        Skip();
        const size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        while (position_ < input_.size() &&
               std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
        if (position_ == start ||
            (position_ == start + 1 && input_[start] == '-')) {
            throw std::runtime_error("expected integer at byte " +
                                     std::to_string(start));
        }
        char* end = nullptr;
        const std::string text = input_.substr(start, position_ - start);
        const long long value = std::strtoll(text.c_str(), &end, 10);
        if (!end || *end != '\0') throw std::runtime_error("invalid integer");
        return static_cast<int64_t>(value);
    }

    void Finish() {
        Skip();
        if (position_ != input_.size())
            throw std::runtime_error("unexpected trailing operation JSON");
    }

private:
    void Skip() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }
    const std::string& input_;
    size_t position_ = 0;
};

RationalTime ReadTime(Reader& reader) {
    reader.Expect("{\"value\":");
    const int64_t value = reader.Integer();
    reader.Expect(",\"rate\":");
    const int64_t rate = reader.Integer();
    reader.Expect("}");
    if (rate < std::numeric_limits<int32_t>::min() ||
        rate > std::numeric_limits<int32_t>::max()) {
        throw std::runtime_error("RationalTime rate outside int32_t range");
    }
    return {value, static_cast<int32_t>(rate)};
}

std::vector<ExactTimelinePosition> ReadExactPositions(Reader& reader) {
    std::vector<ExactTimelinePosition> positions;
    reader.Expect("[");
    if (reader.Consume("]")) return positions;
    while (true) {
        reader.Expect("{\"clip_id\":");
        const Ulid id = reader.String();
        reader.Expect(",\"timeline_in\":");
        const RationalTime timelineIn = ReadTime(reader);
        reader.Expect("}");
        positions.push_back({id, timelineIn});
        if (reader.Consume("]")) return positions;
        reader.Expect(",");
    }
}

std::vector<ExactTrackState> ReadExactTracks(Reader& reader) {
    std::vector<ExactTrackState> tracks;
    reader.Expect("[");
    if (reader.Consume("]")) return tracks;
    while (true) {
        ExactTrackState state;
        reader.Expect("{\"track_id\":");
        state.track_id = reader.String();
        reader.Expect(",\"clips\":[");
        if (!reader.Consume("]")) {
            while (true) {
                DocumentClip clip;
                reader.Expect("{\"id\":");
                clip.id = reader.String();
                reader.Expect(",\"source_id\":");
                clip.source_id = reader.String();
                reader.Expect(",\"source_in\":");
                clip.source_in = ReadTime(reader);
                reader.Expect(",\"duration\":");
                clip.duration = ReadTime(reader);
                reader.Expect(",\"timeline_in\":");
                clip.timeline_in = ReadTime(reader);
                if (reader.Consume(",\"include_audio\":false"))
                    clip.include_audio = false;
                else
                    reader.Consume(",\"include_audio\":true");
                if (reader.Consume(",\"link_group_id\":"))
                    clip.link_group_id = reader.String();
                if (reader.Consume(",\"sync_anchor_clip_id\":")) {
                    clip.sync_anchor_clip_id = reader.String();
                    reader.Expect(",\"sync_reference_delta\":");
                    clip.sync_reference_delta = ReadTime(reader);
                }
                reader.Expect("}");
                state.clips.push_back(std::move(clip));
                if (reader.Consume("]")) break;
                reader.Expect(",");
            }
        }
        reader.Expect("}");
        tracks.push_back(std::move(state));
        if (reader.Consume("]")) return tracks;
        reader.Expect(",");
    }
}

}  // namespace

const char* EditErrorName(EditError error) {
    switch (error) {
        case EditError::None:
            return "None";
        case EditError::UnknownTrack:
            return "UnknownTrack";
        case EditError::UnknownClip:
            return "UnknownClip";
        case EditError::UnknownSource:
            return "UnknownSource";
        case EditError::UnknownBin:
            return "UnknownBin";
        case EditError::UnknownMedia:
            return "UnknownMedia";
        case EditError::InvalidDuration:
            return "InvalidDuration";
        case EditError::InvalidTimelineIn:
            return "InvalidTimelineIn";
        case EditError::SourceOutOfBounds:
            return "SourceOutOfBounds";
        case EditError::Overlap:
            return "Overlap";
        case EditError::DuplicateId:
            return "DuplicateId";
        case EditError::ArithmeticError:
            return "ArithmeticError";
        case EditError::InvalidOperation:
            return "InvalidOperation";
        case EditError::ValidationFailed:
            return "ValidationFailed";
        case EditError::EmptyUndo:
            return "EmptyUndo";
        case EditError::EmptyRedo:
            return "EmptyRedo";
        case EditError::IoError:
            return "IoError";
        case EditError::ParseError:
            return "ParseError";
    }
    return "InvalidOperation";
}

bool ApplyOperation(Document& document, Operation& operation,
                    Operation& inverse, EditError& error,
                    std::string& message) {
    Document candidate = document;
    Operation normalized = operation;
    Operation generatedInverse = RemoveClipOperation{};
    try {
        bool applied = false;
        if (auto* insert = std::get_if<InsertClipOperation>(&normalized)) {
            applied = ApplyInsert(candidate, *insert, generatedInverse, error,
                                  message);
        } else if (auto* remove =
                       std::get_if<RemoveClipOperation>(&normalized)) {
            applied = ApplyRemove(candidate, *remove, generatedInverse, error,
                                  message);
        } else if (auto* trim = std::get_if<TrimClipOperation>(&normalized)) {
            applied =
                ApplyTrim(candidate, *trim, generatedInverse, error, message);
        } else if (auto* move = std::get_if<MoveClipOperation>(&normalized)) {
            applied =
                ApplyMove(candidate, *move, generatedInverse, error, message);
        } else if (auto* linkedMove =
                       std::get_if<MoveLinkedClipsOperation>(&normalized)) {
            applied = ApplyMoveLinked(candidate, *linkedMove, generatedInverse,
                                      error, message);
        } else if (auto* linkedTrim =
                       std::get_if<TrimLinkedClipsOperation>(&normalized)) {
            applied = ApplyTrimLinked(candidate, *linkedTrim, generatedInverse,
                                      error, message);
        } else if (auto* linkedRemove =
                       std::get_if<RemoveLinkedClipsOperation>(&normalized)) {
            applied = ApplyRemoveLinked(candidate, *linkedRemove,
                                        generatedInverse, error, message);
        } else if (auto* split = std::get_if<SplitClipOperation>(&normalized)) {
            applied =
                ApplySplit(candidate, *split, generatedInverse, error, message);
        } else if (auto* gap = std::get_if<DeleteGapOperation>(&normalized)) {
            applied = ApplyDeleteGap(candidate, *gap, generatedInverse, error,
                                     message);
        } else if (auto* detach =
                       std::get_if<DetachAudioOperation>(&normalized)) {
            applied = ApplyDetachAudio(candidate, *detach, generatedInverse,
                                       error, message);
        } else if (auto* addTrack =
                       std::get_if<AddTrackOperation>(&normalized)) {
            applied = ApplyAddTrack(candidate, *addTrack, generatedInverse,
                                    error, message);
        } else if (auto* removeTrack =
                       std::get_if<RemoveTrackOperation>(&normalized)) {
            applied = ApplyRemoveTrack(candidate, *removeTrack,
                                       generatedInverse, error, message);
        } else if (auto* addBin = std::get_if<AddBinOperation>(&normalized)) {
            applied = ApplyAddBin(candidate, *addBin, generatedInverse, error,
                                  message);
        } else if (auto* removeBin =
                       std::get_if<RemoveBinOperation>(&normalized)) {
            applied = ApplyRemoveBin(candidate, *removeBin, generatedInverse,
                                     error, message);
        } else if (auto* renameBin =
                       std::get_if<RenameBinOperation>(&normalized)) {
            applied = ApplyRenameBin(candidate, *renameBin, generatedInverse,
                                     error, message);
        } else if (auto* setMediaBin =
                       std::get_if<SetMediaBinOperation>(&normalized)) {
            applied = ApplySetMediaBin(candidate, *setMediaBin,
                                       generatedInverse, error, message);
        } else if (auto* setClipLink =
                       std::get_if<SetClipLinkOperation>(&normalized)) {
            applied = ApplySetClipLink(candidate, *setClipLink,
                                       generatedInverse, error, message);
        } else if (auto* join = std::get_if<JoinClipOperation>(&normalized)) {
            applied =
                ApplyJoin(candidate, *join, generatedInverse, error, message);
        }
        if (!applied) return false;
    } catch (const std::exception& exception) {
        Fail(EditError::ArithmeticError, exception.what(), error, message);
        return false;
    }
    document = std::move(candidate);
    operation = std::move(normalized);
    inverse = std::move(generatedInverse);
    error = EditError::None;
    message.clear();
    return true;
}

std::string SerializeOperation(const Operation& operation) {
    std::ostringstream output;
    if (const auto* insert = std::get_if<InsertClipOperation>(&operation)) {
        output << "{\"type\":\"InsertClip\",\"track_id\":\"" << insert->track_id
               << "\",\"source_id\":\"" << insert->source_id
               << "\",\"source_in\":";
        WriteTime(output, insert->source_in);
        output << ",\"duration\":";
        WriteTime(output, insert->duration);
        output << ",\"timeline_in\":";
        WriteTime(output, insert->timeline_in);
        output << ",\"clip_id\":\"" << insert->clip_id
               << "\",\"exact_timeline\":";
        WriteExactPositions(output, insert->exact_timeline_result);
        output << '}';
    } else if (const auto* remove =
                   std::get_if<RemoveClipOperation>(&operation)) {
        output << "{\"type\":\"RemoveClip\",\"clip_id\":\"" << remove->clip_id
               << "\",\"exact_timeline\":";
        WriteExactPositions(output, remove->exact_timeline_result);
        output << '}';
    } else if (const auto* trim = std::get_if<TrimClipOperation>(&operation)) {
        output << "{\"type\":\"TrimClip\",\"clip_id\":\"" << trim->clip_id
               << "\",\"edge\":\""
               << (trim->edge == TrimEdge::Head ? "Head" : "Tail")
               << "\",\"delta\":";
        WriteTime(output, trim->delta);
        output << ",\"exact_clip\":";
        if (!trim->exact_clip_result) {
            output << "null";
        } else {
            output << "{\"source_in\":";
            WriteTime(output, trim->exact_clip_result->source_in);
            output << ",\"duration\":";
            WriteTime(output, trim->exact_clip_result->duration);
            output << ",\"timeline_in\":";
            WriteTime(output, trim->exact_clip_result->timeline_in);
            output << '}';
        }
        output << '}';
    } else if (const auto* move = std::get_if<MoveClipOperation>(&operation)) {
        output << "{\"type\":\"MoveClip\",\"clip_id\":\"" << move->clip_id
               << "\",\"track_id\":\"" << move->track_id
               << "\",\"timeline_in\":";
        WriteTime(output, move->timeline_in);
        output << ",\"exact_tracks\":";
        WriteExactTracks(output, move->exact_track_result);
        output << '}';
    } else if (const auto* linkedMove =
                   std::get_if<MoveLinkedClipsOperation>(&operation)) {
        output << "{\"type\":\"MoveLinkedClips\",\"link_group_id\":\""
               << linkedMove->link_group_id << "\",\"moves\":[";
        for (size_t index = 0; index < linkedMove->moves.size(); ++index) {
            if (index) output << ',';
            const LinkedClipMove& move = linkedMove->moves[index];
            output << "{\"clip_id\":\"" << move.clip_id << "\",\"track_id\":\""
                   << move.track_id << "\",\"timeline_in\":";
            WriteTime(output, move.timeline_in);
            output << '}';
        }
        output << "],\"exact_tracks\":";
        WriteExactTracks(output, linkedMove->exact_track_result);
        output << '}';
    } else if (const auto* linkedTrim =
                   std::get_if<TrimLinkedClipsOperation>(&operation)) {
        output << "{\"type\":\"TrimLinkedClips\",\"link_group_id\":\""
               << linkedTrim->link_group_id << "\",\"trims\":[";
        for (size_t index = 0; index < linkedTrim->trims.size(); ++index) {
            if (index) output << ',';
            const LinkedClipTrim& trim = linkedTrim->trims[index];
            output << "{\"clip_id\":\"" << trim.clip_id << "\",\"edge\":\""
                   << (trim.edge == TrimEdge::Head ? "Head" : "Tail")
                   << "\",\"delta\":";
            WriteTime(output, trim.delta);
            output << '}';
        }
        output << "],\"exact_tracks\":";
        WriteExactTracks(output, linkedTrim->exact_track_result);
        output << '}';
    } else if (const auto* linkedRemove =
                   std::get_if<RemoveLinkedClipsOperation>(&operation)) {
        output << "{\"type\":\"RemoveLinkedClips\",\"link_group_id\":\""
               << linkedRemove->link_group_id << "\",\"clip_ids\":[";
        for (size_t index = 0; index < linkedRemove->clip_ids.size(); ++index) {
            if (index) output << ',';
            WriteString(output, linkedRemove->clip_ids[index]);
        }
        output << "],\"exact_tracks\":";
        WriteExactTracks(output, linkedRemove->exact_track_result);
        output << '}';
    } else if (const auto* split =
                   std::get_if<SplitClipOperation>(&operation)) {
        output << "{\"type\":\"SplitClip\",\"clip_id\":\"" << split->clip_id
               << "\",\"timeline_position\":";
        WriteTime(output, split->timeline_position);
        output << ",\"right_clip_id\":\"" << split->right_clip_id << "\"}";
    } else if (const auto* gap = std::get_if<DeleteGapOperation>(&operation)) {
        output << "{\"type\":\"DeleteGap\",\"track_id\":\"" << gap->track_id
               << "\",\"gap_start\":";
        WriteTime(output, gap->gap_start);
        output << ",\"gap_duration\":";
        WriteTime(output, gap->gap_duration);
        output << ",\"exact_tracks\":";
        WriteExactTracks(output, gap->exact_track_result);
        output << '}';
    } else if (const auto* detach =
                   std::get_if<DetachAudioOperation>(&operation)) {
        output << "{\"type\":\"DetachAudio\",\"video_clip_id\":\""
               << detach->video_clip_id << "\",\"audio_track_id\":\""
               << detach->audio_track_id << "\",\"audio_clip_id\":\""
               << detach->audio_clip_id << "\",\"exact_tracks\":";
        WriteExactTracks(output, detach->exact_track_result);
        output << '}';
    } else if (const auto* addTrack =
                   std::get_if<AddTrackOperation>(&operation)) {
        output << "{\"type\":\"AddTrack\",\"track_id\":\"" << addTrack->track_id
               << "\",\"kind\":\"" << addTrack->kind
               << "\",\"index\":" << addTrack->index << '}';
    } else if (const auto* removeTrack =
                   std::get_if<RemoveTrackOperation>(&operation)) {
        output << "{\"type\":\"RemoveTrack\",\"track_id\":\""
               << removeTrack->track_id << "\"}";
    } else if (const auto* addBin = std::get_if<AddBinOperation>(&operation)) {
        output << "{\"type\":\"AddBin\",\"bin_id\":\"" << addBin->bin_id
               << "\",\"name\":";
        WriteString(output, addBin->name);
        output << ",\"parent_id\":\"" << addBin->parent_id << "\"";
        output << '}';
    } else if (const auto* removeBin =
                   std::get_if<RemoveBinOperation>(&operation)) {
        output << "{\"type\":\"RemoveBin\",\"bin_id\":\"" << removeBin->bin_id
               << "\",\"name\":";
        WriteString(output, removeBin->name);
        output << ",\"parent_id\":\"" << removeBin->parent_id << "\"";
        output << '}';
    } else if (const auto* renameBin =
                   std::get_if<RenameBinOperation>(&operation)) {
        output << "{\"type\":\"RenameBin\",\"bin_id\":\"" << renameBin->bin_id
               << "\",\"name\":";
        WriteString(output, renameBin->name);
        output << '}';
    } else if (const auto* setMediaBin =
                   std::get_if<SetMediaBinOperation>(&operation)) {
        output << "{\"type\":\"SetMediaBin\",\"media_id\":\""
               << setMediaBin->media_id << "\",\"bin_id\":\""
               << setMediaBin->bin_id << "\"}";
    } else if (const auto* setClipLink =
                   std::get_if<SetClipLinkOperation>(&operation)) {
        output << "{\"type\":\"SetClipLink\",\"first_clip_id\":\""
               << setClipLink->first_clip_id << "\",\"second_clip_id\":\""
               << setClipLink->second_clip_id << "\",\"first_group_id\":\""
               << setClipLink->first_group_id << "\",\"second_group_id\":\""
               << setClipLink->second_group_id << "\",\"exact_first\":";
        const auto writeState = [&](const auto& state) {
            if (!state) {
                output << "null";
                return;
            }
            output << "{\"group_id\":\"" << state->group_id
                   << "\",\"anchor_clip_id\":\"" << state->anchor_clip_id
                   << "\",\"reference_delta\":";
            WriteTime(output, state->reference_delta);
            output << '}';
        };
        writeState(setClipLink->exact_first_result);
        output << ",\"exact_second\":";
        writeState(setClipLink->exact_second_result);
        output << '}';
    } else {
        const auto& join = std::get<JoinClipOperation>(operation);
        output << "{\"type\":\"JoinClip\",\"left_clip_id\":\""
               << join.left_clip_id << "\",\"right_clip_id\":\""
               << join.right_clip_id << "\",\"joined_times\":{\"source_in\":";
        WriteTime(output, join.joined_times.source_in);
        output << ",\"duration\":";
        WriteTime(output, join.joined_times.duration);
        output << ",\"timeline_in\":";
        WriteTime(output, join.joined_times.timeline_in);
        output << "}}";
    }
    return output.str();
}

bool DeserializeOperation(const std::string& json, Operation& operation,
                          EditError& error, std::string& message) {
    try {
        Reader reader(json);
        reader.Expect("{\"type\":");
        const std::string type = reader.String();
        if (type == "InsertClip") {
            reader.Expect(",\"track_id\":");
            InsertClipOperation value;
            value.track_id = reader.String();
            reader.Expect(",\"source_id\":");
            value.source_id = reader.String();
            reader.Expect(",\"source_in\":");
            value.source_in = ReadTime(reader);
            reader.Expect(",\"duration\":");
            value.duration = ReadTime(reader);
            reader.Expect(",\"timeline_in\":");
            value.timeline_in = ReadTime(reader);
            reader.Expect(",\"clip_id\":");
            value.clip_id = reader.String();
            reader.Expect(",\"exact_timeline\":");
            value.exact_timeline_result = ReadExactPositions(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "RemoveClip") {
            reader.Expect(",\"clip_id\":");
            RemoveClipOperation value;
            value.clip_id = reader.String();
            reader.Expect(",\"exact_timeline\":");
            value.exact_timeline_result = ReadExactPositions(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "TrimClip") {
            reader.Expect(",\"clip_id\":");
            TrimClipOperation value;
            value.clip_id = reader.String();
            reader.Expect(",\"edge\":");
            const std::string edge = reader.String();
            if (edge == "Head")
                value.edge = TrimEdge::Head;
            else if (edge == "Tail")
                value.edge = TrimEdge::Tail;
            else
                throw std::runtime_error("unknown trim edge '" + edge + "'");
            reader.Expect(",\"delta\":");
            value.delta = ReadTime(reader);
            reader.Expect(",\"exact_clip\":");
            if (!reader.Consume("null")) {
                reader.Expect("{\"source_in\":");
                ExactClipTimes exact;
                exact.source_in = ReadTime(reader);
                reader.Expect(",\"duration\":");
                exact.duration = ReadTime(reader);
                reader.Expect(",\"timeline_in\":");
                exact.timeline_in = ReadTime(reader);
                reader.Expect("}");
                value.exact_clip_result = exact;
            }
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "MoveClip") {
            reader.Expect(",\"clip_id\":");
            MoveClipOperation value;
            value.clip_id = reader.String();
            reader.Expect(",\"track_id\":");
            value.track_id = reader.String();
            reader.Expect(",\"timeline_in\":");
            value.timeline_in = ReadTime(reader);
            if (!reader.Consume("}")) {
                reader.Expect(",\"exact_tracks\":");
                value.exact_track_result = ReadExactTracks(reader);
                reader.Expect("}");
            }
            operation = std::move(value);
        } else if (type == "MoveLinkedClips") {
            reader.Expect(",\"link_group_id\":");
            MoveLinkedClipsOperation value;
            value.link_group_id = reader.String();
            reader.Expect(",\"moves\":[");
            if (!reader.Consume("]")) {
                while (true) {
                    LinkedClipMove move;
                    reader.Expect("{\"clip_id\":");
                    move.clip_id = reader.String();
                    reader.Expect(",\"track_id\":");
                    move.track_id = reader.String();
                    reader.Expect(",\"timeline_in\":");
                    move.timeline_in = ReadTime(reader);
                    reader.Expect("}");
                    value.moves.push_back(std::move(move));
                    if (reader.Consume("]")) break;
                    reader.Expect(",");
                }
            }
            reader.Expect(",\"exact_tracks\":");
            value.exact_track_result = ReadExactTracks(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "TrimLinkedClips") {
            reader.Expect(",\"link_group_id\":");
            TrimLinkedClipsOperation value;
            value.link_group_id = reader.String();
            reader.Expect(",\"trims\":[");
            if (!reader.Consume("]")) {
                while (true) {
                    LinkedClipTrim trim;
                    reader.Expect("{\"clip_id\":");
                    trim.clip_id = reader.String();
                    reader.Expect(",\"edge\":");
                    const std::string edge = reader.String();
                    if (edge != "Head" && edge != "Tail")
                        throw std::runtime_error("invalid linked trim edge");
                    trim.edge =
                        edge == "Head" ? TrimEdge::Head : TrimEdge::Tail;
                    reader.Expect(",\"delta\":");
                    trim.delta = ReadTime(reader);
                    reader.Expect("}");
                    value.trims.push_back(std::move(trim));
                    if (reader.Consume("]")) break;
                    reader.Expect(",");
                }
            }
            reader.Expect(",\"exact_tracks\":");
            value.exact_track_result = ReadExactTracks(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "RemoveLinkedClips") {
            reader.Expect(",\"link_group_id\":");
            RemoveLinkedClipsOperation value;
            value.link_group_id = reader.String();
            reader.Expect(",\"clip_ids\":[");
            if (!reader.Consume("]")) {
                while (true) {
                    value.clip_ids.push_back(reader.String());
                    if (reader.Consume("]")) break;
                    reader.Expect(",");
                }
            }
            reader.Expect(",\"exact_tracks\":");
            value.exact_track_result = ReadExactTracks(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "SplitClip") {
            reader.Expect(",\"clip_id\":");
            SplitClipOperation value;
            value.clip_id = reader.String();
            reader.Expect(",\"timeline_position\":");
            value.timeline_position = ReadTime(reader);
            reader.Expect(",\"right_clip_id\":");
            value.right_clip_id = reader.String();
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "DeleteGap") {
            reader.Expect(",\"track_id\":");
            DeleteGapOperation value;
            value.track_id = reader.String();
            reader.Expect(",\"gap_start\":");
            value.gap_start = ReadTime(reader);
            reader.Expect(",\"gap_duration\":");
            value.gap_duration = ReadTime(reader);
            reader.Expect(",\"exact_tracks\":");
            value.exact_track_result = ReadExactTracks(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "DetachAudio") {
            reader.Expect(",\"video_clip_id\":");
            DetachAudioOperation value;
            value.video_clip_id = reader.String();
            reader.Expect(",\"audio_track_id\":");
            value.audio_track_id = reader.String();
            reader.Expect(",\"audio_clip_id\":");
            value.audio_clip_id = reader.String();
            reader.Expect(",\"exact_tracks\":");
            value.exact_track_result = ReadExactTracks(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "AddTrack") {
            reader.Expect(",\"track_id\":");
            AddTrackOperation value;
            value.track_id = reader.String();
            reader.Expect(",\"kind\":");
            value.kind = reader.String();
            reader.Expect(",\"index\":");
            const int64_t index = reader.Integer();
            if (index < std::numeric_limits<int32_t>::min() ||
                index > std::numeric_limits<int32_t>::max())
                throw std::runtime_error("track index outside int32_t range");
            value.index = static_cast<int32_t>(index);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "RemoveTrack") {
            reader.Expect(",\"track_id\":");
            RemoveTrackOperation value;
            value.track_id = reader.String();
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "AddBin") {
            reader.Expect(",\"bin_id\":");
            AddBinOperation value;
            value.bin_id = reader.String();
            reader.Expect(",\"name\":");
            value.name = reader.String();
            if (!reader.Consume("}")) {
                reader.Expect(",\"parent_id\":");
                value.parent_id = reader.String();
                reader.Expect("}");
            }
            operation = std::move(value);
        } else if (type == "RemoveBin") {
            reader.Expect(",\"bin_id\":");
            RemoveBinOperation value;
            value.bin_id = reader.String();
            reader.Expect(",\"name\":");
            value.name = reader.String();
            if (!reader.Consume("}")) {
                reader.Expect(",\"parent_id\":");
                value.parent_id = reader.String();
                reader.Expect("}");
            }
            operation = std::move(value);
        } else if (type == "RenameBin") {
            reader.Expect(",\"bin_id\":");
            RenameBinOperation value;
            value.bin_id = reader.String();
            reader.Expect(",\"name\":");
            value.name = reader.String();
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "SetMediaBin") {
            reader.Expect(",\"media_id\":");
            SetMediaBinOperation value;
            value.media_id = reader.String();
            reader.Expect(",\"bin_id\":");
            value.bin_id = reader.String();
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "SetClipLink") {
            reader.Expect(",\"first_clip_id\":");
            SetClipLinkOperation value;
            value.first_clip_id = reader.String();
            reader.Expect(",\"second_clip_id\":");
            value.second_clip_id = reader.String();
            reader.Expect(",\"first_group_id\":");
            value.first_group_id = reader.String();
            reader.Expect(",\"second_group_id\":");
            value.second_group_id = reader.String();
            if (!reader.Consume("}")) {
                const auto readState =
                    [&]() -> std::optional<SetClipLinkOperation::ExactState> {
                    if (reader.Consume("null")) return std::nullopt;
                    SetClipLinkOperation::ExactState state;
                    reader.Expect("{\"group_id\":");
                    state.group_id = reader.String();
                    reader.Expect(",\"anchor_clip_id\":");
                    state.anchor_clip_id = reader.String();
                    reader.Expect(",\"reference_delta\":");
                    state.reference_delta = ReadTime(reader);
                    reader.Expect("}");
                    return state;
                };
                reader.Expect(",\"exact_first\":");
                value.exact_first_result = readState();
                reader.Expect(",\"exact_second\":");
                value.exact_second_result = readState();
                reader.Expect("}");
            }
            operation = std::move(value);
        } else if (type == "JoinClip") {
            reader.Expect(",\"left_clip_id\":");
            JoinClipOperation value;
            value.left_clip_id = reader.String();
            reader.Expect(",\"right_clip_id\":");
            value.right_clip_id = reader.String();
            reader.Expect(",\"joined_times\":{\"source_in\":");
            value.joined_times.source_in = ReadTime(reader);
            reader.Expect(",\"duration\":");
            value.joined_times.duration = ReadTime(reader);
            reader.Expect(",\"timeline_in\":");
            value.joined_times.timeline_in = ReadTime(reader);
            reader.Expect("}}");
            operation = std::move(value);
        } else {
            throw std::runtime_error("unknown operation type '" + type + "'");
        }
        reader.Finish();
        error = EditError::None;
        message.clear();
        return true;
    } catch (const std::exception& exception) {
        error = EditError::ParseError;
        message = exception.what();
        return false;
    }
}
```

### src/Operations.h

```cpp
#pragma once

#include "Document.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

enum class TrimEdge { Head, Tail };

enum class EditError {
    None,
    UnknownTrack,
    UnknownClip,
    UnknownSource,
    UnknownBin,
    UnknownMedia,
    InvalidDuration,
    InvalidTimelineIn,
    SourceOutOfBounds,
    Overlap,
    DuplicateId,
    ArithmeticError,
    InvalidOperation,
    ValidationFailed,
    EmptyUndo,
    EmptyRedo,
    IoError,
    ParseError,
};

const char* EditErrorName(EditError error);

struct ExactTimelinePosition {
    Ulid clip_id;
    RationalTime timeline_in;
};

struct ExactClipTimes {
    RationalTime source_in;
    RationalTime duration;
    RationalTime timeline_in;
};

struct InsertClipOperation {
    Ulid track_id;
    Ulid source_id;
    RationalTime source_in;
    RationalTime duration;
    RationalTime timeline_in;

    // Empty on first application; generated by ApplyOperation and retained for
    // redo/inverse identity.
    Ulid clip_id;
    std::vector<ExactTimelinePosition> exact_timeline_result;
};

struct RemoveClipOperation {
    Ulid clip_id;
    std::vector<ExactTimelinePosition> exact_timeline_result;
};

struct TrimClipOperation {
    Ulid clip_id;
    TrimEdge edge = TrimEdge::Head;
    RationalTime delta;
    std::optional<ExactClipTimes> exact_clip_result;
};

struct ExactTrackState {
    Ulid track_id;
    std::vector<DocumentClip> clips;
};

struct MoveClipOperation {
    Ulid clip_id;
    Ulid track_id;
    RationalTime timeline_in;

    // Empty for a new move. ApplyOperation records the exact resulting tracks
    // so redo retains IDs created by an overwrite split. Inverse operations
    // carry the corresponding exact pre-move tracks.
    std::vector<ExactTrackState> exact_track_result;
};

struct LinkedClipMove {
    Ulid clip_id;
    Ulid track_id;
    RationalTime timeline_in;
};

struct MoveLinkedClipsOperation {
    Ulid link_group_id;
    std::vector<LinkedClipMove> moves;
    std::vector<ExactTrackState> exact_track_result;
};

struct LinkedClipTrim {
    Ulid clip_id;
    TrimEdge edge = TrimEdge::Head;
    RationalTime delta;
};

// One trim gesture over linked A/V is one event-log entry. Exact track
// snapshots make redo/undo deterministic and guarantee all-or-nothing apply.
struct TrimLinkedClipsOperation {
    Ulid link_group_id;
    std::vector<LinkedClipTrim> trims;
    std::vector<ExactTrackState> exact_track_result;
};

struct RemoveLinkedClipsOperation {
    Ulid link_group_id;
    std::vector<Ulid> clip_ids;
    std::vector<ExactTrackState> exact_track_result;
};

struct DeleteGapOperation {
    Ulid track_id;
    RationalTime gap_start;
    RationalTime gap_duration;
    std::vector<ExactTrackState> exact_track_result;
};

struct DetachAudioOperation {
    Ulid video_clip_id;
    Ulid audio_track_id;
    Ulid audio_clip_id;
    std::vector<ExactTrackState> exact_track_result;
};

struct AddTrackOperation {
    Ulid track_id;
    std::string kind = "video";
    int32_t index = -1;
};

struct RemoveTrackOperation {
    Ulid track_id;
};

struct AddBinOperation {
    Ulid bin_id;
    std::string name;
    Ulid parent_id;
};

struct RemoveBinOperation {
    Ulid bin_id;
    // Filled for the inverse/redo path so the stable ID and name survive.
    std::string name;
    Ulid parent_id;
};

struct RenameBinOperation {
    Ulid bin_id;
    std::string name;
};

struct SetMediaBinOperation {
    Ulid media_id;
    // Empty assigns the media to the project root.
    Ulid bin_id;
};

struct SetClipLinkOperation {
    Ulid first_clip_id;
    Ulid second_clip_id;
    Ulid first_group_id;
    Ulid second_group_id;
    struct ExactState {
        Ulid group_id;
        Ulid anchor_clip_id;
        RationalTime reference_delta{0, 1};
    };
    std::optional<ExactState> exact_first_result;
    std::optional<ExactState> exact_second_result;
};

struct SplitClipOperation {
    Ulid clip_id;
    RationalTime timeline_position;

    // Generated on first application and retained for redo identity.
    Ulid right_clip_id;
};

// JoinClip is the exact inverse stored by the event log for SplitClip. Keeping
// it explicit makes persisted undo/redo deterministic across timebases.
struct JoinClipOperation {
    Ulid left_clip_id;
    Ulid right_clip_id;
    ExactClipTimes joined_times;
};

using Operation =
    std::variant<InsertClipOperation, RemoveClipOperation, TrimClipOperation,
                 MoveClipOperation, DeleteGapOperation, DetachAudioOperation,
                 MoveLinkedClipsOperation, TrimLinkedClipsOperation,
                 RemoveLinkedClipsOperation, AddTrackOperation,
                 RemoveTrackOperation, SplitClipOperation, JoinClipOperation,
                 AddBinOperation, RemoveBinOperation, RenameBinOperation,
                 SetMediaBinOperation, SetClipLinkOperation>;

// On success, operation is enriched with generated IDs and exact redo state.
// Both document and operation remain unchanged on failure.
bool ApplyOperation(Document& document, Operation& operation,
                    Operation& inverse, EditError& error, std::string& message);

std::string SerializeOperation(const Operation& operation);
bool DeserializeOperation(const std::string& json, Operation& operation,
                          EditError& error, std::string& message);
```

### src/PerformanceMetrics.cc

```cpp
#include "PerformanceMetrics.h"

#include <algorithm>
#include <vector>

namespace {

int64_t Percentile(const std::vector<int64_t>& sorted, size_t percent) {
    if (sorted.empty()) {
        return 0;
    }
    const size_t rank =
        std::max<size_t>(1, (percent * sorted.size() + 99) / 100);
    return sorted[std::min(rank - 1, sorted.size() - 1)];
}

}  // namespace

void PerformanceMetrics::RecordRequest(bool cacheHit) {
    std::lock_guard<std::mutex> lock(mutex_);
    cacheHits_.push_back(cacheHit);
    hitCount_ += cacheHit ? 1 : 0;
    if (cacheHits_.size() > kWindowSize) {
        hitCount_ -= cacheHits_.front() ? 1 : 0;
        cacheHits_.pop_front();
    }
}

void PerformanceMetrics::RecordDelivery(int64_t microseconds, bool cacheHit) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& samples =
        cacheHit ? hitDeliveryMicroseconds_ : missDeliveryMicroseconds_;
    samples.push_back(microseconds);
    if (samples.size() > kWindowSize) {
        samples.pop_front();
    }
}

void PerformanceMetrics::RecordDrop() {
    drops_.fetch_add(1, std::memory_order_relaxed);
}

void PerformanceMetrics::FrameStarted() {
    framesInFlight_.fetch_add(1, std::memory_order_relaxed);
}

void PerformanceMetrics::FrameFinished() {
    framesInFlight_.fetch_sub(1, std::memory_order_relaxed);
}

PerformanceMetrics::Snapshot PerformanceMetrics::GetSnapshot() const {
    std::vector<int64_t> sortedHits;
    std::vector<int64_t> sortedMisses;
    Snapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sortedHits.assign(hitDeliveryMicroseconds_.begin(),
                          hitDeliveryMicroseconds_.end());
        sortedMisses.assign(missDeliveryMicroseconds_.begin(),
                            missDeliveryMicroseconds_.end());
        snapshot.hitDeliverySamples = sortedHits.size();
        snapshot.missDeliverySamples = sortedMisses.size();
        snapshot.hitRate = cacheHits_.empty() ? 0.0
                                              : static_cast<double>(hitCount_) /
                                                    cacheHits_.size();
    }
    std::sort(sortedHits.begin(), sortedHits.end());
    std::sort(sortedMisses.begin(), sortedMisses.end());
    snapshot.hitP50Us = Percentile(sortedHits, 50);
    snapshot.hitP95Us = Percentile(sortedHits, 95);
    snapshot.hitP99Us = Percentile(sortedHits, 99);
    snapshot.missP50Us = Percentile(sortedMisses, 50);
    snapshot.missP95Us = Percentile(sortedMisses, 95);
    snapshot.missP99Us = Percentile(sortedMisses, 99);
    snapshot.drops = drops_.load(std::memory_order_relaxed);
    snapshot.framesInFlight = framesInFlight_.load(std::memory_order_relaxed);
    return snapshot;
}
```

### src/PerformanceMetrics.h

```cpp
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

class PerformanceMetrics {
public:
    struct Snapshot {
        int64_t hitP50Us = 0;
        int64_t hitP95Us = 0;
        int64_t hitP99Us = 0;
        int64_t missP50Us = 0;
        int64_t missP95Us = 0;
        int64_t missP99Us = 0;
        double hitRate = 0.0;
        uint64_t drops = 0;
        int framesInFlight = 0;
        size_t hitDeliverySamples = 0;
        size_t missDeliverySamples = 0;
    };

    void RecordRequest(bool cacheHit);
    void RecordDelivery(int64_t microseconds, bool cacheHit);
    void RecordDrop();
    void FrameStarted();
    void FrameFinished();
    Snapshot GetSnapshot() const;

private:
    static constexpr size_t kWindowSize = 120;

    mutable std::mutex mutex_;
    std::deque<int64_t> hitDeliveryMicroseconds_;
    std::deque<int64_t> missDeliveryMicroseconds_;
    std::deque<bool> cacheHits_;
    size_t hitCount_ = 0;
    std::atomic<uint64_t> drops_{0};
    std::atomic<int> framesInFlight_{0};
};
```

### src/RationalTime.h

```cpp
#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>

struct RationalTime {
    int64_t value = 0;
    int32_t rate = 1;

    constexpr RationalTime() = default;
    constexpr RationalTime(int64_t valueIn, int32_t rateIn)
        : value(valueIn), rate(rateIn) {}

    RationalTime add(const RationalTime& other) const {
        CheckRate(rate);
        CheckRate(other.rate);
        const int64_t commonRate = Lcm(rate, other.rate);
        if (commonRate > std::numeric_limits<int32_t>::max()) {
            throw std::overflow_error(
                "RationalTime common rate overflows int32_t");
        }
        const __int128 result =
            static_cast<__int128>(value) * (commonRate / rate) +
            static_cast<__int128>(other.value) * (commonRate / other.rate);
        return {CheckedInt64(result), static_cast<int32_t>(commonRate)};
    }

    RationalTime sub(const RationalTime& other) const {
        CheckRate(rate);
        CheckRate(other.rate);
        const int64_t commonRate = Lcm(rate, other.rate);
        if (commonRate > std::numeric_limits<int32_t>::max()) {
            throw std::overflow_error(
                "RationalTime common rate overflows int32_t");
        }
        const __int128 result =
            static_cast<__int128>(value) * (commonRate / rate) -
            static_cast<__int128>(other.value) * (commonRate / other.rate);
        return {CheckedInt64(result), static_cast<int32_t>(commonRate)};
    }

    // Returns -1, 0 or 1 without converting either operand to floating point.
    constexpr int compare(const RationalTime& other) const {
        if (rate <= 0 || other.rate <= 0) {
            throw std::invalid_argument("RationalTime rate must be positive");
        }
        const __int128 left = static_cast<__int128>(value) * other.rate;
        const __int128 right = static_cast<__int128>(other.value) * rate;
        return left < right ? -1 : (left > right ? 1 : 0);
    }

    // Rescaling is exact. Silent rounding would move edit boundaries.
    RationalTime rescale(int32_t newRate) const {
        CheckRate(rate);
        CheckRate(newRate);
        const __int128 numerator = static_cast<__int128>(value) * newRate;
        if (numerator % rate != 0) {
            throw std::invalid_argument(
                "RationalTime cannot be rescaled exactly");
        }
        return {CheckedInt64(numerator / rate), newRate};
    }

    // Maps a time to the containing frame. This is floor division, including
    // for negative values, and supports rates such as 30000/1001.
    int64_t to_frames(int32_t framesPerSecond) const {
        return to_frames(framesPerSecond, 1);
    }

    int64_t to_frames(int32_t frameRateNumerator,
                      int32_t frameRateDenominator) const {
        CheckRate(rate);
        CheckRate(frameRateNumerator);
        CheckRate(frameRateDenominator);
        const __int128 numerator =
            static_cast<__int128>(value) * frameRateNumerator;
        const __int128 denominator =
            static_cast<__int128>(rate) * frameRateDenominator;
        __int128 quotient = numerator / denominator;
        if (numerator < 0 && numerator % denominator != 0) {
            --quotient;
        }
        return CheckedInt64(quotient);
    }

    constexpr bool operator==(const RationalTime& other) const {
        return compare(other) == 0;
    }
    constexpr bool operator!=(const RationalTime& other) const {
        return !(*this == other);
    }
    constexpr bool operator<(const RationalTime& other) const {
        return compare(other) < 0;
    }
    constexpr bool operator<=(const RationalTime& other) const {
        return compare(other) <= 0;
    }
    constexpr bool operator>(const RationalTime& other) const {
        return compare(other) > 0;
    }
    constexpr bool operator>=(const RationalTime& other) const {
        return compare(other) >= 0;
    }

private:
    static constexpr void CheckRate(int32_t candidate) {
        if (candidate <= 0) {
            throw std::invalid_argument("RationalTime rate must be positive");
        }
    }

    static constexpr int64_t Gcd(int64_t a, int64_t b) {
        while (b != 0) {
            const int64_t remainder = a % b;
            a = b;
            b = remainder;
        }
        return a;
    }

    static constexpr int64_t Lcm(int32_t a, int32_t b) {
        return (static_cast<int64_t>(a) / Gcd(a, b)) * b;
    }

    static constexpr int64_t CheckedInt64(__int128 valueIn) {
        if (valueIn < std::numeric_limits<int64_t>::min() ||
            valueIn > std::numeric_limits<int64_t>::max()) {
            throw std::overflow_error("RationalTime value overflows int64_t");
        }
        return static_cast<int64_t>(valueIn);
    }
};
```

### src/Renderer.h

```cpp
#pragma once

#include "Document.h"

#import <AppKit/AppKit.h>

#include <cstdint>
#include <vector>

struct AVFrame;

struct MetalRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float alpha = 1.0f;
};

struct TimelineRenderData {
    double video_height = 0.0;
    ColorManagementSettings color_management;
    std::vector<int32_t> video_rotation_degrees;
    std::vector<MetalRect> rectangles;
};

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool Initialize(NSView* view);
    void Resize(NSRect bounds);
    // Frames are ordered bottom-to-top and composited in that order. Null
    // entries are timeline holes and reveal lower tracks.
    bool RenderFrames(const std::vector<AVFrame*>& frames,
                      const TimelineRenderData& timeline);

private:
    struct Impl;
    Impl* impl_;
};
```

### src/Renderer.mm

```objectivec
#include "Renderer.h"

#include "ColorManagement.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <array>
#include <cstdio>

struct Renderer::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    CAMetalLayer* layer = nil;
    id<MTLRenderPipelineState> workingPipeline = nil;
    id<MTLRenderPipelineState> outputPipeline = nil;
    id<MTLRenderPipelineState> solidPipeline = nil;
    id<MTLSamplerState> sampler = nil;
    id<MTLTexture> workingTexture = nil;
    std::vector<std::array<id<MTLTexture>, 3>> planes;
    std::vector<std::array<int, 3>> textureWidths;
    std::vector<std::array<int, 3>> textureHeights;
    std::vector<int> textureFormats;
    CGColorSpaceRef sdrColorSpace = nullptr;
    CGColorSpaceRef hlgColorSpace = nullptr;

    ~Impl() {
        if (sdrColorSpace) CGColorSpaceRelease(sdrColorSpace);
        if (hlgColorSpace) CGColorSpaceRelease(hlgColorSpace);
    }
};

Renderer::Renderer() : impl_(new Impl()) {}
Renderer::~Renderer() { delete impl_; }

bool Renderer::Initialize(NSView* view) {
    impl_->device = MTLCreateSystemDefaultDevice();
    impl_->queue = [impl_->device newCommandQueue];
    if (!impl_->device || !impl_->queue) {
        std::fprintf(stderr, "Unable to initialize Metal device/queue\n");
        return false;
    }

    impl_->layer = [CAMetalLayer layer];
    impl_->layer.device = impl_->device;
    // XR keeps ten useful bits for HLG while remaining usable for SDR projects.
    impl_->layer.pixelFormat = MTLPixelFormatBGRA10_XR;
    impl_->layer.framebufferOnly = YES;
    impl_->sdrColorSpace = CGColorSpaceCreateWithName(kCGColorSpaceITUR_709);
    impl_->hlgColorSpace =
        CGColorSpaceCreateWithName(kCGColorSpaceITUR_2100_HLG);
    impl_->layer.colorspace = impl_->sdrColorSpace;
    impl_->layer.contentsScale = view.window.backingScaleFactor
                                     ?: NSScreen.mainScreen.backingScaleFactor;
    [view setWantsLayer:YES];
    view.layer = impl_->layer;
    Resize(view.bounds);

    NSError* error = nil;
    NSString* shaderPath =
        [NSString stringWithUTF8String:CUTMACHINE_SHADER_PATH];
    NSString* shaderSource =
        [NSString stringWithContentsOfFile:shaderPath
                                  encoding:NSUTF8StringEncoding
                                     error:&error];
    if (!shaderSource) {
        std::fprintf(stderr, "Unable to read %s: %s\n", shaderPath.UTF8String,
                     error.localizedDescription.UTF8String);
        return false;
    }
    id<MTLLibrary> library = [impl_->device newLibraryWithSource:shaderSource
                                                         options:nil
                                                           error:&error];
    if (!library) {
        std::fprintf(stderr, "Unable to compile %s: %s\n",
                     shaderPath.UTF8String,
                     error.localizedDescription.UTF8String);
        return false;
    }

    id<MTLFunction> vertex = [library newFunctionWithName:@"vertex_main"];
    id<MTLFunction> fragment =
        [library newFunctionWithName:@"fragment_working"];
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = vertex;
    descriptor.fragmentFunction = fragment;
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    descriptor.colorAttachments[0].blendingEnabled = YES;
    descriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
    descriptor.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
    descriptor.colorAttachments[0].sourceRGBBlendFactor =
        MTLBlendFactorSourceAlpha;
    descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    descriptor.colorAttachments[0].destinationRGBBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
    descriptor.colorAttachments[0].destinationAlphaBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
    impl_->workingPipeline =
        [impl_->device newRenderPipelineStateWithDescriptor:descriptor
                                                      error:&error];
    if (!impl_->workingPipeline) {
        std::fprintf(stderr, "Unable to create Metal working pipeline: %s\n",
                     error.localizedDescription.UTF8String);
        return false;
    }

    descriptor.vertexFunction = vertex;
    descriptor.fragmentFunction =
        [library newFunctionWithName:@"fragment_output"];
    descriptor.colorAttachments[0].pixelFormat = impl_->layer.pixelFormat;
    descriptor.colorAttachments[0].blendingEnabled = NO;
    impl_->outputPipeline =
        [impl_->device newRenderPipelineStateWithDescriptor:descriptor
                                                      error:&error];
    if (!impl_->outputPipeline) {
        std::fprintf(stderr, "Unable to create Metal output pipeline: %s\n",
                     error.localizedDescription.UTF8String);
        return false;
    }

    descriptor.vertexFunction = [library newFunctionWithName:@"vertex_solid"];
    descriptor.fragmentFunction =
        [library newFunctionWithName:@"fragment_solid"];
    descriptor.colorAttachments[0].blendingEnabled = YES;
    descriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
    descriptor.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
    descriptor.colorAttachments[0].sourceRGBBlendFactor =
        MTLBlendFactorSourceAlpha;
    descriptor.colorAttachments[0].sourceAlphaBlendFactor =
        MTLBlendFactorSourceAlpha;
    descriptor.colorAttachments[0].destinationRGBBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
    descriptor.colorAttachments[0].destinationAlphaBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
    impl_->solidPipeline =
        [impl_->device newRenderPipelineStateWithDescriptor:descriptor
                                                      error:&error];
    if (!impl_->solidPipeline) {
        std::fprintf(stderr, "Unable to create solid pipeline: %s\n",
                     error.localizedDescription.UTF8String);
        return false;
    }

    MTLSamplerDescriptor* samplerDescriptor = [MTLSamplerDescriptor new];
    samplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
    samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
    samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    impl_->sampler =
        [impl_->device newSamplerStateWithDescriptor:samplerDescriptor];
    return impl_->sampler != nil;
}

void Renderer::Resize(NSRect bounds) {
    if (!impl_->layer) {
        return;
    }
    const CGFloat scale = impl_->layer.contentsScale;
    impl_->layer.frame = bounds;
    impl_->layer.drawableSize =
        CGSizeMake(bounds.size.width * scale, bounds.size.height * scale);
}

bool Renderer::RenderFrames(const std::vector<AVFrame*>& frames,
                            const TimelineRenderData& timeline) {
    if (!impl_->workingPipeline || !impl_->outputPipeline ||
        !impl_->solidPipeline || !impl_->queue) {
        return false;
    }

    if (impl_->planes.size() < frames.size()) {
        impl_->planes.resize(frames.size());
        impl_->textureWidths.resize(frames.size(), {});
        impl_->textureHeights.resize(frames.size(), {});
        impl_->textureFormats.resize(frames.size(), AV_PIX_FMT_NONE);
    }
    for (size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex) {
        const AVFrame* frame = frames[frameIndex];
        if (!frame) continue;
        const AVPixelFormat pixelFormat =
            static_cast<AVPixelFormat>(frame->format);
        const AVPixFmtDescriptor* pixel = av_pix_fmt_desc_get(pixelFormat);
        if (frame->width <= 0 || frame->height <= 0 || !pixel ||
            pixel->nb_components < 3 ||
            (pixel->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL |
                             AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_HWACCEL)) ||
            pixel->comp[0].plane != 0 || pixel->comp[1].plane != 1 ||
            pixel->comp[2].plane != 2 ||
            pixel->comp[0].depth != pixel->comp[1].depth ||
            pixel->comp[0].depth != pixel->comp[2].depth ||
            pixel->comp[0].depth < 8 || pixel->comp[0].depth > 16 ||
            !frame->data[0] ||
            !frame->data[1] || !frame->data[2]) {
            const char* name = av_get_pix_fmt_name(pixelFormat);
            std::fprintf(stderr,
                         "Unsupported planar AVFrame passed to renderer: %s\n",
                         name ? name : "unknown");
            return false;
        }
        std::array<int, 3> planeWidths = {
            frame->width,
            AV_CEIL_RSHIFT(frame->width, pixel->log2_chroma_w),
            AV_CEIL_RSHIFT(frame->width, pixel->log2_chroma_w)};
        std::array<int, 3> planeHeights = {
            frame->height,
            AV_CEIL_RSHIFT(frame->height, pixel->log2_chroma_h),
            AV_CEIL_RSHIFT(frame->height, pixel->log2_chroma_h)};
        if (impl_->textureWidths[frameIndex] != planeWidths ||
            impl_->textureHeights[frameIndex] != planeHeights ||
            impl_->textureFormats[frameIndex] != frame->format) {
            for (int plane = 0; plane < 3; ++plane) {
                MTLTextureDescriptor* textureDescriptor = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:
                        pixel->comp[0].depth > 8 ? MTLPixelFormatR16Unorm
                                                 : MTLPixelFormatR8Unorm
                                                 width:planeWidths[plane]
                                                height:planeHeights[plane]
                                             mipmapped:NO];
                textureDescriptor.storageMode = MTLStorageModeShared;
                textureDescriptor.usage = MTLTextureUsageShaderRead;
                impl_->planes[frameIndex][plane] =
                    [impl_->device newTextureWithDescriptor:textureDescriptor];
                if (!impl_->planes[frameIndex][plane]) {
                    std::fprintf(
                        stderr, "Unable to allocate Metal layer %zu plane %d\n",
                        frameIndex, plane);
                    return false;
                }
            }
            impl_->textureWidths[frameIndex] = planeWidths;
            impl_->textureHeights[frameIndex] = planeHeights;
            impl_->textureFormats[frameIndex] = frame->format;
        }

        for (int plane = 0; plane < 3; ++plane) {
            if (frame->linesize[plane] <= 0) {
                std::fprintf(stderr,
                             "Unsupported negative/zero linesize for layer %zu "
                             "plane %d: %d\n",
                             frameIndex, plane, frame->linesize[plane]);
                return false;
            }
            const MTLRegion region =
                MTLRegionMake2D(0, 0, planeWidths[plane], planeHeights[plane]);
            [impl_->planes[frameIndex][plane]
                replaceRegion:region
                  mipmapLevel:0
                    withBytes:frame->data[plane]
                  bytesPerRow:static_cast<NSUInteger>(frame->linesize[plane])];
        }
    }

    const bool hlgOutput = timeline.color_management.enabled &&
                           timeline.color_management.output_transfer == "hlg";
    impl_->layer.colorspace =
        hlgOutput ? impl_->hlgColorSpace : impl_->sdrColorSpace;
    impl_->layer.wantsExtendedDynamicRangeContent = hlgOutput;
    impl_->layer.EDRMetadata = hlgOutput ? CAEDRMetadata.HLGMetadata : nil;

    id<CAMetalDrawable> drawable = [impl_->layer nextDrawable];
    if (!drawable) {
        std::fprintf(stderr, "CAMetalLayer returned no drawable\n");
        return false;
    }

    id<MTLCommandBuffer> commandBuffer = [impl_->queue commandBuffer];
    if (!impl_->workingTexture ||
        impl_->workingTexture.width != drawable.texture.width ||
        impl_->workingTexture.height != drawable.texture.height) {
        MTLTextureDescriptor* workingDescriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                         width:drawable.texture.width
                                        height:drawable.texture.height
                                     mipmapped:NO];
        workingDescriptor.storageMode = MTLStorageModePrivate;
        workingDescriptor.usage =
            MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        impl_->workingTexture =
            [impl_->device newTextureWithDescriptor:workingDescriptor];
        if (!impl_->workingTexture) {
            std::fprintf(stderr, "Unable to allocate ACES working texture\n");
            return false;
        }
    }

    MTLRenderPassDescriptor* workingPass =
        [MTLRenderPassDescriptor renderPassDescriptor];
    workingPass.colorAttachments[0].texture = impl_->workingTexture;
    workingPass.colorAttachments[0].loadAction = MTLLoadActionClear;
    workingPass.colorAttachments[0].storeAction = MTLStoreActionStore;
    workingPass.colorAttachments[0].clearColor =
        MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    id<MTLRenderCommandEncoder> workingEncoder =
        [commandBuffer renderCommandEncoderWithDescriptor:workingPass];

    const double scale = impl_->layer.contentsScale;
    const double videoHeight =
        std::clamp(timeline.video_height * scale, 0.0,
                   static_cast<double>(drawable.texture.height));
    if (videoHeight > 0.0) {
        [workingEncoder
            setViewport:MTLViewport{0.0, 0.0,
                                    static_cast<double>(drawable.texture.width),
                                    videoHeight, 0.0, 1.0}];
        [workingEncoder setRenderPipelineState:impl_->workingPipeline];
        struct PresentationParameters {
            float left;
            float top;
            float width;
            float height;
            int32_t quarterTurns;
            float opacity;
            int32_t colorManagementEnabled;
            int32_t inputGamut;
            int32_t inputTransfer;
            int32_t useAcescct;
            float redFromCr;
            float greenFromCb;
            float greenFromCr;
            float blueFromCb;
            float sampleScale;
            float yOffset;
            float yScale;
            float chromaOffset;
            float chromaScale;
            float padding[2];
        } parameters = {};
        const auto gamut = [](const std::string& value) -> int32_t {
            if (value == "sony_sgamut3_cine") return 1;
            if (value == "sony_sgamut3") return 2;
            if (value == "rec2020") return 3;
            return 0;
        };
        parameters.colorManagementEnabled =
            timeline.color_management.enabled ? 1 : 0;
        parameters.inputGamut = gamut(timeline.color_management.input_gamut);
        parameters.inputTransfer =
            timeline.color_management.input_transfer == "sony_slog3"
                ? 1
                : (timeline.color_management.input_transfer == "linear" ? 2
                                                                           : 0);
        parameters.useAcescct =
            timeline.color_management.working_gamut == "acescct" ? 1 : 0;
        for (size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex) {
            const AVFrame* frame = frames[frameIndex];
            if (!frame) continue;
            const AVPixFmtDescriptor* pixel = av_pix_fmt_desc_get(
                static_cast<AVPixelFormat>(frame->format));
            const int depth = pixel->comp[0].depth;
            bool fullRange = timeline.color_management.input_range == "full";
            if (timeline.color_management.input_range == "auto") {
                fullRange = frame->color_range == AVCOL_RANGE_JPEG ||
                            (frame->color_range == AVCOL_RANGE_UNSPECIFIED &&
                             parameters.inputTransfer == 1);
            }
            const YuvCodeParameters code =
                BuildYuvCodeParameters(depth, fullRange);
            parameters.sampleScale = code.sample_scale;
            parameters.yOffset = code.y_offset;
            parameters.yScale = code.y_scale;
            parameters.chromaOffset = code.chroma_offset;
            parameters.chromaScale = code.chroma_scale;
            const bool bt2020Matrix =
                timeline.color_management.input_ycbcr_matrix == "auto"
                    ? frame->colorspace == AVCOL_SPC_BT2020_NCL
                    : timeline.color_management.input_ycbcr_matrix ==
                          "bt2020_ncl";
            const YuvMatrixParameters matrix =
                BuildYuvMatrixParameters(bt2020Matrix);
            parameters.redFromCr = matrix.red_from_cr;
            parameters.greenFromCb = matrix.green_from_cb;
            parameters.greenFromCr = matrix.green_from_cr;
            parameters.blueFromCb = matrix.blue_from_cb;
            for (NSUInteger plane = 0; plane < 3; ++plane)
                [workingEncoder
                    setFragmentTexture:impl_->planes[frameIndex][plane]
                                atIndex:plane];
            const int32_t degrees =
                frameIndex < timeline.video_rotation_degrees.size()
                    ? timeline.video_rotation_degrees[frameIndex]
                    : 0;
            const int32_t turns =
                static_cast<int32_t>(std::lround(degrees / 90.0));
            parameters.quarterTurns = ((turns % 4) + 4) % 4;
            const double displayedWidth =
                parameters.quarterTurns % 2 ? frame->height : frame->width;
            const double displayedHeight =
                parameters.quarterTurns % 2 ? frame->width : frame->height;
            const double contentAspect = displayedWidth / displayedHeight;
            const double viewportAspect =
                static_cast<double>(drawable.texture.width) / videoHeight;
            parameters.left = parameters.top = 0.0f;
            parameters.width = parameters.height = 1.0f;
            if (contentAspect > viewportAspect) {
                parameters.height =
                    static_cast<float>(viewportAspect / contentAspect);
                parameters.top = (1.0f - parameters.height) * 0.5f;
            } else {
                parameters.width =
                    static_cast<float>(contentAspect / viewportAspect);
                parameters.left = (1.0f - parameters.width) * 0.5f;
            }
            parameters.opacity = 1.0f;
            [workingEncoder setFragmentBytes:&parameters
                                      length:sizeof(parameters)
                                     atIndex:0];
            [workingEncoder setFragmentSamplerState:impl_->sampler atIndex:0];
            [workingEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                               vertexStart:0
                               vertexCount:6];
        }
    }
    [workingEncoder endEncoding];

    MTLRenderPassDescriptor* pass =
        [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder
        setViewport:MTLViewport{0.0, 0.0,
                                static_cast<double>(drawable.texture.width),
                                static_cast<double>(drawable.texture.height),
                                0.0, 1.0}];
    [encoder setRenderPipelineState:impl_->outputPipeline];
    struct OutputParameters {
        int32_t colorManagementEnabled;
        int32_t outputGamut;
        int32_t outputTransfer;
        int32_t padding;
    } outputParameters = {
        timeline.color_management.enabled ? 1 : 0,
        timeline.color_management.output_gamut == "rec2020" ? 1 : 0,
        hlgOutput ? 1 : 0,
        0};
    [encoder setFragmentTexture:impl_->workingTexture atIndex:0];
    [encoder setFragmentSamplerState:impl_->sampler atIndex:0];
    [encoder setFragmentBytes:&outputParameters
                       length:sizeof(outputParameters)
                      atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:6];

    [encoder
        setViewport:MTLViewport{0.0, 0.0,
                                static_cast<double>(drawable.texture.width),
                                static_cast<double>(drawable.texture.height),
                                0.0, 1.0}];
    [encoder setRenderPipelineState:impl_->solidPipeline];
    struct SolidParameters {
        float rect[4];
        float color[4];
        float drawableSize[2];
        float padding[2];
        int32_t outputTransfer;
        int32_t colorPadding[3];
    } solid;
    solid.outputTransfer = hlgOutput ? 1 : 0;
    for (const MetalRect& item : timeline.rectangles) {
        double left = item.x;
        double top = item.y;
        double width = item.width;
        double height = item.height;
        if (width < 0.0) {
            left += width;
            width = -width;
        }
        if (height < 0.0) {
            top += height;
            height = -height;
        }
        if (width <= 0.0 || height <= 0.0) continue;
        solid.rect[0] = static_cast<float>(left * scale);
        solid.rect[1] = static_cast<float>(top * scale);
        solid.rect[2] = static_cast<float>(width * scale);
        solid.rect[3] = static_cast<float>(height * scale);
        solid.color[0] = item.red;
        solid.color[1] = item.green;
        solid.color[2] = item.blue;
        solid.color[3] = item.alpha;
        solid.drawableSize[0] = static_cast<float>(drawable.texture.width);
        solid.drawableSize[1] = static_cast<float>(drawable.texture.height);
        solid.padding[0] = solid.padding[1] = 0.0f;
        [encoder setVertexBytes:&solid length:sizeof(solid) atIndex:0];
        [encoder setFragmentBytes:&solid length:sizeof(solid) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:0
                    vertexCount:6];
    }
    [encoder endEncoding];
    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];

    // Keep the synchronous path while the cache/renderer boundary is measured.
    [commandBuffer waitUntilCompleted];
    if (commandBuffer.status == MTLCommandBufferStatusError) {
        std::fprintf(stderr, "Metal command failed: %s\n",
                     commandBuffer.error.localizedDescription.UTF8String);
        return false;
    }
    return true;
}
```

### src/Timeline.cc

```cpp
#include "Timeline.h"

#include <algorithm>
#include <stdexcept>

Timeline::Timeline(const Document& document) : document_(document) {}

std::vector<TrackResolution> Timeline::Resolve(RationalTime position) const {
    if (position.rate <= 0) {
        throw std::invalid_argument("timeline position rate must be positive");
    }
    std::vector<TrackResolution> result;
    result.reserve(document_.tracks.size());
    for (const DocumentTrack& track : document_.tracks) {
        result.push_back({track.id, ResolveInTrack(track, position)});
    }
    return result;
}

std::optional<ResolvedFrame> Timeline::ResolveTrack(
    const Ulid& trackId, RationalTime position) const {
    if (position.rate <= 0) {
        throw std::invalid_argument("timeline position rate must be positive");
    }
    const DocumentTrack* track = document_.FindTrack(trackId);
    if (!track) {
        throw std::invalid_argument("unknown track ID '" + trackId + "'");
    }
    return ResolveInTrack(*track, position);
}

std::optional<ResolvedFrame> Timeline::ResolveInTrack(
    const DocumentTrack& track, RationalTime position) const {
    // upper_bound finds the last clip whose start is <= position. Validation
    // guarantees ordering and no overlap, so only that clip can contain it.
    const auto after = std::upper_bound(
        track.clips.begin(), track.clips.end(), position,
        [](const RationalTime& value, const DocumentClip& clip) {
            return value < clip.timeline_in;
        });
    if (after == track.clips.begin()) {
        return std::nullopt;
    }
    const DocumentClip& clip = *std::prev(after);
    const RationalTime offset = position.sub(clip.timeline_in);
    if (offset.value < 0 || offset >= clip.duration) {
        return std::nullopt;
    }
    const DocumentSource* source = document_.FindSource(clip.source_id);
    if (!source) {
        throw std::logic_error("validated clip references an unknown source");
    }
    const RationalTime sourceTime = clip.source_in.add(offset);
    return ResolvedFrame{
        source->id,
        sourceTime.to_frames(source->rate.num, source->rate.den),
    };
}

RationalTime Timeline::Duration() const {
    RationalTime duration{0, 1};
    RationalTime timebase{0, 1};
    for (const DocumentTrack& track : document_.tracks) {
        for (const DocumentClip& clip : track.clips) {
            timebase = timebase.add(RationalTime{0, clip.timeline_in.rate});
            timebase = timebase.add(RationalTime{0, clip.duration.rate});
            const RationalTime end = clip.timeline_in.add(clip.duration);
            if (end > duration) {
                duration = end;
            }
        }
    }
    return duration.rescale(timebase.rate);
}
```

### src/Timeline.h

```cpp
#pragma once

#include "Document.h"

#include <cstdint>
#include <optional>
#include <vector>

struct ResolvedFrame {
    Ulid source_id;
    int64_t source_frame = 0;
};

struct TrackResolution {
    Ulid track_id;
    std::optional<ResolvedFrame> frame;
};

class Timeline {
public:
    explicit Timeline(const Document& document);

    std::vector<TrackResolution> Resolve(RationalTime position) const;
    std::optional<ResolvedFrame> ResolveTrack(const Ulid& trackId,
                                              RationalTime position) const;
    RationalTime Duration() const;

private:
    std::optional<ResolvedFrame> ResolveInTrack(const DocumentTrack& track,
                                                RationalTime position) const;

    const Document& document_;
};
```

### src/TimelineView.cc

```cpp
#include "TimelineView.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {

void CheckViewport(const TimelineViewport& viewport) {
    if (viewport.view_start.rate <= 0 ||
        !std::isfinite(viewport.pixels_per_second) ||
        viewport.pixels_per_second <= 0.0 ||
        !std::isfinite(viewport.track_height) || viewport.track_height <= 0.0 ||
        !std::isfinite(viewport.header_width)) {
        throw std::invalid_argument("invalid timeline viewport");
    }
}

int64_t RoundedInt64(long double value) {
    if (!std::isfinite(value) ||
        value < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
        value > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        throw std::overflow_error("timeline coordinate is outside int64 range");
    }
    // Round to nearest frame/tick. Halfway values are rounded away from zero,
    // matching std::round and making the policy explicit for negative views.
    return static_cast<int64_t>(std::round(value));
}

int64_t RoundNonNegativeRatio(__int128 numerator, __int128 denominator) {
    if (numerator < 0 || denominator <= 0)
        throw std::invalid_argument("invalid playhead quantization ratio");
    const __int128 quotient = numerator / denominator;
    const __int128 remainder = numerator % denominator;
    const __int128 rounded = quotient + (remainder * 2 >= denominator ? 1 : 0);
    if (rounded > std::numeric_limits<int64_t>::max())
        throw std::overflow_error("playhead position exceeds int64 range");
    return static_cast<int64_t>(rounded);
}

ExactClipTimes ProposedTimes(const DocumentClip& clip, TrimEdge edge,
                             const RationalTime& delta) {
    ExactClipTimes result{clip.source_in, clip.duration, clip.timeline_in};
    if (edge == TrimEdge::Head) {
        result.source_in = result.source_in.add(delta);
        result.duration = result.duration.sub(delta);
        result.timeline_in = result.timeline_in.add(delta);
    } else {
        result.duration = result.duration.add(delta);
    }
    return result;
}

RationalTime MaxTime(const RationalTime& left, const RationalTime& right) {
    return left < right ? right : left;
}

RationalTime MinTime(const RationalTime& left, const RationalTime& right) {
    return left > right ? right : left;
}

}  // namespace

RationalTime QuantizePlayheadPosition(RationalTime position,
                                      PlayheadResolution resolution,
                                      MediaRate frameRate, int32_t sampleRate) {
    if (position.rate <= 0 || position.value < 0)
        throw std::invalid_argument("playhead position must be non-negative");
    if (resolution == PlayheadResolution::Sample) {
        if (sampleRate <= 0)
            throw std::invalid_argument("sample rate must be positive");
        const int64_t sample = RoundNonNegativeRatio(
            static_cast<__int128>(position.value) * sampleRate, position.rate);
        return {sample, sampleRate};
    }
    if (frameRate.num <= 0 || frameRate.den <= 0)
        throw std::invalid_argument("frame rate must be positive");
    const int64_t frame = RoundNonNegativeRatio(
        static_cast<__int128>(position.value) * frameRate.num,
        static_cast<__int128>(position.rate) * frameRate.den);
    if (static_cast<__int128>(frame) * frameRate.den >
        std::numeric_limits<int64_t>::max())
        throw std::overflow_error("frame position exceeds int64 range");
    return {frame * frameRate.den, frameRate.num};
}

double TimelineViewport::TimeToX(RationalTime time) const {
    CheckViewport(*this);
    if (time.rate <= 0)
        throw std::invalid_argument("time rate must be positive");
    const long double seconds =
        static_cast<long double>(time.value) / time.rate -
        static_cast<long double>(view_start.value) / view_start.rate;
    return header_width + static_cast<double>(seconds * pixels_per_second);
}

RationalTime TimelineViewport::XToTime(double x, int32_t rate) const {
    CheckViewport(*this);
    if (!std::isfinite(x) || rate <= 0)
        throw std::invalid_argument("invalid coordinate or requested rate");
    const long double seconds =
        static_cast<long double>(view_start.value) / view_start.rate +
        (static_cast<long double>(x) - header_width) / pixels_per_second;
    return {RoundedInt64(seconds * rate), rate};
}

void TimelineViewport::ScrollByPixels(double delta_x, int32_t rate) {
    view_start = XToTime(header_width + delta_x, rate);
}

void TimelineViewport::ZoomAroundX(double x, double factor, int32_t rate) {
    if (!std::isfinite(factor) || factor <= 0.0)
        throw std::invalid_argument("zoom factor must be positive");
    const RationalTime anchor = XToTime(x, rate);
    pixels_per_second = std::clamp(pixels_per_second * factor, 4.0, 4000.0);
    const RationalTime displaced = XToTime(x, rate);
    view_start = view_start.add(anchor.sub(displaced));
}

void TimelineViewport::FitDuration(RationalTime duration, double width) {
    CheckViewport(*this);
    if (duration.rate <= 0 || !std::isfinite(width) || width <= header_width) {
        throw std::invalid_argument("invalid duration or viewport width");
    }
    view_start = {0, duration.rate};
    if (duration.value <= 0) {
        pixels_per_second = 100.0;
        return;
    }
    const long double seconds =
        static_cast<long double>(duration.value) / duration.rate;
    const long double available = std::max(1.0, width - header_width - 24.0);
    pixels_per_second =
        std::clamp(static_cast<double>(available / seconds), 4.0, 4000.0);
}

std::vector<double> TimelineViewport::TickXs(double width) const {
    CheckViewport(*this);
    struct Step {
        int64_t value;
        int32_t rate;
    };
    static constexpr Step steps[] = {
        {1, 120}, {1, 60},  {1, 30},  {1, 24},  {1, 10},   {1, 5},
        {1, 2},   {1, 1},   {2, 1},   {5, 1},   {10, 1},   {30, 1},
        {60, 1},  {120, 1}, {300, 1}, {600, 1}, {1800, 1}, {3600, 1}};
    Step step = steps[sizeof(steps) / sizeof(steps[0]) - 1];
    for (const Step candidate : steps) {
        if ((static_cast<double>(candidate.value) / candidate.rate) *
                pixels_per_second >=
            56.0) {
            step = candidate;
            break;
        }
    }
    const __int128 numerator =
        static_cast<__int128>(view_start.value) * step.rate;
    const __int128 denominator =
        static_cast<__int128>(view_start.rate) * step.value;
    __int128 ordinal = numerator / denominator;
    if (numerator > 0 && numerator % denominator != 0) ++ordinal;
    std::vector<double> result;
    for (size_t count = 0; count < 10000; ++count, ++ordinal) {
        const __int128 tickValue = ordinal * step.value;
        if (tickValue < std::numeric_limits<int64_t>::min() ||
            tickValue > std::numeric_limits<int64_t>::max())
            break;
        const double x = TimeToX({static_cast<int64_t>(tickValue), step.rate});
        if (x > width) break;
        if (x >= header_width) result.push_back(x);
    }
    return result;
}

std::vector<const DocumentTrack*> TimelineTracksInDisplayOrder(
    const Document& document) {
    std::vector<const DocumentTrack*> tracks;
    tracks.reserve(document.tracks.size());
    for (const DocumentTrack& track : document.tracks) tracks.push_back(&track);
    std::stable_sort(tracks.begin(), tracks.end(),
                     [](const DocumentTrack* a, const DocumentTrack* b) {
                         return a->index < b->index;
                     });
    return tracks;
}

std::optional<TimelineHit> HitTestTimeline(const Document& document,
                                           const TimelineViewport& viewport,
                                           double x, double y, double width) {
    if (x < viewport.header_width || x > width || y < kTimelineRulerHeight)
        return std::nullopt;
    const auto tracks = TimelineTracksInDisplayOrder(document);
    const size_t trackIndex = static_cast<size_t>(
        std::floor((y - kTimelineRulerHeight) / viewport.track_height));
    if (trackIndex >= tracks.size()) return std::nullopt;
    const DocumentTrack& track = *tracks[trackIndex];
    for (const DocumentClip& clip : track.clips) {
        const double left = viewport.TimeToX(clip.timeline_in);
        const double right =
            viewport.TimeToX(clip.timeline_in.add(clip.duration));
        if (right < viewport.header_width || left > width) continue;
        if (x >= left && x <= right) {
            const double headDistance = std::abs(x - left);
            const double tailDistance = std::abs(x - right);
            TimelineHitEdge edge = TimelineHitEdge::None;
            if (std::min(headDistance, tailDistance) <= kTimelineEdgeHitWidth) {
                edge = headDistance <= tailDistance ? TimelineHitEdge::Head
                                                    : TimelineHitEdge::Tail;
            }
            return TimelineHit{clip.id, track.id, edge};
        }
    }
    return std::nullopt;
}

std::optional<TimelineGapSelection> HitTestTimelineGap(
    const Document& document, const TimelineViewport& viewport, double x,
    double y, double width, int32_t timeRate) {
    if (x < viewport.header_width || x > width || y < kTimelineRulerHeight)
        return std::nullopt;
    const auto tracks = TimelineTracksInDisplayOrder(document);
    const size_t trackIndex = static_cast<size_t>(
        std::floor((y - kTimelineRulerHeight) / viewport.track_height));
    if (trackIndex >= tracks.size()) return std::nullopt;
    const DocumentTrack& track = *tracks[trackIndex];
    const RationalTime position = viewport.XToTime(x, timeRate);
    RationalTime cursor{0, timeRate};
    for (const DocumentClip& clip : track.clips) {
        if (cursor < clip.timeline_in && position >= cursor &&
            position < clip.timeline_in) {
            return TimelineGapSelection{track.id, cursor,
                                        clip.timeline_in.sub(cursor)};
        }
        cursor = clip.timeline_in.add(clip.duration);
    }
    return std::nullopt;
}

std::vector<Ulid> LassoHitTestTimeline(const Document& document,
                                       const TimelineViewport& viewport,
                                       double startX, double startY,
                                       double endX, double endY, double width) {
    const double left = std::max(viewport.header_width, std::min(startX, endX));
    const double right = std::min(width, std::max(startX, endX));
    const double top = std::max(kTimelineRulerHeight, std::min(startY, endY));
    const double bottom = std::max(startY, endY);
    std::vector<Ulid> result;
    if (right < left || bottom < top) return result;
    const auto tracks = TimelineTracksInDisplayOrder(document);
    for (size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        const double clipTop =
            kTimelineRulerHeight + trackIndex * viewport.track_height + 3.0;
        const double clipBottom = clipTop + viewport.track_height - 6.0;
        if (clipBottom < top || clipTop > bottom) continue;
        for (const DocumentClip& clip : tracks[trackIndex]->clips) {
            const double clipLeft = viewport.TimeToX(clip.timeline_in);
            const double clipRight =
                viewport.TimeToX(clip.timeline_in.add(clip.duration));
            if (clipRight >= left && clipLeft <= right)
                result.push_back(clip.id);
        }
    }
    return result;
}

std::vector<Ulid> ExpandLinkedClipSelection(const Document& document,
                                            const std::vector<Ulid>& clipIds) {
    std::vector<Ulid> result;
    std::vector<Ulid> groups;
    for (const Ulid& id : clipIds) {
        const DocumentClip* clip = document.FindClip(id);
        if (!clip ||
            std::find(result.begin(), result.end(), id) != result.end())
            continue;
        result.push_back(id);
        if (!clip->link_group_id.empty() &&
            std::find(groups.begin(), groups.end(), clip->link_group_id) ==
                groups.end())
            groups.push_back(clip->link_group_id);
    }
    for (const DocumentTrack& track : document.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (clip.link_group_id.empty() ||
                std::find(groups.begin(), groups.end(), clip.link_group_id) ==
                    groups.end() ||
                std::find(result.begin(), result.end(), clip.id) !=
                    result.end())
                continue;
            result.push_back(clip.id);
        }
    }
    return result;
}

std::optional<RationalTime> ClipSyncDrift(const Document& document,
                                          const Ulid& clipId) {
    const DocumentClip* clip = document.FindClip(clipId);
    if (!clip || clip->sync_anchor_clip_id.empty()) return std::nullopt;
    const DocumentClip* anchor = document.FindClip(clip->sync_anchor_clip_id);
    if (!anchor || anchor->link_group_id != clip->link_group_id)
        return std::nullopt;
    try {
        const RationalTime clipPhase = clip->timeline_in.sub(clip->source_in);
        const RationalTime anchorPhase =
            anchor->timeline_in.sub(anchor->source_in);
        return clipPhase.sub(anchorPhase).sub(clip->sync_reference_delta);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

TimelineInteraction::TimelineInteraction(Document& document, EditLog& editLog,
                                         TimelineViewport& viewport)
    : document_(document), edit_log_(editLog), viewport_(viewport) {}

void TimelineInteraction::PointerDown(double x, double y, double width,
                                      int32_t timeRate) {
    requested_playhead_.reset();
    preview_.reset();
    move_preview_.reset();
    snap_guide_time_.reset();
    drag_hit_.reset();
    drag_start_time_.reset();
    const auto hit = HitTestTimeline(document_, viewport_, x, y, width);
    if (!hit) {
        selected_gap_ =
            HitTestTimelineGap(document_, viewport_, x, y, width, timeRate);
        if (x >= viewport_.header_width && y >= 0.0) {
            requested_playhead_ = viewport_.XToTime(x, timeRate);
        }
        selected_clip_id_.clear();
        selected_clip_ids_.clear();
        return;
    }
    selected_gap_.reset();
    selected_clip_id_ = hit->clip_id;
    selected_clip_ids_ = {hit->clip_id};
    if (linked_selection_enabled_)
        selected_clip_ids_ =
            ExpandLinkedClipSelection(document_, selected_clip_ids_);
    const DocumentClip* clip = document_.FindClip(hit->clip_id);
    drag_rate_ = clip ? clip->duration.rate : timeRate;
    drag_x_ = x;
    drag_start_time_ = viewport_.XToTime(x, drag_rate_);
    drag_hit_ = hit;
    UpdatePreview(x, y, width);
}

void TimelineInteraction::PointerDrag(double x, double y, double width) {
    if (drag_hit_) UpdatePreview(x, y, width);
}

void TimelineInteraction::UpdatePreview(double x, double y, double width) {
    (void)width;
    const DocumentClip* clip = document_.FindClip(drag_hit_->clip_id);
    if (!clip || !drag_start_time_) {
        preview_.reset();
        return;
    }
    RationalTime delta =
        viewport_.XToTime(x, drag_rate_).sub(*drag_start_time_);
    drag_x_ = x;
    snap_guide_time_.reset();
    if (drag_hit_->edge == TimelineHitEdge::None) {
        TimelineMovePreview preview;
        preview.clip_id = clip->id;
        preview.timeline_in = clip->timeline_in.add(delta);
        if (preview.timeline_in < RationalTime{0, 1})
            preview.timeline_in = {0, preview.timeline_in.rate};
        const auto tracks = TimelineTracksInDisplayOrder(document_);
        if (y >= kTimelineRulerHeight) {
            const size_t index = static_cast<size_t>(std::floor(
                (y - kTimelineRulerHeight) / viewport_.track_height));
            if (index < tracks.size())
                preview.target_track_id = tracks[index]->id;
        }
        if (!preview.target_track_id.empty()) {
            const DocumentTrack* targetTrack =
                document_.FindTrack(preview.target_track_id);
            RationalTime unsnapped = preview.timeline_in;
            std::optional<RationalTime> snappedBoundary;
            if (snapping_enabled_ && targetTrack) {
                const auto startSnap =
                    FindSnapTime(unsnapped, *targetTrack, clip->id, true);
                const RationalTime end = unsnapped.add(clip->duration);
                const auto endSnap = FindSnapTime(end, *targetTrack, clip->id);
                if (startSnap && endSnap) {
                    const double startDistance =
                        std::abs(viewport_.TimeToX(*startSnap) -
                                 viewport_.TimeToX(unsnapped));
                    const double endDistance = std::abs(
                        viewport_.TimeToX(*endSnap) - viewport_.TimeToX(end));
                    if (startDistance <= endDistance) {
                        preview.timeline_in = *startSnap;
                        snappedBoundary = startSnap;
                    } else {
                        preview.timeline_in = endSnap->sub(clip->duration);
                        snappedBoundary = endSnap;
                    }
                } else if (startSnap) {
                    preview.timeline_in = *startSnap;
                    snappedBoundary = startSnap;
                } else if (endSnap) {
                    preview.timeline_in = endSnap->sub(clip->duration);
                    snappedBoundary = endSnap;
                }
            }
            const auto buildOperation = [&](RationalTime anchorTimeline) {
                preview.linked_moves.clear();
                preview.link_group_id.clear();
                const bool linked = linked_selection_enabled_ &&
                                    selected_clip_ids_.size() > 1 &&
                                    !clip->link_group_id.empty();
                if (linked) {
                    const RationalTime groupDelta =
                        anchorTimeline.sub(clip->timeline_in);
                    for (const Ulid& id : selected_clip_ids_) {
                        const DocumentClip* member = document_.FindClip(id);
                        const DocumentTrack* memberTrack =
                            document_.FindTrackForClip(id);
                        if (!member || !memberTrack ||
                            member->link_group_id != clip->link_group_id)
                            continue;
                        preview.linked_moves.push_back(
                            {id,
                             id == clip->id ? preview.target_track_id
                                            : memberTrack->id,
                             member->timeline_in.add(groupDelta)});
                    }
                    if (preview.linked_moves.size() > 1) {
                        preview.link_group_id = clip->link_group_id;
                        return Operation{MoveLinkedClipsOperation{
                            clip->link_group_id, preview.linked_moves, {}}};
                    }
                }
                return Operation{MoveClipOperation{
                    clip->id, preview.target_track_id, anchorTimeline, {}}};
            };
            Document candidate = document_;
            Operation operation = buildOperation(preview.timeline_in);
            Operation inverse = RemoveClipOperation{};
            EditError error = EditError::None;
            std::string message;
            preview.valid =
                ApplyOperation(candidate, operation, inverse, error, message);
            if (!preview.valid && snappedBoundary) {
                // Never let magnetism turn an otherwise legal free drop into
                // an invalid one.
                candidate = document_;
                operation = buildOperation(unsnapped);
                preview.valid = ApplyOperation(candidate, operation, inverse,
                                               error, message);
                preview.timeline_in = unsnapped;
                snappedBoundary.reset();
            }
            if (preview.valid && snappedBoundary)
                snap_guide_time_ = snappedBoundary;
        }
        move_preview_ = preview;
        preview_.reset();
        return;
    }
    const TrimEdge edge = drag_hit_->edge == TimelineHitEdge::Head
                              ? TrimEdge::Head
                              : TrimEdge::Tail;
    const DocumentTrack* track = document_.FindTrackForClip(clip->id);
    const RationalTime originalBoundary =
        edge == TrimEdge::Head ? clip->timeline_in
                               : clip->timeline_in.add(clip->duration);
    RationalTime candidateBoundary = originalBoundary.add(delta);
    std::optional<RationalTime> snappedBoundary;
    if (snapping_enabled_ && track) {
        snappedBoundary = FindSnapTime(candidateBoundary, *track, clip->id);
        if (snappedBoundary) delta = snappedBoundary->sub(originalBoundary);
    }
    delta = ConstrainTrimDelta(*clip, edge, delta);
    candidateBoundary = originalBoundary.add(delta);
    if (snappedBoundary && candidateBoundary == *snappedBoundary)
        snap_guide_time_ = snappedBoundary;
    TimelineTrimPreview preview;
    preview.clip_id = clip->id;
    try {
        preview.times = ProposedTimes(*clip, edge, delta);
        Document candidate = document_;
        Operation operation =
            TrimClipOperation{clip->id, edge, delta, std::nullopt};
        if (linked_selection_enabled_ && !clip->link_group_id.empty()) {
            std::vector<LinkedClipTrim> trims;
            for (const Ulid& id : selected_clip_ids_) {
                const DocumentClip* member = document_.FindClip(id);
                if (member && member->link_group_id == clip->link_group_id)
                    trims.push_back({id, edge, delta});
            }
            if (trims.size() > 1) {
                preview.link_group_id = clip->link_group_id;
                operation = TrimLinkedClipsOperation{
                    clip->link_group_id, std::move(trims), {}};
            }
        }
        Operation inverse = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        preview.valid =
            ApplyOperation(candidate, operation, inverse, error, message);
        if (!preview.link_group_id.empty()) {
            for (const Ulid& id : selected_clip_ids_) {
                const DocumentClip* member = preview.valid
                                                 ? candidate.FindClip(id)
                                                 : document_.FindClip(id);
                if (member && member->link_group_id == preview.link_group_id)
                    preview.linked_times.push_back(
                        {id, preview.valid
                                 ? ExactClipTimes{member->source_in,
                                                  member->duration,
                                                  member->timeline_in}
                                 : ProposedTimes(*member, edge, delta)});
            }
        }
    } catch (const std::exception&) {
        preview.times = {clip->source_in, clip->duration, clip->timeline_in};
        preview.valid = false;
    }
    preview_ = preview;
    move_preview_.reset();
}

RationalTime TimelineInteraction::ConstrainTrimDelta(const DocumentClip& clip,
                                                     TrimEdge edge,
                                                     RationalTime delta) const {
    const DocumentSource* source = document_.FindSource(clip.source_id);
    const DocumentTrack* track = document_.FindTrackForClip(clip.id);
    if (!source || !track) return delta;
    const RationalTime zero{0, 1};
    const RationalTime minimumDuration{1, drag_rate_};
    const auto found = std::find_if(
        track->clips.begin(), track->clips.end(),
        [&](const DocumentClip& candidate) { return candidate.id == clip.id; });
    const size_t index =
        static_cast<size_t>(std::distance(track->clips.begin(), found));

    RationalTime lower;
    RationalTime upper;
    if (edge == TrimEdge::Head) {
        lower = MaxTime(zero.sub(clip.source_in), zero.sub(clip.timeline_in));
        if (index > 0) {
            const DocumentClip& previous = track->clips[index - 1];
            lower = MaxTime(lower, previous.timeline_in.add(previous.duration)
                                       .sub(clip.timeline_in));
        }
        upper = clip.duration.sub(minimumDuration);
    } else {
        lower = minimumDuration.sub(clip.duration);
        upper = source->duration.sub(clip.source_in.add(clip.duration));
        if (index + 1 < track->clips.size()) {
            const DocumentClip& next = track->clips[index + 1];
            upper = MinTime(upper, next.timeline_in.sub(
                                       clip.timeline_in.add(clip.duration)));
        }
    }
    if (delta < lower) return lower;
    if (delta > upper) return upper;
    return delta;
}

std::optional<RationalTime> TimelineInteraction::FindSnapTime(
    RationalTime candidate, const DocumentTrack& track,
    const Ulid& excludedClipId, bool includeSyncTarget) const {
    std::optional<RationalTime> best;
    double bestDistance = kTimelineSnapDistance + 1.0;
    const auto consider = [&](RationalTime target) {
        const double distance =
            std::abs(viewport_.TimeToX(target) - viewport_.TimeToX(candidate));
        if (distance <= kTimelineSnapDistance && distance < bestDistance) {
            best = target;
            bestDistance = distance;
        }
    };
    consider({0, candidate.rate});
    if (includeSyncTarget && !linked_selection_enabled_) {
        const DocumentClip* clip = document_.FindClip(excludedClipId);
        const DocumentClip* anchor =
            clip ? document_.FindClip(clip->sync_anchor_clip_id) : nullptr;
        if (clip && anchor && anchor->id != clip->id &&
            anchor->link_group_id == clip->link_group_id) {
            try {
                const RationalTime anchorPhase =
                    anchor->timeline_in.sub(anchor->source_in);
                consider(clip->source_in.add(anchorPhase)
                             .add(clip->sync_reference_delta));
            } catch (const std::exception&) {
            }
        }
    }
    for (const DocumentClip& other : track.clips) {
        if (other.id == excludedClipId) continue;
        consider(other.timeline_in);
        consider(other.timeline_in.add(other.duration));
    }
    return best;
}

std::optional<Operation> TimelineInteraction::PendingOperation() const {
    if (!drag_hit_ || !drag_start_time_) return std::nullopt;
    if (drag_hit_->edge == TimelineHitEdge::None) {
        if (!move_preview_ || move_preview_->target_track_id.empty())
            return std::nullopt;
        const DocumentTrack* originalTrack =
            document_.FindTrackForClip(drag_hit_->clip_id);
        const DocumentClip* clip = document_.FindClip(drag_hit_->clip_id);
        if (!originalTrack || !clip) return std::nullopt;
        if (move_preview_->target_track_id == originalTrack->id &&
            move_preview_->timeline_in == clip->timeline_in)
            return std::nullopt;
        if (move_preview_->linked_moves.size() > 1)
            return Operation{MoveLinkedClipsOperation{
                move_preview_->link_group_id, move_preview_->linked_moves, {}}};
        return Operation{MoveClipOperation{drag_hit_->clip_id,
                                           move_preview_->target_track_id,
                                           move_preview_->timeline_in}};
    }
    const DocumentClip* clip = document_.FindClip(drag_hit_->clip_id);
    if (!clip || !preview_) return std::nullopt;
    const RationalTime delta =
        drag_hit_->edge == TimelineHitEdge::Head
            ? preview_->times.timeline_in.sub(clip->timeline_in)
            : preview_->times.duration.sub(clip->duration);
    if (delta.value == 0) return std::nullopt;
    if (!preview_->link_group_id.empty() && preview_->linked_times.size() > 1) {
        std::vector<LinkedClipTrim> trims;
        trims.reserve(preview_->linked_times.size());
        for (const auto& member : preview_->linked_times)
            trims.push_back({member.first,
                             drag_hit_->edge == TimelineHitEdge::Head
                                 ? TrimEdge::Head
                                 : TrimEdge::Tail,
                             delta});
        return Operation{TrimLinkedClipsOperation{
            preview_->link_group_id, std::move(trims), {}}};
    }
    return Operation{TrimClipOperation{drag_hit_->clip_id,
                                       drag_hit_->edge == TimelineHitEdge::Head
                                           ? TrimEdge::Head
                                           : TrimEdge::Tail,
                                       delta, std::nullopt}};
}

bool TimelineInteraction::PointerUp(EditError& error, std::string& message) {
    bool applied = false;
    const std::optional<Operation> operation = PendingOperation();
    const bool valid = (preview_ && preview_->valid) ||
                       (move_preview_ && move_preview_->valid);
    if (operation && valid)
        applied = edit_log_.Apply(document_, *operation, error, message);
    else {
        error = EditError::None;
        message.clear();
    }
    drag_hit_.reset();
    drag_start_time_.reset();
    preview_.reset();
    move_preview_.reset();
    snap_guide_time_.reset();
    return applied;
}

void TimelineInteraction::CancelDrag() {
    drag_hit_.reset();
    drag_start_time_.reset();
    preview_.reset();
    move_preview_.reset();
    snap_guide_time_.reset();
}

const Ulid& TimelineInteraction::SelectedClipId() const {
    return selected_clip_id_;
}

const std::vector<Ulid>& TimelineInteraction::SelectedClipIds() const {
    return selected_clip_ids_;
}

void TimelineInteraction::SelectClip(const Ulid& clipId) {
    selected_clip_id_ = document_.FindClip(clipId) ? clipId : Ulid{};
    selected_clip_ids_.clear();
    if (!selected_clip_id_.empty())
        selected_clip_ids_.push_back(selected_clip_id_);
    selected_gap_.reset();
}

void TimelineInteraction::SelectClips(const std::vector<Ulid>& clipIds) {
    selected_clip_ids_.clear();
    for (const Ulid& id : clipIds) {
        if (document_.FindClip(id) &&
            std::find(selected_clip_ids_.begin(), selected_clip_ids_.end(),
                      id) == selected_clip_ids_.end())
            selected_clip_ids_.push_back(id);
    }
    selected_clip_id_ =
        selected_clip_ids_.size() == 1 ? selected_clip_ids_.front() : Ulid{};
    selected_gap_.reset();
}

const std::optional<TimelineGapSelection>& TimelineInteraction::SelectedGap()
    const {
    return selected_gap_;
}

void TimelineInteraction::ClearGapSelection() { selected_gap_.reset(); }

const std::optional<RationalTime>& TimelineInteraction::RequestedPlayhead()
    const {
    return requested_playhead_;
}

void TimelineInteraction::ClearRequestedPlayhead() {
    requested_playhead_.reset();
}

const std::optional<TimelineTrimPreview>& TimelineInteraction::TrimPreview()
    const {
    return preview_;
}

const std::optional<TimelineMovePreview>& TimelineInteraction::MovePreview()
    const {
    return move_preview_;
}

bool TimelineInteraction::HasActiveDrag() const {
    return drag_hit_.has_value();
}

void TimelineInteraction::SetSnappingEnabled(bool enabled) {
    snapping_enabled_ = enabled;
    if (!enabled) snap_guide_time_.reset();
}

bool TimelineInteraction::SnappingEnabled() const { return snapping_enabled_; }

const std::optional<RationalTime>& TimelineInteraction::SnapGuideTime() const {
    return snap_guide_time_;
}

void TimelineInteraction::SetLinkedSelectionEnabled(bool enabled) {
    linked_selection_enabled_ = enabled;
}

std::vector<TimelineClipRect> VisibleTimelineClips(
    const Document& document, const TimelineViewport& viewport, double width,
    const std::vector<Ulid>& selectedClipIds,
    const std::optional<TimelineTrimPreview>& preview,
    const std::optional<TimelineMovePreview>& movePreview) {
    std::vector<TimelineClipRect> result;
    std::vector<TimelineClipRect> movingRects;
    const auto tracks = TimelineTracksInDisplayOrder(document);
    for (size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        for (const DocumentClip& clip : tracks[trackIndex]->clips) {
            size_t displayTrackIndex = trackIndex;
            RationalTime start = clip.timeline_in;
            RationalTime duration = clip.duration;
            bool isPreview = false;
            bool valid = true;
            bool isMoving = false;
            if (preview && (preview->clip_id == clip.id ||
                            std::any_of(preview->linked_times.begin(),
                                        preview->linked_times.end(),
                                        [&](const auto& member) {
                                            return member.first == clip.id;
                                        }))) {
                const auto member = std::find_if(
                    preview->linked_times.begin(), preview->linked_times.end(),
                    [&](const auto& value) { return value.first == clip.id; });
                const ExactClipTimes& times =
                    member == preview->linked_times.end() ? preview->times
                                                          : member->second;
                start = times.timeline_in;
                duration = times.duration;
                isPreview = true;
                valid = preview->valid;
            } else if (movePreview) {
                const auto linked =
                    std::find_if(movePreview->linked_moves.begin(),
                                 movePreview->linked_moves.end(),
                                 [&](const LinkedClipMove& move) {
                                     return move.clip_id == clip.id;
                                 });
                const bool anchor = movePreview->clip_id == clip.id;
                if (!anchor && linked == movePreview->linked_moves.end()) {
                    // No preview for this clip.
                } else {
                    start = linked != movePreview->linked_moves.end()
                                ? linked->timeline_in
                                : movePreview->timeline_in;
                    isPreview = true;
                    valid = movePreview->valid;
                    isMoving = true;
                    const Ulid targetTrack =
                        linked != movePreview->linked_moves.end()
                            ? linked->track_id
                            : movePreview->target_track_id;
                    const auto target =
                        std::find_if(tracks.begin(), tracks.end(),
                                     [&](const DocumentTrack* track) {
                                         return track->id == targetTrack;
                                     });
                    if (target != tracks.end())
                        displayTrackIndex = static_cast<size_t>(
                            std::distance(tracks.begin(), target));
                }
            }
            const double left = viewport.TimeToX(start);
            const double right = viewport.TimeToX(start.add(duration));
            if (std::max(left, right) < viewport.header_width ||
                std::min(left, right) > width)
                continue;
            TimelineClipRect rect{
                clip.id,
                clip.source_id,
                left,
                kTimelineRulerHeight +
                    displayTrackIndex * viewport.track_height + 3.0,
                right - left,
                viewport.track_height - 6.0,
                std::find(selectedClipIds.begin(), selectedClipIds.end(),
                          clip.id) != selectedClipIds.end(),
                isPreview,
                valid,
                isMoving,
                tracks[displayTrackIndex]->kind == "audio",
                std::nullopt};
            if (rect.audio) {
                rect.sync_drift = ClipSyncDrift(document, clip.id);
                if (rect.sync_drift && isMoving && movePreview &&
                    movePreview->linked_moves.size() <= 1)
                    rect.sync_drift =
                        rect.sync_drift->add(start.sub(clip.timeline_in));
            }
            if (isMoving)
                movingRects.push_back(std::move(rect));
            else
                result.push_back(std::move(rect));
        }
    }
    // Ghosts are deliberately last, matching NLE stacking conventions and
    // keeping an overwrite destination readable above affected clips.
    for (TimelineClipRect& rect : movingRects)
        result.push_back(std::move(rect));
    return result;
}
```

### src/TimelineView.h

```cpp
#pragma once

#include "Document.h"
#include "EditLog.h"

#include <optional>
#include <vector>

// The only boundary between exact timeline time and display coordinates.
// Coordinates are in logical points; backing-scale conversion belongs to the
// renderer and never affects edit rounding.
struct TimelineViewport {
    RationalTime view_start{0, 1};
    double pixels_per_second = 100.0;
    double track_height = 44.0;
    double header_width = 72.0;

    double TimeToX(RationalTime time) const;
    RationalTime XToTime(double x, int32_t rate) const;

    void ScrollByPixels(double delta_x, int32_t rate);
    void ZoomAroundX(double x, double factor, int32_t rate);
    void FitDuration(RationalTime duration, double width);
    std::vector<double> TickXs(double width) const;
};

constexpr double kTimelineRulerHeight = 24.0;
constexpr double kTimelineEdgeHitWidth = 6.0;
constexpr double kTimelineSnapDistance = 8.0;

enum class PlayheadResolution { Frame, Sample };

RationalTime QuantizePlayheadPosition(RationalTime position,
                                      PlayheadResolution resolution,
                                      MediaRate frameRate,
                                      int32_t sampleRate = 48000);

enum class TimelineHitEdge { None, Head, Tail };

struct TimelineHit {
    Ulid clip_id;
    Ulid track_id;
    TimelineHitEdge edge = TimelineHitEdge::None;
};

struct TimelineGapSelection {
    Ulid track_id;
    RationalTime start;
    RationalTime duration;
};

struct TimelineClipRect {
    Ulid clip_id;
    Ulid source_id;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    bool selected = false;
    bool preview = false;
    bool valid = true;
    bool moving = false;
    bool audio = false;
    std::optional<RationalTime> sync_drift;
};

std::vector<const DocumentTrack*> TimelineTracksInDisplayOrder(
    const Document& document);

std::optional<TimelineHit> HitTestTimeline(const Document& document,
                                           const TimelineViewport& viewport,
                                           double x, double y, double width);
std::optional<TimelineGapSelection> HitTestTimelineGap(
    const Document& document, const TimelineViewport& viewport, double x,
    double y, double width, int32_t timeRate);
std::vector<Ulid> LassoHitTestTimeline(const Document& document,
                                       const TimelineViewport& viewport,
                                       double startX, double startY,
                                       double endX, double endY, double width);
std::vector<Ulid> ExpandLinkedClipSelection(const Document& document,
                                            const std::vector<Ulid>& clipIds);

struct TimelineTrimPreview {
    Ulid clip_id;
    ExactClipTimes times;
    bool valid = false;
    Ulid link_group_id;
    std::vector<std::pair<Ulid, ExactClipTimes>> linked_times;
};

struct TimelineMovePreview {
    Ulid clip_id;
    Ulid target_track_id;
    RationalTime timeline_in;
    bool valid = false;
    Ulid link_group_id;
    std::vector<LinkedClipMove> linked_moves;
};

std::optional<RationalTime> ClipSyncDrift(const Document& document,
                                          const Ulid& clipId);

class TimelineInteraction {
public:
    TimelineInteraction(Document& document, EditLog& editLog,
                        TimelineViewport& viewport);

    void PointerDown(double x, double y, double width, int32_t timeRate);
    void PointerDrag(double x, double y, double width);
    bool PointerUp(EditError& error, std::string& message);
    void CancelDrag();

    const Ulid& SelectedClipId() const;
    const std::vector<Ulid>& SelectedClipIds() const;
    void SelectClip(const Ulid& clipId);
    void SelectClips(const std::vector<Ulid>& clipIds);
    const std::optional<TimelineGapSelection>& SelectedGap() const;
    void ClearGapSelection();
    const std::optional<RationalTime>& RequestedPlayhead() const;
    void ClearRequestedPlayhead();
    const std::optional<TimelineTrimPreview>& TrimPreview() const;
    const std::optional<TimelineMovePreview>& MovePreview() const;
    bool HasActiveDrag() const;
    void SetSnappingEnabled(bool enabled);
    bool SnappingEnabled() const;
    const std::optional<RationalTime>& SnapGuideTime() const;
    void SetLinkedSelectionEnabled(bool enabled);
    std::optional<Operation> PendingOperation() const;

private:
    void UpdatePreview(double x, double y, double width);
    RationalTime ConstrainTrimDelta(const DocumentClip& clip, TrimEdge edge,
                                    RationalTime delta) const;
    std::optional<RationalTime> FindSnapTime(
        RationalTime candidate, const DocumentTrack& track,
        const Ulid& excludedClipId, bool includeSyncTarget = false) const;

    Document& document_;
    EditLog& edit_log_;
    TimelineViewport& viewport_;
    Ulid selected_clip_id_;
    std::vector<Ulid> selected_clip_ids_;
    std::optional<TimelineGapSelection> selected_gap_;
    std::optional<RationalTime> requested_playhead_;
    std::optional<TimelineTrimPreview> preview_;
    std::optional<TimelineMovePreview> move_preview_;
    std::optional<TimelineHit> drag_hit_;
    std::optional<RationalTime> drag_start_time_;
    int32_t drag_rate_ = 1;
    double drag_x_ = 0.0;
    bool snapping_enabled_ = true;
    bool linked_selection_enabled_ = true;
    std::optional<RationalTime> snap_guide_time_;
};

std::vector<TimelineClipRect> VisibleTimelineClips(
    const Document& document, const TimelineViewport& viewport, double width,
    const std::vector<Ulid>& selectedClipIds,
    const std::optional<TimelineTrimPreview>& preview,
    const std::optional<TimelineMovePreview>& movePreview = std::nullopt);
```

### src/Ulid.cc

```cpp
#include "Ulid.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>

namespace {

constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

int DecodeCharacter(char character) {
    if (character >= 'a' && character <= 'z') {
        character = static_cast<char>(character - 'a' + 'A');
    }
    for (int index = 0; index < 32; ++index) {
        if (kAlphabet[index] == character) {
            return index;
        }
    }
    return -1;
}

}  // namespace

Ulid GenerateUlid() {
    static std::mutex mutex;
    static std::mt19937_64 random(std::random_device{}());
    static uint64_t previousMilliseconds = 0;
    static std::array<uint8_t, 10> randomness = {};

    std::lock_guard<std::mutex> lock(mutex);
    uint64_t milliseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    if (milliseconds < previousMilliseconds) {
        milliseconds = previousMilliseconds;
    }
    if (milliseconds == previousMilliseconds) {
        for (int index = 9; index >= 0; --index) {
            if (++randomness[static_cast<size_t>(index)] != 0) {
                break;
            }
        }
    } else {
        for (size_t index = 0; index < randomness.size(); index += 8) {
            const uint64_t bits = random();
            for (size_t byte = 0; byte < 8 && index + byte < randomness.size();
                 ++byte) {
                randomness[index + byte] =
                    static_cast<uint8_t>(bits >> (byte * 8));
            }
        }
        previousMilliseconds = milliseconds;
    }

    // ULID is a 128-bit value encoded as 26 Crockford base32 characters.
    // The first character has only three significant bits.
    std::array<uint8_t, 16> bytes = {};
    for (int index = 5; index >= 0; --index) {
        bytes[static_cast<size_t>(index)] = static_cast<uint8_t>(milliseconds);
        milliseconds >>= 8;
    }
    for (size_t index = 0; index < randomness.size(); ++index) {
        bytes[index + 6] = randomness[index];
    }

    std::string result(26, '0');
    uint32_t buffer = 0;
    int bitCount = 2;  // two leading zero bits make 130 encoded bits
    size_t byteIndex = 0;
    for (size_t output = 0; output < result.size(); ++output) {
        while (bitCount < 5) {
            buffer = (buffer << 8) | bytes[byteIndex++];
            bitCount += 8;
        }
        bitCount -= 5;
        result[output] = kAlphabet[(buffer >> bitCount) & 31U];
    }
    return result;
}

bool IsValidUlid(const Ulid& value) {
    if (value.size() != 26 || DecodeCharacter(value[0]) > 7) {
        return false;
    }
    for (char character : value) {
        if (DecodeCharacter(character) < 0) {
            return false;
        }
    }
    return true;
}
```

### src/Ulid.h

```cpp
#pragma once

#include <string>

using Ulid = std::string;

Ulid GenerateUlid();
bool IsValidUlid(const Ulid& value);
```

### src/main.mm

```objectivec
#import <AppKit/AppKit.h>

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "AudioPlayback.h"
#include "Cli.h"
#include "DecodeWorker.h"
#include "Document.h"
#include "EditLog.h"
#include "FrameCache.h"
#include "Ingest.h"
#include "PerformanceMetrics.h"
#include "Renderer.h"
#include "Timeline.h"
#include "TimelineView.h"

namespace {

constexpr size_t kGlobalCacheBudget = 2000000000ULL;  // 2.0 GB, all sources.
constexpr double kAddTrackRowHeight = 24.0;
NSPasteboardType const kCutmachineMediaPasteboardType =
    @"com.cutmachine.library-media";

enum class TimelineTool { Select, Hand, Zoom, Cut };

NSString* ToolName(TimelineTool tool) {
    switch (tool) {
        case TimelineTool::Select:
            return @"Sélection (V)";
        case TimelineTool::Hand:
            return @"Main (H)";
        case TimelineTool::Zoom:
            return @"Zoom (Z)";
        case TimelineTool::Cut:
            return @"Lame (C/B)";
    }
}

struct ResolvedSlot {
    bool active = false;
    Ulid sourceId;
    int64_t frame = -1;
};

struct RenderedSlot {
    bool active = false;
    Ulid sourceId;
    int64_t frame = -1;

    bool operator==(const RenderedSlot& other) const {
        return active == other.active && sourceId == other.sourceId &&
               frame == other.frame;
    }
};

struct AppState {
    Document document;
    EditLog editLog;
    TimelineViewport viewport;
    std::unique_ptr<TimelineInteraction> interaction;
    std::unique_ptr<Timeline> timeline;
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<FrameCache> frameCache;
    std::unique_ptr<PerformanceMetrics> performanceMetrics;
    std::unique_ptr<AudioPlayback> audioPlayback;
    std::map<Ulid, std::unique_ptr<DecodeWorker>> workers;
    // Runtime probe cache. UI metadata never mutates the edit document.
    std::map<Ulid, LibraryMedia> mediaMetadata;
    std::vector<Ulid> videoTrackIds;
    std::vector<ResolvedSlot> requested;
    std::vector<RenderedSlot> rendered;
    RationalTime duration{0, 1};
    RationalTime requestedPosition{0, 1};
    PlayheadResolution playheadResolution = PlayheadResolution::Frame;
    bool overlayDirty = true;
    TimelineTool tool = TimelineTool::Select;
    bool spaceHand = false;
    bool navigationDragging = false;
    bool scrubDragging = false;
    bool editDragging = false;
    bool lassoCandidate = false;
    bool lassoDragging = false;
    bool linkedSelection = true;
    bool linkedSelectionGesture = true;
    double lassoStartX = 0.0;
    double lassoStartY = 0.0;
    double lassoCurrentX = 0.0;
    double lassoCurrentY = 0.0;
    bool spaceUsedForPan = false;
    double navigationLastX = 0.0;
    int playbackDirection = 0;
    RationalTime playbackAnchor{0, 1};
    std::chrono::steady_clock::time_point playbackStarted;
    std::optional<double> cutPreviewX;
    std::optional<double> cutPreviewY;
    bool sourceMonitor = false;
    Ulid sourceMonitorId;
    RationalTime sourceMonitorPosition{0, 1};
    Ulid contextClipId;
    Ulid contextTrackId;
    RationalTime contextTime{0, 1};
    std::optional<TimelineGapSelection> contextGap;
};

std::array<float, 3> ClipColor(const Ulid& sourceId, bool audio) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char byte : sourceId) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    const float variation = static_cast<float>(hash & 0xff) / 2550.0f;
    if (audio)
        return {0.14f + variation * 0.25f, 0.48f + variation,
                0.27f + variation * 0.45f};
    return {0.18f + variation * 0.35f, 0.31f + variation * 0.55f,
            0.62f + variation};
}

NSString* TimeString(const RationalTime& time) {
    return [NSString stringWithFormat:@"%lld/%d",
                                      static_cast<long long>(time.value),
                                      time.rate];
}

std::string SyncDriftLabel(const RationalTime& drift, MediaRate frameRate) {
    const int64_t frames = drift.to_frames(frameRate.num, frameRate.den);
    const RationalTime frameQuantized{
        frames * static_cast<int64_t>(frameRate.den), frameRate.num};
    int64_t value = frames;
    const char* unit = "f";
    if (frameQuantized != drift) {
        value = drift.to_frames(48000);
        unit = "smp";
    }
    return std::string(value > 0 ? "+" : "") + std::to_string(value) + unit;
}

}  // namespace

@protocol TimelineEventTarget <NSObject>
- (void)timelineMouseDown:(NSEvent*)event;
- (void)timelineMouseDragged:(NSEvent*)event;
- (void)timelineMouseUp:(NSEvent*)event;
- (void)timelineMouseMoved:(NSEvent*)event;
- (void)timelineScroll:(NSEvent*)event;
- (BOOL)timelineKeyDown:(NSEvent*)event;
- (void)timelineKeyUp:(NSEvent*)event;
- (BOOL)timelineDropMedia:(NSString*)mediaId atViewPoint:(NSPoint)point;
- (NSMenu*)timelineMenuForEvent:(NSEvent*)event;
@end

@interface TimelineMetalView : NSView
@property(nonatomic, weak) id<TimelineEventTarget> eventTarget;
@end

@implementation TimelineMetalView
- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame]))
        [self registerForDraggedTypes:@[ kCutmachineMediaPasteboardType ]];
    return self;
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (void)mouseDown:(NSEvent*)event {
    [self.window makeFirstResponder:self];
    [self.eventTarget timelineMouseDown:event];
}
- (void)mouseDragged:(NSEvent*)event {
    [self.eventTarget timelineMouseDragged:event];
}
- (void)mouseUp:(NSEvent*)event {
    [self.eventTarget timelineMouseUp:event];
}
- (void)mouseMoved:(NSEvent*)event {
    [self.eventTarget timelineMouseMoved:event];
}
- (void)scrollWheel:(NSEvent*)event {
    [self.eventTarget timelineScroll:event];
}
- (void)keyDown:(NSEvent*)event {
    if (![self.eventTarget timelineKeyDown:event]) [super keyDown:event];
}
- (void)keyUp:(NSEvent*)event {
    [self.eventTarget timelineKeyUp:event];
}
- (NSMenu*)menuForEvent:(NSEvent*)event {
    return [self.eventTarget timelineMenuForEvent:event];
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    return [[sender draggingPasteboard]
               availableTypeFromArray:@[ kCutmachineMediaPasteboardType ]]
               ? NSDragOperationCopy
               : NSDragOperationNone;
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSString* mediaId = [[sender draggingPasteboard]
        stringForType:kCutmachineMediaPasteboardType];
    if (!mediaId) return NO;
    const NSPoint point = [self convertPoint:sender.draggingLocation
                                    fromView:nil];
    return [self.eventTarget timelineDropMedia:mediaId atViewPoint:point];
}
@end

@interface ContextOutlineView : NSOutlineView
@end
@implementation ContextOutlineView
- (NSMenu*)menuForEvent:(NSEvent*)event {
    const NSInteger row = [self
        rowAtPoint:[self convertPoint:event.locationInWindow fromView:nil]];
    if (row >= 0)
        [self selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
            byExtendingSelection:NO];
    return [super menuForEvent:event];
}
@end

@interface ContextTableView : NSTableView
@end
@implementation ContextTableView
- (NSMenu*)menuForEvent:(NSEvent*)event {
    const NSInteger row = [self
        rowAtPoint:[self convertPoint:event.locationInWindow fromView:nil]];
    if (row >= 0)
        [self selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
            byExtendingSelection:NO];
    return [super menuForEvent:event];
}
@end

@interface ContextCollectionView : NSCollectionView
@end
@implementation ContextCollectionView
- (NSMenu*)menuForEvent:(NSEvent*)event {
    NSIndexPath* indexPath =
        [self indexPathForItemAtPoint:[self convertPoint:event.locationInWindow
                                                fromView:nil]];
    if (indexPath) self.selectionIndexPaths = [NSSet setWithObject:indexPath];
    return [super menuForEvent:event];
}
@end

@interface MediaIconItem : NSCollectionViewItem
@end

@implementation MediaIconItem
- (void)loadView {
    self.view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 132, 112)];
    NSImageView* image =
        [[NSImageView alloc] initWithFrame:NSMakeRect(14, 27, 104, 76)];
    image.imageScaling = NSImageScaleProportionallyUpOrDown;
    image.wantsLayer = YES;
    image.layer.backgroundColor =
        [NSColor colorWithWhite:0.13 alpha:1.0].CGColor;
    image.layer.cornerRadius = 5.0;
    NSTextField* label =
        [[NSTextField alloc] initWithFrame:NSMakeRect(4, 4, 124, 19)];
    label.editable = NO;
    label.selectable = NO;
    label.bezeled = NO;
    label.drawsBackground = NO;
    label.alignment = NSTextAlignmentCenter;
    label.font = [NSFont systemFontOfSize:11.0];
    label.lineBreakMode = NSLineBreakByTruncatingMiddle;
    self.imageView = image;
    self.textField = label;
    [self.view addSubview:image];
    [self.view addSubview:label];
}
- (void)setSelected:(BOOL)selected {
    [super setSelected:selected];
    self.view.layer.backgroundColor =
        selected ? [NSColor selectedContentBackgroundColor].CGColor
                 : NSColor.clearColor.CGColor;
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate,
                                   NSWindowDelegate,
                                   TimelineEventTarget,
                                   NSOutlineViewDataSource,
                                   NSOutlineViewDelegate,
                                   NSTableViewDataSource,
                                   NSTableViewDelegate,
                                   NSCollectionViewDataSource,
                                   NSCollectionViewDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) TimelineMetalView* metalView;
@property(nonatomic, strong) NSView* mediaPanel;
@property(nonatomic, strong) NSPopUpButton* binPopup;
@property(nonatomic, strong) NSPopUpButton* mediaPopup;
@property(nonatomic, strong) NSTextField* binSummaryLabel;
@property(nonatomic, strong) NSButton* assignMediaButton;
@property(nonatomic, strong) NSOutlineView* binOutline;
@property(nonatomic, strong) NSTableView* mediaTable;
@property(nonatomic, strong) NSSearchField* mediaSearchField;
@property(nonatomic, strong) NSMutableArray<NSString*>* visibleMediaIds;
@property(nonatomic, copy) NSString* selectedBinId;
@property(nonatomic, strong) NSCollectionView* mediaCollection;
@property(nonatomic, strong) NSScrollView* mediaListScroll;
@property(nonatomic, strong) NSScrollView* mediaIconScroll;
@property(nonatomic, strong) NSSegmentedControl* mediaViewToggle;
@property(nonatomic, strong) NSButton* sourceMonitorButton;
@property(nonatomic, strong) NSTextField* infoLabel;
@property(nonatomic, strong) NSButton* detachAudioButton;
@property(nonatomic, strong) NSButton* linkedSelectionButton;
@property(nonatomic, strong) NSTimer* displayTimer;
@property(nonatomic, copy) NSString* documentPath;
@property(nonatomic, assign) AppState* state;
@end

@implementation AppDelegate

- (NSMenuItem*)menuItem:(NSString*)title action:(SEL)action key:(NSString*)key {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title
                                                  action:action
                                           keyEquivalent:key ?: @""];
    item.target = self;
    return item;
}

- (void)installApplicationMenus {
    NSMenu* bar = [[NSMenu alloc] initWithTitle:@"Main"];
    NSMenuItem* appRoot = [[NSMenuItem alloc] initWithTitle:@"CUTMACHINE"
                                                     action:nil
                                              keyEquivalent:@""];
    NSMenu* app = [[NSMenu alloc] initWithTitle:@"CUTMACHINE"];
    [app addItemWithTitle:@"À propos de CUTMACHINE"
                   action:@selector(orderFrontStandardAboutPanel:)
            keyEquivalent:@""];
    [app addItem:NSMenuItem.separatorItem];
    [app addItemWithTitle:@"Quitter CUTMACHINE"
                   action:@selector(terminate:)
            keyEquivalent:@"q"];
    appRoot.submenu = app;
    [bar addItem:appRoot];

    NSMenuItem* editRoot = [[NSMenuItem alloc] initWithTitle:@"Édition"
                                                      action:nil
                                               keyEquivalent:@""];
    NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Édition"];
    [edit addItem:[self menuItem:@"Annuler"
                          action:@selector(menuUndo:)
                             key:@"z"]];
    NSMenuItem* redo = [self menuItem:@"Rétablir"
                               action:@selector(menuRedo:)
                                  key:@"z"];
    redo.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [edit addItem:redo];
    [edit addItem:NSMenuItem.separatorItem];
    [edit addItem:[self menuItem:@"Supprimer la sélection"
                          action:@selector(menuDeleteSelection:)
                             key:@"\b"]];
    editRoot.submenu = edit;
    [bar addItem:editRoot];

    NSMenuItem* clipRoot = [[NSMenuItem alloc] initWithTitle:@"Clip"
                                                      action:nil
                                               keyEquivalent:@""];
    NSMenu* clip = [[NSMenu alloc] initWithTitle:@"Clip"];
    [clip addItem:[self menuItem:@"Ouvrir dans le moniteur source"
                          action:@selector(openSelectedMediaInSourceMonitor:)
                             key:@""]];
    [clip addItem:[self menuItem:@"Séparer l’audio"
                          action:@selector(detachAudioButtonPressed:)
                             key:@"u"]];
    [clip addItem:[self menuItem:@"Couper au playhead"
                          action:@selector(menuCutSelectedAtPlayhead:)
                             key:@""]];
    clipRoot.submenu = clip;
    [bar addItem:clipRoot];

    NSMenuItem* colorRoot = [[NSMenuItem alloc] initWithTitle:@"Couleur"
                                                       action:nil
                                                keyEquivalent:@""];
    NSMenu* color = [[NSMenu alloc] initWithTitle:@"Couleur"];
    [color addItem:[self menuItem:@"Preset Sony S-Log3 → Rec.2020 HLG"
                               action:@selector(applySonyColorPreset:)
                                  key:@""]];
    [color addItem:[self menuItem:@"Configurer la gestion colorimétrique…"
                               action:@selector(configureColorManagement:)
                                  key:@""]];
    [color addItem:NSMenuItem.separatorItem];
    [color addItem:[self menuItem:@"Désactiver la gestion colorimétrique"
                               action:@selector(disableColorManagement:)
                                  key:@""]];
    colorRoot.submenu = color;
    [bar addItem:colorRoot];

    NSMenuItem* timelineRoot = [[NSMenuItem alloc] initWithTitle:@"Timeline"
                                                          action:nil
                                                   keyEquivalent:@""];
    NSMenu* timeline = [[NSMenu alloc] initWithTitle:@"Timeline"];
    [timeline addItem:[self menuItem:@"Outil Sélection"
                              action:@selector(menuSelectTool:)
                                 key:@"v"]];
    [timeline addItem:[self menuItem:@"Outil Lame"
                              action:@selector(menuCutTool:)
                                 key:@"c"]];
    [timeline addItem:NSMenuItem.separatorItem];
    [timeline addItem:[self menuItem:@"Magnétisme"
                              action:@selector(menuToggleSnapping:)
                                 key:@"n"]];
    [timeline addItem:[self menuItem:@"Sélection liée"
                              action:@selector(menuToggleLinkedSelection:)
                                 key:@""]];
    [timeline addItem:[self menuItem:@"Cadrer toute la timeline"
                              action:@selector(menuFitTimeline:)
                                 key:@"f"]];
    [timeline addItem:NSMenuItem.separatorItem];
    [timeline addItem:[self menuItem:@"Ajouter une piste vidéo"
                              action:@selector(menuAddVideoTrack:)
                                 key:@""]];
    [timeline addItem:[self menuItem:@"Ajouter une piste audio"
                              action:@selector(menuAddAudioTrack:)
                                 key:@""]];
    timelineRoot.submenu = timeline;
    [bar addItem:timelineRoot];

    NSMenuItem* playbackRoot = [[NSMenuItem alloc] initWithTitle:@"Lecture"
                                                          action:nil
                                                   keyEquivalent:@""];
    NSMenu* playback = [[NSMenu alloc] initWithTitle:@"Lecture"];
    [playback addItem:[self menuItem:@"Lecture / Pause"
                              action:@selector(menuPlayPause:)
                                 key:@" "]];
    [playback addItem:[self menuItem:@"Lecture arrière"
                              action:@selector(menuPlayReverse:)
                                 key:@"j"]];
    [playback addItem:[self menuItem:@"Arrêt"
                              action:@selector(menuStop:)
                                 key:@"k"]];
    [playback addItem:[self menuItem:@"Lecture avant"
                              action:@selector(menuPlayForward:)
                                 key:@"l"]];
    playbackRoot.submenu = playback;
    [bar addItem:playbackRoot];
    NSApp.mainMenu = bar;
}

- (BOOL)commitColorSettings:(const ColorManagementSettings&)settings {
    const ColorManagementSettings previous =
        self.state->document.color_management;
    self.state->document.color_management = settings;
    std::string message;
    if (![self persistEdits:message]) {
        self.state->document.color_management = previous;
        std::fprintf(stderr, "Unable to persist color settings: %s\n",
                     message.c_str());
        NSBeep();
        return NO;
    }
    self.state->overlayDirty = true;
    return YES;
}

- (void)applySonyColorPreset:(id)sender {
    (void)sender;
    ColorManagementSettings settings;
    settings.enabled = true;
    settings.input_gamut = "sony_sgamut3_cine";
    settings.input_transfer = "sony_slog3";
    settings.input_ycbcr_matrix = "bt709";
    settings.input_range = "full";
    settings.working_gamut = "acescct";
    settings.output_gamut = "rec2020";
    settings.output_transfer = "hlg";
    [self commitColorSettings:settings];
}

- (void)disableColorManagement:(id)sender {
    (void)sender;
    ColorManagementSettings settings = self.state->document.color_management;
    settings.enabled = false;
    [self commitColorSettings:settings];
}

- (void)configureColorManagement:(id)sender {
    (void)sender;
    const ColorManagementSettings& current =
        self.state->document.color_management;
    NSAlert* alert = [NSAlert new];
    alert.messageText = @"Gestion colorimétrique du projet";
    alert.informativeText =
        @"Les transformations sont calculées en linéaire dans l’espace de "
         "composition AP1, avec ACEScct pour le grading. Pour les rushes "
         "Sony habituels, utilisez S-Gamut3.Cine / S-Log3 en plage Full, "
         "puis Rec.2020 / HLG.";
    [alert addButtonWithTitle:@"Appliquer"];
    [alert addButtonWithTitle:@"Annuler"];

    NSView* accessory = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 500, 264)];
    NSButton* enabled = [NSButton checkboxWithTitle:@"Activer pour ce projet"
                                             target:nil
                                             action:nil];
    enabled.frame = NSMakeRect(0, 236, 500, 24);
    enabled.state = current.enabled ? NSControlStateValueOn
                                    : NSControlStateValueOff;
    [accessory addSubview:enabled];

    CGFloat y = 202.0;
    auto addPopup = [&](NSString* labelText,
                        std::initializer_list<std::pair<NSString*, NSString*>>
                            choices,
                        const std::string& selected) {
        NSTextField* label = [NSTextField labelWithString:labelText];
        label.frame = NSMakeRect(0, y + 3.0, 175, 20);
        label.alignment = NSTextAlignmentRight;
        [accessory addSubview:label];
        NSPopUpButton* popup = [[NSPopUpButton alloc]
            initWithFrame:NSMakeRect(188, y, 300, 26)
                pullsDown:NO];
        for (const auto& choice : choices) {
            [popup addItemWithTitle:choice.first];
            popup.lastItem.representedObject = choice.second;
            if ([choice.second isEqualToString:
                    [NSString stringWithUTF8String:selected.c_str()]])
                [popup selectItem:popup.lastItem];
        }
        [accessory addSubview:popup];
        y -= 32.0;
        return popup;
    };
    NSPopUpButton* inputGamut = addPopup(
        @"Gamut d’entrée :", {{@"Sony S-Gamut3.Cine", @"sony_sgamut3_cine"},
                              {@"Sony S-Gamut3", @"sony_sgamut3"},
                              {@"Rec.709", @"rec709"},
                              {@"Rec.2020", @"rec2020"}},
        current.input_gamut);
    NSPopUpButton* inputTransfer = addPopup(
        @"Courbe d’entrée :", {{@"Sony S-Log3", @"sony_slog3"},
                               {@"Rec.709", @"rec709"},
                               {@"Linéaire", @"linear"}},
        current.input_transfer);
    NSPopUpButton* ycbcr = addPopup(
        @"Matrice YCbCr :", {{@"Auto (métadonnées)", @"auto"},
                             {@"BT.709", @"bt709"},
                             {@"BT.2020 non constante", @"bt2020_ncl"}},
        current.input_ycbcr_matrix);
    NSPopUpButton* inputRange = addPopup(
        @"Plage du signal :", {{@"Auto (métadonnées)", @"auto"},
                               {@"Full / Extended", @"full"},
                               {@"Legal / Limited", @"limited"}},
        current.input_range);
    NSPopUpButton* working = addPopup(
        @"Espace de travail :", {{@"ACEScct (AP1)", @"acescct"},
                                  {@"Rec.2020 linéaire", @"rec2020"},
                                  {@"Rec.709 linéaire", @"rec709"}},
        current.working_gamut);
    NSPopUpButton* outputGamut = addPopup(
        @"Gamut de sortie :", {{@"Rec.2020", @"rec2020"},
                               {@"Rec.709", @"rec709"}},
        current.output_gamut);
    NSPopUpButton* outputTransfer = addPopup(
        @"Courbe de sortie :", {{@"HLG", @"hlg"},
                                {@"Rec.709", @"rec709"}},
        current.output_transfer);
    alert.accessoryView = accessory;
    if ([alert runModal] != NSAlertFirstButtonReturn) return;

    auto value = [](NSPopUpButton* popup) {
        NSString* string = popup.selectedItem.representedObject;
        return std::string(string.UTF8String ?: "");
    };
    ColorManagementSettings settings;
    settings.enabled = enabled.state == NSControlStateValueOn;
    settings.input_gamut = value(inputGamut);
    settings.input_transfer = value(inputTransfer);
    settings.input_ycbcr_matrix = value(ycbcr);
    settings.input_range = value(inputRange);
    settings.working_gamut = value(working);
    settings.output_gamut = value(outputGamut);
    settings.output_transfer = value(outputTransfer);
    if (settings.output_transfer == "hlg" &&
        settings.output_gamut != "rec2020") {
        NSAlert* invalid = [NSAlert new];
        invalid.messageText = @"Configuration incompatible";
        invalid.informativeText =
            @"La sortie HLG doit utiliser le gamut Rec.2020.";
        [invalid runModal];
        return;
    }
    [self commitColorSettings:settings];
}

- (instancetype)initWithDocumentPath:(NSString*)documentPath {
    if ((self = [super init])) {
        _documentPath = [documentPath copy];
        _state = new AppState();
    }
    return self;
}

- (BOOL)loadDocumentAndSources {
    const std::string documentPath(self.documentPath.UTF8String ?: "");
    std::string error;
    if (!Document::Load(documentPath, self.state->document, error)) {
        std::fprintf(stderr, "Unable to load document: %s\n", error.c_str());
        return NO;
    }
    const std::string logPath = EditLogPathForDocument(documentPath);
    std::error_code logExistsError;
    EditError editError = EditError::None;
    if (std::filesystem::exists(logPath, logExistsError) &&
        !EditLog::Load(logPath, self.state->editLog, editError, error)) {
        std::fprintf(stderr, "Unable to load edit log: %s\n", error.c_str());
        return NO;
    }
    if (logExistsError) {
        std::fprintf(stderr, "Unable to inspect edit log: %s\n",
                     logExistsError.message().c_str());
        return NO;
    }
    self.state->viewport.view_start = {0, 1};
    self.state->viewport.pixels_per_second = 100.0;
    self.state->viewport.track_height = 44.0;
    self.state->viewport.header_width = 96.0;
    self.state->interaction = std::make_unique<TimelineInteraction>(
        self.state->document, self.state->editLog, self.state->viewport);
    self.state->timeline = std::make_unique<Timeline>(self.state->document);
    try {
        self.state->duration = self.state->timeline->Duration();
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "Invalid timeline duration: %s\n",
                     exception.what());
        return NO;
    }
    self.state->frameCache = std::make_unique<FrameCache>(kGlobalCacheBudget);
    self.state->performanceMetrics = std::make_unique<PerformanceMetrics>();
    const std::filesystem::path baseDirectory =
        std::filesystem::absolute(std::filesystem::path(documentPath))
            .parent_path();
    for (const DocumentSource& source : self.state->document.sources) {
        const LibraryMedia* media =
            self.state->document.FindLibraryMedia(source.id);
        std::filesystem::path mediaPath(source.path);
        if (mediaPath.is_relative()) mediaPath = baseDirectory / mediaPath;
        LibraryMedia detected;
        detected.id = source.id;
        detected.path = media ? media->path : source.path;
        detected.filename = mediaPath.filename().string();
        std::string probeError;
        if (ProbeMediaMetadata(mediaPath.lexically_normal().string(), detected,
                               probeError)) {
            self.state->mediaMetadata[source.id] = detected;
            std::fprintf(
                stderr,
                "Media %s: %dx%d, %s/%s, range=%s matrix=%s transfer=%s "
                "primaries=%s, rotation %d degrees, %s\n",
                detected.filename.c_str(), detected.width, detected.height,
                detected.codec.c_str(), detected.pixel_format.c_str(),
                detected.color_range.c_str(), detected.color_space.c_str(),
                detected.color_transfer.c_str(),
                detected.color_primaries.c_str(), detected.rotation_degrees,
                detected.orientation.c_str());
        } else {
            std::fprintf(stderr, "Metadata probe failed for %s: %s\n",
                         mediaPath.string().c_str(), probeError.c_str());
            if (media && media->metadata_complete)
                self.state->mediaMetadata[source.id] = *media;
        }
    }
    if (![self separateEmbeddedAudioByDefault:error]) {
        std::fprintf(stderr, "Unable to separate embedded audio: %s\n",
                     error.c_str());
        return NO;
    }
    self.state->audioPlayback = std::make_unique<AudioPlayback>();
    if (!self.state->audioPlayback->Open(self.state->document,
                                         baseDirectory.string(), error)) {
        std::fprintf(stderr, "Unable to initialize audio: %s\n", error.c_str());
        return NO;
    }
    for (const DocumentSource& source : self.state->document.sources) {
        std::filesystem::path mediaPath(source.path);
        if (mediaPath.is_relative()) {
            mediaPath = baseDirectory / mediaPath;
        }
        auto worker =
            std::make_unique<DecodeWorker>(source.id, *self.state->frameCache,
                                           *self.state->performanceMetrics);
        if (!worker->Open(mediaPath.lexically_normal().string(), 5)) {
            std::fprintf(stderr, "Unable to open source %s at %s\n",
                         source.id.c_str(), mediaPath.string().c_str());
            return NO;
        }
        if (static_cast<int64_t>(worker->FrameRateNumerator()) *
                source.rate.den !=
            static_cast<int64_t>(source.rate.num) *
                worker->FrameRateDenominator()) {
            std::fprintf(
                stderr, "Source %s declares rate %d/%d but media is %d/%d\n",
                source.id.c_str(), source.rate.num, source.rate.den,
                worker->FrameRateNumerator(), worker->FrameRateDenominator());
            return NO;
        }
        const int64_t declaredFrames =
            source.duration.to_frames(source.rate.num, source.rate.den);
        if (declaredFrames > worker->FrameCount()) {
            std::fprintf(
                stderr,
                "Source %s declares %lld frames but media exposes %lld\n",
                source.id.c_str(), static_cast<long long>(declaredFrames),
                static_cast<long long>(worker->FrameCount()));
            return NO;
        }
        self.state->workers.emplace(source.id, std::move(worker));
    }

    [self rebuildVideoTrackIds];
    return YES;
}

- (BOOL)separateEmbeddedAudioByDefault:(std::string&)message {
    std::vector<Ulid> videoClipIds;
    for (const DocumentTrack& track : self.state->document.tracks) {
        if (track.kind != "video") continue;
        for (const DocumentClip& clip : track.clips) {
            if (!clip.include_audio) continue;
            const auto detected =
                self.state->mediaMetadata.find(clip.source_id);
            const LibraryMedia* media =
                detected == self.state->mediaMetadata.end()
                    ? self.state->document.FindLibraryMedia(clip.source_id)
                    : &detected->second;
            if (media && media->metadata_complete && media->has_audio)
                videoClipIds.push_back(clip.id);
        }
    }
    EditError error = EditError::None;
    for (const Ulid& videoClipId : videoClipIds) {
        const Ulid audioClipId = GenerateUlid();
        Ulid targetTrackId;
        for (const DocumentTrack* track :
             TimelineTracksInDisplayOrder(self.state->document)) {
            if (track->kind != "audio") continue;
            Document candidate = self.state->document;
            Operation probe =
                DetachAudioOperation{videoClipId, track->id, audioClipId, {}};
            Operation inverse = RemoveClipOperation{};
            EditError probeError = EditError::None;
            std::string probeMessage;
            if (ApplyOperation(candidate, probe, inverse, probeError,
                               probeMessage)) {
                targetTrackId = track->id;
                break;
            }
        }
        if (targetTrackId.empty()) {
            int32_t index = 0;
            for (const DocumentTrack& track : self.state->document.tracks)
                index = std::max(index, track.index + 1);
            targetTrackId = GenerateUlid();
            if (!self.state->editLog.Apply(
                    self.state->document,
                    Operation{AddTrackOperation{targetTrackId, "audio", index}},
                    error, message))
                return NO;
        }
        if (!self.state->editLog.Apply(
                self.state->document,
                Operation{DetachAudioOperation{
                    videoClipId, targetTrackId, audioClipId, {}}},
                error, message))
            return NO;
    }
    size_t migratedLinks = 0;
    std::vector<Ulid> claimedAudio;
    struct LegacyPair {
        Ulid video;
        Ulid audio;
        Ulid group;
    };
    std::vector<LegacyPair> legacyPairs;
    for (const DocumentTrack& videoTrack : self.state->document.tracks) {
        if (videoTrack.kind != "video") continue;
        for (const DocumentClip& video : videoTrack.clips) {
            if (video.include_audio || !video.sync_anchor_clip_id.empty())
                continue;
            const DocumentClip* match = nullptr;
            for (const DocumentTrack& audioTrack :
                 self.state->document.tracks) {
                if (audioTrack.kind != "audio") continue;
                for (const DocumentClip& audio : audioTrack.clips) {
                    if (!audio.sync_anchor_clip_id.empty() ||
                        std::find(claimedAudio.begin(), claimedAudio.end(),
                                  audio.id) != claimedAudio.end() ||
                        audio.source_id != video.source_id ||
                        audio.source_in != video.source_in ||
                        audio.duration != video.duration ||
                        audio.timeline_in != video.timeline_in)
                        continue;
                    match = &audio;
                    break;
                }
                if (match) break;
            }
            if (!match) continue;
            const Ulid audioId = match->id;
            const Ulid groupId =
                !video.link_group_id.empty()
                    ? video.link_group_id
                    : (!match->link_group_id.empty() ? match->link_group_id
                                                     : audioId);
            legacyPairs.push_back({video.id, audioId, groupId});
            claimedAudio.push_back(audioId);
        }
    }
    for (const auto& pair : legacyPairs) {
        if (!self.state->editLog.Apply(
                self.state->document,
                Operation{SetClipLinkOperation{pair.video, pair.audio,
                                               pair.group, pair.group}},
                error, message))
            return NO;
        ++migratedLinks;
    }
    if (!videoClipIds.empty() || migratedLinks > 0) {
        if (![self persistEdits:message]) return NO;
        std::fprintf(
            stderr,
            "Separated %zu embedded audio clip(s), migrated %zu A/V link(s)\n",
            videoClipIds.size(), migratedLinks);
    }
    return YES;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [self installApplicationMenus];

    if (![self loadDocumentAndSources]) {
        [NSApp terminate:nil];
        return;
    }

    const NSRect windowRect = NSMakeRect(0.0, 0.0, 1600.0, 960.0);
    self.window =
        [[NSWindow alloc] initWithContentRect:windowRect
                                    styleMask:(NSWindowStyleMaskTitled |
                                               NSWindowStyleMaskClosable |
                                               NSWindowStyleMaskResizable)
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    self.window.title = @"CUTMACHINE — timeline scrub";
    self.window.delegate = self;
    self.window.acceptsMouseMovedEvents = YES;

    NSView* content = [[NSView alloc] initWithFrame:windowRect];
    self.window.contentView = content;
    constexpr double mediaPanelWidth = 320.0;
    self.mediaPanel =
        [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, mediaPanelWidth,
                                                 windowRect.size.height)];
    self.mediaPanel.autoresizingMask = NSViewHeightSizable;
    self.mediaPanel.wantsLayer = YES;
    self.mediaPanel.layer.backgroundColor =
        [NSColor colorWithWhite:0.075 alpha:1.0].CGColor;
    [content addSubview:self.mediaPanel];

    NSTextField* libraryTitle =
        [NSTextField labelWithString:@"MÉDIATHÈQUE / CHUTIERS"];
    libraryTitle.frame = NSMakeRect(14.0, windowRect.size.height - 34.0,
                                    mediaPanelWidth - 28.0, 18.0);
    libraryTitle.autoresizingMask = NSViewMinYMargin;
    libraryTitle.font = [NSFont systemFontOfSize:11.0
                                          weight:NSFontWeightSemibold];
    libraryTitle.textColor = NSColor.secondaryLabelColor;
    [self.mediaPanel addSubview:libraryTitle];

    NSButton* addBinButton =
        [NSButton buttonWithTitle:@"+ Chutier"
                           target:self
                           action:@selector(createBinPressed:)];
    addBinButton.frame =
        NSMakeRect(12.0, windowRect.size.height - 70.0, 142.0, 28.0);
    addBinButton.autoresizingMask = NSViewMinYMargin;
    addBinButton.bezelStyle = NSBezelStyleRounded;
    [self.mediaPanel addSubview:addBinButton];

    NSButton* deleteBinButton =
        [NSButton buttonWithTitle:@"Supprimer"
                           target:self
                           action:@selector(deleteBinPressed:)];
    deleteBinButton.frame =
        NSMakeRect(166.0, windowRect.size.height - 70.0, 142.0, 28.0);
    deleteBinButton.autoresizingMask = NSViewMinYMargin;
    deleteBinButton.bezelStyle = NSBezelStyleRounded;
    deleteBinButton.toolTip = @"Supprime le chutier sélectionné s’il est vide";
    [self.mediaPanel addSubview:deleteBinButton];

    self.binOutline = [[ContextOutlineView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, mediaPanelWidth - 24.0, 220.0)];
    NSTableColumn* binColumn =
        [[NSTableColumn alloc] initWithIdentifier:@"bin"];
    [self.binOutline addTableColumn:binColumn];
    self.binOutline.outlineTableColumn = binColumn;
    self.binOutline.headerView = nil;
    self.binOutline.dataSource = self;
    self.binOutline.delegate = self;
    NSScrollView* binScroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(12.0, windowRect.size.height - 300.0,
                                 mediaPanelWidth - 24.0, 220.0)];
    binScroll.autoresizingMask = NSViewMinYMargin;
    binScroll.documentView = self.binOutline;
    binScroll.hasVerticalScroller = YES;
    binScroll.borderType = NSBezelBorder;
    [self.mediaPanel addSubview:binScroll];

    self.mediaSearchField = [[NSSearchField alloc]
        initWithFrame:NSMakeRect(12.0, windowRect.size.height - 338.0,
                                 mediaPanelWidth - 104.0, 26.0)];
    self.mediaSearchField.placeholderString = @"Rechercher nom, codec, format…";
    self.mediaSearchField.target = self;
    self.mediaSearchField.action = @selector(mediaSearchChanged:);
    self.mediaSearchField.continuous = YES;
    self.mediaSearchField.autoresizingMask = NSViewMinYMargin;
    [self.mediaPanel addSubview:self.mediaSearchField];

    self.mediaViewToggle = [[NSSegmentedControl alloc]
        initWithFrame:NSMakeRect(mediaPanelWidth - 84.0,
                                 windowRect.size.height - 338.0, 72.0, 26.0)];
    self.mediaViewToggle.segmentCount = 2;
    [self.mediaViewToggle setLabel:@"☷" forSegment:0];
    [self.mediaViewToggle setLabel:@"▦" forSegment:1];
    self.mediaViewToggle.selectedSegment = 0;
    self.mediaViewToggle.target = self;
    self.mediaViewToggle.action = @selector(mediaViewChanged:);
    self.mediaViewToggle.autoresizingMask = NSViewMinYMargin;
    [self.mediaPanel addSubview:self.mediaViewToggle];

    self.mediaTable = [[ContextTableView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, mediaPanelWidth - 24.0, 500.0)];
    for (NSArray<NSString*>* definition in @[
             @[ @"name", @"Nom", @"145" ], @[ @"format", @"Format", @"85" ],
             @[ @"duration", @"Durée", @"60" ]
         ]) {
        NSTableColumn* column =
            [[NSTableColumn alloc] initWithIdentifier:definition[0]];
        column.title = definition[1];
        column.width = definition[2].doubleValue;
        [self.mediaTable addTableColumn:column];
    }
    self.mediaTable.dataSource = self;
    self.mediaTable.delegate = self;
    self.mediaTable.usesAlternatingRowBackgroundColors = YES;
    self.mediaListScroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(12.0, 112.0, mediaPanelWidth - 24.0,
                                 windowRect.size.height - 462.0)];
    self.mediaListScroll.autoresizingMask = NSViewHeightSizable;
    self.mediaListScroll.documentView = self.mediaTable;
    self.mediaListScroll.hasVerticalScroller = YES;
    self.mediaListScroll.hasHorizontalScroller = YES;
    self.mediaListScroll.borderType = NSBezelBorder;
    [self.mediaPanel addSubview:self.mediaListScroll];

    NSCollectionViewFlowLayout* iconLayout =
        [[NSCollectionViewFlowLayout alloc] init];
    iconLayout.itemSize = NSMakeSize(132.0, 112.0);
    iconLayout.minimumInteritemSpacing = 6.0;
    iconLayout.minimumLineSpacing = 8.0;
    iconLayout.sectionInset = NSEdgeInsetsMake(8, 8, 8, 8);
    self.mediaCollection = [[ContextCollectionView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, mediaPanelWidth - 24.0, 500.0)];
    self.mediaCollection.collectionViewLayout = iconLayout;
    self.mediaCollection.dataSource = self;
    self.mediaCollection.delegate = self;
    self.mediaCollection.selectable = YES;
    [self.mediaCollection setDraggingSourceOperationMask:NSDragOperationCopy
                                                forLocal:YES];
    [self.mediaCollection registerClass:MediaIconItem.class
                  forItemWithIdentifier:@"media-icon"];
    self.mediaIconScroll =
        [[NSScrollView alloc] initWithFrame:self.mediaListScroll.frame];
    self.mediaIconScroll.autoresizingMask = NSViewHeightSizable;
    self.mediaIconScroll.documentView = self.mediaCollection;
    self.mediaIconScroll.hasVerticalScroller = YES;
    self.mediaIconScroll.borderType = NSBezelBorder;
    self.mediaIconScroll.hidden = YES;
    [self.mediaPanel addSubview:self.mediaIconScroll];

    self.mediaTable.target = self;
    self.mediaTable.doubleAction = @selector(openSelectedMediaInSourceMonitor:);
    NSClickGestureRecognizer* iconDoubleClick =
        [[NSClickGestureRecognizer alloc]
            initWithTarget:self
                    action:@selector(openSelectedMediaInSourceMonitor:)];
    iconDoubleClick.numberOfClicksRequired = 2;
    [self.mediaCollection addGestureRecognizer:iconDoubleClick];
    [self.mediaTable setDraggingSourceOperationMask:NSDragOperationCopy
                                           forLocal:YES];

    self.assignMediaButton =
        [NSButton buttonWithTitle:@"Déplacer le média dans ce chutier"
                           target:self
                           action:@selector(assignMediaToBinPressed:)];
    self.assignMediaButton.frame = NSMakeRect(12.0, 76.0, 184.0, 28.0);
    self.assignMediaButton.autoresizingMask = NSViewMaxYMargin;
    self.assignMediaButton.bezelStyle = NSBezelStyleRounded;
    [self.mediaPanel addSubview:self.assignMediaButton];

    self.sourceMonitorButton =
        [NSButton buttonWithTitle:@"Source"
                           target:self
                           action:@selector(openSelectedMediaInSourceMonitor:)];
    self.sourceMonitorButton.frame = NSMakeRect(202.0, 76.0, 106.0, 28.0);
    self.sourceMonitorButton.autoresizingMask = NSViewMaxYMargin;
    self.sourceMonitorButton.bezelStyle = NSBezelStyleRounded;
    self.sourceMonitorButton.toolTip =
        @"Ouvre le média sélectionné dans le moniteur source";
    [self.mediaPanel addSubview:self.sourceMonitorButton];

    NSMenu* binContext = [[NSMenu alloc] initWithTitle:@"Chutier"];
    [binContext addItem:[self menuItem:@"Nouveau sous-chutier…"
                                action:@selector(createBinPressed:)
                                   key:@""]];
    [binContext addItem:[self menuItem:@"Renommer…"
                                action:@selector(renameBinPressed:)
                                   key:@""]];
    [binContext addItem:NSMenuItem.separatorItem];
    [binContext addItem:[self menuItem:@"Supprimer"
                                action:@selector(deleteBinPressed:)
                                   key:@""]];
    self.binOutline.menu = binContext;

    NSMenu* mediaContext = [[NSMenu alloc] initWithTitle:@"Média"];
    [mediaContext
        addItem:[self menuItem:@"Ouvrir dans le moniteur source"
                        action:@selector(openSelectedMediaInSourceMonitor:)
                           key:@""]];
    [mediaContext addItem:[self menuItem:@"Déplacer dans le chutier sélectionné"
                                  action:@selector(assignMediaToBinPressed:)
                                     key:@""]];
    [mediaContext addItem:NSMenuItem.separatorItem];
    [mediaContext addItem:[self menuItem:@"Révéler dans le Finder"
                                  action:@selector(revealSelectedMediaInFinder:)
                                     key:@""]];
    self.mediaTable.menu = mediaContext;
    self.mediaCollection.menu = mediaContext;

    self.binSummaryLabel = [NSTextField labelWithString:@""];
    self.binSummaryLabel.frame =
        NSMakeRect(14.0, 42.0, mediaPanelWidth - 28.0, 34.0);
    self.binSummaryLabel.autoresizingMask = NSViewMaxYMargin;
    self.binSummaryLabel.font = [NSFont systemFontOfSize:11.0];
    self.binSummaryLabel.textColor = NSColor.secondaryLabelColor;
    self.binSummaryLabel.maximumNumberOfLines = 2;
    [self.mediaPanel addSubview:self.binSummaryLabel];

    self.metalView = [[TimelineMetalView alloc]
        initWithFrame:NSMakeRect(mediaPanelWidth, 42.0,
                                 windowRect.size.width - mediaPanelWidth,
                                 windowRect.size.height - 42.0)];
    self.metalView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.metalView.eventTarget = self;
    [content addSubview:self.metalView];

    self.infoLabel = [NSTextField labelWithString:@"Aucun clip sélectionné"];
    self.infoLabel.frame =
        NSMakeRect(mediaPanelWidth + 20.0, 12.0,
                   windowRect.size.width - mediaPanelWidth - 375.0, 18.0);
    self.infoLabel.autoresizingMask = NSViewWidthSizable;
    self.infoLabel.font =
        [NSFont monospacedDigitSystemFontOfSize:12.0
                                         weight:NSFontWeightRegular];
    self.infoLabel.textColor = NSColor.secondaryLabelColor;
    self.infoLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    [content addSubview:self.infoLabel];

    self.detachAudioButton =
        [NSButton buttonWithTitle:@"Séparer audio  U"
                           target:self
                           action:@selector(detachAudioButtonPressed:)];
    self.detachAudioButton.frame =
        NSMakeRect(windowRect.size.width - 170.0, 7.0, 150.0, 28.0);
    self.detachAudioButton.autoresizingMask = NSViewMinXMargin;
    self.detachAudioButton.bezelStyle = NSBezelStyleRounded;
    self.detachAudioButton.controlSize = NSControlSizeSmall;
    self.detachAudioButton.enabled = NO;
    self.detachAudioButton.toolTip =
        @"Crée un clip audio indépendant via DetachAudio (U)";
    [content addSubview:self.detachAudioButton];

    self.linkedSelectionButton =
        [NSButton buttonWithTitle:@"Sélection liée : ON"
                           target:self
                           action:@selector(linkedSelectionPressed:)];
    self.linkedSelectionButton.frame =
        NSMakeRect(windowRect.size.width - 340.0, 7.0, 160.0, 28.0);
    self.linkedSelectionButton.autoresizingMask = NSViewMinXMargin;
    self.linkedSelectionButton.bezelStyle = NSBezelStyleRounded;
    self.linkedSelectionButton.controlSize = NSControlSizeSmall;
    [self.linkedSelectionButton setButtonType:NSButtonTypePushOnPushOff];
    self.linkedSelectionButton.state = NSControlStateValueOn;
    self.linkedSelectionButton.toolTip =
        @"Sélectionner ensemble l’image et son audio associé";
    [content addSubview:self.linkedSelectionButton];
    [self refreshBinControlsSelecting:@"__all__"];

    self.state->renderer = std::make_unique<Renderer>();
    if (!self.state->renderer->Initialize(self.metalView)) {
        std::fprintf(stderr, "Renderer initialization failed\n");
        [NSApp terminate:nil];
        return;
    }
    self.state->viewport.FitDuration(self.state->duration,
                                     self.metalView.bounds.size.width);

    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    for (auto& worker : self.state->workers) {
        worker.second->Start();
    }
    [self requestResolvedPosition:{0, 1}];
    self.displayTimer =
        [NSTimer scheduledTimerWithTimeInterval:(1.0 / 60.0)
                                         target:self
                                       selector:@selector(displayTick:)
                                       userInfo:nil
                                        repeats:YES];
}

- (void)requestResolvedPosition:(RationalTime)position {
    self.state->requestedPosition = position;
    self.state->requested.assign(self.state->videoTrackIds.size(), {});
    for (size_t slot = 0; slot < self.state->videoTrackIds.size(); ++slot) {
        const std::optional<ResolvedFrame> resolved =
            self.state->timeline->ResolveTrack(self.state->videoTrackIds[slot],
                                               position);
        if (!resolved) {
            continue;
        }
        self.state->requested[slot] =
            ResolvedSlot{true, resolved->source_id, resolved->source_frame};
        const auto worker = self.state->workers.find(resolved->source_id);
        if (worker != self.state->workers.end()) {
            worker->second->RequestFrame(resolved->source_frame);
        }
    }
}

- (void)refreshBinControlsSelecting:(NSString*)selectedBinId {
    if (!self.binOutline || !self.mediaTable) return;
    NSString* requested = selectedBinId ?: self.selectedBinId ?: @"__all__";
    self.selectedBinId = requested;
    [self.binOutline reloadData];
    [self.binOutline expandItem:nil expandChildren:YES];
    const NSInteger row = [self.binOutline rowForItem:requested];
    if (row >= 0)
        [self.binOutline selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                     byExtendingSelection:NO];
    [self rebuildMediaList];
}

- (void)binSelectionChanged:(id)sender {
    (void)sender;
    const NSInteger row = self.binOutline.selectedRow;
    if (row >= 0) self.selectedBinId = [self.binOutline itemAtRow:row];
    [self rebuildMediaList];
}

- (NSArray<NSString*>*)childBinIds:(NSString*)parentId {
    NSMutableArray<NSString*>* result = [NSMutableArray array];
    const std::string parent(parentId.UTF8String ?: "");
    std::vector<const DocumentBin*> bins;
    for (const DocumentBin& bin : self.state->document.bins)
        if (bin.parent_id == parent) bins.push_back(&bin);
    std::stable_sort(bins.begin(), bins.end(),
                     [](const DocumentBin* left, const DocumentBin* right) {
                         return left->name < right->name;
                     });
    for (const DocumentBin* bin : bins)
        [result addObject:[NSString stringWithUTF8String:bin->id.c_str()]];
    return result;
}

- (NSInteger)outlineView:(NSOutlineView*)outlineView
    numberOfChildrenOfItem:(id)item {
    (void)outlineView;
    if (!item) return 2 + [self childBinIds:@""].count;
    NSString* identifier = item;
    if ([identifier hasPrefix:@"__"]) return 0;
    return [self childBinIds:identifier].count;
}

- (id)outlineView:(NSOutlineView*)outlineView
            child:(NSInteger)index
           ofItem:(id)item {
    (void)outlineView;
    if (!item) {
        if (index == 0) return @"__all__";
        if (index == 1) return @"__root__";
        return [self childBinIds:@""][index - 2];
    }
    return [self childBinIds:item][index];
}

- (BOOL)outlineView:(NSOutlineView*)outlineView isItemExpandable:(id)item {
    (void)outlineView;
    NSString* identifier = item;
    return ![identifier hasPrefix:@"__"] &&
           [self childBinIds:identifier].count > 0;
}

- (NSView*)outlineView:(NSOutlineView*)outlineView
    viewForTableColumn:(NSTableColumn*)tableColumn
                  item:(id)item {
    (void)outlineView;
    (void)tableColumn;
    NSTextField* label = [NSTextField labelWithString:@""];
    NSString* identifier = item;
    if ([identifier isEqualToString:@"__all__"])
        label.stringValue = @"▣ Tous les médias";
    else if ([identifier isEqualToString:@"__root__"])
        label.stringValue = @"⌂ Sans chutier";
    else {
        const DocumentBin* bin =
            self.state->document.FindBin(identifier.UTF8String ?: "");
        label.stringValue =
            bin ? [NSString stringWithFormat:@"▸ %s", bin->name.c_str()]
                : @"Chutier manquant";
    }
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    return label;
}

- (void)outlineViewSelectionDidChange:(NSNotification*)notification {
    if (notification.object == self.binOutline)
        [self binSelectionChanged:self.binOutline];
}

- (void)rebuildMediaList {
    self.visibleMediaIds = [NSMutableArray array];
    NSString* query = self.mediaSearchField.stringValue.lowercaseString ?: @"";
    const std::string selected(self.selectedBinId.UTF8String ?: "__all__");
    std::vector<const LibraryMedia*> mediaItems;
    for (const LibraryMedia& media : self.state->document.library) {
        if (selected != "__all__" &&
            ((selected == "__root__" && !media.bin_id.empty()) ||
             (selected != "__root__" && media.bin_id != selected)))
            continue;
        NSString* searchable = [NSString
            stringWithFormat:@"%s %s %s %s %dx%d", media.filename.c_str(),
                             media.path.c_str(), media.codec.c_str(),
                             media.orientation.c_str(), media.width,
                             media.height];
        if (query.length > 0 &&
            [searchable.lowercaseString rangeOfString:query].location ==
                NSNotFound)
            continue;
        mediaItems.push_back(&media);
    }
    std::stable_sort(mediaItems.begin(), mediaItems.end(),
                     [](const LibraryMedia* left, const LibraryMedia* right) {
                         return left->filename < right->filename;
                     });
    for (const LibraryMedia* media : mediaItems)
        [self.visibleMediaIds
            addObject:[NSString stringWithUTF8String:media->id.c_str()]];
    [self.mediaTable reloadData];
    [self.mediaCollection reloadData];
    const NSUInteger count = self.visibleMediaIds.count;
    self.binSummaryLabel.stringValue = [NSString
        stringWithFormat:@"%lu média%@ affiché%@", (unsigned long)count,
                         count == 1 ? @"" : @"s", count == 1 ? @"" : @"s"];
    self.assignMediaButton.enabled =
        self.mediaTable.selectedRow >= 0 &&
        ![self.selectedBinId isEqualToString:@"__all__"];
}

- (void)mediaViewChanged:(id)sender {
    (void)sender;
    const BOOL icons = self.mediaViewToggle.selectedSegment == 1;
    self.mediaListScroll.hidden = icons;
    self.mediaIconScroll.hidden = !icons;
    [self rebuildMediaList];
}

- (NSString*)selectedMediaId {
    if (self.mediaViewToggle.selectedSegment == 1) {
        NSIndexPath* selected =
            self.mediaCollection.selectionIndexPaths.anyObject;
        if (!selected || selected.item >= self.visibleMediaIds.count)
            return nil;
        return self.visibleMediaIds[selected.item];
    }
    const NSInteger row = self.mediaTable.selectedRow;
    if (row < 0 || row >= (NSInteger)self.visibleMediaIds.count) return nil;
    return self.visibleMediaIds[row];
}

- (NSInteger)collectionView:(NSCollectionView*)collectionView
     numberOfItemsInSection:(NSInteger)section {
    (void)collectionView;
    (void)section;
    return self.visibleMediaIds.count;
}

- (NSCollectionViewItem*)collectionView:(NSCollectionView*)collectionView
    itemForRepresentedObjectAtIndexPath:(NSIndexPath*)indexPath {
    MediaIconItem* item = [collectionView makeItemWithIdentifier:@"media-icon"
                                                    forIndexPath:indexPath];
    if (indexPath.item >= self.visibleMediaIds.count) return item;
    const LibraryMedia* media = self.state->document.FindLibraryMedia(
        self.visibleMediaIds[indexPath.item].UTF8String ?: "");
    if (!media) return item;
    item.textField.stringValue =
        [NSString stringWithUTF8String:media->filename.c_str()];
    NSImage* symbol = [NSImage imageWithSystemSymbolName:@"film"
                                accessibilityDescription:@"Média vidéo"];
    symbol.size = NSMakeSize(44.0, 44.0);
    item.imageView.image = symbol;
    item.view.toolTip = [NSString
        stringWithFormat:@"%s · %s · %dx%d · %@", media->filename.c_str(),
                         media->codec.c_str(), media->width, media->height,
                         TimeString(media->duration)];
    return item;
}

- (id<NSPasteboardWriting>)collectionView:(NSCollectionView*)collectionView
       pasteboardWriterForItemAtIndexPath:(NSIndexPath*)indexPath {
    (void)collectionView;
    if (indexPath.item >= self.visibleMediaIds.count) return nil;
    NSPasteboardItem* item = [[NSPasteboardItem alloc] init];
    [item setString:self.visibleMediaIds[indexPath.item]
            forType:kCutmachineMediaPasteboardType];
    return item;
}

- (BOOL)tableView:(NSTableView*)tableView
    writeRowsWithIndexes:(NSIndexSet*)rowIndexes
            toPasteboard:(NSPasteboard*)pasteboard {
    if (tableView != self.mediaTable || rowIndexes.count != 1) return NO;
    const NSUInteger row = rowIndexes.firstIndex;
    if (row >= self.visibleMediaIds.count) return NO;
    [pasteboard declareTypes:@[ kCutmachineMediaPasteboardType ] owner:nil];
    return [pasteboard setString:self.visibleMediaIds[row]
                         forType:kCutmachineMediaPasteboardType];
}

- (void)openSelectedMediaInSourceMonitor:(id)sender {
    (void)sender;
    NSString* identifier = [self selectedMediaId];
    if (!identifier) {
        const DocumentClip* clip = self.state->document.FindClip(
            self.state->interaction->SelectedClipId());
        if (clip)
            identifier =
                [NSString stringWithUTF8String:clip->source_id.c_str()];
    }
    if (!identifier) return;
    [self openMediaIdentifierInSourceMonitor:identifier];
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    const SEL action = item.action;
    if (action == @selector(menuUndo:))
        return self.state && self.state->editLog.AppliedCount() > 0;
    if (action == @selector(menuRedo:))
        return self.state && self.state->editLog.UndoneCount() > 0;
    if (action == @selector(menuToggleSnapping:)) {
        if (!self.state || !self.state->interaction) return NO;
        item.state = self.state->interaction->SnappingEnabled()
                         ? NSControlStateValueOn
                         : NSControlStateValueOff;
        return YES;
    }
    if (action == @selector(menuToggleLinkedSelection:)) {
        if (!self.state || !self.state->interaction) return NO;
        item.state = self.state->linkedSelection ? NSControlStateValueOn
                                                 : NSControlStateValueOff;
        return YES;
    }
    if (action == @selector(renameBinPressed:) || action == @selector
                                                      (deleteBinPressed:))
        return self.state &&
               self.state->document.FindBin(self.selectedBinId.UTF8String
                                                ?: "") != nullptr;
    if (action == @selector(assignMediaToBinPressed:))
        return [self selectedMediaId] != nil &&
               ![self.selectedBinId isEqualToString:@"__all__"];
    if (action == @selector(detachAudioButtonPressed:)) {
        if (!self.state || !self.state->interaction) return NO;
        const DocumentClip* clip = self.state->document.FindClip(
            self.state->interaction->SelectedClipId());
        return clip && clip->include_audio;
    }
    if (action == @selector(menuCutSelectedAtPlayhead:)) {
        if (!self.state || !self.state->interaction) return NO;
        const DocumentClip* clip = self.state->document.FindClip(
            self.state->interaction->SelectedClipId());
        return clip && self.state->requestedPosition > clip->timeline_in &&
               self.state->requestedPosition <
                   clip->timeline_in.add(clip->duration);
    }
    return YES;
}

- (void)openMediaIdentifierInSourceMonitor:(NSString*)identifier {
    const Ulid sourceId(identifier.UTF8String ?: "");
    const DocumentSource* source = self.state->document.FindSource(sourceId);
    const LibraryMedia* media = self.state->document.FindLibraryMedia(sourceId);
    const auto worker = self.state->workers.find(sourceId);
    if (!source || !media || worker == self.state->workers.end()) {
        self.binSummaryLabel.stringValue =
            @"Ce média doit être réingéré pour devenir une source montable.";
        return;
    }
    [self setPlaybackDirection:0];
    self.state->sourceMonitor = true;
    self.state->sourceMonitorId = sourceId;
    self.state->sourceMonitorPosition = {0, source->duration.rate};
    self.state->requested.assign(1, ResolvedSlot{true, sourceId, 0});
    worker->second->RequestFrame(0);
    self.state->rendered.clear();
    self.state->overlayDirty = true;
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"MONITEUR SOURCE    %s    durée %@    %d/%d fps",
                         media->filename.c_str(), TimeString(source->duration),
                         source->rate.num, source->rate.den];
}

- (void)revealSelectedMediaInFinder:(id)sender {
    (void)sender;
    NSString* identifier = [self selectedMediaId];
    if (!identifier) return;
    const LibraryMedia* media =
        self.state->document.FindLibraryMedia(identifier.UTF8String ?: "");
    if (!media) return;
    std::filesystem::path path(media->path);
    if (path.is_relative()) {
        path = std::filesystem::absolute(
                   std::filesystem::path(self.documentPath.UTF8String ?: ""))
                   .parent_path() /
               path;
    }
    NSURL* url = [NSURL
        fileURLWithPath:[NSString stringWithUTF8String:path.lexically_normal()
                                                           .c_str()]];
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ url ]];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
    return tableView == self.mediaTable ? self.visibleMediaIds.count : 0;
}

- (NSView*)tableView:(NSTableView*)tableView
    viewForTableColumn:(NSTableColumn*)tableColumn
                   row:(NSInteger)row {
    if (tableView != self.mediaTable || row < 0 ||
        row >= (NSInteger)self.visibleMediaIds.count)
        return nil;
    const LibraryMedia* media = self.state->document.FindLibraryMedia(
        self.visibleMediaIds[row].UTF8String ?: "");
    NSTextField* label = [NSTextField labelWithString:@""];
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    if (!media) return label;
    if ([tableColumn.identifier isEqualToString:@"name"])
        label.stringValue =
            [NSString stringWithUTF8String:media->filename.c_str()];
    else if ([tableColumn.identifier isEqualToString:@"format"])
        label.stringValue =
            media->metadata_complete
                ? [NSString stringWithFormat:@"%s %dx%d", media->codec.c_str(),
                                             media->width, media->height]
                : @"—";
    else
        label.stringValue =
            [NSString stringWithFormat:@"%@", TimeString(media->duration)];
    return label;
}

- (void)tableViewSelectionDidChange:(NSNotification*)notification {
    if (notification.object != self.mediaTable) return;
    self.assignMediaButton.enabled =
        self.mediaTable.selectedRow >= 0 &&
        ![self.selectedBinId isEqualToString:@"__all__"];
}

- (void)collectionView:(NSCollectionView*)collectionView
    didSelectItemsAtIndexPaths:(NSSet<NSIndexPath*>*)indexPaths {
    (void)collectionView;
    (void)indexPaths;
    self.assignMediaButton.enabled =
        [self selectedMediaId] != nil &&
        ![self.selectedBinId isEqualToString:@"__all__"];
}

- (void)mediaSearchChanged:(id)sender {
    (void)sender;
    [self rebuildMediaList];
}

- (void)createBinPressed:(id)sender {
    (void)sender;
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Nouveau chutier";
    alert.informativeText = @"Donnez un nom au chutier de médias.";
    [alert addButtonWithTitle:@"Créer"];
    [alert addButtonWithTitle:@"Annuler"];
    NSTextField* input =
        [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 280, 24)];
    input.placeholderString = @"Rushes, Interviews, B-roll…";
    alert.accessoryView = input;
    if ([alert runModal] != NSAlertFirstButtonReturn) return;
    NSString* trimmed = [input.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet
                                            .whitespaceAndNewlineCharacterSet];
    if (trimmed.length == 0) return;

    const Ulid binId = GenerateUlid();
    EditError error = EditError::None;
    std::string message;
    if (!self.state->editLog.Apply(
            self.state->document,
            Operation{AddBinOperation{
                binId, trimmed.UTF8String ?: "",
                self.state->document.FindBin(self.selectedBinId.UTF8String
                                                 ?: "")
                    ? std::string(self.selectedBinId.UTF8String ?: "")
                    : std::string{}}},
            error, message)) {
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"Création refusée : %s", message.c_str()];
        return;
    }
    if (![self persistEdits:message])
        std::fprintf(stderr, "Unable to persist bin creation: %s\n",
                     message.c_str());
    [self refreshBinControlsSelecting:[NSString
                                          stringWithUTF8String:binId.c_str()]];
}

- (void)deleteBinPressed:(id)sender {
    (void)sender;
    const std::string binId(self.selectedBinId.UTF8String ?: "");
    if (!self.state->document.FindBin(binId)) return;
    EditError error = EditError::None;
    std::string message;
    if (!self.state->editLog.Apply(self.state->document,
                                   Operation{RemoveBinOperation{binId, "", ""}},
                                   error, message)) {
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"Suppression refusée : %s", message.c_str()];
        return;
    }
    if (![self persistEdits:message])
        std::fprintf(stderr, "Unable to persist bin deletion: %s\n",
                     message.c_str());
    [self refreshBinControlsSelecting:@"__all__"];
}

- (void)renameBinPressed:(id)sender {
    (void)sender;
    const std::string binId(self.selectedBinId.UTF8String ?: "");
    const DocumentBin* bin = self.state->document.FindBin(binId);
    if (!bin) return;
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Renommer le chutier";
    [alert addButtonWithTitle:@"Renommer"];
    [alert addButtonWithTitle:@"Annuler"];
    NSTextField* input =
        [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 280, 24)];
    input.stringValue = [NSString stringWithUTF8String:bin->name.c_str()];
    alert.accessoryView = input;
    if ([alert runModal] != NSAlertFirstButtonReturn) return;
    NSString* name = [input.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet
                                            .whitespaceAndNewlineCharacterSet];
    if (name.length == 0) return;
    EditError error = EditError::None;
    std::string message;
    if (!self.state->editLog.Apply(
            self.state->document,
            Operation{RenameBinOperation{binId, name.UTF8String ?: ""}}, error,
            message)) {
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"Renommage refusé : %s", message.c_str()];
        return;
    }
    if (![self persistEdits:message])
        std::fprintf(stderr, "Unable to persist bin rename: %s\n",
                     message.c_str());
    [self refreshBinControlsSelecting:self.selectedBinId];
}

- (void)assignMediaToBinPressed:(id)sender {
    (void)sender;
    const NSInteger row = self.mediaTable.selectedRow;
    if (row < 0 || row >= (NSInteger)self.visibleMediaIds.count) return;
    NSString* media = self.visibleMediaIds[row];
    NSString* bin = [self.selectedBinId isEqualToString:@"__root__"]
                        ? @""
                        : self.selectedBinId;
    if (!media || [bin isEqualToString:@"__all__"]) return;
    EditError error = EditError::None;
    std::string message;
    if (!self.state->editLog.Apply(
            self.state->document,
            Operation{SetMediaBinOperation{media.UTF8String ?: "",
                                           bin.UTF8String ?: ""}},
            error, message)) {
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"Classement refusé : %s", message.c_str()];
        return;
    }
    if (![self persistEdits:message])
        std::fprintf(stderr, "Unable to persist media bin: %s\n",
                     message.c_str());
    [self refreshBinControlsSelecting:bin.length == 0 ? @"__root__" : bin];
}

- (MediaRate)playheadFrameRate {
    if (!self.state->document.sources.empty())
        return self.state->document.sources.front().rate;
    return {25, 1};
}

- (int32_t)playheadInputRate {
    return self.state->playheadResolution == PlayheadResolution::Sample
               ? 48000
               : [self playheadFrameRate].num;
}

- (double)timelineHeight {
    const double contentHeight =
        kTimelineRulerHeight +
        self.state->document.tracks.size() * self.state->viewport.track_height +
        kAddTrackRowHeight;
    return std::min(contentHeight,
                    std::max(0.0, self.metalView.bounds.size.height - 160.0));
}

- (double)videoHeight {
    return self.metalView.bounds.size.height - [self timelineHeight];
}

- (NSPoint)timelinePointForEvent:(NSEvent*)event {
    const NSPoint point = [self.metalView convertPoint:event.locationInWindow
                                              fromView:nil];
    return NSMakePoint(point.x, self.metalView.bounds.size.height - point.y -
                                    [self videoHeight]);
}

- (TimelineTool)effectiveTool {
    return self.state->spaceHand ? TimelineTool::Hand : self.state->tool;
}

- (void)applyToolCursor {
    switch ([self effectiveTool]) {
        case TimelineTool::Select:
            [NSCursor.arrowCursor set];
            break;
        case TimelineTool::Hand:
            [(self.state->navigationDragging ? NSCursor.closedHandCursor
                                             : NSCursor.openHandCursor) set];
            break;
        case TimelineTool::Zoom:
            [NSCursor.crosshairCursor set];
            break;
        case TimelineTool::Cut:
            [NSCursor.crosshairCursor set];
            break;
    }
}

- (void)setTimelineTool:(TimelineTool)tool {
    self.state->tool = tool;
    self.state->interaction->CancelDrag();
    self.state->navigationDragging = false;
    self.state->scrubDragging = false;
    self.state->editDragging = false;
    self.state->lassoCandidate = false;
    self.state->lassoDragging = false;
    self.state->cutPreviewX.reset();
    self.state->cutPreviewY.reset();
    [self applyToolCursor];
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}

- (void)requestTimelinePosition:(RationalTime)position {
    if (self.state->sourceMonitor) {
        self.state->sourceMonitor = false;
        self.state->sourceMonitorId.clear();
        self.state->rendered.clear();
    }
    if (position < RationalTime{0, 1}) position = {0, position.rate};
    if (position > self.state->duration) position = self.state->duration;
    RationalTime quantized = QuantizePlayheadPosition(
        position, self.state->playheadResolution, [self playheadFrameRate]);
    if (quantized > self.state->duration) {
        if (self.state->playheadResolution == PlayheadResolution::Sample) {
            quantized = {self.state->duration.to_frames(48000), 48000};
        } else {
            const MediaRate rate = [self playheadFrameRate];
            quantized = {
                self.state->duration.to_frames(rate.num, rate.den) * rate.den,
                rate.num};
        }
    }
    [self requestResolvedPosition:quantized];
    if (self.state->playbackDirection == 0 && self.state->audioPlayback) {
        std::string error;
        if (!self.state->audioPlayback->ScrubAt(quantized, error))
            std::fprintf(stderr, "Audio scrub failed: %s\n", error.c_str());
    }
    self.state->overlayDirty = true;
}

- (BOOL)timelineDropMedia:(NSString*)mediaId atViewPoint:(NSPoint)point {
    const double timelineY =
        self.metalView.bounds.size.height - point.y - [self videoHeight];
    if (point.x < self.state->viewport.header_width ||
        timelineY < kTimelineRulerHeight)
        return NO;
    const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
    const NSInteger trackIndex = static_cast<NSInteger>(
        (timelineY - kTimelineRulerHeight) / self.state->viewport.track_height);
    if (trackIndex < 0 || trackIndex >= (NSInteger)tracks.size()) return NO;
    const DocumentTrack* track = tracks[trackIndex];
    if (!track || track->kind != "video") return NO;
    const Ulid sourceId(mediaId.UTF8String ?: "");
    const DocumentSource* source = self.state->document.FindSource(sourceId);
    if (!source) {
        self.binSummaryLabel.stringValue =
            @"Ce média doit être réingéré avant son montage.";
        return NO;
    }
    RationalTime timelineIn;
    try {
        timelineIn =
            self.state->viewport.XToTime(point.x, source->duration.rate);
        if (timelineIn.value < 0) timelineIn = {0, timelineIn.rate};
    } catch (const std::exception& exception) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Drop refusé : %s", exception.what()];
        return NO;
    }
    Operation operation = InsertClipOperation{track->id,
                                              sourceId,
                                              {0, source->duration.rate},
                                              source->duration,
                                              timelineIn,
                                              {},
                                              {}};
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (!self.state->editLog.Apply(self.state->document, std::move(operation),
                                   error, message)) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Insertion refusée (%s) : %s",
                                       EditErrorName(error), message.c_str()];
        return NO;
    }
    const auto& stored = std::get<InsertClipOperation>(
        self.state->editLog.AppliedEntries().back().op);
    self.state->interaction->SelectClip(stored.clip_id);
    [self refreshTimelineAfterEditFromPosition:playhead];
    if (![self persistEdits:message])
        std::fprintf(stderr, "Unable to persist media drop: %s\n",
                     message.c_str());
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
    return YES;
}

- (NSString*)transportStatus {
    if (self.state->playbackDirection > 0) return @"Lecture ▶";
    if (self.state->playbackDirection < 0) return @"Lecture ◀";
    return @"Pause";
}

- (NSString*)playheadResolutionStatus {
    return self.state->playheadResolution == PlayheadResolution::Frame
               ? @"Image (M)"
               : @"Échantillon 48 kHz (M)";
}

- (void)setPlaybackDirection:(int)direction {
    self.state->playbackDirection = std::clamp(direction, -1, 1);
    self.state->playbackAnchor = self.state->requestedPosition;
    self.state->playbackStarted = std::chrono::steady_clock::now();
    if (self.state->audioPlayback) {
        self.state->audioPlayback->Stop();
        if (self.state->playbackDirection != 0) {
            std::string error;
            if (!self.state->audioPlayback->PlayFrom(
                    self.state->playbackAnchor, self.state->playbackDirection,
                    error))
                std::fprintf(stderr, "Audio playback failed: %s\n",
                             error.c_str());
        }
    }
    [self updateSelectionInfo];
}

- (void)updateSelectionInfo {
    self.detachAudioButton.enabled = NO;
    const size_t selectionCount =
        self.state->interaction->SelectedClipIds().size();
    if (selectionCount > 1) {
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"%@    Playhead %@    Outil %@    %zu clips "
                             @"sélectionnés    Liens %@",
                             [self transportStatus],
                             [self playheadResolutionStatus],
                             ToolName(self.state->tool), selectionCount,
                             self.state->linkedSelection ? @"ON" : @"OFF"];
        return;
    }
    const DocumentClip* clip = self.state->document.FindClip(
        self.state->interaction->SelectedClipId());
    if (!clip) {
        if (const auto& gap = self.state->interaction->SelectedGap()) {
            self.infoLabel.stringValue = [NSString
                stringWithFormat:@"%@    Playhead %@    Outil %@    Aimant %@  "
                                 @"  Trou sélectionné    piste %s    début %@  "
                                 @"  durée %@    Delete pour raccorder",
                                 [self transportStatus],
                                 [self playheadResolutionStatus],
                                 ToolName(self.state->tool),
                                 self.state->interaction->SnappingEnabled()
                                     ? @"ON (N)"
                                     : @"OFF (N)",
                                 gap->track_id.c_str(), TimeString(gap->start),
                                 TimeString(gap->duration)];
            return;
        }
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"%@    Playhead %@    Outil %@    Aimant %@    "
                             @"Aucun clip sélectionné",
                             [self transportStatus],
                             [self playheadResolutionStatus],
                             ToolName(self.state->tool),
                             self.state->interaction->SnappingEnabled()
                                 ? @"ON (N)"
                                 : @"OFF (N)"];
        return;
    }
    const DocumentSource* source =
        self.state->document.FindSource(clip->source_id);
    const DocumentTrack* selectedTrack =
        self.state->document.FindTrackForClip(clip->id);
    const auto metadata = self.state->mediaMetadata.find(clip->source_id);
    const LibraryMedia* media = metadata == self.state->mediaMetadata.end()
                                    ? nullptr
                                    : &metadata->second;
    NSString* sourceText =
        source ? [NSString stringWithFormat:@"%s (%s)", source->path.c_str(),
                                            source->id.c_str()]
               : [NSString stringWithUTF8String:clip->source_id.c_str()];
    NSString* metadataText = @"métadonnées indisponibles";
    if (media && media->metadata_complete) {
        metadataText = [NSString
            stringWithFormat:@"%dx%d %@  %s/%s  range %s  matrix %s  rot %d°  "
                             @"%d/%d fps  audio %@",
                             media->width, media->height,
                             [NSString stringWithUTF8String:media->orientation
                                                                .c_str()],
                             media->pixel_format.c_str(),
                             media->color_transfer.c_str(),
                             media->color_range.c_str(),
                             media->color_space.c_str(),
                             media -> rotation_degrees, media -> rate.num,
                             media->rate.den,
                             media->has_audio ? @"oui" : @"non"];
    }
    NSString* roleText = @"clip";
    if (selectedTrack && selectedTrack->kind == "audio")
        roleText = @"audio séparé";
    else if (selectedTrack && selectedTrack->kind == "video") {
        roleText = clip->include_audio ? @"vidéo + audio lié — U séparer"
                                       : @"vidéo seule";
        self.detachAudioButton.enabled = clip->include_audio;
    }
    NSString* syncText = @"";
    if (selectedTrack && selectedTrack->kind == "audio") {
        const auto drift = ClipSyncDrift(self.state->document, clip->id);
        if (drift && drift->value != 0) {
            const std::string label =
                SyncDriftLabel(*drift, [self playheadFrameRate]);
            syncText = [NSString
                stringWithFormat:@"    Décalage sync %s", label.c_str()];
        }
    }
    self.infoLabel.stringValue = [NSString
        stringWithFormat:
            @"%@    Playhead %@    Outil %@    Aimant %@    %@%@    source %@  "
            @"  %@    source_in %@    durée %@    timeline_in %@",
            [self transportStatus], [self playheadResolutionStatus],
            ToolName(self.state->tool),
            self.state->interaction->SnappingEnabled() ? @"ON (N)" : @"OFF (N)",
            roleText, syncText, sourceText, metadataText,
            TimeString(clip->source_in), TimeString(clip->duration),
            TimeString(clip->timeline_in)];
}

- (void)detachAudioButtonPressed:(id)sender {
    (void)sender;
    [self.window makeFirstResponder:self.metalView];
    [self detachSelectedAudio];
}

- (void)linkedSelectionPressed:(NSButton*)sender {
    self.state->linkedSelection = sender.state == NSControlStateValueOn;
    self.state->interaction->SetLinkedSelectionEnabled(
        self.state->linkedSelection);
    sender.title = self.state->linkedSelection ? @"Sélection liée : ON"
                                               : @"Sélection liée : OFF";
    const std::vector<Ulid> selected =
        self.state->interaction->SelectedClipIds();
    if (self.state->linkedSelection) {
        self.state->interaction->SelectClips(
            ExpandLinkedClipSelection(self.state->document, selected));
    } else if (selected.size() > 1) {
        self.state->interaction->SelectClip(selected.front());
    }
    [self.window makeFirstResponder:self.metalView];
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}

- (void)timelineMouseDown:(NSEvent*)event {
    const NSPoint point = [self timelinePointForEvent:event];
    if (point.y < 0.0 || point.y > [self timelineHeight]) return;
    const double addTrackY =
        kTimelineRulerHeight +
        self.state->document.tracks.size() * self.state->viewport.track_height;
    if (point.y >= addTrackY && point.y < addTrackY + kAddTrackRowHeight &&
        point.x >= 0.0 && point.x < self.state->viewport.header_width) {
        [self addTrack:(point.x >= self.state->viewport.header_width * 0.5)];
        return;
    }
    if (point.y < kTimelineRulerHeight && point.x >= 0.0 &&
        point.x < self.state->viewport.header_width) {
        const int index = std::clamp(static_cast<int>(point.x / 24.0), 0, 3);
        [self setTimelineTool:static_cast<TimelineTool>(index)];
        return;
    }
    const TimelineTool tool = [self effectiveTool];
    self.state->lassoCandidate = false;
    self.state->lassoDragging = false;
    if (tool == TimelineTool::Hand) {
        if (self.state->spaceHand) self.state->spaceUsedForPan = true;
        self.state->navigationDragging = true;
        self.state->navigationLastX = point.x;
        [self applyToolCursor];
        return;
    }
    if (tool == TimelineTool::Zoom) {
        const bool zoomOut =
            (event.modifierFlags & NSEventModifierFlagOption) != 0;
        self.state->viewport.ZoomAroundX(point.x, zoomOut ? 0.5 : 2.0,
                                         self.state->duration.rate);
        self.state->overlayDirty = true;
        return;
    }
    if (tool == TimelineTool::Cut) {
        [self setPlaybackDirection:0];
        const auto hit =
            HitTestTimeline(self.state->document, self.state->viewport, point.x,
                            point.y, self.metalView.bounds.size.width);
        if (!hit) return;
        const DocumentClip* clip = self.state->document.FindClip(hit->clip_id);
        if (!clip) return;
        Operation operation = SplitClipOperation{
            clip->id,
            self.state->viewport.XToTime(point.x, clip->duration.rate),
            {}};
        EditError error = EditError::None;
        std::string message;
        const RationalTime playhead = self.state->requestedPosition;
        if (self.state->editLog.Apply(self.state->document,
                                      std::move(operation), error, message)) {
            self.state->interaction->SelectClip(hit->clip_id);
            [self refreshTimelineAfterEditFromPosition:playhead];
            if (![self persistEdits:message])
                std::fprintf(stderr, "Unable to persist cut: %s\n",
                             message.c_str());
            [self updateSelectionInfo];
            self.state->overlayDirty = true;
        } else if (error != EditError::InvalidTimelineIn) {
            std::fprintf(stderr, "Cut rejected (%s): %s\n",
                         EditErrorName(error), message.c_str());
        }
        return;
    }
    const auto selectionHit =
        HitTestTimeline(self.state->document, self.state->viewport, point.x,
                        point.y, self.metalView.bounds.size.width);
    if (tool == TimelineTool::Select && !selectionHit &&
        point.x >= self.state->viewport.header_width &&
        point.y >= kTimelineRulerHeight && point.y < addTrackY) {
        self.state->lassoCandidate = true;
        self.state->lassoStartX = self.state->lassoCurrentX = point.x;
        self.state->lassoStartY = self.state->lassoCurrentY = point.y;
    }
    [self setPlaybackDirection:0];
    self.state->linkedSelectionGesture =
        self.state->linkedSelection &&
        (event.modifierFlags & NSEventModifierFlagOption) == 0;
    self.state->interaction->SetLinkedSelectionEnabled(
        self.state->linkedSelectionGesture);
    self.state->interaction->PointerDown(point.x, point.y,
                                         self.metalView.bounds.size.width,
                                         [self playheadInputRate]);
    if (self.state->linkedSelectionGesture && selectionHit) {
        self.state->interaction->SelectClips(ExpandLinkedClipSelection(
            self.state->document, {selectionHit->clip_id}));
    }
    self.state->editDragging = self.state->interaction->HasActiveDrag();
    self.state->scrubDragging =
        self.state->interaction->RequestedPlayhead().has_value();
    if (self.state->interaction->RequestedPlayhead()) {
        [self requestTimelinePosition:*self.state->interaction
                                           ->RequestedPlayhead()];
        self.state->interaction->ClearRequestedPlayhead();
    }
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}

- (void)timelineMouseDragged:(NSEvent*)event {
    const NSPoint point = [self timelinePointForEvent:event];
    if (self.state->navigationDragging) {
        const double delta = self.state->navigationLastX - point.x;
        self.state->viewport.ScrollByPixels(delta, self.state->duration.rate);
        self.state->navigationLastX = point.x;
        self.state->overlayDirty = true;
        return;
    }
    if (self.state->lassoCandidate) {
        self.state->lassoCurrentX = point.x;
        self.state->lassoCurrentY = point.y;
        const double deltaX = point.x - self.state->lassoStartX;
        const double deltaY = point.y - self.state->lassoStartY;
        if (!self.state->lassoDragging && std::hypot(deltaX, deltaY) >= 4.0) {
            self.state->lassoDragging = true;
            self.state->scrubDragging = false;
        }
        if (self.state->lassoDragging) {
            std::vector<Ulid> selected = LassoHitTestTimeline(
                self.state->document, self.state->viewport,
                self.state->lassoStartX, self.state->lassoStartY,
                self.state->lassoCurrentX, self.state->lassoCurrentY,
                self.metalView.bounds.size.width);
            if (self.state->linkedSelectionGesture)
                selected =
                    ExpandLinkedClipSelection(self.state->document, selected);
            self.state->interaction->SelectClips(selected);
            [self updateSelectionInfo];
            self.state->overlayDirty = true;
            return;
        }
    }
    if (self.state->scrubDragging) {
        [self requestTimelinePosition:self.state->viewport.XToTime(
                                          point.x, [self playheadInputRate])];
        return;
    }
    self.state->interaction->PointerDrag(point.x, point.y,
                                         self.metalView.bounds.size.width);
    self.state->overlayDirty = true;
}

- (void)timelineMouseMoved:(NSEvent*)event {
    self.state->cutPreviewX.reset();
    self.state->cutPreviewY.reset();
    if ([self effectiveTool] != TimelineTool::Cut) {
        self.state->overlayDirty = true;
        return;
    }
    const NSPoint point = [self timelinePointForEvent:event];
    if (point.y < kTimelineRulerHeight || point.y > [self timelineHeight]) {
        self.state->overlayDirty = true;
        return;
    }
    const auto hit =
        HitTestTimeline(self.state->document, self.state->viewport, point.x,
                        point.y, self.metalView.bounds.size.width);
    const DocumentClip* clip =
        hit ? self.state->document.FindClip(hit->clip_id) : nullptr;
    if (clip) {
        const RationalTime cut =
            self.state->viewport.XToTime(point.x, clip->duration.rate);
        if (cut > clip->timeline_in &&
            cut < clip->timeline_in.add(clip->duration)) {
            self.state->cutPreviewX = self.state->viewport.TimeToX(cut);
            const auto tracks =
                TimelineTracksInDisplayOrder(self.state->document);
            const auto track = std::find_if(
                tracks.begin(), tracks.end(), [&](const DocumentTrack* value) {
                    return value->id == hit->track_id;
                });
            if (track != tracks.end())
                self.state->cutPreviewY = kTimelineRulerHeight +
                                          std::distance(tracks.begin(), track) *
                                              self.state->viewport.track_height;
        }
    }
    self.state->overlayDirty = true;
}

- (BOOL)persistEdits:(std::string&)message {
    const char* path = self.documentPath.UTF8String;
    return CommitDocumentAndEditLog(path ? path : "", self.state->document,
                                    self.state->editLog, message);
}

- (void)rebuildVideoTrackIds {
    std::vector<const DocumentTrack*> tracks;
    for (const DocumentTrack& track : self.state->document.tracks)
        if (track.kind == "video") tracks.push_back(&track);
    std::sort(tracks.begin(), tracks.end(),
              [](const DocumentTrack* left, const DocumentTrack* right) {
                  return left->index < right->index;
              });
    self.state->videoTrackIds.clear();
    for (const DocumentTrack* track : tracks)
        self.state->videoTrackIds.push_back(track->id);
    self.state->requested.assign(tracks.size(), {});
    self.state->rendered.assign(tracks.size(), {});
}

- (void)addTrack:(BOOL)audio {
    int32_t index = 0;
    for (const DocumentTrack& track : self.state->document.tracks)
        index = std::max(index, track.index + 1);
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    Operation operation =
        AddTrackOperation{"", audio ? "audio" : "video", index};
    if (self.state->editLog.Apply(self.state->document, std::move(operation),
                                  error, message)) {
        [self refreshTimelineAfterEditFromPosition:playhead];
        if (![self persistEdits:message])
            std::fprintf(stderr, "Unable to persist track creation: %s\n",
                         message.c_str());
        [self updateSelectionInfo];
        self.state->overlayDirty = true;
    } else {
        std::fprintf(stderr, "Track creation rejected (%s): %s\n",
                     EditErrorName(error), message.c_str());
    }
}

- (void)detachSelectedAudio {
    const DocumentClip* clip = self.state->document.FindClip(
        self.state->interaction->SelectedClipId());
    const DocumentTrack* sourceTrack =
        clip ? self.state->document.FindTrackForClip(clip->id) : nullptr;
    if (!clip || !sourceTrack || sourceTrack->kind != "video" ||
        !clip->include_audio) {
        self.infoLabel.stringValue =
            @"Sélectionnez un clip vidéo dont l’audio est encore lié";
        return;
    }
    const Ulid audioClipId = GenerateUlid();
    Ulid targetTrackId;
    for (const DocumentTrack* track :
         TimelineTracksInDisplayOrder(self.state->document)) {
        if (track->kind != "audio") continue;
        Document candidate = self.state->document;
        Operation probe =
            DetachAudioOperation{clip->id, track->id, audioClipId, {}};
        Operation inverse = RemoveClipOperation{};
        EditError probeError = EditError::None;
        std::string probeMessage;
        if (ApplyOperation(candidate, probe, inverse, probeError,
                           probeMessage)) {
            targetTrackId = track->id;
            break;
        }
    }
    if (targetTrackId.empty()) {
        self.infoLabel.stringValue =
            @"Ajoutez une piste audio verte libre, puis appuyez sur U";
        return;
    }
    const RationalTime playhead = self.state->requestedPosition;
    EditError error = EditError::None;
    std::string message;
    Operation operation =
        DetachAudioOperation{clip->id, targetTrackId, audioClipId, {}};
    if (self.state->editLog.Apply(self.state->document, std::move(operation),
                                  error, message)) {
        self.state->interaction->SelectClip(audioClipId);
        [self refreshTimelineAfterEditFromPosition:playhead];
        if (![self persistEdits:message])
            std::fprintf(stderr, "Unable to persist audio detach: %s\n",
                         message.c_str());
        [self updateSelectionInfo];
        self.state->overlayDirty = true;
    } else {
        std::fprintf(stderr, "Audio detach rejected (%s): %s\n",
                     EditErrorName(error), message.c_str());
    }
}

- (void)refreshTimelineAfterEditFromPosition:(RationalTime)position {
    self.state->interaction->ClearGapSelection();
    [self rebuildVideoTrackIds];
    if (self.state->audioPlayback)
        self.state->audioPlayback->RebuildTimeline(self.state->document);
    self.state->duration = self.state->timeline->Duration();
    if (position < RationalTime{0, 1}) position = {0, 1};
    if (position > self.state->duration) position = self.state->duration;
    RationalTime quantized = QuantizePlayheadPosition(
        position, self.state->playheadResolution, [self playheadFrameRate]);
    if (quantized > self.state->duration) quantized = self.state->duration;
    [self requestResolvedPosition:quantized];
}

- (void)timelineMouseUp:(NSEvent*)event {
    (void)event;
    if (self.state->lassoCandidate) {
        const bool completedLasso = self.state->lassoDragging;
        self.state->lassoCandidate = false;
        self.state->lassoDragging = false;
        if (completedLasso) {
            self.state->scrubDragging = false;
            self.state->interaction->SetLinkedSelectionEnabled(
                self.state->linkedSelection);
            [self updateSelectionInfo];
            self.state->overlayDirty = true;
            return;
        }
    }
    if (self.state->navigationDragging || self.state->scrubDragging) {
        self.state->navigationDragging = false;
        self.state->scrubDragging = false;
        self.state->interaction->SetLinkedSelectionEnabled(
            self.state->linkedSelection);
        [self applyToolCursor];
        return;
    }
    if (!self.state->editDragging) {
        self.state->interaction->SetLinkedSelectionEnabled(
            self.state->linkedSelection);
        return;
    }
    self.state->editDragging = false;
    const RationalTime playhead = self.state->requestedPosition;
    EditError error = EditError::None;
    std::string message;
    if (self.state->interaction->PointerUp(error, message)) {
        [self refreshTimelineAfterEditFromPosition:playhead];
        if (![self persistEdits:message])
            std::fprintf(stderr, "Unable to persist edit: %s\n",
                         message.c_str());
    } else if (error != EditError::None) {
        std::fprintf(stderr, "Edit rejected (%s): %s\n", EditErrorName(error),
                     message.c_str());
    }
    [self updateSelectionInfo];
    self.state->interaction->SetLinkedSelectionEnabled(
        self.state->linkedSelection);
    self.state->overlayDirty = true;
}

- (void)timelineScroll:(NSEvent*)event {
    const NSPoint point = [self timelinePointForEvent:event];
    if (point.y < 0.0 || point.y > [self timelineHeight]) return;
    const double delta = std::abs(event.scrollingDeltaX) > 0.01
                             ? event.scrollingDeltaX
                             : event.scrollingDeltaY;
    try {
        if ((event.modifierFlags & NSEventModifierFlagCommand) != 0) {
            self.state->viewport.ZoomAroundX(point.x, std::exp(-delta * 0.01),
                                             self.state->duration.rate);
        } else {
            self.state->viewport.ScrollByPixels(delta,
                                                self.state->duration.rate);
        }
        self.state->overlayDirty = true;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "Viewport update rejected: %s\n",
                     exception.what());
    }
}

- (BOOL)timelineKeyDown:(NSEvent*)event {
    const NSEventModifierFlags modifiers =
        event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    NSString* characters = event.charactersIgnoringModifiers.lowercaseString;
    if ((modifiers & NSEventModifierFlagCommand) != 0) {
        if ([characters isEqualToString:@"l"] &&
            (modifiers & NSEventModifierFlagShift) != 0) {
            self.linkedSelectionButton.state = self.state->linkedSelection
                                                   ? NSControlStateValueOff
                                                   : NSControlStateValueOn;
            [self linkedSelectionPressed:self.linkedSelectionButton];
            return YES;
        }
        if ([characters isEqualToString:@"t"] &&
            (modifiers & NSEventModifierFlagShift) != 0) {
            [self addTrack:(modifiers & NSEventModifierFlagOption) != 0];
            return YES;
        }
        if (![characters isEqualToString:@"z"]) return NO;
        EditError error = EditError::None;
        std::string message;
        const bool redo = (modifiers & NSEventModifierFlagShift) != 0;
        const RationalTime playhead = self.state->requestedPosition;
        const bool changed = redo ? self.state->editLog.Redo(
                                        self.state->document, error, message)
                                  : self.state->editLog.Undo(
                                        self.state->document, error, message);
        if (changed) {
            [self refreshTimelineAfterEditFromPosition:playhead];
            [self refreshBinControlsSelecting:nil];
            if (![self persistEdits:message])
                std::fprintf(stderr, "Unable to persist undo/redo: %s\n",
                             message.c_str());
            [self updateSelectionInfo];
            self.state->overlayDirty = true;
        } else if (error != EditError::EmptyUndo &&
                   error != EditError::EmptyRedo) {
            std::fprintf(stderr, "%s failed (%s): %s\n", redo ? "Redo" : "Undo",
                         EditErrorName(error), message.c_str());
        }
        return YES;
    }

    if ([characters isEqualToString:@"u"]) {
        [self detachSelectedAudio];
        return YES;
    }

    if (event.keyCode == 51 ||
        event.keyCode == 117) {  // Delete / Forward Delete
        const auto gap = self.state->interaction->SelectedGap();
        const Ulid selectedClipId = self.state->interaction->SelectedClipId();
        const std::vector<Ulid> selectedClipIds =
            self.state->interaction->SelectedClipIds();
        if (!gap && selectedClipIds.empty()) return NO;
        const RationalTime playhead = self.state->requestedPosition;
        EditError error = EditError::None;
        std::string message;
        Operation operation =
            gap ? Operation{DeleteGapOperation{
                      gap->track_id, gap->start, gap->duration, {}}}
                : Operation{RemoveClipOperation{selectedClipId.empty()
                                                    ? selectedClipIds.front()
                                                    : selectedClipId,
                                                {}}};
        if (!gap && self.state->linkedSelection && selectedClipIds.size() > 1) {
            const DocumentClip* first =
                self.state->document.FindClip(selectedClipIds.front());
            const bool oneGroup =
                first && !first->link_group_id.empty() &&
                std::all_of(selectedClipIds.begin(), selectedClipIds.end(),
                            [&](const Ulid& id) {
                                const DocumentClip* clip =
                                    self.state->document.FindClip(id);
                                return clip && clip->link_group_id ==
                                                   first->link_group_id;
                            });
            if (oneGroup)
                operation = RemoveLinkedClipsOperation{
                    first->link_group_id, selectedClipIds, {}};
        }
        if (self.state->editLog.Apply(self.state->document,
                                      std::move(operation), error, message)) {
            if (!gap) self.state->interaction->SelectClip("");
            [self refreshTimelineAfterEditFromPosition:playhead];
            if (![self persistEdits:message])
                std::fprintf(stderr, "Unable to persist gap deletion: %s\n",
                             message.c_str());
            [self updateSelectionInfo];
            self.state->overlayDirty = true;
        } else {
            std::fprintf(stderr, "Delete rejected (%s): %s\n",
                         EditErrorName(error), message.c_str());
        }
        return YES;
    }

    if ([characters isEqualToString:@" "]) {
        if (!event.isARepeat) self.state->spaceUsedForPan = false;
        self.state->spaceHand = true;
        [self applyToolCursor];
        return YES;
    }
    if ([characters isEqualToString:@"v"]) {
        [self setTimelineTool:TimelineTool::Select];
        return YES;
    }
    if ([characters isEqualToString:@"h"]) {
        [self setTimelineTool:TimelineTool::Hand];
        return YES;
    }
    if ([characters isEqualToString:@"z"]) {
        [self setTimelineTool:TimelineTool::Zoom];
        return YES;
    }
    if ([characters isEqualToString:@"c"] ||
        [characters isEqualToString:@"b"]) {
        [self setTimelineTool:TimelineTool::Cut];
        return YES;
    }
    if ([characters isEqualToString:@"f"]) {
        self.state->viewport.FitDuration(self.state->duration,
                                         self.metalView.bounds.size.width);
        self.state->overlayDirty = true;
        return YES;
    }
    if ([characters isEqualToString:@"j"]) {
        [self setPlaybackDirection:-1];
        return YES;
    }
    if ([characters isEqualToString:@"k"]) {
        [self setPlaybackDirection:0];
        return YES;
    }
    if ([characters isEqualToString:@"l"]) {
        [self setPlaybackDirection:1];
        return YES;
    }
    if ([characters isEqualToString:@"n"]) {
        self.state->interaction->SetSnappingEnabled(
            !self.state->interaction->SnappingEnabled());
        [self updateSelectionInfo];
        self.state->overlayDirty = true;
        return YES;
    }
    if ([characters isEqualToString:@"m"]) {
        self.state->playheadResolution =
            self.state->playheadResolution == PlayheadResolution::Frame
                ? PlayheadResolution::Sample
                : PlayheadResolution::Frame;
        if (self.state->playbackDirection == 0)
            [self requestTimelinePosition:self.state->requestedPosition];
        [self updateSelectionInfo];
        self.state->overlayDirty = true;
        return YES;
    }
    if ([characters isEqualToString:@"+"] ||
        [characters isEqualToString:@"="] ||
        [characters isEqualToString:@"-"]) {
        const bool zoomOut = [characters isEqualToString:@"-"];
        const double center = (self.state->viewport.header_width +
                               self.metalView.bounds.size.width) /
                              2.0;
        self.state->viewport.ZoomAroundX(center, zoomOut ? (1.0 / 1.5) : 1.5,
                                         self.state->duration.rate);
        self.state->overlayDirty = true;
        return YES;
    }
    if (event.keyCode == 115) {  // Home
        [self requestTimelinePosition:{0, self.state->duration.rate}];
        return YES;
    }
    if (event.keyCode == 119) {  // End
        [self requestTimelinePosition:self.state->duration];
        return YES;
    }
    if (event.keyCode == 123 || event.keyCode == 124) {  // Left / right
        const int64_t amount =
            (modifiers & NSEventModifierFlagShift) != 0 ? 10 : 1;
        const int64_t direction = event.keyCode == 123 ? -1 : 1;
        const RationalTime delta =
            self.state->playheadResolution == PlayheadResolution::Sample
                ? RationalTime{direction * amount, 48000}
                : RationalTime{
                      direction * amount * [self playheadFrameRate].den,
                      [self playheadFrameRate].num};
        [self requestTimelinePosition:self.state->requestedPosition.add(delta)];
        return YES;
    }
    if (event.keyCode == 53) {  // Escape
        self.state->interaction->CancelDrag();
        self.state->navigationDragging = false;
        self.state->scrubDragging = false;
        self.state->editDragging = false;
        self.state->lassoCandidate = false;
        self.state->lassoDragging = false;
        self.state->overlayDirty = true;
        return YES;
    }
    return NO;
}

- (void)timelineKeyUp:(NSEvent*)event {
    if ([event.charactersIgnoringModifiers isEqualToString:@" "]) {
        self.state->spaceHand = false;
        [self applyToolCursor];
        if (!self.state->spaceUsedForPan) {
            [self setPlaybackDirection:self.state->playbackDirection == 0 ? 1
                                                                          : 0];
        }
    }
}

- (void)menuUndo:(id)sender {
    (void)sender;
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (self.state->editLog.Undo(self.state->document, error, message)) {
        [self refreshTimelineAfterEditFromPosition:playhead];
        [self refreshBinControlsSelecting:nil];
        [self persistEdits:message];
        [self updateSelectionInfo];
    }
}

- (void)menuRedo:(id)sender {
    (void)sender;
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (self.state->editLog.Redo(self.state->document, error, message)) {
        [self refreshTimelineAfterEditFromPosition:playhead];
        [self refreshBinControlsSelecting:nil];
        [self persistEdits:message];
        [self updateSelectionInfo];
    }
}

- (BOOL)deleteCurrentTimelineSelection {
    const auto gap = self.state->interaction->SelectedGap();
    const std::vector<Ulid> ids = self.state->interaction->SelectedClipIds();
    if (!gap && ids.empty()) return NO;
    Operation operation =
        gap ? Operation{DeleteGapOperation{
                  gap->track_id, gap->start, gap->duration, {}}}
            : Operation{RemoveClipOperation{ids.front(), {}}};
    if (!gap && self.state->linkedSelection && ids.size() > 1) {
        const DocumentClip* first = self.state->document.FindClip(ids.front());
        if (first && !first->link_group_id.empty())
            operation =
                RemoveLinkedClipsOperation{first->link_group_id, ids, {}};
    }
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (!self.state->editLog.Apply(self.state->document, std::move(operation),
                                   error, message))
        return NO;
    self.state->interaction->SelectClip("");
    [self refreshTimelineAfterEditFromPosition:playhead];
    [self persistEdits:message];
    [self updateSelectionInfo];
    return YES;
}

- (void)menuDeleteSelection:(id)sender {
    (void)sender;
    if (self.window.firstResponder == self.binOutline)
        [self deleteBinPressed:nil];
    else
        [self deleteCurrentTimelineSelection];
}

- (void)menuCutSelectedAtPlayhead:(id)sender {
    (void)sender;
    const Ulid clipId = self.state->interaction->SelectedClipId();
    if (clipId.empty()) return;
    self.state->contextClipId = clipId;
    self.state->contextTime = self.state->requestedPosition;
    [self contextCutClip:nil];
}

- (void)menuSelectTool:(id)sender {
    (void)sender;
    [self setTimelineTool:TimelineTool::Select];
}
- (void)menuCutTool:(id)sender {
    (void)sender;
    [self setTimelineTool:TimelineTool::Cut];
}
- (void)menuToggleSnapping:(id)sender {
    (void)sender;
    self.state->interaction->SetSnappingEnabled(
        !self.state->interaction->SnappingEnabled());
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}
- (void)menuToggleLinkedSelection:(id)sender {
    (void)sender;
    self.linkedSelectionButton.state = self.state->linkedSelection
                                           ? NSControlStateValueOff
                                           : NSControlStateValueOn;
    [self linkedSelectionPressed:self.linkedSelectionButton];
}
- (void)menuFitTimeline:(id)sender {
    (void)sender;
    self.state->viewport.FitDuration(self.state->duration,
                                     self.metalView.bounds.size.width);
    self.state->overlayDirty = true;
}
- (void)menuAddVideoTrack:(id)sender {
    (void)sender;
    [self addTrack:NO];
}
- (void)menuAddAudioTrack:(id)sender {
    (void)sender;
    [self addTrack:YES];
}
- (void)menuPlayPause:(id)sender {
    (void)sender;
    [self setPlaybackDirection:self.state->playbackDirection == 0 ? 1 : 0];
}
- (void)menuPlayReverse:(id)sender {
    (void)sender;
    [self setPlaybackDirection:-1];
}
- (void)menuStop:(id)sender {
    (void)sender;
    [self setPlaybackDirection:0];
}
- (void)menuPlayForward:(id)sender {
    (void)sender;
    [self setPlaybackDirection:1];
}

- (NSMenu*)timelineMenuForEvent:(NSEvent*)event {
    const NSPoint viewPoint =
        [self.metalView convertPoint:event.locationInWindow fromView:nil];
    const double timelineY =
        self.metalView.bounds.size.height - viewPoint.y - [self videoHeight];
    self.state->contextClipId.clear();
    self.state->contextTrackId.clear();
    self.state->contextGap.reset();
    if (timelineY < 0.0 || timelineY > [self timelineHeight]) return nil;
    const RationalTime time =
        self.state->viewport.XToTime(viewPoint.x, self.state->duration.rate);
    self.state->contextTime = time;
    const auto hit =
        HitTestTimeline(self.state->document, self.state->viewport, viewPoint.x,
                        timelineY, self.metalView.bounds.size.width);
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Timeline"];
    if (hit) {
        self.state->contextClipId = hit->clip_id;
        self.state->contextTrackId = hit->track_id;
        std::vector<Ulid> selection{hit->clip_id};
        if (self.state->linkedSelection)
            selection =
                ExpandLinkedClipSelection(self.state->document, selection);
        self.state->interaction->SelectClips(selection);
        [self updateSelectionInfo];
        [menu addItem:[self menuItem:@"Ouvrir dans le moniteur source"
                              action:@selector(contextOpenClipSource:)
                                 key:@""]];
        [menu addItem:[self menuItem:@"Retrouver dans la médiathèque"
                              action:@selector(contextFindClipInMediaPool:)
                                 key:@""]];
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:[self menuItem:@"Couper ici"
                              action:@selector(contextCutClip:)
                                 key:@""]];
        [menu addItem:[self menuItem:@"Séparer l’audio"
                              action:@selector(detachAudioButtonPressed:)
                                 key:@""]];
        [menu addItem:[self menuItem:@"Supprimer"
                              action:@selector(contextDeleteSelection:)
                                 key:@""]];
        return menu;
    }
    const auto gap = HitTestTimelineGap(
        self.state->document, self.state->viewport, viewPoint.x, timelineY,
        self.metalView.bounds.size.width, self.state->duration.rate);
    if (gap) {
        self.state->contextGap = gap;
        [menu addItem:[self menuItem:@"Fermer le gap"
                              action:@selector(contextCloseGap:)
                                 key:@""]];
        return menu;
    }
    const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
    const NSInteger index = static_cast<NSInteger>(
        (timelineY - kTimelineRulerHeight) / self.state->viewport.track_height);
    if (index >= 0 && index < (NSInteger)tracks.size())
        self.state->contextTrackId = tracks[index]->id;
    [menu addItem:[self menuItem:@"Ajouter une piste vidéo"
                          action:@selector(menuAddVideoTrack:)
                             key:@""]];
    [menu addItem:[self menuItem:@"Ajouter une piste audio"
                          action:@selector(menuAddAudioTrack:)
                             key:@""]];
    [menu addItem:NSMenuItem.separatorItem];
    [menu addItem:[self menuItem:@"Cadrer toute la timeline"
                          action:@selector(menuFitTimeline:)
                             key:@""]];
    return menu;
}

- (void)contextOpenClipSource:(id)sender {
    (void)sender;
    const DocumentClip* clip =
        self.state->document.FindClip(self.state->contextClipId);
    if (clip)
        [self openMediaIdentifierInSourceMonitor:
                  [NSString stringWithUTF8String:clip->source_id.c_str()]];
}

- (void)contextFindClipInMediaPool:(id)sender {
    (void)sender;
    const DocumentClip* clip =
        self.state->document.FindClip(self.state->contextClipId);
    if (!clip) return;
    self.mediaSearchField.stringValue = @"";
    [self refreshBinControlsSelecting:@"__all__"];
    NSString* source = [NSString stringWithUTF8String:clip->source_id.c_str()];
    const NSUInteger row = [self.visibleMediaIds indexOfObject:source];
    if (row != NSNotFound) {
        self.mediaViewToggle.selectedSegment = 0;
        [self mediaViewChanged:nil];
        [self.mediaTable selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                     byExtendingSelection:NO];
        [self.mediaTable scrollRowToVisible:row];
        [self.window makeFirstResponder:self.mediaTable];
    }
}

- (void)contextCutClip:(id)sender {
    (void)sender;
    const DocumentClip* clip =
        self.state->document.FindClip(self.state->contextClipId);
    if (!clip || self.state->contextTime <= clip->timeline_in ||
        self.state->contextTime >= clip->timeline_in.add(clip->duration))
        return;
    Operation operation =
        SplitClipOperation{clip->id, self.state->contextTime, {}};
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (self.state->editLog.Apply(self.state->document, std::move(operation),
                                  error, message)) {
        [self refreshTimelineAfterEditFromPosition:playhead];
        [self persistEdits:message];
        [self updateSelectionInfo];
    }
}

- (void)contextDeleteSelection:(id)sender {
    (void)sender;
    [self deleteCurrentTimelineSelection];
}

- (void)contextCloseGap:(id)sender {
    (void)sender;
    if (!self.state->contextGap) return;
    const TimelineGapSelection gap = *self.state->contextGap;
    self.state->interaction->SelectClip("");
    Operation operation =
        DeleteGapOperation{gap.track_id, gap.start, gap.duration, {}};
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (self.state->editLog.Apply(self.state->document, std::move(operation),
                                  error, message)) {
        [self refreshTimelineAfterEditFromPosition:playhead];
        [self persistEdits:message];
        [self updateSelectionInfo];
    }
}

- (void)advancePlayback {
    if (self.state->playbackDirection == 0) return;
    const auto now = std::chrono::steady_clock::now();
    const int64_t elapsedUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            now - self.state->playbackStarted)
            .count();
    if (elapsedUs <= 0) return;
    const RationalTime delta{
        elapsedUs * static_cast<int64_t>(self.state->playbackDirection),
        1000000};
    const RationalTime next = self.state->playbackAnchor.add(delta);
    if (next <= RationalTime{0, 1}) {
        [self requestTimelinePosition:{0, self.state->duration.rate}];
        [self setPlaybackDirection:0];
    } else if (next >= self.state->duration) {
        [self requestTimelinePosition:self.state->duration];
        [self setPlaybackDirection:0];
    } else {
        [self requestTimelinePosition:next];
    }
}

- (void)displayTick:(NSTimer*)timer {
    (void)timer;
    [self advancePlayback];
    [self presentNearestFrameAtDeadline:YES];
}

- (TimelineRenderData)timelineRenderData {
    TimelineRenderData data;
    data.color_management = self.state->document.color_management;
    const double width = self.metalView.bounds.size.width;
    const double timelineHeight = [self timelineHeight];
    data.video_height = [self videoHeight];
    const double top = data.video_height;
    auto add = [&](double x, double y, double w, double h, float r, float g,
                   float b, float a = 1.0f) {
        if (w > 0.0 && h > 0.0)
            data.rectangles.push_back({x, y, w, h, r, g, b, a});
    };
    const auto addTinyText = [&](double x, double y, const std::string& text) {
        const auto maskFor = [](char character) -> uint8_t {
            static constexpr uint8_t digits[] = {0x3f, 0x06, 0x5b, 0x4f, 0x66,
                                                 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
            if (character >= '0' && character <= '9')
                return digits[character - '0'];
            if (character == 'f') return 0x71;
            if (character == 's') return 0x6d;
            if (character == 'm') return 0x37;
            if (character == 'p') return 0x73;
            if (character == '-') return 0x40;
            return 0;
        };
        double cursor = x;
        for (char character : text) {
            if (character == '+') {
                add(cursor + 1.0, y + 3.0, 3.0, 1.0, 1.0f, 0.93f, 0.84f);
                add(cursor + 2.0, y + 2.0, 1.0, 3.0, 1.0f, 0.93f, 0.84f);
                cursor += 6.0;
                continue;
            }
            const uint8_t mask = maskFor(character);
            const auto segment = [&](uint8_t bit, double sx, double sy,
                                     double sw, double sh) {
                if (mask & bit)
                    add(cursor + sx, y + sy, sw, sh, 1.0f, 0.93f, 0.84f);
            };
            segment(0x01, 1, 0, 3, 1);
            segment(0x02, 4, 1, 1, 2);
            segment(0x04, 4, 4, 1, 2);
            segment(0x08, 1, 6, 3, 1);
            segment(0x10, 0, 4, 1, 2);
            segment(0x20, 0, 1, 1, 2);
            segment(0x40, 1, 3, 3, 1);
            cursor += 6.0;
        }
    };

    add(0.0, top - 2.0, width, 2.0, 0.04f, 0.045f, 0.052f);
    add(0.0, top, width, timelineHeight, 0.055f, 0.061f, 0.070f);
    add(0.0, top, width, kTimelineRulerHeight, 0.105f, 0.116f, 0.132f);
    for (int index = 0; index < 4; ++index) {
        const bool active = static_cast<int>([self effectiveTool]) == index;
        add(index * 24.0, top, 24.0, kTimelineRulerHeight,
            active ? 0.12f : 0.075f, active ? 0.42f : 0.086f,
            active ? 0.62f : 0.105f);
        add(index * 24.0 + 23.0, top, 1.0, kTimelineRulerHeight, 0.22f, 0.23f,
            0.25f);
    }
    // Tool glyphs: selection arrow, hand/pan and magnifier. They remain
    // geometry in the Metal pass, not AppKit controls.
    add(7.0, top + 5.0, 4.0, 13.0, 0.92f, 0.93f, 0.96f);
    add(10.0, top + 14.0, 7.0, 4.0, 0.92f, 0.93f, 0.96f);
    add(31.0, top + 9.0, 10.0, 10.0, 0.92f, 0.93f, 0.96f);
    add(33.0, top + 5.0, 2.0, 7.0, 0.92f, 0.93f, 0.96f);
    add(37.0, top + 5.0, 2.0, 7.0, 0.92f, 0.93f, 0.96f);
    add(54.0, top + 5.0, 10.0, 2.0, 0.92f, 0.93f, 0.96f);
    add(54.0, top + 15.0, 10.0, 2.0, 0.92f, 0.93f, 0.96f);
    add(54.0, top + 7.0, 2.0, 8.0, 0.92f, 0.93f, 0.96f);
    add(62.0, top + 7.0, 2.0, 8.0, 0.92f, 0.93f, 0.96f);
    add(63.0, top + 16.0, 5.0, 2.0, 0.92f, 0.93f, 0.96f);
    // Razor blade.
    add(78.0, top + 6.0, 11.0, 3.0, 0.92f, 0.93f, 0.96f);
    add(80.0, top + 9.0, 7.0, 8.0, 0.92f, 0.93f, 0.96f);
    add(77.0, top + 16.0, 13.0, 2.0, 0.92f, 0.93f, 0.96f);
    const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
    for (size_t index = 0; index < tracks.size(); ++index) {
        const double y = top + kTimelineRulerHeight +
                         index * self.state->viewport.track_height;
        if (y >= top + timelineHeight) break;
        const float trackShade = index % 2 == 0 ? 0.075f : 0.088f;
        add(0.0, y, width, self.state->viewport.track_height, trackShade,
            trackShade + 0.006f, trackShade + 0.012f);
        add(0.0, y, self.state->viewport.header_width,
            self.state->viewport.track_height, 0.105f, 0.116f, 0.132f);
        const bool video = tracks[index]->kind == "video";
        add(0.0, y, 4.0, self.state->viewport.track_height,
            video ? 0.12f : 0.18f, video ? 0.48f : 0.62f,
            video ? 0.78f : 0.35f);
        add(10.0, y + 9.0, 27.0, 26.0, 0.15f, 0.17f, 0.20f);
        add(15.0, y + 15.0, 17.0, 3.0, video ? 0.20f : 0.30f,
            video ? 0.58f : 0.72f, video ? 0.86f : 0.42f);
        add(15.0, y + 23.0, 12.0, 3.0, 0.48f, 0.51f, 0.56f);
        add(self.state->viewport.header_width - 44.0, y + 14.0, 10.0, 10.0,
            0.19f, 0.21f, 0.24f);
        add(self.state->viewport.header_width - 25.0, y + 14.0, 10.0, 10.0,
            0.19f, 0.21f, 0.24f);
        add(0.0, y + self.state->viewport.track_height - 1.0, width, 1.0, 0.13f,
            0.145f, 0.165f);
    }
    const double addTrackY = top + kTimelineRulerHeight +
                             tracks.size() * self.state->viewport.track_height;
    if (addTrackY < top + timelineHeight) {
        add(0.0, addTrackY, width, kAddTrackRowHeight, 0.058f, 0.064f, 0.073f);
        add(0.0, addTrackY, self.state->viewport.header_width,
            kAddTrackRowHeight, 0.09f, 0.10f, 0.115f);
        const double split = self.state->viewport.header_width * 0.5;
        add(split, addTrackY, 1.0, kAddTrackRowHeight, 0.16f, 0.18f, 0.20f);
        for (int half = 0; half < 2; ++half) {
            const double centerX = split * (half + 0.5);
            const float red = half == 0 ? 0.30f : 0.36f;
            const float green = half == 0 ? 0.68f : 0.78f;
            const float blue = half == 0 ? 0.90f : 0.48f;
            add(centerX - 6.0, addTrackY + 11.0, 12.0, 2.0, red, green, blue);
            add(centerX - 1.0, addTrackY + 6.0, 2.0, 12.0, red, green, blue);
        }
    }
    add(self.state->viewport.header_width - 1.0, top, 1.0, timelineHeight,
        0.26f, 0.26f, 0.28f);
    const std::vector<double> tickXs = self.state->viewport.TickXs(width);
    for (size_t tickIndex = 0; tickIndex < tickXs.size(); ++tickIndex) {
        const double tickX = tickXs[tickIndex];
        add(tickX, top, 1.0, kTimelineRulerHeight, 0.46f, 0.47f, 0.50f);
        add(tickX, top + kTimelineRulerHeight - 5.0, 1.0, 5.0, 0.65f, 0.66f,
            0.70f);
        if (tickIndex + 1 < tickXs.size()) {
            const double interval = tickXs[tickIndex + 1] - tickX;
            for (int subdivision = 1; subdivision < 4; ++subdivision) {
                add(tickX + interval * subdivision / 4.0,
                    top + kTimelineRulerHeight - 4.0, 1.0, 4.0, 0.30f, 0.32f,
                    0.35f);
            }
        }
    }

    if (const auto& gap = self.state->interaction->SelectedGap()) {
        const auto track = std::find_if(tracks.begin(), tracks.end(),
                                        [&](const DocumentTrack* value) {
                                            return value->id == gap->track_id;
                                        });
        if (track != tracks.end()) {
            const double rawLeft = self.state->viewport.TimeToX(gap->start);
            const double rawRight =
                self.state->viewport.TimeToX(gap->start.add(gap->duration));
            const double left = std::max(self.state->viewport.header_width,
                                         std::min(rawLeft, rawRight));
            const double right = std::min(width, std::max(rawLeft, rawRight));
            const double y = top + kTimelineRulerHeight +
                             std::distance(tracks.begin(), track) *
                                 self.state->viewport.track_height;
            if (right > left && y < top + timelineHeight) {
                const double height = self.state->viewport.track_height;
                add(left, y, right - left, height, 0.10f, 0.34f, 0.48f, 0.42f);
                add(left, y, right - left, 2.0, 0.24f, 0.82f, 1.0f);
                add(left, y + height - 2.0, right - left, 2.0, 0.24f, 0.82f,
                    1.0f);
                add(left, y, 2.0, height, 0.24f, 0.82f, 1.0f);
                add(right - 2.0, y, 2.0, height, 0.24f, 0.82f, 1.0f);
                for (double stripe = left + 8.0; stripe < right; stripe += 12.0)
                    add(stripe, y + 5.0, 1.0, std::max(0.0, height - 10.0),
                        0.20f, 0.58f, 0.72f, 0.55f);
            }
        }
    }

    const auto clips =
        VisibleTimelineClips(self.state->document, self.state->viewport, width,
                             self.state->interaction->SelectedClipIds(),
                             self.state->interaction->TrimPreview(),
                             self.state->interaction->MovePreview());
    const bool singleClipSelection =
        self.state->interaction->SelectedClipIds().size() == 1;
    for (const TimelineClipRect& clip : clips) {
        double left = std::min(clip.x, clip.x + clip.width);
        double right = std::max(clip.x, clip.x + clip.width);
        const bool headVisible =
            left >= self.state->viewport.header_width && left <= width;
        const bool tailVisible =
            right >= self.state->viewport.header_width && right <= width;
        left = std::max(left, self.state->viewport.header_width);
        right = std::min(right, width);
        if (right <= left) continue;
        const double y = top + clip.y;
        if (y >= top + timelineHeight) continue;
        add(left + 1.0, y + 2.0, std::max(0.0, right - left), clip.height,
            0.015f, 0.018f, 0.022f, 0.65f);
        if (clip.moving) {
            add(left - 2.0, y - 2.0, right - left + 4.0, clip.height + 4.0,
                0.86f, 0.16f, 0.12f, 0.82f);
        }
        if (clip.selected) {
            add(left - 2.0, y - 2.0, right - left + 4.0, clip.height + 4.0,
                clip.valid ? 0.95f : 1.0f, clip.valid ? 0.78f : 0.16f,
                clip.valid ? 0.18f : 0.12f);
        }
        const auto color = ClipColor(clip.source_id, clip.audio);
        if (!clip.valid)
            add(left, y, right - left, clip.height, 0.72f, 0.08f, 0.08f, 0.92f);
        else
            add(left, y, right - left, clip.height,
                std::min(1.0f, color[0] + (clip.preview ? 0.12f : 0.0f)),
                std::min(1.0f, color[1] + (clip.preview ? 0.12f : 0.0f)),
                std::min(1.0f, color[2] + (clip.preview ? 0.12f : 0.0f)));
        add(left, y, right - left, 3.0, std::min(1.0f, color[0] + 0.22f),
            std::min(1.0f, color[1] + 0.22f), std::min(1.0f, color[2] + 0.22f));
        if (clip.audio && clip.sync_drift && clip.sync_drift->value != 0) {
            const std::string label =
                SyncDriftLabel(*clip.sync_drift, [self playheadFrameRate]);
            const double badgeWidth = label.size() * 6.0 + 4.0;
            if (right - left >= badgeWidth + 6.0) {
                const double badgeX = left + 5.0;
                const double badgeY = y + 7.0;
                add(badgeX, badgeY, badgeWidth, 11.0, 0.66f, 0.12f, 0.07f,
                    0.94f);
                addTinyText(badgeX + 2.0, badgeY + 2.0, label);
            } else {
                add(left + 3.0, y + 7.0, 3.0, std::min(11.0, clip.height - 9.0),
                    0.94f, 0.18f, 0.08f);
            }
        }
        // Every edit boundary must remain readable when adjacent clips share
        // the same source color. Fixed-point dark edges create a clear cut
        // seam without depending on zoom or inserting a fake timeline gap.
        const double outlineWidth = std::min(1.0, right - left);
        add(left, y, outlineWidth, clip.height, 0.025f, 0.029f, 0.035f, 0.96f);
        add(right - outlineWidth, y, outlineWidth, clip.height, 0.025f, 0.029f,
            0.035f, 0.96f);
        add(left, y + clip.height - 1.0, right - left, 1.0, 0.025f, 0.029f,
            0.035f, 0.90f);
        if (clip.selected && singleClipSelection) {
            // Resolve-style trim handles: fixed point sizes, independent of
            // zoom, with a grip bar and short top/bottom caps.
            const double span = right - left;
            const double gripWidth = std::min(3.0, span / 2.0);
            const double capWidth = std::min(7.0, span);
            if (headVisible) {
                add(left, y, std::min(6.0, span), clip.height, 0.08f, 0.08f,
                    0.09f, 0.42f);
                add(left, y + 2.0, gripWidth, std::max(0.0, clip.height - 4.0),
                    1.0f, 0.82f, 0.18f);
                add(left, y, capWidth, 2.0, 1.0f, 0.82f, 0.18f);
                add(left, y + clip.height - 2.0, capWidth, 2.0, 1.0f, 0.82f,
                    0.18f);
            }
            if (tailVisible) {
                add(std::max(left, right - 6.0), y, std::min(6.0, span),
                    clip.height, 0.08f, 0.08f, 0.09f, 0.42f);
                add(right - gripWidth, y + 2.0, gripWidth,
                    std::max(0.0, clip.height - 4.0), 1.0f, 0.82f, 0.18f);
                add(right - capWidth, y, capWidth, 2.0, 1.0f, 0.82f, 0.18f);
                add(right - capWidth, y + clip.height - 2.0, capWidth, 2.0,
                    1.0f, 0.82f, 0.18f);
            }
        }
    }

    if (self.state->lassoDragging) {
        const double left = std::max(
            self.state->viewport.header_width,
            std::min(self.state->lassoStartX, self.state->lassoCurrentX));
        const double right = std::min(
            width,
            std::max(self.state->lassoStartX, self.state->lassoCurrentX));
        const double lassoTop = std::max(
            kTimelineRulerHeight,
            std::min(self.state->lassoStartY, self.state->lassoCurrentY));
        const double bottom =
            std::max(self.state->lassoStartY, self.state->lassoCurrentY);
        if (right > left && bottom > lassoTop) {
            add(left, top + lassoTop, right - left, bottom - lassoTop, 0.12f,
                0.52f, 0.78f, 0.18f);
            add(left, top + lassoTop, right - left, 1.0, 0.28f, 0.78f, 1.0f);
            add(left, top + bottom - 1.0, right - left, 1.0, 0.28f, 0.78f,
                1.0f);
            add(left, top + lassoTop, 1.0, bottom - lassoTop, 0.28f, 0.78f,
                1.0f);
            add(right - 1.0, top + lassoTop, 1.0, bottom - lassoTop, 0.28f,
                0.78f, 1.0f);
        }
    }

    if (self.state->interaction->SnapGuideTime()) {
        const double snapX = self.state->viewport.TimeToX(
            *self.state->interaction->SnapGuideTime());
        if (snapX >= self.state->viewport.header_width && snapX <= width)
            add(snapX, top + kTimelineRulerHeight, 1.0,
                timelineHeight - kTimelineRulerHeight, 0.15f, 0.88f, 1.0f);
    }
    if (self.state->cutPreviewX && self.state->cutPreviewY) {
        add(*self.state->cutPreviewX - 1.0, top + *self.state->cutPreviewY, 2.0,
            self.state->viewport.track_height, 1.0f, 0.16f, 0.12f);
    }

    const double playheadX =
        self.state->viewport.TimeToX(self.state->requestedPosition);
    if (playheadX >= self.state->viewport.header_width && playheadX <= width) {
        add(playheadX - 4.0, top, 8.0, 6.0, 1.0f, 0.20f, 0.14f);
        add(playheadX - 1.0, top, 2.0, timelineHeight, 1.0f, 0.22f, 0.16f);
    }
    return data;
}

- (void)presentNearestFrameAtDeadline:(BOOL)isDisplayDeadline {
    if (!self.state->frameCache || !self.state->renderer) {
        return;
    }

    std::vector<AVFrame*> frames(self.state->requested.size(), nullptr);
    std::vector<RenderedSlot> candidates(self.state->requested.size());
    bool missing = false;
    for (size_t slot = 0; slot < self.state->requested.size(); ++slot) {
        const ResolvedSlot& requested = self.state->requested[slot];
        if (!requested.active) {
            continue;
        }
        int64_t cachedFrame = -1;
        frames[slot] = self.state->frameCache->GetNearest(
            requested.sourceId, requested.frame, cachedFrame);
        candidates[slot] = {true, requested.sourceId, cachedFrame};
        if (!frames[slot] || cachedFrame != requested.frame) {
            missing = true;
        }
    }
    if (isDisplayDeadline && missing) {
        self.state->performanceMetrics->RecordDrop();
    }
    if (self.state->overlayDirty || candidates != self.state->rendered) {
        TimelineRenderData timelineData = [self timelineRenderData];
        timelineData.video_rotation_degrees.resize(candidates.size(), 0);
        for (size_t slot = 0; slot < candidates.size(); ++slot) {
            if (!candidates[slot].active) continue;
            const auto media =
                self.state->mediaMetadata.find(candidates[slot].sourceId);
            if (media != self.state->mediaMetadata.end() &&
                media->second.metadata_complete)
                timelineData.video_rotation_degrees[slot] =
                    media->second.rotation_degrees;
        }
        if (self.state->renderer->RenderFrames(frames, timelineData)) {
            self.state->rendered = candidates;
            self.state->overlayDirty = false;
        }
    }
    for (AVFrame*& frame : frames) av_frame_free(&frame);
}

- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    if (self.state->renderer) {
        self.state->renderer->Resize(self.metalView.bounds);
        self.state->overlayDirty = true;
    }
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    (void)notification;
    if (self.state->audioPlayback) self.state->audioPlayback->Stop();
    [self.displayTimer invalidate];
    self.displayTimer = nil;
    for (auto& worker : self.state->workers) {
        worker.second->Stop();
    }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return YES;
}

- (void)dealloc {
    [_displayTimer invalidate];
    delete _state;
}

@end

int main(int argc, char* argv[]) {
    if (argc == 3 && std::string(argv[1]) == "--describe") {
        std::string output;
        const int result = DescribeCommand(argv[2], output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc == 4 && std::string(argv[1]) == "--apply-op") {
        std::string output;
        const int result = ApplyOperationCommand(argv[2], argv[3], output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if ((argc == 4 || argc == 5) && std::string(argv[1]) == "--ingest" &&
        (argc == 4 || std::string(argv[4]) == "--recursive")) {
        std::string output;
        const int result = IngestCommand(argv[2], argv[3], argc == 5, output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc != 2 || (argc >= 2 && argv[1][0] == '-')) {
        std::fprintf(stderr,
                     "Usage: %s /path/to/timeline.json\n"
                     "       %s --describe /path/to/timeline.json\n"
                     "       %s --apply-op /path/to/timeline.json '<op.json>'\n"
                     "       %s --ingest /path/to/timeline.json /path/to/media "
                     "[--recursive]\n",
                     argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }

    @autoreleasepool {
        [NSApplication sharedApplication];
        NSString* documentPath = [NSString stringWithUTF8String:argv[1]];
        AppDelegate* delegate =
            [[AppDelegate alloc] initWithDocumentPath:documentPath];
        NSApp.delegate = delegate;
        [NSApp run];
    }
    return 0;
}
```

### src/shader.metal

```metal
#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut vertex_main(uint vertexId [[vertex_id]]) {
    const float2 positions[6] = {
        float2(-1.0, -1.0), float2( 1.0, -1.0), float2( 1.0,  1.0),
        float2(-1.0, -1.0), float2( 1.0,  1.0), float2(-1.0,  1.0),
    };
    const float2 uvs[6] = {
        float2(0.0, 1.0), float2(1.0, 1.0), float2(1.0, 0.0),
        float2(0.0, 1.0), float2(1.0, 0.0), float2(0.0, 0.0),
    };
    VertexOut out;
    out.position = float4(positions[vertexId], 0.0, 1.0);
    out.uv = uvs[vertexId];
    return out;
}

float3 sampleYUV(float2 uv,
                   texture2d<float> yTexture,
                   texture2d<float> uTexture,
                   texture2d<float> vTexture,
                   sampler planeSampler,
                   float redFromCr,
                   float greenFromCb,
                   float greenFromCr,
                   float blueFromCb,
                   float sampleScale,
                   float yOffset,
                   float yScale,
                   float chromaOffset,
                   float chromaScale) {
    // R16 textures normalize against 65535 although FFmpeg stores 9-12 bit
    // planar samples in the low bits. sampleScale restores code/maxCode.
    const float yCode = yTexture.sample(planeSampler, uv).r * sampleScale;
    const float uCode = uTexture.sample(planeSampler, uv).r * sampleScale;
    const float vCode = vTexture.sample(planeSampler, uv).r * sampleScale;
    const float y = (yCode - yOffset) * yScale;
    const float cb = (uCode - chromaOffset) * chromaScale;
    const float cr = (vCode - chromaOffset) * chromaScale;

    return float3(y + redFromCr * cr,
                  y + greenFromCb * cb + greenFromCr * cr,
                  y + blueFromCb * cb);
}

struct PresentationParameters {
    float left;
    float top;
    float width;
    float height;
    int quarterTurns;
    float opacity;
    int colorManagementEnabled;
    int inputGamut;
    int inputTransfer;
    int useAcescct;
    float redFromCr;
    float greenFromCb;
    float greenFromCr;
    float blueFromCb;
    float sampleScale;
    float yOffset;
    float yScale;
    float chromaOffset;
    float chromaScale;
    float2 padding;
};

float3 decodeTransfer(float3 signal, int transfer) {
    if (transfer == 2) return signal;
    if (transfer == 1) {
        // Sony's published full-range formula. 420/1023 maps to 18% scene
        // reflection and the linear branch changes at 171.2102946929/1023.
        const float3 high =
            pow(10.0, (signal * 1023.0 - 420.0) / 261.5) * 0.19 - 0.01;
        const float3 low = (signal * 1023.0 - 95.0) * 0.01125 /
                           (171.2102946929 - 95.0);
        return select(low, high, signal >= 171.2102946929 / 1023.0);
    }
    const float3 positive = max(signal, 0.0);
    return select(positive / 4.5,
                  pow((positive + 0.099) / 1.099, 1.0 / 0.45),
                  positive >= 0.081);
}

float3 mulRows(float3 value, float3 row0, float3 row1, float3 row2) {
    return float3(dot(value, row0), dot(value, row1), dot(value, row2));
}

float3 sourceToAP1(float3 rgb, int gamut) {
    float3 ap0;
    if (gamut == 1) { // ACES reference CSC: S-Gamut3.Cine to ACES2065-1.
        ap0 = mulRows(rgb,
                     float3(0.6387886672, 0.2723514337, 0.0888598991),
                     float3(-0.0039159060, 1.0880732309, -0.0841573249),
                     float3(-0.0299072021, -0.0264325799, 1.0563397820));
    } else if (gamut == 2) { // ACES reference CSC: S-Gamut3 to ACES2065-1.
        ap0 = mulRows(rgb,
                     float3(0.7529825954, 0.1433702162, 0.1036471884),
                     float3(0.0217076974, 1.0153188355, -0.0370265329),
                     float3(-0.0094160527, 0.0033704179, 1.0060456349));
    } else {
        float3 xyzD65;
        if (gamut == 3) {
            xyzD65 = mulRows(rgb,
                            float3(0.636958, 0.144617, 0.168881),
                            float3(0.262700, 0.677998, 0.059302),
                            float3(0.000000, 0.028073, 1.060985));
        } else {
            xyzD65 = mulRows(rgb,
                            float3(0.412391, 0.357584, 0.180481),
                            float3(0.212639, 0.715169, 0.072192),
                            float3(0.019331, 0.119195, 0.950532));
        }
        const float3 xyzD60 = mulRows(
            xyzD65,
            float3(1.013030, 0.006105, -0.014971),
            float3(0.007698, 0.998165, -0.005032),
            float3(-0.002841, 0.004685, 0.924507));
        ap0 = mulRows(xyzD60,
                      float3(1.049811, 0.000000, -0.000097),
                      float3(-0.495903, 1.373313, 0.098240),
                      float3(0.000000, 0.000000, 0.991252));
    }
    // ACES2065-1 (AP0) to the AP1 primaries shared by ACEScg/ACEScct.
    return mulRows(ap0,
                   float3(1.4514393161, -0.2365107469, -0.2149285693),
                   float3(-0.0765537734, 1.1762296998, -0.0996759264),
                   float3(0.0083161484, -0.0060324498, 0.9977163014));
}

float3 linearAP1ToACEScct(float3 value) {
    const float3 high = (log2(value) + 9.72) / 17.52;
    const float3 low = value * 10.5402377416545 + 0.0729055341958355;
    return select(low, high, value > 0.0078125);
}

float3 acesCctToLinearAP1(float3 value) {
    constexpr float breakpoint = 0.155251141552511;
    const float3 high = exp2(value * 17.52 - 9.72);
    const float3 low =
        (value - 0.0729055341958355) / 10.5402377416545;
    return select(low, high, value > breakpoint);
}

float3 ap1ToOutput(float3 ap1, int outputGamut) {
    const float3 xyzD60 = mulRows(
        ap1,
        float3(0.6624541811, 0.1340042065, 0.1561876870),
        float3(0.2722287168, 0.6740817658, 0.0536895174),
        float3(-0.0055746495, 0.0040607335, 1.0103391003));
    const float3 xyzD65 = mulRows(
        xyzD60,
        float3(0.9872240087, -0.0061132286, 0.0159532883),
        float3(-0.0075983718, 1.0018614847, 0.0053300358),
        float3(0.0030725771, -0.0050959615, 1.0816806031));
    if (outputGamut == 1) {
        return mulRows(xyzD65,
                       float3(1.716651, -0.355671, -0.253366),
                       float3(-0.666684, 1.616481, 0.015769),
                       float3(0.017640, -0.042771, 0.942103));
    }
    return mulRows(xyzD65,
                   float3(3.240970, -1.537383, -0.498611),
                   float3(-0.969244, 1.875968, 0.041555),
                   float3(0.055630, -0.203977, 1.056972));
}

float3 encodeOutput(float3 linear, int transfer) {
    linear = max(linear, 0.0);
    if (transfer == 1) {
        constexpr float a = 0.17883277;
        constexpr float b = 0.28466892;
        constexpr float c = 0.55991073;
        return clamp(select(sqrt(3.0 * linear),
                            a * log(12.0 * linear - b) + c,
                            linear > (1.0 / 12.0)),
                     0.0, 1.0);
    }
    return clamp(select(4.5 * linear,
                        1.099 * pow(linear, 0.45) - 0.099,
                        linear >= 0.018),
                 0.0, 1.0);
}

bool presentationUV(float2 outputUV, float left, float top, float width,
                    float height, int quarterTurns, thread float2& codedUV) {
    if (outputUV.x < left || outputUV.x > left + width ||
        outputUV.y < top || outputUV.y > top + height)
        return false;
    const float2 displayUV =
        (outputUV - float2(left, top)) / float2(width, height);
    switch (quarterTurns) {
        case 1: codedUV = float2(1.0 - displayUV.y, displayUV.x); break;
        case 2: codedUV = float2(1.0 - displayUV.x,
                                 1.0 - displayUV.y); break;
        case 3: codedUV = float2(displayUV.y, 1.0 - displayUV.x); break;
        default: codedUV = displayUV; break;
    }
    return true;
}

fragment float4 fragment_working(VertexOut in [[stage_in]],
                              texture2d<float> yTexture [[texture(0)]],
                              texture2d<float> uTexture [[texture(1)]],
                              texture2d<float> vTexture [[texture(2)]],
                              sampler planeSampler [[sampler(0)]],
                              constant PresentationParameters& parameters [[buffer(0)]]) {
    float2 codedUV;
    if (!presentationUV(in.uv, parameters.left, parameters.top,
                        parameters.width, parameters.height,
                        parameters.quarterTurns, codedUV))
        return float4(0.0);
    float3 color = sampleYUV(
        codedUV, yTexture, uTexture, vTexture, planeSampler,
        parameters.redFromCr, parameters.greenFromCb, parameters.greenFromCr,
        parameters.blueFromCb, parameters.sampleScale,
        parameters.yOffset, parameters.yScale, parameters.chromaOffset,
        parameters.chromaScale);
    if (parameters.colorManagementEnabled != 0) {
        color = sourceToAP1(decodeTransfer(color, parameters.inputTransfer),
                            parameters.inputGamut);
        if (parameters.useAcescct != 0) {
            // Creative operations belong between these two calls. Composite
            // storage remains scene-linear AP1 so alpha blending is correct.
            color = acesCctToLinearAP1(linearAP1ToACEScct(color));
        }
    } else {
        color = clamp(color, 0.0, 1.0);
    }
    return float4(color, clamp(parameters.opacity, 0.0, 1.0));
}

struct OutputParameters {
    int colorManagementEnabled;
    int outputGamut;
    int outputTransfer;
    int padding;
};

fragment float4 fragment_output(
    VertexOut in [[stage_in]],
    texture2d<float> workingTexture [[texture(0)]],
    sampler textureSampler [[sampler(0)]],
    constant OutputParameters& parameters [[buffer(0)]]) {
    float4 working = workingTexture.sample(textureSampler, in.uv);
    if (parameters.colorManagementEnabled == 0) return working;
    float3 output = ap1ToOutput(working.rgb, parameters.outputGamut);
    if (parameters.outputTransfer == 1) {
        // BT.2408 places HDR Reference White at HLG signal 0.75. Sony scene
        // reflection 0.9 is therefore scaled to inverseOETF(0.75).
        output *= 0.2944028442;
    }
    return float4(encodeOutput(output, parameters.outputTransfer), working.a);
}

struct SolidParameters {
    float4 rect;
    float4 color;
    float2 drawableSize;
    float2 padding;
    int outputTransfer;
    int3 colorPadding;
};

struct SolidVertexOut {
    float4 position [[position]];
};

vertex SolidVertexOut vertex_solid(
    uint vertexId [[vertex_id]],
    constant SolidParameters& parameters [[buffer(0)]]) {
    const float2 corners[6] = {
        float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
        float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
    };
    const float2 pixel = parameters.rect.xy +
                         corners[vertexId] * parameters.rect.zw;
    const float2 ndc = float2(pixel.x / parameters.drawableSize.x * 2.0 - 1.0,
                              1.0 - pixel.y / parameters.drawableSize.y * 2.0);
    SolidVertexOut out;
    out.position = float4(ndc, 0.0, 1.0);
    return out;
}

fragment float4 fragment_solid(
    constant SolidParameters& parameters [[buffer(0)]]) {
    float3 rgb = parameters.color.rgb;
    if (parameters.outputTransfer == 1) {
        // UI is SDR-authored. Map its white to HLG Reference White (signal
        // 0.75), rather than HDR peak white, to keep the timeline comfortable.
        rgb = select(rgb / 12.92, pow((rgb + 0.055) / 1.055, 2.4),
                     rgb > 0.04045);
        rgb = encodeOutput(rgb * 0.2649625598, 1);
    }
    return float4(rgb, parameters.color.a);
}
```

### tests/audio_playback_tests.mm

```objectivec
#include "AudioPlayback.h"
#include "TimelineView.h"
#include "Ulid.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

std::string Quote(const std::filesystem::path& path) {
    std::string result = "'";
    for (char character : path.string())
        result += character == '\'' ? "'\\''" : std::string(1, character);
    return result + "'";
}

}  // namespace

int main() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       (GenerateUlid() + "-audio-playback");
    const std::filesystem::path media = root / "tone.mp4";
    std::filesystem::create_directories(root);
    const std::string generate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error "
        "-f lavfi -i 'color=c=black:s=32x32:r=25:d=1' "
        "-f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=1' "
        "-c:v mpeg4 -c:a aac -shortest " +
        Quote(media);
    if (std::system(generate.c_str()) != 0) {
        std::cerr << "FAIL: unable to generate audio fixture\n";
        return 1;
    }

    Document document;
    const Ulid sourceId = "01KA0000000000000000000001";
    document.sources = {{sourceId, "tone.mp4", {25, 1}, {25, 25}}};
    document.tracks = {
        {"01KA0000000000000000000002",
         "video",
         0,
         {{"01KA0000000000000000000003",
           sourceId,
           {0, 25},
           {25, 25},
           {0, 25},
           false}}},
        {"01KA0000000000000000000004",
         "audio",
         1,
         {{"01KA0000000000000000000005",
           sourceId,
           {0, 25},
           {25, 25},
           {0, 25},
           true}}},
    };
    AudioPlayback playback;
    std::string error;
    const bool opened = playback.Open(document, root.string(), error);
    const bool decoded = playback.DecodedSourceCount() == 1;
    const bool planned = playback.PlannedClipCount() == 1;
    const bool started = opened && playback.PlayFrom({0, 25}, 1, error);
    playback.Stop();
    const RationalTime firstMousePosition = QuantizePlayheadPosition(
        {121, 250}, PlayheadResolution::Frame, {25, 1});
    const RationalTime sameFrameMousePosition = QuantizePlayheadPosition(
        {124, 250}, PlayheadResolution::Frame, {25, 1});
    const RationalTime nextFrameMousePosition = QuantizePlayheadPosition(
        {126, 250}, PlayheadResolution::Frame, {25, 1});
    const bool scrubbed = opened && playback.ScrubAt(firstMousePosition, error);
    const bool sameFrameSuppressed =
        firstMousePosition == sameFrameMousePosition &&
        playback.ScrubAt(sameFrameMousePosition, error) &&
        playback.ScrubTriggerCount() == 1;
    const bool nextFrameTriggered =
        nextFrameMousePosition != firstMousePosition &&
        playback.ScrubAt(nextFrameMousePosition, error) &&
        playback.ScrubTriggerCount() == 2;
    playback.Stop();
    std::filesystem::remove_all(root);
    if (!opened || !decoded || !planned || !started || !scrubbed ||
        !sameFrameSuppressed || !nextFrameTriggered) {
        std::cerr << "FAIL: audio decode/mix plan: " << error << '\n';
        return 1;
    }
    std::cout << "PASS: audio source decodes and enters the timeline mix\n";
    return 0;
}
```

### tests/cli_tests.cc

```cpp
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
```

### tests/edit_tests.cc

```cpp
#include "Document.h"
#include "EditLog.h"
#include "Operations.h"
#include "Timeline.h"

#include <cstdint>
#include <filesystem>
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

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    try {
        function();
        if (failures == before) std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: " << name << ": " << exception.what() << '\n';
    }
}

Document EditDocument() {
    Document document;
    document.sources = {
        {"01K20000000000000000000001", "A.MP4", {25, 1}, {1000, 25}},
        {"01K20000000000000000000002", "B.MP4", {25, 1}, {1000, 25}},
    };
    document.tracks = {
        {"01K20000000000000000000003",
         "video",
         0,
         {
             {"01K20000000000000000000004",
              "01K20000000000000000000001",
              {100, 25},
              {10, 25},
              {0, 25}},
             {"01K20000000000000000000005",
              "01K20000000000000000000001",
              {200, 25},
              {10, 25},
              {20, 25}},
             {"01K20000000000000000000006",
              "01K20000000000000000000002",
              {300, 25},
              {10, 25},
              {40, 25}},
         }},
    };
    return document;
}

uint64_t CanonicalHash(const Document& document) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (unsigned char byte : document.SaveToString()) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

bool Apply(EditLog& log, Document& document, Operation operation,
           const std::string& label) {
    EditError error = EditError::None;
    std::string message;
    const bool result =
        log.Apply(document, std::move(operation), error, message);
    Check(result,
          label + " failed with " + EditErrorName(error) + ": " + message);
    return result;
}

void ExpectRejected(Document document, Operation operation, EditError expected,
                    const std::string& label) {
    const std::string before = document.SaveToString();
    EditLog log;
    EditError error = EditError::None;
    std::string message;
    Check(!log.Apply(document, std::move(operation), error, message),
          label + " must be rejected");
    Check(error == expected, label + " expected " + EditErrorName(expected) +
                                 ", got " + EditErrorName(error) + ": " +
                                 message);
    Check(document.SaveToString() == before,
          label + " must leave the document byte-identical");
    Check(log.AppliedCount() == 0 && log.UndoneCount() == 0,
          label + " must leave the log unchanged");
}

}  // namespace

int main() {
    Test("insert/remove ripple positions", [] {
        Document document = EditDocument();
        EditLog log;
        InsertClipOperation insert{document.tracks[0].id,
                                   document.sources[1].id,
                                   {50, 25},
                                   {5, 25},
                                   {10, 25},
                                   {},
                                   {}};
        Check(Apply(log, document, insert, "insert"), "insert applies");
        const auto& stored =
            std::get<InsertClipOperation>(log.AppliedEntries().back().op);
        Check(document.tracks[0].clips.size() == 4, "insert adds one clip");
        Check(document.FindClip(stored.clip_id) != nullptr,
              "inserted clip is addressed by generated ULID");
        Check(document.FindClip("01K20000000000000000000005")->timeline_in ==
                  RationalTime{25, 25},
              "second original clip ripples right by insertion duration");
        Check(document.FindClip("01K20000000000000000000006")->timeline_in ==
                  RationalTime{45, 25},
              "all following clips ripple right");

        Check(Apply(log, document, RemoveClipOperation{stored.clip_id, {}},
                    "remove"),
              "remove applies");
        Check(document.FindClip(stored.clip_id) == nullptr,
              "remove deletes clip");
        Check(document.FindClip("01K20000000000000000000005")->timeline_in ==
                  RationalTime{20, 25},
              "remove ripples following clip left");
        Check(document.FindClip("01K20000000000000000000006")->timeline_in ==
                  RationalTime{40, 25},
              "remove restores every following position");
    });

    Test("mixed-rate ripple undo restores exact representation", [] {
        Document document;
        document.sources = {
            {"01K40000000000000000000001", "25.MP4", {25, 1}, {250, 25}},
            {"01K40000000000000000000002",
             "2997.MP4",
             {30000, 1001},
             {300300, 30000}},
        };
        document.tracks = {
            {"01K40000000000000000000003",
             "video",
             0,
             {
                 {"01K40000000000000000000004",
                  "01K40000000000000000000001",
                  {0, 25},
                  {25, 25},
                  {0, 25}},
                 {"01K40000000000000000000005",
                  "01K40000000000000000000002",
                  {100100, 30000},
                  {1001, 30000},
                  {30000, 30000}},
             }},
        };
        const std::string original = document.SaveToString();
        EditLog log;
        Check(Apply(log, document,
                    InsertClipOperation{document.tracks[0].id,
                                        document.sources[1].id,
                                        {0, 30000},
                                        {1001, 30000},
                                        {25, 25},
                                        "01K40000000000000000000006",
                                        {}},
                    "mixed-rate insert"),
              "mixed-rate insertion applies");
        Check(
            document.FindClip("01K40000000000000000000005")->timeline_in.rate ==
                30000,
            "ripple uses an exact common timebase");
        EditError error = EditError::None;
        std::string message;
        Check(log.Undo(document, error, message),
              "mixed-rate insertion undo succeeds: " + message);
        Check(document.SaveToString() == original,
              "mixed-rate undo restores original RationalTime representation");
    });

    Test("head and tail trim stay local", [] {
        {
            Document isolated = EditDocument();
            isolated.tracks[0].clips.erase(isolated.tracks[0].clips.begin() + 1,
                                           isolated.tracks[0].clips.end());
            EditLog log;
            Check(Apply(log, isolated,
                        TrimClipOperation{isolated.tracks[0].clips[0].id,
                                          TrimEdge::Head,
                                          {2, 25},
                                          std::nullopt},
                        "isolated head trim"),
                  "isolated head trim applies");
            const DocumentClip& clip = isolated.tracks[0].clips[0];
            Check(clip.source_in == RationalTime{102, 25} &&
                      clip.duration == RationalTime{8, 25} &&
                      clip.timeline_in == RationalTime{2, 25},
                  "head trim changes exactly source_in, duration and "
                  "timeline_in");
            Check(Apply(log, isolated,
                        TrimClipOperation{
                            clip.id, TrimEdge::Tail, {-2, 25}, std::nullopt},
                        "isolated tail trim"),
                  "isolated tail trim applies");
            Check(isolated.tracks[0].clips[0].duration == RationalTime{6, 25},
                  "tail trim changes duration");
        }
        {
            Document framed = EditDocument();
            const RationalTime beforePrevious =
                framed.tracks[0].clips[0].timeline_in;
            const RationalTime beforeNext =
                framed.tracks[0].clips[2].timeline_in;
            EditLog log;
            const Ulid middle = framed.tracks[0].clips[1].id;
            Check(Apply(log, framed,
                        TrimClipOperation{
                            middle, TrimEdge::Head, {2, 25}, std::nullopt},
                        "framed head trim"),
                  "framed head trim applies");
            Check(framed.tracks[0].clips[0].timeline_in == beforePrevious &&
                      framed.tracks[0].clips[2].timeline_in == beforeNext,
                  "head trim does not move surrounding clips");
            Check(Apply(log, framed,
                        TrimClipOperation{
                            middle, TrimEdge::Tail, {2, 25}, std::nullopt},
                        "framed tail trim"),
                  "framed tail trim applies inside the gap");
            Check(framed.tracks[0].clips[2].timeline_in == beforeNext,
                  "tail trim does not move following clip");
        }
    });

    Test("invalid preconditions are atomic and named", [] {
        const Document base = EditDocument();
        const InsertClipOperation validInsert{base.tracks[0].id,
                                              base.sources[0].id,
                                              {0, 25},
                                              {2, 25},
                                              {10, 25},
                                              {},
                                              {}};
        InsertClipOperation unknownTrack = validInsert;
        unknownTrack.track_id = "01K29999999999999999999991";
        ExpectRejected(base, unknownTrack, EditError::UnknownTrack,
                       "unknown track_id");
        InsertClipOperation unknownSource = validInsert;
        unknownSource.source_id = "01K29999999999999999999992";
        ExpectRejected(base, unknownSource, EditError::UnknownSource,
                       "unknown source_id");
        InsertClipOperation zeroDuration = validInsert;
        zeroDuration.duration = {0, 25};
        ExpectRejected(base, zeroDuration, EditError::InvalidDuration,
                       "zero insert duration");
        InsertClipOperation negativeDuration = validInsert;
        negativeDuration.duration = {-1, 25};
        ExpectRejected(base, negativeDuration, EditError::InvalidDuration,
                       "negative insert duration");
        InsertClipOperation negativeTimeline = validInsert;
        negativeTimeline.timeline_in = {-1, 25};
        ExpectRejected(base, negativeTimeline, EditError::InvalidTimelineIn,
                       "negative insertion timeline_in");
        InsertClipOperation overlap = validInsert;
        overlap.timeline_in = {5, 25};
        ExpectRejected(base, overlap, EditError::Overlap, "insertion overlap");
        InsertClipOperation outside = validInsert;
        outside.source_in = {999, 25};
        ExpectRejected(base, outside, EditError::SourceOutOfBounds,
                       "insert source bounds");
        ExpectRejected(base,
                       RemoveClipOperation{"01K29999999999999999999993", {}},
                       EditError::UnknownClip, "unknown remove clip_id");
        ExpectRejected(base,
                       TrimClipOperation{"01K29999999999999999999994",
                                         TrimEdge::Tail,
                                         {1, 25},
                                         std::nullopt},
                       EditError::UnknownClip, "unknown trim clip_id");
        ExpectRejected(base,
                       TrimClipOperation{base.tracks[0].clips[0].id,
                                         TrimEdge::Tail,
                                         {-10, 25},
                                         std::nullopt},
                       EditError::InvalidDuration, "zero trim duration");
        ExpectRejected(base,
                       TrimClipOperation{base.tracks[0].clips[0].id,
                                         TrimEdge::Head,
                                         {-101, 25},
                                         std::nullopt},
                       EditError::InvalidTimelineIn,
                       "head trim before timeline zero");

        Document sourceBound = base;
        sourceBound.tracks[0].clips[0].timeline_in = {200, 25};
        sourceBound.tracks[0].clips[1].timeline_in = {220, 25};
        sourceBound.tracks[0].clips[2].timeline_in = {240, 25};
        ExpectRejected(sourceBound,
                       TrimClipOperation{sourceBound.tracks[0].clips[0].id,
                                         TrimEdge::Head,
                                         {-101, 25},
                                         std::nullopt},
                       EditError::SourceOutOfBounds,
                       "head trim before source start");

        Document tailOverlap = base;
        tailOverlap.tracks[0].clips[1].timeline_in = {11, 25};
        tailOverlap.tracks[0].clips[2].timeline_in = {40, 25};
        ExpectRejected(tailOverlap,
                       TrimClipOperation{tailOverlap.tracks[0].clips[0].id,
                                         TrimEdge::Tail,
                                         {2, 25},
                                         std::nullopt},
                       EditError::Overlap, "tail trim overlap");
    });

    Test("operation and edit-log serialization round trips", [] {
        Document document = EditDocument();
        const std::string originalDocument = document.SaveToString();
        EditLog log;
        Check(Apply(log, document,
                    InsertClipOperation{document.tracks[0].id,
                                        document.sources[1].id,
                                        {10, 25},
                                        {3, 25},
                                        {10, 25},
                                        {},
                                        {}},
                    "serialized insert"),
              "insert applies");
        Check(Apply(log, document,
                    TrimClipOperation{document.tracks[0].clips[0].id,
                                      TrimEdge::Tail,
                                      {-1, 25},
                                      std::nullopt},
                    "serialized trim"),
              "trim applies");
        const Operation& original = log.AppliedEntries()[0].op;
        const std::string json = SerializeOperation(original);
        Operation parsed = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(json, parsed, error, message),
              "operation parses: " + message);
        Check(SerializeOperation(parsed) == json,
              "operation JSON is canonical after round trip");

        const std::string logJson = log.Serialize();
        EditLog parsedLog;
        Check(EditLog::Deserialize(logJson, parsedLog, error, message),
              "edit log parses: " + message);
        Check(parsedLog.Serialize() == logJson,
              "edit log JSON is canonical after round trip");

        const std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            (GenerateUlid() + "-cutmachine-edit-log.json");
        Check(log.Save(path.string(), error, message),
              "edit log saves: " + message);
        EditLog loaded;
        Check(EditLog::Load(path.string(), loaded, error, message),
              "edit log loads: " + message);
        Check(loaded.Serialize() == logJson, "persisted edit log is identical");
        Check(loaded.Undo(document, error, message),
              "loaded log can undo trim: " + message);
        Check(loaded.Undo(document, error, message),
              "loaded log can undo insert: " + message);
        Check(document.SaveToString() == originalDocument,
              "persisted inverses restore the original document bytes");
        std::filesystem::remove(path);
    });

    Test("bins create and classify media through reversible operations", [] {
        Document document = EditDocument();
        LibraryMedia media;
        media.id = document.sources[0].id;
        media.path = "A.MP4";
        media.filename = "A.MP4";
        media.codec = "h264";
        media.width = 1920;
        media.height = 1080;
        media.rate = {25, 1};
        media.duration = {1000, 25};
        media.orientation = "landscape";
        document.library.push_back(media);
        const std::string initial = document.SaveToString();
        EditLog log;
        const Ulid binId = "01K80000000000000000000001";
        Check(Apply(log, document, AddBinOperation{binId, "Rushes \"A\""},
                    "add bin"),
              "bin creation succeeds");
        Check(Apply(log, document,
                    SetMediaBinOperation{document.library[0].id, binId},
                    "assign media bin"),
              "media assignment succeeds");
        Check(document.bins.size() == 1 && document.library[0].bin_id == binId,
              "bin and media assignment are stored in the document");
        const std::string serialized = log.Serialize();
        EditLog parsed;
        EditError error = EditError::None;
        std::string message;
        Check(EditLog::Deserialize(serialized, parsed, error, message) &&
                  parsed.Serialize() == serialized,
              "bin operations round-trip in the event log: " + message);
        Check(log.Undo(document, error, message) &&
                  document.library[0].bin_id.empty(),
              "undo restores media to the project root");
        Check(log.Undo(document, error, message) && document.bins.empty() &&
                  document.SaveToString() == initial,
              "second undo removes the empty bin byte-exactly");
        Check(log.Redo(document, error, message) &&
                  log.Redo(document, error, message) &&
                  document.library[0].bin_id == binId,
              "redo recreates the same bin ULID and assignment");
    });

    Test("nested bins preserve hierarchy and reject destructive removal", [] {
        Document document = EditDocument();
        LibraryMedia media;
        media.id = document.sources[0].id;
        media.path = "A.MP4";
        media.filename = "A.MP4";
        media.codec = "h264";
        media.width = 1920;
        media.height = 1080;
        media.rate = {25, 1};
        media.duration = {1000, 25};
        media.orientation = "landscape";
        document.library.push_back(media);
        EditLog log;
        const Ulid parent = "01K81000000000000000000001";
        const Ulid child = "01K81000000000000000000002";
        Check(Apply(log, document, AddBinOperation{parent, "Rushes", ""},
                    "add parent bin"),
              "top-level bin creation succeeds");
        Check(Apply(log, document, AddBinOperation{child, "Interview", parent},
                    "add child bin"),
              "child bin creation succeeds");
        Check(document.FindBin(child) &&
                  document.FindBin(child)->parent_id == parent,
              "child retains its exact parent ULID");

        const std::string childJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(childJson, parsed, error, message) &&
                  SerializeOperation(parsed) == childJson,
              "nested AddBin JSON round-trips canonically");

        Check(
            Apply(log, document, RenameBinOperation{child, "Interview selects"},
                  "rename child bin"),
            "bin rename succeeds through the event log");
        const std::string renameJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Check(DeserializeOperation(renameJson, parsed, error, message) &&
                  SerializeOperation(parsed) == renameJson,
              "RenameBin JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.FindBin(child)->name == "Interview",
              "rename undo restores the original name");

        ExpectRejected(document, RemoveBinOperation{parent, "", ""},
                       EditError::InvalidOperation, "remove bin with child");
        Check(Apply(log, document, SetMediaBinOperation{media.id, child},
                    "classify in child"),
              "media can be placed in a nested bin");
        ExpectRejected(document, RemoveBinOperation{child, "", ""},
                       EditError::InvalidOperation,
                       "remove non-empty child bin");

        Document loaded;
        Check(Document::LoadFromString(document.SaveToString(), loaded,
                                       message) &&
                  loaded.FindBin(child) &&
                  loaded.FindBin(child)->parent_id == parent,
              "document JSON preserves the bin hierarchy");

        Document cyclic = document;
        cyclic.FindBin(parent)->parent_id = child;
        Check(!cyclic.Validate(message), "bin hierarchy cycles are rejected");
    });

    Test("empty undo/redo and redo clearing", [] {
        Document document = EditDocument();
        const std::string original = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(!log.Undo(document, error, message) &&
                  error == EditError::EmptyUndo,
              "undo on empty log returns EmptyUndo");
        Check(!log.Redo(document, error, message) &&
                  error == EditError::EmptyRedo,
              "redo on empty log returns EmptyRedo");
        Check(document.SaveToString() == original,
              "empty undo/redo leave document unchanged");

        Check(Apply(log, document,
                    TrimClipOperation{document.tracks[0].clips[0].id,
                                      TrimEdge::Tail,
                                      {-1, 25},
                                      std::nullopt},
                    "trim before undo"),
              "trim applies");
        Check(log.Undo(document, error, message), "undo succeeds");
        Check(log.UndoneCount() == 1, "undo populates redo stack");
        Check(Apply(log, document,
                    TrimClipOperation{document.tracks[0].clips[1].id,
                                      TrimEdge::Tail,
                                      {-1, 25},
                                      std::nullopt},
                    "new trim"),
              "new trim applies");
        Check(log.UndoneCount() == 0, "new operation clears redo stack");
    });

    Test("20 operations full undo/redo canonical hash", [] {
        Document document = EditDocument();
        // Keep one base clip so eight end insertions form a simple, valid
        // chain.
        document.tracks[0].clips.erase(document.tracks[0].clips.begin() + 1,
                                       document.tracks[0].clips.end());
        std::string validation;
        Check(document.Validate(validation),
              "hash fixture validates: " + validation);
        const std::string originalJson = document.SaveToString();
        const uint64_t originalHash = CanonicalHash(document);
        EditLog log;
        std::vector<Ulid> inserted;
        const std::vector<Ulid> insertedIds = {
            "01K30000000000000000000001", "01K30000000000000000000002",
            "01K30000000000000000000003", "01K30000000000000000000004",
            "01K30000000000000000000005", "01K30000000000000000000006",
            "01K30000000000000000000007", "01K30000000000000000000008",
        };

        for (int index = 0; index < 8; ++index) {
            const RationalTime end = Timeline(document).Duration();
            Check(Apply(log, document,
                        InsertClipOperation{document.tracks[0].id,
                                            document.sources[index % 2].id,
                                            {400 + index * 10, 25},
                                            {5, 25},
                                            end,
                                            insertedIds[index],
                                            {}},
                        "hash insert " + std::to_string(index)),
                  "hash insertion succeeds");
            inserted.push_back(
                std::get<InsertClipOperation>(log.AppliedEntries().back().op)
                    .clip_id);
        }
        for (int index = 4; index < 8; ++index) {
            Check(Apply(log, document,
                        TrimClipOperation{inserted[index],
                                          TrimEdge::Tail,
                                          {-1, 25},
                                          std::nullopt},
                        "hash tail trim " + std::to_string(index)),
                  "hash tail trim succeeds");
        }
        for (int index = 0; index < 4; ++index) {
            Check(
                Apply(
                    log, document,
                    TrimClipOperation{
                        inserted[index], TrimEdge::Head, {1, 25}, std::nullopt},
                    "hash head trim " + std::to_string(index)),
                "hash head trim succeeds");
        }
        Check(Apply(log, document, RemoveClipOperation{inserted[0], {}},
                    "hash remove 0"),
              "first hash remove succeeds");
        Check(Apply(log, document, RemoveClipOperation{inserted[2], {}},
                    "hash remove 2"),
              "second hash remove succeeds");
        Check(Apply(log, document,
                    TrimClipOperation{document.tracks[0].clips[0].id,
                                      TrimEdge::Tail,
                                      {-1, 25},
                                      std::nullopt},
                    "hash base tail trim"),
              "base tail trim succeeds");
        Check(Apply(log, document,
                    TrimClipOperation{
                        inserted[7], TrimEdge::Head, {1, 25}, std::nullopt},
                    "hash final head trim"),
              "final head trim succeeds");
        Check(log.AppliedCount() == 20, "exactly 20 operations were applied");
        const uint64_t editedHash = CanonicalHash(document);
        Check(editedHash != originalHash,
              "20 operations materially change document");

        EditError error = EditError::None;
        std::string message;
        for (int index = 0; index < 20; ++index) {
            Check(log.Undo(document, error, message),
                  "undo " + std::to_string(index) + ": " + message);
        }
        const uint64_t firstUndoHash = CanonicalHash(document);
        Check(firstUndoHash == originalHash &&
                  document.SaveToString() == originalJson,
              "full undo restores canonical bytes exactly");

        for (int index = 0; index < 20; ++index) {
            Check(log.Redo(document, error, message),
                  "redo " + std::to_string(index) + ": " + message);
        }
        Check(CanonicalHash(document) == editedHash,
              "full redo restores edited canonical hash");
        for (int index = 0; index < 20; ++index) {
            Check(log.Undo(document, error, message),
                  "second undo " + std::to_string(index) + ": " + message);
        }
        const uint64_t secondUndoHash = CanonicalHash(document);
        Check(secondUndoHash == originalHash &&
                  document.SaveToString() == originalJson,
              "redo then full undo restores canonical bytes exactly again");
        std::cout << "HASH original=" << originalHash
                  << " edited=" << editedHash << " undo1=" << firstUndoHash
                  << " undo2=" << secondUndoHash << '\n';
    });

    if (failures) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All edit tests passed\n";
    return 0;
}
```

### tests/ingest_tests.cc

```cpp
#include "Cli.h"
#include "Document.h"
#include "Ingest.h"
#include "Operations.h"
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

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

}  // namespace

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / (GenerateUlid() + "-ingest");
    const std::filesystem::path mediaDirectory = root / "media";
    const std::filesystem::path documentPath = root / "document.json";
    const std::filesystem::path rawPath = root / "raw.mp4";
    const std::filesystem::path videoPath = mediaDirectory / "rotated.mp4";
    std::filesystem::create_directories(mediaDirectory);

    Document empty;
    std::string error;
    Check(empty.Save(documentPath.string(), error),
          "empty document saves: " + error);

    const std::string generate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error "
        "-f lavfi -i 'color=c=black:s=64x32:r=30000/1001:d=1.001' "
        "-f lavfi -i 'sine=frequency=1000:sample_rate=48000:duration=1.001' "
        "-c:v mpeg4 -c:a aac -shortest " +
        Quote(rawPath);
    Check(std::system(generate.c_str()) == 0,
          "FFmpeg must generate the media fixture");
    const std::string rotate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -display_rotation:v:0 90 -i " +
        Quote(rawPath) + " -c copy " + Quote(videoPath);
    Check(std::system(rotate.c_str()) == 0,
          "FFmpeg must attach a display matrix");

    {
        std::ofstream text(mediaDirectory / "notes.txt");
        text << "not media\n";
    }
    {
        std::ofstream corrupt(mediaDirectory / "corrupt.mp4", std::ios::binary);
        corrupt << "broken mp4";
    }

    std::string output;
    Check(IngestCommand(documentPath.string(), mediaDirectory.string(), false,
                        output) == 0,
          "mixed folder ingest succeeds: " + output);
    Check(output.find("\"added\":1") != std::string::npos &&
              output.find("\"skipped\":2") != std::string::npos,
          "one media is added and two unreadable files are skipped");
    Check(output.find("notes.txt") != std::string::npos &&
              output.find("corrupt.mp4") != std::string::npos,
          "both unreadable files have reported reasons");

    Document ingested;
    Check(Document::Load(documentPath.string(), ingested, error),
          "ingested document loads: " + error);
    Check(ingested.library.size() == 1, "library contains one media");
    if (ingested.library.size() == 1) {
        const LibraryMedia& media = ingested.library[0];
        Check(media.rate.num == 30000 && media.rate.den == 1001,
              "avg_frame_rate remains the exact 30000/1001 rational");
        Check(media.width == 64 && media.height == 32,
              "stored dimensions remain the coded dimensions");
        Check(!media.pixel_format.empty() && !media.color_range.empty() &&
                  !media.color_space.empty() &&
                  !media.color_transfer.empty() &&
                  !media.color_primaries.empty(),
              "pixel format and color signalling flow through ingest");
        Check(media.orientation == "portrait",
              "display-matrix rotation controls orientation");
        Check(std::abs(media.rotation_degrees) == 90,
              "display-matrix rotation is retained for presentation");
        Check(media.has_audio && media.audio_rate == 48000 &&
                  media.audio_channels == 1,
              "audio header metadata is extracted");
        const DocumentSource* source = ingested.FindSource(media.id);
        Check(source && source->rate.num == media.rate.num &&
                  source->rate.den == media.rate.den &&
                  source->duration == media.duration,
              "ingest creates a source with the same stable media ULID");
        ingested.tracks.push_back(
            {"01K82000000000000000000001", "video", 0, {}});
        Operation insert = InsertClipOperation{ingested.tracks[0].id,
                                               media.id,
                                               {0, media.duration.rate},
                                               media.duration,
                                               {0, media.duration.rate},
                                               {},
                                               {}};
        Operation inverse = RemoveClipOperation{};
        EditError editError = EditError::None;
        std::string editMessage;
        Check(ApplyOperation(ingested, insert, inverse, editError, editMessage),
              "an ingested media can be inserted directly: " + editMessage);
    }

    const std::string beforeSecondIngest = Read(documentPath);
    Check(IngestCommand(documentPath.string(), mediaDirectory.string(), false,
                        output) == 0,
          "second ingest succeeds: " + output);
    Check(output.find("\"added\":0") != std::string::npos &&
              Read(documentPath) == beforeSecondIngest,
          "second ingest is byte-idempotent");

    std::string description;
    Check(DescribeCommand(documentPath.string(), description) == 0,
          "describe succeeds after ingest");
    Check(description.find("{\"timeline\":") == 0 &&
              description.find("\"library\":[") != std::string::npos &&
              description.find("\"alias\":\"M1\"") != std::string::npos &&
              description.find("\"in_use\":false") != std::string::npos,
          "describe separates timeline and library with media aliases");

    const std::string beforeFailedScan = Read(documentPath);
    Check(IngestCommand(documentPath.string(), (root / "missing").string(),
                        false, output) == 1 &&
              Read(documentPath) == beforeFailedScan,
          "global scan failure leaves the document byte-identical");

    std::filesystem::remove_all(root);
    if (failures) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All ingest tests passed\n";
    return 0;
}
```

### tests/model_tests.cc

```cpp
#include "Document.h"
#include "ColorManagement.h"
#include "Timeline.h"
#include "Ulid.h"

#include <cstdio>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

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
    try {
        function();
        if (failures == 0) {
            std::cout << "PASS: " << name << '\n';
        }
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: " << name << ": " << exception.what() << '\n';
    }
}

Document ValidDocument() {
    Document document;
    document.sources = {
        {"01K00000000000000000000001", "A.MP4", {25, 1}, {200, 25}},
        {"01K00000000000000000000002", "B.MP4", {25, 1}, {200, 25}},
    };
    document.tracks = {
        {"01K00000000000000000000003",
         "video",
         0,
         {
             {"01K00000000000000000000004",
              "01K00000000000000000000001",
              {100, 25},
              {2, 25},
              {0, 25}},
             {"01K00000000000000000000005",
              "01K00000000000000000000002",
              {10, 25},
              {2, 25},
              {2, 25}},
         }},
        {"01K00000000000000000000006",
         "video",
         1,
         {
             {"01K00000000000000000000007",
              "01K00000000000000000000001",
              {0, 25},
              {1, 25},
              {1, 25}},
         }},
    };
    return document;
}

void ExpectInvalid(Document document, const std::string& expected,
                   const std::string& label) {
    std::string error;
    Document loaded;
    Check(!Document::LoadFromString(document.SaveToString(), loaded, error),
          label + " must be rejected while loading");
    Check(
        error.find(expected) != std::string::npos,
        label + " error must contain '" + expected + "', got '" + error + "'");
}

}  // namespace

int main() {
    Test("reference color transfer functions", [] {
        const auto Near = [](double actual, double expected, double epsilon,
                             const std::string& message) {
            Check(std::abs(actual - expected) <= epsilon,
                  message + ": got " + std::to_string(actual));
        };
        Near(DecodeSonySLog3(95.0 / 1023.0), 0.0, 1e-12,
             "S-Log3 code 95 is scene black");
        Near(DecodeSonySLog3(420.0 / 1023.0), 0.18, 1e-12,
             "S-Log3 code 420 is 18% grey");
        Near(DecodeSonySLog3(598.0 / 1023.0), 0.9, 0.005,
             "S-Log3 code 598 is 90% white");
        for (double value : {-0.01, 0.0, 0.0078125, 0.18, 4.0})
            Near(DecodeAcesCct(EncodeAcesCct(value)), value, 1e-12,
                 "ACEScct round trip");
        Near(EncodeHlg(DecodeHlg(0.75)), 0.75, 1e-9,
             "HLG reference white signal round trip");
        Near(EncodeHlg(0.9 * HlgSceneReflectionScale()), 0.75, 1e-9,
             "90% scene white maps to HLG reference white");
        const YuvCodeParameters full = BuildYuvCodeParameters(10, true);
        const YuvCodeParameters legal = BuildYuvCodeParameters(10, false);
        Near(full.y_offset, 0.0, 1e-7, "full range starts at code zero");
        Near(legal.y_offset, 64.0 / 1023.0, 1e-7,
             "10-bit legal range starts at code 64");
        Near(legal.y_scale, 1023.0 / 876.0, 1e-7,
             "10-bit legal luma spans 876 codes");
        const YuvMatrixParameters bt709 = BuildYuvMatrixParameters(false);
        const YuvMatrixParameters bt2020 = BuildYuvMatrixParameters(true);
        Near(bt709.red_from_cr, 1.5748, 1e-6,
             "BT.709 uses normalized chroma coefficients once");
        Near(bt2020.red_from_cr, 1.4746, 1e-6,
             "BT.2020 NCL uses its own normalized matrix");
    });

    Test("RationalTime arithmetic across rates", [] {
        const RationalTime a{1, 24};
        const RationalTime b{1, 48};
        const RationalTime sum = a.add(b);
        Check(sum.value == 3 && sum.rate == 48, "1/24 + 1/48 must be 3/48");
        Check(a.compare(RationalTime{2, 48}) == 0,
              "equivalent rates compare equal");
        Check(sum.sub(a) == b, "sub must preserve the exact rational result");
        Check(a.rescale(48).value == 2, "exact rescale must produce 2/48");
        Check(RationalTime{1001, 30000}.to_frames(30000, 1001) == 1,
              "30000/1001 frame mapping must remain integral");
        const Ulid generated = GenerateUlid();
        Check(IsValidUlid(generated), "generated IDs must be valid ULIDs");
        Check(generated != GenerateUlid(),
              "successive generated ULIDs must differ");
    });

    Test("load/save/load canonical round trip", [] {
        const Document original = ValidDocument();
        std::string error;
        Check(original.Validate(error), "fixture must validate: " + error);
        const std::filesystem::path first =
            std::filesystem::temp_directory_path() / (GenerateUlid() + ".json");
        const std::filesystem::path second =
            std::filesystem::temp_directory_path() / (GenerateUlid() + ".json");
        Check(original.Save(first.string(), error), "first save: " + error);
        Document loaded;
        Check(Document::Load(first.string(), loaded, error), "load: " + error);
        Check(loaded.Save(second.string(), error), "second save: " + error);
        Document loadedAgain;
        Check(Document::Load(second.string(), loadedAgain, error),
              "second load: " + error);
        Check(loaded.SaveToString() == loadedAgain.SaveToString(),
              "canonical JSON must be byte-identical after second load");
        std::filesystem::remove(first);
        std::filesystem::remove(second);
    });

    Test("version 1 documents migrate to the version 2 library", [] {
        const std::string json =
            "{\"version\":1,\"sources\":[{\"id\":"
            "\"01K90000000000000000000001\",\"path\":\"legacy.mov\","
            "\"rate\":{\"num\":25,\"den\":1},\"duration\":"
            "{\"value\":100,\"rate\":25}}],\"tracks\":[]}";
        Document document;
        std::string error;
        Check(Document::LoadFromString(json, document, error),
              "version 1 must load: " + error);
        Check(document.version == 2, "version 1 must migrate in memory");
        Check(document.library.size() == 1,
              "the legacy source must remain visible in the library");
        Check(document.library[0].id == document.sources[0].id &&
                  !document.library[0].metadata_complete,
              "migration must preserve identity without inventing metadata");
    });

    Test("color management settings persist and validate", [] {
        Document document = ValidDocument();
        document.color_management.enabled = true;
        document.color_management.input_gamut = "sony_sgamut3_cine";
        document.color_management.input_transfer = "sony_slog3";
        document.color_management.input_ycbcr_matrix = "bt709";
        document.color_management.input_range = "full";
        document.color_management.working_gamut = "acescct";
        document.color_management.output_gamut = "rec2020";
        document.color_management.output_transfer = "hlg";
        std::string error;
        Check(document.Validate(error), "Sony/HLG pipeline must validate: " + error);
        Document loaded;
        Check(Document::LoadFromString(document.SaveToString(), loaded, error),
              "Sony/HLG pipeline must load: " + error);
        Check(loaded.color_management.enabled &&
                  loaded.color_management.input_gamut == "sony_sgamut3_cine" &&
                  loaded.color_management.input_transfer == "sony_slog3" &&
                  loaded.color_management.input_range == "full" &&
                  loaded.color_management.working_gamut == "acescct" &&
                  loaded.color_management.output_gamut == "rec2020" &&
                  loaded.color_management.output_transfer == "hlg",
              "all color pipeline stages must round-trip");

        loaded.color_management.output_gamut = "rec709";
        Check(!loaded.Validate(error) && error.find("HLG") != std::string::npos,
              "HLG with a non-Rec.2020 gamut must be rejected");
        loaded.color_management.output_gamut = "rec2020";
        loaded.color_management.input_transfer = "unknown_log";
        Check(!loaded.Validate(error) &&
                  error.find("color_management") != std::string::npos,
              "unknown transfer functions must be rejected");
    });

    Test("timeline resolution", [] {
        const Document document = ValidDocument();
        Timeline timeline(document);
        const Ulid& primaryTrack = document.tracks[0].id;
        const Ulid& sparseTrack = document.tracks[1].id;

        const auto first = timeline.ResolveTrack(primaryTrack, {0, 25});
        Check(first && first->source_id == document.sources[0].id &&
                  first->source_frame == 100,
              "first timeline frame must map to first clip source frame 100");

        const auto cut = timeline.ResolveTrack(primaryTrack, {2, 25});
        Check(cut && cut->source_id == document.sources[1].id &&
                  cut->source_frame == 10,
              "half-open cut boundary must map to second clip");

        Check(!timeline.ResolveTrack(sparseTrack, {0, 25}),
              "position before sparse clip must resolve to a hole");

        const auto last = timeline.ResolveTrack(primaryTrack, {3, 25});
        Check(last && last->source_id == document.sources[1].id &&
                  last->source_frame == 11,
              "last timeline frame must map to second clip source frame 11");
        Check(!timeline.ResolveTrack(primaryTrack, {4, 25}),
              "timeline end is exclusive");

        const auto allTracks = timeline.Resolve({0, 25});
        Check(
            allTracks.size() == 2 && allTracks[0].frame && !allTracks[1].frame,
            "Resolve must return one populated-or-hole result per track");
    });

    Test("mixed-rate cut resolution at the common timebase", [] {
        Document document;
        document.sources = {
            {"01K10000000000000000000001", "25.MP4", {25, 1}, {250, 25}},
            {"01K10000000000000000000002",
             "2997.MP4",
             {30000, 1001},
             {300300, 30000}},
        };
        document.tracks = {
            {"01K10000000000000000000003",
             "video",
             0,
             {
                 {"01K10000000000000000000004",
                  "01K10000000000000000000001",
                  {0, 25},
                  {25, 25},
                  {0, 25}},
                 {"01K10000000000000000000005",
                  "01K10000000000000000000002",
                  {100100, 30000},
                  {1001, 30000},
                  {30000, 30000}},
             }},
        };
        std::string error;
        Check(document.Validate(error),
              "mixed-rate fixture must validate: " + error);
        Timeline timeline(document);
        Check(timeline.Duration().rate == 30000,
              "timeline timebase must be the exact LCM 30000");

        const auto before =
            timeline.ResolveTrack(document.tracks[0].id, {29999, 30000});
        Check(before && before->source_id == document.sources[0].id &&
                  before->source_frame == 24,
              "tick before cut must remain on the last 25 fps frame");
        const auto atCut =
            timeline.ResolveTrack(document.tracks[0].id, {30000, 30000});
        Check(atCut && atCut->source_id == document.sources[1].id &&
                  atCut->source_frame == 100,
              "cut tick must resolve to the first 30000/1001 clip frame");
    });

    Test("document validation", [] {
        {
            Document value = ValidDocument();
            value.tracks[0].id = value.sources[0].id;
            ExpectInvalid(value, "duplicate ID", "duplicate IDs");
        }
        {
            Document value = ValidDocument();
            value.tracks[0].clips[0].source_id = "01K00000000000000000000009";
            ExpectInvalid(value, "unknown source_id", "unknown source_id");
        }
        {
            Document value = ValidDocument();
            value.tracks[0].clips[1].timeline_in = {1, 25};
            ExpectInvalid(value, "overlap", "overlapping clips");
        }
        {
            Document value = ValidDocument();
            std::swap(value.tracks[0].clips[0], value.tracks[0].clips[1]);
            ExpectInvalid(value, "not sorted", "unsorted clips");
        }
        {
            Document value = ValidDocument();
            value.tracks[0].clips[0].duration.value = 0;
            ExpectInvalid(value, "zero or negative duration", "zero duration");
        }
        {
            Document value = ValidDocument();
            value.tracks[0].clips[0].duration.value = -1;
            ExpectInvalid(value, "zero or negative duration",
                          "negative duration");
        }
        {
            Document value = ValidDocument();
            value.tracks[0].clips[0].source_in = {199, 25};
            ExpectInvalid(value, "outside source bounds", "source bounds");
        }
        {
            Document value = ValidDocument();
            value.sources[0].rate.num = 0;
            ExpectInvalid(value, "media rate", "zero source media rate");
        }
        {
            Document value = ValidDocument();
            value.tracks[0].clips[0].timeline_in.rate = 0;
            ExpectInvalid(value, "time rate", "zero RationalTime rate");
        }
    });

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All model tests passed\n";
    return 0;
}
```

### tests/timeline_view_tests.cc

```cpp
#include "TimelineView.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <string>
#include <variant>

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
        if (before == failures) std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: " << name << ": " << exception.what() << '\n';
    }
}

Document Fixture() {
    Document document;
    document.sources = {
        {"01KT0000000000000000000001", "source.mov", {10, 1}, {100, 10}},
    };
    document.tracks = {
        {"01KT0000000000000000000002",
         "video",
         0,
         {{"01KT0000000000000000000003",
           "01KT0000000000000000000001",
           {10, 10},
           {20, 10},
           {10, 10}},
          {"01KT0000000000000000000004",
           "01KT0000000000000000000001",
           {40, 10},
           {10, 10},
           {40, 10}}}},
    };
    return document;
}

TimelineViewport Viewport() {
    TimelineViewport viewport;
    viewport.view_start = {0, 10};
    viewport.pixels_per_second = 100.0;
    viewport.track_height = 40.0;
    viewport.header_width = 50.0;
    return viewport;
}

uint64_t Hash(const std::string& bytes) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

}  // namespace

int main() {
    Test("time and pixels invert at frame boundaries", [] {
        const struct Case {
            int32_t rate;
            int64_t frameStep;
        } cases[] = {{24, 1}, {25, 1}, {30000, 1001}};
        const double zooms[] = {4.0, 37.5, 100.0, 1333.0, 4000.0};
        for (const Case value : cases) {
            for (double zoom : zooms) {
                TimelineViewport viewport;
                viewport.view_start = {value.frameStep * 7, value.rate};
                viewport.pixels_per_second = zoom;
                viewport.header_width = 71.0;
                for (int64_t frame = -5; frame <= 50; ++frame) {
                    const RationalTime time{frame * value.frameStep,
                                            value.rate};
                    const RationalTime roundTrip =
                        viewport.XToTime(viewport.TimeToX(time), value.rate);
                    Check(roundTrip == time,
                          "round trip must preserve frame at every zoom");
                }
            }
        }
    });

    Test("playhead quantizes exactly to frames or audio samples", [] {
        const MediaRate ntsc{30000, 1001};
        Check(QuantizePlayheadPosition({1500, 30000}, PlayheadResolution::Frame,
                                       ntsc) == RationalTime{1001, 30000},
              "NTSC position below half-frame rounds to the first frame");
        Check(QuantizePlayheadPosition({1502, 30000}, PlayheadResolution::Frame,
                                       ntsc) == RationalTime{2002, 30000},
              "NTSC position above half-frame rounds to the second frame");
        Check(QuantizePlayheadPosition({1, 96000}, PlayheadResolution::Sample,
                                       ntsc) == RationalTime{1, 48000},
              "half an audio sample rounds explicitly away from zero");
        Check(QuantizePlayheadPosition({1, 192000}, PlayheadResolution::Sample,
                                       ntsc) == RationalTime{0, 48000},
              "sub-half-sample position rounds to sample zero");
    });

    Test("hit test resolves clips, holes and six-point edges", [] {
        const Document document = Fixture();
        const TimelineViewport viewport = Viewport();
        const double trackY = kTimelineRulerHeight + 20.0;
        const auto inside =
            HitTestTimeline(document, viewport, 200.0, trackY, 700.0);
        Check(inside && inside->clip_id == document.tracks[0].clips[0].id &&
                  inside->edge == TimelineHitEdge::None,
              "clip body returns its ULID");
        Check(!HitTestTimeline(document, viewport, 375.0, trackY, 700.0),
              "gap returns no clip");
        const auto head =
            HitTestTimeline(document, viewport, 154.0, trackY, 700.0);
        Check(head && head->edge == TimelineHitEdge::Head,
              "point within six points of head returns head edge");
        const auto tail =
            HitTestTimeline(document, viewport, 346.0, trackY, 700.0);
        Check(tail && tail->edge == TimelineHitEdge::Tail,
              "point within six points of tail returns tail edge");
    });

    Test("bounded gaps are selectable but trailing space is not", [] {
        Document document = Fixture();
        TimelineViewport viewport = Viewport();
        const double trackY = kTimelineRulerHeight + 20.0;
        const auto gap =
            HitTestTimelineGap(document, viewport, 375.0, trackY, 700.0, 10);
        Check(gap && gap->track_id == document.tracks[0].id &&
                  gap->start == RationalTime{30, 10} &&
                  gap->duration == RationalTime{10, 10},
              "clicking the bounded hole resolves its exact range");
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        interaction.PointerDown(375.0, trackY, 700.0, 10);
        Check(interaction.SelectedGap() && interaction.SelectedClipId().empty(),
              "pointer interaction selects the gap instead of a clip");
        interaction.PointerDown(600.0, trackY, 700.0, 10);
        Check(!interaction.SelectedGap(),
              "unbounded space after the last clip is not a deletable gap");
    });

    Test("lasso selects every intersecting clip rectangle", [] {
        Document document = Fixture();
        TimelineViewport viewport = Viewport();
        const double top = kTimelineRulerHeight + 5.0;
        const double bottom =
            kTimelineRulerHeight + viewport.track_height - 5.0;
        const auto selected = LassoHitTestTimeline(document, viewport, 140.0,
                                                   top, 460.0, bottom, 700.0);
        Check(selected.size() == 2 &&
                  selected[0] == document.tracks[0].clips[0].id &&
                  selected[1] == document.tracks[0].clips[1].id,
              "lasso intersection returns both clip ULIDs in display order");
        Check(LassoHitTestTimeline(document, viewport, 360.0, top, 440.0,
                                   bottom, 700.0)
                  .empty(),
              "lasso confined to a gap selects no clip");
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        interaction.SelectClips(selected);
        Check(interaction.SelectedClipIds().size() == 2 &&
                  interaction.SelectedClipId().empty(),
              "multi-selection has no accidental single edit target");
        const auto rectangles =
            VisibleTimelineClips(document, viewport, 700.0,
                                 interaction.SelectedClipIds(), std::nullopt);
        Check(std::count_if(rectangles.begin(), rectangles.end(),
                            [](const TimelineClipRect& clip) {
                                return clip.selected;
                            }) == 2,
              "every lasso member receives selected rendering");
    });

    Test("deleting a gap closes one track atomically and undo restores bytes",
         [] {
             Document document = Fixture();
             const std::string initial = document.SaveToString();
             EditLog log;
             EditError error = EditError::None;
             std::string message;
             Operation operation = DeleteGapOperation{
                 document.tracks[0].id, {30, 10}, {10, 10}, {}};
             Check(log.Apply(document, std::move(operation), error, message),
                   "gap deletion applies: " + message);
             Check(log.AppliedCount() == 1 &&
                       document.tracks[0].clips[0].timeline_in ==
                           RationalTime{10, 10} &&
                       document.tracks[0].clips[1].timeline_in ==
                           RationalTime{30, 10},
                   "all following clips shift left by the exact gap duration");
             const std::string json =
                 SerializeOperation(log.AppliedEntries().back().op);
             Operation parsed = RemoveClipOperation{};
             Check(DeserializeOperation(json, parsed, error, message) &&
                       SerializeOperation(parsed) == json,
                   "DeleteGap JSON round-trips canonically");
             Check(log.Undo(document, error, message),
                   "gap deletion undo succeeds: " + message);
             Check(document.SaveToString() == initial,
                   "undo restores the canonical document byte for byte");
             Check(log.Redo(document, error, message) &&
                       document.tracks[0].clips[1].timeline_in ==
                           RationalTime{30, 10},
                   "redo closes the same gap exactly");
         });

    Test("a gap deletion crossing a clip is rejected without mutation", [] {
        Document document = Fixture();
        const std::string initial = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Operation operation =
            DeleteGapOperation{document.tracks[0].id, {20, 10}, {10, 10}, {}};
        Check(!log.Apply(document, std::move(operation), error, message) &&
                  error == EditError::InvalidOperation,
              "range intersecting a clip is rejected");
        Check(log.AppliedCount() == 0 && document.SaveToString() == initial,
              "rejected deletion leaves document bytes and event log intact");
    });

    Test("adding a video track is atomic, stable and exactly reversible", [] {
        Document document = Fixture();
        const std::string initial = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Operation operation = AddTrackOperation{"", "video", 1};
        Check(log.Apply(document, std::move(operation), error, message),
              "track creation applies: " + message);
        Check(document.tracks.size() == 2 &&
                  document.tracks[1].kind == "video" &&
                  document.tracks[1].index == 1 &&
                  IsValidUlid(document.tracks[1].id),
              "new video layer has a stable generated identity");
        const Ulid addedId = document.tracks[1].id;
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "AddTrack JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == initial,
              "track creation undo restores exact document bytes");
        Check(log.Redo(document, error, message) &&
                  document.tracks.back().id == addedId,
              "redo retains the generated track ULID");
    });

    Test("detached audio becomes an independent exact timeline clip", [] {
        Document document = Fixture();
        document.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        const std::string initial = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.tracks[0].clips[0].id;
        Operation detach =
            DetachAudioOperation{videoId, document.tracks[1].id, "", {}};
        Check(log.Apply(document, std::move(detach), error, message),
              "audio detach applies: " + message);
        Check(!document.tracks[0].clips[0].include_audio &&
                  document.tracks[1].clips.size() == 1,
              "video contribution is muted and audio rectangle is created");
        const DocumentClip audio = document.tracks[1].clips[0];
        Check(IsValidUlid(audio.id) && audio.id != videoId &&
                  audio.source_id == document.tracks[0].clips[0].source_id &&
                  audio.source_in == document.tracks[0].clips[0].source_in &&
                  audio.duration == document.tracks[0].clips[0].duration &&
                  audio.timeline_in == document.tracks[0].clips[0].timeline_in,
              "detached clip retains exact source and timeline timing");
        Check(audio.link_group_id == audio.id &&
                  document.tracks[0].clips[0].link_group_id == audio.id,
              "detached image and audio share a stable link group");
        const std::vector<Ulid> linked =
            ExpandLinkedClipSelection(document, {videoId});
        Check(linked.size() == 2 && std::find(linked.begin(), linked.end(),
                                              audio.id) != linked.end(),
              "linked selection expands from video to its audio clip");
        const auto rectangles = VisibleTimelineClips(
            document, Viewport(), 700.0, linked, std::nullopt);
        Check(std::count_if(rectangles.begin(), rectangles.end(),
                            [](const TimelineClipRect& rectangle) {
                                return rectangle.audio;
                            }) == 1,
              "audio rectangles carry their distinct semantic render role");
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "DetachAudio JSON round-trips canonically");

        Operation sampleOffset =
            MoveClipOperation{audio.id,
                              document.tracks[1].id,
                              audio.timeline_in.add({1, 48000}),
                              {}};
        Check(log.Apply(document, std::move(sampleOffset), error, message) &&
                  ClipSyncDrift(document, audio.id) == RationalTime{1, 48000},
              "sub-frame drift remains exact at audio-sample resolution");
        Check(log.Undo(document, error, message) &&
                  ClipSyncDrift(document, audio.id) == RationalTime{0, 1},
              "sample offset undo restores zero drift");
        Operation headTrim =
            TrimClipOperation{audio.id, TrimEdge::Head, {1, 10}, std::nullopt};
        Check(log.Apply(document, std::move(headTrim), error, message) &&
                  ClipSyncDrift(document, audio.id) == RationalTime{0, 1},
              "head trim changes source and timeline equally without false "
              "drift");
        Check(log.Undo(document, error, message),
              "head trim sync test undo succeeds");

        Operation move =
            MoveClipOperation{audio.id, document.tracks[1].id, {50, 10}, {}};
        Check(log.Apply(document, std::move(move), error, message),
              "detached audio moves independently: " + message);
        Check(
            document.tracks[1].clips[0].timeline_in == RationalTime{50, 10} &&
                document.tracks[0].clips[0].timeline_in == RationalTime{10, 10},
            "moving audio leaves video timing unchanged");
        Check(log.Undo(document, error, message), "audio move undo succeeds");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == initial,
              "detach undo restores exact document bytes");
        Check(log.Redo(document, error, message) &&
                  document.tracks[1].clips[0].id == audio.id,
              "detach redo retains the generated audio ULID");
        Operation unlink = SetClipLinkOperation{videoId, audio.id, "", ""};
        Check(
            log.Apply(document, std::move(unlink), error, message) &&
                document.FindClip(videoId)->link_group_id.empty() &&
                document.FindClip(audio.id)->link_group_id.empty(),
            "linked selection can be atomically removed without editing times");
        const std::string unlinkJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Check(DeserializeOperation(unlinkJson, parsed, error, message) &&
                  SerializeOperation(parsed) == unlinkJson,
              "SetClipLink JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.FindClip(videoId)->link_group_id == audio.id &&
                  document.FindClip(audio.id)->link_group_id == audio.id,
              "link toggle undo restores the exact shared group");
    });

    Test("linked selection moves video and audio atomically", [] {
        Document document = Fixture();
        document.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.tracks[0].clips[0].id;
        Operation detach = DetachAudioOperation{
            videoId, document.tracks[1].id, "01KT0000000000000000000006", {}};
        Check(log.Apply(document, std::move(detach), error, message),
              "linked fixture detach applies: " + message);
        const Ulid audioId = document.tracks[1].clips[0].id;
        const std::string beforeMove = document.SaveToString();
        TimelineViewport viewport = Viewport();
        TimelineInteraction interaction(document, log, viewport);
        const double y = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(200.0, y, 700.0, 10);
        interaction.PointerDrag(250.0, y, 700.0);
        Check(interaction.SelectedClipIds().size() == 2 &&
                  interaction.MovePreview() &&
                  interaction.MovePreview()->linked_moves.size() == 2 &&
                  interaction.MovePreview()->valid,
              "linked drag previews both members as one valid candidate");
        const auto pending = interaction.PendingOperation();
        Check(pending &&
                  std::holds_alternative<MoveLinkedClipsOperation>(*pending),
              "linked drag emits the dedicated atomic operation");
        Check(interaction.PointerUp(error, message),
              "linked pointer-up applies: " + message);
        Check(document.FindClip(videoId)->timeline_in == RationalTime{15, 10} &&
                  document.FindClip(audioId)->timeline_in ==
                      RationalTime{15, 10} &&
                  ClipSyncDrift(document, audioId) == RationalTime{0, 1},
              "video and audio receive the same exact delta without drift");
        const std::string operationJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(operationJson, parsed, error, message) &&
                  SerializeOperation(parsed) == operationJson,
              "MoveLinkedClips JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == beforeMove,
              "one undo restores the pre-move document byte-exactly");
    });

    Test("invalid linked move emits nothing and sync drift is exact", [] {
        Document document = Fixture();
        document.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.tracks[0].clips[0].id;
        Operation detach = DetachAudioOperation{
            videoId, document.tracks[1].id, "01KT0000000000000000000006", {}};
        Check(log.Apply(document, std::move(detach), error, message),
              "invalid fixture detach applies");
        const Ulid audioId = document.tracks[1].clips[0].id;
        Operation offset =
            MoveClipOperation{audioId, document.tracks[1].id, {0, 10}, {}};
        Check(log.Apply(document, std::move(offset), error, message),
              "independent audio offset applies");
        Check(ClipSyncDrift(document, audioId) == RationalTime{-10, 10},
              "audio drift reports its exact signed phase difference");
        TimelineViewport viewport = Viewport();
        viewport.pixels_per_second = 50.0;
        TimelineInteraction independent(document, log, viewport);
        independent.SetLinkedSelectionEnabled(false);
        const double audioY =
            kTimelineRulerHeight + viewport.track_height + 20.0;
        independent.PointerDown(75.0, audioY, 700.0, 10);
        independent.PointerDrag(120.0, audioY, 700.0);
        Check(independent.MovePreview() &&
                  independent.MovePreview()->timeline_in ==
                      RationalTime{10, 10} &&
                  independent.SnapGuideTime() == RationalTime{10, 10},
              "independent audio snaps exactly back to its sync reference");
        independent.CancelDrag();
        const std::string beforeInvalid = document.SaveToString();
        const size_t logCount = log.AppliedCount();
        TimelineInteraction interaction(document, log, viewport);
        const double y = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(125.0, y, 700.0, 10);
        interaction.PointerDrag(50.0, y, 700.0);
        Check(interaction.MovePreview() && !interaction.MovePreview()->valid,
              "partner crossing timeline zero invalidates the whole preview");
        Check(!interaction.PointerUp(error, message) &&
                  document.SaveToString() == beforeInvalid &&
                  log.AppliedCount() == logCount,
              "invalid linked drop emits nothing and preserves exact bytes");
    });

    Test("linked trim previews and commits one atomic operation", [] {
        Document document = Fixture();
        document.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.tracks[0].clips[0].id;
        Operation detach = DetachAudioOperation{
            videoId, document.tracks[1].id, "01KT0000000000000000000006", {}};
        Check(log.Apply(document, std::move(detach), error, message),
              "linked trim fixture detaches audio");
        const Ulid audioId = document.tracks[1].clips[0].id;
        const std::string beforeTrim = document.SaveToString();
        TimelineViewport viewport = Viewport();
        TimelineInteraction interaction(document, log, viewport);
        const double videoY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(150.0, videoY, 700.0, 10);
        interaction.PointerDrag(200.0, videoY, 700.0);
        const auto pending = interaction.PendingOperation();
        Check(interaction.TrimPreview() &&
                  interaction.TrimPreview()->linked_times.size() == 2 &&
                  interaction.TrimPreview()->valid,
              "linked trim previews both A/V rectangles");
        Check(pending &&
                  std::holds_alternative<TrimLinkedClipsOperation>(*pending),
              "linked edge drag emits TrimLinkedClips");
        Check(interaction.PointerUp(error, message),
              "linked trim commits: " + message);
        Check(document.FindClip(videoId)->timeline_in == RationalTime{15, 10} &&
                  document.FindClip(audioId)->timeline_in ==
                      RationalTime{15, 10} &&
                  ClipSyncDrift(document, audioId) == RationalTime{0, 1},
              "linked head trim applies the same exact delta without drift");
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "TrimLinkedClips JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == beforeTrim,
              "one undo restores the pre-trim document bytes");
    });

    Test("invalid linked trim emits nothing byte-identically", [] {
        Document document = Fixture();
        document.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Operation detach = DetachAudioOperation{document.tracks[0].clips[0].id,
                                                document.tracks[1].id,
                                                "01KT0000000000000000000006",
                                                {}};
        Check(log.Apply(document, std::move(detach), error, message),
              "invalid linked trim fixture detaches audio");
        document.tracks[1].clips[0].duration = {2, 10};
        const std::string before = document.SaveToString();
        const size_t logCount = log.AppliedCount();
        TimelineViewport viewport = Viewport();
        TimelineInteraction interaction(document, log, viewport);
        const double videoY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(150.0, videoY, 700.0, 10);
        interaction.PointerDrag(180.0, videoY, 700.0);
        Check(interaction.TrimPreview() && !interaction.TrimPreview()->valid,
              "one constrained linked member marks the whole preview invalid");
        Check(!interaction.PointerUp(error, message) &&
                  document.SaveToString() == before &&
                  log.AppliedCount() == logCount,
              "invalid linked trim emits no event and changes no bytes");
    });

    Test("linked delete is atomic and reversible", [] {
        Document document = Fixture();
        document.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.tracks[0].clips[0].id;
        Operation detach = DetachAudioOperation{
            videoId, document.tracks[1].id, "01KT0000000000000000000006", {}};
        Check(log.Apply(document, std::move(detach), error, message),
              "linked delete fixture detaches audio");
        const Ulid audioId = document.tracks[1].clips[0].id;
        const Ulid group = document.FindClip(videoId)->link_group_id;
        const std::string before = document.SaveToString();
        Operation remove =
            RemoveLinkedClipsOperation{group, {videoId, audioId}, {}};
        Check(log.Apply(document, std::move(remove), error, message) &&
                  !document.FindClip(videoId) && !document.FindClip(audioId),
              "linked delete removes both members in one event");
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "RemoveLinkedClips JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "linked delete undo restores exact document bytes");
    });

    Test("pan, cursor-centered zoom and fit preserve navigation invariants",
         [] {
             TimelineViewport viewport = Viewport();
             const double anchorX = 425.0;
             const RationalTime before = viewport.XToTime(anchorX, 1000);
             viewport.ZoomAroundX(anchorX, 2.0, 1000);
             Check(viewport.XToTime(anchorX, 1000) == before,
                   "zoom keeps the cursor time anchored");
             const RationalTime start = viewport.view_start;
             viewport.ScrollByPixels(100.0, 1000);
             Check(viewport.view_start > start,
                   "positive pan advances the visible start");
             viewport.FitDuration({200, 10}, 1000.0);
             Check(viewport.view_start == RationalTime{0, 10},
                   "fit starts at timeline zero");
             Check(viewport.TimeToX({200, 10}) <= 1000.0,
                   "fit keeps the timeline end visible");
         });

    Test("simulated trim emits the canonical operation and undo restores hash",
         [] {
             Document document = Fixture();
             const uint64_t initialHash = Hash(document.SaveToString());
             TimelineViewport viewport = Viewport();
             EditLog log;
             TimelineInteraction interaction(document, log, viewport);
             const double trackY = kTimelineRulerHeight + 20.0;
             interaction.PointerDown(350.0, trackY, 700.0, 10);
             interaction.PointerDrag(300.0, trackY, 700.0);
             const auto pending = interaction.PendingOperation();
             Check(
                 pending && std::holds_alternative<TrimClipOperation>(*pending),
                 "drag creates a TrimClip operation");
             if (pending) {
                 const auto& trim = std::get<TrimClipOperation>(*pending);
                 Check(trim.clip_id == document.tracks[0].clips[0].id &&
                           trim.edge == TrimEdge::Tail &&
                           trim.delta == RationalTime{-5, 10},
                       "tail drag has the expected exact delta");
             }
             EditError error = EditError::None;
             std::string message;
             Check(interaction.PointerUp(error, message),
                   "valid drag applies through the edit log: " + message);
             Check(log.AppliedCount() == 1,
                   "one mouse gesture emits exactly one operation");
             Check(document.tracks[0].clips[0].duration == RationalTime{15, 10},
                   "document contains the trim result");
             Check(log.Undo(document, error, message),
                   "trim undo succeeds: " + message);
             Check(Hash(document.SaveToString()) == initialHash,
                   "undo restores the initial canonical document hash");
         });

    Test("clip body drag moves atomically across tracks and undo restores hash",
         [] {
             Document document = Fixture();
             document.tracks.push_back(
                 {"01KT0000000000000000000005", "video", 1, {}});
             const std::string initial = document.SaveToString();
             TimelineViewport viewport = Viewport();
             EditLog log;
             TimelineInteraction interaction(document, log, viewport);
             const double firstTrackY = kTimelineRulerHeight + 20.0;
             const double secondTrackY =
                 kTimelineRulerHeight + viewport.track_height + 20.0;
             interaction.PointerDown(200.0, firstTrackY, 700.0, 10);
             interaction.PointerDrag(250.0, secondTrackY, 700.0);
             Check(
                 interaction.MovePreview() && interaction.MovePreview()->valid,
                 "cross-track move preview is valid");
             const auto pending = interaction.PendingOperation();
             Check(
                 pending && std::holds_alternative<MoveClipOperation>(*pending),
                 "body drag emits MoveClip");
             if (pending) {
                 const auto& move = std::get<MoveClipOperation>(*pending);
                 Check(move.track_id == document.tracks[1].id &&
                           move.timeline_in == RationalTime{15, 10},
                       "move carries exact destination track and time");
                 const std::string json = SerializeOperation(*pending);
                 Operation parsed = RemoveClipOperation{};
                 EditError parseError = EditError::None;
                 std::string parseMessage;
                 Check(DeserializeOperation(json, parsed, parseError,
                                            parseMessage) &&
                           SerializeOperation(parsed) == json,
                       "MoveClip JSON round-trips canonically");
             }
             EditError error = EditError::None;
             std::string message;
             Check(interaction.PointerUp(error, message),
                   "move applies through one edit-log entry: " + message);
             Check(log.AppliedCount() == 1 &&
                       document.tracks[0].clips.size() == 1 &&
                       document.tracks[1].clips.size() == 1,
                   "one event transfers the clip between tracks");
             Check(log.Undo(document, error, message),
                   "move undo succeeds: " + message);
             Check(document.SaveToString() == initial,
                   "move undo restores canonical document bytes");
         });

    Test("overlapping clip move overwrites covered clips atomically", [] {
        Document document = Fixture();
        const std::string initial = document.SaveToString();
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        const double trackY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(200.0, trackY, 700.0, 10);
        interaction.PointerDrag(500.0, trackY, 700.0);
        Check(interaction.MovePreview() && interaction.MovePreview()->valid,
              "overlap is accepted as an overwrite preview");
        EditError error = EditError::None;
        std::string message;
        Check(interaction.PointerUp(error, message) && log.AppliedCount() == 1,
              "overwrite emits one atomic move event");
        Check(
            document.tracks[0].clips.size() == 1 &&
                document.tracks[0].clips[0].id ==
                    "01KT0000000000000000000003" &&
                document.tracks[0].clips[0].timeline_in == RationalTime{40, 10},
            "fully covered destination clip is removed");
        Check(log.Undo(document, error, message), "overwrite undo succeeds");
        Check(document.SaveToString() == initial,
              "overwrite undo restores document bytes");
    });

    Test("overwrite through a longer clip preserves both survivors", [] {
        Document document = Fixture();
        document.tracks.push_back({"01KT0000000000000000000005",
                                   "video",
                                   1,
                                   {{"01KT0000000000000000000006",
                                     "01KT0000000000000000000001",
                                     {40, 10},
                                     {50, 10},
                                     {30, 10}}}});
        const std::string initial = document.SaveToString();
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        const double firstTrackY = kTimelineRulerHeight + 20.0;
        const double secondTrackY =
            kTimelineRulerHeight + viewport.track_height + 20.0;
        interaction.PointerDown(200.0, firstTrackY, 900.0, 10);
        interaction.PointerDrag(600.0, secondTrackY, 900.0);
        EditError error = EditError::None;
        std::string message;
        Check(interaction.PointerUp(error, message),
              "spanning overwrite applies: " + message);
        const auto& clips = document.tracks[1].clips;
        Check(clips.size() == 3,
              "spanning destination becomes left, moved and right clips");
        Check(clips[0].id == "01KT0000000000000000000006" &&
                  clips[0].timeline_in == RationalTime{30, 10} &&
                  clips[0].duration == RationalTime{20, 10},
              "left survivor retains its ULID");
        Check(clips[1].id == "01KT0000000000000000000003" &&
                  clips[1].timeline_in == RationalTime{50, 10},
              "moved clip occupies the overwrite interval");
        const Ulid rightSurvivorId = clips[2].id;
        Check(rightSurvivorId != clips[0].id && IsValidUlid(rightSurvivorId) &&
                  clips[2].source_in == RationalTime{80, 10} &&
                  clips[2].timeline_in == RationalTime{70, 10} &&
                  clips[2].duration == RationalTime{10, 10},
              "right survivor gets an exact stable identity and source range");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == initial,
              "spanning overwrite undo restores exact bytes");
        Check(log.Redo(document, error, message),
              "spanning overwrite redo succeeds");
        Check(document.tracks[1].clips[2].id == rightSurvivorId,
              "redo retains the right survivor ULID");
    });

    Test("split clip is atomic, stable and exactly reversible", [] {
        Document document = Fixture();
        const std::string initial = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(log.Apply(document,
                        SplitClipOperation{
                            document.tracks[0].clips[0].id, {20, 10}, {}},
                        error, message),
              "split applies: " + message);
        const Ulid rightId =
            std::get<SplitClipOperation>(log.AppliedEntries().back().op)
                .right_clip_id;
        Check(IsValidUlid(rightId), "split generates a stable right ULID");
        Check(document.tracks[0].clips.size() == 3,
              "split adds one clip without ripple");
        const DocumentClip& left = document.tracks[0].clips[0];
        const DocumentClip& right = document.tracks[0].clips[1];
        Check(left.duration == RationalTime{10, 10} && right.id == rightId &&
                  right.source_in == RationalTime{20, 10} &&
                  right.duration == RationalTime{10, 10} &&
                  right.timeline_in == RationalTime{20, 10},
              "split preserves exact source and timeline continuity");
        const std::string operationJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(operationJson, parsed, error, message) &&
                  SerializeOperation(parsed) == operationJson,
              "SplitClip JSON round-trips canonically");
        Check(log.Undo(document, error, message), "split undo succeeds");
        Check(document.SaveToString() == initial,
              "split undo restores document bytes");
        Check(log.Redo(document, error, message), "split redo succeeds");
        Check(document.tracks[0].clips[1].id == rightId,
              "split redo retains the generated right ULID");
    });

    Test("split at a clip boundary is rejected atomically", [] {
        Document document = Fixture();
        const std::string initial = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(!log.Apply(document,
                         SplitClipOperation{
                             document.tracks[0].clips[0].id, {10, 10}, {}},
                         error, message) &&
                  error == EditError::InvalidTimelineIn,
              "cut on head is rejected");
        Check(log.AppliedCount() == 0 && document.SaveToString() == initial,
              "rejected cut leaves bytes and event log unchanged");
    });

    Test("trim drag is clamped before duration can cross zero", [] {
        Document document = Fixture();
        const std::string initial = document.SaveToString();
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        const double trackY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(150.0, trackY, 700.0, 10);
        interaction.PointerDrag(400.0, trackY, 700.0);
        Check(interaction.TrimPreview() && interaction.TrimPreview()->valid &&
                  interaction.TrimPreview()->times.duration ==
                      RationalTime{1, 10},
              "preview stops at one timebase tick of duration");
        EditError error = EditError::None;
        std::string message;
        Check(interaction.PointerUp(error, message),
              "release applies the constrained trim");
        Check(log.AppliedCount() == 1 &&
                  document.tracks[0].clips[0].duration == RationalTime{1, 10},
              "constrained trim emits one valid event");
        Check(log.Undo(document, error, message), "constrained trim undoes");
        Check(document.SaveToString() == initial,
              "undo restores bytes after constrained trim");
    });

    Test("magnetism snaps a trim to the neighboring cut", [] {
        Document document = Fixture();
        TimelineViewport viewport = Viewport();
        viewport.pixels_per_second = 50.0;
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        const double trackY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(200.0, trackY, 700.0, 10);
        interaction.PointerDrag(243.0, trackY, 700.0);
        Check(interaction.TrimPreview() && interaction.TrimPreview()->valid &&
                  interaction.TrimPreview()->times.duration ==
                      RationalTime{30, 10},
              "tail snaps exactly to the next clip start");
        Check(interaction.SnapGuideTime() &&
                  *interaction.SnapGuideTime() == RationalTime{40, 10},
              "snap exposes an exact guide time for rendering");
        EditError error = EditError::None;
        std::string message;
        Check(interaction.PointerUp(error, message), "snapped trim applies");
        Check(document.tracks[0].clips[0].duration == RationalTime{30, 10},
              "document receives the snapped duration");
    });

    Test("tail trim cannot pass the source media end", [] {
        Document document = Fixture();
        document.tracks[0].clips.erase(document.tracks[0].clips.begin() + 1);
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        const double trackY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(350.0, trackY, 2200.0, 10);
        interaction.PointerDrag(2000.0, trackY, 2200.0);
        Check(interaction.TrimPreview() && interaction.TrimPreview()->valid &&
                  interaction.TrimPreview()->times.duration ==
                      RationalTime{90, 10},
              "preview stops exactly at source duration");
        Check(interaction.TrimPreview()->times.source_in.add(
                  interaction.TrimPreview()->times.duration) ==
                  document.sources[0].duration,
              "preview source range ends at the media boundary");
    });

    return failures == 0 ? 0 : 1;
}
```

## Skipped files (manifest)

| Path | Reason |
|---|---|
| .clang-format | tooling/CI config |
| .clang-tidy | tooling/CI config |
| .github/dependabot.yml | tooling/CI config |
| .github/pull_request_template.md | tooling/CI config |
| .github/workflows/build.yml | tooling/CI config |
| .github/workflows/lint.yml | tooling/CI config |
| .github/workflows/sanitize.yml | tooling/CI config |
| .github/workflows/secrets-scan.yml | secret filename pattern |
| .gitignore | tooling/CI config |
| Brewfile | tooling/CI config |
| eval-trace.json | too large (422 KB > 300 KB) |
