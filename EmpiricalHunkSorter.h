#pragma once
#ifndef _EMPIRICAL_HUNK_SORTER_H_
#define _EMPIRICAL_HUNK_SORTER_H_

class HunkList;
class ModelList;
class ModelList1k;
class ProgressBar;
class Transform;
class EmpiricalHunkSorter {
	static int tryHunkCombination(HunkList* hunklist, Transform& transform, ModelList& codeModels, ModelList& dataModels, ModelList1k& models1k, int baseprob, bool saturate, bool use1KMode, int* out_size1, int* out_size2);
public:
	EmpiricalHunkSorter();
	~EmpiricalHunkSorter();

	static int sortHunkList(HunkList* hunklist, Transform& transform, ModelList& codeModels, ModelList& dataModels, ModelList1k& models1k, int baseprob, bool saturate, int numIterations, ProgressBar* progress, bool use1KMode, int* out_size1, int* out_size2);
};

#endif