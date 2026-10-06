#include "TextureStreaming.h"

#include "Menu.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "Utils/MathUtils.h"
#include "Utils/UI.h"

#include <imgui_stdlib.h>

#include <dxgi1_4.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "pdh.lib")

#define I18N_KEY_PREFIX "feature.texture_streaming."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	TextureStreaming::Settings,
	Enabled,
	LoadAtRememberedSize,
	BudgetMode,
	BudgetStartPercent,
	Refill,
	RefillGapPercent,
	RamCacheMB,
	SafetyFactor,
	MinEdge,
	ScanBudgetMs,
	DetailedLog,
	Exclude)

namespace
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	TextureStreaming::Settings& Cfg()
	{
		return globals::features::textureStreaming.settings;
	}

	namespace GpuMemory
	{
		PDH_HQUERY g_query = nullptr;
		PDH_HCOUNTER g_dedicated = nullptr;
		PDH_HCOUNTER g_shared = nullptr;
		bool g_failed = false;
		wchar_t g_prefix[32]{};

		bool Init() noexcept
		{
			if (g_query || g_failed)
				return g_query != nullptr;
			swprintf_s(g_prefix, L"pid_%lu_", GetCurrentProcessId());
			if (PdhOpenQueryW(nullptr, 0, &g_query) != ERROR_SUCCESS ||
				PdhAddEnglishCounterW(g_query, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &g_dedicated) != ERROR_SUCCESS ||
				PdhAddEnglishCounterW(g_query, L"\\GPU Process Memory(*)\\Shared Usage", 0, &g_shared) != ERROR_SUCCESS) {
				if (g_query)
					PdhCloseQuery(g_query);
				g_query = nullptr;
				g_failed = true;
				return false;
			}
			PdhCollectQueryData(g_query);
			return true;
		}

		unsigned long long Sum(PDH_HCOUNTER a_counter) noexcept
		{
			DWORD size = 0, count = 0;
			if (PdhGetFormattedCounterArrayW(a_counter, PDH_FMT_LARGE, &size, &count, nullptr) != PDH_MORE_DATA || size == 0)
				return 0;
			std::vector<unsigned char> buf(size);
			auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
			if (PdhGetFormattedCounterArrayW(a_counter, PDH_FMT_LARGE, &size, &count, items) != ERROR_SUCCESS)
				return 0;
			const auto len = wcslen(g_prefix);
			unsigned long long sum = 0;
			for (DWORD i = 0; i < count; ++i) {
				if (items[i].szName && wcsncmp(items[i].szName, g_prefix, len) == 0 && items[i].FmtValue.CStatus == ERROR_SUCCESS)
					sum += static_cast<unsigned long long>(items[i].FmtValue.largeValue);
			}
			return sum;
		}

		bool Query(unsigned long long& a_dedicated, unsigned long long& a_shared) noexcept
		{
			if (!Init() || PdhCollectQueryData(g_query) != ERROR_SUCCESS)
				return false;
			a_dedicated = Sum(g_dedicated);
			a_shared = Sum(g_shared);
			return a_dedicated > 0;
		}
	}

	struct FormatInfo
	{
		bool bc = false;
		std::uint32_t bytes = 0;

		bool operator==(const FormatInfo&) const = default;
	};

	FormatInfo InfoOf(std::uint32_t a_dxgi) noexcept
	{
		switch (a_dxgi) {
		case 70:
		case 71:
		case 72:
		case 79:
		case 80:
		case 81:
			return { true, 8 };
		case 73:
		case 74:
		case 75:
		case 76:
		case 77:
		case 78:
		case 82:
		case 83:
		case 84:
		case 94:
		case 95:
		case 96:
		case 97:
		case 98:
		case 99:
			return { true, 16 };
		case 23:
		case 24:
		case 25:
		case 27:
		case 28:
		case 29:
		case 30:
		case 31:
		case 32:
		case 87:
		case 88:
		case 90:
		case 91:
		case 92:
		case 93:
			return { false, 4 };
		case 9:
		case 10:
		case 11:
		case 12:
		case 13:
		case 14:
			return { false, 8 };
		case 48:
		case 49:
		case 50:
		case 51:
		case 52:
		case 53:
		case 54:
		case 55:
		case 56:
		case 57:
		case 58:
		case 85:
		case 86:
		case 115:
			return { false, 2 };
		case 60:
		case 61:
		case 62:
		case 63:
		case 64:
		case 65:
			return { false, 1 };
		default:
			return {};
		}
	}

	std::uint64_t MipBytes(FormatInfo a_fi, std::uint32_t a_w, std::uint32_t a_h) noexcept
	{
		if (a_fi.bc)
			return std::uint64_t(std::max(1u, (a_w + 3) / 4)) * std::max(1u, (a_h + 3) / 4) * a_fi.bytes;
		return std::uint64_t(a_w) * a_h * a_fi.bytes;
	}

	std::uint32_t RowPitch(FormatInfo a_fi, std::uint32_t a_w) noexcept
	{
		return a_fi.bc ? std::max(1u, (a_w + 3) / 4) * a_fi.bytes : a_w * a_fi.bytes;
	}

	std::uint64_t ChainBytes(FormatInfo a_fi, std::uint32_t a_w, std::uint32_t a_h, std::uint32_t a_mips) noexcept
	{
		std::uint64_t sum = 0;
		for (std::uint32_t i = 0; i < a_mips; ++i)
			sum += MipBytes(a_fi, std::max(1u, a_w >> i), std::max(1u, a_h >> i));
		return sum;
	}

	constexpr std::uint32_t FourCC(char a, char b, char c, char d)
	{
		return std::uint32_t(std::uint8_t(a)) | (std::uint32_t(std::uint8_t(b)) << 8) | (std::uint32_t(std::uint8_t(c)) << 16) | (std::uint32_t(std::uint8_t(d)) << 24);
	}

	struct DDSPixelFormat
	{
		std::uint32_t size, flags, fourCC, rgbBitCount, rMask, gMask, bMask, aMask;
	};

	struct DDSHeader
	{
		std::uint32_t size, flags, height, width, pitch, depth, mipCount;
		std::uint32_t reserved1[11];
		DDSPixelFormat pf;
		std::uint32_t caps, caps2, caps3, caps4, reserved2;
	};
	static_assert(sizeof(DDSHeader) == 124);

	struct DDSHeaderDX10
	{
		std::uint32_t dxgiFormat, resourceDimension, miscFlag, arraySize, miscFlags2;
	};

	struct FileInfo
	{
		std::uint32_t width = 0, height = 0, mips = 0;
		FormatInfo fi;
		std::uint32_t dataOffset = 0;
	};

	std::string ReadHeader(RE::BSResourceNiBinaryStream& a_stream, FileInfo& a_out)
	{
		if (!a_stream.good())
			return "file not found";
		std::uint32_t magic = 0;
		DDSHeader h{};
		if (!a_stream.read(&magic, 1) || magic != FourCC('D', 'D', 'S', ' ') || !a_stream.read(&h, 1) || h.size != 124)
			return "not a DDS";
		a_out.width = h.width;
		a_out.height = h.height;
		a_out.mips = std::max(1u, h.mipCount);
		a_out.dataOffset = 4 + 124;
		if (h.caps2 & 0x200)
			return "cubemap";
		if (h.depth > 1 && (h.caps2 & 0x200000))
			return "volume texture";
		const auto& pf = h.pf;
		if (pf.flags & 0x4) {
			switch (pf.fourCC) {
			case FourCC('D', 'X', 'T', '1'):
			case FourCC('A', 'T', 'I', '1'):
			case FourCC('B', 'C', '4', 'U'):
			case FourCC('B', 'C', '4', 'S'):
				a_out.fi = { true, 8 };
				break;
			case FourCC('D', 'X', 'T', '2'):
			case FourCC('D', 'X', 'T', '3'):
			case FourCC('D', 'X', 'T', '4'):
			case FourCC('D', 'X', 'T', '5'):
			case FourCC('A', 'T', 'I', '2'):
			case FourCC('B', 'C', '5', 'U'):
			case FourCC('B', 'C', '5', 'S'):
				a_out.fi = { true, 16 };
				break;
			case FourCC('D', 'X', '1', '0'):
				{
					DDSHeaderDX10 dx{};
					if (!a_stream.read(&dx, 1))
						return "DX10 header missing";
					a_out.dataOffset += sizeof(DDSHeaderDX10);
					if (dx.resourceDimension != 3 || dx.arraySize > 1 || (dx.miscFlag & 0x4))
						return "not a plain 2D image";
					a_out.fi = InfoOf(dx.dxgiFormat);
					break;
				}
			default:
				return std::format("FourCC 0x{:08X} unknown", pf.fourCC);
			}
		} else if (pf.flags & (0x40 | 0x20000 | 0x2)) {
			a_out.fi = { false, pf.rgbBitCount / 8 };
		} else {
			return "pixel format unknown";
		}
		if (a_out.fi.bytes == 0)
			return "format not supported";
		return {};
	}

	enum class Probe : std::uint8_t
	{
		kNone,
		kPending,
		kOk,
		kBad
	};

	struct TexState
	{
		RE::NiPointer<RE::NiSourceTexture> hold;
		ID3D11Resource* res = nullptr;
		std::string path;
		const char* name = nullptr;
		std::uint32_t fullW = 0, fullH = 0, fullMips = 0;
		std::uint32_t curW = 0, curH = 0, curMips = 0;
		std::uint32_t format = 0;
		FormatInfo fi;
		Probe probe = Probe::kNone;
		bool eligible = false;
		bool busy = false;
		std::uint32_t passId = 0;
		float passNeed = 0;
		std::uint32_t lowPasses = 0;
		std::uint32_t lowTarget = 0;
		Clock::time_point lastSeen{};
		Clock::time_point lastReload{};

		std::uint32_t FullEdge() const noexcept { return std::max(fullW, fullH); }
		std::uint32_t CurEdge() const noexcept { return std::max(curW, curH); }
		bool Reduced() const noexcept { return CurEdge() < FullEdge(); }
	};

	std::unordered_map<RE::BSGraphics::Texture*, TexState> g_tex;

	ID3D11Device* g_device = nullptr;
	ID3D11DeviceContext* g_context = nullptr;

	winrt::com_ptr<IDXGIAdapter3> g_adapter;
	std::atomic<std::uint64_t> g_vramUsage{ 0 }, g_vramBudget{ 0 };
	std::atomic<float> g_vramPct{ -1.0f };
	std::uint32_t g_reducedCount = 0;
	std::uint64_t g_savedBytes = 0;
	std::atomic<std::uint64_t> g_procDedicated{ 0 }, g_procShared{ 0 };
	std::atomic<std::uint64_t> g_dxgiUsage{ 0 };
	constexpr std::uint64_t kSharedPressure = 256ull << 20;
	Clock::time_point g_lastPressure{};
	Clock::time_point g_lastLoad{};
	constexpr auto kRefillCalm = 30s;
	std::atomic<Clock::rep> g_lastPressureTicks{ 0 }, g_lastLoadTicks{ 0 };
	constexpr auto kLoadWindow = 60s;
	constexpr auto kPressureMemory = 10min;
	std::uint64_t g_inflightUp = 0;
	constexpr std::uint64_t kRefillPerPass = 512ull << 20;

	void InitAdapter()
	{
		g_adapter = globals::menu ? globals::menu->GetDXGIAdapter3() : nullptr;
		if (!g_adapter)
			logger::warn("[Texture Streaming] No IDXGIAdapter3; budget mode unavailable, always downscaling by distance");
	}

	void UpdateVram()
	{
		if (!g_adapter)
			return;
		DXGI_QUERY_VIDEO_MEMORY_INFO info{};
		if (SUCCEEDED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) && info.Budget > 0) {
			const auto usage = std::max<std::uint64_t>(info.CurrentUsage, g_procDedicated.load(std::memory_order_relaxed));
			g_dxgiUsage.store(info.CurrentUsage, std::memory_order_relaxed);
			g_vramUsage.store(usage, std::memory_order_relaxed);
			g_vramBudget.store(info.Budget, std::memory_order_relaxed);
			g_vramPct.store(100.0f * static_cast<float>(usage) / static_cast<float>(info.Budget), std::memory_order_relaxed);
		}
	}

	std::atomic<std::int64_t> g_lastVramQuery{ 0 };

	void RefreshVramThrottled()
	{
		const auto now = Clock::now().time_since_epoch().count();
		auto last = g_lastVramQuery.load(std::memory_order_relaxed);
		if (now - last >= std::chrono::duration_cast<Clock::duration>(100ms).count() && g_lastVramQuery.compare_exchange_strong(last, now))
			UpdateVram();
	}

	bool Pressure() noexcept
	{
		const auto& cfg = Cfg();
		if (!cfg.BudgetMode)
			return true;
		const float pct = g_vramPct.load(std::memory_order_relaxed);
		const bool overflowing = g_procShared.load(std::memory_order_relaxed) >= kSharedPressure && pct >= cfg.BudgetStartPercent - cfg.RefillGapPercent / 2.0f;
		return pct < 0.0f || pct >= cfg.BudgetStartPercent || overflowing;
	}

	bool LoadPressure() noexcept
	{
		if (Pressure())
			return true;
		const auto now = Clock::now().time_since_epoch().count();
		const auto load = g_lastLoadTicks.load(std::memory_order_relaxed), pressure = g_lastPressureTicks.load(std::memory_order_relaxed);
		return load != 0 && pressure != 0 && now - load < Clock::duration(kLoadWindow).count() &&
		       now - pressure < Clock::duration(kPressureMemory).count();
	}

	constexpr float kUpMargin = 1.25f;
	constexpr float kUpMarginPressure = 2.0f;
	constexpr auto kCooldown = 30s;
	constexpr int kLowPasses = 3;

	float UpMargin() noexcept
	{
		return Pressure() ? kUpMarginPressure : kUpMargin;
	}

	std::uint32_t UpTarget(std::uint32_t a_want, std::uint32_t a_full) noexcept
	{
		return Pressure() ? std::min(a_full, a_want) : std::min(a_full, a_want * 2);
	}

	struct Counters
	{
		std::uint32_t downs = 0, ups = 0, upFails = 0, probesBad = 0;
		std::uint32_t pingPong = 0;
		std::uint32_t refills = 0;
		double upMs = 0, upMB = 0;
		std::uint32_t passes = 0, passFrames = 0, passNodes = 0;
		double passMs = 0;
	} g_stats;
	std::uint32_t g_failLogged = 0;

	struct DeferredRelease
	{
		Clock::time_point time;
		IUnknown* obj;
		std::uint64_t bytes;
	};
	std::deque<DeferredRelease> g_deferred;
	constexpr std::uint64_t kReleaseBytesPerCall = 64ull << 20;

	void DeferRelease(IUnknown* a_obj, std::uint64_t a_bytes = 0)
	{
		if (a_obj)
			g_deferred.push_back({ Clock::now(), a_obj, a_bytes });
	}

	void ProcessDeferred()
	{
		const auto now = Clock::now();
		std::uint64_t released = 0;
		while (!g_deferred.empty() && now - g_deferred.front().time >= 500ms) {
			const auto& e = g_deferred.front();
			if (released > 0 && released + e.bytes > kReleaseBytesPerCall)
				break;
			released += e.bytes;
			e.obj->Release();
			g_deferred.pop_front();
		}
	}

	void Swap(RE::BSGraphics::Texture* a_r, ID3D11Texture2D* a_tex, ID3D11ShaderResourceView* a_srv, std::uint64_t a_oldBytes)
	{
		DeferRelease(a_r->texture, a_oldBytes);
		DeferRelease(a_r->resourceView);
		a_r->texture = a_tex;
		a_r->resourceView = a_srv;
	}

	struct Job
	{
		bool reload = false;
		RE::BSGraphics::Texture* r = nullptr;
		ID3D11Resource* expectRes = nullptr;
		std::string path;
		std::uint32_t fullW = 0, fullH = 0, fullMips = 0, format = 0;
		FormatInfo fi;
		std::uint32_t skip = 0;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		std::uint64_t extraBytes = 0;
	};

	struct Result
	{
		Job job;
		std::string error;
		FileInfo file;
		ID3D11Texture2D* tex = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
		double ms = 0;
		double mb = 0;
	};

	std::mutex g_qLock;
	std::condition_variable g_qCv;
	std::deque<Job> g_jobs;
	std::vector<Result> g_results;
	std::atomic<bool> g_workerStarted{ false };

	bool SkipBytes(RE::BSResourceNiBinaryStream& a_s, std::uint64_t a_bytes)
	{
		const auto pos = a_s.tell();
		a_s.seek(static_cast<std::int32_t>(a_bytes));
		return a_s.tell() == pos + a_bytes;
	}

	bool DiscardBytes(RE::BSResourceNiBinaryStream& a_s, std::uint64_t a_bytes)
	{
		std::vector<std::byte> buf(std::min<std::uint64_t>(a_bytes, 8u << 20));
		while (a_bytes > 0) {
			const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_bytes, buf.size()));
			if (!a_s.read(buf.data(), n))
				return false;
			a_bytes -= n;
		}
		return true;
	}

	struct CacheEntry
	{
		std::vector<std::byte> data;
		std::uint32_t skip = 0;
		std::uint32_t fullW = 0, fullH = 0, fullMips = 0;
		FormatInfo fi;
		std::list<std::string>::iterator lru;
	};
	std::mutex g_cacheLock;
	std::unordered_map<std::string, CacheEntry> g_cache;
	std::list<std::string> g_cacheLru;
	std::uint64_t g_cacheBytes = 0;
	std::atomic<std::uint32_t> g_cacheHits{ 0 }, g_cacheMisses{ 0 };
	std::unordered_map<std::string, std::uint32_t> g_reloadCount;

	std::uint64_t CacheLimit() noexcept
	{
		return static_cast<std::uint64_t>(std::max(0.f, Cfg().RamCacheMB)) << 20;
	}

	void CacheTrim(std::uint64_t a_limit)
	{
		while (g_cacheBytes > a_limit && !g_cacheLru.empty()) {
			const auto it = g_cache.find(g_cacheLru.back());
			if (it != g_cache.end()) {
				g_cacheBytes -= it->second.data.size();
				g_cache.erase(it);
			}
			g_cacheLru.pop_back();
		}
	}

	bool CacheGet(const std::string& a_path, std::uint32_t a_fullW, std::uint32_t a_fullH, std::uint32_t a_fullMips, FormatInfo a_fi, std::uint32_t a_skip, std::vector<std::byte>& a_out)
	{
		std::scoped_lock lock(g_cacheLock);
		const auto it = g_cache.find(a_path);
		if (it == g_cache.end())
			return false;
		auto& e = it->second;
		if (e.fullW != a_fullW || e.fullH != a_fullH || e.fullMips != a_fullMips || !(e.fi == a_fi) || e.skip > a_skip)
			return false;
		std::uint64_t off = 0;
		for (std::uint32_t i = e.skip; i < a_skip; ++i)
			off += MipBytes(a_fi, std::max(1u, a_fullW >> i), std::max(1u, a_fullH >> i));
		if (off > e.data.size())
			return false;
		a_out.assign(e.data.begin() + static_cast<std::ptrdiff_t>(off), e.data.end());
		g_cacheLru.splice(g_cacheLru.begin(), g_cacheLru, e.lru);
		return true;
	}

	void CachePut(const std::string& a_path, std::vector<std::byte>&& a_data, std::uint32_t a_skip, std::uint32_t a_fullW, std::uint32_t a_fullH, std::uint32_t a_fullMips, FormatInfo a_fi)
	{
		const auto limit = CacheLimit();
		if (limit == 0 || a_data.size() > limit / 4)
			return;
		std::scoped_lock lock(g_cacheLock);
		if (++g_reloadCount[a_path] < 2)
			return;
		if (const auto it = g_cache.find(a_path); it != g_cache.end()) {
			if (it->second.skip <= a_skip)
				return;
			g_cacheBytes -= it->second.data.size();
			g_cacheLru.erase(it->second.lru);
			g_cache.erase(it);
		}
		g_cacheLru.push_front(a_path);
		auto& e = g_cache[a_path];
		e.data = std::move(a_data);
		e.skip = a_skip;
		e.fullW = a_fullW;
		e.fullH = a_fullH;
		e.fullMips = a_fullMips;
		e.fi = a_fi;
		e.lru = g_cacheLru.begin();
		g_cacheBytes += e.data.size();
		CacheTrim(limit);
	}

	void RunJob(Result& a_res)
	{
		const auto& job = a_res.job;
		const auto t0 = Clock::now();
		std::vector<std::byte> data;
		bool fromCache = false;
		if (job.reload && CacheGet(job.path, job.fullW, job.fullH, job.fullMips, job.fi, job.skip, data)) {
			fromCache = true;
			a_res.file.width = job.fullW;
			a_res.file.height = job.fullH;
			a_res.file.mips = job.fullMips;
			a_res.file.fi = job.fi;
			g_cacheHits.fetch_add(1, std::memory_order_relaxed);
		}
		std::unique_ptr<RE::BSResourceNiBinaryStream> stream;
		if (!fromCache) {
			if (job.reload)
				g_cacheMisses.fetch_add(1, std::memory_order_relaxed);
			stream = std::make_unique<RE::BSResourceNiBinaryStream>(job.path);
			a_res.error = ReadHeader(*stream, a_res.file);
			if (!a_res.error.empty() || !job.reload)
				return;
			const auto& f = a_res.file;
			if (f.width != job.fullW || f.height != job.fullH || f.mips != job.fullMips || !(f.fi == job.fi)) {
				a_res.error = "file has changed";
				return;
			}
			std::uint64_t skipBytes = 0;
			for (std::uint32_t i = 0; i < job.skip; ++i)
				skipBytes += MipBytes(f.fi, std::max(1u, f.width >> i), std::max(1u, f.height >> i));
			if (skipBytes > 0 && !SkipBytes(*stream, skipBytes)) {
				stream = std::make_unique<RE::BSResourceNiBinaryStream>(job.path);
				FileInfo again;
				if (!ReadHeader(*stream, again).empty() || !DiscardBytes(*stream, skipBytes)) {
					a_res.error = "read error while skipping";
					return;
				}
			}
		}
		const auto& f = a_res.file;
		const std::uint32_t w = std::max(1u, f.width >> job.skip), h = std::max(1u, f.height >> job.skip);
		const std::uint32_t mips = f.mips - job.skip;
		const auto bytes = ChainBytes(f.fi, w, h, mips);
		if (fromCache && data.size() < bytes) {
			a_res.error = "RAM buffer incomplete";
			return;
		}
		if (!fromCache) {
			data.resize(bytes);
			for (std::uint64_t done = 0; done < bytes;) {
				const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(bytes - done, 16u << 20));
				if (!stream->read(data.data() + done, n)) {
					a_res.error = "file too short";
					return;
				}
				done += n;
			}
			stream.reset();
		}

		std::vector<D3D11_SUBRESOURCE_DATA> init(mips);
		std::uint64_t off = 0;
		for (std::uint32_t i = 0; i < mips; ++i) {
			const std::uint32_t mw = std::max(1u, w >> i), mh = std::max(1u, h >> i);
			init[i].pSysMem = data.data() + off;
			init[i].SysMemPitch = RowPitch(f.fi, mw);
			init[i].SysMemSlicePitch = static_cast<std::uint32_t>(MipBytes(f.fi, mw, mh));
			off += MipBytes(f.fi, mw, mh);
		}
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = w;
		desc.Height = h;
		desc.MipLevels = mips;
		desc.ArraySize = 1;
		desc.Format = static_cast<DXGI_FORMAT>(job.format);
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(g_device->CreateTexture2D(&desc, init.data(), &a_res.tex)) || !a_res.tex) {
			a_res.error = "CreateTexture2D failed";
			return;
		}
		Util::SetResourceName(a_res.tex, "TextureStreaming::%s", job.path.c_str());
		auto sd = job.srvDesc;
		sd.Texture2D.MostDetailedMip = 0;
		sd.Texture2D.MipLevels = static_cast<UINT>(-1);
		if (FAILED(g_device->CreateShaderResourceView(a_res.tex, &sd, &a_res.srv)) || !a_res.srv) {
			a_res.tex->Release();
			a_res.tex = nullptr;
			a_res.error = "CreateShaderResourceView failed";
			return;
		}
		Util::SetResourceName(a_res.srv, "TextureStreaming::%s SRV", job.path.c_str());
		a_res.mb = bytes / 1048576.0;
		a_res.ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
		if (!fromCache)
			CachePut(job.path, std::move(data), job.skip, job.fullW, job.fullH, job.fullMips, job.fi);
	}

	void ApplyResults();
	void PreviewTick();
	void SaveSizes();
	std::atomic<bool> g_saveRequested{ false };

	void Worker()
	{
		auto lastTick = Clock::now();
		auto lastGpu = Clock::now() - 1s;
		for (;;) {
			Job job;
			bool have = false;
			{
				std::unique_lock lock(g_qLock);
				g_qCv.wait_for(lock, 250ms, [] { return !g_jobs.empty(); });
				if (!g_jobs.empty()) {
					job = std::move(g_jobs.front());
					g_jobs.pop_front();
					have = true;
				}
			}
			if (have) {
				Result res;
				res.job = std::move(job);
				try {
					RunJob(res);
				} catch (const std::exception& e) {
					res.error = e.what();
				}
				{
					std::scoped_lock lock(g_qLock);
					g_results.push_back(std::move(res));
				}
				if (const auto tasks = SKSE::GetTaskInterface())
					tasks->AddTask([] { ApplyResults(); });
			}
			if (g_saveRequested.exchange(false, std::memory_order_relaxed))
				SaveSizes();
			if (Clock::now() - lastGpu >= 1s) {
				lastGpu = Clock::now();
				unsigned long long dedicated = 0, shared = 0;
				if (GpuMemory::Query(dedicated, shared)) {
					g_procDedicated.store(dedicated, std::memory_order_relaxed);
					g_procShared.store(shared, std::memory_order_relaxed);
				}
			}
			if (Clock::now() - lastTick >= 250ms) {
				lastTick = Clock::now();
				if (const auto tasks = SKSE::GetTaskInterface())
					tasks->AddTask([] { PreviewTick(); });
			}
		}
	}

	void EnsureWorker()
	{
		if (!g_workerStarted.exchange(true))
			std::thread(Worker).detach();
	}

	void Enqueue(Job a_job)
	{
		EnsureWorker();
		{
			std::scoped_lock lock(g_qLock);
			g_jobs.push_back(std::move(a_job));
		}
		g_qCv.notify_one();
	}

	std::vector<std::string> g_excludeTokens;
	std::string g_excludeSource;

	bool Excluded(const std::string& a_path)
	{
		const auto& cfg = Cfg().Exclude;
		if (cfg != g_excludeSource) {
			g_excludeSource = cfg;
			g_excludeTokens.clear();
			std::size_t pos = 0;
			while (pos <= cfg.size()) {
				const auto end = cfg.find(',', pos);
				auto tok = cfg.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
				std::ranges::transform(tok, tok.begin(), [](char c) { return c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
				std::erase(tok, ' ');
				if (!tok.empty())
					g_excludeTokens.push_back(std::move(tok));
				if (end == std::string::npos)
					break;
				pos = end + 1;
			}
		}
		return std::ranges::any_of(g_excludeTokens, [&](const std::string& t) { return a_path.find(t) != std::string::npos; });
	}

	bool Active() noexcept
	{
		return Cfg().Enabled;
	}

	std::uint32_t NeededEdge(const TexState& a_st, float a_needPx) noexcept
	{
		const auto full = a_st.FullEdge();
		const float need = std::max(a_needPx * Cfg().SafetyFactor, Cfg().MinEdge);
		std::uint32_t p = 4;
		while (p < need && p < full)
			p <<= 1;
		return std::min(p, full);
	}

	std::uint32_t WantedEdge(const TexState& a_st, float a_needPx) noexcept
	{
		return Active() ? NeededEdge(a_st, a_needPx) : a_st.FullEdge();
	}

	bool ReadDesc(ID3D11Resource* a_res, D3D11_TEXTURE2D_DESC& a_desc) noexcept
	{
		__try {
			D3D11_RESOURCE_DIMENSION dim{};
			a_res->GetType(&dim);
			if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
				return false;
			static_cast<ID3D11Texture2D*>(a_res)->GetDesc(&a_desc);
			return true;
		} __except (1) {
			return false;
		}
	}

	std::string NormalizePath(const char* a_name)
	{
		std::string p = a_name ? a_name : "";
		std::ranges::transform(p, p.begin(), [](char c) { return c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
		if (p.starts_with("data\\"))
			p.erase(0, 5);
		return p;
	}

	std::mutex g_sizeLock;
	std::unordered_map<std::string, std::uint32_t> g_loadEdge;
	std::unordered_set<std::string> g_neverReduce;
	thread_local std::string t_loadPath;
	bool g_sizesDirty = false;
	std::atomic<std::uint32_t> g_loadedReduced{ 0 };
	thread_local RE::NiSourceTexture* t_loadingSrc = nullptr;
	bool g_loaderHooked = false;

	std::filesystem::path SizesFile()
	{
		auto dir = logger::log_directory();
		return dir ? *dir / "BottledShaders_TextureSizes.txt" : std::filesystem::path{};
	}

	using EdgeList = std::vector<std::pair<std::string, std::uint32_t>>;

	void RememberEdges(const EdgeList& a_edges)
	{
		std::scoped_lock lock(g_sizeLock);
		for (const auto& [path, edge] : a_edges) {
			if (path.empty())
				continue;
			const std::uint32_t wanted = edge != 0 && g_neverReduce.contains(path) ? 0u : edge;
			if (wanted == 0) {
				g_sizesDirty |= g_loadEdge.erase(path) > 0;
			} else if (auto& e = g_loadEdge[path]; e != wanted) {
				e = wanted;
				g_sizesDirty = true;
			}
		}
	}

	bool HasRememberedEdge(const std::string& a_path)
	{
		std::scoped_lock lock(g_sizeLock);
		return g_loadEdge.contains(a_path);
	}

	void LoadSizes()
	{
		const auto file = SizesFile();
		if (file.empty())
			return;
		std::ifstream in(file);
		std::string line;
		std::size_t n = 0;
		while (std::getline(in, line)) {
			const auto bar = line.find('|');
			if (bar == std::string::npos)
				continue;
			const auto edge = static_cast<std::uint32_t>(std::strtoul(line.c_str(), nullptr, 10));
			if (edge >= 64) {
				g_loadEdge[line.substr(bar + 1)] = edge;
				++n;
			}
		}
		logger::info("[Texture Streaming] {} remembered texture sizes loaded", n);
	}

	void SaveSizes()
	{
		std::vector<std::pair<std::string, std::uint32_t>> copy;
		{
			std::scoped_lock lock(g_sizeLock);
			if (!g_sizesDirty)
				return;
			g_sizesDirty = false;
			copy.assign(g_loadEdge.begin(), g_loadEdge.end());
		}
		const auto file = SizesFile();
		if (file.empty())
			return;
		const auto tmp = std::filesystem::path(file).concat(".tmp");
		std::ofstream out(tmp, std::ios::trunc);
		for (const auto& [path, edge] : copy)
			out << edge << '|' << path << '\n';
		out.close();
		std::error_code ec;
		std::filesystem::rename(tmp, file, ec);
	}

	std::uint64_t LoadMaxSize() noexcept
	{
		const auto src = t_loadingSrc;
		if (!src)
			return 0;
		const auto& cfg = Cfg();
		if (cfg.BudgetMode)
			RefreshVramThrottled();
		if (!cfg.LoadAtRememberedSize || !cfg.Enabled || !LoadPressure())
			return 0;
		try {
			const auto path = NormalizePath(src->name.c_str());
			std::scoped_lock lock(g_sizeLock);
			const auto it = g_loadEdge.find(path);
			if (it == g_loadEdge.end())
				return 0;
			g_loadedReduced.fetch_add(1, std::memory_order_relaxed);
			t_loadPath = path;
			return it->second;
		} catch (...) {
			return 0;
		}
	}

	std::uint32_t SafeLoadEdge(const TexState& a_st, std::uint32_t a_edge) noexcept
	{
		if (a_edge == 0 || a_edge >= a_st.FullEdge() || a_st.fullMips <= 1)
			return 0;
		std::uint32_t skip = 0;
		while (skip + 1 < a_st.fullMips && (std::max(1u, a_st.fullW >> skip) > a_edge || std::max(1u, a_st.fullH >> skip) > a_edge))
			++skip;
		const std::uint32_t w = std::max(1u, a_st.fullW >> skip), h = std::max(1u, a_st.fullH >> skip);
		if (skip == 0 || w > a_edge || h > a_edge || (a_st.fi.bc && (w % 4 != 0 || h % 4 != 0)))
			return 0;
		return a_edge;
	}

	void QueueReload(TexState& a_st, RE::BSGraphics::Texture* a_r, std::uint32_t a_targetEdge)
	{
		std::uint32_t skip = 0;
		while ((a_st.FullEdge() >> (skip + 1)) >= a_targetEdge && skip + 1 < a_st.fullMips)
			++skip;
		while (skip > 0 && a_st.fi.bc && ((std::max(1u, a_st.fullW >> skip) % 4) != 0 || (std::max(1u, a_st.fullH >> skip) % 4) != 0))
			--skip;
		Job job;
		job.reload = true;
		job.r = a_r;
		job.expectRes = a_st.res;
		job.path = a_st.path;
		job.fullW = a_st.fullW;
		job.fullH = a_st.fullH;
		job.fullMips = a_st.fullMips;
		job.format = a_st.format;
		job.fi = a_st.fi;
		job.skip = skip;
		if (!a_r->resourceView)
			return;
		a_r->resourceView->GetDesc(&job.srvDesc);
		if (job.srvDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) {
			a_st.eligible = false;
			return;
		}
		const std::uint64_t now = ChainBytes(a_st.fi, a_st.curW, a_st.curH, a_st.curMips);
		const std::uint64_t then = ChainBytes(a_st.fi, std::max(1u, a_st.fullW >> skip), std::max(1u, a_st.fullH >> skip), a_st.fullMips - skip);
		job.extraBytes = then > now ? then - now : 0;
		g_inflightUp += job.extraBytes;
		a_st.busy = true;
		Enqueue(std::move(job));
	}

	void QueueProbe(TexState& a_st, RE::BSGraphics::Texture* a_r)
	{
		Job job;
		job.r = a_r;
		job.expectRes = a_st.res;
		job.path = a_st.path;
		a_st.probe = Probe::kPending;
		a_st.busy = true;
		Enqueue(std::move(job));
	}

	std::uint32_t g_passId = 1;

	void OnSeen(RE::NiSourceTexture* a_src, float a_needPx)
	{
		const auto r = a_src->rendererTexture;
		if (!r || !r->texture)
			return;
		auto [it, inserted] = g_tex.try_emplace(r);
		auto& st = it->second;
		if (inserted || st.res != r->texture || st.name != a_src->name.c_str()) {
			const bool busy = st.busy;
			st = TexState{};
			st.busy = busy;
			st.res = r->texture;
			st.name = a_src->name.c_str();
			D3D11_TEXTURE2D_DESC d{};
			if (ReadDesc(st.res, d)) {
				st.curW = st.fullW = d.Width;
				st.curH = st.fullH = d.Height;
				st.curMips = st.fullMips = d.MipLevels;
				st.format = static_cast<std::uint32_t>(d.Format);
				st.fi = InfoOf(st.format);
				st.path = NormalizePath(a_src->name.c_str());
				st.eligible = d.ArraySize == 1 && d.SampleDesc.Count == 1 && !(d.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) && d.MipLevels > 1 && st.fi.bytes > 0 &&
				              st.path.starts_with("textures\\") && st.path.ends_with(".dds") && !Excluded(st.path);
				if (st.eligible && !st.busy && HasRememberedEdge(st.path))
					QueueProbe(st, r);
			}
		}
		st.lastSeen = Clock::now();
		if (st.passId != g_passId) {
			st.passId = g_passId;
			st.passNeed = 0;
		}
		st.passNeed = std::max(st.passNeed, a_needPx);
		if (!st.eligible)
			return;
		if (!st.hold)
			st.hold.reset(a_src);
		if (st.Reduced() && !st.busy && st.probe == Probe::kOk) {
			if (WantedEdge(st, a_needPx / UpMargin()) > st.CurEdge())
				QueueReload(st, r, UpTarget(WantedEdge(st, a_needPx), st.FullEdge()));
		}
	}

	struct DownJob
	{
		RE::BSGraphics::Texture* r;
		RE::NiPointer<RE::NiSourceTexture> src;
		std::uint32_t targetEdge;
	};
	std::deque<DownJob> g_down;

	void Downscale(const DownJob& a_job, std::uint64_t& a_freed)
	{
		a_freed = 0;
		const auto it = g_tex.find(a_job.r);
		if (it == g_tex.end())
			return;
		auto& st = it->second;
		st.busy = false;
		const auto r = a_job.r;
		if (!a_job.src || a_job.src->rendererTexture != r || r->texture != st.res || !r->resourceView || !g_device || !g_context)
			return;
		std::uint32_t drop = 0;
		while ((st.CurEdge() >> (drop + 1)) >= a_job.targetEdge && drop + 1 < st.curMips)
			++drop;
		while (drop > 0 && st.fi.bc && (((st.curW >> drop) % 4) != 0 || ((st.curH >> drop) % 4) != 0))
			--drop;
		if (drop == 0)
			return;
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = std::max(1u, st.curW >> drop);
		desc.Height = std::max(1u, st.curH >> drop);
		desc.MipLevels = st.curMips - drop;
		desc.ArraySize = 1;
		desc.Format = static_cast<DXGI_FORMAT>(st.format);
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		ID3D11Texture2D* tex = nullptr;
		if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &tex)) || !tex) {
			st.eligible = false;
			return;
		}
		Util::SetResourceName(tex, "TextureStreaming::%s", st.path.c_str());
		D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
		r->resourceView->GetDesc(&sd);
		if (sd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) {
			tex->Release();
			st.eligible = false;
			return;
		}
		sd.Texture2D.MostDetailedMip = 0;
		sd.Texture2D.MipLevels = static_cast<UINT>(-1);
		ID3D11ShaderResourceView* srv = nullptr;
		if (FAILED(g_device->CreateShaderResourceView(tex, &sd, &srv)) || !srv) {
			tex->Release();
			st.eligible = false;
			return;
		}
		Util::SetResourceName(srv, "TextureStreaming::%s SRV", st.path.c_str());
		for (std::uint32_t i = 0; i < desc.MipLevels; ++i)
			g_context->CopySubresourceRegion(tex, i, 0, 0, 0, st.res, i + drop, nullptr);
		const auto oldBytes = ChainBytes(st.fi, st.curW, st.curH, st.curMips);
		Swap(r, tex, srv, oldBytes);
		a_freed = oldBytes - ChainBytes(st.fi, desc.Width, desc.Height, desc.MipLevels);
		st.res = tex;
		st.curW = desc.Width;
		st.curH = desc.Height;
		st.curMips = desc.MipLevels;
		st.hold = a_job.src;
		if (st.lastReload.time_since_epoch().count() != 0 && Clock::now() - st.lastReload < 60s)
			++g_stats.pingPong;
		++g_stats.downs;
	}

	void LogFail(const std::string& a_path, const std::string& a_err)
	{
		if (g_failLogged < 30) {
			++g_failLogged;
			logger::info("[Texture Streaming] not reducible: {} ({})", a_path, a_err);
		}
	}

	void ApplyResults()
	{
		std::vector<Result> results;
		{
			std::scoped_lock lock(g_qLock);
			results.swap(g_results);
		}
		for (auto& res : results) {
			const auto& job = res.job;
			g_inflightUp -= std::min(g_inflightUp, job.extraBytes);
			const auto it = g_tex.find(job.r);
			const bool valid = it != g_tex.end() && it->second.res == job.expectRes && job.r->texture == job.expectRes;
			if (it != g_tex.end())
				it->second.busy = false;
			if (!job.reload) {
				if (!valid)
					continue;
				auto& st = it->second;
				const auto& f = res.file;
				if (!res.error.empty()) {
					st.probe = Probe::kBad;
					st.eligible = false;
					++g_stats.probesBad;
					LogFail(st.path, res.error);
					continue;
				}
				std::uint32_t skip = 0;
				while (skip < 16 && (f.width >> skip) > st.curW)
					++skip;
				const bool match = f.fi == st.fi && (f.width >> skip) == st.curW && std::max(1u, f.height >> skip) == st.curH && f.mips >= skip + 1 && f.mips - skip == st.curMips;
				if (!match) {
					st.probe = Probe::kBad;
					st.eligible = false;
					++g_stats.probesBad;
					LogFail(st.path, std::format("file {}x{} {} mips, in game {}x{} {} mips", f.width, f.height, f.mips, st.curW, st.curH, st.curMips));
					continue;
				}
				st.fullW = f.width;
				st.fullH = f.height;
				st.fullMips = f.mips;
				st.probe = Probe::kOk;
				continue;
			}
			const bool alive = valid && it->second.hold && it->second.hold->rendererTexture == job.r;
			if (!res.error.empty() || !alive) {
				if (res.tex)
					res.tex->Release();
				if (res.srv)
					res.srv->Release();
				if (!res.error.empty()) {
					++g_stats.upFails;
					if (it != g_tex.end()) {
						it->second.eligible = false;
						LogFail(job.path, "reload: " + res.error);
					}
				}
				continue;
			}
			auto& st = it->second;
			Swap(job.r, res.tex, res.srv, ChainBytes(st.fi, st.curW, st.curH, st.curMips));
			st.res = res.tex;
			st.curW = std::max(1u, job.fullW >> job.skip);
			st.curH = std::max(1u, job.fullH >> job.skip);
			st.curMips = job.fullMips - job.skip;
			st.lastReload = Clock::now();
			++g_stats.ups;
			g_stats.upMs += res.ms;
			g_stats.upMB += res.mb;
		}
		ProcessDeferred();
	}

	std::vector<RE::NiPointer<RE::NiAVObject>> g_stack;
	bool g_passActive = false;
	Clock::time_point g_passStart{};
	std::uint32_t g_passFrames = 0, g_passNodes = 0;
	double g_passMs = 0;
	float g_camX = 0, g_camY = 0, g_camZ = 0;
	float g_pixelsPerUnit = 1000.0f;

	constexpr std::uint32_t kMaxTextures = 128;

	int SafeGather(RE::BSLightingShaderMaterialBase* a_material, RE::NiSourceTexture** a_out) noexcept
	{
		__try {
			std::uint32_t count = a_material->GetTextures(a_out);
			return static_cast<int>(std::min(count, kMaxTextures));
		} __except (1) {
			return -1;
		}
	}

	void VisitGeometry(RE::BSGeometry* a_geom, float a_forceNeed)
	{
		const auto prop = a_geom->GetGeometryRuntimeData().shaderProperty.get();
		const auto lsp = prop ? netimmerse_cast<RE::BSLightingShaderProperty*>(prop) : nullptr;
		const auto material = lsp ? static_cast<RE::BSLightingShaderMaterialBase*>(lsp->material) : nullptr;
		if (!material)
			return;
		float need = a_forceNeed;
		if (need <= 0) {
			const auto& b = a_geom->worldBound;
			const float dx = b.center.x - g_camX, dy = b.center.y - g_camY, dz = b.center.z - g_camZ;
			const float dist = std::sqrt(dx * dx + dy * dy + dz * dz) - b.radius;
			need = dist <= 1.0f ? 1.0e6f : 2.0f * b.radius / dist * g_pixelsPerUnit;
		}
		RE::NiSourceTexture* textures[kMaxTextures]{};
		const int n = SafeGather(material, textures);
		for (int i = 0; i < n; ++i) {
			if (textures[i])
				OnSeen(textures[i], need);
		}
	}

	void Visit(RE::NiAVObject* a_obj, std::vector<RE::NiPointer<RE::NiAVObject>>& a_stack, float a_forceNeed)
	{
		if (const auto node = a_obj->AsNode()) {
			for (const auto& child : node->GetChildren()) {
				if (child)
					a_stack.push_back(child);
			}
		} else if (const auto geom = a_obj->AsGeometry()) {
			VisitGeometry(geom, a_forceNeed);
		}
	}

	void UpdateCamera()
	{
		const auto pos = Util::GetCameraWorldPosition();
		g_camX = pos.x;
		g_camY = pos.y;
		g_camZ = pos.z;
		float tanHalf = 0.6f;
		if (const auto cam = RE::Main::WorldRootCamera()) {
			const float top = cam->GetRuntimeData2().viewFrustum.fTop;
			if (top > 0.01f && top < 5.0f)
				tanHalf = top;
		}
		float screenH = 1200.0f;
		if (const auto state = globals::game::graphicsState; state && state->screenHeight > 0)
			screenH = static_cast<float>(state->screenHeight);
		g_pixelsPerUnit = screenH / (2.0f * tanHalf);
	}

	std::atomic<bool> g_probeCenter{ false };

	void ProbeCenter()
	{
		const auto root = RE::Main::WorldRootNode();
		const auto cam = RE::Main::WorldRootCamera();
		if (!root || !cam) {
			logger::info("[Texture Streaming] crosshair probe: no world or camera");
			return;
		}
		const auto& cw = cam->world;
		const float px = cw.translate.x, py = cw.translate.y, pz = cw.translate.z;
		float fx = cw.rotate.entry[0][0], fy = cw.rotate.entry[1][0], fz = cw.rotate.entry[2][0];
		const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
		if (fl < 1e-4f) {
			logger::info("[Texture Streaming] crosshair probe: no view direction");
			return;
		}
		fx /= fl, fy /= fl, fz /= fl;
		struct Hit
		{
			RE::BSGeometry* geom;
			float dist;
			float need;
		};
		std::vector<Hit> hits;
		std::vector<RE::NiAVObject*> stack{ root };
		std::uint32_t visited = 0;
		while (!stack.empty()) {
			const auto obj = stack.back();
			stack.pop_back();
			if (!obj || obj->GetFlags().any(RE::NiAVObject::Flag::kHidden))
				continue;
			++visited;
			const auto& b = obj->worldBound;
			const float dx = b.center.x - px, dy = b.center.y - py, dz = b.center.z - pz;
			const float t = dx * fx + dy * fy + dz * fz;
			const float centerDist2 = dx * dx + dy * dy + dz * dz;
			const bool onRay = b.radius > 0.0f && t >= -b.radius && centerDist2 - t * t <= b.radius * b.radius;
			const float centerDist = std::sqrt(centerDist2);
			if (const auto node = obj->AsNode()) {
				for (const auto& child : node->GetChildren()) {
					if (child)
						stack.push_back(child.get());
				}
			} else if (const auto geom = obj->AsGeometry(); geom && onRay && b.radius < 5000.0f) {
				const auto gp = geom->GetGeometryRuntimeData().shaderProperty.get();
				if (!gp || !netimmerse_cast<RE::BSLightingShaderProperty*>(gp))
					continue;
				const float dist = std::max(0.0f, t - std::sqrt(std::max(0.0f, b.radius * b.radius - (centerDist2 - t * t))));
				const float surf = std::max(0.0f, centerDist - b.radius);
				hits.push_back({ geom, dist, surf <= 1.0f ? 1.0e6f : 2.0f * b.radius / surf * g_pixelsPerUnit });
			}
		}
		std::ranges::sort(hits, [](const Hit& a, const Hit& b) { return a.dist < b.dist; });
		logger::info("[Texture Streaming] crosshair probe: {} objects under the crosshair (nearest first, {} nodes checked), streaming {}", hits.size(), visited, Active() ? "ON" : "OFF");
		for (std::size_t i = 0; i < hits.size() && i < 25; ++i) {
			const auto geom = hits[i].geom;
			const auto prop = geom->GetGeometryRuntimeData().shaderProperty.get();
			const auto lsp = prop ? netimmerse_cast<RE::BSLightingShaderProperty*>(prop) : nullptr;
			const auto material = lsp ? static_cast<RE::BSLightingShaderMaterialBase*>(lsp->material) : nullptr;
			logger::info("[Texture Streaming]  #{} '{}' distance {:.0f} | radius {:.0f} | ~{:.0f} px | material {}", i + 1, geom->name.c_str() ? geom->name.c_str() : "",
				hits[i].dist, geom->worldBound.radius, hits[i].need, material ? static_cast<int>(material->GetFeature()) : -1);
			if (!material)
				continue;
			RE::NiSourceTexture* textures[kMaxTextures]{};
			const int n = SafeGather(material, textures);
			for (int t = 0; t < n; ++t) {
				const auto src = textures[t];
				const auto r = src ? src->rendererTexture : nullptr;
				if (!r || !r->texture)
					continue;
				D3D11_TEXTURE2D_DESC d{};
				const bool hasDesc = ReadDesc(r->texture, d);
				std::string extra = "unmanaged";
				if (const auto it = g_tex.find(r); it != g_tex.end()) {
					const auto& st = it->second;
					extra = std::format("original {}x{} ({} mips) | now {}x{} | need {:.0f} px | {}{}", st.fullW, st.fullH, st.fullMips, st.curW, st.curH,
						st.passNeed, st.eligible ? "reducible" : "not reducible", st.path != NormalizePath(src->name.c_str()) ? " | PATH DIFFERS: " + st.path : "");
				}
				std::uint32_t remembered = 0;
				{
					std::scoped_lock lock(g_sizeLock);
					if (const auto it = g_loadEdge.find(NormalizePath(src->name.c_str())); it != g_loadEdge.end())
						remembered = it->second;
				}
				logger::info("[Texture Streaming]     [{}] {} | D3D {}x{} {} mips format {} | remembered {} | {}", t, src->name.c_str() ? src->name.c_str() : "",
					hasDesc ? d.Width : 0, hasDesc ? d.Height : 0, hasDesc ? d.MipLevels : 0, hasDesc ? static_cast<int>(d.Format) : -1, remembered, extra);
			}
		}
	}

	void PassEnd()
	{
		const auto now = Clock::now();
		const bool active = Active();
		const bool pressure = Pressure();
		struct Candidate
		{
			DownJob job;
			std::uint64_t saving;
			float ratio;
		};
		std::vector<Candidate> candidates;
		struct RefillCandidate
		{
			RE::BSGraphics::Texture* r;
			TexState* st;
			float priority;
			std::uint64_t bytes;
		};
		std::vector<RefillCandidate> refill;
		const auto& cfg = Cfg();
		const float pct = g_vramPct.load(std::memory_order_relaxed);
		if (pressure) {
			g_lastPressure = now;
			g_lastPressureTicks.store(now.time_since_epoch().count(), std::memory_order_relaxed);
		}
		const bool calm = now - g_lastPressure >= kRefillCalm && now - g_lastLoad >= kRefillCalm;
		const bool wantRefill = active && cfg.BudgetMode && cfg.Refill && calm && pct >= 0.0f && pct < cfg.BudgetStartPercent - cfg.RefillGapPercent &&
		                        g_procShared.load(std::memory_order_relaxed) < kSharedPressure / 2;
		g_reducedCount = 0;
		g_savedBytes = 0;
		EdgeList edges;
		for (auto it = g_tex.begin(); it != g_tex.end(); ++it) {
			auto& st = it->second;
			const auto r = it->first;
			const bool seen = st.passId == g_passId;
			if (st.Reduced()) {
				++g_reducedCount;
				g_savedBytes += ChainBytes(st.fi, st.fullW, st.fullH, st.fullMips) - ChainBytes(st.fi, st.curW, st.curH, st.curMips);
			}
			if (seen && st.eligible && (st.probe == Probe::kOk || st.probe == Probe::kNone))
				edges.emplace_back(st.path, SafeLoadEdge(st, NeededEdge(st, st.passNeed)));
			if (wantRefill && seen && st.eligible && !st.busy && st.hold && st.probe == Probe::kOk && st.Reduced()) {
				refill.push_back({ r, &st, st.passNeed / static_cast<float>(st.CurEdge()),
					ChainBytes(st.fi, st.fullW, st.fullH, st.fullMips) - ChainBytes(st.fi, st.curW, st.curH, st.curMips) });
			}
			if (st.eligible && !st.busy) {
				if (st.Reduced() && st.hold && st.probe == Probe::kOk && (!active || (seen && WantedEdge(st, st.passNeed / UpMargin()) > st.CurEdge()))) {
					QueueReload(st, r, active ? UpTarget(WantedEdge(st, st.passNeed), st.FullEdge()) : st.FullEdge());
				} else if (active && seen && pressure) {
					const auto want = WantedEdge(st, st.passNeed);
					if (want * 2 <= st.CurEdge() && now - st.lastReload >= kCooldown) {
						if (st.probe == Probe::kNone) {
							QueueProbe(st, r);
						} else if (st.probe == Probe::kOk && st.hold) {
							st.lowTarget = st.lowPasses == 0 ? want : std::max(st.lowTarget, want);
							if (++st.lowPasses >= kLowPasses) {
								st.lowPasses = 0;
								st.busy = true;
								const std::uint32_t edge = st.lowTarget;
								const std::uint32_t w = std::max(1u, st.curW * edge / st.CurEdge()), h = std::max(1u, st.curH * edge / st.CurEdge());
								candidates.push_back({ { r, st.hold, edge }, ChainBytes(st.fi, st.curW, st.curH, st.curMips) - ChainBytes(st.fi, w, h, st.curMips),
									st.passNeed / static_cast<float>(st.CurEdge()) });
							}
						}
					} else {
						st.lowPasses = 0;
					}
				}
			}
		}
		RememberEdges(edges);
		if (!refill.empty()) {
			const double budget = static_cast<double>(g_vramBudget.load(std::memory_order_relaxed));
			const double usage = static_cast<double>(g_vramUsage.load(std::memory_order_relaxed));
			double room = budget * (cfg.BudgetStartPercent - cfg.RefillGapPercent * 0.75) / 100.0 - usage - static_cast<double>(g_inflightUp);
			room = std::min(room, static_cast<double>(kRefillPerPass));
			std::ranges::sort(refill, [](const RefillCandidate& a, const RefillCandidate& b) { return a.priority > b.priority; });
			for (const auto& c : refill) {
				if (room < static_cast<double>(c.bytes))
					break;
				if (!c.st->busy) {
					QueueReload(*c.st, c.r, c.st->FullEdge());
					room -= static_cast<double>(c.bytes);
					++g_stats.refills;
				}
			}
		}
		for (auto it = g_tex.begin(); it != g_tex.end();) {
			auto& st = it->second;
			if (st.hold && !st.busy)
				st.hold.reset();
			if (!st.hold && !st.busy && now - st.lastSeen > 120s)
				it = g_tex.erase(it);
			else
				++it;
		}
		std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
			if (std::abs(a.ratio - b.ratio) > 0.01f)
				return a.ratio < b.ratio;
			return a.saving > b.saving;
		});
		for (auto& c : candidates)
			g_down.push_back(std::move(c.job));
	}

	void PreviewTick()
	{
		ApplyResults();
		const auto ui = globals::game::ui;
		if (!ui)
			return;
		static constexpr std::array kPreviewMenus{ RE::InventoryMenu::MENU_NAME, RE::ContainerMenu::MENU_NAME, RE::BarterMenu::MENU_NAME,
			RE::GiftMenu::MENU_NAME, RE::CraftingMenu::MENU_NAME, RE::MagicMenu::MENU_NAME };
		if (std::ranges::none_of(kPreviewMenus, [&](const auto& a_name) { return ui->IsMenuOpen(a_name); }))
			return;
		const auto mgr = RE::Inventory3DManager::GetSingleton();
		if (!mgr)
			return;
		std::vector<RE::NiPointer<RE::NiAVObject>> stack;
		for (const auto& model : mgr->GetRuntimeData().loadedModels) {
			if (model.spModel)
				stack.push_back(model.spModel);
		}
		std::uint32_t guard = 0;
		while (!stack.empty() && ++guard < 20000) {
			const auto obj = std::move(stack.back());
			stack.pop_back();
			if (obj)
				Visit(obj.get(), stack, 1.0e6f);
		}
	}

	void ReportCompact()
	{
		const auto& s = g_stats;
		logger::info(
			"[Texture Streaming] VRAM {:.1f}/{:.1f} GB ({:.0f} %, shared {:.0f} MB) | {} textures reduced, {:.0f} MB saved | last minute: "
			"reduced {}, reloaded {} ({:.0f} MB), ping-pong {}, refilled {}, loaded reduced {}, from RAM {}, load errors {}",
			g_vramUsage.load() / 1073741824.0, g_vramBudget.load() / 1073741824.0, std::max(0.0f, g_vramPct.load()), g_procShared.load() / 1048576.0, g_reducedCount,
			g_savedBytes / 1048576.0, s.downs, s.ups, s.upMB, s.pingPong, s.refills, g_loadedReduced.exchange(0), g_cacheHits.exchange(0), s.upFails);
		g_cacheMisses.exchange(0);
		g_stats = {};
	}

	void Report()
	{
		std::uint32_t managed = 0, eligible = 0, reduced = 0, probeBad = 0;
		double fullMB = 0, curMB = 0;
		for (const auto& entry : g_tex) {
			const auto& st = entry.second;
			++managed;
			if (st.eligible)
				++eligible;
			if (st.probe == Probe::kBad)
				++probeBad;
			if (st.Reduced()) {
				++reduced;
				fullMB += ChainBytes(st.fi, st.fullW, st.fullH, st.fullMips) / 1048576.0;
				curMB += ChainBytes(st.fi, st.curW, st.curH, st.curMips) / 1048576.0;
			}
		}
		std::size_t queued = 0;
		{
			std::scoped_lock lock(g_qLock);
			queued = g_jobs.size();
		}
		const auto& s = g_stats;
		logger::info("[Texture Streaming] {} | textures {} (reducible {}, file mismatch {}) | reduced {} -> {:.0f} MB instead of {:.0f} MB = {:.0f} MB saved",
			Active() ? "ON" : "OFF", managed, eligible, probeBad, reduced, curMB, fullMB, fullMB - curMB);
		logger::info("[Texture Streaming]   10 s: reduced {} | reloaded {} ({:.0f} MB, avg {:.0f} ms) | load errors {} | ping-pong {} | refilled {} | queue {} | passes {} (avg {:.0f} frames, {:.0f} nodes, {:.2f} ms total)",
			s.downs, s.ups, s.upMB, s.ups ? s.upMs / s.ups : 0.0, s.upFails, s.pingPong, s.refills, queued, s.passes, s.passes ? double(s.passFrames) / s.passes : 0.0,
			s.passes ? double(s.passNodes) / s.passes : 0.0, s.passes ? s.passMs / s.passes : 0.0);
		std::size_t remembered = 0;
		{
			std::scoped_lock lock(g_sizeLock);
			remembered = g_loadEdge.size();
		}
		logger::info("[Texture Streaming]   VRAM {:.1f} / {:.1f} GB ({:.0f} %, DXGI {:.1f} GB, shared {:.0f} MB) | budget mode {} from {:.0f} % -> {}",
			g_vramUsage.load() / 1073741824.0, g_vramBudget.load() / 1073741824.0, std::max(0.0f, g_vramPct.load()), g_dxgiUsage.load() / 1073741824.0,
			g_procShared.load() / 1048576.0, Cfg().BudgetMode ? "ON" : "OFF", Cfg().BudgetStartPercent, Pressure() ? "reducing" : "enough room");
		{
			std::scoped_lock lock(g_cacheLock);
			CacheTrim(CacheLimit());
			logger::info("[Texture Streaming]   RAM buffer {:.0f} / {:.0f} MB, {} textures | from RAM {} | from disk {}", g_cacheBytes / 1048576.0,
				Cfg().RamCacheMB, g_cache.size(), g_cacheHits.exchange(0), g_cacheMisses.exchange(0));
		}
		logger::info("[Texture Streaming]   load at remembered size: {} | loaded reduced {} | remembered sizes {}", Cfg().LoadAtRememberedSize ? "ON" : "OFF",
			g_loadedReduced.exchange(0), remembered);
		g_stats = {};
	}

	void OnFrame()
	{
		static Clock::time_point lastSave = Clock::now();
		if (Clock::now() - lastSave >= 300s) {
			lastSave = Clock::now();
			g_saveRequested.store(true, std::memory_order_relaxed);
		}
		static Clock::time_point lastReport = Clock::now();
		const auto& cfg = Cfg();
		const auto now = Clock::now();

		if (!g_device) {
			g_device = globals::d3d::device;
			g_context = globals::d3d::context;
			if (!g_device || !g_context)
				return;
			InitAdapter();
		}
		if (g_probeCenter.exchange(false)) {
			UpdateCamera();
			ProbeCenter();
		}
		static Clock::time_point lastVram{};
		if (now - lastVram >= 250ms) {
			lastVram = now;
			UpdateVram();
		}

		ApplyResults();

		if (now - lastReport >= (cfg.DetailedLog ? 10s : 60s)) {
			lastReport = now;
			if (cfg.DetailedLog)
				Report();
			else
				ReportCompact();
		}

		const auto ui = globals::game::ui;
		if (!ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
			g_stack.clear();
			g_passActive = false;
			for (const auto& job : g_down) {
				if (const auto it = g_tex.find(job.r); it != g_tex.end())
					it->second.busy = false;
			}
			g_down.clear();
			return;
		}

		if (!Pressure() && !g_down.empty()) {
			for (const auto& job : g_down) {
				if (const auto it = g_tex.find(job.r); it != g_tex.end())
					it->second.busy = false;
			}
			g_down.clear();
		}
		std::uint64_t freedThisFrame = 0;
		for (int i = 0; i < 8 && !g_down.empty() && g_deferred.size() < 64; ++i) {
			const auto job = std::move(g_down.front());
			g_down.pop_front();
			std::uint64_t freed = 0;
			Downscale(job, freed);
			freedThisFrame += freed;
			if (freedThisFrame >= kReleaseBytesPerCall)
				break;
		}

		if (!g_passActive) {
			const bool idle = cfg.BudgetMode && !Pressure() && g_reducedCount == 0;
			if (now - g_passStart < (idle ? 5000ms : 500ms))
				return;
			const auto root = RE::Main::WorldRootNode();
			if (!root)
				return;
			g_passActive = true;
			g_passStart = now;
			g_passFrames = g_passNodes = 0;
			g_passMs = 0;
			++g_passId;
			g_stack.clear();
			g_stack.emplace_back(root);
		}
		UpdateCamera();
		++g_passFrames;
		const auto t0 = Clock::now();
		const auto budget = std::chrono::duration<double, std::milli>(std::clamp(cfg.ScanBudgetMs, 0.05f, 5.0f));
		std::uint32_t n = 0;
		while (!g_stack.empty()) {
			const auto obj = std::move(g_stack.back());
			g_stack.pop_back();
			if (obj)
				Visit(obj.get(), g_stack, 0.0f);
			++n;
			if ((n & 31) == 0 && Clock::now() - t0 >= budget)
				break;
		}
		g_passNodes += n;
		g_passMs += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
		if (g_stack.empty()) {
			g_passActive = false;
			++g_stats.passes;
			g_stats.passFrames += g_passFrames;
			g_stats.passNodes += g_passNodes;
			g_stats.passMs += g_passMs;
			PassEnd();
		}
	}

	void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg->type == SKSE::MessagingInterface::kPreLoadGame)
			globals::features::textureStreaming.Reset("loading save");
		else if (a_msg->type == SKSE::MessagingInterface::kNewGame)
			globals::features::textureStreaming.Reset("new game");
	}
}

