// Copyright 2022 Tencent Inc. All rights reserved.
//
// Author: PerfDog@tencent.com
// Version: 1.0

#ifndef PERFDOG_EXTENSION_H_
#define PERFDOG_EXTENSION_H_

#include <string>

#ifndef PERFDOG_EXTENSION_ENABLE
  //编译开关
  #define PERFDOG_EXTENSION_ENABLE
#endif

namespace perfdog {

#if defined(PERFDOG_EXTENSION_ENABLE) && (defined(__ANDROID__) || defined(__APPLE__))

//总开关
int EnableSendToPerfDog();

void PostValueF(const std::string& category, const std::string& key, float a);
void PostValueF(const std::string& category, const std::string& key, float a, float b);
void PostValueF(const std::string& category, const std::string& key, float a, float b, float c);
void PostValueI(const std::string& category, const std::string& key, int a);
void PostValueI(const std::string& category, const std::string& key, int a, int b);
void PostValueI(const std::string& category, const std::string& key, int a, int b, int c);
void PostValueS(const std::string& category, const std::string& key, const std::string& value);

//标记进入一个新场景,这个场景会持续到下次setLabel
void setLabel(const std::string& name);

//对当前时刻进行批注及标定
void addNote(const std::string& name);

#else

int EnableSendToPerfDog() { return 0; }

void PostValueF(const std::string&, const std::string&, float) {}
void PostValueF(const std::string&, const std::string&, float, float) {}
void PostValueF(const std::string&, const std::string&, float, float, float) {}
void PostValueI(const std::string&, const std::string&, int) {}
void PostValueI(const std::string&, const std::string&, int, int) {}
void PostValueI(const std::string&, const std::string&, int, int, int) {}
void PostValueS(const std::string&, const std::string&, const std::string&) {}

//标记进入一个新场景,这个场景会持续到下次setLabel
void setLabel(const std::string&) {}

//对当前时刻进行批注及标定
void addNote(const std::string&) {}

#endif

}  // namespace perfdog

#endif  // PERFDOG_EXTENSION_H_
