---
name: monter-une-itw
description: Monter une interview (ITW/ITM/ADS/LISAA) dans un projet CUTMACHINE et la livrer — cohérence du propos, resserrement, plans de coupe, étalonnage S-Log3, export, SRT, renvoi vers DaVinci Resolve. À utiliser quand on demande de « monter » une interview, d'en refaire l'export, d'y ajouter des plans d'illustration, d'en sortir les sous-titres ou de la renvoyer dans Resolve.
---

# Monter une interview avec CUTMACHINE

## La règle qui prime sur toutes les autres

**Un montage d'interview est d'abord un propos qui se tient.** Pas une durée,
pas un nombre de plans par minute, pas un pourcentage de plans de coupe. Si le
spectateur ne peut pas suivre le raisonnement de la personne, aucun chiffre ne
rattrape ça.

Cette règle est écrite en tête parce que la version précédente de ce skill ne
l'avait pas, et que le montage produit sous sa conduite a été refusé. Les
chiffres y étaient tous bons — 25 plans/minute, 48 % de B-roll, première coupe
à 3,0 s — et le film ne se tenait pas.

### Le test de l'antécédent

C'est le contrôle qui aurait attrapé l'erreur, et il se fait sur le texte, à
la sélection, avant toute opération.

**Lis le texte retenu comme quelqu'un qui n'a pas vu les rushes.** Pour chaque
mot qui renvoie à autre chose — `c'est`, `ça`, `ce projet`, `le`, `la`, `y`,
`en`, `également`, `aussi`, `du coup`, `pour ça` — demande-toi : *ce à quoi ce
mot renvoie est-il dans le montage ?* Si la réponse est non, tu as une
référence pendante, et c'est une faute de montage, pas une imperfection.

Deux exemples réels, tous deux produits par la procédure d'avant :

- Un montage ouvrait sur « **…de réhabiliter** des anciens moulins à
  Châteaulin ». Ni sujet ni verbe : le film démarre au milieu d'une phrase.
- Un autre ouvrait sur « **c'est** un lieu de passage pour tous les gens qui
  vont vers la côte ». « C'est » quoi ? Le nom de la ville n'arrive que huit
  mots plus loin.

Et le cas le plus coûteux, parce qu'il est invisible plan par plan : garder
une conséquence dont on a coupé la cause. Sur la même série, « **Et moi, en
tant que sportive également**, c'est complètement ce que je ressens dans ce
projet-là » était conservé, alors que la phrase qui le portait — « j'ai
compris que le sport dynamise les événements, les espaces et les personnes » —
avait été coupée. Le « également » ne renvoie à rien, « ce que je ressens »
non plus. Chaque plan était propre ; l'enchaînement ne voulait rien dire.

### Ce qui en découle

- **Garde l'antécédent avec sa conséquence.** Elles se sélectionnent
  ensemble ou pas du tout.
- **Ne réordonne que si la référence survit au déplacement.** Prendre une
  bonne phrase au milieu pour en faire l'accroche marche seulement si elle se
  suffit à elle-même. « Personne ne s'arrête jamais à Châteaulin » se suffit ;
  « c'est un lieu de passage » non.
- **Préfère peu de blocs longs et complets à beaucoup de morceaux courts.**
  Un bloc qui contient sa propre logique ne peut pas produire de référence
  pendante ; un morceau de deux secondes, si.
- **Une digression technique n'a pas sa place dans une minute.** Sur le même
  montage, quinze secondes sur le traitement du corten passaient avant le
  propos du personnage. Ce qui est passionnant en soutenance encombre un
  format court.

### L'ordre de travail qui en découle

1. Lire **toute** la parole de la personne comme un script.
2. Écrire son raisonnement en une phrase : de quoi elle part, ce qu'elle a
   fait, pourquoi ça compte, où ça la mène.
3. Choisir les blocs qui portent ce raisonnement, entiers.
4. Passer le test de l'antécédent sur le texte retenu.
5. **Seulement ensuite** : resserrer, illustrer, exporter.

La durée est ce qui **résulte** de ce choix, pas ce qui le commande.

---

## Les silences : mesure-les, ne les devine pas

Deuxième reproche du monteur sur la version refusée : « tu laisses des
silences ». Il avait raison, et le défaut était invisible dans les rapports
d'opération.

### Le réglage qui laissait passer le problème

`tighten_pauses` prend `min_gap_ms` et `keep_frames`. La première passe a
tourné en **400 ms / 6 images** et son rapport annonçait fièrement quatre
creux refermés par plan — en taisant que **36 à 44 creux étaient « trop
courts »** et laissés en place. Additionnés, ces creux sous 400 ms faisaient
le film entier respirer mou.

Réglage qui marche : **250 ms / 3 images**. Sur le même montage, il a refermé
**19 creux au lieu de 4** et retiré 6,5 s au lieu de 4,1 s. Le silence mesuré
est passé de 6,1 % à 2,1 % de la durée.

Ne descends pas beaucoup plus bas sans écouter : sous ~200 ms on colle les
mots entre eux et la parole devient haletante.

### Comment le mesurer

`silencedetect` sur le son monté, **au bon seuil**. C'est le piège : à
−35 dB on ne voit presque rien (1,6 % sur un film pourtant mou), parce que le
fond de salle est au-dessus de ce seuil. C'est à **−30 dB** que les creux
apparaissent — on cherche l'absence de *parole*, pas l'absence de *signal*.

```sh
ffmpeg -nostdin -hide_banner -i "$FILM" \
  -af silencedetect=noise=-30dB:d=0.18 -f null - 2>&1 | grep silence_
```

