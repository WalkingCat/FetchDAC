#include "stdafx.h"

#pragma comment(lib, "version.lib")
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace std;

void PrintLastError(const string& message)
{
	DWORD error = GetLastError();
	LPSTR buffer = nullptr;
	size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR) &buffer, 0, NULL);
	printf_s(" %s: 0x%x - %s", message.c_str(), error, buffer);
	LocalFree(buffer);
}

wstring get_file_arch(const wstring& library) {
	wstring ret;

	auto file = CreateFile(library.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file != INVALID_HANDLE_VALUE) {
		auto mapping = CreateFileMapping(file, NULL, PAGE_READONLY, 0, 0, NULL);
		if (mapping != NULL) {
			auto base = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
			if (base != NULL) {
				PIMAGE_NT_HEADERS header = ImageNtHeader(base);
				if (header != nullptr) {
					switch (header->FileHeader.Machine)
					{
					case IMAGE_FILE_MACHINE_I386:	ret = L"x86"; break;
					case IMAGE_FILE_MACHINE_AMD64:	ret = L"amd64"; break;
					case IMAGE_FILE_MACHINE_ARM:	ret = L"armce"; break;
					//case 0x1c4:						ret = L"arm"; break;
					case IMAGE_FILE_MACHINE_ARMNT:	ret = L"arm"; break;
					default: break;
					}
				} else PrintLastError("ImageNtHeader failed");
				UnmapViewOfFile(base);
			} else PrintLastError("MapViewOfFile failed");
			CloseHandle(mapping);
		} else PrintLastError("CreateFileMapping failed");
		CloseHandle(file);
	} else PrintLastError("CreateFile failed");

	return ret;
}

