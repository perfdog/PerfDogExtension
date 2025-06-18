// Copyright 2022 Tencent Inc. All rights reserved.
//
// Author: PerfDog@tencent.com
// Version: 1.3

#if defined(__ANDROID__) || defined(__APPLE__) || defined(_WIN32) || defined(_GAMING_XBOX)

#include "perfdog_extension_cpp98.h"

#ifdef PERFDOG_EXTENSION_ENABLE

// C++98 compatible includes
#include <string>
#include <vector>
#include <map>
#include <list>
#include <utility>
#include <stdint.h>
#include <stdlib.h>
#include <sstream>
#include <cstring>
#include <cstdio>

// Platform specific includes
#if defined(__ANDROID__) || defined(__APPLE__)
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include <errno.h>
#define INVALID_SOCKET (-1)
#define CAUSE_BY_SIGNAL() (errno == EINTR)
#elif defined(_WIN32)
#include <windows.h>
#include <winsock.h>
#include <process.h>
#define socklen_t int
#define CAUSE_BY_SIGNAL() (false)
#endif

#if defined(__ANDROID__)
#include <android/log.h>
#elif defined(__APPLE__)
#include <Foundation/Foundation.h>
#include <mach/mach_time.h>
#elif defined(_WIN32) || defined(_GAMING_XBOX)
#include <processthreadsapi.h>
#endif

#if defined(__ANDROID__)
#define SOCKET_SEND_FLAG MSG_NOSIGNAL
#else
#define SOCKET_SEND_FLAG 0
#endif

namespace perfdog {

// C++98 compatible constants
static const uint32_t kMagicNumber = 'PDEX';
static const uint32_t kProtocolVersion = 1;
static const int kMaxMessageLength = 4096;
static const int kBlockSize = 4096;

// C++98 compatible enum
enum MessageType {
  kInvalid = 0,
  kStartTestReq,  // 空 empty
  kStartTestRsp,  // 空 empty
  kStopTestReq,   // 空 empty
  kStopTestRsp,   // 空 empty
  kStringMap,
  kCustomFloatValue,
  kCustomIntegerValue,
  kCustomStringValue,
  kCleanMap,  // 空 empty
  kSetLabelReq,
  kAddNoteReq,
  kDeepMarkReq,
  kDeepCounterReq,
  kDeepPushReq,
  kDeepPopReq,
};

struct Header {
  uint32_t magic_number;
  uint32_t version;
  
  Header() {
    magic_number = kMagicNumber;
    version = kProtocolVersion;
  }
};

struct MessageHeader {
  uint16_t length;
  uint16_t type;
};

static const int kMessageHeaderLength = sizeof(MessageHeader);

// C++98 compatible Buffer class
class Buffer {
 public:
  Buffer(char* buf, int capacity) : buf_(buf), capacity_(capacity), pos_(0), is_overflow_(false) {}

  int DataSize() const { return pos_; }
  int Remaining() const { return capacity_ - pos_; }
  
  Buffer Slice() { return Buffer(buf_ + pos_, capacity_ - pos_); }

  void Advance(uint32_t n) {
    pos_ += n;
    if (pos_ > capacity_) {
      is_overflow_ = true;
      pos_ = capacity_;
    }
  }

  void WriteInt32(int32_t value) { WriteBytes(&value, sizeof(value)); }
  int32_t ReadInt32() {
    int32_t value = 0;
    ReadBytes(&value, sizeof(value));
    return value;
  }

  void WriteUint32(uint32_t value) { WriteBytes(&value, sizeof(value)); }
  uint32_t ReadUint32() {
    uint32_t value = 0;
    ReadBytes(&value, sizeof(value));
    return value;
  }

  void WriteUint64(uint64_t value) { WriteBytes(&value, sizeof(value)); }
  uint64_t ReadUint64() {
    uint64_t value = 0;
    ReadBytes(&value, sizeof(value));
    return value;
  }

  void WriteFloat(float value) { WriteBytes(&value, sizeof(value)); }
  float ReadFloat() {
    float value = 0;
    ReadBytes(&value, sizeof(value));
    return value;
  }

  void WriteString(const std::string& value) {
    WriteUint32(static_cast<uint32_t>(value.size()));
    if (!value.empty()) {
      WriteBytes(const_cast<void*>(static_cast<const void*>(value.data())), static_cast<uint32_t>(value.size()));
    }
  }

  void ReadString(std::string& value) {
    uint32_t size = ReadUint32();
    value.clear();
    if (size > 0) {
      value.resize(size);
      ReadBytes(&value[0], size);
    }
  }

  void WriteBytes(void* data, uint32_t size) {
    if (is_overflow_) return;
    if (pos_ + size > static_cast<uint32_t>(capacity_)) {
      is_overflow_ = true;
      printf("overflow\n");
      return;
    }
    memcpy(buf_ + pos_, data, size);
    pos_ += size;
  }

  void ReadBytes(void* data, uint32_t size) {
    if (is_overflow_) return;
    if (pos_ + size > static_cast<uint32_t>(capacity_)) {
      is_overflow_ = true;
      return;
    }
    memcpy(data, buf_ + pos_, size);
    pos_ += size;
  }

  bool IsOverflow() const { return is_overflow_; }

