# Audit du planner CUTMACHINE

Date de mesure : 10 août 2026. Cet audit n'a modifié ni le prompt système, ni
le schéma, ni la vue `--describe`, ni le code du planner ou du binaire. La seule
modification de code est l'instrumentation de `sidecar/eval.py`.

## Addendum — expérience de simplification du planner

Le verdict initial ci-dessous s'arrêtait trop tôt. Le 10 août 2026, deux
changements ont été appliqués et mesurés séquentiellement avec le même modèle
local `qwen2.5-coder:7b`, à température zéro et sur les 15 cas inchangés.

| État | Score corpus | Cas de référence |
|---|---:|---:|
| Baseline auditée | 8/15 | PASS |
| Conversions temporelles déterministes | 11/15 | PASS |
| Puis résolution déterministe des entités | 15/15 | PASS |

Le 11/15 de l'étape temporelle est un replay contrôlé des réponses brutes du
run immédiatement précédent après ajout du normaliseur de position absolue ;
le prompt, le schéma et les sorties du modèle sont inchangés. Le 15/15 final
est un run HTTP complet après ajout du résolveur d'entités. La variance finale
n'a pas été remesurée : il s'agit d'un run, à comparer aux trois runs identiques
de la baseline.

La première étape remplace le `delta` signé demandé au modèle par une intention
`Shorten|Extend`, un bord et une quantité positive `Frames|Seconds`. Le code
calcule ensuite signe, timebase et position absolue. La seconde étape résout les
pistes ordinales, clips ordinaux ou uniques, alias et noms de source depuis la
vue avant de valider l'opération.

Le résultat le plus probant n'est pas seulement 15/15 : sur les cas 2, 11, 13
et 15, les ULID bruts produits par Qwen restent exactement les mauvais ULID de
la baseline, tandis que l'opération normalisée est correcte. Le gain vient donc
bien du déplacement d'un travail déterministe hors du modèle, et non d'une
amélioration fortuite de sa réponse. La conclusion initiale « capacité du
modèle, pas conception » est réfutée pour ce corpus : la chaîne technique était
correcte, mais son contrat faisait porter au modèle des conventions internes et
des résolutions que le code pouvait garantir.

## Verdict

**Le facteur limitant mesuré est la capacité du modèle local, pas la conception
commune aux deux backends.** Ollama avec `qwen2.5-coder:7b` obtient 8/15
(53,3 %) lors de chacun des trois runs. Anthropic avec le modèle effectivement
retourné `claude-sonnet-4-5-20250929` obtient 15/15 (100 %). L'écart est de
46,7 points, Anthropic ne reproduit aucun des sept échecs Ollama, et tous les
oracles sont valides directement dans le binaire.

Le cas critique « Raccourcis le premier plan de 2 secondes » passe sur les deux
backends. La fonction centrale est donc viable sur ce cas, même si le modèle
local reste insuffisant sur le corpus complet.

## Taux et variance

| Backend | Run 1 | Run 2 | Run 3 | Moyenne corpus | Variance observée |
|---|---:|---:|---:|---:|---:|
| Ollama `qwen2.5-coder:7b` | 8/15 (53,3 %) | 8/15 (53,3 %) | 8/15 (53,3 %) | 53,3 % | étendue 0 point ; écart-type 0 point |
| Anthropic `claude-sonnet-4-5-20250929` | 15/15 (100 %) | — | — | 100 % | non mesurée (un run demandé) |

Les sorties brutes Ollama sont identiques cas par cas sur les trois runs, pas
seulement les scores. L'ajout du cas de référence donne 9/16 à Ollama sur chaque
run et 16/16 à Anthropic ; ces scores étendus ne remplacent pas les taux du
corpus historique.

## Résultats des 15 cas

« PASS » signifie que l'opération normalisée est sémantiquement égale à
l'oracle. Les résultats Ollama ci-dessous sont ceux des trois runs, puisqu'ils
sont identiques. La famille indiquée est la première cause rencontrée ; une
cause secondaire est explicitement signalée.

