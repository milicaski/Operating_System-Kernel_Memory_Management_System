#include "slab.h"
#include "buddy.h"
#include <stdio.h>
#include <math.h>
#include <windows.h>
#define LOCK WaitForSingleObject(mutex,INFINITE);
#define UNLOCK ReleaseMutex(mutex);
HANDLE mutex;

struct kmem_cache_s {
	struct kmem_cache_s* nextCache;
	int flagShrinkHistory;
	char name[1024];
	int error;
	int sizeOfObjectInBytes;
	int sizeOfCahceInBlocks;//bez meta pod
	int numOfSlabs;
	int numOfSlotsInSlab;
	int sizeOfSlabInBlocks;
	int currOffset;
	struct Slab* firstFreeSlab;
	struct Slab* firstPartiallyFullSlab;
	struct Slab* firstFullSlab;
	void (*constructor)(void*);
	void (*destructor)(void*);
};

struct CacheOfCaches {
	struct kmem_cache_s* firstCache;
	int numOfCaches;
};

struct Slab {
	struct Slab* nextSlab;
	struct Slot* firstFreeSlot;
	int numOfFreeSlots;
	int offset;
};

struct Slot {
	struct Slot* nextSlot;
	void* startOfSpaceForObj;//sluzi nam za obj
};

void kmem_init(void* space, int block_num) {
	if (space == 0 || block_num <= 0)
		return;
	mutex = CreateMutex(NULL, FALSE, NULL);
	LOCK
	buddy_init(space, block_num);
	struct CacheOfCaches* posOfCahcheOfCaches = (struct CacheOfCaches*)endOfMetaBuddy();//kes keseva se smesta odmah ispod meta pod za buddy u tom prvom bloku
	posOfCahcheOfCaches->firstCache = 0;
	posOfCahcheOfCaches->numOfCaches = 0;
	UNLOCK
}

kmem_cache_t* kmem_cache_create(const char* name, size_t size, void (*ctor)(void*), void (*dtor)(void*)) {
	if (name == 0 || size <= 0)
		return;
	LOCK
	struct CacheOfCaches* posOfCacheOfCaches = (struct CacheOfCaches*)(endOfMetaBuddy());
	kmem_cache_t* newCache = 0;
	kmem_cache_t* curr = posOfCacheOfCaches->firstCache;

	if (curr == 0) {//nema nijedan kes
		newCache =(kmem_cache_t*)(posOfCacheOfCaches + 1);
		posOfCacheOfCaches->firstCache = newCache;
	}
	else {
		void* startOfCurrBlock = (void*)(posOfMetaBuddy);
		void* endOfCurrBlock = (void*)((char*)posOfMetaBuddy + BLOCK_SIZE);//kraj prvog bloka
		void* endOfCurr = (void*)(curr + 1);//kraj tek kesa
		while (curr->nextCache){
			if (!(endOfCurr > startOfCurrBlock && endOfCurr <= endOfCurrBlock)) {//znaci da se preslo u narednu plocu
				endOfCurrBlock = (void*)((char*)curr + BLOCK_SIZE);//curr je smesten od pocetka tog bloka
				startOfCurrBlock = (void*)(curr);
			}
			curr = curr->nextCache;
			endOfCurr = (void*)(curr + 1);
		}
		if (!(endOfCurr > startOfCurrBlock && endOfCurr <= endOfCurrBlock)) {//znaci da se preslo u narednu plocu
			endOfCurrBlock = (void*)((char*)curr + BLOCK_SIZE);//curr je smesten od pocetka tog bloka
			startOfCurrBlock = (void*)(curr);
		}//moramo jos jednom da proverimo za curr, treba nam pok na zadnji kes to je curr da bismo uvezli novi

		if ((void*)((char*)endOfCurr + sizeof(kmem_cache_t)) > endOfCurrBlock) {//novi blok
			void* newBlock = buddy_alloc(1);
			if (newBlock == 0) {
				UNLOCK
				return 0;
			}
			newCache = (kmem_cache_t*)newBlock;
		}
		else {
			newCache = (kmem_cache_t*)endOfCurr;
		}
		curr->nextCache = newCache;
	}
	posOfCacheOfCaches->numOfCaches++;
	newCache->constructor = ctor;
	newCache->destructor = dtor;
	newCache->error = 0;
	newCache->firstFreeSlab = 0;
	newCache->firstFullSlab = 0;
	newCache->firstPartiallyFullSlab = 0;
	newCache->flagShrinkHistory = -1;
	strcpy(newCache->name, name);
	newCache->nextCache = 0;
	newCache->numOfSlabs = 0;
	newCache->sizeOfObjectInBytes = size;
	newCache->sizeOfCahceInBlocks = 0;
	newCache->currOffset = 0;//pocetni offset
	newCache->numOfSlotsInSlab = -1;//na pocetku nevalidna vrednost
	newCache->sizeOfSlabInBlocks = 0;
	UNLOCK
	return newCache;
}

