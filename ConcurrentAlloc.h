#pragma once
#include "PageCache.h"
#include "Common.h"
#include "ThreadCache.h"

static void* ConcurrentAlloc(size_t size)
{
	if(size>MAX_BYTES)
	{
		
		size_t alignSize=SizeClass::RoundUp(size);
		size_t kpage=alignSize>>PAGE_SHIFT;
		
		PageCache::GetInstance()->_pageMtx.lock();
		Span* span=PageCache::GetInstance()->NewSpan(kpage);
		PageCache::GetInstance()->_pageMtx.unlock();
		
		void* ptr=(void*)(span->_pageId<<PAGE_SHIFT);
		
		return ptr;
	}
	// 通过TLS 每个线程无锁的获取自己的专属的ThreadCache对象
	if (pTLSThreadCache == nullptr)
	{
		pTLSThreadCache = new ThreadCache;
	}

	cout << std::this_thread::get_id() << ":"<<pTLSThreadCache<<endl;

	return pTLSThreadCache->Allocate(size);
}

static void ConcurrentFree(void* ptr, size_t size)
{
	if(size>MAX_BYTES)
	{

		
		PageCache::GetInstance()->_pageMtx.lock();
		//根据内存对象地址，算出对应的页号，然后查找页号对应的span
		Span* span=PageCache::GetInstance()->MapObjectToSpan(ptr);
		PageCache::GetInstance()->ReleaseSpanToPageCache(span);
		PageCache::GetInstance()->_pageMtx.unlock();
		
		return;
	}
	else
	{
		assert(pTLSThreadCache);
		pTLSThreadCache->Deallocate(ptr, size);
	}

	
}