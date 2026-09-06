# Checklist de vérification visuelle — Phase 2 (UI)

Tout ce qui suit a été écrit et vérifié en C++ pur (compilation, tests,
déterminisme) dans un sandbox Linux sans Metal ni AppKit. Rien n'a jamais
été rendu à l'écran. Cette checklist est à parcourir sur la première machine
macOS disponible, avant de considérer la Phase 2 comme terminée pour de bon.

Coche au fur et à mesure ; note l'écart à côté de chaque item qui échoue
plutôt que de le corriger à la volée — certains touchent du code partagé
entre plusieurs tickets.

## 0. Build

- [ ] `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j` réussit sans warning nouveau
- [ ] `ctest --test-dir build --output-on-failure` passe en entier, y compris les suites jamais exécutées en Linux : `cutmachine_cli_tests`, `cutmachine_mcp_tests`, `cutmachine_audio_playback_tests`
- [ ] `build/CUTMACHINE.app` se lance depuis le Finder sans crash au démarrage
- [ ] Le binaire CLI `./build/cutmachine` fonctionne toujours sans argument (page d'accueil) et avec `--apply-op`/`--mcp-serve`

## 1. Design system — cohérence globale (F2.1)

- [ ] Les couleurs de la timeline (déjà existante) et des nouveaux panneaux sont visuellement identiques là où elles sont censées l'être (accent cyan focus/snap, vert/orange in-out, ambre sélection) — `UiTheme.h` est la seule source, une divergence ici est un bug de câblage, pas de goût
- [ ] Pas de flash/pop de couleur au changement d'onglet ou au redimensionnement de fenêtre
- [ ] Dock droit (Inspector/Chat) : largeur fixe à 300pt, ne se redimensionne pas au drag (c'est voulu — vérifier que ça ne semble pas cassé/figé à l'usage)
- [ ] Dock bas (Transport) : hauteur fixe à 84pt, bien collé au-dessus de la barre de statut, sans chevauchement avec la timeline Metal
- [ ] **Point cosmétique déjà signalé par l'agent F2.5** : le dock bas utilise le même chrome `CMPanelHostView` à onglets que le dock droit, alors qu'il n'a qu'un seul slot (Transport) — un bandeau d'onglet "Lecture" de 26pt se retrouve au-dessus des contrôles de lecture. Juger à l'usage si c'est gênant ; si oui, c'est un correctif localisé à `PanelHostView`/`main.mm`, pas une réouverture de ticket
- [ ] Contraste texte/fond suffisant en usage prolongé (`kTextTertiary` à 42% de luminance sur `kSurfaceBase` à 5.5% — c'est le rapport le plus faible du système, vérifier qu'il reste lisible et pas juste "conforme sur le papier")
- [ ] Aucun texte ni contrôle ne dépend d'un thème clair — l'app est sombre uniquement, vérifier qu'aucune vue système (NSAlert, NSOpenPanel...) ne détonne trop

## 2. Inspector (F2.2)

- [ ] Sélectionner un clip vidéo affiche ses propriétés (nom, piste, début, durée) correctement formatées
- [ ] Sélectionner un clip audio, un clip image, zéro clip, plusieurs clips : chaque état a un affichage cohérent (pas de propriétés vidéo affichées sur un clip audio, pas de crash sur sélection multiple)
- [ ] Les 8 sliders de grading (exposure/contrast/saturation/vibrance/temperature/tint/highlights/shadows) reflètent l'état réel du clip sélectionné, y compris "aucun grade appliqué" (valeurs neutres)
- [ ] Glisser un slider : le label de valeur suit en direct, mais le commit réel (undo-able) n'arrive qu'après un court silence (~120ms) — vérifier que ça ne "rate" pas de valeurs intermédiaires importantes et que l'undo après un drag restitue bien l'état d'avant-drag, pas un état intermédiaire
- [ ] Changer de clip sélectionné pendant un drag en cours n'applique pas le commit en attente au mauvais clip (le debounce doit s'invalider au changement de sélection)
- [ ] `Cmd+Z` après un ajustement de grading annule proprement et remet le slider à sa position précédente
- [ ] **Vérifier le rendu réel du grading** (pas juste l'UI) : appliquer exposure/temperature/etc. sur un clip et confirmer visuellement que l'image change dans le moniteur — c'est la partie jamais testée du tout (kernels Metal + `fragment_working`), la plus à risque de tout F1.3/F2.2
- [ ] Layout de `ClipGradeParameters`/`GradeEntry` : si l'image sort avec des valeurs de grade visiblement incohérentes/aléatoires (un exposure qui affecte la saturation, par ex.), suspecter en premier le désalignement mémoire entre `Renderer.mm` et `shader.metal` signalé comme "relu mais jamais compilé" dans le rapport F1.3

## 3. Media panel (F2.3)

- [ ] Onglet **Média** : le panneau bins/médiathèque préexistant fonctionne toujours à l'identique (rien régressé par le passage sous `CMTabStripView`)
- [ ] Onglet **Audio** : ne liste que les médias avec piste audio (`has_audio`), recherche et filtrage par bin fonctionnent
- [ ] Onglet **Légendes** : liste les `CaptionStyle` existants, création/suppression fonctionne, appliquer un style à la sélection courante de la timeline produit bien des clips caption visibles
- [ ] Changer d'onglet ne perd pas l'état de sélection/scroll des deux autres onglets
- [ ] Le résumé "sélection courante" dans l'onglet Légendes se met à jour même quand cet onglet n'est pas affiché (vérifier qu'il est bien à jour au moment où on y revient, pas juste qu'il ne crash pas)

## 4. Chat panel (F2.4)

- [ ] Sans clé API dans l'environnement : le panneau indique clairement l'absence de clé plutôt que d'échouer silencieusement ou de crasher
- [ ] Avec une clé Anthropic valide (`ANTHROPIC_API_KEY` ou équivalent — vérifier le nom exact de variable utilisé dans `ChatLlmClient.cc`) : une instruction simple ("coupe ce clip en deux") produit un appel d'outil visible dans la transcription, puis une mutation réelle de la timeline
- [ ] La mutation déclenchée par le chat apparaît **identique** à une mutation équivalente faite à la souris — même undo, même log, aucune trace de "deux systèmes" différents
- [ ] Une instruction ambiguë ou un ID de clip inexistant renvoie une erreur lisible dans le chat, pas un crash
- [ ] Le panneau ne fait aucun appel réseau tant qu'aucune clé n'est configurée (vérifier avec un outil réseau/proxy si besoin — c'est le garde-fou "pas de service central")
- [ ] Fermer/rouvrir le panneau (changer d'onglet et revenir) ne perd pas l'historique de conversation en cours

## 5. Transport (F2.5)

- [ ] Play/pause, step avant/arrière, scrub bar : tous reflètent et pilotent la **même** position de playhead que le raccourci clavier et le clic direct sur la timeline (pas de désynchronisation possible)
- [ ] Le timecode affiché dans le dock bas et celui affiché dans le bandeau d'info sous la timeline sont toujours identiques (même formule, `Timecode.h`)
- [ ] Scrub rapide (drag continu sur la barre) reste fluide, pas de lag perceptible
- [ ] Mode NTSC (29.97 / 30000:1001) : le timecode ne dérive pas après plusieurs minutes de lecture
- [ ] **Passage d'un plan au suivant en lecture (PERF-2026-09)** : sur un montage à deux pistes vidéo (interview en V1, plans de coupe en V2), lire à travers l'entrée *et* la sortie de chaque plan de coupe. Aucune image de la piste du dessous ne doit apparaître à la jonction ; en cas de décodeur en retard, le moniteur **garde l'image précédente** (jusqu'à une demi-seconde) au lieu de laisser un trou. Vérifier aussi en lecture arrière et en ×2/×4, et sur un plan de coupe jamais lu depuis l'ouverture du projet (cache froid) — c'est le cas qui produisait le flash
- [ ] Un vrai trou dans le montage (V2 sans plan à cet endroit) laisse bien voir V1, immédiatement : le maintien d'image ne doit pas retarder ce cas-là

## 6. Interaction croisée entre panneaux

- [ ] Un edit fait via le chat (F2.4) rafraîchit correctement l'Inspector (F2.2) si le clip édité est celui sélectionné
- [ ] Un edit fait à la souris rafraîchit l'Inspector sans action supplémentaire
- [ ] Appliquer un style de légende (Media panel) puis sélectionner ce clip : l'Inspector affiche l'état cohérent (pas de conflit entre les deux panneaux sur le même clip)
- [ ] Aucun des quatre panneaux ne bloque l'UI pendant une opération longue (génération de transcript, détection de beats) — vérifier qu'il y a un minimum de feedback de chargement, même basique

## 7. Cas volontairement laissés de côté (à ne pas confondre avec des bugs)

- Roues/courbes de grading : pas implémentées côté moteur (F1.3), donc absentes de l'Inspector — normal, pas un oubli
- Recherche sémantique média, détection de beats exposée en UI : hors scope de cette passe (voir ROADMAP.md, "Explicitement hors scope")
- Disposition des panneaux non réarrangeable, pas de thème clair : voulu (`PHILOSOPHY.md`, non-but sur l'état d'interface persistant)
