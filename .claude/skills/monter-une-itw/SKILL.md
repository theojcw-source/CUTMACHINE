---
name: monter-une-itw
description: Monter une interview (ITW/ITM/ADS/LISAA) dans un projet CUTMACHINE et la livrer — style de la maison, bout à bout, montage, plans de coupe, recalage sur le signal, étalonnage S-Log3, export, SRT, renvoi vers DaVinci Resolve. À utiliser quand on demande de « monter » une interview, d'en refaire l'export, d'y ajouter des plans d'illustration, d'en sortir les sous-titres ou de la renvoyer dans Resolve.
---

# Monter une interview avec CUTMACHINE

## Lis ça d'abord : deux passes, pas une

La première exécution de cette procédure a rendu un montage jugé par le
monteur **« bon bout à bout, mais pas utilisable tel quel »**. Il a dû le
retravailler. Le diagnostic est exact et il faut le prendre au sérieux :

- un **bout à bout** enchaîne les bonnes phrases dans le bon ordre, sans
  coupe fautive. C'est ce que produit `create_interview_short`, et c'est ce
  que la procédure d'origine polissait ;
- un **montage** a en plus un rythme, une accroche, une durée tenue, et rien
  de redondant. C'est un travail différent, avec ses propres critères.

Polir un bout à bout ne le transforme pas en montage. La passe 2 ci-dessous
existe parce qu'elle avait été sautée sans que personne s'en aperçoive — les
vérifications au frame près passaient toutes, et elles ne voient pas
l'ennui.

**Symptôme mesurable de la passe sautée**, relevé sur le montage livré :
quatre plans sur neuf s'ouvraient sur « Alors », cinq sur neuf sur un
marqueur de discours. C'était lisible dans les transcriptions du début à la
fin ; elles n'avaient jamais été lues d'un bloc, seulement clip par clip
pour vérifier les bornes.

---

## Passe 0 — Lire le style de la maison

**Ne saute jamais cette étape.** Le chutier **`TL`** du projet Resolve
contient les montages déjà finis par le monteur, en timelines — pas en
fichiers. Ce sont des références de montage faites par la personne pour qui
tu travailles, et elles se mesurent.

```python
# via sidecar/resolve_bridge.py : connect(), puis pour chaque timeline du chutier TL
#   durée      = (GetEndFrame() - GetStartFrame()) / cadence
#   plans      = somme des GetItemListInTrack("video", v)
#   1re coupe  = fin du premier plan de V1
#   B-roll     = durée cumulée des plans sur V2 et au-dessus
```

### Chiffres mesurés sur ITM267 (15 montages exploitables, 08/2026)

| | médiane | étendue |
|---|---|---|
| durée | **64,6 s** | 41 → 86 s |
| plans | **21** | 10 → 33 |
| plans par minute | **17,5** | 12 → 30 |
| première coupe | **3,0 s** | 0,8 → 10 s |
| part de B-roll | **40 %** | 18 → 73 % |

Ces chiffres sont la cible. Le premier montage produit par cette procédure
les manquait tous dans le même sens — 12,2 plans/minute, première coupe à
6,8 s, 31 % de B-roll — et a été jugé « bon bout à bout, pas utilisable ».
**Sous-couper est le mode d'échec par défaut de cette procédure.** Si tes
chiffres sont sous la médiane, tu n'as pas fini.

Regarde aussi la première seconde de trois montages finis, et note s'ils
ouvrent sur un plan de beauté ou sur un visage.

## Comment regarder un rush

**Ne décris jamais un plan depuis son nom de fichier ni depuis sa
transcription seule.** `C7422.MP4` ne dit rien, et la transcription ne dit
rien de l'image. C'est comme ça qu'un plan de beauté sur fond noir se
retrouve enterré à la dixième seconde d'un montage.

Procède **du grossier au fin**, jamais l'inverse :

1. **Planche contact** sur toute la durée du rush — une vignette toutes les
   quelques secondes, assemblées en une seule image. Une planche de neuf
   vignettes coûte une lecture ; neuf lectures coûtent neuf fois plus et se
   comparent moins bien.
   ```sh
   for t in 2 6 10 14 18; do
     ffmpeg -nostdin -v error -ss $t -i "$RUSH" -frames:v 1 -vf scale=150:-2 -y "/tmp/v_$t.jpg"
   done
   ffmpeg -nostdin -v error -i /tmp/v_2.jpg -i /tmp/v_6.jpg -i /tmp/v_10.jpg \
          -i /tmp/v_14.jpg -i /tmp/v_18.jpg \
          -filter_complex "[0][1][2][3][4]hstack=inputs=5" -y /tmp/planche.jpg
   ```
