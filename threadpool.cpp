#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include "threadpool.h"

//初始化线程池
struct threadpool* threadpool_init(int thread_num, int queue_max_num)
{
	struct threadpool* pool = NULL;
	do
	{
		//创建线程池并初始化线程池参数
		pool = (threadpool*)malloc(sizeof(struct threadpool));
		if(pool == NULL)
		{
			DEBUG_PRINT_ERROR_MSG("failed to malloc threadpool!");
			break;
		}
		pool->thread_num = thread_num;
		pool->queue_max_num = queue_max_num;
		pool->queue_cur_num = 0;
		pool->head = NULL;
		pool->tail = NULL;
		if(pthread_mutex_init(&(pool->mutex),NULL))
		{
			DEBUG_PRINT_ERROR_MSG("failed to init mutex!");
			break;
		}
		if(pthread_cond_init(&(pool->queue_empty),NULL))
		{
			DEBUG_PRINT_ERROR_MSG("failed to init queue_empty!");
			break;
		}
		if(pthread_cond_init(&(pool->queue_not_empty),NULL))
		{
			DEBUG_PRINT_ERROR_MSG("failed to init queue_not_empty");
			break;
		}
		if(pthread_cond_init(&(pool->queue_not_full),NULL))
		{
			DEBUG_PRINT_ERROR_MSG("failed to init queue_noe_full");
			break;
		}

		//新建线程数组
		pool->pthreads = (pthread_t *)malloc(sizeof(pthread_t)*thread_num);
		if(pool->pthreads == NULL)
		{
			DEBUG_PRINT_ERROR_MSG("failed to malloc pthreads");
			break;
		}
		pool->queue_close = 0;
		pool->pool_close = 0;
		int i;
		for(i = 0; i<pool->thread_num; ++i)
		{
		    //事实上，创建线程后，他们会阻塞在等待任务队列非空的条件变量上
		    //注意到，传递给线程创建函数的参数是线程池句柄
			pthread_create(&(pool->pthreads[i]),NULL,threadpool_function,(void*) pool);
		}

		return pool;
	}while(0);

	return NULL;
}

//向线程池的任务队列添加任务
int threadpool_add_job(struct threadpool* pool, void* (*callback_function)(void* arg), void* arg)
{
	assert(pool != NULL);
	assert(callback_function != NULL);
	assert(arg != NULL);

	//对任务队列的操作需要上锁
	pthread_mutex_lock(&(pool->mutex));
	
	while((pool->queue_cur_num == pool->queue_max_num) && !(pool->queue_close || pool->pool_close))
	{
		//任务队列已满的情况下，需要等待一个线程处理一个任务，发出queue_not_full信号后，才能添加任务
		//同时需要注意，在等待期间，线程池关闭信号是否已经被置位
		//当关闭线程池时，主线程需要广播该信号，以避免有线程在此阻塞而无法关闭
		pthread_cond_wait(&(pool->queue_not_full),&(pool->mutex));
	}

	if(pool->queue_close || pool->pool_close)
	{
		//如果在等待queue_not_full过程中，程序发出了关闭指令，则退出子线程
		pthread_mutex_unlock(&(pool->mutex));
		return -1;
	}

	//向任务队列添加任务
	struct job* pjob = (struct job*)malloc(sizeof(struct job));
	if(pjob == NULL)
	{
	    //返回前必须释放锁，否则造成死锁
		pthread_mutex_unlock(&(pool->mutex));
		return -1;
	}
	pjob->callback_function = callback_function;
	pjob->arg = arg;
	pjob->next = NULL;

	if(pool->head == NULL)
	{
		//如果任务队列为空
		pool->head = pool->tail = pjob;
		//发出队列非空的信号，唤醒线程来处理任务
		pthread_cond_broadcast(&(pool->queue_not_empty));
		//pthread_cond_signal(&(pool->queue_not_empty));
	}
	else
	{
	    //任务队列非空，那么直接在队列尾部添加
		pool->tail->next = pjob;
		pool->tail = pjob;
	}

    //任务队列+1
	pool->queue_cur_num++;
	//上面对线程池中的控制变量的操作必须是互斥的
	pthread_mutex_unlock(&(pool->mutex));

	return 0;
}

