/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file tree_cmd.cpp Handling of tree tiles. */

#include "stdafx.h"
#include "clear_map.h"
#include "landscape.h"
#include "tree_map.h"
#include "viewport_func.h"
#include "command_func.h"
#include "town.h"
#include "genworld.h"
#include "clear_func.h"
#include "company_func.h"
#include "sound_func.h"
#include "water.h"
#include "company_base.h"
#include "core/random_func.hpp"
#include "newgrf_generic.h"
#include "date_func.h"

#include "table/strings.h"
#include "table/tree_land.h"
#include "table/clear_land.h"

#include "safeguards.h"

/**
 * List of tree placer algorithm.
 *
 * This enumeration defines all possible tree placer algorithm in the game.
 */
enum TreePlacer {
	TP_NONE,     ///< No tree placer algorithm
	TP_ORIGINAL, ///< The original algorithm
	TP_IMPROVED, ///< A 'improved' algorithm
	TP_PERFECT,  ///< A 'best' algorithm
};

/** Where to place trees while in-game? */
enum ExtraTreePlacement {
	ETP_NO_SPREAD,           ///< Grow trees on tiles that have them but don't spread to new ones
	ETP_SPREAD_RAINFOREST,   ///< Grow trees on tiles that have them, only spread to new ones in rainforests
	ETP_SPREAD_ALL,          ///< Grow trees and spread them without restrictions
	ETP_NO_GROWTH_NO_SPREAD, ///< Don't grow trees and don't spread them at all
};

/** Determines when to consider building more trees. */
byte _trees_tick_ctr;

static const uint16 DEFAULT_TREE_STEPS = 1000;             ///< Default number of attempts for placing trees.
static const uint16 DEFAULT_RAINFOREST_TREE_STEPS = 15000; ///< Default number of attempts for placing extra trees at rainforest in tropic.
static const uint16 EDITOR_TREE_DIV = 5;                   ///< Game editor tree generation divisor factor.

/**
 * Tests if a tile can be converted to MP_TREES
 * This is true for clear ground without farms or rocks.
 *
 * @param tile the tile of interest
 * @param allow_desert Allow planting trees on CLEAR_DESERT?
 * @return true if trees can be built.
 */
static bool CanPlantTreesOnTile(TileIndex tile, bool allow_desert)
{
	if ((_settings_game.game_creation.tree_placer == TP_PERFECT) &&
		(_settings_game.game_creation.landscape == LT_ARCTIC) &&
		(GetTileZ(tile) > (HighestTreePlacementSnowLine() + _settings_game.construction.trees_around_snow_line_range))) {
		return false;
	}

	switch (GetTileType(tile)) {
		case MP_WATER:
			return !IsBridgeAbove(tile) && IsCoast(tile) && !IsSlopeWithOneCornerRaised(GetTileSlope(tile));

		case MP_CLEAR:
			return !IsBridgeAbove(tile) && !IsClearGround(tile, CLEAR_FIELDS) && GetRawClearGround(tile) != CLEAR_ROCKS &&
			       (allow_desert || !IsClearGround(tile, CLEAR_DESERT));

		default: return false;
	}
}

/**
 * Creates a tree tile
 * Ground type and density is preserved.
 *
 * @pre the tile must be suitable for trees.
 *
 * @param tile where to plant the trees.
 * @param treetype The type of the tree
 * @param count the number of trees (minus 1)
 * @param growth the growth status
 */
static void PlantTreesOnTile(TileIndex tile, TreeType treetype, uint count, uint growth)
{
	assert(treetype != TREE_INVALID);
	assert_tile(CanPlantTreesOnTile(tile, true), tile);

	TreeGround ground;
	uint density = 3;

	switch (GetTileType(tile)) {
		case MP_WATER:
			ground = TREE_GROUND_SHORE;
			ClearNeighbourNonFloodingStates(tile);
			break;

		case MP_CLEAR:
			switch (GetClearGround(tile)) {
				case CLEAR_GRASS:  ground = TREE_GROUND_GRASS;       break;
				case CLEAR_ROUGH:  ground = TREE_GROUND_ROUGH;       break;
				case CLEAR_SNOW:   ground = GetRawClearGround(tile) == CLEAR_ROUGH ? TREE_GROUND_ROUGH_SNOW : TREE_GROUND_SNOW_DESERT; break;
				default:           ground = TREE_GROUND_SNOW_DESERT; break;
			}
			if (GetClearGround(tile) != CLEAR_ROUGH) density = GetClearDensity(tile);
			break;

		default: NOT_REACHED();
	}

	MakeTree(tile, treetype, count, growth, ground, density);
}

/**
 * Previous value of _settings_game.construction.trees_around_snow_line_range
 * used to calculate _arctic_tree_occurance
 */
static uint8 _previous_trees_around_snow_line_range = 255;

/**
 * Array of probabilities for artic trees to appear,
 * by normalised distance from snow line
 */
static std::vector<uint8> _arctic_tree_occurance;

/** Recalculate _arctic_tree_occurance */
static void RecalculateArcticTreeOccuranceArray()
{
	/*
	 * Approximate: 256 * exp(-3 * distance / range)
	 * By using:
	 * 256 * ((1 + (-3 * distance / range) / 6) ** 6)
	 * ((256 - (128 * distance / range)) ** 6) >> (5 * 8);
	 */
	uint8 range = _settings_game.construction.trees_around_snow_line_range;
	_previous_trees_around_snow_line_range = range;
	_arctic_tree_occurance.clear();
	_arctic_tree_occurance.reserve((range * 5) / 4);
	_arctic_tree_occurance.push_back(255);
	if (range == 0) return;
	for (uint i = 1; i < 256; i++) {
		uint x = 256 - ((128 * i) / range);
		uint32 output = x;
		output *= x;
		output *= x;
		output *= x;
		output >>= 16;
		output *= x;
		output *= x;
		output >>= 24;
		if (output == 0) break;
		_arctic_tree_occurance.push_back(static_cast<uint8>(output));
	}
}

