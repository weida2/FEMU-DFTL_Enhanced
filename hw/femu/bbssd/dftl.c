#include "dftl.h"
static void *ftl_thread(void *arg);
static uint64_t translation_page_write(struct ssd *ssd, uint64_t vpn);
static uint64_t translation_page_read(struct ssd *ssd, uint64_t vpn, NvmeRequest *req, struct nand_lun **trans_lun);

uint64_t dftl_evict(DFTLTable *d_maptbl, LRUCache *cache, LRUCache* nand_cache, uint64_t *lat, struct ssd* ssd);
uint64_t addNodeToNandCache(LRUCache *nand_cache, Node *node);
static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct
        nand_cmd *ncmd);
static inline struct ppa pgidx2ppa(struct ssd *ssd, uint64_t pgidx);


/**
 * @brief DFTL_method
 * 
 * 创建CMT_table_Node
 */


uint64_t hash(uint64_t key, uint64_t capacity)
{
    return key % capacity;
}
void map_set_bit(uint8_t *map, uint8_t bit_idx) {
    *map |= (1 << bit_idx);
}

uint8_t map_check_bit(uint8_t *map, uint8_t bit_idx) {
    return *map & (1 << bit_idx);
}

void map_clear_bit(uint8_t *map, uint8_t bit_idx) {
    *map &= ~(1 << bit_idx);    
}


Node *createNode(struct ssd* ssd, uint64_t page_idx) {
    Node *new_node = (Node *)malloc(sizeof(Node));

    // 当下刷时再获取在闪存块中的ppn
    // struct ppa ppa;
    // ppa = get_new_page(ssd, TRANS);
    // ssd_advance_write_pointer(ssd, TRANS);
    // uint64_t ppn = ppa2vpgidx(ssd, &ppa);
    // new_node->ppn = ppn;        // ppn

    //ssd->rmap[vpgidx] = page_idx; // rmap.g == page.type ? 0: translate page, 1: data page 
                                    // rmap.vpn, rmap.lpn


 
    new_node->page_idx = page_idx; // vpn

    new_node->next = NULL;
    new_node->pre = NULL;
    
    new_node->l2p_entries = (uint64_t *)malloc(sizeof(uint64_t) * ENTRY_PER_PAGE);
    for (int i = 0; i < ENTRY_PER_PAGE; i++) {
        new_node->l2p_entries[i] = UNMAPPED_PPA;
    }
    
    new_node->update_in_flash = true; // dirty
    new_node->hash_next = NULL;
    
    return new_node;
}


// 创建LRU 采用哈希链式结构
LRUCache* createLRUCache(uint64_t capacity, uint32_t bmpCnt) {
    LRUCache *cache = (LRUCache *)malloc(sizeof(LRUCache));
    cache->capacity = capacity;
    cache->count = 0;
    cache->head = NULL;
    cache->tail = NULL;

    cache->hashTable = (Node **)malloc(sizeof(Node *) * capacity);
    for (int i = 0; i < capacity; i++) {
        cache->hashTable[i] = NULL;
    }

    // 初始化位图 用来判断该页在 闪存上 或者 cache 上
    cache->flash_bmp = (uint8_t *)malloc(sizeof(uint8_t) * bmpCnt);
    memset(cache->flash_bmp, 0, sizeof(uint8_t) * bmpCnt);

    assert(cache->flash_bmp[0] == (uint8_t)0);

    return cache;
}


void deleteLRUCache(LRUCache* cache) {
    if (cache == NULL) return ;
    if (cache->hashTable) {
        for (int i = 0; i < cache->capacity; i++) {
            Node *node = cache->hashTable[i];
            while (node != NULL) {
                Node *node_next = node->hash_next;
                free(node);
                node = node_next;
            }
        }
        free(cache->hashTable);
    }
    free(cache->flash_bmp);
    free(cache);
}


// 头插法 新插入的直接addtoFront
void addToFront(LRUCache *cache, Node *node) {
    node->next = cache->head;
    node->pre = NULL;

    if (cache->head != NULL) {
        cache->head->pre = node;
    }
    cache->head = node;
    if (cache->tail == NULL) {
        cache->tail = node;
    }
}

// 已经存在的 moveToFront
void moveToFront(LRUCache *cache, Node *node) {
    if (node == cache->head) {
        return ;
    }
    if (node == cache->tail) {
        cache->tail = node->pre;
    }

    node->pre->next = node->next;
    if (node->next != NULL) {
        node->next->pre = node->pre;
    }
    addToFront(cache, node);
}

// tmp no use
void MoveToFrontByKey(LRUCache *cache, uint64_t key) {
    uint64_t page_idx = key / ENTRY_PER_PAGE;
    uint64_t index = hash(page_idx, cache->capacity);

    Node *cur = cache->hashTable[index];
    while (cur != NULL) {
        if (cur->page_idx == page_idx) {
            moveToFront(cache, cur);
            break;
        }
        cur = cur->hash_next;
    }

    return ;
}

void removeByKey(LRUCache *cache, uint64_t key) {
    uint64_t page_idx = key;
    uint64_t index = hash(page_idx, cache->capacity);

    Node *pre = NULL;
    Node *cur = cache->hashTable[index];

    // first: update hash[key]_list
    while (cur != NULL) {
        if (cur->page_idx == page_idx) {
            break;
        }
        pre = cur;
        cur = cur->hash_next;
    }
    if (cur == NULL) {
        // node with key no exist in cache
        femu_log("remov key not exist\n");
        return ;
    }
    if (pre == NULL) {
        // has_head node == page_idx
        cache->hashTable[index] = cache->hashTable[index]->hash_next;
    }else {
        pre->hash_next = cur->hash_next;
    }

    // second: update LRU node list (two-dir)
    if (cache->head == cur) {
        cache->head = cur->next;
    }
    if (cache->tail == cur) {
        cache->tail = cur->pre;
    }
    if (cur->pre != NULL) {
        cur->pre->next = cur->next;
    }
    if (cur->next != NULL) {
        cur->next->pre = cur->pre;
    }

    free(cur->l2p_entries);
    free(cur);

    cache->count--;
}

