--[[
Cœur pur du plugin Resolve : parcours du Media Pool, manifeste, commandes.

Aucune dépendance à `bmd`, à l'UIManager ni à un Resolve lancé : tout ce
fichier travaille sur des tables et des objets passés en argument, ce qui le
rend testable avec `fuscript -l lua` sans ouvrir DaVinci Resolve
(tests/test_cutmachine_lua.lua). La fenêtre, elle, vit dans CUTMACHINE.lua.

Le manifeste produit ici est le même schéma que celui de
`sidecar/resolve_bridge.py` : deux producteurs, un seul format, un seul
importeur côté moteur. La version Lua a un avantage propre — elle tourne
*dans* Resolve, donc elle ne réclame pas le scripting externe réservé à
Resolve Studio.
--]]

local M = {}

M.SCHEMA = "cutmachine.resolve-manifest.v1"

-- Single-quoted shell escaping: the only form that survives arbitrary bytes,
-- including the accents and spaces that project and bin names are full of.
function M.shell_quote(value)
    return "'" .. tostring(value):gsub("'", "'\\''") .. "'"
end

local function trim(value)
    if value == nil then return "" end
    return tostring(value):match("^%s*(.-)%s*$")
end

-- Reads one clip property, tolerating both the Lua binding's nil and the
-- empty string Resolve returns for an unset field.
local function clip_property(clip, key)
    local ok, value = pcall(function() return clip:GetClipProperty(key) end)
    if not ok then return "" end
    return trim(value)
end

--[[
Walks a Media Pool folder into the flat bins/clips/skipped lists of the
manifest.

Bin keys are opaque references ("b1", "b2", ...) rather than name paths:
Resolve allows '/' inside a bin name and allows two siblings to share one, so
a path key would need escaping and would still collide. The root folder maps
to the empty parent key -- CUTMACHINE has no bin object for the project root.
Media Pool entries without a file path (timelines, compound clips, generators)
are reported as skipped rather than silently dropped.
--]]
function M.collect(root)
    local bins, clips, skipped = {}, {}, {}
    local counter = 0

    local function walk(folder, key)
        for _, clip in ipairs(folder:GetClipList() or {}) do
            local path = clip_property(clip, "File Path")
            local name = clip_property(clip, "Clip Name")
            if name == "" then name = clip_property(clip, "File Name") end
            if path == "" then
                local reason = clip_property(clip, "Type")
                if reason == "" then reason = "sans fichier" end
                skipped[#skipped + 1] =
                    { name = name, bin_key = key, reason = reason }
            else
                local entry = { path = path, name = name, bin_key = key }
                local ok, uid = pcall(function() return clip:GetUniqueId() end)
                if ok and uid and uid ~= "" then entry.resolve_uid = uid end
                clips[#clips + 1] = entry
            end
        end
        for _, child in ipairs(folder:GetSubFolderList() or {}) do
            counter = counter + 1
            local child_key = "b" .. counter
            bins[#bins + 1] = {
                key = child_key,
                name = child:GetName(),
                parent_key = key,
            }
            walk(child, child_key)
        end
    end

    walk(root, "")
    return bins, clips, skipped
end

function M.build_manifest(project_name, resolve_version, root)
    local bins, clips, skipped = M.collect(root)
    return {
        schema = M.SCHEMA,
        project = project_name or "",
        resolve_version = resolve_version or "",
        bins = bins,
        clips = clips,
        skipped = skipped,
    }
end

--[[
Accepts either the package directory or the project file inside it, and always
returns the file `--import-resolve` expects. A trailing slash is tolerated:
users drag folders in from the Finder, and the Finder adds one.
--]]
function M.project_json_path(value)
    local path = trim(value):gsub("/+$", "")
    if path == "" then return nil, "chemin de projet vide" end
    if path:match("%.json$") then return path end
    return path .. "/project.cutmachine.json"
end

-- The package directory a project file lives in, used to decide whether the
-- project must be created before it can be imported into.
function M.package_path(project_json)
    return (project_json:gsub("/project%.cutmachine%.json$", ""))
end

local function command(binary, ...)
    local parts = { M.shell_quote(binary) }
    for _, argument in ipairs({ ... }) do
        parts[#parts + 1] = M.shell_quote(argument)
    end
    -- stderr folded into stdout: a missing binary or a dyld failure has to
    -- reach the window, not vanish into the terminal Resolve never opened.
    return table.concat(parts, " ") .. " 2>&1"
end

function M.create_command(binary, package, name)
    return command(binary, "--create-project", package, name)
end

function M.import_command(binary, project_json, manifest_path)
    return command(binary, "--import-resolve", project_json, manifest_path)
end

function M.describe_command(binary, project_json)
    return command(binary, "--describe", project_json)
end

return M
