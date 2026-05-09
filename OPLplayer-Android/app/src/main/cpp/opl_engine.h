#pragma once
#include <string>
#include <cstdint>

bool        OplEngine_Start();
void        OplEngine_Stop();
void        OplEngine_InitChip();
void        OplEngine_SetUdpEnabled(bool en);
void        OplEngine_SetSerialEnabled(bool en);
void        OplEngine_FeedUdpPacket(const uint8_t* data, int len);
void        OplEngine_FeedSerial(const uint8_t* data, int len);
void        OplEngine_SetVolume(float v);
void        OplEngine_SetGain(float g);
void        OplEngine_SetPrebufMs(int ms);
std::string OplEngine_GetStats();
