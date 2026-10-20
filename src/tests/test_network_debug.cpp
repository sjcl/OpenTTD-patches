/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file test_network_debug.cpp Tests for network debug related functions. */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../network/core/tcp_game.h"
#include "../network/network_join_progress.h"

TEST_CASE("Map replay progress cannot extend the absolute join deadline", "[network][join][security]")
{
	using namespace std::chrono;
	const NetworkJoinProgress::Clock::time_point start{};
	NetworkJoinProgress client; client.Start(start); client.BeginReplay(UINT64_MAX, start);
	/* Normal 225-second replay is admitted with the existing inactivity limit. */
	for (int second = 1; second <= 225; ++second) {
		CHECK(client.Report(second, start + seconds(second)) == NetworkJoinProgress::Result::Accepted);
		CHECK_FALSE(client.Idle(start + seconds(second), milliseconds(15000)));
		CHECK_FALSE(client.Expired(start + seconds(second), seconds(900)));
	}
	CHECK(client.Idle(start + seconds(241), milliseconds(15000)));
	/* An attacker can keep making tiny valid claims, but cannot buy more time. */
	for (int second = 226; second <= 900; ++second) REQUIRE(client.Report(second, start + seconds(second)) == NetworkJoinProgress::Result::Accepted);
	CHECK(client.Expired(start + seconds(900), seconds(900)));
	client.Start(start + seconds(900)); client.BeginReplay(UINT64_MAX, start + seconds(900));
	CHECK(client.Expired(start + seconds(900), seconds(900)));
}

TEST_CASE("Map replay progress validates work and bounds bursts independently per client", "[network][join][security]")
{
	using namespace std::chrono;
	const NetworkJoinProgress::Clock::time_point start{};
	NetworkJoinProgress client, other; client.Start(start); other.Start(start);
	client.BeginReplay(1000, start); other.BeginReplay(1000, start);
	CHECK(client.Report(0, start) == NetworkJoinProgress::Result::Invalid);
	CHECK(client.Report(1001, start) == NetworkJoinProgress::Result::Invalid);
	CHECK(client.Report(1, start) == NetworkJoinProgress::Result::Accepted);
	CHECK(client.Report(1, start) == NetworkJoinProgress::Result::Invalid);
	for (int i = 2; i <= 16; ++i) CHECK(client.Report(i, start) == NetworkJoinProgress::Result::Accepted);
	CHECK(client.Report(17, start) == NetworkJoinProgress::Result::Flood);
	CHECK(other.Report(100, start) == NetworkJoinProgress::Result::Accepted);
	CHECK(client.Report(17, start + milliseconds(250)) == NetworkJoinProgress::Result::Accepted);
	CHECK(client.Report(18, start + milliseconds(250)) == NetworkJoinProgress::Result::Flood);
	CHECK(other.Idle(start + seconds(16), milliseconds(15000)));
}

TEST_CASE("Parallel host preparation never suspends the absolute join deadline", "[network][join][security]")
{
	using namespace std::chrono;
	const NetworkJoinProgress::Clock::time_point start{};
	NetworkJoinProgress client, other; client.Start(start); other.Start(start + seconds(100));
	client.BeginReplay(1000, start + seconds(400));
	CHECK(client.Report(1, start + seconds(899)) == NetworkJoinProgress::Result::Accepted);
	CHECK_FALSE(client.Idle(start + seconds(900), milliseconds(15000)));
	CHECK_FALSE(client.Expired(start + seconds(899), seconds(900)));
	CHECK(client.Expired(start + seconds(900), seconds(900)));
	CHECK(client.GetJoinTime(start + seconds(900)) == seconds(900));
	/* Queue transitions, another map and valid progress cannot renew the limit. */
	client.Start(start + seconds(900)); client.BeginReplay(1000, start + seconds(900));
	CHECK(client.Report(1, start + seconds(901)) == NetworkJoinProgress::Result::Accepted);
	CHECK(client.Expired(start + seconds(901), seconds(900)));
	CHECK_FALSE(other.Expired(start + seconds(999), seconds(900)));
	CHECK(other.Expired(start + seconds(1000), seconds(900)));
}


TEST_CASE("GetPacketGameTypeName")
{
	uint number_of_tests = 0;
#define CHECK_NAME(pkttype) number_of_tests++; CHECK(GetPacketGameTypeName(PacketGameType:: pkttype) == std::string_view{#pkttype})
	CHECK_NAME(ServerFull);
	CHECK_NAME(ServerBanned);
	CHECK_NAME(ClientJoin);
	CHECK_NAME(ServerError);
	CHECK_NAME(ClientUnused);
	CHECK_NAME(ServerUnused);
	CHECK_NAME(ServerGameInfo);
	CHECK_NAME(ClientGameInfo);
	CHECK_NAME(ServerNewGame);
	CHECK_NAME(ServerShutdown);
	CHECK_NAME(ServerGameInfoExtended);
	CHECK_NAME(ServerAuthenticationRequest);
	CHECK_NAME(ClientAuthenticationResponse);
	CHECK_NAME(ServerEnableEncryption);
	CHECK_NAME(ClientIdentify);
	CHECK_NAME(ServerCheckNewGRFs);
	CHECK_NAME(ClientNewGRFsChecked);
	CHECK_NAME(ServerNeedCompanyPassword);
	CHECK_NAME(ClientCompanyPassword);
	CHECK_NAME(ClientSettingsPassword);
	CHECK_NAME(ServerSettingsAccess);
	CHECK_NAME(ServerWelcome);
	CHECK_NAME(ServerClientInfo);
	CHECK_NAME(ClientGetMap);
	CHECK_NAME(ServerWaitForMap);
	CHECK_NAME(ServerMapBegin);
	CHECK_NAME(ServerMapSize);
	CHECK_NAME(ServerMapData);
	CHECK_NAME(ServerMapDone);
	CHECK_NAME(ClientMapOk);
	CHECK_NAME(ClientMapProgress);
	CHECK_NAME(ServerMapProgress);
	CHECK_NAME(ServerClientJoined);
	CHECK_NAME(ServerFrame);
	CHECK_NAME(ClientAck);
	CHECK_NAME(ServerSync);
	CHECK_NAME(ClientCommand);
	CHECK_NAME(ServerCommand);
	CHECK_NAME(ClientChat);
	CHECK_NAME(ServerChat);
	CHECK_NAME(ServerExternalChat);
	CHECK_NAME(ClientRemoteConsoleCommand);
	CHECK_NAME(ServerRemoteConsoleCommand);
	CHECK_NAME(ClientMove);
	CHECK_NAME(ServerMove);
	CHECK_NAME(ClientSetPassword);
	CHECK_NAME(ClientSetName);
	CHECK_NAME(ServerCompanyUpdate);
	CHECK_NAME(ServerConfigurationUpdate);
	CHECK_NAME(ClientQuit);
	CHECK_NAME(ServerQuit);
	CHECK_NAME(ClientError);
	CHECK_NAME(ServerErrorQuit);
	CHECK_NAME(ClientDesyncLog);
	CHECK_NAME(ServerDesyncLog);
	CHECK_NAME(ClientDesyncMessage);
	CHECK_NAME(ClientDesyncSyncData);
	CHECK_NAME(ServerPassengerPrepare);
	CHECK_NAME(ClientPassengerPrepared);
#undef CHECK_NAME
	CHECK(number_of_tests == static_cast<uint>(PacketGameType::End));
}