Repère : **c'est le creux le plus long qui compte, pas le total.** Douze
respirations de 0,2 s ne s'entendent pas ; un seul trou de 0,6 s s'entend.
Vise **aucun creux au-delà de ~0,3 s**, et regarde le total (3 à 4 %) comme un
signe, pas comme une note. Le contrôle se fait sur le son monté, pas après
l'export.

### Ne descends pas sous 300 ms

Mesuré : à **250 ms**, `tighten_pauses` a refermé un creux de 7 images **à
l'intérieur du mot « notre »** — le montage disait « Ouais. nd'étude » là où
la personne dit « Pour notre projet d'étude ». Le creux était réel dans
l'enveloppe (une occlusive laisse un trou), mais ce n'était pas une pause.

**300 à 320 ms / 4 images** est le bon compromis : les mots restent entiers et
le plus long creux tombe à 0,26 s. C'est le réglage par défaut à retenir.

Et surtout : **relis le texte monté après chaque changement de réglage.**
Aucun rapport d'opération ne dit qu'un mot a été coupé en deux ; seule la
transcription du son monté le montre.

### Deux limites à connaître

- `tighten_pauses` ne touche **ni la tête ni la queue** du plan : elle les
  publie (`head_air`, `tail_air`) et laisse. L'air de tête se rogne avec
  `ripple_trim` piloté par `analyze_speech_onset`.
- Chaque creux refermé **coupe le plan en deux**. Un montage resserré à 250 ms
  passe de 6 blocs à 25 plans : autant de faux raccords à couvrir. C'est le
  prix du resserrement, et c'est ce qui justifie les plans de coupe.

---

## Les pièges de pilotage qui coûtent un aller-retour

Aucun ne lève d'erreur parlante. Tous ont été rencontrés en conditions réelles.

### Les spans appartiennent à la timeline active

`get_timeline_transcript` et `create_interview_short` lisent la timeline
**active**. Après avoir fabriqué un montage, l'actif est ce montage : demander
`S276` là-dedans rend `unknown span_id 'S276'`, alors que le span existe
parfaitement dans le dérushage.

**Repasse sur le dérushage avant chaque nouvelle sélection**
(`set_active_timeline`). Et méfie-toi de la suite : quand la création échoue,
l'actif ne bouge pas, donc l'opération suivante — un resserrement, par
exemple — s'applique au **dérushage**. C'est arrivé : 92 creux refermés dans
la matière brute au lieu du film. Mets un garde-fou qui refuse toute opération
de montage quand l'actif est le dérushage.

### `ripple_trim` n'emporte pas ce qu'on croit

`tighten_pauses` emporte le plan son lié tout seul et le dit dans `also_cut`.
**`ripple_trim`, non.** Rogner la tête d'un plan vidéo laisse le son en place,
décale l'image seule et pose un trou en fin de piste vidéo — **sans changer la
durée de la timeline**, donc le contrôle « la durée a-t-elle bougé ? » ne voit
rien.

Il faut nommer explicitement :

- `linked_clip_ids` : le plan son de la paire ;
- `sync_track_ids` : la piste de coupe, qui n'appartient à aucun groupe lié et
  ne suivrait pas le décalage.

Contrôle après coup : les durées de V1 et de A1 doivent coïncider plan par
plan.

### Désigner le bon plan quand un rush en fournit plusieurs

Un même rush alimente souvent trois blocs du montage. « Le premier » ou « le
dernier » vise à côté. Trie les plans du rush par `source_in` et nomme le rang.
Et applique les corrections de bordure **avant** le resserrement : après, le
rush est éclaté en dix morceaux et le rang ne désigne plus rien.

### Le chemin du projet, c'est le `.json`, pas le dossier

Toutes les commandes veulent
`…/NOM.cutmachine-project/project.cutmachine.json`. Passe le dossier et tu
reçois `ParseError: unsupported project package (CUTMACHINE package v2
required)` — un message qui accuse le format du paquet alors que le paquet est
parfaitement valide : le contrôle cherche `manifest.json` à côté du chemin
donné, et à côté d'un dossier il n'y a rien. Ne perds pas dix minutes à
inspecter le manifeste.

### Les alias de `describe` se renumérotent

`A1`, `A2`… sont positionnels : ils changent après chaque découpe. Repère
toujours un plan par `source_in` ou par identifiant, jamais par alias.

---

## La question de l'intervieweur s'entend, et rien ne la signale

Le défaut le plus coûteux de cette série, parce qu'il est **invisible dans
toutes les transcriptions**.

whisper ne transcrit que le locuteur principal. Quand un rush commence par la
question de l'intervieweur, whisper la saute et pose le premier mot de la
réponse à l'image 0. `create_interview_short` construit l'entrée du plan
là-dessus, donc le montage démarre **sur la question**. Elle s'entend
parfaitement au montage ; elle n'apparaît dans aucun contrôle textuel.

Mesuré sur LISAASTR136 : six blocs sur huit montages ouvraient sur « Et c'est
quoi ? », « Qu'est-ce que c'est ? » ou l'équivalent.

### Pourquoi l'onset ne le voit pas

`analyze_speech_onset` mesure de la **parole**, pas *qui* parle. La question
est de la parole : l'onset la déclare « tight » et ne suggère aucun rognage.

### Le discriminateur fiable : le niveau

**L'intervieweur n'est pas micro-cravaté.** Sa question est systématiquement
**15 à 20 dB sous** la réponse. Sur un rush mesuré : question à −27/−49 dB,
réponse à −11/−14 dB, avec un creux net entre les deux.

Le repérage se fait donc sur l'enveloppe RMS 20 ms, avec un seuil **calé sur
le rush lui-même** — 90e centile moins ~9 dB — et une tenue de quelques
fenêtres pour ne pas déclencher sur un bruit :

```
seuil = centile90(niveaux) - 9 dB
début = première fenêtre qui tient >= 6 fenêtres au-dessus du seuil
```