// 新的Node-> page_idx 
uint64_t addNodeToCache(DFTLTable *d_maptbl, LRUCache *cache, LRUCache *nand_cache, Node *node, bool *evict_happen, uint64_t *evict_key ,uint64_t* lat, struct ssd *ssd) {
    if (cache->count >= cache->capacity) {
        *evict_happen = true;
        *evict_key = dftl_evict(d_maptbl, cache, nand_cache, lat, ssd);
    }

    uint64_t index = hash(node->page_idx, cache->capacity);
    // 头插法到hastbl[i]中
    Node *cur = cache->hashTable[index];
    node->hash_next = cur;
    cache->hashTable[index] = node;

    // 将新插入的node 放到LRU_list 的top
    addToFront(cache, node);
    cache->count++;

    // update_map
    uint64_t bmp_idx = node->page_idx / 8;
    uint8_t  bit_idx  = node->page_idx % 8;
    map_set_bit(&(cache->flash_bmp[bmp_idx]), bit_idx);
    
    return node->page_idx;
}

// before add 
// same node.page_idx remove
uint64_t addNodeToNandCache(LRUCache *nand_cache, Node *node) {
    uint64_t index = hash(node->page_idx, nand_cache->capacity); // index = node->page_idx;
   // removeByKey(nand_cache, index);
    Node *cur = nand_cache->hashTable[index];
    node->hash_next = cur;
    nand_cache->hashTable[index] = node;

    addToFront(nand_cache, node);
    nand_cache->count++;

    // update_map
    uint64_t bmp_idx = node->page_idx / 8;
    uint8_t  bit_idx  = node->page_idx % 8;
    map_set_bit(&(nand_cache->flash_bmp[bmp_idx]), bit_idx);

    return node->page_idx;
}

uint64_t lru_get(DFTLTable *d_maptbl, LRUCache *cache, uint64_t key, struct ssd* ssd) {
    uint64_t page_idx = key / ENTRY_PER_PAGE;
    uint64_t index = hash(page_idx, cache->capacity);

    Node *cur = cache->hashTable[index];
    while (cur != NULL) {
        if (cur->page_idx == page_idx) {
            moveToFront(cache, cur);
            return cur->l2p_entries[key % ENTRY_PER_PAGE];  // ppa = l2p[oft]
        }
        cur = cur->hash_next;
    }

    // lpn还未写入 读缺失
    return UNMAPPED_PPA;
}

void lru_put(DFTLTable *d_maptbl, LRUCache *cache,LRUCache *nand_cache,uint64_t key, uint64_t value, struct ssd* ssd, uint64_t *lat) {
    uint64_t page_idx = key / ENTRY_PER_PAGE;
    uint64_t hash_index = hash(page_idx, cache->capacity);

  //  femu_log("[lru_put] lpa: %lu-> ppa: %lu, page_idx: %lu, cache_size: %lu\n", key, value, page_idx, cache->capacity);

    Node *cur = cache->hashTable[hash_index];
    while (cur != NULL) {
        if (cur->page_idx == page_idx) {
            // this node([page_idx][l2p_0~1024]) upadte, should be upadte in flash
            cur->l2p_entries[key % ENTRY_PER_PAGE] = value;
            cur->update_in_flash = true;
            moveToFront(cache, cur);
            return ;
        }
        cur = cur->hash_next;
    }

    uint64_t bmp_idx = page_idx / 8;
    uint8_t  bit_idx = page_idx % 8;

    // if (map_check_bit(&(d_maptbl->nand_cache->flash_bmp[bmp_idx]), bit_idx)) {
    //     Node *old_node = nand_cache->hashTable[has_index];
    //     Node *pre_ = NULL;

    //     while (old_node != NULL) {
    //         if (old_node->page_idx == page_idx) {
    //             break;
    //         }
    //         pre_ = old_node;
    //         old_node = old_node->hash_next;
    //     }
    //     if (old_node == NULL) {
    //         femu_log("LRU_put error: %lu\n", page_idx);
    //         return ;
    //     }
    //     old_node->l2p_entries[key % ENTRY_PER_PAGE] = value;
    //     old_node->update_in_flash = true;

    //     bool evict_happen = false;
    //     uint64_t evict_key = UNMAPPED_PPA;
    //     addNodeToCache(d_maptbl, cache, nand_cache, old_node, &evict_happen, &evict_key, lat, ssd);

    //     // read_trans_tbl lat
    //     uint64_t ppn;
    //     ppn = get_gtd_ent(d_maptbl, page_idx);
    //     if (ppn == UNMAPPED_PPA) {
    //         ftl_err("[lru_put_trs_read] vpn: %lu,in nand_cache, but not in gtd\n", page_idx);
    //         return 0;
    //     }
    //     struct ppa ppa;
    //     ppa = pgidx2ppa(ssd, ppn);
    //     struct nand_cmd trd;
    //     trd.type = USER_IO;
    //     trd.cmd  = NAND_READ;
    //     trd.stime = 0;
    //     ssd_advance_status(ssd, &ppa, &trd);


    // }
    // else {
        // this page_idx has not been created (New)
        Node *new_node = createNode(ssd, page_idx);
        new_node->l2p_entries[key % ENTRY_PER_PAGE] = value;
        bool evict_happen = false;
        uint64_t evict_key = UNMAPPED_PPA;

        addNodeToCache(d_maptbl, cache, nand_cache, new_node, &evict_happen, &evict_key, lat, ssd);
    // }
    return ;
}

void nand_cache_update(LRUCache *nand_cache,uint64_t key, uint64_t value) {
    uint64_t page_idx = key / ENTRY_PER_PAGE;
    uint64_t hash_index = hash(page_idx, nand_cache->capacity);


    Node *cur = nand_cache->hashTable[hash_index];
    while (cur != NULL) {
        if (cur->page_idx == page_idx) {
            // this node([page_idx][l2p_0~1024]) upadte, should be upadte in flash
            cur->l2p_entries[key % ENTRY_PER_PAGE] = value;
            cur->update_in_flash = true;
            return ;
        }
        cur = cur->hash_next;
    }
    if (cur == NULL) {
        femu_log("[GC_clean][nand_cache update] no_find key: %lu\n", key);
    }

    return ;
}

