#pragma once

#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace game_profiles {
  struct profile_t {
    std::string name;
    std::string client_uuid;
    int width = 0;
    int height = 0;
    int fps_millihertz = 0;
    std::string file;
    nlohmann::json settings = nlohmann::json::object();
  };

  struct transaction_t {
    std::filesystem::path file;
    nlohmann::json backup;
  };

  // Caller supplies an expanded canonical absolute file path and excludes game writers.
  // Nonempty backup owns cleanup, including when apply returns false after journaling.
  bool apply(const profile_t &profile, transaction_t &transaction, std::string &error);
  bool restore(transaction_t &transaction, std::string &error);

  // The same strict schema is used for HTTP saves and direct apps.json loads.
  bool parse(const nlohmann::json &rows, std::vector<profile_t> &profiles, std::string &error);
  const profile_t *select(const std::vector<profile_t> &profiles, std::string_view client_uuid,
                          int width, int height, int fps_millihertz);
}  // namespace game_profiles
