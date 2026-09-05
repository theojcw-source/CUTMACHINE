# Consignes pour les agents

Fichier lu automatiquement par les agents de code (Codex via `AGENTS.md`,
Claude Code via `CLAUDE.md` qui pointe ici). Il contient ce qu'on ne peut pas
deviner en lisant le code, et ce que les agents précédents ont enfreint.

À lire avant de coder : [`PHILOSOPHY.md`](PHILOSOPHY.md) (les principes 1 à 5
ne se négocient pas), [`ROADMAP.md`](ROADMAP.md) (les garde-fous transverses),
[`AUDIT.md`](AUDIT.md) si le ticket touche au pilotage par modèle.

## Build et tests

Prérequis : macOS, CMake 3.24+, pkg-config, FFmpeg. `brew bundle --file=Brewfile`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Le workflow `ctest.yml` lance désormais la suite complète sur macOS et un
sous-ensemble sans AppKit sous ASan/UBSan. `build.yml` et `sanitize.yml`
restent des garde-fous de compilation séparés. Lance quand même `ctest`
localement avant de rendre, systématiquement : la CI n'est pas une boucle de
développement.

La CI vérifie en revanche le formatage (`clang-format-18 -style=file`, sur les
fichiers modifiés seulement) et `clang-tidy -p build` sur les `.cc`/`.mm`
modifiés. Reformate avant de committer.

## Contraintes structurelles

Elles viennent des principes 1 à 5 de `PHILOSOPHY.md`.
`tests/architecture_tests.py` (cible `cutmachine_architecture_tests`) en garde
une partie mécaniquement — s'il casse, c'est l'architecture qui a bougé, pas
le test qui a tort.

- **Rien ne mute un `Document` directement.** Toute modification est une
  structure de `Operations.h` appliquée via `EditLog::Apply`, réversible et
  sérialisable. Une fonctionnalité qui ne s'exprime pas comme opération est
  probablement mal conçue.
- **`RationalTime` partout.** Jamais un flottant sur une position ou une
  durée. Les conversions non exactes se refusent, elles ne s'arrondissent pas.
  La seule frontière entre temps exact et pixels est `TimelineViewport`
  ([`TimelineView.h`](src/TimelineView.h)), et elle arrondit explicitement.
- **Adressage par ULID.** Jamais par index de tableau.
- **Le moteur d'abord.** Une capacité existe dans `src/`, puis est exposée au
  CLI (`--apply-op`), au MCP (`McpTools.cc`), puis à l'UI. Jamais l'inverse.
  Ce qui n'est accessible qu'à la souris n'existe pas.
- **L'état d'interface est une préférence locale.** `NSUserDefaults` via
  [`UiPreferences.h`](src/UiPreferences.h) — jamais dans le document projet.
  Rouvrir un projet sur une autre machine doit produire le même montage.
- **Test de round-trip obligatoire** pour toute nouvelle opération : apply →
  undo → comparaison octet à octet, plus un test de sérialisation canonique.

## Conventions de code

- Style : Google, indentation 4 espaces, 80 colonnes, pointeurs à gauche
  (`.clang-format`). `IncludeBlocks: Preserve` — ne réordonne pas le bloc
  `extern "C"` de FFmpeg.
- **Commentaires de code en anglais, textes d'interface et documentation en
  français.** C'est la convention en place partout, respecte-la.
- Les commentaires d'en-tête citent leur ticket (`F2.1 -- ROADMAP.md`) et
  expliquent *pourquoi*, pas *quoi*. Continue cet usage : c'est la trace que
  laisse chaque agent au suivant.
- `clang-tidy` exclut volontairement `cppcoreguidelines` et la plupart des
  `modernize` : ce code interopère avec FFmpeg et les frameworks Apple, les
  pointeurs bruts y sont normaux.

## Interface — les quatre faits que les agents enfreignent

1. **Le layout est en frames + autoresizing masks. Aucun Auto Layout nulle
   part.** Ne sois pas le premier à en introduire sans l'avoir discuté.