//创建线程的函数，在该函数中调用回调函数处理参数
void* threadpool_function(void* arg)
{
	struct threadpool* pool = (struct threadpool*)arg;
	struct job* pjob = NULL;
	//程序死循环，从队列中提取消息并调用回调函数处理数据
	while(1)
	{
		pthread_mutex_lock(&(pool->mutex));
		while((pool->queue_cur_num == 0) && !pool->pool_close)
		{
			//任务队列为空时，等待条件queue_not_empty
			//该条件由任务添加函数在任务队列为空时触发
			//在线程刚创建时，会阻塞在该条件上
			pthread_cond_wait(&(pool->queue_not_empty),&(pool->mutex));
		}

		if(pool->pool_close)
		{
			//等待条件时，可能程序已经发起关闭信号
			//释放锁资源，避免死锁
			pthread_mutex_unlock(&(pool->mutex));
			pthread_exit(NULL);
		}

        //处理一个任务，则任务队列-1
		pool->queue_cur_num--;
		pjob = pool->head;
		if(pool->queue_cur_num == 0)
		{
			pool->head = pool->tail = NULL;
			//通告队列已经为空
			//在关闭线程池时需要该信号通知主进程，任务已经提取完毕
			//主进程必须等待任务处理完毕，而不是提取完毕才能退出
			pthread_cond_signal(&(pool->queue_empty));
		}
		else
		{
			pool->head = pjob->next;
		}

		if(pool->queue_cur_num == pool->queue_max_num-1)
		{
			//通告队列非满
			//在生产者向队列添加任务，但是队列已满时，需要等待该条件
			//因此在满足条件时需要触发该条件变量唤醒生产者
			pthread_cond_broadcast(&(pool->queue_not_full));
		}
		
		//上面对线程池中的控制变量的操作必须是互斥的
		pthread_mutex_unlock(&(pool->mutex));

		//调用回调函数处理数据
		(*(pjob->callback_function))(pjob->arg);
		free(pjob);
		pjob = NULL;
	}
}

//销毁线程池
int threadpool_destory(struct threadpool* pool)
{
	assert(pool != NULL);
	
	//对线程池控制变量的操作必须互斥
	//（最好可以使用高的优先级抢占锁资源，怎么实现？）
	pthread_mutex_lock(&(pool->mutex));
	
	//若线程池已经销毁或者正在由其他线程销毁
	if(pool->queue_close || pool->pool_close)
	{
		pthread_mutex_unlock(&(pool->mutex));
		return -1;
	}
	
	//先关闭任务队列，避免新的任务添加
	pool->queue_close = 1;
	while(pool->queue_cur_num != 0)
	{
		//等待任务队列为空，即等待线程队列将任务队列提取完毕
		pthread_cond_wait(&(pool->queue_empty),&(pool->mutex));
	}

	//关闭线程
	pool->pool_close = 1;
	//上面对线程池中的控制变量的操作必须是互斥的
	pthread_mutex_unlock(&(pool->mutex));
	
	//唤醒线程池中等待的任务队列
	pthread_cond_broadcast(&(pool->queue_not_full));
	pthread_cond_broadcast(&(pool->queue_not_empty));
	
	
	int i;
	for(i=0; i< pool->thread_num; ++i)
	{
		//等待线程池中的任务处理完毕
		//上面的开关只是任务提取完毕，他们可能还没有处理完
		pthread_join(pool->pthreads[i],NULL);
	}

	//清理资源
	pthread_mutex_destroy(&(pool->mutex));
	pthread_cond_destroy(&(pool->queue_empty));
	pthread_cond_destroy(&(pool->queue_not_empty));
	pthread_cond_destroy(&(pool->queue_not_full));
	free(pool->pthreads);
	struct job* p;
	while(pool->head != NULL)
	{
		p = pool->head;
		pool->head = p->next;
		free(p);
	}
	free(pool);
	return 0;
}

