# CUTMACHINE — résultats du spike XAVC S-I 4K

## Verdict

Le chemin technique est validé sur la machine de test Apple Silicon : deux
sources XAVC S-I 4K 25p sont décodées en logiciel par FFmpeg, transférées sous
forme de trois plans 10 bits chacune, converties en RGB et compositées par
Metal. L'application AppKit affiche le résultat et permet de changer de frame
avec un slider.

Le débit séquentiel rend deux pistes possibles. Le seek aléatoire n'est pas un
régime de fonctionnement viable à chaque frame et doit rester limité au
chargement, aux sauts lointains et aux vrais cache misses. Le cache et le
prefetch séquentiel sont donc des éléments nécessaires du produit, pas de
simples optimisations.

Le spike ne valide pas encore un scrub arrière fluide avec deux pistes. Cette
limite est localisée dans la reconstruction du cache en sens inverse sous
contention, et non dans le décodage séquentiel, l'upload ou le compositing GPU.

## Configuration testée

- macOS sur Apple Silicon, mémoire unifiée de 16 Go
- FFmpeg, décodage logiciel uniquement
- média Sony FX30 : 3840 × 2160, 25 fps, 168 frames
- H.264 High 4:2:2 Intra 10 bits, `yuv422p10le`, All-I
- `FF_THREAD_FRAME | FF_THREAD_SLICE` demandé
- FFmpeg a activé `FF_THREAD_FRAME` (`active_thread_type = 0x1`)
- 11 threads pour les mesures isolées initiales
- 5 threads par contexte pour le test à deux pistes
- cache global limité à 2,0 Go

Il n'y a ni VideoToolbox, ni autre hwaccel, ni `sws_scale`, ni conversion de
format sur le CPU.

## Résultats déterminants

| Mesure | Résultat | Interprétation |
|---|---:|---|
| Seek et décode à froid, accès aléatoire | 25,84 ms/frame | Latence d'un démarrage sans pipeline ; acceptable pour un saut ponctuel, pas pour chaque frame |
| Décode séquentiel, 168 frames, sans flush | 3,91 ms/frame | Le frame threading fonctionne ; facteur 6,6 face au régime à froid |
| Upload des trois plans avec `replaceRegion` | p95 2,71 ms/frame | L'upload synchrone n'est pas le goulot du spike |
| Estimation de deux pistes | 7,82 ms de décode + 5,42 ms d'upload | 13,24 ms avant compositing, dans un budget d'affichage de 16,67 ms mais avec peu de marge |

La mesure séquentielle jette les 20 premières frames afin de ne pas compter
l'amorçage du pipeline de frame threading. Aucun `avcodec_flush_buffers` n'est
effectué entre les frames. À l'inverse, le benchmark aléatoire effectue un
seek et un flush avant chaque échantillon : il mesure volontairement le coût à
froid.

La première lecture des 25,84 ms a failli conduire à déclarer la voie logicielle
inutilisable. Cette conclusion était incorrecte : le protocole détruisait le
pipeline de frame threading entre chaque mesure. Le chiffre décisif pour le
prefetch est le débit séquentiel de 3,91 ms/frame.

## Trace de scrub

Le protocole parcourt les 168 frames en avant, revient jusqu'à la première,
puis repart en avant. Les requêtes sont émises à 60 Hz, soit 502 échéances
d'affichage. Une frame est comptée comme drop si au moins une source ne possède
pas exactement l'index demandé à l'échéance ; le renderer affiche alors la
frame disponible la plus proche.

| Sources | Drops | Localisation |
|---|---:|---|
| Une piste | 5 | Tous pendant l'amorçage initial |
| Deux pistes | 75 | 2 au premier aller, 65 au retour, 8 au second aller |