/**
 * @brief dftl_evict
 * 
 * pop()  use lru strategy: least recently use node in cache
 * 
 */

uint64_t dftl_evict(DFTLTable *d_maptbl, LRUCache *cache, LRUCache* nand_cache, uint64_t *lat, struct ssd* ssd) {
    if (cache->count >= cache->capacity) {
        Node *lastNode = cache->tail;
        if (lastNode) {
            uint64_t has_index = hash(lastNode->page_idx, cache->capacity);
            Node *cur = cache->hashTable[has_index];
            Node *pre = NULL;

            while (cur != NULL && cur != lastNode) {
                pre = cur;
                cur = cur->hash_next;
            }
            if (cur == lastNode) {
                // hash_list one-dir
                if (pre) {
                    pre->hash_next = cur->hash_next;
                }else {
                    cache->hashTable[has_index] = cur->hash_next;
                }
                // Lru_node_list two-dir
                if (lastNode->pre) {
                    lastNode->pre->next = lastNode->next;
                }
                if (lastNode->next) {
                    lastNode->next->pre = lastNode->pre;
                }
                cache->tail = lastNode->pre;

                uint64_t page_idx = lastNode->page_idx;      
                
                // 将CMT中dirty的Node flush to nand, update gtd
                if (lastNode->update_in_flash) {
                    d_maptbl->counter.evit_cnt++;

                    // flush策略, 先读后写
                    
                    int cnt = 0;
                    for (int i = 0; i < ENTRY_PER_PAGE; i++) {
                        if (lastNode->l2p_entries[i] != UNMAPPED_PPA) {
                            cnt++;
                        } 
                    }
                    femu_debug("[dftl_evict]: evict happen, 刷入闪存页号: %lu, valid_cnt: %d\n", lastNode->page_idx, cnt);
                    // update gtd & write node_page to flash & simulat lat
                    uint64_t wr_lat = 0;
                    wr_lat = translation_page_write(ssd, page_idx);
                    (*lat) += wr_lat;
                    
                    addNodeToNandCache(nand_cache, lastNode); 
                    
                }
                // upate CMT bitmap
                uint64_t bmp_idx = lastNode->page_idx / 8;
                uint8_t  bit_idx  = lastNode->page_idx % 8;
                map_clear_bit(&(cache->flash_bmp[bmp_idx]), bit_idx);

                cache->count--;
                
                return page_idx;
            }
        }
    }
    femu_err("EVICT FAILED\n");
    return UNMAPPED_PPA;
}


// 从 flash移出node应该保留 读映射表该页的lun的延迟模拟
uint64_t move_node_from_nand_to_cache(DFTLTable *d_maptbl, LRUCache *cache, LRUCache *nand_cache, uint64_t key, uint64_t* lat, NvmeRequest *req, struct nand_lun **trans_lun, struct ssd *ssd) {
    uint64_t page_idx = key / ENTRY_PER_PAGE;
    uint64_t has_index = hash(page_idx, nand_cache->capacity);
    
    Node *cur = nand_cache->hashTable[has_index];
    Node *pre = NULL;

    while (cur != NULL) {
        if (cur->page_idx == page_idx) {

            break;
        }
        pre = cur;
        cur = cur->hash_next;
    }
    if (cur == NULL) {
        return UNMAPPED_PPA;
    }


    uint64_t evict_key = UNMAPPED_PPA;
    bool evict_happen = false;
   // (*lat) += NAND_READ_LATENCY;   
    cur->update_in_flash = false; 

    uint64_t trans_lat = translation_page_read(ssd, page_idx, req, trans_lun);

    (*lat) += trans_lat;
    addNodeToCache(d_maptbl, cache, nand_cache, cur, &evict_happen, &evict_key, lat, ssd);

    // uint64_t bmp_idx = cur->page_idx / 8;
    // uint8_t  bit_idx = cur->page_idx % 8;
    // dftl_node_map_clear_bit(&(cache->flash_bmp[bmp_idx]), bit_idx);

    //nand_cache->count--;

    return cur->page_idx; 
}

void gtd_entry_init(G_map_entry *gtd) {
    gtd->ppn = UNMAPPED_PPA;
}

uint64_t get_gtd_ent(DFTLTable *d_tbl, uint64_t vpn) {
    return d_tbl->GTD[vpn].ppn;
}

void set_gtd_ent(DFTLTable *d_tbl, uint64_t vpn, uint64_t ppn) {
    d_tbl->GTD[vpn].ppn = ppn;
}

void node_entry_init(Node_entry *node_entry) {
    node_entry->lun_end_time = 0;
    node_entry->rely_lun = NULL;

    //QTAILQ_INIT(&node_entry->rely_req_list);
    node_entry->num_req = 0;

    node_entry->tag = 1;
}

DFTLTable* dftl_table_init(uint32_t tt_pages) {
    DFTLTable *table = (DFTLTable *)malloc(sizeof(DFTLTable));
    assert(table != NULL);

    uint64_t total_vpn = (tt_pages + ENTRY_PER_PAGE - 1) / ENTRY_PER_PAGE;
    uint64_t bmp_cnt = (total_vpn + 7) / 8;  // tt_pages / 1024 / 8
    
    table->CMT = createLRUCache(total_vpn * CMT_ratio, bmp_cnt);

    table->GTD = (G_map_entry *)malloc(sizeof(G_map_entry) * total_vpn);
    for (uint64_t i = 0; i < total_vpn; i++) {
        gtd_entry_init(&table->GTD[i]);
    }

    table->nodeTag = (Node_entry *)malloc(sizeof(Node_entry) * total_vpn);
    for (uint64_t i = 0; i < total_vpn; i++) {
        table->nodeTag[i].node_idx = i;
        node_entry_init(&table->nodeTag[i]);
    }


    table->nand_cache = createLRUCache(total_vpn, bmp_cnt);

    table->counter.group_write_cnt   = 0;
    table->counter.group_read_cnt    = 0;
    table->counter.group_read_miss   = 0;
    table->counter.group_cmt_hit     = 0;
    table->counter.group_cmt_miss    = 0;
    table->counter.evit_cnt          = 0;
    table->counter.gc_data_cnt       = 0;
    table->counter.gc_trans_cnt       = 0;

    table->counter.req_dely          = 0;
    table->counter.req_nodely        = 0;
    
    table->counter.has_tbl = g_malloc0(sizeof(uint64_t) * tt_pages);
    for (int i = 0; i < tt_pages; i++) {
        table->counter.has_tbl[i] = 0;
    }
    table->counter.write_maxLBA = 0;
    table->counter.write_minLBA = 16777216; // 64GB

    return table;
}

