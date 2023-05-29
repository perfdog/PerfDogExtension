# PerfDog自定义数据拓展说明

为了更方便且准确地测试及分析性能问题，目前支持将自定义的性能相关数据实时同步到PerfDog，同样也支持数据展示、存储及对比等功能。例如可以在代码中添加标签及批注，而不用人为的判断，更加准确且方便。也可以将一些更加细分的数据，例如顶点或纹理内存占用、DrawCall、Triangle、网络情况等，同步到PerfDog，便于深入分析。甚至可以将一些逻辑数据，比如场景、坐标、朝向、怪物或角色的数量、粒子特效等等，便于重现场景进而深入分析。

## 支持的平台
- Android
- iOS
- Windows

## 接入PerfDogExtension

### 1. 集成PerfDog提供的源文件

PerfDog会提供两个源文件,分别是perfdog_extension.h和perfdog_extension.cpp.使用者需要把他们放进工程里.文件可以从https://github.com/perfdog/PerfDogExtension 上下载

### 2. 启动PerfDog自定义数据拓展

```
int EnableSendToPerfDog()
```

游戏启动后调用这个接口启动PerfDog自定义数据拓展.这个接口会创建一个socket并监听53000端口

### 3. 发送自定义数据

- 浮点数(支持一个key对应1到3个value)
```
void PostValueF(const std::string& category, const std::string& key, float a);
void PostValueF(const std::string& category, const std::string& key, float a, float b);
void PostValueF(const std::string& category, const std::string& key, float a, float b, float c);
```

- 整数(支持一个key对应1到3个value)
```
void PostValueI(const std::string& category, const std::string& key, int a);
void PostValueI(const std::string& category, const std::string& key, int a, int b);
void PostValueI(const std::string& category, const std::string& key, int a, int b, int c);
```

- 字符串(支持一个key对应1到1个value)
```
void PostValueS(const std::string& category, const std::string& key, const std::string& value);
```

- 设置场景Label标签

```
//标记进入一个新场景,这个场景会持续到下次setLabel
void setLabel(const std::string& name);
```

等效于红框内的按钮

![](https://github.com/perfdog/PerfDogExtension/raw/master/img/img1.png)

- 添加批注

```
//对当前时刻进行批注及标定
void addNote(const std::string& name);
```

等效于图表上左键双击

![](https://github.com/perfdog/PerfDogExtension/raw/master/img/img2.png)

note:客户端收到数据后会将所有category相同的数据放到一个表里,标题为category,线条的名字为key.如果value有多个,线条的名字为key_a,key_b,key_c

## 禁用PerfDogExtension

在perfdog_extension.h里注释下图框里的代码

## 已知问题

在mac上ios手机在wifi模式下无法获取自定义数据

## 性能测试

测试环境:pixel3,小核锁定1766MHz,测试程序绑定在小核上

测试结果:在每秒调用2w次PostValueI的情况下cpu占用为1%~2%

## 错误排查

- Android

用adb logcat查看手机日志,搜索PerfDogExtension.例如端口被占用的错误日志
2022-07-27 17:15:29.495 8680-8682/? E/PerfDogExtension: bind:Address already in use

- IOS

在mac上用控制台查看手机日志,搜索PerfDogExtension
