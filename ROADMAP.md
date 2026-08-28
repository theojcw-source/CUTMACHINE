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

Restent ouverts sur cet axe :

- **La transcription n'est toujours pas déclenchable par l'agent** (`--transcribe` en CLI, action de menu dans l'app). Elle demande un chemin de modèle Whisper, qui est une préférence utilisateur — à traiter comme telle avant d'exposer l'outil.
- **Aucun verrou partagé entre `--mcp-serve` et l'app.** Seule l'app prend `ProjectSessionLock` ; deux surfaces sur le même projet divergent en silence.
- **Vérification du rendu** (comparer l'export à ce que le document décrit) reste un travail manuel.

Restent ouverts et non traités ici, par ordre de gravité (voir l'audit) :

- **`Export.cc` ne lit jamais `clip.effects`.** L'étalonnage est visible au
  moniteur et absent du fichier livré. C'est un défaut de correction, et le
  premier ticket à prendre.
- **`Test copilot/` n'est pas branché** : trois suites de tests et le job CI
  `ctest.yml` sont écrits et non référencés dans `CMakeLists.txt`.
- **Aucune sortie interopérable** (OTIO/FCPXML/EDL).
- **Pas de marqueur de tour dans `EditLog`** : impossible d'annuler une
  intervention d'agent en un geste.
- **La langue n'entre pas dans l'identité du cache de transcription.**

Toutes les phases (0, 1, 2) sont closes. Reste hors roadmap : validation
visuelle réelle sur macOS (AppKit/Metal non compilables dans ce
sandbox Linux) et le fossé `ProjectStorage.cc`/CommonCrypto qui bloque
la compilation CMake complète hors macOS (préexistant, sans rapport
avec ce chantier).
