# Brief technique pour une passe de design

Document à donner tel quel à un outil de design. Il décrit ce que l'application
peut réellement dessiner, avec quelles primitives et dans quelle géométrie —
pour qu'une proposition de design soit implémentable sans réécriture du moteur
de rendu.

**Ce que c'est** : CUTMACHINE, un banc de montage vidéo (NLE) macOS natif. Un
moteur d'édition C++ et plusieurs surfaces qui l'exposent — interface
graphique, ligne de commande, serveur MCP pour agent. Sombre uniquement,
interface en français.

---

## 1. Stack

| Couche | Technologie |
|---|---|
| Langage | C++17 + Objective-C++ (ARC). Aucun Swift. |
| Chrome d'interface | AppKit — `NSView`, layout en frames + autoresizing masks, `NSSplitView`. **Aucun Auto Layout, aucun storyboard, aucun SwiftUI.** |
| Timeline et moniteurs | Metal, `CAMetalLayer` custom, un seul fichier `shader.metal` |
| Médias | FFmpeg (libavformat / libavcodec / libavutil / libswresample), AVFAudio pour la lecture audio |
| Transcription | whisper.cpp v1.7.4, local |
| Agent | serveur MCP natif (HTTP + JSON-RPC) dans le binaire, panneau de chat BYOK |
| Build | CMake 3.24+, bundle `com.cutmachine.editor`, macOS uniquement |

---

## 2. Contraintes de rendu — la section qui décide de ce qui est dessinable

**Il y a deux systèmes de rendu aux capacités très différentes.** Une
proposition qui les confond n'est pas implémentable.

### Système A — les surfaces Metal (timeline, moniteur source, moniteur programme)

Le rendu d'interface passe par un unique pipeline `vertex_solid` /
`fragment_solid` qui consomme une display list de rectangles. Sa primitive
unique :

```
add(x, y, largeur, hauteur, r, g, b, a)   // rectangle plein, aligné sur les axes
```

Ce qui **n'existe pas** aujourd'hui dans ce système :

- **le texte** — seulement un afficheur 7 segments fait de rectangles, qui ne
  connaît que les chiffres et `f s m p - +` ;
- les coins arrondis, les contours, les ombres, les dégradés ;
- l'antialiasing, la rotation, le clipping, le flou ;
- les icônes vectorielles — les symboles existants (œil, cadenas, solo, muet)
  sont trois ou quatre rectangles placés à la main.

Configuration de la couche : `MTLPixelFormat BGRA10_XR`, espace colorimétrique
ITU-R 709, bascule ITU-R 2100 HLG + EDR quand la séquence est en HDR.

### Système B — les panneaux AppKit (médiathèque, inspecteur, agent, transport)

Vues AppKit standard restylées : `NSTextField`, `NSButton`, `NSSlider`,
`NSOutlineView`, `NSTableView`, `NSSearchField`. Donc ici : vrai texte système,
vraies polices, vrai antialiasing. Les composants réutilisables existants sont
un conteneur à en-tête titré, une ligne libellé/contrôle/valeur, un en-tête de
section, des boutons et sliders stylés, et une barre d'onglets.

**Conséquence pour le design** : ce qui est proposé pour un panneau peut
supposer du texte et des formes ; ce qui est proposé pour la timeline ou les
moniteurs doit soit tenir en rectangles pleins, soit indiquer explicitement
quelle primitive nouvelle il faut d'abord implémenter (rendu de texte par atlas
de glyphes, formes SDF pour les coins arrondis et l'antialiasing).

---

## 3. Géométrie actuelle — le canevas à redessiner

Fenêtre par défaut **1600 × 960**, taille minimale 900 × 560, redimensionnable.

```
┌──────────────────────────────────────────────────────────────────┐
│  Médiathèque   │      Moniteur source │ Moniteur programme       │  ← 44 %
│    320 pt      │         (Metal)      │      (Metal)             │
│                ├──────────────────────┴──────────────────────────┤   Inspecteur
│  onglets 26pt  │                                                 │   / Agent
│  Média         │           Timeline (Metal)                      │   300 pt
│  Audio         │                                                 │  ← 56 %
│  Légendes      │                                                 │   onglets 26pt
├────────────────┴─────────────────────────────────────────────────┤
│  Transport (lecture, scrub, timecode)                     84 pt   │
├──────────────────────────────────────────────────────────────────┤
│  Barre de statut                                          42 pt   │
└──────────────────────────────────────────────────────────────────┘
```

- Colonnes gérées par un `NSSplitView` vertical à trois volets, diviseurs fins,
  géométrie mémorisée par AppKit (`autosaveName`). Les largeurs 320 / 300 sont
  nominales, pas verrouillées par du code.
- Zone centrale : un second `NSSplitView` (timeline 56 % / paire de moniteurs
  44 %), les deux moniteurs à 50/50.
- Dock bas et barre de statut : hauteurs fixes, épinglées au bas de la fenêtre.
- Chaque dock à onglets réserve une bande de 26 pt en haut.