 private:
  char* buf_;
  int capacity_;
  int pos_;
  bool is_overflow_;
};

// C++98 compatible Message base class
class Message {
 public:
  virtual ~Message() {}
  virtual MessageType Type() const = 0;
  virtual void Serialize(Buffer& buffer) const = 0;
  virtual void Deserialize(Buffer& buffer) = 0;
};

// C++98 compatible StringMap
class StringMap : public Message {
 public:
  StringMap() : id_(0) {}
  StringMap(uint32_t id, const std::string& name) : id_(id), name_(name) {}

  virtual MessageType Type() const { return kStringMap; }
  uint32_t Id() const { return id_; }
  const std::string& Name() const { return name_; }

  virtual void Serialize(Buffer& buffer) const {
    buffer.WriteUint32(id_);
    buffer.WriteString(name_);
  }

  virtual void Deserialize(Buffer& buffer) {
    id_ = buffer.ReadUint32();
    buffer.ReadString(name_);
  }

 private:
  uint32_t id_;
  std::string name_;
};

// C++98 compatible StringOrId
class StringOrId : public Message {
 public:
  static const uint32_t kInvalidStringId = 0xffffffff;

  StringOrId() : id_(kInvalidStringId) {}
  StringOrId(uint32_t id) : id_(id) {}
  StringOrId(const std::string& string) : id_(kInvalidStringId), string_(string) {}

  bool IsId() const { return id_ != kInvalidStringId; }
  uint32_t GetId() const { return id_; }
  const std::string& GetString() const { return string_; }

  virtual MessageType Type() const { return kInvalid; }

  virtual void Serialize(Buffer& buffer) const {
    buffer.WriteUint32(id_);
    buffer.WriteString(string_);
  }

  virtual void Deserialize(Buffer& buffer) {
    id_ = buffer.ReadUint32();
    buffer.ReadString(string_);
  }

 private:
  uint32_t id_;
  std::string string_;
};

// C++98 compatible CustomValue
class CustomValue : public Message {
 public:
  CustomValue() : time_(0) {}
  CustomValue(uint64_t time, const StringOrId& category, const StringOrId& key_name)
      : time_(time), category_(category), key_name_(key_name) {}

  virtual MessageType Type() const { return kInvalid; }

  virtual void Serialize(Buffer& buffer) const {
    buffer.WriteUint64(time_);
    category_.Serialize(buffer);
    key_name_.Serialize(buffer);
  }

  virtual void Deserialize(Buffer& buffer) {
    time_ = buffer.ReadUint64();
    category_.Deserialize(buffer);
    key_name_.Deserialize(buffer);
  }

  uint64_t GetTime() const { return time_; }
  StringOrId& GetCategory() { return category_; }
  StringOrId& GetKeyName() { return key_name_; }

 private:
  uint64_t time_;
  StringOrId category_;
  StringOrId key_name_;
};

// C++98 compatible CustomFloatValue
class CustomFloatValue : public CustomValue {
 public:
  CustomFloatValue() {}
  CustomFloatValue(uint64_t time, const StringOrId& category, const StringOrId& key_name, const std::vector<float>& values)
      : CustomValue(time, category, key_name), values_(values) {}

  virtual MessageType Type() const { return kCustomFloatValue; }

  virtual void Serialize(Buffer& buffer) const {
    CustomValue::Serialize(buffer);
    buffer.WriteUint32(static_cast<uint32_t>(values_.size()));
    for (size_t i = 0; i < values_.size(); ++i) {
      buffer.WriteFloat(values_[i]);
    }
  }

  virtual void Deserialize(Buffer& buffer) {
    CustomValue::Deserialize(buffer);
    uint32_t size = buffer.ReadUint32();
    values_.clear();
    if (size > 0) {
      values_.reserve(size);
      for (uint32_t i = 0; i < size; ++i) {
        values_.push_back(buffer.ReadFloat());
      }
    }
  }

 private:
  std::vector<float> values_;
};

// C++98 compatible CustomIntegerValue
class CustomIntegerValue : public CustomValue {
 public:
  CustomIntegerValue() {}
  CustomIntegerValue(uint64_t time, const StringOrId& category, const StringOrId& key_name, const std::vector<int32_t>& values)
      : CustomValue(time, category, key_name), values_(values) {}

  virtual MessageType Type() const { return kCustomIntegerValue; }

  virtual void Serialize(Buffer& buffer) const {
    CustomValue::Serialize(buffer);
    buffer.WriteUint32(static_cast<uint32_t>(values_.size()));
    for (size_t i = 0; i < values_.size(); ++i) {
      buffer.WriteInt32(values_[i]);
    }
  }

  virtual void Deserialize(Buffer& buffer) {
    CustomValue::Deserialize(buffer);
    uint32_t size = buffer.ReadUint32();
    values_.clear();
    if (size > 0) {
      values_.reserve(size);
      for (uint32_t i = 0; i < size; ++i) {
        values_.push_back(buffer.ReadInt32());
      }
    }
  }

  const std::vector<int32_t>& GetValues() const { return values_; }

 private:
  std::vector<int32_t> values_;
};

// C++98 compatible CustomStringValue
class CustomStringValue : public CustomValue {
 public:
  CustomStringValue() {}
  CustomStringValue(uint64_t time, const StringOrId& category, const StringOrId& key_name, const StringOrId& value)
      : CustomValue(time, category, key_name), value_(value) {}

  virtual MessageType Type() const { return kCustomStringValue; }

  virtual void Serialize(Buffer& buffer) const {
    CustomValue::Serialize(buffer);
    value_.Serialize(buffer);
  }