int kmem_cache_shrink(kmem_cache_t* cachep){//moze da se desi da ostane kes bez ijedne ploce ali necu da brisem tad kes,
	//pa ce pri aloc novog obj ponovo da se racunaju sizeOfSlabInBlocks i numOfSlotsInSlab al nmvz isto ce da se izracuna
	if (cachep == 0)
		return 0;
	LOCK
	if (cachep->flagShrinkHistory == 1) {
		cachep->flagShrinkHistory = 0;//sledeci poziv shrinka ce dozvoliti
		UNLOCK
		return 0;
	}
	cachep->flagShrinkHistory = 0;

	int isObj = 1;
	char name[64];
	strncpy(name, cachep->name, 5);
	name[5] = '\0';
	if (!strcmp(name, "size-")) {
		isObj = 0;
	}
	struct Slab* old = 0;
	struct Slab* curr = cachep->firstFreeSlab;
	int ret = 0;
	while (curr) {
		old = curr;
		curr = curr->nextSlab;
		cachep->firstFreeSlab = curr;
		old->nextSlab = 0;
		if (isObj) {
			struct Slot* slot = old->firstFreeSlot;
			while (slot) {
				if (cachep->destructor)
					cachep->destructor(slot->startOfSpaceForObj);
				slot = slot->nextSlot;
			}
		}
		buddy_free(old, cachep->sizeOfSlabInBlocks);
		ret = ret + cachep->sizeOfSlabInBlocks;
		cachep->numOfSlabs--;
		cachep->sizeOfCahceInBlocks = cachep->sizeOfCahceInBlocks - cachep->sizeOfSlabInBlocks;
	}
	cachep->firstFreeSlab = 0;
	UNLOCK
	return ret;
}

