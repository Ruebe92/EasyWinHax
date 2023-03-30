#include "launch.h"
#include "proc.h"
#include "mem.h"
#include <stdint.h>

// how long to wait for return value of launched function in milliseconds
#define LAUNCH_TIMEOUT 5000
#define PAGE_SIZE 0x1000
#define LODWORD(qword) (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(qword)))

#define LAUNCH_DATA_X64_SPACE 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#define LAUNCH_DATA_X86_SPACE 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00

namespace launch {

	typedef struct HookData {
		DWORD processId;
		HMODULE hModule;
		HOOKPROC pHookFunc;
		FARPROC pCallNextHookEx;
		HHOOK hHook;
		HWND hWnd;
	}HookData;

	// x86 specific parts of the implementations
	namespace x86 {

		static bool createRemoteThread(HANDLE hProc, tLaunchFunc pFunc, void* pArg, void* pRet);
		static bool hijackThread(HANDLE hProc, BYTE* pShellCode, HANDLE hThread, DWORD threadId, tLaunchFunc pFunc, void* pArg, void* pRet);

		#ifndef _WIN64

		static bool setWindowsHook(HANDLE hProc, BYTE* pShellCode, HookData* pHookData, tLaunchFunc pFunc, void* pArg, void* pRet);

		#endif // !_WIN64

		static bool hookBeginPaint(HANDLE hProc, BYTE* pShellCode, BYTE* pNtUserBeginPaint, tLaunchFunc pFunc, void* pArg, void* pRet);
		static bool queueUserApc(HANDLE hProc, BYTE* pShellCode, HANDLE hThread, tLaunchFunc pFunc, void* pArg, void* pRet);

	}

	#ifdef _WIN64

	// x64 specific parts of the implementations
	namespace x64 {

		static bool createRemoteThread(HANDLE hProc, BYTE* pShellCode, tLaunchFunc pFunc, void* pArg, void* pRet);
		static bool hijackThread(HANDLE hProc, BYTE* pShellCode, HANDLE hThread, DWORD threadId, tLaunchFunc pFunc, void* pArg, void* pRet);
		static bool setWindowsHook(HANDLE hProc, BYTE* pShellCode, HookData* pHookData, tLaunchFunc pFunc, void* pArg, void* pRet);
		static bool hookBeginPaint(HANDLE hProc, BYTE* pShellCode, BYTE* pNtUserBeginPaint, tLaunchFunc pFunc, void* pArg, void* pRet);
		static bool queueUserApc(HANDLE hProc, BYTE* pShellCode, HANDLE hThread, tLaunchFunc pFunc, void* pArg, void* pRet);

	}

	#endif // _WIN64


	bool createRemoteThread(HANDLE hProc, tLaunchFunc pFunc, void* pArg, void* pRet) {
		BOOL isWow64 = FALSE;
		IsWow64Process(hProc, &isWow64);

		bool success = false;

		if (isWow64) {
			
			success = x86::createRemoteThread(hProc, pFunc, pArg, pRet);

		}
		else {

			// x64 targets only feasable for x64 compilations
			#ifdef _WIN64

			// shell coding for x64 processes is done just to get the full 8 byte return value of x64 threads
			// GetExitCodeThread only gets a DWORD value
			BYTE* const pShellCode = reinterpret_cast<BYTE*>(VirtualAllocEx(hProc, nullptr, PAGE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));

			if (!pShellCode) return false;

			success = x64::createRemoteThread(hProc, pShellCode, pFunc, pArg, pRet);

			VirtualFreeEx(hProc, pShellCode, 0, MEM_RELEASE);

			#endif

		}

		return success;
	}


	bool hijackThread(HANDLE hProc, tLaunchFunc pFunc, void* pArg, void* pRet) {
		const DWORD processId = GetProcessId(hProc);

		if (!processId) return false;

		// get the first/main thread entry
		proc::ThreadEntry threadEntry{};
		proc::getProcessThreadEntries(processId, &threadEntry, 1);

		const HANDLE hThread = OpenThread(THREAD_SET_CONTEXT | THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, threadEntry.threadId);

		if (!hThread) return false;

		BYTE* const pShellCode = reinterpret_cast<BYTE*>(VirtualAllocEx(hProc, nullptr, PAGE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));

		if (!pShellCode) {
			CloseHandle(hThread);

			return false;
		}

		BOOL isWow64 = FALSE;
		IsWow64Process(hProc, &isWow64);
		
		bool success = false;

		if (isWow64) {
			
			success = x86::hijackThread(hProc, pShellCode, hThread, threadEntry.threadId, pFunc, pArg, pRet);

		}
		else {

			// x64 targets only feasable for x64 compilations
			#ifdef _WIN64

			success = x64::hijackThread(hProc, pShellCode, hThread, threadEntry.threadId, pFunc, pArg, pRet);

			#endif // _WIN64

		}

		VirtualFreeEx(hProc, pShellCode, 0, MEM_RELEASE);
		CloseHandle(hThread);

		return success;
	}