Sur les rushes de cette série, ça rend des débuts de 12 à 25 images
(0,5 à 1,0 s) — cohérents avec la lecture manuelle de l'enveloppe.

### Quand le niveau ne suffit pas : le creux

Le discriminateur du niveau suppose que l'intervieweur est loin du micro.
Ça ne tient pas toujours. Sur ADS260 (C8009), la question et la réponse sont
à des crêtes voisines — +13 contre +18 dans l'échelle de l'enveloppe — et
seul **le creux entre les deux** sépare : question jusqu'à l'image 179,
silence 180-185, réponse à partir de 186.

Alors lis le creux, pas le niveau. Et méfie-toi des groupes tout faits :
`analyze_speech_onset` regroupe avec un écart de 200 ms par défaut, donc
**il fusionne la question et la réponse quand la personne enchaîne vite**.
Mesuré sur ce même rush : le premier groupe publié allait de 0 à 181 et
couvrait les deux, ce qui a placé l'entrée du bloc 65 images trop tard. Le
montage disait « **Long et fastidieux**, j'ai peint en tout… » là où elle dit
« Ça a été un processus long et fastidieux ». Descends `group_gap_ms`, ou lis
directement les niveaux 20 ms du cache `speechonset`.

### whisper épingle le premier mot du rush à l'image 0

Défaut jumeau du précédent, et il frappe **même quand il n'y a pas de
question**. whisper ne place pas le premier mot où il est : il le pose à zéro
et étire les suivants jusqu'à retomber sur la parole réelle vers le milieu du
fichier.

Mesuré sur les quatre rushes d'ADS260 où le bloc d'ouverture était pris en
tête, en comparant le premier mot annoncé au premier groupe de l'enveloppe :

| rush | whisper | enveloppe | dérive |
|---|---|---|---|
| C8008 | 0 | 34 | **+34** |
| C8012 | 0 | 29 | **+29** |
| C8013 | 0 | 94 | **+94** |
| C8014 | 0 | 45 | **+45** |

Un plan entré sur l'horodatage whisper commence donc par une à quatre
secondes de fond de salle — et `trim_boundary_air` les referme, mais après
coup, sur un montage déjà découpé.

**Prends toujours l'entrée du premier bloc d'un rush sur l'enveloppe.** En
revanche, à partir du premier vrai creux, whisper redevient juste : sur
C8008, la fin de « …un film d'animation. » est annoncée à 135 et l'enveloppe
décroche à 136. La dérive est un défaut de **tête de fichier**, pas un
décalage général.

Contrôle qui la révèle en une ligne, sans rien décoder : compare le premier
mot de la transcription au premier groupe de parole du cache. S'ils ne
coïncident pas à quelques images près, tous les horodatages de la première
phrase sont faux.

### Le contrôle à passer sur chaque montage

Pour chaque rush du montage, compare le `source_in` du **premier** bloc au
début mesuré. S'il est en deçà, la question est dedans.

Deux cas de correction, et le second est celui qu'on rate :

- le bloc commence avant la parole : `ripple_trim` de la tête sur le début
  mesuré ;
- **le premier morceau est entièrement dans la question** — fréquent après
  resserrement, qui coupe justement au creux entre question et réponse. Un
  morceau plus court que le rognage demandé **ne se rogne pas** : le moteur
  répond sans rien faire et la durée ne bouge pas. Il faut le `remove_clip`,
  puis rogner le suivant.

### Certains refus reviennent en texte, pas en `ok:false`

`create_interview_short` refuse deux spans « separated by more than a
breath » — utile, ça empêche de recoller deux morceaux entre lesquels la
personne a fait autre chose. Mais **le refus arrive parfois comme une chaîne
de texte**, pas comme `{"ok":false}`.

Un client qui ne teste que `ok` croit avoir réussi. La suite s'exécute alors
sur la timeline précédente — le dérushage si aucun montage n'existe encore —
et l'erreur ne se voit que trois opérations plus loin, sous une forme
incompréhensible (`IndexError` sur la liste des pistes, parce que le
dérushage n'a pas de piste vidéo à l'index 0).

Fais lever une exception sur toute réponse commençant par `ValidationFailed`,
`ParseError`, `NotFound`, `Conflict` ou `Refused`, même hors JSON. Et vérifie
le nom de la timeline active après chaque création.

### Quand un span refuse de se coller au suivant

Le refus « separated by more than a breath » se contourne en **coupant le
segment en deux** — mais souvent la vraie réponse est de ne pas vouloir ce
raccord. Sur un cas mesuré, `S3`/`S4` séparaient « Voici notre projet de fin
d'étude » de « Notre projet ici a porté sur… » : deux départs, dont le second
est le bon. Prends le second et **rallonge sa tête** avec `ripple_trim` sur
l'image du mot voulu — `ripple_trim` étend aussi bien qu'il rogne.

---

## La finition : les creux que le resserrement ne voit pas

`tighten_pauses` ne touche **ni la tête ni la queue** d'un plan. Après
resserrement, les seuls creux qui restent sont donc aux bordures — et ce sont
justement ceux qui s'entendent, puisque les creux internes ont été refermés.

Boucle de finition, à répéter jusqu'à ce qu'elle ne trouve plus rien :

1. mesurer les creux du son monté (`silencedetect -30dB`) ;
2. pour chaque creux ≥ 0,4 s, trouver le plan qui le contient ;
3. si le creux est **près du début** du plan → rogner la tête ;
   près de la **fin** → rogner la queue ;
4. supprimer les plans de moins de 6 images : à 0,2 s ce ne sont pas des
   plans, ce sont des clignotements.

