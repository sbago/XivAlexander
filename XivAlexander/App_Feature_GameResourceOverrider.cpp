#include "pch.h"
#include "App_Feature_GameResourceOverrider.h"

#include <XivAlexanderCommon/Sqex_Excel_Generator.h>
#include <XivAlexanderCommon/Sqex_Excel_Reader.h>
#include <XivAlexanderCommon/Sqex_FontCsv_CreateConfig.h>
#include <XivAlexanderCommon/Sqex_FontCsv_Creator.h>
#include <XivAlexanderCommon/Sqex_Sqpack_Creator.h>
#include <XivAlexanderCommon/Sqex_Sqpack_Reader.h>
#include <XivAlexanderCommon/Utils_Win32_Process.h>
#include <XivAlexanderCommon/Utils_Win32_ThreadPool.h>
#include "App_ConfigRepository.h"
#include "App_Misc_DebuggerDetectionDisabler.h"
#include "App_Misc_Hooks.h"
#include "App_Misc_Logger.h"
#include "App_Window_ProgressPopupWindow.h"
#include "DllMain.h"

std::weak_ptr<App::Feature::GameResourceOverrider::Implementation> App::Feature::GameResourceOverrider::s_pImpl;

class ReEnterPreventer {
	std::mutex m_lock;
	std::set<DWORD> m_tids;

public:
	class Lock {
		ReEnterPreventer& p;
		bool re = false;

	public:
		explicit Lock(ReEnterPreventer& p)
			: p(p) {
			const auto tid = GetCurrentThreadId();

			std::lock_guard lock(p.m_lock);
			re = p.m_tids.find(tid) != p.m_tids.end();
			if (!re)
				p.m_tids.insert(tid);
		}

		Lock(const Lock&) = delete;
		Lock& operator=(const Lock&) = delete;
		Lock(Lock&& r) = delete;
		Lock& operator=(Lock&&) = delete;

		~Lock() {
			if (!re) {
				std::lock_guard lock(p.m_lock);
				p.m_tids.erase(GetCurrentThreadId());
			}
		}

		operator bool() const {
			// True if new enter
			return !re;
		}
	};
};

static std::map<void*, size_t>* s_TestVal;

__declspec(dllexport) std::map<void*, size_t>* GetHeapTracker() {
	return s_TestVal;
}

struct App::Feature::GameResourceOverrider::Implementation {
	const std::shared_ptr<Config> m_config;
	const std::shared_ptr<Misc::Logger> m_logger;
	const std::shared_ptr<Misc::DebuggerDetectionDisabler> m_debugger;
	std::vector<std::unique_ptr<Misc::Hooks::PointerFunction<uint32_t, uint32_t, const char*, size_t>>> fns{};
	std::set<std::string> m_alreadyLogged{};
	Utils::CallOnDestruction::Multiple m_cleanup;

	Misc::Hooks::ImportedFunction<HANDLE, LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE> CreateFileW{"kernel32::CreateFileW", "kernel32.dll", "CreateFileW"};
	Misc::Hooks::ImportedFunction<BOOL, HANDLE> CloseHandle{"kernel32::CloseHandle", "kernel32.dll", "CloseHandle"};
	Misc::Hooks::ImportedFunction<BOOL, HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED> ReadFile{"kernel32::ReadFile", "kernel32.dll", "ReadFile"};
	Misc::Hooks::ImportedFunction<BOOL, HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD> SetFilePointerEx{"kernel32::SetFilePointerEx", "kernel32.dll", "SetFilePointerEx"};

	Misc::Hooks::ImportedFunction<LPVOID, HANDLE, DWORD, SIZE_T> HeapAlloc{"kernel32.dll::HeapAlloc", "kernel32.dll", "HeapAlloc"};
	Misc::Hooks::ImportedFunction<BOOL, HANDLE, DWORD, LPVOID> HeapFree{"kernel32.dll::HeapFree", "kernel32.dll", "HeapFree"};

	ReEnterPreventer m_repCreateFileW, m_repReadFile;

	const std::filesystem::path m_baseSqpackDir = Utils::Win32::Process::Current().PathOf().remove_filename() / L"sqpack";

	static constexpr int PathTypeIndex = -1;
	static constexpr int PathTypeIndex2 = -2;
	static constexpr int PathTypeInvalid = -3;

	struct OverlayedHandleData {
		Utils::Win32::Event IdentifierHandle;
		std::filesystem::path Path;
		LARGE_INTEGER FilePointer;
		std::shared_ptr<Sqex::RandomAccessStream> Stream;

		void ChooseStreamFrom(const Sqex::Sqpack::Creator::SqpackViews& views, int pathType) {
			switch (pathType) {
				case PathTypeIndex:
					Stream = views.Index;
					break;

				case PathTypeIndex2:
					Stream = views.Index2;
					break;

				default:
					if (pathType < 0 || static_cast<size_t>(pathType) >= views.Data.size())
						throw std::runtime_error("invalid #");
					Stream = views.Data[pathType];
			}
		}
	};

	std::mutex m_virtualPathMapMutex;
	std::map<std::filesystem::path, Sqex::Sqpack::Creator::SqpackViews> m_sqpackViews;
	std::map<HANDLE, std::unique_ptr<OverlayedHandleData>> m_overlayedHandles;
	std::set<std::filesystem::path> m_ignoredIndexFiles;
	std::atomic<int> m_stk;

	std::mutex m_processHeapAllocationTrackerMutex;
	std::map<void*, size_t> m_processHeapAllocations;

	class AtomicIntEnter {
		std::atomic<int>& v;
	public:
		AtomicIntEnter(std::atomic<int>& v)
			: v(v) {
			++v;
		}