/**
 * Get a random TreeType for the given tile based on a given seed
 *
 * This function returns a random TreeType which can be placed on the given tile.
 * The seed for randomness must be less than 256, use #GB on the value of Random()
 * to get such a value.
 *
 * @param tile The tile to get a random TreeType from
 * @param seed The seed for randomness, must be less than 256
 * @return The random tree type
 */
static TreeType GetRandomTreeType(TileIndex tile, uint seed)
{
	switch (_settings_game.game_creation.landscape) {
		case LT_TEMPERATE:
			return (TreeType)(seed * TREE_COUNT_TEMPERATE / 256 + TREE_TEMPERATE);

		case LT_ARCTIC: {
			if (!_settings_game.construction.trees_around_snow_line_enabled) {
				return (TreeType)(seed * TREE_COUNT_SUB_ARCTIC / 256 + TREE_SUB_ARCTIC);
			}

			uint8 range = _settings_game.construction.trees_around_snow_line_range;
			if (range != _previous_trees_around_snow_line_range) RecalculateArcticTreeOccuranceArray();

			int z = GetTileZ(tile);
			int height_above_snow_line = 0;

			if (z > HighestTreePlacementSnowLine()) {
				height_above_snow_line = z - HighestTreePlacementSnowLine();
			} else if (z < LowestTreePlacementSnowLine()) {
				height_above_snow_line = z - LowestTreePlacementSnowLine();
			}
			uint normalised_distance = (height_above_snow_line < 0) ? -height_above_snow_line : height_above_snow_line + 1;
			bool arctic_tree = false;
			if (normalised_distance < _arctic_tree_occurance.size()) {
				arctic_tree = RandomRange(256) < _arctic_tree_occurance[normalised_distance];
			}
			if (height_above_snow_line < 0) {
				/* Below snow level mixed forest. */
				return (arctic_tree) ? (TreeType)(seed * TREE_COUNT_SUB_ARCTIC / 256 + TREE_SUB_ARCTIC) : (TreeType)(seed * TREE_COUNT_TEMPERATE / 256 + TREE_TEMPERATE);
			} else {
				/* Above is Arctic trees and thinning out. */
				return (arctic_tree) ? (TreeType)(seed * TREE_COUNT_SUB_ARCTIC / 256 + TREE_SUB_ARCTIC) : TREE_INVALID;
			}
		}
		case LT_TROPIC:
			switch (GetTropicZone(tile)) {
				case TROPICZONE_NORMAL:  return (TreeType)(seed * TREE_COUNT_SUB_TROPICAL / 256 + TREE_SUB_TROPICAL);
				case TROPICZONE_DESERT:  return (TreeType)((seed > 12) ? TREE_INVALID : TREE_CACTUS);
				default:                 return (TreeType)(seed * TREE_COUNT_RAINFOREST / 256 + TREE_RAINFOREST);
			}

		default:
			return (TreeType)(seed * TREE_COUNT_TOYLAND / 256 + TREE_TOYLAND);
	}
}

/**
 * Make a random tree tile of the given tile
 *
 * Create a new tree-tile for the given tile. The second parameter is used for
 * randomness like type and number of trees.
 *
 * @param tile The tile to make a tree-tile from
 * @param r The randomness value from a Random() value
 */
static void PlaceTree(TileIndex tile, uint32 r)
{
	TreeType tree = GetRandomTreeType(tile, GB(r, 24, 8));

	if (tree != TREE_INVALID) {
		PlantTreesOnTile(tile, tree, GB(r, 22, 2), std::min<byte>(GB(r, 16, 3), 6));
		MarkTileDirtyByTile(tile);

		/* Rerandomize ground, if neither snow nor shore */
		TreeGround ground = GetTreeGround(tile);
		if (ground != TREE_GROUND_SNOW_DESERT && ground != TREE_GROUND_ROUGH_SNOW && ground != TREE_GROUND_SHORE) {
			SetTreeGroundDensity(tile, (TreeGround)GB(r, 28, 1), 3);
		}
	}
}

/**
 * Creates a number of tree groups.
 * The number of trees in each group depends on how many trees are actually placed around the given tile.
 *
 * @param num_groups Number of tree groups to place.
 */
static void PlaceTreeGroups(uint num_groups)
{
	do {
		TileIndex center_tile = RandomTile();

		for (uint i = 0; i < DEFAULT_TREE_STEPS; i++) {
			uint32 r = Random();
			int x = GB(r, 0, 5) - 16;
			int y = GB(r, 8, 5) - 16;
			uint dist = abs(x) + abs(y);
			TileIndex cur_tile = TileAddWrap(center_tile, x, y);

			IncreaseGeneratingWorldProgress(GWP_TREE);

			if (cur_tile != INVALID_TILE && dist <= 13 && CanPlantTreesOnTile(cur_tile, true)) {
				PlaceTree(cur_tile, r);
			}
		}

	} while (--num_groups);
}

static TileIndex FindTreePositionAtSameHeight(TileIndex tile, int height, uint steps)
{
	for (uint i = 0; i < steps; i++) {
		const uint32 r = Random();
		const int x = GB(r, 0, 5) - 16;
		const int y = GB(r, 8, 5) - 16;
		const TileIndex cur_tile = TileAddWrap(tile, x, y);

		if (cur_tile == INVALID_TILE) continue;

		/* Keep in range of the existing tree */
		if (abs(x) + abs(y) > 16) continue;

		/* Clear tile, no farm-tiles or rocks */
		if (!CanPlantTreesOnTile(cur_tile, true)) continue;

		/* Not too much height difference */
		if (Delta(GetTileZ(cur_tile), height) > 2) continue;

		/* We found a position */
		return cur_tile;
	}

	return INVALID_TILE;
}