void* kmem_cache_alloc(kmem_cache_t* cachep){
	if (cachep == 0) return 0;
	LOCK
	struct Slab* slab = 0;
	struct Slot* freeSlot = 0;
	
	if (cachep->firstPartiallyFullSlab) {
		slab = cachep->firstPartiallyFullSlab;
		freeSlot = slab->firstFreeSlot;
		slab->firstFreeSlot = slab->firstFreeSlot->nextSlot;
		freeSlot->nextSlot = 0;
		slab->numOfFreeSlots--;
		if (slab->numOfFreeSlots == 0) {
			cachep->firstPartiallyFullSlab = cachep->firstPartiallyFullSlab->nextSlab;
			slab->nextSlab = cachep->firstFullSlab;
			cachep->firstFullSlab = slab;
		}
	}
	else if (cachep->firstFreeSlab) {
		slab = cachep->firstFreeSlab;
		freeSlot = slab->firstFreeSlot;
		slab->firstFreeSlot = slab->firstFreeSlot->nextSlot;
		slab->numOfFreeSlots--;
		freeSlot->nextSlot = 0;
		cachep->firstFreeSlab = cachep->firstFreeSlab->nextSlab;
		slab->nextSlab = cachep->firstPartiallyFullSlab;
		cachep->firstPartiallyFullSlab = slab;

	}
	else {
		int isDataValid = 0;
		if (cachep->firstFullSlab) {
			isDataValid = 1;
		}
		slab = create_and_init_new_Slab(cachep->sizeOfObjectInBytes, isDataValid, 1, cachep);
		if (slab == 0) {
			cachep->error |= 1;
			UNLOCK
			return 0;
		}
		freeSlot = slab->firstFreeSlot;
		slab->firstFreeSlot = slab->firstFreeSlot->nextSlot;
		slab->numOfFreeSlots--;
		freeSlot->nextSlot = 0;

		slab->nextSlab = cachep->firstPartiallyFullSlab;
		cachep->firstPartiallyFullSlab = slab;
		if (cachep->flagShrinkHistory != -1)
			cachep->flagShrinkHistory = 1;
		cachep->numOfSlabs++;
		cachep->sizeOfCahceInBlocks = cachep->sizeOfCahceInBlocks + cachep->sizeOfSlabInBlocks;
	}
	
	UNLOCK
	return freeSlot->startOfSpaceForObj;
}

void kmem_cache_free(kmem_cache_t* cachep, void* objp){
	if (cachep == 0 || objp == 0) return;
	LOCK
	int ret = findObj(1, cachep, objp);
	if (ret == -1)
		cachep->error |= 2;//pokusaj dealociranja necega sto nije ni alocirano za taj kes
	else if (ret == -2)
		cachep->error |= 4;//pokusaj dealociranja objekta koji nije na dobroj poziciji
	UNLOCK
}

void* kmalloc(size_t size) {
	if (size <= 0)
		return 0;
	LOCK
	struct CacheOfCaches* posOfCacheOfCaches = (struct CacheOfCaches*)(endOfMetaBuddy());
	char name[1024];
	int powI = 0;
	int newSize = pow(2, powI);
	while (newSize < size) {//svodjenje size na stepen dvojke
		powI++;
		newSize = pow(2, powI);
	}
	size = newSize;
	sprintf(name, "size-%d", size);
	kmem_cache_t* curr = posOfCacheOfCaches->firstCache;
	while (curr) {
		if (!strcmp(curr->name, name)) {
			break;
		 }
		curr = curr->nextCache;
	}
	if (curr == 0) {//nema kes za taj mem baf
		kmem_cache_t* newCache = kmem_cache_create(name, size, 0, 0);
		if (newCache == 0) {
			UNLOCK
			return 0;
		}
		curr = newCache;
	}

	struct Slab* slab = 0;
	struct Slot* freeSlot = 0;
	if (curr->firstPartiallyFullSlab) {
		slab = curr->firstPartiallyFullSlab;

		freeSlot = slab->firstFreeSlot;
		slab->firstFreeSlot = slab->firstFreeSlot->nextSlot;
		freeSlot->nextSlot = 0;
		slab->numOfFreeSlots--;
		if (slab->numOfFreeSlots == 0) {//prebacujemo u listu punih ploca
			curr->firstPartiallyFullSlab = curr->firstPartiallyFullSlab->nextSlab;
			slab->nextSlab = 0;
			slab->nextSlab = curr->firstFullSlab;
			curr->firstFullSlab = slab;
		}
		UNLOCK
		return freeSlot;
	}
	else if (curr->firstFreeSlab) {
		slab = curr->firstFreeSlab;

		freeSlot = slab->firstFreeSlot;
		slab->firstFreeSlot = slab->firstFreeSlot->nextSlot;
		freeSlot->nextSlot = 0;
		slab->numOfFreeSlots--;
		curr->firstFreeSlab = curr->firstFreeSlab->nextSlab;
		slab->nextSlab = 0;
		slab->nextSlab = curr->firstPartiallyFullSlab;
		curr->firstPartiallyFullSlab = slab;
		UNLOCK
		return freeSlot;
	}
	else {
		int isDataValid = 0;
		if (curr->firstFullSlab)
			isDataValid = 1;
		slab = create_and_init_new_Slab(size, isDataValid, 0, curr);
		if (slab == 0) {
			curr->error |= 1;//kod greske kad nema mesta za slab
			UNLOCK
			return 0;
		}
		curr->sizeOfCahceInBlocks = curr->sizeOfCahceInBlocks + curr->sizeOfSlabInBlocks;
	
		if (curr->flagShrinkHistory != -1)
			curr->flagShrinkHistory = 1;//imalo potrebe za povecanje
		freeSlot = slab->firstFreeSlot;
		slab->firstFreeSlot = slab->firstFreeSlot->nextSlot;
		freeSlot->nextSlot = 0;
		slab->numOfFreeSlots--;
		slab->nextSlab = 0;
		slab->nextSlab = curr->firstPartiallyFullSlab;
		curr->firstPartiallyFullSlab = slab;
		curr->numOfSlabs++;
		UNLOCK
		return freeSlot;
	}
}

