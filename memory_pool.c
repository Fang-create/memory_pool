#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>


// 以alignment对齐
#define mp_align(n, alignment) (((n)+(alignment-1)) & ~(alignment-1))
#define mp_align_ptr(p, alignment) (void *)((((size_t)p)+(alignment-1)) & ~(alignment-1))
//对齐
#define MP_ALIGNMENT  32   
//允许分配最大的块大小
#define MAX_ALLOC_BLOCK 4096
//设计内存池----小块内存预先分配，大块内存直接分配
#define MP_PAGE_SIZE  4096
#define MP_MAX_ALLOC_FROM_POOL	 (MP_PAGE_SIZE-1)

static int count = 0;
struct mp_pool_s * mp_create_pool(size_t size);
void mp_reset_pool(struct mp_pool_s * pool);
void mp_destroy_pool(struct mp_pool_s *pool);
void *mp_alloc(struct mp_pool_s *pool, size_t size);
void *mp_calloc(struct mp_pool_s * pool, size_t size);
void mp_free(struct mp_pool_s *pool, void *p);

struct mp_large_s{
	void *alloc;  //指向大块的内存
	struct mp_large_s *next;
};

//定义，首先申请一块内存，使用接口pmalloc取内存；
struct mp_node_s{
	unsigned char *start;
	unsigned char *end;
	
	struct mp_node_s *next;   //内存不够的malloc一块，使用链表连接
	int failed; //表示内存是否被使用
};

struct mp_pool_s{
	size_t max;    //大小块区分标识
	struct mp_node_s *current;
	struct mp_large_s *large;
	//柔性数组，加一个标签
	struct mp_node_s head[0];   //指向第一个mp_node_s节点，不知道有多少个节点
};

//四个接口函数
// create memory pool

//destroy pool

//malloc/calloc

//free

struct mp_pool_s * mp_create_pool(size_t size)
{
	//尽量不要使用全局的，会分散内存
	struct mp_pool_s *p;
	//
	//使用函数用于申请大块内存 MP_ALIGNMENT为2的幂
	//int posix_memalign(void **memptr, size_t alignment, size_t size);
	int ret = posix_memalign((void **)&p, MP_ALIGNMENT, size + sizeof(
				         struct mp_pool_s) +  sizeof(struct mp_node_s));
	if(ret != 0){
		perror("posix_memalign");
		return NULL;
	}
	
	p->max = size < MP_MAX_ALLOC_FROM_POOL ? size : MP_MAX_ALLOC_FROM_POOL;
	p->current = p->head;
	p->large = NULL;
	p->head->start = (unsigned char *)p + sizeof(struct mp_pool_s) + sizeof(struct mp_node_s);
	p->head->end = p->head->start + size;
	p->head->failed = 0;

	return p; 
}

void mp_reset_pool(struct mp_pool_s * pool)
{
	struct mp_node_s *h;
	struct mp_large_s *l;

	for(l = pool->large; l; l = l->next){
		if(l->alloc){
			free(l->alloc);
		}
	}
	pool->large = NULL;
	//printf("free block\n");
	for(h = pool->head; h; h = h->next){
		h->start = (unsigned char *)h + sizeof(struct mp_node_s);
	}
}
void  mp_destroy_pool(struct mp_pool_s *pool)
{
	struct mp_large_s *l,*t;
	//mp_large_s 未释放? 因为mp_large_s内存从pool中分配，小块内存是不释放的；
	for(l = pool->large;l ;l = l->next){
		if(l->alloc){
			free(l->alloc);
		}
	}

	struct mp_node_s *h = pool->head->next;
	struct mp_node_s *p;
	while(h){
		p = h->next;
		free(h);
		h = p;
	}
	p = NULL;
	free(pool);
}