/**
 * Plants a tree at the same height as an existing tree.
 *
 * Plant a tree around the given tile which is at the same
 * height or at some offset (2 units) of it.
 *
 * @param tile The base tile to add a new tree somewhere around
 */
static void PlantTreeAtSameHeight(TileIndex tile)
{
	const auto new_tile = FindTreePositionAtSameHeight(tile, GetTileZ(tile), 1);

	if (new_tile != INVALID_TILE) {
		PlantTreesOnTile(new_tile, GetTreeType(tile), 0, 0);
	}
}

/**
 * Place a tree at the same height as an existing tree.
 *
 * Add a new tree around the given tile which is at the same
 * height or at some offset (2 units) of it.
 *
 * @param tile The base tile to add a new tree somewhere around
 * @param height The height (from GetTileZ)
 */
static void PlaceTreeAtSameHeight(TileIndex tile, int height)
{
	const auto new_tile = FindTreePositionAtSameHeight(tile, height, DEFAULT_TREE_STEPS);

	if (new_tile != INVALID_TILE) {
		PlaceTree(new_tile, Random());
	}
}

int GetSparseTreeRange()
{
	const auto max_map_height = std::max<int>(32, _settings_game.construction.map_height_limit);
	const auto sparse_tree_range = std::min(8, (4 * max_map_height) / 32);

	return sparse_tree_range;
}

int MaxTreeCount(const TileIndex tile)
{
	const auto tile_z = GetTileZ(tile);
	const auto round_up_divide = [](const uint x, const uint y) { return (x / y) + ((x % y != 0) ? 1 : 0); };

	int max_trees_z_based = round_up_divide(tile_z * 4, GetSparseTreeRange());
	max_trees_z_based = std::max(1, max_trees_z_based);
	max_trees_z_based += (_settings_game.game_creation.landscape != LT_TROPIC ? 0 : 1);

	int max_trees_snow_line_based = 4;

	if (_settings_game.game_creation.landscape == LT_ARCTIC) {
		if (_settings_game.construction.trees_around_snow_line_range != _previous_trees_around_snow_line_range) RecalculateArcticTreeOccuranceArray();
		const uint height_above_snow_line = std::max<int>(0, tile_z - HighestTreePlacementSnowLine());
		max_trees_snow_line_based = (height_above_snow_line < _arctic_tree_occurance.size()) ?
			(1 + (_arctic_tree_occurance[height_above_snow_line] * 4) / 255) :
			0;
	}

	return std::min(max_trees_z_based, max_trees_snow_line_based);
}

/**
 * Place some trees randomly
 *
 * This function just place some trees randomly on the map.
 */
void PlaceTreesRandomly()
{
	int i, j, ht;

	i = ScaleByMapSize(DEFAULT_TREE_STEPS);
	if (_game_mode == GM_EDITOR) i /= EDITOR_TREE_DIV;
	do {
		uint32 r = Random();
		TileIndex tile = RandomTileSeed(r);

		IncreaseGeneratingWorldProgress(GWP_TREE);

		if (CanPlantTreesOnTile(tile, true)) {
			PlaceTree(tile, r);
			if (_settings_game.game_creation.tree_placer != TP_IMPROVED &&
				_settings_game.game_creation.tree_placer != TP_PERFECT) continue;

			/* Place a number of trees based on the tile height.
			 *  This gives a cool effect of multiple trees close together.
			 *  It is almost real life ;) */
			ht = GetTileZ(tile);
			/* The higher we get, the more trees we plant */
			j = ht * 2;
			/* Above snowline more trees! */
			if (_settings_game.game_creation.landscape == LT_ARCTIC && ht > GetSnowLine()) j *= 3;
			while (j--) {
				PlaceTreeAtSameHeight(tile, ht);
			}
		}
	} while (--i);

	/* place extra trees at rainforest area */
	if (_settings_game.game_creation.landscape == LT_TROPIC) {
		i = ScaleByMapSize(DEFAULT_RAINFOREST_TREE_STEPS);
		if (_game_mode == GM_EDITOR) i /= EDITOR_TREE_DIV;

		do {
			uint32 r = Random();
			TileIndex tile = RandomTileSeed(r);

			IncreaseGeneratingWorldProgress(GWP_TREE);

			if (GetTropicZone(tile) == TROPICZONE_RAINFOREST && CanPlantTreesOnTile(tile, false)) {
				PlaceTree(tile, r);
			}
		} while (--i);
	}
}

/**
 * Remove all trees
 *
 * This function remove all trees on the map.
 */
void RemoveAllTrees()
{
	if (_game_mode != GM_EDITOR) return;

	for(uint i = 0; i < MapSizeX(); i++) {
		for(uint j = 0; j < MapSizeY(); j++) {
			TileIndex tile = TileXY(i, j);
			if(GetTileType(tile) == MP_TREES) {
				DoCommandP(tile, 0, 0, CMD_LANDSCAPE_CLEAR | CMD_MSG(STR_ERROR_CAN_T_CLEAR_THIS_AREA), CcPlaySound_EXPLOSION);
			}
		}
	}
}

/**
 * Place some trees in a radius around a tile.
 * The trees are placed in an quasi-normal distribution around the indicated tile, meaning that while
 * the radius does define a square, the distribution inside the square will be roughly circular.
 * @note This function the interactive RNG and must only be used in editor and map generation.
 * @param tile      Tile to place trees around.
 * @param treetype  Type of trees to place. Must be a valid tree type for the climate.
 * @param radius    Maximum distance (on each axis) from tile to place trees.
 * @param count     Maximum number of trees to place.
 * @param set_zone  Whether to create a rainforest zone when placing rainforest trees.
 * @return Number of trees actually placed.
 */