void dftl_table_clear(DFTLTable *table) {
    if (table == NULL) return ;
    deleteLRUCache(table->CMT);
    deleteLRUCache(table->nand_cache);

    free(table->GTD);
    free(table->counter.has_tbl);
    free(table->nodeTag);
    
    free(table);
}

void dftl_check(DFTLTable *table, uint64_t lpn ,uint64_t* lat, struct ssd* ssd, NvmeRequest *req, struct nand_lun **trans_lun) {
    uint64_t page_idx = lpn / ENTRY_PER_PAGE;
    uint64_t bmp_idx = page_idx / 8;
    uint8_t  bit_idx = page_idx % 8;

    // 要读的页不在CMT上 此时需要将该页映射表读出来 
    //！同时进行延迟模拟推进，以及后续同读该页的请求[缓存]
    if (!map_check_bit(&(table->CMT->flash_bmp[bmp_idx]), bit_idx)) {
        uint64_t evit_page_idx = move_node_from_nand_to_cache(table, table->CMT, table->nand_cache, lpn, lat, req, trans_lun, ssd);
        if (evit_page_idx == UNMAPPED_PPA) {
            return ;
        }

        struct nand_lun *tmp_lun = *trans_lun;
        table->nodeTag[page_idx].rely_lun = tmp_lun;
        table->nodeTag[page_idx].lun_end_time = tmp_lun->next_lun_avail_time;

        req->stag = ReqCache;   // 延迟
        req->expire_time_start = (req->expire_time_start < tmp_lun->next_lun_avail_time) ? \
                                 tmp_lun->next_lun_avail_time : req->expire_time_start;

        // QTAILQ_INSERT_TAIL(&table->nodeTag[page_idx].rely_req_list, req, entry);
        table->nodeTag[page_idx].num_req++;
        table->nodeTag[page_idx].tag = 2;  

        table->counter.group_cmt_miss++;
        table->counter.req_dely++;
    } else if (table->nodeTag[page_idx].tag == 2 && req->stime < table->nodeTag[page_idx].lun_end_time) {
        req->stag = ReqCache;
        req->expire_time_start = (req->expire_time_start < table->nodeTag[page_idx].lun_end_time) ? \
                                 table->nodeTag[page_idx].lun_end_time : req->expire_time_start;

        // QTAILQ_INSERT_TAIL(&table->nodeTag[page_idx].rely_req_list, req, entry);
        table->nodeTag[page_idx].num_req++;

        table->counter.group_cmt_miss++;
        table->counter.req_dely++;
    } else {
        table->counter.req_nodely++;
    }

    return ;
}

void dftl_read(DFTLTable *table, uint64_t lpn ,uint64_t* lat, struct ssd* ssd, NvmeRequest *req) { 
    uint64_t page_idx = lpn / ENTRY_PER_PAGE;

    //CMT LRU
    LRUCache *cache = ssd->d_maptbl->CMT;
    uint64_t index = hash(page_idx, cache->capacity);
    Node *cur = cache->hashTable[index];
    while (cur != NULL) {
        if (cur->page_idx == page_idx) {
            moveToFront(cache, cur);
            break;
            // return cur->l2p_entries[key % ENTRY_PER_PAGE];  // ppa = l2p[oft]
        }
        cur = cur->hash_next;
    }
    // (ret_ppn = cur->l2p_entries[key % ENTRY_PER_PAGE]) != ppa   //脏读

    //更新CMT中Node的信息 
    ssd->d_maptbl->nodeTag[page_idx].tag = 1;

    return ;
}


uint64_t dftl_get(DFTLTable *table, uint64_t lpn ,uint64_t* lat, struct ssd* ssd, NvmeRequest *req, struct nand_lun **trans_lun) {
    uint64_t page_idx = lpn / ENTRY_PER_PAGE;
    uint64_t bmp_idx = page_idx / 8;
    uint8_t  bit_idx = page_idx % 8;
    uint64_t ppa = UNMAPPED_PPA;

    // 要读的页不在CMT上 此时需要将该页映射表读出来 
    //！同时进行延迟模拟推进，以及后续同读该页的请求[缓存]
    if (!map_check_bit(&(table->CMT->flash_bmp[bmp_idx]), bit_idx)) {
        uint64_t evit_page_idx = move_node_from_nand_to_cache(table, table->CMT, table->nand_cache, lpn, lat, req, trans_lun, ssd);
        if (evit_page_idx == UNMAPPED_PPA) {
            return evit_page_idx;
        }
    }    
    else {
        ppa = lru_get(table, table->CMT, lpn, ssd);
        if (ppa != UNMAPPED_PPA) {
            table->counter.group_cmt_hit++;
        }
        // 一个问题 (1k读粒度导致) (读放大)
        // 如果此时ppa == UNMAP说明 读的该lpa所在页号page_idx是存在，但是对应的lpa不存在(访问不存在的lpa)，粗粒度读缺失，造成多余的闪存访问延迟

    }
    return ppa;
}


void dftl_put(DFTLTable* table, uint64_t lpn, uint64_t ppn, struct ssd *ssd, uint64_t *lat) {
    lru_put(table, table->CMT, table->nand_cache, lpn, ppn, ssd, lat);
    return ;
}

/**
 * @brief ssd_method
 * 
 * 
 */


static inline bool should_gc(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    return ssd->maptbl[lpn];
}

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    ftl_assert(lpn < ssd->sp.tt_pgs);
    ssd->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    pgidx = ppa->g.ch  * spp->pgs_per_ch  + \
            ppa->g.lun * spp->pgs_per_lun + \
            ppa->g.pl  * spp->pgs_per_pl  + \
            ppa->g.blk * spp->pgs_per_blk + \
            ppa->g.pg;

    ftl_assert(pgidx < spp->tt_pgs);

    return pgidx;
}

