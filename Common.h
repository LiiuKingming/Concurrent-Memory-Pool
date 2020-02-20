//
// Created by 28943 on 2020/1/9.
//
#ifndef CPLUSPLUS_COMMON_H
#define CPLUSPLUS_COMMON_H

#include <iostream>
#include <cassert>
#include <map>
#include <unordered_map>
#include <thread>
#include <mutex>
#include "ObjectPool.h"

#ifdef _WIN32
#include <windows.h>
#endif // _WIN32

using std::cout;
using std::endl;

const size_t MAX_SIZE = 64 * 1024;
const size_t NFREE_LIST = MAX_SIZE / 8;
const size_t MAX_PAGES = 129;
const size_t PAGE_SHIFT = 12; // 4kΪҳλ��

inline void*& NextObj(void* obj){
    // ͨ��ǿתָ�����ͷ���ͷ��4/8���ֽڼ��洢����һ�ڴ��ĵ�ַ
    return *((void**)obj);
}

class FreeList{
private:
    void* m_freeList = nullptr;
    size_t m_num = 0;

public:
    void Push(void* obj){ // ͷ��
        NextObj(obj) = m_freeList;
        m_freeList = obj;
        ++m_num;
    }

    void* Pop() { // ͷɾ
        void *obj = m_freeList;
        m_freeList = NextObj(obj);
        --m_num;
        return obj;
    }

    void PushRange(void* head, void* tail, size_t num){
        NextObj(tail) = m_freeList;
        m_freeList = head;
        m_num += num;
    }

    size_t PopRange(void*& start, void*& end, size_t num){
        size_t actualNum = 0;
        void* prev = nullptr;
        void* cur = m_freeList;

        for(; actualNum < num && cur != nullptr; ++actualNum){
            prev = cur;
            cur = NextObj(cur);
        }

        start = m_freeList;
        end = prev;
        m_freeList = cur;

        m_num -= actualNum;

        return actualNum;
    }

    size_t Num(){
        return m_num;
    }

    bool Empty(){
        return m_freeList == nullptr;
    }

    void Clear(){
        m_freeList = nullptr;
        m_num = 0;
    }
};

class SizeClass{
public:
    // ����[1%, 10%]���ҵ��ڴ���Ƭ�˷�
    // [1, 128] 8byte���� freelist[0, 16)
    // [129, 1024] 16byte���� freelist[16, 72)
    // [1025, 8*1024] 128byte���� freelist[72, 128)
    // [8*1204 + 1, 64*1024] 1024byte���� freelist[128, 184)
    static size_t m_RoundUp(size_t size, size_t alignment){
        return (size + alignment - 1)&(~(alignment - 1));
    }

    // [9-16] + 7 = [16-23] -> 16 8 4 2 1
    // [17-32] + 15 = [32,47] ->32 16 8 4 2 1
    static inline size_t RoundUp(size_t size){
        assert(size <= MAX_SIZE);

        if (size <= 128){
            return  m_RoundUp(size, 8);
        } else if (size <= 1024){
            return  m_RoundUp(size, 16);
        } else if (size <= 8192){
            return  m_RoundUp(size, 128);
        } else if(size <= 65536){
            return  m_RoundUp(size, 1024);
        }

        return -1;
    }

    // [9, 16] + 7  -> [16, 23]
    static size_t m_ListIndex(size_t size, size_t alignShift){
        return ((size + (1 << alignShift) - 1) >> alignShift) - 1;
    }

    static size_t ListIndex(size_t size){
        assert(size <= MAX_SIZE);

        // ÿ�������ж��ٸ���
        static int s_groupArray[4] = {16, 56, 56, 56};

        if(size <= 128){
            return m_ListIndex(size, 3); // 1 << 3
        } else if(size <= 1024){
            return m_ListIndex(size - 128, 4) + s_groupArray[0]; // 1 << 4

        } else if(size <= 8192){
            return m_ListIndex(size - 1024, 7) + s_groupArray[1] + s_groupArray[0]; // 1 << 7

        } else if(size <= 65536){
            return m_ListIndex(size - 8192, 10) + s_groupArray[2] + s_groupArray[1] + s_groupArray[0]; // 1 << 10
        }

        return -1;
    }

    // tc��ccҪpage
    static size_t NumMoveSize(size_t size){
        if(size == 0){
            return 0;
        }

        size_t num = MAX_SIZE / size;
        if(num < 2){
            num = 2;
        }
        // ��С�������8byteǡ������4k
        if(num > 512){
            return 512;
        }

        return num;
    }

    static size_t NumMovePage(size_t size){
        size_t num = NumMoveSize(size);
        size_t npage = num * size;

        npage >>= 12; // �൱��npage / 4k
        if(npage == 0){
            npage = 1;
        }

        return npage;
    }
};

#ifdef _WIN32
typedef unsigned int PAGE_ID;
#else
typedef unsigned long long PAPAGE_ID ;
#endif // _WIN32

// span : ����ҳΪ��λ�Ķ���, �����Ƿ������ϲ�, ����ڴ���Ƭ����
struct Span{
    PAGE_ID m_pageid = 0; // ҳ��
    PAGE_ID m_pagesize = 0; // ҳ������

    FreeList m_freeList; // ������������
    size_t m_objSize = 0; // �����������Ĵ�С
    int m_usecount = 0; // �ڴ�����ʹ�ü���

    Span* m_next = nullptr;
    Span* m_prev = nullptr;
};

class SpanList{
    Span* m_head;
    std::mutex m_mtx;

public:
    SpanList(){
        m_head = new Span; // �滻�ɶ����
        m_head->m_next = m_head;
        m_head->m_prev = m_head;
    }

    Span* Begin(){
        return m_head->m_next;
    }

    Span* End(){
        return m_head;
    }

    void PushFront(Span* newspan){
        Insert(m_head->m_next, newspan);
    }

    void PopFront(){
        Erase(m_head->m_next);
    }

    void PushBack(Span* newspan){
        Insert(m_head, newspan);
    }

    void PopBack(){
        Erase(m_head->m_prev);
    }

    static void Insert(Span* pos, Span* newspan){
        Span* prev = pos->m_prev;

        // prev newspan pos
        prev->m_next = newspan;
        newspan->m_next = pos;
        pos->m_prev = newspan;
        newspan->m_prev = prev;
    }

    void Erase(Span* pos){
        assert(pos != m_head);

        Span* prev = pos->m_prev;
        Span* next = pos->m_next;

        prev->m_next = next;
        next->m_prev = prev;
    }

    bool Empty(){
        return Begin() == End();
    }

    void Lock(){
        m_mtx.lock();
    }

    void Unlock(){
        m_mtx.unlock();
    }

};

inline static void* SystemAlloc(size_t numPage){
#ifdef _WIN32
    void* ptr = VirtualAlloc(0, numPage * (1 << PAGE_SHIFT),
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    // brk mmap
#endif
    if (ptr == nullptr){
        throw std::bad_alloc();
    }
    return ptr;
}

inline static void SystemFree(void* ptr){
#ifdef _WIN32
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
#endif
}

#endif //CPLUSPLUS_COMMON_H