uint PlaceTreeGroupAroundTile(TileIndex tile, TreeType treetype, uint radius, uint count, bool set_zone)
{
	assert(treetype < TREE_TOYLAND + TREE_COUNT_TOYLAND);
	const bool allow_desert = treetype == TREE_CACTUS;
	uint planted = 0;

	for (; count > 0; count--) {
		/* Simple quasi-normal distribution with range [-radius; radius) */
		auto mkcoord = [&]() -> int32 {
			const uint32 rand = InteractiveRandom();
			const int32 dist = GB<int32>(rand, 0, 8) + GB<int32>(rand, 8, 8) + GB<int32>(rand, 16, 8) + GB<int32>(rand, 24, 8);
			const int32 scu = dist * radius / 512;
			return scu - radius;
		};
		const int32 xofs = mkcoord();
		const int32 yofs = mkcoord();
		const TileIndex tile_to_plant = TileAddWrap(tile, xofs, yofs);
		if (tile_to_plant != INVALID_TILE) {
			if (IsTileType(tile_to_plant, MP_TREES) && GetTreeCount(tile_to_plant) < 4) {
				AddTreeCount(tile_to_plant, 1);
				SetTreeGrowth(tile_to_plant, 0);
				MarkTileDirtyByTile(tile_to_plant, VMDF_NOT_MAP_MODE_NON_VEG);
				planted++;
			} else if (CanPlantTreesOnTile(tile_to_plant, allow_desert)) {
				PlantTreesOnTile(tile_to_plant, treetype, 0, 3);
				MarkTileDirtyByTile(tile_to_plant, VMDF_NOT_MAP_MODE_NON_VEG);
				planted++;
			}
		}
	}

	if (set_zone && IsInsideMM(treetype, TREE_RAINFOREST, TREE_CACTUS)) {
		for (TileIndex t : TileArea(tile).Expand(radius)) {
			if (GetTileType(t) != MP_VOID && DistanceSquare(tile, t) < radius * radius) SetTropicZone(t, TROPICZONE_RAINFOREST);
		}
	}

	return planted;
}

/**
 * Place new trees.
 *
 * This function takes care of the selected tree placer algorithm and
 * place randomly the trees for a new game.
 */
void GenerateTrees()
{
	uint i, total;

	if (_settings_game.game_creation.tree_placer == TP_NONE) return;

	switch (_settings_game.game_creation.tree_placer) {
		case TP_ORIGINAL: i = _settings_game.game_creation.landscape == LT_ARCTIC ? 15 : 6; break;
		case TP_IMPROVED:
		case TP_PERFECT: i = _settings_game.game_creation.landscape == LT_ARCTIC ?  4 : 2; break;
		default: NOT_REACHED();
	}

	total = ScaleByMapSize(DEFAULT_TREE_STEPS);
	if (_settings_game.game_creation.landscape == LT_TROPIC) total += ScaleByMapSize(DEFAULT_RAINFOREST_TREE_STEPS);
	total *= i;
	uint num_groups = (_settings_game.game_creation.landscape != LT_TOYLAND) ? ScaleByMapSize(GB(Random(), 0, 5) + 25) : 0;

	if (_settings_game.game_creation.tree_placer != TP_PERFECT) {
		total += num_groups * DEFAULT_TREE_STEPS;
	}

	SetGeneratingWorldProgress(GWP_TREE, total);

	if (_settings_game.game_creation.tree_placer != TP_PERFECT) {
		if (num_groups != 0) PlaceTreeGroups(num_groups);
	}

	for (; i != 0; i--) {
		PlaceTreesRandomly();
	}
}

/**
 * Plant a tree.
 * @param tile end tile of area-drag
 * @param flags type of operation
 * @param p1 tree type, TREE_INVALID means random.
 * @param p2 start tile of area-drag of tree plantation
 * @param text unused
 * @return the cost of this operation or an error
 */
