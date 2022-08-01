#include <windows.h>
#include "ImportHandler.h"

#include "PartList.h"
#include "Hunk.h"
#include "StringMisc.h"
#include "Log.h"
#include "Symbol.h"
#include "data.h"

#include <vector>
#include <set>
#include <ppl.h>
#include <cassert>

using namespace std;

const char *LoadDLL(const char *name);

static unsigned int RVAToFileOffset(const char* module, unsigned int rva)
{
	const IMAGE_DOS_HEADER* pDH = (const PIMAGE_DOS_HEADER)module;
	const IMAGE_NT_HEADERS32* pNTH = (const PIMAGE_NT_HEADERS32)(module + pDH->e_lfanew);
	int numSections = pNTH->FileHeader.NumberOfSections;
	int numDataDirectories = pNTH->OptionalHeader.NumberOfRvaAndSizes;
	const IMAGE_SECTION_HEADER* sectionHeaders = (const IMAGE_SECTION_HEADER*)&pNTH->OptionalHeader.DataDirectory[numDataDirectories];
	for(int i = 0; i < numSections; i++)
	{
		if(rva >= sectionHeaders[i].VirtualAddress && rva < sectionHeaders[i].VirtualAddress + sectionHeaders[i].SizeOfRawData)
		{
			return rva - sectionHeaders[i].VirtualAddress + sectionHeaders[i].PointerToRawData;
		}
	}
	return rva;
}

static int GetOrdinal(const char* function, const char* dll) {
	const char* module = LoadDLL(dll);

	const IMAGE_DOS_HEADER* dh = (const IMAGE_DOS_HEADER*)module;
	const IMAGE_FILE_HEADER* coffHeader = (const IMAGE_FILE_HEADER*)(module + dh->e_lfanew + 4);
	const IMAGE_OPTIONAL_HEADER32* pe = (const IMAGE_OPTIONAL_HEADER32*)(coffHeader + 1);
	const IMAGE_EXPORT_DIRECTORY* exportdir = (const IMAGE_EXPORT_DIRECTORY*) (module + RVAToFileOffset(module, pe->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress));
	
	const short* ordinalTable = (const short*) (module + RVAToFileOffset(module, exportdir->AddressOfNameOrdinals));
	const int* nameTable = (const int*)(module + RVAToFileOffset(module, exportdir->AddressOfNames));
	for(int i = 0; i < (int)exportdir->NumberOfNames; i++) {
		int ordinal = ordinalTable[i] + exportdir->Base;
		const char* name = module + RVAToFileOffset(module, nameTable[i]);
		if(strcmp(name, function) == 0) {
			return ordinal;
		}
	}

	Log::Error("", "Import '%s' cannot be found in '%s'", function, dll);
	return -1;
}

void ForEachExportInDLL(const char *dll, std::function<void (const char*)> fun) {
	const char* module = LoadDLL(dll);

	const IMAGE_DOS_HEADER* dh = (const IMAGE_DOS_HEADER*)module;
	const IMAGE_FILE_HEADER* coffHeader = (const IMAGE_FILE_HEADER*)(module + dh->e_lfanew + 4);
	const IMAGE_OPTIONAL_HEADER32* pe = (const IMAGE_OPTIONAL_HEADER32*)(coffHeader + 1);
	const IMAGE_EXPORT_DIRECTORY* exportdir = (const IMAGE_EXPORT_DIRECTORY*)(module + RVAToFileOffset(module, pe->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress));

	const int* nameTable = (const int*)(module + RVAToFileOffset(module, exportdir->AddressOfNames));
	for (int i = 0; i < (int)exportdir->NumberOfNames; i++) {
		const char* name = module + RVAToFileOffset(module, nameTable[i]);
		fun(name);
	}
}


