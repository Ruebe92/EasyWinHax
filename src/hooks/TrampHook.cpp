#include "TrampHook.h"
#include "..\mem.h"
#include "..\proc.h"

namespace hax {

	namespace ex {

		TrampHook::TrampHook(HANDLE hProc, BYTE* origin, const BYTE* shell, size_t shellSize, const char* originCallPattern, size_t size) :
			_hProc(hProc), _origin(origin), _detour(nullptr), _detourOriginCall(nullptr), _size(size), _gateway(nullptr), _hooked(false)
		{
			_detour = static_cast<BYTE*>(VirtualAllocEx(hProc, nullptr, sizeof(shell), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));

			if (_detour && shell) {

				if (WriteProcessMemory(hProc, _detour, shell, shellSize, nullptr)) {
					// scan for the origin call in the shell code injected into the target
					_detourOriginCall = mem::ex::findSigAddress(hProc, _detour, shellSize, originCallPattern);
				}

			}

		}


		TrampHook::TrampHook(
			HANDLE hProc, const char* modName, const char* funcName, const BYTE* shell, size_t shellSize, const char* originCallPattern, size_t size
		) : _hProc(hProc), _origin(nullptr), _detour(nullptr), _detourOriginCall(nullptr), _size(size), _gateway(nullptr), _hooked(false)
		{
			HMODULE hMod = proc::ex::getModuleHandle(hProc, modName);

			if (hMod) {
				this->_origin = reinterpret_cast<BYTE*>(proc::ex::getProcAddress(hProc, hMod, funcName));
			}

			_detour = static_cast<BYTE*>(VirtualAllocEx(hProc, nullptr, shellSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));

			if (_detour && shell) {

				if (WriteProcessMemory(this->_hProc, this->_detour, shell, shellSize, nullptr)) {
					// scan for the origin call in the shell code injected into the target
					this->_detourOriginCall = mem::ex::findSigAddress(hProc, _detour, shellSize, originCallPattern);
				}

			}

		}


		TrampHook::~TrampHook() {
			this->disable();

			if (this->_detour) {
				VirtualFreeEx(this->_hProc, this->_detour, 0, MEM_RELEASE);
			}
			
		}


		bool TrampHook::enable() {

			if (this->_hooked || !this->_origin || !this->_detour || !this->_detourOriginCall) return false;

			// install the trampoline hook
			this->_gateway = mem::ex::trampHook(this->_hProc, this->_origin, this->_detour, this->_detourOriginCall, this->_size);

			if (!this->_gateway) return false;

			this->_hooked = true;

			return true;
		}


		bool TrampHook::disable() {
			if (!this->_gateway) return false;

			// overwritten/stolen bytes
			BYTE* const stolen = new BYTE[this->_size]{};

			if (!stolen) return false;

			// read the stolen bytes from the gateway
			if (!ReadProcessMemory(this->_hProc, this->_gateway, stolen, this->_size, nullptr)) {
				delete[] stolen;

				return false;
			}

			if (this->_origin && this->_hooked) {

				// patch the overwritten bytes back to the origin function
				if (mem::ex::patch(this->_hProc, this->_origin, stolen, this->_size)) {
					this->_hooked = false;
					delete[] stolen;

					return VirtualFreeEx(this->_hProc, _gateway, 0, MEM_RELEASE);
				}

			}

			delete[] stolen;

			return false;
		}


		bool TrampHook::isHooked() const {

			return this->_hooked;
		}


		BYTE* TrampHook::getOrigin() const {

			return this->_origin;
		}


		BYTE* TrampHook::getDetour() const {

			return this->_detour;
		}


		BYTE* TrampHook::getGateway() const {

			return this->_gateway;
		}

	}


	namespace in {

		TrampHook::TrampHook(BYTE* origin, const BYTE* detour, size_t size) :
			_origin(origin), _detour(detour), _size(size), _gateway(nullptr), _hooked(false) {}


		TrampHook::TrampHook(const char* modName, const char* funcName, const BYTE* detour, size_t size) :
			_origin(nullptr), _detour(detour), _size(size), _gateway(nullptr), _hooked(false)
		{
			const HMODULE hMod = proc::in::getModuleHandle(modName);

			if (hMod) {
				this->_origin = reinterpret_cast<BYTE*>(proc::in::getProcAddress(hMod, funcName));
			}

		}


		TrampHook::~TrampHook() {
			this->disable();
		}


		bool TrampHook::enable() {

			if (this->_hooked || !this->_origin || !this->_detour) return false;

			this->_gateway = mem::in::trampHook(this->_origin, this->_detour, this->_size);

			if (!this->_gateway) return false;

			this->_hooked = true;

			return true;
		}


		bool TrampHook::disable() {

			if (!this->_gateway) return false;

			if (this->_origin && this->_hooked) {

				if (mem::in::patch(this->_origin, this->_gateway, this->_size)) {
					this->_hooked = false;

					return VirtualFree(_gateway, 0, MEM_RELEASE);

				}

			}

			return false;
		}


		bool TrampHook::isHooked() const {

			return this->_hooked;
		}



		BYTE* TrampHook::getOrigin() const {

			return this->_origin;
		}


		BYTE* TrampHook::getDetour() const {

			return const_cast<BYTE*>(this->_detour);
		}


		BYTE* TrampHook::getGateway() const {

			return this->_gateway;
		}

	}
	
}