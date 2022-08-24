// Copyright 2022 Tencent Inc. All rights reserved.
//
// Author: PerfDog@tencent.com
// Version: 1.1

#if defined(__ANDROID__) || defined(__APPLE__)

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

namespace perfdog {

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
  uint16_t length;  // 0表示Message是空的,不为0后面的length个字节是Message的内容
  uint16_t type;
};

static constexpr int kMessageHeaderLength = sizeof(MessageHeader);

enum MessageType : uint16_t {
  kInvalid = 0,
  kStartTestReq,  //空
  kStartTestRsp,  //空
  kStopTestReq,   //空
  kStopTestRsp,   //空
  kStringMap,
  kCustomFloatValue,
  kCustomIntegerValue,
  kCustomStringValue,
  kCleanMap,  //空
  kSetLabelReq,
  kAddNoteReq,
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
  int pos_;
  int capacity_;
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

//非英文字符需要UTF-8编码
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
//数据处理
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

class PerfDogExtension {
 public:
  PerfDogExtension() : stop_(false), start_test_(false) {}

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
  virtual void ErrorLog(const char* format, ...) = 0;
  virtual uint64_t CurrentTime() = 0;  //毫秒

 private:
  void StartTest() {
    std::lock_guard<std::mutex> lock_guard(lock_);

    if (!writer_) {
      stop_ = false;
      start_test_ = true;
      ClearData();
      writer_ = std::unique_ptr<std::thread>(new std::thread(&PerfDogExtension::DrainBuffer, this));
    }
    WriteMessage(EmptyMessageWrapper(kStartTestRsp));
  }

