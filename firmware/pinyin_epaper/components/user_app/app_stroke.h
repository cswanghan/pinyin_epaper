/*****************************************************************************
 * 笔顺朗读 —— 用刷屏的 17.6 秒念笔画
 *
 * 四色墨水屏刷一次 17.6 秒（波形固化在面板 OTP 里，没有单色/快刷模式）。这段时间
 * 原本只有「字音 + 词组」约 4 秒有声音。把笔顺念出来正好填上：孩子跟着读音和笔顺
 * 在纸上写，写完抬头，字刚好出现在屏幕上。
 *
 * 音频是拼出来的：SD 卡上只有 29 个笔画名 + 「N 画」+ 尾句共 54 个片段（约 1.6MB），
 * 按 stroke/bishun.txt 里的笔顺排进播放队列。每字存整段要多占 600MB，而且停顿会被
 * 烤进音频里调不动。
 *****************************************************************************/
#ifndef APP_STROKE_H
#define APP_STROKE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 读 <sd_root>/stroke/bishun.txt 和各片段的时长。缺文件返回 false —— 不影响其余功能，
 * 只是不念笔顺（老卡插上就是这个情况）*/
bool app_stroke_init(const char *sd_root, int total);

/* 把一个字的笔顺排进播放队列（调用前字音、词组已经排进去了）。
 * gap_ms > 0: 用这个固定停顿；gap_ms <= 0: 按刷屏窗口自动算，让念完刚好赶上字出现。
 * 返回预计念完要多少毫秒，0 = 这个字没有笔顺数据 */
int app_stroke_speak(int id, int gap_ms);

/* 逐画回调 —— 彩屏上一边念一边把笔画写出来时用。
 * 在这一画的名字**真正开始响**的那一刻被调到（不是排进队列的那一刻）：
 * app_stroke 会先等音频队列放空，再排这一条，然后调它。
 * 回调该在 dur_ms 毫秒里把第 idx 画画完再返回；返回 false = 中断（来新字了），
 * 剩下的笔画不再念。idx 从 0 数；短字念两遍时 idx 会从头再来一轮。*/
typedef bool (*app_stroke_step_t)(int idx, int n, int dur_ms, void *ctx);

/* 带逐画回调的版本。step 传 NULL 就完全等同 app_stroke_speak。
 * 注意传了 step 就会阻塞到念完为止（要在这段时间里画动画），调用方自己负责
 * 在回调里检查有没有新字进来。*/
int app_stroke_speak_ex(int id, int gap_ms, app_stroke_step_t step, void *ctx);

/* 笔画数；-1 = 没有笔顺数据（调试控制台用）*/
int app_stroke_count(int id);

/* 把刚刷完这一屏实测花了多少毫秒告诉笔顺模块，下一个字就按这个真实窗口摊停顿。
 * 四色墨水屏的波形时长随温度变，同一块板实测到过 17.6 秒，也实测到过 20.5 秒；
 * 写死一个常数会在笔顺念完和字显形之间留出好几秒干等 */
void app_stroke_set_window(int ms);

#ifdef __cplusplus
}
#endif
#endif /* APP_STROKE_H */
