WeChat: cstutorcs
QQ: 749389476
Email: tutorcs@163.com
#ifndef SFS_THREADS_H
#define SFS_THREADS_H

// This header provides a wrapper for pthread_create to work with the
//   instrumentation without having to instrument the entire LUA
//   library.  It is not for general use.
//
//#include <pthread.h>

int __ctThreadCreateActual(pthread_t *thread, const pthread_attr_t *attr,
                           void *(*start_routine)(void *), void *arg);
int sfs_thread_create_internal(pthread_t *thread, const pthread_attr_t *attr,
                               void *(*start_routine)(void *), void *arg);

#endif
