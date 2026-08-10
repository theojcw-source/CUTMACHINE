# CUTMACHINE

CUTMACHINE charge un document JSON de timeline, ouvre chaque source média par
son ULID, puis résout le scrub en clés de cache `(source_id, source_frame)`.
Les intervalles des clips sont semi-ouverts : `[timeline_in, timeline_in +
duration)`. Un trou ne déclenche aucun décodage et est rendu en noir.

## Build et tests

Prérequis : macOS, CMake 3.24+, pkg-config et FFmpeg (`libavformat`,
`libavcodec`, `libavutil`).

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
- `sources[].rate` est interprété comme une cadence rationnelle, tandis que le
  `rate` d'un `RationalTime` est un timebase entier. Ainsi, une frame à
  `30000/1001` dure `1001` ticks dans un timebase `30000`; cette convention est
  nécessaire car `RationalTime::rate` ne peut lui-même contenir un quotient.
- Le slider transporte des ticks entiers dans le timebase commun exact de la
  timeline (PPCM des rates rencontrés). AppKit impose une valeur `double` à
  `NSSlider`, mais cette valeur
  n'est jamais utilisée dans un calcul temporel : elle est immédiatement lue
  comme entier puis enveloppée dans un `RationalTime`.
- Le renderer reste volontairement limité aux deux premières pistes vidéo
  classées par leur champ `index`, conformément au périmètre de ce jalon.

## API d'édition

`Operations.h` expose uniquement `InsertClipOperation`, `RemoveClipOperation`
et `TrimClipOperation`. `EditLog::Apply`, `Undo` et `Redo` renvoient un
`EditError` nommé et garantissent l'atomicité du document.

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
exactes et déduit l'orientation après application de la display matrix. Les
fichiers non vidéo ou corrompus sont rapportés dans `errors` sans faire échouer
le lot ; l'identité idempotente est le chemin absolu résolu.

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