	bool setWindowsHook(HANDLE hProc, tLaunchFunc pFunc, void* pArg, void* pRet) {
		const DWORD processId = GetProcessId(hProc);

		if (!processId) return false;

		// arbitrary but has to be loaded in both caller and target process
		const HMODULE hHookedMod = proc::in::getModuleHandle("Kernel32.dll");

		if (!hHookedMod) return false;

		const HMODULE hUser32 = proc::ex::getModuleHandle(hProc, "User32.dll");

		if (!hUser32) return false;

		const FARPROC pCallNextHookEx = proc::ex::getProcAddress(hProc, hUser32, "CallNextHookEx");

		if (!pCallNextHookEx) return false;

		BYTE* const pShellCode = reinterpret_cast<BYTE*>(VirtualAllocEx(hProc, nullptr, PAGE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));

		if (!pShellCode) return false;

		HookData hookData{};
		hookData.processId = processId;
		hookData.hModule = hHookedMod;
		hookData.pHookFunc = reinterpret_cast<HOOKPROC>(pShellCode);
		hookData.pCallNextHookEx = pCallNextHookEx;

		BOOL isWow64 = FALSE;
		IsWow64Process(hProc, &isWow64);

		bool success = false;

		// installing hook only possible from process with matching architechture
		if (isWow64) {

			#ifndef _WIN64

			success = x86::setWindowsHook(hProc, pShellCode, &hookData, pFunc, pArg, pRet);

			#endif // !_WIN64

		}
		else {

			#ifdef _WIN64

			success = x64::setWindowsHook(hProc, pShellCode, &hookData, pFunc, pArg, pRet);

			#endif // _WIN64

		}

		VirtualFreeEx(hProc, pShellCode, 0, MEM_RELEASE);

		return success;
	}


	bool hookBeginPaint(HANDLE hProc, tLaunchFunc pFunc, void* pArg, void* pRet) {
		const HMODULE hNtdll = proc::ex::getModuleHandle(hProc, "win32u.dll");

		if (!hNtdll) return false;

		BYTE* const pNtUserBeginPaint = reinterpret_cast<BYTE*>(proc::ex::getProcAddress(hProc, hNtdll, "NtUserBeginPaint"));

		if (!pNtUserBeginPaint) return false;

		BYTE* const pShellCode = reinterpret_cast<BYTE*>(VirtualAllocEx(hProc, nullptr, PAGE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));

		if (!pShellCode) return false;

		BOOL isWow64 = FALSE;
		IsWow64Process(hProc, &isWow64);

		bool success = false;

		if (isWow64) {

			success = x86::hookBeginPaint(hProc, pShellCode, pNtUserBeginPaint, pFunc, pArg, pRet);

		}
		else {

			// x64 targets only feasable for x64 compilations
			#ifdef _WIN64

			success = x64::hookBeginPaint(hProc, pShellCode, pNtUserBeginPaint, pFunc, pArg, pRet);

			#endif // _WIN64

		}

		VirtualFreeEx(hProc, pShellCode, 0, MEM_RELEASE);

		return success;
	}


	bool queueUserApc(HANDLE hProc, tLaunchFunc pFunc, void* pArg, void* pRet) {
		const DWORD processId = GetProcessId(hProc);

		proc::ProcessEntry procEntry{};

		if (!proc::getProcessEntry(processId, &procEntry)) return false;

		proc::ThreadEntry* const pThreadEntries = new proc::ThreadEntry[procEntry.threadCount];

		if (!proc::getProcessThreadEntries(processId, pThreadEntries, procEntry.threadCount)) {
			delete[] pThreadEntries;

			return false;
		}

		DWORD threadId = 0;

		for (DWORD i = 0; i < procEntry.threadCount; i++) {

			// SignalObjectAndWait, MsgWaitForMultipleObjectsEx, WaitForMultipleObjectsEx, WaitForSingleObjectEx set wait reason to WrQueue
			// SleepEx sets wait reason to DelayExecution
			if (pThreadEntries[i].waitReason == KWAIT_REASON::WrQueue || pThreadEntries[i].waitReason == KWAIT_REASON::DelayExecution) {
				threadId = pThreadEntries[i].threadId;

				break;
			}

		}

		delete[] pThreadEntries;

		if (!threadId) return false;

		const HANDLE hThread = OpenThread(THREAD_SET_CONTEXT, FALSE, threadId);

		if (!hThread) return false;

		BYTE* const pShellCode = reinterpret_cast<BYTE*>(VirtualAllocEx(hProc, nullptr, PAGE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));

		if (!pShellCode) {
			CloseHandle(hThread);

			return false;
		}

		BOOL isWow64 = FALSE;
		IsWow64Process(hProc, &isWow64);

		bool success = false;

		if (isWow64) {

			success = x86::queueUserApc(hProc, pShellCode, hThread, pFunc, pArg, pRet);

		}
		else {

			// x64 targets only feasable for x64 compilations
			#ifdef _WIN64

			success = x64::queueUserApc(hProc, pShellCode, hThread, pFunc, pArg, pRet);

			#endif // _WIN64

		}

		VirtualFreeEx(hProc, pShellCode, 0, MEM_RELEASE);
		CloseHandle(hThread);

		return success;
	}


	static bool checkShellCodeFlag(HANDLE hProc, const void* pFlag);
	// callback for setWindowsHook
	static BOOL CALLBACK setHookCallback(HWND hWnd, LPARAM lParam);
	// callback to resize window for hookUserBeginPaint
	static BOOL CALLBACK resizeCallback(HWND hWnd, LPARAM lParam);

	namespace x86 {

		// struct for data within the shell code
		typedef struct LaunchData {
			uint32_t pArg;
			uint32_t pFunc;
			uint32_t pRet;
			uint8_t flag;
		}LaunchData;

		static bool createRemoteThread(HANDLE hProc, tLaunchFunc pFunc, void* pArg, void* pRet) {
			const HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(pFunc), pArg, 0, nullptr);

			if (!hThread) return false;

			if (WaitForSingleObject(hThread, LAUNCH_TIMEOUT)) {

				#pragma warning (push)
				#pragma warning (disable: 6258)
				TerminateThread(hThread, 0);
				#pragma warning (pop)

				CloseHandle(hThread);

				return false;
			}

			GetExitCodeThread(hThread, reinterpret_cast<DWORD*>(pRet));
			CloseHandle(hThread);

			return true;
		}

