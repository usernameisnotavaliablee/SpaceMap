#include "tips.h"
#include <Windows.h>
#include <cstdlib>

// 贴士池:用法(tip:) + 冷知识(冷芝士) + 冷笑话
// 源文件编码 UTF-8,编译时 -finput-charset=UTF-8
// 启动时洗牌，每次运行顺序不同。
static const char* TIPS[] = {
    // ---- 用法 & 参数 ----
    "tip:map -i 进入 TUI 交互模式,像 ncdu 一样上下浏览、回车进目录 ",
    "tip:map -t 20 列出最大的 20 个文件,揪出占空间的元凶 ",
    "tip:map -a 显示全部文件夹(含被折叠的小目录和 0B 目录) ",
    "tip:map -j 输出 JSON,配合 -o result.json 可写入文件做后续分析 ",
    "tip:map -s name 按名称排序,map -s size 按大小排序(默认按大小) ",
    "tip:map -v 查看到底跳过了哪些目录、哪些目录读取出错 ",
    "tip:map -w 8 手动指定扫描线程数,默认会按 CPU 自动选择 ",
    "tip:把 map.exe 丢进 C:\\Windows,之后在任何路径敲 map 就能用 ",
    "tip:扫描 NTFS 卷根目录(如 map C:\\)会自动走 MFT 直读,快到飞起 ",
    "tip:TUI 里按 T 看大文件,按 S 切换排序,按 Q 或 ESC 退出 ",
    "tip:TUI 里按退格返回上级目录,能一路退到盘符根 ",
    "tip:--no-color 关闭颜色,方便把输出重定向到文件或日志 ",
    "tip:0B 目录默认隐藏；当大部分子目录都很大时,小目录会自动折叠 ",
    "tip:.开头的目录(.git/.cache)在 Windows 上是普通目录,会被正常统计 ",
    "tip:-a 的结果有 3 分钟缓存,反复查看同一目录时秒出 ",
	"这是一个tip",
    // ---- 磁盘 / 文件系统冷知识 ----
    "冷芝士 NTFS 把整个磁盘的文件信息存在一张叫 MFT 的表里,直接读它比逐个翻文件夹快得多 ",
    "冷芝士 一个 4KB 簇的磁盘上,存 1 字节的文件也要占 4KB——剩下的全是浪费 ",
    "冷芝士 删除文件通常只是把它从目录里抹掉,数据还躺在原地,所以才能被恢复 ",
    "冷芝士 1 KB 到底是 1000 还是 1024 字节？硬盘厂商按 1000 算,所以你的 1TB 盘只有约 931 GiB ",
    "冷芝士 文件夹本身几乎不占空间,它只是一份指向文件的清单 ",
    "冷芝士 Windows 的回收站其实是每个盘根目录下一个叫 $RECYCLE.BIN 的隐藏文件夹 ",
    "冷芝士 符号链接/junction 是目录的「快捷方式」,扫描工具一不小心就会重复计数甚至绕圈 ",
    "冷芝士 碎片整理之所以提速,是把一个文件分散的碎块挪到一起,让磁头少跑路——SSD 则不需要 ",
    "冷芝士 SSD 的每个存储单元写入次数有限,所以它会「磨损均衡」地把写入摊开到整块盘 ",
    "冷芝士 %TEMP% 临时目录是最容易悄悄堆积垃圾的地方",
    // ---- 冷笑话 ----
	"清完磁盘垃圾建议找找镜子看看还有没有垃圾",
	"冷知识 你绷住了",
	"有人想要Linux版本的吗 去提issue吧",
	"其实没有人会看tips",
	"你就用吧 没人点star的我一点也不难受",
	"和她的回忆可以删了 你的盘里还有她 但她的心里没有你",
    "硬盘满了别慌,毕竟空间是用来被占满的,就像周末是用来加班的 ",
    "程序员清理磁盘的三个阶段:删缓存、删 node_modules、删硬盘分区 ",
    "我把 C 盘清理干净了,结果发现占空间最多的是我清理出来的备份 ",
    "你存的不是文件,是再也不会打开的回忆",
    "你下载文件夹是装了单向阀吗,只进不出",
    "扫描进度条跑得越快,越说明你的盘是真的空虚",
    "还是不知道是什么占了那么多空间?快去看看你的截图吧",
	"这么快又满了吗~杂鱼杂鱼",
	"你忘得掉昨天下了什么软件 但永远记得第一次见她时头发的香气",
};

static const int TIPS_N = (int)(sizeof(TIPS) / sizeof(TIPS[0]));

int tips_count() { return TIPS_N; }

const char* tip_at(int index) {
    if (TIPS_N == 0) return "";
    int i = index % TIPS_N;
    if (i < 0) i += TIPS_N;
    return TIPS[i];
}

int tips_random_start() {
    if (TIPS_N == 0) return 0;
    LARGE_INTEGER li;
    unsigned long long seed = (unsigned long long)GetTickCount();
    if (QueryPerformanceCounter(&li)) {
        seed ^= (unsigned long long)li.QuadPart;
    }
    srand((unsigned int)seed);
    // Fisher-Yates 洗牌
    for (int i = TIPS_N - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        const char* tmp = TIPS[i];
        TIPS[i] = TIPS[j];
        TIPS[j] = tmp;
    }
    return 0;
}

const char* tip_for_elapsed(int start, long long elapsed_ms, long long rotate_ms) {
    if (TIPS_N == 0) return "";
    if (rotate_ms <= 0) rotate_ms = 2500;
    long long slot = elapsed_ms / rotate_ms;
    long long idx = (long long)start + slot;
    int i = (int)(idx % TIPS_N);
    if (i < 0) i += TIPS_N;
    return TIPS[i];
}
