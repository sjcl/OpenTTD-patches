/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */

#ifndef PASSENGER_PRECOMPUTED_GUI_H
#define PASSENGER_PRECOMPUTED_GUI_H

#include "passenger_precomputed_progress.h"
#include "core/geometry_type.hpp"

void ResetInitialPassengerProgress(bool arm);
void UpdateInitialPassengerProgress();
const PassengerPrecomputedProgress *GetInitialPassengerProgress();
Dimension GetPassengerProgressSize();
void DrawPassengerProgress(const Rect &r, const PassengerPrecomputedProgress &progress);

#endif /* PASSENGER_PRECOMPUTED_GUI_H */