void TextureStreaming::LoadSettings(json& o_json)
{
	settings = o_json;
	const Settings defaults{};
	settings.BudgetStartPercent = Util::ClampFinite(settings.BudgetStartPercent, defaults.BudgetStartPercent, 50.f, 98.f);
	settings.RefillGapPercent = Util::ClampFinite(settings.RefillGapPercent, defaults.RefillGapPercent, 2.f, 30.f);
	settings.RamCacheMB = Util::ClampFinite(settings.RamCacheMB, defaults.RamCacheMB, 0.f, 8192.f);
	settings.SafetyFactor = Util::ClampFinite(settings.SafetyFactor, defaults.SafetyFactor, 1.f, 4.f);
	settings.MinEdge = Util::ClampFinite(settings.MinEdge, defaults.MinEdge, 256.f, 4096.f);
	settings.ScanBudgetMs = Util::ClampFinite(settings.ScanBudgetMs, defaults.ScanBudgetMs, 0.05f, 5.f);
	settings.MinEdge = static_cast<float>(std::bit_ceil(static_cast<std::uint32_t>(settings.MinEdge)));
}

void TextureStreaming::SaveSettings(json& o_json)
{
	o_json = settings;
}

void TextureStreaming::RestoreDefaultSettings()
{
	settings = {};
}