		// ASM:
		// push   oldEip						save old eip for return instruction
		// push   ecx							save registers
		// push   eax
		// push   edx
		// pushf
		// mov    ecx, pLaunchDataEx				
		// mov    eax, DWORD PTR [ecx+0x4]		load pLaunchData->pFunc
		// push   ecx							save pLaunchData
		// push   [ecx]							push pLaunchData->pArg for function call
		// call   eax							call pLaunchData->pFunc
		// pop    ecx							restore pLaunchData
		// mov    DWORD PTR [ecx+0x8], eax		write return value to pLaunchData->pRet
		// popf									restore registers
		// pop    edx
		// pop    eax
		// mov    BYTE PTR [ecx+0xc], 0x1		set pLaunchData->flag to one
		// pop    ecx							restore register
		// ret									return to old eip
		static BYTE hijackShell[]{ 0x68, 0x00, 0x00, 0x00, 0x00, 0x51, 0x50, 0x52, 0x9C, 0xB9, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x41, 0x04, 0x51, 0xFF, 0x31, 0xFF, 0xD0, 0x59, 0x89, 0x41, 0x08, 0x9D, 0x5A, 0x58, 0xC6, 0x41, 0x0C, 0x01, 0x59, 0xC3, LAUNCH_DATA_X86_SPACE };

		static bool hijackThread(HANDLE hProc, BYTE* pShellCode, HANDLE hThread, DWORD threadId, tLaunchFunc pFunc, void* pArg, void* pRet) {

			if (SuspendThread(hThread) == 0xFFFFFFFF) return false;

			WOW64_CONTEXT wow64Context{};
			wow64Context.ContextFlags = CONTEXT_CONTROL;

			if (!Wow64GetThreadContext(hThread, &wow64Context)) {
				ResumeThread(hThread);

				return false;
			}

			const ptrdiff_t launchDataOffset = sizeof(hijackShell) - sizeof(LaunchData);

			LaunchData* const pLaunchData = reinterpret_cast<LaunchData*>(hijackShell + launchDataOffset);
			pLaunchData->pArg = LODWORD(pArg);
			pLaunchData->pFunc = LODWORD(pFunc);

			const uint32_t oldEip = wow64Context.Eip;
			*reinterpret_cast<uint32_t*>(hijackShell + 0x01) = oldEip;

			const LaunchData* const pLaunchDataEx = reinterpret_cast<LaunchData*>(pShellCode + launchDataOffset);
			*reinterpret_cast<uint32_t*>(hijackShell + 0x0A) = LODWORD(pLaunchDataEx);

			if (!WriteProcessMemory(hProc, pShellCode, hijackShell, sizeof(hijackShell), nullptr)) {
				ResumeThread(hThread);

				return false;
			}

			wow64Context.Eip = LODWORD(pShellCode);

			if (!Wow64SetThreadContext(hThread, &wow64Context)) {
				ResumeThread(hThread);

				return false;
			}

			// post thread message to ensure thread execution
			PostThreadMessageA(threadId, 0, 0, 0);

			if (ResumeThread(hThread) == 0xFFFFFFFF) {
				wow64Context.Eip = oldEip;
				Wow64SetThreadContext(hThread, &wow64Context);
				ResumeThread(hThread);

				return false;
			}

			if (!checkShellCodeFlag(hProc, &pLaunchDataEx->flag)) {

				if (SuspendThread(hThread) != 0xFFFFFFFF) {
					wow64Context.Eip = oldEip;
					Wow64SetThreadContext(hThread, &wow64Context);
					ResumeThread(hThread);
				}

				return false;
			}

			if (!ReadProcessMemory(hProc, &pLaunchDataEx->pRet, pRet, sizeof(uint32_t), nullptr)) return false;

			return true;
		}


		#ifndef _WIN64

		// ASM:
		// push   ebp								setup stack frame
		// mov    ebp, esp
		// jmp    0x0								jump to skip pLaunchData->pFunc call after first execution
		// push   eax								save registers
		// push   ebx
		// mov    ebx, pLaunchData	
		// mov    BYTE PTR[ebx - 0x30], 0x1B		fix jump to skip pLaunchData->pFunc call
		// push   ebx								save pLaunchData
		// push   DWORD PTR[ebx]					load pLaunchData->pArg for pLaunchData->pFunc call
		// call   DWORD PTR[ebx + 0x4]
		// pop    ebx								restore pLaunchData
		// mov    DWORD PTR[ebx + 0x8], eax			write return value to pLaunchData->pRet
		// mov    BYTE PTR[ebx + 0xC], 0x1			set pLaunchData->flag to one
		// pop    ebx								restore registers for pCallNextHookEx call
		// pop    eax
		// push   DWORD PTR[ebp + 0x10]				load arguments for pCallNextHookEx call
		// push   DWORD PTR[ebp + 0xc]
		// push   DWORD PTR[ebp + 0x8]
		// push   0x0
		// call   pCallNextHook
		// pop    ebp
		// ret    0xc
		static BYTE windowsHookShell[]{ 0x55, 0x89, 0xE5, 0xEB, 0x00, 0x50, 0x53, 0xBB, 0x00, 0x00, 0x00, 0x00, 0xC6, 0x43, 0xD0, 0x1B, 0x53, 0xFF, 0x33, 0xFF, 0x53, 0x04, 0x5B, 0x89, 0x43, 0x08, 0xC6, 0x43, 0x0C, 0x01, 0x5B, 0x58, 0xFF, 0x75, 0x10, 0xFF, 0x75, 0x0C, 0xFF, 0x75, 0x08, 0x6A, 0x00, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x5D, 0xC2, 0x0C, 0x00, LAUNCH_DATA_X86_SPACE };

