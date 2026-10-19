/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */

/** @file passenger_precomputed_gui.cpp Non-modal progress for initial passenger tables. */
#include "stdafx.h"
#include "passenger_precomputed_gui.h"
#include "passenger_service_database.h"
#include "window_gui.h"
#include "window_func.h"
#include "strings_func.h"
#include "gfx_func.h"
#include "progress.h"
#include "core/geometry_func.hpp"
#include "openttd.h"
#include "network/network.h"
#include "table/strings.h"
#include <chrono>

#include "safeguards.h"

static PassengerInitialBuildProgress _initial_progress;
static std::optional<PassengerPrecomputedProgress> _progress;
static std::chrono::steady_clock::time_point _progress_update;
static uint _progress_indicator;

static StringID ProgressPhaseString(PassengerPrecomputedProgress::Phase phase)
{
	using Phase = PassengerPrecomputedProgress::Phase;
	switch (phase) {
		case Phase::Services: return STR_PASSENGER_PRECOMPUTED_SERVICES;
		case Phase::Graph: return STR_PASSENGER_PRECOMPUTED_GRAPH;
		case Phase::Restoring: return STR_PASSENGER_PRECOMPUTED_RESTORING;
		case Phase::Tables: return STR_PASSENGER_PRECOMPUTED_TABLES;
		case Phase::Complete: return STR_PASSENGER_PRECOMPUTED_COMPLETE;
		case Phase::MemoryRejected: return STR_PASSENGER_PRECOMPUTED_MEMORY;
		case Phase::Invalid: return STR_PASSENGER_PRECOMPUTED_INVALID;
		case Phase::Disabled: return STR_EMPTY;
		default: NOT_REACHED();
	}
}

Dimension GetPassengerProgressSize()
{
	Dimension size{0, 0};
	for (auto phase : {PassengerPrecomputedProgress::Phase::Services, PassengerPrecomputedProgress::Phase::Graph,
			PassengerPrecomputedProgress::Phase::Restoring, PassengerPrecomputedProgress::Phase::Tables,
			PassengerPrecomputedProgress::Phase::Complete, PassengerPrecomputedProgress::Phase::MemoryRejected,
			PassengerPrecomputedProgress::Phase::Invalid}) {
		size = maxdim(size, GetStringBoundingBox(ProgressPhaseString(phase)));
	}
	size = maxdim(size, GetStringBoundingBox(GetString(STR_PASSENGER_PRECOMPUTED_DESTINATIONS, GetParamMaxDigits(8), GetParamMaxDigits(8))));
	size.height = 3 * GetCharacterHeight(FontSize::Normal) + 2 * WidgetDimensions::scaled.vsep_normal + 2 * WidgetDimensions::scaled.bevel.top;
	return size;
}

void DrawPassengerProgress(const Rect &r, const PassengerPrecomputedProgress &progress)
{
	const int height = GetCharacterHeight(FontSize::Normal);
	DrawString(r.left, r.right, r.top, ProgressPhaseString(progress.phase), TextColour::FromString, AlignmentH::Centre);
	/* A restored saved frontier may intentionally be partial. Once the local
	 * preparation barrier is done, show completion without a misleading partial
	 * percentage or a still-moving indicator while waiting for the server. */
	if (!progress.preparing) return;
	const int top = r.top + height + WidgetDimensions::scaled.vsep_normal;
	const Rect bar{r.left, top, r.right, top + height + 2 * WidgetDimensions::scaled.bevel.top - 1};
	DrawFrameRect(bar, Colours::Grey, {FrameFlag::BorderOnly, FrameFlag::Lowered});
	const Rect inner = bar.Shrink(WidgetDimensions::scaled.bevel);
	if (const auto percent = progress.Percent()) {
		if (*percent != 0) DrawFrameRect(inner.WithWidth(inner.Width() * *percent / 100, _current_text_dir == TD_RTL), Colours::Mauve, {});
		DrawString(inner.left, inner.right, inner.top, GetString(STR_PASSENGER_PRECOMPUTED_PERCENT, *percent), TextColour::FromString, AlignmentH::Centre);
	} else {
		/* Indeterminate activity: deliberately no numeric percentage. */
		const int width = std::max(1, inner.Width() / 5);
		const int offset = (inner.Width() - width) * (_progress_indicator % 10) / 9;
		DrawFrameRect(inner.WithWidth(width, false).Translate(offset, 0), Colours::Mauve, {});
	}
	if (progress.phase == PassengerPrecomputedProgress::Phase::Restoring && progress.retained_total_known) {
		DrawString(r.left, r.right, bar.bottom + 1 + WidgetDimensions::scaled.vsep_normal,
				GetString(STR_PASSENGER_PRECOMPUTED_DESTINATIONS, progress.restored_destinations, progress.retained_stations), TextColour::FromString, AlignmentH::Centre);
	} else if (progress.total_known) {
		DrawString(r.left, r.right, bar.bottom + 1 + WidgetDimensions::scaled.vsep_normal,
				GetString(STR_PASSENGER_PRECOMPUTED_DESTINATIONS, progress.destinations, progress.stations), TextColour::FromString, AlignmentH::Centre);
	}
}