void TextureStreaming::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Stream Textures By Distance"), &settings.Enabled);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("enabled_tooltip"), "Shrinks textures of far objects in VRAM and reloads them when you come closer. Switching this off reloads every reduced texture at full size, which takes a few seconds."));

	ImGui::Checkbox(T(TKEY("load_at_remembered_size"), "Load At Remembered Size"), &settings.LoadAtRememberedSize);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("load_at_remembered_size_tooltip"), "The size a texture needed last time, also from earlier sessions, is passed to the DDS loader so the top mip levels are never read. Only applied while VRAM is tight."));

	ImGui::SeparatorText(T(TKEY("budget"), "VRAM Budget"));

	ImGui::Checkbox(T(TKEY("budget_mode"), "Budget Mode"), &settings.BudgetMode);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("budget_mode_tooltip"), "Only downscale once the game uses more than the threshold below of the video memory Windows grants it. With enough VRAM nothing happens and nothing costs anything. Off always downscales by distance."));

	{
		auto _ = Util::DisableGuard(!settings.BudgetMode);
		ImGui::Indent();

		ImGui::SliderFloat(T(TKEY("budget_start_percent"), "Start Downscaling At"), &settings.BudgetStartPercent, 50.f, 98.f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("budget_start_percent_tooltip"), "VRAM usage as a share of the budget above which textures are downscaled, largest savings first."));

		ImGui::Checkbox(T(TKEY("refill"), "Refill When Room Frees Up"), &settings.Refill);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("refill_tooltip"), "Once usage drops clearly below the threshold, reduced textures are reloaded at full size again, most needed first."));

		ImGui::SliderFloat(T(TKEY("refill_gap_percent"), "Refill Gap"), &settings.RefillGapPercent, 2.f, 30.f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("refill_gap_percent_tooltip"), "How many percentage points below the threshold usage has to fall before refilling starts."));

		ImGui::Unindent();
	}

	ImGui::SeparatorText(T(TKEY("quality"), "Quality"));

	ImGui::SliderFloat(T(TKEY("safety_factor"), "Safety Factor"), &settings.SafetyFactor, 1.f, 4.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("safety_factor_tooltip"), "Required texture edge = size of the object on screen times this factor. Higher keeps more detail and saves less VRAM."));

	static constexpr std::array<float, 5> kEdges{ 256.f, 512.f, 1024.f, 2048.f, 4096.f };
	static constexpr std::array<const char*, 5> kEdgeLabels{ "256", "512", "1024", "2048", "4096" };
	int edgeIndex = 2;
	for (int i = 0; i < static_cast<int>(kEdges.size()); ++i) {
		if (kEdges[i] == settings.MinEdge)
			edgeIndex = i;
	}
	if (ImGui::Combo(T(TKEY("min_edge"), "Minimum Edge"), &edgeIndex, kEdgeLabels.data(), static_cast<int>(kEdgeLabels.size())))
		settings.MinEdge = kEdges[edgeIndex];
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("min_edge_tooltip"), "Textures are never reduced below this edge length."));

	ImGui::SliderFloat(T(TKEY("scan_budget_ms"), "Scan Budget"), &settings.ScanBudgetMs, 0.05f, 2.f, "%.2f ms", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("scan_budget_ms_tooltip"), "Main-thread time per frame spent walking the scene to measure how large each texture appears."));

	ImGui::SliderFloat(T(TKEY("ram_cache_mb"), "RAM Buffer"), &settings.RamCacheMB, 0.f, 4096.f, "%.0f MB", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("ram_cache_mb_tooltip"), "Texture data reloaded more than once stays in RAM up to this size, so textures that go back and forth skip the disk. 0 disables it."));

	ImGui::InputText(T(TKEY("exclude"), "Excluded Path Parts"), &settings.Exclude);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("exclude_tooltip"), "Comma-separated, lowercase path fragments that are never reduced."));

	ImGui::SeparatorText(T(TKEY("diagnostics"), "Diagnostics"));

	ImGui::Checkbox(T(TKEY("detailed_log"), "Detailed Log"), &settings.DetailedLog);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("detailed_log_tooltip"), "Write a full report every 10 seconds instead of one summary line per minute."));

	if (ImGui::Button(T(TKEY("probe_crosshair"), "Log Textures Under Crosshair")))
		RequestCenterProbe();
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("probe_crosshair_tooltip"), "Writes the path and current size of every texture you are looking at to the log. Handy for bug reports."));

	if (!g_loaderHooked)
		ImGui::TextDisabled("%s", T(TKEY("loader_unavailable"), "Loading at remembered size is inactive: the DDS loader hook could not be installed."));

	const auto status = GetStatus();
	ImGui::SeparatorText(T(TKEY("status"), "Status"));
	if (ImGui::BeginTable("TextureStreamingStatus", 2, ImGuiTableFlags_SizingStretchProp)) {
		const auto row = [](const char* a_label, const std::string& a_value) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(a_label);
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(a_value.c_str());
		};
		if (status.vramBudget > 0)
			row(T(TKEY("status_vram"), "VRAM used / budget"), std::format("{:.2f} / {:.2f} GB ({:.0f} %)", status.vramUsage / 1073741824.0, status.vramBudget / 1073741824.0, status.vramPercent));
		else
			row(T(TKEY("status_vram"), "VRAM used / budget"), T(TKEY("status_unknown"), "unknown"));
		row(T(TKEY("status_shared"), "Spilled to system RAM"), std::format("{:.0f} MB", status.sharedBytes / 1048576.0));
		row(T(TKEY("status_pressure"), "Downscaling"), status.pressure ? T(TKEY("status_active"), "active") : T(TKEY("status_idle"), "idle"));
		row(T(TKEY("status_reduced"), "Reduced textures"), std::format("{} / {}", status.reduced, status.managed));
		row(T(TKEY("status_saved"), "VRAM saved"), std::format("{:.0f} MB", status.savedBytes / 1048576.0));
		row(T(TKEY("status_queue"), "Reload queue"), std::to_string(status.queued));
		row(T(TKEY("status_remembered"), "Remembered sizes"), std::to_string(status.remembered));
		row(T(TKEY("status_ram_cache"), "RAM buffer"), std::format("{:.0f} MB", status.ramCacheBytes / 1048576.0));
		ImGui::EndTable();
	}
}