2. **La palette, les espacements et l'échelle typographique viennent de
   [`UiTheme.h`](src/UiTheme.h).** Pas de littéral de couleur dans un nouveau
   code. La conversion vers AppKit passe par `UiThemeAppKit.h`. (État actuel :
   `timelineRenderData` contient encore ~95 littéraux hérités — ne t'en sers
   pas comme précédent, c'est de la dette identifiée.)
3. **La timeline et le moniteur sont dessinés en Metal, en rectangles pleins
   uniquement.** `TimelineRenderData` est une display list de quads opaques
   consommée par `vertex_solid`/`fragment_solid` ([`shader.metal`](src/shader.metal)).
   Il n'existe **pas** de rendu de texte (seulement un afficheur 7 segments
   dans `addTinyText`, chiffres et `f s m p -`), pas de coins arrondis, pas de
   contours, pas d'ombres, pas d'antialiasing. `kCornerRadius` existe dans le
   thème et rien ne sait l'appliquer côté Metal. Ne planifie pas un rendu qui
   suppose ces primitives sans les implémenter d'abord.
4. **Rien de la Phase 2 n'a jamais été affiché à l'écran.** Tout a été écrit
   dans un sandbox Linux sans AppKit ni Metal. « Ça a l'air correct » n'est pas
   un état atteignable par lecture : voir
   [`VISUAL_QA_CHECKLIST.md`](VISUAL_QA_CHECKLIST.md), qui est la dette à
   solder sur un vrai Mac.

Piège supplémentaire : la géométrie **dessinée** vit dans `timelineRenderData`
([`src/main.mm`](src/main.mm), non testée) et la géométrie **cliquable** dans
[`TimelineView.cc`](src/TimelineView.cc) (pure, testée). Deux codes séparés
pour le même rectangle. Si tu touches à l'un, vérifie l'autre — la divergence
se voit à la souris et ne fait échouer aucun test.

## Reprendre un travail de design

Un mockup HTML/CSS ou une image ne se traduit pas mécaniquement vers des quads
Metal et des frames AppKit : ni flexbox, ni `border-radius`, ni `box-shadow`,
ni rendu de texte n'ont d'équivalent ici. Un mockup est une **référence
visuelle**, jamais une source à porter.

Ce qui se transmet réellement d'un agent à l'autre :

- des tokens, sous forme de constantes dans `UiTheme.h` (testables sans macOS,
  voir `tests/ui_theme_tests.cc`) ;
- de la géométrie, sous forme de display list ou de table déclarative
  (`TimelineRenderData`, [`PanelLayout.h`](src/PanelLayout.h)) ;
- les états énumérés explicitement : survol, actif, sélectionné, désactivé,
  focus (aujourd'hui non spécifiés — et il n'existe aucun `NSTrackingArea`
  dans le projet, donc aucun survol) ;
- des captures de référence, comme cible de comparaison.

## Plateforme

macOS uniquement, et sans conditionnel : `CMakeLists.txt` déclare `OBJC OBJCXX`
et linke `-framework AppKit / Metal / AVFAudio / QuartzCore` sans `if(APPLE)`.
Ailleurs, le projet ne configure même pas.

Une partie du code est malgré tout du C++ portable, sans dépendance AppKit, et
ses suites tournent sur un hôte Linux : `ui_theme`, `panel_layout`,
`timeline_view`, `transport_bar`, `timecode`, `media_panel_model`,
`inspector_grade_controls`, `fft`, `beat_detection`. Cette séparation est
volontaire — la *politique* d'interface est portable et testable, les `.mm` ne
font que la câbler. Ne la casse pas en remontant de la logique dans un `.mm`.

## À ne pas faire

- Committer des médias (`*.mp4`, `*.mov`, `*.mxf` sont ignorés — le repo
  contient déjà un fichier de test de 268 Mo hors index, ne l'ajoute pas).
- Écrire une clé d'API dans un fichier suivi. Les modèles distants s'utilisent
  avec la clé de l'utilisateur, sans compte ni service central.
- Copier, transposer ou traduire ligne à ligne le code de l'éditeur concurrent
  étudié dans `ROADMAP.md` (GPLv3). Sa lecture sert à comprendre un problème,
  jamais à être reproduite. En cas de doute sur cette limite : s'arrêter et
  demander.
- Ajouter une fonctionnalité « parce qu'un concurrent l'a ». Elle entre si elle
  sert la visibilité sur l'état du projet ou la capacité à intervenir dessus.