  virtual void Deserialize(Buffer& buffer) {
    CustomValue::Deserialize(buffer);
    value_.Deserialize(buffer);
  }

 private:
  StringOrId value_;
};

// C++98 compatible SetLabelReq
class SetLabelReq : public Message {
 public:
  SetLabelReq(uint64_t time, const std::string& name) : time_(time), name_(name) {}

  virtual MessageType Type() const { return kSetLabelReq; }

  virtual void Serialize(Buffer& buffer) const {
    buffer.WriteUint64(time_);
    buffer.WriteString(name_);
  }

  virtual void Deserialize(Buffer& buffer) {
    time_ = buffer.ReadUint64();
    buffer.ReadString(name_);
  }

 private:
  uint64_t time_;
  std::string name_;
};

// C++98 compatible AddNoteReq
class AddNoteReq : public Message {
 public:
  AddNoteReq(uint64_t time, const std::string& name) : time_(time), name_(name) {}

  virtual MessageType Type() const { return kAddNoteReq; }

  virtual void Serialize(Buffer& buffer) const {
    buffer.WriteUint64(time_);
    buffer.WriteString(name_);
  }

  virtual void Deserialize(Buffer& buffer) {
    time_ = buffer.ReadUint64();
    buffer.ReadString(name_);
  }

 private:
  uint64_t time_;
  std::string name_;
};

// C++98 compatible DeepMessage
class DeepMessage : public Message {
 public:
  DeepMessage(uint64_t time, int threadId, const StringOrId& name)
      : time_(time), thread_id_(threadId), name_(name) {}

  virtual MessageType Type() const = 0;

  virtual void Serialize(Buffer& buffer) const {
    buffer.WriteUint64(time_);
    buffer.WriteInt32(thread_id_);
    name_.Serialize(buffer);
  }

  virtual void Deserialize(Buffer& buffer) {
    time_ = buffer.ReadUint64();
    thread_id_ = buffer.ReadInt32();
    name_.Deserialize(buffer);
  }

 private:
  uint64_t time_;
  int thread_id_;
  StringOrId name_;
};

// C++98 compatible DeepMarkReq
class DeepMarkReq : public DeepMessage {
 public:
  DeepMarkReq(uint64_t time, int threadId, const StringOrId& name) : DeepMessage(time, threadId, name) {}

  virtual MessageType Type() const { return kDeepMarkReq; }
};

// C++98 compatible DeepCounterReq
class DeepCounterReq : public Message {
 public:
  DeepCounterReq(uint64_t time, const StringOrId& name, float value) : time_(time), name_(name), value_(value) {}

  virtual MessageType Type() const { return kDeepCounterReq; }

  virtual void Serialize(Buffer& buffer) const {
    buffer.WriteUint64(time_);
    name_.Serialize(buffer);
    buffer.WriteFloat(value_);
  }

  virtual void Deserialize(Buffer& buffer) {
    time_ = buffer.ReadUint64();
    name_.Deserialize(buffer);
    value_ = buffer.ReadFloat();
  }

 private:
  uint64_t time_;
  StringOrId name_;
  float value_;
};

// C++98 compatible DeepPushReq
class DeepPushReq : public DeepMessage {
 public:
  DeepPushReq(uint64_t time, int threadId, const StringOrId& name) : DeepMessage(time, threadId, name) {}

  virtual MessageType Type() const { return kDeepPushReq; }
};

// C++98 compatible DeepPopReq
class DeepPopReq : public DeepMessage {
 public:
  DeepPopReq(uint64_t time, int threadId, const StringOrId& name) : DeepMessage(time, threadId, name) {}

  virtual MessageType Type() const { return kDeepPopReq; }
};

// C++98 compatible EmptyMessageWrapper
class EmptyMessageWrapper : public Message {
 public:
  EmptyMessageWrapper(MessageType type) : type_(type) {}
  virtual MessageType Type() const { return type_; }
  virtual void Serialize(Buffer& buffer) const {}
  virtual void Deserialize(Buffer& buffer) {}

 private:
  MessageType type_;
};

// C++98 compatible Block structure
struct Block {
  int capacity;
  int cursor;
  uint8_t* data;  // 使用指针而不是数组

  inline int available() const { return capacity - cursor; }
  
  Block() : capacity(0), cursor(0), data(0) {}
};

// C++98 compatible memory management
static void ReleaseBlock(Block* block) { 
  if (block) {
    if (block->data) {
      delete[] block->data;
    }
    delete block;
  }
}

// C++98 compatible smart pointer replacement
class BlockPtr {
 public:
  BlockPtr() : block_(0), deleter_(ReleaseBlock) {}
  explicit BlockPtr(Block* block) : block_(block), deleter_(ReleaseBlock) {}
  
  // 拷贝构造函数
  BlockPtr(const BlockPtr& other) : block_(0), deleter_(ReleaseBlock) {
    if (other.block_) {
      // 创建新的Block对象
      block_ = new Block();
      block_->capacity = other.block_->capacity;
      block_->cursor = other.block_->cursor;
      block_->data = new uint8_t[other.block_->capacity];
      memcpy(block_->data, other.block_->data, other.block_->cursor);
    }
  }
  
  // 赋值操作符
  BlockPtr& operator=(const BlockPtr& other) {
    if (this != &other) {
      // 释放当前资源
      if (block_) {
        deleter_(block_);
      }
      
      // 复制新资源
      block_ = 0;
      if (other.block_) {
        block_ = new Block();
        block_->capacity = other.block_->capacity;
        block_->cursor = other.block_->cursor;
        block_->data = new uint8_t[other.block_->capacity];
        memcpy(block_->data, other.block_->data, other.block_->cursor);
      }
    }
    return *this;
  }
  