enum PassengerProgressWidgets : WidgetID { WID_PP_PROGRESS };

struct PassengerProgressWindow : Window {
	PassengerProgressWindow(WindowDesc &desc) : Window(desc) { this->InitNested(0); }

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget == WID_PP_PROGRESS && _progress) DrawPassengerProgress(r, *_progress);
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, const Dimension &, Dimension &, Dimension &) override
	{
		if (widget == WID_PP_PROGRESS) size = maxdim(size, GetPassengerProgressSize());
	}

	void Close(int data = 0) override
	{
		_initial_progress.Dismiss();
		this->Window::Close(data);
	}
};

static constexpr std::initializer_list<NWidgetPart> _passenger_progress_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Grey),
		NWidget(WWT_CAPTION, Colours::Grey), SetStringTip(STR_PASSENGER_PRECOMPUTED_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Grey),
		NWidget(WWT_EMPTY, Colours::Invalid, WID_PP_PROGRESS), SetMinimalSize(350, 0), SetFill(1, 0), SetPadding(WidgetDimensions::unscaled.modalpopup),
	EndContainer(),
};

static WindowDesc _passenger_progress_desc(__FILE__, __LINE__,
	WindowPosition::Center, nullptr, 0, 0,
	WindowClass::PassengerPrecomputedProgress, WindowClass::None, {}, _passenger_progress_widgets);

/** Programmatic closure does not count as a user dismissing the progress. */
static void ClosePassengerProgressWindow()
{
	if (Window *w = FindWindowById(WindowClass::PassengerPrecomputedProgress, 0)) w->Window::Close();
}

void ResetInitialPassengerProgress(bool arm)
{
	ClosePassengerProgressWindow();
	_initial_progress.Reset(arm);
	_progress.reset();
	_progress_update = std::chrono::steady_clock::now();
	_progress_indicator = 0;
}

const PassengerPrecomputedProgress *GetInitialPassengerProgress()
{
	return _progress ? &*_progress : nullptr;
}

void UpdateInitialPassengerProgress()
{
	if (_game_mode != GameMode::Normal) { ResetInitialPassengerProgress(false); return; }
	if (HasModalProgress()) return;
	if (!_initial_progress.Pending()) return;
	const auto progress = GetPassengerServiceDatabase().GetPrecomputedProgress();
	const bool was_active = _initial_progress.Active();
	_initial_progress.Observe(progress.preparing);
	if (!_initial_progress.Active()) {
		ClosePassengerProgressWindow();
		if (was_active && _progress) {
			/* Keep the local completion visible in an existing join window.
			 * Its server barrier, registration and disconnect rules stay intact. */
			_progress = progress;
			InvalidateWindowData(WindowClass::NetworkStatus, NetworkStatusWindowNumber::Join);
		}
		return;
	}
	const bool joining = FindWindowById(WindowClass::NetworkStatus, NetworkStatusWindowNumber::Join) != nullptr;
	if (joining) ClosePassengerProgressWindow();
	const auto now = std::chrono::steady_clock::now();
	if (now - _progress_update < std::chrono::milliseconds(200)) return;
	_progress_update = now;
	_progress = progress;
	++_progress_indicator;
	if (joining) {
		InvalidateWindowData(WindowClass::NetworkStatus, NetworkStatusWindowNumber::Join);
	} else if (!_network_dedicated && _initial_progress.ShowWindow()) {
		if (FindWindowById(WindowClass::PassengerPrecomputedProgress, 0) == nullptr) new PassengerProgressWindow(_passenger_progress_desc);
		SetWindowDirty(WindowClass::PassengerPrecomputedProgress, 0);
	}
}
