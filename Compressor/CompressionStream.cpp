#include <memory>
#include <windows.h>
#include "model.h"
#include "aritcode.h"
#include "common.h"
#include "CompressionStream.h"
#include "..\misc.h"
#include <ctime>
#include <cstdio>
#include <mmintrin.h>

using namespace std;

struct Weights;

const int MAX_CONTEXT_LENGTH = 8;

void updateWeights(Weights *w, int bit);

static int previousPowerOf2(int v) {
	v--;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	return v+1;
}

struct TinyHashEntry {
	unsigned int hash;
	unsigned char prob[2];
	unsigned char used;
};

void CompressionStream::Compress(const unsigned char* d, int size, const ModelList& models, int baseprobs[8], int hashsize, bool finish) {
	hashsize /= 2;
	int bitlength = size*8;
	unsigned char* data = new unsigned char[size+MAX_CONTEXT_LENGTH];
	memcpy(data, m_context, MAX_CONTEXT_LENGTH);
	data += MAX_CONTEXT_LENGTH;
	memcpy(data, d, size);
		
	unsigned int weightmasks[256];
	unsigned char masks[256];
	int nmodels = models.nmodels;
	unsigned int w = models.getMaskList(masks, finish);
	int weights[256];

	int n = 0;
	int v = 0;
	while(w != 0 && n < models.nmodels) {
		while (w & 0x80000000) {
			w <<= 1;
			v++;
		}
		w <<= 1;
		weights[n] = v;
		weightmasks[n] = (int)masks[n] | (w & 0xFFFFFF00);
		n++;
	}

	unsigned int tinyhashsize = previousPowerOf2(bitlength*nmodels);
	TinyHashEntry* hashtable = new TinyHashEntry[tinyhashsize];
	memset(hashtable, 0, tinyhashsize*sizeof(TinyHashEntry));
	TinyHashEntry* hashEntries[32];

	for(int bitpos = 0 ; bitpos < bitlength; bitpos++) {
		int bit = GetBit(data, bitpos);

		if((bitpos&7)==0 && m_sizefillptr) {
			*m_sizefillptr++ = AritCodePos(&m_aritstate)/(BITPREC_TABLE/BITPREC);
		}

		// Query models
		unsigned int probs[2] = { baseprobs[bitpos&7], baseprobs[bitpos&7]};
		for(int m = 0 ; m < nmodels; m++) {
			unsigned int hash = ModelHash(data, bitpos, weightmasks[m]) % hashsize;
			unsigned int tinyHash = hash & (tinyhashsize-1);
			TinyHashEntry *he = &hashtable[tinyHash];
			while(he->hash != hash && he->used == 1) {
				tinyHash++;
				if(tinyHash >= tinyhashsize)
					tinyHash = 0;
				he = &hashtable[tinyHash];
			}
			
			he->hash = hash;
			he->used = 1;
			hashEntries[m] = he;

			int fac = weights[m];
			unsigned int shift = (1 - (((he->prob[0]+255)&(he->prob[1]+255)) >> 8))*2 + fac;
			probs[0] += ((unsigned int)he->prob[0] << shift);
			probs[1] += ((unsigned int)he->prob[1] << shift);
		}

		// Encode bit
		AritCode(&m_aritstate, probs[0], probs[1], bit);

		// Update models
		for(int m = 0; m < models.nmodels; m++) {
			updateWeights((Weights*)hashEntries[m]->prob, bit);
		}
	}

	if(m_sizefillptr) {
		*m_sizefillptr++ = AritCodePos(&m_aritstate)/(BITPREC_TABLE/BITPREC);
	}

	delete[] hashtable;
	{	//save context for next call
		int s = min(size, MAX_CONTEXT_LENGTH);
		if(s > 0)
			memcpy(m_context+8-s, data+size-s, s);
	}

	data -= MAX_CONTEXT_LENGTH;
	delete[] data;
}

