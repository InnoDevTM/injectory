#include "injectory/process.hpp"
#include "injectory/module.hpp"
#include "injectory/library.hpp"
#include "injectory/file.hpp"
#include "injectory/memoryarea.hpp"

#include <stdio.h>
#include <Psapi.h>
#include <boost/filesystem.hpp>
#include <boost/interprocess/file_mapping.hpp>
namespace ip = boost::interprocess;


// Matt Pietrek's function
IMAGE_SECTION_HEADER* GetEnclosingSectionHeader(DWORD_PTR rva, IMAGE_NT_HEADERS& nt_header)
{
	IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(&nt_header);

	for (int i = 0; i < nt_header.FileHeader.NumberOfSections; i++, section++)
	{
		// This 3 line idiocy is because Watcom's linker actually sets the
		// Misc.VirtualSize field to 0.  (!!! - Retards....!!!)
		DWORD_PTR size = section->Misc.VirtualSize;
		if (size == 0)
			size = section->SizeOfRawData;

		// Is the RVA within this section?
		if (rva >= section->VirtualAddress && rva < (section->VirtualAddress + size))
			return section;
	}

	return 0;
}

// Matt Pietrek's function
void* GetPtrFromRVA(DWORD_PTR rva, IMAGE_NT_HEADERS& nt_header, const ip::mapped_region& imageBase)
{
	IMAGE_SECTION_HEADER* section = GetEnclosingSectionHeader(rva, nt_header);
	if(!section)
		return 0;
	LONG_PTR delta = (LONG_PTR)( section->VirtualAddress - section->PointerToRawData );
	return (void*)((byte*)imageBase.get_address() + rva - delta);
}

void Process::fixIAT(const ip::mapped_region& imageBase, IMAGE_NT_HEADERS& nt_header, IMAGE_IMPORT_DESCRIPTOR* imgImpDesc)
{
	fs::path parentPath = path().parent_path();
	if (!SetDllDirectoryW(parentPath.wstring().c_str()))
	{
		DWORD errcode = GetLastError();
		BOOST_THROW_EXCEPTION(ex_fix_iat() << e_api_function("SetDllDirectory") << e_text("could not set path to target process") << e_file(parentPath) << e_last_error(errcode));
	}

	while (LPSTR lpModuleName = (LPSTR)GetPtrFromRVA(imgImpDesc->Name, nt_header, imageBase))
	{
		// ACHTUNG: LoadLibraryEx kann eine DLL nur anhand des Namen aus einem anderen
		// Verzeichnis laden wie der Zielprozess!
		Module localModule = Module::load(to_wstring(lpModuleName), DONT_RESOLVE_DLL_REFERENCES);

		Library lib(localModule.path());
		Module remoteModule = isInjected(lib);
		if (!remoteModule)
			remoteModule = inject(lib);

		IMAGE_THUNK_DATA* itd = (IMAGE_THUNK_DATA*)GetPtrFromRVA(imgImpDesc->FirstThunk, nt_header, imageBase);

		while(itd->u1.AddressOfData)
		{
			IMAGE_IMPORT_BY_NAME*iibn = (IMAGE_IMPORT_BY_NAME*)GetPtrFromRVA(itd->u1.AddressOfData, nt_header, imageBase);
			itd->u1.Function = (DWORD_PTR)remoteModule.getProcAddress((LPCSTR)iibn->Name);

			itd++;
		}      

		imgImpDesc++;
	}
}

void Process::mapSections(void* lpModuleBase, byte* dllBin, IMAGE_NT_HEADERS& nt_header)
{
	IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(&nt_header);

	for (int i = 0; i < nt_header.FileHeader.NumberOfSections; i++, section++)
	{
		void* dst = (void*)( (DWORD_PTR)lpModuleBase + section->VirtualAddress );
		const void* src = (const void*)( (DWORD_PTR)dllBin + section->PointerToRawData );

		memory(dst, section->SizeOfRawData).write(src);

		/*
		// next section header, calculate virtualSize of section header
		SIZE_T virtualSize = section->VirtualAddress;
		//printf("section: %s | %p | %x\n", section->Name, section->VirtualAddress, virtualSize);
		if (section->VirtualAddress)
			virtualSize = section->VirtualAddress - virtualSize;
		PDWORD lpflOldProtect = 0;
		if(!VirtualProtectEx(hProcess, (LPVOID)( (DWORD_PTR)lpModuleBase + section->VirtualAddress ), virtualSize,
			section->Characteristics & 0x00FFFFFF, lpflOldProtect))
		{
			PRINT_ERROR_MSGA("VirtualProtectEx failed.");
			return FALSE;
		}
		*/
	}
}

