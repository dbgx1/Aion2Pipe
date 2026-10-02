#pragma once
// Test-process-only VFS shim. Actual SQLite WAL/transaction machinery handles
// the injected write error; no production files or real disk capacity are changed.
#include <sqlite3.h>
#include <atomic>
#include <map>
#include <mutex>
#include <stdexcept>
namespace sqlite_fault {
inline std::atomic<int> errorCode{SQLITE_OK};
inline std::atomic<unsigned> failures{};
struct Methods {const sqlite3_io_methods* original;sqlite3_io_methods wrapped;};
inline std::map<sqlite3_file*,Methods> files;
inline std::mutex mutex;
inline sqlite3_vfs* parent{};
inline sqlite3_vfs wrapper{};
inline int write(sqlite3_file* file,const void* buffer,int bytes,sqlite3_int64 offset){
    const auto error=errorCode.load();if(error!=SQLITE_OK){++failures;return error;}
    const sqlite3_io_methods* original;
    {std::lock_guard lock(mutex);original=files.at(file).original;}
    return original->xWrite(file,buffer,bytes,offset);
}
inline int close(sqlite3_file* file){
    const sqlite3_io_methods* original;
    {std::lock_guard lock(mutex);original=files.at(file).original;file->pMethods=original;}
    const auto result=original->xClose(file);
    {std::lock_guard lock(mutex);files.erase(file);}
    return result;
}
inline int open(sqlite3_vfs*,const char* name,sqlite3_file* file,int flags,int* output){
    const auto result=parent->xOpen(parent,name,file,flags,output);
    if(result==SQLITE_OK){
        std::lock_guard lock(mutex);auto& saved=files.emplace(file,Methods{file->pMethods,*file->pMethods}).first->second;
        saved.wrapped.xWrite=write;saved.wrapped.xClose=close;file->pMethods=&saved.wrapped;
    }
    return result;
}
inline void install(){
    parent=sqlite3_vfs_find(nullptr);if(!parent)throw std::runtime_error("no default SQLite VFS");
    wrapper=*parent;wrapper.zName="aion-test-write-fault";wrapper.pNext=nullptr;wrapper.xOpen=open;
    if(sqlite3_vfs_register(&wrapper,1)!=SQLITE_OK)throw std::runtime_error("register fault VFS");
}
}
