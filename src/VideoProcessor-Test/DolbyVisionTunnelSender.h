#pragma once
/*
 * Copyright(C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-only
 */
// The independent Dolby Vision RGB-tunnel sender used by the formatter tests
// and the GPU tunnel unpack tests (shared since 2026-10-16).
#include "CppUnitTest.h"
#include <VideoFrame.h>
#include <VideoState.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace Tests
{
    namespace DvSender
    {
        using namespace Microsoft::VisualStudio::CppUnitTestFramework;
        using Packet = std::array<uint8_t, 128>;

        // A forward sender, independent of the decoder's bit-mask expressions.
        // ETSI GS CCM 001 V1.1.1 sections 6.2-6.4 define the byte syntax,
        // scrambling and raster order. RGB8 packing follows HDMI 1.4a 6.5.4
        // and the independently published CroqueMr/libreelec-x86-DV packer:
        // d9ad989e, CB1/src/dvbridge_render.c, lines 629-633.
        inline uint32_t SenderCrc(const uint8_t* bytes, size_t count)
        {
            uint32_t remainder = 0xFFFFFFFFU;
            for (size_t byte = 0; byte < count; ++byte)
            {
                for (int bit = 7; bit >= 0; --bit)
                {
                    const bool feedback = ((remainder >> 31) ^
                        ((bytes[byte] >> bit) & 1U)) != 0;
                    remainder <<= 1;
                    if (feedback)
                        remainder ^= 0x04C11DB7U;
                }
            }
            return remainder;
        }

        inline void Put16(std::vector<uint8_t>& bytes, size_t at, uint16_t value)
        {
            bytes[at] = static_cast<uint8_t>(value >> 8);
            bytes[at + 1] = static_cast<uint8_t>(value);
        }

        inline void Put32(uint8_t* bytes, uint32_t value)
        {
            for (unsigned byte = 0; byte < 4; ++byte)
                bytes[byte] = static_cast<uint8_t>(value >> (24 - byte * 8));
        }

        inline void FinishPacket(Packet& packet)
        {
            Put32(packet.data() + 124, SenderCrc(packet.data(), 124));
        }

        inline std::vector<uint8_t> DmBody(uint16_t peak = 3072,
            bool fullRange = true, size_t unknownPayload = 0)
        {
            std::vector<uint8_t> dm(71, 0);
            dm[1] = 1; // scene refresh
            const int16_t nonlinear[] = {
                8192, -4096, 2048, 0, 8192, -2048, 512, 256, 8192
            };
            const uint16_t linear[] = {
                16384, 0, 0, 0, 8192, 0, 0, 0, 4096
            };
            for (size_t i = 0; i < 9; ++i)
            {
                Put16(dm, 2 + i * 2, static_cast<uint16_t>(nonlinear[i]));
                Put16(dm, 32 + i * 2, linear[i]);
            }
            Put32(dm.data() + 20, 0x01000000U);
            Put32(dm.data() + 24, 0x08000000U);
            Put32(dm.data() + 28, 0x08000000U);
            dm[50] = dm[51] = 0xFF;
            dm[60] = 12;
            dm[61] = 2;
            dm[62] = 1;
            dm[63] = fullRange ? 1 : 0;
            Put16(dm, 64, 62);
            Put16(dm, 66, 3696);
            dm[69] = 42;
            dm[70] = unknownPayload ? 2 : 1;
            if (unknownPayload)
            {
                const size_t at = dm.size();
                dm.resize(at + 5 + unknownPayload, 0xA5);
                Put32(dm.data() + at, static_cast<uint32_t>(unknownPayload));
                dm[at + 4] = 200; // bounded unknown extension, deliberately skipped
            }
            const size_t at = dm.size();
            dm.resize(at + 11, 0);
            Put32(dm.data() + at, 6);
            dm[at + 4] = 1;
            Put16(dm, at + 5, 62);
            Put16(dm, at + 7, peak);
            Put16(dm, at + 9, 1024);
            return dm;
        }

        inline std::vector<Packet> Packetize(const std::vector<uint8_t>& body,
            uint8_t current = 3, uint8_t affected = 3,
            bool noMetadata = false, bool eos = false)
        {
            std::vector<Packet> packets;
            size_t offset = 0;
            do
            {
                Packet packet{};
                const bool first = packets.empty();
                const size_t capacity = first ? 119 : 121;
                const size_t chunk = noMetadata ? 0 :
                    (std::min)(capacity, body.size() - offset);
                const bool last = noMetadata || offset + chunk == body.size();
                const unsigned kind = first ? (last ? 0 : 1) : (last ? 3 : 2);
                packet[0] = static_cast<uint8_t>((kind << 6) | (noMetadata ? 1 : 0));
                packet[1] = static_cast<uint8_t>((affected << 4) | current);
                packet[2] = eos ? 1 : 0;
                if (first && !noMetadata)
                {
                    packet[3] = static_cast<uint8_t>(body.size() >> 8);
                    packet[4] = static_cast<uint8_t>(body.size());
                }
                if (chunk)
                    std::copy_n(body.data() + offset, chunk,
                        packet.data() + (first ? 5 : 3));
                FinishPacket(packet);
                packets.push_back(packet);
                offset += chunk;
                if (last)
                    break;
            } while (true);
            return packets;
        }

        inline void Put32LittleEndian(uint8_t* bytes, uint32_t value)
        {
            for (unsigned byte = 0; byte < 4; ++byte)
                bytes[byte] = static_cast<uint8_t>(value >> (byte * 8));
        }

        // audit 2026-10-16: encoder-side reference for every container and
        // timing. The sender packs a known 12-bit I/C picture as HDMI RGB8
        // (HDMI 1.4a 6.5.4), places each RGB8 byte in a DeckLink 10-bit field
        // (high8 = byte << 2, low8 = byte), and writes the word as r210
        // ([00 R G B] big-endian), R10b ([R G B pad] big-endian) or R10l
        // ([R G B pad] little-endian), independently of the decoder.
        struct SenderPicture
        {
            VideoStateComPtr state;
            std::vector<uint16_t> intensity, chroma;
            std::vector<BYTE> input;
            bool high8;
            VideoFrameEncoding encoding = VideoFrameEncoding::R210;
            unsigned padding = 0;         // R10 bottom two bits, must be ignored
            bool rangeConverted = false;  // card rescales 8-bit RGB to 64-940

            SenderPicture(bool high, uint32_t width = 130, uint32_t height = 128,
                VideoFrameEncoding container = VideoFrameEncoding::R210,
                unsigned timeScale = 24000, unsigned frameDuration = 1001,
                bool interlaced = false, unsigned r10Padding = 0,
                bool convertRange = false)
                : high8(high), encoding(container), padding(r10Padding),
                rangeConverted(convertRange)
            {
                state = new VideoState();
                state->valid = true;
                state->displayMode = std::make_shared<DisplayMode>(
                    width, height, interlaced, timeScale, frameDuration);
                state->videoFrameEncoding = container;
                state->dolbyVisionTunnel = true;
                intensity.resize(static_cast<size_t>(width) * height);
                chroma.resize(intensity.size());
                input.resize(state->BytesPerFrame(), 0xAD);
                for (size_t i = 0; i < intensity.size(); ++i)
                {
                    intensity[i] = static_cast<uint16_t>(i & 4095U);
                    chroma[i] = static_cast<uint16_t>((i * 73U + 31U) & 4095U);
                    Pack(i);
                }
            }

            void WriteRgb8(size_t pixel, unsigned r8, unsigned g8, unsigned b8)
            {
                const unsigned rgb8[] = { r8, g8, b8 };
                uint32_t field[3];
                for (size_t i = 0; i < 3; ++i)
                {
                    field[i] = rangeConverted ?
                        64U + (rgb8[i] * 876U + 127U) / 255U :
                        rgb8[i] * (high8 ? 4U : 1U);
                }
                const uint32_t r210 = (field[0] << 20) | (field[1] << 10) | field[2];
                const size_t width = state->displayMode->FrameWidth();
                uint8_t* at = input.data() + (pixel / width) * state->BytesPerRow() +
                    (pixel % width) * 4;
                if (encoding == VideoFrameEncoding::R210)
                    Put32(at, r210);
                else if (encoding == VideoFrameEncoding::R10b)
                    Put32(at, (r210 << 2) | (padding & 3U));
                else
                    Put32LittleEndian(at, (r210 << 2) | (padding & 3U));
            }

            void Pack(size_t pixel)
            {
                const unsigned y = intensity[pixel], c = chroma[pixel];
                WriteRgb8(pixel, c / 16, y / 16, (c % 16) * 16 + y % 16);
            }

            // Ordinary SDR RGB: a horizontal grey ramp (no tunnel structure).
            void FillSdrRamp()
            {
                const size_t width = state->displayMode->FrameWidth();
                for (size_t pixel = 0; pixel < intensity.size(); ++pixel)
                {
                    const unsigned v = static_cast<unsigned>(
                        (pixel % width) * 255 / (width - 1));
                    WriteRgb8(pixel, v, v, v);
                }
            }

            void SetFlat(uint16_t i, uint16_t c)
            {
                std::fill(intensity.begin(), intensity.end(), i);
                std::fill(chroma.begin(), chroma.end(), c);
                for (size_t pixel = 0; pixel < intensity.size(); ++pixel)
                    Pack(pixel);
            }

            void Embed(const std::vector<Packet>& packets)
            {
                Assert::IsTrue(packets.size() * 3072 <= intensity.size());
                for (size_t p = 0; p < packets.size(); ++p)
                {
                    for (size_t copy = 0; copy < 3; ++copy)
                    {
                        for (size_t bit = 0; bit < 1024; ++bit)
                        {
                            const size_t pixel = p * 3072 + copy * 1024 + bit;
                            const unsigned metadataBit =
                                (packets[p][bit / 8] >> (7 - bit % 8)) & 1U;
                            unsigned parity = 0;
                            for (unsigned b = 0; b < 12; ++b)
                            {
                                parity ^= (intensity[pixel] >> b) & 1U;
                                if (b != 0)
                                    parity ^= (chroma[pixel] >> b) & 1U;
                            }
                            chroma[pixel] = static_cast<uint16_t>(
                                (chroma[pixel] / 2) * 2 + (metadataBit ^ parity));
                            Pack(pixel);
                        }
                    }
                }
            }

            void CorruptCopy(size_t packet, size_t copy, size_t bit = 17)
            {
                const size_t pixel = packet * 3072 + copy * 1024 + bit;
                chroma[pixel] ^= 1;
                Pack(pixel);
            }

            void CorruptAllCopies(size_t packet = 0)
            {
                for (size_t copy = 0; copy < 3; ++copy)
                    CorruptCopy(packet, copy);
            }
        };
    }
}