		static bool setWindowsHook(HANDLE hProc, BYTE* pShellCode, HookData* pHookData, tLaunchFunc pFunc, void* pArg, void* pRet) {
			const ptrdiff_t launchDataOffset = sizeof(windowsHookShell) - sizeof(LaunchData);

			LaunchData* const pLaunchData = reinterpret_cast<LaunchData*>(windowsHookShell + launchDataOffset);
			pLaunchData->pArg = reinterpret_cast<uint32_t>(pArg);
			pLaunchData->pFunc = reinterpret_cast<uint32_t>(pFunc);

			const LaunchData* const pLaunchDataEx = reinterpret_cast<LaunchData*>(pShellCode + launchDataOffset);
			*reinterpret_cast<uint32_t*>(windowsHookShell + 0x08) = reinterpret_cast<uint32_t>(pLaunchDataEx);
			*reinterpret_cast<uint32_t*>(windowsHookShell + 0x2C) = reinterpret_cast<uint32_t>(pHookData->pCallNextHookEx) - (reinterpret_cast<uint32_t>(pShellCode) + 0x30);

			if (!WriteProcessMemory(hProc, pShellCode, windowsHookShell, sizeof(windowsHookShell), nullptr)) return false;

			EnumWindows(setHookCallback, reinterpret_cast<LPARAM>(pHookData));

			if (!pHookData->hHook || !pHookData->hWnd) return false;

			// foreground window to activate the hook
			const HWND hFgWnd = GetForegroundWindow();
			SetForegroundWindow(pHookData->hWnd);
			SetForegroundWindow(hFgWnd);

			if (!checkShellCodeFlag(hProc, &pLaunchDataEx->flag)) {
				UnhookWindowsHookEx(pHookData->hHook);

				return false;
			}

			UnhookWindowsHookEx(pHookData->hHook);

			if (!ReadProcessMemory(hProc, &pLaunchDataEx->pRet, pRet, sizeof(uint32_t), nullptr)) return false;

			return true;
		}

		#endif // !_WIN64


		// ASM:
		// jmp    0x2							jump to skip pLaunchData->pFunc call after first execution
		// push   ebx							save register
		// mov    ebx, pLaunchData						
		// mov    BYTE PTR[ebx - 0x1f], 0x17	fix jump to skip pLaunchData->pFunc call
		// push   DWORD PTR[ebx]				load pLaunchData->pArg for pLaunchData->pFunc call
		// call   DWORD PTR[ebx + 0x4]			call pLaunchData->pFunc
		// mov    DWORD PTR[ebx + 0x8], eax		write return value to pLaunchData->pRet
		// mov    BYTE PTR[ebx + 0xc], 0x1		set pLaunchData->flag to one
		// pop    ebx							restore register
		// mov    eax, pGateway					jump to gateway to execute NtUserBeginPaint
		// jmp    eax
		static BYTE hookBeginPaintShell[]{ 0xEB, 0x00, 0x53, 0xBB, 0x00, 0x00, 0x00, 0x00, 0xC6, 0x43, 0xE1, 0x17, 0xFF, 0x33, 0xFF, 0x53, 0x04, 0x89, 0x43, 0x08, 0xC6, 0x43, 0x0C, 0x01, 0x5B, 0xB8, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xE0, LAUNCH_DATA_X86_SPACE };

		static bool hookBeginPaint(HANDLE hProc, BYTE* pShellCode, BYTE* pNtUserBeginPaint, tLaunchFunc pFunc, void* pArg, void* pRet) {
			const ptrdiff_t launchDataOffset = sizeof(hookBeginPaintShell) - sizeof(LaunchData);

			LaunchData* const pLaunchData = reinterpret_cast<LaunchData*>(hookBeginPaintShell + launchDataOffset);
			pLaunchData->pArg = LODWORD(pArg);
			pLaunchData->pFunc = LODWORD(pFunc);

			const LaunchData* const pLaunchDataEx = reinterpret_cast<LaunchData*>(pShellCode + launchDataOffset);
			*reinterpret_cast<uint32_t*>(hookBeginPaintShell + 0x04) = LODWORD(pLaunchDataEx);

			if (!WriteProcessMemory(hProc, pShellCode, hookBeginPaintShell, sizeof(hookBeginPaintShell), nullptr)) return false;

			const size_t lenStolen = 10;
			BYTE* const pGateway = mem::ex::trampHook(hProc, pNtUserBeginPaint, pShellCode, pShellCode + 0x1A, lenStolen);

			if (!pGateway) return false;

			const DWORD processId = GetProcessId(hProc);

			if (processId) {
				// resize the window to trigger NtUserBeginPaint execution
				EnumWindows(resizeCallback, reinterpret_cast<LPARAM>(&processId));
			}
			
			const bool success = checkShellCodeFlag(hProc, &pLaunchDataEx->flag);

			BYTE* const stolen = new BYTE[lenStolen]{};

			// read the stolen bytes from the gateway
			if (!ReadProcessMemory(hProc, pGateway, stolen, lenStolen, nullptr)) {
				// if gateway gets deallocated here, the process will crash
				delete[] stolen;

				return false;
			}

			// patch the stolen bytes back
			if (!mem::ex::patch(hProc, pNtUserBeginPaint, stolen, lenStolen)) {
				// if gateway gets deallocated here, the process will crash
				delete[] stolen;

				return false;
			}

			delete[] stolen;
			// now gateway can be deallocated safely
			VirtualFreeEx(hProc, pGateway, 0, MEM_RELEASE);

			if (!success) return false;

			if (!ReadProcessMemory(hProc, &pLaunchDataEx->pRet, pRet, sizeof(uint32_t), nullptr)) return false;

			return true;
		}