CommandCost CmdPlantTree(TileIndex tile, DoCommandFlag flags, uint32 p1, uint32 p2, const char *text)
{
	StringID msg = INVALID_STRING_ID;
	CommandCost cost(EXPENSES_OTHER);
	const byte tree_to_plant = GB(p1, 0, 8); // We cannot use Extract as min and max are climate specific.

	if (p2 >= MapSize()) return CMD_ERROR;
	/* Check the tree type within the current climate */
	if (tree_to_plant != TREE_INVALID && !IsInsideBS(tree_to_plant, _tree_base_by_landscape[_settings_game.game_creation.landscape], _tree_count_by_landscape[_settings_game.game_creation.landscape])) return CMD_ERROR;

	Company *c = (_game_mode != GM_EDITOR) ? Company::GetIfValid(_current_company) : nullptr;
	int limit = (c == nullptr ? INT32_MAX : GB(c->tree_limit, 16, 16));

	TileArea ta(tile, p2);
	for (TileIndex tile : ta) {
		switch (GetTileType(tile)) {
			case MP_TREES: {
				bool grow_existing_tree_instead = false;

				/* no more space for trees? */
				if (_settings_game.game_creation.tree_placer == TP_PERFECT) {
					if (GetTreeCount(tile) >= 4 || ((GetTreeType(tile) != TREE_CACTUS) && ((int)GetTreeCount(tile) >= MaxTreeCount(tile)))) {
						if (GetTreeGrowth(tile) < 3) {
							grow_existing_tree_instead = true;
						} else {
							msg = STR_ERROR_TREE_ALREADY_HERE;
							continue;
						}
					}
				} else {
					if (GetTreeCount(tile) == 4) {
						msg = STR_ERROR_TREE_ALREADY_HERE;
						continue;
					}
				}

				/* Test tree limit. */
				if (--limit < 1) {
					msg = STR_ERROR_TREE_PLANT_LIMIT_REACHED;
					break;
				}

				if (flags & DC_EXEC) {
					if (grow_existing_tree_instead) {
						SetTreeGrowth(tile, 3);
					} else {
						AddTreeCount(tile, 1);
					}
					MarkTileDirtyByTile(tile, VMDF_NOT_MAP_MODE_NON_VEG);
					if (c != nullptr) c->tree_limit -= 1 << 16;
				}
				/* 2x as expensive to add more trees to an existing tile */
				cost.AddCost(_price[PR_BUILD_TREES] * 2);
				break;
			}

			case MP_WATER:
				if (!CanPlantTreesOnTile(tile, false) || !IsCoast(tile) || IsSlopeWithOneCornerRaised(GetTileSlope(tile))) {
					msg = STR_ERROR_CAN_T_BUILD_ON_WATER;
					continue;
				}
				FALLTHROUGH;

			case MP_CLEAR: {
				if (!CanPlantTreesOnTile(tile, false) || IsBridgeAbove(tile)) {
					msg = STR_ERROR_SITE_UNSUITABLE;
					continue;
				}

				TreeType treetype = (TreeType)tree_to_plant;
				/* Be a bit picky about which trees go where. */
				if (_settings_game.game_creation.landscape == LT_TROPIC && treetype != TREE_INVALID && (
						/* No cacti outside the desert */
						(treetype == TREE_CACTUS && GetTropicZone(tile) != TROPICZONE_DESERT) ||
						/* No rain forest trees outside the rain forest, except in the editor mode where it makes those tiles rain forest tile */
						(IsInsideMM(treetype, TREE_RAINFOREST, TREE_CACTUS) && GetTropicZone(tile) != TROPICZONE_RAINFOREST && _game_mode != GM_EDITOR) ||
						/* And no subtropical trees in the desert/rain forest */
						(IsInsideMM(treetype, TREE_SUB_TROPICAL, TREE_TOYLAND) && GetTropicZone(tile) != TROPICZONE_NORMAL))) {
					msg = STR_ERROR_TREE_WRONG_TERRAIN_FOR_TREE_TYPE;
					continue;
				}

				/* Test tree limit. */
				if (--limit < 1) {
					msg = STR_ERROR_TREE_PLANT_LIMIT_REACHED;
					break;
				}

				if (IsTileType(tile, MP_CLEAR)) {
					/* Remove fields or rocks. Note that the ground will get barrened */
					switch (GetRawClearGround(tile)) {
						case CLEAR_FIELDS:
						case CLEAR_ROCKS: {
							CommandCost ret = DoCommand(tile, 0, 0, flags, CMD_LANDSCAPE_CLEAR);
							if (ret.Failed()) return ret;
							cost.AddCost(ret);
							break;
						}

						default: break;
					}
				}

				if (_game_mode != GM_EDITOR && Company::IsValidID(_current_company)) {
					Town *t = ClosestTownFromTile(tile, _settings_game.economy.dist_local_authority);
					if (t != nullptr) ChangeTownRating(t, RATING_TREE_UP_STEP, RATING_TREE_MAXIMUM, flags);
				}

				if (flags & DC_EXEC) {
					if (treetype == TREE_INVALID) {
						treetype = GetRandomTreeType(tile, GB(Random(), 24, 8));
						if (treetype == TREE_INVALID) {
							if (_settings_game.construction.trees_around_snow_line_enabled && _settings_game.game_creation.landscape == LT_ARCTIC) {
								if (GetTileZ(tile) <= (int)_settings_game.game_creation.snow_line_height) {
									treetype = (TreeType)(GB(Random(), 24, 8) * TREE_COUNT_TEMPERATE / 256 + TREE_TEMPERATE);
								} else {
									treetype = (TreeType)(GB(Random(), 24, 8) * TREE_COUNT_SUB_ARCTIC / 256 + TREE_SUB_ARCTIC);
								}
							} else {
								treetype = TREE_CACTUS;
							}
						}
					}

					/* Plant full grown trees in scenario editor */
					PlantTreesOnTile(tile, treetype, 0, _game_mode == GM_EDITOR ? 3 : 0);
					MarkTileDirtyByTile(tile, VMDF_NOT_MAP_MODE_NON_VEG);
					if (c != nullptr) c->tree_limit -= 1 << 16;

					/* When planting rainforest-trees, set tropiczone to rainforest in editor. */
					if (_game_mode == GM_EDITOR && IsInsideMM(treetype, TREE_RAINFOREST, TREE_CACTUS)) {
						SetTropicZone(tile, TROPICZONE_RAINFOREST);
					}
				}
				cost.AddCost(_price[PR_BUILD_TREES]);
				break;
			}

			default:
				msg = STR_ERROR_SITE_UNSUITABLE;
				break;
		}

		/* Tree limit used up? No need to check more. */
		if (limit < 0) break;
	}

	if (cost.GetCost() == 0) {
		return_cmd_error(msg);
	} else {
		return cost;
	}
}

struct TreeListEnt : PalSpriteID {
	byte x, y;
};