wstring get_file_version(const wstring& library) {
	wstring ret;

	auto size = GetFileVersionInfoSize(library.c_str(), NULL);
	if (size != 0) {
		auto data = new BYTE[size];
		if (GetFileVersionInfo(library.c_str(), 0, size, data) != FALSE) {
			LPVOID info = nullptr; UINT len = 0;
			if (VerQueryValue(data, L"\\", &info, &len) != FALSE) {
				if ((info != nullptr) && (len >= sizeof(VS_FIXEDFILEINFO))) {
					VS_FIXEDFILEINFO *ffi = (VS_FIXEDFILEINFO*) info;
					wchar_t buffer[24] = {};
					swprintf_s(buffer, L"%u.%u.%u.%02u", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
					ret = buffer;
				}
			} else PrintLastError("VerQueryValue failed");
		} else PrintLastError("GetFileVersionInfo failed");
		delete data;
	} else PrintLastError("GetFileVersionInfoSize failed");

	return ret;
}

int _tmain(int argc, _TCHAR* argv[])
{
	printf_s("\n FetchDAC - Download DAC/SOS for CLR runtime library\n\n");

	//////////////////////////////////////////////////////////////////////////
	// process command line, set runtime and arch_subst

	bool error_usage = false;

	wstring runtime, arch_subst, output;
	bool load_pdb = false;

	for (int i = 1; i < argc; ++i) {
		if (_wcsicmp(argv[i], L"-a") == 0) {
			if (((i + 1) < argc) && arch_subst.empty()) arch_subst = argv[++i];
			else { error_usage = true; break; }
		} else if (_wcsicmp(argv[i], L"-o") == 0) {
			if (((i + 1) < argc) && output.empty()) output = argv[++i];
			else { error_usage = true; break; }
		} else if (_wcsicmp(argv[i], L"-p") == 0) {
			load_pdb = true;
		} else {
			if (runtime.empty()) runtime = argv[i];
			else { error_usage = true; break; }
		}
	}

	if (runtime.empty()) error_usage = true;

	if (error_usage) {
		printf_s(" Usage: fetchdac [-a <architecture>] [-o <directory>] [-p] <runtime.dll>\n\n");
		printf_s("  <runtime.dll>\t: CLR runtime library (clr|coreclr|mrt100|slr100).dll\n");
		printf_s("  -o\t\t: specify output directory, default is current\n");
		printf_s("  -a\t\t: load DAC/SOS for specified architecture (arm|x86|amd64)\n");
		printf_s("  -p\t\t: load PDB\n");
		printf_s("  by default, fetchdac load same arch for x86/amd64 target, x86 for arm target\n\n");
		return 0;
	}

	if (output.empty()) output = L'.'; // else check if its valid directory name ??

	auto runtime_arch = get_file_arch(runtime);
	if (runtime_arch.empty()) return 0; // error messages already printed

	wstring load_arch;
	if (arch_subst.empty()) { // user didn't specify, use default logic
		if (runtime_arch == L"arm") load_arch = L"x86";
		else load_arch = runtime_arch;
	} else load_arch = arch_subst;

	const auto runtime_version = get_file_version(runtime);

	const auto filename = PathFindFileName(runtime.c_str());

	printf_s(" Runtime Library: %S, Arch: %S, Version: %S\n", filename, runtime_arch.c_str(), runtime_version.c_str());
	
	//////////////////////////////////////////////////////////////////////////
	// find out dac/sos request file name.

	wstring dac, sos;

	const auto clr_suffix = L"_" + load_arch + L"_" + runtime_arch + L"_" + runtime_version + L".dll";
	const auto mrt_suffix = L"_win" + load_arch + L".dll";

	if		(_wcsnicmp(filename, L"clr", 3)		== 0) { dac = L"mscordacwks" + clr_suffix; sos = L"sos" + clr_suffix; }
	else if (_wcsnicmp(filename, L"coreclr", 7) == 0) { dac = L"mscordaccore" + clr_suffix; sos = L"sos" + clr_suffix; }
	else if (_wcsnicmp(filename, L"slr100", 6)	== 0) {	dac = L"slr100dac.dll";	sos = L"slr100sos.dll";	}
	else if (_wcsnicmp(filename, L"mrt100", 6)	== 0) {	dac = L"mrt100dac" + mrt_suffix; sos = L"mrt100sos" + mrt_suffix; }

	if (dac.empty() && sos.empty()) return 0;

	//////////////////////////////////////////////////////////////////////////
	// do real work

	SYMSRV_INDEX_INFO sii = {};
	sii.sizeofstruct = sizeof(sii);
	if (SymSrvGetFileIndexInfo(runtime.c_str(), &sii, 0) == FALSE) {
		PrintLastError("SymSrvGetFileIndexInfo failed");
		return 0;
	}

	HANDLE sym_handle = GetCurrentProcess();
	if(SymInitialize(sym_handle, (L"SRV*" + output + L"*http://msdl.microsoft.com/download/symbols").c_str(), FALSE) != FALSE) {
		wchar_t found_file[MAX_PATH] = {};

		printf_s(" Load DAC ");
		if (!dac.empty() && (SymFindFileInPath(sym_handle, NULL, dac.c_str(), (PVOID) sii.timestamp, sii.size, 0, SSRVOPT_DWORD, found_file, NULL, NULL) != FALSE)) {
			 printf_s("OK: %S\n", found_file);
		} else printf_s("failed: %S\n", dac.c_str());

		printf_s(" Load SOS ");
		if (!sos.empty() && (SymFindFileInPath(sym_handle, NULL, sos.c_str(), (PVOID) sii.timestamp, sii.size, 0, SSRVOPT_DWORD, found_file, NULL, NULL) != FALSE)) {
			printf_s("OK: %S\n", found_file);
		} else printf_s("failed: %S\n", sos.c_str());

		if (load_pdb) {
			printf_s(" Load PDB ");
			if (SymFindFileInPath(sym_handle, NULL, sii.pdbfile, &sii.guid, sii.age, 0, SSRVOPT_GUIDPTR, found_file, NULL, NULL) != FALSE) {
				printf_s("OK: %S\n", found_file);
			} else printf_s("failed: %S\n", sii.pdbfile);
		}

		SymCleanup(sym_handle);
	} else PrintLastError("SymInitialize failed");

	return 0;
}

