
#ifndef __LRU_HH__
#define __LRU_HH__

#include <deque>
#include <vector>
#include <map>
#include <iostream>
#include <utility>

#include "mem/mem_addr_type.hh"
#include "base/statistics.hh"

class LRU
{
	public:

	LRU(int);

	std::map<Addr, int> map_index;
	int head;
	int tail;
	int size;
	int max_size;

	struct node {
		Addr hostAddr;
		int next;
		int pre;
	};
	node *member;
	bool *exist;

	virtual struct PageAddr put(Addr);
	virtual struct PageAddr firstPut(Addr);
	struct PageAddr findInLRU(int);
	// struct PageAddr notFindInLRU(int);

	void getAllHostPages(std::vector<Addr>&);

	void reset();

	bool isEmpty();

};



#endif