static const char *GetForwardRVA(const char* dll, const char* function) {
	const char* module = LoadDLL(dll);
	const IMAGE_DOS_HEADER* pDH = (const PIMAGE_DOS_HEADER)module;
	const IMAGE_NT_HEADERS32* pNTH = (const PIMAGE_NT_HEADERS32)(module + pDH->e_lfanew);

	const DWORD exportRVA = pNTH->OptionalHeader.DataDirectory[0].VirtualAddress;
	const IMAGE_EXPORT_DIRECTORY* pIED = (const PIMAGE_EXPORT_DIRECTORY)(module + RVAToFileOffset(module, exportRVA));

	const short* ordinalTable = (const short*)(module + RVAToFileOffset(module, pIED->AddressOfNameOrdinals));
	const DWORD* namePointerTable = (const DWORD*)(module + RVAToFileOffset(module, pIED->AddressOfNames));
	const DWORD* addressTableRVAOffset = (const DWORD*)(module + RVAToFileOffset(module, pIED->AddressOfFunctions));

	for(unsigned int i = 0; i < pIED->NumberOfNames; i++) {
		short ordinal = ordinalTable[i];
		const char* name = (const char*)(module + RVAToFileOffset(module, namePointerTable[i]));

		if(strcmp(name, function) == 0) {
			DWORD address = addressTableRVAOffset[ordinal];
			if(address >= pNTH->OptionalHeader.DataDirectory[0].VirtualAddress &&
				address < pNTH->OptionalHeader.DataDirectory[0].VirtualAddress + pNTH->OptionalHeader.DataDirectory[0].Size)
				return module + RVAToFileOffset(module, address);
			return NULL;
		}
	}

	Log::Error("", "Import '%s' cannot be found in '%s'", function, dll);
	return false;
}


static bool ImportHunkRelation(const Hunk* h1, const Hunk* h2) {
	// Sort by DLL name
	if(strcmp(h1->GetImportDll(), h2->GetImportDll()) != 0) {
		// kernel32 always first
		if(strcmp(h1->GetImportDll(), "kernel32") == 0)
			return true;
		if(strcmp(h2->GetImportDll(), "kernel32") == 0)
			return false;

		// Then user32, to ensure MessageBoxA@16 is ready when we need it
		if(strcmp(h1->GetImportDll(), "user32") == 0)
			return true;
		if(strcmp(h2->GetImportDll(), "user32") == 0)
			return false;


		return strcmp(h1->GetImportDll(), h2->GetImportDll()) < 0;
	}

	// Sort by ordinal
	return GetOrdinal(h1->GetImportName(), h1->GetImportDll()) < 
		GetOrdinal(h2->GetImportName(), h2->GetImportDll());
}

static const int HashCode(const char* str) {
	int code = 0;
	char eax;
	do {
		code = _rotl(code, 6);
		eax = *str++;
		code ^= eax;

	} while(eax);
	return code;
}


__forceinline unsigned int HashCode1K(const char* str, int hash_multiplier, int hash_bits)
{
	int eax = 0;
	unsigned char c;
	do
	{
		c = *str++;
		eax = ((eax & 0xFFFFFF00) + c) * hash_multiplier;
	} while(c & 0x7F);

	eax = (eax & 0xFFFFFF00) | (unsigned char)(c + c);

	return ((unsigned int)eax) >> (32 - hash_bits);
}

static bool SolveDllOrderConstraints(std::vector<unsigned int>& constraints, unsigned int* new_order)
{
	if(constraints[0] > 1)	// kernel32 must be first. it can't have dependencies on anything else
	{
		return false;
	}

	std::vector<unsigned int> constraints2 = constraints;
	unsigned int used_mask = 0;

	int num = (int)constraints.size();
	for(int i = 0; i < num; i++)
	{
		int selected = -1;
		for(int j = 0; j < num; j++)
		{
			if(((used_mask >> j) & 1) == 0 && (constraints[j] == 0))
			{
				selected = j;
				break;
			}
		}

		if(selected == -1)
		{
			return false;
		}

		
		*new_order++ = selected;
		used_mask |= (1u<<selected);
		for(int j = 0; j < num; j++)
		{
			constraints[j] &= ~(1u<<selected);
		}
	}

	return true;
}