int CompressionStream::EvaluateSize(const unsigned char* d, int size, const ModelList& models, int baseprobs[8], char* context) {
	int bitlength = size*8;
	unsigned char* data = new unsigned char[size+MAX_CONTEXT_LENGTH];
	memcpy(data, context, MAX_CONTEXT_LENGTH);
	data += MAX_CONTEXT_LENGTH;
	memcpy(data, d, size);

	unsigned int weightmasks[256];
	unsigned char masks[256];
	int nmodels = models.nmodels;
	unsigned int w = models.getMaskList(masks, false);
	int weights[256];

	int n = 0;
	int v = 0;
	while(w != 0 && n < models.nmodels) {
		while (w & 0x80000000) {
			w <<= 1;
			v++;
		}
		w <<= 1;
		weights[n] = v;
		weightmasks[n] = (int)masks[n] | (w & 0xFFFFFF00);
		n++;
	}

	unsigned int* sums = new unsigned int[bitlength*2];	//summed predictions
	for(int i = 0; i < bitlength; i++) {
		sums[i*2] = baseprobs[i & 7];
		sums[i*2+1] = baseprobs[i & 7];
	}

	unsigned int tinyhashsize = previousPowerOf2(bitlength*2);
	TinyHashEntry* hashtable = new TinyHashEntry[tinyhashsize];

	for(int modeli = 0; modeli < nmodels; modeli++) {
		//clear hashtable
		memset(hashtable, 0, tinyhashsize*sizeof(TinyHashEntry));

		for(int bitpos = 0; bitpos < bitlength; bitpos++) {
			int bit = GetBit(data, bitpos);

			unsigned int hash = ModelHash(data, bitpos, weightmasks[modeli]);
			unsigned int tinyHash = hash & (tinyhashsize-1);
			TinyHashEntry *he = &hashtable[tinyHash];
			while(he->hash != hash && he->used == 1) {
				tinyHash++;
				if(tinyHash >= tinyhashsize)
					tinyHash = 0;
				he = &hashtable[tinyHash];
			}

			he->hash = hash;
			he->used = 1;

			int fac = weights[modeli];
			unsigned int shift = (1 - (((he->prob[0]+255)&(he->prob[1]+255)) >> 8))*2 + fac;
			sums[bitpos*2+0] += ((int)he->prob[0] << shift);
			sums[bitpos*2+1] += ((int)he->prob[1] << shift);
			updateWeights((Weights*)he->prob, bit);
		}
	}

	//sum 
	long long totalsize = 0;
	#pragma omp parallel for reduction(+:totalsize)
	for(int bitpos = 0; bitpos < bitlength; bitpos++) {
		int bit = GetBit(data, bitpos);
		totalsize += AritSize2(sums[bitpos*2+bit], sums[bitpos*2+!bit]);
	}

	delete[] hashtable;

	data -= MAX_CONTEXT_LENGTH;
	delete[] data;
	delete[] sums;

	return (int) (totalsize / (BITPREC_TABLE / BITPREC));
}

inline unsigned int QuickHash(const byte *data, int pos, __m64 mask, int bytemask) {
	__m64 contextdata = *(__m64 *)&data[pos-8];
	__m64 scrambler = _mm_set_pi8(23,5,17,13,11,7,19,3);
	__m64 sample = _mm_mullo_pi16(_mm_and_si64(contextdata, mask), scrambler);
	unsigned int contexthash1 = _mm_cvtsi64_si32(sample);
	unsigned int contexthash2 = _mm_cvtsi64_si32(_mm_srli_si64(sample, 32));
	unsigned int contexthash = contexthash1 ^ contexthash2;
	unsigned char databyte = (unsigned char)(data[pos] & bytemask);
	return contexthash + ((unsigned int)databyte);
}