void fixRelocations(const ip::mapped_region& dllBin,
	const MemoryArea& moduleBase,
	IMAGE_NT_HEADERS& nt_header,
	IMAGE_BASE_RELOCATION* imgBaseReloc
	)
{
	LONG_PTR delta = (DWORD_PTR)moduleBase.address() - nt_header.OptionalHeader.ImageBase;
	//SIZE_T relocationSize = pNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;
	WORD* relocData = 0;

	if (!imgBaseReloc->SizeOfBlock) // image has no relocations
		return;
	
	do
	{
		byte* relocBase = (byte*)GetPtrFromRVA(imgBaseReloc->VirtualAddress, nt_header, dllBin);
		SIZE_T numRelocations = (imgBaseReloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
		SIZE_T i = 0;

		//printf("numRelocations: %d\n", numRelocations);

		relocData = (WORD*)( (DWORD_PTR)&imgBaseReloc + sizeof(IMAGE_BASE_RELOCATION) );

		// loop over all relocation entries
		for(i = 0; i < numRelocations; i++, relocData++)
		{
			// Get reloc data
			BYTE RelocType = *relocData >> 12;
			WORD Offset = *relocData & 0xFFF;

			switch(RelocType)
			{
			case IMAGE_REL_BASED_ABSOLUTE:
				break;

			case IMAGE_REL_BASED_HIGHLOW:
				*(DWORD32*)(relocBase + Offset) += (DWORD32)delta;
				break;

			case IMAGE_REL_BASED_DIR64:
				*(DWORD64*)(relocBase + Offset) += delta;

				break;

			default:
				BOOST_THROW_EXCEPTION(ex("unsuppported relocation type"));
			}
		}

		imgBaseReloc = (IMAGE_BASE_RELOCATION*)relocData;

	} while( *(DWORD*)relocData );
}

void Process::callTlsInitializers(
	HMODULE hModule,
	DWORD fdwReason,
	IMAGE_TLS_DIRECTORY& imgTlsDir)
{
	DWORD_PTR callbacks = (DWORD_PTR)imgTlsDir.AddressOfCallBacks;

	if (callbacks)
	{
		for (;;)
		{
			void* callback = memory<void*>((void*)callbacks);

			if (!callback)
				break;

			remoteDllMainCall(callback, hModule, fdwReason, nullptr);
			callbacks += sizeof(DWORD_PTR);
		}
	}
}

Module Process::mapRemoteModule(const Library& lib)
{
	try
	{
		File file = File::create(lib.path(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);

		ip::file_mapping m_file(lib.path().string().c_str(), ip::read_only);
		ip::mapped_region region(m_file, ip::read_only);

		const IMAGE_DOS_HEADER& dos_header = *(IMAGE_DOS_HEADER*)region.get_address();
		
		if(dos_header.e_magic != IMAGE_DOS_SIGNATURE)
			BOOST_THROW_EXCEPTION (ex_map_remote() << e_text("invalid DOS header"));
		
		IMAGE_NT_HEADERS& nt_header = *(IMAGE_NT_HEADERS*)((DWORD_PTR) region.get_address()+dos_header.e_lfanew);

		if(nt_header.Signature != IMAGE_NT_SIGNATURE)
			BOOST_THROW_EXCEPTION (ex_map_remote() << e_text("invalid PE header"));

		// Allocate space for the module in the remote process
		MemoryArea moduleBase = alloc(nt_header.OptionalHeader.SizeOfImage, false);
		
		// fix imports
		IMAGE_IMPORT_DESCRIPTOR& imgImpDesc = *(IMAGE_IMPORT_DESCRIPTOR*)GetPtrFromRVA(
			nt_header.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress,
			nt_header,
			region);
		if (nt_header.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size)
			fixIAT(region, nt_header, &imgImpDesc);
		
		// fix relocs
		IMAGE_BASE_RELOCATION& imgBaseReloc = *(IMAGE_BASE_RELOCATION*)GetPtrFromRVA(
			(DWORD)(nt_header.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress),
			nt_header,
			region);
		if(nt_header.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size)
			fixRelocations(region, moduleBase, nt_header, &imgBaseReloc);

		// Write the PE header into the remote process's memory space
		moduleBase.write(region.get_address(), nt_header.FileHeader.SizeOfOptionalHeader +
			sizeof(nt_header.FileHeader) +
			sizeof(nt_header.Signature));

		// Map the sections into the remote process(they need to be aligned
		// along their virtual addresses)
		mapSections(moduleBase.address(), (byte*)region.get_address(), nt_header);

		// call all tls callbacks
		IMAGE_TLS_DIRECTORY& imgTlsDir = *(IMAGE_TLS_DIRECTORY*)GetPtrFromRVA(
			nt_header.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress,
			nt_header,
			region);
		if(nt_header.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size)
			callTlsInitializers((HMODULE)moduleBase.address(), DLL_PROCESS_ATTACH, imgTlsDir);

		// call entry point
		remoteDllMainCall(
			(LPVOID)((DWORD_PTR)moduleBase.address() + nt_header.OptionalHeader.AddressOfEntryPoint),
			(HMODULE)moduleBase.address(), 1, nullptr);

		return isInjected((HMODULE)moduleBase.address());
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(ex("failed to map PE file into memory") << e_library(lib.path()) << e_process(*this) <<
			boost::errinfo_nested_exception(boost::current_exception()));
	}
}
