#include "pch.h"
#include "DepthConstantScan.h"
#include <atomic>

namespace
{
	constexpr int    kScanHotkey          = VK_F11;
	constexpr size_t kChunkBytes          = 1 << 20;
	constexpr size_t kContextBefore       = 0x20;
	constexpr size_t kContextAfter        = 0x40;
	constexpr size_t kMaxHitsPerCandidate = 200;
	constexpr size_t kMaxRegionBytes      = 1ull << 30;
	constexpr float  kFarMinimum          = 100.f;
	constexpr float  kFarMaximum          = 1000000.f;

	struct NearCandidate
	{
		float       value;
		const char* label;
	};

	constexpr std::array<NearCandidate, 4> kNearCandidates {{
		{ 0.0078125f, "2^-7"   },
		{ 0.06f,      "0.06"   },
		{ 0.025f,     "0.025"  },
		{ 0.1f,       "0.1"    },
	}};

	constexpr std::array<ptrdiff_t, 3> kFarOffsets { 4, 8, -4 };

	std::atomic<bool> scanRunning { false };
	bool              hotkeyWasDown = false;

	bool copyGuarded(void* destination, const void* source, size_t bytes)
	{
		__try
		{
			std::memcpy(destination, source, bytes);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool isReadable(DWORD protect)
	{
		if ((protect & PAGE_GUARD) != 0 || (protect & PAGE_NOACCESS) != 0)
		{
			return false;
		}
		const DWORD access = protect & 0xFF;
		return access == PAGE_READONLY || access == PAGE_READWRITE || access == PAGE_WRITECOPY
			|| access == PAGE_EXECUTE_READ || access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
	}

	constexpr std::array<const char*, 7> kGameModules { "halo1.dll", "halo2.dll", "groundhog.dll", "halo3.dll", "halo3odst.dll", "haloreach.dll", "halo4.dll" };

	HMODULE moduleAt(uintptr_t address)
	{
		HMODULE module = nullptr;
		if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module))
		{
			return nullptr;
		}
		return module;
	}

	std::string moduleName(HMODULE module)
	{
		char path[MAX_PATH] {};
		GetModuleFileNameA(module, path, MAX_PATH);
		const char* name = std::strrchr(path, '\\');
		return name != nullptr ? name + 1 : path;
	}

	bool isGameModule(HMODULE module)
	{
		if (module == nullptr)
		{
			return false;
		}
		const std::string name = moduleName(module);
		return std::any_of(kGameModules.begin(), kGameModules.end(), [&name](const char* game) { return _stricmp(name.c_str(), game) == 0; });
	}

	std::string sectionName(HMODULE module, uintptr_t rva)
	{
		const auto* base    = reinterpret_cast<const uint8_t*>(module);
		const auto* dos     = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt      = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		const auto* section = IMAGE_FIRST_SECTION(nt);
		for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
		{
			if (rva >= section[i].VirtualAddress && rva < section[i].VirtualAddress + std::max(section[i].Misc.VirtualSize, section[i].SizeOfRawData))
			{
				return std::string(reinterpret_cast<const char*>(section[i].Name), strnlen(reinterpret_cast<const char*>(section[i].Name), IMAGE_SIZEOF_SHORT_NAME));
			}
		}
		return "?";
	}

	std::string describeAddress(uintptr_t address, const MEMORY_BASIC_INFORMATION& region)
	{
		if (region.Type == MEM_IMAGE)
		{
			if (HMODULE module = moduleAt(address))
			{
				const uintptr_t rva = address - reinterpret_cast<uintptr_t>(module);
				return std::format("{}+0x{:x} [{} {}]", moduleName(module), rva, sectionName(module, rva), (region.Protect & 0xFF) == PAGE_READONLY ? "ro" : "rw");
			}
		}
		return std::format("heap 0x{:x} (region 0x{:x} size 0x{:x})", address, reinterpret_cast<uintptr_t>(region.AllocationBase), region.RegionSize);
	}

	std::string loadedGameName()
	{
		static constexpr std::array<std::pair<const wchar_t*, const char*>, 7> gameModules {{
			{ L"halo1.dll",     "halo1"     },
			{ L"halo2.dll",     "halo2"     },
			{ L"groundhog.dll", "halo2mp"   },
			{ L"halo3.dll",     "halo3"     },
			{ L"halo3odst.dll", "halo3odst" },
			{ L"haloreach.dll", "haloreach" },
			{ L"halo4.dll",     "halo4"     },
		}};
		std::string name;
		for (const auto& [module, label] : gameModules)
		{
			if (GetModuleHandleW(module) != nullptr)
			{
				name += name.empty() ? label : std::string("+") + label;
			}
		}
		return name.empty() ? "nogame" : name;
	}

	std::string contextFloats(const std::vector<uint8_t>& buffer, size_t offset)
	{
		std::string text;
		const size_t start = offset >= kContextBefore ? offset - kContextBefore : 0;
		const size_t end   = std::min(buffer.size(), offset + kContextAfter);
		for (size_t i = start; i + sizeof(float) <= end; i += sizeof(float))
		{
			float value;
			std::memcpy(&value, buffer.data() + i, sizeof(value));
			text += std::format("{}{:+d}:{:.6g} ", i == offset ? "*" : "", (ptrdiff_t)i - (ptrdiff_t)offset, value);
		}
		return text;
	}

