# CUTMACHINE

CUTMACHINE charge un package projet v2, ouvre chaque source média par
son ULID, puis résout le scrub en clés de cache `(source_id, source_frame)`.
Les intervalles des clips sont semi-ouverts : `[timeline_in, timeline_in +
duration)`. Un trou ne déclenche aucun décodage et est rendu en noir.

## Build et tests

Prérequis : macOS, CMake 3.24+, pkg-config et FFmpeg (`libavformat`,
`libavcodec`, `libavutil`, `libswresample`, `libswscale`).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Le test natif `cutmachine_ui_smoke_tests` ouvre l’éditeur sur un projet
jetable et vérifie les vrais contrôles AppKit/Metal : affichage/masquage du
moniteur Source, glissières de l’Inspecteur, montage Insérer/Écraser et dépôt
d’un média dans la timeline. Il couvre aussi le clic de playhead, le déplacement
d’un clip et la navette J/K/L à vitesses 1×, 2× et 4×. Il s’exécute dans une session macOS graphique,
sans autorisation Accessibilité :

```sh
ctest --test-dir build -R cutmachine_ui_smoke_tests --output-on-failure
```

## Lancement

La compilation produit une application macOS dans `build/CUTMACHINE.app`.
Ouvrez-la par double-clic dans le Finder, depuis Spotlight après l’avoir copiée
dans Applications, ou compilez-la si nécessaire puis lancez-la avec la cible
`run-app` :

```sh
cmake --build build --target run-app -j
```

Pour ouvrir directement un bundle déjà compilé :

```sh
open build/CUTMACHINE.app
```

Le binaire en ligne de commande reste également disponible et peut être lancé
sans argument :

```sh
./build/cutmachine
```

### Boucle de développement

Le script de développement fournit une boucle proche de `npm run dev` pour
l’application native : chaque modification des sources déclenche une
compilation incrémentale, ferme proprement l’ancienne instance, puis ouvre le
nouveau bundle. Il n’a besoin ni de Homebrew ni de `fswatch`.

```sh
./scripts/dev.sh
```

Il ne s’agit pas d’un hot reload dans le processus : AppKit et les shaders
Metal sont reconstruits, puis l’application redémarre. Le projet récent peut
ensuite être rouvert depuis l’écran d’accueil.

Une page d’accueil permet de créer un projet, d’ouvrir un fichier existant ou
de reprendre l’un des huit projets récents. La création demande le nom et
l’emplacement du package `.cutmachine-project`, initialise une première timeline,
puis ouvre directement l’espace de montage.

Pour le stockage, les sauvegardes, les médias liés et le partage entre machines,
voir la [spécification de stockage des projets](docs/PROJECT_STORAGE_SPEC.md).
**Fichier → Collecter le projet…** crée un package `.cutmachine-project`
autonome contenant une copie vérifiée des originaux, un manifeste SHA-256 et
un historique distinct par timeline. Ce package peut être transféré puis ouvert
directement par CUTMACHINE sur un autre Mac. À l’ouverture, les empreintes sont
revérifiées et tout original manquant ou modifié déclenche un avertissement.
Un verrou de session empêche également deux instances d’écrire simultanément
le même projet. Dans le package v2, chaque timeline vit dans son propre fichier
`Timelines/<ULID>.json` et est sauvegardée transactionnellement avec le snapshot
projet et les historiques. Les anciens fichiers JSON et packages v1 ne sont
plus acceptés.

Il reste possible d’ouvrir directement un projet depuis la ligne de commande en
visant son fichier interne :

