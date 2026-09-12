#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>
#include <src/private_state_file.h>
#include <src/game_profiles.h>
#include <src/confighttp_validation.h>

namespace {
  constexpr auto client = "11111111-1111-4111-8111-111111111111";
  constexpr auto other = "22222222-2222-4222-8222-222222222222";
  nlohmann::json row(std::string name = "Default") {
    return {{"name", name}, {"file", "/tmp/UserSettings.json"}, {"settings", {{"DLSS", {{"value", "Balanced"}, {"index", 1}}}}}};
  }

  struct GameProfiles : testing::Test {
    std::vector<game_profiles::profile_t> profiles;
    std::string error, http_error;

    void reject(const nlohmann::json &rows) {
      EXPECT_FALSE(game_profiles::parse(rows, profiles, error)) << rows.dump();
      EXPECT_FALSE(error.empty());
      EXPECT_TRUE(profiles.empty());
      EXPECT_FALSE(confighttp::validation::validate_app_payload({{"game-profiles", rows}}, http_error));
      EXPECT_EQ(error, http_error);
    }
  };
}

TEST_F(GameProfiles, RoundedFpsModesPrecedenceAndOrdering) {
  auto rows = nlohmann::json::array({row(), row("Client"), row("Resolution"), row("Handheld"), row("4K60"), row("4K120"), row("Fractional"), row("4K90")});
  rows[1]["client-uuid"] = client;
  rows[2].update({{"width", 1280}, {"height", 800}});
  rows[3].update({{"client-uuid", client}, {"width", 1280}, {"height", 800}, {"fps", 90}});
  rows[4].update({{"client-uuid", other}, {"width", 3840}, {"height", 2160}, {"fps", 60}});
  rows[5].update({{"client-uuid", other}, {"width", 3840}, {"height", 2160}, {"fps", 120}});
  rows[6].update({{"width", 3840}, {"height", 2160}, {"fps", 23.976}});
  rows[7].update({{"client-uuid", other}, {"width", 3840}, {"height", 2160}, {"fps", 90}});
  struct Case { const char *uuid; int w, h, fps; const char *name; };
  for (int pass = 0; pass < 2; ++pass) {
    ASSERT_TRUE(game_profiles::parse(rows, profiles, error)) << error;
    for (auto c : {Case{client,1280,800,90000,"Handheld"}, {client,1280,800,60000,"Client"},
                   {other,3840,2160,60000,"4K60"}, {other,3840,2160,90000,"4K90"}, {other,3840,2160,120000,"4K120"},
                   {other,3840,2160,59940,"4K60"}, {other,3840,2160,119880,"4K120"},
                   {other,3840,2160,23976,"Fractional"}, {other,3840,2160,24000,"Fractional"}, {other,1280,800,60000,"Resolution"},
                   {other,1920,1080,60000,"Default"}}) {
      const auto *selected = game_profiles::select(profiles, c.uuid, c.w,c.h,c.fps);
      ASSERT_NE(selected, nullptr);
      EXPECT_EQ(selected->name, c.name);
    }
    std::reverse(rows.begin(), rows.end());
  }
}

TEST_F(GameProfiles, StrictSchemaAndHttpValidationShareErrors) {
  for (auto invalid : {
      nlohmann::json(nullptr), nlohmann::json::object(),
      nlohmann::json::array({{{"name","missing commands"}}}),
      nlohmann::json::array({{{"name","x"},{"do","x"},{"undo",""}}})}) {
    reject(invalid);
  }
  for (const auto &patch : std::vector<nlohmann::json>{
      {{"width",true}}, {{"width",1.0},{"height",800}}, {{"width",0},{"height",800}},
      {{"width",32769},{"height",800}}, {{"width",1280}}, {{"fps",60}},
      {{"width",1280},{"height",800},{"fps",60.0005}},
      {{"width",1280},{"height",800},{"fps",1000.001}},
      {{"width",1280},{"height",800},{"fps-millihertz",60000}},
      {{"width",18446744073709551615ull},{"height",800}},
      {{"client-uuid","bad"}}, {{"client-uuid",""}}, {{"name",""}}, {{"do","a\nb"}},
      {{"undo",false}}, {{"unknown",1}}}) {
    auto value = row(); value.update(patch);
    reject(nlohmann::json::array({value}));
  }
}

