# Spécification Médiathèque et chutiers

Le panneau reprend les conventions communes du Project panel de Premiere et du
Media Pool de Resolve, adaptées au modèle journalisé de CUTMACHINE.

## Comportement livré

- La colonne de gauche est une arborescence de chutiers imbriqués.
- `Tous les médias` donne une vue globale ; `Sans chutier` montre la racine.
- Créer un chutier lorsqu'un chutier est sélectionné crée un enfant.
- Un chutier ne peut être supprimé que s'il ne contient ni média ni enfant.
- Un sélecteur bascule entre une vue liste (nom, codec/résolution, durée) et
  une grille d'icônes, comme les modes de présentation du Finder.
- La recherche filtre immédiatement sur le nom, chemin, codec, orientation et
  dimensions, sans modifier le document.
- Un média sélectionné peut être déplacé dans le chutier courant. Le
  déplacement émet `SetMediaBinOperation` ; création et suppression utilisent
  `AddBinOperation` et `RemoveBinOperation`.
- `Cmd+Z`/`Cmd+Shift+Z` annulent et rejouent toutes ces mutations.
- Le clic droit sur un chutier permet de créer un enfant, le renommer ou le
  supprimer. `RenameBinOperation` rend le renommage sérialisable et annulable.
- Le clic droit sur un média permet de l'ouvrir dans Source, le déplacer vers
  le chutier sélectionné ou révéler son fichier dans le Finder.
- Un double-clic, ou le bouton `Source`, charge le média dans le moniteur Metal
  sans déplacer le playhead de programme.
- Un drag depuis la liste ou la grille vers une piste vidéo émet une unique
  `InsertClipOperation` avec `source_in = 0` et la durée source exacte.

Le choix hiérarchique suit les bins imbriqués du
[Media Pool Resolve](https://documents.blackmagicdesign.com/UserManuals/DaVinci_Resolve_14_Reference_Manual.pdf)
et le fait que les chutiers Premiere peuvent contenir médias et autres
chutiers selon la
[documentation Adobe](https://helpx.adobe.com/sg/premiere/desktop/organize-media/file-organization/bins-overview.html).
La vue liste et sa recherche s'appuient sur les comportements documentés du
[Project panel](https://helpx.adobe.com/premiere/desktop/get-started/customize-the-project-panel/customize-list-view-in-project-panel.html)
et de la
[recherche Premiere](https://helpx.adobe.com/premiere/desktop/organize-media/file-organization/search-options-in-premiere.html).

## Modèle JSON

Un `DocumentBin` possède un ULID, un nom et un `parent_id` facultatif. Un
`parent_id` vide désigne le premier niveau. Le validateur refuse les parents
inconnus, les auto-références et tout cycle. L'identité d'un média ne change
jamais lorsqu'il est reclassé.

```json
{"id":"01K…","name":"Jour 01","parent_id":"01J…"}
```

`parent_id` est omis lors de la sauvegarde pour les chutiers racine afin de
rester compatible avec les documents existants.

## Suite prévue

1. Glisser-déposer de médias et de chutiers dans l'arborescence, via une
   opération `SetBinParent` atomique.
2. Couleurs de labels et renommage direct dans l'arborescence.
3. Remplacer l'icône vidéo générique par une poster frame et un hover scrub.
4. Smart bins fondés sur des règles de métadonnées, sans dupliquer les médias.
5. Colonnes triables : cadence, orientation, audio, usage en timeline et
   statut online/offline.

Les Smart Bins resteront des vues calculées : leur contenu ne sera pas écrit
comme une seconde appartenance physique dans le document.