static void *mp_alloc_block(struct mp_pool_s *pool, size_t size)
{
	count++;
	unsigned char *m = NULL;
	struct mp_node_s *p = pool->head;  //p + sizeof(mp_node_s) = p->start
	size_t psize = (size_t)(p->end - (unsigned char *)p);

	int ret = posix_memalign((void **)&m, MP_ALIGNMENT, psize);
	if(ret){
		perror("posix_memalign");
		return NULL;
	}

	struct mp_node_s *new_node = (struct mp_node_s *)m;
	new_node->end = m + psize;
	new_node->next = NULL;
	new_node->failed = 0;

	m = m + sizeof(struct mp_node_s);
	//对齐
	m = mp_align_ptr(m, MP_ALIGNMENT);
	new_node->start = m + size;

	struct mp_node_s *t;
	struct mp_node_s *current = pool->current;
	//避免链表过长，大于4个就将current往后移动；
	for(t = current; t->next != NULL;t = t->next){
		if(t->failed++ > 4){
			current = t->next;
		}
	}
	//将节点放到最后
	t->next = new_node; 
	//current 是否为第一个节点，如果是
	pool->current = current ? current : new_node;  
	return m;
}

//大块内存申请
static void *mp_alloc_large(struct mp_pool_s *pool, size_t size)
{
	void *p = NULL;
	int ret = posix_memalign((void **)&p, MP_ALIGNMENT, size);
	if(ret != 0){
		perror("posix_memalign");
		return NULL;
	}
	struct mp_large_s *large;
	size_t n = 0;
	for(large = pool->large; large;large = large->next){
		//如果large块中有空的位置，则放入空的位置
		if(large->alloc == NULL){
			large->alloc = p;
			return p;
		}
		//如果前三个都不是空的，就不取检测了,直接将p 头插法
		if(n++ > 3){
			break;  
		}
	}
	//申请mp_large_s指针
	large = mp_alloc(pool, sizeof(struct mp_large_s));
	if(large == NULL){
		free(p);
		return NULL;
	}
	//将void *p 挂在alloc中
	large->alloc = p;
	//头插法
	large->next = pool->large;
	pool->large = large;

	return p;
}

//pool内存申请函数
void *mp_alloc(struct mp_pool_s *pool, size_t size)
{
	struct mp_node_s *p;
	unsigned char *m;
	if(size <= pool->max){
		p = pool->current;
		do{
			m = mp_align_ptr(p->start, MP_ALIGNMENT);   //对齐
			if((size_t)(p->end - m) >= size){
				p->start = m + size;
				return m;
			}
			p = p->next;		
		}while(p);
		return mp_alloc_block(pool, size);
	}
	//返回大块的内存申请
	return mp_alloc_large(pool, size);
}

//calloc

void *mp_calloc(struct mp_pool_s * pool, size_t size)
{
	void *p = mp_alloc(pool, size);
	if(p != NULL){
		memset(p, 0, size);
	}
	return p;
}
//free内存，只是释放大块的内存；
void mp_free(struct mp_pool_s *pool, void *p)
{
	struct mp_large_s *l;
	for(l = pool->large; l; l = l->next){
		//如果p的指针与其相等
		if(p == l->alloc){
			free(l->alloc);
			l->alloc = NULL;

			return;
		}
	}

	//小块内存不释放；
	
}


int main(int argc, char * argv [ ])
{
	// 4096
	int size = 1 << 12;

	struct mp_pool_s *p = mp_create_pool(size);
	if(!p) return -1;

	int i = 0;
	for(i = 0;i < 10;i++){
		void *mp = mp_alloc(p, 512);
	}
	printf("malloc count = %d\n",count);
	int j = 0;
	for(i = 0;i < 5;i++){
		char *pp = mp_calloc(p, 32);
		for(j = 0; j < 32;j++){
			if(pp[j]){
				printf("calloc wrong\n");
			}
		}
			
	}
	printf("malloc count = %d\n",count);
	for(i = 0; i < 5; i++){
		void *l = mp_alloc(p, 8192);
		mp_free(p, l);
	}
	mp_reset_pool(p);

	
	
	for (i = 0;i < 80;i ++) {
		mp_alloc(p, 256);
	}
	printf("malloc count = %d\n",count);
	mp_destroy_pool(p);

	return 0;
	
}


