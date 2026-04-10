`pthread_t` 
``` cpp
int pthread_create(pthread_t _Nullable * _Nonnull __restrict, 一个(不能为空的)指针，指向一个可以为空(0)的线程ID
    const pthread_attr_t * _Nullable __restrict, 一个可以为空的指针，指向配置属性
    void * _Nullable (* _Nonnull)(void * _Nullable),
    void * _Nullable __restrict);
int pthread_create(pthread_t * __restrict,
    const pthread_attr_t * _Nullable __restrict,
    void *(* _Nonnull)(void *), void * _Nullable __restrict);
```

四个参数含义：
线程ID
线程属性
线程运行函数
函数参数
