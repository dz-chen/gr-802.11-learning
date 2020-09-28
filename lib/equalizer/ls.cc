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

/*ls:信道估计算法*/
/*
    被调用:d_equalizer->equalize(current_symbol, d_current_symbol,symbols, out + o * 48, d_frame_mod);
    Y=HX+N ,H是要估计的信道特征
*/

void ls::equalize(gr_complex *in, int n, gr_complex *symbols, uint8_t *bits, boost::shared_ptr<gr::digital::constellation> mod) {

	if(n == 0) {    //新的一帧
		std::memcpy(d_H, in, 64 * sizeof(gr_complex));     //用in初始化d_H
	} else if(n == 1) {
		double signal = 0;
		double noise = 0;
		for(int i = 0; i < 64; i++) {
            // 前6个和后5个都是子载波的保护带宽
			if((i == 32) || (i < 6) || ( i > 58)) {   //剔除了1+6+5=12个数据,剩余52个=48个数据副载波+4个导频副载波 ?参考:https://blog.csdn.net/rs_network/article/details/49162455
				continue;
			}
			noise += std::pow(std::abs(d_H[i] - in[i]), 2);
			signal += std::pow(std::abs(d_H[i] + in[i]), 2);
			d_H[i] += in[i];   //2Y (n=1时用in初始化了d_H)
			d_H[i] /= LONG[i] * gr_complex(2, 0);   //2X ;  LONG[i]即Freq X (Sync_long)
		}

		d_snr = 10 * std::log10(signal / noise / 2);

	} else {

		int c = 0;
		for(int i = 0; i < 64; i++) {
			if( (i == 11) || (i == 25) || (i == 32) || (i == 39) || (i == 53) || (i < 6) || ( i > 58)) {
				continue;
			} else {
				symbols[c] = in[i] / d_H[i];
				bits[c] = mod->decision_maker(&symbols[c]);
				c++;
			}
		}
	}
}

double ls::get_snr() {
	return d_snr;
}
