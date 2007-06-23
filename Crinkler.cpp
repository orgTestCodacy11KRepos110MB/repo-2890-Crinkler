#include "Crinkler.h"
#include "Compressor/Compressor.h"
#include "TinyCompress.h"

#include <list>
#include <set>
#include <ctime>
#include <fstream>

#include "HunkList.h"
#include "Hunk.h"
#include "CoffObjectLoader.h"
#include "CoffLibraryLoader.h"
#include "ImportHandler.h"
#include "Log.h"
#include "HeuristicHunkSorter.h"
#include "EmpiricalHunkSorter.h"
#include "misc.h"
#include "ConsoleProgressBar.h"
#include "WindowProgressBar.h"
#include "CompositeProgressBar.h"
#include "data.h"
#include "Symbol.h"
#include "StringMisc.h"

using namespace std;

Crinkler::Crinkler() {
	m_subsytem = SUBSYSTEM_WINDOWS;
	m_imageBase = 0x00400000;
	m_hashsize = 50*1024*1024;
	m_compressionType = COMPRESSION_FAST;
	m_useSafeImporting = false;
	m_hashtries = 0;
	m_hunktries = 0;
	m_verboseFlags = 0;
	m_showProgressBar = false;
	m_modelbits = 8;
	m_1KMode = false;
}


Crinkler::~Crinkler() {
}

void Crinkler::replaceDlls(HunkList& hunklist) {
	set<string> usedDlls;
	//replace dll
	for(int i = 0; i < hunklist.getNumHunks(); i++) {
		Hunk* hunk = hunklist[i];
		if(hunk->getFlags() & HUNK_IS_IMPORT) {
			map<string, string>::iterator it = m_replaceDlls.find(toLower(hunk->getImportDll()));
			if(it != m_replaceDlls.end()) {
				hunk->setImportDll(it->second.c_str());
				usedDlls.insert(it->first);
			}
		}
	}

	//warn about unused replace DLLs
	for(map<string, string>::iterator it = m_replaceDlls.begin(); it != m_replaceDlls.end(); it++) {
		if(usedDlls.find(it->first) == usedDlls.end()) {
			Log::warning(0, "", "no functions were imported from replaced dll '%s'", it->first.c_str());
		}
	}
}


void Crinkler::load(const char* filename) {
	HunkList* hunkList = m_hunkLoader.loadFromFile(filename);
	if(hunkList) {
		m_hunkPool.append(hunkList);
		delete hunkList;
	} else {
		Log::error(0, "", "failed to load file '%s'", filename);
	}
}

void Crinkler::load(const char* data, int size, const char* module) {
	HunkList* hunkList = m_hunkLoader.load(data, size, module);
	m_hunkPool.append(hunkList);
	delete hunkList;
}

Symbol* Crinkler::findUndecoratedSymbol(const char* name) const {
	Symbol* s = m_hunkPool.findUndecoratedSymbol(name);
	if(s)
		return s;
	else {
		Log::error(0, "", "could not find symbol '%s'", name);
		return NULL;
	}
}


Symbol* Crinkler::getEntrySymbol() const {
	if(m_entry.size() == 0) {
		switch(m_subsytem) {
			case SUBSYSTEM_CONSOLE:
				return findUndecoratedSymbol("mainCRTStartup");
			case SUBSYSTEM_WINDOWS:
				return findUndecoratedSymbol("WinMainCRTStartup");
		}
		return NULL;
	} else {
		return findUndecoratedSymbol(m_entry.c_str());
	}
}

static int getPreciseTime() {
	LARGE_INTEGER time;
	LARGE_INTEGER freq;
	QueryPerformanceCounter(&time);
	QueryPerformanceFrequency(&freq);
	return (int)(time.QuadPart*1000 / freq.QuadPart);
}


static int previousPrime(int n) {
in:
	n = (n-2)|1;
	for (int i = 3 ; i*i < n ; i += 2) {
		if (n/i*i == n) goto in;
	}
	return n;
}