TEST_F(GameProfiles, NumericFpsBoundsPrecisionAndCanonicalDuplicates) {
  auto value = row(); value.update({{"width",1920},{"height",1080}});
  for (const auto &[fps, millihertz] : {std::pair{1.0, 1000}, {1.001, 1000}, {23.976, 24000},
                                      {59.499, 59000}, {59.5, 60000}, {59.94, 60000}, {60.0, 60000},
                                      {90.0, 90000}, {119.88, 120000}, {120.0, 120000}, {999.999, 1000000}, {1000.0, 1000000}}) {
    value["fps"] = fps;
    ASSERT_TRUE(game_profiles::parse(nlohmann::json::array({value}), profiles, error)) << error;
    EXPECT_EQ(profiles[0].fps_millihertz, millihertz);
    EXPECT_NE(game_profiles::select(profiles,client,1920,1080,millihertz),nullptr);
    EXPECT_NE(game_profiles::select(profiles,client,1920,1080,static_cast<int>(std::round(fps * 1000.0))),nullptr);
    EXPECT_TRUE(confighttp::validation::validate_app_payload({{"game-profiles",{value}}}, http_error)) << http_error;
  }
  for (const auto &fps : std::vector<nlohmann::json>{
      true, false, nullptr, "60", 0, -1, 0.001, 0.999, 59.9401, 1000.001,
      std::nextafter(59.94, 60.0), std::numeric_limits<double>::quiet_NaN(),
      std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
      18446744073709551615ull}) {
    value["fps"] = fps;
    reject(nlohmann::json::array({value}));
  }
  auto duplicate = value; duplicate["name"] = "Different";
  for (const auto &pair : std::vector<nlohmann::json>{{60,60.0}, {59.94,60}, {119.88,120}, {23.976,24}, {59.5,60}}) {
    value["fps"] = pair[0]; duplicate["fps"] = pair[1];
    EXPECT_FALSE(game_profiles::parse(nlohmann::json::array({value,duplicate}), profiles, error));
    EXPECT_NE(error.find("duplicate selector"), std::string::npos);
  }
  value["fps"] = 59.499; duplicate["fps"] = 59.5;
  ASSERT_TRUE(game_profiles::parse(nlohmann::json::array({value,duplicate}), profiles, error)) << error;
  for (const auto &[fps, name] : {std::pair{59499,"Default"}, {59500,"Different"}, {59940,"Different"}, {60000,"Different"}}) {
    const auto *selected = game_profiles::select(profiles,client,1920,1080,fps);
    ASSERT_NE(selected,nullptr);
    EXPECT_EQ(selected->name,name);
  }
}

TEST_F(GameProfiles, UuidCaseDuplicatesNoMatchAndLegacy) {
  auto value = row(); value["client-uuid"] = "ABCDEFAB-1111-4111-8111-111111111111";
  ASSERT_TRUE(game_profiles::parse(nlohmann::json::array({value}), profiles, error));
  EXPECT_NE(game_profiles::select(profiles,"abcdefab-1111-4111-8111-111111111111",1,1,1),nullptr);
  EXPECT_EQ(game_profiles::select(profiles,other,1,1,1),nullptr);
  EXPECT_EQ(game_profiles::select(profiles,"",1,1,1),nullptr);
  auto duplicate=value; duplicate["client-uuid"]="abcdefab-1111-4111-8111-111111111111"; duplicate["name"]="Different";
  EXPECT_FALSE(game_profiles::parse(nlohmann::json::array({value,duplicate}), profiles,error));
  EXPECT_FALSE(game_profiles::parse(nlohmann::json::array({row(),row("Another")}),profiles,error));
  EXPECT_TRUE(confighttp::validation::validate_app_payload({{"name","Legacy"}},error));
  EXPECT_TRUE(game_profiles::parse(nlohmann::json::array(),profiles,error));
  EXPECT_TRUE(profiles.empty());
  EXPECT_TRUE(game_profiles::parse(nlohmann::json::array({{{"name","No-op"},{"file",""},{"settings",nlohmann::json::object()}}}),profiles,error));
}