TextureStreaming::Status TextureStreaming::GetStatus()
{
	Status status;
	status.vramUsage = g_vramUsage.load(std::memory_order_relaxed);
	status.vramBudget = g_vramBudget.load(std::memory_order_relaxed);
	status.vramPercent = g_vramPct.load(std::memory_order_relaxed);
	status.sharedBytes = g_procShared.load(std::memory_order_relaxed);
	status.managed = static_cast<std::uint32_t>(g_tex.size());
	status.reduced = g_reducedCount;
	status.savedBytes = g_savedBytes;
	status.pressure = Pressure();
	{
		std::scoped_lock lock(g_qLock);
		status.queued = static_cast<std::uint32_t>(g_jobs.size());
	}
	{
		std::scoped_lock lock(g_sizeLock);
		status.remembered = static_cast<std::uint32_t>(g_loadEdge.size());
	}
	{
		std::scoped_lock lock(g_cacheLock);
		status.ramCacheBytes = g_cacheBytes;
	}
	return status;
}

void TextureStreaming::Load()
{
	LoadSizes();
	SKSE::GetMessagingInterface()->RegisterListener(MessageHandler);

	auto& func = Hooks::CreateTextureFromDDS::func;
	func = REL::RelocationID(75721, 77533).address();
	LONG result = DetourTransactionBegin();
	if (result == NO_ERROR) {
		result = DetourUpdateThread(GetCurrentThread());
		if (result == NO_ERROR)
			result = DetourAttach(reinterpret_cast<PVOID*>(&func), reinterpret_cast<PVOID>(Hooks::CreateTextureFromDDS::thunk));
		if (result == NO_ERROR)
			result = DetourTransactionCommit();
		else
			DetourTransactionAbort();
	}
	g_loaderHooked = result == NO_ERROR;
	if (g_loaderHooked)
		logger::info("[Texture Streaming] Installed DDS loader hook");
	else
		logger::warn("[Texture Streaming] DDS loader hook failed (Detours error {}); loading at remembered size inactive", result);
}