int findObj(int isObj, kmem_cache_t* cache, void* objp) {//fja za trazenje objp u kesu i njegovo freeovanje
	//0 ako nadje i obrise
	//-1 ako ga nema u ovom kesu
	//-2 ako nije na dobroj poz u slabu
	struct Slab* prevSlab = 0;
	struct Slab* currSlab = cache->firstFullSlab;
	while (currSlab) {
		void* start = (void*)((char*)currSlab + sizeof(struct Slab) + currSlab->offset);
		void* end = (void*)((char*)currSlab + cache->sizeOfSlabInBlocks * BLOCK_SIZE);
		if (objp >= start && objp < end) {//u ovom je slabu
			//uvesti nazad u slab kao slobodan
			//pomeriti se iznad za smestajenje struct slot
			//poziv destruktora
			void* startObj = objp;
			if (isObj)
				startObj = (void*)((char*)objp - sizeof(struct Slot));
			int dif = (char*)startObj - (char*)start;
			int realSize = (isObj == 1) ? cache->sizeOfObjectInBytes + sizeof(struct Slot) : cache->sizeOfObjectInBytes;
			if ((dif % realSize) != 0)
				return -2;

			struct Slot* newSlot = 0; 
			if (isObj) {
				newSlot= (struct Slot*)((char*)objp - sizeof(struct Slot));
			}
			else {
				newSlot = (struct Slot*)(objp);
			}
			newSlot->nextSlot = 0;
			if (isObj) {
				newSlot->startOfSpaceForObj = objp;
			}
			else {
				newSlot->startOfSpaceForObj = 0;
			}

			if (isObj) {
				if (cache->destructor)
					cache->destructor(objp);
				if (cache->constructor)
					cache->constructor(objp);
			}
		
			newSlot->nextSlot = currSlab->firstFreeSlot;
			currSlab->firstFreeSlot = newSlot;
			currSlab->numOfFreeSlots++;;
			//premestam slab u drugu listu, nije vise skroz puna
			if (prevSlab) {
				prevSlab->nextSlab = currSlab->nextSlab;
				currSlab->nextSlab = 0;
			}
			else {
				cache->firstFullSlab = currSlab->nextSlab;
				
				currSlab->nextSlab = 0;
			}
			currSlab->nextSlab = cache->firstPartiallyFullSlab;
			cache->firstPartiallyFullSlab = currSlab;
			return 0;
		}
		prevSlab = currSlab;
		currSlab = currSlab->nextSlab;
	}

	prevSlab = 0;
	currSlab = cache->firstPartiallyFullSlab;
	while (currSlab) {
		void* start = (void*)((char*)currSlab + sizeof(struct Slab) + currSlab->offset);
		void* end = (void*)((char*)currSlab + cache->sizeOfSlabInBlocks * BLOCK_SIZE);
		if (objp >= start && objp < end) {//u ovom je slabu
			//uvesti nazad u slab kao slobodan
			//pomeriti se iznad za smestajenje struct Slot
			//poziv destruktora
			void *startObj = objp;
			if (isObj)
				startObj = (void*)((char*)objp - sizeof(struct Slot));
			int dif = (char*)startObj - (char*)start;
			int realSize = (isObj == 1) ? cache->sizeOfObjectInBytes + sizeof(struct Slot) : cache->sizeOfObjectInBytes;
			if ((dif % realSize) != 0)
				return -2;

			struct Slot* newSlot = 0;
			if (isObj) {
				newSlot = (struct Slot*)((char*)objp - sizeof(struct Slot));
			}
			else {
				newSlot = (struct Slot*)(objp);
			}
			newSlot->nextSlot = 0;
			if (isObj) {
				newSlot->startOfSpaceForObj = objp;
			}
			else {
				newSlot->startOfSpaceForObj = 0;
			}

			if (isObj) {
				if (cache->destructor)
					cache->destructor(objp);
				if (cache->constructor)
					cache->constructor(objp);
			}
			newSlot->nextSlot = currSlab->firstFreeSlot;
			currSlab->firstFreeSlot = newSlot;
			currSlab->numOfFreeSlots++;

			if (currSlab->numOfFreeSlots == cache->numOfSlotsInSlab) {//postala je prazna, premesti u red praznih
				if (prevSlab) {
					prevSlab->nextSlab = currSlab->nextSlab;
					currSlab->nextSlab = 0;
				}
				else {
					cache->firstPartiallyFullSlab = currSlab->nextSlab;
					currSlab->nextSlab = 0;
				}
				currSlab->nextSlab = cache->firstFreeSlab;
				cache->firstFreeSlab = currSlab;
			}

			return 0;
		}
		prevSlab = currSlab;
		currSlab = currSlab->nextSlab;
	}
	return -1;
}

