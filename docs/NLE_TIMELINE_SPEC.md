# Spécification de montage timeline

Cette spécification traduit les conventions utiles de Premiere et Resolve dans
le modèle exact et journalisé de CUTMACHINE. Elle décrit des comportements,
pas une copie d'interface.

## Invariants

- Un geste de montage produit zéro ou une opération dans l'event log.
- Une opération multi-clip est atomique : si un membre échoue, le document et
  le log restent identiques octet pour octet.
- Tous les temps de montage sont des `RationalTime`. Seul
  `TimelineViewport` convertit temps et pixels.
- `Option` au début d'un geste inverse temporairement la sélection liée.
- L'aperçu peut être invalide et rouge, mais ne mute jamais le document.
- Overwrite, clear et ripple delete sont trois commandes distinctes.

## Menus et contexte

La barre macOS regroupe les commandes stables dans `Édition`, `Clip`,
`Timeline` et `Lecture`. Les menus contextuels sont construits au clic : leur
contenu dépend du panneau actif et de l'objet sous le pointeur, conformément à
la distinction entre menu de panneau et menu contextuel décrite par
[Premiere](https://helpx.adobe.com/premiere/desktop/get-started/tour-the-workspace/display-panel-options-and-menu.html).

- Média : ouvrir dans Source, déplacer dans le chutier courant, révéler dans
  le Finder.
- Chutier : créer un enfant, renommer, supprimer s'il est vide.
- Clip : ouvrir dans Source, retrouver dans la médiathèque, couper au point du
  clic, séparer l'audio, supprimer.
- Gap : fermer le gap.
- Piste ou fond : ajouter une piste et cadrer la timeline.

Les actions de montage passent par l'event log. Les commandes de présentation
comme `Révéler dans le Finder` ne mutent pas le document. Le raccourci affiché
dans le menu et le raccourci clavier direct appellent la même méthode.

## Sémantique retenue

### Sélection et liens A/V

Avec la sélection liée active, cliquer un membre sélectionne son groupe. Move,
trim, blade et delete doivent agir sur tous les membres concernés en une seule
transaction. Un montage indépendant conserve le décalage A/V exact et affiche
son badge. Cette règle reprend la sélection liée et son override `Option`
décrits par [Premiere](https://helpx.adobe.com/premiere/desktop/edit-projects/change-clip-sequence/select-clips.html),
ainsi que le blade appliqué aux parties audio et vidéo lorsque la sélection
liée est active dans le
[guide Resolve](https://documents.blackmagicdesign.com/UserManuals/DaVinci-Resolve-18-Editors-Guide.pdf).

État : move, trim et delete liés sont implémentés. Le blade lié est le prochain
lot ; il devra créer un nouveau groupe stable pour les segments droits afin
que les deux côtés du raccord ne deviennent pas un unique groupe de quatre
clips.

### Suppression et ripple

- `Clear` retire la sélection et laisse son intervalle libre.
- `RippleDelete` retire la sélection puis décale les éléments suivants sur les
  pistes participant au ripple.
- Supprimer un gap sélectionné ferme uniquement ce gap dans la version
  actuelle.

Premiere distingue explicitement delete et ripple delete, ce dernier décalant
les clips suivants pour fermer le trou :
[documentation Adobe](https://helpx.adobe.com/au/premiere/desktop/edit-projects/change-clip-sequence/remove-clips-from-a-sequence.html).

État : `ClearClip`/`ClearLinkedClips` laissent un blanc, tandis que
`RemoveClip`/`RemoveLinkedClips` effectuent une suppression ripple. Les deux
variantes liées sont atomiques ; `Delete` déclenche Clear et `Shift+Delete`
déclenche RippleDelete.

### Verrouillages de piste

- `track_lock` interdit toute mutation de la piste, mais pas sa lecture.
- `sync_lock` décide si la piste suit une insertion, un ripple trim ou un
  ripple delete effectué ailleurs.
- Une piste directement éditée participe toujours au ripple ; `sync_lock`
  concerne les autres pistes.

Cette distinction est conforme au
[Track Lock Adobe](https://helpx.adobe.com/ie/premiere/desktop/edit-projects/change-clip-sequence/track-lock-to-prevent-changes.html),
au [Sync Lock Adobe](https://helpx.adobe.com/premiere/desktop/edit-projects/change-clip-sequence/sync-lock-to-prevent-changes.html)
et au contrôle indépendant ajouté dans
[Resolve 20.2](https://documents.blackmagicdesign.com/SupportNotes/DaVinci_Resolve_20.2_New_Features_Guide.pdf).

État : spécifié, non implémenté. Le JSON devra porter les deux booléens et les
toggles devront émettre `SetTrackLock`/`SetTrackSyncLock`, jamais muter le
document directement.

### Outils de trim

1. Trim normal : change un bord et laisse un gap ou refuse un overlap.
2. Ripple trim : change un bord et translate les clips aval des pistes en
   sync lock.
3. Roll : déplace un raccord partagé sans changer la durée totale.
4. Slip : conserve le rectangle timeline et translate `source_in`.
5. Slide : déplace le clip en compensant les deux raccords voisins.

Le ripple trim doit décaler l'aval et mettre à jour la durée de séquence en
temps réel, comme le décrit
[Adobe](https://helpx.adobe.com/ca/premiere/desktop/edit-projects/trim-clips/perform-ripple-edits.html).
Chaque outil aura une opération dédiée avec snapshots exacts des pistes
affectées ; aucun ne sera simulé par plusieurs appels UI.

État : trim normal simple et lié implémenté. Ordre prévu : ripple, roll, slip,
slide.

## Priorités d'implémentation

### P0 — cohérence de montage

- Blade lié atomique avec nouveaux groupes gauche/droite.
- `ClearClips` et `RippleDeleteClips` multi-sélection.
- `track_lock` et `sync_lock`, avec icônes dans les en-têtes Metal.
- Navigation raccord précédent/suivant et nudge de sélection d'une image ou
  d'un échantillon.

### P1 — trim professionnel

- Ripple trim, roll edit, slip et slide.
- Sélection explicite d'un raccord et feedback de curseur par type de trim.
- Track targeting pour déterminer les pistes concernées par navigation,
  insert et match frame.

### P2 — assemblage et monitoring

- Source monitor avec In/Out, insert et overwrite vers pistes ciblées.
- Lift/extract sur une plage In/Out.
- Mute/solo/arm audio, niveaux et fondus de clip.
- Marqueurs de timeline et de clip.

Hors du prochain cycle : transitions, effets, titrage, multicam, proxies,
keyframes, scopes couleur et export final. Chacun nécessite un modèle métier
avant son interface.

## Arrondi temps/pixels

`XToTime(x, rate)` calcule en `long double`, multiplie par le timebase demandé,
puis arrondit au tick le plus proche avec les demi-ticks à l'opposé de zéro.
Le résultat est donc exactement sur la grille demandée. Pour `30000/1001`, on
demande le timebase `30000` et une frontière d'image est un multiple de `1001`
ticks. Le choix est symétrique pour un `view_start` négatif et rend
`XToTime(TimeToX(t), t.rate)` inverse à un tick près, sans laisser un temps de
document transiter en `double` hors du viewport.
