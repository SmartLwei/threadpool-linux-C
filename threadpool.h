#ifndef THREADPOOL_H_
#define THREADPOOL_H_
#include <pthread.h>



/**
 * 生产者——消费者模型
 * 生产者向任务队列中添加任务，每个任务为：
 * {处理消息的函数，函数的参数}
 * 处理消息的函数也成为钩子，回调函数
 * 为了构造队列，还需要包含一个next的指针
 * {回调函数，回调函数的参数，next}
 * 线程数组中的线程运行过程中调用job中的回调函数处理参数
 **/
struct job
{
	//子线程的回调函数，子线程调用该函数处理数据
	void* (*callback_function)(void* arg);
	//传递给子进程的参数
	void* arg;
	//使用链表构成一个任务队列
	struct job* next;
};

/**
 * 线程池"句柄"
 * 包含线程中加锁和同步相关的信息
 **/
struct threadpool
{
	int thread_num;					//线程池中的线程个数
	int queue_max_num;				//排队的最大任务数量
	struct job* head;				//任务队列中的第一个，当一个线程空闲后取该任务执行
	struct job* tail;				//任务队列中的最后一个，添加任务时添加到该任务之后
	pthread_t* pthreads;			//线程池中的线程数组
	pthread_mutex_t mutex;			//互斥锁，对队列控制信息的操作必须互斥
	pthread_cond_t queue_empty;		//任务队列为空的条件锁。当关闭线程池时，使用该条件保证已经在队列中的任务被处理后程序才退出
	pthread_cond_t queue_not_empty;	//任务队列不为空的条件锁。线程尝试提取job来处理，但是job队列为空时，会等待该条件
									//此后当向队列添加新的任务时，可以触发该条件唤醒某一个线程
	pthread_cond_t queue_not_full;	//当向任务队列添加任务时，如果任务队列已满，则添加任务的线程应该等待队列非满才能成功添加
									//此后当有线程提取一个job处理时，因为此时任务数量-1，任务队列由原来的满变为非满，触发该条件
	int queue_cur_num;				//当前排队的队列长度
	int queue_close;				//关闭队列，阻止添加新的任务，通常需要关闭线程池时先关闭任务队列
	                                //等队列中的任务处理完后再关闭线程数组
	int pool_close;					//关闭线程数组
};

/**
 * 初始化线程池
 * @thread_num		子线程数量
 * @queue_max_num	最大任务队列数量
 * */
struct threadpool* threadpool_init(int thread_num, int queue_max_num);

/**
 * 向任务队列添加任务
 * 添加时如果任务队列由空转换为非空，需要触发条件变量queue_not_empty
 *      当创建一个线程，或者一个线程空闲后，它会尝试从任务队列中提取任务执行
 *      如果任务队列为空，那么线程就会等待，即等待条件变量queue_not_empty
 *      因此添加任务时需要判断队列是否由空转换为非空，以便唤醒线程
 * 如果任务队列已满，则等待条件变量queue_not_full
 *      任务队列数量是有上限的，当达到上限时将不再不允许添加新的任务
 *      只有当已经满的任务变为不满，即有一个线程提取了一个任务使得队列不满时
 *      那个线程需要触发条件变量queue_not_full，来唤醒向队列添加任务的程序
 *      向队列添加任务通常都是生产者做的，因此这个函数可能阻塞主线程
 * 需要随时注意线程池是否已经发出了关闭线程数组或者关闭任务队列的指令
 * @pool				线程池
 * @callback_function	函数指针，使用这个函数处理数据
 * @arg					数据，回调函数的参数
 * */
int threadpool_add_job(struct threadpool* pool, void* (*callback_function)(void *arg), void* arg); 

/**
 * 使用这个函数创建子线程
 * 该函数从线程池的任务队列中提取任务
 * 提取任务时，如果任务队列为空，则等待条件变量queue_not_empty
 * 如果提取任务之前，队列已满，则提起之后触发条件变量queue_not_full
 *      如果队列满后，生产者还在尝试添加任务，那么生产者会阻塞
 *      因此如果满足条件，线程需要触发该条件变量
 * 提取任务之后，如果任务队列为空，则触发条件变量queue_empty
 *      当尝试关闭线程池时，应当首先关闭任务队列，阻止新的任务的添加
 *      此外，必须（最好）等待当前队列中所有的任务处理完成
 *      处理完最后一个任务后，触发该条件
 * 需要随时注意线程池是否已经发出了关闭线程数组或者关闭任务队列的指令
 * @arg				线程池
 * */
void* threadpool_function(void* arg);

/**
 * 销毁线程池，程序退出时调用
 * 在该函数中将关闭任务队列和关闭线程数组的标志置位
 * 该函数需要等待条件变量queue_empty，保证队列中的所有任务都被处理
 * @pool			线程池
 * */
int threadpool_destory(struct threadpool* pool);

#endif