		~AtomicIntEnter() {
			--v;
		}
	};

	Implementation()
		: m_config(Config::Acquire())
		, m_logger(Misc::Logger::Acquire())
		, m_debugger(Misc::DebuggerDetectionDisabler::Acquire()) {

		if (m_config->Runtime.UseResourceOverriding) {
			const auto hDefaultHeap = GetProcessHeap();

			m_cleanup += HeapAlloc.SetHook([this, hDefaultHeap](HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes) {
				const auto res = HeapAlloc.bridge(hHeap, dwFlags, dwBytes);
				if (res && hHeap == hDefaultHeap) {
					std::lock_guard lock(m_processHeapAllocationTrackerMutex);
					m_processHeapAllocations[res] = dwBytes;
				}
				return res;
			});

			m_cleanup += HeapFree.SetHook([this, hDefaultHeap](HANDLE hHeap, DWORD dwFlags, LPVOID lpMem) {
				const auto res = HeapFree.bridge(hHeap, dwFlags, lpMem);
				if (res && hHeap == hDefaultHeap) {
					std::lock_guard lock(m_processHeapAllocationTrackerMutex);
					m_processHeapAllocations.erase(hHeap);
				}
				return res;
			});

			m_cleanup += CreateFileW.SetHook([this](
				_In_ LPCWSTR lpFileName,
				_In_ DWORD dwDesiredAccess,
				_In_ DWORD dwShareMode,
				_In_opt_ LPSECURITY_ATTRIBUTES lpSecurityAttributes,
				_In_ DWORD dwCreationDisposition,
				_In_ DWORD dwFlagsAndAttributes,
				_In_opt_ HANDLE hTemplateFile
			) {
					AtomicIntEnter implUseLock(m_stk);

					if (const auto lock = ReEnterPreventer::Lock(m_repCreateFileW); lock &&
						!(dwDesiredAccess & GENERIC_WRITE) &&
						dwCreationDisposition == OPEN_EXISTING &&
						!hTemplateFile) {
						try {
							const auto fileToOpen = std::filesystem::absolute(lpFileName);
							const auto recreatedFilePath = m_baseSqpackDir / fileToOpen.parent_path().filename() / fileToOpen.filename();
							const auto indexFile = std::filesystem::path(recreatedFilePath).replace_extension(L".index");
							const auto index2File = std::filesystem::path(recreatedFilePath).replace_extension(L".index2");
							if (exists(indexFile) && exists(index2File) && m_ignoredIndexFiles.find(indexFile) == m_ignoredIndexFiles.end()) {
								int pathType = PathTypeInvalid;

								if (fileToOpen == indexFile) {
									pathType = PathTypeIndex;
								} else if (fileToOpen == index2File) {
									pathType = PathTypeIndex2;
								} else {
									for (auto i = 0; i < 8; ++i) {
										const auto datFile = std::filesystem::path(recreatedFilePath).replace_extension(std::format(L".dat{}", i));
										if (fileToOpen == datFile) {
											pathType = i;
											break;
										}
									}
								}

								if (pathType != PathTypeInvalid) {
									if (const auto res = SetUpVirtualFile(fileToOpen, indexFile, pathType))
										return res;
								}
							}
						} catch (const Utils::Win32::Error& e) {
							m_logger->Format<LogLevel::Warning>(LogCategory::GameResourceOverrider, L"CreateFileW: {}, Message: {}", lpFileName, e.what());
						} catch (const std::exception& e) {
							m_logger->Format<LogLevel::Warning>(LogCategory::GameResourceOverrider, "CreateFileW: {}, Message: {}", lpFileName, e.what());
						}
					}

					return CreateFileW.bridge(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
				});

			m_cleanup += CloseHandle.SetHook([this](
				HANDLE handle
			) {
					AtomicIntEnter implUseLock(m_stk);

					std::unique_lock lock(m_virtualPathMapMutex);
					if (m_overlayedHandles.erase(handle))
						return 0;

					return CloseHandle.bridge(handle);
				});

			m_cleanup += ReadFile.SetHook([this](
				_In_ HANDLE hFile,
				_Out_writes_bytes_to_opt_(nNumberOfBytesToRead, *lpNumberOfBytesRead) __out_data_source(FILE) LPVOID lpBuffer,
				_In_ DWORD nNumberOfBytesToRead,
				_Out_opt_ LPDWORD lpNumberOfBytesRead,
				_Inout_opt_ LPOVERLAPPED lpOverlapped
			) {
					AtomicIntEnter implUseLock(m_stk);

					OverlayedHandleData* pvpath = nullptr;
					{
						std::lock_guard lock(m_virtualPathMapMutex);
						const auto vpit = m_overlayedHandles.find(hFile);
						if (vpit != m_overlayedHandles.end())
							pvpath = vpit->second.get();
					}

					if (const auto lock = ReEnterPreventer::Lock(m_repReadFile); pvpath && lock) {
						auto& vpath = *pvpath;
						try {
							const auto fp = lpOverlapped ? ((static_cast<uint64_t>(lpOverlapped->OffsetHigh) << 32) | lpOverlapped->Offset) : vpath.FilePointer.QuadPart;
							const auto read = vpath.Stream->ReadStreamPartial(fp, lpBuffer, nNumberOfBytesToRead);

							if (lpNumberOfBytesRead)
								*lpNumberOfBytesRead = static_cast<DWORD>(read);

							if (read != nNumberOfBytesToRead) {
								m_logger->Format<LogLevel::Warning>(LogCategory::GameResourceOverrider, L"ReadFile: {}, requested {} bytes, read {} bytes; state: {}",
									vpath.Path.filename(), nNumberOfBytesToRead, read, vpath.Stream->DescribeState());
							}

							if (lpOverlapped) {
								if (lpOverlapped->hEvent)
									SetEvent(lpOverlapped->hEvent);
								lpOverlapped->Internal = 0;
								lpOverlapped->InternalHigh = static_cast<DWORD>(read);
							} else
								vpath.FilePointer.QuadPart = fp + read;

							return TRUE;

						} catch (const Utils::Win32::Error& e) {
							if (e.Code() != ERROR_IO_PENDING)
								m_logger->Format<LogLevel::Warning>(LogCategory::GameResourceOverrider, L"ReadFile: {}, Message: {}",
									vpath.Path.filename(), e.what());
							SetLastError(e.Code());
							return FALSE;

						} catch (const std::exception& e) {
							m_logger->Format<LogLevel::Warning>(LogCategory::GameResourceOverrider, L"ReadFile: {}, Message: {}",
								vpath.Path.filename(), e.what());
							SetLastError(ERROR_READ_FAULT);
							return FALSE;
						}
					}
					return ReadFile.bridge(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
				});

			m_cleanup += SetFilePointerEx.SetHook([this](
				_In_ HANDLE hFile,
				_In_ LARGE_INTEGER liDistanceToMove,
				_Out_opt_ PLARGE_INTEGER lpNewFilePointer,
				_In_ DWORD dwMoveMethod) {
					AtomicIntEnter implUseLock(m_stk);

					OverlayedHandleData* pvpath = nullptr;
					{
						std::lock_guard lock(m_virtualPathMapMutex);
						const auto vpit = m_overlayedHandles.find(hFile);
						if (vpit != m_overlayedHandles.end())
							pvpath = vpit->second.get();
					}
					if (!pvpath)
						return SetFilePointerEx.bridge(hFile, liDistanceToMove, lpNewFilePointer, dwMoveMethod);

					auto& vpath = *pvpath;
					try {
						const auto len = vpath.Stream->StreamSize();

						if (dwMoveMethod == FILE_BEGIN)
							vpath.FilePointer.QuadPart = liDistanceToMove.QuadPart;
						else if (dwMoveMethod == FILE_CURRENT)
							vpath.FilePointer.QuadPart += liDistanceToMove.QuadPart;
						else if (dwMoveMethod == FILE_END)
							vpath.FilePointer.QuadPart = len - liDistanceToMove.QuadPart;
						else {
							SetLastError(ERROR_INVALID_PARAMETER);
							return FALSE;
						}

						if (vpath.FilePointer.QuadPart > static_cast<int64_t>(len))
							vpath.FilePointer.QuadPart = static_cast<int64_t>(len);

						if (lpNewFilePointer)
							*lpNewFilePointer = vpath.FilePointer;

					} catch (const Utils::Win32::Error& e) {
						m_logger->Format<LogLevel::Warning>(LogCategory::GameResourceOverrider, L"SetFilePointerEx: {}, Message: {}",
							vpath.Path.filename(), e.what());
						SetLastError(e.Code());
						return FALSE;

					} catch (const std::exception& e) {
						m_logger->Format<LogLevel::Warning>(LogCategory::GameResourceOverrider, L"ReadFile: {}, Message: {}",
							vpath.Path.filename(), e.what());
						SetLastError(ERROR_READ_FAULT);
						return FALSE;
					}

					return TRUE;

				});
		}

		for (auto ptr : Misc::Signatures::LookupForData([](const IMAGE_SECTION_HEADER& p) {
					return strncmp(reinterpret_cast<const char*>(p.Name), ".text", 5) == 0;
				},
				"\x40\x57\x48\x8d\x3d\x00\x00\x00\x00\x00\x8b\xd8\x4c\x8b\xd2\xf7\xd1\x00\x85\xc0\x74\x25\x41\xf6\xc2\x03\x74\x1f\x41\x0f\xb6\x12\x8b\xc1",
				"\xFF\xFF\xFF\xFF\xFF\x00\x00\x00\x00\x00\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x00\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF",
				34,
				{}
			)) {
			fns.emplace_back(std::make_unique<Misc::Hooks::PointerFunction<uint32_t, uint32_t, const char*, size_t>>(
				"FFXIV::GeneralHashCalcFn",
				reinterpret_cast<uint32_t(__stdcall*)(uint32_t, const char*, size_t)>(ptr)
			));
			m_cleanup += fns.back()->SetHook([this, ptr, self = fns.back().get()](uint32_t initVal, const char* str, size_t len) {
				if (!str || !*str)
					return self->bridge(initVal, str, len);

				auto name = std::string(str);
				std::string ext, rest;
				if (const auto i1 = name.find_first_of('.'); i1 != std::string::npos) {
					ext = name.substr(i1);
					name.resize(i1);
					if (const auto i2 = ext.find_first_of('.', 1); i2 != std::string::npos) {
						rest = ext.substr(i2);
						ext.resize(i2);
					}
				}

				if (m_config->Runtime.HashTrackerLanguageOverride != Sqex::Language::Unspecified) {
					const char* languageCodes[] = { "ja", "en", "de", "fr", "chs", "cht", "ko" };
					const auto targetLanguageCode = languageCodes[static_cast<int>(m_config->Runtime.HashTrackerLanguageOverride.Value()) - 1];
					
					std::string nameLower = name;
					std::ranges::transform(nameLower, nameLower.begin(), [](char c) {return static_cast<char>(std::tolower(c)); });
					std::string newName;
					if (nameLower.starts_with("ui/uld/logo")) {
						// do nothing, as overriding this often freezes the game
					} else {
						for (const auto languageCode : languageCodes) {
							char t[16];
							sprintf_s(t, "_%s", languageCode);
							if (nameLower.ends_with(t)) {
								newName = name.substr(0, name.size() - strlen(languageCode)) + targetLanguageCode;
								break;
							}
							sprintf_s(t, "/%s/", languageCode);
							if (const auto pos = nameLower.find(t); pos != std::string::npos) {
								newName = std::format("{}/{}/{}", name.substr(0, pos), targetLanguageCode, name.substr(pos + strlen(t)));
								break;
							}
						}
					}
					if (!newName.empty()) {
						const auto verifyTarget = Sqex::Sqpack::EntryPathSpec(std::format("{}{}", newName, ext));
						auto found = false;
						for (const auto& t : m_sqpackViews | std::views::values) {
							found = t.EntryOffsets.find(verifyTarget) != t.EntryOffsets.end();
							if (found)
								break;
						}

						if (found) {
							name = newName;
							const auto newStr = std::format("{}{}{}", name, ext, rest);
							if (!m_config->Runtime.UseHashTrackerKeyLogging) {
								if (m_alreadyLogged.find(name) == m_alreadyLogged.end()) {
									m_alreadyLogged.emplace(name);
									m_logger->Format(LogCategory::GameResourceOverrider, "{:x}: {} => {}", reinterpret_cast<size_t>(ptr), str, newStr);
								}
							}
							Utils::Win32::Process::Current().WriteMemory(const_cast<char*>(str), newStr.c_str(), newStr.size() + 1, true);
							len = newStr.size();
						}
					}
				}
				const auto res = self->bridge(initVal, str, len);

				if (m_config->Runtime.UseHashTrackerKeyLogging) {
					if (m_alreadyLogged.find(name) == m_alreadyLogged.end()) {
						m_alreadyLogged.emplace(name);
						m_logger->Format(LogCategory::GameResourceOverrider, "{:x}: {},{},{} => {:08x}", reinterpret_cast<size_t>(ptr), name, ext, rest, res);
					}
				}

				return res;
			});
		}
	}

	~Implementation() {
		m_cleanup.Clear();

		while (m_stk) {
			Sleep(1);
		}
		Sleep(1);
	}

	HANDLE SetUpVirtualFile(const std::filesystem::path& fileToOpen, const std::filesystem::path& indexFile, int pathType) {
		auto overlayedHandle = std::make_unique<OverlayedHandleData>(Utils::Win32::Event::Create(), fileToOpen, LARGE_INTEGER{}, nullptr);

		std::lock_guard lock(m_virtualPathMapMutex);
		for (const auto& [k, v] : m_sqpackViews) {
			if (equivalent(k, indexFile)) {
				overlayedHandle->ChooseStreamFrom(v, pathType);
				break;
			}
		}

		m_logger->Format<LogLevel::Info>(LogCategory::GameResourceOverrider,
			"Taking control of {}/{} (parent: {}/{}, type: {})",
			fileToOpen.parent_path().filename(), fileToOpen.filename(),
			indexFile.parent_path().filename(), indexFile.filename(),
			pathType);

		if (!overlayedHandle->Stream) {
			if (pathType != PathTypeIndex && pathType != PathTypeIndex2) {
				m_ignoredIndexFiles.insert(indexFile);
				m_logger->Format<LogLevel::Info>(LogCategory::GameResourceOverrider,
					"=> Ignoring, because the game is accessing dat file without accessing either index or index2 file.");
				return nullptr;
			}

			auto creator = Sqex::Sqpack::Creator(
				indexFile.parent_path().filename().string(),
				indexFile.filename().replace_extension().replace_extension().string()
			);
			if (const auto result = creator.AddEntriesFromSqPack(indexFile, true, true);
				!result.Added.empty() || !result.Replaced.empty()) {
				m_logger->Format<LogLevel::Info>(LogCategory::GameResourceOverrider,
					"=> Processed SqPack {}/{}: Added {}, replaced {}, ignored {}, error {}",
					creator.DatExpac, creator.DatName,
					result.Added.size(), result.Replaced.size(), result.SkippedExisting.size(), result.Error.size());
				for (const auto& [pathSpec, errorMessage] : result.Error) {
					m_logger->Format<LogLevel::Warning>(LogCategory::GameResourceOverrider,
						"\t=> Error processing {}: {}", pathSpec, errorMessage);
				}
			}

			auto additionalEntriesFound = false;
			additionalEntriesFound |= SetUpVirtualFileFromExternalSqpacks(creator, indexFile);
			additionalEntriesFound |= SetUpVirtualFileFromTexToolsModPacks(creator, indexFile);
			additionalEntriesFound |= SetUpVirtualFileFromFileEntries(creator, indexFile);
			additionalEntriesFound |= SetUpVirtualFileFromFontConfig(creator, indexFile);

			// Nothing to override, 
			if (!additionalEntriesFound) {
				m_ignoredIndexFiles.insert(indexFile);
				m_logger->Format<LogLevel::Info>(LogCategory::GameResourceOverrider,
					"=> Found no resources to override, releasing control.");
				return nullptr;
			}

			auto res = creator.AsViews(false);
			overlayedHandle->ChooseStreamFrom(res, pathType);
			m_sqpackViews.emplace(indexFile, std::move(res));
		}

		const auto key = static_cast<HANDLE>(overlayedHandle->IdentifierHandle);
		m_overlayedHandles.insert_or_assign(key, std::move(overlayedHandle));
		SetLastError(0);
		return key;
	}

	bool SetUpVirtualFileFromExternalSqpacks(Sqex::Sqpack::Creator& creator, const std::filesystem::path& indexFile) {
		auto changed = false;

		if (creator.DatName == "0a0000") {
			const auto cachedDir = m_config->Init.ResolveConfigStorageDirectoryPath() / "Cached" / creator.DatExpac / creator.DatName;
			if (!(exists(cachedDir / "TTMPD.mpd") && exists(cachedDir / "TTMPL.mpl"))) {
				
				std::map<std::string, int> exhTable;
				// maybe generate exl?

				for (const auto& pair : Sqex::Excel::ExlReader(*creator["exd/root.exl"]))
					exhTable.emplace(pair);

				std::vector<std::unique_ptr<Sqex::Sqpack::Reader>> readers;

				for (const auto& additionalSqpackRootDirectory : m_config->Runtime.AdditionalSqpackRootDirectories.Value()) {
					const auto file = additionalSqpackRootDirectory / indexFile.parent_path().filename() / indexFile.filename();
					if (!exists(file))
						continue;

					readers.emplace_back(std::make_unique<Sqex::Sqpack::Reader>(file));
				}
				if (readers.empty())
					return false;

				create_directories(cachedDir);

				const auto actCtx = Dll::ActivationContext().With();
				Window::ProgressPopupWindow progressWindow(Dll::FindGameMainWindow(false));
				progressWindow.UpdateMessage("Generating merged exd files...");
				progressWindow.Show();
				Utils::Win32::TpEnvironment pool;

				std::atomic_int64_t progress = 0;
				static constexpr auto ProgressMaxPerTask = 1000;
				std::atomic_int64_t maxProgress = exhTable.size() * ProgressMaxPerTask;

				progressWindow.UpdateProgress(progress, maxProgress);
				{
					const auto ttmpl = Utils::Win32::File::Create(cachedDir / "TTMPL.mpl.tmp", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0);
					const auto ttmpd = Utils::Win32::File::Create(cachedDir / "TTMPD.mpd.tmp", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0);
					uint64_t ttmplPtr = 0, ttmpdPtr = 0;
					std::mutex writeMtx;

					const auto compressThread = Utils::Win32::Thread(L"CompressThread", [&]() {
						for (const auto& exhName : exhTable | std::views::keys) {
							pool.SubmitWork([&]() {
								if (progressWindow.GetCancelEvent().Wait(0) == WAIT_OBJECT_0)
									return;

								size_t progressIndex = 0;
								std::vector<uint64_t> progresses(readers.size() + 2);
								uint64_t lastAddedProgress = 0;
								const auto publishProgress = [&]() {
									const auto addProgress = std::accumulate(progresses.begin(), progresses.end(), 0ULL) / (readers.size() + 2);
									progress += addProgress - lastAddedProgress;
									lastAddedProgress = addProgress;
								};

								const auto exhPath = Sqex::Sqpack::EntryPathSpec(std::format("exd/{}.exh", exhName));
								std::unique_ptr<Sqex::Excel::Depth2ExhExdCreator> exCreator;
								{
									const auto exhReaderSource = Sqex::Excel::ExhReader(exhName, *creator[exhPath]);
									if (exhReaderSource.Header.Depth != Sqex::Excel::Exh::Depth::Level2) {
										progress += ProgressMaxPerTask;
										return;
									}

									if (std::ranges::find(exhReaderSource.Languages, Sqex::Language::Unspecified) != exhReaderSource.Languages.end()) {
										progress += ProgressMaxPerTask;
										return;
									}

									m_logger->Format<LogLevel::Info>(LogCategory::GameResourceOverrider,
										"=> Merging {}", exhName);

									exCreator = std::make_unique<Sqex::Excel::Depth2ExhExdCreator>(exhName, *exhReaderSource.Columns, exhReaderSource.Header.SomeSortOfBufferSize);
									exCreator->FillMissingLanguageFrom = Sqex::Language::English;  // TODO: make it into option

									uint64_t localProgress = 0;
									for (const auto language : exhReaderSource.Languages) {
										for (const auto& page : exhReaderSource.Pages) {
											progresses[progressIndex] = ProgressMaxPerTask * localProgress++ / (exhReaderSource.Languages.size() * exhReaderSource.Pages.size());
											localProgress++;
											publishProgress();

											const auto exdPathSpec = exhReaderSource.GetDataPathSpec(page, language);
											try {
												const auto exdReader = Sqex::Excel::ExdReader(exhReaderSource, creator[exdPathSpec]);
												exCreator->AddLanguage(language);
												for (const auto i : exdReader.GetIds())
													exCreator->SetRow(i, language, exdReader.ReadDepth2(i));
											} catch (const std::out_of_range&) {
												// pass
											}
										}
									}
									progresses[progressIndex++] = ProgressMaxPerTask;
									publishProgress();
								}

								for (const auto& reader : readers) {
									try {
										const auto exhReaderCurrent = Sqex::Excel::ExhReader(exhName, *(*reader)[exhPath]);

										uint64_t localProgress = 0;
										for (const auto language : exhReaderCurrent.Languages) {
											for (const auto& page : exhReaderCurrent.Pages) {
												progresses[progressIndex] = ProgressMaxPerTask * localProgress++ / (exhReaderCurrent.Languages.size() * exhReaderCurrent.Pages.size());
												localProgress++;
												publishProgress();

												const auto exdPathSpec = exhReaderCurrent.GetDataPathSpec(page, language);
												try {
													const auto exdReader = Sqex::Excel::ExdReader(exhReaderCurrent, (*reader)[exdPathSpec]);
													exCreator->AddLanguage(language);
													for (const auto i : exdReader.GetIds()) {
														auto cols = exCreator->GetRow(i, Sqex::Language::Japanese);
														auto colRef2 = exCreator->GetRow(i, Sqex::Language::English);
														auto colRef3 = exCreator->GetRow(i, Sqex::Language::German);
														auto cols2 = exdReader.ReadDepth2(i);
														for (size_t j = 0; j < cols.size() && j < cols2.size(); ++j) {
															if (cols[j].Type != Sqex::Excel::Exh::String)
																continue;
															if (cols[j].String == colRef2[j].String && cols[j].String == colRef3[j].String)
																continue;
															if (cols2[j].String.empty())
																continue;
															cols[j].String = cols2[j].String;
														}
														exCreator->SetRow(i, language, cols);
														// exCreator->SetRow(i, Sqex::Language::Japanese, cols);
													}
												} catch (const std::out_of_range&) {
													// pass
												}
											}
										}
									} catch (const std::out_of_range&) {
										// pass
									}
									progresses[progressIndex++] = ProgressMaxPerTask;
									publishProgress();
								}

								{
									auto compiled = exCreator->Compile();
									m_logger->Format<LogLevel::Info>(LogCategory::GameResourceOverrider,
										"=> Saving {}", exhName);

									uint64_t localProgress = 0;
									for (auto& [entryPathSpec, data] : compiled) {
										progresses[progressIndex] = ProgressMaxPerTask * localProgress / compiled.size();
										publishProgress();

										const auto targetPath = cachedDir / entryPathSpec.Original;

										const auto provider = std::make_shared<Sqex::Sqpack::MemoryBinaryEntryProvider>(entryPathSpec, std::make_shared<Sqex::MemoryRandomAccessStream>(std::move(*reinterpret_cast<std::vector<uint8_t>*>(&data))));
										const auto len = provider->StreamSize();
										const auto dv = provider->ReadStreamIntoVector<char>(0, len);

										if (progressWindow.GetCancelEvent().Wait(0) == WAIT_OBJECT_0)
											return;
										const auto lock = std::lock_guard(writeMtx);
										const auto entryLine = std::format("{}\n", nlohmann::json::object({
											{"FullPath", Utils::ToUtf8(entryPathSpec.Original.wstring())},
											{"ModOffset", ttmpdPtr},
											{"ModSize", len},
											{"DatFile", "0a0000"},
											}).dump());
										ttmplPtr += ttmpl.Write(ttmplPtr, std::span(entryLine));
										ttmpdPtr += ttmpd.Write(ttmpdPtr, std::span(dv));
									}
									progresses[progressIndex++] = ProgressMaxPerTask;
									publishProgress();
								}
							});
						}
						pool.WaitOutstanding();
					});
					while (true) {
						if (WAIT_TIMEOUT != progressWindow.DoModalLoop(100, { compressThread }))
							break;

						progressWindow.UpdateProgress(progress, maxProgress);
					}
					pool.Cancel();
					compressThread.Wait();
				}

				if (progressWindow.GetCancelEvent().Wait(0) == WAIT_OBJECT_0) {
					try {
						std::filesystem::remove(cachedDir / "TTMPL.mpl.tmp");
					} catch (...) {
						// whatever
					}
					try {
						std::filesystem::remove(cachedDir / "TTMPD.mpd.tmp");
					} catch (...) {
						// whatever
					}
					return false;
				}

				std::filesystem::rename(cachedDir / "TTMPL.mpl.tmp", cachedDir / "TTMPL.mpl");
				std::filesystem::rename(cachedDir / "TTMPD.mpd.tmp", cachedDir / "TTMPD.mpd");
			}

			try {
				const auto logCacher = creator.Log([&](const auto& s) {
					m_logger->Format<LogLevel::Info>(LogCategory::GameResourceOverrider, "=> {}", s);
				});
				const auto result = creator.AddEntriesFromTTMP(cachedDir);
				if (!result.Added.empty() || !result.Replaced.empty())
					changed = true;
			} catch (const std::exception& e) {
				m_logger->Format<LogLevel::Warning>(LogCategory::GameResourceOverrider, "=> Error: {}", e.what());
			}
		} else {
			for (const auto& additionalSqpackRootDirectory : m_config->Runtime.AdditionalSqpackRootDirectories.Value()) {
				const auto file = additionalSqpackRootDirectory / indexFile.parent_path().filename() / indexFile.filename();
				if (!exists(file))
					continue;
				
				const auto batchAddResult = creator.AddEntriesFromSqPack(file, false, false);

				if (!batchAddResult.Added.empty()) {
					changed = true;
					m_logger->Format<LogLevel::Info>(LogCategory::GameResourceOverrider,
						"=> Processed external SqPack {}: Added {}",
						file, batchAddResult.Added.size());
					for (const auto& [pathSpec, errorMessage] : batchAddResult.Error) {
						m_logger->Format<LogLevel::Warning>(LogCategory::GameResourceOverrider,
							"\t=> Error processing {}: {}", pathSpec, errorMessage);
					}
				}
			}
		}

		return changed;
	}

	bool SetUpVirtualFileFromTexToolsModPacks(Sqex::Sqpack::Creator& creator, const std::filesystem::path& indexPath) {
		auto additionalEntriesFound = false;
		std::vector<std::filesystem::path> dirs;

		if (m_config->Runtime.UseDefaultTexToolsModPackSearchDirectory) {
			dirs.emplace_back(indexPath.parent_path().parent_path().parent_path() / "TexToolsMods");
			dirs.emplace_back(m_config->Init.ResolveConfigStorageDirectoryPath() / "TexToolsMods");
		}

		for (const auto& dir : m_config->Runtime.AdditionalTexToolsModPackSearchDirectories.Value()) {
			if (!dir.empty())
				dirs.emplace_back(Config::TranslatePath(dir));
		}

		for (const auto& dir : dirs) {
			if (dir.empty() || !is_directory(dir))
				continue;

			std::vector<std::filesystem::path> files;
			try {
				for (const auto& iter : std::filesystem::recursive_directory_iterator(dir)) {
					if (iter.path().filename() != "TTMPL.mpl")
						continue;
					files.emplace_back(iter);
				}
			} catch (const std::exception& e) {
				m_logger->Format<LogLevel::Warning>(LogCategory::GameResourceOverrider,
					"=> Failed to list items in {}: {}",
					dir, e.what());
				continue;
			}

			std::ranges::sort(files);
			for (const auto& file : files) {
				m_logger->Format<LogLevel::Info>(LogCategory::GameResourceOverrider, "Processing {}", file);

				if (exists(file.parent_path() / "disable")) {
					m_logger->Format<LogLevel::Info>(LogCategory::GameResourceOverrider, "=> Disabled because \"disable\" file exists");
					continue;
				}
				try {
					const auto logCacher = creator.Log([&](const auto& s) {
						m_logger->Format<LogLevel::Info>(LogCategory::GameResourceOverrider, "=> {}", s);
					});
					const auto result = creator.AddEntriesFromTTMP(file.parent_path());
					if (!result.Added.empty() || !result.Replaced.empty())
						additionalEntriesFound = true;
				} catch (const std::exception& e) {
					m_logger->Format<LogLevel::Warning>(LogCategory::GameResourceOverrider, "=> Error: {}", e.what());
				}
			}
		}
		return additionalEntriesFound;
	}

	bool SetUpVirtualFileFromFileEntries(Sqex::Sqpack::Creator& creator, const std::filesystem::path& indexPath) {
		auto additionalEntriesFound = false;
		std::vector<std::filesystem::path> dirs;

		if (m_config->Runtime.UseDefaultGameResourceFileEntryRootDirectory) {
			dirs.emplace_back(indexPath.parent_path().parent_path());
			dirs.emplace_back(m_config->Init.ResolveConfigStorageDirectoryPath() / "ReplacementFileEntries");
		}

		for (const auto& dir : m_config->Runtime.AdditionalGameResourceFileEntryRootDirectories.Value()) {
			if (!dir.empty())
				dirs.emplace_back(Config::TranslatePath(dir));
		}

		for (size_t i = 0, i_ = dirs.size(); i < i_; ++i) {
			dirs.emplace_back(dirs[i] / std::format("{}.win32", creator.DatExpac) / creator.DatName);
			dirs[i] = dirs[i] / creator.DatExpac / creator.DatName;
		}

		for (const auto& dir : dirs) {
			if (!is_directory(dir))
				continue;

			std::vector<std::filesystem::path> files;

			try {
				for (const auto& iter : std::filesystem::recursive_directory_iterator(dir)) {
					if (is_directory(iter))
						continue;
					files.emplace_back(iter);
				}
			} catch (const std::exception& e) {
				m_logger->Format<LogLevel::Warning>(LogCategory::GameResourceOverrider,
					"=> Failed to list items in {}: {}",
					dir, e.what());
				continue;
			}

			std::ranges::sort(files);
			for (const auto& file : files) {
				if (is_directory(file))
					continue;

				try {
					const auto result = creator.AddEntryFromFile(relative(file, dir), file);
					const auto item = result.AnyItem();
					if (!item)
						throw std::runtime_error("Unexpected error");
					m_logger->Format<LogLevel::Info>(LogCategory::GameResourceOverrider,
						"=> {} file {}: (nameHash={:08x}, pathHash={:08x}, fullPathHash={:08x})",
						result.Added.empty() ? "Replaced" : "Added",
						item->PathSpec().Original,
						item->PathSpec().NameHash,
						item->PathSpec().PathHash,
						item->PathSpec().FullPathHash);
					additionalEntriesFound = true;
				} catch (const std::exception& e) {
					m_logger->Format<LogLevel::Warning>(LogCategory::GameResourceOverrider,
						"=> Failed to add file {}: {}",
						file, e.what());
				}
			}
		}
		return additionalEntriesFound;
	}

	bool SetUpVirtualFileFromFontConfig(Sqex::Sqpack::Creator& creator, const std::filesystem::path& indexPath) {
		if (indexPath.filename() != L"000000.win32.index")
			return false;

		if (const auto fontConfigPathStr = m_config->Runtime.OverrideFontConfig.Value(); !fontConfigPathStr.empty()) {
			const auto fontConfigPath = Config::TranslatePath(fontConfigPathStr);
			try {
				if (!exists(fontConfigPath))
					throw std::runtime_error(std::format("=> Font config file was not found: ", fontConfigPathStr));

				const auto cachedDir = m_config->Init.ResolveConfigStorageDirectoryPath() / "Cached" / creator.DatExpac / creator.DatName;
				if (!(exists(cachedDir / "TTMPD.mpd") && exists(cachedDir / "TTMPL.mpl"))) {
					create_directories(cachedDir);

					const auto actCtx = Dll::ActivationContext().With();
					Window::ProgressPopupWindow progressWindow(Dll::FindGameMainWindow(false));
					progressWindow.UpdateMessage("Generating fonts...");
					progressWindow.Show();

					m_logger->Format<LogLevel::Info>(LogCategory::GameResourceOverrider,
						"=> Generating font per file: {}",
						fontConfigPathStr);

					std::ifstream fin(fontConfigPath);
					nlohmann::json j;
					fin >> j;
					auto cfg = j.get<Sqex::FontCsv::CreateConfig::FontCreateConfig>();

					Sqex::FontCsv::FontSetsCreator fontCreator(cfg, Utils::Win32::Process::Current().PathOf().parent_path());
					while (true) {
						if (WAIT_TIMEOUT != progressWindow.DoModalLoop(100, { fontCreator.GetWaitableObject() }))
							break;

						const auto progress = fontCreator.GetProgress();
						progressWindow.UpdateProgress(progress.Progress, progress.Max);
						if (progress.Indeterminate)
							progressWindow.UpdateMessage(std::format("Generating fonts... ({} task(s) yet to be started)", progress.Indeterminate));
						else
							progressWindow.UpdateMessage("Generating fonts...");
					}
					if (progressWindow.GetCancelEvent().Wait(0) != WAIT_OBJECT_0) {
						progressWindow.UpdateMessage("Compressing data...");
						Utils::Win32::TpEnvironment pool;
						const auto streams = fontCreator.GetResult().GetAllStreams();

						std::atomic_int64_t progress = 0;
						uint64_t maxProgress = 0;
						for (auto& stream : streams | std::views::values)
							maxProgress += stream->StreamSize() * 2;
						progressWindow.UpdateProgress(progress, maxProgress);

						const auto ttmpl = Utils::Win32::File::Create(cachedDir / "TTMPL.mpl.tmp", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0);
						const auto ttmpd = Utils::Win32::File::Create(cachedDir / "TTMPD.mpd.tmp", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0);
						uint64_t ttmplPtr = 0, ttmpdPtr = 0;
						std::mutex writeMtx;

						const auto compressThread = Utils::Win32::Thread(L"CompressThread", [&]() {
							for (auto& [entryPathSpec, stream] : streams) {
								pool.SubmitWork([&]() {
									if (progressWindow.GetCancelEvent().Wait(0) == WAIT_OBJECT_0)
										return;

									std::shared_ptr<Sqex::Sqpack::EntryProvider> provider;
									auto extension = entryPathSpec.Original.extension().wstring();
									CharLowerW(&extension[0]);
									if (extension == L".tex")
										provider = std::make_shared<Sqex::Sqpack::MemoryTextureEntryProvider>(entryPathSpec, stream);
									else
										provider = std::make_shared<Sqex::Sqpack::MemoryBinaryEntryProvider>(entryPathSpec, stream);
									const auto len = provider->StreamSize();
									const auto dv = provider->ReadStreamIntoVector<char>(0, len);
									progress += stream->StreamSize();

									if (progressWindow.GetCancelEvent().Wait(0) == WAIT_OBJECT_0)
										return;
									const auto lock = std::lock_guard(writeMtx);
									const auto entryLine = std::format("{}\n", nlohmann::json::object({
										{"FullPath", Utils::ToUtf8(entryPathSpec.Original.wstring())},
										{"ModOffset", ttmpdPtr},
										{"ModSize", len},
										{"DatFile", "000000"},
										}).dump());
									ttmplPtr += ttmpl.Write(ttmplPtr, std::span(entryLine));
									ttmpdPtr += ttmpd.Write(ttmpdPtr, std::span(dv));
									progress += stream->StreamSize();
								});
							}
							pool.WaitOutstanding();
						});
						
						while (true) {
							if (WAIT_TIMEOUT != progressWindow.DoModalLoop(100, { compressThread }))
								break;

							progressWindow.UpdateProgress(progress, maxProgress);
						}
						pool.Cancel();
						compressThread.Wait();
					}

					if (progressWindow.GetCancelEvent().Wait(0) == WAIT_OBJECT_0) {
						try {
							std::filesystem::remove(cachedDir / "TTMPL.mpl.tmp");
						} catch (...) {
							// whatever
						}
						try {
							std::filesystem::remove(cachedDir / "TTMPD.mpd.tmp");
						} catch (...) {
							// whatever
						}
						return false;
					}

					std::filesystem::rename(cachedDir / "TTMPL.mpl.tmp", cachedDir / "TTMPL.mpl");
					std::filesystem::rename(cachedDir / "TTMPD.mpd.tmp", cachedDir / "TTMPD.mpd");
				}

				try {
					const auto logCacher = creator.Log([&](const auto& s) {
						m_logger->Format<LogLevel::Info>(LogCategory::GameResourceOverrider, "=> {}", s);
					});
					const auto result = creator.AddEntriesFromTTMP(cachedDir);
					if (!result.Added.empty() || !result.Replaced.empty())
						return true;
				} catch (const std::exception& e) {
					m_logger->Format<LogLevel::Warning>(LogCategory::GameResourceOverrider, "=> Error: {}", e.what());
				}
			} catch (const std::runtime_error& e) {
				m_logger->Format<LogLevel::Warning>(LogCategory::GameResourceOverrider, e.what());
			}
		}

		return false;
	}
};

std::shared_ptr<App::Feature::GameResourceOverrider::Implementation> App::Feature::GameResourceOverrider::AcquireImplementation() {
	auto m_pImpl = s_pImpl.lock();
	if (!m_pImpl) {
		static std::mutex mtx;
		const auto lock = std::lock_guard(mtx);
		m_pImpl = s_pImpl.lock();
		if (!m_pImpl)
			s_pImpl = m_pImpl = std::make_unique<Implementation>();
	}
	return m_pImpl;
}

App::Feature::GameResourceOverrider::GameResourceOverrider()
	: m_pImpl(AcquireImplementation()) {
}

App::Feature::GameResourceOverrider::~GameResourceOverrider() = default;

bool App::Feature::GameResourceOverrider::CanUnload() const {
	return m_pImpl->m_overlayedHandles.empty();
}