		// ASM:
		// push   ebp							setup stack frame
		// mov    ebp, esp
		// mov    ecx, DWORD PTR[ebp + 0x8]		load pLaunchData
		// push   ecx							save register with pLaunchData
		// push   DWORD PTR[ecx]				load pLaunchData->pArg for pLaunchData->pFunc call
		// call   DWORD PTR[ecx + 0x4]			call pLaunchData->pFunc
		// pop    ecx							restore register to pLaunchData
		// mov    DWORD PTR[ecx + 0x8], eax		write return value to pLaunchData->pRet
		// mov    BYTE PTR[ecx + 0xc], 0x1		set pLaunchData->flag to one
		// pop    ebp							cleanup stack frame
		// ret    0x4
		static BYTE queueUserApcShell[]{ 0x55, 0x89, 0xE5, 0x8B, 0x4D, 0x08, 0x51, 0xFF, 0x31, 0xFF, 0x51, 0x04, 0x59, 0x89, 0x41, 0x08, 0xC6, 0x41, 0x0C, 0x01, 0x5D, 0xC2, 0x04, 0x00, LAUNCH_DATA_X86_SPACE };

		static bool queueUserApc(HANDLE hProc, BYTE* pShellCode, HANDLE hThread, tLaunchFunc pFunc, void* pArg, void* pRet) {
			const ptrdiff_t launchDataOffset = sizeof(queueUserApcShell) - sizeof(LaunchData);

			LaunchData* const pLaunchData = reinterpret_cast<LaunchData*>(queueUserApcShell + launchDataOffset);
			pLaunchData->pArg = LODWORD(pArg);
			pLaunchData->pFunc = LODWORD(pFunc);

			if (!WriteProcessMemory(hProc, pShellCode, queueUserApcShell, sizeof(queueUserApcShell), nullptr)) return false;

			const HMODULE hNtdll = proc::in::getModuleHandle("ntdll.dll");

			if (!hNtdll) return false;

			const tRtlQueueApcWow64Thread pRtlQueueApcWow64Thread = reinterpret_cast<tRtlQueueApcWow64Thread>(proc::in::getProcAddress(hNtdll, "RtlQueueApcWow64Thread"));

			if (!pRtlQueueApcWow64Thread) return false;

			LaunchData* const pLaunchDataEx = reinterpret_cast<LaunchData*>(pShellCode + launchDataOffset);

			if (pRtlQueueApcWow64Thread(hThread, pShellCode, pLaunchDataEx, nullptr, nullptr) != STATUS_SUCCESS) return false;

			if (!checkShellCodeFlag(hProc, &pLaunchDataEx->flag)) return false;

			if (!ReadProcessMemory(hProc, &pLaunchDataEx->pRet, pRet, sizeof(uint32_t), nullptr)) return false;

			return true;
		}

	}


	#ifdef _WIN64

	namespace x64 {

		// struct for data within the shell code
		typedef struct LaunchData {
			uint64_t pArg;
			uint64_t pFunc;
			uint64_t pRet;
			uint8_t flag;
		}LaunchData;

		// ASM:
		// push rcx								save pLaunchData
		// mov rax, rcx							load pLaunchData->pArg for pLaunchData->pFunc call
		// mov rcx, QWORD PTR[rax]
		// sub rsp, 0x20						setup shadow space for function call
		// call QWORD PTR[rax + 0x8]			call pFunc
		// add rsp, 0x20						cleanup shadow space from function call
		// pop rcx								restore pLaunchData
		// mov QWORD PTR[rcx + 0x10], rax		write return value to &pLaunchData->pRet
		// xor rax, rax
		// ret
		static BYTE crtShell[]{ 0x51, 0x48, 0x8B, 0xC1, 0x48, 0x8B, 0x08, 0x48, 0x83, 0xEC, 0x20, 0xFF, 0x50, 0x08, 0x48, 0x83, 0xC4, 0x20, 0x59, 0x48, 0x89, 0x41, 0x10, 0x48, 0x31, 0xC0, 0xC3, LAUNCH_DATA_X64_SPACE };

		static bool createRemoteThread(HANDLE hProc, BYTE* pShellCode, tLaunchFunc pFunc, void* pArg, void* pRet) {
			LaunchData* const pLaunchData = reinterpret_cast<LaunchData*>(crtShell + sizeof(crtShell) - sizeof(LaunchData));
			pLaunchData->pArg = reinterpret_cast<uint64_t>(pArg);
			pLaunchData->pFunc = reinterpret_cast<uint64_t>(pFunc);

			if (!WriteProcessMemory(hProc, pShellCode, crtShell, sizeof(crtShell), nullptr)) return false;

			LaunchData* const pLaunchDataEx = reinterpret_cast<LaunchData*>(pShellCode + sizeof(crtShell) - sizeof(LaunchData));
			const HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(pShellCode), pLaunchDataEx, 0, nullptr);

			if (!hThread) return false;

			if (WaitForSingleObject(hThread, LAUNCH_TIMEOUT)) {

				#pragma warning (push)
				#pragma warning (disable: 6258)
				TerminateThread(hThread, 0);
				#pragma warning (pop)

				CloseHandle(hThread);

				return false;
			}

			ReadProcessMemory(hProc, &pLaunchDataEx->pRet, pRet, sizeof(uint64_t), nullptr);
			CloseHandle(hThread);

			return true;
		}


		// ASM:
		// push   QWORD PTR[rip + 0x4f]			save old rip (in pLaunchData->pRet) for return instruction
		// push   rax							save registers
		// push   rcx
		// push   rdx
		// push   r8
		// push   r9
		// push   r10
		// push   r11
		// pushf
		// mov    rcx, QWORD PTR[rip + 0x2c]	load pLaunchData->pArg for pLaunchData->pFunc call
		// mov    rax, QWORD PTR[rip + 0x2d]	load pLaunchData->pFunc
		// sub    rsp, 0x20						setup shadow space for function call
		// call   rax
		// add    rsp, 0x20						cleanup shadow space from function call
		// mov    QWORD PTR[rip + 0x24], rax	write return value to pLaunchData->pRet
		// popf									restore registers
		// pop    r11
		// pop    r10
		// pop    r9
		// pop    r8
		// pop    rdx
		// pop    rcx
		// pop    rax
		// mov    BYTE PTR[rip + 0x19], 0x1		set pLaunchData->flag to one
		// ret
		static BYTE hijackShell[]{ 0xFF, 0x35, 0x4F, 0x00, 0x00, 0x00, 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x9C, 0x48, 0x8B, 0x0D, 0x2C, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05, 0x2D, 0x00, 0x00, 0x00, 0x48, 0x83, 0xEC, 0x20, 0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x20, 0x48, 0x89, 0x05, 0x24, 0x00, 0x00, 0x00, 0x9D, 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58, 0xC6, 0x05, 0x19, 0x00, 0x00, 0x00, 0x01, 0xC3, LAUNCH_DATA_X64_SPACE };

		static bool hijackThread(HANDLE hProc, BYTE* pShellCode, HANDLE hThread, DWORD threadId, tLaunchFunc pFunc, void* pArg, void* pRet) {

			if (SuspendThread(hThread) == 0xFFFFFFFF) return false;

			CONTEXT context{};
			context.ContextFlags = CONTEXT_CONTROL;

			if (!GetThreadContext(hThread, &context)) {
				ResumeThread(hThread);

				return false;
			}

			const ptrdiff_t launchDataOffset = sizeof(hijackShell) - sizeof(LaunchData);

			LaunchData* const pLaunchData = reinterpret_cast<LaunchData*>(hijackShell + launchDataOffset);
			pLaunchData->pArg = reinterpret_cast<uint64_t>(pArg);
			pLaunchData->pFunc = reinterpret_cast<uint64_t>(pFunc);

			const uint64_t oldRip = context.Rip;
			// save old rip at pRet for convenience
			// it is used before it is overwritten by the return value
			pLaunchData->pRet = oldRip;

			if (!WriteProcessMemory(hProc, pShellCode, hijackShell, sizeof(hijackShell), nullptr)) {
				ResumeThread(hThread);

				return false;
			}

			context.Rip = reinterpret_cast<uint64_t>(pShellCode);

			if (!SetThreadContext(hThread, &context)) {
				ResumeThread(hThread);

				return false;
			}

			// post thread message to ensure thread execution
			PostThreadMessageA(threadId, 0, 0, 0);

			if (ResumeThread(hThread) == 0xFFFFFFFF) {
				context.Rip = oldRip;
				SetThreadContext(hThread, &context);
				ResumeThread(hThread);

				return false;
			}

			const LaunchData* const pLaunchDataEx = reinterpret_cast<LaunchData*>(pShellCode + launchDataOffset);

			if (!checkShellCodeFlag(hProc, &pLaunchDataEx->flag)) {

				if (SuspendThread(hThread) != 0xFFFFFFFF) {
					context.Rip = oldRip;
					SetThreadContext(hThread, &context);
					ResumeThread(hThread);
				}

				return false;
			}

			if (!ReadProcessMemory(hProc, &pLaunchDataEx->pRet, pRet, sizeof(uint64_t), nullptr)) return false;

			return true;
		}


		// ASM:
		// push   rbp							save registers						
		// push   rsp
		// push   rbx
		// push   r8
		// push   rdx
		// push   rcx
		// jmp    0x0							jump to skip pLaunchData->pFunc call after first execution
		// mov    BYTE PTR[rip - 0x08], 0x2A	fix jump to skip pLaunchData->pFunc call
		// mov    rcx, QWORD PTR[rip + 0x39]	load pLaunchData->pArg for pLaunchData->pFunc call
		// sub    rsp, 0x28						setup shadow space for function call
		// call   QWORD PTR[rip + 0x37]			call pLaunchData->pFunc
		// add    rsp, 0x28						cleanup shadow space from function call
		// mov    QWORD PTR[rip + 0x34], rax	write return value to pLaunchData->pRet
		// mov    BYTE PTR[rip + 0x35], 0x1		set pLaunchData->flag to one
		// pop    rdx							restore registers for pCallNextHookEx call
		// pop    r8
		// pop    r9
		// movabs rbx, pCallNextHookEx			
		// sub    rsp, 0x28						setup shadow space for function call
		// call   rbx							call pCallNextHookEx
		// add    rsp, 0x28						cleanup shadow space from function call
		// pop    rbx							restore registers
		// pop    rsp
		// pop    rbp
		// ret
		static BYTE windowsHookShell[]{ 0x55, 0x54, 0x53, 0x41, 0x50, 0x52, 0x51, 0xEB, 0x00, 0xC6, 0x05, 0xF8, 0xFF, 0xFF, 0xFF, 0x2A, 0x48, 0x8B, 0x0D, 0x39, 0x00, 0x00, 0x00, 0x48, 0x83, 0xEC, 0x28, 0xFF, 0x15, 0x37, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x28, 0x48, 0x89, 0x05, 0x34, 0x00, 0x00, 0x00, 0xC6, 0x05, 0x35, 0x00, 0x00, 0x00, 0x01, 0x5A, 0x41, 0x58, 0x41, 0x59, 0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x83, 0xEC, 0x28, 0xFF, 0xD3, 0x48, 0x83, 0xC4, 0x28, 0x5B, 0x5C, 0x5D, 0xC3, LAUNCH_DATA_X64_SPACE };

		static bool setWindowsHook(HANDLE hProc, BYTE* pShellCode, HookData* pHookData, tLaunchFunc pFunc, void* pArg, void* pRet) {
			const ptrdiff_t launchDataOffset = sizeof(windowsHookShell) - sizeof(LaunchData);

			LaunchData* const pLaunchData = reinterpret_cast<LaunchData*>(windowsHookShell + launchDataOffset);
			pLaunchData->pArg = reinterpret_cast<uint64_t>(pArg);
			pLaunchData->pFunc = reinterpret_cast<uint64_t>(pFunc);

			*reinterpret_cast<uint64_t*>(windowsHookShell + 0x3A) = reinterpret_cast<uint64_t>(pHookData->pCallNextHookEx);

			const LaunchData* const pLaunchDataEx = reinterpret_cast<LaunchData*>(pShellCode + launchDataOffset);

			if (!WriteProcessMemory(hProc, pShellCode, windowsHookShell, sizeof(windowsHookShell), nullptr)) return false;

			EnumWindows(setHookCallback, reinterpret_cast<LPARAM>(pHookData));

			if (!pHookData->hHook || !pHookData->hWnd) return false;

			// foreground window to activate the hook
			const HWND hFgWnd = GetForegroundWindow();
			SetForegroundWindow(pHookData->hWnd);
			SetForegroundWindow(hFgWnd);

			if (!checkShellCodeFlag(hProc, &pLaunchDataEx->flag)) {
				UnhookWindowsHookEx(pHookData->hHook);

				return false;
			}

			UnhookWindowsHookEx(pHookData->hHook);

			if (!ReadProcessMemory(hProc, &pLaunchDataEx->pRet, pRet, sizeof(uint64_t), nullptr)) return false;

			return true;
		}

		
		// ASM:
		// jmp    0x2							jump to skip pLaunchData->pFunc call after first execution
		// mov    BYTE PTR[rip - 0x8], 0x2e		fix jump to skip pLaunchData->pFunc call
		// push   rcx							save registers
		// push   rdx
		// mov    rcx, QWORD PTR[rip + 0x2a]	load pLaunchData->pArg for pLaunchData->pFunc call
		// sub    rsp, 0x28						setup shadow space for function call
		// call   QWORD PTR[rip + 0x28]			call pLaunchData->pFunc
		// add    rsp, 0x28						cleanup shadow space of function call
		// mov    QWORD PTR[rip + 0x25], rax	write return value to pLaunchData->pRet
		// mov    BYTE PTR[rip + 0x26], 0x1		set pLaunchData->flag to one
		// pop    rdx							restore registers
		// pop    rcx
		// movabs rax, pGateway					jump to gateway to execute NtUserBeginPaint
		// jmp    rax
		static BYTE hookBeginPaintShell[]{ 0xEB, 0x00, 0xC6, 0x05, 0xF8, 0xFF, 0xFF, 0xFF, 0x2E, 0x51, 0x52, 0x48, 0x8B, 0x0D, 0x2A, 0x00, 0x00, 0x00, 0x48, 0x83, 0xEC, 0x28, 0xFF, 0x15, 0x28, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x28, 0x48, 0x89, 0x05, 0x25, 0x00, 0x00, 0x00, 0xC6, 0x05, 0x26, 0x00, 0x00, 0x00, 0x01, 0x5A, 0x59, 0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xE0, LAUNCH_DATA_X64_SPACE };

		static bool hookBeginPaint(HANDLE hProc, BYTE* pShellCode, BYTE* pNtUserBeginPaint, tLaunchFunc pFunc, void* pArg, void* pRet) {
			const ptrdiff_t launchDataOffset = sizeof(hookBeginPaintShell) - sizeof(LaunchData);

			LaunchData* const pLaunchData = reinterpret_cast<LaunchData*>(hookBeginPaintShell + launchDataOffset);
			pLaunchData->pArg = reinterpret_cast<uint64_t>(pArg);
			pLaunchData->pFunc = reinterpret_cast<uint64_t>(pFunc);

			if (!WriteProcessMemory(hProc, pShellCode, hookBeginPaintShell, sizeof(hookBeginPaintShell), nullptr)) {
				VirtualFreeEx(hProc, pShellCode, 0, MEM_RELEASE);

				return false;
			}

			const size_t lenStolen = 8;
			BYTE* const pGateway = mem::ex::trampHook(hProc, pNtUserBeginPaint, pShellCode, pShellCode + 0x32, lenStolen);

			if (!pGateway) return false;

			const DWORD processId = GetProcessId(hProc);

			if (processId) {
				// resize the window to trigger UserBeginPaint execution
				EnumWindows(resizeCallback, reinterpret_cast<LPARAM>(&processId));
			}

			const LaunchData* const pLaunchDataEx = reinterpret_cast<LaunchData*>(pShellCode + launchDataOffset);
			const bool success = checkShellCodeFlag(hProc, &pLaunchDataEx->flag);

			BYTE* const pStolen = new BYTE[lenStolen]{};

			// read the stolen bytes from the gateway
			if (!ReadProcessMemory(hProc, pGateway, pStolen, lenStolen, nullptr)) {
				// if gateway gets deallocated here, the process will crash
				delete[] pStolen;

				return false;
			}

			// patch the stolen bytes back
			if (!mem::ex::patch(hProc, pNtUserBeginPaint, pStolen, lenStolen)) {
				// if gateway gets deallocated here, the process will crash
				delete[] pStolen;

				return false;
			}

			delete[] pStolen;
			// now gateway can be deallocated safely
			VirtualFreeEx(hProc, pGateway, 0, MEM_RELEASE);

			if (!success) return false;

			if (!ReadProcessMemory(hProc, &pLaunchDataEx->pRet, pRet, sizeof(uint64_t), nullptr)) return false;

			return true;
		}

		// ASM:
		// push   rcx							save register with pLaunchData
		// mov    rax, QWORD PTR[rcx + 0x8]		load pLaunchData->pFunc for call
		// mov    rcx, QWORD PTR[rcx]			load pLaunchData->pArg for pLaunchData->pFunc call
		// sub    rsp, 0x20						setup shadow space for function call
		// call   rax							call pLaunchData->pFunc
		// add    rsp, 0x20						cleanup shadow space of function call
		// pop    rcx							restore register to pLaunchData
		// mov    QWORD PTR[rcx + 0x10], rax	write return value to pLaunchData->pRet
		// mov    BYTE PTR[rcx + 0x18], 0x1		set pLaunchData->flag to one
		// ret
		static BYTE queueUserApcShell[]{ 0x51, 0x48, 0x8B, 0x41, 0x08, 0x48, 0x8B, 0x09, 0x48, 0x83, 0xEC, 0x20, 0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x20, 0x59, 0x48, 0x89, 0x41, 0x10, 0xC6, 0x41, 0x18, 0x01, 0xC3, LAUNCH_DATA_X64_SPACE };

		static bool queueUserApc(HANDLE hProc, BYTE* pShellCode, HANDLE hThread, tLaunchFunc pFunc, void* pArg, void* pRet) {
			const ptrdiff_t launchDataOffset = sizeof(queueUserApcShell) - sizeof(LaunchData);

			LaunchData* const pLaunchData = reinterpret_cast<LaunchData*>(queueUserApcShell + launchDataOffset);
			pLaunchData->pArg = reinterpret_cast<uint64_t>(pArg);
			pLaunchData->pFunc = reinterpret_cast<uint64_t>(pFunc);

			if (!WriteProcessMemory(hProc, pShellCode, queueUserApcShell, sizeof(queueUserApcShell), nullptr)) return false;

			LaunchData* const pLaunchDataEx = reinterpret_cast<LaunchData*>(pShellCode + launchDataOffset);

			if (!QueueUserAPC(reinterpret_cast<PAPCFUNC>(pShellCode), hThread, reinterpret_cast<ULONG_PTR>(pLaunchDataEx))) return false;

			if (!checkShellCodeFlag(hProc, &pLaunchDataEx->flag)) return false;

			if (!ReadProcessMemory(hProc, &pLaunchDataEx->pRet, pRet, sizeof(uint64_t), nullptr)) return false;

			return true;
		}

	}

	#endif // _WIN64


	// check via flag if shell code has been executed
	static bool checkShellCodeFlag(HANDLE hProc, const void* pFlag) {
		bool flag = false;
		const ULONGLONG start = GetTickCount64();

		do {
			ReadProcessMemory(hProc, pFlag, &flag, sizeof(flag), nullptr);

			if (flag) break;

			Sleep(50);
		} while (GetTickCount64() - start < LAUNCH_TIMEOUT);

		return flag;
	}


	// set WNDPROC hook in the thread of a window of the target process
	static BOOL CALLBACK setHookCallback(HWND hWnd, LPARAM lParam) {
		HookData* const pHookCallbackData = reinterpret_cast<HookData*>(lParam);

		if (pHookCallbackData->hHook) return TRUE;

		DWORD curProcId = 0;
		const DWORD curThreadId = GetWindowThreadProcessId(hWnd, &curProcId);

		if (!curThreadId || !curProcId) return TRUE;

		if (curProcId != pHookCallbackData->processId)  return TRUE;

		if (!IsWindowVisible(hWnd)) return TRUE;

		char className[MAX_PATH]{};

		if (!GetClassNameA(hWnd, className, MAX_PATH)) return TRUE;

		// window procedure hooks only possible for non-console windows
		if (!strcmp(className, "ConsoleWindowClass")) return TRUE;

		// pretend the shell code is loaded by the module
		const HHOOK hHook = SetWindowsHookExA(WH_CALLWNDPROC, pHookCallbackData->pHookFunc, pHookCallbackData->hModule, curThreadId);

		if (!hHook)  return TRUE;

		pHookCallbackData->hHook = hHook;
		pHookCallbackData->hWnd = hWnd;

		return FALSE;
	}


	// resize window to trigger NtUserBeginPaint
	static BOOL CALLBACK resizeCallback(HWND hWnd, LPARAM lParam) {
		DWORD curProcId = 0;
		GetWindowThreadProcessId(hWnd, &curProcId);

		if (!curProcId) return TRUE;

		const DWORD processId = *reinterpret_cast<DWORD*>(lParam);

		if (curProcId != processId) return TRUE;

		if (!IsWindowVisible(hWnd)) return TRUE;

		char className[MAX_PATH]{};

		if (!GetClassNameA(hWnd, className, MAX_PATH)) return TRUE;

		// NtUserBeginPaint only gets called for non-console windows
		if (!strcmp(className, "ConsoleWindowClass")) return TRUE;

		WINDOWPLACEMENT wndPlacement{};
		wndPlacement.length = sizeof(WINDOWPLACEMENT);

		if (!GetWindowPlacement(hWnd, &wndPlacement)) return TRUE;

		const UINT oldShowCmd = wndPlacement.showCmd;

		if (oldShowCmd == SW_MINIMIZE || oldShowCmd == SW_SHOWMINIMIZED || oldShowCmd == SW_SHOWMINNOACTIVE) {
			wndPlacement.showCmd = SW_RESTORE;
		}
		else {
			wndPlacement.showCmd = SW_SHOWMINIMIZED;
		}

		if (!SetWindowPlacement(hWnd, &wndPlacement)) return TRUE;

		wndPlacement.showCmd = oldShowCmd;

		if (!SetWindowPlacement(hWnd, &wndPlacement)) return TRUE;

		return FALSE;
	}

}