Prends une marge large pour « près du début » — **25 images**, pas 10. Avec
une marge étroite, un creux à 15 images du début est classé « interne », on
lui applique un resserrement qui refuse d'y toucher, et il ne bouge jamais.
Mesuré : trois passes avec marge 25 ont fait tomber le silence de 3,4 % à
0,3 %, plus grand creux de 0,67 s à 0,19 s.

### L'enveloppe tranche, pas whisper

Le repérage de la bordure **ne se fait pas sur les horodatages whisper**. Cas
mesuré : whisper place « l'objectif, » à l'image 386, l'enveloppe montre du
silence jusqu'à 398 et la parole qui démarre à **399**. Treize images de
dérive, en plein milieu du fichier.

Symétriquement, ne rogne pas au jugé : couper la queue « douze images avant la
fin » a mangé la fin de « café-librairie », que le montage disait alors
« café libre ». **Lis l'enveloppe, prends l'image où le niveau décroche.**

Et après chaque rognage de bordure, **relis le texte monté**. C'est le seul
contrôle qui voit un mot amputé.

---

## Les plans de coupe servent le sens, pas le compteur

Ils ont deux fonctions, dans cet ordre :

1. **montrer ce dont la personne parle** au moment où elle en parle ;
2. couvrir les faux raccords que le resserrement a créés.

La première prime. Un plan de coupe posé « parce qu'il faut 40 % » et qui
montre autre chose que le propos est pire que pas de plan de coupe du tout :
il détourne l'attention au lieu de la porter.

### L'image peut porter une référence

La règle de l'antécédent a une exception, et elle est utile : **un objet
montré à l'écran est son propre antécédent.** Quand quelqu'un dit « lorsqu'on
va souffler dans **ce tuyau** » sans que le tuyau ait été nommé avant, le plan
de coupe qui montre le tuyau à cet instant règle la question. Encore faut-il
qu'il soit là *à cet instant* — d'où la première fonction.

### Règles de pose vérifiées

- **Jamais quelqu'un qui parle face caméra** en plan de coupe : ça se lit
  comme une faute de synchro. Vérifie à l'image, pas au nom de fichier.
- **Laisse-les courir.** Mieux vaut cinq coupes de 5 à 8 s que douze de 2 s.
  Les retours courts au visage donnent un montage nerveux et illisible.
- **Garde le visage pour les moments personnels.** Quand la personne dit ce
  qui la touche, on la regarde. C'est le seul endroit du film où l'image du
  visage vaut mieux qu'une maquette.
- **Pose en ordre croissant de position sur une piste vide.** `insert_clip`
  insère et fait un ripple : revenir enrichir une piste déjà peuplée décale
  tout ce qui est en aval, silencieusement. Pour densifier, `remove_track` +
  `add_track` et repose tout d'un coup.
- **Sources à 50 i/s** : `source_in` au débit de la source, `duration` au
  débit de la séquence.
- La piste de coupe n'appartient à aucun groupe lié : nomme-la dans
  `sync_track_ids` de chaque `ripple_trim`, sinon elle ne suit pas.

### Le compte des plans se lit après, pas avant

Une fois le propos tenu et les silences fermés, regarde les chiffres pour
détecter une anomalie — pas pour piloter. Sur cette série, les huit montages
finis tombent entre 20 et 30 plans/minute et 40 à 55 % de B-roll **sans que
ces valeurs aient été visées**. Elles sont la conséquence d'un resserrement
serré (beaucoup de faux raccords à couvrir) et d'un matériau très illustratif.

---

## L'ordre de travail, de bout en bout

Chaque étape produit ce dont la suivante a besoin. Prises dans le désordre,
elles se défont mutuellement — notamment les corrections de bordure, qui ne
sont plus adressables une fois le resserrement passé.

1. **Transcrire tous les rushes en verbatim**, en une seule fois (`media_ids`)
   pour ne charger le modèle qu'une fois.
2. **Classer les rushes** : parole / plan muet. whisper hallucine sur le
   silence, et un plan de coupe peut porter du son (voir plus bas).
3. **Analyser les attaques** (`analyze_speech_onset`) puis
   **`align_transcript --write`**. Avant toute décision de coupe.
4. **Lire toute la parole de la personne** et écrire son raisonnement en une
   phrase.
5. **Choisir les blocs**, entiers, dans un ordre qui préserve les références.
6. **Mesurer chaque borne sur l'enveloppe** avant de poser quoi que ce soit :
   entrée sur l'attaque moins 2 à 3 images, sortie sur le décrochage plus
   2 à 3. Les horodatages whisper ne servent qu'à *trouver* la phrase.
7. **Monter les blocs** — `create_interview_short` depuis le dérushage actif,
   ou `add_timeline` + `insert_clip` image/son + `set_clip_link` quand la
   sélection est en images source plutôt qu'en spans.
8. **Corriger les bordures** (`trim_boundary_air`) — *avant* le resserrement.
9. **Resserrer** (`tighten_pauses`, 300–320 ms / 4 images).
10. **Purger les questions d'intervieweur** (repérage au creux et au niveau).
11. **Finition** : fragments-éclair, creux de bordure, en boucle.
12. **Relire le texte du son monté** (`transcribe_timeline`). Tant qu'il n'est
    pas propre, ne pas continuer.
13. **Poser les plans de coupe** sur le sens.
14. **Couleur, puis livraison** : export, SRT, Resolve — voir plus bas ce qui
    est vraiment demandé.

Le contrôle du texte (12) est le seul qui attrape les mots amputés et les
enchaînements absurdes. **Fais-le au moins deux fois** : une fois le montage
posé, une fois après le resserrement.

### `transcribe_timeline` remplace toute la chorégraphie

