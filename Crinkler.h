#ifndef _CRINKLER_H_
#define _CRINKLER_H_

#include <list>
#include "MultiLoader.h"
#include "HunkList.h"
#include "Compressor/Compressor.h"
#include "CompositeTransform.h"


#include <string>

class HunkLoader;

enum SubsystemType {SUBSYSTEM_CONSOLE, SUBSYSTEM_WINDOWS};

#define VERBOSE_LABELS		1
#define VERBOSE_IMPORTS		2
#define VERBOSE_MODELS		4
#define VERBOSE_FUNCTIONS	8

#define CRINKLER_TITLE			"Crinkler 1.0 (" __DATE__ ") (c) 2005-2006 Aske Simon Christensen & Rune Stubbe"
#define CRINKLER_LINKER_VERSION	"10"

class Crinkler {
	MultiLoader				m_hunkLoader;
	HunkList				m_hunkPool;
	std::string				m_entry;
	SubsystemType			m_subsytem;
	int						m_imageBase;
	int						m_hashsize;
	int						m_hashtries;
	int						m_hunktries;
	int						m_verboseFlags;
	bool					m_useSafeImporting;
	CompressionType			m_compressionType;
	std::vector<std::string>	m_rangeDlls;
	bool					m_showProgressBar;
	CompositeTransform		m_transform;

	
	Symbol* getEntrySymbol() const;
	Symbol* findUndecoratedSymbol(const char* name) const;

public:
	Crinkler();
	~Crinkler();

	void load(const char* filename);
	void load(const char* data, int size, const char* module);

	void link(const char* filename);

	Crinkler* setEntry(const char* entry);
	Crinkler* setVerboseFlags(int verboseFlags);
	Crinkler* setSubsystem(SubsystemType subsystem);
	Crinkler* setHashsize(int hashsize);
	Crinkler* setHunktries(int hunktries);
	Crinkler* setCompressionType(CompressionType compressionType);
	Crinkler* setImportingType(bool safe);
	Crinkler* setHashtries(int hashtries);
	Crinkler* addRangeDll(const char* dllname);
	Crinkler* replaceDll(const char* dll1, const char* dll2);
	Crinkler* clearRangeDlls();
	Crinkler* addRangeDlls(std::list<std::string>& dllnames);
	Crinkler* showProgressBar(bool show);

	Crinkler* addTransform(Transform* transform);
};

#endif