void extractFunctions(CompressionSummaryRecord* csr, vector<CompressionSummaryRecord*>& functions) {
	for(vector<CompressionSummaryRecord*>::iterator it = csr->children.begin(); it != csr->children.end(); it++) {
		if((*it)->type & RECORD_FUNCTION)
			functions.push_back(*it);
		extractFunctions(*it, functions);
	}
}

bool compareFunctionsByName(CompressionSummaryRecord* a, CompressionSummaryRecord* b) {
	return a->functionName < b->functionName;
}

bool compareFunctionsByCompSize(CompressionSummaryRecord* a, CompressionSummaryRecord* b) {
	return a->compressedFunctionSize > b->compressedFunctionSize;
}

bool compareFunctionsByAddress(CompressionSummaryRecord* a, CompressionSummaryRecord* b) {
	return a->pos < b->pos;
}

void verboseFunctions(CompressionSummaryRecord* csr, bool (*compareFunction)(CompressionSummaryRecord*, CompressionSummaryRecord*)) {
	vector<CompressionSummaryRecord*> functions;
	printf("\nfunction name                                      size          compsize\n");
	extractFunctions(csr, functions);

	sort(functions.begin(), functions.end(), compareFunction);

	for(vector<CompressionSummaryRecord*>::iterator it = functions.begin(); it != functions.end(); it++)
		printf("  %-36.36s        %9d          %8.2f\n", (*it)->name.c_str(), (*it)->functionSize, (*it)->compressedFunctionSize / (BITPREC*8.0f));
}

void verboseLabels(CompressionSummaryRecord* csr) {
	if(csr->type & RECORD_ROOT) {
		printf("\nlabel name                                   pos comp-pos      size compsize");
	} else {
		if(csr->type & RECORD_SECTION)
			printf("\n%-38.38s", csr->name.c_str());
		else if(csr->type & RECORD_PUBLIC)
			printf("  %-36.36s", csr->name.c_str());
		else
			printf("    %-34.34s", csr->name.c_str());


		if(csr->compressedPos >= 0)
			printf(" %9d %8.2f %9d %8.2f\n", csr->pos, csr->compressedPos / (BITPREC*8.0f), csr->size, csr->compressedSize / (BITPREC*8.0f));
		else
			printf(" %9d          %9d\n", csr->pos, csr->size);
	}

	for(vector<CompressionSummaryRecord*>::iterator it = csr->children.begin(); it != csr->children.end(); it++)
		verboseLabels(*it);
}