**Métriques de la timeline** : règle 28 pt, hauteur de piste 44 pt, largeur de
l'en-tête de piste 72 pt, ligne « ajouter une piste » 22 pt, distance
d'aimantation 8 pt, zone de saisie d'un bord de clip 6 pt. Séquence par défaut
1920 × 1080.

---

## 4. Design system existant

Source unique : `src/UiTheme.h`. Les valeurs flottantes font foi (elles vont
directement dans Metal) ; les hexadécimaux ci-dessous en sont l'équivalent
approché pour un outil de design.

**Surfaces** (du plus sombre au plus clair)

| Rôle | Hex |
|---|---|
| Fond de base | `#0E0E0F` |
| Panneau | `#131314` |
| Surélevé | `#18191A` |
| Ligne paire / impaire | `#121213` / `#141415` |
| Contrôle | `#212224` |
| Contrôle actif | `#295775` |

**Bordures** : discrète `#212222`, marquée `#424247`.

**Texte** : primaire `#E6E6EB`, secondaire `#999CA1`, tertiaire `#6B6E73`.
*(Le tertiaire sur le fond de base donne un contraste d'environ 3,8:1 — sous le
seuil WCAG AA de 4,5:1 pour du texte courant. À corriger ou à réserver au texte
large.)*

**Accents**, avec leur signification déjà établie dans la timeline :

| Couleur | Hex | Sens |
|---|---|---|
| Cyan | `#3DD1FF` | focus, aimantation, onglet actif |
| Vert | `#33E085` | point d'entrée, positif |
| Orange | `#FF6B3D` | point de sortie |
| Ambre | `#F2C72E` | sélection valide |
| Rouge | `#DB291F` | déplacement en cours, erreur |
| Bleu piste | `#1F6EAB` | piste / clip vidéo |
| Vert piste | `#217A47` | piste / clip audio |

**Échelle d'espacement** (points) : 2, 4, 8, 12, 16, 24, 32.

**Échelle typographique** (points) : 10 légende, 11 petit, 12 corps, 13
section, 15 titre. Police système (SF), chiffres à chasse fixe pour les valeurs
numériques et le timecode.

**Métriques de composants** : en-tête de panneau 28 pt, ligne de contrôle
24 pt, barre d'onglets 26 pt, rayon de coin 4 pt *(déclaré, mais inapplicable
côté Metal aujourd'hui)*.

---

## 5. Contraintes produit

- **Sombre uniquement.** Pas de thème clair, y compris futur.
- **Interface en français.**
- L'état d'interface (onglet actif, géométrie des volets) est une préférence
  locale de la machine, jamais une donnée du projet.
- La composition des docks est fixe : un panneau par emplacement, pas de
  réarrangement par l'utilisateur.
- Le rendu étant intégralement dessiné, rien n'oblige à reprendre l'esthétique
  par défaut des logiciels de montage existants. C'est un choix assumé du
  projet, pas une contrainte à contourner.

---

## 6. Trous connus, utiles à combler par le design

- **Le survol ne se voit pas.** Il est pourtant détecté dans la timeline : le
  curseur change (redimensionnement sur un bord de clip, main, loupe, ciseaux)
  et la barre de statut annonce le geste disponible (« TRIM · glisser le
  raccord », « ROLL EDIT · Cmd-glisser »). Mais **aucun élément ne change
  d'apparence sous le pointeur**, et les panneaux AppKit n'ont aucun survol du
  tout — il n'existe pas un seul `NSTrackingArea` dans le projet. Les états à
  spécifier explicitement : repos, survol, actif/pressé, sélectionné,
  désactivé, focus clavier.
- **Aucun nom de clip lisible dans la timeline**, faute de rendu de texte.
- **Aucun retour de chargement** pendant les opérations longues
  (transcription, détection de rythme, génération de proxies).
- Les icônes de piste (œil, cadenas, solo, muet, verrou de synchro) sont des
  amas de rectangles et ne se lisent pas comme des symboles.

---

## 7. Forme attendue en sortie

Un mockup HTML/CSS ne se traduit pas mécaniquement vers des rectangles Metal et
des frames AppKit : ni flexbox, ni `border-radius`, ni `box-shadow`, ni rendu
de texte n'ont d'équivalent direct. Un mockup reste une **référence visuelle**,
pas une source à porter.

Ce qui est directement exploitable :

1. des **tokens** — couleurs, espacements, tailles — sous forme de valeurs
   nommées, pour remplacer celles de `UiTheme.h` ;
2. de la **géométrie** en points, rectangle par rectangle pour les surfaces
   Metal ;
3. les **états** énumérés pour chaque élément interactif ;
4. la **liste explicite des primitives nouvelles** que le design exige (rendu
   de texte, coins arrondis, ombres…), pour qu'elles soient implémentées avant
   et non devinées pendant ;
5. des **captures de référence**, comme cible de comparaison visuelle.

Dernier point à savoir : **rien de cette interface n'a jamais été affiché à
l'écran.** Toute la couche de panneaux a été écrite et testée sans macOS, donc
sans rendu. Les proportions décrites ici sont celles du code, pas celles d'une
capture — voir `VISUAL_QA_CHECKLIST.md`.
