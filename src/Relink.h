#pragma once

#include "Document.h"

#include <string>
#include <vector>

struct RelinkReplacement {
    Ulid media_id;
    LibraryMedia media;
    std::string stored_path;
};

bool ValidateRelinkCandidate(const Document& document, const Ulid& mediaId,
                             const LibraryMedia& replacement,
                             std::string& error);
