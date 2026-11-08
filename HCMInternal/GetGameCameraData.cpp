#include "pch.h"
#include <fstream>
#include <mutex>
#include <set>
#include <vector>
#include "GetGameCameraData.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"
#include "DynamicStructFactory.h"
#include "IMCCStateHook.h"

class GetGameCameraData::GetGameCameraDataImpl
{
private:

	// data
	enum class cameraDataFields { position, velocity, FOV, lookDirForward, lookDirUp};
	std::shared_ptr<DynamicStruct<cameraDataFields>> cameraDataStruct;

	std::shared_ptr<MultilevelPointer> cameraDataPointer;

	const std::wstring moduleName;
	const std::string dumpTag;
	const GameState::Value gameValue;


	bool cacheValid = false;
	std::unique_ptr<GameCameraData> cachedCameraData;

	//callbacks
	ScopedCallback< eventpp::CallbackList<void(const MCCState&)>> MCCStateChangedCallback;

	// event to fire on mcc state change (just clear the caches)
	void onGameStateChange(const MCCState&)
	{
		PLOG_INFO << "clearing GetCameraDataImpl caches";
		cacheValid = false;
	}

public:
	GetGameCameraDataImpl(GameState game, IDIContainer& dicon)
		:
		moduleName(game.toModuleName()),
		dumpTag(game.toString()),
		gameValue(game),
		MCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& state) { onGameStateChange(state); })
	{
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		cameraDataStruct = DynamicStructFactory::make<cameraDataFields>(ptr, game);
		cameraDataPointer = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(cameraDataPointer), game);
	}


	GameCameraData getGameCameraData()
	{
		if (cacheValid == false)
		{
			uintptr_t cameraDataPointerResolved;
			if (!cameraDataPointer->resolve(&cameraDataPointerResolved)) throw HCMRuntimeException(std::format("Failed to resolve cameraDataPointer: ", MultilevelPointer::GetLastError()));

			LOG_ONCE_CAPTURE(PLOG_DEBUG << "cameraDataPointerResolved: " << std::hex << rs, rs = cameraDataPointerResolved);

			cameraDataStruct->currentBaseAddress = cameraDataPointerResolved;

			/**/static std::mutex dumpMutex;
			static std::set<GameState::Value> dumpedGames;
			bool shouldDump = false;
			{
				std::lock_guard lock(dumpMutex);
				if (!dumpedGames.contains(this->gameValue))
				{
					dumpedGames.insert(this->gameValue);
					shouldDump = true;
				}
			}
			if (shouldDump)
			{
				struct DumpRegion { uint64_t base; std::vector<uint8_t> data; };
				std::vector<DumpRegion> regions;

				auto dumpRange = [&](uintptr_t start, size_t size, const char* tag)
				{
					std::vector<uint8_t> buffer(size, 0);
					for (size_t off = 0; off < size; off += 0x1000)
					{
						const uintptr_t addr = start + off;
						MEMORY_BASIC_INFORMATION mbi{};
						if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
						{
							continue;
						}
						const DWORD protect = mbi.Protect & 0xFF;
						const bool readable = protect == PAGE_READONLY || protect == PAGE_READWRITE || protect == PAGE_WRITECOPY
							|| protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
						if (!readable || addr < (uintptr_t)mbi.BaseAddress || addr >= (uintptr_t)mbi.BaseAddress + mbi.RegionSize)
						{
							continue;
						}
						size_t chunk = std::min<size_t>(0x1000, (uintptr_t)mbi.BaseAddress + mbi.RegionSize - addr);
						chunk = std::min<size_t>(chunk, size - off);
						memcpy(buffer.data() + off, (const void*)addr, chunk);
					}
					regions.push_back({ (uint64_t)start, std::move(buffer) });
					PLOG_INFO << "occlusion probe: camdump region " << tag << " base=0x" << std::hex << start << " size=0x" << size;
				};

				// region 1: camera struct neighborhood
				dumpRange(cameraDataPointerResolved - 0x20000, 0x40000, "camera");

				// region 2: game module .data section (render globals)
				if (HMODULE gameModule = GetModuleHandleW(moduleName.c_str()))
				{
					auto* dos = (IMAGE_DOS_HEADER*)gameModule;
					auto* nt = (IMAGE_NT_HEADERS64*)((uint8_t*)gameModule + dos->e_lfanew);
					auto* sec = IMAGE_FIRST_SECTION(nt);
					for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
					{
						if (strncmp((const char*)sec[i].Name, ".data", 5) == 0)
						{
							dumpRange((uintptr_t)gameModule + sec[i].VirtualAddress, sec[i].Misc.VirtualSize, ".data");
						}
					}
				}

				// region 3: pointer chase from camera neighborhood - dump 8KB at each heap-ish pointer target
				{
					const uintptr_t camStart = cameraDataPointerResolved - 0x8000;
					const size_t camSize = 0x10000;
					std::vector<uint8_t> camBuf(camSize);
					for (size_t off = 0; off < camSize; off += 0x1000)
					{
						const uintptr_t addr = camStart + off;
						MEMORY_BASIC_INFORMATION mbi{};
						if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
						{
							continue;
						}
						const DWORD protect = mbi.Protect & 0xFF;
						if (protect != PAGE_READONLY && protect != PAGE_READWRITE && protect != PAGE_WRITECOPY)
						{
							continue;
						}
						const size_t chunk = std::min<size_t>(0x1000, (uintptr_t)mbi.BaseAddress + mbi.RegionSize - addr);
						memcpy(camBuf.data() + off, (const void*)addr, std::min(chunk, camSize - off));
					}
					int chased = 0;
					for (size_t off = 0; off + 8 <= camSize && chased < 96; off += 8)
					{
						const uint64_t ptr = *(const uint64_t*)(camBuf.data() + off);
						if (ptr < 0x10000 || ptr > 0x7FFFFFFFFFFFull)
						{
							continue;
						}
						MEMORY_BASIC_INFORMATION mbi{};
						if (VirtualQuery((LPCVOID)ptr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
						{
							continue;
						}
						const DWORD protect = mbi.Protect & 0xFF;
						if (!(protect == PAGE_READONLY || protect == PAGE_READWRITE || protect == PAGE_WRITECOPY))
						{
							continue;
						}
						dumpRange((uintptr_t)ptr, 0x2000, "chase");
						++chased;
					}
					PLOG_INFO << "occlusion probe: camdump chased=" << chased;
				}

				char tempPath[MAX_PATH]{};
				GetTempPathA(MAX_PATH, tempPath);
				const std::string outPath = std::string(tempPath) + "hcm_camdump_" + dumpTag + ".bin";
				std::ofstream out(outPath, std::ios::binary);
				const uint32_t regionCount = (uint32_t)regions.size();
				out.write((const char*)&regionCount, sizeof(regionCount));
				for (const auto& region : regions)
				{
					const uint64_t rbase = region.base;
					const uint64_t rsize = region.data.size();
					out.write((const char*)&rbase, sizeof(rbase));
					out.write((const char*)&rsize, sizeof(rsize));
					out.write((const char*)region.data.data(), region.data.size());
				}
				PLOG_INFO << "occlusion probe: camdump written " << outPath << " regions=" << regionCount;
			}/**/

			cachedCameraData = std::make_unique<GameCameraData>
			(
				cameraDataStruct->field<SimpleMath::Vector3>(cameraDataFields::position),
				cameraDataStruct->field<SimpleMath::Vector3>(cameraDataFields::velocity),
				cameraDataStruct->field<float>(cameraDataFields::FOV),
				cameraDataStruct->field<SimpleMath::Vector3>(cameraDataFields::lookDirForward),
				cameraDataStruct->field<SimpleMath::Vector3>(cameraDataFields::lookDirUp)
			);

			cacheValid = true;
		}

		if (IsBadReadPtr((void*)cachedCameraData->position, 12)) throw HCMRuntimeException(std::format("Bad read at {}", (uintptr_t)cachedCameraData->position));

		return *cachedCameraData.get();
	}
};


GetGameCameraData::GetGameCameraData(GameState gameImpl, IDIContainer& dicon)
{
	pimpl = std::make_unique< GetGameCameraDataImpl>(gameImpl, dicon);
}
GetGameCameraData::~GetGameCameraData()
{
	PLOG_DEBUG << "~" << getName();
}

GameCameraData GetGameCameraData::getGameCameraData() { return pimpl->getGameCameraData(); }