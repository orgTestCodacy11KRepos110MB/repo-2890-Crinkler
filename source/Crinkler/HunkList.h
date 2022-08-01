#pragma once
#ifndef _HUNK_LIST_H_
#define _HUNK_LIST_H_

#include <vector>
#include <functional>
#include "../Compressor/Compressor.h"

class Hunk;
class Symbol;
class PartList;

class Part
{
	std::string			m_name;
	int					m_linkedOffset;
	int					m_linkedSize;

	friend class PartList;
public:
	Part(const char* name, bool initialized) :
		m_name(name),
		m_linkedOffset(-1),
		m_linkedSize(-1),
		m_initialized(initialized),
		m_compressedSize(INT_MAX)
	{
	}
	~Part();

	std::vector<Hunk*>	m_hunks;

	ModelList1k		m_model1k;
	ModelList4k		m_model4k;
	bool			m_initialized;

	int				m_compressedSize;	//TODO: fix me

	Hunk*&			operator[] (unsigned idx);
	Hunk* const&	operator[] (unsigned idx) const;

	void			AddHunkBack(Hunk* hunk);
	void			AddHunkFront(Hunk* hunk);

	void			Clear();
	
	void			InsertHunk(int index, Hunk* hunk);
	void			RemoveHunk(Hunk* hunk);

	const char*		GetName() const { return m_name.c_str(); }
	int				GetNumHunks() const { return (int)m_hunks.size(); }
	int				GetLinkedOffset() const { return m_linkedOffset; }
	int				GetLinkedSize() const { return m_linkedSize; }
	int				GetCompressedSize() const { return m_compressedSize; }

	bool			IsInitialized() const { return m_initialized; }

	void			ForEachHunk(std::function<void(Hunk*)> fun);
	void			ForEachHunk(std::function<void(Part&, Hunk*, Hunk*)> fun);
	bool			ForEachHunkWithBreak(std::function<bool(Hunk*)> fun);
	bool			ForEachHunkWithBreak(std::function<bool(Part&, Hunk*, Hunk*)> fun);

	void			RemoveMatchingHunks(std::function<bool(Hunk*)> fun);
};

// Invariant: part list always contains at least code, data and bss parts
class PartList {
	std::vector<Part*>	m_parts;
public:
	PartList();
	~PartList();
	Part& operator[] (unsigned idx);
	Part const& operator[] (unsigned idx) const;

	void	AddPart(const char* name, bool binitialized);
	Part&	GetOrAddPart(const char* name);

	Part&	GetCodePart() const{ return *m_parts[0]; }
	Part&	GetDataPart() const { return *m_parts[1]; }
	Part&	GetUninitializedPart() const{ return *m_parts.back(); }

	int		GetNumParts() const { return (int)m_parts.size(); }
	int		GetNumInitializedParts() const { return (int)m_parts.size() - 1; }

	void	ForEachHunk(std::function<void(Hunk*)> fun);
	void	ForEachHunk(std::function<void(Part&, Hunk*, Hunk*)> fun);
	bool	ForEachHunkWithBreak(std::function<bool(Hunk*)> fun);
	bool	ForEachHunkWithBreak(std::function<bool(Part&, Hunk*, Hunk*)> fun);

	void	ForEachPart(std::function<void(Part&, int)> fun);

	Hunk*	Link(const char* name, int baseAddress);

	Symbol* FindSymbol(const char* name);
	Symbol* FindUndecoratedSymbol(const char* name);

	void	RemoveMatchingHunks(std::function<bool(Hunk*)> fun);
	void	RemoveHunk(Hunk* hunk);
	void	RemoveUnreferencedHunks(std::vector<Hunk*> startHunks);

	bool	NeedsContinuationJump(Hunk* hunk, Hunk* nextHunk) const;
};

#endif