static void DrawTile_Trees(TileInfo *ti, DrawTileProcParams params)
{
	if (!params.no_ground_tiles) {
		switch (GetTreeGround(ti->tile)) {
			case TREE_GROUND_SHORE: DrawShoreTile(ti->tileh); break;
			case TREE_GROUND_GRASS: DrawClearLandTile(ti, GetTreeDensity(ti->tile)); break;
			case TREE_GROUND_ROUGH: DrawHillyLandTile(ti); break;
			default: DrawGroundSprite(_clear_land_sprites_snow_desert[GetTreeDensity(ti->tile)] + SlopeToSpriteOffset(ti->tileh), PAL_NONE); break;
		}
	}

	/* Do not draw trees when the invisible trees setting is set */
	if (IsInvisibilitySet(TO_TREES)) return;

	uint tmp = CountBits(ti->tile + ti->x + ti->y);
	uint index = GB(tmp, 0, 2) + (GetTreeType(ti->tile) << 2);

	/* different tree styles above one of the grounds */
	if ((GetTreeGround(ti->tile) == TREE_GROUND_SNOW_DESERT || GetTreeGround(ti->tile) == TREE_GROUND_ROUGH_SNOW) &&
			GetTreeDensity(ti->tile) >= 2 &&
			IsInsideMM(index, TREE_SUB_ARCTIC << 2, TREE_RAINFOREST << 2)) {
		index += 164 - (TREE_SUB_ARCTIC << 2);
	}

	assert(index < lengthof(_tree_layout_sprite));

	const PalSpriteID *s = _tree_layout_sprite[index];
	const TreePos *d = _tree_layout_xy[GB(tmp, 2, 2)];

	/* combine trees into one sprite object */
	StartSpriteCombine();

	TreeListEnt te[4];

	/* put the trees to draw in a list */
	uint trees = GetTreeCount(ti->tile);

	PaletteID palette_adjust = 0;
	if (_settings_client.gui.shade_trees_on_slopes && ti->tileh != SLOPE_FLAT) {
		extern int GetSlopeTreeBrightnessAdjust(Slope slope);
		int adjust = GetSlopeTreeBrightnessAdjust(ti->tileh);
		if (adjust != 0) {
			SetBit(palette_adjust, PALETTE_BRIGHTNESS_MODIFY);
			SB(palette_adjust, PALETTE_BRIGHTNESS_OFFSET, PALETTE_BRIGHTNESS_WIDTH, adjust & ((1 << PALETTE_BRIGHTNESS_WIDTH) - 1));
		}
	}

	for (uint i = 0; i < trees; i++) {
		SpriteID sprite = s[0].sprite + (i == trees - 1 ? GetTreeGrowth(ti->tile) : 3);
		PaletteID pal = s[0].pal | palette_adjust;

		te[i].sprite = sprite;
		te[i].pal    = pal;
		te[i].x = d->x;
		te[i].y = d->y;
		s++;
		d++;
	}

	/* draw them in a sorted way */
	int z = ti->z + GetSlopeMaxPixelZ(ti->tileh) / 2;

	for (; trees > 0; trees--) {
		uint min = te[0].x + te[0].y;
		uint mi = 0;

		for (uint i = 1; i < trees; i++) {
			if ((uint)(te[i].x + te[i].y) < min) {
				min = te[i].x + te[i].y;
				mi = i;
			}
		}

		AddSortableSpriteToDraw(te[mi].sprite, te[mi].pal, ti->x + te[mi].x, ti->y + te[mi].y, 16 - te[mi].x, 16 - te[mi].y, 0x30, z, IsTransparencySet(TO_TREES), -te[mi].x, -te[mi].y);

		/* replace the removed one with the last one */
		te[mi] = te[trees - 1];
	}

	EndSpriteCombine();
}


static int GetSlopePixelZ_Trees(TileIndex tile, uint x, uint y)
{
	int z;
	Slope tileh = GetTilePixelSlope(tile, &z);

	return z + GetPartialPixelZ(x & 0xF, y & 0xF, tileh);
}

static Foundation GetFoundation_Trees(TileIndex tile, Slope tileh)
{
	return FOUNDATION_NONE;
}

static CommandCost ClearTile_Trees(TileIndex tile, DoCommandFlag flags)
{
	uint num;

	if (Company::IsValidID(_current_company)) {
		Town *t = ClosestTownFromTile(tile, _settings_game.economy.dist_local_authority);
		if (t != nullptr) ChangeTownRating(t, RATING_TREE_DOWN_STEP, RATING_TREE_MINIMUM, flags);
	}

	num = GetTreeCount(tile);
	if (IsInsideMM(GetTreeType(tile), TREE_RAINFOREST, TREE_CACTUS)) num *= 4;

	if (flags & DC_EXEC) DoClearSquare(tile);

	return CommandCost(EXPENSES_CONSTRUCTION, num * _price[PR_CLEAR_TREES]);
}

static void GetTileDesc_Trees(TileIndex tile, TileDesc *td)
{
	TreeType tt = GetTreeType(tile);

	if (IsInsideMM(tt, TREE_RAINFOREST, TREE_CACTUS)) {
		td->str = STR_LAI_TREE_NAME_RAINFOREST;
	} else {
		td->str = tt == TREE_CACTUS ? STR_LAI_TREE_NAME_CACTUS_PLANTS : STR_LAI_TREE_NAME_TREES;
	}

	td->owner[0] = GetTileOwner(tile);
}

static void TileLoopTreesDesert(TileIndex tile)
{
	switch (GetTropicZone(tile)) {
		case TROPICZONE_DESERT:
			if (GetTreeGround(tile) != TREE_GROUND_SNOW_DESERT) {
				SetTreeGroundDensity(tile, TREE_GROUND_SNOW_DESERT, 3);
				MarkTileDirtyByTile(tile, VMDF_NOT_MAP_MODE_NON_VEG);
			}
			break;

		case TROPICZONE_RAINFOREST: {
			static const SoundFx forest_sounds[] = {
				SND_42_RAINFOREST_1,
				SND_43_RAINFOREST_2,
				SND_44_RAINFOREST_3,
				SND_48_RAINFOREST_4
			};
			uint32 r = Random();

			if (Chance16I(1, 200, r) && _settings_client.sound.ambient) SndPlayTileFx(forest_sounds[GB(r, 16, 2)], tile);
			break;
		}

		default: break;
	}
}

