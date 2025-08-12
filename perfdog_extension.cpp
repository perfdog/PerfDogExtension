// Copyright 2022 Tencent Inc. All rights reserved.
//
// Author: PerfDog@tencent.com
// Version: 1.3

#if defined(__ANDROID__) || defined(__APPLE__) || defined(_WIN32) || defined(_GAMING_XBOX)

#include "perfdog_extension.h"

#ifdef PERFDOG_EXTENSION_ENABLE

#include <atomic>
#include <chrono>
#include <forward_list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <stdint.h>
#include <stdlib.h>
#include <sstream>

#ifdef _WIN32
#pragma warning(default : 4018)
#endif

namespace perfdog {

/**
 * Protocol
 * [Header][ [MessageHeader][Message] ... [MessageHeader][Message] ]
 * Workflow:
 * Client                          PerfDogExtension
 *                                  bind
 *                                  listen
 * connect                  -->
 *                                  accept
 *                          <--     Header
 * MessageHeader            -->
 * StartTestReq             -->
 *                          <--     MessageHeader
 *                          <--     StartTestRsp
 *                          <--     MessageHeader
 *                          <--     StringMap
 *                          <--     MessageHeader
 *                          <--     CustomIntegerValue
 *                          ...
 * MessageHeader            -->
 * StopTestReq              -->
 *                          <--     MessageHeader
 *                          <--     StopTestRsp
 *
 * note:StopTestReq may not be sent, directly received EOF
 */
/**
 * 协议
 * [Header][ [MessageHeader][Message] ... [MessageHeader][Message] ]
 * 流程:
 * 客户端                          PerfDog拓展
 *                                  bind
 *                                  listen
 * connect                  -->
 *                                  accept
 *                          <--     Header
 * MessageHeader            -->
 * StartTestReq             -->
 *                          <--     MessageHeader
 *                          <--     StartTestRsp
 *                          <--     MessageHeader
 *                          <--     StringMap
 *                          <--     MessageHeader
 *                          <--     CustomIntegerValue
 *                          ...
 * MessageHeader            -->
 * StopTestReq              -->
 *                          <--     MessageHeader
 *                          <--     StopTestRsp
 *
 * note:StopTestReq可能会发不过来,直接收到了EOF
 */
struct Header {
  static constexpr uint32_t kMagicNumber = 'PDEX';
  static constexpr uint32_t kProtocolVersion = 1;

  uint32_t magic_number = kMagicNumber;
  uint32_t version = kProtocolVersion;
};

static constexpr int kMaxMessageLength = 4096;

struct MessageHeader {
  // 0 means the message is empty, if not, the next 'length' bytes are the content of the message.
  // 0表示Message是空的,不为0后面的length个字节是Message的内容
  uint16_t length;

  uint16_t type;
};

static constexpr int kMessageHeaderLength = sizeof(MessageHeader);

enum MessageType : uint16_t {
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
    WriteUint32(value.size());
    if (!value.empty()) {
      WriteBytes((void*)value.data(), value.size());
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
    if (pos_ + size > capacity_) {
      is_overflow_ = true;
      printf("overflow\n");
      return;
    }

    memcpy(buf_ + pos_, data, size);
    pos_ += size;
  }

