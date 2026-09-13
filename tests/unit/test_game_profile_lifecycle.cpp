#include <gtest/gtest.h>
#include <src/process.h>
#include <src/file_handler.h>
#include "../tests_paths.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <thread>

#ifdef __linux__
namespace {
  struct ProcessProfileFixture : testing::Test {
    std::filesystem::path dir;
    proc::proc_t process;
    proc::ctx_t app;
    boost::process::v1::environment env {boost::this_process::environment()};

    decltype(config::video) saved_video = config::video;
    std::string saved_apps_path = config::stream.file_apps;

    void SetUp() override {
      dir = test_paths::root() / ("game-profile-" + std::to_string(getpid()));
      std::filesystem::create_directories(dir);
      env["PROFILE_FILE"] = (dir / "settings.json").string();
      app.working_dir = dir.string();
      app.exit_timeout = std::chrono::seconds(1);
      app.game_profiles.push_back({.name="Fixture", .file="$(PROFILE_FILE)", .settings={{"DLSS", {{"value", "prepared"}}}}});
      write("settings.json", R"({"data":[{"options":[{"name":"DLSS","value":"original","index":2}]}],"unrelated":"original"})");
      process.use_disposable_process_fixture_for_tests();
      auto launch_env = env;
      process.reload_configuration(proc::proc_t(std::move(launch_env), {}));
      config::video.linux_display.stream_mode = "desktop_display";
      config::video.linux_display.private_runtime = "labwc";
      config::video.linux_display.headless_mode = false;
      config::video.linux_display.use_cage_compositor = false;
      config::video.linux_display.auto_manage_displays = false;
      config::video.linux_display.streaming_output.clear();
      config::video.output_name.clear();
      config::stream.file_apps = (dir / "apps.json").string();
      write("apps.json", nlohmann::json{{"version",13},{"env",{{"PROFILE_FILE",(dir / "settings.json").string()}}},{"apps",nlohmann::json::array()}}.dump());
      app.id = "4242";
      app.uuid = "11111111-1111-4111-8111-111111111111";
      app.name = "Disposable profile fixture";
      app.cmd = "/bin/sh game.sh";
      app.scale_factor = 100;
      app.wait_all = true;
      app.prep_cmds = {{"/bin/sh prep.sh", "/bin/sh prep-undo.sh", false}};
      write("prep.sh", "grep -q prepared settings.json || exit 3; echo prep >> order\n");
      write("prep-undo.sh", "test ! -e game-running || exit 3; grep -q prepared settings.json || exit 4; echo prep-undo >> order\n");
      write("game.sh", "grep -q prepared settings.json || exit 4; "
            "trap 'rm -f game-running; echo game-stopped >> order; exit 0' TERM; "
            "echo game >> order; touch game-running; while :; do sleep 0.1; done\n");
    }
    void TearDown() override {
      process.set_game_profile_capture_failure_for_tests(false);
      process.terminate(false, false);
      config::video = saved_video;
      config::stream.file_apps = saved_apps_path;
      std::filesystem::remove_all(dir);
    }
    void write(const char *name, const std::string &value) { std::ofstream(dir / name) << value; }
    std::string read(const char *name) { return file_handler::read_file((dir / name).string().c_str()); }
    nlohmann::json settings() { return nlohmann::json::parse(read("settings.json")); }
    std::string value() { return settings()["data"][0]["options"][0]["value"]; }
    std::shared_ptr<rtsp_stream::launch_session_t> launch() {
      auto value = std::make_shared<rtsp_stream::launch_session_t>();
      value->paired_app_launch = true;
      value->unique_id = "11111111-1111-4111-8111-111111111111";
      value->device_name = "disposable-profile-test";
      value->requested_width = 1280; value->requested_height = 800; value->requested_fps = 60000;
      value->width = 1280; value->height = 800; value->fps = 60000;
      value->scale_factor = 100;
      value->mirror_desktop = true;
      value->stream_mode = "desktop_display";
      value->user_locked_display_mode = true;
      return value;
    }
    bool game_started() {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (!std::filesystem::exists(dir / "game-running") && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      return std::filesystem::exists(dir / "game-running");
    }
  };
}

TEST_F(ProcessProfileFixture, SurvivingGrandchildMustStopEvenAfterParentExits) {
  write("grandchild.sh", "sleep 10 &\n");
  std::error_code ec;
  boost::filesystem::path cwd(dir.string());
  boost::process::v1::group writers;
  auto parent = platf::run_command(false, true, "/bin/sh grandchild.sh", cwd, env, nullptr, ec, &writers);
  ASSERT_FALSE(ec);
  parent.wait();
  ASSERT_EQ(parent.exit_code(), 0);
  EXPECT_FALSE(proc::game_profile_group_stopped_for_tests(writers, false));
  EXPECT_TRUE(proc::game_profile_group_stopped_for_tests(writers, true));
}

TEST_F(ProcessProfileFixture, InvalidFileOrOptionFailsBeforeMutation) {
  const auto original = read("settings.json");
  app.game_profiles[0].file = (dir / "missing.json").string();
  EXPECT_EQ(process.execute(app, launch()), 422);
  EXPECT_FALSE(process.game_profile_pending_for_tests());
  app.game_profiles[0].file = "$(PROFILE_FILE)";
  app.game_profiles[0].settings = {{"Missing", {{"value", "prepared"}}}};
  EXPECT_EQ(process.execute(app, launch()), 422);
  EXPECT_FALSE(process.game_profile_pending_for_tests());
  EXPECT_EQ(read("settings.json"), original);
  EXPECT_FALSE(std::filesystem::exists(dir / "order"));
  EXPECT_FALSE(std::filesystem::exists(dir / "settings.json.polaris-profile.json"));
}

TEST_F(ProcessProfileFixture, HostInputViewerAndNoOpDoNotApply) {
  app.prep_cmds.clear();
  app.cmd = "/bin/true";
  for (const std::string mode : {"host", "input", "viewer", "no-op"}) {
    SCOPED_TRACE(mode);
    auto request = launch();
    request->paired_app_launch = mode != "host";
    request->input_only = mode == "input";
    request->watch_only = mode == "viewer";
    if (mode == "no-op") app.game_profiles[0].settings.clear();
    ASSERT_EQ(process.execute(app, request), 0);
    EXPECT_FALSE(process.game_profile_pending_for_tests());
    EXPECT_EQ(value(), "original");
    EXPECT_FALSE(std::filesystem::exists(dir / "settings.json.polaris-profile.json"));
    process.terminate(false, false);
  }
}

TEST_F(ProcessProfileFixture, DiskValidationDisablesOnlyBadAppAndPreservesProfilesAndSteamUndo) {
  const nlohmann::json profiles = {{{"name","Fixture"},{"file","$(PROFILE_FILE)"},{"settings",{{"DLSS",{{"value","prepared"}}}}},{"width",1280},{"height",800},{"fps",90}}};
  const nlohmann::json hooks = {{{"do",""},{"undo","steam -shutdown"}}};
  auto valid = nlohmann::json{{"name","Valid"},{"uuid","11111111-1111-4111-8111-111111111111"},{"game-profiles",profiles},{"prep-cmd",hooks}};
  auto invalid = valid; invalid["uuid"]="22222222-2222-4222-8222-222222222222"; invalid["name"]="Invalid";
  invalid["game-profiles"][0]["width"] = true;
  const auto path = dir / "apps.json";
  write("apps.json", nlohmann::json{{"version",13},{"apps",{valid,invalid}}}.dump());
  auto parsed = proc::parse(path.string());
  ASSERT_TRUE(parsed.has_value());
  const auto apps = parsed->get_apps();
  const auto found = std::find_if(apps.begin(),apps.end(),[](const auto &app){return app.name=="Valid";});
  ASSERT_NE(found,apps.end());
  ASSERT_EQ(found->game_profiles.size(),1);
  EXPECT_EQ(found->game_profiles[0].file,"$(PROFILE_FILE)");
  EXPECT_EQ(found->game_profiles[0].settings["DLSS"]["value"],"prepared");
  EXPECT_TRUE(std::none_of(apps.begin(),apps.end(),[](const auto &app){return app.name=="Invalid";}));
  const auto saved = nlohmann::json::parse(read("apps.json"));
  EXPECT_EQ(saved["apps"][0]["game-profiles"],profiles);
  EXPECT_EQ(saved["apps"][0]["prep-cmd"],hooks);
}

TEST_F(ProcessProfileFixture, ExecuteTerminateAndResumeKeepPreparationOrdering) {
  ASSERT_EQ(process.execute(app, launch()), 0);
  ASSERT_TRUE(game_started());
  auto resume = launch();
  resume->requested_fps = 120000;
  EXPECT_EQ(process.validate_resolved_profile_for_running_app(resume), 0);
  EXPECT_EQ(read("order"), "prep\ngame\n");
  process.terminate(false, true);
  EXPECT_EQ(read("order"), "prep\ngame\ngame-stopped\nprep-undo\n");
  EXPECT_EQ(value(), "original");
  EXPECT_FALSE(process.game_profile_pending_for_tests());
}

TEST_F(ProcessProfileFixture, ExecuteRefusesReapplyUntilRealTerminateRecoverySucceeds) {
  write("prep-undo.sh", "test ! -e game-running || exit 3; echo prep-undo >> order\n");
  ASSERT_EQ(process.execute(app, launch()), 0);
  ASSERT_TRUE(game_started());
  process.set_game_profile_capture_failure_for_tests(true);
  process.terminate(false, false);
  EXPECT_TRUE(process.game_profile_pending_for_tests());
  EXPECT_EQ(process.execute(app, launch()), 422);
  EXPECT_EQ(value(), "prepared");
  process.set_game_profile_capture_failure_for_tests(false);
  auto edited = settings();
  edited["unrelated"] = "game-edited";
  write("settings.json", "invalid");
  process.terminate(false, true);
  EXPECT_TRUE(process.game_profile_pending_for_tests());
  // Reloaded app, environment and client must not redirect pending restoration.
  app.game_profiles[0].file = (dir / "changed.json").string();
  env["PROFILE_FILE"] = (dir / "changed-env.json").string();
  process.reload_configuration(proc::proc_t(std::move(env), {app}));
  auto changed_client = launch();
  changed_client->unique_id = "22222222-2222-4222-8222-222222222222";
  EXPECT_EQ(process.execute(app, changed_client), 422);
  write("settings.json", edited.dump());
  // Launch retries frozen restoration before applying again, despite the changed environment.
  app.game_profiles[0].file = (dir / "settings.json").string();
  ASSERT_EQ(process.execute(app, changed_client), 0);
  ASSERT_TRUE(game_started());
  EXPECT_NE(read("order").find("prep-undo\nprep\ngame\n"), std::string::npos);
  process.terminate(false, false);
  EXPECT_EQ(value(), "original");
  EXPECT_FALSE(process.game_profile_pending_for_tests());
  EXPECT_EQ(settings()["unrelated"], "game-edited");
  EXPECT_TRUE(std::filesystem::exists(dir / "settings.json.polaris-profile.json"));
}

TEST_F(ProcessProfileFixture, MixedMainAndDetachedIsRejectedBeforeAnyPreparation) {
  app.detached = {"/bin/sh detached.sh"};
  write("detached.sh", "touch detached-started\n");
  EXPECT_EQ(process.execute(app, launch()), 422);
  EXPECT_EQ(value(), "original");
  EXPECT_FALSE(std::filesystem::exists(dir / "order"));
  EXPECT_FALSE(std::filesystem::exists(dir / "detached-started"));
  EXPECT_FALSE(process.game_profile_pending_for_tests());
}

TEST_F(ProcessProfileFixture, OrdinaryPrepFailureAndUndoOnlyCommandsRemainCompatible) {
  write("prep.sh", "exit 7\n");
  app.prep_cmds.push_back({"", "/bin/sh undo-only.sh", false});
  write("undo-only.sh", "echo undo-only >> order\n");
  ASSERT_EQ(process.execute(app, launch()), 0);
  ASSERT_TRUE(game_started());
  process.terminate(false, false);
  EXPECT_EQ(read("order"), "game\ngame-stopped\nundo-only\nprep-undo\n");
}
#endif
