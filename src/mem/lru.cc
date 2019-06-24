

#include "mem/lru.hh"


LRU::LRU(int length)
{
    head = -1;
    tail = -1;
    size = 0;
    max_size = length;

    member = new node[length];
    for (int i = 0 ; i < length ; i++) {
        member[i].hostAddr = std::numeric_limits<Addr>::max();
        member[i].next = -1;
        member[i].pre = -1;
    }

    exist = new bool[length]();
    for (int i = 0 ; i < length ; i++) {
        exist[i] = false;
    }
}

void
LRU::reset()
{
    head = -1;
    tail = -1;
    size = 0;

    for (int i = 0 ; i < max_size ; i++) {
        exist[i] = false;
    }

    mapIndex.clear();
}


bool
LRU::isEmpty()
{
    bool rv = true;
    for (int i = 0 ; i < max_size ; i++) {
        if (exist[i]) {
           rv = false; return rv;
        }

    }

    return rv;
}

PageAddr
LRU::put(Addr hostAddr)
{
    struct PageAddr pageAddr;
    if (size == 0)
        return firstPut(hostAddr);

    auto iter = mapIndex.find(hostAddr);

    if (iter != mapIndex.end()) {
        //find
        int __index = iter->second;
        return findInLRU(__index);
    } else {
        //not find
        if (size == max_size) {
            Addr evict_Addr = member[tail].hostAddr;
            int pre = member[tail].pre;
            member[pre].next = -1;
            member[tail].pre = -1;
            int n = mapIndex.erase(evict_Addr);
            if (n != 1) {
                printf("LRU error!1\n");
                exit(-1);
            }

            member[tail].hostAddr = hostAddr;
            member[tail].next = head;
            member[head].pre = tail;
            mapIndex[hostAddr] = tail;

            head = tail;
            tail = pre;
            pageAddr.val = evict_Addr;
            return pageAddr;
        } else {
            int __index;
            for (__index = 0 ; __index < max_size ; __index++) {
                if (exist[__index] == false) {
                    exist[__index] = true;
                    break;
                }
            }
            member[__index].hostAddr = hostAddr;
            mapIndex[hostAddr] = __index;
            member[head].pre = __index;
            member[__index].next = head;
            member[__index].pre = -1;
            head = __index;
            size++;
            pageAddr.val = std::numeric_limits<Addr>::max();
            return pageAddr;
        }
    }
}

PageAddr
LRU::firstPut(Addr hostAddr)
{
    assert(size == 0);

    struct PageAddr pageAddr;
    member[size].hostAddr = hostAddr;
    member[size].pre = -1;
    member[size].next = -1;
    mapIndex[hostAddr] = size;
    head = size;
    tail = size;
    exist[size] = true;
    size++;
    pageAddr.val = std::numeric_limits<Addr>::max();
    return pageAddr;
}

PageAddr
LRU::findInLRU(int index)
{
    struct PageAddr pageAddr;
    if (index != head) {
        if (index != tail) {
            int next = member[index].next;
            int pre = member[index].pre;
            member[pre].next = next;
            member[next].pre = pre;
            member[index].pre = -1;
            member[index].next = head;
            member[head].pre = index;
            head = index;
        } else {
            int pre = member[index].pre;
            member[pre].next = -1;
            member[index].pre = -1;
            member[index].next = head;
            member[head].pre = index;
            head = index;
            tail = pre;
        }
    }

    pageAddr.val = std::numeric_limits<Addr>::max();
    return pageAddr;
}

void
LRU::erase(Addr hostAddr)
{
    if(size == 1)
	{
		reset();
		return;
	}

	auto iter = mapIndex.find(hostAddr);

	if(iter != mapIndex.end())
	{
		int __index = iter->second;
		size--;
		exist[__index] = false;
		mapIndex.erase(hostAddr);

		int pre = member[__index].pre;
		int next = member[__index].next;
		if(pre != -1 && next != -1)
		{
			member[pre].next = next;
			member[next].pre = pre;

			member[__index].pre = -1;
			member[__index].next = -1;
			member[__index].hostAddr = -1;
		}
		else if(pre == -1) //index == head
		{
			member[__index].next = -1;
			member[next].pre = -1;

			member[__index].hostAddr = -1;
			head = next;
		}
		else //index == tail
		{
			member[__index].pre = -1;
			member[pre].next = -1;

			member[__index].hostAddr = -1;
			tail = pre;
		}
	}
	else
	{
		/*printf("LRU erase error!\n");
		exit(-1);*/
		return;//nothing to do in ROW
	}
}

void
LRU::getAllHostPages(std::vector<Addr>& v)
{
    for (int i = 0 ; i < max_size ; i++) {
        if (exist[i] == true) {
            v.push_back(member[i].hostAddr);
        }
    }
    assert(v.size() == (size_t)size);
}

