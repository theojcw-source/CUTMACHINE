--[[
Tests du cœur pur du plugin Resolve.

    fuscript -l lua tools/resolve-plugin/tests/test_cutmachine_lua.lua

`fuscript` est l'interpréteur Lua 5.1 livré avec DaVinci Resolve. Ces tests
n'ouvrent ni Resolve ni fenêtre : le Media Pool est remplacé par des tables,
ce qui suffit puisque cutmachine_resolve_lib ne connaît du Media Pool que
quatre méthodes.
--]]

local here = (debug.getinfo(1, "S").source or ""):match("@?(.+[/\\])") or "./"
package.path = here .. "../?.lua;" .. package.path
local lib = require("cutmachine_resolve_lib")

local failures = 0
local function check(condition, message)
    if not condition then
        failures = failures + 1
        io.stderr:write("FAIL: " .. message .. "\n")
    end
end

-- Duck types of a MediaPoolItem and a Folder: the library only ever calls
-- GetClipProperty, GetUniqueId, GetName, GetClipList and GetSubFolderList.
local function clip(properties, uid)
    return {
        GetClipProperty = function(_, key) return properties[key] end,
        GetUniqueId = function(_) return uid end,
    }
end

local function folder(name, clips, folders)
    return {
        GetName = function(_) return name end,
        GetClipList = function(_) return clips or {} end,
        GetSubFolderList = function(_) return folders or {} end,
    }
end

local function media_pool()
    local rush = clip({
        ["File Path"] = "/rushes/C8015.MP4",
        ["Clip Name"] = "C8015.MP4",
    }, "5d45")
    local loose = clip({
        ["File Path"] = "/rushes/C8035.MP4",
        ["File Name"] = "C8035.MP4",
    })
    local timeline = clip({
        ["File Path"] = "",
        ["Clip Name"] = "DERUSH",
        ["Type"] = "Timeline",
    })
    local rosie = folder("Rosie", { rush })
    local rushes = folder("1_RUSHES", {}, { rosie })
    local tl = folder("TL", { timeline })
    return folder("Master", { loose }, { rushes, tl })
end

-- Arborescence
local bins, clips, skipped = lib.collect(media_pool())
local by_name = {}
for _, bin in ipairs(bins) do by_name[bin.name] = bin end
check(#bins == 3, "trois chutiers sont relevés")
check(by_name["1_RUSHES"] and by_name["1_RUSHES"].parent_key == "",
      "un chutier de premier niveau a la racine pour parent")
check(by_name["Rosie"] and
      by_name["Rosie"].parent_key == by_name["1_RUSHES"].key,
      "un sous-chutier pointe vers la clé de son parent")

-- Rushes
check(#clips == 2, "deux rushes traversent")
local by_clip = {}
for _, entry in ipairs(clips) do by_clip[entry.name] = entry end
check(by_clip["C8035.MP4"] and by_clip["C8035.MP4"].bin_key == "",
      "un rush de la racine porte la clé de chutier vide")
check(by_clip["C8035.MP4"] and by_clip["C8035.MP4"].resolve_uid == nil,
      "un rush sans identifiant Resolve n'en invente pas")
check(by_clip["C8015.MP4"] and
      by_clip["C8015.MP4"].bin_key == by_name["Rosie"].key,
      "un rush imbriqué garde son chutier")
check(by_clip["C8015.MP4"] and by_clip["C8015.MP4"].resolve_uid == "5d45",
      "l'identifiant Resolve est conservé")
check(by_clip["C8015.MP4"] and
      by_clip["C8015.MP4"].path == "/rushes/C8015.MP4",
      "le chemin fichier est conservé tel quel")

-- Écartés
check(#skipped == 1 and skipped[1].name == "DERUSH" and
      skipped[1].reason == "Timeline",
      "une entrée sans fichier est rapportée, pas ignorée")

-- Homonymes
local twins = folder("Master", {}, { folder("Jour 01"), folder("Jour 01") })
local twin_bins = lib.collect(twins)
check(#twin_bins == 2 and twin_bins[1].key ~= twin_bins[2].key,
      "deux chutiers frères homonymes reçoivent des clés distinctes")

-- Manifeste
local manifest = lib.build_manifest("ITM267", "20.3.1.6", media_pool())
check(manifest.schema == "cutmachine.resolve-manifest.v1",
      "le manifeste annonce le schéma que l'importeur attend")
check(manifest.project == "ITM267" and manifest.resolve_version == "20.3.1.6",
      "le manifeste identifie le projet et la version de Resolve")

-- Chemins
check(lib.project_json_path("/x/P.cutmachine-project") ==
      "/x/P.cutmachine-project/project.cutmachine.json",
      "un dossier de paquet se complète en fichier projet")
check(lib.project_json_path("/x/P.cutmachine-project/") ==
      "/x/P.cutmachine-project/project.cutmachine.json",
      "une barre oblique finale du Finder est tolérée")
check(lib.project_json_path("/x/P.cutmachine-project/project.cutmachine.json")
      == "/x/P.cutmachine-project/project.cutmachine.json",
      "un fichier projet est déjà le bon chemin")
check(lib.project_json_path("   ") == nil, "un chemin vide est refusé")
check(lib.package_path("/x/P.cutmachine-project/project.cutmachine.json") ==
      "/x/P.cutmachine-project",
      "le paquet se déduit du fichier projet")

-- Échappement
check(lib.shell_quote("Théo's") == "'Théo'\\''s'",
      "une apostrophe est échappée pour le shell")
check(lib.import_command("/bin/cm", "/p/x.json", "/tmp/m.json") ==
      "'/bin/cm' '--import-resolve' '/p/x.json' '/tmp/m.json' 2>&1",
      "la commande d'import est intégralement citée")

if failures == 0 then
    print("plugin resolve : tests passés")
else
    io.stderr:write(string.format("%d test(s) en échec\n", failures))
end
os.exit(failures == 0 and 0 or 1)