static void TileLoopTreesAlps(TileIndex tile)
{
	int k;
	if ((int)TileHeight(tile) < GetSnowLine() - 1) {
		/* Fast path to avoid needing to check all 4 corners */
		k = -1;
	} else {
		k = GetTileZ(tile) - GetSnowLine() + 1;
	}

	if (k < 0) {
		switch (GetTreeGround(tile)) {
			case TREE_GROUND_SNOW_DESERT: SetTreeGroundDensity(tile, TREE_GROUND_GRASS, 3); break;
			case TREE_GROUND_ROUGH_SNOW:  SetTreeGroundDensity(tile, TREE_GROUND_ROUGH, 3); break;
			default: return;
		}
	} else {
		uint density = std::min<uint>(k, 3);

		if (GetTreeGround(tile) != TREE_GROUND_SNOW_DESERT && GetTreeGround(tile) != TREE_GROUND_ROUGH_SNOW) {
			TreeGround tg = GetTreeGround(tile) == TREE_GROUND_ROUGH ? TREE_GROUND_ROUGH_SNOW : TREE_GROUND_SNOW_DESERT;
			SetTreeGroundDensity(tile, tg, density);
		} else if (GetTreeDensity(tile) != density) {
			SetTreeGroundDensity(tile, GetTreeGround(tile), density);
		} else {
			if (GetTreeDensity(tile) == 3) {
				uint32 r = Random();
				if (Chance16I(1, 200, r) && _settings_client.sound.ambient) {
					SndPlayTileFx((r & 0x80000000) ? SND_39_ARCTIC_SNOW_2 : SND_34_ARCTIC_SNOW_1, tile);
				}
			}
			return;
		}
	}
	MarkTileDirtyByTile(tile, VMDF_NOT_MAP_MODE_NON_VEG);
}

static bool CanPlantExtraTrees(TileIndex tile)
{
	return ((_settings_game.game_creation.landscape == LT_TROPIC && GetTropicZone(tile) == TROPICZONE_RAINFOREST) ?
		(_settings_game.construction.extra_tree_placement == ETP_SPREAD_ALL || _settings_game.construction.extra_tree_placement == ETP_SPREAD_RAINFOREST) :
		_settings_game.construction.extra_tree_placement == ETP_SPREAD_ALL);
}

static void TileLoop_Trees(TileIndex tile)
{
	if (GetTreeGround(tile) == TREE_GROUND_SHORE) {
		TileLoop_Water(tile);
	} else {
		switch (_settings_game.game_creation.landscape) {
			case LT_TROPIC: TileLoopTreesDesert(tile); break;
			case LT_ARCTIC: TileLoopTreesAlps(tile);   break;
		}
	}

	AmbientSoundEffect(tile);

	/* _tick_counter is incremented by 256 between each call, so ignore lower 8 bits.
	 * Also, we add tile % 31 to spread the updates evenly over the map,
	 * where 31 is just some prime number that looks ok. */
	uint32 cycle = (uint32)((tile % 31) + (_tick_counter >> 8));

	/* Handle growth of grass (under trees/on MP_TREES tiles) at every 8th processings, like it's done for grass on MP_CLEAR tiles. */
	if ((cycle & 7) == 7 && GetTreeGround(tile) == TREE_GROUND_GRASS) {
		uint density = GetTreeDensity(tile);
		if (density < 3) {
			SetTreeGroundDensity(tile, TREE_GROUND_GRASS, density + 1);
			MarkTileDirtyByTile(tile, VMDF_NOT_MAP_MODE_NON_VEG);
		}
	}

	if ((cycle & 15) < 15) return;

	if (_settings_game.construction.extra_tree_placement == ETP_NO_GROWTH_NO_SPREAD) return;

	if (_settings_game.construction.tree_growth_rate > 0) {
		if (_settings_game.construction.tree_growth_rate == 4) return;

		/* slow, very slow, extremely slow */
		uint16 grow_slowing_values[4] = { 0x10000 / 5, 0x10000 / 20, 0x10000 / 120, 0 };

		if (GB(Random(), 0, 16) >= grow_slowing_values[_settings_game.construction.tree_growth_rate - 1]) {
			return;
		}
	}

	switch (GetTreeGrowth(tile)) {
		case 3: // regular sized tree
			if (_settings_game.game_creation.landscape == LT_TROPIC &&
					GetTreeType(tile) != TREE_CACTUS &&
					GetTropicZone(tile) == TROPICZONE_DESERT) {
				AddTreeGrowth(tile, 1);
			} else {
				switch (GB(Random(), 0, 3)) {
					case 0: // start destructing
						AddTreeGrowth(tile, 1);
						break;

					case 1: { // add a tree
						if (_settings_game.game_creation.tree_placer == TP_PERFECT) {
							if ((GetTreeCount(tile) < 4) && ((GetTreeType(tile) == TREE_CACTUS) || ((int)GetTreeCount(tile) < MaxTreeCount(tile)))) {
								AddTreeCount(tile, 1);
								SetTreeGrowth(tile, 0);
								break;
							}
						} else if (GetTreeCount(tile) < 4 && CanPlantExtraTrees(tile)) {
							AddTreeCount(tile, 1);
							SetTreeGrowth(tile, 0);
							break;
						}
					}
					FALLTHROUGH;

					case 2: { // add a neighbouring tree
						if (!CanPlantExtraTrees(tile)) break;

						if (_settings_game.game_creation.tree_placer == TP_PERFECT &&
							((_settings_game.game_creation.landscape != LT_TROPIC && GetTileZ(tile) <= GetSparseTreeRange()) ||
								(GetTreeType(tile) == TREE_CACTUS) ||
								(_settings_game.game_creation.landscape == LT_ARCTIC && GetTileZ(tile) >= HighestTreePlacementSnowLine() + _settings_game.construction.trees_around_snow_line_range / 3))) {
							// On lower levels we spread more randomly to not bunch up.
							if (GetTreeType(tile) != TREE_CACTUS || (RandomRange(100) < 50)) {
								PlantTreeAtSameHeight(tile);
							}
						} else {
							const TreeType tree_type = GetTreeType(tile);

							tile += TileOffsByDir((Direction)(Random() & 7));

							if (!CanPlantTreesOnTile(tile, false)) return;

							// Don't plant trees, if ground was freshly cleared
							if (IsTileType(tile, MP_CLEAR) && GetClearGround(tile) == CLEAR_GRASS && GetClearDensity(tile) != 3) return;

							PlantTreesOnTile(tile, tree_type, 0, 0);
						}
						break;
					}

					default:
						return;
				}
			}
			break;

		case 6: // final stage of tree destruction
			if (!CanPlantExtraTrees(tile)) {
				/* if trees can't spread just plant a new one to prevent deforestation */
				SetTreeGrowth(tile, 0);
			} else if (GetTreeCount(tile) > 1) {
				/* more than one tree, delete it */
				AddTreeCount(tile, -1);
				SetTreeGrowth(tile, 3);
			} else {
				/* just one tree, change type into MP_CLEAR */
				switch (GetTreeGround(tile)) {
					case TREE_GROUND_SHORE: MakeShore(tile); break;
					case TREE_GROUND_GRASS: MakeClear(tile, CLEAR_GRASS, GetTreeDensity(tile)); break;
					case TREE_GROUND_ROUGH: MakeClear(tile, CLEAR_ROUGH, 3); break;
					case TREE_GROUND_ROUGH_SNOW: {
						uint density = GetTreeDensity(tile);
						MakeClear(tile, CLEAR_ROUGH, 3);
						MakeSnow(tile, density);
						break;
					}
					default: // snow or desert
						if (_settings_game.game_creation.landscape == LT_TROPIC) {
							MakeClear(tile, CLEAR_DESERT, GetTreeDensity(tile));
						} else {
							uint density = GetTreeDensity(tile);
							MakeClear(tile, CLEAR_GRASS, 3);
							MakeSnow(tile, density);
						}
						break;
				}
			}
			break;

		default:
			AddTreeGrowth(tile, 1);
			break;
	}

	MarkTileDirtyByTile(tile, VMDF_NOT_MAP_MODE_NON_VEG);
}

