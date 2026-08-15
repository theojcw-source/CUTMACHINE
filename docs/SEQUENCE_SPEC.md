# Objet séquence

La séquence est le canevas de montage persistant du projet. Elle est distincte
des sources et ne change pas lorsqu’un média d’une autre définition ou cadence
est monté.

```json
"sequence": {
  "id": "01K…",
  "name": "Sequence 1",
  "width": 1920,
  "height": 1080,
  "frame_rate": {"num": 25, "den": 1},
  "markers": [],
  "tracks": []
}
```

- `id` est un ULID stable destiné aux futures références entre séquences.
- `width` et `height` définissent le canevas vidéo, pas la taille des sources.
- `frame_rate` est rationnel ; `30000/1001` n’est jamais remplacé par `29.97`.
- `tracks` est la timeline possédée par la séquence ; clips et pistes ne peuvent
  pas exister au niveau racine du projet.
- `markers` contient les marqueurs de cette séquence, avec leurs ULID et temps
  rationnels exacts.
- un document sans ce bloc est invalide.

Les documents de version 1 ou 2 sont refusés. Toute timeline physique utilise
la version 3 imbriquée ; aucune collection de pistes ne vit à la racine.

Les changements de nom, dimensions et cadence utilisent l'opération
`UpdateSequence`, référencée par `sequence_id`. Son inverse conserve exactement
les anciens réglages : undo/redo restaure les octets canoniques sans changer
l'ULID, les pistes ou les marqueurs appartenant à la séquence.

Le moniteur ajuste le canevas entier dans sa zone vidéo, puis ajuste chaque
source dans ce canevas. L’export utilise par défaut exactement les dimensions et
la cadence de la séquence ; les presets de livraison peuvent les surcharger.