int CompressionStream::EvaluateSizeQuick(const unsigned char* d, int size, const ModelList& models, int baseprobs[8], char* context, int bitpos) {
	int bitlength = size*8;
	unsigned char* data = new unsigned char[size+MAX_CONTEXT_LENGTH];
	memcpy(data, context, MAX_CONTEXT_LENGTH);
	data += MAX_CONTEXT_LENGTH;
	memcpy(data, d, size);

	unsigned int tinyhashsize = previousPowerOf2(size*2);
	unsigned int recths = ((1<<31)/tinyhashsize-1)*2+1;
	unsigned int recthslog = 0;
	while((1<<recthslog) <= recths) recthslog++;
	TinyHashEntry* hashtable = new TinyHashEntry[tinyhashsize];

	unsigned int* sums = new unsigned int[size*2];	//summed predictions

	for(int i = 0; i < size; i++) {
		sums[i*2] = baseprobs[bitpos];
		sums[i*2+1] = baseprobs[bitpos];
	}

	int bytemask = (0xff00 >> bitpos);
	int nmodels = models.nmodels; 
	for(int modeli = 0; modeli < nmodels; modeli++) {
		//clear hashtable
		memset(hashtable, 0, tinyhashsize*sizeof(TinyHashEntry));

		int weight = models[modeli].weight;
		unsigned char w = (unsigned char)models[modeli].mask;
		unsigned char maskbytes[8];
		for (int i = 0 ; i < 8 ; i++) {
			maskbytes[i] = ((w >> i) & 1) * 0xff;
		}
		__m64 mask = *(__m64 *)maskbytes;

		for(int pos = 0; pos < size; pos++) {
			int bit = (data[pos] >> (7-bitpos)) & 1;

			unsigned int hash = QuickHash(data, pos, mask, bytemask);
			unsigned int tinyHash = (hash*recths)>>recthslog;
			//unsigned int tinyHash = hash & (tinyhashsize-1);
			TinyHashEntry *he = &hashtable[tinyHash];
			while(he->hash != hash && he->used == 1) {
				tinyHash++;
				if(tinyHash >= tinyhashsize)
					tinyHash = 0;
				he = &hashtable[tinyHash];
			}

			he->hash = hash;
			he->used = 1;

			int fac = weight;
			unsigned int shift = (1 - (((he->prob[0]+255)&(he->prob[1]+255)) >> 8))*2 + fac;
			sums[pos*2+0] += ((int)he->prob[0] << shift);
			sums[pos*2+1] += ((int)he->prob[1] << shift);

			//update weights
			he->prob[bit] += 1;
			if (he->prob[!bit] > 1) he->prob[!bit] >>= 1;
		}
	}

	//sum 
	long long totalsize = 0;
	#pragma omp parallel for reduction(+:totalsize)
	for(int pos = 0; pos < size; pos++) {
		int bit = (data[pos] >> (7-bitpos)) & 1;
		totalsize += AritSize2(sums[pos*2+bit], sums[pos*2+!bit]);
	}
	
	_mm_empty();

	delete[] hashtable;

	data -= MAX_CONTEXT_LENGTH;
	delete[] data;
	delete[] sums;

	return (int) (totalsize / (BITPREC_TABLE / BITPREC));
}

inline unsigned int QuickHash_OLD(const byte *data, int bitpos, __m64 mask) {
	int bytepos = bitpos >> 3;
	int subbit = bitpos & 7;
	__m64 contextdata = *(__m64 *)&data[bytepos-8];
	__m64 scrambler = _mm_set_pi8(23,5,17,13,11,7,19,3);
	__m64 sample = _mm_mullo_pi16(_mm_and_si64(contextdata, mask), scrambler);
	unsigned int contexthash1 = _mm_cvtsi64_si32(sample);
	unsigned int contexthash2 = _mm_cvtsi64_si32(_mm_srli_si64(sample, 32));
	unsigned int contexthash = contexthash1 ^ contexthash2;
	unsigned char databyte = (unsigned char)(data[bytepos] & (0xff00 >> subbit));
	unsigned int hash = contexthash + (contexthash << subbit) + ((((unsigned int)databyte)+1) << subbit) + subbit;
	//if (contexthash1 != 0 && subbit == 0) printf("(%d %d %d) ", bitpos, contexthash1, contexthash2);
	return hash;
}

