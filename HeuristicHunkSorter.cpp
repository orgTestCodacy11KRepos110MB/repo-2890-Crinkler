#include "HeuristicHunkSorter.h"
#include "HunkList.h"
#include "Hunk.h"
#include <vector>
#include <algorithm>

using namespace std;

bool hunkRelation(Hunk* h1, Hunk* h2) {
	//initialized data < uninitialized data
	if((h1->getRawSize() != 0) != (h2->getRawSize() != 0))
		return h1->getRawSize() > h2->getRawSize();

	//code < non-code
	if((h1->getFlags() & HUNK_IS_CODE) != (h2->getFlags() & HUNK_IS_CODE))
		return (h1->getFlags() & HUNK_IS_CODE);

	if(strcmp(h1->getName(), "ImportListHunk")==0)
		return true;
	if(strcmp(h2->getName(), "ImportListHunk")==0)
		return false;

	if(h1->getRawSize() == 0)
		return h1->getVirtualSize() < h2->getVirtualSize();

	if(h1->getAlignmentBits() == h2->getAlignmentBits()) {
		if(h1->getRawSize() != h2->getRawSize())
			return h1->getRawSize() > h2->getRawSize();
		else {
			{
				//compare data
				return memcmp(h1->getPtr(), h2->getPtr(), h1->getRawSize()) > 0;
			}
		}				
	}
	return h1->getAlignmentBits() < h2->getAlignmentBits();
}

void HeuristicHunkSorter::sortHunkList(HunkList* hunklist) {
	vector<Hunk*> hunks;
	vector<Hunk*> fixedHunks;

	//move hunks to vector
	for(int i = 0; i < hunklist->getNumHunks(); i++) {
		Hunk* h = (*hunklist)[i];
		if(h->getFlags() & HUNK_IS_FIXED)
			fixedHunks.push_back(h);
		else
			hunks.push_back(h);
	}
	hunklist->clear();

	//sort hunks
	sort(hunks.begin(), hunks.end(), hunkRelation);

	//copy back hunks to hunklist
	for(vector<Hunk*>::const_iterator it = fixedHunks.begin(); it != fixedHunks.end(); it++)
		hunklist->addHunkBack(*it);
	for(vector<Hunk*>::const_iterator it = hunks.begin(); it != hunks.end(); it++)
		hunklist->addHunkBack(*it);	
}