  ~BlockPtr() {
    if (block_) {
      deleter_(block_);
    }
  }
  
  Block* get() const { return block_; }
  Block* operator->() const { return block_; }
  Block& operator*() const { return *block_; }
  
  void reset(Block* block) {
    if (block_) {
      deleter_(block_);
    }
    block_ = block;
  }
  
  Block* release() {
    Block* temp = block_;
    block_ = 0;
    return temp;
  }
  
  bool operator!() const { return !block_; }
  operator bool() const { return block_ != 0; }

 private:
  Block* block_;
  void (*deleter_)(Block*);
};

static BlockPtr AllocBlock() {
  Block* block = new Block();
  block->data = new uint8_t[kBlockSize];
  block->cursor = 0;
  block->capacity = kBlockSize;
  return BlockPtr(block);
}

// C++98 compatible error logging
static void ErrorLog(const char* format, ...) {
  char output[256];
  va_list vargs;
  va_start(vargs, format);
  vsnprintf(output, sizeof(output), format, vargs);
  
  #if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_ERROR, "PerfDogExtension", "%s", output);
  #elif defined(__APPLE__)
    NSLog(@"PerfDogExtension: %s", output);
  #elif defined(_WIN32) || defined(_GAMING_XBOX)
    puts(output);
    OutputDebugStringA(output);
  #else
    printf("%s\n", output);
  #endif
  
  va_end(vargs);
}

// C++98 compatible thread replacement
class Thread {
 public:
  Thread() : thread_(0), running_(false) {}
  
  template<typename Func>
  Thread(const Func& func) : thread_(0), running_(false) {
    start(func);
  }
  
  ~Thread() {
    if (running_ && thread_) {
      join();
    }
  }
  
  template<typename Func>
  void start(const Func& func) {
    if (!running_) {
      running_ = true;
      #if defined(__ANDROID__) || defined(__APPLE__)
        pthread_create(&thread_, 0, &Thread::thread_func<Func>, new Func(func));
      #elif defined(_WIN32)
        thread_ = reinterpret_cast<void*>(_beginthreadex(0, 0, &Thread::thread_func_win<Func>, new Func(func), 0, 0));
      #endif
    }
  }
  
  void join() {
    if (running_ && thread_) {
      #if defined(__ANDROID__) || defined(__APPLE__)
        pthread_join(thread_, 0);
      #elif defined(_WIN32)
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
      #endif
      running_ = false;
    }
  }
  
  void detach() {
    if (running_ && thread_) {
      #if defined(__ANDROID__) || defined(__APPLE__)
        pthread_detach(thread_);
      #elif defined(_WIN32)
        CloseHandle(thread_);
      #endif
      running_ = false;
    }
  }

 private:
  #if defined(__ANDROID__) || defined(__APPLE__)
    pthread_t thread_;
  #elif defined(_WIN32)
    void* thread_;
  #endif
  bool running_;
  
  template<typename Func>
  static void* thread_func(void* arg) {
    Func* func = static_cast<Func*>(arg);
    (*func)();
    delete func;
    return 0;
  }
  
  #if defined(_WIN32)
  template<typename Func>
  static unsigned __stdcall thread_func_win(void* arg) {
    Func* func = static_cast<Func*>(arg);
    (*func)();
    delete func;
    return 0;
  }
  #endif
  
  // Disable copy
  Thread(const Thread&);
  Thread& operator=(const Thread&);
};

// C++98 compatible mutex replacement
class Mutex {
 public:
  Mutex() {
    #if defined(__ANDROID__) || defined(__APPLE__)
      pthread_mutex_init(&mutex_, 0);
    #elif defined(_WIN32)
      InitializeCriticalSection(&cs_);
    #endif
  }
  
  ~Mutex() {
    #if defined(__ANDROID__) || defined(__APPLE__)
      pthread_mutex_destroy(&mutex_);
    #elif defined(_WIN32)
      DeleteCriticalSection(&cs_);
    #endif
  }
  
  void lock() {
    #if defined(__ANDROID__) || defined(__APPLE__)
      pthread_mutex_lock(&mutex_);
    #elif defined(_WIN32)
      EnterCriticalSection(&cs_);
    #endif
  }
  
  void unlock() {
    #if defined(__ANDROID__) || defined(__APPLE__)
      pthread_mutex_unlock(&mutex_);
    #elif defined(_WIN32)
      LeaveCriticalSection(&cs_);
    #endif
  }

 private:
  #if defined(__ANDROID__) || defined(__APPLE__)
    pthread_mutex_t mutex_;
  #elif defined(_WIN32)
    CRITICAL_SECTION cs_;
  #endif
  
  // Disable copy
  Mutex(const Mutex&);
  Mutex& operator=(const Mutex&);
};

// C++98 compatible lock guard
class LockGuard {
 public:
  explicit LockGuard(Mutex& mutex) : mutex_(mutex) {
    mutex_.lock();
  }
  
  ~LockGuard() {
    mutex_.unlock();
  }

 private:
  Mutex& mutex_;
  
  // Disable copy
  LockGuard(const LockGuard&);
  LockGuard& operator=(const LockGuard&);
};

// C++98 compatible atomic bool replacement
class AtomicBool {
 public:
  AtomicBool() : value_(false) {}
  explicit AtomicBool(bool value) : value_(value) {}
  