2. **Transcription** du passage repéré, pour savoir ce qui s'y dit.
3. **Zoom** : quelques images rapprochées sur l'intervalle retenu, pour
   trancher un point d'entrée ou juger un raccord.

Pour juger un **raccord**, il faut les deux images qui l'encadrent côte à
côte — dernière image sortante et première image entrante. C'est ce qui a
révélé le seul mauvais raccord du montage (A5→A6, sortie en plein filé).

Attention : les images extraites d'un rush log sont **délavées**. Ce n'est
pas ce que verra le spectateur, et ça fausse tout jugement d'exposition ou de
lisibilité. Tant que `read_frame` n'applique pas la transformation couleur du
document, extrais tes images depuis un export étalonné quand le jugement
porte sur l'image elle-même.

## Les six pièges techniques

Ils sont tous vérifiés en conditions réelles. Ils coûtent une livraison
ratée ou trois minutes d'encodage jetées si on les découvre après coup.

### 1. La gestion couleur est désactivée par défaut → livraison en log

Rushes Sony ILME-FX30 en **S-Log3-Cine / S-Gamut3.Cine**, déclaré dans le
fichier annexe de tournage :

```sh
grep CaptureGamma 1_RUSHES/C7432M01.XML   # -> value="s-log3-cine"
```

`color_management.enabled` vaut `false` à la création. Sans correction
l'export sort plat et délavé. Par MCP :

```json
{"name":"set_color_management","arguments":{
  "enabled":true,"input_gamut":"sony_sgamut3_cine","input_transfer":"sony_slog3",
  "input_ycbcr_matrix":"bt709","input_range":"auto","working_gamut":"acescct",
  "output_gamut":"rec709","output_transfer":"rec709"}}
```

Contrôle : un rush log est vers YMIN 160 / SATAVG 30 ; un livrable étalonné
descend les noirs sous 30 et monte la saturation au-delà de 45.

```sh
ffprobe -v error -f lavfi -i "movie='FICHIER':sp=30,signalstats" \
  -show_entries frame_tags=lavfi.signalstats.YMIN,lavfi.signalstats.YAVG,\
lavfi.signalstats.YMAX,lavfi.signalstats.SATAVG -of csv=p=0 -read_intervals "%+#1"
```

### 2. Les horodatages whisper sont faux — en tête, et au milieu

Whisper place les premiers mots d'un segment sur du silence : sur `C7429` il
donne « Alors, » à 0,00 s alors que la parole commence à 1,36 s.
`create_interview_short` construit ses entrées là-dessus, d'où jusqu'à 1,3 s
d'air mort en tête de plan, que le monteur entend comme des hésitations.

**« Chaque coupe entre sur du silence » n'est pas un bon signe : c'est le
défaut.**

La mesure est dans le moteur, ne la refais pas à la main :

```sh
./build/cutmachine --speech-onset "$PROJET" "<media-id>"   # une fois par source
./build/cutmachine --speech-onset-report "$PROJET"
```

ou, en MCP, `analyze_speech_onset` puis `list_speech_onsets`. Le rapport
publie `lead_in` et **`suggested_trim` en images entières, amorce déduite** :
passe-le tel quel à `ripple_trim`, ne le recalcule pas. `link_group_id` donne
le groupe à rogner en entier pour ne pas casser la synchro.

Avant de rogner, **prouve que ce n'est pas de la parole** : compare le niveau
de la zone au plancher de bruit du même plan et à un silence connu. Du
souffle est 20 dB sous la parole ; un mot dit doucement, non.

**La dérive n'est pas cantonnée à la tête du segment.** Mesurée jusqu'à
**1,3 s en plein milieu d'un rush** sur ADS260 : sur `C8011`, « une fois qu'il
est fini » est annoncée à 10,52 s et se trouve à 11,64 s. Deux conséquences
directes :

