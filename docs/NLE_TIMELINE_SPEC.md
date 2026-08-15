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

État : move, trim, delete et blade liés sont implémentés. Le blade coupe tous
les membres A/V qui contiennent le raccord dans une transaction unique, puis
crée deux groupes stables distincts pour les segments gauches et droits.

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

État : `track_lock` est implémenté et persisté sous `locked`. Le cadenas de
l’en-tête émet une opération `SetTrackLock` annulable ; insert, trim, move,
clear, ripple, blade, séparation audio et suppression de piste sont refusés au
niveau du moteur lorsqu’une piste concernée est verrouillée. `sync_lock` est
persisté séparément (actif par défaut), modifié par une opération annulable et
affiché par la cellule « chaîne » de l’en-tête. Le ciblage de piste est un état
de session propre à chaque timeline : Clear ne touche que les pistes ciblées ;
Ripple Delete touche les pistes ciblées et entraîne les autres pistes dont le
sync lock est actif. Un verrou dur reste toujours prioritaire.

### Montage Source vers Record

Les moniteurs conservent des zones In/Out indépendantes. Un clic dans Source
l’active, permet de scrubber horizontalement et route `I`, `O` et `Alt+X` vers
la zone Source ; un clic dans Record ou la timeline route de nouveau ces
commandes vers la timeline. Les flèches déplacent la tête Source d’une image
(`Shift` : dix images).

`,` effectue un Insert atomique au point Record : la piste vidéo ciblée reçoit
le rush et le temps est ouvert sur les autres pistes non verrouillées en sync
lock. `.` effectue un Overwrite atomique de la même durée sans déplacer
l’aval. Une zone Record In/Out peut fournir la durée manquante d’une zone
Source ; quatre points de durées incompatibles sont refusés explicitement.
Une source contenant du son crée toujours un clip séparé sur la piste audio
ciblée (ou sur une piste audio créée au besoin). Le clip vidéo reste muet ; la
paire partage un groupe lié et une référence de synchronisation exacte.

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
Chaque outil possède une opération dédiée avec snapshots exacts des pistes
affectées ; aucun n'est simulé par plusieurs appels UI.

État : trim normal simple et lié, ripple trim (`Shift`-drag), roll edit
(`Cmd`-drag) et slip (`Y`) sont implémentés, y compris l'aperçu multipiste,
les limites source, le sync lock lorsque pertinent et l'undo/redo atomique.
Ordre restant : slide.

## Priorités d'implémentation

### P0 — cohérence de montage

- Blade lié atomique avec nouveaux groupes gauche/droite.
- `ClearClips` et `RippleDeleteClips` multi-sélection.
- `track_lock`, `sync_lock` et ciblage, avec contrôles dans les en-têtes Metal.
- Navigation raccord précédent/suivant et nudge de sélection d'une image ou
  d'un échantillon.
- Étendre le montage Source vers Record au patch audio explicite.

### P1 — trim professionnel

- Ripple trim et roll edit : implémentés.
- Slip : implémenté ; slide : à implémenter.
- Feedback de curseur par type de trim : implémenté ; sélection persistante
  d'un raccord : à implémenter.
- Track targeting pour déterminer les pistes concernées par navigation,
  insert et match frame.

### P2 — assemblage et monitoring

- Lift/extract sur une plage In/Out.
- Mute/solo/arm audio, niveaux et fondus de clip.
- Marqueurs de timeline et de clip.

### Transitions vidéo — P0 livré

Le fondu enchaîné est un `DocumentTransition` appartenant à la séquence et
référençant deux clips vidéo adjacents sur une même piste. Les rectangles de
clips restent contigus et non superposés ; la transition lit leurs poignées
source avant/après le cut. Sa durée est alignée sur des images de la séquence,
avec alignement centré, début au cut ou fin au cut. Toute poignée insuffisante,
référence audio, piste verrouillée ou raccord non contigu est refusé par le
moteur. `AddTransition`, `UpdateTransition` et `RemoveTransition` sont
sérialisables, annulables et utilisables par le CLI. Le moniteur Metal et
l’export FFmpeg appliquent le même fondu, sans mutation ni transition audio
implicite.

### Infrastructure média livrée en parallèle

- Scheduler borné et annulable pour probe, proxy, waveform, thumbnail et
  relink ; tous ces types sont maintenant raccordés à l'application.
- Proxies ProRes Proxy vidéo seuls, activation globale et repli automatique
  vers l'original.
- Waveforms audio mises en cache hors du document et rendues en tenant compte
  du `source_in` et du niveau de zoom.
- Thumbnails vidéo orientées et letterboxées, mises en cache hors du document
  pour la vue grille des chutiers.
- Relink transactionnel conservant l'identité du rush et invalidant tous les
  caches dérivés après contrôle de cadence, durée et présence audio.
- Relink en lot récursif des médias offline par nom de fichier, avec détection
  explicite des correspondances ambiguës et commit atomique du lot compatible.
- L'original reste la source exclusive de l'audio et de l'export final.

Hors du prochain cycle : effets, titrage, multicam, keyframes et
scopes couleur. Chacun nécessite un modèle métier avant son interface.

## Arrondi temps/pixels

`XToTime(x, rate)` calcule en `long double`, multiplie par le timebase demandé,
puis arrondit au tick le plus proche avec les demi-ticks à l'opposé de zéro.
Le résultat est donc exactement sur la grille demandée. Pour `30000/1001`, on
demande le timebase `30000` et une frontière d'image est un multiple de `1001`
ticks. Le choix est symétrique pour un `view_start` négatif et rend
`XToTime(TimeToX(t), t.rate)` inverse à un tick près, sans laisser un temps de
document transiter en `double` hors du viewport.
