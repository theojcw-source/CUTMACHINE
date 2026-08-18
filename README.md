# CUTMACHINE

CUTMACHINE charge un package projet v2, ouvre chaque source média par
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
dans Applications, ou avec :

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
./build/cutmachine --describe ./Film.cutmachine-project/project.cutmachine.json
./build/cutmachine --apply-op ./Film.cutmachine-project/project.cutmachine.json \
  '{"type":"TrimClip","clip_id":"01K00000000000000000000003","edge":"Tail","delta":{"value":-1,"rate":25},"exact_clip":null}'
./build/cutmachine --apply-project-op ./Film.cutmachine-project/project.cutmachine.json \
  '{"type":"AddProjectTimeline","name":"Vertical","width":1080,"height":1920,"frame_rate":{"num":25,"den":1},"timeline_id":"","video_track_id":"","audio_track_id":"","exact_project_hex":null}'
./build/cutmachine --undo-project-op ./Film.cutmachine-project/project.cutmachine.json
./build/cutmachine --redo-project-op ./Film.cutmachine-project/project.cutmachine.json
./build/cutmachine --ingest ./Film.cutmachine-project/project.cutmachine.json ./rushes --recursive
./build/cutmachine --export ./Film.cutmachine-project/project.cutmachine.json ./film.mp4
```

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
l’export construit une LUT 3D 65³ à partir des mêmes fonctions de transfert et
matrices que le shader Metal : S-Log3/S-Gamut3.Cine → AP1 → Rec.2020 HLG. Le
flux HEVC Main10 est marqué explicitement BT.2020 non constant, ARIB STD-B67 et
plage légale.

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
et sa plage, puis l'IDT l'amène en ACES AP1. Le point de grading est encodé en
ACEScct ; la composition des pistes se fait ensuite en AP1 linéaire dans
une texture flottante 16 bits. L'export conserve la transformation de sortie du
projet, notamment Rec.2020/HLG. Les moniteurs de montage en dérivent une vue
locale Rec.709/SDR afin que macOS ne présente pas le master HLG comme une couche
EDR au milieu de l'interface. Cette préférence d'affichage ne modifie ni le
document ni le rendu exporté.

`--apply-op`
réutilise le format canonique de `SerializeOperation`, remplace le document de
façon transactionnelle et conserve le journal dans le fichier compagnon
`<document>.editlog.json`. En cas de refus, ni le document ni ce journal ne sont
modifiés.

`--apply-project-op` applique de la même façon les opérations multi-timeline,
de métadonnées de chutier et de relink. Son historique distinct est conservé
dans `<document>.project-editlog.json` et les commandes `--undo-project-op` et
`--redo-project-op` le parcourent sans initialiser l’interface graphique.

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
