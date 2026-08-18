# CLAUDE.md

Les consignes de ce dépôt sont dans [`AGENTS.md`](AGENTS.md), partagé avec les
autres agents de code. **Lis-le avant toute modification** — il contient les
contraintes structurelles (opérations, `RationalTime`, ULID, moteur avant
interface), les conventions de code, et les quatre faits sur l'interface que
les agents successifs ont enfreints.

Rappels propres à une session Claude Code :

- `ctest --test-dir build --output-on-failure` n'est lancé par aucun job de CI.
  C'est à toi de le faire tourner avant de rendre, pas seulement de compiler.
- La compilation complète demande FFmpeg et whisper.cpp et n'est pas instantanée.
  Pour un changement qui ne touche que la couche C++ portable, la cible de test
  concernée suffit :
  `cmake --build build --target cutmachine_ui_theme_tests && ctest --test-dir build -R ui_theme`.
- Documentation et interface en français, commentaires de code en anglais.
- Ne lance pas l'application pour « vérifier visuellement » un changement d'UI
  sans le dire : rien de la Phase 2 n'a été validé à l'écran, et
  [`VISUAL_QA_CHECKLIST.md`](VISUAL_QA_CHECKLIST.md) est la procédure prévue
  pour ça.
