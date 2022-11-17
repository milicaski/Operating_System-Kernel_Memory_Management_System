#include "buddy.h"
#include "slab.h"
#include <math.h>

void buddy_init(void* space, int block_num) {
	int sizeOfBuddyAllocatorInBlocks = block_num;
	posOfMetaBuddy = (struct MetaBuddy*)space;//zapamtili pok na mesto u mem gde su smesteni meta podaci
	init_metaBuddy();

	int numOfBlocksForMetaBuddy = ceil((double)sizeof(struct MetaBuddy) / BLOCK_SIZE);
	sizeOfBuddyAllocatorInBlocks -= numOfBlocksForMetaBuddy;
	struct Buddy* next = (struct Buddy*)((char*)space + BLOCK_SIZE * numOfBlocksForMetaBuddy);
	posOfMetaBuddy->startOfBuddySpace = next;
	next->nextBuddy = 0;

	int sizeLeftToScheduleInBlocks = sizeOfBuddyAllocatorInBlocks;
	int isSetIndex = 0;
	int degree = 0;
	while (sizeLeftToScheduleInBlocks) {
		int found = 0;
		int num = 0;
		while (!found) {
			num = pow(2, degree);
			if (num < sizeLeftToScheduleInBlocks) {
				degree++;
			}
			else if (num == sizeLeftToScheduleInBlocks) {
				found = 1;
			}
			else {
				degree--;
				num = pow(2, degree);
				if (!(num > sizeLeftToScheduleInBlocks))
					found = 1;
			}
		}//num je taj stepen dvojke koji ide u listu
		num = pow(2, degree);
		posOfMetaBuddy->arrayBuddys[degree] = next;
		next->nextBuddy = 0;
		next->sizeInBlocks = num;
		next = (struct Buddy*)((char*)next + BLOCK_SIZE * num);
		sizeLeftToScheduleInBlocks -= num;
		posOfMetaBuddy->sizeOfBuddySpaceInBlocks += num;
		if (!isSetIndex) {
			posOfMetaBuddy->biggestIndex = degree;
			isSetIndex = 1;
		}
		degree--;
	}

}
int find_Degree_forNumOfBlocks(int numOfBlocks) {
	int found = 0;
	int degree = 0;
	while (!found) {
		int num = pow(2, degree);
		if (num < numOfBlocks) {
			degree++;
		}
		else if (num == numOfBlocks) {
			found = 1;
		}
	}//stepen trazenog broja blokova
	return degree;
}

void* buddy_alloc(int numOfBlocks) {
	if (numOfBlocks <= 0)
		return 0;
	
	int degree = find_Degree_forNumOfBlocks(numOfBlocks);
	
	for (int i = degree; i <= posOfMetaBuddy->biggestIndex; i++) {
		if (posOfMetaBuddy->arrayBuddys[i] != 0) {
			if (i == degree) {//ima bas mem te velicine
				struct Buddy* retBuddy = posOfMetaBuddy->arrayBuddys[i];
				posOfMetaBuddy->arrayBuddys[i] = posOfMetaBuddy->arrayBuddys[i]->nextBuddy;
				retBuddy->nextBuddy = 0;
				if (i == posOfMetaBuddy->biggestIndex) {
					int j = 29;
					while (j >= 0 && posOfMetaBuddy->arrayBuddys[j] == 0) {
						j--;
					}
					if (j < 0)
						posOfMetaBuddy->biggestIndex = -1;
					else
						posOfMetaBuddy->biggestIndex = j;
				}
					//posOfMetaBuddy->biggestIndex = -1;
				return retBuddy;
			}
			else {
				
				int j = i;
				struct Buddy* buddyToCut = posOfMetaBuddy->arrayBuddys[i];
				posOfMetaBuddy->arrayBuddys[i] = posOfMetaBuddy->arrayBuddys[i]->nextBuddy;
				buddyToCut->nextBuddy = 0;
				while (numOfBlocks < buddyToCut->sizeInBlocks) {
					int newNumOfBlocks = pow(2, j) / 2;
					struct Buddy* newBuddy = (struct Buddy*)((char*)buddyToCut + BLOCK_SIZE * newNumOfBlocks);//napravila novi buddy
					
					newBuddy->sizeInBlocks = newNumOfBlocks;
					newBuddy->nextBuddy = posOfMetaBuddy->arrayBuddys[j - 1];
					posOfMetaBuddy->arrayBuddys[j - 1] = newBuddy;

					buddyToCut->sizeInBlocks = newNumOfBlocks;
					j--;
				}
				if (i == posOfMetaBuddy->biggestIndex) {
					posOfMetaBuddy->biggestIndex--;
				}
				return buddyToCut;
			}
		}
	}
	return 0;
}

void buddy_free(void* p, int numOfBlocks){
	if (p == 0 || numOfBlocks <= 0 || numOfBlocks > posOfMetaBuddy->sizeOfBuddySpaceInBlocks) return;

	struct Buddy* myAddress = (struct Buddy*)p;
	myAddress->nextBuddy = 0;
	myAddress->sizeInBlocks = numOfBlocks;
	int currMyNumOfBlocks = numOfBlocks;
	while (1) {
		int degree = find_Degree_forNumOfBlocks(currMyNumOfBlocks);
		int myIndex = ((char*)myAddress - (char*)posOfMetaBuddy->startOfBuddySpace) / BLOCK_SIZE;
		int check = myIndex / currMyNumOfBlocks;
		struct Buddy* myBuddysAddress = 0;
		if (check % 2) {
			myBuddysAddress = (struct Buddy*)((char*)myAddress - currMyNumOfBlocks * BLOCK_SIZE);
		}
		else {
			myBuddysAddress = (struct Buddy*)((char*)myAddress + currMyNumOfBlocks * BLOCK_SIZE);
		}
		struct Buddy* curr = posOfMetaBuddy->arrayBuddys[degree];
		struct Buddy* prev = 0;
		while (curr) {
			if (curr == myBuddysAddress) {
				break;
			}
			else {
				prev = curr;
				curr = curr->nextBuddy;
			}
		}
		if (curr) {
			if (prev) {
				prev->nextBuddy = curr->nextBuddy;
			}
			else {
				posOfMetaBuddy->arrayBuddys[degree] = curr->nextBuddy;
			}
			curr->nextBuddy = 0;
			if (check % 2) {
				myBuddysAddress->sizeInBlocks = myBuddysAddress->sizeInBlocks + myAddress->sizeInBlocks;
				myAddress = myBuddysAddress;
			}
			else {
				myAddress->sizeInBlocks = myAddress->sizeInBlocks + myBuddysAddress->sizeInBlocks;
			}
			currMyNumOfBlocks = myAddress->sizeInBlocks;

		}
		else {//nema vise spajanja
			myAddress->sizeInBlocks = currMyNumOfBlocks;
			myAddress->nextBuddy = posOfMetaBuddy->arrayBuddys[degree];
			posOfMetaBuddy->arrayBuddys[degree] = myAddress;
			if (degree > posOfMetaBuddy->biggestIndex)
				posOfMetaBuddy->biggestIndex = degree;
			break;
		}
	}
}

void init_metaBuddy() {
	posOfMetaBuddy->sizeOfBuddySpaceInBlocks = 0;
	for (int i = 0; i < 30; i++) {
		posOfMetaBuddy->arrayBuddys[i] = 0;
	}
}
void* startOfSpace() {
	return posOfMetaBuddy;
}

void* endOfMetaBuddy() {
	void* end = posOfMetaBuddy + 1;
	return end;
}