```sh
./build/cutmachine ./Film.cutmachine-project/project.cutmachine.json
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
- Les médias vidéo portant un canal alpha sont composités dans Metal. Chaque
  clip vidéo possède aussi une opacité exacte `num/den`, modifiable dans
  l’Inspecteur ou par l’opération `SetClipOpacity`; les fondus multiplient cette
  valeur et l’export opaque en tient compte.

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
`Shift` pendant le drag active le ripple trim : le bord est retaillé et tous
les clips aval des pistes en sync lock suivent le raccord. `Cmd` active le roll
edit sur deux clips contigus : la coupe se déplace, mais la fin du programme ne
change pas. L'outil Slip (`Y`) glisse le contenu source sous un rectangle dont
la position et la durée timeline restent fixes ; le geste est borné par le
début et la fin du rush. Les variantes liées appliquent le même delta exact aux
rectangles image et audio séparés ; chacune est journalisée comme une seule
opération annulable.
`Cmd+C` copie la sélection et `Cmd+V` la colle en overwrite au playhead ;
`Option`-glisser duplique directement vers la position visée. Une sélection
liée recrée des clips image/audio séparés avec un nouveau groupe de liaison,
dans une unique `PasteClipsOperation` annulable.
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
donc annulable et rejouable à l'octet près. Lorsque **Sélection liée** est
active et que le clip suivant possède un audio lié, la fermeture du trou
décale ensemble les pistes image et audio de la même durée exacte.

La palette en haut à gauche expose les outils Sélection (`V`), Main (`H`),
Zoom (`Z`, avec `Option` pour dézoomer), Lame (`C` ou `B`) et Slip (`Y`). La lame affiche
la future coupe en rouge et un clic dans un clip crée deux segments contigus,
avec un nouvel ULID stable pour celui de droite. Lorsque **Sélection liée** est
active, la même coupe est appliquée atomiquement au clip audio séparé et les
deux côtés deviennent deux nouvelles paires A/V indépendantes. La sélection permet aussi de
scrubber en continu dans les trous et la règle. `Espace` lance ou arrête la lecture ;
maintenu pendant un drag, il devient temporairement l'outil Main. `J`, `K` et
`L` contrôlent la lecture arrière, l'arrêt et la lecture avant. `F` cadre toute
la timeline, `+`/`-` zooment, Home/End rejoignent les extrémités, les flèches
gauche/droite avancent d'une frame (`Shift` : dix frames), et haut/bas vont au
raccord précédent/suivant. Maintenir `Cmd` pendant le scrub aimante la playhead
au raccord le plus proche ; pendant un move ou un trim, `Cmd` ajoute aussi la
playhead aux cibles de magnétisme.
La ligne `+` sous les en-têtes est divisée en deux : bleu pour ajouter une
piste vidéo, vert pour ajouter une piste audio. `Cmd+Shift+T` ajoute une piste
vidéo et `Cmd+Option+Shift+T` une piste audio. La création est atomique et
annulable ; un clip peut ensuite être glissé vers une piste compatible avec
l'outil Sélection. Le player résout et compose toutes les pistes vidéo du
document, sans limite fonctionnelle fixée à deux couches. `Cmd+T` ajoute un
fondu enchaîné centré sur le cut vidéo au playhead (ou sur un raccord voisin
du clip sélectionné). La transition est un objet de séquence distinct des
clips : elle référence les deux clips, valide les poignées média exactes, se
journalise via `Add/Update/RemoveTransition` et se rend de façon identique
dans Metal et dans l’export FFmpeg. Elle ne crée ni ne modifie aucun clip
audio.
Un clic droit sur une piste propose **Supprimer la piste** et **Supprimer les
pistes vides**. Une piste occupée demande confirmation ; ses clips sont inclus
dans l'inverse de `RemoveTrackOperation`, afin que `Cmd+Z` restaure la piste et
son contenu exacts.

Le menu **CUTMACHINE > Raccourcis clavier…** (`Cmd+,`) permet de réassigner les
outils, la lecture J/K/L, les points In/Out, delete/ripple delete, le fondu
enchaîné, le magnétisme, la sélection liée, le cadrage et les commandes Source
vers Record. Un clavier AZERTY visuel permet de choisir une commande, ses
modificateurs, puis de cliquer directement sur la touche à lui affecter. La
saisie textuelle reste disponible pour les combinaisons comme `V`, `Space`,
`Alt+X` ou `Cmd+Shift+L`. Une valeur vide désactive la commande. Les doublons et les
conflits avec les raccourcis système fixes sont refusés ; un bouton restaure
les valeurs par défaut. La configuration est globale à l'application,
persistée dans les préférences macOS, et les menus sont actualisés
immédiatement. Le plein écran est affecté à `P` par défaut et reste
réassignable dans cette même fenêtre.

Le cadenas dans chaque en-tête verrouille la piste via une opération persistée
et annulable. Une piste verrouillée reste rendue et lisible sous des hachures,
mais toute mutation qui la cible est refusée atomiquement. Le séparateur
vertical du chutier ajuste sa largeur ; la frontière entre moniteur et timeline
se glisse verticalement pour adapter l’espace de montage.

## Audio

Les flux audio des sources sont décodés par FFmpeg, puis
convertis en PCM float stéréo 48 kHz par `libswresample`. Un `AVAudioSourceNode`
mixte en temps réel uniquement les clips placés sur des pistes audio. Un clip
vidéo est toujours muet : aucun fallback de lecture ou d'export ne récupère
son audio. Le callback audio lit un plan immuable construit depuis la
timeline et ne touche jamais directement au document éditable. `Espace` et
`J/K/L` pilotent simultanément image et son, y compris la navette accélérée
avant/arrière à 2× et 4× (échantillonnage accéléré, sans correction de pitch) ;
un seek, un trim, un move, une
fermeture de trou ou un undo reconstruit le plan de mixage au raccord exact.
Les échantillons additionnés sont limités dans `[-1, 1]` pour éviter un
dépassement numérique lors du mixage multipiste.

À l'ouverture d'un ancien projet, tout clip vidéo dont le drapeau de migration
indique encore du son embarqué est séparé. L'application crée au besoin des
pistes audio et émet une
`DetachAudioOperation` par clip dans l'event log : le rectangle audio reçoit
son propre ULID mais conserve exactement les mêmes `source_in`, durée et
`timeline_in`. Tous les nouveaux montages créent directement cette paire avec
`include_audio:false` sur la vidéo ; le son n'est donc jamais embarqué ni joué
deux fois. Audio et image peuvent ensuite être déplacés, trimés,
coupés ou supprimés indépendamment. Cette normalisation est persistée et reste
annulable avec `Cmd+Z`. Le bouton **Séparer audio** (`U`) reste disponible pour
migrer manuellement un ancien clip.

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

Le panneau **Projet / Chutiers** occupe la gauche de la fenêtre, hors de
la surface de timeline Metal. La séquence active y apparaît comme un objet
adressable distinct des médias ; un double-clic revient au moniteur Programme.
Une arborescence affiche les chutiers imbriqués, la racine et une vue de tous
les objets. Un sélecteur propose une liste de
métadonnées ou une grille d'icônes ; le champ de recherche filtre les deux.
Dans les deux vues, `Cmd+clic` ajoute ou retire un objet de la sélection et
`Shift+clic` sélectionne une plage, comme dans le Finder. Le clic droit conserve
la multisélection et **Déplacer…** classe ensemble tous les rushes sélectionnés.
**+ Chutier** crée un enfant du chutier courant et **Supprimer** refuse un
chutier contenant encore des médias ou des enfants. Les chutiers se déplacent
et s'imbriquent par glisser-déposer, à la manière du Finder ; le dépôt sur
**Sans chutier** les replace à la racine. Ces déplacements utilisent
`MoveBinOperation`, rejettent les cycles et suivent donc
`Cmd+Z`/`Cmd+Shift+Z`. Les champs `bins`,
`parent_id` et `bin_id` sont persistés et exposés par `--describe`. Voir la
[`spécification des chutiers`](docs/BINS_SPEC.md).
Le menu contextuel **Nouveau chutier** crée immédiatement l'élément dans
l'arborescence et active son renommage inline, sans ouvrir de dialogue. Un clic
droit dans la zone vide crée à la racine ; **Renommer** édite également le nom
directement dans la vue.

**Fichier → Importer des rushes…** (`Cmd+I`) accepte plusieurs vidéos ou un
dossier complet et range directement les nouveaux médias dans le chutier
sélectionné. Des fichiers du Finder et des rushes déjà présents peuvent aussi
être déposés sur un chutier. Le menu contextuel **Déplacer…** propose la même
organisation sans drag-and-drop via `SetMediaBinOperation`.

L’analyse FFmpeg des imports passe par un `MediaTaskManager` à concurrence
bornée. Chaque tâche possède un ULID, un type, un état, une progression, un
détail et une erreur éventuelle. Le panneau Média montre la tâche active et
permet son annulation coopérative. Les workers ne mutent jamais le projet : un
batch terminé est validé, appliqué et sauvegardé sur le thread principal.

Les proxies utilisent ce même ordonnanceur. Les rushes supérieurs à 1920 px,
HEVC/H.265 ou AV1 déclenchent automatiquement un transcodage ProRes Proxy
silencieux, limité à 1280 px de large, dans `.cutmachine/proxies/`. Le chemin
du proxy est persisté dans le projet sans remplacer celui de l'original. Le
menu contextuel d'un rush permet de le générer, le recréer ou le supprimer ;
**Timeline → Utiliser les proxies** bascule immédiatement entre proxy et
original. Un proxy absent, illisible ou incompatible retombe sur l'original.
L'audio et l'export final utilisent toujours les médias originaux : aucun son
n'est embarqué dans le clip vidéo ni dans son proxy.

Les médias contenant de l'audio déclenchent aussi une analyse waveform en
arrière-plan. Le cache binaire, régénérable, vit dans
`.cutmachine/waveforms/` et n'alourdit pas le JSON du projet. Les clips des
pistes audio affichent la portion exacte correspondant à leur `source_in` ;
les pics sont agrégés selon le zoom afin de conserver les transitoires. Les
clips vidéo restent silencieux et n'affichent jamais de waveform.

La vue grille des chutiers utilise des thumbnails vidéo 320×180 générées par
le même scheduler et stockées dans `.cutmachine/thumbnails/`. L'image est
extraite à 10 % du rush (au plus à 10 secondes), respecte l'orientation puis
est letterboxée sans déformation. Une vignette absente ou corrompue est
recréée automatiquement ; **Régénérer la vignette** force son remplacement.
Les médias offline conservent leur avertissement visuel.

Le menu contextuel **Reconnecter le média…** remplace le fichier d'un rush
sans changer son ULID, ses clips, ses liens A/V ni son chutier. Le nouveau
fichier est probé par une tâche `Relink` avant toute mutation. Une cadence
différente, une durée inférieure à l'original ou l'absence d'une piste audio
déjà utilisée est refusée. Après sauvegarde transactionnelle, les workers
vidéo et audio sont reconstruits et les proxy, waveform et thumbnail obsolètes
sont invalidés puis régénérés depuis le remplacement.

**Fichier → Reconnecter les médias offline…** effectue la même opération en
lot depuis un dossier parcouru récursivement. La correspondance utilise le nom
de fichier sans tenir compte de la casse. Un nom absent reste offline ; un nom
présent plusieurs fois est signalé comme ambigu et n'est jamais choisi au
hasard. Tous les remplacements compatibles trouvés sont sauvegardés dans une
seule transaction, puis les workers et caches ne sont reconstruits qu'une
fois pour le lot entier.

Un double-clic sur un média, ou le bouton **Source**, l'ouvre dans le moniteur
Metal. Source et Record possèdent leurs propres points In/Out : `I`, `O` et
`Alt+X` agissent sur le moniteur actif. `,` insère la zone Source sur la piste
vidéo ciblée en ouvrant les pistes en sync lock ; `.` écrase la même durée sans
décaler l'aval. Ces montages sont une seule opération exacte, annulable, même
lorsqu'un clip au point de montage doit être scindé. Un drag depuis la liste
ou la grille vers une piste vidéo crée une
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
`SplitClipOperation`, ainsi que `RippleTrimOperation`, `RollEditOperation`,
`SlipEditOperation`, les variantes liées de move, trim et remove et
`AddTrackOperation` pour le
multipiste. Les marqueurs de projet sont des objets
adressables persistants ; `AddMarkerOperation`, `RemoveMarkerOperation` et
`UpdateMarkerOperation` passent par le même journal et apparaissent dans
`--describe` sous les aliases `K1`, `K2`, etc. Les réglages de la séquence
passent eux aussi par `UpdateSequenceOperation`, avec validation atomique et
undo/redo sans remplacer son ULID ni sa timeline. La feuille de route
comportementale est décrite dans
[`docs/NLE_TIMELINE_SPEC.md`](docs/NLE_TIMELINE_SPEC.md).
`JoinClipOperation` est l'inverse exact persisté d'une coupe. `EditLog::Apply`,
`Undo` et `Redo` renvoient un `EditError` nommé et garantissent l'atomicité du
document.

Le log conserve, dans les inverses Insert/Remove, les représentations exactes
des `timeline_in` affectés par le ripple. Cette métadonnée est nécessaire pour
restaurer les octets canoniques d'origine lorsque des timebases différentes
représentent le même instant. Elle ne constitue pas une opération supplémentaire.

Deux fondations restent volontairement indépendantes de l'interface :
`ProjectRecovery` écrit et inspecte un autosave canonique dérivé sans jamais
remplacer implicitement le document principal, et `KeyframeTrack` fournit des
keyframes à ULID stable avec interpolation Hold/Linear. Le temps des keyframes
reste rationnel jusqu'au calcul de la fraction d'interpolation ; seules les
valeurs de paramètres sont flottantes.

## Commandes headless

Ces commandes s'exécutent avant toute initialisation d'AppKit, de Metal ou du
décodage média :

```sh
./build/cutmachine --create-project ./Film.cutmachine-project "Film"
./build/cutmachine --transcribe \
  ./Film.cutmachine-project/project.cutmachine.json '<media-id>[,<media-id>…]' \
  ./models/ggml-large-v3.bin fr --verbatim
