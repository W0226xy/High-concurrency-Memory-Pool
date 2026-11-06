#include "CentralCache.h"
#include "PageCache.h"

CentralCache CentralCache::_sInst;

// 从 CentralCache 的某个桶中，找到一个还有空闲小对象的 span；
//如果没有，就向 PageCache 申请一个 span，把它切成多个小对象，再挂回当前桶
Span* CentralCache::GetOneSpan(SpanList& list, size_t size)
{
	// 查看当前的spanlist中是否有还有未分配对象的span
	//list 是当前大小类别对应的 SpanList。一个 span 管理一段连续页，并被切成多个固定大小的小对象。
	Span* it = list.Begin();
	while (it != list.End())
	{
		//每个span里面有_freeList指针，指向切好的小块内存的自由链表的第一个对象，如果不为空，说明该span还有未分配对象
		if (it->_freeList != nullptr)
		{
			return it;
		}
		else
		{
			it = it->_next;//找当前spanlist的下一个span
		}
	}
	// 走到这里说明当前的spanlist中没有还有未分配对象的span，需要去page cache中获取一个新的span

	// 遍历完对应size的桶发现没有可用span，先把central cache的桶锁解掉，这样如果其他线程释放内存对象回来，不会阻塞
	list._mtx.unlock();

	// 这里去page cache中获取一个span，pagecache的操作会同时影响多个桶和共享映射表，所以需要加全局锁
	PageCache::GetInstance()->_pageMtx.lock();
	//NewSpan:获取一个k页的span。NumMovePage(size)计算size大小的对象，PageCache计算一次向系统获取几个页
	Span* span = PageCache::GetInstance()->NewSpan(SizeClass::NumMovePage(size));
	PageCache::GetInstance()->_pageMtx.unlock();

	// 对获取span进行切分，不需要加锁，因为这会其他线程访问不到这个span

	// 计算span的大块内存的起始地址和大块内存的大小(字节数)
	char* start = (char*)(span->_pageId << PAGE_SHIFT);// span->_pageId是span的大块内存的起始页号，左移13位就是span的大块内存的起始地址
	size_t bytes = span->_n << PAGE_SHIFT;// span->_n是span的页数，bytes是span的大块内存的字节数
	//系统内存按页划分，第0页起始地址为0，第1页起始地址为8kb，第2页16kb。。。
	//注意：这里的“第 0 页”只是整个虚拟地址空间的第 0 个页区间，并不表示你的程序真的能使用地址 0
	// 页号：   98         99       100      101      102      103
	//        ┌────────┬────────┬────────┬────────┬────────┬────────┐
	//        │  8KB   │  8KB   │  8KB   │  8KB   │  8KB   │  8KB   │
	//        └────────┴────────┴────────┴────────┴────────┴────────┘
	//                                  └─────────── Span ───────────┘
	//                                   _pageId=100，_n=3



	char* end = start + bytes;
	//假设现在申请到一个 span，它有 1 页，每页大小是 8KB = 8192 字节；当前要给 CentralCache 分配的对象大小是 32 字节
	// 那么这个 span 可以切分成 8192 / 32 = 256 个小对象
	//假设这段内存的起始地址是 0x1000，即start = 0x1000，end = start + bytes = 0x1000 + 8192 = 0x3000
	//也就是说，可切分的内存范围是：[start, end)，即[0x1000, 0x3000)
	//每块 32 字节：
	// 第1块：0x1000 ~ 0x101F
	// 第2块：0x1020 ~ 0x103F
	// 第3块：0x1040 ~ 0x105F
	// ...
	// 第256块：0x2FE0 ~ 0x2FFF



	// 把大块内存切成自由链表链接起来，先切一块下来去做头，方便尾插
	//这里start和end是大块内存的起始地址和结束地址，size是小对象的大小
	//tail是span->_freeList自由链表的最后一个对象，初始时指向第一个小对象

	span->_freeList = start;// span->_freeList指针指向切好的小块内存的自由链表的第一个对象
	start += size;// start指针向后移动size字节，指向下一个小对象的起始地址
	void* tail = span->_freeList;// tail指针指向自由链表的最后一个对象，初始时指向第一个小对象
	int i = 1;
	//之后从 start 开始，每次移动 size 个字节，把整段内存切成大小为 size 的小对象，并用每个对象的开头空间存放下一个对象的地址：
	while (start < end)
	{
		++i;
		NextObj(tail) = start;
		tail = NextObj(tail); 
		start += size;
	}

	// 切好span以后，需要把span挂到桶里面去的时候，再加锁
	//切分过程中，这个新 span 尚未放入 CentralCache 桶，其他线程无法通过该桶找到它，所以切分本身不需要持有桶锁。切分完成后再加锁，把 span 插入链表头部，然后返回。
	list._mtx.lock();
	list.PushFront(span);

	return span;
}

// 从中心缓存获取一定数量的对象给thread cache
size_t CentralCache::FetchRangeObj(void*& start, void*& end, size_t batchNum, size_t size)
{
	size_t index = SizeClass::Index(size);
	_spanLists[index]._mtx.lock();

	Span* span = GetOneSpan(_spanLists[index], size);
	assert(span);
	assert(span->_freeList);

	// 从span中获取batchNum个对象
	// 如果不够batchNum个，有多少拿多少
	start = span->_freeList;
	end = start;
	size_t i = 0;
	size_t actualNum = 1;
	while ( i < batchNum - 1 && NextObj(end) != nullptr)
	{
		end = NextObj(end);
		++i;
		++actualNum;
	}
	span->_freeList = NextObj(end);
	NextObj(end) = nullptr;
	span->_useCount += actualNum;

	_spanLists[index]._mtx.unlock();

	return actualNum;
}

void CentralCache::ReleaseListToSpans(void* start, size_t size)
{
	size_t index = SizeClass::Index(size);
	_spanLists[index]._mtx.lock();
	while (start)
	{
		void* next = NextObj(start);

		Span* span = PageCache::GetInstance()->MapObjectToSpan(start);
		NextObj(start) = span->_freeList;
		span->_freeList = start;
		span->_useCount--;

		// 说明span的切分出去的所有小块内存都回来了
		// 这个span就可以再回收给page cache，pagecache可以再尝试去做前后页的合并
		if (span->_useCount == 0)
		{
			_spanLists[index].Erase(span);
			span->_freeList = nullptr;
			span->_next = nullptr;
			span->_prev = nullptr;

			// 释放span给page cache时，使用page cache的锁就可以了
			// 这时把桶锁解掉
			_spanLists[index]._mtx.unlock();

			PageCache::GetInstance()->_pageMtx.lock();
			PageCache::GetInstance()->ReleaseSpanToPageCache(span);
			PageCache::GetInstance()->_pageMtx.unlock();

			_spanLists[index]._mtx.lock();
		}

		start = next;
	}

	_spanLists[index]._mtx.unlock();
}