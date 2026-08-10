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
