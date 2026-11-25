// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// repair.grain_split: CPU wire layout and Acquire reader for kP10gGrainLevel (ProductId 35, 15b A4b).
// Layout matches GrainLevelGpu.h RGBA32F texel: 4 floats per block (N_b, g, f'^2 N_b, rho1) = 16 bytes/block.
// Header-only, pure C++14, no Windows or GPU headers.

#include <VP-V/core/Products.h>
#include <VP-V/core/SkeletonAcquire.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace GrainSplit
{
	namespace LevelCpu
	{
		static_assert(VpSkeleton::kP10gGrainLevel == 35, "kP10gGrainLevel fixed ProductId 35");

		inline double CodeToEightBitOf(int sourceBits)
		{
			const int bits = (sourceBits >= 1 && sourceBits <= 16) ? sourceBits : 10;
			return 255.0 / static_cast<double>((1u << bits) - 1u);
		}

		struct Meta
		{
			int sourceBits = 10;
			int blockPx = 16;
			double CodeToEightBit() const { return CodeToEightBitOf(sourceBits); }
		};

		struct BlockLevel
		{
			float nb = 0.0f;         // x: block grain level N_b (codes^2 of D)
			float gate = 0.0f;       // y: temporal coherence gate g
			float retained = 0.0f;   // z: retained grain share f'^2 * N_b
			float rho1 = -1.0f;      // w: lag-1 temporal correlation (-1 = unmeasured)
		};
		static_assert(sizeof(BlockLevel) == 4 * sizeof(float), "BlockLevel matches 4-float RGBA32F texel");

		inline std::vector<uint8_t> Pack(const std::vector<BlockLevel>& blocks)
		{
			std::vector<uint8_t> bytes(blocks.size() * 4u * sizeof(float));
			for (size_t i = 0; i < blocks.size(); ++i)
			{
				const float v[4] = { blocks[i].nb, blocks[i].gate, blocks[i].retained, blocks[i].rho1 };
				std::memcpy(bytes.data() + i * 4u * sizeof(float), v, sizeof(v));
			}
			return bytes;
		}

		inline bool Unpack(const uint8_t* bytes, size_t len, size_t nb,
			std::vector<double>& outNb, std::vector<double>& outRetained,
			std::vector<double>& outGate, std::vector<double>& outRho1, std::string& why)
		{
			const size_t expected = nb * 4u * sizeof(float);
			if (bytes == nullptr || nb == 0 || len != expected)
			{
				why = "kP10gGrainLevel CPU payload size mismatch (got " + std::to_string(len)
					+ " B, expected " + std::to_string(expected) + " B for " + std::to_string(nb) + " blocks)";
				return false;
			}
			std::vector<double> tmpNb(nb), tmpRet(nb), tmpGate(nb), tmpRho(nb);
			for (size_t i = 0; i < nb; ++i)
			{
				float v[4] = {};
				std::memcpy(v, bytes + i * 4u * sizeof(float), sizeof(v));
				if (!std::isfinite(v[0]) || v[0] < 0.0f || !std::isfinite(v[1])
					|| !std::isfinite(v[2]) || v[2] < 0.0f || !std::isfinite(v[3]))
				{
					why = "kP10gGrainLevel contains non-finite or negative block level values";
					return false;
				}
				tmpNb[i] = static_cast<double>(v[0]);
				tmpGate[i] = static_cast<double>(v[1]);
				tmpRet[i] = static_cast<double>(v[2]);
				tmpRho[i] = static_cast<double>(v[3]);
			}
			outNb = std::move(tmpNb);
			outRetained = std::move(tmpRet);
			outGate = std::move(tmpGate);
			outRho1 = std::move(tmpRho);
			return true;
		}

		inline bool Acquire(VpSkeleton::SkeletonMemo* memo, uint64_t seq, size_t nb,
			std::vector<double>& outNb, std::vector<double>& outRetained,
			std::vector<double>& outGate, std::vector<double>& outRho1, std::string& why)
		{
			if (memo == nullptr)
			{
				why = "no skeleton memo bound for kP10gGrainLevel";
				return false;
			}
			if (seq == 0u)
			{
				why = "seq == 0 (unkeyed)";
				return false;
			}
			const VpSkeleton::AcquireResult res = memo->Acquire(
				VpSkeleton::kP10gGrainLevel, seq, VpSkeleton::AcquireForm::Cpu);
			if (!res.Ok() || res.cpu == nullptr)
			{
				const std::string detail = memo->LastCause(VpSkeleton::kP10gGrainLevel);
				why = std::string("kP10gGrainLevel unavailable (") + VpSkeleton::Token(res.cause)
					+ (detail.empty() ? ")" : ": " + detail + ")");
				return false;
			}
			return Unpack(res.cpu, res.cpuBytes, nb, outNb, outRetained, outGate, outRho1, why);
		}
	}
}