void kfree(const void* objp) {
	if (objp == 0)
		return;
	LOCK
	struct CacheOfCaches* posOfCacheOfCaches = (struct CacheOfCaches*)(endOfMetaBuddy());

	kmem_cache_t* currCache = posOfCacheOfCaches->firstCache;
	while (currCache) {
		char name[64];
		strncpy(name, currCache->name, 5);
		name[5] = '\0';
		if (!strcmp(name,"size-")) {
			int ret = findObj(0, currCache, objp);
			if (ret == 0) {
				kmem_cache_shrink(currCache);
				UNLOCK
				return;
			}
				
		}

		currCache = currCache->nextCache;
	}
	UNLOCK
}

void kmem_cache_destroy(kmem_cache_t* cachep){
	if (cachep == 0)
		return;
	LOCK
	struct CacheOfCaches* posOfCacheOfCaches = (struct CacheOfCaches*)(endOfMetaBuddy());
	kmem_cache_t* prevCache = 0;
	kmem_cache_t* currCache = posOfCacheOfCaches->firstCache;
	while (currCache != cachep) {
		prevCache = currCache;
		currCache = currCache->nextCache;
	}
	if (currCache == 0) {
		UNLOCK
		return;//ne postoji ovaj kes
	}
	if (cachep->numOfSlabs > 0 && (cachep->firstFullSlab || cachep->firstPartiallyFullSlab)) {
		cachep->error |= 8;//ne moze se obrisati kes nije prazan
		UNLOCK
		return;
	}
	
	if (cachep->firstFreeSlab) {//numOfSlabs je veci od nule sve su prazne ploce
		int isObj = 1;
		char name[64];
		strncpy(name, cachep->name, 5);
		name[5] = '\0';
		if (!strcmp(name, "size-")) {
			isObj = 0;
		}

		struct Slab* old = 0;
		struct Slab* curr = cachep->firstFreeSlab;
		while (curr) {
			old = curr;
			curr = curr->nextSlab;
			cachep->firstFreeSlab = curr;
			old->nextSlab = 0;
			if (isObj) {
				struct Slot* slot = old->firstFreeSlot;
				while (slot) {
					if (cachep->destructor)
						cachep->destructor(slot->startOfSpaceForObj);
					slot = slot->nextSlot;
				}
			}
			buddy_free(old, cachep->sizeOfSlabInBlocks);
			cachep->numOfSlabs--;
			cachep->sizeOfCahceInBlocks = cachep->sizeOfCahceInBlocks - cachep->sizeOfSlabInBlocks;
		}
		cachep->firstFreeSlab = 0;
	}
	
	if (prevCache) {
		prevCache->nextCache = cachep->nextCache;
	}
	else {
		posOfCacheOfCaches->firstCache = cachep->nextCache;
	}
	cachep->nextCache = 0;
	cachep->constructor = 0;
	cachep->destructor = 0;
	cachep->firstFreeSlab = 0;
	cachep->firstFullSlab = 0;
	cachep->firstPartiallyFullSlab = 0;
	cachep->flagShrinkHistory = 0;
	for (int i = 0; i < 1024; i++) {
		cachep->name[i] = 0;
	}
	cachep->numOfSlabs = 0;
	cachep->numOfSlotsInSlab = 0;
	cachep->currOffset = 0;
	cachep->sizeOfCahceInBlocks = 0;
	cachep->sizeOfObjectInBytes = 0;
	cachep->sizeOfSlabInBlocks = 0;
	cachep->error = 0;
	UNLOCK
}

