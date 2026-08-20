# Roadmap : fiabiliser la sauvegarde des projets

Cette roadmap transforme l'audit du 20 août 2026 en lots vérifiables. Elle
complète `ROADMAP.md` sans modifier les principes structurels de
`PHILOSOPHY.md` : le JSON canonique reste la vérité, l'historique reste une
donnée et toute modification du montage passe par une opération.

## Invariants transverses

1. Un succès de sauvegarde signifie que le projet, toutes ses timelines et
   tous ses journaux appartiennent à la même génération logique.
2. Après une erreur d'entrée-sortie, l'état affiché et l'état durable restent
   identiques. Une opération non persistée ne devient pas silencieusement
   l'état de travail courant.
3. Après un arrêt à n'importe quelle étape d'un commit, le prochain démarrage
   retrouve automatiquement soit l'ancienne génération complète, soit la
   nouvelle, puis peut proposer l'autosauvegarde.
4. L'autosauvegarde d'un projet multi-timeline se valide et se récupère comme
   un `Project`, avec ses identités stables. Elle ne remplace jamais le projet
   sans décision explicite de l'utilisateur.
5. Le manifeste décrit exactement les artefacts actifs. Supprimer une
   timeline ne laisse pas son montage ou son historique dans le package.
6. Les écritures annoncées comme durables sont synchronisées sur le stockage,
   y compris le répertoire qui porte les renommages atomiques.

## S1 — Récupération fonctionnelle

- Détecter le type canonique de l'autosauvegarde (`Document` ou `Project`) et
  appeler le parseur correspondant.
- Tester le chemin réel `WriteAutosave(Project) -> Inspect -> LoadAutosave` en
  états disponible, périmé et invalide.
- Inspecter l'autosauvegarde avant de refuser un projet principal incomplet ou
  endommagé, puis valider le package reconstruit.
- Ne supprimer l'autosauvegarde qu'après récupération validée ou choix
  explicite de l'ignorer.

Critère d'acceptation : un autosave multi-timeline plus récent provoque la
proposition de récupération utilisée par l'UI, même si le commit précédent a
été interrompu.

## S2 — Transaction multi-fichiers résistante aux arrêts

- Donner à chaque commit un identifiant de génération et un état de
  transaction découvrable au redémarrage.
- Préparer et synchroniser tous les fichiers avant publication.
- Restaurer automatiquement une génération incomplète à partir des backups,
  ou terminer sa publication lorsque cela est démontrablement sûr.
- Synchroniser les fichiers et répertoires aux frontières de durabilité.
- Injecter en test des états correspondant à un arrêt après préparation,
  backup partiel et publication partielle.

Critère d'acceptation : pour chaque point d'arrêt simulé, la réouverture rend
un ensemble entièrement ancien ou entièrement nouveau, jamais un mélange.

## S3 — Cohérence de l'interface en cas d'échec

- Appliquer les opérations et undo/redo à des copies temporaires de
  `Document` et `EditLog`.
- Ne publier ces copies dans `AppState` qu'après le commit disque réussi.
- Traiter explicitement chaque retour de persistance, notamment collage,
  suppression de plage, déplacement dans les bins, undo et redo.
- Conserver une erreur visible et actionnable lorsque le stockage refuse une
  écriture.

Critère d'acceptation : après une panne d'écriture injectée, le JSON canonique
de l'état affiché et les compteurs d'historique sont inchangés.

## S4 — Package exact et sans résidus

- Régénérer `manifest.json` à chaque commit sans perdre les empreintes des
  médias collectés.
- Ajouter et retirer les entrées de timelines avec le projet.
- Supprimer après commit les JSON et journaux appartenant à des timelines
  retirées, sans suivre de lien ni sortir du package.
- Vérifier qu'une collecte portable conserve uniquement les artefacts actifs.

Critère d'acceptation : après ajout puis suppression d'une timeline, le
manifeste, `Timelines/` et les journaux ne contiennent que les identifiants du
projet courant.

## S5 — Historique récupérable

- Étendre l'autosauvegarde à une enveloppe versionnée contenant le `Project`,
  les journaux de timelines et le journal projet, ou référencer une génération
  transactionnelle complète.
- Valider chaque journal avant promotion.
- Refuser une enveloppe incohérente sans modifier le projet enregistré.

Critère d'acceptation : après récupération, undo et redo ont les mêmes
compteurs et produisent les mêmes JSON qu'avant l'interruption.

Ce lot est séparé de S1 à S4 : restaurer d'abord le montage de façon fiable est
prioritaire, mais perdre silencieusement l'historique ne constitue pas l'état
final attendu.

## Validation finale

- `clang-format-18 -style=file` sur chaque fichier C++/Objective-C++ modifié ;
- `cmake --build build -j` ;
- `ctest --test-dir build --output-on-failure` ;
- `clang-tidy -p build` sur les `.cc`/`.mm` modifiés ;
- test manuel macOS : interruption forcée aux frontières de transaction,
  relance, récupération, undo/redo et contrôle du package.

## État au 20 août 2026

| Lot | État |
|---|---|
| S1 — Récupération fonctionnelle | implémenté et testé |
| S2 — Transaction résistante aux arrêts | implémenté et testé par états de crash simulés |
| S3 — Cohérence de l'interface | implémenté, smoke test AppKit vert |
| S4 — Package exact et sans résidus | implémenté et testé |
| S5 — Historique récupérable | implémenté et testé avec enveloppe v1 |
| Validation manuelle par arrêt forcé réel | à effectuer sur un projet de travail jetable |
