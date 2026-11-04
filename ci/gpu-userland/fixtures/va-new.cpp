
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <random>
#include <set>
#include <vector>
#include "rb_tree.h"
static std::set<void*> live;
static std::set<unsigned> failures;
static unsigned allocation_calls=0, injected=0, rejected_overlap=0;
static void *va_calloc(size_t n,size_t size) {
 ++allocation_calls;
 if(failures.erase(allocation_calls)){++injected;return nullptr;}
 void *p=std::calloc(n,size);assert(p && live.insert(p).second);return p;
}
static void va_release(void *p){assert(live.erase(p)==1);std::free(p);}
template<typename... A> static void mesa_loge(const char*,A...){++rejected_overlap;}
#define calloc va_calloc
#define free va_release
struct darwin_va_hole {
   struct rb_node by_address, by_size;
   uint64_t start, size;
};
struct darwin_va {
   struct rb_tree address, size;
   bool seeded;
   uint64_t holes, allocs, frees, merges;
   uint64_t used;   /* bytes handed out */
};
static int
darwin_va_cmp_address(const struct rb_node *a, const struct rb_node *b)
{
   const struct darwin_va_hole *x = rb_node_data(struct darwin_va_hole, a, by_address);
   const struct darwin_va_hole *y = rb_node_data(struct darwin_va_hole, b, by_address);
   return x->start > y->start ? -1 : x->start < y->start ? 1 : 0;
}
static int
darwin_va_cmp_size(const struct rb_node *a, const struct rb_node *b)
{
   const struct darwin_va_hole *x = rb_node_data(struct darwin_va_hole, a, by_size);
   const struct darwin_va_hole *y = rb_node_data(struct darwin_va_hole, b, by_size);
   if (x->size != y->size)
      return x->size > y->size ? -1 : 1;
   return x->start > y->start ? -1 : x->start < y->start ? 1 : 0;
}
static struct darwin_va_hole *
darwin_va_smallest_fit(struct darwin_va *va, uint64_t size)
{
   struct rb_node *best = NULL;
   for (struct rb_node *x = va->size.root; x;) {
      if (rb_node_data(struct darwin_va_hole, x, by_size)->size >= size) {
         best = x;
         x = x->left;
      } else {
         x = x->right;
      }
   }
   return best ? rb_node_data(struct darwin_va_hole, best, by_size) : NULL;
}
static struct darwin_va_hole *
darwin_va_hole_at_or_below(struct darwin_va *va, uint64_t address)
{
   struct rb_node *best = NULL;
   for (struct rb_node *x = va->address.root; x;) {
      if (rb_node_data(struct darwin_va_hole, x, by_address)->start <= address) {
         best = x;
         x = x->right;
      } else {
         x = x->left;
      }
   }
   return best ? rb_node_data(struct darwin_va_hole, best, by_address) : NULL;
}
static bool
darwin_va_add(struct darwin_va *va, uint64_t start, uint64_t size)
{
   if (!size)
      return true;
   struct darwin_va_hole *hole = (struct darwin_va_hole *)calloc(1, sizeof(*hole));
   if (!hole)
      return false;   /* the range is lost, not reused: never handed out twice */
   hole->start = start;
   hole->size = size;
   rb_tree_insert(&va->address, &hole->by_address, darwin_va_cmp_address);
   rb_tree_insert(&va->size, &hole->by_size, darwin_va_cmp_size);
   ++va->holes;
   return true;
}
static void
darwin_va_del(struct darwin_va *va, struct darwin_va_hole *hole)
{
   rb_tree_remove(&va->address, &hole->by_address);
   rb_tree_remove(&va->size, &hole->by_size);
   --va->holes;
   free(hole);
}
static void
darwin_va_carve(struct darwin_va *va, struct darwin_va_hole *hole, uint64_t at, uint64_t size)
{
   const uint64_t begin = hole->start, end = hole->start + hole->size;
   darwin_va_del(va, hole);
   darwin_va_add(va, begin, at - begin);
   darwin_va_add(va, at + size, end - (at + size));
   ++va->allocs;
   va->used += size;
}
static uint64_t
darwin_va_alloc(struct darwin_va *va, uint64_t size, uint64_t alignment, bool high)
{
   if (!size || !alignment || (alignment & (alignment - 1)))
      return 0;
   struct darwin_va_hole *hole = darwin_va_smallest_fit(va, size);
   while (hole) {
      const uint64_t end = hole->start + hole->size;
      uint64_t at;
      bool fits;
      if (high) {
         at = (end - size) & ~(alignment - 1);
         fits = at >= hole->start;
      } else {
         at = (hole->start + alignment - 1) & ~(alignment - 1);
         fits = at >= hole->start && at <= end && end - at >= size;
      }
      if (fits) {
         darwin_va_carve(va, hole, at, size);
         return at;
      }
      struct rb_node *next = rb_node_next(&hole->by_size);
      hole = next ? rb_node_data(struct darwin_va_hole, next, by_size) : NULL;
   }
   return 0;
}
static bool
darwin_va_alloc_at(struct darwin_va *va, uint64_t address, uint64_t size)
{
   struct darwin_va_hole *hole = darwin_va_hole_at_or_below(va, address);
   if (!size || !hole || address - hole->start >= hole->size || hole->start + hole->size - address < size)
      return false;
   darwin_va_carve(va, hole, address, size);
   return true;
}
static void
darwin_va_free(struct darwin_va *va, uint64_t address, uint64_t size)
{
   if (!size)
      return;
   struct darwin_va_hole *below = darwin_va_hole_at_or_below(va, address);
   struct rb_node *after = below ? rb_node_next(&below->by_address) : rb_tree_first(&va->address);
   struct darwin_va_hole *above = after ? rb_node_data(struct darwin_va_hole, after, by_address) : NULL;
   if ((below && below->start + below->size > address) || (above && above->start < address + size)) {
      mesa_loge("darwin: GPU range %#llx+%#llx freed over a free hole", (unsigned long long)address,
                (unsigned long long)size);
      return;
   }
   uint64_t begin = address, end = address + size;
   if (below && below->start + below->size == address) {
      begin = below->start;
      darwin_va_del(va, below);
      ++va->merges;
   }
   if (above && above->start == end) {
      end = above->start + above->size;
      darwin_va_del(va, above);
      ++va->merges;
   }
   darwin_va_add(va, begin, end - begin);
   ++va->frees;
   va->used -= size;
}
static void
darwin_va_finish(struct darwin_va *va)
{
   /* rb_tree_foreach_safe caches only the successor. That successor can still
    * follow parent pointers through the node just freed. Unlink both indices
    * before releasing any hole, using the same removal as carve and merge. */
   while (struct rb_node *node = rb_tree_first(&va->address))
      darwin_va_del(va, rb_node_data(struct darwin_va_hole, node, by_address));
}