Ce contrôle demandait avant une quarantaine de lignes hors moteur — extraire
le son de chaque plan avec `ffmpeg -ss/-t`, concaténer, habiller d'une vidéo
noire (`--ingest` refusait un fichier sans image, et voulait un dossier),
transcrire, relire le cache. **Ce n'est plus le cas.** `transcribe_timeline`
(outil MCP, ou `--transcribe-timeline`) décode le son des pistes audio aux
bornes des plans, le mixe directement dans whisper, et rend le texte. Aucun
fichier intermédiaire, aucun export.

Sur ADS260 il a attrapé les deux seules fautes du montage, invisibles
ailleurs : un bloc entré 65 images trop tard (« **Long et fastidieux**, j'ai
peint… ») et un bloc sorti 124 images trop tôt (« un projet de court métrage
qui a été, **ce ne sera pas du tout à l'aquarelle** » — « tout récemment
financé » manquait). Les deux venaient de bornes lues sur des groupes de
parole plutôt que sur l'enveloppe brute.

Le cache s'écrit dans `.cutmachine/timeline-transcripts/<id du montage>.json`
et **porte les mots aux positions du montage** — c'est ce qui sert à faire le
SRT (voir plus bas).

### La boucle de finition peut diverger — borne-la

Mesuré : sur un creux à cheval sur une jonction de blocs, la boucle a rogné
quatre fois la même queue et le creux a **grandi** (0,51 → 0,64 s) au lieu de
disparaître. Le trou n'appartenait pas au plan qui le contenait : il était
pour moitié la queue du plan sortant, pour moitié la tête du plan entrant.
Rogner un seul côté déplace la jonction sans fermer le trou.

Deux garde-fous :

- **borne le nombre de passes** et signale la non-convergence au lieu de
  boucler ;
- quand un creux enjambe une jonction, **mesure les deux côtés** et rogne les
  deux : la queue du sortant à l'image où le niveau décroche, la tête de
  l'entrant à l'image où il remonte. Sur le cas mesuré : queue à 356 au lieu
  de 359, tête à 379 au lieu de 368 — 0,64 s ramenés à 0,31 s en une fois.

Et laisse **2 à 3 images de battement** avant l'attaque plutôt que de couper
dessus : une coupe pile sur le premier phonème l'escamote (le montage disait
« .. c'est un sujet » au lieu de « Et sur le plan plus personnel, c'est un
sujet »).

---

## Renvoyer dans DaVinci Resolve

Le projet Resolve doit être **ouvert** : au lancement, Resolve part sur
« Untitled Project » et le pont rapporte 0 chutier sans erreur.

### Demande-toi d'abord si l'export est vraiment le livrable

Quand la commande est « remonte la timeline », le livrable est **la timeline
Resolve**, pas un fichier. Le monteur étalonne et sort le master lui-même
depuis Resolve ; rendre un `.mov` depuis CUTMACHINE ne fait que consommer
2,2 min par film et produire un fichier que personne n'ouvrira.

**Ne rends un master que si on le demande.** Ce qui reste dû dans ce cas :
la timeline renvoyée, et le SRT si le projet en portait un.

### L'ordre est piégeux : les sous-titres d'abord

`AppendToTimeline` pose l'élément de sous-titres **à la fin de ce que la
timeline contient déjà**, pas au début de sa piste. Sur une timeline de
montage déjà remplie, les cues atterrissent après le dernier plan et la
timeline double de longueur, sans erreur.

Construis donc dans cet ordre :

1. `CreateEmptyTimeline` ;
2. `AddTrack("subtitle")` ;
3. `AppendToTimeline` du SRT — il tombe à zéro, la timeline est vide ;
4. `AddTrack("video")` autant que de couches ;
5. les plans, avec leur `recordFrame` **absolu** (décalé du `GetStartFrame()`
   de la timeline : 90000 pour 01:00:00:00 à 25 i/s).

Ne mets **rien d'autre** dans le `clipInfo` du SRT que `mediaPoolItem` et
`trackIndex` : y ajouter `startFrame`, `endFrame`, `recordFrame` ou
`mediaType` fait planter le pont.

**Il n'y a pas de contournement, et `recordFrame` n'en est pas un.** Mesuré sur
un montage de 1853 images déjà rempli : piste de sous-titres neuve et vide,
`AppendToTimeline` pose quand même les 102 cues **à l'image 1853** et double la
timeline. Avec `recordFrame` mis à l'origine de la séquence, même résultat —
il est ignoré, sans erreur.

Donc **sous-titrer une timeline déjà montée demande de la reconstruire**. Deux
règles pour que ça ne coûte pas le travail du monteur :

- **construis à côté, jamais par-dessus** : une timeline sœur (`…_ST`), l'autre
  intacte tant que le relevé ne coïncide pas ;
- **relève d'abord tout ce que la timeline porte** — pas seulement les plans.
  Sur ADS260, onze plans portaient une transformation faite à la main :
  quatre recadrages à 1,52× avec un tilt différent chacun sur l'interview, et
  sept incrustations à 0,32× sur la couche d'illustration. Elles ne survivent
  qu'à un `GetProperty` avant / `SetProperty` après. Vérifie aussi les
  marqueurs, les compositions Fusion et les versions d'étalonnage : ce qui
  n'est pas relevé est perdu sans avertissement.

### Le point d'entrée source revient parfois une image plus bas

Autre asymétrie entre ce que Resolve accepte et ce qu'il rapporte : appelé
avec `startFrame: X`, `GetSourceStartFrame()` rend parfois **X − 1**. Mesuré
sur la reconstruction d'ADS260 : dix plans sur trente-trois, sans motif visible
(même rush, même cadence, mêmes voisins conformes).

Position et durée restent exactes, donc les coupes ne bougent pas — mais la
matière jouée glisse d'une image. Ne raisonne pas là-dessus, **mesure** :
construis, compare le relevé à l'original, réinjecte l'écart, reconstruis. Sur
le cas mesuré ça converge en deux passes. Borne le nombre de passes et abandonne
en laissant l'original intact plutôt que de boucler.

