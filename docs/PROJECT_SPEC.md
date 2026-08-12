# Objet projet

La disposition des fichiers, les règles de partage, les limites de concurrence
et la cible de portabilité sont spécifiées dans
[`PROJECT_STORAGE_SPEC.md`](PROJECT_STORAGE_SPEC.md).

`Project` est l’agrégat d’un montage, comparable au projet de Premiere ou à la
bibliothèque de projet de Resolve. Une instance possède :

- une identité et un nom stables ;
- les rushes et chutiers partagés ;
- les sources montables ;
- les réglages communs, notamment la gestion de couleur ;
- une ou plusieurs timelines et l’ULID de la timeline ouverte par défaut.

Les dimensions et la cadence restent des réglages de timeline : deux exports
horizontal et vertical peuvent donc partager les mêmes rushes sans dupliquer le
projet.

Le moteur de timeline édite un `Document`, snapshot d’une timeline
adressée et des ressources partagées. `MakeDocument(timeline_id)` ouvre ce
contexte ; `CommitDocument(timeline_id, document)` le valide puis le réintègre
au projet. `MakeActiveDocument()` et `CommitActiveDocument()` adressent la
timeline active sans introduire un second format de persistance.

Cette frontière est maintenant utilisée par l’application. `AppState` possède
le `Project` complet et un `Document` focalisé. Les timelines sont affichées
comme des objets du chutier ; un double-clic sélectionne leur document de
montage, tandis que `Cmd+Option+N` en crée une au format de la timeline
courante. La sélection courante, la position de lecture et l’undo de chaque
timeline sont conservés séparément pendant la session.

Sélectionner une timeline est une navigation, comme déplacer le playhead : ce
n’est ni une `ProjectOperation`, ni une entrée d’undo, ni une réécriture du
fichier projet. `active_timeline_id` reste dans le format v2 comme choix
d’ouverture par défaut, mais
`AppState::activeTimelineId` porte la sélection de session.

## Opérations et historique projet

Les mutations persistantes au-dessus d’une timeline utilisent un second
journal, `ProjectEditLog`, indépendant du `EditLog` de montage :

- `AddProjectTimelineOperation` et `RemoveProjectTimelineOperation` ajoutent ou
  retirent une timeline ; les ULID de timeline et de pistes sont générés à la
  première application puis conservés au redo ;
- `SetProjectBinMetadataOperation` modifie description, note, tags et ordre
  d’un rush ou d’une timeline ;
- `SetProjectTimelineBinOperation` classe une timeline dans un chutier ;
- `RelinkProjectMediaOperation` reconnecte atomiquement un ou plusieurs rushes
  partagés par toutes les timelines et invalide leurs proxies.

Chaque opération est validée sur une copie, sérialisable et enrichie d’un
snapshot canonique exact. Undo restitue donc les octets projet d’origine et
redo conserve toutes les identités. L’application route `Cmd+Z` et
`Cmd+Shift+Z` vers le journal timeline ou projet concerné. Les commandes
headless `--apply-project-op`, `--undo-project-op` et `--redo-project-op`
emploient exactement le même moteur et le sidecar
`<projet>.project-editlog.json`.

## Persistance

Le fichier projet utilise exclusivement l’enveloppe `cutmachine-project`,
version 2. Elle
conserve l’identité du projet, la timeline active, toutes les timelines, les
métadonnées du chutier et leur placement. Chaque snapshot de timeline reste un
`Document` canonique version 3, afin que le moteur, le CLI et l’export utilisent
toujours le même format validé.

Les fichiers document autonomes, enveloppes projet v1 et packages v1 sont
refusés. Il n’existe plus de promotion ni de migration implicite.

Avant chaque commit, une autosave atomique du projet complet est écrite. Elle
n’est supprimée qu’après réussite du remplacement transactionnel du projet, du
journal de la timeline focalisée et du journal projet. Au lancement, une
autosave valide plus récente déclenche un choix explicite de récupération.

## Modèle de chutier

Le chutier suit la séparation utilisée par Kdenlive entre un arbre de données
et sa projection filtrée. `ProjectBinModel` expose dans une même hiérarchie les
dossiers, rushes et timelines, sans dépendre d’AppKit. Les métadonnées de
présentation (description, note de zéro à cinq, tags et ordre d’insertion)
restent au niveau projet et ne polluent pas les données techniques du média.

Le filtrage accepte les tags, notes et types en logique **OU** à l’intérieur
d’une catégorie et combine les catégories en logique **ET**. Un dossier reste
visible si un descendant correspond. Les filtres utilisé/inutilisé sont
calculés à partir de toutes les timelines, avec un compteur séparé pour la
timeline active. Le tri naturel place donc `Clip 2` avant `Clip 10` et garde les
dossiers avant leurs éléments.
