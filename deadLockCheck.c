#define _GNU_SOURCE

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <dlfcn.h>

#define MAX   100

typedef unsigned long int uint64;
typedef int (* pthread_mutex_lock_t) (pthread_mutex_t *mutex);
pthread_mutex_lock_t pthread_mutex_lock_f;

typedef int (* pthread_mutex_unlock_t) (pthread_mutex_t *mutex);
pthread_mutex_unlock_t pthread_mutex_unlock_f;


//主要的思想就是采用有向图来解决

//锁的两种状态
enum Type{
    PROCESS,
    RESOURCE,
};

struct source_type{
    uint64_t id;     //线程id
    enum Type type;  //资源状态

    uint64_t lock_id;//线程占用的锁的地址
    int degress;
};

//有向图的节点
struct vertex{
    struct source_type s;
    struct vertex* next;
};


//使用邻接表实现的图
struct task_graph{
    struct vertex list[MAX];   //邻接数组
    int num;

    //存储锁的占用情况信息
    struct source_type locklist[MAX];
    int lockidx;
};


struct task_graph* tg = NULL;
int path[MAX + 1];
int visited[MAX];     //在dfs检测死锁的时候会用到
int k  = 0;
int deadlock = 0;

int inc(int* value,int add){
    int old;
    __asm__ volatile(
        "lock;xaddl %2, %1;"
        : "=a"(old)
        : "m"(*value), "a" (add)
        : "cc","memory"
    );

    return old;
}

//创建图的节点
struct vertex* create_vertex(struct source_type type){
    struct vertex* tex = (struct vertex*)malloc(sizeof(struct vertex));
    tex->s = type;
    tex->next = NULL;

    return tex;
}

//找到是否存在type对应的数组项
int search_vertex(struct source_type type){
    int i = 0;
    for(i = 0;i < tg->num;++i){
        if(tg->list[i].s.type == type.type && tg->list[i].s.id == type.id){
            return i;
        }
    }
    return -1;
}

//添加节点
void add_vertex(struct source_type type){
    if(search_vertex(type) == -1){
        tg->list[tg->num].s = type;
        tg->list[tg->num].next = NULL;
        tg->num++;
    }
}

int add_edge(struct source_type from,struct source_type to){
    add_vertex(from);
    add_vertex(to);

    //添加一条从from到to的边
    struct vertex* v = &(tg->list[search_vertex(from)]);
    while(v->next != NULL){
        v = v->next;
    }
    v->next = create_vertex(to);
}

//验证i 和 j之间是否存在边
int verify_edge(struct source_type i,struct source_type j){
    if(tg->num == 0){
        return 0;
    }

    int idx = search_vertex(i);
    if(idx == -1){
        return 0;
    }

    struct vertex* v = &(tg->list[idx]);

    while(v != NULL){
        if(v->s.id == j.id){
            return 1;
        }
        v = v->next;
    }

    return 0;
}

//删除掉from 和 to之间存在的边
int remove_edge(struct source_type from,struct source_type to){
    int idxi = search_vertex(from);
    int idxj = search_vertex(to);

    if(idxi != -1 && idxj != -1){
        struct vertex* v = &tg->list[idxi];
        struct vertex* remove;

        while(v->next != NULL){
            if(v->next->s.id == to.id){
                remove = v->next;
                v->next = v->next->next;

                free(remove);
                break;
            }
            v = v->next;
        }
    }
}

int search_lock(uint64 lock){
    int i = 0;
    for(i = 0;i < tg->lockidx;++i){
        if(tg->locklist[i].lock_id == lock){
            return i;
        }
    }
    return -1;
}

int search_empty_lock(uint64 lock){
    int i = 0;
    for(i = 0;i < tg->lockidx;++i){
        if(tg->locklist[i].lock_id == 0){
            return i;
        }
    }
    return tg->lockidx;
}

void print_deadlock(void){
    int i = 0;
    printf("deadlock: ");
    for(i = 0;i < k - 1;++i){
        printf(" %ld--->",tg->list[path[i]].s.id);
    }
    printf("%ld\n",tg->list[path[i]].s.id);
}

int DFS(int idx){
    struct vertex* ver = &tg->list[idx];
    if(visited[idx] == 1){
        //如果已经访问过了，那么代表存在环，也就是存在死锁
        path[k++] = idx;
        print_deadlock();
        deadlock = 1;

        return 0;
    }

    //如果没有被加锁
    visited[idx] = 1;
    path[k++] = idx;

    while(ver->next != NULL){
        DFS(search_vertex(ver->next->s));
        k--;

        ver = ver->next;
    }

    return 1;
}

int search_for_cycle(int idx){
    struct vertex* ver = &tg->list[idx];
    visited[idx] = 1;
    k = 0;
    path[k++] = idx;

    while(ver->next != NULL){
        int i = 0;
        //将上一次的数据给清空
        for(i = 0;i < tg->num;++i){
            if(i == idx){
                continue;
            }
            visited[i] = 0;
        }

        for(i = 1;i < MAX;++i){
            path[i] = -1;
        }
        k = 1;

        DFS(search_vertex(ver->next->s));
        ver = ver->next;
    }
}

void check_dead_lock(void){
    int i = 0;
    deadlock = 0;

    for(i = 0;i < tg->num;++i){
        if(deadlock == 1){
            //检测到死锁
            break;
        }
        search_for_cycle(i);
    }
    if(deadlock == 0){
        printf("no deadlock\n");
    }
}