/**
 * Decrement the tree tick counter.
 * The interval is scaled by map size to allow for the same density regardless of size.
 * Adjustment for map sizes below the standard 256 * 256 are handled earlier.
 * @return number of trees to try to plant
 */
uint DecrementTreeCounter()
{
	uint scaled_map_size = ScaleByMapSize(1);
	if (scaled_map_size >= 256) return scaled_map_size >> 8;

	/* byte underflow */
	byte old_trees_tick_ctr = _trees_tick_ctr;
	_trees_tick_ctr -= scaled_map_size;
	return old_trees_tick_ctr <= _trees_tick_ctr ? 1 : 0;
}

void OnTick_Trees()
{
	/* Don't spread trees if that's not allowed */
	if (_settings_game.construction.extra_tree_placement == ETP_NO_SPREAD || _settings_game.construction.extra_tree_placement == ETP_NO_GROWTH_NO_SPREAD) return;

	uint32 r;
	TileIndex tile;
	TreeType tree;

	/* Skip some tree ticks for map sizes below 256 * 256. 64 * 64 is 16 times smaller, so
	 * this is the maximum number of ticks that are skipped. Number of ticks to skip is
	 * inversely proportional to map size, so that is handled to create a mask. */
	int skip = ScaleByMapSize(16);
	if (skip < 16 && (_tick_counter & (16 / skip - 1)) != 0) return;

	/* place a tree at a random rainforest spot */
	if (_settings_game.game_creation.landscape == LT_TROPIC) {
		for (uint c = ScaleByMapSize(1); c > 0; c--) {
			if ((r = Random(), tile = RandomTileSeed(r), GetTropicZone(tile) == TROPICZONE_RAINFOREST) &&
				CanPlantTreesOnTile(tile, false) &&
				(tree = GetRandomTreeType(tile, GB(r, 24, 8))) != TREE_INVALID) {
				PlantTreesOnTile(tile, tree, 0, 0);
			}
		}
	}

	if (_settings_game.construction.extra_tree_placement == ETP_SPREAD_RAINFOREST) return;

	for (uint ctr = DecrementTreeCounter(); ctr > 0; ctr--) {
		/* place a tree at a random spot */
		r = Random();
		tile = RandomTileSeed(r);
		if (CanPlantTreesOnTile(tile, false) && (tree = GetRandomTreeType(tile, GB(r, 24, 8))) != TREE_INVALID) {
			PlantTreesOnTile(tile, tree, 0, 0);
		}
	}
}

static TrackStatus GetTileTrackStatus_Trees(TileIndex tile, TransportType mode, uint sub_mode, DiagDirection side)
{
	return 0;
}

static void ChangeTileOwner_Trees(TileIndex tile, Owner old_owner, Owner new_owner)
{
	/* not used */
}

void InitializeTrees()
{
	_trees_tick_ctr = 0;
}

static CommandCost TerraformTile_Trees(TileIndex tile, DoCommandFlag flags, int z_new, Slope tileh_new)
{
	return DoCommand(tile, 0, 0, flags, CMD_LANDSCAPE_CLEAR);
}


extern const TileTypeProcs _tile_type_trees_procs = {
	DrawTile_Trees,           // draw_tile_proc
	GetSlopePixelZ_Trees,     // get_slope_z_proc
	ClearTile_Trees,          // clear_tile_proc
	nullptr,                     // add_accepted_cargo_proc
	GetTileDesc_Trees,        // get_tile_desc_proc
	GetTileTrackStatus_Trees, // get_tile_track_status_proc
	nullptr,                     // click_tile_proc
	nullptr,                     // animate_tile_proc
	TileLoop_Trees,           // tile_loop_proc
	ChangeTileOwner_Trees,    // change_tile_owner_proc
	nullptr,                     // add_produced_cargo_proc
	nullptr,                     // vehicle_enter_tile_proc
	GetFoundation_Trees,      // get_foundation_proc
	TerraformTile_Trees,      // terraform_tile_proc
};
