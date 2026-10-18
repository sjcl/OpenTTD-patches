/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file null_v.cpp The video driver that doesn't blit. */

#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-rebaseline-88cb61e3/source-8462e6cd/src/stdafx.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-rebaseline-88cb61e3/baseline/save-integrity/wpr_gate.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-rebaseline-88cb61e3/source-8462e6cd/src/openttd.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-rebaseline-88cb61e3/source-8462e6cd/src/date_func.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-rebaseline-88cb61e3/source-8462e6cd/src/passenger_journey_active.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-rebaseline-88cb61e3/source-8462e6cd/src/console_func.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-rebaseline-88cb61e3/source-8462e6cd/src/settings_type.h"
#include <charconv>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <vector>
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-rebaseline-88cb61e3/source-8462e6cd/src/gfx_func.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-rebaseline-88cb61e3/source-8462e6cd/src/blitter/factory.hpp"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-rebaseline-88cb61e3/source-8462e6cd/src/sl/saveload_func.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-rebaseline-88cb61e3/source-8462e6cd/src/window_func.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-rebaseline-88cb61e3/source-8462e6cd/src/thread.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-rebaseline-88cb61e3/source-8462e6cd/src/video/null_v.h"
#ifdef WITH_PASSENGER_BENCHMARK
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-rebaseline-88cb61e3/source-8462e6cd/src/console_func.h"
#endif

#include <atomic>

#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-rebaseline-88cb61e3/source-8462e6cd/src/safeguards.h"

/** Factory for the null video driver. */
static FVideoDriver_Null iFVideoDriver_Null;

extern std::atomic<bool> _exit_game;

const char *VideoDriver_Null::Start(const StringList &parm)
{
#ifdef _MSC_VER
	/* Disable the MSVC assertion message box. */
	_set_error_mode(_OUT_TO_STDERR);
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif

	this->UpdateAutoResolution();

	this->ticks = GetDriverParamInt(parm, "ticks", 1000);
	this->until_exit = GetDriverParamBool(parm, "until_exit");
	_screen.width  = _screen.pitch = _cur_resolution.width;
	_screen.height = _cur_resolution.height;
	_screen.dst_ptr = nullptr;
	ScreenSizeChanged();

	/* Do not render, nor blit */
	Debug(misc, 1, "Forcing blitter 'null'...");
	BlitterFactory::SelectBlitter("null");
	return nullptr;
}

void VideoDriver_Null::Stop() { }

void VideoDriver_Null::MakeDirty(int, int, int, int) {}