  bool load() const {
    #if defined(__ANDROID__) || defined(__APPLE__)
      return __sync_fetch_and_add(&value_, 0) != 0;  // 使用fetch_and_add(0)来读取值
    #elif defined(_WIN32)
      return InterlockedAdd(&value_, 0) != 0;  // 使用InterlockedAdd(0)来读取值
    #endif
  }
  
  void store(bool value) {
    #if defined(__ANDROID__) || defined(__APPLE__)
      __sync_lock_test_and_set(&value_, value ? 1 : 0);
    #elif defined(_WIN32)
      InterlockedExchange(&value_, value ? 1 : 0);
    #endif
  }
  
  operator bool() const { return load(); }

 private:
  mutable volatile long value_;  // Use long for Windows compatibility
};

// 全局时间频率变量
#if defined(__APPLE__)
static uint64_t g_time_freq_ = 0;
#elif defined(_WIN32)
static LARGE_INTEGER g_time_freq_;
static bool g_time_freq_initialized = false;
#endif

// C++98 compatible time functions
static uint64_t CurrentTimeMs() {
  #if defined(__ANDROID__)
    timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
      perror("clock_gettime:");
      return 0;
    }
    return t.tv_sec * (uint64_t)1e3 + t.tv_nsec / 1000000;
  #elif defined(__APPLE__)
    if (g_time_freq_ == 0) {
      mach_timebase_info_data_t Info;
      mach_timebase_info(&Info);
      g_time_freq_ = (uint64_t(1 * 1000 * 1000) * uint64_t(Info.denom)) / uint64_t(Info.numer);
    }
    uint64_t now = mach_absolute_time();
    return now / g_time_freq_;
  #elif defined(_WIN32)
    if (!g_time_freq_initialized) {
      QueryPerformanceFrequency(&g_time_freq_);
      g_time_freq_initialized = true;
    }
    LARGE_INTEGER time;
    QueryPerformanceCounter(&time);
    return time.QuadPart * 1000 / g_time_freq_.QuadPart;
  #endif
}

static int CurrentThreadId() {
  #if defined(__ANDROID__)
    return gettid();
  #elif defined(__APPLE__)
    return (int)pthread_mach_thread_np(pthread_self());
  #elif defined(_WIN32)
    return GetCurrentThreadId();
  #endif
}

// C++98 compatible sleep function
static void SleepMs(int milliseconds) {
  #if defined(__ANDROID__) || defined(__APPLE__)
    usleep(milliseconds * 1000);
  #elif defined(_WIN32)
    Sleep(milliseconds);
  #endif
}

// Main PerfDogExtension class (simplified for C++98)
class PerfDogExtension {
 public:
  PerfDogExtension() : stop_(false), start_test_(false), initialized_(false), initialize_result_(0), string_id_(0), writer_(0) {}

  virtual ~PerfDogExtension() { 
    StopTest(); 
  }

  int Init() {
    LockGuard lock_guard(lock_);

    if (!initialized_) {
      initialized_ = true;
      initialize_result_ = StartServer();
      ErrorLog("init return %d", initialize_result_);
    }
    return initialize_result_;
  }

  void PostValueF(const std::string& category, const std::string& key, float a) { 
    std::vector<float> values;
    values.push_back(a);
    PostValue(category, key, values); 
  }
  
  void PostValueF(const std::string& category, const std::string& key, float a, float b) {
    std::vector<float> values;
    values.push_back(a);
    values.push_back(b);
    PostValue(category, key, values);
  }
  
  void PostValueF(const std::string& category, const std::string& key, float a, float b, float c) {
    std::vector<float> values;
    values.push_back(a);
    values.push_back(b);
    values.push_back(c);
    PostValue(category, key, values);
  }
  
  void PostValueI(const std::string& category, const std::string& key, int a) { 
    std::vector<int32_t> values;
    values.push_back(a);
    PostValue(category, key, values); 
  }
  
  void PostValueI(const std::string& category, const std::string& key, int a, int b) {
    std::vector<int32_t> values;
    values.push_back(a);
    values.push_back(b);
    PostValue(category, key, values);
  }
  
  void PostValueI(const std::string& category, const std::string& key, int a, int b, int c) {
    std::vector<int32_t> values;
    values.push_back(a);
    values.push_back(b);
    values.push_back(c);
    PostValue(category, key, values);
  }
  
  void PostValue(const std::string& category, const std::string& key, const std::vector<float>& values) {
    if (IsStartTest()) {
      CustomFloatValue message(CurrentTimeMs(), StringToId(category), StringToId(key), values);
      WriteMessage(message);
    }
  }
  
  void PostValue(const std::string& category, const std::string& key, const std::vector<int32_t>& values) {
    if (IsStartTest()) {
      CustomIntegerValue message(CurrentTimeMs(), StringToId(category), StringToId(key), values);
      WriteMessage(message);
    }
  }
  
  void PostValueS(const std::string& category, const std::string& key, const std::string& value) {
    if (IsStartTest()) {
      CustomStringValue message(CurrentTimeMs(), StringToId(category), StringToId(key), StringOrId(value));
      WriteMessage(message);
    }
  }

  void setLabel(const std::string& name) {
    if (IsStartTest()) {
      SetLabelReq message(CurrentTimeMs(), name);
      WriteMessage(message);
    }
  }

