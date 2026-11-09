#pragma once

#include <iostream>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include <time.h>
#include <assert.h>

#include <thread>
#include <mutex>

using std::cout;
using std::endl;

#ifdef _WIN32
	#include <windows.h>
#else
	// ...
#endif

static const size_t MAX_BYTES = 256 * 1024;
static const size_t NFREELIST = 208;//最多208个自由链表桶
static const size_t NPAGES = 129;//PageCache最大的桶128page，从0号桶开始，_spanLists[k]存放k页的span，0号桶不存放span，1号桶存放1页的span，128号桶存放128页的span
static const size_t PAGE_SHIFT = 13;// 每页8KB，2^13字节

// PAGE_ID需要容纳指针转换后的整数，uintptr_t在32/64位平台上都适用。
using PAGE_ID = std::uintptr_t;

// 直接去堆上按页申请空间
inline static void* SystemAlloc(size_t kpage)//kpage表示申请的页数，每页8KB
{
#ifdef _WIN32
	//VirtualAlloc 是 Windows 的底层虚拟内存申请接口
	// kpage << 13 表示申请的字节数，每页8KB，总共 kpage 页
	//0: 表示系统自动选择起始地址
	// MEM_COMMIT | MEM_RESERVE: 同时提交和保留内存
	// PAGE_READWRITE: 可读写权限
	void* ptr = VirtualAlloc(0, kpage << 13, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
	// 页号按 8KB 计算，所以起始地址也必须按 8KB 对齐。
	const size_t bytes = kpage << PAGE_SHIFT;
	void* ptr = std::aligned_alloc(size_t{1} << PAGE_SHIFT, bytes);
#endif

	if (ptr == nullptr)
		throw std::bad_alloc();

	return ptr;
}

inline static void SystemFree(void* ptr)
{
#ifdef _WIN32
	VirtualFree(ptr, 0, MEM_RELEASE);
#else
	// sbrk unmmap等
#endif
}


static void*& NextObj(void* obj)// 获取obj的下一个对象指针，返回对下一个对象指针的引用
{//obj下一个对象指针在obj所占内存的起始位置，用来存储自由链表中下一个对象的地址
	//32位系统下是4字节，64位系统下是8字节，保证每个空闲内存块至少能存储一个指针的大小
	//ps：void*不能直接做 ++ 或 +1 因为 void 表示“未知类型”，编译器不知道它指向的对象大小，也就无法决定该移动多少字节。
	//void* 主要适合“先保存一个通用地址”；需要访问、移动或做地址运算时，再转回具体指针类型。
	return *(void**)obj;
}


// 管理切分好的小对象的自由链表
class FreeList
{
public:
	void Push(void* obj)
	{
		assert(obj);

		// 头插
		//*(void**)obj = _freeList;
		NextObj(obj) = _freeList;// 将obj的下一个指针指向当前自由链表第一个对象
		_freeList = obj;// 将obj设置为新的自由链表第一个对象

		++_size;
	}

	//头插入n个对象
	void PushRange(void* start, void* end, size_t n)
	{
		NextObj(end) = _freeList;
		_freeList = start;

		_size += n;
	}

	//取n个对象（头删）
	void PopRange(void*& start, void*& end, size_t n)
	{
		assert(n <= _size);
		start = _freeList;
		end = start;

		for (size_t i = 0; i < n - 1; ++i)
		{
			end = NextObj(end);
		}

		_freeList = NextObj(end);
		NextObj(end) = nullptr;
		_size -= n;
	}

	void* Pop()
	{
		assert(_freeList);

		// 头删
		void* obj = _freeList;// 取出自由链表第一个对象
		_freeList = NextObj(obj);// 将自由链表头指向下一个对象
		--_size;// 自由链表中对象数量减1

		return obj;
	}

	bool Empty()
	{
		return _freeList == nullptr;
	}

	size_t& MaxSize()
	{
		return _maxSize;
	}

	size_t Size()
	{
		return _size;
	}

private:
	void* _freeList = nullptr;// 自由链表的头指针,指向自由链表中的第一个对象
	size_t _maxSize = 1;
	size_t _size = 0;
};

// 计算对象大小的对齐映射规则
class SizeClass
{
public:
	// 整体控制在最多10%左右的内碎片浪费
	// [1,128]					8byte对齐	    freelist[0,16) 16个桶
	// [128+1,1024]				16byte对齐	    freelist[16,72) 56个桶
	// [1024+1,8*1024]			128byte对齐	    freelist[72,128) 56个桶
	// [8*1024+1,64*1024]		1024byte对齐     freelist[128,184) 56个桶
	// [64*1024+1,256*1024]		8*1024byte对齐   freelist[184,208) 24个桶

	// 哈希桶对应位置如下：
	// 8
	// 16
	// 24
	// ...
	// 128
	// 144
	// 160
	// ...
	// 1024
	// 1152
	// 1280
	// ...
	// 8*1024
	// 9*1024
	// ...
	// 64*1024
	// 72*1024
	// ...
	// 256*1024

	//以144字节的桶举例，申请129字节的对象，会被对齐到144字节，映射到对应的自由链表桶
	//(144-129)/144约等于0.104，说明有约10%的内碎片浪费

	/*size_t _RoundUp(size_t size, size_t alignNum)
	{
		size_t alignSize;
		if (size % alignNum != 0)
		{
			alignSize = (size / alignNum + 1)*alignNum;
		}
		else
		{
			alignSize = size;
		}

		return alignSize;
	}*/
	// 1-8 
	static inline size_t _RoundUp(size_t bytes, size_t alignNum)
	{
		// 将bytes上调到alignNum的整数倍
		// return ((bytes + alignNum - 1) & ~(alignNum - 1));
		//下面写法和上面注释掉的位运算写法效果一样，只是更直观

		// 已经是 alignNum 的整数倍，直接返回
		if (bytes % alignNum == 0)
		{
			return bytes;
		}
		// 否则补到下一个 alignNum 的整数倍
		return (bytes / alignNum + 1) * alignNum;
		//举个例子_RoundUp(129, 16) = 144，说明申请129字节的对象会被对齐到144字节
		//_RoundUp(9, 8); // 结果为16，说明申请9字节的对象会被对齐到16字节
		//9%8！=0，说明9不是8的整数倍
		//(9/8+1)*8 = 16，说明9字节被对齐到16字节
	}

	static inline size_t RoundUp(size_t size)
	{
		// 根据对象大小选择合适的对齐方式
		if (size <= 128)
		{//这里首先用8字节对齐，因为64位系统下指针大小为8字节，保证每个空闲内存块至少能存储一个指针的大小
			return _RoundUp(size, 8);
		}
		else if (size <= 1024)
		{
			return _RoundUp(size, 16);
		}
		else if (size <= 8*1024)
		{
			return _RoundUp(size, 128);
		}
		else if (size <= 64*1024)
		{
			return _RoundUp(size, 1024);
		}
		else if (size <= 256 * 1024)
		{
			return _RoundUp(size, 8*1024);
		}
		else// size > 256 * 1024，以页为单位对齐,这里写成1<<PAGE_SHIFT是为了方便以后修改页大小，假设页大小改为16KB，只需要修改PAGE_SHIFT的值即可
		{
			return _RoundUp(size, 1<<PAGE_SHIFT);
		}
	}

	// static inline size_t _Index(size_t bytes, size_t alignNum)
	// {
	// 	if (bytes % alignNum == 0)
	// 	{
	// 		return bytes / alignNum - 1;
	// 	}
	// 	else
	// 	{
	// 		return bytes / alignNum;
	// 	}
	// 	// 这里的逻辑是将字节数映射到对应的自由链表索引
	// 	// 如果bytes是alignNum的整数倍，索引为bytes/alignNum - 1
	// 	// 否则索引为bytes/alignNum

	// 	// 举个例子，_Index(16, 8) = 1，_Index(17, 8) = 2
	// 	// 对应的自由链表索引从0开始
	// 	// _Index(7, 8) = 7/8 = 0, 对应自由链表的第0个桶
	// 	// _Index(8, 8) = 8/8 - 1 = 0, 对应自由链表的第0个桶
	// 	// _Index(16, 8) = 16/8 - 1 = 1
	// 	// _Index(17, 8) = 17/8 = 2
	// }


	//实现效果和上面一样
	static inline size_t _Index(size_t bytes, size_t align_shift)
	{
		// 这里的逻辑是将字节数映射到对应的自由链表索引
		// align_shift 是对齐的位移量，比如8字节对齐，align_shift = 3
		// 如果bytes是2的align_shift次方的整数倍，索引为(bytes >> align_shift) - 1
		// 否则索引为((bytes + (1 << align_shift) - 1) >> align_shift) - 1
		return ((bytes + (1 << align_shift) - 1) >> align_shift) - 1;
	}

	// 计算映射的哪一个自由链表桶
	static inline size_t Index(size_t bytes)
	{
		assert(bytes <= MAX_BYTES);

		static int group_array[5] = { 16, 56, 56, 56, 24 };
		// group_array记录每个区间的链表数量
		// [1,128] 8字节对齐 16个桶
		// [129,1024] 16字节对齐 56个桶
		// [1025,8*1024] 128字节对齐 56个桶
		// [8*1024+1,64*1024] 1024字节对齐 56个桶
		// [64*1024+1,256*1024] 8*1024字节对齐 24个桶
		if (bytes <= 128){
			// 8字节对齐，索引从0开始
			//举个例子
			//_Index(128, 3)
			// = ((128 + 8 - 1) >> 3) - 1
			// = (135 >> 3) - 1
			// = 16 - 1
			// = 15，对应自由链表的第15个桶
			return _Index(bytes, 3);
		}
		else if (bytes <= 1024){
			// 16字节对齐，索引从16开始
			// 举个例子
			//_Index(129-128, 4)
			// = ((1 + 16 - 1) >> 4) - 1
			// = (16 >> 4) - 1
			// = 1 - 1
			// = 0
			//0+group_array[0] = 16，对应自由链表的第16个桶
			return _Index(bytes - 128, 4) + group_array[0];
		}
		else if (bytes <= 8 * 1024){
			// 128字节对齐，索引从72开始
			return _Index(bytes - 1024, 7) + group_array[1] + group_array[0];
		}
		else if (bytes <= 64 * 1024){
			// 1024字节对齐，索引从128开始
			return _Index(bytes - 8 * 1024, 10) + group_array[2] + group_array[1] + group_array[0];
		}
		else if (bytes <= 256 * 1024){
			// 8*1024字节对齐，索引从184开始
			return _Index(bytes - 64 * 1024, 13) + group_array[3] + group_array[2] + group_array[1] + group_array[0];
		}
		else{
			assert(false);
		}

		return -1;
	}

	// 一次thread cache从中心缓存获取多少个
	static size_t NumMoveSize(size_t size)
	{
		assert(size > 0);

		// [2, 512]，一次批量移动多少个对象的(慢启动)上限值
		// 小对象一次批量上限高
		// 小对象一次批量上限低
		int num = MAX_BYTES / size;
		if (num < 2)
			num = 2;

		if (num > 512)
			num = 512;

		return num;
	}

	// PageCache计算一次向系统获取几个页
	// 单个对象 8byte
	// ...
	// 单个对象 256KB
	static size_t NumMovePage(size_t size)
	{
		//该size大小的对象，threadCache一次性向centralCache要多少个。走到这个函数说明centralcache中没有对应大小的span可供切割了，来向pagecache申请。
		size_t num = NumMoveSize(size);
		//对象越小，分配的页越少（对象越小，1页可切分的对象越多）
		size_t npage = num*size;//对象个数*单个对象大小，这里算的是整体的字节大小
		//字节大小转换成对应页数
		npage >>= PAGE_SHIFT;
		//至少给1页
		if (npage == 0)
			npage = 1;

		return npage;
	}
};

// 管理多个连续页大块内存跨度结构
struct Span
{
	PAGE_ID _pageId = 0; // 大块内存起始页的页号
	size_t  _n = 0;      // 页的数量

	Span* _next = nullptr;	// 双向链表的结构
	Span* _prev = nullptr;

	size_t _useCount = 0; // 切好小块内存，被分配给thread cache的计数
	void* _freeList = nullptr;  // 切好的小块内存的自由链表

	bool _isUse = false;          // 当前span是否在被使用（用于pagecache合并内存时判断）
};

// 带头双向循环链表 
class SpanList
{
public:
	SpanList()
	{
		_head = new Span;
		_head->_next = _head;
		_head->_prev = _head;
	}

	Span* Begin()
	{
		return _head->_next;
	}

	Span* End()
	{
		return _head;
	}

	bool Empty()
	{
		return _head->_next == _head;
	}

	void PushFront(Span* span)
	{
		Insert(Begin(), span);
	}

	Span* PopFront()
	{
		Span* front = _head->_next;
		Erase(front);
		return front;
	}

	void Insert(Span* pos, Span* newSpan)
	{
		assert(pos);
		assert(newSpan);

		Span* prev = pos->_prev;
		// prev newspan pos
		prev->_next = newSpan;
		newSpan->_prev = prev;
		newSpan->_next = pos;
		pos->_prev = newSpan;
	}

	void Erase(Span* pos)
	{
		assert(pos);
		assert(pos != _head);

		Span* prev = pos->_prev;
		Span* next = pos->_next;

		prev->_next = next;
		next->_prev = prev;
	}

private:
	Span* _head;
public:
	std::mutex _mtx; // 桶锁
};