void VideoDriver_Null::MainLoop()
{
	SetSelfAsGameThread();
	if (this->until_exit) {
		while (!_exit_game) {
			::GameLoop();
			::InputLoop();
			::UpdateWindows();
		}
	} else {

        struct Sample { int64_t tick; uint64_t simulation, journey; };
        std::vector<Sample> samples;
        samples.reserve(this->ticks);
        int64_t start = 0;
        bool started = false;
        uint64_t loops = 0, error_resumes = 0;
        int64_t checkpoint = 0;
        int64_t rebuild_tick = 0;
        if (const char *value = getenv("OPENTTD_PASSENGER_CHECKPOINT_TICK")) std::from_chars(value, value + strlen(value), checkpoint);
        if (const char *value = getenv("OPENTTD_PASSENGER_REBUILD_TICK")) std::from_chars(value, value + strlen(value), rebuild_tick);
        while (!_exit_game && (!started || _state_ticks.base() - start < this->ticks)) {
            if (started && _pause_mode.Test(PauseMode::Error)) {
                ++error_resumes;
                IConsoleCmdExec("unpause");
            }
            int64_t before = _state_ticks.base();
            ::GameLoop();
            ::InputLoop();
            ::UpdateWindows();
            ++loops;
            if (!started && _game_mode == GameMode::Normal) {
                std::ifstream clock("on-1-console.txt");
                std::string line;
                while (std::getline(clock, line)) {
                    constexpr std::string_view prefix = "State ticks: ";
                    const auto position = line.find(prefix); // Console logs may prefix a wall-clock timestamp.
                    if (position == std::string::npos) continue;
                    auto parsed = std::from_chars(line.data() + position + prefix.size(), line.data() + line.size(), start);
                    if (parsed.ec != std::errc{}) continue;
                    started = true;
                    before = start;
                    break;
                }
                if (!started) { Debug(misc, 0, "Missing benchmark StateTicks"); _exit_game = true; }
            }
            if (started && _state_ticks.base() > before) {
                const auto &stats = GetPassengerJourneyTickDiagnostic();
                samples.push_back({_state_ticks.base(), stats.simulation_microseconds, stats.journey_microseconds});
                if (checkpoint > 0 && _state_ticks.base() - start == checkpoint) {
                    IConsoleCmdExec("passenger_journey_stats save_integrity");
                    std::rename("save-integrity.json", "transport-checkpoint.json");
                }
                if (rebuild_tick > 0 && _state_ticks.base() - start == rebuild_tick) {
                    /* A declared measurement transition between ticks. Keep its
                     * diagnostic/command time outside both simulation samples. */
                    const auto diagnostic_started = std::chrono::steady_clock::now();
                    IConsoleCmdExec("script midpoint-console.txt");
                    IConsoleCmdExec("passenger_journey_stats total");
                    IConsoleCmdExec("passenger_journey_stats save_integrity");
                    IConsoleCmdExec("script");
                    std::rename("save-integrity.json", "midpoint-save-integrity.json");
                    const auto setting_started = std::chrono::steady_clock::now();
                    IConsoleCmdExec("setting linkgraph.passenger_transfer_penalty 17");
                    const auto finished = std::chrono::steady_clock::now();
                    std::ofstream event("rebuild-transition.json");
                    event << "{\"elapsed_tick\":" << rebuild_tick << ",\"state_tick\":" << _state_ticks.base()
                        << ",\"cost\":" << _settings_game.linkgraph.passenger_transfer_penalty
                        << ",\"diagnostic_us\":" << std::chrono::duration_cast<std::chrono::microseconds>(setting_started - diagnostic_started).count()
                        << ",\"setting_us\":" << std::chrono::duration_cast<std::chrono::microseconds>(finished - setting_started).count() << "}\n";
                }
            }
            if (loops == 1 || loops % 256 == 0) {
                std::ofstream progress("progress.json");
                progress << "{\"start\":" << start << ",\"end\":" << _state_ticks.base()
                    << ",\"samples\":" << samples.size() << ",\"pause_bits\":" << unsigned(_pause_mode.base()) << "}\n";
            }
            if (!started && loops > 4096) {
                Debug(misc, 0, "Benchmark did not advance StateTicks"); _exit_game = true;
            }
        }
        if (!PassengerWpr::Gate("wpr-finished.txt", "wpr-saved.txt")) _exit_game = true;
        std::ofstream out("actual-ticks.json");
        out << "{\"start_tick\":" << start << ",\"end_tick\":" << _state_ticks.base()
            << ",\"driver_loops\":" << loops << ",\"error_resumes\":" << error_resumes << ",\"samples\":[";
        bool first = true;
        for (const auto &sample : samples) {
            out << (first ? "" : ",") << '[' << sample.tick << ',' << sample.simulation << ',' << sample.journey << ']';
            first = false;
        }
        out << "]}\n";

	}

	/* If requested, make a save just before exit. The normal exit-flow is
	 * not triggered from this driver, so we have to do this manually. */
	if (_settings_client.gui.autosave_on_exit) {
		DoExitSave();
	}
    if (!this->until_exit && getenv("OPENTTD_PASSENGER_MEASURE") != nullptr) {
        IConsoleCmdExec("exec scripts/passenger_benchmark_end.scr 0");
    }
}

bool VideoDriver_Null::ChangeResolution(int, int) { return false; }

bool VideoDriver_Null::ToggleFullscreen(bool) { return false; }