  void addNote(const std::string& name) {
    if (IsStartTest()) {
      AddNoteReq message(CurrentTimeMs(), name);
      WriteMessage(message);
    }
  }

  void DeepMark(const std::string& name) {
    if (IsStartTest()) {
      DeepMarkReq message(CurrentTimeMs(), CurrentThreadId(), StringToId(name));
      WriteMessage(message);
    }
  }

  void DeepCounter(const std::string& name, float value) {
    if (IsStartTest()) {
      DeepCounterReq message(CurrentTimeMs(), StringOrId(name), value);
      WriteMessage(message);
    }
  }

  void DeepPush(const std::string& name) {
    if (IsStartTest()) {
      DeepPushReq message(CurrentTimeMs(), CurrentThreadId(), StringToId(name));
      WriteMessage(message);
    }
  }

  void DeepPop(const std::string& name) {
    if (IsStartTest()) {
      DeepPopReq message(CurrentTimeMs(), CurrentThreadId(), StringToId(name));
      WriteMessage(message);
    }
  }

  bool IsStartTest() { return start_test_.load(); }

 protected:
  void OnConnect() { SendStreamHeader(); }

  void OnMessage(uint16_t type, const Message* message) {
    switch (type) {
      case kStartTestReq: {
        StartTest();
        break;
      }
      case kStopTestReq: {
        StopTest();
        break;
      }
      default:
        ErrorLog("unknown message:%u", type);
    }
  }

  void OnDisconnect() { StopTest(); }

  virtual int StartServer() = 0;
  virtual void StopServer() = 0;
  virtual int SendData(const void* buf, int size) = 0;

 private:
  void StartTest() {
    LockGuard lock_guard(lock_);

    if (!writer_) {
      stop_.store(false);
      start_test_.store(true);
      ClearData();
      ThreadFunc func(this, &PerfDogExtension::DrainBuffer);
      writer_ = new Thread(func);
    }
    WriteMessage(EmptyMessageWrapper(kStartTestRsp));
    ErrorLog("start test");
  }

  void StopTest() {
    LockGuard lock_guard(lock_);

    if (writer_) {
      stop_.store(true);
      start_test_.store(false);
      writer_->join();
      delete writer_;
      writer_ = 0;
      ClearData();
      ErrorLog("stop test");
    }
  }

  void ClearData() {
    LockGuard lock_guard(buffer_lock_);
    LockGuard lock_guard2(string_id_map_lock_);

    block_list_.clear();
    string_id_map_.clear();
  }

  int WriteData(const void* buf, int size) {
    LockGuard lock_guard(buffer_lock_);

    if (block_list_.empty() || block_list_.front()->available() < size) {
      block_list_.push_front(AllocBlock());
    }
    BlockPtr& block = block_list_.front();

    uint8_t* data_ptr = block->data + block->cursor;
    memcpy(data_ptr, buf, size);
    block->cursor += size;

    return 0;
  }

  void DrainBuffer() {
    int loop_count = 0;
    
    while (!stop_.load()) {
      loop_count++;
      bool stop_value = stop_.load();
      
      if (stop_value) {
        break;
      }

      std::list<BlockPtr> commit_list;

      {
        LockGuard lock_guard(buffer_lock_);
        commit_list.swap(block_list_);
      }

      commit_list.reverse();
      for (std::list<BlockPtr>::iterator it = commit_list.begin(); it != commit_list.end(); ++it) {
        if (it->get()->cursor > 0) {
          Block* block = it->get();
          if (block) {
            if (block->data) {
              SendData(block->data, block->cursor);
            } else {
              ErrorLog("ERROR: block->data is NULL!");
            }
          } else {
            ErrorLog("ERROR: block pointer is NULL!");
          }
        }
      }

      SleepMs(100);
    }
  }

  void SendStreamHeader() {
    Header header;
    SendData(&header, sizeof(header));
  }

  void WriteMessage(const Message& message) {
    char temp[kMessageHeaderLength + kMaxMessageLength];

    Buffer buffer(temp + kMessageHeaderLength, sizeof(temp) - kMessageHeaderLength);
    message.Serialize(buffer);

    MessageHeader* message_header = reinterpret_cast<MessageHeader*>(temp);
    message_header->length = static_cast<uint16_t>(buffer.DataSize());
    message_header->type = message.Type();

    WriteData(temp, kMessageHeaderLength + buffer.DataSize());
  }

  uint32_t StringToId(const std::string& str) {
    LockGuard lock(string_id_map_lock_);

    std::map<std::string, uint32_t>::iterator ite = string_id_map_.find(str);
    if (ite == string_id_map_.end()) {
      uint32_t id = ++string_id_;
      string_id_map_.insert(std::make_pair(str, id));

      StringMap string_map(id, str);
      WriteMessage(string_map);
      return id;
    } else {
      return ite->second;
    }
  }

  // Thread function wrapper for C++98
  class ThreadFunc {
   public:
    ThreadFunc(PerfDogExtension* obj, void (PerfDogExtension::*func)()) 
        : obj_(obj), func_(func) {}
    
    void operator()() {
      (obj_->*func_)();
    }
    
   private:
    PerfDogExtension* obj_;
    void (PerfDogExtension::*func_)();
  };

 private:
  bool initialized_;
  int initialize_result_;
  Thread* writer_;
  Mutex lock_;
  AtomicBool stop_;
  AtomicBool start_test_;
  std::list<BlockPtr> block_list_;
  Mutex buffer_lock_;
  std::map<std::string, uint32_t> string_id_map_;
  Mutex string_id_map_lock_;
  uint32_t string_id_;
};

}  // namespace perfdog

