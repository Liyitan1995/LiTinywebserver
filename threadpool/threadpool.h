#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
//简单封装了互斥量和信号量的接口
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
//线程池类模板参数T是任务类型，T中必须有接口process
template <typename T>
class threadpool
{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    //connPool是数据库连接池指针
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request, int state);
    bool append_p(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    //启动线程池
    void run();
private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数 10000
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue; //请求队列
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //是否有任务需要处理
    connection_pool *m_connPool;  //数据库连接池
    int m_actor_model;          //模型切换
};
template <typename T> //线程池构造函数
threadpool<T>::threadpool( int actor_model, connection_pool *connPool, int thread_number, int max_requests) : m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
        throw std::exception();
    for (int i = 0; i < thread_number; ++i)
    {
        //循环创建线程，并将工作线程按要求进行运行
        if (pthread_create(m_threads + i, NULL, worker, this) != 0) //这个this会传给 worker的arg
        {
            delete[] m_threads;
            throw std::exception();
        }
        //将线程进行分离后，不用单独对工作线程进行回收,使得其它线程回收和杀死该线程
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
}

template <typename T>
bool threadpool<T>::append(T *request, int state)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();//信号量的V操作，多了一个工作任务T使得信号量+1
    return true;
}
//将“待办工作”加入到请求队列
//传入的是fd
template <typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
//线程回调函数/工作函数，arg其实是this
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    //将参数强转为线程池类，调用成员方法
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}
//回调函数会调用这个函数工作
//工作线程就是不断地等任务队列有新任务，然后就加锁取任务->取到任务解锁->执行任务
template <typename T>
void threadpool<T>::run()
{
    cout << "pool线程\n";
    while (true)
    {
        cout << "线程池waiting\n";
        //信号量等待
        m_queuestat.wait();
        //被唤醒后先加互斥锁
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }

        //从请求队列中取出第一个任务
        //将任务从请求队列删除
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
            continue;
        //preator reactor
        if (1 == m_actor_model)
        {
            if (0 == request->m_state)
            {
                if (request->read_once())
                {
                    request->improv = 1;
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    request->process();
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
            else
            {
                if (request->write())
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        else
        {
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }
        cout << "单个线程down\n";
    }
}
#endif