  void StopTest() {
    std::lock_guard<std::mutex> lock_guard(lock_);

    if (writer_) {
      stop_ = true;
      start_test_ = false;
      writer_->join();
      writer_ = nullptr;
      ClearData();
    }
    WriteMessage(EmptyMessageWrapper(kStopTestRsp));
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
    while (!stop_) {
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
  std::unique_ptr<std::thread> writer_;
  std::mutex lock_;
  std::atomic_bool stop_;
  std::atomic_bool start_test_;
  std::forward_list<BlockPtr> block_list_;  //前插链表
  std::mutex buffer_lock_;
  std::unordered_map<std::string, uint32_t> string_id_map_;
  std::mutex string_id_map_lock_;
  uint32_t string_id_ = 0;
};
}  // namespace perfdog

//----------------------------------------------------------------
//网络收发
//----------------------------------------------------------------
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace perfdog {
class ScopeFile {
 public:
  explicit ScopeFile(int fd = -1) : fd_(fd) {}
  ScopeFile(ScopeFile& other) = delete;
  ScopeFile& operator=(ScopeFile& other) = delete;
  ScopeFile(ScopeFile&& other) noexcept {
    fd_ = other.fd_;
    other.fd_ = -1;
  }

  ScopeFile& operator=(ScopeFile&& other) noexcept {
    release();
    fd_ = other.fd_;
    other.fd_ = -1;

    return *this;
  }

  int Get() const { return fd_; }

  virtual ~ScopeFile() { release(); }

 private:
  void release() {
    if (fd_ != -1) {
      close(fd_);
    }
    fd_ = -1;
  }

  int fd_;
};

class PerfDogExtensionPosix : public PerfDogExtension {
 public:
  PerfDogExtensionPosix() : server_fd_(-1), stop_(false) {}

  ~PerfDogExtensionPosix() override { PerfDogExtensionPosix::StopServer(); }

 protected:
  int StartServer() override {
    std::lock_guard<std::mutex> lock_guard(lock_);

    if (server_) return 0;

    server_fd_ = ScopeFile(socket(AF_INET, SOCK_STREAM, 0));
    if (server_fd_.Get() == -1) {
      PrintError("socket");
      return 1;
    }

    int opt = 1;
    if (setsockopt(server_fd_.Get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
      PrintError("setsockopt");
      return 1;
    }

    sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(53000);
    if (bind(server_fd_.Get(), (sockaddr*)&local_addr, sizeof(local_addr)) == -1) {
      PrintError("bind");
      return 1;
    }

    if (listen(server_fd_.Get(), 1) == -1) {
      PrintError("listen");
      return 1;
    }

    stop_ = false;
    server_ = std::unique_ptr<std::thread>(new std::thread(&PerfDogExtensionPosix::ProcessClientMessage, this));
    return 0;
  }

  void StopServer() override {
    std::lock_guard<std::mutex> lock_guard(lock_);

    if (server_) {
      stop_ = true;
      server_->join();
      server_ = nullptr;
    }
  }

  int SendData(const void* buf, int size) override {
    std::lock_guard<std::mutex> lock_guard(lock_);

    if (client_fd_.Get() != -1) {
      int ret = send(client_fd_.Get(), buf, size, MSG_NOSIGNAL);
      if (ret != size) ErrorLog("send error:size %d,ret %d", size, ret);
      return ret;
    } else {
      return 0;
    }
  }

  void PrintError(const char* prefix) { ErrorLog("%s:%s", prefix, strerror(errno)); }

  // 0:timeout -1:错误或者EOF >0:实际读到的数据
  int ReadWithTimeout(int fd, int timeout_ms, char* buf, int count) {
    pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret == -1) {
      PrintError("poll");
      return -1;
    } else if (ret > 0) {
      if (pfd.revents & POLLIN) {
        int nread = read(fd, buf, count);
        return nread > 0 ? nread : -1;
      } else {
        ErrorLog("fd error:%d", pfd.revents);
        return -1;
      }
    }

    return 0;
  }

  void ProcessClientMessage() {
    pollfd server_poll;
    server_poll.fd = server_fd_.Get();
    server_poll.events = POLLIN;

    while (!stop_) {
      int ret = poll(&server_poll, 1, 100);
      if (ret == -1) {
        PrintError("poll");
        return;
      }

      if (ret > 0) {
        if (server_poll.revents & POLLIN) {
          sockaddr_in peer_addr;
          socklen_t peer_addr_size = sizeof(peer_addr);
          int fd = accept(server_fd_.Get(), (sockaddr*)&peer_addr, &peer_addr_size);

          {
            std::lock_guard<std::mutex> lock_guard(lock_);
            client_fd_ = ScopeFile(fd);
          }
          OnConnect();
          ReadData(client_fd_);
          {
            std::lock_guard<std::mutex> lock_guard(lock_);
            client_fd_ = ScopeFile();
          }
          OnDisconnect();
        } else {
          ErrorLog("server fd error:%d", ret);
          return;
        }
      }
    }
  }

  void ReadData(ScopeFile& fd) {
    std::string data;
    char temp[4096];

    while (!stop_) {
      int ret = ReadWithTimeout(fd.Get(), 100, temp, sizeof(temp));
      if (ret == -1) return;

      if (ret > 0) {
        data.append(temp, ret);

        //解析数据
        Buffer buffer(&data[0], data.size());
        while (buffer.Remaining() >= kMessageHeaderLength) {
          Buffer slice = buffer.Slice();

          if (parseMessage(slice)) {
            buffer.Advance(slice.DataSize());
          } else {
            break;
          }
        }

        //清除已读的数据
        if (buffer.DataSize() > 0) data.erase(0, buffer.DataSize());
      }
    }
  }

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

    //解析数据
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
  std::mutex lock_;
  std::unique_ptr<std::thread> server_;
  ScopeFile server_fd_;
  ScopeFile client_fd_;
  std::atomic_bool stop_;
};
}  // namespace perfdog

//----------------------------------------------------------------
//各个平台的实现
//----------------------------------------------------------------
#ifdef __ANDROID__
#include <android/log.h>

namespace perfdog {

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

  void ErrorLog(const char* format, ...) override {
    va_list arglist;
    va_start(arglist, format);
    __android_log_vprint(ANDROID_LOG_ERROR, "PerfDogExtension", format, arglist);
    va_end(arglist);
  }
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

namespace perfdog {
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

  void ErrorLog(const char* format, ...) override {
    char output[256];
    va_list vargs;
    va_start(vargs, format);
    vsnprintf(output, sizeof(output), format, vargs);
    NSLog(@"PerfDogExtension: %s", output);
    va_end(vargs);
  }

 private:
  uint64_t time_freq_;
};

static PerfDogExtension& GetPerfDogExtensionInstance() {
  static PerfDogExtensionIos instance;
  return instance;
}
}  // namespace perfdog
#endif

//----------------------------------------------------------------
//外部接口
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

}  // namespace perfdog

#endif  // PERFDOG_EXTENSION_ENABLE
#endif