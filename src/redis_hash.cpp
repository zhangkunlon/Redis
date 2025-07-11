#include "redis_hash.h"
#include "redis_socket.h"
#include "redis_log.h"
const size_t k_resizing_work = 1024; // 调整大小时每次转移的节点数

static void h_init(HTab *htap, size_t n) {
    assert(n > 0 && ((n - 1) & n) == 0);
    htap->tab = (HNode **)calloc(sizeof(HNode *), n);
    htap->mask = n - 1;
    htap->size = 0;
}

static void h_insert(HTab *htab, HNode *node) {
    size_t pos = node->hcode & htab->mask;
    HNode *next = htab->tab[pos];
    node->next = next;
    htab->tab[pos] = node;
    htab->size++;
}

static HNode **h_lookup(HTab *htab, HNode *key, bool (*cmp)(HNode *, HNode *)) {
    if (htab->tab == nullptr) {
        return nullptr;
    }

    size_t pos = key->hcode & htab->mask;
    HNode **from = &htab->tab[pos]; //一个链表
    while (*from != nullptr) {
        if (cmp(*from, key)) return from;
        from = &(*from)->next;
    }
    return nullptr;
}

static HNode *h_detach(HTab *htab, HNode **from) {
    HNode *node = *from;
    *from = (*from)->next;
    htab->size--;
    return node;
}

static void hm_help_resizing(HMap *hmap) {
    if (hmap->ht2.tab == nullptr) return;

    size_t nwork = 0;
    while (nwork < k_resizing_work && hmap->ht2.size > 0) {
        // 检查是否已经扫描完所有槽位
        if (hmap->resizing_pos > hmap->ht2.mask) {
            break;
        }

        // 从ht2里扫描节点并把它们挪到ht1
        HNode **from = &hmap->ht2.tab[hmap->resizing_pos];
        if (*from == nullptr) {
            hmap->resizing_pos++;
            continue;
        }

        h_insert(&hmap->ht1, h_detach(&hmap->ht2, from));
        nwork++;
    }

    if (hmap->ht2.size == 0) {
        // 转移完成
        free(hmap->ht2.tab);
        hmap->ht2 = HTab{};
        hmap->resizing_pos = 0;
        REDIS_LOG(LogLevel::DEBUG, "resizing done");
    }
}

static void hm_start_resizing_double(HMap *hmap) {
    assert(hmap->ht2.tab == nullptr);
    // 创建一个更大的哈希表并交换它们
    hmap->ht2 = hmap->ht1;
    h_init(&hmap->ht1, (hmap->ht1.mask + 1) * 2);
    hmap->resizing_pos = 0;
    REDIS_LOG(LogLevel::DEBUG, "start resizing, new capacity: %d", hmap->ht1.mask + 1);
}

const size_t k_max_load_factor = 4; // 最大负载因子
const size_t k_min_capacity = 4; // 最小容量，防止哈希表过小

static void hm_start_resizing_half(HMap *hmap) {
    assert(hmap->ht2.tab == nullptr);
    size_t new_capacity = (hmap->ht1.mask + 1) / 2;
    // 确保不会缩容到小于最小容量
    if (new_capacity < k_min_capacity) {
        new_capacity = k_min_capacity;
    }
    // 创建一个更小的哈希表并交换它们
    hmap->ht2 = hmap->ht1;
    h_init(&hmap->ht1, new_capacity);
    hmap->resizing_pos = 0;
    REDIS_LOG(LogLevel::DEBUG, "start shrinking, new capacity: %zu", hmap->ht1.mask + 1);
}

void hm_insert(HMap *hmap, HNode *node) {
    if (hmap->ht1.tab == nullptr) {
        h_init(&hmap->ht1, k_min_capacity);
    }

    h_insert(&hmap->ht1, node);

    if (hmap->ht2.tab == nullptr) {
        // 检查是否需要扩容
        size_t capacity = hmap->ht1.mask + 1;
        size_t load_factor = hmap->ht1.size / capacity;
        if (load_factor >= k_max_load_factor) {
            hm_start_resizing_double(hmap);
        }
    }
    hm_help_resizing(hmap);
}

HNode *hm_pop(HMap *hmap, HNode* key, bool (*cmp)(HNode *, HNode *)) {
    hm_help_resizing(hmap);
    HNode *result = nullptr;

    HNode **from = h_lookup(&hmap->ht1, key, cmp);
    if (from) {
        result = h_detach(&hmap->ht1, from);
    } else {
        from = h_lookup(&hmap->ht2, key, cmp);
        if (from) {
            result = h_detach(&hmap->ht2, from);
        }
    }

    // 删除成功后检查是否需要缩容
    if (result && hmap->ht2.tab == nullptr) {
        size_t capacity = hmap->ht1.mask + 1;
        // 当负载因子过低且容量大于最小容量时，触发缩容
        if (capacity > k_min_capacity && hmap->ht1.size * k_max_load_factor < capacity) {
            hm_start_resizing_half(hmap);
        }
    }

    hm_help_resizing(hmap);
    return result;
}

HNode *hm_lookup(HMap *hmap, HNode* key, bool (*cmp)(HNode *, HNode *)) {
    hm_help_resizing(hmap);
    HNode **from = h_lookup(&hmap->ht1, key, cmp);
    if (!from) {
        from = h_lookup(&hmap->ht2, key, cmp);
    }
    return from ? *from : nullptr;
}

bool entry_eq(HNode *lhs, HNode * rhs) {
    struct Entry *le = container_of(lhs, struct Entry, node);
    struct Entry * re = container_of(rhs, struct Entry, node);
    return lhs->hcode == rhs->hcode && le->key == re->key;
}