# Plugin Resolve

Appeler le moteur CUTMACHINE depuis DaVinci Resolve, sans quitter l'application.

```sh
tools/resolve-plugin/install.sh
```

Puis, dans Resolve : `Workspace → Scripts → Utility → CUTMACHINE`.

## Ce que la fenêtre sait faire

- **Importer les chutiers** — lit le Media Pool du projet ouvert, écrit le
  manifeste, appelle `cutmachine --import-resolve`. Le projet CUTMACHINE est
  créé s'il n'existe pas encore. Le rapport affiche les rushes importés, les
  chutiers créés et **chaque rush écarté avec son motif**.
- **Décrire le projet** — appelle `cutmachine --describe` et résume ce que le
  moteur a en mémoire : rushes, rushes montés, chutiers, pistes, marqueurs.
  C'est la vue que verra l'agent.

## Pourquoi du Lua

Zéro dépendance : le Media Pool est lu en Lua, le JSON passe par `dkjson`
livré avec Fusion, le moteur est appelé par `io.popen`. Aucun interpréteur
Python à installer, aucune version à faire correspondre.

Surtout, le script tourne **dans** Resolve, donc il ne réclame pas le scripting
externe réservé à Resolve Studio. `sidecar/resolve_bridge.py` produit le même
manifeste depuis un terminal, mais lui exige Studio. Deux producteurs, un seul
schéma, un seul importeur côté moteur.

## Structure

| Fichier | |
|---|---|
| `cutmachine_resolve_lib.lua` | le cœur : parcours du Media Pool, manifeste, commandes. Aucune dépendance à `bmd` ni à l'UI. |
| `CUTMACHINE.lua` | la fenêtre : champs, boutons, journal. Modèle one-shot `RunLoop`/`ExitLoop`, sans process zombie. |
| `tests/test_cutmachine_lua.lua` | les tests du cœur, avec un Media Pool en tables. |

## Tests

```sh
"/Applications/DaVinci Resolve/DaVinci Resolve.app/Contents/Libraries/Fusion/fuscript" \
  -l lua tools/resolve-plugin/tests/test_cutmachine_lua.lua
```

`fuscript` est l'interpréteur Lua 5.1 de Resolve. Les tests n'ouvrent ni
Resolve ni fenêtre. `ctest` les lance automatiquement quand Resolve est
installé — **attention, `fuscript` sort toujours avec le code 0**, même sur
une erreur Lua : le test se juge sur sa sortie texte, pas sur son code retour.

## Limite connue

Pendant un appel, la fenêtre est figée : la boucle Fusion n'a pas de fil
d'exécution secondaire à qui déléguer, et sonder quelques centaines de rushes
prend une minute. Le bandeau d'état le dit avant de figer plutôt que de faire
comme si de rien n'était.
