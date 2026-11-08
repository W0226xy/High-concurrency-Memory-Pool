#include "PageCache.h"

PageCache PageCache::_sInst;

// 获取一个K页的span
Span* PageCache::NewSpan(size_t k)
{
	//要申请的内存>128页，向堆要
	if(k>NPAGES-1)
	{
		void* ptr=SystemAlloc(k);
		Span* span=new Span;
		span->_pageId=(PAGE_ID)ptr>>PAGE_SHIFT;
		span->_n=k;
		_idSpanMap[span->_pageId]=span;

		return span;
	}

	assert(k > 0 && k < NPAGES);

	

	// 先检查第k个桶里面有没有span
	if (!_spanLists[k].Empty())
	{
		return _spanLists[k].PopFront();
	}

	// 检查一下后面的桶里面有没有span，如果有可以把它进行切分
	for (size_t i = k+1; i < NPAGES; ++i)
	{
		if (!_spanLists[i].Empty())
		{
			Span* nSpan = _spanLists[i].PopFront();
			Span* kSpan = new Span;

			// 在nSpan的头部切一个k页下来
			// k页span返回给centracache
			// n-k页的Span再挂到对应映射的位置
			kSpan->_pageId = nSpan->_pageId;
			kSpan->_n = k;

			nSpan->_pageId += k;
			nSpan->_n -= k;


			//切剩下的挂到pagecache对应页的桶上
			_spanLists[nSpan->_n].PushFront(nSpan);
			// 存储nSpan的首尾页号跟nSpan映射，方便page cache回收内存时进行的合并查找
			_idSpanMap[nSpan->_pageId] = nSpan;
			_idSpanMap[nSpan->_pageId + nSpan->_n - 1] = nSpan;


			// 建立id和span的映射，方便central cache回收小块内存时，查找对应的span
			for (PAGE_ID i = 0; i < kSpan->_n; ++i)
			{	//建立要分配出去的页号和kspan的映射，方便central cache回收小块内存时，查找对应的span
				//也就是分配出去的对应的页属于哪个span
				_idSpanMap[kSpan->_pageId + i] = kSpan;
			}

			return kSpan;
		}
	}

	// 走到这个位置就说明后面没有大页的span了，一般是刚开始的时候，pagecache还没有从堆上申请过span，或者是page cache里面的span都被central cache分配完了
	Span* bigSpan = new Span;
	// 这时就去找堆要一个128页的span，插入到128号桶
	void* ptr = SystemAlloc(NPAGES - 1);
	bigSpan->_pageId = (PAGE_ID)ptr >> PAGE_SHIFT;
	bigSpan->_n = NPAGES - 1;
	_spanLists[bigSpan->_n].PushFront(bigSpan);

	//再递归调一次，前面的逻辑就会把128页的span切分成k页的span返回给central cache
	return NewSpan(k);
}

//根据内存对象地址，算出对应的页号，然后查找页号对应的span
Span* PageCache::MapObjectToSpan(void* obj)
{
	PAGE_ID id = ((PAGE_ID)obj >> PAGE_SHIFT);
	auto ret = _idSpanMap.find(id);
	if (ret != _idSpanMap.end())
	{
		return ret->second;
	}
	else
	{
		assert(false);
		return nullptr;
	}
}

void PageCache::ReleaseSpanToPageCache(Span* span)
{
	//释放大于128页的span，说明不是找内存池要的，直接释放给堆
	if(span->_n > NPAGES - 1)
	{
		void* ptr = (void*)(span->_pageId << PAGE_SHIFT);// span->_pageId是span的大块内存的起始页号，左移13位就是span的大块内存的起始地址
		SystemFree(ptr);
		delete span;
		return;
	}


	// 对span前后的页，尝试进行合并，缓解内存碎片(外碎片)问题
	//因为如果不合并，返回的很多都是较小页的span，如果想要大块内存，就又需要去堆上重新申请，增加了堆的压力
	while (1)
	{
		PAGE_ID prevId = span->_pageId - 1;//获取当前span前1页的pageid
		auto ret = _idSpanMap.find(prevId);//根据页号找到对应span
		// 前面的页号没有，不合并了
		if (ret == _idSpanMap.end())
		{
			break;
		}

		// 前面相邻页的span在使用，不合并了
		Span* prevSpan = ret->second;
		if (prevSpan->_isUse == true)
		{
			break;
		}

		// 合并出超过128页的span没办法管理，不合并了
		//这里是有可能的，比如前一个申请了128页的span，后一个申请了1页的span，这两个span的页号正好相邻
		if (prevSpan->_n + span->_n > NPAGES-1)
		{
			break;
		}

		span->_pageId = prevSpan->_pageId;//合并完，前一个span的页号就是大块内存页号
		span->_n += prevSpan->_n;//合并完，前一个span的页数加上当前span的页数就是大块内存页数

		_spanLists[prevSpan->_n].Erase(prevSpan);
		delete prevSpan;
	}

	// 向后合并
	while (1)
	{
		PAGE_ID nextId = span->_pageId + span->_n;//获取当前span后n页的pageid，当前span共n页
		auto ret = _idSpanMap.find(nextId);//根据页号找到对应span
		// 后面的页号没有，不合并了
		if (ret == _idSpanMap.end())
		{
			break;
		}
		// 后面相邻页的span在使用，不合并了
		Span* nextSpan = ret->second;
		if (nextSpan->_isUse == true)
		{
			break;
		}
		// 合并出超过128页的span没办法管理，不合并了	
		if (nextSpan->_n + span->_n > NPAGES-1)
		{
			break;
		}
		// 合并完页数加上后一个span的页数就是大块内存页数，原先span起始页号不需要改
		span->_n += nextSpan->_n;

		_spanLists[nextSpan->_n].Erase(nextSpan);
		delete nextSpan;
	}

	// 合并完后，把span挂到pagecache对应页的桶上
	_spanLists[span->_n].PushFront(span);
	//合并完的span标记为未使用
	span->_isUse = false;
	// 建立id和span的映射，方便central cache回收小块内存时，查找对应的span
	_idSpanMap[span->_pageId] = span;
	_idSpanMap[span->_pageId+span->_n-1] = span;
}

