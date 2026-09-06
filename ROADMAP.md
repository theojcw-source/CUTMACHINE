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

- **Corrigé — verrou partagé entre `--mcp-serve` et l'app.** Les deux surfaces
  prennent `ProjectSessionLock` et un second écrivain reçoit un refus JSON.
- **Vérification du rendu** (comparer l'export à ce que le document décrit) reste un travail manuel.

État des points relevés ici, par ordre de gravité (voir l'audit) :

- **`Export.cc` ne lit jamais `clip.effects`.** L'étalonnage est visible au
  moniteur et absent du fichier livré. C'est un défaut de correction, et le
  premier ticket à prendre.
- **Corrigé — `Test copilot/` est branché** : les trois suites sont des cibles
  CTest et le workflow `.github/workflows/ctest.yml` les exécute sur macOS.
- **Aucune sortie interopérable** (OTIO/FCPXML/EDL). L'entrée existe désormais
  pour DaVinci Resolve Studio (`sidecar/resolve_bridge.py` + `--import-resolve`,
  chutiers et rushes seulement) ; rien ne repart de CUTMACHINE vers un autre
  logiciel de montage.
- **Pas de marqueur de tour dans `EditLog`** : impossible d'annuler une
  intervention d'agent en un geste.
- **Corrigé — la langue entre dans l'identité du cache de transcription** ; les anciens
  caches restent lisibles avec `auto` comme valeur explicite par défaut.

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
| A5 — **`sync_track_ids` sur `split_linked_clips`, `remove_linked_clips` et `remove_clip`.** `ripple_trim` l'accepte déjà, pas les autres. C'est précisément pourquoi retirer un plan a forcé la reconstruction complète d'une piste de coupe : la piste sans membre du groupe lié ne suit pas le décalage | fait (opérations, CLI `--apply-op`, MCP, sérialisation canonique et undo exact) |
| A6 — **`contact_sheet` et `cut_sheet`, rendus à travers le pipeline couleur.** `read_frame` (P1) donne une image par appel : mauvaise granularité pour juger un montage. Il faut une planche des milieux de plans et une planche des paires d'images encadrant chaque coupe. Et elles doivent passer par la transformation couleur du document — une image tirée d'un rush log est délavée, donc inutilisable pour juger une exposition ou une lisibilité | fait (sélection exacte des plans visibles et des paires de raccord, JPEG borné avec vue SDR OCIO, CLI/MCP et métadonnées par cellule) |
| A7 — **Gain par clip et fondus audio.** Schéma (comme F0.1), opération, export et rendu. Mesuré : un plan à −27 dB, 10 dB sous le reste du montage, n'a pu être traité qu'en le supprimant — avec la phrase qu'il portait | fait (schéma v7 migré depuis v4/v5/v6, `SetClipAudio`, gain et fondus exacts en export/lecture, MCP et undo canonique) |
| A8 — **`timeline_stats` : la densité comptée en plans visibles.** Durée, changements de plan visibles, plans/minute, part de plans de coupe, position de la première coupe. Une coupe de V1 masquée par un plan de coupe sur V2 n'est pas un changement de plan : sur un des quatre montages, 28 clips ne font que 25 plans visibles. `list_shot_quality` sait déjà raisonner sur le recouvrement (P3) ; c'est la même donnée. Sans cet outil, le contrôle arithmétique que la procédure exige se fait à la main, donc mal | fait (moteur pur, CLI `--timeline-stats`, outil MCP `timeline_stats`, recouvrements V1/V2 testés) |
| A9 — **Garde-fou sur une transcription aberrante.** Comparer le nombre de mots rendus à la durée de parole mesurée, et signaler l'écart. Mesuré : sur C4774, 32 s de parole à −19 dB, `ggml-large-v3` rend 6 mots — de façon reproductible, sur le rush entier comme sur un proxy — là où `ggml-small` en rend 85, dont le seul détail concret du personnage. Sans ce contrôle, la transcription passe pour vide et le rush est écarté à tort | fait (`likely_incomplete`, seuil exact 10 s / 30 mots-min, cache et réponses CLI/MCP) |

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

---

## Suite d'audit — fiabilité du pilotage (QC-2026-09b)

Déclenché par le montage des huit interviews de
`LISAASTR136_ITW_PFEStrasbourg2026` (210 rushes, 68,4 min de matière) en une
session, **et par le refus de la première livraison**. Les huit films
respectaient tous les repères mesurables — 20 à 30 plans/minute, 40 à 55 % de
plans de coupe, première coupe à 3,0 s, durées dans la fourchette — et le
monteur les a refusés d'un mot : « le montage est pas bien ».

Le diagnostic, établi en relisant les textes montés : **des antécédents coupés
dont les conséquences étaient gardées.** Un montage ouvrait sur « …de
réhabiliter » sans sujet ni verbe ; un autre conservait « et moi, en tant que
sportive **également**, c'est ce que je ressens » après suppression de la
phrase qui portait ce « également ». Chaque plan était propre, l'enchaînement
ne voulait rien dire.

Ce constat est d'abord un problème de procédure, traité dans le skill
`monter-une-itw` (réécrit intégralement, la règle de l'antécédent en tête).
Mais la reprise complète des huit montages a mis au jour une deuxième
famille : **des défauts qu'aucune surface du moteur ne permet de voir**, et
des pannes de pilotage silencieuses. Ce sont les tickets ci-dessous.

Coûts et faits mesurés sur ce projet :

| constat | mesure |
|---|---|
| Rushes muets transcrits quand même, avec un texte halluciné | **103 sur 210** (« Sous-titrage Société Radio-Canada ») |
| Blocs ouvrant sur la question de l'intervieweur | **6 sur 8 montages**, invisibles dans toute transcription |
| Écart de niveau intervieweur / sujet | **15 à 20 dB** (question −27/−49 dB, réponse −11/−14 dB) |
| Silence résiduel après `tighten_pauses` à 400 ms / 6 images | **4,2 %** de la durée, creux jusqu'à **0,67 s** |
| Dérive des horodatages whisper, en plein milieu de fichier | **13 à 22 images** (0,5 à 0,9 s) |
| `--apply-project-op` | ~10 s l'unité ; 15 suppressions dépassent 120 s |
| Export 1080×1920 depuis 4K S-Log3 | ~2,2 min par film de ~60 s |

| Ticket | Statut |
|---|---|
| B1 — **Discrimination du locuteur au niveau.** `analyze_speech_onset` rend les groupes de parole et leur niveau, pas un onset unique | fait (groupes en cache, `dominant_onset`, seuils paramétrables, CLI/MCP/agent intégré, fixture faible-creux-fort) |
| B2 — **Enveloppe de réponse uniforme.** Tout refus revient en `{"ok":false,...}`, jamais en chaîne nue | fait (sérialiseur commun CLI/MCP et parcours exhaustif des catalogues) |
| B3 — **Refuser plutôt que réussir sans rien faire.** Une opération hors bornes lève `InvalidOperation` | fait (cinq opérations, bornes rationnelles explicites, aucun journal sur refus) |
| B4 — **`trim_boundary_air` : fermer les creux de bordure**, que `tighten_pauses` laisse par construction | fait (`TrimBoundaryAir` atomique, variantes plan/raccord fondées sur les groupes B1, paires liées et pistes sync, convergence et undo testés) |
| B5 — **`ripple_trim` emporte le groupe lié par défaut**, comme `tighten_pauses` | fait (sous-ensemble explicite conservé, résultat `also_cut`, avertissement de synchronisation) |
| B6 — **`get_timeline_transcript` exact après découpe** : plus de mots doublés ni manquants | fait (attribution par chevauchement maximal, `straddles_cut`, test de coupe au milieu d'un mot) |
| B7 — **`transcribe_timeline` : le texte réellement monté**, en une commande | fait (PCM assemblé sans export, cache exact modèle/langue/montage, MCP et `--srt-from-media`) |
| B8 — **Les timelines deviennent de première classe** : listées par `describe`, créées et supprimées en MCP | fait (`describe`, quatre outils MCP, backend fichier et agent intégré, exemples canoniques documentés) |
| B9 — **`--timeline <id>` explicite** sur les commandes de sortie | fait (trois sorties CLI, outils MCP d'édition et mode strict local `TimelineRequired`) |
| B10 — **`--apply-project-op` : lot et chargement paresseux** | fait (lot atomique en un undo, journaux partiels préservés, 15 suppressions testées sous 10 s) |
| B11 — **`--ingest` accepte un fichier, et un média sans image** | fait (fichier ou dossier, `has_video`, temps audio exact, refus explicites des outils image) |
| B12 — **Garde-fou sur la transcription hallucinée** (jumeau d'A9, dans l'autre sens) | fait (durée de parole mesurée, signal et tournures connues, refus MCP avec `force`) |

---

### B1 — Discrimination du locuteur au niveau

> **En tant qu'**agent qui monte une interview, **je veux** savoir où commence
> la parole *du sujet* et non la première parole venue, **afin de** ne pas
> faire ouvrir un plan sur la question de l'intervieweur.

**Constat mesuré.** Six blocs sur les huit montages ouvraient sur « Et c'est
quoi ? », « Qu'est-ce que c'est ? » ou l'équivalent. Le défaut est **invisible
dans toutes les transcriptions** : whisper ne transcrit que le locuteur
principal et pose son premier mot à l'image 0, par-dessus la question.
`analyze_speech_onset` ne le voit pas non plus — il mesure de la parole, pas
*qui* parle, et déclare `tight: true`.

Le discriminateur est fiable et générique : **l'intervieweur n'est jamais
micro-cravaté.** Mesuré sur C8781 — question de 0,00 à 0,94 s entre −27 et
−49 dB, creux net à −57 dB, réponse à partir de l'image 25 entre −11 et
−18 dB. Reproduit sur les huit personnes.

**Critères d'acceptation.**

1. `analyze_speech_onset` enregistre, en plus de l'onset actuel, la liste des
   **groupes de parole** du plan : `{start, end, level_dbfs, peak_dbfs}`,
   séparés par des creux d'au moins un seuil paramétrable.
2. `list_speech_onsets` publie ces groupes et un champ dérivé
   `dominant_onset` : début du premier groupe dont le niveau est à moins de
   `dominance_db` (défaut 9 dB) du 90ᵉ centile du plan, avec une tenue
   minimale (défaut 6 fenêtres de 20 ms).
3. Sur les rushes de ce projet, `dominant_onset` tombe entre 12 et 25 images
   et coïncide avec la lecture manuelle de l'enveloppe, à ±2 images.
4. Un plan sans question en tête garde `dominant_onset == onset`.
5. Test de non-régression : une fixture synthétique « parole faible 1 s, creux
   0,2 s, parole forte » rend le second groupe.

**Implémentation.** Fonction pure sur l'enveloppe déjà produite par A1
(`(échantillons, seuils) → groupes`), donc testable sans FFmpeg — même forme
qu'`align_transcript`. Aucun champ de document nouveau : c'est un cache
d'analyse, comme l'onset. Exposition CLI + MCP + UI (principe 3).

**Dépend de** A1 (l'enveloppe existe déjà, gratuite).

---

### B2 — Enveloppe de réponse uniforme sur tous les refus

> **En tant qu'**auteur d'un script de pilotage, **je veux** que tout refus du
> moteur arrive sous la même forme, **afin de** ne pas confondre un échec avec
> un succès et n'exécuter la suite qu'à tort.

**Constat mesuré.** `create_interview_short` a refusé un écart entre spans
(« spans 'S3' and 'S4' are separated by more than a breath ») **en renvoyant
une chaîne de texte nue**, pas `{"ok":false}`. Le client n'a pas levé
d'erreur ; les trois opérations suivantes se sont appliquées à la timeline
précédente — le dérushage — et la panne n'a fait surface que trois étapes plus
loin, sous la forme d'un `IndexError` dans le code appelant, sans rapport
visible avec la cause.

C'est le pire mode de panne du système : silencieux, à retardement, et le
message d'erreur final désigne le mauvais coupable.

**Critères d'acceptation.**

1. Toute réponse d'outil MCP et de commande CLI qui n'aboutit pas est un objet
   JSON portant `ok: false`, `error` (code stable) et `detail` (message).
2. Aucun chemin de code ne rend une chaîne libre en cas d'échec. Un test
   parcourt le catalogue d'outils, provoque un refus sur chacun, et vérifie la
   forme de la réponse.
3. Les codes d'erreur existants (`ParseError`, `ValidationFailed`,
   `UnknownSequence`, `InvalidOperation`…) sont conservés.

**Implémentation.** Point unique de sortie d'erreur dans `McpTools.cc` et
`Cli.cc`. Le travail est mécanique ; la valeur est dans le test exhaustif.

---

### B3 — Refuser plutôt que réussir sans rien faire

> **En tant qu'**agent, **je veux** qu'une opération impossible échoue
> bruyamment, **afin de** ne pas croire une timeline modifiée alors qu'elle
> est intacte.

**Constat mesuré.** `ripple_trim` appelé avec un `source_frame` situé hors des
bornes du plan visé répond `{"ok":true}` et ne change rien. Rencontré deux
fois : sur Fanny (C8875, plan de 21 images, rognage demandé avant son début)
et sur Lucien (C8920). Dans les deux cas la durée de la timeline n'a pas
bougé, donc le contrôle « la durée a-t-elle changé ? » ne l'a pas vu.

C'est la même famille que le constat déjà consigné sur `split_linked_clips`,
qui rend `ok:true` sur une position hors du plan visé.

**Critères d'acceptation.**

1. `ripple_trim`, `trim_clip`, `split_clip`, `split_linked_clips` et
   `roll_edit` refusent avec `InvalidOperation` quand la position ou l'image
   source demandée est hors des bornes du plan.
2. Le message nomme les bornes réelles et la valeur reçue.
3. Un test par opération, dérivé des deux cas mesurés.

**Implémentation.** Validation dans le parseur d'arguments plutôt que dans
l'application : le refus doit précéder l'écriture du journal, sinon on
enregistre une opération sans effet.

---

### B4 — `trim_boundary_air` : fermer les creux de bordure

> **En tant que** monteur, **je veux** que les silences en tête et en queue de
> plan se ferment comme les silences internes, **afin de** ne pas devoir
> reprendre chaque bordure à la main après le resserrement.

**Constat mesuré.** `tighten_pauses` ne touche ni la tête ni la queue d'un
plan — c'est documenté et volontaire (il publie `head_air` et `tail_air`).
Conséquence : **après resserrement, les seuls creux restants sont aux
bordures, et ce sont exactement ceux qui s'entendent**, puisque les internes
ont été refermés.

Boucle écrite à la main pour cette session (mesurer, trouver le plan, rogner
le bon bord, répéter) : sur Fanny, silence de **3,4 % → 0,3 %** et plus grand
creux de **0,67 s → 0,19 s** en trois passes.

Deux pièges rencontrés, à reprendre dans l'implémentation :

- **Un creux à cheval sur une jonction n'appartient à aucun des deux plans.**
  Rogner un seul côté déplace la jonction sans fermer le trou : la boucle a
  divergé, quatre passes, creux passé de 0,51 à **0,64 s**. Il faut mesurer
  les deux côtés et rogner les deux (queue du sortant à l'image où le niveau
  décroche, tête de l'entrant à l'image où il remonte).
- **Couper pile sur l'attaque escamote le premier phonème.** Il faut laisser
  2 à 3 images de battement. Mesuré : le montage disait « .. c'est un sujet »
  au lieu de « Et sur le plan plus personnel, c'est un sujet ».

**Critères d'acceptation.**

1. Opération `trim_boundary_air(clip_id, keep_frames, min_air_ms)` : rogne
   l'air de tête et de queue au-delà de `keep_frames`, en une opération
   réversible, emportant la paire A/V liée et les pistes synchronisées.
2. Les bornes viennent de l'enveloppe (B1/A1), **jamais** des horodatages de
   mots — mesuré : whisper annonce « l'objectif » à l'image 386, l'attaque
   réelle est à 399.
3. Variante `close_junction_air(clip_gauche, clip_droit)` qui traite les deux
   côtés d'une jonction en une seule opération et converge par construction.
4. Un test reproduit le cas de divergence à quatre passes.

**Implémentation.** Résout une `RemoveWordsOperation` comme le fait A2, ou une
paire de `RippleTrim` — l'important est qu'un seul `undo` défasse le geste.

**Dépend de** B1 pour la mesure de bordure, B5 pour l'emport du groupe lié.

---

### B5 — `ripple_trim` emporte le groupe lié par défaut

> **En tant qu'**agent, **je veux** que rogner un plan emporte son plan son,
> **afin de** ne pas désynchroniser un montage sans m'en apercevoir.

**Constat mesuré.** `tighten_pauses` emporte la paire A/V toute seule et le
dit dans `also_cut`. **`ripple_trim`, non.** Rogner la tête d'un plan vidéo
d'un montage sorti de `create_interview_short` laisse le plan son intact,
décale l'image seule, et pose un trou de la longueur du rognage en fin de
piste vidéo — **sans changer la durée de la timeline**, donc invisible au
contrôle le plus évident.

Mesuré : rognage de 11 images, timeline à 1489 images avant et après, V1 et A1
désynchronisés à partir du plan touché.

L'asymétrie entre deux opérations voisines est le vrai défaut : rien ne
prévient l'appelant que l'une fait ce que l'autre ne fait pas.

**Critères d'acceptation.**

1. `ripple_trim` emporte par défaut tous les membres du groupe lié du plan
   visé, et le rapporte comme `tighten_pauses` le fait avec `also_cut`.
2. `linked_clip_ids` reste accepté pour désigner un sous-ensemble explicite.
3. Un `sync_track_ids` omis alors que d'autres pistes contiennent des plans en
   aval produit un **avertissement** dans la réponse (pas un refus : couvrir
   une piste de coupe volontairement est légitime).
4. Test : rogner une tête sur un montage à deux pistes vidéo et une piste
   audio laisse V1, V2 et A1 alignés.

**Voisin d'A5**, qui ajoute `sync_track_ids` à `split_linked_clips`,
`remove_linked_clips` et `remove_clip`. Les deux tickets ferment la même
classe ; les prendre ensemble.

---

### B6 — `get_timeline_transcript` exact après découpe

> **En tant qu'**agent, **je veux** que la transcription d'une timeline
> corresponde à ce qu'elle joue, **afin de** ne pas corriger des défauts qui
> n'existent pas.

**Constat mesuré.** Relu sur un montage déjà resserré, `get_timeline_transcript`
rend des mots **doublés** (« à Châteaulin, Châteaulin, donc ») et des mots
**manquants** (« On transformé en complexe sportif » pour « On l'a
transformé »). Neuf défauts apparents sur un seul montage, **tous inexistants** :
la transcription du son réellement monté était propre.

Cause : un mot à cheval sur une coupe est compté dans les deux plans, ou dans
aucun. Le resserrement multiplie les coupes (6 blocs → 25 plans), donc le
problème s'aggrave exactement quand on en a le plus besoin.

**Critères d'acceptation.**

1. Un mot dont l'intervalle chevauche une bordure de plan est rendu **une
   seule fois**, attribué au plan qui en contient la plus grande part.
2. Un mot entièrement hors des plages jouées n'est pas rendu.
3. Les spans portent un drapeau `straddles_cut` quand le mot a été tranché,
   pour que l'appelant sache que le rendu peut être approximatif.
4. Test : timeline de deux plans contigus issus d'un même rush, coupée au
   milieu d'un mot ; le mot apparaît une fois.

---

### B7 — `transcribe_timeline` : le texte réellement monté

> **En tant qu'**agent, **je veux** obtenir en une commande le texte de ce que
> la timeline joue vraiment, **afin de** vérifier qu'un montage se tient avant
> de l'exporter.

**Constat mesuré.** C'est le seul contrôle qui attrape un mot amputé et un
enchaînement absurde — et donc le seul qui aurait attrapé le défaut ayant fait
refuser la première livraison. Aujourd'hui il demande une chorégraphie de ~40
lignes hors moteur : extraire le son de chaque plan avec `ffmpeg -ss/-t`,
concaténer, emballer dans une vidéo noire, poser dans un dossier temporaire,
`--ingest`, lancer un serveur MCP, `transcribe_media`, relire le cache.

Fait **au moins 43 fois** dans cette session — un fichier de son monté par
contrôle, sans compter les passes de finition qui réécrivaient le même. Chaque
itération coûte ~15 s de machine et beaucoup de code fragile.

**Critères d'acceptation.**

1. `transcribe_timeline(timeline_id?)` rend le texte du son des pistes audio
   de la timeline, plan par plan et concaténé, sans passer par un export
   vidéo.
2. Aucun fichier intermédiaire visible par l'appelant.
3. Le résultat est mis en cache et invalidé par toute opération qui change les
   bornes des plans audio.
4. Le même appareillage sert B12 (garde-fou d'hallucination) et le SRT depuis
   le livrable : une variante `--srt-from-media <fichier>` remplace le projet
   jetable décrit dans le skill.

**Implémentation.** Le décodage et la concaténation existent déjà dans le
chemin d'export ; il s'agit de les brancher sur le transcripteur au lieu de
l'encodeur. Attention à la langue dans l'identité du cache — c'est le trou
déjà consigné plus haut.

---

### B8 — Les timelines deviennent de première classe

> **En tant qu'**agent qui produit huit montages dans un projet, **je veux**
> lister, créer et supprimer des timelines par les surfaces normales, **afin
> de** ne pas lire le disque ni fabriquer du JSON canonique à la main.

**Constat mesuré.** `describe` ne montre que la timeline active. Pour
retrouver les huit montages j'ai dû énumérer `Timelines/*.json` dans le
paquet et parser chaque fichier pour en lire le nom.

Créer ou supprimer une timeline n'existe pas en MCP : il faut
`--apply-project-op` avec un JSON **positionnel et strict**, dont j'ai dû lire
le sérialiseur C++ pour découvrir la forme exacte — le champ
`"exact_project_hex":null` n'est deviné nulle part, et son absence rend
`ParseError: expected ',"exact_project_hex":' at byte 74`.

**Critères d'acceptation.**

1. `describe` rend un tableau `timelines` : `{id, name, width, height,
   frame_rate, duration, active}`.
2. Outils MCP `list_timelines`, `add_timeline`, `remove_timeline`,
   `rename_timeline`, avec la même sémantique que les opérations projet
   existantes.
3. `remove_timeline` refuse la dernière timeline du projet (comportement déjà
   présent dans l'opération).
4. La documentation d'`--apply-project-op` donne un exemple complet par type
   d'opération projet.

---

### B9 — `--timeline <id>` explicite sur les commandes de sortie

> **En tant qu'**agent qui enchaîne huit exports, **je veux** nommer la
> timeline dans la commande, **afin de** ne pas dépendre d'un état global que
> l'opération précédente a pu laisser ailleurs.

**Constat mesuré.** `--export`, `--export-srt`, `--export-resolve-timeline` et
**toutes** les opérations MCP agissent sur la timeline active. Avec huit
montages, c'est une chorégraphie permanente de `set_active_timeline`.

Le danger est réel, pas théorique : une création de montage ayant échoué
(voir B2) a laissé le dérushage actif, et le resserrement suivant s'est
appliqué à la matière brute — **92 creux refermés dans le dérushage** au lieu
du film. Rattrapé parce que le dérushage est un brouillon reconstruit à chaque
personne ; sur une timeline précieuse, c'était une perte.

**Critères d'acceptation.**

1. Les trois commandes de sortie acceptent `--timeline <id>` ; sans lui, elles
   gardent le comportement actuel.
2. Les outils MCP d'édition acceptent un `timeline_id` optionnel.
3. Un mode strict (réglage local) fait **refuser** toute opération d'édition
   qui ne nomme pas sa timeline, pour les scripts qui veulent l'explicite.

---

### B10 — `--apply-project-op` : lot et chargement paresseux

> **En tant qu'**agent qui nettoie un projet, **je veux** appliquer plusieurs
> opérations projet en un appel, **afin de** ne pas payer un rechargement
> complet par suppression.

**Constat mesuré.** Chaque `--apply-project-op` charge le projet **et le
journal d'édition de chaque timeline** avant d'appliquer. Sur ce projet (210
médias, 24 timelines), une suppression coûte ~10 s. Mes 15 suppressions de
timelines abandonnées ont dépassé les 120 s d'un budget de commande et ont dû
repasser en tâche de fond.

**Critères d'acceptation.**

1. `--apply-project-op` accepte un **tableau** d'opérations, appliquées dans
   l'ordre, journalisées comme un lot annulable en un `undo`.
2. Les journaux de timeline ne sont chargés que si l'opération les touche.
3. Mesure de non-régression : supprimer 15 timelines d'un projet de cette
   taille tient sous 10 s au total.

---

### B11 — `--ingest` accepte un fichier, et un média sans image

> **En tant qu'**agent, **je veux** ingérer un fichier précis, y compris
> audio seul, **afin de** ne pas fabriquer une vidéo noire et un dossier
> temporaire pour vérifier un son.

**Constat mesuré.** Deux refus successifs sur le même geste :

- `--ingest <fichier>` → `"path is not a directory"` ;
- `--ingest <dossier contenant un wav>` → `"no video stream"`.

Contournement écrit pour cette session : emballer le wav dans une vidéo noire
64×64 en H.264 (`color=c=black:s=64x64:r=25`), le poser seul dans un dossier
temporaire, puis ingérer le dossier. Exécuté à chaque contrôle de son monté,
soit au moins 43 fois.

**Critères d'acceptation.**

1. `--ingest` accepte indifféremment un fichier ou un dossier.
2. Un média sans piste vidéo est ingéré, avec `has_video: false` dans la
   bibliothèque ; les outils qui exigent une image le refusent explicitement
   plutôt que l'ingestion.
3. B7 rend ce contournement inutile pour le contrôle du son monté ; ce ticket
   reste utile pour tout le reste (voix off, musique, son seul).

---

### B12 — Garde-fou sur la transcription hallucinée

> **En tant qu'**agent, **je veux** être averti qu'une transcription est
> inventée, **afin de** ne pas bâtir une sélection sur du texte qui ne
> correspond à aucun son.

**Constat mesuré.** **103 rushes sur 210** — la moitié du tournage — sont des
plans de coupe sans parole, et whisper y écrit « Sous-titrage Société
Radio-Canada » ou « Sous-titres par Jérémy Diaz ». Ces textes n'ont aucun
rapport avec le contenu.

A3 enregistre bien `audio_level` à l'ingestion, mais ces plans **ne sont pas
muets** : ils portent du fond de salle. Ils passent donc le filtre
`include_silent` et repartent avec un texte faux.

C'est le jumeau d'A9, dans l'autre sens : A9 signale une transcription trop
courte pour la durée de parole, B12 signale une transcription **sans parole
mesurable pour la porter**.

**Critères d'acceptation.**

1. Après transcription, comparer le nombre de mots à la **durée de parole
   mesurée** par l'enveloppe (pas à la durée du média).
2. Marquer `likely_hallucinated: true` quand des mots sont rendus alors que
   l'enveloppe ne contient aucun groupe de parole au-dessus du plancher.
3. Une liste noire de tournures connues (mentions de sous-titrage, d'abonnement,
   de générique) renforce le signal sans en être l'unique critère — un rush qui
   parle réellement de sous-titres ne doit pas être marqué.
4. `list_disfluencies`, `remove_words` et `create_interview_short` refusent de
   travailler sur un transcript marqué, sauf `force: true`.

**Dépend de** B1 (les groupes de parole).

---

### Constats qui mettent à jour des points existants

- **A8 confirmé, et une correction à mon propre compte rendu.** Les nombres de
  plans que j'ai publiés pour ces huit montages (20 à 35) **surestiment les
  changements de plan visibles** : une coupe de V1 masquée par un plan de
  coupe sur V2 n'en est pas un. Comptés à la main faute d'outil, donc mal —
  exactement ce que le ticket prévoit.
- **A6 confirmé, deux fois.** J'ai réécrit à la main une planche par personne
  (une vignette par plan de coupe) puis une planche de cadrage (trois images
  par rush de parole), avec un délogarithme approximatif
  (`eq=contrast=1.5:saturation=1.9:gamma=0.78`) uniquement pour *identifier* le
  contenu. Sans ça, impossible de savoir ce que montraient 103 plans. La
  planche de cadrage a d'ailleurs renversé une conclusion : deux personnes
  jugées « sans plans de coupe » d'après les transcriptions en avaient sept,
  classés « parole » parce que l'équipe bavardait par-dessus.
- **A5 confirmé.** Supprimer un plan a bien forcé la reconstruction complète de
  la piste de coupe, sur Fanny et sur Lucien.
- **Le repère couleur du skill était faux.** « Un rush log est vers SATAVG 30 »
  ne vaut pas pour cette série : les rushes sortent entre **6 et 18**, les
  livrables entre **12 et 56**. C'est le **rapport** entre les deux, au même
  instant, qui prouve l'étalonnage — jamais une valeur absolue. `YMIN` ne
  prouve rien : il monte ou descend selon le contenu.
- **`sidecar/resolve_bridge.py` : Resolve met en cache le média importé par son
  chemin.** Réécrire un `.srt` sur le disque ne rafraîchit pas l'élément du
  Media Pool : `ImportMedia` rend l'ancien. Sur huit montages renvoyés, **sept
  portaient l'ancien sous-titrage**, et seuls trois avaient une durée
  aberrante — les autres coïncidaient par hasard. Parade retenue : importer
  depuis un nom de fichier neuf à chaque révision, et **compter les éléments de
  sous-titres de la timeline renvoyée** pour les comparer au nombre de cues du
  SRT. À porter dans le pont plutôt que dans chaque script appelant.
- **Le réglage de `tighten_pauses` a une limite basse.** À **250 ms**, il a
  refermé un creux de 7 images *à l'intérieur du mot « notre »* — le montage
  disait « nd'étude » pour « Pour notre projet d'étude ». Le creux était réel
  dans l'enveloppe (une occlusive laisse un trou) sans être une pause.
  **300 à 320 ms / 4 images** est le compromis mesuré : mots entiers, plus
  grand creux à 0,26 s. À documenter dans l'aide de l'outil.
- **Le seuil de mesure du silence compte autant que le réglage.** À −35 dB, un
  montage manifestement mou mesure 1,6 % de silence ; à **−30 dB**, le même
  mesure 4,2 %. Le fond de salle est au-dessus de −35 dB : on cherche l'absence
  de *parole*, pas de *signal*.

### Ordre de prise recommandé

**B2 et B3 d'abord** : ce sont les moins coûteux (une journée à deux) et ils
suppriment une classe entière de pannes en cascade, qui rendent tous les
autres diagnostics plus difficiles. Tant qu'un refus peut passer pour un
succès, aucune mesure n'est fiable.

**B1 ensuite** : il ferme le défaut le plus grave — un défaut qu'aucune surface
ne permettait de voir — et il débloque B4 et B12, qui s'appuient sur ses
groupes de parole.

**B5 avec A5** : même classe, même fichier, à prendre d'un bloc.

**B7 puis B6** : B7 donne le contrôle qui aurait attrapé le refus de
livraison ; B6 rend fiable la lecture rapide qui sert entre deux contrôles.

**B8, B9, B10, B11** sont indépendants, courts, et gagnables en parallèle.
B9 mérite d'être pris tôt malgré sa taille : il supprime un risque de
destruction silencieuse.

**B4 et B12** en dernier, une fois B1 en place.

---

## Montage autonome avec Claude Code via MCP — IA-2026-09

L'usage prioritaire est Claude Code connecté au serveur MCP du projet. La
qualité recherchée est celle du film : un propos compréhensible, fidèle à la
personne, des raccords propres, un livrable contrôlé et peu de reprises
manuelles. Les performances du panneau de chat ne mesurent pas cet usage.

Le premier terrain d'évaluation reste l'interview, déjà documentée ici.
L'audit des 15 opérations isolées dans `AUDIT.md` valide leur pilotage ; il ne
mesure pas la réussite d'un montage entier. Aucun gain éditorial n'est acquis
sans comparaison de films complets.

| Ticket | État | Résultat attendu |
|---|---|---|
| IA1 — Transcrire le mix entendu | fait | `transcribe_timeline` applique gains, fondus et limiteur de l'export ; les réglages audio invalident son cache |
| IA2 — Procédure Claude Code actuelle | fait | Le skill `monter-une-itw` décrit une seule procédure cohérente avec les outils MCP présents |
| IA3 — Sélection contextualisée et liée à une révision | à faire | Claude choisit des idées avec leur contexte ; une sélection périmée est refusée |
| IA4 — Contrôle du montage composé | à faire | Claude inspecte le résultat aux raccords, avec le même rendu que l'export |
| IA5 — Tâches longues et reprise MCP | à faire | Analyse suivie par ID, progression, annulation et reprise sans travail dupliqué |
| IA6 — Évaluation de montages complets | à préparer avant IA3 | Mesurer l'autonomie et la qualité éditoriale à modèle, matière et brief constants |

### IA1 — Transcrire le mix entendu

Défaut reproduit : modifier le gain à −96 dB et ajouter un fondu ne changeait
ni le graphe audio de transcription ni sa clé de cache. Un texte pouvait donc
être utilisé comme contrôle alors que les mots correspondants avaient été
atténués dans le montage livré.

Les enveloppes audio et le limiteur FFmpeg sont désormais partagés entre
`Export.cc` et `TimelineTranscription.cc` via `AudioMixFilters.h`. Le mix est
assemblé en stéréo à 48 kHz avant la conversion mono/16 kHz nécessaire à
Whisper. La clé de cache versionne ce pipeline et inclut gain, fondu d'entrée
et fondu de sortie.

Validation : PCM synthétique décodé sans modèle Whisper ; durée et trou
initial conservés, gain mesuré, bords atténués, forte amplification limitée.
Chaque invalidation et le retour à l'identité initiale après restauration du
réglage sont testés séparément. Cela valide le signal soumis au modèle, pas
la fidélité de ce que Whisper transcrit : une question faible peut rester
inaudible pour le modèle tout en étant audible pour le spectateur.

### IA2 — Procédure Claude Code actuelle

Le guide `.claude/skills/monter-une-itw/SKILL.md` contenait des apprentissages
utiles et des consignes héritées qui les contredisaient : 250 ms recommandées
puis interdites, liaison A/V de `ripple_trim` décrite avant B5, refus décrits
avant B2/B3. Ces passages sont corrigés ; les conventions de livraison et
les observations propres aux tournages sont conservées.

La boucle est explicite : lire la matière avec son contexte, choisir
un propos complet, construire une timeline distincte, relire le texte monté,
contrôler le mix et les raccords, puis corriger les écarts observés. Une mesure
de cadence ou de silence n'est jamais une note de qualité narrative. Le guide
précise les limites de chaque observation et les paramètres réellement
présents dans le catalogue MCP.

Validation locale IA1/IA2 du 6 septembre 2026 : compilation et 54 suites CTest
réussies sur macOS, formatage clang-format 18 vérifié sur les fichiers C++
concernés, métadonnées du skill validées par `quick_validate.py`. Aucun
montage complet par Claude Code n'a été évalué pour cette livraison.

### IA3 — Sélection contextualisée et liée à une révision

Les spans actuels suivent les cartons de sous-titrage, autour de 42
caractères (`InterviewShort.h`), et leurs alias `S1`, `S2` sont recalculés
lors de la lecture (`InterviewShort.cc`). Ils ne constituent ni des idées
complètes ni une identité persistante.

Exposer des blocs de parole avec leur texte précédent/suivant, leurs spans
constitutifs, la source et les réserves de transcription. Garder les bornes
en `RationalTime`, calculées par le moteur. Le modèle choisit les blocs et
juge les références, causalités et transitions ; il ne reconstruit pas les
temps des mots. Ne pas transformer chaque pronom en refus déterministe.

Associer chaque sélection à la timeline et à une révision du document et des
transcripts utilisés. Si une insertion, une suppression ou un réalignement
modifie le contexte, refuser la sélection avec une erreur `StaleSelection`
explicite. Un ancien `S12` ne doit jamais désigner silencieusement un autre
passage. Exposition moteur, CLI et MCP, puis surfaces graphiques concernées.

Validation : source préservée, aucun temps calculé par le modèle, sélection
ancienne refusée, plages exactes et undo canonique. Évaluation éditoriale sur
les exemples « de réhabiliter » sans sujet et « également » sans antécédent.

### IA4 — Contrôle du montage composé

Les planches actuelles décodent un rush par cellule et appliquent le format
et la transformation couleur globale (`TimelineSheets.cc`). Elles renseignent
sur le contenu source ; elles ne prouvent pas le résultat multicouche, les
effets de clip, les transitions et les légendes de l'export.

Ajouter une commande moteur/CLI/MCP de rendu d'une fenêtre courte autour d'un
raccord, issue du pipeline d'export : images composées, positions exactes,
transcription du mix et mesures audio. Signaler les différences entre texte
attendu et retranscrit comme des alertes à examiner. Corriger auparavant la
prise en compte des effets couleur dans l'export.

Validation : fixtures avec deux couches, opacité, fondu, légende, gain audio,
question faible suivie d'une réponse forte et mot traversant une coupe.
Comparer la fenêtre au même intervalle exporté. Sur les films réels, compter
les mots coupés, questions résiduelles et raccords repris par le monteur.

### IA5 — Tâches longues et reprise MCP

Exposer les analyses via `MediaTaskManager` : lancement donnant un `task_id`,
lecture du statut/progrès/résultat, annulation et reprise après déconnexion.
Les analyses travaillent sur des snapshots identifiés ; leurs résultats ne
s'installent pas sur un autre état. Les écritures restent sérialisées.

Pour les mutations susceptibles d'être répétées après perte de réponse,
ajouter révision attendue et clé d'idempotence. Un appel rejoué doit retrouver
son résultat, pas créer une deuxième timeline. Faire respecter le verrou par
tous les écrivains, et borner l'attente sur une connexion HTTP inactive.

Validation : déconnexion pendant transcription puis reprise sans relancer le
modèle, annulation bornée, réponse perdue après création sans doublon,
résultat d'analyse périmé identifié explicitement.

### IA6 — Évaluation de montages complets

Constituer un petit corpus fixe à partir de projets de test autorisés, sans
committer de rushes : brief, transcriptions, annotations des passages utiles,
pièges connus et procédure reproductible. Inclure les causes gardées ou
coupées, références pendantes, question non transcrite, horodatages décalés,
plans montrant une bouche qui parle et chevauchements de pistes.

Comparer à modèle et brief identiques : première version acceptée ou non,
interventions humaines, erreurs d'outils, reprises, durée et coût observés.
Faire juger les films sans indiquer la variante utilisée : compréhension,
fidélité du sens, ouverture autonome, transitions, rythme et lisibilité.
Conserver les désaccords éditoriaux au lieu de les masquer derrière une note
automatique. Répéter les cas pour distinguer progrès et variance du modèle.

Ordre : IA1/IA2 terminés, établir la référence IA6, puis IA3 et IA4. IA5 peut
avancer en parallèle. Mesurer chaque changement avant d'en attribuer le gain
à un nouveau modèle, à un prompt plus long ou à davantage d'appels d'outils.