### Resolve met en cache le média importé, par son chemin

Le piège le plus coûteux de cette série. **Réécrire un `.srt` sur le disque ne
rafraîchit pas l'élément du Media Pool** : `ImportMedia` rend l'ancien, et la
timeline repart avec les cues de la version précédente.

Ça ne se voit pas dans le retour de l'API, et pas toujours dans la durée : sur
huit montages renvoyés, **sept portaient l'ancien sous-titrage**, et seuls
trois avaient une durée aberrante — les autres coïncidaient par hasard parce
que le nouveau montage était plus long que l'ancien SRT.

Deux parades :

- **importe depuis un nom de fichier neuf** à chaque révision
  (`…_st2.srt`), sur un chemin durable — pas `/tmp`, Resolve garde le lien ;
- **compte les éléments de sous-titres** de la timeline renvoyée et compare-le
  au nombre de cues du SRT. C'est le seul contrôle qui attrape le cache.

### Les illustrations à une autre cadence ne passent pas par le moteur

Les stockshots et les films d'élèves arrivent en 23,976 ou 24 i/s alors que le
montage est à 25. `--export-resolve-timeline` **refuse** ces plans, et il a
raison : il exige une durée qui tombe sur une image source entière, et aucune
durée n'est entière à la fois à 25 et à 23,976 — il faudrait 1001 images de
séquence, soit 40 s, pour retomber sur la grille.

La parade est de ne pas les faire porter par CUTMACHINE : monte la parole dans
le moteur, exporte, puis **écris la couche d'illustration directement dans le
fichier d'échange** avec `video_layer: 1` et `with_audio: false`. `path` doit
être celui de l'élément du Media Pool ; `send_timeline` ajoute la piste vidéo
qu'il faut.

Deux faits mesurés sur ce chemin :

- **Resolve conforme par la durée.** N images source occupent
  `N × 25 / cadence_source` images de séquence — vérifié sur les 137 plans de
  la timeline d'illustration d'ADS260, où l'entrée du plan suivant tombe
  exactement là où ce calcul la met.
- **Et il tronque, il n'arrondit pas.** Sur les neuf coupes posées, la durée
  rendue vaut à chaque fois le plancher de ce produit. Dimensionner à
  l'arrondi laisse la dernière coupe **une image trop courte** — donc une
  image d'interview qui clignote sous elle. Choisis la longueur source dont le
  plancher vaut la durée voulue, et positionne la coupe suivante sur cette
  durée-là, pas sur celle que tu visais.

Deux coupes contiguës posées sur les durées visées se chevauchent d'une
demi-image et Resolve écrase silencieusement : mesuré sur un carton de titre
suivi du plan suivant.

### La DA des sous-titres ne se lit par aucune API

Quand le monteur a stylé les cartons d'une timeline et demande de reporter ce
look sur les autres, rien dans le scripting Resolve ne le donne :
`timelineItem.GetProperty()` sur un carton ne rend que le bloc de transformation
(Pan, Tilt, ZoomX…), identique entre une timeline stylée et une timeline nue, et
les 157 réglages de `timeline.GetSetting()` sont identiques eux aussi. Le README
local ne documente aucune méthode de style de sous-titre.

**Le look est dans le `FieldsBlob` de la piste de sous-titres**, pas sur les
cartons. Une piste nue y porte deux champs (`NumLayers`,
`ExcludeTrackFromSequenceCaching`) ; une piste stylée en porte un troisième,
`EffectFiltersBA`, avec le descripteur de police Qt en clair
(`Fira Sans,13,-1,5,57,0` + `Medium`) et le reste compressé en zstd.

Le seul chemin est donc un aller-retour `.drt` — une archive zip contenant
`project.xml`, `MediaPool/Master/MpFolder.xml` et `SeqContainer/<uuid>.xml` :

1. `timeline.Export(chemin, resolve.EXPORT_DRT)` sur la timeline stylée, et
   relever le `<FieldsBlob>` qui suit `<SubtitleTrackVec>` ;
2. exporter chaque timeline à styler, y remplacer ce même blob, rezipper ;
3. `mediaPool.ImportTimelineFromFile(chemin)`.

Trois faits mesurés sur ITM270 :