static inline struct ppa pgidx2ppa(struct ssd *ssd, uint64_t pgidx)
{
    struct ssdparams *spp = &ssd->sp;
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = pgidx / spp->pgs_per_ch;
    ppa.g.lun = (pgidx - ppa.g.ch * spp->pgs_per_ch) / spp->pgs_per_lun;
    ppa.g.pl = (pgidx - ppa.g.ch * spp->pgs_per_ch - ppa.g.lun * spp->pgs_per_lun) / spp->pgs_per_pl;
    ppa.g.blk = (pgidx - ppa.g.ch * spp->pgs_per_ch - ppa.g.lun * spp->pgs_per_lun - 
                ppa.g.pl  * spp->pgs_per_pl) / spp->pgs_per_blk;
    ppa.g.pg = (pgidx - ppa.g.ch * spp->pgs_per_ch - ppa.g.lun * spp->pgs_per_lun - 
                ppa.g.pl  * spp->pgs_per_pl - ppa.g.blk * spp->pgs_per_blk);

    ftl_assert(pgidx < spp->tt_pgs);

    return ppa;
}

// superblcok,superpage 's encode
uint64_t ppa2vpgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t vpgidx;

    
    vpgidx = ppa->g.blk * spp->pgs_per_line  + \
             ppa->g.pg  * spp->blks_per_line + \
             ppa->g.pl  * spp->luns_per_line + \
             ppa->g.lun * spp->nchs          + \
             ppa->g.ch;

    ftl_assert(vpgidx < spp->tt_pgs);

    return vpgidx;
}

static inline struct ppa vpgidx2ppa(struct ssd *ssd, uint64_t vpgidx)
{
    struct ssdparams *spp = &ssd->sp;
    struct ppa ppa;
    ppa.ppa = 0;


    ppa.g.blk = vpgidx / spp->pgs_per_line;
    ppa.g.pg  = (vpgidx - ppa.g.blk * spp->pgs_per_line) / spp->blks_per_line;
    ppa.g.pl  = (vpgidx - ppa.g.blk * spp->pgs_per_line - ppa.g.pg * spp->blks_per_line) / spp->luns_per_line;
    ppa.g.lun = (vpgidx - ppa.g.blk * spp->pgs_per_line - ppa.g.pg * spp->blks_per_line -
                 ppa.g.pl * spp->luns_per_line) / spp->nchs;
    ppa.g.ch  = (vpgidx - ppa.g.blk * spp->pgs_per_line - ppa.g.pg * spp->blks_per_line -
                 ppa.g.pl * spp->luns_per_line - ppa.g.lun * spp->nchs);

    ftl_assert(vpgidx < spp->tt_pgs);

    return ppa;
}

static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    return ssd->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    ssd->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
}

static void ssd_init_lines(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;

    lm->tt_lines = spp->blks_per_pl;
    ftl_assert(lm->tt_lines == spp->tt_lines);
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

    QTAILQ_INIT(&lm->free_line_list);
    lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
            victim_line_get_pri, victim_line_set_pri,
            victim_line_get_pos, victim_line_set_pos);
    QTAILQ_INIT(&lm->full_line_list);

    lm->free_line_cnt = 0;
    for (int i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        line->pos = 0;
        line->type = DATA;
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    ftl_assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
}

static void ssd_init_write_pointer(struct ssd *ssd, int type)
{
    struct write_pointer *wpp;
    if (type == TRANS) {
        wpp = &ssd->t_wp;
    } else {
        wpp = &ssd->d_wp;
    }

    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    /* wpp->curline is always our next-to-write super-block */
    wpp->curline = curline;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    //wpp->blk = 0;
    wpp->blk = curline->id;
    wpp->pl = 0;

    if (type == TRANS) {
        wpp->curline->type = TRANS;
    } else {
        wpp->curline->type = DATA;
    }
}

static inline void check_addr(int a, int max)
{
    ftl_assert(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct ssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
        return NULL;
    }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    return curline;
}

void ssd_advance_write_pointer(struct ssd *ssd, int type)
{
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp;
    if (type == TRANS) {
        wpp = &ssd->t_wp;
    } else {
        wpp = &ssd->d_wp;
    }

    struct line_mgmt *lm = &ssd->lm;
    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    if (wpp->ch == spp->nchs) {
        wpp->ch = 0;
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        /* in this case, we should go to next lun */
        if (wpp->lun == spp->luns_per_ch) {
            wpp->lun = 0;
            /* go to next page in the block */
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            if (wpp->pg == spp->pgs_per_blk) {
                wpp->pg = 0;
                /* move current line to {victim,full} line list */
                // 超级块写满了
                if (wpp->curline->vpc == spp->pgs_per_line) {
                    /* all pgs are still valid, move to full line list */
                    ftl_assert(wpp->curline->ipc == 0);
                    QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
                    lm->full_line_cnt++; // 满了且全是有效页
                } else {
                    ftl_assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
                    /* there must be some invalid pages in this line */
                    ftl_assert(wpp->curline->ipc > 0);
                    // 将超级块放到gc victim优先队列中
                    pqueue_insert(lm->victim_line_pq, wpp->curline);
                    lm->victim_line_cnt++;
                }
                /* current line is used up, pick another empty line */
                check_addr(wpp->blk, spp->blks_per_pl);
                wpp->curline = NULL;
                wpp->curline = get_next_free_line(ssd);
                if (!wpp->curline) {
                    /* TODO */
                    femu_err("!wpp->curline\n");
                    abort();
                }
                if (type == TRANS) {
                    wpp->curline->type = TRANS;
                } else {
                    wpp->curline->type = DATA;
                }

                wpp->blk = wpp->curline->id;
                
                check_addr(wpp->blk, spp->blks_per_pl);
                /* make sure we are starting from page 0 in the super block */
                ftl_assert(wpp->pg == 0);
                ftl_assert(wpp->lun == 0);
                ftl_assert(wpp->ch == 0);
                /* TODO: assume # of pl_per_lun is 1, fix later */
                ftl_assert(wpp->pl == 0);
            }
        }
    }
}