| # | Instruction | Résultat Ollama (3/3 runs) | Résultat Anthropic | Famille |
|---:|---|---|---|---|
| 1 | Supprime le clip A1. | PASS | PASS | — |
| 2 | Enlève le deuxième clip de la piste vidéo 1. | FAIL : choisit B1 (`…008`) au lieu d'A2 (`…005`) | PASS | E — mauvais clip sélectionné ; l'ULID produit existe dans la vue |
| 3 | Supprime A3, le plan issu d'illustrations.mov. | PASS | PASS | — |
| 4 | Retire l'unique clip de la deuxième piste. | PASS | PASS | — |
| 5 | Raccourcis la fin de A1 de 10 images. | PASS | PASS | — |
| 6 | Retire 5 images à la fin de A2. | PASS | PASS | — |
| 7 | Coupe les 10 premières images de A3. | FAIL : `delta=-10/25`, attendu `+10/25` | PASS | C — signe temporel faux |
| 8 | Récupère 5 images avant le début actuel de A2. | PASS | PASS | — |
| 9 | Prolonge la fin de A3 de 10 images. | PASS | PASS | — |
| 10 | Enlève 5 images au début de B1. | FAIL : `delta=-5/25`, attendu `+5/25` | PASS | C — signe temporel faux |
| 11 | Sur la piste vidéo 1, insère à l'image 50 dix images de interview.mov à partir de sa source 200. | FAIL : piste 2 (`…007`) au lieu de piste 1 (`…003`) | PASS | E — mauvaise piste sélectionnée ; l'ULID produit existe dans la vue |
| 12 | Au début de la piste vidéo 2, insère les 15 premières images de illustrations.mov. | PASS | PASS | — |
| 13 | Ajoute à la fin de la piste vidéo 1 vingt images de interview.mov depuis l'image source 300. | FAIL : piste 2 au lieu de piste 1 ; `timeline_in=125/25` au lieu de `150/25` | PASS | E — mauvaise piste en première cause ; C secondaire |
| 14 | Dans le trou après A1, à l'image 50, place 10 images de illustrations.mov depuis l'image source 50. | FAIL : `timeline_in=75/25`, attendu `50/25` | PASS | C — position temporelle fausse |
| 15 | Insère au tout début de la piste vidéo 2 cinq images de interview.mov à partir de l'image source 400. | FAIL : piste 1 au lieu de piste 2 | PASS | E — mauvaise piste sélectionnée ; l'ULID produit existe dans la vue |

## Répartition des échecs

| Backend / périmètre | A | B | C | D | E | Total |
|---|---:|---:|---:|---:|---:|---:|
| Ollama, par run de 15 cas | 0 | 0 | 3 | 0 | 4 | 7 |
| Ollama, agrégé sur 3 runs (45 réponses) | 0 | 0 | 9 | 0 | 12 | 21 |
| Anthropic, 15 réponses | 0 | 0 | 0 | 0 | 0 | 0 |

Le cas 13 possède aussi une cause C secondaire, non ajoutée aux totaux afin de
respecter la classification exclusive par première cause. Aucun modèle n'a
inventé ou déformé d'ULID pendant ces runs : les erreurs d'identité Ollama sont
des sélections d'objets existants, donc E et non B. Il n'y a ni mauvais type
d'opération (A), ni sortie hors schéma/refus injustifié (D).

## Cas de référence isolé

Le cas ne figurait pas dans les 15 instructions. Il a été ajouté comme cas
supplémentaire, sans modifier le corpus historique ni ses dénominateurs.

Instruction : **« Raccourcis le premier plan de 2 secondes. »**

Oracle : `TrimClip`, clip A1 (`01K40000000000000000000004`), bord `Tail`,
`delta={"value":-50,"rate":25}`.

| Backend | Résultat | Opération produite |
|---|---|---|
| Ollama | PASS sur 3/3 runs | exactement l'oracle sur les champs sémantiques |
| Anthropic | PASS | exactement l'oracle sur les champs sémantiques |

## Vérifications de contrôle

1. **Contrainte Ollama réellement émise : oui.** Les 48 requêtes HTTP Ollama
   capturées (45 corpus + 3 référence) contiennent le champ `format`. Sa valeur
   est égale au schéma partagé. Ce constat vient du corps sérialisé de chaque
   `urllib.request.Request`, pas d'une lecture statique de l'intention du code.

2. **Schémas identiques : oui.** Le `format` effectivement envoyé à Ollama et
   l'`input_schema` effectivement envoyé dans l'unique outil Anthropic sont
   strictement identiques comme objets JSON, et tous deux égaux à
   `PLANNER_RESPONSE_SCHEMA`.