static void AddKnownExportsForDll(std::vector<string>& exports, const char* dll_name)
{
	struct s_known_exports_header
	{
		int num_dlls;
		struct
		{
			int name_offset;
			int num_exports;
			int export_name_offset_table;
		} dll_infos[1];
	};

	const s_known_exports_header* known_exports_header = (const s_known_exports_header*)knownDllExports;
	
	int num_known_dlls = known_exports_header->num_dlls;
	for(int known_dll_index = 0; known_dll_index < num_known_dlls; known_dll_index++)
	{
		const char* known_dll_name = knownDllExports + known_exports_header->dll_infos[known_dll_index].name_offset;
		if(strcmp(dll_name, known_dll_name) == 0)
		{
			int num_exports = known_exports_header->dll_infos[known_dll_index].num_exports;
			const int* offset_table = (const int*) ((const char*)knownDllExports + known_exports_header->dll_infos[known_dll_index].export_name_offset_table);
			for(int i = 0; i < num_exports; i++)
			{
				const char* name = knownDllExports + offset_table[i];
				exports.push_back(name);
			}
			break;
		}
	}
}

static bool FindCollisionFreeHash(vector<string>& dll_names, const vector<Hunk*>& importHunks, int& hash_multiplier, int& hash_bits)
{
	assert(dll_names.size() <= 32);

	dll_names.erase(std::find(dll_names.begin(), dll_names.end(), string("kernel32")));
	dll_names.insert(dll_names.begin(), "kernel32");
	
	struct SDllInfo
	{
		std::vector<std::string>	exports;
		std::vector<char>			used;
	};

	int num_dlls = (int)dll_names.size();
	std::vector<unsigned int> best_dll_order(num_dlls);

	// Load DLLs and mark functions that are imported
	vector<SDllInfo> dllinfos(num_dlls);
	
	for(int dll_index = 0; dll_index < num_dlls; dll_index++)
	{
		const char* dllname = dll_names[dll_index].c_str();
		SDllInfo& info = dllinfos[dll_index];

		{
			// Scrape exports from DLL on this machine
			const char* module = LoadDLL(dllname);

			const IMAGE_DOS_HEADER* dh = (const IMAGE_DOS_HEADER*)module;
			const IMAGE_FILE_HEADER* coffHeader = (const IMAGE_FILE_HEADER*)(module + dh->e_lfanew + 4);
			const IMAGE_OPTIONAL_HEADER32* pe = (const IMAGE_OPTIONAL_HEADER32*)(coffHeader + 1);
			const IMAGE_EXPORT_DIRECTORY* exportdir = (const IMAGE_EXPORT_DIRECTORY*)(module + RVAToFileOffset(module, pe->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress));
			int num_names = exportdir->NumberOfNames;
			const int* name_table = (const int*)(module + RVAToFileOffset(module, exportdir->AddressOfNames));
			for(int i = 0; i < num_names; i++)
			{
				const char* name = module + RVAToFileOffset(module, name_table[i]);
				info.exports.push_back(name);
			}
		}

		// Combine with list of known exports for this DLL
		AddKnownExportsForDll(info.exports, dllname);
		std::sort(info.exports.begin(), info.exports.end());
		info.exports.erase(std::unique(info.exports.begin(), info.exports.end()), info.exports.end());

		int num_exports = (int)info.exports.size();
		info.used.resize(num_exports);

		for(Hunk* importHunk : importHunks)
		{
			if(strcmp(dllname, importHunk->GetImportDll()) == 0)
			{
				// Mark those that are used
				auto it = std::find(info.exports.begin(), info.exports.end(), importHunk->GetImportName());
				
				if(it != info.exports.end())
				{
					int idx = (int)std::distance(info.exports.begin(), it);
					info.used[idx] = 1;
				}
				else
				{
					assert(false);
					Log::Error("", "Could not find '%s' in '%s'", importHunk->GetImportName(), importHunk->GetImportDll());
				}
			}
		}
	}

	const int MAX_BITS = 16;

	int best_num_bits = INT_MAX;
	
	// Find hash function that works
	// We do however allow hash overlaps from separate DLLs.
	// To exploit this we sort the dlls to avoid collisions when possible
	
	struct SBucket
	{
		unsigned int	unreferenced_functions_dll_mask;
		unsigned char	referenced_function_dll_index;		// dll_index + 1
	};
	int best_low_byte = INT_MAX;
	int best_high_byte = INT_MAX;
	
	concurrency::critical_section cs;
	for(int num_bits = MAX_BITS; num_bits >= 1; num_bits--)
	{
		concurrency::parallel_for(0, 256, [&](int high_byte)
		{
			{
				Concurrency::critical_section::scoped_lock l(cs);
				if(num_bits == best_num_bits && high_byte > best_high_byte)
				{
					return;
				}
			}
			std::vector<unsigned int> dll_constraints(num_dlls);
			std::vector<unsigned int> new_dll_order(num_dlls);
			SBucket* buckets = new SBucket[(size_t)1 << num_bits];
			for(int low_byte = 0; low_byte < 256; low_byte++)
			{
				for(int dll_index = 0; dll_index < num_dlls; dll_index++)
				{
					dll_constraints[dll_index] = 0;
				}

				int hash_multiplier = (high_byte << 16) | (low_byte << 8) | 1;

				memset(buckets, 0, sizeof(SBucket) << num_bits);
				bool has_collisions = false;

				unsigned int dll_index = 0;
				for(SDllInfo& dllinfo : dllinfos)
				{
					unsigned int dll_mask = (1u << dll_index);
					
					int num_names = (int)dllinfo.exports.size();
					for(int i = 0; i < num_names; i++)
					{
						unsigned int hashcode = HashCode1K(dllinfo.exports[i].c_str(), hash_multiplier, num_bits);
						bool new_referenced = dllinfo.used[i];
						bool old_referenced = buckets[hashcode].referenced_function_dll_index > 0;

						if(new_referenced)
						{
							if(old_referenced)
							{
								has_collisions = true;
								break;
							}
							else
							{
								buckets[hashcode].referenced_function_dll_index = dll_index + 1;
								buckets[hashcode].unreferenced_functions_dll_mask &= ~dll_mask;	// Clear unreferenced before this

								dll_constraints[dll_index] |= buckets[hashcode].unreferenced_functions_dll_mask;
							}
						}
						else
						{
							buckets[hashcode].unreferenced_functions_dll_mask |= dll_mask;
							if(old_referenced)
							{
								int old_dll_index = buckets[hashcode].referenced_function_dll_index - 1;
								if(old_dll_index == dll_index)
								{
									has_collisions = true;
									break;
								}
								dll_constraints[old_dll_index] |= dll_mask;
							}
						}
					}
					dll_index++;

					if(has_collisions)
					{
						break;
					}
				}

				if(!has_collisions && SolveDllOrderConstraints(dll_constraints, &new_dll_order[0]))
				{
					Concurrency::critical_section::scoped_lock l(cs);
					if(num_bits < best_num_bits || high_byte < best_high_byte)
					{
						best_low_byte = low_byte;
						best_high_byte = high_byte;
						best_num_bits = num_bits;
						best_dll_order = new_dll_order;
					}
					break;
				}
			}

			delete[] buckets;
		});

		int best_hash_multiplier = (best_high_byte << 16) | (best_low_byte << 8) | 1;
		if(best_num_bits > num_bits)
		{
			break;
		}
	}
	int best_hash_multiplier = (best_high_byte << 16) | (best_low_byte << 8) | 1;

	if(best_num_bits == INT_MAX)
	{
		return false;
	}

	// Reorder DLLs
	std::vector<std::string> new_dlls(num_dlls);
	for(int i = 0; i < num_dlls; i++)
	{
		new_dlls[i] = dll_names[best_dll_order[i]];
	}
	dll_names = new_dlls;
	
	hash_multiplier = best_hash_multiplier;
	hash_bits = best_num_bits;
	return true;
}

