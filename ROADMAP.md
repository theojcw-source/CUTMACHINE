# Roadmap : combler l'écart avec un éditeur concurrent agent-natif

Ce document découpe en tickets exécutables le travail identifié en comparant
CUTMACHINE à un éditeur concurrent agent-natif récemment sorti (GPLv3, macOS
26 Tahoe/Apple Silicon uniquement, éditeur Swift-natif avec serveur MCP
intégré et génération IA payante). Le constat de départ : le
moteur pur de CUTMACHINE (`Operations.h`) est déjà proche en richesse de leur
surface d'édition ; l'écart réel est dans la surface exposée à l'agent, le
grading créatif, quelques sous-systèmes de perception (transcription, beats),
et l'interface.

Voir `PHILOSOPHY.md` (amendé) pour le changement de doctrine qui autorise ce
chantier : la parité fonctionnelle et l'investissement UI ne sont plus des
non-buts, tant qu'ils restent justifiés et respectent les principes
structurels (1 à 5).

## Garde-fous transverses — s'appliquent à tous les tickets

Chaque agent qui prend un ticket doit lire `PHILOSOPHY.md` et `AUDIT.md`
avant de coder, et respecter :

1. **Le document reste la vérité (principe 1).** Tout nouvel état de projet
   (grade, caption, groupe multicam) est un champ sérialisé du document JSON
   canonique, jamais un état caché. Toute préférence d'interface (thème,
   disposition) reste locale à l'app, jamais dans le document.
2. **Toute mutation passe par une opération (principe 2).** Pas de mutation
   directe d'un `Document`/`DocumentClip`. Chaque nouvelle capacité s'exprime
   comme une structure dans `Operations.h`, appliquée via `EditLog::Apply`,
   réversible.
3. **Aucune surface n'est privilégiée (principe 3).** Une capacité existe
   d'abord dans le moteur (`src/`), puis est exposée au CLI (`--apply-op`),
   au MCP, et à l'UI. Jamais l'inverse.
4. **Le temps est exact (principe 4).** `RationalTime`, jamais de flottant
   sur une position/durée. Toute nouvelle conversion suit le même refus des
   arrondis silencieux que `RationalTime::rescale`.
5. **Déterminisme testé (principe 6).** Toute nouvelle opération a un test
   round-trip (apply → undo → comparaison octet-à-octet) et un test de
   sérialisation canonique. `ctest --test-dir build` doit rester vert.
6. **Pas de service central (non-but conservé).** Toute intégration de
   modèle distant (LLM, génération, transcription cloud) utilise la clé de
   l'utilisateur. Aucun compte, aucun crédit, aucune facturation CUTMACHINE.
7. **Référence au concurrent étudié : conceptuelle uniquement.** Leur code (GPLv3)
   sert à comprendre un problème ou une architecture, jamais à être copié,
   transposé ligne à ligne, ou traduit mécaniquement de Swift vers C++.
   Écrire l'implémentation à partir de la compréhension du besoin, pas en
   regardant leur code pendant l'écriture. En cas de doute sur cette limite,
   s'arrêter et demander plutôt que de trancher seul.

## Phase 0 — Fondations de schéma (séquentiel, non parallélisable)

Touche `Document.h`/`Document.cc`/`ProjectStorage.cc`, donc fait en premier,
par un seul agent/session, avant que quoi que ce soit d'autre ne branche
dessus.

- **F0.1 — Schéma `Effect` sur `DocumentClip`.** Pile d'effets (`type`,
  `params`) sur un clip vidéo/image, `color.*` en premier (exposure,
  contrast, saturation, vibrance, temperature, tint, highlights, shadows).
  Bump de version de schéma document (v3 → v4), migration refusée comme les
  v1/v2 actuelles (pas de migration silencieuse).
- **F0.2 — Schéma caption/texte.** Groupe de clips caption partageant un
  `caption_group_id`, un style, un texte par clip.
- **F0.3 — Schéma groupe multicam.** `MulticamGroup` avec membres et angle
  actif, réutilise `sync_reference_delta`/`sync_anchor_clip_id` déjà présents
  pour l'alignement A/V.