- **`remove_words` coupe à côté sur ces rushes.** Il résout les images depuis
  ces horodatages (`ResolveWordRemoval`), donc il enlève les mauvaises. Pour
  une compression interne, mesure les images dans le signal, puis
  `split_linked_clips` suivi d'un `ripple_trim` sur la partie droite.
- **Ré-transcrire un extrait ne suffit pas.** Whisper étale encore le premier
  mot sur le début du fichier : un extrait qui démarre ~0,2 s avant la parole
  se lit, un extrait qui démarre dans le silence, non. Le seul étalon fiable
  est le son **monté** — c'est l'étape 1 de la passe 3, et elle passe avant
  l'export.

### 3. `--disfluencies` ne voit que ce qui est écrit

Il renvoie `[]` alors que le monteur entend des « euh » : whisper, même en
verbatim, ne les écrit pas. Les fillers non écrits se cachent dans les trous
**entre** deux mots transcrits. Cherche les trous ≥ 0,30 s et regarde s'ils
sont voisés : trou voisé = mot manquant à la transcription ; trou muet =
respiration, on n'y touche pas.

### 4. Les caches de qualité d'image périmés sont rejetés en silence

Un cache `version: 2` n'est pas lu par un binaire qui écrit du `version: 3` :
les clips remontent en `unanalyzed`, ce qui **n'est pas un feu vert**, c'est
un inconnu. Régénère avec `--shot-quality`, puis `--shot-quality-report`.

`Soft` / `Blurry` sont relatifs à la médiane de la source : sur un plan filé
à la main, un pic de flou est du filé. **Regarde l'image** avant de rejeter.

### 5. `--export` ne prend pas de dimensions

Il suit le format de séquence. Pour livrer en 1080×1920 depuis une séquence
2160×3840, passe par `update_sequence`, puis **annule après l'export** et
vérifie que la séquence est revenue à son format.

### 6. exFAT n'a pas de liens durs