./build/cutmachine --align-transcripts \
  ./Film.cutmachine-project/project.cutmachine.json --write
./build/cutmachine --tighten-pauses \
  ./Film.cutmachine-project/project.cutmachine.json '<clip-id>' 400 6
./build/cutmachine --locate-source-frame \
  ./Film.cutmachine-project/project.cutmachine.json '<media-id>' 1412
./build/cutmachine --disfluencies \
  ./Film.cutmachine-project/project.cutmachine.json '<clip-id>'
./build/cutmachine --remove-words \
  ./Film.cutmachine-project/project.cutmachine.json '<clip-id>' \
  '[{"start_word_index":40,"end_word_index":40}]'
./build/cutmachine --shot-quality \
  ./Film.cutmachine-project/project.cutmachine.json '<media-id>'
./build/cutmachine --shot-quality-report \
  ./Film.cutmachine-project/project.cutmachine.json
./build/cutmachine --describe ./Film.cutmachine-project/project.cutmachine.json
./build/cutmachine --apply-op ./Film.cutmachine-project/project.cutmachine.json \
  '{"type":"TrimClip","clip_id":"01K00000000000000000000003","edge":"Tail","delta":{"value":-1,"rate":25},"exact_clip":null}'
./build/cutmachine --apply-project-op ./Film.cutmachine-project/project.cutmachine.json \
  '{"type":"AddProjectTimeline","name":"Vertical","width":1080,"height":1920,"frame_rate":{"num":25,"den":1},"timeline_id":"","video_track_id":"","audio_track_id":"","exact_project_hex":null}'