void Crinkler::link(const char* filename) {
	//open output file now, just to be sure :)
	FILE* outfile;
	if(fopen_s(&outfile, filename, "wb")) {
		Log::error(0, "", "could not open '%s' for writing", filename);
		return;
	}

	//place entry point in the beginning
	Symbol* entry = getEntrySymbol();
	if(entry == NULL) {
		Log::error(0, "", "could not find entry symbol");
		return;
	}

	//add a jump to the entry point, if the entry point is not at the beginning of a hunk
	if(entry->value > 0) {
		Log::warning(0, "", "Could not move entry point to beginning of code, inserted jump");
		unsigned char jumpCode[5] = {0xE9, 0x00, 0x00, 0x00, 0x00};
		Hunk* jumpHunk = new Hunk("jump_to_entry_point", (char*)jumpCode, HUNK_IS_CODE, 0, 5, 5);
		Symbol* newEntry = new Symbol("jumpEntry", 0, SYMBOL_IS_RELOCATEABLE, jumpHunk);
		jumpHunk->addSymbol(newEntry);
		m_hunkPool.addHunkFront(jumpHunk);
		relocation r = {entry->name.c_str(), 1, RELOCTYPE_REL32};
		jumpHunk->addRelocation(r);
		jumpHunk->fixate();
		entry = newEntry;	//jumphunk is the new entry :)
	} else {
		m_hunkPool.removeHunk(entry->hunk);
		m_hunkPool.addHunkFront(entry->hunk);
		entry->hunk->fixate();
	}

	//1byte aligned entry point
	if(entry->hunk->getAlignmentBits() > 0) {
		Log::warning(0, "", "entry point hunk has alignment greater than 1, forcing alignment of 1");
		entry->hunk->setAlignmentBits(0);
	}

	{	//check dependencies and remove unused hunks
		list<Hunk*> startHunks;
		startHunks.push_back(entry->hunk);
		startHunks.push_back(m_hunkPool.findSymbol("__imp__LoadLibraryA@4")->hunk);	//don't throw loadlibrary away. we are going to need it
		m_hunkPool.removeUnreferencedHunks(startHunks);
	}

	//replace dlls
	replaceDlls(m_hunkPool);

	//handle imports
	HunkList* headerHunks;
	if(m_1KMode)
		headerHunks = m_hunkLoader.load(header1KObj, header1KObj_end - header1KObj, "crinkler header");
	else
		headerHunks = m_hunkLoader.load(headerObj, headerObj_end - headerObj, "crinkler header");

	Hunk* header = headerHunks->findSymbol("_header")->hunk;
	headerHunks->removeHunk(header);
	bool usesRangeImport;
	{	//add imports
		
		HunkList* importHunkList = ImportHandler::createImportHunks(&m_hunkPool, header, m_rangeDlls, m_verboseFlags & VERBOSE_IMPORTS, usesRangeImport);
		m_hunkPool.removeImportHunks();
		m_hunkPool.append(importHunkList);
		delete importHunkList;
	}

	//do imports
	if(m_useSafeImporting)
		if(usesRangeImport)
			load(importSafeRangeObj, importSafeRangeObj_end - importSafeRangeObj, "crinkler import");
		else
			load(importSafeObj, importSafeObj_end - importSafeObj, "crinkler import");
	else
		if(usesRangeImport)
			load(importRangeObj, importRangeObj_end - importRangeObj, "crinkler import");
		else
			load(importObj, importObj_end - importObj, "crinkler import");

	Symbol* import = findUndecoratedSymbol("Import");
	m_hunkPool.removeHunk(import->hunk);
	m_hunkPool.addHunkFront(import->hunk);
	import->hunk->fixate();
	import->hunk->addSymbol(new Symbol("_ImageBase", m_imageBase, 0, import->hunk));

	HeuristicHunkSorter::sortHunkList(&m_hunkPool);
	int sectionSize = 0x10000;

	//create phase 1 data hunk
	int splittingPoint;


	Hunk* phase1 = m_transform.linkAndTransform(&m_hunkPool, m_imageBase+sectionSize*2, &splittingPoint);



	printf("\nUncompressed size of code: %5d\n", splittingPoint);
	printf("Uncompressed size of data: %5d\n", phase1->getRawSize() - splittingPoint);

	//Do 1k specific stuff
	if (m_1KMode) {
		int compressed_size = phase1->getRawSize()*2+100;
		unsigned char* compressed_data = new unsigned char[compressed_size];
		
		TinyCompress((unsigned char*)phase1->getPtr(), phase1->getRawSize(), compressed_data, compressed_size);
		Hunk* phase1Compressed = new Hunk("compressed data", (char*)compressed_data, 0, 1, compressed_size, compressed_size);
		phase1Compressed->addSymbol(new Symbol("_PackedData", 0, SYMBOL_IS_RELOCATEABLE, phase1Compressed));
		delete[] compressed_data;
		printf("compressed size: %d\n", compressed_size);

		HunkList phase2list;
		phase2list.addHunkBack(header);
		Hunk* depacker = headerHunks->findSymbol("_DepackEntry")->hunk;
		headerHunks->removeHunk(depacker);
		phase2list.addHunkBack(depacker);
		phase2list.addHunkBack(phase1Compressed);

		Hunk* phase2 = phase2list.toHunk("final");
		//add constants
		{
			int virtualSize = align(phase1->getVirtualSize(), 16);
			int packedDataPos = phase2->findSymbol("_PackedData")->value;
			int packedDataOffset = (packedDataPos - sectionSize*2)*8;
			printf("packed data offset: %x\n", packedDataOffset);
			printf("image size: %x\n", phase1->getRawSize());
			phase2->addSymbol(new Symbol("_UnpackedData", m_imageBase + sectionSize*2, 0, phase2));
			phase2->addSymbol(new Symbol("_PackedDataOffset", packedDataOffset, 0, phase2));
			phase2->addSymbol(new Symbol("_VirtualSize", virtualSize, 0, phase2));
			phase2->addSymbol(new Symbol("_ImageBase", m_imageBase, 0, phase2));

			int baseprob = 13;
			*(phase2->getPtr() + phase2->findSymbol("_BaseProbPtr1")->value) = baseprob;
			*(phase2->getPtr() + phase2->findSymbol("_BaseProbPtr2")->value) = baseprob;
			*(phase2->getPtr() + phase2->findSymbol("_SubsystemTypePtr")->value) = m_subsytem == SUBSYSTEM_WINDOWS ? IMAGE_SUBSYSTEM_WINDOWS_GUI : IMAGE_SUBSYSTEM_WINDOWS_CUI;
			*((short*)(phase2->getPtr() + phase2->findSymbol("_LinkerVersionPtr")->value)) = CRINKLER_LINKER_VERSION;
			*((short*)(phase2->getPtr() + phase2->findSymbol("_UnpackedDataLengthPtr")->value)) = phase1->getRawSize()*8;
		}
		phase2->relocate(m_imageBase);
		

		fwrite(phase2->getPtr(), 1, phase2->getRawSize(), outfile);
		fclose(outfile);

		printf("\nFinal file size: %d\n\n", phase2->getRawSize());

		delete headerHunks;
		delete phase1;
		delete phase2;

		return;
	}

	//calculate baseprobs
	int baseprob = 10;
	int baseprobs[8];
	for(int i = 0; i < 8; i++)
		baseprobs[i] = baseprob;	//flat baseprob

	int best_hashsize = previousPrime(m_hashsize/2)*2;
	int modelskip = 0;
	ModelList ml1, ml2;
	Hunk* phase1Compressed, *models;
	int maxsize = phase1->getRawSize()*10;	//allocate plenty of memory
	unsigned char* data = new unsigned char[maxsize];
	int* sizefill = new int[phase1->getRawSize()*2];
	int size, idealsize = 0;
	if (m_compressionType == COMPRESSION_INSTANT) {
		ml1 = InstantModels();
		ml2 = InstantModels();
	} else {
		//Construct composite progress bar
		ConsoleProgressBar consoleBar;
		WindowProgressBar windowBar;
		CompositeProgressBar progressBar;
		progressBar.addProgressBar(&consoleBar);
		if(m_showProgressBar)
			progressBar.addProgressBar(&windowBar);

		progressBar.init();
		progressBar.beginTask("Estimating models for code");
		ml1 = ApproximateModels((unsigned char*)phase1->getPtr(), splittingPoint, baseprobs, &size, &progressBar, m_verboseFlags & VERBOSE_MODELS, m_compressionType, m_modelbits);
		progressBar.endTask();
		
		idealsize = size;

		progressBar.beginTask("Estimating models for data");
		ml2 = ApproximateModels((unsigned char*)phase1->getPtr()+splittingPoint, phase1->getRawSize() - splittingPoint, baseprobs, &size, &progressBar, m_verboseFlags & VERBOSE_MODELS, m_compressionType, m_modelbits);
		progressBar.endTask();
		idealsize += size;
		printf("\nIdeal compressed total size: %d\n", idealsize / BITPREC / 8);

		if(m_hunktries > 0) {
			EmpiricalHunkSorter::sortHunkList(&m_hunkPool, ml1, ml2, baseprobs, m_hunktries, m_showProgressBar ? &windowBar : NULL);
			delete phase1;
			phase1 = m_transform.linkAndTransform(&m_hunkPool, m_imageBase+sectionSize*2, &splittingPoint);
			//reestimate models
			progressBar.beginTask("Reestimating models for code");
			ml1 = ApproximateModels((unsigned char*)phase1->getPtr(), splittingPoint, baseprobs, &size, &progressBar, m_verboseFlags & VERBOSE_MODELS, m_compressionType, m_modelbits);
			progressBar.endTask();
			idealsize = size;

			progressBar.beginTask("Reestimating models for data");
			ml2 = ApproximateModels((unsigned char*)phase1->getPtr()+splittingPoint, phase1->getRawSize() - splittingPoint, baseprobs, &size, &progressBar, m_verboseFlags & VERBOSE_MODELS, m_compressionType, m_modelbits);
			progressBar.endTask();
			idealsize += size;

			printf("\nIdeal compressed total size: %d\n", idealsize / BITPREC / 8);
		}
		

		//hashing time
		if(m_hashtries > 0) {
			int bestsize = INT_MAX;
			int hashsize = best_hashsize;
			progressBar.beginTask("Optimizing hash table size");

			for(int i = 0; i < m_hashtries; i++) {
				CompressionStream cs(data, NULL, maxsize);
				hashsize = previousPrime(hashsize/2)*2;
				cs.Compress((unsigned char*)phase1->getPtr(), splittingPoint, ml1, baseprobs, hashsize, false);
				cs.Compress((unsigned char*)phase1->getPtr() + splittingPoint, phase1->getRawSize() - splittingPoint, ml2, baseprobs, hashsize, true);
				
				size = cs.close();
				if(size <= bestsize) {
					bestsize = size;
					best_hashsize = hashsize;
				}
				
				progressBar.update(i+1, m_hashtries);
			}
			progressBar.endTask();
		}
	}

	CompressionStream cs(data, sizefill, maxsize);
	cs.Compress((unsigned char*)phase1->getPtr(), splittingPoint, ml1, baseprobs, best_hashsize, false);
	cs.Compress((unsigned char*)phase1->getPtr() + splittingPoint, phase1->getRawSize() - splittingPoint, ml2, baseprobs, best_hashsize, true);
	size = cs.close();
	if(m_compressionType != COMPRESSION_INSTANT)
		printf("Real compressed total size: %d\nBytes lost to hashing: %d\n", size, size - idealsize / BITPREC / 8);

	CompressionSummaryRecord* csr = phase1->getCompressionSummary(sizefill, splittingPoint);
	delete[] sizefill;
	if(m_verboseFlags & VERBOSE_LABELS)
		verboseLabels(csr);
	if(m_verboseFlags & VERBOSE_FUNCTIONS)
		verboseFunctions(csr, compareFunctionsByAddress);
	if(m_verboseFlags & VERBOSE_FUNCTIONS_BYSIZE)
		verboseFunctions(csr, compareFunctionsByCompSize);
	if(m_verboseFlags & VERBOSE_FUNCTIONS_BYNAME)
		verboseFunctions(csr, compareFunctionsByName);
	delete csr;
	
	phase1Compressed = new Hunk("compressed data", (char*)data, 0, 1, size, size);
	delete[] data;
	phase1Compressed->addSymbol(new Symbol("_PackedData", 0, SYMBOL_IS_RELOCATEABLE, phase1Compressed));
	int modelsSize = 16 + ml1.nmodels + ml2.nmodels;
	{
		unsigned char masks1[256];
		unsigned char masks2[256];
		unsigned int w1 = ml1.getMaskList(masks1, false);
		unsigned int w2 = ml2.getMaskList(masks2, true);
		models = new Hunk("models", 0, 0, 1, modelsSize, modelsSize);
		models->addSymbol(new Symbol("_Models", 0, SYMBOL_IS_RELOCATEABLE, models));
		char* ptr = models->getPtr();
		*(unsigned int*)ptr = splittingPoint*8;			ptr += sizeof(unsigned int);
		*(unsigned int*)ptr = w1;						ptr += sizeof(unsigned int);
		for(int m = 0; m < ml1.nmodels; m++)
			*ptr++ = masks1[m];
		*(unsigned int*)ptr = phase1->getRawSize()*8;	ptr += sizeof(unsigned int);
		*(unsigned int*)ptr = w2;						ptr += sizeof(unsigned int);
		for(int m = 0; m < ml2.nmodels; m++)
			*ptr++ = masks2[m];
	}
	modelskip = ml1.nmodels+8;


	HunkList phase2list;
	phase2list.addHunkBack(header);
	Hunk* depacker = headerHunks->findSymbol("_DepackEntry")->hunk;
	headerHunks->removeHunk(depacker);
	phase2list.addHunkBack(depacker);
	phase2list.addHunkBack(models);
	phase2list.addHunkBack(phase1Compressed);
	header->addSymbol(new Symbol("_HashTable", sectionSize*2+phase1->getRawSize(), SYMBOL_IS_RELOCATEABLE, header));

	Hunk* phase2 = phase2list.toHunk("final");
	//add constants
	{
		int virtualSize = max(phase1->getVirtualSize(), phase1->getRawSize()+best_hashsize);
		virtualSize = align(virtualSize, 16);
		phase2->addSymbol(new Symbol("_HashTableSize", best_hashsize/2, 0, phase2));
		phase2->addSymbol(new Symbol("_UnpackedData", m_imageBase + sectionSize*2, 0, phase2));
		phase2->addSymbol(new Symbol("_VirtualSize", virtualSize, 0, phase2));
		phase2->addSymbol(new Symbol("_ImageBase", m_imageBase, 0, phase2));

		*(phase2->getPtr() + phase2->findSymbol("_BaseProbPtr")->value) = baseprob;
		*(phase2->getPtr() + phase2->findSymbol("_ModelSkipPtr")->value) = modelskip;
		*(phase2->getPtr() + phase2->findSymbol("_SubsystemTypePtr")->value) = m_subsytem == SUBSYSTEM_WINDOWS ? IMAGE_SUBSYSTEM_WINDOWS_GUI : IMAGE_SUBSYSTEM_WINDOWS_CUI;
		*((short*)(phase2->getPtr() + phase2->findSymbol("_LinkerVersionPtr")->value)) = CRINKLER_LINKER_VERSION;
	}
	phase2->relocate(m_imageBase);

	fwrite(phase2->getPtr(), 1, phase2->getRawSize(), outfile);
	fclose(outfile);

	printf("\nFinal file size: %d\n\n", phase2->getRawSize());

	delete headerHunks;
	delete phase1;
	delete phase2;
}

