#pragma once

// RESOLVE-2026-08 -- a DaVinci Resolve bin is not a file: it lives in the
// project database, and a .drp is an opaque archive. There is nothing to
// parse, so the only honest way in is to make Resolve talk. The Studio-only
// scripting API does that in `sidecar/resolve_bridge.py`, which flattens the
// Media Pool into the manifest this file consumes.
//
// The split matters: the bridge exports *identity and organisation only*
// (path, name, bin), never technical metadata. Resolve reports FPS as a
// float, and PHILOSOPHY.md principle 4 forbids a float from becoming a rate.
// FFmpeg stays the single source of truth for cadence and duration, through
// the same ProbeMediaMetadata the directory ingest already uses -- so a rush
// imported from Resolve is byte-identical to the same rush ingested from
// disk, and no import path can invent a time value.
//
// Parsing and planning are pure and testable without FFmpeg, a project on
// disk, or Resolve running (tests/resolve_import_tests.cc). Only
// ImportResolveCommand touches any of the three.

#include "Document.h"
#include "Operations.h"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

inline constexpr char kResolveManifestSchema[] =
    "cutmachine.resolve-manifest.v1";

struct ResolveManifestBin {
    // Opaque intra-manifest reference. Never stored: bins are addressed by
    // ULID once imported, and the manifest is not a document.
    std::string key;
    std::string name;
    // Empty designates the project root, which has no bin object.
    std::string parent_key;
};

struct ResolveManifestClip {
    std::string path;
    std::string name;
    std::string bin_key;
    // Resolve's own clip identifier, carried for diagnostics only. Media
    // identity on this side is the resolved file path, as in Ingest.cc.
    std::string resolve_uid;
};

struct ResolveManifest {
    std::string project;
    std::string resolve_version;
    std::vector<ResolveManifestBin> bins;
    std::vector<ResolveManifestClip> clips;
};

// Strict: rejects an unknown schema, a duplicate or empty bin key, an unknown
// parent, a cycle, an empty clip path, and a clip in an unknown bin. Every
// failure names what it refused.
bool ParseResolveManifest(const std::string& json, ResolveManifest& manifest,
                          std::string& error);

struct ResolveImportPlan {
    // Bins to create, parents first, with their ULIDs already generated.
    std::vector<AddBinOperation> new_bins;
    // Every manifest key, including "" for the root, mapped to its ULID.
    std::map<std::string, Ulid> bin_ids;
    // Manifest bins already present in the document under the same name and
    // parent. Re-importing the same manifest creates nothing.
    size_t reused_bins = 0;
};

// Matches manifest bins onto the document's existing bins by (name, parent)
// so a second import of a growing Media Pool is additive, not duplicating.
bool PlanResolveImport(const Document& document,
                       const ResolveManifest& manifest,
                       ResolveImportPlan& plan, std::string& error);

// Headless entry point: creates the bins through AddBinOperation, ingests each
// clip through the shared FFmpeg probe, files it with SetMediaBinOperation,
// and commits document, project and timeline log together. Bin creation and
// filing are journalized and undoable; the library insertion follows the same
// unjournalized path as IngestCommand (Ingest.cc), which is where that
// asymmetry already lives.
int ImportResolveCommand(const std::string& projectPath,
                         const std::string& manifestPath, std::string& output);