./build/cutmachine --undo-project-op ./Film.cutmachine-project/project.cutmachine.json
./build/cutmachine --redo-project-op ./Film.cutmachine-project/project.cutmachine.json
./build/cutmachine --ingest ./Film.cutmachine-project/project.cutmachine.json ./rushes --recursive
./build/cutmachine --import-resolve ./Film.cutmachine-project/project.cutmachine.json ./manifest.json
./build/cutmachine --export ./Film.cutmachine-project/project.cutmachine.json ./film.mp4
```

## Import depuis DaVinci Resolve

Un chutier Resolve n'est pas un fichier : il vit dans la base du projet, et un
`.drp` est une archive opaque. Il n'y a donc rien à analyser — la seule entrée
honnête est de faire parler Resolve.

### Depuis Resolve, en un clic

```sh
tools/resolve-plugin/install.sh
```

Puis dans Resolve : `Workspace → Scripts → Utility → CUTMACHINE`. La fenêtre
importe les chutiers du projet ouvert et sait interroger le moteur
(`--describe`) pour montrer ce que verra l'agent. Voir
[`tools/resolve-plugin/README.md`](tools/resolve-plugin/README.md).

C'est le chemin recommandé : le script tourne **dans** Resolve, donc il ne
réclame pas le scripting externe réservé à Resolve Studio, et n'a besoin
d'aucun interpréteur Python.

### Depuis un terminal

```sh
# 1. Resolve Studio est lancé, le projet ouvert. Le pont lit le Media Pool.
python3 -m sidecar.resolve_bridge -o manifest.json

# 2. CUTMACHINE reproduit l'arborescence et ingère les rushes.
./build/cutmachine --import-resolve \
  ./Film.cutmachine-project/project.cutmachine.json ./manifest.json
```

Cette voie-là exige **DaVinci Resolve Studio** : la version gratuite n'exécute
des scripts que depuis l'application. En échange, elle s'automatise.

Les deux producteurs écrivent le même schéma de manifeste et passent par le
même importeur — vérifié sur un projet de 408 rushes, où les deux manifestes
sont équivalents. Ni l'un ni l'autre ne modifie le projet Resolve : ils lisent
le Media Pool.

Le manifeste ne transporte **que l'identité et l'organisation** — chemin, nom,
chutier. Aucune métadonnée technique : Resolve renvoie sa cadence en flottant,
et un flottant ne devient jamais un `RationalTime`. C'est la sonde FFmpeg de
`--ingest` qui fait autorité sur la cadence et la durée, si bien qu'un rush
importé depuis Resolve est identique au même rush ingéré depuis un dossier.

Les chutiers sont créés par `AddBinOperation` et les rushes classés par
`SetMediaBinOperation` : l'import complet s'annule par `Cmd+Z`, chutier par
chutier. Réimporter le même manifeste ne duplique rien — les chutiers sont
appariés par nom et par parent, les rushes par chemin absolu résolu. Un Media
Pool qui grossit se réimporte donc en n'ajoutant que le nouveau. Attention :
un rush déjà présent est reclassé dans son chutier Resolve, donc un
réimport écrase le classement fait à la main dans CUTMACHINE.

Les entrées sans fichier — timelines, clips composés, générateurs — sont
écartées par le pont et rapportées dans son `skipped`. Côté import, tout rush
refusé est **nommé avec son motif** dans le tableau `errors`, comme pour
`--ingest` : sur un Media Pool de plusieurs centaines de rushes, un simple
compteur obligerait à comparer le chutier à la main. Un rush déjà présent dans
la médiathèque compte dans `skipped` sans figurer dans `errors` — c'est le
résultat attendu d'un réimport, pas un échec.

```json
{"ok":true,"added":405,"skipped":3,"bins_created":22,"bins_reused":0,
 "errors":[{"file":"Ian Post - Electricity.wav","reason":"no video stream"}]}
```

Deux motifs de refus reviennent souvent. Le média hors ligne : le volume doit
être monté au moment de l'import. Et le **fichier audio seul** — musique,
ambiance, son stock — que la sonde refuse faute de flux vidéo. C'est une limite
de `ProbeMediaMetadata`, partagée avec `--ingest`, pas de l'import Resolve.

Ce qui reste hors périmètre : les timelines Resolve elles-mêmes (seuls les
rushes traversent), les mots-clés, drapeaux et commentaires de clip, le
timecode de départ — `LibraryMedia` n'a pas de champ pour l'accueillir — et le
trajet retour vers Resolve, qui demande une sortie interopérable (voir
`ROADMAP.md`).

## Format de séquence

Une séquence neuve est en 1920×1080 à 25 i/s. Ce n'est presque jamais le
format des rushes, et le corriger à la main demande de savoir une chose que
les fichiers cachent : **un tournage vertical est stocké en paysage** avec un
drapeau de rotation. Lire `3840×2160` dans les métadonnées et le recopier dans
la séquence donne un montage couché.

`--propose-sequence` déduit le format des rushes du projet, sans rien modifier :

```sh
./build/cutmachine --propose-sequence ./Film.cutmachine-project/project.cutmachine.json
```

```json
{"ok":true,
 "chosen":{"width":2160,"height":3840,"frame_rate":{"num":25,"den":1},"media_count":365},
 "unanimous":false,"media_considered":405,"media_ignored":0,
 "candidates":[{"width":2160,"height":3840,"frame_rate":{"num":25,"den":1},"media_count":365},
               {"width":2160,"height":3840,"frame_rate":{"num":50,"den":1},"media_count":40}]}
```

La règle est la majorité, et elle est entièrement déterministe : à égalité de
comptes c'est la plus grande image qui tranche, puis la cadence la plus haute,
puis les champs bruts — le même projet donne toujours la même réponse, quel que
soit l'ordre de la médiathèque. Les formats minoritaires sont **rapportés**, pas
escamotés : un tournage à cadences mixtes doit se voir. Les médias sans image
exploitable (son seul, rotation qui n'est pas un angle droit) sont comptés à
part plutôt que rattachés de force.

Côté agent, l'outil MCP `conform_sequence` applique ce format en une
`UpdateSequenceOperation` réversible, et retourne ce qu'il a choisi et ce qu'il
a écarté. `preview: true` calcule sans appliquer. Une séquence déjà conforme est
signalée sans être réécrite, pour ne pas encombrer l'historique d'une opération
nulle.

C'est la division du travail du principe 7 : l'appelant nomme l'intention
(« que la séquence corresponde aux rushes »), le moteur calcule les nombres.
`update_sequence` reste disponible pour poser un format arbitraire.

## Transcription verbatim et nettoyage des hésitations

### Configurer le modèle

Le chemin du modèle Whisper est un **réglage local de la machine**, jamais une
donnée de projet : un chemin enregistré dans le projet n'existerait pas sur un
autre Mac, et le même montage doit s'ouvrir partout. Il se pose une fois, dans
`~/.config/cutmachine/.env` — le même fichier que la clé API du panneau chat :

```sh
echo 'CUTMACHINE_WHISPER_MODEL=/chemin/vers/ggml-large-v3.bin' \
  >> ~/.config/cutmachine/.env