TEST_F(GameProfiles, NamedSettingsSchemaIsStrict) {
  for (const auto &patch : std::vector<nlohmann::json>{
      {{"file", ""}}, {{"file", "   "}}, {{"file", false}}, {{"file", "bad\npath"}},
      {{"settings", nullptr}}, {{"settings", nlohmann::json::array()}},
      {{"settings", {{"", {{"value", true}}}}}},
      {{"settings", {{"bad\nname", {{"value", true}}}}}},
      {{"settings", {{"DLSS", nlohmann::json::object()}}}},
      {{"settings", {{"DLSS", {{"default_value", true}}}}}},
      {{"settings", {{"DLSS", {{"value", nullptr}}}}}},
      {{"settings", {{"DLSS", {{"value", nlohmann::json::array()}}}}}},
      {{"settings", {{"DLSS", {{"value", std::numeric_limits<double>::infinity()}}}}}},
      {{"settings", {{"DLSS", {{"index", true}}}}}},
      {{"settings", {{"DLSS", {{"index", -1}}}}}},
      {{"settings", {{"DLSS", {{"index", 1.0}}}}}}}) {
    auto value = row(); value.update(patch);
    reject(nlohmann::json::array({value}));
  }
  for (const auto *key : {"file", "settings"}) {
    auto value = row(); value.erase(key);
    reject(nlohmann::json::array({value}));
  }
}

namespace {
  using json = nlohmann::json;
  using fault = private_state_file::write_fault_e;

  class GameProfileTransaction : public testing::Test {
  protected:
    std::filesystem::path directory, file, journal;
    game_profiles::profile_t profile;
    game_profiles::transaction_t transaction;
    std::string error, original;
    json document;

    void SetUp() override {
      auto pattern = (std::filesystem::temp_directory_path() / "polaris-profile-XXXXXX").string();
      const auto created = ::mkdtemp(pattern.data());
      ASSERT_NE(created, nullptr);
      directory = created;
      file = directory / "UserSettings.json";
      journal = file.string() + ".polaris-profile.json";
      document = {{"data", json::array({{{"options", json::array({
        {{"name", "DLSS"}, {"value", "Quality"}, {"index", 0}, {"values", {"Quality", "Balanced"}}},
        {{"name", "Other"}, {"value", 10}}})}}})}, {"unrelated", true}};
      original = document.dump(2) + "\n";
      write(file, original);
      ASSERT_EQ(::chmod(file.c_str(), 0644), 0);
      profile.file = file.string();
      profile.settings = {{"DLSS", {{"value", "Balanced"}, {"index", 1}}}};
      private_state_file::set_write_fault_for_tests(fault::none);
    }

    void TearDown() override {
      private_state_file::set_write_fault_for_tests(fault::none);
      if (!directory.empty()) std::filesystem::remove_all(directory);
    }