- **F0.4 — Tests de déterminisme mis à jour** pour les trois nouveaux champs
  (round-trip sérialisation, undo byte-identique).

## Phase 1 — Moteur & agent (parallélisable après Phase 0)

Chaque ticket = un agent, un worktree, une branche. Indépendants entre eux
une fois F0 mergé.

- **F1.1 — Serveur MCP natif.** HTTP + JSON-RPC dans le binaire `cutmachine`
  (remplace à terme le sidecar Python pour l'exécution, le sidecar peut
  rester comme client MCP parmi d'autres). Adapte `--apply-op`/`EditLog`
  existant, pas de nouvelle logique métier.
- **F1.2 — Surface d'opérations élargie côté agent.** `ToolName`-équivalent
  C++ + schémas JSON pour les opérations déjà présentes dans `Operations.h`
  (insert/remove/trim/split/move/ripple/roll/slip/transitions/markers/bins),
  aujourd'hui inaccessibles au sidecar autrement qu'en `TrimClip`.
  Inclut le résolveur d'ID par préfixe (ambiguïté → erreur explicite, jamais
  un choix arbitraire).
- **F1.3 — Moteur de grading couleur créatif.** `EffectRegistry`-équivalent :
  knobs de F0.1 → filtres/kernels Metal. Commencer par exposure/contrast/
  saturation/temperature/tint (équivalents `CIColorControls`/
  `CITemperatureAndTint` faisables nativement), courbes et roues ensuite.
- **F1.4 — Transcription + coupe par mot.** Intégration whisper.cpp (clé/API
  utilisateur si cloud, modèle local sinon — respecte le garde-fou 6),
  mapping mot→frame, opération de suppression de mots/silences avec padding
  configurable.
- **F1.5 — Multicam.** Opérations de création de groupe et de changement
  d'angle actif sur F0.3.
- **F1.6 — Détection de beats.** Détection d'onsets par DSP (pas de modèle
  ML dans un premier temps — spectral flux suffit pour caler des coupes),
  exposée en lecture seule à l'agent.

## Phase 2 — UI (dépend de Phase 1, séquencer par sous-système)

- **F2.1 — Design system minimal.** Composants réutilisables AppKit/Metal,
  pas de thème persistant dans le document (garde-fou 1), préférences
  locales seulement.
- **F2.2 — Inspector.** Propriétés clip + panneau de grading (roues, courbes)
  branché sur F1.3.
- **F2.3 — Media panel.** Médiathèque, onglets media/audio/captions, branché
  sur l'ingest existant + F0.2.
- **F2.4 — Panel agent/chat.** UI de chat intégrée, appelle le même
  dispatcher que F1.1/F1.2 (aucune logique dupliquée entre chat et MCP).
- **F2.5 — Contrôles de lecture étoffés.** Scrub bar, transport, timecode,
  au-dessus de `Renderer.mm` existant.

## Explicitement hors scope

- Marketplace de génération IA avec crédits/facturation — contredit le
  garde-fou "pas de service central". Une intégration BYOK directe (clé
  utilisateur, appel direct à un fournisseur) resterait envisageable comme
  ticket séparé, hors de cette roadmap.
- Recherche sémantique média (embeddings) — utile mais pas priorisé, pas de
  ticket dans cette passe.
- Localisation multi-langue, onboarding, télémétrie/analytics — non
  pertinents pour la thèse du projet (voir non-buts).

## Statut

| Ticket | Statut |
|---|---|
| F0.1–F0.4 | fait |
| F1.1–F1.2 | fait (serveur MCP natif ; 55 tools au 28/08/2026) |
| F1.3 | fait (grading couleur, rendu Metal non vérifiable hors macOS) |
| F1.4 | fait (transcription whisper.cpp + coupe par mot) |
| F1.5 | fait (multicam) |
| F1.6 | fait (détection de beats, DSP spectral flux) |
| F2.1 | fait (design system minimal, AppKit non vérifiable hors macOS) |
| F2.2 | fait (propriétés clip + 8 sliders de grading F1.3, AppKit non vérifiable hors macOS ; roues/courbes non implémentées côté moteur, donc hors scope ici — voir ColorEffects.h) |
| F2.3 | fait (panneau média Média/Audio/Légendes, AppKit non vérifiable hors macOS) |
| F2.4 | fait (chat branché sur le même dispatcher McpTools que le serveur MCP, BYOK) |
| F2.5 | fait (transport : lecture/pause, scrub bar, timecode) |