```

La variable d'environnement l'emporte sur le fichier, donc un essai ponctuel
se fait en la préfixant à la commande, sans rien modifier.

Une fois posé, le réglage sert les trois surfaces à l'identique : `--transcribe`
sans chemin explicite, l'outil MCP `transcribe_media`, et l'application. Tant
qu'il n'est pas posé, les trois refusent de la même façon, en nommant la
variable et le fichier à éditer.

`transcribe_media` est ce qui rend le travail sur les mots atteignable par un
agent : `list_disfluencies`, `remove_words` et le montage d'interview lisent
tous un transcript, et aucun d'eux ne pouvait le faire exister. Il faut
`verbatim: true` pour pouvoir retirer les hésitations ensuite — le décodage par
défaut les supprime silencieusement, donc elles ne sont plus là pour être
coupées.

### Le reste

`--transcribe` accepte une langue explicite et un mode `--verbatim`. Le chemin
du modèle y reste facultatif : donné, il l'emporte ; omis, c'est le réglage
local qui s'applique.

Il accepte aussi **plusieurs `media-id` séparés par des virgules** (MCP :
`media_ids`), et ne charge alors le modèle qu'une fois. Le chargement mesure
~8 s ; sur les 43 rushes parlés d'un projet réel, c'était près de six minutes
passées à relire le même fichier, contre 11 min d'inférence utile. Les médias
dont le document dit qu'ils sont muets sont **sautés et signalés** plutôt que
transcrits — 29 des 71 rushes du même projet étaient des plans de coupe à
−74 dBFS, tous passés à Whisper à 11× le temps réel pour ne rien produire.
`--include-silent` (MCP : `include_silent`) force le passage.

Ce niveau vient de `--ingest`, qui mesure désormais le niveau audio moyen de
chaque média et l'enregistre dans le document (`audio_level`), au même titre
que la cadence et la durée. C'est un décodage audio, donc un coût réel :
mesuré à 0,3 s sur un rush 4K de 268 Mo. Une entrée de bibliothèque écrite
avant ce champ se relit « non mesurée » — jamais « silencieuse » ; un
ré-`--ingest` la remplit.

L'auto-détection de langue se trompe sur un rush long et majoritairement non
parlé (mesuré : gallois détecté sur une interview française). Nomme la langue.

Sans `--verbatim`, Whisper nettoie ce qu'il entend : sur une interview de
7 min 35 il n'a conservé que 3 tics sur les 20 réellement prononcés. Le mode
verbatim place devant chaque fenêtre de 30 s un prompt qui biaise le décodage
vers les hésitations, les répétitions et les faux départs — d'où 607 mots
transcrits au lieu de 390, et les « euh », « heu », « ben » et « bah »
présents dans le cache. Le drapeau fait partie de l'identité du transcript :
une transcription verbatim n'est jamais réutilisée à la place d'une
transcription standard, ni l'inverse.

Le nettoyage se fait ensuite en deux temps, et jamais par un modèle :

1. `--disfluencies` (MCP : `list_disfluencies`) liste ce qui est *prouvable* —
   les syllabes qui ne sont pas des mots français, et les mots répétés
   immédiatement — sous forme d'index de mots, avec leur texte pour relecture.
   Ce qui relève du jugement (« ce "donc" est-il un tic ? ») n'est
   volontairement pas proposé.
2. `--remove-words` (MCP : `remove_words`) applique la coupe. Il ne prend que
   des **index de mots** : c'est `ResolveWordRemoval` qui calcule les images,
   jamais l'appelant. La coupe est une seule `RemoveWordsOperation`,
   réversible, qui referme la timeline.

Un modèle peut donc proposer une sélection et la justifier, sans jamais
calculer un timecode.

`clean_disfluencies` (MCP) réunit les deux étapes en une seule intention :
CUTMACHINE détecte, calcule les images et applique **une** opération
réversible pour tout le plan. Le modèle n'énumère rien. Les répétitions ne
sont retirées que si `include_repetitions` est demandé — un mot répété peut
être une insistance voulue, une syllabe d'hésitation jamais.

### Recaler les mots sur le signal, et refermer les silences

`--speech-onset` met aussi en cache les groupes de parole séparés par au
moins 200 ms de creux. `--speech-onset-report` (MCP :
`list_speech_onsets`) publie pour chaque groupe ses bornes source, son niveau
moyen et sa crête en dBFS. Le champ `dominant_onset` désigne le premier groupe
tenu au moins six fenêtres de 20 ms et situé à moins de 9 dB du 90e centile du
plan. Une question faible hors micro reste donc visible dans `groups`, mais
n'est plus confondue avec l'entrée du sujet équipé d'un micro-cravate. Le
seuil séparant les groupes et leur plancher au-dessus du bruit sont
paramétrables dans les outils MCP (`group_gap_ms`, `group_floor_db`).

Les horodatages de Whisper sont excellents la plupart du temps et
occasionnellement faux d'une seconde. `--align-transcripts` (MCP :
`align_transcript`) compare chaque frontière de mot à l'enveloppe d'énergie
produite par `--speech-onset`, corrige celles qui tombent dans le silence, et
**refuse** les autres plutôt que de les déplacer au jugé — une frontière
ambiguë, sans front de parole à portée, ou qui demanderait un déplacement
au-delà du plafond est signalée et laissée où elle est. Sans `--write`, c'est
un rapport ; avec, la correction est écrite dans le cache de transcription que
lisent `remove_words`, `clean_disfluencies` et le montage d'interview. C'est
ce qui rend une coupe par mot fiable ; sans ça, la seule façon de vérifier une
borne était d'extraire un fragment et de le retranscrire.

`--tighten-pauses` (MCP : `tighten_pauses`) referme les silences **internes**
d'un plan sans lire un seul mot : il cherche les creux de l'enveloppe d'au
moins `min_gap` millisecondes, ramène chacun à `keep` images (réparties entre
les deux bords, pour que le raccord tombe au plus profond du silence), et
referme en ripple. Une seule `RemoveWordsOperation`, donc un seul `undo`, et
la paire A/V liée est emportée avec. L'air de tête et de queue n'est pas
touché : c'est un rognage, la question de `--speech-onset`, et il est publié
dans le rapport plutôt que coupé en douce.

### Adresser la timeline par le rush

Une décision de montage se prend dans le rush (« couper juste après le nom »,
« cette prise commence à 1412 »), mais toutes les opérations prennent une
position **timeline**. La conversion est une soustraction et une addition —
c'est-à-dire exactement ce qu'on rate une fois sur dix.

`--locate-source-frame` (MCP : `locate_source_frame`) répond « où joue
l'image N de ce rush » : quel plan, quelle piste, quelle position. Une même
image peut être sur la timeline plusieurs fois — l'image et son son détaché,
ou un rush utilisé deux fois — donc toutes les correspondances sont rendues,
jamais une choisie.

Et les outils de coupe et de rognage acceptent la même adresse :
`split_clip`, `trim_clip` et `ripple_trim` prennent `source_frame` **à la
place** de `timeline_position` ou de `delta` — jamais les deux, refus explicite
sinon. Sur un rognage, `Head` veut dire « le plan démarre sur cette image » et
`Tail` « c'est la dernière image jouée » : la borne est inclusive, parce que
c'est ce qu'un numéro d'image veut dire pour un monteur, et le +1 appartient au
moteur.

Une paire A/V liée est coupée des deux côtés dans la **même** opération.
`RemoveWordsOperation` porte un champ `linked_clip_ids` : chaque clip nommé
perd exactement les mêmes images source et referme sa propre piste. Sans ça,
nettoyer le son d'une interview raccourcirait la bande et laisserait l'image,
et un seul `undo` ne suffirait plus à revenir en arrière. Les outils
`clean_disfluencies` et `remove_words` résolvent eux-mêmes les membres du
groupe de liaison qui couvrent réellement la coupe ; l'opération, elle, reste
stricte — un clip nommé explicitement qui ne contient pas les plages est
refusé (`SourceOutOfBounds`), jamais coupé à moitié.

## Contrôle qualité des plans

Un plan flou ou bougé ne se juge pas, il se mesure. `--shot-quality` analyse
un média image par image et écrit un cache dans
`.cutmachine/shotquality/<media-id>.json` ; `--shot-quality-report` (MCP :
`list_shot_quality`) note chaque clip de la timeline active.

Deux mesures, aucune inférence :

- **Netteté** — variance du laplacien sur le plan de luminance, notée par
  rapport à la médiane du média lui-même. Le relatif est volontaire : la
  variance du laplacien n'a pas de sens absolu d'un contenu à l'autre, alors
  qu'à l'intérieur d'un même rush, même caméra et même optique, la comparaison
  tient. Le prix de ce choix est énoncé plutôt que caché — un média flou de
  bout en bout note tous ses plans « Sharp », d'où la publication des médianes
  absolues à côté de la note.
- **Bougé** — différence absolue moyenne de luminance entre deux échantillons,
  notée en absolu : la grandeur veut dire la même chose sur n'importe quel
  rush.

Le grade porte sur le **décile le plus mauvais**, pas sur la médiane : un plan
qui perd le point à mi-course reste sain en médiane et reste inutilisable.

Mesuré sur une prise réelle (interview à la main, Sony FX30, 6,72 s, quatre
échantillons par seconde, 1,7 s d'analyse) : le corps du plan tient entre
4 055 et 4 481 de netteté, et les cinq derniers échantillons s'effondrent à
3 499, 3 691, 2 616, 1 461 puis 336 pendant que le bougé passe de 5 000–36 000
à 67 624. Vérifié à l'image : la caméra quitte le sujet en fin de prise, le
cadre part sur un mur et la dernière image n'est plus qu'une bouillie. C'est
exactement ce que la mesure doit attraper. Un clip posé sur cette fin est noté
`Blurry` / `Shaky`, celui posé sur le corps `Sharp` / `Steady`.

Les seuils de bougé sont calés sur cette prise unique. C'est un point
d'ancrage, pas un étalonnage : chaque note est publiée avec le nombre qui l'a
produite, précisément pour qu'ils se déplacent sur preuve.

Un média jamais analysé apparaît sous `unanalyzed`, jamais parmi les plans
propres. L'absence de mesure n'est pas un feu vert : `analyze_shot_quality`
(MCP) lance l'analyse depuis l'agent, sans passer par la ligne de commande.

La note tient compte du recouvrement. Un plan mou entièrement caché par une
illustration posée au-dessus n'est pas un défaut à corriger, puisque personne
ne le voit — chaque entrée porte donc sa note (`clean`, ce qui est mesuré), sa
durée encore visible (`visible`, ce que dit le document) et `needs_attention`,
qui est la conjonction des deux. Sans ça, la vue réclamait de recouper des
plans invisibles.

### Découper un rush en plans

Noter ce qui est déjà monté est une chose ; savoir ce que les rushes
contiennent en est une autre. Un fichier sorti de la caméra en une seule prise
peut tenir six prises, et tant qu'elles ne sont pas nommées il n'y a rien à
sélectionner, à noter ou à décrire d'autre que le fichier entier.

`--shot-quality-report` publie donc, à côté de la liste des clips, un tableau
`sources` : pour chaque média analysé, les plans détectés à l'intérieur, avec
leur début, leur fin et l'échantillon le plus net — l'image à extraire quand
une seule doit représenter le plan. Les temps sont dans le domaine temporel de
la source, donc utilisables tels quels comme `source_in`.

Reste une mesure, sans modèle : une coupe est une discontinuité de l'image.
Une troisième grandeur par échantillon s'ajoute aux deux précédentes, la
**distance d'histogramme de luma** — la fraction des pixels ayant changé de
casier de luminance. Le cache passe en version 3 ; les anciens sont refusés et
réanalysés plutôt que complétés par une valeur jamais mesurée.

L'idée de départ était qu'un panoramique déplace l'image sans changer sa
distribution, contrairement à une coupe. **Mesurée, cette idée est fausse.**
Sur la prise réelle ci-dessus, l'instant où la caméra quitte le sujet atteint
70 433 de bougé et 292 944 de distance d'histogramme ; une vraie coupe montée
dans la même matière mesure 48 489 et 30 900. Le mouvement de caméra note
*plus haut que la coupe sur les deux grandeurs*. Aucun couple de seuils
absolus ne les sépare.

Ce qui les sépare n'est pas l'amplitude du changement mais sa forme. Une
caméra a de l'inertie : un mouvement s'étale sur plusieurs échantillons. Une
coupe est instantanée : elle tombe sur un seul. Rapportés à la médiane de leur
voisinage, les mêmes événements se rangent — les mouvements de caméra montent
à 153 %, 194 %, 201 %, 202 %, 210 %, 232 % et 299 %, les deux coupes réelles à
408 % et 3 956 %. Le seuil est à 350 %, à égale distance des deux, et 162 des
combinaisons balayées autour de ce point donnent le même résultat sur les
quatre fixtures : la règle est sur un plateau, pas sur une arête.

Les limites sont connues et énoncées : un fondu enchaîné n'est pas trouvé,
puisqu'il n'a aucun échantillon isolé sur lequel piquer ; une coupe entre deux
plans réellement semblables peut passer sous les planchers absolus ; et
l'étalonnage repose sur quatre fixtures dont une seule est de la vraie matière.
Élargir ce corpus est ce qui ferait cesser d'être provisoires les nombres
ci-dessus.

## Regarder un plan

Mesurer ne suffit pas, et le manque s'est constaté sur un vrai montage : un
plan noté `Sharp` / `Steady` — donc parfaitement propre — était inutilisable
en coupe parce qu'il montrait quelqu'un **en train de parler**. Le spectateur
voyait une bouche articuler autre chose que ce qu'il entendait. Aucun seuil
sur la variance du laplacien n'attrape ça ; un monteur l'attrape d'un coup
d'œil.

`read_frame` (MCP) renvoie donc l'image elle-même, en JPEG, à côté de sa
réponse texte. Soit `clip_id` avec une `position` (`Start`, `Middle`, `End`
— jamais un timecode, le moteur résout l'image), soit `media_id` avec un
`source_in` exact pour inspecter un rush qui n'est pas encore monté.

Le partage du travail reste le même que partout ailleurs : ce qui est
**prouvable** est décidé par le code ([`ShotQuality.h`](src/ShotQuality.h)),
ce qui demande un regard reçoit une image
([`FrameCapture.h`](src/FrameCapture.h)). Rien dans `FrameCapture` ne décide,
il rend.

Côté transport, l'image traverse MCP comme un bloc `image` à côté du bloc
`text`, et le chat intégré la fait suivre au modèle dans le `tool_result`.
Elle n'est transmise qu'aux fournisseurs dont l'API accepte une image dans un
résultat d'outil ; ailleurs le texte passe et l'image est écartée, plutôt que
remplacée par une description que le modèle prendrait pour une observation.

## Export vidéo final

Le menu **Fichier → Exporter la vidéo finale…** ouvre le module de rendu de
CUTMACHINE. Le preset principal produit un MP4 ou MOV HEVC/H.265
Main10 avec audio AAC stéréo 48 kHz.
Le dialogue privilégie quatre presets stables : **Haute qualité**, **Master
x265**, **Web 1080p** et **Livraison UHD**. Des menus déroulants permettent de
surcharger ponctuellement le conteneur, la résolution, la cadence ou le moteur
d’encodage ; aucun débit ou timebase n’a besoin d’être saisi manuellement.
**VideoToolbox** privilégie la vitesse et **x265 logiciel** la qualité constante.
Le rendu est effectué hors du thread de l’interface, expose sa progression et
peut être annulé.

La commande headless équivalente est :

```sh
./build/cutmachine --export Projet.cutmachine-project/project.cutmachine.json sortie.mp4
./build/cutmachine --export Projet.cutmachine-project/project.cutmachine.json sortie.mp4 --software
./build/cutmachine --export Projet.cutmachine-project/project.cutmachine.json sortie.mp4 --overwrite
```

L’export compose les pistes vidéo dans leur ordre, conserve les zones noires,
respecte les rotations et le ratio des sources, puis mixe les clips audio sans
normalisation automatique. Il écrit d’abord un fichier temporaire voisin et ne
remplace la destination qu’après un encodage réussi. Pour les projets Sony,
l’export construit avec OpenColorIO une LUT 3D 65³ issue de la configuration
ACES Studio intégrée : S-Log3/S-Gamut3.Cine → ACEScg/ACEScct → Rec.2020 HLG.
Le flux HEVC Main10 est marqué explicitement BT.2020 non constant,
ARIB STD-B67 et plage légale.

## Séquence

La timeline possède un objet `sequence` indépendant des médias sources, avec
un ULID, un nom, une largeur, une hauteur et une cadence rationnelle. Cet objet
possède directement ses `tracks` et ses `markers` : aucune timeline ne flotte
au niveau du projet sans séquence. Comme dans Premiere Pro ou Resolve, ce
canevas pilote le moniteur et sert de base aux
presets d’export ; une vidéo portrait ne transforme donc pas implicitement une
séquence horizontale. **Timeline → Réglages de séquence…** propose les formats
HD, UHD, vertical et carré ainsi que les cadences usuelles. Les documents plus
anciens sans bloc `sequence` ou antérieurs à la version 3 sont refusés.

L’application charge désormais un `Project` complet autour de ce document
actif. Toutes ses timelines apparaissent dans le chutier ; un double-clic en
active une et `Cmd+Option+N` en crée une nouvelle. Une autosave du projet
complet protège chaque commit atomique et peut être récupérée au lancement.

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

Le schéma courant du document est la version 3. `library`, `bins` et `sources`
sont des objets projet ; `tracks` et `markers` appartiennent à `sequence`.
Les versions 1 et 2 sont refusées. Bibliothèque et source partagent l'ULID du
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
et sa plage. OpenColorIO 2.5.2 applique ensuite les transformations de la
configuration intégrée et figée
`studio-config-v2.2.0_aces-v1.3_ocio-v2.4` : aucune variable `$OCIO` ni aucun
fichier externe n'intervient dans le rendu. Le player échantillonne les
processeurs OCIO en deux LUT 3D 65³ mises en cache par Metal, avant et après le
grading ACEScct ; la composition des pistes reste en AP1 linéaire dans une
texture flottante 16 bits. L'export utilise le même processeur OCIO sous forme
de LUT `.cube`. Les moniteurs de montage dérivent une vue locale Rec.709/SDR du
réglage de livraison afin que macOS ne présente pas le master HLG comme une
couche EDR au milieu de l'interface. Cette préférence d'affichage ne modifie ni
le document ni le rendu exporté.

Les réglages passent par l'opération réversible `SetColorManagement`, disponible
également via `--apply-op` et l'outil MCP `set_color_management`. Une combinaison
gamut/courbe d'entrée absente de la configuration intégrée est refusée avant le
rendu plutôt que remplacée silencieusement.

`--apply-op`
réutilise le format canonique de `SerializeOperation`, remplace le document de
façon transactionnelle et conserve le journal dans le fichier compagnon
`<document>.editlog.json`. En cas de refus, ni le document ni ce journal ne sont
modifiés.

`--apply-project-op` applique de la même façon les opérations multi-timeline,
de métadonnées de chutier et de relink. Son historique distinct est conservé
dans `<document>.project-editlog.json` et les commandes `--undo-project-op` et
`--redo-project-op` le parcourent sans initialiser l’interface graphique.

Les objets passés à `--apply-project-op` doivent reprendre exactement l’ordre
et tous les champs produits par `SerializeProjectOperation`. `exact_project_hex`
vaut `null` pour une nouvelle intention ; il est rempli par le journal pour un
rejeu exact. Exemples canoniques complets (les identifiants sont des ULID) :

```json
{"type":"AddProjectTimeline","name":"Vertical","width":1080,"height":1920,"frame_rate":{"num":25,"den":1},"timeline_id":"","video_track_id":"","audio_track_id":"","exact_project_hex":null}
{"type":"CreateProjectTimelineFromSegments","name":"Extrait","width":1920,"height":1080,"frame_rate":{"num":25,"den":1},"segments":[{"source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":50,"rate":25},"video_clip_id":"","audio_clip_id":"","link_group_id":""}],"timeline_id":"","video_track_id":"","audio_track_id":"","make_active":true,"exact_project_hex":null}
{"type":"RemoveProjectTimeline","timeline_id":"01K00000000000000000000002","exact_project_hex":null}
{"type":"SetProjectBinMetadata","item_id":"01K00000000000000000000001","metadata":{"description":"Interview retenue","rating":5,"tags":["interview"],"insert_order":0,"display_name":"Interview principale"},"exact_project_hex":null}
{"type":"SetProjectTimelineBin","timeline_id":"01K00000000000000000000002","bin_id":"01K00000000000000000000003","exact_project_hex":null}
{"type":"RenameProjectItem","item_id":"01K00000000000000000000002","name":"Montage vertical","exact_project_hex":null}
{"type":"SetActiveProjectTimeline","timeline_id":"01K00000000000000000000002","exact_project_hex":null}
{"type":"RelinkProjectMedia","replacements":[{"media_id":"01K00000000000000000000001","replacement":{"id":"01K00000000000000000000001","path":"rushes/interview.mov","filename":"interview.mov","codec":"h264","width":1920,"height":1080,"pixel_format":"yuv420p","color_range":"tv","color_space":"bt709","color_transfer":"bt709","color_primaries":"bt709","rotation_degrees":0,"rate":{"num":25,"den":1},"duration":{"value":250,"rate":25},"orientation":"landscape","has_audio":1,"audio_rate":48000,"audio_channels":2,"bin_id":"","proxy_path":"","metadata_complete":1},"stored_path":"rushes/interview.mov"}],"exact_project_hex":null}
```

## Agent intégré et clé utilisateur

Le panneau **Agent** contacte directement soit l’API Messages d’Anthropic, soit
l’API locale native d’Ollama. Le bouton **Configurer le moteur
IA…**, placé sous la conversation, permet de choisir le fournisseur, le modèle,
l’URL de base et la clé API. Le même dialogue est disponible dans **CUTMACHINE
→ Moteur IA…**.

Pour utiliser le modèle Qwen Coder déjà installé sur la machine, choisissez
**Ollama local**. Le dialogue préremplit le modèle `qwen2.5-coder:7b` et l’URL
`http://localhost:11434`. Ollama local ne
nécessite aucune clé API. Les appels d’outils restent disponibles : le modèle
peut donc observer et modifier le montage via les mêmes opérations réversibles
que le moteur Anthropic.