void TextureStreaming::DataLoaded()
{
	stl::write_vfunc<0x1A, Hooks::BSShaderResourceManager_CreateRendererTexture>(RE::VTABLE_BSShaderResourceManager[0]);
	MenuOpenCloseEventHandler::Register();
	if (!g_device) {
		g_device = globals::d3d::device;
		g_context = globals::d3d::context;
		if (g_device) {
			InitAdapter();
			UpdateVram();
		}
	}
	EnsureWorker();
	logger::info("[Texture Streaming] Installed renderer texture hook");
}

void TextureStreaming::EarlyPrepass()
{
	OnFrame();
}

void TextureStreaming::Reset(const char* a_reason)
{
	std::size_t jobs = 0;
	{
		std::scoped_lock lock(g_qLock);
		jobs = g_jobs.size();
		g_jobs.clear();
	}
	const auto held = std::ranges::count_if(g_tex, [](const auto& a_e) { return static_cast<bool>(a_e.second.hold); });
	g_stack.clear();
	g_passActive = false;
	g_down.clear();
	g_tex.clear();
	g_reducedCount = 0;
	g_savedBytes = 0;
	g_lastLoad = Clock::now();
	g_lastLoadTicks.store(g_lastLoad.time_since_epoch().count(), std::memory_order_relaxed);
	g_inflightUp = 0;
	g_saveRequested.store(true, std::memory_order_relaxed);
	logger::info("[Texture Streaming] Reset ({}): released {} held textures and the scan, dropped {} jobs", a_reason, held, jobs);
}

