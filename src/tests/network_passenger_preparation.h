/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */
#ifndef TESTS_NETWORK_PASSENGER_PREPARATION_H
#define TESTS_NETWORK_PASSENGER_PREPARATION_H

#include "../network/network_server.h"
#include "../network/network_client.h"
#include "../network/core/packet.h"

/** Exercise the real packet handlers without a TCP connection or simulation ACKs. */
struct NetworkPassengerPreparationTestAccess {
	static Packet Read(Packet &source, NetworkSocketHandler *handler)
	{
		source.PrepareToSend();
		Packet result(Packet::ReadTag{}, handler, TCP_MTU, source.Size());
		result.TransferIn([&](std::span<uint8_t> target) {
			return source.TransferOutWithLimit([&](std::span<const uint8_t> bytes) {
				std::ranges::copy(bytes, target.begin()); return bytes.size();
			}, target.size());
		});
		REQUIRE(result.PrepareToRead()); result.Recv_uint8();
		return result;
	}
	static NetworkRecvStatus Ready(NetworkClientSocket &client, uint32_t frame)
	{
		Packet source(&client, PacketGameType::ClientPassengerPrepared, TCP_MTU); source.Send_uint32(frame);
		auto packet = Read(source, &client); return client.ReceiveClientPassengerPrepared(packet);
	}
	static NetworkRecvStatus Announce(ClientNetworkGameSocketHandler &client, bool preparing, uint32_t frame)
	{
		Packet source(&client, PacketGameType::ServerPassengerPrepare, TCP_MTU); source.Send_bool(preparing); source.Send_uint32(frame);
		auto packet = Read(source, &client); return client.ReceiveServerPassengerPrepare(packet);
	}
	static void Activate(ClientNetworkGameSocketHandler &client) { client.status = ClientNetworkGameSocketHandler::ServerStatus::Active; }
	static auto Frame(const NetworkClientSocket &client) { return client.passenger_prepare_frame; }
	static bool Prepared(const ClientNetworkGameSocketHandler &client) { return client.passenger_prepared; }
	static NetworkRecvStatus Sync(ClientNetworkGameSocketHandler &client, uint32_t frame, uint32_t seed, uint64_t checksum)
	{
		Packet source(&client, PacketGameType::ServerSync, TCP_MTU);
		source.Send_uint32(frame); source.Send_uint32(seed); source.Send_uint64(checksum);
		auto packet = Read(source, &client); return client.ReceiveServerSync(packet);
	}
	static bool CheckSync() { return ClientNetworkGameSocketHandler::CheckSync(); }
};

#endif
