# Language
- [English](README.md)
- [中文](README_zh.md)

# PerfDog Custom Data Extension Description

In order to test and analyze performance problems more conveniently and accurately, PerfDog supports real-time synchronization of customized performance-related data to PerfDog, as well as data display, storage and comparison functions. For example, you can add tags and annotations to the code without human judgment, which is more accurate and convenient. You can also synchronize some more detailed data, such as vertex or texture memory usage, DrawCall, Triangle, network conditions, etc., to PerfDog for in-depth analysis. You can even synchronize some logical data, such as scene, coordinates, orientation, number of monsters or characters, particle effects, etc., to facilitate the reproduction of the scene for in-depth analysis.

## Supported platforms
- Android
- iOS
- Windows
- PlayStation
- Xbox
- Harmony

## Access to PerfDogExtension

### 1. Integration of source files provided by PerfDog

PerfDog will provide two source files, perfdog_extension.h and perfdog_extension.cpp. Users need to put them into the project. The files can be downloaded from https://github.com/perfdog/PerfDogExtension
- PlayStation custom data extension requires additional source files to be integrated. Please obtain them on the official website or by contacting the sales manager.
- Xbox platform requires additional firewall configuration to open host ports

### 2. Launch PerfDog Custom Data Extension

```
int EnableSendToPerfDog()
```

This interface is called after the game has started to initiate PerfDog's custom data extension. This interface creates a socket and listens on port 53000.

### 3. Send customized data

- Floating point numbers (supports 1 to 3 values for a key)
```
void PostValueF(const std::string& category, const std::string& key, float a);
void PostValueF(const std::string& category, const std::string& key, float a, float b);
void PostValueF(const std::string& category, const std::string& key, float a, float b, float c);
```

- Integer (supports 1 to 3 values for a key)
```
void PostValueI(const std::string& category, const std::string& key, int a);
void PostValueI(const std::string& category, const std::string& key, int a, int b);
void PostValueI(const std::string& category, const std::string& key, int a, int b, int c);
```

- String (supports 1 values for a key)
```
void PostValueS(const std::string& category, const std::string& key, const std::string& value);;
```

- Set the scene Label
```
// Marks the entry of a new scene, which lasts until the next setLabel.
void setLabel(const std::string& name);
```

Equivalent to the button in the red box

![](https://github.com/perfdog/PerfDogExtension/raw/master/img/img1_en.png)

- Add annotations

```
// Annotate and calibrate the current moment
void addNote(const std::string& name);
```

Equivalent to a left double-click on a chart

![](https://github.com/perfdog/PerfDogExtension/raw/master/img/img2_en.png)

note: when the client receives the data, it will put all the data with the same category into a table, the title is category, and the name of the line is key. if the value has more than one, the name of the line is key_a, key_b, key_c.

## Disable PerfDogExtension

Comment out the code in the box below in perfdog_extension.h

![](https://github.com/perfdog/PerfDogExtension/raw/master/img/img3.png)

## Known issues

Can't get custom data from ios phone on mac in wifi mode

## Performance test

Test environment: pixel3, small core locked at 1766MHz, test program bound to small core.

Test result: cpu usage is 1%~2% under the case of calling PostValueI 2w times per second.

## Troubleshooting

- Android

Use adb logcat to check cell phone logs, search for PerfDogExtension. e.g. port occupied error logs.
2022-07-27 17:15:29.495 8680-8682/? E/PerfDogExtension: bind:Address already in use

- IOS

Checking phone logs on mac using console, searching for PerfDogExtension