// Platform specific implementations
namespace perfdog {

// C++98 compatible socket wrapper
class Socket {
 public:
  Socket() : fd_(-1) {}
  
  #if defined(__ANDROID__) || defined(__APPLE__)
    explicit Socket(int fd) : fd_(fd) {}
  #elif defined(_WIN32)
    explicit Socket(SOCKET fd) : fd_(static_cast<int>(fd)) {}
  #endif
  
  ~Socket() {
    close();
  }
  
  bool isValid() const { return fd_ != -1; }
  
  int getFd() const { return fd_; }
  
  void close() {
    if (fd_ != -1) {
      #if defined(__ANDROID__) || defined(__APPLE__)
        ::close(fd_);
      #elif defined(_WIN32)
        closesocket(static_cast<SOCKET>(fd_));
      #endif
      fd_ = -1;
    }
  }
  
  void reset(int fd) {
    close();
    fd_ = fd;
  }

 private:
  int fd_;
  
  // Disable copy
  Socket(const Socket&);
  Socket& operator=(const Socket&);
};

// C++98 compatible PerfDogExtension implementation
class PerfDogExtensionImpl : public PerfDogExtension {
 public:
  PerfDogExtensionImpl() : server_socket_(), client_socket_(), server_thread_(0), server_running_(false) {
    #if defined(_WIN32)
      WSADATA wsa_data;
      WSAStartup(MAKEWORD(2, 2), &wsa_data);
    #endif
  }
  
  ~PerfDogExtensionImpl() {
    StopServer();
    #if defined(_WIN32)
      WSACleanup();
    #endif
  }

