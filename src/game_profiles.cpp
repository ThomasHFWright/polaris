#include "game_profiles.h"
#include "private_state_file.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <nlohmann/json.hpp>
#include <set>
#include <random>
#include <stdexcept>
#include <tuple>

namespace game_profiles {
  namespace {
    bool valid_text(const std::string &text) {
      return text.find_first_not_of(' ') != std::string::npos &&
        std::none_of(text.begin(), text.end(), [](unsigned char ch) { return ch < 32 || ch == 127; });
    }

    std::string settings_error(const nlohmann::json &settings) {
      if (!settings.is_object()) return "settings must be an object";
      for (const auto &[name, edits] : settings.items()) {
        if (!valid_text(name)) return "setting names must be nonempty without control characters";
        if (!edits.is_object() || edits.empty()) return "setting edits must be nonempty objects";
        for (const auto &[field, value] : edits.items()) {
          if (field != "value" && field != "index") return "only value and index may be edited";
          if (field == "index" && (!value.is_number_integer() || value < 0)) return "index must be a nonnegative integer";
          if (!(value.is_string() || value.is_boolean() || value.is_number()) ||
              (value.is_number_float() && !std::isfinite(value.get<double>()))) return "setting fields must be finite scalars";
        }
      }
      return {};
    }

    std::string canonical_uuid(std::string_view value) {
      if (value.size() != 36) return {};
      std::string result(value);
      for (std::size_t i = 0; i < result.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
          if (result[i] != '-') return {};
        } else {
          const auto ch = static_cast<unsigned char>(result[i]);
          if (!std::isxdigit(ch)) return {};
          result[i] = static_cast<char>(std::tolower(ch));
        }
      }
      return result;
    }
  }

  bool parse(const nlohmann::json &rows, std::vector<profile_t> &profiles, std::string &error) {
    profiles.clear();
    error.clear();
    if (!rows.is_array()) {
      error = "game-profiles must be an array";
      return false;
    }
    std::vector<profile_t> parsed;
    std::set<std::tuple<std::string, int, int, int>> selectors;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto &row = rows[i];
      const auto fail = [&](const std::string &message) {
        error = "game-profiles[" + std::to_string(i) + "]: " + message;
        return false;
      };
      if (!row.is_object()) return fail("must be an object");
      profile_t profile;
      for (const auto &[key, value] : row.items()) {
        if (key != "name" && key != "client-uuid" && key != "width" && key != "height" &&
            key != "fps" && key != "file" && key != "settings") return fail("unknown field " + key);
        if (key == "settings") {
          if (const auto problem = settings_error(value); !problem.empty()) return fail(problem);
        } else if (key == "fps") {
          if (!value.is_number()) return fail("fps must be a number");
          const auto fps = value.get<double>();
          if (!std::isfinite(fps) || fps < 1 || fps > 1000 || std::round(fps * 1000.0) / 1000.0 != fps) {
            return fail("fps must be between 1 and 1000 with at most 3 decimal places");
          }
          profile.fps_millihertz = static_cast<int>(std::round(fps)) * 1000;
        } else if (key == "width" || key == "height") {
          const int maximum = 32768;
          if (!value.is_number_integer() || value < 1 || value > maximum) {
            return fail(key + " must be an integer between 1 and " + std::to_string(maximum));
          }
        } else {
          if (!value.is_string()) return fail(key + " must be a string");
          const auto &text = value.get_ref<const std::string &>();
          if (std::any_of(text.begin(), text.end(), [](unsigned char ch) { return ch < 32 || ch == 127; })) {
            return fail(key + " must not contain control characters");
          }
        }
      }
      for (const auto *key : {"name", "file", "settings"}) {
        if (!row.contains(key)) return fail(std::string(key) + " is required");
      }
      profile.name = row["name"].get<std::string>();
      if (profile.name.find_first_not_of(' ') == std::string::npos) return fail("name is required");
      profile.file = row["file"].get<std::string>();
      profile.settings = row["settings"];
      if (profile.file.find_first_not_of(' ') == std::string::npos && !profile.settings.empty()) return fail("file is required for nonempty settings");
      if (row.contains("client-uuid")) {
        profile.client_uuid = canonical_uuid(row["client-uuid"].get<std::string>());
        if (profile.client_uuid.empty()) return fail("client-uuid must be a UUID");
      }
      if (row.contains("width") != row.contains("height") ||
          (row.contains("fps") && !row.contains("width"))) return fail("width and height are required together and before fps");
      profile.width = row.value("width", 0);
      profile.height = row.value("height", 0);
      if (!selectors.emplace(profile.client_uuid, profile.width, profile.height, profile.fps_millihertz).second) {
        return fail("duplicate selector");
      }
      parsed.push_back(std::move(profile));
    }
    profiles = std::move(parsed);
    return true;
  }

  namespace {
    constexpr std::size_t max_file_bytes = 4 * 1024 * 1024;
    constexpr std::size_t max_backup_bytes = 64 * 1024 * 1024;
    using json = nlohmann::json;
    using private_state_file::write_status_e;

    void require(bool condition, const std::string &message) {
      if (!condition) throw std::runtime_error(message);
    }

    bool same_scalar_type(const json &a, const json &b) {
      return (a.is_number() && b.is_number()) ||
        (a.type() == b.type() && (a.is_string() || a.is_boolean()));
    }

    void find_options(json &node, const std::string &name, std::vector<json *> &found) {
      if (node.is_object()) {
        for (auto &[key, child] : node.items()) {
          if (key == "options" && child.is_array()) {
            for (auto &option : child) {
              if (option.is_object() && option.contains("name") && option["name"] == name) found.push_back(&option);
            }
          }
          find_options(child, name, found);
        }
      } else if (node.is_array()) {
        for (auto &child : node) find_options(child, name, found);
      }
    }

    json edit_options(json &document, const json &settings) {
      require(settings_error(settings).empty(), "Invalid controlled settings");
      json previous = json::object();
      // ponytail: one scan per managed option; index names if large profiles become necessary.
      for (const auto &[name, edits] : settings.items()) {
        std::vector<json *> found;
        find_options(document, name, found);
        require(found.size() == 1, "Expected exactly one option named " + name);
        auto &option = *found.front();
        for (const auto &[field, value] : edits.items()) {
          require(option.contains(field) && same_scalar_type(option[field], value) &&
                  (field != "index" || option[field].is_number_integer()), "Missing or incompatible " + name + "." + field);
          previous[name][field] = option[field];
          option[field] = value;
        }
        if (option.contains("values")) {
          const auto &values = option["values"];
          require(values.is_array(), "Invalid available values for " + name);
          require(option.contains("value") && std::find(values.begin(), values.end(), option["value"]) != values.end(), "Unavailable value for " + name);
          if (option.contains("index")) {
            const auto &index = option["index"];
            require(index.is_number_integer() && index >= 0 && index < values.size(), "Invalid index for " + name);
            require(values[index.get<std::size_t>()] == option["value"], "Value/index mismatch for " + name);
          }
        }
      }
      return previous;
    }

    std::filesystem::path journal_path(const std::filesystem::path &file) {
      return file.string() + ".polaris-profile.json";
    }

    json read_backup(const std::string &bytes, const std::filesystem::path &file) {
      auto backup = json::parse(bytes);
      require(backup.is_object() && backup.value("version", 0) == 1 &&
              backup.at("path") == file.string() && backup.at("bytes").is_string() &&
              backup.at("id").is_string() && backup.at("active").is_boolean() &&
              backup.at("previous").is_object() && !backup.at("previous").empty() &&
              settings_error(backup.at("previous")).empty(), "Invalid profile journal; manual recovery required");
      return backup;
    }

    std::string transaction_id() {
      std::random_device random;
      std::string id;
      for (int i = 0; i < 32; ++i) id += "0123456789abcdef"[random() & 15];
      return id;
    }

    void verify_backup(const private_state_file::read_result_t &current, const transaction_t &transaction) {
      require(bool(current), "Profile journal is unavailable; manual recovery required");
      auto observed = read_backup(current.payload, transaction.file);
      observed["active"] = true;  // An uncertain inactive-marker write is safe to retry.
      require(observed == transaction.backup, "Profile journal identity changed; manual recovery required");
    }
  }

  bool apply(const profile_t &profile, transaction_t &transaction, std::string &error) {
    error.clear();
    try {
      require(transaction.backup.empty(), "A profile transaction already owns cleanup");
      require(settings_error(profile.settings).empty(), "Invalid profile settings");
      if (profile.settings.empty()) return true;
      const std::filesystem::path file(profile.file);
      require(valid_text(profile.file) && file.is_absolute() && file.lexically_normal() == file,
              "Profile file must be an expanded canonical absolute path");
      transaction.file = file;
      const auto result = private_state_file::update_atomic(file, max_file_bytes,
        [&](const auto &current) -> std::optional<std::string> {
          require(bool(current), "Profile file must already exist");
          auto document = json::parse(current.payload);
          auto previous = edit_options(document, profile.settings);
          auto reversed = document;
          edit_options(reversed, previous);  // Prove the original controlled fields can be restored before journaling.
          auto prepared = document.dump(2) + "\n";
          require(prepared.size() <= max_file_bytes, "Profile file is too large");
          json backup = {{"version", 1}, {"path", file.string()}, {"bytes", current.payload},
                         {"previous", std::move(previous)}, {"active", true}, {"id", transaction_id()}};
          const auto payload = backup.dump();
          const auto saved = private_state_file::update_atomic(journal_path(file), max_backup_bytes,
            [&](const auto &existing) -> std::optional<std::string> {
              if (existing) {
                const json prior = read_backup(existing.payload, file);
                require(!prior.at("active").get<bool>(), "Active profile journal exists; manual recovery required before launch");
              }
              return payload;
            });
          if (saved.status != write_status_e::not_committed) transaction.backup = std::move(backup);
          require(saved.status == write_status_e::committed, "Profile backup was not durably committed; cleanup may be pending");
          return prepared;
        }, true, true);
      require(result.status == write_status_e::committed, "Profile file write failed or durability is uncertain; retain pending cleanup");
      return true;
    } catch (const std::exception &exception) {
      error = exception.what();
      return false;
    }
  }

  bool restore(transaction_t &transaction, std::string &error) {
    error.clear();
    if (transaction.backup.empty()) return true;
    try {
      require(transaction.backup.at("path") == transaction.file.string(), "Frozen profile target changed; manual recovery required");
      const auto journal = journal_path(transaction.file);
      const auto result = private_state_file::update_atomic(transaction.file, max_file_bytes,
        [&](const auto &current) -> std::optional<std::string> {
          require(bool(current), "Profile file is unavailable; manual recovery required");
          verify_backup(private_state_file::read_secure(journal, max_backup_bytes, false, false), transaction);
          auto document = json::parse(current.payload);
          edit_options(document, transaction.backup.at("previous"));
          return document.dump(2) + "\n";
        }, true, true);
      require(result.status == write_status_e::committed, "Profile restore failed or durability is uncertain; cleanup remains pending");
      const auto marked = private_state_file::update_atomic(journal, max_backup_bytes,
        [&](const auto &current) -> std::optional<std::string> {
          verify_backup(current, transaction);
          auto inactive = transaction.backup;
          inactive["active"] = false;
          return inactive.dump();
        });
      require(marked.status == write_status_e::committed, "Profile journal completion failed or durability is uncertain; retry cleanup");
      transaction = {};
      return true;
    } catch (const std::exception &exception) {
      error = exception.what();
      return false;
    }
  }

  const profile_t *select(const std::vector<profile_t> &profiles, std::string_view client_uuid,
                          int width, int height, int fps_millihertz) {
    const auto client = canonical_uuid(client_uuid);
    if (client.empty()) return nullptr;
    const auto rounded_fps_millihertz = std::round(fps_millihertz / 1000.0) * 1000;
    const profile_t *selected = nullptr;
    int best = -1;
    for (const auto &profile : profiles) {
      if (!profile.client_uuid.empty() && profile.client_uuid != client) continue;
      if (profile.width && (profile.width != width || profile.height != height)) continue;
      if (profile.fps_millihertz && profile.fps_millihertz != rounded_fps_millihertz) continue;
      const int rank = (!profile.client_uuid.empty() ? 3 : 0) + (profile.fps_millihertz ? 2 : profile.width ? 1 : 0);
      if (rank > best) {
        selected = &profile;
        best = rank;
      }
    }
    return selected;
  }
}  // namespace game_profiles
