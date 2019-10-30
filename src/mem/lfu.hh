#ifndef __LFU_HH__
#define __LFU_HH__

#include <deque>
#include <vector>
#include <map>
#include <iostream>
#include <utility>

#include "mem/mem_addr_type.hh"
#include "base/statistics.hh"


class LFU
{
	public:

	LFU(int cacheMaxSize)
	: maxSize(cacheMaxSize), size(0), head(-1), tail(-1)
	{
		lfuNodes = new node[maxSize];
		for (int i = 0 ; i < maxSize ; i++) {
		lfuNodes[i].hostAddr = std::numeric_limits<Addr>::max();
		lfuNodes[i].DValue = 0;
		lfuNodes[i].next = -1;
		lfuNodes[i].pre = -1;
		}
	}

	struct node {
		Addr hostAddr;
		int DValue;
		int next;
		int pre;
	};

	void order(int idx) {
		int cur = tail;

		if (head == -1 && tail == -1) {
			head = tail = idx;
			checkLFUErr();
			return;
		}

		while (cur != -1) {
			if (lfuNodes[cur].DValue > lfuNodes[idx].DValue) {
				lfuNodes[idx].pre = cur;
				lfuNodes[idx].next = lfuNodes[cur].next;
				if (cur == tail) {
					tail = idx;
				} else {
					lfuNodes[lfuNodes[cur].next].pre = idx;
				}
				lfuNodes[cur].next = idx;
				checkLFUErr();
				return;

			} else {
				cur = lfuNodes[cur].pre;
			}
		}

		if (cur == -1) {
			lfuNodes[head].pre = idx;
			lfuNodes[idx].next = head;
			lfuNodes[idx].pre = -1;
			head = idx;
		}
		checkLFUErr();
	}

	void increment(int i, int Dvalue)
	{
		lfuNodes[i].DValue += Dvalue;
		assert(Dvalue > 0);
		if (size == 1 || i == head) {
			checkLFUErr();
			return;
		} else if (i == tail) {
			int pre = lfuNodes[i].pre;
			lfuNodes[pre].next = -1;
			tail = pre;
			lfuNodes[i].pre = lfuNodes[i].next = -1;
			order(i);
		} else {
			int pre = lfuNodes[i].pre;
			int next = lfuNodes[i].next;
			lfuNodes[pre].next = next;
			lfuNodes[next].pre = pre;
			lfuNodes[i].pre = lfuNodes[i].next = -1;
			order(i);
		}

	}

	struct PageAddr insert(Addr addr, int Dvalue)
	{
		struct PageAddr evictPage = {std::numeric_limits<Addr>::max()};
		if (size == maxSize) {
			evictPage.val = lfuNodes[tail].hostAddr;
			// std::cout << lfuNodes[tail].hostAddr <<"/"<< lfuNodes[tail].DValue << " removed.\n";

			int tmpIdx = tail;
			int pre = lfuNodes[tmpIdx].pre;
			lfuNodes[pre].next = -1;
			lfuNodes[tmpIdx].pre = -1;
			lfuNodes[tmpIdx].next = -1;
			tail = pre;

			int n = mapIndex.erase(lfuNodes[tmpIdx].hostAddr);
			if (n != 1) {
				printf("LFU error!1\n");
				exit(-1);
			}
			lfuNodes[tmpIdx].hostAddr = addr;
			lfuNodes[tmpIdx].DValue = Dvalue;
			mapIndex.insert(std::make_pair(addr, tmpIdx));
			order(tmpIdx);
			return evictPage;

		}
		++size;
		for (int i = 0; i < maxSize; i++) {
		if(lfuNodes[i].hostAddr == std::numeric_limits<Addr>::max()) {
			lfuNodes[i].hostAddr = addr;
			lfuNodes[i].DValue = Dvalue;
			lfuNodes[i].pre = lfuNodes[i].next = -1;
			mapIndex.insert(std::make_pair(addr, i));
			order(i);
			break;
		}
		}

		// std::cout << "cache block " << addr << " inserted.\n";
		return evictPage;
	}

	virtual struct PageAddr refer(Addr addr, int Dvalue)
	{
		struct PageAddr evictPage = {std::numeric_limits<Addr>::max()};
		if (mapIndex.find(addr) == mapIndex.end())
			evictPage = insert(addr, Dvalue);
		else
			increment(mapIndex[addr], Dvalue);

		return evictPage;
	}

	virtual void reset()
	{
		head = -1;
		tail = -1;
		size = 0;
		mapIndex.clear();
		for (int i = 0; i < maxSize; ++i) {
			lfuNodes[i].hostAddr = std::numeric_limits<Addr>::max();
			lfuNodes[i].DValue = 0;
			lfuNodes[i].next = -1;
			lfuNodes[i].pre = -1;
		}
	}
	void checkLFUErr()
	{
		std::unordered_set<Addr> lfuList;
		for (int i = 0, cur = head; i < size; ++i, cur = lfuNodes[cur].next) {
			lfuList.insert(lfuNodes[cur].hostAddr);
		}
		assert(lfuList.size() == capacity());
	}
	bool isEmpty() {return size == 0;}
	int capacity() {return size;}
	bool findAddr(Addr addr)
	{
		bool ret = false;
		for (int i = 0, cur = head; i < size; ++i, cur = lfuNodes[cur].next) {
			if (lfuNodes[cur].hostAddr == addr) {
				ret = true;
				break;
			}
		}
		return ret;
	}

	void printLFU()
	{
		for (int i = 0, cur = head; i < size; ++i, cur = lfuNodes[cur].next) {
			std::cout<<lfuNodes[cur].DValue<<", ";
		}
		std::cout<<"\n";
	}

	public:
	std::map<Addr, int> mapIndex;
	const int maxSize;
	int size;
	int head;
	int tail;
	node *lfuNodes;

};

class LFUDA : public LFU
{
	public:
	LFUDA(int _cacheMaxSize, int _threshold, int _maxHits)
	: LFU(_cacheMaxSize), threshold(_threshold), maxHits(_maxHits)
	{

	}

	struct PageAddr refer(Addr addr, int Dvalue) override
	{
		struct PageAddr evictPage = {std::numeric_limits<Addr>::max()};
		if (mapIndex.find(addr) == mapIndex.end()) {
			++threshold;
			if (size == maxSize) {
				if (lfuNodes[tail].DValue > threshold) {
					return evictPage;
				} else {
					evictPage = insert(addr, threshold);
				}
			} else {
				evictPage = insert(addr, threshold);
			}
		} else {
			increment(mapIndex[addr], Dvalue);
			if (lfuNodes[mapIndex[addr]].DValue >= maxHits) {
				// std::cout<<"Reach Max Hits\n";
				for (auto & iter : mapIndex) {
					lfuNodes[iter.second].DValue =
						lfuNodes[iter.second].DValue - maxHits < 0 ?
						0 : lfuNodes[iter.second].DValue - maxHits;

				}

				threshold = 0;
			}
		}

		return evictPage;
	}

	virtual void reset() override
	{
		LFU::reset();
		threshold = 0;
	}

	private:
	int threshold;
	const int maxHits;
};


#endif