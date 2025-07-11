#pragma once
#include <memory>
#include <assert.h>
#include <string>

// 哈希表节点，应该嵌入到有效载荷中
struct HNode {
    HNode *next = nullptr;
    uint64_t hcode = 0;
};

// 一个简单的固定大小的哈希表
struct HTab {
    // 元素数组
    HNode **tab = nullptr;
    // 哈希码
    size_t mask = 0;
    // 元素个数
    size_t size = 0;
};

// 真正的哈希表接口
struct HMap {
    /* 我们的哈希表大小是固定的，要是负载因子（load factor）太高，就得换个大的。
    在Redis里用哈希表时，还有个额外的考量。给大哈希表调整大小，得把好多节点挪到新表里去，这可能会让服务器卡顿一会儿。
    为了避免这种情况，我们不一下子挪动所有节点，而是用两个哈希表，慢慢地把节点在它们之间转移*/
    HTab ht1;
    HTab ht2;
    size_t resizing_pos = 0;
};


/**
 * @brief 初始化一个固定大小的哈希表
 * @param n 必须是2的幂次方
 * @note 当哈希表的大小是2的幂次方时，索引操作就只是用哈希码做个简单的按位掩码运算
 * */ 
static void h_init(HTab *htab, size_t n);

/**
 * @brief 哈希表插入操作
*/
static void h_insert(HTab *htab, HNode *node);

/**
 * @brief 哈希表查找子程序
 * @return 拥有目标节点的父指针的地址
 * @note 这个地址可以用来删除目标节点
*/
static HNode **h_lookup(HTab *htab, HNode *key, bool (*cmp)(HNode *, HNode *));

/**
 * @brief 哈希表删除子程序
*/
static HNode *h_detach(HTab *htab, HNode **from);

/**
 * @brief 慢慢挪动节点
 * @note 我们的哈希表大小是固定的，要是负载因子（load factor）太高，就得换个大的。在Redis里用哈希表时，还有个额外的考量。给大哈希表调整大小，得把好多节点挪到新表里去，这可能会让服务器卡顿一会儿。为了避免这种情况，我们不一下子挪动所有节点，而是用两个哈希表，慢慢地把节点在它们之间转移
*/
static void hm_help_resizing(HMap *hmap);

/**
 * @brief 插入节点
 * @note 在表太满时会触发调整大小操作
*/
void hm_insert(HMap *hmap, HNode *node);

/**
 * @brief 查找节点
*/
HNode *hm_lookup(HMap *hmap, HNode* key, bool (*cmp)(HNode *, HNode *));

/**
 * @brief 触发大小调整操作
*/
static void hm_start_resizing(HMap *hmap);

/**
 * @brief 删除HMap中的key 
*/
HNode *hm_pop(HMap *hmap, HNode* key, bool (*cmp)(HNode *, HNode *));

// 键的结构
struct Entry {
    HNode node;
    std::string key;
    std::string val;
};
// 键空间的数据结构
static struct {
    HMap db;
} g_data;

/**
 * @brief 侵入式数据结构核心宏
 * @note hm_lookup函数返回一个指向HNode的指针，而HNode是Entry的一个成员，我们得用点指针运算把这个指针转成Entry指针。在C项目里，container_of宏就是干这个用的
*/
#define container_of(ptr, type, member) ({\
    const typeof(((type *)0)->member ) *mptr = (ptr);\
    (type *)((char *) mptr - offsetof(type, member));})
bool entry_eq(HNode *lhs, HNode * rhs);