struct Slab* create_and_init_new_Slab(size_t size, int isDataValid, int isObj, kmem_cache_t *cache) {//za male mem baf i obj se razlikuje velicina prostora za slot
	//tj podela ploce (bez meta pod za plocu) na slotove, kod malih mem baf slot je vel malog mem baf i na poc tog prostora se smesta meta pod za slot
	//a za obj treba slot da bude vel meta pod za slot+vel obj
	int realSize = size;
	if (isObj == 1) {
		realSize = size + sizeof(struct Slot);
	}

	int numOfObjInSlab = 0;
	int numOfBlocks = 0;
	if (isDataValid == 1) {
		numOfObjInSlab = cache->numOfSlotsInSlab;
		numOfBlocks = cache->sizeOfSlabInBlocks;
	}
	else {//uvek min dva slot mora
		//nadji, i namesti cache sve info
		int bestN = -1;
		int bestNumOfSlots = 0;
		int bestFrag = INT_MAX;
		int minI = 0;
		int tmp = (BLOCK_SIZE * pow(2, minI) - sizeof(struct Slab)) / realSize;
		while (tmp == 0) {
			minI++;
			tmp = (BLOCK_SIZE * pow(2, minI) - sizeof(struct Slab)) / realSize;
		}
		for (int i = minI; i < minI + 3; i++) {
			int num = (BLOCK_SIZE * pow(2, i) - sizeof(struct Slab)) / realSize;
			int frag = BLOCK_SIZE * pow(2, i) - sizeof(struct Slab) - realSize * num;
			
			if (num > 1 && frag < bestFrag) {
				bestFrag = frag;
				bestN = i;
				bestNumOfSlots = num;
			}
		}
		numOfBlocks = pow(2, bestN);
		numOfObjInSlab = bestNumOfSlots;
	}
	
	struct Slab* newSlab = (struct Slab*)(buddy_alloc(numOfBlocks));
	if (newSlab == 0)
		return 0;
	if (isDataValid == 0) {
		cache->numOfSlotsInSlab = numOfObjInSlab; 
		cache->sizeOfSlabInBlocks = numOfBlocks;
	}
	void* endOfSlab = (void*)((char*)newSlab + numOfBlocks * BLOCK_SIZE);
	newSlab->nextSlab = 0;
	newSlab->numOfFreeSlots = numOfObjInSlab;
	newSlab->offset = cache->currOffset;