static Hunk* ForwardImport(Hunk* hunk) {
	do {
		const char *forward = GetForwardRVA(hunk->GetImportDll(), hunk->GetImportName());
		if (forward == NULL) break;

		string dllName, functionName;
		int sep = int(strstr(forward, ".") - forward);
		dllName.append(forward, sep);
		dllName = ToLower(dllName);
		functionName.append(&forward[sep + 1], strlen(forward) - (sep + 1));
		Log::Warning("", "Import '%s' from '%s' uses forwarded RVA. Replaced by '%s' from '%s'",
			hunk->GetImportName(), hunk->GetImportDll(), functionName.c_str(), dllName.c_str());
		hunk = new Hunk(hunk->GetName(), functionName.c_str(), dllName.c_str());
	} while (true);
	return hunk;
}

void ImportHandler::AddImportHunks4K(PartList& parts, Hunk*& hashHunk, map<string, string>& fallbackDlls, const vector<string>& rangeDlls, bool verbose, bool& enableRangeImport) {
	if(verbose)
		printf("\n-Imports----------------------------------\n");

	vector<Hunk*> importHunks;
	vector<bool> usedRangeDlls(rangeDlls.size());
	
	// Fill list for import hunks
	enableRangeImport = false;
	parts.ForEachHunkWithBreak([&rangeDlls, &importHunks, &usedRangeDlls, &enableRangeImport](Hunk* hunk)
		{
			if(hunk->GetFlags() & HUNK_IS_IMPORT) {
				hunk = ForwardImport(hunk);

				// Is the DLL a range DLL?
				for(int i = 0; i < (int)rangeDlls.size(); i++) {
					if(ToUpper(rangeDlls[i]) == ToUpper(hunk->GetImportDll())) {
						usedRangeDlls[i] = true;
						enableRangeImport = true;
						return true;
					}
				}
				importHunks.push_back(hunk);
			}
			return false;
		});

	// Sort import hunks
	sort(importHunks.begin(), importHunks.end(), ImportHunkRelation);

	set<string> usedFallbackDlls;
	vector<unsigned int> hashes;
	Hunk* importList = new Hunk("ImportListHunk", 0, HUNK_IS_WRITEABLE, 16, 0, 0);
	char dllNames[1024] = {0};
	char* dllNamesPtr = dllNames+1;
	char* hashCounter = dllNames;
	string currentDllName;
	int pos = 0;
	for(vector<Hunk*>::const_iterator it = importHunks.begin(); it != importHunks.end();) {
		Hunk* importHunk = *it;
		bool useRange = false;

		// Is the DLL a range DLL?
		for(int i = 0; i < (int)rangeDlls.size(); i++) {
			if(ToUpper(rangeDlls[i]) == ToUpper(importHunk->GetImportDll())) {
				usedRangeDlls[i] = true;
				useRange = true;
				break;
			}
		}

		// Skip non hashes
		if(currentDllName.compare(importHunk->GetImportDll()))
		{
			if(strcmp(importHunk->GetImportDll(), "kernel32") != 0)
			{
				set<string> seen;
				string dll = importHunk->GetImportDll();
				strcpy_s(dllNamesPtr, sizeof(dllNames)-(dllNamesPtr-dllNames), dll.c_str());
				dllNamesPtr += dll.size() + 1;
				while (fallbackDlls.count(dll) != 0) {
					usedFallbackDlls.insert(dll);
					seen.insert(dll);
					*dllNamesPtr = 0;
					dllNamesPtr += 1;
					dll = fallbackDlls[dll];
					strcpy_s(dllNamesPtr, sizeof(dllNames) - (dllNamesPtr - dllNames), dll.c_str());
					dllNamesPtr += dll.size() + 1;
					if (seen.count(dll) != 0) Log::Error("", "Cyclic DLL fallback");
				}
				hashCounter = dllNamesPtr;
				*hashCounter = 0;
				dllNamesPtr += 1;
			}


			currentDllName = importHunk->GetImportDll();
			if(verbose)
				printf("%s\n", currentDllName.c_str());
		}

		(*hashCounter)++;
		int hashcode = HashCode(importHunk->GetImportName());
		hashes.push_back(hashcode);
		int startOrdinal = GetOrdinal(importHunk->GetImportName(), importHunk->GetImportDll());
		int ordinal = startOrdinal;

		// Add import
		if(verbose) {
			if(useRange)
				printf("  ordinal range {\n  ");
			printf("  %s (ordinal %d, hash %08X)\n", (*it)->GetImportName(), startOrdinal, hashcode);
		}

		importList->AddSymbol(new Symbol(importHunk->GetName(), pos*4, SYMBOL_IS_RELOCATEABLE, importList));
		it++;

		while(useRange && it != importHunks.end() && currentDllName.compare((*it)->GetImportDll()) == 0)	// Import the rest of the range
		{
			int o = GetOrdinal((*it)->GetImportName(), (*it)->GetImportDll());
			if(o - startOrdinal >= 254)
				break;

			if(verbose) {
				printf("    %s (ordinal %d)\n", (*it)->GetImportName(), o);
			}

			ordinal = o;
			importList->AddSymbol(new Symbol((*it)->GetName(), (pos+ordinal-startOrdinal)*4, SYMBOL_IS_RELOCATEABLE, importList));
			it++;
		}

		if(verbose && useRange)
			printf("  }\n");

		if(enableRangeImport)
			*dllNamesPtr++ = ordinal - startOrdinal + 1;
		pos += ordinal - startOrdinal + 1;
	}
	*dllNamesPtr++ = -1;

	// Warn about unused range DLLs
	for (int i = 0; i < (int)rangeDlls.size(); i++) {
		if (!usedRangeDlls[i]) {
			Log::Warning("", "No functions were imported from range DLL '%s'", rangeDlls[i].c_str());
		}
	}

	// Warn about unused fallback DLLs
	for (auto fallback : fallbackDlls) {
		if (usedFallbackDlls.count(fallback.first) == 0) {
			Log::Warning("", "No functions were imported from fallback DLL '%s'", fallback.first.c_str());
		}
	}

	importList->SetVirtualSize(pos*4);
	importList->AddSymbol(new Symbol("_ImportList", 0, SYMBOL_IS_RELOCATEABLE, importList));
	importList->AddSymbol(new Symbol(".bss", 0, SYMBOL_IS_RELOCATEABLE|SYMBOL_IS_SECTION, importList, "crinkler import"));

	hashHunk = new Hunk("HashHunk", (char*)hashes.data(), 0, 0, int(hashes.size()*sizeof(unsigned int)), int(hashes.size()*sizeof(unsigned int)));
	
	// Add new hunks
	parts.GetUninitializedPart().AddHunkBack(importList);

	Hunk* dllNamesHunk = new Hunk("DllNames", dllNames, HUNK_IS_WRITEABLE | HUNK_IS_LEADING, 0, int(dllNamesPtr - dllNames), int(dllNamesPtr - dllNames));
	dllNamesHunk->AddSymbol(new Symbol(".data", 0, SYMBOL_IS_RELOCATEABLE|SYMBOL_IS_SECTION, dllNamesHunk, "crinkler import"));
	dllNamesHunk->AddSymbol(new Symbol("_DLLNames", 0, SYMBOL_IS_RELOCATEABLE, dllNamesHunk));
	parts.GetDataPart().AddHunkBack(dllNamesHunk);

	parts.RemoveMatchingHunks([](Hunk* hunk) {
		return hunk->GetFlags() & HUNK_IS_IMPORT;
		});
}

