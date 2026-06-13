#ifndef SPACEMAP_TIPS_H
#define SPACEMAP_TIPS_H

#include <string>

// 等待扫描时在进度条下方滚动显示的小贴士：
// 包含 SpaceMap 用法/参数、磁盘冷知识、冷笑话等。

// 返回贴士总条数
int tips_count();

// 取第 i 条贴士（自动取模，越界安全）。返回 UTF-8 文本。
const char* tip_at(int index);

// 取一个伪随机起始下标（基于系统时钟，每次进程启动不同）。
int tips_random_start();

// 根据起始下标 start 与已过去的毫秒数 elapsed_ms，
// 计算当前应显示哪一条贴士（每 rotate_ms 毫秒切换一条）。
const char* tip_for_elapsed(int start, long long elapsed_ms, long long rotate_ms);

#endif
