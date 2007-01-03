#include <cassert>

#include "HunkList.h"
#include "Hunk.h"
#include "Log.h"
#include "misc.h"

using namespace std;

#include <stack>
#include <set>
#include <algorithm>


HunkList::HunkList() {
}

HunkList::~HunkList() {
	//free hunks
	for(vector<Hunk*>::iterator it = m_hunks.begin(); it != m_hunks.end(); it++)
		delete *it;
}

Hunk*& HunkList::operator[] (unsigned idx) {
	assert(idx < (int)m_hunks.size());
	return m_hunks[idx];
}

Hunk* const & HunkList::operator[] (unsigned idx) const {
	assert(idx < (int)m_hunks.size());
	return m_hunks[idx];
}

void HunkList::addHunkBack(Hunk* hunk) {
	m_hunks.push_back(hunk);
}

void HunkList::addHunkFront(Hunk* hunk) {
	m_hunks.insert(m_hunks.begin(), hunk);
}

void HunkList::insertHunk(int index, Hunk* hunk) {
	m_hunks.insert(m_hunks.begin() + index, hunk);
}

Hunk* HunkList::removeHunk(Hunk* hunk) {
	vector<Hunk*>::iterator it = find(m_hunks.begin(), m_hunks.end(), hunk);
	if(it != m_hunks.end())
		m_hunks.erase(it);
	return hunk;
}


void HunkList::clear() {
	m_hunks.clear();
}

int HunkList::getNumHunks() const {
	return (int)m_hunks.size();
}

void HunkList::setHunk(int index, Hunk* h) {
	assert(index >= 0 && index < (int)m_hunks.size());
	m_hunks[index] = h;
}


void HunkList::append(HunkList* hunklist) {
	for(vector<Hunk*>::const_iterator it = hunklist->m_hunks.begin(); it != hunklist->m_hunks.end(); it++) {
		m_hunks.push_back(new Hunk(*(*it)));
	}
}

Hunk* HunkList::toHunk(const char* name, int* splittingPoint) const {
	//calculate raw size
	int rawsize = 0;
	int virtualsize = 0;
	int alignmentBits = 0;
	unsigned int flags = 0;
	for(vector<Hunk*>::const_iterator it = m_hunks.begin(); it != m_hunks.end(); it++) {
		Hunk* h = *it;
		virtualsize = align(virtualsize, h->getAlignmentBits());
		if(h->getRawSize() > 0)
			rawsize = virtualsize + h->getRawSize();
		virtualsize += h->getVirtualSize();
		alignmentBits = max(alignmentBits, h->getAlignmentBits());
		if(h->getFlags() & HUNK_IS_CODE)
			flags |= HUNK_IS_CODE;
		if(h->getFlags() & HUNK_IS_WRITEABLE)
			flags |= HUNK_IS_WRITEABLE;
	}

	//copy data
	Hunk* newHunk = new Hunk(name, 0, flags, alignmentBits, rawsize, virtualsize);
	int address = 0;
	if(splittingPoint != NULL)
		*splittingPoint = -1;

	for(vector<Hunk*>::const_iterator it = m_hunks.begin(); it != m_hunks.end(); it++) {
		Hunk* h = *it;
		address = align(address, h->getAlignmentBits());

		//copy symbols
		for(map<string, Symbol*>::const_iterator jt = h->m_symbols.begin(); jt != h->m_symbols.end(); jt++) {
			Symbol* s = new Symbol(*jt->second);
			s->hunk = newHunk;
			if(s->flags & SYMBOL_IS_RELOCATEABLE) {
				s->value += address;
			}
			newHunk->addSymbol(s);
		}

		//copy relocations
		for(list<relocation>::const_iterator jt = h->m_relocations.begin(); jt != h->m_relocations.end(); jt++) {
			relocation r = *jt;
			r.offset += address;
			newHunk->addRelocation(r);
		}

		//TODO: what if it is all code?
		if(splittingPoint && *splittingPoint == -1 && !(h->getFlags() & HUNK_IS_CODE))
			*splittingPoint = address;

		memcpy(&newHunk->getPtr()[address], h->getPtr(), h->getRawSize());
		address += h->getVirtualSize();
	}
	newHunk->trim();

	return newHunk;
}


Symbol* HunkList::findUndecoratedSymbol(const char* name) const {
	for(vector<Hunk*>::const_iterator it = m_hunks.begin(); it != m_hunks.end(); it++) {
		Symbol* s = (*it)->findUndecoratedSymbol(name);
		if(s != NULL)
			return s;
	}
	return NULL;
}

Symbol* HunkList::findSymbol(const char* name) const {
	for(vector<Hunk*>::const_iterator it = m_hunks.begin(); it != m_hunks.end(); it++) {
		Symbol* s = (*it)->findSymbol(name);
		if(s != NULL)
			return s;
	}
	return NULL;
}

void HunkList::removeUnreferencedHunks(list<Hunk*> startHunks) {
	//make a combined symbol table for all hunks to speed up search
	map<string, Hunk*> combinedSymbolTable;
	for(vector<Hunk*>::const_iterator it = m_hunks.begin(); it != m_hunks.end(); it++) {
		Hunk* h = *it;
		for(map<string, Symbol*>::const_iterator jt = h->m_symbols.begin(); jt != h->m_symbols.end(); jt++) {
			combinedSymbolTable[jt->first] = h;
		}
	}

	stack<Hunk*> stak;
	for(list<Hunk*>::iterator it = startHunks.begin(); it != startHunks.end(); it++) {
		(*it)->m_numReferences++;
		stak.push(*it);
	}

	//mark reachable hunks
	while(stak.size() > 0) {
		Hunk* h = stak.top();
		stak.pop();

		for(list<relocation>::iterator it = h->m_relocations.begin(); it != h->m_relocations.end(); it++) {
			map<string, Hunk*>::const_iterator p = combinedSymbolTable.find(it->symbolname);
			if(p != combinedSymbolTable.end()) {
				if(p->second->m_numReferences++ == 0) {
					stak.push(p->second);
				}
			}
		}
	}

	//delete unreferenced hunks
	for(vector<Hunk*>::iterator it = m_hunks.begin(); it != m_hunks.end();) {
		if((*it)->getNumReferences() == 0) {
			delete *it;
			it = m_hunks.erase(it);
		} else {
			it++;
		}
	}
}

void HunkList::removeImportHunks() {
	//delete import hunks
	for(vector<Hunk*>::iterator it = m_hunks.begin(); it != m_hunks.end();) {
		if((*it)->getFlags() & HUNK_IS_IMPORT) {
			delete *it;
			it = m_hunks.erase(it);
		} else {
			it++;
		}
	}
}

void HunkList::trim() {
	for(vector<Hunk*>::iterator it = m_hunks.begin(); it != m_hunks.end(); it++)
		(*it)->trim();
}

void HunkList::printHunks() {
	for(vector<Hunk*>::iterator it = m_hunks.begin(); it != m_hunks.end(); it++)
		(*it)->printSymbols();
}