`Exporter::Run` validait l'export par `link(2)` : échec avec `Operation not
supported` **après avoir rendu 100 % des images**, puis destruction du rendu.
Corrigé par un repli `rename`. Si un export meurt à 100 %, c'est là.

Ne passe **jamais** `--overwrite` sur un nom de livrable existant.

---

## Passe 1 — le bout à bout

1. **État des lieux.** `--describe` : format de séquence, pistes, chutiers,
   durée. Repère la timeline de dérushage et celle de montage.
2. **Transcriptions en verbatim** (`"verbatim": true`), sinon
   `--transcribe … --verbatim`.
3. **Sélection.** Timeline de dérushage active, `get_timeline_transcript`,
   puis `create_interview_short` en nommant les `span_id` dans l'ordre
   éditorial. **Ne retape jamais un timecode.**
4. **Recale les entrées** (piège 2) et nettoie les fillers (piège 3).

À ce stade tu as un bout à bout correct. **Tu n'as pas un montage.**

---

## Passe 2 — le montage

C'est la passe qui manquait. Elle se fait sur le texte avant de se faire à
l'image : ouvre les transcriptions des plans retenus et lis-les **d'un
bloc**, comme un script, pas clip par clip.

### L'ordre compte : le silence d'abord, le texte ensuite

Resserre en deux temps, dans cet ordre.

**D'abord les temps morts, sans transcription.** Les pauses, les respirations
entre phrases et l'air mort en tête de plan se mesurent dans le signal
(`--speech-onset-report`) et se coupent sans lire un mot. C'est mécanique,
c'est sûr, et ça se fait avant tout jugement éditorial.

**Ensuite la redondance, sur le texte.** Une fois le silence retiré, ce qui
reste est de la parole, et les choix deviennent éditoriaux.

L'ordre inverse — partir de la transcription — est ce qui a produit le
premier montage raté : on ne coupe que ce qui est écrit, et le silence n'est
écrit nulle part.

### Tiens une durée

Prends la médiane de la passe 0 comme cible et écris-la avant de commencer.
Sans cible, « quelle phrase est de trop » est une question de goût ; avec,
c'est une contrainte. Le montage d'origine faisait 71 s pour 121 s de rushes
— un ratio de dérushage de 0,6, très faible pour de l'interview.

### Coupe à l'intérieur des phrases

**C'est le geste qui sépare les deux passes.** `create_interview_short`
travaille par spans, taillés pour la lisibilité des sous-titres — c'est de la
granularité de bout à bout. Un span sélectionné n'est pas un atome : il se
recoupe.

Mesuré sur la reprise du monteur, plan par plan, contre la version rendue :

| source | version rendue | reprise du monteur |
|---|---|---|
| C7429 | 1 bloc de 8,2 s | **2 morceaux**, 2,5 s au total |
| C7430 | 1 bloc de 14,4 s | **5 morceaux**, 10,9 s au total |
| C7431 | 1 bloc de 8,3 s | 2 morceaux, 6,3 s |
| C7432 | 2 blocs, 15,6 s | 3 morceaux, 11,3 s |
| C7436 | 1 bloc de 13,9 s | **3 morceaux**, 11,8 s |

Neuf plans sont devenus dix-sept, et 17,5 secondes de parole ont disparu.
Aucune phrase n'a été supprimée en entier : elles ont toutes été resserrées
de l'intérieur. La compression se fait au mot, avec `remove_words`, ou par
`split_clip` puis suppression — exactement l'outillage du nettoyage des
fillers, appliqué à la redondance.

**Ne t'arrête pas aux « euh » évidents.** C'est l'erreur du premier montage :
les fillers ont été nettoyés, et rien d'autre. Quatre familles se coupent, et
il faut les traiter toutes les quatre :

- **les marqueurs de discours en tête de plan** — « Alors », « Du coup »,
  « Et », « Ensuite ». Le montage d'origine en avait cinq sur neuf, dont
  quatre fois « Alors ». Sur une coupe, ils s'entendent tous ;
- **les faux départs et les phrases reprises** — la personne se corrige et
  redit sa phrase : garde la seconde version, jette la première ;
- **les redites** — deux formulations de la même idée, à dix secondes
  d'écart : garde la plus nette ;
- **les fins molles et les relances** — « et voilà », « Ok », « tout ici »,
  et les questions de l'intervieweur restées dans le plan.

### Relis après avoir coupé

Une coupe au mot déplace tous les indices suivants : **relis la transcription
complète après chaque passe de suppression**, jamais seulement le clip
touché. Tu cherches trois choses :

- une **pensée orpheline** — une phrase dont la chute est partie avec la
  coupe. C'est le défaut exact laissé sur A4, où « c'est les seuls à pouvoir
  vivre dehors » avait disparu ;
- une **prise en double** — la version qu'on croyait supprimée est restée ;
- un **enchaînement bancal** entre deux morceaux qui ne se suivaient pas.

Et garde la hiérarchie dans le bon sens : **un propos qui se tient vaut mieux
qu'un montage court**. La durée cible est un garde-fou contre la mollesse,
pas un objectif à battre.

### Vérifie que l'histoire est entière

Relis le texte retenu comme un récit et demande-toi ce qui manque. Sur le
montage d'origine, le plan A4 s'arrêtait sur « filtrer son air directement en
lui » et laissait tomber « ça a créé beaucoup de modifications génétiques […]
c'est les seuls à pouvoir vivre dehors » — c'est-à-dire **la chute de
l'histoire du personnage**. Le défaut avait été repéré et laissé au motif que
la demande portait sur la finition. Ne fais pas ça : signale-le, ou corrige-le
et dis-le.

### Décide de la première seconde

Sur un format vertical, l'accroche est une décision à part entière, et la
maison coupe tôt : **première coupe à 3,0 s en médiane**, parfois à 0,8 s.

Le montage rendu tenait son plan d'ouverture 6,8 s. Le monteur l'a ramené à
1,8 s, et a envoyé le plan de beauté du personnage sur fond noir juste
derrière — le même plan qui, dans la version rendue, n'arrivait qu'à la
dixième seconde. Demande-toi explicitement : *quelle est la première image, et
combien de temps la tient-on ?*

### Compte tes plans avant de rendre

Le contrôle le plus efficace, et il est arithmétique :

```
plans par minute = nombre total de plans vidéo / (durée en secondes / 60)
```

Sous **17 plans/minute**, tu rends un bout à bout. La version rendue était à
12,2 ; la reprise du monteur à 25,8. Aligne aussi les durées de plan : le
montage rendu finissait sur 13,9 s de plan parlant ininterrompu — le monteur
l'a découpé en trois. Si un plan dépasse largement les autres, il faut soit
le couper, soit le couvrir.

### Pose les plans de coupe

Piste vidéo au-dessus (`add_track`, `index: 2`), `insert_clip` **en ordre
croissant de position**. Règles vérifiées :

- **Jamais quelqu'un qui parle face caméra** : un plan de coupe sur une autre
  parole se lit comme une faute de synchro. Vérifie à l'image.
- **Couvre les raccords faibles**, et décale la coupe image d'une
  demi-seconde avant la coupe son.
- **Un plan de coupe sert aussi à masquer une compression** : si tu coupes
  dans une phrase, l'image doit souvent passer ailleurs à cet endroit. Un
  montage compressé demande plus de couverture qu'un bout à bout — d'où les
  40 % de médiane maison.
- **Laisse-les courir.** La version rendue alternait locuteur / coupe /
  locuteur avec des retours courts. Le monteur, lui, a enchaîné deux plans de
  coupe sans revenir au visage : 9,2 secondes continues de B-roll sur la
  narration d'ouverture, et un plan de 8,3 s plus loin. Un plan de coupe de
  4 s entouré de retours d'une seconde et demie donne un montage nerveux et
  illisible ; mieux vaut moins de retours, plus longs.
- **Sources à 50 i/s** : `source_in` au débit de la source
  (`{"value":350,"rate":50}`), `duration` au débit de la séquence. Les deux
  doivent tomber sur des images entières.
- La piste de coupe ne contient aucun membre des groupes liés : nomme-la dans
  `sync_track_ids` à chaque `ripple_trim`, sinon elle ne suit pas le décalage.

---

## Passe 3 — finition et sortie

**L'ordre compte, et le prendre à l'envers coûte cher.** L'encodage est le
poste le plus long de la procédure — ~4 min pour 70 s de film, le double si une
analyse tourne en même temps. Le contrôle qui attrape les mots mangés aux
bordures de plan se fait donc **avant** l'export, sur un montage son fabriqué
en quelques secondes. Sur ADS260, l'avoir fait après a coûté **trois exports
pour 70 secondes**, chacun ne trouvant qu'un ou deux défauts.

1. **Relis le texte réellement monté.** Concatène le son des plans retenus
   depuis les rushes, transcris-le, lis-le d'un bloc :
   ```sh
   # une entrée par plan, aux bornes source du montage
   for each plan: ffmpeg -nostdin -v error -ss <src_in/25> -t <durée/25> \
       -i "$RUSHES/<rush>.MP4" -ac 1 -ar 16000 -y "p_<n>.wav"
   ffmpeg -nostdin -v error -f concat -safe 0 -i plans.txt -c copy -y son.wav
   ```
   puis `--ingest` + `--transcribe` sur ce fichier. Tu cherches un mot avalé en
   tête ou en queue de plan — sur ADS260 : « Je m'appelle » disparu, « Le
   moment » amputé, un plan qui finissait sur un « où ça met » qui pendait —
   pas la qualité de l'image. Corrige les bordures, refais le son, relis.
   **Tant que ce texte n'est pas propre, n'exporte pas.**
2. **Son.** Les plans viennent de rushes différents, à des distances de micro
   différentes. Vérifie le niveau moyen plan par plan et signale les écarts.
   *(Le moteur n'expose pas de gain par clip ni de fondu audio à ce jour —
   si un écart est audible, dis-le plutôt que de le passer sous silence.)*
3. **Couleur.** Piège 1.
4. **Export, une seule fois.** Piège 5, puis :
   ```sh
   ./build/cutmachine --export "$PROJET" "$EXPORT/<CODE>_ITW_<Sujet>_<Perso>.mov"
   ```
   Ne lance aucune analyse lourde pendant — `--shot-quality` sur sept rushes 4K
   a doublé la durée de l'encodage sur ADS260, pour ne rien trouver : sur une
   interview au pied, les plans signalés étaient des gestes de main.
5. **Sous-titres, depuis le livrable.** `--export-srt` sur le projet de montage
   produit des cues fausses : elles héritent des horodatages du piège 2 et
   perdent les mots que whisper avait posés sur du silence rogné (sur ADS260,
   la première cue disait « encore moi. » au lieu de « Salut, c'est encore
   moi. »). Ingère le `.mov` rendu dans un projet jetable, transcris-le,
   pose-le sur une piste audio, puis `--export-srt`. Les cues viennent des
   **pistes audio**, donc un plan de coupe ne se sous-titre pas tout seul.

---

## Renvoyer dans DaVinci Resolve

Le projet Resolve doit être **ouvert** : au lancement, Resolve part sur
« Untitled Project » et le pont rapporte 0 chutier sans erreur.

```sh
./build/cutmachine --export-resolve-timeline "$PROJET" > /tmp/tl.json
python3 sidecar/resolve_bridge.py --send /tmp/tl.json --name "<nom cible>"
```

`--name` est indispensable pour renvoyer une révision : Resolve refuse un nom
déjà pris. Le schéma v2 porte par plan sa couche vidéo, sa position et le
sort de son audio ; le pont crée les pistes manquantes et décale chaque
`recordFrame` du `GetStartFrame()` de la timeline (90000 pour 01:00:00:00 à
25 i/s). Les plans de coupe partent en `mediaType: 1`, sans son.

### Le SRT dans Resolve

`Timeline.ImportIntoTimeline` **refuse le SRT** : il le lit comme un EDL et
le log dit `Import Log (Fatal) - File of srt type cannot be imported.`

Ce qui fonctionne :

```python
tl.AddTrack("subtitle")
item = mp.ImportMedia(["/chemin/durable/sous-titres.srt"])[0]
mp.AppendToTimeline([{"mediaPoolItem": item, "trackIndex": 1}])
```

**Mais l'ordre est piégeux : les sous-titres d'abord, les plans ensuite.**
`AppendToTimeline` pose l'élément de sous-titres à la fin de **ce que la
timeline contient déjà**, pas au début de sa piste. Sur une timeline de
montage déjà remplie, les 34 cues d'ADS260 ont atterri à l'image 1760 — juste
après le dernier plan — et la timeline est passée de 70 à 140 s sans erreur.

Le pont `--send` créant sa propre timeline, il faut donc reconstruire à la
main dans le bon ordre : `CreateEmptyTimeline`, `AddTrack("subtitle")`,
`AppendToTimeline` du SRT (il tombe à zéro, la timeline est vide), **puis**
les plans avec leur `recordFrame` absolu — la position absolue les place
correctement quoi qu'il y ait déjà. Réutilise `index_media_pool` du pont pour
retrouver les rushes : il renvoie `{"by_path": …, "by_name": …}`, pas un
dictionnaire plat.

Contrôle après coup : `GetEndFrame() - GetStartFrame()` doit valoir la durée
du montage, et le premier élément de sous-titre partir de zéro. Une timeline
deux fois trop longue est la signature de ce piège.

**Ne mets rien d'autre dans le `clipInfo` du SRT.** Ajouter `startFrame`,
`endFrame`, `recordFrame` ou `mediaType` sur un élément de sous-titres a fait
planter le pont Python. Importe depuis un chemin durable, pas depuis `/tmp`.
La persistance après redémarrage de Resolve **reste à reconfirmer**.

---

## Vérifications avant de rendre

- `ctest --test-dir build --output-on-failure`. `cutmachine_ui_smoke_tests`
  est instable quand une autre application est au premier plan : relance-le
  seul avant de conclure à une régression.
- Durée, résolution, cadence, présence du son (`ffprobe`).
- Niveau audio : `ffmpeg -hide_banner -i "$F" -af volumedetect -f null -`
  (`-v error` masque la sortie de volumedetect).
- **Ouverture** : le niveau moyen de la première seconde et demie doit être
  proche de celui du fichier entier. S'il est nettement plus bas, il reste de
  l'air mort en tête.
- Planche de vignettes aux points de coupe et au milieu de chaque plan de
  coupe.
- **Ne compare jamais à un fichier sans vérifier qu'il existe** : `ffprobe`
  sur un chemin absent sort une chaîne vide, et une « comparaison » avec du
  vide passe pour un résultat.

## Ce que ces vérifications ne voient pas

Elles attrapent les défauts, jamais l'ennui. Un montage peut passer toutes
les vérifications ci-dessus et rester un bout à bout : durée juste, coupes
propres, son présent, image nette — et aucun rythme.

Tant qu'un agent ne peut pas percevoir le montage dans le temps, la passe 2
est le seul garde-fou, et elle se fait **sur le texte lu d'un bloc**. Rends
toujours, avec le fichier, ce que tu n'as pas pu juger : dis explicitement
que tu n'as ni vu ni entendu le résultat, et sur quoi reposent tes
affirmations.
