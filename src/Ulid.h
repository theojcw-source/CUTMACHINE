#pragma once

#include <string>

using Ulid = std::string;

Ulid GenerateUlid();
bool IsValidUlid(const Ulid& value);
