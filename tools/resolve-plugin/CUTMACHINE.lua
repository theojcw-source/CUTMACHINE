--[[
CUTMACHINE — appels au moteur depuis DaVinci Resolve
----------------------------------------------------

Workspace → Scripts → Utility → CUTMACHINE

Une fenêtre, deux appels :
  • « Importer les chutiers » lit le Media Pool du projet ouvert, écrit le
    manifeste et le passe à `cutmachine --import-resolve`.
  • « Décrire le projet » appelle `cutmachine --describe` et résume ce que le
    moteur a en mémoire — de quoi vérifier ce que verra l'agent.

Script ONE-SHOT (modèle RunLoop/ExitLoop) : la fenêtre s'ouvre, travaille, et
se termine proprement à la fermeture. Aucun process zombie.

Zéro dépendance Python : le Media Pool est lu en Lua, le JSON passe par dkjson
(livré avec Fusion), et le moteur est appelé par io.popen. Comme le script
tourne *dans* Resolve, il ne réclame pas le scripting externe réservé à
Resolve Studio — contrairement à `sidecar/resolve_bridge.py`, qui produit le
même manifeste depuis un terminal.

Les logs vont dans ~/Desktop/CUTMACHINE_Resolve.log et la console Resolve.
--]]

local fu   = bmd.scriptapp("Fusion")
local ui   = fu.UIManager
local disp = bmd.UIDispatcher(ui)

local HOME     = os.getenv("HOME") or ""
local LOG_PATH = HOME .. "/Desktop/CUTMACHINE_Resolve.log"
local _log_fh  = io.open(LOG_PATH, "a")

local function _log(level, message, ...)
    local ok, formatted = pcall(string.format, message, ...)
    if not ok then formatted = tostring(message) end
    local line = string.format("%s [%s] %s\n", os.date("%H:%M:%S"), level,
                               formatted)
    if _log_fh then _log_fh:write(line) _log_fh:flush() end
    print(line)
end
local function log_info(m, ...) _log("INFO", m, ...) end
local function log_error(m, ...) _log("ERROR", m, ...) end

log_info("========== CUTMACHINE : démarrage ==========")

local json
do
    local ok, module = pcall(require, "dkjson")
    if not ok then
        log_error("dkjson introuvable : %s", module)
        error("dkjson requis")
    end
    json = module
end

local SCRIPT_DIR = (debug.getinfo(1, "S").source or ""):match("@?(.+[/\\])")
                   or "./"
package.path = SCRIPT_DIR .. "?.lua;" .. package.path
local lib = require("cutmachine_resolve_lib")

local STATE_DIR  = HOME .. "/.config/cutmachine"
local STATE_PATH = STATE_DIR .. "/resolve_plugin.json"

local function load_state()
    local file = io.open(STATE_PATH, "r")
    if not file then return {} end
    local text = file:read("*all")
    file:close()
    return json.decode(text or "") or {}
end

local function save_state(state)
    os.execute("mkdir -p " .. lib.shell_quote(STATE_DIR))
    local file = io.open(STATE_PATH, "w")
    if file then file:write(json.encode(state)) file:close() end
end

-- Runs a command and returns its combined output. The UI thread blocks for the
-- duration: an import that probes several hundred rushes takes a minute, and
-- Fusion's dispatcher has no worker thread we could hand this to. The status
-- line warns before the freeze rather than pretending it will not happen.
local function run(command)
    log_info("$ %s", command)
    local handle = io.popen(command)
    if not handle then return nil, "io.popen a échoué" end
    local output = handle:read("*all")
    handle:close()
    log_info("← %.400s", output or "")
    return output
end

local function file_exists(path)
    local file = io.open(path, "r")
    if file then file:close() return true end
    return false
end

-- The binary is not on Resolve's PATH in any reliable way, so the first usable
-- candidate wins and is then remembered in the state file.
local function find_binary(remembered)
    local candidates = { remembered, "/Volumes/code/CUTMACHINE/build/cutmachine",
                         HOME .. "/CUTMACHINE/build/cutmachine" }
    for _, candidate in ipairs(candidates) do
        if candidate and candidate ~= "" and file_exists(candidate) then
            return candidate
        end
    end
    local handle = io.popen("command -v cutmachine 2>/dev/null")
    if handle then
        local found = (handle:read("*all") or ""):match("^%s*(.-)%s*$")
        handle:close()
        if found ~= "" then return found end
    end
    return remembered or ""
