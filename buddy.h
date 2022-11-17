
void buddy_init(void* space, int block_num);
void* buddy_alloc(int numOfBlocks);
void buddy_free(void* p, int numOfBlocks);
void init_metaBuddy();
int find_Degree_forNumOfBlocks(int numOfBlocks);
void* endOfMetaBuddy();
void* startOfSpace();

struct MetaBuddy *posOfMetaBuddy;
struct Buddy {
	struct Buddy* nextBuddy;
	int sizeInBlocks;
};
struct MetaBuddy{
	struct Buddy* startOfBuddySpace;
	 int sizeOfBuddySpaceInBlocks;
	 int biggestIndex;
	struct Buddy* arrayBuddys[30];
};