Le transport Ollama utilise `/api/chat` et demande un contexte de 32K tokens.
Avant de renvoyer `describe` au modèle local, l’app conserve la timeline, les
sources utilisées, les aliases de médiathèque, les chutiers et les marqueurs,
mais retire les métadonnées média redondantes. Pour les modèles anciens qui
écrivent un appel sous la forme JSON `{name, arguments}` au lieu d’un vrai
`tool_calls`, ce format n’est exécuté que si l’objet est strict et désigne un
outil réellement publié par CUTMACHINE.

Pour monter une interview, `get_timeline_transcript` renvoie des spans
sélectionnables et `create_interview_short` **n'accepte que leurs `span_id`**.
Le modèle ne recopie aucun temps : c'est le moteur qui résout chaque
identifiant en position exacte, ce qui rend une coupe au milieu d'un mot
inexprimable au lieu de simplement déconseillée. Quand une idée dépasse un
span — ils sont découpés pour la lisibilité d'un sous-titre, environ
42 caractères, ce qui n'est pas une unité de montage — `span_id` et
`end_span_id` désignent un intervalle contigu que le moteur fusionne en une
seule plage. Un intervalle qui enjambe un silence ou change de source est
refusé, jamais recousu en silence.

Pour les raccourcissements A/V courants, l’outil MCP
`shorten_linked_clip` reçoit seulement un clip, un bord et une quantité en
images ou secondes. CUTMACHINE résout lui-même tous les membres liés, le signe
du trim et le `RationalTime` exact, puis applique une seule opération atomique.
Avec `preview=true`, la même opération est validée sur une copie via `EditLog`
et renvoyée sans modifier le projet.