struct ppa get_new_page(struct ssd *ssd, int type)
{
    struct write_pointer *wpp;
    if (type == TRANS) {
        wpp = &ssd->t_wp;
    } else {
        wpp = &ssd->d_wp;
    }
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    ftl_assert(ppa.g.pl == 0);

    return ppa;
}

static void check_params(struct ssdparams *spp)
{
    /*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

    //ftl_assert(is_power_of_2(spp->luns_per_ch));
    //ftl_assert(is_power_of_2(spp->nchs));
}

static void ssd_init_params(struct ssdparams *spp)
{
    spp->secsz = 512;
    spp->secs_per_pg = 8;
    spp->pgs_per_blk = 512; //*2 = 32GB
    spp->blks_per_pl = 256; /*1 = 16GB */
    spp->pls_per_lun = 1;
    spp->luns_per_ch = 8;
    spp->nchs = 8;

    spp->pg_rd_lat = NAND_READ_LATENCY;
    spp->pg_wr_lat = NAND_PROG_LATENCY;
    spp->blk_er_lat = NAND_ERASE_LATENCY;
    spp->ch_xfer_lat = 0;

    /* calculated values */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;  // 16GB  8 * 8 = 64

    /* line is special, put it at the end */
    spp->blks_per_line = spp->tt_luns * 1; /* TODO: to fix under multiplanes */ // no多通道 plane = 1
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; /* TODO: to fix under multiplanes */

    // super_block == line
    // 32 GB
    spp->luns_per_line = spp->nchs * spp->luns_per_ch;          // 64
    spp->pls_per_line = spp->luns_per_line * spp->pls_per_lun;  // 64 
    spp->blks_per_line = spp->pls_per_line;                     // 64 (blks_per_line = pages_per_superpagd) 

    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;  // 64 * 512 = 32768 pages


    spp->gc_thres_pcent = 0.75;
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_pcent_high = 0.95;
    spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
    spp->enable_gc_delay = true;


    check_params(spp);
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
    pg->nsecs = spp->secs_per_pg;
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    for (int i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
    blk->npgs = spp->pgs_per_blk;
    blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
    for (int i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    for (int i = 0; i < pl->nblks; i++) {
        ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    lun->npls = spp->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    for (int i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    for (int i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

static void ssd_init_maptbl(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }

}

static void ssd_init_rmap(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

void ssd_init(FemuCtrl *n)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;

    ftl_assert(ssd);


    ssd_init_params(spp);
    
    
    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (int i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    }

    /* initialize maptbl */
    ssd_init_maptbl(ssd);

    /* initialize rmap */
    ssd_init_rmap(ssd);


    ssd->d_maptbl = dftl_table_init(ssd->sp.tt_pgs);

    /* initialize all the lines */
    ssd_init_lines(ssd);

    /* initialize write pointer, this is how we allocate new pages for writes */
    // two wps: data_wp, trans_wp;
    ssd_init_write_pointer(ssd, DATA);
    ssd_init_write_pointer(ssd, TRANS);

    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
                       QEMU_THREAD_JOINABLE);
}

static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >=
        0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg
        >= 0 && pg < spp->pgs_per_blk && sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_lun *lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_plane *pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->lm.lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct
        nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lun = get_lun(ssd, ppa);
    uint64_t lat = 0;

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
#if 0
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
#endif
        break;

    case NAND_WRITE:
        /* write: transfer data through channel first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        } else {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }
        lat = lun->next_lun_avail_time - cmd_stime;

#if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                     ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
#endif
        break;

    case NAND_ERASE:
        /* erase: only need to advance NAND status */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        break;

    default:
        ftl_err("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;

    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(ssd, ppa);

    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    if (line->vpc == spp->pgs_per_line) {
        ftl_assert(line->ipc == 0);
        was_full_line = true;
    }
    line->ipc++;
    ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    /* Adjust the position of the victime line in the pq under over-writes */
    if (line->pos) {
        /* Note that line->vpc will be updated by this call */
        pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
    } else {
        line->vpc--;
    }

    if (was_full_line) {
        /* move line: "full" -> "victim" */
        QTAILQ_REMOVE(&lm->full_line_list, line, entry);
        lm->full_line_cnt--;
        pqueue_insert(lm->victim_line_pq, line);
        lm->victim_line_cnt++;
    }
}

static void mark_page_valid(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;
}

static void mark_block_free(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);
    struct nand_page *pg = NULL;

    for (int i = 0; i < spp->pgs_per_blk; i++) {
        /* reset page status */
        pg = &blk->pg[i];
        ftl_assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* reset block status */
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
}

/*
* @brief translation_page method (Normal & GC)
*
*/
static uint64_t translation_page_read(struct ssd *ssd, uint64_t vpn, NvmeRequest *req, struct nand_lun **trans_lun) {
    struct ppa ppa;
    uint64_t ppn;
    
    ppn = get_gtd_ent(ssd->d_maptbl, vpn);
    if (ppn == UNMAPPED_PPA) {
        femu_err("[trans_read] vpn: %lu,in nand_cache, but not in gtd\n", vpn);
        return 0;
    }
    ppa = pgidx2ppa(ssd, ppn);
    uint64_t lat = 0;
    struct nand_cmd trd;
    trd.type = USER_IO;
    trd.cmd  = NAND_READ;
    trd.stime = req->stime;
    lat = ssd_advance_status(ssd, &ppa, &trd);

    *trans_lun = get_lun(ssd, &ppa);
    // femu_log("[tr_pages_read] vpn: %lu(lpn:%lu) -> ppn: %lu(lun.id: %lu), trans_lun->time: %lu\n", 
    //         vpn, vpn*ENTRY_PER_PAGE, ppn, (ppa.g.ch * ssd->sp.luns_per_ch + ppa.g.lun), (*trans_lun)->next_lun_avail_time);
    return lat;
}

static uint64_t translation_page_write(struct ssd *ssd, uint64_t vpn) {
    struct ppa new_ppa;
    struct ppa old_ppa;
    uint64_t old_ppn, new_ppn;
    uint64_t lat = 0;

    old_ppn = get_gtd_ent(ssd->d_maptbl, vpn);
    if (old_ppn != UNMAPPED_PPA) {
        old_ppa = pgidx2ppa(ssd, old_ppn);
        mark_page_invalid(ssd, &old_ppa);
        set_rmap_ent(ssd, INVALID_LPN, &old_ppa);
    }
    new_ppa = get_new_page(ssd, TRANS);
    new_ppn = ppa2pgidx(ssd, &new_ppa);
    set_gtd_ent(ssd->d_maptbl, vpn, new_ppn);
    set_rmap_ent(ssd, vpn, &new_ppa);
    mark_page_valid(ssd, &new_ppa);
    ssd_advance_write_pointer(ssd, TRANS);

    struct nand_cmd twr;
    twr.type = USER_IO;
    twr.cmd = NAND_WRITE;
    twr.stime = 0;
    lat = ssd_advance_status(ssd, &new_ppa, &twr);

    return lat;
}


static inline uint64_t gc_translation_page_read(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t lat = 0;
    struct nand_cmd trd;
    trd.type = GC_IO;
    trd.cmd = NAND_READ;
    trd.stime = 0;
    lat = ssd_advance_status(ssd, ppa, &trd);
    
    return lat;
}

static uint64_t gc_translation_page_write(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t vpn = get_rmap_ent(ssd, old_ppa);
    uint64_t lat = 0;

    if (mapped_ppa(old_ppa)) {
        /* update old page information first */
        mark_page_invalid(ssd, old_ppa);
        set_rmap_ent(ssd, INVALID_LPN, old_ppa);
    }
    new_ppa = get_new_page(ssd, TRANS);


    /* update maptbl */
    uint64_t new_ppn;
    new_ppn = ppa2pgidx(ssd, &new_ppa);
    set_gtd_ent(ssd->d_maptbl, vpn, new_ppn);
    /* update rmap */
    set_rmap_ent(ssd, vpn, &new_ppa);

    mark_page_valid(ssd, &new_ppa);
    ssd_advance_write_pointer(ssd, TRANS);
    /* need to advance the write pointer here */


    struct nand_cmd twr;
    twr.type = GC_IO;
    twr.cmd = NAND_WRITE;
    twr.stime = 0;
    lat = ssd_advance_status(ssd, &new_ppa, &twr);

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return lat;
}



static void gc_read_page(struct ssd *ssd, struct ppa *ppa)
{
    /* advance ssd status, we don't care about how long it takes */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        ssd_advance_status(ssd, ppa, &gcr);
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);

    ftl_assert(valid_lpn(ssd, lpn));
    new_ppa = get_new_page(ssd, DATA);


    /* update maptbl */
    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &new_ppa);


    mark_page_valid(ssd, &new_ppa);
    ssd_advance_write_pointer(ssd, DATA);

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
}


static struct line *select_victim_line(struct ssd *ssd, bool force)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = NULL;

    victim_line = pqueue_peek(lm->victim_line_pq);
    if (!victim_line) {
        return NULL;
    }

    if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) {
        return NULL;
    }

    pqueue_pop(lm->victim_line_pq);
    victim_line->pos = 0;
    lm->victim_line_cnt--;

    /* victim_line is a danggling node now */
    return victim_line;
}