#undef calloc
#undef free
static constexpr uint64_t page=4096,base=0x20000000;
struct Range {uint64_t at,size;};
static unsigned verify_node(rb_node *n,rb_node *parent,std::set<rb_node*> &seen) {
 if(!n)return 1;
 assert(seen.insert(n).second && rb_node_parent(n)==parent);
 bool black=n->parent&1;
 if(!parent)assert(black);
 if(!black){assert(!n->left || (n->left->parent&1));assert(!n->right || (n->right->parent&1));}
 unsigned l=verify_node(n->left,n,seen),r=verify_node(n->right,n,seen);assert(l==r);
 return l+(black?1:0);
}
// State0 is free,1 allocated,2 irretrievably lost after metadata OOM.
static void check(darwin_va &va,std::vector<unsigned char> &state,bool allow_loss=false) {
 std::set<rb_node*> a,b;verify_node(va.address.root,nullptr,a);verify_node(va.size.root,nullptr,b);
 assert(a.size()==b.size() && a.size()==va.holes);
 std::set<darwin_va_hole*> ha,hb;
 std::vector<bool> covered(state.size(),false);
 uint64_t last=base;
 for(rb_node *n=rb_tree_first(&va.address);n;n=rb_node_next(n)) {
  auto *h=rb_node_data(darwin_va_hole,n,by_address);ha.insert(h);
  assert(h->size && !(h->start%page) && !(h->size%page) && h->start>=last);
  assert(h->start>=base && h->start+h->size<=base+state.size()*page);
  for(uint64_t p=(h->start-base)/page;p<(h->start+h->size-base)/page;p++){
   assert(!covered[p] && state[p]==0);covered[p]=true;
  }
  last=h->start+h->size;
 }
 uint64_t previous_size=0,previous_start=0;
 for(rb_node *n=rb_tree_first(&va.size);n;n=rb_node_next(n)) {
  auto *h=rb_node_data(darwin_va_hole,n,by_size);hb.insert(h);
  assert(h->size>previous_size || (h->size==previous_size && h->start>previous_start));
  previous_size=h->size;previous_start=h->start;
 }
 assert(ha==hb);
 uint64_t used=0;
 for(unsigned p=0;p<state.size();p++) {
  if(state[p]==1)used+=page;
  if(state[p]==0 && !covered[p]){assert(allow_loss);state[p]=2;}
  if(state[p]==2)assert(!covered[p]);
 }
 assert(used==va.used);
}
static void claim(std::vector<unsigned char> &state,uint64_t at,uint64_t bytes) {
 assert(at>=base && at+bytes<=base+state.size()*page);
 for(uint64_t p=(at-base)/page;p<(at+bytes-base)/page;p++){assert(state[p]==0);state[p]=1;}
}
static void release(std::vector<unsigned char> &state,Range r) {
 for(uint64_t p=(r.at-base)/page;p<(r.at+r.size-base)/page;p++){assert(state[p]==1);state[p]=0;}
}
static void finish(darwin_va &va) {
 darwin_va_finish(&va);
#if !TEST_OLD
 assert(!va.address.root && !va.size.root && !va.holes && live.empty());
 // A second finish is a no-op, including an initially empty tree.
 darwin_va_finish(&va);assert(live.empty());
#endif
}
static void fragmented(unsigned n) {
 darwin_va va{};
 for(unsigned i=0;i<n;i++)assert(darwin_va_add(&va,base+i*page*2,page));
 finish(va);
}
int main(void) {
 // In-order freeing of this real balanced tree used to follow a freed parent.
 fragmented(257);
#if TEST_OLD
 std::abort(); // A silent old pass is never accepted as a reproduced UAF.
#else
 for(unsigned n: {0u,1u,2u,3u,8u,31u,1024u})fragmented(n);
 for(unsigned mask=1;mask<4;mask++) {
  darwin_va va{};std::vector<unsigned char> state(16);
  assert(darwin_va_add(&va,base,state.size()*page));
  unsigned next=allocation_calls+1;
  if(mask&1)failures.insert(next);if(mask&2)failures.insert(next+1);
  assert(darwin_va_alloc_at(&va,base+5*page,2*page));claim(state,base+5*page,2*page);
  check(va,state,true);
  darwin_va_free(&va,base+5*page,2*page);release(state,{base+5*page,2*page});check(va,state);
  while(uint64_t at=darwin_va_alloc(&va,page,page,false)){claim(state,at,page);check(va,state);}
  finish(va);assert(failures.empty());
 }
 {
  darwin_va va{};std::vector<unsigned char> state(16);
  failures.insert(allocation_calls+1);assert(!darwin_va_add(&va,base,16*page));
  assert(!darwin_va_alloc(&va,page,page,false));finish(va);
  assert(darwin_va_add(&va,base,16*page));
  assert(darwin_va_alloc_at(&va,base+5*page,2*page));claim(state,base+5*page,2*page);
  // Both neighbouring holes were removed before a merged-hole allocation fails.
  failures.insert(allocation_calls+1);darwin_va_free(&va,base+5*page,2*page);release(state,{base+5*page,2*page});
  check(va,state,true);assert(!va.holes && !darwin_va_alloc(&va,page,page,false));finish(va);
 }
 {
  darwin_va va{};std::vector<unsigned char> state(16);assert(darwin_va_add(&va,base,16*page));
  assert(!darwin_va_alloc(&va,page,3,false) && !darwin_va_alloc(&va,0,page,false));
  assert(!darwin_va_alloc_at(&va,base-page,page) && !darwin_va_alloc_at(&va,base,17*page));
  darwin_va_free(&va,base+page,page);assert(rejected_overlap==1 && va.used==0);check(va,state);finish(va);
 }
 unsigned operations=0;
 for(unsigned seed=0;seed<12;seed++) {
  std::mt19937 rng(0x8850+seed);darwin_va va{};std::vector<unsigned char> state(512);
  std::vector<Range> active;assert(darwin_va_add(&va,base,state.size()*page));
  for(unsigned i=0;i<12000;i++) {
   // Different true allocator paths and fragmented trees, without failure injection.
   if(!active.empty() && rng()%3==0) {
    unsigned j=rng()%active.size();Range r=active[j];darwin_va_free(&va,r.at,r.size);release(state,r);
    active[j]=active.back();active.pop_back();
   } else {
    uint64_t bytes=(1+rng()%13)*page,align=page<<(rng()%4),at=0;
    if(rng()%4==0) {uint64_t requested=base+(rng()%state.size())*page;
     if(darwin_va_alloc_at(&va,requested,bytes))at=requested;
    } else at=darwin_va_alloc(&va,bytes,align,(rng()&1));
    if(at){claim(state,at,bytes);active.push_back({at,bytes});}
   }
   check(va,state);++operations;
  }
  std::shuffle(active.begin(),active.end(),rng);
  for(Range r:active){darwin_va_free(&va,r.at,r.size);release(state,r);check(va,state);}
  assert(va.holes==1 && va.used==0 && va.merges>0);finish(va);
 }
 printf("PASS: actual Mesa RB dual-index invariants,fragmented destruction,144000 mixed VA operations,merge,overlap refusal,metadata OOM lost-not-reissued (%u injected),no leaks\n",injected);
 assert(operations==144000 && injected==6 && failures.empty() && live.empty());
#endif
}
