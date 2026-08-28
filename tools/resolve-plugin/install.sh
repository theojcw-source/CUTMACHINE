#!/bin/bash
# Installe le plugin CUTMACHINE dans le menu Scripts de DaVinci Resolve.
set -e

SCRIPTS="$HOME/Library/Application Support/Blackmagic Design/DaVinci Resolve/Fusion/Scripts/Utility"
HERE="$(cd "$(dirname "$0")" && pwd)"

echo "→ Dossier Scripts de Resolve..."
mkdir -p "$SCRIPTS"

echo "→ Copie du plugin et de son cœur..."
cp "$HERE/CUTMACHINE.lua" "$SCRIPTS/"
cp "$HERE/cutmachine_resolve_lib.lua" "$SCRIPTS/"

echo ""
echo "✓ Plugin installé."
echo "  1. Ouvre DaVinci Resolve et ton projet"
echo "  2. Workspace → Scripts → Utility → CUTMACHINE"
echo "  3. Renseigne le chemin du binaire et celui du projet CUTMACHINE"
echo ""
echo "  Le binaire est détecté seul s'il est dans le PATH ou dans"
echo "  build/cutmachine du dépôt. Les logs vont dans :"
echo "  ~/Desktop/CUTMACHINE_Resolve.log"