/* here ppa identifies the block we want to clean */
static void clean_one_data_block(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;

    uint64_t vpn2update[spp->pgs_per_blk];
    int nums_vpn = 0, flag;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);

        if (pg_iter->status == PG_VALID) {
            uint64_t old_lpn, new_ppn, old_vpn;

            gc_read_page(ssd, ppa);
            /* delay the maptbl update until "write" happens */
            old_lpn = get_rmap_ent(ssd, ppa);

            gc_write_page(ssd, ppa);

            new_ppn = ppa2pgidx(ssd, &ssd->maptbl[old_lpn]);

            nand_cache_update(ssd->d_maptbl->nand_cache, old_lpn, new_ppn);
            
            old_vpn = old_lpn / ENTRY_PER_PAGE;
            flag = 0;
            for (int i = 0; i < nums_vpn; i++) {
                if (vpn2update[i] == old_vpn) {
                    flag = 1;
                    break;
                }
            }
            if (!flag) {
                vpn2update[nums_vpn++] = old_vpn;
                uint64_t trans_ppn = get_gtd_ent(ssd->d_maptbl, old_vpn);
                if (trans_ppn == UNMAPPED_PPA)
                    break;
                struct ppa trans_ppa = pgidx2ppa(ssd, trans_ppn);
                gc_translation_page_read(ssd, &trans_ppa);
                gc_translation_page_write(ssd, &trans_ppa);
            }

            cnt++;
        }
    }

    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
}

static void clean_one_trans_block(struct ssd *ssd, struct ppa *ppa){
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_translation_page_read(ssd, ppa);
            gc_translation_page_write(ssd, ppa);

            cnt++;
        }
    }

    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);    
}

static void mark_line_free(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = get_line(ssd, ppa);
    line->ipc = 0;
    line->vpc = 0;
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
}

static int do_gc(struct ssd *ssd, bool force)
{
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;

    victim_line = select_victim_line(ssd, force);


    if (!victim_line) {
       // femu_log("no victime_line\n");
        return -1;
    }

    ppa.g.blk = victim_line->id;

    ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
              victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
              ssd->lm.free_line_cnt);

    /* copy back valid data */
    // superblok level clean block
    for (ch = 0; ch < spp->nchs; ch++) {
        for (lun = 0; lun < spp->luns_per_ch; lun++) {
            ppa.g.ch = ch;
            ppa.g.lun = lun;
            ppa.g.pl = 0;
            lunp = get_lun(ssd, &ppa);
            if (victim_line->type == DATA) {
                ssd->d_maptbl->counter.gc_data_cnt++;
                clean_one_data_block(ssd, &ppa);
            } else {
                ssd->d_maptbl->counter.gc_trans_cnt++;
                clean_one_trans_block(ssd, &ppa);
            }
            mark_block_free(ssd, &ppa);

            if (spp->enable_gc_delay) {
                struct nand_cmd gce;
                gce.type = GC_IO;
                gce.cmd = NAND_ERASE;
                gce.stime = 0;
                ssd_advance_status(ssd, &ppa, &gce);
            }

            lunp->gc_endtime = lunp->next_lun_avail_time;
        }
    }

    /* update line status */
    mark_line_free(ssd, &ppa);
    return 0;
}