int CompressionStream::EvaluateSizeQuick_OLD(const unsigned char* d, int size, const ModelList& models, int baseprobs[8], char* context) {
	int bitlength = size*8;
	unsigned char* data = new unsigned char[size+MAX_CONTEXT_LENGTH];
	memcpy(data, context, MAX_CONTEXT_LENGTH);
	data += MAX_CONTEXT_LENGTH;
	memcpy(data, d, size);

	unsigned int weightmasks[256];
	unsigned char masks[256];
	int nmodels = models.nmodels;
	unsigned int w = models.getMaskList(masks, false);
	int weights[256];

	int n = 0;
	int v = 0;
	while(w != 0 && n < models.nmodels) {
		while (w & 0x80000000) {
			w <<= 1;
			v++;
		}
		w <<= 1;
		weights[n] = v;
		weightmasks[n] = (int)masks[n] | (w & 0xFFFFFF00);
		n++;
	}

	unsigned int* sums = new unsigned int[bitlength*2];	//summed predictions
	for(int i = 0; i < bitlength; i++) {
		sums[i*2] = baseprobs[i & 7];
		sums[i*2+1] = baseprobs[i & 7];
	}

	unsigned int tinyhashsize = previousPowerOf2(bitlength*2);
	unsigned int recths = ((1<<31)/tinyhashsize-1)*2+1;
	unsigned int recthslog = 0;
	while((1<<recthslog) <= recths) recthslog++;
	TinyHashEntry* hashtable = new TinyHashEntry[tinyhashsize];

	for(int modeli = 0; modeli < nmodels; modeli++) {
		//clear hashtable
		memset(hashtable, 0, tinyhashsize*sizeof(TinyHashEntry));

		unsigned char w = (unsigned char)weightmasks[modeli];
		unsigned char maskbytes[8];
		for (int i = 0 ; i < 8 ; i++) {
			maskbytes[i] = ((w >> i) & 1) * 0xff;
		}
		__m64 mask = *(__m64 *)maskbytes;

		for(int bitpos = 0; bitpos < bitlength; bitpos++) {
			int bit = GetBit(data, bitpos);

			unsigned int hash = QuickHash_OLD(data, bitpos, mask);
			unsigned int tinyHash = (hash*recths)>>recthslog;
			//unsigned int tinyHash = hash & (tinyhashsize-1);
			TinyHashEntry *he = &hashtable[tinyHash];
			while(he->hash != hash && he->used == 1) {
				tinyHash++;
				if(tinyHash >= tinyhashsize)
					tinyHash = 0;
				he = &hashtable[tinyHash];
			}

			he->hash = hash;
			he->used = 1;

			int fac = weights[modeli];
			unsigned int shift = (1 - (((he->prob[0]+255)&(he->prob[1]+255)) >> 8))*2 + fac;
			sums[bitpos*2+0] += ((int)he->prob[0] << shift);
			sums[bitpos*2+1] += ((int)he->prob[1] << shift);
			updateWeights((Weights*)he->prob, bit);
		}
	}

	_mm_empty();

	//sum 
	long long totalsize = 0;
#pragma omp parallel for reduction(+:totalsize)
	for(int bitpos = 0; bitpos < bitlength; bitpos++) {
		int bit = GetBit(data, bitpos);
		totalsize += AritSize2(sums[bitpos*2+bit], sums[bitpos*2+!bit]);
	}

	delete[] hashtable;

	data -= MAX_CONTEXT_LENGTH;
	delete[] data;
	delete[] sums;

	return (int) (totalsize / (BITPREC_TABLE / BITPREC));
}


CompressionStream::CompressionStream(unsigned char* data, int* sizefill, int maxsize) :
	m_data(data), m_sizefill(sizefill), m_sizefillptr(sizefill), m_maxsize(maxsize)
{
	if(data != NULL) {
		memset(m_data, 0, m_maxsize);
		AritCodeInit(&m_aritstate, m_data);
	}
	
	memset(m_context, 0, MAX_CONTEXT_LENGTH);
}

CompressionStream::~CompressionStream() {
}

int CompressionStream::close(void) {
	return (AritCodeEnd(&m_aritstate) + 7) / 8;
}