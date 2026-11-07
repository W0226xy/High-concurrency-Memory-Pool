#pragma once

#include "Common.h"

class PageCache
{
public:
	//这里和CentralCache一样，也是单例模式
	//单例模式是一种设计模式：一个类在整个程序运行期间只创建一个对象，并提供统一的方式让其他代码访问它。适合管理全局唯一的资源
	static PageCache* GetInstance()
	{
		return &_sInst;
	}

	// 获取从对象到span的映射
	Span* MapObjectToSpan(void* obj);

	// 释放空闲span回到Pagecache，并合并相邻的span
	void ReleaseSpanToPageCache(Span* span);

	// 获取一个K页的span
	Span* NewSpan(size_t k);

	std::mutex _pageMtx;//这里是PageCache的全局锁，保护整个PageCache的访问，防止多个线程同时访问PageCache导致数据不一致的问题。
	//这里设计全局锁，主要是因为 PageCache 的操作会同时影响多个桶和共享映射表，不能只把当前桶单独锁住。
	// 比如 NewSpan(k)：
	// 1. 先检查第 k 个桶；
	// 2. 如果没有空闲 span，会继续检查更大的桶；
	// 3. 找到后从大 span 中切出 k 页，再把剩余部分放入另一个桶；
	// 4. 同时更新 _idSpanMap 中多个页号到 span 的映射。
	// 而 ReleaseSpanToPageCache() 也会查找前后相邻页对应的 span，从它们所在的桶中删除，并把合并后的 span 放进新桶，还要更新映射关系。

	// 因此一次操作可能涉及多个桶、多个 span，以及 _idSpanMap。如果只锁一个桶，其他线程可能同时修改相邻桶或映射表，出现重复分配、链表状态错误、映射与 span 不一致等问题。全局锁可以把这些跨桶操作作为一个整体保护起来，逻辑更简单
	
private:
	SpanList _spanLists[NPAGES];

	//记录页号和span的映射
	std::unordered_map<PAGE_ID, Span*> _idSpanMap;

	PageCache()
	{}

	PageCache(const PageCache&) = delete;


	static PageCache _sInst;//饿汉模式：程序启动或类加载时就创建单例对象。优点是使用时不用再创建，通常也不需要额外处理并发初始化；
};