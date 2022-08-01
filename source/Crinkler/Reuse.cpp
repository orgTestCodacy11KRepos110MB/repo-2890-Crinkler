#include "Reuse.h"

#include "Log.h"
#include "MemoryFile.h"
#include "StringMisc.h"

enum State {
	INITIAL, CODE_MODELS, DATA_MODELS, CODE_HUNKS, DATA_HUNKS, BSS_HUNKS, HASHSIZE
};

#define CODE_MODELS_TAG "# Code models"
#define DATA_MODELS_TAG "# Data models"
#define CODE_HUNKS_TAG "# Code sections"
#define DATA_HUNKS_TAG "# Data sections"
#define BSS_HUNKS_TAG "# Uninitialized sections"
#define HASHSIZE_TAG "# Hash table size"

static ModelList4k *ParseModelList(const char *line) {
	ModelList4k *ml = new ModelList4k();
	int mask, weight, n;
	while (sscanf(line, " %x:%d%n", &mask, &weight, &n) == 2) {
		ml->AddModel(Model{ (unsigned char)weight, (unsigned char)mask });
		line += n;
	}
	return ml;
}

Reuse::Reuse() : m_code_models(nullptr), m_data_models(nullptr), m_hashsize(0) {}

Reuse::Reuse(const ModelList4k& code_models, const ModelList4k& data_models, const PartList& parts, int hashsize) {
	m_code_models = new ModelList4k(code_models);
	m_data_models = new ModelList4k(data_models);
	/*
	// REFACTOR_TODO
	for (int h = 0; h < hl.GetNumHunks(); h++) {
		Hunk *hunk = hl[h];
		const std::string& id = hunk->GetID();
		if (hunk->GetRawSize() == 0) {
			m_bss_hunk_ids.push_back(id);
		}
		else if (hunk->GetFlags() & HUNK_IS_CODE) {
			m_code_hunk_ids.push_back(id);
		}
		else {
			m_data_hunk_ids.push_back(id);
		}
	}
	*/
	m_hashsize = hashsize;
}

Reuse* LoadReuseFile(const char *filename) {
	MemoryFile mf(filename, false);
	if (mf.GetPtr() == nullptr) return nullptr;
	Reuse *reuse = new Reuse();
	State state = INITIAL;
	for (auto line : IntoLines(mf.GetPtr(), mf.GetSize())) {
		if (line.empty()) continue;
		if (line == CODE_MODELS_TAG) {
			state = CODE_MODELS;
		}
		else if (line == DATA_MODELS_TAG) {
			state = DATA_MODELS;
		}
		else if (line == CODE_HUNKS_TAG) {
			state = CODE_HUNKS;
		}
		else if (line == DATA_HUNKS_TAG) {
			state = DATA_HUNKS;
		}
		else if (line == BSS_HUNKS_TAG) {
			state = BSS_HUNKS;
		}
		else if (line == HASHSIZE_TAG) {
			state = HASHSIZE;
		}
		else if (line[0] == '#') {
			Log::Warning(filename, "Unknown reuse file tag: %s", line.c_str());
		}
		else switch (state) {
		case CODE_MODELS:
			reuse->m_code_models = ParseModelList(line.c_str());
			break;
		case DATA_MODELS:
			reuse->m_data_models = ParseModelList(line.c_str());
			break;
		case CODE_HUNKS:
			reuse->m_code_hunk_ids.push_back(line);
			break;
		case DATA_HUNKS:
			reuse->m_data_hunk_ids.push_back(line);
			break;
		case BSS_HUNKS:
			reuse->m_bss_hunk_ids.push_back(line);
			break;
		case HASHSIZE:
			sscanf(line.c_str(), " %d", &reuse->m_hashsize);
			break;
		}
	}
	return reuse;
}

void Reuse::Save(const char *filename) const {
	FILE* f = fopen(filename, "w");
	fprintf(f, "\n%s\n", CODE_MODELS_TAG);
	m_code_models->Print(f);
	fprintf(f, "\n%s\n", DATA_MODELS_TAG);
	m_data_models->Print(f);
	fprintf(f, "\n%s\n", CODE_HUNKS_TAG);
	for (auto id : m_code_hunk_ids) {
		fprintf(f, "%s\n", id.c_str());
	}
	fprintf(f, "\n%s\n", DATA_HUNKS_TAG);
	for (auto id : m_data_hunk_ids) {
		fprintf(f, "%s\n", id.c_str());
	}
	fprintf(f, "\n%s\n", BSS_HUNKS_TAG);
	for (auto id : m_bss_hunk_ids) {
		fprintf(f, "%s\n", id.c_str());
	}
	fprintf(f, "\n%s\n", HASHSIZE_TAG);
	fprintf(f, "%d\n", m_hashsize);
	fclose(f);
}