## Suite d'audit — QC-2026-08

Chantiers issus de l'audit externe du 28 août 2026, sur l'axe « interview
dynamique, sans plan flou ni bougé, sans hésitations ».

| Ticket | Statut |
|---|---|
| Q1 — Module `ShotQuality` : netteté (variance du laplacien) et bougé mesurés par FFmpeg, cache `.cutmachine/shotquality/`, cœur pur testé sans FFmpeg | fait |
| Q2 — `--shot-quality`, `--shot-quality-report`, outil MCP `list_shot_quality` | fait |
| Q3 — `create_interview_short` n'accepte plus que des `span_id`, avec fusion d'un intervalle contigu (`end_span_id`) résolue par le moteur | fait |
| Q4 — `clean_disfluencies` : une intention, une opération réversible, aucune énumération par le modèle | fait |
| Q4b — `RemoveWordsOperation.linked_clip_ids` : une paire A/V liée est coupée des deux côtés dans une seule opération réversible (avant, nettoyer le son laissait l'image et désynchronisait) | fait |
| Q5 — `MainThreadBackend` transmet enfin `ReadSourceTranscript` : le nettoyage par mot était injoignable depuis le panneau chat alors qu'il marchait en MCP | fait |
| Q6 — Segmentation d'un rush en plans : distance d'histogramme de luma par échantillon (cache v3), `DetectSourceShots`, plans exposés par source dans `--shot-quality-report` et `list_shot_quality` | fait |

## Suite d'audit — parité des surfaces (QC-2026-08b)

Déclenché par un constat : sur le montage « Le dixième titan », l'essentiel du
travail utile était hors du catalogue d'outils, donc hors de portée de l'agent
intégré. Ces tickets ramènent ces capacités dans le moteur.

| Ticket | Statut |
|---|---|
| P1 — `read_frame` : l'agent peut **regarder** une image (JPEG via MCP et jusque dans le `tool_result` Anthropic). Corrige la classe d'erreur qu'aucune mesure n'attrape — un plan net et stable qui montre quelqu'un en train de parler | fait |
| P2 — `analyze_shot_quality` : l'agent peut produire l'analyse qu'il ne pouvait que lire | fait |
| P3 — `list_shot_quality` tient compte du recouvrement (`visible`, `fully_covered`, `needs_attention`) au lieu de réclamer la correction de plans invisibles | fait |
| P4 — `SetActiveProjectTimelineOperation` + outil `set_active_timeline` : changer de timeline n'était possible qu'à la souris | fait |
| P5 — Notation du bougé rendue relative à la source ; les seuils absolus condamnaient 67 % d'une interview à la main | fait |
| P6 — Contiguïté des spans adossée à `kSubtitleCueMaximumGap` : les respirations inter-mots ne sont plus des refus | fait |
| P7 — `transcribe_media` : l'agent peut **produire** le transcript dont dépendait tout son travail sur les mots. Le chemin du modèle Whisper devient un réglage local (`LocalEnv.h`), donc résolu identiquement par la CLI, le serveur MCP et l'app | fait |
| P8 — `conform_sequence` : le format de séquence se **déduit** des rushes (`SequenceFormat.h`) au lieu d'être deviné. Un tournage vertical est stocké en paysage avec un drapeau de rotation ; choisir la taille stockée donnait une séquence couchée | fait |

Restent ouverts sur cet axe :

- **Aucun verrou partagé entre `--mcp-serve` et l'app.** Seule l'app prend `ProjectSessionLock` ; deux surfaces sur le même projet divergent en silence.
- **Vérification du rendu** (comparer l'export à ce que le document décrit) reste un travail manuel.

Restent ouverts et non traités ici, par ordre de gravité (voir l'audit) :

- **`Export.cc` ne lit jamais `clip.effects`.** L'étalonnage est visible au
  moniteur et absent du fichier livré. C'est un défaut de correction, et le
  premier ticket à prendre.
- **`Test copilot/` n'est pas branché** : trois suites de tests et le job CI
  `ctest.yml` sont écrits et non référencés dans `CMakeLists.txt`.
- **Aucune sortie interopérable** (OTIO/FCPXML/EDL). L'entrée existe désormais
  pour DaVinci Resolve Studio (`sidecar/resolve_bridge.py` + `--import-resolve`,
  chutiers et rushes seulement) ; rien ne repart de CUTMACHINE vers un autre
  logiciel de montage.
- **Pas de marqueur de tour dans `EditLog`** : impossible d'annuler une
  intervention d'agent en un geste.
- **La langue n'entre pas dans l'identité du cache de transcription.**

## Suite d'audit — coût de production (QC-2026-09)

Déclenché par le montage des quatre interviews d'`ADS213_ITW_Findetudefevr26`
(Zoé, Anis, Raphaël, Soraya) en une seule session. Les quatre films sont
sortis conformes au style de la maison — 60 à 78 s, 19 à 25 plans visibles par
minute, 46 à 49 % de plans de coupe — mais le coût de production a été dominé
par des allers-retours que le moteur peut supprimer. Les chiffres ci-dessous
sont **mesurés sur ce projet**, pas estimés.

Coûts unitaires relevés :

| opération | coût mesuré |
|---|---|
| `--transcribe` | ~8 s de chargement de modèle + 11× temps réel + ~3 s de démux d'un rush 4K |
| `--speech-onset` | 0,19 s — réutilise le cache audio, donc gratuit |
| `--shot-quality` | 11 s par rush |

La transcription des 43 rushes parlés a coûté ~11 min de machine : ce n'est pas
le poste dominant. Le poste dominant a été **une quarantaine de sondes de
bornes** — extraire un fragment de rush, l'ingérer dans un projet jetable, le
transcrire, lire le résultat — plus quatre reconstructions complètes de piste
de coupe et sept validations du son monté. Toutes les sondes ont la même
cause : **les horodatages de mots ne sont pas fiables**, donc aucune décision
de coupe ne peut s'appuyer sur le transcript seul. Tout le reste en découle,
y compris l'inutilisabilité de `remove_words` déjà consignée.

| Ticket | Statut |
|---|---|
| A1 — **`align_transcript` : recaler les mots sur le signal.** Fonction pure (liste de mots, enveloppe d'énergie) → mots re-datés, testable sans FFmpeg ; recale chaque frontière sur la transition d'énergie la plus proche dans une fenêtre bornée, et marque celles qui restent douteuses. L'enveloppe existe déjà (cache de `--speech-onset`, gratuit). Supprime la boucle de sondage, rend `remove_words` fiable, et fait disparaître la classe « mot avalé en bordure de plan » | fait (fonction pure, `--align-transcripts [--write]`, outil `align_transcript` ; seuil de `--speech-onset` porté à 25 % — 90ᵉ centile − 12 dB) |
| A2 — **Opération `tighten_pauses(clip, min_gap, keep)`.** Trouve les creux ≥ `min_gap` dans la plage source du plan, ramène chacun à `keep` images, referme en ripple, le tout en une opération réversible. Doit couvrir la paire A/V liée et les pistes synchronisées, comme Q4b l'a fait pour `remove_words`. Mesuré : deux appels équivalents ont retiré 6,6 s de silences internes sur un seul montage, sans lire un mot | fait (`PauseTightening.h`, `--tighten-pauses`, outil `tighten_pauses` ; résout une `RemoveWordsOperation` portant la paire A/V et les pistes synchronisées) |
| A3 — **Transcription par lot, médias muets sautés.** `--transcribe` accepte plusieurs `media-id` et ne charge le modèle qu'une fois (~8 s × N économisés). `--ingest` enregistre le niveau audio moyen mesuré par FFmpeg comme fait du document, au même titre que la cadence et la durée. Mesuré : 29 des 71 rushes du projet étaient des plans de coupe muets à −74 dB, passés à whisper avant d'être reconnus | fait (`--transcribe` prend plusieurs `media-id`, un seul chargement de modèle ; `--ingest` mesure et enregistre `audio_level`, schéma document v6) |
| A4 — **Adressage par image source.** Résolveur `(media_id, source_frame)` → `(clip_id, timeline_position)` sur la timeline active, exposé en outil et utilisé par les découpes et les rognages. Mesuré : chaque position timeline calculée à la main dans cette session a produit au moins une erreur ; le script qui prenait des bornes source et résolvait la position lui-même n'en a produit aucune | fait (`SourceAddress.h`, `--locate-source-frame`, outil `locate_source_frame` ; `split_clip`, `trim_clip` et `ripple_trim` acceptent `source_frame`) |
| A5 — **`sync_track_ids` sur `split_linked_clips`, `remove_linked_clips` et `remove_clip`.** `ripple_trim` l'accepte déjà, pas les autres. C'est précisément pourquoi retirer un plan a forcé la reconstruction complète d'une piste de coupe : la piste sans membre du groupe lié ne suit pas le décalage | à faire |
| A6 — **`contact_sheet` et `cut_sheet`, rendus à travers le pipeline couleur.** `read_frame` (P1) donne une image par appel : mauvaise granularité pour juger un montage. Il faut une planche des milieux de plans et une planche des paires d'images encadrant chaque coupe. Et elles doivent passer par la transformation couleur du document — une image tirée d'un rush log est délavée, donc inutilisable pour juger une exposition ou une lisibilité | à faire |
| A7 — **Gain par clip et fondus audio.** Schéma (comme F0.1), opération, export et rendu. Mesuré : un plan à −27 dB, 10 dB sous le reste du montage, n'a pu être traité qu'en le supprimant — avec la phrase qu'il portait | à faire |
| A8 — **`timeline_stats` : la densité comptée en plans visibles.** Durée, changements de plan visibles, plans/minute, part de plans de coupe, position de la première coupe. Une coupe de V1 masquée par un plan de coupe sur V2 n'est pas un changement de plan : sur un des quatre montages, 28 clips ne font que 25 plans visibles. `list_shot_quality` sait déjà raisonner sur le recouvrement (P3) ; c'est la même donnée. Sans cet outil, le contrôle arithmétique que la procédure exige se fait à la main, donc mal | à faire |
| A9 — **Garde-fou sur une transcription aberrante.** Comparer le nombre de mots rendus à la durée de parole mesurée, et signaler l'écart. Mesuré : sur C4774, 32 s de parole à −19 dB, `ggml-large-v3` rend 6 mots — de façon reproductible, sur le rush entier comme sur un proxy — là où `ggml-small` en rend 85, dont le seul détail concret du personnage. Sans ce contrôle, la transcription passe pour vide et le rush est écarté à tort | à faire |

### Constats qui mettent à jour des points existants

- **La sortie vers Resolve existe.** Le point « aucune sortie interopérable »
  ci-dessus est en partie périmé : `--export-resolve-timeline` plus
  `sidecar/resolve_bridge.py --send` ont renvoyé les quatre montages dans le
  projet Resolve ouvert, sur deux pistes vidéo, à la durée exacte au frame
  près. Il manque toujours OTIO/FCPXML/EDL.
- **`--speech-onset` sous-estimait l'air mort.** Il annonçait `tight: true`
  sur des plans portant 1,0 à 1,2 s de silence en tête, et `suggested_trim: 3`
  là où l'attaque réelle est 27 images plus loin. Son seuil était calé trop
  près du plancher de bruit ; sur ces rushes celui-ci ne tombe qu'à ~18 dB
  sous la parole. **Corrigé avec A1** : `speech_ratio_percent` passe de 13 à
  25 % (90ᵉ centile − 12 dB), et `tests/speech_onset_tests.cc` reproduit les
  deux comportements — l'ancien seuil prenant une respiration pour l'attaque
  et proposant les 3 images relevées, le nouveau tombant sur le mot.
- **Le grading créatif reste absent du fichier livré, mais la gestion couleur,
  non.** Le point « `Export.cc` ne lit jamais `clip.effects` » reste vrai et
  distinct de `color_management`, qui, lui, est bien appliqué à l'export :
  mesuré sur la même image, SATAVG passe de 18,0 (rush log) à 40,4
  (livrable), YAVG de 452 à 616.

### Ordre de prise recommandé

A1 d'abord : lui seul supprime le poste dominant, et A2 comme A9 s'appuient sur
l'enveloppe qu'il fabrique. A3, A5 et A8 sont indépendants, courts, et
gagnables en parallèle. A4 et A6 ensuite. A7 est le seul qui touche au schéma
document, donc il suit les règles de la Phase 0 (bump de version, migration
refusée) et se fait seul.

**A1 à A4 sont faits.** Notes laissées par cette passe, pour qui prend la
suite :

- A3 a finalement touché au schéma document (`LibraryMedia::audio_level`,
  v5 → v6), ce que l'ordre de prise attribuait à A7 seul. Le niveau moyen est
  un fait mesuré sur le fichier, au même titre que la cadence, et le ticket le
  demandait explicitement comme fait du document. Un document v5 se charge
  toujours, et son entrée se lit « non mesurée » — pas « silencieuse » : la
  distinction est portée par `audio_level_measured`, et un ré-`--ingest`
  remplit le champ. Même précédent que la migration v4 → v5 de l'opacité.
  `--ingest` décode désormais l'audio de chaque rush : mesuré à 0,3 s sur un
  fichier 4K de 268 Mo, contre les 11× temps réel qu'une transcription inutile
  coûtait.
- A2 ne crée pas d'opération : il résout une `RemoveWordsOperation`, qui fait
  déjà exactement le travail (couper des plages source, refermer en ripple,
  emporter la paire A/V de Q4b, décaler les pistes synchronisées) et le fait
  réversiblement. Une seconde opération qui referme des plages source serait
  une seconde implémentation de la même chose.
- A2 ne touche pas à l'air de tête et de queue d'un plan : c'est la question
  de `SpeechOnset.h` et d'un rognage, et y répondre ici ferait un autre
  montage que celui demandé. Les deux sont mesurés et publiés dans le rapport.
- `LinkedClipIdsFor` a quitté `McpTools.cc` pour `Operations.h`
  (`LinkedClipIdsCoveringRanges`) : la CLI en avait besoin aussi, et une règle
  sur les plans qu'une coupe doit emporter est du moteur.
- A4 rend la queue d'un rognage **inclusive** : `source_frame` y désigne la
  dernière image gardée, pas la première écartée. C'est ce que veut dire un
  numéro d'image pour un monteur, et le +1 appartient au moteur.
- Corrigé en passant, sans rapport avec A1–A4 : `cutmachine_ui_smoke_tests`
  était **intermittent**. Ses vérifications démarrent sur le tour de boucle
  suivant `applicationDidFinishLaunching`, alors que devenir *key* est un
  aller-retour avec le WindowServer ; quand la course était perdue, AppKit
  routait le premier clic vers l'activation et non vers la vue, et les sept
  vérifications de sélection, de renommage et de glissé échouaient ensemble.
  Mesuré à 1 exécution sur 6 au repos, et 9 sur 12 avec quatre instances en
  concurrence pour le focus ; 0 sur 12 après correction (attente bornée de la
  fenêtre *key*, avec un échec explicite si elle ne le devient jamais).
- Reste ouvert et non traité ici : `McpLiveBackend` (donc le panneau chat)
  n'expose toujours ni l'enveloppe de parole, ni la transcription, ni
  l'alignement — c'est le même trou que Q5 a bouché pour
  `ReadSourceTranscript`, et il concerne maintenant `tighten_pauses` et
  `align_transcript`. Le serveur MCP et la CLI, eux, les ont.

Toutes les phases (0, 1, 2) sont closes. Reste hors roadmap : validation
visuelle réelle sur macOS (AppKit/Metal non compilables dans ce
sandbox Linux) et le fossé `ProjectStorage.cc`/CommonCrypto qui bloque
la compilation CMake complète hors macOS (préexistant, sans rapport
avec ce chantier).