    static std::string read(const std::filesystem::path &path) {
      std::ifstream input(path, std::ios::binary);
      return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
    static void write(const std::filesystem::path &path, const std::string &bytes) {
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      output << bytes;
      ASSERT_TRUE(output.good());
    }
    json current() { return json::parse(read(file)); }
    json backup() { return json::parse(read(journal)); }
    bool apply() { return game_profiles::apply(profile, transaction, error); }
    bool restore() { return game_profiles::restore(transaction, error); }
  };
}

TEST_F(GameProfileTransaction, RestoresByNameOnlyAndRetainsFullBackupAndPermissions) {
  ASSERT_TRUE(apply()) << error;
  ASSERT_FALSE(transaction.backup.empty());
  EXPECT_EQ(backup()["bytes"], original);
  EXPECT_TRUE(backup()["active"]);
  struct stat metadata {};
  ASSERT_EQ(::stat(file.c_str(), &metadata), 0);
  EXPECT_EQ(metadata.st_mode & 0777, 0644);
  ASSERT_EQ(::stat(journal.c_str(), &metadata), 0);
  EXPECT_EQ(metadata.st_mode & 0777, 0600);
  auto changed = current();
  auto &options = changed["data"][0]["options"];
  std::reverse(options.begin(), options.end());
  options[0]["value"] = 42;
  options[1]["game-added"] = "keep";
  changed["unrelated"] = false;
  changed["new"] = 7;
  write(file, changed.dump());
  profile.file = "/unrelated/config/reload";
  profile.settings.clear();
  ASSERT_TRUE(restore()) << error;
  auto restored = current();
  EXPECT_EQ(restored["data"][0]["options"][1]["value"], "Quality");
  EXPECT_EQ(restored["data"][0]["options"][1]["index"], 0);
  EXPECT_EQ(restored["data"][0]["options"][1]["game-added"], "keep");
  EXPECT_EQ(restored["data"][0]["options"][0]["value"], 42);
  EXPECT_EQ(restored["unrelated"], false);
  EXPECT_EQ(restored["new"], 7);
  EXPECT_TRUE(transaction.backup.empty());
  EXPECT_FALSE(backup()["active"]);
  EXPECT_EQ(backup()["bytes"], original);
  ASSERT_EQ(::stat(file.c_str(), &metadata), 0);
  EXPECT_EQ(metadata.st_mode & 0777, 0644);
  EXPECT_TRUE(restore());
}

TEST_F(GameProfileTransaction, InvalidOptionsNeverMutateFileOrCreateBackup) {
  for (int scenario = 0; scenario < 7; ++scenario) {
    auto invalid = document;
    auto &options = invalid["data"][0]["options"];
    if (scenario == 0) options.push_back(options[0]);
    if (scenario == 1) options[0]["name"] = "Missing";
    if (scenario == 2) options[0]["value"] = true;
    if (scenario == 3) options[0].erase("index");
    if (scenario == 4) options[0]["values"] = {"Quality"};
    if (scenario == 5) options[0]["values"] = {"Balanced", "Quality"};
    if (scenario == 6) { invalid["elsewhere"] = options[0]; options.erase(0); }
    const auto bytes = invalid.dump(); write(file, bytes);
    EXPECT_FALSE(apply()) << scenario;
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(read(file), bytes);
    EXPECT_FALSE(std::filesystem::exists(journal));
    EXPECT_TRUE(transaction.backup.empty());
  }
}

TEST_F(GameProfileTransaction, RestoreValidationIsAtomicAndRetryable) {
  ASSERT_TRUE(apply()) << error;
  const auto applied = current();
  for (int scenario = 0; scenario < 4; ++scenario) {
    auto invalid = applied;
    auto &options = invalid["data"][0]["options"];
    if (scenario == 0) options.push_back(options[0]);
    if (scenario == 1) options[0]["name"] = "Missing";
    if (scenario == 2) options[0]["value"] = true;
    if (scenario == 3) options[0].erase("index");
    const auto bytes = invalid.dump(); write(file, bytes);
    EXPECT_FALSE(restore()) << scenario;
    EXPECT_EQ(read(file), bytes);
    EXPECT_FALSE(transaction.backup.empty());
    EXPECT_TRUE(backup()["active"]);
  }
  write(file, applied.dump());
  EXPECT_TRUE(restore()) << error;
}

TEST_F(GameProfileTransaction, OccupiedAndStaleJournalsAreNeverAdoptedOrOverwritten) {
  ASSERT_TRUE(apply()) << error;
  const auto saved = read(journal), applied = read(file);
  EXPECT_FALSE(apply());
  game_profiles::transaction_t fresh;
  EXPECT_FALSE(game_profiles::apply(profile, fresh, error));
  EXPECT_NE(error.find("manual recovery"), std::string::npos);
  EXPECT_TRUE(fresh.backup.empty());
  EXPECT_EQ(read(journal), saved);
  EXPECT_EQ(read(file), applied);
  ASSERT_TRUE(restore()) << error;
  EXPECT_TRUE(game_profiles::apply(profile, fresh, error)) << error;
  EXPECT_TRUE(game_profiles::restore(fresh, error)) << error;
}

TEST_F(GameProfileTransaction, ReplacedJournalAndChangedTargetCannotAuthorizeRestore) {
  ASSERT_TRUE(apply()) << error;
  const auto saved = read(journal), applied = read(file);
  auto replacement = backup(); replacement["id"] = "another transaction";
  write(journal, replacement.dump());
  EXPECT_FALSE(restore());
  EXPECT_EQ(read(file), applied);
  EXPECT_EQ(backup(), replacement);
  write(journal, saved);
  transaction.file = directory / "other.json";
  EXPECT_FALSE(restore());
  EXPECT_EQ(read(file), applied);
  transaction.file = file;
  EXPECT_TRUE(restore()) << error;
}

TEST_F(GameProfileTransaction, BackupFailuresAndUncertaintyNeverTouchTarget) {
  for (const auto injected : {fault::open, fault::short_write, fault::sync, fault::rename}) {
    private_state_file::set_write_fault_for_tests(injected);
    EXPECT_FALSE(apply());
    EXPECT_TRUE(transaction.backup.empty());
    EXPECT_EQ(read(file), original);
    EXPECT_FALSE(std::filesystem::exists(journal));
  }
  private_state_file::set_write_fault_for_tests(fault::post_rename_durability);
  EXPECT_FALSE(apply());
  EXPECT_FALSE(transaction.backup.empty());
  EXPECT_TRUE(backup()["active"]);
  EXPECT_EQ(read(file), original);
  private_state_file::set_write_fault_for_tests(fault::none);
  EXPECT_TRUE(restore()) << error;
}

TEST_F(GameProfileTransaction, RestoreWriteFailuresAndUncertaintyKeepCleanupPending) {
  ASSERT_TRUE(apply()) << error;
  const auto applied = read(file);
  private_state_file::set_write_fault_for_tests(fault::rename);
  EXPECT_FALSE(restore());
  EXPECT_EQ(read(file), applied);
  EXPECT_TRUE(backup()["active"]);
  private_state_file::set_write_fault_for_tests(fault::post_rename_durability);
  EXPECT_FALSE(restore());
  EXPECT_EQ(current(), document);
  EXPECT_FALSE(transaction.backup.empty());
  EXPECT_TRUE(backup()["active"]);
  private_state_file::set_write_fault_for_tests(fault::none);
  EXPECT_TRUE(restore()) << error;
}

TEST_F(GameProfileTransaction, VisibleInactiveMarkWithOwnedTransactionCanRetryDurably) {
  ASSERT_TRUE(apply()) << error;
  const auto frozen = transaction.backup;
  // Model a completion write that became visible but did not confirm durability.
  auto inactive = frozen;
  inactive["active"] = false;
  write(journal, inactive.dump());
  write(file, original);
  private_state_file::set_write_fault_for_tests(fault::sync);
  EXPECT_FALSE(restore());  // Matching target bytes still require a durable write.
  EXPECT_EQ(transaction.backup, frozen);
  EXPECT_EQ(backup(), inactive);
  EXPECT_EQ(read(file), original);
  private_state_file::set_write_fault_for_tests(fault::none);
  ASSERT_TRUE(restore()) << error;
  EXPECT_TRUE(transaction.backup.empty());
  EXPECT_EQ(current(), document);
  EXPECT_EQ(backup(), inactive);
  EXPECT_EQ(backup()["bytes"], original);
}

TEST_F(GameProfileTransaction, EmptySettingsAreNoOpAndMissingFileIsRejected) {
  profile.file.clear(); profile.settings = json::object();
  EXPECT_TRUE(apply());
  EXPECT_TRUE(transaction.backup.empty());
  EXPECT_FALSE(std::filesystem::exists(journal));
  profile.file = (directory / "missing.json").string();
  profile.settings = {{"DLSS", {{"value", "Balanced"}}}};
  EXPECT_FALSE(apply());
  EXPECT_TRUE(transaction.backup.empty());
  EXPECT_FALSE(std::filesystem::exists(profile.file));
}

TEST_F(GameProfileTransaction, ValueOnlyEditDoesNotOwnIndexOrOtherSameNamedObjects) {
  auto &option = document["data"][0]["options"][0];
  option.erase("values");
  document["outside-options"] = {{"name", "DLSS"}, {"value", "untouched"}};
  write(file, document.dump());
  profile.settings = {{"DLSS", {{"value", "Balanced"}}}};
  ASSERT_TRUE(apply()) << error;
  auto changed = current();
  changed["data"][0]["options"][0]["index"] = 9;
  write(file, changed.dump());
  ASSERT_TRUE(restore()) << error;
  EXPECT_EQ(current()["data"][0]["options"][0]["value"], "Quality");
  EXPECT_EQ(current()["data"][0]["options"][0]["index"], 9);
  EXPECT_EQ(current()["outside-options"]["value"], "untouched");
}

TEST_F(GameProfileTransaction, OriginallyInconsistentValueIndexCannotCreateUnrestorableBackup) {
  document["data"][0]["options"][0]["index"] = 1;  // Quality is index 0, while apply would fix both fields.
  const auto bytes = document.dump();
  write(file, bytes);
  EXPECT_FALSE(apply());
  EXPECT_NE(error.find("Value/index mismatch"), std::string::npos);
  EXPECT_EQ(read(file), bytes);
  EXPECT_FALSE(std::filesystem::exists(journal));
  EXPECT_TRUE(transaction.backup.empty());
}

TEST_F(GameProfileTransaction, BrowserIntegerValueCanEditAndRestoreFloatingPointField) {
  auto &option = document["data"][0]["options"][0];
  option["value"] = 1.0;
  option["values"] = {1.0, 2.0};
  write(file, document.dump());
  // JSON.stringify({value: 2.0, index: 1}) sends an integer value over HTTP.
  const auto browser = json::parse(R"({"name":"Numeric","file":"/placeholder","settings":{"DLSS":{"value":2,"index":1}}})");
  auto row = browser; row["file"] = file.string();
  std::vector<game_profiles::profile_t> parsed;
  ASSERT_TRUE(confighttp::validation::validate_app_payload({{"game-profiles", {row}}}, error)) << error;
  ASSERT_TRUE(game_profiles::parse(json::array({row}), parsed, error)) << error;
  profile = parsed.front();
  ASSERT_TRUE(apply()) << error;
  EXPECT_EQ(current()["data"][0]["options"][0]["value"], 2);
  ASSERT_TRUE(restore()) << error;
  const auto restored = current()["data"][0]["options"][0]["value"];
  EXPECT_TRUE(restored.is_number_float());
  EXPECT_EQ(restored, 1.0);
}

TEST_F(GameProfileTransaction, ExistingFloatingPointIndexRemainsInvalid) {
  document["data"][0]["options"][0]["index"] = 0.0;
  const auto bytes = document.dump(); write(file, bytes);
  EXPECT_FALSE(apply());
  EXPECT_EQ(read(file), bytes);
  EXPECT_FALSE(std::filesystem::exists(journal));
  EXPECT_TRUE(transaction.backup.empty());
}
