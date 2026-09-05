# Stockage, portabilité et partage des projets

Statut : **spécification technique normative — format neuf v2 uniquement**.

## 1. Format accepté

CUTMACHINE ouvre uniquement un package Finder `<nom>.cutmachine-project` au
format v2. Les anciens fichiers JSON autonomes, documents timeline v1/v2,
enveloppes projet v1 et packages v1 sont refusés : aucune migration implicite
n’est exécutée.

```text
Film.cutmachine-project/
├── manifest.json
├── project.cutmachine.json
├── Timelines/
│   └── <timeline-ulid>.json
├── project.cutmachine.json.timeline-<timeline-ulid>.editlog.json
├── project.cutmachine.json.project-editlog.json
├── Media/                         # présent après une collecte
└── .cutmachine/                   # caches locaux régénérables
```

`manifest.json` porte `format: "cutmachine-collection"`, `version: 2`, le
chemin du projet, l’index des timelines et l’inventaire des médias collectés.
`project.cutmachine.json` porte `project_format: "cutmachine-project"`,
`project_version: 2` et `timeline_snapshots`. Les fichiers
`Timelines/<ULID>.json`, obligatoirement en document version 3, sont l’autorité
pour le contenu des séquences ; les snapshots servent à la cohérence et à la
récupération transactionnelle.

## 2. Autorité des données

| Élément | Autorité | Requis à l’ouverture | Régénérable |
|---|---:|---:|---:|
| `manifest.json` | oui | oui | non |
| `project.cutmachine.json` | oui | oui | non |
| `Timelines/<ULID>.json` | oui | oui | non |
| médias originaux | oui pour le rendu | non | non |
| journaux `.editlog.json` | historique undo/redo | non | non |
| `.autosave` | secours transitoire | non | non |
| `.cutmachine/` | non | non | oui |

Chaque timeline et son journal sont adressés par ULID. Le chargement vérifie que
l’ULID du fichier physique correspond à l’index, recompose le projet, puis le
valide. Perdre un journal ne détruit pas le montage mais supprime son historique.

## 3. Création et ouverture

La page de démarrage crée directement un package v2 avec une timeline. Elle
n’écrit jamais de projet JSON isolé. L’utilisateur ouvre le package depuis le
Finder ou la page d’accueil ; en interne, l’application adresse
`project.cutmachine.json`.

Les commandes headless reçoivent ce chemin interne :

```sh
cutmachine --describe Film.cutmachine-project/project.cutmachine.json
```

La liste des projets récents et les raccourcis clavier restent dans
`NSUserDefaults` et ne sont pas partagés avec le package.

## 4. Médias et chemins

Un média garde un ULID stable. Un chemin relatif est résolu depuis la racine du
package ; un chemin absolu reste local à la machine. Les caches, proxies,
waveforms et vignettes sont dérivés et peuvent être supprimés.

Un package de travail peut référencer des originaux externes. Pour le partager,
**Fichier → Collecter le projet…** crée un nouveau package autonome sans modifier
la source. La collecte :

- refuse d’écraser une destination existante ;
- exige tous les originaux lisibles ;
- copie chaque média sous `Media/<ULID>-<nom>` ;
- remplace les chemins par des chemins relatifs internes sans `..` ;
- exclut les proxies et caches ;
- conserve les journaux ;
- inscrit taille et SHA-256 dans le manifeste ;
- recharge et valide la copie avant publication par renommage.

À l’ouverture, les tailles et SHA-256 sont contrôlés. Un original manquant ou
modifié déclenche un avertissement. Les empreintes vérifient le contenu mais ne
remplacent jamais les ULID métier.

## 5. Sauvegarde, autosave et concurrence

Une sauvegarde prépare des fichiers temporaires voisins, sauvegarde les
versions remplacées, puis publie le projet, toutes les timelines et tous les
journaux. Un rollback est tenté lorsqu’une étape retourne une erreur.
L’autosave contient un snapshot canonique et peut être récupéré explicitement au
prochain lancement.

Un verrou `<project.cutmachine.json>.lock/owner` contient un jeton, le Mac,
l’utilisateur et le PID. Une seconde écriture est refusée. Un verrou dont le
PID local n’existe plus est retiré ; un verrou d’une autre machine n’est jamais
cassé automatiquement.

La transaction ne touche que les fichiers qu'elle a elle-même écrits : dans
`Timelines/`, un fichier n'est reconnu comme timeline obsolète que si son nom
est un ULID valide suivi de `.json`. C'est ce qui rend le format utilisable sur
un volume exFAT ou FAT — celui de la plupart des disques média partagés — où
macOS crée un fichier compagnon AppleDouble `._<nom>` à côté de chaque fichier.
Ces compagnons ne peuvent pas être renommés seuls, et les prendre pour des
timelines périmées faisait échouer toute sauvegarde d'un projet stocké sur un
tel disque.

Les renommages multiples ne sont pas une transaction matérielle face à un crash
machine. Il faut fermer CUTMACHINE et attendre la synchronisation complète avant
d’ouvrir un package sur un autre Mac. Pour un transfert, une archive ZIP fermée
est préférable à la synchronisation d’un projet actif.

## 6. Procédure de partage

1. Choisir **Fichier → Collecter le projet…**.
2. Attendre la validation finale.
3. Fermer le projet.
4. Copier le package ou le compresser en ZIP.
5. Sur l’autre Mac, décompresser puis ouvrir le `.cutmachine-project`.

Une archive pérenne doit aussi conserver un master vidéo indépendant de
CUTMACHINE et tout élément externe nécessaire au workflow. Les caches et
proxies ne remplacent jamais les originaux.

## 7. Critères d’acceptation

- [x] création et ouverture limitées au package v2 ;
- [x] rejet explicite des formats projet/timeline historiques ;
- [x] une timeline physique et un journal par ULID ;
- [x] collecte autonome avec chemins internes et SHA-256 ;
- [x] validation d’intégrité à l’ouverture ;
- [x] détection d’une seconde session d’écriture ;
- [ ] crash injecté à chaque étape laissant toujours une génération ouvrable ;
- [ ] comparaison automatisée du rendu source et du rendu collecté ;
- [ ] transfert ZIP aller-retour testé sur deux machines.