Dans le dernier test dual, la fenêtre dérivée du budget est de 21 frames par
source : 16 dans la direction du prefetch, 4 de l'autre côté et la frame
courante. Le cache résident atteint environ 1,974 Go. Les 65 drops concentrés
sur le retour montrent que le prochain travail doit cibler le scrub inverse ;
une optimisation générale du renderer ne répondrait pas au signal observé.

ThreadSanitizer ne signale aucune race sur la trace complète à deux workers.
Les `AVFrame` conservées par le cache utilisent `av_frame_ref`/`av_frame_unref`,
et tous les appels Metal restent sur le thread principal avec une seule command
queue.

## Bugs silencieux révélés par l'instrumentation

### 1. Le prefetch s'auto-évinçait

La première fenêtre demandait 81 frames alors que le budget réel n'en gardait
qu'environ 74. Le worker décodait donc des frames que le LRU supprimait presque
immédiatement. Une variante du même défaut existait dans le prefetch inverse :
un nouveau bloc spéculatif était produit à chaque mouvement même si le bloc
précédent couvrait déjà la prochaine frontière.

La fenêtre est maintenant dérivée du budget : 70 % des frames finançables,
réparties entre les sources actives. Les 30 % restants couvrent la frame
affichée, les frames en vol, les références du renderer et la spéculation. Le
prefetch inverse ne produit un nouveau bloc que lorsque sa prochaine frontière
est réellement absente.

### 2. Une inversion redécodait des frames déjà en cache

Après un changement de direction, le cache pouvait contenir une longue plage
continue tandis que le curseur interne du décodeur pointait encore près de
l'ancien bloc. Au premier trou, le worker tentait de rejoindre ce trou en
redécodant séquentiellement toute la plage déjà en cache. Le hit rate restait
élevé, mais le prefetch perdait silencieusement la course avec le slider.

Le worker resynchronise désormais le décodeur directement sur le premier trou
si le rattrapage dépasse quatre frames. Cette correction a supprimé les drops
d'inversion sur la trace mono.

## Corrections apportées aux métriques

Les premiers percentiles de « latence de livraison » affichaient 0,000 ms. Ils
chronométraient en réalité un lookup dans la map sur des cache hits, pas une
livraison. Les métriques séparent maintenant :

- les hits, qui mesurent seulement l'accès au cache et restent proches de zéro ;
- les misses, mesurés entre la requête d'un index absent et son insertion dans
  le cache sous une forme affichable.

Le HUD expose aussi le hit rate glissant, les octets résidents, les drops
cumulés et les frames en vol. Sans cette séparation, un hit rate proche de
100 % masquait les rares misses responsables des drops visibles.

## Décisions conservées

- trois textures `MTLPixelFormatR16Unorm` par source, jamais deux ;
- `frame->linesize[i]` utilisé comme `bytesPerRow` ;
- remise à l'échelle `65535.0 / 1023.0` dans le shader ;
- conversion BT.709 video range et compositing exclusivement sur le GPU ;
- deux `AVCodecContext` indépendants à 5 threads pour deux pistes ;
- cache global de 2,0 Go, jamais un budget par piste ;
- affichage non bloquant : la frame exacte si disponible, sinon la plus proche.

## Suite éventuelle, hors du spike

Le spike s'arrête ici. Si le scrub arrière dual devient une exigence produit,
les expériences doivent être faites dans cet ordre :

1. Sur un miss arrière à l'index `N`, seek à `N - 24`, puis décoder les 24
   frames vers l'avant afin d'amortir un seek froid sur un bloc séquentiel.
2. Inverser dynamiquement la fenêtre avec la direction : environ 20 frames en
   arrière et 6 en avant pendant un retour, au lieu d'une répartition figée.
3. Mesurer seulement ensuite un contexte de seek dédié avec
   `FF_THREAD_SLICE`, si le média Sony contient effectivement plusieurs slices
   par frame.

La mesure slice threading n'est pas requise pour le verdict actuel et n'a pas
été réalisée. Elle ne doit donc pas être présentée comme un gain acquis.
