# Philosophie de CUTMACHINE

## Ce que c'est

Un moteur de montage adressable : un modèle de document, une algèbre
d'opérations qui le transforme, et plusieurs surfaces qui exposent les deux.

L'interface graphique est une de ces surfaces. La ligne de commande en est une
autre, le pilote conversationnel une troisième. Aucune n'est privilégiée,
toutes sont remplaçables. Le modèle et l'algèbre ne le sont pas.

## Ce que ce n'est pas

Une copie de Premiere. Le moteur adressable — document, algèbre d'opérations,
surfaces interchangeables — reste le cœur du projet et ne se négocie pas.

Mais un moteur sans surface crédible ne se fait pas adopter, même s'il a
raison sur le fond. Depuis que l'espace existe des éditeurs pensés pour être
pilotés par un agent — Palmier Pro en est un exemple concret, pas open source
sur sa partie génération, verrouillé à une seule plateforme, mais avec une
interface et une surface d'opérations que CUTMACHINE n'a pas encore — la
parité fonctionnelle et une interface soignée ne sont plus écartées par
principe. Elles servent l'adoption du moteur, exactement comme un temps exact
sert la fiabilité d'un raccord.

CUTMACHINE occupe toujours une autre place : le montage que l'on peut
scripter, versionner, inspecter et brancher sur autre chose — mais avec une
interface qui n'oblige plus à choisir entre les deux.

---

## Principes

### 1. Le document est la vérité

L'état complet d'un projet tient dans un fichier JSON lisible, versionné et
sérialisé de façon canonique. Pas de base de données, pas de format binaire,
pas d'état caché dans l'application.

Conséquence directe : un projet se lit, se compare, se versionne dans git, se
génère par script et s'inspecte sans lancer le logiciel.

### 2. Toute modification est une opération

Rien ne mute le document directement. Chaque changement est une opération
atomique, réversible, sérialisable et nommée.

Une opération n'est pas une commande impérative. C'est un objet que l'on peut
stocker, transmettre, rejouer, annuler et inspecter. L'historique d'un montage
est donc une donnée, pas un effet de bord.

Corollaire de conception : une fonctionnalité qui ne peut pas s'exprimer comme
une opération sérialisable est probablement mal conçue. Cette contrainte vaut
pour tout ce qui reste à construire.

### 3. Aucune surface n'est privilégiée

Un geste à la souris et une instruction en langage naturel produisent la même
opération, passent par le même journal, subissent les mêmes validations.

Une fonctionnalité existe d'abord dans le moteur, ensuite dans l'interface.
Jamais l'inverse. Ce qui n'est accessible qu'à la souris n'existe pas.

### 4. Le temps est exact

Les positions et les durées sont des rationnels entiers, jamais des flottants.
Les conversions non exactes sont refusées plutôt qu'arrondies silencieusement.

Une frame perdue dans un arrondi ne se voit pas au moment où elle se produit.
Elle se voit trois heures plus tard, sur un raccord qui ne tombe plus juste, et
elle ne se retrouve pas.

La frontière entre temps exact et pixels flottants existe à un seul endroit du
code, et elle arrondit explicitement.

### 5. L'adressage est stable

Chaque objet porte un identifiant persistant. Rien n'est adressé par position
dans un tableau.

Un index change à chaque insertion. Un identifiant permet à un script écrit
hier, ou à un agent qui raisonne sur un état antérieur, de désigner encore le
bon objet.

### 6. Le déterminisme se teste

Même document, mêmes médias, même résultat, octet pour octet. Une séquence
d'opérations appliquée puis annulée restitue le fichier d'origine à l'identique,
pas seulement le même montage.

Ce critère n'est pas de la rigueur décorative. C'est ce qui rend une régression
détectable par une machine plutôt que par l'œil.

### 7. Le code garantit ce que le modèle devinerait

Un modèle de langage exprime une intention. Il ne calcule pas une convention de
signe, ne résout pas une référence ordinale, ne convertit pas une durée dans un
timebase.

Chaque calcul déterministe retiré du modèle est une famille entière d'erreurs
qui n'existera jamais, quelle que soit la formulation employée et quel que soit
le modèle utilisé.

Cette règle vient d'une mesure, pas d'une intuition. Voir `AUDIT.md`.

### 8. Ça doit être agréable à utiliser, et beau

Un outil de montage se juge au geste, pas à la liste de ses fonctions. Une
opération correcte mais pénible à déclencher est une opération ratée.

L'apparence compte pour la même raison. Le rendu est intégralement dessiné, donc
rien n'oblige à reproduire l'esthétique par défaut des logiciels existants.
C'est une liberté qu'ont peu de projets et il serait absurde de ne pas s'en
servir.

Ce critère est subjectif et le reste. Il se tranche en montant réellement
quelque chose avec l'outil, pas en discutant.

### 9. L'instrumentation précède l'optimisation

Aucune décision de performance n'est prise sans mesure. Deux fois pendant le
développement du moteur de lecture, un chiffre a contredit une conviction
raisonnable, et dans les deux cas la conviction aurait tué une piste viable.

Un bug qui ne provoque ni crash ni erreur ne se trouve pas en relisant le code.

---

## Non-buts

Du travail écarté parce qu'il est ennuyeux ou imitatif, pas des portes fermées.
Aucune de ces lignes n'interdit d'essayer quelque chose.

- **Pas de parité fonctionnelle par imitation.** Une fonction entre parce
  qu'elle sert la visibilité sur l'état du projet, la capacité à intervenir
  dessus, ou la crédibilité du produit face aux alternatives existantes — pas
  parce qu'un concurrent l'a. La nuance compte : la fonction est toujours
  justifiée, la liste de fonctions ne l'est jamais par elle-même.
- **L'état d'interface persistant est une préférence locale, jamais une
  vérité.** Thèmes, dispositions, panneaux déplaçables peuvent être
  mémorisés pour le confort d'usage, mais ne vivent jamais dans le document
  projet. Rouvrir un projet sur une autre machine, avec des préférences
  d'interface différentes, doit produire le même montage.
- **Pas de format propriétaire.** Si le document cesse d'être lisible et
  modifiable à la main, le projet a perdu sa raison d'être.
- **Pas de service central.** Les modèles distants s'utilisent avec la clé de
  l'utilisateur. Le logiciel ne s'interpose pas entre lui et son fournisseur.
- **Pas de règle qui empêche d'essayer.** Une idée qui n'entre dans aucune
  case existante se teste d'abord et se juge ensuite. Un principe qui bloque
  une expérience intéressante est un principe à réécrire.

---

## Comment lire ce document

Les principes 1 à 5 sont structurels. Les enfreindre demanderait une
réécriture, pas une correction. Ce sont les seuls qui méritent d'être défendus.

Les principes 6 à 9 sont méthodologiques. Ils décrivent la façon de travailler
qui a produit les cinq premiers, et rien n'oblige à s'y tenir sur un essai.

Les non-buts protègent le temps disponible, pas une doctrine.

Reste que c'est un projet créatif avant d'être un projet d'ingénierie. La
rigueur du modèle existe pour rendre les expériences possibles, pas pour les
encadrer. Un document de principes qui rend le travail moins intéressant a
échoué à sa fonction.