static void* thread_routine(void* arg){
    while(1){
        sleep(5);
        check_dead_lock();
    }
}
void start_check(void){
    tg = (struct task_graph*)malloc(sizeof(struct task_graph));
    tg->num = 0;
    tg->lockidx = 0;

    pthread_t tid;

    pthread_create(&tid,NULL,thread_routine,NULL);
}

//加锁之前会调用的函数
/*
     1.先判断锁是否被占有
        如果没有被占有，就直接加锁就完事
        如果被占有，就需要等在这里
*/

//线程tid请求对地址为mutex的锁进行加锁
int lock_before(pthread_t tid,uint64 lockaddr){
    int idx = 0;
    for(idx = 0;idx < tg->lockidx;idx++){
        if((tg->locklist[idx].lock_id == lockaddr)){
            //如果请求的这个锁已经被加上了
            struct source_type from;
            from.id = tid;
            from.type = PROCESS;
            add_vertex(from);

            struct source_type to;
            to.id = tg->locklist[idx].id;
            tg->locklist[idx].degress++;
            to.type = PROCESS;
            add_vertex(to);

            if(!verify_edge(from,to)){
                add_edge(from,to);
            }
        }
    }
}

/*
     判断锁是否被占有，如果能执行到这里，就说明本次加锁成功了，但是就应该在图中表示出他们之间的关系
*/
int lock_after(pthread_t tid,uint64 lockaddr){
    int idx = 0;
    if(-1 == (idx = search_lock(lockaddr))){
        //如果之前没有加锁
        int eidx = search_empty_lock(lockaddr);

        tg->locklist[eidx].id = tid;
        tg->locklist[eidx].lock_id = lockaddr;

        inc(&tg->lockidx,1);


    }else{
        //如果已经加上了锁
        struct source_type from;
        from.id = tid;
        from.type = PROCESS;

        struct source_type to;
        to.id = tg->locklist[idx].id;
        tg->locklist[idx].degress--;
        to.type = PROCESS;

        if(verify_edge(from,to)){
            remove_edge(from,to);
        }
        tg->locklist[idx].id = tid;
    }
}

/*
     删除关系
*/
int unlock_after(pthread_t tid,uint64 lockaddr){
    int idx = search_lock(lockaddr);
    if(tg->locklist[idx].degress == 0){
        tg->locklist[idx].id = 0;
        tg->locklist[idx].lock_id = 0;
    }
}

int pthread_mutex_lock(pthread_mutex_t* mutex){
    pthread_t selfid = pthread_self();
    lock_before(selfid,(uint64)mutex);
    pthread_mutex_lock_f(mutex);
    lock_after(selfid,(uint64)mutex);
}

int pthread_mutex_unlock(pthread_mutex_t* mutex){
    pthread_t selfid = pthread_self();
    pthread_mutex_unlock_f(mutex);
    unlock_after(selfid,(uint64)mutex);
}

static int init_hook(){
    pthread_mutex_lock_f  = dlsym(RTLD_NEXT,"pthread_mutex_lock");
    pthread_mutex_unlock_f = dlsym(RTLD_NEXT,"pthread_mutex_unlock");
}

pthread_mutex_t mutex1 = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex2 = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex3 = PTHREAD_MUTEX_INITIALIZER;

void* thread_func1(void* arg){
    pthread_mutex_lock(&mutex1);
    sleep(1);
    pthread_mutex_lock(&mutex2);

    pthread_mutex_unlock(&mutex2);
    pthread_mutex_unlock(&mutex1);

    return (void*)0;
}
void* thread_func2(void* arg){
    pthread_mutex_lock(&mutex2);
    sleep(1);
    pthread_mutex_lock(&mutex3);

    pthread_mutex_unlock(&mutex3);
    pthread_mutex_unlock(&mutex2);
}
void* thread_func3(void* arg){
    pthread_mutex_lock(&mutex1);
    sleep(1);
    pthread_mutex_lock(&mutex3);

    pthread_mutex_unlock(&mutex3);
    pthread_mutex_unlock(&mutex1);
}


int main(int argc,char** argv){
#if 0
    tg = (struct task_graph*)malloc(sizeof(struct task_graph));
    tg->num = 0;

    struct source_type v1;
    v1.id = 1;
    v1.type = PROCESS;
    add_vertex(v1);

    struct source_type v2;
    v2.id = 2;
    v2.type = PROCESS;
    add_vertex(v2);

    struct source_type v3;
    v3.id = 3;
    v3.type = PROCESS;
    add_vertex(v3);

    struct source_type v4;
    v4.id = 4;
    v4.type = PROCESS;
    add_vertex(v4);

    struct source_type v5;
    v5.id = 5;
    v5.type = PROCESS;
    add_vertex(v5);

    add_edge(v1,v2); //线程一申请线程二的资源
    add_edge(v2,v3); //线程二申请线程三的资源
    add_edge(v3,v4); //线程三申请线程四的资源
    add_edge(v4,v5); //线程四申请线程五的资源
    add_edge(v3,v1); //线程三申请线程一的资源

    search_for_cycle(search_vertex(v1));
#else
   init_hook();
   start_check();

   pthread_t tid1,tid2,tid3;
   pthread_create(&tid1,NULL,thread_func1,NULL);
   pthread_create(&tid2,NULL,thread_func2,NULL);
   pthread_create(&tid3,NULL,thread_func3,NULL);


   pthread_join(tid1,NULL);
   pthread_join(tid2,NULL);
   pthread_join(tid3,NULL);
#endif

  return 0;
}