void ImportHandler::AddImportHunks1K(PartList& parts, bool verbose, int& hash_bits, int& max_dll_name_length) {
	if (verbose)
	{
		printf("\n-Imports----------------------------------\n");
	}

	vector<Hunk*> importHunks;
	set<string> dll_set;

	bool found_kernel32 = false;

	// Fill list for import hunks
	parts.ForEachHunk([&found_kernel32, &dll_set, &importHunks](Hunk* hunk) {
		if(hunk->GetFlags() & HUNK_IS_IMPORT)
		{
			hunk = ForwardImport(hunk);
			if(strcmp(hunk->GetImportDll(), "kernel32") == 0)
			{
				found_kernel32 = true;
			}
			dll_set.insert(hunk->GetImportDll());
			importHunks.push_back(hunk);
		}
	});


	if(!found_kernel32)
	{
		Log::Error("", "Kernel32 needs to be linked for import code to function.");
	}

	int hash_multiplier;
	vector<string> dlls(dll_set.begin(), dll_set.end());
	if (!FindCollisionFreeHash(dlls, importHunks, hash_multiplier, hash_bits))
	{
		Log::Error("", "Could not find collision-free hash function");
	}

	string dllnames;
	
	int max_name_length = 0;
	for(string name : dlls)
	{
		max_name_length = max(max_name_length, (int)name.size() + 1);
	}

	for(string name : dlls)
	{
		while (dllnames.size() % max_name_length)
		{
			dllnames.push_back(0);
		}
		
		if(name.compare("kernel32") != 0)
		{
			dllnames += name;
		}
	}
	 
	Hunk* importList = new Hunk("ImportListHunk", 0, HUNK_IS_WRITEABLE, 8, 0, 65536*256);
	importList->AddSymbol(new Symbol("_HashMultiplier", hash_multiplier, 0, importList));
	importList->AddSymbol(new Symbol("_ImportList", 0, SYMBOL_IS_RELOCATEABLE, importList));
	for(Hunk* importHunk : importHunks)
	{
		unsigned int hashcode = HashCode1K(importHunk->GetImportName(), hash_multiplier, hash_bits);
		importList->AddSymbol(new Symbol(importHunk->GetName(), hashcode*4, SYMBOL_IS_RELOCATEABLE, importList));
	}

	if(verbose)
	{
		for(string dllname : dlls)
		{
			printf("%s\n", dllname.c_str());
				
			for(Hunk* importHunk : importHunks)
			{
				if(strcmp(importHunk->GetImportDll(), dllname.c_str()) == 0)
				{
					int ordinal = GetOrdinal(importHunk->GetImportName(), importHunk->GetImportDll());
					printf("  %s (ordinal %d)\n", importHunk->GetImportName(), ordinal);
				}
			}
		}
	}

	Hunk* dllNamesHunk = new Hunk("DllNames", dllnames.c_str(), HUNK_IS_WRITEABLE | HUNK_IS_LEADING, 0, (int)dllnames.size() + 1, (int)dllnames.size() + 1);
	dllNamesHunk->AddSymbol(new Symbol("_DLLNames", 0, SYMBOL_IS_RELOCATEABLE, dllNamesHunk));
	parts.GetDataPart().AddHunkBack(dllNamesHunk);
	parts.GetUninitializedPart().AddHunkBack(importList);
	max_dll_name_length = max_name_length;

	printf(
		"\n"
		"Note: Programs linked using the TINYIMPORT option may break if a future Windows\n"
		"version adds functions to one of the imported DLLs. Such breakage cannot be\n"
		"fixed by using the RECOMPRESS feature. When using this option, it is strongly\n"
		"recommended to also distribute a version of your program linked using the\n"
		"normal import mechanism (without the TINYIMPORT option).\n"
	);

	parts.RemoveMatchingHunks([](Hunk* hunk) {
		return hunk->GetFlags() & HUNK_IS_IMPORT;
		});
}