	void scan()
	{
		wchar_t tempPath[MAX_PATH] {};
		GetTempPathW(MAX_PATH, tempPath);
		SYSTEMTIME now {};
		GetLocalTime(&now);
		const std::string game = loadedGameName();
		const auto folder = std::filesystem::path(tempPath) / "HCMDepthDumps";
		std::error_code error;
		std::filesystem::create_directories(folder, error);
		const auto outputPath = folder / std::format("{:04}{:02}{:02}-{:02}{:02}{:02}_{}_constscan.txt", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, game);
		std::ofstream output(outputPath);
		output << "game=" << game << "\n";
		output << "candidate near value with a float in [" << kFarMinimum << ", " << kFarMaximum << "] at +4, +8 or -4 bytes\n";

		std::array<std::array<size_t, kNearCandidates.size()>, 2> hitCountsByPass {};
		std::vector<uint8_t> buffer;
		buffer.reserve(kChunkBytes + kContextAfter);
		const uintptr_t bufferAddress = reinterpret_cast<uintptr_t>(buffer.data());
		size_t scannedBytes = 0;
		for (size_t pass = 0; pass < hitCountsByPass.size(); ++pass)
		{
			const bool gameModulePass = pass == 0;
			auto& hitCounts = hitCountsByPass[pass];
			output << (gameModulePass ? "--- game module (uncapped)\n" : "--- everything else\n");
			uintptr_t address = 0;
			MEMORY_BASIC_INFORMATION region {};
			while (VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) == sizeof(region))
			{
				const uintptr_t regionBase = reinterpret_cast<uintptr_t>(region.BaseAddress);
				address = regionBase + region.RegionSize;
				const bool containsScanBuffer = bufferAddress >= regionBase && bufferAddress < regionBase + region.RegionSize;
				if (region.State != MEM_COMMIT || region.Type == MEM_MAPPED || !isReadable(region.Protect) || region.RegionSize > kMaxRegionBytes || containsScanBuffer)
				{
					continue;
				}
				if (gameModulePass != (region.Type == MEM_IMAGE && isGameModule(moduleAt(regionBase))))
				{
					continue;
				}
				for (size_t chunkOffset = 0; chunkOffset < region.RegionSize; chunkOffset += kChunkBytes)
				{
					const size_t chunkBytes = std::min(kChunkBytes + kContextAfter, region.RegionSize - chunkOffset);
					buffer.resize(chunkBytes);
					if (!copyGuarded(buffer.data(), reinterpret_cast<const void*>(regionBase + chunkOffset), chunkBytes))
					{
						break;
					}
					scannedBytes += chunkBytes;
					const size_t scanEnd = std::min(kChunkBytes, chunkBytes);
					for (size_t offset = 0; offset + sizeof(float) <= scanEnd; offset += sizeof(float))
					{
						float value;
						std::memcpy(&value, buffer.data() + offset, sizeof(value));
						for (size_t candidate = 0; candidate < kNearCandidates.size(); ++candidate)
						{
							if (value != kNearCandidates[candidate].value || (!gameModulePass && hitCounts[candidate] >= kMaxHitsPerCandidate))
							{
								continue;
							}
							for (const ptrdiff_t farOffset : kFarOffsets)
							{
								const ptrdiff_t farPosition = (ptrdiff_t)offset + farOffset;
								if (farPosition < 0 || (size_t)farPosition + sizeof(float) > buffer.size())
								{
									continue;
								}
								float farValue;
								std::memcpy(&farValue, buffer.data() + farPosition, sizeof(farValue));
								if (farValue < kFarMinimum || farValue > kFarMaximum)
								{
									continue;
								}
								const uintptr_t hitAddress = regionBase + chunkOffset + offset;
								output << std::format("near={} far={:.6g}@{:+d} at {}\n    {}\n",
									kNearCandidates[candidate].label, farValue, farOffset, describeAddress(hitAddress, region), contextFloats(buffer, offset));
								++hitCounts[candidate];
								break;
							}
						}
					}
				}
			}
		}
		output << std::format("scanned {} MB\n", scannedBytes / (1024 * 1024));
		for (size_t candidate = 0; candidate < kNearCandidates.size(); ++candidate)
		{
			output << std::format("hits near={}: game module {}, elsewhere {}{}\n", kNearCandidates[candidate].label, hitCountsByPass[0][candidate], hitCountsByPass[1][candidate],
				hitCountsByPass[1][candidate] >= kMaxHitsPerCandidate ? " (capped)" : "");
		}
		PLOG_INFO << "depth constant scan written to " << outputPath.string();
		scanRunning = false;
	}
}

namespace DepthConstantScan
{
	void poll()
	{
		DWORD foregroundProcess = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
		const bool down = foregroundProcess == GetCurrentProcessId() && (GetAsyncKeyState(kScanHotkey) & 0x8000) != 0;
		const bool pressed = down && !hotkeyWasDown;
		hotkeyWasDown = down;
		if (pressed && !scanRunning.exchange(true))
		{
			PLOG_INFO << "depth constant scan started";
			std::thread(scan).detach();
		}
	}
}
