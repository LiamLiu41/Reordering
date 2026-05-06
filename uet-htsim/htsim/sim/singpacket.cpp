#include "singpacket.h"

PacketDB<SingDataPacket> SingDataPacket::_packetdb;
PacketDB<SingAck> SingAck::_packetdb;
PacketDB<SingNack> SingNack::_packetdb;
PacketDB<SingCnp> SingCnp::_packetdb;
