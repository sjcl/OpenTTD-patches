/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file arena_alloc.cpp Test the bump allocator containers. */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../core/arena_alloc.hpp"

#include <set>

#include "../safeguards.h"

namespace {
	struct Node {
		uint64_t key, cost, parent, flags, extra[3];
		Node(uint64_t key) : key(key), cost(key * 3), parent(key + 1), flags(0), extra{key, key, key} {}
	};
	static_assert(sizeof(Node) == 56); // The size of a rail pathfinder node on 64-bit builds.
}

TEST_CASE("Bump allocator containers reuse large blocks across short-lived instances")
{
	std::set<const void *> first;
	{
		BumpAllocContainer<Node, 4096> items;
		for (uint64_t i = 0; i < 4096 * 3; ++i) CHECK(items.New(i)->key == i);
		for (size_t i = 0; i < 4096 * 3; i += 4096) first.insert(items.Get(i));
		CHECK(items.size() == 4096 * 3);
	}
	/* A following container receives the cached blocks, uninitialised as from malloc. */
	std::set<const void *> second;
	{
		BumpAllocContainer<Node, 4096> items;
		for (uint64_t i = 0; i < 4096 * 3; ++i) CHECK(items.New(i + 7)->cost == (i + 7) * 3);
		for (size_t i = 0; i < 4096 * 3; i += 4096) second.insert(items.Get(i));
		uint64_t sum = 0; items.Iterate([&](Node *node) { sum += node->key; });
		CHECK(sum == (4096 * 3) * (4096 * 3 - 1) / 2 + 7 * 4096 * 3);
		items.clear();
		CHECK(items.size() == 0);
		CHECK(items.New(1)->key == 1);
	}
	CHECK(first == second);
	/* Small blocks keep the original malloc/free behaviour. */
	BumpAllocContainer<Node, 16> small;
	for (uint64_t i = 0; i < 100; ++i) CHECK(small.New(i)->parent == i + 1);
}

TEST_CASE("Global bump allocator survives main thread cache teardown")
{
	/* This container outlives the main thread's TLS cache, like the global
	 * YAPF segment cache. CTest also checks the process's exit status after
	 * Catch has finished; destruction must not write into a dead block cache. */
	static BumpAllocContainer<Node, 4096> global;
	CHECK(global.New(17)->cost == 51);
	{
		BumpAllocContainer<Node, 4096> temporary;
		CHECK(temporary.New(23)->cost == 69);
	}
}