	struct Slot* prev = 0;
	struct Slot* currSlot = (struct Slot*)((char*)newSlab + sizeof(struct Slab) + cache->currOffset);
	newSlab->firstFreeSlot = currSlot;
	currSlot->nextSlot = 0;
	currSlot->startOfSpaceForObj = 0;
	if (isObj == 1) {
		currSlot->startOfSpaceForObj = (void*)(currSlot + 1);
		if (cache->constructor != 0) {
			cache->constructor(currSlot->startOfSpaceForObj);
		}
	}
	void* endOfCurrSlot = (void*)((char*)currSlot + realSize);
	while (((void*)((char*)endOfCurrSlot + realSize)) <= endOfSlab) {
		prev = currSlot;
		currSlot = (struct Slot*)((char*)currSlot + realSize);
		endOfCurrSlot = (void*)((char*)currSlot + realSize);
		prev->nextSlot = currSlot;
		currSlot->nextSlot = 0;
		currSlot->startOfSpaceForObj = 0;
		if (isObj == 1) {
			currSlot->startOfSpaceForObj = (void*)(currSlot + 1);
			if (cache->constructor != 0) {
				cache->constructor(currSlot->startOfSpaceForObj);
			}
		}
	}
	//postavi novi offset u kesu
	int frag = cache->sizeOfSlabInBlocks * BLOCK_SIZE - sizeof(struct Slab) - cache->numOfSlotsInSlab * realSize;
	int numOffsets = frag / CACHE_L1_LINE_SIZE;
	int lastOffset = CACHE_L1_LINE_SIZE * numOffsets;
	cache->currOffset = cache->currOffset + CACHE_L1_LINE_SIZE;
	if (cache->currOffset > lastOffset)
		cache->currOffset = 0;
	return newSlab;
}

int kmem_cache_error(kmem_cache_t* cachep) {
	LOCK
	if (cachep->error == 0) {
		printf("Nema gresaka\n");
		UNLOCK
		return 0;
	}
	if (cachep->error & 1) {
		printf("Nema vise slobodnog memorijskog prostora za alociranje novog slab-a\n");
	}
	if (cachep->error & 2) {
		printf("Bio je pokusaj dealociranja onoga sto nije bilo alocirano\n");
	}
	if (cachep->error & 4) {
		printf("Bio je pokusaj dealociranja objekta koji nije na dobroj poziciji\n");
	}
	if (cachep->error & 8) {
		printf("Bio je pokusaj brisanja kesa koji nije prazan\n");
	}
	UNLOCK
	return -1;
}

void kmem_cache_info(kmem_cache_t* cachep) {
	LOCK
	printf("Info za kes %s:[", cachep->name);
	printf("%d, ", cachep->sizeOfObjectInBytes);
	printf("%d, ", cachep->sizeOfCahceInBlocks);
	printf("%d, ", cachep->numOfSlabs);
	printf("%d, ", cachep->numOfSlotsInSlab);
	int numUsed = 0;
	int sum = cachep->numOfSlabs * cachep->numOfSlotsInSlab;
	for (struct Slab* curr = cachep->firstFullSlab; curr != 0; curr = curr->nextSlab)
		numUsed += cachep->numOfSlotsInSlab;
	for (struct Slab* curr = cachep->firstPartiallyFullSlab; curr != 0; curr = curr->nextSlab)
		numUsed += (cachep->numOfSlotsInSlab - curr->numOfFreeSlots);
	if (sum != 0)
		printf("%f%%]\n", ((double)numUsed / sum) * 100);
	else
		printf("PRAZNO]\n");
	UNLOCK
}