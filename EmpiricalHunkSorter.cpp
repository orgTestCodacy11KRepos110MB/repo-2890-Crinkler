#include "EmpiricalHunkSorter.h"
#include <ctime>
#include <cmath>
#include <ppl.h>
#include "HunkList.h"
#include "Hunk.h"
#include "Compressor/CompressionStream.h"
#include "Compressor/ProgressBar.h"
#include "Crinkler.h"

using namespace std;

EmpiricalHunkSorter::EmpiricalHunkSorter() {
}

EmpiricalHunkSorter::~EmpiricalHunkSorter() {
}

int EmpiricalHunkSorter::tryHunkCombination(HunkList* hunklist, Transform& transform, ModelList& codeModels, ModelList& dataModels, ModelList1k& models1k, int baseprob, bool saturate, bool use1KMode, int* out_size1, int* out_size2)
{
	int splittingPoint;

	Hunk* phase1;
	Symbol* import = hunklist->findSymbol("_Import");
	transform.linkAndTransform(hunklist, import, CRINKLER_CODEBASE, phase1, NULL, &splittingPoint, false);


	int totalsize = 0;
	if (use1KMode)
	{
		int max_size = phase1->getRawSize() * 2 + 1000;
		unsigned char* compressed_data_ptr = new unsigned char[max_size];
		Compress1K((unsigned char*)phase1->getPtr(), phase1->getRawSize(), compressed_data_ptr, max_size, models1k.boost, models1k.baseprob0, models1k.baseprob1, models1k.modelmask, nullptr, &totalsize);	//TODO: estimate instead of compress
		delete[] compressed_data_ptr;

		if(out_size1) *out_size1 = totalsize;
		if(out_size2) *out_size2 = 0;
	}
	else
	{
		CompressionStream cs(NULL, NULL, 0, saturate);
		int sizes[16];

		char contexts[2][8];
		memset(contexts[0], 0, 8);
		memset(contexts[1], 0, 8);
		int context_size = 8;
		if (splittingPoint < 8) context_size = splittingPoint;
		memcpy(contexts[1] + (8 - context_size), phase1->getPtr() + splittingPoint - context_size, context_size);

		concurrency::parallel_for(0, 16, [&](int i)
		{
			if (i < 8)
				sizes[i] = cs.EvaluateSize((unsigned char*)phase1->getPtr(), splittingPoint, codeModels, baseprob, contexts[0], i);
			else
				sizes[i] = cs.EvaluateSize((unsigned char*)phase1->getPtr() + splittingPoint, phase1->getRawSize() - splittingPoint, dataModels, baseprob, contexts[1], i - 8);
		});

		delete phase1;

		int size1 = codeModels.nmodels * 8 * BITPREC;
		int size2 = dataModels.nmodels * 8 * BITPREC;
		for(int i = 0; i < 8; i++)
		{
			size1 += sizes[i];
			size2 += sizes[i+8];
		}

		totalsize = size1 + size2;
		if(out_size1) *out_size1 = size1;
		if(out_size2) *out_size2 = size2;
	}

	return totalsize;
}

void permuteHunklist(HunkList* hunklist, int strength) {
	int n_permutes = (rand() % strength) + 1;
	for (int p = 0 ; p < n_permutes ; p++)
	{
		int h1i, h2i;
		int sections[3];
		int nHunks = hunklist->getNumHunks();
		int codeHunks = 0;
		int dataHunks = 0;
		int uninitHunks = 0;

		{
			//count different types of hunks
			codeHunks = 0;
			while(codeHunks < nHunks && (*hunklist)[codeHunks]->getFlags() & HUNK_IS_CODE)
				codeHunks++;
			dataHunks = codeHunks;
			while(dataHunks < nHunks && (*hunklist)[dataHunks]->getRawSize() > 0)
				dataHunks++;
			uninitHunks = nHunks - dataHunks;
			dataHunks -= codeHunks;
			if (codeHunks < 2 && dataHunks < 2 && uninitHunks < 2) return;
			sections[0] = codeHunks;
			sections[1] = dataHunks;
			sections[2] = uninitHunks;
		}

		int s;
		do {
			s = rand() % 3;
		} while (sections[s] < 2);
		int max_n = sections[s]/2;
		if (max_n > strength) max_n = strength;
		int n = (rand() % max_n) + 1;
		h1i = rand() % (sections[s] - n + 1);
		do {
			h2i = rand() % (sections[s] - n + 1);
		} while (h2i == h1i);
		int base = (s > 0 ? sections[0] : 0) + (s > 1 ? sections[1] : 0);

		if (h2i < h1i)
		{
			// Insert before
			for (int i = 0 ; i < n ; i++)
			{
				hunklist->insertHunk(base+h2i+i, hunklist->removeHunk((*hunklist)[base+h1i+i]));
			}
		} else {
			// Insert after
			for (int i = 0 ; i < n ; i++)
			{
				hunklist->insertHunk(base+h2i+n-1, hunklist->removeHunk((*hunklist)[base+h1i]));
			}
		}
	}
}

