#pragma once

#include "Document.h"

#include <filesystem>
#include <string>
#include <vector>

// ALPHA-2026-08 -- Read-only semantic view for agent-driven interview edits.
// Every selectable span carries source-exact times copied from the transcript;
// the model selects ranges but never computes an edit boundary.

// One selectable stretch of speech on the audible timeline. `span_id` is the
// only handle a caller is meant to use: the exact times are published so a
// human reading the view can check them, not so a model can retype them.
// ResolveTranscriptSpanRange is what turns ids back into times, and it is
// the only intended path from one to the other (PHILOSOPHY.md principle 7).
struct TimelineTranscriptSpan {
    std::string span_id;  // "S1", "S2", ... in timeline order.
    Ulid source_id;
    RationalTime source_in;
    RationalTime duration;
    RationalTime timeline_in;
    std::string text;
};

// Collects the spans of every audible audio track, in timeline order, from
// the transcripts cached under `transcriptDirectory`.
bool BuildTimelineTranscriptSpans(
    const Document& document, const std::filesystem::path& transcriptDirectory,
    std::vector<TimelineTranscriptSpan>& spans, std::string& error);

std::string SerializeTimelineTranscriptSpans(
    const Document& document, const std::vector<TimelineTranscriptSpan>& spans);

// Reads back what SerializeTimelineTranscriptSpans wrote. Exists so a
// backend that can only hand out the rendered JSON view can still answer a
// structured span lookup without a second cache path; see McpBackend.h's
// default ReadTimelineTranscriptSpans.
bool ParseTimelineTranscriptSpans(const std::string& json,
                                  std::vector<TimelineTranscriptSpan>& spans,
                                  std::string& error);

bool DescribeTimelineTranscriptForAgent(
    const Document& document, const std::filesystem::path& transcriptDirectory,
    std::string& json, std::string& error);

// Resolves an inclusive run of span ids into the one exact source range it
// covers. This is the deterministic computation PHILOSOPHY.md principle 7
// asks for on the editorial path, and the counterpart of what
// ResolveWordRemoval already does on the cleanup path: the caller names
// *which spans* (semantic, agent-sized), this resolves *which frames*
// (exact, code-guaranteed).
//
// Taking a run rather than a single id is what lets a caller express an idea
// that outlives one span. Spans are cut for subtitle legibility -- about 42
// characters, or a sentence end, or a pause -- which is a display convention
// and not an editorial unit, so a single sentence routinely spans three of
// them. Merging them here, from ids, keeps that regrouping in code instead
// of asking a model to add two timecodes together.
//
// Refuses, rather than repairing, a run whose spans are not all from one
// source or are not contiguous in that source: a gap between two spans is
// silence or another speaker, and quietly swallowing it would produce a cut
// nobody asked for. `endSpanId` may be empty, meaning a run of one.
bool ResolveTranscriptSpanRange(
    const std::vector<TimelineTranscriptSpan>& spans,
    const std::string& startSpanId, const std::string& endSpanId,
    Ulid& sourceId, RationalTime& sourceIn, RationalTime& duration,
    std::string& error);
