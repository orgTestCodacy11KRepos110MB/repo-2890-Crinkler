#pragma once

#include <vector>
#include <string>

#include "../Compressor/ModelList.h"
#include "Hunk.h"
#include "PartList.h"
#include "ExplicitHunkSorter.h"

enum ReuseType {
	REUSE_OFF, REUSE_WRITE, REUSE_IMPROVE, REUSE_STABLE
};

static const char *ReuseTypeName(ReuseType mode) {
	switch (mode) {
	case REUSE_OFF:
		return "OFF";
	case REUSE_WRITE:
		return "WRITE";
	case REUSE_IMPROVE:
		return "IMPROVE";
	case REUSE_STABLE:
		return "STABLE";
	}
	return "";
}

class Reuse {
	ModelList4k *m_code_models;
	ModelList4k *m_data_models;

	std::vector<std::string> m_code_hunk_ids;
	std::vector<std::string> m_data_hunk_ids;
	std::vector<std::string> m_bss_hunk_ids;

	int m_hashsize;

	friend Reuse* LoadReuseFile(const char *filename);
	friend void ExplicitHunkSorter::SortHunks(PartList& parts, Reuse *reuse);

public:
	Reuse();
	Reuse(const ModelList4k& code_models, const ModelList4k& data_models, const PartList& parts, int hashsize);

	const ModelList4k*	GetCodeModels() const { return m_code_models; }
	const ModelList4k*	GetDataModels() const { return m_data_models; }
	int					GetHashSize() const { return m_hashsize; }

	void				Save(const char* filename) const;
};

Reuse* LoadReuseFile(const char *filename);