  void ReadBytes(void* data, uint32_t size) {
    if (is_overflow_) return;
    if (pos_ + size > capacity_) {
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

class Message {
 public:
  virtual MessageType Type() const = 0;
  virtual void Serialize(Buffer& buffer) const = 0;
  virtual void Deserialize(Buffer& buffer) = 0;
};

class StringMap : public Message {
 public:
  StringMap() = default;
  StringMap(uint32_t id, std::string name) : id_(id), name_(std::move(name)) {}

  MessageType Type() const override { return kStringMap; };

  uint32_t Id() const { return id_; }

  const std::string& Name() const { return name_; }

  void Serialize(Buffer& buffer) const override {
    buffer.WriteUint32(id_);
    buffer.WriteString(name_);
  }

  void Deserialize(Buffer& buffer) override {
    id_ = buffer.ReadUint32();
    buffer.ReadString(name_);
  }

 private:
  uint32_t id_;
  std::string name_;
};

// Non-English characters require UTF-8 encoding
// 非英文字符需要UTF-8编码
class StringOrId : public Message {
 public:
  static constexpr uint32_t kInvalidStringId = 0xffffffff;

  StringOrId() : id_(kInvalidStringId) {}

  StringOrId(uint32_t id) : id_(id) {}

  StringOrId(std::string string) : id_(kInvalidStringId), string_(std::move(string)) {}

  StringOrId(StringOrId&& other) {
    id_ = other.id_;
    other.id_ = kInvalidStringId;
    string_ = std::move(other.string_);
  }

  bool IsId() const { return id_ != kInvalidStringId; }

  uint32_t GetId() const { return id_; }

  const std::string& GetString() const { return string_; }

  MessageType Type() const override { return kInvalid; }

  void Serialize(Buffer& buffer) const override {
    buffer.WriteUint32(id_);
    buffer.WriteString(string_);
  }

  void Deserialize(Buffer& buffer) override {
    id_ = buffer.ReadUint32();
    buffer.ReadString(string_);
  }

 private:
  uint32_t id_;
  std::string string_;
};

class CustomValue : public Message {
 public:
  CustomValue() = default;
  CustomValue(uint64_t time, StringOrId category, StringOrId key_name)
      : time_(time), category_(std::move(category)), key_name_(std::move(key_name)) {}

  MessageType Type() const override { return kInvalid; };

  void Serialize(Buffer& buffer) const override {
    buffer.WriteUint64(time_);
    category_.Serialize(buffer);
    key_name_.Serialize(buffer);
  }

  void Deserialize(Buffer& buffer) override {
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

class CustomFloatValue : public CustomValue {
 public:
  CustomFloatValue() = default;
  CustomFloatValue(uint64_t time, StringOrId category, StringOrId key_name, std::vector<float> values)
      : CustomValue(time, std::move(category), std::move(key_name)), values_(std::move(values)) {}

  MessageType Type() const override { return kCustomFloatValue; }

  void Serialize(Buffer& buffer) const override {
    CustomValue::Serialize(buffer);
    buffer.WriteUint32(values_.size());
    for (float value : values_) {
      buffer.WriteFloat(value);
    }
  }

  void Deserialize(Buffer& buffer) override {
    CustomValue::Deserialize(buffer);
    uint32_t size = buffer.ReadUint32();
    if (size > 0) {
      values_.reserve(size);
      for (int i = 0; i < size; ++i) {
        values_.push_back(buffer.ReadFloat());
      }
    }
  }

 private:
  std::vector<float> values_;
};

class CustomIntegerValue : public CustomValue {
 public:
  CustomIntegerValue() = default;

  CustomIntegerValue(uint64_t time, StringOrId category, StringOrId key_name, std::vector<int32_t> values)
      : CustomValue(time, std::move(category), std::move(key_name)), values_(std::move(values)) {}

  MessageType Type() const override { return kCustomIntegerValue; };

  void Serialize(Buffer& buffer) const override {
    CustomValue::Serialize(buffer);
    buffer.WriteUint32(values_.size());
    for (int32_t value : values_) {
      buffer.WriteInt32(value);
    }
  }

  void Deserialize(Buffer& buffer) override {
    CustomValue::Deserialize(buffer);
    uint32_t size = buffer.ReadUint32();
    if (size > 0) {
      values_.reserve(size);
      for (int i = 0; i < size; ++i) {
        values_.push_back(buffer.ReadInt32());
      }
    }
  }

  const std::vector<int32_t>& GetValues() const { return values_; }

 private:
  std::vector<int32_t> values_;
};

class CustomStringValue : public CustomValue {
 public:
  CustomStringValue() = default;

  CustomStringValue(uint64_t time, StringOrId category, StringOrId key_name, StringOrId value)
      : CustomValue(time, std::move(category), std::move(key_name)), value_(std::move(value)) {}

  MessageType Type() const override { return kCustomStringValue; };

  void Serialize(Buffer& buffer) const override {
    CustomValue::Serialize(buffer);
    value_.Serialize(buffer);
  }

  void Deserialize(Buffer& buffer) override {
    CustomValue::Deserialize(buffer);
    value_.Deserialize(buffer);
  }

 private:
  StringOrId value_;
};

class SetLabelReq : public Message {
 public:
  SetLabelReq(uint64_t time, const std::string& name) : time_(time), name_(name) {}

  MessageType Type() const override { return kSetLabelReq; }

  void Serialize(Buffer& buffer) const override {
    buffer.WriteUint64(time_);
    buffer.WriteString(name_);
  }

  void Deserialize(Buffer& buffer) override {
    time_ = buffer.ReadUint64();
    buffer.ReadString(name_);
  }

 private:
  uint64_t time_;
  std::string name_;
};

class AddNoteReq : public Message {
 public:
  AddNoteReq(uint64_t time, const std::string& name) : time_(time), name_(name) {}

  MessageType Type() const override { return kAddNoteReq; }

  void Serialize(Buffer& buffer) const override {
    buffer.WriteUint64(time_);
    buffer.WriteString(name_);
  }

  void Deserialize(Buffer& buffer) override {
    time_ = buffer.ReadUint64();
    buffer.ReadString(name_);
  }

 private:
  uint64_t time_;
  std::string name_;
};

class DeepMessage : public Message {
 public:
  DeepMessage(uint64_t time, int threadId, StringOrId name)
      : time_(time), thread_id_(threadId), name_(std::move(name)) {}

  virtual MessageType Type() const override = 0;

  void Serialize(Buffer& buffer) const override {
    buffer.WriteUint64(time_);
    buffer.WriteInt32(thread_id_);
    name_.Serialize(buffer);
  }

  void Deserialize(Buffer& buffer) override {
    time_ = buffer.ReadUint64();
    thread_id_ = buffer.ReadInt32();
    name_.Deserialize(buffer);
  }

 private:
  uint64_t time_;
  int thread_id_;
  StringOrId name_;
};

class DeepMarkReq : public DeepMessage {
 public:
  DeepMarkReq(uint64_t time, int threadId, StringOrId name) : DeepMessage(time, threadId, std::move(name)) {}

  MessageType Type() const override { return kDeepMarkReq; }
};

class DeepCounterReq : public Message {
 public:
  DeepCounterReq(uint64_t time, StringOrId name, float value) : time_(time), name_(std::move(name)), value_(value) {}

  MessageType Type() const override { return kDeepCounterReq; }

  void Serialize(Buffer& buffer) const override {
    buffer.WriteUint64(time_);
    name_.Serialize(buffer);
    buffer.WriteFloat(value_);
  }

  void Deserialize(Buffer& buffer) override {
    time_ = buffer.ReadUint64();
    name_.Deserialize(buffer);
    value_ = buffer.ReadFloat();
  }

 private:
  uint64_t time_;
  StringOrId name_;
  float value_;
};

class DeepPushReq : public DeepMessage {
 public:
  DeepPushReq(uint64_t time, int threadId, StringOrId name) : DeepMessage(time, threadId, std::move(name)) {}

  MessageType Type() const override { return kDeepPushReq; }
};

class DeepPopReq : public DeepMessage {
 public:
  DeepPopReq(uint64_t time, int threadId, StringOrId name) : DeepMessage(time, threadId, std::move(name)) {}

  MessageType Type() const override { return kDeepPopReq; }
};

class EmptyMessageWrapper : public Message {
 public:
  EmptyMessageWrapper(MessageType type) : type_(type) {}
  MessageType Type() const override { return type_; }
  void Serialize(Buffer& buffer) const override {}
  void Deserialize(Buffer& buffer) override {}

 private:
  MessageType type_;
};

//----------------------------------------------------------------
// Data processing
// 数据处理
//----------------------------------------------------------------
constexpr int kBlockSize = 4096;
static_assert(kBlockSize >= kMaxMessageLength, "kBlockSize is less than kMaxMessageLength");

struct Block {
  int capacity;
  int cursor;
  uint8_t data[0];

  inline int available() const { return capacity - cursor; }
};

static void ReleaseBlock(Block* block) { delete[] block; }
using BlockPtr = std::unique_ptr<Block, decltype(&ReleaseBlock)>;
static BlockPtr AllocBlock() {
  Block* block = reinterpret_cast<Block*>(new char[kBlockSize]);
  block->cursor = 0;
  block->capacity = kBlockSize - sizeof(Block);
  return {block, ReleaseBlock};
}

static void ErrorLog(const char* format, ...);
static void PrintError(const char* file, int line, const char* prefix);
#define PRINT_ERROR(prefix) PrintError(__FILE__, __LINE__, prefix)

class PlatformThread {
 public:
  PlatformThread(void (*func_ptr)(void*), void* arg) : func_ptr_(func_ptr), arg_(arg) {}

  virtual ~PlatformThread() = default;

  virtual void Join() = 0;

 protected:
  void Run() { func_ptr_(arg_); }

 private:
  void (*func_ptr_)(void*);
  void* arg_;
};

PlatformThread* CreatePlatformThread(void (*func_ptr)(void*), void* arg);

class PerfDogExtension {
 public:
  PerfDogExtension() : stop_send_(false), start_test_(false) {}

  int Init() {
    std::lock_guard<std::mutex> lock_guard(lock_);

    if (!initialized_) {
      initialized_ = true;
      initialize_result_ = StartServer();
      ErrorLog("init return %d", initialize_result_);
    }
    return initialize_result_;
  }

  virtual ~PerfDogExtension() { StopTest(); };

  void PostValueF(const std::string& category, const std::string& key, float a) { PostValue(category, key, {a}); }
  void PostValueF(const std::string& category, const std::string& key, float a, float b) {
    PostValue(category, key, {a, b});
  }
  void PostValueF(const std::string& category, const std::string& key, float a, float b, float c) {
    PostValue(category, key, {a, b, c});
  }
  void PostValueI(const std::string& category, const std::string& key, int a) { PostValue(category, key, {a}); }
  void PostValueI(const std::string& category, const std::string& key, int a, int b) {
    PostValue(category, key, {a, b});
  }
  void PostValueI(const std::string& category, const std::string& key, int a, int b, int c) {
    PostValue(category, key, {a, b, c});
  }
  void PostValue(const std::string& category, const std::string& key, std::initializer_list<float> value) {
    if (IsStartTest()) {
      CustomFloatValue message(CurrentTime(), StringToId(category), StringToId(key), value);
      WriteMessage(message);
    }
  }
  void PostValue(const std::string& category, const std::string& key, std::initializer_list<int32_t> value) {
    if (IsStartTest()) {
      CustomIntegerValue message(CurrentTime(), StringToId(category), StringToId(key), value);
      WriteMessage(message);
    }
  }
  void PostValueS(const std::string& category, const std::string& key, const std::string& value) {
    if (IsStartTest()) {
      CustomStringValue message(CurrentTime(), StringToId(category), StringToId(key), StringToId(value));
      WriteMessage(message);
    }
  }

  void setLabel(const std::string& name) {
    if (IsStartTest()) {
      SetLabelReq message(CurrentTime(), name);
      WriteMessage(message);
    }
  }

  void addNote(const std::string& name) {
    if (IsStartTest()) {
      AddNoteReq message(CurrentTime(), name);
      WriteMessage(message);
    }
  }

  void DeepMark(const std::string& name) {
    if (IsStartTest()) {
      DeepMarkReq message(CurrentTime(), CurrentThreadId(), StringToId(name));
      WriteMessage(message);
    }
  }

  void DeepCounter(const std::string& name, float value) {
    if (IsStartTest()) {
      DeepCounterReq message(CurrentTime(), StringToId(name), value);
      WriteMessage(message);
    }
  }

  void DeepPush(const std::string& name) {
    if (IsStartTest()) {
      DeepPushReq message(CurrentTime(), CurrentThreadId(), StringToId(name));
      WriteMessage(message);
    }
  }

  void DeepPop(const std::string& name) {
    if (IsStartTest()) {
      DeepPopReq message(CurrentTime(), CurrentThreadId(), StringToId(name));
      WriteMessage(message);
    }
  }

  bool IsStartTest() { return start_test_; }

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
  virtual uint64_t CurrentTime() = 0;  // 毫秒 ms
  virtual int CurrentThreadId() = 0;

 private:
  void StartTest() {
    std::lock_guard<std::mutex> lock_guard(lock_);

    if (!writer_) {
      stop_send_ = false;
      start_test_ = true;
      ClearData();
      writer_ = std::unique_ptr<PlatformThread>(
          CreatePlatformThread([](void* arg) { reinterpret_cast<PerfDogExtension*>(arg)->DrainBuffer(); }, this));
    }
    WriteMessage(EmptyMessageWrapper(kStartTestRsp));
    ErrorLog("start test");
  }

  void StopTest() {
    std::lock_guard<std::mutex> lock_guard(lock_);

    if (writer_) {
      stop_send_ = true;
      start_test_ = false;
      writer_->Join();
      writer_ = nullptr;
      ClearData();
      ErrorLog("stop test");
    }
  }

  void ClearData() {
    std::lock_guard<std::mutex> lock_guard(buffer_lock_);
    std::lock_guard<std::mutex> lock_guard2(string_id_map_lock_);

    block_list_.clear();
    string_id_map_.clear();
  }

  int WriteData(const void* buf, int size) {
    std::lock_guard<std::mutex> lock_guard(buffer_lock_);

    if (block_list_.empty() || block_list_.front()->available() < size) {
      block_list_.push_front(AllocBlock());
    }
    BlockPtr& block = block_list_.front();
    memcpy(block->data + block->cursor, buf, size);
    block->cursor += size;

    return 0;
  }

  void DrainBuffer() {
    while (!stop_send_) {
      std::forward_list<BlockPtr> commit_list;

      {
        std::lock_guard<std::mutex> lock_guard(buffer_lock_);
        std::swap(commit_list, block_list_);
      }

      commit_list.reverse();
      for (BlockPtr& block : commit_list) {
        if (block->cursor > 0) {
          SendData(reinterpret_cast<const char*>(block->data), block->cursor);
        }
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
    message_header->length = (uint16_t)buffer.DataSize();
    message_header->type = message.Type();

    WriteData(temp, kMessageHeaderLength + buffer.DataSize());
  }

  uint32_t StringToId(const std::string& str) {
    std::unique_lock<std::mutex> lock(string_id_map_lock_);

    auto ite = string_id_map_.find(str);
    if (ite == string_id_map_.end()) {
      uint32_t id = ++string_id_;
      string_id_map_.insert({str, id});

      StringMap string_map(id, str);
      WriteMessage(string_map);
      return id;
    } else {
      return ite->second;
    }
  }

 private:
  bool initialized_ = false;
  int initialize_result_ = 0;
  std::unique_ptr<PlatformThread> writer_;
  std::mutex lock_;
  std::atomic_bool stop_send_;
  std::atomic_bool start_test_;
  std::forward_list<BlockPtr> block_list_;  // 前插链表 prependant linked list
  std::mutex buffer_lock_;
  std::unordered_map<std::string, uint32_t> string_id_map_;
  std::mutex string_id_map_lock_;
  uint32_t string_id_ = 0;
};
}  // namespace perfdog

//----------------------------------------------------------------
// network transmission and reception
// 网络收发
//----------------------------------------------------------------
#if defined(__ANDROID__) || defined(__APPLE__)
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#define socklen_t int
#endif

#if defined(__ANDROID__)
#define SOCKET_SEND_FLAG MSG_NOSIGNAL
#else
#define SOCKET_SEND_FLAG 0
#endif

#if defined(__ANDROID__) || defined(__APPLE__)
#define CAUSE_BY_SIGNAL() (errno == EINTR)
#else
#define CAUSE_BY_SIGNAL() (false)
#endif

#if defined(_WIN32)
#define poll(...) WSAPoll(__VA_ARGS__)
#define POLL_FlAG POLLRDNORM
#else
#define POLL_FlAG POLLIN
#endif

namespace perfdog {
template <typename T, int (*CloseFunction)(T), T InvalidValue>
class ScopedResource {
 public:
  explicit ScopedResource(T fd = InvalidValue) : fd_(fd) {}
  ScopedResource(ScopedResource& other) = delete;
  ScopedResource& operator=(ScopedResource& other) = delete;
  ScopedResource(ScopedResource&& other) noexcept {
    fd_ = other.fd_;
    other.fd_ = InvalidValue;
  }

  ScopedResource& operator=(ScopedResource&& other) noexcept {
    release();
    fd_ = other.fd_;
    other.fd_ = InvalidValue;

    return *this;
  }

  T Get() const { return fd_; }

  virtual ~ScopedResource() { release(); }

 private:
  void release() {
    if (fd_ != InvalidValue) {
      CloseFunction(fd_);
    }
    fd_ = InvalidValue;
  }

  T fd_;
};

#if defined(__ANDROID__) || defined(__APPLE__)
using ScopeFile = ScopedResource<int, close, -1>;
#elif defined(_WIN32)
using ScopeFile = ScopedResource<SOCKET, closesocket, INVALID_SOCKET>;
#endif

#ifdef _GAMING_XBOX
#define SOCKET_PORT 43000
#else
#define SOCKET_PORT 53000
#endif

class PerfDogExtensionPosix : public PerfDogExtension {
 public:
  PerfDogExtensionPosix() : server_fd_(-1), stop_server_(false) {}

  ~PerfDogExtensionPosix() override { PerfDogExtensionPosix::StopServer(); }

 protected:
  int StartServer() override {
    std::lock_guard<std::mutex> lock_guard(server_lock_);

    if (server_) return 0;

    server_fd_ = ScopeFile(::socket(AF_INET, SOCK_STREAM, 0));
    if (server_fd_.Get() == -1) {
      PRINT_ERROR("socket");
      return 1;
    }

    int opt = 1;
    if (::setsockopt(server_fd_.Get(), SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) < 0) {
      PRINT_ERROR("setsockopt");
      return 1;
    }

#if defined(SO_NOSIGPIPE)
    {
      int on = 1;
      ::setsockopt(server_fd_.Get(), SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    }
#endif

    sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(SOCKET_PORT);
    if (::bind(server_fd_.Get(), (sockaddr*)&local_addr, sizeof(local_addr)) == -1) {
      PRINT_ERROR("bind");
      return 1;
    }

    if (::listen(server_fd_.Get(), 1) == -1) {
      PRINT_ERROR("listen");
      return 1;
    }

    stop_server_ = false;
    server_ = std::unique_ptr<PlatformThread>(CreatePlatformThread(
        [](void* arg) { reinterpret_cast<PerfDogExtensionPosix*>(arg)->ProcessClientMessage(); }, this));
    return 0;
  }

  void StopServer() override {
    std::lock_guard<std::mutex> lock_guard(server_lock_);

    if (server_) {
      stop_server_ = true;
      server_->Join();
      server_ = nullptr;
    }
  }

  int SendData(const void* buf, int size) override {
    std::lock_guard<std::mutex> lock_guard(client_fd_lock_);

    if (client_fd_.Get() != -1) {
      int ret = CallSend(client_fd_.Get(), (const char*)buf, size, SOCKET_SEND_FLAG);
      if (ret != size) ErrorLog("send error:size %d,ret %d", size, ret);
      return ret;
    } else {
      return 0;
    }
  }

  int CallRecv(int fd, char* buf, size_t n, int flags) {
    while (true) {
      int ret = ::recv(fd, buf, n, flags);
      if (ret == -1) {
        if (CAUSE_BY_SIGNAL()) {
          continue;
        }
        PRINT_ERROR("recv");
      }

      return ret;
    }
  }

  int CallSend(int fd, const char* buf, size_t n, int flags) {
    while (true) {
      int ret = ::send(fd, buf, n, flags);
      if (ret == -1) {
        if (CAUSE_BY_SIGNAL()) {
          continue;
        }
        PRINT_ERROR("send");
      }

      return ret;
    }
  }

  int CallAccept(int fd, struct sockaddr* addr, socklen_t* addr_length) {
    while (true) {
      int ret = ::accept(fd, addr, addr_length);
      if (ret == -1) {
        if (CAUSE_BY_SIGNAL()) {
          continue;
        }
        PRINT_ERROR("accept");
      }

      return ret;
    }
  }

  // 0:timeout -1:error or EOF >0:Actual readings
  // 0:timeout -1:错误或者EOF >0:实际读到的数据
  int ReadWithTimeout(const ScopeFile& fd, int timeout_ms, char* buf, int count) {
    pollfd rfds;

    memset(&rfds, 0, sizeof(rfds));
    rfds.fd = fd.Get();
    rfds.events = POLL_FlAG;

    int ret = ::poll(&rfds, 1, timeout_ms);
    if (ret == -1) {
      if (CAUSE_BY_SIGNAL()) {
        return 0;
      }
      PRINT_ERROR("poll");
      return -1;
    } else if (ret) {
      if (rfds.revents & POLL_FlAG) {
        int nread = CallRecv(fd.Get(), buf, count, 0);
        return nread > 0 ? nread : -1;
      } else {
        if ((rfds.revents & POLLHUP) == 0) {
          ErrorLog("client fd poll revents: 0x%x", rfds.revents);
        }
        return -1;
      }
    } else
      return 0;
  }

  void ProcessClientMessage() {
    while (!stop_server_) {
      pollfd rfds;

      memset(&rfds, 0, sizeof(rfds));
      rfds.fd = server_fd_.Get();
      rfds.events = POLL_FlAG;

      int ret = ::poll(&rfds, 1, 100);
      if (ret == -1) {
        if (CAUSE_BY_SIGNAL()) {
          continue;
        }
        PRINT_ERROR("poll");
        return;
      }

      if (ret > 0) {
        if ((rfds.revents & POLL_FlAG) == 0) {
          ErrorLog("server_fd_ poll revents: 0x%x", rfds.revents);
          return;
        }

        sockaddr_in peer_addr;
        socklen_t peer_addr_size = sizeof(peer_addr);
        ScopeFile fd(CallAccept(server_fd_.Get(), (sockaddr*)&peer_addr, &peer_addr_size));

        {
          std::lock_guard<std::mutex> lock_guard(client_fd_lock_);
          client_fd_ = std::move(fd);
          ErrorLog("client connected");
        }
        OnConnect();
        ReadData(client_fd_);
        {
          std::lock_guard<std::mutex> lock_guard(client_fd_lock_);
          ErrorLog("client disconnect");
          client_fd_ = ScopeFile();
        }
        OnDisconnect();
      }
    }
  }

  void ReadData(ScopeFile& fd) {
    std::string data;
    char temp[4096];

    while (!stop_server_) {
      int ret = ReadWithTimeout(fd, 100, temp, sizeof(temp));
      if (ret == -1) return;

      if (ret > 0) {
        data.append(temp, ret);

        // parse data
        // 解析数据
        Buffer buffer(&data[0], data.size());
        while (buffer.Remaining() >= kMessageHeaderLength) {
          Buffer slice = buffer.Slice();

          if (parseMessage(slice)) {
            buffer.Advance(slice.DataSize());
          } else {
            break;
          }
        }

        // Clear read data
        // 清除已读的数据
        if (buffer.DataSize() > 0) data.erase(0, buffer.DataSize());
      }
    }
  }

  //-1:error 0:data not complete 1:successfully parsing
  //-1:error 0:数据还没收全 1:解析成功
  int parseMessage(Buffer& buffer) {
    MessageHeader header;

    if (buffer.Remaining() < kMessageHeaderLength) return 0;
    buffer.ReadBytes(&header, sizeof(header));
    if (buffer.Remaining() < header.length) return 0;

    if (header.length > kMaxMessageLength) {
      ErrorLog("message too large:%u %u", header.type, header.length);
      return -1;
    }

    // parse data
    // 解析数据
    if (header.length == 0) {
      OnMessage(header.type, nullptr);
    } else {
      switch (header.type) {
        case kStringMap: {
          StringMap msg;
          msg.Deserialize(buffer);
          if (buffer.IsOverflow()) {
            ErrorLog("message %u overflow", header.type);
            return -1;
          }

          OnMessage(header.type, &msg);
          break;
        }
        case kCustomIntegerValue: {
          CustomIntegerValue msg;
          msg.Deserialize(buffer);
          if (buffer.IsOverflow()) {
            ErrorLog("message %u overflow", header.type);
            return -1;
          }

          OnMessage(header.type, &msg);
          break;
        }
        default:
          ErrorLog("unknown %d\n", header.type);
      }
    }

    return 1;
  }

 private:
  std::mutex server_lock_;
  std::unique_ptr<PlatformThread> server_;
  ScopeFile server_fd_;
  ScopeFile client_fd_;
  std::mutex client_fd_lock_;
  std::atomic_bool stop_server_;
};
}  // namespace perfdog

//----------------------------------------------------------------
// Realization of each platform
// 各个平台的实现
//----------------------------------------------------------------
#ifdef __ANDROID__
#include <android/log.h>

namespace perfdog {

static void ErrorLog(const char* format, ...) {
  va_list arglist;
  va_start(arglist, format);
  __android_log_vprint(ANDROID_LOG_ERROR, "PerfDogExtension", format, arglist);
  va_end(arglist);
}

static void PrintError(const char* file, int line, const char* prefix) {
  ErrorLog("%s:%d %s:%s", file, line, prefix, strerror(errno));
}

class PerfDogExtensionAndroid : public PerfDogExtensionPosix {
 protected:
  uint64_t CurrentTime() override {
    timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
      perror("clock_gettime:");
      return 0;
    }

    return t.tv_sec * (uint64_t)1e3 + t.tv_nsec / 1000000;
  }

  int CurrentThreadId() override { return gettid(); }
};

static PerfDogExtension& GetPerfDogExtensionInstance() {
  static PerfDogExtensionAndroid instance;
  return instance;
}
}  // namespace perfdog
#endif

#ifdef __APPLE__
#include <Foundation/Foundation.h>
#include <mach/mach_time.h>
#include <pthread.h>

namespace perfdog {

static void ErrorLog(const char* format, ...) {
  char output[256];
  va_list vargs;
  va_start(vargs, format);
  vsnprintf(output, sizeof(output), format, vargs);
  NSLog(@"PerfDogExtension: %s", output);
  va_end(vargs);
}

static void PrintError(const char* file, int line, const char* prefix) {
  ErrorLog("%s:%d %s:%s", file, line, prefix, strerror(errno));
}

class PerfDogExtensionIos : public PerfDogExtensionPosix {
 public:
  PerfDogExtensionIos() {
    mach_timebase_info_data_t Info;
    mach_timebase_info(&Info);
    time_freq_ = (uint64_t(1 * 1000 * 1000) * uint64_t(Info.denom)) / uint64_t(Info.numer);
  }

 protected:
  uint64_t CurrentTime() override {
    uint64_t now = mach_absolute_time();
    return now / time_freq_;
  }

  int CurrentThreadId() override { return (int)pthread_mach_thread_np(pthread_self()); }

 private:
  uint64_t time_freq_;
};

static PerfDogExtension& GetPerfDogExtensionInstance() {
  static PerfDogExtensionIos instance;
  return instance;
}
}  // namespace perfdog
#endif

#if defined(_WIN32) || defined(_GAMING_XBOX)
#include <processthreadsapi.h>

namespace perfdog {

static void ErrorLog(const char* format, ...) {
  char output[256];
  va_list vargs;
  va_start(vargs, format);
  vsnprintf(output, sizeof(output), format, vargs);
  puts(output);
  OutputDebugStringA(output);
  va_end(vargs);
}

static void PrintError(const char* file, int line, const char* prefix) {
  char* buf = NULL;
  const char* errmsg;

  FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                 WSAGetLastError(), 0, (LPSTR)&buf, 0, NULL);

  if (buf)
    errmsg = buf;
  else
    errmsg = "Unknown error";

  ErrorLog("%s:%d %s:%s", file, line, prefix, errmsg);
  if (buf) LocalFree(buf);
}

class PerfDogExtensionWindow : public PerfDogExtensionPosix {
 public:
  PerfDogExtensionWindow() {
    WSADATA wsa_data;

    int ret = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (ret != 0) {
      PRINT_ERROR("WSAStartup");
    }

    QueryPerformanceFrequency(&time_freq_);
  }

 protected:
  uint64_t CurrentTime() override {
    LARGE_INTEGER time;

    QueryPerformanceCounter(&time);
    return time.QuadPart * 1000 / time_freq_.QuadPart;
  }

  int CurrentThreadId() override { return GetCurrentThreadId(); }

 private:
  LARGE_INTEGER time_freq_;
};

static PerfDogExtension& GetPerfDogExtensionInstance() {
  static PerfDogExtensionWindow instance;
  return instance;
}
}  // namespace perfdog
#endif

// thread implement
#if defined(__ANDROID__) || defined(__APPLE__)

#include <pthread.h>

namespace perfdog {

class PlatformThreadPThread : public PlatformThread {
 public:
  explicit PlatformThreadPThread(void (*func_ptr)(void*), void* arg)
      : PlatformThread(func_ptr, arg), thread_id_(0), thread_started_(false), had_call_join_(false) {
    pthread_t thread_id;
    int ret = pthread_create(&thread_id, nullptr, &ThreadEntry, this);
    if (ret) {
      PRINT_ERROR("pthread_create");
      return;
    }
    this->thread_id_ = thread_id;
    this->thread_started_ = true;
  }

  ~PlatformThreadPThread() override { PlatformThreadPThread::Join(); }

  void Join() override {
    if (thread_started_ && !had_call_join_) {
      had_call_join_ = true;
      pthread_join(thread_id_, nullptr);
    }
  }

 private:
  static void* ThreadEntry(void* t) {
    reinterpret_cast<PlatformThreadPThread*>(t)->Run();
    return nullptr;
  }

  pthread_t thread_id_;
  std::atomic_bool thread_started_;
  std::atomic_bool had_call_join_;
};

PlatformThread* CreatePlatformThread(void (*func_ptr)(void*), void* arg) {
  return new PlatformThreadPThread(func_ptr, arg);
}

}  // namespace perfdog

#elif defined(_WIN32) || defined(_GAMING_XBOX)

namespace perfdog {

class PlatformThreadWin : public PlatformThread {
 public:
  PlatformThreadWin(void (*func_ptr)(void*), void* arg)
      : PlatformThread(func_ptr, arg), handle_(nullptr), thread_started_(false), had_call_join_(false) {
    handle_ = CreateThread(nullptr, 0, ThreadEntry, this, 0, nullptr);
    if (handle_ == nullptr) {
      PRINT_ERROR("CreateThread");
      return;
    }
    thread_started_ = true;
  }

  ~PlatformThreadWin() override { PlatformThreadWin::Join(); }

  void Join() override {
    if (thread_started_ && !had_call_join_) {
      had_call_join_ = true;
      WaitForSingleObject(handle_, INFINITE);
      CloseHandle(handle_);
    }
  }

 private:
  static DWORD ThreadEntry(void* t) {
    reinterpret_cast<PlatformThreadWin*>(t)->Run();
    return 0;
  }

  HANDLE handle_;
  std::atomic_bool thread_started_;
  std::atomic_bool had_call_join_;
};

PlatformThread* CreatePlatformThread(void (*func_ptr)(void*), void* arg) {
  return new PlatformThreadWin(func_ptr, arg);
}

}  // namespace perfdog

#endif

//----------------------------------------------------------------
// External interface
// 外部接口
//----------------------------------------------------------------
namespace perfdog {
int EnableSendToPerfDog() { return GetPerfDogExtensionInstance().Init(); }
bool IsStartTest() { return GetPerfDogExtensionInstance().IsStartTest(); }

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

void setLabel(const std::string& name) { GetPerfDogExtensionInstance().setLabel(name); }

void addNote(const std::string& name) { GetPerfDogExtensionInstance().addNote(name); }

void DeepMark(const std::string& name) { GetPerfDogExtensionInstance().DeepMark(name); }

void DeepCounter(const std::string& name, float value) { GetPerfDogExtensionInstance().DeepCounter(name, value); }

void DeepPush(const std::string& name) { GetPerfDogExtensionInstance().DeepPush(name); }

void DeepPop(const std::string& name) { GetPerfDogExtensionInstance().DeepPop(name); }

}  // namespace perfdog

#endif  // PERFDOG_EXTENSION_ENABLE
#endif