void TextureStreaming::RequestCenterProbe()
{
	g_probeCenter = true;
}

RE::BSEventNotifyControl TextureStreaming::MenuOpenCloseEventHandler::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	if (a_event && a_event->opening && (a_event->menuName == RE::MainMenu::MENU_NAME || a_event->menuName == RE::LoadingMenu::MENU_NAME))
		globals::features::textureStreaming.Reset(a_event->menuName == RE::MainMenu::MENU_NAME ? "main menu" : "loading screen");
	return RE::BSEventNotifyControl::kContinue;
}

void TextureStreaming::MenuOpenCloseEventHandler::Register()
{
	static MenuOpenCloseEventHandler singleton;
	auto ui = globals::game::ui;
	if (!ui) {
		logger::error("[Texture Streaming] UI event source not found");
		return;
	}
	ui->GetEventSource<RE::MenuOpenCloseEvent>()->AddEventSink(&singleton);
}

std::int32_t TextureStreaming::Hooks::CreateTextureFromDDS::thunk(void* a_device, void* a_stream, void** a_out, void* a_header, std::uint64_t a_maxSize, std::uint64_t a_6)
{
	t_loadPath.clear();
	bool changed = false;
	if (const auto ours = LoadMaxSize(); ours != 0 && (a_maxSize == 0 || ours < a_maxSize)) {
		a_maxSize = ours;
		changed = true;
	}
	const auto result = func(a_device, a_stream, a_out, a_header, a_maxSize, a_6);
	if (changed && result < 0 && !t_loadPath.empty()) {
		{
			std::scoped_lock lock(g_sizeLock);
			g_neverReduce.insert(t_loadPath);
			g_loadEdge.erase(t_loadPath);
			g_sizesDirty = true;
		}
		logger::warn("[Texture Streaming] loading with maxsize {} failed (0x{:X}); never loading reduced again: {}", a_maxSize, static_cast<std::uint32_t>(result), t_loadPath);
	}
	return result;
}

void TextureStreaming::Hooks::BSShaderResourceManager_CreateRendererTexture::thunk(void* a_manager, RE::NiSourceTexture* a_texture)
{
	const auto prev = t_loadingSrc;
	t_loadingSrc = a_texture;
	func(a_manager, a_texture);
	t_loadingSrc = prev;
}