3. **Validité des 15 oracles : 15/15.** Chaque opération attendue a été soumise
   à `--apply-op` sur une copie fraîche de `sidecar/eval-document.json`. Les 15
   ont été acceptées. Aucun échec du planner n'est imputable à un faux cas de
   test.

4. **Taille de la vue : 1 638 caractères/octets UTF-8.** Le JSON compact exact
   issu de `--describe` mesure **753 tokens avec le tokenizer réel de
   `qwen2.5-coder:7b`** (`/api/generate`, mode `raw`) et **606 tokens avec
   `claude-sonnet-4-5`** (API `count_tokens`). Cette vue seule est modeste et ne
   constitue pas un signal de surcharge contextuelle. À titre distinct, une
   requête Anthropic complète observée consomme 2 340 tokens d'entrée, prompt
   système et enveloppe inclus.

5. **Validation ULID avant le binaire.** La chaîne est : réponse HTTP → décodage
   structuré → `_parse_plan` → `_normalize_operation` → validation contre les
   IDs de la vue. Un ULID absent déclenche donc un `PlannerError` local dans
   `planner.plan`, avant tout appel à `CutmachineBinary.apply_operation`. Le
   chemin d'évaluation n'appelle d'ailleurs jamais `--apply-op` pour une sortie
   modèle. Ce rejet peut masquer le refus nommé qu'aurait donné le C++, mais il
   ne masque pas le score : le harnais le comptabilise comme échec, et
   l'instrumentation conserve la sortie brute avec le point de rupture
   `validation_locale_ulid_avant_binaire`. Aucun rejet B n'a eu lieu dans cette
   campagne.

## Lecture diagnostique

La contrainte structurée fonctionne : 64/64 réponses ont traversé le transport,
le décodage et la validation locale sans erreur de schéma ou refus. Les sept
échecs récurrents de Qwen sont sémantiques : quatre résolutions d'objet/piste et
trois calculs temporels. Anthropic résout ces mêmes formulations sans changer
aucune autre composante de la chaîne. Cela exclut, sur ce corpus, le schéma, les
oracles, la longueur de vue et l'absence de contrainte de décodage comme causes
principales.

La conclusion reste bornée à ce corpus de 15 cas, un seul document, un seul run
Anthropic et les modèles mesurés. Elle ne prouve pas qu'aucun défaut de
conception n'existe hors corpus ; elle montre que les échecs observés ici ne
sont pas communs aux deux backends.

## Corrections envisageables, non appliquées

Classement par gain attendu sur ce corpus :

1. **Employer Anthropic pour le chemin exigeant ou router vers un modèle plus
   capable.** Gain observé : +7 cas sur 15, soit +46,7 points. C'est le seul gain
   directement mesuré, mais il introduit coût, latence réseau et dépendance à un
   service externe.

2. **Sortir la résolution des entités ordinales et des pistes du raisonnement
   libre.** Un résolveur déterministe pourrait transformer « deuxième clip de
   la piste 1 » et « piste vidéo 2 » en candidats ULID avant la production de
   l'opération. Gain maximal direct : 4 cas sur 15 (+26,7 points), avec moins de
   flexibilité linguistique à gérer explicitement.

3. **Rendre déterministes les conversions temporelles après extraction de
   l'intention.** Extraire action, bord et quantité, puis calculer signe,
   timebase et position dans une couche typée cible directement les trois
   échecs C (+20 points) et la cause secondaire du cas 13. Cela déplace une part
   du planner vers du code vérifiable.

4. **Évaluer un modèle local plus capable avec exactement ce protocole.** Le
   potentiel est de récupérer jusqu'aux sept cas perdus sans changer
   l'architecture, mais le gain n'est pas encore mesuré et le coût mémoire ou la
   latence peuvent augmenter.

5. **Ne modifier prompt, schéma ou vue qu'après une expérience isolée.** Leur
   gain attendu est faible dans cette campagne : zéro échec A/B/D, schémas
   identiques et respectés, vue de 753 tokens, et les règles de signe sont déjà
   présentes dans le prompt. Une telle piste demanderait un nouveau protocole
   A/B ; aucune modification ni aucun few-shot n'a été appliqué ici.

## Artefacts

`eval-trace.json` contient 64 enregistrements : instruction, oracle, sortie
brute avant validation, opération finale ou refus/erreur, point de rupture,
classification primaire et causes secondaires, réponse HTTP brute et preuve du
schéma présent dans la requête émise. Aucune clé API ni aucun en-tête secret n'y
est enregistré.
