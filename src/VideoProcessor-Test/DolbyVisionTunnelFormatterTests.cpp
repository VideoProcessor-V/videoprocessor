/*
 * Copyright(C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "pch.h"
#include "CppUnitTest.h"
#include <VideoFrame.h>
#include <VideoState.h>
#include <CpuFeatures.h>
#include <video_frame_formatter/CR210DolbyVisionTunnelFormatter.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include <DebugLog.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
    namespace
    {
        using Packet = std::array<uint8_t, 128>;
        using Formatter = CR210DolbyVisionTunnelFormatter;

        // A forward sender, independent of the decoder's bit-mask expressions.
        // ETSI GS CCM 001 V1.1.1 sections 6.2-6.4 define the byte syntax,
        // scrambling and raster order. RGB8 packing follows HDMI 1.4a 6.5.4
        // and the independently published CroqueMr/libreelec-x86-DV packer:
        // d9ad989e, CB1/src/dvbridge_render.c, lines 629-633.
        uint32_t SenderCrc(const uint8_t* bytes, size_t count)
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

        void Put16(std::vector<uint8_t>& bytes, size_t at, uint16_t value)
        {
            bytes[at] = static_cast<uint8_t>(value >> 8);
            bytes[at + 1] = static_cast<uint8_t>(value);
        }

        void Put32(uint8_t* bytes, uint32_t value)
        {
            for (unsigned byte = 0; byte < 4; ++byte)
                bytes[byte] = static_cast<uint8_t>(value >> (24 - byte * 8));
        }

        void FinishPacket(Packet& packet)
        {
            Put32(packet.data() + 124, SenderCrc(packet.data(), 124));
        }

        std::vector<uint8_t> DmBody(uint16_t peak = 3072,
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

        std::vector<Packet> Packetize(const std::vector<uint8_t>& body,
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

        void Put32LittleEndian(uint8_t* bytes, uint32_t value)
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

        DolbyVisionTunnelFrameMetadata Deliver(Formatter& formatter,
            SenderPicture& picture, uint64_t counter,
            std::vector<BYTE>* converted = nullptr)
        {
            std::vector<BYTE> output(formatter.GetOutFrameSize(), 0xCD);
            VideoFrame frame(picture.input.data(), counter, 0, nullptr);
            Assert::IsTrue(formatter.FormatVideoFrame(frame, output.data()));
            if (converted)
                *converted = std::move(output);
            return formatter.GetDolbyVisionTunnelMetadata();
        }

        void AssertExactSamples(const SenderPicture& picture,
            const std::vector<BYTE>& output)
        {
            const auto* samples = reinterpret_cast<const uint16_t*>(output.data());
            Assert::IsTrue(output.size() >= picture.intensity.size() * 4);
            for (size_t pixel = 0; pixel < picture.intensity.size(); ++pixel)
            {
                // Only assert on a mismatch: 4K pictures have 16M samples.
                if (samples[pixel] != picture.intensity[pixel] * 16U)
                    Assert::AreEqual(static_cast<unsigned>(picture.intensity[pixel] * 16),
                        static_cast<unsigned>(samples[pixel]), L"12-bit intensity changed");
                if (samples[picture.intensity.size() + pixel] != picture.chroma[pixel] * 16U)
                    Assert::AreEqual(static_cast<unsigned>(picture.chroma[pixel] * 16),
                        static_cast<unsigned>(samples[picture.intensity.size() + pixel]),
                        L"12-bit chroma changed");
            }
        }

        // audit 2026-10-16: appends one extension block after the existing ones.
        void AddExtension(std::vector<uint8_t>& dm, uint8_t level,
            const std::vector<uint8_t>& payload)
        {
            const size_t at = dm.size();
            dm.resize(at + 5 + payload.size(), 0);
            Put32(dm.data() + at, static_cast<uint32_t>(payload.size()));
            dm[at + 4] = level;
            std::copy(payload.begin(), payload.end(), dm.begin() + at + 5);
            ++dm[70];
        }

        void Push16(std::vector<uint8_t>& bytes, uint16_t value)
        {
            bytes.push_back(static_cast<uint8_t>(value >> 8));
            bytes.push_back(static_cast<uint8_t>(value));
        }

        // Pictures shown as black until the derived SDR-exit window ends.
        uint64_t SdrExitPictures(unsigned timeScale, unsigned frameDuration)
        {
            return static_cast<uint64_t>(std::ceil(
                2.0 * timeScale / frameDuration - 1e-9));
        }

        bool AllZero(const std::vector<BYTE>& bytes)
        {
            return std::all_of(bytes.begin(), bytes.end(), [](BYTE b) { return b == 0; });
        }

        std::string tunnelLog;
        void __cdecl CollectTunnelLog(const char* message)
        {
            tunnelLog += std::string(message) + "\n";
        }
        struct TunnelLogSink
        {
            TunnelLogSink() { tunnelLog.clear(); DebugLog::SetExternalSink(CollectTunnelLog); }
            ~TunnelLogSink() { DebugLog::SetExternalSink(nullptr); }
        };

        size_t CountOccurrences(const std::string& text, const std::string& needle)
        {
            size_t count = 0;
            for (size_t at = text.find(needle); at != std::string::npos;
                at = text.find(needle, at + needle.size()))
                ++count;
            return count;
        }

        void AssertPeak(const DolbyVisionTunnelFrameMetadata& dm, uint16_t peak)
        {
            Assert::IsTrue(dm.valid && dm.hasSceneMetadata);
            Assert::AreEqual(static_cast<unsigned>(peak), static_cast<unsigned>(dm.sceneMaxPq));
            Assert::IsFalse(dm.allowSdrFallback);
        }
    }

    TEST_CLASS(DolbyVisionTunnelFormatterTests)
    {
    public:
        TEST_METHOD(IndependentSenderHasKnownCrcAndPackingAnchors)
        {
            const uint8_t reference[] = { '1','2','3','4','5','6','7','8','9' };
            Assert::AreEqual(0x0376E6E7U, SenderCrc(reference, sizeof(reference)));
            const auto packets = Packetize(DmBody());
            // Fixed footer of the complete single-packet fixture, not a roundtrip assertion.
            const uint8_t footer[] = { 0xBD, 0x58, 0x9A, 0x44 };
            Assert::IsTrue(std::equal(footer, footer + 4, packets[0].data() + 124));
            Assert::AreEqual(0U, SenderCrc(packets[0].data(), packets[0].size()));
            SenderPicture picture(false);
            // Y=0xABC, C=0x123 => RGB8=(0x12,0xAB,0x3C), r210=0x0122AC3C.
            picture.intensity[3073] = 0xABC;
            picture.chroma[3073] = 0x123;
            picture.Pack(3073);
            const size_t at = (3073 / 130) * picture.state->BytesPerRow() + (3073 % 130) * 4;
            const uint8_t known[] = { 0x01, 0x22, 0xAC, 0x3C };
            Assert::IsTrue(std::equal(known, known + 4, picture.input.data() + at));
            picture.Embed(packets);
            Formatter formatter;
            formatter.OnVideoState(picture.state);
            std::vector<BYTE> output;
            AssertPeak(Deliver(formatter, picture, 1, &output), 3072);
            AssertExactSamples(picture, output);
        }

        TEST_METHOD(SinglePacketReadsSignedMatricesOffsetsAndRange)
        {
            for (const bool high8 : { false, true })
            {
                SenderPicture picture(high8);
                picture.Embed(Packetize(DmBody(3072, false)));
                Formatter formatter;
                formatter.OnVideoState(picture.state);
                const auto dm = Deliver(formatter, picture, 1);
                AssertPeak(dm, 3072);
                Assert::IsFalse(dm.fullRange);
                Assert::AreEqual(-0.5f, dm.nonlinear[0][1]);
                Assert::AreEqual(0.25f, dm.nonlinear[0][2]);
                Assert::AreEqual(0.0625f, dm.nonlinearOffset[0]);
                Assert::AreEqual(0.5f, dm.nonlinearOffset[1]);
                Assert::AreEqual(1.0f, dm.linear[0][0]);
                Assert::AreEqual(0.25f, dm.linear[2][2]);
                Assert::AreEqual(62U, static_cast<unsigned>(dm.sourceMinPq));
                Assert::AreEqual(3696U, static_cast<unsigned>(dm.sourceMaxPq));
                Assert::AreEqual(high8 ? 1U : 2U, static_cast<unsigned>(dm.rgbBitAlignment));
                const auto contract = formatter.GetOutputContract();
                Assert::IsTrue(contract.sampleRange == VideoFrameSampleRange::LIMITED);
                Assert::AreEqual(12U, static_cast<unsigned>(contract.colorDepth));
                Assert::AreEqual(4U, static_cast<unsigned>(contract.bitShift));
            }
        }

        TEST_METHOD(AllTwelveBitCodesSurviveScalarAndAvx2WithPaddingAndTails)
        {
            for (const bool high8 : { false, true })
            {
                // DisplayMode requires both dimensions >=100. Cover 0/2/6 SIMD tail pixels.
                for (const unsigned width : { 128U, 130U, 134U })
                {
                    SenderPicture picture(high8, width, 100);
                    picture.Embed(Packetize(DmBody()));
                    for (const auto method : { Formatter::ConversionMethod::SCALAR,
                        Formatter::ConversionMethod::AVX2 })
                    {
                        if (method == Formatter::ConversionMethod::AVX2 &&
                            !CpuFeatures::SupportsAvx2Kernels())
                        {
                            Logger::WriteMessage(L"AVX2 qualification skipped: CPU/OS lacks AVX2 support" );
                            continue;
                        }
                        Formatter formatter;
                        formatter.SetConversionMethod(method);
                        formatter.OnVideoState(picture.state);
                        std::vector<BYTE> output;
                        AssertPeak(Deliver(formatter, picture, 1, &output), 3072);
                        AssertExactSamples(picture, output);
                    }
                }
            }
        }

        TEST_METHOD(ThreadedRowsAndUnevenTailPreserveEverySample)
        {
            for (const bool high8 : { false, true })
            {
                SenderPicture picture(high8, 1922, 1081);
                picture.Embed(Packetize(DmBody()));
                for (const auto method : { Formatter::ConversionMethod::SCALAR,
                    Formatter::ConversionMethod::AVX2 })
                {
                    if (method == Formatter::ConversionMethod::AVX2 &&
                        !CpuFeatures::SupportsAvx2Kernels())
                    {
                        Logger::WriteMessage(L"AVX2 qualification skipped: CPU/OS lacks AVX2 support" );
                        continue;
                    }
                    Formatter formatter;
                    formatter.SetConversionMethod(method);
                    formatter.OnVideoState(picture.state);
                    std::vector<BYTE> output;
                    AssertPeak(Deliver(formatter, picture, 1, &output), 3072);
                    AssertExactSamples(picture, output);
                }
            }
        }

        TEST_METHOD(AlignmentChangeWithNoMetadataUsesNewLayoutAndActiveDm)
        {
            SenderPicture high(true), low(false);
            Formatter formatter;
            formatter.OnVideoState(high.state);
            high.Embed(Packetize(DmBody()));
            AssertPeak(Deliver(formatter, high, 1), 3072);
            low.Embed(Packetize({}, 3, 3, true));
            std::vector<BYTE> output;
            AssertPeak(Deliver(formatter, low, 2, &output), 3072);
            AssertExactSamples(low, output);
            high.Embed(Packetize({}, 3, 3, true));
            AssertPeak(Deliver(formatter, high, 3, &output), 3072);
            AssertExactSamples(high, output);
        }
        TEST_METHOD(ContinuationBoundariesAndUnknownBlocksPreserveL1)
        {
            // Body lengths 119, 120, 124, 244, 512. L1 crosses both first
            // and later packet boundaries; 512 is the ETSI five-packet example.
            for (const size_t unknown : { size_t(32), size_t(33), size_t(37),
                size_t(157), size_t(425) })
            {
                for (const bool high8 : { false, true })
                {
                    SenderPicture picture(high8);
                    picture.Embed(Packetize(DmBody(2048, true, unknown)));
                    Formatter formatter;
                    formatter.OnVideoState(picture.state);
                    const auto dm = Deliver(formatter, picture, 1);
                    AssertPeak(dm, 2048);
                    Assert::AreEqual(1024U, static_cast<unsigned>(dm.sceneAvgPq));
                }
            }
        }

        TEST_METHOD(RepeatedCopiesRecoverFirstAndContinuationCorruption)
        {
            for (const bool high8 : { false, true })
            {
                SenderPicture picture(high8);
                picture.Embed(Packetize(DmBody(2048, true, 157)));
                picture.CorruptCopy(0, 0);
                picture.CorruptCopy(0, 1);
                picture.CorruptCopy(1, 0);
                picture.CorruptCopy(2, 0);
                picture.CorruptCopy(2, 1);
                Formatter formatter;
                formatter.OnVideoState(picture.state);
                AssertPeak(Deliver(formatter, picture, 1), 2048);
            }
        }

        TEST_METHOD(DamagedContinuationNeverReplacesActiveMetadata)
        {
            SenderPicture picture(false);
            Formatter formatter;
            formatter.OnVideoState(picture.state);
            picture.Embed(Packetize(DmBody()));
            AssertPeak(Deliver(formatter, picture, 1), 3072);
            picture.Embed(Packetize(DmBody(2048, true, 157)));
            picture.CorruptAllCopies(1);
            AssertPeak(Deliver(formatter, picture, 2), 3072);
            picture.Embed(Packetize(DmBody(2048, true, 157)));
            AssertPeak(Deliver(formatter, picture, 3), 2048);
        }

        TEST_METHOD(MismatchedContinuationIdsFlagsAndKindsAreRejected)
        {
            for (const size_t field : { size_t(0), size_t(1), size_t(2) })
            {
                SenderPicture picture(true);
                Formatter formatter;
                formatter.OnVideoState(picture.state);
                picture.Embed(Packetize(DmBody()));
                AssertPeak(Deliver(formatter, picture, 1), 3072);
                auto packets = Packetize(DmBody(2048, true, 157));
                packets[1][field] ^= field == 0 ? 0x40 : 1;
                FinishPacket(packets[1]);
                picture.Embed(packets);
                AssertPeak(Deliver(formatter, picture, 2), 3072);
            }
        }

        TEST_METHOD(DeclaredLengthBoundsAndMalformedExtensionsDoNotReplaceDm)
        {
            for (const uint16_t declared : { uint16_t(70), uint16_t(120), uint16_t(0x2F01) })
            {
                SenderPicture picture(false);
                Formatter formatter;
                formatter.OnVideoState(picture.state);
                picture.Embed(Packetize(DmBody()));
                AssertPeak(Deliver(formatter, picture, 1), 3072);
                auto packets = Packetize(DmBody(2048));
                packets[0][3] = static_cast<uint8_t>(declared >> 8);
                packets[0][4] = static_cast<uint8_t>(declared);
                FinishPacket(packets[0]);
                picture.Embed(packets);
                AssertPeak(Deliver(formatter, picture, 2), 3072);
            }
            SenderPicture picture(false);
            Formatter formatter;
            formatter.OnVideoState(picture.state);
            picture.Embed(Packetize(DmBody()));
            AssertPeak(Deliver(formatter, picture, 1), 3072);
            auto bad = DmBody(2048);
            Put32(bad.data() + 71, 7); // six payload bytes available
            picture.Embed(Packetize(bad));
            AssertPeak(Deliver(formatter, picture, 2), 3072);
            bad = DmBody(4096); // L1 values must be 12-bit
            picture.Embed(Packetize(bad));
            AssertPeak(Deliver(formatter, picture, 3), 3072);
        }

        TEST_METHOD(DeclaredPacketCountCannotReadBeyondActiveRaster)
        {
            SenderPicture picture(false, 100, 100); // minimum mode; only three packets fit
            Formatter formatter;
            formatter.OnVideoState(picture.state);
            picture.Embed(Packetize(DmBody()));
            AssertPeak(Deliver(formatter, picture, 1), 3072);
            auto packets = Packetize(DmBody());
            packets[0][0] = 0x40; // valid first-of-many kind
            packets[0][3] = 2;
            packets[0][4] = 0; // valid declared 512-byte body needs five packets
            FinishPacket(packets[0]);
            picture.Embed(packets);
            AssertPeak(Deliver(formatter, picture, 2), 3072);
        }

        TEST_METHOD(UnsupportedHeaderCannotAuthorizeDolbyVisionLock)
        {
            // Reserved metadata type/version, reserved EOS bits, and an
            // affected ID that is neither current nor immediate next.
            for (const unsigned variant : { 0U, 1U, 2U, 3U })
            {
                SenderPicture picture(false);
                auto packets = Packetize(DmBody());
                if (variant == 0) packets[0][0] |= 0x10;
                if (variant == 1) packets[0][0] |= 0x02;
                if (variant == 2) packets[0][2] |= 0x02;
                if (variant == 3) packets[0][1] = 0x53;
                FinishPacket(packets[0]);
                picture.Embed(packets);
                Formatter formatter;
                formatter.OnVideoState(picture.state);
                const auto dm = Deliver(formatter, picture, 1);
                Assert::IsFalse(dm.valid);
                Assert::IsTrue(dm.allowSdrFallback);
            }
        }
        TEST_METHOD(ValidHeaderBlocksSdrEvenWhenBodyIsUnsupported)
        {
            SenderPicture picture(false);
            Formatter formatter;
            formatter.OnVideoState(picture.state);
            auto body = DmBody();
            body[60] = 10; // unsupported tunnel code depth
            picture.Embed(Packetize(body));
            const auto dm = Deliver(formatter, picture, 1);
            Assert::IsFalse(dm.valid || dm.allowSdrFallback);
        }

        TEST_METHOD(NoMetadataRetainsActiveDmAndDoesNotResurrectAbsentDm)
        {
            SenderPicture picture(false);
            Formatter formatter;
            formatter.OnVideoState(picture.state);
            picture.Embed(Packetize({}, 3, 3, true));
            auto dm = Deliver(formatter, picture, 1);
            Assert::IsFalse(dm.valid || dm.allowSdrFallback);
            picture.Embed(Packetize(DmBody()));
            AssertPeak(Deliver(formatter, picture, 2), 3072);
            for (uint64_t counter = 3; counter < 40; ++counter)
            {
                picture.Embed(Packetize({}, 3, 3, true));
                AssertPeak(Deliver(formatter, picture, counter), 3072);
            }
            picture.Embed(Packetize({}, 4, 4, true));
            dm = Deliver(formatter, picture, 40);
            Assert::IsFalse(dm.valid || dm.allowSdrFallback);
        }

        TEST_METHOD(AdvanceMetadataAppliesOnlyToImmediateNextPicture)
        {
            SenderPicture picture(false);
            Formatter formatter;
            formatter.OnVideoState(picture.state);
            picture.Embed(Packetize(DmBody(), 15, 15));
            AssertPeak(Deliver(formatter, picture, 10), 3072);
            picture.Embed(Packetize(DmBody(2048), 15, 0)); // ID wrap
            AssertPeak(Deliver(formatter, picture, 11), 3072);
            picture.Embed(Packetize({}, 0, 0, true));
            picture.CorruptAllCopies(); // contiguous counter alone associates pending DM
            AssertPeak(Deliver(formatter, picture, 12), 2048);
        }

        TEST_METHOD(DroppedNextPictureWaitsForHeaderNamingAffectedId)
        {
            SenderPicture picture(true);
            Formatter formatter;
            formatter.OnVideoState(picture.state);
            picture.Embed(Packetize(DmBody(), 1, 1));
            AssertPeak(Deliver(formatter, picture, 10), 3072);
            picture.Embed(Packetize(DmBody(2048), 1, 2));
            AssertPeak(Deliver(formatter, picture, 11), 3072);
            picture.CorruptAllCopies();
            AssertPeak(Deliver(formatter, picture, 13), 3072);
            picture.Embed(Packetize({}, 2, 2, true));
            AssertPeak(Deliver(formatter, picture, 14), 2048);
        }

        TEST_METHOD(UnrelatedCurrentIdDiscardsPendingAndActiveDm)
        {
            SenderPicture picture(true);
            Formatter formatter;
            formatter.OnVideoState(picture.state);
            picture.Embed(Packetize(DmBody(), 1, 1));
            AssertPeak(Deliver(formatter, picture, 10), 3072);
            picture.Embed(Packetize(DmBody(2048), 1, 2));
            AssertPeak(Deliver(formatter, picture, 11), 3072);
            picture.Embed(Packetize({}, 3, 3, true));
            const auto dm = Deliver(formatter, picture, 13);
            Assert::IsFalse(dm.valid || dm.allowSdrFallback);
            picture.Embed(Packetize({}, 2, 2, true));
            Assert::IsFalse(Deliver(formatter, picture, 14).valid);
        }

        TEST_METHOD(EosNoMetadataAllowsSdrAfterBoundaryIncludingCounterGap)
        {
            SenderPicture picture(false);
            Formatter formatter;
            formatter.OnVideoState(picture.state);
            picture.Embed(Packetize(DmBody()));
            AssertPeak(Deliver(formatter, picture, 10), 3072);
            picture.Embed(Packetize({}, 3, 3, true, true));
            AssertPeak(Deliver(formatter, picture, 11), 3072);
            // A blank RGB frame contains no CRC-valid metadata.
            std::fill(picture.input.begin(), picture.input.end(), 0);
            const auto dm = Deliver(formatter, picture, 14);
            Assert::IsFalse(dm.valid);
            Assert::IsTrue(dm.allowSdrFallback);
            picture.Embed(Packetize(DmBody(2048)));
            AssertPeak(Deliver(formatter, picture, 15), 2048);
        }

        TEST_METHOD(MovieExitBlanksFirstUntrustedPictureButRetainsDmForRecovery)
        {
            for (const bool high8 : { false, true })
            {
                SenderPicture picture(high8);
                Formatter formatter;
                formatter.OnVideoState(picture.state);
                picture.Embed(Packetize(DmBody()));
                Assert::IsFalse(Deliver(formatter, picture, 1).requiresBlackFrame);

                // HDMI goes blank before capture publishes its new menu mode.
                // Cached DV decoding of this RGB picture used to create color.
                std::fill(picture.input.begin(), picture.input.end(), 0);
                const auto lost = Deliver(formatter, picture, 2);
                AssertPeak(lost, 3072); // cache survives a brief interruption
                Assert::IsTrue(lost.requiresBlackFrame);
                Assert::IsFalse(lost.allowSdrFallback);

                SenderPicture resumed(high8);
                resumed.Embed(Packetize({}, 3, 3, true));
                std::vector<BYTE> output;
                const auto recovered = Deliver(formatter, resumed, 3, &output);
                AssertPeak(recovered, 3072);
                Assert::IsFalse(recovered.requiresBlackFrame);
                AssertExactSamples(resumed, output);
            }
        }

        TEST_METHOD(BlankingPersistsThroughLostDmAndStopsOnFreshDmOrEos)
        {
            SenderPicture picture(false);
            Formatter formatter;
            formatter.OnVideoState(picture.state);
            picture.Embed(Packetize(DmBody()));
            Deliver(formatter, picture, 1);
            std::fill(picture.input.begin(), picture.input.end(), 0);
            for (uint64_t counter = 2; counter <= 25; ++counter)
                Assert::IsTrue(Deliver(formatter, picture, counter).requiresBlackFrame);
            picture.Embed(Packetize({}, 3, 3, true));
            const auto noDm = Deliver(formatter, picture, 26);
            Assert::IsFalse(noDm.valid);
            Assert::IsTrue(noDm.requiresBlackFrame);
            picture.Embed(Packetize(DmBody(2048)));
            Assert::IsFalse(Deliver(formatter, picture, 27).requiresBlackFrame);
            picture.Embed(Packetize({}, 3, 3, true, true));
            Assert::IsFalse(Deliver(formatter, picture, 28).requiresBlackFrame);
            std::fill(picture.input.begin(), picture.input.end(), 0);
            const auto sdr = Deliver(formatter, picture, 29);
            Assert::IsFalse(sdr.requiresBlackFrame);
            Assert::IsTrue(sdr.allowSdrFallback);
        }

        TEST_METHOD(RecoveredHeaderAndDamagedContinuationDoNotBlankKnownDvPixels)
        {
            SenderPicture picture(true);
            Formatter formatter;
            formatter.OnVideoState(picture.state);
            picture.Embed(Packetize(DmBody()));
            Deliver(formatter, picture, 1);
            picture.Embed(Packetize(DmBody(2048, true, 157)));
            picture.CorruptCopy(0, 0);
            picture.CorruptCopy(0, 1); // third header copy still authenticates DV
            picture.CorruptAllCopies(1); // old DM remains usable for same ID
            const auto dm = Deliver(formatter, picture, 2);
            AssertPeak(dm, 3072);
            Assert::IsFalse(dm.requiresBlackFrame);
        }

        TEST_METHOD(LossLimitSuspendsUntilFreshCompleteDmWithoutSdrFallback)
        {
            SenderPicture picture(true);
            Formatter formatter;
            formatter.OnVideoState(picture.state);
            picture.Embed(Packetize(DmBody()));
            AssertPeak(Deliver(formatter, picture, 1), 3072);
            picture.CorruptAllCopies();
            for (uint64_t counter = 2; counter < 25; ++counter)
                AssertPeak(Deliver(formatter, picture, counter), 3072);
            std::vector<BYTE> output;
            auto dm = Deliver(formatter, picture, 25, &output);
            Assert::IsFalse(dm.valid || dm.allowSdrFallback);
            Assert::IsTrue(std::all_of(output.begin(), output.end(), [](BYTE b) { return b == 0; }));
            picture.Embed(Packetize({}, 3, 3, true));
            dm = Deliver(formatter, picture, 26);
            Assert::IsFalse(dm.valid || dm.allowSdrFallback);
            picture.Embed(Packetize(DmBody(2048)));
            AssertPeak(Deliver(formatter, picture, 27), 2048);
            formatter.OnVideoState(picture.state);
            std::fill(picture.input.begin(), picture.input.end(), 0);
            dm = Deliver(formatter, picture, 28);
            Assert::IsFalse(dm.valid);
            Assert::IsTrue(dm.allowSdrFallback);
        }

        // ---- audit 2026-10-16: metadata-ID wrap ---------------------------

        TEST_METHOD(AdvanceMetadataFollowsIdsAcrossRepeatedWrap)
        {
            // 20 consecutive advance transactions: IDs 13,14,15,0,1,...,0,1
            // (crossing 15 -> 0 twice). Each picture applies the previous
            // picture's DM and announces the next one.
            SenderPicture picture(false);
            Formatter formatter;
            formatter.OnVideoState(picture.state);
            picture.Embed(Packetize(DmBody(1000), 13, 13));
            AssertPeak(Deliver(formatter, picture, 1), 1000);
            uint8_t id = 13;
            uint16_t expected = 1000;
            for (uint16_t step = 0; step < 20; ++step)
            {
                const uint8_t next = static_cast<uint8_t>((id + 1) & 15);
                picture.Embed(Packetize(DmBody(static_cast<uint16_t>(1100 + step)), id, next));
                const auto dm = Deliver(formatter, picture, 2U + step);
                AssertPeak(dm, expected);
                Assert::IsFalse(dm.requiresBlackFrame);
                expected = static_cast<uint16_t>(1100 + step);
                id = next;
            }
        }

        TEST_METHOD(ActiveIdMatchAfterSixteenHeaderlessPicturesIsAmbiguous)
        {
            for (const bool wrapPossible : { false, true })
            {
                SenderPicture picture(false);
                Formatter formatter;
                formatter.OnVideoState(picture.state);
                picture.Embed(Packetize(DmBody(), 3, 3));
                AssertPeak(Deliver(formatter, picture, 1), 3072);
                picture.CorruptAllCopies();
                // 14 lost pictures: the ID can have moved at most 15 steps.
                // 15 lost pictures: 16 steps, i.e. possibly all the way round.
                const uint64_t resume = wrapPossible ? 17 : 16;
                for (uint64_t counter = 2; counter < resume; ++counter)
                    Assert::IsTrue(Deliver(formatter, picture, counter).requiresBlackFrame);
                picture.Embed(Packetize({}, 3, 3, true));
                const auto dm = Deliver(formatter, picture, resume);
                if (!wrapPossible)
                {
                    AssertPeak(dm, 3072);
                    Assert::IsFalse(dm.requiresBlackFrame);
                    continue;
                }
                Assert::IsFalse(dm.valid || dm.allowSdrFallback);
                Assert::IsTrue(dm.requiresBlackFrame);
                picture.Embed(Packetize(DmBody(2048), 3, 3));
                AssertPeak(Deliver(formatter, picture, resume + 1), 2048);
            }
        }

        TEST_METHOD(PendingAdvanceDmIsNotAppliedAfterPossibleIdWrap)
        {
            SenderPicture picture(true);
            Formatter formatter;
            formatter.OnVideoState(picture.state);
            picture.Embed(Packetize(DmBody(), 1, 1));
            AssertPeak(Deliver(formatter, picture, 10), 3072);
            picture.Embed(Packetize(DmBody(2048), 1, 2));
            AssertPeak(Deliver(formatter, picture, 11), 3072);
            // Picture 12 is skipped and 13..27 have no header (below the loss
            // limit). Picture 28 names ID 2 again, 17 pictures later: the
            // sender may have wrapped through 2 since, so the old pending DM
            // must not be applied.
            picture.CorruptAllCopies();
            for (uint64_t counter = 13; counter < 28; ++counter)
                Deliver(formatter, picture, counter);
            picture.Embed(Packetize({}, 2, 2, true));
            const auto dm = Deliver(formatter, picture, 28);
            Assert::IsFalse(dm.valid || dm.allowSdrFallback);
            Assert::IsTrue(dm.requiresBlackFrame);
            picture.Embed(Packetize(DmBody(1024), 2, 2));
            AssertPeak(Deliver(formatter, picture, 29), 1024);
        }

        // ---- audit 2026-10-16: SDR switch without EOS ---------------------

        TEST_METHOD(SdrSwitchWithoutEosFallsBackAfterRateDerivedWindow)
        {
            const unsigned rates[][2] = { { 24000, 1001 }, { 50, 1 }, { 60000, 1001 } };
            for (const auto& rate : rates)
            {
                for (const bool high8 : { false, true })
                {
                    SenderPicture tunnel(high8, 130, 128, VideoFrameEncoding::R210,
                        rate[0], rate[1]);
                    SenderPicture sdr(high8, 130, 128, VideoFrameEncoding::R210,
                        rate[0], rate[1]);
                    SenderPicture blank(high8, 130, 128, VideoFrameEncoding::R210,
                        rate[0], rate[1]);
                    sdr.FillSdrRamp();
                    std::fill(blank.input.begin(), blank.input.end(), 0);
                    Formatter formatter;
                    formatter.OnVideoState(tunnel.state);
                    tunnel.Embed(Packetize(DmBody()));
                    AssertPeak(Deliver(formatter, tunnel, 1), 3072);
                    const uint64_t window = SdrExitPictures(rate[0], rate[1]);
                    // Blank HDMI during the switch, then the SDR menu: black
                    // for the whole uncertainty window, never SDR pixels.
                    // (requiresBlackFrame is the renderer's contract; once the
                    // cached DM is dropped the formatter output is zero too.)
                    const uint64_t lossLimit = static_cast<uint64_t>(std::floor(
                        static_cast<double>(rate[0]) / rate[1] * 24.0 * 1001.0 / 24000.0 + 0.5));
                    std::vector<BYTE> output;
                    for (uint64_t counter = 2; counter <= window; ++counter)
                    {
                        SenderPicture& picture = counter < 6 ? blank : sdr;
                        const auto dm = Deliver(formatter, picture, counter, &output);
                        Assert::IsTrue(dm.requiresBlackFrame);
                        Assert::IsFalse(dm.allowSdrFallback);
                        if (counter >= 1 + lossLimit)
                            Assert::IsTrue(!dm.valid && AllZero(output));
                    }
                    std::vector<BYTE> exitOutput;
                    const auto exited = Deliver(formatter, sdr, window + 1, &exitOutput);
                    Assert::IsTrue(AllZero(exitOutput)); // renderer uses the raw RGB frame
                    Assert::IsFalse(exited.valid || exited.requiresBlackFrame);
                    Assert::IsTrue(exited.allowSdrFallback);
                    Assert::AreEqual(high8 ? 1U : 2U,
                        static_cast<unsigned>(exited.rgbBitAlignment));
                    Assert::IsTrue(Deliver(formatter, sdr, window + 2).allowSdrFallback);
                    // A later DV title locks again on its first CRC-valid DM.
                    tunnel.Embed(Packetize(DmBody(2048)));
                    std::vector<BYTE> relocked;
                    AssertPeak(Deliver(formatter, tunnel, window + 3, &relocked), 2048);
                    AssertExactSamples(tunnel, relocked);
                }
            }
        }

        TEST_METHOD(BriefLossShorterThanWindowHoldsBlackThenResumesDv)
        {
            SenderPicture picture(false);
            Formatter formatter;
            formatter.OnVideoState(picture.state);
            picture.Embed(Packetize(DmBody()));
            AssertPeak(Deliver(formatter, picture, 1), 3072);
            SenderPicture blank(false);
            std::fill(blank.input.begin(), blank.input.end(), 0);
            const uint64_t window = SdrExitPictures(24000, 1001);
            for (uint64_t counter = 2; counter <= window; ++counter)
            {
                const auto dm = Deliver(formatter, blank, counter);
                Assert::IsTrue(dm.requiresBlackFrame);
                Assert::IsFalse(dm.allowSdrFallback);
            }
            picture.Embed(Packetize(DmBody(2048)));
            std::vector<BYTE> output;
            const auto resumed = Deliver(formatter, picture, window + 1, &output);
            AssertPeak(resumed, 2048);
            Assert::IsFalse(resumed.requiresBlackFrame);
            AssertExactSamples(picture, output);
        }

        TEST_METHOD(RealTunnelWithLostHeadersNeverFallsBackToSdr)
        {
            // Busy and flat-dark tunnel content, every header copy damaged,
            // for three SDR-exit windows: black throughout, never SDR.
            for (const bool flatDark : { false, true })
            {
                for (const bool high8 : { false, true })
                {
                    SenderPicture picture(high8);
                    if (flatDark)
                        picture.SetFlat(64, 2048);
                    picture.Embed(Packetize(DmBody()));
                    Formatter formatter;
                    formatter.OnVideoState(picture.state);
                    AssertPeak(Deliver(formatter, picture, 1), 3072);
                    picture.CorruptAllCopies();
                    const uint64_t window = SdrExitPictures(24000, 1001);
                    for (uint64_t counter = 2; counter < 2 + 3 * window; ++counter)
                    {
                        const auto dm = Deliver(formatter, picture, counter);
                        const std::wstring where = L"flatDark=" + std::to_wstring(flatDark) +
                            L" high8=" + std::to_wstring(high8) + L" counter=" +
                            std::to_wstring(counter) + L" valid=" + std::to_wstring(dm.valid);
                        Assert::IsFalse(dm.allowSdrFallback, where.c_str());
                        Assert::IsTrue(dm.requiresBlackFrame, where.c_str());
                    }
                    picture.Embed(Packetize(DmBody(2048)));
                    AssertPeak(Deliver(formatter, picture, 2 + 3 * window), 2048);
                }
            }
        }

        TEST_METHOD(LossLimitIsDerivedFromFrameRate)
        {
            // ~1.001 s: 24 pictures at 23.976, 50 at 50, 60 at 59.94.
            const unsigned rates[][3] = { { 24000, 1001, 24 }, { 50, 1, 50 }, { 60000, 1001, 60 } };
            for (const auto& rate : rates)
            {
                SenderPicture picture(true, 130, 128, VideoFrameEncoding::R210, rate[0], rate[1]);
                Formatter formatter;
                formatter.OnVideoState(picture.state);
                picture.Embed(Packetize(DmBody()));
                AssertPeak(Deliver(formatter, picture, 1), 3072);
                picture.CorruptAllCopies();
                for (uint64_t counter = 2; counter < 1U + rate[2]; ++counter)
                    AssertPeak(Deliver(formatter, picture, counter), 3072);
                const auto dm = Deliver(formatter, picture, 1U + rate[2]);
                Assert::IsFalse(dm.valid || dm.allowSdrFallback);
            }
        }

        // ---- audit 2026-10-16: metadata (L1 min, L2/L8 trims) -------------

        TEST_METHOD(SceneMinimumIsCarriedWithMaxAndAverage)
        {
            SenderPicture picture(false);
            Formatter formatter;
            formatter.OnVideoState(picture.state);
            picture.Embed(Packetize(DmBody(2048)));
            const auto dm = Deliver(formatter, picture, 1);
            AssertPeak(dm, 2048);
            Assert::AreEqual(62U, static_cast<unsigned>(dm.sceneMinPq));
            Assert::AreEqual(1024U, static_cast<unsigned>(dm.sceneAvgPq));
            Assert::AreEqual(0U, static_cast<unsigned>(dm.trimCount));
        }

        TEST_METHOD(L2AndL8TrimsAreParsedWithoutAffectingDm)
        {
            auto body = DmBody(2048);
            std::vector<uint8_t> l2;
            for (const uint16_t value : { uint16_t(2081), uint16_t(2148), uint16_t(1998),
                uint16_t(2048), uint16_t(2060), uint16_t(2058), uint16_t(0xFFFF) })
                Push16(l2, value);
            AddExtension(body, 2, l2);
            std::vector<uint8_t> l8{ 1 };
            for (const uint16_t value : { uint16_t(2100), uint16_t(2000), uint16_t(2040),
                uint16_t(2050), uint16_t(2070), uint16_t(0xFFFE), uint16_t(2048), uint16_t(2200) })
                Push16(l8, value);
            for (uint8_t i = 0; i < 12; ++i)
                l8.push_back(static_cast<uint8_t>(120 + i));
            AddExtension(body, 8, l8);
            std::vector<uint8_t> shortL8{ 3 };
            for (int i = 0; i < 6; ++i)
                Push16(shortL8, static_cast<uint16_t>(2048 + i));
            AddExtension(body, 8, shortL8);
            AddExtension(body, 2, std::vector<uint8_t>(10, 0x11)); // too short: skipped
            for (const bool high8 : { false, true })
            {
                SenderPicture picture(high8);
                picture.Embed(Packetize(body));
                Formatter formatter;
                formatter.OnVideoState(picture.state);
                const auto dm = Deliver(formatter, picture, 1);
                AssertPeak(dm, 2048);
                Assert::AreEqual(62U, static_cast<unsigned>(dm.sceneMinPq));
                Assert::AreEqual(3U, static_cast<unsigned>(dm.trimCount));
                Assert::IsFalse(dm.trimOverflow);
                const auto& a = dm.trims[0];
                Assert::AreEqual(2U, static_cast<unsigned>(a.level));
                Assert::AreEqual(2081U, static_cast<unsigned>(a.targetMaxPq));
                Assert::AreEqual(2148U, static_cast<unsigned>(a.slope));
                Assert::AreEqual(1998U, static_cast<unsigned>(a.offset));
                Assert::AreEqual(2048U, static_cast<unsigned>(a.power));
                Assert::AreEqual(2060U, static_cast<unsigned>(a.chromaWeight));
                Assert::AreEqual(2058U, static_cast<unsigned>(a.saturationGain));
                Assert::AreEqual(-1, static_cast<int>(a.msWeight));
                const auto& b = dm.trims[1];
                Assert::AreEqual(8U, static_cast<unsigned>(b.level));
                Assert::AreEqual(1U, static_cast<unsigned>(b.targetDisplayIndex));
                Assert::AreEqual(2100U, static_cast<unsigned>(b.slope));
                Assert::AreEqual(2070U, static_cast<unsigned>(b.saturationGain));
                Assert::AreEqual(-2, static_cast<int>(b.msWeight));
                Assert::IsTrue(b.hasMidContrastAndClip && b.hasSaturationVector && b.hasHueVector);
                Assert::AreEqual(2048U, static_cast<unsigned>(b.midContrast));
                Assert::AreEqual(2200U, static_cast<unsigned>(b.clipTrim));
                Assert::AreEqual(120U, static_cast<unsigned>(b.saturationVector[0]));
                Assert::AreEqual(131U, static_cast<unsigned>(b.hueVector[5]));
                const auto& c = dm.trims[2];
                Assert::AreEqual(3U, static_cast<unsigned>(c.targetDisplayIndex));
                Assert::AreEqual(2053U, static_cast<unsigned>(c.msWeight));
                Assert::IsFalse(c.hasMidContrastAndClip || c.hasSaturationVector || c.hasHueVector);
                // Matrices and source range are unchanged by trims.
                Assert::AreEqual(-0.5f, dm.nonlinear[0][1]);
                Assert::AreEqual(3696U, static_cast<unsigned>(dm.sourceMaxPq));
            }
        }

        TEST_METHOD(ExtensionLevelsAreSummarisedOncePerTitle)
        {
            TunnelLogSink sink;
            auto body = DmBody(2048);
            std::vector<uint8_t> l2;
            for (int f = 0; f < 7; ++f)
                Push16(l2, 2048);
            AddExtension(body, 2, l2);
            AddExtension(body, 5, std::vector<uint8_t>(8, 0));
            AddExtension(body, 2, l2);
            SenderPicture picture(false);
            Formatter formatter;
            formatter.OnVideoState(picture.state);
            picture.Embed(Packetize(body));
            for (uint64_t counter = 1; counter <= 5; ++counter)
                AssertPeak(Deliver(formatter, picture, counter), 2048);
            const std::string line = "title DM extension levels (first complete DM): "
                "L1=yes L2=x2 L3=no L4=no L5=yes L6=no L8=no L9=no L10=no L11=no L254=no other=none";
            Assert::AreEqual(size_t(1), CountOccurrences(tunnelLog, line));
            Assert::AreEqual(size_t(1), CountOccurrences(tunnelLog, "first L2 trim (not applied)"));
            // A level that appears later in the title is reported once more.
            auto withL6 = body;
            AddExtension(withL6, 6, std::vector<uint8_t>(8, 0));
            picture.Embed(Packetize(withL6));
            Deliver(formatter, picture, 6);
            Deliver(formatter, picture, 7);
            Assert::AreEqual(size_t(1), CountOccurrences(tunnelLog, "(new level in title)"));
            // EOS ends the title; the next title is summarised again.
            picture.Embed(Packetize({}, 3, 3, true, true));
            Deliver(formatter, picture, 8);
            picture.Embed(Packetize(DmBody(1024)));
            AssertPeak(Deliver(formatter, picture, 9), 1024);
            Assert::AreEqual(size_t(1), CountOccurrences(tunnelLog,
                "(first complete DM): L1=yes L2=no L3=no L4=no L5=no L6=no"));
        }

        TEST_METHOD(MoreTrimsThanCapacityAreFlaggedNotRejected)
        {
            auto body = DmBody(2048);
            for (uint16_t i = 0; i < 9; ++i)
            {
                std::vector<uint8_t> l2;
                for (int f = 0; f < 7; ++f)
                    Push16(l2, static_cast<uint16_t>(1000 + i));
                AddExtension(body, 2, l2);
            }
            SenderPicture picture(false);
            picture.Embed(Packetize(body));
            Formatter formatter;
            formatter.OnVideoState(picture.state);
            const auto dm = Deliver(formatter, picture, 1);
            AssertPeak(dm, 2048);
            Assert::AreEqual(8U, static_cast<unsigned>(dm.trimCount));
            Assert::IsTrue(dm.trimOverflow);
            Assert::AreEqual(1007U, static_cast<unsigned>(dm.trims[7].targetMaxPq));
        }

        // ---- audit 2026-10-16: faithful-unpack coverage -------------------

        TEST_METHOD(AllContainersTimingsAndRastersUnpackBitExact)
        {
            struct Case { uint32_t width, height; unsigned scale, duration;
                VideoFrameEncoding encoding; bool high8; unsigned padding; };
            const Case cases[] = {
                { 1920, 1080, 24000, 1001, VideoFrameEncoding::R210, false, 0 },
                { 1920, 1080, 24, 1, VideoFrameEncoding::R10b, true, 3 },
                { 1920, 1080, 25, 1, VideoFrameEncoding::R10l, false, 1 },
                { 1920, 1080, 30000, 1001, VideoFrameEncoding::R10b, false, 2 },
                { 1920, 1080, 30, 1, VideoFrameEncoding::R10l, true, 3 },
                { 1920, 1080, 50, 1, VideoFrameEncoding::R210, true, 0 },
                { 1920, 1080, 60000, 1001, VideoFrameEncoding::R10b, true, 0 },
                { 1920, 1080, 60, 1, VideoFrameEncoding::R10l, false, 0 },
                { 1280, 720, 60000, 1001, VideoFrameEncoding::R210, false, 0 },
                { 1282, 721, 50, 1, VideoFrameEncoding::R10l, true, 2 }, // tail + odd height
                { 3840, 2160, 24000, 1001, VideoFrameEncoding::R10b, true, 1 },
                { 3840, 2160, 60000, 1001, VideoFrameEncoding::R10l, false, 3 },
            };
            for (const auto& test : cases)
            {
                SenderPicture picture(test.high8, test.width, test.height, test.encoding,
                    test.scale, test.duration, false, test.padding);
                // Five packets: the DM spans several raster lines.
                picture.Embed(Packetize(DmBody(2048, true, 425)));
                for (const auto method : { Formatter::ConversionMethod::SCALAR,
                    Formatter::ConversionMethod::AVX2 })
                {
                    if (method == Formatter::ConversionMethod::AVX2 &&
                        !CpuFeatures::SupportsAvx2Kernels())
                    {
                        Logger::WriteMessage(L"AVX2 qualification skipped: CPU/OS lacks AVX2 support");
                        continue;
                    }
                    Formatter formatter;
                    formatter.SetConversionMethod(method);
                    formatter.OnVideoState(picture.state);
                    std::vector<BYTE> output;
                    const auto dm = Deliver(formatter, picture, 1, &output);
                    AssertPeak(dm, 2048);
                    Assert::AreEqual(test.high8 ? 1U : 2U,
                        static_cast<unsigned>(dm.rgbBitAlignment));
                    AssertExactSamples(picture, output);
                }
            }
        }

        TEST_METHOD(R10ContainersCoverEveryTwelveBitCodeAndTails)
        {
            for (const auto encoding : { VideoFrameEncoding::R10b, VideoFrameEncoding::R10l })
            {
                for (const bool high8 : { false, true })
                {
                    for (const unsigned width : { 128U, 130U, 134U })
                    {
                        SenderPicture picture(high8, width, 100, encoding, 24000, 1001,
                            false, width & 3U);
                        picture.Embed(Packetize(DmBody()));
                        for (const auto method : { Formatter::ConversionMethod::SCALAR,
                            Formatter::ConversionMethod::AVX2 })
                        {
                            if (method == Formatter::ConversionMethod::AVX2 &&
                                !CpuFeatures::SupportsAvx2Kernels())
                                continue;
                            Formatter formatter;
                            formatter.SetConversionMethod(method);
                            formatter.OnVideoState(picture.state);
                            std::vector<BYTE> output;
                            AssertPeak(Deliver(formatter, picture, 1, &output), 3072);
                            AssertExactSamples(picture, output);
                        }
                    }
                }
            }
        }

        TEST_METHOD(MaximumLengthDmReassemblesAcrossOneHundredPackets)
        {
            // 0x2F00 bytes = 100 packets (300 copies), spanning 160 lines at 1080p.
            const size_t unknown = 0x2F00 - 71 - 5 - 11;
            const auto body = DmBody(2048, true, unknown);
            Assert::AreEqual(size_t(0x2F00), body.size());
            const auto packets = Packetize(body);
            Assert::AreEqual(size_t(100), packets.size());
            SenderPicture picture(false, 1920, 1080);
            picture.Embed(packets);
            Formatter formatter;
            formatter.OnVideoState(picture.state);
            std::vector<BYTE> output;
            AssertPeak(Deliver(formatter, picture, 1, &output), 2048);
            AssertExactSamples(picture, output);
            // The last continuation packet damaged in every copy: rejected, old DM kept.
            picture.Embed(Packetize(DmBody(1024, true, unknown)));
            picture.CorruptAllCopies(99);
            AssertPeak(Deliver(formatter, picture, 2), 2048);
        }

        TEST_METHOD(MajorityVoteRecoversDifferentErrorsInAllThreeCopies)
        {
            for (const bool high8 : { false, true })
            {
                SenderPicture picture(high8);
                picture.Embed(Packetize(DmBody(2048, true, 157)));
                // Header and a continuation: each copy has a different bad bit.
                for (const size_t packet : { size_t(0), size_t(1) })
                {
                    picture.CorruptCopy(packet, 0, 17);
                    picture.CorruptCopy(packet, 1, 300);
                    picture.CorruptCopy(packet, 2, 901);
                }
                Formatter formatter;
                formatter.OnVideoState(picture.state);
                AssertPeak(Deliver(formatter, picture, 1), 2048);
            }
            // The same bit wrong in two copies cannot be repaired: no lock.
            SenderPicture picture(false);
            picture.Embed(Packetize(DmBody()));
            picture.CorruptCopy(0, 0, 40);
            picture.CorruptCopy(0, 1, 40);
            picture.CorruptCopy(0, 2, 41);
            Formatter formatter;
            formatter.OnVideoState(picture.state);
            const auto dm = Deliver(formatter, picture, 1);
            Assert::IsFalse(dm.valid);
            Assert::IsTrue(dm.allowSdrFallback);
        }

        TEST_METHOD(UnsupportedSignalsAreRejectedCleanly)
        {
            // Interlaced raster: a valid tunnel never locks DV; output black.
            {
                SenderPicture picture(false, 1920, 1080, VideoFrameEncoding::R210,
                    30000, 1001, true);
                picture.Embed(Packetize(DmBody()));
                Formatter formatter;
                formatter.OnVideoState(picture.state);
                std::vector<BYTE> output;
                const auto dm = Deliver(formatter, picture, 1, &output);
                Assert::IsFalse(dm.valid || dm.requiresBlackFrame);
                Assert::IsTrue(dm.allowSdrFallback);
                Assert::IsTrue(AllZero(output));
            }
            // A card that rescales RGB8 into R10's 64-940 range does not keep
            // the tunnel bytes: CRC never validates, DV never locks.
            for (const auto encoding : { VideoFrameEncoding::R10b, VideoFrameEncoding::R10l })
            {
                SenderPicture picture(true, 130, 128, encoding, 24000, 1001, false, 0, true);
                picture.Embed(Packetize(DmBody()));
                Formatter formatter;
                formatter.OnVideoState(picture.state);
                std::vector<BYTE> output;
                const auto dm = Deliver(formatter, picture, 1, &output);
                Assert::IsFalse(dm.valid || dm.requiresBlackFrame);
                Assert::IsTrue(AllZero(output));
            }
            // Odd width and non-RGB containers are refused at configuration.
            {
                SenderPicture odd(false, 131, 128);
                Formatter formatter;
                Assert::ExpectException<std::runtime_error>([&]() { formatter.OnVideoState(odd.state); });
            }
            for (const auto encoding : { VideoFrameEncoding::V210, VideoFrameEncoding::R12B })
            {
                SenderPicture picture(false);
                picture.state->videoFrameEncoding = encoding;
                Formatter formatter;
                Assert::ExpectException<std::runtime_error>([&]() { formatter.OnVideoState(picture.state); });
            }
        }
    };
}