- **le nom de la timeline importée vient du nom du fichier `.drt`**, pas du XML ;
- le contenu survit intact au round-trip (plans, couches, son, sous-titres,
  positions et durées relus à l'identique) ;
- le contrôle qui prouve la greffe est de **réexporter la timeline importée et
  de comparer son blob** à celui de la source. Rien d'autre ne le montre, et
  surtout pas l'API.

Comme la timeline doit de toute façon être reconstruite pour changer les cues
(voir l'ordre piégeux plus haut), fais les deux d'un coup : reconstruis avec le
nouveau SRT, puis greffe le blob à l'import.

### Les contrôles après renvoi

- `GetEndFrame() - GetStartFrame()` doit valoir **exactement** la durée du
  montage. Une timeline plus longue = le piège de l'ordre.
- **La piste d'interview ne doit avoir aucun trou** et la piste son doit
  compter autant de plans qu'elle. Un trou = une image noire au montage.
- **Relis les positions et les durées de la piste d'illustration** telles que
  Resolve les rapporte, pas telles que tu les as demandées : c'est là que la
  troncature de conformation se voit.
- Le nombre d'éléments de sous-titres doit valoir le nombre de cues du SRT.
  Sinon = le piège du cache.
- Le premier élément de sous-titre doit partir de zéro (à une image ou deux
  près, selon la première cue).
- Resolve refuse un nom déjà pris : pour republier une révision, supprime
  l'ancienne timeline (`DeleteClips` sur son MediaPoolItem) ou change de nom.

---

## Couleur, export, sous-titres

### La gestion couleur est désactivée par défaut

Rushes Sony ILME-FX30 en **S-Log3-Cine / S-Gamut3.Cine**, déclaré dans le
fichier annexe de tournage :

```sh
grep CaptureGamma 1_RUSHES/C7432M01.XML   # -> value="s-log3-cine"
```

Sans `set_color_management`, l'export sort plat et délavé :

```json
{"name":"set_color_management","arguments":{
  "enabled":true,"input_gamut":"sony_sgamut3_cine","input_transfer":"sony_slog3",
  "input_ycbcr_matrix":"bt709","input_range":"auto","working_gamut":"acescct",
  "output_gamut":"rec709","output_transfer":"rec709"}}
```

**Contrôle : c'est la saturation qui tranche, pas les noirs.** Compare la
`SATAVG` du livrable à celle du rush log correspondant, au même instant.
Mesuré sur LISAASTR136 : rushes entre **6 et 18**, livrables entre **12 et
56** — un facteur deux à quatre selon la matière. Les valeurs absolues ne
veulent rien dire : un studio blanc avec des maquettes blanches sort à 9 tout
en étant parfaitement étalonné. `YMIN` ne prouve rien du tout.

### `--export` ne prend pas de dimensions

Il suit le format de séquence. Pour livrer en 1080×1920 depuis une séquence
2160×3840 : `update_sequence`, export, **puis remettre** la séquence à son
format de travail. Ne passe **jamais** `--overwrite` sur un nom de livrable
existant — supprime l'ancien fichier d'abord.

exFAT n'a pas de liens durs : si un export meurt à 100 %, c'est là (corrigé
par un repli `rename` dans `Exporter::Run`).

### Le SRT se fait depuis le son monté, plus depuis le livrable

`--export-srt` produit toujours des cues fausses, et pour une raison
identifiée : il les construit sur les transcriptions **des rushes**, donc il
hérite de la dérive de tête de whisper. Sur ADS260, sa première cue disait
« m'appelle Alix, je suis en train de » — « Salut, je » perdu, parce que ces
mots sont annoncés aux images 0-21 d'un plan qui entre à 32.

Mais il n'y a plus besoin de rendre le master pour autant. Le cache de
`transcribe_timeline` — `.cutmachine/timeline-transcripts/<id>.json` — porte
les mots **aux positions du montage**, mesurés sur le son réellement assemblé.
Transcris **sans `--verbatim`** (un sous-titre ne porte pas les hésitations)
et groupe en cues sur une fin de phrase, un creux d'une douzaine d'images, ou
~42 caractères.

Contrôle : la dernière cue doit tomber sur la durée du film, à l'image près.
Sur ADS260 : dernier mot à l'image 2115, montage à 2115.

### whisper pose le début des mots trop tôt — la transcription de Resolve le dit

Le défaut ne se voit pas sur un mot isolé, seulement à l'usage : le carton
change avant qu'on ait fini d'entendre le précédent. Sur ADS260 le monteur a
repris **23 frontières sur 92** à la main, toutes dans le même sens.

Le juge de paix est la transcription de Resolve, qui est un autre moteur :
`timeline.CreateSubtitlesFromAudio()`. Mesuré sur 52 ancres — les débuts *et*
les fins de ses cartons, appariés aux mots de whisper :

| mesure | valeur |
|---|---|
| Resolve − whisper, sur l'ensemble | **+2 images** de médiane |
| aux frontières que le monteur a corrigées | **+3,7 images** |
| aux frontières qu'il a gardées | **+1,6 image** |
| corrélation entre la dérive mesurée et sa correction | **+0,55** |

La dérive est donc réelle, et elle est **deux fois plus forte exactement là où
elle s'entend**. Recale les mots de whisper sur l'échelle de Resolve par
interpolation entre les ancres, **et seulement au-delà de 5 images d'écart** :
en deçà on déplace tout sans que personne l'entende. Sur ADS260 ce réglage
recale 94 mots sur 264 et fait tomber l'erreur aux 23 frontières corrigées de
**9,17 à 4,91 images** de moyenne, pour 1,8 image de déplacement sur celles
que le monteur avait validées.

**Ce que ça ne corrige pas.** Un tiers de ses reprises reste — un bloc à +6/+9
images où les deux moteurs sont d'accord. C'est du goût, pas de la dérive : ne
cherche pas à le modéliser.

### Deux pièges de `CreateSubtitlesFromAudio`

- **Elle échoue en silence hors de la page Edit.** Appelée depuis la page
  Deliver elle rend `False` en zéro seconde, sans message. `resolve.OpenPage("edit")`
  d'abord.
- **Les clés de réglage ne sont pas des chaînes.** `{"AutoCaptionLanguage": 5}`
  rend `False` ; les vraies clés sont les constantes `resolve.SUBTITLE_LANGUAGE`,
  `SUBTITLE_CAPTION_PRESET`, `SUBTITLE_CHARS_PER_LINE`, `SUBTITLE_LINE_BREAK`,
  `SUBTITLE_GAP` — qui valent 0 à 4. Le README local
  (`/Library/Application Support/Blackmagic Design/DaVinci Resolve/Developer/Scripting/README.txt`,
  section « Auto Caption Settings ») les documente, avec **1 à 60 caractères par
  ligne** : le mot à mot est donc prévu par l'API. **Lis ce README avant de
  chercher en ligne** — il correspond à la version installée, la documentation
  publique non.
- **Mais en 20.3.1 le dictionnaire échoue en silence.** Avec les bonnes clés
  numériques, l'appel rend `True`, ne crée aucune piste et ne produit aucun
  carton — quelle que soit la valeur, y compris celles qui reproduisent le
  comportement par défaut. Seul l'appel **sans argument** marche. Les réglages
  projet `limitSubtitleCPL` et `transcriptionLanguage` s'écrivent bien par
  `SetSetting` mais ne changent rien au résultat.
- **Et l'appel répété finit par bloquer le moteur.** Après une série d'essais,
  même l'appel nu rend `True` avec zéro carton en une seconde au lieu des ~20 s
  d'une vraie transcription. Rien ne le signale. Il faut redémarrer Resolve.
  **Fais tes essais de réglage en dernier**, une fois les ancres récoltées.

Faute de mot à mot, on récupère le découpage par défaut (42 caractères) : ses
42 cartons donnent quand même **52 ancres** avec leurs fins, une toutes les
1,4 seconde. C'est suffisant pour mesurer la dérive.

### La cadence de lecture, pas seulement le nombre de caractères

Les repères du métier (Netflix, BBC) : **20 caractères par seconde** au
maximum, **5/6 de seconde** (0,83 s) de durée minimale par carton, deux lignes
au plus. Mesuré sur ADS260 :

| jeu | cartons | durée médiane | CPS médian | sous 0,83 s | au-delà de 20 CPS |
|---|---|---|---|---|---|
| 18 caractères, une ligne | 102 | 0,72 s | 18,1 | 65 | 29 |
| la version corrigée à la main | 97 | 0,72 s | 17,9 | 57 | 38 |
| la transcription Resolve, 42 car. | 42 | 1,56 s | 20,9 | 4 | 22 |

Autrement dit **un réglage à 18 caractères sans garde entre cartons produit un
sous-titrage hors norme des deux côtés** : les deux tiers des cartons durent
moins que le minimum, un quart dépasse la vitesse de lecture. C'est un choix
assumé du format vertical, pas une erreur — mais dis-le, et ne va pas plus bas.

### Le nombre de caractères est par carton, pas par ligne

**La maison sous-titre à ~20 caractères par carton, sur une ligne.** Deux
lignes se tolèrent quand un morceau ne se coupe pas — 5 cartons sur 97 sur
ADS260, 5 sur 138 sur ITM270 — mais c'est l'exception, pas la cible.

La version précédente de cette section disait l'inverse : « la limite est une
cible par ligne, avec deux lignes tolérées ». C'était une déduction, pas une
mesure, et elle a coûté une livraison entière. Lue comme ça sur ITM270, elle a
donné 25 caractères par ligne sur deux lignes — jusqu'à **50 par carton**,
deux fois et demie le réglage demandé. Le monteur l'a corrigé en une phrase :
« l'idée des sous-titres c'est d'avoir aux alentours de 20 caractères ».

Le chiffre était pourtant déjà dans la table ci-dessus (« 18 caractères, une
ligne, 102 cartons ») : c'est l'interprétation qui était fausse. Ne la
redéduis pas.

### Des cartons sous 0,83 s, c'est le réglage qui veut ça

Conséquence directe : à 20 caractères et à un débit de pub, un carton dure
~0,8 s, et beaucoup passent juste en dessous du plancher métier. **Ne les
refusionne pas** — un garde-fou « tout carton sous 0,83 s est fusionné avec
son voisin » ramène mécaniquement les cartons à 40-50 caractères et annule le
réglage. Mesuré sur ITM270 : il a reconstitué des cartons de 45 caractères sur
deux lignes en une passe.

Ne fusionne que les vrais éclairs — **sous ~8 images** — et seulement quand le
texte fusionné tient encore dans le budget de lignes.

### Découpe les cartons en équilibrant, pas en remplissant

Remplir gloutonnement jusqu'à la limite déborde à chaque carton et laisse le
reste de la phrase seul sur le dernier : « du make-up sur » puis « toi. » à
0,56 s, « de maquillage ITM » puis « Paris. » à 0,40 s.

Découpe plutôt une phrase en `ceil(caractères / 20)` cartons et pose chaque
frontière **au mot le plus proche de la fraction idéale**, avec une prime aux
mots qui finissent par une virgule ou un point. Sur ITM270 ça a fait passer
les cartons de 4-30 caractères très irréguliers à une médiane de 17 à 19, sans
aucun orphelin.

Dernier détail qui se voit : **whisper accroche la ponctuation d'une phrase au
token suivant.** Sans reprise, un carton s'ouvre sur « ? T'inquiète » alors
que le point d'interrogation appartient au carton d'avant. Remonte toute
ponctuation en tête de carton sur le précédent — avec l'espace insécable que
le français met devant `? ! ; :`.

### Contrôles avant de rendre

- durée, résolution, cadence, présence du son (`ffprobe`) ;
- niveau moyen, et **niveau de la première seconde et demie** : s'il est
  nettement plus bas, il reste de l'air mort en tête. Mesuré : un montage
  ouvrait 20 dB sous sa moyenne, soit 1,44 s de silence ;
- ne compare jamais à un fichier sans vérifier qu'il existe : `ffprobe` sur un
  chemin absent sort une chaîne vide, et une « comparaison » avec du vide
  passe pour un résultat.

## Ce que ces vérifications ne voient pas

Elles attrapent les défauts, jamais l'ennui, et jamais l'incohérence d'un
propos. Un montage peut passer toutes les mesures ci-dessus et rester
inutilisable — c'est exactement ce qui est arrivé à la première version de
cette série : durées justes, silences fermés, sync propre, et un enchaînement
qui ne voulait rien dire.

Le seul garde-fou est la **relecture du texte monté**, faite comme quelqu'un
qui n'a pas vu les rushes.

Et rends toujours, avec les fichiers, ce que tu n'as pas pu juger : dis
explicitement que tu n'as ni vu ni entendu le résultat, et sur quoi reposent
tes affirmations.