Le modèle et l’URL sont des préférences locales `NSUserDefaults`. La clé est
conservée séparément dans le Trousseau macOS : elle n’est écrite ni dans le
document projet, ni dans son journal, ni dans les préférences. Les variables
`ANTHROPIC_API_KEY`, `CUTMACHINE_ANTHROPIC_MODEL` et
`CUTMACHINE_ANTHROPIC_URL` restent utilisables comme valeurs de repli.

## Sidecar conversationnel

Le pilote Python utilise uniquement la bibliothèque standard. Le backend est
sélectionné sans modifier le REPL :

```sh
# Ollama local
export CUTMACHINE_BACKEND=ollama
export CUTMACHINE_MODEL=qwen3:8b
python3 -m sidecar.repl ./Film.cutmachine-project/project.cutmachine.json

# API Anthropic
export CUTMACHINE_BACKEND=anthropic
export CUTMACHINE_MODEL=claude-sonnet-4-5
export ANTHROPIC_API_KEY=...
python3 -m sidecar.repl ./Film.cutmachine-project/project.cutmachine.json
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

## Licence

CUTMACHINE est distribué sous la GNU Affero General Public License, version 3
ou toute version ultérieure (`AGPL-3.0-or-later`). Consultez [LICENSE](LICENSE)
pour les conditions complètes.

Cette licence s'applique à compter de la version qui contient ce changement.
Les versions antérieures publiées sous Apache License 2.0 restent disponibles
selon les droits déjà accordés par cette licence.
