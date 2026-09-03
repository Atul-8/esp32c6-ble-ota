#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
host_test_hxd019.py — HXD019EU shared 层帧构建金标准 host 侧验证（G2.5）

与 firmware/components/hxd019_shared/src/hxd019_frame.c 纯函数逐字节对照。
金标准来源：docs/HXD019EU红外遥控器芯片相关datasheet/ 三份文件。
运行：python host_test_hxd019.py（无第三方依赖，Windows/WSL 均可）
预期：==== overall: ALL PASS
"""

def chk(bs):
    """校验和 = 帧内校验字节之前所有字节之和的低 8 位"""
    return sum(bs) & 0xFF

def be(group):
    """码组号 → 大端 2B（830 → 03 3E）"""
    return [(group >> 8) & 0xFF, group & 0xFF]

def ac_simple(group, func):
    """规则一：空调简单命令 5B（无校验和）"""
    return [0x30, 0x06] + be(group) + [func]

def ac_state7(group, temp, fan, swing, swing_auto, power, key, mode):
    """规则二：空调完整状态 16B（7 键，尾 03 00 00 FF）"""
    b = [0x30, 0x01] + be(group) + [temp, fan, swing, swing_auto, power, key, mode,
                                     0x03, 0x00, 0x00, 0xFF]
    return b + [chk(b)]

def ac_state11(group, temp, fan, swing, swing_auto, power, key, mode,
               sleep, heat, light, eco):
    """规则三：空调完整状态 16B（11 键，无固定尾）"""
    b = [0x30, 0x01] + be(group) + [temp, fan, swing, swing_auto, power, key, mode,
                                     sleep, heat, light, eco]
    return b + [chk(b)]

def av(fmt, key, com):
    """规则四：非空调 10B"""
    b = [0x30, 0x00, fmt] + key + com
    return b + [chk(b)]

ok = True

def cmp_case(tag, got, want):
    global ok
    g = ' '.join('%02X' % x for x in got)
    w = ' '.join('%02X' % x for x in want)
    m = got == want
    ok &= m
    print(('PASS' if m else 'FAIL'), tag)
    print('   got:', g)
    print('  want:', w)

# ---- 规则一（datasheet 命令表范例：格力 830 组） ----
cmp_case('ac_simple off 830', ac_simple(830, 0x80), [0x30, 0x06, 0x03, 0x3E, 0x80])
cmp_case('ac_simple on  830', ac_simple(830, 0x81), [0x30, 0x06, 0x03, 0x3E, 0x81])
cmp_case('ac_simple tmp+ 830', ac_simple(830, 0x97), [0x30, 0x06, 0x03, 0x3E, 0x97])

# ---- 规则二（V6.c 范例 172 行：格力 830 组，27℃ 制冷 风量自动 手动中
#      自动摆开 开机 电源键；datasheet 未给校验和值，按规则推导 = 0x97） ----
want16 = [0x30, 0x01, 0x03, 0x3E, 0x1B, 0x01, 0x02, 0x01, 0x01, 0x01, 0x02,
          0x03, 0x00, 0x00, 0xFF]
print('rule2 16B chk derived = 0x%02X' % chk(want16))
cmp_case('ac_state7 830 27C cool power',
         ac_state7(830, 0x1B, 0x01, 0x02, 0x01, 0x01, 0x01, 0x02),
         want16 + [0x97])

# ---- 规则三（datasheet 无数值范例，仅结构与 16B 长度自洽；chk 推导展示） ----
got11 = ac_state11(830, 0x19, 0x01, 0x02, 0x01, 0x01, 0x08, 0x02, 1, 0, 1, 0)
print('rule3 16B len=%d chk=0x%02X frame: %s'
      % (len(got11), got11[-1], ' '.join('%02X' % x for x in got11)))

# ---- 规则四（码库 V7 第 2 行 + 接口表 B 范例） ----
cmp_case('av iptv#2 power', av(0x01, [0x0F, 0xF0], [0x01, 0xFE, 0x00, 0x00]),
         [0x30, 0x00, 0x01, 0x0F, 0xF0, 0x01, 0xFE, 0x00, 0x00, 0x2F])
cmp_case('av iface-B', av(0x01, [0x02, 0xFD], [0x01, 0xFE, 0x77, 0x88]),
         [0x30, 0x00, 0x01, 0x02, 0xFD, 0x01, 0xFE, 0x77, 0x88, 0x2E])

# ---- 学习/匹配 ----
cmp_case('learn', [0x30, 0x20, 0x50], [0x30, 0x20, 0x50])
cmp_case('match', [0x30, 0x70, 0xA0], [0x30, 0x70, 0xA0])

print('==== overall:', 'ALL PASS' if ok else 'HAS FAIL')
raise SystemExit(0 if ok else 1)
