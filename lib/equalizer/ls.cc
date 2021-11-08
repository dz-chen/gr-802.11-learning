/*
 * Copyright (C) 2016 Bastian Bloessl <bloessl@ccs-labs.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ls.h"
#include <cstring>
#include <iostream>

using namespace gr::ieee802_11::equalizer;

// ls:信道估计算法 => least square:最小二乘

/*
 * 被调用:d_equalizer->equalize(current_symbol, d_current_symbol,symbols, out + o * 48, d_frame_mod);
 * Y=HX+N ,H是要估计的信道特征
 * 输入参数=>
 *       in:当前符号(经过补偿后的)
 *       n :当前符号在帧中的编号(帧已经去掉了STF)
 *       symbols: 48个gr_complex; => 经过信道估计后反算出来的symbol中的数据部分
 *       bits:输出数据
 *       mod:
 * 
 * 处理流程:
 *       遇到symbol 0(LTF的第一个符号) => 初始化H      
 *       遇到symbol 1(LTF的第二个符号) => 进行信道估计
 *       遇到symbol 2及以上 => 直接计算信道均衡(用信道估计的参数反算原始symbol)后的数据
 */

void ls::equalize(gr_complex *in, int n, gr_complex *symbols, uint8_t *bits, boost::shared_ptr<gr::digital::constellation> mod) {

	if(n == 0) {    //新的一帧
		std::memcpy(d_H, in, 64 * sizeof(gr_complex));     //用in初始化d_H
	} 
    else if(n == 1) {
		double signal = 0;
		double noise = 0;
		for(int i = 0; i < 64; i++) {
            // 前6个和后5个都是子载波的保护带宽
            // 第32个是DC偏置
            //剔除了1+6+5=12个数据,剩余52个=48个数据副载波+4个导频副载波 参考:https://blog.csdn.net/rs_network/article/details/49162455
			if((i == 32) || (i < 6) || ( i > 58)) {   
				continue;
			}
			noise += std::pow(std::abs(d_H[i] - in[i]), 2);
			signal += std::pow(std::abs(d_H[i] + in[i]), 2);
			d_H[i] += in[i];   //2Y (n=1时用in初始化了d_H)
			d_H[i] /= LONG[i] * gr_complex(2, 0);   //2X ;  LONG[i]即Freq X (Sync_long); LONG来自base.cc
		}

		d_snr = 10 * std::log10(signal / noise / 2);

	}
    else {
		int c = 0;
		for(int i = 0; i < 64; i++) {
            // 剔除保护带宽、导频、DC
			if( (i == 11) || (i == 25) || (i == 32) || (i == 39) || (i == 53) || (i < 6) || ( i > 58)) {
				continue;
			}
            else {
                // symbols 其实就是信道估计后的数据子载波数据
				symbols[c] = in[i] / d_H[i];
                
                /*
                 * 返回对应数据在星座图中最匹配的点 
                 * => 与调制方法有关; 参考frame_equalizer_impl.cc中 d_frame_mod 的设置
                 */
				bits[c] = mod->decision_maker(&symbols[c]);
				c++;
			}
		}
	}
}

double ls::get_snr() {
	return d_snr;
}