void randomPermute(HunkList* hunklist) {
	bool done = false;

	int sections[3];
	int nHunks = hunklist->getNumHunks();
	int codeHunks = 0;
	int dataHunks = 0;
	int uninitHunks = 0;

	//count different types of hunks
	{
		codeHunks = 0;
		while(codeHunks < nHunks && (*hunklist)[codeHunks]->getFlags() & HUNK_IS_CODE)
			codeHunks++;
		dataHunks = codeHunks;
		while(dataHunks < nHunks && (*hunklist)[dataHunks]->getRawSize() > 0)
			dataHunks++;
		uninitHunks = nHunks - dataHunks;
		dataHunks -= codeHunks;
		sections[0] = codeHunks;
		sections[1] = dataHunks;
		sections[2] = uninitHunks;
	}

	int idx = 0;
	for(int j = 0; j < 3; j++) {
		for(int i = 0; i < sections[j]; i++) {
			int swapidx = rand() % (sections[j]-i);
			swap((*hunklist)[idx+i], (*hunklist)[idx+swapidx]);
		}
		idx += sections[j];
	}
}

int EmpiricalHunkSorter::sortHunkList(HunkList* hunklist, Transform& transform, ModelList& codeModels, ModelList& dataModels, ModelList1k& models1k, int baseprob, bool saturate, int numIterations, ProgressBar* progress, bool use1KMode, int* out_size1, int* out_size2)
{
	int nHunks = hunklist->getNumHunks();
	
	printf("\n\nReordering sections...\n");
	fflush(stdout);
	
	int best_size1;
	int best_size2;
	int best_total_size = tryHunkCombination(hunklist, transform, codeModels, dataModels, models1k, baseprob, saturate, use1KMode, &best_size1, &best_size2);
	if(use1KMode)
	{
		printf("  Iteration: %5d  Size: %5.2f\n", 0, best_total_size / (BITPREC * 8.0f));
	}
	else
	{
		printf("  Iteration: %5d  Code: %.2f  Data: %.2f  Size: %.2f\n", 0, best_size1 / (BITPREC * 8.0f), best_size2 / (BITPREC * 8.0f), best_total_size / (BITPREC * 8.0f));
	}
	
	if(progress)
		progress->beginTask("Reordering sections");

	Hunk** backup = new Hunk*[nHunks];
	int fails = 0;
	int stime = clock();
	for(int i = 1; i < numIterations; i++) {
		for(int j = 0; j < nHunks; j++)
			backup[j] = (*hunklist)[j];

		// save export hunk, if present
		Hunk* eh = nullptr;
		int ehi;
		for (ehi = 0; ehi < hunklist->getNumHunks(); ehi++) {
			if ((*hunklist)[ehi]->getFlags() & HUNK_IS_TRAILING) {
				eh = (*hunklist)[ehi];
				hunklist->removeHunk(eh);
				break;
			}
		}

		permuteHunklist(hunklist, 2);
		
		// restore export hunk, if present
		if (eh) {
			hunklist->insertHunk(ehi, eh);
		}
		int size1, size2;
		int total_size = tryHunkCombination(hunklist, transform, codeModels, dataModels, models1k, baseprob, saturate, use1KMode, &size1, &size2);
		//printf("size: %5.2f\n", size / (BITPREC * 8.0f));
		if(total_size < best_total_size) {
			if(use1KMode)
			{
				printf("  Iteration: %5d  Size: %5.2f\n", i, total_size / (BITPREC * 8.0f));
			}
			else
			{
				printf("  Iteration: %5d  Code: %.2f  Data: %.2f  Size: %.2f\n", i, size1 / (BITPREC * 8.0f), size2 / (BITPREC * 8.0f), total_size / (BITPREC * 8.0f));
			}
			
			fflush(stdout);
			best_total_size = total_size;
			best_size1 = size1;
			best_size2 = size2;
			fails = 0;
		} else {
			fails++;
			//restore from backup
			for(int j = 0; j < nHunks; j++)
				(*hunklist)[j] = backup[j];
		}
		if(progress)
			progress->update(i+1, numIterations);
	}
	if(progress)
		progress->endTask();

	if(out_size1) *out_size1 = best_size1;
	if(out_size2) *out_size2 = best_size2;

	delete[] backup;
	int timespent = (clock() - stime)/CLOCKS_PER_SEC;
	printf("Time spent: %dm%02ds\n", timespent/60, timespent%60);
	return best_total_size;
}