end

local function decode_result(output)
    if not output or output == "" then
        return nil, "aucune sortie du binaire (chemin correct ?)"
    end
    -- CUTMACHINE writes one JSON object per command; anything before it is a
    -- loader or FFmpeg message worth showing verbatim when parsing fails.
    local candidate = output:match("({.*})%s*$")
    if not candidate then return nil, output end
    local decoded = json.decode(candidate)
    if not decoded then return nil, output end
    return decoded
end

local function current_project()
    local resolve = bmd.scriptapp("Resolve")
    if not resolve then return nil, nil, "Resolve ne répond pas" end
    local manager = resolve:GetProjectManager()
    local project = manager and manager:GetCurrentProject()
    if not project then return nil, nil, "aucun projet ouvert dans Resolve" end
    return resolve, project
end

--[[ Actions ]]

local function action_import(binary, project_path)
    local resolve, project, failure = current_project()
    if failure then return false, failure end

    local media_pool = project:GetMediaPool()
    local manifest = lib.build_manifest(project:GetName(),
                                        resolve:GetVersionString(),
                                        media_pool:GetRootFolder())
    log_info("Media Pool : %d rush(es), %d chutier(s), %d écarté(s)",
             #manifest.clips, #manifest.bins, #manifest.skipped)
    if #manifest.clips == 0 then
        return false, "aucun rush dans le Media Pool de ce projet"
    end

    local manifest_path = os.tmpname() .. ".json"
    local file = io.open(manifest_path, "w")
    if not file then return false, "manifeste temporaire non écrit" end
    file:write(json.encode(manifest))
    file:close()

    local project_json, path_error = lib.project_json_path(project_path)
    if not project_json then return false, path_error end
    if not file_exists(project_json) then
        local package = lib.package_path(project_json)
        log_info("Projet absent, création : %s", package)
        local created = decode_result(
            run(lib.create_command(binary, package, project:GetName())))
        if not created or created.ok ~= true then
            return false, "création du projet impossible : " ..
                          tostring(created and created.error or "voir le log")
        end
    end

    local decoded, raw = decode_result(
        run(lib.import_command(binary, project_json, manifest_path)))
    os.remove(manifest_path)
    if not decoded then return false, "réponse illisible : " .. tostring(raw) end
    if decoded.ok ~= true then
        return false, "import refusé : " .. tostring(decoded.detail)
    end

    local lines = {
        string.format("%d rush(es) importé(s), %d chutier(s) créé(s), %d réutilisé(s).",
                      decoded.added or 0, decoded.bins_created or 0,
                      decoded.bins_reused or 0),
    }
    if (decoded.skipped or 0) > 0 then
        lines[#lines + 1] = string.format("%d écarté(s) :", decoded.skipped)
        for _, entry in ipairs(decoded.errors or {}) do
            lines[#lines + 1] = "  • " .. tostring(entry.file) .. " — " ..
                                tostring(entry.reason)
        end
    end
    for _, entry in ipairs(manifest.skipped) do
        lines[#lines + 1] = "  • " .. tostring(entry.name) ..
                            " — non transmis (" .. tostring(entry.reason) .. ")"
    end
    return true, table.concat(lines, "\n")
end

local function action_describe(binary, project_path)
    local project_json, path_error = lib.project_json_path(project_path)
    if not project_json then return false, path_error end
    if not file_exists(project_json) then
        return false, "aucun projet CUTMACHINE à " .. project_json
    end
    local decoded, raw = decode_result(
        run(lib.describe_command(binary, project_json)))
    if not decoded then return false, "réponse illisible : " .. tostring(raw) end

    local used = 0
    for _, media in ipairs(decoded.library or {}) do
        if media.in_use then used = used + 1 end
    end
    local tracks = decoded.timeline and decoded.timeline.tracks or {}
    return true, string.format(
        "%d rush(es) dont %d monté(s), %d chutier(s), %d piste(s), %d marqueur(s).",
        #(decoded.library or {}), used, #(decoded.bins or {}), #tracks,
        #(decoded.markers or {}))
end

--[[ Fenêtre ]]

local state = load_state()
local win = disp:AddWindow({
    ID = "CutmachineWin",
    WindowTitle = "CUTMACHINE",
    Geometry = { 200, 200, 620, 420 },
    ui:VGroup{
        Spacing = 6,
        ui:Label{
            Text = "Appels au moteur CUTMACHINE depuis le projet Resolve ouvert",
            Weight = 0,
            StyleSheet = "font-weight:bold;color:#aaa;font-size:11px;",
        },
        ui:HGroup{ Weight = 0, Spacing = 4,
            ui:Label{ Text = "Binaire :", Weight = 0, MinimumSize = { 70, 0 } },
            ui:LineEdit{ ID = "Binary", Weight = 1 },
        },
        ui:HGroup{ Weight = 0, Spacing = 4,
            ui:Label{ Text = "Projet :", Weight = 0, MinimumSize = { 70, 0 } },
            ui:LineEdit{ ID = "Project", Weight = 1,
                         PlaceholderText = "…/Film.cutmachine-project" },
        },
        ui:Label{
            Text = "Le projet est créé s'il n'existe pas encore.",
            Weight = 0, StyleSheet = "color:#999;font-size:10px;",
        },
        ui:HGroup{ Weight = 0, Spacing = 4,
            ui:Button{ ID = "Import", Text = "Importer les chutiers", Weight = 1 },
            ui:Button{ ID = "Describe", Text = "Décrire le projet", Weight = 1 },
            ui:Button{ ID = "Close", Text = "Fermer", Weight = 0 },
        },
        ui:Label{ ID = "Status", Text = "Prêt.", Weight = 0,
                  StyleSheet = "color:#8c8;" },
        ui:TextEdit{ ID = "Log", Weight = 1, ReadOnly = true },
    },
})

win:Find("Binary").Text = find_binary(state.binary)
win:Find("Project").Text = state.project or ""

local log_lines = {}
local function ui_log(line)
    log_lines[#log_lines + 1] = line
    win:Find("Log"):SetText(table.concat(log_lines, "\n"))
end

local _closing = false
local function close()
    if _closing then return end
    _closing = true
    pcall(function() win:Hide() end)
    pcall(function() disp:ExitLoop() end)
end
win.On.CutmachineWin.Close = function(_) close() end
win.On.Close.Clicked = function(_) close() end

-- Both buttons share this: remember the fields, warn about the freeze, run,
-- then report. pcall keeps a Lua error inside the window instead of killing
-- the dispatcher and leaving Resolve with a dead script.
local function perform(action, busy_text)
    local binary = win:Find("Binary").Text or ""
    local project_path = win:Find("Project").Text or ""
    if binary == "" then
        win:Find("Status").Text = "Renseigne le chemin du binaire cutmachine."
        return
    end
    save_state({ binary = binary, project = project_path })

    win:Find("Import").Enabled = false
    win:Find("Describe").Enabled = false
    win:Find("Status").Text = busy_text
    ui_log(busy_text)

    local ok, success, message = pcall(action, binary, project_path)
    if not ok then
        success, message = false, "erreur interne : " .. tostring(success)
    end
    win:Find("Status").Text = success and "Terminé."
                              or "Échec — voir le détail."
    ui_log(message or "")
    log_info("Résultat : %s", tostring(message))

    win:Find("Import").Enabled = true
    win:Find("Describe").Enabled = true
end

win.On.Import.Clicked = function(_)
    perform(action_import,
            "Import en cours — la fenêtre reste figée le temps que le moteur "
            .. "sonde les rushes.")
end
win.On.Describe.Clicked = function(_)
    perform(action_describe, "Lecture du projet CUTMACHINE…")
end

win:Show()
disp:RunLoop()
log_info("RunLoop terminé → fermeture")
pcall(function() win:Hide() end)
if _log_fh then _log_fh:close() _log_fh = nil end
os.exit(0)