static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req, FemuCtrl *n)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat = 0, maxlat = 0;

    uint64_t ret_ppn;
    uint64_t maptbl_lat = 0;
    struct nand_lun *trans_lun = NULL;
 
    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }
    
    // resend check
    if (req->stag == ReqResend) {
        for (lpn = start_lpn; lpn <= end_lpn; lpn += 1) {
            dftl_read(ssd->d_maptbl, lpn, &maptbl_lat, ssd, req);
            // uint64_t ed_lpn = ((lpn + ENTRY_PER_PAGE) > end_lpn) ? end_lpn : (lpn + ENTRY_PER_PAGE);
            ssd->d_maptbl->counter.group_read_cnt++;
            ppa = get_maptbl_ent(ssd, lpn);
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                ssd->d_maptbl->counter.group_read_miss++;
                continue;
            }
            struct nand_cmd srd;
            srd.type = USER_IO;
            srd.cmd = NAND_READ;
            srd.stime = req->expire_time_start;
            sublat = ssd_advance_status(ssd, &ppa, &srd) + srd.stime - req->stime;
            maxlat = (sublat > maxlat) ? sublat : maxlat;    
        }      
        goto ed;       
    }

    // check
    else if (req->stag == ReqInitial) {
        for (lpn = start_lpn; lpn <= end_lpn; lpn += ENTRY_PER_PAGE) {
            dftl_check(ssd->d_maptbl, lpn, &maptbl_lat, ssd, req, &trans_lun);
            femu_debug("[dftl_check_out] req_id: %lu, req_stag: %d, req_slpn: %lu, req_ex_start: %lu\n", 
                    req->id, req->stag, start_lpn, req->expire_time_start);
        }
        if (req->stag == ReqCache) {
            goto ed;
        }
    } 

    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ssd->d_maptbl->counter.group_read_cnt++;
        ret_ppn = dftl_get(ssd->d_maptbl, lpn, &maptbl_lat, ssd, req, &trans_lun);
        if (ret_ppn == UNMAPPED_PPA) {
            ssd->d_maptbl->counter.group_read_miss++;
            continue;
        }
        else if (!valid_lpn(ssd, ret_ppn)) {
            femu_log("Invalid ppA: %lu\n", ret_ppn);
            continue;
        }
        assert(ret_ppn != UNMAPPED_PPA);
        ppa = pgidx2ppa(ssd, ret_ppn);

        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        sublat = ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

ed:
    return maxlat;
}

static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0, w_translat = 0;
    int r;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    while (should_gc_high(ssd)) {  //
        /* perform GC here until !should_gc(ssd) */ // front
        r = do_gc(ssd, true);
        if (r == -1)
            break;
    }


    // static
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        if (ssd->d_maptbl->counter.has_tbl[lpn] == 0) {
            ssd->d_maptbl->counter.has_tbl[lpn] = 1;
            ssd->d_maptbl->counter.group_write_cnt++;
            if (ssd->d_maptbl->counter.write_maxLBA < lpn) ssd->d_maptbl->counter.write_maxLBA = lpn;
            if (ssd->d_maptbl->counter.write_minLBA > lpn) ssd->d_maptbl->counter.write_minLBA = lpn;
        }

        w_translat = 0;
        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            mark_page_invalid(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }
        
        /* new write */
        ppa = get_new_page(ssd, DATA);
        uint64_t ppn = ppa2pgidx(ssd, &ppa);
        dftl_put(ssd->d_maptbl, lpn, ppn, ssd, &w_translat);
        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);
        mark_page_valid(ssd, &ppa);
        ssd_advance_write_pointer(ssd, DATA);


        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        curlat = (curlat > w_translat) ? curlat : w_translat;
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return maxlat;
}

__attribute__((optimize("O0")))
static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = n->ssd;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int rc;
    int i;
    
    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    }

    /* FIXME: not safe, to handle ->to_ftl and ->to_poller gracefully */
    ssd->to_ftl = n->to_ftl;
    ssd->to_poller = n->to_poller;

    while (1) {
        for (i = 1; i <= n->nr_pollers; i++) {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);


            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
            }


            ftl_assert(req);
            switch (req->cmd.opcode) {
            case NVME_CMD_WRITE:
                lat = ssd_write(ssd, req);
                break;
            case NVME_CMD_READ:
                lat = ssd_read(ssd, req, n);
                break;
            case NVME_CMD_DSM:
                lat = 0;
                break;
            default:
                ftl_err("FTL received unkown request type, ERROR\n");
            }

            req->reqlat = lat;
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }

            /* clean one line if needed (in the background) */
            if (should_gc(ssd)) {
                do_gc(ssd, false);
            }
        }
    }

    return NULL;
}

void dftl_static(DFTLTable *d_maptbl, FemuCtrl *n) {
    femu_log("[DFTL]write_cnt:%d, read_cnt: %d, read_miss: %d, cmt_hit: %d, cmt_miss: %d, evit_cnt: %d, gc_data_cnt: %d, gc_trans_cnt:%d\n", 
    d_maptbl->counter.group_write_cnt, d_maptbl->counter.group_read_cnt,
    d_maptbl->counter.group_read_miss, d_maptbl->counter.group_cmt_hit,
    d_maptbl->counter.group_cmt_miss, d_maptbl->counter.evit_cnt,
    d_maptbl->counter.gc_data_cnt, d_maptbl->counter.gc_trans_cnt);

    femu_log("[DFTL]cache_cnt: %lu/%lu, nand_cnt: %lu/%lu\n", d_maptbl->CMT->count, d_maptbl->CMT->capacity, 
    d_maptbl->nand_cache->count, d_maptbl->nand_cache->capacity);

    pqueue_t *pq = n->req_list;
    femu_log("[DFTL]req_delay: %d, req_nodely: %d, 暂存队列请求数: %lu\n", d_maptbl->counter.req_dely, 
            d_maptbl->counter.req_nodely, pqueue_size(pq));
}