Crinkler* Crinkler::set1KMode(bool use1KMode) {
	m_1KMode = use1KMode;
	return this;
}

Crinkler* Crinkler::setEntry(const char* entry) {
	m_entry = entry;
	return this;
}

Crinkler* Crinkler::setSubsystem(SubsystemType subsystem) {
	m_subsytem = subsystem;
	return this;
}

Crinkler* Crinkler::setHashsize(int hashsize) {
	m_hashsize = hashsize*1024*1024;
	return this;
}

Crinkler* Crinkler::setCompressionType(CompressionType compressionType) {
	m_compressionType = compressionType;
	return this;
}

Crinkler* Crinkler::setImportingType(bool safe) {
	m_useSafeImporting = safe;
	return this;
}

Crinkler* Crinkler::addRangeDll(const char* dllname) {
	m_rangeDlls.push_back(dllname);
	return this;
}

Crinkler* Crinkler::addReplaceDll(const char* dll1, const char* dll2) {
	m_replaceDlls.insert(pair<string, string>(toLower(dll1), toLower(dll2)));
	return this;
}

Crinkler* Crinkler::clearRangeDlls() {
	m_rangeDlls.clear();
	return this;
}

Crinkler* Crinkler::setHashtries(int hashtries) {
	m_hashtries = hashtries;
	return this;
}

Crinkler* Crinkler::setVerboseFlags(int verboseFlags) {
	m_verboseFlags = verboseFlags;
	return this;
}

Crinkler* Crinkler::showProgressBar(bool show) {
	m_showProgressBar = show;
	return this;
}

Crinkler* Crinkler::addTransform(Transform* transform) {
	m_transform.addTransform(transform);
	return this;
}

Crinkler* Crinkler::setHunktries(int hunktries) {
	m_hunktries = hunktries;
	return this;
}

Crinkler* Crinkler::setModelBits(int modelbits) {
	m_modelbits = modelbits;
	return this;
}