 protected:
  virtual int StartServer() {
    LockGuard lock_guard(server_mutex_);
    
    if (server_running_) return 0;
    
    // Create server socket
    #if defined(__ANDROID__) || defined(__APPLE__)
      int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    #elif defined(_WIN32)
      SOCKET server_fd = socket(AF_INET, SOCK_STREAM, 0);
    #endif
    
    if (server_fd == -1 || server_fd == INVALID_SOCKET) {
      ErrorLog("Failed to create socket");
      return 1;
    }
    
    server_socket_.reset(static_cast<int>(server_fd));
    
    // Set socket options
    int opt = 1;
    #if defined(__ANDROID__) || defined(__APPLE__)
      setsockopt(server_socket_.getFd(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
      
      #if defined(SO_NOSIGPIPE)
        int on = 1;
        setsockopt(server_socket_.getFd(), SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
      #endif
    #elif defined(_WIN32)
      setsockopt(static_cast<SOCKET>(server_socket_.getFd()), SOL_SOCKET, SO_REUSEADDR, 
                 reinterpret_cast<const char*>(&opt), sizeof(opt));
    #endif
    
    // Bind socket
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53000);  // Default port
    addr.sin_addr.s_addr = INADDR_ANY;
    
    #if defined(__ANDROID__) || defined(__APPLE__)
      if (bind(server_socket_.getFd(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == -1) {
    #elif defined(_WIN32)
      if (bind(static_cast<SOCKET>(server_socket_.getFd()), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    #endif
        ErrorLog("Failed to bind socket");
        return 1;
      }
    
    // Listen for connections
    #if defined(__ANDROID__) || defined(__APPLE__)
      if (listen(server_socket_.getFd(), 1) == -1) {
    #elif defined(_WIN32)
      if (listen(static_cast<SOCKET>(server_socket_.getFd()), 1) == SOCKET_ERROR) {
    #endif
        ErrorLog("Failed to listen on socket");
        return 1;
      }
    
    server_running_ = true;
    class ServerThreadFunc func(this);
    server_thread_ = new Thread(func);
    
    return 0;
  }
  
  virtual void StopServer() {
    LockGuard lock_guard(server_mutex_);
    
    if (server_running_) {
      server_running_ = false;
      
      if (server_thread_) {
        server_thread_->join();
        delete server_thread_;
        server_thread_ = 0;
      }
      
      server_socket_.close();
      client_socket_.close();
    }
  }
  
  virtual int SendData(const void* buf, int size) {
    LockGuard lock_guard(client_mutex_);
    
    if (!client_socket_.isValid()) {
      return 0;
    }
    
    int sent = 0;
    const char* data = static_cast<const char*>(buf);
    
    while (sent < size) {
      #if defined(__ANDROID__) || defined(__APPLE__)
        int result = send(client_socket_.getFd(), data + sent, size - sent, 0);
        if (result == -1) {
          if (CAUSE_BY_SIGNAL()) {
            continue;
          }
          break;
        }
      #elif defined(_WIN32)
        int result = send(static_cast<SOCKET>(client_socket_.getFd()), data + sent, size - sent, 0);
        if (result == SOCKET_ERROR) {
          if (CAUSE_BY_SIGNAL()) {
            continue;
          }
          break;
        }
      #endif
      
      if (result <= 0) {
        break;
      }
      
      sent += result;
    }
    
    return sent;
  }

 private:
  void ProcessClientMessage() {
    while (server_running_) {
      // Check for new connections
      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(server_socket_.getFd(), &readfds);
      
      struct timeval timeout;
      timeout.tv_sec = 0;
      timeout.tv_usec = 100000;  // 100ms
      
      #if defined(__ANDROID__) || defined(__APPLE__)
        int result = select(server_socket_.getFd() + 1, &readfds, 0, 0, &timeout);
      #elif defined(_WIN32)
        int result = select(0, &readfds, 0, 0, &timeout);
      #endif
      
      if (result > 0 && FD_ISSET(server_socket_.getFd(), &readfds)) {
        // Accept new connection
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        
        #if defined(__ANDROID__) || defined(__APPLE__)
          int client_fd = accept(server_socket_.getFd(), reinterpret_cast<struct sockaddr*>(&client_addr), &client_addr_len);
        #elif defined(_WIN32)
          SOCKET client_fd = accept(static_cast<SOCKET>(server_socket_.getFd()), reinterpret_cast<struct sockaddr*>(&client_addr), &client_addr_len);
        #endif
        
        if (client_fd != -1 && client_fd != INVALID_SOCKET) {
          {
            LockGuard lock_guard(client_mutex_);
            client_socket_.reset(static_cast<int>(client_fd));
            ErrorLog("Client connected");
          }
          
          OnConnect();
          ReadData();
          
          {
            LockGuard lock_guard(client_mutex_);
            client_socket_.close();
            ErrorLog("Client disconnected");
          }
          
          OnDisconnect();
        }
      }
    }
  }
  
  void ReadData() {
    char buffer[4096];
    
    while (server_running_) {
      #if defined(__ANDROID__) || defined(__APPLE__)
        int received = recv(client_socket_.getFd(), buffer, sizeof(buffer), 0);
        if (received == -1) {
          if (CAUSE_BY_SIGNAL()) {
            continue;
          }
          break;
        }
      #elif defined(_WIN32)
        int received = recv(static_cast<SOCKET>(client_socket_.getFd()), buffer, sizeof(buffer), 0);
        if (received == SOCKET_ERROR) {
          break;
        }
      #endif
      
      if (received <= 0) {
        break;
      }
      
      // Process received data
      ProcessReceivedData(buffer, received);
    }
  }
  
  void ProcessReceivedData(const char* data, int size) {
    // Simple message processing - in a real implementation, you'd parse the protocol
    if (size >= 4) {
      uint16_t* msg_type = reinterpret_cast<uint16_t*>(const_cast<char*>(data + 2));
      if (*msg_type == kStartTestReq) {
        OnMessage(kStartTestReq, 0);
      } else if (*msg_type == kStopTestReq) {
        OnMessage(kStopTestReq, 0);
      }
    }
  }
  
  // Thread function wrapper for server thread
  class ServerThreadFunc {
   public:
    ServerThreadFunc(PerfDogExtensionImpl* obj) : obj_(obj) {}
    
    void operator()() {
      obj_->ProcessClientMessage();
    }
    
   private:
    PerfDogExtensionImpl* obj_;
  };

 private:
  Socket server_socket_;
  Socket client_socket_;
  Thread* server_thread_;
  bool server_running_;
  Mutex server_mutex_;
  Mutex client_mutex_;
};

}  // namespace perfdog

// External interface functions
namespace perfdog {

// Simple singleton implementation for C++98
static PerfDogExtension* g_instance = 0;
static Mutex g_instance_mutex;

static PerfDogExtension& GetPerfDogExtensionInstance() {
  LockGuard lock(g_instance_mutex);
  if (!g_instance) {
    // Create the concrete implementation
    g_instance = new PerfDogExtensionImpl();
  }
  return *g_instance;
}

int EnableSendToPerfDog() { 
  return GetPerfDogExtensionInstance().Init(); 
}

bool IsStartTest() { 
  return GetPerfDogExtensionInstance().IsStartTest(); 
}

void PostValueF(const std::string& category, const std::string& key, float a) {
  GetPerfDogExtensionInstance().PostValueF(category, key, a);
}

void PostValueF(const std::string& category, const std::string& key, float a, float b) {
  GetPerfDogExtensionInstance().PostValueF(category, key, a, b);
}

void PostValueF(const std::string& category, const std::string& key, float a, float b, float c) {
  GetPerfDogExtensionInstance().PostValueF(category, key, a, b, c);
}

void PostValueI(const std::string& category, const std::string& key, int a) {
  GetPerfDogExtensionInstance().PostValueI(category, key, a);
}

void PostValueI(const std::string& category, const std::string& key, int a, int b) {
  GetPerfDogExtensionInstance().PostValueI(category, key, a, b);
}

void PostValueI(const std::string& category, const std::string& key, int a, int b, int c) {
  GetPerfDogExtensionInstance().PostValueI(category, key, a, b, c);
}

void PostValueS(const std::string& category, const std::string& key, const std::string& value) {
  GetPerfDogExtensionInstance().PostValueS(category, key, value);
}

void setLabel(const std::string& name) { 
  GetPerfDogExtensionInstance().setLabel(name); 
}

void addNote(const std::string& name) { 
  GetPerfDogExtensionInstance().addNote(name); 
}

void DeepMark(const std::string& name) { 
  GetPerfDogExtensionInstance().DeepMark(name); 
}

void DeepCounter(const std::string& name, float value) { 
  GetPerfDogExtensionInstance().DeepCounter(name, value); 
}

void DeepPush(const std::string& name) { 
  GetPerfDogExtensionInstance().DeepPush(name); 
}

void DeepPop(const std::string& name) { 
  GetPerfDogExtensionInstance().DeepPop(name); 
}

}  // namespace perfdog

#endif  // PERFDOG_EXTENSION_ENABLE
#endif 